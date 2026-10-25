/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-mode test for the WSK provider in netio.sys
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>
#include <ws2def.h>
#include <wsk.h>

#include "NetioWsk.h"

#define TAG_TEST 'kWtN'
#define LOOPBACK 0x0100007F /* 127.0.0.1 in network order */

/* Calling a WSK routine synchronously, the way a client waits for its IRP */
typedef struct _SYNC_CALL
{
    PIRP Irp;
    KEVENT Event;
} SYNC_CALL, *PSYNC_CALL;

static
NTSTATUS
NTAPI
SyncCallComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
PIRP
SyncCallBegin(
    _Out_ PSYNC_CALL Call)
{
    KeInitializeEvent(&Call->Event, NotificationEvent, FALSE);
    Call->Irp = IoAllocateIrp(1, FALSE);
    if (Call->Irp)
        IoSetCompletionRoutine(Call->Irp, SyncCallComplete, &Call->Event, TRUE, TRUE, TRUE);
    return Call->Irp;
}

/* Returns the IRP's final status; the provider completes the IRP whatever
 * the routine returned */
static
NTSTATUS
SyncCallEnd(
    _Inout_ PSYNC_CALL Call,
    _In_ NTSTATUS Returned,
    _Out_opt_ PULONG_PTR Information)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Timeout.QuadPart = -10LL * 1000 * 1000 * 10;
    Status = KeWaitForSingleObject(&Call->Event, Executive, KernelMode, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "IRP not completed (wait 0x%lx, returned 0x%lx)\n", Status, Returned);
    if (Status != STATUS_SUCCESS)
    {
        /* Do not leave it pending: its completion would signal our stack */
        IoCancelIrp(Call->Irp);
        Status = KeWaitForSingleObject(&Call->Event, Executive, KernelMode, FALSE, &Timeout);
        ok(Status == STATUS_SUCCESS, "Cancelled IRP not completed\n");
        if (Status == STATUS_SUCCESS)
            IoFreeIrp(Call->Irp);
        /* Leak the IRP rather than free one the provider may still own */
        return STATUS_IO_TIMEOUT;
    }

    Status = Call->Irp->IoStatus.Status;
    if (Information)
        *Information = Call->Irp->IoStatus.Information;
    IoFreeIrp(Call->Irp);
    return Status;
}

static volatile LONG DatagramsIndicated;
static volatile LONG BytesIndicated;

static
NTSTATUS
WSKAPI
ReceiveFromEvent(
    _In_opt_ PVOID SocketContext,
    _In_ ULONG Flags,
    _In_opt_ PWSK_DATAGRAM_INDICATION DataIndication)
{
    for (; DataIndication != NULL; DataIndication = DataIndication->Next)
    {
        InterlockedIncrement(&DatagramsIndicated);
        InterlockedExchangeAdd(&BytesIndicated, (LONG)DataIndication->Buffer.Length);
    }
    return STATUS_SUCCESS;
}

static const WSK_CLIENT_DISPATCH ClientDispatch = { MAKE_WSK_VERSION(1, 0), 0, NULL };
static const WSK_CLIENT_DATAGRAM_DISPATCH DatagramDispatch = { ReceiveFromEvent };

typedef struct _TEST_BUFFER
{
    PVOID Data;
    PMDL Mdl;
} TEST_BUFFER, *PTEST_BUFFER;

static
BOOLEAN
AllocateBuffer(
    _Out_ PTEST_BUFFER Buffer,
    _In_ ULONG Length,
    _In_ UCHAR Fill)
{
    Buffer->Mdl = NULL;
    Buffer->Data = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_TEST);
    if (!Buffer->Data)
        return FALSE;
    RtlFillMemory(Buffer->Data, Length, Fill);
    Buffer->Mdl = IoAllocateMdl(Buffer->Data, Length, FALSE, FALSE, NULL);
    if (!Buffer->Mdl)
    {
        ExFreePoolWithTag(Buffer->Data, TAG_TEST);
        Buffer->Data = NULL;
        return FALSE;
    }
    MmBuildMdlForNonPagedPool(Buffer->Mdl);
    return TRUE;
}

static
VOID
FreeBuffer(
    _Inout_ PTEST_BUFFER Buffer)
{
    if (Buffer->Mdl)
        IoFreeMdl(Buffer->Mdl);
    if (Buffer->Data)
        ExFreePoolWithTag(Buffer->Data, TAG_TEST);
}

static
PWSK_SOCKET
CreateSocket(
    _In_ PWSK_PROVIDER_NPI Npi,
    _In_ USHORT Type,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _In_opt_ const VOID *Dispatch)
{
    SYNC_CALL Call;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    if (!SyncCallBegin(&Call))
        return NULL;
    Status = Npi->Dispatch->WskSocket(Npi->Client, AF_INET, Type, Protocol, Flags, NULL,
                                      Dispatch, NULL, NULL, NULL, Call.Irp);
    Status = SyncCallEnd(&Call, Status, &Information);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status) ? (PWSK_SOCKET)Information : NULL;
}

static
VOID
CloseSocket(
    _In_ PWSK_SOCKET Socket)
{
    PWSK_PROVIDER_BASIC_DISPATCH Basic = (PWSK_PROVIDER_BASIC_DISPATCH)Socket->Dispatch;
    SYNC_CALL Call;
    NTSTATUS Status;

    if (!SyncCallBegin(&Call))
        return;
    Status = Basic->WskCloseSocket(Socket, Call.Irp);
    Status = SyncCallEnd(&Call, Status, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static
NTSTATUS
BindAndGetLocalTo(
    _In_ PWSK_SOCKET Socket,
    _In_ PFN_WSK_BIND Bind,
    _In_ PFN_WSK_GET_LOCAL_ADDRESS GetLocalAddress,
    _In_ ULONG BindAddress,
    _Out_ PSOCKADDR_IN Local)
{
    SOCKADDR_IN Address;
    SYNC_CALL Call;
    NTSTATUS Status;

    RtlZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = BindAddress;
    Address.sin_port = 0; /* any */

    if (!SyncCallBegin(&Call))
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = Bind(Socket, (PSOCKADDR)&Address, 0, Call.Irp);
    Status = SyncCallEnd(&Call, Status, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlFillMemory(Local, sizeof(*Local), 0xCC);
    if (!SyncCallBegin(&Call))
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = GetLocalAddress(Socket, (PSOCKADDR)Local, Call.Irp);
    Status = SyncCallEnd(&Call, Status, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return Status;

    ok_eq_uint(Local->sin_family, AF_INET);
    ok_eq_hex(Local->sin_addr.s_addr, BindAddress);
    ok(Local->sin_port != 0, "The transport did not pick a port\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
BindAndGetLocal(
    _In_ PWSK_SOCKET Socket,
    _In_ PFN_WSK_BIND Bind,
    _In_ PFN_WSK_GET_LOCAL_ADDRESS GetLocalAddress,
    _Out_ PSOCKADDR_IN Local)
{
    return BindAndGetLocalTo(Socket, Bind, GetLocalAddress, LOOPBACK, Local);
}

static
VOID
TestDatagram(
    _In_ PWSK_PROVIDER_NPI Npi)
{
    PWSK_PROVIDER_DATAGRAM_DISPATCH Dispatch;
    WSK_EVENT_CALLBACK_CONTROL EventControl;
    TEST_BUFFER Buffers[3] = { { 0 } };
    static const ULONG Lengths[3] = { 10, 20, 30 };
    WSK_BUF_LIST List[3];
    SOCKADDR_IN Local;
    PWSK_SOCKET Socket;
    WSK_BUF WskBuf;
    SYNC_CALL Call;
    ULONG_PTR Information;
    NTSTATUS Status;
    ULONG i;

    Socket = CreateSocket(Npi, SOCK_DGRAM, IPPROTO_UDP, WSK_FLAG_DATAGRAM_SOCKET, &DatagramDispatch);
    if (skip(Socket != NULL, "No datagram socket\n"))
        return;
    Dispatch = (PWSK_PROVIDER_DATAGRAM_DISPATCH)Socket->Dispatch;

    /* Clients built for Windows 8 and later call this */
    ok(Dispatch->WskSendMessages != NULL, "No WskSendMessages in the datagram dispatch\n");

    Status = BindAndGetLocal(Socket, Dispatch->WskBind, Dispatch->WskGetLocalAddress, &Local);
    if (skip(NT_SUCCESS(Status), "Bind failed\n"))
        goto Close;

    for (i = 0; i < RTL_NUMBER_OF(Buffers); i++)
    {
        if (!AllocateBuffer(&Buffers[i], Lengths[i], (UCHAR)(0x41 + i)))
            goto Free;
        List[i].Next = (i + 1 < RTL_NUMBER_OF(List)) ? &List[i + 1] : NULL;
        List[i].Buffer.Mdl = Buffers[i].Mdl;
        List[i].Buffer.Offset = 0;
        List[i].Buffer.Length = Lengths[i];
    }

    /* Without a destination, sending is an error */
    WskBuf = List[0].Buffer;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskSendTo(Socket, &WskBuf, 0, NULL, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    }

    DatagramsIndicated = 0;
    BytesIndicated = 0;

    /* Event callbacks are enabled without an IRP */
    RtlZeroMemory(&EventControl, sizeof(EventControl));
    EventControl.NpiId = &NPI_WSK_INTERFACE_ID;
    EventControl.EventMask = WSK_EVENT_RECEIVE_FROM;
    Status = Dispatch->WskControlSocket(Socket, WskSetOption, SO_WSK_EVENT_CALLBACK, SOL_SOCKET,
                                        sizeof(EventControl), &EventControl, 0, NULL, NULL, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Fix the destination: ourselves */
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskControlSocket(Socket, WskIoctl, SIO_WSK_SET_REMOTE_ADDRESS, 0,
                                            sizeof(Local), &Local, 0, NULL, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    /* WskSendTo without an address now goes there */
    if (SyncCallBegin(&Call))
    {
        Information = 0;
        Status = Dispatch->WskSendTo(Socket, &WskBuf, 0, NULL, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, Lengths[0]);
    }

    /* WskSendMessages sends every buffer, with an explicit address... */
    if (SyncCallBegin(&Call))
    {
        Information = 0;
        Status = Dispatch->WskSendMessages(Socket, List, 0, (PSOCKADDR)&Local, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, Lengths[0] + Lengths[1] + Lengths[2]);
    }

    /* ...or with the fixed one */
    if (SyncCallBegin(&Call))
    {
        Information = 0;
        Status = Dispatch->WskSendMessages(Socket, &List[1], 0, NULL, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, Lengths[1] + Lengths[2]);
    }

    /* Removing the fixed address makes address-less sends fail again */
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskControlSocket(Socket, WskIoctl, SIO_WSK_SET_REMOTE_ADDRESS, 0,
                                            0, NULL, 0, NULL, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskSendMessages(Socket, List, 0, NULL, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    }

    /* Everything sent to ourselves comes back through the event. Loopback
     * delivery is asynchronous, so wait for it: what is still in flight
     * when the socket closes would reach the next socket given its port. */
    for (i = 0; i < 50 && DatagramsIndicated < 6; i++)
    {
        LARGE_INTEGER Delay;
        Delay.QuadPart = -10LL * 1000 * 100;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    ok_eq_long(DatagramsIndicated, 6L);
    ok_eq_long(BytesIndicated, (LONG)(Lengths[0] + Lengths[0] + Lengths[1] + Lengths[2] + Lengths[1] + Lengths[2]));

Free:
    for (i = 0; i < RTL_NUMBER_OF(Buffers); i++)
        FreeBuffer(&Buffers[i]);
Close:
    CloseSocket(Socket);
}


/* ------------------------------------------------------------------ */
/* Receive tests                                                       */
/* ------------------------------------------------------------------ */

/* The NetioWsk test harness runs a TCP echo server on the QEMU host,
 * reachable through slirp at 10.0.2.2. It echoes what it gets and closes
 * once the client finished sending. */
#define ECHO_HOST 0x0202000A /* 10.0.2.2 in network order */
#define ECHO_PORT 47777

#define EVENT_BUFFER_SIZE 256

typedef enum _EVENT_MODE
{
    EventAccept,        /* STATUS_SUCCESS */
    EventRetain,        /* STATUS_PENDING, released later */
    EventAcceptPart,    /* STATUS_SUCCESS, only part of the data accepted */
} EVENT_MODE;

/* What the event callbacks saw. They run at DISPATCH_LEVEL, so they only
 * record; the test checks afterwards. */
typedef struct _EVENT_RECORD
{
    KSPIN_LOCK Lock;
    KEVENT DataEvent;
    KEVENT DisconnectEvent;
    EVENT_MODE Mode;
    ULONG Calls;
    ULONG Flags;
    ULONG Length;
    UCHAR Data[EVENT_BUFFER_SIZE];
    SOCKADDR_IN Remote;
    SIZE_T BytesIndicated;
    PVOID Retained;
    PVOID Context;
    ULONG DisconnectCalls;
    ULONG DisconnectFlags;
} EVENT_RECORD, *PEVENT_RECORD;

static EVENT_RECORD Record;

static
VOID
ResetRecord(
    _In_ EVENT_MODE Mode)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Record.Lock, &OldIrql);
    Record.Mode = Mode;
    Record.Calls = 0;
    Record.Flags = 0;
    Record.Length = 0;
    Record.BytesIndicated = 0;
    Record.Retained = NULL;
    Record.Context = NULL;
    RtlZeroMemory(&Record.Remote, sizeof(Record.Remote));
    KeClearEvent(&Record.DataEvent);
    KeReleaseSpinLock(&Record.Lock, OldIrql);
}

static
BOOLEAN
WaitFor(
    _In_ PKEVENT Event)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10LL * 1000 * 1000 * 10;
    return KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}

/* Appends what a WSK_BUF describes to the record. Lock held. */
static
VOID
RecordBuffer(
    _In_ const WSK_BUF *Buffer)
{
    PMDL Mdl = Buffer->Mdl;
    ULONG Offset = Buffer->Offset;
    SIZE_T Left = Buffer->Length;
    ULONG Count;
    PUCHAR Va;

    while (Mdl != NULL && Left > 0)
    {
        if (Offset >= MmGetMdlByteCount(Mdl))
        {
            Offset -= MmGetMdlByteCount(Mdl);
            Mdl = Mdl->Next;
            continue;
        }
        Va = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        if (Va == NULL)
            return;
        Count = (ULONG)min(Left, MmGetMdlByteCount(Mdl) - Offset);
        Count = min(Count, EVENT_BUFFER_SIZE - Record.Length);
        RtlCopyMemory(Record.Data + Record.Length, Va + Offset, Count);
        Record.Length += Count;
        Left -= Count;
        Offset = 0;
        Mdl = Mdl->Next;
    }
}

static
NTSTATUS
WSKAPI
RecordReceiveFromEvent(
    _In_opt_ PVOID SocketContext,
    _In_ ULONG Flags,
    _In_opt_ PWSK_DATAGRAM_INDICATION DataIndication)
{
    PWSK_DATAGRAM_INDICATION First = DataIndication;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Record.Lock, &OldIrql);
    Record.Calls++;
    Record.Flags = Flags;
    Record.Context = SocketContext;
    for (; DataIndication != NULL; DataIndication = DataIndication->Next)
    {
        RecordBuffer(&DataIndication->Buffer);
        if (DataIndication->RemoteAddress != NULL)
            RtlCopyMemory(&Record.Remote, DataIndication->RemoteAddress, sizeof(Record.Remote));
    }
    if (Record.Mode == EventRetain && Record.Retained == NULL)
    {
        Record.Retained = First;
        Status = STATUS_PENDING;
    }
    KeReleaseSpinLock(&Record.Lock, OldIrql);
    KeSetEvent(&Record.DataEvent, IO_NO_INCREMENT, FALSE);
    return Status;
}

static
NTSTATUS
WSKAPI
RecordReceiveEvent(
    _In_opt_ PVOID SocketContext,
    _In_ ULONG Flags,
    _In_opt_ PWSK_DATA_INDICATION DataIndication,
    _In_ SIZE_T BytesIndicated,
    _Inout_ SIZE_T *BytesAccepted)
{
    PWSK_DATA_INDICATION First = DataIndication;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Record.Lock, &OldIrql);
    Record.Calls++;
    Record.Flags = Flags;
    Record.Context = SocketContext;
    Record.BytesIndicated += BytesIndicated;
    if (Record.Mode == EventAcceptPart && BytesIndicated > 2)
    {
        /* Take two bytes, the rest has to be read with WskReceive */
        WSK_BUF Part = DataIndication->Buffer;
        Part.Length = 2;
        RecordBuffer(&Part);
        *BytesAccepted = 2;
    }
    else
    {
        for (; DataIndication != NULL; DataIndication = DataIndication->Next)
            RecordBuffer(&DataIndication->Buffer);
        if (Record.Mode == EventRetain && Record.Retained == NULL)
        {
            Record.Retained = First;
            Status = STATUS_PENDING;
        }
    }
    KeReleaseSpinLock(&Record.Lock, OldIrql);
    KeSetEvent(&Record.DataEvent, IO_NO_INCREMENT, FALSE);
    return Status;
}

static
NTSTATUS
WSKAPI
RecordDisconnectEvent(
    _In_opt_ PVOID SocketContext,
    _In_ ULONG Flags)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Record.Lock, &OldIrql);
    Record.DisconnectCalls++;
    Record.DisconnectFlags = Flags;
    KeReleaseSpinLock(&Record.Lock, OldIrql);
    KeSetEvent(&Record.DisconnectEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

static const WSK_CLIENT_DATAGRAM_DISPATCH RecordDatagramDispatch = { RecordReceiveFromEvent };
static const WSK_CLIENT_CONNECTION_DISPATCH RecordConnectionDispatch = { RecordReceiveEvent, RecordDisconnectEvent, NULL };

static
NTSTATUS
SetEvents(
    _In_ PWSK_SOCKET Socket,
    _In_ ULONG Mask)
{
    PWSK_PROVIDER_BASIC_DISPATCH Basic = (PWSK_PROVIDER_BASIC_DISPATCH)Socket->Dispatch;
    WSK_EVENT_CALLBACK_CONTROL EventControl;

    EventControl.NpiId = &NPI_WSK_INTERFACE_ID;
    EventControl.EventMask = Mask;
    return Basic->WskControlSocket(Socket, WskSetOption, SO_WSK_EVENT_CALLBACK, SOL_SOCKET,
                                   sizeof(EventControl), &EventControl, 0, NULL, NULL, NULL);
}

static
PWSK_SOCKET
CreateSocketWithContext(
    _In_ PWSK_PROVIDER_NPI Npi,
    _In_ USHORT Type,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _In_opt_ PVOID Context,
    _In_opt_ const VOID *Dispatch)
{
    SYNC_CALL Call;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    if (!SyncCallBegin(&Call))
        return NULL;
    Status = Npi->Dispatch->WskSocket(Npi->Client, AF_INET, Type, Protocol, Flags, Context,
                                      Dispatch, NULL, NULL, NULL, Call.Irp);
    Status = SyncCallEnd(&Call, Status, &Information);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status) ? (PWSK_SOCKET)Information : NULL;
}

static
NTSTATUS
SendDatagram(
    _In_ PWSK_SOCKET Socket,
    _In_ const SOCKADDR_IN *To,
    _In_ PTEST_BUFFER Buffer,
    _In_ ULONG Length)
{
    PWSK_PROVIDER_DATAGRAM_DISPATCH Dispatch = (PWSK_PROVIDER_DATAGRAM_DISPATCH)Socket->Dispatch;
    WSK_BUF WskBuf;
    SYNC_CALL Call;
    NTSTATUS Status;

    WskBuf.Mdl = Buffer->Mdl;
    WskBuf.Offset = 0;
    WskBuf.Length = Length;
    if (!SyncCallBegin(&Call))
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = Dispatch->WskSendTo(Socket, &WskBuf, 0, (PSOCKADDR)To, 0, NULL, Call.Irp);
    return SyncCallEnd(&Call, Status, NULL);
}

typedef struct _RECEIVE_FROM_CALL
{
    SYNC_CALL Call;
    WSK_BUF WskBuf;
    SOCKADDR_IN Remote;
    ULONG ControlLength;
    ULONG ControlFlags;
    NTSTATUS Returned;
} RECEIVE_FROM_CALL, *PRECEIVE_FROM_CALL;

static
BOOLEAN
ReceiveFromBegin(
    _In_ PWSK_SOCKET Socket,
    _Out_ PRECEIVE_FROM_CALL Rf,
    _In_ PTEST_BUFFER Buffer,
    _In_ ULONG Length)
{
    PWSK_PROVIDER_DATAGRAM_DISPATCH Dispatch = (PWSK_PROVIDER_DATAGRAM_DISPATCH)Socket->Dispatch;

    RtlFillMemory(&Rf->Remote, sizeof(Rf->Remote), 0xCC);
    Rf->ControlLength = 0x55;
    Rf->ControlFlags = 0x55;
    Rf->WskBuf.Mdl = Buffer->Mdl;
    Rf->WskBuf.Offset = 0;
    Rf->WskBuf.Length = Length;
    if (!SyncCallBegin(&Rf->Call))
        return FALSE;
    Rf->Returned = Dispatch->WskReceiveFrom(Socket, &Rf->WskBuf, 0, (PSOCKADDR)&Rf->Remote,
                                            &Rf->ControlLength, NULL, &Rf->ControlFlags, Rf->Call.Irp);
    return TRUE;
}

static
VOID
TestDatagramReceive(
    _In_ PWSK_PROVIDER_NPI Npi,
    _In_ ULONG Address)
{
    PWSK_PROVIDER_DATAGRAM_DISPATCH Dispatch;
    PWSK_SOCKET Receiver, Sender;
    SOCKADDR_IN ReceiverAddress, SenderAddress;
    TEST_BUFFER Out = { 0 }, In = { 0 };
    RECEIVE_FROM_CALL Rf;
    ULONG_PTR Information;
    PVOID Retained;
    NTSTATUS Status;
    ULONG i;

    trace("Datagram receive on 0x%08lx\n", Address);

    Receiver = CreateSocketWithContext(Npi, SOCK_DGRAM, IPPROTO_UDP, WSK_FLAG_DATAGRAM_SOCKET,
                                       &Record, &RecordDatagramDispatch);
    Sender = CreateSocketWithContext(Npi, SOCK_DGRAM, IPPROTO_UDP, WSK_FLAG_DATAGRAM_SOCKET,
                                     NULL, NULL);
    if (skip(Receiver && Sender, "No sockets\n"))
        goto Close;
    Dispatch = (PWSK_PROVIDER_DATAGRAM_DISPATCH)Receiver->Dispatch;

    if (!AllocateBuffer(&Out, 64, 0) || !AllocateBuffer(&In, 64, 0))
        goto Close;
    for (i = 0; i < 64; i++)
        ((PUCHAR)Out.Data)[i] = (UCHAR)(i + 1);

    Status = BindAndGetLocalTo(Receiver, Dispatch->WskBind, Dispatch->WskGetLocalAddress, Address, &ReceiverAddress);
    if (skip(NT_SUCCESS(Status), "Bind failed\n"))
        goto Close;
    Status = BindAndGetLocalTo(Sender, Dispatch->WskBind, Dispatch->WskGetLocalAddress, Address, &SenderAddress);
    if (skip(NT_SUCCESS(Status), "Bind failed\n"))
        goto Close;

    /* 1. A WskReceiveFrom posted first completes with the next datagram */
    RtlZeroMemory(In.Data, 64);
    if (ReceiveFromBegin(Receiver, &Rf, &In, 64))
    {
        ok_eq_hex(Rf.Returned, STATUS_PENDING);
        Status = SendDatagram(Sender, &ReceiverAddress, &Out, 17);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Information = 0;
        Status = SyncCallEnd(&Rf.Call, Rf.Returned, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 17);
        ok(RtlCompareMemory(In.Data, Out.Data, 17) == 17, "Wrong data received\n");
        ok_eq_uint(Rf.Remote.sin_family, AF_INET);
        ok_eq_hex(Rf.Remote.sin_addr.s_addr, SenderAddress.sin_addr.s_addr);
        ok_eq_uint(Rf.Remote.sin_port, SenderAddress.sin_port);
        ok_eq_ulong(Rf.ControlLength, 0UL);
        ok_eq_ulong(Rf.ControlFlags, 0UL);
    }

    /* 2. A datagram that arrives first waits for WskReceiveFrom */
    Status = SendDatagram(Sender, &ReceiverAddress, &Out, 5);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlZeroMemory(In.Data, 64);
    if (ReceiveFromBegin(Receiver, &Rf, &In, 64))
    {
        Information = 0;
        Status = SyncCallEnd(&Rf.Call, Rf.Returned, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 5);
        ok(RtlCompareMemory(In.Data, Out.Data, 5) == 5, "Wrong data received\n");
        ok_eq_uint(Rf.Remote.sin_port, SenderAddress.sin_port);
    }

    /* 3. Too small a buffer truncates the datagram */
    Status = SendDatagram(Sender, &ReceiverAddress, &Out, 10);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlZeroMemory(In.Data, 64);
    if (ReceiveFromBegin(Receiver, &Rf, &In, 4))
    {
        Information = 0;
        Status = SyncCallEnd(&Rf.Call, Rf.Returned, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 4);
        ok_eq_ulong(Rf.ControlFlags, (ULONG)MSG_TRUNC);
        ok_eq_uint(((PUCHAR)In.Data)[4], 0);
    }

    /* 4. Cancelling a pending WskReceiveFrom */
    if (ReceiveFromBegin(Receiver, &Rf, &In, 64))
    {
        ok_eq_hex(Rf.Returned, STATUS_PENDING);
        IoCancelIrp(Rf.Call.Irp);
        Status = SyncCallEnd(&Rf.Call, Rf.Returned, NULL);
        ok_eq_hex(Status, STATUS_CANCELLED);
    }

    /* 5. With the event enabled, datagrams are indicated */
    ResetRecord(EventAccept);
    Status = SetEvents(Receiver, WSK_EVENT_RECEIVE_FROM);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SendDatagram(Sender, &ReceiverAddress, &Out, 33);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(WaitFor(&Record.DataEvent), "WskReceiveFromEvent was not called\n");
    ok_eq_ulong(Record.Calls, 1UL);
    ok_eq_ulong(Record.Length, 33UL);
    ok(RtlCompareMemory(Record.Data, Out.Data, 33) == 33, "Wrong data indicated\n");
    ok_eq_pointer(Record.Context, &Record);
    ok_eq_uint(Record.Remote.sin_family, AF_INET);
    ok_eq_hex(Record.Remote.sin_addr.s_addr, SenderAddress.sin_addr.s_addr);
    ok_eq_uint(Record.Remote.sin_port, SenderAddress.sin_port);

    /* 6. Retaining an indication, then giving it back */
    ResetRecord(EventRetain);
    Status = SendDatagram(Sender, &ReceiverAddress, &Out, 9);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(WaitFor(&Record.DataEvent), "WskReceiveFromEvent was not called\n");
    ok_eq_ulong(Record.Length, 9UL);
    Retained = Record.Retained;
    ok(Retained != NULL, "Indication not retained\n");
    if (Retained)
    {
        /* Still valid after the callback returned */
        PWSK_DATAGRAM_INDICATION Indication = Retained;
        PUCHAR Va = MmGetSystemAddressForMdlSafe(Indication->Buffer.Mdl, NormalPagePriority);
        ok_eq_size(Indication->Buffer.Length, (SIZE_T)9);
        ok(Va != NULL && RtlCompareMemory(Va + Indication->Buffer.Offset, Out.Data, 9) == 9,
           "Retained data changed\n");
        Status = Dispatch->WskRelease(Receiver, Indication);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    /* 7. Disabled again, datagrams wait for WskReceiveFrom */
    ResetRecord(EventAccept);
    Status = SetEvents(Receiver, WSK_EVENT_DISABLE | WSK_EVENT_RECEIVE_FROM);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SendDatagram(Sender, &ReceiverAddress, &Out, 3);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlZeroMemory(In.Data, 64);
    if (ReceiveFromBegin(Receiver, &Rf, &In, 64))
    {
        Information = 0;
        Status = SyncCallEnd(&Rf.Call, Rf.Returned, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 3);
    }
    ok_eq_ulong(Record.Calls, 0UL);

    /* 8. What arrived while disabled is indicated when enabled */
    Status = SendDatagram(Sender, &ReceiverAddress, &Out, 7);
    ok_eq_hex(Status, STATUS_SUCCESS);
    /* Give it time to arrive */
    {
        LARGE_INTEGER Delay;
        Delay.QuadPart = -10LL * 1000 * 200;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    Status = SetEvents(Receiver, WSK_EVENT_RECEIVE_FROM);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(WaitFor(&Record.DataEvent), "Backlogged datagram not indicated\n");
    ok_eq_ulong(Record.Length, 7UL);

    /* 9. Closing the socket completes a pending WskReceiveFrom */
    Status = SetEvents(Receiver, WSK_EVENT_DISABLE | WSK_EVENT_RECEIVE_FROM);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (ReceiveFromBegin(Receiver, &Rf, &In, 64))
    {
        ok_eq_hex(Rf.Returned, STATUS_PENDING);
        CloseSocket(Receiver);
        Receiver = NULL;
        Status = SyncCallEnd(&Rf.Call, Rf.Returned, NULL);
        ok_eq_hex(Status, STATUS_CANCELLED);
    }

Close:
    FreeBuffer(&Out);
    FreeBuffer(&In);
    if (Receiver)
        CloseSocket(Receiver);
    if (Sender)
        CloseSocket(Sender);
}

/* Closing TCP sockets that never connected */
static
VOID
TestStreamCloseUnconnected(
    _In_ PWSK_PROVIDER_NPI Npi)
{
    PWSK_PROVIDER_CONNECTION_DISPATCH Dispatch;
    PWSK_SOCKET Socket;
    SOCKADDR_IN Local;
    NTSTATUS Status;

    /* Never bound */
    Socket = CreateSocket(Npi, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_CONNECTION_SOCKET, NULL);
    if (Socket)
        CloseSocket(Socket);

    /* Bound, never connected */
    Socket = CreateSocket(Npi, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_CONNECTION_SOCKET, NULL);
    if (Socket)
    {
        Dispatch = (PWSK_PROVIDER_CONNECTION_DISPATCH)Socket->Dispatch;
        Status = BindAndGetLocal(Socket, Dispatch->WskBind, Dispatch->WskGetLocalAddress, &Local);
        ok_eq_hex(Status, STATUS_SUCCESS);
        CloseSocket(Socket);
    }

    /* With the receive events enabled */
    Socket = CreateSocketWithContext(Npi, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_CONNECTION_SOCKET,
                                     &Record, &RecordConnectionDispatch);
    if (Socket)
    {
        Status = SetEvents(Socket, WSK_EVENT_RECEIVE | WSK_EVENT_DISCONNECT);
        ok_eq_hex(Status, STATUS_SUCCESS);
        CloseSocket(Socket);
    }
}

static
NTSTATUS
StreamSend(
    _In_ PWSK_SOCKET Socket,
    _In_ PTEST_BUFFER Buffer,
    _In_ ULONG Length)
{
    PWSK_PROVIDER_CONNECTION_DISPATCH Dispatch = (PWSK_PROVIDER_CONNECTION_DISPATCH)Socket->Dispatch;
    ULONG_PTR Information = 0;
    WSK_BUF WskBuf;
    SYNC_CALL Call;
    NTSTATUS Status;

    WskBuf.Mdl = Buffer->Mdl;
    WskBuf.Offset = 0;
    WskBuf.Length = Length;
    if (!SyncCallBegin(&Call))
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = Dispatch->WskSend(Socket, &WskBuf, WSK_FLAG_NODELAY, Call.Irp);
    Status = SyncCallEnd(&Call, Status, &Information);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Information, Length);
    return Status;
}

/* Reads exactly Length bytes with WskReceive, at Offset into Buffer */
static
NTSTATUS
StreamReceiveAll(
    _In_ PWSK_SOCKET Socket,
    _In_ PTEST_BUFFER Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    PWSK_PROVIDER_CONNECTION_DISPATCH Dispatch = (PWSK_PROVIDER_CONNECTION_DISPATCH)Socket->Dispatch;
    ULONG_PTR Information;
    WSK_BUF WskBuf;
    SYNC_CALL Call;
    NTSTATUS Status = STATUS_SUCCESS;

    while (Length > 0)
    {
        WskBuf.Mdl = Buffer->Mdl;
        WskBuf.Offset = Offset;
        WskBuf.Length = Length;
        if (!SyncCallBegin(&Call))
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = Dispatch->WskReceive(Socket, &WskBuf, 0, Call.Irp);
        Information = 0;
        Status = SyncCallEnd(&Call, Status, &Information);
        if (!NT_SUCCESS(Status))
            break;
        if (Information == 0)
        {
            Status = STATUS_END_OF_FILE;
            break;
        }
        ok(Information <= Length, "Received %Iu bytes, asked for %lu\n", Information, Length);
        Offset += (ULONG)Information;
        Length -= (ULONG)min(Information, Length);
    }
    return Status;
}

static
VOID
TestStreamReceive(
    _In_ PWSK_PROVIDER_NPI Npi)
{
    PWSK_PROVIDER_CONNECTION_DISPATCH Dispatch;
    PWSK_SOCKET Socket;
    SOCKADDR_IN Local, Remote, Peer;
    TEST_BUFFER Out = { 0 }, In = { 0 };
    SYNC_CALL Call;
    WSK_BUF WskBuf;
    ULONG_PTR Information;
    PWSK_DATA_INDICATION Retained;
    NTSTATUS Status;
    ULONG i;

    if (!AllocateBuffer(&Out, 128, 0) || !AllocateBuffer(&In, 128, 0))
        goto Free;
    for (i = 0; i < 128; i++)
        ((PUCHAR)Out.Data)[i] = (UCHAR)(0x80 + i);

    Socket = CreateSocketWithContext(Npi, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_CONNECTION_SOCKET,
                                     &Record, &RecordConnectionDispatch);
    if (skip(Socket != NULL, "No stream socket\n"))
        goto Free;
    Dispatch = (PWSK_PROVIDER_CONNECTION_DISPATCH)Socket->Dispatch;

    /* Not connected yet */
    WskBuf.Mdl = In.Mdl;
    WskBuf.Offset = 0;
    WskBuf.Length = 16;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskReceive(Socket, &WskBuf, 0, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_INVALID_DEVICE_STATE);
    }

    RtlZeroMemory(&Local, sizeof(Local));
    Local.sin_family = AF_INET;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskBind(Socket, (PSOCKADDR)&Local, 0, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    RtlZeroMemory(&Remote, sizeof(Remote));
    Remote.sin_family = AF_INET;
    Remote.sin_addr.s_addr = ECHO_HOST;
    Remote.sin_port = RtlUshortByteSwap(ECHO_PORT);
    Status = STATUS_UNSUCCESSFUL;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskConnect(Socket, (PSOCKADDR)&Remote, 0, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
    }
    if (skip(NT_SUCCESS(Status), "Could not connect to the host echo server: 0x%lx\n", Status))
        goto Close;

    RtlFillMemory(&Peer, sizeof(Peer), 0xCC);
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskGetRemoteAddress(Socket, (PSOCKADDR)&Peer, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_hex(Peer.sin_addr.s_addr, (ULONG)ECHO_HOST);
        ok_eq_uint(Peer.sin_port, RtlUshortByteSwap(ECHO_PORT));
    }

    /* 1. WskReceive gets the echo */
    if (NT_SUCCESS(StreamSend(Socket, &Out, 40)))
    {
        RtlZeroMemory(In.Data, 128);
        Status = StreamReceiveAll(Socket, &In, 0, 40);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlCompareMemory(In.Data, Out.Data, 40) == 40, "Wrong echo\n");
    }

    /* 2. WskReceiveEvent gets it once enabled */
    ResetRecord(EventAccept);
    Record.DisconnectCalls = 0;
    KeClearEvent(&Record.DisconnectEvent);
    Status = SetEvents(Socket, WSK_EVENT_RECEIVE | WSK_EVENT_DISCONNECT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(StreamSend(Socket, &Out, 50)))
    {
        for (i = 0; i < 50 && Record.Length < 50; i++)
        {
            if (!WaitFor(&Record.DataEvent))
                break;
            KeClearEvent(&Record.DataEvent);
        }
        ok_eq_ulong(Record.Length, 50UL);
        ok(RtlCompareMemory(Record.Data, Out.Data, 50) == 50, "Wrong data indicated\n");
        ok_eq_pointer(Record.Context, &Record);
        ok(Record.Calls >= 1, "WskReceiveEvent not called\n");
    }

    /* 3. Retained data stays valid until WskRelease */
    ResetRecord(EventRetain);
    if (NT_SUCCESS(StreamSend(Socket, &Out, 20)))
    {
        ok(WaitFor(&Record.DataEvent), "WskReceiveEvent was not called\n");
        Retained = Record.Retained;
        ok(Retained != NULL, "Indication not retained\n");
        /* Whatever followed is accepted normally */
        for (i = 0; i < 20 && Record.Length < 20; i++)
        {
            KeClearEvent(&Record.DataEvent);
            if (!WaitFor(&Record.DataEvent))
                break;
        }
        ok_eq_ulong(Record.Length, 20UL);
        if (Retained)
        {
            PUCHAR Va = MmGetSystemAddressForMdlSafe(Retained->Buffer.Mdl, NormalPagePriority);
            ok(Retained->Buffer.Length > 0 && Retained->Buffer.Length <= 20, "Length %Iu\n", Retained->Buffer.Length);
            ok(Va != NULL && RtlCompareMemory(Va + Retained->Buffer.Offset, Out.Data, Retained->Buffer.Length) == Retained->Buffer.Length,
               "Retained data changed\n");
            Status = Dispatch->WskRelease(Socket, Retained);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
    }

    /* 4. What the event does not accept is left for WskReceive */
    ResetRecord(EventAcceptPart);
    if (NT_SUCCESS(StreamSend(Socket, &Out, 30)))
    {
        ok(WaitFor(&Record.DataEvent), "WskReceiveEvent was not called\n");
        ok_eq_ulong(Record.Length, 2UL);
        RtlZeroMemory(In.Data, 128);
        RtlCopyMemory(In.Data, Record.Data, 2);
        Status = StreamReceiveAll(Socket, &In, 2, 28);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlCompareMemory(In.Data, Out.Data, 30) == 30, "Wrong data\n");
        ok_eq_ulong(Record.Calls, 1UL);
    }

    /* 5. Indications resume once WskReceive drained the data */
    ResetRecord(EventAccept);
    if (NT_SUCCESS(StreamSend(Socket, &Out, 10)))
    {
        ok(WaitFor(&Record.DataEvent), "WskReceiveEvent was not called\n");
        ok_eq_ulong(Record.Length, 10UL);
    }

    /* 6. Graceful disconnect: the server closes after us and that is indicated */
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskDisconnect(Socket, NULL, 0, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ok(WaitFor(&Record.DisconnectEvent), "WskDisconnectEvent was not called\n");
    ok_eq_ulong(Record.DisconnectCalls, 1UL);
    ok_eq_ulong(Record.DisconnectFlags, 0UL);

    /* Receiving now reports the end of the stream */
    WskBuf.Mdl = In.Mdl;
    WskBuf.Offset = 0;
    WskBuf.Length = 16;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskReceive(Socket, &WskBuf, 0, Call.Irp);
        Information = 0x55;
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 0);
    }

Close:
    CloseSocket(Socket);
Free:
    FreeBuffer(&Out);
    FreeBuffer(&In);
}

/* A WskReceive pending when the socket is closed completes */
static
VOID
TestStreamClosePending(
    _In_ PWSK_PROVIDER_NPI Npi)
{
    PWSK_PROVIDER_CONNECTION_DISPATCH Dispatch;
    PWSK_SOCKET Socket;
    SOCKADDR_IN Local, Remote;
    TEST_BUFFER In = { 0 };
    SYNC_CALL Call, Receive;
    WSK_BUF WskBuf;
    NTSTATUS Status, Returned;

    if (!AllocateBuffer(&In, 64, 0))
        return;
    Socket = CreateSocket(Npi, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_CONNECTION_SOCKET, NULL);
    if (skip(Socket != NULL, "No stream socket\n"))
        goto Free;
    Dispatch = (PWSK_PROVIDER_CONNECTION_DISPATCH)Socket->Dispatch;

    RtlZeroMemory(&Local, sizeof(Local));
    Local.sin_family = AF_INET;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskBind(Socket, (PSOCKADDR)&Local, 0, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    RtlZeroMemory(&Remote, sizeof(Remote));
    Remote.sin_family = AF_INET;
    Remote.sin_addr.s_addr = ECHO_HOST;
    Remote.sin_port = RtlUshortByteSwap(ECHO_PORT);
    Status = STATUS_UNSUCCESSFUL;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskConnect(Socket, (PSOCKADDR)&Remote, 0, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
    }
    if (!skip(NT_SUCCESS(Status), "Could not connect to the host echo server: 0x%lx\n", Status))
    {
        WskBuf.Mdl = In.Mdl;
        WskBuf.Offset = 0;
        WskBuf.Length = 64;
        if (SyncCallBegin(&Receive))
        {
            Returned = Dispatch->WskReceive(Socket, &WskBuf, 0, Receive.Irp);
            ok_eq_hex(Returned, STATUS_PENDING);
            CloseSocket(Socket);
            Socket = NULL;
            Status = SyncCallEnd(&Receive, Returned, NULL);
            ok_eq_hex(Status, STATUS_CANCELLED);
        }
    }
    if (Socket)
        CloseSocket(Socket);
Free:
    FreeBuffer(&In);
}

/* A datagram round trip through the NIC: the host echoes it back */
static
VOID
TestDatagramHostEcho(
    _In_ PWSK_PROVIDER_NPI Npi)
{
    PWSK_PROVIDER_DATAGRAM_DISPATCH Dispatch;
    PWSK_SOCKET Socket;
    SOCKADDR_IN Local, Remote;
    TEST_BUFFER Out = { 0 }, In = { 0 };
    RECEIVE_FROM_CALL Rf;
    ULONG_PTR Information;
    SYNC_CALL Call;
    NTSTATUS Status;
    ULONG i;

    if (!AllocateBuffer(&Out, 64, 0) || !AllocateBuffer(&In, 64, 0))
        goto Free;
    for (i = 0; i < 64; i++)
        ((PUCHAR)Out.Data)[i] = (UCHAR)(0x30 + i);

    Socket = CreateSocketWithContext(Npi, SOCK_DGRAM, IPPROTO_UDP, WSK_FLAG_DATAGRAM_SOCKET,
                                     &Record, &RecordDatagramDispatch);
    if (skip(Socket != NULL, "No datagram socket\n"))
        goto Free;
    Dispatch = (PWSK_PROVIDER_DATAGRAM_DISPATCH)Socket->Dispatch;

    RtlZeroMemory(&Local, sizeof(Local));
    Local.sin_family = AF_INET;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskBind(Socket, (PSOCKADDR)&Local, 0, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    RtlZeroMemory(&Remote, sizeof(Remote));
    Remote.sin_family = AF_INET;
    Remote.sin_addr.s_addr = ECHO_HOST;
    Remote.sin_port = RtlUshortByteSwap(ECHO_PORT);

    /* The test may run before DHCP configured the NIC: wait until a probe
     * datagram goes out, and take its echo */
    for (i = 0; i < 60; i++)
    {
        Status = SendDatagram(Socket, &Remote, &Out, 1);
        if (Status != STATUS_NETWORK_UNREACHABLE && Status != STATUS_HOST_UNREACHABLE)
            break;
        {
            LARGE_INTEGER Delay;
            Delay.QuadPart = -10LL * 1000 * 1000;
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        }
    }
    trace("Network up after %lu s\n", i);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (ReceiveFromBegin(Socket, &Rf, &In, 64))
    {
        Information = 0;
        Status = SyncCallEnd(&Rf.Call, Rf.Returned, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 1);
    }

    /* With WskReceiveFrom */
    RtlZeroMemory(In.Data, 64);
    if (ReceiveFromBegin(Socket, &Rf, &In, 64))
    {
        Status = SendDatagram(Socket, &Remote, &Out, 48);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Information = 0;
        Status = SyncCallEnd(&Rf.Call, Rf.Returned, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 48);
        ok(RtlCompareMemory(In.Data, Out.Data, 48) == 48, "Wrong echo\n");
        ok_eq_hex(Rf.Remote.sin_addr.s_addr, (ULONG)ECHO_HOST);
        ok_eq_uint(Rf.Remote.sin_port, RtlUshortByteSwap(ECHO_PORT));
    }

    /* With the event callback, the way ovpn-dco receives */
    ResetRecord(EventAccept);
    Status = SetEvents(Socket, WSK_EVENT_RECEIVE_FROM);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SendDatagram(Socket, &Remote, &Out, 21);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(WaitFor(&Record.DataEvent), "WskReceiveFromEvent was not called\n");
    ok_eq_ulong(Record.Length, 21UL);
    ok(RtlCompareMemory(Record.Data, Out.Data, 21) == 21, "Wrong echo\n");
    ok_eq_hex(Record.Remote.sin_addr.s_addr, (ULONG)ECHO_HOST);
    ok_eq_uint(Record.Remote.sin_port, RtlUshortByteSwap(ECHO_PORT));

    /* Sends from DISPATCH_LEVEL, the way ovpn-dco sends its keepalives
     * from a timer DPC. The remote address only has to be valid during the
     * call (ovpn-dco passes one from its stack), so clear it right after,
     * before a send deferred to a worker thread can run */
    ResetRecord(EventAccept);
    if (SyncCallBegin(&Call))
    {
        SOCKADDR_IN Target = Remote;
        WSK_BUF WskBuf;
        KIRQL OldIrql;

        WskBuf.Mdl = Out.Mdl;
        WskBuf.Offset = 0;
        WskBuf.Length = 33;
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Status = Dispatch->WskSendTo(Socket, &WskBuf, 0, (PSOCKADDR)&Target, 0, NULL, Call.Irp);
        RtlZeroMemory(&Target, sizeof(Target));
        KeLowerIrql(OldIrql);
        Information = 0;
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 33);
        ok(WaitFor(&Record.DataEvent), "No echo of the DISPATCH_LEVEL WskSendTo\n");
        ok_eq_ulong(Record.Length, 33UL);
    }

    ResetRecord(EventAccept);
    if (SyncCallBegin(&Call))
    {
        SOCKADDR_IN Target = Remote;
        WSK_BUF_LIST List;
        KIRQL OldIrql;

        List.Next = NULL;
        List.Buffer.Mdl = Out.Mdl;
        List.Buffer.Offset = 4;
        List.Buffer.Length = 17;
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Status = Dispatch->WskSendMessages(Socket, &List, 0, (PSOCKADDR)&Target, 0, NULL, Call.Irp);
        RtlZeroMemory(&Target, sizeof(Target));
        KeLowerIrql(OldIrql);
        Information = 0;
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, 17);
        ok(WaitFor(&Record.DataEvent), "No echo of the DISPATCH_LEVEL WskSendMessages\n");
        ok_eq_ulong(Record.Length, 17UL);
        ok(RtlCompareMemory(Record.Data, (PUCHAR)Out.Data + 4, 17) == 17, "Wrong echo\n");
    }

    CloseSocket(Socket);
Free:
    FreeBuffer(&Out);
    FreeBuffer(&In);
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
    WSK_CLIENT_NPI ClientNpi;
    WSK_REGISTRATION Registration;
    WSK_PROVIDER_NPI Npi;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    if (ControlCode != IOCTL_NETIO_WSK_RUN_TESTS)
        return STATUS_NOT_SUPPORTED;

    ok_irql(PASSIVE_LEVEL);

    ClientNpi.ClientContext = NULL;
    ClientNpi.Dispatch = &ClientDispatch;
    Status = WskRegister(&ClientNpi, &Registration);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    Status = WskCaptureProviderNPI(&Registration, WSK_INFINITE_WAIT, &Npi);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        KeInitializeSpinLock(&Record.Lock);
        KeInitializeEvent(&Record.DataEvent, NotificationEvent, FALSE);
        KeInitializeEvent(&Record.DisconnectEvent, NotificationEvent, FALSE);

        TestDatagram(&Npi);
        TestDatagramReceive(&Npi, LOOPBACK);
        TestDatagramHostEcho(&Npi);
        TestStreamCloseUnconnected(&Npi);
        TestStreamReceive(&Npi);
        TestStreamClosePending(&Npi);
        WskReleaseProviderNPI(&Registration);
    }

    WskDeregister(&Registration);
    return STATUS_SUCCESS;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"NetioWsk";
    return KmtRegisterMessageHandler(IOCTL_NETIO_WSK_RUN_TESTS, NULL, TestMessageHandler);
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
}
