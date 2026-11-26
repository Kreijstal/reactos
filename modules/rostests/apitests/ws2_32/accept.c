/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test accepting connections: AcceptEx on a connection that is already queued, what a failed AcceptEx and an accepted connection leave behind, what WSAAccept's condition function is told, and the names of both ends
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "ws2_32.h"
#include <ndk/exfuncs.h>
#include <mswsock.h>

#define ACCEPT_ADDRESS_LENGTH (sizeof(struct sockaddr_in) + 16)
#define ACCEPT_DATA_LENGTH 64
#define ACCEPT_WAIT_MS 5000

/* Pool tags: an MDL (IoAllocateMdl), AFD's queue entry of a connection waiting
 * to be accepted, and the TDI connection information AFD allocates */
#define TAG_MDL ' LDM'
#define TAG_AFD_ACCEPT_QUEUE 'qafA'
#define TAG_AFD_TDI_CONNECTION_INFORMATION 'cTfA'

static LPFN_ACCEPTEX pAcceptEx;
static LPFN_GETACCEPTEXSOCKADDRS pGetAcceptExSockaddrs;

/* Allocations of a pool tag not freed yet */
static
BOOL
GetPoolTagOutstanding(ULONG Tag, LONG *Outstanding)
{
    PSYSTEM_POOLTAG_INFORMATION Info;
    ULONG Size = 0x10000, i;
    NTSTATUS Status;

    for (;;)
    {
        Info = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Info)
            return FALSE;
        Status = NtQuerySystemInformation(SystemPoolTagInformation, Info, Size, &Size);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            break;
        HeapFree(GetProcessHeap(), 0, Info);
        Size += 0x1000;
    }

    *Outstanding = 0;
    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < Info->Count; i++)
        {
            if (Info->TagInfo[i].TagUlong != Tag)
                continue;
            *Outstanding += (LONG)(Info->TagInfo[i].PagedAllocs - Info->TagInfo[i].PagedFrees);
            *Outstanding += (LONG)(Info->TagInfo[i].NonPagedAllocs - Info->TagInfo[i].NonPagedFrees);
        }
    }

    HeapFree(GetProcessHeap(), 0, Info);
    return NT_SUCCESS(Status);
}

static
SOCKET
CreateListener(struct sockaddr_in *Address)
{
    SOCKET Listener;
    int AddressLength, Result;

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Listener == INVALID_SOCKET)
        return INVALID_SOCKET;

    memset(Address, 0, sizeof(*Address));
    Address->sin_family = AF_INET;
    Address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Result = bind(Listener, (struct sockaddr *)Address, sizeof(*Address));
    ok(Result == 0, "bind failed: %d\n", WSAGetLastError());
    AddressLength = sizeof(*Address);
    Result = getsockname(Listener, (struct sockaddr *)Address, &AddressLength);
    ok(Result == 0, "getsockname failed: %d\n", WSAGetLastError());
    Result = listen(Listener, SOMAXCONN);
    ok(Result == 0, "listen failed: %d\n", WSAGetLastError());
    return Listener;
}

static
SOCKET
Connect(const struct sockaddr_in *Address, struct sockaddr_in *ClientAddress)
{
    SOCKET Client;
    int AddressLength, Result;

    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Client != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Client == INVALID_SOCKET)
        return INVALID_SOCKET;
    Result = connect(Client, (const struct sockaddr *)Address, sizeof(*Address));
    ok(Result == 0, "connect failed: %d\n", WSAGetLastError());
    if (ClientAddress)
    {
        AddressLength = sizeof(*ClientAddress);
        Result = getsockname(Client, (struct sockaddr *)ClientAddress, &AddressLength);
        ok(Result == 0, "getsockname failed: %d\n", WSAGetLastError());
    }
    return Client;
}

/*
 * A connection that completed before any accept was posted waits in the
 * listener's backlog.  An AcceptEx posted then takes it: it completes without
 * another client connecting, with the first data block if the client sent
 * one already.
 */
static
void
TestAcceptExQueued(DWORD DataLength)
{
    SOCKET Listener, Acceptor = INVALID_SOCKET, Client = INVALID_SOCKET, Late = INVALID_SOCKET;
    struct sockaddr_in Address, ClientAddress, *Local, *Remote;
    char Buffer[ACCEPT_DATA_LENGTH + 2 * ACCEPT_ADDRESS_LENGTH];
    static const char Data[] = "queued";
    WSAOVERLAPPED Overlapped;
    DWORD Bytes = 0, Wait, Error;
    int LocalLength, RemoteLength, Result;
    BOOL Success, Pending = FALSE;

    memset(&Overlapped, 0, sizeof(Overlapped));
    Listener = CreateListener(&Address);
    if (Listener == INVALID_SOCKET)
        return;
    Acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Acceptor != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    Client = Connect(&Address, &ClientAddress);
    if (Acceptor == INVALID_SOCKET || Client == INVALID_SOCKET)
        goto Cleanup;

    if (DataLength)
    {
        Result = send(Client, Data, sizeof(Data), 0);
        ok(Result == sizeof(Data), "send returned %d: %d\n", Result, WSAGetLastError());
    }

    /* Let the listener queue the connection (and the data reach it) */
    Sleep(300);

    memset(Buffer, 0xcc, sizeof(Buffer));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Success = pAcceptEx(Listener, Acceptor, Buffer, DataLength,
                        ACCEPT_ADDRESS_LENGTH, ACCEPT_ADDRESS_LENGTH,
                        &Bytes, &Overlapped);
    Error = WSAGetLastError();
    ok(Success || Error == ERROR_IO_PENDING, "%lu: AcceptEx failed: %lu\n", DataLength, Error);
    if (!Success && Error != ERROR_IO_PENDING)
        goto Cleanup;
    Pending = TRUE;

    Wait = WaitForSingleObject(Overlapped.hEvent, ACCEPT_WAIT_MS);
    ok(Wait == WAIT_OBJECT_0, "%lu: AcceptEx did not take the queued connection: %lu\n", DataLength, Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        /* Make sure it is not still pending on the client below */
        Late = Connect(&Address, NULL);
        Wait = WaitForSingleObject(Overlapped.hEvent, ACCEPT_WAIT_MS);
        ok(Wait == WAIT_OBJECT_0, "%lu: AcceptEx did not take the next connection either: %lu\n",
           DataLength, Wait);
        goto Cleanup;
    }
    Pending = FALSE;

    Success = WSAGetOverlappedResult(Listener, &Overlapped, &Bytes, FALSE, &Error);
    ok(Success, "%lu: AcceptEx failed: %lu\n", DataLength, WSAGetLastError());
    if (!Success)
        goto Cleanup;

    ok(Bytes == (DataLength ? sizeof(Data) : 0), "%lu: AcceptEx received %lu bytes\n", DataLength, Bytes);
    if (DataLength)
        ok(!memcmp(Buffer, Data, sizeof(Data)), "%lu: AcceptEx received other data\n", DataLength);

    Local = Remote = NULL;
    LocalLength = RemoteLength = 0;
    pGetAcceptExSockaddrs(Buffer, DataLength, ACCEPT_ADDRESS_LENGTH, ACCEPT_ADDRESS_LENGTH,
                          (struct sockaddr **)&Local, &LocalLength,
                          (struct sockaddr **)&Remote, &RemoteLength);
    ok(Remote && Remote->sin_port == ClientAddress.sin_port,
       "%lu: AcceptEx took another connection (%u, expected %u)\n", DataLength,
       Remote ? ntohs(Remote->sin_port) : 0, ntohs(ClientAddress.sin_port));
    ok(Local && Local->sin_port == Address.sin_port,
       "%lu: AcceptEx returned local port %u, expected %u\n", DataLength,
       Local ? ntohs(Local->sin_port) : 0, ntohs(Address.sin_port));

    /* The accepted socket is connected to the client */
    Result = setsockopt(Acceptor, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&Listener, sizeof(Listener));
    ok(Result == 0, "%lu: SO_UPDATE_ACCEPT_CONTEXT failed: %d\n", DataLength, WSAGetLastError());
    Result = send(Acceptor, Data, sizeof(Data), 0);
    ok(Result == sizeof(Data), "%lu: send on the accepted socket returned %d: %d\n", DataLength, Result, WSAGetLastError());
    memset(Buffer, 0, sizeof(Buffer));
    Result = recv(Client, Buffer, sizeof(Buffer), 0);
    ok(Result == sizeof(Data) && !memcmp(Buffer, Data, sizeof(Data)),
       "%lu: the client received %d: %d\n", DataLength, Result, WSAGetLastError());

Cleanup:
    if (Late != INVALID_SOCKET)
        closesocket(Late);
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    if (Acceptor != INVALID_SOCKET)
        closesocket(Acceptor);
    closesocket(Listener);
    if (Pending)
    {
        /* It writes to Overlapped and Buffer until it completes: closing the
         * listener ends it */
        Wait = WaitForSingleObject(Overlapped.hEvent, ACCEPT_WAIT_MS);
        ok(Wait == WAIT_OBJECT_0, "%lu: the AcceptEx never completed: %lu\n", DataLength, Wait);
    }
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
}

/*
 * An AcceptEx whose output buffer cannot be written fails, many times over,
 * and leaves nothing allocated behind.
 */
#define ACCEPTEX_FAULT_ITERATIONS 3000

static
void
TestAcceptExBadBuffer(void)
{
    SOCKET Listener, Acceptor;
    struct sockaddr_in Address;
    WSAOVERLAPPED Overlapped;
    PVOID Reserved;
    LONG Before, After;
    DWORD Bytes, Error, Wait;
    BOOL Success;
    ULONG i, Failed = 0;

    Listener = CreateListener(&Address);
    if (Listener == INVALID_SOCKET)
        return;
    Acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Acceptor != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());

    /* Address space with no memory behind it */
    Reserved = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ok(Reserved != NULL, "VirtualAlloc failed: %lu\n", GetLastError());

    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (Acceptor == INVALID_SOCKET || !Reserved || !GetPoolTagOutstanding(TAG_MDL, &Before))
    {
        skip("Cannot run the test\n");
        goto Cleanup;
    }

    for (i = 0; i < ACCEPTEX_FAULT_ITERATIONS; i++)
    {
        ResetEvent(Overlapped.hEvent);
        Success = pAcceptEx(Listener, Acceptor, Reserved, 0,
                            ACCEPT_ADDRESS_LENGTH, ACCEPT_ADDRESS_LENGTH,
                            &Bytes, &Overlapped);
        Error = WSAGetLastError();
        if (!Success && Error == ERROR_IO_PENDING)
        {
            /* Not refused right away: it must not take a connection into
             * that buffer */
            CancelIo((HANDLE)Listener);
            Wait = WaitForSingleObject(Overlapped.hEvent, ACCEPT_WAIT_MS);
            ok(Wait == WAIT_OBJECT_0, "the cancelled AcceptEx did not complete: %lu\n", Wait);
            if (Wait != WAIT_OBJECT_0)
                goto Cleanup;
            Success = WSAGetOverlappedResult(Listener, &Overlapped, &Bytes, FALSE, &Error);
        }
        if (!Success)
            Failed++;
    }
    ok(Failed == ACCEPTEX_FAULT_ITERATIONS, "%lu of %u AcceptEx into no memory succeeded\n",
       ACCEPTEX_FAULT_ITERATIONS - Failed, ACCEPTEX_FAULT_ITERATIONS);

    ok(GetPoolTagOutstanding(TAG_MDL, &After), "Cannot query the pool tags\n");
    trace("MDLs outstanding: %ld before, %ld after %u failed AcceptEx\n", Before, After, ACCEPTEX_FAULT_ITERATIONS);
    ok(After - Before < ACCEPTEX_FAULT_ITERATIONS / 4,
       "%ld MDLs leaked by %u failed AcceptEx\n", After - Before, ACCEPTEX_FAULT_ITERATIONS);

Cleanup:
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
    if (Reserved)
        VirtualFree(Reserved, 0, MEM_RELEASE);
    if (Acceptor != INVALID_SOCKET)
        closesocket(Acceptor);
    closesocket(Listener);
}

/*
 * Every connection AFD queues for accept() or AcceptEx() is given back once
 * accepted.
 */
#define ACCEPT_LEAK_ITERATIONS 100

static
void
TestAcceptLeak(void)
{
    SOCKET Listener, Acceptor, Client, Accepted;
    struct sockaddr_in Address;
    char Buffer[2 * ACCEPT_ADDRESS_LENGTH];
    WSAOVERLAPPED Overlapped;
    LONG QueueBefore, QueueAfter, InfoBefore, InfoAfter;
    DWORD Bytes, Wait;
    BOOL Success;
    ULONG i;

    Listener = CreateListener(&Address);
    if (Listener == INVALID_SOCKET)
        return;
    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!GetPoolTagOutstanding(TAG_AFD_ACCEPT_QUEUE, &QueueBefore) ||
        !GetPoolTagOutstanding(TAG_AFD_TDI_CONNECTION_INFORMATION, &InfoBefore))
    {
        skip("Cannot query the pool tags\n");
        goto Cleanup;
    }

    for (i = 0; i < ACCEPT_LEAK_ITERATIONS; i++)
    {
        /* accept() */
        Client = Connect(&Address, NULL);
        if (Client == INVALID_SOCKET)
            goto Cleanup;
        Accepted = accept(Listener, NULL, NULL);
        ok(Accepted != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
        closesocket(Client);
        if (Accepted == INVALID_SOCKET)
            goto Cleanup;
        closesocket(Accepted);

        /* AcceptEx(), posted before the client connects */
        Acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(Acceptor != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
        if (Acceptor == INVALID_SOCKET)
            goto Cleanup;
        ResetEvent(Overlapped.hEvent);
        Success = pAcceptEx(Listener, Acceptor, Buffer, 0,
                            ACCEPT_ADDRESS_LENGTH, ACCEPT_ADDRESS_LENGTH,
                            &Bytes, &Overlapped);
        ok(Success || WSAGetLastError() == ERROR_IO_PENDING, "AcceptEx failed: %d\n", WSAGetLastError());
        Client = Connect(&Address, NULL);
        Wait = WaitForSingleObject(Overlapped.hEvent, ACCEPT_WAIT_MS);
        ok(Wait == WAIT_OBJECT_0, "AcceptEx did not complete: %lu\n", Wait);
        if (Client != INVALID_SOCKET)
            closesocket(Client);
        closesocket(Acceptor);
        if (Wait != WAIT_OBJECT_0)
        {
            closesocket(Listener);
            Listener = INVALID_SOCKET;
            WaitForSingleObject(Overlapped.hEvent, ACCEPT_WAIT_MS);
            goto Cleanup;
        }
    }

    /* Let the closed sockets go */
    Sleep(1000);

    ok(GetPoolTagOutstanding(TAG_AFD_ACCEPT_QUEUE, &QueueAfter), "Cannot query the pool tags\n");
    ok(GetPoolTagOutstanding(TAG_AFD_TDI_CONNECTION_INFORMATION, &InfoAfter), "Cannot query the pool tags\n");
    trace("Accept queue entries outstanding: %ld before, %ld after; connection information: %ld before, %ld after\n",
          QueueBefore, QueueAfter, InfoBefore, InfoAfter);
    ok(QueueAfter - QueueBefore < ACCEPT_LEAK_ITERATIONS / 2,
       "%ld accept queue entries leaked by %u accept() and %u AcceptEx()\n",
       QueueAfter - QueueBefore, ACCEPT_LEAK_ITERATIONS, ACCEPT_LEAK_ITERATIONS);
    ok(InfoAfter - InfoBefore < ACCEPT_LEAK_ITERATIONS / 2,
       "%ld connection informations leaked by %u accept() and %u AcceptEx()\n",
       InfoAfter - InfoBefore, ACCEPT_LEAK_ITERATIONS, ACCEPT_LEAK_ITERATIONS);

Cleanup:
    if (Listener != INVALID_SOCKET)
        closesocket(Listener);
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
}

/*
 * WSAAccept's condition function gets the caller's address (the client) in
 * lpCallerId and the local address of the connection -- the address the
 * listener is bound to -- in lpCalleeId.
 */
typedef struct _CONDITION_RESULT
{
    ULONG Calls;
    struct sockaddr_in Caller;
    int CallerLength;
    struct sockaddr_in Callee;
    int CalleeLength;
} CONDITION_RESULT;

static
int
CALLBACK
Condition(LPWSABUF lpCallerId, LPWSABUF lpCallerData, LPQOS lpSQOS, LPQOS lpGQOS,
          LPWSABUF lpCalleeId, LPWSABUF lpCalleeData, GROUP *g, DWORD_PTR dwCallbackData)
{
    CONDITION_RESULT *Result = (CONDITION_RESULT *)dwCallbackData;

    Result->Calls++;
    if (lpCallerId && lpCallerId->buf)
    {
        Result->CallerLength = lpCallerId->len;
        memcpy(&Result->Caller, lpCallerId->buf, min(lpCallerId->len, sizeof(Result->Caller)));
    }
    if (lpCalleeId && lpCalleeId->buf)
    {
        Result->CalleeLength = lpCalleeId->len;
        memcpy(&Result->Callee, lpCalleeId->buf, min(lpCalleeId->len, sizeof(Result->Callee)));
    }
    return CF_ACCEPT;
}

static
void
TestWSAAcceptCondition(void)
{
    SOCKET Listener, Client, Accepted;
    struct sockaddr_in Address, ClientAddress;
    CONDITION_RESULT Result;

    Listener = CreateListener(&Address);
    if (Listener == INVALID_SOCKET)
        return;
    Client = Connect(&Address, &ClientAddress);
    if (Client == INVALID_SOCKET)
        goto Cleanup;

    memset(&Result, 0, sizeof(Result));
    Accepted = WSAAccept(Listener, NULL, NULL, Condition, (DWORD_PTR)&Result);
    ok(Accepted != INVALID_SOCKET, "WSAAccept failed: %d\n", WSAGetLastError());
    ok(Result.Calls == 1, "The condition function was called %lu times\n", Result.Calls);

    ok(Result.CallerLength >= (int)sizeof(Result.Caller), "lpCallerId has %d bytes\n", Result.CallerLength);
    ok(Result.Caller.sin_family == AF_INET &&
       Result.Caller.sin_addr.s_addr == ClientAddress.sin_addr.s_addr &&
       Result.Caller.sin_port == ClientAddress.sin_port,
       "lpCallerId is %u %s:%u, expected the client's %s:%u\n",
       Result.Caller.sin_family, inet_ntoa(Result.Caller.sin_addr), ntohs(Result.Caller.sin_port),
       "127.0.0.1", ntohs(ClientAddress.sin_port));

    ok(Result.CalleeLength >= (int)sizeof(Result.Callee), "lpCalleeId has %d bytes\n", Result.CalleeLength);
    ok(Result.Callee.sin_family == AF_INET &&
       Result.Callee.sin_addr.s_addr == Address.sin_addr.s_addr &&
       Result.Callee.sin_port == Address.sin_port,
       "lpCalleeId is %u %s:%u, expected the listener's %s:%u\n",
       Result.Callee.sin_family, inet_ntoa(Result.Callee.sin_addr), ntohs(Result.Callee.sin_port),
       "127.0.0.1", ntohs(Address.sin_port));

    if (Accepted != INVALID_SOCKET)
        closesocket(Accepted);
    closesocket(Client);
Cleanup:
    closesocket(Listener);
}

/*
 * Both ends of an accepted connection name the same two addresses: the
 * client's getsockname() (bound implicitly by connect()) is the accepted
 * socket's getpeername(), and the other way round.
 */
static
void
TestConnectedNames(void)
{
    SOCKET Listener, Client, Accepted;
    struct sockaddr_in Address, ClientName, ClientPeer, AcceptedName, AcceptedPeer, FromAccept;
    int Length, Result;

    Listener = CreateListener(&Address);
    if (Listener == INVALID_SOCKET)
        return;
    Client = Connect(&Address, &ClientName);
    if (Client == INVALID_SOCKET)
        goto Cleanup;
    Length = sizeof(FromAccept);
    Accepted = accept(Listener, (struct sockaddr *)&FromAccept, &Length);
    ok(Accepted != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    if (Accepted == INVALID_SOCKET)
    {
        closesocket(Client);
        goto Cleanup;
    }

    Length = sizeof(ClientPeer);
    Result = getpeername(Client, (struct sockaddr *)&ClientPeer, &Length);
    ok(Result == 0, "getpeername failed: %d\n", WSAGetLastError());
    Length = sizeof(AcceptedName);
    Result = getsockname(Accepted, (struct sockaddr *)&AcceptedName, &Length);
    ok(Result == 0, "getsockname failed: %d\n", WSAGetLastError());
    Length = sizeof(AcceptedPeer);
    Result = getpeername(Accepted, (struct sockaddr *)&AcceptedPeer, &Length);
    ok(Result == 0, "getpeername failed: %d\n", WSAGetLastError());

    ok(ClientPeer.sin_port == Address.sin_port, "The client's peer port is %u, expected %u\n",
       ntohs(ClientPeer.sin_port), ntohs(Address.sin_port));
    ok(AcceptedName.sin_port == Address.sin_port, "The accepted socket's port is %u, expected %u\n",
       ntohs(AcceptedName.sin_port), ntohs(Address.sin_port));
    ok(AcceptedPeer.sin_port == FromAccept.sin_port, "The accepted socket's peer port is %u, accept() returned %u\n",
       ntohs(AcceptedPeer.sin_port), ntohs(FromAccept.sin_port));
    ok(ClientName.sin_port == AcceptedPeer.sin_port, "The client's port is %u, the accepted socket's peer port %u\n",
       ntohs(ClientName.sin_port), ntohs(AcceptedPeer.sin_port));
    ok(ClientName.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "The client's address is %s\n",
       inet_ntoa(ClientName.sin_addr));

    closesocket(Accepted);
    closesocket(Client);
Cleanup:
    closesocket(Listener);
}

START_TEST(accept)
{
    WSADATA WsaData;
    GUID AcceptExGuid = WSAID_ACCEPTEX, SockaddrsGuid = WSAID_GETACCEPTEXSOCKADDRS;
    SOCKET Socket;
    DWORD Bytes;
    int Result;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Result == 0, "WSAStartup failed: %d\n", Result);

    TestConnectedNames();
    TestWSAAcceptCondition();

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    Result = WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                      &AcceptExGuid, sizeof(AcceptExGuid),
                      &pAcceptEx, sizeof(pAcceptEx), &Bytes, NULL, NULL);
    if (Result == 0)
        Result = WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                          &SockaddrsGuid, sizeof(SockaddrsGuid),
                          &pGetAcceptExSockaddrs, sizeof(pGetAcceptExSockaddrs), &Bytes, NULL, NULL);
    closesocket(Socket);
    if (Result != 0 || !pAcceptEx || !pGetAcceptExSockaddrs)
    {
        skip("No AcceptEx: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }

    TestAcceptExQueued(0);
    TestAcceptExQueued(ACCEPT_DATA_LENGTH);
    TestAcceptExBadBuffer();
    TestAcceptLeak();

    WSACleanup();
}
