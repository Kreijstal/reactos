/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystems/ntfs/lfslog.c
 * PURPOSE:          NTFS $LogFile Phase 2 slice 2: live write-ahead-logging
 *                   (WAL) glue.  Kreijstal/reactos#34.
 *
 * SCOPE (this file):
 *
 *   Bridges the standalone emission layer (lfs.c) into the live driver.  It
 *   owns the Vcb-resident logging context, the write-ahead emission hook
 *   NtfsLfsLogMetadataPage (called from mft.c's UpdateFileRecord / INDX
 *   writer before a metadata page hits the disk), the on-disk $Volume DIRTY
 *   bit set/clear handshake, and the clean-dismount flush.
 *
 * RUNTIME GATE (the most important property of this file):
 *
 *   Every LOGGING entry point here is a pass-through NO-OP when
 *   Vcb->LoggingEnabled is FALSE, which is the DEFAULT for every mounted
 *   volume.  With logging off, NtfsLfsLogMetadataPage returns STATUS_SUCCESS
 *   without touching $LogFile and stamps no LSN, so the metadata write path is
 *   byte-for-byte identical to a build without this slice.  This is
 *   deliberate: emission without recovery (slice 3) buys zero durability and
 *   only adds latency, and a wrong on-disk format/ordering would corrupt a
 *   real volume.  Logging is therefore flipped on only by the userspace
 *   harness (and, later, by an explicit opt-in once replay lands).
 *
 *   The $Volume DIRTY bit is the ONE exception, and deliberately so: it is a
 *   plain $VOLUME_INFORMATION flag, not a journal construct.  Windows sets it
 *   whenever NTFS detects damage it cannot resolve, so that the next boot's
 *   autochk runs chkdsk; that is orthogonal to whether a journal exists, and
 *   it is the only self-healing path we have while replay is unimplemented.
 *   NtfsLfsQueryVolumeDirty / NtfsLfsSetVolumeDirty / NtfsMarkVolumeCorrupt
 *   therefore run regardless of LoggingEnabled.
 *
 * WAL INVARIANT:
 *
 *   When logging IS enabled, the redo/undo record describing a metadata
 *   change, the log RCRD page that carries it, and the dirty restart area are
 *   all flushed to $LogFile:$DATA BEFORE the metadata page itself is written.
 *   The caller (UpdateFileRecord) calls NtfsLfsLogMetadataPage first, stamps
 *   the returned LSN into the page's Lsn field, and only then writes the page.
 *
 * ON-DISK FORMAT AUTHORITY: see the banner in lfs.c.  The record layout, LSN
 * encoding and SeqNumberBits derivation are confirmed against ntfs-3g
 * (/usr/include/ntfs-3g/logfile.h) and libfsntfs.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
 * @internal
 * @brief  Read $LogFile:$DATA's byte length (the usable log size).
 */
static NTSTATUS
LfsQueryLogFileSize(PDEVICE_EXTENSION Vcb, PULONGLONG SizeOut)
{
    PFILE_RECORD_HEADER LogFileRecord;
    PNTFS_ATTR_CONTEXT DataCtx = NULL;
    NTSTATUS Status;

    *SizeOut = 0;

    LogFileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (LogFileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, LogFileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = FindAttribute(Vcb, LogFileRecord, AttributeData, L"", 0, &DataCtx, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    *SizeOut = AttributeDataLength(DataCtx->pRecord);
    Status = STATUS_SUCCESS;

Cleanup:
    if (DataCtx != NULL)
        ReleaseAttributeContext(DataCtx);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
    return Status;
}

/**
 * @internal
 * @brief  Build the (heavy) Vcb-resident logging context: read the $LogFile
 *         size, allocate the in-memory mirror, and prime the write cursor.
 *
 * Deliberately lazy - this allocates a multi-MiB NonPagedPool mirror, so it
 * is only run when logging is actually turned on (NtfsLfsSetLoggingEnabled),
 * never on a default mount.  Performs NO disk I/O (NtfsLfsInitWriteContext is
 * purely in-memory), so the real on-disk $LogFile is untouched until an emit.
 */
static NTSTATUS
LfsBuildContext(PDEVICE_EXTENSION Vcb)
{
    NTSTATUS Status;
    ULONGLONG LogFileSize;
    ULONG PageSize;
    ULONG ImageLength;
    PUCHAR Image = NULL;
    PLFS_WRITE_CONTEXT Ctx = NULL;

    if (Vcb->LfsContextValid)
        return STATUS_SUCCESS;

    /* Log page size mirrors lfsparse.c's LfsPageSize: 4K unless the cluster
     * size exceeds it. */
    PageSize = (Vcb->NtfsInfo.BytesPerCluster > 4096)
                   ? Vcb->NtfsInfo.BytesPerCluster
                   : 4096;

    Status = LfsQueryLogFileSize(Vcb, &LogFileSize);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("LFS slice2: cannot read $LogFile size (0x%08lx)\n", Status);
        return Status;
    }

    /* Mirror image: at least two restart pages + two RCRD pages, page-aligned.
     * We cap the in-memory mirror at the real log size so the LSN-to-offset
     * field math matches the on-disk FileSize.  An over-large log would pin a
     * lot of NonPagedPool; logging is opt-in, so this allocation only happens
     * for a deliberately-enabled volume. */
    if (LogFileSize < (ULONGLONG)PageSize * 4)
        return STATUS_FILE_CORRUPT_ERROR;

    ImageLength = (ULONG)(LogFileSize - (LogFileSize % PageSize));

    Ctx = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Ctx), TAG_NTFS);
    if (Ctx == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Image = ExAllocatePoolWithTag(NonPagedPool, ImageLength, TAG_NTFS);
    if (Image == NULL)
    {
        ExFreePoolWithTag(Ctx, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initialise a self-consistent in-memory log image.  This lays down two
     * CLEAN restart copies + empty RCRD pages and primes the write cursor.
     * It is purely in-memory: NtfsLfsInitWriteContext performs no disk I/O,
     * so the real on-disk $LogFile is left untouched until an actual emit
     * (which only happens when LoggingEnabled is set). */
    Status = NtfsLfsInitWriteContext(Vcb, Ctx, Image, ImageLength, PageSize);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Image, TAG_NTFS);
        ExFreePoolWithTag(Ctx, TAG_NTFS);
        return Status;
    }

    Vcb->LfsLogContext = Ctx;
    Vcb->LfsImage = Image;
    Vcb->LfsImageLength = ImageLength;
    Vcb->LfsPageSize = PageSize;
    Vcb->LfsContextValid = TRUE;
    /* Logging stays OFF here: a context being available is not the same as
     * logging being turned on.  The caller flips LoggingEnabled. */
    Vcb->VolumeDirtyOnDisk = FALSE;

    DPRINT("LFS slice2: write context ready (image=%lu bytes, page=%lu, "
           "seqbits=%lu)\n", ImageLength, PageSize, Ctx->SeqNumberBits);
    return STATUS_SUCCESS;
}

/**
 * @name NtfsLfsMountInitWriteContext
 * @implemented
 *
 * Mount-time hook.  Deliberately LIGHTWEIGHT: it does NOT allocate the
 * multi-MiB log mirror and does NOT enable logging - a default mount must
 * stay zero-overhead and behave exactly as a volume with no journal.  It only
 * resets the logging state to a known-disabled baseline; the heavy context is
 * built lazily by NtfsLfsSetLoggingEnabled if and when logging is turned on.
 * The mount-time clean-shutdown landmark (Vcb->LogFileLsn) is already seeded
 * by NtfsLfsCheckCleanShutdown, which runs just before this.
 */
NTSTATUS
NtfsLfsMountInitWriteContext(PDEVICE_EXTENSION Vcb)
{
    Vcb->LoggingEnabled = FALSE;
    Vcb->LfsContextValid = FALSE;
    Vcb->VolumeDirtyOnDisk = FALSE;
    Vcb->LfsLogContext = NULL;
    Vcb->LfsImage = NULL;
    Vcb->LfsImageLength = 0;
    Vcb->LfsPageSize = 0;
    return STATUS_SUCCESS;
}

/**
 * @name NtfsLfsTeardownWriteContext
 * @implemented
 */
VOID
NtfsLfsTeardownWriteContext(PDEVICE_EXTENSION Vcb)
{
    Vcb->LoggingEnabled = FALSE;
    Vcb->LfsContextValid = FALSE;

    if (Vcb->LfsImage != NULL)
    {
        ExFreePoolWithTag(Vcb->LfsImage, TAG_NTFS);
        Vcb->LfsImage = NULL;
    }
    if (Vcb->LfsLogContext != NULL)
    {
        ExFreePoolWithTag(Vcb->LfsLogContext, TAG_NTFS);
        Vcb->LfsLogContext = NULL;
    }
    Vcb->LfsImageLength = 0;
}

/**
 * @name NtfsLfsSetLoggingEnabled
 * @implemented
 */
NTSTATUS
NtfsLfsSetLoggingEnabled(PDEVICE_EXTENSION Vcb, BOOLEAN Enable)
{
    NTSTATUS Status;

    if (!Enable)
    {
        Vcb->LoggingEnabled = FALSE;
        return STATUS_SUCCESS;
    }

    if (!Vcb->LfsContextValid)
    {
        Status = LfsBuildContext(Vcb);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Vcb->LoggingEnabled = TRUE;
    return STATUS_SUCCESS;
}

/**
 * @internal
 * @brief  Write a single page of the in-memory log image out to
 *         $LogFile:$DATA at the same byte offset.  This is the disk side of
 *         the WAL flush.  Only called when logging is enabled.
 */
static NTSTATUS
LfsFlushImageRange(PDEVICE_EXTENSION Vcb, ULONG Offset, ULONG Length)
{
    PFILE_RECORD_HEADER LogFileRecord;
    PNTFS_ATTR_CONTEXT DataCtx = NULL;
    NTSTATUS Status;
    ULONG Written = 0;

    LogFileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (LogFileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, LogFileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = FindAttribute(Vcb, LogFileRecord, AttributeData, L"", 0, &DataCtx, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = WriteAttribute(Vcb, DataCtx, (ULONGLONG)Offset,
                            Vcb->LfsImage + Offset, Length, &Written,
                            LogFileRecord);
    if (NT_SUCCESS(Status) && Written != Length)
        Status = STATUS_UNSUCCESSFUL;

Cleanup:
    if (DataCtx != NULL)
        ReleaseAttributeContext(DataCtx);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
    return Status;
}

/**
 * @internal
 * @brief  Point @p InfoOut at the $VOLUME_INFORMATION value INSIDE @p Record.
 *
 * Deliberately does not go through FindAttribute: PrepareAttributeContext
 * hands back a private COPY of the attribute (mft.c), so a caller that flips a
 * bit in Context->pRecord and then writes the file record back persists the
 * unmodified record and gets STATUS_SUCCESS for it.  FindFirstAttribute walks
 * the record buffer itself and yields pointers into it, which is what an
 * in-place edit needs.
 */
static NTSTATUS
LfsFindVolumeInformation(PDEVICE_EXTENSION Vcb,
                         PFILE_RECORD_HEADER Record,
                         PNTFS_VOLUME_INFORMATION *InfoOut)
{
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    NTSTATUS Status;

    Status = FindFirstAttribute(&Context, Vcb, Record, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == AttributeVolumeInformation)
        {
            if (Attribute->IsNonResident ||
                Attribute->Resident.ValueLength < sizeof(NTFS_VOLUME_INFORMATION))
            {
                FindCloseAttribute(&Context);
                return STATUS_FILE_CORRUPT_ERROR;
            }

            *InfoOut = (PNTFS_VOLUME_INFORMATION)((PUCHAR)Attribute +
                                                  Attribute->Resident.ValueOffset);
            FindCloseAttribute(&Context);
            return STATUS_SUCCESS;
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/**
 * @name NtfsLfsQueryVolumeDirty
 * @implemented
 *
 * Read the on-disk $VOLUME_INFORMATION DIRTY bit into Vcb->VolumeDirtyOnDisk.
 * Called once at mount so the set/clear handshake below starts from the truth
 * on the platter rather than from an assumed-clean default: a volume that was
 * left dirty by a previous boot must not be quietly re-marked (a wasted
 * metadata write) nor believed clean.
 */
NTSTATUS
NtfsLfsQueryVolumeDirty(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER VolumeRecord;
    PNTFS_VOLUME_INFORMATION Info;
    NTSTATUS Status;

    VolumeRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (VolumeRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, NTFS_FILE_VOLUME, VolumeRecord);
    if (NT_SUCCESS(Status))
    {
        Status = LfsFindVolumeInformation(Vcb, VolumeRecord, &Info);
        if (NT_SUCCESS(Status))
        {
            Vcb->VolumeDirtyOnDisk =
                (Info->Flags & NTFS_VOLUME_FLAG_DIRTY) ? TRUE : FALSE;
        }
    }

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, VolumeRecord);
    return Status;
}

/**
 * @name NtfsLfsSetVolumeDirty
 * @implemented
 *
 * Set / clear the on-disk $VOLUME_INFORMATION DIRTY bit.  Idempotent.
 */
NTSTATUS
NtfsLfsSetVolumeDirty(PDEVICE_EXTENSION Vcb, BOOLEAN Dirty)
{
    PFILE_RECORD_HEADER VolumeRecord;
    PNTFS_VOLUME_INFORMATION Info;
    NTSTATUS Status;

    /* Already in the requested state: nothing to do. */
    if ((BOOLEAN)Vcb->VolumeDirtyOnDisk == Dirty)
        return STATUS_SUCCESS;

    VolumeRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (VolumeRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, NTFS_FILE_VOLUME, VolumeRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Must resolve to a pointer INSIDE VolumeRecord - see the helper. */
    Status = LfsFindVolumeInformation(Vcb, VolumeRecord, &Info);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (Dirty)
        Info->Flags |= NTFS_VOLUME_FLAG_DIRTY;
    else
        Info->Flags &= ~NTFS_VOLUME_FLAG_DIRTY;

    /* Persist the $Volume record.  UpdateFileRecord applies the USA itself. */
    Status = UpdateFileRecord(Vcb, NTFS_FILE_VOLUME, VolumeRecord);
    if (NT_SUCCESS(Status))
        Vcb->VolumeDirtyOnDisk = Dirty;

Cleanup:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, VolumeRecord);
    return Status;
}

/**
 * @name NtfsMarkVolumeCorrupt
 * @implemented
 *
 * Record on disk that the driver found damage it could not repair in place, so
 * that the next boot's autochk runs chkdsk over the volume.  This mirrors what
 * Windows NTFS does when it detects an inconsistency: it does not attempt an
 * online rebuild of arbitrary structures, it flags the volume and lets the
 * checker fix it while nothing else holds the disk.
 *
 * It matters more here than it does on Windows.  Windows reaches this state
 * rarely because $LogFile replay repairs a torn update at mount; we have no
 * replay (see the file header), so damage from an unclean shutdown is durable
 * and this is the ONLY path by which it ever gets cleaned up.  Without it the
 * driver skips the broken structure on every boot, forever, and the volume
 * silently decays.
 *
 * Called from the detection sites, which run deep inside the index walk with
 * directory resources held; it must therefore stay cheap and must not fail the
 * caller's request.  It writes at most one $Volume record per mount (the bit is
 * sticky until chkdsk clears it), and does nothing at all on a read-only mount.
 */
VOID
NtfsMarkVolumeCorrupt(PDEVICE_EXTENSION Vcb)
{
    NTSTATUS Status;

    /* Sticky: the bit stays set until chkdsk's clean stamp clears it, so one
     * write per mount is enough no matter how many broken entries we meet. */
    if (Vcb->VolumeDirtyOnDisk)
        return;

    /* A read-only mount must not write, and a volume already gated read-only
     * for a dirty log is heading for the checker anyway. */
    if (Vcb->Flags & VCB_VOLUME_READ_ONLY)
        return;

    Status = NtfsLfsSetVolumeDirty(Vcb, TRUE);
    if (NT_SUCCESS(Status))
    {
        DPRINT1("Volume marked dirty: chkdsk will run on the next boot.\n");
    }
    else
    {
        DPRINT1("Could not mark the volume dirty (0x%08lx); the damage will "
                "persist unrepaired.\n", Status);
    }
}

/**
 * @name NtfsLfsLogMetadataPage
 * @implemented
 *
 * Write-ahead emission hook.  See header doc for the full contract.  The
 * critical property: when Vcb->LoggingEnabled is FALSE this returns
 * STATUS_SUCCESS immediately, leaving *PageLsn untouched, so the caller's
 * metadata write proceeds exactly as before.
 */
NTSTATUS
NtfsLfsLogMetadataPage(PDEVICE_EXTENSION Vcb,
                       USHORT RedoOperation,
                       USHORT UndoOperation,
                       USHORT TargetAttribute,
                       ULONGLONG TargetVcn,
                       const VOID *RedoData,
                       USHORT RedoLength,
                       const VOID *UndoData,
                       USHORT UndoLength,
                       PULONGLONG PageLsn)
{
    NTSTATUS Status;
    LFS_EMIT_PARAMS Params;
    PLFS_WRITE_CONTEXT Ctx;
    ULONGLONG Lsn = 0;
    ULONG RecordPageBase;

    /* THE GATE.  Off by default -> instant pass-through, no side effects. */
    if (!Vcb->LoggingEnabled)
        return STATUS_SUCCESS;

    if (!Vcb->LfsContextValid || Vcb->LfsLogContext == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;

    Ctx = Vcb->LfsLogContext;

    /* Set the on-disk DIRTY bit on the first log write after a clean mount,
     * BEFORE the redo record is durable, so a crash between here and the log
     * flush still leaves the volume marked dirty for chkdsk. */
    if (!Vcb->VolumeDirtyOnDisk)
    {
        Status = NtfsLfsSetVolumeDirty(Vcb, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    RtlZeroMemory(&Params, sizeof(Params));
    Params.RedoOperation = RedoOperation;
    Params.UndoOperation = UndoOperation;
    Params.RedoData = (const UCHAR *)RedoData;
    Params.RedoLength = RedoLength;
    Params.UndoData = (const UCHAR *)UndoData;
    Params.UndoLength = UndoLength;
    Params.TargetAttribute = TargetAttribute;
    Params.TargetVcn = TargetVcn;
    Params.ClientPreviousLsn = Ctx->LastLsn;

    /* Emit into the in-memory image: assigns the LSN, USA-stamps the RCRD
     * page, and flips both restart copies to the dirty landmark. */
    Status = NtfsLfsEmitRecord(Ctx, &Params, &Lsn);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Flush the log to disk BEFORE the caller writes the metadata page.
     * Two ranges must be durable: the restart pages (offset 0, two pages) and
     * the RCRD page that now carries this record. */
    RecordPageBase = (NtfsLfsLsnToOffset(Ctx->SeqNumberBits, Lsn) /
                      Ctx->PageSize) * Ctx->PageSize;

    Status = LfsFlushImageRange(Vcb, RecordPageBase, Ctx->PageSize);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = LfsFlushImageRange(Vcb, 0, Ctx->PageSize * 2);
    if (!NT_SUCCESS(Status))
        return Status;

    if (PageLsn != NULL)
        *PageLsn = Lsn;

    return STATUS_SUCCESS;
}

/**
 * @name NtfsLfsDismountFlush
 * @implemented
 *
 * Clean-dismount flush: CLEAN restart landmark + clear the on-disk DIRTY bit.
 */
NTSTATUS
NtfsLfsDismountFlush(PDEVICE_EXTENSION Vcb)
{
    NTSTATUS Status;

    if (!Vcb->LoggingEnabled || !Vcb->LfsContextValid)
        return STATUS_SUCCESS;

    /* Stamp both restart copies CLEAN (CurrentLsn = last, LastLsnDataLen = 0)
     * and flush them. */
    Status = NtfsLfsWriteRestart(Vcb->LfsLogContext, Vcb, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = LfsFlushImageRange(Vcb, 0, Vcb->LfsPageSize * 2);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Clear the on-disk dirty bit: the volume is now consistent. */
    return NtfsLfsSetVolumeDirty(Vcb, FALSE);
}

/* EOF */
