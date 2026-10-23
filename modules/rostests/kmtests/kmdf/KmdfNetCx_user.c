/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx test, user-mode part
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * For NetAdapterCx 2.1 and 2.0 in turn: register the class extension the
 * way its INF does, create a root-enumerated network device whose function
 * driver is the loopback client, let PnP start it, check the client's view
 * through its device, push packets through NDIS with the protocol driver,
 * then remove the device again.
 */

#include <kmt_test.h>
#include <cfgmgr32.h>

#include "../kmtest/kmtest.h"
#include "KmdfNetCx.h"

#define FORMAT(_Buffer, ...) \
    (_snwprintf((_Buffer), RTL_NUMBER_OF(_Buffer) - 1, __VA_ARGS__), \
     (_Buffer)[RTL_NUMBER_OF(_Buffer) - 1] = UNICODE_NULL)

#define NET_CLASS_GUID      L"{4d36e972-e325-11ce-bfc1-08002be10318}"
#define NET_CLASS_KEY       L"SYSTEM\\CurrentControlSet\\Control\\Class\\" NET_CLASS_GUID
#define NETCX_VERSIONS_KEY  L"SYSTEM\\CurrentControlSet\\Control\\Wdf\\Kmdf\\NetAdapterCx\\Versions\\2\\1"

typedef struct _NETCX_TEST_DEVICE
{
    ULONG Minor;
    WCHAR DeviceId[32];
    WCHAR Description[64];
    WCHAR ServiceName[32];
    WCHAR ImageName[32];
    WCHAR ClassInstance[8];
    WCHAR NetCfgInstanceId[40];
    WCHAR ExportName[64];
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    SC_HANDLE Service;
    DEVINST DevInst;
} NETCX_TEST_DEVICE, *PNETCX_TEST_DEVICE;

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
RegisterClassExtension(
    _Out_ SC_HANDLE *Service)
{
    HKEY Key;
    DWORD Error;

    *Service = NULL;

    Error = KmtCreateService(NETCX_SERVICE_NAME, NETCX_IMAGE_NAME, NULL, Service);
    if (Error == ERROR_SERVICE_EXISTS)
        Error = ERROR_SUCCESS;
    ok_eq_int(Error, ERROR_SUCCESS);

    /* What WDFLDR reads to find the class extension of a 2.x client */
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, NETCX_VERSIONS_KEY, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &Key, NULL);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        ok_eq_int(SetString(Key, L"Service", NETCX_SERVICE_NAME), ERROR_SUCCESS);
        RegCloseKey(Key);
    }
}

/* What the network class installer writes for an adapter */
static
BOOLEAN
WriteClassKey(
    _In_ PNETCX_TEST_DEVICE Device)
{
    WCHAR Path[256];
    WCHAR Medium[16];
    HKEY Key, Linkage;
    DWORD One = 1;
    LONG Error;

    FORMAT(Path, L"%s\\%s", NET_CLASS_KEY, Device->ClassInstance);
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error != ERROR_SUCCESS)
        return FALSE;

    FORMAT(Medium, L"%u", NETCX_TEST_MEDIUM);
    SetString(Key, L"DriverDesc", Device->Description);
    SetString(Key, L"NetCfgInstanceId", Device->NetCfgInstanceId);
    SetString(Key, L"*MediaType", Medium);
    SetString(Key, L"*PhysicalMediaType", L"0");
    SetString(Key, L"*IfType", L"53");
    RegSetValueExW(Key, L"Characteristics", 0, REG_DWORD, (const BYTE *)&One, sizeof(One));

    Error = RegCreateKeyExW(Key, L"Linkage", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Linkage, NULL);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        SetString(Linkage, L"Export", Device->ExportName);
        SetString(Linkage, L"RootDevice", Device->NetCfgInstanceId);
        RegCloseKey(Linkage);
    }

    RegCloseKey(Key);
    return Error == ERROR_SUCCESS;
}

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
BOOLEAN
InstallDevice(
    _Inout_ PNETCX_TEST_DEVICE Device)
{
    WCHAR Driver[64];
    DEVINST Root;
    CONFIGRET Cr;
    ULONG Status = 0, Problem = 0, i;
    DWORD Error;

    FORMAT(Device->ExportName, L"\\Device\\%s", Device->NetCfgInstanceId);

    Error = KmtCreateService(Device->ServiceName, Device->ImageName, NULL, &Device->Service);
    if (Error == ERROR_SERVICE_EXISTS)
        Error = ERROR_SUCCESS;
    ok_eq_int(Error, ERROR_SUCCESS);

    if (!WriteClassKey(Device))
        return FALSE;

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        return FALSE;

    /* Registry first, as a phantom: nothing must try to install it */
    Cr = CM_Create_DevNodeW(&Device->DevInst, Device->DeviceId, Root,
                            CM_CREATE_DEVNODE_GENERATE_ID | CM_CREATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        return FALSE;

    Cr = CM_Get_Device_IDW(Device->DevInst, Device->InstanceId, MAX_DEVICE_ID_LEN, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);
    trace("Device %S, adapter %S\n", Device->InstanceId, Device->ExportName);

    FORMAT(Driver, L"%s\\%s", NET_CLASS_GUID, Device->ClassInstance);
    ok_eq_ulong(SetProperty(Device->DevInst, CM_DRP_CLASS, L"Net", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Device->DevInst, CM_DRP_CLASSGUID, NET_CLASS_GUID, FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Device->DevInst, CM_DRP_HARDWAREID, Device->DeviceId, TRUE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Device->DevInst, CM_DRP_DEVICEDESC, Device->Description, FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Device->DevInst, CM_DRP_SERVICE, Device->ServiceName, FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(Device->DevInst, CM_DRP_DRIVER, Driver, FALSE), CR_SUCCESS);

    /* Now make it a device and start it */
    Cr = CM_Create_DevNodeW(&Device->DevInst, Device->InstanceId, Root, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        return FALSE;

    Cr = CM_Setup_DevNode(Device->DevInst, CM_SETUP_DEVNODE_READY);
    ok_eq_ulong(Cr, CR_SUCCESS);

    for (i = 0; i < 100; i++)
    {
        Cr = CM_Get_DevNode_Status(&Status, &Problem, Device->DevInst, 0);
        if (Cr == CR_SUCCESS && (Status & DN_STARTED))
            break;
        Sleep(100);
    }

    ok(Cr == CR_SUCCESS && (Status & DN_STARTED),
       "Device not started: cr %lu, status 0x%lx, problem %lu\n", Cr, Status, Problem);
    trace("Device %S: status 0x%lx, problem %lu\n", Device->InstanceId, Status, Problem);
    return Cr == CR_SUCCESS && (Status & DN_STARTED);
}

static
BOOLEAN
LoopIoctl(
    _In_ ULONG Minor,
    _In_ DWORD Code,
    _Out_opt_ PNETCXLOOP_STATE State)
{
    WCHAR Name[32];
    HANDLE Handle;
    DWORD Returned = 0;
    BOOL Ok;

    if (State)
        RtlZeroMemory(State, sizeof(*State));

    FORMAT(Name, NETCXLOOP_DOS_NAME_FORMAT, Minor);
    Handle = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening %S failed with %lu\n", Name, GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;

    Ok = DeviceIoControl(Handle, Code, NULL, 0, State, State ? sizeof(*State) : 0, &Returned, NULL);
    ok(Ok, "IOCTL 0x%lx failed with %lu\n", Code, GetLastError());
    if (State)
        ok_eq_ulong(Returned, (ULONG)sizeof(*State));
    CloseHandle(Handle);
    return Ok && (!State || Returned == sizeof(*State));
}

/* Is the NDIS adapter device \Device\{guid} there? */
static
BOOLEAN
NdisAdapterPresent(
    _In_ PCWSTR ExportName)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    UNICODE_STRING Name;
    HANDLE Handle;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, ExportName);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle, SYNCHRONIZE, &ObjectAttributes, &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(Status))
        NtClose(Handle);
    return NT_SUCCESS(Status);
}

static
VOID
RunTraffic(
    _In_ PCWSTR ExportName)
{
    DWORD Error;

    Error = KmtLoadAndOpenDriver(NETCXPROTO_SERVICE_NAME, FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    Error = KmtSendWStringToDriver(IOCTL_NETCXPROTO_SET_ADAPTER, ExportName);
    ok_eq_int(Error, ERROR_SUCCESS);
    Error = KmtSendToDriver(IOCTL_NETCXPROTO_RUN);
    ok_eq_int(Error, ERROR_SUCCESS);
    KmtCloseDriver();
    KmtUnloadDriver();
}

/*
 * The device stays installed: our cfgmgr cannot remove a device yet (the
 * kernel does not implement query-remove), and a KMDF function device that
 * reports itself removed (WdfDeviceSetDeviceState) brings the system down
 * in KMDF's own cleanup after its driver's callbacks ran, with or without
 * NetAdapterCx on it. Stopping the adapter is as far as the test goes.
 */
static
VOID
FinishDevice(
    _Inout_ PNETCX_TEST_DEVICE Device)
{
    NETCXLOOP_STATE State;

    if (LoopIoctl(Device->Minor, IOCTL_NETCXLOOP_STOP_ADAPTER, NULL) &&
        LoopIoctl(Device->Minor, IOCTL_NETCXLOOP_GET_STATE, &State))
    {
        ok_eq_long(State.TxCancels, 2L);
        ok_eq_long(State.RxCancels, 2L);
        ok_eq_long(State.RxQueueStops, 2L);
        ok_eq_long(State.QueueDestroys, 4L);
    }

    if (Device->Service)
        KmtCloseService(&Device->Service);
}

static
VOID
TestVersion(
    _In_ ULONG Minor)
{
    NETCX_TEST_DEVICE Device;
    NETCXLOOP_STATE State;
    WCHAR Name[32];
    HANDLE Handle;
    LONG64 Before;

    trace("NetAdapterCx 2.%lu client\n", Minor);

    RtlZeroMemory(&Device, sizeof(Device));
    Device.Minor = Minor;
    FORMAT(Device.DeviceId, L"KmtNetCxLoop2%lu", Minor);
    FORMAT(Device.Description, L"NetAdapterCx 2.%lu loopback test adapter", Minor);
    FORMAT(Device.ServiceName, L"KmtNetCxLoop2%lu", Minor);
    FORMAT(Device.ImageName, L"netcxloop2%lu_drv.sys", Minor);
    FORMAT(Device.ClassInstance, L"092%lu", Minor);
    FORMAT(Device.NetCfgInstanceId, L"{6E4E4358-0000-4000-8000-0000000002%02lX}", Minor);

    FORMAT(Name, NETCXLOOP_DOS_NAME_FORMAT, Minor);
    Handle = CreateFileW(Name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Handle);
        skip(FALSE, "Loopback device 2.%lu left from an earlier run\n", Minor);
        return;
    }

    if (!InstallDevice(&Device))
    {
        skip(FALSE, "Loopback device 2.%lu did not start\n", Minor);
        if (Device.Service)
            KmtDeleteService(Device.ServiceName, &Device.Service);
        return;
    }

    /* The client after its EvtDriverDeviceAdd and the start of the device */
    if (LoopIoctl(Minor, IOCTL_NETCXLOOP_GET_STATE, &State))
    {
        ok_eq_ulong(State.VersionMinor, Minor);
        ok_eq_ulong(State.TableEntries, Minor ? 122UL : 75UL);
        ok_eq_ulong(State.NullSlots, 0UL);
        ok_eq_ulong(State.GlobalsBound, 1UL);
        ok_eq_hex(State.DeviceAddStatus, STATUS_SUCCESS);
        ok_eq_hex(State.AdapterCreateStatus, STATUS_SUCCESS);
        ok_eq_hex(State.AdapterStartStatus, STATUS_SUCCESS);
        ok_eq_hex(State.StubStatus, STATUS_NOT_IMPLEMENTED);
        ok_eq_long(State.TxQueueCreates, 1L);
        ok_eq_long(State.RxQueueCreates, 1L);
        ok_eq_long(State.RxQueueStarts, 1L);
        ok_eq_long(State.RxQueueStops, 0L);
        ok_eq_long(State.VirtualAddressExtension, 2L);
        ok_eq_long(State.ChecksumExtension, Minor ? 1L : 0L);
        ok((State.NetLuid >> 48) == 53, "NetLuid 0x%I64x has no IF_TYPE_PROP_VIRTUAL\n", State.NetLuid);
    }
    ok(NdisAdapterPresent(Device.ExportName), "No NDIS adapter %S\n", Device.ExportName);

    /* Traffic through NDIS */
    RunTraffic(Device.ExportName);
    if (LoopIoctl(Minor, IOCTL_NETCXLOOP_GET_STATE, &State))
    {
        /*
         * 32 single frames, 3 in one list and one in two pieces. Other
         * protocols bound to the adapter may have sent a frame or two.
         */
        ok(State.TxPackets >= 36, "Only %I64d packets sent\n", State.TxPackets);
        ok_eq_longlong(State.RxPackets, State.TxPackets);
        ok_eq_longlong(State.TxBytes, State.RxBytes);
        ok_eq_longlong(State.TxMultiFragmentPackets, 1LL);
        ok_eq_longlong(State.Dropped, 0LL);
        trace("Tx %I64d/%I64d bytes, Rx %I64d/%I64d bytes, armed tx %ld rx %ld\n",
              State.TxPackets, State.TxBytes, State.RxPackets, State.RxBytes,
              State.TxArmed, State.RxArmed);
    }

    /* NetAdapterStop: NDIS halts the adapter, the queues go away */
    if (LoopIoctl(Minor, IOCTL_NETCXLOOP_STOP_ADAPTER, NULL) &&
        LoopIoctl(Minor, IOCTL_NETCXLOOP_GET_STATE, &State))
    {
        ok_eq_long(State.TxCancels, 1L);
        ok_eq_long(State.RxCancels, 1L);
        ok_eq_long(State.RxQueueStops, 1L);
        ok_eq_long(State.QueueDestroys, 2L);
    }

    /* NetAdapterStart again: new queues, traffic flows again */
    if (LoopIoctl(Minor, IOCTL_NETCXLOOP_START_ADAPTER, NULL) &&
        LoopIoctl(Minor, IOCTL_NETCXLOOP_GET_STATE, &State))
    {
        ok_eq_hex(State.AdapterRestartStatus, STATUS_SUCCESS);
        ok_eq_long(State.TxQueueCreates, 2L);
        ok_eq_long(State.RxQueueCreates, 2L);
        ok_eq_long(State.RxQueueStarts, 2L);
    }

    Before = State.TxPackets;
    RunTraffic(Device.ExportName);
    if (LoopIoctl(Minor, IOCTL_NETCXLOOP_GET_STATE, &State))
    {
        ok(State.TxPackets >= Before + 36, "Only %I64d packets sent\n", State.TxPackets - Before);
        ok_eq_longlong(State.RxPackets, State.TxPackets);
        ok_eq_longlong(State.Dropped, 0LL);
    }

    FinishDevice(&Device);
}

START_TEST(KmdfNetCx)
{
    SC_HANDLE CxService;

    RegisterClassExtension(&CxService);

    TestVersion(1);
    TestVersion(0);

    /* The class extension stays loaded, as on Windows */
    if (CxService)
        KmtCloseService(&CxService);
}

/*
 * Bring-up probe for OpenVPN's ovpn-dco-win, a NetAdapterCx 2.0/2.1 client.
 * Copy an ovpn-dco.sys (OpenVPN's signed build or a GCC build) next to
 * kmtest.exe; without one the test is skipped. The device is set up the
 * way ovpn-dco.inf sets it up (raw IP, IF_TYPE_PROP_VIRTUAL).
 */
#define OVPN_IOCTL_GET_VERSION CTL_CODE(FILE_DEVICE_UNKNOWN, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)

START_TEST(KmdfNetCxOvpnDco)
{
    NETCX_TEST_DEVICE Device;
    SC_HANDLE CxService;
    HANDLE Handle;
    LONG Version[3] = { 0 };
    DWORD Returned = 0;
    WCHAR Path[MAX_PATH];
    BOOL Ok;

    /* Where KmtCreateService looks: the directory of kmtest.exe */
    if (!GetModuleFileNameW(NULL, Path, RTL_NUMBER_OF(Path)) || !wcsrchr(Path, L'\\'))
        return;
    wcscpy(wcsrchr(Path, L'\\') + 1, L"ovpn-dco.sys");
    if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES)
    {
        skip(FALSE, "No ovpn-dco.sys next to kmtest.exe\n");
        return;
    }

    Handle = CreateFileW(L"\\\\.\\ovpn-dco-ver", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (Handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Handle);
        skip(FALSE, "ovpn-dco left from an earlier run\n");
        return;
    }

    RegisterClassExtension(&CxService);

    RtlZeroMemory(&Device, sizeof(Device));
    wcscpy(Device.DeviceId, L"ovpn-dco");
    wcscpy(Device.Description, L"OpenVPN Data Channel Offload");
    wcscpy(Device.ServiceName, L"ovpn-dco");
    wcscpy(Device.ImageName, L"ovpn-dco.sys");
    wcscpy(Device.ClassInstance, L"0930");
    wcscpy(Device.NetCfgInstanceId, L"{6E4E4358-0000-4000-8000-000000000300}");

    if (!InstallDevice(&Device))
        goto Exit;

    ok(NdisAdapterPresent(Device.ExportName), "No NDIS adapter %S\n", Device.ExportName);

    /* The control device the driver creates for its first device */
    Handle = CreateFileW(L"\\\\.\\ovpn-dco-ver", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening ovpn-dco-ver failed with %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
    {
        Ok = DeviceIoControl(Handle, OVPN_IOCTL_GET_VERSION, NULL, 0, Version, sizeof(Version),
                             &Returned, NULL);
        ok(Ok, "OVPN_IOCTL_GET_VERSION failed with %lu\n", GetLastError());
        trace("ovpn-dco %ld.%ld.%ld\n", Version[0], Version[1], Version[2]);
        CloseHandle(Handle);
    }

    /* The adapter's own device: what openvpn.exe opens */
    Handle = CreateFileW(L"\\\\.\\ovpn-dco", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening ovpn-dco failed with %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
        CloseHandle(Handle);

Exit:
    if (Device.Service)
        KmtCloseService(&Device.Service);
    if (CxService)
        KmtCloseService(&CxService);
}
