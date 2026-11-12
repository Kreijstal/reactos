/*
 * PROJECT:     ReactOS TCP/IP protocol driver
 * LICENCE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ICMP functions implementation
 * COPYRIGHT:   2019 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include "precomp.h"
#include <checksum.h>

/* Silence the TCPIP-* trace prints below by default; comment out the
 * "#define NDEBUG" to re-enable. */
#define NDEBUG
#undef UNIMPLEMENTED
#include <reactos/debug.h>

/*
 * An echo request in flight. Replies are matched to it by the identifier and
 * sequence number of the echo request they answer (an echo reply carries
 * them in its own header, an ICMP error in the copy of the request it
 * quotes), not by address file: every raw ICMP address file receives every
 * ICMP datagram, so a request must never take a reply meant for another one,
 * including late replies to requests that already ended.
 */
typedef struct _ICMP_PACKET_CONTEXT
{
    /* On IcmpEchoRequestList while Accepting, protected by IcmpEchoRequestLock */
    LIST_ENTRY ListEntry;
    BOOLEAN Accepting;
    BOOLEAN TimerSet;
    UINT16 Identifier;
    UINT16 Sequence;
    PUCHAR CurrentReply;
    UINT32 RemainingSize;
    LONG nReplies;

    TDI_REQUEST TdiRequest;
    KDPC TimeoutDpc;
    KTIMER TimeoutTimer;
    LARGE_INTEGER TimerResolution;
    INT64 StartTicks;
    PIRP Irp;
    PIO_WORKITEM FinishWorker;
} ICMP_PACKET_CONTEXT, *PICMP_PACKET_CONTEXT;

/*
 * The requests that still take replies. A request leaves the list when its
 * reply buffer is full or when it times out, under this lock, and exactly
 * one of those two (or DispEchoRequest, when the buffer filled up before the
 * timer was set) queues EndRequestHandler, which alone frees the request.
 */
static LIST_ENTRY IcmpEchoRequestList = { &IcmpEchoRequestList, &IcmpEchoRequestList };
static KSPIN_LOCK IcmpEchoRequestLock;

static volatile INT16 IcmpSequence = 0;


static
UINT32
GetReplyStatus(PICMP_HEADER IcmpHeader)
{
    switch (IcmpHeader->Type)
    {
        case ICMP_TYPE_ECHO_REPLY:
            return IP_SUCCESS;
        case ICMP_TYPE_DEST_UNREACH:
            switch (IcmpHeader->Code)
            {
                case ICMP_CODE_DU_NET_UNREACH:
                    return IP_DEST_NET_UNREACHABLE;
                case ICMP_CODE_DU_HOST_UNREACH:
                    return IP_DEST_HOST_UNREACHABLE;
                case ICMP_CODE_DU_PROTOCOL_UNREACH:
                    return IP_DEST_PROT_UNREACHABLE;
                case ICMP_CODE_DU_PORT_UNREACH:
                    return IP_DEST_PORT_UNREACHABLE;
                case ICMP_CODE_DU_FRAG_DF_SET:
                    return IP_DEST_NET_UNREACHABLE;
                case ICMP_CODE_DU_SOURCE_ROUTE_FAILED:
                    return IP_BAD_ROUTE;
                default:
                    return IP_DEST_NET_UNREACHABLE;
            }
        case ICMP_TYPE_SOURCE_QUENCH:
            return IP_SOURCE_QUENCH;
        case ICMP_TYPE_TIME_EXCEEDED:
            if (IcmpHeader->Code == ICMP_CODE_TE_REASSEMBLY)
                return IP_TTL_EXPIRED_REASSEM;
            else
                return IP_TTL_EXPIRED_TRANSIT;
        case ICMP_TYPE_PARAMETER:
            return IP_PARAM_PROBLEM;
        default:
            return IP_REQ_TIMED_OUT;
    }
}

/*
 * Get the identifier and the sequence number of the echo request an ICMP
 * message answers, if it answers one
 */
static
BOOLEAN
GetEchoRequestKey(
    _In_ PICMP_HEADER IcmpHeader,
    _In_ UINT32 IcmpSize,
    _Out_ PUINT16 Identifier,
    _Out_ PUINT16 Sequence)
{
    PIPv4_HEADER Original;
    PICMP_HEADER OriginalIcmp;
    UINT32 OriginalHeaderSize;

    ASSERT(IcmpSize >= sizeof(ICMP_HEADER));

    switch (IcmpHeader->Type)
    {
        case ICMP_TYPE_ECHO_REPLY:
            *Identifier = IcmpHeader->Identifier;
            *Sequence = IcmpHeader->Seq;
            return TRUE;

        case ICMP_TYPE_DEST_UNREACH:
        case ICMP_TYPE_SOURCE_QUENCH:
        case ICMP_TYPE_TIME_EXCEEDED:
        case ICMP_TYPE_PARAMETER:
            /* An error quotes the IP header of the datagram that caused it
             * and at least the first 8 bytes of its payload: for an echo
             * request, its ICMP header */
            if (IcmpSize < sizeof(ICMP_HEADER) + sizeof(IPv4_HEADER))
                return FALSE;

            Original = (PIPv4_HEADER)(IcmpHeader + 1);
            OriginalHeaderSize = (Original->VerIHL & 0x0F) * 4;
            if (OriginalHeaderSize < sizeof(IPv4_HEADER) ||
                IcmpSize < sizeof(ICMP_HEADER) + OriginalHeaderSize + sizeof(ICMP_HEADER) ||
                Original->Protocol != IPPROTO_ICMP)
            {
                return FALSE;
            }

            OriginalIcmp = (PICMP_HEADER)((PUCHAR)Original + OriginalHeaderSize);
            if (OriginalIcmp->Type != ICMP_TYPE_ECHO_REQUEST)
                return FALSE;

            *Identifier = OriginalIcmp->Identifier;
            *Sequence = OriginalIcmp->Seq;
            return TRUE;

        default:
            return FALSE;
    }
}

/* Store a reply in the request's buffer. Called with IcmpEchoRequestLock held */
static
VOID
StoreReply(
    _Inout_ PICMP_PACKET_CONTEXT Context,
    _In_ PIP_PACKET IPPacket)
{
    PIPv4_HEADER IpHeader = IPPacket->Header;
    PICMP_HEADER IcmpHeader = IPPacket->Data;
    PUCHAR Data = (PUCHAR)(IcmpHeader + 1);
    UINT32 DataSize = IPPacket->TotalSize - IPPacket->HeaderSize - sizeof(ICMP_HEADER);
    PUCHAR Options = (PUCHAR)(IpHeader + 1);
    UINT32 OptionsSize = IPPacket->HeaderSize - sizeof(IPv4_HEADER);
    PUCHAR SystemBuffer = Context->Irp->AssociatedIrp.SystemBuffer;
    PUCHAR UserBuffer = Context->Irp->UserBuffer;
    PICMP_ECHO_REPLY Reply = (PICMP_ECHO_REPLY)Context->CurrentReply;
    UINT32 Size;
    PUCHAR Target;

    ASSERT(Context->Accepting);
    ASSERT(Context->RemainingSize >= sizeof(ICMP_ECHO_REPLY));

    Reply->Address = IpHeader->SrcAddr;
    Reply->Status = GetReplyStatus(IcmpHeader);
    Reply->RoundTripTime = (UINT32)((KeQueryPerformanceCounter(NULL).QuadPart - Context->StartTicks) * 1000 /
                                    Context->TimerResolution.QuadPart);
    Reply->Reserved = 0;
    Reply->Data = NULL;
    Reply->DataSize = 0;
    Reply->Options.Ttl = IpHeader->Ttl;
    Reply->Options.Tos = IpHeader->Tos;
    Reply->Options.Flags = WN2H(IpHeader->FlagsFragOfs) >> 13;
    Reply->Options.OptionsData = NULL;
    Reply->Options.OptionsSize = 0;

    Context->RemainingSize -= sizeof(ICMP_ECHO_REPLY);
    Context->CurrentReply += sizeof(ICMP_ECHO_REPLY);

    /* The reply structures grow from the start of the buffer, their data
     * and options from its end */
    Size = min(Context->RemainingSize, DataSize);
    if (Size > 0)
    {
        Context->RemainingSize -= Size;
        Target = Context->CurrentReply + Context->RemainingSize;
        RtlCopyMemory(Target, Data, Size);
        Reply->Data = UserBuffer + (Target - SystemBuffer);
        Reply->DataSize = (USHORT)Size;
    }

    Size = min(Context->RemainingSize, OptionsSize);
    if (Size > 0)
    {
        Context->RemainingSize -= Size;
        Target = Context->CurrentReply + Context->RemainingSize;
        RtlCopyMemory(Target, Options, Size);
        Reply->Options.OptionsData = UserBuffer + (Target - SystemBuffer);
        Reply->Options.OptionsSize = (UCHAR)Size;
    }

    Context->nReplies++;
}

IO_WORKITEM_ROUTINE EndRequestHandler;

VOID
NTAPI
EndRequestHandler(
    PDEVICE_OBJECT DeviceObject,
    PVOID _Context)
{
    PICMP_PACKET_CONTEXT Context = (PICMP_PACKET_CONTEXT)_Context;
    PIO_STACK_LOCATION CurrentStack;
    PIRP Irp;
    NTSTATUS Status;

    /* No reply can reach the request any more */
    ASSERT(!Context->Accepting);

    TI_DbgPrint(DEBUG_ICMP, ("Finishing request Context: %p\n", Context));

    Irp = Context->Irp;
    CurrentStack = IoGetCurrentIrpStackLocation(Irp);

    if (Context->nReplies > 0)
    {
        ((PICMP_ECHO_REPLY)Irp->AssociatedIrp.SystemBuffer)->Reserved = Context->nReplies;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = CurrentStack->Parameters.DeviceIoControl.OutputBufferLength;
    }
    else
    {
        PICMP_ECHO_REPLY ReplyBuffer = (PICMP_ECHO_REPLY)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(ReplyBuffer, sizeof(*ReplyBuffer));
        ReplyBuffer->Status = IP_REQ_TIMED_OUT;

        Irp->IoStatus.Status = STATUS_TIMEOUT;
        Irp->IoStatus.Information = sizeof(*ReplyBuffer);
    }

    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    Status = FileCloseAddress(&Context->TdiRequest);
    ASSERT(NT_SUCCESS(Status));

    IoFreeWorkItem(Context->FinishWorker);
    ExFreePoolWithTag(Context, OUT_DATA_TAG);
}

/*
 * Called by ICMPReceive for every ICMP datagram received: hand a reply to
 * the echo request it answers, if that one still takes replies
 */
VOID
IcmpEchoReceive(
    _In_ PIP_PACKET IPPacket)
{
    PICMP_PACKET_CONTEXT Context = NULL;
    PLIST_ENTRY Entry;
    UINT16 Identifier, Sequence;
    BOOLEAN Finish = FALSE;
    KIRQL OldIrql;

    if (IPPacket->Type != IP_ADDRESS_V4)
        return;

    if (!GetEchoRequestKey(IPPacket->Data, IPPacket->TotalSize - IPPacket->HeaderSize,
                           &Identifier, &Sequence))
    {
        return;
    }

    KeAcquireSpinLock(&IcmpEchoRequestLock, &OldIrql);

    for (Entry = IcmpEchoRequestList.Flink; Entry != &IcmpEchoRequestList; Entry = Entry->Flink)
    {
        Context = CONTAINING_RECORD(Entry, ICMP_PACKET_CONTEXT, ListEntry);
        if (Context->Identifier == Identifier && Context->Sequence == Sequence)
            break;
        Context = NULL;
    }

    if (Context)
    {
        TI_DbgPrint(DEBUG_ICMP, ("Reply for Context: %p\n", Context));

        StoreReply(Context, IPPacket);

        if (Context->RemainingSize < sizeof(ICMP_ECHO_REPLY))
        {
            /* The buffer is full: the request ends now, unless its timer
             * already expired (the timeout DPC ends it then) or is not set
             * yet (DispEchoRequest ends it then) */
            RemoveEntryList(&Context->ListEntry);
            Context->Accepting = FALSE;
            Finish = Context->TimerSet && KeCancelTimer(&Context->TimeoutTimer);
        }
    }

    KeReleaseSpinLock(&IcmpEchoRequestLock, OldIrql);

    if (Finish)
        IoQueueWorkItem(Context->FinishWorker, EndRequestHandler, DelayedWorkQueue, Context);
}

KDEFERRED_ROUTINE TimeoutHandler;

VOID
NTAPI
TimeoutHandler(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID _Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PICMP_PACKET_CONTEXT Context = (PICMP_PACKET_CONTEXT)_Context;

    /* The timer expired: IcmpEchoReceive could not cancel it, so ending the
     * request is up to us, whether or not its buffer filled up meanwhile */
    KeAcquireSpinLockAtDpcLevel(&IcmpEchoRequestLock);
    ASSERT(Context->TimerSet);
    if (Context->Accepting)
    {
        RemoveEntryList(&Context->ListEntry);
        Context->Accepting = FALSE;
    }
    KeReleaseSpinLockFromDpcLevel(&IcmpEchoRequestLock);

    IoQueueWorkItem(Context->FinishWorker, EndRequestHandler, DelayedWorkQueue, Context);
}

NTSTATUS
DispEchoRequest(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    PICMP_ECHO_REQUEST Request = Irp->AssociatedIrp.SystemBuffer;
    UINT32 OutputBufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    UINT32 InputBufferLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    NTSTATUS Status, CloseStatus;
    TDI_CONNECTION_INFORMATION ConnectionInfo;
    TA_IP_ADDRESS RemoteAddressTa, LocalAddressTa;
    PADDRESS_FILE AddrFile;
    ULONG DataUsed;
    PUCHAR Buffer;
    UINT16 RequestSize;
    PICMP_PACKET_CONTEXT SendContext;
    LARGE_INTEGER RequestTimeout;
    UINT8 SavedTtl;
    BOOLEAN Finish;
    KIRQL OldIrql;

    TI_DbgPrint(DEBUG_ICMP, ("About to send datagram, OutputBufferLength: %u, SystemBuffer: %p\n", OutputBufferLength, Irp->AssociatedIrp.SystemBuffer));

    // check buffers
    if (OutputBufferLength < sizeof(ICMP_ECHO_REPLY) || InputBufferLength < sizeof(ICMP_ECHO_REQUEST))
    {
        return STATUS_INVALID_PARAMETER;
    }

    // check request parameters
    if ((Request->DataSize > UINT16_MAX - sizeof(ICMP_HEADER) - sizeof(IPv4_HEADER)) ||
        ((UINT32)Request->DataOffset + Request->DataSize > InputBufferLength) ||
        ((UINT32)Request->OptionsOffset + Request->OptionsSize > InputBufferLength))
    {
        return STATUS_INVALID_PARAMETER;
    }

    SendContext = ExAllocatePoolWithTag(NonPagedPool, sizeof(*SendContext), OUT_DATA_TAG);
    if (!SendContext)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(SendContext, sizeof(*SendContext));

    RequestSize = sizeof(ICMP_HEADER) + Request->DataSize;
    Buffer = ExAllocatePoolWithTag(NonPagedPool, RequestSize, OUT_DATA_TAG);
    SendContext->FinishWorker = IoAllocateWorkItem(DeviceObject);
    if (!Buffer || !SendContext->FinishWorker)
    {
        if (SendContext->FinishWorker)
            IoFreeWorkItem(SendContext->FinishWorker);
        if (Buffer)
            ExFreePoolWithTag(Buffer, OUT_DATA_TAG);
        ExFreePoolWithTag(SendContext, OUT_DATA_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    SendContext->TdiRequest.RequestContext = Irp;

    // setting up everything needed for sending the packet

    RtlZeroMemory(&RemoteAddressTa, sizeof(RemoteAddressTa));
    RtlZeroMemory(&LocalAddressTa, sizeof(LocalAddressTa));
    RtlZeroMemory(&ConnectionInfo, sizeof(ConnectionInfo));

    RemoteAddressTa.TAAddressCount = 1;
    RemoteAddressTa.Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    RemoteAddressTa.Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    RemoteAddressTa.Address[0].Address[0].in_addr = Request->Address;

    LocalAddressTa.TAAddressCount = 1;
    LocalAddressTa.Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    LocalAddressTa.Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    LocalAddressTa.Address[0].Address[0].in_addr = 0;

    Status = FileOpenAddress(&SendContext->TdiRequest, &LocalAddressTa, IPPROTO_ICMP, FALSE, NULL);
    if (!NT_SUCCESS(Status))
    {
        TI_DbgPrint(DEBUG_ICMP, ("Failed to open address file status: 0x%x\n", Status));

        IoFreeWorkItem(SendContext->FinishWorker);
        ExFreePoolWithTag(Buffer, OUT_DATA_TAG);
        ExFreePoolWithTag(SendContext, OUT_DATA_TAG);
        return Status;
    }

    AddrFile = (PADDRESS_FILE)SendContext->TdiRequest.Handle.AddressHandle;

    ConnectionInfo.RemoteAddress = &RemoteAddressTa;
    ConnectionInfo.RemoteAddressLength = sizeof(RemoteAddressTa);

    // making up the request packet

    ((PICMP_HEADER)Buffer)->Type = ICMP_TYPE_ECHO_REQUEST;
    ((PICMP_HEADER)Buffer)->Code = 0;
    ((PICMP_HEADER)Buffer)->Checksum = 0;
    ((PICMP_HEADER)Buffer)->Identifier = (UINT_PTR)PsGetCurrentProcessId() & UINT16_MAX;
    ((PICMP_HEADER)Buffer)->Seq = InterlockedIncrement16(&IcmpSequence);
    memcpy(Buffer + sizeof(ICMP_HEADER), (PUCHAR)Request + Request->DataOffset, Request->DataSize);
    ((PICMP_HEADER)Buffer)->Checksum = IPv4Checksum(Buffer, RequestSize, 0);
    /* Without options (IcmpSendEcho(..., RequestOptions = NULL, ...)) the
     * echo goes out with the default TTL: Ttl is zero then */
    SavedTtl = Request->HasOptions ? Request->Ttl : 128;
    RequestTimeout.QuadPart = (-1LL) * 10 * 1000 * Request->Timeout;

    // setting up the context; the reply buffer overlays the request

    KeQueryPerformanceCounter(&SendContext->TimerResolution);
    SendContext->Irp = Irp;
    SendContext->Identifier = ((PICMP_HEADER)Buffer)->Identifier;
    SendContext->Sequence = ((PICMP_HEADER)Buffer)->Seq;
    SendContext->CurrentReply = Irp->AssociatedIrp.SystemBuffer;
    SendContext->RemainingSize = OutputBufferLength;
    SendContext->nReplies = 0;
    KeInitializeDpc(&SendContext->TimeoutDpc, &TimeoutHandler, SendContext);
    KeInitializeTimerEx(&SendContext->TimeoutTimer, SynchronizationTimer);

    RtlZeroMemory(Irp->AssociatedIrp.SystemBuffer, OutputBufferLength);

    LockObject(AddrFile);
    AddrFile->TTL = SavedTtl;
    UnlockObject(AddrFile);

    /* A reply can arrive as soon as the request is sent */
    KeAcquireSpinLock(&IcmpEchoRequestLock, &OldIrql);
    InsertTailList(&IcmpEchoRequestList, &SendContext->ListEntry);
    SendContext->Accepting = TRUE;
    KeReleaseSpinLock(&IcmpEchoRequestLock, OldIrql);

    SendContext->StartTicks = KeQueryPerformanceCounter(NULL).QuadPart;
    Status = AddrFile->Send(AddrFile, &ConnectionInfo, (PCHAR)Buffer, RequestSize, &DataUsed);

    ExFreePoolWithTag(Buffer, OUT_DATA_TAG);

    if (!NT_SUCCESS(Status))
    {
        TI_DbgPrint(DEBUG_ICMP, ("Failed to send a datagram: 0x%x\n", Status));

        /* No timer was set: nobody else ends the request */
        KeAcquireSpinLock(&IcmpEchoRequestLock, &OldIrql);
        if (SendContext->Accepting)
        {
            RemoveEntryList(&SendContext->ListEntry);
            SendContext->Accepting = FALSE;
        }
        KeReleaseSpinLock(&IcmpEchoRequestLock, OldIrql);

        CloseStatus = FileCloseAddress(&SendContext->TdiRequest);
        ASSERT(NT_SUCCESS(CloseStatus));

        IoFreeWorkItem(SendContext->FinishWorker);
        ExFreePoolWithTag(SendContext, OUT_DATA_TAG);
        return Status;
    }

    IoMarkIrpPending(Irp);

    KeAcquireSpinLock(&IcmpEchoRequestLock, &OldIrql);
    Finish = !SendContext->Accepting;
    if (!Finish)
    {
        KeSetTimer(&SendContext->TimeoutTimer, RequestTimeout, &SendContext->TimeoutDpc);
        SendContext->TimerSet = TRUE;
    }
    KeReleaseSpinLock(&IcmpEchoRequestLock, OldIrql);

    /* The buffer filled up before the timer was set */
    if (Finish)
        IoQueueWorkItem(SendContext->FinishWorker, EndRequestHandler, DelayedWorkQueue, SendContext);

    return STATUS_PENDING;
}
