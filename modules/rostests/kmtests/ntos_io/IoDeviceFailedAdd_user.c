/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Restart of a device whose AddDevice failed, user-mode test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A device can get its function driver before it is installed: the kernel
 * binds a boot device through the CriticalDeviceDatabase. The driver may
 * then fail AddDevice for lack of what the installation writes (NDIS fails
 * a network adapter without its Linkage key with STATUS_OBJECT_NAME_NOT_FOUND),
 * and the device is left with problem CM_PROB_FAILED_ADD while its bus
 * driver keeps the PDO. Once user mode has installed the device,
 * CM_Setup_DevNode(CM_SETUP_DEVNODE_READY) restarts a device that does not
 * run because of such a problem: AddDevice is called again, and the device
 * starts.
 */

#include <kmt_test.h>
#include <cfgmgr32.h>

#include "../kmtest/kmtest.h"
#include "IoDeviceFailedAdd.h"

#define SERVICE_KEY     L"SYSTEM\\CurrentControlSet\\Services\\" IOFAILEDADD_SERVICE_NAME
#define PARAMETERS_KEY  SERVICE_KEY L"\\Parameters"
#define ENUM_KEY        L"SYSTEM\\CurrentControlSet\\Enum\\"
#define CHILD_KEY       ENUM_KEY IOFAILEDADD_CHILD_INSTANCE_PATH

static
CONFIGRET
SetProperty(
    _In_ DEVINST DevInst,
    _In_ ULONG Property,
    _In_ PCWSTR Value,
    _In_ BOOLEAN MultiSz)
{
    WCHAR Buffer[128];
    ULONG Length = (ULONG)((wcslen(Value) + 1) * sizeof(WCHAR));

    wcscpy(Buffer, Value);
    if (MultiSz)
    {
        Buffer[wcslen(Value) + 1] = UNICODE_NULL;
        Length += sizeof(WCHAR);
    }
    return CM_Set_DevNode_Registry_PropertyW(DevInst, Property, Buffer, Length, 0);
}

static
ULONG
GetCounter(
    _In_ PCWSTR Name)
{
    DWORD Value = 0, Size = sizeof(Value), Type = REG_NONE;
    HKEY Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY, 0, KEY_QUERY_VALUE, &Key))
        return 0;
    if (RegQueryValueExW(Key, Name, NULL, &Type, (BYTE *)&Value, &Size) || Type != REG_DWORD)
        Value = 0;
    RegCloseKey(Key);
    return Value;
}

static
LONG
SetString(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_ PCWSTR Value)
{
    return RegSetValueExW(Key, Name, 0, REG_SZ, (const BYTE *)Value,
                          (DWORD)((wcslen(Value) + 1) * sizeof(WCHAR)));
}

static
VOID
DeleteChildKey(VOID)
{
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, CHILD_KEY L"\\Device Parameters");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, CHILD_KEY L"\\Control");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, CHILD_KEY L"\\LogConf");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, CHILD_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ENUM_KEY IOFAILEDADD_CHILD_DEVICE_ID);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ENUM_KEY IOFAILEDADD_CHILD_ENUMERATOR);
}

/* Waits for the device to start, or (FailedAdds != 0) for it to have failed AddDevice */
static
BOOLEAN
WaitDevice(
    _In_ DEVINST DevInst,
    _In_ ULONG FailedAdds,
    _Out_ PULONG Status,
    _Out_ PULONG Problem)
{
    CONFIGRET Cr = CR_FAILURE;
    ULONG i;

    *Status = 0;
    *Problem = 0;
    for (i = 0; i < 100; i++)
    {
        Cr = CM_Get_DevNode_Status(Status, Problem, DevInst, 0);
        if (Cr == CR_SUCCESS && (*Status & DN_STARTED))
            return TRUE;
        if (FailedAdds != 0 && GetCounter(IOFAILEDADD_VALUE_FAILED_ADDS) >= FailedAdds &&
            Cr == CR_SUCCESS && (*Status & DN_HAS_PROBLEM))
        {
            return TRUE;
        }
        Sleep(100);
    }
    return FALSE;
}

static
VOID
TestFailedAdd(
    _In_ DEVINST Bus)
{
    DEVINST Child;
    CONFIGRET Cr;
    ULONG Status, Problem, i;
    BOOL Started;
    DWORD Installed = 1;
    DWORD Error;
    HKEY Key;

    /* The bus reports the child, whose function driver fails AddDevice */
    for (i = 0; i < 100; i++)
    {
        Cr = CM_Locate_DevNodeW(&Child, (DEVINSTID_W)IOFAILEDADD_CHILD_INSTANCE_PATH, CM_LOCATE_DEVNODE_NORMAL);
        if (Cr == CR_SUCCESS)
            break;
        Sleep(100);
    }
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        return;

    ok(WaitDevice(Child, 1, &Status, &Problem), "AddDevice of the child did not fail\n");
    trace("Before the installation: status 0x%lx, problem %lu\n", Status, Problem);
    ok(!(Status & DN_STARTED), "Child started without its registry value\n");
    ok(Status & DN_HAS_PROBLEM, "No problem, status 0x%lx\n", Status);
    ok_eq_ulong(Problem, (ULONG)CM_PROB_FAILED_ADD);
    ok_eq_ulong(GetCounter(IOFAILEDADD_VALUE_FAILED_ADDS), 1UL);
    ok_eq_ulong(GetCounter(IOFAILEDADD_VALUE_DEVICE_ADDS), 0UL);

    /* The bus driver keeps the PDO of the child */
    ok_eq_ulong(CM_Locate_DevNodeW(&Child, (DEVINSTID_W)IOFAILEDADD_CHILD_INSTANCE_PATH,
                                   CM_LOCATE_DEVNODE_NORMAL), CR_SUCCESS);

    /* The installation writes what the driver needs, in IoOpenDeviceRegistryKey's device key */
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, CHILD_KEY L"\\Device Parameters", 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &Key, NULL);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;
    Error = RegSetValueExW(Key, IOFAILEDADD_VALUE_INSTALLED, 0, REG_DWORD,
                           (const BYTE *)&Installed, sizeof(Installed));
    ok_eq_int(Error, ERROR_SUCCESS);
    RegCloseKey(Key);

    /* And has the device started: AddDevice anew, then IRP_MN_START_DEVICE */
    Cr = CM_Setup_DevNode(Child, CM_SETUP_DEVNODE_READY);
    ok_eq_ulong(Cr, CR_SUCCESS);
    /* The call goes first: the message reads what it returns */
    Started = WaitDevice(Child, 0, &Status, &Problem);
    ok(Started, "Child not started: status 0x%lx, problem %lu\n", Status, Problem);
    trace("After the installation: status 0x%lx, problem %lu\n", Status, Problem);
    ok(!(Status & DN_HAS_PROBLEM), "Problem %lu left\n", Problem);
    ok_eq_ulong(GetCounter(IOFAILEDADD_VALUE_FAILED_ADDS), 1UL);
    ok_eq_ulong(GetCounter(IOFAILEDADD_VALUE_DEVICE_ADDS), 1UL);
    ok_eq_ulong(GetCounter(IOFAILEDADD_VALUE_STARTS), 1UL);

    UNREFERENCED_PARAMETER(Bus);
}

START_TEST(IoDeviceFailedAdd)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    PNP_VETO_TYPE VetoType = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH];
    SC_HANDLE Service = NULL;
    DEVINST Root, Bus;
    ULONG Status, Problem;
    BOOL Started;
    CONFIGRET Cr;
    DWORD Error;
    HKEY Key;

    Error = KmtCreateService(IOFAILEDADD_SERVICE_NAME, IOFAILEDADD_IMAGE_NAME, NULL, &Service);
    if (Error == ERROR_SERVICE_EXISTS)
        Error = ERROR_SUCCESS;
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);
    DeleteChildKey();

    /*
     * The child is bound to its function driver before it gets installed,
     * like a CriticalDeviceDatabase device: Service, no driver key yet. Its
     * Class tells user mode that there is nothing to install for it.
     */
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, CHILD_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &Key, NULL);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        goto Cleanup;
    ok_eq_int(SetString(Key, L"Service", IOFAILEDADD_SERVICE_NAME), ERROR_SUCCESS);
    ok_eq_int(SetString(Key, L"Class", L"System"), ERROR_SUCCESS);
    ok_eq_int(SetString(Key, L"ClassGUID", L"{4d36e97d-e325-11ce-bfc1-08002be10318}"), ERROR_SUCCESS);
    RegCloseKey(Key);

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        goto Cleanup;

    /* The bus: registry first, as a phantom, so that nothing tries to install it */
    Cr = CM_Create_DevNodeW(&Bus, (DEVINSTID_W)IOFAILEDADD_BUS_ID, Root,
                            CM_CREATE_DEVNODE_GENERATE_ID | CM_CREATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        goto Cleanup;
    Cr = CM_Get_Device_IDW(Bus, InstanceId, MAX_DEVICE_ID_LEN, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);
    trace("Bus %S\n", InstanceId);

    ok_eq_ulong(SetProperty(Bus, CM_DRP_CLASS, L"System", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Bus, CM_DRP_CLASSGUID, L"{4d36e97d-e325-11ce-bfc1-08002be10318}", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Bus, CM_DRP_HARDWAREID, IOFAILEDADD_BUS_ID, TRUE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Bus, CM_DRP_DEVICEDESC, L"Failed AddDevice test bus", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Bus, CM_DRP_SERVICE, IOFAILEDADD_SERVICE_NAME, FALSE), CR_SUCCESS);

    Cr = CM_Create_DevNodeW(&Bus, InstanceId, Root, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        goto Uninstall;
    Cr = CM_Setup_DevNode(Bus, CM_SETUP_DEVNODE_READY);
    ok_eq_ulong(Cr, CR_SUCCESS);
    /* The call goes first: the message reads what it returns */
    Started = WaitDevice(Bus, 0, &Status, &Problem);
    ok(Started, "Bus not started: status 0x%lx, problem %lu\n", Status, Problem);

    if (Status & DN_STARTED)
        TestFailedAdd(Bus);
    else
        skip(FALSE, "No bus\n");

    /* The child goes with its bus */
    VetoName[0] = UNICODE_NULL;
    Cr = CM_Query_And_Remove_SubTreeW(Bus, &VetoType, VetoName, RTL_NUMBER_OF(VetoName),
                                      CM_REMOVE_NO_RESTART);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        trace("Veto type %d, name %S\n", VetoType, VetoName);

Uninstall:
    Cr = CM_Locate_DevNodeW(&Bus, InstanceId, CM_LOCATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr == CR_SUCCESS)
    {
        Cr = CM_Uninstall_DevNode(Bus, 0);
        ok_eq_ulong(Cr, CR_SUCCESS);
    }

Cleanup:
    DeleteChildKey();
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ENUM_KEY L"Root\\" IOFAILEDADD_BUS_ID);
    KmtDeleteService(IOFAILEDADD_SERVICE_NAME, &Service);
}
