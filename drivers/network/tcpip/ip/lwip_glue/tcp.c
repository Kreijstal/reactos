#include <debug.h>
#include <lwip/tcpip.h>

#include "lwip_glue.h"

static const char * const tcp_state_str[] = {
  "CLOSED",
  "LISTEN",
  "SYN_SENT",
  "SYN_RCVD",
  "ESTABLISHED",
  "FIN_WAIT_1",
  "FIN_WAIT_2",
  "CLOSE_WAIT",
  "CLOSING",
  "LAST_ACK",
  "TIME_WAIT"
};

/* The way that lwIP does multi-threading is really not ideal for our purposes but
 * we best go along with it unless we want another unstable TCP library. lwIP uses
 * a thread called the "tcpip thread" which is the only one allowed to call raw API
 * functions. Since this is the case, for each of our LibTCP* functions, we queue a request
 * for a callback to "tcpip thread" which calls our LibTCP*Callback functions. Yes, this is
 * a lot of unnecessary thread swapping and it could definitely be faster, but I don't want
 * to going messing around in lwIP because I have no desire to create another mess like oskittcp */

extern KEVENT TerminationEvent;
extern NPAGED_LOOKASIDE_LIST MessageLookasideList;
extern NPAGED_LOOKASIDE_LIST QueueEntryLookasideList;

/* Required for ERR_T to NTSTATUS translation in receive error handling */
NTSTATUS TCPTranslateError(const err_t err);

void
LibTCPDumpPcb(PVOID SocketContext)
{
    struct tcp_pcb *pcb = (struct tcp_pcb*)SocketContext;
    unsigned int addr = lwip_ntohl(pcb->remote_ip.addr);

    DbgPrint("\tState: %s\n", tcp_state_str[pcb->state]);
    DbgPrint("\tRemote: (%d.%d.%d.%d, %d)\n",
    (addr >> 24) & 0xFF,
    (addr >> 16) & 0xFF,
    (addr >> 8) & 0xFF,
    addr & 0xFF,
    pcb->remote_port);
}

static
void
LibTCPEmptyQueue(PCONNECTION_ENDPOINT Connection)
{
    PLIST_ENTRY Entry;
    PQUEUE_ENTRY qp = NULL;

    ReferenceObject(Connection);

    while (!IsListEmpty(&Connection->PacketQueue))
    {
        Entry = RemoveHeadList(&Connection->PacketQueue);
        qp = CONTAINING_RECORD(Entry, QUEUE_ENTRY, ListEntry);

        /* We're in the tcpip thread here so this is safe */
        pbuf_free(qp->p);

        ExFreeToNPagedLookasideList(&QueueEntryLookasideList, qp);
    }

    DereferenceObject(Connection);
}

void LibTCPEnqueuePacket(PCONNECTION_ENDPOINT Connection, struct pbuf *p)
{
    PQUEUE_ENTRY qp;

    qp = (PQUEUE_ENTRY)ExAllocateFromNPagedLookasideList(&QueueEntryLookasideList);
    qp->p = p;
    qp->Offset = 0;

    LockObject(Connection);
    InsertTailList(&Connection->PacketQueue, &qp->ListEntry);
    UnlockObject(Connection);
}

/* Apply the receive window credit owed for data the client already consumed.
 * MUST run in the tcpip-thread context. */
static
void
LibTCPFlushRecvCredit(PCONNECTION_ENDPOINT Connection, PTCP_PCB pcb)
{
    LONG Credit;

    Credit = InterlockedExchange(&Connection->PendingRecvCredit, 0);
    if (Credit <= 0)
        return;

    /* The client cannot consume more than the window we advertised */
    ASSERT(Credit <= TCP_WND);
    if (Credit > TCP_WND)
        Credit = TCP_WND;

    tcp_recved(pcb, (u16_t)Credit);
}

static
void
LibTCPRecvCreditCallback(void *arg)
{
    PCONNECTION_ENDPOINT Connection = arg;
    PTCP_PCB pcb = Connection->SocketContext;

    if (pcb != NULL)
        LibTCPFlushRecvCredit(Connection, pcb);

    /* Matches the reference taken in LibTCPAccountRecvCredit */
    DereferenceObject(Connection);
}

/* Record that the client consumed Bytes and ask the tcpip thread to reopen the
 * window by that much. Callable from any context, and in particular from the
 * tcpip thread itself (InternalRecvEventHandler -> TCPRecvEventHandler ->
 * LibTCPGetDataFromConnectionQueue), so this must never block. */
static
void
LibTCPAccountRecvCredit(PCONNECTION_ENDPOINT Connection, UINT Bytes)
{
    if (Bytes == 0)
        return;

    InterlockedExchangeAdd(&Connection->PendingRecvCredit, (LONG)Bytes);

    if (Connection->RecvCreditMsg == NULL)
        return;

    ReferenceObject(Connection);
    if (tcpip_callbackmsg_trycallback(Connection->RecvCreditMsg) != ERR_OK)
    {
        /* The mailbox is full. The credit stays pending and is applied by the
         * next successful post or by InternalPollEventHandler. */
        DereferenceObject(Connection);
    }
}

BOOLEAN
LibTCPAllocateRecvCredit(PCONNECTION_ENDPOINT Connection)
{
    Connection->RecvCreditMsg = tcpip_callbackmsg_new(LibTCPRecvCreditCallback, Connection);

    return (Connection->RecvCreditMsg != NULL);
}

VOID
LibTCPFreeRecvCredit(PCONNECTION_ENDPOINT Connection)
{
    if (Connection->RecvCreditMsg == NULL)
        return;

    /* Safe: a queued message holds a reference on the connection, so this
     * cannot run while a post is still outstanding. */
    tcpip_callbackmsg_delete(Connection->RecvCreditMsg);
    Connection->RecvCreditMsg = NULL;
}

PQUEUE_ENTRY LibTCPDequeuePacket(PCONNECTION_ENDPOINT Connection)
{
    PLIST_ENTRY Entry;
    PQUEUE_ENTRY qp = NULL;

    if (IsListEmpty(&Connection->PacketQueue)) return NULL;

    Entry = RemoveHeadList(&Connection->PacketQueue);

    qp = CONTAINING_RECORD(Entry, QUEUE_ENTRY, ListEntry);

    return qp;
}

NTSTATUS LibTCPGetDataFromConnectionQueue(PCONNECTION_ENDPOINT Connection, PUCHAR RecvBuffer, UINT RecvLen, UINT *Received)
{
    PQUEUE_ENTRY qp;
    struct pbuf* p;
    NTSTATUS Status;
    UINT ReadLength, PayloadLength, Offset, Copied;

    (*Received) = 0;

    LockObject(Connection);

    if (!IsListEmpty(&Connection->PacketQueue))
    {
        while ((qp = LibTCPDequeuePacket(Connection)) != NULL)
        {
            p = qp->p;

            /* Calculate the payload length first */
            PayloadLength = p->tot_len;
            PayloadLength -= qp->Offset;
            Offset = qp->Offset;

            /* Check if we're reading the whole buffer */
            ReadLength = MIN(PayloadLength, RecvLen);
            ASSERT(ReadLength != 0);
            if (ReadLength != PayloadLength)
            {
                /* Save this one for later */
                qp->Offset += ReadLength;
                InsertHeadList(&Connection->PacketQueue, &qp->ListEntry);
                qp = NULL;
            }

            Copied = pbuf_copy_partial(p, RecvBuffer, ReadLength, Offset);
            ASSERT(Copied == ReadLength);

            /* Update trackers */
            RecvLen -= ReadLength;
            RecvBuffer += ReadLength;
            (*Received) += ReadLength;

            if (qp != NULL)
            {
                /* Use this special pbuf free callback function because we're outside tcpip thread */
                pbuf_free_callback(qp->p);

                ExFreeToNPagedLookasideList(&QueueEntryLookasideList, qp);
            }
            else
            {
                /* If we get here, it means we've filled the buffer */
                ASSERT(RecvLen == 0);
            }

            ASSERT((*Received) != 0);
            Status = STATUS_SUCCESS;

            if (!RecvLen)
                break;
        }
    }
    else
    {
        if (Connection->ReceiveShutdown)
            Status = Connection->ReceiveShutdownStatus;
        else
            Status = STATUS_PENDING;
    }

    UnlockObject(Connection);

    /* Now that the data is out of the queue, give the window back to lwIP.
     * This is what throttles the sender when the client reads slowly. */
    LibTCPAccountRecvCredit(Connection, *Received);

    return Status;
}

static
BOOLEAN
WaitForEventSafely(PRKEVENT Event)
{
    PVOID WaitObjects[] = {Event, &TerminationEvent};

    if (KeWaitForMultipleObjects(2,
                                 WaitObjects,
                                 WaitAny,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL,
                                 NULL) == STATUS_WAIT_0)
    {
        /* Signalled by the caller's event */
        return TRUE;
    }
    else /* if KeWaitForMultipleObjects() == STATUS_WAIT_1 */
    {
        /* Signalled by our termination event */
        return FALSE;
    }
}

static
err_t
InternalSendEventHandler(void *arg, PTCP_PCB pcb, const u16_t space)
{
    /* Make sure the socket didn't get closed */
    if (!arg) return ERR_OK;

    TCPSendEventHandler(arg, space);

    return ERR_OK;
}

/* Periodic safety net: applies any receive credit that could not be posted to
 * the tcpip thread. Without this a dropped post could leave the window closed
 * with no further receive to trigger a retry. */
static
err_t
InternalPollEventHandler(void *arg, PTCP_PCB pcb)
{
    PCONNECTION_ENDPOINT Connection = arg;

    /* Make sure the socket didn't get closed */
    if (!arg) return ERR_OK;

    LibTCPFlushRecvCredit(Connection, pcb);

    return ERR_OK;
}

static
err_t
InternalRecvEventHandler(void *arg, PTCP_PCB pcb, struct pbuf *p, const err_t err)
{
    PCONNECTION_ENDPOINT Connection = arg;

    /* Make sure the socket didn't get closed */
    if (!arg)
    {
        if (p)
            pbuf_free(p);

        return ERR_OK;
    }

    if (p)
    {
        LibTCPEnqueuePacket(Connection, p);

        /* Deliberately no tcp_recved() here: the window is reopened only once
         * the client has actually consumed the data, in
         * LibTCPGetDataFromConnectionQueue. Crediting on arrival would keep the
         * window permanently open and let a slow reader queue without bound. */
        TCPRecvEventHandler(arg);
    }
    else if (err == ERR_OK)
    {
        /* Complete pending reads with 0 bytes to indicate a graceful closure,
         * but note that send is still possible in this state so we don't close the
         * whole socket here (by calling tcp_close()) as that would violate TCP specs
         */
        Connection->ReceiveShutdown = TRUE;
        Connection->ReceiveShutdownStatus = STATUS_SUCCESS;

        /* If we already did a send shutdown, we're in TIME_WAIT so we can't use this PCB anymore */
        if (Connection->SendShutdown)
        {
            Connection->SocketContext = NULL;
            tcp_arg(pcb, NULL);
        }

        /* Indicate the graceful close event */
        TCPRecvEventHandler(arg);

        /* If the PCB is gone, clean up the connection */
        if (Connection->SendShutdown)
        {
            TCPFinEventHandler(Connection, ERR_CLSD);
        }
    }

    return ERR_OK;
}

/* This function MUST return an error value that is not ERR_ABRT or ERR_OK if the connection
 * is not accepted to avoid leaking the new PCB */
static
err_t
InternalAcceptEventHandler(void *arg, PTCP_PCB newpcb, const err_t err)
{
    /* Make sure the socket didn't get closed */
    if (!arg)
        return ERR_CLSD;

    TCPAcceptEventHandler(arg, newpcb);

    /* Set in LibTCPAccept (called from TCPAcceptEventHandler) */
    if (newpcb->callback_arg)
        return ERR_OK;
    else
        return ERR_CLSD;
}

static
err_t
InternalConnectEventHandler(void *arg, PTCP_PCB pcb, const err_t err)
{
    /* Make sure the socket didn't get closed */
    if (!arg)
        return ERR_OK;

    TCPConnectEventHandler(arg, err);

    return ERR_OK;
}

static
void
InternalErrorEventHandler(void *arg, const err_t err)
{
    PCONNECTION_ENDPOINT Connection = arg;

    /* Make sure the socket didn't get closed */
    if (!arg || Connection->SocketContext == NULL) return;

    /* The PCB is dead now */
    Connection->SocketContext = NULL;

    /* Give them one shot to receive the remaining data */
    Connection->ReceiveShutdown = TRUE;
    Connection->ReceiveShutdownStatus = TCPTranslateError(err);
    TCPRecvEventHandler(Connection);

    /* Terminate the connection */
    TCPFinEventHandler(Connection, err);
}

/* Handlers installed on a fully established PCB held on the listener's
 * PendingAcceptPcbs queue because no TDI listen request was pending when
 * lwIP completed the handshake (see TCPAcceptEventHandler). They all run
 * in the tcpip-thread context, with arg pointing to the queue entry. */

static
err_t
PendingAcceptRecvHandler(void *arg, PTCP_PCB pcb, struct pbuf *p, const err_t err)
{
    PPENDING_ACCEPT_PCB PendingEntry = arg;
    PCONNECTION_ENDPOINT Listener;

    if (!arg)
    {
        if (p)
            pbuf_free(p);

        return ERR_OK;
    }

    if (p != NULL)
    {
        /* Refuse the data without freeing it: lwIP keeps it in
         * pcb->refused_data and re-delivers it once the real handlers are
         * installed by LibTCPAccept. The receive window provides the flow
         * control while the connection waits in the queue. */
        return ERR_MEM;
    }

    /* p == NULL: the peer closed the connection while it was queued */
    Listener = PendingEntry->Listener;

    LockObject(Listener);
    PendingEntry->Dead = TRUE;
    RemoveEntryList(&PendingEntry->Entry);
    Listener->PendingAcceptCount--;
    UnlockObject(Listener);

    /* Detach from the PCB and hand it back to lwIP */
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_backlog_accepted(pcb);
    if (tcp_close(pcb) != ERR_OK)
        tcp_abort(pcb);

    ExFreePoolWithTag(PendingEntry, PENDING_ACCEPT_PCB_TAG);
    DereferenceObject(Listener);

    return ERR_OK;
}

static
err_t
PendingAcceptSentHandler(void *arg, PTCP_PCB pcb, const u16_t space)
{
    /* Nothing was queued for sending yet: nothing to do */
    return ERR_OK;
}

static
void
PendingAcceptErrHandler(void *arg, const err_t err)
{
    PPENDING_ACCEPT_PCB PendingEntry = arg;
    PCONNECTION_ENDPOINT Listener;

    /* The PCB is already gone (lwIP frees it before calling us) */
    if (!arg)
        return;

    Listener = PendingEntry->Listener;

    LockObject(Listener);
    PendingEntry->Dead = TRUE;
    RemoveEntryList(&PendingEntry->Entry);
    Listener->PendingAcceptCount--;
    UnlockObject(Listener);

    ExFreePoolWithTag(PendingEntry, PENDING_ACCEPT_PCB_TAG);
    DereferenceObject(Listener);
}

void
LibTCPHoldPendingAccept(PTCP_PCB pcb, PPENDING_ACCEPT_PCB PendingEntry)
{
    /* Called from TCPAcceptEventHandler, i.e. in the tcpip-thread context,
     * so the raw API can be used directly (same as LibTCPAccept) */
    ASSERT(PendingEntry);

    tcp_arg(pcb, PendingEntry);
    tcp_recv(pcb, PendingAcceptRecvHandler);
    tcp_sent(pcb, PendingAcceptSentHandler);
    tcp_err(pcb, PendingAcceptErrHandler);

    /* Keep the connection counted against the lwIP listen backlog until a
     * TDI listen request claims it */
    tcp_backlog_delayed(pcb);
}

static
void
LibTCPDispatchPendingAcceptCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PCONNECTION_ENDPOINT Connection = msg->Input.DispatchAccept.Connection;
    PPENDING_ACCEPT_PCB PendingEntry = NULL;
    PLIST_ENTRY Entry;
    struct tcp_pcb *pcb;

    ASSERT(msg);

    LockObject(Connection);
    if (!IsListEmpty(&Connection->PendingAcceptPcbs))
    {
        Entry = RemoveHeadList(&Connection->PendingAcceptPcbs);
        PendingEntry = CONTAINING_RECORD(Entry, PENDING_ACCEPT_PCB, Entry);

        /* Entries are unlinked in the same critical section that marks
         * them dead, so a queued entry is always live */
        ASSERT(!PendingEntry->Dead);

        Connection->PendingAcceptCount--;
    }
    UnlockObject(Connection);

    if (PendingEntry)
    {
        pcb = PendingEntry->NewPcb;

        /* Detach the holding handlers; TCPAcceptEventHandler either installs
         * the real ones via LibTCPAccept, re-queues the PCB, or leaves
         * callback_arg NULL on failure */
        tcp_arg(pcb, NULL);
        tcp_backlog_accepted(pcb);

        DereferenceObject(PendingEntry->Listener);
        ExFreePoolWithTag(PendingEntry, PENDING_ACCEPT_PCB_TAG);

        TCPAcceptEventHandler(Connection, pcb);

        /* Same contract as InternalAcceptEventHandler: an unclaimed PCB must
         * not leak (this can only happen on allocation failure). lwIP
         * re-delivers pcb->refused_data by itself once the new recv handler
         * is in place. */
        if (pcb->callback_arg == NULL)
            tcp_abort(pcb);
    }

    DereferenceObject(Connection);

    ExFreeToNPagedLookasideList(&MessageLookasideList, msg);
}

void
LibTCPDispatchPendingAccept(PCONNECTION_ENDPOINT Connection)
{
    struct lwip_callback_msg *msg;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (!msg)
        return;

    ReferenceObject(Connection);
    msg->Input.DispatchAccept.Connection = Connection;

    /* Fire-and-forget: the caller (TCPAccept) runs in TDI dispatch context
     * with the accepting endpoint already locked, and the callback locks
     * that endpoint again (TCPAcceptEventHandler locks the bucket's
     * AssociatedEndpoint), so waiting on a completion event here would
     * deadlock. The callback frees the message. */
    if (tcpip_callback_with_block(LibTCPDispatchPendingAcceptCallback, msg, 1) != ERR_OK)
    {
        DereferenceObject(Connection);
        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);
    }
}

static
void
LibTCPDrainPendingAcceptQueue(PCONNECTION_ENDPOINT Connection)
{
    PPENDING_ACCEPT_PCB PendingEntry;
    PLIST_ENTRY Entry;

    /* We're in the tcpip thread here so aborting the PCBs is safe */
    for (;;)
    {
        LockObject(Connection);
        if (IsListEmpty(&Connection->PendingAcceptPcbs))
        {
            UnlockObject(Connection);
            break;
        }

        Entry = RemoveHeadList(&Connection->PendingAcceptPcbs);
        PendingEntry = CONTAINING_RECORD(Entry, PENDING_ACCEPT_PCB, Entry);
        ASSERT(!PendingEntry->Dead);
        Connection->PendingAcceptCount--;
        UnlockObject(Connection);

        /* Silence our handlers before aborting so PendingAcceptErrHandler
         * doesn't run on the entry we already own */
        tcp_arg(PendingEntry->NewPcb, NULL);
        tcp_abort(PendingEntry->NewPcb);

        ExFreePoolWithTag(PendingEntry, PENDING_ACCEPT_PCB_TAG);
        DereferenceObject(Connection);
    }
}

static
void
LibTCPSocketCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;

    ASSERT(msg);

    msg->Output.Socket.NewPcb = tcp_new();

    if (msg->Output.Socket.NewPcb)
    {
        tcp_arg(msg->Output.Socket.NewPcb, msg->Input.Socket.Arg);
        tcp_err(msg->Output.Socket.NewPcb, InternalErrorEventHandler);
    }

    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

struct tcp_pcb *
LibTCPSocket(void *arg)
{
    struct lwip_callback_msg *msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    struct tcp_pcb *ret;

    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Socket.Arg = arg;

        tcpip_callback_with_block(LibTCPSocketCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Socket.NewPcb;
        else
            ret = NULL;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return NULL;
}

static
void
LibTCPFreeSocketCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;

    ASSERT(msg);

    /* Calling tcp_close will free it */
    tcp_close(msg->Input.FreeSocket.pcb);

    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

void LibTCPFreeSocket(PTCP_PCB pcb)
{
    struct lwip_callback_msg msg;

    KeInitializeEvent(&msg.Event, NotificationEvent, FALSE);
    msg.Input.FreeSocket.pcb = pcb;

    tcpip_callback_with_block(LibTCPFreeSocketCallback, &msg, 1);

    WaitForEventSafely(&msg.Event);
}


static
void
LibTCPBindCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PTCP_PCB pcb = msg->Input.Bind.Connection->SocketContext;

    ASSERT(msg);

    if (!msg->Input.Bind.Connection->SocketContext)
    {
        msg->Output.Bind.Error = ERR_CLSD;
        goto done;
    }

    /* We're guaranteed that the local address is valid to bind at this point */
    pcb->so_options |= SOF_REUSEADDR;

    msg->Output.Bind.Error = tcp_bind(pcb,
                                      msg->Input.Bind.IpAddress,
                                      lwip_ntohs(msg->Input.Bind.Port));

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPBind(PCONNECTION_ENDPOINT Connection, ip4_addr_t *const ipaddr, const u16_t port)
{
    struct lwip_callback_msg *msg;
    err_t ret;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Bind.Connection = Connection;
        msg->Input.Bind.IpAddress = ipaddr;
        msg->Input.Bind.Port = port;

        tcpip_callback_with_block(LibTCPBindCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Bind.Error;
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

static
void
LibTCPListenCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    u8_t backlog;

    ASSERT(msg);

    if (!msg->Input.Listen.Connection->SocketContext)
    {
        msg->Output.Listen.NewPcb = NULL;
        goto done;
    }

    /* lwIP stores the listen backlog in a u8_t (tcp_pcb_listen.backlog), so
     * clamp it here instead of letting the parameter silently truncate
     * (e.g. 1024 -> 0, which tcp_backlog_set turns into a backlog of 1) */
    backlog = (msg->Input.Listen.Backlog > 0xFF) ? 0xFF : (u8_t)msg->Input.Listen.Backlog;

    msg->Output.Listen.NewPcb = tcp_listen_with_backlog((PTCP_PCB)msg->Input.Listen.Connection->SocketContext, backlog);

    if (msg->Output.Listen.NewPcb)
    {
        tcp_accept(msg->Output.Listen.NewPcb, InternalAcceptEventHandler);
    }

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

PTCP_PCB
LibTCPListen(PCONNECTION_ENDPOINT Connection, const ULONG backlog)
{
    struct lwip_callback_msg *msg;
    PTCP_PCB ret;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Listen.Connection = Connection;
        msg->Input.Listen.Backlog = backlog;

        tcpip_callback_with_block(LibTCPListenCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Listen.NewPcb;
        else
            ret = NULL;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return NULL;
}

static
void
LibTCPSendCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PTCP_PCB pcb = msg->Input.Send.Connection->SocketContext;
    ULONG SendLength;
    UCHAR SendFlags;

    ASSERT(msg);

    if (!msg->Input.Send.Connection->SocketContext)
    {
        msg->Output.Send.Error = ERR_CLSD;
        goto done;
    }

    if (msg->Input.Send.Connection->SendShutdown)
    {
        msg->Output.Send.Error = ERR_CLSD;
        goto done;
    }

    SendFlags = TCP_WRITE_FLAG_COPY;
    SendLength = msg->Input.Send.DataLength;
    if (tcp_sndbuf(pcb) == 0)
    {
        /* No buffer space so return pending */
        msg->Output.Send.Error = ERR_INPROGRESS;
        goto done;
    }
    else if (tcp_sndbuf(pcb) < SendLength)
    {
        /* We've got some room so let's send what we can */
        SendLength = tcp_sndbuf(pcb);

        /* Don't set the push flag */
        SendFlags |= TCP_WRITE_FLAG_MORE;
    }

    msg->Output.Send.Error = tcp_write(pcb,
                                       msg->Input.Send.Data,
                                       SendLength,
                                       SendFlags);
    if (msg->Output.Send.Error == ERR_OK)
    {
        /* Queued successfully so try to send it */
        tcp_output((PTCP_PCB)msg->Input.Send.Connection->SocketContext);
        msg->Output.Send.Information = SendLength;
    }
    else if (msg->Output.Send.Error == ERR_MEM)
    {
        /* The queue is too long */
        msg->Output.Send.Error = ERR_INPROGRESS;
    }

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPSend(PCONNECTION_ENDPOINT Connection, void *const dataptr, const u16_t len, ULONG *sent, const int safe)
{
    err_t ret;
    struct lwip_callback_msg *msg;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Send.Connection = Connection;
        msg->Input.Send.Data = dataptr;
        msg->Input.Send.DataLength = len;

        if (safe)
            LibTCPSendCallback(msg);
        else
            tcpip_callback_with_block(LibTCPSendCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Send.Error;
        else
            ret = ERR_CLSD;

        if (ret == ERR_OK)
            *sent = msg->Output.Send.Information;
        else
            *sent = 0;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

static
void
LibTCPConnectCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    err_t Error;

    ASSERT(arg);

    if (!msg->Input.Connect.Connection->SocketContext)
    {
        msg->Output.Connect.Error = ERR_CLSD;
        goto done;
    }

    tcp_recv((PTCP_PCB)msg->Input.Connect.Connection->SocketContext, InternalRecvEventHandler);
    tcp_sent((PTCP_PCB)msg->Input.Connect.Connection->SocketContext, InternalSendEventHandler);
    tcp_poll((PTCP_PCB)msg->Input.Connect.Connection->SocketContext, InternalPollEventHandler, 2);

    Error = tcp_connect((PTCP_PCB)msg->Input.Connect.Connection->SocketContext,
                        msg->Input.Connect.IpAddress, lwip_ntohs(msg->Input.Connect.Port),
                        InternalConnectEventHandler);

    msg->Output.Connect.Error = Error == ERR_OK ? ERR_INPROGRESS : Error;

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPConnect(PCONNECTION_ENDPOINT Connection, ip_addr_t *const ipaddr, const u16_t port)
{
    struct lwip_callback_msg *msg;
    err_t ret;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Connect.Connection = Connection;
        msg->Input.Connect.IpAddress = ipaddr;
        msg->Input.Connect.Port = port;

        tcpip_callback_with_block(LibTCPConnectCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
        {
            ret = msg->Output.Connect.Error;
        }
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

static
void
LibTCPShutdownCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PTCP_PCB pcb = msg->Input.Shutdown.Connection->SocketContext;

    if (!msg->Input.Shutdown.Connection->SocketContext)
    {
        msg->Output.Shutdown.Error = ERR_CLSD;
        goto done;
    }

    /* LwIP makes the (questionable) assumption that SHUTDOWN_RDWR is equivalent to tcp_close().
     * This assumption holds even if the shutdown calls are done separately (even through multiple
     * WinSock shutdown() calls). This assumption means that lwIP has the right to deallocate our
     * PCB without telling us if we shutdown TX and RX. To avoid these problems, we'll clear the
     * socket context if we have called shutdown for TX and RX.
     */
    if (msg->Input.Shutdown.shut_rx != msg->Input.Shutdown.shut_tx) {
        if (msg->Input.Shutdown.shut_rx) {
            msg->Output.Shutdown.Error = tcp_shutdown(pcb, TRUE, FALSE);
        }
        if (msg->Input.Shutdown.shut_tx) {
            msg->Output.Shutdown.Error = tcp_shutdown(pcb, FALSE, TRUE);
        }
    }
    else if (msg->Input.Shutdown.shut_rx) {
        /* We received both RX and TX requests, which seems to mean closing connection from TDI.
         * So call tcp_close, otherwise we risk to be put in TCP_WAIT_* states, which makes further
         * attempts to close the socket to fail in this state.
         */
        msg->Output.Shutdown.Error = tcp_close(pcb);
    }
    else {
        /* This case shouldn't happen */
        DbgPrint("Requested socket shutdown(0, 0) !\n");
    }

    if (!msg->Output.Shutdown.Error)
    {
        if (msg->Input.Shutdown.shut_rx)
        {
            msg->Input.Shutdown.Connection->ReceiveShutdown = TRUE;
            msg->Input.Shutdown.Connection->ReceiveShutdownStatus = STATUS_FILE_CLOSED;
        }

        if (msg->Input.Shutdown.shut_tx)
            msg->Input.Shutdown.Connection->SendShutdown = TRUE;

        if (msg->Input.Shutdown.Connection->ReceiveShutdown &&
            msg->Input.Shutdown.Connection->SendShutdown)
        {
            /* The PCB is not ours anymore */
            msg->Input.Shutdown.Connection->SocketContext = NULL;
            tcp_arg(pcb, NULL);
            TCPFinEventHandler(msg->Input.Shutdown.Connection, ERR_CLSD);
        }
    }

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPShutdown(PCONNECTION_ENDPOINT Connection, const int shut_rx, const int shut_tx)
{
    struct lwip_callback_msg *msg;
    err_t ret;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);

        msg->Input.Shutdown.Connection = Connection;
        msg->Input.Shutdown.shut_rx = shut_rx;
        msg->Input.Shutdown.shut_tx = shut_tx;

        tcpip_callback_with_block(LibTCPShutdownCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Shutdown.Error;
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

static
void
LibTCPCloseCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PTCP_PCB pcb = msg->Input.Close.Connection->SocketContext;

    /* Empty the queue even if we're already "closed" */
    LibTCPEmptyQueue(msg->Input.Close.Connection);

    /* Abort any established connections still waiting for a TDI listen
     * request (only listeners ever have these queued) */
    LibTCPDrainPendingAcceptQueue(msg->Input.Close.Connection);

    /* Check if we've already been closed */
    if (msg->Input.Close.Connection->Closing)
    {
        msg->Output.Close.Error = ERR_OK;
        goto done;
    }

    /* Enter "closing" mode if we're doing a normal close */
    if (msg->Input.Close.Callback)
        msg->Input.Close.Connection->Closing = TRUE;

    /* Check if the PCB was already "closed" but the client doesn't know it yet */
    if (!msg->Input.Close.Connection->SocketContext)
    {
        msg->Output.Close.Error = ERR_OK;
        goto done;
    }

    /* Clear the PCB pointer and stop callbacks */
    msg->Input.Close.Connection->SocketContext = NULL;
    tcp_arg(pcb, NULL);

    /* This may generate additional callbacks but we don't care,
     * because they're too inconsistent to rely on */
    msg->Output.Close.Error = tcp_close(pcb);

    if (msg->Output.Close.Error)
    {
        /* Restore the PCB pointer */
        msg->Input.Close.Connection->SocketContext = pcb;
        msg->Input.Close.Connection->Closing = FALSE;
    }
    else if (msg->Input.Close.Callback)
    {
        TCPFinEventHandler(msg->Input.Close.Connection, ERR_CLSD);
    }

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPClose(PCONNECTION_ENDPOINT Connection, const int safe, const int callback)
{
    err_t ret;
    struct lwip_callback_msg *msg;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);

        msg->Input.Close.Connection = Connection;
        msg->Input.Close.Callback = callback;

        if (safe)
            LibTCPCloseCallback(msg);
        else
            tcpip_callback_with_block(LibTCPCloseCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Close.Error;
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

void
LibTCPAccept(PTCP_PCB pcb, struct tcp_pcb *listen_pcb, void *arg)
{
    ASSERT(arg);

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, InternalRecvEventHandler);
    tcp_sent(pcb, InternalSendEventHandler);
    tcp_err(pcb, InternalErrorEventHandler);
    tcp_poll(pcb, InternalPollEventHandler, 2);
    tcp_arg(pcb, arg);

    tcp_accepted(listen_pcb);
}

err_t
LibTCPGetHostName(PTCP_PCB pcb, ip_addr_t *const ipaddr, u16_t *const port)
{
    if (!pcb)
        return ERR_CLSD;

    *ipaddr = pcb->local_ip;
    *port = pcb->local_port;

    return ERR_OK;
}

err_t
LibTCPGetPeerName(PTCP_PCB pcb, ip_addr_t * const ipaddr, u16_t * const port)
{
    if (!pcb)
        return ERR_CLSD;

    *ipaddr = pcb->remote_ip;
    *port = pcb->remote_port;

    return ERR_OK;
}

void
LibTCPSetNoDelay(
    PTCP_PCB pcb,
    BOOLEAN Set)
{
    if (Set)
        pcb->flags |= TF_NODELAY;
    else
        pcb->flags &= ~TF_NODELAY;
}

void
LibTCPSetKeepAlive(
    PTCP_PCB pcb,
    BOOLEAN Set)
{
    if (Set)
        pcb->so_options |= SOF_KEEPALIVE;
    else
        pcb->so_options &= ~SOF_KEEPALIVE;
}

void
LibTcpSetKeepAliveValues(
    PTCP_PCB pcb,
    u32_t KeepAliveTime,
    u32_t KeepAliveInterval
)
{
    pcb->keep_idle = KeepAliveTime;
    pcb->keep_intvl = KeepAliveInterval;
    pcb->keep_cnt = 10;
}

void
LibTCPGetSocketStatus(
    PTCP_PCB pcb,
    PULONG State)
{
    /* Translate state from enum tcp_state -> MIB_TCP_STATE */
    *State = pcb->state + 1;
}
