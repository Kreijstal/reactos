/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for DeleteProfileW
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * DeleteProfileW(Sid, NULL, NULL) takes the profile directory from the
 * ProfileImagePath of the SID's ProfileList entry, deletes the directory with
 * all it holds and removes the ProfileList entry.  Needs write access to
 * ProfileList; skipped otherwise.
 */

#include <apitest.h>
#include <winreg.h>
#include <userenv.h>
#include <strsafe.h>

#define PROFILE_LIST L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList"

/* A SID no account has: the domain part is made up. */
static const WCHAR TestSid[] = L"S-1-5-21-620620620-620620620-620620620-1001";
static const WCHAR UnknownSid[] = L"S-1-5-21-620620620-620620620-620620620-1002";

static BOOL
KeyExists(LPCWSTR SubKey)
{
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SubKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(hKey);
    return TRUE;
}

START_TEST(DeleteProfile)
{
    WCHAR TempDir[MAX_PATH], ProfileDir[MAX_PATH], SubDir[MAX_PATH], File[MAX_PATH];
    WCHAR KeyName[MAX_PATH];
    HANDLE hFile;
    HKEY hKey;
    LONG Error;
    BOOL Success;

    GetTempPathW(ARRAYSIZE(TempDir), TempDir);
    StringCchPrintfW(ProfileDir, ARRAYSIZE(ProfileDir), L"%srostest_delprof", TempDir);
    StringCchPrintfW(SubDir, ARRAYSIZE(SubDir), L"%s\\Documents", ProfileDir);
    StringCchPrintfW(File, ARRAYSIZE(File), L"%s\\file.txt", SubDir);
    StringCchPrintfW(KeyName, ARRAYSIZE(KeyName), L"%s\\%s", PROFILE_LIST, TestSid);

    /* A profile directory with something in it ... */
    CreateDirectoryW(ProfileDir, NULL);
    CreateDirectoryW(SubDir, NULL);
    hFile = CreateFileW(File, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW(%ls) failed with error %lu\n", File, GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);

    /* ... and its ProfileList entry. */
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_SET_VALUE, NULL, &hKey, NULL);
    if (Error != ERROR_SUCCESS)
    {
        skip("Cannot create %ls (%ld)\n", KeyName, Error);
        DeleteFileW(File);
        RemoveDirectoryW(SubDir);
        RemoveDirectoryW(ProfileDir);
        return;
    }
    Error = RegSetValueExW(hKey, L"ProfileImagePath", 0, REG_EXPAND_SZ, (LPBYTE)ProfileDir,
                           (lstrlenW(ProfileDir) + 1) * sizeof(WCHAR));
    ok(Error == ERROR_SUCCESS, "RegSetValueExW returned %ld\n", Error);
    RegCloseKey(hKey);

    Success = DeleteProfileW(TestSid, NULL, NULL);
    ok(Success, "DeleteProfileW(%ls) failed with error %lu\n", TestSid, GetLastError());
    ok(GetFileAttributesW(ProfileDir) == INVALID_FILE_ATTRIBUTES,
       "The profile directory '%ls' is still there\n", ProfileDir);
    ok(!KeyExists(KeyName), "The ProfileList entry of %ls is still there\n", TestSid);

    /* A SID without a profile has nothing to delete. */
    Success = DeleteProfileW(UnknownSid, NULL, NULL);
    ok(!Success, "DeleteProfileW(%ls) succeeded\n", UnknownSid);

    /* Clean up whatever is left. */
    DeleteFileW(File);
    RemoveDirectoryW(SubDir);
    RemoveDirectoryW(ProfileDir);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, KeyName);
}
