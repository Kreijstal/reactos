/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests creating named objects with a mandatory label in their SACL
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */
#include "precomp.h"

typedef BOOL (WINAPI *FN_ConvertStringSecurityDescriptorToSecurityDescriptorW)(LPCWSTR, DWORD, PSECURITY_DESCRIPTOR *, PULONG);
typedef BOOL (WINAPI *FN_OpenProcessToken)(HANDLE, DWORD, PHANDLE);
typedef BOOL (WINAPI *FN_LookupPrivilegeValueW)(LPCWSTR, LPCWSTR, PLUID);
typedef BOOL (WINAPI *FN_AdjustTokenPrivileges)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD);

static FN_ConvertStringSecurityDescriptorToSecurityDescriptorW pConvertSd;

/* Create a mutex with the given SDDL; returns the error, closes the mutex */
static DWORD
CreateLabelledMutex(PCWSTR Sddl, PCWSTR Name)
{
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, FALSE };
    HANDLE Mutex;
    DWORD Error;

    if (!pConvertSd(Sddl, 1 /* SDDL_REVISION_1 */, &Sa.lpSecurityDescriptor, NULL))
    {
        skip("Cannot convert %ls: %lu\n", Sddl, GetLastError());
        return 0xdeadbeef;
    }

    SetLastError(0xdeadbeef);
    Mutex = CreateMutexW(&Sa, FALSE, Name);
    Error = Mutex ? ERROR_SUCCESS : GetLastError();
    if (Mutex)
        CloseHandle(Mutex);
    LocalFree(Sa.lpSecurityDescriptor);
    return Error;
}

START_TEST(ObjectLabel)
{
    HMODULE Advapi32 = LoadLibraryW(L"advapi32.dll");
    FN_OpenProcessToken pOpenProcessToken;
    FN_LookupPrivilegeValueW pLookupPrivilegeValueW;
    FN_AdjustTokenPrivileges pAdjustTokenPrivileges;
    TOKEN_PRIVILEGES Privileges = { 1 };
    WCHAR Name[64];
    HANDLE Token;
    DWORD Error;

    pConvertSd = (FN_ConvertStringSecurityDescriptorToSecurityDescriptorW)
        GetProcAddress(Advapi32, "ConvertStringSecurityDescriptorToSecurityDescriptorW");
    pOpenProcessToken = (FN_OpenProcessToken)GetProcAddress(Advapi32, "OpenProcessToken");
    pLookupPrivilegeValueW = (FN_LookupPrivilegeValueW)GetProcAddress(Advapi32, "LookupPrivilegeValueW");
    pAdjustTokenPrivileges = (FN_AdjustTokenPrivileges)GetProcAddress(Advapi32, "AdjustTokenPrivileges");
    if (!pConvertSd || !pOpenProcessToken || !pLookupPrivilegeValueW || !pAdjustTokenPrivileges)
    {
        skip("advapi32 functions are missing\n");
        return;
    }

    /* Make sure SeSecurityPrivilege is not enabled, whoever runs the test */
    if (pOpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &Token))
    {
        if (pLookupPrivilegeValueW(NULL, L"SeSecurityPrivilege", &Privileges.Privileges[0].Luid))
        {
            Privileges.Privileges[0].Attributes = 0;
            pAdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL);
        }
        CloseHandle(Token);
    }

    /* A SACL with only a mandatory label needs no privilege */
    swprintf(Name, _countof(Name), L"ObjectLabelApitest-%lu-Label", GetCurrentProcessId());
    Error = CreateLabelledMutex(L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", Name);
    ok(Error == ERROR_SUCCESS, "Labelled mutex: error %lu\n", Error);

    /* Neither does an unnamed object */
    Error = CreateLabelledMutex(L"D:(A;;GA;;;WD)S:(ML;;NWNRNX;;;LW)", NULL);
    ok(Error == ERROR_SUCCESS, "Unnamed labelled mutex: error %lu\n", Error);

    /* Auditing settings do need SeSecurityPrivilege */
    swprintf(Name, _countof(Name), L"ObjectLabelApitest-%lu-Audit", GetCurrentProcessId());
    Error = CreateLabelledMutex(L"D:(A;;GA;;;WD)S:(AU;SA;GA;;;WD)", Name);
    ok(Error == ERROR_PRIVILEGE_NOT_HELD, "Audited mutex: error %lu\n", Error);

    /* And so does a label next to an audit ACE */
    swprintf(Name, _countof(Name), L"ObjectLabelApitest-%lu-Both", GetCurrentProcessId());
    Error = CreateLabelledMutex(L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)(AU;SA;GA;;;WD)", Name);
    ok(Error == ERROR_PRIVILEGE_NOT_HELD, "Labelled and audited mutex: error %lu\n", Error);
}
