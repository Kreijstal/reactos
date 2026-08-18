/*
 * PROJECT:     ReactOS KDNET Shared Adapter Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Gives the OS a network interface on the adapter kdnet.dll owns
 *
 * A machine with one NIC used to have to choose: a kernel debugger, or a
 * network.  pci.sys hides the debug adapter behind VEN_DEAD&DEV_BEEF precisely
 * so that no ordinary miniport can bind to it and start driving rings that
 * kdnet.dll is already driving from bugcheck context.
 *
 * This driver binds to that hidden device and does not drive it either.  It
 * owns no registers, no interrupt and no DMA; every frame goes through the
 * share interface in kdnet.dll, which keeps exclusive control of the hardware
 * and hands us the traffic that is not the debugger's.  Transmits are pushed
 * synchronously; receives are polled, because the interrupt belongs to the
 * debugger and taking it away is exactly what would break debugging.
 *
 * Nothing here loads unless /KDNETSHARE was passed at boot: without it
 * KdNetShareRegister fails and MiniportInitializeEx returns, leaving the device
 * with the same "no driver" problem code it had before this driver existed.
 */

#include "kdnetshare.h"
#include <debug.h>

static NDIS_HANDLE KdnsMiniportDriverHandle = NULL;

static NDIS_OID KdnsSupportedOids[] =
{
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_LINK_SPEED,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_STATISTICS,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_GEN_LINK_PARAMETERS,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS,
};

#define KDNS_SUPPORTED_OID_COUNT \
    (sizeof(KdnsSupportedOids) / sizeof(KdnsSupportedOids[0]))


/* ------------------------------------------------------------------------- *
 * Receive
 * ------------------------------------------------------------------------- */

/*
 * Called by kdnet.dll at HIGH_LEVEL with its share lock held, once per frame
 * that belongs to us.  The constraints there are severe: the frame buffer is
 * the receive ring and dies when we return, nothing may block, and nothing may
 * print - DbgPrint reaches KdSendPacket, which would re-enter the transport
 * that is mid-poll.  So this does the only thing it can: copy into a free slot
 * and get out.  The NBL machinery runs afterwards, back at DISPATCH_LEVEL.
 */
static VOID
NTAPI
KdnsShareReceive(
    _In_opt_ PVOID Context,
    _In_reads_bytes_(Length) const UCHAR *Frame,
    _In_ ULONG Length)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)Context;
    ULONG Index, Slot;

    if (Adapter == NULL || Length == 0 || Length > KDNS_FRAME_SIZE)
        return;
    if (!Adapter->DataPathRunning || Adapter->Halting)
        return;

    for (Index = 0; Index < KDNS_RX_BUFFERS; ++Index)
    {
        Slot = (Adapter->RxNext + Index) % KDNS_RX_BUFFERS;

        if (InterlockedCompareExchange(&Adapter->Rx[Slot].State,
                                       KDNS_SLOT_READY,
                                       KDNS_SLOT_FREE) != KDNS_SLOT_FREE)
        {
            continue;
        }

        NdisMoveMemory(Adapter->Rx[Slot].Data, Frame, Length);
        Adapter->Rx[Slot].Length = Length;
        Adapter->RxNext = (Slot + 1) % KDNS_RX_BUFFERS;
        return;
    }

    /* Every slot is either filled or still up in the stack.  Dropping is the
     * correct response - the alternative is to stall the debugger's poll. */
    Adapter->RxNoBuffer++;
}

/* Builds and indicates NBLs for whatever the callback filled in.  Runs at
 * DISPATCH_LEVEL, outside kdnet's share lock. */
static VOID
KdnsIndicateReadySlots(
    _In_ PKDNS_ADAPTER Adapter)
{
    PNET_BUFFER_LIST Chain = NULL, Last = NULL, Nbl;
    PMDL Mdl;
    ULONG Slot, Count = 0;

    for (Slot = 0; Slot < KDNS_RX_BUFFERS; ++Slot)
    {
        if (InterlockedCompareExchange(&Adapter->Rx[Slot].State,
                                       KDNS_SLOT_INDICATED,
                                       KDNS_SLOT_READY) != KDNS_SLOT_READY)
        {
            continue;
        }

        Mdl = NdisAllocateMdl(Adapter->MiniportAdapterHandle,
                              Adapter->Rx[Slot].Data,
                              Adapter->Rx[Slot].Length);
        if (Mdl == NULL)
        {
            InterlockedExchange(&Adapter->Rx[Slot].State, KDNS_SLOT_FREE);
            Adapter->RxNoBuffer++;
            continue;
        }

        Nbl = NdisAllocateNetBufferAndNetBufferList(Adapter->RxNblPool,
                                                    0, 0, Mdl, 0,
                                                    Adapter->Rx[Slot].Length);
        if (Nbl == NULL)
        {
            NdisFreeMdl(Mdl);
            InterlockedExchange(&Adapter->Rx[Slot].State, KDNS_SLOT_FREE);
            Adapter->RxNoBuffer++;
            continue;
        }

        /* Remember which slot to release when the stack returns this NBL. */
        NET_BUFFER_LIST_MINIPORT_RESERVED(Nbl)[0] = (PVOID)(ULONG_PTR)Slot;
        Nbl->SourceHandle = Adapter->MiniportAdapterHandle;
        NET_BUFFER_LIST_NEXT_NBL(Nbl) = NULL;

        if (Chain == NULL)
            Chain = Nbl;
        else
            NET_BUFFER_LIST_NEXT_NBL(Last) = Nbl;
        Last = Nbl;

        Adapter->RxOk++;
        Adapter->RxBytes += Adapter->Rx[Slot].Length;
        ++Count;
    }

    if (Chain != NULL)
    {
        NdisMIndicateReceiveNetBufferLists(Adapter->MiniportAdapterHandle,
                                           Chain, 0, Count,
                                           NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);
    }
}

VOID
NTAPI
KdnsReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)MiniportAdapterContext;
    PNET_BUFFER_LIST Nbl = NetBufferLists, Next;
    PMDL Mdl;
    ULONG Slot;

    UNREFERENCED_PARAMETER(ReturnFlags);

    while (Nbl != NULL)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(Nbl);
        Slot = (ULONG)(ULONG_PTR)NET_BUFFER_LIST_MINIPORT_RESERVED(Nbl)[0];

        Mdl = NET_BUFFER_FIRST_MDL(NET_BUFFER_LIST_FIRST_NB(Nbl));
        NdisFreeNetBufferList(Nbl);
        if (Mdl != NULL)
            NdisFreeMdl(Mdl);

        if (Slot < KDNS_RX_BUFFERS)
            InterlockedExchange(&Adapter->Rx[Slot].State, KDNS_SLOT_FREE);

        Nbl = Next;
    }
}


/* ------------------------------------------------------------------------- *
 * Poll
 * ------------------------------------------------------------------------- */

static VOID
NTAPI
KdnsPollTimerDpc(
    _In_ PVOID SystemSpecific1,
    _In_ PVOID FunctionContext,
    _In_ PVOID SystemSpecific2,
    _In_ PVOID SystemSpecific3)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)FunctionContext;

    UNREFERENCED_PARAMETER(SystemSpecific1);
    UNREFERENCED_PARAMETER(SystemSpecific2);
    UNREFERENCED_PARAMETER(SystemSpecific3);

    if (Adapter == NULL)
        return;

    /* PollActive is what Pause and Halt wait on, so it must bracket every call
     * into kdnet.dll and every indication that follows from one - and it has to
     * be raised BEFORE the Halting test, or a DPC that has just passed the test
     * would be invisible to the waiter and run on a torn-down adapter. */
    InterlockedIncrement(&Adapter->PollActive);

    if (Adapter->DataPathRunning && !Adapter->Halting)
    {
        if (KdNetSharePoll(KDNS_RX_DRAIN) != 0)
            KdnsIndicateReadySlots(Adapter);
    }

    InterlockedDecrement(&Adapter->PollActive);
}

static VOID
KdnsStartPolling(
    _In_ PKDNS_ADAPTER Adapter)
{
    LARGE_INTEGER Due;

    Due.QuadPart = -(LONGLONG)KDNS_POLL_INTERVAL_MS * 10000;
    NdisSetTimerObject(Adapter->PollTimer, Due, KDNS_POLL_INTERVAL_MS, NULL);
}

static VOID
KdnsStopPolling(
    _In_ PKDNS_ADAPTER Adapter)
{
    if (Adapter->PollTimer != NULL)
        NdisCancelTimerObject(Adapter->PollTimer);

    /* NdisCancelTimerObject does not wait for a DPC already running, and that
     * DPC is inside kdnet.dll holding the share lock.  Deregistering while it
     * runs would let the callback fire against freed state. */
    while (InterlockedCompareExchange(&Adapter->PollActive, 0, 0) != 0)
        NdisMSleep(1000);
}


/* ------------------------------------------------------------------------- *
 * Transmit
 * ------------------------------------------------------------------------- */

VOID
NTAPI
KdnsSendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)MiniportAdapterContext;
    PNET_BUFFER_LIST Nbl = NetBufferLists;
    PNET_BUFFER NetBuffer;
    ULONG CompleteFlags = 0;
    ULONG Length;
    PUCHAR Data;

    UNREFERENCED_PARAMETER(PortNumber);

    if (NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendFlags))
        CompleteFlags |= NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL;

    while (Nbl != NULL)
    {
        NDIS_STATUS NblStatus = NDIS_STATUS_SUCCESS;

        for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(Nbl);
             NetBuffer != NULL;
             NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
        {
            Length = NET_BUFFER_DATA_LENGTH(NetBuffer);

            if (!Adapter->DataPathRunning || Adapter->Halting ||
                Length == 0 || Length > KDNS_FRAME_SIZE)
            {
                Adapter->TxError++;
                NblStatus = NDIS_STATUS_FAILURE;
                continue;
            }

            /* kdnet.dll copies the frame into its own transmit ring, but it
             * needs it flat first: it has no idea what an MDL chain is. */
            Data = NdisGetDataBuffer(NetBuffer, Length, Adapter->TxFrame, 1, 0);
            if (Data == NULL)
            {
                Adapter->TxError++;
                NblStatus = NDIS_STATUS_FAILURE;
                continue;
            }

            if (NT_SUCCESS(KdNetShareTransmit(Data, Length)))
            {
                Adapter->TxOk++;
                Adapter->TxBytes += Length;
            }
            else
            {
                Adapter->TxError++;
                NblStatus = NDIS_STATUS_FAILURE;
            }
        }

        NET_BUFFER_LIST_STATUS(Nbl) = NblStatus;
        Nbl = NET_BUFFER_LIST_NEXT_NBL(Nbl);
    }

    /* Every send finished inline, so nothing is ever left pending. */
    NdisMSendNetBufferListsComplete(Adapter->MiniportAdapterHandle,
                                    NetBufferLists, CompleteFlags);
}

VOID
NTAPI
KdnsCancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
}


/* ------------------------------------------------------------------------- *
 * OIDs
 * ------------------------------------------------------------------------- */

static VOID
KdnsIndicateLinkState(
    _In_ PKDNS_ADAPTER Adapter,
    _In_ BOOLEAN Connected)
{
    NDIS_LINK_STATE LinkState;
    NDIS_STATUS_INDICATION Indication;
    ULONG64 Speed = (ULONG64)Adapter->ShareInfo.LinkSpeedMbps * 1000000ULL;

    NdisZeroMemory(&LinkState, sizeof(LinkState));
    LinkState.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    LinkState.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    LinkState.Header.Size = sizeof(NDIS_LINK_STATE);
    LinkState.MediaConnectState =
        Connected ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    LinkState.MediaDuplexState = MediaDuplexStateFull;
    LinkState.XmitLinkSpeed = Connected ? Speed : NDIS_LINK_SPEED_UNKNOWN;
    LinkState.RcvLinkSpeed = Connected ? Speed : NDIS_LINK_SPEED_UNKNOWN;
    LinkState.PauseFunctions = NdisPauseFunctionsUnsupported;

    NdisZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size = sizeof(NDIS_STATUS_INDICATION);
    Indication.SourceHandle = Adapter->MiniportAdapterHandle;
    Indication.StatusCode = NDIS_STATUS_LINK_STATE;
    Indication.StatusBuffer = &LinkState;
    Indication.StatusBufferSize = sizeof(LinkState);

    NdisMIndicateStatusEx(Adapter->MiniportAdapterHandle, &Indication);
}

static NDIS_STATUS
KdnsQueryInformation(
    _In_ PKDNS_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST Request)
{
    struct _QUERY *Query = &Request->DATA.QUERY_INFORMATION;
    NDIS_STATISTICS_INFO Stats;
    ULONG64 Value64;
    ULONG Value32;
    const VOID *Source = NULL;
    ULONG SourceLength = 0;

    switch (Query->Oid)
    {
        case OID_GEN_SUPPORTED_LIST:
            Source = KdnsSupportedOids;
            SourceLength = sizeof(KdnsSupportedOids);
            break;

        case OID_GEN_HARDWARE_STATUS:
            Value32 = NdisHardwareStatusReady;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            Value32 = NdisMedium802_3;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_MAXIMUM_FRAME_SIZE:
            Value32 = KDNS_MTU_SIZE;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            Value32 = KDNS_FRAME_SIZE;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_MAXIMUM_LOOKAHEAD:
            Value32 = KDNS_FRAME_SIZE;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_CURRENT_LOOKAHEAD:
            Value32 = Adapter->Lookahead;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_CURRENT_PACKET_FILTER:
            Value32 = Adapter->PacketFilter;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_VENDOR_ID:
            /* Locally administered; there is no real vendor behind this. */
            Value32 = 0x00FFFFFF;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_VENDOR_DESCRIPTION:
            Source = "ReactOS KDNET Shared Adapter";
            SourceLength = sizeof("ReactOS KDNET Shared Adapter");
            break;

        case OID_GEN_VENDOR_DRIVER_VERSION:
        case OID_GEN_DRIVER_VERSION:
            Value32 = 0x00010000;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_LINK_SPEED:
            /* Reported in 100 bps units. */
            Value32 = Adapter->ShareInfo.LinkSpeedMbps * 10000;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_MEDIA_CONNECT_STATUS:
            Value32 = Adapter->Registered ? NdisMediaStateConnected
                                          : NdisMediaStateDisconnected;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        case OID_GEN_XMIT_OK:
            Value64 = Adapter->TxOk;
            Source = &Value64;
            SourceLength = sizeof(Value64);
            break;

        case OID_GEN_RCV_OK:
            Value64 = Adapter->RxOk;
            Source = &Value64;
            SourceLength = sizeof(Value64);
            break;

        case OID_GEN_XMIT_ERROR:
            Value64 = Adapter->TxError;
            Source = &Value64;
            SourceLength = sizeof(Value64);
            break;

        case OID_GEN_RCV_ERROR:
        case OID_802_3_RCV_ERROR_ALIGNMENT:
        case OID_802_3_XMIT_ONE_COLLISION:
        case OID_802_3_XMIT_MORE_COLLISIONS:
            Value64 = 0;
            Source = &Value64;
            SourceLength = sizeof(Value64);
            break;

        case OID_GEN_RCV_NO_BUFFER:
            Value64 = Adapter->RxNoBuffer;
            Source = &Value64;
            SourceLength = sizeof(Value64);
            break;

        case OID_GEN_STATISTICS:
            NdisZeroMemory(&Stats, sizeof(Stats));
            Stats.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            Stats.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
            Stats.Header.Size = sizeof(NDIS_STATISTICS_INFO);
            Stats.SupportedStatistics =
                NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV |
                NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV |
                NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS |
                NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT |
                NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT |
                NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR;
            Stats.ifHCInOctets = Adapter->RxBytes;
            Stats.ifHCInUcastPkts = Adapter->RxOk;
            Stats.ifInDiscards = Adapter->RxNoBuffer;
            Stats.ifHCOutOctets = Adapter->TxBytes;
            Stats.ifHCOutUcastPkts = Adapter->TxOk;
            Stats.ifOutErrors = Adapter->TxError;
            Source = &Stats;
            SourceLength = sizeof(Stats);
            break;

        case OID_802_3_PERMANENT_ADDRESS:
            Source = Adapter->PermanentAddress;
            SourceLength = KDNS_ADDRESS_LENGTH;
            break;

        case OID_802_3_CURRENT_ADDRESS:
            Source = Adapter->CurrentAddress;
            SourceLength = KDNS_ADDRESS_LENGTH;
            break;

        case OID_802_3_MULTICAST_LIST:
            Source = Adapter->MulticastList;
            SourceLength = Adapter->MulticastCount * KDNS_ADDRESS_LENGTH;
            break;

        case OID_802_3_MAXIMUM_LIST_SIZE:
            Value32 = KDNS_MAX_MULTICAST;
            Source = &Value32;
            SourceLength = sizeof(Value32);
            break;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    if (Query->InformationBufferLength < SourceLength)
    {
        Query->BytesNeeded = SourceLength;
        Query->BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    if (SourceLength != 0)
        NdisMoveMemory(Query->InformationBuffer, Source, SourceLength);
    Query->BytesWritten = SourceLength;
    Query->BytesNeeded = SourceLength;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
KdnsSetInformation(
    _In_ PKDNS_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST Request)
{
    struct _SET *Set = &Request->DATA.SET_INFORMATION;
    ULONG Count;

    switch (Set->Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (Set->InformationBufferLength < sizeof(ULONG))
            {
                Set->BytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            /* The adapter is always in promiscuous mode as far as the wire is
             * concerned - kdnet hands us every frame it does not want - so the
             * filter is recorded for reporting and nothing else. */
            Adapter->PacketFilter = *(PULONG)Set->InformationBuffer;
            Set->BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_GEN_CURRENT_LOOKAHEAD:
            if (Set->InformationBufferLength < sizeof(ULONG))
            {
                Set->BytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            Adapter->Lookahead = *(PULONG)Set->InformationBuffer;
            Set->BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_802_3_MULTICAST_LIST:
            if ((Set->InformationBufferLength % KDNS_ADDRESS_LENGTH) != 0)
                return NDIS_STATUS_INVALID_LENGTH;

            Count = Set->InformationBufferLength / KDNS_ADDRESS_LENGTH;
            if (Count > KDNS_MAX_MULTICAST)
            {
                Set->BytesNeeded = KDNS_MAX_MULTICAST * KDNS_ADDRESS_LENGTH;
                return NDIS_STATUS_MULTICAST_FULL;
            }

            if (Count != 0)
            {
                NdisMoveMemory(Adapter->MulticastList,
                               Set->InformationBuffer,
                               Set->InformationBufferLength);
            }
            Adapter->MulticastCount = Count;
            Set->BytesRead = Set->InformationBufferLength;
            return NDIS_STATUS_SUCCESS;

        case OID_GEN_LINK_PARAMETERS:
            /* The link belongs to the debugger; speed and duplex are not ours
             * to change.  Accepting the request keeps the stack happy. */
            Set->BytesRead = Set->InformationBufferLength;
            return NDIS_STATUS_SUCCESS;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

NDIS_STATUS
NTAPI
KdnsOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)MiniportAdapterContext;

    switch (OidRequest->RequestType)
    {
        case NdisRequestQueryInformation:
        case NdisRequestQueryStatistics:
            return KdnsQueryInformation(Adapter, OidRequest);

        case NdisRequestSetInformation:
            return KdnsSetInformation(Adapter, OidRequest);

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

VOID
NTAPI
KdnsCancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);

    /* OID requests complete inline. */
}


/* ------------------------------------------------------------------------- *
 * Lifecycle
 * ------------------------------------------------------------------------- */

static NDIS_STATUS
KdnsSetAttributes(
    _In_ PKDNS_ADAPTER Adapter)
{
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES RegAttr;
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES GenAttr;
    NDIS_STATUS Status;
    ULONG64 Speed = (ULONG64)Adapter->ShareInfo.LinkSpeedMbps * 1000000ULL;

    NdisZeroMemory(&RegAttr, sizeof(RegAttr));
    RegAttr.Header.Type =
        NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    RegAttr.Header.Revision =
        NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    RegAttr.Header.Size =
        sizeof(NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES);
    RegAttr.MiniportAdapterContext = (NDIS_HANDLE)Adapter;
    RegAttr.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND |
                             NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK;
    RegAttr.CheckForHangTimeInSeconds = 0;
    /* Internal, not PCI: we are handed the debug NIC's device node but we
     * claim none of its resources - kdnet.dll mapped them before the PnP
     * manager existed and still owns every one of them. */
    RegAttr.InterfaceType = NdisInterfaceInternal;

    Status = NdisMSetMiniportAttributes(Adapter->MiniportAdapterHandle,
                 (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&RegAttr);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("KDNETSHARE: registration attributes failed 0x%08X\n", Status);
        return Status;
    }

    NdisZeroMemory(&GenAttr, sizeof(GenAttr));
    GenAttr.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    GenAttr.Header.Revision =
        NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
    GenAttr.Header.Size = sizeof(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES);
    GenAttr.MediaType = NdisMedium802_3;
    GenAttr.PhysicalMediumType = NdisPhysicalMedium802_3;
    GenAttr.MtuSize = KDNS_MTU_SIZE;
    GenAttr.MaxXmitLinkSpeed = Speed;
    GenAttr.MaxRcvLinkSpeed = Speed;
    GenAttr.XmitLinkSpeed = Speed;
    GenAttr.RcvLinkSpeed = Speed;
    GenAttr.MediaConnectState = MediaConnectStateConnected;
    GenAttr.MediaDuplexState = MediaDuplexStateFull;
    GenAttr.LookaheadSize = KDNS_FRAME_SIZE;
    GenAttr.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                         NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                         NDIS_MAC_OPTION_NO_LOOPBACK;
    GenAttr.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED |
                                     NDIS_PACKET_TYPE_MULTICAST |
                                     NDIS_PACKET_TYPE_ALL_MULTICAST |
                                     NDIS_PACKET_TYPE_BROADCAST |
                                     NDIS_PACKET_TYPE_PROMISCUOUS;
    GenAttr.MaxMulticastListSize = KDNS_MAX_MULTICAST;
    GenAttr.MacAddressLength = KDNS_ADDRESS_LENGTH;
    NdisMoveMemory(GenAttr.PermanentMacAddress, Adapter->PermanentAddress,
                   KDNS_ADDRESS_LENGTH);
    NdisMoveMemory(GenAttr.CurrentMacAddress, Adapter->CurrentAddress,
                   KDNS_ADDRESS_LENGTH);
    GenAttr.AccessType = NET_IF_ACCESS_BROADCAST;
    GenAttr.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    GenAttr.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    GenAttr.IfType = IF_TYPE_ETHERNET_CSMACD;
    GenAttr.IfConnectorPresent = TRUE;
    GenAttr.SupportedOidList = KdnsSupportedOids;
    GenAttr.SupportedOidListLength = sizeof(KdnsSupportedOids);

    Status = NdisMSetMiniportAttributes(Adapter->MiniportAdapterHandle,
                 (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&GenAttr);
    if (Status != NDIS_STATUS_SUCCESS)
        DPRINT1("KDNETSHARE: general attributes failed 0x%08X\n", Status);

    return Status;
}

NDIS_STATUS
NTAPI
KdnsInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParams;
    NDIS_TIMER_CHARACTERISTICS TimerChars;
    KDNET_SHARE_REGISTRATION Registration;
    PKDNS_ADAPTER Adapter;
    NDIS_STATUS Status;
    NTSTATUS NtStatus;

    UNREFERENCED_PARAMETER(MiniportDriverContext);
    UNREFERENCED_PARAMETER(MiniportInitParameters);

    Adapter = NdisAllocateMemoryWithTagPriority(NdisMiniportHandle,
                                                sizeof(KDNS_ADAPTER),
                                                KDNS_TAG,
                                                NormalPoolPriority);
    if (Adapter == NULL)
        return NDIS_STATUS_RESOURCES;

    NdisZeroMemory(Adapter, sizeof(KDNS_ADAPTER));
    Adapter->MiniportAdapterHandle = NdisMiniportHandle;
    Adapter->Lookahead = KDNS_FRAME_SIZE;

    /* Ask the debugger transport what it is driving.  This is also the gate:
     * without /KDNETSHARE the query fails and we go no further, leaving the
     * device exactly as unclaimed as it was before. */
    NtStatus = KdNetShareQuery(&Adapter->ShareInfo);
    if (!NT_SUCCESS(NtStatus))
    {
        DPRINT1("KDNETSHARE: transport not sharing (0x%08X); "
                "pass /KDNETSHARE to enable\n", NtStatus);
        Status = NDIS_STATUS_NOT_SUPPORTED;
        goto Fail;
    }

    /* One adapter, one MAC.  The debugger and the OS differ by IP address, not
     * by hardware address, because there is only one piece of hardware. */
    NdisMoveMemory(Adapter->PermanentAddress, Adapter->ShareInfo.MacAddress,
                   KDNS_ADDRESS_LENGTH);
    NdisMoveMemory(Adapter->CurrentAddress, Adapter->PermanentAddress,
                   KDNS_ADDRESS_LENGTH);

    Status = KdnsSetAttributes(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    NdisZeroMemory(&PoolParams, sizeof(PoolParams));
    PoolParams.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParams.Header.Size = sizeof(NET_BUFFER_LIST_POOL_PARAMETERS);
    PoolParams.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    PoolParams.fAllocateNetBuffer = TRUE;
    PoolParams.ContextSize = 0;
    PoolParams.PoolTag = KDNS_TAG;
    PoolParams.DataSize = KDNS_FRAME_SIZE;

    Adapter->RxNblPool = NdisAllocateNetBufferListPool(NdisMiniportHandle,
                                                       &PoolParams);
    if (Adapter->RxNblPool == NULL)
    {
        Status = NDIS_STATUS_RESOURCES;
        goto Fail;
    }

    NdisZeroMemory(&TimerChars, sizeof(TimerChars));
    TimerChars.Header.Type = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
    TimerChars.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
    TimerChars.Header.Size = NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1;
    TimerChars.TimerFunction = KdnsPollTimerDpc;
    TimerChars.FunctionContext = Adapter;

    Status = NdisAllocateTimerObject(NdisMiniportHandle, &TimerChars,
                                     &Adapter->PollTimer);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    Registration.Version = KDNET_SHARE_INTERFACE_VERSION;
    Registration.Receive = KdnsShareReceive;
    Registration.Context = Adapter;

    NtStatus = KdNetShareRegister(&Registration);
    if (!NT_SUCCESS(NtStatus))
    {
        DPRINT1("KDNETSHARE: KdNetShareRegister failed 0x%08X\n", NtStatus);
        Status = (NtStatus == STATUS_NOT_SUPPORTED) ? NDIS_STATUS_NOT_SUPPORTED
                                                    : NDIS_STATUS_FAILURE;
        goto Fail;
    }
    Adapter->Registered = TRUE;

    DPRINT1("KDNETSHARE: sharing the debug adapter, MAC "
            "%02x:%02x:%02x:%02x:%02x:%02x\n",
            Adapter->PermanentAddress[0], Adapter->PermanentAddress[1],
            Adapter->PermanentAddress[2], Adapter->PermanentAddress[3],
            Adapter->PermanentAddress[4], Adapter->PermanentAddress[5]);

    return NDIS_STATUS_SUCCESS;

Fail:
    if (Adapter->PollTimer != NULL)
        NdisFreeTimerObject(Adapter->PollTimer);
    if (Adapter->RxNblPool != NULL)
        NdisFreeNetBufferListPool(Adapter->RxNblPool);
    NdisFreeMemory(Adapter, sizeof(KDNS_ADAPTER), 0);
    return Status;
}

VOID
NTAPI
KdnsHaltEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)MiniportAdapterContext;
    ULONG Slot, Spins;

    UNREFERENCED_PARAMETER(HaltAction);

    Adapter->Halting = TRUE;
    Adapter->DataPathRunning = FALSE;

    KdnsStopPolling(Adapter);

    /* After this returns, kdnet.dll guarantees KdnsShareReceive is neither
     * running nor callable again, so the adapter can be torn down. */
    if (Adapter->Registered)
    {
        KdNetShareDeregister();
        Adapter->Registered = FALSE;
    }

    /* NBLs already indicated belong to the protocol stack, and their MDLs
     * point into this allocation.  Freeing underneath them would corrupt
     * whoever still holds them, so wait for them to come back. */
    for (Spins = 0; Spins < 5000; ++Spins)
    {
        BOOLEAN Outstanding = FALSE;

        for (Slot = 0; Slot < KDNS_RX_BUFFERS; ++Slot)
        {
            if (Adapter->Rx[Slot].State == KDNS_SLOT_INDICATED)
            {
                Outstanding = TRUE;
                break;
            }
        }
        if (!Outstanding)
            break;

        NdisMSleep(1000);
    }

    if (Adapter->PollTimer != NULL)
    {
        NdisFreeTimerObject(Adapter->PollTimer);
        Adapter->PollTimer = NULL;
    }
    if (Adapter->RxNblPool != NULL)
    {
        NdisFreeNetBufferListPool(Adapter->RxNblPool);
        Adapter->RxNblPool = NULL;
    }

    NdisFreeMemory(Adapter, sizeof(KDNS_ADAPTER), 0);
}

NDIS_STATUS
NTAPI
KdnsPauseEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS MiniportPauseParameters)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)MiniportAdapterContext;

    UNREFERENCED_PARAMETER(MiniportPauseParameters);

    Adapter->DataPathRunning = FALSE;
    KdnsStopPolling(Adapter);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
KdnsRestartEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS MiniportRestartParameters)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)MiniportAdapterContext;

    UNREFERENCED_PARAMETER(MiniportRestartParameters);

    Adapter->DataPathRunning = TRUE;
    KdnsStartPolling(Adapter);

    /* The debugger already brought the link up; if it had not, kdnet.dll would
     * never have reached the state where sharing is possible. */
    KdnsIndicateLinkState(Adapter, Adapter->Registered);

    return NDIS_STATUS_SUCCESS;
}

VOID
NTAPI
KdnsShutdownEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    PKDNS_ADAPTER Adapter = (PKDNS_ADAPTER)MiniportAdapterContext;

    UNREFERENCED_PARAMETER(ShutdownAction);

    /* Stop touching the adapter, but leave kdnet.dll's registration alone:
     * a bugcheck-time shutdown is exactly when the debugger is needed most. */
    Adapter->DataPathRunning = FALSE;
}

VOID
NTAPI
KdnsDevicePnpEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(NetDevicePnPEvent);
}

VOID
NTAPI
KdnsUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (KdnsMiniportDriverHandle != NULL)
    {
        NdisMDeregisterMiniportDriver(KdnsMiniportDriverHandle);
        KdnsMiniportDriverHandle = NULL;
    }
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Chars;
    NDIS_STATUS Status;

    NdisZeroMemory(&Chars, sizeof(Chars));
    Chars.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    Chars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    Chars.Header.Size = sizeof(NDIS_MINIPORT_DRIVER_CHARACTERISTICS);
    Chars.MajorNdisVersion = 6;
    Chars.MinorNdisVersion = 20;
    Chars.MajorDriverVersion = 1;
    Chars.MinorDriverVersion = 0;

    Chars.InitializeHandlerEx = KdnsInitializeEx;
    Chars.HaltHandlerEx = KdnsHaltEx;
    Chars.UnloadHandler = KdnsUnload;
    Chars.PauseHandler = KdnsPauseEx;
    Chars.RestartHandler = KdnsRestartEx;
    Chars.OidRequestHandler = KdnsOidRequest;
    Chars.SendNetBufferListsHandler = KdnsSendNetBufferLists;
    Chars.ReturnNetBufferListsHandler = KdnsReturnNetBufferLists;
    Chars.CancelSendHandler = KdnsCancelSend;
    Chars.CheckForHangHandlerEx = NULL;
    Chars.ResetHandlerEx = NULL;
    Chars.DevicePnPEventNotifyHandler = KdnsDevicePnpEventNotify;
    Chars.ShutdownHandlerEx = KdnsShutdownEx;
    Chars.CancelOidRequestHandler = KdnsCancelOidRequest;

    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Chars, &KdnsMiniportDriverHandle);
    if (Status != NDIS_STATUS_SUCCESS)
        DPRINT1("KDNETSHARE: NdisMRegisterMiniportDriver failed 0x%08X\n", Status);

    return Status;
}
