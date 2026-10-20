/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (see COPYING.LIB)
 * PURPOSE:     Public offline NTFS check/repair on a dedicated test volume
 */
#include <kmt_test.h>
#include <winioctl.h>
#include <fmifs/fmifs.h>

#include "NtfsFslib_user.h"
typedef VOID (NTAPI *FORMAT_EX)(PWCHAR, FMIFS_MEDIA_FLAG, PWCHAR, PWCHAR,
                               BOOLEAN, ULONG, PFMIFSCALLBACK);
typedef VOID (NTAPI *CHECK_VOLUME)(PWCHAR, PWCHAR, BOOLEAN, BOOLEAN, BOOLEAN,
                                  BOOLEAN, PVOID, PVOID, PFMIFSCALLBACK);
static ULONG CompletionCount, OutputCount, CheckStatus, BusyCount;
static BOOLEAN CheckCompleted;

static BOOLEAN NTAPI
CheckCallback(CALLBACKCOMMAND Command, ULONG SubAction, PVOID Argument)
{
    if (Command == DONE)
    {
        CheckCompleted = *(PBOOLEAN)Argument;
        CheckStatus = SubAction;
        ++CompletionCount;
    }
    else if (Command == OUTPUT)
        ++OutputCount;
    else if (Command == VOLUMEINUSE)
        ++BusyCount;
    return TRUE;
}

static VOID
InvokeCheck(CHECK_VOLUME Check, PWCHAR Root, BOOLEAN Repair, BOOLEAN DirtyOnly,
             BOOLEAN Completed, NTSTATUS Expected)
{
    CompletionCount = OutputCount = BusyCount = 0;
    CheckCompleted = !Completed;
    CheckStatus = ~Expected;
    Check(Root, L"NTFS", Repair, TRUE, DirtyOnly, FALSE, NULL, NULL, CheckCallback);
    ok_eq_ulong(CompletionCount, 1);
    ok(CheckCompleted == Completed, "Check completed %u, expected %u\n", CheckCompleted, Completed);
    ok_eq_hex(CheckStatus, Expected);
}

static ULONG
GetDirtyFlags(PWSTR Volume)
{
    HANDLE Handle;
    DWORD Bytes, Flags = MAXULONG;
    Handle = CreateFileW(Volume, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Cannot open volume: %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE) return Flags;
    ok(DeviceIoControl(Handle, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &Flags,
                       sizeof(Flags), &Bytes, NULL), "Cannot query dirty flag: %lu\n", GetLastError());
    CloseHandle(Handle);
    return Flags;
}

/* Exercise repair of a log-gated read-only mount too. Only the two restart
 * pages of this freshly formatted, dedicated fixture are changed. */
static BOOLEAN
MakeLogDirty(PWSTR Volume)
{
    HANDLE Handle;
    NTFS_VOLUME_DATA_BUFFER Geometry;
    NTFS_FILE_RECORD_INPUT_BUFFER Input;
    PNTFS_FILE_RECORD_OUTPUT_BUFFER Output = NULL;
    PUCHAR Record, Attr, Run, Pages = NULL;
    ULONG OutputSize, Position, Type, Length, i, LengthBytes, OffsetBytes;
    USHORT AttributeOffset, MappingOffset, RestartOffset;
    ULONGLONG Lcn = 0, Clusters = 0;
    LARGE_INTEGER Offset;
    DWORD Bytes;
    BOOLEAN Success = FALSE, Locked = FALSE;

    Handle = CreateFileW(Volume, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (!ok(Handle != INVALID_HANDLE_VALUE, "Cannot open log fixture\n")) return FALSE;
    if (!ok(DeviceIoControl(Handle, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                             &Geometry, sizeof(Geometry), &Bytes, NULL), "Cannot query geometry\n"))
        goto Cleanup;
    OutputSize = FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) +
                 Geometry.BytesPerFileRecordSegment;
    Output = HeapAlloc(GetProcessHeap(), 0, OutputSize);
    Pages = HeapAlloc(GetProcessHeap(), 0, 8192);
    if (!ok(Output != NULL && Pages != NULL, "Cannot allocate log fixture\n")) goto Cleanup;
    Input.FileReferenceNumber.QuadPart = 2;
    if (!ok(DeviceIoControl(Handle, FSCTL_GET_NTFS_FILE_RECORD, &Input, sizeof(Input),
                             Output, OutputSize, &Bytes, NULL), "Cannot locate $LogFile\n"))
        goto Cleanup;
    if (!ok(Output->FileRecordLength >= 24 &&
            Output->FileRecordLength <= Geometry.BytesPerFileRecordSegment,
            "Invalid $LogFile record length\n")) goto Cleanup;
    Record = Output->FileRecordBuffer;
    memcpy(&AttributeOffset, Record + 20, sizeof(AttributeOffset));
    for (Position = AttributeOffset; Position <= Output->FileRecordLength - 8; Position += Length)
    {
        Attr = Record + Position;
        memcpy(&Type, Attr, sizeof(Type));
        memcpy(&Length, Attr + 4, sizeof(Length));
        if (Type == MAXULONG || Length < 16 || Length > Output->FileRecordLength - Position) break;
        if (Type != 0x80 || Attr[9] != 0) continue;
        if (!Attr[8] || Length < 64) break;
        memcpy(&MappingOffset, Attr + 32, sizeof(MappingOffset));
        if (MappingOffset >= Length) break;
        Run = Attr + MappingOffset;
        LengthBytes = Run[0] & 15;
        OffsetBytes = Run[0] >> 4;
        if (!LengthBytes || LengthBytes > 8 || !OffsetBytes || OffsetBytes > 8 ||
            LengthBytes + OffsetBytes + 1 > Length - MappingOffset ||
            (Run[LengthBytes + OffsetBytes] & 0x80)) break;
        for (i = 0; i < LengthBytes; ++i) Clusters |= (ULONGLONG)Run[1 + i] << (8 * i);
        for (i = 0; i < OffsetBytes; ++i) Lcn |= (ULONGLONG)Run[1 + LengthBytes + i] << (8 * i);
        break;
    }
    if (!ok(Lcn != 0 && Lcn < Geometry.TotalClusters.QuadPart &&
            Clusters * Geometry.BytesPerCluster >= 8192 &&
            (Geometry.TotalClusters.QuadPart - Lcn) * Geometry.BytesPerCluster >= 8192,
            "Invalid contiguous log fixture location\n")) goto Cleanup;
    Locked = DeviceIoControl(Handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    if (!ok(Locked, "Cannot lock log fixture: %lu\n", GetLastError())) goto Cleanup;
    if (!ok(DeviceIoControl(Handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL),
            "Cannot dismount log fixture\n")) goto Cleanup;
    Offset.QuadPart = Lcn * Geometry.BytesPerCluster;
    if (!ok(SetFilePointerEx(Handle, Offset, NULL, FILE_BEGIN) &&
            ReadFile(Handle, Pages, 8192, &Bytes, NULL) && Bytes == 8192,
            "Cannot read restart pair\n")) goto Cleanup;
    for (i = 0; i < 8192; i += 4096)
    {
        if (!ok(!memcmp(Pages + i, "RSTR", 4), "Missing restart signature\n")) goto Cleanup;
        memcpy(&RestartOffset, Pages + i + 0x18, sizeof(RestartOffset));
        if (!ok(RestartOffset <= 4096 - 16, "Invalid restart offset\n")) goto Cleanup;
        /* This flag precedes the sector tail; its USA saved word is unchanged. */
        Pages[i + RestartOffset + 14] &= ~2;
    }
    if (!ok(SetFilePointerEx(Handle, Offset, NULL, FILE_BEGIN) &&
            WriteFile(Handle, Pages, 8192, &Bytes, NULL) && Bytes == 8192,
            "Cannot write dirty restart pair\n")) goto Cleanup;
    Success = FlushFileBuffers(Handle);
    ok(Success, "Cannot flush dirty restart pair\n");
Cleanup:
    if (Locked) DeviceIoControl(Handle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    if (Output != NULL) HeapFree(GetProcessHeap(), 0, Output);
    if (Pages != NULL) HeapFree(GetProcessHeap(), 0, Pages);
    CloseHandle(Handle);
    return Success;
}

START_TEST(NtfsChkdsk)
{
    WCHAR Root[4], Volume[16], Path[MAX_PATH];
    HMODULE Module;
    FORMAT_EX FormatEx;
    CHECK_VOLUME Check;
    HANDLE File, Handle;
    DWORD Bytes, Flags;
    const CHAR Marker[] = "Offline repair preserves ordinary file contents";
    CHAR Readback[sizeof(Marker)];

    if (!NtfsKmtFindFormatVolume(Root, Volume))
    {
        skip(FALSE, "No dedicated KMTNTFS check/repair fixture\n");
        return;
    }
    Module = LoadLibraryW(L"fmifs.dll");
    ok(Module != NULL, "Cannot load fmifs.dll\n");
    if (Module == NULL) return;
    FormatEx = (FORMAT_EX)GetProcAddress(Module, "FormatEx");
    Check = (CHECK_VOLUME)GetProcAddress(Module, "Chkdsk");
    ok(FormatEx != NULL && Check != NULL, "Missing public format/check entry point\n");
    if (FormatEx == NULL || Check == NULL) goto Cleanup;
    CheckCompleted = FALSE;
    FormatEx(Root, FMIFS_HARDDISK, L"NTFS", L"KMTNTFS", TRUE, 4096, CheckCallback);
    ok(CheckCompleted, "Initial format failed\n");
    if (!CheckCompleted) goto Cleanup;
    InvokeCheck(Check, Root, FALSE, FALSE, TRUE, STATUS_SUCCESS);
    ok(OutputCount >= 4, "Missing check stages/summary: %lu messages\n", OutputCount);
    ok_eq_ulong(GetDirtyFlags(Volume) & VOLUME_IS_DIRTY, 0);

    swprintf(Path, RTL_NUMBER_OF(Path), L"%sKMT_CHKDSK_DATA.bin", Root);
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "Cannot create repair fixture: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) goto Cleanup;
    ok(WriteFile(File, Marker, sizeof(Marker), &Bytes, NULL), "Marker write failed\n");
    ok_eq_ulong(Bytes, sizeof(Marker));
    ok(FlushFileBuffers(File), "Marker flush failed\n");
    Handle = CreateFileW(Volume, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Cannot open volume for dirty mark\n");
    if (Handle == INVALID_HANDLE_VALUE) { CloseHandle(File); goto Cleanup; }
    ok(DeviceIoControl(Handle, FSCTL_MARK_VOLUME_DIRTY, NULL, 0, NULL, 0, &Bytes, NULL),
       "Cannot mark volume dirty: %lu\n", GetLastError());
    CloseHandle(Handle);
    InvokeCheck(Check, Root, FALSE, FALSE, TRUE, STATUS_DISK_CORRUPT_ERROR);
    ok(GetDirtyFlags(Volume) & VOLUME_IS_DIRTY, "Read-only check cleared dirty flag\n");
    InvokeCheck(Check, Root, TRUE, FALSE, FALSE, STATUS_ACCESS_DENIED);
    ok_eq_ulong(BusyCount, 1);
    ok(GetDirtyFlags(Volume) & VOLUME_IS_DIRTY, "Busy repair cleared dirty flag\n");
    SetFilePointer(File, 0, NULL, FILE_BEGIN);
    ok(ReadFile(File, Readback, sizeof(Readback), &Bytes, NULL), "Busy repair changed live file\n");
    ok_eq_ulong(Bytes, sizeof(Readback));
    ok(!memcmp(Readback, Marker, sizeof(Marker)), "Busy repair changed data\n");
    CloseHandle(File);

    if (!MakeLogDirty(Volume)) goto Cleanup;
    ok(GetVolumeInformationW(Root, NULL, 0, NULL, NULL, &Flags, NULL, 0),
       "Cannot query log-gated mount\n");
    ok(Flags & FILE_READ_ONLY_VOLUME, "Dirty restart did not gate the mount\n");
    InvokeCheck(Check, Root, TRUE, FALSE, TRUE, STATUS_SUCCESS);
    ok(GetVolumeInformationW(Root, NULL, 0, NULL, NULL, &Flags, NULL, 0),
       "Cannot query repaired mount\n");
    ok(!(Flags & FILE_READ_ONLY_VOLUME), "Repaired log still gates writes\n");
    ok_eq_ulong(GetDirtyFlags(Volume) & VOLUME_IS_DIRTY, 0);
    InvokeCheck(Check, Root, FALSE, FALSE, TRUE, STATUS_SUCCESS);
    InvokeCheck(Check, Root, FALSE, TRUE, TRUE, STATUS_SUCCESS);
    ok_eq_ulong(OutputCount, 0);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "Repair lost file: %lu\n", GetLastError());
    if (File != INVALID_HANDLE_VALUE)
    {
        ok(ReadFile(File, Readback, sizeof(Readback), &Bytes, NULL), "Repaired file read failed\n");
        ok_eq_ulong(Bytes, sizeof(Readback));
        ok(!memcmp(Readback, Marker, sizeof(Marker)), "Repair changed data\n");
        CloseHandle(File);
    }
    DeleteFileW(Path);
    KmtRunKernelTest("NtfsChkdsk");
Cleanup:
    FreeLibrary(Module);
}
