/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test disabling and enabling a wireless adapter
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * CM_Disable_DevNode on a Native 802.11 miniport, then CM_Enable_DevNode:
 *   - while disabled the device has CM_PROB_DISABLED, and its driver image is
 *     unloaded once its last device is gone;
 *   - enabled again, the driver is loaded anew and the adapter works: nwifi
 *     rebinds and its upper miniport comes back, so a connection brings the
 *     IP interface up.
 * Uses the vwifi test miniport (ROOT\VWIFI), or the device whose hardware ID
 * WLANAPI_TEST_DEVICE names.  Skipped without one.
 */

#include <apitest.h>
#include <wlanapi.h>
#include <winuser.h>
#include <winreg.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <stdio.h>

#define OPEN_SSID L"ReactOS-Open"
static const WCHAR ProfileName[] = L"rostest-restart";

typedef struct _MODULE_ENTRY
{
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} MODULE_ENTRY;

typedef struct _MODULE_LIST
{
    ULONG NumberOfModules;
    MODULE_ENTRY Modules[1];
} MODULE_LIST;

typedef LONG (NTAPI *PFN_QUERY)(ULONG, PVOID, ULONG, PULONG);

/* Is ImageName (e.g. "vwifi.sys") a loaded kernel module? */
static BOOL
IsModuleLoaded(PCSTR ImageName)
{
    PFN_QUERY Query = (PFN_QUERY)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                "NtQuerySystemInformation");
    ULONG Size = 256 * 1024, i;
    MODULE_LIST *List;
    BOOL Found = FALSE;

    List = HeapAlloc(GetProcessHeap(), 0, Size);
    if (List == NULL || Query == NULL ||
        Query(11 /* SystemModuleInformation */, List, Size, &Size) < 0)
    {
        HeapFree(GetProcessHeap(), 0, List);
        return FALSE;
    }
    for (i = 0; i < List->NumberOfModules; i++)
    {
        MODULE_ENTRY *Module = &List->Modules[i];

        if (Module->OffsetToFileName < sizeof(Module->FullPathName) &&
            _stricmp((PCSTR)Module->FullPathName + Module->OffsetToFileName,
                     ImageName) == 0)
        {
            Found = TRUE;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, List);
    return Found;
}

/* The device instance and its service name for the hardware ID. */
static BOOL
FindDevice(PCWSTR HardwareId, DEVINST *DevInst, PSTR Service, DWORD ServiceSize)
{
    HDEVINFO Set;
    SP_DEVINFO_DATA Dev;
    WCHAR Ids[1024];
    DWORD i;
    BOOL Found = FALSE;

    Set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (Set == INVALID_HANDLE_VALUE)
        return FALSE;
    Dev.cbSize = sizeof(Dev);
    for (i = 0; !Found && SetupDiEnumDeviceInfo(Set, i, &Dev); i++)
    {
        PCWSTR Id;

        ZeroMemory(Ids, sizeof(Ids));
        if (!SetupDiGetDeviceRegistryPropertyW(Set, &Dev, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)Ids, sizeof(Ids) - 2 * sizeof(WCHAR),
                                               NULL))
            continue;
        for (Id = Ids; *Id; Id += wcslen(Id) + 1)
        {
            if (_wcsicmp(Id, HardwareId) == 0 &&
                SetupDiGetDeviceRegistryPropertyA(Set, &Dev, SPDRP_SERVICE, NULL,
                                                  (PBYTE)Service, ServiceSize, NULL))
            {
                *DevInst = Dev.DevInst;
                Found = TRUE;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(Set);
    return Found;
}

static BOOL
WaitForModule(PCSTR ImageName, BOOL Loaded, DWORD Seconds)
{
    DWORD i;

    for (i = 0; i < Seconds * 10; i++)
    {
        if (IsModuleLoaded(ImageName) == Loaded)
            return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static BOOL
WaitForStarted(DEVINST DevInst, DWORD Seconds)
{
    ULONG Status, Problem;
    DWORD i;

    for (i = 0; i < Seconds * 10; i++)
    {
        if (CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0) == CR_SUCCESS &&
            (Status & DN_STARTED) && !(Status & DN_HAS_PROBLEM))
            return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static BOOL
FirstInterface(HANDLE Client, GUID *Interface)
{
    PWLAN_INTERFACE_INFO_LIST List = NULL;
    BOOL Found = FALSE;

    if (WlanEnumInterfaces(Client, NULL, &List) == ERROR_SUCCESS &&
        List != NULL && List->dwNumberOfItems != 0)
    {
        *Interface = List->InterfaceInfo[0].InterfaceGuid;
        Found = TRUE;
    }
    if (List != NULL)
        WlanFreeMemory(List);
    return Found;
}

/* Wireless IP interfaces, and how many of them are up. */
static VOID
CountWirelessInterfaces(PULONG Count, PULONG Up)
{
    PIP_ADAPTER_ADDRESSES Addresses, Adapter;
    ULONG Size = 64 * 1024;

    *Count = *Up = 0;
    Addresses = HeapAlloc(GetProcessHeap(), 0, Size);
    if (Addresses == NULL)
        return;
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, Addresses, &Size) == ERROR_SUCCESS)
    {
        for (Adapter = Addresses; Adapter != NULL; Adapter = Adapter->Next)
        {
            if (Adapter->IfType != IF_TYPE_IEEE80211)
                continue;
            (*Count)++;
            if (Adapter->OperStatus == IfOperStatusUp)
                (*Up)++;
        }
    }
    HeapFree(GetProcessHeap(), 0, Addresses);
}

static BOOL
ConnectOpen(HANDLE Client, const GUID *Interface, DWORD Seconds)
{
    WCHAR Xml[1024];
    WLAN_CONNECTION_PARAMETERS Params;
    PWLAN_CONNECTION_ATTRIBUTES Attributes;
    DWORD Reason = 0, Size, i;

    _snwprintf(Xml, ARRAYSIZE(Xml),
        L"<?xml version=\"1.0\"?>"
        L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        L"<name>%s</name>"
        L"<SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>"
        L"<connectionType>ESS</connectionType>"
        L"<connectionMode>manual</connectionMode>"
        L"<MSM><security><authEncryption>"
        L"<authentication>open</authentication><encryption>none</encryption>"
        L"<useOneX>false</useOneX>"
        L"</authEncryption></security></MSM>"
        L"</WLANProfile>",
        ProfileName, OPEN_SSID);
    Xml[ARRAYSIZE(Xml) - 1] = UNICODE_NULL;
    if (WlanSetProfile(Client, Interface, 0, Xml, NULL, TRUE, NULL, &Reason) != ERROR_SUCCESS)
        return FALSE;

    ZeroMemory(&Params, sizeof(Params));
    Params.wlanConnectionMode = wlan_connection_mode_profile;
    Params.strProfile = ProfileName;
    Params.dot11BssType = dot11_BSS_type_infrastructure;
    if (WlanConnect(Client, Interface, &Params, NULL) != ERROR_SUCCESS)
        return FALSE;

    for (i = 0; i < Seconds; i++)
    {
        Attributes = NULL;
        if (WlanQueryInterface(Client, Interface, wlan_intf_opcode_current_connection,
                               NULL, &Size, (PVOID *)&Attributes, NULL) == ERROR_SUCCESS &&
            Attributes != NULL)
        {
            BOOL Connected = (Attributes->isState == wlan_interface_state_connected);
            WlanFreeMemory(Attributes);
            if (Connected)
                return TRUE;
        }
        Sleep(1000);
    }
    return FALSE;
}

START_TEST(devrestart)
{
    WCHAR HardwareId[200] = L"ROOT\\VWIFI";
    CHAR Service[64], Image[80];
    DEVINST DevInst;
    ULONG Status, Problem, Count, Up;
    HANDLE Client;
    DWORD Negotiated, i;
    GUID Interface;
    BOOL Connected = FALSE;

    GetEnvironmentVariableW(L"WLANAPI_TEST_DEVICE", HardwareId, ARRAYSIZE(HardwareId));
    if (!FindDevice(HardwareId, &DevInst, Service, sizeof(Service)))
    {
        skip("No device %ls\n", HardwareId);
        return;
    }
    sprintf(Image, "%s.sys", Service);
    if (!IsModuleLoaded(Image) || !WaitForStarted(DevInst, 1))
    {
        skip("%ls is not running\n", HardwareId);
        return;
    }

    /* Disabled: a problem of its own, and no driver left in memory. */
    ok(CM_Disable_DevNode(DevInst, 0) == CR_SUCCESS, "CM_Disable_DevNode failed\n");
    ok(CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0) == CR_SUCCESS,
       "CM_Get_DevNode_Status failed\n");
    ok(!(Status & DN_STARTED), "Disabled device still started (0x%lx)\n", Status);
    ok((Status & DN_HAS_PROBLEM) && Problem == CM_PROB_DISABLED,
       "Disabled device: status 0x%lx problem %lu, expected CM_PROB_DISABLED\n",
       Status, Problem);
    ok(WaitForModule(Image, FALSE, 10), "%s still loaded with its device disabled\n", Image);

    /* Enabled: started again on a freshly loaded driver. */
    ok(CM_Enable_DevNode(DevInst, 0) == CR_SUCCESS, "CM_Enable_DevNode failed\n");
    ok(WaitForStarted(DevInst, 20), "Device did not start again\n");
    ok(IsModuleLoaded(Image), "%s not loaded after the enable\n", Image);

    /* And it works: the wireless interface is back, connects, and its IP
     * interface comes up -- one of it, not a stale one beside it. */
    if (WlanOpenHandle(2, NULL, &Negotiated, &Client) != ERROR_SUCCESS)
    {
        skip("WlanSvc is not running\n");
        return;
    }
    for (i = 0; i < 100 && !FirstInterface(Client, &Interface); i++)
        Sleep(200);
    ok(i < 100, "No wireless interface after the enable\n");
    if (i < 100)
    {
        Connected = ConnectOpen(Client, &Interface, 30);
        ok(Connected, "No connection to %ls after the enable\n", OPEN_SSID);
    }
    for (i = 0; i < 20; i++)
    {
        CountWirelessInterfaces(&Count, &Up);
        if (Up != 0)
            break;
        Sleep(500);
    }
    ok(Count == 1, "%lu wireless IP interfaces, expected 1\n", Count);
    ok(Up == 1, "The wireless IP interface is not up\n");

    if (i < 100 && Connected)
        WlanDisconnect(Client, &Interface, NULL);
    WlanDeleteProfile(Client, &Interface, ProfileName, NULL);
    WlanCloseHandle(Client, NULL);
}
