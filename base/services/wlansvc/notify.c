/*
 * PROJECT:     ReactOS WLAN Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:        base/services/wlansvc/notify.c
 * PURPOSE:     ACM/connection notification fan-out + async delivery queue
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 *
 * Each WlanSvcIndicate* enqueues a copy of the event onto every subscribed
 * handle and signals its event, releasing the client worker parked in
 * _RpcAsyncGetNotification, which returns one notification per call.
 *
 * Indicate routines run with WlanSvcLock held by the caller; the async getter
 * holds the lock only while touching the queue, never while waiting -- but it
 * does hold a reference on the handle for the whole call, so the handle and
 * its event outlive the wait even if the client closes or dies meanwhile.
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* Append a notification to one handle's queue and wake its async getter. */
static VOID
WlanSvcQueueToHandle(PWLANSVCHANDLE Handle,
                     DWORD Source,
                     DWORD Code,
                     const GUID *pGuid,
                     const void *pData,
                     DWORD DataSize)
{
    PWLANSVC_NOTIFICATION notif;
    SIZE_T total;

    if ((Handle->dwNotifSource & Source) == 0)
        return;

    total = FIELD_OFFSET(WLANSVC_NOTIFICATION, Data) + (DataSize ? DataSize : 0);
    notif = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (notif == NULL)
        return;

    notif->NotificationSource = Source;
    notif->NotificationCode = Code;
    if (pGuid != NULL)
        notif->InterfaceGuid = *pGuid;
    notif->dwDataSize = DataSize;
    if (pData != NULL && DataSize != 0)
        memcpy(notif->Data, pData, DataSize);

    InsertTailList(&Handle->NotificationQueue, &notif->ListEntry);
    if (Handle->hNotifyEvent != NULL)
        SetEvent(Handle->hNotifyEvent);
}

/* Fan a single notification out to every subscribed open handle. */
static VOID
WlanSvcBroadcast(DWORD Source,
                 DWORD Code,
                 const GUID *pGuid,
                 const void *pData,
                 DWORD DataSize)
{
    PLIST_ENTRY entry;

    for (entry = WlanSvcHandleListHead.Flink;
         entry != &WlanSvcHandleListHead;
         entry = entry->Flink)
    {
        PWLANSVCHANDLE h =
            CONTAINING_RECORD(entry, WLANSVCHANDLE, WlanSvcHandleListEntry);
        WlanSvcQueueToHandle(h, Source, Code, pGuid, pData, DataSize);
    }
}

VOID
WlanSvcIndicateAcm(PWLANSVC_INTERFACE Iface, DWORD NotificationCode)
{
    WlanSvcBroadcast(WLAN_NOTIFICATION_SOURCE_ACM,
                     NotificationCode,
                     &Iface->InterfaceGuid,
                     NULL, 0);
}

VOID
WlanSvcIndicateConnection(PWLANSVC_INTERFACE Iface,
                          DWORD NotificationCode,
                          WLAN_CONNECTION_MODE Mode,
                          LPCWSTR ProfileName,
                          WLAN_REASON_CODE Reason)
{
    WLAN_CONNECTION_NOTIFICATION_DATA cnd;

    ZeroMemory(&cnd, sizeof(cnd));
    cnd.wlanConnectionMode = Mode;
    if (ProfileName != NULL)
        wcsncpy(cnd.strProfileName, ProfileName, WLAN_MAX_NAME_LENGTH - 1);
    cnd.dot11Ssid = Iface->ConnectedSsid;
    cnd.dot11BssType = Iface->ConnectedBssType;
    cnd.bSecurityEnabled = (Iface->ConnectedAuth != DOT11_AUTH_ALGO_80211_OPEN);
    cnd.wlanReasonCode = Reason;
    cnd.dwFlags = 0;
    cnd.strProfileXml[0] = L'\0';

    WlanSvcBroadcast(WLAN_NOTIFICATION_SOURCE_ACM,
                     NotificationCode,
                     &Iface->InterfaceGuid,
                     &cnd, sizeof(cnd));
}

/*
 * How long the getter parks on the handle's event before returning
 * ERROR_TIMEOUT so the RPC runtime can release the per-context-handle lock and
 * the client can re-issue.  Bounds worst-case unsubscribe/teardown latency; a
 * real notification wakes the wait immediately, so this never delays delivery.
 */
#define WLANSVC_NOTIF_POLL_MS 2000

/*
 * Pop the oldest queued notification into an RPC-allocated buffer, waiting on
 * the handle's event if the queue is empty.  The payload is a separate RPC
 * allocation referenced by pData ([unique, size_is(dwDataSize)] in the IDL).
 *
 * Returns ERROR_TIMEOUT when the poll interval elapses with nothing queued and
 * the client still subscribed: the caller must return this to the client so
 * the stub (and rpcrt4's per-handle lock) unwinds; the client's worker loops
 * and calls again.
 */
DWORD
WlanSvcDequeueNotification(PWLANSVCHANDLE Handle, PWLAN_NOTIFICATION_DATA *ppData)
{
    PWLAN_NOTIFICATION_DATA out;
    PWLANSVC_NOTIFICATION notif = NULL;
    PLIST_ENTRY entry;
    HANDLE hEvent;

    *ppData = NULL;

    for (;;)
    {
        EnterCriticalSection(&WlanSvcLock);

        /* Torn down while we were parked (client closed, or its binding ran
         * down).  Our caller's reference keeps the memory alive long enough to
         * read this and leave. */
        if (Handle->Closing)
        {
            LeaveCriticalSection(&WlanSvcLock);
            return ERROR_INVALID_HANDLE;
        }

        if (!IsListEmpty(&Handle->NotificationQueue))
        {
            entry = RemoveHeadList(&Handle->NotificationQueue);
            notif = CONTAINING_RECORD(entry, WLANSVC_NOTIFICATION, ListEntry);
            if (IsListEmpty(&Handle->NotificationQueue) &&
                Handle->hNotifyEvent != NULL)
            {
                ResetEvent(Handle->hNotifyEvent);
            }
        }
        else if (Handle->dwNotifSource == WLAN_NOTIFICATION_SOURCE_NONE)
        {
            /* Client unsubscribed and nothing is queued -> release the worker. */
            LeaveCriticalSection(&WlanSvcLock);
            return ERROR_INVALID_STATE;
        }

        /* Read the event under the lock and wait on that copy: the field is
         * cleared by the final release, and only the reference we hold keeps
         * that from happening while we are in the wait below. */
        hEvent = Handle->hNotifyEvent;
        LeaveCriticalSection(&WlanSvcLock);

        if (notif != NULL)
            break;

        if (hEvent == NULL)
            return ERROR_INVALID_STATE;

        /*
         * Wait with a bounded timeout, never INFINITE.  rpcrt4 holds this
         * handle's own per-context-handle CRITICAL_SECTION (RpcContextHandle.lock)
         * across the entire _RpcAsyncGetNotification stub, so parking here
         * forever pins that lock and deadlocks every other call that touches
         * the handle -- including the client's own _RpcRegisterNotification(NONE)
         * that is trying to wake us: a circular wait (see docs/asus.txt 85).
         * On timeout we return to the RPC runtime, which drops the lock, and
         * the client's worker re-issues the getter.  A real notification or an
         * unsubscribe signals hEvent and wakes us at once, so this costs a
         * single idle round-trip per interval and nothing on the hot path.
         */
        if (WaitForSingleObject(hEvent, WLANSVC_NOTIF_POLL_MS) == WAIT_TIMEOUT)
            return ERROR_TIMEOUT;
    }

    out = midl_user_allocate(sizeof(*out));
    if (out == NULL)
    {
        HeapFree(GetProcessHeap(), 0, notif);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    ZeroMemory(out, sizeof(*out));
    out->NotificationSource = notif->NotificationSource;
    out->NotificationCode = notif->NotificationCode;
    out->InterfaceGuid = notif->InterfaceGuid;
    out->dwDataSize = notif->dwDataSize;
    out->pData = NULL;

    if (notif->dwDataSize != 0)
    {
        out->pData = midl_user_allocate(notif->dwDataSize);
        if (out->pData != NULL)
            memcpy(out->pData, notif->Data, notif->dwDataSize);
        else
            out->dwDataSize = 0;
    }

    HeapFree(GetProcessHeap(), 0, notif);
    *ppData = out;
    return ERROR_SUCCESS;
}

/* Free any pending notifications on a handle (called at handle close). */
VOID
WlanSvcDrainHandleQueue(PWLANSVCHANDLE Handle)
{
    while (!IsListEmpty(&Handle->NotificationQueue))
    {
        PLIST_ENTRY entry = RemoveHeadList(&Handle->NotificationQueue);
        PWLANSVC_NOTIFICATION notif =
            CONTAINING_RECORD(entry, WLANSVC_NOTIFICATION, ListEntry);
        HeapFree(GetProcessHeap(), 0, notif);
    }
}
