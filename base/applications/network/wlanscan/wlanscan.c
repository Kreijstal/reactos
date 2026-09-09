/*
 * PROJECT:     ReactOS Native WiFi
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Command-line scan of available 802.11 networks
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <stdarg.h>
#include <windows.h>
#include <wlanapi.h>

/* Not yet in ReactOS' wlanapi.h; exported by wlanapi.dll */
DWORD WINAPI WlanGetNetworkBssList(HANDLE hClientHandle, const GUID *pInterfaceGuid,
                                   const PDOT11_SSID pDot11Ssid, DOT11_BSS_TYPE dot11BssType,
                                   BOOL bSecurityEnabled, PVOID pReserved,
                                   PWLAN_BSS_LIST *ppWlanBssList);
#include <stdio.h>

static void
WlanEmit(PCWSTR Format, ...)
{
    WCHAR WideBuffer[1024];
    CHAR AnsiBuffer[1024];
    va_list Args;

    va_start(Args, Format);
    _vsnwprintf(WideBuffer, ARRAYSIZE(WideBuffer) - 1, Format, Args);
    va_end(Args);
    WideBuffer[ARRAYSIZE(WideBuffer) - 1] = L'\0';

    fputws(WideBuffer, stdout);
    fflush(stdout);

    WideCharToMultiByte(CP_UTF8, 0, WideBuffer, -1, AnsiBuffer, sizeof(AnsiBuffer), NULL, NULL);
    OutputDebugStringA(AnsiBuffer);
}
#define wprintf WlanEmit

typedef struct _SCAN_WAIT_CONTEXT
{
    HANDLE Event;
    const GUID *InterfaceGuid;
} SCAN_WAIT_CONTEXT, *PSCAN_WAIT_CONTEXT;

static VOID WINAPI
ScanNotificationCallback(PWLAN_NOTIFICATION_DATA Data, PVOID Context)
{
    PSCAN_WAIT_CONTEXT WaitContext = Context;

    if (Data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM)
        return;

    if (Data->NotificationCode != wlan_notification_acm_scan_complete &&
        Data->NotificationCode != wlan_notification_acm_scan_fail)
    {
        return;
    }

    if (!IsEqualGUID(&Data->InterfaceGuid, WaitContext->InterfaceGuid))
        return;

    SetEvent(WaitContext->Event);
}

static PCWSTR
AuthName(DOT11_AUTH_ALGORITHM Auth)
{
    switch (Auth)
    {
        case DOT11_AUTH_ALGO_80211_OPEN:       return L"Open";
        case DOT11_AUTH_ALGO_80211_SHARED_KEY: return L"Shared";
        case DOT11_AUTH_ALGO_WPA:              return L"WPA";
        case DOT11_AUTH_ALGO_WPA_PSK:          return L"WPA-PSK";
        case DOT11_AUTH_ALGO_RSNA:             return L"WPA2";
        case DOT11_AUTH_ALGO_RSNA_PSK:         return L"WPA2-PSK";
        case DOT11_AUTH_ALGO_WPA3_SAE:         return L"WPA3-SAE";
        default:                               return L"?";
    }
}

/* The SSID is a raw byte array; render it as text for display. */
static VOID
SsidToString(const DOT11_SSID *Ssid, WCHAR *Buffer, int cch)
{
    int len;

    if (Ssid->uSSIDLength == 0)
    {
        lstrcpynW(Buffer, L"<hidden>", cch);
        return;
    }

    len = MultiByteToWideChar(CP_UTF8, 0, (const char *)Ssid->ucSSID,
                              Ssid->uSSIDLength, Buffer, cch - 1);
    if (len <= 0)
        len = 0;
    Buffer[len] = L'\0';
}

static BOOL
XmlEscape(PCWSTR Source, PWSTR Destination, SIZE_T Capacity)
{
    while (*Source != L'\0')
    {
        PCWSTR Replacement = NULL;
        SIZE_T Length;
        switch (*Source)
        {
            case L'&': Replacement = L"&amp;"; break;
            case L'<': Replacement = L"&lt;"; break;
            case L'>': Replacement = L"&gt;"; break;
            case L'\"': Replacement = L"&quot;"; break;
            case L'\'': Replacement = L"&apos;"; break;
        }
        if (Replacement != NULL)
        {
            Length = wcslen(Replacement);
            if (Length >= Capacity)
                return FALSE;
            memcpy(Destination, Replacement, Length * sizeof(WCHAR));
            Destination += Length;
            Capacity -= Length;
        }
        else
        {
            if (Capacity <= 1)
                return FALSE;
            *Destination++ = *Source;
            Capacity--;
        }
        Source++;
    }
    if (Capacity == 0)
        return FALSE;
    *Destination = L'\0';
    return TRUE;
}

static PCWSTR
StateName(WLAN_INTERFACE_STATE State)
{
    switch (State)
    {
        case wlan_interface_state_not_ready:            return L"not ready";
        case wlan_interface_state_connected:            return L"connected";
        case wlan_interface_state_ad_hoc_network_formed:return L"ad-hoc formed";
        case wlan_interface_state_disconnecting:        return L"disconnecting";
        case wlan_interface_state_disconnected:         return L"disconnected";
        case wlan_interface_state_associating:          return L"associating";
        case wlan_interface_state_discovering:          return L"discovering";
        case wlan_interface_state_authenticating:       return L"authenticating";
        default:                                        return L"?";
    }
}

/*
 * Read the passphrase from stdin instead of the command line.  A key on the
 * command line is visible to anyone who can list processes, and it lands in
 * whatever shell history or batch file invoked us; stdin can be redirected
 * from a file that is deleted straight afterwards.  Bytes are taken as UTF-8,
 * which is what the profile XML wants anyway.
 */
static BOOL
ReadPassphrase(PWSTR Buffer, int cch)
{
    CHAR Raw[512];
    int Length;

    if (fgets(Raw, sizeof(Raw), stdin) == NULL)
    {
        SecureZeroMemory(Raw, sizeof(Raw));
        return FALSE;
    }

    /* Strip the line terminator, whichever flavour arrived. */
    Length = (int)strlen(Raw);
    while (Length > 0 && (Raw[Length - 1] == '\n' || Raw[Length - 1] == '\r'))
        Raw[--Length] = '\0';

    if (Length == 0)
    {
        SecureZeroMemory(Raw, sizeof(Raw));
        return FALSE;
    }

    Length = MultiByteToWideChar(CP_UTF8, 0, Raw, Length, Buffer, cch - 1);
    SecureZeroMemory(Raw, sizeof(Raw));
    if (Length <= 0)
        return FALSE;

    Buffer[Length] = L'\0';
    return TRUE;
}

/*
 * WlanConnect only reports that the request was accepted.  Ask the service
 * what actually happened, so a failed association is not mistaken for a
 * successful one -- the whole point of this command as a diagnostic.
 */
/* RSN cipher suite selector type -> name (IEEE 802.11 table 9-131). */
static PCWSTR
CipherName(UCHAR suiteType)
{
    switch (suiteType)
    {
        case 1:  return L"WEP40";
        case 2:  return L"TKIP";
        case 4:  return L"CCMP";
        case 5:  return L"WEP104";
        case 8:  return L"GCMP";
        default: return L"?";
    }
}

static VOID
ReportConnection(HANDLE hClient, const GUID *pGuid)
{
    PWLAN_CONNECTION_ATTRIBUTES pConn = NULL;
    DWORD dwSize = 0, dwResult;
    WCHAR szSsid[64];

    dwResult = WlanQueryInterface(hClient, pGuid,
                                  wlan_intf_opcode_current_connection,
                                  NULL, &dwSize, (PVOID *)&pConn, NULL);
    if (dwResult != ERROR_SUCCESS || pConn == NULL)
    {
        wprintf(L"  Connection state: query failed: %lu\n", dwResult);
        return;
    }

    if (dwSize < sizeof(*pConn))
    {
        wprintf(L"  Connection state: short reply (%lu bytes)\n", dwSize);
        WlanFreeMemory(pConn);
        return;
    }

    SsidToString(&pConn->wlanAssociationAttributes.dot11Ssid,
                 szSsid, ARRAYSIZE(szSsid));

    wprintf(L"  Connection state: %s\n", StateName(pConn->isState));
    if (pConn->isState == wlan_interface_state_connected)
    {
        const BYTE *b = pConn->wlanAssociationAttributes.dot11Bssid;

        wprintf(L"    SSID  : %s\n", szSsid);
        wprintf(L"    BSSID : %02x:%02x:%02x:%02x:%02x:%02x\n",
                b[0], b[1], b[2], b[3], b[4], b[5]);
        wprintf(L"    Auth  : %s   Signal: %lu%%\n",
                AuthName(pConn->wlanSecurityAttributes.dot11AuthAlgorithm),
                pConn->wlanAssociationAttributes.wlanSignalQuality);
    }

    WlanFreeMemory(pConn);
}

static DWORD
ScanInterface(HANDLE hClient, const GUID *pGuid)
{
    DWORD dwResult, i;
    PWLAN_AVAILABLE_NETWORK_LIST pNetList = NULL;
    SCAN_WAIT_CONTEXT WaitContext;
    DWORD dwPrevNotifSource;

    wprintf(L"  Scanning...\n");

    WaitContext.Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    WaitContext.InterfaceGuid = pGuid;
    if (WaitContext.Event != NULL)
    {
        WlanRegisterNotification(hClient,
                                 WLAN_NOTIFICATION_SOURCE_ACM,
                                 TRUE,
                                 ScanNotificationCallback,
                                 &WaitContext,
                                 NULL,
                                 &dwPrevNotifSource);
    }

    dwResult = WlanScan(hClient, pGuid, NULL, NULL, NULL);
    if (dwResult != ERROR_SUCCESS)
        wprintf(L"  (WlanScan returned %lu; reading cached results anyway)\n", dwResult);

    if (WaitContext.Event != NULL)
    {
        WaitForSingleObject(WaitContext.Event, 15000);
        WlanRegisterNotification(hClient,
                                 WLAN_NOTIFICATION_SOURCE_NONE,
                                 TRUE,
                                 NULL,
                                 NULL,
                                 NULL,
                                 NULL);
        CloseHandle(WaitContext.Event);
    }

    dwResult = WlanGetAvailableNetworkList(hClient, pGuid,
                   WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES,
                   NULL, &pNetList);
    if (dwResult != ERROR_SUCCESS || pNetList == NULL)
    {
        wprintf(L"  WlanGetAvailableNetworkList failed: %lu\n", dwResult);
        return dwResult;
    }

    if (pNetList->dwNumberOfItems == 0)
    {
        wprintf(L"  No networks found.\n");
    }
    else
    {
        wprintf(L"  %-32s %5s  %-7s  %s\n", L"SSID", L"Sig%", L"Sec", L"Auth");
        wprintf(L"  ---------------------------------------------------------------\n");

        for (i = 0; i < pNetList->dwNumberOfItems; i++)
        {
            PWLAN_AVAILABLE_NETWORK n = &pNetList->Network[i];
            WCHAR szSsid[64];

            SsidToString(&n->dot11Ssid, szSsid, ARRAYSIZE(szSsid));
            wprintf(L"  %-32.32s %5lu  %-7s  %s%s\n",
                    szSsid,
                    n->wlanSignalQuality,
                    n->bSecurityEnabled ? L"secured" : L"open",
                    AuthName(n->dot11DefaultAuthAlgorithm),
                    (n->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) ? L"  [connected]" : L"");
        }
        wprintf(L"  (%lu network(s))\n", pNetList->dwNumberOfItems);
    }

    /* Per-BSS view: the frequency the service recorded for each BSS next to
     * the channel the beacon itself carries in its DS Parameter Set IE.  The
     * two disagreeing is a driver bug (the BSS is remembered on the channel
     * it was HEARD on, not the one it lives on). */
    {
        PWLAN_BSS_LIST pBssList = NULL;
        DWORD dwBss = WlanGetNetworkBssList(hClient, pGuid, NULL,
                                            dot11_BSS_type_any, FALSE, NULL,
                                            &pBssList);
        if (dwBss == ERROR_SUCCESS && pBssList != NULL)
        {
            wprintf(L"\n  %-24s %-17s %7s %6s %5s  %s\n",
                    L"SSID", L"BSSID", L"kHz", L"DS-ch", L"RSSI", L"RSN");
            for (i = 0; i < pBssList->dwNumberOfItems; i++)
            {
                PWLAN_BSS_ENTRY e = &pBssList->wlanBssEntries[i];
                PUCHAR ie = (PUCHAR)e + e->ulIeOffset;
                ULONG left = e->ulIeSize, ds = 0;
                WCHAR szSsid[64];
                WCHAR szSec[48] = L"";

                while (left >= 2 && (ULONG)ie[1] + 2 <= left)
                {
                    if (ie[0] == 3 && ie[1] == 1) ds = ie[2];
                    /* RSN: version(2) group(4) count(2) pairwise(4 each).
                     * The group cipher is the one the AP dictates; a
                     * WPA/WPA2 mixed AP says g=TKIP with p=CCMP. */
                    if (ie[0] == 48 && ie[1] >= 8 && szSec[0] == L'\0')
                    {
                        ULONG n = ie[8] | (ie[9] << 8), k, pos = 10;
                        _snwprintf(szSec, ARRAYSIZE(szSec), L"g=%s p=",
                                   CipherName(ie[7]));
                        for (k = 0; k < n && pos + 4 <= (ULONG)ie[1] + 2; k++, pos += 4)
                        {
                            wcsncat(szSec, k ? L"+" : L"", ARRAYSIZE(szSec) - wcslen(szSec) - 1);
                            wcsncat(szSec, CipherName(ie[pos + 3]), ARRAYSIZE(szSec) - wcslen(szSec) - 1);
                        }
                    }
                    left -= ie[1] + 2;
                    ie += ie[1] + 2;
                }
                SsidToString(&e->dot11Ssid, szSsid, ARRAYSIZE(szSsid));
                wprintf(L"  %-24.24s %02x:%02x:%02x:%02x:%02x:%02x %7lu %6lu %5ld  %s\n",
                        szSsid,
                        e->dot11Bssid[0], e->dot11Bssid[1], e->dot11Bssid[2],
                        e->dot11Bssid[3], e->dot11Bssid[4], e->dot11Bssid[5],
                        e->ulChCenterFrequency, ds, e->lRssi, szSec);
            }
            WlanFreeMemory(pBssList);
        }
        else
        {
            wprintf(L"  WlanGetNetworkBssList failed: %lu\n", dwBss);
        }
    }

    WlanFreeMemory(pNetList);
    return ERROR_SUCCESS;
}

int
wmain(int argc, WCHAR *argv[])
{
    HANDLE hClient = NULL;
    DWORD dwVersion = 0, dwResult, i;
    PWLAN_INTERFACE_INFO_LIST pIfList = NULL;

    wprintf(L"ReactOS Native WiFi scan\n\n");

    dwResult = WlanOpenHandle(WLAN_API_VERSION_2_0, NULL, &dwVersion, &hClient);
    if (dwResult != ERROR_SUCCESS)
    {
        wprintf(L"WlanOpenHandle failed: %lu "
                L"(is the WLAN AutoConfig service running?)\n", dwResult);
        return 1;
    }

    dwResult = WlanEnumInterfaces(hClient, NULL, &pIfList);
    if (dwResult != ERROR_SUCCESS || pIfList == NULL)
    {
        wprintf(L"WlanEnumInterfaces failed: %lu\n", dwResult);
        WlanCloseHandle(hClient, NULL);
        return 1;
    }

    if (pIfList->dwNumberOfItems == 0)
        wprintf(L"No wireless interfaces present.\n");

    for (i = 0; i < pIfList->dwNumberOfItems; i++)
    {
        PWLAN_INTERFACE_INFO pIf = &pIfList->InterfaceInfo[i];

        wprintf(L"Interface %lu: %s\n", i, pIf->strInterfaceDescription);
        ScanInterface(hClient, &pIf->InterfaceGuid);
        if (argc >= 2 && argc <= 4)
        {
            DOT11_SSID Ssid;
            WLAN_CONNECTION_PARAMETERS Parameters;
            int Length;
            WCHAR ProfileXml[1024];
            WCHAR EscapedSsid[256], EscapedKey[512];
            WCHAR KeyBuffer[256];
            PCWSTR Passphrase;
            DWORD ReasonCode = 0;

            ZeroMemory(&Ssid, sizeof(Ssid));
            Length = WideCharToMultiByte(CP_UTF8, 0, argv[1],
                                         (int)wcslen(argv[1]),
                                         (PCHAR)Ssid.ucSSID,
                                         sizeof(Ssid.ucSSID), NULL, NULL);
            if (Length <= 0 || Length > sizeof(Ssid.ucSSID))
            {
                wprintf(L"  Invalid SSID.\n");
                continue;
            }
            Ssid.uSSIDLength = Length;
            ZeroMemory(&Parameters, sizeof(Parameters));
            if (argc >= 3)
            {
                /*
                 * Optional 4th argument selects the security suite; default is
                 * WPA2-PSK/CCMP.  The <authentication>/<encryption> pair maps
                 * one-to-one onto WlanSvcParseProfileXml (base/services/wlansvc/
                 * profile.c).  WPA3SAE is accepted here but only associates once
                 * the SAE (dragonfly) exchange exists in the supplicant.
                 */
                PCWSTR AuthStr = L"WPA2PSK";
                PCWSTR EncStr = L"AES";
                if (argc == 4)
                {
                    if (_wcsicmp(argv[3], L"tkip") == 0 ||
                        _wcsicmp(argv[3], L"wpa") == 0 ||
                        _wcsicmp(argv[3], L"wpapsk") == 0)
                    {
                        AuthStr = L"WPAPSK";
                        EncStr = L"TKIP";
                    }
                    else if (_wcsicmp(argv[3], L"wpa3") == 0 ||
                             _wcsicmp(argv[3], L"sae") == 0)
                    {
                        AuthStr = L"WPA3SAE";
                        EncStr = L"AES";
                    }
                    else if (_wcsicmp(argv[3], L"wpa2") == 0 ||
                             _wcsicmp(argv[3], L"rsnapsk") == 0)
                    {
                        AuthStr = L"WPA2PSK";
                        EncStr = L"AES";
                    }
                    else
                    {
                        wprintf(L"  Unknown security '%s' "
                                L"(use wpa2|tkip|wpa3).\n", argv[3]);
                        continue;
                    }
                }

                Passphrase = argv[2];
                if (wcscmp(argv[2], L"-") == 0)
                {
                    if (!ReadPassphrase(KeyBuffer, ARRAYSIZE(KeyBuffer)))
                    {
                        wprintf(L"  No passphrase on stdin.\n");
                        SecureZeroMemory(KeyBuffer, sizeof(KeyBuffer));
                        continue;
                    }
                    Passphrase = KeyBuffer;
                }

                if (!XmlEscape(argv[1], EscapedSsid, ARRAYSIZE(EscapedSsid)) ||
                    !XmlEscape(Passphrase, EscapedKey, ARRAYSIZE(EscapedKey)))
                {
                    wprintf(L"  SSID or passphrase is too long.\n");
                    SecureZeroMemory(EscapedKey, sizeof(EscapedKey));
                    SecureZeroMemory(KeyBuffer, sizeof(KeyBuffer));
                    continue;
                }
                _snwprintf(ProfileXml, ARRAYSIZE(ProfileXml) - 1,
                    L"<WLANProfile><name>%s</name><SSIDConfig><SSID>"
                    L"<name>%s</name></SSID></SSIDConfig>"
                    L"<connectionType>ESS</connectionType>"
                    L"<connectionMode>manual</connectionMode><MSM><security>"
                    L"<authEncryption><authentication>%s</authentication>"
                    L"<encryption>%s</encryption></authEncryption>"
                    L"<sharedKey><keyType>passPhrase</keyType>"
                    L"<protected>false</protected><keyMaterial>%s</keyMaterial>"
                    L"</sharedKey></security></MSM></WLANProfile>",
                    EscapedSsid, EscapedSsid, AuthStr, EncStr, EscapedKey);
                ProfileXml[ARRAYSIZE(ProfileXml) - 1] = L'\0';
                SecureZeroMemory(EscapedKey, sizeof(EscapedKey));
                SecureZeroMemory(KeyBuffer, sizeof(KeyBuffer));
                dwResult = WlanSetProfile(hClient, &pIf->InterfaceGuid, 0,
                                          ProfileXml, NULL, TRUE, NULL,
                                          &ReasonCode);
                SecureZeroMemory(ProfileXml, sizeof(ProfileXml));
                if (dwResult != ERROR_SUCCESS)
                {
                    wprintf(L"  WlanSetProfile failed: %lu (reason %lu)\n",
                            dwResult, ReasonCode);
                    continue;
                }
                Parameters.wlanConnectionMode = wlan_connection_mode_profile;
                Parameters.strProfile = argv[1];
            }
            else
            {
                Parameters.wlanConnectionMode =
                    wlan_connection_mode_discovery_secure;
                Parameters.pDot11Ssid = &Ssid;
            }
            Parameters.dot11BssType = dot11_BSS_type_infrastructure;
            dwResult = WlanConnect(hClient, &pIf->InterfaceGuid,
                                   &Parameters, NULL);
            wprintf(L"  Connect request for '%s': %lu\n", argv[1], dwResult);
            /* Association completion is asynchronous.  Keep the process
             * alive briefly so this command is useful as a diagnostic even
             * without a separate notification client. */
            Sleep(15000);
            ReportConnection(hClient, &pIf->InterfaceGuid);
        }
        wprintf(L"\n");
    }

    WlanFreeMemory(pIfList);
    WlanCloseHandle(hClient, NULL);
    return 0;
}
