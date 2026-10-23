/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NDIS 6 miniport handlers for NetAdapterCx adapters
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * To NDIS each NETADAPTER is an NDIS 6.20 miniport adapter. The class
 * extension registers these handlers for every client driver
 * (NdisWdfRegisterMiniportDriver) and translates:
 *
 *   InitializeEx     attributes from the client's capabilities
 *   Restart/Pause    create and start / stop and delete the client's queues
 *   Send/Return      NET_BUFFER_LISTs to and from the rings (queue.c)
 *   OID requests     answered from the adapter's state
 */

#include "nxprivate.h"
#include "nxapi.h"

#define NDEBUG
#include <debug.h>

#define NX_VENDOR_DESCRIPTION   "ReactOS NetAdapterCx adapter"

static const NDIS_OID NxSupportedOids[] =
{
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_PHYSICAL_MEDIUM,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_GEN_STATISTICS,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
};

static
PNX_ADAPTER
NxMiniportAdapter(
    _In_ NDIS_HANDLE MiniportAdapterContext)
{
    return (PNX_ADAPTER)MiniportAdapterContext;
}

static
ULONG
NxReadConfigurationUlong(
    _In_ NDIS_HANDLE Configuration,
    _In_ PCWSTR Keyword,
    _In_ ULONG Default)
{
    PNDIS_CONFIGURATION_PARAMETER Parameter;
    NDIS_STRING Name;
    NDIS_STATUS Status;

    if (Configuration == NULL)
        return Default;

    RtlInitUnicodeString(&Name, Keyword);
    NdisReadConfiguration(&Status, &Parameter, Configuration, &Name, NdisParameterInteger);
    if (Status != NDIS_STATUS_SUCCESS || Parameter->ParameterType != NdisParameterInteger)
        return Default;

    return Parameter->ParameterData.IntegerData;
}

/* The standard keywords the INF of a network adapter writes */
static
VOID
NxReadKeywords(
    _In_ PNX_ADAPTER Adapter)
{
    NDIS_CONFIGURATION_OBJECT Object;
    NDIS_HANDLE Configuration = NULL;
    NDIS_STATUS Status;

    RtlZeroMemory(&Object, sizeof(Object));
    Object.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
    Object.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
    Object.Header.Size = NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1;
    Object.NdisHandle = Adapter->NdisHandle;

    Status = NdisOpenConfigurationEx(&Object, &Configuration);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT("No adapter configuration (0x%lx), using defaults\n", Status);
        Configuration = NULL;
    }

    Adapter->Medium = (NDIS_MEDIUM)NxReadConfigurationUlong(Configuration, L"*MediaType", NdisMedium802_3);
    Adapter->PhysicalMedium = (NDIS_PHYSICAL_MEDIUM)NxReadConfigurationUlong(Configuration, L"*PhysicalMediaType",
                                                                             NdisPhysicalMediumUnspecified);
    Adapter->IfType = (NET_IFTYPE)NxReadConfigurationUlong(Configuration, L"*IfType",
                                                           Adapter->Medium == NdisMedium802_3 ?
                                                               IF_TYPE_ETHERNET_CSMACD : IF_TYPE_PROP_VIRTUAL);
    Adapter->AccessType = (NET_IF_ACCESS_TYPE)NxReadConfigurationUlong(Configuration, L"*AccessType",
                                                                       Adapter->Medium == NdisMedium802_3 ?
                                                                           NET_IF_ACCESS_BROADCAST :
                                                                           NET_IF_ACCESS_POINT_TO_POINT);
    Adapter->DirectionType = (NET_IF_DIRECTION_TYPE)NxReadConfigurationUlong(Configuration, L"*DirectionType",
                                                                             NET_IF_DIRECTION_SENDRECEIVE);
    Adapter->ConnectionType = (NET_IF_CONNECTION_TYPE)NxReadConfigurationUlong(Configuration, L"*ConnectionType",
                                                                               NET_IF_CONNECTION_DEDICATED);

    if (Adapter->Medium != NdisMedium802_3 && Adapter->Medium != NdisMediumIP)
    {
        DbgPrint("NetAdapterCx: medium %d is not supported, using 802.3\n", Adapter->Medium);
        Adapter->Medium = NdisMedium802_3;
    }

    if (Configuration != NULL)
        NdisCloseConfiguration(Configuration);
}

static
VOID
NxAssignMacAddress(
    _In_ PNX_ADAPTER Adapter)
{
    LARGE_INTEGER Counter;
    ULONG Random;

    if (Adapter->Medium != NdisMedium802_3)
        return;

    if (Adapter->PermanentAddress.Length == 0)
    {
        /* Locally administered unicast address */
        Counter = KeQueryPerformanceCounter(NULL);
        Random = Counter.LowPart ^ (ULONG)(ULONG_PTR)Adapter;
        Adapter->PermanentAddress.Length = 6;
        Adapter->PermanentAddress.Address[0] = 0x02;
        Adapter->PermanentAddress.Address[1] = 0x4E; /* 'N' */
        RtlCopyMemory(&Adapter->PermanentAddress.Address[2], &Random, 4);
    }

    if (Adapter->CurrentAddress.Length == 0)
        Adapter->CurrentAddress = Adapter->PermanentAddress;
}

static MINIPORT_INITIALIZE NxMiniportInitializeEx;

static
NDIS_STATUS
NTAPI
NxMiniportInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES Registration;
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES General;
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParameters;
    PNX_ADAPTER Adapter;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(MiniportDriverContext);

    Adapter = (PNX_ADAPTER)MiniportInitParameters->MiniportAddDeviceContext;
    if (Adapter == NULL)
        return NDIS_STATUS_FAILURE;

    NxLock(&Adapter->Lock);

    Adapter->NdisHandle = NdisMiniportHandle;
    Adapter->NetLuidIndex = MiniportInitParameters->NetLuid.Info.NetLuidIndex;

    RtlZeroMemory(&Registration, sizeof(Registration));
    Registration.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    Registration.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    Registration.Header.Size = sizeof(Registration);
    Registration.MiniportAdapterContext = Adapter;
    Registration.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND;
    Registration.CheckForHangTimeInSeconds = 0;
    Registration.InterfaceType = NdisInterfaceInternal;

    Status = NdisMSetMiniportAttributes(NdisMiniportHandle,
                                        (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&Registration);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Exit;

    NxReadKeywords(Adapter);
    NxAssignMacAddress(Adapter);

    RtlZeroMemory(&General, sizeof(General));
    General.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    General.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
    General.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
    General.MediaType = Adapter->Medium;
    General.PhysicalMediumType = Adapter->PhysicalMedium;
    General.MtuSize = Adapter->MtuSize;
    General.MaxXmitLinkSpeed = Adapter->LinkLayerCapabilities.MaxTxLinkSpeed;
    General.MaxRcvLinkSpeed = Adapter->LinkLayerCapabilities.MaxRxLinkSpeed;
    General.XmitLinkSpeed = Adapter->LinkState.TxLinkSpeed;
    General.RcvLinkSpeed = Adapter->LinkState.RxLinkSpeed;
    General.MediaConnectState = (NDIS_MEDIA_CONNECT_STATE)Adapter->LinkState.MediaConnectState;
    General.MediaDuplexState = (NDIS_MEDIA_DUPLEX_STATE)Adapter->LinkState.MediaDuplexState;
    General.LookaheadSize = Adapter->MtuSize;
    General.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                         NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                         NDIS_MAC_OPTION_NO_LOOPBACK;
    General.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED |
                                     NDIS_PACKET_TYPE_MULTICAST |
                                     NDIS_PACKET_TYPE_ALL_MULTICAST |
                                     NDIS_PACKET_TYPE_BROADCAST |
                                     NDIS_PACKET_TYPE_PROMISCUOUS;
    General.MaxMulticastListSize = 32;
    General.MacAddressLength = (Adapter->Medium == NdisMedium802_3) ? 6 : 0;
    RtlCopyMemory(General.PermanentMacAddress, Adapter->PermanentAddress.Address,
                  min(sizeof(General.PermanentMacAddress), sizeof(Adapter->PermanentAddress.Address)));
    RtlCopyMemory(General.CurrentMacAddress, Adapter->CurrentAddress.Address,
                  min(sizeof(General.CurrentMacAddress), sizeof(Adapter->CurrentAddress.Address)));
    General.AccessType = Adapter->AccessType;
    General.DirectionType = Adapter->DirectionType;
    General.ConnectionType = Adapter->ConnectionType;
    General.IfType = Adapter->IfType;
    General.IfConnectorPresent = FALSE;
    General.SupportedStatistics = NDIS_STATISTICS_XMIT_OK_SUPPORTED |
                                  NDIS_STATISTICS_RCV_OK_SUPPORTED |
                                  NDIS_STATISTICS_XMIT_ERROR_SUPPORTED |
                                  NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED |
                                  NDIS_STATISTICS_BYTES_RCV_SUPPORTED |
                                  NDIS_STATISTICS_BYTES_XMIT_SUPPORTED |
                                  NDIS_STATISTICS_GEN_STATISTICS_SUPPORTED;
    General.SupportedPauseFunctions = (NDIS_SUPPORTED_PAUSE_FUNCTIONS)Adapter->LinkState.SupportedPauseFunctions;
    General.AutoNegotiationFlags = Adapter->LinkState.AutoNegotiationFlags;
    General.SupportedOidList = (PNDIS_OID)NxSupportedOids;
    General.SupportedOidListLength = sizeof(NxSupportedOids);

    Status = NdisMSetMiniportAttributes(NdisMiniportHandle,
                                        (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&General);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Exit;

    RtlZeroMemory(&PoolParameters, sizeof(PoolParameters));
    PoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParameters.Header.Size = sizeof(PoolParameters);
    PoolParameters.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    PoolParameters.fAllocateNetBuffer = TRUE;
    PoolParameters.PoolTag = NX_RX_TAG;

    Adapter->RxNblPool = NdisAllocateNetBufferListPool(NdisMiniportHandle, &PoolParameters);
    if (Adapter->RxNblPool == NULL)
    {
        Status = NDIS_STATUS_RESOURCES;
        goto Exit;
    }

    Adapter->PacketFilter = 0;
    Adapter->Lookahead = Adapter->MtuSize;
    Adapter->Initialized = TRUE;

    DbgPrint("NetAdapterCx: adapter %p initialized, medium %d, mtu %lu, link %d\n",
             Adapter->Handle, Adapter->Medium, Adapter->MtuSize,
             Adapter->LinkState.MediaConnectState);

Exit:
    NxUnlock(&Adapter->Lock);
    return Status;
}

static MINIPORT_HALT NxMiniportHaltEx;

static
VOID
NTAPI
NxMiniportHaltEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PNX_ADAPTER Adapter = NxMiniportAdapter(MiniportAdapterContext);

    UNREFERENCED_PARAMETER(HaltAction);

    NxLock(&Adapter->Lock);

    /* NDIS pauses first; be safe if it did not */
    NxDatapathStop(Adapter);

    if (Adapter->RxNblPool != NULL)
    {
        NdisFreeNetBufferListPool(Adapter->RxNblPool);
        Adapter->RxNblPool = NULL;
    }

    Adapter->Initialized = FALSE;
    NxUnlock(&Adapter->Lock);

    DbgPrint("NetAdapterCx: adapter %p halted\n", Adapter->Handle);
}

static MINIPORT_PAUSE NxMiniportPause;

static
NDIS_STATUS
NTAPI
NxMiniportPause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters)
{
    PNX_ADAPTER Adapter = NxMiniportAdapter(MiniportAdapterContext);

    UNREFERENCED_PARAMETER(PauseParameters);

    NxLock(&Adapter->Lock);
    NxDatapathStop(Adapter);
    NxUnlock(&Adapter->Lock);

    return NDIS_STATUS_SUCCESS;
}

static MINIPORT_RESTART NxMiniportRestart;

static
NDIS_STATUS
NTAPI
NxMiniportRestart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters)
{
    PNX_ADAPTER Adapter = NxMiniportAdapter(MiniportAdapterContext);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RestartParameters);

    NxLock(&Adapter->Lock);
    Status = NxDatapathStart(Adapter);
    NxUnlock(&Adapter->Lock);

    return NT_SUCCESS(Status) ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;
}

static MINIPORT_SEND_NET_BUFFER_LISTS NxMiniportSendNetBufferLists;

static
VOID
NTAPI
NxMiniportSendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PNX_ADAPTER Adapter = NxMiniportAdapter(MiniportAdapterContext);
    PNET_BUFFER_LIST Nbl;

    UNREFERENCED_PARAMETER(PortNumber);

    if (NetBufferLists == NULL)
        return;

    if (ExAcquireRundownProtection(&Adapter->DatapathRundown))
    {
        if (Adapter->TxQueue != NULL)
        {
            NxQueueSend(Adapter->TxQueue, NetBufferLists);
            ExReleaseRundownProtection(&Adapter->DatapathRundown);
            return;
        }
        ExReleaseRundownProtection(&Adapter->DatapathRundown);
    }

    /* Paused */
    for (Nbl = NetBufferLists; Nbl != NULL; Nbl = NET_BUFFER_LIST_NEXT_NBL(Nbl))
        NET_BUFFER_LIST_STATUS(Nbl) = NDIS_STATUS_PAUSED;

    NdisMSendNetBufferListsComplete(Adapter->NdisHandle, NetBufferLists,
                                    NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendFlags) ?
                                        NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
}

static MINIPORT_RETURN_NET_BUFFER_LISTS NxMiniportReturnNetBufferLists;

static
VOID
NTAPI
NxMiniportReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    UNREFERENCED_PARAMETER(ReturnFlags);

    NxRxReturnNetBufferLists(NxMiniportAdapter(MiniportAdapterContext), NetBufferLists);
}

static MINIPORT_CANCEL_SEND NxMiniportCancelSend;

static
VOID
NTAPI
NxMiniportCancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId)
{
    /* Sends complete once the client returns their packets */
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
}

static MINIPORT_DEVICE_PNP_EVENT_NOTIFY NxMiniportDevicePnPEventNotify;

static
VOID
NTAPI
NxMiniportDevicePnPEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(NetDevicePnPEvent);
}

static MINIPORT_SHUTDOWN NxMiniportShutdownEx;

static
VOID
NTAPI
NxMiniportShutdownEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ShutdownAction);
}

static MINIPORT_CANCEL_OID_REQUEST NxMiniportCancelOidRequest;

static
VOID
NTAPI
NxMiniportCancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    /* Requests complete synchronously */
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
}

static
NDIS_STATUS
NxQueryInformation(
    _In_ PNX_ADAPTER Adapter,
    _Inout_ PNDIS_OID_REQUEST Request)
{
    NDIS_OID Oid = Request->DATA.QUERY_INFORMATION.Oid;
    PVOID Buffer = Request->DATA.QUERY_INFORMATION.InformationBuffer;
    ULONG BufferLength = Request->DATA.QUERY_INFORMATION.InformationBufferLength;
    NDIS_STATISTICS_INFO Statistics;
    PVOID Source;
    ULONG SourceLength = sizeof(ULONG);
    ULONG Value = 0;
    ULONG64 Value64 = 0;
    BOOLEAN Use64 = FALSE;

    Source = &Value;

    switch (Oid)
    {
        case OID_GEN_SUPPORTED_LIST:
            Source = (PVOID)NxSupportedOids;
            SourceLength = sizeof(NxSupportedOids);
            break;

        case OID_GEN_HARDWARE_STATUS:
            Value = NdisHardwareStatusReady;
            break;

        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            Value = Adapter->Medium;
            break;

        case OID_GEN_PHYSICAL_MEDIUM:
            Value = Adapter->PhysicalMedium;
            break;

        case OID_GEN_MAXIMUM_LOOKAHEAD:
        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_GEN_MAXIMUM_FRAME_SIZE:
            Value = Adapter->MtuSize;
            break;

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            Value = Adapter->MtuSize + ((Adapter->Medium == NdisMedium802_3) ? 14 : 0);
            break;

        case OID_GEN_TRANSMIT_BUFFER_SPACE:
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            Value = (Adapter->MtuSize + 14) * 64;
            break;

        case OID_GEN_LINK_SPEED:
            /* In units of 100 bps */
            Value64 = Adapter->LinkState.TxLinkSpeed;
            Value = (Value64 == NET_ADAPTER_LINK_SPEED_UNKNOWN) ? 0 : (ULONG)min(Value64 / 100, MAXULONG);
            break;

        case OID_GEN_VENDOR_ID:
            Value = 0x00FFFFFF;
            break;

        case OID_GEN_VENDOR_DESCRIPTION:
            Source = NX_VENDOR_DESCRIPTION;
            SourceLength = sizeof(NX_VENDOR_DESCRIPTION);
            break;

        case OID_GEN_VENDOR_DRIVER_VERSION:
            Value = (NX_VERSION_MAJOR << 16) | NX_VERSION_MINOR;
            break;

        case OID_GEN_DRIVER_VERSION:
            Value = (6 << 8) | 20;
            SourceLength = sizeof(USHORT);
            break;

        case OID_GEN_CURRENT_PACKET_FILTER:
            Value = Adapter->PacketFilter;
            break;

        case OID_GEN_MAC_OPTIONS:
            Value = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                    NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                    NDIS_MAC_OPTION_NO_LOOPBACK;
            break;

        case OID_GEN_MEDIA_CONNECT_STATUS:
            Value = (Adapter->LinkState.MediaConnectState == MediaConnectStateConnected) ?
                        NdisMediaStateConnected : NdisMediaStateDisconnected;
            break;

        case OID_GEN_MAXIMUM_SEND_PACKETS:
            Value = 64;
            break;

        case OID_GEN_XMIT_OK:
            Value64 = Adapter->TxPackets;
            Use64 = TRUE;
            break;

        case OID_GEN_RCV_OK:
            Value64 = Adapter->RxPackets;
            Use64 = TRUE;
            break;

        case OID_GEN_XMIT_ERROR:
            Value64 = Adapter->TxErrors;
            Use64 = TRUE;
            break;

        case OID_GEN_RCV_ERROR:
            Value = 0;
            break;

        case OID_GEN_RCV_NO_BUFFER:
            Value64 = Adapter->RxDropped;
            Use64 = TRUE;
            break;

        case OID_GEN_STATISTICS:
            RtlZeroMemory(&Statistics, sizeof(Statistics));
            Statistics.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            Statistics.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
            Statistics.Header.Size = sizeof(Statistics);
            Statistics.SupportedStatistics = NDIS_STATISTICS_XMIT_OK_SUPPORTED |
                                             NDIS_STATISTICS_RCV_OK_SUPPORTED |
                                             NDIS_STATISTICS_XMIT_ERROR_SUPPORTED |
                                             NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED |
                                             NDIS_STATISTICS_BYTES_RCV_SUPPORTED |
                                             NDIS_STATISTICS_BYTES_XMIT_SUPPORTED;
            Statistics.ifHCInUcastPkts = Adapter->RxPackets;
            Statistics.ifHCOutUcastPkts = Adapter->TxPackets;
            Statistics.ifHCInOctets = Adapter->RxBytes;
            Statistics.ifHCOutOctets = Adapter->TxBytes;
            Statistics.ifOutErrors = Adapter->TxErrors;
            Statistics.ifInDiscards = Adapter->RxDropped;
            Source = &Statistics;
            SourceLength = sizeof(Statistics);
            break;

        case OID_802_3_PERMANENT_ADDRESS:
        case OID_802_3_CURRENT_ADDRESS:
            if (Adapter->Medium != NdisMedium802_3)
                return NDIS_STATUS_NOT_SUPPORTED;
            Source = (Oid == OID_802_3_PERMANENT_ADDRESS) ? Adapter->PermanentAddress.Address :
                                                            Adapter->CurrentAddress.Address;
            SourceLength = 6;
            break;

        case OID_802_3_MAXIMUM_LIST_SIZE:
            Value = 32;
            break;

        default:
            DPRINT("Query OID 0x%08lx not supported\n", Oid);
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    if (Use64)
    {
        /* Counters come as 32 or 64 bits, as the caller has room for */
        Source = &Value64;
        SourceLength = (BufferLength >= sizeof(ULONG64)) ? sizeof(ULONG64) : sizeof(ULONG);
    }

    if (BufferLength < SourceLength)
    {
        Request->DATA.QUERY_INFORMATION.BytesNeeded = SourceLength;
        Request->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    RtlCopyMemory(Buffer, Source, SourceLength);
    Request->DATA.QUERY_INFORMATION.BytesWritten = SourceLength;
    Request->DATA.QUERY_INFORMATION.BytesNeeded = SourceLength;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NxSetInformation(
    _In_ PNX_ADAPTER Adapter,
    _Inout_ PNDIS_OID_REQUEST Request)
{
    NDIS_OID Oid = Request->DATA.SET_INFORMATION.Oid;
    PVOID Buffer = Request->DATA.SET_INFORMATION.InformationBuffer;
    ULONG BufferLength = Request->DATA.SET_INFORMATION.InformationBufferLength;

    Request->DATA.SET_INFORMATION.BytesRead = 0;
    Request->DATA.SET_INFORMATION.BytesNeeded = 0;

    switch (Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
        case OID_GEN_CURRENT_LOOKAHEAD:
            if (BufferLength < sizeof(ULONG))
            {
                Request->DATA.SET_INFORMATION.BytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (Oid == OID_GEN_CURRENT_PACKET_FILTER)
                Adapter->PacketFilter = *(PULONG)Buffer;
            else
                Adapter->Lookahead = *(PULONG)Buffer;
            Request->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_802_3_MULTICAST_LIST:
            /* Every frame is delivered; no hardware filter to program */
            Request->DATA.SET_INFORMATION.BytesRead = BufferLength;
            return NDIS_STATUS_SUCCESS;

        default:
            DPRINT("Set OID 0x%08lx not supported\n", Oid);
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static MINIPORT_OID_REQUEST NxMiniportOidRequest;

static
NDIS_STATUS
NTAPI
NxMiniportOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PNX_ADAPTER Adapter = NxMiniportAdapter(MiniportAdapterContext);

    if (Adapter == NULL || !Adapter->Initialized)
        return NDIS_STATUS_ADAPTER_NOT_READY;

    switch (OidRequest->RequestType)
    {
        case NdisRequestQueryInformation:
        case NdisRequestQueryStatistics:
            return NxQueryInformation(Adapter, OidRequest);

        case NdisRequestSetInformation:
            return NxSetInformation(Adapter, OidRequest);

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

VOID
NxMiniportIndicateLinkState(
    _In_ PNX_ADAPTER Adapter)
{
    NDIS_STATUS_INDICATION Indication;
    NDIS_LINK_STATE LinkState;

    RtlZeroMemory(&LinkState, sizeof(LinkState));
    LinkState.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    LinkState.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    LinkState.Header.Size = sizeof(LinkState);
    LinkState.MediaConnectState = (NDIS_MEDIA_CONNECT_STATE)Adapter->LinkState.MediaConnectState;
    LinkState.MediaDuplexState = (NDIS_MEDIA_DUPLEX_STATE)Adapter->LinkState.MediaDuplexState;
    LinkState.XmitLinkSpeed = Adapter->LinkState.TxLinkSpeed;
    LinkState.RcvLinkSpeed = Adapter->LinkState.RxLinkSpeed;
    LinkState.PauseFunctions = (NDIS_SUPPORTED_PAUSE_FUNCTIONS)Adapter->LinkState.SupportedPauseFunctions;
    LinkState.AutoNegotiationFlags = Adapter->LinkState.AutoNegotiationFlags;

    RtlZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size = sizeof(Indication);
    Indication.SourceHandle = Adapter->NdisHandle;
    Indication.StatusCode = NDIS_STATUS_LINK_STATE;
    Indication.StatusBuffer = &LinkState;
    Indication.StatusBufferSize = sizeof(LinkState);

    NdisMIndicateStatusEx(Adapter->NdisHandle, &Indication);
}

VOID
NxMiniportFillCharacteristics(
    _Out_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics)
{
    RtlZeroMemory(Characteristics, sizeof(*Characteristics));
    Characteristics->Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    Characteristics->Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    Characteristics->Header.Size = sizeof(*Characteristics);
    Characteristics->MajorNdisVersion = 6;
    Characteristics->MinorNdisVersion = 20;
    Characteristics->MajorDriverVersion = NX_VERSION_MAJOR;
    Characteristics->MinorDriverVersion = NX_VERSION_MINOR;

    Characteristics->InitializeHandlerEx = NxMiniportInitializeEx;
    Characteristics->HaltHandlerEx = NxMiniportHaltEx;
    Characteristics->PauseHandler = NxMiniportPause;
    Characteristics->RestartHandler = NxMiniportRestart;
    Characteristics->OidRequestHandler = NxMiniportOidRequest;
    Characteristics->SendNetBufferListsHandler = NxMiniportSendNetBufferLists;
    Characteristics->ReturnNetBufferListsHandler = NxMiniportReturnNetBufferLists;
    Characteristics->CancelSendHandler = NxMiniportCancelSend;
    Characteristics->DevicePnPEventNotifyHandler = NxMiniportDevicePnPEventNotify;
    Characteristics->ShutdownHandlerEx = NxMiniportShutdownEx;
    Characteristics->CancelOidRequestHandler = NxMiniportCancelOidRequest;
}
