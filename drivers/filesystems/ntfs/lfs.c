/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     LFS restart/record emission (3857e1c02ad, 3203ef56e06)
 *
 * Single-page codec only. Runtime logging remains disabled by default;
 * recovery and NTFS transaction management are separate work.
 */
#include "lfswrite.h"

#define NDEBUG
#include <debug.h>

/* On-page byte offsets shared with mkntfs build_rstr_page and the parser. */
#define LFS_RSTR_USA_OFFSET        0x28
#define LFS_RSTR_AREA_OFFSET       0x40
#define LFS_RSTR_CLIENT_OFFSET     0x70
#define LFS_RCRD_USA_OFFSET        0x28
/* First record slot.  Must clear the USA, which for a 4K page / 512B sector
 * spans 0x28..0x39 (UsaCount = 9 words).  0x40 is 8-byte aligned and past
 * the USA. */
#define LFS_RCRD_FIRST_RECORD      0x40
#define LFS_RCRD_NEXTREC_FIELD     0x18

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
    Hdr->Type = NTFS_LOG_RESTART_MAGIC;
    Hdr->UsaOffset = LFS_RSTR_USA_OFFSET;
    Hdr->UsaCount = UsaCount;
    Hdr->Lsn = 0;

    ((PNTFS_LOG_RESTART_HEADER)Page)->SystemPageSize = PageSize;
    ((PNTFS_LOG_RESTART_HEADER)Page)->LogPageSize = PageSize;
    ((PNTFS_LOG_RESTART_HEADER)Page)->RestartOffset = LFS_RSTR_AREA_OFFSET;
    ((PNTFS_LOG_RESTART_HEADER)Page)->MajorVersion = 1;
    ((PNTFS_LOG_RESTART_HEADER)Page)->MinorVersion = 1;
    *(USHORT *)(Page + LFS_RSTR_USA_OFFSET) = 0;          /* USN seed       */

    RtlZeroMemory(&Area, sizeof(Area));
    Area.CurrentLsn          = CurrentLsn;
    Area.LogClients          = 1;
    /* NTFS occupies client slot 0 and is the only client that ever registers,
     * so the in-use chain is that one slot and the free chain is empty.  This
     * used to be stated the other way round (slot 0 free, nothing in use),
     * which reads as "no client has ever opened this log" - a state recovery
     * cannot start from, since the restart LSN it needs lives in the client
     * record of an IN-USE client. */
    Area.ClientFreeList      = NTFS_LFS_CLIENT_NONE;
    Area.ClientInUseList     = NTFS_LFS_CLIENT_INDEX_NTFS;
    Area.Flags               = Clean ? NTFS_LFS_RESTART_FLAG_CLEAN : 0;
    Area.SeqNumberBits       = NtfsLfsSeqNumberBits(FileSize);
    Area.RestartAreaLength   = sizeof(NTFS_LFS_RESTART_AREA) + sizeof(NTFS_LFS_CLIENT_RECORD);
    Area.ClientArrayOffset   = (USHORT)sizeof(NTFS_LFS_RESTART_AREA);
    Area.FileSize            = FileSize;
    Area.LastLsnDataLength   = LastLsnDataLength;
    Area.RecordHeaderLength  = NTFS_LFS_RECORD_HEADER_LENGTH;
    Area.LogPageDataOffset   = LFS_RSTR_AREA_OFFSET;
    Area.RestartOpenLogCount = 1;
    Area.Reserved            = 0;
    RtlCopyMemory(Page + LFS_RSTR_AREA_OFFSET, &Area, sizeof(Area));

    /* Client checkpoint tables are outside the single-page emitter. */
    ASSERT(LFS_RSTR_AREA_OFFSET + sizeof(NTFS_LFS_RESTART_AREA) == LFS_RSTR_CLIENT_OFFSET);
    if (LFS_RSTR_CLIENT_OFFSET + sizeof(NTFS_LFS_CLIENT_RECORD) > PageSize)
        return STATUS_INVALID_PARAMETER;

    {
        NTFS_LFS_CLIENT_RECORD Client;

        RtlZeroMemory(&Client, sizeof(Client));
        Client.OldestLsn        = 0;
        Client.ClientRestartLsn = 0;
        Client.PrevClient       = NTFS_LFS_CLIENT_NONE;
        Client.NextClient       = NTFS_LFS_CLIENT_NONE;
        Client.SeqNumber        = 1;
        Client.ClientNameLength = (ULONG)(sizeof(NTFS_LFS_CLIENT_NAME_NTFS) - sizeof(WCHAR));
        RtlCopyMemory(Client.ClientName, NTFS_LFS_CLIENT_NAME_NTFS,
                      Client.ClientNameLength);
        RtlCopyMemory(Page + LFS_RSTR_CLIENT_OFFSET, &Client, sizeof(Client));
    }

    /* Stamp the per-sector USA sentinels.  AddFixupArray increments the USN
     * (0 -> 1) and writes each sector's trailing USHORT, saving the
     * originals into the USA so FixupUpdateSequenceArray reverses it. */
    return AddFixupArray(Vcb, Hdr);
}

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
    Hdr->Type = 0x44524352;
    Hdr->UsaOffset = LFS_RCRD_USA_OFFSET;
    Hdr->UsaCount = UsaCount;
    Hdr->Lsn = 0;

    /* NextRecordOffset names free space, not the first log record. */
    *(USHORT *)(Page + LFS_RCRD_NEXTREC_FIELD) = LFS_RCRD_FIRST_RECORD;
    *(USHORT *)(Page + 0x14) = 1; /* PageCount */
    *(USHORT *)(Page + 0x16) = 1; /* PagePosition */
    *(USHORT *)(Page + LFS_RCRD_USA_OFFSET) = 0;          /* USN seed */

    return AddFixupArray(Vcb, Hdr);
}

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

    /* LFS 1.1 reserves two restart pages and two tail-copy pages. */
    if (ImageLength < PageSize * 6 || (ImageLength % PageSize) != 0)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->Image = Image;
    Ctx->ImageLength = ImageLength;
    Ctx->PageSize = PageSize;
    Ctx->SectorSize = SectorSize;
    Ctx->FirstRcrdOffset = PageSize * 4;
    Ctx->SeqNumberBits = NtfsLfsSeqNumberBits((ULONGLONG)ImageLength);
    Ctx->CurrentSeqNumber = 1;
    Ctx->WriteOffset = Ctx->FirstRcrdOffset + LFS_RCRD_FIRST_RECORD;
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

static VOID
LfsRecordGeometry(USHORT RedoLength,
                  USHORT UndoLength,
                  USHORT LcnsToFollow,
                  PULONG RedoAbs,
                  PULONG UndoAbs,
                  PULONG ClientDataLength,
                  PULONG TotalLength)
{
    ULONG R, U;

    /* Client data = NTFS sub-header (0x20) | redo | undo, redo first, both
     * 8-byte aligned.  Client data starts at the 0x30 generic-header end. */
    R = ALIGN_UP_BY(NTFS_LFS_RECORD_HEADER_LENGTH + NTFS_LFS_ATTR_HEADER_LENGTH +
                     (ULONG)LcnsToFollow * sizeof(LONGLONG), 8);
    U = ALIGN_UP_BY(R + RedoLength, 8);

    *RedoAbs = R;
    *UndoAbs = U;
    *ClientDataLength = (U - NTFS_LFS_RECORD_HEADER_LENGTH) + UndoLength;
    *TotalLength = ALIGN_UP_BY(NTFS_LFS_RECORD_HEADER_LENGTH + *ClientDataLength, 8);
}

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
    Rec->ClientPreviousLsn = Params->ClientPreviousLsn;
    Rec->ClientUndoNextLsn = Params->ClientUndoNextLsn;
    Rec->ClientDataLength = ClientDataLength;
    Rec->ClientId = 1; /* client sequence number */
    Rec->Alignment = Ctx->ClientId; /* client array index */
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
    Rec->Alignment2 = Params->TargetBlockSize;
    Rec->TargetVcn = Params->TargetVcn;

    if (Params->LcnsToFollow != 0)
        RtlCopyMemory(Buffer + sizeof(*Rec), Params->Lcns,
                       (ULONG)Params->LcnsToFollow * sizeof(LONGLONG));
    if (Params->RedoLength != 0 && Params->RedoData != NULL)
        RtlCopyMemory(Buffer + RedoAbs, Params->RedoData, Params->RedoLength);
    if (Params->UndoLength != 0 && Params->UndoData != NULL)
        RtlCopyMemory(Buffer + UndoAbs, Params->UndoData, Params->UndoLength);

    if (RecordLengthOut != NULL)
        *RecordLengthOut = TotalLength;

    return STATUS_SUCCESS;
}

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

    if ((Params->RedoLength != 0 && Params->RedoData == NULL) ||
        (Params->UndoLength != 0 && Params->UndoData == NULL) ||
        (Params->LcnsToFollow != 0 && Params->Lcns == NULL))
        return STATUS_INVALID_PARAMETER;

    /* Use the same geometry helper Format uses so the range check before we
     * touch the page is exact. */
    LfsRecordGeometry(Params->RedoLength, Params->UndoLength, Params->LcnsToFollow,
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
        /* Recycling requires a transaction-aware checkpoint. Preserve
         * every existing record until one has made reuse safe. */
        if (PageBase + Ctx->PageSize > Ctx->ImageLength)
        {
            return STATUS_LOG_FILE_FULL;
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

        Hdr->Lsn = ThisLsn;
        *(PULONG)(Page + 0x10) |= 1; /* a complete record ends on this page */
        *(PULONGLONG)(Page + 0x20) = ThisLsn; /* LastEndLsn */
        *(PUSHORT)(Page + LFS_RCRD_NEXTREC_FIELD) = (USHORT)(RecordEnd - PageBase);

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
