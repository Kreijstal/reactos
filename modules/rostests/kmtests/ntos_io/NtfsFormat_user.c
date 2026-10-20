/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (see COPYING.LIB)
 * PURPOSE:     Public formatter entry point on a dedicated throwaway volume
 */
#include <kmt_test.h>
#include <winioctl.h>
#include <fmifs/fmifs.h>
#include "NtfsFslib_user.h"

typedef VOID (NTAPI *FORMAT_EX)(PWCHAR, FMIFS_MEDIA_FLAG, PWCHAR, PWCHAR,
                               BOOLEAN, ULONG, PFMIFSCALLBACK);
static BOOLEAN FormatSucceeded;
static ULONG CompletionCount;

static BOOLEAN NTAPI
FormatCallback(CALLBACKCOMMAND Command, ULONG SubAction, PVOID Argument)
{
    UNREFERENCED_PARAMETER(SubAction);
    if (Command == DONE)
    {
        FormatSucceeded = *(PBOOLEAN)Argument;
        ++CompletionCount;
    }
    return TRUE;
}

/* Formatting is limited to the harness's labelled, 256 MiB data disk.
 * A label match alone must never authorize formatting an arbitrary disk. */
BOOLEAN
NtfsKmtFindFormatVolume(PWCHAR Root, PWCHAR Volume)
{
    WCHAR Drive, Label[64], FileSystem[32], Windows[MAX_PATH];
    PARTITION_INFORMATION_EX Partition;
    GET_LENGTH_INFORMATION Length;
    HANDLE Handle;
    DWORD Bytes;
    BOOLEAN Match;

    if (!GetWindowsDirectoryW(Windows, RTL_NUMBER_OF(Windows)))
        return FALSE;
    for (Drive = L'D'; Drive <= L'Z'; ++Drive)
    {
        Root[0] = Drive; Root[1] = L':'; Root[2] = L'\\'; Root[3] = 0;
        if (Drive == Windows[0] ||
            !GetVolumeInformationW(Root, Label, RTL_NUMBER_OF(Label), NULL, NULL, NULL,
                                   FileSystem, RTL_NUMBER_OF(FileSystem)) ||
            wcscmp(Label, L"KMTNTFS") || wcscmp(FileSystem, L"NTFS"))
            continue;
        swprintf(Volume, 16, L"\\\\.\\%c:", Drive);
        Handle = CreateFileW(Volume, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (Handle == INVALID_HANDLE_VALUE)
            continue;
        Match = DeviceIoControl(Handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                                &Length, sizeof(Length), &Bytes, NULL) &&
                Length.Length.QuadPart >= 250LL * 1024 * 1024 &&
                Length.Length.QuadPart < 256LL * 1024 * 1024 &&
                DeviceIoControl(Handle, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0,
                                &Partition, sizeof(Partition), &Bytes, NULL) &&
                Partition.StartingOffset.QuadPart == 1024 * 1024;
        CloseHandle(Handle);
        if (Match)
            return TRUE;
    }
    return FALSE;
}

/* Take the fixture off NTFS: invalidate both boot sectors through a locked,
 * dismounted volume handle, so the next open of the volume is mounted by RAW.
 * This is the state setup's format starts from on a fresh partition. */
static BOOLEAN
MakeVolumeRaw(PWCHAR Root, PWCHAR Volume)
{
    GET_LENGTH_INFORMATION Length;
    WCHAR FileSystem[32];
    LARGE_INTEGER Offset;
    HANDLE Handle;
    DWORD Bytes;
    BYTE Sector[512];
    BOOLEAN Done = FALSE;

    Handle = CreateFileW(Volume, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Cannot open %ls: %lu\n", Volume, GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!DeviceIoControl(Handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL) ||
        !DeviceIoControl(Handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL) ||
        !DeviceIoControl(Handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                         &Length, sizeof(Length), &Bytes, NULL))
    {
        ok(FALSE, "Cannot take %ls offline: %lu\n", Volume, GetLastError());
        goto Cleanup;
    }
    ZeroMemory(Sector, sizeof(Sector));
    Sector[510] = 0x55;
    Sector[511] = 0xAA;
    Offset.QuadPart = 0;
    ok(SetFilePointerEx(Handle, Offset, NULL, FILE_BEGIN) &&
       WriteFile(Handle, Sector, sizeof(Sector), &Bytes, NULL) && Bytes == sizeof(Sector),
       "Primary boot sector write failed: %lu\n", GetLastError());
    Offset.QuadPart = Length.Length.QuadPart - sizeof(Sector);
    ok(SetFilePointerEx(Handle, Offset, NULL, FILE_BEGIN) &&
       WriteFile(Handle, Sector, sizeof(Sector), &Bytes, NULL) && Bytes == sizeof(Sector),
       "Backup boot sector write failed: %lu\n", GetLastError());
    DeviceIoControl(Handle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    Done = TRUE;
Cleanup:
    CloseHandle(Handle);
    if (!Done)
        return FALSE;
    /* The volume must no longer be recognized as NTFS */
    if (GetVolumeInformationW(Root, NULL, 0, NULL, NULL, NULL,
                              FileSystem, RTL_NUMBER_OF(FileSystem)) &&
        !wcscmp(FileSystem, L"NTFS"))
    {
        ok(FALSE, "%ls still mounts as NTFS after its boot sectors were invalidated\n", Root);
        return FALSE;
    }
    return TRUE;
}

static VOID
InvokeFormat(FORMAT_EX FormatEx, PWCHAR Root, ULONG ClusterSize, BOOLEAN Expected)
{
    CompletionCount = 0;
    FormatSucceeded = !Expected;
    FormatEx(Root, FMIFS_HARDDISK, L"NTFS", L"KMTNTFS", TRUE, ClusterSize, FormatCallback);
    ok(CompletionCount != 0, "Formatter did not report completion\n");
    ok(FormatSucceeded == Expected, "Formatter success %u, expected %u\n",
       FormatSucceeded, Expected);
}

START_TEST(NtfsFormat)
{
    HMODULE Module;
    FORMAT_EX FormatEx;
    WCHAR Root[4], Volume[16], Path[MAX_PATH];
    HANDLE File;
    DWORD Bytes, i, SectorsPerCluster, BytesPerSector, FreeClusters, TotalClusters;
    const CHAR Marker[] = "Formatting a busy volume must preserve existing files";
    CHAR Readback[sizeof(Marker)];
    const ULONG ClusterSizes[] = { 512, 4096 };

    if (!NtfsKmtFindFormatVolume(Root, Volume))
    {
        skip(FALSE, "No dedicated KMTNTFS format fixture\n");
        return;
    }
    Module = LoadLibraryW(L"fmifs.dll");
    ok(Module != NULL, "Cannot load fmifs.dll: %lu\n", GetLastError());
    if (Module == NULL) return;
    FormatEx = (FORMAT_EX)GetProcAddress(Module, "FormatEx");
    ok(FormatEx != NULL, "No FormatEx entry point\n");
    if (FormatEx == NULL) { FreeLibrary(Module); return; }

    swprintf(Path, RTL_NUMBER_OF(Path), L"%sKMT_FORMAT_LOCK.bin", Root);
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "Cannot create lock fixture: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) { FreeLibrary(Module); return; }
    ok(WriteFile(File, Marker, sizeof(Marker), &Bytes, NULL), "Marker write failed\n");
    ok_eq_ulong(Bytes, sizeof(Marker));
    ok(FlushFileBuffers(File), "Marker flush failed\n");
    InvokeFormat(FormatEx, Root, 4096, FALSE);
    SetFilePointer(File, 0, NULL, FILE_BEGIN);
    ok(ReadFile(File, Readback, sizeof(Readback), &Bytes, NULL), "Busy-volume read failed\n");
    ok_eq_ulong(Bytes, sizeof(Readback));
    ok(!memcmp(Readback, Marker, sizeof(Marker)), "Failed format changed existing data\n");
    CloseHandle(File);
    DeleteFileW(Path);

    /* Invalid geometry must fail before invalidating either boot sector. */
    InvokeFormat(FormatEx, Root, 513, FALSE);
    for (i = 0; i < RTL_NUMBER_OF(ClusterSizes); ++i)
    {
        InvokeFormat(FormatEx, Root, ClusterSizes[i], TRUE);
        if (!FormatSucceeded) break;
        ok(GetDiskFreeSpaceW(Root, &SectorsPerCluster, &BytesPerSector,
                             &FreeClusters, &TotalClusters), "Cannot query formatted geometry\n");
        ok_eq_ulong(SectorsPerCluster * BytesPerSector, ClusterSizes[i]);
        KmtRunKernelTest("NtfsFormat");
    }

    /* Setup formats a fresh partition, which is mounted by RAW, not by a
     * file system: the formatter must work through RAW's volume handle. */
    if (MakeVolumeRaw(Root, Volume))
    {
        WCHAR Label[64] = L"", FileSystem[32] = L"";

        InvokeFormat(FormatEx, Root, 4096, TRUE);
        ok(GetVolumeInformationW(Root, Label, RTL_NUMBER_OF(Label), NULL, NULL, NULL,
                                 FileSystem, RTL_NUMBER_OF(FileSystem)),
           "Cannot query the volume formatted from RAW: %lu\n", GetLastError());
        ok(!wcscmp(FileSystem, L"NTFS"), "Volume formatted from RAW mounts as %ls\n", FileSystem);
        ok(!wcscmp(Label, L"KMTNTFS"), "Volume formatted from RAW has label %ls\n", Label);
        if (FormatSucceeded)
            KmtRunKernelTest("NtfsFormat");
    }
    FreeLibrary(Module);
}
