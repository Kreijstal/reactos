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

#define TERMSRV_CLIPRDR_CHANNEL_NAME             "cliprdr"
#define TERMSRV_CLIPRDR_MAX_CHANNEL_NAME_LENGTH  8
#define TERMSRV_CLIPRDR_INVALID_CHANNEL_ID       0

typedef enum _TERMSRV_CLIPRDR_RESULT
{
    TermSrvCliprdrSuccess = 0,
    TermSrvCliprdrNeedMoreData,
    TermSrvCliprdrBufferTooSmall,
    TermSrvCliprdrInvalidHeader,
    TermSrvCliprdrInvalidLength,
    TermSrvCliprdrUnsupportedPdu,
    TermSrvCliprdrFormatNotAvailable
} TERMSRV_CLIPRDR_RESULT;

struct _TERMSRV_CLIPRDR_BACKEND;

typedef TERMSRV_CLIPRDR_RESULT
(*TERMSRV_CLIPRDR_BACKEND_SET_DATA)(
    _Inout_ struct _TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength);

typedef TERMSRV_CLIPRDR_RESULT
(*TERMSRV_CLIPRDR_BACKEND_GET_DATA)(
    _Inout_ struct _TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _Out_writes_bytes_to_opt_(BufferLength, *RequiredLength) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *RequiredLength);

typedef TERMSRV_CLIPRDR_RESULT
(*TERMSRV_CLIPRDR_BACKEND_CLEAR)(
    _Inout_ struct _TERMSRV_CLIPRDR_BACKEND *Backend);

typedef struct _TERMSRV_CLIPRDR_BACKEND_OPS
{
    TERMSRV_CLIPRDR_BACKEND_SET_DATA SetData;
    TERMSRV_CLIPRDR_BACKEND_GET_DATA GetData;
    TERMSRV_CLIPRDR_BACKEND_CLEAR Clear;
} TERMSRV_CLIPRDR_BACKEND_OPS;

typedef struct _TERMSRV_CLIPRDR_BACKEND
{
    const TERMSRV_CLIPRDR_BACKEND_OPS *Ops;
    VOID *Context;
} TERMSRV_CLIPRDR_BACKEND;

#define TERMSRV_CLIPRDR_DUMMY_MAX_DATA_LENGTH    4096

typedef struct _TERMSRV_CLIPRDR_DUMMY_BACKEND
{
    BOOL HasData;
    ULONG FormatId;
    SIZE_T DataLength;
    UCHAR Data[TERMSRV_CLIPRDR_DUMMY_MAX_DATA_LENGTH];
} TERMSRV_CLIPRDR_DUMMY_BACKEND;

typedef struct _TERMSRV_CLIPRDR_PDU
{
    USHORT MsgType;
    USHORT MsgFlags;
    ULONG DataLength;
    const UCHAR *Payload;
} TERMSRV_CLIPRDR_PDU;

typedef struct _TERMSRV_CLIPRDR_CHANNEL
{
    BOOL Enabled;
    USHORT ChannelId;
} TERMSRV_CLIPRDR_CHANNEL;

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrChannelInit(
    _Out_ TERMSRV_CLIPRDR_CHANNEL *Channel);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrChannelReset(
    _Out_ TERMSRV_CLIPRDR_CHANNEL *Channel);

BOOL
TermSrvCliprdrIsStaticChannelName(
    _In_reads_bytes_(NameLength) const CHAR *Name,
    _In_ SIZE_T NameLength);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrAssignChannelId(
    _Inout_ TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ USHORT ChannelId);

BOOL
TermSrvCliprdrIsChannelId(
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ USHORT ChannelId);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrBackendSetData(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrBackendGetData(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _Out_writes_bytes_to_opt_(BufferLength, *RequiredLength) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *RequiredLength);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrBackendClear(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrDummyBackendInit(
    _Out_ TERMSRV_CLIPRDR_DUMMY_BACKEND *Dummy,
    _Out_ TERMSRV_CLIPRDR_BACKEND *Backend);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrDummyBackendReset(
    _Out_ TERMSRV_CLIPRDR_DUMMY_BACKEND *Dummy);

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

/*
 * Consumes one complete cliprdr PDU payload and writes at most one response PDU.
 * Unsupported PDUs write zero bytes and return TermSrvCliprdrUnsupportedPdu.
 * Missing requested clipboard data writes a CB_FORMAT_DATA_RESPONSE FAIL PDU
 * and returns TermSrvCliprdrFormatNotAvailable.
 * Output buffer failures write zero bytes and return TermSrvCliprdrBufferTooSmall.
 */
TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrHandlePdu(
    _In_reads_bytes_(InputLength) const UCHAR *Input,
    _In_ SIZE_T InputLength,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _Out_writes_bytes_to_(OutputLength, *BytesWritten) UCHAR *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten);
