/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Disabled-by-default TCP listener scaffold
 */

#include <winsock2.h>
#include <windows.h>

#include "listener.h"
#include "rdpbcgr.h"

#define TERMSRV_LISTEN_ENV_NAME L"REACTOS_TERMSRV_LISTEN"
#define TERMSRV_LISTEN_PORT 3389
#define TERMSRV_LISTEN_BACKLOG 4
#define TERMSRV_SELECT_TIMEOUT_MS 250

typedef enum _TERMSRV_PACKET_PLACEHOLDER
{
    TermSrvPacketUnknown,
    TermSrvPacketTpkt,
    TermSrvPacketFastPath
} TERMSRV_PACKET_PLACEHOLDER;

typedef enum _TERMSRV_CLIENT_PACKET_CLASS
{
    TermSrvClientPacketAbsent,
    TermSrvClientPacketInvalid,
    TermSrvClientPacketMcsConnectInitial
} TERMSRV_CLIENT_PACKET_CLASS;

static BOOL
TermSrvListenerEnabled(VOID)
{
    WCHAR Value[2];

    return (GetEnvironmentVariableW(TERMSRV_LISTEN_ENV_NAME, Value, ARRAYSIZE(Value)) == 1 &&
            Value[0] == L'1');
}

static TERMSRV_PACKET_PLACEHOLDER
TermSrvIdentifyPacketPlaceholder(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ INT Length)
{
    if (Length >= 4 && Buffer[0] == 3)
        return TermSrvPacketTpkt;

    if (Length >= 2 && (Buffer[0] & 0x03) == 0)
        return TermSrvPacketFastPath;

    return TermSrvPacketUnknown;
}

static BOOL
TermSrvStopRequested(
    _In_ HANDLE StopEvent)
{
    return WaitForSingleObject(StopEvent, 0) == WAIT_OBJECT_0;
}

static INT
TermSrvSetNonBlocking(
    _In_ SOCKET Socket)
{
    u_long NonBlocking = 1;

    return ioctlsocket(Socket, FIONBIO, &NonBlocking);
}

static INT
TermSrvReceiveWithTimeout(
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength)
{
    fd_set ReadSet;
    TIMEVAL Timeout;
    INT Received;
    INT SelectResult;

    Received = recv(Client, (char *)Buffer, BufferLength, 0);
    if (Received != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK)
        return Received;

    FD_ZERO(&ReadSet);
    FD_SET(Client, &ReadSet);
    Timeout.tv_sec = 0;
    Timeout.tv_usec = TERMSRV_SELECT_TIMEOUT_MS * 1000;

    SelectResult = select(0, &ReadSet, NULL, NULL, &Timeout);
    if (SelectResult <= 0 || !FD_ISSET(Client, &ReadSet) || TermSrvStopRequested(StopEvent))
        return 0;

    return recv(Client, (char *)Buffer, BufferLength, 0);
}

static TERMSRV_CLIENT_PACKET_CLASS
TermSrvClassifyClientPacket(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ INT Length)
{
    TERMSRV_RDPBCGR_MCS_CONNECT_INITIAL ConnectInitial;

    if (Length <= 0)
        return TermSrvClientPacketAbsent;

    if (TermSrvIdentifyPacketPlaceholder(Buffer, Length) != TermSrvPacketTpkt)
        return TermSrvClientPacketInvalid;

    if (TermSrvRdpBcgrParseMcsConnectInitial(Buffer,
                                            (SIZE_T)Length,
                                            &ConnectInitial) == TermSrvRdpBcgrSuccess)
    {
        return TermSrvClientPacketMcsConnectInitial;
    }

    return TermSrvClientPacketInvalid;
}

static VOID
TermSrvHandleClient(
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent)
{
    UCHAR Buffer[512];
    INT Received;

    TermSrvSetNonBlocking(Client);

    Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, sizeof(Buffer));

    if (Received > 0)
    {
        TERMSRV_RDPBCGR_CONNECTION_REQUEST Request;
        TERMSRV_RDPBCGR_CONNECTION_CONFIRM Confirm;
        UCHAR Reply[32];
        SIZE_T ReplyLength;

        if (TermSrvIdentifyPacketPlaceholder(Buffer, Received) == TermSrvPacketTpkt &&
            TermSrvRdpBcgrParseConnectionRequest(Buffer,
                                                (SIZE_T)Received,
                                                &Request) == TermSrvRdpBcgrSuccess)
        {
            ZeroMemory(&Confirm, sizeof(Confirm));
            Confirm.DestinationReference = Request.SourceReference;
            Confirm.ClassOption = Request.ClassOption;
            Confirm.HasNegotiation = Request.HasNegotiation;
            if (Confirm.HasNegotiation)
            {
                Confirm.Negotiation.Type = TermSrvRdpBcgrNegResponse;
                Confirm.Negotiation.Length = 8;
                Confirm.Negotiation.Protocols = TermSrvRdpBcgrProtocolStandard;
            }

            if (TermSrvRdpBcgrWriteConnectionConfirm(Reply,
                                                     sizeof(Reply),
                                                     &Confirm,
                                                     &ReplyLength) == TermSrvRdpBcgrSuccess)
            {
                send(Client, (const char *)Reply, (INT)ReplyLength, 0);
            }

            Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, sizeof(Buffer));
            if (TermSrvClassifyClientPacket(Buffer, Received) ==
                TermSrvClientPacketMcsConnectInitial)
            {
                /* The scaffold recognizes this envelope but stops before MCS processing. */
            }
        }
    }

    closesocket(Client);
}

static DWORD
TermSrvListenerLoop(
    _In_ SOCKET ListenSocket,
    _In_ HANDLE StopEvent)
{
    fd_set ReadSet;
    TIMEVAL Timeout;
    INT SelectResult;
    SOCKET Client;

    while (!TermSrvStopRequested(StopEvent))
    {
        FD_ZERO(&ReadSet);
        FD_SET(ListenSocket, &ReadSet);
        Timeout.tv_sec = 0;
        Timeout.tv_usec = TERMSRV_SELECT_TIMEOUT_MS * 1000;

        SelectResult = select(0, &ReadSet, NULL, NULL, &Timeout);
        if (SelectResult == SOCKET_ERROR)
            return ERROR_NETWORK_UNREACHABLE;

        if (SelectResult == 0 || !FD_ISSET(ListenSocket, &ReadSet))
            continue;

        for (;;)
        {
            Client = accept(ListenSocket, NULL, NULL);
            if (Client == INVALID_SOCKET)
            {
                if (WSAGetLastError() == WSAEWOULDBLOCK)
                    break;

                return ERROR_NETWORK_UNREACHABLE;
            }

            TermSrvHandleClient(Client, StopEvent);

            if (TermSrvStopRequested(StopEvent))
                break;
        }
    }

    return ERROR_SUCCESS;
}

DWORD
TermSrvListenerRun(
    _In_ HANDLE StopEvent)
{
    WSADATA WsaData;
    SOCKET ListenSocket;
    SOCKADDR_IN Address;
    DWORD Result;

    if (!TermSrvListenerEnabled())
    {
        WaitForSingleObject(StopEvent, INFINITE);
        return ERROR_SUCCESS;
    }

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    if (Result != 0)
        return Result;

    ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ListenSocket == INVALID_SOCKET)
    {
        Result = WSAGetLastError();
        WSACleanup();
        return Result;
    }

    if (TermSrvSetNonBlocking(ListenSocket) == SOCKET_ERROR)
    {
        Result = WSAGetLastError();
        closesocket(ListenSocket);
        WSACleanup();
        return Result;
    }

    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_ANY);
    Address.sin_port = htons(TERMSRV_LISTEN_PORT);

    if (bind(ListenSocket, (SOCKADDR *)&Address, sizeof(Address)) == SOCKET_ERROR)
    {
        Result = WSAGetLastError();
        closesocket(ListenSocket);
        WSACleanup();
        return Result;
    }

    if (listen(ListenSocket, TERMSRV_LISTEN_BACKLOG) == SOCKET_ERROR)
    {
        Result = WSAGetLastError();
        closesocket(ListenSocket);
        WSACleanup();
        return Result;
    }

    Result = TermSrvListenerLoop(ListenSocket, StopEvent);

    closesocket(ListenSocket);
    WSACleanup();
    return Result;
}
