/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that sockets duplicated by WSADuplicateSocket() on two threads at once are one socket
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "ws2_32.h"

/*
 * Every descriptor WSADuplicateSocket() hands out for a socket names that
 * same socket, so the state of the socket is the same through all of them: a
 * socket made to listen through one is listening (SO_ACCEPTCONN) through the
 * others.  That has to hold when two threads duplicate a socket at the same
 * time, and in particular when that is the first time it is duplicated.
 *
 * Each iteration duplicates a fresh socket on two threads at once, makes it
 * listen through the original descriptor and asks both duplicates.
 */

#define DUPRACE_ITERATIONS 400

typedef struct _DUPLICATOR
{
    SOCKET Socket;
    volatile LONG *Arrived;
    WSAPROTOCOL_INFOW ProtocolInfo;
    int Result;
    int Error;
} DUPLICATOR, *PDUPLICATOR;

static
DWORD
WINAPI
DuplicatorThread(PVOID Parameter)
{
    PDUPLICATOR Duplicator = Parameter;

    /* Both threads duplicate as close to the same moment as we can get */
    InterlockedIncrement(Duplicator->Arrived);
    while (*Duplicator->Arrived < 2)
        YieldProcessor();

    Duplicator->Result = WSADuplicateSocketW(Duplicator->Socket,
                                             GetCurrentProcessId(),
                                             &Duplicator->ProtocolInfo);
    Duplicator->Error = WSAGetLastError();
    return 0;
}

static
BOOL
RunIteration(ULONG Iteration, PULONG Mismatches)
{
    DUPLICATOR Duplicators[2];
    HANDLE Threads[2];
    SOCKET Socket, Duplicates[2] = { INVALID_SOCKET, INVALID_SOCKET };
    volatile LONG Arrived = 0;
    struct sockaddr_in Address;
    BOOL Listening, Mismatch = FALSE;
    int Length, Result;
    DWORD Wait;
    ULONG i;

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "%lu: socket failed: %d\n", Iteration, WSAGetLastError());
    if (Socket == INVALID_SOCKET)
        return FALSE;

    memset(&Address, 0, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Result = bind(Socket, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0, "%lu: bind failed: %d\n", Iteration, WSAGetLastError());

    memset(Duplicators, 0, sizeof(Duplicators));
    for (i = 0; i < 2; i++)
    {
        Duplicators[i].Socket = Socket;
        Duplicators[i].Arrived = &Arrived;
        Threads[i] = CreateThread(NULL, 0, DuplicatorThread, &Duplicators[i], 0, NULL);
        ok(Threads[i] != NULL, "%lu: CreateThread failed: %lu\n", Iteration, GetLastError());
        if (!Threads[i])
        {
            /* Let the other one through */
            InterlockedIncrement(&Arrived);
        }
    }

    Wait = WaitForMultipleObjects(2, Threads, TRUE, 30000);
    ok(Wait == WAIT_OBJECT_0, "%lu: the duplicating threads did not finish: %lu\n", Iteration, Wait);
    for (i = 0; i < 2; i++)
    {
        if (Threads[i])
            CloseHandle(Threads[i]);
    }
    if (Wait != WAIT_OBJECT_0)
        return FALSE;

    for (i = 0; i < 2; i++)
    {
        ok(Duplicators[i].Result == 0, "%lu: WSADuplicateSocketW %lu failed: %d\n",
           Iteration, i, Duplicators[i].Error);
        if (Duplicators[i].Result != 0)
            continue;

        Duplicates[i] = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                   &Duplicators[i].ProtocolInfo, 0, 0);
        ok(Duplicates[i] != INVALID_SOCKET, "%lu: WSASocketW %lu failed: %d\n",
           Iteration, i, WSAGetLastError());
    }

    Result = listen(Socket, 1);
    ok(Result == 0, "%lu: listen failed: %d\n", Iteration, WSAGetLastError());

    for (i = 0; i < 2; i++)
    {
        if (Duplicates[i] == INVALID_SOCKET)
            continue;

        Listening = FALSE;
        Length = sizeof(Listening);
        Result = getsockopt(Duplicates[i], SOL_SOCKET, SO_ACCEPTCONN, (char *)&Listening, &Length);
        ok(Result == 0, "%lu: getsockopt %lu failed: %d\n", Iteration, i, WSAGetLastError());
        if (Result == 0 && !Listening)
            Mismatch = TRUE;

        closesocket(Duplicates[i]);
    }

    closesocket(Socket);

    if (Mismatch)
        (*Mismatches)++;
    return TRUE;
}

START_TEST(duprace)
{
    WSADATA WsaData;
    ULONG Iteration, Mismatches = 0;
    int Result;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Result == 0, "WSAStartup failed: %d\n", Result);

    for (Iteration = 0; Iteration < DUPRACE_ITERATIONS; Iteration++)
    {
        if (!RunIteration(Iteration, &Mismatches))
            break;
    }

    ok(Mismatches == 0, "a duplicate did not see the socket listen in %lu of %lu iterations\n",
       Mismatches, Iteration);
    trace("%lu iterations, %lu mismatches\n", Iteration, Mismatches);

    WSACleanup();
}
