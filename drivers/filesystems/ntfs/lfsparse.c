/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     Inspect $LogFile restart pages before permitting metadata writes
 *
 * Port of the restart inspection in 9bdb4dad41d/c7b1453c80e/c6b3756151b.
 * Restart pages alternate: different LSNs are normal, and a valid copy can
 * survive a torn update of its peer. The newest valid copy is authoritative.
 * Layout: libfsntfs NTFS format documentation, section 14.1/14.2.
 */
#include "lfs.h"
#define NDEBUG
#include <debug.h>

static BOOLEAN
NtfsLogPageSizeValid(ULONG Size, ULONG SectorSize)
{
    return Size >= SectorSize && Size <= NTFS_LOG_MAX_PAGE &&
           (Size & (Size - 1)) == 0;
}

/* No restart copy anywhere: the first two 4 KiB pages are uniform (zero,
 * or the formatter's 0xFF), and no larger page size puts a restart page
 * after them.  The record pages may hold anything: without a restart area
 * they describe nothing. */
static BOOLEAN
NtfsLogUnused(PUCHAR Data, ULONG Length)
{
    ULONG i, Size, Head = min(Length, 2 * PAGE_SIZE);
    UCHAR Fill = Data[0];

    if (Fill != 0 && Fill != 0xff) return FALSE;
    for (i = 1; i < Head; i++)
        if (Data[i] != Fill) return FALSE;
    for (Size = 2 * PAGE_SIZE; Size <= NTFS_LOG_MAX_PAGE && Size + sizeof(ULONG) <= Length; Size *= 2)
    {
        if (*(PULONG)(Data + Size) == NTFS_LOG_RESTART_MAGIC)
            return FALSE;
    }
    return TRUE;
}

static NTSTATUS
NtfsLogReadRestart(PDEVICE_EXTENSION Vcb,
                    PUCHAR Page,
                    ULONG PageSize,
                    ULONGLONG FileSize,
                    PNTFS_LOG_RESTART_INFO Info,
                    PUSHORT Major,
                    PUSHORT Minor)
{
    PNTFS_LOG_RESTART_HEADER Header = (PVOID)Page;
    PNTFS_LOG_RESTART_AREA Restart = &Info->Area;
    PUCHAR Client;
    ULONG SectorSize = Vcb->NtfsInfo.BytesPerSector;
    ULONG UsaEnd, ClientEnd;
    NTSTATUS Status;

    if (Header->Header.Type != NTFS_LOG_RESTART_MAGIC ||
        Header->SystemPageSize != PageSize ||
        !NtfsLogPageSizeValid(Header->LogPageSize, SectorSize) ||
        Header->Header.UsaCount != PageSize / SectorSize + 1 ||
        Header->Header.UsaOffset < sizeof(*Header))
        return STATUS_FILE_CORRUPT_ERROR;
    UsaEnd = Header->Header.UsaOffset + Header->Header.UsaCount * sizeof(USHORT);
    if (UsaEnd > SectorSize - sizeof(USHORT) ||
        Header->RestartOffset < ALIGN_UP_BY(UsaEnd, sizeof(ULONGLONG)) ||
        (Header->RestartOffset & (sizeof(ULONGLONG) - 1)) ||
        Header->RestartOffset > PageSize - sizeof(*Restart))
        return STATUS_FILE_CORRUPT_ERROR;
    Status = FixupUpdateSequenceArray(Vcb, &Header->Header);
    if (!NT_SUCCESS(Status)) return Status;
    RtlCopyMemory(Restart, Page + Header->RestartOffset, sizeof(*Restart));
    ClientEnd = Restart->ClientArrayOffset + (ULONG)Restart->LogClients * 0xa0;
    if (Restart->RestartAreaLength < sizeof(*Restart) ||
        Restart->RestartAreaLength > PageSize - Header->RestartOffset ||
        Restart->ClientArrayOffset < sizeof(*Restart) ||
        (Restart->ClientArrayOffset & (sizeof(ULONGLONG) - 1)) ||
        ClientEnd > Restart->RestartAreaLength ||
        (Restart->ClientFreeList != MAXUSHORT && Restart->ClientFreeList >= Restart->LogClients) ||
        (Restart->ClientInUseList != MAXUSHORT && Restart->ClientInUseList >= Restart->LogClients) ||
        Restart->FileSize > FileSize || Restart->FileSize < 4 * Header->LogPageSize ||
        Restart->SeqNumberBits == 0 || Restart->SeqNumberBits >= 64 ||
        Restart->RecordHeaderLength < 0x30 ||
        Restart->LogPageDataOffset < 0x28 ||
        Restart->LogPageDataOffset >= Header->LogPageSize)
        return STATUS_FILE_CORRUPT_ERROR;
    Info->LogPageSize = Header->LogPageSize;
    Info->SystemPageSize = Header->SystemPageSize;
    Info->LogSize = FileSize;
    Info->ClientInUse = Restart->ClientInUseList == 0;
    Info->ClientSeqNumber = 0;
    Info->OldestLsn = Info->ClientRestartLsn = 0;
    if (Info->ClientInUse)
    {
        /* ntfs-3g LOG_CLIENT_RECORD: oldest_lsn, client_restart_lsn,
         * prev/next client, seq_number. */
        Client = Page + Header->RestartOffset + Restart->ClientArrayOffset;
        RtlCopyMemory(&Info->OldestLsn, Client, sizeof(ULONGLONG));
        RtlCopyMemory(&Info->ClientRestartLsn, Client + 8, sizeof(ULONGLONG));
        RtlCopyMemory(&Info->ClientSeqNumber, Client + 0x14, sizeof(USHORT));
    }
    *Major = Header->MajorVersion;
    *Minor = Header->MinorVersion;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsLfsReadRestart(PDEVICE_EXTENSION Vcb, PNTFS_LOG_RESTART_INFO Best)
{
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PUCHAR Data = NULL, Page = NULL;
    NTFS_LOG_RESTART_INFO Info;
    ULONGLONG FileSize;
    ULONG Length, Size, Copy;
    USHORT Major, Minor;
    BOOLEAN Found = FALSE;
    NTSTATUS Status;

    RtlZeroMemory(Best, sizeof(*Best));
    Vcb->LogFileDirty = TRUE;
    Vcb->LogFileVersionUnsupported = FALSE;
    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    FileSize = AttributeDataLength(Context->pRecord);
    Best->LogSize = FileSize;
    Length = (ULONG)min(FileSize, 2ULL * NTFS_LOG_MAX_PAGE);
    if (Length < 2 * Vcb->NtfsInfo.BytesPerSector)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }
    Data = ExAllocatePoolWithTag(PagedPool, Length, TAG_NTFS);
    Page = ExAllocatePoolWithTag(PagedPool, NTFS_LOG_MAX_PAGE, TAG_NTFS);
    if (Data == NULL || Page == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    if (ReadAttribute(Vcb, Context, 0, (PCHAR)Data, Length) != Length)
    {
        Status = STATUS_UNEXPECTED_IO_ERROR;
        goto Cleanup;
    }
    /* Both the upstream formatter (FF) and unused zero-filled logs are
     * accepted. Arbitrary damage without RSTR magic is never a clean log. */
    if (NtfsLogUnused(Data, Length))
    {
        Vcb->LogFileDirty = FALSE;
        Vcb->LogFileLsn = 0;
        Status = STATUS_SUCCESS;
        goto Cleanup;
    }
    /* Try page sizes independently so an invalid primary header cannot
     * hide a valid backup with an otherwise unknown system page size. */
    for (Size = Vcb->NtfsInfo.BytesPerSector;
         Size <= NTFS_LOG_MAX_PAGE && Size <= Length / 2;
         Size *= 2)
    {
        for (Copy = 0; Copy < 2; Copy++)
        {
            RtlCopyMemory(Page, Data + Copy * Size, Size);
            Status = NtfsLogReadRestart(Vcb, Page, Size, FileSize, &Info, &Major, &Minor);
            if (NT_SUCCESS(Status) && (!Found || Info.Area.CurrentLsn > Best->Area.CurrentLsn))
            {
                *Best = Info;
                Vcb->LogFileMajorVersion = Major;
                Vcb->LogFileMinorVersion = Minor;
                Found = TRUE;
            }
        }
    }
    Status = STATUS_FILE_CORRUPT_ERROR;
    if (Found)
    {
        Vcb->LogFileLsn = Best->Area.CurrentLsn;
        Vcb->LogFileVersionUnsupported = Vcb->LogFileMajorVersion != 1 ||
                                       Vcb->LogFileMinorVersion > 1;
        Vcb->LogFileDirty = !(Best->Area.Flags & NTFS_LOG_CLEAN);
        Status = Vcb->LogFileDirty ? STATUS_LOG_FILE_FULL : STATUS_SUCCESS;
    }
Cleanup:
    if (Page != NULL) ExFreePoolWithTag(Page, TAG_NTFS);
    if (Data != NULL) ExFreePoolWithTag(Data, TAG_NTFS);
    if (Context != NULL) ReleaseAttributeContext(Context);
    if (Record != NULL) ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    return Status;
}

NTSTATUS
NtfsLfsCheckCleanShutdown(PDEVICE_EXTENSION Vcb)
{
    NTFS_LOG_RESTART_INFO Info;

    return NtfsLfsReadRestart(Vcb, &Info);
}
