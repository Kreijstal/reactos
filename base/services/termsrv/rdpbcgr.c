/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal MS-RDPBCGR connection sequence byte helpers
 */

#include "rdpbcgr.h"

#include <string.h>

#define TPKT_VERSION 0x03
#define TPKT_HEADER_LENGTH 4

#define X224_CR_TPDU 0xE0
#define X224_CC_TPDU 0xD0
#define X224_DT_TPDU 0xF0
#define X224_FIXED_LENGTH 6
#define X224_DATA_LENGTH 2
#define X224_DATA_EOT 0x80

#define RDP_NEGOTIATION_LENGTH 8
#define MCS_SEND_DATA_REQUEST 0x64
#define SECURITY_FLAGS_LENGTH 4
#define SEC_EXCHANGE_PKT 0x00000001
#define SEC_INFO_PKT 0x00000040
#define STATIC_CHANNEL_HEADER_LENGTH 1
#define STATIC_CHANNEL_DEF_LENGTH (TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH + 4)

static USHORT
ReadBe16(
    _In_reads_bytes_(2) const UCHAR *Buffer)
{
    return (USHORT)(((USHORT)Buffer[0] << 8) | Buffer[1]);
}

static USHORT
ReadLe16(
    _In_reads_bytes_(2) const UCHAR *Buffer)
{
    return (USHORT)(((USHORT)Buffer[1] << 8) | Buffer[0]);
}

static ULONG
ReadLe32(
    _In_reads_bytes_(4) const UCHAR *Buffer)
{
    return ((ULONG)Buffer[3] << 24) |
           ((ULONG)Buffer[2] << 16) |
           ((ULONG)Buffer[1] << 8) |
           Buffer[0];
}

static VOID
WriteBe16(
    _Out_writes_bytes_(2) UCHAR *Buffer,
    _In_ USHORT Value)
{
    Buffer[0] = (UCHAR)(Value >> 8);
    Buffer[1] = (UCHAR)Value;
}

static VOID
WriteLe16(
    _Out_writes_bytes_(2) UCHAR *Buffer,
    _In_ USHORT Value)
{
    Buffer[0] = (UCHAR)Value;
    Buffer[1] = (UCHAR)(Value >> 8);
}

static VOID
WriteLe32(
    _Out_writes_bytes_(4) UCHAR *Buffer,
    _In_ ULONG Value)
{
    Buffer[0] = (UCHAR)Value;
    Buffer[1] = (UCHAR)(Value >> 8);
    Buffer[2] = (UCHAR)(Value >> 16);
    Buffer[3] = (UCHAR)(Value >> 24);
}

static TERMSRV_RDPBCGR_RESULT
ParseMcsDataBody(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ const UCHAR **Body,
    _Out_ SIZE_T *BodyLength)
{
    SIZE_T PacketLength;
    SIZE_T HeaderLength;
    SIZE_T Offset;

    if (Body == NULL || BodyLength == NULL || Buffer == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    *Body = NULL;
    *BodyLength = 0;

    HeaderLength = TPKT_HEADER_LENGTH + 1 + X224_DATA_LENGTH;
    if (BufferLength < HeaderLength)
        return TermSrvRdpBcgrNeedMoreData;

    if (Buffer[0] != TPKT_VERSION || Buffer[1] != 0)
        return TermSrvRdpBcgrInvalidHeader;

    PacketLength = ReadBe16(&Buffer[2]);
    if (PacketLength < HeaderLength)
        return TermSrvRdpBcgrInvalidLength;

    if (PacketLength > BufferLength)
        return TermSrvRdpBcgrNeedMoreData;

    if (PacketLength != BufferLength)
        return TermSrvRdpBcgrInvalidLength;

    if (Buffer[TPKT_HEADER_LENGTH] != X224_DATA_LENGTH)
        return TermSrvRdpBcgrInvalidLength;

    Offset = TPKT_HEADER_LENGTH + 1;
    if (Buffer[Offset] != X224_DT_TPDU ||
        Buffer[Offset + 1] != X224_DATA_EOT)
    {
        return TermSrvRdpBcgrUnsupportedPdu;
    }

    *Body = &Buffer[HeaderLength];
    *BodyLength = PacketLength - HeaderLength;
    return TermSrvRdpBcgrSuccess;
}

static TERMSRV_RDPBCGR_RESULT
WriteMcsDataPdu(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_reads_bytes_(BodyLength) const UCHAR *Body,
    _In_ SIZE_T BodyLength,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T PacketLength;
    SIZE_T HeaderLength;

    if (BytesWritten == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    *BytesWritten = 0;

    if (Buffer == NULL || Body == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    HeaderLength = TPKT_HEADER_LENGTH + 1 + X224_DATA_LENGTH;
    if (BodyLength > 0xFFFF - HeaderLength)
        return TermSrvRdpBcgrInvalidLength;

    PacketLength = HeaderLength + BodyLength;
    if (PacketLength > 0xFFFF)
        return TermSrvRdpBcgrInvalidLength;

    if (BufferLength < PacketLength)
        return TermSrvRdpBcgrBufferTooSmall;

    Buffer[0] = TPKT_VERSION;
    Buffer[1] = 0;
    WriteBe16(&Buffer[2], (USHORT)PacketLength);

    Buffer[4] = X224_DATA_LENGTH;
    Buffer[5] = X224_DT_TPDU;
    Buffer[6] = X224_DATA_EOT;
    memcpy(&Buffer[HeaderLength], Body, BodyLength);

    *BytesWritten = PacketLength;
    return TermSrvRdpBcgrSuccess;
}

static BOOL
FindCookieTerminator(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ SIZE_T Length,
    _Out_ SIZE_T *CookieLength,
    _Out_ SIZE_T *TerminatorLength)
{
    SIZE_T Index;

    for (Index = 0; Index < Length; Index++)
    {
        if (Buffer[Index] == '\n')
        {
            if (Index > 0 && Buffer[Index - 1] == '\r')
            {
                *CookieLength = Index - 1;
                *TerminatorLength = 2;
            }
            else
            {
                *CookieLength = Index;
                *TerminatorLength = 1;
            }

            return TRUE;
        }
    }

    return FALSE;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseConnectionRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_CONNECTION_REQUEST *Request)
{
    SIZE_T PacketLength;
    SIZE_T X224Length;
    SIZE_T Offset;
    SIZE_T Remaining;
    SIZE_T CookieLength;
    SIZE_T TerminatorLength;

    if (Request == NULL || Buffer == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    memset(Request, 0, sizeof(*Request));

    if (BufferLength < TPKT_HEADER_LENGTH + 1)
        return TermSrvRdpBcgrNeedMoreData;

    if (Buffer[0] != TPKT_VERSION || Buffer[1] != 0)
        return TermSrvRdpBcgrInvalidHeader;

    PacketLength = ReadBe16(&Buffer[2]);
    if (PacketLength < TPKT_HEADER_LENGTH + 1)
        return TermSrvRdpBcgrInvalidLength;

    if (PacketLength > BufferLength)
        return TermSrvRdpBcgrNeedMoreData;

    if (PacketLength != BufferLength)
        return TermSrvRdpBcgrInvalidLength;

    X224Length = Buffer[TPKT_HEADER_LENGTH];
    if (X224Length < X224_FIXED_LENGTH ||
        X224Length + TPKT_HEADER_LENGTH + 1 != PacketLength)
    {
        return TermSrvRdpBcgrInvalidLength;
    }

    Offset = TPKT_HEADER_LENGTH + 1;
    if (Buffer[Offset] != X224_CR_TPDU)
        return TermSrvRdpBcgrUnsupportedPdu;

    Request->DestinationReference = ReadBe16(&Buffer[Offset + 1]);
    Request->SourceReference = ReadBe16(&Buffer[Offset + 3]);
    Request->ClassOption = Buffer[Offset + 5];

    Offset += X224_FIXED_LENGTH;
    Remaining = PacketLength - Offset;

    if (Remaining == 0)
        return TermSrvRdpBcgrSuccess;

    if (Remaining >= RDP_NEGOTIATION_LENGTH &&
        Buffer[Offset] == TermSrvRdpBcgrNegRequest)
    {
        goto ParseNegotiationRequest;
    }

    if (!FindCookieTerminator(&Buffer[Offset],
                              Remaining,
                              &CookieLength,
                              &TerminatorLength))
    {
        return TermSrvRdpBcgrInvalidLength;
    }

    Request->Cookie = &Buffer[Offset];
    Request->CookieLength = (USHORT)CookieLength;
    Request->HasCookie = TRUE;

    Offset += CookieLength + TerminatorLength;
    Remaining = PacketLength - Offset;

    if (Remaining == 0)
        return TermSrvRdpBcgrSuccess;

ParseNegotiationRequest:
    if (Remaining != RDP_NEGOTIATION_LENGTH)
        return TermSrvRdpBcgrInvalidLength;

    if (Buffer[Offset] != TermSrvRdpBcgrNegRequest)
        return TermSrvRdpBcgrUnsupportedPdu;

    Request->Negotiation.Type = Buffer[Offset];
    Request->Negotiation.Flags = Buffer[Offset + 1];
    Request->Negotiation.Length = ReadLe16(&Buffer[Offset + 2]);
    if (Request->Negotiation.Length != RDP_NEGOTIATION_LENGTH)
        return TermSrvRdpBcgrInvalidLength;

    Request->Negotiation.Protocols = ReadLe32(&Buffer[Offset + 4]);
    Request->HasNegotiation = TRUE;
    return TermSrvRdpBcgrSuccess;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsConnectInitial(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_MCS_CONNECT_INITIAL *ConnectInitial)
{
    TERMSRV_RDPBCGR_RESULT Result;
    SIZE_T BodyLength;
    const UCHAR *Body;

    if (ConnectInitial == NULL || Buffer == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    memset(ConnectInitial, 0, sizeof(*ConnectInitial));

    Result = ParseMcsDataBody(Buffer, BufferLength, &Body, &BodyLength);
    if (Result != TermSrvRdpBcgrSuccess)
        return Result;

    ConnectInitial->Payload = Body;
    ConnectInitial->PayloadLength = BodyLength;
    return TermSrvRdpBcgrSuccess;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsErectDomainRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength)
{
    static const UCHAR ExpectedBody[] = { 0x04, 0x01, 0x00, 0x01, 0x00 };
    TERMSRV_RDPBCGR_RESULT Result;
    SIZE_T BodyLength;
    const UCHAR *Body;

    Result = ParseMcsDataBody(Buffer, BufferLength, &Body, &BodyLength);
    if (Result != TermSrvRdpBcgrSuccess)
        return Result;

    if (BodyLength != sizeof(ExpectedBody))
        return TermSrvRdpBcgrInvalidLength;

    if (memcmp(Body, ExpectedBody, sizeof(ExpectedBody)) != 0)
        return TermSrvRdpBcgrUnsupportedPdu;

    return TermSrvRdpBcgrSuccess;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsAttachUserRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength)
{
    TERMSRV_RDPBCGR_RESULT Result;
    SIZE_T BodyLength;
    const UCHAR *Body;

    Result = ParseMcsDataBody(Buffer, BufferLength, &Body, &BodyLength);
    if (Result != TermSrvRdpBcgrSuccess)
        return Result;

    if (BodyLength != 1)
        return TermSrvRdpBcgrInvalidLength;

    if (Body[0] != 0x28)
        return TermSrvRdpBcgrUnsupportedPdu;

    return TermSrvRdpBcgrSuccess;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsChannelJoinRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_REQUEST *Request)
{
    TERMSRV_RDPBCGR_RESULT Result;
    SIZE_T BodyLength;
    const UCHAR *Body;

    if (Request == NULL || Buffer == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    memset(Request, 0, sizeof(*Request));

    Result = ParseMcsDataBody(Buffer, BufferLength, &Body, &BodyLength);
    if (Result != TermSrvRdpBcgrSuccess)
        return Result;

    if (BodyLength != 5)
        return TermSrvRdpBcgrInvalidLength;

    if (Body[0] != 0x38)
        return TermSrvRdpBcgrUnsupportedPdu;

    Request->Initiator = ReadBe16(&Body[1]);
    Request->ChannelId = ReadBe16(&Body[3]);
    return TermSrvRdpBcgrSuccess;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsSendDataPayload(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD *SendData)
{
    TERMSRV_RDPBCGR_RESULT Result;
    SIZE_T BodyLength;
    SIZE_T PayloadLength;
    const UCHAR *Body;

    if (SendData == NULL || Buffer == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    memset(SendData, 0, sizeof(*SendData));

    Result = ParseMcsDataBody(Buffer, BufferLength, &Body, &BodyLength);
    if (Result != TermSrvRdpBcgrSuccess)
        return Result;

    if (BodyLength < 8)
        return TermSrvRdpBcgrInvalidLength;

    if (Body[0] != MCS_SEND_DATA_REQUEST)
        return TermSrvRdpBcgrUnsupportedPdu;

    PayloadLength = ReadBe16(&Body[6]);
    if (PayloadLength != BodyLength - 8)
        return TermSrvRdpBcgrInvalidLength;

    SendData->Initiator = ReadBe16(&Body[1]);
    SendData->ChannelId = ReadBe16(&Body[3]);
    SendData->Priority = Body[5];
    SendData->Payload = &Body[8];
    SendData->PayloadLength = PayloadLength;
    return TermSrvRdpBcgrSuccess;
}

static TERMSRV_RDPBCGR_RESULT
ParseOpaqueSecurityPayload(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ ULONG ExpectedFlags,
    _Out_ TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD *Packet)
{
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD SendData;
    ULONG Flags;

    if (Packet == NULL || Buffer == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    memset(Packet, 0, sizeof(*Packet));

    Result = TermSrvRdpBcgrParseMcsSendDataPayload(Buffer,
                                                   BufferLength,
                                                   &SendData);
    if (Result != TermSrvRdpBcgrSuccess)
        return Result;

    if (SendData.PayloadLength < SECURITY_FLAGS_LENGTH)
        return TermSrvRdpBcgrInvalidLength;

    Flags = ReadLe32(SendData.Payload);
    if (Flags != ExpectedFlags)
        return TermSrvRdpBcgrUnsupportedPdu;

    Packet->Initiator = SendData.Initiator;
    Packet->ChannelId = SendData.ChannelId;
    Packet->Priority = SendData.Priority;
    Packet->Flags = Flags;
    Packet->Payload = &SendData.Payload[SECURITY_FLAGS_LENGTH];
    Packet->PayloadLength = SendData.PayloadLength - SECURITY_FLAGS_LENGTH;
    return TermSrvRdpBcgrSuccess;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseSecurityExchangePayload(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD *SecurityExchange)
{
    return ParseOpaqueSecurityPayload(Buffer,
                                      BufferLength,
                                      SEC_EXCHANGE_PKT,
                                      SecurityExchange);
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseClientInfoPayload(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD *ClientInfo)
{
    return ParseOpaqueSecurityPayload(Buffer,
                                      BufferLength,
                                      SEC_INFO_PKT,
                                      ClientInfo);
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseStaticChannelList(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList)
{
    SIZE_T Count;
    SIZE_T ExpectedLength;
    SIZE_T Offset;
    SIZE_T Index;

    if (ChannelList == NULL || Buffer == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    memset(ChannelList, 0, sizeof(*ChannelList));

    if (BufferLength < STATIC_CHANNEL_HEADER_LENGTH)
        return TermSrvRdpBcgrNeedMoreData;

    Count = Buffer[0];
    if (Count > TERMSRV_RDPBCGR_MAX_STATIC_CHANNELS)
        return TermSrvRdpBcgrInvalidLength;

    ExpectedLength = STATIC_CHANNEL_HEADER_LENGTH + Count * STATIC_CHANNEL_DEF_LENGTH;
    if (BufferLength < ExpectedLength)
        return TermSrvRdpBcgrNeedMoreData;

    if (BufferLength != ExpectedLength)
        return TermSrvRdpBcgrInvalidLength;

    ChannelList->Count = Count;
    Offset = STATIC_CHANNEL_HEADER_LENGTH;

    for (Index = 0; Index < Count; Index++)
    {
        memcpy(ChannelList->Channels[Index].Name,
               &Buffer[Offset],
               TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH);
        Offset += TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH;

        ChannelList->Channels[Index].Options = ReadLe32(&Buffer[Offset]);
        ChannelList->Channels[Index].Index = Index;
        Offset += 4;
    }

    return TermSrvRdpBcgrSuccess;
}

static BOOL
StaticChannelNameEquals(
    _In_reads_bytes_(TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH) const UCHAR *ChannelName,
    _In_reads_bytes_(NameLength) const CHAR *Name,
    _In_ SIZE_T NameLength)
{
    SIZE_T Index;

    if (ChannelName == NULL || Name == NULL ||
        NameLength > TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH)
    {
        return FALSE;
    }

    if (memcmp(ChannelName, Name, NameLength) != 0)
        return FALSE;

    for (Index = NameLength;
         Index < TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH;
         Index++)
    {
        if (ChannelName[Index] != '\0')
            return FALSE;
    }

    return TRUE;
}

BOOL
TermSrvRdpBcgrFindStaticChannelByName(
    _In_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList,
    _In_reads_bytes_(NameLength) const CHAR *Name,
    _In_ SIZE_T NameLength,
    _Out_ SIZE_T *ChannelIndex)
{
    SIZE_T Index;

    if (ChannelIndex == NULL)
        return FALSE;

    *ChannelIndex = 0;

    if (ChannelList == NULL || Name == NULL)
        return FALSE;

    for (Index = 0; Index < ChannelList->Count; Index++)
    {
        if (StaticChannelNameEquals(ChannelList->Channels[Index].Name,
                                    Name,
                                    NameLength))
        {
            *ChannelIndex = ChannelList->Channels[Index].Index;
            return TRUE;
        }
    }

    return FALSE;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteConnectionConfirm(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDPBCGR_CONNECTION_CONFIRM *Confirm,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T PacketLength;
    SIZE_T X224Length;
    SIZE_T Offset;

    if (BytesWritten == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    *BytesWritten = 0;

    if (Buffer == NULL || Confirm == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    X224Length = X224_FIXED_LENGTH;
    if (Confirm->HasNegotiation)
    {
        if (Confirm->Negotiation.Length != RDP_NEGOTIATION_LENGTH ||
            (Confirm->Negotiation.Type != TermSrvRdpBcgrNegResponse &&
             Confirm->Negotiation.Type != TermSrvRdpBcgrNegFailure))
        {
            return TermSrvRdpBcgrInvalidLength;
        }

        X224Length += RDP_NEGOTIATION_LENGTH;
    }

    PacketLength = TPKT_HEADER_LENGTH + 1 + X224Length;
    if (PacketLength > 0xFFFF || X224Length > 0xFF)
        return TermSrvRdpBcgrInvalidLength;

    if (BufferLength < PacketLength)
        return TermSrvRdpBcgrBufferTooSmall;

    Buffer[0] = TPKT_VERSION;
    Buffer[1] = 0;
    WriteBe16(&Buffer[2], (USHORT)PacketLength);

    Buffer[4] = (UCHAR)X224Length;
    Offset = 5;
    Buffer[Offset] = X224_CC_TPDU;
    WriteBe16(&Buffer[Offset + 1], Confirm->DestinationReference);
    WriteBe16(&Buffer[Offset + 3], Confirm->SourceReference);
    Buffer[Offset + 5] = Confirm->ClassOption;
    Offset += X224_FIXED_LENGTH;

    if (Confirm->HasNegotiation)
    {
        Buffer[Offset] = Confirm->Negotiation.Type;
        Buffer[Offset + 1] = Confirm->Negotiation.Flags;
        WriteLe16(&Buffer[Offset + 2], Confirm->Negotiation.Length);
        WriteLe32(&Buffer[Offset + 4], Confirm->Negotiation.Protocols);
    }

    *BytesWritten = PacketLength;
    return TermSrvRdpBcgrSuccess;
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteMcsConnectResponse(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _Out_ SIZE_T *BytesWritten)
{
    return WriteMcsDataPdu(Buffer,
                           BufferLength,
                           Payload,
                           PayloadLength,
                           BytesWritten);
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteMcsAttachUserConfirm(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDPBCGR_MCS_ATTACH_USER_CONFIRM *Confirm,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Body[4];

    if (Confirm == NULL)
    {
        if (BytesWritten != NULL)
            *BytesWritten = 0;
        return TermSrvRdpBcgrInvalidHeader;
    }

    Body[0] = 0x2e;
    Body[1] = 0x00;
    WriteBe16(&Body[2], Confirm->UserChannelId);

    return WriteMcsDataPdu(Buffer,
                           BufferLength,
                           Body,
                           sizeof(Body),
                           BytesWritten);
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteMcsChannelJoinConfirm(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_CONFIRM *Confirm,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Body[8];

    if (Confirm == NULL)
    {
        if (BytesWritten != NULL)
            *BytesWritten = 0;
        return TermSrvRdpBcgrInvalidHeader;
    }

    Body[0] = 0x3e;
    Body[1] = 0x00;
    WriteBe16(&Body[2], Confirm->Initiator);
    WriteBe16(&Body[4], Confirm->RequestedChannelId);
    WriteBe16(&Body[6], Confirm->ConfirmedChannelId);

    return WriteMcsDataPdu(Buffer,
                           BufferLength,
                           Body,
                           sizeof(Body),
                           BytesWritten);
}

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteMcsSendDataPayload(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD *SendData,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T BodyLength;
    SIZE_T PacketLength;
    SIZE_T HeaderLength;

    if (BytesWritten == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    *BytesWritten = 0;

    if (Buffer == NULL || SendData == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    if (SendData->PayloadLength != 0 && SendData->Payload == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    HeaderLength = TPKT_HEADER_LENGTH + 1 + X224_DATA_LENGTH;
    if (SendData->PayloadLength > 0xFFFF ||
        SendData->PayloadLength > 0xFFFF - HeaderLength - 8)
    {
        return TermSrvRdpBcgrInvalidLength;
    }

    BodyLength = 8 + SendData->PayloadLength;
    PacketLength = HeaderLength + BodyLength;
    if (BufferLength < PacketLength)
        return TermSrvRdpBcgrBufferTooSmall;

    Buffer[0] = TPKT_VERSION;
    Buffer[1] = 0;
    WriteBe16(&Buffer[2], (USHORT)PacketLength);

    Buffer[4] = X224_DATA_LENGTH;
    Buffer[5] = X224_DT_TPDU;
    Buffer[6] = X224_DATA_EOT;

    Buffer[7] = MCS_SEND_DATA_REQUEST;
    WriteBe16(&Buffer[8], SendData->Initiator);
    WriteBe16(&Buffer[10], SendData->ChannelId);
    Buffer[12] = SendData->Priority;
    WriteBe16(&Buffer[13], (USHORT)SendData->PayloadLength);
    if (SendData->PayloadLength != 0)
        memcpy(&Buffer[15], SendData->Payload, SendData->PayloadLength);

    *BytesWritten = PacketLength;
    return TermSrvRdpBcgrSuccess;
}

PCSTR
TermSrvRdpBcgrResultName(
    _In_ TERMSRV_RDPBCGR_RESULT Result)
{
    switch (Result)
    {
        case TermSrvRdpBcgrSuccess: return "Success";
        case TermSrvRdpBcgrNeedMoreData: return "NeedMoreData";
        case TermSrvRdpBcgrBufferTooSmall: return "BufferTooSmall";
        case TermSrvRdpBcgrInvalidHeader: return "InvalidHeader";
        case TermSrvRdpBcgrInvalidLength: return "InvalidLength";
        case TermSrvRdpBcgrUnsupportedPdu: return "UnsupportedPdu";
        default: return "Unknown";
    }
}
