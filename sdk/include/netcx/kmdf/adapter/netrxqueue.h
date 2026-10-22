/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx receive queues
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifndef _NETADAPTERCXTYPES_H_
#error netadaptercxtypes.h must be included before netrxqueue.h
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
(NTAPI *PFN_NETRXQUEUECREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETRXQUEUE_INIT * NetRxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * RxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG * Configuration,
    _Out_ NETPACKETQUEUE * PacketQueue);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NetRxQueueCreate(
    _Inout_ NETRXQUEUE_INIT * NetRxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * RxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG * Configuration,
    _Out_ NETPACKETQUEUE * PacketQueue)
{
    return ((PFN_NETRXQUEUECREATE)NetFunctions[NetRxQueueCreateTableIndex])(NetDriverGlobals, NetRxQueueInit, RxQueueAttributes, Configuration, PacketQueue);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETRXQUEUENOTIFYMORERECEIVEDPACKETSAVAILABLE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NetRxQueueNotifyMoreReceivedPacketsAvailable(
    _In_ NETPACKETQUEUE PacketQueue)
{
    ((PFN_NETRXQUEUENOTIFYMORERECEIVEDPACKETSAVAILABLE)NetFunctions[NetRxQueueNotifyMoreReceivedPacketsAvailableTableIndex])(NetDriverGlobals, PacketQueue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
ULONG
(NTAPI *PFN_NETRXQUEUEINITGETQUEUEID)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETRXQUEUE_INIT * NetRxQueueInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
ULONG
NetRxQueueInitGetQueueId(
    _In_ NETRXQUEUE_INIT * NetRxQueueInit)
{
    return ((PFN_NETRXQUEUEINITGETQUEUEID)NetFunctions[NetRxQueueInitGetQueueIdTableIndex])(NetDriverGlobals, NetRxQueueInit);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_RING_COLLECTION const *
(NTAPI *PFN_NETRXQUEUEGETRINGCOLLECTION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NET_RING_COLLECTION const *
NetRxQueueGetRingCollection(
    _In_ NETPACKETQUEUE PacketQueue)
{
    return ((PFN_NETRXQUEUEGETRINGCOLLECTION)NetFunctions[NetRxQueueGetRingCollectionTableIndex])(NetDriverGlobals, PacketQueue);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETRXQUEUEGETEXTENSION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ NET_EXTENSION_QUERY const * Query,
    _Out_ NET_EXTENSION * Extension);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetRxQueueGetExtension(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ NET_EXTENSION_QUERY const * Query,
    _Out_ NET_EXTENSION * Extension)
{
    ((PFN_NETRXQUEUEGETEXTENSION)NetFunctions[NetRxQueueGetExtensionTableIndex])(NetDriverGlobals, PacketQueue, Query, Extension);
}

WDF_EXTERN_C_END
