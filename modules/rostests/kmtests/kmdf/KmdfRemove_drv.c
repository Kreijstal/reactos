/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF device removal test driver
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A plain KMDF function driver: a device with a symbolic link, a device
 * interface, a power-managed default queue, file object callbacks and a few
 * child objects, which is what a typical KMDF client brings to the removal
 * of its device. Counts its callbacks and keeps the counts in its service
 * key, where user mode reads them after the driver is gone.
 */

#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>

#include "KmdfRemove.h"

/* {6E4E4358-0000-4000-8000-000000000400} */
static const GUID GUID_KMDFREMOVE_INTERFACE =
    { 0x6e4e4358, 0x0000, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00 } };

typedef struct _KMDFREMOVE_COUNTERS
{
    LONG DeviceAdds;
    LONG D0Entries;
    LONG D0Exits;
    LONG QueryRemoves;
    LONG SurpriseRemovals;
    LONG ReleaseHardwares;
    LONG FileCleanups;
    LONG DeviceCleanups;
    LONG DeviceDestroys;
    LONG ChildCleanups;
    LONG Unloads;
} KMDFREMOVE_COUNTERS;

typedef struct _KMDFREMOVE_DEVICE
{
    WDFDEVICE Device;
    WDFMEMORY Memory;
    WDFTIMER Timer;
    WDFWORKITEM WorkItem;
    WDFCOLLECTION Collection;
    WDFWAITLOCK WaitLock;
    WDFSPINLOCK SpinLock;
} KMDFREMOVE_DEVICE, *PKMDFREMOVE_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KMDFREMOVE_DEVICE, KmdfRemoveGetDevice)

static KMDFREMOVE_COUNTERS Counters;
static WCHAR ParametersPathBuffer[256];

/* The counters, written to Services\KmtKmdfRemove\Parameters */
static
VOID
SaveCounters(VOID)
{
    static const struct
    {
        PCWSTR Name;
        LONG *Value;
    } Values[] =
    {
        { KMDFREMOVE_VALUE_DEVICE_ADDS, &Counters.DeviceAdds },
        { KMDFREMOVE_VALUE_D0_ENTRIES, &Counters.D0Entries },
        { KMDFREMOVE_VALUE_D0_EXITS, &Counters.D0Exits },
        { KMDFREMOVE_VALUE_QUERY_REMOVES, &Counters.QueryRemoves },
        { KMDFREMOVE_VALUE_SURPRISE_REMOVALS, &Counters.SurpriseRemovals },
        { KMDFREMOVE_VALUE_RELEASE_HARDWARES, &Counters.ReleaseHardwares },
        { KMDFREMOVE_VALUE_FILE_CLEANUPS, &Counters.FileCleanups },
        { KMDFREMOVE_VALUE_DEVICE_CLEANUPS, &Counters.DeviceCleanups },
        { KMDFREMOVE_VALUE_DEVICE_DESTROYS, &Counters.DeviceDestroys },
        { KMDFREMOVE_VALUE_CHILD_CLEANUPS, &Counters.ChildCleanups },
        { KMDFREMOVE_VALUE_UNLOADS, &Counters.Unloads },
    };
    ULONG i, Value;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || ParametersPathBuffer[0] == UNICODE_NULL)
        return;

    for (i = 0; i < RTL_NUMBER_OF(Values); i++)
    {
        Value = (ULONG)*Values[i].Value;
        RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, ParametersPathBuffer, Values[i].Name,
                              REG_DWORD, &Value, sizeof(Value));
    }
}

static EVT_WDF_DEVICE_D0_ENTRY KmdfRemoveEvtD0Entry;

static
NTSTATUS
NTAPI
KmdfRemoveEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    InterlockedIncrement(&Counters.D0Entries);
    return STATUS_SUCCESS;
}

static EVT_WDF_DEVICE_D0_EXIT KmdfRemoveEvtD0Exit;

static
NTSTATUS
NTAPI
KmdfRemoveEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(TargetState);
    InterlockedIncrement(&Counters.D0Exits);
    return STATUS_SUCCESS;
}

static EVT_WDF_DEVICE_QUERY_REMOVE KmdfRemoveEvtQueryRemove;

static
NTSTATUS
NTAPI
KmdfRemoveEvtQueryRemove(
    _In_ WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    InterlockedIncrement(&Counters.QueryRemoves);
    return STATUS_SUCCESS;
}

static EVT_WDF_DEVICE_SURPRISE_REMOVAL KmdfRemoveEvtSurpriseRemoval;

static
VOID
NTAPI
KmdfRemoveEvtSurpriseRemoval(
    _In_ WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    InterlockedIncrement(&Counters.SurpriseRemovals);
}

static EVT_WDF_DEVICE_RELEASE_HARDWARE KmdfRemoveEvtReleaseHardware;

static
NTSTATUS
NTAPI
KmdfRemoveEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    InterlockedIncrement(&Counters.ReleaseHardwares);
    return STATUS_SUCCESS;
}

static EVT_WDF_FILE_CLEANUP KmdfRemoveEvtFileCleanup;

static
VOID
NTAPI
KmdfRemoveEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    InterlockedIncrement(&Counters.FileCleanups);
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP KmdfRemoveEvtChildCleanup;

static
VOID
NTAPI
KmdfRemoveEvtChildCleanup(
    _In_ WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object);
    InterlockedIncrement(&Counters.ChildCleanups);
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP KmdfRemoveEvtDeviceCleanup;

static
VOID
NTAPI
KmdfRemoveEvtDeviceCleanup(
    _In_ WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object);
    InterlockedIncrement(&Counters.DeviceCleanups);
    SaveCounters();
}

static EVT_WDF_OBJECT_CONTEXT_DESTROY KmdfRemoveEvtDeviceDestroy;

static
VOID
NTAPI
KmdfRemoveEvtDeviceDestroy(
    _In_ WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object);
    InterlockedIncrement(&Counters.DeviceDestroys);
    SaveCounters();
}

static EVT_WDF_TIMER KmdfRemoveEvtTimer;

static
VOID
NTAPI
KmdfRemoveEvtTimer(
    _In_ WDFTIMER Timer)
{
    UNREFERENCED_PARAMETER(Timer);
}

static EVT_WDF_WORKITEM KmdfRemoveEvtWorkItem;

static
VOID
NTAPI
KmdfRemoveEvtWorkItem(
    _In_ WDFWORKITEM WorkItem)
{
    UNREFERENCED_PARAMETER(WorkItem);
}

static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL KmdfRemoveEvtIoDeviceControl;

static
VOID
NTAPI
KmdfRemoveEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    WDF_DEVICE_STATE DeviceState;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_KMDFREMOVE_REPORT_REMOVED:
            WDF_DEVICE_STATE_INIT(&DeviceState);
            DeviceState.Removed = WdfTrue;
            WdfDeviceSetDeviceState(Device, &DeviceState);
            break;

        case IOCTL_KMDFREMOVE_REPORT_FAILED:
            WDF_DEVICE_STATE_INIT(&DeviceState);
            DeviceState.Failed = WdfTrue;
            WdfDeviceSetDeviceState(Device, &DeviceState);
            break;

        case IOCTL_KMDFREMOVE_SET_FAILED:
            WdfDeviceSetFailed(Device, WdfDeviceFailedNoRestart);
            break;

        default:
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
            return;
    }

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

static
NTSTATUS
CreateChildren(
    _In_ PKMDFREMOVE_DEVICE Context)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_TIMER_CONFIG TimerConfig;
    WDF_WORKITEM_CONFIG WorkItemConfig;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Context->Device;
    Attributes.EvtCleanupCallback = KmdfRemoveEvtChildCleanup;
    Status = WdfMemoryCreate(&Attributes, NonPagedPool, 'mRmK', 1000, &Context->Memory, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_TIMER_CONFIG_INIT(&TimerConfig, KmdfRemoveEvtTimer);
    Status = WdfTimerCreate(&TimerConfig, &Attributes, &Context->Timer);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_WORKITEM_CONFIG_INIT(&WorkItemConfig, KmdfRemoveEvtWorkItem);
    Status = WdfWorkItemCreate(&WorkItemConfig, &Attributes, &Context->WorkItem);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfCollectionCreate(&Attributes, &Context->Collection);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfWaitLockCreate(&Attributes, &Context->WaitLock);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfSpinLockCreate(&Attributes, &Context->SpinLock);
    if (!NT_SUCCESS(Status))
        return Status;

    return WdfCollectionAdd(Context->Collection, Context->Memory);
}

static EVT_WDF_DRIVER_DEVICE_ADD KmdfRemoveEvtDeviceAdd;

static
NTSTATUS
NTAPI
KmdfRemoveEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_FILEOBJECT_CONFIG FileConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    UNICODE_STRING SymbolicLink;
    PKMDFREMOVE_DEVICE Context;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    InterlockedIncrement(&Counters.DeviceAdds);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = KmdfRemoveEvtD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = KmdfRemoveEvtD0Exit;
    PnpPowerCallbacks.EvtDeviceQueryRemove = KmdfRemoveEvtQueryRemove;
    PnpPowerCallbacks.EvtDeviceSurpriseRemoval = KmdfRemoveEvtSurpriseRemoval;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = KmdfRemoveEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK,
                               KmdfRemoveEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &FileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, KMDFREMOVE_DEVICE);
    Attributes.EvtCleanupCallback = KmdfRemoveEvtDeviceCleanup;
    Attributes.EvtDestroyCallback = KmdfRemoveEvtDeviceDestroy;
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Context = KmdfRemoveGetDevice(Device);
    Context->Device = Device;

    RtlInitUnicodeString(&SymbolicLink, L"\\DosDevices\\KmtKmdfRemove");
    Status = WdfDeviceCreateSymbolicLink(Device, &SymbolicLink);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Status = WdfDeviceCreateDeviceInterface(Device, &GUID_KMDFREMOVE_INTERFACE, NULL);
    if (!NT_SUCCESS(Status))
        goto Exit;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = KmdfRemoveEvtIoDeviceControl;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Status = CreateChildren(Context);

Exit:
    DbgPrint("KmdfRemove: device add 0x%lx\n", Status);
    SaveCounters();
    return Status;
}

static EVT_WDF_DRIVER_UNLOAD KmdfRemoveEvtDriverUnload;

static
VOID
NTAPI
KmdfRemoveEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    InterlockedIncrement(&Counters.Unloads);
    SaveCounters();
}

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    UNICODE_STRING Path;

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

    WDF_DRIVER_CONFIG_INIT(&Config, KmdfRemoveEvtDeviceAdd);
    Config.EvtDriverUnload = KmdfRemoveEvtDriverUnload;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
