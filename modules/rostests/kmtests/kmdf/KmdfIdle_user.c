/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF system-managed idle test, user-mode part
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A root-enumerated KMDF device with SystemManagedIdleTimeoutWithHint must go
 * through the power framework: KMDF registers it (the driver gets the
 * POHANDLE), declares the component idle once the device has no I/O (the
 * driver's idle condition callback runs and completes it), and powers the
 * device down when the power framework no longer requires device power. A
 * wake-up request brings it back to D0 and the component back to active, the
 * device idles again, and removal unregisters it.
 */

#include <kmt_test.h>
#include <cfgmgr32.h>

#include "../kmtest/kmtest.h"
#include "KmdfIdle.h"

#define SERVICE_KEY     L"SYSTEM\\CurrentControlSet\\Services\\" KMDFIDLE_SERVICE_NAME
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

    Cr = CM_Create_DevNodeW(DevInst, (DEVINSTID_W)KMDFIDLE_DEVICE_ID, Root,
                            CM_CREATE_DEVNODE_GENERATE_ID | CM_CREATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr != CR_SUCCESS)
        return FALSE;

    Cr = CM_Get_Device_IDW(*DevInst, InstanceId, MAX_DEVICE_ID_LEN, 0);
    ok_eq_ulong(Cr, CR_SUCCESS);

    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_CLASS, L"System", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_CLASSGUID, L"{4d36e97d-e325-11ce-bfc1-08002be10318}", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_HARDWAREID, KMDFIDLE_DEVICE_ID, TRUE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_DEVICEDESC, L"KMDF idle test device", FALSE), CR_SUCCESS);
    ok_eq_ulong(SetProperty(*DevInst, CM_DRP_SERVICE, KMDFIDLE_SERVICE_NAME, FALSE), CR_SUCCESS);

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
BOOLEAN
GetLog(
    _In_ HANDLE Handle,
    _Out_ PKMDFIDLE_LOG Log)
{
    DWORD Returned = 0;
    BOOL Ok;

    RtlZeroMemory(Log, sizeof(*Log));
    Ok = DeviceIoControl(Handle, IOCTL_KMDFIDLE_GET_LOG, NULL, 0, Log, sizeof(*Log), &Returned, NULL);
    ok(Ok && Returned == sizeof(*Log), "GET_LOG failed: %d, %lu bytes, error %lu\n", Ok, Returned, GetLastError());
    return Ok && Returned == sizeof(*Log);
}

static
VOID
TraceLog(
    _In_ PCSTR When,
    _In_ PKMDFIDLE_LOG Log)
{
    static const PCSTR Names[] =
    {
        "-", "PostRegister", "Active", "Idle", "D0Entry", "D0Exit", "PreUnregister", "WakeDone"
    };
    CHAR Line[512];
    ULONG i, Length = 0;

    Line[0] = ANSI_NULL;
    for (i = 0; i < min(Log->Count, KMDFIDLE_MAX_EVENTS) && Length < sizeof(Line) - 20; i++)
    {
        Length += sprintf(Line + Length, " %s",
                          Log->Events[i] < RTL_NUMBER_OF(Names) ? Names[Log->Events[i]] : "?");
    }
    trace("%s:%s\n", When, Line);
}

/* Index of the first Event at or after Start, or -1 */
static
LONG
FindEvent(
    _In_ PKMDFIDLE_LOG Log,
    _In_ ULONG Start,
    _In_ KMDFIDLE_EVENT Event)
{
    ULONG i;

    for (i = Start; i < min(Log->Count, KMDFIDLE_MAX_EVENTS); i++)
    {
        if (Log->Events[i] == Event)
            return (LONG)i;
    }
    return -1;
}

static
ULONG
CountEvents(
    _In_ PKMDFIDLE_LOG Log,
    _In_ KMDFIDLE_EVENT Event)
{
    ULONG i, Count = 0;

    for (i = 0; i < min(Log->Count, KMDFIDLE_MAX_EVENTS); i++)
    {
        if (Log->Events[i] == Event)
            Count++;
    }
    return Count;
}

/* Waits until the log holds Count D0Exit events */
static
BOOLEAN
WaitForEvents(
    _In_ HANDLE Handle,
    _In_ KMDFIDLE_EVENT Event,
    _In_ ULONG Count,
    _Out_ PKMDFIDLE_LOG Log)
{
    ULONG i;

    for (i = 0; i < 100; i++)
    {
        if (!GetLog(Handle, Log))
            return FALSE;
        if (CountEvents(Log, Event) >= Count)
            return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static
BOOLEAN
WaitForD0Exits(
    _In_ HANDLE Handle,
    _In_ ULONG Count,
    _Out_ PKMDFIDLE_LOG Log)
{
    return WaitForEvents(Handle, KmdfIdleEventD0Exit, Count, Log);
}

static
VOID
TestIdle(VOID)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    PNP_VETO_TYPE VetoType = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH];
    KMDFIDLE_LOG Log, SavedLog;
    LONG Register, Idle, Exit = -1, Active, Entry, Wake = -1, Unregister;
    DWORD Returned, Size, Type;
    HANDLE Handle;
    DEVINST DevInst;
    CONFIGRET Cr;
    BOOL Ok;
    HKEY Key;

    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);

    if (!InstallDevice(&DevInst, InstanceId))
    {
        skip(FALSE, "Device did not start\n");
        return;
    }
    trace("Device %S\n", InstanceId);

    Handle = CreateFileW(KMDFIDLE_DOS_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Opening the device failed with %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        goto Remove;

    if (!GetLog(Handle, &Log))
        goto Close;
    if (!Log.PoFxPresent)
    {
        skip(FALSE, "The kernel has no power framework\n");
        goto Close;
    }

    /* Registered with the power framework, then idle, then down to Dx */
    ok(WaitForD0Exits(Handle, 1, &Log), "The device did not idle\n");
    TraceLog("After start", &Log);
    ok(Log.HandleValid, "No POHANDLE from the power framework registration\n");
    ok_eq_ulong(Log.BadArguments, 0UL);
    Register = FindEvent(&Log, 0, KmdfIdleEventPostRegister);
    Idle = FindEvent(&Log, 0, KmdfIdleEventIdle);
    Exit = FindEvent(&Log, 0, KmdfIdleEventD0Exit);
    ok(Register >= 0, "Not registered with the power framework\n");
    ok(Idle > Register, "No idle condition after the registration (%ld, %ld)\n", Idle, Register);
    ok(Exit > Idle, "D0 exit not after the idle condition (%ld, %ld)\n", Exit, Idle);
    ok_eq_ulong(CountEvents(&Log, KmdfIdleEventActive), 0UL);
    ok_eq_ulong(CountEvents(&Log, KmdfIdleEventIdle), 1UL);

    /* A wake-up makes the component active and the device D0 again */
    Ok = DeviceIoControl(Handle, IOCTL_KMDFIDLE_WAKE, NULL, 0, NULL, 0, &Returned, NULL);
    ok(Ok, "WAKE failed with %lu\n", GetLastError());
    /* The wake-up returns once the device is in D0. The power framework
     * reports the component active from its worker after the device said it
     * is powered on, so on MP the callback may still be on its way. */
    ok(WaitForEvents(Handle, KmdfIdleEventActive, 1, &Log), "No active condition after the wake-up\n");
    if (!GetLog(Handle, &Log))
        goto Close;
    TraceLog("After wake", &Log);
    Active = FindEvent(&Log, Exit + 1, KmdfIdleEventActive);
    Entry = FindEvent(&Log, Exit + 1, KmdfIdleEventD0Entry);
    Wake = FindEvent(&Log, Exit + 1, KmdfIdleEventWakeDone);
    ok(Active > Exit, "No active condition after the wake-up (%ld)\n", Active);
    ok(Entry > Exit, "No D0 entry after the wake-up (%ld)\n", Entry);
    ok(Wake > Entry, "Wake-up done before D0 entry (%ld, %ld)\n", Wake, Entry);

    /* And idles again */
    ok(WaitForD0Exits(Handle, 2, &Log), "The device did not idle again\n");
    TraceLog("After the second idle", &Log);
    Idle = FindEvent(&Log, Wake + 1, KmdfIdleEventIdle);
    Exit = FindEvent(&Log, Wake + 1, KmdfIdleEventD0Exit);
    ok(Idle > Wake, "No idle condition after the wake-up (%ld)\n", Idle);
    ok(Exit > Idle, "D0 exit not after the second idle condition (%ld, %ld)\n", Exit, Idle);
    ok_eq_ulong(Log.BadArguments, 0UL);

Close:
    CloseHandle(Handle);

Remove:
    Cr = CM_Query_And_Remove_SubTreeW(DevInst, &VetoType, VetoName, RTL_NUMBER_OF(VetoName),
                                      CM_REMOVE_NO_RESTART);
    ok_eq_ulong(Cr, CR_SUCCESS);

    /* The driver saves the log when it unregisters and when it unloads */
    RtlZeroMemory(&SavedLog, sizeof(SavedLog));
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY, 0, KEY_QUERY_VALUE, &Key))
    {
        Size = sizeof(SavedLog);
        if (RegQueryValueExW(Key, L"Log", NULL, &Type, (BYTE *)&SavedLog, &Size) || Size != sizeof(SavedLog))
            RtlZeroMemory(&SavedLog, sizeof(SavedLog));
        RegCloseKey(Key);
    }
    if (SavedLog.PoFxPresent)
    {
        TraceLog("After removal", &SavedLog);
        Unregister = FindEvent(&SavedLog, 0, KmdfIdleEventPreUnregister);
        ok(Unregister > Exit, "Not unregistered after the last D0 exit (%ld, %ld)\n", Unregister, Exit);
        ok_eq_ulong(CountEvents(&SavedLog, KmdfIdleEventPreUnregister), 1UL);
        ok_eq_ulong(SavedLog.BadArguments, 0UL);
    }

    Cr = CM_Locate_DevNodeW(&DevInst, InstanceId, CM_LOCATE_DEVNODE_PHANTOM);
    ok_eq_ulong(Cr, CR_SUCCESS);
    if (Cr == CR_SUCCESS)
    {
        Cr = CM_Uninstall_DevNode(DevInst, 0);
        ok_eq_ulong(Cr, CR_SUCCESS);
    }
}

START_TEST(KmdfIdle)
{
    SC_HANDLE Service = NULL;
    DWORD Error;

    Error = KmtCreateService(KMDFIDLE_SERVICE_NAME, KMDFIDLE_IMAGE_NAME, NULL, &Service);
    if (Error == ERROR_SERVICE_EXISTS)
        Error = ERROR_SUCCESS;
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    TestIdle();

    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ENUM_KEY L"Root\\" KMDFIDLE_DEVICE_ID);
    KmtDeleteService(KMDFIDLE_SERVICE_NAME, &Service);
}
