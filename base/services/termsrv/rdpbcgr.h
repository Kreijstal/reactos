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

PCSTR
TermSrvRdpBcgrResultName(
    _In_ TERMSRV_RDPBCGR_RESULT Result);
