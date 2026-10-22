/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx device initialization
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifndef _NETADAPTERCXTYPES_H_
#error netadaptercxtypes.h must be included before netdevice.h
#endif

WDF_EXTERN_C_START

typedef
_Function_class_(EVT_NET_DEVICE_PREVIEW_WAKE_SOURCE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_NET_DEVICE_PREVIEW_WAKE_SOURCE(
    _In_ WDFDEVICE Device,
    _In_ NETWAKESOURCE WakeSource);
typedef EVT_NET_DEVICE_PREVIEW_WAKE_SOURCE *PFN_NET_DEVICE_PREVIEW_WAKE_SOURCE;

typedef
_Function_class_(EVT_NET_DEVICE_PREVIEW_POWER_OFFLOAD)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_NET_DEVICE_PREVIEW_POWER_OFFLOAD(
    _In_ WDFDEVICE Device,
    _In_ NETPOWEROFFLOAD PowerOffload);
typedef EVT_NET_DEVICE_PREVIEW_POWER_OFFLOAD *PFN_NET_DEVICE_PREVIEW_POWER_OFFLOAD;

typedef struct _NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS
{
    ULONG Size;
    PFN_NET_DEVICE_PREVIEW_WAKE_SOURCE EvtDevicePreviewBitmapPattern;
    PFN_NET_DEVICE_PREVIEW_POWER_OFFLOAD EvtDevicePreviewArpOffload;
    PFN_NET_DEVICE_PREVIEW_POWER_OFFLOAD EvtDevicePreviewNSOffload;
} NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS;

FORCEINLINE
VOID
NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS_INIT(
    _Out_ NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS * Callbacks)
{
    RtlZeroMemory(Callbacks, sizeof(*Callbacks));
    Callbacks->Size = sizeof(*Callbacks);
}

#if NETADAPTER_VERSION_MINOR >= 1
typedef
_Function_class_(EVT_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS(
    _In_ WDFDEVICE Device);
typedef EVT_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS *PFN_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS;

typedef struct _NET_DEVICE_RESET_CAPABILITIES
{
    ULONG Size;
    GUID ResetDiagnosticsGuid;
    PFN_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS EvtNetDeviceCollectResetDiagnostics;
} NET_DEVICE_RESET_CAPABILITIES;

FORCEINLINE
VOID
NET_DEVICE_RESET_CAPABILITIES_INIT(
    _Out_ NET_DEVICE_RESET_CAPABILITIES * ResetCapabilities,
    _In_ GUID ResetDiagnosticsGuid,
    _In_ PFN_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS EvtNetDeviceCollectResetDiagnostics)
{
    RtlZeroMemory(ResetCapabilities, sizeof(*ResetCapabilities));
    ResetCapabilities->Size = sizeof(*ResetCapabilities);
    ResetCapabilities->ResetDiagnosticsGuid = ResetDiagnosticsGuid;
    ResetCapabilities->EvtNetDeviceCollectResetDiagnostics = EvtNetDeviceCollectResetDiagnostics;
}
#endif /* 2.1 */

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETDEVICEINITCONFIG)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NetDeviceInitConfig(
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    return ((PFN_NETDEVICEINITCONFIG)NetFunctions[NetDeviceInitConfigTableIndex])(NetDriverGlobals, DeviceInit);
}

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETDEVICEOPENCONFIGURATION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * ConfigurationAttributes,
    _Out_ NETCONFIGURATION * Configuration);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NetDeviceOpenConfiguration(
    _In_ WDFDEVICE Device,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * ConfigurationAttributes,
    _Out_ NETCONFIGURATION * Configuration)
{
    return ((PFN_NETDEVICEOPENCONFIGURATION)NetFunctions[NetDeviceOpenConfigurationTableIndex])(NetDriverGlobals, Device, ConfigurationAttributes, Configuration);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETDEVICEINITSETPOWERPOLICYEVENTCALLBACKS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _In_ NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS const * Callbacks);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetDeviceInitSetPowerPolicyEventCallbacks(
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _In_ NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS const * Callbacks)
{
    ((PFN_NETDEVICEINITSETPOWERPOLICYEVENTCALLBACKS)NetFunctions[NetDeviceInitSetPowerPolicyEventCallbacksTableIndex])(NetDriverGlobals, DeviceInit, Callbacks);
}

#if NETADAPTER_VERSION_MINOR >= 1

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETDEVICEINITSETRESETCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _In_ NET_DEVICE_RESET_CAPABILITIES const * ResetCapabilities);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NetDeviceInitSetResetCapabilities(
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _In_ NET_DEVICE_RESET_CAPABILITIES const * ResetCapabilities)
{
    ((PFN_NETDEVICEINITSETRESETCAPABILITIES)NetFunctions[NetDeviceInitSetResetCapabilitiesTableIndex])(NetDriverGlobals, DeviceInit, ResetCapabilities);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETDEVICESTORERESETDIAGNOSTICS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ SIZE_T ResetDiagnosticsSize,
    _In_reads_bytes_(ResetDiagnosticsSize) UINT8 const * ResetDiagnosticsBuffer);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NetDeviceStoreResetDiagnostics(
    _In_ WDFDEVICE Device,
    _In_ SIZE_T ResetDiagnosticsSize,
    _In_reads_bytes_(ResetDiagnosticsSize) UINT8 const * ResetDiagnosticsBuffer)
{
    ((PFN_NETDEVICESTORERESETDIAGNOSTICS)NetFunctions[NetDeviceStoreResetDiagnosticsTableIndex])(NetDriverGlobals, Device, ResetDiagnosticsSize, ResetDiagnosticsBuffer);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETDEVICEREQUESTRESET)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NetDeviceRequestReset(
    _In_ WDFDEVICE Device)
{
    ((PFN_NETDEVICEREQUESTRESET)NetFunctions[NetDeviceRequestResetTableIndex])(NetDriverGlobals, Device);
}

#endif /* 2.1 */

WDF_EXTERN_C_END
