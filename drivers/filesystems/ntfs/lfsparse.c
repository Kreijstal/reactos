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

static BOOLEAN
NtfsLogUnused(PUCHAR Data, ULONG Length)
{
    ULONG i;
    UCHAR Fill = Data[0];

    if (Fill != 0 && Fill != 0xff) return FALSE;
    for (i = 1; i < Length; i++)
        if (Data[i] != Fill) return FALSE;
    return TRUE;
}

static NTSTATUS
NtfsLogReadRestart(PDEVICE_EXTENSION Vcb,
                    PUCHAR Page,
                    ULONG PageSize,
                    ULONGLONG FileSize,
                    PNTFS_LOG_RESTART_AREA Restart,
                    PUSHORT Major,
                    PUSHORT Minor)
{
    PNTFS_LOG_RESTART_HEADER Header = (PVOID)Page;
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
    *Major = Header->MajorVersion;
    *Minor = Header->MinorVersion;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsLfsCheckCleanShutdown(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PUCHAR Data = NULL, Page = NULL;
    NTFS_LOG_RESTART_AREA Restart, Best = {0};
    ULONGLONG FileSize;
    ULONG Length, Size, Copy;
    USHORT Major, Minor;
    BOOLEAN Found = FALSE;
    NTSTATUS Status;

    Vcb->LogFileDirty = TRUE;
    Vcb->LogFileVersionUnsupported = FALSE;
    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    FileSize = AttributeDataLength(Context->pRecord);
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
            Status = NtfsLogReadRestart(Vcb, Page, Size, FileSize, &Restart, &Major, &Minor);
            if (NT_SUCCESS(Status) && (!Found || Restart.CurrentLsn > Best.CurrentLsn))
            {
                Best = Restart;
                Vcb->LogFileMajorVersion = Major;
                Vcb->LogFileMinorVersion = Minor;
                Found = TRUE;
            }
        }
    }
    Status = STATUS_FILE_CORRUPT_ERROR;
    if (Found)
    {
        Vcb->LogFileLsn = Best.CurrentLsn;
        Vcb->LogFileVersionUnsupported = Vcb->LogFileMajorVersion != 1 ||
                                       Vcb->LogFileMinorVersion > 1;
        Vcb->LogFileDirty = !(Best.Flags & NTFS_LOG_CLEAN);
        Status = Vcb->LogFileDirty ? STATUS_LOG_FILE_FULL : STATUS_SUCCESS;
    }
Cleanup:
    if (Page != NULL) ExFreePoolWithTag(Page, TAG_NTFS);
    if (Data != NULL) ExFreePoolWithTag(Data, TAG_NTFS);
    if (Context != NULL) ReleaseAttributeContext(Context);
    if (Record != NULL) ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    return Status;
}
