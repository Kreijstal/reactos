/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test SO_RCVTIMEO / SO_SNDTIMEO on blocking sockets
 */

#include "ws2_32.h"

#define TIMEOUT_MS 500

/* How long we let a blocking call run before calling it hung */
#define HANG_MS 10000

typedef enum _BLOCKING_OP
{
    OpRecv,
    OpRecvFrom,
    OpSend
} BLOCKING_OP;

typedef struct _BLOCKING_CALL
{
    SOCKET Socket;
    BLOCKING_OP Op;
    char *Buffer;
    int Length;
    int Result;
    int Error;
    int Iterations;
    DWORD Elapsed;
} BLOCKING_CALL, *PBLOCKING_CALL;

static
DWORD
WINAPI
BlockingThread(PVOID Parameter)
{
    PBLOCKING_CALL Call = Parameter;
    struct sockaddr_in From;
    int FromLen = sizeof(From);
    DWORD Start;

    Start = GetTickCount();
    switch (Call->Op)
    {
        case OpRecv:
            Call->Result = recv(Call->Socket, Call->Buffer, Call->Length, 0);
            break;
        case OpRecvFrom:
            Call->Result = recvfrom(Call->Socket, Call->Buffer, Call->Length, 0,
                                    (struct sockaddr *)&From, &FromLen);
            break;
        case OpSend:
            /* Keep sending until the peer's window and our buffers are full
             * and a send has to block; that is the call which must time out */
            for (Call->Iterations = 0; Call->Iterations < 1024; Call->Iterations++)
            {
                Start = GetTickCount();
                Call->Result = send(Call->Socket, Call->Buffer, Call->Length, 0);
                if (Call->Result == SOCKET_ERROR)
                    break;
            }
            break;
    }
    Call->Error = WSAGetLastError();
    Call->Elapsed = GetTickCount() - Start;
    return 0;
}

/* Set once a blocking call never returned; its thread and socket are left alone */
static BOOL g_Hung;

/*
 * Runs one blocking call on a worker thread so that a call that never times
 * out fails the test instead of hanging it. Returns FALSE if it hung. The
 * worker, its socket and the call block are then left as they are (closing a
 * socket under a blocked call can block as well); process exit cleans up.
 */
static
BOOL
RunBlocking(PBLOCKING_CALL *CallOut, SOCKET Socket, BLOCKING_OP Op, char *Buffer, int Length)
{
    PBLOCKING_CALL Call;
    HANDLE Thread;
    DWORD Wait;

    Call = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Call));
    *CallOut = Call;
    if (!Call)
        return FALSE;
    Call->Socket = Socket;
    Call->Op = Op;
    Call->Buffer = Buffer;
    Call->Length = Length;

    Thread = CreateThread(NULL, 0, BlockingThread, Call, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        return FALSE;

    Wait = WaitForSingleObject(Thread, HANG_MS);
    if (Wait != WAIT_OBJECT_0)
    {
        g_Hung = TRUE;
        CloseHandle(Thread);
        *CallOut = NULL;
        return FALSE;
    }
    CloseHandle(Thread);
    return TRUE;
}

static
void
CheckTimedOut(PBLOCKING_CALL Call, PCSTR What)
{
    ok(Call->Result == SOCKET_ERROR, "%s returned %d\n", What, Call->Result);
    ok(Call->Error == WSAETIMEDOUT, "%s error %d, expected WSAETIMEDOUT\n", What, Call->Error);
    ok(Call->Elapsed >= TIMEOUT_MS - 100 && Call->Elapsed <= 2000,
       "%s took %lu ms, expected about %u ms\n", What, Call->Elapsed, TIMEOUT_MS);
}

static
void
SetTimeouts(SOCKET Socket, DWORD Recv, DWORD Send)
{
    int Err;

    Err = setsockopt(Socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&Recv, sizeof(Recv));
    ok(Err == 0, "setsockopt(SO_RCVTIMEO) failed: %d\n", WSAGetLastError());
    Err = setsockopt(Socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&Send, sizeof(Send));
    ok(Err == 0, "setsockopt(SO_SNDTIMEO) failed: %d\n", WSAGetLastError());
}

static
void
Test_GetSockOpt(void)
{
    SOCKET s;
    DWORD Value;
    int Size, Err;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(s != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (s == INVALID_SOCKET)
        return;

    /* The default is zero: never time out */
    Value = 0xdeadbeef;
    Size = sizeof(Value);
    Err = getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&Value, &Size);
    ok(Err == 0, "getsockopt(SO_RCVTIMEO) failed: %d\n", WSAGetLastError());
    ok(Value == 0, "default SO_RCVTIMEO is %lu\n", Value);
    ok(Size == sizeof(DWORD), "Size = %d\n", Size);
    Value = 0xdeadbeef;
    Size = sizeof(Value);
    Err = getsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&Value, &Size);
    ok(Err == 0, "getsockopt(SO_SNDTIMEO) failed: %d\n", WSAGetLastError());
    ok(Value == 0, "default SO_SNDTIMEO is %lu\n", Value);

    SetTimeouts(s, 1234, 5678);
    Value = 0;
    Size = sizeof(Value);
    Err = getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&Value, &Size);
    ok(Err == 0, "getsockopt(SO_RCVTIMEO) failed: %d\n", WSAGetLastError());
    ok(Value == 1234, "SO_RCVTIMEO is %lu\n", Value);
    Value = 0;
    Size = sizeof(Value);
    Err = getsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&Value, &Size);
    ok(Err == 0, "getsockopt(SO_SNDTIMEO) failed: %d\n", WSAGetLastError());
    ok(Value == 5678, "SO_SNDTIMEO is %lu\n", Value);

    closesocket(s);
}

static
void
Test_Udp(void)
{
    static char Buffer[64];
    SOCKET s, Peer;
    struct sockaddr_in Addr;
    int AddrLen = sizeof(Addr);
    PBLOCKING_CALL Call;
    int Err;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(s != INVALID_SOCKET && Peer != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (s == INVALID_SOCKET || Peer == INVALID_SOCKET)
        return;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Err = bind(s, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Err == 0, "bind failed: %d\n", WSAGetLastError());
    Err = getsockname(s, (struct sockaddr *)&Addr, &AddrLen);
    ok(Err == 0, "getsockname failed: %d\n", WSAGetLastError());

    SetTimeouts(s, TIMEOUT_MS, TIMEOUT_MS);

    if (!RunBlocking(&Call, s, OpRecv, Buffer, sizeof(Buffer)))
    {
        ok(0, "UDP recv did not time out\n");
        return;
    }
    CheckTimedOut(Call, "UDP recv");

    if (!RunBlocking(&Call, s, OpRecvFrom, Buffer, sizeof(Buffer)))
    {
        ok(0, "UDP recvfrom did not time out\n");
        return;
    }
    CheckTimedOut(Call, "UDP recvfrom");

    /* The socket is still usable: a datagram sent now is received */
    Err = sendto(Peer, "ping", 4, 0, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Err == 4, "sendto returned %d, error %d\n", Err, WSAGetLastError());
    if (!RunBlocking(&Call, s, OpRecvFrom, Buffer, sizeof(Buffer)))
    {
        ok(0, "UDP recvfrom hung with a datagram queued\n");
        return;
    }
    ok(Call->Result == 4, "recvfrom returned %d, error %d\n", Call->Result, Call->Error);
    ok(memcmp(Buffer, "ping", 4) == 0, "Wrong data received\n");

    closesocket(s);
    closesocket(Peer);
}

static
void
Test_Tcp(void)
{
    static char Buffer[64];
    SOCKET Listener, Client, Server;
    struct sockaddr_in Addr;
    int AddrLen = sizeof(Addr);
    PBLOCKING_CALL Call;
    char *Big;
    const int BigSize = 1024 * 1024;
    int Err;

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET && Client != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Listener == INVALID_SOCKET || Client == INVALID_SOCKET)
        return;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Err = bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Err == 0, "bind failed: %d\n", WSAGetLastError());
    Err = getsockname(Listener, (struct sockaddr *)&Addr, &AddrLen);
    ok(Err == 0, "getsockname failed: %d\n", WSAGetLastError());
    Err = listen(Listener, 1);
    ok(Err == 0, "listen failed: %d\n", WSAGetLastError());
    Err = connect(Client, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Err == 0, "connect failed: %d\n", WSAGetLastError());
    Server = accept(Listener, NULL, NULL);
    ok(Server != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    closesocket(Listener);
    if (Err != 0 || Server == INVALID_SOCKET)
    {
        closesocket(Client);
        return;
    }

    SetTimeouts(Client, TIMEOUT_MS, TIMEOUT_MS);

    if (!RunBlocking(&Call, Client, OpRecv, Buffer, sizeof(Buffer)))
    {
        ok(0, "TCP recv did not time out\n");
        return;
    }
    CheckTimedOut(Call, "TCP recv");

    /* Data that arrives after the timeout is still delivered */
    Err = send(Server, "pong", 4, 0);
    ok(Err == 4, "send returned %d, error %d\n", Err, WSAGetLastError());
    if (!RunBlocking(&Call, Client, OpRecv, Buffer, sizeof(Buffer)))
    {
        ok(0, "TCP recv hung with data queued\n");
        return;
    }
    ok(Call->Result == 4, "recv returned %d, error %d\n", Call->Result, Call->Error);
    ok(memcmp(Buffer, "pong", 4) == 0, "Wrong data received\n");

    /* A send the peer never drains times out too */
    Big = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, BigSize);
    ok(Big != NULL, "HeapAlloc failed\n");
    if (Big)
    {
        if (!RunBlocking(&Call, Client, OpSend, Big, BigSize))
        {
            /* The worker may still be writing into Big; leak it */
            ok(0, "TCP send did not time out\n");
            return;
        }
        ok(Call->Result == SOCKET_ERROR, "TCP send returned %d after %d sends\n",
           Call->Result, Call->Iterations);
        ok(Call->Error == WSAETIMEDOUT, "TCP send error %d, expected WSAETIMEDOUT\n", Call->Error);
        ok(Call->Elapsed >= TIMEOUT_MS - 100 && Call->Elapsed <= 2000,
           "TCP send took %lu ms\n", Call->Elapsed);

        /* Drain what the client queued, so that its graceful close can finish */
        SetTimeouts(Server, TIMEOUT_MS, 0);
        while (recv(Server, Big, BigSize, 0) > 0)
            ;
        HeapFree(GetProcessHeap(), 0, Big);
    }

    closesocket(Client);
    closesocket(Server);
}

START_TEST(recvtimeout)
{
    WSADATA WsaData;
    int Err;

    Err = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Err == 0, "WSAStartup failed: %d\n", Err);
    if (Err != 0)
        return;

    Test_GetSockOpt();
    Test_Udp();
    Test_Tcp();

    if (!g_Hung)
        WSACleanup();
}
