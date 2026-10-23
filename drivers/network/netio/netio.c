/*
 * PROJECT:     NetIO driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NT6 compatible NETIO.SYS driver: A BSD sockets like kernel
 *              internal interface for networking (TCP/IP, UDP/IP).
 * COPYRIGHT:   Copyright 2023-2025 Johannes Khoshnazar-Thoma <johannes@johannesthoma.com>
 */

/*
Copyright © 2025 Johannes Khoshnazar-Thoma <johannes@johannesthoma.com>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
 * [NETIO] NETIO.SYS driver
 *
 * This files adds the NETIO.SYS driver to ReactOS. It is not
 * feature complete (meaning some functionality is unimplemented)
 * but does its job quite good for what it originally was written
 * for (which is getting WinDRBD working on ReactOS/Windows 2003 SP2).
 *
 * The driver re-uses parts of the AFD.SYS driver, namely those
 * functions that ease communitating with the transport device
 * interface (TDI). Other than that, following features are implemented
 * and should work:
 *
 *     * TCP/IP networking: connect, listen, accept, read, write,
 *       receive and disconnect event callbacks
 *     * UDP/IP networking: write, read, receive event callbacks
 *
 * ICMP support does not exist yet, and neither does IPv6.
 * In particular the listen/accept mechanism allows one to write
 * kernel side TCP servers that one can connect to via the internet.
 */

#include <ntifs.h>
#include <netiodef.h>
#include <ws2def.h>
#include <wsk.h>
#include <ndis.h>

/* If you want to see the DPRINT() output in your debugger,
 * remove the following line (or comment it out):
 * Note that DPRINT1() is always printed to the debugger. */
#define NDEBUG
#include <reactos/debug.h>

#include <tdi.h>
#include <tcpioctl.h>
#include <tdikrnl.h>
#include <tdiinfo.h>
#include <tdi_proto.h>
#include <tdiconn.h>

#define TAG_NETIO 'OIEN'
#define TAG_NETIO_RECV 'rIEN'

#ifndef TAG_AFD_TDI_CONNECTION_INFORMATION
#define TAG_AFD_TDI_CONNECTION_INFORMATION 'cTfA'
#endif

/* AFD Share Flags */
#define AFD_SHARE_UNIQUE    0x0L
#define AFD_SHARE_REUSE     0x1L
#define AFD_SHARE_WILDCARD  0x2L
#define AFD_SHARE_EXCLUSIVE 0x3L

/* Function trace */
// #define FUNCTION_TRACE DPRINT1("%s() ...\n", __FUNCTION__)
#define FUNCTION_TRACE do { } while (0)

typedef struct _WSK_SOCKET_INTERNAL
{
    WSK_SOCKET Socket;

    ADDRESS_FAMILY family;      /* AF_INET or AF_INET6 */
    USHORT type;                /* SOCK_DGRAM, SOCK_STREAM, ... */
    ULONG proto;                /* IPPROTO_UDP, IPPROTO_TCP */
    ULONG WskFlags;             /* WSK_FLAG_LISTEN_SOCKET, ... */
    PVOID user_context;         /* parameter for callbacks, opaque */
    UNICODE_STRING TdiName;     /* \\Devices\\Tcp, \\Devices\\Udp */

    struct sockaddr LocalAddress;
    PFILE_OBJECT LocalAddressFile;
    HANDLE LocalAddressHandle;

    struct sockaddr RemoteAddress;      /* Remember latest remote connection */
    BOOLEAN FixedRemoteAddress;         /* Datagram: set by SIO_WSK_SET_REMOTE_ADDRESS */
    PFILE_OBJECT RemoteAddressFile;
    HANDLE RemoteAddressHandle;

    /* Those exist for connection oriented (TCP/IP) sockets only */
    PFILE_OBJECT ConnectionFile;        /* Returned by TdiOpenConnectionEndpointFile() */
    HANDLE ConnectionHandle;            /* Returned by TdiOpenConnectionEndpointFile() */
    BOOLEAN ConnectionFileAssociated;

    /* Client event callbacks given to WskSocket (or returned by the
     * client's WskAcceptEvent): a WSK_CLIENT_LISTEN_DISPATCH,
     * WSK_CLIENT_DATAGRAM_DISPATCH or WSK_CLIENT_CONNECTION_DISPATCH
     * depending on the socket type. */
    const VOID *ClientDispatch;
    volatile ULONG CallbackMask;

    /* Receive side, protected by RecvLock */
    KSPIN_LOCK RecvLock;
    LIST_ENTRY RecvRequests;       /* NETIO_RECV_REQUEST: pending WskReceive(From) IRPs */
    BOOLEAN Closing;               /* WskCloseSocket was called */

    /* Datagram sockets: inbound datagrams come from a TDI receive
     * datagram event handler on LocalAddressFile. Those nobody took yet
     * wait on the backlog. */
    LIST_ENTRY DatagramBacklog;    /* NETIO_DATAGRAM */
    ULONG BacklogCount;
    ULONG BacklogBytes;
    BOOLEAN DatagramHandlerRegistered;
    EX_RUNDOWN_REF EventRundown;   /* held while the TDI event handler runs */

    /* Connection oriented sockets: once connected, a TDI receive into a
     * NETIO_CHUNK is kept posted while someone wants data. What was
     * received and not consumed yet is in Chunk. */
    BOOLEAN Connected;
    BOOLEAN PumpActive;            /* receive work item queued or TDI receive pending */
    BOOLEAN RecvShutdown;          /* the peer closed or the connection broke */
    BOOLEAN DisconnectIndicated;
    BOOLEAN IndicationBlocked;     /* client did not accept all data: wait for WskReceive */
    BOOLEAN Delivering;
    BOOLEAN DeliverAgain;
    NTSTATUS RecvShutdownStatus;
    struct _NETIO_CHUNK *Chunk;
    WORK_QUEUE_ITEM PumpWorkItem;

    UINT Flags;                    /* SO_REUSEADDR, ... see ws2def.h */
    LONG RefCount;                 /* See ReferenceSocket/DereferenceSocket */

    PIRP ListenIrp;	           /* must be cancelled on close */
    volatile BOOLEAN ListenCancelled;
    HANDLE ListenThreadHandle;     /* needed to restart listening */
    PKTHREAD ListenThread;
    KEVENT StartListenEvent;
    volatile BOOLEAN ListenThreadShouldRun;

    /* AcceptSocket's keep a reference on their listen sockets so
     * that the Address file will be closed only if there are no
     * open connections any more.
     */
    struct _WSK_SOCKET_INTERNAL *ListenSocket;

    /* DereferenceSocket() only works at PASSIVE_LEVEL. If > PASSIVE_LEVEL,
     * it will put the socket on this list and wake the putsockets
     * thread.
     */
    struct _WSK_SOCKET_INTERNAL *NextSocketToDereference;
    ULONG NumSocketDereferences;
} WSK_SOCKET_INTERNAL, *PWSK_SOCKET_INTERNAL;

typedef struct _NETIO_CONTEXT
{
    PIRP UserIrp;
    PWSK_SOCKET_INTERNAL socket;
    PTDI_CONNECTION_INFORMATION TargetConnectionInfo;
    PTDI_CONNECTION_INFORMATION PeerAddrRet;
} NETIO_CONTEXT, *PNETIO_CONTEXT;

static void
ReferenceSocket(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    FUNCTION_TRACE;

    InterlockedIncrement(&Socket->RefCount);
}

static void
DereferenceSocket(_In_ PWSK_SOCKET_INTERNAL Socket);

static VOID
ReceiveShutdown(_In_ PWSK_SOCKET_INTERNAL Socket);

static VOID
FreeChunk(_In_ struct _NETIO_CHUNK *Chunk);

/* This function can be called several times on the same socket.
   In particular it will be called by WskClose and later by the
   DereferenceSocket() of the socket.
 */

static
void SocketShutdown(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    NTSTATUS status;

    FUNCTION_TRACE;

    ReceiveShutdown(Socket);

    if (Socket->ListenSocket != NULL)
    {
        DereferenceSocket(Socket->ListenSocket);
        Socket->ListenSocket = NULL;
    }
    if (Socket->ListenThreadHandle != NULL)
    {
        Socket->ListenThreadShouldRun = FALSE;
        KeSetEvent(&Socket->StartListenEvent, IO_NO_INCREMENT, FALSE);
        status = KeWaitForSingleObject(Socket->ListenThread, Executive, KernelMode, FALSE, (PLARGE_INTEGER)NULL);
        if (!NT_SUCCESS(status))
        {
            DPRINT1("KeWaitForSingleObject failed with status 0x%08x!\n", status);
        }
        ObDereferenceObject(Socket->ListenThread);
        Socket->ListenThread = NULL;
        Socket->ListenThreadHandle = NULL;
    }
    if (Socket->ListenIrp != NULL)
    {
        Socket->ListenCancelled = TRUE;
        IoCancelIrp(Socket->ListenIrp);
        Socket->ListenIrp = NULL;
    }
    if (Socket->ConnectionFile != NULL)
    {
        if (Socket->ConnectionFileAssociated)
        {
            /* This fails with error STATUS_CONNECTION_ACTIVE on Windows 2003 */
            status = TdiDisassociateAddressFile(Socket->ConnectionFile);
            if (!NT_SUCCESS(status))
            {
                DPRINT1("Warning: TdiDisassociateAddressFile returned status %08x\n", status);
            }
            Socket->ConnectionFileAssociated = FALSE;
        }
        ObDereferenceObject(Socket->ConnectionFile);
        Socket->ConnectionFile = NULL;
    }
    if (Socket->ConnectionHandle != NULL)
    {
        ZwClose(Socket->ConnectionHandle);
        Socket->ConnectionHandle = NULL;
    }
}

static void
SocketDestroy(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    SocketShutdown(Socket);

    /* Especially listen sockets must keep the LocalAddressFile
     * open when being closed and there are still accepted sockets
     * somewhere. Else the accepted socket I/O will fail with
     * STATUS_INVALID_DEVICE_STATE. So do not close the
     * address file in Shutdown only close it here when all AcceptSockets
     * are gone (AcceptSockets hold a reference to the listen socket).
     */

    if (Socket->LocalAddressFile != NULL)
    {
        ObDereferenceObject(Socket->LocalAddressFile);
        Socket->LocalAddressFile = NULL;
    }
    if (Socket->LocalAddressHandle != NULL)
    {
        ZwClose(Socket->LocalAddressHandle);
        Socket->LocalAddressHandle = NULL;
    }
    /* Received stream data nobody consumed */
    if (Socket->Chunk != NULL)
    {
        FreeChunk(Socket->Chunk);
        Socket->Chunk = NULL;
    }
    ExFreePoolWithTag(Socket, TAG_NETIO);
}

static void
DereferenceSocketSynchronous(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    if (InterlockedDecrement(&Socket->RefCount) == 0)
    {
        SocketDestroy(Socket);
    }
}

static PWSK_SOCKET_INTERNAL SocketsToDereference = NULL;
static KSPIN_LOCK SocketsToDereferenceListLock = 0;
static volatile BOOLEAN DereferenceSocketsThreadShouldRun = FALSE;
static KEVENT DereferenceSocketsEvent;
static HANDLE DereferenceSocketsThreadHandle;

static VOID NTAPI DereferenceSocketsThread(_In_opt_ void *p)
{
    NTSTATUS status;
    PWSK_SOCKET_INTERNAL SocketToDereference;
    KIRQL OldIrql;
    ULONG NumSocketDereferences;

    FUNCTION_TRACE;

    while (DereferenceSocketsThreadShouldRun)
    {
        status = KeWaitForSingleObject(&DereferenceSocketsEvent, Executive, KernelMode, FALSE, NULL);
        if (!NT_SUCCESS(status))
        {
            DPRINT1("KeWaitForSingleObject failed with status 0x%08x!\n", status);
        }

        KeAcquireSpinLock(&SocketsToDereferenceListLock, &OldIrql);
        while (SocketsToDereference != NULL)
        {
            SocketToDereference = SocketsToDereference;
            SocketsToDereference = SocketToDereference->NextSocketToDereference;
            NumSocketDereferences = SocketToDereference->NumSocketDereferences;

            while (NumSocketDereferences > 0)
            {
                SocketToDereference->NumSocketDereferences--;
                NumSocketDereferences = SocketToDereference->NumSocketDereferences;

                KeReleaseSpinLock(&SocketsToDereferenceListLock, OldIrql);
                DereferenceSocketSynchronous(SocketToDereference);
                KeAcquireSpinLock(&SocketsToDereferenceListLock, &OldIrql);
            }
        }
        KeReleaseSpinLock(&SocketsToDereferenceListLock, OldIrql);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS StartSocketDereferenceThread(void)
{
    NTSTATUS status;

    KeInitializeSpinLock(&SocketsToDereferenceListLock);
    KeInitializeEvent(&DereferenceSocketsEvent, SynchronizationEvent, FALSE);
    DereferenceSocketsThreadShouldRun = TRUE;

    status = PsCreateSystemThread(&DereferenceSocketsThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, DereferenceSocketsThread, NULL);
    if (!NT_SUCCESS(status))
    {
        DPRINT1("Could not start put sockets thread, status is %x\n", status);
    }
    return status;
}

static void StopSocketDereferenceThread(void)
{
    DereferenceSocketsThreadShouldRun = FALSE;
    KeSetEvent(&DereferenceSocketsEvent, IO_NO_INCREMENT, FALSE);

    /* eventually it will terminate, no need to wait for that. */
}

static void
DereferenceSocket(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    KIRQL OldIrql;

    FUNCTION_TRACE;

    /* We are doing calls like ZwClose, TdiDisassociateAddressFile, ...
       which require IRQL to be PASSIVE_LEVEL. So if we get here
       when IRQL is greater than PASSIVE_LEVEL, defer the DereferenceSocket()
       to a separate thread. */

    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
    {
        KeAcquireSpinLock(&SocketsToDereferenceListLock, &OldIrql);
        if (Socket->NumSocketDereferences == 0)
        {
            Socket->NextSocketToDereference = SocketsToDereference;
            SocketsToDereference = Socket;
            Socket->NumSocketDereferences = 1;
        }
        else
        {
            Socket->NumSocketDereferences++;
        }
        KeReleaseSpinLock(&SocketsToDereferenceListLock, OldIrql);

        KeSetEvent(&DereferenceSocketsEvent, IO_NO_INCREMENT, FALSE);

        return;
    }
    DereferenceSocketSynchronous(Socket);
}

/* ---------------------------------------------------------------------
 * Receive support
 *
 * Datagram sockets register a TDI_EVENT_RECEIVE_DATAGRAM handler on their
 * address file when they are bound. Each inbound datagram goes to the
 * oldest pending WskReceiveFrom IRP, else to the client's
 * WskReceiveFromEvent callback, else to a bounded backlog that later
 * WskReceiveFrom calls drain.
 *
 * Connection oriented sockets keep one TDI receive posted into a
 * NETIO_CHUNK while there is a consumer (a pending WskReceive IRP or an
 * enabled WSK_EVENT_RECEIVE callback). The data received is handed to
 * WskReceive IRPs or indicated to WskReceiveEvent; the next TDI receive
 * is posted only once it is consumed, so the transport's flow control
 * keeps working. A receive that completes with no data (graceful close)
 * or with an error ends the receive side and is reported through
 * WskDisconnectEvent.
 * --------------------------------------------------------------------- */

#define NETIO_CHUNK_SIZE          0x4000
#define NETIO_BACKLOG_MAX_COUNT   128
#define NETIO_BACKLOG_MAX_BYTES   (256 * 1024)

typedef struct _NETIO_RECV_REQUEST
{
    LIST_ENTRY Entry;
    PIRP Irp;
    PWSK_SOCKET_INTERNAL Socket;
    WSK_BUF Buffer;
    /* WskReceiveFrom only */
    PSOCKADDR RemoteAddress;
    PULONG ControlLength;
    PULONG ControlFlags;
    BOOLEAN Queued;
} NETIO_RECV_REQUEST, *PNETIO_RECV_REQUEST;

typedef struct _NETIO_DATAGRAM
{
    WSK_DATAGRAM_INDICATION Indication;     /* must stay first */
    LIST_ENTRY Entry;
    SOCKADDR_IN Remote;
    PMDL Mdl;
    ULONG Length;
    UCHAR Data[ANYSIZE_ARRAY];
} NETIO_DATAGRAM, *PNETIO_DATAGRAM;

typedef struct _NETIO_CHUNK
{
    WSK_DATA_INDICATION Indication;         /* must stay first */
    PWSK_SOCKET_INTERNAL Socket;
    PMDL Mdl;
    ULONG Offset;                           /* unconsumed data */
    ULONG Length;
    ULONG Size;
    UCHAR Data[ANYSIZE_ARRAY];
} NETIO_CHUNK, *PNETIO_CHUNK;

static ULONG
CallbackFlags(VOID)
{
    return (KeGetCurrentIrql() >= DISPATCH_LEVEL) ? WSK_FLAG_AT_DISPATCH_LEVEL : 0;
}

/* Copies Length bytes from Source into the memory a WSK_BUF describes,
 * an MDL chain whose first Offset bytes are skipped. Returns how much
 * fit. Callable at DISPATCH_LEVEL. */
static SIZE_T
CopyToWskBuf(_In_ const WSK_BUF *Buffer, _In_reads_bytes_(Length) const UCHAR *Source, _In_ SIZE_T Length)
{
    PMDL Mdl = Buffer->Mdl;
    ULONG Offset = Buffer->Offset;
    SIZE_T Room = Buffer->Length;
    SIZE_T Done = 0, Count;
    ULONG MdlLength;
    PUCHAR Va;

    while (Mdl != NULL && Done < Length && Room > 0)
    {
        MdlLength = MmGetMdlByteCount(Mdl);
        if (Offset >= MdlLength)
        {
            Offset -= MdlLength;
            Mdl = Mdl->Next;
            continue;
        }
        Va = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        if (Va == NULL)
            break;
        Count = min(MdlLength - Offset, Length - Done);
        Count = min(Count, Room);
        RtlCopyMemory(Va + Offset, Source + Done, Count);
        Done += Count;
        Room -= Count;
        Offset = 0;
        Mdl = Mdl->Next;
    }
    return Done;
}

static VOID
CompleteRecvRequest(_In_ PNETIO_RECV_REQUEST Request, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    PWSK_SOCKET_INTERNAL Socket = Request->Socket;
    PIRP Irp = Request->Irp;

    ExFreePoolWithTag(Request, TAG_NETIO_RECV);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    DereferenceSocket(Socket);
}

static VOID NTAPI
CancelRecvRequest(_Inout_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PNETIO_RECV_REQUEST Request = Irp->Tail.Overlay.DriverContext[0];
    PWSK_SOCKET_INTERNAL Socket = Request->Socket;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    /* Whoever cleared the cancel routine owns the IRP: that is us. Just
     * make sure it is no longer queued. */
    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    if (Request->Queued)
    {
        RemoveEntryList(&Request->Entry);
        Request->Queued = FALSE;
    }
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    CompleteRecvRequest(Request, STATUS_CANCELLED, 0);
}

/* Allocates the request tracking a WskReceive(From) IRP. The IRP is
 * marked pending and holds a socket reference until it completes. */
static PNETIO_RECV_REQUEST
CreateRecvRequest(_In_ PWSK_SOCKET_INTERNAL Socket, _In_ PWSK_BUF Buffer, _In_ PIRP Irp)
{
    PNETIO_RECV_REQUEST Request;

    Request = ExAllocatePoolZero(NonPagedPool, sizeof(*Request), TAG_NETIO_RECV);
    if (Request == NULL)
        return NULL;
    Request->Irp = Irp;
    Request->Socket = Socket;
    Request->Buffer = *Buffer;
    Irp->Tail.Overlay.DriverContext[0] = Request;
    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);
    return Request;
}

/* Puts a request on the socket's queue. Fails it if the socket is closing,
 * and completes it if it was cancelled already. */
static VOID
QueueRecvRequest(_In_ PNETIO_RECV_REQUEST Request)
{
    PWSK_SOCKET_INTERNAL Socket = Request->Socket;
    PIRP Irp = Request->Irp;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    if (Socket->Closing)
    {
        KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
        CompleteRecvRequest(Request, STATUS_INVALID_DEVICE_STATE, 0);
        return;
    }
    IoSetCancelRoutine(Irp, CancelRecvRequest);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL)
    {
        KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
        CompleteRecvRequest(Request, STATUS_CANCELLED, 0);
        return;
    }
    /* If the cancel routine is already running, it will dequeue it */
    InsertTailList(&Socket->RecvRequests, &Request->Entry);
    Request->Queued = TRUE;
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
}

/* Takes the oldest request that is not being cancelled. RecvLock held. */
static PNETIO_RECV_REQUEST
PopRecvRequest(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    PNETIO_RECV_REQUEST Request;

    while (!IsListEmpty(&Socket->RecvRequests))
    {
        Request = CONTAINING_RECORD(RemoveHeadList(&Socket->RecvRequests), NETIO_RECV_REQUEST, Entry);
        Request->Queued = FALSE;
        if (IoSetCancelRoutine(Request->Irp, NULL) != NULL)
            return Request;
        /* Being cancelled: the cancel routine completes it */
    }
    return NULL;
}

/* Datagrams */

static PNETIO_DATAGRAM
AllocateDatagram(_In_reads_bytes_(Length) const VOID *Data, _In_ ULONG Length, _In_ const SOCKADDR_IN *Remote)
{
    PNETIO_DATAGRAM Datagram;

    Datagram = ExAllocatePoolUninitialized(NonPagedPool,
                                           FIELD_OFFSET(NETIO_DATAGRAM, Data[max(Length, 1)]),
                                           TAG_NETIO_RECV);
    if (Datagram == NULL)
        return NULL;

    Datagram->Mdl = IoAllocateMdl(Datagram->Data, max(Length, 1), FALSE, FALSE, NULL);
    if (Datagram->Mdl == NULL)
    {
        ExFreePoolWithTag(Datagram, TAG_NETIO_RECV);
        return NULL;
    }
    MmBuildMdlForNonPagedPool(Datagram->Mdl);

    RtlCopyMemory(Datagram->Data, Data, Length);
    Datagram->Length = Length;
    Datagram->Remote = *Remote;

    Datagram->Indication.Next = NULL;
    Datagram->Indication.Buffer.Mdl = Datagram->Mdl;
    Datagram->Indication.Buffer.Offset = 0;
    Datagram->Indication.Buffer.Length = Length;
    Datagram->Indication.ControlInfo = NULL;
    Datagram->Indication.ControlInfoLength = 0;
    Datagram->Indication.RemoteAddress = (PSOCKADDR)&Datagram->Remote;
    return Datagram;
}

static VOID
FreeDatagram(_In_ PNETIO_DATAGRAM Datagram)
{
    IoFreeMdl(Datagram->Mdl);
    ExFreePoolWithTag(Datagram, TAG_NETIO_RECV);
}

static VOID
CompleteDatagramRequest(
    _In_ PNETIO_RECV_REQUEST Request,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length,
    _In_ const SOCKADDR_IN *Remote)
{
    SIZE_T Copied;

    Copied = CopyToWskBuf(&Request->Buffer, Data, Length);
    if (Request->RemoteAddress != NULL)
        RtlCopyMemory(Request->RemoteAddress, Remote, sizeof(*Remote));
    if (Request->ControlLength != NULL)
        *Request->ControlLength = 0;
    if (Request->ControlFlags != NULL)
        *Request->ControlFlags = (Copied < Length) ? MSG_TRUNC : 0;
    CompleteRecvRequest(Request, STATUS_SUCCESS, Copied);
}

/* Hands backlogged datagrams to pending WskReceiveFrom IRPs */
static VOID
DrainDatagramBacklog(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    PNETIO_RECV_REQUEST Request;
    PNETIO_DATAGRAM Datagram;
    KIRQL OldIrql;

    for (;;)
    {
        KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
        if (IsListEmpty(&Socket->DatagramBacklog))
        {
            KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
            return;
        }
        Request = PopRecvRequest(Socket);
        if (Request == NULL)
        {
            KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
            return;
        }
        Datagram = CONTAINING_RECORD(RemoveHeadList(&Socket->DatagramBacklog), NETIO_DATAGRAM, Entry);
        Socket->BacklogCount--;
        Socket->BacklogBytes -= Datagram->Length;
        KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

        CompleteDatagramRequest(Request, Datagram->Data, Datagram->Length, &Datagram->Remote);
        FreeDatagram(Datagram);
    }
}

/* Indicates backlogged datagrams once the client enabled WskReceiveFromEvent */
static VOID
IndicateDatagramBacklog(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    const WSK_CLIENT_DATAGRAM_DISPATCH *Dispatch = Socket->ClientDispatch;
    PNETIO_DATAGRAM Datagram;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (Dispatch == NULL || Dispatch->WskReceiveFromEvent == NULL)
        return;

    for (;;)
    {
        KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
        if (Socket->Closing || !(Socket->CallbackMask & WSK_EVENT_RECEIVE_FROM) ||
            IsListEmpty(&Socket->DatagramBacklog) || !IsListEmpty(&Socket->RecvRequests))
        {
            KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
            break;
        }
        Datagram = CONTAINING_RECORD(RemoveHeadList(&Socket->DatagramBacklog), NETIO_DATAGRAM, Entry);
        Socket->BacklogCount--;
        Socket->BacklogBytes -= Datagram->Length;
        KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

        Status = Dispatch->WskReceiveFromEvent(Socket->user_context, CallbackFlags(), &Datagram->Indication);
        if (Status == STATUS_PENDING)
            continue;
        if (Status != STATUS_DATA_NOT_ACCEPTED)
        {
            FreeDatagram(Datagram);
            continue;
        }

        /* Not accepted: it waits for WskReceiveFrom, in front */
        KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
        InsertHeadList(&Socket->DatagramBacklog, &Datagram->Entry);
        Socket->BacklogCount++;
        Socket->BacklogBytes += Datagram->Length;
        KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
        break;
    }

    /* A WskReceiveFrom may have been queued meanwhile */
    DrainDatagramBacklog(Socket);
}

/* TDI_EVENT_RECEIVE_DATAGRAM handler, runs at DISPATCH_LEVEL */
static NTSTATUS NTAPI
ReceiveDatagramEventHandler(
    _In_opt_ PVOID TdiEventContext,
    _In_ LONG SourceAddressLength,
    _In_reads_bytes_(SourceAddressLength) PVOID SourceAddress,
    _In_ LONG OptionsLength,
    _In_reads_bytes_opt_(OptionsLength) PVOID Options,
    _In_ ULONG ReceiveDatagramFlags,
    _In_ ULONG BytesIndicated,
    _In_ ULONG BytesAvailable,
    _Out_ ULONG *BytesTaken,
    _In_ PVOID Tsdu,
    _Out_opt_ PIRP *IoRequestPacket)
{
    PWSK_SOCKET_INTERNAL Socket = TdiEventContext;
    const WSK_CLIENT_DATAGRAM_DISPATCH *Dispatch;
    PTA_IP_ADDRESS Source = SourceAddress;
    PNETIO_RECV_REQUEST Request;
    PNETIO_DATAGRAM Datagram;
    SOCKADDR_IN Remote;
    BOOLEAN Indicate;
    NTSTATUS Status;
    KIRQL OldIrql;

    *BytesTaken = BytesAvailable;
    if (IoRequestPacket != NULL)
        *IoRequestPacket = NULL;

    if (!ExAcquireRundownProtection(&Socket->EventRundown))
        return STATUS_SUCCESS;

    RtlZeroMemory(&Remote, sizeof(Remote));
    Remote.sin_family = AF_INET;
    if (SourceAddressLength >= (LONG)sizeof(TA_IP_ADDRESS) &&
        Source->TAAddressCount >= 1 &&
        Source->Address[0].AddressType == TDI_ADDRESS_TYPE_IP)
    {
        Remote.sin_port = Source->Address[0].Address[0].sin_port;
        Remote.sin_addr.s_addr = Source->Address[0].Address[0].in_addr;
    }

    /* The transport indicates whole datagrams */
    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    if (Socket->Closing)
    {
        KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
        goto out;
    }
    Request = PopRecvRequest(Socket);
    Dispatch = Socket->ClientDispatch;
    Indicate = (Request == NULL &&
                (Socket->CallbackMask & WSK_EVENT_RECEIVE_FROM) &&
                Dispatch != NULL && Dispatch->WskReceiveFromEvent != NULL &&
                IsListEmpty(&Socket->DatagramBacklog));
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    if (Request != NULL)
    {
        CompleteDatagramRequest(Request, Tsdu, BytesIndicated, &Remote);
        goto out;
    }

    Datagram = AllocateDatagram(Tsdu, BytesIndicated, &Remote);
    if (Datagram == NULL)
        goto out;

    if (Indicate)
    {
        Status = Dispatch->WskReceiveFromEvent(Socket->user_context, CallbackFlags(), &Datagram->Indication);
        if (Status == STATUS_PENDING)
        {
            /* Retained by the client until it calls WskRelease */
            goto out;
        }
        if (Status != STATUS_DATA_NOT_ACCEPTED)
        {
            FreeDatagram(Datagram);
            goto out;
        }
        /* Not accepted: keep it for WskReceiveFrom */
    }

    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    if (!Socket->Closing &&
        Socket->BacklogCount < NETIO_BACKLOG_MAX_COUNT &&
        Socket->BacklogBytes + Datagram->Length <= NETIO_BACKLOG_MAX_BYTES)
    {
        InsertTailList(&Socket->DatagramBacklog, &Datagram->Entry);
        Socket->BacklogCount++;
        Socket->BacklogBytes += Datagram->Length;
        Datagram = NULL;
    }
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    if (Datagram != NULL)
    {
        /* Backlog full: drop it, like a full socket buffer would */
        FreeDatagram(Datagram);
    }
    else
    {
        /* A WskReceiveFrom may have been queued meanwhile */
        DrainDatagramBacklog(Socket);
    }

out:
    ExReleaseRundownProtection(&Socket->EventRundown);
    return STATUS_SUCCESS;
}

/* Connection oriented sockets */

static PNETIO_CHUNK
AllocateChunk(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    PNETIO_CHUNK Chunk;

    Chunk = ExAllocatePoolUninitialized(NonPagedPool,
                                        FIELD_OFFSET(NETIO_CHUNK, Data[NETIO_CHUNK_SIZE]),
                                        TAG_NETIO_RECV);
    if (Chunk == NULL)
        return NULL;
    Chunk->Mdl = IoAllocateMdl(Chunk->Data, NETIO_CHUNK_SIZE, FALSE, FALSE, NULL);
    if (Chunk->Mdl == NULL)
    {
        ExFreePoolWithTag(Chunk, TAG_NETIO_RECV);
        return NULL;
    }
    MmBuildMdlForNonPagedPool(Chunk->Mdl);
    Chunk->Socket = Socket;
    Chunk->Size = NETIO_CHUNK_SIZE;
    Chunk->Offset = 0;
    Chunk->Length = 0;
    Chunk->Indication.Next = NULL;
    return Chunk;
}

static VOID
FreeChunk(_In_ PNETIO_CHUNK Chunk)
{
    IoFreeMdl(Chunk->Mdl);
    ExFreePoolWithTag(Chunk, TAG_NETIO_RECV);
}

static VOID NTAPI StreamReceiveWorker(_In_ PVOID Parameter);

/* Starts a TDI receive if someone wants data and none is pending.
 * Callable at DISPATCH_LEVEL: the receive is posted from a work item. */
static VOID
StreamKickReceive(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    BOOLEAN Queue = FALSE;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    if (!Socket->Closing && Socket->Connected && !Socket->RecvShutdown &&
        !Socket->PumpActive && Socket->Chunk == NULL &&
        ((Socket->CallbackMask & WSK_EVENT_RECEIVE) || !IsListEmpty(&Socket->RecvRequests)))
    {
        Socket->PumpActive = TRUE;
        Queue = TRUE;
    }
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    if (Queue)
    {
        ReferenceSocket(Socket);
        ExInitializeWorkItem(&Socket->PumpWorkItem, StreamReceiveWorker, Socket);
        ExQueueWorkItem(&Socket->PumpWorkItem, DelayedWorkQueue);
    }
}

/* Hands received stream data to WskReceive IRPs and WskReceiveEvent, and
 * reports the end of the stream. Only one caller delivers at a time;
 * others ask it to go around again. */
static VOID
StreamDeliver(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    const WSK_CLIENT_CONNECTION_DISPATCH *Dispatch = Socket->ClientDispatch;
    PNETIO_RECV_REQUEST Request;
    PNETIO_CHUNK Chunk, Done;
    SIZE_T Copied, Accepted;
    NTSTATUS Status;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    if (Socket->Delivering)
    {
        Socket->DeliverAgain = TRUE;
        KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
        return;
    }
    Socket->Delivering = TRUE;

    do
    {
        Socket->DeliverAgain = FALSE;
        Done = NULL;

        /* Pending WskReceive IRPs first: they were asked for */
        while ((Chunk = Socket->Chunk) != NULL && Chunk->Length > 0 &&
               (Request = PopRecvRequest(Socket)) != NULL)
        {
            KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
            /* Only the deliverer consumes the chunk, no need to hold the lock */
            Copied = CopyToWskBuf(&Request->Buffer, Chunk->Data + Chunk->Offset, Chunk->Length);
            CompleteRecvRequest(Request, STATUS_SUCCESS, Copied);
            KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
            Chunk->Offset += (ULONG)Copied;
            Chunk->Length -= (ULONG)Copied;
        }

        Chunk = Socket->Chunk;
        if (Chunk != NULL && Chunk->Length == 0)
        {
            Socket->Chunk = NULL;
            Socket->IndicationBlocked = FALSE;
            Done = Chunk;
            Chunk = NULL;
        }

        if (Chunk != NULL && !Socket->IndicationBlocked && !Socket->Closing &&
            (Socket->CallbackMask & WSK_EVENT_RECEIVE) &&
            Dispatch != NULL && Dispatch->WskReceiveEvent != NULL)
        {
            KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

            Chunk->Indication.Next = NULL;
            Chunk->Indication.Buffer.Mdl = Chunk->Mdl;
            Chunk->Indication.Buffer.Offset = Chunk->Offset;
            Chunk->Indication.Buffer.Length = Chunk->Length;
            /* Clients only set it when they take part of the data */
            Accepted = Chunk->Length;
            Status = Dispatch->WskReceiveEvent(Socket->user_context, CallbackFlags(),
                                               &Chunk->Indication, Chunk->Length, &Accepted);

            KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
            if (Status == STATUS_PENDING)
            {
                /* Retained by the client until it calls WskRelease */
                Socket->Chunk = NULL;
            }
            else if (Status == STATUS_SUCCESS && Accepted >= Chunk->Length)
            {
                Socket->Chunk = NULL;
                Done = Chunk;
            }
            else
            {
                /* Partly or not accepted: the rest waits for WskReceive */
                if (Status == STATUS_SUCCESS)
                {
                    Chunk->Offset += (ULONG)Accepted;
                    Chunk->Length -= (ULONG)Accepted;
                }
                Socket->IndicationBlocked = TRUE;
            }
            Socket->DeliverAgain = TRUE;
        }

        if (Socket->Chunk == NULL && Socket->RecvShutdown && !Socket->Closing)
        {
            /* End of stream: a graceful close completes receives with no
             * data, a broken connection with its error */
            while ((Request = PopRecvRequest(Socket)) != NULL)
            {
                KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
                CompleteRecvRequest(Request, Socket->RecvShutdownStatus, 0);
                KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
            }

            if (!Socket->DisconnectIndicated &&
                (Socket->CallbackMask & WSK_EVENT_DISCONNECT) &&
                Dispatch != NULL && Dispatch->WskDisconnectEvent != NULL)
            {
                Socket->DisconnectIndicated = TRUE;
                KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
                Dispatch->WskDisconnectEvent(Socket->user_context,
                                             NT_SUCCESS(Socket->RecvShutdownStatus) ? 0 : WSK_FLAG_ABORTIVE);
                KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
            }
        }

        if (Done != NULL)
        {
            KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
            FreeChunk(Done);
            KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
        }
    } while (Socket->DeliverAgain);

    Socket->Delivering = FALSE;
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    StreamKickReceive(Socket);
}

static NTSTATUS NTAPI
StreamReceiveComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    PNETIO_CHUNK Chunk = Context;
    PWSK_SOCKET_INTERNAL Socket = Chunk->Socket;
    NTSTATUS Status = Irp->IoStatus.Status;
    ULONG_PTR Received = Irp->IoStatus.Information;
    PMDL Mdl, NextMdl;
    KIRQL OldIrql;

    /* TdiReceive allocated the IRP and locked an MDL of its own */
    for (Mdl = Irp->MdlAddress; Mdl != NULL; Mdl = NextMdl)
    {
        NextMdl = Mdl->Next;
        MmUnlockPages(Mdl);
        IoFreeMdl(Mdl);
    }
    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);

    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    Socket->PumpActive = FALSE;
    if (Socket->Closing)
    {
        /* Nothing to report any more */
    }
    else if (NT_SUCCESS(Status) && Received > 0)
    {
        ASSERT(Socket->Chunk == NULL);
        Chunk->Offset = 0;
        Chunk->Length = (ULONG)min(Received, Chunk->Size);
        Socket->Chunk = Chunk;
        Chunk = NULL;
    }
    else
    {
        Socket->RecvShutdown = TRUE;
        Socket->RecvShutdownStatus = NT_SUCCESS(Status) ? STATUS_SUCCESS : Status;
    }
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    if (Chunk != NULL)
        FreeChunk(Chunk);

    StreamDeliver(Socket);
    DereferenceSocket(Socket);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID NTAPI
StreamReceiveWorker(_In_ PVOID Parameter)
{
    PWSK_SOCKET_INTERNAL Socket = Parameter;
    PFILE_OBJECT ConnectionFile = NULL;
    PNETIO_CHUNK Chunk;
    NTSTATUS Status;
    KIRQL OldIrql;
    PIRP Irp;

    Chunk = AllocateChunk(Socket);

    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    if (Chunk != NULL && !Socket->Closing && Socket->ConnectionFile != NULL)
    {
        ConnectionFile = Socket->ConnectionFile;
        ObReferenceObject(ConnectionFile);
    }
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    Status = STATUS_INVALID_DEVICE_STATE;
    if (ConnectionFile != NULL)
    {
        Irp = NULL;
        Status = TdiReceive(&Irp, ConnectionFile, TDI_RECEIVE_NORMAL, (PCHAR)Chunk->Data, Chunk->Size,
                            StreamReceiveComplete, Chunk);
        ObDereferenceObject(ConnectionFile);
        if (Irp != NULL)
        {
            /* StreamReceiveComplete takes it from here */
            return;
        }
    }

    /* Closing the socket meanwhile is not worth a message */
    if (ConnectionFile != NULL)
        DPRINT1("Could not post a stream receive, status 0x%08lx\n", Status);
    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    Socket->PumpActive = FALSE;
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
    if (Chunk != NULL)
        FreeChunk(Chunk);
    DereferenceSocket(Socket);
}

/* Called when the socket is closed, at PASSIVE_LEVEL: stops the event
 * callbacks and fails what is still waiting for data. */
static VOID
ReceiveShutdown(_In_ PWSK_SOCKET_INTERNAL Socket)
{
    LIST_ENTRY Backlog;
    PNETIO_RECV_REQUEST Request;
    PNETIO_DATAGRAM Datagram;
    KIRQL OldIrql;
    NTSTATUS Status;

    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    Socket->Closing = TRUE;
    Socket->CallbackMask = 0;
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    if (Socket->DatagramHandlerRegistered)
    {
        Status = TdiSetEventHandler(Socket->LocalAddressFile, TDI_EVENT_RECEIVE_DATAGRAM, NULL, NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not remove the receive datagram handler, status 0x%08lx\n", Status);
        }
        /* Wait for handlers still running */
        ExWaitForRundownProtectionRelease(&Socket->EventRundown);
        Socket->DatagramHandlerRegistered = FALSE;
    }

    InitializeListHead(&Backlog);
    KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    while (!IsListEmpty(&Socket->DatagramBacklog))
        InsertTailList(&Backlog, RemoveHeadList(&Socket->DatagramBacklog));
    Socket->BacklogCount = 0;
    Socket->BacklogBytes = 0;
    while ((Request = PopRecvRequest(Socket)) != NULL)
    {
        KeReleaseSpinLock(&Socket->RecvLock, OldIrql);
        CompleteRecvRequest(Request, STATUS_CANCELLED, 0);
        KeAcquireSpinLock(&Socket->RecvLock, &OldIrql);
    }
    KeReleaseSpinLock(&Socket->RecvLock, OldIrql);

    while (!IsListEmpty(&Backlog))
    {
        Datagram = CONTAINING_RECORD(RemoveHeadList(&Backlog), NETIO_DATAGRAM, Entry);
        FreeDatagram(Datagram);
    }
}

NTSTATUS NTAPI
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    FUNCTION_TRACE;

    return STATUS_SUCCESS;
}

static NTSTATUS WSKAPI
WskSocket(
    _In_ PWSK_CLIENT Client,
    _In_ ADDRESS_FAMILY AddressFamily,
    _In_ USHORT SocketType,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _In_opt_ PVOID SocketContext,
    _In_opt_ const VOID * Dispatch,
    _In_opt_ PEPROCESS OwningProcess,
    _In_opt_ PETHREAD OwningThread,
    _In_opt_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _Inout_ PIRP Irp);

static NTSTATUS NTAPI
NetioComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID ContextParam)
{
    PNETIO_CONTEXT Context = (PNETIO_CONTEXT)ContextParam;
    PIRP UserIrp = Context->UserIrp;

    FUNCTION_TRACE;

    UserIrp->IoStatus.Status = Irp->IoStatus.Status;
    UserIrp->IoStatus.Information = Irp->IoStatus.Information;

    if (Context->PeerAddrRet != NULL && NT_SUCCESS(Irp->IoStatus.Status))
    {
        PSOCKADDR RemoteAddress =
            (PSOCKADDR)(&((PTRANSPORT_ADDRESS)Context->PeerAddrRet->RemoteAddress)->Address[0].AddressType);

        memcpy(&Context->socket->RemoteAddress, RemoteAddress, sizeof(Context->socket->RemoteAddress));
    }
    /* Cancelled too (e.g. by closing the socket): the client waits for it */
    IoCompleteRequest(UserIrp, IO_NETWORK_INCREMENT);

    DereferenceSocket(Context->socket);
    if (Context->TargetConnectionInfo != NULL)
    {
        ExFreePoolWithTag(Context->TargetConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);
    }
    if (Context->PeerAddrRet != NULL)
    {
        ExFreePoolWithTag(Context->PeerAddrRet, TAG_AFD_TDI_CONNECTION_INFORMATION);
    }
    ExFreePoolWithTag(Context, TAG_NETIO);

    return STATUS_SUCCESS;
}

typedef struct _LISTEN_CONTEXT
{
    PWSK_SOCKET_INTERNAL ListenSocket;
    PWSK_SOCKET_INTERNAL AcceptSocket;
    PTDI_CONNECTION_INFORMATION RequestConnectionInfo, ReturnConnectionInfo;
} LISTEN_CONTEXT, *PLISTEN_CONTEXT;

static NTSTATUS NTAPI
CompletionFireEvent(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    PKEVENT Event = (PKEVENT)Context;

    FUNCTION_TRACE;

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS CreateSocket(
    _In_ ADDRESS_FAMILY AddressFamily,
    _In_ USHORT SocketType,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _Outptr_ PWSK_SOCKET_INTERNAL *TheSocket)
{
    PIRP NewSocketIrp;
    KEVENT CompletionEvent;
    NTSTATUS Status;

    FUNCTION_TRACE;

    NewSocketIrp = IoAllocateIrp(1, FALSE);
    if (NewSocketIrp == NULL)
    {
        DPRINT1("Out of memory?\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&CompletionEvent, NotificationEvent, FALSE);
    IoSetCompletionRoutine(NewSocketIrp, CompletionFireEvent, &CompletionEvent, TRUE, TRUE, TRUE);
    NewSocketIrp->Tail.Overlay.Thread = PsGetCurrentThread();

    Status = WskSocket(NULL, AddressFamily, SocketType, Protocol, Flags,
        NULL, NULL, NULL, NULL, NULL, NewSocketIrp);

    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
        Status = NewSocketIrp->IoStatus.Status;
    }

    if (NT_SUCCESS(Status))
    {
        *TheSocket = (PWSK_SOCKET_INTERNAL)NewSocketIrp->IoStatus.Information;
    }

    IoFreeIrp(NewSocketIrp);
    return Status;
}

static void QueueListening(_In_ PWSK_SOCKET_INTERNAL ListenSocket)
{
    FUNCTION_TRACE;

    KeSetEvent(&ListenSocket->StartListenEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS NTAPI
ListenComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    PLISTEN_CONTEXT ListenContext = (PLISTEN_CONTEXT)Context;
    PWSK_SOCKET_INTERNAL ListenSocket = ListenContext->ListenSocket;
    PWSK_SOCKET_INTERNAL AcceptSocket = ListenContext->AcceptSocket;
    const WSK_CLIENT_LISTEN_DISPATCH *ListenDispatch = ListenSocket->ClientDispatch;

    /* A PTRANSPORT_ADDRESS address field has an additional
     * AddressLength field so the struct sockaddr_in starts
     * at the AddressType (the address family, 2 for AF_INET)
     * field.
     */

    PSOCKADDR RemoteAddress =
        (PSOCKADDR)(&((PTRANSPORT_ADDRESS)ListenContext->ReturnConnectionInfo->RemoteAddress)->Address[0].AddressType);
    PVOID AcceptSocketContext;
    const WSK_CLIENT_CONNECTION_DISPATCH *AcceptSocketDispatch;
    NTSTATUS Status;

    FUNCTION_TRACE;

    if (ListenSocket->CallbackMask & WSK_EVENT_ACCEPT &&
        ListenDispatch->WskAcceptEvent != NULL &&
        ListenSocket->ListenIrp != NULL &&
        !ListenSocket->ListenCancelled &&
        Irp->IoStatus.Status != STATUS_CANCELLED)
    {
        ListenSocket->ListenIrp = NULL;

        AcceptSocketContext = NULL;
        AcceptSocketDispatch = NULL;
        memcpy(&AcceptSocket->RemoteAddress, RemoteAddress, sizeof(AcceptSocket->RemoteAddress));
        memcpy(&AcceptSocket->LocalAddress, &ListenSocket->LocalAddress, sizeof(AcceptSocket->LocalAddress));
        AcceptSocket->Connected = TRUE;

        Status = ListenDispatch->WskAcceptEvent(ListenSocket->user_context,
                                                0,
                                                &ListenSocket->LocalAddress,
                                                RemoteAddress,
                                                &AcceptSocket->Socket,
                                                &AcceptSocketContext,
                                                &AcceptSocketDispatch);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ListenDispatch->WskAcceptEvent returned non-successful status 0x%08x\n", Status);
        }
        else
        {
            /* The client's context and callbacks for the new connection */
            AcceptSocket->user_context = AcceptSocketContext;
            AcceptSocket->ClientDispatch = AcceptSocketDispatch;
        }
    }
    else
    {
        DereferenceSocket(AcceptSocket);	/* close the AcceptSocket */
    }
    ListenSocket->ListenIrp = NULL;

    DereferenceSocket(AcceptSocket);
    DereferenceSocket(ListenSocket);

    ExFreePoolWithTag(ListenContext->ReturnConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);
    ExFreePoolWithTag(ListenContext->RequestConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);
    ExFreePoolWithTag(ListenContext, TAG_NETIO);

    /* And wait for the next incoming connection.
     * This is done in a separate thread at IRQL = PASSIVE_LEVEL
     */

    QueueListening(ListenSocket);

    /* TdiListen now allocates this IRP with IoAllocateIrp (so a pending listen
     * is not cancelled when the requesting thread/process exits), which means
     * it is not freed by a thread completion APC.  Free it here and stop the
     * I/O manager from processing it any further. */
    IoFreeIrp(Irp);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
StartListening(_In_ PWSK_SOCKET_INTERNAL ListenSocket)
{
    PIRP tdiIrp;
    NTSTATUS status;
    PLISTEN_CONTEXT lc;
    PWSK_SOCKET_INTERNAL AcceptSocket;

    FUNCTION_TRACE;

    if (ListenSocket->LocalAddressHandle == NULL)
    {
        DPRINT1("LocalAddressHandle is not set, need to bind your socket before listening\n");
        return STATUS_INVALID_PARAMETER;
    }

    status = CreateSocket(ListenSocket->family, ListenSocket->type, ListenSocket->proto, WSK_FLAG_CONNECTION_SOCKET, &AcceptSocket);
    if (!NT_SUCCESS(status))
    {
        DPRINT1("Could not create AcceptSocket, status is 0x%08x\n", status);
        return status;
    }
    AcceptSocket->ListenSocket = ListenSocket;
    /* Dereferenced when the AcceptSocket is closed */
    ReferenceSocket(AcceptSocket->ListenSocket);

    status = STATUS_INSUFFICIENT_RESOURCES;
    lc = ExAllocatePoolUninitialized(NonPagedPool, sizeof(*lc), TAG_NETIO);
    if (lc == NULL)
    {
        goto err_out_free_accept_socket;
    }
    lc->ListenSocket = ListenSocket;
    lc->AcceptSocket = AcceptSocket;

    tdiIrp = NULL;

    status = TdiAssociateAddressFile(ListenSocket->LocalAddressHandle, AcceptSocket->ConnectionFile);
    if (!NT_SUCCESS(status))
    {
        goto err_out_free_lc;
    }
    AcceptSocket->ConnectionFileAssociated = TRUE;

    lc->RequestConnectionInfo = NULL;
    TdiBuildNullConnectionInfo(&lc->RequestConnectionInfo, TDI_ADDRESS_TYPE_IP);
    if (lc->RequestConnectionInfo == NULL)
    {
        goto err_out_free_lc_and_disassociate;
    }
    lc->RequestConnectionInfo->OptionsLength = 0;

    lc->ReturnConnectionInfo = NULL;
    TdiBuildNullConnectionInfo(&lc->ReturnConnectionInfo, TDI_ADDRESS_TYPE_IP);
    if (lc->ReturnConnectionInfo == NULL)
    {
        goto err_out_free_lc_and_req_conn_info;
    }
    lc->ReturnConnectionInfo->OptionsLength = 0;

    ReferenceSocket(ListenSocket);
    ReferenceSocket(AcceptSocket);

    ListenSocket->ListenCancelled = FALSE;
    /* pass the ConnectionFile from accept socket, not from ListenSocket ... */
    status = TdiListen(&tdiIrp, AcceptSocket->ConnectionFile, &lc->RequestConnectionInfo, &lc->ReturnConnectionInfo, ListenComplete, lc);

    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(lc->ReturnConnectionInfo, TAG_NETIO);
        DereferenceSocket(ListenSocket);
        DereferenceSocket(AcceptSocket);
        goto err_out_free_lc_and_req_conn_info;
    }
    ListenSocket->ListenIrp = tdiIrp;

    return STATUS_PENDING;

err_out_free_lc_and_req_conn_info:
    ExFreePoolWithTag(lc->RequestConnectionInfo, TAG_NETIO);

err_out_free_lc_and_disassociate:
    status = TdiDisassociateAddressFile(AcceptSocket->ConnectionFile);
    if (!NT_SUCCESS(status))
    {
        DPRINT1("Warning: TdiDisassociateAddressFile returned status %08x\n", status);
    }
    AcceptSocket->ConnectionFileAssociated = FALSE;

err_out_free_lc:
    ExFreePoolWithTag(lc, TAG_NETIO);

err_out_free_accept_socket:
    DereferenceSocket(AcceptSocket);

    return status;
}

static VOID NTAPI RequeueListenThread(_In_ PVOID p)
{
    PWSK_SOCKET_INTERNAL ListenSocket = (PWSK_SOCKET_INTERNAL)p;
    NTSTATUS status;

    FUNCTION_TRACE;

    while (ListenSocket->ListenThreadShouldRun)
    {
        status = KeWaitForSingleObject(&ListenSocket->StartListenEvent, Executive, KernelMode, FALSE, NULL);
        if (!NT_SUCCESS(status))
        {
            DPRINT1("KeWaitForSingleObject failed with status 0x%08x!\n", status);
        }
        if (!ListenSocket->ListenThreadShouldRun)
        {
            break;
        }
        StartListening(ListenSocket);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS WSKAPI
WskControlSocket(
    _In_ PWSK_SOCKET SocketParam,
    _In_ WSK_CONTROL_SOCKET_TYPE RequestType,
    _In_ ULONG ControlCode,
    _In_ ULONG Level,
    _In_ SIZE_T InputSize,
    _In_reads_bytes_opt_(InputSize) PVOID InputBuffer,
    _In_ SIZE_T OutputSize,
    _Out_writes_bytes_opt_(OutputSize) PVOID OutputBuffer,
    _Out_opt_ SIZE_T * OutputSizeReturned,
    _Inout_opt_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;

    FUNCTION_TRACE;

    /* The IRP is optional: SO_WSK_EVENT_CALLBACK, for one, is set without one */
    if (Irp != NULL)
        IoSetNextIrpStackLocation(Irp);

    if (Socket == NULL)
    {
        status = STATUS_INVALID_PARAMETER;
        goto err_out;
    }

    switch (RequestType)
    {
        case WskSetOption:
            switch (Level)
            {
                case SOL_SOCKET:
                    switch (ControlCode)
                    {
                        case SO_REUSEADDR:     /* add more supported flags here */
                            if (InputBuffer == NULL)
                            {
                                DPRINT1("WskControlSocket: Need an InputBuffer for this operation\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                            }
                            if (InputSize < sizeof(UINT))
                            {
                                DPRINT1("WskControlSocket: InputBuffer too small for this operation\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                            }
                            UINT enable = *(PUINT)InputBuffer;
                            if (enable != 0)
                            {
                                Socket->Flags |= ControlCode;
                            }
                            else
                            {
                                Socket->Flags &= ~ControlCode;
                            }
                            status = STATUS_SUCCESS;
                            break;

                            /* Windows specific. This sets the mask for callback functions. */
                        case SO_WSK_EVENT_CALLBACK:
                            if (InputBuffer == NULL)
                            {
                                DPRINT1("WskControlSocket: Need an InputBuffer for this operation\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                            }
                            if (InputSize < sizeof(WSK_EVENT_CALLBACK_CONTROL))
                            {
                                DPRINT1("WskControlSocket: InputBuffer too small for this operation\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                            }
                            PWSK_EVENT_CALLBACK_CONTROL Control = (WSK_EVENT_CALLBACK_CONTROL *)InputBuffer;
                            ULONG Events = Control->EventMask & ~WSK_EVENT_DISABLE;
                            ULONG OldMask = Socket->CallbackMask;

                            if (Control->EventMask & WSK_EVENT_DISABLE)
                            {
                                /* Disables the given events, the others stay */
                                InterlockedAnd((volatile LONG *)&Socket->CallbackMask, ~(LONG)Events);
                                status = STATUS_SUCCESS;
                                break;
                            }

                            /* Enables the given events, the others stay */
                            InterlockedOr((volatile LONG *)&Socket->CallbackMask, (LONG)Events);

                            if (((OldMask & WSK_EVENT_ACCEPT) == 0) &&
                                ((Events & WSK_EVENT_ACCEPT) == WSK_EVENT_ACCEPT))
                            {
                                QueueListening(Socket);
                            }
                            if (Socket->type == SOCK_DGRAM && (Events & WSK_EVENT_RECEIVE_FROM))
                            {
                                /* Datagrams that arrived before */
                                IndicateDatagramBacklog(Socket);
                            }
                            if (Socket->type == SOCK_STREAM &&
                                (Events & (WSK_EVENT_RECEIVE | WSK_EVENT_DISCONNECT)))
                            {
                                /* Indicate what arrived already, start receiving */
                                StreamDeliver(Socket);
                            }

                            status = STATUS_SUCCESS;
                            break;

                        default:
                            DPRINT1("WskControlSocket: ControlCode %d Not implemented\n", ControlCode);
                    }
                    break;
                default:
                    DPRINT1("WskControlSocket: Level %d Not implemented\n", Level);
            }
            break;

        case WskIoctl:
            switch (ControlCode)
            {
                /* Windows specific. Gives a datagram socket a fixed
                 * destination, used when WskSendTo has no RemoteAddress. */
                case SIO_WSK_SET_REMOTE_ADDRESS:
                    if (Socket->type != SOCK_DGRAM)
                    {
                        status = STATUS_INVALID_PARAMETER;
                        break;
                    }
                    if (InputBuffer == NULL || InputSize == 0)
                    {
                        /* Remove the fixed remote address */
                        Socket->FixedRemoteAddress = FALSE;
                        RtlZeroMemory(&Socket->RemoteAddress, sizeof(Socket->RemoteAddress));
                        status = STATUS_SUCCESS;
                        break;
                    }
                    if (InputSize < sizeof(Socket->RemoteAddress) ||
                        ((PSOCKADDR)InputBuffer)->sa_family != Socket->family)
                    {
                        status = STATUS_INVALID_PARAMETER;
                        break;
                    }
                    RtlCopyMemory(&Socket->RemoteAddress, InputBuffer, sizeof(Socket->RemoteAddress));
                    Socket->FixedRemoteAddress = TRUE;
                    status = STATUS_SUCCESS;
                    break;

                default:
                    DPRINT1("WskControlSocket: Ioctl 0x%lx Not implemented\n", ControlCode);
            }
            break;

        case WskGetOption:
        default:
            DPRINT1("WskControlSocket: Option %d Not implemented\n", RequestType);
    }

err_out:
    if (Irp == NULL)
        return status;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static NTSTATUS WSKAPI
WskCloseSocket(_In_ PWSK_SOCKET SocketParam, _Inout_ PIRP Irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);

    IoSetNextIrpStackLocation(Irp);

    /* There might be a reference from (for example) a pending
     * receive. Shutdown the socket here explicitly. We expect
     * all pending I/O operations to be cancelled, then.
     */

    SocketShutdown(Socket);
    DereferenceSocket(Socket);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static PTRANSPORT_ADDRESS
TdiTransportAddressFromSocketAddress(_In_ PSOCKADDR SocketAddress)
{
    PTRANSPORT_ADDRESS ta;
    ULONG Size;

    Size = FIELD_OFFSET(TRANSPORT_ADDRESS, Address) +
           FIELD_OFFSET(TA_ADDRESS, Address[sizeof(SocketAddress->sa_data)]);

    ta = ExAllocatePoolUninitialized(NonPagedPool, Size, TAG_NETIO);
    if (ta == NULL)
    {
        DPRINT1("TdiTransportAddressFromSocketAddress: Out of memory\n");
        return NULL;
    }

    ta->TAAddressCount = 1;
    ta->Address[0].AddressLength = sizeof(SocketAddress->sa_data);
    ta->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;	/* AF_INET */

    memcpy(&ta->Address[0].Address[0], &SocketAddress->sa_data, ta->Address[0].AddressLength);

    return ta;
}

static PTDI_CONNECTION_INFORMATION
TdiConnectionInfoFromSocketAddress(_In_ PSOCKADDR SocketAddress)
{
    PTRANSPORT_ADDRESS TargetAddress;
    PTDI_CONNECTION_INFORMATION ConnectionInformation = NULL;
    NTSTATUS status;

    TargetAddress = TdiTransportAddressFromSocketAddress(SocketAddress);
    if (TargetAddress == NULL)
        return NULL;

    status = TdiBuildConnectionInfo(&ConnectionInformation, TargetAddress);
    ExFreePoolWithTag(TargetAddress, TAG_NETIO);

    if (!NT_SUCCESS(status))
    {
        if (ConnectionInformation != NULL)
        {
            ExFreePoolWithTag(ConnectionInformation, TAG_AFD_TDI_CONNECTION_INFORMATION);
        }
        return NULL;
    }
    ConnectionInformation->OptionsLength = 0;
    return ConnectionInformation;
}

static NTSTATUS WSKAPI
WskBind(_In_ PWSK_SOCKET SocketParam, _In_ PSOCKADDR LocalAddress, _Reserved_ ULONG Flags, _Inout_ PIRP Irp)
{
    NTSTATUS status;
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);

    PTRANSPORT_ADDRESS ta = TdiTransportAddressFromSocketAddress(LocalAddress);

    FUNCTION_TRACE;
    IoSetNextIrpStackLocation(Irp);

    if (ta == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }
    if (Socket->LocalAddressHandle != NULL)
    {
        status = STATUS_INVALID_PARAMETER;
        goto err_out;
    }

    status = TdiOpenAddressFile(&Socket->TdiName,
                                ta, AFD_SHARE_REUSE, &Socket->LocalAddressHandle, &Socket->LocalAddressFile);

    ExFreePoolWithTag(ta, TAG_NETIO);

    if (NT_SUCCESS(status) && Socket->type == SOCK_DGRAM)
    {
        /* Inbound datagrams are indicated to us from now on */
        status = TdiSetEventHandler(Socket->LocalAddressFile, TDI_EVENT_RECEIVE_DATAGRAM,
                                    (PVOID)ReceiveDatagramEventHandler, Socket);
        if (NT_SUCCESS(status))
        {
            Socket->DatagramHandlerRegistered = TRUE;
        }
        else
        {
            DPRINT1("Could not set the receive datagram handler, status 0x%08lx\n", status);
            ObDereferenceObject(Socket->LocalAddressFile);
            Socket->LocalAddressFile = NULL;
            ZwClose(Socket->LocalAddressHandle);
            Socket->LocalAddressHandle = NULL;
        }
    }

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static NTSTATUS WSKAPI
WskSendTo(
    _In_ PWSK_SOCKET SocketParam,
    _In_ PWSK_BUF Buffer,
    _Reserved_ ULONG Flags,
    _In_opt_ PSOCKADDR RemoteAddress,
    _In_ ULONG ControlInfoLength,
    _In_reads_bytes_opt_(ControlInfoLength) PCMSGHDR ControlInfo,
    _Inout_ PIRP Irp)
{
    PIRP tdiIrp = NULL;
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    PTDI_CONNECTION_INFORMATION TargetConnectionInfo;
    NTSTATUS status;
    void *BufferData;
    PNETIO_CONTEXT NetioContext;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);

    status = STATUS_INSUFFICIENT_RESOURCES;
    NetioContext = ExAllocatePoolUninitialized(NonPagedPool, sizeof(*NetioContext), TAG_NETIO);
    if (NetioContext == NULL)
    {
        goto err_out;
    }
    NetioContext->socket = Socket;
    NetioContext->UserIrp = Irp;

    if (RemoteAddress == NULL)
    {
        /* Use the address set with SIO_WSK_SET_REMOTE_ADDRESS */
        if (!Socket->FixedRemoteAddress)
        {
            status = STATUS_INVALID_PARAMETER;
            goto err_out_free_nc;
        }
        RemoteAddress = &Socket->RemoteAddress;
    }

    TargetConnectionInfo = TdiConnectionInfoFromSocketAddress(RemoteAddress);
    if (TargetConnectionInfo == NULL)
    {
        goto err_out_free_nc;
    }
    NetioContext->TargetConnectionInfo = TargetConnectionInfo;
    NetioContext->PeerAddrRet = NULL;

    BufferData = MmGetSystemAddressForMdlSafe(Buffer->Mdl, NormalPagePriority);
    if (BufferData == NULL)
    {
        DPRINT1("Error mapping MDL\n");
        goto err_out_free_nc_and_tci;
    }

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);

    /* This will create a tdiIrp */
    status = TdiSendDatagram(&tdiIrp, Socket->LocalAddressFile, ((char *)BufferData) + Buffer->Offset,
                        Buffer->Length, TargetConnectionInfo, NetioComplete, NetioContext);

    /* If allocating tdiIrp fails we get here.
     * Call the IoCompletion of the application's Irp so this Irp
     * gets freed.
     */
    if (!NT_SUCCESS(status))
    {
        DereferenceSocket(Socket);
        goto err_out_free_nc_and_tci;	/* caller has to clean up mdl mapping */
    }
    return STATUS_PENDING;

err_out_free_nc_and_tci:
    ExFreePoolWithTag(TargetConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);

err_out_free_nc:
    ExFreePoolWithTag(NetioContext, TAG_NETIO);

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return status;
}

static NTSTATUS WSKAPI
WskReceiveFrom(
    _In_ PWSK_SOCKET SocketParam,
    _In_ PWSK_BUF Buffer,
    _Reserved_ ULONG Flags,
    _Out_opt_ PSOCKADDR RemoteAddress,
    _Inout_ PULONG ControlLength,
    _Out_writes_bytes_opt_(*ControlLength) PCMSGHDR ControlInfo,
    _Out_opt_ PULONG ControlFlags, _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    PNETIO_RECV_REQUEST Request;
    NTSTATUS Status;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);

    if (Buffer == NULL || Buffer->Mdl == NULL)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto err_out;
    }
    if (!Socket->DatagramHandlerRegistered)
    {
        /* Not bound */
        Status = STATUS_INVALID_DEVICE_STATE;
        goto err_out;
    }

    Request = CreateRecvRequest(Socket, Buffer, Irp);
    if (Request == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }
    Request->RemoteAddress = RemoteAddress;
    Request->ControlLength = ControlLength;
    Request->ControlFlags = ControlFlags;

    /* Completed by the next datagram, or one waiting already */
    QueueRecvRequest(Request);
    DrainDatagramBacklog(Socket);
    return STATUS_PENDING;

err_out:
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    return STATUS_PENDING;
}

/* Gives back datagrams the client retained by returning STATUS_PENDING
 * from its WskReceiveFromEvent */
static NTSTATUS WSKAPI
WskReleaseUdp(_In_ PWSK_SOCKET SocketParam, _In_ PWSK_DATAGRAM_INDICATION DatagramIndication)
{
    PWSK_DATAGRAM_INDICATION Next;

    FUNCTION_TRACE;

    while (DatagramIndication != NULL)
    {
        Next = DatagramIndication->Next;
        FreeDatagram(CONTAINING_RECORD(DatagramIndication, NETIO_DATAGRAM, Indication));
        DatagramIndication = Next;
    }
    return STATUS_SUCCESS;
}

/* Gives back data the client retained by returning STATUS_PENDING from
 * its WskReceiveEvent */
static NTSTATUS WSKAPI
WskReleaseTcp(_In_ PWSK_SOCKET SocketParam, _In_ PWSK_DATA_INDICATION DataIndication)
{
    PWSK_DATA_INDICATION Next;

    FUNCTION_TRACE;

    while (DataIndication != NULL)
    {
        Next = DataIndication->Next;
        FreeChunk(CONTAINING_RECORD(DataIndication, NETIO_CHUNK, Indication));
        DataIndication = Next;
    }
    return STATUS_SUCCESS;
}

/* Asks the transport which address, including the port it picked for a
 * wildcard bind, an address file is bound to. Must run at PASSIVE_LEVEL. */
static NTSTATUS
QueryBoundAddress(_In_ PFILE_OBJECT AddressFile, _Out_ PSOCKADDR LocalAddress)
{
    const ULONG Size = FIELD_OFFSET(TDI_ADDRESS_INFO, Address) + sizeof(TA_IP_ADDRESS) + 64;
    PTDI_ADDRESS_INFO Info;
    PTA_ADDRESS Address;
    NTSTATUS Status;
    PMDL Mdl;

    Info = ExAllocatePoolZero(NonPagedPool, Size, TAG_NETIO);
    if (Info == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Mdl = IoAllocateMdl(Info, Size, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        ExFreePoolWithTag(Info, TAG_NETIO);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    _SEH2_TRY
    {
        MmProbeAndLockPages(Mdl, KernelMode, IoModifyAccess);
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        IoFreeMdl(Mdl);
        ExFreePoolWithTag(Info, TAG_NETIO);
        return Status;
    }

    /* Consumes the MDL */
    Status = TdiQueryInformation(AddressFile, TDI_QUERY_ADDRESS_INFO, Mdl);
    if (NT_SUCCESS(Status))
    {
        Address = &Info->Address.Address[0];
        if (Info->Address.TAAddressCount < 1 ||
            Address->AddressLength > sizeof(LocalAddress->sa_data))
        {
            Status = STATUS_INVALID_NETWORK_RESPONSE;
        }
        else
        {
            RtlZeroMemory(LocalAddress, sizeof(*LocalAddress));
            LocalAddress->sa_family = Address->AddressType;
            RtlCopyMemory(LocalAddress->sa_data, Address->Address, Address->AddressLength);
        }
    }

    ExFreePoolWithTag(Info, TAG_NETIO);
    return Status;
}

static NTSTATUS WSKAPI
WskGetLocalAddress(_In_ PWSK_SOCKET SocketParam, _Out_ PSOCKADDR LocalAddress, _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS Status;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);

    if (Socket->LocalAddressFile != NULL)
    {
        Status = QueryBoundAddress(Socket->LocalAddressFile, LocalAddress);
    }
    else if (Socket->LocalAddress.sa_family != 0)
    {
        /* Accepted sockets share the address of their listen socket */
        RtlCopyMemory(LocalAddress, &Socket->LocalAddress, sizeof(*LocalAddress));
        Status = STATUS_SUCCESS;
    }
    else
    {
        /* Not bound */
        Status = STATUS_INVALID_DEVICE_STATE;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

/* WskSendMessages: one TDI send per buffer, the client IRP completes after
 * the last one with the total byte count, or the first error. */
typedef struct _NETIO_SEND_MESSAGES_CONTEXT
{
    PIRP UserIrp;
    PWSK_SOCKET_INTERNAL Socket;
    PTDI_CONNECTION_INFORMATION TargetConnectionInfo;
    volatile LONG Pending;
    volatile LONG Status;
    volatile LONG64 BytesSent;
} NETIO_SEND_MESSAGES_CONTEXT, *PNETIO_SEND_MESSAGES_CONTEXT;

static VOID
SendMessagesFail(_Inout_ PNETIO_SEND_MESSAGES_CONTEXT Context, _In_ NTSTATUS Status)
{
    /* Keep the first error */
    InterlockedCompareExchange(&Context->Status, Status, STATUS_SUCCESS);
}

static VOID
SendMessagesDereference(_Inout_ PNETIO_SEND_MESSAGES_CONTEXT Context)
{
    PIRP UserIrp = Context->UserIrp;

    if (InterlockedDecrement(&Context->Pending) != 0)
        return;

    UserIrp->IoStatus.Status = Context->Status;
    UserIrp->IoStatus.Information = NT_SUCCESS(Context->Status) ? (ULONG_PTR)Context->BytesSent : 0;
    IoCompleteRequest(UserIrp, IO_NETWORK_INCREMENT);

    DereferenceSocket(Context->Socket);
    ExFreePoolWithTag(Context->TargetConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);
    ExFreePoolWithTag(Context, TAG_NETIO);
}

static NTSTATUS NTAPI
SendMessagesComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID ContextParam)
{
    PNETIO_SEND_MESSAGES_CONTEXT Context = ContextParam;

    if (NT_SUCCESS(Irp->IoStatus.Status))
        InterlockedExchangeAdd64(&Context->BytesSent, (LONG64)Irp->IoStatus.Information);
    else
        SendMessagesFail(Context, Irp->IoStatus.Status);

    SendMessagesDereference(Context);

    /* Let the I/O manager free the TDI IRP and its MDL */
    return STATUS_SUCCESS;
}

static NTSTATUS WSKAPI
WskSendMessages(
    _In_ PWSK_SOCKET SocketParam,
    _In_ PWSK_BUF_LIST BufferList,
    _Reserved_ ULONG Flags,
    _In_opt_ PSOCKADDR RemoteAddress,
    _In_ ULONG ControlInfoLength,
    _In_reads_bytes_opt_(ControlInfoLength) PVOID ControlInfo,
    _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    PNETIO_SEND_MESSAGES_CONTEXT Context;
    PWSK_BUF_LIST Entry;
    PIRP TdiIrp;
    PCHAR Data;
    NTSTATUS Status;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);

    if (RemoteAddress == NULL && Socket->FixedRemoteAddress)
        RemoteAddress = &Socket->RemoteAddress;

    if (BufferList == NULL || RemoteAddress == NULL)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto err_out;
    }

    Context = ExAllocatePoolZero(NonPagedPool, sizeof(*Context), TAG_NETIO);
    if (Context == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }

    Context->TargetConnectionInfo = TdiConnectionInfoFromSocketAddress(RemoteAddress);
    if (Context->TargetConnectionInfo == NULL)
    {
        ExFreePoolWithTag(Context, TAG_NETIO);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }

    Context->UserIrp = Irp;
    Context->Socket = Socket;
    Context->Status = STATUS_SUCCESS;
    /* Held by this function until every send has been issued */
    Context->Pending = 1;

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);

    for (Entry = BufferList; Entry != NULL; Entry = Entry->Next)
    {
        if (Entry->Buffer.Length == 0)
            continue;

        Data = MmGetSystemAddressForMdlSafe(Entry->Buffer.Mdl, NormalPagePriority);
        if (Data == NULL)
        {
            SendMessagesFail(Context, STATUS_INSUFFICIENT_RESOURCES);
            break;
        }

        InterlockedIncrement(&Context->Pending);
        TdiIrp = NULL;
        Status = TdiSendDatagram(&TdiIrp, Socket->LocalAddressFile, Data + Entry->Buffer.Offset,
                                 (UINT)Entry->Buffer.Length, Context->TargetConnectionInfo,
                                 SendMessagesComplete, Context);
        if (TdiIrp == NULL)
        {
            /* The IRP was never sent, so the completion routine won't run */
            SendMessagesFail(Context, NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status);
            SendMessagesDereference(Context);
            break;
        }
    }

    SendMessagesDereference(Context);
    return STATUS_PENDING;

err_out:
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static NTSTATUS WSKAPI
WskGetRemoteAddress(_In_ PWSK_SOCKET SocketParam, _Out_ PSOCKADDR RemoteAddress, _Inout_ PIRP Irp)
{
    FUNCTION_TRACE;

    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS Status = STATUS_INVALID_PARAMETER;

    IoSetNextIrpStackLocation(Irp);
    if (Socket != NULL)
    {
        memcpy(RemoteAddress, &Socket->RemoteAddress, sizeof(*RemoteAddress));
        Status = STATUS_SUCCESS;
    }
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;  /* Caller has to wait for his completion routine */
}

static NTSTATUS WSKAPI
WskSocketConnect(
    _In_ PWSK_CLIENT Client,
    _In_ USHORT SocketType,
    _In_ ULONG Protocol,
    _In_ PSOCKADDR LocalAddress,
    _In_ PSOCKADDR RemoteAddress,
    _In_ ULONG Flags,
    _In_opt_ PVOID SocketContext,
    _In_opt_ const WSK_CLIENT_CONNECTION_DISPATCH * Dispatch,
    _In_opt_ PEPROCESS OwningProcess,
    _In_opt_ PETHREAD OwningThread,
    _In_opt_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _Inout_ PIRP Irp)
{
    FUNCTION_TRACE;

    UNIMPLEMENTED;
    if (Irp != NULL)
    {
        Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
        IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    }
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WSKAPI
WskControlClient(
    _In_ PWSK_CLIENT Client,
    _In_ ULONG ControlCode,
    _In_ SIZE_T InputSize,
    _In_reads_bytes_opt_(InputSize) PVOID InputBuffer,
    _In_ SIZE_T OutputSize,
    _Out_writes_bytes_opt_(OutputSize) PVOID OutputBuffer,
    _Out_opt_ SIZE_T * OutputSizeReturned, _Inout_opt_ PIRP Irp)
{
    FUNCTION_TRACE;

    UNIMPLEMENTED;
    if (Irp != NULL)
    {
        Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
        IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    }
    return STATUS_NOT_IMPLEMENTED;
}

/* NETIO.SYS builds for NT 6.1, whose datagram dispatch table ends with
 * WskGetLocalAddress. Clients built for Windows 8 and later read
 * WskSendMessages right after it, so append it. */
typedef struct _NETIO_DATAGRAM_DISPATCH
{
    WSK_PROVIDER_DATAGRAM_DISPATCH Nt61;
    PFN_WSK_SEND_MESSAGES WskSendMessages;
} NETIO_DATAGRAM_DISPATCH;

static NETIO_DATAGRAM_DISPATCH UdpDispatch = {
    .Nt61 = {
        .WskControlSocket = WskControlSocket,
        .WskCloseSocket = WskCloseSocket,
        .WskBind = WskBind,
        .WskSendTo = WskSendTo,
        .WskReceiveFrom = WskReceiveFrom,
        .WskRelease = WskReleaseUdp,
        .WskGetLocalAddress = WskGetLocalAddress,
    },
    .WskSendMessages = WskSendMessages,
};

/* Connection oriented routines (TCP/IP) */

static NTSTATUS NTAPI
ConnectComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID ContextParam)
{
    PNETIO_CONTEXT Context = ContextParam;
    PWSK_SOCKET_INTERNAL Socket = Context->socket;

    /* Before the client's completion routine runs: it may enable the
     * receive events right away. */
    if (NT_SUCCESS(Irp->IoStatus.Status))
        Socket->Connected = TRUE;

    ReferenceSocket(Socket);
    NetioComplete(DeviceObject, Irp, ContextParam);
    StreamKickReceive(Socket);
    DereferenceSocket(Socket);

    return STATUS_SUCCESS;
}

static NTSTATUS WSKAPI
WskConnect(_In_ PWSK_SOCKET SocketParam, _In_ PSOCKADDR RemoteAddress, _Reserved_ ULONG Flags, _Inout_ PIRP Irp)
{
    PTDI_CONNECTION_INFORMATION TargetConnectionInfo, PeerAddrRet;
    PIRP tdiIrp;
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS status, status2;
    PNETIO_CONTEXT NetioContext;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);

    status = STATUS_INVALID_PARAMETER;
    if (Socket->LocalAddressHandle == NULL)
    {
        DPRINT1("LocalAddressHandle is not set, need to bind your socket before connecting\n");
        goto err_out;
    }

    status = STATUS_INSUFFICIENT_RESOURCES;
    NetioContext = ExAllocatePoolUninitialized(NonPagedPool, sizeof(*NetioContext), TAG_NETIO);
    if (NetioContext == NULL)
    {
        goto err_out;
    }
    NetioContext->socket = Socket;
    NetioContext->UserIrp = Irp;

    tdiIrp = NULL;

    status = TdiAssociateAddressFile(Socket->LocalAddressHandle, Socket->ConnectionFile);
    if (!NT_SUCCESS(status))
    {
        goto err_out_free_nc;
    }
    Socket->ConnectionFileAssociated = TRUE;

    TargetConnectionInfo = TdiConnectionInfoFromSocketAddress(RemoteAddress);
    if (TargetConnectionInfo == NULL)
    {
        goto err_out_free_nc_and_disassociate;
    }
    NetioContext->TargetConnectionInfo = TargetConnectionInfo;

    PeerAddrRet = NULL;
    TdiBuildNullConnectionInfo(&PeerAddrRet, TDI_ADDRESS_TYPE_IP);
    if (PeerAddrRet == NULL)
    {
        goto err_out_free_nc_and_tci;
    }
    PeerAddrRet->OptionsLength = 0;
    NetioContext->PeerAddrRet = PeerAddrRet;

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);

    status = TdiConnect(&tdiIrp, Socket->ConnectionFile, TargetConnectionInfo, PeerAddrRet, ConnectComplete, NetioContext);

    if (!NT_SUCCESS(status))
    {
        /* If allocating tdiIrp fails we get here.
         * Call the IoCompletion of the application's Irp so this Irp
         * gets freed.
         */
        ExFreePoolWithTag(PeerAddrRet, TAG_NETIO);
        DereferenceSocket(Socket);
        goto err_out_free_nc_and_tci;
    }
    return STATUS_PENDING;

err_out_free_nc_and_tci:
    ExFreePoolWithTag(TargetConnectionInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);

err_out_free_nc_and_disassociate:
    status2 = TdiDisassociateAddressFile(Socket->ConnectionFile);
    if (!NT_SUCCESS(status2))
    {
        DPRINT1("Warning: TdiDisassociateAddressFile returned status %08x\n", status);
    }
    Socket->ConnectionFileAssociated = FALSE;

err_out_free_nc:
    ExFreePoolWithTag(NetioContext, TAG_NETIO);

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return status;
}

static NTSTATUS WSKAPI
WskSend(_In_ PWSK_SOCKET SocketParam, _In_ PWSK_BUF Buffer, _In_ ULONG Flags, _Inout_ PIRP Irp)
{
    PIRP tdiIrp = NULL;
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    NTSTATUS status;
    void *BufferData;
    PNETIO_CONTEXT NetioContext;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);
    status = STATUS_INSUFFICIENT_RESOURCES;

    NetioContext = ExAllocatePoolUninitialized(NonPagedPool, sizeof(*NetioContext), TAG_NETIO);
    if (NetioContext == NULL)
    {
        goto err_out;
    }
    NetioContext->socket = Socket;
    NetioContext->UserIrp = Irp;

    NetioContext->TargetConnectionInfo = NULL;
    NetioContext->PeerAddrRet = NULL;

    BufferData = MmGetSystemAddressForMdlSafe(Buffer->Mdl, NormalPagePriority);
    if (BufferData == NULL)
    {
        DPRINT1("Error mapping MDL\n");
        goto err_out_free_nc;
    }

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);

    /* This will create a tdiIrp */
    status = TdiSend(&tdiIrp, Socket->ConnectionFile, 0, ((char *)BufferData) + Buffer->Offset,
                     Buffer->Length, NetioComplete, NetioContext);

    /* If allocating tdiIrp fails we get here.
     * Call the IoCompletion of the application's Irp so this Irp
     * gets freed.
     */
    if (!NT_SUCCESS(status))
    {
        DereferenceSocket(Socket);
        goto err_out_free_nc;	/* caller has to clean up mdl mapping */
    }
    return STATUS_PENDING;

err_out_free_nc:
    ExFreePoolWithTag(NetioContext, TAG_NETIO);

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return status;
}

/* Completes once data arrived (any amount up to the buffer size), with no
 * data once the peer closed its side, or with the error that broke the
 * connection. */
static NTSTATUS WSKAPI
WskReceive(_In_ PWSK_SOCKET SocketParam, _In_ PWSK_BUF Buffer, _In_ ULONG Flags, _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    PNETIO_RECV_REQUEST Request;
    NTSTATUS Status;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);

    if (Buffer == NULL || Buffer->Mdl == NULL || Buffer->Length == 0)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto err_out;
    }
    if (!Socket->Connected)
    {
        Status = STATUS_INVALID_DEVICE_STATE;
        goto err_out;
    }

    Request = CreateRecvRequest(Socket, Buffer, Irp);
    if (Request == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }
    QueueRecvRequest(Request);

    /* Serve it from data received already, else start receiving */
    StreamDeliver(Socket);
    return STATUS_PENDING;

err_out:
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    return STATUS_PENDING;
}

/* Graceful: shuts down the sending side, the socket can still receive.
 * WSK_FLAG_ABORTIVE resets the connection instead. */
static NTSTATUS WSKAPI
WskDisconnect(_In_ PWSK_SOCKET SocketParam, _In_opt_ PWSK_BUF Buffer, _In_ ULONG Flags, _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket = CONTAINING_RECORD(SocketParam, WSK_SOCKET_INTERNAL, Socket);
    PNETIO_CONTEXT NetioContext;
    PIRP tdiIrp = NULL;
    NTSTATUS status;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);

    if (Buffer != NULL && Buffer->Length != 0)
    {
        /* Sending final data along is not supported yet */
        status = STATUS_NOT_SUPPORTED;
        goto err_out;
    }
    if (!Socket->Connected || Socket->ConnectionFile == NULL)
    {
        status = STATUS_INVALID_DEVICE_STATE;
        goto err_out;
    }

    NetioContext = ExAllocatePoolZero(NonPagedPool, sizeof(*NetioContext), TAG_NETIO);
    if (NetioContext == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }
    NetioContext->socket = Socket;
    NetioContext->UserIrp = Irp;

    IoMarkIrpPending(Irp);
    ReferenceSocket(Socket);

    status = TdiDisconnect(&tdiIrp, Socket->ConnectionFile, NULL,
                           (Flags & WSK_FLAG_ABORTIVE) ? TDI_DISCONNECT_ABORT : TDI_DISCONNECT_RELEASE,
                           NetioComplete, NetioContext, NULL, NULL);
    if (!NT_SUCCESS(status))
    {
        DereferenceSocket(Socket);
        ExFreePoolWithTag(NetioContext, TAG_NETIO);
        goto err_out;
    }
    return STATUS_PENDING;

err_out:
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    return STATUS_PENDING;
}


static WSK_PROVIDER_CONNECTION_DISPATCH TcpDispatch = {
    .WskControlSocket = WskControlSocket,
    .WskCloseSocket = WskCloseSocket,
    .WskBind = WskBind,
    .WskConnect = WskConnect,
    .WskGetLocalAddress = WskGetLocalAddress,
    .WskGetRemoteAddress = WskGetRemoteAddress,
    .WskSend = WskSend,
    .WskReceive = WskReceive,
    .WskDisconnect = WskDisconnect,
    .WskRelease = WskReleaseTcp,
};

static NTSTATUS WSKAPI
WskSocket(
    _In_ PWSK_CLIENT Client,
    _In_ ADDRESS_FAMILY AddressFamily,
    _In_ USHORT SocketType,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _In_opt_ PVOID SocketContext,
    _In_opt_ const VOID * Dispatch,
    _In_opt_ PEPROCESS OwningProcess,
    _In_opt_ PETHREAD OwningThread,
    _In_opt_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _Inout_ PIRP Irp)
{
    PWSK_SOCKET_INTERNAL Socket;
    NTSTATUS status;

    FUNCTION_TRACE;

    IoSetNextIrpStackLocation(Irp);

    if (AddressFamily != AF_INET)
    {
        DPRINT1("Address family %d not supported\n", AddressFamily);
        status = STATUS_NOT_SUPPORTED;
        goto err_out;
    }
    switch (SocketType)
    {
        case SOCK_DGRAM:
            if (Protocol != IPPROTO_UDP)
            {
                DPRINT1("SOCK_DGRAM only supports IPPROTO_UDP\n");
                status = STATUS_INVALID_PARAMETER;
                goto err_out;
            }
            if (Flags != WSK_FLAG_DATAGRAM_SOCKET)
            {
                DPRINT1("SOCK_DGRAM flags must be WSK_FLAG_DATAGRAM_SOCKET\n");
                status = STATUS_INVALID_PARAMETER;
                goto err_out;
            }
            break;

        case SOCK_STREAM:
            if (Protocol != IPPROTO_TCP)
            {
                DPRINT1("SOCK_STREAM only supports IPPROTO_TCP\n");
                status = STATUS_INVALID_PARAMETER;
                goto err_out;
            }
            if ((Flags != WSK_FLAG_CONNECTION_SOCKET) && (Flags != WSK_FLAG_LISTEN_SOCKET))
            {
                DPRINT1("SOCK_STREAM flags must be either WSK_FLAG_CONNECTION_SOCKET or WSK_FLAG_LISTEN_SOCKET\n");
                status = STATUS_INVALID_PARAMETER;
                goto err_out;
            }
            break;

        case SOCK_RAW:
            DPRINT1("SOCK_RAW not supported\n");
            status = STATUS_NOT_SUPPORTED;
            goto err_out;

        default:
            status = STATUS_INVALID_PARAMETER;
            goto err_out;
    }

    Socket = ExAllocatePoolZero(NonPagedPool, sizeof(*Socket), TAG_NETIO);
    if (Socket == NULL)
    {
        DPRINT1("WskSocket: Out of memory\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }
    Socket->family = AddressFamily;
    Socket->type = SocketType;
    Socket->proto = Protocol;
    Socket->WskFlags = Flags;
    Socket->user_context = SocketContext;
    Socket->ClientDispatch = Dispatch;
    Socket->RefCount = 1;            /* DereferenceSocket() is in WskCloseSocket */
    KeInitializeSpinLock(&Socket->RecvLock);
    InitializeListHead(&Socket->RecvRequests);
    InitializeListHead(&Socket->DatagramBacklog);
    ExInitializeRundownProtection(&Socket->EventRundown);

    switch (SocketType)
    {
        case SOCK_DGRAM:
            Socket->Socket.Dispatch = &UdpDispatch;
            RtlInitUnicodeString(&Socket->TdiName, L"\\Device\\Udp");
            break;
        case SOCK_STREAM:
            Socket->Socket.Dispatch = &TcpDispatch;
            RtlInitUnicodeString(&Socket->TdiName, L"\\Device\\Tcp");

            if (Flags != WSK_FLAG_LISTEN_SOCKET)
            {
                status = TdiOpenConnectionEndpointFile(&Socket->TdiName, &Socket->ConnectionHandle, &Socket->ConnectionFile);
                if (!NT_SUCCESS(status))
                {
                    DPRINT1("Could not open TDI handle, status is %x\n", status);
                    ExFreePoolWithTag(Socket, TAG_NETIO);
                    goto err_out;
                }
            }
            else
            {
                KeInitializeEvent(&Socket->StartListenEvent, SynchronizationEvent, FALSE);
                Socket->ListenThreadShouldRun = TRUE;

                status = PsCreateSystemThread(&Socket->ListenThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, RequeueListenThread, Socket);
                if (!NT_SUCCESS(status))
                {
                    DPRINT1("Could not start listen thread, status is %x\n", status);
                    ExFreePoolWithTag(Socket, TAG_NETIO);
                    goto err_out;
                }
                status = ObReferenceObjectByHandle(Socket->ListenThreadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, (void **)&Socket->ListenThread, NULL);
                if (!NT_SUCCESS(status))
                {
                    DPRINT1("Could not get a PKTHREAD object, status is %x\n", status);
                    ExFreePoolWithTag(Socket, TAG_NETIO);
                    goto err_out;
                }
                ZwClose(Socket->ListenThreadHandle);
            }

            if (Flags == WSK_FLAG_LISTEN_SOCKET && Socket->ClientDispatch == NULL)
            {
                DPRINT1("Warning: no callbacks given for listen socket\n");
            }
            break;

        default:
            DPRINT1("Socket type not yet supported\n");
            status = STATUS_NOT_SUPPORTED;
            goto err_out;
    }

    Irp->IoStatus.Information = (ULONG_PTR)Socket;
    status = STATUS_SUCCESS;

err_out:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return STATUS_PENDING;
}

static WSK_PROVIDER_DISPATCH provider_dispatch = {
    .Version = 0,
    .Reserved = 0,
    .WskSocket = WskSocket,
    .WskSocketConnect = WskSocketConnect,
    .WskControlClient = WskControlClient
};

NTSTATUS WSKAPI
WskRegister(_In_ PWSK_CLIENT_NPI client_npi, _Out_ PWSK_REGISTRATION reg)
{
    FUNCTION_TRACE;

    reg->ReservedRegistrationState = 0;
    reg->ReservedRegistrationContext = NULL;
    KeInitializeSpinLock(&reg->ReservedRegistrationLock);

    return STATUS_SUCCESS;
}

NTSTATUS WSKAPI
WskCaptureProviderNPI(_In_ PWSK_REGISTRATION reg, _In_ ULONG wait, _Out_ PWSK_PROVIDER_NPI npi)
{
    FUNCTION_TRACE;

    npi->Client = NULL;
    npi->Dispatch = &provider_dispatch;

    return StartSocketDereferenceThread();
}

VOID WSKAPI
WskReleaseProviderNPI(_In_ PWSK_REGISTRATION reg)
{
    FUNCTION_TRACE;

    StopSocketDereferenceThread();
}

VOID WSKAPI
WskDeregister(_In_ PWSK_REGISTRATION reg)
{
    FUNCTION_TRACE;

    reg->ReservedRegistrationState = 0;
    reg->ReservedRegistrationContext = NULL;
}
