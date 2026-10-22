/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx adapter objects and capabilities
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifndef _NETADAPTERCXTYPES_H_
#error netadaptercxtypes.h must be included before netadapter.h
#endif

#include <net/returncontexttypes.h>

WDF_EXTERN_C_START

#if NETADAPTER_VERSION_MINOR >= 1
#define NET_ADAPTER_LINK_SPEED_UNKNOWN ((ULONG64)-1)
#endif

typedef enum _NET_ADAPTER_AUTO_NEGOTIATION_FLAGS
{
    NetAdapterAutoNegotiationFlagNone = 0x00000000,
    NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated = 0x00000001,
    NetAdapterAutoNegotiationFlagRcvLinkSpeedautoNegotiated = 0x00000002,
    NetAdapterAutoNegotiationFlagDuplexAutoNegotiated = 0x00000004,
    NetAdapterAutoNegotiationFlagPauseFunctionsAutoNegotiated = 0x00000008,
} NET_ADAPTER_AUTO_NEGOTIATION_FLAGS;

typedef enum _NET_ADAPTER_PAUSE_FUNCTION_TYPE
{
    NetAdapterPauseFunctionTypeUnsupported = 0,
    NetAdapterPauseFunctionTypeSendOnly = 1,
    NetAdapterPauseFunctionTypeReceiveOnly = 2,
    NetAdapterPauseFunctionTypeSendAndReceive = 3,
    NetAdapterPauseFunctionTypeUnknown = 4,
} NET_ADAPTER_PAUSE_FUNCTION_TYPE;

#if NETADAPTER_VERSION_MINOR >= 1
typedef enum _NET_ADAPTER_WAKE_PATTERN_ID
{
    NetAdapterWakeFilterPatternId = 0x0000fffc,
    NetAdapterWakeEapolPatternId = 0x0000fffd,
    NetAdapterWakeMagicPatternId = 0x0000fffe,
} NET_ADAPTER_WAKE_PATTERN_ID;

typedef enum _NET_PACKET_FILTER_FLAGS
{
    NetPacketFilterFlagDirected = 0x00000001,
    NetPacketFilterFlagMulticast = 0x00000002,
    NetPacketFilterFlagAllMulticast = 0x00000004,
    NetPacketFilterFlagBroadcast = 0x00000008,
    NetPacketFilterFlagPromiscuous = 0x00000020,
} NET_PACKET_FILTER_FLAGS;
#endif

typedef enum _NET_MEMORY_MAPPING_REQUIREMENT
{
    NetMemoryMappingRequirementNone = 0,
    NetMemoryMappingRequirementDmaMapped = 1,
} NET_MEMORY_MAPPING_REQUIREMENT;

typedef enum _NET_RX_FRAGMENT_BUFFER_ALLOCATION_MODE
{
    NetRxFragmentBufferAllocationModeSystem = 0,
    NetRxFragmentBufferAllocationModeDriver = 1,
} NET_RX_FRAGMENT_BUFFER_ALLOCATION_MODE;

typedef enum _NET_RX_FRAGMENT_BUFFER_ATTACHMENT_MODE
{
    NetRxFragmentBufferAttachmentModeSystem = 0,
    NetRxFragmentBufferAttachmentModeDriver = 1,
} NET_RX_FRAGMENT_BUFFER_ATTACHMENT_MODE;

/* Same layout as NDIS_IF_PHYSICAL_ADDRESS */
typedef struct _NET_ADAPTER_LINK_LAYER_ADDRESS
{
    USHORT Length;
    UCHAR Address[32];
} NET_ADAPTER_LINK_LAYER_ADDRESS;

FORCEINLINE
VOID
NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(
    _Out_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress,
    _In_range_(1, 32) USHORT Length,
    _In_reads_bytes_(Length) UCHAR const * AddressBuffer)
{
    RtlZeroMemory(LinkLayerAddress, sizeof(*LinkLayerAddress));
    LinkLayerAddress->Length = Length;
    RtlCopyMemory(LinkLayerAddress->Address, AddressBuffer, Length);
}

/* Datapath queue creation */

typedef
_Function_class_(EVT_NET_ADAPTER_CREATE_TXQUEUE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_NET_ADAPTER_CREATE_TXQUEUE(
    _In_ NETADAPTER Adapter,
    _Inout_ NETTXQUEUE_INIT * TxQueueInit);
typedef EVT_NET_ADAPTER_CREATE_TXQUEUE *PFN_NET_ADAPTER_CREATE_TXQUEUE;

typedef
_Function_class_(EVT_NET_ADAPTER_CREATE_RXQUEUE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_NET_ADAPTER_CREATE_RXQUEUE(
    _In_ NETADAPTER Adapter,
    _Inout_ NETRXQUEUE_INIT * RxQueueInit);
typedef EVT_NET_ADAPTER_CREATE_RXQUEUE *PFN_NET_ADAPTER_CREATE_RXQUEUE;

typedef struct _NET_ADAPTER_DATAPATH_CALLBACKS
{
    ULONG Size;
    PFN_NET_ADAPTER_CREATE_TXQUEUE EvtAdapterCreateTxQueue;
    PFN_NET_ADAPTER_CREATE_RXQUEUE EvtAdapterCreateRxQueue;
} NET_ADAPTER_DATAPATH_CALLBACKS;

FORCEINLINE
VOID
NET_ADAPTER_DATAPATH_CALLBACKS_INIT(
    _Out_ NET_ADAPTER_DATAPATH_CALLBACKS * DatapathCallbacks,
    _In_ PFN_NET_ADAPTER_CREATE_TXQUEUE EvtAdapterCreateTxQueue,
    _In_ PFN_NET_ADAPTER_CREATE_RXQUEUE EvtAdapterCreateRxQueue)
{
    RtlZeroMemory(DatapathCallbacks, sizeof(*DatapathCallbacks));
    DatapathCallbacks->Size = sizeof(*DatapathCallbacks);
    DatapathCallbacks->EvtAdapterCreateTxQueue = EvtAdapterCreateTxQueue;
    DatapathCallbacks->EvtAdapterCreateRxQueue = EvtAdapterCreateRxQueue;
}

/* Link layer */

typedef struct _NET_ADAPTER_LINK_LAYER_CAPABILITIES
{
    ULONG Size;
    ULONG64 MaxTxLinkSpeed;
    ULONG64 MaxRxLinkSpeed;
} NET_ADAPTER_LINK_LAYER_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_LINK_LAYER_CAPABILITIES * LinkLayerCapabilities,
    _In_ ULONG64 MaxTxLinkSpeed,
    _In_ ULONG64 MaxRxLinkSpeed)
{
    RtlZeroMemory(LinkLayerCapabilities, sizeof(*LinkLayerCapabilities));
    LinkLayerCapabilities->Size = sizeof(*LinkLayerCapabilities);
    LinkLayerCapabilities->MaxTxLinkSpeed = MaxTxLinkSpeed;
    LinkLayerCapabilities->MaxRxLinkSpeed = MaxRxLinkSpeed;
}

typedef struct _NET_ADAPTER_LINK_STATE
{
    ULONG Size;
    ULONG64 TxLinkSpeed;
    ULONG64 RxLinkSpeed;
    NET_IF_MEDIA_CONNECT_STATE MediaConnectState;
    NET_IF_MEDIA_DUPLEX_STATE MediaDuplexState;
    NET_ADAPTER_PAUSE_FUNCTION_TYPE SupportedPauseFunctions;
    NET_ADAPTER_AUTO_NEGOTIATION_FLAGS AutoNegotiationFlags;
} NET_ADAPTER_LINK_STATE;

FORCEINLINE
VOID
NET_ADAPTER_LINK_STATE_INIT(
    _Out_ NET_ADAPTER_LINK_STATE * LinkState,
    _In_ ULONG64 LinkSpeed,
    _In_ NET_IF_MEDIA_CONNECT_STATE MediaConnectState,
    _In_ NET_IF_MEDIA_DUPLEX_STATE MediaDuplexState,
    _In_ NET_ADAPTER_PAUSE_FUNCTION_TYPE SupportedPauseFunctions,
    _In_ NET_ADAPTER_AUTO_NEGOTIATION_FLAGS AutoNegotiationFlags)
{
    RtlZeroMemory(LinkState, sizeof(*LinkState));
    LinkState->Size = sizeof(*LinkState);
    LinkState->TxLinkSpeed = LinkSpeed;
    LinkState->RxLinkSpeed = LinkSpeed;
    LinkState->MediaConnectState = MediaConnectState;
    LinkState->MediaDuplexState = MediaDuplexState;
    LinkState->SupportedPauseFunctions = SupportedPauseFunctions;
    LinkState->AutoNegotiationFlags = AutoNegotiationFlags;
}

FORCEINLINE
VOID
NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(
    _Out_ NET_ADAPTER_LINK_STATE * LinkState)
{
    NET_ADAPTER_LINK_STATE_INIT(LinkState,
                                (ULONG64)-1,
                                MediaConnectStateDisconnected,
                                MediaDuplexStateUnknown,
                                NetAdapterPauseFunctionTypeUnsupported,
                                NetAdapterAutoNegotiationFlagNone);
}

/* Datapath capabilities */

typedef struct _NET_ADAPTER_DMA_CAPABILITIES
{
    ULONG Size;
    WDFDMAENABLER DmaEnabler;
    PHYSICAL_ADDRESS MaximumPhysicalAddress;
    WDF_TRI_STATE CacheEnabled;
    NODE_REQUIREMENT PreferredNode;
} NET_ADAPTER_DMA_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_DMA_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_DMA_CAPABILITIES * DmaCapabilities,
    _In_ WDFDMAENABLER DmaEnabler)
{
    RtlZeroMemory(DmaCapabilities, sizeof(*DmaCapabilities));
    DmaCapabilities->Size = sizeof(*DmaCapabilities);
    DmaCapabilities->DmaEnabler = DmaEnabler;
    DmaCapabilities->CacheEnabled = WdfUseDefault;
    DmaCapabilities->PreferredNode = MM_ANY_NODE_OK;
}

typedef struct _NET_ADAPTER_TX_CAPABILITIES
{
    ULONG Size;
    NET_MEMORY_MAPPING_REQUIREMENT MappingRequirement;
    SIZE_T PayloadBackfill;
    SIZE_T MaximumNumberOfFragments;
    SIZE_T FragmentBufferAlignment;
    UINT32 FragmentRingNumberOfElementsHint;
    SIZE_T MaximumNumberOfQueues;
    NET_ADAPTER_DMA_CAPABILITIES * DmaCapabilities;
} NET_ADAPTER_TX_CAPABILITIES;

/* Defaults: no fragment count limit, byte alignment */
FORCEINLINE
VOID
NET_ADAPTER_TX_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_TX_CAPABILITIES * TxCapabilities,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    RtlZeroMemory(TxCapabilities, sizeof(*TxCapabilities));
    TxCapabilities->Size = sizeof(*TxCapabilities);
    TxCapabilities->MappingRequirement = NetMemoryMappingRequirementNone;
    TxCapabilities->MaximumNumberOfFragments = ~(SIZE_T)0;
    TxCapabilities->FragmentBufferAlignment = 1;
    TxCapabilities->MaximumNumberOfQueues = MaximumNumberOfQueues;
}

FORCEINLINE
VOID
NET_ADAPTER_TX_CAPABILITIES_INIT_FOR_DMA(
    _Out_ NET_ADAPTER_TX_CAPABILITIES * TxCapabilities,
    _In_ NET_ADAPTER_DMA_CAPABILITIES * DmaCapabilities,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    NET_ADAPTER_TX_CAPABILITIES_INIT(TxCapabilities, MaximumNumberOfQueues);
    TxCapabilities->MappingRequirement = NetMemoryMappingRequirementDmaMapped;
    TxCapabilities->DmaCapabilities = DmaCapabilities;
}

typedef
_Function_class_(EVT_NET_ADAPTER_RETURN_RX_BUFFER)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_RETURN_RX_BUFFER(
    _In_ NETADAPTER Adapter,
    _In_ NET_FRAGMENT_RETURN_CONTEXT_HANDLE RxReturnContext);
typedef EVT_NET_ADAPTER_RETURN_RX_BUFFER *PFN_NET_ADAPTER_RETURN_RX_BUFFER;

typedef struct _NET_ADAPTER_RX_CAPABILITIES
{
    ULONG Size;
    NET_RX_FRAGMENT_BUFFER_ALLOCATION_MODE AllocationMode;
    NET_RX_FRAGMENT_BUFFER_ATTACHMENT_MODE AttachmentMode;
    UINT32 FragmentRingNumberOfElementsHint;
    SIZE_T MaximumFrameSize;
    SIZE_T MaximumNumberOfQueues;
    union
    {
        /* NetRxFragmentBufferAllocationModeDriver */
        struct
        {
            PFN_NET_ADAPTER_RETURN_RX_BUFFER EvtAdapterReturnRxBuffer;
        } DUMMYSTRUCTNAME;
        /* NetRxFragmentBufferAllocationModeSystem */
        struct
        {
            NET_MEMORY_MAPPING_REQUIREMENT MappingRequirement;
            SIZE_T FragmentBufferAlignment;
            NET_ADAPTER_DMA_CAPABILITIES * DmaCapabilities;
        } DUMMYSTRUCTNAME2;
    } DUMMYUNIONNAME;
} NET_ADAPTER_RX_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(
    _Out_ NET_ADAPTER_RX_CAPABILITIES * RxCapabilities,
    _In_ SIZE_T MaximumFrameSize,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    RtlZeroMemory(RxCapabilities, sizeof(*RxCapabilities));
    RxCapabilities->Size = sizeof(*RxCapabilities);
    RxCapabilities->AllocationMode = NetRxFragmentBufferAllocationModeSystem;
    RxCapabilities->AttachmentMode = NetRxFragmentBufferAttachmentModeSystem;
    RxCapabilities->MaximumFrameSize = MaximumFrameSize;
    RxCapabilities->MaximumNumberOfQueues = MaximumNumberOfQueues;
    RxCapabilities->MappingRequirement = NetMemoryMappingRequirementNone;
    RxCapabilities->FragmentBufferAlignment = 1;
}

FORCEINLINE
VOID
NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(
    _Out_ NET_ADAPTER_RX_CAPABILITIES * RxCapabilities,
    _In_ NET_ADAPTER_DMA_CAPABILITIES * DmaCapabilities,
    _In_ SIZE_T MaximumFrameSize,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(RxCapabilities, MaximumFrameSize, MaximumNumberOfQueues);
    RxCapabilities->MappingRequirement = NetMemoryMappingRequirementDmaMapped;
    RxCapabilities->DmaCapabilities = DmaCapabilities;
}

FORCEINLINE
VOID
NET_ADAPTER_RX_CAPABILITIES_INIT_DRIVER_MANAGED(
    _Out_ NET_ADAPTER_RX_CAPABILITIES * RxCapabilities,
    _In_ PFN_NET_ADAPTER_RETURN_RX_BUFFER EvtAdapterReturnRxBuffer,
    _In_ SIZE_T MaximumFrameSize,
    _In_ SIZE_T MaximumNumberOfQueues)
{
    RtlZeroMemory(RxCapabilities, sizeof(*RxCapabilities));
    RxCapabilities->Size = sizeof(*RxCapabilities);
    RxCapabilities->AllocationMode = NetRxFragmentBufferAllocationModeDriver;
    RxCapabilities->AttachmentMode = NetRxFragmentBufferAttachmentModeDriver;
    RxCapabilities->MaximumFrameSize = MaximumFrameSize;
    RxCapabilities->MaximumNumberOfQueues = MaximumNumberOfQueues;
    /* Same default as the system managed modes; it shares the union */
    RxCapabilities->FragmentBufferAlignment = 1;
    RxCapabilities->EvtAdapterReturnRxBuffer = EvtAdapterReturnRxBuffer;
}

/* 2.0 checksum offload, superseded by netadapteroffload.h in 2.1 */

typedef
_Function_class_(EVT_NET_ADAPTER_OFFLOAD_SET_CHECKSUM)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_OFFLOAD_SET_CHECKSUM(
    _In_ NETADAPTER Adapter,
    _In_ NETOFFLOAD Offload);
typedef EVT_NET_ADAPTER_OFFLOAD_SET_CHECKSUM *PFN_NET_ADAPTER_OFFLOAD_SET_CHECKSUM;

typedef struct _NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES
{
    ULONG Size;
    BOOLEAN IPv4;
    BOOLEAN Tcp;
    BOOLEAN Udp;
    PFN_NET_ADAPTER_OFFLOAD_SET_CHECKSUM EvtAdapterOffloadSetChecksum;
} NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES * ChecksumCapabilities,
    _In_ BOOLEAN IPv4,
    _In_ BOOLEAN Tcp,
    _In_ BOOLEAN Udp,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_CHECKSUM EvtAdapterOffloadSetChecksum)
{
    RtlZeroMemory(ChecksumCapabilities, sizeof(*ChecksumCapabilities));
    ChecksumCapabilities->Size = sizeof(*ChecksumCapabilities);
    ChecksumCapabilities->IPv4 = IPv4;
    ChecksumCapabilities->Tcp = Tcp;
    ChecksumCapabilities->Udp = Udp;
    ChecksumCapabilities->EvtAdapterOffloadSetChecksum = EvtAdapterOffloadSetChecksum;
}

/* Power management */

typedef struct _NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES
{
    ULONG Size;
    BOOLEAN ArpOffload;
    SIZE_T MaximumOffloadCount;
} NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES * Capabilities,
    _In_ SIZE_T MaximumOffloadCount)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->ArpOffload = TRUE;
    Capabilities->MaximumOffloadCount = MaximumOffloadCount;
}

typedef struct _NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES
{
    ULONG Size;
    BOOLEAN NSOffload;
    SIZE_T MaximumOffloadCount;
} NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES * Capabilities,
    _In_ SIZE_T MaximumOffloadCount)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->NSOffload = TRUE;
    Capabilities->MaximumOffloadCount = MaximumOffloadCount;
}

typedef struct _NET_ADAPTER_WAKE_BITMAP_CAPABILITIES
{
    ULONG Size;
    BOOLEAN BitmapPattern;
    SIZE_T MaximumPatternCount;
    SIZE_T MaximumPatternSize;
} NET_ADAPTER_WAKE_BITMAP_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_WAKE_BITMAP_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_BITMAP_CAPABILITIES * Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

typedef struct _NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES
{
    ULONG Size;
    BOOLEAN MagicPacket;
} NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES * Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

typedef struct _NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES
{
    ULONG Size;
    BOOLEAN MediaConnect;
    BOOLEAN MediaDisconnect;
} NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES * Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

typedef struct _NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES
{
    ULONG Size;
    BOOLEAN PacketFilterMatch;
} NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES * Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

typedef struct _NET_ADAPTER_WAKE_REASON_PACKET
{
    ULONG Size;
    ULONG PatternId;
    ULONG OriginalPacketSize;
    WDFMEMORY WakePacket;
} NET_ADAPTER_WAKE_REASON_PACKET;

FORCEINLINE
VOID
NET_ADAPTER_WAKE_REASON_PACKET_INIT(
    _Out_ NET_ADAPTER_WAKE_REASON_PACKET * Reason)
{
    RtlZeroMemory(Reason, sizeof(*Reason));
    Reason->Size = sizeof(*Reason);
}

#if NETADAPTER_VERSION_MINOR >= 1
FORCEINLINE
VOID
NET_ADAPTER_WAKE_REASON_MAGIC_PACKET_INIT(
    _Out_ NET_ADAPTER_WAKE_REASON_PACKET * Reason)
{
    NET_ADAPTER_WAKE_REASON_PACKET_INIT(Reason);
    Reason->PatternId = NetAdapterWakeMagicPatternId;
}

FORCEINLINE
VOID
NET_ADAPTER_WAKE_REASON_EAPOL_PACKET_INIT(
    _Out_ NET_ADAPTER_WAKE_REASON_PACKET * Reason)
{
    NET_ADAPTER_WAKE_REASON_PACKET_INIT(Reason);
    Reason->PatternId = NetAdapterWakeEapolPatternId;
}

FORCEINLINE
VOID
NET_ADAPTER_WAKE_REASON_FILTER_PACKET_INIT(
    _Out_ NET_ADAPTER_WAKE_REASON_PACKET * Reason)
{
    NET_ADAPTER_WAKE_REASON_PACKET_INIT(Reason);
    Reason->PatternId = NetAdapterWakeFilterPatternId;
}

typedef struct _NET_ADAPTER_WAKE_EAPOL_PACKET_CAPABILITIES
{
    ULONG Size;
    BOOLEAN EapolPacket;
} NET_ADAPTER_WAKE_EAPOL_PACKET_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_WAKE_EAPOL_PACKET_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_WAKE_EAPOL_PACKET_CAPABILITIES * Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
}

/* Receive filtering */

typedef
_Function_class_(EVT_NET_ADAPTER_SET_RECEIVE_FILTER)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_SET_RECEIVE_FILTER(
    _In_ NETADAPTER Adapter,
    _In_ NETRECEIVEFILTER ReceiveFilter);
typedef EVT_NET_ADAPTER_SET_RECEIVE_FILTER *PFN_NET_ADAPTER_SET_RECEIVE_FILTER;

typedef struct _NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES
{
    ULONG Size;
    NET_PACKET_FILTER_FLAGS SupportedPacketFilters;
    SIZE_T MaximumMulticastAddresses;
    PFN_NET_ADAPTER_SET_RECEIVE_FILTER EvtSetReceiveFilter;
} NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES * Capabilities,
    _In_ PFN_NET_ADAPTER_SET_RECEIVE_FILTER EvtSetReceiveFilter)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->EvtSetReceiveFilter = EvtSetReceiveFilter;
}
#endif /* 2.1 */

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NETADAPTER_INIT *
(NTAPI *PFN_NETADAPTERINITALLOCATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NETADAPTER_INIT *
NetAdapterInitAllocate(
    _In_ WDFDEVICE Device)
{
    return ((PFN_NETADAPTERINITALLOCATE)NetFunctions[NetAdapterInitAllocateTableIndex])(NetDriverGlobals, Device);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERINITFREE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT * AdapterInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterInitFree(
    _In_ NETADAPTER_INIT * AdapterInit)
{
    ((PFN_NETADAPTERINITFREE)NetFunctions[NetAdapterInitFreeTableIndex])(NetDriverGlobals, AdapterInit);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERINITSETDATAPATHCALLBACKS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTER_INIT * AdapterInit,
    _In_ NET_ADAPTER_DATAPATH_CALLBACKS * DatapathCallbacks);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterInitSetDatapathCallbacks(
    _Inout_ NETADAPTER_INIT * AdapterInit,
    _In_ NET_ADAPTER_DATAPATH_CALLBACKS * DatapathCallbacks)
{
    ((PFN_NETADAPTERINITSETDATAPATHCALLBACKS)NetFunctions[NetAdapterInitSetDatapathCallbacksTableIndex])(NetDriverGlobals, AdapterInit, DatapathCallbacks);
}

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETADAPTERCREATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT * AdapterInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * AdapterAttributes,
    _Out_ NETADAPTER * Adapter);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NetAdapterCreate(
    _In_ NETADAPTER_INIT * AdapterInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * AdapterAttributes,
    _Out_ NETADAPTER * Adapter)
{
    return ((PFN_NETADAPTERCREATE)NetFunctions[NetAdapterCreateTableIndex])(NetDriverGlobals, AdapterInit, AdapterAttributes, Adapter);
}

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETADAPTERSTART)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NetAdapterStart(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERSTART)NetFunctions[NetAdapterStartTableIndex])(NetDriverGlobals, Adapter);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSTOP)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterStop(
    _In_ NETADAPTER Adapter)
{
    ((PFN_NETADAPTERSTOP)NetFunctions[NetAdapterStopTableIndex])(NetDriverGlobals, Adapter);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETLINKLAYERCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_CAPABILITIES * LinkLayerCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterSetLinkLayerCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_CAPABILITIES * LinkLayerCapabilities)
{
    ((PFN_NETADAPTERSETLINKLAYERCAPABILITIES)NetFunctions[NetAdapterSetLinkLayerCapabilitiesTableIndex])(NetDriverGlobals, Adapter, LinkLayerCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETLINKLAYERMTUSIZE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ ULONG MtuSize);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterSetLinkLayerMtuSize(
    _In_ NETADAPTER Adapter,
    _In_ ULONG MtuSize)
{
    ((PFN_NETADAPTERSETLINKLAYERMTUSIZE)NetFunctions[NetAdapterSetLinkLayerMtuSizeTableIndex])(NetDriverGlobals, Adapter, MtuSize);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERPOWEROFFLOADSETARPCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterPowerOffloadSetArpCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTERPOWEROFFLOADSETARPCAPABILITIES)NetFunctions[NetAdapterPowerOffloadSetArpCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERPOWEROFFLOADSETNSCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterPowerOffloadSetNSCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTERPOWEROFFLOADSETNSCAPABILITIES)NetFunctions[NetAdapterPowerOffloadSetNSCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETBITMAPCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_BITMAP_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterWakeSetBitmapCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_BITMAP_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTERWAKESETBITMAPCAPABILITIES)NetFunctions[NetAdapterWakeSetBitmapCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETMEDIACHANGECAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterWakeSetMediaChangeCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTERWAKESETMEDIACHANGECAPABILITIES)NetFunctions[NetAdapterWakeSetMediaChangeCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETMAGICPACKETCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterWakeSetMagicPacketCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTERWAKESETMAGICPACKETCAPABILITIES)NetFunctions[NetAdapterWakeSetMagicPacketCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETPACKETFILTERCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterWakeSetPacketFilterCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTERWAKESETPACKETFILTERCAPABILITIES)NetFunctions[NetAdapterWakeSetPacketFilterCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETDATAPATHCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_TX_CAPABILITIES * TxCapabilities,
    _In_ NET_ADAPTER_RX_CAPABILITIES * RxCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterSetDataPathCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_TX_CAPABILITIES * TxCapabilities,
    _In_ NET_ADAPTER_RX_CAPABILITIES * RxCapabilities)
{
    ((PFN_NETADAPTERSETDATAPATHCAPABILITIES)NetFunctions[NetAdapterSetDataPathCapabilitiesTableIndex])(NetDriverGlobals, Adapter, TxCapabilities, RxCapabilities);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETLINKSTATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_STATE * State);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NetAdapterSetLinkState(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_STATE * State)
{
    ((PFN_NETADAPTERSETLINKSTATE)NetFunctions[NetAdapterSetLinkStateTableIndex])(NetDriverGlobals, Adapter, State);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NET_LUID
(NTAPI *PFN_NETADAPTERGETNETLUID)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NET_LUID
NetAdapterGetNetLuid(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERGETNETLUID)NetFunctions[NetAdapterGetNetLuidTableIndex])(NetDriverGlobals, Adapter);
}

typedef
_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETADAPTEROPENCONFIGURATION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * ConfigurationAttributes,
    _Out_ NETCONFIGURATION * Configuration);

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NetAdapterOpenConfiguration(
    _In_ NETADAPTER Adapter,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * ConfigurationAttributes,
    _Out_ NETCONFIGURATION * Configuration)
{
    return ((PFN_NETADAPTEROPENCONFIGURATION)NetFunctions[NetAdapterOpenConfigurationTableIndex])(NetDriverGlobals, Adapter, ConfigurationAttributes, Configuration);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETPERMANENTLINKLAYERADDRESS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterSetPermanentLinkLayerAddress(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress)
{
    ((PFN_NETADAPTERSETPERMANENTLINKLAYERADDRESS)NetFunctions[NetAdapterSetPermanentLinkLayerAddressTableIndex])(NetDriverGlobals, Adapter, LinkLayerAddress);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETCURRENTLINKLAYERADDRESS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterSetCurrentLinkLayerAddress(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS * LinkLayerAddress)
{
    ((PFN_NETADAPTERSETCURRENTLINKLAYERADDRESS)NetFunctions[NetAdapterSetCurrentLinkLayerAddressTableIndex])(NetDriverGlobals, Adapter, LinkLayerAddress);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETCHECKSUMCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES * HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterOffloadSetChecksumCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES * HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETCHECKSUMCAPABILITIES)NetFunctions[NetAdapterOffloadSetChecksumCapabilitiesTableIndex])(NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISCHECKSUMIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsChecksumIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISCHECKSUMIPV4ENABLED)NetFunctions[NetOffloadIsChecksumIPv4EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISCHECKSUMTCPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsChecksumTcpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISCHECKSUMTCPENABLED)NetFunctions[NetOffloadIsChecksumTcpEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISCHECKSUMUDPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsChecksumUdpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISCHECKSUMUDPENABLED)NetFunctions[NetOffloadIsChecksumUdpEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERREPORTWAKEREASONPACKET)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_REASON_PACKET const * Reason);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterReportWakeReasonPacket(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_REASON_PACKET const * Reason)
{
    ((PFN_NETADAPTERREPORTWAKEREASONPACKET)NetFunctions[NetAdapterReportWakeReasonPacketTableIndex])(NetDriverGlobals, Adapter, Reason);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERREPORTWAKEREASONMEDIACHANGE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_IF_MEDIA_CONNECT_STATE Reason);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterReportWakeReasonMediaChange(
    _In_ NETADAPTER Adapter,
    _In_ NET_IF_MEDIA_CONNECT_STATE Reason)
{
    ((PFN_NETADAPTERREPORTWAKEREASONMEDIACHANGE)NetFunctions[NetAdapterReportWakeReasonMediaChangeTableIndex])(NetDriverGlobals, Adapter, Reason);
}

#if NETADAPTER_VERSION_MINOR >= 1

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERWAKESETEAPOLPACKETCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_EAPOL_PACKET_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterWakeSetEapolPacketCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_WAKE_EAPOL_PACKET_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTERWAKESETEAPOLPACKETCAPABILITIES)NetFunctions[NetAdapterWakeSetEapolPacketCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETRECEIVEFILTERCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterSetReceiveFilterCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTERSETRECEIVEFILTERCAPABILITIES)NetFunctions[NetAdapterSetReceiveFilterCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_PACKET_FILTER_FLAGS
(NTAPI *PFN_NETRECEIVEFILTERGETPACKETFILTER)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NET_PACKET_FILTER_FLAGS
NetReceiveFilterGetPacketFilter(
    _In_ NETRECEIVEFILTER ReceiveFilter)
{
    return ((PFN_NETRECEIVEFILTERGETPACKETFILTER)NetFunctions[NetReceiveFilterGetPacketFilterTableIndex])(NetDriverGlobals, ReceiveFilter);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
SIZE_T
(NTAPI *PFN_NETRECEIVEFILTERGETMULTICASTADDRESSCOUNT)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
SIZE_T
NetReceiveFilterGetMulticastAddressCount(
    _In_ NETRECEIVEFILTER ReceiveFilter)
{
    return ((PFN_NETRECEIVEFILTERGETMULTICASTADDRESSCOUNT)NetFunctions[NetReceiveFilterGetMulticastAddressCountTableIndex])(NetDriverGlobals, ReceiveFilter);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_ADAPTER_LINK_LAYER_ADDRESS const *
(NTAPI *PFN_NETRECEIVEFILTERGETMULTICASTADDRESSLIST)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NET_ADAPTER_LINK_LAYER_ADDRESS const *
NetReceiveFilterGetMulticastAddressList(
    _In_ NETRECEIVEFILTER ReceiveFilter)
{
    return ((PFN_NETRECEIVEFILTERGETMULTICASTADDRESSLIST)NetFunctions[NetReceiveFilterGetMulticastAddressListTableIndex])(NetDriverGlobals, ReceiveFilter);
}

#endif /* 2.1 */

WDF_EXTERN_C_END
