/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NETADAPTER objects
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Adapter lifecycle, as plain transitions under the adapter lock:
 *
 *   NetAdapterCreate        WDF object, child of the client's WDFDEVICE
 *   NetAdapterStart         NdisWdfPnPAddDevice; if the device runs, start
 *   device started          NdisWdfActionPnpStart: NDIS calls the miniport
 *                           InitializeEx and Restart handlers (miniport.c),
 *                           Restart creates the client's queues (queue.c)
 *   device stopping or      NdisWdfActionPnpStop: NDIS pauses and halts
 *   NetAdapterStop
 *   adapter deleted         NdisWdfActionPnpRemove: NDIS frees its adapter
 */

#include "nxprivate.h"
#include "nxapi.h"

#define NDEBUG
#include <debug.h>

static
PNX_ADAPTER
NxAdapterGet(
    _In_ NETADAPTER Handle)
{
    PNX_ADAPTER Adapter = NxAdapterGetContext(Handle);

    if (Adapter == NULL)
        KeBugCheckEx(WDF_VIOLATION, 0x4E4358, (ULONG_PTR)Handle, 1, 0);
    return Adapter;
}

static
PNX_ADAPTER_INIT
NxAdapterInitGet(
    _In_ NETADAPTER_INIT *AdapterInit)
{
    PNX_ADAPTER_INIT Init = (PNX_ADAPTER_INIT)AdapterInit;

    if (Init == NULL || Init->Signature != NX_ADAPTER_INIT_SIGNATURE)
        KeBugCheckEx(WDF_VIOLATION, 0x4E4358, (ULONG_PTR)AdapterInit, 2, 0);
    return Init;
}

/* Called with the device lock held */
VOID
NxAdapterStartNdis(
    _In_ PNX_ADAPTER Adapter)
{
    NDIS_STATUS Status;

    NxLock(&Adapter->Lock);

    if (Adapter->StartRequested && Adapter->NdisAdded && !Adapter->NdisStarted)
    {
        Status = NdisWdfPnpPowerEventHandler(Adapter->NdisHandle, NdisWdfActionPnpStart);
        if (Status == NDIS_STATUS_SUCCESS)
        {
            Adapter->NdisStarted = TRUE;
            DbgPrint("NetAdapterCx: adapter %p started\n", Adapter->Handle);
        }
        else
        {
            DbgPrint("NetAdapterCx: adapter %p failed to start 0x%lx\n",
                     Adapter->Handle, Status);
        }
    }

    NxUnlock(&Adapter->Lock);
}

VOID
NxAdapterStopNdis(
    _In_ PNX_ADAPTER Adapter)
{
    NxLock(&Adapter->Lock);

    if (Adapter->NdisStarted)
    {
        NdisWdfPnpPowerEventHandler(Adapter->NdisHandle, NdisWdfActionPnpStop);
        Adapter->NdisStarted = FALSE;
        DbgPrint("NetAdapterCx: adapter %p stopped\n", Adapter->Handle);
    }

    NxUnlock(&Adapter->Lock);
}

VOID
NxAdapterRemoveNdis(
    _In_ PNX_ADAPTER Adapter)
{
    NxLock(&Adapter->Lock);

    if (Adapter->NdisAdded)
    {
        /* Halts first when still started */
        NdisWdfPnpPowerEventHandler(Adapter->NdisHandle, NdisWdfActionPnpRemove);
        Adapter->NdisStarted = FALSE;
        Adapter->NdisAdded = FALSE;
        Adapter->NdisHandle = NULL;
    }

    NxUnlock(&Adapter->Lock);
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP NxEvtAdapterCleanup;

static
VOID
NTAPI
NxEvtAdapterCleanup(
    _In_ WDFOBJECT Object)
{
    PNX_ADAPTER Adapter = NxAdapterGetContext(Object);
    PNX_DEVICE NxDevice = Adapter->NxDevice;

    NxAdapterRemoveNdis(Adapter);

    NxLock(&NxDevice->Lock);
    RemoveEntryList(&Adapter->DeviceLink);
    NxUnlock(&NxDevice->Lock);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NETADAPTER_INIT *
NTAPI
imp_NetAdapterInitAllocate(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ WDFDEVICE Device)
{
    PNX_ADAPTER_INIT Init;

    PAGED_CODE();

    NxGetPrivateGlobals(DriverGlobals);

    Init = ExAllocatePoolZero(NonPagedPool, sizeof(*Init), NX_TAG);
    if (Init == NULL)
        return NULL;

    Init->Signature = NX_ADAPTER_INIT_SIGNATURE;
    Init->Device = Device;
    return (NETADAPTER_INIT *)Init;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
imp_NetAdapterInitFree(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER_INIT * AdapterInit)
{
    PNX_ADAPTER_INIT Init;

    PAGED_CODE();

    NxGetPrivateGlobals(DriverGlobals);

    /* Freeing NULL is allowed: the client may free unconditionally */
    if (AdapterInit == NULL)
        return;

    Init = NxAdapterInitGet(AdapterInit);
    Init->Signature = 0;
    ExFreePoolWithTag(Init, NX_TAG);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
imp_NetAdapterInitSetDatapathCallbacks(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _Inout_ NETADAPTER_INIT * AdapterInit,
    _In_ NET_ADAPTER_DATAPATH_CALLBACKS * DatapathCallbacks)
{
    PNX_ADAPTER_INIT Init;

    PAGED_CODE();

    NxGetPrivateGlobals(DriverGlobals);
    Init = NxAdapterInitGet(AdapterInit);
    NX_COPY_SIZED(&Init->DatapathCallbacks, DatapathCallbacks);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
imp_NetAdapterCreate(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER_INIT * AdapterInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * AdapterAttributes,
    _Out_ NETADAPTER * Adapter)
{
    PNX_PRIVATE_GLOBALS Globals;
    PNX_ADAPTER_INIT Init;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES ClientAttributes;
    PNX_DEVICE NxDevice;
    PNX_ADAPTER NxAdapter;
    WDFOBJECT Object;
    NTSTATUS Status;

    PAGED_CODE();

    *Adapter = NULL;

    Globals = NxGetPrivateGlobals(DriverGlobals);
    Init = NxAdapterInitGet(AdapterInit);

    if (Init->CreatedAdapter != NULL)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (Init->DatapathCallbacks.EvtAdapterCreateTxQueue == NULL ||
        Init->DatapathCallbacks.EvtAdapterCreateRxQueue == NULL)
    {
        DPRINT1("No datapath callbacks\n");
        return STATUS_INVALID_PARAMETER;
    }

    NxDevice = NxDeviceGet(Init->Device);
    if (NxDevice == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* A grandchild of the device: cleaned up before the device state */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, NX_ADAPTER);
    Attributes.ParentObject = NxDevice->Object;
    Attributes.EvtCleanupCallback = NxEvtAdapterCleanup;

    Status = WdfObjectCreate(&Attributes, &Object);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfObjectCreate failed 0x%lx\n", Status);
        return Status;
    }

    NxAdapter = NxAdapterGetContext(Object);
    NxAdapter->Handle = (NETADAPTER)Object;
    NxAdapter->Device = Init->Device;
    NxAdapter->NxDevice = NxDevice;
    NxAdapter->Globals = Globals;
    KeInitializeMutex(&NxAdapter->Lock, 0);
    KeInitializeEvent(&NxAdapter->RxIdle, NotificationEvent, TRUE);
    /* Run down until the datapath starts */
    ExInitializeRundownProtection(&NxAdapter->DatapathRundown);
    ExWaitForRundownProtectionRelease(&NxAdapter->DatapathRundown);
    NxAdapter->DatapathCallbacks = Init->DatapathCallbacks;

    /* Defaults until the client says otherwise */
    NxAdapter->MtuSize = 1500;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&NxAdapter->LinkLayerCapabilities,
                                             NET_ADAPTER_LINK_SPEED_UNKNOWN,
                                             NET_ADAPTER_LINK_SPEED_UNKNOWN);
    NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&NxAdapter->LinkState);
    NET_ADAPTER_TX_CAPABILITIES_INIT(&NxAdapter->TxCapabilities, 1);
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&NxAdapter->RxCapabilities, 1514, 1);
    NxAdapter->TxChecksumOffload.Signature = NX_OFFLOAD_SIGNATURE;
    NxAdapter->RxChecksumOffload.Signature = NX_OFFLOAD_SIGNATURE;
    NxAdapter->Medium = NdisMedium802_3;
    NxAdapter->PhysicalMedium = NdisPhysicalMediumUnspecified;
    NxAdapter->IfType = IF_TYPE_ETHERNET_CSMACD;
    NxAdapter->AccessType = NET_IF_ACCESS_BROADCAST;
    NxAdapter->DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    NxAdapter->ConnectionType = NET_IF_CONNECTION_DEDICATED;

    /* The client's context and callbacks live on the same object */
    if (AdapterAttributes != NULL && AdapterAttributes->ContextTypeInfo != NULL)
    {
        ClientAttributes = *AdapterAttributes;
        ClientAttributes.ParentObject = NULL;
        ClientAttributes.ExecutionLevel = WdfExecutionLevelInheritFromParent;
        ClientAttributes.SynchronizationScope = WdfSynchronizationScopeInheritFromParent;

        Status = WdfObjectAllocateContext(Object, &ClientAttributes, NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("WdfObjectAllocateContext for the client failed 0x%lx\n", Status);
            /* Not on the device list yet: keep the cleanup away from it */
            InitializeListHead(&NxAdapter->DeviceLink);
            WdfObjectDelete(Object);
            return Status;
        }
    }

    NxLock(&NxDevice->Lock);
    InsertTailList(&NxDevice->Adapters, &NxAdapter->DeviceLink);
    NxUnlock(&NxDevice->Lock);

    Init->CreatedAdapter = (NETADAPTER)Object;
    *Adapter = (NETADAPTER)Object;

    DbgPrint("NetAdapterCx: created adapter %p on device %p\n", Object, Init->Device);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
imp_NetAdapterStart(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter)
{
    PNX_PRIVATE_GLOBALS Globals;
    PNX_ADAPTER NxAdapter;
    PNX_DEVICE NxDevice;
    NDIS_WDF_ADD_DEVICE_INFO AddDeviceInfo;
    PDEVICE_OBJECT Fdo;
    NDIS_HANDLE MiniportDriverHandle;
    NDIS_STATUS NdisStatus;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    Globals = NxGetPrivateGlobals(DriverGlobals);
    NxAdapter = NxAdapterGet(Adapter);
    NxDevice = NxAdapter->NxDevice;

    NxLock(&NxDevice->Lock);
    NxLock(&NxAdapter->Lock);

    if (NxAdapter->StartRequested)
    {
        Status = STATUS_INVALID_DEVICE_STATE;
        goto Exit;
    }

    if (!NxAdapter->NdisAdded)
    {
        Fdo = WdfDeviceWdmGetDeviceObject(NxAdapter->Device);

        Status = NxGetMiniportDriverHandle(Globals, Fdo->DriverObject, &MiniportDriverHandle);
        if (!NT_SUCCESS(Status))
            goto Exit;

        RtlZeroMemory(&AddDeviceInfo, sizeof(AddDeviceInfo));
        AddDeviceInfo.Size = sizeof(AddDeviceInfo);
        AddDeviceInfo.MiniportDriverHandle = MiniportDriverHandle;
        AddDeviceInfo.PhysicalDeviceObject = WdfDeviceWdmGetPhysicalDevice(NxAdapter->Device);
        AddDeviceInfo.FunctionalDeviceObject = Fdo;
        AddDeviceInfo.MiniportAddDeviceContext = NxAdapter;

        NdisStatus = NdisWdfPnPAddDevice(&AddDeviceInfo, &NxAdapter->NdisHandle);
        if (NdisStatus != NDIS_STATUS_SUCCESS)
        {
            DbgPrint("NetAdapterCx: NdisWdfPnPAddDevice failed 0x%lx\n", NdisStatus);
            Status = STATUS_UNSUCCESSFUL;
            goto Exit;
        }

        NxAdapter->NdisAdded = TRUE;
    }

    NxAdapter->StartRequested = TRUE;

    /* Started before the device: NDIS comes up with the device */
    if (NxDevice->Started)
        NxAdapterStartNdis(NxAdapter);

Exit:
    NxUnlock(&NxAdapter->Lock);
    NxUnlock(&NxDevice->Lock);
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
imp_NetAdapterStop(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter)
{
    PNX_ADAPTER NxAdapter;
    PNX_DEVICE NxDevice;

    PAGED_CODE();

    NxGetPrivateGlobals(DriverGlobals);
    NxAdapter = NxAdapterGet(Adapter);
    NxDevice = NxAdapter->NxDevice;

    NxLock(&NxDevice->Lock);
    NxLock(&NxAdapter->Lock);
    NxAdapter->StartRequested = FALSE;
    NxAdapterStopNdis(NxAdapter);
    NxUnlock(&NxAdapter->Lock);
    NxUnlock(&NxDevice->Lock);
}

VOID
NTAPI
imp_NetAdapterSetLinkLayerCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_CAPABILITIES * LinkLayerCapabilities)
{
    NxGetPrivateGlobals(DriverGlobals);
    NX_COPY_SIZED(&NxAdapterGet(Adapter)->LinkLayerCapabilities, LinkLayerCapabilities);
}

VOID
NTAPI
imp_NetAdapterSetLinkLayerMtuSize(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ ULONG MtuSize)
{
    NxGetPrivateGlobals(DriverGlobals);

    if (MtuSize == 0)
        return;

    /* A change after the adapter started is not indicated to NDIS yet */
    NxAdapterGet(Adapter)->MtuSize = MtuSize;
}

VOID
NTAPI
imp_NetAdapterSetDataPathCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_TX_CAPABILITIES * TxCapabilities,
    _In_ NET_ADAPTER_RX_CAPABILITIES * RxCapabilities)
{
    PNX_ADAPTER NxAdapter;

    NxGetPrivateGlobals(DriverGlobals);
    NxAdapter = NxAdapterGet(Adapter);

    if (TxCapabilities != NULL)
        NX_COPY_SIZED(&NxAdapter->TxCapabilities, TxCapabilities);
    if (RxCapabilities != NULL)
        NX_COPY_SIZED(&NxAdapter->RxCapabilities, RxCapabilities);
    NxAdapter->DatapathCapabilitiesSet = TRUE;
}

VOID
NTAPI
imp_NetAdapterSetLinkState(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_STATE * State)
{
    PNX_ADAPTER NxAdapter;

    NxGetPrivateGlobals(DriverGlobals);
    NxAdapter = NxAdapterGet(Adapter);

    NX_COPY_SIZED(&NxAdapter->LinkState, State);

    /* Before initialization the state goes out in the general attributes */
    if (NxAdapter->Initialized)
        NxMiniportIndicateLinkState(NxAdapter);
}

NET_LUID
NTAPI
imp_NetAdapterGetNetLuid(
    _In_ NET_DRIVER_GLOBALS * Globals,
    _In_ NETADAPTER Adapter)
{
    PNX_ADAPTER NxAdapter;
    NET_LUID Luid;

    NxGetPrivateGlobals(Globals);
    NxAdapter = NxAdapterGet(Adapter);

    Luid.Value = 0;
    Luid.Info.IfType = NxAdapter->IfType;
    Luid.Info.NetLuidIndex = NxAdapter->NetLuidIndex;
    return Luid;
}

static
VOID
NxCopyLinkLayerAddress(
    _Out_ NET_ADAPTER_LINK_LAYER_ADDRESS *Destination,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS const *Source)
{
    RtlZeroMemory(Destination, sizeof(*Destination));
    Destination->Length = min(Source->Length, (USHORT)sizeof(Destination->Address));
    RtlCopyMemory(Destination->Address, Source->Address, Destination->Length);
}

VOID
NTAPI
imp_NetAdapterSetPermanentLinkLayerAddress(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress)
{
    NxGetPrivateGlobals(DriverGlobals);
    NxCopyLinkLayerAddress(&NxAdapterGet(Adapter)->PermanentAddress, LinkLayerAddress);
}

VOID
NTAPI
imp_NetAdapterSetCurrentLinkLayerAddress(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress)
{
    NxGetPrivateGlobals(DriverGlobals);
    NxCopyLinkLayerAddress(&NxAdapterGet(Adapter)->CurrentAddress, LinkLayerAddress);
}

static
PNX_OFFLOAD
NxOffloadGet(
    _In_ NETOFFLOAD Offload)
{
    PNX_OFFLOAD NxOffload = (PNX_OFFLOAD)Offload;

    if (NxOffload == NULL || NxOffload->Signature != NX_OFFLOAD_SIGNATURE)
        KeBugCheckEx(WDF_VIOLATION, 0x4E4358, (ULONG_PTR)Offload, 3, 0);
    return NxOffload;
}

/* NetAdapterCx 2.0 checksum offload: one set of flags for both directions */
VOID
NTAPI
imp_NetAdapterOffloadSetChecksumCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES * HardwareCapabilities)
{
    PNX_ADAPTER NxAdapter;

    NxGetPrivateGlobals(DriverGlobals);
    NxAdapter = NxAdapterGet(Adapter);

    NX_COPY_SIZED(&NxAdapter->ChecksumCapabilities, HardwareCapabilities);
    NxAdapter->ChecksumCapabilitiesSet = TRUE;

    NxAdapter->TxChecksumOffload.IPv4 = NxAdapter->RxChecksumOffload.IPv4 = HardwareCapabilities->IPv4;
    NxAdapter->TxChecksumOffload.Tcp = NxAdapter->RxChecksumOffload.Tcp = HardwareCapabilities->Tcp;
    NxAdapter->TxChecksumOffload.Udp = NxAdapter->RxChecksumOffload.Udp = HardwareCapabilities->Udp;
}

BOOLEAN
NTAPI
imp_NetOffloadIsChecksumIPv4Enabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->IPv4;
}

BOOLEAN
NTAPI
imp_NetOffloadIsChecksumTcpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->Tcp;
}

BOOLEAN
NTAPI
imp_NetOffloadIsChecksumUdpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->Udp;
}

VOID
NTAPI
imp_NetAdapterOffloadSetTxChecksumCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES * HardwareCapabilities)
{
    PNX_ADAPTER NxAdapter;

    NxGetPrivateGlobals(DriverGlobals);
    NxAdapter = NxAdapterGet(Adapter);

    NX_COPY_SIZED(&NxAdapter->TxChecksumCapabilities, HardwareCapabilities);
    NxAdapter->TxChecksumCapabilitiesSet = TRUE;

    NxAdapter->TxChecksumOffload.IPv4 =
        (HardwareCapabilities->Layer3Flags & (NetAdapterOffloadLayer3FlagIPv4NoOptions |
                                              NetAdapterOffloadLayer3FlagIPv4WithOptions)) != 0;
    NxAdapter->TxChecksumOffload.Tcp =
        (HardwareCapabilities->Layer4Flags & (NetAdapterOffloadLayer4FlagTcpNoOptions |
                                              NetAdapterOffloadLayer4FlagTcpWithOptions)) != 0;
    NxAdapter->TxChecksumOffload.Udp =
        (HardwareCapabilities->Layer4Flags & NetAdapterOffloadLayer4FlagUdp) != 0;
}

BOOLEAN
NTAPI
imp_NetOffloadIsTxChecksumIPv4Enabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->IPv4;
}

BOOLEAN
NTAPI
imp_NetOffloadIsTxChecksumTcpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->Tcp;
}

BOOLEAN
NTAPI
imp_NetOffloadIsTxChecksumUdpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->Udp;
}

VOID
NTAPI
imp_NetAdapterOffloadSetRxChecksumCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES * Capabilities)
{
    PNX_ADAPTER NxAdapter;

    NxGetPrivateGlobals(DriverGlobals);
    NxAdapter = NxAdapterGet(Adapter);

    NX_COPY_SIZED(&NxAdapter->RxChecksumCapabilities, Capabilities);
    NxAdapter->RxChecksumCapabilitiesSet = TRUE;

    /* Receive checksum validation is all or nothing */
    NxAdapter->RxChecksumOffload.IPv4 = TRUE;
    NxAdapter->RxChecksumOffload.Tcp = TRUE;
    NxAdapter->RxChecksumOffload.Udp = TRUE;
}

BOOLEAN
NTAPI
imp_NetOffloadIsRxChecksumIPv4Enabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->IPv4;
}

BOOLEAN
NTAPI
imp_NetOffloadIsRxChecksumTcpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->Tcp;
}

BOOLEAN
NTAPI
imp_NetOffloadIsRxChecksumUdpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxOffloadGet(Offload)->Udp;
}

NETADAPTER
NTAPI
imp_NetAdapterInitGetCreatedAdapter(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER_INIT * AdapterInit)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxAdapterInitGet(AdapterInit)->CreatedAdapter;
}

WDFOBJECT
NTAPI
imp_NetAdapterGetParent(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter)
{
    NxGetPrivateGlobals(DriverGlobals);
    return (WDFOBJECT)NxAdapterGet(Adapter)->Device;
}

ULONG
NTAPI
imp_NetAdapterGetLinkLayerMtuSize(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxAdapterGet(Adapter)->MtuSize;
}

NDIS_HANDLE
NTAPI
imp_NetAdapterWdmGetNdisHandle(
    _In_ NET_DRIVER_GLOBALS * Globals,
    _In_ NETADAPTER Adapter)
{
    NxGetPrivateGlobals(Globals);
    return NxAdapterGet(Adapter)->NdisHandle;
}

NDIS_HANDLE
NTAPI
imp_NetAdapterDriverWdmGetHandle(
    _In_ NET_DRIVER_GLOBALS * Globals,
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    return NxGetPrivateGlobals(Globals)->MiniportDriverHandle;
}
