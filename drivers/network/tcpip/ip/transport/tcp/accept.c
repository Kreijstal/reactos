/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        transport/tcp/accept.c
 * PURPOSE:     Transmission Control Protocol Listen/Accept code
 * PROGRAMMERS: Art Yerkes (arty@users.sf.net)
 * REVISIONS:
 *   arty 12/21/2004 Created
 */

#include "precomp.h"

#include <lwip_glue/lwip_glue.h>

extern NPAGED_LOOKASIDE_LIST TdiBucketLookasideList;

NTSTATUS TCPCheckPeerForAccept(PVOID Context,
                               PTDI_REQUEST_KERNEL Request)
{
    struct tcp_pcb *newpcb = (struct tcp_pcb*)Context;
    NTSTATUS Status;
    PTDI_CONNECTION_INFORMATION WhoIsConnecting;
    PTA_IP_ADDRESS RemoteAddress;
    ip_addr_t ipaddr;

    if (Request->RequestFlags & TDI_QUERY_ACCEPT)
        DbgPrint("TDI_QUERY_ACCEPT NOT SUPPORTED!!!\n");

    WhoIsConnecting = (PTDI_CONNECTION_INFORMATION)Request->ReturnConnectionInformation;
    RemoteAddress = (PTA_IP_ADDRESS)WhoIsConnecting->RemoteAddress;

    u16_t peerport = 0;

    RemoteAddress->TAAddressCount = 1;
    RemoteAddress->Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    RemoteAddress->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;

    Status = TCPTranslateError(LibTCPGetPeerName(newpcb,
                                                 &ipaddr,
                                                 &peerport));

    /* lwIP keeps the port in host byte order; TDI addresses are network order */
    RemoteAddress->Address[0].Address[0].sin_port = lwip_htons(peerport);
    RemoteAddress->Address[0].Address[0].in_addr = ipaddr.addr;

    return Status;
}

/* This listen is on a socket we keep as internal.  That socket has the same
 * lifetime as the address file */
NTSTATUS TCPListen(PCONNECTION_ENDPOINT Connection, ULONG Backlog)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ip_addr_t AddressToBind;
    TA_IP_ADDRESS LocalAddress;

    ASSERT(Connection);

    LockObject(Connection);

    ASSERT_KM_POINTER(Connection->AddressFile);

    TI_DbgPrint(DEBUG_TCP,("[IP, TCPListen] Called\n"));

    TI_DbgPrint(DEBUG_TCP, ("Connection->SocketContext %x\n",
        Connection->SocketContext));

    AddressToBind.addr = Connection->AddressFile->Address.Address.IPv4Address;

    Status = TCPTranslateError(LibTCPBind(Connection,
                                          &AddressToBind,
                                          Connection->AddressFile->Port));

    if (NT_SUCCESS(Status))
    {
        /* Check if we had an unspecified port */
        if (!Connection->AddressFile->Port)
        {
            /* We did, so we need to copy back the port */
            Status = TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)&LocalAddress, FALSE);
            if (NT_SUCCESS(Status))
            {
                /* Allocate the port in the port bitmap */
                UINT AllocatedPort = TCPAllocatePort(LocalAddress.Address[0].Address[0].sin_port);
                /* This should never fail unless all ports are in use */
                if (AllocatedPort == (UINT) -1)
                {
                    DbgPrint("ERR: No more ports available.\n");
                    UnlockObject(Connection);
                    return STATUS_TOO_MANY_ADDRESSES;
                }
                Connection->AddressFile->Port = AllocatedPort;
            }
        }
    }

    if (NT_SUCCESS(Status))
    {
        /* Full TDI-side backlog, bounding the pending-accept queue.
         * The lwIP-side listen backlog is clamped to 255 in LibTCPListen. */
        Connection->ListenBacklog = Backlog;

        Connection->SocketContext = LibTCPListen(Connection, Backlog);
        if (!Connection->SocketContext)
            Status = STATUS_UNSUCCESSFUL;
    }

    UnlockObject(Connection);

    TI_DbgPrint(DEBUG_TCP,("[IP, TCPListen] Leaving. Status = %x\n", Status));

    return Status;
}

BOOLEAN TCPAbortListenForSocket
(   PCONNECTION_ENDPOINT Listener,
    PCONNECTION_ENDPOINT Connection)
{
    PLIST_ENTRY ListEntry;
    PTDI_BUCKET Bucket;
    BOOLEAN Found = FALSE;

    LockObject(Listener);

    ListEntry = Listener->ListenRequest.Flink;
    while (ListEntry != &Listener->ListenRequest)
    {
        Bucket = CONTAINING_RECORD(ListEntry, TDI_BUCKET, Entry);

        if (Bucket->AssociatedEndpoint == Connection)
        {
            DereferenceObject(Bucket->AssociatedEndpoint);
            RemoveEntryList( &Bucket->Entry );
            ExFreeToNPagedLookasideList(&TdiBucketLookasideList, Bucket);
            Found = TRUE;
            break;
        }

        ListEntry = ListEntry->Flink;
    }

    UnlockObject(Listener);

    return Found;
}

NTSTATUS TCPAccept ( PTDI_REQUEST Request,
                     PCONNECTION_ENDPOINT Listener,
                     PCONNECTION_ENDPOINT Connection,
                     PTCP_COMPLETION_ROUTINE Complete,
                     PVOID Context )
{
    NTSTATUS Status;
    PTDI_BUCKET Bucket;
    BOOLEAN DispatchPendingAccept = FALSE;

    LockObject(Listener);

    Bucket = ExAllocateFromNPagedLookasideList(&TdiBucketLookasideList);

    if (Bucket)
    {
        Bucket->AssociatedEndpoint = Connection;
        ReferenceObject(Bucket->AssociatedEndpoint);

        Bucket->Request.RequestNotifyObject = Complete;
        Bucket->Request.RequestContext = Context;
        InsertTailList( &Listener->ListenRequest, &Bucket->Entry );
        Status = STATUS_PENDING;

        DispatchPendingAccept = !IsListEmpty(&Listener->PendingAcceptPcbs);
    }
    else
        Status = STATUS_NO_MEMORY;

    UnlockObject(Listener);

    if (DispatchPendingAccept)
    {
        /* A connection that completed its handshake while no listen bucket
         * was queued is waiting: hand it to the new bucket. The PCB may only
         * be touched from the tcpip-thread context, so post a callback
         * instead of dispatching it here. */
        LibTCPDispatchPendingAccept(Listener);
    }

    return Status;
}
