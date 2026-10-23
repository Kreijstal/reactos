/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetDeviceInitConfig and device PnP/power hooks
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * The client calls NetDeviceInitConfig from EvtDriverDeviceAdd before
 * WdfDeviceCreate. The class extension hooks into the device's PnP/power
 * state machine through the KMDF class extension callbacks:
 *
 *   post self managed I/O init   device is started: bring up NDIS for
 *                                every adapter the client started
 *   pre release hardware         device is stopping or going away: halt
 *   pre surprise removal         same
 *
 * Unlike the Windows class extension these are plain functions instead of
 * a state machine engine: the transitions are few and KMDF serializes them.
 */

#include "nxprivate.h"
#include "nxapi.h"

#define NDEBUG
#include <debug.h>

static KGUARDED_MUTEX NxDeviceListLock;
static LIST_ENTRY NxDeviceList;

VOID
NxDeviceInitializeList(VOID)
{
    KeInitializeGuardedMutex(&NxDeviceListLock);
    InitializeListHead(&NxDeviceList);
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP NxEvtDeviceCleanup;

static
VOID
NTAPI
NxEvtDeviceCleanup(
    _In_ WDFOBJECT Object)
{
    PNX_DEVICE NxDevice = NxDeviceGetObjectContext(Object);

    /* The adapters, children of the device as well, are gone already */
    KeAcquireGuardedMutex(&NxDeviceListLock);
    RemoveEntryList(&NxDevice->Link);
    KeReleaseGuardedMutex(&NxDeviceListLock);
}

static
PNX_DEVICE
NxDeviceFindLocked(
    _In_ WDFDEVICE Device)
{
    PLIST_ENTRY Entry;
    PNX_DEVICE NxDevice;

    for (Entry = NxDeviceList.Flink; Entry != &NxDeviceList; Entry = Entry->Flink)
    {
        NxDevice = CONTAINING_RECORD(Entry, NX_DEVICE, Link);
        if (NxDevice->Device == Device)
            return NxDevice;
    }
    return NULL;
}

PNX_DEVICE
NxDeviceFind(
    _In_ WDFDEVICE Device)
{
    PNX_DEVICE NxDevice;

    KeAcquireGuardedMutex(&NxDeviceListLock);
    NxDevice = NxDeviceFindLocked(Device);
    KeReleaseGuardedMutex(&NxDeviceListLock);
    return NxDevice;
}

PNX_DEVICE
NxDeviceGet(
    _In_ WDFDEVICE Device)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    PNX_DEVICE NxDevice;
    WDFOBJECT Object;
    NTSTATUS Status;

    PAGED_CODE();

    KeAcquireGuardedMutex(&NxDeviceListLock);

    NxDevice = NxDeviceFindLocked(Device);
    if (NxDevice == NULL)
    {
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, NX_DEVICE);
        Attributes.ParentObject = Device;
        Attributes.EvtCleanupCallback = NxEvtDeviceCleanup;

        Status = WdfObjectCreate(&Attributes, &Object);
        if (NT_SUCCESS(Status))
        {
            NxDevice = NxDeviceGetObjectContext(Object);
            NxDevice->Device = Device;
            NxDevice->Object = Object;
            KeInitializeMutex(&NxDevice->Lock, 0);
            InitializeListHead(&NxDevice->Adapters);
            InsertTailList(&NxDeviceList, &NxDevice->Link);
        }
        else
        {
            DPRINT1("WdfObjectCreate failed 0x%lx\n", Status);
        }
    }

    KeReleaseGuardedMutex(&NxDeviceListLock);
    return NxDevice;
}

static
VOID
NxDeviceForEachAdapter(
    _In_ PNX_DEVICE NxDevice,
    _In_ VOID (*Callback)(PNX_ADAPTER))
{
    PLIST_ENTRY Entry;
    PNX_ADAPTER Adapter;

    /* Callbacks block, so walk the list under the device lock only */
    NxLock(&NxDevice->Lock);
    for (Entry = NxDevice->Adapters.Flink;
         Entry != &NxDevice->Adapters;
         Entry = Entry->Flink)
    {
        Adapter = CONTAINING_RECORD(Entry, NX_ADAPTER, DeviceLink);
        Callback(Adapter);
    }
    NxUnlock(&NxDevice->Lock);
}

static EVT_WDFCX_DEVICE_POST_SELF_MANAGED_IO_INIT NxEvtCxDevicePostSelfManagedIoInit;

static
NTSTATUS
NTAPI
NxEvtCxDevicePostSelfManagedIoInit(
    _In_ WDFDEVICE Device)
{
    PNX_DEVICE NxDevice;

    NxDevice = NxDeviceGet(Device);
    if (NxDevice == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    DPRINT("Device %p started\n", Device);

    NxDevice->Started = TRUE;
    NxDeviceForEachAdapter(NxDevice, NxAdapterStartNdis);
    return STATUS_SUCCESS;
}

static
VOID
NxDeviceStopping(
    _In_ WDFDEVICE Device)
{
    PNX_DEVICE NxDevice = NxDeviceFind(Device);

    if (NxDevice == NULL || !NxDevice->Started)
        return;

    NxDevice->Started = FALSE;
    NxDeviceForEachAdapter(NxDevice, NxAdapterStopNdis);
}

static EVT_WDFCX_DEVICE_PRE_RELEASE_HARDWARE NxEvtCxDevicePreReleaseHardware;

static
NTSTATUS
NTAPI
NxEvtCxDevicePreReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    NxDeviceStopping(Device);
    return STATUS_SUCCESS;
}

static EVT_WDFCX_DEVICE_PRE_SURPRISE_REMOVAL NxEvtCxDevicePreSurpriseRemoval;

static
VOID
NTAPI
NxEvtCxDevicePreSurpriseRemoval(
    _In_ WDFDEVICE Device)
{
    NxDeviceStopping(Device);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
imp_NetDeviceInitConfig(
    _In_ NET_DRIVER_GLOBALS * Globals,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDFCX_PNPPOWER_EVENT_CALLBACKS Callbacks;
    PWDFCXDEVICE_INIT CxDeviceInit;

    PAGED_CODE();

    NxGetPrivateGlobals(Globals);

    if (DeviceInit == NULL)
        return STATUS_INVALID_PARAMETER;

    CxDeviceInit = WdfCxDeviceInitAllocate(DeviceInit);
    if (CxDeviceInit == NULL)
    {
        DPRINT1("WdfCxDeviceInitAllocate failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WDFCX_PNPPOWER_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtCxDevicePostSelfManagedIoInit = NxEvtCxDevicePostSelfManagedIoInit;
    Callbacks.EvtCxDevicePreReleaseHardware = NxEvtCxDevicePreReleaseHardware;
    Callbacks.EvtCxDevicePreSurpriseRemoval = NxEvtCxDevicePreSurpriseRemoval;
    WdfCxDeviceInitSetPnpPowerEventCallbacks(CxDeviceInit, &Callbacks);

    /* What NDIS miniport FDOs are */
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_PHYSICAL_NETCARD);

    return STATUS_SUCCESS;
}
