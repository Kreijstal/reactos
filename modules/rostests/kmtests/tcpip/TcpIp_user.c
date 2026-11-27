/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         User mode part of the TcpIp.sys test suite
 * PROGRAMMER:      Jérôme Gardou <jerome.gardou@reactos.org>
 */

#include <kmt_test.h>
#include <winsock2.h>

#include "tcpip.h"

static
DWORD
LoadTcpIpTestDriver(void)
{
    DWORD Error;

    /* Start the special-purpose driver */
    Error = KmtLoadAndOpenDriver(L"TcpIp", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return Error;

    return ERROR_SUCCESS;
}

static
void
UnloadTcpIpTestDriver(void)
{
    /* Stop the driver. */
    KmtCloseDriver();
    KmtUnloadDriver();
}

START_TEST(TcpIpTdi)
{
    DWORD Error;

    Error = LoadTcpIpTestDriver();
    ok_eq_int(Error, 0);
    if (Error)
        return;

    Error = KmtSendToDriver(IOCTL_TEST_TDI);
    ok_eq_ulong(Error, ERROR_SUCCESS);

    UnloadTcpIpTestDriver();
}

static
DWORD
WINAPI
AcceptProc(
    _In_ LPVOID Parameter)
{
    WORD WinsockVersion;
    WSADATA WsaData;
    int Error;
    SOCKET ListenSocket, AcceptSocket;
    struct sockaddr_in ListenAddress, AcceptAddress;
    int AcceptAddressLength;
    HANDLE ReadyToConnectEvent = (HANDLE)Parameter;

    /* Initialize winsock */
    WinsockVersion = MAKEWORD(2, 0);
    Error = WSAStartup(WinsockVersion, &WsaData);
    ok_eq_int(Error, 0);

    ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(ListenSocket != INVALID_SOCKET, "socket failed\n");

    ZeroMemory(&ListenAddress, sizeof(ListenAddress));
    ListenAddress.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
    ListenAddress.sin_port = htons(TEST_CONNECT_SERVER_PORT);
    ListenAddress.sin_family = AF_INET;

    Error = bind(ListenSocket, (struct sockaddr*)&ListenAddress, sizeof(ListenAddress));
    ok_eq_int(Error, 0);

    Error = listen(ListenSocket, 1);
    ok_eq_int(Error, 0);

    SetEvent(ReadyToConnectEvent);

    AcceptAddressLength = sizeof(AcceptAddress);
    AcceptSocket = accept(ListenSocket, (struct sockaddr*)&AcceptAddress, &AcceptAddressLength);
    ok(AcceptSocket != INVALID_SOCKET, "accept failed\n");
    ok_eq_long(AcceptAddressLength, sizeof(AcceptAddress));
    ok_eq_hex(AcceptAddress.sin_addr.S_un.S_addr, inet_addr("127.0.0.1"));
    ok_eq_hex(AcceptAddress.sin_port, htons(TEST_CONNECT_CLIENT_PORT));

    return 0;
}

START_TEST(TcpIpConnect)
{
    HANDLE AcceptThread;
    HANDLE ReadyToConnectEvent;
    DWORD Error;

    ReadyToConnectEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ok(ReadyToConnectEvent != NULL, "CreateEvent failed\n");

    AcceptThread = CreateThread(NULL, 0, AcceptProc, (PVOID)ReadyToConnectEvent, 0, NULL);
    ok(AcceptThread != NULL, "CreateThread failed\n");

    WaitForSingleObject(ReadyToConnectEvent, INFINITE);

    LoadTcpIpTestDriver();

    Error = KmtSendToDriver(IOCTL_TEST_CONNECT);
    ok_eq_ulong(Error, ERROR_SUCCESS);

    Error = WaitForSingleObject(AcceptThread, 10 * 1000);
    ok(Error == WAIT_OBJECT_0, "AcceptThread timed out\n");

    UnloadTcpIpTestDriver();

    WSACleanup();
}

/* The peer of the kernel side of TcpIpMdlChain: expects its data, answers */
static
DWORD
WINAPI
MdlChainPeerProc(
    _In_ LPVOID Parameter)
{
    WSADATA WsaData;
    SOCKET ListenSocket, PeerSocket;
    struct sockaddr_in Address;
    DWORD Timeout = 10 * 1000;
    CHAR Buffer[32];
    int Received, Result;
    HANDLE ReadyEvent = (HANDLE)Parameter;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok_eq_int(Result, 0);

    ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(ListenSocket != INVALID_SOCKET, "socket failed\n");

    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
    Address.sin_port = htons(TEST_MDL_CHAIN_TCP_SERVER_PORT);
    Result = bind(ListenSocket, (struct sockaddr *)&Address, sizeof(Address));
    ok_eq_int(Result, 0);
    Result = listen(ListenSocket, 1);
    ok_eq_int(Result, 0);

    SetEvent(ReadyEvent);

    PeerSocket = accept(ListenSocket, NULL, NULL);
    ok(PeerSocket != INVALID_SOCKET, "accept failed with %d\n", WSAGetLastError());
    closesocket(ListenSocket);
    if (PeerSocket == INVALID_SOCKET)
        return 0;
    setsockopt(PeerSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&Timeout, sizeof(Timeout));

    /* All of the chain, in its order */
    Received = 0;
    while (Received < (int)sizeof(TEST_MDL_CHAIN_TCP_DATA) - 1)
    {
        Result = recv(PeerSocket, Buffer + Received, sizeof(Buffer) - Received, 0);
        if (Result <= 0)
            break;
        Received += Result;
    }
    ok_eq_int(Received, (int)sizeof(TEST_MDL_CHAIN_TCP_DATA) - 1);
    ok(!memcmp(Buffer, TEST_MDL_CHAIN_TCP_DATA, sizeof(TEST_MDL_CHAIN_TCP_DATA) - 1),
       "Received %.*s\n", Received, Buffer);

    Result = send(PeerSocket, TEST_MDL_CHAIN_TCP_ANSWER, sizeof(TEST_MDL_CHAIN_TCP_ANSWER) - 1, 0);
    ok_eq_int(Result, (int)sizeof(TEST_MDL_CHAIN_TCP_ANSWER) - 1);

    /* Until the kernel side closes */
    while (recv(PeerSocket, Buffer, sizeof(Buffer), 0) > 0)
        ;
    closesocket(PeerSocket);
    WSACleanup();

    return 0;
}

START_TEST(TcpIpMdlChain)
{
    HANDLE PeerThread, ReadyEvent;
    DWORD Error;

    ReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ok(ReadyEvent != NULL, "CreateEvent failed\n");

    PeerThread = CreateThread(NULL, 0, MdlChainPeerProc, (PVOID)ReadyEvent, 0, NULL);
    ok(PeerThread != NULL, "CreateThread failed\n");

    Error = WaitForSingleObject(ReadyEvent, 10 * 1000);
    ok_eq_ulong(Error, WAIT_OBJECT_0);

    Error = LoadTcpIpTestDriver();
    if (Error)
        return;

    Error = KmtSendToDriver(IOCTL_TEST_MDL_CHAIN);
    ok_eq_ulong(Error, ERROR_SUCCESS);

    Error = WaitForSingleObject(PeerThread, 30 * 1000);
    ok(Error == WAIT_OBJECT_0, "The peer thread timed out\n");

    UnloadTcpIpTestDriver();
}
