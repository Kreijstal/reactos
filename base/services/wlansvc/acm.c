/*
 * PROJECT:     ReactOS WLAN Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:        base/services/wlansvc/acm.c
 * PURPOSE:     Auto Configuration Module: connect disconnected interfaces to
 *              their auto-connect profiles
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 *
 * Like the Windows ACM, a disconnected interface with AutoConfig enabled is
 * reconnected to the first profile (in profile order) whose connectionMode
 * is auto and whose SSID the last scan saw.  An interface the client
 * disconnected through WlanDisconnect stays down until the next WlanConnect.
 *
 * The work runs on its own thread: WlanSvcDoScan waits for the scan-complete
 * event the notify worker sets, so it cannot run on the notify worker.
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* How often a disconnected interface rescans for its auto-connect networks. */
#define WLANSVC_ACM_RETRY_MS 60000
/* A connect request owns the interface this long (association + 4-way
 * handshake): the disconnect it raises while tearing down the previous link
 * must not send the ACM after it. */
#define WLANSVC_ACM_CONNECT_GRACE_MS 30000

static HANDLE        WlanSvcAcmThread = NULL;
static HANDLE        WlanSvcAcmEvent = NULL;
static volatile LONG WlanSvcAcmRun = 0;

static BOOL
WlanSvcAcmHasAutoProfile(PWLANSVC_INTERFACE Iface)
{
    PLIST_ENTRY entry;

    for (entry = Iface->ProfileListHead.Flink;
         entry != &Iface->ProfileListHead;
         entry = entry->Flink)
    {
        if (CONTAINING_RECORD(entry, WLANSVC_PROFILE, ListEntry)->AutoConnect)
            return TRUE;
    }
    return FALSE;
}

static BOOL
WlanSvcAcmWantsConnect(PWLANSVC_INTERFACE Iface)
{
    NWIFI_LINK_STATE ls;

    if (!Iface->AutoConfigEnabled ||
        Iface->UserDisconnected ||
        Iface->State != wlan_interface_state_disconnected ||
        GetTickCount() - Iface->LastConnectTick < WLANSVC_ACM_CONNECT_GRACE_MS ||
        !WlanSvcAcmHasAutoProfile(Iface))
    {
        return FALSE;
    }

    /* The cached state follows notifications, which can be stale; nwifi's
     * own state decides whether the link is really down. */
    return NwifiQueryState(Iface->NwifiIndex, Iface->UpperLuid,
                           &Iface->MacAddress, &ls) == ERROR_SUCCESS &&
           ls.Status == NwifiLinkDisconnected;
}

/* The first auto-connect profile whose SSID is in the BSS cache. */
static PWLANSVC_PROFILE
WlanSvcAcmPickProfile(PWLANSVC_INTERFACE Iface)
{
    PLIST_ENTRY pe, be;

    for (pe = Iface->ProfileListHead.Flink;
         pe != &Iface->ProfileListHead;
         pe = pe->Flink)
    {
        PWLANSVC_PROFILE prof = CONTAINING_RECORD(pe, WLANSVC_PROFILE, ListEntry);

        if (!prof->AutoConnect)
            continue;

        for (be = Iface->BssListHead.Flink;
             be != &Iface->BssListHead;
             be = be->Flink)
        {
            PWLANSVC_BSS_ENTRY bss =
                CONTAINING_RECORD(be, WLANSVC_BSS_ENTRY, ListEntry);

            if (bss->Ssid.uSSIDLength == prof->Ssid.uSSIDLength &&
                memcmp(bss->Ssid.ucSSID, prof->Ssid.ucSSID,
                       prof->Ssid.uSSIDLength) == 0 &&
                bss->BssType == prof->BssType)
            {
                return prof;
            }
        }
    }
    return NULL;
}

/* Lock held.  WlanSvcDoScan drops it, so the interface is looked up again. */
static VOID
WlanSvcAcmServiceInterface(ULONG NwifiIndex)
{
    PWLANSVC_INTERFACE iface;
    PWLANSVC_PROFILE prof;
    WLAN_CONNECTION_PARAMETERS params;
    WCHAR name[WLAN_MAX_NAME_LENGTH];
    DWORD dwResult;

    iface = WlanSvcFindInterfaceByIndex(NwifiIndex);
    if (iface == NULL || !WlanSvcAcmWantsConnect(iface))
        return;

    if (WlanSvcDoScan(iface, NULL) == ERROR_DEVICE_NOT_CONNECTED)
        return;

    iface = WlanSvcFindInterfaceByIndex(NwifiIndex);
    if (iface == NULL || !WlanSvcAcmWantsConnect(iface))
        return;

    prof = WlanSvcAcmPickProfile(iface);
    if (prof == NULL)
        return;

    wcsncpy(name, prof->Name, WLAN_MAX_NAME_LENGTH - 1);
    name[WLAN_MAX_NAME_LENGTH - 1] = L'\0';

    ZeroMemory(&params, sizeof(params));
    params.wlanConnectionMode = wlan_connection_mode_profile;
    params.strProfile = name;
    params.dot11BssType = prof->BssType;

    dwResult = WlanSvcConnect(iface, &params);
    DPRINT1("WLANSVC: auto-connect ifidx %lu to '%S': %lu\n",
            NwifiIndex, name, dwResult);
}

static DWORD WINAPI
WlanSvcAcmWorker(LPVOID lpParameter)
{
    UNREFERENCED_PARAMETER(lpParameter);

    while (InterlockedCompareExchange(&WlanSvcAcmRun, 0, 0))
    {
        PLIST_ENTRY entry;
        ULONG indices[16];
        ULONG count = 0, i;

        WaitForSingleObject(WlanSvcAcmEvent, WLANSVC_ACM_RETRY_MS);
        if (!InterlockedCompareExchange(&WlanSvcAcmRun, 0, 0))
            break;

        EnterCriticalSection(&WlanSvcLock);
        for (entry = WlanSvcInterfaceListHead.Flink;
             entry != &WlanSvcInterfaceListHead && count < ARRAYSIZE(indices);
             entry = entry->Flink)
        {
            PWLANSVC_INTERFACE iface =
                CONTAINING_RECORD(entry, WLANSVC_INTERFACE, ListEntry);

            if (WlanSvcAcmWantsConnect(iface))
                indices[count++] = iface->NwifiIndex;
        }
        for (i = 0; i < count; i++)
            WlanSvcAcmServiceInterface(indices[i]);
        LeaveCriticalSection(&WlanSvcLock);
    }

    return 0;
}

/* Re-evaluate every interface now instead of at the next retry tick. */
VOID
WlanSvcAcmKick(VOID)
{
    if (WlanSvcAcmEvent != NULL)
        SetEvent(WlanSvcAcmEvent);
}

VOID
WlanSvcAcmStart(VOID)
{
    if (WlanSvcAcmThread != NULL)
        return;

    WlanSvcAcmEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (WlanSvcAcmEvent == NULL)
        return;

    InterlockedExchange(&WlanSvcAcmRun, 1);
    WlanSvcAcmThread = CreateThread(NULL, 0, WlanSvcAcmWorker, NULL, 0, NULL);
    if (WlanSvcAcmThread == NULL)
    {
        InterlockedExchange(&WlanSvcAcmRun, 0);
        CloseHandle(WlanSvcAcmEvent);
        WlanSvcAcmEvent = NULL;
    }
}

VOID
WlanSvcAcmStop(VOID)
{
    if (WlanSvcAcmThread == NULL)
        return;

    InterlockedExchange(&WlanSvcAcmRun, 0);
    SetEvent(WlanSvcAcmEvent);
    WaitForSingleObject(WlanSvcAcmThread, INFINITE);
    CloseHandle(WlanSvcAcmThread);
    WlanSvcAcmThread = NULL;
    CloseHandle(WlanSvcAcmEvent);
    WlanSvcAcmEvent = NULL;
}
