/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NDIS 6 protocol side of the NetAdapterCx test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Opens the NetAdapterCx loopback adapter as an NDIS 6 protocol, sends
 * numbered packets through it and checks that each comes back unchanged
 * (and does the same as an NDIS 5 protocol, further below):
 *
 *   NdisSendNetBufferLists -> NDIS -> NetAdapterCx miniport -> Tx ring
 *   -> loopback client -> Rx ring -> NetAdapterCx -> NDIS -> here
 */

#include <kmt_test.h>
#include <ndis.h>

#include "KmdfNetCx.h"

#ifndef NDIS_OPEN_PARAMETERS_REVISION_1
#define NDIS_OPEN_PARAMETERS_REVISION_1 1
#endif

/* NDIS 6.0, not declared by our ndis.h yet */
NDIS_STATUS
NTAPI
NdisOpenAdapterEx(
    _In_ NDIS_HANDLE NdisProtocolHandle,
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PVOID OpenParameters,
    _In_ NDIS_HANDLE BindContext,
    _Out_ PNDIS_HANDLE NdisBindingHandle);

NDIS_STATUS
NTAPI
NdisCloseAdapterEx(
    _In_ NDIS_HANDLE NdisBindingHandle);

#define TAG_PROTO           'pCxN'
#define PROTO_MAGIC         0x5458434E /* "NCXT" */
#define PROTO_SINGLES       32
#define PROTO_MAX_PACKETS   64
#define PROTO_FRAME_MAX     2048

static NDIS_HANDLE ProtocolHandle;
static NDIS_HANDLE BindingHandle;
static NDIS_HANDLE NblPool;
static NDIS_HANDLE NbPool;
static WCHAR TargetAdapter[128];
static KEVENT OpenEvent;
static KEVENT DoneEvent;
static volatile LONG SendsOutstanding;
static volatile LONG SendsFailed;
static volatile LONG ReceivesExpected;
static volatile LONG Received;
static volatile LONG Mismatched;
static volatile LONG Unexpected;
static UCHAR SeenCount[PROTO_MAX_PACKETS];

/* A raw IPv4 looking frame: version nibble, magic, number, pattern */
static
VOID
ProtoFillFrame(
    _Out_writes_bytes_(Length) PUCHAR Data,
    _In_ ULONG Length,
    _In_ ULONG Sequence)
{
    ULONG i;

    for (i = 0; i < Length; i++)
        Data[i] = (UCHAR)(Sequence + i);
    Data[0] = 0x45;
    *(PULONG)&Data[4] = PROTO_MAGIC;
    *(PULONG)&Data[8] = Sequence;
    *(PULONG)&Data[12] = Length;
}

static
BOOLEAN
ProtoCheckFrame(
    _In_reads_bytes_(Length) PUCHAR Data,
    _In_ ULONG Length,
    _Out_ PULONG Sequence)
{
    ULONG i;

    *Sequence = MAXULONG;
    if (Length < 16 || Data[0] != 0x45 || *(PULONG)&Data[4] != PROTO_MAGIC)
        return FALSE;

    *Sequence = *(PULONG)&Data[8];
    if (*(PULONG)&Data[12] != Length)
        return FALSE;

    for (i = 16; i < Length; i++)
    {
        if (Data[i] != (UCHAR)(*Sequence + i))
            return FALSE;
    }
    return TRUE;
}

static
VOID
ProtoCheckDone(VOID)
{
    if (SendsOutstanding == 0 && Received + Mismatched >= ReceivesExpected)
        KeSetEvent(&DoneEvent, IO_NO_INCREMENT, FALSE);
}

static PROTOCOL_RECEIVE_NET_BUFFER_LISTS ProtoReceiveNetBufferLists;

static
VOID
NTAPI
ProtoReceiveNetBufferLists(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG NumberOfNetBufferLists,
    _In_ ULONG ReceiveFlags)
{
    static UCHAR Storage[PROTO_FRAME_MAX];
    PNET_BUFFER_LIST Nbl;
    PNET_BUFFER Nb;
    PUCHAR Data;
    ULONG Length, Sequence;

    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(PortNumber);
    UNREFERENCED_PARAMETER(NumberOfNetBufferLists);

    for (Nbl = NetBufferLists; Nbl != NULL; Nbl = NET_BUFFER_LIST_NEXT_NBL(Nbl))
    {
        for (Nb = NET_BUFFER_LIST_FIRST_NB(Nbl); Nb != NULL; Nb = NET_BUFFER_NEXT_NB(Nb))
        {
            Length = NET_BUFFER_DATA_LENGTH(Nb);
            Data = (Length <= sizeof(Storage)) ? NdisGetDataBuffer(Nb, Length, Storage, 1, 0) : NULL;

            if (Data != NULL && ProtoCheckFrame(Data, Length, &Sequence) &&
                Sequence < PROTO_MAX_PACKETS)
            {
                SeenCount[Sequence]++;
                InterlockedIncrement(&Received);
            }
            else if (Data != NULL && Length >= 8 && *(PULONG)&Data[4] == PROTO_MAGIC)
            {
                InterlockedIncrement(&Mismatched);
            }
            else
            {
                /* Not ours */
                InterlockedIncrement(&Unexpected);
            }
        }
    }

    if (NDIS_TEST_RECEIVE_CAN_PEND(ReceiveFlags))
        NdisReturnNetBufferLists(BindingHandle, NetBufferLists, 0);

    ProtoCheckDone();
}

static
VOID
ProtoFreeNbl(
    _In_ PNET_BUFFER_LIST Nbl)
{
    PNET_BUFFER Nb, NextNb, First = NET_BUFFER_LIST_FIRST_NB(Nbl);
    PMDL Mdl, Next;

    for (Nb = First; Nb != NULL; Nb = NextNb)
    {
        NextNb = NET_BUFFER_NEXT_NB(Nb);
        for (Mdl = NET_BUFFER_FIRST_MDL(Nb); Mdl != NULL; Mdl = Next)
        {
            Next = Mdl->Next;
            ExFreePoolWithTag(MmGetMdlVirtualAddress(Mdl), TAG_PROTO);
            IoFreeMdl(Mdl);
        }
        NET_BUFFER_FIRST_MDL(Nb) = NULL;

        /* Only the first NET_BUFFER came with the list */
        if (Nb != First)
            NdisFreeNetBuffer(Nb);
    }
    NET_BUFFER_NEXT_NB(First) = NULL;
    NdisFreeNetBufferList(Nbl);
}

static PROTOCOL_SEND_NET_BUFFER_LISTS_COMPLETE ProtoSendNetBufferListsComplete;

static
VOID
NTAPI
ProtoSendNetBufferListsComplete(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG SendCompleteFlags)
{
    PNET_BUFFER_LIST Nbl, Next;

    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(SendCompleteFlags);

    for (Nbl = NetBufferLists; Nbl != NULL; Nbl = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(Nbl);
        if (NET_BUFFER_LIST_STATUS(Nbl) != NDIS_STATUS_SUCCESS)
            InterlockedIncrement(&SendsFailed);
        ProtoFreeNbl(Nbl);
        InterlockedDecrement(&SendsOutstanding);
    }

    ProtoCheckDone();
}

/* One NET_BUFFER of Length bytes; a split puts the first Split bytes in an MDL of their own */
static
PMDL
ProtoBuildMdlChain(
    _In_ ULONG Sequence,
    _In_ ULONG Length,
    _In_ ULONG Split)
{
    PUCHAR Frame, Second;
    PMDL First, Last;

    Frame = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_PROTO);
    if (Frame == NULL)
        return NULL;
    ProtoFillFrame(Frame, Length, Sequence);

    if (Split == 0 || Split >= Length)
    {
        First = IoAllocateMdl(Frame, Length, FALSE, FALSE, NULL);
        if (First == NULL)
        {
            ExFreePoolWithTag(Frame, TAG_PROTO);
            return NULL;
        }
        MmBuildMdlForNonPagedPool(First);
        return First;
    }

    /* Separate allocation for the tail, so ProtoFreeNbl frees each MDL's buffer */
    Second = ExAllocatePoolWithTag(NonPagedPool, Length - Split, TAG_PROTO);
    if (Second == NULL)
    {
        ExFreePoolWithTag(Frame, TAG_PROTO);
        return NULL;
    }
    RtlCopyMemory(Second, Frame + Split, Length - Split);

    First = IoAllocateMdl(Frame, Split, FALSE, FALSE, NULL);
    Last = IoAllocateMdl(Second, Length - Split, FALSE, FALSE, NULL);
    if (First == NULL || Last == NULL)
    {
        if (First)
            IoFreeMdl(First);
        if (Last)
            IoFreeMdl(Last);
        ExFreePoolWithTag(Frame, TAG_PROTO);
        ExFreePoolWithTag(Second, TAG_PROTO);
        return NULL;
    }
    MmBuildMdlForNonPagedPool(First);
    MmBuildMdlForNonPagedPool(Last);
    First->Next = Last;
    return First;
}

static
PNET_BUFFER_LIST
ProtoBuildNbl(
    _In_ ULONG FirstSequence,
    _In_ ULONG Count,
    _In_reads_(Count) const ULONG *Lengths,
    _In_ ULONG Split)
{
    PNET_BUFFER_LIST Nbl;
    PNET_BUFFER Nb, Previous;
    PMDL Mdl;
    ULONG i;

    Mdl = ProtoBuildMdlChain(FirstSequence, Lengths[0], Split);
    if (Mdl == NULL)
        return NULL;

    Nbl = NdisAllocateNetBufferAndNetBufferList(NblPool, 0, 0, Mdl, 0, Lengths[0]);
    if (Nbl == NULL)
        return NULL;

    Previous = NET_BUFFER_LIST_FIRST_NB(Nbl);
    for (i = 1; i < Count; i++)
    {
        Mdl = ProtoBuildMdlChain(FirstSequence + i, Lengths[i], 0);
        Nb = Mdl ? NdisAllocateNetBuffer(NbPool, Mdl, 0, Lengths[i]) : NULL;
        if (Nb == NULL)
            break;
        NET_BUFFER_NEXT_NB(Previous) = Nb;
        Previous = Nb;
    }

    return Nbl;
}

static PROTOCOL_BIND_ADAPTER_EX ProtoBindAdapterEx;

static
NDIS_STATUS
NTAPI
ProtoBindAdapterEx(
    _In_ NDIS_HANDLE ProtocolDriverContext,
    _In_ NDIS_HANDLE BindContext,
    _In_ PNDIS_BIND_PARAMETERS BindParameters)
{
    NDIS_OPEN_PARAMETERS OpenParameters;
    NDIS_MEDIUM Medium = NETCX_TEST_MEDIUM;
    UINT SelectedMedium = 0;
    UNICODE_STRING Target;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(ProtocolDriverContext);

    RtlInitUnicodeString(&Target, TargetAdapter);
    if (BindParameters->AdapterName == NULL ||
        !RtlEqualUnicodeString(BindParameters->AdapterName, &Target, TRUE))
    {
        return NDIS_STATUS_NOT_ACCEPTED;
    }

    trace("Binding to %wZ: medium %d, mtu %lu, link speed %lu\n",
          BindParameters->AdapterName, BindParameters->MediaType,
          BindParameters->MtuSize, BindParameters->MaxXmitLinkSpeed);
    ok_eq_long((LONG)BindParameters->MediaType, (LONG)NETCX_TEST_MEDIUM);
    ok_eq_ulong(BindParameters->MtuSize, 1500UL);

    RtlZeroMemory(&OpenParameters, sizeof(OpenParameters));
    OpenParameters.Header.Type = NDIS_OBJECT_TYPE_OPEN_PARAMETERS;
    OpenParameters.Header.Revision = NDIS_OPEN_PARAMETERS_REVISION_1;
    OpenParameters.Header.Size = sizeof(OpenParameters);
    OpenParameters.AdapterName = BindParameters->AdapterName;
    OpenParameters.MediumArray = &Medium;
    OpenParameters.MediumArraySize = 1;
    OpenParameters.SelectedMediumIndex = &SelectedMedium;

    Status = NdisOpenAdapterEx(ProtocolHandle, NULL, &OpenParameters, BindContext, &BindingHandle);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    if (Status == NDIS_STATUS_SUCCESS)
        KeSetEvent(&OpenEvent, IO_NO_INCREMENT, FALSE);
    return Status;
}

static PROTOCOL_UNBIND_ADAPTER_EX ProtoUnbindAdapterEx;

static
NDIS_STATUS
NTAPI
ProtoUnbindAdapterEx(
    _In_ NDIS_HANDLE UnbindContext,
    _In_ NDIS_HANDLE ProtocolBindingContext)
{
    UNREFERENCED_PARAMETER(UnbindContext);
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    return NDIS_STATUS_SUCCESS;
}

static PROTOCOL_OPEN_ADAPTER_COMPLETE_EX ProtoOpenAdapterCompleteEx;

static
VOID
NTAPI
ProtoOpenAdapterCompleteEx(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(Status);
}

static PROTOCOL_CLOSE_ADAPTER_COMPLETE_EX ProtoCloseAdapterCompleteEx;

static
VOID
NTAPI
ProtoCloseAdapterCompleteEx(
    _In_ NDIS_HANDLE ProtocolBindingContext)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
}

static PROTOCOL_NET_PNP_EVENT ProtoNetPnPEvent;

static
NDIS_STATUS
NTAPI
ProtoNetPnPEvent(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PVOID NetPnPEventNotification)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(NetPnPEventNotification);
    return NDIS_STATUS_SUCCESS;
}

static PROTOCOL_OID_REQUEST_COMPLETE ProtoOidRequestComplete;

static
VOID
NTAPI
ProtoOidRequestComplete(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(OidRequest);
    UNREFERENCED_PARAMETER(Status);
}

static PROTOCOL_STATUS_EX ProtoStatusEx;

static
VOID
NTAPI
ProtoStatusEx(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(StatusIndication);
}

static
VOID
ProtoSendAll(VOID)
{
    static const ULONG Triple[3] = { 100, 200, 300 };
    PNET_BUFFER_LIST Nbls[PROTO_SINGLES + 2];
    ULONG Length, Count = 0, i;

    /* Thirty-two single frames, 64 to 1304 bytes */
    for (i = 0; i < PROTO_SINGLES; i++)
    {
        Length = 64 + i * 40;
        Nbls[Count] = ProtoBuildNbl(i, 1, &Length, 0);
        ok(Nbls[Count] != NULL, "Frame %lu not built\n", i);
        if (Nbls[Count])
            Count++;
    }

    /* One NET_BUFFER_LIST with three NET_BUFFERs: three packets */
    Nbls[Count] = ProtoBuildNbl(40, 3, Triple, 0);
    ok(Nbls[Count] != NULL, "Triple not built\n");
    if (Nbls[Count])
        Count++;

    /* One frame in two MDLs: one packet with two fragments */
    Length = 500;
    Nbls[Count] = ProtoBuildNbl(50, 1, &Length, 60);
    ok(Nbls[Count] != NULL, "Split frame not built\n");
    if (Nbls[Count])
        Count++;

    ReceivesExpected = PROTO_SINGLES + 3 + 1;
    SendsOutstanding = (LONG)Count;

    for (i = 0; i < Count; i++)
        NdisSendNetBufferLists(BindingHandle, Nbls[i], NDIS_DEFAULT_PORT_NUMBER, 0);
}

static
VOID
ProtoRunTest(VOID)
{
    NDIS_PROTOCOL_DRIVER_CHARACTERISTICS Characteristics;
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParameters;
    NET_BUFFER_POOL_PARAMETERS NbPoolParameters;
    NDIS_STRING Name = RTL_CONSTANT_STRING(L"KmtNetCxProto");
    LARGE_INTEGER Timeout;
    NDIS_STATUS Status;
    ULONG i;

    KeInitializeEvent(&OpenEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&DoneEvent, NotificationEvent, FALSE);
    BindingHandle = NULL;
    SendsOutstanding = SendsFailed = 0;
    ReceivesExpected = Received = Mismatched = Unexpected = 0;
    RtlZeroMemory(SeenCount, sizeof(SeenCount));

    RtlZeroMemory(&PoolParameters, sizeof(PoolParameters));
    PoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParameters.Header.Size = sizeof(PoolParameters);
    PoolParameters.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    PoolParameters.fAllocateNetBuffer = TRUE;
    PoolParameters.PoolTag = TAG_PROTO;
    NblPool = NdisAllocateNetBufferListPool(NULL, &PoolParameters);
    ok(NblPool != NULL, "No NET_BUFFER_LIST pool\n");
    if (NblPool == NULL)
        return;

    RtlZeroMemory(&NbPoolParameters, sizeof(NbPoolParameters));
    NbPoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    NbPoolParameters.Header.Revision = NET_BUFFER_POOL_PARAMETERS_REVISION_1;
    NbPoolParameters.Header.Size = sizeof(NbPoolParameters);
    NbPoolParameters.PoolTag = TAG_PROTO;
    NbPool = NdisAllocateNetBufferPool(NULL, &NbPoolParameters);
    ok(NbPool != NULL, "No NET_BUFFER pool\n");
    if (NbPool == NULL)
    {
        NdisFreeNetBufferListPool(NblPool);
        return;
    }

    RtlZeroMemory(&Characteristics, sizeof(Characteristics));
    Characteristics.Header.Type = NDIS_OBJECT_TYPE_PROTOCOL_DRIVER_CHARACTERISTICS;
    Characteristics.Header.Revision = NDIS_PROTOCOL_DRIVER_CHARACTERISTICS_REVISION_1;
    Characteristics.Header.Size = sizeof(Characteristics);
    Characteristics.MajorNdisVersion = 6;
    Characteristics.MinorNdisVersion = 0;
    Characteristics.Name = Name;
    Characteristics.BindAdapterHandlerEx = ProtoBindAdapterEx;
    Characteristics.UnbindAdapterHandlerEx = ProtoUnbindAdapterEx;
    Characteristics.OpenAdapterCompleteHandlerEx = ProtoOpenAdapterCompleteEx;
    Characteristics.CloseAdapterCompleteHandlerEx = ProtoCloseAdapterCompleteEx;
    Characteristics.NetPnPEventHandler = ProtoNetPnPEvent;
    Characteristics.OidRequestCompleteHandler = ProtoOidRequestComplete;
    Characteristics.StatusHandlerEx = ProtoStatusEx;
    Characteristics.ReceiveNetBufferListsHandler = ProtoReceiveNetBufferLists;
    Characteristics.SendNetBufferListsCompleteHandler = ProtoSendNetBufferListsComplete;

    /* Binds to the adapters NDIS already has, our target among them */
    Status = NdisRegisterProtocolDriver(NULL, &Characteristics, &ProtocolHandle);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    if (Status != NDIS_STATUS_SUCCESS)
        goto FreePool;

    Timeout.QuadPart = -10LL * 1000 * 1000 * 5;
    Status = KeWaitForSingleObject(&OpenEvent, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(BindingHandle != NULL, "Adapter %S not opened\n", TargetAdapter);
    if (BindingHandle == NULL)
        goto Deregister;

    ProtoSendAll();

    Timeout.QuadPart = -10LL * 1000 * 1000 * 10;
    Status = KeWaitForSingleObject(&DoneEvent, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);

    ok_eq_long(SendsOutstanding, 0L);
    ok_eq_long(SendsFailed, 0L);
    ok_eq_long(Received, ReceivesExpected);
    ok_eq_long(Mismatched, 0L);
    trace("Sent %ld frames, received %ld, %ld foreign frames\n",
          ReceivesExpected, Received, Unexpected);

    for (i = 0; i < PROTO_SINGLES; i++)
        ok(SeenCount[i] == 1, "Frame %lu seen %u times\n", i, SeenCount[i]);
    for (i = 40; i < 43; i++)
        ok(SeenCount[i] == 1, "Frame %lu seen %u times\n", i, SeenCount[i]);
    ok(SeenCount[50] == 1, "Split frame seen %u times\n", SeenCount[50]);

    /* Unfinished sends would complete into freed state */
    if (SendsOutstanding != 0)
    {
        Timeout.QuadPart = -10LL * 1000 * 1000 * 10;
        KeWaitForSingleObject(&DoneEvent, Executive, KernelMode, FALSE, &Timeout);
    }

    /*
     * Our NDIS does not yet wait for protocol callbacks still running when
     * the binding closes. The last one set DoneEvent and is returning:
     * give it time to leave this driver before it can be unloaded.
     */
    Timeout.QuadPart = -10LL * 1000 * 200;
    KeDelayExecutionThread(KernelMode, FALSE, &Timeout);

    Status = NdisCloseAdapterEx(BindingHandle);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    BindingHandle = NULL;

Deregister:
    NdisDeregisterProtocolDriver(ProtocolHandle);
    ProtocolHandle = NULL;

FreePool:
    if (SendsOutstanding == 0)
    {
        NdisFreeNetBufferPool(NbPool);
        NdisFreeNetBufferListPool(NblPool);
    }
    NbPool = NULL;
    NblPool = NULL;
}

/*
 * NDIS 5 side: what tcpip sees of an NDIS 6 adapter. Two bindings of one
 * legacy protocol open the adapter; NDIS wraps every received frame in an
 * NDIS_PACKET and indicates it to both. The first binding keeps the packet.
 *
 * First the second binding gives the first one's packet back while the
 * indication is still going on, the way a protocol that keeps a packet
 * for a worker thread can see it returned from another processor before
 * NDIS is done indicating it: the packet must still go back once, not
 * once from NdisReturnPackets and again when the indication ends. Then
 * the first binding keeps a few packets at a time: NDIS must not hand
 * out one that is still kept, which it does once a packet went back to
 * its pool twice.
 */
#define LEGACY_EARLY_FRAMES     8
#define LEGACY_HELD_FRAMES      8
#define LEGACY_FRAMES           (LEGACY_EARLY_FRAMES + LEGACY_HELD_FRAMES)
#define LEGACY_FRAME_MAX        256
#define LEGACY_SEQUENCE_BASE    100

static NDIS_HANDLE LegacyProtocolHandle;
static NDIS_HANDLE LegacyBinding[2];
static NDIS_HANDLE LegacyPacketPool;
static NDIS_HANDLE LegacyBufferPool;
static KSPIN_LOCK LegacyLock;
static PNDIS_PACKET LegacyHeld[LEGACY_FRAMES];
static BOOLEAN LegacyReturnEarly;
static volatile LONG LegacyReceived;
static volatile LONG LegacyEarlyReturns;
static volatile LONG LegacyKeptTwice;
static volatile LONG LegacyMismatched;
static volatile LONG LegacySendsOutstanding;

/* Copies the start of the packet's data; returns its total length */
static
ULONG
LegacyCopyPacket(
    _In_ PNDIS_PACKET Packet,
    _Out_writes_bytes_(Size) PUCHAR Data,
    _In_ ULONG Size)
{
    PNDIS_BUFFER Buffer;
    UINT Total, Length, Copied = 0;
    PVOID Va;

    NdisQueryPacket(Packet, NULL, NULL, &Buffer, &Total);
    while (Buffer != NULL && Copied < Size)
    {
        NdisQueryBufferSafe(Buffer, &Va, &Length, NormalPagePriority);
        if (Va == NULL)
            break;
        Length = min(Length, Size - Copied);
        RtlCopyMemory(Data + Copied, Va, Length);
        Copied += Length;
        NdisGetNextBuffer(Buffer, &Buffer);
    }
    return Total;
}

/* Our frame, intact: returns its index, or MAXULONG */
static
ULONG
LegacyFrameIndex(
    _In_ PNDIS_PACKET Packet,
    _Out_ PBOOLEAN Damaged)
{
    UCHAR Data[LEGACY_FRAME_MAX];
    ULONG Length, Sequence;

    *Damaged = FALSE;
    Length = LegacyCopyPacket(Packet, Data, sizeof(Data));
    if (Length < 16 || Length > sizeof(Data) || *(PULONG)&Data[4] != PROTO_MAGIC)
        return MAXULONG;

    if (!ProtoCheckFrame(Data, Length, &Sequence) ||
        Sequence < LEGACY_SEQUENCE_BASE ||
        Sequence >= LEGACY_SEQUENCE_BASE + LEGACY_FRAMES)
    {
        *Damaged = TRUE;
        return MAXULONG;
    }
    return Sequence - LEGACY_SEQUENCE_BASE;
}

static
INT
NTAPI
LegacyReceivePacket(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNDIS_PACKET Packet)
{
    ULONG Binding = (ULONG)(ULONG_PTR)ProtocolBindingContext;
    PNDIS_PACKET Return = NULL;
    BOOLEAN Damaged;
    KIRQL OldIrql;
    ULONG Index, i;
    INT Kept = 0;

    Index = LegacyFrameIndex(Packet, &Damaged);
    if (Damaged)
        InterlockedIncrement(&LegacyMismatched);
    if (Index == MAXULONG)
        return 0;

    KeAcquireSpinLock(&LegacyLock, &OldIrql);
    if (Binding == 0)
    {
        /* A packet we keep must not come back in another indication */
        for (i = 0; i < LEGACY_FRAMES; i++)
        {
            if (LegacyHeld[i] == Packet)
                InterlockedIncrement(&LegacyKeptTwice);
        }

        if (LegacyHeld[Index] == NULL)
        {
            LegacyHeld[Index] = Packet;
            InterlockedIncrement(&LegacyReceived);
            Kept = 1;
        }
    }
    else if (LegacyReturnEarly && LegacyHeld[Index] == Packet)
    {
        /* Give back what the first binding keeps, before the indication ends */
        LegacyHeld[Index] = NULL;
        Return = Packet;
    }
    KeReleaseSpinLock(&LegacyLock, OldIrql);

    if (Return != NULL)
    {
        NdisReturnPackets(&Return, 1);
        InterlockedIncrement(&LegacyEarlyReturns);

        /*
         * Other protocols bound to the adapter (tcpip) keep the packet too,
         * and return it from a worker thread: give them the time to, so
         * the packet has no references left but the indication's own.
         */
        for (i = 0; i < 20; i++)
            KeStallExecutionProcessor(500);
    }

    return Kept;
}

static
NDIS_STATUS
NTAPI
LegacyReceive(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ NDIS_HANDLE MacReceiveContext,
    _In_ PVOID HeaderBuffer,
    _In_ UINT HeaderBufferSize,
    _In_ PVOID LookaheadBuffer,
    _In_ UINT LookaheadBufferSize,
    _In_ UINT PacketSize)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(MacReceiveContext);
    UNREFERENCED_PARAMETER(HeaderBuffer);
    UNREFERENCED_PARAMETER(HeaderBufferSize);
    UNREFERENCED_PARAMETER(LookaheadBuffer);
    UNREFERENCED_PARAMETER(LookaheadBufferSize);
    UNREFERENCED_PARAMETER(PacketSize);
    return NDIS_STATUS_NOT_ACCEPTED;
}

static
VOID
LegacyFreeSendPacket(
    _In_ PNDIS_PACKET Packet)
{
    PNDIS_BUFFER Buffer;
    PVOID Va;
    UINT Length;

    NdisUnchainBufferAtFront(Packet, &Buffer);
    if (Buffer != NULL)
    {
        NdisQueryBufferSafe(Buffer, &Va, &Length, NormalPagePriority);
        NdisFreeBuffer(Buffer);
        if (Va != NULL)
            ExFreePoolWithTag(Va, TAG_PROTO);
    }
    NdisFreePacket(Packet);
}

static
VOID
NTAPI
LegacySendComplete(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNDIS_PACKET Packet,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    LegacyFreeSendPacket(Packet);
    InterlockedDecrement(&LegacySendsOutstanding);
}

static
VOID
NTAPI
LegacyOpenAdapterComplete(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ NDIS_STATUS Status,
    _In_ NDIS_STATUS OpenErrorStatus)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(OpenErrorStatus);
}

static
VOID
NTAPI
LegacyStatusOnly(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(Status);
}

static
VOID
NTAPI
LegacyRequestComplete(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNDIS_REQUEST NdisRequest,
    _In_ NDIS_STATUS Status)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(NdisRequest);
    UNREFERENCED_PARAMETER(Status);
}

static
VOID
NTAPI
LegacyTransferDataComplete(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNDIS_PACKET Packet,
    _In_ NDIS_STATUS Status,
    _In_ UINT BytesTransferred)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(Packet);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(BytesTransferred);
}

static
VOID
NTAPI
LegacyBindingOnly(
    _In_ NDIS_HANDLE ProtocolBindingContext)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
}

static
VOID
NTAPI
LegacyStatus(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ NDIS_STATUS GeneralStatus,
    _In_ PVOID StatusBuffer,
    _In_ UINT StatusBufferSize)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(GeneralStatus);
    UNREFERENCED_PARAMETER(StatusBuffer);
    UNREFERENCED_PARAMETER(StatusBufferSize);
}

/* Opens our target adapter twice, nothing else */
static
VOID
NTAPI
LegacyBindAdapter(
    _Out_ PNDIS_STATUS Status,
    _In_ NDIS_HANDLE BindContext,
    _In_ PNDIS_STRING DeviceName,
    _In_ PVOID SystemSpecific1,
    _In_ PVOID SystemSpecific2)
{
    NDIS_MEDIUM Medium = (NDIS_MEDIUM)NETCX_TEST_MEDIUM;
    NDIS_STATUS OpenError;
    UNICODE_STRING Target;
    UINT SelectedMedium;
    ULONG i;

    UNREFERENCED_PARAMETER(BindContext);
    UNREFERENCED_PARAMETER(SystemSpecific1);
    UNREFERENCED_PARAMETER(SystemSpecific2);

    RtlInitUnicodeString(&Target, TargetAdapter);
    if (DeviceName == NULL || !RtlEqualUnicodeString(DeviceName, &Target, TRUE) ||
        LegacyBinding[0] != NULL)
    {
        *Status = NDIS_STATUS_NOT_ACCEPTED;
        return;
    }

    for (i = 0; i < 2; i++)
    {
        SelectedMedium = MAXULONG;
        NdisOpenAdapter(Status, &OpenError, &LegacyBinding[i], &SelectedMedium,
                        &Medium, 1, LegacyProtocolHandle, (NDIS_HANDLE)(ULONG_PTR)i,
                        DeviceName, 0, NULL);
        ok_eq_hex(*Status, NDIS_STATUS_SUCCESS);
        ok_eq_uint(SelectedMedium, 0);
        if (*Status != NDIS_STATUS_SUCCESS)
        {
            LegacyBinding[i] = NULL;
            return;
        }
    }
}

static
VOID
NTAPI
LegacyUnbindAdapter(
    _Out_ PNDIS_STATUS Status,
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ NDIS_HANDLE UnbindContext)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(UnbindContext);
    *Status = NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NTAPI
LegacyPnPEvent(
    _In_ NDIS_HANDLE ProtocolBindingContext,
    _In_ PNET_PNP_EVENT NetPnPEvent)
{
    UNREFERENCED_PARAMETER(ProtocolBindingContext);
    UNREFERENCED_PARAMETER(NetPnPEvent);
    return NDIS_STATUS_SUCCESS;
}

static
VOID
NTAPI
LegacyUnload(VOID)
{
}

/* Sends frame Index through the first binding; the adapter loops it back */
static
BOOLEAN
LegacySend(
    _In_ ULONG Index)
{
    ULONG Length = 64 + Index * 8;
    PNDIS_PACKET Packet;
    PNDIS_BUFFER Buffer;
    NDIS_STATUS Status;
    PUCHAR Frame;

    Frame = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_PROTO);
    if (Frame == NULL)
        return FALSE;
    ProtoFillFrame(Frame, Length, LEGACY_SEQUENCE_BASE + Index);

    NdisAllocatePacket(&Status, &Packet, LegacyPacketPool);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        ExFreePoolWithTag(Frame, TAG_PROTO);
        return FALSE;
    }
    NdisAllocateBuffer(&Status, &Buffer, LegacyBufferPool, Frame, Length);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NdisFreePacket(Packet);
        ExFreePoolWithTag(Frame, TAG_PROTO);
        return FALSE;
    }
    NdisChainBufferAtFront(Packet, Buffer);

    InterlockedIncrement(&LegacySendsOutstanding);
    NdisSend(&Status, LegacyBinding[0], Packet);
    if (Status != NDIS_STATUS_PENDING)
    {
        ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
        LegacyFreeSendPacket(Packet);
        InterlockedDecrement(&LegacySendsOutstanding);
    }
    return TRUE;
}

/* Waits for Count frames to be received, and for the sends to complete */
static
BOOLEAN
LegacyWaitReceived(
    _In_ LONG Count)
{
    LARGE_INTEGER Interval;
    ULONG i;

    Interval.QuadPart = -10LL * 1000 * 10;
    for (i = 0; i < 500; i++)
    {
        if (LegacyReceived >= Count && LegacySendsOutstanding == 0)
            return TRUE;
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }
    return FALSE;
}

static
VOID
LegacyReturnHeld(VOID)
{
    PNDIS_PACKET Packet;
    KIRQL OldIrql;
    ULONG i;

    for (i = 0; i < LEGACY_FRAMES; i++)
    {
        KeAcquireSpinLock(&LegacyLock, &OldIrql);
        Packet = LegacyHeld[i];
        LegacyHeld[i] = NULL;
        KeReleaseSpinLock(&LegacyLock, OldIrql);
        if (Packet != NULL)
            NdisReturnPackets(&Packet, 1);
    }
}

static
VOID
LegacyRunTest(VOID)
{
    NDIS_PROTOCOL_CHARACTERISTICS Characteristics;
    NDIS_STRING Name = RTL_CONSTANT_STRING(L"KmtNetCxProto5");
    NDIS_STATUS Status;
    BOOLEAN Damaged;
    ULONG i, j;

    KeInitializeSpinLock(&LegacyLock);
    RtlZeroMemory(LegacyHeld, sizeof(LegacyHeld));
    LegacyBinding[0] = LegacyBinding[1] = NULL;
    LegacyReturnEarly = TRUE;
    LegacyReceived = LegacyEarlyReturns = LegacyKeptTwice = 0;
    LegacyMismatched = LegacySendsOutstanding = 0;

    NdisAllocatePacketPool(&Status, &LegacyPacketPool, LEGACY_FRAMES, 4 * sizeof(PVOID));
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    if (Status != NDIS_STATUS_SUCCESS)
        return;
    NdisAllocateBufferPool(&Status, &LegacyBufferPool, LEGACY_FRAMES);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);

    RtlZeroMemory(&Characteristics, sizeof(Characteristics));
    Characteristics.MajorNdisVersion = 4;
    Characteristics.MinorNdisVersion = 0;
    Characteristics.Name = Name;
    Characteristics.OpenAdapterCompleteHandler = LegacyOpenAdapterComplete;
    Characteristics.CloseAdapterCompleteHandler = LegacyStatusOnly;
    Characteristics.SendCompleteHandler = LegacySendComplete;
    Characteristics.TransferDataCompleteHandler = LegacyTransferDataComplete;
    Characteristics.ResetCompleteHandler = LegacyStatusOnly;
    Characteristics.RequestCompleteHandler = LegacyRequestComplete;
    Characteristics.ReceiveHandler = LegacyReceive;
    Characteristics.ReceiveCompleteHandler = LegacyBindingOnly;
    Characteristics.StatusHandler = LegacyStatus;
    Characteristics.StatusCompleteHandler = LegacyBindingOnly;
    Characteristics.ReceivePacketHandler = LegacyReceivePacket;
    Characteristics.BindAdapterHandler = LegacyBindAdapter;
    Characteristics.UnbindAdapterHandler = LegacyUnbindAdapter;
    Characteristics.PnPEventHandler = LegacyPnPEvent;
    Characteristics.UnloadHandler = LegacyUnload;

    NdisRegisterProtocol(&Status, &LegacyProtocolHandle, &Characteristics, sizeof(Characteristics));
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        LegacyProtocolHandle = NULL;
        goto FreePools;
    }

    /* NDIS binds us to its adapters as we register */
    ok(LegacyBinding[0] != NULL && LegacyBinding[1] != NULL, "Adapter %S not opened twice\n", TargetAdapter);
    if (LegacyBinding[0] == NULL || LegacyBinding[1] == NULL)
        goto Close;

    /* The second binding returns the packets the first one keeps */
    for (i = 0; i < LEGACY_EARLY_FRAMES; i++)
    {
        ok(LegacySend(i), "Frame %lu not sent\n", i);
        ok(LegacyWaitReceived((LONG)i + 1), "Frame %lu not received\n", i);
    }
    ok_eq_long(LegacyReceived, (LONG)LEGACY_EARLY_FRAMES);
    ok_eq_long(LegacyEarlyReturns, (LONG)LEGACY_EARLY_FRAMES);
    for (i = 0; i < LEGACY_EARLY_FRAMES; i++)
        ok(LegacyHeld[i] == NULL, "Frame %lu still kept\n", i);

    /* The first binding keeps them all */
    LegacyReturnEarly = FALSE;
    for (i = LEGACY_EARLY_FRAMES; i < LEGACY_FRAMES; i++)
    {
        ok(LegacySend(i), "Frame %lu not sent\n", i);
        ok(LegacyWaitReceived((LONG)i + 1), "Frame %lu not received\n", i);
    }
    ok_eq_long(LegacyReceived, (LONG)LEGACY_FRAMES);
    ok_eq_long(LegacyKeptTwice, 0L);
    for (i = LEGACY_EARLY_FRAMES; i < LEGACY_FRAMES; i++)
    {
        ok(LegacyHeld[i] != NULL, "Frame %lu not kept\n", i);
        if (LegacyHeld[i] == NULL)
            continue;
        for (j = i + 1; j < LEGACY_FRAMES; j++)
            ok(LegacyHeld[i] != LegacyHeld[j], "Frames %lu and %lu in one packet %p\n", i, j, LegacyHeld[i]);
        /* Nothing was received into a packet we keep */
        ok_eq_ulong(LegacyFrameIndex(LegacyHeld[i], &Damaged), i);
        ok(!Damaged, "Frame %lu damaged\n", i);
    }
    ok_eq_long(LegacyMismatched, 0L);
    trace("Legacy protocol: %ld frames received, %ld returned early\n",
          LegacyReceived, LegacyEarlyReturns);

    LegacyReturnHeld();

Close:
    for (i = 0; i < 2; i++)
    {
        if (LegacyBinding[i] != NULL)
        {
            NdisCloseAdapter(&Status, LegacyBinding[i]);
            ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
            LegacyBinding[i] = NULL;
        }
    }
    NdisDeregisterProtocol(&Status, LegacyProtocolHandle);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    LegacyProtocolHandle = NULL;

FreePools:
    if (LegacySendsOutstanding == 0)
    {
        NdisFreeBufferPool(LegacyBufferPool);
        NdisFreePacketPool(LegacyPacketPool);
    }
    LegacyBufferPool = NULL;
    LegacyPacketPool = NULL;
}

static
NTSTATUS
TestMessageHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(OutLength);

    switch (ControlCode)
    {
        case IOCTL_NETCXPROTO_SET_ADAPTER:
            if (Buffer == NULL || InLength == 0 || InLength >= sizeof(TargetAdapter))
                return STATUS_INVALID_PARAMETER;
            RtlZeroMemory(TargetAdapter, sizeof(TargetAdapter));
            RtlCopyMemory(TargetAdapter, Buffer, InLength);
            return STATUS_SUCCESS;

        case IOCTL_NETCXPROTO_RUN:
            ProtoRunTest();
            return STATUS_SUCCESS;

        case IOCTL_NETCXPROTO_RUN_LEGACY:
            LegacyRunTest();
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"NetCxProto";

    Status = KmtRegisterMessageHandler(IOCTL_NETCXPROTO_SET_ADAPTER, NULL, TestMessageHandler);
    if (NT_SUCCESS(Status))
        Status = KmtRegisterMessageHandler(IOCTL_NETCXPROTO_RUN, NULL, TestMessageHandler);
    if (NT_SUCCESS(Status))
        Status = KmtRegisterMessageHandler(IOCTL_NETCXPROTO_RUN_LEGACY, NULL, TestMessageHandler);
    return Status;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(DriverObject);
}
