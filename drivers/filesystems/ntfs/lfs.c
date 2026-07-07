/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystems/ntfs/lfs.c
 * PURPOSE:          NTFS $LogFile Phase 2 slice 1: write-side LFS (Log File
 *                   Service) emission layer.  Kreijstal/reactos#34.
 *
 * SCOPE (this file):
 *
 *   This is the WRITE side of the journal: it formats a single log record
 *   (header + redo + undo payload), assigns it an LSN, places it into an
 *   in-memory image of $LogFile:$DATA inside an RCRD page (applying the
 *   per-sector USA fixup exactly the way the on-disk format requires), and
 *   flips the restart area between the CLEAN and dirty landmarks.
 *
 *   It is a STANDALONE, unit-testable module: every function here operates
 *   purely on a caller-supplied LFS_WRITE_CONTEXT over a caller-owned byte
 *   buffer.  Nothing in this file performs disk I/O, and nothing here is
 *   wired into the live metadata write path (NtfsWriteDiskCached).  The
 *   live wiring (write-ahead ordering, $Volume::DIRTY handshake, recovery
 *   replay) is the next slice and is DELIBERATELY NOT implemented here -
 *   a half-working journal that races real metadata writes is worse than
 *   none (see issue scope).
 *
 * ON-DISK FORMAT AUTHORITY:
 *
 *   The byte layout this file emits is identical to what the in-tree
 *   formatter lays down (sdk/lib/fslib/ntfslib/mkntfs.c build_rstr_page)
 *   and what the Phase-0 parser (lfsparse.c) consumes:
 *
 *     RSTR page:  hdr @ 0x00, RestartOffset USHORT @ 0x10, USA @ 0x28,
 *                 NTFS_LFS_RESTART_AREA @ 0x40.
 *     RCRD page:  hdr @ 0x00, NextRecordOffset USHORT @ 0x22, USA @ 0x28,
 *                 first NTFS_LFS_RECORD_HEADER @ 0x30.
 *
 *   USA writer  == AddFixupArray (mft.c): increments USN, saves each
 *                  sector-tail USHORT into the USA, stamps the USN sentinel.
 *   USA reader  == FixupUpdateSequenceArray (mft.c): the parser side.
 *
 *   LSN encoding == Microsoft LFS: the high SeqNumberBits hold a sequence
 *   number; the low (64 - SeqNumberBits) bits hold the log-file byte offset
 *   divided by 8.  Layout authority: libfsntfs (forensic read library,
 *   safe to reference) + Microsoft public LFS documentation.  Linux ntfs3's
 *   fslog.c is GPL-2.0-only (NO code copy permitted); used only as a field
 *   cross-check.
 *
 * FORMAT FACTS CONFIRMED AGAINST ntfs-3g (/usr/include/ntfs-3g/logfile.h,
 * a documentation-only on-disk-layout header) - resolving the slice-1
 * unconfirmed assumptions:
 *
 *   (1) SeqNumberBits is NOT a constant.  It is derived from the $LogFile
 *       byte size as  67 - bit_length(FileSize)  (RESTART_AREA::seq_number_bits
 *       doc; libntfs-3g/logfile.c ntfs_check_restart_area).  See
 *       NtfsLfsSeqNumberBits.  Slice 1's hardcoded 0x2D only happened to be
 *       correct for the ~2-4 MiB default log.
 *
 *   (2) RedoOffset / UndoOffset in a record are measured from the START OF
 *       THE LOG RECORD (LOG_RECORD byte 0 == this_lsn), not from client-data
 *       start.  ntfs-3g's LOG_RECORD struct begins at the record start: a
 *       0x30-byte generic header, then the attribute-log fields 0x30..0x50,
 *       then lcn_list[0] / redo / undo data at 0x50 when lcns_to_follow == 0.
 *       Our NTFS_LFS_RECORD_HEADER is byte-identical (sizeof 0x50).  Slice 1
 *       wrote these client-data-relative; corrected to record-relative here.
 *
 *   (3) Multi-page continuation (LcnsToFollow > 0 array + a record larger
 *       than one page):
 *         - LcnsToFollow (USHORT @ 0x3E) counts LCN entries; when > 0 an
 *           array of LcnsToFollow * 8 bytes (leLCN) sits at record offset
 *           0x50, BEFORE the redo/undo payload (so RedoOffset/UndoOffset are
 *           pushed past the array).
 *         - A record whose ClientDataLength runs past the page's record area
 *           continues onto the next RCRD page.  The producer sets
 *           NTFS_LFS_RECORD_FLAG_MULTI_PAGE (0x0001) and the page header's
 *           PageCount / PagePosition (1-based index within the multi-page
 *           set); the continued tail resumes at the next page's
 *           LogPageDataOffset (right after RCRD header + USA, i.e. 0x40 for a
 *           4K/512 page).  RECORD_PAGE_HEADER carries NextRecordOffset and
 *           LastEndLsn so the reader can stitch tail onto head.
 *       This module EMITS single-page records only: it always sets
 *       LcnsToFollow = 0 and clears the MULTI_PAGE flag, and refuses
 *       (STATUS_LOG_FILE_FULL) any record that would not fit one page.  The
 *       multi-page emitter is a later slice; the format is documented here so
 *       it can be added without re-deriving the layout.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* On-page byte offsets shared with mkntfs build_rstr_page and the parser. */
#define LFS_RSTR_USA_OFFSET        0x28
#define LFS_RSTR_AREA_OFFSET       0x40
#define LFS_RSTR_CLIENT_OFFSET     0x70
#define LFS_RCRD_USA_OFFSET        0x28
/* First record slot.  Must clear the USA, which for a 4K page / 512B sector
 * spans 0x28..0x39 (UsaCount = 9 words).  0x40 is 8-byte aligned and past
 * the USA; it also matches the parser's NextRecordOffset >= 0x28 contract. */
#define LFS_RCRD_FIRST_RECORD      0x40
#define LFS_RCRD_NEXTREC_FIELD     0x22

/* FUNCTIONS ****************************************************************/

/**
 * @name NtfsLfsSeqNumberBits
 * @implemented
 *
 * Derive the restart area's SeqNumberBits from the $LogFile byte size.
 *
 * Microsoft / ntfs-3g formula (libntfs-3g/logfile.c ntfs_check_restart_area,
 * confirmed against /usr/include/ntfs-3g/logfile.h RESTART_AREA::seq_number_bits
 * doc comment "67 - the number of bits required to store the logfile size in
 * bytes"):
 *
 *     SeqNumberBits = 67 - bit_length(FileSize)
 *
 * where bit_length(x) is the number of significant bits in x
 * (floor(log2(x)) + 1).  The argument is the FULL byte size of $LogFile:$DATA,
 * NOT FileSize >> 3.  For the common ~2 MiB / 4 MiB default log this yields
 * 45 (0x2D), which is why the slice-1 hardcode happened to match those sizes;
 * it is wrong for every other log size, so we compute it here.
 */
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

/**
 * @name NtfsLfsMakeLsn
 * @implemented
 *
 * Compose a 64-bit LSN from a sequence number and a log-file byte offset.
 * Microsoft LFS encoding: the low (64 - SeqNumberBits) bits carry the byte
 * offset right-shifted by 3 (LSNs are 8-byte granular); the high
 * SeqNumberBits carry the sequence number.
 */
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

/**
 * @name NtfsLfsLsnToOffset
 * @implemented
 *
 * Recover the log-file byte offset from an LSN (inverse of MakeLsn for the
 * offset field).  Returns the byte offset; the sequence number is dropped.
 */
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

/**
 * @name NtfsLfsBuildRestartPage
 * @implemented
 *
 * Lay down one RSTR restart page into @p Page.  Byte-identical to mkntfs's
 * build_rstr_page so a driver- and a formatter-produced page agree, then
 * apply the USA via AddFixupArray so NtfsLfsParseRestartPage round-trips it.
 */
NTSTATUS
NtfsLfsBuildRestartPage(PDEVICE_EXTENSION Vcb,
                        PUCHAR Page,
                        ULONG PageSize,
                        ULONGLONG FileSize,
                        ULONGLONG CurrentLsn,
                        ULONG LastLsnDataLength,
                        BOOLEAN Clean)
{
    PNTFS_RECORD_HEADER Hdr;
    NTFS_LFS_RESTART_AREA Area;
    USHORT UsaCount;
    ULONG SectorSize;

    if (Vcb == NULL || Page == NULL)
        return STATUS_INVALID_PARAMETER;

    SectorSize = Vcb->NtfsInfo.BytesPerSector;
    if (SectorSize == 0 || PageSize < SectorSize ||
        (PageSize % SectorSize) != 0)
        return STATUS_INVALID_PARAMETER;

    /* UsaCount = sectors-per-page + 1 (one sentinel word + one saved word
     * per sector).  The USA must fit between LFS_RSTR_USA_OFFSET and the
     * restart area at 0x40. */
    UsaCount = (USHORT)(PageSize / SectorSize + 1);
    if ((ULONG)LFS_RSTR_USA_OFFSET + (ULONG)UsaCount * sizeof(USHORT) >
        LFS_RSTR_AREA_OFFSET)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Page, PageSize);

    Hdr = (PNTFS_RECORD_HEADER)Page;
    Hdr->Type = NRH_RSTR_TYPE;
    Hdr->UsaOffset = LFS_RSTR_USA_OFFSET;
    Hdr->UsaCount = UsaCount;
    Hdr->Lsn = 0;

    /* RestartOffset @ 0x10 (the field the parser reads) and the libfsntfs
     * mirror fields up to 0x1A.  AddFixupArray will set usa[0]; pre-seed it
     * to 0 so the post-increment lands the sentinel at 1, matching mkntfs. */
    *(USHORT *)(Page + 0x10) = LFS_RSTR_AREA_OFFSET;
    *(USHORT *)(Page + 0x12) = (USHORT)PageSize;          /* SystemPageSize */
    *(USHORT *)(Page + 0x14) = (USHORT)PageSize;          /* LogPageSize    */
    *(USHORT *)(Page + 0x16) = LFS_RSTR_AREA_OFFSET;      /* RestartOffset  */
    *(USHORT *)(Page + NTFS_LFS_RSTR_MAJOR_VERSION_OFFSET) =
        NTFS_LFS_SUPPORTED_MAJOR_VERSION;                 /* MajorVersion   */
    *(USHORT *)(Page + NTFS_LFS_RSTR_MINOR_VERSION_OFFSET) = 0; /* MinorVersion */
    *(USHORT *)(Page + LFS_RSTR_USA_OFFSET) = 0;          /* USN seed       */

    RtlZeroMemory(&Area, sizeof(Area));
    Area.CurrentLsn          = CurrentLsn;
    Area.LogClients          = 1;
    Area.ClientFreeList      = 0;
    Area.ClientInUseList     = 0xFFFF;
    Area.Flags               = Clean ? NTFS_LFS_RESTART_FLAG_CLEAN : 0;
    Area.SeqNumberBits       = NtfsLfsSeqNumberBits(FileSize);
    Area.RestartAreaLength   = (USHORT)sizeof(NTFS_LFS_RESTART_AREA);
    Area.ClientArrayOffset   = (USHORT)sizeof(NTFS_LFS_RESTART_AREA);
    Area.FileSize            = FileSize;
    Area.LastLsnDataLength   = LastLsnDataLength;
    Area.RecordHeaderLength  = NTFS_LFS_RECORD_HEADER_LENGTH;
    Area.LogPageDataOffset   = LFS_RSTR_AREA_OFFSET;
    Area.RestartOpenLogCount = 1;
    Area.Reserved            = 0;
    RtlCopyMemory(Page + LFS_RSTR_AREA_OFFSET, &Area, sizeof(Area));

    /* Stamp the per-sector USA sentinels.  AddFixupArray increments the USN
     * (0 -> 1) and writes each sector's trailing USHORT, saving the
     * originals into the USA so FixupUpdateSequenceArray reverses it. */
    return AddFixupArray(Vcb, Hdr);
}

/**
 * @internal
 * @brief  Lay down one empty RCRD page header (no records yet) into @p Page,
 *         then USA-stamp it.  WriteOffset bookkeeping in the context tracks
 *         the first free record slot at LFS_RCRD_FIRST_RECORD.
 */
static NTSTATUS
LfsBuildEmptyRcrdPage(PDEVICE_EXTENSION Vcb, PUCHAR Page, ULONG PageSize)
{
    PNTFS_RECORD_HEADER Hdr;
    USHORT UsaCount;
    ULONG SectorSize = Vcb->NtfsInfo.BytesPerSector;

    if (SectorSize == 0 || PageSize < SectorSize)
        return STATUS_INVALID_PARAMETER;

    UsaCount = (USHORT)(PageSize / SectorSize + 1);
    if ((ULONG)LFS_RCRD_USA_OFFSET + (ULONG)UsaCount * sizeof(USHORT) >
        LFS_RCRD_FIRST_RECORD)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Page, PageSize);

    Hdr = (PNTFS_RECORD_HEADER)Page;
    Hdr->Type = NRH_RCRD_TYPE;
    Hdr->UsaOffset = LFS_RCRD_USA_OFFSET;
    Hdr->UsaCount = UsaCount;
    Hdr->Lsn = 0;

    /* NextRecordOffset @ 0x22: first record sits at 0x30. */
    *(USHORT *)(Page + LFS_RCRD_NEXTREC_FIELD) = LFS_RCRD_FIRST_RECORD;
    *(USHORT *)(Page + LFS_RCRD_USA_OFFSET) = 0;          /* USN seed */

    return AddFixupArray(Vcb, Hdr);
}

/**
 * @name NtfsLfsInitWriteContext
 * @implemented
 *
 * Initialise an emission context: lay down both CLEAN RSTR copies, prime
 * the RCRD region with empty record pages, and set the write cursor to the
 * first record slot of the first RCRD page.
 */
NTSTATUS
NtfsLfsInitWriteContext(PDEVICE_EXTENSION Vcb,
                        PLFS_WRITE_CONTEXT Ctx,
                        PUCHAR Image,
                        ULONG ImageLength,
                        ULONG PageSize)
{
    NTSTATUS Status;
    ULONG SectorSize;
    ULONG Off;

    if (Vcb == NULL || Ctx == NULL || Image == NULL)
        return STATUS_INVALID_PARAMETER;

    SectorSize = Vcb->NtfsInfo.BytesPerSector;
    if (SectorSize == 0 || PageSize < SectorSize ||
        (PageSize % SectorSize) != 0)
        return STATUS_INVALID_PARAMETER;

    /* Need at least two restart copies + two RCRD pages. */
    if (ImageLength < PageSize * 4 || (ImageLength % PageSize) != 0)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->Image = Image;
    Ctx->ImageLength = ImageLength;
    Ctx->PageSize = PageSize;
    Ctx->SectorSize = SectorSize;
    Ctx->FirstRcrdOffset = PageSize * 2;
    Ctx->SeqNumberBits = NtfsLfsSeqNumberBits((ULONGLONG)ImageLength);
    Ctx->CurrentSeqNumber = 1;
    Ctx->WriteOffset = PageSize * 2 + LFS_RCRD_FIRST_RECORD;
    Ctx->LastLsn = 0;
    Ctx->ClientId = 0;

    /* Two byte-identical CLEAN restart copies (CurrentLsn = 0, clean). */
    Status = NtfsLfsBuildRestartPage(Vcb, Image, PageSize,
                                     (ULONGLONG)ImageLength, 0, 0, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlCopyMemory(Image + PageSize, Image, PageSize);

    /* Empty RCRD pages for the rest of the image. */
    for (Off = PageSize * 2; Off + PageSize <= ImageLength; Off += PageSize)
    {
        Status = LfsBuildEmptyRcrdPage(Vcb, Image + Off, PageSize);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

/**
 * @internal
 * @brief  Compute the on-disk byte geometry of a record from its redo/undo
 *         payload lengths.  Shared by Format and Emit so they never drift.
 *
 *   @p RedoAbs / @p UndoAbs : absolute byte offsets (from record start) of
 *                             the redo and undo payloads.
 *   @p ClientDataLength     : bytes after the 0x30 generic header.
 *   @p TotalLength          : full 8-byte-aligned record length.
 */
static VOID
LfsRecordGeometry(USHORT RedoLength,
                  USHORT UndoLength,
                  PULONG RedoAbs,
                  PULONG UndoAbs,
                  PULONG ClientDataLength,
                  PULONG TotalLength)
{
    ULONG R, U;

    /* Client data = NTFS sub-header (0x20) | redo | undo, redo first, both
     * 8-byte aligned.  Client data starts at the 0x30 generic-header end. */
    R = ALIGN_UP_BY(NTFS_LFS_RECORD_HEADER_LENGTH + NTFS_LFS_ATTR_HEADER_LENGTH, 8);
    U = ALIGN_UP_BY(R + RedoLength, 8);

    *RedoAbs = R;
    *UndoAbs = U;
    *ClientDataLength = (U - NTFS_LFS_RECORD_HEADER_LENGTH) + UndoLength;
    *TotalLength = ALIGN_UP_BY(NTFS_LFS_RECORD_HEADER_LENGTH + *ClientDataLength, 8);
}

/**
 * @name NtfsLfsFormatRecord
 * @implemented
 *
 * Serialise one log record (NTFS_LFS_RECORD_HEADER + redo payload + undo
 * payload) into @p Buffer.  No USA, no page placement - pure codec so a
 * unit test can compare bytes without a page container.
 */
NTSTATUS
NtfsLfsFormatRecord(PLFS_WRITE_CONTEXT Ctx,
                    const LFS_EMIT_PARAMS *Params,
                    ULONGLONG ThisLsn,
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

    LfsRecordGeometry(Params->RedoLength, Params->UndoLength,
                      &RedoAbs, &UndoAbs, &ClientDataLength, &TotalLength);

    if (BufferLength < TotalLength)
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(Buffer, TotalLength);

    Rec = (PNTFS_LFS_RECORD_HEADER)Buffer;
    Rec->ThisLsn = ThisLsn;
    Rec->ClientPreviousLsn = Params->ClientPreviousLsn;
    Rec->ClientUndoNextLsn = Params->ClientUndoNextLsn;
    Rec->ClientDataLength = ClientDataLength;
    Rec->ClientId = Ctx->ClientId;
    Rec->Alignment = 0;
    Rec->RecordType = NTFS_LFS_RECTYPE_NORMAL;
    Rec->TransactionId = Params->TransactionId;
    Rec->Flags = 0;
    Rec->RedoOperation = Params->RedoOperation;
    Rec->UndoOperation = Params->UndoOperation;
    /* RedoOffset / UndoOffset are measured from the START OF THE LOG RECORD
     * (the NTFS_LFS_RECORD_HEADER == ntfs-3g LOG_RECORD start at byte 0), NOT
     * from the client-data start.  Confirmed against ntfs-3g
     * /usr/include/ntfs-3g/logfile.h struct LOG_RECORD: this_lsn is at offset
     * 0, the fixed header runs to 0x50 (lcn_list[0]), and redo_offset/
     * undo_offset point at the redo/undo payload which, with lcns_to_follow==0,
     * begins at 0x50.  Slice 1 wrote these client-data-relative (abs - 0x30);
     * that is corrected here to be record-relative. */
    Rec->RedoOffset = (USHORT)RedoAbs;
    Rec->RedoLength = Params->RedoLength;
    Rec->UndoOffset = (USHORT)UndoAbs;
    Rec->UndoLength = Params->UndoLength;
    Rec->TargetAttribute = Params->TargetAttribute;
    Rec->LcnsToFollow = 0;
    Rec->RecordOffset = Params->RecordOffset;
    Rec->AttributeOffset = Params->AttributeOffset;
    Rec->MftClusterIndex = 0;
    Rec->Alignment2 = 0;
    Rec->TargetVcn = Params->TargetVcn;

    if (Params->RedoLength != 0 && Params->RedoData != NULL)
        RtlCopyMemory(Buffer + RedoAbs, Params->RedoData, Params->RedoLength);
    if (Params->UndoLength != 0 && Params->UndoData != NULL)
        RtlCopyMemory(Buffer + UndoAbs, Params->UndoData, Params->UndoLength);

    if (RecordLengthOut != NULL)
        *RecordLengthOut = TotalLength;

    return STATUS_SUCCESS;
}

/**
 * @name NtfsLfsEmitRecord
 * @implemented
 *
 * Emit one log record into the in-memory image at the write cursor.  Steps:
 *
 *   1. Compute the record's total length; if it would cross the current
 *      RCRD page boundary, advance the cursor to the next page (slice 1
 *      never splits a record across pages - the MULTI_PAGE flag and span
 *      logic is a later slice).
 *   2. Assign ThisLsn from the cursor's byte offset via NtfsLfsMakeLsn.
 *   3. Reverse the page USA (so we write over real payload bytes), format
 *      the record in place at the cursor, re-stamp the page USA.
 *   4. Advance the cursor (8-byte aligned), update LastLsn, and flip the
 *      restart copies to the dirty landmark (CurrentLsn = new LSN,
 *      LastLsnDataLength = the record's ClientDataLength).
 */
NTSTATUS
NtfsLfsEmitRecord(PLFS_WRITE_CONTEXT Ctx,
                  const LFS_EMIT_PARAMS *Params,
                  PULONGLONG LsnOut)
{
    NTSTATUS Status;
    ULONG RedoAbs, UndoAbs, ClientDataLength, TotalLength;
    ULONG PageBase, PageEnd, RecordEnd;
    PUCHAR Page;
    ULONGLONG ThisLsn;

    if (Ctx == NULL || Ctx->Image == NULL || Params == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Use the same geometry helper Format uses so the range check before we
     * touch the page is exact. */
    LfsRecordGeometry(Params->RedoLength, Params->UndoLength,
                      &RedoAbs, &UndoAbs, &ClientDataLength, &TotalLength);

    /* A single record must fit within one page's record area. */
    if (TotalLength > Ctx->PageSize - LFS_RCRD_FIRST_RECORD)
        return STATUS_LOG_FILE_FULL;

    /* Which page is the cursor in?  If the record won't fit before the
     * page end, jump to the first record slot of the next page. */
    PageBase = (Ctx->WriteOffset / Ctx->PageSize) * Ctx->PageSize;
    PageEnd = PageBase + Ctx->PageSize;

    if (Ctx->WriteOffset + TotalLength > PageEnd)
    {
        PageBase = PageEnd;
        /* Wrap the RCRD region back to the start if we ran off the end,
         * bumping the sequence number (the log is a circular buffer). */
        if (PageBase + Ctx->PageSize > Ctx->ImageLength)
        {
            PageBase = Ctx->FirstRcrdOffset;
            Ctx->CurrentSeqNumber++;
        }
        Ctx->WriteOffset = PageBase + LFS_RCRD_FIRST_RECORD;
        PageEnd = PageBase + Ctx->PageSize;
    }

    Page = Ctx->Image + PageBase;
    RecordEnd = Ctx->WriteOffset + TotalLength;
    if (RecordEnd > PageEnd || RecordEnd > Ctx->ImageLength)
        return STATUS_LOG_FILE_FULL;

    /* LSN from the record's byte offset in the image. */
    ThisLsn = NtfsLfsMakeLsn(Ctx->SeqNumberBits, Ctx->CurrentSeqNumber,
                             Ctx->WriteOffset);

    /* FixupUpdateSequenceArray / AddFixupArray want a Vcb only for the USA
     * stride (BytesPerSector).  Drive them from a thin local shim built out
     * of the context so emission stays Vcb-free and unit-testable. */
    {
        DEVICE_EXTENSION Shim;
        PNTFS_RECORD_HEADER Hdr = (PNTFS_RECORD_HEADER)Page;

        RtlZeroMemory(&Shim, sizeof(Shim));
        Shim.NtfsInfo.BytesPerSector = Ctx->SectorSize;

        /* Reverse the page USA so each sector's trailing USHORT holds its
         * real payload byte again (not the sentinel) before we write the
         * record over it.  Empty pages laid down by init are USA-stamped. */
        Status = FixupUpdateSequenceArray(&Shim, Hdr);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = NtfsLfsFormatRecord(Ctx, Params, ThisLsn,
                                     Ctx->Image + Ctx->WriteOffset,
                                     TotalLength, NULL);
        if (!NT_SUCCESS(Status))
        {
            /* Re-stamp so the page isn't left with a reversed USA. */
            AddFixupArray(&Shim, Hdr);
            return Status;
        }

        /* The 0x22 field is the FIRST-record offset (the parser reads it as
         * such); the empty-page init already stamped it to 0x40, so leave it
         * alone here.  The page header LSN tracks the last record written. */
        Hdr->Lsn = ThisLsn;

        Status = AddFixupArray(&Shim, Hdr);
        if (!NT_SUCCESS(Status))
            return Status;

        Ctx->WriteOffset = RecordEnd;
        Ctx->LastLsn = ThisLsn;

        /* Flip both restart copies to the dirty landmark: a crash now must
         * force a read-only remount until replay/chkdsk. */
        Status = NtfsLfsBuildRestartPage(&Shim, Ctx->Image, Ctx->PageSize,
                                         (ULONGLONG)Ctx->ImageLength,
                                         ThisLsn, ClientDataLength, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;
        RtlCopyMemory(Ctx->Image + Ctx->PageSize, Ctx->Image, Ctx->PageSize);
    }

    if (LsnOut != NULL)
        *LsnOut = ThisLsn;

    return STATUS_SUCCESS;
}

/**
 * @name NtfsLfsWriteRestart
 * @implemented
 *
 * Rewrite both restart copies.  @p Clean selects the CLEAN landmark
 * (dismount): CurrentLsn = Ctx->LastLsn, LastLsnDataLength = 0.  When not
 * clean, the dirty landmark (LastLsnDataLength != 0) is preserved by the
 * emit path; this entry point is primarily the clean-dismount flush.
 */
NTSTATUS
NtfsLfsWriteRestart(PLFS_WRITE_CONTEXT Ctx,
                    PDEVICE_EXTENSION Vcb,
                    BOOLEAN Clean)
{
    NTSTATUS Status;

    if (Ctx == NULL || Ctx->Image == NULL || Vcb == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsLfsBuildRestartPage(Vcb, Ctx->Image, Ctx->PageSize,
                                     (ULONGLONG)Ctx->ImageLength,
                                     Ctx->LastLsn, 0, Clean);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlCopyMemory(Ctx->Image + Ctx->PageSize, Ctx->Image, Ctx->PageSize);
    return STATUS_SUCCESS;
}

/* EOF */
