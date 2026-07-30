/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        tcpip/ninfo.c
 * PURPOSE:     Network information
 * PROGRAMMERS: Art Yerkes
 * REVISIONS:
 *   CSH 01/08-2000 Created
 */

#include "precomp.h"

#define IP_ROUTE_TYPE_ADD 3
#define IP_ROUTE_TYPE_DEL 2

/* Get IPRouteEntry s for each of the routes in the system */
TDI_STATUS InfoTdiQueryGetRouteTable( PIP_INTERFACE IF, PNDIS_BUFFER Buffer, PUINT BufferSize ) {
    TDI_STATUS Status;
    KIRQL OldIrql;
    UINT RtCount = CountFIBs(IF);
    UINT Size = sizeof( IPROUTE_ENTRY ) * RtCount;
    PFIB_ENTRY RCache, RCacheCur;
    PIPROUTE_ENTRY RouteEntries, RtCurrent;
    UINT i;

    TI_DbgPrint(DEBUG_INFO, ("Called, routes = %d\n",
			    RtCount));

    if (RtCount == 0)
        return InfoCopyOut(NULL, 0, NULL, BufferSize);

    RouteEntries = ExAllocatePoolWithTag( NonPagedPool, Size, ROUTE_ENTRY_TAG );
    RtCurrent = RouteEntries;

    RCache = ExAllocatePoolWithTag( NonPagedPool, sizeof( FIB_ENTRY ) * RtCount, FIB_TAG );
    RCacheCur = RCache;

    if( !RCache || !RouteEntries ) {
	if( RCache ) ExFreePoolWithTag( RCache, FIB_TAG );
	if( RouteEntries ) ExFreePoolWithTag( RouteEntries, ROUTE_ENTRY_TAG );
	return TDI_NO_RESOURCES;
    }

    RtlZeroMemory( RouteEntries, Size );

    RtCount = CopyFIBs( IF, RCache );

    while( RtCurrent < RouteEntries + RtCount ) {
	ASSERT(RCacheCur->Router);

	RtlCopyMemory( &RtCurrent->Dest,
		       &RCacheCur->NetworkAddress.Address,
		       sizeof(RtCurrent->Dest) );
	RtlCopyMemory( &RtCurrent->Mask,
		       &RCacheCur->Netmask.Address,
		       sizeof(RtCurrent->Mask) );
	RtlCopyMemory( &RtCurrent->Gw,
		       &RCacheCur->Router->Address.Address,
		       sizeof(RtCurrent->Gw) );

	RtCurrent->Metric1 = RCacheCur->Metric;
	RtCurrent->Type = TDI_ADDRESS_TYPE_IP;

	TI_DbgPrint
	    (DEBUG_INFO,
	     ("%d: NA %08x NM %08x GW %08x MT %x\n",
	      RtCurrent - RouteEntries,
	      RtCurrent->Dest,
	      RtCurrent->Mask,
	      RtCurrent->Gw,
	      RtCurrent->Metric1 ));

	TcpipAcquireSpinLock(&EntityListLock, &OldIrql);
	for (i = 0; i < EntityCount; i++)
             if (EntityList[i].context == IF)
                 break;

        if (i < EntityCount)
            RtCurrent->Index = EntityList[i].tei_instance;
        else
            RtCurrent->Index = 0;

	TcpipReleaseSpinLock(&EntityListLock, OldIrql);

	RtCurrent++; RCacheCur++;
    }

    Status = InfoCopyOut( (PCHAR)RouteEntries, Size, Buffer, BufferSize );

    ExFreePoolWithTag( RouteEntries, ROUTE_ENTRY_TAG );
    ExFreePoolWithTag( RCache, FIB_TAG );

    TI_DbgPrint(DEBUG_INFO, ("Returning %08x\n", Status));

    return Status;
}

TDI_STATUS InfoTdiQueryGetAddrTable(TDIEntityID ID,
				    PNDIS_BUFFER Buffer,
				    PUINT BufferSize)
{
    KIRQL OldIrql;
    PIPADDR_ENTRY IPEntry;
    PIP_INTERFACE CurrentIF;
    UINT i;

    TI_DbgPrint(DEBUG_INFO, ("Called.\n"));


    TcpipAcquireSpinLock(&EntityListLock, &OldIrql);

    for (i = 0; i < EntityCount; i++)
    {
        if (EntityList[i].tei_entity == ID.tei_entity &&
            EntityList[i].tei_instance == ID.tei_instance)
            break;
    }

    if (i == EntityCount)
    {
        TcpipReleaseSpinLock(&EntityListLock, OldIrql);
        return TDI_INVALID_PARAMETER;
    }

    IPEntry = ExAllocatePoolWithTag(NonPagedPool, sizeof(IPADDR_ENTRY), IP_ADDRESS_TAG);
    if (!IPEntry)
    {
        TcpipReleaseSpinLock(&EntityListLock, OldIrql);
        return TDI_NO_RESOURCES;
    }

    CurrentIF = EntityList[i].context;

    IPEntry->Index = CurrentIF->Index;
    GetInterfaceIPv4Address(CurrentIF,
			    ADE_UNICAST,
			    &IPEntry->Addr);
    GetInterfaceIPv4Address(CurrentIF,
			    ADE_ADDRMASK,
			    &IPEntry->Mask);
    GetInterfaceIPv4Address(CurrentIF,
			    ADE_BROADCAST,
			    &IPEntry->BcastAddr);

    TcpipReleaseSpinLock(&EntityListLock, OldIrql);

    InfoCopyOut((PCHAR)IPEntry, sizeof(IPADDR_ENTRY),
		Buffer, BufferSize);

    ExFreePoolWithTag(IPEntry, IP_ADDRESS_TAG);

    return TDI_SUCCESS;
}

TDI_STATUS InfoTdiQueryGetIPSnmpInfo( TDIEntityID ID,
                                      PIP_INTERFACE IF,
				      PNDIS_BUFFER Buffer,
				      PUINT BufferSize ) {
    IPSNMPInfo SnmpInfo;
    UINT IfCount = CountInterfaces();
    UINT RouteCount = CountFIBs(IF);
    TDI_STATUS Status = TDI_INVALID_REQUEST;

    TI_DbgPrint(DEBUG_INFO, ("Called.\n"));

    RtlZeroMemory(&SnmpInfo, sizeof(SnmpInfo));

    SnmpInfo.ipsi_numif = IfCount;
    SnmpInfo.ipsi_numaddr = 1;
    SnmpInfo.ipsi_numroutes = RouteCount;

    Status = InfoCopyOut( (PCHAR)&SnmpInfo, sizeof(SnmpInfo),
			  Buffer, BufferSize );

    TI_DbgPrint(DEBUG_INFO, ("Returning %08x\n", Status));

    return Status;
}

#define ntohs(n) ((((n) & 0xff) << 8) | (((n) & 0xff00) >> 8))

TDI_STATUS InfoTdiQueryGetConnectionTcpTable(PADDRESS_FILE AddrFile,
				    PNDIS_BUFFER Buffer,
				    PUINT BufferSize,
                    TDI_TCPUDP_CLASS_INFO Class)
{
    SIZE_T Size;
    MIB_TCPROW_OWNER_MODULE TcpRow;
    TDI_STATUS Status = TDI_INVALID_REQUEST;
    PCONNECTION_ENDPOINT Connection;
    PCONNECTION_ENDPOINT *Connections = NULL;
    ULONG ConnCount = 0, RowCount = 0, MaxRows, i;
    BOOLEAN HasListener;
    PCHAR Rows;

    TI_DbgPrint(DEBUG_INFO, ("Called.\n"));

    if (Class == TcpUdpClassOwnerPid)
    {
        Size = sizeof(MIB_TCPROW_OWNER_PID);
    }
    else if (Class == TcpUdpClassOwner)
    {
        Size = sizeof(MIB_TCPROW_OWNER_MODULE);
    }
    else
    {
        Size = sizeof(MIB_TCPROW);
    }

    /* Snapshot the connection chain under the address file lock, taking a
     * reference on every endpoint. The endpoints are queried only after the
     * address file is unlocked again: querying locks the connection, and the
     * associate/listen paths lock the connection first and the address file
     * second, so nesting the two the other way around risks a deadlock. */
    LockObject(AddrFile);

    HasListener = (AddrFile->Listener != NULL);

    for (Connection = AddrFile->Connection;
         Connection != NULL;
         Connection = Connection->Next)
    {
        ConnCount++;
    }

    if (ConnCount != 0)
    {
        Connections = ExAllocatePoolWithTag(NonPagedPool,
                                            ConnCount * sizeof(PCONNECTION_ENDPOINT),
                                            QUERY_CONTEXT_TAG);
        if (!Connections)
        {
            UnlockObject(AddrFile);
            return TDI_NO_RESOURCES;
        }

        i = 0;
        for (Connection = AddrFile->Connection;
             Connection != NULL;
             Connection = Connection->Next)
        {
            ReferenceObject(Connection);
            Connections[i++] = Connection;
        }
    }

    UnlockObject(AddrFile);

    MaxRows = ConnCount + (HasListener ? 1 : 0);
    if (MaxRows == 0)
        return TDI_INVALID_REQUEST;

    Rows = ExAllocatePoolWithTag(NonPagedPool, MaxRows * Size, QUERY_CONTEXT_TAG);
    if (!Rows)
    {
        for (i = 0; i < ConnCount; i++)
            DereferenceObject(Connections[i]);
        if (Connections)
            ExFreePoolWithTag(Connections, QUERY_CONTEXT_TAG);
        return TDI_NO_RESOURCES;
    }

    RtlZeroMemory(&TcpRow, sizeof(TcpRow));
    TcpRow.dwOwningPid = HandleToUlong(AddrFile->ProcessId);
    TcpRow.liCreateTimestamp = AddrFile->CreationTime;
    if (Class == TcpUdpClassOwner)
        TcpRow.OwningModuleInfo[0] = (ULONG_PTR)AddrFile->SubProcessTag;

    if (HasListener)
    {
        TcpRow.dwState = MIB_TCP_STATE_LISTEN;
        TcpRow.dwLocalAddr = AddrFile->Address.Address.IPv4Address;
        TcpRow.dwLocalPort = AddrFile->Port;
        TcpRow.dwRemoteAddr = 0;
        TcpRow.dwRemotePort = 0;

        RtlCopyMemory(Rows + RowCount * Size, &TcpRow, Size);
        RowCount++;
    }

    for (i = 0; i < ConnCount; i++)
    {
        TA_IP_ADDRESS EndPoint;
        NTSTATUS QueryStatus;

        Connection = Connections[i];

        if (Connection->SocketContext == NULL)
            continue;

        QueryStatus = TCPGetSocketStatus(Connection, &TcpRow.dwState);
        if (!NT_SUCCESS(QueryStatus) || TcpRow.dwState == MIB_TCP_STATE_CLOSED)
        {
            /* An endpoint parked on a listener for a future accept */
            continue;
        }

        QueryStatus = TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)&EndPoint, FALSE);
        if (!NT_SUCCESS(QueryStatus))
            continue;
        ASSERT(EndPoint.TAAddressCount >= 1);
        ASSERT(EndPoint.Address[0].AddressLength == TDI_ADDRESS_LENGTH_IP);
        TcpRow.dwLocalAddr = EndPoint.Address[0].Address[0].in_addr;
        TcpRow.dwLocalPort = ntohs(EndPoint.Address[0].Address[0].sin_port);

        QueryStatus = TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)&EndPoint, TRUE);
        if (!NT_SUCCESS(QueryStatus))
            continue;
        ASSERT(EndPoint.TAAddressCount >= 1);
        ASSERT(EndPoint.Address[0].AddressLength == TDI_ADDRESS_LENGTH_IP);
        TcpRow.dwRemoteAddr = EndPoint.Address[0].Address[0].in_addr;
        TcpRow.dwRemotePort = ntohs(EndPoint.Address[0].Address[0].sin_port);

        RtlCopyMemory(Rows + RowCount * Size, &TcpRow, Size);
        RowCount++;
    }

    for (i = 0; i < ConnCount; i++)
        DereferenceObject(Connections[i]);
    if (Connections)
        ExFreePoolWithTag(Connections, QUERY_CONTEXT_TAG);

    if (RowCount != 0)
    {
        Status = InfoCopyOut(Rows, RowCount * Size,
                             Buffer, BufferSize);
    }

    ExFreePoolWithTag(Rows, QUERY_CONTEXT_TAG);

    TI_DbgPrint(DEBUG_INFO, ("Returning %08x\n", Status));

    return Status;
}

TDI_STATUS InfoTdiQueryGetConnectionUdpTable(PADDRESS_FILE AddrFile,
				    PNDIS_BUFFER Buffer,
				    PUINT BufferSize,
				    TDI_TCPUDP_CLASS_INFO Class)
{
    SIZE_T Size;
    MIB_UDPROW_OWNER_MODULE UdpRow;
    TDI_STATUS Status = TDI_INVALID_REQUEST;

    TI_DbgPrint(DEBUG_INFO, ("Called.\n"));

    if (Class == TcpUdpClassOwnerPid)
    {
        Size = sizeof(MIB_UDPROW_OWNER_PID);
    }
    else if (Class == TcpUdpClassOwner)
    {
        Size = sizeof(MIB_UDPROW_OWNER_MODULE);
    }
    else
    {
        Size = sizeof(MIB_UDPROW);
    }

    UdpRow.dwLocalAddr = AddrFile->Address.Address.IPv4Address;
    UdpRow.dwLocalPort = AddrFile->Port;
    UdpRow.dwOwningPid = HandleToUlong(AddrFile->ProcessId);
    UdpRow.liCreateTimestamp = AddrFile->CreationTime;
    UdpRow.dwFlags = 0; /* FIXME */
    if (Class == TcpUdpClassOwner)
    {
        RtlZeroMemory(&UdpRow.OwningModuleInfo[0], sizeof(UdpRow.OwningModuleInfo));
        UdpRow.OwningModuleInfo[0] = (ULONG_PTR)AddrFile->SubProcessTag;
    }

    Status = InfoCopyOut( (PCHAR)&UdpRow,
			  Size, Buffer, BufferSize );

    TI_DbgPrint(DEBUG_INFO, ("Returning %08x\n", Status));

    return Status;
}

TDI_STATUS InfoTdiSetRoute(PIP_INTERFACE IF, PVOID Buffer, UINT BufferSize)
{
    IP_ADDRESS Address, Netmask, Router;
    PIPROUTE_ENTRY Route = Buffer;

    if (!Buffer || BufferSize < sizeof(IPROUTE_ENTRY))
        return TDI_INVALID_PARAMETER;

    AddrInitIPv4( &Address, Route->Dest );
    AddrInitIPv4( &Netmask, Route->Mask );
    AddrInitIPv4( &Router,  Route->Gw );

    if (IF == Loopback)
    {
        DbgPrint("Failing attempt to add route to loopback adapter\n");
        return TDI_INVALID_PARAMETER;
    }

    if( Route->Type == IP_ROUTE_TYPE_ADD ) { /* Add the route */
        TI_DbgPrint(DEBUG_INFO,("Adding route (%s)\n", A2S(&Address)));
        IF->Metric = Route->Metric1;
	if (!RouterCreateRoute(&Address, &Netmask, &Router, IF))
	    return TDI_NO_RESOURCES;

        return TDI_SUCCESS;
     } else if( Route->Type == IP_ROUTE_TYPE_DEL ) {
	TI_DbgPrint(DEBUG_INFO,("Removing route (%s)\n", A2S(&Address)));
	if (NT_SUCCESS(RouterRemoveRoute( &Address, &Router )))
            return TDI_SUCCESS;
        else
            return TDI_INVALID_PARAMETER;
     }

     return TDI_INVALID_REQUEST;
}
