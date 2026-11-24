/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test sending from a UDP socket bound to a specific local address
 */

#include "ws2_32.h"
#include <iphlpapi.h>

/* A DNS query for "reactos.org" A: any well-formed answer proves the round trip */
static const UCHAR DnsQuery[] =
{
    0x52, 0x4f, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    7, 'r', 'e', 'a', 'c', 't', 'o', 's', 3, 'o', 'r', 'g', 0,
    0x00, 0x01, 0x00, 0x01
};

/* The first adapter with an IPv4 address, and the first DNS server */
static
BOOL
GetLocalAndDns(IN_ADDR *Local, IN_ADDR *Dns)
{
    ULONG Size = 0;
    PIP_ADAPTER_INFO Info, Adapter;
    PFIXED_INFO Params;
    BOOL Found = FALSE;

    if (GetAdaptersInfo(NULL, &Size) != ERROR_BUFFER_OVERFLOW)
        return FALSE;
    Info = malloc(Size);
    if (!Info)
        return FALSE;
    if (GetAdaptersInfo(Info, &Size) == ERROR_SUCCESS)
    {
        for (Adapter = Info; Adapter && !Found; Adapter = Adapter->Next)
        {
            Local->s_addr = inet_addr(Adapter->IpAddressList.IpAddress.String);
            Found = (Local->s_addr != 0 && Local->s_addr != INADDR_NONE);
        }
    }
    free(Info);
    if (!Found)
        return FALSE;

    Size = 0;
    if (GetNetworkParams(NULL, &Size) != ERROR_BUFFER_OVERFLOW)
        return FALSE;
    Params = malloc(Size);
    if (!Params)
        return FALSE;
    Found = FALSE;
    if (GetNetworkParams(Params, &Size) == ERROR_SUCCESS)
    {
        Dns->s_addr = inet_addr(Params->DnsServerList.IpAddress.String);
        Found = (Dns->s_addr != 0 && Dns->s_addr != INADDR_NONE);
    }
    free(Params);
    return Found;
}

static
SOCKET
BoundUdpSocket(IN_ADDR Address, USHORT Port)
{
    SOCKET Sock;
    struct sockaddr_in Addr = { AF_INET };
    int Ret;

    Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Sock != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Sock == INVALID_SOCKET)
        return Sock;

    Addr.sin_addr = Address;
    Addr.sin_port = Port;
    Ret = bind(Sock, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Ret == 0, "bind failed: %d\n", WSAGetLastError());
    return Sock;
}

/* recv() that gives up after 5 seconds instead of blocking the test forever */
static
int
RecvWithTimeout(SOCKET Sock, char *Buffer, int Length)
{
    fd_set ReadFds;
    struct timeval Timeout = { 5, 0 };
    int Ret;

    FD_ZERO(&ReadFds);
    FD_SET(Sock, &ReadFds);
    Ret = select(0, &ReadFds, NULL, NULL, &Timeout);
    if (Ret != 1)
    {
        if (Ret == 0)
            WSASetLastError(WSAETIMEDOUT);
        return SOCKET_ERROR;
    }
    return recv(Sock, Buffer, Length, 0);
}

static
void
TestLoopback(void)
{
    IN_ADDR Loopback;
    SOCKET Receiver, Sender;
    struct sockaddr_in To;
    int Len = sizeof(To);
    char Buffer[16];
    int Ret;

    Loopback.s_addr = htonl(INADDR_LOOPBACK);
    Receiver = BoundUdpSocket(Loopback, 0);
    Sender = BoundUdpSocket(Loopback, 0);
    if (Receiver == INVALID_SOCKET || Sender == INVALID_SOCKET)
        goto Cleanup;

    Ret = getsockname(Receiver, (struct sockaddr *)&To, &Len);
    ok(Ret == 0, "getsockname failed: %d\n", WSAGetLastError());
    Ret = sendto(Sender, "bound", 5, 0, (struct sockaddr *)&To, sizeof(To));
    ok(Ret == 5, "loopback sendto returned %d, error %d\n", Ret, WSAGetLastError());
    Ret = RecvWithTimeout(Receiver, Buffer, sizeof(Buffer));
    ok(Ret == 5, "loopback recv returned %d, error %d\n", Ret, WSAGetLastError());

Cleanup:
    if (Receiver != INVALID_SOCKET)
        closesocket(Receiver);
    if (Sender != INVALID_SOCKET)
        closesocket(Sender);
}

static
void
TestAdapterAddress(void)
{
    IN_ADDR Local, Dns;
    SOCKET Sock;
    struct sockaddr_in To = { AF_INET };
    UCHAR Answer[512];
    int Ret;

    if (!GetLocalAndDns(&Local, &Dns))
    {
        skip("No adapter with an IPv4 address and a DNS server\n");
        return;
    }

    /* Bound to the adapter's own address, the query must leave through that
     * adapter towards the DNS server, and its answer must come back */
    Sock = BoundUdpSocket(Local, 0);
    if (Sock == INVALID_SOCKET)
        return;

    To.sin_addr = Dns;
    To.sin_port = htons(53);
    Ret = sendto(Sock, (const char *)DnsQuery, sizeof(DnsQuery), 0, (struct sockaddr *)&To, sizeof(To));
    ok(Ret == sizeof(DnsQuery), "sendto from %s returned %d, error %d\n", inet_ntoa(Local), Ret, WSAGetLastError());

    Ret = RecvWithTimeout(Sock, (char *)Answer, sizeof(Answer));
    ok(Ret >= (int)sizeof(DnsQuery), "recv returned %d, error %d\n", Ret, WSAGetLastError());
    if (Ret >= 2)
        ok(Answer[0] == DnsQuery[0] && Answer[1] == DnsQuery[1], "answer id %02x%02x\n", Answer[0], Answer[1]);

    closesocket(Sock);
}

START_TEST(sendbound)
{
    WSADATA WsaData;

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");
    TestLoopback();
    TestAdapterAddress();
    WSACleanup();
}
