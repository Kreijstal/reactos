/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client entry points that are not implemented yet
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Every slot of the client function table points somewhere. These entries
 * log the call and return a value that tells the client the feature is
 * absent: STATUS_NOT_IMPLEMENTED, FALSE, 0 or NULL. Setters return nothing.
 *
 * Parameter lists follow the NETEXPORT definitions of the MIT-licensed
 * NetAdapterCx sources (release_21H2). Arguments are passed through as
 * pointer-sized or 32-bit values only, so the generic types below keep the
 * calling convention of the real prototypes on every architecture.
 */

#include "nxprivate.h"
#include "nxapi.h"

static
VOID
NxStubCalled(
    _In_opt_ PVOID DriverGlobals,
    _In_ PCSTR Name)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    DbgPrint("NetAdapterCx: %s is not implemented\n", Name);
}

static
VOID
NTAPI
stub_NetAdapterPowerOffloadSetArpCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterPowerOffloadSetArpCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
VOID
NTAPI
stub_NetAdapterPowerOffloadSetNSCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterPowerOffloadSetNSCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
VOID
NTAPI
stub_NetAdapterWakeSetBitmapCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterWakeSetBitmapCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
VOID
NTAPI
stub_NetAdapterWakeSetMediaChangeCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterWakeSetMediaChangeCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
VOID
NTAPI
stub_NetAdapterWakeSetMagicPacketCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterWakeSetMagicPacketCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
VOID
NTAPI
stub_NetAdapterWakeSetPacketFilterCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterWakeSetPacketFilterCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
NTSTATUS
NTAPI
stub_NetAdapterOpenConfiguration(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID ConfigurationAttributes,
    _In_opt_ PVOID Configuration)
{
    NxStubCalled(Globals, "NetAdapterOpenConfiguration");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(ConfigurationAttributes);
    UNREFERENCED_PARAMETER(Configuration);
    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
stub_NetAdapterReportWakeReasonPacket(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Reason)
{
    NxStubCalled(DriverGlobals, "NetAdapterReportWakeReasonPacket");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Reason);
}

static
VOID
NTAPI
stub_NetAdapterReportWakeReasonMediaChange(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ NET_IF_MEDIA_CONNECT_STATE Reason)
{
    NxStubCalled(DriverGlobals, "NetAdapterReportWakeReasonMediaChange");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Reason);
}

static
PVOID
NTAPI
stub_NetAdapterExtensionInitAllocate(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterInit)
{
    NxStubCalled(DriverGlobals, "NetAdapterExtensionInitAllocate");
    UNREFERENCED_PARAMETER(AdapterInit);
    return NULL;
}

static
VOID
NTAPI
stub_NetAdapterExtensionInitSetOidRequestPreprocessCallback(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterExtensionInit,
    _In_opt_ PVOID PreprocessOidRequest)
{
    NxStubCalled(DriverGlobals, "NetAdapterExtensionInitSetOidRequestPreprocessCallback");
    UNREFERENCED_PARAMETER(AdapterExtensionInit);
    UNREFERENCED_PARAMETER(PreprocessOidRequest);
}

static
VOID
NTAPI
stub_NetAdapterDispatchPreprocessedOidRequest(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Request,
    _In_opt_ PVOID Context)
{
    NxStubCalled(DriverGlobals, "NetAdapterDispatchPreprocessedOidRequest");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Context);
}

static
VOID
NTAPI
stub_NetAdapterExtensionInitSetNdisPmCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterExtensionInit,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterExtensionInitSetNdisPmCapabilities");
    UNREFERENCED_PARAMETER(AdapterExtensionInit);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
VOID
NTAPI
stub_NetConfigurationClose(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration)
{
    NxStubCalled(Globals, "NetConfigurationClose");
    UNREFERENCED_PARAMETER(Configuration);
}

static
NTSTATUS
NTAPI
stub_NetConfigurationOpenSubConfiguration(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID SubConfigurationName,
    _In_opt_ PVOID SubConfigurationAttributes,
    _In_opt_ PVOID SubConfiguration)
{
    NxStubCalled(Globals, "NetConfigurationOpenSubConfiguration");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(SubConfigurationName);
    UNREFERENCED_PARAMETER(SubConfigurationAttributes);
    UNREFERENCED_PARAMETER(SubConfiguration);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationQueryUlong(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ ULONG Flags,
    _In_opt_ PVOID ValueName,
    _In_opt_ PVOID Value)
{
    NxStubCalled(Globals, "NetConfigurationQueryUlong");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(Value);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationQueryString(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID ValueName,
    _In_opt_ PVOID StringAttributes,
    _In_opt_ PVOID WdfString)
{
    NxStubCalled(Globals, "NetConfigurationQueryString");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(StringAttributes);
    UNREFERENCED_PARAMETER(WdfString);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationQueryMultiString(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID ValueName,
    _In_opt_ PVOID StringsAttributes,
    _In_opt_ PVOID Collection)
{
    NxStubCalled(Globals, "NetConfigurationQueryMultiString");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(StringsAttributes);
    UNREFERENCED_PARAMETER(Collection);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationQueryBinary(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID ValueName,
    _In_opt_ POOL_TYPE PoolType,
    _In_opt_ PVOID MemoryAttributes,
    _In_opt_ PVOID WdfMemory)
{
    NxStubCalled(Globals, "NetConfigurationQueryBinary");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(PoolType);
    UNREFERENCED_PARAMETER(MemoryAttributes);
    UNREFERENCED_PARAMETER(WdfMemory);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationQueryLinkLayerAddress(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID LinkLayerAddress)
{
    NxStubCalled(DriverGlobals, "NetConfigurationQueryLinkLayerAddress");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(LinkLayerAddress);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationAssignUlong(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID ValueName,
    _In_opt_ ULONG Value)
{
    NxStubCalled(Globals, "NetConfigurationAssignUlong");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(Value);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationAssignUnicodeString(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID ValueName,
    _In_opt_ PVOID Value)
{
    NxStubCalled(Globals, "NetConfigurationAssignUnicodeString");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(Value);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationAssignMultiString(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID ValueName,
    _In_opt_ PVOID StringsCollection)
{
    NxStubCalled(Globals, "NetConfigurationAssignMultiString");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(StringsCollection);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetConfigurationAssignBinary(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Configuration,
    _In_opt_ PVOID ValueName,
    _In_opt_ PVOID Buffer,
    _In_opt_ ULONG BufferLength)
{
    NxStubCalled(Globals, "NetConfigurationAssignBinary");
    UNREFERENCED_PARAMETER(Configuration);
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BufferLength);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetDeviceOpenConfiguration(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Device,
    _In_opt_ PVOID ConfigurationAttributes,
    _In_opt_ PVOID Configuration)
{
    NxStubCalled(Globals, "NetDeviceOpenConfiguration");
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ConfigurationAttributes);
    UNREFERENCED_PARAMETER(Configuration);
    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
stub_NetDeviceInitSetPowerPolicyEventCallbacks(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID DeviceInit,
    _In_opt_ PVOID Callbacks)
{
    NxStubCalled(DriverGlobals, "NetDeviceInitSetPowerPolicyEventCallbacks");
    UNREFERENCED_PARAMETER(DeviceInit);
    UNREFERENCED_PARAMETER(Callbacks);
}

static
VOID
NTAPI
stub_NetDeviceInitSetResetConfig(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID DeviceInit,
    _In_opt_ PVOID ResetConfig)
{
    NxStubCalled(DriverGlobals, "NetDeviceInitSetResetConfig");
    UNREFERENCED_PARAMETER(DeviceInit);
    UNREFERENCED_PARAMETER(ResetConfig);
}

static
NTSTATUS
NTAPI
stub_NetDeviceAssignSupportedOidList(
    _In_opt_ PVOID Globals,
    _In_opt_ PVOID Device,
    _In_opt_ PVOID SupportedOids,
    _In_opt_ SIZE_T SupportedOidsCount)
{
    NxStubCalled(Globals, "NetDeviceAssignSupportedOidList");
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(SupportedOids);
    UNREFERENCED_PARAMETER(SupportedOidsCount);
    return STATUS_NOT_IMPLEMENTED;
}

static
ULONG
NTAPI
stub_NetPowerOffloadGetType(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID PowerOffload)
{
    NxStubCalled(DriverGlobals, "NetPowerOffloadGetType");
    UNREFERENCED_PARAMETER(PowerOffload);
    return 0;
}

static
VOID
NTAPI
stub_NetPowerOffloadGetArpParameters(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID PowerOffload,
    _In_opt_ PVOID Parameters)
{
    NxStubCalled(DriverGlobals, "NetPowerOffloadGetArpParameters");
    UNREFERENCED_PARAMETER(PowerOffload);
    UNREFERENCED_PARAMETER(Parameters);
}

static
VOID
NTAPI
stub_NetPowerOffloadGetNSParameters(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID PowerOffload,
    _In_opt_ PVOID Parameters)
{
    NxStubCalled(DriverGlobals, "NetPowerOffloadGetNSParameters");
    UNREFERENCED_PARAMETER(PowerOffload);
    UNREFERENCED_PARAMETER(Parameters);
}

static
VOID
NTAPI
stub_NetDeviceGetPowerOffloadList(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Device,
    _In_opt_ PVOID List)
{
    NxStubCalled(DriverGlobals, "NetDeviceGetPowerOffloadList");
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(List);
}

static
SIZE_T
NTAPI
stub_NetPowerOffloadListGetCount(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID List)
{
    NxStubCalled(DriverGlobals, "NetPowerOffloadListGetCount");
    UNREFERENCED_PARAMETER(List);
    return 0;
}

static
PVOID
NTAPI
stub_NetPowerOffloadListGetElement(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID List,
    _In_opt_ SIZE_T Index)
{
    NxStubCalled(DriverGlobals, "NetPowerOffloadListGetElement");
    UNREFERENCED_PARAMETER(List);
    UNREFERENCED_PARAMETER(Index);
    return NULL;
}

static
VOID
NTAPI
stub_NetAdapterSetReceiveScalingCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterSetReceiveScalingCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
ULONG
NTAPI
stub_NetWakeSourceGetType(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID WakeSource)
{
    NxStubCalled(DriverGlobals, "NetWakeSourceGetType");
    UNREFERENCED_PARAMETER(WakeSource);
    return 0;
}

static
PVOID
NTAPI
stub_NetWakeSourceGetAdapter(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID WakeSource)
{
    NxStubCalled(DriverGlobals, "NetWakeSourceGetAdapter");
    UNREFERENCED_PARAMETER(WakeSource);
    return NULL;
}

static
VOID
NTAPI
stub_NetWakeSourceGetBitmapParameters(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID WakeSource,
    _In_opt_ PVOID Parameters)
{
    NxStubCalled(DriverGlobals, "NetWakeSourceGetBitmapParameters");
    UNREFERENCED_PARAMETER(WakeSource);
    UNREFERENCED_PARAMETER(Parameters);
}

static
VOID
NTAPI
stub_NetWakeSourceGetMediaChangeParameters(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID WakeSource,
    _In_opt_ PVOID Parameters)
{
    NxStubCalled(DriverGlobals, "NetWakeSourceGetMediaChangeParameters");
    UNREFERENCED_PARAMETER(WakeSource);
    UNREFERENCED_PARAMETER(Parameters);
}

static
VOID
NTAPI
stub_NetDeviceGetWakeSourceList(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Device,
    _In_opt_ PVOID List)
{
    NxStubCalled(DriverGlobals, "NetDeviceGetWakeSourceList");
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(List);
}

static
SIZE_T
NTAPI
stub_NetWakeSourceListGetCount(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID List)
{
    NxStubCalled(DriverGlobals, "NetWakeSourceListGetCount");
    UNREFERENCED_PARAMETER(List);
    return 0;
}

static
PVOID
NTAPI
stub_NetWakeSourceListGetElement(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID List,
    _In_opt_ SIZE_T Index)
{
    NxStubCalled(DriverGlobals, "NetWakeSourceListGetElement");
    UNREFERENCED_PARAMETER(List);
    UNREFERENCED_PARAMETER(Index);
    return NULL;
}

static
VOID
NTAPI
stub_NetAdapterWakeSetEapolPacketCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterWakeSetEapolPacketCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
VOID
NTAPI
stub_NetAdapterSetReceiveFilterCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID ReceiveFilter)
{
    NxStubCalled(DriverGlobals, "NetAdapterSetReceiveFilterCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(ReceiveFilter);
}

static
ULONG
NTAPI
stub_NetReceiveFilterGetPacketFilter(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID ReceiveFilter)
{
    NxStubCalled(DriverGlobals, "NetReceiveFilterGetPacketFilter");
    UNREFERENCED_PARAMETER(ReceiveFilter);
    return 0;
}

static
SIZE_T
NTAPI
stub_NetReceiveFilterGetMulticastAddressCount(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID ReceiveFilter)
{
    NxStubCalled(DriverGlobals, "NetReceiveFilterGetMulticastAddressCount");
    UNREFERENCED_PARAMETER(ReceiveFilter);
    return 0;
}

static
PVOID
NTAPI
stub_NetReceiveFilterGetMulticastAddressList(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID ReceiveFilter)
{
    NxStubCalled(DriverGlobals, "NetReceiveFilterGetMulticastAddressList");
    UNREFERENCED_PARAMETER(ReceiveFilter);
    return NULL;
}

static
NTSTATUS
NTAPI
stub_NetDriverExtensionInitialize(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Driver,
    _In_opt_ PVOID Config)
{
    NxStubCalled(DriverGlobals, "NetDriverExtensionInitialize");
    UNREFERENCED_PARAMETER(Driver);
    UNREFERENCED_PARAMETER(Config);
    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
stub_NetAdapterExtensionInitSetDirectOidRequestPreprocessCallback(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterExtensionInit,
    _In_opt_ PVOID PreprocessDirectOidRequest)
{
    NxStubCalled(DriverGlobals, "NetAdapterExtensionInitSetDirectOidRequestPreprocessCallback");
    UNREFERENCED_PARAMETER(AdapterExtensionInit);
    UNREFERENCED_PARAMETER(PreprocessDirectOidRequest);
}

static
VOID
NTAPI
stub_NetExAdapterInitSetDirectOidPreprocessCallback(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterExtensionInit,
    _In_opt_ PVOID PreprocessDirectOid)
{
    NxStubCalled(DriverGlobals, "NetExAdapterInitSetDirectOidPreprocessCallback");
    UNREFERENCED_PARAMETER(AdapterExtensionInit);
    UNREFERENCED_PARAMETER(PreprocessDirectOid);
}

static
VOID
NTAPI
stub_NetAdapterExtensionInitSetTxPeerDemuxCallback(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterExtensionInit,
    _In_opt_ PVOID TxPeerDemux)
{
    NxStubCalled(DriverGlobals, "NetAdapterExtensionInitSetTxPeerDemuxCallback");
    UNREFERENCED_PARAMETER(AdapterExtensionInit);
    UNREFERENCED_PARAMETER(TxPeerDemux);
}

static
VOID
NTAPI
stub_NetAdapterDispatchPreprocessedDirectOidRequest(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Request,
    _In_opt_ PVOID Context)
{
    NxStubCalled(DriverGlobals, "NetAdapterDispatchPreprocessedDirectOidRequest");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Context);
}

static
NTSTATUS
NTAPI
stub_NetExAdapterDispatchPreprocessedDirectOid(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Request,
    _In_opt_ PVOID Context)
{
    NxStubCalled(DriverGlobals, "NetExAdapterDispatchPreprocessedDirectOid");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Context);
    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
stub_NetAdapterExtensionSetNdisPmCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterExtensionSetNdisPmCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
NTSTATUS
NTAPI
stub_NetAdapterInitAllocateContext(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterInit,
    _In_opt_ PVOID Attributes,
    _In_opt_ PVOID Context)
{
    NxStubCalled(DriverGlobals, "NetAdapterInitAllocateContext");
    UNREFERENCED_PARAMETER(AdapterInit);
    UNREFERENCED_PARAMETER(Attributes);
    UNREFERENCED_PARAMETER(Context);
    return STATUS_NOT_IMPLEMENTED;
}

static
PVOID
NTAPI
stub_NetAdapterInitGetTypedContextWorker(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterInit,
    _In_opt_ PVOID TypeInfo)
{
    NxStubCalled(DriverGlobals, "NetAdapterInitGetTypedContextWorker");
    UNREFERENCED_PARAMETER(AdapterInit);
    UNREFERENCED_PARAMETER(TypeInfo);
    return NULL;
}

static
VOID
NTAPI
stub_NetAdapterOffloadSetGsoCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID HardwareCapabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterOffloadSetGsoCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(HardwareCapabilities);
}

static
BOOLEAN
NTAPI
stub_NetOffloadIsLsoIPv4Enabled(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Offload)
{
    NxStubCalled(DriverGlobals, "NetOffloadIsLsoIPv4Enabled");
    UNREFERENCED_PARAMETER(Offload);
    return FALSE;
}

static
BOOLEAN
NTAPI
stub_NetOffloadIsLsoIPv6Enabled(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Offload)
{
    NxStubCalled(DriverGlobals, "NetOffloadIsLsoIPv6Enabled");
    UNREFERENCED_PARAMETER(Offload);
    return FALSE;
}

static
BOOLEAN
NTAPI
stub_NetOffloadIsUsoIPv4Enabled(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Offload)
{
    NxStubCalled(DriverGlobals, "NetOffloadIsUsoIPv4Enabled");
    UNREFERENCED_PARAMETER(Offload);
    return FALSE;
}

static
BOOLEAN
NTAPI
stub_NetOffloadIsUsoIPv6Enabled(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Offload)
{
    NxStubCalled(DriverGlobals, "NetOffloadIsUsoIPv6Enabled");
    UNREFERENCED_PARAMETER(Offload);
    return FALSE;
}

static
VOID
NTAPI
stub_NetAdapterOffloadSetRscCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID HardwareCapabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterOffloadSetRscCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(HardwareCapabilities);
}

static
BOOLEAN
NTAPI
stub_NetOffloadIsTcpRscIPv4Enabled(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Offload)
{
    NxStubCalled(DriverGlobals, "NetOffloadIsTcpRscIPv4Enabled");
    UNREFERENCED_PARAMETER(Offload);
    return FALSE;
}

static
BOOLEAN
NTAPI
stub_NetOffloadIsTcpRscIPv6Enabled(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Offload)
{
    NxStubCalled(DriverGlobals, "NetOffloadIsTcpRscIPv6Enabled");
    UNREFERENCED_PARAMETER(Offload);
    return FALSE;
}

static
BOOLEAN
NTAPI
stub_NetOffloadIsRscTcpTimestampOptionEnabled(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Offload)
{
    NxStubCalled(DriverGlobals, "NetOffloadIsRscTcpTimestampOptionEnabled");
    UNREFERENCED_PARAMETER(Offload);
    return FALSE;
}

static
VOID
NTAPI
stub_NetAdapterOffloadSetIeee8021qTagCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter,
    _In_opt_ PVOID HardwareCapabilities)
{
    NxStubCalled(DriverGlobals, "NetAdapterOffloadSetIeee8021qTagCapabilities");
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(HardwareCapabilities);
}

static
VOID
NTAPI
stub_NetAdapterInitAddTxDemux(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID AdapterInit,
    _In_opt_ PVOID Demux)
{
    NxStubCalled(DriverGlobals, "NetAdapterInitAddTxDemux");
    UNREFERENCED_PARAMETER(AdapterInit);
    UNREFERENCED_PARAMETER(Demux);
}

static
PVOID
NTAPI
stub_NetAdapterGetTxPeerAddressDemux(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Adapter)
{
    NxStubCalled(DriverGlobals, "NetAdapterGetTxPeerAddressDemux");
    UNREFERENCED_PARAMETER(Adapter);
    return NULL;
}

static
NTSTATUS
NTAPI
stub_NetBufferQueueCreate(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID ExecutionContext,
    _In_opt_ PVOID Config,
    _In_opt_ PVOID Attributes,
    _In_opt_ PVOID BufferQueue)
{
    NxStubCalled(DriverGlobals, "NetBufferQueueCreate");
    UNREFERENCED_PARAMETER(ExecutionContext);
    UNREFERENCED_PARAMETER(Config);
    UNREFERENCED_PARAMETER(Attributes);
    UNREFERENCED_PARAMETER(BufferQueue);
    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
stub_NetBufferQueueGetExtension(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID BufferQueue,
    _In_opt_ PVOID Query,
    _In_opt_ PVOID Extension)
{
    NxStubCalled(DriverGlobals, "NetBufferQueueGetExtension");
    UNREFERENCED_PARAMETER(BufferQueue);
    UNREFERENCED_PARAMETER(Query);
    UNREFERENCED_PARAMETER(Extension);
}

static
PVOID
NTAPI
stub_NetBufferQueueGetRing(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID BufferQueue)
{
    NxStubCalled(DriverGlobals, "NetBufferQueueGetRing");
    UNREFERENCED_PARAMETER(BufferQueue);
    return NULL;
}

static
VOID
NTAPI
stub_NetDeviceInitSetResetCapabilities(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID DeviceInit,
    _In_opt_ PVOID Capabilities)
{
    NxStubCalled(DriverGlobals, "NetDeviceInitSetResetCapabilities");
    UNREFERENCED_PARAMETER(DeviceInit);
    UNREFERENCED_PARAMETER(Capabilities);
}

static
VOID
NTAPI
stub_NetDeviceStoreResetDiagnostics(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Device,
    _In_opt_ SIZE_T ResetDiagnosticsSize,
    _In_opt_ PVOID ResetDiagnosticsBuffer)
{
    NxStubCalled(DriverGlobals, "NetDeviceStoreResetDiagnostics");
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResetDiagnosticsSize);
    UNREFERENCED_PARAMETER(ResetDiagnosticsBuffer);
}

static
VOID
NTAPI
stub_NetDeviceRequestReset(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Device)
{
    NxStubCalled(DriverGlobals, "NetDeviceRequestReset");
    UNREFERENCED_PARAMETER(Device);
}

static
NTSTATUS
NTAPI
stub_NetExecutionContextCreate(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID Device,
    _In_opt_ PVOID Config,
    _In_opt_ PVOID ClientAttributes,
    _In_opt_ PVOID ExecutionContext)
{
    NxStubCalled(DriverGlobals, "NetExecutionContextCreate");
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Config);
    UNREFERENCED_PARAMETER(ClientAttributes);
    UNREFERENCED_PARAMETER(ExecutionContext);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
stub_NetExecutionContextTaskCreate(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID NetExecutionContextHandle,
    _In_opt_ PVOID ClientAttributes,
    _In_opt_ PVOID NetExecutionContextTaskConfig,
    _In_opt_ PVOID NetExecutionContextTaskHandle)
{
    NxStubCalled(DriverGlobals, "NetExecutionContextTaskCreate");
    UNREFERENCED_PARAMETER(NetExecutionContextHandle);
    UNREFERENCED_PARAMETER(ClientAttributes);
    UNREFERENCED_PARAMETER(NetExecutionContextTaskConfig);
    UNREFERENCED_PARAMETER(NetExecutionContextTaskHandle);
    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
stub_NetExecutionContextNotify(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID ExecutionContext)
{
    NxStubCalled(DriverGlobals, "NetExecutionContextNotify");
    UNREFERENCED_PARAMETER(ExecutionContext);
}

static
VOID
NTAPI
stub_NetExecutionContextTaskEnqueue(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID NetExecutionContextTaskHandle)
{
    NxStubCalled(DriverGlobals, "NetExecutionContextTaskEnqueue");
    UNREFERENCED_PARAMETER(NetExecutionContextTaskHandle);
}

static
VOID
NTAPI
stub_NetExecutionContextTaskWaitCompletion(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID NetExecutionContextTaskHandle)
{
    NxStubCalled(DriverGlobals, "NetExecutionContextTaskWaitCompletion");
    UNREFERENCED_PARAMETER(NetExecutionContextTaskHandle);
}

static
PVOID
NTAPI
stub_NetPowerOffloadGetAdapter(
    _In_opt_ PVOID DriverGlobals,
    _In_opt_ PVOID PowerOffload)
{
    NxStubCalled(DriverGlobals, "NetPowerOffloadGetAdapter");
    UNREFERENCED_PARAMETER(PowerOffload);
    return NULL;
}

static NETFUNC NxStubTable[NetFunctionTableNumEntries] =
{
    [NetAdapterPowerOffloadSetArpCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterPowerOffloadSetArpCapabilities,
    [NetAdapterPowerOffloadSetNSCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterPowerOffloadSetNSCapabilities,
    [NetAdapterWakeSetBitmapCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterWakeSetBitmapCapabilities,
    [NetAdapterWakeSetMediaChangeCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterWakeSetMediaChangeCapabilities,
    [NetAdapterWakeSetMagicPacketCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterWakeSetMagicPacketCapabilities,
    [NetAdapterWakeSetPacketFilterCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterWakeSetPacketFilterCapabilities,
    [NetAdapterOpenConfigurationTableIndex] = (NETFUNC)stub_NetAdapterOpenConfiguration,
    [NetAdapterReportWakeReasonPacketTableIndex] = (NETFUNC)stub_NetAdapterReportWakeReasonPacket,
    [NetAdapterReportWakeReasonMediaChangeTableIndex] = (NETFUNC)stub_NetAdapterReportWakeReasonMediaChange,
    [NetAdapterExtensionInitAllocateTableIndex] = (NETFUNC)stub_NetAdapterExtensionInitAllocate,
    [NetAdapterExtensionInitSetOidRequestPreprocessCallbackTableIndex] = (NETFUNC)stub_NetAdapterExtensionInitSetOidRequestPreprocessCallback,
    [NetAdapterDispatchPreprocessedOidRequestTableIndex] = (NETFUNC)stub_NetAdapterDispatchPreprocessedOidRequest,
    [NetAdapterExtensionInitSetNdisPmCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterExtensionInitSetNdisPmCapabilities,
    [NetConfigurationCloseTableIndex] = (NETFUNC)stub_NetConfigurationClose,
    [NetConfigurationOpenSubConfigurationTableIndex] = (NETFUNC)stub_NetConfigurationOpenSubConfiguration,
    [NetConfigurationQueryUlongTableIndex] = (NETFUNC)stub_NetConfigurationQueryUlong,
    [NetConfigurationQueryStringTableIndex] = (NETFUNC)stub_NetConfigurationQueryString,
    [NetConfigurationQueryMultiStringTableIndex] = (NETFUNC)stub_NetConfigurationQueryMultiString,
    [NetConfigurationQueryBinaryTableIndex] = (NETFUNC)stub_NetConfigurationQueryBinary,
    [NetConfigurationQueryLinkLayerAddressTableIndex] = (NETFUNC)stub_NetConfigurationQueryLinkLayerAddress,
    [NetConfigurationAssignUlongTableIndex] = (NETFUNC)stub_NetConfigurationAssignUlong,
    [NetConfigurationAssignUnicodeStringTableIndex] = (NETFUNC)stub_NetConfigurationAssignUnicodeString,
    [NetConfigurationAssignMultiStringTableIndex] = (NETFUNC)stub_NetConfigurationAssignMultiString,
    [NetConfigurationAssignBinaryTableIndex] = (NETFUNC)stub_NetConfigurationAssignBinary,
    [NetDeviceOpenConfigurationTableIndex] = (NETFUNC)stub_NetDeviceOpenConfiguration,
    [NetDeviceInitSetPowerPolicyEventCallbacksTableIndex] = (NETFUNC)stub_NetDeviceInitSetPowerPolicyEventCallbacks,
    [NetDeviceInitSetResetConfigTableIndex] = (NETFUNC)stub_NetDeviceInitSetResetConfig,
    [NetDeviceAssignSupportedOidListTableIndex] = (NETFUNC)stub_NetDeviceAssignSupportedOidList,
    [NetPowerOffloadGetTypeTableIndex] = (NETFUNC)stub_NetPowerOffloadGetType,
    [NetPowerOffloadGetArpParametersTableIndex] = (NETFUNC)stub_NetPowerOffloadGetArpParameters,
    [NetPowerOffloadGetNSParametersTableIndex] = (NETFUNC)stub_NetPowerOffloadGetNSParameters,
    [NetDeviceGetPowerOffloadListTableIndex] = (NETFUNC)stub_NetDeviceGetPowerOffloadList,
    [NetPowerOffloadListGetCountTableIndex] = (NETFUNC)stub_NetPowerOffloadListGetCount,
    [NetPowerOffloadListGetElementTableIndex] = (NETFUNC)stub_NetPowerOffloadListGetElement,
    [NetAdapterSetReceiveScalingCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterSetReceiveScalingCapabilities,
    [NetWakeSourceGetTypeTableIndex] = (NETFUNC)stub_NetWakeSourceGetType,
    [NetWakeSourceGetAdapterTableIndex] = (NETFUNC)stub_NetWakeSourceGetAdapter,
    [NetWakeSourceGetBitmapParametersTableIndex] = (NETFUNC)stub_NetWakeSourceGetBitmapParameters,
    [NetWakeSourceGetMediaChangeParametersTableIndex] = (NETFUNC)stub_NetWakeSourceGetMediaChangeParameters,
    [NetDeviceGetWakeSourceListTableIndex] = (NETFUNC)stub_NetDeviceGetWakeSourceList,
    [NetWakeSourceListGetCountTableIndex] = (NETFUNC)stub_NetWakeSourceListGetCount,
    [NetWakeSourceListGetElementTableIndex] = (NETFUNC)stub_NetWakeSourceListGetElement,
    [NetAdapterWakeSetEapolPacketCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterWakeSetEapolPacketCapabilities,
    [NetAdapterSetReceiveFilterCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterSetReceiveFilterCapabilities,
    [NetReceiveFilterGetPacketFilterTableIndex] = (NETFUNC)stub_NetReceiveFilterGetPacketFilter,
    [NetReceiveFilterGetMulticastAddressCountTableIndex] = (NETFUNC)stub_NetReceiveFilterGetMulticastAddressCount,
    [NetReceiveFilterGetMulticastAddressListTableIndex] = (NETFUNC)stub_NetReceiveFilterGetMulticastAddressList,
    [NetDriverExtensionInitializeTableIndex] = (NETFUNC)stub_NetDriverExtensionInitialize,
    [NetAdapterExtensionInitSetDirectOidRequestPreprocessCallbackTableIndex] = (NETFUNC)stub_NetAdapterExtensionInitSetDirectOidRequestPreprocessCallback,
    [NetExAdapterInitSetDirectOidPreprocessCallbackTableIndex] = (NETFUNC)stub_NetExAdapterInitSetDirectOidPreprocessCallback,
    [NetAdapterExtensionInitSetTxPeerDemuxCallbackTableIndex] = (NETFUNC)stub_NetAdapterExtensionInitSetTxPeerDemuxCallback,
    [NetAdapterDispatchPreprocessedDirectOidRequestTableIndex] = (NETFUNC)stub_NetAdapterDispatchPreprocessedDirectOidRequest,
    [NetExAdapterDispatchPreprocessedDirectOidTableIndex] = (NETFUNC)stub_NetExAdapterDispatchPreprocessedDirectOid,
    [NetAdapterExtensionSetNdisPmCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterExtensionSetNdisPmCapabilities,
    [NetAdapterInitAllocateContextTableIndex] = (NETFUNC)stub_NetAdapterInitAllocateContext,
    [NetAdapterInitGetTypedContextWorkerTableIndex] = (NETFUNC)stub_NetAdapterInitGetTypedContextWorker,
    [NetAdapterOffloadSetGsoCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterOffloadSetGsoCapabilities,
    [NetOffloadIsLsoIPv4EnabledTableIndex] = (NETFUNC)stub_NetOffloadIsLsoIPv4Enabled,
    [NetOffloadIsLsoIPv6EnabledTableIndex] = (NETFUNC)stub_NetOffloadIsLsoIPv6Enabled,
    [NetOffloadIsUsoIPv4EnabledTableIndex] = (NETFUNC)stub_NetOffloadIsUsoIPv4Enabled,
    [NetOffloadIsUsoIPv6EnabledTableIndex] = (NETFUNC)stub_NetOffloadIsUsoIPv6Enabled,
    [NetAdapterOffloadSetRscCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterOffloadSetRscCapabilities,
    [NetOffloadIsTcpRscIPv4EnabledTableIndex] = (NETFUNC)stub_NetOffloadIsTcpRscIPv4Enabled,
    [NetOffloadIsTcpRscIPv6EnabledTableIndex] = (NETFUNC)stub_NetOffloadIsTcpRscIPv6Enabled,
    [NetOffloadIsRscTcpTimestampOptionEnabledTableIndex] = (NETFUNC)stub_NetOffloadIsRscTcpTimestampOptionEnabled,
    [NetAdapterOffloadSetIeee8021qTagCapabilitiesTableIndex] = (NETFUNC)stub_NetAdapterOffloadSetIeee8021qTagCapabilities,
    [NetAdapterInitAddTxDemuxTableIndex] = (NETFUNC)stub_NetAdapterInitAddTxDemux,
    [NetAdapterGetTxPeerAddressDemuxTableIndex] = (NETFUNC)stub_NetAdapterGetTxPeerAddressDemux,
    [NetBufferQueueCreateTableIndex] = (NETFUNC)stub_NetBufferQueueCreate,
    [NetBufferQueueGetExtensionTableIndex] = (NETFUNC)stub_NetBufferQueueGetExtension,
    [NetBufferQueueGetRingTableIndex] = (NETFUNC)stub_NetBufferQueueGetRing,
    [NetDeviceInitSetResetCapabilitiesTableIndex] = (NETFUNC)stub_NetDeviceInitSetResetCapabilities,
    [NetDeviceStoreResetDiagnosticsTableIndex] = (NETFUNC)stub_NetDeviceStoreResetDiagnostics,
    [NetDeviceRequestResetTableIndex] = (NETFUNC)stub_NetDeviceRequestReset,
    [NetExecutionContextCreateTableIndex] = (NETFUNC)stub_NetExecutionContextCreate,
    [NetExecutionContextTaskCreateTableIndex] = (NETFUNC)stub_NetExecutionContextTaskCreate,
    [NetExecutionContextNotifyTableIndex] = (NETFUNC)stub_NetExecutionContextNotify,
    [NetExecutionContextTaskEnqueueTableIndex] = (NETFUNC)stub_NetExecutionContextTaskEnqueue,
    [NetExecutionContextTaskWaitCompletionTableIndex] = (NETFUNC)stub_NetExecutionContextTaskWaitCompletion,
    [NetPowerOffloadGetAdapterTableIndex] = (NETFUNC)stub_NetPowerOffloadGetAdapter,
};

/* Fills the stubbed slots; implemented ones are set by NxFillFunctionTable */
VOID
NxFillStubSlots(
    _Inout_updates_(NetFunctionTableNumEntries) NETFUNC *Table)
{
    ULONG i;

    for (i = 0; i < NetFunctionTableNumEntries; i++)
    {
        if (NxStubTable[i] != NULL)
            Table[i] = NxStubTable[i];
    }
}
