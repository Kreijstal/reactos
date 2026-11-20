/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that \Device\HarddiskN\PartitionM follows a layout change
 * COPYRIGHT:   ReactOS Developers
 *
 * When a partition is removed from the layout, PnP removes its device only
 * later. Its \Device\HarddiskN\PartitionM name must not keep pointing at the
 * removed volume in the meantime: setup creates a partition in the place of a
 * deleted one and immediately opens it by that name to format it, and got the
 * old volume (whose stale file system then mounted, or which disappeared
 * before the format opened it).
 *
 * The test only runs on a disk without any partition, which it partitions and
 * cleans up again; it never touches a disk holding data.
 */

#include "precomp.h"
#include <ntddstor.h>
#include <mountmgr.h>
#include <mountdev.h>
#include <strsafe.h>
#include <fmifs/fmifs.h>

#define FILE_SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#define MB (1024 * 1024)

typedef struct _LAYOUT4
{
    DRIVE_LAYOUT_INFORMATION_EX Layout;
    PARTITION_INFORMATION_EX More[3];
} LAYOUT4;

static BOOL
IsBlankDisk(HANDLE Disk, PULONG DiskNumber)
{
    LAYOUT4 Layout;
    STORAGE_DEVICE_NUMBER Number;
    GET_LENGTH_INFORMATION Length;
    DWORD Size, i;

    if (!DeviceIoControl(Disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &Number, sizeof(Number), &Size, NULL) ||
        !DeviceIoControl(Disk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &Length, sizeof(Length), &Size, NULL) ||
        Length.Length.QuadPart < 64 * MB)
    {
        return FALSE;
    }

    if (DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, &Layout, sizeof(Layout), &Size, NULL))
    {
        if (Layout.Layout.PartitionStyle == PARTITION_STYLE_GPT)
            return FALSE;
        for (i = 0; i < Layout.Layout.PartitionCount && i < 4; i++)
        {
            if (Layout.Layout.PartitionEntry[i].Mbr.PartitionType != PARTITION_ENTRY_UNUSED)
                return FALSE;
        }
    }
    else
    {
        /* More than 4 entries or an unknown state: leave the disk alone */
        return FALSE;
    }

    *DiskNumber = Number.DeviceNumber;
    return TRUE;
}

static BOOL
SetOnePartition(HANDLE Disk, LONGLONG Length)
{
    LAYOUT4 Layout;
    DWORD Size;

    ZeroMemory(&Layout, sizeof(Layout));
    Layout.Layout.PartitionStyle = PARTITION_STYLE_MBR;
    Layout.Layout.PartitionCount = 4;
    Layout.Layout.Mbr.Signature = 0x52545052; /* 'RPTR' */
    if (Length)
    {
        Layout.Layout.PartitionEntry[0].PartitionStyle = PARTITION_STYLE_MBR;
        Layout.Layout.PartitionEntry[0].StartingOffset.QuadPart = 1 * MB;
        Layout.Layout.PartitionEntry[0].PartitionLength.QuadPart = Length;
        Layout.Layout.PartitionEntry[0].RewritePartition = TRUE;
        Layout.Layout.PartitionEntry[0].Mbr.PartitionType = PARTITION_FAT_16;
        Layout.Layout.PartitionEntry[0].Mbr.HiddenSectors = MB / 512;
    }
    for (Size = 1; Size < 4; Size++)
        Layout.Layout.PartitionEntry[Size].RewritePartition = TRUE;

    return DeviceIoControl(Disk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, &Layout, sizeof(Layout), NULL, 0, &Size, NULL);
}

/* Opens \Device\HarddiskN\Partition1 without mounting it and returns the
 * volume device it names */
static NTSTATUS
QueryPartition1(ULONG DiskNumber, PWSTR VolumeName, ULONG VolumeNameLength)
{
    WCHAR Name[64];
    UNICODE_STRING NameU;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK Iosb;
    HANDLE Handle;
    NTSTATUS Status;
    struct { MOUNTDEV_NAME; WCHAR Buffer[64]; } DeviceName;

    StringCchPrintfW(Name, _countof(Name), L"\\Device\\Harddisk%lu\\Partition1", DiskNumber);
    RtlInitUnicodeString(&NameU, Name);
    InitializeObjectAttributes(&ObjectAttributes, &NameU, OBJ_CASE_INSENSITIVE, NULL, NULL);
    /* FILE_READ_ATTRIBUTES only: a direct device open, no file system mount */
    Status = NtOpenFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &ObjectAttributes,
                        &Iosb, FILE_SHARE_ALL, FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtDeviceIoControlFile(Handle, NULL, NULL, NULL, &Iosb, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME,
                                   NULL, 0, &DeviceName, sizeof(DeviceName));
    NtClose(Handle);
    if (NT_SUCCESS(Status))
    {
        StringCchCopyNW(VolumeName, VolumeNameLength, DeviceName.Name,
                        DeviceName.NameLength / sizeof(WCHAR));
    }
    return Status;
}

/* Waits for Partition1 to name a volume other than Exclude */
static NTSTATUS
WaitForPartition1(ULONG DiskNumber, PCWSTR Exclude, PWSTR VolumeName, ULONG VolumeNameLength)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    ULONG i;

    for (i = 0; i < 100; i++)
    {
        Status = QueryPartition1(DiskNumber, VolumeName, VolumeNameLength);
        if (NT_SUCCESS(Status) && (!Exclude || wcscmp(VolumeName, Exclude) != 0))
            return Status;
        Sleep(100);
    }
    return NT_SUCCESS(Status) ? STATUS_TIMEOUT : Status;
}

typedef VOID (NTAPI *PFORMATEX)(PWCHAR, FMIFS_MEDIA_FLAG, PWCHAR, PWCHAR, BOOLEAN, ULONG, PFMIFSCALLBACK);

static BOOLEAN NTAPI
FormatCallback(CALLBACKCOMMAND Command, ULONG SubAction, PVOID ActionInfo)
{
    if (Command == DONE)
        ok(*(PBOOLEAN)ActionInfo, "Formatting the old volume failed\n");
    return TRUE;
}

START_TEST(PartitionRelayout)
{
    PFORMATEX pFormatEx = (PFORMATEX)GetProcAddress(LoadLibraryW(L"fmifs.dll"), "FormatEx");
    WCHAR DiskName[96];
    WCHAR VolumeA[64], VolumeNow[64], VolumeB[64];
    HANDLE Disk = INVALID_HANDLE_VALUE, OldVolume;
    ULONG DiskNumber = 0, i, Round, Stale = 0;
    DWORD Start;
    CREATE_DISK CreateDisk;
    NTSTATUS Status;
    DWORD Size;

    for (i = 0; i < 16; i++)
    {
        StringCchPrintfW(DiskName, _countof(DiskName), L"\\\\.\\PhysicalDrive%lu", i);
        Disk = CreateFileW(DiskName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
        if (Disk == INVALID_HANDLE_VALUE)
            continue;
        if (IsBlankDisk(Disk, &DiskNumber))
            break;
        CloseHandle(Disk);
        Disk = INVALID_HANDLE_VALUE;
    }
    if (Disk == INVALID_HANDLE_VALUE)
    {
        skip("No blank disk to repartition\n");
        return;
    }
    trace("Using blank disk %lu\n", DiskNumber);

    ZeroMemory(&CreateDisk, sizeof(CreateDisk));
    CreateDisk.PartitionStyle = PARTITION_STYLE_MBR;
    CreateDisk.Mbr.Signature = 0x52545052;
    ok(DeviceIoControl(Disk, IOCTL_DISK_CREATE_DISK, &CreateDisk, sizeof(CreateDisk), NULL, 0, &Size, NULL),
       "IOCTL_DISK_CREATE_DISK failed with %lu\n", GetLastError());

    /* Partition 1, 16 MB */
    ok(SetOnePartition(Disk, 16 * MB), "Setting the first layout failed with %lu\n", GetLastError());
    Status = WaitForPartition1(DiskNumber, NULL, VolumeA, _countof(VolumeA));
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Replace partition 1 by one of another size. PnP removes the old device
     * asynchronously, so this catches a stale name only when the removal is
     * slower than the open below (as it was in setup). The second round
     * keeps the new volume open while it goes away: its file system holds
     * the volume locked while it passes the removal down, and that must not
     * wait for MountMgr, which may be opening the same volume. */
    for (Round = 0; Round < 2; Round++)
    {
        /* Keep a file system mounted on the old volume, as setup has when it
         * deletes the partition of an existing installation */
        StringCchPrintfW(DiskName, _countof(DiskName), L"\\\\?\\GLOBALROOT%ls\\", VolumeA);
        if (pFormatEx && Round == 0)
            pFormatEx(DiskName, FMIFS_HARDDISK, L"FAT", L"", TRUE, 0, FormatCallback);
        OldVolume = CreateFileW(DiskName, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

        ok(SetOnePartition(Disk, (Round & 1) ? 16 * MB : 32 * MB),
           "Round %lu: setting the layout failed with %lu\n", Round, GetLastError());

        /* From now on, the name must never reach the volume that is gone */
        Start = GetTickCount();
        do
        {
            Status = QueryPartition1(DiskNumber, VolumeNow, _countof(VolumeNow));
            if (NT_SUCCESS(Status) && wcscmp(VolumeNow, VolumeA) == 0)
            {
                Stale++;
                break;
            }
            if (NT_SUCCESS(Status))
                break;
        } while (GetTickCount() - Start < 300);

        if (OldVolume != INVALID_HANDLE_VALUE)
            CloseHandle(OldVolume);

        ok(Stale == 0, "Round %lu: Partition1 still names the removed volume %ls\n", Round, VolumeA);

        /* And it then names the new volume */
        Status = WaitForPartition1(DiskNumber, VolumeA, VolumeB, _countof(VolumeB));
        ok(NT_SUCCESS(Status), "Round %lu: waiting for the new Partition1 failed with 0x%lx\n", Round, Status);
        if (!NT_SUCCESS(Status))
            break;
        StringCchCopyW(VolumeA, _countof(VolumeA), VolumeB);
    }
    trace("%lu rounds, last Partition1 %ls\n", Round, VolumeA);

Cleanup:
    ok(SetOnePartition(Disk, 0), "Clearing the layout failed with %lu\n", GetLastError());
    CloseHandle(Disk);
}

/*
 * Formatting a partition as NTFS makes it an NTFS volume, whatever its MBR
 * type was: like Windows, format sets the type to IFS (0x07), and NTFS
 * recognizes the volume by its boot sector, not by the partition type.
 */
START_TEST(NtfsFormatType)
{
    PFORMATEX pFormatEx = (PFORMATEX)GetProcAddress(LoadLibraryW(L"fmifs.dll"), "FormatEx");
    WCHAR DiskName[96], Volume[64], FsName[16];
    HANDLE Disk = INVALID_HANDLE_VALUE, VolumeHandle;
    PARTITION_INFORMATION_EX PartInfo;
    ULONG DiskNumber = 0, i;
    CREATE_DISK CreateDisk;
    NTSTATUS Status;
    DWORD Size;

    if (!pFormatEx)
    {
        skip("fmifs!FormatEx not available\n");
        return;
    }
    for (i = 0; i < 16; i++)
    {
        StringCchPrintfW(DiskName, _countof(DiskName), L"\\\\.\\PhysicalDrive%lu", i);
        Disk = CreateFileW(DiskName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
        if (Disk == INVALID_HANDLE_VALUE)
            continue;
        if (IsBlankDisk(Disk, &DiskNumber))
            break;
        CloseHandle(Disk);
        Disk = INVALID_HANDLE_VALUE;
    }
    if (Disk == INVALID_HANDLE_VALUE)
    {
        skip("No blank disk to repartition\n");
        return;
    }

    ZeroMemory(&CreateDisk, sizeof(CreateDisk));
    CreateDisk.PartitionStyle = PARTITION_STYLE_MBR;
    CreateDisk.Mbr.Signature = 0x52545052;
    ok(DeviceIoControl(Disk, IOCTL_DISK_CREATE_DISK, &CreateDisk, sizeof(CreateDisk), NULL, 0, &Size, NULL),
       "IOCTL_DISK_CREATE_DISK failed with %lu\n", GetLastError());

    /* A FAT16-type partition (0x06), as diskpart creates it */
    ok(SetOnePartition(Disk, 32 * MB), "Setting the layout failed with %lu\n", GetLastError());
    Status = WaitForPartition1(DiskNumber, NULL, Volume, _countof(Volume));
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    StringCchPrintfW(DiskName, _countof(DiskName), L"\\\\?\\GLOBALROOT%ls\\", Volume);
    pFormatEx(DiskName, FMIFS_HARDDISK, L"NTFS", L"", TRUE, 0, FormatCallback);

    /* The partition is an IFS one now */
    StringCchPrintfW(DiskName, _countof(DiskName), L"\\\\?\\GLOBALROOT%ls", Volume);
    VolumeHandle = CreateFileW(DiskName, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
    ok(VolumeHandle != INVALID_HANDLE_VALUE, "Opening %ls failed with %lu\n", DiskName, GetLastError());
    if (VolumeHandle != INVALID_HANDLE_VALUE)
    {
        ok(DeviceIoControl(VolumeHandle, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0,
                           &PartInfo, sizeof(PartInfo), &Size, NULL),
           "IOCTL_DISK_GET_PARTITION_INFO_EX failed with %lu\n", GetLastError());
        ok(PartInfo.Mbr.PartitionType == PARTITION_IFS,
           "Partition type is 0x%02x after formatting as NTFS\n", PartInfo.Mbr.PartitionType);
        CloseHandle(VolumeHandle);
    }

    /* And it mounts as NTFS */
    StringCchPrintfW(DiskName, _countof(DiskName), L"\\\\?\\GLOBALROOT%ls\\", Volume);
    VolumeHandle = CreateFileW(DiskName, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok(VolumeHandle != INVALID_HANDLE_VALUE, "Opening %ls failed with %lu\n", DiskName, GetLastError());
    if (VolumeHandle != INVALID_HANDLE_VALUE)
    {
        struct { FILE_FS_ATTRIBUTE_INFORMATION; WCHAR Buffer[16]; } Attr;
        IO_STATUS_BLOCK Iosb;

        Status = NtQueryVolumeInformationFile(VolumeHandle, &Iosb, &Attr, sizeof(Attr),
                                              FileFsAttributeInformation);
        ok_ntstatus(Status, STATUS_SUCCESS);
        FsName[0] = UNICODE_NULL;
        if (NT_SUCCESS(Status))
            StringCchCopyNW(FsName, _countof(FsName), Attr.FileSystemName,
                            Attr.FileSystemNameLength / sizeof(WCHAR));
        ok(wcscmp(FsName, L"NTFS") == 0, "File system is '%ls'\n", FsName);
        CloseHandle(VolumeHandle);
    }

Cleanup:
    ok(SetOnePartition(Disk, 0), "Clearing the layout failed with %lu\n", GetLastError());
    CloseHandle(Disk);
}

/*
 * A partition recreated with the offset and length of one whose device is
 * still open gets the device instance ID of the old one (partmgr builds it
 * from the disk signature, the offset and the length). PnP must take the new
 * partition for that instance while the old device waits for its handles to
 * be closed, as Windows does for a device that is plugged in again while it
 * is still open, and not bugcheck on a duplicate device instance.
 *
 * The signature in these IDs (and in the unique ID MountMgr knows the
 * volume by) is that of the layout that was set, not that of the layout
 * last read from the disk.
 */
START_TEST(PartitionSameIdentity)
{
    WCHAR DiskName[96], Name[96];
    WCHAR VolumeA[64], VolumeB[64], VolumeC[64], VolumeNow[64];
    HANDLE Disk = INVALID_HANDLE_VALUE, OldVolume = NULL, NewVolume;
    UNICODE_STRING NameU;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK Iosb;
    PARTITION_INFORMATION_EX PartInfo;
    struct { MOUNTDEV_UNIQUE_ID Id; UCHAR Buffer[32]; } UniqueId;
    LAYOUT4 Layout;
    ULONG DiskNumber = 0, i;
    CREATE_DISK CreateDisk;
    NTSTATUS Status;
    DWORD Size;

    for (i = 0; i < 16; i++)
    {
        StringCchPrintfW(DiskName, _countof(DiskName), L"\\\\.\\PhysicalDrive%lu", i);
        Disk = CreateFileW(DiskName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
        if (Disk == INVALID_HANDLE_VALUE)
            continue;
        if (IsBlankDisk(Disk, &DiskNumber))
            break;
        CloseHandle(Disk);
        Disk = INVALID_HANDLE_VALUE;
    }
    if (Disk == INVALID_HANDLE_VALUE)
    {
        skip("No blank disk to repartition\n");
        return;
    }
    trace("Using blank disk %lu\n", DiskNumber);

    ZeroMemory(&CreateDisk, sizeof(CreateDisk));
    CreateDisk.PartitionStyle = PARTITION_STYLE_MBR;
    CreateDisk.Mbr.Signature = 0x32545052; /* 'RPT2' */
    ok(DeviceIoControl(Disk, IOCTL_DISK_CREATE_DISK, &CreateDisk, sizeof(CreateDisk), NULL, 0, &Size, NULL),
       "IOCTL_DISK_CREATE_DISK failed with %lu\n", GetLastError());
    /* Have the disk's layout read, with that signature */
    ok(DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, &Layout, sizeof(Layout), &Size, NULL),
       "IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed with %lu\n", GetLastError());
    ok(Layout.Layout.Mbr.Signature == 0x32545052, "Signature is 0x%08lx\n", Layout.Layout.Mbr.Signature);

    /* Partition 1, 16 MB, in a layout with another signature ('RPTR'), and
     * keep its device open */
    ok(SetOnePartition(Disk, 16 * MB), "Setting the first layout failed with %lu\n", GetLastError());
    Status = WaitForPartition1(DiskNumber, NULL, VolumeA, _countof(VolumeA));
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RtlInitUnicodeString(&NameU, VolumeA);
    InitializeObjectAttributes(&ObjectAttributes, &NameU, OBJ_CASE_INSENSITIVE, NULL, NULL);
    /* FILE_READ_ATTRIBUTES only: a direct device open, no file system mount */
    Status = NtOpenFile(&OldVolume, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &ObjectAttributes,
                        &Iosb, FILE_SHARE_ALL, FILE_SYNCHRONOUS_IO_NONALERT);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        OldVolume = NULL;
        goto Cleanup;
    }

    /* Its unique ID is made of the signature of the layout just set and its offset */
    Status = NtDeviceIoControlFile(OldVolume, NULL, NULL, NULL, &Iosb, IOCTL_MOUNTDEV_QUERY_UNIQUE_ID,
                                   NULL, 0, &UniqueId, sizeof(UniqueId));
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(UniqueId.Id.UniqueIdLength == 12, "UniqueIdLength is %u\n", UniqueId.Id.UniqueIdLength);
        ok(*(PULONG)&UniqueId.Id.UniqueId[0] == 0x52545052,
           "Unique ID signature is 0x%08lx\n", *(PULONG)&UniqueId.Id.UniqueId[0]);
        ok(*(PULONGLONG)&UniqueId.Id.UniqueId[4] == 1 * MB,
           "Unique ID offset is 0x%I64x\n", *(PULONGLONG)&UniqueId.Id.UniqueId[4]);
    }

    /* Another partition in its place: the open one is surprise-removed */
    ok(SetOnePartition(Disk, 32 * MB), "Setting the second layout failed with %lu\n", GetLastError());
    Status = WaitForPartition1(DiskNumber, VolumeA, VolumeB, _countof(VolumeB));
    ok(NT_SUCCESS(Status), "Waiting for the second Partition1 failed with 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* And the first one again, while the device of the first one is open */
    ok(SetOnePartition(Disk, 16 * MB), "Setting the third layout failed with %lu\n", GetLastError());
    Status = WaitForPartition1(DiskNumber, VolumeB, VolumeC, _countof(VolumeC));
    ok(NT_SUCCESS(Status), "Waiting for the third Partition1 failed with 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    ok(wcscmp(VolumeC, VolumeA) != 0, "Partition1 names the removed volume %ls\n", VolumeA);
    trace("Volumes %ls, %ls, %ls\n", VolumeA, VolumeB, VolumeC);

    /* The removal of the old device completes now; it leaves the new one alone */
    NtClose(OldVolume);
    OldVolume = NULL;
    Sleep(1000);

    Status = QueryPartition1(DiskNumber, VolumeNow, _countof(VolumeNow));
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok(wcscmp(VolumeNow, VolumeC) == 0, "Partition1 names %ls, expected %ls\n", VolumeNow, VolumeC);

    StringCchPrintfW(Name, _countof(Name), L"\\\\?\\GLOBALROOT%ls", VolumeC);
    NewVolume = CreateFileW(Name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
    ok(NewVolume != INVALID_HANDLE_VALUE, "Opening %ls failed with %lu\n", Name, GetLastError());
    if (NewVolume != INVALID_HANDLE_VALUE)
    {
        ok(DeviceIoControl(NewVolume, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0,
                           &PartInfo, sizeof(PartInfo), &Size, NULL),
           "IOCTL_DISK_GET_PARTITION_INFO_EX failed with %lu\n", GetLastError());
        ok(PartInfo.PartitionLength.QuadPart == 16 * MB,
           "Partition length is 0x%I64x\n", PartInfo.PartitionLength.QuadPart);
        CloseHandle(NewVolume);
    }

Cleanup:
    if (OldVolume)
        NtClose(OldVolume);
    ok(SetOnePartition(Disk, 0), "Clearing the layout failed with %lu\n", GetLastError());
    CloseHandle(Disk);
}
