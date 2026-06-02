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

static NTSTATUS SatisfyAccept( PAFD_DEVICE_EXTENSION DeviceExt,
                               PIRP Irp,
                               PFILE_OBJECT NewFileObject,
                               PAFD_TDI_OBJECT_QELT Qelt ) {
    PAFD_FCB FCB = NewFileObject->FsContext;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceExt);

    if( !SocketAcquireStateLock( FCB ) )
        return LostSocket( Irp );

    /* Transfer the connection to the new socket, launch the opening read */
    AFD_DbgPrint(MID_TRACE,("Completing a real accept (FCB %p)\n", FCB));

    FCB->Connection = Qelt->Object;

    if (FCB->RemoteAddress)
    {
        ExFreePoolWithTag(FCB->RemoteAddress, TAG_AFD_TRANSPORT_ADDRESS);
    }

    FCB->RemoteAddress =
        TaCopyTransportAddress( Qelt->ConnInfo->RemoteAddress );

    if( !FCB->RemoteAddress )
        Status = STATUS_NO_MEMORY;
    else
        Status = MakeSocketIntoConnection( FCB );

    if (NT_SUCCESS(Status))
        Status = TdiBuildConnectionInfo(&FCB->ConnectCallInfo, FCB->RemoteAddress);

    if (NT_SUCCESS(Status))
        Status = TdiBuildConnectionInfo(&FCB->ConnectReturnInfo, FCB->RemoteAddress);

    return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
}

static NTSTATUS SatisfyPreAccept( PIRP Irp, PAFD_TDI_OBJECT_QELT Qelt ) {
    PAFD_RECEIVED_ACCEPT_DATA ListenReceive =
        (PAFD_RECEIVED_ACCEPT_DATA)Irp->AssociatedIrp.SystemBuffer;
    PTA_IP_ADDRESS IPAddr;

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

    if( Irp->MdlAddress ) UnlockRequest( Irp, IoGetCurrentIrpStackLocation( Irp ) );

    Irp->IoStatus.Information = ((PCHAR)&IPAddr[1]) - ((PCHAR)ListenReceive);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    (void)IoSetCancelRoutine(Irp, NULL);
    IoCompleteRequest( Irp, IO_NETWORK_INCREMENT );
    return STATUS_SUCCESS;
}

static NTSTATUS SatisfySuperAccept( PAFD_DEVICE_EXTENSION DeviceExt,
                                    PAFD_FCB ListenFCB,
                                    PIRP Irp,
                                    PIO_STACK_LOCATION IrpSp );

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
        return STATUS_FILE_CLOSED;

    ASSERT(FCB->ListenIrp.InFlightRequest == Irp);
    FCB->ListenIrp.InFlightRequest = NULL;

    if( FCB->SharedData.State == SOCKET_STATE_CLOSED ) {
        /* Cleanup our IRP queue because the FCB is being destroyed */
        while( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_PREACCEPT] ) ) {
           NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_PREACCEPT]);
           NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
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

        SocketStateUnlock( FCB );
        return STATUS_FILE_CLOSED;
    }

    AFD_DbgPrint(MID_TRACE,("Completing listen request.\n"));
    AFD_DbgPrint(MID_TRACE,("IoStatus was %x\n", Irp->IoStatus.Status));

    if (Irp->IoStatus.Status != STATUS_SUCCESS)
    {
        SocketStateUnlock(FCB);
        return Irp->IoStatus.Status;
    }

    Qelt = ExAllocatePoolWithTag(NonPagedPool,
                                 sizeof(*Qelt),
                                 TAG_AFD_ACCEPT_QUEUE);

    if( !Qelt ) {
        Status = STATUS_NO_MEMORY;
    } else {
        UINT AddressType =
            FCB->LocalAddress->Address[0].AddressType;

        Qelt->Object = FCB->Connection;
        Qelt->Seq = FCB->ConnSeq++;
        AFD_DbgPrint(MID_TRACE,("Address Type: %u (RA %p)\n",
                                AddressType,
                                FCB->ListenIrp.
                                ConnectionReturnInfo->RemoteAddress));

        Status = TdiBuildNullConnectionInfo( &Qelt->ConnInfo, AddressType );
        if( NT_SUCCESS(Status) ) {
            TaCopyTransportAddressInPlace
               ( Qelt->ConnInfo->RemoteAddress,
                 FCB->ListenIrp.ConnectionReturnInfo->RemoteAddress );
            InsertTailList( &FCB->PendingConnections, &Qelt->ListEntry );
        }
    }

    /* Satisfy a super-accept (AcceptEx) request if one is available */
    if( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_SUPERACCEPT] ) &&
        !IsListEmpty( &FCB->PendingConnections ) ) {
        PLIST_ENTRY PendingIrpEntry =
            RemoveHeadList( &FCB->PendingIrpList[FUNCTION_SUPERACCEPT] );
        PIRP SuperAcceptIrp =
            CONTAINING_RECORD( PendingIrpEntry, IRP, Tail.Overlay.ListEntry );
        SatisfySuperAccept( FCB->DeviceExt, FCB, SuperAcceptIrp,
                            IoGetCurrentIrpStackLocation(SuperAcceptIrp) );
    }
    /* Satisfy a pre-accept request if one is available */
    else if( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_PREACCEPT] ) &&
        !IsListEmpty( &FCB->PendingConnections ) ) {
        PLIST_ENTRY PendingIrp  =
            RemoveHeadList( &FCB->PendingIrpList[FUNCTION_PREACCEPT] );
        PLIST_ENTRY PendingConn = FCB->PendingConnections.Flink;
        SatisfyPreAccept
            ( CONTAINING_RECORD( PendingIrp, IRP,
                                 Tail.Overlay.ListEntry ),
              CONTAINING_RECORD( PendingConn, AFD_TDI_OBJECT_QELT,
                                 ListEntry ) );
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

    SocketStateUnlock( FCB );

    return Status;
}

NTSTATUS AfdListenSocket( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                          PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_LISTEN_DATA ListenReq;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called on %p\n", FCB));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if( !(ListenReq = LockRequest( Irp, IrpSp, FALSE, NULL )) )
        return UnlockAndMaybeComplete( FCB, STATUS_NO_MEMORY, Irp,
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

            Status = ObReferenceObjectByHandle
                ( AcceptData->ListenHandle,
                  FILE_ALL_ACCESS,
                  NULL,
                  KernelMode,
                  (PVOID *)&NewFileObject,
                  NULL );

            if( !NT_SUCCESS(Status) ) return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );

            ASSERT(NewFileObject != FileObject);
            ASSERT(NewFileObject->FsContext != FCB);

            /* We have a pending connection ... complete this irp right away */
            Status = SatisfyAccept( DeviceExt, Irp, NewFileObject, PendingConnObj );

            ObDereferenceObject( NewFileObject );

            AFD_DbgPrint(MID_TRACE,("Completed a wait for accept\n"));

            ExFreePoolWithTag(PendingConnObj, TAG_AFD_ACCEPT_QUEUE);

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

static NTSTATUS SatisfySuperAccept( PAFD_DEVICE_EXTENSION DeviceExt,
                                    PAFD_FCB ListenFCB,
                                    PIRP Irp,
                                    PIO_STACK_LOCATION IrpSp ) {
    PAFD_SUPER_ACCEPT_DATA AcceptInfo;
    PAFD_TDI_OBJECT_QELT PendingConnObj;
    PLIST_ENTRY PendingConn;
    PFILE_OBJECT AcceptFileObject = NULL;
    PAFD_FCB AcceptFCB;
    NTSTATUS Status;
    PCHAR OutputBuffer;
    ULONG OutputLength;
    PTRANSPORT_ADDRESS RemoteAddr;
    PTRANSPORT_ADDRESS LocalAddr;
    ULONG AddrLen;
    HANDLE AcceptHandle;
    ULONG ReceiveDataLength;
    ULONG LocalAddressLength;
    ULONG RemoteAddressLength;

    UNREFERENCED_PARAMETER(DeviceExt);

    AcceptInfo = (PAFD_SUPER_ACCEPT_DATA)Irp->AssociatedIrp.SystemBuffer;
    OutputBuffer = (PCHAR)Irp->AssociatedIrp.SystemBuffer;
    OutputLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

    /* METHOD_BUFFERED: input and output share SystemBuffer, so capture
     * the input fields into locals BEFORE writing output */
    AcceptHandle = AcceptInfo->AcceptHandle;
    ReceiveDataLength = AcceptInfo->ReceiveDataLength;
    LocalAddressLength = AcceptInfo->LocalAddressLength;
    RemoteAddressLength = AcceptInfo->RemoteAddressLength;

    /* Get the first pending connection */
    if (IsListEmpty(&ListenFCB->PendingConnections))
    {
        AFD_DbgPrint(MIN_TRACE,("SuperAccept: no pending connection\n"));
        return STATUS_UNSUCCESSFUL;
    }

    PendingConn = RemoveHeadList(&ListenFCB->PendingConnections);
    PendingConnObj = CONTAINING_RECORD(PendingConn, AFD_TDI_OBJECT_QELT, ListEntry);

    /* The accept socket file object was resolved in AfdSuperAccept's caller
     * context and stashed here.  We cannot resolve the user-mode handle
     * now: when the IRP was pre-posted (libuv/AcceptEx pattern), this
     * routine runs from ListenComplete in arbitrary thread context, not
     * the requesting process. */
    AcceptFileObject = (PFILE_OBJECT)Irp->Tail.Overlay.DriverContext[0];
    Irp->Tail.Overlay.DriverContext[0] = NULL;
    if (!AcceptFileObject)
    {
        AFD_DbgPrint(MIN_TRACE,("SuperAccept: missing prereferenced accept object (handle=%p)\n",
                                AcceptHandle));
        InsertHeadList(&ListenFCB->PendingConnections, &PendingConnObj->ListEntry);
        return STATUS_INVALID_HANDLE;
    }

    AcceptFCB = AcceptFileObject->FsContext;

    /* Transfer TDI connection to accept socket */
    if (!SocketAcquireStateLock(AcceptFCB))
    {
        ObDereferenceObject(AcceptFileObject);
        InsertHeadList(&ListenFCB->PendingConnections, &PendingConnObj->ListEntry);
        return STATUS_FILE_CLOSED;
    }

    AcceptFCB->Connection = PendingConnObj->Object;

    if (AcceptFCB->RemoteAddress)
    {
        ExFreePoolWithTag(AcceptFCB->RemoteAddress, TAG_AFD_TRANSPORT_ADDRESS);
    }

    AcceptFCB->RemoteAddress =
        TaCopyTransportAddress(PendingConnObj->ConnInfo->RemoteAddress);

    if (!AcceptFCB->RemoteAddress)
        Status = STATUS_NO_MEMORY;
    else
        Status = MakeSocketIntoConnection(AcceptFCB);

    if (NT_SUCCESS(Status))
        Status = TdiBuildConnectionInfo(&AcceptFCB->ConnectCallInfo,
                                        AcceptFCB->RemoteAddress);

    if (NT_SUCCESS(Status))
        Status = TdiBuildConnectionInfo(&AcceptFCB->ConnectReturnInfo,
                                        AcceptFCB->RemoteAddress);

    SocketStateUnlock(AcceptFCB);

    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(AcceptFileObject);
        ExFreePoolWithTag(PendingConnObj, TAG_AFD_ACCEPT_QUEUE);
        return Status;
    }

    /* Fill output buffer with addresses in AcceptEx format:
     * [ReceiveDataLength bytes of data (usually 0)]
     * [LocalAddressLength bytes: 4-byte len + SOCKADDR]
     * [RemoteAddressLength bytes: 4-byte len + SOCKADDR]
     */
    if (OutputBuffer && OutputLength >= ReceiveDataLength +
                                        LocalAddressLength +
                                        RemoteAddressLength)
    {
        PCHAR LocalSlot = OutputBuffer + ReceiveDataLength;
        PCHAR RemoteSlot = LocalSlot + LocalAddressLength;

        RtlZeroMemory(LocalSlot, LocalAddressLength);
        RtlZeroMemory(RemoteSlot, RemoteAddressLength);

        /* Local address from the listen socket */
        LocalAddr = ListenFCB->LocalAddress;
        if (LocalAddr && LocalAddr->TAAddressCount > 0)
        {
            AddrLen = sizeof(USHORT) + LocalAddr->Address[0].AddressLength;
            if (AddrLen + sizeof(INT) <= LocalAddressLength)
            {
                *(INT *)LocalSlot = AddrLen;
                *(USHORT *)(LocalSlot + sizeof(INT)) =
                    LocalAddr->Address[0].AddressType;
                RtlCopyMemory(LocalSlot + sizeof(INT) + sizeof(USHORT),
                              LocalAddr->Address[0].Address,
                              LocalAddr->Address[0].AddressLength);
            }
        }

        /* Remote address from the accepted connection */
        RemoteAddr = PendingConnObj->ConnInfo->RemoteAddress;
        if (RemoteAddr && RemoteAddr->TAAddressCount > 0)
        {
            AddrLen = sizeof(USHORT) + RemoteAddr->Address[0].AddressLength;
            if (AddrLen + sizeof(INT) <= RemoteAddressLength)
            {
                *(INT *)RemoteSlot = AddrLen;
                *(USHORT *)(RemoteSlot + sizeof(INT)) =
                    RemoteAddr->Address[0].AddressType;
                RtlCopyMemory(RemoteSlot + sizeof(INT) + sizeof(USHORT),
                              RemoteAddr->Address[0].Address,
                              RemoteAddr->Address[0].AddressLength);
            }
        }
    }

    ObDereferenceObject(AcceptFileObject);
    ExFreePoolWithTag(PendingConnObj, TAG_AFD_ACCEPT_QUEUE);

    /* Update listen socket poll state */
    if (!IsListEmpty(&ListenFCB->PendingConnections))
    {
        ListenFCB->PollState |= AFD_EVENT_ACCEPT;
        ListenFCB->PollStatus[FD_ACCEPT_BIT] = STATUS_SUCCESS;
        PollReeval(ListenFCB->DeviceExt, ListenFCB->FileObject);
    }
    else
    {
        ListenFCB->PollState &= ~AFD_EVENT_ACCEPT;
    }

    Irp->IoStatus.Information = ReceiveDataLength +
                                 LocalAddressLength +
                                 RemoteAddressLength;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    (void)IoSetCancelRoutine(Irp, NULL);
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS AfdSuperAccept( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                         PIO_STACK_LOCATION IrpSp ) {
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_DEVICE_EXTENSION DeviceExt =
        (PAFD_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_SUPER_ACCEPT_DATA AcceptInfo;
    PFILE_OBJECT AcceptFileObject = NULL;
    ULONG InputLength;
    NTSTATUS Status;

    AFD_DbgPrint(MID_TRACE,("AfdSuperAccept called\n"));

    if (!SocketAcquireStateLock(FCB)) return LostSocket(Irp);

    if (FCB->SharedData.State != SOCKET_STATE_LISTENING)
    {
        AFD_DbgPrint(MIN_TRACE,("SuperAccept on non-listening socket\n"));
        return UnlockAndMaybeComplete(FCB, STATUS_INVALID_PARAMETER, Irp, 0);
    }

    InputLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    if (InputLength < sizeof(AFD_SUPER_ACCEPT_DATA))
    {
        return UnlockAndMaybeComplete(FCB, STATUS_BUFFER_TOO_SMALL, Irp, 0);
    }

    /* Resolve the accept socket handle NOW, in the caller's process
     * context.  If the IRP pends, SatisfySuperAccept will run from
     * ListenComplete in an arbitrary thread context where the user
     * handle would not resolve. */
    AcceptInfo = (PAFD_SUPER_ACCEPT_DATA)Irp->AssociatedIrp.SystemBuffer;
    Status = ObReferenceObjectByHandle(AcceptInfo->AcceptHandle,
                                       0, /* no access required; AFD doesn't
                                           * act on the handle rights */
                                       *IoFileObjectType,
                                       Irp->RequestorMode,
                                       (PVOID *)&AcceptFileObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        AFD_DbgPrint(MIN_TRACE,("SuperAccept: bad accept handle %p err=0x%08x\n",
                                AcceptInfo->AcceptHandle, Status));
        return UnlockAndMaybeComplete(FCB, Status, Irp, 0);
    }
    Irp->Tail.Overlay.DriverContext[0] = AcceptFileObject;

    /* Check if there's already a pending connection */
    if (!IsListEmpty(&FCB->PendingConnections))
    {
        Status = SatisfySuperAccept(DeviceExt, FCB, Irp, IrpSp);
        SocketStateUnlock(FCB);
        return Status;
    }

    /* No pending connection - pend the IRP for later completion */
    AFD_DbgPrint(MID_TRACE,("SuperAccept: pending IRP\n"));
    return LeaveIrpUntilLater(FCB, Irp, FUNCTION_SUPERACCEPT);
}
