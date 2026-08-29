/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/write.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */

#include "afd.h"

static IO_COMPLETION_ROUTINE SendComplete;
static NTSTATUS NTAPI SendComplete
( PDEVICE_OBJECT DeviceObject,
  PIRP Irp,
  PVOID Context ) {
    NTSTATUS Status = Irp->IoStatus.Status;
    PAFD_FCB FCB = (PAFD_FCB)Context;
    PLIST_ENTRY NextIrpEntry;
    PIRP NextIrp = NULL;
    PIO_STACK_LOCATION NextIrpSp;
    PAFD_SEND_INFO SendReq = NULL;
    PAFD_MAPBUF Map;
    SIZE_T TotalBytesCopied = 0, TotalBytesProcessed = 0, SpaceAvail, i;
    UINT SendLength, BytesCopied;
    BOOLEAN HaltSendQueue;

    UNREFERENCED_PARAMETER(DeviceObject);

    /*
     * The Irp parameter passed in is the IRP of the stream between AFD and
     * TDI driver. It's not very useful to us. We need the IRPs of the stream
     * between usermode and AFD. Those are chained from
     * FCB->PendingIrpList[FUNCTION_SEND] and you'll see them in the code
     * below as "NextIrp" ('cause they are the next usermode IRP to be
     * processed).
     */

    AFD_DbgPrint(MID_TRACE,("Called, status %x, %u bytes used\n",
                            Irp->IoStatus.Status,
                            Irp->IoStatus.Information));

    if( !SocketAcquireStateLock( FCB ) )
        return STATUS_FILE_CLOSED;

    ASSERT(FCB->SendIrp.InFlightRequest == Irp);
    FCB->SendIrp.InFlightRequest = NULL;
    /* Request is not in flight any longer */

    if( FCB->SharedData.State == SOCKET_STATE_CLOSED ) {
        /* Cleanup our IRP queue because the FCB is being destroyed */
        while( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_SEND] ) ) {
            NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
            NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
            NextIrpSp = IoGetCurrentIrpStackLocation( NextIrp );
            SendReq = GetLockedData(NextIrp, NextIrpSp);
            NextIrp->IoStatus.Status = STATUS_FILE_CLOSED;
            NextIrp->IoStatus.Information = 0;
            UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
            if( NextIrp->MdlAddress ) UnlockRequest( NextIrp, IoGetCurrentIrpStackLocation( NextIrp ) );
            (void)IoSetCancelRoutine(NextIrp, NULL);
            IoCompleteRequest( NextIrp, IO_NETWORK_INCREMENT );
        }

        /* Every owner of the window is gone, so the window holds nothing that
         * anyone is still accounting for.  Leaving the counts behind would
         * strand them: a later send folds a stale SendOrphanedBytes into its
         * own, and a stale BytesUsed shrinks the window for good. */
        FCB->Send.BytesUsed = 0;
        FCB->SendOrphanedBytes = 0;

        RetryDisconnectCompletion(FCB);

        SocketStateUnlock( FCB );
        return STATUS_FILE_CLOSED;
    }

    if( !NT_SUCCESS(Status) ) {
        /* Complete all following send IRPs with error */

        while( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_SEND] ) ) {
            NextIrpEntry =
                RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
            NextIrp =
                CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
            NextIrpSp = IoGetCurrentIrpStackLocation( NextIrp );
            SendReq = GetLockedData(NextIrp, NextIrpSp);

            UnlockBuffers( SendReq->BufferArray,
                           SendReq->BufferCount,
                           FALSE );

            NextIrp->IoStatus.Status = Status;
            NextIrp->IoStatus.Information = 0;

            if ( NextIrp->MdlAddress ) UnlockRequest( NextIrp, IoGetCurrentIrpStackLocation( NextIrp ) );
            (void)IoSetCancelRoutine(NextIrp, NULL);
            IoCompleteRequest( NextIrp, IO_NETWORK_INCREMENT );
        }

        /* As above: the queue was emptied without the drain loop, so drop the
         * window accounting with it. */
        FCB->Send.BytesUsed = 0;
        FCB->SendOrphanedBytes = 0;

        RetryDisconnectCompletion(FCB);

        SocketStateUnlock( FCB );

        return STATUS_SUCCESS;
    }

    RtlMoveMemory( FCB->Send.Window,
                   FCB->Send.Window + Irp->IoStatus.Information,
                   FCB->Send.BytesUsed - Irp->IoStatus.Information );

    TotalBytesProcessed = 0;
    SendLength = Irp->IoStatus.Information;
    HaltSendQueue = FALSE;
    while (!IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]) && SendLength > 0) {
        NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
        NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
        NextIrpSp = IoGetCurrentIrpStackLocation( NextIrp );
        SendReq = GetLockedData(NextIrp, NextIrpSp);
        Map = (PAFD_MAPBUF)(SendReq->BufferArray + SendReq->BufferCount);

        TotalBytesCopied = (ULONG_PTR)NextIrp->Tail.Overlay.DriverContext[3];

        /* This send is still only waiting for window space -- it owns none of
         * the bytes the transport just reported, and neither does anything
         * behind it.  Whatever is left is a cancelled send's, and is retired
         * against FCB->SendOrphanedBytes once the loop ends.  Put the IRP back
         * and stop; the block below is what finally buffers it.  (The old
         * ASSERT(TotalBytesCopied != 0) here assumed every queued send owns
         * window bytes, which was never true of one parked by
         * LeaveIrpUntilLater().  ASSERT(SendLength == 0) below is the guard
         * that actually catches a miscount.) */
        if (TotalBytesCopied == 0)
        {
            InsertHeadList(&FCB->PendingIrpList[FUNCTION_SEND],
                           &NextIrp->Tail.Overlay.ListEntry);
            break;
        }

        /* If we didn't get enough, keep waiting */
        if (TotalBytesCopied > SendLength)
        {
            /* Update the bytes left to copy */
            TotalBytesCopied -= SendLength;
            NextIrp->Tail.Overlay.DriverContext[3] = (PVOID)TotalBytesCopied;

            /* Update the state variables */
            FCB->Send.BytesUsed -= SendLength;
            TotalBytesProcessed += SendLength;
            SendLength = 0;

            /* Pend the IRP */
            InsertHeadList(&FCB->PendingIrpList[FUNCTION_SEND],
                           &NextIrp->Tail.Overlay.ListEntry);
            HaltSendQueue = TRUE;
            break;
        }

        ASSERT(NextIrp->IoStatus.Information != 0);

        NextIrp->IoStatus.Status = Irp->IoStatus.Status;

        FCB->Send.BytesUsed -= TotalBytesCopied;
        TotalBytesProcessed += TotalBytesCopied;
        SendLength -= TotalBytesCopied;

        (void)IoSetCancelRoutine(NextIrp, NULL);

        UnlockBuffers( SendReq->BufferArray,
                       SendReq->BufferCount,
                       FALSE );

        if (NextIrp->MdlAddress) UnlockRequest(NextIrp, NextIrpSp);

        IoCompleteRequest(NextIrp, IO_NETWORK_INCREMENT);
    }

    /* Whatever is left belongs to sends that were cancelled after their
     * payload had already been copied into the window -- see
     * AfdReleaseSendWindowOwnership().  The transport transmitted those bytes
     * and counted them here, but there is no longer an IRP to complete for
     * them, so retire them against the running total directly.  They are
     * always the tail of the window: an interior stretch is folded into the
     * IRP that owns the bytes after it, and anything buffered later takes
     * them over as it is appended behind them. */
    if (SendLength > 0 && FCB->SendOrphanedBytes > 0)
    {
        UINT Orphaned = MIN(SendLength, FCB->SendOrphanedBytes);

        FCB->SendOrphanedBytes -= Orphaned;
        FCB->Send.BytesUsed -= Orphaned;
        TotalBytesProcessed += Orphaned;
        SendLength -= Orphaned;
    }

    /* Still non-zero means the transport reported bytes that neither a pending
     * send IRP nor the orphan counter claims, i.e. the window accounting has
     * drifted.  The bare ASSERT said only that it happened; on the ASUS it
     * fired 80 ms into a file transfer (2026-08-28 boot 70) and left nothing to
     * work from.  Print the terms first - the assert still fires, this only
     * makes it say WHICH side is short. */
    if (SendLength != 0)
    {
        AFD_DbgPrint(MIN_TRACE,("send window drift: SendLength=%u left, "
                     "Information=%u, orphaned=%u, BytesUsed=%u, processed=%u, "
                     "halted=%u, pendingEmpty=%u\n",
                     SendLength, (UINT)Irp->IoStatus.Information,
                     FCB->SendOrphanedBytes, FCB->Send.BytesUsed,
                     (UINT)TotalBytesProcessed, (UINT)HaltSendQueue,
                     (UINT)IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND])));
    }

    ASSERT(SendLength == 0);

   if ( !HaltSendQueue && !IsListEmpty( &FCB->PendingIrpList[FUNCTION_SEND] ) ) {
        NextIrpEntry = FCB->PendingIrpList[FUNCTION_SEND].Flink;
        NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
        NextIrpSp = IoGetCurrentIrpStackLocation( NextIrp );
        SendReq = GetLockedData(NextIrp, NextIrpSp);
        Map = (PAFD_MAPBUF)(SendReq->BufferArray + SendReq->BufferCount);

        AFD_DbgPrint(MID_TRACE,("SendReq @ %p\n", SendReq));

        /* Only a send that never got window space belongs here -- the one
         * LeaveIrpUntilLater() parked.  The head of the queue is NOT always
         * such a send: the drain loop above stops as soon as SendLength runs
         * out, so a send whose payload is already in the window stays at the
         * head whenever the transport reported exactly the bytes owned by the
         * sends ahead of it.  That is the ordinary case once two sends are
         * outstanding, because the second one is buffered while the first
         * TdiSend is still in flight and so is not covered by it.
         *
         * Buffering it again would append a second copy of bytes the window
         * already holds -- the peer receives the payload twice -- and
         * DriverContext[3] is overwritten rather than added to, so the window
         * would carry more bytes than the queue claims and the next completion
         * would trip ASSERT(SendLength == 0) above.  A send that is already
         * buffered needs nothing here: FCB->Send.BytesUsed is still non-zero,
         * so the TdiSend at the end of this function transmits it. */
        if (NextIrp->Tail.Overlay.DriverContext[3] != NULL)
            NextIrp = NULL;

        SpaceAvail = FCB->Send.Size - FCB->Send.BytesUsed;
        TotalBytesCopied = 0;

        /* Count the total transfer size */
        SendLength = 0;
        for (i = 0; i < SendReq->BufferCount; i++)
        {
            SendLength += SendReq->BufferArray[i].len;
        }

        /* Make sure we've got the space */
        if (NextIrp != NULL && SendLength > SpaceAvail)
        {
           /* Blocking sockets have to wait here */
           if (SendLength <= FCB->Send.Size && !((SendReq->AfdFlags & AFD_IMMEDIATE) || (FCB->NonBlocking)))
           {
               FCB->PollState &= ~AFD_EVENT_SEND;

               NextIrp = NULL;
           }

           /* Check if we can send anything */
           if (SpaceAvail == 0)
           {
               FCB->PollState &= ~AFD_EVENT_SEND;

               /* We should never be non-overlapped and get to this point */
               ASSERT(SendReq->AfdFlags & AFD_OVERLAPPED);

               NextIrp = NULL;
           }
        }

        if (NextIrp != NULL)
        {
            for( i = 0; i < SendReq->BufferCount; i++ ) {
                BytesCopied = MIN(SendReq->BufferArray[i].len, SpaceAvail);

                Map[i].BufferAddress =
                   MmMapLockedPages( Map[i].Mdl, KernelMode );

                RtlCopyMemory( FCB->Send.Window + FCB->Send.BytesUsed,
                               Map[i].BufferAddress,
                               BytesCopied );

                MmUnmapLockedPages( Map[i].BufferAddress, Map[i].Mdl );

                TotalBytesCopied += BytesCopied;
                SpaceAvail -= BytesCopied;
                FCB->Send.BytesUsed += BytesCopied;
            }

            NextIrp->IoStatus.Information = TotalBytesCopied;
            /* What we just appended sits behind any bytes orphaned by a
             * cancelled send, so those stop being the tail of the window and
             * this IRP takes them over.  Only the accounting moves -- the
             * caller is still told just its own byte count. */
            NextIrp->Tail.Overlay.DriverContext[3] = (PVOID)
                ((ULONG_PTR)NextIrp->IoStatus.Information + FCB->SendOrphanedBytes);
            FCB->SendOrphanedBytes = 0;
        }
    }

    if (FCB->Send.Size - FCB->Send.BytesUsed != 0 && !FCB->SendClosed &&
        IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]))
    {
        FCB->PollState |= AFD_EVENT_SEND;
        FCB->PollStatus[FD_WRITE_BIT] = STATUS_SUCCESS;
        PollReeval( FCB->DeviceExt, FCB->FileObject );
    }
    else
    {
        FCB->PollState &= ~AFD_EVENT_SEND;
    }


    /* Some data is still waiting */
    if( FCB->Send.BytesUsed )
    {
        Status = TdiSend( &FCB->SendIrp.InFlightRequest,
                          FCB->Connection.Object,
                          0,
                          FCB->Send.Window,
                          FCB->Send.BytesUsed,
                          SendComplete,
                          FCB );
    }
    else
    {
        /* Nothing is waiting so try to complete a pending disconnect */
        RetryDisconnectCompletion(FCB);
    }

    SocketStateUnlock( FCB );

    return STATUS_SUCCESS;
}

static IO_COMPLETION_ROUTINE PacketSocketSendComplete;
static NTSTATUS NTAPI PacketSocketSendComplete
( PDEVICE_OBJECT DeviceObject,
  PIRP Irp,
  PVOID Context ) {
    PAFD_FCB FCB = (PAFD_FCB)Context;
    PLIST_ENTRY NextIrpEntry;
    PIRP NextIrp;
    PAFD_SEND_INFO SendReq;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called, status %x, %u bytes used\n",
                            Irp->IoStatus.Status,
                            Irp->IoStatus.Information));

    if( !SocketAcquireStateLock( FCB ) )
        return STATUS_FILE_CLOSED;

    ASSERT(FCB->SendIrp.InFlightRequest == Irp);
    FCB->SendIrp.InFlightRequest = NULL;
    /* Request is not in flight any longer */

    if( FCB->SharedData.State == SOCKET_STATE_CLOSED ) {
        /* Cleanup our IRP queue because the FCB is being destroyed */
        while( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_SEND] ) ) {
            NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
            NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
            SendReq = GetLockedData(NextIrp, IoGetCurrentIrpStackLocation(NextIrp));
            NextIrp->IoStatus.Status = STATUS_FILE_CLOSED;
            NextIrp->IoStatus.Information = 0;
            (void)IoSetCancelRoutine(NextIrp, NULL);
            UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
            UnlockRequest( NextIrp, IoGetCurrentIrpStackLocation( NextIrp ) );
            IoCompleteRequest( NextIrp, IO_NETWORK_INCREMENT );
        }
        SocketStateUnlock( FCB );
        return STATUS_FILE_CLOSED;
    }

    ASSERT(!IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]));

    /* TDI spec guarantees FIFO ordering on IRPs */
    NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
    NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);

    SendReq = GetLockedData(NextIrp, IoGetCurrentIrpStackLocation(NextIrp));

    NextIrp->IoStatus.Status = Irp->IoStatus.Status;
    NextIrp->IoStatus.Information = Irp->IoStatus.Information;

    (void)IoSetCancelRoutine(NextIrp, NULL);

    UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);

    UnlockRequest(NextIrp, IoGetCurrentIrpStackLocation(NextIrp));

    IoCompleteRequest(NextIrp, IO_NETWORK_INCREMENT);

    FCB->PollState |= AFD_EVENT_SEND;
    FCB->PollStatus[FD_WRITE_BIT] = STATUS_SUCCESS;
    PollReeval(FCB->DeviceExt, FCB->FileObject);

    SocketStateUnlock(FCB);

    return STATUS_SUCCESS;
}

#ifdef _WIN64
static
NTSTATUS
AfdConvert32BitUdpWrite(PIRP Irp,
                        PAFD_SEND_INFO_UDP32 SendReq32,
                        PAFD_SEND_INFO_UDP* pOutSendReq)
{
    PAFD_SEND_INFO_UDP SendReq;
    
    SendReq = ExAllocatePoolWithTag(NonPagedPool,
                                    sizeof(*SendReq),
                                    TAG_AFD_DATA_BUFFER);
    if (!SendReq)
    {
        return STATUS_NO_MEMORY;
    }

    SendReq->BufferCount    = SendReq32->BufferCount;
    SendReq->BufferArray    = UlongToPtr(SendReq32->BufferArray);
    SendReq->AfdFlags       = SendReq32->AfdFlags;
    
    SendReq->TdiRequest.SendDatagramInformation = 
        UlongToPtr(SendReq32->TdiRequest.SendDatagramInformation);
        
    SendReq->TdiRequest.Request.Handle.AddressHandle = 
        UlongToPtr(SendReq32->TdiRequest.Request.Handle.AddressHandle);
    
    SendReq->TdiRequest.Request.RequestNotifyObject = 
        UlongToPtr(SendReq32->TdiRequest.Request.RequestNotifyObject);
        
    SendReq->TdiRequest.Request.RequestContext = 
        UlongToPtr(SendReq32->TdiRequest.Request.RequestContext);
        
    SendReq->TdiRequest.Request.TdiStatus = SendReq32->TdiRequest.Request.TdiStatus;
    
    SendReq->TdiConnection.UserDataLength = SendReq32->TdiConnection.UserDataLength;
    SendReq->TdiConnection.UserData = UlongToPtr(SendReq32->TdiConnection.UserData);
    
    SendReq->TdiConnection.OptionsLength = SendReq32->TdiConnection.OptionsLength;
    SendReq->TdiConnection.Options = UlongToPtr(SendReq32->TdiConnection.Options);
    
    SendReq->TdiConnection.RemoteAddressLength = SendReq32->TdiConnection.RemoteAddressLength;
    SendReq->TdiConnection.RemoteAddress = UlongToPtr(SendReq32->TdiConnection.RemoteAddress);
    
    Irp->Tail.Overlay.DriverContext[0] = SendReq;
    *pOutSendReq = SendReq;
    
    ExFreePoolWithTag(SendReq32, TAG_AFD_DATA_BUFFER);
    return STATUS_SUCCESS;
}
#endif

static
NTSTATUS
AfdConnectedSocketWriteDataUdp(PAFD_FCB FCB,
                               PDEVICE_OBJECT DeviceObject, PIRP Irp,
                               PIO_STACK_LOCATION IrpSp, BOOLEAN Short)
{
    PAFD_SEND_INFO_UDP SendReq;
    PTDI_CONNECTION_INFORMATION TargetAddress;
    KPROCESSOR_MODE LockMode;
    NTSTATUS Status;

    /* Check that the socket is bound */
    if (FCB->SharedData.State != SOCKET_STATE_BOUND || !FCB->RemoteAddress)
    {
        AFD_DbgPrint(MIN_TRACE,("Invalid parameter\n"));
        return UnlockAndMaybeComplete(FCB, 
                                      STATUS_INVALID_PARAMETER, 
                                      Irp,
                                      0);
    }

    if (!(SendReq = LockRequest(Irp, IrpSp, FALSE, &LockMode)))
    {
        return UnlockAndMaybeComplete(FCB, STATUS_NO_MEMORY, Irp, 0);
    }
    
#ifdef _WIN64
    if ((IrpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL ||
         IrpSp->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL) &&
        IoIs32bitProcess(Irp))
    {
        Status = AfdConvert32BitUdpWrite(Irp, (PAFD_SEND_INFO_UDP32)SendReq, &SendReq);
        if (!NT_SUCCESS(Status))
        {
            return UnlockAndMaybeComplete(FCB, Status, Irp, 0);
        }
        
        SendReq->BufferArray = LockBuffers(SendReq->BufferArray,
                                           SendReq->BufferCount,
                                           NULL, NULL,
                                           FALSE, FALSE, LockMode,
                                           TRUE);
    }
    else
#endif
    {
        /* Must lock buffers before handing off user data */
        SendReq->BufferArray = LockBuffers(SendReq->BufferArray,
                                              SendReq->BufferCount,
                                              NULL, NULL,
                                              FALSE, FALSE, LockMode,
                                              FALSE);
    }
    
    if(!SendReq->BufferArray) 
    {
        return UnlockAndMaybeComplete(FCB, 
                                      STATUS_ACCESS_VIOLATION,
                                      Irp, 
                                      0);
    }

    Status = TdiBuildConnectionInfo(&TargetAddress, FCB->RemoteAddress);

    if (!NT_SUCCESS(Status))
    {
        UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
        return UnlockAndMaybeComplete(FCB, Status, Irp, 0);
    }
    
    FCB->PollState &= ~AFD_EVENT_SEND;

    Status = QueueUserModeIrp(FCB, Irp, FUNCTION_SEND);
    if (Status == STATUS_PENDING)
    {
        Status = TdiSendDatagram(&FCB->SendIrp.InFlightRequest,
                                 FCB->AddressFile.Object,
                                 SendReq->BufferArray[0].buf,
                                 SendReq->BufferArray[0].len,
                                 TargetAddress,
                                 PacketSocketSendComplete,
                                 FCB);
                                 
        if (Status != STATUS_PENDING)
        {
            NT_VERIFY(RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]) == &Irp->Tail.Overlay.ListEntry);
            Irp->IoStatus.Status = Status;
            Irp->IoStatus.Information = 0;
            (void)IoSetCancelRoutine(Irp, NULL);
            UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
            UnlockRequest(Irp, IoGetCurrentIrpStackLocation(Irp));
            IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
        }
    }

    ExFreePoolWithTag(TargetAddress, TAG_AFD_TDI_CONNECTION_INFORMATION);

    SocketStateUnlock(FCB);

    return STATUS_PENDING;
}

/*
 * A buffered stream send is about to leave PendingIrpList[FUNCTION_SEND]
 * without SendComplete() having accounted for it -- it is being cancelled,
 * which is what AfdCleanupSocket() does to every pending IRP when the last
 * handle to the socket is closed.
 *
 * Its payload is already in FCB->Send.Window, is counted in
 * FCB->Send.BytesUsed, and is very likely inside an in-flight TdiSend.  TCP has
 * no un-send: those bytes are still going to the peer, and the transport will
 * still report them in Irp->IoStatus.Information.  Dropping the count with the
 * IRP is what left SendComplete() draining the queue empty with bytes still
 * unaccounted for and tripping ASSERT(SendLength == 0).
 *
 * So pass the ownership on rather than discarding it.  The next IRP that holds
 * window bytes owns the stretch immediately after this one, so folding the
 * count into it preserves both the total and the order the drain loop consumes
 * them in -- and it stays correct across IRPs that hold no window bytes at all
 * (queued by LeaveIrpUntilLater()), since those occupy nothing to step over.
 * If nothing follows, the bytes are the tail of the window and go to the FCB.
 *
 * Must be called with the socket lock held and while Irp is still linked.
 */
VOID
AfdReleaseSendWindowOwnership(PAFD_FCB FCB, PIRP Irp)
{
    ULONG_PTR Buffered = (ULONG_PTR)Irp->Tail.Overlay.DriverContext[3];
    PLIST_ENTRY Entry;
    PIRP NextIrp;

    /* Never got window space, so it owns none of it. */
    if (Buffered == 0)
        return;

    Irp->Tail.Overlay.DriverContext[3] = NULL;

    for (Entry = Irp->Tail.Overlay.ListEntry.Flink;
         Entry != &FCB->PendingIrpList[FUNCTION_SEND];
         Entry = Entry->Flink)
    {
        NextIrp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (NextIrp->Tail.Overlay.DriverContext[3] != NULL)
        {
            NextIrp->Tail.Overlay.DriverContext[3] = (PVOID)
                ((ULONG_PTR)NextIrp->Tail.Overlay.DriverContext[3] + Buffered);
            return;
        }
    }

    FCB->SendOrphanedBytes += (UINT)Buffered;
}

NTSTATUS NTAPI
AfdConnectedSocketWriteData(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                            PIO_STACK_LOCATION IrpSp, BOOLEAN Short) {
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_SEND_INFO SendReq;
#ifdef _WIN64
    PAFD_SEND_INFO32 SendReq32 = NULL;
#endif
    UINT TotalBytesCopied = 0, i, SpaceAvail = 0, BytesCopied, SendLength;
    KPROCESSOR_MODE LockMode;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Short);

    AFD_DbgPrint(MID_TRACE,("Called on %p\n", FCB));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    FCB->EventSelectDisabled &= ~AFD_EVENT_SEND;

    /* None of this request is in the send window yet.  Every send IRP that
     * reaches PendingIrpList[FUNCTION_SEND] without having been buffered
     * leaves this NULL -- including every datagram send, which never uses the
     * window at all -- which is how AfdReleaseSendWindowOwnership() tells a
     * buffered send from one that owns no window bytes. */
    Irp->Tail.Overlay.DriverContext[3] = NULL;

    if(FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS)
    {
        return AfdConnectedSocketWriteDataUdp(FCB, DeviceObject, Irp, IrpSp, Short);
    }

    if (FCB->PollState & AFD_EVENT_CLOSE)
    {
        AFD_DbgPrint(MIN_TRACE,("Connection reset by remote peer\n"));

        /* This is an unexpected remote disconnect */
        return UnlockAndMaybeComplete(FCB, FCB->PollStatus[FD_CLOSE_BIT], Irp, 0);
    }

    if (FCB->PollState & AFD_EVENT_ABORT)
    {
        AFD_DbgPrint(MIN_TRACE,("Connection aborted\n"));

        /* This is an abortive socket closure on our side */
        return UnlockAndMaybeComplete(FCB, FCB->PollStatus[FD_CLOSE_BIT], Irp, 0);
    }

    if (FCB->SendClosed)
    {
        AFD_DbgPrint(MIN_TRACE,("No more sends\n"));

        /* This is a graceful send closure */
        return UnlockAndMaybeComplete(FCB, STATUS_FILE_CLOSED, Irp, 0);
    }

    if( !(SendReq = LockRequest( Irp, IrpSp, FALSE, &LockMode )) )
        return UnlockAndMaybeComplete
            ( FCB, STATUS_NO_MEMORY, Irp, 0 );
            
#ifdef _WIN64
    if ((IrpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL ||
         IrpSp->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL) &&
        IoIs32bitProcess(Irp))
    {
        SendReq32 = (PAFD_SEND_INFO32)SendReq;
        SendReq = ExAllocatePoolWithTag(NonPagedPool,
                                        sizeof(*SendReq),
                                        TAG_AFD_DATA_BUFFER);

        if (!SendReq)
        {
            return UnlockAndMaybeComplete(FCB, 
                                          STATUS_NO_MEMORY,
                                          Irp, 0);
        }

        SendReq->BufferCount     = SendReq32->BufferCount;
        SendReq->BufferArray     = UlongToPtr(SendReq32->BufferArray);
        SendReq->AfdFlags        = SendReq32->AfdFlags;
        SendReq->TdiFlags        = SendReq32->TdiFlags;
        
        Irp->Tail.Overlay.DriverContext[0] = SendReq;
        
        ExFreePoolWithTag(SendReq32, TAG_AFD_DATA_BUFFER);
        
        SendReq->BufferArray = LockBuffers(SendReq->BufferArray,
                                           SendReq->BufferCount,
                                           NULL, NULL,
                                           FALSE, FALSE, LockMode,
                                           TRUE);
    }
    else
#endif
    {
        SendReq->BufferArray = LockBuffers(SendReq->BufferArray,
                                           SendReq->BufferCount,
                                           NULL, NULL,
                                           FALSE, FALSE, LockMode,
                                           FALSE);
    }
    
    if( !SendReq->BufferArray ) {
        return UnlockAndMaybeComplete( FCB, STATUS_ACCESS_VIOLATION,
                                       Irp, 0 );
    }

    AFD_DbgPrint(MID_TRACE,("Socket state %u\n", FCB->SharedData.State));

    if( FCB->SharedData.State != SOCKET_STATE_CONNECTED ) {
        AFD_DbgPrint(MID_TRACE,("Socket not connected\n"));
        UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );
        return UnlockAndMaybeComplete( FCB, STATUS_INVALID_CONNECTION, Irp, 0 );
    }

    AFD_DbgPrint(MID_TRACE,("FCB->Send.BytesUsed = %u\n",
                            FCB->Send.BytesUsed));

    SpaceAvail = FCB->Send.Size - FCB->Send.BytesUsed;

    /* Sends have to reach the connection in the order they were issued.  If an
     * earlier send is still waiting for window space, letting this one take
     * space now would put its bytes into the window -- and so onto the wire --
     * ahead of the older request's, and would leave the pending list in an
     * order the send window no longer matches, which SendComplete()'s drain
     * loop reads as an IRP owning no bytes.  Treat the window as full instead,
     * so the checks below apply the caller's own blocking/overlapped policy
     * exactly as they do for a genuinely full window.  A send that never got
     * space leaves DriverContext[3] NULL and can only be at the tail, so the
     * last entry is the one to look at. */
    if (!IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]))
    {
        PIRP LastIrp = CONTAINING_RECORD(FCB->PendingIrpList[FUNCTION_SEND].Blink,
                                         IRP, Tail.Overlay.ListEntry);

        if (LastIrp->Tail.Overlay.DriverContext[3] == NULL)
            SpaceAvail = 0;
    }

    AFD_DbgPrint(MID_TRACE,("We can accept %u bytes\n",
                            SpaceAvail));

    /* Count the total transfer size */
    SendLength = 0;
    for (i = 0; i < SendReq->BufferCount; i++)
    {
        SendLength += SendReq->BufferArray[i].len;
    }

    /* Make sure we've got the space */
    if (SendLength > SpaceAvail)
    {
        /* Blocking sockets have to wait here */
        if (SendLength <= FCB->Send.Size && !((SendReq->AfdFlags & AFD_IMMEDIATE) || (FCB->NonBlocking)))
        {
            FCB->PollState &= ~AFD_EVENT_SEND;
            return LeaveIrpUntilLater(FCB, Irp, FUNCTION_SEND);
        }

        /* Check if we can send anything */
        if (SpaceAvail == 0)
        {
            FCB->PollState &= ~AFD_EVENT_SEND;

            /* Non-overlapped sockets will fail if we can send nothing */
            if (!(SendReq->AfdFlags & AFD_OVERLAPPED))
            {
                UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );
                return UnlockAndMaybeComplete( FCB, STATUS_CANT_WAIT, Irp, 0 );
            }
            else
            {
                /* Overlapped sockets just pend */
                return LeaveIrpUntilLater(FCB, Irp, FUNCTION_SEND);
            }
        }
    }

    for ( i = 0; SpaceAvail > 0 && i < SendReq->BufferCount; i++ )
    {
        BytesCopied = MIN(SendReq->BufferArray[i].len, SpaceAvail);

        AFD_DbgPrint(MID_TRACE,("Copying Buffer %u, %p:%u to %p\n",
                                i,
                                SendReq->BufferArray[i].buf,
                                BytesCopied,
                                FCB->Send.Window + FCB->Send.BytesUsed));

        RtlCopyMemory(FCB->Send.Window + FCB->Send.BytesUsed,
                      SendReq->BufferArray[i].buf,
                      BytesCopied);

        TotalBytesCopied += BytesCopied;
        SpaceAvail -= BytesCopied;
        FCB->Send.BytesUsed += BytesCopied;
    }

    Irp->IoStatus.Information = TotalBytesCopied;

    if( TotalBytesCopied == 0 ) {
        AFD_DbgPrint(MID_TRACE,("Empty send\n"));
        UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );
        return UnlockAndMaybeComplete
        ( FCB, STATUS_SUCCESS, Irp, TotalBytesCopied );
    }

    if (SpaceAvail)
    {
        FCB->PollState |= AFD_EVENT_SEND;
        FCB->PollStatus[FD_WRITE_BIT] = STATUS_SUCCESS;
        PollReeval( FCB->DeviceExt, FCB->FileObject );
    }
    else
    {
        FCB->PollState &= ~AFD_EVENT_SEND;
    }

    /* We use the IRP tail for some temporary storage here.  Bytes orphaned by
     * a cancelled send sit in front of what we just appended, so they are no
     * longer the tail of the window and this IRP takes them over; see
     * AfdReleaseSendWindowOwnership().  Only the accounting moves -- the
     * caller is still told just its own byte count. */
    Irp->Tail.Overlay.DriverContext[3] = (PVOID)
        ((ULONG_PTR)Irp->IoStatus.Information + FCB->SendOrphanedBytes);
    FCB->SendOrphanedBytes = 0;

    Status = QueueUserModeIrp(FCB, Irp, FUNCTION_SEND);
    if (Status == STATUS_PENDING && !FCB->SendIrp.InFlightRequest)
    {
        TdiSend(&FCB->SendIrp.InFlightRequest,
                FCB->Connection.Object,
                0,
                FCB->Send.Window,
                FCB->Send.BytesUsed,
                SendComplete,
                FCB);
    }

    SocketStateUnlock(FCB);

    return STATUS_PENDING;
}

NTSTATUS NTAPI
AfdPacketSocketWriteData(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                         PIO_STACK_LOCATION IrpSp) {
    NTSTATUS Status = STATUS_SUCCESS;
    PTDI_CONNECTION_INFORMATION TargetAddress;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_SEND_INFO_UDP SendReq;
    KPROCESSOR_MODE LockMode;
    BOOLEAN Convert32Bit = FALSE;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called on %p\n", FCB));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    FCB->EventSelectDisabled &= ~AFD_EVENT_SEND;

    /* A datagram send never uses FCB->Send.Window, so it owns no window
     * bytes; say so explicitly, since it shares PendingIrpList[FUNCTION_SEND]
     * with the stream sends that do. */
    Irp->Tail.Overlay.DriverContext[3] = NULL;

    /* Check that the socket is bound */
    if( FCB->SharedData.State != SOCKET_STATE_BOUND &&
        FCB->SharedData.State != SOCKET_STATE_CREATED)
    {
        AFD_DbgPrint(MIN_TRACE,("Invalid socket state\n"));
        return UnlockAndMaybeComplete(FCB, STATUS_INVALID_PARAMETER, Irp, 0);
    }

    if (FCB->SendClosed)
    {
        AFD_DbgPrint(MIN_TRACE,("No more sends\n"));
        return UnlockAndMaybeComplete(FCB, STATUS_FILE_CLOSED, Irp, 0);
    }

    if( !(SendReq = LockRequest( Irp, IrpSp, FALSE, &LockMode )) )
        return UnlockAndMaybeComplete(FCB, STATUS_NO_MEMORY, Irp, 0);

#ifdef _WIN64
    if ((IrpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL ||
         IrpSp->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL) &&
        IoIs32bitProcess(Irp))
    {
        Status = AfdConvert32BitUdpWrite(Irp, (PAFD_SEND_INFO_UDP32)SendReq, &SendReq);
        Convert32Bit = TRUE;

        if (!NT_SUCCESS(Status))
        {
            return UnlockAndMaybeComplete(FCB, Status, Irp, 0);
        }
    }
#endif

    if (FCB->SharedData.State == SOCKET_STATE_CREATED)
    {
        if (FCB->LocalAddress)
        {
            ExFreePoolWithTag(FCB->LocalAddress, TAG_AFD_TRANSPORT_ADDRESS);
        }

        FCB->LocalAddress =
        TaBuildNullTransportAddress( ((PTRANSPORT_ADDRESS)SendReq->TdiConnection.RemoteAddress)->
                                      Address[0].AddressType );

        if( FCB->LocalAddress ) {
            Status = WarmSocketForBind( FCB, AFD_SHARE_WILDCARD );

            if( NT_SUCCESS(Status) )
                FCB->SharedData.State = SOCKET_STATE_BOUND;
            else
                return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
        } else
            return UnlockAndMaybeComplete
            ( FCB, STATUS_NO_MEMORY, Irp, 0 );
    }

    SendReq->BufferArray = LockBuffers( SendReq->BufferArray,
                                        SendReq->BufferCount,
                                        NULL, NULL,
                                        FALSE, FALSE, LockMode,
                                        Convert32Bit );

    if( !SendReq->BufferArray )
        return UnlockAndMaybeComplete( FCB, STATUS_ACCESS_VIOLATION,
                                       Irp, 0 );

    AFD_DbgPrint
        (MID_TRACE,("RemoteAddress #%d Type %u\n",
                    ((PTRANSPORT_ADDRESS)SendReq->TdiConnection.RemoteAddress)->
                    TAAddressCount,
                    ((PTRANSPORT_ADDRESS)SendReq->TdiConnection.RemoteAddress)->
                    Address[0].AddressType));

    Status = TdiBuildConnectionInfo( &TargetAddress,
                            ((PTRANSPORT_ADDRESS)SendReq->TdiConnection.RemoteAddress) );

    /* Check the size of the Address given ... */

    if( NT_SUCCESS(Status) ) {
        FCB->PollState &= ~AFD_EVENT_SEND;

        Status = QueueUserModeIrp(FCB, Irp, FUNCTION_SEND);
        if (Status == STATUS_PENDING)
        {
            if (SendReq->BufferCount > 1)
            {
                AFD_DbgPrint(MIN_TRACE,("WARN: More than one buffer %ld\n", SendReq->BufferCount));
            }
            Status = TdiSendDatagram(&FCB->SendIrp.InFlightRequest,
                                     FCB->AddressFile.Object,
                                     SendReq->BufferArray[0].buf,
                                     SendReq->BufferArray[0].len,
                                     TargetAddress,
                                     PacketSocketSendComplete,
                                     FCB);
            if (Status != STATUS_PENDING)
            {
                NT_VERIFY(RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]) == &Irp->Tail.Overlay.ListEntry);
                Irp->IoStatus.Status = Status;
                Irp->IoStatus.Information = ((Status == STATUS_SUCCESS) ? SendReq->BufferArray[0].len : 0);
                (void)IoSetCancelRoutine(Irp, NULL);
                UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
                UnlockRequest(Irp, IoGetCurrentIrpStackLocation(Irp));
                IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
            }
        }

        ExFreePoolWithTag(TargetAddress, TAG_AFD_TDI_CONNECTION_INFORMATION);

        SocketStateUnlock(FCB);

        return STATUS_PENDING;
    }
    else
    {
        UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
        return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
    }
}
