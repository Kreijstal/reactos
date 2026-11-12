/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that a flood of large ICMP echo requests does not wedge the network stack
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <apitest.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

/*
 * Each 32000-byte echo request leaves as ~22 IP fragments, every one of them
 * an NDIS packet descriptor from tcpip's packet pool (2000 descriptors).
 * Flooding the default gateway with more than that many fragments (here
 * ~55000) exposed NDIS calling a serialized miniport's MiniportSend
 * concurrently with its MiniportHandleInterrupt: e1000's transmit ring
 * wedged, every later send was queued for good, the pool ran dry and every
 * further echo failed with ERROR_NO_SYSTEM_RESOURCES (or timed out).
 */
#define FLOOD_COUNT        2500
#define FLOOD_SIZE         32000
#define FLOOD_TIMEOUT      1000
#define MAX_CONSECUTIVE_FAILURES 20

static
BOOL
GetDefaultGateway(IPAddr *Gateway, PDWORD IfIndex)
{
    PMIB_IPFORWARDTABLE Table;
    ULONG Size = 0;
    DWORD Error, i;
    BOOL Found = FALSE;

    Error = GetIpForwardTable(NULL, &Size, FALSE);
    if (Error != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;

    Table = malloc(Size);
    if (!Table)
        return FALSE;

    if (GetIpForwardTable(Table, &Size, FALSE) == NO_ERROR)
    {
        for (i = 0; i < Table->dwNumEntries; i++)
        {
            if (Table->table[i].dwForwardDest == 0 &&
                Table->table[i].dwForwardMask == 0 &&
                Table->table[i].dwForwardNextHop != 0)
            {
                *Gateway = Table->table[i].dwForwardNextHop;
                *IfIndex = Table->table[i].dwForwardIfIndex;
                Found = TRUE;
                break;
            }
        }
    }

    free(Table);
    return Found;
}

static
BOOL
GetBroadcastAddress(DWORD IfIndex, IPAddr *Broadcast)
{
    PMIB_IPADDRTABLE Table;
    ULONG Size = 0;
    DWORD i;
    BOOL Found = FALSE;

    if (GetIpAddrTable(NULL, &Size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;

    Table = malloc(Size);
    if (!Table)
        return FALSE;

    if (GetIpAddrTable(Table, &Size, FALSE) == NO_ERROR)
    {
        for (i = 0; i < Table->dwNumEntries; i++)
        {
            if (Table->table[i].dwIndex == IfIndex)
            {
                *Broadcast = Table->table[i].dwAddr | ~Table->table[i].dwMask;
                Found = TRUE;
                break;
            }
        }
    }

    free(Table);
    return Found;
}

static
BOOL
SendEchoEx(HANDLE hIcmp, IPAddr Address, PVOID Data, WORD Size, PIP_OPTION_INFORMATION Options,
           PVOID Reply, DWORD ReplySize, DWORD Timeout, PDWORD Error)
{
    PICMP_ECHO_REPLY EchoReply = Reply;
    DWORD Ret;

    /* Room for exactly one reply: the echo completes as soon as it arrives
     * instead of collecting replies until the timeout */
    if (ReplySize > sizeof(ICMP_ECHO_REPLY) + Size + 8)
        ReplySize = sizeof(ICMP_ECHO_REPLY) + Size + 8;

    SetLastError(0xDEADBEEF);
    Ret = IcmpSendEcho(hIcmp, Address, Data, Size, Options, Reply, ReplySize, Timeout);
    *Error = GetLastError();
    if (Ret == 0)
        return FALSE;

    if (EchoReply->Status != IP_SUCCESS)
    {
        *Error = EchoReply->Status;
        return FALSE;
    }

    if (EchoReply->DataSize != Size)
    {
        *Error = ERROR_INVALID_DATA;
        return FALSE;
    }

    return TRUE;
}

static
BOOL
SendEcho(HANDLE hIcmp, IPAddr Address, PVOID Data, WORD Size,
         PVOID Reply, DWORD ReplySize, DWORD Timeout, PDWORD Error)
{
    IP_OPTION_INFORMATION Options = { 0 };

    Options.Ttl = 128;
    return SendEchoEx(hIcmp, Address, Data, Size, &Options, Reply, ReplySize, Timeout, Error);
}

/* Interface statistics queries (GetIfTable) reach the miniport as NDIS
 * requests: issue them while the flood keeps its send and interrupt
 * handlers busy */
static volatile LONG StopQueries;
static LONG Queries, QueryFailures;
static DWORD LastQueryError;

static
DWORD
WINAPI
QueryThread(PVOID Context)
{
    PMIB_IFTABLE Table;
    ULONG Size;
    DWORD Error;

    while (!StopQueries)
    {
        Size = 0;
        Error = GetIfTable(NULL, &Size, FALSE);
        if (Error == ERROR_INSUFFICIENT_BUFFER)
        {
            Table = malloc(Size);
            if (!Table)
                break;
            Error = GetIfTable(Table, &Size, FALSE);
            free(Table);
        }
        Queries++;
        if (Error != NO_ERROR)
        {
            QueryFailures++;
            LastQueryError = Error;
        }
    }

    return 0;
}

/* A second sender keeps sends overlapping the receive interrupt handling
 * on other processors: single-frame UDP datagrams to the subnet broadcast
 * address (a second ICMP sender would also receive the first one's
 * replies). Paced, so that it cannot by itself queue more sends than the
 * adapter drains and run tcpip's packet pool dry */
#define UDP_SIZE 1024
static volatile LONG StopUdp;
static LONG UdpSent;

static
DWORD
WINAPI
UdpThread(PVOID Context)
{
    IPAddr Broadcast = *(IPAddr *)Context;
    struct sockaddr_in To;
    SOCKET Socket;
    BOOL On = TRUE;
    PCHAR Data;

    Data = calloc(1, UDP_SIZE);
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!Data || Socket == INVALID_SOCKET)
        goto Cleanup;
    setsockopt(Socket, SOL_SOCKET, SO_BROADCAST, (PCHAR)&On, sizeof(On));

    ZeroMemory(&To, sizeof(To));
    To.sin_family = AF_INET;
    To.sin_port = htons(9); /* discard */
    To.sin_addr.s_addr = Broadcast;

    while (!StopUdp)
    {
        if (sendto(Socket, Data, UDP_SIZE, 0, (struct sockaddr *)&To, sizeof(To)) == UDP_SIZE)
            UdpSent++;
        Sleep(1);
    }

Cleanup:
    if (Socket != INVALID_SOCKET)
        closesocket(Socket);
    free(Data);
    return 0;
}

#define FLOOD_THREADS 1

typedef struct _FLOOD
{
    IPAddr Gateway;
    DWORD Sent, Replies, NoResources, OtherFailures, Consecutive, LastError;
} FLOOD, *PFLOOD;

static
DWORD
WINAPI
FloodThread(PVOID Context)
{
    PFLOOD Flood = Context;
    HANDLE hIcmp;
    PUCHAR Data;
    PVOID Reply;
    DWORD ReplySize = sizeof(ICMP_ECHO_REPLY) + FLOOD_SIZE + 8, Error, i;

    hIcmp = IcmpCreateFile();
    Data = malloc(FLOOD_SIZE);
    Reply = malloc(ReplySize);
    if (hIcmp == INVALID_HANDLE_VALUE || !Data || !Reply)
    {
        Flood->LastError = GetLastError();
        goto Cleanup;
    }
    for (i = 0; i < FLOOD_SIZE; i++)
        Data[i] = (UCHAR)('a' + i % 23);

    for (Flood->Sent = 0; Flood->Sent < FLOOD_COUNT; )
    {
        Flood->Sent++;
        if (SendEcho(hIcmp, Flood->Gateway, Data, FLOOD_SIZE, Reply, ReplySize, FLOOD_TIMEOUT, &Error))
        {
            Flood->Replies++;
            Flood->Consecutive = 0;
            continue;
        }

        Flood->LastError = Error;
        if (Error == ERROR_NO_SYSTEM_RESOURCES)
            Flood->NoResources++;
        else
            Flood->OtherFailures++;

        /* A wedged adapter fails every echo from now on: do not wait out
         * the remaining timeouts */
        if (++Flood->Consecutive >= MAX_CONSECUTIVE_FAILURES)
            break;
    }

Cleanup:
    free(Reply);
    free(Data);
    if (hIcmp != INVALID_HANDLE_VALUE)
        IcmpCloseHandle(hIcmp);
    return 0;
}

START_TEST(IcmpFlood)
{
    HANDLE hThread, hUdpThread;
    IPAddr Broadcast;
    DWORD IfIndex;
    WSADATA WsaData;
    HANDLE hFloodThreads[FLOOD_THREADS];
    FLOOD Floods[FLOOD_THREADS];
    HANDLE hIcmp;
    IPAddr Gateway;
    PUCHAR Data;
    PVOID Reply;
    DWORD ReplySize, Error, i, t;
    DWORD LastError = 0, LaterOk;
    DWORD Start;
    struct in_addr Address;

    if (!GetDefaultGateway(&Gateway, &IfIndex))
    {
        skip("No default gateway, no network to test with\n");
        return;
    }
    Address.S_un.S_addr = Gateway;
    trace("Default gateway %s\n", inet_ntoa(Address));

    hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE)
    {
        skip("IcmpCreateFile failed: %lu\n", GetLastError());
        return;
    }

    ReplySize = sizeof(ICMP_ECHO_REPLY) + FLOOD_SIZE + 8;
    Data = malloc(FLOOD_SIZE);
    Reply = malloc(ReplySize);
    if (!Data || !Reply)
    {
        skip("Out of memory\n");
        goto Cleanup;
    }
    for (i = 0; i < FLOOD_SIZE; i++)
        Data[i] = (UCHAR)('a' + i % 23);

    /* The gateway must answer at all, or there is nothing to test */
    if (!SendEcho(hIcmp, Gateway, Data, 32, Reply, ReplySize, 2000, &Error) &&
        !SendEcho(hIcmp, Gateway, Data, 32, Reply, ReplySize, 2000, &Error))
    {
        skip("The default gateway does not answer echo requests (%lu)\n", Error);
        goto Cleanup;
    }

    /* Without options the echo goes out with the default TTL */
    ok(SendEchoEx(hIcmp, Gateway, Data, 32, NULL, Reply, ReplySize, 2000, &Error) ||
       SendEchoEx(hIcmp, Gateway, Data, 32, NULL, Reply, ReplySize, 2000, &Error),
       "Echo without options was not answered: %lu\n", Error);

    hThread = CreateThread(NULL, 0, QueryThread, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");
    hUdpThread = NULL;
    if (GetBroadcastAddress(IfIndex, &Broadcast))
    {
        hUdpThread = CreateThread(NULL, 0, UdpThread, &Broadcast, 0, NULL);
        ok(hUdpThread != NULL, "CreateThread failed: %lu\n", GetLastError());
    }
    else
    {
        skip("No address on the gateway's interface, no UDP load\n");
    }

    Start = GetTickCount();
    ZeroMemory(Floods, sizeof(Floods));
    for (t = 0; t < FLOOD_THREADS; t++)
    {
        Floods[t].Gateway = Gateway;
        hFloodThreads[t] = CreateThread(NULL, 0, FloodThread, &Floods[t], 0, NULL);
        ok(hFloodThreads[t] != NULL, "CreateThread failed: %lu\n", GetLastError());
    }
    for (t = 0; t < FLOOD_THREADS; t++)
    {
        if (!hFloodThreads[t])
            continue;
        ok(WaitForSingleObject(hFloodThreads[t], 15 * 60 * 1000) == WAIT_OBJECT_0, "Flood %lu hangs\n", t);
        CloseHandle(hFloodThreads[t]);
    }
    for (t = 0; t < FLOOD_THREADS; t++)
    {
        trace("Flood %lu: %lu of %lu echoes of %u bytes answered in %lu ms (%lu out of resources, %lu other failures, last error %lu)\n",
              t, Floods[t].Replies, Floods[t].Sent, FLOOD_SIZE, GetTickCount() - Start,
              Floods[t].NoResources, Floods[t].OtherFailures, Floods[t].LastError);
    }

    if (hUdpThread)
    {
        StopUdp = TRUE;
        ok(WaitForSingleObject(hUdpThread, 30000) == WAIT_OBJECT_0, "UDP sends hang\n");
        CloseHandle(hUdpThread);
        trace("%ld UDP datagrams of %u bytes sent during the flood\n", UdpSent, UDP_SIZE);
    }
    WSACleanup();

    if (hThread)
    {
        StopQueries = TRUE;
        ok(WaitForSingleObject(hThread, 30000) == WAIT_OBJECT_0, "Interface queries hang\n");
        CloseHandle(hThread);
        trace("%ld interface table queries during the flood\n", Queries);
        ok(Queries > 0, "No interface table query completed\n");
        ok(QueryFailures == 0, "%ld interface table queries failed, last error %lu\n",
           QueryFailures, LastQueryError);
    }

    for (t = 0; t < FLOOD_THREADS; t++)
    {
        ok(Floods[t].Sent == FLOOD_COUNT, "Flood %lu stopped after %lu echoes: %lu consecutive failures, last error %lu\n",
           t, Floods[t].Sent, Floods[t].Consecutive, Floods[t].LastError);
        ok(Floods[t].NoResources == 0, "Flood %lu: %lu echoes failed with ERROR_NO_SYSTEM_RESOURCES\n",
           t, Floods[t].NoResources);
        ok(Floods[t].Replies >= Floods[t].Sent - Floods[t].Sent / 100, "Flood %lu: only %lu of %lu echoes were answered\n",
           t, Floods[t].Replies, Floods[t].Sent);
    }

    /* The stack must still work after the flood */
    LaterOk = 0;
    for (i = 0; i < 4; i++)
    {
        if (SendEcho(hIcmp, Gateway, Data, 32, Reply, ReplySize, 2000, &Error))
            LaterOk++;
        else
            LastError = Error;
    }
    ok(LaterOk == 4, "Only %lu of 4 small echoes after the flood were answered, last error %lu\n",
       LaterOk, LastError);

    LaterOk = 0;
    for (i = 0; i < 4; i++)
    {
        if (SendEcho(hIcmp, Gateway, Data, FLOOD_SIZE, Reply, ReplySize, 2000, &Error))
            LaterOk++;
        else
            LastError = Error;
    }
    ok(LaterOk == 4, "Only %lu of 4 large echoes after the flood were answered, last error %lu\n",
       LaterOk, LastError);

Cleanup:
    free(Reply);
    free(Data);
    IcmpCloseHandle(hIcmp);
}
