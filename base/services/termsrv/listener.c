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
#include "termsrv.h"

#define TERMSRV_LISTEN_ENV_NAME L"REACTOS_TERMSRV_LISTEN"
#define TERMSRV_NOAUTH_ENV_NAME L"REACTOS_TERMSRV_NOAUTH"
#define TERMSRV_LISTEN_PORT_ENV_NAME L"REACTOS_TERMSRV_PORT"
#define TERMSRV_LISTEN_PORT 3389
#define TERMSRV_LISTEN_BACKLOG 4
#define TERMSRV_SELECT_TIMEOUT_MS 2000
#define TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID 1001
#define TERMSRV_RDP_SCAFFOLD_SHARE_ID 0x000103e9
#define TERMSRV_MCS_SCAFFOLD_FIRST_STATIC_CHANNEL_ID 1004
#define TERMSRV_CLIPRDR_SCAFFOLD_CHANNEL_ID 1007
#define TERMSRV_MESSAGE_SCAFFOLD_CHANNEL_ID 1008
#define TERMSRV_CLIPRDR_SCAFFOLD_FORMAT_ID 13
#define TERMSRV_MCS_MAX_CHANNEL_JOIN_REQUESTS 7
#define TERMSRV_SEC_AUTODETECT_REQ 0x00001000
#define TERMSRV_SEC_AUTODETECT_RSP 0x00002000
#define TERMSRV_RDP_PDU_TYPE_CONFIRM_ACTIVE 0x03
#define TERMSRV_RDP_PDU_TYPE_DATA 0x07
#define TERMSRV_RDP_DATA_TYPE_CONTROL 0x14
#define TERMSRV_RDP_DATA_TYPE_INPUT 0x1c
#define TERMSRV_RDP_DATA_TYPE_SYNCHRONIZE 0x1f
#define TERMSRV_RDP_DATA_TYPE_UPDATE 0x02
#define TERMSRV_RDP_DATA_TYPE_FONT_LIST 0x27
#define TERMSRV_RDP_DATA_TYPE_FONT_MAP 0x28
#define TERMSRV_RDP_CONTROL_ACTION_REQUEST_CONTROL 1
#define TERMSRV_RDP_CONTROL_ACTION_GRANTED_CONTROL 2
#define TERMSRV_RDP_CONTROL_ACTION_COOPERATE 4
#define TERMSRV_SCAFFOLD_STATIC_CHANNEL_LIST_HEADER_LENGTH 1
#define TERMSRV_SCAFFOLD_STATIC_CHANNEL_DEF_LENGTH \
    (TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH + 4)

static const UCHAR TermSrvMcsConnectResponsePayload[] =
{
    /*
     * BER MCS Connect-Response with a minimal T.124 ConferenceCreateResponse.
     * The GCC user data advertises RDP 5.x core data, four static virtual
     * channels, and no standard RDP encryption.
     */
    0x7f, 0x66, 0x72,
    0x0a, 0x01, 0x00,
    0x02, 0x01, 0x00,
    0x30, 0x20,
    0x02, 0x02, 0x00, 0x22,
    0x02, 0x02, 0x00, 0x03,
    0x02, 0x02, 0x00, 0x00,
    0x02, 0x02, 0x00, 0x01,
    0x02, 0x02, 0x00, 0x00,
    0x02, 0x02, 0x00, 0x01,
    0x02, 0x02, 0xff, 0xff,
    0x02, 0x02, 0x00, 0x02,
    0x04, 0x40,
    0x00, 0x05, 0x00, 0x14, 0x7c, 0x00, 0x01,
    0x2a, 0x14, 0x76, 0x0a, 0x01, 0x01, 0x00,
    0x01, 0xc0, 0x00, 'M', 'c', 'D', 'n', 0x32,
    0x01, 0x0c, 0x10, 0x00,
    0x04, 0x00, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x03, 0x0c, 0x10, 0x00,
    0xeb, 0x03, 0x04, 0x00,
    0xec, 0x03, 0xed, 0x03,
    0xee, 0x03, 0xef, 0x03,
    0x04, 0x0c, 0x06, 0x00,
    0xf0, 0x03,
    0x02, 0x0c, 0x0c, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

typedef enum _TERMSRV_PACKET_PLACEHOLDER
{
    TermSrvPacketUnknown,
    TermSrvPacketTpkt,
    TermSrvPacketFastPath
} TERMSRV_PACKET_PLACEHOLDER;

typedef struct _TERMSRV_CLIENT_CONTEXT
{
    TERMSRV_SESSION_MANAGER SessionManager;
    TERMSRV_RDP_PEER Peer;
    TERMSRV_CLIPRDR_CHANNEL CliprdrChannel;
    TERMSRV_CLIPRDR_DUMMY_BACKEND CliprdrDummy;
    TERMSRV_CLIPRDR_BACKEND CliprdrBackend;
} TERMSRV_CLIENT_CONTEXT;

static BOOL
TermSrvListenerEnabled(VOID)
{
    WCHAR Value[2];

    return (GetEnvironmentVariableW(TERMSRV_LISTEN_ENV_NAME, Value, ARRAYSIZE(Value)) == 1 &&
            Value[0] == L'1');
}

static BOOL
TermSrvNoAuthEnabled(VOID)
{
    WCHAR Value[2];

    return (GetEnvironmentVariableW(TERMSRV_NOAUTH_ENV_NAME, Value, ARRAYSIZE(Value)) == 1 &&
            Value[0] == L'1');
}

static USHORT
TermSrvListenerPort(VOID)
{
    WCHAR Value[8];
    INT Port;

    if (GetEnvironmentVariableW(TERMSRV_LISTEN_PORT_ENV_NAME, Value, ARRAYSIZE(Value)) == 0)
        return TERMSRV_LISTEN_PORT;

    Port = _wtoi(Value);
    if (Port <= 0 || Port > 0xffff)
        return TERMSRV_LISTEN_PORT;

    return (USHORT)Port;
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

static VOID
TermSrvLogRdpPeerFailure(
    _In_z_ const CHAR *Operation,
    _In_ TERMSRV_RDP_STATUS Status)
{
    CHAR Message[128];

    _snprintf(Message,
              sizeof(Message),
              "%s failed: %s",
              Operation,
              TermSrvRdpStatusName(Status));
    Message[sizeof(Message) - 1] = '\0';
    TermSrvLogFailure(Message);
}

static VOID
TermSrvAdvancePeer(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_z_ PCSTR Packet,
    _In_z_ PCSTR Operation)
{
    TERMSRV_RDP_STATUS Status;

    if (Context == NULL)
        return;

    Status = TermSrvRdpPeerReceive(&Context->Peer, Packet);
    if (Status != TermSrvRdpSuccess)
        TermSrvLogRdpPeerFailure(Operation, Status);
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

static INT
TermSrvReceiveExactWithTimeout(
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength)
{
    INT Total;

    Total = 0;
    while (Total < BufferLength)
    {
        INT Received;

        Received = TermSrvReceiveWithTimeout(Client,
                                             StopEvent,
                                             &Buffer[Total],
                                             BufferLength - Total);
        if (Received <= 0)
            return Received;

        Total += Received;
    }

    return Total;
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
    USHORT PacketLength;

    *Received = TermSrvReceiveExactWithTimeout(Client,
                                               StopEvent,
                                               Buffer,
                                               4);
    if (*Received <= 0)
    {
        TermSrvLogFailure("expected packet was not received");
        return FALSE;
    }

    if (Buffer[0] != 3 || Buffer[1] != 0)
    {
        TermSrvLogFailure("expected TPKT header");
        return FALSE;
    }

    PacketLength = (USHORT)(((USHORT)Buffer[2] << 8) | Buffer[3]);
    if (PacketLength < 7 || PacketLength > (USHORT)BufferLength)
    {
        TermSrvLogFailure("invalid TPKT length");
        return FALSE;
    }

    *Received = TermSrvReceiveExactWithTimeout(Client,
                                               StopEvent,
                                               &Buffer[4],
                                               PacketLength - 4);
    if (*Received <= 0)
    {
        TermSrvLogFailure("expected TPKT body was not received");
        return FALSE;
    }

    *Received = PacketLength;

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
TermSrvLooksLikeScaffoldStaticChannelList(
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength)
{
    SIZE_T Count;
    SIZE_T ExpectedLength;

    if (Payload == NULL ||
        PayloadLength < TERMSRV_SCAFFOLD_STATIC_CHANNEL_LIST_HEADER_LENGTH)
    {
        return FALSE;
    }

    Count = Payload[0];
    if (Count > TERMSRV_RDPBCGR_MAX_STATIC_CHANNELS)
        return FALSE;

    ExpectedLength = TERMSRV_SCAFFOLD_STATIC_CHANNEL_LIST_HEADER_LENGTH +
                     Count * TERMSRV_SCAFFOLD_STATIC_CHANNEL_DEF_LENGTH;
    return PayloadLength == ExpectedLength;
}

static BOOL
TermSrvTryParseScaffoldStaticChannelList(
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _Out_ TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList)
{
    TERMSRV_RDPBCGR_RESULT Result;

    if (!TermSrvLooksLikeScaffoldStaticChannelList(Payload, PayloadLength))
        return FALSE;

    Result = TermSrvRdpBcgrParseStaticChannelList(Payload,
                                                  PayloadLength,
                                                  ChannelList);
    if (Result != TermSrvRdpBcgrSuccess)
    {
        TermSrvLogRdpBcgrFailure("scaffold static channel list parse", Result);
        return FALSE;
    }

    return TRUE;
}

static BOOL
TermSrvTryAssignCliprdrFromStaticChannelList(
    _Inout_ TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList)
{
    TERMSRV_CLIPRDR_RESULT ClipResult;

    if (ChannelList == NULL)
        return FALSE;

    ClipResult = TermSrvCliprdrAssignFromStaticChannelList(
        Channel,
        ChannelList,
        TERMSRV_MCS_SCAFFOLD_FIRST_STATIC_CHANNEL_ID);
    if (ClipResult == TermSrvCliprdrSuccess)
        return TRUE;

    TermSrvLogCliprdrFailure("cliprdr static channel list assign", ClipResult);
    return FALSE;
}

static BOOL
TermSrvRouteOptionalCliprdrPacket(
    _In_ SOCKET Client,
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    UCHAR CliprdrPayload[512];
    UCHAR WrappedReply[544];
    SIZE_T BytesWritten;
    SIZE_T WrappedBytesWritten;
    TERMSRV_CLIPRDR_RESULT ClipResult;
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD SendData;
    TERMSRV_RDPBCGR_RESULT Result;

    if (TermSrvIdentifyPacketPlaceholder(Buffer, Received) != TermSrvPacketTpkt)
    {
        TermSrvLogFailure("expected TPKT packet");
        return FALSE;
    }

    ClipResult = TermSrvCliprdrRouteMcsSendData(Buffer,
                                                (SIZE_T)Received,
                                                Channel,
                                                Backend,
                                                CliprdrPayload,
                                                sizeof(CliprdrPayload),
                                                &BytesWritten);
    if (BytesWritten != 0)
    {
        ZeroMemory(&SendData, sizeof(SendData));
        SendData.Initiator = TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID;
        SendData.ChannelId = (Channel->ChannelId != TERMSRV_CLIPRDR_INVALID_CHANNEL_ID) ?
                             Channel->ChannelId :
                             TERMSRV_CLIPRDR_SCAFFOLD_CHANNEL_ID;
        SendData.Priority = 0x70;
        SendData.Payload = CliprdrPayload;
        SendData.PayloadLength = BytesWritten;

        Result = TermSrvRdpBcgrWriteMcsSendDataPayload(WrappedReply,
                                                       sizeof(WrappedReply),
                                                       &SendData,
                                                       &WrappedBytesWritten);
        if (Result != TermSrvRdpBcgrSuccess)
        {
            TermSrvLogRdpBcgrFailure("cliprdr MCS Send Data write", Result);
            return FALSE;
        }

        if (!TermSrvSendPacket(Client, WrappedReply, WrappedBytesWritten))
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
TermSrvIsCliprdrMcsPacket(
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD SendData;

    if (Channel == NULL ||
        Channel->ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID ||
        TermSrvRdpBcgrParseMcsSendDataPayload(Buffer,
                                              (SIZE_T)Received,
                                              &SendData) != TermSrvRdpBcgrSuccess)
    {
        return FALSE;
    }

    return SendData.ChannelId == Channel->ChannelId;
}

static BOOL
TermSrvWriteServerGlobalPayload(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T PacketLength;
    SIZE_T BodyLength;
    SIZE_T PayloadOffset;

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    if (Buffer == NULL || Payload == NULL || PayloadLength > 0x7fff)
        return FALSE;

    BodyLength = ((PayloadLength > 0x7f) ? 8 : 7) + PayloadLength;
    PacketLength = 7 + BodyLength;
    if (BufferLength < PacketLength || PacketLength > 0xffff)
        return FALSE;

    Buffer[0] = 0x03;
    Buffer[1] = 0x00;
    Buffer[2] = (UCHAR)(PacketLength >> 8);
    Buffer[3] = (UCHAR)PacketLength;
    Buffer[4] = 0x02;
    Buffer[5] = 0xf0;
    Buffer[6] = 0x80;

    Buffer[7] = 0x68;
    Buffer[8] = 0x00;
    Buffer[9] = 0x00;
    Buffer[10] = 0x03;
    Buffer[11] = 0xeb;
    Buffer[12] = 0x70;
    if (PayloadLength > 0x7f)
    {
        Buffer[13] = 0x80;
        Buffer[14] = (UCHAR)PayloadLength;
        PayloadOffset = 15;
    }
    else
    {
        Buffer[13] = (UCHAR)PayloadLength;
        PayloadOffset = 14;
    }
    CopyMemory(&Buffer[PayloadOffset], Payload, PayloadLength);

    *BytesWritten = PacketLength;
    return TRUE;
}

static BOOL
TermSrvWriteLicenseValidClientPacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    static const UCHAR Payload[] =
    {
        /* TS_SECURITY_HEADER: SEC_LICENSE_PKT */
        0x80, 0x00, 0x00, 0x00,
        /* ERROR_ALERT, PREAMBLE_VERSION_3_0, wMsgSize = 16 */
        0xff, 0x03, 0x10, 0x00,
        /* STATUS_VALID_CLIENT, ST_NO_TRANSITION */
        0x07, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        /* BB_ERROR_BLOB, empty */
        0x04, 0x00, 0x00, 0x00
    };

    return TermSrvWriteServerGlobalPayload(Buffer,
                                           BufferLength,
                                           Payload,
                                           sizeof(Payload),
                                           BytesWritten);
}

static BOOL
TermSrvWriteDemandActivePacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    static const UCHAR Payload[] =
    {
        /* Share Control Header: totalLength, Demand Active, user channel. */
        0x4e, 0x00, 0x11, 0x00, 0xe9, 0x03,
        /* Demand Active PDU Data. */
        0xe9, 0x03, 0x01, 0x00,
        0x04, 0x00,
        0x38, 0x00,
        'R', 'D', 'P', 0x00,
        0x02, 0x00, 0x00, 0x00,
        /* General Capability Set. */
        0x01, 0x00, 0x18, 0x00,
        0x01, 0x00, 0x03, 0x00,
        0x00, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x01,
        /* Bitmap Capability Set. */
        0x02, 0x00, 0x1c, 0x00,
        0x20, 0x00, 0x01, 0x01,
        0x01, 0x00, 0x00, 0x04,
        0x00, 0x03, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x01, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        /* sessionId */
        0x00, 0x00, 0x00, 0x00
    };

    return TermSrvWriteServerGlobalPayload(Buffer,
                                           BufferLength,
                                           Payload,
                                           sizeof(Payload),
                                           BytesWritten);
}

static VOID
TermSrvWriteLe16(
    _Out_writes_bytes_(2) UCHAR *Buffer,
    _In_ USHORT Value)
{
    Buffer[0] = (UCHAR)Value;
    Buffer[1] = (UCHAR)(Value >> 8);
}

static VOID
TermSrvWriteLe32(
    _Out_writes_bytes_(4) UCHAR *Buffer,
    _In_ ULONG Value)
{
    Buffer[0] = (UCHAR)Value;
    Buffer[1] = (UCHAR)(Value >> 8);
    Buffer[2] = (UCHAR)(Value >> 16);
    Buffer[3] = (UCHAR)(Value >> 24);
}

static BOOL
TermSrvWriteShareDataPacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ UCHAR DataType,
    _In_reads_bytes_(BodyLength) const UCHAR *Body,
    _In_ SIZE_T BodyLength,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Payload[128];
    SIZE_T PayloadLength;

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    if (BodyLength > sizeof(Payload) - 18 || (Body == NULL && BodyLength != 0))
        return FALSE;

    PayloadLength = 18 + BodyLength;
    TermSrvWriteLe16(&Payload[0], (USHORT)PayloadLength);
    TermSrvWriteLe16(&Payload[2], 0x10 | TERMSRV_RDP_PDU_TYPE_DATA);
    TermSrvWriteLe16(&Payload[4], TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID);
    TermSrvWriteLe32(&Payload[6], TERMSRV_RDP_SCAFFOLD_SHARE_ID);
    Payload[10] = 0;
    Payload[11] = 1;
    TermSrvWriteLe16(&Payload[12], (USHORT)BodyLength);
    Payload[14] = DataType;
    Payload[15] = 0;
    TermSrvWriteLe16(&Payload[16], 0);
    if (BodyLength != 0)
        CopyMemory(&Payload[18], Body, BodyLength);

    return TermSrvWriteServerGlobalPayload(Buffer,
                                           BufferLength,
                                           Payload,
                                           PayloadLength,
                                           BytesWritten);
}

static BOOL
TermSrvWriteTestBitmapUpdatePacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Body[4 + 18 + 32];
    UCHAR *Rectangle;
    UCHAR *Pixels;
    USHORT Colors[16];
    SIZE_T Index;

    Colors[0] = 0xf800;
    Colors[1] = 0x07e0;
    Colors[2] = 0x001f;
    Colors[3] = 0xffff;
    Colors[4] = 0xffff;
    Colors[5] = 0x001f;
    Colors[6] = 0x07e0;
    Colors[7] = 0xf800;
    Colors[8] = 0x07ff;
    Colors[9] = 0xf81f;
    Colors[10] = 0xffe0;
    Colors[11] = 0x0000;
    Colors[12] = 0x0000;
    Colors[13] = 0xffe0;
    Colors[14] = 0xf81f;
    Colors[15] = 0x07ff;

    TermSrvWriteLe16(&Body[0], 0x0001);
    TermSrvWriteLe16(&Body[2], 1);

    Rectangle = &Body[4];
    TermSrvWriteLe16(&Rectangle[0], 0);
    TermSrvWriteLe16(&Rectangle[2], 0);
    TermSrvWriteLe16(&Rectangle[4], 3);
    TermSrvWriteLe16(&Rectangle[6], 3);
    TermSrvWriteLe16(&Rectangle[8], 4);
    TermSrvWriteLe16(&Rectangle[10], 4);
    TermSrvWriteLe16(&Rectangle[12], 16);
    TermSrvWriteLe16(&Rectangle[14], 0);
    TermSrvWriteLe16(&Rectangle[16], 32);

    Pixels = &Rectangle[18];
    for (Index = 0; Index < ARRAYSIZE(Colors); Index++)
        TermSrvWriteLe16(&Pixels[Index * 2], Colors[Index]);

    return TermSrvWriteShareDataPacket(Buffer,
                                       BufferLength,
                                       TERMSRV_RDP_DATA_TYPE_UPDATE,
                                       Body,
                                       sizeof(Body),
                                       BytesWritten);
}

static BOOL
TermSrvWriteServerSynchronizePacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Body[4];

    TermSrvWriteLe16(&Body[0], 1);
    TermSrvWriteLe16(&Body[2], TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID);
    return TermSrvWriteShareDataPacket(Buffer,
                                       BufferLength,
                                       TERMSRV_RDP_DATA_TYPE_SYNCHRONIZE,
                                       Body,
                                       sizeof(Body),
                                       BytesWritten);
}

static BOOL
TermSrvWriteServerControlPacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT Action,
    _In_ USHORT GrantId,
    _In_ ULONG ControlId,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Body[8];

    TermSrvWriteLe16(&Body[0], Action);
    TermSrvWriteLe16(&Body[2], GrantId);
    TermSrvWriteLe32(&Body[4], ControlId);
    return TermSrvWriteShareDataPacket(Buffer,
                                       BufferLength,
                                       TERMSRV_RDP_DATA_TYPE_CONTROL,
                                       Body,
                                       sizeof(Body),
                                       BytesWritten);
}

static BOOL
TermSrvWriteServerFontMapPacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Body[8];

    TermSrvWriteLe16(&Body[0], 0);
    TermSrvWriteLe16(&Body[2], 0);
    TermSrvWriteLe16(&Body[4], 0x0003);
    TermSrvWriteLe16(&Body[6], 4);
    return TermSrvWriteShareDataPacket(Buffer,
                                       BufferLength,
                                       TERMSRV_RDP_DATA_TYPE_FONT_MAP,
                                       Body,
                                       sizeof(Body),
                                       BytesWritten);
}

static BOOL
TermSrvWriteAutoDetectRttRequest(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    static const UCHAR Payload[] =
    {
        /* TS_SECURITY_HEADER: SEC_AUTODETECT_REQ */
        0x00, 0x10, 0x00, 0x00,
        /* RDP_RTT_REQUEST, sequence 0x23, connect-time request type. */
        0x06, 0x00, 0x23, 0x00, 0x01, 0x10
    };
    SIZE_T PacketLength;
    SIZE_T BodyLength;

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    if (Buffer == NULL)
        return FALSE;

    BodyLength = 8 + sizeof(Payload);
    PacketLength = 7 + BodyLength;
    if (BufferLength < PacketLength || PacketLength > 0xffff)
        return FALSE;

    Buffer[0] = 0x03;
    Buffer[1] = 0x00;
    Buffer[2] = (UCHAR)(PacketLength >> 8);
    Buffer[3] = (UCHAR)PacketLength;
    Buffer[4] = 0x02;
    Buffer[5] = 0xf0;
    Buffer[6] = 0x80;

    Buffer[7] = 0x68;
    Buffer[8] = 0x00;
    Buffer[9] = 0x00;
    Buffer[10] = (UCHAR)(TERMSRV_MESSAGE_SCAFFOLD_CHANNEL_ID >> 8);
    Buffer[11] = (UCHAR)TERMSRV_MESSAGE_SCAFFOLD_CHANNEL_ID;
    Buffer[12] = 0x70;
    Buffer[13] = 0x80;
    Buffer[14] = sizeof(Payload);
    CopyMemory(&Buffer[15], Payload, sizeof(Payload));

    *BytesWritten = PacketLength;
    return TRUE;
}

static BOOL
TermSrvLooksLikeAutoDetectRttResponse(
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received)
{
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD SendData;
    const UCHAR *Payload;
    ULONG Flags;

    Result = TermSrvRdpBcgrParseMcsSendDataPayload(Buffer,
                                                   (SIZE_T)Received,
                                                   &SendData);
    if (Result != TermSrvRdpBcgrSuccess || SendData.PayloadLength < 10)
        return FALSE;

    Payload = SendData.Payload;
    Flags = ((ULONG)Payload[3] << 24) |
            ((ULONG)Payload[2] << 16) |
            ((ULONG)Payload[1] << 8) |
            Payload[0];

    return Flags == TERMSRV_SEC_AUTODETECT_RSP &&
           Payload[4] == 0x06 &&
           Payload[5] == 0x01 &&
           Payload[6] == 0x23 &&
           Payload[7] == 0x00 &&
           Payload[8] == 0x00 &&
           Payload[9] == 0x00;
}

static BOOL
TermSrvTryGetSharePduType(
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received,
    _Out_ UCHAR *PduType,
    _Out_ UCHAR *DataType,
    _Out_ USHORT *ControlAction)
{
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_RDPBCGR_SHARE_DATA_PDU ShareData;

    if (PduType == NULL || DataType == NULL || ControlAction == NULL)
        return FALSE;

    *PduType = 0;
    *DataType = 0;
    *ControlAction = 0;
    Result = TermSrvRdpBcgrParseShareDataPdu(Buffer,
                                            (SIZE_T)Received,
                                            &ShareData);
    if (Result != TermSrvRdpBcgrSuccess)
        return FALSE;

    *PduType = ShareData.PduType;
    *DataType = ShareData.DataType;
    *ControlAction = ShareData.ControlAction;

    return TRUE;
}

static VOID
TermSrvHandleSlowPathInputPacket(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received)
{
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_RDPBCGR_INPUT_EVENTS InputEvents;

    Result = TermSrvRdpBcgrParseSlowPathInputPdu(Buffer,
                                                 (SIZE_T)Received,
                                                 &InputEvents);
    if (Result != TermSrvRdpBcgrSuccess)
    {
        if (Result != TermSrvRdpBcgrUnsupportedPdu)
            TermSrvLogRdpBcgrFailure("slow-path input parse", Result);
        return;
    }

    TermSrvAdvancePeer(Context, "SLOW_INPUT wire=rdpbcgr", "slow-path input delivery");
}

static BOOL
TermSrvSendServerControlPacket(
    _In_ SOCKET Client,
    _Out_writes_bytes_(ReplyLength) UCHAR *Reply,
    _In_ SIZE_T ReplyLength,
    _In_ USHORT Action,
    _In_ USHORT GrantId,
    _In_ ULONG ControlId)
{
    SIZE_T BytesWritten;

    if (!TermSrvWriteServerControlPacket(Reply,
                                         ReplyLength,
                                         Action,
                                         GrantId,
                                         ControlId,
                                         &BytesWritten) ||
        !TermSrvSendPacket(Client, Reply, BytesWritten))
    {
        TermSrvLogFailure("server control packet write failed");
        return FALSE;
    }

    return TRUE;
}

static BOOL
TermSrvHandleClientFinalization(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _In_ INT InitialReceived,
    _Out_writes_bytes_(ReplyLength) UCHAR *Reply,
    _In_ SIZE_T ReplyLength)
{
    INT Received;
    INT PacketCount;
    BOOL SawConfirmActive;
    BOOL SentSyncAndCooperate;
    BOOL SentGranted;
    BOOL SentFontMap;

    Received = InitialReceived;
    SawConfirmActive = FALSE;
    SentSyncAndCooperate = FALSE;
    SentGranted = FALSE;
    SentFontMap = FALSE;

    for (PacketCount = 0; PacketCount < 8; PacketCount++)
    {
        UCHAR PduType;
        UCHAR DataType;
        USHORT ControlAction;

        if (Received <= 0 &&
            !TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
        {
            return TRUE;
        }

        if (!TermSrvTryGetSharePduType(Buffer,
                                       Received,
                                       &PduType,
                                       &DataType,
                                       &ControlAction))
        {
            Received = 0;
            continue;
        }

        if (PduType == TERMSRV_RDP_PDU_TYPE_CONFIRM_ACTIVE && !SentSyncAndCooperate)
        {
            SIZE_T BytesWritten;

            SawConfirmActive = TRUE;
            TermSrvAdvancePeer(Context,
                               "CONFIRM_ACTIVE",
                               "confirm active session transition");
            if (!TermSrvWriteServerSynchronizePacket(Reply, ReplyLength, &BytesWritten) ||
                !TermSrvSendPacket(Client, Reply, BytesWritten) ||
                !TermSrvSendServerControlPacket(Client,
                                                Reply,
                                                ReplyLength,
                                                TERMSRV_RDP_CONTROL_ACTION_COOPERATE,
                                                0,
                                                0))
            {
                TermSrvLogFailure("server synchronize/cooperate write failed");
                return FALSE;
            }

            SentSyncAndCooperate = TRUE;
        }
        else if (PduType == TERMSRV_RDP_PDU_TYPE_DATA &&
                 DataType == TERMSRV_RDP_DATA_TYPE_CONTROL &&
                 ControlAction == TERMSRV_RDP_CONTROL_ACTION_REQUEST_CONTROL &&
                 SawConfirmActive &&
                 SentSyncAndCooperate &&
                 !SentGranted)
        {
            if (!TermSrvSendServerControlPacket(Client,
                                                Reply,
                                                ReplyLength,
                                                TERMSRV_RDP_CONTROL_ACTION_GRANTED_CONTROL,
                                                TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID,
                                                0x03ea))
            {
                TermSrvLogFailure("server granted-control write failed");
                return FALSE;
            }

            SentGranted = TRUE;
        }
        else if (PduType == TERMSRV_RDP_PDU_TYPE_DATA &&
                 DataType == TERMSRV_RDP_DATA_TYPE_FONT_LIST &&
                 SentGranted)
        {
            SIZE_T BytesWritten;

            if (!TermSrvWriteServerFontMapPacket(Reply, ReplyLength, &BytesWritten) ||
                !TermSrvSendPacket(Client, Reply, BytesWritten) ||
                !TermSrvWriteTestBitmapUpdatePacket(Reply, ReplyLength, &BytesWritten) ||
                !TermSrvSendPacket(Client, Reply, BytesWritten))
            {
                TermSrvLogFailure("server font-map/bitmap packet write failed");
                return FALSE;
            }

            SentFontMap = TRUE;
            break;
        }

        Received = 0;
    }

    return SentFontMap || !SawConfirmActive;
}

static BOOL
TermSrvRunActiveLoop(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    INT Received;

    while (!TermSrvStopRequested(StopEvent))
    {
        Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, BufferLength);
        if (Received == 0)
            return TRUE;

        if (Received == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                continue;

            TermSrvLogSocketFailure("recv");
            return FALSE;
        }

        switch (TermSrvIdentifyPacketPlaceholder(Buffer, Received))
        {
            case TermSrvPacketFastPath:
                TermSrvAdvancePeer(Context,
                                   "FAST_INPUT wire=fastpath",
                                   "fast-path input delivery");
                break;

            case TermSrvPacketTpkt:
                if (TermSrvIsCliprdrMcsPacket(Buffer, Received, Channel))
                {
                    if (!TermSrvRouteOptionalCliprdrPacket(Client,
                                                          Buffer,
                                                          Received,
                                                          Channel,
                                                          Backend))
                    {
                        return FALSE;
                    }
                }
                else
                {
                    TermSrvHandleSlowPathInputPacket(Context, Buffer, Received);
                }
                break;

            default:
                TermSrvLogFailure("ignoring unknown active RDP packet");
                break;
        }
    }

    return TRUE;
}

static BOOL
TermSrvConsumeOptionalClientInfoAndCliprdrPacket(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _In_ INT InitialReceived,
    _Out_writes_bytes_(ReplyLength) UCHAR *Reply,
    _In_ SIZE_T ReplyLength,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ BOOL NoAuthEnabled)
{
    INT Received;
    SIZE_T BytesWritten;
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD ClientInfo;

    Received = InitialReceived;
    if (Received <= 0)
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
    if (Result != TermSrvRdpBcgrSuccess)
        TermSrvLogRdpBcgrFailure("client info parse", Result);

    if (!NoAuthEnabled)
    {
        TermSrvLogFailure("rejecting RDP scaffold client because REACTOS_TERMSRV_NOAUTH is not set");
        return FALSE;
    }

    if (Context->Peer.State == TermSrvRdpStateChannelsJoined)
        TermSrvAdvancePeer(Context,
                           "SECURITY client_random=wire",
                           "security session transition");

    TermSrvAdvancePeer(Context,
                       "CLIENT_INFO user=DOMAIN\\rdp-scaffold",
                       "client info session attach");

    if (!TermSrvWriteAutoDetectRttRequest(Reply, ReplyLength, &BytesWritten) ||
        !TermSrvSendPacket(Client, Reply, BytesWritten))
    {
        TermSrvLogFailure("autodetect RTT request write failed");
        return FALSE;
    }

    if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
        return TRUE;

    if (TermSrvLooksLikeAutoDetectRttResponse(Buffer, Received))
    {
        if (!TermSrvWriteLicenseValidClientPacket(Reply, ReplyLength, &BytesWritten) ||
            !TermSrvSendPacket(Client, Reply, BytesWritten))
        {
            TermSrvLogFailure("license valid-client packet write failed");
            return FALSE;
        }

        if (!TermSrvWriteDemandActivePacket(Reply, ReplyLength, &BytesWritten) ||
            !TermSrvSendPacket(Client, Reply, BytesWritten))
        {
            TermSrvLogFailure("demand active packet write failed");
            return FALSE;
        }

        if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
            return TRUE;

        return TermSrvHandleClientFinalization(Context,
                                               Client,
                                               StopEvent,
                                               Buffer,
                                               BufferLength,
                                               Received,
                                               Reply,
                                               ReplyLength) &&
               TermSrvRunActiveLoop(Context,
                                    Client,
                                    StopEvent,
                                    Buffer,
                                    BufferLength,
                                    Channel,
                                    Backend);
    }

    return TermSrvRouteOptionalCliprdrPacket(Client,
                                            Buffer,
                                            Received,
                                            Channel,
                                            Backend);
}

static BOOL
TermSrvRunEarlyMcsPhase(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _Out_writes_bytes_(ReplyLength) UCHAR *Reply,
    _In_ SIZE_T ReplyLength,
    _In_opt_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *StaticChannelList,
    _In_ BOOL NoAuthEnabled)
{
    INT Received;
    SIZE_T BytesWritten;
    ULONG JoinCount;
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_RDPBCGR_MCS_ATTACH_USER_CONFIRM AttachUserConfirm;
    TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD SecurityExchange;
    BOOL HaveSecurityExchange;
    INT InitialClientInfoReceived;

    if (!TermSrvInitializeCliprdrScaffold(&Context->CliprdrChannel,
                                          &Context->CliprdrDummy,
                                          &Context->CliprdrBackend))
    {
        return FALSE;
    }

    TermSrvTryAssignCliprdrFromStaticChannelList(&Context->CliprdrChannel,
                                                 StaticChannelList);

    if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
        return FALSE;

    Result = TermSrvRdpBcgrParseMcsErectDomainRequest(Buffer, (SIZE_T)Received);
    if (Result != TermSrvRdpBcgrSuccess)
    {
        TermSrvLogRdpBcgrFailure("MCS erect domain request parse", Result);
        return FALSE;
    }
    TermSrvAdvancePeer(Context, "ERECT", "MCS erect domain session transition");

    if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
        return FALSE;

    Result = TermSrvRdpBcgrParseMcsAttachUserRequest(Buffer, (SIZE_T)Received);
    if (Result != TermSrvRdpBcgrSuccess)
    {
        TermSrvLogRdpBcgrFailure("MCS attach user request parse", Result);
        return FALSE;
    }
    TermSrvAdvancePeer(Context, "ATTACH", "MCS attach user session transition");

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
    InitialClientInfoReceived = 0;

    for (JoinCount = 0; JoinCount < TERMSRV_MCS_MAX_CHANNEL_JOIN_REQUESTS; JoinCount++)
    {
        TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_REQUEST ChannelJoinRequest;
        TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_CONFIRM ChannelJoinConfirm;

        if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
            break;

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
                TermSrvAdvancePeer(Context,
                                   "SECURITY client_random=wire",
                                   "security exchange session transition");
                break;
            }

            TermSrvLogRdpBcgrFailure("security exchange parse", Result);
            return FALSE;
        }

        ZeroMemory(&ChannelJoinConfirm, sizeof(ChannelJoinConfirm));
        ChannelJoinConfirm.Initiator = ChannelJoinRequest.Initiator;
        ChannelJoinConfirm.RequestedChannelId = ChannelJoinRequest.ChannelId;
        ChannelJoinConfirm.ConfirmedChannelId = ChannelJoinRequest.ChannelId;

        if (ChannelJoinRequest.ChannelId == TERMSRV_CLIPRDR_SCAFFOLD_CHANNEL_ID &&
            Context->CliprdrChannel.ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID)
        {
            TERMSRV_CLIPRDR_RESULT ClipResult;

            ClipResult = TermSrvCliprdrAssignChannelId(&Context->CliprdrChannel,
                                                       ChannelJoinRequest.ChannelId);
            if (ClipResult != TermSrvCliprdrSuccess)
            {
                TermSrvLogCliprdrFailure("cliprdr fixed scaffold channel assign",
                                         ClipResult);
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

        if (ChannelJoinRequest.ChannelId == TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID ||
            ChannelJoinRequest.ChannelId == 1003)
        {
            CHAR PeerPacket[32];

            _snprintf(PeerPacket,
                      sizeof(PeerPacket),
                      "JOIN id=%u",
                      ChannelJoinRequest.ChannelId);
            PeerPacket[sizeof(PeerPacket) - 1] = '\0';
            TermSrvAdvancePeer(Context, PeerPacket, "MCS channel join session transition");
        }
    }

    if (!HaveSecurityExchange)
    {
        if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received))
            return TRUE;

        Result = TermSrvRdpBcgrParseSecurityExchangePayload(Buffer,
                                                            (SIZE_T)Received,
                                                            &SecurityExchange);
        if (Result != TermSrvRdpBcgrSuccess)
        {
            TermSrvLogRdpBcgrFailure("security exchange parse, treating packet as client info", Result);
            InitialClientInfoReceived = Received;
        }
        else
        {
            TermSrvAdvancePeer(Context,
                               "SECURITY client_random=wire",
                               "security exchange session transition");
        }
    }

    if (!TermSrvConsumeOptionalClientInfoAndCliprdrPacket(Context,
                                                         Client,
                                                         StopEvent,
                                                         Buffer,
                                                         BufferLength,
                                                         InitialClientInfoReceived,
                                                         Reply,
                                                         ReplyLength,
                                                         &Context->CliprdrChannel,
                                                         &Context->CliprdrBackend,
                                                         NoAuthEnabled))
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
    TERMSRV_CLIENT_CONTEXT Context;
    INT Received;

    ZeroMemory(&Context, sizeof(Context));
    TermSrvSessionManagerInit(&Context.SessionManager);
    TermSrvRdpPeerInit(&Context.Peer, &Context.SessionManager);

    TermSrvSetNonBlocking(Client);

    Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, sizeof(Buffer));

    if (Received > 0)
    {
        TERMSRV_RDPBCGR_CONNECTION_REQUEST Request;
        TERMSRV_RDPBCGR_CONNECTION_CONFIRM Confirm;
        UCHAR Reply[2048];
        SIZE_T ReplyLength;
        TERMSRV_RDPBCGR_RESULT Result;
        TERMSRV_RDPBCGR_MCS_CONNECT_INITIAL ConnectInitial;
        TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST StaticChannelList;
        BOOL HaveStaticChannelList;

        HaveStaticChannelList = FALSE;
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
            TermSrvAdvancePeer(&Context,
                               "X224 cookie=mstshash=wire",
                               "X224 session transition");

            Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, sizeof(Buffer));
            if (Received > 0 &&
                TermSrvIdentifyPacketPlaceholder(Buffer, Received) == TermSrvPacketTpkt &&
                TermSrvRdpBcgrParseMcsConnectInitial(Buffer,
                                                    (SIZE_T)Received,
                                                    &ConnectInitial) == TermSrvRdpBcgrSuccess)
            {
                HaveStaticChannelList = TermSrvTryParseScaffoldStaticChannelList(
                    ConnectInitial.Payload,
                    ConnectInitial.PayloadLength,
                    &StaticChannelList);

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
                TermSrvAdvancePeer(&Context,
                                   "MCS channels=;fast=1",
                                   "MCS session transition");

                if (!TermSrvRunEarlyMcsPhase(&Context,
                                             Client,
                                             StopEvent,
                                             Buffer,
                                             sizeof(Buffer),
                                             Reply,
                                             sizeof(Reply),
                                             HaveStaticChannelList ? &StaticChannelList : NULL,
                                             TermSrvNoAuthEnabled()))
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
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = htons(TermSrvListenerPort());

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
