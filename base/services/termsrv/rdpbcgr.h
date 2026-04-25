/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal MS-RDPBCGR connection sequence byte helpers
 */

#pragma once

#include <windef.h>

typedef enum _TERMSRV_RDPBCGR_RESULT
{
    TermSrvRdpBcgrSuccess = 0,
    TermSrvRdpBcgrNeedMoreData,
    TermSrvRdpBcgrBufferTooSmall,
    TermSrvRdpBcgrInvalidHeader,
    TermSrvRdpBcgrInvalidLength,
    TermSrvRdpBcgrUnsupportedPdu
} TERMSRV_RDPBCGR_RESULT;

typedef enum _TERMSRV_RDPBCGR_NEG_TYPE
{
    TermSrvRdpBcgrNegRequest = 0x01,
    TermSrvRdpBcgrNegResponse = 0x02,
    TermSrvRdpBcgrNegFailure = 0x03
} TERMSRV_RDPBCGR_NEG_TYPE;

typedef enum _TERMSRV_RDPBCGR_PROTOCOL
{
    TermSrvRdpBcgrProtocolStandard = 0x00000000,
    TermSrvRdpBcgrProtocolSsl = 0x00000001,
    TermSrvRdpBcgrProtocolCredSsp = 0x00000002,
    TermSrvRdpBcgrProtocolHybridEx = 0x00000008,
    TermSrvRdpBcgrProtocolRdstls = 0x00000010
} TERMSRV_RDPBCGR_PROTOCOL;

typedef struct _TERMSRV_RDPBCGR_NEGOTIATION
{
    UCHAR Type;
    UCHAR Flags;
    USHORT Length;
    ULONG Protocols;
} TERMSRV_RDPBCGR_NEGOTIATION;

typedef struct _TERMSRV_RDPBCGR_CONNECTION_REQUEST
{
    USHORT DestinationReference;
    USHORT SourceReference;
    UCHAR ClassOption;
    const UCHAR *Cookie;
    USHORT CookieLength;
    BOOL HasCookie;
    BOOL HasNegotiation;
    TERMSRV_RDPBCGR_NEGOTIATION Negotiation;
} TERMSRV_RDPBCGR_CONNECTION_REQUEST;

typedef struct _TERMSRV_RDPBCGR_CONNECTION_CONFIRM
{
    USHORT DestinationReference;
    USHORT SourceReference;
    UCHAR ClassOption;
    BOOL HasNegotiation;
    TERMSRV_RDPBCGR_NEGOTIATION Negotiation;
} TERMSRV_RDPBCGR_CONNECTION_CONFIRM;

typedef struct _TERMSRV_RDPBCGR_MCS_CONNECT_INITIAL
{
    const UCHAR *Payload;
    SIZE_T PayloadLength;
} TERMSRV_RDPBCGR_MCS_CONNECT_INITIAL;

typedef struct _TERMSRV_RDPBCGR_MCS_ATTACH_USER_CONFIRM
{
    USHORT UserChannelId;
} TERMSRV_RDPBCGR_MCS_ATTACH_USER_CONFIRM;

typedef struct _TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_REQUEST
{
    USHORT Initiator;
    USHORT ChannelId;
} TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_REQUEST;

typedef struct _TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_CONFIRM
{
    USHORT Initiator;
    USHORT RequestedChannelId;
    USHORT ConfirmedChannelId;
} TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_CONFIRM;

typedef struct _TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD
{
    USHORT Initiator;
    USHORT ChannelId;
    UCHAR Priority;
    const UCHAR *Payload;
    SIZE_T PayloadLength;
} TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD;

typedef struct _TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD
{
    USHORT Initiator;
    USHORT ChannelId;
    UCHAR Priority;
    ULONG Flags;
    const UCHAR *Payload;
    SIZE_T PayloadLength;
} TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD;

#define TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH 8
#define TERMSRV_RDPBCGR_MAX_STATIC_CHANNELS 31

typedef struct _TERMSRV_RDPBCGR_STATIC_CHANNEL
{
    UCHAR Name[TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH];
    ULONG Options;
    SIZE_T Index;
} TERMSRV_RDPBCGR_STATIC_CHANNEL;

typedef struct _TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST
{
    SIZE_T Count;
    TERMSRV_RDPBCGR_STATIC_CHANNEL Channels[TERMSRV_RDPBCGR_MAX_STATIC_CHANNELS];
} TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST;

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseConnectionRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_CONNECTION_REQUEST *Request);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsConnectInitial(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_MCS_CONNECT_INITIAL *ConnectInitial);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsErectDomainRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsAttachUserRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsChannelJoinRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_REQUEST *Request);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseMcsSendDataPayload(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD *SendData);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseSecurityExchangePayload(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD *SecurityExchange);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseClientInfoPayload(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD *ClientInfo);

/*
 * Parses a small scaffold static virtual channel list:
 *   UINT8 count;
 *   CHANNEL_DEF[count] where each entry is 8 name bytes + UINT32LE options.
 *
 * This matches the CS_NET channelDef element shape without claiming to parse
 * the surrounding GCC user data yet.
 */
TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrParseStaticChannelList(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList);

BOOL
TermSrvRdpBcgrFindStaticChannelByName(
    _In_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList,
    _In_reads_bytes_(NameLength) const CHAR *Name,
    _In_ SIZE_T NameLength,
    _Out_ SIZE_T *ChannelIndex);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteConnectionConfirm(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDPBCGR_CONNECTION_CONFIRM *Confirm,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteMcsConnectResponse(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteMcsAttachUserConfirm(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDPBCGR_MCS_ATTACH_USER_CONFIRM *Confirm,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteMcsChannelJoinConfirm(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_CONFIRM *Confirm,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_RDPBCGR_RESULT
TermSrvRdpBcgrWriteMcsSendDataPayload(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD *SendData,
    _Out_ SIZE_T *BytesWritten);

PCSTR
TermSrvRdpBcgrResultName(
    _In_ TERMSRV_RDPBCGR_RESULT Result);
