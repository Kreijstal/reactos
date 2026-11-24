/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Restart of a started network adapter, user-mode test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Device installation restarts a device that already runs, so that its
 * drivers pick up what the installation changed: the network class installer
 * disables and enables a started adapter for NDIS to bind the protocols anew
 * (CM_Disable_DevNode, CM_Enable_DevNode), the property change of a device is
 * a query-remove and a set-up (CM_Query_And_Remove_SubTree, CM_Setup_DevNode).
 * Either way the PnP manager removes the device and starts it again on the
 * same PDO, which its bus driver keeps for the device that is still there.
 * The adapter must come back: started, bound to TCP/IP, and with an address
 * again if it had one.
 */

#include <kmt_test.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>

static const GUID NetClassGuid =
    { 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

typedef enum _RESTART_METHOD
{
    RestartDisableEnable,
    RestartRemoveSetup,
} RESTART_METHOD;

static
BOOLEAN
FindStartedPciAdapter(
    _Out_ PDEVINST DevInst,
    _Out_writes_(MAX_DEVICE_ID_LEN) PWSTR InstanceId,
    _Out_writes_(64) PSTR AdapterName)
{
    SP_DEVINFO_DATA DevInfoData;
    HDEVINFO DevInfo;
    WCHAR Guid[64];
    DWORD Type, Size, i;
    ULONG Status, Problem;
    BOOLEAN Found = FALSE;
    HKEY Key;

    DevInfo = SetupDiGetClassDevsW(&NetClassGuid, NULL, NULL, DIGCF_PRESENT);
    ok(DevInfo != INVALID_HANDLE_VALUE, "SetupDiGetClassDevsW failed with %lu\n", GetLastError());
    if (DevInfo == INVALID_HANDLE_VALUE)
        return FALSE;

    DevInfoData.cbSize = sizeof(DevInfoData);
    for (i = 0; !Found && SetupDiEnumDeviceInfo(DevInfo, i, &DevInfoData); i++)
    {
        if (CM_Get_Device_IDW(DevInfoData.DevInst, InstanceId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
            continue;
        if (_wcsnicmp(InstanceId, L"PCI\\", 4) != 0)
            continue;
        if (CM_Get_DevNode_Status(&Status, &Problem, DevInfoData.DevInst, 0) != CR_SUCCESS ||
            !(Status & DN_STARTED))
            continue;

        Key = SetupDiOpenDevRegKey(DevInfo, &DevInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
        if (Key == INVALID_HANDLE_VALUE)
            continue;
        Size = sizeof(Guid) - sizeof(WCHAR);
        RtlZeroMemory(Guid, sizeof(Guid));
        if (RegQueryValueExW(Key, L"NetCfgInstanceId", NULL, &Type, (PBYTE)Guid, &Size) == ERROR_SUCCESS &&
            Type == REG_SZ)
        {
            WideCharToMultiByte(CP_ACP, 0, Guid, -1, AdapterName, 64, NULL, NULL);
            *DevInst = DevInfoData.DevInst;
            Found = TRUE;
        }
        RegCloseKey(Key);
    }

    SetupDiDestroyDeviceInfoList(DevInfo);
    return Found;
}

/* Whether TCP/IP has the adapter, and whether the adapter has an address */
static
BOOLEAN
AdapterBound(
    _In_ PCSTR AdapterName,
    _Out_opt_ PBOOLEAN HasAddress)
{
    PIP_ADAPTER_INFO Info, Current;
    ULONG Size = 0;
    BOOLEAN Bound = FALSE;

    if (HasAddress)
        *HasAddress = FALSE;
    if (GetAdaptersInfo(NULL, &Size) != ERROR_BUFFER_OVERFLOW)
        return FALSE;
    Info = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Info)
        return FALSE;
    if (GetAdaptersInfo(Info, &Size) == ERROR_SUCCESS)
    {
        for (Current = Info; Current; Current = Current->Next)
        {
            if (_stricmp(Current->AdapterName, AdapterName) != 0)
                continue;
            Bound = TRUE;
            if (HasAddress)
                *HasAddress = strcmp(Current->IpAddressList.IpAddress.String, "0.0.0.0") != 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, Info);
    return Bound;
}

static
BOOLEAN
WaitStarted(
    _In_ DEVINST DevInst,
    _In_ BOOLEAN Started,
    _In_ ULONG Tries)
{
    ULONG Status = 0, Problem = 0, i;
    CONFIGRET Cr;

    for (i = 0; i < Tries; i++)
    {
        Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
        if (Started ? (Cr == CR_SUCCESS && (Status & DN_STARTED))
                    : (Cr != CR_SUCCESS || !(Status & DN_STARTED)))
        {
            return TRUE;
        }
        Sleep(100);
    }
    trace("Device status: cr %lu, status 0x%lx, problem %lu\n", Cr, Status, Problem);
    return FALSE;
}

static
VOID
TestRestart(
    _In_ RESTART_METHOD Method)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    CHAR AdapterName[64];
    PNP_VETO_TYPE VetoType = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH];
    BOOLEAN WasBound = FALSE, HadAddress = FALSE, HasAddress = FALSE, Bound = FALSE;
    DEVINST DevInst;
    CONFIGRET Cr;
    ULONG i;

    trace("Restart by %s\n", Method == RestartDisableEnable ? "disable and enable" : "query-remove and set-up");

    if (!FindStartedPciAdapter(&DevInst, InstanceId, AdapterName))
    {
        skip(FALSE, "No started PCI network adapter\n");
        return;
    }
    trace("Adapter %S, %s\n", InstanceId, AdapterName);

    /* Let a starting adapter get its address first */
    for (i = 0; i < 300; i++)
    {
        WasBound = AdapterBound(AdapterName, &HadAddress);
        if (WasBound && HadAddress)
            break;
        Sleep(100);
    }
    trace("Before the restart: bound %d, address %d\n", WasBound, HadAddress);

    VetoName[0] = UNICODE_NULL;
    if (Method == RestartDisableEnable)
        Cr = CM_Disable_DevNode(DevInst, 0);
    else
        Cr = CM_Query_And_Remove_SubTreeW(DevInst, &VetoType, VetoName, RTL_NUMBER_OF(VetoName),
                                          CM_REMOVE_NO_RESTART);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
    {
        trace("Veto type %d, name %S\n", VetoType, VetoName);
        return;
    }

    /* The PDO stays, its device is no longer started */
    ok(WaitStarted(DevInst, FALSE, 100), "Adapter still started\n");
    ok_eq_ulong(CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_NORMAL), CR_SUCCESS);
    for (i = 0; i < 100 && AdapterBound(AdapterName, NULL); i++)
        Sleep(100);
    ok(!AdapterBound(AdapterName, NULL), "TCP/IP still has the removed adapter\n");

    if (Method == RestartDisableEnable)
        Cr = CM_Enable_DevNode(DevInst, 0);
    else
        Cr = CM_Setup_DevNode(DevInst, CM_SETUP_DEVNODE_READY);
    ok_eq_ulong(Cr, CR_SUCCESS);

    ok(WaitStarted(DevInst, TRUE, 100), "Adapter not started again\n");

    /* TCP/IP binds the adapter again, if it had it */
    if (!WasBound)
    {
        skip(FALSE, "TCP/IP did not have %s\n", AdapterName);
        return;
    }
    for (i = 0; i < 300; i++)
    {
        Bound = AdapterBound(AdapterName, &HasAddress);
        if (Bound && (HasAddress || !HadAddress))
            break;
        Sleep(100);
    }
    ok(Bound, "TCP/IP did not get %s back\n", AdapterName);
    if (HadAddress)
        ok(HasAddress, "%s has no address after the restart\n", AdapterName);
}

START_TEST(IoDeviceRestart)
{
    TestRestart(RestartDisableEnable);
    TestRestart(RestartRemoveSetup);
}
