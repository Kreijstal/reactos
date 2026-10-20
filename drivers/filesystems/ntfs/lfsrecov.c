/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     $LogFile restart recovery at mount
 *
 * A dirty restart area means the volume went away while records were
 * still needed.  Recovery, as NTFS does it at mount:
 *
 *   analysis  the records from the client's OldestLsn (where the last
 *             checkpoint left the oldest record still needed) to the
 *             restart area's CurrentLsn, walked back through
 *             ClientPreviousLsn; every one is checked, the transactions
 *             that never committed (ForgetTransaction) are found;
 *   redo      in LSN order, each file record image is written where the
 *             record on disk is older than it;
 *   undo      the uncommitted transactions' records, newest first, put
 *             their before images back;
 *
 * then the volume is flushed and a clean restart area is written.
 *
 * Operations replayed (the ones lfslog.c writes):
 *   InitializeFileRecordSegment / InitializeFileRecordSegment
 *             redo and undo are whole file record images; the target is
 *             TargetVcn and MftClusterIndex of $MFT, at the LCNs listed;
 *   UpdateNonResidentValue / UpdateNonResidentValue
 *             redo and undo are the bytes of a non-resident metadata write
 *             (index buffers, bitmaps, $Secure, attribute lists) at
 *             AttributeOffset into the first of the LCNs listed; they are
 *             written in LSN order;
 *   ForgetTransaction / Noop      commits its transaction;
 *   Noop / Noop (no transaction)  metadata outside the log changed after
 *             the last checkpoint: chkdsk must look at the volume.
 *
 * Anything else, a record that fails a check, or a log with an NTFS restart
 * record (ClientRestartLsn, as Windows writes) is not guessed at: recovery
 * fails, the caller mounts the volume read-only and marks it dirty.
 *
 * Layout: ntfs-3g logfile.h.
 */
#include "lfswrite.h"
#define NDEBUG
#include <debug.h>

#define LFS_RCRD_MAGIC 0x44524352

typedef struct _LOG_RECORD_INFO
{
    ULONGLONG Lsn;
    ULONGLONG Target;           /* file record, for file record images */
    ULONG TransactionId;
    USHORT RedoOperation;
    BOOLEAN Committed;
} LOG_RECORD_INFO, *PLOG_RECORD_INFO;

typedef struct _LOG_READER
{
    PDEVICE_EXTENSION Vcb;
    PNTFS_ATTR_CONTEXT Log;
    NTFS_LOG_RESTART_INFO Restart;
    ULONG PageSize;
    ULONG LogSize;
    ULONG FirstRcrdOffset;
    ULONG SeqNumberBits;
    PUCHAR Page;
    PUCHAR Record;              /* the record ReadRecord returned */
    ULONG RecordCapacity;
    PUCHAR Image;               /* one file record */
    PLOG_RECORD_INFO Records;   /* in LSN order once analysed */
    ULONG Count;
    ULONG Capacity;
} LOG_READER, *PLOG_READER;

static NTSTATUS
LogReadPage(PLOG_READER Reader, ULONG Offset, ULONGLONG MinimumLsn)
{
    PNTFS_LFS_PAGE_HEADER Hdr = (PNTFS_LFS_PAGE_HEADER)Reader->Page;
    PDEVICE_EXTENSION Vcb = Reader->Vcb;

    if (ReadAttribute(Vcb, Reader->Log, Offset, (PCHAR)Reader->Page, Reader->PageSize) != Reader->PageSize)
        return STATUS_UNEXPECTED_IO_ERROR;
    if (Hdr->Header.Type != LFS_RCRD_MAGIC ||
        Hdr->Header.UsaCount != Reader->PageSize / Vcb->NtfsInfo.BytesPerSector + 1 ||
        Hdr->Header.UsaOffset < sizeof(NTFS_LFS_PAGE_HEADER) ||
        Hdr->Header.UsaOffset + Hdr->Header.UsaCount * sizeof(USHORT) > NTFS_LFS_PAGE_DATA_OFFSET)
        return STATUS_FILE_CORRUPT_ERROR;
    if (!NT_SUCCESS(FixupUpdateSequenceArray(Vcb, &Hdr->Header)))
        return STATUS_FILE_CORRUPT_ERROR;
    /* A page of an earlier lap holds no byte of the record wanted. */
    if (Hdr->Header.Lsn < MinimumLsn)
        return STATUS_FILE_CORRUPT_ERROR;
    return STATUS_SUCCESS;
}

/* Reads the whole record Lsn into Reader->Record. */
static NTSTATUS
LogReadRecord(PLOG_READER Reader, ULONGLONG Lsn, PNTFS_LFS_RECORD_HEADER *RecordOut)
{
    PNTFS_LFS_RECORD_HEADER Rec = (PNTFS_LFS_RECORD_HEADER)Reader->Record;
    ULONG Offset, Within, Length, Copied, Part, Page;
    NTSTATUS Status;

    Offset = NtfsLfsLsnToOffset(Reader->SeqNumberBits, Lsn);
    Within = Offset % Reader->PageSize;
    if (Lsn == 0 || (Offset & 7) || Offset < Reader->FirstRcrdOffset || Offset >= Reader->LogSize ||
        Within < NTFS_LFS_PAGE_DATA_OFFSET || Reader->PageSize - Within < NTFS_LFS_RECORD_HEADER_LENGTH)
        return STATUS_FILE_CORRUPT_ERROR;
    Page = Offset - Within;
    Status = LogReadPage(Reader, Page, Lsn);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlCopyMemory(Rec, Reader->Page + Within, NTFS_LFS_RECORD_HEADER_LENGTH);
    if (Rec->ThisLsn != Lsn || Rec->ClientDataLength > Reader->RecordCapacity - NTFS_LFS_RECORD_HEADER_LENGTH)
        return STATUS_FILE_CORRUPT_ERROR;
    Length = NTFS_LFS_RECORD_HEADER_LENGTH + Rec->ClientDataLength;
    Copied = 0;
    for (;;)
    {
        Part = min(Length - Copied, Reader->PageSize - Within);
        RtlCopyMemory(Reader->Record + Copied, Reader->Page + Within, Part);
        Copied += Part;
        if (Copied == Length)
            break;
        /* The record continues after the header of the next page. */
        if (!(Rec->Flags & NTFS_LFS_RECORD_FLAG_MULTI_PAGE))
            return STATUS_FILE_CORRUPT_ERROR;
        Page += Reader->PageSize;
        if (Page >= Reader->LogSize)
            Page = Reader->FirstRcrdOffset;
        Status = LogReadPage(Reader, Page, Lsn);
        if (!NT_SUCCESS(Status))
            return Status;
        Within = NTFS_LFS_PAGE_DATA_OFFSET;
    }
    *RecordOut = Rec;
    return STATUS_SUCCESS;
}

/* A file record image as the driver writes them: FILE, an update sequence
 * array for this record size, attributes after it. */
static BOOLEAN
LogImageValid(PLOG_READER Reader, const UCHAR *Image, ULONG Length, BOOLEAN AllowZero)
{
    PFILE_RECORD_HEADER Rec = (PFILE_RECORD_HEADER)Image;
    ULONG Size = Reader->Vcb->NtfsInfo.BytesPerFileRecord, i;

    if (Length != Size)
        return FALSE;
    if (AllowZero)
    {
        for (i = 0; i < Length && Image[i] == 0; i++);
        if (i == Length)
            return TRUE;
    }
    return Rec->Ntfs.Type == NRH_FILE_TYPE &&
           Rec->Ntfs.UsaCount == Size / Reader->Vcb->NtfsInfo.BytesPerSector + 1 &&
           Rec->Ntfs.UsaOffset >= sizeof(NTFS_RECORD_HEADER) &&
           Rec->Ntfs.UsaOffset + Rec->Ntfs.UsaCount * sizeof(USHORT) <= Rec->AttributeOffset &&
           Rec->AttributeOffset < Size && Rec->BytesInUse <= Size;
}

/* Checks a file record image record: geometry, target, LCNs, images. */
static NTSTATUS
LogCheckFileRecordOp(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER Rec, PULONGLONG TargetOut)
{
    PDEVICE_EXTENSION Vcb = Reader->Vcb;
    ULONG Bpc = Vcb->NtfsInfo.BytesPerCluster, Size = Vcb->NtfsInfo.BytesPerFileRecord;
    PUCHAR Client = (PUCHAR)Rec + NTFS_LFS_RECORD_HEADER_LENGTH;
    PLONGLONG Lcns = (PLONGLONG)((PUCHAR)Rec + sizeof(NTFS_LFS_RECORD_HEADER));
    ULONGLONG Byte, Clusters;
    LONGLONG Lcn, Count;
    ULONG Within, i;
    BOOLEAN Mapped;

    if (Rec->TransactionId == 0 || Rec->Alignment2 != NTFS_LFS_ACTS_ON_MFT ||
        Rec->RedoLength != Size || Rec->UndoLength != Size ||
        Rec->RedoOffset < NTFS_LFS_ATTR_HEADER_LENGTH + Rec->LcnsToFollow * sizeof(LONGLONG) ||
        (ULONG)Rec->RedoOffset + Rec->RedoLength > Rec->ClientDataLength ||
        Rec->UndoOffset < Rec->RedoOffset + Rec->RedoLength ||
        (ULONG)Rec->UndoOffset + Rec->UndoLength > Rec->ClientDataLength)
        return STATUS_FILE_CORRUPT_ERROR;
    Within = Rec->MftClusterIndex * 512;
    if (Within >= Bpc || Rec->TargetVcn >= Vcb->NtfsInfo.ClusterCount)
        return STATUS_FILE_CORRUPT_ERROR;
    Byte = Rec->TargetVcn * Bpc + Within;
    Clusters = (Within + Size + Bpc - 1) / Bpc;
    if (Byte % Size != 0 || Rec->LcnsToFollow != Clusters)
        return STATUS_FILE_CORRUPT_ERROR;
    for (i = 0; i < Rec->LcnsToFollow; i++)
    {
        if (Lcns[i] < 0 || (ULONGLONG)Lcns[i] >= Vcb->NtfsInfo.ClusterCount)
            return STATUS_FILE_CORRUPT_ERROR;
        /* Where $MFT maps the cluster already, it is where the record is. */
        ExAcquireResourceSharedLite(&Vcb->MftContextResource, TRUE);
        Mapped = FsRtlLookupLargeMcbEntry(&Vcb->MFTContext->DataRunsMCB, Rec->TargetVcn + i,
                                          &Lcn, &Count, NULL, NULL, NULL) && Lcn >= 0;
        ExReleaseResourceLite(&Vcb->MftContextResource);
        if (Mapped && Lcn != Lcns[i])
            return STATUS_FILE_CORRUPT_ERROR;
    }
    if (!LogImageValid(Reader, Client + Rec->RedoOffset, Rec->RedoLength, FALSE) ||
        !LogImageValid(Reader, Client + Rec->UndoOffset, Rec->UndoLength, TRUE))
        return STATUS_FILE_CORRUPT_ERROR;
    *TargetOut = Byte / Size;
    return STATUS_SUCCESS;
}

/* Checks a non-resident metadata write record. */
static NTSTATUS
LogCheckRangeOp(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER Rec)
{
    PDEVICE_EXTENSION Vcb = Reader->Vcb;
    ULONG Bpc = Vcb->NtfsInfo.BytesPerCluster, i;
    PLONGLONG Lcns = (PLONGLONG)((PUCHAR)Rec + sizeof(NTFS_LFS_RECORD_HEADER));

    if (Rec->TransactionId == 0 || Rec->Alignment2 != 0 || Rec->MftClusterIndex != 0 ||
        Rec->RecordOffset != 0 || Rec->AttributeOffset >= Bpc ||
        Rec->RedoLength == 0 || Rec->UndoLength != Rec->RedoLength ||
        Rec->LcnsToFollow != (Rec->AttributeOffset + Rec->RedoLength + Bpc - 1) / Bpc ||
        Rec->RedoOffset < NTFS_LFS_ATTR_HEADER_LENGTH + Rec->LcnsToFollow * sizeof(LONGLONG) ||
        (ULONG)Rec->RedoOffset + Rec->RedoLength > Rec->ClientDataLength ||
        Rec->UndoOffset < Rec->RedoOffset + Rec->RedoLength ||
        (ULONG)Rec->UndoOffset + Rec->UndoLength > Rec->ClientDataLength)
        return STATUS_FILE_CORRUPT_ERROR;
    for (i = 0; i < Rec->LcnsToFollow; i++)
    {
        if (Lcns[i] < 0 || (ULONGLONG)Lcns[i] >= Vcb->NtfsInfo.ClusterCount)
            return STATUS_FILE_CORRUPT_ERROR;
    }
    return STATUS_SUCCESS;
}

/* Every record recovery meets is one of the operations above. */
static NTSTATUS
LogClassify(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER Rec, PLOG_RECORD_INFO Info)
{
    RtlZeroMemory(Info, sizeof(*Info));
    Info->Lsn = Rec->ThisLsn;
    Info->TransactionId = Rec->TransactionId;
    Info->RedoOperation = Rec->RedoOperation;
    if (Rec->Alignment != NTFS_LFS_CLIENT_INDEX_NTFS ||
        Rec->ClientId != Reader->Restart.ClientSeqNumber ||
        Rec->RecordType != NTFS_LFS_RECTYPE_NORMAL ||
        Rec->ClientDataLength < NTFS_LFS_ATTR_HEADER_LENGTH)
        return STATUS_FILE_CORRUPT_ERROR;
    if (Rec->RedoOperation == NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT &&
        Rec->UndoOperation == NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT)
        return LogCheckFileRecordOp(Reader, Rec, &Info->Target);
    if (Rec->RedoOperation == NTFS_LFS_OP_UPDATE_NONRESIDENT_VALUE &&
        Rec->UndoOperation == NTFS_LFS_OP_UPDATE_NONRESIDENT_VALUE)
        return LogCheckRangeOp(Reader, Rec);
    /* In a log this driver wrote, any other record is damage. */
    if (Rec->RedoLength != 0 || Rec->UndoLength != 0 || Rec->LcnsToFollow != 0)
        return STATUS_FILE_CORRUPT_ERROR;
    if (Rec->RedoOperation == NTFS_LFS_OP_FORGET_TRANSACTION &&
        Rec->UndoOperation == NTFS_LFS_OP_NOOP && Rec->TransactionId != 0)
        return STATUS_SUCCESS;
    if (Rec->RedoOperation == NTFS_LFS_OP_NOOP &&
        Rec->UndoOperation == NTFS_LFS_OP_NOOP && Rec->TransactionId == 0)
        return STATUS_SUCCESS;
    DPRINT1("LFS recovery: unknown operation %x/%x at %I64x\n",
            Rec->RedoOperation, Rec->UndoOperation, Rec->ThisLsn);
    return STATUS_FILE_CORRUPT_ERROR;
}

static NTSTATUS
LogAddRecord(PLOG_READER Reader, PLOG_RECORD_INFO Info)
{
    PLOG_RECORD_INFO Grown;

    if (Reader->Count == Reader->Capacity)
    {
        ULONG Capacity = max(64, Reader->Capacity * 2);
        Grown = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(*Grown), TAG_NTFS);
        if (Grown == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        if (Reader->Records != NULL)
        {
            RtlCopyMemory(Grown, Reader->Records, Reader->Count * sizeof(*Grown));
            ExFreePoolWithTag(Reader->Records, TAG_NTFS);
        }
        Reader->Records = Grown;
        Reader->Capacity = Capacity;
    }
    Reader->Records[Reader->Count++] = *Info;
    return STATUS_SUCCESS;
}

/* The record after Rec: the next one of its batch right behind it, or the
 * first one of the next batch, at the next page.  0 when neither is one. */
static ULONGLONG
LogNextRecord(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER Rec)
{
    ULONG OffsetBits = 64 - Reader->SeqNumberBits;
    ULONGLONG Seq = Rec->ThisLsn >> OffsetBits, Candidate, This = Rec->ThisLsn;
    ULONG Offset = NtfsLfsLsnToOffset(Reader->SeqNumberBits, This);
    ULONG Length = ALIGN_UP_BY(NTFS_LFS_RECORD_HEADER_LENGTH + Rec->ClientDataLength, 8);
    ULONG Page = Offset - Offset % Reader->PageSize, Room, End;
    PNTFS_LFS_RECORD_HEADER Next;

    /* Where the record ends: across pages after their headers. */
    Room = Reader->PageSize - (Offset - Page);
    while (Length > Room)
    {
        Length -= Room;
        Page += Reader->PageSize;
        if (Page >= Reader->LogSize)
        {
            Page = Reader->FirstRcrdOffset;
            Seq++;
        }
        Offset = Page + NTFS_LFS_PAGE_DATA_OFFSET;
        Room = Reader->PageSize - NTFS_LFS_PAGE_DATA_OFFSET;
    }
    End = Offset + Length;
    if (End - Page + NTFS_LFS_RECORD_HEADER_LENGTH <= Reader->PageSize)
    {
        Candidate = NtfsLfsMakeLsn(Reader->SeqNumberBits, Seq, End);
        if (NT_SUCCESS(LogReadRecord(Reader, Candidate, &Next)) && Next->ClientPreviousLsn == This)
            return Candidate;
    }
    Page += Reader->PageSize;
    if (Page >= Reader->LogSize)
    {
        Page = Reader->FirstRcrdOffset;
        Seq++;
    }
    return NtfsLfsMakeLsn(Reader->SeqNumberBits, Seq, Page + NTFS_LFS_PAGE_DATA_OFFSET);
}

static BOOLEAN
LogIsImageOp(PLOG_RECORD_INFO Op)
{
    return Op->RedoOperation == NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT ||
           Op->RedoOperation == NTFS_LFS_OP_UPDATE_NONRESIDENT_VALUE;
}

/* Analysis: the records OldestLsn..CurrentLsn, in LSN order, each checked,
 * the committed transactions marked.  Undoing a transaction must not undo
 * a later change of another one: that would be a guess, and fails. */
static NTSTATUS
LogAnalyse(PLOG_READER Reader, PNTFS_LOG_RECOVERY_RESULT Result)
{
    ULONGLONG Start = Reader->Restart.OldestLsn, Current = Reader->Restart.Area.CurrentLsn;
    ULONGLONG Lsn, Next, Prev = 0;
    PNTFS_LFS_RECORD_HEADER Rec;
    LOG_RECORD_INFO Info;
    ULONG i, j, Limit;
    NTSTATUS Status;

    /* The restart area names where the scan starts: the oldest record still
     * needed, or (past CurrentLsn) where the next record was to go. */
    if (Start == 0)
        return STATUS_FILE_CORRUPT_ERROR;
    Limit = Reader->LogSize / NTFS_LFS_RECORD_HEADER_LENGTH;
    for (Lsn = Start; Lsn != 0; Lsn = Next)
    {
        Status = LogReadRecord(Reader, Lsn, &Rec);
        if (NT_SUCCESS(Status) && Prev != 0 && Rec->ClientPreviousLsn != Prev)
            Status = STATUS_FILE_CORRUPT_ERROR;
        if (!NT_SUCCESS(Status))
        {
            /* A needed record must be there; past it, the log ends where the
             * next record is not. */
            if (Prev == 0 && Start <= Current)
            {
                DPRINT1("LFS recovery: start record %I64x: %lx\n", Lsn, Status);
                return Status;
            }
            break;
        }
        Status = LogClassify(Reader, Rec, &Info);
        if (NT_SUCCESS(Status))
            Status = LogAddRecord(Reader, &Info);
        if (!NT_SUCCESS(Status) || Reader->Count > Limit)
        {
            DPRINT1("LFS recovery: record %I64x: %lx\n", Lsn, Status);
            return NT_SUCCESS(Status) ? STATUS_FILE_CORRUPT_ERROR : Status;
        }
        Prev = Lsn;
        Next = LogNextRecord(Reader, Rec);
    }
    /* Records the restart area names are in the log. */
    if (Current >= Start && (Reader->Count == 0 || Reader->Records[Reader->Count - 1].Lsn < Current))
    {
        DPRINT1("LFS recovery: the log ends at %I64x before CurrentLsn %I64x\n",
                Reader->Count ? Reader->Records[Reader->Count - 1].Lsn : 0, Current);
        return STATUS_FILE_CORRUPT_ERROR;
    }
    UNREFERENCED_PARAMETER(j);

    Result->Records = Reader->Count;
    for (i = 0; i < Reader->Count; i++)
    {
        PLOG_RECORD_INFO Forget = &Reader->Records[i];

        if (Forget->RedoOperation == NTFS_LFS_OP_NOOP)
        {
            Result->NeedsCheck = TRUE;
            continue;
        }
        if (Forget->RedoOperation != NTFS_LFS_OP_FORGET_TRANSACTION)
            continue;
        /* It commits the records of its transaction logged before it (a
         * transaction committed early, for a full log, may have none left
         * in the log). */
        for (j = i; j-- != 0;)
        {
            PLOG_RECORD_INFO Op = &Reader->Records[j];
            if (LogIsImageOp(Op) && Op->TransactionId == Forget->TransactionId)
                Op->Committed = TRUE;
        }
    }
    return STATUS_SUCCESS;
}

/* Where a record's image goes: Size bytes from Within into the first of
 * the clusters it lists, on through the next ones. */
static VOID
LogRecordTarget(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER Rec, PULONG Within, PULONG Size)
{
    if (Rec->RedoOperation == NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT)
    {
        *Within = Rec->MftClusterIndex * 512;
        *Size = Reader->Vcb->NtfsInfo.BytesPerFileRecord;
    }
    else
    {
        ASSERT(Rec->RedoOperation == NTFS_LFS_OP_UPDATE_NONRESIDENT_VALUE);
        *Within = Rec->AttributeOffset;
        *Size = Rec->RedoLength;
    }
}

static NTSTATUS
LogRecordIo(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER Rec, PUCHAR Buffer, BOOLEAN Write)
{
    PDEVICE_EXTENSION Vcb = Reader->Vcb;
    PLONGLONG Lcns = (PLONGLONG)((PUCHAR)Rec + sizeof(NTFS_LFS_RECORD_HEADER));
    ULONG Bpc = Vcb->NtfsInfo.BytesPerCluster, Within, Size, Done = 0, Part, i;
    NTSTATUS Status = STATUS_SUCCESS;

    LogRecordTarget(Reader, Rec, &Within, &Size);
    for (i = 0; NT_SUCCESS(Status) && Done < Size; i++)
    {
        ASSERT(i < Rec->LcnsToFollow);
        Part = min(Bpc - Within, Size - Done);
        if (Write)
            Status = NtfsWriteDiskCached(Vcb, Lcns[i] * Bpc + Within, Part, Buffer + Done);
        else
            Status = NtfsReadDiskCached(Vcb, Lcns[i] * Bpc + Within, Part, Buffer + Done);
        Done += Part;
        Within = 0;
    }
    return Status;
}

/* Whether two records' images share a byte of the volume. */
static BOOLEAN
LogRecordsOverlap(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER A, const LONGLONG *LcnsA,
                  ULONG WithinA, ULONG SizeA, PNTFS_LFS_RECORD_HEADER B)
{
    PLONGLONG LcnsB = (PLONGLONG)((PUCHAR)B + sizeof(NTFS_LFS_RECORD_HEADER));
    ULONG Bpc = Reader->Vcb->NtfsInfo.BytesPerCluster, WithinB, SizeB, i, j, DoneA, DoneB, PartA, PartB, WA, WB;
    ULONGLONG StartA, StartB;

    UNREFERENCED_PARAMETER(A);
    LogRecordTarget(Reader, B, &WithinB, &SizeB);
    for (i = 0, DoneA = 0, WA = WithinA; DoneA < SizeA; i++, WA = 0)
    {
        PartA = min(Bpc - WA, SizeA - DoneA);
        StartA = LcnsA[i] * Bpc + WA;
        for (j = 0, DoneB = 0, WB = WithinB; DoneB < SizeB; j++, WB = 0)
        {
            PartB = min(Bpc - WB, SizeB - DoneB);
            StartB = LcnsB[j] * Bpc + WB;
            if (StartA < StartB + PartB && StartB < StartA + PartA)
                return TRUE;
            DoneB += PartB;
        }
        DoneA += PartA;
    }
    return FALSE;
}

/* Undoing a transaction must not undo a later change of another one to
 * the same bytes: that would be a guess, and fails. */
static NTSTATUS
LogCheckUndo(PLOG_READER Reader)
{
    struct
    {
        NTFS_LFS_RECORD_HEADER Header;
        LONGLONG Lcns[64];
    } Copy;
    PNTFS_LFS_RECORD_HEADER Rec;
    PLOG_RECORD_INFO Op, Later;
    ULONG i, j, Within, Size;
    NTSTATUS Status;

    for (i = 0; i < Reader->Count; i++)
    {
        Op = &Reader->Records[i];
        if (!LogIsImageOp(Op) || Op->Committed)
            continue;
        Status = LogReadRecord(Reader, Op->Lsn, &Rec);
        if (!NT_SUCCESS(Status))
            return Status;
        if (Rec->LcnsToFollow > RTL_NUMBER_OF(Copy.Lcns))
            return STATUS_FILE_CORRUPT_ERROR;
        RtlCopyMemory(&Copy, Rec, sizeof(Copy.Header) + Rec->LcnsToFollow * sizeof(LONGLONG));
        LogRecordTarget(Reader, &Copy.Header, &Within, &Size);
        for (j = i + 1; j < Reader->Count; j++)
        {
            Later = &Reader->Records[j];
            if (!LogIsImageOp(Later) || Later->TransactionId == Op->TransactionId)
                continue;
            Status = LogReadRecord(Reader, Later->Lsn, &Rec);
            if (!NT_SUCCESS(Status))
                return Status;
            if (LogRecordsOverlap(Reader, &Copy.Header, Copy.Lcns, Within, Size, Rec))
            {
                DPRINT1("LFS recovery: uncommitted %I64x is followed by %I64x of the same bytes\n",
                        Op->Lsn, Later->Lsn);
                return STATUS_FILE_CORRUPT_ERROR;
            }
        }
    }
    return STATUS_SUCCESS;
}

/* The record's LSN on disk, or 0 when it is not a readable file record. */
static ULONGLONG
LogDiskLsn(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER Rec)
{
    PFILE_RECORD_HEADER File = (PFILE_RECORD_HEADER)Reader->Image;
    PDEVICE_EXTENSION Vcb = Reader->Vcb;

    if (!NT_SUCCESS(LogRecordIo(Reader, Rec, Reader->Image, FALSE)) ||
        File->Ntfs.Type != NRH_FILE_TYPE ||
        File->Ntfs.UsaCount != Vcb->NtfsInfo.BytesPerFileRecord / Vcb->NtfsInfo.BytesPerSector + 1 ||
        File->Ntfs.UsaOffset < sizeof(NTFS_RECORD_HEADER) ||
        File->Ntfs.UsaOffset + File->Ntfs.UsaCount * sizeof(USHORT) > Vcb->NtfsInfo.BytesPerFileRecord ||
        !NT_SUCCESS(FixupUpdateSequenceArray(Vcb, &File->Ntfs)))
        return 0;
    return File->Ntfs.Lsn;
}

static PLOG_RECORD_INFO
LogFindLsn(PLOG_READER Reader, ULONGLONG Lsn)
{
    ULONG Low = 0, High = Reader->Count, Mid;

    while (Low < High)
    {
        Mid = Low + (High - Low) / 2;
        if (Reader->Records[Mid].Lsn == Lsn)
            return &Reader->Records[Mid];
        if (Reader->Records[Mid].Lsn < Lsn)
            Low = Mid + 1;
        else
            High = Mid;
    }
    return NULL;
}

/* Writes a record's redo or undo image where it goes; a file record also
 * to $MFTMirr's copy of it. */
static NTSTATUS
LogWriteImage(PLOG_READER Reader, PNTFS_LFS_RECORD_HEADER Rec, ULONGLONG Target, BOOLEAN Redo)
{
    PFILE_RECORD_HEADER File = (PFILE_RECORD_HEADER)Reader->Image;
    PUCHAR Image = (PUCHAR)Rec + NTFS_LFS_RECORD_HEADER_LENGTH + (Redo ? Rec->RedoOffset : Rec->UndoOffset);
    ULONG Size = Reader->Vcb->NtfsInfo.BytesPerFileRecord, i;
    NTSTATUS Status;

    if (Rec->RedoOperation == NTFS_LFS_OP_UPDATE_NONRESIDENT_VALUE)
        return LogRecordIo(Reader, Rec, Image, TRUE);

    RtlCopyMemory(Reader->Image, Image, Size);
    for (i = 0; i < Size && Reader->Image[i] == 0; i++);
    if (i != Size)
    {
        /* A redone record carries the LSN of the record that redid it. */
        if (Redo)
            File->Ntfs.Lsn = Rec->ThisLsn;
        AddFixupArray(Reader->Vcb, &File->Ntfs);
    }
    Status = LogRecordIo(Reader, Rec, Reader->Image, TRUE);
    if (NT_SUCCESS(Status))
        Status = NtfsWriteFileRecordMirror(Reader->Vcb, Target, File);
    return Status;
}

static NTSTATUS
LogRedo(PLOG_READER Reader, PNTFS_LOG_RECOVERY_RESULT Result)
{
    PNTFS_LFS_RECORD_HEADER Rec;
    PLOG_RECORD_INFO Op, Newer;
    ULONGLONG DiskLsn;
    ULONG i;
    NTSTATUS Status;

    for (i = 0; i < Reader->Count; i++)
    {
        Op = &Reader->Records[i];
        if (!LogIsImageOp(Op))
            continue;
        Status = LogReadRecord(Reader, Op->Lsn, &Rec);
        if (!NT_SUCCESS(Status))
            return Status;
        /* Skip a file record already as new: one this log stamped with this
         * record's LSN or a later one of the same file record.  Other
         * metadata carries no LSN: its images are written in LSN order. */
        if (Op->RedoOperation == NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT)
        {
            DiskLsn = LogDiskLsn(Reader, Rec);
            Newer = DiskLsn >= Op->Lsn ? LogFindLsn(Reader, DiskLsn) : NULL;
            if (Newer != NULL && Newer->RedoOperation == NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT &&
                Newer->Target == Op->Target)
                continue;
            if (Op->Target < NTFS_FILE_FIRST_USER_FILE)
                Result->SystemRecords = TRUE;
        }
        Status = LogWriteImage(Reader, Rec, Op->Target, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
        Result->Redone++;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
LogUndo(PLOG_READER Reader, PNTFS_LOG_RECOVERY_RESULT Result)
{
    PNTFS_LFS_RECORD_HEADER Rec;
    PLOG_RECORD_INFO Op;
    ULONG i;
    NTSTATUS Status;

    for (i = Reader->Count; i-- != 0;)
    {
        Op = &Reader->Records[i];
        if (!LogIsImageOp(Op) || Op->Committed)
            continue;
        Status = LogReadRecord(Reader, Op->Lsn, &Rec);
        if (!NT_SUCCESS(Status))
            return Status;
        Status = LogWriteImage(Reader, Rec, Op->Target, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;
        Result->Undone++;
        if (Op->RedoOperation == NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT &&
            Op->Target < NTFS_FILE_FIRST_USER_FILE)
            Result->SystemRecords = TRUE;
    }
    return STATUS_SUCCESS;
}

/* The last LSN the log holds: the last record found, or CurrentLsn. */
static ULONGLONG
LogEndLsn(PLOG_READER Reader)
{
    ULONGLONG End = Reader->Restart.Area.CurrentLsn;

    if (Reader->Count != 0 && Reader->Records[Reader->Count - 1].Lsn > End)
        End = Reader->Records[Reader->Count - 1].Lsn;
    return End;
}

/* Both restart copies clean, one after the other, naming the last record:
 * LSNs keep increasing past it. */
static NTSTATUS
LogWriteCleanRestart(PLOG_READER Reader)
{
    ULONG Copy, Written;
    NTSTATUS Status = STATUS_SUCCESS;

    for (Copy = 0; NT_SUCCESS(Status) && Copy < 2; Copy++)
    {
        Status = NtfsLfsBuildRestartPage(Reader->Vcb, Reader->Page, Reader->PageSize,
                                         Reader->LogSize, LogEndLsn(Reader), 0, TRUE);
        if (NT_SUCCESS(Status))
            Status = WriteAttribute(Reader->Vcb, Reader->Log, (ULONGLONG)Copy * Reader->PageSize,
                                    Reader->Page, Reader->PageSize, &Written, NULL);
        if (NT_SUCCESS(Status))
            Status = NtfsFlushVolumeStream(Reader->Vcb);
    }
    return Status;
}

NTSTATUS
NtfsLfsRecover(PDEVICE_EXTENSION Vcb, PNTFS_LOG_RECOVERY_RESULT Result)
{
    PFILE_RECORD_HEADER Record = NULL;
    LOG_READER Reader;
    PNTFS_LOG_RESTART_AREA Area;
    NTSTATUS Status;

    RtlZeroMemory(Result, sizeof(*Result));
    RtlZeroMemory(&Reader, sizeof(Reader));
    Reader.Vcb = Vcb;
    ASSERT(!(Vcb->Flags & VCB_VOLUME_READ_ONLY));
    Status = NtfsLfsReadRestart(Vcb, &Reader.Restart);
    if (Status != STATUS_LOG_FILE_FULL)
        return NT_SUCCESS(Status) ? STATUS_SUCCESS : Status;
    if (Vcb->LogFileVersionUnsupported)
        return STATUS_NOT_SUPPORTED;

    /* Only the restart areas this driver writes: one NTFS client, no NTFS
     * restart record, the codec's page geometry. */
    Area = &Reader.Restart.Area;
    if (Reader.Restart.ClientRestartLsn != 0)
    {
        DPRINT1("LFS recovery: the log holds an NTFS restart record (%I64x); not replayed\n",
                Reader.Restart.ClientRestartLsn);
        return STATUS_NOT_SUPPORTED;
    }
    if (!Reader.Restart.ClientInUse || Area->LogClients != 1 ||
        Reader.Restart.ClientSeqNumber != NTFS_LFS_CLIENT_SEQ_NTFS ||
        Reader.Restart.LogPageSize != PAGE_SIZE || Reader.Restart.SystemPageSize != PAGE_SIZE ||
        Reader.Restart.LogSize > MAXULONG || Area->FileSize != Reader.Restart.LogSize ||
        Reader.Restart.LogSize < (NTFS_LFS_FIRST_RECORD_PAGE + 2) * PAGE_SIZE ||
        (Reader.Restart.LogSize % PAGE_SIZE) != 0 ||
        Area->SeqNumberBits != NtfsLfsSeqNumberBits(Reader.Restart.LogSize) ||
        Area->RecordHeaderLength != NTFS_LFS_RECORD_HEADER_LENGTH ||
        Area->LogPageDataOffset != NTFS_LFS_PAGE_DATA_OFFSET ||
        Vcb->NtfsInfo.BytesPerFileRecord > MAXUSHORT)
        return STATUS_NOT_SUPPORTED;
    Reader.PageSize = PAGE_SIZE;
    Reader.LogSize = (ULONG)Reader.Restart.LogSize;
    Reader.FirstRcrdOffset = NTFS_LFS_FIRST_RECORD_PAGE * PAGE_SIZE;
    Reader.SeqNumberBits = Area->SeqNumberBits;
    /* The largest record the writer emits. */
    Reader.RecordCapacity = max(NtfsLfsRecordLength(Vcb->NtfsInfo.BytesPerFileRecord,
                                                    Vcb->NtfsInfo.BytesPerFileRecord, 16),
                                NtfsLfsRecordLength(NTFS_LOG_METADATA_CHUNK, NTFS_LOG_METADATA_CHUNK,
                                                    NTFS_LOG_METADATA_LCNS));

    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    Reader.Page = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_NTFS);
    Reader.Record = ExAllocatePoolWithTag(PagedPool, Reader.RecordCapacity, TAG_NTFS);
    Reader.Image = ExAllocatePoolWithTag(PagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (Record == NULL || Reader.Page == NULL || Reader.Record == NULL || Reader.Image == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"", 0, &Reader.Log, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    if (!Reader.Log->pRecord->IsNonResident ||
        (Reader.Log->pRecord->Flags & (NTFS_ATTR_SPARSE | NTFS_ATTR_COMPRESSION_MASK)))
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }

    /* Nothing is written until every record checked out. */
    Status = LogAnalyse(&Reader, Result);
    if (NT_SUCCESS(Status))
        Status = LogCheckUndo(&Reader);
    if (NT_SUCCESS(Status))
        Status = LogRedo(&Reader, Result);
    if (NT_SUCCESS(Status))
        Status = LogUndo(&Reader, Result);
    if (NT_SUCCESS(Status))
        Status = NtfsFlushVolumeStream(Vcb);
    if (NT_SUCCESS(Status))
        Status = LogWriteCleanRestart(&Reader);
    if (NT_SUCCESS(Status))
    {
        Vcb->LogFileDirty = FALSE;
        Vcb->LogFileLsn = LogEndLsn(&Reader);
        DPRINT1("LFS recovery: %lu records, %lu images redone, %lu undone%s\n",
                Result->Records, Result->Redone, Result->Undone,
                Result->NeedsCheck ? ", unlogged metadata may be stale" : "");
    }

Cleanup:
    if (Reader.Log != NULL) ReleaseAttributeContext(Reader.Log);
    if (Reader.Records != NULL) ExFreePoolWithTag(Reader.Records, TAG_NTFS);
    if (Reader.Image != NULL) ExFreePoolWithTag(Reader.Image, TAG_NTFS);
    if (Reader.Record != NULL) ExFreePoolWithTag(Reader.Record, TAG_NTFS);
    if (Reader.Page != NULL) ExFreePoolWithTag(Reader.Page, TAG_NTFS);
    if (Record != NULL) ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    return Status;
}
