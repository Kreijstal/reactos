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
    SIZE_T PacketLength;
    SIZE_T HeaderLength;
    SIZE_T Offset;

    if (ConnectInitial == NULL || Buffer == NULL)
        return TermSrvRdpBcgrInvalidHeader;

    memset(ConnectInitial, 0, sizeof(*ConnectInitial));

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

    ConnectInitial->Payload = &Buffer[Offset + X224_DATA_LENGTH];
    ConnectInitial->PayloadLength = PacketLength - HeaderLength;
    return TermSrvRdpBcgrSuccess;
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
