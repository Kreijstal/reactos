/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implemented client entry points
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Parameter lists as in the NETEXPORT definitions of the MIT-licensed
 * NetAdapterCx sources (release_21H2). Where the client headers declare a
 * PFN_ type for an entry point, driver.c checks the definition against it.
 */

#pragma once

NETADAPTER_INIT *
NTAPI
imp_NetAdapterInitAllocate(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ WDFDEVICE Device);

void
NTAPI
imp_NetAdapterInitFree(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER_INIT * AdapterInit);

void
NTAPI
imp_NetAdapterInitSetDatapathCallbacks(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _Inout_ NETADAPTER_INIT * AdapterInit,
    _In_ NET_ADAPTER_DATAPATH_CALLBACKS * DatapathCallbacks);

NTSTATUS
NTAPI
imp_NetAdapterCreate(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER_INIT * AdapterInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * AdapterAttributes,
    _Out_ NETADAPTER * Adapter);

NTSTATUS
NTAPI
imp_NetAdapterStart(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter);

void
NTAPI
imp_NetAdapterStop(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter);

void
NTAPI
imp_NetAdapterSetLinkLayerCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_CAPABILITIES * LinkLayerCapabilities);

void
NTAPI
imp_NetAdapterSetLinkLayerMtuSize(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ ULONG MtuSize);

void
NTAPI
imp_NetAdapterSetDataPathCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_TX_CAPABILITIES * TxCapabilities,
    _In_ NET_ADAPTER_RX_CAPABILITIES * RxCapabilities);

void
NTAPI
imp_NetAdapterSetLinkState(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_STATE * State);

NET_LUID
NTAPI
imp_NetAdapterGetNetLuid(
    _In_ NET_DRIVER_GLOBALS * Globals,
    _In_ NETADAPTER Adapter);

void
NTAPI
imp_NetAdapterSetPermanentLinkLayerAddress(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress);

void
NTAPI
imp_NetAdapterSetCurrentLinkLayerAddress(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress);

void
NTAPI
imp_NetAdapterOffloadSetChecksumCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES * HardwareCapabilities);

BOOLEAN
NTAPI
imp_NetOffloadIsChecksumIPv4Enabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

BOOLEAN
NTAPI
imp_NetOffloadIsChecksumTcpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

BOOLEAN
NTAPI
imp_NetOffloadIsChecksumUdpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

NETADAPTER
NTAPI
imp_NetAdapterInitGetCreatedAdapter(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER_INIT * AdapterInit);

WDFOBJECT
NTAPI
imp_NetAdapterGetParent(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter);

ULONG
NTAPI
imp_NetAdapterGetLinkLayerMtuSize(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter);

NDIS_HANDLE
NTAPI
imp_NetAdapterWdmGetNdisHandle(
    _In_ NET_DRIVER_GLOBALS * Globals,
    _In_ NETADAPTER Adapter);

NDIS_HANDLE
NTAPI
imp_NetAdapterDriverWdmGetHandle(
    _In_ NET_DRIVER_GLOBALS * Globals,
    _In_ WDFDRIVER Driver);

NTSTATUS
NTAPI
imp_NetDeviceInitConfig(
    _In_ NET_DRIVER_GLOBALS * Globals,
    _Inout_ PWDFDEVICE_INIT DeviceInit);

NTSTATUS
NTAPI
imp_NetRxQueueCreate(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _Inout_ NETRXQUEUE_INIT * NetRxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * RxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG * Configuration,
    _Out_ NETPACKETQUEUE * PacketQueue);

void
NTAPI
imp_NetRxQueueNotifyMoreReceivedPacketsAvailable(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE RxQueue);

ULONG
NTAPI
imp_NetRxQueueInitGetQueueId(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETRXQUEUE_INIT * NetRxQueueInit);

NET_RING_COLLECTION const *
NTAPI
imp_NetRxQueueGetRingCollection(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE NetRxQueue);

void
NTAPI
imp_NetRxQueueGetExtension(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE NetRxQueue,
    _In_ NET_EXTENSION_QUERY const * Query,
    _Out_ NET_EXTENSION* Extension);

NTSTATUS
NTAPI
imp_NetTxQueueCreate(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _Inout_ NETTXQUEUE_INIT * NetTxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * TxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG * Configuration,
    _Out_ NETPACKETQUEUE * PacketQueue);

void
NTAPI
imp_NetTxQueueNotifyMoreCompletedPacketsAvailable(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE TxQueue);

ULONG
NTAPI
imp_NetTxQueueInitGetQueueId(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETTXQUEUE_INIT * NetTxQueueInit);

NET_RING_COLLECTION const *
NTAPI
imp_NetTxQueueGetRingCollection(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE NetTxQueue);

void
NTAPI
imp_NetTxQueueGetExtension(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE NetTxQueue,
    _In_ NET_EXTENSION_QUERY const * Query,
    _Out_ NET_EXTENSION* Extension);

void
NTAPI
imp_NetAdapterOffloadSetTxChecksumCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES * HardwareCapabilities);

BOOLEAN
NTAPI
imp_NetOffloadIsTxChecksumIPv4Enabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

BOOLEAN
NTAPI
imp_NetOffloadIsTxChecksumTcpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

BOOLEAN
NTAPI
imp_NetOffloadIsTxChecksumUdpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

void
NTAPI
imp_NetAdapterOffloadSetRxChecksumCapabilities(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES * Capabilities);

BOOLEAN
NTAPI
imp_NetOffloadIsRxChecksumIPv4Enabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

BOOLEAN
NTAPI
imp_NetOffloadIsRxChecksumTcpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

BOOLEAN
NTAPI
imp_NetOffloadIsRxChecksumUdpEnabled(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETOFFLOAD Offload);

UINT8
NTAPI
imp_NetTxQueueGetDemux8021p(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE Queue);

VOID
NxFillStubSlots(
    _Inout_updates_(NetFunctionTableNumEntries) NETFUNC *Table);
