/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test select() against closesocket() and data arriving from other threads
 */

#include "ws2_32.h"

typedef struct _SELECT_CALL
{
    SOCKET Socket;
    BOOL InWrite;
    BOOL InExcept;
    int Result;
    int Error;
    BOOL InRead;
    HANDLE Started;
} SELECT_CALL, *PSELECT_CALL;

static
DWORD
WINAPI
SelectThread(PVOID Parameter)
{
    PSELECT_CALL Call = Parameter;
    fd_set ReadFds, WriteFds, ExceptFds;
    struct timeval Timeout = { 5, 0 };

    FD_ZERO(&ReadFds);
    FD_ZERO(&WriteFds);
    FD_ZERO(&ExceptFds);
    FD_SET(Call->Socket, &ReadFds);
    if (Call->InWrite)
        FD_SET(Call->Socket, &WriteFds);
    if (Call->InExcept)
        FD_SET(Call->Socket, &ExceptFds);

    SetEvent(Call->Started);
    Call->Result = select(0, &ReadFds, Call->InWrite ? &WriteFds : NULL,
                          Call->InExcept ? &ExceptFds : NULL, &Timeout);
    Call->Error = WSAGetLastError();
    Call->InRead = FD_ISSET(Call->Socket, &ReadFds);
    return 0;
}

static
SOCKET
CreateListener(void)
{
    SOCKET Socket;
    struct sockaddr_in Addr;
    int Err;

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Err = bind(Socket, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Err == 0, "bind failed: %d\n", WSAGetLastError());
    Err = listen(Socket, SOMAXCONN);
    ok(Err == 0, "listen failed: %d\n", WSAGetLastError());
    return Socket;
}

static
BOOL
MakePair(SOCKET *Client, SOCKET *Server)
{
    SOCKET Listener;
    struct sockaddr_in Addr;
    int AddrLen = sizeof(Addr);
    int Err;

    *Client = *Server = INVALID_SOCKET;

    Listener = CreateListener();
    if (Listener == INVALID_SOCKET)
        return FALSE;
    Err = getsockname(Listener, (struct sockaddr *)&Addr, &AddrLen);
    ok(Err == 0, "getsockname failed: %d\n", WSAGetLastError());

    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(*Client != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    Err = connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Err == 0, "connect failed: %d\n", WSAGetLastError());
    *Server = accept(Listener, NULL, NULL);
    ok(*Server != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    closesocket(Listener);

    if (Err != 0 || *Server == INVALID_SOCKET)
    {
        closesocket(*Client);
        if (*Server != INVALID_SOCKET)
            closesocket(*Server);
        *Client = *Server = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

/* closesocket() on a socket another thread is selecting on completes the
 * select at once, with the socket reported readable */
static
void
Test_CloseWakesSelect(SOCKET Socket, BOOL InWrite, BOOL InExcept, PCSTR What)
{
    PSELECT_CALL Call;
    HANDLE Thread;
    DWORD Wait, Ticks;
    int Err;

    if (Socket == INVALID_SOCKET)
        return;

    /* Left alone if the select never returns */
    Call = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Call));
    if (!Call)
        return;
    Call->Socket = Socket;
    Call->InWrite = InWrite;
    Call->InExcept = InExcept;
    Call->Result = -2;
    Call->Started = CreateEventW(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, SelectThread, Call, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        return;

    WaitForSingleObject(Call->Started, INFINITE);
    Sleep(200);

    Ticks = GetTickCount();
    Err = closesocket(Socket);
    ok(Err == 0, "%s: closesocket returned %d, error %d\n", What, Err, WSAGetLastError());

    Wait = WaitForSingleObject(Thread, 10000);
    Ticks = GetTickCount() - Ticks;
    ok(Wait == WAIT_OBJECT_0, "%s: select did not return\n", What);
    if (Wait != WAIT_OBJECT_0)
        return;
    ok(Ticks < 2000, "%s: select returned %lu ms after the close\n", What, Ticks);
    ok(Call->Result == 1, "%s: select returned %d, error %d\n", What, Call->Result, Call->Error);
    ok(Call->InRead, "%s: closed socket not in readfds\n", What);

    CloseHandle(Thread);
    CloseHandle(Call->Started);
    HeapFree(GetProcessHeap(), 0, Call);
}

/*
 * Stress: selects with timeouts of a clock tick or two complete by timing out,
 * by data arriving and by their socket being closed, all on several CPUs at once.
 */
#define STRESS_PAIRS 6
#define STRESS_CLOSERS 4
#define STRESS_MS 20000

static volatile LONG g_Stop;
static LONG g_Selects, g_SelectsSignalled, g_Sends;
static LONG g_CloseSelects, g_CloseWoken, g_CloseHangs;

typedef struct _PAIR_CTX
{
    SOCKET Client;
    SOCKET Server;
} PAIR_CTX, *PPAIR_CTX;

static
DWORD
WINAPI
PairSelector(PVOID Parameter)
{
    PPAIR_CTX Pair = Parameter;
    fd_set ReadFds, ExceptFds;
    struct timeval Timeout;
    char Buffer[256];
    ULONG Iteration = 0;
    u_long NonBlocking = 1;
    int Result;

    ioctlsocket(Pair->Server, FIONBIO, &NonBlocking);
    while (!g_Stop)
    {
        FD_ZERO(&ReadFds);
        FD_ZERO(&ExceptFds);
        FD_SET(Pair->Server, &ReadFds);
        FD_SET(Pair->Server, &ExceptFds);
        Timeout.tv_sec = 0;
        Timeout.tv_usec = (Iteration++ % 3) * 1000;
        Result = select(0, &ReadFds, NULL, &ExceptFds, &Timeout);
        InterlockedIncrement(&g_Selects);
        if (Result > 0)
        {
            InterlockedIncrement(&g_SelectsSignalled);
            while (recv(Pair->Server, Buffer, sizeof(Buffer), 0) > 0)
                ;
        }
    }
    return 0;
}

static
DWORD
WINAPI
PairFeeder(PVOID Parameter)
{
    PPAIR_CTX Pair = Parameter;
    ULONG Iteration = 0;
    char Byte = 'x';

    while (!g_Stop)
    {
        if (send(Pair->Client, &Byte, 1, 0) == 1)
            InterlockedIncrement(&g_Sends);
        /* Spread the arrivals over the timer ticks */
        switch (Iteration++ % 4)
        {
            case 0: Sleep(0); break;
            case 1: Sleep(1); break;
            case 2: SwitchToThread(); break;
            default: Sleep(2); break;
        }
    }
    return 0;
}

static
DWORD
WINAPI
CloseSelector(PVOID Parameter)
{
    SOCKET Socket = (SOCKET)Parameter;
    fd_set ReadFds, ExceptFds;
    struct timeval Timeout = { 0, 15000 };

    FD_ZERO(&ReadFds);
    FD_ZERO(&ExceptFds);
    FD_SET(Socket, &ReadFds);
    FD_SET(Socket, &ExceptFds);
    /* Times out, or the close wakes it up; either is fine */
    if (select(0, &ReadFds, NULL, &ExceptFds, &Timeout) > 0)
        InterlockedIncrement(&g_CloseWoken);
    return 0;
}

static
DWORD
WINAPI
Closer(PVOID Parameter)
{
    ULONG Iteration = (ULONG)(ULONG_PTR)Parameter;
    SOCKET Socket;
    HANDLE Thread;

    while (!g_Stop)
    {
        Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Socket == INVALID_SOCKET)
            break;

        Thread = CreateThread(NULL, 0, CloseSelector, (PVOID)Socket, 0, NULL);
        if (!Thread)
        {
            closesocket(Socket);
            break;
        }
        InterlockedIncrement(&g_CloseSelects);

        /* Close around the time the select times out */
        Sleep(10 + (Iteration++ * 7) % 12);
        closesocket(Socket);

        if (WaitForSingleObject(Thread, 10000) != WAIT_OBJECT_0)
        {
            InterlockedIncrement(&g_CloseHangs);
            CloseHandle(Thread);
            break;
        }
        CloseHandle(Thread);
    }
    return 0;
}

static
void
Test_SelectStress(void)
{
    PAIR_CTX Pairs[STRESS_PAIRS];
    HANDLE Threads[STRESS_PAIRS * 2 + STRESS_CLOSERS];
    ULONG Count = 0, i;
    DWORD Wait;

    for (i = 0; i < STRESS_PAIRS; i++)
    {
        if (!MakePair(&Pairs[i].Client, &Pairs[i].Server))
            return;
    }

    g_Stop = FALSE;
    for (i = 0; i < STRESS_PAIRS; i++)
    {
        Threads[Count++] = CreateThread(NULL, 0, PairSelector, &Pairs[i], 0, NULL);
        Threads[Count++] = CreateThread(NULL, 0, PairFeeder, &Pairs[i], 0, NULL);
    }
    for (i = 0; i < STRESS_CLOSERS; i++)
        Threads[Count++] = CreateThread(NULL, 0, Closer, (PVOID)(ULONG_PTR)(i * 5), 0, NULL);
    for (i = 0; i < Count; i++)
        ok(Threads[i] != NULL, "CreateThread %lu failed: %lu\n", i, GetLastError());

    Sleep(STRESS_MS);
    InterlockedExchange(&g_Stop, TRUE);

    Wait = WaitForMultipleObjects(Count, Threads, TRUE, 30000);
    ok(Wait < WAIT_OBJECT_0 + Count, "stress threads did not finish: %lu\n", Wait);
    ok(g_CloseHangs == 0, "%ld selects not woken by closesocket\n", g_CloseHangs);
    ok(g_Selects > 1000, "only %ld selects\n", g_Selects);
    trace("%ld selects (%ld signalled), %ld sends, %ld selects closed under (%ld woken)\n",
          g_Selects, g_SelectsSignalled, g_Sends, g_CloseSelects, g_CloseWoken);

    if (Wait >= WAIT_OBJECT_0 + Count)
        return;
    for (i = 0; i < Count; i++)
        CloseHandle(Threads[i]);
    for (i = 0; i < STRESS_PAIRS; i++)
    {
        closesocket(Pairs[i].Client);
        closesocket(Pairs[i].Server);
    }
}

START_TEST(selectclose)
{
    WSADATA WsaData;
    SOCKET Client, Server;
    int Err;

    Err = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Err == 0, "WSAStartup failed: %d\n", Err);
    if (Err != 0)
        return;

    Test_CloseWakesSelect(CreateListener(), FALSE, FALSE, "listening");
    Test_CloseWakesSelect(CreateListener(), TRUE, TRUE, "listening, all sets");
    Test_CloseWakesSelect(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), FALSE, TRUE, "unbound");
    if (MakePair(&Client, &Server))
    {
        Test_CloseWakesSelect(Server, FALSE, TRUE, "connected");
        closesocket(Client);
    }

    Test_SelectStress();

    WSACleanup();
}
