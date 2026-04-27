/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal MS-RDPECLIP virtual channel byte helpers
 */

#include "cliprdr.h"
#include "rdpbcgr.h"

#include <windows.h>
#include <string.h>

#define CLIPRDR_HEADER_LENGTH 8
#define CHANNEL_PDU_HEADER_LENGTH 8
#define CHANNEL_FLAG_FIRST 0x00000001
#define CHANNEL_FLAG_LAST 0x00000002
#define CHANNEL_FLAG_SHOW_PROTOCOL 0x00000010

#define CLIPRDR_CLIPBOARD_WINDOW_CLASS L"ReactOSTermSrvCliprdrWindow"
#define CLIPRDR_FILEDESCRIPTORW_LENGTH 592
#define CLIPRDR_FILEDESCRIPTORW_NAME_OFFSET 72

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

BOOL
TermSrvCliprdrFindStaticChannel(
    _In_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList,
    _Out_ SIZE_T *ChannelIndex)
{
    SIZE_T Index;

    if (ChannelIndex == NULL)
        return FALSE;

    *ChannelIndex = 0;

    if (ChannelList == NULL)
        return FALSE;

    for (Index = 0; Index < ChannelList->Count; Index++)
    {
        if (TermSrvCliprdrIsStaticChannelName(
                (const CHAR *)ChannelList->Channels[Index].Name,
                TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH))
        {
            *ChannelIndex = ChannelList->Channels[Index].Index;
            return TRUE;
        }
    }

    return FALSE;
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

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrAssignFromStaticChannelList(
    _Inout_ TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList,
    _In_ USHORT FirstStaticChannelId)
{
    SIZE_T ChannelIndex;
    SIZE_T ChannelId;

    if (Channel == NULL)
        return TermSrvCliprdrInvalidHeader;

    TermSrvCliprdrChannelReset(Channel);

    if (ChannelList == NULL ||
        FirstStaticChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID)
    {
        return TermSrvCliprdrInvalidHeader;
    }

    if (!TermSrvCliprdrFindStaticChannel(ChannelList, &ChannelIndex))
        return TermSrvCliprdrUnsupportedPdu;

    if (ChannelIndex > (SIZE_T)0xffff - FirstStaticChannelId)
        return TermSrvCliprdrInvalidLength;

    ChannelId = FirstStaticChannelId + ChannelIndex;
    return TermSrvCliprdrAssignChannelId(Channel, (USHORT)ChannelId);
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

static BOOL
TryUnwrapVirtualChannelPayload(
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _Outptr_result_bytebuffer_(*CliprdrLength) const UCHAR **CliprdrPayload,
    _Out_ SIZE_T *CliprdrLength)
{
    ULONG DeclaredLength;
    ULONG Flags;

    if (CliprdrPayload == NULL || CliprdrLength == NULL)
        return FALSE;

    *CliprdrPayload = Payload;
    *CliprdrLength = PayloadLength;

    if (Payload == NULL || PayloadLength < CHANNEL_PDU_HEADER_LENGTH)
        return FALSE;

    DeclaredLength = ReadLe32(&Payload[0]);
    Flags = ReadLe32(&Payload[4]);
    if ((Flags & (CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST)) !=
        (CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST))
    {
        return FALSE;
    }

    if (DeclaredLength > PayloadLength - CHANNEL_PDU_HEADER_LENGTH)
        return FALSE;

    *CliprdrPayload = Payload + CHANNEL_PDU_HEADER_LENGTH;
    *CliprdrLength = DeclaredLength;
    return TRUE;
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

static BOOL
IsSupportedWin32ClipboardFormat(
    _In_ ULONG FormatId)
{
    return FormatId == TERMSRV_CLIPRDR_CF_UNICODETEXT ||
           FormatId == TERMSRV_CLIPRDR_CF_TEXT ||
           FormatId == TERMSRV_CLIPRDR_CF_HDROP ||
           FormatId == TERMSRV_CLIPRDR_CF_DIB ||
           FormatId == TERMSRV_CLIPRDR_CF_DIBV5 ||
           FormatId >= 0xc000;
}

static LRESULT CALLBACK
CliprdrClipboardWndProc(
    _In_ HWND Wnd,
    _In_ UINT Msg,
    _In_ WPARAM WParam,
    _In_ LPARAM LParam)
{
    return DefWindowProcW(Wnd, Msg, WParam, LParam);
}

static TERMSRV_CLIPRDR_RESULT
EnsureClipboardOwnerWindow(
    _Inout_ TERMSRV_CLIPRDR_WIN32_BACKEND *Win32)
{
    WNDCLASSW Class;
    HINSTANCE Instance;

    if (Win32->ClipboardWindow != NULL &&
        IsWindow(Win32->ClipboardWindow))
    {
        return TermSrvCliprdrSuccess;
    }

    Instance = GetModuleHandleW(NULL);

    memset(&Class, 0, sizeof(Class));
    Class.lpfnWndProc = CliprdrClipboardWndProc;
    Class.hInstance = Instance;
    Class.lpszClassName = CLIPRDR_CLIPBOARD_WINDOW_CLASS;

    if (!RegisterClassW(&Class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return TermSrvCliprdrInvalidHeader;
    }

    Win32->ClipboardWindow = CreateWindowExW(0,
                                            CLIPRDR_CLIPBOARD_WINDOW_CLASS,
                                            L"",
                                            WS_POPUP,
                                            0,
                                            0,
                                            0,
                                            0,
                                            NULL,
                                            NULL,
                                            Instance,
                                            NULL);
    if (Win32->ClipboardWindow == NULL)
    {
        return TermSrvCliprdrInvalidHeader;
    }

    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWin32BackendAttach(
    _Inout_ TERMSRV_CLIPRDR_WIN32_BACKEND *Win32)
{
    DWORD DesktopAccess;

    if (Win32 == NULL)
        return TermSrvCliprdrInvalidHeader;

    if (Win32->Attached)
        return EnsureClipboardOwnerWindow(Win32);

    Win32->WindowStation = OpenWindowStationW(L"WinSta0",
                                              FALSE,
                                              WINSTA_ACCESSCLIPBOARD |
                                              WINSTA_READATTRIBUTES |
                                              WINSTA_ENUMDESKTOPS);
    if (Win32->WindowStation == NULL)
    {
        return TermSrvCliprdrInvalidHeader;
    }

    if (!SetProcessWindowStation((HWINSTA)Win32->WindowStation))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    DesktopAccess = DESKTOP_READOBJECTS |
                    DESKTOP_WRITEOBJECTS |
                    DESKTOP_CREATEWINDOW;

    Win32->Desktop = OpenInputDesktop(0, FALSE, DesktopAccess);
    if (Win32->Desktop == NULL)
    {
        Win32->Desktop = OpenDesktopW(L"Default", 0, FALSE, DesktopAccess);
        if (Win32->Desktop == NULL)
        {
            return TermSrvCliprdrInvalidHeader;
        }
    }

    if (!SetThreadDesktop((HDESK)Win32->Desktop))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    if (EnsureClipboardOwnerWindow(Win32) != TermSrvCliprdrSuccess)
        return TermSrvCliprdrInvalidHeader;

    Win32->Attached = TRUE;
    return TermSrvCliprdrSuccess;
}

static TERMSRV_CLIPRDR_RESULT
Win32BackendSetData(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength)
{
    TERMSRV_CLIPRDR_WIN32_BACKEND *Win32;
    HGLOBAL Global;
    UCHAR *Target;
    SIZE_T AllocLength;

    if (Backend == NULL || Backend->Context == NULL ||
        (DataLength != 0 && Data == NULL))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    if (!IsSupportedWin32ClipboardFormat(FormatId))
        return TermSrvCliprdrFormatNotAvailable;

    Win32 = (TERMSRV_CLIPRDR_WIN32_BACKEND *)Backend->Context;
    if (TermSrvCliprdrWin32BackendAttach(Win32) != TermSrvCliprdrSuccess)
        return TermSrvCliprdrInvalidHeader;

    AllocLength = DataLength;
    if (FormatId == TERMSRV_CLIPRDR_CF_UNICODETEXT)
    {
        if (AllocLength > (SIZE_T)-1 - sizeof(WCHAR))
            return TermSrvCliprdrInvalidLength;
        AllocLength += sizeof(WCHAR);
    }
    else if (FormatId == TERMSRV_CLIPRDR_CF_TEXT)
    {
        if (AllocLength > (SIZE_T)-1 - sizeof(CHAR))
            return TermSrvCliprdrInvalidLength;
        AllocLength += sizeof(CHAR);
    }
    if (AllocLength == 0)
        AllocLength = 1;

    Global = GlobalAlloc(GMEM_MOVEABLE, AllocLength);
    if (Global == NULL)
    {
        return TermSrvCliprdrBufferTooSmall;
    }

    Target = (UCHAR *)GlobalLock(Global);
    if (Target == NULL)
    {
        GlobalFree(Global);
        return TermSrvCliprdrInvalidHeader;
    }

    if (DataLength != 0)
        memcpy(Target, Data, DataLength);
    if (FormatId == TERMSRV_CLIPRDR_CF_UNICODETEXT)
        memset(Target + DataLength, 0, sizeof(WCHAR));
    else if (FormatId == TERMSRV_CLIPRDR_CF_TEXT)
        memset(Target + DataLength, 0, sizeof(CHAR));
    GlobalUnlock(Global);

    if (!OpenClipboard(Win32->ClipboardWindow))
    {
        GlobalFree(Global);
        return TermSrvCliprdrInvalidHeader;
    }

    if (Backend->ReplaceOnNextSet)
    {
        EmptyClipboard();
        Backend->ReplaceOnNextSet = FALSE;
    }
    if (SetClipboardData((UINT)FormatId, Global) == NULL)
    {
        CloseClipboard();
        GlobalFree(Global);
        return TermSrvCliprdrInvalidHeader;
    }

    CloseClipboard();
    return TermSrvCliprdrSuccess;
}

static TERMSRV_CLIPRDR_RESULT
Win32BackendGetData(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _In_ ULONG FormatId,
    _Out_writes_bytes_to_opt_(BufferLength, *RequiredLength) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *RequiredLength)
{
    TERMSRV_CLIPRDR_WIN32_BACKEND *Win32;
    HANDLE Data;
    const UCHAR *Source;
    SIZE_T Length;

    if (RequiredLength == NULL)
        return TermSrvCliprdrInvalidHeader;
    *RequiredLength = 0;

    if (Backend == NULL || Backend->Context == NULL)
        return TermSrvCliprdrInvalidHeader;

    if (!IsSupportedWin32ClipboardFormat(FormatId))
        return TermSrvCliprdrFormatNotAvailable;

    Win32 = (TERMSRV_CLIPRDR_WIN32_BACKEND *)Backend->Context;
    if (TermSrvCliprdrWin32BackendAttach(Win32) != TermSrvCliprdrSuccess)
        return TermSrvCliprdrInvalidHeader;

    if (!IsClipboardFormatAvailable((UINT)FormatId))
        return TermSrvCliprdrFormatNotAvailable;

    if (!OpenClipboard(Win32->ClipboardWindow))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    Data = GetClipboardData((UINT)FormatId);
    if (Data == NULL)
    {
        CloseClipboard();
        return TermSrvCliprdrFormatNotAvailable;
    }

    Source = (const UCHAR *)GlobalLock(Data);
    if (Source == NULL)
    {
        CloseClipboard();
        return TermSrvCliprdrInvalidHeader;
    }

    Length = GlobalSize(Data);
    if (FormatId == TERMSRV_CLIPRDR_CF_UNICODETEXT)
    {
        const WCHAR *Text = (const WCHAR *)Source;
        SIZE_T Chars = 0;

        while ((Chars + 1) * sizeof(WCHAR) <= Length && Text[Chars] != 0)
            Chars++;
        Length = (Chars + 1) * sizeof(WCHAR);
    }
    else if (FormatId == TERMSRV_CLIPRDR_CF_TEXT)
    {
        const CHAR *Text = (const CHAR *)Source;
        SIZE_T Chars = 0;

        while (Chars < Length && Text[Chars] != 0)
            Chars++;
        if (Chars < Length)
            Chars++;
        Length = Chars;
    }

    *RequiredLength = Length;
    if (BufferLength < Length)
    {
        GlobalUnlock(Data);
        CloseClipboard();
        return TermSrvCliprdrBufferTooSmall;
    }

    if (Length != 0 && Buffer == NULL)
    {
        GlobalUnlock(Data);
        CloseClipboard();
        return TermSrvCliprdrInvalidHeader;
    }

    if (Length != 0)
        memcpy(Buffer, Source, Length);

    GlobalUnlock(Data);
    CloseClipboard();
    return TermSrvCliprdrSuccess;
}

static TERMSRV_CLIPRDR_RESULT
Win32BackendClear(
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    TERMSRV_CLIPRDR_WIN32_BACKEND *Win32;

    if (Backend == NULL || Backend->Context == NULL)
        return TermSrvCliprdrInvalidHeader;

    Win32 = (TERMSRV_CLIPRDR_WIN32_BACKEND *)Backend->Context;
    if (TermSrvCliprdrWin32BackendAttach(Win32) != TermSrvCliprdrSuccess)
        return TermSrvCliprdrInvalidHeader;

    if (!OpenClipboard(Win32->ClipboardWindow))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    EmptyClipboard();
    CloseClipboard();
    return TermSrvCliprdrSuccess;
}

static const TERMSRV_CLIPRDR_BACKEND_OPS Win32BackendOps =
{
    Win32BackendSetData,
    Win32BackendGetData,
    Win32BackendClear
};

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
    Backend->PendingFormatId = 0;
    Backend->ReplaceOnNextSet = TRUE;

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

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWin32BackendInit(
    _Out_ TERMSRV_CLIPRDR_WIN32_BACKEND *Win32,
    _Out_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    if (Win32 == NULL || Backend == NULL)
        return TermSrvCliprdrInvalidHeader;

    memset(Win32, 0, sizeof(*Win32));
    Backend->Ops = &Win32BackendOps;
    Backend->Context = Win32;
    Backend->PendingFormatId = 0;
    Backend->ReplaceOnNextSet = TRUE;
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
TermSrvCliprdrWriteCapabilities(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Payload[16];
    ULONG GeneralFlags;

    GeneralFlags = TERMSRV_CLIPRDR_CB_USE_LONG_FORMAT_NAMES |
                   TERMSRV_CLIPRDR_CB_STREAM_FILECLIP_ENABLED |
                   TERMSRV_CLIPRDR_CB_FILECLIP_NO_FILE_PATHS |
                   TERMSRV_CLIPRDR_CB_HUGE_FILE_SUPPORT_ENABLED;

    WriteLe16(&Payload[0], 1);
    WriteLe16(&Payload[2], 0);
    WriteLe16(&Payload[4], TERMSRV_CLIPRDR_CB_CAPSTYPE_GENERAL);
    WriteLe16(&Payload[6], 12);
    WriteLe32(&Payload[8], TERMSRV_CLIPRDR_CB_CAPS_VERSION_2);
    WriteLe32(&Payload[12], GeneralFlags);

    return WritePdu(Buffer,
                    BufferLength,
                    TERMSRV_CLIPRDR_CB_CLIP_CAPS,
                    0,
                    Payload,
                    sizeof(Payload),
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

static BOOL
IsSupportedPhaseOneFormat(
    _In_ ULONG FormatId)
{
    return FormatId == TERMSRV_CLIPRDR_CF_UNICODETEXT ||
           FormatId == TERMSRV_CLIPRDR_CF_TEXT ||
           FormatId == TERMSRV_CLIPRDR_CF_DIB ||
           FormatId == TERMSRV_CLIPRDR_CF_DIBV5;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFormatList(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_CLIPRDR_FORMAT_LIST *FormatList)
{
    TERMSRV_CLIPRDR_PDU Pdu;
    TERMSRV_CLIPRDR_RESULT Result;
    SIZE_T Offset;

    if (FormatList == NULL)
        return TermSrvCliprdrInvalidHeader;

    memset(FormatList, 0, sizeof(*FormatList));

    Result = TermSrvCliprdrParsePdu(Buffer, BufferLength, &Pdu);
    if (Result != TermSrvCliprdrSuccess)
        return Result;

    if (Pdu.MsgType != TERMSRV_CLIPRDR_CB_FORMAT_LIST)
        return TermSrvCliprdrUnsupportedPdu;

    Offset = 0;
    if (Pdu.MsgFlags & TERMSRV_CLIPRDR_CB_ASCII_NAMES)
    {
        while (Offset + 36 <= Pdu.DataLength &&
               FormatList->Count < TERMSRV_CLIPRDR_MAX_FORMATS)
        {
            ULONG Count = FormatList->Count++;
            ULONG Index;

            FormatList->Formats[Count].FormatId = ReadLe32(&Pdu.Payload[Offset]);
            for (Index = 0; Index + 1 < TERMSRV_CLIPRDR_MAX_FORMAT_NAME &&
                 Index < 32 && Pdu.Payload[Offset + 4 + Index] != 0; Index++)
            {
                FormatList->Formats[Count].Name[Index] =
                    (WCHAR)Pdu.Payload[Offset + 4 + Index];
            }
            Offset += 36;
        }
        return TermSrvCliprdrSuccess;
    }

    while (Offset + 4 <= Pdu.DataLength &&
           FormatList->Count < TERMSRV_CLIPRDR_MAX_FORMATS)
    {
        ULONG Count = FormatList->Count++;
        ULONG NameIndex = 0;

        FormatList->Formats[Count].FormatId = ReadLe32(&Pdu.Payload[Offset]);
        Offset += 4;

        while (Offset + 1 < Pdu.DataLength)
        {
            WCHAR Ch = (WCHAR)ReadLe16(&Pdu.Payload[Offset]);
            Offset += 2;
            if (Ch == 0)
                break;
            if (NameIndex + 1 < TERMSRV_CLIPRDR_MAX_FORMAT_NAME)
                FormatList->Formats[Count].Name[NameIndex++] = Ch;
        }
    }

    return TermSrvCliprdrSuccess;
}

BOOL
TermSrvCliprdrFormatListContains(
    _In_ const TERMSRV_CLIPRDR_FORMAT_LIST *FormatList,
    _In_ ULONG FormatId)
{
    ULONG Index;

    if (FormatList == NULL)
        return FALSE;

    for (Index = 0; Index < FormatList->Count; Index++)
    {
        if (FormatList->Formats[Index].FormatId == FormatId)
            return TRUE;
    }

    return FALSE;
}

ULONG
TermSrvCliprdrFindNamedFormat(
    _In_ const TERMSRV_CLIPRDR_FORMAT_LIST *FormatList,
    _In_z_ const WCHAR *Name)
{
    ULONG Index;

    if (FormatList == NULL || Name == NULL)
        return 0;

    for (Index = 0; Index < FormatList->Count; Index++)
    {
        if (lstrcmpiW(FormatList->Formats[Index].Name, Name) == 0)
            return FormatList->Formats[Index].FormatId;
    }

    return 0;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFormatList(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_reads_(FormatCount) const TERMSRV_CLIPRDR_FORMAT *Formats,
    _In_ ULONG FormatCount,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Payload[1024];
    SIZE_T Offset;
    ULONG Index;

    if (Formats == NULL && FormatCount != 0)
        return TermSrvCliprdrInvalidHeader;

    Offset = 0;
    for (Index = 0; Index < FormatCount; Index++)
    {
        SIZE_T NameIndex;

        if (Offset + 6 > sizeof(Payload))
            return TermSrvCliprdrBufferTooSmall;

        WriteLe32(&Payload[Offset], Formats[Index].FormatId);
        Offset += 4;

        for (NameIndex = 0;
             Formats[Index].Name[NameIndex] != 0 &&
             NameIndex < TERMSRV_CLIPRDR_MAX_FORMAT_NAME - 1;
             NameIndex++)
        {
            if (Offset + 2 > sizeof(Payload))
                return TermSrvCliprdrBufferTooSmall;
            WriteLe16(&Payload[Offset], Formats[Index].Name[NameIndex]);
            Offset += 2;
        }

        WriteLe16(&Payload[Offset], 0);
        Offset += 2;
    }

    return WritePdu(Buffer,
                    BufferLength,
                    TERMSRV_CLIPRDR_CB_FORMAT_LIST,
                    0,
                    Payload,
                    Offset,
                    BytesWritten);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFormatDataRequest(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ ULONG FormatId,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Payload[4];

    WriteLe32(Payload, FormatId);
    return WritePdu(Buffer,
                    BufferLength,
                    TERMSRV_CLIPRDR_CB_FORMAT_DATA_REQUEST,
                    0,
                    Payload,
                    sizeof(Payload),
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

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFileList(
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength,
    _Out_ TERMSRV_CLIPRDR_FILE_LIST *FileList)
{
    ULONG Count;
    ULONG Index;

    if (FileList == NULL)
        return TermSrvCliprdrInvalidHeader;

    memset(FileList, 0, sizeof(*FileList));

    if (Data == NULL || DataLength < sizeof(ULONG))
        return TermSrvCliprdrInvalidHeader;

    Count = ReadLe32(Data);
    if (Count > TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS)
        return TermSrvCliprdrBufferTooSmall;

    if ((DataLength - sizeof(ULONG)) / CLIPRDR_FILEDESCRIPTORW_LENGTH < Count)
        return TermSrvCliprdrInvalidLength;

    FileList->Count = Count;
    for (Index = 0; Index < Count; Index++)
    {
        const UCHAR *Descriptor;
        TERMSRV_CLIPRDR_FILE_DESCRIPTOR *Output;
        ULONG NameIndex;

        Descriptor = Data + sizeof(ULONG) +
                     (SIZE_T)Index * CLIPRDR_FILEDESCRIPTORW_LENGTH;
        Output = &FileList->Descriptors[Index];

        Output->Flags = ReadLe32(&Descriptor[0]);
        Output->Attributes = ReadLe32(&Descriptor[36]);
        Output->CreationTime.dwLowDateTime = ReadLe32(&Descriptor[40]);
        Output->CreationTime.dwHighDateTime = ReadLe32(&Descriptor[44]);
        Output->LastAccessTime.dwLowDateTime = ReadLe32(&Descriptor[48]);
        Output->LastAccessTime.dwHighDateTime = ReadLe32(&Descriptor[52]);
        Output->LastWriteTime.dwLowDateTime = ReadLe32(&Descriptor[56]);
        Output->LastWriteTime.dwHighDateTime = ReadLe32(&Descriptor[60]);
        Output->FileSizeHigh = ReadLe32(&Descriptor[64]);
        Output->FileSizeLow = ReadLe32(&Descriptor[68]);

        for (NameIndex = 0;
             NameIndex < TERMSRV_CLIPRDR_MAX_FILE_NAME - 1;
             NameIndex++)
        {
            WCHAR Ch;

            Ch = (WCHAR)ReadLe16(&Descriptor[CLIPRDR_FILEDESCRIPTORW_NAME_OFFSET +
                                             NameIndex * sizeof(WCHAR)]);
            if (Ch == 0)
                break;
            Output->FileName[NameIndex] = Ch;
        }
    }

    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFileList(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_CLIPRDR_FILE_LIST *FileList,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T Required;
    ULONG Index;

    if (BytesWritten == NULL)
        return TermSrvCliprdrInvalidHeader;
    *BytesWritten = 0;

    if (Buffer == NULL || FileList == NULL ||
        FileList->Count > TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS)
    {
        return TermSrvCliprdrInvalidHeader;
    }

    Required = sizeof(ULONG) +
               (SIZE_T)FileList->Count * CLIPRDR_FILEDESCRIPTORW_LENGTH;
    if (BufferLength < Required)
        return TermSrvCliprdrBufferTooSmall;

    memset(Buffer, 0, Required);
    WriteLe32(Buffer, FileList->Count);

    for (Index = 0; Index < FileList->Count; Index++)
    {
        UCHAR *Descriptor;
        const TERMSRV_CLIPRDR_FILE_DESCRIPTOR *Input;
        ULONG NameIndex;

        Descriptor = Buffer + sizeof(ULONG) +
                     (SIZE_T)Index * CLIPRDR_FILEDESCRIPTORW_LENGTH;
        Input = &FileList->Descriptors[Index];

        WriteLe32(&Descriptor[0], Input->Flags);
        WriteLe32(&Descriptor[36], Input->Attributes);
        WriteLe32(&Descriptor[40], Input->CreationTime.dwLowDateTime);
        WriteLe32(&Descriptor[44], Input->CreationTime.dwHighDateTime);
        WriteLe32(&Descriptor[48], Input->LastAccessTime.dwLowDateTime);
        WriteLe32(&Descriptor[52], Input->LastAccessTime.dwHighDateTime);
        WriteLe32(&Descriptor[56], Input->LastWriteTime.dwLowDateTime);
        WriteLe32(&Descriptor[60], Input->LastWriteTime.dwHighDateTime);
        WriteLe32(&Descriptor[64], Input->FileSizeHigh);
        WriteLe32(&Descriptor[68], Input->FileSizeLow);

        for (NameIndex = 0;
             NameIndex < TERMSRV_CLIPRDR_MAX_FILE_NAME - 1 &&
             Input->FileName[NameIndex] != 0;
             NameIndex++)
        {
            WriteLe16(&Descriptor[CLIPRDR_FILEDESCRIPTORW_NAME_OFFSET +
                                  NameIndex * sizeof(WCHAR)],
                      Input->FileName[NameIndex]);
        }
    }

    *BytesWritten = Required;
    return TermSrvCliprdrSuccess;
}

static TERMSRV_CLIPRDR_RESULT
ValidateFileContentsRequest(
    _In_ const TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST *Request)
{
    if (Request == NULL)
        return TermSrvCliprdrInvalidHeader;

    if ((Request->Flags & (TERMSRV_CLIPRDR_FILECONTENTS_SIZE |
                           TERMSRV_CLIPRDR_FILECONTENTS_RANGE)) ==
        (TERMSRV_CLIPRDR_FILECONTENTS_SIZE |
         TERMSRV_CLIPRDR_FILECONTENTS_RANGE))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    if (Request->Flags & TERMSRV_CLIPRDR_FILECONTENTS_SIZE)
    {
        if (Request->Requested != sizeof(ULONGLONG) ||
            Request->PositionLow != 0 ||
            Request->PositionHigh != 0)
        {
            return TermSrvCliprdrInvalidLength;
        }
    }

    if ((Request->Flags & (TERMSRV_CLIPRDR_FILECONTENTS_SIZE |
                           TERMSRV_CLIPRDR_FILECONTENTS_RANGE)) == 0)
    {
        return TermSrvCliprdrInvalidHeader;
    }

    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFileContentsRequest(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST *Request,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Payload[28];
    SIZE_T PayloadLength;
    TERMSRV_CLIPRDR_RESULT Result;

    if (BytesWritten == NULL)
        return TermSrvCliprdrInvalidHeader;
    *BytesWritten = 0;

    Result = ValidateFileContentsRequest(Request);
    if (Result != TermSrvCliprdrSuccess)
        return Result;

    PayloadLength = Request->HasClipDataId ? 28 : 24;
    WriteLe32(&Payload[0], Request->StreamId);
    WriteLe32(&Payload[4], Request->ListIndex);
    WriteLe32(&Payload[8], Request->Flags);
    WriteLe32(&Payload[12], Request->PositionLow);
    WriteLe32(&Payload[16], Request->PositionHigh);
    WriteLe32(&Payload[20], Request->Requested);
    if (Request->HasClipDataId)
        WriteLe32(&Payload[24], Request->ClipDataId);

    return WritePdu(Buffer,
                    BufferLength,
                    TERMSRV_CLIPRDR_CB_FILECONTENTS_REQUEST,
                    0,
                    Payload,
                    PayloadLength,
                    BytesWritten);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFileContentsRequest(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST *Request)
{
    TERMSRV_CLIPRDR_PDU Pdu;
    TERMSRV_CLIPRDR_RESULT Result;

    if (Request == NULL)
        return TermSrvCliprdrInvalidHeader;
    memset(Request, 0, sizeof(*Request));

    Result = TermSrvCliprdrParsePdu(Buffer, BufferLength, &Pdu);
    if (Result != TermSrvCliprdrSuccess)
        return Result;

    if (Pdu.MsgType != TERMSRV_CLIPRDR_CB_FILECONTENTS_REQUEST)
        return TermSrvCliprdrUnsupportedPdu;

    if (Pdu.DataLength != 24 && Pdu.DataLength != 28)
        return TermSrvCliprdrInvalidLength;

    Request->StreamId = ReadLe32(&Pdu.Payload[0]);
    Request->ListIndex = ReadLe32(&Pdu.Payload[4]);
    Request->Flags = ReadLe32(&Pdu.Payload[8]);
    Request->PositionLow = ReadLe32(&Pdu.Payload[12]);
    Request->PositionHigh = ReadLe32(&Pdu.Payload[16]);
    Request->Requested = ReadLe32(&Pdu.Payload[20]);
    if (Pdu.DataLength == 28)
    {
        Request->HasClipDataId = TRUE;
        Request->ClipDataId = ReadLe32(&Pdu.Payload[24]);
    }

    return ValidateFileContentsRequest(Request);
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrWriteFileContentsResponse(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT ResponseFlags,
    _In_ ULONG StreamId,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T TotalLength;

    if (BytesWritten == NULL)
        return TermSrvCliprdrInvalidHeader;
    *BytesWritten = 0;

    if (!IsValidResponseFlags(ResponseFlags) ||
        (DataLength != 0 && Data == NULL))
    {
        return TermSrvCliprdrInvalidHeader;
    }

    TotalLength = CLIPRDR_HEADER_LENGTH + sizeof(ULONG) + DataLength;
    if (Buffer == NULL || BufferLength < TotalLength)
        return TermSrvCliprdrBufferTooSmall;

    WriteLe16(&Buffer[0], TERMSRV_CLIPRDR_CB_FILECONTENTS_RESPONSE);
    WriteLe16(&Buffer[2], ResponseFlags);
    WriteLe32(&Buffer[4], (ULONG)(sizeof(ULONG) + DataLength));
    WriteLe32(&Buffer[CLIPRDR_HEADER_LENGTH], StreamId);
    if (DataLength != 0)
        memcpy(&Buffer[CLIPRDR_HEADER_LENGTH + sizeof(ULONG)], Data, DataLength);

    *BytesWritten = TotalLength;
    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrParseFileContentsResponse(
    _In_reads_bytes_(BufferLength) const UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ TERMSRV_CLIPRDR_FILE_CONTENTS_RESPONSE *Response)
{
    TERMSRV_CLIPRDR_PDU Pdu;
    TERMSRV_CLIPRDR_RESULT Result;

    if (Response == NULL)
        return TermSrvCliprdrInvalidHeader;
    memset(Response, 0, sizeof(*Response));

    Result = TermSrvCliprdrParsePdu(Buffer, BufferLength, &Pdu);
    if (Result != TermSrvCliprdrSuccess)
        return Result;

    if (Pdu.MsgType != TERMSRV_CLIPRDR_CB_FILECONTENTS_RESPONSE)
        return TermSrvCliprdrUnsupportedPdu;

    if (Pdu.DataLength < sizeof(ULONG))
        return TermSrvCliprdrInvalidLength;

    Response->StreamId = ReadLe32(Pdu.Payload);
    Response->Data = &Pdu.Payload[sizeof(ULONG)];
    Response->DataLength = Pdu.DataLength - sizeof(ULONG);
    return TermSrvCliprdrSuccess;
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrHandlePdu(
    _In_reads_bytes_(InputLength) const UCHAR *Input,
    _In_ SIZE_T InputLength,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _Out_writes_bytes_to_(OutputLength, *BytesWritten) UCHAR *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten)
{
    TERMSRV_CLIPRDR_PDU Pdu;
    TERMSRV_CLIPRDR_RESULT Result;
    ULONG FormatId;
    SIZE_T RequiredLength;

    if (BytesWritten == NULL)
        return TermSrvCliprdrInvalidHeader;

    *BytesWritten = 0;

    if (Input == NULL || Output == NULL || !IsValidBackend(Backend))
        return TermSrvCliprdrInvalidHeader;

    Result = TermSrvCliprdrParsePdu(Input, InputLength, &Pdu);
    if (Result != TermSrvCliprdrSuccess)
        return Result;

    switch (Pdu.MsgType)
    {
        case TERMSRV_CLIPRDR_CB_FORMAT_LIST:
            return TermSrvCliprdrWriteFormatListResponse(
                Output,
                OutputLength,
                TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                BytesWritten);

        case TERMSRV_CLIPRDR_CB_FORMAT_DATA_RESPONSE:
            if ((Pdu.MsgFlags & TERMSRV_CLIPRDR_CB_RESPONSE_OK) == 0)
                return TermSrvCliprdrFormatNotAvailable;

            /*
             * The listener serializes pull requests, so the backend's
             * LastSetFormatId tells us which advertised client format this
             * payload completes.
             */
            FormatId = Backend->PendingFormatId;
            if (!IsSupportedPhaseOneFormat(FormatId) && FormatId < 0xc000)
                FormatId = TERMSRV_CLIPRDR_CF_UNICODETEXT;

            Result = TermSrvCliprdrBackendSetData(Backend,
                                                  FormatId,
                                                  Pdu.Payload,
                                                  Pdu.DataLength);
            if (Result == TermSrvCliprdrSuccess)
                return TermSrvCliprdrSuccess;
            return Result;

        case TERMSRV_CLIPRDR_CB_FORMAT_DATA_REQUEST:
            if (Pdu.DataLength != sizeof(ULONG))
                return TermSrvCliprdrInvalidLength;

            if (OutputLength < CLIPRDR_HEADER_LENGTH)
                return TermSrvCliprdrBufferTooSmall;

            FormatId = ReadLe32(Pdu.Payload);
            Result = TermSrvCliprdrBackendGetData(Backend,
                                                  FormatId,
                                                  &Output[CLIPRDR_HEADER_LENGTH],
                                                  OutputLength - CLIPRDR_HEADER_LENGTH,
                                                  &RequiredLength);
            if (Result == TermSrvCliprdrFormatNotAvailable)
            {
                Result = TermSrvCliprdrWriteFormatDataResponse(
                    Output,
                    OutputLength,
                    TERMSRV_CLIPRDR_CB_RESPONSE_FAIL,
                    NULL,
                    0,
                    BytesWritten);
                if (Result == TermSrvCliprdrSuccess)
                    return TermSrvCliprdrFormatNotAvailable;

                return Result;
            }

            if (Result != TermSrvCliprdrSuccess)
                return Result;

            WriteLe16(&Output[0], TERMSRV_CLIPRDR_CB_FORMAT_DATA_RESPONSE);
            WriteLe16(&Output[2], TERMSRV_CLIPRDR_CB_RESPONSE_OK);
            WriteLe32(&Output[4], (ULONG)RequiredLength);
            *BytesWritten = CLIPRDR_HEADER_LENGTH + RequiredLength;
            return TermSrvCliprdrSuccess;

        case TERMSRV_CLIPRDR_CB_FILECONTENTS_REQUEST:
        case TERMSRV_CLIPRDR_CB_FILECONTENTS_RESPONSE:
        case TERMSRV_CLIPRDR_CB_CLIP_CAPS:
        case TERMSRV_CLIPRDR_CB_LOCK_CLIPDATA:
        case TERMSRV_CLIPRDR_CB_UNLOCK_CLIPDATA:
        case TERMSRV_CLIPRDR_CB_FORMAT_LIST_RESPONSE:
            return TermSrvCliprdrSuccess;

        default:
            return TermSrvCliprdrUnsupportedPdu;
    }
}

static TERMSRV_CLIPRDR_RESULT
MapRdpBcgrResult(
    _In_ TERMSRV_RDPBCGR_RESULT Result)
{
    switch (Result)
    {
        case TermSrvRdpBcgrSuccess:
            return TermSrvCliprdrSuccess;

        case TermSrvRdpBcgrNeedMoreData:
            return TermSrvCliprdrNeedMoreData;

        case TermSrvRdpBcgrBufferTooSmall:
            return TermSrvCliprdrBufferTooSmall;

        case TermSrvRdpBcgrInvalidHeader:
            return TermSrvCliprdrInvalidHeader;

        case TermSrvRdpBcgrInvalidLength:
            return TermSrvCliprdrInvalidLength;

        case TermSrvRdpBcgrUnsupportedPdu:
        default:
            return TermSrvCliprdrUnsupportedPdu;
    }
}

TERMSRV_CLIPRDR_RESULT
TermSrvCliprdrRouteMcsSendData(
    _In_reads_bytes_(InputLength) const UCHAR *Input,
    _In_ SIZE_T InputLength,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Inout_ TERMSRV_CLIPRDR_BACKEND *Backend,
    _Out_writes_bytes_to_(OutputLength, *BytesWritten) UCHAR *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten)
{
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD SendData;
    TERMSRV_RDPBCGR_RESULT ParseResult;

    if (BytesWritten == NULL)
        return TermSrvCliprdrInvalidHeader;

    *BytesWritten = 0;

    if (Input == NULL || Output == NULL || Channel == NULL)
        return TermSrvCliprdrInvalidHeader;

    ParseResult = TermSrvRdpBcgrParseMcsSendDataPayload(Input,
                                                        InputLength,
                                                        &SendData);
    if (ParseResult != TermSrvRdpBcgrSuccess)
        return MapRdpBcgrResult(ParseResult);

    if (!TermSrvCliprdrIsChannelId(Channel, SendData.ChannelId))
        return TermSrvCliprdrUnsupportedPdu;

    TryUnwrapVirtualChannelPayload(SendData.Payload,
                                   SendData.PayloadLength,
                                   &SendData.Payload,
                                   &SendData.PayloadLength);

    return TermSrvCliprdrHandlePdu(SendData.Payload,
                                   SendData.PayloadLength,
                                   Backend,
                                   Output,
                                   OutputLength,
                                   BytesWritten);
}
