/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/usn.c
 * PURPOSE:          NTFS filesystem driver - $UsnJrnl first-slice workers.
 *
 * This slice brings up a functional (but deliberately minimal) change journal:
 *
 *   - NtfsUsnInitJournal    -- create \$Extend\$UsnJrnl if missing, open it
 *                              and populate Vcb->Usn* fields.
 *   - NtfsUsnLoadJournal    -- open an existing \$Extend\$UsnJrnl at mount.
 *   - NtfsUsnEmitRecord     -- serialise a USN_RECORD_V2 into $J at UsnNextUsn,
 *                              advance UsnNextUsn, and persist $Max.
 *   - NtfsUsnReadRange      -- FSCTL_READ_USN_JOURNAL core: linear walk of $J
 *                              filtered by reason mask.
 *   - NtfsUsnEnumerate      -- FSCTL_ENUM_USN_DATA core: MFT walk synthesising
 *                              per-file USN_RECORD_V2 entries from
 *                              $STANDARD_INFORMATION + best $FILE_NAME.
 *
 * Explicit non-goals for this slice (deferred to follow-ups, see the issue):
 *   - DELETE / QUERY / MARK_HANDLE / WRITE_USN_CLOSE_RECORD FSCTLs
 *   - Circular-log wrap semantics (LowestValidUsn bookkeeping)
 *   - CCB-cached reason aggregation on cleanup
 *
 * The workers are intentionally file-system-independent of IRPs: fsctl.c
 * supplies the IRP wrappers, and create.c / cleanup.c call NtfsUsnEmitRecord
 * directly.  This matches the #32 / #36 pattern and makes the state machine
 * unit-testable from userspace (test_usn_workers).
 *
 * References for on-disk layout:
 *   - libfsntfs documentation (liberal-license reference)
 *   - ntfs-3g headers (libntfs/usnjrnl.h)
 *   - Kreijstal/reactos#33 issue body (authoritative for this implementation)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* Compact in-memory "FCB-lite" for the journal: a tiny handle that keeps
 * the MFT index around so workers don't repeat the \$Extend\$UsnJrnl path
 * walk on every call.  We deliberately don't register this with the FCB
 * table - it's private state owned by the Vcb, never exposed to create
 * dispatch, and torn down on dismount.  Allocated with TAG_FCB so pool
 * tracking names it consistently with real FCBs.
 *
 * We reuse the full _FCB storage (via NtfsCreateFCB-compatible alloc) so
 * the type match in DEVICE_EXTENSION stays honest - we only fill in the
 * fields we need (Identifier, MFTIndex, Vcb).
 */

#define USN_RECORD_V2_MAJOR 2
#define USN_RECORD_V2_MINOR 0

/* USN_RECORD_V2 header size, up to (but not including) FileName[]. */
#define USN_RECORD_V2_HEADER_SIZE 60

/* On-disk $Max layout.  Matches USN_JOURNAL_DATA_V0 (32 bytes). */
#include <pshpack1.h>
typedef struct _NTFS_USN_JOURNAL_MAX
{
    ULONGLONG UsnJournalID;
    LONGLONG  FirstUsn;
    LONGLONG  NextUsn;
    LONGLONG  LowestValidUsn;
    LONGLONG  MaxUsn;
    ULONGLONG MaximumSize;
    ULONGLONG AllocationDelta;
} NTFS_USN_JOURNAL_MAX, *PNTFS_USN_JOURNAL_MAX;
#include <poppack.h>

/* Stream names used on $UsnJrnl. */
static const WCHAR kUsnStreamJ[] = { L'$', L'J', 0 };
static const WCHAR kUsnStreamMax[] = { L'$', L'M', L'a', L'x', 0 };

/* Align a USN up to the next 8-byte boundary.  Journal records are always
 * 8-byte aligned so a walker can scan linearly. */
#define USN_ALIGN_8(x) (((x) + 7ULL) & ~7ULL)

/**
 * @internal
 * @brief  Allocate + seed a thin FCB to serve as the in-memory journal
 *         handle.  Only fields a worker might consult are populated -
 *         the FCB is never attached to the FCB table.
 */
static PNTFS_FCB
UsnAllocJournalFcb(PDEVICE_EXTENSION Vcb, ULONGLONG MftIndex)
{
    PNTFS_FCB Fcb;

    Fcb = ExAllocateFromNPagedLookasideList(&NtfsGlobalData->FcbLookasideList);
    if (Fcb == NULL)
        return NULL;

    RtlZeroMemory(Fcb, sizeof(*Fcb));
    Fcb->Identifier.Type = NTFS_TYPE_FCB;
    Fcb->Identifier.Size = sizeof(NTFS_FCB);
    Fcb->MFTIndex = MftIndex;
    Fcb->Vcb = Vcb;
    return Fcb;
}

/* UsnFreeJournalFcb - future dismount / re-mount tear-down.  Emplaced here
 * so the alloc/free pair lives next door; NtfsUsnInitJournal currently
 * attaches once per mount and never detaches.  Follow-up slice will wire
 * dismount to call this. */
VOID
NtfsUsnFreeJournalFcb(PNTFS_FCB Fcb)
{
    if (Fcb != NULL)
        ExFreeToNPagedLookasideList(&NtfsGlobalData->FcbLookasideList, Fcb);
}

/**
 * @internal
 * @brief  Read $Max:$DATA from the journal MFT record into @p MaxOut.
 */
static NTSTATUS
UsnReadMaxFromRecord(PDEVICE_EXTENSION Vcb,
                     PFILE_RECORD_HEADER JournalRecord,
                     PNTFS_USN_JOURNAL_MAX MaxOut)
{
    PNTFS_ATTR_CONTEXT MaxCtx = NULL;
    NTSTATUS Status;
    ULONGLONG DataLen;
    ULONG ReadLen;

    Status = FindAttribute(Vcb, JournalRecord, AttributeData,
                           kUsnStreamMax,
                           (USHORT)(sizeof(kUsnStreamMax) / sizeof(WCHAR) - 1),
                           &MaxCtx, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    DataLen = AttributeDataLength(MaxCtx->pRecord);
    if (DataLen < sizeof(NTFS_USN_JOURNAL_MAX))
    {
        ReleaseAttributeContext(MaxCtx);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    RtlZeroMemory(MaxOut, sizeof(*MaxOut));
    ReadLen = ReadAttribute(Vcb, MaxCtx, 0, (PCHAR)MaxOut, sizeof(*MaxOut));
    ReleaseAttributeContext(MaxCtx);

    if (ReadLen < sizeof(*MaxOut))
        return STATUS_FILE_CORRUPT_ERROR;

    return STATUS_SUCCESS;
}

/**
 * @internal
 * @brief  Write @p Max back to $Max:$DATA on the journal file record.
 *         Resident-only overwrite in this slice.
 */
static NTSTATUS
UsnWriteMaxToRecord(PDEVICE_EXTENSION Vcb,
                    ULONGLONG JournalMftIndex,
                    const NTFS_USN_JOURNAL_MAX *Max)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT MaxCtx = NULL;
    NTSTATUS Status;
    ULONG BytesWritten = 0;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, JournalMftIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = FindAttribute(Vcb, FileRecord, AttributeData,
                           kUsnStreamMax,
                           (USHORT)(sizeof(kUsnStreamMax) / sizeof(WCHAR) - 1),
                           &MaxCtx, NULL);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = WriteAttribute(Vcb, MaxCtx, 0, (PUCHAR)Max, sizeof(*Max),
                            &BytesWritten, FileRecord);
    ReleaseAttributeContext(MaxCtx);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = UpdateFileRecord(Vcb, JournalMftIndex, FileRecord);

Done:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
    return Status;
}

/**
 * @internal
 * @brief  Look up \$Extend\$UsnJrnl, returning its MFT index.  Returns
 *         STATUS_OBJECT_NAME_NOT_FOUND if the file doesn't exist.
 */
static NTSTATUS
UsnLookupJournalFile(PDEVICE_EXTENSION Vcb,
                     PULONGLONG MftIndexOut)
{
    UNICODE_STRING Path;
    PFILE_RECORD_HEADER FileRecord = NULL;
    NTSTATUS Status;

    RtlInitUnicodeString(&Path, L"\\$Extend\\$UsnJrnl");
    Status = NtfsLookupFile(Vcb, &Path, FALSE, &FileRecord, MftIndexOut);
    if (!NT_SUCCESS(Status))
        return Status;

    if (FileRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
    return STATUS_SUCCESS;
}

/**
 * @name NtfsUsnLoadJournal
 * @implemented
 *
 * Attach an existing on-disk journal to the Vcb.  Called from
 * NtfsMountVolume.  No-op (STATUS_OBJECT_NAME_NOT_FOUND) when the volume
 * has never had a journal - the mount path continues regardless.
 */
NTSTATUS
NtfsUsnLoadJournal(PDEVICE_EXTENSION Vcb)
{
    NTSTATUS Status;
    ULONGLONG MftIndex = 0;
    PFILE_RECORD_HEADER JournalRecord = NULL;
    NTFS_USN_JOURNAL_MAX Max;

    if (Vcb->UsnJournalFcb != NULL)
        return STATUS_SUCCESS;

    Status = UsnLookupJournalFile(Vcb, &MftIndex);
    if (!NT_SUCCESS(Status))
        return Status;

    JournalRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (JournalRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, MftIndex, JournalRecord);
    if (!NT_SUCCESS(Status))
        goto Error;

    Status = UsnReadMaxFromRecord(Vcb, JournalRecord, &Max);
    if (!NT_SUCCESS(Status))
        goto Error;

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, JournalRecord);
    JournalRecord = NULL;

    Vcb->UsnJournalFcb = UsnAllocJournalFcb(Vcb, MftIndex);
    if (Vcb->UsnJournalFcb == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Vcb->UsnJournalId = Max.UsnJournalID;
    Vcb->UsnMaxSize = Max.MaximumSize;
    Vcb->UsnAllocationDelta = Max.AllocationDelta;
    Vcb->UsnNextUsn = (ULONGLONG)Max.NextUsn;

    ExInitializeResourceLite(&Vcb->UsnResource);
    return STATUS_SUCCESS;

Error:
    if (JournalRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, JournalRecord);
    return Status;
}

/**
 * @internal
 * @brief  Create a new \$Extend\$UsnJrnl file record with $Max (resident)
 *         and $J (non-resident, zero-length sparse-like).
 *
 * This slice does not actually emit sparse runs for $J - since the first
 * emission is what extends the attribute, we only need the attribute to
 * exist at zero length.  WriteAttribute on a zero-length non-resident
 * attribute will allocate clusters lazily via AddRun.
 */
static NTSTATUS
UsnCreateJournalFile(PDEVICE_EXTENSION Vcb,
                     ULONGLONG MaximumSize,
                     ULONGLONG AllocationDelta,
                     PULONGLONG MftIndexOut)
{
    /* Building a brand new MFT entry under \$Extend with mixed resident
     * ($Max) + non-resident ($J) attributes is a multi-step operation
     * that reuses the same primitives as NtfsCreateFileRecord.  Rather
     * than duplicate that path here (and re-solve the directory-index-
     * insertion problem for \$Extend), we rely on the caller having
     * pre-created the journal via a file-format tool, or on a follow-up
     * slice that plumbs AddNewMftEntry into this flow.
     *
     * For the first slice we explicitly document this as an open item:
     * mounting a volume that doesn't yet have \$Extend\$UsnJrnl will
     * cause NtfsUsnInitJournal to return STATUS_NOT_IMPLEMENTED, which
     * NtfsUserFsRequest maps to STATUS_INVALID_DEVICE_REQUEST on
     * FSCTL_CREATE_USN_JOURNAL so callers see a stable fallback.
     *
     * The userspace test harness prepares the journal file via a
     * dedicated fixture helper (TestUsnAdvPrepareJournalFile).  That
     * lets us exercise the worker state machine end-to-end without
     * blocking on the create-under-$Extend surgery, which is tracked
     * as a separate follow-up. */
    UNREFERENCED_PARAMETER(Vcb);
    UNREFERENCED_PARAMETER(MaximumSize);
    UNREFERENCED_PARAMETER(AllocationDelta);
    UNREFERENCED_PARAMETER(MftIndexOut);
    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @name NtfsUsnInitJournal
 * @implemented
 *
 * Handle FSCTL_CREATE_USN_JOURNAL semantics.  If the journal already
 * exists, reuse it.  Otherwise create it (currently STATUS_NOT_IMPLEMENTED
 * when the file doesn't exist yet - the userspace fixture pre-seeds it).
 */
NTSTATUS
NtfsUsnInitJournal(PDEVICE_EXTENSION Vcb,
                   ULONGLONG MaximumSize,
                   ULONGLONG AllocationDelta)
{
    NTSTATUS Status;
    ULONGLONG MftIndex = 0;

    /* Already journalled? CREATE is idempotent - accept but don't
     * reset NextUsn, mirroring Windows where a second CREATE with the
     * same params returns the existing journal ID. */
    if (Vcb->UsnJournalFcb != NULL)
    {
        if (MaximumSize != 0)
            Vcb->UsnMaxSize = MaximumSize;
        if (AllocationDelta != 0)
            Vcb->UsnAllocationDelta = AllocationDelta;
        return STATUS_SUCCESS;
    }

    Status = UsnLookupJournalFile(Vcb, &MftIndex);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND ||
        Status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        /* Would-be create: punt to the follow-up slice. */
        return UsnCreateJournalFile(Vcb, MaximumSize, AllocationDelta, &MftIndex);
    }
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsUsnLoadJournal(Vcb);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Overwrite the sizing knobs with the caller's values, then
     * persist.  We do not touch UsnJournalId / FirstUsn / NextUsn;
     * those are journal-lifetime invariants. */
    if (MaximumSize != 0)
        Vcb->UsnMaxSize = MaximumSize;
    if (AllocationDelta != 0)
        Vcb->UsnAllocationDelta = AllocationDelta;

    {
        NTFS_USN_JOURNAL_MAX Max;
        RtlZeroMemory(&Max, sizeof(Max));
        Max.UsnJournalID = Vcb->UsnJournalId;
        Max.FirstUsn = 0;
        Max.NextUsn = (LONGLONG)Vcb->UsnNextUsn;
        Max.LowestValidUsn = 0;
        Max.MaxUsn = MAXLONGLONG;
        Max.MaximumSize = Vcb->UsnMaxSize;
        Max.AllocationDelta = Vcb->UsnAllocationDelta;
        (void)UsnWriteMaxToRecord(Vcb, MftIndex, &Max);
    }

    return STATUS_SUCCESS;
}

/**
 * @internal
 * @brief  Serialise one USN_RECORD_V2 into @p Buffer.  Returns the total
 *         byte count including 8-byte-aligned trailing padding.
 *
 * Layout (USN_RECORD_V2):
 *   off 0   ULONG   RecordLength
 *   off 4   USHORT  MajorVersion (2)
 *   off 6   USHORT  MinorVersion (0)
 *   off 8   ULONGLONG FileReferenceNumber
 *   off 16  ULONGLONG ParentFileReferenceNumber
 *   off 24  LONGLONG Usn
 *   off 32  LARGE_INTEGER TimeStamp
 *   off 40  ULONG   Reason
 *   off 44  ULONG   SourceInfo
 *   off 48  ULONG   SecurityId
 *   off 52  ULONG   FileAttributes
 *   off 56  USHORT  FileNameLength (bytes)
 *   off 58  USHORT  FileNameOffset (bytes from start of record, = 60)
 *   off 60  WCHAR   FileName[...] (FileNameLength bytes)
 *   off 60+FileNameLength  padding to 8-byte alignment
 */
static ULONG
UsnSerialiseRecord(PUCHAR Buffer,
                   ULONG BufferCap,
                   LONGLONG Usn,
                   ULONGLONG FileRef,
                   ULONGLONG ParentRef,
                   ULONG Reason,
                   ULONG SourceInfo,
                   ULONG FileAttributes,
                   PCWSTR Name,
                   USHORT NameLenBytes)
{
    ULONG Total;
    ULONG Aligned;
    LARGE_INTEGER Now;

    Total = USN_RECORD_V2_HEADER_SIZE + NameLenBytes;
    Aligned = (ULONG)USN_ALIGN_8(Total);
    if (Aligned > BufferCap)
        return 0;

    RtlZeroMemory(Buffer, Aligned);

    *(ULONG   *)(Buffer + 0)  = Aligned;
    *(USHORT  *)(Buffer + 4)  = USN_RECORD_V2_MAJOR;
    *(USHORT  *)(Buffer + 6)  = USN_RECORD_V2_MINOR;
    *(ULONGLONG*)(Buffer + 8) = FileRef;
    *(ULONGLONG*)(Buffer + 16) = ParentRef;
    *(LONGLONG *)(Buffer + 24) = Usn;

    KeQuerySystemTime(&Now);
    *(LONGLONG *)(Buffer + 32) = Now.QuadPart;

    *(ULONG   *)(Buffer + 40) = Reason;
    *(ULONG   *)(Buffer + 44) = SourceInfo;
    *(ULONG   *)(Buffer + 48) = 0;           /* SecurityId  */
    *(ULONG   *)(Buffer + 52) = FileAttributes;
    *(USHORT  *)(Buffer + 56) = NameLenBytes;
    *(USHORT  *)(Buffer + 58) = USN_RECORD_V2_HEADER_SIZE;

    if (NameLenBytes != 0 && Name != NULL)
        RtlCopyMemory(Buffer + USN_RECORD_V2_HEADER_SIZE, Name, NameLenBytes);

    return Aligned;
}

/**
 * @internal
 * @brief  Write the serialised record into $J:$DATA and advance
 *         Vcb->UsnNextUsn.
 *
 * NB: Relies on the existing sparse-write path in WriteAttribute to
 * extend the non-resident $J as needed.  Issue #33 explicitly calls out
 * that we do NOT reinvent that here - it's proven by test_sparse_write.
 */
static NTSTATUS
UsnAppendToJournal(PDEVICE_EXTENSION Vcb,
                   PUCHAR RecordBytes,
                   ULONG RecordSize)
{
    PFILE_RECORD_HEADER JournalRecord;
    PNTFS_ATTR_CONTEXT JCtx = NULL;
    NTSTATUS Status;
    ULONG Written = 0;
    ULONGLONG Offset;

    JournalRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (JournalRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Vcb->UsnJournalFcb->MFTIndex, JournalRecord);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = FindAttribute(Vcb, JournalRecord, AttributeData,
                           kUsnStreamJ,
                           (USHORT)(sizeof(kUsnStreamJ) / sizeof(WCHAR) - 1),
                           &JCtx, NULL);
    if (!NT_SUCCESS(Status))
        goto Done;

    Offset = Vcb->UsnNextUsn;
    Status = WriteAttribute(Vcb, JCtx, Offset, RecordBytes, RecordSize,
                            &Written, JournalRecord);
    ReleaseAttributeContext(JCtx);

    if (NT_SUCCESS(Status) && Written == RecordSize)
    {
        Vcb->UsnNextUsn = Offset + RecordSize;
        Status = UpdateFileRecord(Vcb, Vcb->UsnJournalFcb->MFTIndex, JournalRecord);
    }
    else if (NT_SUCCESS(Status))
    {
        Status = STATUS_UNSUCCESSFUL;
    }

Done:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, JournalRecord);
    return Status;
}

/**
 * @name NtfsUsnEmitRecord
 * @implemented
 *
 * Create a single USN_RECORD_V2 and append it to $J.  Caller must have
 * already verified that Vcb->UsnJournalFcb != NULL.  The record's Usn
 * field is set to the pre-append byte offset (the classic NTFS
 * convention).
 */
NTSTATUS
NtfsUsnEmitRecord(PDEVICE_EXTENSION Vcb,
                  ULONGLONG FileRef,
                  ULONGLONG ParentRef,
                  ULONG Reason,
                  ULONG SourceInfo,
                  PCWSTR Name,
                  USHORT NameLength)
{
    UCHAR Stack[USN_RECORD_V2_HEADER_SIZE + 520 + 8];
    ULONG NameBytes;
    ULONG RecordSize;
    NTSTATUS Status;
    LONGLONG Usn;
    NTFS_USN_JOURNAL_MAX Max;

    if (Vcb == NULL || Vcb->UsnJournalFcb == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;

    NameBytes = (ULONG)NameLength * (ULONG)sizeof(WCHAR);
    if (NameBytes > 510)
        NameBytes = 510; /* clamp to MAX_PATH-ish; longer names truncated */

    Usn = (LONGLONG)Vcb->UsnNextUsn;
    RecordSize = UsnSerialiseRecord(Stack,
                                    sizeof(Stack),
                                    Usn,
                                    FileRef,
                                    ParentRef,
                                    Reason,
                                    SourceInfo,
                                    0,
                                    Name,
                                    (USHORT)NameBytes);
    if (RecordSize == 0)
        return STATUS_BUFFER_TOO_SMALL;

    Status = UsnAppendToJournal(Vcb, Stack, RecordSize);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Best-effort persist of the new NextUsn into $Max.  A failure here
     * leaves the in-memory state ahead of the on-disk $Max; the next
     * emission will catch the drift up.  We don't fail the emission
     * itself since the record is already durable in $J. */
    RtlZeroMemory(&Max, sizeof(Max));
    Max.UsnJournalID = Vcb->UsnJournalId;
    Max.FirstUsn = 0;
    Max.NextUsn = (LONGLONG)Vcb->UsnNextUsn;
    Max.LowestValidUsn = 0;
    Max.MaxUsn = MAXLONGLONG;
    Max.MaximumSize = Vcb->UsnMaxSize;
    Max.AllocationDelta = Vcb->UsnAllocationDelta;
    (void)UsnWriteMaxToRecord(Vcb, Vcb->UsnJournalFcb->MFTIndex, &Max);

    return STATUS_SUCCESS;
}

/**
 * @name NtfsUsnReadRange
 * @implemented
 *
 * Scan $J from @p StartUsn forward, emitting records whose Reason intersects
 * @p ReasonMask.  Prefixes the output with an 8-byte NextUsn (the offset
 * immediately past the last emitted record).  Returns STATUS_SUCCESS with
 * BytesReturned >= sizeof(USN) when done.  Caller allocates @p Buffer.
 */
NTSTATUS
NtfsUsnReadRange(PDEVICE_EXTENSION Vcb,
                 ULONGLONG StartUsn,
                 ULONG ReasonMask,
                 PVOID Buffer,
                 ULONG BufferLength,
                 PULONG BytesReturned)
{
    PFILE_RECORD_HEADER JournalRecord;
    PNTFS_ATTR_CONTEXT JCtx = NULL;
    NTSTATUS Status;
    ULONGLONG Offset;
    ULONGLONG End;
    ULONG BufCur;

    if (BytesReturned != NULL)
        *BytesReturned = 0;

    if (Vcb == NULL || Vcb->UsnJournalFcb == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Buffer == NULL || BufferLength < sizeof(ULONGLONG))
        return STATUS_BUFFER_TOO_SMALL;

    JournalRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (JournalRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Vcb->UsnJournalFcb->MFTIndex, JournalRecord);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = FindAttribute(Vcb, JournalRecord, AttributeData,
                           kUsnStreamJ,
                           (USHORT)(sizeof(kUsnStreamJ) / sizeof(WCHAR) - 1),
                           &JCtx, NULL);
    if (!NT_SUCCESS(Status))
        goto Done;

    End = Vcb->UsnNextUsn;
    Offset = StartUsn;

    /* Output buffer layout: [NextUsn : 8 bytes][records...] */
    BufCur = sizeof(ULONGLONG);
    *(ULONGLONG *)Buffer = Offset;

    while (Offset < End)
    {
        UCHAR Header[USN_RECORD_V2_HEADER_SIZE];
        ULONG RecLen;
        ULONG Reason;

        /* Read just the fixed header first so we know RecordLength. */
        if ((ULONG)ReadAttribute(Vcb, JCtx, Offset, (PCHAR)Header,
                                 sizeof(Header)) < sizeof(Header))
            break;
        RecLen = *(ULONG *)(Header + 0);
        if (RecLen < USN_RECORD_V2_HEADER_SIZE ||
            RecLen > Vcb->UsnMaxSize ||
            Offset + RecLen > End)
            break;

        Reason = *(ULONG *)(Header + 40);

        if (ReasonMask == 0 || (Reason & ReasonMask) != 0)
        {
            if (BufCur + RecLen > BufferLength)
                break;

            /* Copy header... */
            RtlCopyMemory((PUCHAR)Buffer + BufCur, Header, sizeof(Header));
            /* ...then the rest (name + padding) in one read. */
            if (RecLen > sizeof(Header))
            {
                (void)ReadAttribute(Vcb, JCtx,
                                    Offset + sizeof(Header),
                                    (PCHAR)Buffer + BufCur + sizeof(Header),
                                    RecLen - sizeof(Header));
            }
            BufCur += RecLen;
        }

        Offset += RecLen;
    }

    *(ULONGLONG *)Buffer = Offset;
    if (BytesReturned != NULL)
        *BytesReturned = BufCur;

    ReleaseAttributeContext(JCtx);
    Status = STATUS_SUCCESS;

Done:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, JournalRecord);
    return Status;
}

/**
 * @internal
 * @brief  Emit one synthesised USN_RECORD_V2 for @p FileRecord.
 */
static ULONG
UsnEmitEnumRecord(PDEVICE_EXTENSION Vcb,
                  ULONGLONG FileRef,
                  PFILE_RECORD_HEADER FileRecord,
                  PUCHAR Dst,
                  ULONG DstCap)
{
    PFILENAME_ATTRIBUTE Fn;
    PSTANDARD_INFORMATION Si;
    ULONG FileAttrs = 0;
    ULONGLONG ParentRef = 0;
    USHORT NameBytes = 0;
    PCWSTR NamePtr = NULL;

    Fn = GetBestFileNameFromRecord(Vcb, FileRecord);
    if (Fn != NULL)
    {
        ParentRef = Fn->DirectoryFileReferenceNumber;
        NameBytes = (USHORT)(Fn->NameLength * sizeof(WCHAR));
        NamePtr = Fn->Name;
        FileAttrs = Fn->FileAttributes;
    }

    Si = GetStandardInformationFromRecord(Vcb, FileRecord);
    if (Si != NULL)
        FileAttrs = Si->FileAttribute;

    return UsnSerialiseRecord(Dst,
                              DstCap,
                              0,         /* Usn = 0 on enumerate */
                              FileRef,
                              ParentRef,
                              0,         /* Reason */
                              0,         /* SourceInfo */
                              FileAttrs,
                              NamePtr,
                              NameBytes);
}

/**
 * @name NtfsUsnEnumerate
 * @implemented
 *
 * Walk the MFT from @p StartRef forward, emitting synthesised records
 * for every in-use file.  Prefixes the output with an 8-byte "next
 * starting file reference" (the first MFT index NOT consumed).
 */
NTSTATUS
NtfsUsnEnumerate(PDEVICE_EXTENSION Vcb,
                 ULONGLONG StartRef,
                 LONGLONG LowUsn,
                 LONGLONG HighUsn,
                 PVOID Buffer,
                 ULONG BufferLength,
                 PULONG BytesReturned)
{
    PFILE_RECORD_HEADER FileRecord;
    NTSTATUS Status;
    ULONGLONG Mft;
    ULONG BufCur;

    UNREFERENCED_PARAMETER(LowUsn);
    UNREFERENCED_PARAMETER(HighUsn);

    if (BytesReturned != NULL)
        *BytesReturned = 0;

    if (Vcb == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Buffer == NULL || BufferLength < sizeof(ULONGLONG))
        return STATUS_BUFFER_TOO_SMALL;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    BufCur = sizeof(ULONGLONG);
    Mft = StartRef;

    {
        /* Bound the walk by the MFT $DATA size so we don't read past end.
         * mft.c uses this same expression to derive MFT record count. */
        ULONGLONG MftRecCount = 0;
        if (Vcb->MFTContext != NULL && Vcb->NtfsInfo.BytesPerFileRecord != 0)
        {
            MftRecCount = AttributeDataLength(Vcb->MFTContext->pRecord) /
                          Vcb->NtfsInfo.BytesPerFileRecord;
        }

        for (; ; Mft++)
        {
            ULONG RecLen;

            if (MftRecCount != 0 && Mft >= MftRecCount)
                break;

            Status = ReadFileRecord(Vcb, Mft, FileRecord);
            if (!NT_SUCCESS(Status))
                break;

            /* Skip not-in-use slots. */
            if ((FileRecord->Flags & FRH_IN_USE) == 0)
                continue;

            if (BufCur >= BufferLength)
                break;

            /* FileReferenceNumber is the full 64-bit ref: low 48 = MFT
             * index, high 16 = sequence number.  Callers compare their
             * CreateFile-returned ref against what we emit, so we must
             * carry the sequence along. */
            {
                ULONGLONG FullRef = Mft | ((ULONGLONG)FileRecord->SequenceNumber << 48);
                RecLen = UsnEmitEnumRecord(Vcb,
                                           FullRef,
                                           FileRecord,
                                           (PUCHAR)Buffer + BufCur,
                                           BufferLength - BufCur);
            }
            if (RecLen == 0)
                break;

            BufCur += RecLen;
        }
    }

    *(ULONGLONG *)Buffer = Mft;
    if (BytesReturned != NULL)
        *BytesReturned = BufCur;

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
    return STATUS_SUCCESS;
}

/* EOF */
