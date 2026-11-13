/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that the start menu is created when the user's Start Menu folder is missing
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "shelltest.h"
#include <shlguid_undoc.h>

#define USF_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders"

typedef VOID (WINAPI *FN_SHFlushSFCache)(VOID);

static BOOL GetValue(HKEY hKey, PCWSTR Name, PWSTR Buffer, DWORD cb, PDWORD Type)
{
    return RegQueryValueExW(hKey, Name, NULL, Type, (PBYTE)Buffer, &cb) == ERROR_SUCCESS;
}

static void SetOrDelete(HKEY hKey, PCWSTR Name, BOOL Had, PCWSTR Value, DWORD Type)
{
    if (Had)
        RegSetValueExW(hKey, Name, 0, Type, (const BYTE *)Value, (DWORD)(wcslen(Value) + 1) * sizeof(WCHAR));
    else
        RegDeleteValueW(hKey, Name);
}

START_TEST(StartMenu)
{
    HKEY hKey;
    WCHAR OldStart[MAX_PATH], OldPrograms[MAX_PATH], Root[MAX_PATH], StartDir[MAX_PATH], ProgramsDir[MAX_PATH];
    DWORD StartType = REG_EXPAND_SZ, ProgramsType = REG_EXPAND_SZ;
    WCHAR OldCommonStart[MAX_PATH], OldCommonPrograms[MAX_PATH], CommonStartDir[MAX_PATH];
    DWORD CommonStartType = REG_EXPAND_SZ, CommonProgramsType = REG_EXPAND_SZ;
    BOOL HadStart, HadPrograms, HadCommonStart = FALSE, HadCommonPrograms = FALSE;
    HKEY hKeyCommon = NULL;
    FN_SHFlushSFCache pSHFlushSFCache;
    IMenuPopup *pMenuPopup = NULL;
    HRESULT hr;

    CoInitialize(NULL);

    pSHFlushSFCache = (FN_SHFlushSFCache)GetProcAddress(GetModuleHandleW(L"shell32.dll"), MAKEINTRESOURCEA(526));
    if (!pSHFlushSFCache)
    {
        skip("SHFlushSFCache not found\n");
        CoUninitialize();
        return;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, USF_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
    {
        skip("Cannot open User Shell Folders\n");
        CoUninitialize();
        return;
    }

    /* Point the user's Start Menu and Programs folders to a place that does not exist */
    GetTempPathW(_countof(Root), Root);
    wcscat(Root, L"StartMenuApitest");
    wsprintfW(StartDir, L"%s\\Start Menu", Root);
    wsprintfW(ProgramsDir, L"%s\\Start Menu\\Programs", Root);
    RemoveDirectoryW(ProgramsDir);
    RemoveDirectoryW(StartDir);
    RemoveDirectoryW(Root);
    ok(GetFileAttributesW(StartDir) == INVALID_FILE_ATTRIBUTES, "%S exists\n", StartDir);

    /* Also make the common Start Menu folder missing, if we may */
    wsprintfW(CommonStartDir, L"%s\\Common Start Menu", Root);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, USF_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKeyCommon) == ERROR_SUCCESS)
    {
        WCHAR CommonProgramsDir[MAX_PATH];
        wsprintfW(CommonProgramsDir, L"%s\\Programs", CommonStartDir);
        HadCommonStart = GetValue(hKeyCommon, L"Common Start Menu", OldCommonStart, sizeof(OldCommonStart), &CommonStartType);
        HadCommonPrograms = GetValue(hKeyCommon, L"Common Programs", OldCommonPrograms, sizeof(OldCommonPrograms), &CommonProgramsType);
        RegSetValueExW(hKeyCommon, L"Common Start Menu", 0, REG_SZ, (const BYTE *)CommonStartDir, (DWORD)(wcslen(CommonStartDir) + 1) * sizeof(WCHAR));
        RegSetValueExW(hKeyCommon, L"Common Programs", 0, REG_SZ, (const BYTE *)CommonProgramsDir, (DWORD)(wcslen(CommonProgramsDir) + 1) * sizeof(WCHAR));
    }
    else
    {
        trace("Cannot change the common folders, only the user's are missing\n");
    }

    HadStart = GetValue(hKey, L"Start Menu", OldStart, sizeof(OldStart), &StartType);
    HadPrograms = GetValue(hKey, L"Programs", OldPrograms, sizeof(OldPrograms), &ProgramsType);
    RegSetValueExW(hKey, L"Start Menu", 0, REG_SZ, (const BYTE *)StartDir, (DWORD)(wcslen(StartDir) + 1) * sizeof(WCHAR));
    RegSetValueExW(hKey, L"Programs", 0, REG_SZ, (const BYTE *)ProgramsDir, (DWORD)(wcslen(ProgramsDir) + 1) * sizeof(WCHAR));
    pSHFlushSFCache();

    /* Like Windows, the start menu is still created, with the user's folders */
    hr = CoCreateInstance(CLSID_StartMenu, NULL, CLSCTX_INPROC_SERVER, IID_IMenuPopup, (void **)&pMenuPopup);
    ok_hr(hr, S_OK);
    if (pMenuPopup)
        pMenuPopup->Release();
    ok(GetFileAttributesW(StartDir) != INVALID_FILE_ATTRIBUTES, "%S was not created\n", StartDir);
    ok(GetFileAttributesW(ProgramsDir) != INVALID_FILE_ATTRIBUTES, "%S was not created\n", ProgramsDir);

    /* Restore */
    SetOrDelete(hKey, L"Start Menu", HadStart, OldStart, StartType);
    SetOrDelete(hKey, L"Programs", HadPrograms, OldPrograms, ProgramsType);
    RegCloseKey(hKey);
    if (hKeyCommon)
    {
        SetOrDelete(hKeyCommon, L"Common Start Menu", HadCommonStart, OldCommonStart, CommonStartType);
        SetOrDelete(hKeyCommon, L"Common Programs", HadCommonPrograms, OldCommonPrograms, CommonProgramsType);
        RegCloseKey(hKeyCommon);
    }
    pSHFlushSFCache();
    RemoveDirectoryW(ProgramsDir);
    RemoveDirectoryW(StartDir);
    RemoveDirectoryW(Root);
    CoUninitialize();
}
