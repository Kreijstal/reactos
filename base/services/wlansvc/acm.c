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
 * is auto and whose SSID the last scan saw.  A profile whose connect never
 * came up is passed over for the next one in the list, until every
 * candidate has failed once and the walk starts again from the top.  An
 * interface the client disconnected through WlanDisconnect stays down until
 * the next WlanConnect.
 *
 * The work runs on its own thread: WlanSvcDoScan waits for the scan-complete
 * event the notify worker sets, so it cannot run on the notify worker.
 */

#include "precomp.h"
#include <winreg.h>

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

/*
 * Why the ACM did or did not reconnect, for a box whose debug output never
 * reaches anyone at runtime (the ASUS: DPRINT is lost after boot).  The
 * counters are published to a volatile registry key after every pass, so
 * `reg query HKLM\SYSTEM\CurrentControlSet\Services\WlanSvc\AcmStatus`
 * answers "did it see the drop, did it try, what stopped it".
 */
#define WLANSVC_ACM_STATUS_KEY \
    L"SYSTEM\\CurrentControlSet\\Services\\WlanSvc\\AcmStatus"

/* Values of LastDecline: the first test that kept the ACM away from an
 * interface the service believed disconnected. */
#define ACM_DECLINE_NONE            0
#define ACM_DECLINE_AUTOCONFIG_OFF  1
#define ACM_DECLINE_USER            2   /* WlanDisconnect: waits for WlanConnect */
#define ACM_DECLINE_NOT_DOWN        3   /* cached state not disconnected */
#define ACM_DECLINE_GRACE           4   /* within 30 s of a connect request */
#define ACM_DECLINE_NO_AUTO_PROFILE 5
#define ACM_DECLINE_QUERY_FAILED    6   /* IOCTL_NWIFI_QUERY_STATE failed */
#define ACM_DECLINE_NWIFI_NOT_DOWN  7   /* nwifi says the link is not down */
#define ACM_DECLINE_NO_PROFILE_SEEN 8   /* the scan listed no auto SSID */

/* Every field a DWORD, published in this order under these names. */
static struct
{
    DWORD Kicks;
    DWORD Passes;
    DWORD Wanted;           /* interfaces found needing a reconnect */
    DWORD LastDecline;
    DWORD LastDeclineState; /* Iface->State at that decline */
    DWORD LastNwifiStatus;  /* NWIFI_LINK_STATUS at that decline */
    DWORD Scans;
    DWORD LastScanResult;
    DWORD Connects;
    DWORD LastConnectResult;
    DWORD LastPassTick;
    DWORD LastConnectTick;
    DWORD FailedAttempts;   /* profile connects that never came up */
    DWORD ListRestarts;     /* every candidate failed: walked from the top */
} WlanSvcAcmStats;

static const PCWSTR WlanSvcAcmStatNames[] =
{
    L"Kicks", L"Passes", L"Wanted", L"LastDecline", L"LastDeclineState",
    L"LastNwifiStatus", L"Scans", L"LastScanResult", L"Connects",
    L"LastConnectResult", L"LastPassTick", L"LastConnectTick",
    L"FailedAttempts", L"ListRestarts"
};

static VOID
WlanSvcAcmPublish(VOID)
{
    const DWORD *Value = (const DWORD *)&WlanSvcAcmStats;
    HKEY hKey;
    ULONG i;

    C_ASSERT(sizeof(WlanSvcAcmStats) ==
             sizeof(WlanSvcAcmStatNames) / sizeof(WlanSvcAcmStatNames[0]) *
             sizeof(DWORD));

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, WLANSVC_ACM_STATUS_KEY, 0, NULL,
                        REG_OPTION_VOLATILE, KEY_SET_VALUE, NULL, &hKey,
                        NULL) != ERROR_SUCCESS)
        return;

    for (i = 0; i < ARRAYSIZE(WlanSvcAcmStatNames); i++)
    {
        RegSetValueExW(hKey, WlanSvcAcmStatNames[i], 0, REG_DWORD,
                       (const BYTE *)&Value[i], sizeof(DWORD));
    }
    RegCloseKey(hKey);
}

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

/* ACM_DECLINE_NONE if the interface should be reconnected now. */
static DWORD
WlanSvcAcmDecline(PWLANSVC_INTERFACE Iface, PDWORD NwifiStatus)
{
    NWIFI_LINK_STATE ls;

    *NwifiStatus = MAXDWORD;
    if (!Iface->AutoConfigEnabled)
        return ACM_DECLINE_AUTOCONFIG_OFF;
    if (Iface->UserDisconnected)
        return ACM_DECLINE_USER;
    if (Iface->State != wlan_interface_state_disconnected)
        return ACM_DECLINE_NOT_DOWN;
    if (GetTickCount() - Iface->LastConnectTick < WLANSVC_ACM_CONNECT_GRACE_MS)
        return ACM_DECLINE_GRACE;
    if (!WlanSvcAcmHasAutoProfile(Iface))
        return ACM_DECLINE_NO_AUTO_PROFILE;

    /* The cached state follows notifications, which can be stale; nwifi's
     * own state decides whether the link is really down. */
    if (NwifiQueryState(Iface->NwifiIndex, Iface->UpperLuid,
                        &Iface->MacAddress, &ls) != ERROR_SUCCESS)
        return ACM_DECLINE_QUERY_FAILED;
    *NwifiStatus = ls.Status;
    if (ls.Status != NwifiLinkDisconnected)
        return ACM_DECLINE_NWIFI_NOT_DOWN;
    return ACM_DECLINE_NONE;
}

static BOOL
WlanSvcAcmWantsConnect(PWLANSVC_INTERFACE Iface)
{
    DWORD NwifiStatus;
    DWORD Decline = WlanSvcAcmDecline(Iface, &NwifiStatus);

    /* A connected interface declining is the normal case; record only why
     * an interface the service thinks is DOWN is being left alone. */
    if (Decline != ACM_DECLINE_NONE &&
        Iface->State == wlan_interface_state_disconnected)
    {
        WlanSvcAcmStats.LastDecline = Decline;
        WlanSvcAcmStats.LastDeclineState = Iface->State;
        WlanSvcAcmStats.LastNwifiStatus = NwifiStatus;
    }
    return Decline == ACM_DECLINE_NONE;
}

/* Lock held.  The connection the interface was asked for came up. */
VOID
WlanSvcAcmConnectionUp(PWLANSVC_INTERFACE Iface)
{
    PLIST_ENTRY pe;

    Iface->AttemptPending = FALSE;

    /* A working connection ends the walk: after the next drop the list is
     * tried from the top again, the preferred network first. */
    for (pe = Iface->ProfileListHead.Flink;
         pe != &Iface->ProfileListHead;
         pe = pe->Flink)
    {
        CONTAINING_RECORD(pe, WLANSVC_PROFILE, ListEntry)->AcmFailed = FALSE;
    }
}

/* Lock held.  ProfileName is being replaced or deleted (NULL: any): its
 * pending attempt says nothing about the profile that comes next. */
VOID
WlanSvcAcmForgetAttempt(PWLANSVC_INTERFACE Iface, LPCWSTR ProfileName)
{
    if (Iface->AttemptPending &&
        (ProfileName == NULL || _wcsicmp(Iface->AttemptProfile, ProfileName) == 0))
    {
        Iface->AttemptPending = FALSE;
    }
}

/*
 * Lock held, the interface is down and past the connect grace.  A profile
 * connect still pending then never came up -- a wrong key, or an AP that
 * would not have us -- so the walk moves past that profile.
 */
static VOID
WlanSvcAcmJudgeAttempt(PWLANSVC_INTERFACE Iface)
{
    PWLANSVC_PROFILE prof;

    if (!Iface->AttemptPending)
        return;
    Iface->AttemptPending = FALSE;

    prof = WlanSvcFindProfile(Iface, Iface->AttemptProfile);
    if (prof != NULL)
    {
        prof->AcmFailed = TRUE;
        WlanSvcAcmStats.FailedAttempts++;
        DPRINT1("WLANSVC: connect with '%S' never came up; trying the next profile\n",
                prof->Name);
    }
}

/*
 * The first auto-connect profile whose SSID is in the BSS cache, skipping
 * the ones that failed (SkipFailed).  *SkippedOne says whether a failed
 * profile would otherwise have been picked.
 */
static PWLANSVC_PROFILE
WlanSvcAcmFindCandidate(PWLANSVC_INTERFACE Iface, BOOL SkipFailed,
                        PBOOL SkippedOne)
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
                if (SkipFailed && prof->AcmFailed)
                {
                    *SkippedOne = TRUE;
                    break;
                }
                return prof;
            }
        }
    }
    return NULL;
}

/* The profile to connect next, walking the preference list on failure. */
static PWLANSVC_PROFILE
WlanSvcAcmPickProfile(PWLANSVC_INTERFACE Iface)
{
    PWLANSVC_PROFILE prof;
    PLIST_ENTRY pe;
    BOOL skipped = FALSE;

    prof = WlanSvcAcmFindCandidate(Iface, TRUE, &skipped);
    if (prof != NULL || !skipped)
        return prof;

    /* Every network in range failed once: start over from the top. */
    for (pe = Iface->ProfileListHead.Flink;
         pe != &Iface->ProfileListHead;
         pe = pe->Flink)
    {
        CONTAINING_RECORD(pe, WLANSVC_PROFILE, ListEntry)->AcmFailed = FALSE;
    }
    WlanSvcAcmStats.ListRestarts++;
    return WlanSvcAcmFindCandidate(Iface, FALSE, &skipped);
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

    WlanSvcAcmJudgeAttempt(iface);

    WlanSvcAcmStats.Scans++;
    dwResult = WlanSvcDoScan(iface, NULL);
    WlanSvcAcmStats.LastScanResult = dwResult;
    if (dwResult == ERROR_DEVICE_NOT_CONNECTED)
        return;

    iface = WlanSvcFindInterfaceByIndex(NwifiIndex);
    if (iface == NULL || !WlanSvcAcmWantsConnect(iface))
        return;

    prof = WlanSvcAcmPickProfile(iface);
    if (prof == NULL)
    {
        WlanSvcAcmStats.LastDecline = ACM_DECLINE_NO_PROFILE_SEEN;
        return;
    }

    wcsncpy(name, prof->Name, WLAN_MAX_NAME_LENGTH - 1);
    name[WLAN_MAX_NAME_LENGTH - 1] = L'\0';

    ZeroMemory(&params, sizeof(params));
    params.wlanConnectionMode = wlan_connection_mode_profile;
    params.strProfile = name;
    params.dot11BssType = prof->BssType;

    dwResult = WlanSvcConnect(iface, &params);
    WlanSvcAcmStats.Connects++;
    WlanSvcAcmStats.LastConnectResult = dwResult;
    WlanSvcAcmStats.LastConnectTick = GetTickCount();
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
        WlanSvcAcmStats.Passes++;
        WlanSvcAcmStats.Wanted += count;
        WlanSvcAcmStats.LastPassTick = GetTickCount();
        for (i = 0; i < count; i++)
            WlanSvcAcmServiceInterface(indices[i]);
        WlanSvcAcmPublish();
        LeaveCriticalSection(&WlanSvcLock);
    }

    return 0;
}

/* Re-evaluate every interface now instead of at the next retry tick. */
VOID
WlanSvcAcmKick(VOID)
{
    if (WlanSvcAcmEvent != NULL)
    {
        InterlockedIncrement((volatile LONG *)&WlanSvcAcmStats.Kicks);
        SetEvent(WlanSvcAcmEvent);
    }
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
