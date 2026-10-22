/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Packet queue configuration shared by Tx and Rx queues
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifndef _NETADAPTERCXTYPES_H_
#error netadaptercxtypes.h must be included before netpacketqueue.h
#endif

WDF_EXTERN_C_START

typedef
_Function_class_(EVT_PACKET_QUEUE_START)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_START(
    _In_ NETPACKETQUEUE PacketQueue);
typedef EVT_PACKET_QUEUE_START *PFN_PACKET_QUEUE_START;

typedef
_Function_class_(EVT_PACKET_QUEUE_STOP)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_STOP(
    _In_ NETPACKETQUEUE PacketQueue);
typedef EVT_PACKET_QUEUE_STOP *PFN_PACKET_QUEUE_STOP;

typedef
_Function_class_(EVT_PACKET_QUEUE_ADVANCE)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_ADVANCE(
    _In_ NETPACKETQUEUE PacketQueue);
typedef EVT_PACKET_QUEUE_ADVANCE *PFN_PACKET_QUEUE_ADVANCE;

typedef
_Function_class_(EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ BOOLEAN NotificationEnabled);
typedef EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED *PFN_PACKET_QUEUE_SET_NOTIFICATION_ENABLED;

typedef
_Function_class_(EVT_PACKET_QUEUE_CANCEL)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_CANCEL(
    _In_ NETPACKETQUEUE PacketQueue);
typedef EVT_PACKET_QUEUE_CANCEL *PFN_PACKET_QUEUE_CANCEL;

typedef struct _NET_PACKET_QUEUE_CONFIG
{
    ULONG Size;
    PFN_PACKET_QUEUE_START EvtStart;
    PFN_PACKET_QUEUE_STOP EvtStop;
    PFN_PACKET_QUEUE_ADVANCE EvtAdvance;
    PFN_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtSetNotificationEnabled;
    PFN_PACKET_QUEUE_CANCEL EvtCancel;
#if NETADAPTER_VERSION_MINOR >= 1
    /* Added in 2.1; the framework reads it only when Size covers it */
    NETEXECUTIONCONTEXT ExecutionContext;
#endif
} NET_PACKET_QUEUE_CONFIG;

FORCEINLINE
VOID
NET_PACKET_QUEUE_CONFIG_INIT(
    _Out_ NET_PACKET_QUEUE_CONFIG * Config,
    _In_ PFN_PACKET_QUEUE_ADVANCE EvtAdvance,
    _In_ PFN_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtSetNotificationEnabled,
    _In_ PFN_PACKET_QUEUE_CANCEL EvtCancel)
{
    RtlZeroMemory(Config, sizeof(*Config));
    Config->Size = sizeof(*Config);
    Config->EvtAdvance = EvtAdvance;
    Config->EvtSetNotificationEnabled = EvtSetNotificationEnabled;
    Config->EvtCancel = EvtCancel;
}

WDF_EXTERN_C_END
