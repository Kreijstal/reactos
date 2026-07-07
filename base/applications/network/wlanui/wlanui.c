/*
 * PROJECT:     ReactOS Wireless Network Selector
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A small Win32 GUI front-end over wlanapi that lists the
 *              available 802.11 networks reported by the Native WiFi stack
 *              (wlansvc -> nwifi -> dot11 miniport) and connects to one,
 *              prompting for a WPA2-PSK key when the network is secured.
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 */

#include <stdarg.h>
#include <windows.h>
#include <commctrl.h>
#include <wlanapi.h>
#include <wchar.h>
#include "resource.h"

static HINSTANCE g_hInst;
static HANDLE    g_hClient  = NULL;
static GUID      g_Iface;
static BOOL      g_HaveIface = FALSE;
static PWLAN_AVAILABLE_NETWORK_LIST g_pNetList = NULL;  /* cached for connect */

#define WM_WLAN_NOTIFY  (WM_APP + 1)
#define SCAN_TIMER_ID   1
#define SCAN_SETTLE_MS  3000

/* ------------------------------------------------------------------------- */

static VOID
SetStatusRes(HWND hDlg, UINT uId)
{
    WCHAR sz[256];
    LoadStringW(g_hInst, uId, sz, ARRAYSIZE(sz));
    SetDlgItemTextW(hDlg, IDC_STATUS, sz);
}

static VOID
SetStatusFmt(HWND hDlg, UINT uFmtId, PCWSTR arg, DWORD num)
{
    WCHAR fmt[256], out[320];
    LoadStringW(g_hInst, uFmtId, fmt, ARRAYSIZE(fmt));
    _snwprintf(out, ARRAYSIZE(out) - 1, fmt, arg, num);
    out[ARRAYSIZE(out) - 1] = L'\0';
    SetDlgItemTextW(hDlg, IDC_STATUS, out);
}

/* Render a raw DOT11_SSID byte array as a display string. */
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

static PCWSTR
AuthName(DOT11_AUTH_ALGORITHM Auth)
{
    switch (Auth)
    {
        case DOT11_AUTH_ALGO_80211_OPEN:       return L"Open";
        case DOT11_AUTH_ALGO_80211_SHARED_KEY: return L"Shared";
        case DOT11_AUTH_ALGO_WPA:              return L"WPA-Enterprise";
        case DOT11_AUTH_ALGO_WPA_PSK:          return L"WPA-Personal";
        case DOT11_AUTH_ALGO_RSNA:             return L"WPA2-Enterprise";
        case DOT11_AUTH_ALGO_RSNA_PSK:         return L"WPA2-Personal";
        default:                               return L"-";
    }
}

/* ------------------------------------------------------------------------- */

static VOID
InitList(HWND hList)
{
    LVCOLUMNW col;
    WCHAR hdr[64];
    const struct { UINT id; int cx; } cols[] = {
        { IDS_COL_SSID,     150 },
        { IDS_COL_SIGNAL,    45 },
        { IDS_COL_SECURITY,  55 },
        { IDS_COL_AUTH,     110 },
        { IDS_COL_STATUS,    70 },
    };
    int i;

    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (i = 0; i < ARRAYSIZE(cols); i++)
    {
        LoadStringW(g_hInst, cols[i].id, hdr, ARRAYSIZE(hdr));
        col.iSubItem = i;
        col.cx       = cols[i].cx;
        col.pszText  = hdr;
        ListView_InsertColumn(hList, i, &col);
    }
}

/* Fill the list view from a fresh WlanGetAvailableNetworkList(). The list
 * is cached in g_pNetList so the connect path can recover the selected
 * network's SSID / security attributes by item index (lParam). */
static VOID
PopulateList(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_NETLIST);
    DWORD dwResult, i;
    PWLAN_AVAILABLE_NETWORK_LIST pList = NULL;

    ListView_DeleteAllItems(hList);
    if (g_pNetList)
    {
        WlanFreeMemory(g_pNetList);
        g_pNetList = NULL;
    }
    if (!g_HaveIface)
        return;

    dwResult = WlanGetAvailableNetworkList(g_hClient, &g_Iface,
                   WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES,
                   NULL, &pList);
    if (dwResult != ERROR_SUCCESS || pList == NULL)
        return;

    g_pNetList = pList;
    for (i = 0; i < pList->dwNumberOfItems; i++)
    {
        PWLAN_AVAILABLE_NETWORK n = &pList->Network[i];
        LVITEMW item;
        WCHAR sz[128];

        ZeroMemory(&item, sizeof(item));
        item.mask   = LVIF_TEXT | LVIF_PARAM;
        item.iItem  = (int)i;
        item.lParam = (LPARAM)i;
        SsidToString(&n->dot11Ssid, sz, ARRAYSIZE(sz));
        item.pszText = sz;
        ListView_InsertItem(hList, &item);

        _snwprintf(sz, ARRAYSIZE(sz), L"%lu%%", n->wlanSignalQuality);
        ListView_SetItemText(hList, i, 1, sz);

        LoadStringW(g_hInst, n->bSecurityEnabled ? IDS_SECURED : IDS_OPEN,
                    sz, ARRAYSIZE(sz));
        ListView_SetItemText(hList, i, 2, sz);

        ListView_SetItemText(hList, i, 3, (LPWSTR)AuthName(n->dot11DefaultAuthAlgorithm));

        if (n->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED)
        {
            LoadStringW(g_hInst, IDS_CONNECTED, sz, ARRAYSIZE(sz));
            /* IDS_CONNECTED is a "%s" format; show the plain word instead. */
            ListView_SetItemText(hList, i, 4, L"Connected");
        }
        else
        {
            ListView_SetItemText(hList, i, 4, L"");
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Security-key prompt                                                       */

typedef struct _KEYPROMPT
{
    PCWSTR Ssid;
    WCHAR  Key[128];
} KEYPROMPT, *PKEYPROMPT;

static INT_PTR CALLBACK
KeyDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PKEYPROMPT kp = (PKEYPROMPT)GetWindowLongPtrW(hDlg, GWLP_USERDATA);

    switch (msg)
    {
        case WM_INITDIALOG:
        {
            WCHAR fmt[128], out[256];
            kp = (PKEYPROMPT)lParam;
            SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)kp);
            LoadStringW(g_hInst, IDS_KEYPROMPTFMT, fmt, ARRAYSIZE(fmt));
            _snwprintf(out, ARRAYSIZE(out) - 1, fmt, kp->Ssid);
            out[ARRAYSIZE(out) - 1] = L'\0';
            SetDlgItemTextW(hDlg, IDC_KEYPROMPT, out);
            SendDlgItemMessageW(hDlg, IDC_KEY, EM_LIMITTEXT, ARRAYSIZE(kp->Key) - 1, 0);
            SetFocus(GetDlgItem(hDlg, IDC_KEY));
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_SHOWKEY:
                    SendDlgItemMessageW(hDlg, IDC_KEY, EM_SETPASSWORDCHAR,
                        IsDlgButtonChecked(hDlg, IDC_SHOWKEY) ? 0 : (WPARAM)L'*', 0);
                    InvalidateRect(GetDlgItem(hDlg, IDC_KEY), NULL, TRUE);
                    return TRUE;

                case IDOK:
                    GetDlgItemTextW(hDlg, IDC_KEY, kp->Key, ARRAYSIZE(kp->Key));
                    EndDialog(hDlg, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

/* Build a WPA2-PSK profile XML that wlansvc's profile parser understands
 * (it keys off <name>, <SSIDConfig><SSID><name>, <authentication> and
 * <keyMaterial>). */
static VOID
BuildProfileXml(PCWSTR Ssid, PCWSTR Key, PWSTR Xml, SIZE_T cch)
{
    _snwprintf(Xml, cch - 1,
        L"<?xml version=\"1.0\"?>\r\n"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\r\n"
        L"  <name>%s</name>\r\n"
        L"  <SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>\r\n"
        L"  <connectionType>ESS</connectionType>\r\n"
        L"  <connectionMode>manual</connectionMode>\r\n"
        L"  <MSM><security>\r\n"
        L"    <authEncryption>\r\n"
        L"      <authentication>WPA2PSK</authentication>\r\n"
        L"      <encryption>AES</encryption>\r\n"
        L"      <useOneX>false</useOneX>\r\n"
        L"    </authEncryption>\r\n"
        L"    <sharedKey>\r\n"
        L"      <keyType>passPhrase</keyType>\r\n"
        L"      <protected>false</protected>\r\n"
        L"      <keyMaterial>%s</keyMaterial>\r\n"
        L"    </sharedKey>\r\n"
        L"  </security></MSM>\r\n"
        L"</WLANProfile>\r\n",
        Ssid, Ssid, Key);
    Xml[cch - 1] = L'\0';
}

/* Connect to the network currently selected in the list view. */
static VOID
ConnectSelected(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_NETLIST);
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    LVITEMW item;
    PWLAN_AVAILABLE_NETWORK n;
    WLAN_CONNECTION_PARAMETERS cp;
    DOT11_SSID ssid;
    WCHAR szSsid[64];
    DWORD dwResult;

    if (sel < 0 || g_pNetList == NULL)
    {
        SetStatusRes(hDlg, IDS_SELECTNET);
        return;
    }

    ZeroMemory(&item, sizeof(item));
    item.mask  = LVIF_PARAM;
    item.iItem = sel;
    if (!ListView_GetItem(hList, &item))
        return;
    if ((DWORD)item.lParam >= g_pNetList->dwNumberOfItems)
        return;
    n = &g_pNetList->Network[item.lParam];

    ssid = n->dot11Ssid;
    SsidToString(&ssid, szSsid, ARRAYSIZE(szSsid));

    ZeroMemory(&cp, sizeof(cp));
    cp.dot11BssType = n->dot11BssType ? n->dot11BssType : dot11_BSS_type_infrastructure;

    if (n->bSecurityEnabled)
    {
        KEYPROMPT kp;
        WCHAR xml[2048];
        DWORD dwReason = 0;

        ZeroMemory(&kp, sizeof(kp));
        kp.Ssid = szSsid;
        if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_KEY), hDlg,
                            KeyDlgProc, (LPARAM)&kp) != IDOK)
            return;

        BuildProfileXml(szSsid, kp.Key, xml, ARRAYSIZE(xml));
        dwResult = WlanSetProfile(g_hClient, &g_Iface, 0, xml, NULL, TRUE,
                                  NULL, &dwReason);
        if (dwResult != ERROR_SUCCESS)
        {
            SetStatusFmt(hDlg, IDS_CONNECTFAIL, szSsid, dwResult);
            return;
        }
        /* Connect by stored profile name (== SSID here). */
        cp.wlanConnectionMode = wlan_connection_mode_profile;
        cp.strProfile         = szSsid;
    }
    else
    {
        /* Open network: connect by SSID, no profile. */
        cp.wlanConnectionMode = wlan_connection_mode_discovery_unsecure;
        cp.pDot11Ssid         = &ssid;
    }

    SetStatusFmt(hDlg, IDS_CONNECTING, szSsid, 0);
    dwResult = WlanConnect(g_hClient, &g_Iface, &cp, NULL);
    if (dwResult != ERROR_SUCCESS)
        SetStatusFmt(hDlg, IDS_CONNECTFAIL, szSsid, dwResult);
    /* Success/failure of the association itself arrives asynchronously via
     * the ACM notification callback (WM_WLAN_NOTIFY). */
}

/* ------------------------------------------------------------------------- */

/* Runs on a wlanapi worker thread; marshal to the UI thread. */
static VOID WINAPI
WlanNotifyCallback(PWLAN_NOTIFICATION_DATA pData, PVOID pContext)
{
    HWND hDlg = (HWND)pContext;
    if (pData && hDlg)
        PostMessageW(hDlg, WM_WLAN_NOTIFY, pData->NotificationCode, 0);
}

static VOID
StartScan(HWND hDlg)
{
    if (!g_HaveIface)
        return;
    SetStatusRes(hDlg, IDS_SCANNING);
    WlanScan(g_hClient, &g_Iface, NULL, NULL, NULL);
    /* Give the miniport time to report beacons, then repopulate. The ACM
     * scan_complete notification also repopulates if it arrives first. */
    SetTimer(hDlg, SCAN_TIMER_ID, SCAN_SETTLE_MS, NULL);
}

static INT_PTR CALLBACK
MainDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_INITDIALOG:
        {
            WCHAR szIf[256];
            PWLAN_INTERFACE_INFO_LIST pIfList = NULL;
            DWORD er;

            InitList(GetDlgItem(hDlg, IDC_NETLIST));

            er = g_hClient ? WlanEnumInterfaces(g_hClient, NULL, &pIfList)
                           : ERROR_SERVICE_NOT_ACTIVE;
            if (er == ERROR_SUCCESS &&
                pIfList && pIfList->dwNumberOfItems > 0)
            {
                g_Iface = pIfList->InterfaceInfo[0].InterfaceGuid;
                g_HaveIface = TRUE;
                lstrcpynW(szIf, pIfList->InterfaceInfo[0].strInterfaceDescription,
                          ARRAYSIZE(szIf));
                SetDlgItemTextW(hDlg, IDC_IFACE, szIf);
            }
            else
            {
                SetStatusRes(hDlg, g_hClient ? IDS_NOIFACE : IDS_NOSERVICE);
                EnableWindow(GetDlgItem(hDlg, IDC_REFRESH), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_CONNECT), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_DISCONNECT), FALSE);
            }
            if (pIfList)
                WlanFreeMemory(pIfList);

            if (g_HaveIface)
            {
                WlanRegisterNotification(g_hClient, WLAN_NOTIFICATION_SOURCE_ACM,
                    TRUE, WlanNotifyCallback, hDlg, NULL, NULL);
                /* Defer the first scan to a posted message so WM_INITDIALOG
                 * returns and the dialog paints immediately, even when
                 * wlansvc is still coming up (a blocking WlanScan here would
                 * otherwise leave the window unpainted). */
                PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_REFRESH, BN_CLICKED), 0);
            }
            /* Make sure the window is visible and on top: the selector may be
             * auto-started before the shell has settled the Z-order. */
            ShowWindow(hDlg, SW_SHOW);
            SetForegroundWindow(hDlg);
            BringWindowToTop(hDlg);
            return TRUE;
        }

        case WM_TIMER:
            if (wParam == SCAN_TIMER_ID)
            {
                KillTimer(hDlg, SCAN_TIMER_ID);
                PopulateList(hDlg);
                SetStatusRes(hDlg, IDS_DISCONNECTED);
            }
            return TRUE;

        case WM_WLAN_NOTIFY:
            switch (wParam)
            {
                case wlan_notification_acm_scan_complete:
                    KillTimer(hDlg, SCAN_TIMER_ID);
                    PopulateList(hDlg);
                    break;
                case wlan_notification_acm_connection_complete:
                    PopulateList(hDlg);
                    break;
                case wlan_notification_acm_disconnected:
                    PopulateList(hDlg);
                    SetStatusRes(hDlg, IDS_DISCONNECTED);
                    break;
            }
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_REFRESH:
                    StartScan(hDlg);
                    return TRUE;
                case IDC_CONNECT:
                    ConnectSelected(hDlg);
                    return TRUE;
                case IDC_DISCONNECT:
                    if (g_HaveIface)
                    {
                        WlanDisconnect(g_hClient, &g_Iface, NULL);
                        SetStatusRes(hDlg, IDS_DISCONNECTED);
                    }
                    return TRUE;
                case IDCANCEL:
                    EndDialog(hDlg, 0);
                    return TRUE;
            }
            break;

        case WM_NOTIFY:
        {
            LPNMHDR nm = (LPNMHDR)lParam;
            if (nm->idFrom == IDC_NETLIST && nm->code == NM_DBLCLK)
            {
                ConnectSelected(hDlg);
                return TRUE;
            }
            break;
        }

        case WM_DESTROY:
            KillTimer(hDlg, SCAN_TIMER_ID);
            if (g_pNetList)
            {
                WlanFreeMemory(g_pNetList);
                g_pNetList = NULL;
            }
            return TRUE;
    }
    return FALSE;
}

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow)
{
    INITCOMMONCONTROLSEX icc;
    DWORD dwVersion = 0;

    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShow);

    g_hInst = hInstance;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    /* A failure here still shows the dialog with a "service not running"
     * status so the user gets a diagnosable window rather than nothing. */
    WlanOpenHandle(WLAN_API_VERSION_2_0, NULL, &dwVersion, &g_hClient);

    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainDlgProc, 0);

    if (g_hClient)
        WlanCloseHandle(g_hClient, NULL);
    return 0;
}
