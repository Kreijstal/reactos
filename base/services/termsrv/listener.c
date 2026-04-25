/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Disabled-by-default TCP listener scaffold
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#include "cliprdr.h"
#include "listener.h"
#include "rdpbcgr.h"

#define TERMSRV_LISTEN_ENV_NAME L"REACTOS_TERMSRV_LISTEN"
#define TERMSRV_LISTEN_PORT 3389
#define TERMSRV_LISTEN_BACKLOG 4
#define TERMSRV_SELECT_TIMEOUT_MS 250
#define TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID 1001
#define TERMSRV_CLIPRDR_SCAFFOLD_CHANNEL_ID 1007
#define TERMSRV_CLIPRDR_SCAFFOLD_FORMAT_ID 13
#define TERMSRV_MCS_MAX_CHANNEL_JOIN_REQUESTS 4

static const UCHAR TermSrvMcsConnectResponsePayload[] =
{
    /* Placeholder Connect-Response body; rdpbcgr.c wraps it in TPKT/X.224. */
    0x7f, 0x66, 0x03, 0x01, 0x02
};

typedef enum _TERMSRV_PACKET_PLACEHOLDER
{
    TermSrvPacketUnknown,
    TermSrvPacketTpkt,
    TermSrvPacketFastPath
} TERMSRV_PACKET_PLACEHOLDER;

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

static VOID
TermSrvLogFailure(
    _In_z_ const CHAR *Message)
{
    OutputDebugStringA("termsrv: ");
    OutputDebugStringA(Message);
    OutputDebugStringA("\n");
}

static VOID
TermSrvLogSocketFailure(
    _In_z_ const CHAR *Operation)
{
    CHAR Message[128];

    _snprintf(Message,
              sizeof(Message),
              "%s failed: WSA error %u",
              Operation,
              (unsigned int)WSAGetLastError());
    Message[sizeof(Message) - 1] = '\0';
    TermSrvLogFailure(Message);
}

static VOID
TermSrvLogRdpBcgrFailure(
    _In_z_ const CHAR *Operation,
    _In_ TERMSRV_RDPBCGR_RESULT Result)
{
    CHAR Message[128];

    _snprintf(Message,
              sizeof(Message),
              "%s failed: %s",
              Operation,
              TermSrvRdpBcgrResultName(Result));
    Message[sizeof(Message) - 1] = '\0';
    TermSrvLogFailure(Message);
}

static PCSTR
TermSrvCliprdrResultName(
    _In_ TERMSRV_CLIPRDR_RESULT Result)
{
    switch (Result)
    {
        case TermSrvCliprdrSuccess: return "Success";
        case TermSrvCliprdrNeedMoreData: return "NeedMoreData";
        case TermSrvCliprdrBufferTooSmall: return "BufferTooSmall";
        case TermSrvCliprdrInvalidHeader: return "InvalidHeader";
        case TermSrvCliprdrInvalidLength: return "InvalidLength";
        case TermSrvCliprdrUnsupportedPdu: return "UnsupportedPdu";
        case TermSrvCliprdrFormatNotAvailable: return "FormatNotAvailable";
        default: return "Unknown";
    }
}

static VOID
TermSrvLogCliprdrFailure(
    _In_z_ const CHAR *Operation,
    _In_ TERMSRV_CLIPRDR_RESULT Result)
{
    CHAR Message[128];

    _snprintf(Message,
              sizeof(Message),
              "%s failed: %s",
              Operation,
              TermSrvCliprdrResultName(Result));
    Message[sizeof(Message) - 1] = '\0';
    TermSrvLogFailure(Message);
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

static BOOL
TermSrvSendPacket(
    _In_ SOCKET Client,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ SIZE_T Length)
{
    INT Sent;

    Sent = send(Client, (const char *)Buffer, (INT)Length, 0);
    if (Sent == SOCKET_ERROR)
    {
        TermSrvLogSocketFailure("send");
        return FALSE;
    }

    if ((SIZE_T)Sent != Length)
    {
        TermSrvLogFailure("send returned a short write");
        return FALSE;
    }

    return TRUE;
}

static BOOL
TermSrvReceiveTpkt(
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _Out_ INT *Received)
{
    *Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, BufferLength);
    if (*Received <= 0)
    {
        TermSrvLogFailure("expected packet was not received");
        return FALSE;
    }

    if (TermSrvIdentifyPacketPlaceholder(Buffer, *Received) != TermSrvPacketTpkt)
    {
        TermSrvLogFailure("expected TPKT packet");
        return FALSE;
    }

    return TRUE;
}

static BOOL
TermSrvInitializeCliprdrScaffold(
    _Out_ TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Out_ TERMSRV_CLIPRDR_DUMMY_BACKEND *Dummy,
    _Out_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    static const UCHAR ClipboardText[] =
    {
        'R', 0, 'e', 0, 'a', 0, 'c', 0, 't', 0, 'O', 0, 'S', 0, ' ', 0,
        't', 0, 'e', 0, 'r', 0, 'm', 0, 's', 0, 'r', 0, 'v', 0, 0, 0
    };
    TERMSRV_CLIPRDR_RESULT ClipResult;

    ClipResult = TermSrvCliprdrChannelInit(Channel);
    if (ClipResult != TermSrvCliprdrSuccess)
    {
        TermSrvLogCliprdrFailure("cliprdr channel init", ClipResult);
        return FALSE;
    }

    ClipResult = TermSrvCliprdrDummyBackendInit(Dummy, Backend);
    if (ClipResult != TermSrvCliprdrSuccess)
    {
        TermSrvLogCliprdrFailure("cliprdr dummy backend init", ClipResult);
        return FALSE;
    }

    ClipResult = TermSrvCliprdrBackendSetData(Backend,
                                              TERMSRV_CLIPRDR_SCAFFOLD_FORMAT_ID,
                                              ClipboardText,
                                              sizeof(ClipboardText));
    if (ClipResult != TermSrvCliprdrSuccess)
    {
        TermSrvLogCliprdrFailure("cliprdr dummy backend data set", ClipResult);
        return FALSE;
    }

    return TRUE;
}

static BOOL
TermSrvRouteOptionalCliprdrPacket(
    _In_ SOCKET Client,
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    UCHAR CliprdrReply[512];
    SIZE_T BytesWritten;
    TERMSRV_CLIPRDR_RESULT ClipResult;

    if (TermSrvIdentifyPacketPlaceholder(Buffer, Received) != TermSrvPacketTpkt)
    {
        TermSrvLogFailure("expected TPKT packet");
        return FALSE;
    }

    ClipResult = TermSrvCliprdrRouteMcsSendData(Buffer,
                                                (SIZE_T)Received,
                                                Channel,
                                                Backend,
                                                CliprdrReply,
                                                sizeof(CliprdrReply),
                                                &BytesWritten);
    if (BytesWritten != 0)
    {
        /*
         * This listener scaffold does not have an MCS Send Data writer yet, so
         * return the cliprdr virtual-channel payload directly for now.
         */
        if (!TermSrvSendPacket(Client, CliprdrReply, BytesWritten))
            return FALSE;
    }

    if (ClipResult == TermSrvCliprdrSuccess ||
        ClipResult == TermSrvCliprdrFormatNotAvailable)
    {
        return TRUE;
    }

    TermSrvLogCliprdrFailure("cliprdr MCS Send Data route", ClipResult);
    return FALSE;
}

static BOOL
TermSrvConsumeOptionalClientInfoAndCliprdrPacket(
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    INT Received;
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD ClientInfo;

    Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, BufferLength);
    if (Received <= 0)
        return TRUE;

    if (TermSrvIdentifyPacketPlaceholder(Buffer, Received) != TermSrvPacketTpkt)
    {
        TermSrvLogFailure("expected TPKT packet");
        return FALSE;
    }

    Result = TermSrvRdpBcgrParseClientInfoPayload(Buffer,
                                                 (SIZE_T)Received,
                                                 &ClientInfo);
    if (Result == TermSrvRdpBcgrSuccess)
    {
        Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, BufferLength);
        if (Received <= 0)
            return TRUE;
    }

    return TermSrvRouteOptionalCliprdrPacket(Client,
                                            Buffer,
                                            Received,
                                            Channel,
                                            Backend);
}

static BOOL
TermSrvRunEarlyMcsPhase(
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _Out_writes_bytes_(ReplyLength) UCHAR *Reply,
    _In_ SIZE_T ReplyLength)
{
    INT Received;
    SIZE_T BytesWritten;
    ULONG JoinCount;
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_RDPBCGR_MCS_ATTACH_USER_CONFIRM AttachUserConfirm;
    TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD SecurityExchange;
    TERMSRV_CLIPRDR_BACKEND CliprdrBackend;
    TERMSRV_CLIPRDR_CHANNEL CliprdrChannel;
    TERMSRV_CLIPRDR_DUMMY_BACKEND CliprdrDummy;
    BOOL HaveSecurityExchange;

    if (!TermSrvInitializeCliprdrScaffold(&CliprdrChannel,
                                          &CliprdrDummy,
                                          &CliprdrBackend))
    {
        return FALSE;
    }

    if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
        return FALSE;

    Result = TermSrvRdpBcgrParseMcsErectDomainRequest(Buffer, (SIZE_T)Received);
    if (Result != TermSrvRdpBcgrSuccess)
    {
        TermSrvLogRdpBcgrFailure("MCS erect domain request parse", Result);
        return FALSE;
    }

    if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
        return FALSE;

    Result = TermSrvRdpBcgrParseMcsAttachUserRequest(Buffer, (SIZE_T)Received);
    if (Result != TermSrvRdpBcgrSuccess)
    {
        TermSrvLogRdpBcgrFailure("MCS attach user request parse", Result);
        return FALSE;
    }

    ZeroMemory(&AttachUserConfirm, sizeof(AttachUserConfirm));
    AttachUserConfirm.UserChannelId = TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID;

    Result = TermSrvRdpBcgrWriteMcsAttachUserConfirm(Reply,
                                                     ReplyLength,
                                                     &AttachUserConfirm,
                                                     &BytesWritten);
    if (Result != TermSrvRdpBcgrSuccess)
    {
        TermSrvLogRdpBcgrFailure("MCS attach user confirm write", Result);
        return FALSE;
    }

    if (!TermSrvSendPacket(Client, Reply, BytesWritten))
        return FALSE;

    HaveSecurityExchange = FALSE;

    for (JoinCount = 0; JoinCount < TERMSRV_MCS_MAX_CHANNEL_JOIN_REQUESTS; JoinCount++)
    {
        TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_REQUEST ChannelJoinRequest;
        TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_CONFIRM ChannelJoinConfirm;

        Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, BufferLength);
        if (Received <= 0)
            break;

        if (TermSrvIdentifyPacketPlaceholder(Buffer, Received) != TermSrvPacketTpkt)
        {
            TermSrvLogFailure("expected TPKT packet");
            return FALSE;
        }

        Result = TermSrvRdpBcgrParseMcsChannelJoinRequest(Buffer,
                                                         (SIZE_T)Received,
                                                         &ChannelJoinRequest);
        if (Result != TermSrvRdpBcgrSuccess)
        {
            Result = TermSrvRdpBcgrParseSecurityExchangePayload(Buffer,
                                                                (SIZE_T)Received,
                                                                &SecurityExchange);
            if (Result == TermSrvRdpBcgrSuccess)
            {
                HaveSecurityExchange = TRUE;
                break;
            }

            TermSrvLogRdpBcgrFailure("security exchange parse", Result);
            return FALSE;
        }

        ZeroMemory(&ChannelJoinConfirm, sizeof(ChannelJoinConfirm));
        ChannelJoinConfirm.Initiator = ChannelJoinRequest.Initiator;
        ChannelJoinConfirm.RequestedChannelId = ChannelJoinRequest.ChannelId;
        ChannelJoinConfirm.ConfirmedChannelId = ChannelJoinRequest.ChannelId;

        if (ChannelJoinRequest.ChannelId == TERMSRV_CLIPRDR_SCAFFOLD_CHANNEL_ID)
        {
            TERMSRV_CLIPRDR_RESULT ClipResult;

            ClipResult = TermSrvCliprdrAssignChannelId(&CliprdrChannel,
                                                       ChannelJoinRequest.ChannelId);
            if (ClipResult != TermSrvCliprdrSuccess)
            {
                TermSrvLogCliprdrFailure("cliprdr scaffold channel assign", ClipResult);
                return FALSE;
            }
        }

        Result = TermSrvRdpBcgrWriteMcsChannelJoinConfirm(Reply,
                                                          ReplyLength,
                                                          &ChannelJoinConfirm,
                                                          &BytesWritten);
        if (Result != TermSrvRdpBcgrSuccess)
        {
            TermSrvLogRdpBcgrFailure("MCS channel join confirm write", Result);
            return FALSE;
        }

        if (!TermSrvSendPacket(Client, Reply, BytesWritten))
            return FALSE;
    }

    if (!HaveSecurityExchange)
    {
        Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, BufferLength);
        if (Received <= 0)
            return TRUE;

        if (TermSrvIdentifyPacketPlaceholder(Buffer, Received) != TermSrvPacketTpkt)
        {
            TermSrvLogFailure("expected TPKT packet");
            return FALSE;
        }

        Result = TermSrvRdpBcgrParseSecurityExchangePayload(Buffer,
                                                            (SIZE_T)Received,
                                                            &SecurityExchange);
        if (Result != TermSrvRdpBcgrSuccess)
        {
            TermSrvLogRdpBcgrFailure("security exchange parse", Result);
            return FALSE;
        }
    }

    if (!TermSrvConsumeOptionalClientInfoAndCliprdrPacket(Client,
                                                         StopEvent,
                                                         Buffer,
                                                         BufferLength,
                                                         &CliprdrChannel,
                                                         &CliprdrBackend))
    {
        return FALSE;
    }

    return TRUE;
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
        TERMSRV_RDPBCGR_RESULT Result;
        TERMSRV_RDPBCGR_MCS_CONNECT_INITIAL ConnectInitial;

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

            Result = TermSrvRdpBcgrWriteConnectionConfirm(Reply,
                                                          sizeof(Reply),
                                                          &Confirm,
                                                          &ReplyLength);
            if (Result != TermSrvRdpBcgrSuccess)
            {
                TermSrvLogRdpBcgrFailure("connection confirm write", Result);
                goto Cleanup;
            }

            if (!TermSrvSendPacket(Client, Reply, ReplyLength))
                goto Cleanup;

            Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, sizeof(Buffer));
            if (Received > 0 &&
                TermSrvIdentifyPacketPlaceholder(Buffer, Received) == TermSrvPacketTpkt &&
                TermSrvRdpBcgrParseMcsConnectInitial(Buffer,
                                                    (SIZE_T)Received,
                                                    &ConnectInitial) == TermSrvRdpBcgrSuccess)
            {
                Result = TermSrvRdpBcgrWriteMcsConnectResponse(Reply,
                                                               sizeof(Reply),
                                                               TermSrvMcsConnectResponsePayload,
                                                               sizeof(TermSrvMcsConnectResponsePayload),
                                                               &ReplyLength);
                if (Result != TermSrvRdpBcgrSuccess)
                {
                    TermSrvLogRdpBcgrFailure("MCS connect response write", Result);
                    goto Cleanup;
                }

                if (!TermSrvSendPacket(Client, Reply, ReplyLength))
                    goto Cleanup;

                if (!TermSrvRunEarlyMcsPhase(Client,
                                             StopEvent,
                                             Buffer,
                                             sizeof(Buffer),
                                             Reply,
                                             sizeof(Reply)))
                {
                    goto Cleanup;
                }
            }
        }
    }

Cleanup:
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
