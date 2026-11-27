/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that TCP_NODELAY reaches the connection of the socket it is
 *              set on: an accepted one, one set before connect, after connect,
 *              and one an accepted socket has from its listener
 */

#include "ws2_32.h"
#include <mstcpip.h>

/* Each round, the writer sends two bytes in two sends and the reader answers
 * once it has both.  With Nagle's algorithm the second byte waits until the
 * first is acknowledged, which the reader delays (by up to 200 ms on Windows,
 * 250 ms with lwIP): ROUNDS rounds take a second or so.  Without it they take
 * a few milliseconds. */
#define ROUNDS 10
#define SLOW_MS 300

typedef enum _NODELAY_WHEN
{
    NoDelayNever,
    NoDelayBeforeConnect,
    NoDelayAfterConnect,
    NoDelayOnListener
} NODELAY_WHEN;

static
BOOL
SetNoDelay(
    _In_ SOCKET Socket)
{
    BOOL On = TRUE;
    int Result;

    Result = setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&On, sizeof(On));
    ok(Result == 0, "setsockopt(TCP_NODELAY) failed with %d\n", WSAGetLastError());
    return Result == 0;
}

/* A connection on the loopback: Client connected to Accepted, with
 * TCP_NODELAY on the client When says, or on the listener */
static
BOOL
Connect(
    _Out_ SOCKET *Client,
    _Out_ SOCKET *Accepted,
    _In_ NODELAY_WHEN When)
{
    SOCKET Listener;
    struct sockaddr_in Address;
    int Length = sizeof(Address);
    DWORD Timeout = 10 * 1000;
    int Result;

    *Client = INVALID_SOCKET;
    *Accepted = INVALID_SOCKET;

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (Listener == INVALID_SOCKET)
        return FALSE;
    if (When == NoDelayOnListener && !SetNoDelay(Listener))
    {
        closesocket(Listener);
        return FALSE;
    }
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Result = bind(Listener, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0, "bind failed with %d\n", WSAGetLastError());
    Result = getsockname(Listener, (struct sockaddr *)&Address, &Length);
    ok(Result == 0, "getsockname failed with %d\n", WSAGetLastError());
    Result = listen(Listener, 1);
    ok(Result == 0, "listen failed with %d\n", WSAGetLastError());

    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(*Client != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (*Client == INVALID_SOCKET)
        goto Fail;
    if (When == NoDelayBeforeConnect && !SetNoDelay(*Client))
        goto Fail;
    Result = connect(*Client, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0, "connect failed with %d\n", WSAGetLastError());
    if (Result)
        goto Fail;
    if (When == NoDelayAfterConnect && !SetNoDelay(*Client))
        goto Fail;

    *Accepted = accept(Listener, NULL, NULL);
    ok(*Accepted != INVALID_SOCKET, "accept failed with %d\n", WSAGetLastError());
    if (*Accepted == INVALID_SOCKET)
        goto Fail;
    closesocket(Listener);

    setsockopt(*Client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&Timeout, sizeof(Timeout));
    setsockopt(*Accepted, SOL_SOCKET, SO_RCVTIMEO, (const char *)&Timeout, sizeof(Timeout));
    return TRUE;

Fail:
    if (*Client != INVALID_SOCKET)
        closesocket(*Client);
    *Client = INVALID_SOCKET;
    closesocket(Listener);
    return FALSE;
}

/* How long ROUNDS rounds of Writer sending two bytes and Reader answering
 * take, in milliseconds */
static
DWORD
RoundTrips(
    _In_ SOCKET Writer,
    _In_ SOCKET Reader)
{
    DWORD Start;
    CHAR Buffer[2];
    int Round, Received, Result;

    Start = GetTickCount();
    for (Round = 0; Round < ROUNDS; Round++)
    {
        Result = send(Writer, "a", 1, 0);
        ok(Result == 1, "send failed with %d\n", WSAGetLastError());
        Result = send(Writer, "b", 1, 0);
        ok(Result == 1, "send failed with %d\n", WSAGetLastError());

        for (Received = 0; Received < 2; Received += Result)
        {
            Result = recv(Reader, Buffer + Received, 2 - Received, 0);
            ok(Result > 0, "recv returned %d, error %d\n", Result, WSAGetLastError());
            if (Result <= 0)
                return INFINITE;
        }
        ok(Buffer[0] == 'a' && Buffer[1] == 'b', "Received %.2s\n", Buffer);

        Result = send(Reader, "c", 1, 0);
        ok(Result == 1, "send failed with %d\n", WSAGetLastError());
        Result = recv(Writer, Buffer, 1, 0);
        ok(Result == 1, "recv returned %d, error %d\n", Result, WSAGetLastError());
        if (Result != 1)
            return INFINITE;
    }

    return GetTickCount() - Start;
}

/* Without TCP_NODELAY the rounds are slow: what tells the others apart */
static
VOID
TestNagle(void)
{
    SOCKET Client, Accepted;
    DWORD Elapsed;

    if (!Connect(&Client, &Accepted, NoDelayNever))
        return;

    Elapsed = RoundTrips(Accepted, Client);
    ok(Elapsed >= SLOW_MS && Elapsed != INFINITE,
       "%d rounds without TCP_NODELAY took %lu ms: Nagle's algorithm did not hold the second send\n",
       ROUNDS, Elapsed);

    Elapsed = RoundTrips(Client, Accepted);
    ok(Elapsed >= SLOW_MS && Elapsed != INFINITE,
       "%d rounds without TCP_NODELAY took %lu ms: Nagle's algorithm did not hold the second send\n",
       ROUNDS, Elapsed);

    closesocket(Client);
    closesocket(Accepted);
}

/* TCP_NODELAY on an accepted socket is for its connection */
static
VOID
TestAccepted(void)
{
    SOCKET Client, Accepted;
    DWORD Elapsed;

    if (!Connect(&Client, &Accepted, NoDelayNever))
        return;

    if (SetNoDelay(Accepted))
    {
        Elapsed = RoundTrips(Accepted, Client);
        ok(Elapsed < SLOW_MS, "%d rounds with TCP_NODELAY on the accepted socket took %lu ms\n",
           ROUNDS, Elapsed);

        /* The client still has Nagle's algorithm */
        Elapsed = RoundTrips(Client, Accepted);
        ok(Elapsed >= SLOW_MS && Elapsed != INFINITE,
           "%d rounds from the client without TCP_NODELAY took %lu ms\n", ROUNDS, Elapsed);
    }

    closesocket(Client);
    closesocket(Accepted);
}

/* TCP_NODELAY set before connect is for the connection connect makes */
static
VOID
TestClient(
    _In_ NODELAY_WHEN When)
{
    SOCKET Client, Accepted;
    DWORD Elapsed;

    if (!Connect(&Client, &Accepted, When))
        return;

    Elapsed = RoundTrips(Client, Accepted);
    ok(Elapsed < SLOW_MS, "%d rounds with TCP_NODELAY set %s connect took %lu ms\n",
       ROUNDS, When == NoDelayBeforeConnect ? "before" : "after", Elapsed);

    /* The accepted socket still has Nagle's algorithm */
    Elapsed = RoundTrips(Accepted, Client);
    ok(Elapsed >= SLOW_MS && Elapsed != INFINITE,
       "%d rounds from the accepted socket without TCP_NODELAY took %lu ms\n", ROUNDS, Elapsed);

    closesocket(Client);
    closesocket(Accepted);
}

/* An accepted socket has the TCP_NODELAY of its listener: its connection
 * does */
static
VOID
TestListener(void)
{
    SOCKET Client, Accepted;
    DWORD Elapsed;

    if (!Connect(&Client, &Accepted, NoDelayOnListener))
        return;

    Elapsed = RoundTrips(Accepted, Client);
    ok(Elapsed < SLOW_MS, "%d rounds with TCP_NODELAY on the listener took %lu ms\n",
       ROUNDS, Elapsed);

    /* The client still has Nagle's algorithm */
    Elapsed = RoundTrips(Client, Accepted);
    ok(Elapsed >= SLOW_MS && Elapsed != INFINITE,
       "%d rounds from the client without TCP_NODELAY took %lu ms\n", ROUNDS, Elapsed);

    closesocket(Client);
    closesocket(Accepted);
}

START_TEST(nodelay)
{
    WSADATA WsaData;
    int Result;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Result == 0, "WSAStartup failed with %d\n", Result);
    if (Result)
        return;

    TestNagle();
    TestAccepted();
    TestClient(NoDelayBeforeConnect);
    TestClient(NoDelayAfterConnect);
    TestListener();

    WSACleanup();
}
