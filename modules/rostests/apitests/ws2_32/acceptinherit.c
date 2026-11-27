/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a socket accepted by accept(), or by AcceptEx() once
 *              SO_UPDATE_ACCEPT_CONTEXT is set, has the options of the
 *              listening socket, and getsockopt(TCP_NODELAY)
 */

#include "ws2_32.h"
#include <mswsock.h>

#define ADDRESS_LENGTH (sizeof(struct sockaddr_in) + 16)

/* The options Wine's test_accept_inheritance() finds an accepted socket
 * inherits on Windows, with values that are not their defaults */
static const struct
{
    int Level;
    int Name;
    int Value;
} IntOptions[] =
{
    { SOL_SOCKET,  SO_REUSEADDR, 1 },
    { SOL_SOCKET,  SO_KEEPALIVE, 1 },
    { SOL_SOCKET,  SO_OOBINLINE, 1 },
    { SOL_SOCKET,  SO_SNDBUF,    0x123 },
    { SOL_SOCKET,  SO_RCVBUF,    0x123 },
    { SOL_SOCKET,  SO_SNDTIMEO,  0x123 },
    { SOL_SOCKET,  SO_RCVTIMEO,  0x123 },
    { IPPROTO_TCP, TCP_NODELAY,  1 },
};

/* A listener on the loopback with the options above set before it binds,
 * and the address it listens on */
static
SOCKET
Listen(
    _Out_ struct sockaddr_in *Address)
{
    SOCKET Listener;
    struct linger Linger;
    int Length = sizeof(*Address);
    ULONG i;
    int Result;

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (Listener == INVALID_SOCKET)
        return INVALID_SOCKET;

    for (i = 0; i < RTL_NUMBER_OF(IntOptions); i++)
    {
        Result = setsockopt(Listener, IntOptions[i].Level, IntOptions[i].Name,
                            (const char *)&IntOptions[i].Value, sizeof(IntOptions[i].Value));
        ok(Result == 0, "Option %lu: setsockopt failed with %d\n", i, WSAGetLastError());
    }
    Linger.l_onoff = 1;
    Linger.l_linger = 555;
    Result = setsockopt(Listener, SOL_SOCKET, SO_LINGER, (const char *)&Linger, sizeof(Linger));
    ok(Result == 0, "setsockopt(SO_LINGER) failed with %d\n", WSAGetLastError());

    ZeroMemory(Address, sizeof(*Address));
    Address->sin_family = AF_INET;
    Address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Result = bind(Listener, (struct sockaddr *)Address, sizeof(*Address));
    ok(Result == 0, "bind failed with %d\n", WSAGetLastError());
    Result = getsockname(Listener, (struct sockaddr *)Address, &Length);
    ok(Result == 0, "getsockname failed with %d\n", WSAGetLastError());
    Result = listen(Listener, 1);
    ok(Result == 0, "listen failed with %d\n", WSAGetLastError());

    return Listener;
}

/* Accepted has the options Listen() set */
static
VOID
CheckInherited(
    _In_ PCSTR Kind,
    _In_ SOCKET Accepted)
{
    struct linger Linger;
    int Value, Length;
    ULONG i;
    int Result;

    for (i = 0; i < RTL_NUMBER_OF(IntOptions); i++)
    {
        Value = 0;
        Length = sizeof(Value);
        Result = getsockopt(Accepted, IntOptions[i].Level, IntOptions[i].Name, (char *)&Value, &Length);
        ok(Result == 0, "%s, option %lu: getsockopt failed with %d\n", Kind, i, WSAGetLastError());
        ok(Value == IntOptions[i].Value, "%s, option %lu: got %#x\n", Kind, i, Value);
    }

    ZeroMemory(&Linger, sizeof(Linger));
    Length = sizeof(Linger);
    Result = getsockopt(Accepted, SOL_SOCKET, SO_LINGER, (char *)&Linger, &Length);
    ok(Result == 0, "%s: getsockopt(SO_LINGER) failed with %d\n", Kind, WSAGetLastError());
    ok(Linger.l_onoff == 1, "%s: l_onoff is %u\n", Kind, Linger.l_onoff);
    ok(Linger.l_linger == 555, "%s: l_linger is %u\n", Kind, Linger.l_linger);
}

static
VOID
TestAccept(void)
{
    SOCKET Listener, Client, Accepted;
    struct sockaddr_in Address;
    int Result;

    Listener = Listen(&Address);
    if (Listener == INVALID_SOCKET)
        return;

    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Client != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0, "connect failed with %d\n", WSAGetLastError());
    Accepted = accept(Listener, NULL, NULL);
    ok(Accepted != INVALID_SOCKET, "accept failed with %d\n", WSAGetLastError());
    if (Accepted != INVALID_SOCKET)
    {
        CheckInherited("accept", Accepted);
        closesocket(Accepted);
    }

    closesocket(Client);
    closesocket(Listener);
}

/* An AcceptEx socket has the listener's options once it is told whose it
 * is, by SO_UPDATE_ACCEPT_CONTEXT */
static
VOID
TestAcceptEx(
    _In_ LPFN_ACCEPTEX pAcceptEx)
{
    SOCKET Listener, Client, Acceptor;
    struct sockaddr_in Address;
    OVERLAPPED Overlapped;
    CHAR Buffer[2 * ADDRESS_LENGTH];
    DWORD Received, Wait;
    BOOL Success;
    int Result, Length;
    BOOLEAN NoDelay;

    Listener = Listen(&Address);
    if (Listener == INVALID_SOCKET)
        return;

    Acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Acceptor != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Success = pAcceptEx(Listener, Acceptor, Buffer, 0, ADDRESS_LENGTH, ADDRESS_LENGTH,
                        &Received, &Overlapped);
    ok(Success || WSAGetLastError() == ERROR_IO_PENDING, "AcceptEx failed with %d\n", WSAGetLastError());

    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Client != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0, "connect failed with %d\n", WSAGetLastError());

    Wait = WaitForSingleObject(Overlapped.hEvent, 10 * 1000);
    ok(Wait == WAIT_OBJECT_0, "AcceptEx did not complete: %lu\n", Wait);
    if (Wait == WAIT_OBJECT_0)
    {
        /* The listening socket it names must be a socket */
        Result = setsockopt(Acceptor, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&Listener, 1);
        ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT,
           "A short SO_UPDATE_ACCEPT_CONTEXT returned %d, error %d\n", Result, WSAGetLastError());

        Result = setsockopt(Acceptor, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&Listener, sizeof(Listener));
        ok(Result == 0, "SO_UPDATE_ACCEPT_CONTEXT failed with %d\n", WSAGetLastError());
        CheckInherited("AcceptEx", Acceptor);

        /* What it was given is its own: the listener's is not changed */
        NoDelay = FALSE;
        Result = setsockopt(Acceptor, IPPROTO_TCP, TCP_NODELAY, (const char *)&NoDelay, sizeof(NoDelay));
        ok(Result == 0, "setsockopt(TCP_NODELAY) failed with %d\n", WSAGetLastError());
        NoDelay = 0xFF;
        Length = sizeof(NoDelay);
        Result = getsockopt(Listener, IPPROTO_TCP, TCP_NODELAY, (char *)&NoDelay, &Length);
        ok(Result == 0, "getsockopt(TCP_NODELAY) failed with %d\n", WSAGetLastError());
        ok(NoDelay == 1, "The listener's TCP_NODELAY is %u\n", NoDelay);
    }
    else
    {
        CancelIo((HANDLE)Listener);
    }

    closesocket(Client);
    closesocket(Acceptor);
    closesocket(Listener);
    CloseHandle(Overlapped.hEvent);
}

/* getsockopt(TCP_NODELAY) answers what was set, as a BOOLEAN: the option is
 * the first byte of what setsockopt() was given, of any length but 0 */
static
VOID
TestGetNoDelay(void)
{
    SOCKET Socket;
    int Value, Result, Length;

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (Socket == INVALID_SOCKET)
        return;

    Value = 0xFF;
    Length = sizeof(Value);
    Result = getsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (char *)&Value, &Length);
    ok(Result == 0, "getsockopt(TCP_NODELAY) failed with %d\n", WSAGetLastError());
    ok(Length == sizeof(BOOLEAN), "The option is %d bytes\n", Length);
    ok((Value & 0xFF) == 0, "TCP_NODELAY is %#x on a new socket\n", Value);

    Value = 1;
    Result = setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&Value, 1);
    ok(Result == 0, "setsockopt(TCP_NODELAY) failed with %d\n", WSAGetLastError());
    Value = 0xFF;
    Length = sizeof(Value);
    Result = getsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (char *)&Value, &Length);
    ok(Result == 0, "getsockopt(TCP_NODELAY) failed with %d\n", WSAGetLastError());
    ok(Value == 1, "TCP_NODELAY is %#x\n", Value);

    Value = 0x100;
    Result = setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&Value, sizeof(Value));
    ok(Result == 0, "setsockopt(TCP_NODELAY) failed with %d\n", WSAGetLastError());
    Value = 0xFF;
    Length = sizeof(Value);
    Result = getsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (char *)&Value, &Length);
    ok(Result == 0, "getsockopt(TCP_NODELAY) failed with %d\n", WSAGetLastError());
    ok(Value == 0, "TCP_NODELAY is %#x after a set to 0x100\n", Value);

    Value = 1;
    Result = setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&Value, 0);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT,
       "setsockopt(TCP_NODELAY) of 0 bytes returned %d, error %d\n", Result, WSAGetLastError());

    Length = 0;
    Result = getsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, (char *)&Value, &Length);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT,
       "getsockopt(TCP_NODELAY) into 0 bytes returned %d, error %d\n", Result, WSAGetLastError());

    /* An unknown level is WSAEINVAL, an unknown option of a known level
     * WSAENOPROTOOPT */
    Length = sizeof(Value);
    Result = getsockopt(Socket, 0x8000, TCP_NODELAY, (char *)&Value, &Length);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL,
       "getsockopt of level 0x8000 returned %d, error %d\n", Result, WSAGetLastError());
    Length = sizeof(Value);
    Result = getsockopt(Socket, IPPROTO_TCP, 0x7777, (char *)&Value, &Length);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAENOPROTOOPT,
       "getsockopt of TCP option 0x7777 returned %d, error %d\n", Result, WSAGetLastError());

    closesocket(Socket);
}

START_TEST(acceptinherit)
{
    WSADATA WsaData;
    GUID AcceptExGuid = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX pAcceptEx = NULL;
    SOCKET Socket;
    DWORD Bytes;
    int Result;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Result == 0, "WSAStartup failed with %d\n", Result);
    if (Result)
        return;

    TestGetNoDelay();
    TestAccept();

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    Result = WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &AcceptExGuid, sizeof(AcceptExGuid),
                      &pAcceptEx, sizeof(pAcceptEx), &Bytes, NULL, NULL);
    closesocket(Socket);
    if (Result == 0 && pAcceptEx)
        TestAcceptEx(pAcceptEx);
    else
        skip("No AcceptEx: %d\n", WSAGetLastError());

    WSACleanup();
}
