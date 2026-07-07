/*
 * Wireless LAN API (wlanapi.dll) -- notification delivery
 *
 * Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * One callback per client handle: the first registration parks a worker
 * thread in _RpcAsyncGetNotification which dispatches each delivered
 * WLAN_NOTIFICATION_DATA to the callback.  Setting the source to NONE or
 * closing the handle stops the worker.
 *
 * The getter is a *synchronous* RPC call that blocks in the service until an
 * event is queued.  The RPC runtime serializes concurrent calls that share a
 * [context_handle] behind a per-handle lock held for the whole call, so a
 * getter parked on the application's handle would deadlock every later call
 * on it (WlanScan, WlanConnect, ...).  To keep the notification channel from
 * blocking control calls -- the isolation Windows gets from a true [async]
 * RPC getter -- each registration opens its *own* dedicated service handle and
 * parks the getter on that; the application handle is never used by the getter.
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <wlansvc_c.h>
#include "wlanapi_local.h"

#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(wlanapi);

/* One registration per opened client handle. */
typedef struct _WLAN_NOTIF_REG
{
    struct _WLAN_NOTIF_REG     *Next;
    HANDLE                      hClientHandle;  /* application handle (key only) */
    HANDLE                      hNotifHandle;   /* dedicated handle for the getter */
    DWORD                       dwSource;
    WLAN_NOTIFICATION_CALLBACK  Callback;
    PVOID                       Context;
    HANDLE                      hThread;
    volatile LONG               Running;
} WLAN_NOTIF_REG, *PWLAN_NOTIF_REG;

static PWLAN_NOTIF_REG   NotifListHead = NULL;
static CRITICAL_SECTION  NotifLock;
static LONG              NotifInit = 0;

static VOID
WlanNotifEnsureInit(VOID)
{
    if (InterlockedCompareExchange(&NotifInit, 1, 0) == 0)
    {
        InitializeCriticalSection(&NotifLock);
        NotifListHead = NULL;
    }
}

/* Lock held by caller. */
static PWLAN_NOTIF_REG
WlanNotifFind(HANDLE hClientHandle)
{
    PWLAN_NOTIF_REG reg;

    for (reg = NotifListHead; reg != NULL; reg = reg->Next)
    {
        if (reg->hClientHandle == hClientHandle)
            return reg;
    }
    return NULL;
}

/* Lock held by caller. */
static VOID
WlanNotifUnlink(PWLAN_NOTIF_REG target)
{
    PWLAN_NOTIF_REG *pp = &NotifListHead;

    while (*pp != NULL)
    {
        if (*pp == target)
        {
            *pp = target->Next;
            target->Next = NULL;
            return;
        }
        pp = &(*pp)->Next;
    }
}

/*
 * Worker: drains notifications from the service and dispatches the callback.
 * The teardown path joins this thread before freeing reg; the parked async
 * call returns non-SUCCESS once the source is NONE or the handle is closed.
 */
static DWORD WINAPI
WlanNotifThread(LPVOID lpParameter)
{
    PWLAN_NOTIF_REG reg = (PWLAN_NOTIF_REG)lpParameter;

    for (;;)
    {
        PWLAN_NOTIFICATION_DATA pData = NULL;
        DWORD dwResult = ERROR_SUCCESS;

        RpcTryExcept
        {
            dwResult = _RpcAsyncGetNotification(reg->hNotifHandle, &pData);
        }
        RpcExcept(EXCEPTION_EXECUTE_HANDLER)
        {
            dwResult = RpcExceptionCode();
        }
        RpcEndExcept;

        if (dwResult != ERROR_SUCCESS || pData == NULL)
        {
            /* Source set to NONE, handle closed, or RPC failure -> stop. */
            if (pData != NULL)
                WlanFreeMemory(pData);
            break;
        }

        /* Skip dispatch if a teardown has started meanwhile. */
        if (InterlockedCompareExchange(&reg->Running, 1, 1) && reg->Callback != NULL)
            reg->Callback(pData, reg->Context);

        if (pData->pData != NULL)
            WlanFreeMemory(pData->pData);
        WlanFreeMemory(pData);
    }

    return 0;
}

/*
 * Release the parked getter, join the worker, close the dedicated service
 * handle, then free reg.  Caller must NOT hold NotifLock; reg must already
 * be unlinked.
 */
static VOID
WlanNotifTeardown(PWLAN_NOTIF_REG reg)
{
    InterlockedExchange(&reg->Running, 0);
    reg->Callback = NULL;

    /* Setting the dedicated handle's source to NONE makes the service release
     * the parked getter so the worker can observe the state and return. */
    if (reg->hNotifHandle != NULL)
    {
        DWORD dwPrev = 0;
        RpcTryExcept
        {
            (void)_RpcRegisterNotification(reg->hNotifHandle,
                                           WLAN_NOTIFICATION_SOURCE_NONE,
                                           &dwPrev);
        }
        RpcExcept(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        RpcEndExcept;
    }

    if (reg->hThread != NULL)
    {
        WaitForSingleObject(reg->hThread, INFINITE);
        CloseHandle(reg->hThread);
    }

    /* The getter has returned, so the dedicated handle is idle: close it. */
    if (reg->hNotifHandle != NULL)
    {
        HANDLE h = reg->hNotifHandle;
        RpcTryExcept
        {
            (void)_RpcCloseHandle(&h);
        }
        RpcExcept(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        RpcEndExcept;
    }

    HeapFree(GetProcessHeap(), 0, reg);
}

DWORD
WlanRegisterNotificationImpl(HANDLE hClientHandle,
                             DWORD dwNotifSource,
                             BOOL bIgnoreDuplicate,
                             WLAN_NOTIFICATION_CALLBACK funcCallback,
                             PVOID pCallbackContext,
                             PDWORD pdwPrevNotifSource)
{
    PWLAN_NOTIF_REG reg;
    DWORD dwPrev = WLAN_NOTIFICATION_SOURCE_NONE;
    DWORD dwResult = ERROR_SUCCESS;

    UNREFERENCED_PARAMETER(bIgnoreDuplicate);

    WlanNotifEnsureInit();

    EnterCriticalSection(&NotifLock);
    reg = WlanNotifFind(hClientHandle);
    if (reg != NULL)
        dwPrev = reg->dwSource;

    if (dwNotifSource == WLAN_NOTIFICATION_SOURCE_NONE || funcCallback == NULL)
    {
        /* Deregister: unlink, then release the getter + free outside the lock. */
        if (reg != NULL)
        {
            WlanNotifUnlink(reg);
            LeaveCriticalSection(&NotifLock);
            WlanNotifTeardown(reg);
        }
        else
        {
            LeaveCriticalSection(&NotifLock);
        }

        if (pdwPrevNotifSource != NULL)
            *pdwPrevNotifSource = dwPrev;
        return ERROR_SUCCESS;
    }

    if (reg != NULL)
    {
        /* Re-registration on an existing handle: update the callback and
         * re-subscribe the dedicated handle (which already has a getter). */
        reg->dwSource = dwNotifSource;
        reg->Callback = funcCallback;
        reg->Context = pCallbackContext;
        LeaveCriticalSection(&NotifLock);

        RpcTryExcept
        {
            DWORD dwOld = 0;
            (void)_RpcRegisterNotification(reg->hNotifHandle, dwNotifSource, &dwOld);
        }
        RpcExcept(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        RpcEndExcept;

        if (pdwPrevNotifSource != NULL)
            *pdwPrevNotifSource = dwPrev;
        return ERROR_SUCCESS;
    }

    /* New registration: reserve the slot, then set up the dedicated handle and
     * getter outside the lock (the RPC calls must not run under NotifLock). */
    reg = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*reg));
    if (reg == NULL)
    {
        LeaveCriticalSection(&NotifLock);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    reg->hClientHandle = hClientHandle;
    reg->dwSource = dwNotifSource;
    reg->Callback = funcCallback;
    reg->Context = pCallbackContext;
    reg->Next = NotifListHead;
    NotifListHead = reg;
    LeaveCriticalSection(&NotifLock);

    /* Open a service handle dedicated to this client's notification stream.
     * The getter parks on it, so it never serializes with control calls made
     * on the application handle. */
    RpcTryExcept
    {
        DWORD dwVer = 0;
        WCHAR szMachine[] = L"localhost";
        dwResult = _RpcOpenHandle(szMachine, 2 /* client version */, &dwVer,
                                  (WLANSVC_RPC_HANDLE)&reg->hNotifHandle);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        dwResult = RpcExceptionCode();
    }
    RpcEndExcept;

    if (dwResult == ERROR_SUCCESS)
    {
        RpcTryExcept
        {
            DWORD dwOld = 0;
            dwResult = _RpcRegisterNotification(reg->hNotifHandle, dwNotifSource, &dwOld);
        }
        RpcExcept(EXCEPTION_EXECUTE_HANDLER)
        {
            dwResult = RpcExceptionCode();
        }
        RpcEndExcept;
    }

    if (dwResult == ERROR_SUCCESS)
    {
        InterlockedExchange(&reg->Running, 1);
        reg->hThread = CreateThread(NULL, 0, WlanNotifThread, reg, 0, NULL);
        if (reg->hThread == NULL)
            dwResult = GetLastError();
    }

    if (dwResult != ERROR_SUCCESS)
    {
        /* Roll back everything we linked/opened. */
        EnterCriticalSection(&NotifLock);
        WlanNotifUnlink(reg);
        LeaveCriticalSection(&NotifLock);
        WlanNotifTeardown(reg);
        return dwResult;
    }

    if (pdwPrevNotifSource != NULL)
        *pdwPrevNotifSource = dwPrev;
    return ERROR_SUCCESS;
}

/*
 * Called from WlanCloseHandle before _RpcCloseHandle: release the getter and
 * close its dedicated service handle before the application handle goes away.
 */
VOID
WlanStopNotificationThread(HANDLE hClientHandle)
{
    PWLAN_NOTIF_REG reg;

    if (InterlockedCompareExchange(&NotifInit, 1, 1) == 0)
        return;

    EnterCriticalSection(&NotifLock);
    reg = WlanNotifFind(hClientHandle);
    if (reg != NULL)
        WlanNotifUnlink(reg);
    LeaveCriticalSection(&NotifLock);

    if (reg != NULL)
        WlanNotifTeardown(reg);
}
