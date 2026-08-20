/*
 * PROJECT:     ReactOS Bluetooth Control Panel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Control Panel entry point of bthprops.cpl
 * COPYRIGHT:   Copyright 2026 The ReactOS Project
 */

#include <stdarg.h>

#define WIN32_NO_STATUS

#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <commctrl.h>
#include <prsht.h>
#include <cpl.h>
#include <strsafe.h>

#include <bthsdpdef.h>
#include <bluetoothapis.h>

#include "resource.h"

static HINSTANCE hApplet = NULL;

/*
 * Returns TRUE when at least one local Bluetooth radio is present.
 *
 * Windows only surfaces the Bluetooth applet when the machine actually has a
 * Bluetooth radio, so we do the same instead of always advertising an applet
 * that cannot possibly do anything useful.
 */
static BOOL
HasBluetoothRadio(VOID)
{
    BLUETOOTH_FIND_RADIO_PARAMS FindParams;
    HBLUETOOTH_RADIO_FIND hFind;
    HANDLE hRadio = NULL;

    ZeroMemory(&FindParams, sizeof(FindParams));
    FindParams.dwSize = sizeof(FindParams);

    hFind = BluetoothFindFirstRadio(&FindParams, &hRadio);
    if (hFind == NULL)
        return FALSE;

    if (hRadio != NULL)
        CloseHandle(hRadio);

    BluetoothFindRadioClose(hFind);
    return TRUE;
}

static VOID
FormatBluetoothAddress(
    const BLUETOOTH_ADDRESS *pAddress,
    PWSTR pszBuffer,
    SIZE_T cchBuffer)
{
    StringCchPrintfW(pszBuffer, cchBuffer,
                     L"%02X:%02X:%02X:%02X:%02X:%02X",
                     pAddress->rgBytes[5], pAddress->rgBytes[4],
                     pAddress->rgBytes[3], pAddress->rgBytes[2],
                     pAddress->rgBytes[1], pAddress->rgBytes[0]);
}

static VOID
InitDeviceListColumns(
    HWND hwndList)
{
    LVCOLUMNW Column;
    WCHAR szText[64];
    RECT rc;
    INT cxClient;

    GetClientRect(hwndList, &rc);
    cxClient = rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL);
    if (cxClient < 100)
        cxClient = 100;

    ZeroMemory(&Column, sizeof(Column));
    Column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    Column.pszText = szText;

    LoadStringW(hApplet, IDS_COLUMN_DEVICE, szText, ARRAYSIZE(szText));
    Column.iSubItem = 0;
    Column.cx = (cxClient * 6) / 10;
    SendMessageW(hwndList, LVM_INSERTCOLUMNW, 0, (LPARAM)&Column);

    LoadStringW(hApplet, IDS_COLUMN_ADDRESS, szText, ARRAYSIZE(szText));
    Column.iSubItem = 1;
    Column.cx = cxClient - Column.cx;
    SendMessageW(hwndList, LVM_INSERTCOLUMNW, 1, (LPARAM)&Column);
}

static VOID
FillDeviceList(
    HWND hwndList)
{
    BLUETOOTH_DEVICE_SEARCH_PARAMS SearchParams;
    BLUETOOTH_DEVICE_INFO DeviceInfo;
    HBLUETOOTH_DEVICE_FIND hFind;
    WCHAR szAddress[32];
    LVITEMW Item;
    INT iItem = 0;

    SendMessageW(hwndList, LVM_DELETEALLITEMS, 0, 0);

    ZeroMemory(&SearchParams, sizeof(SearchParams));
    SearchParams.dwSize = sizeof(SearchParams);
    SearchParams.fReturnAuthenticated = TRUE;
    SearchParams.fReturnRemembered = TRUE;
    SearchParams.fReturnUnknown = TRUE;
    SearchParams.fReturnConnected = TRUE;
    SearchParams.fIssueInquiry = FALSE;
    SearchParams.cTimeoutMultiplier = 0;
    /* A NULL radio handle means "search on all local radios" */
    SearchParams.hRadio = NULL;

    ZeroMemory(&DeviceInfo, sizeof(DeviceInfo));
    DeviceInfo.dwSize = sizeof(DeviceInfo);

    hFind = BluetoothFindFirstDevice(&SearchParams, &DeviceInfo);
    if (hFind == NULL)
        return;

    do
    {
        ZeroMemory(&Item, sizeof(Item));
        Item.mask = LVIF_TEXT;
        Item.iItem = iItem;
        Item.pszText = DeviceInfo.szName;
        iItem = (INT)SendMessageW(hwndList, LVM_INSERTITEMW, 0, (LPARAM)&Item);
        if (iItem == -1)
            break;

        FormatBluetoothAddress(&DeviceInfo.Address, szAddress, ARRAYSIZE(szAddress));

        Item.iItem = iItem;
        Item.iSubItem = 1;
        Item.pszText = szAddress;
        SendMessageW(hwndList, LVM_SETITEMTEXTW, iItem, (LPARAM)&Item);

        iItem++;

        ZeroMemory(&DeviceInfo, sizeof(DeviceInfo));
        DeviceInfo.dwSize = sizeof(DeviceInfo);
    } while (BluetoothFindNextDevice(hFind, &DeviceInfo));

    BluetoothFindDeviceClose(hFind);
}

static INT_PTR CALLBACK
DevicesPageProc(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    HWND hwndList;

    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    switch (uMsg)
    {
        case WM_INITDIALOG:
            hwndList = GetDlgItem(hwndDlg, IDC_DEVICELIST);
            SendMessageW(hwndList, LVM_SETEXTENDEDLISTVIEWSTYLE,
                         LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
            InitDeviceListColumns(hwndList);
            FillDeviceList(hwndList);
            return TRUE;
    }

    return FALSE;
}

static VOID
ShowBluetoothProperties(
    HWND hwndParent)
{
    PROPSHEETPAGEW psp;
    PROPSHEETHEADERW psh;
    HPROPSHEETPAGE hpsp;
    INITCOMMONCONTROLSEX InitControls;

    InitControls.dwSize = sizeof(InitControls);
    InitControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&InitControls);

    ZeroMemory(&psp, sizeof(psp));
    psp.dwSize = sizeof(psp);
    psp.dwFlags = PSP_DEFAULT;
    psp.hInstance = hApplet;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_DEVICES);
    psp.pfnDlgProc = DevicesPageProc;

    hpsp = CreatePropertySheetPageW(&psp);
    if (hpsp == NULL)
        return;

    ZeroMemory(&psh, sizeof(psh));
    psh.dwSize = sizeof(psh);
    psh.dwFlags = PSH_USEICONID;
    psh.hwndParent = hwndParent;
    psh.hInstance = hApplet;
    psh.pszIcon = MAKEINTRESOURCEW(IDI_CPLICON);
    psh.pszCaption = MAKEINTRESOURCEW(IDS_CPLNAME);
    psh.nPages = 1;
    psh.nStartPage = 0;
    psh.phpage = &hpsp;

    if (PropertySheetW(&psh) < 0)
        DestroyPropertySheetPage(hpsp);
}

/*********************************************************************
 * CPlApplet (bthprops.cpl.@)
 *
 * Control Panel entry point.
 */
LONG CALLBACK
CPlApplet(
    HWND hwndCpl,
    UINT uMsg,
    LPARAM lParam1,
    LPARAM lParam2)
{
    UNREFERENCED_PARAMETER(lParam1);

    switch (uMsg)
    {
        case CPL_INIT:
            return TRUE;

        case CPL_GETCOUNT:
            /* Without a radio there is nothing to configure, so hide the applet */
            return HasBluetoothRadio() ? 1 : 0;

        case CPL_INQUIRE:
        {
            CPLINFO *pCplInfo = (CPLINFO *)lParam2;

            pCplInfo->lData = 0;
            pCplInfo->idIcon = IDI_CPLICON;
            pCplInfo->idName = IDS_CPLNAME;
            pCplInfo->idInfo = IDS_CPLDESCRIPTION;
            return 0;
        }

        case CPL_DBLCLK:
            ShowBluetoothProperties(hwndCpl);
            break;

        case CPL_STOP:
        case CPL_EXIT:
            break;
    }

    return FALSE;
}

BOOL WINAPI
DllMain(
    HINSTANCE hinstDLL,
    DWORD dwReason,
    LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);

    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
            hApplet = hinstDLL;
            DisableThreadLibraryCalls(hinstDLL);
            break;
    }

    return TRUE;
}
