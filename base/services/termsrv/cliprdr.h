/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal MS-RDPECLIP virtual channel byte helpers
 */

#pragma once

#include "rdpbcgr.h"

#include <windef.h>

#define TERMSRV_CLIPRDR_CB_MONITOR_READY          0x0001
#define TERMSRV_CLIPRDR_CB_FORMAT_LIST           0x0002
#define TERMSRV_CLIPRDR_CB_FORMAT_LIST_RESPONSE  0x0003
#define TERMSRV_CLIPRDR_CB_FORMAT_DATA_REQUEST   0x0004
#define TERMSRV_CLIPRDR_CB_FORMAT_DATA_RESPONSE  0x0005
#define TERMSRV_CLIPRDR_CB_CLIP_CAPS             0x0007
#define TERMSRV_CLIPRDR_CB_FILECONTENTS_REQUEST  0x0008
#define TERMSRV_CLIPRDR_CB_FILECONTENTS_RESPONSE 0x0009
#define TERMSRV_CLIPRDR_CB_LOCK_CLIPDATA         0x000a
#define TERMSRV_CLIPRDR_CB_UNLOCK_CLIPDATA       0x000b

#define TERMSRV_CLIPRDR_CB_RESPONSE_OK           0x0001
#define TERMSRV_CLIPRDR_CB_RESPONSE_FAIL         0x0002
#define TERMSRV_CLIPRDR_CB_ASCII_NAMES           0x0004

#define TERMSRV_CLIPRDR_CB_CAPSTYPE_GENERAL      0x0001
#define TERMSRV_CLIPRDR_CB_CAPS_VERSION_2        0x00000002
#define TERMSRV_CLIPRDR_CB_USE_LONG_FORMAT_NAMES 0x00000002
#define TERMSRV_CLIPRDR_CB_STREAM_FILECLIP_ENABLED 0x00000004
#define TERMSRV_CLIPRDR_CB_FILECLIP_NO_FILE_PATHS 0x00000008
#define TERMSRV_CLIPRDR_CB_CAN_LOCK_CLIPDATA     0x00000010
#define TERMSRV_CLIPRDR_CB_HUGE_FILE_SUPPORT_ENABLED 0x00000020

#define TERMSRV_CLIPRDR_CHANNEL_NAME             "cliprdr"
#define TERMSRV_CLIPRDR_MAX_CHANNEL_NAME_LENGTH  8
#define TERMSRV_CLIPRDR_INVALID_CHANNEL_ID       0

#define TERMSRV_CLIPRDR_CF_TEXT                  1
#define TERMSRV_CLIPRDR_CF_UNICODETEXT           13
#define TERMSRV_CLIPRDR_CF_DIB                   8
#define TERMSRV_CLIPRDR_CF_DIBV5                 17
#define TERMSRV_CLIPRDR_CF_HDROP                 15
#define TERMSRV_CLIPRDR_MAX_FORMATS              32
#define TERMSRV_CLIPRDR_MAX_FORMAT_NAME          64
#define TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS     64
#define TERMSRV_CLIPRDR_MAX_FILE_NAME            260

#define TERMSRV_CLIPRDR_FILECONTENTS_SIZE        0x00000001
#define TERMSRV_CLIPRDR_FILECONTENTS_RANGE       0x00000002

#define TERMSRV_CLIPRDR_FD_ATTRIBUTES            0x00000004
#define TERMSRV_CLIPRDR_FD_WRITESTIME            0x00000020
#define TERMSRV_CLIPRDR_FD_FILESIZE              0x00000040
#define TERMSRV_CLIPRDR_FD_PROGRESSUI            0x00004000
#define TERMSRV_CLIPRDR_FD_UNICODE               0x80000000

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
    ULONG PendingFormatId;
    BOOL ReplaceOnNextSet;
} TERMSRV_CLIPRDR_BACKEND;

#define TERMSRV_CLIPRDR_DUMMY_MAX_DATA_LENGTH    4096

typedef struct _TERMSRV_CLIPRDR_DUMMY_BACKEND
{
    BOOL HasData;
    ULONG FormatId;
    SIZE_T DataLength;
    UCHAR Data[TERMSRV_CLIPRDR_DUMMY_MAX_DATA_LENGTH];
} TERMSRV_CLIPRDR_DUMMY_BACKEND;

typedef struct _TERMSRV_CLIPRDR_WIN32_BACKEND
{
    HANDLE WindowStation;
    HANDLE Desktop;
    HWND ClipboardWindow;
    BOOL Attached;
} TERMSRV_CLIPRDR_WIN32_BACKEND;

typedef struct _TERMSRV_CLIPRDR_FORMAT
{
    ULONG FormatId;
    WCHAR Name[TERMSRV_CLIPRDR_MAX_FORMAT_NAME];
} TERMSRV_CLIPRDR_FORMAT;

typedef struct _TERMSRV_CLIPRDR_FORMAT_LIST
{
    ULONG Count;
    TERMSRV_CLIPRDR_FORMAT Formats[TERMSRV_CLIPRDR_MAX_FORMATS];
} TERMSRV_CLIPRDR_FORMAT_LIST;

typedef struct _TERMSRV_CLIPRDR_FILE_DESCRIPTOR
{
    ULONG Flags;
    ULONG Attributes;
    FILETIME CreationTime;
    FILETIME LastAccessTime;
    FILETIME LastWriteTime;
    ULONG FileSizeHigh;
    ULONG FileSizeLow;
    WCHAR FileName[TERMSRV_CLIPRDR_MAX_FILE_NAME];
} TERMSRV_CLIPRDR_FILE_DESCRIPTOR;

typedef struct _TERMSRV_CLIPRDR_FILE_LIST
{
    ULONG Count;
    TERMSRV_CLIPRDR_FILE_DESCRIPTOR Descriptors[TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS];
} TERMSRV_CLIPRDR_FILE_LIST;

typedef struct _TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST
{
    ULONG StreamId;
    ULONG ListIndex;
    ULONG Flags;
    ULONG PositionLow;
    ULONG PositionHigh;
    ULONG Requested;
    BOOL HasClipDataId;
    ULONG ClipDataId;
} TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST;

typedef struct _TERMSRV_CLIPRDR_FILE_CONTENTS_RESPONSE
{
    ULONG StreamId;
    const UCHAR *Data;
    SIZE_T DataLength;
} TERMSRV_CLIPRDR_FILE_CONTENTS_RESPONSE;

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

BOOL
TermSrvCliprdrFindStaticChannel(
    _In_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList,
    _Out_ SIZE_T *ChannelIndex);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrAssignChannelId(
    _Inout_ TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ USHORT ChannelId);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrAssignFromStaticChannelList(
    _Inout_ TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList,
    _In_ USHORT FirstStaticChannelId);

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
TermSrvCliprdrWin32BackendInit(
    _Out_ TERMSRV_CLIPRDR_WIN32_BACKEND *Win32,
    _Out_ TERMSRV_CLIPRDR_BACKEND *Backend);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWin32BackendAttach(
    _Inout_ TERMSRV_CLIPRDR_WIN32_BACKEND *Win32);

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
TermSrvCliprdrWriteCapabilities(
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
TermSrvCliprdrParseFormatList(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_CLIPRDR_FORMAT_LIST *FormatList);

BOOL
TermSrvCliprdrFormatListContains(
    _In_ const TERMSRV_CLIPRDR_FORMAT_LIST *FormatList,
    _In_ ULONG FormatId);

ULONG
TermSrvCliprdrFindNamedFormat(
    _In_ const TERMSRV_CLIPRDR_FORMAT_LIST *FormatList,
    _In_z_ const WCHAR *Name);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFormatList(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_reads_(FormatCount) const TERMSRV_CLIPRDR_FORMAT *Formats,
    _In_ ULONG FormatCount,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFormatDataRequest(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ ULONG FormatId,
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

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFileList(
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength,
    _Out_ TERMSRV_CLIPRDR_FILE_LIST *FileList);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFileList(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_CLIPRDR_FILE_LIST *FileList,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFileContentsRequest(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST *Request,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFileContentsRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST *Request);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFileContentsResponse(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT ResponseFlags,
    _In_ ULONG StreamId,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength,
    _Out_ SIZE_T *BytesWritten);

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFileContentsResponse(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_CLIPRDR_FILE_CONTENTS_RESPONSE *Response);

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

/*
 * Consumes one complete scaffold MCS Send Data packet for the assigned
 * cliprdr static virtual channel and writes at most one cliprdr response PDU.
 * Non-cliprdr channel packets write zero bytes and return UnsupportedPdu.
 */
TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrRouteMcsSendData(
    _In_reads_bytes_(InputLength) const UCHAR *Input,
    _In_ SIZE_T InputLength,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _Out_writes_bytes_to_(OutputLength, *BytesWritten) UCHAR *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten);
