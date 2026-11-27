/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/listen.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */

#include "afd.h"
#include "ntdef.h"

/*
 * A wait for a connection (IOCTL_AFD_WAIT_FOR_LISTEN, or an AcceptEx --
 * IOCTL_AFD_SUPER_ACCEPT) leaves the listener's queue without a connection:
 * it was cancelled, or the listener closed.  An AcceptEx still holds the
 * accept socket it reserved and the output buffer it locked; give both back.
 * Called with the listener's lock held, once the IRP is off the queue.
 */
VOID
AfdReleasePreAcceptIrp(PIRP Irp)
{
    PFILE_OBJECT NewFileObject = (PFILE_OBJECT)Irp->Tail.Overlay.DriverContext[2];
    PAFD_FCB FCB2;

    if (NewFileObject)
    {
        FCB2 = NewFileObject->FsContext;

        /* Under its lock (the listener's first, as AfdSuperAccept() takes
         * them); a socket whose handle is closed meanwhile stays closed */
        if (SocketAcquireStateLock(FCB2))
        {
            if (FCB2->SharedData.State == SOCKET_STATE_CONNECTING)
                FCB2->SharedData.State = SOCKET_STATE_CREATED;
            SocketStateUnlock(FCB2);
        }

        ObDereferenceObject(NewFileObject);
        Irp->Tail.Overlay.DriverContext[2] = NULL;
    }

    if (Irp->Tail.Overlay.DriverContext[3])
    {
        MmUnlockPages((PMDL)Irp->Tail.Overlay.DriverContext[3]);
        IoFreeMdl((PMDL)Irp->Tail.Overlay.DriverContext[3]);
        Irp->Tail.Overlay.DriverContext[3] = NULL;
    }
}

/*
 * Frees a connection ListenComplete() queued, once it is accepted (its
 * connection object handed to the accept socket) or the listener is
 * destroyed (the object closed).
 */
VOID
AfdFreeQueuedConnection(PAFD_TDI_OBJECT_QELT Qelt)
{
    ExFreePoolWithTag(Qelt->ConnInfo, TAG_AFD_TDI_CONNECTION_INFORMATION);
    ExFreePoolWithTag(Qelt, TAG_AFD_ACCEPT_QUEUE);
}

static NTSTATUS SatisfyAccept(PAFD_DEVICE_EXTENSION DeviceExt,
                              PIRP Irp,
                              PFILE_OBJECT NewFileObject,
                              PAFD_TDI_OBJECT_QELT Qelt,
                              BOOL SuperAccept)
{
    PAFD_FCB FCB = NewFileObject->FsContext;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceExt);

    if (!SocketAcquireStateLock(FCB))
        return LostSocket( Irp );

    /* The accept socket's handle was closed meanwhile (AfdCleanupSocket()):
     * it takes no connection any more.  The connection stays the caller's,
     * for the next accept. */
    if (FCB->SharedData.State == SOCKET_STATE_CLOSED)
    {
        if (SuperAccept)
        {
            SocketStateUnlock(FCB);
            return STATUS_INVALID_HANDLE;
        }

        return UnlockAndMaybeComplete(FCB, STATUS_INVALID_HANDLE, Irp, 0);
    }

    /* Transfer the connection to the new socket, launch the opening read */
    AFD_DbgPrint(MID_TRACE,("Completing a real accept (FCB %p)\n", FCB));

    FCB->Connection = Qelt->Object;
    Qelt->Object.Object = NULL;
    Qelt->Object.Handle = INVALID_HANDLE_VALUE;

    if (FCB->RemoteAddress)
    {
        ExFreePoolWithTag(FCB->RemoteAddress, TAG_AFD_TRANSPORT_ADDRESS);
    }

    FCB->RemoteAddress =
        TaCopyTransportAddress( Qelt->ConnInfo->RemoteAddress );

    if( !FCB->RemoteAddress )
        Status = STATUS_NO_MEMORY;
    else
        Status = MakeSocketIntoConnection(FCB, !SuperAccept);

    if (NT_SUCCESS(Status))
        Status = TdiBuildConnectionInfo(&FCB->ConnectCallInfo, FCB->RemoteAddress);

    if (NT_SUCCESS(Status))
        Status = TdiBuildConnectionInfo(&FCB->ConnectReturnInfo, FCB->RemoteAddress);

    if (SuperAccept)
    {
        SocketStateUnlock(FCB);
        return Status;
    }

    return UnlockAndMaybeComplete(FCB, Status, Irp, 0);
}

static NTSTATUS SatisfyPreAccept( PIRP Irp, PAFD_TDI_OBJECT_QELT Qelt ) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PAFD_RECEIVED_ACCEPT_DATA ListenReceive =
        (PAFD_RECEIVED_ACCEPT_DATA)Irp->AssociatedIrp.SystemBuffer;
    PTA_IP_ADDRESS IPAddr;
    ULONG AnswerLength = FIELD_OFFSET(AFD_RECEIVED_ACCEPT_DATA, Address) +
                         TaLengthOfTransportAddress(Qelt->ConnInfo->RemoteAddress);

    /* The answer is the output (METHOD_BUFFERED): an output too short for it
     * fails the wait, and the connection waits for the next one */
    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < AnswerLength)
    {
        AFD_DbgPrint(MIN_TRACE,("Output of %lu bytes cannot hold %lu\n",
                                IrpSp->Parameters.DeviceIoControl.OutputBufferLength,
                                AnswerLength));
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        (void)IoSetCancelRoutine(Irp, NULL);
        IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
        return STATUS_BUFFER_TOO_SMALL;
    }

    ListenReceive->SequenceNumber = Qelt->Seq;

    AFD_DbgPrint(MID_TRACE,("Giving SEQ %u to userland\n", Qelt->Seq));
    AFD_DbgPrint(MID_TRACE,("Socket Address (K) %p (U) %p\n",
                            &ListenReceive->Address,
                            Qelt->ConnInfo->RemoteAddress));

    TaCopyTransportAddressInPlace( &ListenReceive->Address,
                                   Qelt->ConnInfo->RemoteAddress );

    IPAddr = (PTA_IP_ADDRESS)&ListenReceive->Address;

    AFD_DbgPrint(MID_TRACE,("IPAddr->TAAddressCount %d\n",
                            IPAddr->TAAddressCount));
    AFD_DbgPrint(MID_TRACE,("IPAddr->Address[0].AddressType %u\n",
                            IPAddr->Address[0].AddressType));
    AFD_DbgPrint(MID_TRACE,("IPAddr->Address[0].AddressLength %u\n",
                            IPAddr->Address[0].AddressLength));
    AFD_DbgPrint(MID_TRACE,("IPAddr->Address[0].Address[0].sin_port %x\n",
                            IPAddr->Address[0].Address[0].sin_port));
    AFD_DbgPrint(MID_TRACE,("IPAddr->Address[0].Address[0].sin_addr %x\n",
                            IPAddr->Address[0].Address[0].in_addr));

    if( Irp->MdlAddress ) UnlockRequest( Irp, IrpSp );

    Irp->IoStatus.Information = AnswerLength;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    (void)IoSetCancelRoutine(Irp, NULL);
    IoCompleteRequest( Irp, IO_NETWORK_INCREMENT );
    return STATUS_SUCCESS;
}

/*
 * Cancel routine of an AcceptEx that took its connection and waits on the
 * accept socket for the first data (see SatisfySuperAccept()).  Cancelling
 * that receive completes it, with STATUS_CANCELLED
 * (AcceptExReceiveComplete()).
 */
static DRIVER_CANCEL AfdCancelAcceptExReceive;
static
VOID
NTAPI
AfdCancelAcceptExReceive(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT FileObject = (PFILE_OBJECT)Irp->Tail.Overlay.DriverContext[2];
    PAFD_FCB FCB;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* The IRP cannot complete while we hold the cancel spin lock
     * (AfdCompleteAcceptExIrp() clears the routine under it), and its
     * reference on the accept socket goes when it completes: take one of our
     * own first */
    ASSERT(FileObject != NULL);
    ObReferenceObject(FileObject);
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    FCB = FileObject->FsContext;
    if (SocketAcquireStateLock(FCB))
    {
        /* Unless the data arrived meanwhile */
        if (FCB->AcceptIrp == Irp)
        {
            ASSERT(FCB->ReceiveIrp.InFlightRequest != NULL);
            IoCancelIrp(FCB->ReceiveIrp.InFlightRequest);
        }

        SocketStateUnlock(FCB);
    }

    ObDereferenceObject(FileObject);
}

/*
 * The output buffer of an AcceptEx holds the first data block, then the local
 * address area, then the remote one.  An area holds the address as a sockaddr
 * (the TDI address type, which is the address family, then the address) at
 * its start, and the sockaddr's length as a USHORT in its last two bytes,
 * where WSPGetAcceptExSockaddrs() reads both.  An area of length 0 is left
 * alone; any other must hold the address and 16 bytes more, as AcceptEx
 * documents.
 */
#define AFD_ACCEPTEX_AREA_SPARE 16

static
ULONG
AfdSockaddrLength(PTA_ADDRESS Address)
{
    return FIELD_OFFSET(TA_ADDRESS, Address) - FIELD_OFFSET(TA_ADDRESS, AddressType) +
           Address->AddressLength;
}

static
BOOLEAN
AfdAcceptExAreaFits(ULONG AreaLength, ULONG SockaddrLength)
{
    return AreaLength == 0 || AreaLength >= SockaddrLength + AFD_ACCEPTEX_AREA_SPARE;
}

static
VOID
AfdWriteAcceptExAddress(PUCHAR Area, ULONG AreaLength, PTA_ADDRESS Address)
{
    USHORT Length = (USHORT)AfdSockaddrLength(Address);

    if (!AreaLength)
        return;

    /* AfdAcceptExAreaFits() said so */
    ASSERT(AreaLength >= Length + sizeof(Length));

    RtlCopyMemory(Area, &Address->AddressType, Length);
    RtlCopyMemory(Area + AreaLength - sizeof(Length), &Length, sizeof(Length));
}

static NTSTATUS SatisfySuperAccept(PAFD_FCB FCB, PIRP Irp, PAFD_TDI_OBJECT_QELT Qelt)
{
    PFILE_OBJECT NewFileObject = (PFILE_OBJECT)Irp->Tail.Overlay.DriverContext[2];
    PAFD_SUPER_ACCEPT_INFO AcceptInfo = (PAFD_SUPER_ACCEPT_INFO)Irp->Tail.Overlay.DriverContext[0]; /* LockRequest stores the request in index 0 */
    PAFD_FCB FCB2 = NewFileObject->FsContext;
    NTSTATUS Status;

    Irp->Tail.Overlay.DriverContext[2] = NULL;

    /* Everything below changes the accept socket, which its handle being
     * closed on another thread changes too: hold its lock (the listener's
     * first, as AfdSuperAccept() takes them), and the reference
     * AfdSuperAccept() took on it, until done. */
    if (!SocketAcquireStateLock(FCB2))
    {
        ASSERT(FALSE);
        return STATUS_FILE_CLOSED;
    }

    /* Both addresses are of the listener's family.  An address area too
     * small for one fails the AcceptEx, and the connection waits for the
     * next accept. */
    ULONG SockaddrLength = AfdSockaddrLength(&FCB->LocalAddress->Address[0]);
    if (!AfdAcceptExAreaFits(AcceptInfo->LocalAddressLength, SockaddrLength) ||
        !AfdAcceptExAreaFits(AcceptInfo->RemoteAddressLength, SockaddrLength))
    {
        InsertHeadList(&FCB->PendingConnections, &Qelt->ListEntry);
        Status = STATUS_BUFFER_TOO_SMALL;
        goto end;
    }

    /* Whatever can fail is done before the connection is the accept
     * socket's: a failure fails the AcceptEx, and the connection waits for
     * the next accept, as it does above.  First the output buffer: the first
     * data block, then the local address, then the remote one. */
    BYTE *OutputBuffer = MmGetSystemAddressForMdlSafe((PMDL)Irp->Tail.Overlay.DriverContext[3], NormalPagePriority);
    if (!OutputBuffer)
    {
        InsertHeadList(&FCB->PendingConnections, &Qelt->ListEntry);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    BYTE *BufferPtr = OutputBuffer;

    /* Then the connection's local address, from the transport */
    LONG RequestSize = (FIELD_OFFSET(TDI_ADDRESS_INFO, Address.Address[0].Address) + FCB->LocalAddress->Address[0].AddressLength);
    PTRANSPORT_ADDRESS RemoteAddress = (PTRANSPORT_ADDRESS)Qelt->ConnInfo->RemoteAddress;
    PTDI_ADDRESS_INFO Buffer = ExAllocatePoolWithTag(NonPagedPool, RequestSize, 'bufT');
    if (Buffer == NULL)
    {
        InsertHeadList(&FCB->PendingConnections, &Qelt->ListEntry);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    PMDL Mdl = IoAllocateMdl(Buffer, RequestSize, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        ExFreePool(Buffer);
        InsertHeadList(&FCB->PendingConnections, &Qelt->ListEntry);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    _SEH2_TRY
    {
        /* TCP eventually calls MmUnlockPages and IoFreeMdl*/
        MmProbeAndLockPages(Mdl, KernelMode, IoModifyAccess);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        IoFreeMdl(Mdl);
        ExFreePool(Buffer);
        InsertHeadList(&FCB->PendingConnections, &Qelt->ListEntry);
        goto end;
    }
    _SEH2_END;

    /* TdiQueryInformation() consumes the MDL */
    Status = TdiQueryInformation(Qelt->Object.Object, TDI_QUERY_ADDRESS_INFO, Mdl);
    if (!NT_SUCCESS(Status))
    {
        ExFreePool(Buffer);
        InsertHeadList(&FCB->PendingConnections, &Qelt->ListEntry);
        goto end;
    }

    Status = SatisfyAccept(NULL, Irp, NewFileObject, Qelt, TRUE);

    if (Qelt->Object.Object)
    {
        /* The accept socket is closed and refused it: the connection waits
         * for the next accept */
        ASSERT(FCB2->SharedData.State == SOCKET_STATE_CLOSED);
        ExFreePool(Buffer);
        InsertHeadList(&FCB->PendingConnections, &Qelt->ListEntry);
        goto end;
    }

    /* The accept socket has the connection now, but could not make it its
     * own: the AcceptEx fails, and closing the socket closes the
     * connection */
    if (!NT_SUCCESS(Status))
    {
        ExFreePool(Buffer);
        goto end;
    }

    /* Write the local address, then the remote one, past the data block:
     * AfdSuperAccept() checked that the three fit in the buffer */
    ASSERT(AfdSockaddrLength(Buffer->Address.Address) == SockaddrLength);
    ASSERT(AfdSockaddrLength(&RemoteAddress->Address[0]) == SockaddrLength);
    BufferPtr += AcceptInfo->ReceiveDataLength;
    AfdWriteAcceptExAddress(BufferPtr, AcceptInfo->LocalAddressLength, Buffer->Address.Address);
    ExFreePool(Buffer);

    BufferPtr += AcceptInfo->LocalAddressLength;
    AfdWriteAcceptExAddress(BufferPtr, AcceptInfo->RemoteAddressLength, &RemoteAddress->Address[0]);

    if (AcceptInfo->ReceiveDataLength)
    {
        BOOLEAN Cancelled;
        KIRQL OldIrql;

        /* The IRP completes once the first data is received
         * (AcceptExReceiveComplete()).  Until then it is on no list of the
         * listener's, so the listener's cancel routine cannot find it: it
         * gets one of its own, which cancels that receive.  The IRP keeps the
         * reference on the accept socket for it. */
        FCB2->AcceptIrp = Irp;
        Irp->Tail.Overlay.DriverContext[2] = NewFileObject;

        IoAcquireCancelSpinLock(&OldIrql);
        Cancelled = Irp->Cancel;
        if (!Cancelled)
            (void)IoSetCancelRoutine(Irp, AfdCancelAcceptExReceive);
        IoReleaseCancelSpinLock(OldIrql);

        Status = TdiReceive(&FCB2->ReceiveIrp.InFlightRequest,
                            FCB2->Connection.Object, TDI_RECEIVE_NORMAL,
                            (PCHAR)OutputBuffer,
                            AcceptInfo->ReceiveDataLength,
                            AcceptExReceiveComplete,
                            FCB2);

        /* The receive did not reach the transport, so nothing else completes
         * the IRP */
        if (FCB2->AcceptIrp == Irp && !FCB2->ReceiveIrp.InFlightRequest)
        {
            ASSERT(!NT_SUCCESS(Status));
            if (FCB2->SharedData.State != SOCKET_STATE_CLOSED)
                FCB2->SharedData.State = SOCKET_STATE_CREATED;
            NewFileObject = AfdCompleteAcceptExIrp(FCB2, Status, 0);
            SocketStateUnlock(FCB2);
            ObDereferenceObject(NewFileObject);
            return Status;
        }

        /* Cancelled while it waited for the connection: AfdCancelHandler()
         * found it off the queue already and left it to us */
        if (Cancelled && FCB2->AcceptIrp == Irp)
        {
            ASSERT(FCB2->ReceiveIrp.InFlightRequest != NULL);
            IoCancelIrp(FCB2->ReceiveIrp.InFlightRequest);
        }

        if (Status == STATUS_PENDING)
            Status = STATUS_SUCCESS;

        SocketStateUnlock(FCB2);
        return Status;
    }
    else
    {
        /* Begin receive buffer */
        Status = TdiReceive(
            &FCB2->ReceiveIrp.InFlightRequest,
            FCB2->Connection.Object,
            TDI_RECEIVE_NORMAL,
            FCB2->Recv.Window,
            FCB2->Recv.Size,
            ReceiveComplete,
            FCB2);

        if (Status == STATUS_PENDING)
            Status = STATUS_SUCCESS;
    }

end:
    /* Complete the IRP, there is no data to receive.  A socket whose handle
     * is closed stays closed. */
    if (FCB2->SharedData.State != SOCKET_STATE_CLOSED)
    {
        if (NT_SUCCESS(Status))
        {
            FCB2->SharedData.State = SOCKET_STATE_CONNECTED;
        }
        else
        {
            FCB2->SharedData.State = SOCKET_STATE_CREATED;
        }
    }

    SocketStateUnlock(FCB2);
    ObDereferenceObject(NewFileObject);

    MmUnlockPages((PMDL)Irp->Tail.Overlay.DriverContext[3]);
    IoFreeMdl((PMDL)Irp->Tail.Overlay.DriverContext[3]); 
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = Status;
    if( Irp->MdlAddress ) UnlockRequest( Irp, IoGetCurrentIrpStackLocation( Irp ) );
    (void)IoSetCancelRoutine(Irp, NULL);
    IoCompleteRequest( Irp, IO_NETWORK_INCREMENT );

    return STATUS_SUCCESS;
}

static IO_COMPLETION_ROUTINE ListenComplete;
static NTSTATUS NTAPI ListenComplete( PDEVICE_OBJECT DeviceObject,
                                      PIRP Irp,
                                      PVOID Context ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PAFD_FCB FCB = (PAFD_FCB)Context;
    PAFD_TDI_OBJECT_QELT Qelt;
    PLIST_ENTRY NextIrpEntry;
    PIRP NextIrp;

    UNREFERENCED_PARAMETER(DeviceObject);

    if( !SocketAcquireStateLock( FCB ) )
    {
        /* The listen IRP is allocated and owned by AFD (IoAllocateIrp, not
         * tied to any thread), so AFD must free it on every completion path
         * and stop further IRP processing. */
        IoFreeIrp(Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    ASSERT(FCB->ListenIrp.InFlightRequest == Irp);
    FCB->ListenIrp.InFlightRequest = NULL;

    if( FCB->SharedData.State == SOCKET_STATE_CLOSED ) {
        /* Cleanup our IRP queue because the FCB is being destroyed.  The
         * cleanup cancelled these already; those still here are being
         * cancelled by another thread, whose cancel routine waits for our
         * lock and will not find them: they are ours to complete. */
        while( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_PREACCEPT] ) ) {
           NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_PREACCEPT]);
           NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
           AfdReleasePreAcceptIrp(NextIrp);
           NextIrp->IoStatus.Status = STATUS_FILE_CLOSED;
           NextIrp->IoStatus.Information = 0;
           if( NextIrp->MdlAddress ) UnlockRequest( NextIrp, IoGetCurrentIrpStackLocation( NextIrp ) );
           (void)IoSetCancelRoutine(NextIrp, NULL);
           IoCompleteRequest( NextIrp, IO_NETWORK_INCREMENT );
        }

        /* Free ConnectionReturnInfo and ConnectionCallInfo */
        if (FCB->ListenIrp.ConnectionReturnInfo)
        {
            ExFreePoolWithTag(FCB->ListenIrp.ConnectionReturnInfo,
                              TAG_AFD_TDI_CONNECTION_INFORMATION);

            FCB->ListenIrp.ConnectionReturnInfo = NULL;
        }

        if (FCB->ListenIrp.ConnectionCallInfo)
        {
            ExFreePoolWithTag(FCB->ListenIrp.ConnectionCallInfo,
                              TAG_AFD_TDI_CONNECTION_INFORMATION);

            FCB->ListenIrp.ConnectionCallInfo = NULL;
        }

        AfdUnlockFromCompletion(FCB);
        IoFreeIrp(Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    AFD_DbgPrint(MID_TRACE,("Completing listen request.\n"));
    AFD_DbgPrint(MID_TRACE,("IoStatus was %x\n", Irp->IoStatus.Status));

    if (Irp->IoStatus.Status != STATUS_SUCCESS)
    {
        /* A failed listen completion must not kill the listening socket:
         * Windows keeps listening. Re-arm the TDI listen request, guarding
         * against a hot loop of synchronous failures. */
        if (FCB->SharedData.State == SOCKET_STATE_LISTENING &&
            FCB->ListenRearmFailures < AFD_LISTEN_REARM_MAX_FAILURES)
        {
            FCB->ListenRearmFailures++;

            /* The failed listen did not consume the warm connection, so
             * only open a new one if it is actually missing */
            if (FCB->Connection.Object == NULL)
                Status = WarmSocketForConnection(FCB);
            else
                Status = STATUS_SUCCESS;

            if (NT_SUCCESS(Status))
            {
                Status = TdiBuildNullConnectionInfoInPlace(FCB->ListenIrp.ConnectionCallInfo,
                                                           FCB->LocalAddress->Address[0].AddressType);
                ASSERT(Status == STATUS_SUCCESS);

                Status = TdiBuildNullConnectionInfoInPlace(FCB->ListenIrp.ConnectionReturnInfo,
                                                           FCB->LocalAddress->Address[0].AddressType);
                ASSERT(Status == STATUS_SUCCESS);

                TdiListen( &FCB->ListenIrp.InFlightRequest,
                           FCB->Connection.Object,
                           &FCB->ListenIrp.ConnectionCallInfo,
                           &FCB->ListenIrp.ConnectionReturnInfo,
                           ListenComplete,
                           FCB );
            }
        }

        AfdUnlockFromCompletion(FCB);
        IoFreeIrp(Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    FCB->ListenRearmFailures = 0;

    /* Queue the connection, once everything its queue entry needs is
     * allocated.  If something is not, the connection stays in FCB->Connection
     * and WarmSocketForConnection() below closes it: the connection is lost,
     * not leaked. */
    Qelt = ExAllocatePoolWithTag(NonPagedPool,
                                 sizeof(*Qelt),
                                 TAG_AFD_ACCEPT_QUEUE);
    if (Qelt)
    {
        Status = TdiBuildNullConnectionInfo(&Qelt->ConnInfo,
                                            FCB->LocalAddress->Address[0].AddressType);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Qelt, TAG_AFD_ACCEPT_QUEUE);
            Qelt = NULL;
        }
    }

    if (Qelt)
    {
        Qelt->Object = FCB->Connection;
        Qelt->Seq = FCB->ConnSeq++;
        /* Ownership moved to the accept queue; clear the FCB slot so the
         * next WarmSocketForConnection doesn't release the queued object */
        FCB->Connection.Object = NULL;
        FCB->Connection.Handle = INVALID_HANDLE_VALUE;
        AFD_DbgPrint(MID_TRACE,("Address Type: %u (RA %p)\n",
                                FCB->LocalAddress->Address[0].AddressType,
                                FCB->ListenIrp.
                                ConnectionReturnInfo->RemoteAddress));

        TaCopyTransportAddressInPlace
           ( Qelt->ConnInfo->RemoteAddress,
             FCB->ListenIrp.ConnectionReturnInfo->RemoteAddress );
        InsertTailList( &FCB->PendingConnections, &Qelt->ListEntry );
    }

    /* Satisfy a pre-accept request if one is available */
    if( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_PREACCEPT] ) &&
        !IsListEmpty( &FCB->PendingConnections ) ) {
        PLIST_ENTRY PendingIrp  =
            RemoveHeadList( &FCB->PendingIrpList[FUNCTION_PREACCEPT] );
        PLIST_ENTRY PendingConn = FCB->PendingConnections.Flink;
        PIRP PendingIrpPtr = CONTAINING_RECORD(PendingIrp, IRP, Tail.Overlay.ListEntry);
        PAFD_TDI_OBJECT_QELT ConnectionData = CONTAINING_RECORD( PendingConn, AFD_TDI_OBJECT_QELT, ListEntry);
        if (PendingIrpPtr->Tail.Overlay.DriverContext[2] && PendingIrpPtr->Tail.Overlay.DriverContext[3])
        {
            RemoveEntryList(PendingConn);
            SatisfySuperAccept(FCB, PendingIrpPtr, ConnectionData);

            /* Unless the accept socket refused it (back on the queue) */
            if (!ConnectionData->Object.Object)
                AfdFreeQueuedConnection(ConnectionData);
        }
        else
        {
            SatisfyPreAccept(PendingIrpPtr, ConnectionData);
        }
    }

    /* Launch new accept socket */
    Status = WarmSocketForConnection( FCB );

    if (NT_SUCCESS(Status))
    {
        Status = TdiBuildNullConnectionInfoInPlace(FCB->ListenIrp.ConnectionCallInfo,
                                                   FCB->LocalAddress->Address[0].AddressType);
        ASSERT(Status == STATUS_SUCCESS);

        Status = TdiBuildNullConnectionInfoInPlace(FCB->ListenIrp.ConnectionReturnInfo,
                                                   FCB->LocalAddress->Address[0].AddressType);
        ASSERT(Status == STATUS_SUCCESS);

        Status = TdiListen( &FCB->ListenIrp.InFlightRequest,
                            FCB->Connection.Object,
                            &FCB->ListenIrp.ConnectionCallInfo,
                            &FCB->ListenIrp.ConnectionReturnInfo,
                            ListenComplete,
                            FCB );

        if (Status == STATUS_PENDING)
            Status = STATUS_SUCCESS;
    }

    /* Trigger a select return if appropriate */
    if( !IsListEmpty( &FCB->PendingConnections ) ) {
        FCB->PollState |= AFD_EVENT_ACCEPT;
        FCB->PollStatus[FD_ACCEPT_BIT] = STATUS_SUCCESS;
        PollReeval( FCB->DeviceExt, FCB->FileObject );
    } else
        FCB->PollState &= ~AFD_EVENT_ACCEPT;

    AfdUnlockFromCompletion(FCB);

    /* This listen IRP is owned by AFD (IoAllocateIrp); a fresh one was posted
     * above for the next connection.  Free the completed IRP ourselves and tell
     * the I/O manager we are done with it. */
    IoFreeIrp(Irp);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS AfdListenSocket( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                          PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS LockStatus;
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_LISTEN_DATA ListenReq;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called on %p\n", FCB));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if( !(ListenReq = LockRequest( Irp, IrpSp, FALSE, NULL, &LockStatus )) )
        return UnlockAndMaybeComplete( FCB, LockStatus, Irp,
                                       0 );

    if( FCB->SharedData.State != SOCKET_STATE_BOUND ) {
        Status = STATUS_INVALID_PARAMETER;
        AFD_DbgPrint(MIN_TRACE,("Could not listen an unbound socket\n"));
        return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
    }

    FCB->DelayedAccept = ListenReq->UseDelayedAcceptance;

    AFD_DbgPrint(MID_TRACE,("ADDRESSFILE: %p\n", FCB->AddressFile.Handle));

    Status = WarmSocketForConnection( FCB );

    AFD_DbgPrint(MID_TRACE,("Status from warmsocket %x\n", Status));

    if( !NT_SUCCESS(Status) ) return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );

    Status = TdiBuildNullConnectionInfo
        ( &FCB->ListenIrp.ConnectionCallInfo,
          FCB->LocalAddress->Address[0].AddressType );

    if (!NT_SUCCESS(Status)) return UnlockAndMaybeComplete(FCB, Status, Irp, 0);

    Status = TdiBuildNullConnectionInfo
        ( &FCB->ListenIrp.ConnectionReturnInfo,
          FCB->LocalAddress->Address[0].AddressType );

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(FCB->ListenIrp.ConnectionCallInfo,
                          TAG_AFD_TDI_CONNECTION_INFORMATION);

        FCB->ListenIrp.ConnectionCallInfo = NULL;
        return UnlockAndMaybeComplete(FCB, Status, Irp, 0);
    }

    FCB->SharedData.State = SOCKET_STATE_LISTENING;

    Status = TdiListen( &FCB->ListenIrp.InFlightRequest,
                        FCB->Connection.Object,
                        &FCB->ListenIrp.ConnectionCallInfo,
                        &FCB->ListenIrp.ConnectionReturnInfo,
                        ListenComplete,
                        FCB );

    if( Status == STATUS_PENDING )
        Status = STATUS_SUCCESS;

    AFD_DbgPrint(MID_TRACE,("Returning %x\n", Status));
    return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
}

NTSTATUS AfdWaitForListen( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                           PIO_STACK_LOCATION IrpSp ) {
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called\n"));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if( !IsListEmpty( &FCB->PendingConnections ) ) {
        PLIST_ENTRY PendingConn = FCB->PendingConnections.Flink;

        /* We have a pending connection ... complete this irp right away */
        Status = SatisfyPreAccept
            ( Irp,
              CONTAINING_RECORD
              ( PendingConn, AFD_TDI_OBJECT_QELT, ListEntry ) );

        AFD_DbgPrint(MID_TRACE,("Completed a wait for accept\n"));

        if ( !IsListEmpty( &FCB->PendingConnections ) )
        {
             FCB->PollState |= AFD_EVENT_ACCEPT;
             FCB->PollStatus[FD_ACCEPT_BIT] = STATUS_SUCCESS;
             PollReeval( FCB->DeviceExt, FCB->FileObject );
        } else
             FCB->PollState &= ~AFD_EVENT_ACCEPT;

        SocketStateUnlock( FCB );
        return Status;
    } else if (FCB->NonBlocking) {
        AFD_DbgPrint(MIN_TRACE,("No connection ready on a non-blocking socket\n"));

        return UnlockAndMaybeComplete(FCB, STATUS_CANT_WAIT, Irp, 0);
    } else {
        AFD_DbgPrint(MID_TRACE,("Holding\n"));

        return LeaveIrpUntilLater( FCB, Irp, FUNCTION_PREACCEPT );
    }
}

NTSTATUS AfdAccept( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                    PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_DEVICE_EXTENSION DeviceExt =
        (PAFD_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_ACCEPT_DATA AcceptData = Irp->AssociatedIrp.SystemBuffer;
    PLIST_ENTRY PendingConn;

    AFD_DbgPrint(MID_TRACE,("Called\n"));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    FCB->EventSelectDisabled &= ~AFD_EVENT_ACCEPT;

    for( PendingConn = FCB->PendingConnections.Flink;
         PendingConn != &FCB->PendingConnections;
         PendingConn = PendingConn->Flink ) {
        PAFD_TDI_OBJECT_QELT PendingConnObj =
            CONTAINING_RECORD( PendingConn, AFD_TDI_OBJECT_QELT, ListEntry );

        AFD_DbgPrint(MID_TRACE,("Comparing Seq %u to Q %u\n",
                                AcceptData->SequenceNumber,
                                PendingConnObj->Seq));

        if( PendingConnObj->Seq == AcceptData->SequenceNumber ) {
            PFILE_OBJECT NewFileObject = NULL;

            RemoveEntryList( PendingConn );

            Status = AfdReferenceSocketHandle(AcceptData->ListenHandle,
                                              Irp,
                                              &NewFileObject);

            if( !NT_SUCCESS(Status) )
            {
                /* The connection is still there for another accept */
                InsertHeadList( &FCB->PendingConnections, PendingConn );
                return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
            }

            ASSERT(NewFileObject != FileObject);
            ASSERT(NewFileObject->FsContext != FCB);

            /* We have a pending connection ... complete this irp right away */
            Status = SatisfyAccept( DeviceExt, Irp, NewFileObject, PendingConnObj, FALSE );

            ObDereferenceObject( NewFileObject );

            AFD_DbgPrint(MID_TRACE,("Completed a wait for accept\n"));

            /* The accept socket refused the connection: its handle was closed
             * meanwhile.  It is still there for another accept. */
            if (PendingConnObj->Object.Object)
                InsertHeadList( &FCB->PendingConnections, PendingConn );
            else
                AfdFreeQueuedConnection(PendingConnObj);

            if( !IsListEmpty( &FCB->PendingConnections ) )
            {
                FCB->PollState |= AFD_EVENT_ACCEPT;
                FCB->PollStatus[FD_ACCEPT_BIT] = STATUS_SUCCESS;
                PollReeval( FCB->DeviceExt, FCB->FileObject );
            } else
                FCB->PollState &= ~AFD_EVENT_ACCEPT;

            SocketStateUnlock( FCB );
            return Status;
        }
    }

    AFD_DbgPrint(MIN_TRACE,("No connection waiting\n"));

    return UnlockAndMaybeComplete( FCB, STATUS_UNSUCCESSFUL, Irp, 0 );
}

NTSTATUS AfdSuperAccept( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                    PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS LockStatus;
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB Fcb = FileObject->FsContext;
    PAFD_SUPER_ACCEPT_INFO AcceptRequest;
    ULONG OutputBufferLength;
    PMDL Mdl;

    PFILE_OBJECT NewFileObject = NULL;

    AFD_DbgPrint(MID_TRACE,("Called\n"));

    if( !SocketAcquireStateLock( Fcb ) ) return LostSocket( Irp );

    /* AfdDispatchRequest() checked */
    ASSERT(IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(*AcceptRequest));

    if( !(AcceptRequest = LockRequest( Irp, IrpSp, FALSE, NULL, &LockStatus )) )
        return UnlockAndMaybeComplete( Fcb, LockStatus, Irp,
                                       0 );

    /* The output buffer holds the first data block and both address areas
     * (see SatisfySuperAccept()), at the lengths the caller gives */
    OutputBufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    if (OutputBufferLength == 0 ||
        (ULONGLONG)AcceptRequest->ReceiveDataLength +
        AcceptRequest->LocalAddressLength +
        AcceptRequest->RemoteAddressLength > OutputBufferLength)
    {
        return UnlockAndMaybeComplete(Fcb, STATUS_INVALID_PARAMETER, Irp, 0);
    }

    Fcb->EventSelectDisabled &= ~AFD_EVENT_ACCEPT;

    /* Validate that accepting socket is valid, and has not been bound or connected */
    Status = AfdReferenceSocketHandle(AcceptRequest->AcceptHandle, Irp, &NewFileObject);
    if (!NT_SUCCESS(Status))
        return UnlockAndMaybeComplete(Fcb, Status, Irp, 0);

    PAFD_FCB Fcb2 = NewFileObject->FsContext;

    /* Never fails: AfdReferenceSocketHandle() returned an AFD socket, whose
     * FCB lives as long as its file object */
    (void)SocketAcquireStateLock(Fcb2);

    /* Verify that the acceptor socket has not been already binded/connected or that it has been used by AcceptEx */
    if (Fcb2->SharedData.State != SOCKET_STATE_CREATED)
    {
        SocketStateUnlock(Fcb2);
        ObDereferenceObject(NewFileObject);
        return UnlockAndMaybeComplete(Fcb, STATUS_INVALID_PARAMETER, Irp, 0);
    }

    /* Lock the output buffer (UserBuffer: METHOD_NEITHER), for the accept to
     * write the addresses and the first data to.  Whatever fails here hands
     * back what was taken. */
    Mdl = IoAllocateMdl(Irp->UserBuffer, OutputBufferLength, FALSE, FALSE, NULL);
    if (!Mdl)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        _SEH2_TRY
        {
            MmProbeAndLockPages(Mdl, Irp->RequestorMode, IoWriteAccess);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        if (!NT_SUCCESS(Status))
            IoFreeMdl(Mdl);
    }

    if (!NT_SUCCESS(Status))
    {
        SocketStateUnlock(Fcb2);
        ObDereferenceObject(NewFileObject);
        return UnlockAndMaybeComplete(Fcb, Status, Irp, 0);
    }

    Fcb2->SharedData.State = SOCKET_STATE_CONNECTING;

    Irp->Tail.Overlay.DriverContext[2] = NewFileObject;
    Irp->Tail.Overlay.DriverContext[3] = Mdl;

    SocketStateUnlock(Fcb2);

    /* A connection that arrived before us waits in the backlog: take it now,
     * as accept() would (AfdWaitForListen()).  SatisfySuperAccept() completes
     * the IRP, or leaves it pending on the accept socket until the first
     * data arrives. */
    if (!IsListEmpty(&Fcb->PendingConnections))
    {
        PAFD_TDI_OBJECT_QELT Qelt =
            CONTAINING_RECORD(RemoveHeadList(&Fcb->PendingConnections),
                              AFD_TDI_OBJECT_QELT, ListEntry);

        IoMarkIrpPending(Irp);
        SatisfySuperAccept(Fcb, Irp, Qelt);

        /* Unless the accept socket refused it (back on the queue) */
        if (!Qelt->Object.Object)
            AfdFreeQueuedConnection(Qelt);

        if (!IsListEmpty(&Fcb->PendingConnections))
        {
            Fcb->PollState |= AFD_EVENT_ACCEPT;
            Fcb->PollStatus[FD_ACCEPT_BIT] = STATUS_SUCCESS;
            PollReeval(Fcb->DeviceExt, Fcb->FileObject);
        }
        else
        {
            Fcb->PollState &= ~AFD_EVENT_ACCEPT;
        }

        SocketStateUnlock(Fcb);
        return STATUS_PENDING;
    }

    /* Proceed later in SatisfySuperAccept(), when a connection arrives */
    return LeaveIrpUntilLater(Fcb, Irp, FUNCTION_PREACCEPT);
}

/*
 * WSAAccept's condition function answered CF_DEFER or CF_REJECT for the
 * connection IOCTL_AFD_WAIT_FOR_LISTEN gave it.  A deferred connection stays
 * where it is in the queue, for the next accept; a rejected one is closed.
 */
NTSTATUS AfdDeferAccept( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                         PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS LockStatus;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_DEFER_ACCEPT_DATA DeferData;
    PAFD_TDI_OBJECT_QELT Qelt = NULL;
    PLIST_ENTRY PendingConn;
    NTSTATUS Status = STATUS_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(DeviceObject);

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    /* AfdDispatchRequest() checked */
    ASSERT(IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(*DeferData));

    if( !(DeferData = LockRequest( Irp, IrpSp, FALSE, NULL, &LockStatus )) )
        return UnlockAndMaybeComplete( FCB, LockStatus, Irp, 0 );

    for( PendingConn = FCB->PendingConnections.Flink;
         PendingConn != &FCB->PendingConnections;
         PendingConn = PendingConn->Flink ) {
        PAFD_TDI_OBJECT_QELT PendingConnObj =
            CONTAINING_RECORD( PendingConn, AFD_TDI_OBJECT_QELT, ListEntry );

        if (PendingConnObj->Seq != DeferData->SequenceNumber)
            continue;

        if (DeferData->RejectConnection)
        {
            RemoveEntryList(PendingConn);
            Qelt = PendingConnObj;
        }

        Status = STATUS_SUCCESS;
        break;
    }

    /* A deferred connection is still there to accept, and signals it again */
    FCB->EventSelectDisabled &= ~AFD_EVENT_ACCEPT;
    if( !IsListEmpty( &FCB->PendingConnections ) )
    {
        FCB->PollState |= AFD_EVENT_ACCEPT;
        FCB->PollStatus[FD_ACCEPT_BIT] = STATUS_SUCCESS;
        PollReeval( FCB->DeviceExt, FCB->FileObject );
    } else
        FCB->PollState &= ~AFD_EVENT_ACCEPT;

    SocketStateUnlock( FCB );

    /* Off the queue, the rejected connection is ours alone */
    if (Qelt)
    {
        TdiDisassociateAddressFile(Qelt->Object.Object);
        ObDereferenceObject(Qelt->Object.Object);
        ZwClose(Qelt->Object.Handle);
        AfdFreeQueuedConnection(Qelt);
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    if (Irp->MdlAddress && Irp->Tail.Overlay.DriverContext[0])
        UnlockRequest(Irp, IrpSp);
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    return Status;
}
