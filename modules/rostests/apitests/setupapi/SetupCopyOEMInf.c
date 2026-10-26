/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the driver store package of SetupCopyOEMInf
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>

#if (DLL_EXPORT_VERSION >= 0x600)
#include <windows.h>
#include <setupapi.h>
#include <versionhelpers.h>

static const char TestInf[] =
    "[Version]\r\n"
    "Signature=\"$Windows NT$\"\r\n"
    "Class=Ports\r\n"
    "ClassGuid={4d36e978-e325-11ce-bfc1-08002be10318}\r\n"
    "Provider=ReactOS\r\n"
    "DriverVer=01/01/2026,1.0.0.0\r\n"
    "\r\n"
    "[SourceDisksNames]\r\n"
    "1=%Disk%,,,\r\n"
    "2=%Disk%,,,sub\r\n"
    "\r\n"
    "[SourceDisksFiles]\r\n"
    "apitest1.txt=1\r\n"
    "apitest2.txt=2\r\n"
    "apitest3.txt=1,deeper\r\n"
    "\r\n"
    "[Strings]\r\n"
    "Disk=\"Apitest disk\"\r\n";

static
BOOL
WriteTestFile(
    _In_ PCWSTR Path,
    _In_ const VOID *Data,
    _In_ DWORD Size)
{
    HANDLE File;
    DWORD Written;
    BOOL Ret;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = WriteFile(File, Data, Size, &Written, NULL) && Written == Size;
    CloseHandle(File);
    return Ret;
}

static
BOOL
PackageHasFile(
    _In_ PCWSTR Directory,
    _In_ PCWSTR Name)
{
    WCHAR Path[MAX_PATH];

    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s", Directory, Name);
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

START_TEST(SetupCopyOEMInf)
{
    WCHAR Temp[MAX_PATH], Dir[MAX_PATH], Path[MAX_PATH], Dest[MAX_PATH], Store[MAX_PATH];
    PWSTR Component = NULL, Slash;
    DWORD Required;
    BOOL Ret;

    /* Nothing is published under that name */
    SetLastError(0xdeadbeef);
    Ret = SetupGetInfDriverStoreLocationW(L"oem99999.inf", NULL, NULL, Store, ARRAYSIZE(Store), NULL);
    ok(!Ret, "Found a package for oem99999.inf\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "LastError %lu\n", GetLastError());

    /* Publishing a package changes the driver store of the machine */
    if (!IsReactOS())
    {
        skip("Not adding a driver package to this machine\n");
        return;
    }

    /* A package in a directory of its own */
    GetTempPathW(ARRAYSIZE(Temp), Temp);
    _snwprintf(Dir, ARRAYSIZE(Dir), L"%sApitestOemInf", Temp);
    CreateDirectoryW(Dir, NULL);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\sub", Dir);
    CreateDirectoryW(Path, NULL);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\deeper", Dir);
    CreateDirectoryW(Path, NULL);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\apitest1.txt", Dir);
    ok(WriteTestFile(Path, "1", 1), "Cannot write %ls\n", Path);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\sub\\apitest2.txt", Dir);
    ok(WriteTestFile(Path, "2", 1), "Cannot write %ls\n", Path);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\deeper\\apitest3.txt", Dir);
    ok(WriteTestFile(Path, "3", 1), "Cannot write %ls\n", Path);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\apitest.inf", Dir);
    ok(WriteTestFile(Path, TestInf, sizeof(TestInf) - 1), "Cannot write %ls\n", Path);

    Ret = SetupCopyOEMInfW(Path, NULL, SPOST_NONE, 0, Dest, ARRAYSIZE(Dest), NULL, &Component);
    ok(Ret, "SetupCopyOEMInfW failed: %lu\n", GetLastError());

    /* The installer deletes its copy, as Wintun does */
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\apitest1.txt", Dir);
    DeleteFileW(Path);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\sub\\apitest2.txt", Dir);
    DeleteFileW(Path);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\deeper\\apitest3.txt", Dir);
    DeleteFileW(Path);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\apitest.inf", Dir);
    DeleteFileW(Path);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\sub", Dir);
    RemoveDirectoryW(Path);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\deeper", Dir);
    RemoveDirectoryW(Path);
    RemoveDirectoryW(Dir);
    if (!Ret || !Component)
        return;

    /* The package keeps the files, laid out as they were */
    Required = 0;
    Ret = SetupGetInfDriverStoreLocationW(Component, NULL, NULL, NULL, 0, &Required);
    ok(Ret, "Size query failed: %lu\n", GetLastError());
    ZeroMemory(Store, sizeof(Store));
    Ret = SetupGetInfDriverStoreLocationW(Component, NULL, NULL, Store, ARRAYSIZE(Store), NULL);
    ok(Ret, "SetupGetInfDriverStoreLocationW(%ls) failed: %lu\n", Component, GetLastError());
    ok(Required == wcslen(Store) + 1, "Required %lu for %ls\n", Required, Store);
    ok(GetFileAttributesW(Store) != INVALID_FILE_ATTRIBUTES, "No %ls\n", Store);
    Slash = wcsrchr(Store, L'\\');
    ok(Slash && !_wcsicmp(Slash + 1, L"apitest.inf"), "Store INF %ls\n", Store);
    if (Slash)
    {
        *Slash = UNICODE_NULL;
        ok(PackageHasFile(Store, L"apitest1.txt"), "No apitest1.txt in %ls\n", Store);
        ok(PackageHasFile(Store, L"sub\\apitest2.txt"), "No sub\\apitest2.txt in %ls\n", Store);
        ok(PackageHasFile(Store, L"deeper\\apitest3.txt"), "No deeper\\apitest3.txt in %ls\n", Store);
    }

    /* Removing the published INF removes the package */
    ok(SetupUninstallOEMInfW(Component, SUOI_FORCEDELETE, NULL), "SetupUninstallOEMInfW failed: %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!SetupGetInfDriverStoreLocationW(Component, NULL, NULL, Path, ARRAYSIZE(Path), NULL),
       "The package of %ls is still there\n", Component);
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "LastError %lu\n", GetLastError());
    if (Slash)
        ok(GetFileAttributesW(Store) == INVALID_FILE_ATTRIBUTES, "%ls is still there\n", Store);
}

#else

START_TEST(SetupCopyOEMInf)
{
    skip("SetupCopyOEMInf requires NT 6.0 or later\n");
}

#endif
