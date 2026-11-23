/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test that the network still sends after the NIC ran out of transmit resources
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "ws2_32.h"
#include <iphlpapi.h>

#define FLOOD_THREADS 4
#define FLOOD_PACKETS 5000
#define FLOOD_SIZE 1400

static struct sockaddr_in DnsServer;
static volatile LONG FloodSent;

/* One DNS query (A record of reactos.org) to the configured server, TRUE when it answers */
static BOOL dns_roundtrip(USHORT id)
{
    static const BYTE question[] =
    {
        0x01, 0x00, /* RD */
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        7, 'r','e','a','c','t','o','s', 3, 'o','r','g', 0,
        0x00, 0x01, 0x00, 0x01
    };
    BYTE query[2 + sizeof(question)], reply[512];
    struct timeval timeout = { 5, 0 };
    fd_set set;
    SOCKET s;
    int ret, tries;
    BOOL answered = FALSE;

    query[0] = HIBYTE(id);
    query[1] = LOBYTE(id);
    memcpy(query + 2, question, sizeof(question));

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(s != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (s == INVALID_SOCKET)
        return FALSE;

    for (tries = 0; tries < 3 && !answered; tries++)
    {
        ret = sendto(s, (const char *)query, sizeof(query), 0, (struct sockaddr *)&DnsServer, sizeof(DnsServer));
        if (ret != sizeof(query))
            continue;
        for (;;)
        {
            FD_ZERO(&set);
            FD_SET(s, &set);
            if (select(0, &set, NULL, NULL, &timeout) != 1)
                break;
            ret = recv(s, (char *)reply, sizeof(reply), 0);
            if (ret >= 2 && reply[0] == query[0] && reply[1] == query[1])
            {
                answered = TRUE;
                break;
            }
        }
    }

    closesocket(s);
    return answered;
}

static DWORD WINAPI flood_thread(PVOID Context)
{
    struct sockaddr_in target = DnsServer;
    static char payload[FLOOD_SIZE];
    SOCKET s;
    int i;

    UNREFERENCED_PARAMETER(Context);

    /* The discard port of the DNS server: nothing answers, every packet goes out */
    target.sin_port = htons(9);
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return 1;
    for (i = 0; i < FLOOD_PACKETS; i++)
    {
        if (sendto(s, payload, sizeof(payload), 0, (struct sockaddr *)&target, sizeof(target)) == sizeof(payload))
            InterlockedIncrement(&FloodSent);
    }
    closesocket(s);
    return 0;
}

START_TEST(sendflood)
{
    PFIXED_INFO info;
    ULONG size = 0;
    HANDLE threads[FLOOD_THREADS];
    WSADATA wsa_data;
    ULONG i;

    if (GetNetworkParams(NULL, &size) != ERROR_BUFFER_OVERFLOW)
    {
        skip("GetNetworkParams failed\n");
        return;
    }
    info = HeapAlloc(GetProcessHeap(), 0, size);
    if (!info || GetNetworkParams(info, &size) != ERROR_SUCCESS)
    {
        skip("GetNetworkParams failed\n");
        HeapFree(GetProcessHeap(), 0, info);
        return;
    }
    DnsServer.sin_family = AF_INET;
    DnsServer.sin_port = htons(53);
    DnsServer.sin_addr.s_addr = inet_addr(info->DnsServerList.IpAddress.String);
    HeapFree(GetProcessHeap(), 0, info);
    if (DnsServer.sin_addr.s_addr == INADDR_NONE || DnsServer.sin_addr.s_addr == INADDR_ANY)
    {
        skip("No DNS server\n");
        return;
    }

    ok(WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0, "WSAStartup failed\n");

    if (!dns_roundtrip(0x5201))
    {
        skip("DNS server %s does not answer\n", inet_ntoa(DnsServer.sin_addr));
        WSACleanup();
        return;
    }

    /* Send faster than the NIC can, from several processors: its transmit
     * ring fills up and it refuses sends with NDIS_STATUS_RESOURCES */
    for (i = 0; i < FLOOD_THREADS; i++)
    {
        threads[i] = CreateThread(NULL, 0, flood_thread, NULL, 0, NULL);
        ok(threads[i] != NULL, "CreateThread failed: %lu\n", GetLastError());
    }
    for (i = 0; i < FLOOD_THREADS; i++)
    {
        if (!threads[i])
            continue;
        ok(WaitForSingleObject(threads[i], 120000) == WAIT_OBJECT_0, "Flood thread %lu did not finish\n", i);
        CloseHandle(threads[i]);
    }
    trace("%ld of %d datagrams sent\n", FloodSent, FLOOD_THREADS * FLOOD_PACKETS);

    /* Once the NIC caught up, sending must work again */
    ok(dns_roundtrip(0x5202), "No DNS answer after the send flood: the network stopped sending\n");

    WSACleanup();
}
