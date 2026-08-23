/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Wireless network notification icon handler
 * COPYRIGHT:   Copyright 2026 The ReactOS Project
 */

#include "precomp.h"

#include <wlanapi.h>
#include <shlwapi.h>

/*
 * The wireless icon is the only entry point to the wireless UI that a user who
 * does not already know it exists will ever find, so it lives next to the
 * volume icon and behaves like the one in Windows: double-click opens the
 * network chooser, right-click offers the same thing plus Network Connections.
 *
 * Everything here is driven by the shared POLL_TIMER_ID tick in CSysTray, the
 * same way the volume and power icons are updated.  wlansvc is an RPC server
 * that may not be running when we start -- and on a machine with no wireless
 * hardware never will be -- so the client handle is opened lazily and every
 * failure simply leaves the icon hidden rather than being reported.
 */

static HICON g_hIconWireless = NULL;
static HANDLE g_hWlan = NULL;
static int g_nLevel = -1;             /* Level currently displayed, -1 = none */
static BOOL g_bVisible = FALSE;
static CString g_strWirelessTooltip;

/* Signal quality (0-100) to the number of bars we light up */
#define WIRELESS_LEVEL_HIDDEN       (-1)
#define WIRELESS_LEVEL_DISCONNECTED 0

static UINT
WirelessIconResource(int nLevel)
{
    switch (nLevel)
    {
        case 1:  return IDI_WIRELESS_1;
        case 2:  return IDI_WIRELESS_2;
        case 3:  return IDI_WIRELESS_3;
        case 4:  return IDI_WIRELESS_4;
        default: return IDI_WIRELESS_DISCONNECTED;
    }
}

/*++
* @name OpenWlanHandle
*
* Opens the WLAN client handle, or reports that it is not available yet.
*
* @return TRUE when g_hWlan is usable.
*
*--*/
static BOOL
OpenWlanHandle(VOID)
{
    DWORD dwNegotiatedVersion;
    DWORD dwError;

    if (g_hWlan != NULL)
        return TRUE;

    dwError = WlanOpenHandle(WLAN_API_VERSION_2_0, NULL, &dwNegotiatedVersion, &g_hWlan);
    if (dwError != ERROR_SUCCESS)
    {
        /*
         * wlansvc is demand-started and may lose its RPC binding across a
         * service restart.  Leave the handle NULL and try again on the next
         * poll: there is nothing to report to the user about this.
         */
        g_hWlan = NULL;
        return FALSE;
    }

    return TRUE;
}

static VOID
CloseWlanHandle(VOID)
{
    if (g_hWlan != NULL)
    {
        WlanCloseHandle(g_hWlan, NULL);
        g_hWlan = NULL;
    }
}

/*++
* @name QueryWirelessState
*
* Works out what the icon should show: whether there is a wireless adapter at
* all, and if it is associated, the SSID and how strong the signal is.
*
* @param pnLevel
*        Receives WIRELESS_LEVEL_HIDDEN when there is no wireless adapter,
*        WIRELESS_LEVEL_DISCONNECTED when there is one but it is not
*        associated, or 1-4 for the signal strength.
*
* @param strTooltip
*        Receives the tooltip text describing that state.
*
*--*/
static VOID
QueryWirelessState(_Out_ int *pnLevel, _Out_ CString &strTooltip)
{
    PWLAN_INTERFACE_INFO_LIST pIfList = NULL;
    PWLAN_CONNECTION_ATTRIBUTES pConn = NULL;
    DWORD dwSize = 0, dwError, i;
    WLAN_OPCODE_VALUE_TYPE opCode;

    *pnLevel = WIRELESS_LEVEL_HIDDEN;

    if (!OpenWlanHandle())
        return;

    dwError = WlanEnumInterfaces(g_hWlan, NULL, &pIfList);
    if (dwError != ERROR_SUCCESS || pIfList == NULL)
    {
        /*
         * The handle can go stale when wlansvc restarts, and every later call
         * on it would fail the same way.  Drop it so the next poll reopens.
         */
        CloseWlanHandle();
        return;
    }

    if (pIfList->dwNumberOfItems == 0)
    {
        /* No wireless hardware: the icon has nothing to say, so it stays away */
        WlanFreeMemory(pIfList);
        return;
    }

    /*
     * Only the first adapter is represented.  A second wireless adapter is rare
     * enough that one icon per adapter would cost more tray space than it is
     * worth, and Windows shows one icon here too.
     */
    *pnLevel = WIRELESS_LEVEL_DISCONNECTED;
    strTooltip.LoadStringW(IDS_WIRELESS_NOT_CONNECTED);

    for (i = 0; i < pIfList->dwNumberOfItems; i++)
    {
        if (pIfList->InterfaceInfo[i].isState != wlan_interface_state_connected)
            continue;

        dwError = WlanQueryInterface(g_hWlan,
                                     &pIfList->InterfaceInfo[i].InterfaceGuid,
                                     wlan_intf_opcode_current_connection,
                                     NULL,
                                     &dwSize,
                                     (PVOID *)&pConn,
                                     &opCode);
        if (dwError != ERROR_SUCCESS || pConn == NULL)
            continue;

        if (pConn->isState == wlan_interface_state_connected)
        {
            WLAN_SIGNAL_QUALITY Quality = pConn->wlanAssociationAttributes.wlanSignalQuality;
            PDOT11_SSID pSsid = &pConn->wlanAssociationAttributes.dot11Ssid;
            CStringW strSsid;
            CStringW strFormat;

            /*
             * Signal quality is a percentage; split it into the four bars the
             * icons draw.  Anything above zero lights at least one bar, so a
             * working-but-weak link never looks the same as no link at all.
             */
            if (Quality >= 75)
                *pnLevel = 4;
            else if (Quality >= 50)
                *pnLevel = 3;
            else if (Quality >= 25)
                *pnLevel = 2;
            else
                *pnLevel = 1;

            /*
             * An SSID is a byte string of uSSIDLength bytes and is not NUL
             * terminated.  It is conventionally UTF-8 but nothing enforces
             * that, so convert only the bytes the adapter reported.
             */
            if (pSsid->uSSIDLength > 0 && pSsid->uSSIDLength <= DOT11_SSID_MAX_LENGTH)
            {
                int cch = MultiByteToWideChar(CP_UTF8, 0,
                                              (LPCSTR)pSsid->ucSSID, pSsid->uSSIDLength,
                                              NULL, 0);
                if (cch > 0)
                {
                    LPWSTR pszBuf = strSsid.GetBuffer(cch + 1);
                    MultiByteToWideChar(CP_UTF8, 0,
                                        (LPCSTR)pSsid->ucSSID, pSsid->uSSIDLength,
                                        pszBuf, cch);
                    pszBuf[cch] = UNICODE_NULL;
                    strSsid.ReleaseBuffer();
                }
            }

            if (strSsid.IsEmpty())
                strSsid.LoadStringW(IDS_WIRELESS_HIDDEN_NETWORK);

            strFormat.LoadStringW(IDS_WIRELESS_CONNECTED_TO);
            strTooltip.Format(strFormat, strSsid.GetString(), (int)Quality);
        }

        WlanFreeMemory(pConn);
        pConn = NULL;
        break;
    }

    WlanFreeMemory(pIfList);
}

/*++
* @name _RunWirelessUI
*
* Opens the wireless network chooser.
*
*--*/
static VOID
_RunWirelessUI(_In_ CSysTray * pSysTray)
{
    ShellExecuteW(pSysTray->GetHWnd(), L"open", L"wlanui.exe", NULL, NULL, SW_SHOWNORMAL);
}

static VOID
_RunNetworkConnections(_In_ CSysTray * pSysTray)
{
    ShellExecuteW(pSysTray->GetHWnd(), L"open", L"rundll32.exe",
                  L"shell32.dll,Control_RunDLL ncpa.cpl", NULL, SW_SHOWNORMAL);
}

static VOID
_ShowContextMenu(_In_ CSysTray * pSysTray)
{
    WCHAR strView[128];
    WCHAR strOpen[128];
    HMENU hPopup;
    POINT pt;
    DWORD id;

    LoadStringW(g_hInstance, IDS_WIRELESS_VIEW_NETWORKS, strView, _countof(strView));
    LoadStringW(g_hInstance, IDS_WIRELESS_OPEN_CONNECTIONS, strOpen, _countof(strOpen));

    hPopup = CreatePopupMenu();
    if (hPopup == NULL)
        return;

    AppendMenuW(hPopup, MF_STRING, IDS_WIRELESS_VIEW_NETWORKS, strView);
    AppendMenuW(hPopup, MF_STRING, IDS_WIRELESS_OPEN_CONNECTIONS, strOpen);
    SetMenuDefaultItem(hPopup, IDS_WIRELESS_VIEW_NETWORKS, FALSE);

    /*
     * Right/bottom aligned and owned by the systray window, exactly as the
     * volume and hotplug menus are: the cursor sits a few pixels from the
     * bottom-right corner when the icon is clicked, so a menu laid out down
     * and to the right of it would be placed off-screen.
     */
    SetForegroundWindow(pSysTray->GetHWnd());
    GetCursorPos(&pt);

    id = TrackPopupMenuEx(hPopup,
                          TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                          pt.x, pt.y, pSysTray->GetHWnd(), NULL);

    DestroyMenu(hPopup);

    switch (id)
    {
        case IDS_WIRELESS_VIEW_NETWORKS:
            _RunWirelessUI(pSysTray);
            break;
        case IDS_WIRELESS_OPEN_CONNECTIONS:
            _RunNetworkConnections(pSysTray);
            break;
    }
}

/*++
* @name Wireless_UpdateIcon
*
* Brings the tray icon in line with nLevel, adding, hiding or reloading it only
* when something actually changed.
*
*--*/
static HRESULT
Wireless_UpdateIcon(_In_ CSysTray * pSysTray, int nLevel, CString &strTooltip)
{
    BOOL bVisible = (nLevel != WIRELESS_LEVEL_HIDDEN);
    BOOL bWasVisible = g_bVisible;
    HRESULT hr;

    if (nLevel == g_nLevel && bVisible == g_bVisible)
    {
        /*
         * The tooltip carries the signal percentage, which changes constantly
         * on an idle link.  Refresh the text without reloading the icon.
         */
        if (bVisible && strTooltip != g_strWirelessTooltip)
        {
            g_strWirelessTooltip = strTooltip;
            return pSysTray->NotifyIcon(NIM_MODIFY, ID_ICON_WIRELESS,
                                        g_hIconWireless, g_strWirelessTooltip);
        }
        return S_OK;
    }

    if (bVisible && nLevel != g_nLevel)
    {
        HICON hNew = (HICON)LoadImageW(g_hInstance,
                                       MAKEINTRESOURCEW(WirelessIconResource(nLevel)),
                                       IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON),
                                       0);
        if (hNew != NULL)
        {
            if (g_hIconWireless != NULL)
                DestroyIcon(g_hIconWireless);
            g_hIconWireless = hNew;
        }
    }

    g_nLevel = nLevel;
    g_bVisible = bVisible;
    g_strWirelessTooltip = strTooltip;

    if (!bVisible)
    {
        hr = bWasVisible
            ? pSysTray->NotifyIcon(NIM_DELETE, ID_ICON_WIRELESS,
                                   g_hIconWireless, g_strWirelessTooltip)
            : S_OK;
        if (g_hIconWireless != NULL)
        {
            DestroyIcon(g_hIconWireless);
            g_hIconWireless = NULL;
        }
        return hr;
    }

    return pSysTray->NotifyIcon(bWasVisible ? NIM_MODIFY : NIM_ADD,
                                ID_ICON_WIRELESS,
                                g_hIconWireless, g_strWirelessTooltip);
}

/* HANDLER ENTRY POINTS ******************************************************/

HRESULT STDMETHODCALLTYPE Wireless_Init(_In_ CSysTray * pSysTray)
{
    int nLevel;
    CString strTooltip;

    TRACE("Wireless_Init\n");

    QueryWirelessState(&nLevel, strTooltip);

    if (strTooltip.IsEmpty())
        strTooltip.LoadStringW(IDS_WIRELESS_NOT_CONNECTED);

    g_nLevel = nLevel;
    g_bVisible = (nLevel != WIRELESS_LEVEL_HIDDEN);
    g_strWirelessTooltip = strTooltip;

    if (!g_bVisible)
        return S_OK;

    g_hIconWireless = (HICON)LoadImageW(g_hInstance,
                                        MAKEINTRESOURCEW(WirelessIconResource(nLevel)),
                                        IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON),
                                        0);

    return pSysTray->NotifyIcon(NIM_ADD, ID_ICON_WIRELESS,
                                g_hIconWireless, g_strWirelessTooltip);
}

HRESULT STDMETHODCALLTYPE Wireless_Update(_In_ CSysTray * pSysTray)
{
    int nLevel;
    CString strTooltip;

    QueryWirelessState(&nLevel, strTooltip);

    if (strTooltip.IsEmpty())
        strTooltip.LoadStringW(IDS_WIRELESS_NOT_CONNECTED);

    return Wireless_UpdateIcon(pSysTray, nLevel, strTooltip);
}

HRESULT STDMETHODCALLTYPE Wireless_Shutdown(_In_ CSysTray * pSysTray)
{
    TRACE("Wireless_Shutdown\n");

    CloseWlanHandle();

    if (g_hIconWireless != NULL)
    {
        DestroyIcon(g_hIconWireless);
        g_hIconWireless = NULL;
    }

    g_nLevel = WIRELESS_LEVEL_HIDDEN;
    g_bVisible = FALSE;

    return pSysTray->NotifyIcon(NIM_DELETE, ID_ICON_WIRELESS, NULL, NULL);
}

HRESULT STDMETHODCALLTYPE Wireless_Message(_In_ CSysTray * pSysTray, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT &lResult)
{
    TRACE("Wireless_Message uMsg=%d, wParam=%x, lParam=%x\n", uMsg, wParam, lParam);

    switch (uMsg)
    {
        case WM_USER + 220:
            if (wParam == WIRELESS_SERVICE_FLAG)
            {
                if (lParam)
                {
                    pSysTray->EnableService(WIRELESS_SERVICE_FLAG, TRUE);
                    return Wireless_Init(pSysTray);
                }
                else
                {
                    pSysTray->EnableService(WIRELESS_SERVICE_FLAG, FALSE);
                    return Wireless_Shutdown(pSysTray);
                }
            }
            return S_FALSE;

        case WM_USER + 221:
            if (wParam == WIRELESS_SERVICE_FLAG)
            {
                lResult = (LRESULT)pSysTray->IsServiceEnabled(WIRELESS_SERVICE_FLAG);
                return S_OK;
            }
            return S_FALSE;

        case WM_TIMER:
            if (wParam == WIRELESS_TIMER_ID)
            {
                /*
                 * A single click that was not followed by a second one inside
                 * the double-click time: the chooser is what a single click is
                 * for, matching the volume icon's single-click slider.
                 */
                KillTimer(pSysTray->GetHWnd(), WIRELESS_TIMER_ID);
                _RunWirelessUI(pSysTray);
            }
            break;

        case ID_ICON_WIRELESS:
            switch (lParam)
            {
                case WM_LBUTTONDOWN:
                    SetTimer(pSysTray->GetHWnd(), WIRELESS_TIMER_ID, GetDoubleClickTime(), NULL);
                    break;

                case WM_LBUTTONDBLCLK:
                    KillTimer(pSysTray->GetHWnd(), WIRELESS_TIMER_ID);
                    _RunWirelessUI(pSysTray);
                    break;

                case WM_RBUTTONUP:
                    _ShowContextMenu(pSysTray);
                    break;

                default:
                    break;
            }
            return S_OK;

        default:
            return S_FALSE;
    }

    return S_FALSE;
}

/* EOF */
