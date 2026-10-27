/*
 * PROJECT:     ReactOS Service Control Manager
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        base/system/services/lock.c
 * PURPOSE:     User-side Services Start Serialization Lock functions
 * COPYRIGHT:   Copyright 2012 Hermès Bélusca
 */

/* INCLUDES *****************************************************************/

#include "services.h"

#include <time.h>
#include <winnls.h>
#include <strsafe.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* The unique user service start lock of the SCM */
static PSTART_LOCK pServiceStartLock = NULL;


/* FUNCTIONS *****************************************************************/

/*
 * NOTE: IsServiceController is TRUE if locked by the
 * Service Control Manager, and FALSE otherwise.
 */
/* The owner is reported as "Domain\\User": the RPC client that locked the
 * database, or the SCM itself while it starts services */
static VOID
ScmGetLockOwner(IN BOOL IsServiceController,
                OUT LPWSTR lpszOwner,
                IN DWORD cchOwner)
{
    WCHAR szName[MAX_PATH + 1], szDomain[MAX_PATH + 1];
    DWORD cchName = _countof(szName), cchDomain = _countof(szDomain);
    UCHAR Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PTOKEN_USER pTokenUser = (PTOKEN_USER)Buffer;
    SID_NAME_USE Use;
    HANDLE hToken;
    DWORD cbBuffer;
    BOOL bSuccess;

    StringCchCopyW(lpszOwner, cchOwner, L".\\NT Service Control Manager");
    if (IsServiceController)
        return;

    if (RpcImpersonateClient(NULL) != RPC_S_OK)
        return;
    bSuccess = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hToken);
    RpcRevertToSelf();
    if (!bSuccess)
        return;

    bSuccess = GetTokenInformation(hToken, TokenUser, Buffer, sizeof(Buffer), &cbBuffer) &&
               LookupAccountSidW(NULL, pTokenUser->User.Sid, szName, &cchName,
                                 szDomain, &cchDomain, &Use);
    CloseHandle(hToken);

    if (bSuccess)
        StringCchPrintfW(lpszOwner, cchOwner, L"%s\\%s", szDomain, szName);
}

DWORD
ScmAcquireServiceStartLock(IN BOOL IsServiceController,
                           OUT LPSC_RPC_LOCK lpLock)
{
    WCHAR szOwner[2 * (MAX_PATH + 1)];
    DWORD dwRequiredSize;
    DWORD dwError = ERROR_SUCCESS;

    *lpLock = NULL;

    ScmGetLockOwner(IsServiceController, szOwner, _countof(szOwner));

    /* Lock the service database exclusively */
    ScmLockDatabaseExclusive();

    if (pServiceStartLock != NULL)
    {
        dwError = ERROR_SERVICE_DATABASE_LOCKED;
        goto done;
    }

    /* Allocate a new lock for the database */
    dwRequiredSize = FIELD_OFFSET(START_LOCK, LockOwner) +
                     (wcslen(szOwner) + 1) * sizeof(WCHAR);

    pServiceStartLock = HeapAlloc(GetProcessHeap(),
                                  HEAP_ZERO_MEMORY,
                                  dwRequiredSize);
    if (pServiceStartLock == NULL)
    {
        dwError = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }

    pServiceStartLock->Tag = LOCK_TAG;
    pServiceStartLock->TimeWhenLocked = (DWORD)time(NULL);

    wcscpy(pServiceStartLock->LockOwner, szOwner);

    *lpLock = (LPSC_RPC_LOCK)pServiceStartLock;

done:
    /* Unlock the service database */
    ScmUnlockDatabase();

    return dwError;
}


DWORD
ScmReleaseServiceStartLock(IN OUT LPSC_RPC_LOCK lpLock)
{
    PSTART_LOCK pStartLock;
    DWORD dwError = ERROR_SUCCESS;

    if (lpLock == NULL)
        return ERROR_INVALID_SERVICE_LOCK;

    pStartLock = (PSTART_LOCK)*lpLock;

    if (pStartLock->Tag != LOCK_TAG)
        return ERROR_INVALID_SERVICE_LOCK;

    /* Lock the service database exclusively */
    ScmLockDatabaseExclusive();

    /* Release the lock handle */
    if ((pStartLock == pServiceStartLock) &&
        (pServiceStartLock != NULL))
    {
        HeapFree(GetProcessHeap(), 0, pServiceStartLock);
        pServiceStartLock = NULL;
        *lpLock = NULL;

        dwError = ERROR_SUCCESS;
    }
    else
    {
        dwError = ERROR_INVALID_SERVICE_LOCK;
    }

    /* Unlock the service database */
    ScmUnlockDatabase();

    return dwError;
}


/*
 * Helper functions for RQueryServiceLockStatusW() and
 * RQueryServiceLockStatusA().
 * We suppose that lpLockStatus points to a valid
 * well-sized buffer.
 */
DWORD
ScmQueryServiceLockStatusW(OUT LPQUERY_SERVICE_LOCK_STATUSW lpLockStatus,
                           IN DWORD cbBufSize,
                           OUT LPDWORD pcbBytesNeeded)
{
    LPCWSTR pszOwner;
    DWORD dwError = ERROR_SUCCESS;

    /* Lock the service database shared */
    ScmLockDatabaseShared();

    pszOwner = pServiceStartLock ? pServiceStartLock->LockOwner : L"";
    *pcbBytesNeeded = sizeof(QUERY_SERVICE_LOCK_STATUSW) +
                      (DWORD)(wcslen(pszOwner) + 1) * sizeof(WCHAR);
    if (cbBufSize < *pcbBytesNeeded)
    {
        dwError = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }

    /* The owner string follows the structure; RPC wants its offset */
    wcscpy((LPWSTR)(lpLockStatus + 1), pszOwner);
    lpLockStatus->lpLockOwner = (LPWSTR)(ULONG_PTR)sizeof(QUERY_SERVICE_LOCK_STATUSW);
    lpLockStatus->fIsLocked = (pServiceStartLock != NULL);
    lpLockStatus->dwLockDuration = pServiceStartLock ?
        (DWORD)time(NULL) - pServiceStartLock->TimeWhenLocked : 0;

done:
    /* Unlock the service database */
    ScmUnlockDatabase();
    return dwError;
}


DWORD
ScmQueryServiceLockStatusA(OUT LPQUERY_SERVICE_LOCK_STATUSA lpLockStatus,
                           IN DWORD cbBufSize,
                           OUT LPDWORD pcbBytesNeeded)
{
    LPCWSTR pszOwner;
    INT cbOwner;
    DWORD dwError = ERROR_SUCCESS;

    /* Lock the service database shared */
    ScmLockDatabaseShared();

    pszOwner = pServiceStartLock ? pServiceStartLock->LockOwner : L"";
    cbOwner = WideCharToMultiByte(CP_ACP, 0, pszOwner, -1, NULL, 0, NULL, NULL);
    *pcbBytesNeeded = sizeof(QUERY_SERVICE_LOCK_STATUSA) + cbOwner;
    if (cbBufSize < *pcbBytesNeeded)
    {
        dwError = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }

    /* The owner string follows the structure; RPC wants its offset */
    WideCharToMultiByte(CP_ACP, 0, pszOwner, -1, (LPSTR)(lpLockStatus + 1), cbOwner, NULL, NULL);
    lpLockStatus->lpLockOwner = (LPSTR)(ULONG_PTR)sizeof(QUERY_SERVICE_LOCK_STATUSA);
    lpLockStatus->fIsLocked = (pServiceStartLock != NULL);
    lpLockStatus->dwLockDuration = pServiceStartLock ?
        (DWORD)time(NULL) - pServiceStartLock->TimeWhenLocked : 0;

done:
    /* Unlock the service database */
    ScmUnlockDatabase();
    return dwError;
}

/* EOF */
