/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a pending AcceptEx can be cancelled, also once it took a connection and waits for its data
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "ws2_32.h"
#include <mswsock.h>

/*
 * An AcceptEx that asks for the first block of data completes only once the
 * connection it took has sent something.  Until then it is a pending request
 * like any other: CancelIo() on the listening socket, issued by the thread
 * that started it, completes it at once with ERROR_OPERATION_ABORTED.  The
 * same holds for one still waiting for a connection.
 */

#define ACCEPTEX_ADDRESS_LENGTH (sizeof(struct sockaddr_in) + 16)
#define ACCEPTEX_DATA_LENGTH 64
#define ACCEPTEX_CANCEL_MS 5000

static
void
TestCancel(LPFN_ACCEPTEX AcceptEx, BOOL Connect, DWORD DataLength)
{
    const char *Kind = Connect ? "connected" : "waiting";
    SOCKET Listener, Acceptor, Client = INVALID_SOCKET;
    struct sockaddr_in Address;
    int AddressLength, Result;
    char Buffer[ACCEPTEX_DATA_LENGTH + 2 * ACCEPTEX_ADDRESS_LENGTH];
    WSAOVERLAPPED Overlapped;
    DWORD Bytes, Wait, Error;
    BOOL Success, Pending = FALSE;

    memset(&Overlapped, 0, sizeof(Overlapped));
    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET, "%s: socket failed: %d\n", Kind, WSAGetLastError());
    Acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Acceptor != INVALID_SOCKET, "%s: socket failed: %d\n", Kind, WSAGetLastError());
    if (Listener == INVALID_SOCKET || Acceptor == INVALID_SOCKET)
        goto Cleanup;

    memset(&Address, 0, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Result = bind(Listener, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0, "%s: bind failed: %d\n", Kind, WSAGetLastError());
    AddressLength = sizeof(Address);
    Result = getsockname(Listener, (struct sockaddr *)&Address, &AddressLength);
    ok(Result == 0, "%s: getsockname failed: %d\n", Kind, WSAGetLastError());
    Result = listen(Listener, 5);
    ok(Result == 0, "%s: listen failed: %d\n", Kind, WSAGetLastError());

    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Success = AcceptEx(Listener, Acceptor, Buffer, DataLength,
                       ACCEPTEX_ADDRESS_LENGTH, ACCEPTEX_ADDRESS_LENGTH,
                       &Bytes, &Overlapped);
    Error = WSAGetLastError();
    ok(!Success && Error == ERROR_IO_PENDING, "%s: AcceptEx did not pend: %d, %lu\n", Kind, Success, Error);
    if (Success || Error != ERROR_IO_PENDING)
        goto Cleanup;
    Pending = TRUE;

    if (Connect)
    {
        Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(Client != INVALID_SOCKET, "%s: socket failed: %d\n", Kind, WSAGetLastError());
        Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
        ok(Result == 0, "%s: connect failed: %d\n", Kind, WSAGetLastError());

        /* Let the AcceptEx take the connection: it now waits for the data,
         * which the client does not send */
        Sleep(300);
    }

    Wait = WaitForSingleObject(Overlapped.hEvent, 0);
    ok(Wait == WAIT_TIMEOUT, "%s: AcceptEx completed before the cancel: %lu, 0x%lx\n",
       Kind, Wait, (ULONG)Overlapped.Internal);

    Success = CancelIo((HANDLE)Listener);
    ok(Success, "%s: CancelIo failed: %lu\n", Kind, GetLastError());

    Wait = WaitForSingleObject(Overlapped.hEvent, ACCEPTEX_CANCEL_MS);
    ok(Wait == WAIT_OBJECT_0, "%s: the cancelled AcceptEx did not complete: %lu\n", Kind, Wait);
    if (Wait == WAIT_OBJECT_0)
    {
        Success = WSAGetOverlappedResult(Listener, &Overlapped, &Bytes, FALSE, &Error);
        Error = WSAGetLastError();
        ok(!Success && Error == ERROR_OPERATION_ABORTED,
           "%s: the cancelled AcceptEx returned %d, %lu\n", Kind, Success, Error);
    }

Cleanup:
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    if (Acceptor != INVALID_SOCKET)
        closesocket(Acceptor);
    if (Listener != INVALID_SOCKET)
        closesocket(Listener);
    if (Pending)
    {
        /* The AcceptEx writes to Overlapped and Buffer until it completes:
         * closing the sockets ends it, if the cancel did not */
        Wait = WaitForSingleObject(Overlapped.hEvent, ACCEPTEX_CANCEL_MS);
        ok(Wait == WAIT_OBJECT_0, "%s: the AcceptEx never completed: %lu\n", Kind, Wait);
    }
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
}

START_TEST(acceptexcancel)
{
    WSADATA WsaData;
    GUID AcceptExGuid = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX AcceptEx = NULL;
    SOCKET Socket;
    DWORD Bytes;
    int Result;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Result == 0, "WSAStartup failed: %d\n", Result);

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    Result = WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                      &AcceptExGuid, sizeof(AcceptExGuid),
                      &AcceptEx, sizeof(AcceptEx), &Bytes, NULL, NULL);
    closesocket(Socket);
    if (Result != 0 || !AcceptEx)
    {
        skip("No AcceptEx: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }

    /* Waiting for a connection, with and without a data block */
    TestCancel(AcceptEx, FALSE, 0);
    TestCancel(AcceptEx, FALSE, ACCEPTEX_DATA_LENGTH);

    /* Holding a connection, waiting for its data */
    TestCancel(AcceptEx, TRUE, ACCEPTEX_DATA_LENGTH);

    WSACleanup();
}
