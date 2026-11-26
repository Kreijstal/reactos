/*
 * PROJECT:     ReactOS Kernel Transaction Manager API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Report availability of kernel transactions
 */

#define WIN32_NO_STATUS
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);
    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);
    return TRUE;
}

HANDLE
WINAPI
CreateTransaction(LPSECURITY_ATTRIBUTES Security, LPGUID Uow,
                  DWORD CreateOptions, DWORD IsolationLevel,
                  DWORD IsolationFlags, DWORD Timeout, LPWSTR Description)
{
    UNREFERENCED_PARAMETER(Security);
    UNREFERENCED_PARAMETER(Uow);
    UNREFERENCED_PARAMETER(IsolationLevel);
    UNREFERENCED_PARAMETER(IsolationFlags);
    UNREFERENCED_PARAMETER(Timeout);
    UNREFERENCED_PARAMETER(Description);
    /* No transaction handle may be returned without atomic commit/rollback. */
    SetLastError((CreateOptions & ~1u) ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI CommitTransaction(HANDLE Transaction)
{
    SetLastError(!Transaction || Transaction == INVALID_HANDLE_VALUE
                 ? ERROR_INVALID_HANDLE : ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI RollbackTransaction(HANDLE Transaction)
{
    return CommitTransaction(Transaction);
}
