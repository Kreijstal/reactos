/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF device removal test, user-mode part
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Root-enumerated devices driven by a plain KMDF function driver go away in
 * the ways a real device does: the driver reports the device removed or
 * failed, or user mode removes or disables it through the configuration
 * manager. Each time the PnP manager removes the device and, with the last
 * device gone, unloads the driver; the driver's callback counts tell how.
 */

#include <kmt_test.h>
#include <cfgmgr32.h>

#include "../kmtest/kmtest.h"
#include "KmdfRemove.h"

#define FORMAT(_Buffer, ...) \
    (_snwprintf((_Buffer), RTL_NUMBER_OF(_Buffer) - 1, __VA_ARGS__), \
     (_Buffer)[RTL_NUMBER_OF(_Buffer) - 1] = UNICODE_NULL)

#define SERVICE_KEY     L"SYSTEM\\CurrentControlSet\\Services\\" KMDFREMOVE_SERVICE_NAME
#define PARAMETERS_KEY  SERVICE_KEY L"\\Parameters"
#define ENUM_KEY        L"SYSTEM\\CurrentControlSet\\Enum\\"

typedef enum _REMOVE_METHOD
{
    RemoveReportRemoved,
    RemoveReportFailed,
    RemoveSetFailed,
    RemoveQueryAndRemove,
    RemoveDisable,
} REMOVE_METHOD;

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
        return (ULONG)-1;
    if (RegQueryValueExW(Key, Name, NULL, &Type, (BYTE *)&Value, &Size) || Type != REG_DWORD)
        Value = (DWORD)-1;
    RegCloseKey(Key);
    return Value;
}

static
BOOLEAN
DevicePresent(VOID)
{
    HANDLE Handle;

    Handle = CreateFileW(KMDFREMOVE_DOS_NAME, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    CloseHandle(Handle);
    return TRUE;
}

static
BOOLEAN
InstallDevice(
    _Out_ PDEVINST DevInst,
    _Out_writes_(MAX_DEVICE_ID_LEN) PWSTR InstanceId)
{
    DEVINST Root;
    CONFIGRET Cr;
    ULONG Status = 0, Problem = 0, i;

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        return FALSE;

    /* Registry first, as a phantom: nothing must try to install it */
    Cr = CM_Create_DevNodeW(DevInst, (DEVINSTID_W)KMDFREMOVE_DEVICE_ID, Root,
                            CM_CREATE_DEVNODE_GENERATE_ID | CM_CREATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        return FALSE;

    Cr = CM_Get_Device_IDW(*DevInst, InstanceId, MAX_DEVICE_ID_LEN, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);

    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_CLASS, L"System", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_CLASSGUID, L"{4d36e97d-e325-11ce-bfc1-08002be10318}", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_HARDWAREID, KMDFREMOVE_DEVICE_ID, TRUE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_DEVICEDESC, L"KMDF removal test device", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_SERVICE, KMDFREMOVE_SERVICE_NAME, FALSE), CR_SUCCESS);

    /* Now make it a device and start it */
    Cr = CM_Create_DevNodeW(DevInst, InstanceId, Root, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        return FALSE;

    Cr = CM_Setup_DevNode(*DevInst, CM_SETUP_DEVNODE_READY);
    ok_eq_ulong(Cr, CR_SUCCESS);

    for (i = 0; i < 100; i++)
    {
        Cr = CM_Get_DevNode_Status(&Status, &Problem, *DevInst, 0);
        if (Cr == CR_SUCCESS && (Status & DN_STARTED))
            break;
        Sleep(100);
    }

    ok(Cr == CR_SUCCESS && (Status & DN_STARTED),
       "Device not started: cr %lu, status 0x%lx, problem %lu\n", Cr, Status, Problem);
    return Cr == CR_SUCCESS && (Status & DN_STARTED);
}

static
VOID
SendIoctl(
    _In_ DWORD Code,
    _In_ BOOLEAN KeepOpen,
    _Out_ PHANDLE KeptHandle)
{
    HANDLE Handle;
    DWORD Returned = 0;
    BOOL Ok;

    *KeptHandle = INVALID_HANDLE_VALUE;
    Handle = CreateFileW(KMDFREMOVE_DOS_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening the device failed with %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return;

    Ok = DeviceIoControl(Handle, Code, NULL, 0, NULL, 0, &Returned, NULL);
    ok(Ok, "IOCTL 0x%lx failed with %lu\n", Code, GetLastError());
    if (KeepOpen)
        *KeptHandle = Handle;
    else
        CloseHandle(Handle);
}

static
VOID
TestRemove(
    _In_ REMOVE_METHOD Method,
    _In_ BOOLEAN KeepOpen)
{
    static const PCSTR MethodNames[] =
    {
        "report removed", "report failed", "set failed", "query and remove", "disable"
    };
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    HANDLE Handle = INVALID_HANDLE_VALUE;
    DEVINST DevInst;
    CONFIGRET Cr;
    PNP_VETO_TYPE VetoType = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH];
    ULONG Status = 0, Problem = 0, i;
    WCHAR KeyName[MAX_PATH];
    BOOLEAN Surprise;
    LONG Error;
    HKEY Key;

    trace("Removal by %s%s\n", MethodNames[Method], KeepOpen ? ", handle open" : "");

    if (DevicePresent())
    {
        skip(FALSE, "Device left from an earlier run\n");
        return;
    }
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);

    if (!InstallDevice(&DevInst, InstanceId))
    {
        skip(FALSE, "Device did not start\n");
        return;
    }
    trace("Device %S\n", InstanceId);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_DEVICE_ADDS), 1UL);

    switch (Method)
    {
        case RemoveReportRemoved:
            SendIoctl(IOCTL_KMDFREMOVE_REPORT_REMOVED, KeepOpen, &Handle);
            break;
        case RemoveReportFailed:
            SendIoctl(IOCTL_KMDFREMOVE_REPORT_FAILED, KeepOpen, &Handle);
            break;
        case RemoveSetFailed:
            SendIoctl(IOCTL_KMDFREMOVE_SET_FAILED, KeepOpen, &Handle);
            break;
        case RemoveQueryAndRemove:
            /* Vetoed while the device is open */
            Handle = CreateFileW(KMDFREMOVE_DOS_NAME, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
            ok(Handle != INVALID_HANDLE_VALUE, "Opening the device failed with %lu\n", GetLastError());
            Cr = CM_Query_And_Remove_SubTreeW(DevInst, &VetoType, VetoName, RTL_NUMBER_OF(VetoName),
                                              CM_REMOVE_NO_RESTART);
            ok_eq_ulong(Cr, CR_REMOVE_VETOED);
            ok_eq_int(VetoType, PNP_VetoOutstandingOpen);
            ok(DevicePresent(), "Device gone after a vetoed removal\n");
            if (Handle != INVALID_HANDLE_VALUE)
                CloseHandle(Handle);
            Handle = INVALID_HANDLE_VALUE;

            VetoType = PNP_VetoTypeUnknown;
            Cr = CM_Query_And_Remove_SubTreeW(DevInst, &VetoType, VetoName, RTL_NUMBER_OF(VetoName),
                                              CM_REMOVE_NO_RESTART);
            ok_eq_ulong(Cr, CR_SUCCESS);
            if (Cr != CR_SUCCESS)
                trace("Veto type %d, name %S\n", VetoType, VetoName);
            break;
        case RemoveDisable:
            Cr = CM_Disable_DevNode(DevInst, 0);
            ok_eq_ulong(Cr, CR_SUCCESS);
            break;
    }

    /* The device goes away; with an open handle once the handle is closed */
    for (i = 0; i < 100; i++)
    {
        if (!DevicePresent() && GetCounter(KMDFREMOVE_VALUE_DEVICE_CLEANUPS) == 1)
            break;
        if (Handle != INVALID_HANDLE_VALUE && i == 10)
        {
            /* IRP_MN_REMOVE_DEVICE waits for the handle */
            ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_DEVICE_CLEANUPS), 0UL);
            CloseHandle(Handle);
            Handle = INVALID_HANDLE_VALUE;
        }
        Sleep(100);
    }
    if (Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Handle);
    ok(!DevicePresent(), "Device still present\n");

    /* Without devices, the driver is unloaded */
    for (i = 0; i < 100; i++)
    {
        if (GetCounter(KMDFREMOVE_VALUE_UNLOADS) == 1)
            break;
        Sleep(100);
    }

    /* Surprise removal for a device that reports itself gone or failed */
    Surprise = (Method == RemoveReportRemoved ||
                Method == RemoveReportFailed ||
                Method == RemoveSetFailed);

    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_DEVICE_ADDS), 1UL);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_D0_ENTRIES), 1UL);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_D0_EXITS), 1UL);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_QUERY_REMOVES), Surprise ? 0UL : 1UL);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_SURPRISE_REMOVALS), Surprise ? 1UL : 0UL);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_RELEASE_HARDWARES), 1UL);
    if (Method < RemoveQueryAndRemove)
        ok(GetCounter(KMDFREMOVE_VALUE_FILE_CLEANUPS) >= 1, "No file cleanup\n");
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_DEVICE_CLEANUPS), 1UL);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_DEVICE_DESTROYS), 1UL);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_CHILD_CLEANUPS), 6UL);
    ok_eq_ulong(GetCounter(KMDFREMOVE_VALUE_UNLOADS), 1UL);

    Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
    trace("Device %S after removal: cr %lu, status 0x%lx, problem %lu\n", InstanceId, Cr, Status, Problem);
    ok(Cr != CR_SUCCESS || !(Status & DN_STARTED), "Device still started\n");

    /* Uninstall the device node for good */
    Cr = CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr == CR_SUCCESS)
    {
        Cr = CM_Uninstall_DevNode(DevInst, 0);
        ok_eq_ulong(Cr, CR_SUCCESS);
        FORMAT(KeyName, ENUM_KEY L"%s", InstanceId);
        Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_READ, &Key);
        ok(Error == ERROR_FILE_NOT_FOUND, "%S still there (error %ld)\n", KeyName, Error);
        if (Error == ERROR_SUCCESS)
            RegCloseKey(Key);
    }
}

START_TEST(KmdfRemove)
{
    SC_HANDLE Service = NULL;
    DWORD Error;

    Error = KmtCreateService(KMDFREMOVE_SERVICE_NAME, KMDFREMOVE_IMAGE_NAME, NULL, &Service);
    if (Error == ERROR_SERVICE_EXISTS)
        Error = ERROR_SUCCESS;
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    TestRemove(RemoveReportRemoved, FALSE);
    TestRemove(RemoveReportRemoved, TRUE);
    TestRemove(RemoveReportFailed, FALSE);
    TestRemove(RemoveSetFailed, FALSE);
    TestRemove(RemoveQueryAndRemove, FALSE);
    TestRemove(RemoveDisable, FALSE);

    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ENUM_KEY L"Root\\" KMDFREMOVE_DEVICE_ID);
    KmtDeleteService(KMDFREMOVE_SERVICE_NAME, &Service);
}
