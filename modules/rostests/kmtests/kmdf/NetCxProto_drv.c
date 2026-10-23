/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NDIS 6 protocol side of the NetAdapterCx test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Opens the NetAdapterCx loopback adapter as an NDIS 6 protocol, sends
 * numbered packets through it and checks that each comes back unchanged:
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
    return Status;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(DriverObject);
}
