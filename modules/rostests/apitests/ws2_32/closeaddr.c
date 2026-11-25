/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that closesocket() releases the socket's address while other threads still have requests on it
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "ws2_32.h"
#include <mswsock.h>

/*
 * closesocket() gives up the socket's local address at once (MSDN,
 * closesocket: the socket's resources are released; only a connected stream
 * socket finishing its graceful close in the background keeps its
 * connection).  That holds when other threads are still in a select() on the
 * socket or have an accept or a receive pending on it: those requests
 * complete, but the address is not theirs to keep.  So binding a new socket
 * exclusively to the same address right after closesocket() returns must
 * succeed.
 *
 * To make that deterministic, every thread runs on one processor and the
 * threads that hold the requests run at idle priority while the closing
 * thread runs at time-critical priority: none of them runs -- and so none of
 * their completed requests lets go of the socket -- until the closing thread
 * has tried the bind.
 */

#define CLOSEADDR_PORT 22231
#define CLOSEADDR_SELECT_MS 5000

typedef struct _HOLDER
{
    SOCKET Socket;
    BOOL Stream;
    BOOL Select;
    LPFN_ACCEPTEX AcceptEx;
    SOCKET AcceptSocket;
    WSAOVERLAPPED Overlapped;
    char Buffer[64 + 2 * (sizeof(struct sockaddr_in) + 16)];
    struct sockaddr_in From;
    int FromLength;
    HANDLE Ready;
    HANDLE Done;
    int Result;
    int Error;
} HOLDER, *PHOLDER;

static
DWORD
WINAPI
HolderThread(PVOID Parameter)
{
    PHOLDER Holder = Parameter;
    fd_set ReadSet, ExceptSet;
    struct timeval Timeout;
    WSABUF Buffer;
    DWORD Bytes, Flags;

    if (Holder->Select)
    {
        FD_ZERO(&ReadSet);
        FD_ZERO(&ExceptSet);
        FD_SET(Holder->Socket, &ReadSet);
        FD_SET(Holder->Socket, &ExceptSet);
        Timeout.tv_sec = CLOSEADDR_SELECT_MS / 1000;
        Timeout.tv_usec = 0;

        SetEvent(Holder->Ready);
        Holder->Result = select(0, &ReadSet, NULL, &ExceptSet, &Timeout);
        Holder->Error = WSAGetLastError();
        return 0;
    }

    if (Holder->Stream)
    {
        if (!Holder->AcceptEx(Holder->Socket, Holder->AcceptSocket, Holder->Buffer, 0,
                              sizeof(struct sockaddr_in) + 16,
                              sizeof(struct sockaddr_in) + 16,
                              &Bytes, &Holder->Overlapped))
        {
            Holder->Error = WSAGetLastError();
        }
    }
    else
    {
        Buffer.buf = Holder->Buffer;
        Buffer.len = 64;
        Flags = 0;
        Holder->FromLength = sizeof(Holder->From);
        if (WSARecvFrom(Holder->Socket, &Buffer, 1, NULL, &Flags,
                        (struct sockaddr *)&Holder->From, &Holder->FromLength,
                        &Holder->Overlapped, NULL) == SOCKET_ERROR)
        {
            Holder->Error = WSAGetLastError();
        }
    }

    SetEvent(Holder->Ready);

    /* The request belongs to this thread; keep it alive until the main
     * thread is done */
    WaitForSingleObject(Holder->Done, INFINITE);
    return 0;
}

static
SOCKET
BindExclusive(BOOL Stream, int *Error)
{
    struct sockaddr_in Address;
    SOCKET Socket;
    BOOL On = TRUE;

    Socket = socket(AF_INET, Stream ? SOCK_STREAM : SOCK_DGRAM, Stream ? IPPROTO_TCP : IPPROTO_UDP);
    if (Socket == INVALID_SOCKET)
    {
        *Error = WSAGetLastError();
        return INVALID_SOCKET;
    }

    if (setsockopt(Socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&On, sizeof(On)) == SOCKET_ERROR)
    {
        *Error = WSAGetLastError();
        closesocket(Socket);
        return INVALID_SOCKET;
    }

    memset(&Address, 0, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = inet_addr("127.0.0.1");
    Address.sin_port = htons(CLOSEADDR_PORT);

    if (bind(Socket, (struct sockaddr *)&Address, sizeof(Address)) == SOCKET_ERROR)
    {
        *Error = WSAGetLastError();
        closesocket(Socket);
        return INVALID_SOCKET;
    }

    *Error = 0;
    return Socket;
}

static
void
TestCloseReleasesAddress(BOOL Stream)
{
    const char *Kind = Stream ? "tcp" : "udp";
    HOLDER Holders[2];
    HANDLE Threads[2];
    SOCKET Socket, Rebound;
    GUID AcceptExGuid = WSAID_ACCEPTEX;
    DWORD Bytes, Wait;
    int Error, Result, OldPriority;
    DWORD_PTR OldAffinity;
    ULONG i;

    Socket = BindExclusive(Stream, &Error);
    ok(Socket != INVALID_SOCKET, "%s: bind failed: %d\n", Kind, Error);
    if (Socket == INVALID_SOCKET)
        return;

    if (Stream)
    {
        Result = listen(Socket, 5);
        ok(Result == 0, "tcp: listen failed: %d\n", WSAGetLastError());
    }

    memset(Holders, 0, sizeof(Holders));
    for (i = 0; i < 2; i++)
    {
        Holders[i].Socket = Socket;
        Holders[i].Stream = Stream;
        Holders[i].Select = (i == 0);
        Holders[i].AcceptSocket = INVALID_SOCKET;
        Holders[i].Ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        Holders[i].Done = CreateEventW(NULL, TRUE, FALSE, NULL);
        Holders[i].Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    }

    if (Stream)
    {
        Result = WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                          &AcceptExGuid, sizeof(AcceptExGuid),
                          &Holders[1].AcceptEx, sizeof(Holders[1].AcceptEx),
                          &Bytes, NULL, NULL);
        ok(Result == 0, "tcp: no AcceptEx: %d\n", WSAGetLastError());
        Holders[1].AcceptSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(Holders[1].AcceptSocket != INVALID_SOCKET, "tcp: socket failed: %d\n", WSAGetLastError());
    }

    /* One processor for all of us, and the holders only run when we wait */
    OldAffinity = SetThreadAffinityMask(GetCurrentThread(), 1);
    ok(OldAffinity != 0, "SetThreadAffinityMask failed: %lu\n", GetLastError());
    OldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    for (i = 0; i < 2; i++)
    {
        Threads[i] = CreateThread(NULL, 0, HolderThread, &Holders[i], CREATE_SUSPENDED, NULL);
        ok(Threads[i] != NULL, "CreateThread failed: %lu\n", GetLastError());
        SetThreadAffinityMask(Threads[i], 1);
        SetThreadPriority(Threads[i], THREAD_PRIORITY_IDLE);
        ResumeThread(Threads[i]);
        WaitForSingleObject(Holders[i].Ready, INFINITE);
    }

    /* Let the select reach AFD and both requests go pending */
    Sleep(200);
    ok(Holders[1].Error == WSA_IO_PENDING, "%s: %s did not pend: %d\n",
       Kind, Stream ? "AcceptEx" : "WSARecvFrom", Holders[1].Error);

    Result = closesocket(Socket);
    ok(Result == 0, "%s: closesocket failed: %d\n", Kind, WSAGetLastError());

    /* No other thread of ours has run since the close */
    Rebound = BindExclusive(Stream, &Error);
    ok(Rebound != INVALID_SOCKET, "%s: the address is still in use right after closesocket: %d\n", Kind, Error);

    SetThreadPriority(GetCurrentThread(), OldPriority);
    if (OldAffinity)
        SetThreadAffinityMask(GetCurrentThread(), OldAffinity);

    for (i = 0; i < 2; i++)
        SetEvent(Holders[i].Done);

    Wait = WaitForMultipleObjects(2, Threads, TRUE, CLOSEADDR_SELECT_MS * 2);
    ok(Wait == WAIT_OBJECT_0, "%s: the holder threads did not finish: %lu\n", Kind, Wait);

    /* The close ended the select at once, reporting the socket */
    ok(Holders[0].Result != 0, "%s: select timed out instead of seeing the close\n", Kind);

    /* The pending request completed as cancelled */
    Wait = WaitForSingleObject(Holders[1].Overlapped.hEvent, 0);
    ok(Wait == WAIT_OBJECT_0, "%s: the pending request did not complete: %lu\n", Kind, Wait);
    if (Wait == WAIT_OBJECT_0)
    {
        ok(Holders[1].Overlapped.Internal != STATUS_SUCCESS &&
           Holders[1].Overlapped.Internal != STATUS_PENDING,
           "%s: the pending request completed with 0x%lx\n", Kind, (ULONG)Holders[1].Overlapped.Internal);
    }

    if (Rebound != INVALID_SOCKET)
    {
        /* And the new socket owns the address for good */
        if (Stream)
        {
            Result = listen(Rebound, 5);
            ok(Result == 0, "tcp: listen on the rebound socket failed: %d\n", WSAGetLastError());
        }
        closesocket(Rebound);
    }

    for (i = 0; i < 2; i++)
    {
        CloseHandle(Threads[i]);
        CloseHandle(Holders[i].Ready);
        CloseHandle(Holders[i].Done);
        CloseHandle(Holders[i].Overlapped.hEvent);
        if (Holders[i].AcceptSocket != INVALID_SOCKET)
            closesocket(Holders[i].AcceptSocket);
    }
}

START_TEST(closeaddr)
{
    WSADATA WsaData;
    int Err;

    Err = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Err == 0, "WSAStartup failed: %d\n", Err);

    TestCloseReleasesAddress(TRUE);
    TestCloseReleasesAddress(FALSE);

    WSACleanup();
}
