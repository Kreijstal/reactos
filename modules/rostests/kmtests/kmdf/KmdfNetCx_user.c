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
 * then remove and uninstall the device again.
 */

#include <kmt_test.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <cfgmgr32.h>

#include "../kmtest/kmtest.h"
#include "KmdfNetCx.h"

#define FORMAT(_Buffer, ...) \
    (_snwprintf((_Buffer), RTL_NUMBER_OF(_Buffer) - 1, __VA_ARGS__), \
     (_Buffer)[RTL_NUMBER_OF(_Buffer) - 1] = UNICODE_NULL)

#define NET_CLASS_GUID      L"{4d36e972-e325-11ce-bfc1-08002be10318}"
#define NET_CLASS_KEY       L"SYSTEM\\CurrentControlSet\\Control\\Class\\" NET_CLASS_GUID
#define NETCX_VERSIONS_KEY  L"SYSTEM\\CurrentControlSet\\Control\\Wdf\\Kmdf\\NetAdapterCx\\Versions\\2\\1"
#define TCPIP_IF_KEY        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces"

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
 * Remove the device the way the device manager does, then uninstall it:
 * NDIS halts the adapter, the device node, its registry keys and, with
 * the last device of its driver gone, the driver go away.
 */
static
VOID
RemoveDevice(
    _Inout_ PNETCX_TEST_DEVICE Device)
{
    PNP_VETO_TYPE VetoType = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH], Path[256];
    SERVICE_STATUS ServiceStatus;
    ULONG Status, Problem, i;
    DEVINST DevInst;
    CONFIGRET Cr;
    HKEY Key;

    VetoName[0] = UNICODE_NULL;
    RtlZeroMemory(&ServiceStatus, sizeof(ServiceStatus));
    Cr = CM_Query_And_Remove_SubTreeW(Device->DevInst, &VetoType, VetoName, RTL_NUMBER_OF(VetoName),
                                      CM_REMOVE_NO_RESTART);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
    {
        trace("Removal of %S vetoed: type %d, name %S\n", Device->InstanceId, VetoType, VetoName);
        return;
    }
    ok(!NdisAdapterPresent(Device->ExportName), "NDIS adapter %S still there\n", Device->ExportName);

    /* The device node goes once the last reference to it is gone */
    for (i = 0; i < 50; i++)
    {
        if (CM_Get_DevNode_Status(&Status, &Problem, Device->DevInst, 0) != CR_SUCCESS)
            break;
        Sleep(100);
    }

    Cr = CM_Locate_DevNodeW(&DevInst, Device->InstanceId, CM_LOCATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr == CR_SUCCESS)
    {
        Cr = CM_Uninstall_DevNode(DevInst, 0);
        ok_eq_ulong(Cr, CR_SUCCESS);
    }

    /* Hardware and software key */
    FORMAT(Path, L"SYSTEM\\CurrentControlSet\\Enum\\%s", Device->InstanceId);
    ok(RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &Key) == ERROR_FILE_NOT_FOUND,
       "%S still there\n", Path);
    FORMAT(Path, L"%s\\%s", NET_CLASS_KEY, Device->ClassInstance);
    ok(RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &Key) == ERROR_FILE_NOT_FOUND,
       "%S still there\n", Path);
    FORMAT(Path, L"SYSTEM\\CurrentControlSet\\Enum\\Root\\%s", Device->DeviceId);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, Path);

    if (!Device->Service)
        return;

    for (i = 0; i < 50; i++)
    {
        if (QueryServiceStatus(Device->Service, &ServiceStatus) &&
            ServiceStatus.dwCurrentState == SERVICE_STOPPED)
            break;
        Sleep(100);
    }
    ok(ServiceStatus.dwCurrentState == SERVICE_STOPPED, "%S not unloaded, state %lu\n",
       Device->ServiceName, ServiceStatus.dwCurrentState);
    KmtDeleteService(Device->ServiceName, &Device->Service);
}

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

    /*
     * Remove the device with its adapter running: TCPIP only tears down the
     * IP interface of an adapter whose media is connected when it unbinds
     * from it, and a stale one makes it query the removed adapter later.
     */
    if (LoopIoctl(Device->Minor, IOCTL_NETCXLOOP_START_ADAPTER, NULL))
        RemoveDevice(Device);
    else if (Device->Service)
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

static
LONG
SetMultiSz(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_ PCWSTR Value)
{
    WCHAR Buffer[64];
    SIZE_T Length = wcslen(Value);

    RtlZeroMemory(Buffer, sizeof(Buffer));
    wcscpy(Buffer, Value);
    /* "" is a list with no strings: just its terminator */
    return RegSetValueExW(Key, Name, 0, REG_MULTI_SZ, (const BYTE *)Buffer,
                          (DWORD)((Length ? Length + 2 : 1) * sizeof(WCHAR)));
}

/*
 * A static IPv4 address for the adapter, the way the TCP/IP property page
 * or "netsh interface ip set address ... static" leave it for tcpip
 */
static
BOOLEAN
WriteStaticAddress(
    _In_ PCWSTR NetCfgInstanceId,
    _In_ PCWSTR Address,
    _In_ PCWSTR Mask)
{
    WCHAR Path[256];
    DWORD Zero = 0;
    HKEY Key;
    LONG Error;

    FORMAT(Path, L"%s\\%s", TCPIP_IF_KEY, NetCfgInstanceId);
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error != ERROR_SUCCESS)
        return FALSE;

    ok_eq_int(RegSetValueExW(Key, L"EnableDHCP", 0, REG_DWORD, (const BYTE *)&Zero, sizeof(Zero)), ERROR_SUCCESS);
    ok_eq_int(SetMultiSz(Key, L"IPAddress", Address), ERROR_SUCCESS);
    ok_eq_int(SetMultiSz(Key, L"SubnetMask", Mask), ERROR_SUCCESS);
    ok_eq_int(SetMultiSz(Key, L"DefaultGateway", L""), ERROR_SUCCESS);
    RegCloseKey(Key);
    return TRUE;
}

/* Waits for tcpip to put Address (network order) on an interface */
static
BOOLEAN
WaitForAddress(
    _In_ ULONG Address,
    _In_ ULONG TimeoutMs,
    _Out_ PMIB_IPADDRROW Row)
{
    PMIB_IPADDRTABLE Table;
    ULONG Size, Waited, i;
    DWORD Error;

    RtlZeroMemory(Row, sizeof(*Row));
    for (Waited = 0; ; Waited += 100)
    {
        Size = 0;
        Error = GetIpAddrTable(NULL, &Size, FALSE);
        if (Error == ERROR_INSUFFICIENT_BUFFER && Size != 0)
        {
            Table = HeapAlloc(GetProcessHeap(), 0, Size);
            if (Table != NULL)
            {
                Error = GetIpAddrTable(Table, &Size, FALSE);
                for (i = 0; Error == NO_ERROR && i < Table->dwNumEntries; i++)
                {
                    if (Table->table[i].dwAddr == Address)
                    {
                        *Row = Table->table[i];
                        HeapFree(GetProcessHeap(), 0, Table);
                        return TRUE;
                    }
                }
                HeapFree(GetProcessHeap(), 0, Table);
            }
        }
        if (Waited >= TimeoutMs)
            return FALSE;
        Sleep(100);
    }
}

/* What tcpip reports for the interface; it must be the raw IP one */
static
VOID
CheckRawIpInterface(
    _In_ DWORD Index,
    _In_ BOOLEAN Connected)
{
    MIB_IFROW IfRow;
    DWORD Error;

    RtlZeroMemory(&IfRow, sizeof(IfRow));
    IfRow.dwIndex = Index;
    Error = GetIfEntry(&IfRow);
    ok_eq_ulong(Error, (DWORD)NO_ERROR);
    if (Error != NO_ERROR)
        return;

    trace("Interface %lu: type %lu, mtu %lu, physaddr length %lu, oper status %lu, \"%.*s\"\n",
          IfRow.dwIndex, IfRow.dwType, IfRow.dwMtu, IfRow.dwPhysAddrLen, IfRow.dwOperStatus,
          (int)min(IfRow.dwDescrLen, sizeof(IfRow.bDescr)), IfRow.bDescr);
    ok_eq_ulong(IfRow.dwType, (DWORD)IF_TYPE_PROP_VIRTUAL);
    ok_eq_ulong(IfRow.dwMtu, 1500UL);
    ok_eq_ulong(IfRow.dwPhysAddrLen, 0UL);
    if (Connected)
        ok_eq_ulong(IfRow.dwOperStatus, (DWORD)MIB_IF_OPER_STATUS_OPERATIONAL);
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
#define OVPN_IOCTL_SET_MODE    CTL_CODE(FILE_DEVICE_UNKNOWN, 10, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define OVPN_MODE_MP           1
#define OVPN_LOCAL_IP          0x0201080AUL    /* 10.8.1.2, network order */

START_TEST(KmdfNetCxOvpnDco)
{
    NETCX_TEST_DEVICE Device;
    SC_HANDLE CxService;
    HANDLE Handle;
    LONG Version[3] = { 0 };
    LONG Mode = OVPN_MODE_MP;
    MIB_IPADDRROW Row;
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

    /* The address openvpn.exe would give the adapter (netsh/iphlpapi) */
    if (!WriteStaticAddress(Device.NetCfgInstanceId, L"10.8.1.2", L"255.255.255.0") ||
        !InstallDevice(&Device))
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

    /* The adapter's link is down until openvpn.exe starts the VPN */
    ok(!WaitForAddress(OVPN_LOCAL_IP, 0, &Row), "10.8.1.2 is up before the link\n");

    /* The adapter's own device: what openvpn.exe opens */
    Handle = CreateFileW(L"\\\\.\\ovpn-dco", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening ovpn-dco failed with %lu\n", GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
    {
        /* Multipeer (server) mode connects the link without a peer */
        Ok = DeviceIoControl(Handle, OVPN_IOCTL_SET_MODE, &Mode, sizeof(Mode), NULL, 0, &Returned, NULL);
        ok(Ok, "OVPN_IOCTL_SET_MODE failed with %lu\n", GetLastError());

        /* tcpip is bound to the raw IP adapter and configures it */
        if (Ok && WaitForAddress(OVPN_LOCAL_IP, 5000, &Row))
        {
            ok_eq_hex(Row.dwMask, 0x00FFFFFFUL);
            trace("10.8.1.2 is on interface %lu\n", Row.dwIndex);
            CheckRawIpInterface(Row.dwIndex, TRUE);
        }
        else if (Ok)
        {
            ok(FALSE, "tcpip did not configure 10.8.1.2 on the ovpn-dco adapter\n");
        }
        /* Closing it stops the VPN and takes the link down */
        CloseHandle(Handle);
    }

    /*
     * Remove the adapter with its link down: TCPIP must drop its IP
     * interface as well, not keep one for the removed adapter.
     */
    if (Row.dwIndex != 0)
    {
        MIB_IFROW IfRow;
        ULONG i;

        RtlZeroMemory(&IfRow, sizeof(IfRow));
        for (i = 0; i < 50; i++)
        {
            IfRow.dwIndex = Row.dwIndex;
            if (GetIfEntry(&IfRow) != NO_ERROR ||
                IfRow.dwOperStatus != MIB_IF_OPER_STATUS_OPERATIONAL)
            {
                break;
            }
            Sleep(100);
        }
        ok(IfRow.dwOperStatus != MIB_IF_OPER_STATUS_OPERATIONAL, "Link still up\n");

        RemoveDevice(&Device);

        IfRow.dwIndex = Row.dwIndex;
        ok(GetIfEntry(&IfRow) != NO_ERROR, "Interface %lu of the removed adapter is still there\n", Row.dwIndex);
    }

Exit:
    if (Device.Service)
        KmtCloseService(&Device.Service);
    if (CxService)
        KmtCloseService(&CxService);
}

/*
 * tcpip on a raw IP (NdisMediumIP) NetAdapterCx adapter, the kind
 * ovpn-dco-win is: the tunnel client keeps what tcpip sends it and
 * receives what we inject. The adapter gets a static address from the
 * registry, the way OpenVPN's adapters are configured.
 */
#define TUN_LOCAL_ADDRESS   L"10.8.0.2"
#define TUN_LOCAL_IP        0x0200080AUL    /* 10.8.0.2, network order */
#define TUN_REMOTE_IP       0x0100080AUL    /* 10.8.0.1 */
#define TUN_MASK            0x00FFFFFFUL    /* 255.255.255.0 */
#define TUN_LOCAL_PORT      47001
#define TUN_REMOTE_PORT     47002

static
BOOLEAN
TunIoctl(
    _In_ DWORD Code,
    _In_reads_bytes_opt_(InLength) PVOID In,
    _In_ DWORD InLength,
    _Out_writes_bytes_opt_(OutLength) PVOID Out,
    _In_ DWORD OutLength,
    _Out_opt_ PDWORD Returned)
{
    HANDLE Handle;
    DWORD Bytes = 0;
    BOOL Ok;

    Handle = CreateFileW(NETCXTUN_DOS_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening the tunnel device failed with %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;

    Ok = DeviceIoControl(Handle, Code, In, InLength, Out, OutLength, &Bytes, NULL);
    CloseHandle(Handle);
    if (Returned)
        *Returned = Bytes;
    return Ok != FALSE;
}

static
USHORT
IpChecksum(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    ULONG Sum = 0, i;

    for (i = 0; i + 1 < Length; i += 2)
        Sum += (Data[i] << 8) | Data[i + 1];
    if (Length & 1)
        Sum += Data[Length - 1] << 8;
    while (Sum >> 16)
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    return (USHORT)~Sum;
}

/* A raw IPv4/UDP packet, checksummed IP header, no UDP checksum */
static
ULONG
BuildUdpPacket(
    _Out_writes_bytes_(2048) PUCHAR Packet,
    _In_ ULONG Source,
    _In_ USHORT SourcePort,
    _In_ ULONG Destination,
    _In_ USHORT DestinationPort,
    _In_ PCSTR Payload)
{
    ULONG PayloadLength = (ULONG)strlen(Payload);
    ULONG Total = 20 + 8 + PayloadLength;
    USHORT Checksum;

    RtlZeroMemory(Packet, 28);
    Packet[0] = 0x45;
    Packet[2] = (UCHAR)(Total >> 8);
    Packet[3] = (UCHAR)Total;
    Packet[4] = 0x12;
    Packet[5] = 0x34;
    Packet[8] = 64;
    Packet[9] = IPPROTO_UDP;
    RtlCopyMemory(&Packet[12], &Source, 4);
    RtlCopyMemory(&Packet[16], &Destination, 4);
    Checksum = IpChecksum(Packet, 20);
    Packet[10] = (UCHAR)(Checksum >> 8);
    Packet[11] = (UCHAR)Checksum;

    Packet[20] = (UCHAR)(SourcePort >> 8);
    Packet[21] = (UCHAR)SourcePort;
    Packet[22] = (UCHAR)(DestinationPort >> 8);
    Packet[23] = (UCHAR)DestinationPort;
    Packet[24] = (UCHAR)((8 + PayloadLength) >> 8);
    Packet[25] = (UCHAR)(8 + PayloadLength);
    RtlCopyMemory(&Packet[28], Payload, PayloadLength);
    return Total;
}

/* Reads what tcpip transmitted; returns the UDP datagram we look for */
static
BOOLEAN
WaitForTxDatagram(
    _In_ ULONG TimeoutMs,
    _Out_writes_bytes_(2048) PUCHAR Frame,
    _Out_ PULONG FrameLength,
    _Inout_ PULONG Frames,
    _Inout_ PULONG NonIpFrames)
{
    ULONG Waited;
    DWORD Returned;

    for (Waited = 0; ; )
    {
        if (TunIoctl(IOCTL_NETCXLOOP_GET_TX_FRAME, NULL, 0, Frame, 2048, &Returned))
        {
            (*Frames)++;
            if (Returned == 0 || ((Frame[0] >> 4) != 4 && (Frame[0] >> 4) != 6))
            {
                (*NonIpFrames)++;
                trace("Not an IP packet: %lu bytes, %02x %02x %02x %02x\n", Returned,
                      Frame[0], Frame[1], Frame[2], Frame[3]);
                continue;
            }
            if (Returned >= 28 && Frame[0] == 0x45 && Frame[9] == IPPROTO_UDP &&
                *(ULONG UNALIGNED *)&Frame[16] == TUN_REMOTE_IP &&
                ((Frame[22] << 8) | Frame[23]) == TUN_REMOTE_PORT)
            {
                *FrameLength = Returned;
                return TRUE;
            }
            trace("Other IP packet: %lu bytes, protocol %u\n", Returned, Frame[9]);
            continue;
        }
        if (Waited >= TimeoutMs)
            return FALSE;
        Sleep(100);
        Waited += 100;
    }
}

static
VOID
TestRawIpTraffic(VOID)
{
    static const CHAR TxPayload[] = "rawip-tx-probe";
    static const CHAR RxPayload[] = "rawip-rx-probe";
    UCHAR Frame[2048];
    CHAR Buffer[64];
    struct sockaddr_in Local, Remote, From;
    ULONG FrameLength = 0, Frames = 0, NonIpFrames = 0;
    int FromLength, Received, Result;
    struct timeval Timeout;
    fd_set Readable;
    WSADATA WsaData;
    SOCKET Socket;
    DWORD Returned;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok_eq_int(Result, 0);
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Socket != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (Socket == INVALID_SOCKET)
        goto Cleanup;

    RtlZeroMemory(&Local, sizeof(Local));
    Local.sin_family = AF_INET;
    Local.sin_port = htons(TUN_LOCAL_PORT);
    Local.sin_addr.s_addr = TUN_LOCAL_IP;
    Result = bind(Socket, (struct sockaddr *)&Local, sizeof(Local));
    ok(Result == 0, "bind failed with %d\n", WSAGetLastError());

    /* Whatever tcpip sent before: it all has to be IP */
    while (TunIoctl(IOCTL_NETCXLOOP_GET_TX_FRAME, NULL, 0, Frame, sizeof(Frame), &Returned))
    {
        Frames++;
        if (Returned == 0 || ((Frame[0] >> 4) != 4 && (Frame[0] >> 4) != 6))
            NonIpFrames++;
    }

    /* Out: 10.8.0.2 -> 10.8.0.1 is on-link, no ARP, no link header */
    RtlZeroMemory(&Remote, sizeof(Remote));
    Remote.sin_family = AF_INET;
    Remote.sin_port = htons(TUN_REMOTE_PORT);
    Remote.sin_addr.s_addr = TUN_REMOTE_IP;
    Result = sendto(Socket, TxPayload, sizeof(TxPayload) - 1, 0, (struct sockaddr *)&Remote, sizeof(Remote));
    ok(Result == sizeof(TxPayload) - 1, "sendto returned %d, error %d\n", Result, WSAGetLastError());

    if (WaitForTxDatagram(5000, Frame, &FrameLength, &Frames, &NonIpFrames))
    {
        /* The IP header is the first byte of the frame */
        ok_eq_hex(Frame[0], 0x45);
        ok_eq_ulong(FrameLength, (ULONG)((Frame[2] << 8) | Frame[3]));
        ok_eq_ulong(FrameLength, 20UL + 8 + sizeof(TxPayload) - 1);
        ok_eq_hex(*(ULONG UNALIGNED *)&Frame[12], TUN_LOCAL_IP);
        ok_eq_hex(*(ULONG UNALIGNED *)&Frame[16], TUN_REMOTE_IP);
        ok_eq_int(IpChecksum(Frame, 20), 0);
        ok_eq_int((Frame[20] << 8) | Frame[21], TUN_LOCAL_PORT);
        ok(FrameLength >= 28 + sizeof(TxPayload) - 1 &&
           RtlEqualMemory(&Frame[28], TxPayload, sizeof(TxPayload) - 1), "Wrong payload\n");
    }
    else
    {
        ok(FALSE, "The datagram to 10.8.0.1 never reached the transmit ring\n");
    }
    ok_eq_ulong(NonIpFrames, 0UL);
    trace("%lu frames transmitted, %lu not IP\n", Frames, NonIpFrames);

    /* In: a raw IP datagram 10.8.0.1 -> 10.8.0.2 on the receive ring */
    FrameLength = BuildUdpPacket(Frame, TUN_REMOTE_IP, TUN_REMOTE_PORT, TUN_LOCAL_IP, TUN_LOCAL_PORT, RxPayload);
    ok(TunIoctl(IOCTL_NETCXLOOP_INJECT_RX_FRAME, Frame, FrameLength, NULL, 0, NULL),
       "Injecting failed with %lu\n", GetLastError());

    FD_ZERO(&Readable);
    FD_SET(Socket, &Readable);
    Timeout.tv_sec = 5;
    Timeout.tv_usec = 0;
    Result = select(0, &Readable, NULL, NULL, &Timeout);
    ok(Result == 1, "select returned %d, error %d\n", Result, WSAGetLastError());
    if (FD_ISSET(Socket, &Readable))
    {
        RtlZeroMemory(Buffer, sizeof(Buffer));
        FromLength = sizeof(From);
        Received = recvfrom(Socket, Buffer, sizeof(Buffer), 0, (struct sockaddr *)&From, &FromLength);
        ok_eq_int(Received, (int)sizeof(RxPayload) - 1);
        ok(Received == sizeof(RxPayload) - 1 && RtlEqualMemory(Buffer, RxPayload, Received),
           "Wrong payload '%s'\n", Buffer);
        ok_eq_hex(From.sin_addr.s_addr, TUN_REMOTE_IP);
        ok_eq_int(ntohs(From.sin_port), TUN_REMOTE_PORT);
    }
    else
    {
        ok(FALSE, "The injected datagram was not delivered\n");
    }

    closesocket(Socket);
Cleanup:
    WSACleanup();
}

START_TEST(KmdfNetCxRawIp)
{
    NETCX_TEST_DEVICE Device;
    SC_HANDLE CxService;
    MIB_IPADDRROW Row;
    HANDLE Handle;

    Handle = CreateFileW(NETCXTUN_DOS_NAME, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Handle);
        skip(FALSE, "Tunnel device left from an earlier run\n");
        return;
    }

    RegisterClassExtension(&CxService);

    RtlZeroMemory(&Device, sizeof(Device));
    Device.Minor = 1;
    wcscpy(Device.DeviceId, L"KmtNetCxTun");
    wcscpy(Device.Description, L"NetAdapterCx raw IP tunnel test adapter");
    wcscpy(Device.ServiceName, L"KmtNetCxTun");
    wcscpy(Device.ImageName, L"netcxtun21_drv.sys");
    wcscpy(Device.ClassInstance, L"0940");
    wcscpy(Device.NetCfgInstanceId, L"{6E4E4358-0000-4000-8000-000000000400}");

    /* Configured before the adapter exists, as an installed VPN adapter is */
    if (!WriteStaticAddress(Device.NetCfgInstanceId, TUN_LOCAL_ADDRESS, L"255.255.255.0") ||
        !InstallDevice(&Device))
    {
        skip(FALSE, "Tunnel device did not start\n");
        goto Exit;
    }

    ok(NdisAdapterPresent(Device.ExportName), "No NDIS adapter %S\n", Device.ExportName);

    /* tcpip bound the raw IP adapter and took the static address */
    if (!WaitForAddress(TUN_LOCAL_IP, 5000, &Row))
    {
        ok(FALSE, "tcpip did not configure 10.8.0.2 on the raw IP adapter\n");
        goto Exit;
    }
    ok_eq_hex(Row.dwMask, TUN_MASK);
    trace("10.8.0.2 is on interface %lu\n", Row.dwIndex);
    CheckRawIpInterface(Row.dwIndex, TRUE);

    TestRawIpTraffic();

    /*
     * The device stays installed: without a peer its link is down, and TCPIP
     * does not tear down the IP interface of an adapter whose media is
     * disconnected when it unbinds from it, then queries the removed adapter
     * through the stale interface (iphlpapi, explorer).
     */

Exit:
    if (Device.Service)
        KmtCloseService(&Device.Service);
    if (CxService)
        KmtCloseService(&CxService);
}
