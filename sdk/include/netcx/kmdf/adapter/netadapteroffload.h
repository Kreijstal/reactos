/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx 2.1 task offload capabilities
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifndef _NETADAPTERCXTYPES_H_
#error netadaptercxtypes.h must be included before netadapteroffload.h
#endif

#if NETADAPTER_VERSION_MINOR >= 1

WDF_EXTERN_C_START

typedef enum _NET_ADAPTER_OFFLOAD_LAYER3_FLAGS
{
    NetAdapterOffloadLayer3FlagIPv4NoOptions = 0x00000001,
    NetAdapterOffloadLayer3FlagIPv4WithOptions = 0x00000002,
    NetAdapterOffloadLayer3FlagIPv6NoExtensions = 0x00000004,
    NetAdapterOffloadLayer3FlagIPv6WithExtensions = 0x00000008,
} NET_ADAPTER_OFFLOAD_LAYER3_FLAGS;

typedef enum _NET_ADAPTER_OFFLOAD_LAYER4_FLAGS
{
    NetAdapterOffloadLayer4FlagTcpNoOptions = 0x00000001,
    NetAdapterOffloadLayer4FlagTcpWithOptions = 0x00000002,
    NetAdapterOffloadLayer4FlagUdp = 0x00000004,
} NET_ADAPTER_OFFLOAD_LAYER4_FLAGS;

typedef enum _NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS
{
    NetAdapterOffloadIeee8021PriorityTaggingFlag = 0x00000001,
    NetAdapterOffloadIeee8021VlanTaggingFlag = 0x00000002,
} NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS;

#define NETCX_OFFLOAD_CALLBACK(Event)                       \
typedef                                                     \
_Function_class_(Event)                                     \
_IRQL_requires_same_                                        \
_IRQL_requires_max_(PASSIVE_LEVEL)                          \
VOID                                                        \
NTAPI                                                       \
Event(                                                      \
    _In_ NETADAPTER Adapter,                                \
    _In_ NETOFFLOAD Offload)

NETCX_OFFLOAD_CALLBACK(EVT_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM);
typedef EVT_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM *PFN_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM;
NETCX_OFFLOAD_CALLBACK(EVT_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM);
typedef EVT_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM *PFN_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM;
NETCX_OFFLOAD_CALLBACK(EVT_NET_ADAPTER_OFFLOAD_SET_GSO);
typedef EVT_NET_ADAPTER_OFFLOAD_SET_GSO *PFN_NET_ADAPTER_OFFLOAD_SET_GSO;
NETCX_OFFLOAD_CALLBACK(EVT_NET_ADAPTER_OFFLOAD_SET_RSC);
typedef EVT_NET_ADAPTER_OFFLOAD_SET_RSC *PFN_NET_ADAPTER_OFFLOAD_SET_RSC;

#undef NETCX_OFFLOAD_CALLBACK

typedef struct _NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES
{
    ULONG Size;
    NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags;
    NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags;
    UINT16 Layer3HeaderOffsetLimit;
    UINT16 Layer4HeaderOffsetLimit;
    PFN_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM EvtAdapterOffloadSetTxChecksum;
} NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES * Capabilities,
    _In_ NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM EvtAdapterOffloadSetTxChecksum)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->Layer3Flags = Layer3Flags;
    Capabilities->EvtAdapterOffloadSetTxChecksum = EvtAdapterOffloadSetTxChecksum;
}

typedef struct _NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES
{
    ULONG Size;
    PFN_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM EvtAdapterOffloadSetRxChecksum;
} NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES * Capabilities,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM EvtAdapterOffloadSetRxChecksum)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->EvtAdapterOffloadSetRxChecksum = EvtAdapterOffloadSetRxChecksum;
}

typedef struct _NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES
{
    ULONG Size;
    NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags;
    NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags;
    UINT16 Layer4HeaderOffsetLimit;
    SIZE_T MaximumOffloadSize;
    SIZE_T MinimumSegmentCount;
    PFN_NET_ADAPTER_OFFLOAD_SET_GSO EvtAdapterOffloadSetGso;
} NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES * GsoCapabilities,
    _In_ NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags,
    _In_ NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags,
    _In_ SIZE_T MaximumOffloadSize,
    _In_ SIZE_T MinimumSegmentCount,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_GSO EvtAdapterOffloadSetGso)
{
    RtlZeroMemory(GsoCapabilities, sizeof(*GsoCapabilities));
    GsoCapabilities->Size = sizeof(*GsoCapabilities);
    GsoCapabilities->Layer3Flags = Layer3Flags;
    GsoCapabilities->Layer4Flags = Layer4Flags;
    GsoCapabilities->MaximumOffloadSize = MaximumOffloadSize;
    GsoCapabilities->MinimumSegmentCount = MinimumSegmentCount;
    GsoCapabilities->EvtAdapterOffloadSetGso = EvtAdapterOffloadSetGso;
}

typedef struct _NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES
{
    ULONG Size;
    NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags;
    NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags;
    BOOLEAN TcpTimestampOption;
    PFN_NET_ADAPTER_OFFLOAD_SET_RSC EvtAdapterOffloadSetRsc;
} NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES * RscCapabilities,
    _In_ NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags,
    _In_ NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_RSC EvtAdapterOffloadSetRsc)
{
    RtlZeroMemory(RscCapabilities, sizeof(*RscCapabilities));
    RscCapabilities->Size = sizeof(*RscCapabilities);
    RscCapabilities->Layer3Flags = Layer3Flags;
    RscCapabilities->Layer4Flags = Layer4Flags;
    RscCapabilities->EvtAdapterOffloadSetRsc = EvtAdapterOffloadSetRsc;
}

typedef struct _NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES
{
    ULONG Size;
    NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS Flags;
} NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES;

FORCEINLINE
VOID
NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES * Ieee8021qCapabilities,
    _In_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS Flags)
{
    RtlZeroMemory(Ieee8021qCapabilities, sizeof(*Ieee8021qCapabilities));
    Ieee8021qCapabilities->Size = sizeof(*Ieee8021qCapabilities);
    Ieee8021qCapabilities->Flags = Flags;
}

#if NETADAPTER_VERSION_MINOR >= 1

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETTXCHECKSUMCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES const * HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterOffloadSetTxChecksumCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES const * HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETTXCHECKSUMCAPABILITIES)NetFunctions[NetAdapterOffloadSetTxChecksumCapabilitiesTableIndex])(NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTXCHECKSUMIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsTxChecksumIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTXCHECKSUMIPV4ENABLED)NetFunctions[NetOffloadIsTxChecksumIPv4EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTXCHECKSUMTCPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsTxChecksumTcpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTXCHECKSUMTCPENABLED)NetFunctions[NetOffloadIsTxChecksumTcpEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTXCHECKSUMUDPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsTxChecksumUdpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTXCHECKSUMUDPENABLED)NetFunctions[NetOffloadIsTxChecksumUdpEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETRXCHECKSUMCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES const * Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterOffloadSetRxChecksumCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES const * Capabilities)
{
    ((PFN_NETADAPTEROFFLOADSETRXCHECKSUMCAPABILITIES)NetFunctions[NetAdapterOffloadSetRxChecksumCapabilitiesTableIndex])(NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISRXCHECKSUMIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsRxChecksumIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISRXCHECKSUMIPV4ENABLED)NetFunctions[NetOffloadIsRxChecksumIPv4EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISRXCHECKSUMTCPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsRxChecksumTcpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISRXCHECKSUMTCPENABLED)NetFunctions[NetOffloadIsRxChecksumTcpEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISRXCHECKSUMUDPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsRxChecksumUdpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISRXCHECKSUMUDPENABLED)NetFunctions[NetOffloadIsRxChecksumUdpEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETGSOCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES * HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterOffloadSetGsoCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES * HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETGSOCAPABILITIES)NetFunctions[NetAdapterOffloadSetGsoCapabilitiesTableIndex])(NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISLSOIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsLsoIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISLSOIPV4ENABLED)NetFunctions[NetOffloadIsLsoIPv4EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISLSOIPV6ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsLsoIPv6Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISLSOIPV6ENABLED)NetFunctions[NetOffloadIsLsoIPv6EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISUSOIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsUsoIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISUSOIPV4ENABLED)NetFunctions[NetOffloadIsUsoIPv4EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISUSOIPV6ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsUsoIPv6Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISUSOIPV6ENABLED)NetFunctions[NetOffloadIsUsoIPv6EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETRSCCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES * HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterOffloadSetRscCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES * HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETRSCCAPABILITIES)NetFunctions[NetAdapterOffloadSetRscCapabilitiesTableIndex])(NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTCPRSCIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsTcpRscIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTCPRSCIPV4ENABLED)NetFunctions[NetOffloadIsTcpRscIPv4EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTCPRSCIPV6ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsTcpRscIPv6Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTCPRSCIPV6ENABLED)NetFunctions[NetOffloadIsTcpRscIPv6EnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISRSCTCPTIMESTAMPOPTIONENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NetOffloadIsRscTcpTimestampOptionEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISRSCTCPTIMESTAMPOPTIONENABLED)NetFunctions[NetOffloadIsRscTcpTimestampOptionEnabledTableIndex])(NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETIEEE8021QTAGCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES * HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetAdapterOffloadSetIeee8021qTagCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES * HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETIEEE8021QTAGCAPABILITIES)NetFunctions[NetAdapterOffloadSetIeee8021qTagCapabilitiesTableIndex])(NetDriverGlobals, Adapter, HardwareCapabilities);
}

#endif /* 2.1 */

WDF_EXTERN_C_END

#endif /* 2.1 */
