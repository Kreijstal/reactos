/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx transmit queues
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifndef _NETADAPTERCXTYPES_H_
#error netadaptercxtypes.h must be included before nettxqueue.h
#endif

#include <net/ringcollection.h>
#include <netpacketqueue.h>
#include <netadapterpacket.h>

WDF_EXTERN_C_START

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETTXQUEUECREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETTXQUEUE_INIT * NetTxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * TxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG * Configuration,
    _Out_ NETPACKETQUEUE * PacketQueue);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NetTxQueueCreate(
    _Inout_ NETTXQUEUE_INIT * NetTxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * TxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG * Configuration,
    _Out_ NETPACKETQUEUE * PacketQueue)
{
    return ((PFN_NETTXQUEUECREATE)NetFunctions[NetTxQueueCreateTableIndex])(NetDriverGlobals, NetTxQueueInit, TxQueueAttributes, Configuration, PacketQueue);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETTXQUEUENOTIFYMORECOMPLETEDPACKETSAVAILABLE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NetTxQueueNotifyMoreCompletedPacketsAvailable(
    _In_ NETPACKETQUEUE PacketQueue)
{
    ((PFN_NETTXQUEUENOTIFYMORECOMPLETEDPACKETSAVAILABLE)NetFunctions[NetTxQueueNotifyMoreCompletedPacketsAvailableTableIndex])(NetDriverGlobals, PacketQueue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
ULONG
(NTAPI *PFN_NETTXQUEUEINITGETQUEUEID)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETTXQUEUE_INIT * NetTxQueueInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
ULONG
NetTxQueueInitGetQueueId(
    _In_ NETTXQUEUE_INIT * NetTxQueueInit)
{
    return ((PFN_NETTXQUEUEINITGETQUEUEID)NetFunctions[NetTxQueueInitGetQueueIdTableIndex])(NetDriverGlobals, NetTxQueueInit);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_RING_COLLECTION const *
(NTAPI *PFN_NETTXQUEUEGETRINGCOLLECTION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NET_RING_COLLECTION const *
NetTxQueueGetRingCollection(
    _In_ NETPACKETQUEUE PacketQueue)
{
    return ((PFN_NETTXQUEUEGETRINGCOLLECTION)NetFunctions[NetTxQueueGetRingCollectionTableIndex])(NetDriverGlobals, PacketQueue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETTXQUEUEGETEXTENSION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ NET_EXTENSION_QUERY const * Query,
    _Out_ NET_EXTENSION * Extension);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetTxQueueGetExtension(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ NET_EXTENSION_QUERY const * Query,
    _Out_ NET_EXTENSION * Extension)
{
    ((PFN_NETTXQUEUEGETEXTENSION)NetFunctions[NetTxQueueGetExtensionTableIndex])(NetDriverGlobals, PacketQueue, Query, Extension);
}

#if NETADAPTER_VERSION_MINOR >= 1

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
UINT8
(NTAPI *PFN_NETTXQUEUEGETDEMUX8021P)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
UINT8
NetTxQueueGetDemux8021p(
    _In_ NETPACKETQUEUE PacketQueue)
{
    return ((PFN_NETTXQUEUEGETDEMUX8021P)NetFunctions[NetTxQueueGetDemux8021pTableIndex])(NetDriverGlobals, PacketQueue);
}

#endif /* 2.1 */

WDF_EXTERN_C_END
