/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Ancillary Function Driver DLL
 * FILE:        dll/win32/msafd/misc/stubs.c
 * PURPOSE:     Stubs
 * PROGRAMMERS: Casper S. Hornstrup (chorns@users.sourceforge.net)
 * REVISIONS:
 *   CSH 01/09-2000 Created
 */

#include <msafd.h>

INT
WSPAPI
WSPCancelBlockingCall(
    OUT LPINT lpErrno)
{
    UNIMPLEMENTED;

    return 0;
}


BOOL
WSPAPI
WSPGetQOSByName(
    IN      SOCKET s,
    IN OUT  LPWSABUF lpQOSName,
    OUT     LPQOS lpQOS,
    OUT     LPINT lpErrno)
{
    UNIMPLEMENTED;

    return FALSE;
}


SOCKET
WSPAPI
WSPJoinLeaf(
    IN  SOCKET s,
    IN  CONST SOCKADDR *name,
    IN  INT namelen,
    IN  LPWSABUF lpCallerData,
    OUT LPWSABUF lpCalleeData,
    IN  LPQOS lpSQOS,
    IN  LPQOS lpGQOS,
    IN  DWORD dwFlags,
    OUT LPINT lpErrno)
{
    UNIMPLEMENTED;

    return (SOCKET)0;
}

BOOL
WSPAPI
WSPAcceptEx(
    IN SOCKET sListenSocket,
    IN SOCKET sAcceptSocket,
    OUT PVOID lpOutputBuffer,
    IN DWORD dwReceiveDataLength,
    IN DWORD dwLocalAddressLength,
    IN DWORD dwRemoteAddressLength,
    OUT LPDWORD lpdwBytesReceived,
    IN OUT LPOVERLAPPED lpOverlapped)
{
    AFD_SUPER_ACCEPT_DATA AcceptData;
    PSOCKET_INFORMATION ListenSocket;
    PSOCKET_INFORMATION AcceptSocket;
    NTSTATUS Status;
    PIO_STATUS_BLOCK IOSB;
    HANDLE Event;
    PVOID APCContext;
    ULONG OutputLength;

    /* Validate parameters */
    if (!lpOverlapped || !lpOutputBuffer)
    {
        WSASetLastError(WSAEINVAL);
        return FALSE;
    }

    ListenSocket = GetSocketStructure(sListenSocket);
    AcceptSocket = GetSocketStructure(sAcceptSocket);
    if (!ListenSocket || !AcceptSocket)
    {
        WSASetLastError(WSAENOTSOCK);
        return FALSE;
    }

    /* Build the AFD super accept request */
    AcceptData.AcceptHandle = (HANDLE)AcceptSocket->Handle;
    AcceptData.ReceiveDataLength = dwReceiveDataLength;
    AcceptData.LocalAddressLength = dwLocalAddressLength;
    AcceptData.RemoteAddressLength = dwRemoteAddressLength;

    OutputLength = dwReceiveDataLength + dwLocalAddressLength + dwRemoteAddressLength;

    /* Set up for IOCP/overlapped — match pattern from WSPRecv */
    IOSB = (PIO_STATUS_BLOCK)&lpOverlapped->Internal;
    IOSB->Status = STATUS_PENDING;
    IOSB->Information = 0;

    Event = lpOverlapped->hEvent;
    if (Event)
    {
        APCContext = NULL;
    }
    else
    {
        /* No event: APC context = overlapped pointer for IOCP delivery */
        APCContext = lpOverlapped;
    }

    Status = NtDeviceIoControlFile(
        (HANDLE)ListenSocket->Handle,
        Event,
        NULL,           /* No APC routine */
        APCContext,
        IOSB,
        IOCTL_AFD_SUPER_ACCEPT,
        &AcceptData,
        sizeof(AcceptData),
        lpOutputBuffer,
        OutputLength);

    if (Status == STATUS_PENDING)
    {
        WSASetLastError(WSA_IO_PENDING);
        return FALSE;
    }

    if (NT_SUCCESS(Status))
    {
        if (lpdwBytesReceived)
            *lpdwBytesReceived = (DWORD)IOSB->Information;
        return TRUE;
    }

    WSASetLastError(TranslateNtStatusError(Status));
    return FALSE;
}

BOOL
WSPAPI
WSPDisconnectEx(
  IN SOCKET hSocket,
  IN LPOVERLAPPED lpOverlapped,
  IN DWORD dwFlags,
  IN DWORD reserved)
{
    UNIMPLEMENTED;

    return FALSE;
}

VOID
WSPAPI
WSPGetAcceptExSockaddrs(
    IN PVOID lpOutputBuffer,
    IN DWORD dwReceiveDataLength,
    IN DWORD dwLocalAddressLength,
    IN DWORD dwRemoteAddressLength,
    OUT struct sockaddr **LocalSockaddr,
    OUT LPINT LocalSockaddrLength,
    OUT struct sockaddr **RemoteSockaddr,
    OUT LPINT RemoteSockaddrLength)
{
    PCHAR Buffer = (PCHAR)lpOutputBuffer;
    PCHAR LocalSlot = Buffer + dwReceiveDataLength;
    PCHAR RemoteSlot = LocalSlot + dwLocalAddressLength;

    /* Each address slot format: [4-byte length][SOCKADDR data] */
    if (LocalSockaddr && LocalSockaddrLength)
    {
        *LocalSockaddrLength = *(INT *)LocalSlot;
        *LocalSockaddr = (struct sockaddr *)(LocalSlot + sizeof(INT));
    }

    if (RemoteSockaddr && RemoteSockaddrLength)
    {
        *RemoteSockaddrLength = *(INT *)RemoteSlot;
        *RemoteSockaddr = (struct sockaddr *)(RemoteSlot + sizeof(INT));
    }
}

/* EOF */
