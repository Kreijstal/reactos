/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     LFS restart and record page emission
 *
 * The record pages form a circular buffer: past the last page, writing
 * resumes at the first one under the next sequence number, so LSNs keep
 * increasing.  A page is reused only when every record on it is older than
 * the oldest record still needed (the client's OldestLsn).
 *
 * Records are written in batches, and every batch starts on a page of its
 * own: a page that holds a record whose write was acknowledged is never
 * written again in the same lap of the log, so a torn page write can only
 * lose the batch being written.  A record longer than what is left of a
 * page continues after the header of the next page (MULTI_PAGE).
 *
 * Layout: ntfs-3g logfile.h (RESTART_PAGE_HEADER, RESTART_AREA,
 * LOG_CLIENT_RECORD, RECORD_PAGE_HEADER, LOG_RECORD).
 */
#include "lfswrite.h"

#define NDEBUG
#include <debug.h>

/* On-page byte offsets shared with mkntfs build_rstr_page and the parser. */
#define LFS_RSTR_USA_OFFSET        0x28
#define LFS_RSTR_AREA_OFFSET       0x40
#define LFS_RSTR_CLIENT_OFFSET     0x70
#define LFS_RCRD_USA_OFFSET        0x28
#define LFS_RCRD_MAGIC             0x44524352

/* FUNCTIONS ****************************************************************/

ULONG
NtfsLfsSeqNumberBits(ULONGLONG FileSize)
{
    ULONG Bits = 0;

    while (FileSize != 0)
    {
        FileSize >>= 1;
        Bits++;
    }

    /* A degenerate / tiny log would push this above 64; clamp so the LSN
     * encode/decode field math stays well-defined.  Real logs are megabytes
     * (bit_length >= 21), so the result is comfortably in the 40s. */
    if (Bits >= 67)
        return 1;
    return 67 - Bits;
}

ULONGLONG
NtfsLfsMakeLsn(ULONG SeqNumberBits, ULONGLONG SeqNumber, ULONG ByteOffset)
{
    ULONG OffsetBits;
    ULONGLONG OffsetPart;

    if (SeqNumberBits == 0 || SeqNumberBits >= 64)
    {
        /* Degenerate / out-of-range: fall back to a pure-offset LSN so the
         * round-trip stays self-consistent. */
        return (ULONGLONG)(ByteOffset >> 3);
    }

    OffsetBits = 64 - SeqNumberBits;
    OffsetPart = (ULONGLONG)(ByteOffset >> 3);
    /* Mask the offset to its field so a stray high bit can't bleed into the
     * sequence-number field. */
    if (OffsetBits < 64)
        OffsetPart &= ((ULONGLONG)1 << OffsetBits) - 1;

    return (SeqNumber << OffsetBits) | OffsetPart;
}

ULONG
NtfsLfsLsnToOffset(ULONG SeqNumberBits, ULONGLONG Lsn)
{
    ULONG OffsetBits;
    ULONGLONG OffsetPart;

    if (SeqNumberBits == 0 || SeqNumberBits >= 64)
        return (ULONG)(Lsn << 3);

    OffsetBits = 64 - SeqNumberBits;
    OffsetPart = Lsn & (((ULONGLONG)1 << OffsetBits) - 1);
    return (ULONG)(OffsetPart << 3);
}

/* The update sequence number of a record page written in this lap: it
 * differs from the previous lap's, so a torn write that leaves sectors of
 * both behind fails the page's fixup. */
static USHORT
LfsPageUsn(ULONGLONG SeqNumber)
{
    return (USHORT)(SeqNumber % 0xFFFE) + 1;
}

static NTSTATUS
LfsBuildRestartPage(ULONG SectorSize,
                    PUCHAR Page,
                    ULONG PageSize,
                    ULONGLONG FileSize,
                    ULONGLONG CurrentLsn,
                    ULONGLONG OldestLsn,
                    ULONG LastLsnDataLength,
                    BOOLEAN Clean,
                    USHORT Usn)
{
    PNTFS_RECORD_HEADER Hdr;
    NTFS_LFS_RESTART_AREA Area;
    NTFS_LFS_CLIENT_RECORD Client;
    DEVICE_EXTENSION Shim;
    USHORT UsaCount;

    if (Page == NULL || SectorSize == 0 || PageSize < SectorSize ||
        (PageSize % SectorSize) != 0)
        return STATUS_INVALID_PARAMETER;

    /* UsaCount = sectors-per-page + 1 (one sentinel word + one saved word
     * per sector).  The USA must fit between LFS_RSTR_USA_OFFSET and the
     * restart area at 0x40. */
    UsaCount = (USHORT)(PageSize / SectorSize + 1);
    if ((ULONG)LFS_RSTR_USA_OFFSET + (ULONG)UsaCount * sizeof(USHORT) >
        LFS_RSTR_AREA_OFFSET)
        return STATUS_INVALID_PARAMETER;
    if (LFS_RSTR_CLIENT_OFFSET + sizeof(NTFS_LFS_CLIENT_RECORD) > PageSize)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Page, PageSize);

    Hdr = (PNTFS_RECORD_HEADER)Page;
    Hdr->Type = NTFS_LOG_RESTART_MAGIC;
    Hdr->UsaOffset = LFS_RSTR_USA_OFFSET;
    Hdr->UsaCount = UsaCount;
    Hdr->Lsn = 0;

    ((PNTFS_LOG_RESTART_HEADER)Page)->SystemPageSize = PageSize;
    ((PNTFS_LOG_RESTART_HEADER)Page)->LogPageSize = PageSize;
    ((PNTFS_LOG_RESTART_HEADER)Page)->RestartOffset = LFS_RSTR_AREA_OFFSET;
    ((PNTFS_LOG_RESTART_HEADER)Page)->MajorVersion = 1;
    ((PNTFS_LOG_RESTART_HEADER)Page)->MinorVersion = 1;
    /* AddFixupArray increments the seed before stamping it. */
    *(USHORT *)(Page + LFS_RSTR_USA_OFFSET) = (USHORT)(Usn - 1);

    RtlZeroMemory(&Area, sizeof(Area));
    Area.CurrentLsn          = CurrentLsn;
    Area.LogClients          = 1;
    /* NTFS occupies client slot 0 and is the only client that ever registers,
     * so the in-use chain is that one slot and the free chain is empty: the
     * restart LSN recovery needs lives in the client record of an in-use
     * client. */
    Area.ClientFreeList      = NTFS_LFS_CLIENT_NONE;
    Area.ClientInUseList     = NTFS_LFS_CLIENT_INDEX_NTFS;
    Area.Flags               = Clean ? NTFS_LFS_RESTART_FLAG_CLEAN : 0;
    Area.SeqNumberBits       = NtfsLfsSeqNumberBits(FileSize);
    Area.RestartAreaLength   = sizeof(NTFS_LFS_RESTART_AREA) + sizeof(NTFS_LFS_CLIENT_RECORD);
    Area.ClientArrayOffset   = (USHORT)sizeof(NTFS_LFS_RESTART_AREA);
    Area.FileSize            = FileSize;
    Area.LastLsnDataLength   = LastLsnDataLength;
    Area.RecordHeaderLength  = NTFS_LFS_RECORD_HEADER_LENGTH;
    Area.LogPageDataOffset   = NTFS_LFS_PAGE_DATA_OFFSET;
    Area.RestartOpenLogCount = 1;
    Area.Reserved            = 0;
    RtlCopyMemory(Page + LFS_RSTR_AREA_OFFSET, &Area, sizeof(Area));
    C_ASSERT(LFS_RSTR_AREA_OFFSET + sizeof(NTFS_LFS_RESTART_AREA) == LFS_RSTR_CLIENT_OFFSET);

    /* OldestLsn is where recovery starts; zero, no record is needed.  No
     * NTFS restart record is written: ClientRestartLsn stays zero. */
    RtlZeroMemory(&Client, sizeof(Client));
    Client.OldestLsn        = OldestLsn;
    Client.ClientRestartLsn = 0;
    Client.PrevClient       = NTFS_LFS_CLIENT_NONE;
    Client.NextClient       = NTFS_LFS_CLIENT_NONE;
    Client.SeqNumber        = NTFS_LFS_CLIENT_SEQ_NTFS;
    Client.ClientNameLength = (ULONG)(sizeof(NTFS_LFS_CLIENT_NAME_NTFS) - sizeof(WCHAR));
    RtlCopyMemory(Client.ClientName, NTFS_LFS_CLIENT_NAME_NTFS, Client.ClientNameLength);
    RtlCopyMemory(Page + LFS_RSTR_CLIENT_OFFSET, &Client, sizeof(Client));

    /* AddFixupArray wants a Vcb only for the sector size. */
    RtlZeroMemory(&Shim, sizeof(Shim));
    Shim.NtfsInfo.BytesPerSector = SectorSize;
    return AddFixupArray(&Shim, Hdr);
}

NTSTATUS
NtfsLfsBuildRestartPage(PDEVICE_EXTENSION Vcb,
                        PUCHAR Page,
                        ULONG PageSize,
                        ULONGLONG FileSize,
                        ULONGLONG CurrentLsn,
                        ULONG LastLsnDataLength,
                        BOOLEAN Clean)
{
    if (Vcb == NULL)
        return STATUS_INVALID_PARAMETER;
    return LfsBuildRestartPage(Vcb->NtfsInfo.BytesPerSector, Page, PageSize, FileSize,
                               CurrentLsn, 0, LastLsnDataLength, Clean, 1);
}

/* Client data = NTFS sub-header (0x20) | LCNs | redo | undo, redo first,
 * each 8-byte aligned.  Client data starts at the 0x30 generic-header end. */
static VOID
LfsRecordGeometry(ULONG RedoLength,
                  ULONG UndoLength,
                  ULONG LcnsToFollow,
                  PULONG RedoAbs,
                  PULONG UndoAbs,
                  PULONG ClientDataLength,
                  PULONG TotalLength)
{
    ULONG R, U;

    R = ALIGN_UP_BY(NTFS_LFS_RECORD_HEADER_LENGTH + NTFS_LFS_ATTR_HEADER_LENGTH +
                     LcnsToFollow * sizeof(LONGLONG), 8);
    U = ALIGN_UP_BY(R + RedoLength, 8);

    *RedoAbs = R;
    *UndoAbs = U;
    *ClientDataLength = (U - NTFS_LFS_RECORD_HEADER_LENGTH) + UndoLength;
    *TotalLength = ALIGN_UP_BY(NTFS_LFS_RECORD_HEADER_LENGTH + *ClientDataLength, 8);
}

ULONG
NtfsLfsRecordLength(ULONG RedoLength, ULONG UndoLength, ULONG LcnsToFollow)
{
    ULONG RedoAbs, UndoAbs, ClientDataLength, TotalLength;

    LfsRecordGeometry(RedoLength, UndoLength, LcnsToFollow,
                      &RedoAbs, &UndoAbs, &ClientDataLength, &TotalLength);
    return TotalLength;
}

ULONG
NtfsLfsStagePages(ULONG PageSize, ULONG BatchBytes)
{
    ULONG PerPage = PageSize - NTFS_LFS_PAGE_DATA_OFFSET;

    /* A record starts only where its header fits, so each record can leave
     * up to a header's worth unused at a page end; one page more covers it
     * for the batches the writer emits (at most a few records). */
    return (BatchBytes + PerPage - 1) / PerPage + 2;
}

NTSTATUS
NtfsLfsInitWriteContext(PLFS_WRITE_CONTEXT Ctx,
                        ULONG SectorSize,
                        ULONG LogSize,
                        ULONG PageSize,
                        ULONGLONG LastLsn,
                        PUCHAR Stage,
                        ULONG StagePages,
                        PUCHAR Scratch,
                        ULONG ScratchLength)
{
    ULONG OffsetBits;

    if (Ctx == NULL || Stage == NULL || Scratch == NULL || StagePages == 0)
        return STATUS_INVALID_PARAMETER;
    if (SectorSize == 0 || PageSize < SectorSize || (PageSize % SectorSize) != 0 ||
        LFS_RCRD_USA_OFFSET + (PageSize / SectorSize + 1) * sizeof(USHORT) > NTFS_LFS_PAGE_DATA_OFFSET)
        return STATUS_INVALID_PARAMETER;

    /* LFS 1.1 reserves two restart pages and two tail-copy pages. */
    if (LogSize < PageSize * (NTFS_LFS_FIRST_RECORD_PAGE + 2) || (LogSize % PageSize) != 0)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->LogSize = LogSize;
    Ctx->PageSize = PageSize;
    Ctx->SectorSize = SectorSize;
    Ctx->FirstRcrdOffset = PageSize * NTFS_LFS_FIRST_RECORD_PAGE;
    Ctx->SeqNumberBits = NtfsLfsSeqNumberBits((ULONGLONG)LogSize);
    OffsetBits = 64 - Ctx->SeqNumberBits;
    /* LSNs keep increasing across sessions: the first record of this one
     * is in the sequence after the last LSN the restart area named. */
    Ctx->CurrentSeqNumber = (LastLsn >> OffsetBits) + 1;
    Ctx->NextPage = Ctx->FirstRcrdOffset;
    Ctx->LastLsn = LastLsn;
    Ctx->OldestLsn = 0;
    Ctx->RestartUsn = (USHORT)(LastLsn >> 3);
    Ctx->ClientId = NTFS_LFS_CLIENT_INDEX_NTFS;
    Ctx->Stage = Stage;
    Ctx->StagePages = StagePages;
    Ctx->Scratch = Scratch;
    Ctx->ScratchLength = ScratchLength;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsLfsFormatRecord(PLFS_WRITE_CONTEXT Ctx,
                    const LFS_EMIT_PARAMS *Params,
                    ULONGLONG ThisLsn,
                    ULONGLONG PreviousLsn,
                    PUCHAR Buffer,
                    ULONG BufferLength,
                    PULONG RecordLengthOut)
{
    PNTFS_LFS_RECORD_HEADER Rec;
    ULONG RedoAbs, UndoAbs;       /* absolute offsets from record start     */
    ULONG ClientDataLength;
    ULONG TotalLength;

    if (Ctx == NULL || Params == NULL || Buffer == NULL)
        return STATUS_INVALID_PARAMETER;

    if ((Params->RedoLength != 0 && Params->RedoData == NULL) ||
        (Params->UndoLength != 0 && Params->UndoData == NULL) ||
        (Params->LcnsToFollow != 0 && Params->Lcns == NULL))
        return STATUS_INVALID_PARAMETER;

    LfsRecordGeometry(Params->RedoLength, Params->UndoLength, Params->LcnsToFollow,
                      &RedoAbs, &UndoAbs, &ClientDataLength, &TotalLength);

    if (RedoAbs - NTFS_LFS_RECORD_HEADER_LENGTH > MAXUSHORT ||
        UndoAbs - NTFS_LFS_RECORD_HEADER_LENGTH > MAXUSHORT)
        return STATUS_INVALID_PARAMETER;
    if (BufferLength < TotalLength)
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(Buffer, TotalLength);

    Rec = (PNTFS_LFS_RECORD_HEADER)Buffer;
    Rec->ThisLsn = ThisLsn;
    Rec->ClientPreviousLsn = PreviousLsn;
    Rec->ClientUndoNextLsn = Params->ClientUndoNextLsn;
    Rec->ClientDataLength = ClientDataLength;
    Rec->ClientId = NTFS_LFS_CLIENT_SEQ_NTFS;  /* client sequence number */
    Rec->Alignment = Ctx->ClientId;             /* client array index */
    Rec->RecordType = NTFS_LFS_RECTYPE_NORMAL;
    Rec->TransactionId = Params->TransactionId;
    Rec->Flags = 0;
    Rec->RedoOperation = Params->RedoOperation;
    Rec->UndoOperation = Params->UndoOperation;
    /* Offsets belong to the NTFS client record, which starts after the
     * generic 0x30-byte LFS header. They exclude that generic header. */
    Rec->RedoOffset = (USHORT)(RedoAbs - NTFS_LFS_RECORD_HEADER_LENGTH);
    Rec->RedoLength = Params->RedoLength;
    Rec->UndoOffset = (USHORT)(UndoAbs - NTFS_LFS_RECORD_HEADER_LENGTH);
    Rec->UndoLength = Params->UndoLength;
    Rec->TargetAttribute = Params->TargetAttribute;
    Rec->LcnsToFollow = Params->LcnsToFollow;
    Rec->RecordOffset = Params->RecordOffset;
    Rec->AttributeOffset = Params->AttributeOffset;
    Rec->MftClusterIndex = Params->ClusterBlockOffset;
    Rec->Alignment2 = Params->AttributeFlags;
    Rec->TargetVcn = Params->TargetVcn;

    if (Params->LcnsToFollow != 0)
        RtlCopyMemory(Buffer + sizeof(*Rec), Params->Lcns,
                       (ULONG)Params->LcnsToFollow * sizeof(LONGLONG));
    if (Params->RedoLength != 0)
        RtlCopyMemory(Buffer + RedoAbs, Params->RedoData, Params->RedoLength);
    if (Params->UndoLength != 0)
        RtlCopyMemory(Buffer + UndoAbs, Params->UndoData, Params->UndoLength);

    if (RecordLengthOut != NULL)
        *RecordLengthOut = TotalLength;

    return STATUS_SUCCESS;
}

/* The page after Page, wrapping past the last page to the first record
 * page under the next sequence number. */
static ULONG
LfsNextPage(PLFS_WRITE_CONTEXT Ctx, ULONG Page, PULONGLONG SeqNumber)
{
    Page += Ctx->PageSize;
    if (Page >= Ctx->LogSize)
    {
        Page = Ctx->FirstRcrdOffset;
        (*SeqNumber)++;
    }
    return Page;
}

/* Page Offset, about to be written in lap SeqNumber, holds records of the
 * previous lap: all of them are below that lap's LSN of the next page. */
static BOOLEAN
LfsPageReusable(PLFS_WRITE_CONTEXT Ctx, ULONG Offset, ULONGLONG SeqNumber)
{
    if (Ctx->OldestLsn == 0 || SeqNumber == 0)
        return TRUE;
    return Ctx->OldestLsn >= NtfsLfsMakeLsn(Ctx->SeqNumberBits, SeqNumber - 1,
                                            Offset + Ctx->PageSize);
}

static VOID
LfsInitRecordPage(PLFS_WRITE_CONTEXT Ctx, PUCHAR Page, USHORT Count, USHORT Position)
{
    PNTFS_LFS_PAGE_HEADER Hdr = (PNTFS_LFS_PAGE_HEADER)Page;

    RtlZeroMemory(Page, Ctx->PageSize);
    Hdr->Header.Type = LFS_RCRD_MAGIC;
    Hdr->Header.UsaOffset = LFS_RCRD_USA_OFFSET;
    Hdr->Header.UsaCount = (USHORT)(Ctx->PageSize / Ctx->SectorSize + 1);
    Hdr->PageCount = Count;
    Hdr->PagePosition = Position;
    Hdr->NextRecordOffset = NTFS_LFS_PAGE_DATA_OFFSET;
}

NTSTATUS
NtfsLfsEmitBatch(PLFS_WRITE_CONTEXT Ctx,
                 const LFS_EMIT_PARAMS *Params,
                 ULONG Count,
                 PULONGLONG Lsns)
{
    ULONG Pos, Page, Pages, i, Length, Room, Copied, Index, Part, LastPage;
    ULONGLONG Seq, LastSeq, PreviousLsn, ThisLsn;
    DEVICE_EXTENSION Shim;
    NTSTATUS Status;

    if (Ctx == NULL || Ctx->Stage == NULL || Params == NULL || Lsns == NULL || Count == 0)
        return STATUS_INVALID_PARAMETER;

    /* Lay the batch out: pages it needs, each one reusable. */
    Seq = Ctx->CurrentSeqNumber;
    Page = Ctx->NextPage;
    Pos = Page + NTFS_LFS_PAGE_DATA_OFFSET;
    Pages = 1;
    if (!LfsPageReusable(Ctx, Page, Seq))
        return STATUS_LOG_FILE_FULL;
    for (i = 0; i < Count; i++)
    {
        Length = NtfsLfsRecordLength(Params[i].RedoLength, Params[i].UndoLength,
                                     Params[i].LcnsToFollow);
        if (Length > Ctx->ScratchLength)
            return STATUS_INVALID_PARAMETER;
        while (Length != 0)
        {
            Room = Ctx->PageSize - (Pos - Page);
            /* A record starts only where its header fits. */
            if (Room == 0 || (Room < NTFS_LFS_RECORD_HEADER_LENGTH &&
                              Length == NtfsLfsRecordLength(Params[i].RedoLength,
                                                            Params[i].UndoLength,
                                                            Params[i].LcnsToFollow)))
            {
                Page = LfsNextPage(Ctx, Page, &Seq);
                Pos = Page + NTFS_LFS_PAGE_DATA_OFFSET;
                if (++Pages > Ctx->StagePages ||
                    Pages > (Ctx->LogSize - Ctx->FirstRcrdOffset) / Ctx->PageSize)
                    return Pages > Ctx->StagePages ? STATUS_INVALID_PARAMETER : STATUS_LOG_FILE_FULL;
                if (!LfsPageReusable(Ctx, Page, Seq))
                    return STATUS_LOG_FILE_FULL;
                continue;
            }
            Part = min(Length, Room);
            Pos += Part;
            Length -= Part;
        }
    }

    /* Room left behind the batch for commits. */
    for (i = 0, LastPage = Page, LastSeq = Seq; i < Ctx->ReservePages; i++)
    {
        LastPage = LfsNextPage(Ctx, LastPage, &LastSeq);
        if (Pages + i + 1 > (Ctx->LogSize - Ctx->FirstRcrdOffset) / Ctx->PageSize ||
            !LfsPageReusable(Ctx, LastPage, LastSeq))
            return STATUS_LOG_FILE_FULL;
    }

    /* Build the pages: the layout above cannot fail any more. */
    for (i = 0; i < Pages; i++)
        LfsInitRecordPage(Ctx, Ctx->Stage + i * Ctx->PageSize, (USHORT)Pages, (USHORT)(i + 1));
    Seq = Ctx->CurrentSeqNumber;
    Page = Ctx->NextPage;
    Pos = Page + NTFS_LFS_PAGE_DATA_OFFSET;
    Index = 0;
    PreviousLsn = Ctx->LastLsn;
    for (i = 0; i < Count; i++)
    {
        PNTFS_LFS_RECORD_HEADER Rec = (PNTFS_LFS_RECORD_HEADER)Ctx->Scratch;
        PNTFS_LFS_PAGE_HEADER Hdr;

        Room = Ctx->PageSize - (Pos - Page);
        if (Room < NTFS_LFS_RECORD_HEADER_LENGTH)
        {
            Page = LfsNextPage(Ctx, Page, &Seq);
            Pos = Page + NTFS_LFS_PAGE_DATA_OFFSET;
            Index++;
            Room = Ctx->PageSize - NTFS_LFS_PAGE_DATA_OFFSET;
        }
        ThisLsn = NtfsLfsMakeLsn(Ctx->SeqNumberBits, Seq, Pos);
        Status = NtfsLfsFormatRecord(Ctx, &Params[i], ThisLsn, PreviousLsn,
                                     Ctx->Scratch, Ctx->ScratchLength, &Length);
        ASSERT(NT_SUCCESS(Status));
        if (!NT_SUCCESS(Status))
            return Status;
        if (Length > Room)
            Rec->Flags |= NTFS_LFS_RECORD_FLAG_MULTI_PAGE;
        Copied = 0;
        for (;;)
        {
            Hdr = (PNTFS_LFS_PAGE_HEADER)(Ctx->Stage + Index * Ctx->PageSize);
            Part = min(Length - Copied, Room);
            RtlCopyMemory((PUCHAR)Hdr + (Pos - Page), Ctx->Scratch + Copied, Part);
            Hdr->Header.Lsn = ThisLsn;
            Copied += Part;
            Pos += Part;
            if (Copied == Length)
            {
                Hdr->Flags |= NTFS_LFS_PAGE_FLAG_RECORD_END;
                Hdr->LastEndLsn = ThisLsn;
                Hdr->NextRecordOffset = (USHORT)(Pos - Page);
                break;
            }
            Page = LfsNextPage(Ctx, Page, &Seq);
            Pos = Page + NTFS_LFS_PAGE_DATA_OFFSET;
            Index++;
            Room = Ctx->PageSize - NTFS_LFS_PAGE_DATA_OFFSET;
        }
        Lsns[i] = ThisLsn;
        PreviousLsn = ThisLsn;
        Ctx->LastDataLength = Rec->ClientDataLength;
    }
    ASSERT(Index + 1 == Pages);

    RtlZeroMemory(&Shim, sizeof(Shim));
    Shim.NtfsInfo.BytesPerSector = Ctx->SectorSize;
    LastPage = Ctx->NextPage;
    LastSeq = Ctx->CurrentSeqNumber;
    for (i = 0; i < Pages; i++)
    {
        PNTFS_RECORD_HEADER Hdr = (PNTFS_RECORD_HEADER)(Ctx->Stage + i * Ctx->PageSize);

        if (i != 0)
            LastPage = LfsNextPage(Ctx, LastPage, &LastSeq);
        *(PUSHORT)((PUCHAR)Hdr + LFS_RCRD_USA_OFFSET) = (USHORT)(LfsPageUsn(LastSeq) - 1);
        AddFixupArray(&Shim, Hdr);
    }

    Ctx->StagedFirst = Ctx->NextPage;
    Ctx->StagedPages = Pages;
    Ctx->NextPage = LfsNextPage(Ctx, LastPage, &LastSeq);
    Ctx->CurrentSeqNumber = LastSeq;
    Ctx->LastLsn = PreviousLsn;
    if (Ctx->OldestLsn == 0)
        Ctx->OldestLsn = Lsns[0];
    return STATUS_SUCCESS;
}

ULONGLONG
NtfsLfsNextLsn(PLFS_WRITE_CONTEXT Ctx)
{
    return NtfsLfsMakeLsn(Ctx->SeqNumberBits, Ctx->CurrentSeqNumber,
                          Ctx->NextPage + NTFS_LFS_PAGE_DATA_OFFSET);
}

ULONG
NtfsLfsStagedPageOffset(PLFS_WRITE_CONTEXT Ctx, ULONG Index)
{
    ULONG Offset = Ctx->StagedFirst;
    ULONGLONG Seq = 0;

    ASSERT(Index < Ctx->StagedPages);
    while (Index-- != 0)
        Offset = LfsNextPage(Ctx, Offset, &Seq);
    return Offset;
}

NTSTATUS
NtfsLfsBuildNextRestart(PLFS_WRITE_CONTEXT Ctx,
                        BOOLEAN Clean,
                        PUCHAR Page,
                        PULONG OffsetOut)
{
    NTSTATUS Status;

    if (Ctx == NULL || Page == NULL || OffsetOut == NULL)
        return STATUS_INVALID_PARAMETER;
    if (++Ctx->RestartUsn == 0)
        Ctx->RestartUsn = 1;
    /* A dirty restart area names where recovery starts scanning: the
     * oldest record still needed, or, when none is, where the next record
     * will go (the LSN past CurrentLsn tells it from a needed one). */
    Status = LfsBuildRestartPage(Ctx->SectorSize, Page, Ctx->PageSize, Ctx->LogSize,
                                 Ctx->LastLsn,
                                 Clean ? 0 : (Ctx->OldestLsn != 0 ? Ctx->OldestLsn : NtfsLfsNextLsn(Ctx)),
                                 Clean ? 0 : Ctx->LastDataLength, Clean, Ctx->RestartUsn);
    if (!NT_SUCCESS(Status))
        return Status;
    *OffsetOut = Ctx->NextRestart * Ctx->PageSize;
    Ctx->NextRestart ^= 1;
    return STATUS_SUCCESS;
}

/* EOF */
