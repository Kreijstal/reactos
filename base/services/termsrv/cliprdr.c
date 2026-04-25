/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal MS-RDPECLIP virtual channel byte helpers
 */

#include "cliprdr.h"

#include <string.h>

#define CLIPRDR_HEADER_LENGTH 8

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
IsValidResponseFlags(
    _In_ USHORT ResponseFlags)
{
    return ResponseFlags == TERMSRV_CLIPRDR_CB_RESPONSE_OK ||
           ResponseFlags == TERMSRV_CLIPRDR_CB_RESPONSE_FAIL;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrChannelInit(
    _Out_ TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    return TermSrvCliprdrChannelReset(Channel);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrChannelReset(
    _Out_ TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    if (Channel == NULL)
        return TermSrvCliprdrInvalidHeader;

    Channel->Enabled = FALSE;
    Channel->ChannelId = TERMSRV_CLIPRDR_INVALID_CHANNEL_ID;

    return TermSrvCliprdrSuccess;
}

BOOL
TermSrvCliprdrIsStaticChannelName(
    _In_reads_bytes_(NameLength) const CHAR *Name,
    _In_ SIZE_T NameLength)
{
    SIZE_T Index;
    static const CHAR CliprdrName[] = TERMSRV_CLIPRDR_CHANNEL_NAME;

    if (Name == NULL)
        return FALSE;

    if (NameLength < sizeof(CliprdrName) - 1)
        return FALSE;

    if (memcmp(Name, CliprdrName, sizeof(CliprdrName) - 1) != 0)
        return FALSE;

    if (NameLength == sizeof(CliprdrName) - 1)
        return TRUE;

    if (NameLength > TERMSRV_CLIPRDR_MAX_CHANNEL_NAME_LENGTH)
        return FALSE;

    for (Index = sizeof(CliprdrName) - 1; Index < NameLength; Index++)
    {
        if (Name[Index] != '\0')
            return FALSE;
    }

    return TRUE;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrAssignChannelId(
    _Inout_ TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ USHORT ChannelId)
{
    if (Channel == NULL || ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID)
        return TermSrvCliprdrInvalidHeader;

    Channel->Enabled = TRUE;
    Channel->ChannelId = ChannelId;

    return TermSrvCliprdrSuccess;
}

BOOL
TermSrvCliprdrIsChannelId(
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ USHORT ChannelId)
{
    if (Channel == NULL || ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID)
        return FALSE;

    return Channel->Enabled && Channel->ChannelId == ChannelId;
}

static TERMSRV_CLIPRDR_RESULT
DummyBackendSetData(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength)
{
    TERMSRV_CLIPRDR_DUMMY_BACKEND *Dummy;

    if (Backend == NULL || Backend->Context == NULL ||
        (DataLength != 0 && Data == NULL))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    if (DataLength > TERMSRV_CLIPRDR_DUMMY_MAX_DATA_LENGTH)
        return TermSrvCliprdrBufferTooSmall;

    Dummy = (TERMSRV_CLIPRDR_DUMMY_BACKEND *)Backend->Context;
    Dummy->HasData = TRUE;
    Dummy->FormatId = FormatId;
    Dummy->DataLength = DataLength;

    if (DataLength != 0)
        memcpy(Dummy->Data, Data, DataLength);

    return TermSrvCliprdrSuccess;
}

static TERMSRV_CLIPRDR_RESULT
DummyBackendGetData(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _Out_writes_bytes_to_opt_(BufferLength, *RequiredLength) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *RequiredLength)
{
    TERMSRV_CLIPRDR_DUMMY_BACKEND *Dummy;

    if (RequiredLength == NULL)
        return TermSrvCliprdrInvalidHeader;

    *RequiredLength = 0;

    if (Backend == NULL || Backend->Context == NULL)
        return TermSrvCliprdrInvalidHeader;

    Dummy = (TERMSRV_CLIPRDR_DUMMY_BACKEND *)Backend->Context;
    if (!Dummy->HasData || Dummy->FormatId != FormatId)
        return TermSrvCliprdrFormatNotAvailable;

    *RequiredLength = Dummy->DataLength;

    if (Dummy->DataLength != 0 && Buffer == NULL)
        return TermSrvCliprdrInvalidHeader;

    if (BufferLength < Dummy->DataLength)
        return TermSrvCliprdrBufferTooSmall;

    if (Dummy->DataLength != 0)
        memcpy(Buffer, Dummy->Data, Dummy->DataLength);

    return TermSrvCliprdrSuccess;
}

static TERMSRV_CLIPRDR_RESULT
DummyBackendClear(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    if (Backend == NULL || Backend->Context == NULL)
        return TermSrvCliprdrInvalidHeader;

    return TermSrvCliprdrDummyBackendReset(
        (TERMSRV_CLIPRDR_DUMMY_BACKEND *)Backend->Context);
}

static const TERMSRV_CLIPRDR_BACKEND_OPS DummyBackendOps =
{
    DummyBackendSetData,
    DummyBackendGetData,
    DummyBackendClear
};

static BOOL
IsValidBackend(
    _In_ const TERMSRV_CLIPRDR_BACKEND *Backend)
{
    return Backend != NULL &&
           Backend->Ops != NULL &&
           Backend->Context != NULL;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrBackendSetData(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength)
{
    if (!IsValidBackend(Backend) || Backend->Ops->SetData == NULL ||
        (DataLength != 0 && Data == NULL))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    return Backend->Ops->SetData(Backend, FormatId, Data, DataLength);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrBackendGetData(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _Out_writes_bytes_to_opt_(BufferLength, *RequiredLength) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *RequiredLength)
{
    if (RequiredLength == NULL)
        return TermSrvCliprdrInvalidHeader;

    *RequiredLength = 0;

    if (!IsValidBackend(Backend) || Backend->Ops->GetData == NULL)
        return TermSrvCliprdrInvalidHeader;

    return Backend->Ops->GetData(Backend,
                                 FormatId,
                                 Buffer,
                                 BufferLength,
                                 RequiredLength);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrBackendClear(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    if (!IsValidBackend(Backend) || Backend->Ops->Clear == NULL)
        return TermSrvCliprdrInvalidHeader;

    return Backend->Ops->Clear(Backend);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrDummyBackendInit(
    _Out_ TERMSRV_CLIPRDR_DUMMY_BACKEND *Dummy,
    _Out_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    TERMSRV_CLIPRDR_RESULT Result;

    if (Dummy == NULL || Backend == NULL)
        return TermSrvCliprdrInvalidHeader;

    Result = TermSrvCliprdrDummyBackendReset(Dummy);
    if (Result != TermSrvCliprdrSuccess)
        return Result;

    Backend->Ops = &DummyBackendOps;
    Backend->Context = Dummy;

    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrDummyBackendReset(
    _Out_ TERMSRV_CLIPRDR_DUMMY_BACKEND *Dummy)
{
    if (Dummy == NULL)
        return TermSrvCliprdrInvalidHeader;

    memset(Dummy, 0, sizeof(*Dummy));
    return TermSrvCliprdrSuccess;
}

static TERMSRV_CLIPRDR_RESULT
WritePdu(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT MsgType,
    _In_ USHORT MsgFlags,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T TotalLength;

    if (BytesWritten == NULL)
        return TermSrvCliprdrInvalidHeader;

    *BytesWritten = 0;

    if (Buffer == NULL || (DataLength != 0 && Data == NULL))
        return TermSrvCliprdrInvalidHeader;

    if (DataLength > 0xFFFFFFFFUL)
        return TermSrvCliprdrInvalidLength;

    if (DataLength > (SIZE_T)-1 - CLIPRDR_HEADER_LENGTH)
        return TermSrvCliprdrInvalidLength;

    TotalLength = CLIPRDR_HEADER_LENGTH + DataLength;
    if (BufferLength < TotalLength)
        return TermSrvCliprdrBufferTooSmall;

    WriteLe16(&Buffer[0], MsgType);
    WriteLe16(&Buffer[2], MsgFlags);
    WriteLe32(&Buffer[4], (ULONG)DataLength);

    if (DataLength != 0)
        memcpy(&Buffer[CLIPRDR_HEADER_LENGTH], Data, DataLength);

    *BytesWritten = TotalLength;
    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParsePdu(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_CLIPRDR_PDU *Pdu)
{
    ULONG DataLength;
    SIZE_T TotalLength;

    if (Pdu == NULL || Buffer == NULL)
        return TermSrvCliprdrInvalidHeader;

    memset(Pdu, 0, sizeof(*Pdu));

    if (BufferLength < CLIPRDR_HEADER_LENGTH)
        return TermSrvCliprdrNeedMoreData;

    DataLength = ReadLe32(&Buffer[4]);
    if ((SIZE_T)DataLength > (SIZE_T)-1 - CLIPRDR_HEADER_LENGTH)
        return TermSrvCliprdrInvalidLength;

    TotalLength = CLIPRDR_HEADER_LENGTH + (SIZE_T)DataLength;
    if (TotalLength > BufferLength)
        return TermSrvCliprdrNeedMoreData;

    if (TotalLength != BufferLength)
        return TermSrvCliprdrInvalidLength;

    Pdu->MsgType = ReadLe16(&Buffer[0]);
    Pdu->MsgFlags = ReadLe16(&Buffer[2]);
    Pdu->DataLength = DataLength;
    Pdu->Payload = &Buffer[CLIPRDR_HEADER_LENGTH];

    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteMonitorReady(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    return WritePdu(Buffer,
                    BufferLength,
                    TERMSRV_CLIPRDR_CB_MONITOR_READY,
                    0,
                    NULL,
                    0,
                    BytesWritten);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFormatListResponse(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT ResponseFlags,
    _Out_ SIZE_T *BytesWritten)
{
    if (BytesWritten != NULL)
        *BytesWritten = 0;

    if (!IsValidResponseFlags(ResponseFlags))
        return TermSrvCliprdrInvalidHeader;

    return WritePdu(Buffer,
                    BufferLength,
                    TERMSRV_CLIPRDR_CB_FORMAT_LIST_RESPONSE,
                    ResponseFlags,
                    NULL,
                    0,
                    BytesWritten);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFormatDataRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ ULONG *FormatId)
{
    TERMSRV_CLIPRDR_PDU Pdu;
    TERMSRV_CLIPRDR_RESULT Result;

    if (FormatId == NULL)
        return TermSrvCliprdrInvalidHeader;

    *FormatId = 0;

    Result = TermSrvCliprdrParsePdu(Buffer, BufferLength, &Pdu);
    if (Result != TermSrvCliprdrSuccess)
        return Result;

    if (Pdu.MsgType != TERMSRV_CLIPRDR_CB_FORMAT_DATA_REQUEST)
        return TermSrvCliprdrUnsupportedPdu;

    if (Pdu.DataLength != sizeof(ULONG))
        return TermSrvCliprdrInvalidLength;

    *FormatId = ReadLe32(Pdu.Payload);
    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFormatDataResponse(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT ResponseFlags,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength,
    _Out_ SIZE_T *BytesWritten)
{
    if (BytesWritten != NULL)
        *BytesWritten = 0;

    if (!IsValidResponseFlags(ResponseFlags))
        return TermSrvCliprdrInvalidHeader;

    return WritePdu(Buffer,
                    BufferLength,
                    TERMSRV_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
                    ResponseFlags,
                    Data,
                    DataLength,
                    BytesWritten);
}
