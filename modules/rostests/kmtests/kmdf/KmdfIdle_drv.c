/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF system-managed idle test driver
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A KMDF function driver whose device idles the way the USB host controller
 * and root hub drivers do: S0-idle with SystemManagedIdleTimeoutWithHint, so
 * KMDF registers the device with the power framework, declares its single
 * component idle when the device has no I/O and powers it down only once the
 * power framework reports that device power is not required. The driver's
 * own power framework callbacks (WdfDeviceWdmAssignPowerFrameworkSettings)
 * record that path; the default queue is not power-managed, so reading the
 * log does not wake the device.
 */

#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>

#include "KmdfIdle.h"

typedef
VOID
NTAPI
FN_PO_FX_COMPLETE_IDLE_CONDITION(
    _In_ POHANDLE Handle,
    _In_ ULONG Component);

typedef struct _KMDFIDLE_DEVICE
{
    WDFDEVICE Device;
    POHANDLE PoHandle;
} KMDFIDLE_DEVICE, *PKMDFIDLE_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KMDFIDLE_DEVICE, KmdfIdleGetDevice)

static KMDFIDLE_LOG Log;
static KSPIN_LOCK LogLock;
static FN_PO_FX_COMPLETE_IDLE_CONDITION *pPoFxCompleteIdleCondition;
static WCHAR ParametersPathBuffer[256];

static
VOID
LogEvent(
    _In_ KMDFIDLE_EVENT Event)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&LogLock, &OldIrql);
    if (Log.Count < KMDFIDLE_MAX_EVENTS)
        Log.Events[Log.Count] = (UCHAR)Event;
    Log.Count++;
    KeReleaseSpinLock(&LogLock, OldIrql);
}

/* The log, written to Services\KmtKmdfIdle\Parameters for after the device is gone */
static
VOID
SaveLog(VOID)
{
    KMDFIDLE_LOG Copy;
    KIRQL OldIrql;

    if (ParametersPathBuffer[0] == UNICODE_NULL)
        return;

    KeAcquireSpinLock(&LogLock, &OldIrql);
    Copy = Log;
    KeReleaseSpinLock(&LogLock, OldIrql);

    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, ParametersPathBuffer, L"Log",
                          REG_BINARY, &Copy, sizeof(Copy));
}

static EVT_WDF_DEVICE_D0_ENTRY KmdfIdleEvtD0Entry;

static
NTSTATUS
NTAPI
KmdfIdleEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    LogEvent(KmdfIdleEventD0Entry);
    return STATUS_SUCCESS;
}

static EVT_WDF_DEVICE_D0_EXIT KmdfIdleEvtD0Exit;

static
NTSTATUS
NTAPI
KmdfIdleEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(TargetState);
    LogEvent(KmdfIdleEventD0Exit);
    return STATUS_SUCCESS;
}

static EVT_WDFDEVICE_WDM_POST_PO_FX_REGISTER_DEVICE KmdfIdleEvtPostPoFxRegister;

static
NTSTATUS
NTAPI
KmdfIdleEvtPostPoFxRegister(
    _In_ WDFDEVICE Device,
    _In_ POHANDLE PoHandle)
{
    KmdfIdleGetDevice(Device)->PoHandle = PoHandle;
    if (PoHandle != NULL)
        Log.HandleValid = TRUE;
    LogEvent(KmdfIdleEventPostRegister);
    return STATUS_SUCCESS;
}

static EVT_WDFDEVICE_WDM_PRE_PO_FX_UNREGISTER_DEVICE KmdfIdleEvtPrePoFxUnregister;

static
VOID
NTAPI
KmdfIdleEvtPrePoFxUnregister(
    _In_ WDFDEVICE Device,
    _In_ POHANDLE PoHandle)
{
    if (PoHandle != KmdfIdleGetDevice(Device)->PoHandle)
        InterlockedIncrement((PLONG)&Log.BadArguments);
    LogEvent(KmdfIdleEventPreUnregister);
    SaveLog();
}

static PO_FX_COMPONENT_ACTIVE_CONDITION_CALLBACK KmdfIdleComponentActive;

static
VOID
NTAPI
KmdfIdleComponentActive(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    PKMDFIDLE_DEVICE DeviceContext = Context;

    if (Component != 0 || DeviceContext->PoHandle == NULL)
        InterlockedIncrement((PLONG)&Log.BadArguments);
    LogEvent(KmdfIdleEventActive);
}

static PO_FX_COMPONENT_IDLE_CONDITION_CALLBACK KmdfIdleComponentIdle;

static
VOID
NTAPI
KmdfIdleComponentIdle(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    PKMDFIDLE_DEVICE DeviceContext = Context;

    if (Component != 0 || DeviceContext->PoHandle == NULL)
        InterlockedIncrement((PLONG)&Log.BadArguments);
    LogEvent(KmdfIdleEventIdle);

    /* A driver that handles the idle condition completes it */
    pPoFxCompleteIdleCondition(DeviceContext->PoHandle, Component);
}

static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL KmdfIdleEvtIoDeviceControl;

static
VOID
NTAPI
KmdfIdleEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PKMDFIDLE_LOG Buffer;
    NTSTATUS Status;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_KMDFIDLE_GET_LOG:
            Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Buffer), (PVOID *)&Buffer, NULL);
            if (!NT_SUCCESS(Status))
                break;
            KeAcquireSpinLock(&LogLock, &OldIrql);
            *Buffer = Log;
            KeReleaseSpinLock(&LogLock, OldIrql);
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*Buffer));
            return;

        case IOCTL_KMDFIDLE_WAKE:
            Status = WdfDeviceStopIdle(Device, TRUE);
            if (!NT_SUCCESS(Status))
                break;
            LogEvent(KmdfIdleEventWakeDone);
            WdfDeviceResumeIdle(Device);
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestComplete(Request, Status);
}

static EVT_WDF_DRIVER_DEVICE_ADD KmdfIdleEvtDeviceAdd;

static
NTSTATUS
NTAPI
KmdfIdleEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings;
    WDF_POWER_FRAMEWORK_SETTINGS PoFxSettings;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    UNICODE_STRING SymbolicLink;
    PKMDFIDLE_DEVICE Context;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = KmdfIdleEvtD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = KmdfIdleEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, KMDFIDLE_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Context = KmdfIdleGetDevice(Device);
    Context->Device = Device;

    RtlInitUnicodeString(&SymbolicLink, L"\\DosDevices\\KmtKmdfIdle");
    Status = WdfDeviceCreateSymbolicLink(Device, &SymbolicLink);
    if (!NT_SUCCESS(Status))
        goto Exit;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.PowerManaged = WdfFalse;
    QueueConfig.EvtIoDeviceControl = KmdfIdleEvtIoDeviceControl;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
        goto Exit;

    /* The power framework settings come before the S0-idle settings */
    if (pPoFxCompleteIdleCondition != NULL)
    {
        WDF_POWER_FRAMEWORK_SETTINGS_INIT(&PoFxSettings);
        PoFxSettings.EvtDeviceWdmPostPoFxRegisterDevice = KmdfIdleEvtPostPoFxRegister;
        PoFxSettings.EvtDeviceWdmPrePoFxUnregisterDevice = KmdfIdleEvtPrePoFxUnregister;
        PoFxSettings.ComponentActiveConditionCallback = KmdfIdleComponentActive;
        PoFxSettings.ComponentIdleConditionCallback = KmdfIdleComponentIdle;
        PoFxSettings.PoFxDeviceContext = Context;
        Status = WdfDeviceWdmAssignPowerFrameworkSettings(Device, &PoFxSettings);
        if (!NT_SUCCESS(Status))
            goto Exit;
    }

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings, IdleCannotWakeFromS0);
    IdleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
    IdleSettings.IdleTimeout = KMDFIDLE_TIMEOUT_MS;
    Status = WdfDeviceAssignS0IdleSettings(Device, &IdleSettings);

Exit:
    DbgPrint("KmdfIdle: device add 0x%lx\n", Status);
    return Status;
}

static EVT_WDF_DRIVER_UNLOAD KmdfIdleEvtDriverUnload;

static
VOID
NTAPI
KmdfIdleEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    SaveLog();
}

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    static UNICODE_STRING RoutineName = RTL_CONSTANT_STRING(L"PoFxCompleteIdleCondition");
    WDF_DRIVER_CONFIG Config;
    UNICODE_STRING Path;

    KeInitializeSpinLock(&LogLock);
    RtlZeroMemory(&Log, sizeof(Log));

    /* NT 5.2 kernels have no power framework; KMDF then manages the idle timeout itself */
    pPoFxCompleteIdleCondition = (FN_PO_FX_COMPLETE_IDLE_CONDITION *)MmGetSystemRoutineAddress(&RoutineName);
    Log.PoFxPresent = (pPoFxCompleteIdleCondition != NULL);

    RtlInitEmptyUnicodeString(&Path, ParametersPathBuffer, sizeof(ParametersPathBuffer) - sizeof(WCHAR));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&Path, L"%wZ\\Parameters", RegistryPath)))
    {
        ParametersPathBuffer[Path.Length / sizeof(WCHAR)] = UNICODE_NULL;
        RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, ParametersPathBuffer);
    }
    else
    {
        ParametersPathBuffer[0] = UNICODE_NULL;
    }

    WDF_DRIVER_CONFIG_INIT(&Config, KmdfIdleEvtDeviceAdd);
    Config.EvtDriverUnload = KmdfIdleEvtDriverUnload;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
