/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Tests for GetVolumeInformation
 * PROGRAMMER:      Pierre Schweitzer <pierre@reactos.org>
 */

#include "precomp.h"

static VOID
TestGetVolumeInformationA(VOID)
{
    BOOL Ret;
    CHAR Outbuf[MAX_PATH];
    DWORD i, MCL, Flags, Len;

    memset(Outbuf, 0xAA, MAX_PATH);
    Ret = GetVolumeInformationA("C:\\", NULL, 0, NULL, &MCL, &Flags, Outbuf, MAX_PATH);
    ok(Ret != FALSE, "GetVolumeInformationA failed: %ld\n", GetLastError());
    for (i = 0; i < MAX_PATH; ++i)
    {
        if (Outbuf[i] == 0)
        {
            break;
        }
    }
    ok(i != MAX_PATH, "String was not null terminated!\n");

    memset(Outbuf, 0xAA, MAX_PATH);
    Len = i;
    Ret = GetVolumeInformationA("C:\\", NULL, 0, NULL, &MCL, &Flags, Outbuf, Len + 1);
    ok(Ret != FALSE, "GetVolumeInformationA failed: %ld\n", GetLastError());
    for (i = 0; i < MAX_PATH; ++i)
    {
        if (Outbuf[i] == 0)
        {
            break;
        }
    }
    ok(i != MAX_PATH, "String was not null terminated!\n");
    ok(i == Len, "String was truncated\n");

    memset(Outbuf, 0xAA, MAX_PATH);
    Len = i;
    SetLastError(0xdeadbeef);
    Ret = GetVolumeInformationA("C:\\", NULL, 0, NULL, &MCL, &Flags, Outbuf, Len);
    ok(Ret != TRUE, "GetVolumeInformationA succeed\n");
    ok(GetLastError() == ERROR_BAD_LENGTH, "Expected ERROR_BAD_LENGTH error, got %ld\n", GetLastError());
    ok(Outbuf[0] != 0xAA, "Output buffer was not written to\n");
    for (i = 0; i < MAX_PATH; ++i)
    {
        if (Outbuf[i] == 0)
        {
            break;
        }
    }
    ok(i == MAX_PATH, "String was null terminated!\n");
    for (i = 0; i < MAX_PATH; ++i)
    {
        if (Outbuf[i] != 0xAA)
        {
            break;
        }
    }
    ok(i != MAX_PATH, "String was not written to!\n");
    ok(i < Len, "Buffer has been overruned\n");
}

static VOID
TestGetVolumeInformationW(VOID)
{
    BOOL Ret;
    WCHAR Outbuf[MAX_PATH];
    DWORD i, MCL, Flags, Len;

    memset(Outbuf, 0xAA, sizeof(Outbuf));
    Ret = GetVolumeInformationW(L"C:\\", NULL, 0, NULL, &MCL, &Flags, Outbuf, MAX_PATH);
    ok(Ret != FALSE, "GetVolumeInformationW failed: %ld\n", GetLastError());
    for (i = 0; i < MAX_PATH; ++i)
    {
        if (Outbuf[i] == 0)
        {
            break;
        }
    }
    ok(i != MAX_PATH, "String was not null terminated!\n");

    memset(Outbuf, 0xAA, sizeof(Outbuf));
    Len = i;
    Ret = GetVolumeInformationW(L"C:\\", NULL, 0, NULL, &MCL, &Flags, Outbuf, Len + 1);
    ok(Ret != FALSE, "GetVolumeInformationW failed: %ld\n", GetLastError());
    for (i = 0; i < MAX_PATH; ++i)
    {
        if (Outbuf[i] == 0)
        {
            break;
        }
    }
    ok(i != MAX_PATH, "String was not null terminated!\n");
    ok(i == Len, "String was truncated\n");

    memset(Outbuf, 0xAA, sizeof(Outbuf));
    Len = i;
    SetLastError(0xdeadbeef);
    Ret = GetVolumeInformationW(L"C:\\", NULL, 0, NULL, &MCL, &Flags, Outbuf, Len);
    ok(Ret != TRUE, "GetVolumeInformationW succeed\n");
    ok(GetLastError() == ERROR_BAD_LENGTH, "Expected ERROR_BAD_LENGTH error, got %ld\n", GetLastError());
    ok(Outbuf[0] != 0xAA, "Output buffer was not written to\n");
    for (i = 0; i < MAX_PATH; ++i)
    {
        if (Outbuf[i] == 0)
        {
            break;
        }
    }
    ok(i == MAX_PATH, "String was null terminated!\n");
    ok(i >= Len, "Buffer has not been overrun\n");
}

/* The root of a volume named by its device, as mountvol and partition
 * tools name volumes without a drive letter */
static void TestGetVolumeInformationGlobalRoot(void)
{
    WCHAR Drive[] = L"C:", Device[MAX_PATH], Path[MAX_PATH + 32];
    WCHAR FsName[MAX_PATH], FsName2[MAX_PATH];
    DWORD Serial = 0, Serial2 = 0;
    BOOL Ret;

    GetWindowsDirectoryW(Path, _countof(Path));
    Drive[0] = Path[0];
    if (!QueryDosDeviceW(Drive, Device, _countof(Device)))
    {
        skip("QueryDosDeviceW(%ls) failed: %lu\n", Drive, GetLastError());
        return;
    }
    _snwprintf(Path, _countof(Path), L"%ls\\", Drive);
    Ret = GetVolumeInformationW(Path, NULL, 0, &Serial, NULL, NULL, FsName, _countof(FsName));
    ok(Ret, "GetVolumeInformationW(%ls) failed: %lu\n", Path, GetLastError());

    _snwprintf(Path, _countof(Path), L"\\\\?\\GLOBALROOT%ls\\", Device);
    SetLastError(0xdeadbeef);
    Ret = GetVolumeInformationW(Path, NULL, 0, &Serial2, NULL, NULL, FsName2, _countof(FsName2));
    ok(Ret, "GetVolumeInformationW(%ls) failed: %lu\n", Path, GetLastError());
    if (Ret)
    {
        ok(Serial2 == Serial, "Serial %lx, expected %lx\n", Serial2, Serial);
        ok(!wcscmp(FsName2, FsName), "File system %ls, expected %ls\n", FsName2, FsName);
    }

    /* Not a root: a directory of the volume */
    GetWindowsDirectoryW(FsName2, _countof(FsName2));
    _snwprintf(Path, _countof(Path), L"\\\\?\\GLOBALROOT%ls%ls\\", Device, FsName2 + 2);
    SetLastError(0xdeadbeef);
    Ret = GetVolumeInformationW(Path, NULL, 0, NULL, NULL, NULL, NULL, 0);
    ok(!Ret, "GetVolumeInformationW(%ls) succeeded\n", Path);
    ok(GetLastError() == ERROR_DIR_NOT_ROOT, "Expected ERROR_DIR_NOT_ROOT, got %lu\n", GetLastError());
}

START_TEST(GetVolumeInformation)
{
    TestGetVolumeInformationW();
    TestGetVolumeInformationA();
    TestGetVolumeInformationGlobalRoot();
}
