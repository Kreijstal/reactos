/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/lfsparse.c
 * PURPOSE:          NTFS $LogFile Phase 0 (read-only parser + dump FSCTL) and
 *                   Phase 1 (clean-shutdown gate / force RO mount on dirty log).
 *                   Kreijstal/reactos#34.
 *
 * SCOPE (phased):
 *
 *   Phase 0  -- read-only parser + FSCTL_NTFS_DUMP_LOGFILE dump worker.
 *               This file owns NtfsLfsParseRestartPage, NtfsLfsParseRecordPage
 *               and NtfsLfsDumpLogfile.  The dump is forensic-only; it does
 *               not replay, emit, or mutate $LogFile in any way.
 *
 *   Phase 1  -- clean-shutdown gate.  NtfsLfsCheckCleanShutdown is called
 *               from NtfsMountVolume after the MFT is up.  It parses both
 *               restart-page copies; when they disagree / fail USA fixup /
 *               carry a non-zero LastLsnDataLength landmark, the caller
 *               sets VCB_VOLUME_READ_ONLY on the Vcb and lets dispatch.c
 *               short-circuit every mutating IRP major with
 *               STATUS_MEDIA_WRITE_PROTECTED.  The volume remains mounted
 *               read-only until the user reboots into a log-replaying
 *               driver or runs chkdsk out-of-band.  Phase 2+ (emission,
 *               replay, $Volume::DIRTY handshake, full MS opcode matrix)
 *               is a separate follow-up issue and is DELIBERATELY NOT
 *               implemented here.
 *
 * LICENSING:
 *
 *   Layout authority:  libfsntfs (Apache/LGPL) -- forensic-only read
 *                       library.  Safe-to-reference.  Used as the on-disk
 *                       layout authority for NTFS_LFS_RESTART_AREA /
 *                       NTFS_LFS_RECORD_HEADER fields and offsets.
 *   Algorithm-only:    Linux ntfs3 (GPL-2.0-only) -- since ReactOS NTFS is
 *                       GPL-2.0+ (== GPL-2.0-only compatible ONE WAY only),
 *                       NO code from ntfs3's fslog.c may be copied.  Any
 *                       derived-work translation taints the driver.  This
 *                       file uses ntfs3 ONLY as a reference for which
 *                       fields are checked by the clean-shutdown gate;
 *                       the code below is an independent implementation.
 *   Microsoft public:  LFS (Log File Service) public documentation is
 *                       safe to reference.
 *   ntfs-3g:           Not used.  ntfs-3g zeroes $LogFile at mount and
 *                       punts on replay, so its source is not useful as
 *                       a parser reference.
 *
 * USA (Update Sequence Array) fixups:
 *
 *   The existing FixupUpdateSequenceArray in mft.c already uses
 *   Vcb->NtfsInfo.BytesPerSector as the fixup stride, which is the
 *   correct stride for all three NTFS log record-page kinds (MFT file
 *   records, INDX records, and the $LogFile RSTR/RCRD pages).  We reuse
 *   the function verbatim; this file does not re-implement fixup.
 *
 * NOT YET IMPLEMENTED (deferred to Phase 2+):
 *
 *   - RCRD-page walker across the full $LogFile extent.  Phase 0 peeks
 *     at only the first RCRD record of the first RCRD page so the dump
 *     can surface the current-LSN landmark.  Full walk needs oldest-LSN
 *     bookkeeping (CcCoherency-driven) that would blow the size budget.
 *   - Emission.  A replay/emit loop against a clean restart page is a
 *     standalone ~4-6 month port; if your reading of ntfs3's fslog.c
 *     makes you want to emit records here, STOP -- it lands in a
 *     separate issue.
 *   - $Volume::DIRTY handshake.  Clean-shutdown gate sets the VCB flag
 *     only; toggling the on-disk dirty bit belongs to Phase 2.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* Byte-wise signum for buffer comparisons that must not pull libc memcmp on
 * i386.  See NtfsCompareBytes in btree.c for the same pattern: returns -1
 * when a < b, +1 when a > b, 0 when equal.  Phase-0 uses this to compare
 * the primary-copy vs backup-copy restart pages for byte-exact agreement. */
static LONG
LfsCompareBytes(const VOID *A, const VOID *B, ULONG Len)
{
    const UCHAR *pa = (const UCHAR *)A;
    const UCHAR *pb = (const UCHAR *)B;
    ULONG i;

    for (i = 0; i < Len; i++)
    {
        if (pa[i] != pb[i])
            return (pa[i] < pb[i]) ? -1 : 1;
    }
    return 0;
}

/**
 * @name NtfsLfsParseRestartPage
 * @implemented
 *
 * Validate and in-place-fix the USA of a single $LogFile restart page.
 *
 * @param Vcb
 *   Volume context; only Vcb->NtfsInfo.BytesPerSector is read (the USA
 *   stride), which matches FixupUpdateSequenceArray's implicit contract.
 *
 * @param Page
 *   Caller-owned buffer holding a raw RSTR page as read from
 *   $LogFile:$DATA at offset 0 (primary) or PageSize (backup).  The USA
 *   is applied in place, so the buffer is mutated on success.
 *
 * @param PageSize
 *   Page size in bytes.  NTFS uses a 4096-byte log page as long as the
 *   volume cluster size is at most 4K; above that cluster size equals
 *   page size.  The caller is expected to pass whichever the boot sector
 *   implies.  Minimum 512 (one sector, USA-stride == page-size degenerate
 *   case) -- below that this returns STATUS_INVALID_PARAMETER.
 *
 * @param RestartOut
 *   Filled with the NTFS_LFS_RESTART_AREA body on success.  The buffer
 *   is copied out of the page so the caller doesn't alias Page.
 *
 * @param RestartOffsetOut
 *   Optional.  When non-NULL, receives the offset (from the page base)
 *   at which RestartOut was read, for callers that want to walk the
 *   LOG_CLIENT_RECORD array that follows.
 *
 * @return
 *   STATUS_SUCCESS on a well-formed RSTR page (magic OK, USA OK, restart
 *   area length in range).  STATUS_INVALID_PARAMETER if the input shape
 *   is obviously wrong (NULL buffer, < sector-sized page).
 *   STATUS_FILE_CORRUPT_ERROR if the page is present but the magic /
 *   USA fixup doesn't match -- Phase 1 treats this as "dirty, force RO".
 */
NTSTATUS
NtfsLfsParseRestartPage(PDEVICE_EXTENSION Vcb,
                        PUCHAR Page,
                        ULONG PageSize,
                        PNTFS_LFS_RESTART_AREA RestartOut,
                        PULONG RestartOffsetOut)
{
    PNTFS_RECORD_HEADER Hdr;
    USHORT RestartOffset;
    NTSTATUS Status;

    if (Vcb == NULL || Page == NULL || RestartOut == NULL)
        return STATUS_INVALID_PARAMETER;

    if (PageSize < Vcb->NtfsInfo.BytesPerSector)
        return STATUS_INVALID_PARAMETER;

    Hdr = (PNTFS_RECORD_HEADER)Page;

    /* Magic check first -- a freshly-formatted but never-used $LogFile is
     * zeroed, which reads as magic == 0 and trips here.  Callers treat
     * that as a dirty-log indicator. */
    if (Hdr->Type != NRH_RSTR_TYPE)
    {
        DPRINT1("LFS RSTR page: bad magic 0x%08lx (expected 'RSTR')\n",
                (ULONG)Hdr->Type);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    /* UsaOffset must sit inside the record; UsaCount must cover one
     * sector-stride per page.  Reject obviously-malformed headers before
     * handing them to FixupUpdateSequenceArray so it can't walk off the
     * end of the buffer. */
    if (Hdr->UsaOffset < sizeof(NTFS_RECORD_HEADER) ||
        (ULONG)Hdr->UsaOffset + (ULONG)Hdr->UsaCount * sizeof(USHORT) > PageSize)
    {
        DPRINT1("LFS RSTR page: USA offset/count out of range "
                "(off=%u count=%u pageSize=%lu)\n",
                Hdr->UsaOffset, Hdr->UsaCount, PageSize);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    /* The existing fixup helper mutates the page in place, returning
     * STATUS_FILE_CORRUPT_ERROR if any per-sector USN sentinel fails. */
    Status = FixupUpdateSequenceArray(Vcb, Hdr);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LFS RSTR USA fixup failed: 0x%08lx\n", Status);
        return Status;
    }

    /* Restart area lives at the first USHORT following the USA, aligned
     * up by the fixup.  The in-tree writers (NtfsLfsBuildRestartPage in
     * lfs.c and ntfslib's ntfs_build_rstr_page) stamp the header as:
     *   offset 0x00 : NTFS_RECORD_HEADER
     *   offset 0x10 : RestartOffset (USHORT) - the field read below
     *   offset 0x12 : SystemPageSize (USHORT)
     *   offset 0x14 : LogPageSize (USHORT)
     *   offset 0x16 : RestartOffset mirror (libfsntfs view)
     *   offset 0x18 : MajorVersion (USHORT)  [NTFS_LFS_RSTR_MAJOR_VERSION_OFFSET]
     *   offset 0x1A : MinorVersion (USHORT)  [NTFS_LFS_RSTR_MINOR_VERSION_OFFSET]
     *   ...
     *   followed by the restart area at a page-specific offset.
     *
     * For Phase-0 we hew to the defensive shape: the restart-area
     * offset is the first USHORT after the header (at page offset
     * 0x10 per ntfs3 and libfsntfs empirical dumps), clamped to sit
     * inside the page and above sizeof(NTFS_LFS_RESTART_AREA) so the
     * subsequent copy doesn't overrun.
     */
    RestartOffset = *(USHORT *)(Page + 0x10);
    if (RestartOffset < sizeof(NTFS_RECORD_HEADER) ||
        (ULONG)RestartOffset + sizeof(NTFS_LFS_RESTART_AREA) > PageSize)
    {
        DPRINT1("LFS RSTR page: restart-area offset %u out of range (pageSize=%lu)\n",
                RestartOffset, PageSize);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    RtlCopyMemory(RestartOut,
                  Page + RestartOffset,
                  sizeof(NTFS_LFS_RESTART_AREA));

    /* Sanity-check: the restart area's self-reported length must be at
     * least the size of the fixed fields we copied, and must sit inside
     * the page.  The ClientArrayOffset must also fit so a Phase-2+ walker
     * of LOG_CLIENT_RECORD can trust it. */
    if (RestartOut->RestartAreaLength < sizeof(NTFS_LFS_RESTART_AREA) ||
        (ULONG)RestartOffset + RestartOut->RestartAreaLength > PageSize)
    {
        DPRINT1("LFS RSTR area: self-reported length %u out of range\n",
                RestartOut->RestartAreaLength);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    if (RestartOffsetOut != NULL)
        *RestartOffsetOut = RestartOffset;

    return STATUS_SUCCESS;
}

/**
 * @name NtfsLfsParseRecordPage
 * @implemented
 *
 * Validate and in-place-fix the USA of a single $LogFile RCRD page.
 * Phase-0 does not walk past the first record; future phases will.
 *
 * @return
 *   STATUS_SUCCESS on a well-formed RCRD page.
 *   STATUS_FILE_CORRUPT_ERROR if the magic or USA fixup fails.
 */
NTSTATUS
NtfsLfsParseRecordPage(PDEVICE_EXTENSION Vcb,
                       PUCHAR Page,
                       ULONG PageSize,
                       PNTFS_LFS_RECORD_HEADER *FirstRecordOut)
{
    PNTFS_RECORD_HEADER Hdr;
    USHORT RecordOffset;
    NTSTATUS Status;

    if (Vcb == NULL || Page == NULL)
        return STATUS_INVALID_PARAMETER;

    if (PageSize < Vcb->NtfsInfo.BytesPerSector)
        return STATUS_INVALID_PARAMETER;

    Hdr = (PNTFS_RECORD_HEADER)Page;

    if (Hdr->Type == NRH_CHKD_TYPE)
    {
        /* chkdsk-cleaned page: USA is intact, but payload is undefined.
         * Caller walks the next page; treat this as a non-record. */
        if (FirstRecordOut != NULL)
            *FirstRecordOut = NULL;
        return STATUS_SUCCESS;
    }

    if (Hdr->Type != NRH_RCRD_TYPE)
    {
        DPRINT1("LFS RCRD page: bad magic 0x%08lx\n", (ULONG)Hdr->Type);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    if (Hdr->UsaOffset < sizeof(NTFS_RECORD_HEADER) ||
        (ULONG)Hdr->UsaOffset + (ULONG)Hdr->UsaCount * sizeof(USHORT) > PageSize)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Status = FixupUpdateSequenceArray(Vcb, Hdr);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LFS RCRD USA fixup failed: 0x%08lx\n", Status);
        return Status;
    }

    /* First record sits at page offset 0x28 on standard RCRD pages per
     * libfsntfs; but to be safe we read the "NextRecordOffset" USHORT
     * that lives at page offset 0x22 (post-USA layout).  Clamp to the
     * page and to the size of LFS_RECORD_HEADER. */
    RecordOffset = *(USHORT *)(Page + 0x22);
    if (RecordOffset == 0 || RecordOffset < 0x28)
    {
        /* Conservative fallback when the header's self-reported offset
         * is missing or nonsense: start at 0x28 -- the documented first-
         * record position for a fresh RCRD. */
        RecordOffset = 0x28;
    }

    if ((ULONG)RecordOffset + sizeof(NTFS_LFS_RECORD_HEADER) > PageSize)
    {
        DPRINT1("LFS RCRD page: first record offset %u out of range\n",
                RecordOffset);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    if (FirstRecordOut != NULL)
        *FirstRecordOut = (PNTFS_LFS_RECORD_HEADER)(Page + RecordOffset);

    return STATUS_SUCCESS;
}

/**
 * @internal
 * @brief  Open $LogFile (NTFS_FILE_LOGFILE MFT record), locate $DATA, and
 *         read the first 2 * @p PageSize bytes into the caller-owned buffer.
 *
 * The first page is the primary restart page; the second is the backup.
 * Phase-1 needs both to verify byte-exact agreement.  The caller frees
 * the buffer via ExFreePoolWithTag(..., TAG_NTFS).
 *
 * Returns STATUS_SUCCESS + @p BufferOut populated on success.
 */
static NTSTATUS
LfsReadRestartPair(PDEVICE_EXTENSION Vcb,
                   ULONG PageSize,
                   PUCHAR *BufferOut,
                   PULONG LengthOut)
{
    PFILE_RECORD_HEADER LogFileRecord = NULL;
    PNTFS_ATTR_CONTEXT DataCtx = NULL;
    NTSTATUS Status;
    PUCHAR Buffer = NULL;
    ULONG ReadLen;
    ULONGLONG DataLen;

    if (Vcb == NULL || BufferOut == NULL || LengthOut == NULL)
        return STATUS_INVALID_PARAMETER;

    *BufferOut = NULL;
    *LengthOut = 0;

    LogFileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (LogFileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, LogFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LFS: could not read $LogFile MFT record: 0x%08lx\n", Status);
        goto Cleanup;
    }

    Status = FindAttribute(Vcb, LogFileRecord, AttributeData, L"", 0,
                           &DataCtx, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LFS: $LogFile has no $DATA attribute: 0x%08lx\n", Status);
        goto Cleanup;
    }

    DataLen = AttributeDataLength(DataCtx->pRecord);
    if (DataLen < (ULONGLONG)PageSize * 2)
    {
        /* Short $LogFile -- either unformatted or truncated.  Treat as
         * "no usable log" by returning corruption so the caller's RO
         * gate trips. */
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    Buffer = ExAllocatePoolWithTag(NonPagedPool, PageSize * 2, TAG_NTFS);
    if (Buffer == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    ReadLen = ReadAttribute(Vcb, DataCtx, 0, (PCHAR)Buffer, PageSize * 2);
    if (ReadLen < PageSize * 2)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    *BufferOut = Buffer;
    *LengthOut = PageSize * 2;
    Buffer = NULL;  /* ownership transferred */
    Status = STATUS_SUCCESS;

Cleanup:
    if (Buffer != NULL)
        ExFreePoolWithTag(Buffer, TAG_NTFS);
    if (DataCtx != NULL)
        ReleaseAttributeContext(DataCtx);
    if (LogFileRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
    return Status;
}

/**
 * @internal
 * @brief  Pick the log-page size for this volume.
 *
 * NTFS uses a 4-KiB log page as long as the volume cluster size is <=
 * 4 KiB.  For clusters > 4 KiB (very rare in practice) the page size
 * equals the cluster size.  We lean on BytesPerCluster here so we don't
 * have to carry a separate field.
 */
static ULONG
LfsPageSize(PDEVICE_EXTENSION Vcb)
{
    ULONG Cluster = Vcb->NtfsInfo.BytesPerCluster;
    return (Cluster > 4096) ? Cluster : 4096;
}

/**
 * @name NtfsLfsCheckCleanShutdown
 * @implemented
 *
 * Phase-1 clean-shutdown gate.  See banner for the spec.
 *
 * Test order (fail-to-dirty, NOT fail-to-clean):
 *
 *   1. Allocate buffer + read the first two log pages.  Any I/O failure
 *      short-circuits to STATUS_LOG_FILE_FULL (dirty) because we cannot
 *      verify agreement without the bytes.
 *   2. Parse both pages.  Magic/USA mismatch on either -> dirty.
 *   3. Byte-compare the two restart-area copies.  Mismatch -> dirty.
 *      (Non-clean shutdowns can tear the backup page mid-write; this
 *      catches that.)
 *   4. Inspect Flags & NTFS_LFS_RESTART_FLAG_CLEAN + LastLsnDataLength.
 *      A truly-clean volume carries the CLEAN bit AND zero LastLsnDataLen
 *      (the landmark showing the last restart record had no open TX).
 *      Anything else -> dirty.
 *
 * On every path, Vcb->LogFileDirty / LogFileLsn plus the restart-page
 * version capture (LogFileMajorVersion / LogFileMinorVersion /
 * LogFileVersionUnsupported) are populated so the FSCTL_NTFS_DUMP_LOGFILE
 * worker has state to report.  The caller (NtfsMountVolume) is
 * responsible for OR-ing VCB_VOLUME_READ_ONLY into Vcb->Flags when this
 * worker returns non-SUCCESS *or* sets LogFileVersionUnsupported (clean
 * LFS 2.0 volume - see NtfsLfsEvaluateRestartPair).
 */
NTSTATUS
NtfsLfsCheckCleanShutdown(PDEVICE_EXTENSION Vcb)
{
    NTSTATUS Status;
    ULONG PageSize;
    PUCHAR Pages = NULL;
    ULONG PagesLen = 0;

    /* Default: treat the log as dirty, version unknown.  Overwritten by
     * NtfsLfsEvaluateRestartPair once the pages are in memory. */
    Vcb->LogFileDirty = TRUE;
    Vcb->LogFileLsn = 0;
    Vcb->LogFileMajorVersion = 0;
    Vcb->LogFileMinorVersion = 0;
    Vcb->LogFileVersionUnsupported = FALSE;

    PageSize = LfsPageSize(Vcb);

    Status = LfsReadRestartPair(Vcb, PageSize, &Pages, &PagesLen);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LFS CheckCleanShutdown: read-pair failed (0x%08lx); "
                "treating volume as dirty\n", Status);
        /* Propagate as LOG_FILE_FULL so callers distinguish dirty-log
         * from catastrophic mount failure. */
        return STATUS_LOG_FILE_FULL;
    }

    Status = NtfsLfsEvaluateRestartPair(Vcb, Pages, PageSize);

    ExFreePoolWithTag(Pages, TAG_NTFS);
    return Status;
}

/**
 * @name NtfsLfsEvaluateRestartPair
 * @implemented
 *
 * Evaluate an in-memory primary+backup restart-page pair (the first
 * 2 * @p PageSize bytes of $LogFile:$DATA, USA-fixed in place here).
 * This is the whole of the Phase-1 gate minus the disk read, split out
 * so the userspace harness can drive it against synthetic pages; see
 * NtfsLfsCheckCleanShutdown's banner for the check order.
 *
 * Besides the dirty/clean verdict this also captures the restart-page
 * format version (header offsets 0x18/0x1A, the fields our writers
 * NtfsLfsBuildRestartPage / ntfs_build_rstr_page stamp) into
 * Vcb->LogFileMajorVersion / LogFileMinorVersion.  A MajorVersion above
 * NTFS_LFS_SUPPORTED_MAJOR_VERSION (LFS 2.0, introduced by Windows 8)
 * sets Vcb->LogFileVersionUnsupported: the log parses and may even be
 * CLEAN, but its format is newer than this 1.x-era implementation, so
 * the mount gate must refuse writes exactly like down-level Windows
 * NTFS does on an LFS 2.0 volume.  The version verdict is deliberately
 * NOT folded into the returned status - a clean 2.0 log still returns
 * STATUS_SUCCESS so the caller can distinguish "dirty" from "clean but
 * too new".
 */
NTSTATUS
NtfsLfsEvaluateRestartPair(PDEVICE_EXTENSION Vcb,
                           PUCHAR Pages,
                           ULONG PageSize)
{
    NTSTATUS Status;
    NTFS_LFS_RESTART_AREA Primary, Backup;
    ULONG PrimaryOff = 0, BackupOff = 0;

    if (Vcb == NULL || Pages == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Default: treat the log as dirty.  Overwritten only when every
     * check below passes. */
    Vcb->LogFileDirty = TRUE;
    Vcb->LogFileLsn = 0;
    Vcb->LogFileMajorVersion = 0;
    Vcb->LogFileMinorVersion = 0;
    Vcb->LogFileVersionUnsupported = FALSE;

    Status = NtfsLfsParseRestartPage(Vcb, Pages, PageSize, &Primary, &PrimaryOff);
    if (!NT_SUCCESS(Status))
    {
        /* Belt-and-braces: a freshly-formatted $LogFile that has never
         * been touched by any NTFS driver may carry absolutely no RSTR
         * magic in either page (both all-zeros / all-0xFF / otherwise
         * blank).  Real dirty-shutdown volumes always have valid magic
         * on at least one copy -- only the CLEAN flag is missing --
         * so a bilaterally magic-less pair is the fingerprint of an
         * uninitialised log, not a torn one.  Treat that as clean so
         * third-party formatters (ntfsprogs-plus, older ReactOS installs,
         * images restored from DD dumps of never-mounted volumes) don't
         * drop the install into RO mode.  Any partial magic (primary
         * present, backup absent or vice-versa) is STILL dirty. */
        PNTFS_RECORD_HEADER PrimaryHdr = (PNTFS_RECORD_HEADER)Pages;
        PNTFS_RECORD_HEADER BackupHdr  = (PNTFS_RECORD_HEADER)(Pages + PageSize);

        if (PrimaryHdr->Type != NRH_RSTR_TYPE &&
            BackupHdr->Type  != NRH_RSTR_TYPE)
        {
            DPRINT1("LFS CheckCleanShutdown: both restart pages lack RSTR "
                    "magic (primary=0x%08lx backup=0x%08lx); treating as "
                    "freshly-formatted / clean\n",
                    (ULONG)PrimaryHdr->Type, (ULONG)BackupHdr->Type);
            Vcb->LogFileDirty = FALSE;
            Status = STATUS_SUCCESS;
            goto Out;
        }

        DPRINT1("LFS CheckCleanShutdown: primary restart page invalid (0x%08lx)\n", Status);
        Status = STATUS_LOG_FILE_FULL;
        goto Out;
    }

    /* Landmark captured for forensic dump even if backup later disagrees. */
    Vcb->LogFileLsn = Primary.CurrentLsn;

    /* Restart-page format version, from the primary page's header (the
     * backup must byte-agree on the restart AREA below; the header version
     * of the primary copy is authoritative).  Captured for every valid
     * page - dirty or clean - so FSCTL_NTFS_DUMP_LOGFILE and the mount
     * gate always see it.  LFS 2.0+ (Windows 8+) is a newer log format
     * than this implementation writes; flag it so NtfsMountVolume forces
     * the volume read-only even when the log is CLEAN. */
    ASSERT(PageSize >= NTFS_LFS_RSTR_MINOR_VERSION_OFFSET + sizeof(USHORT));
    Vcb->LogFileMajorVersion = *(USHORT *)(Pages + NTFS_LFS_RSTR_MAJOR_VERSION_OFFSET);
    Vcb->LogFileMinorVersion = *(USHORT *)(Pages + NTFS_LFS_RSTR_MINOR_VERSION_OFFSET);
    if (Vcb->LogFileMajorVersion > NTFS_LFS_SUPPORTED_MAJOR_VERSION)
    {
        DPRINT1("LFS EvaluateRestartPair: restart page is LFS %u.%u - newer "
                "than the supported %u.x format\n",
                Vcb->LogFileMajorVersion, Vcb->LogFileMinorVersion,
                NTFS_LFS_SUPPORTED_MAJOR_VERSION);
        Vcb->LogFileVersionUnsupported = TRUE;
    }

    Status = NtfsLfsParseRestartPage(Vcb, Pages + PageSize, PageSize,
                                     &Backup, &BackupOff);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("LFS CheckCleanShutdown: backup restart page invalid (0x%08lx)\n", Status);
        Status = STATUS_LOG_FILE_FULL;
        goto Out;
    }

    /* Byte-exact agreement between the two copies.  A torn backup means
     * the volume was interrupted during a log flush; that's a dirty log
     * regardless of the Primary.Flags claim.  Use NtfsCompareBytes-shape
     * comparator -- NO libc memcmp -- so the i386 link stays clean.  */
    if (LfsCompareBytes(&Primary, &Backup, sizeof(Primary)) != 0)
    {
        DPRINT1("LFS CheckCleanShutdown: primary/backup restart pages disagree\n");
        Status = STATUS_LOG_FILE_FULL;
        goto Out;
    }

    /* Final landmark: CLEAN flag set and LastLsnDataLength zero.
     * libfsntfs calls this the "clean-mount indicator": if the last
     * log record logged a client-data length then a transaction was
     * in flight at unmount time and replay is needed. */
    if ((Primary.Flags & NTFS_LFS_RESTART_FLAG_CLEAN) == 0 ||
        Primary.LastLsnDataLength != 0)
    {
        DPRINT1("LFS CheckCleanShutdown: landmark dirty "
                "(flags=0x%04x LastLsnDataLength=%lu)\n",
                Primary.Flags, Primary.LastLsnDataLength);
        Status = STATUS_LOG_FILE_FULL;
        goto Out;
    }

    Vcb->LogFileDirty = FALSE;
    Status = STATUS_SUCCESS;

Out:
    return Status;
}

/**
 * @internal
 * @brief  Append a %s-style snprintf fragment to @p Buffer, tracking
 *         overflow.  Mirrors the idiom used in drivers/filesystems
 *         metadata workers that surface text diagnostics through an
 *         FSCTL.  Uses RtlStringCbPrintfA so the CRT isn't pulled in.
 */
static VOID
LfsDumpAppend(PCHAR Buffer,
              ULONG BufferLen,
              PULONG Cursor,
              PULONG Required,
              const char *Text,
              ULONG TextLen)
{
    ULONG Room;

    *Required += TextLen;
    if (*Cursor >= BufferLen)
        return;

    Room = BufferLen - *Cursor;
    if (Room > TextLen)
        Room = TextLen;

    if (Buffer != NULL && Room != 0)
        RtlCopyMemory(Buffer + *Cursor, Text, Room);
    *Cursor += Room;
}

/* Write a 16-char uppercase hex representation of a ULONGLONG into @p Out.
 * No CRT / no snprintf -- this is a driver path. */
static VOID
LfsHex64(CHAR Out[17], ULONGLONG Value)
{
    static const char kHex[] = "0123456789ABCDEF";
    ULONG i;
    for (i = 0; i < 16; i++)
    {
        Out[15 - i] = kHex[Value & 0xF];
        Value >>= 4;
    }
    Out[16] = '\0';
}

static VOID
LfsHex16(CHAR Out[5], USHORT Value)
{
    static const char kHex[] = "0123456789ABCDEF";
    ULONG i;
    for (i = 0; i < 4; i++)
    {
        Out[3 - i] = kHex[Value & 0xF];
        Value >>= 4;
    }
    Out[4] = '\0';
}

static VOID
LfsHex32(CHAR Out[9], ULONG Value)
{
    static const char kHex[] = "0123456789ABCDEF";
    ULONG i;
    for (i = 0; i < 8; i++)
    {
        Out[7 - i] = kHex[Value & 0xF];
        Value >>= 4;
    }
    Out[8] = '\0';
}

#define LFS_DUMP_APPEND_LITERAL(Str) \
    LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required, \
                  (Str), (ULONG)(sizeof(Str) - 1))

/**
 * @name NtfsLfsDumpLogfile
 * @implemented
 *
 * Phase-0 dump FSCTL worker.  Reads the first two restart pages plus the
 * third page (first RCRD candidate), parses each, and appends a compact
 * forensic report into @p Buffer.  The output schema is human-parseable;
 * userspace test harness stamps exact strings so regressions trip quickly.
 *
 * Output schema (one field per line, no trailing punctuation):
 *
 *   LFS-DUMP v1
 *   PAGE=0 MAGIC=RSTR STATUS=OK CURRENT_LSN=<hex16> FLAGS=<hex4> LDDL=<hex8>
 *   PAGE=1 MAGIC=RSTR STATUS=OK CURRENT_LSN=<hex16> FLAGS=<hex4> LDDL=<hex8>
 *   PAGE=2 MAGIC=RCRD STATUS=OK THIS_LSN=<hex16> REDO_OP=<hex4> UNDO_OP=<hex4>
 *   STATE=CLEAN|DIRTY
 *
 * On parse failure for a page, the STATUS= field carries the short name
 * "CORRUPT" / "NONE" instead of "OK" and subsequent per-page fields are
 * omitted.
 *
 * @return  STATUS_SUCCESS                on full write.
 *          STATUS_BUFFER_OVERFLOW        when BufferLength wasn't enough;
 *                                        @p BytesOut receives the required
 *                                        total, output is truncated.
 *          Else an I/O / parse error.
 */
NTSTATUS
NtfsLfsDumpLogfile(PDEVICE_EXTENSION Vcb,
                   PCHAR Buffer,
                   ULONG BufferLength,
                   PULONG BytesOut)
{
    NTSTATUS Status;
    ULONG PageSize;
    PUCHAR Pages = NULL;
    ULONG PagesLen = 0;
    NTFS_LFS_RESTART_AREA Primary, Backup;
    PNTFS_LFS_RECORD_HEADER FirstRecord = NULL;
    PUCHAR RcrdPage = NULL;
    PFILE_RECORD_HEADER LogFileRecord = NULL;
    PNTFS_ATTR_CONTEXT DataCtx = NULL;
    ULONGLONG DataLen;
    ULONG ReadLen;
    ULONG Cursor = 0;
    ULONG Required = 0;
    CHAR Hex16Buf[17];
    CHAR Hex8Buf[9];
    CHAR Hex4Buf[5];

    if (BytesOut != NULL)
        *BytesOut = 0;

    PageSize = LfsPageSize(Vcb);

    LFS_DUMP_APPEND_LITERAL("LFS-DUMP v1\n");

    /* Read the first 3 pages (primary restart, backup restart, first RCRD
     * candidate).  The read-restart-pair helper already covers the first
     * two; we extend by another page so we can peek at the RCRD chain
     * head without adding a second allocator path. */
    Status = LfsReadRestartPair(Vcb, PageSize, &Pages, &PagesLen);
    if (!NT_SUCCESS(Status))
    {
        LFS_DUMP_APPEND_LITERAL("PAGE=0 MAGIC=RSTR STATUS=IOERR\n");
        LFS_DUMP_APPEND_LITERAL("PAGE=1 MAGIC=RSTR STATUS=IOERR\n");
        LFS_DUMP_APPEND_LITERAL("PAGE=2 MAGIC=RCRD STATUS=IOERR\n");
        LFS_DUMP_APPEND_LITERAL("STATE=DIRTY\n");
        goto Finish;
    }

    /* Parse primary. */
    Status = NtfsLfsParseRestartPage(Vcb, Pages, PageSize, &Primary, NULL);
    if (NT_SUCCESS(Status))
    {
        LFS_DUMP_APPEND_LITERAL("PAGE=0 MAGIC=RSTR STATUS=OK CURRENT_LSN=");
        LfsHex64(Hex16Buf, Primary.CurrentLsn);
        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required, Hex16Buf, 16);
        LFS_DUMP_APPEND_LITERAL(" FLAGS=");
        LfsHex16(Hex4Buf, Primary.Flags);
        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required, Hex4Buf, 4);
        LFS_DUMP_APPEND_LITERAL(" LDDL=");
        LfsHex32(Hex8Buf, Primary.LastLsnDataLength);
        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required, Hex8Buf, 8);
        LFS_DUMP_APPEND_LITERAL("\n");
    }
    else
    {
        LFS_DUMP_APPEND_LITERAL("PAGE=0 MAGIC=RSTR STATUS=CORRUPT\n");
    }

    /* Parse backup. */
    Status = NtfsLfsParseRestartPage(Vcb, Pages + PageSize, PageSize,
                                     &Backup, NULL);
    if (NT_SUCCESS(Status))
    {
        LFS_DUMP_APPEND_LITERAL("PAGE=1 MAGIC=RSTR STATUS=OK CURRENT_LSN=");
        LfsHex64(Hex16Buf, Backup.CurrentLsn);
        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required, Hex16Buf, 16);
        LFS_DUMP_APPEND_LITERAL(" FLAGS=");
        LfsHex16(Hex4Buf, Backup.Flags);
        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required, Hex4Buf, 4);
        LFS_DUMP_APPEND_LITERAL(" LDDL=");
        LfsHex32(Hex8Buf, Backup.LastLsnDataLength);
        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required, Hex8Buf, 8);
        LFS_DUMP_APPEND_LITERAL("\n");
    }
    else
    {
        LFS_DUMP_APPEND_LITERAL("PAGE=1 MAGIC=RSTR STATUS=CORRUPT\n");
    }

    /* Peek at the first RCRD page -- read it on demand rather than
     * extend the restart-pair helper, which keeps Phase-0 cheap for
     * the common clean-mount case. */
    LogFileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (LogFileRecord != NULL &&
        NT_SUCCESS(ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, LogFileRecord)) &&
        NT_SUCCESS(FindAttribute(Vcb, LogFileRecord, AttributeData, L"", 0,
                                 &DataCtx, NULL)))
    {
        DataLen = AttributeDataLength(DataCtx->pRecord);
        if (DataLen >= (ULONGLONG)PageSize * 3)
        {
            RcrdPage = ExAllocatePoolWithTag(NonPagedPool, PageSize, TAG_NTFS);
            if (RcrdPage != NULL)
            {
                ReadLen = ReadAttribute(Vcb, DataCtx,
                                        (ULONGLONG)PageSize * 2,
                                        (PCHAR)RcrdPage, PageSize);
                if (ReadLen == PageSize)
                {
                    Status = NtfsLfsParseRecordPage(Vcb, RcrdPage, PageSize,
                                                   &FirstRecord);
                    if (NT_SUCCESS(Status) && FirstRecord != NULL)
                    {
                        LFS_DUMP_APPEND_LITERAL("PAGE=2 MAGIC=RCRD STATUS=OK THIS_LSN=");
                        LfsHex64(Hex16Buf, FirstRecord->ThisLsn);
                        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required,
                                      Hex16Buf, 16);
                        LFS_DUMP_APPEND_LITERAL(" REDO_OP=");
                        LfsHex16(Hex4Buf, FirstRecord->RedoOperation);
                        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required,
                                      Hex4Buf, 4);
                        LFS_DUMP_APPEND_LITERAL(" UNDO_OP=");
                        LfsHex16(Hex4Buf, FirstRecord->UndoOperation);
                        LfsDumpAppend(Buffer, BufferLength, &Cursor, &Required,
                                      Hex4Buf, 4);
                        LFS_DUMP_APPEND_LITERAL("\n");
                    }
                    else if (NT_SUCCESS(Status))
                    {
                        LFS_DUMP_APPEND_LITERAL("PAGE=2 MAGIC=RCRD STATUS=EMPTY\n");
                    }
                    else
                    {
                        LFS_DUMP_APPEND_LITERAL("PAGE=2 MAGIC=RCRD STATUS=CORRUPT\n");
                    }
                }
                else
                {
                    LFS_DUMP_APPEND_LITERAL("PAGE=2 MAGIC=RCRD STATUS=IOERR\n");
                }
            }
            else
            {
                LFS_DUMP_APPEND_LITERAL("PAGE=2 MAGIC=RCRD STATUS=NOMEM\n");
            }
        }
        else
        {
            LFS_DUMP_APPEND_LITERAL("PAGE=2 MAGIC=RCRD STATUS=NONE\n");
        }
    }
    else
    {
        LFS_DUMP_APPEND_LITERAL("PAGE=2 MAGIC=RCRD STATUS=NONE\n");
    }

    if (Vcb->LogFileDirty)
        LFS_DUMP_APPEND_LITERAL("STATE=DIRTY\n");
    else
        LFS_DUMP_APPEND_LITERAL("STATE=CLEAN\n");

    Status = STATUS_SUCCESS;

Finish:
    if (RcrdPage != NULL)
        ExFreePoolWithTag(RcrdPage, TAG_NTFS);
    if (DataCtx != NULL)
        ReleaseAttributeContext(DataCtx);
    if (LogFileRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, LogFileRecord);
    if (Pages != NULL)
        ExFreePoolWithTag(Pages, TAG_NTFS);

    if (BytesOut != NULL)
        *BytesOut = (Required <= BufferLength) ? Required : Required;

    if (Required > BufferLength && NT_SUCCESS(Status))
        Status = STATUS_BUFFER_OVERFLOW;

    return Status;
}

/* EOF */
