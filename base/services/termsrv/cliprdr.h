/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal MS-RDPECLIP virtual channel byte helpers
 */

#pragma once

#include <windef.h>

#define TERMSRV_CLIPRDR_CB_MONITOR_READY          0x0001
#define TERMSRV_CLIPRDR_CB_FORMAT_LIST           0x0002
#define TERMSRV_CLIPRDR_CB_FORMAT_LIST_RESPONSE  0x0003
#define TERMSRV_CLIPRDR_CB_FORMAT_DATA_REQUEST   0x0004
#define TERMSRV_CLIPRDR_CB_FORMAT_DATA_RESPONSE  0x0005
#define TERMSRV_CLIPRDR_CB_CLIP_CAPS             0x0007

#define TERMSRV_CLIPRDR_CB_RESPONSE_OK           0x0001
#define TERMSRV_CLIPRDR_CB_RESPONSE_FAIL         0x0002

typedef enum _TERMSRV_CLIPRDR_RESULT
{
    TermSrvCliprdrSuccess = 0,
    TermSrvCliprdrNeedMoreData,
    TermSrvCliprdrBufferTooSmall,
    TermSrvCliprdrInvalidHeader,
    TermSrvCliprdrInvalidLength,
    TermSrvCliprdrUnsupportedPdu
} TERMSRV_CLIPRDR_RESULT;

typedef struct _TERMSRV_CLIPRDR_PDU
{
    USHORT MsgType;
    USHORT MsgFlags;
    ULONG DataLength;
    const UCHAR *Payload;
} TERMSRV_CLIPRDR_PDU;

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParsePdu(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_CLIPRDR_PDU *Pdu);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteMonitorReady(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFormatListResponse(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT ResponseFlags,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFormatDataRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ ULONG *FormatId);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFormatDataResponse(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT ResponseFlags,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength,
    _Out_ SIZE_T *BytesWritten);
