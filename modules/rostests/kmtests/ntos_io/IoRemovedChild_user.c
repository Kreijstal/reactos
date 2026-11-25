/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Removal of a device whose child's PDO outlives it, user-mode test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A device that is pulled out is surprise-removed and removed with its
 * children, the children first. A surprise-removed device gets
 * IRP_MN_REMOVE_DEVICE only once all handles to it are closed, and its
 * parent only once the children are removed: the drivers delete their device
 * objects on IRP_MN_REMOVE_DEVICE, while the handles still lead to them (bug
 * #93, a busy USB disk unplugged: classpnp called the deleted PDO of the disk).
 * New opens of the device fail from the surprise removal on.
 *
 * Once the bus driver of a removed child has
 * deleted the child's PDO, the child's device node is gone: CM_Get_DevNode_Status
 * fails with CR_NO_SUCH_DEVINST, even while something still references the
 * PDO (a handle, a mounted volume). The device node must not stay in the tree
 * under its parent until the last reference to its PDO is dropped: the parent's
 * node, freed with the parent's PDO, could then go before its child's (bug #100,
 * a busy USB disk unplugged: "IopFreeDeviceNode: USB\VID_... still has children").
 */

#include <kmt_test.h>
#include <cfgmgr32.h>

#include "../kmtest/kmtest.h"
#include "IoRemovedChild.h"

#define SERVICE_KEY     L"SYSTEM\\CurrentControlSet\\Services\\" IOREMOVEDCHILD_SERVICE_NAME
#define PARAMETERS_KEY  SERVICE_KEY L"\\Parameters"
#define ENUM_KEY        L"SYSTEM\\CurrentControlSet\\Enum\\"

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
SetFlag(
    _In_ PCWSTR Name)
{
    DWORD Value = 1;
    HKEY Key;
    LONG Error;

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &Key, NULL);
    if (Error)
        return Error;
    Error = RegSetValueExW(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
    RegCloseKey(Key);
    return Error;
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

/* Binds an instance to the driver before it is reported, with nothing to install */
static
LONG
PrepareInstance(
    _In_ PCWSTR InstancePath)
{
    WCHAR KeyName[MAX_PATH];
    HKEY Key;
    LONG Error;

    wcscpy(KeyName, ENUM_KEY);
    wcscat(KeyName, InstancePath);
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, NULL, 0, KEY_SET_VALUE, NULL, &Key, NULL);
    if (Error)
        return Error;
    Error = SetString(Key, L"Service", IOREMOVEDCHILD_SERVICE_NAME);
    if (!Error)
        Error = SetString(Key, L"Class", L"System");
    if (!Error)
        Error = SetString(Key, L"ClassGUID", L"{4d36e97d-e325-11ce-bfc1-08002be10318}");
    RegCloseKey(Key);
    return Error;
}

static
VOID
DeleteInstance(
    _In_ PCWSTR DeviceId)
{
    static const PCWSTR SubKeys[] = { L"\\0\\Device Parameters", L"\\0\\Control", L"\\0\\LogConf", L"\\0", L"" };
    WCHAR KeyName[MAX_PATH];
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(SubKeys); i++)
    {
        wcscpy(KeyName, ENUM_KEY);
        wcscat(KeyName, DeviceId);
        wcscat(KeyName, SubKeys[i]);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, KeyName);
    }
}

static
VOID
DeleteInstances(VOID)
{
    DeleteInstance(IOREMOVEDCHILD_LEAF_DEVICE_ID);
    DeleteInstance(IOREMOVEDCHILD_MID_DEVICE_ID);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ENUM_KEY IOREMOVEDCHILD_ENUMERATOR);
}

static
BOOLEAN
WaitStarted(
    _In_ PCWSTR InstancePath,
    _Out_ PDEVINST DevInst)
{
    ULONG Status, Problem, i;

    for (i = 0; i < 100; i++)
    {
        if (CM_Locate_DevNodeW(DevInst, (DEVINSTID_W)InstancePath, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
            CM_Get_DevNode_Status(&Status, &Problem, *DevInst, 0) == CR_SUCCESS &&
            (Status & DN_STARTED))
        {
            return TRUE;
        }
        Sleep(100);
    }
    return FALSE;
}

static
VOID
TestRemovedChild(
    _In_ DEVINST Bus)
{
    DEVINST Mid, Leaf;
    CONFIGRET Cr = CR_FAILURE;
    ULONG Status, Problem, i;
    HANDLE Handle, Handle2;

    ok(WaitStarted(IOREMOVEDCHILD_MID_PATH, &Mid), "The mid device did not start\n");
    if (!WaitStarted(IOREMOVEDCHILD_LEAF_PATH, &Leaf))
    {
        ok(FALSE, "The leaf device did not start\n");
        return;
    }

    /* An application has the leaf device open... */
    Handle = CreateFileW(IOREMOVEDCHILD_LEAF_DOS_NAME, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening the leaf device failed with %lu\n", GetLastError());

    /* ... when the mid device is pulled out, with its child */
    ok_eq_int(SetFlag(IOREMOVEDCHILD_VALUE_MID_GONE), ERROR_SUCCESS);
    ok_eq_ulong(CM_Reenumerate_DevNode(Bus, CM_REENUMERATE_SYNCHRONOUS), CR_SUCCESS);
    for (i = 0; i < 100 && GetCounter(IOREMOVEDCHILD_VALUE_LEAF_SURPRISES) == 0; i++)
        Sleep(100);
    ok_eq_ulong(GetCounter(IOREMOVEDCHILD_VALUE_LEAF_SURPRISES), 1UL);

    if (Handle != INVALID_HANDLE_VALUE)
    {
        /*
         * IRP_MN_REMOVE_DEVICE comes once the handles are closed: until then
         * the leaf's drivers keep their device objects, and the mid device,
         * the parent, waits for its child (bug #93).
         */
        Sleep(1000);
        ok_eq_ulong(GetCounter(IOREMOVEDCHILD_VALUE_LEAF_REMOVES), 0UL);
        ok_eq_ulong(GetCounter(IOREMOVEDCHILD_VALUE_MID_REMOVES), 0UL);

        /* A surprise-removed device cannot be opened any more */
        Handle2 = CreateFileW(IOREMOVEDCHILD_LEAF_DOS_NAME, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
        ok(Handle2 == INVALID_HANDLE_VALUE, "Opened the surprise-removed leaf device\n");
        if (Handle2 != INVALID_HANDLE_VALUE)
            CloseHandle(Handle2);

        CloseHandle(Handle);
    }

    for (i = 0; i < 100 && GetCounter(IOREMOVEDCHILD_VALUE_MID_REMOVES) == 0; i++)
        Sleep(100);
    ok_eq_ulong(GetCounter(IOREMOVEDCHILD_VALUE_LEAF_REMOVES), 1UL);
    ok_eq_ulong(GetCounter(IOREMOVEDCHILD_VALUE_MID_REMOVES), 1UL);

    /*
     * Both PDOs are deleted and still referenced. The removed child's node
     * is gone, the reference to its PDO notwithstanding.
     */
    for (i = 0; i < 30; i++)
    {
        Cr = CM_Get_DevNode_Status(&Status, &Problem, Leaf, 0);
        if (Cr != CR_SUCCESS)
            break;
        Sleep(100);
    }
    ok(Cr == CR_NO_SUCH_DEVINST, "The removed leaf device is still there: %lu (status 0x%lx, problem %lu)\n",
       Cr, Cr == CR_SUCCESS ? Status : 0, Cr == CR_SUCCESS ? Problem : 0);
    Cr = CM_Get_DevNode_Status(&Status, &Problem, Mid, 0);
    trace("The removed mid device: %lu (status 0x%lx, problem %lu)\n", Cr, Status, Problem);

    /* The PDOs go now, the parent's first */
    ok_eq_int(SetFlag(IOREMOVEDCHILD_VALUE_RELEASE), ERROR_SUCCESS);
    ok_eq_ulong(CM_Reenumerate_DevNode(Bus, CM_REENUMERATE_SYNCHRONOUS), CR_SUCCESS);
    for (i = 0; i < 100 && GetCounter(IOREMOVEDCHILD_VALUE_RELEASED) == 0; i++)
        Sleep(100);
    ok_eq_ulong(GetCounter(IOREMOVEDCHILD_VALUE_RELEASED), 1UL);
    ok_eq_ulong(CM_Get_DevNode_Status(&Status, &Problem, Leaf, 0), (ULONG)CR_NO_SUCH_DEVINST);
    ok_eq_ulong(CM_Get_DevNode_Status(&Status, &Problem, Mid, 0), (ULONG)CR_NO_SUCH_DEVINST);
}

START_TEST(IoRemovedChild)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    PNP_VETO_TYPE VetoType = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH];
    SC_HANDLE Service = NULL;
    DEVINST Root, Bus;
    ULONG Status = 0, Problem = 0, i;
    CONFIGRET Cr;
    DWORD Error;

    Error = KmtCreateService(IOREMOVEDCHILD_SERVICE_NAME, IOREMOVEDCHILD_IMAGE_NAME, NULL, &Service);
    if (Error == ERROR_SERVICE_EXISTS)
        Error = ERROR_SUCCESS;
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);
    DeleteInstances();
    ok_eq_int(PrepareInstance(IOREMOVEDCHILD_MID_PATH), ERROR_SUCCESS);
    ok_eq_int(PrepareInstance(IOREMOVEDCHILD_LEAF_PATH), ERROR_SUCCESS);

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        goto Cleanup;

    /* The bus: registry first, as a phantom, so that nothing tries to install it */
    Cr = CM_Create_DevNodeW(&Bus, (DEVINSTID_W)IOREMOVEDCHILD_BUS_ID, Root,
                            CM_CREATE_DEVNODE_GENERATE_ID | CM_CREATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        goto Cleanup;
    Cr = CM_Get_Device_IDW(Bus, InstanceId, MAX_DEVICE_ID_LEN, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);

    ok_eq_ulong(SetProperty(Bus, CM_DRP_CLASS, L"System", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Bus, CM_DRP_CLASSGUID, L"{4d36e97d-e325-11ce-bfc1-08002be10318}", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Bus, CM_DRP_HARDWAREID, IOREMOVEDCHILD_BUS_ID, TRUE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Bus, CM_DRP_DEVICEDESC, L"Removed child test bus", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Bus, CM_DRP_SERVICE, IOREMOVEDCHILD_SERVICE_NAME, FALSE), CR_SUCCESS);

    Cr = CM_Create_DevNodeW(&Bus, InstanceId, Root, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        goto Uninstall;
    Cr = CM_Setup_DevNode(Bus, CM_SETUP_DEVNODE_READY);
    ok_eq_ulong(Cr, CR_SUCCESS);
    for (i = 0; i < 100; i++)
    {
        if (CM_Get_DevNode_Status(&Status, &Problem, Bus, 0) == CR_SUCCESS && (Status & DN_STARTED))
            break;
        Sleep(100);
    }

    if (Status & DN_STARTED)
        TestRemovedChild(Bus);
    else
        skip(FALSE, "No bus: status 0x%lx, problem %lu\n", Status, Problem);

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
    DeleteInstances();
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ENUM_KEY L"Root\\" IOREMOVEDCHILD_BUS_ID);
    KmtDeleteService(IOREMOVEDCHILD_SERVICE_NAME, &Service);
}
