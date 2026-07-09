/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Disabled-by-default TCP listener scaffold
 */

#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <debug.h>

#include "cliprdr.h"
#include "listener.h"
#include "rdpbcgr.h"
#include "rdpcrypt.h"
#include "termsrv.h"

#define TERMSRV_LISTEN_ENV_NAME L"REACTOS_TERMSRV_LISTEN"
#define TERMSRV_NOAUTH_ENV_NAME L"REACTOS_TERMSRV_NOAUTH"
#define TERMSRV_LISTEN_PORT_ENV_NAME L"REACTOS_TERMSRV_PORT"
#define TERMSRV_BACKEND_ENV_NAME L"REACTOS_TERMSRV_BACKEND"
#define TERMSRV_SYNTHETIC_FRAME_ENV_NAME L"REACTOS_TERMSRV_SYNTHETIC_FRAME"
#define TERMSRV_LISTEN_PORT 3389
#define TERMSRV_LISTEN_BACKLOG 4
#define TERMSRV_SELECT_TIMEOUT_MS 2000
#define TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID 1001
#define TERMSRV_MCS_WIRE_USER_CHANNEL_ID 2002
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
#define TERMSRV_TEST_BITMAP_WIDTH 64
#define TERMSRV_TEST_BITMAP_HEIGHT 64
#define TERMSRV_TEST_BITMAP_BPP 16
#define TERMSRV_TEST_BITMAP_BYTES \
    (TERMSRV_TEST_BITMAP_WIDTH * TERMSRV_TEST_BITMAP_HEIGHT * 2)
#define TERMSRV_CAPTURE_BITMAP_BPP 32
#define TERMSRV_CAPTURE_BITMAP_BYTES_PER_PIXEL 4
#define TERMSRV_NTUSER_RDP_FRAME_FORMAT_BGRA32 1
#define TERMSRV_POINTER_MARKER_SIZE 16
#define TERMSRV_POINTER_MARKER_BYTES \
    (TERMSRV_POINTER_MARKER_SIZE * TERMSRV_POINTER_MARKER_SIZE * 2)
#define TERMSRV_SCAFFOLD_STATIC_CHANNEL_LIST_HEADER_LENGTH 1
#define TERMSRV_SCAFFOLD_STATIC_CHANNEL_DEF_LENGTH \
    (TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH + 4)
#define TERMSRV_CHANNEL_PDU_HEADER_LENGTH 8
#define TERMSRV_CHANNEL_FLAG_FIRST 0x00000001
#define TERMSRV_CHANNEL_FLAG_LAST 0x00000002
#define TERMSRV_CHANNEL_FLAG_SHOW_PROTOCOL 0x00000010
#define TERMSRV_CLIPRDR_HEADER_LENGTH 8
#define TERMSRV_ACTIVE_BUFFER_LENGTH 16384
#define TERMSRV_CLIPRDR_FILE_CHUNK 4096
#define TERMSRV_CLIPRDR_TEMP_ROOT L"TermSrvClip"
#define TERMSRV_CLIPRDR_MAX_WIRE_FILE_DESCRIPTORS 20

static VOID
TermSrvLogFailure(
    _In_z_ const CHAR *Message);

/* Points at the active connection's crypto state while a client is being
 * serviced (connections are handled one at a time). NULL when idle or before
 * the key exchange completes. Used by the server PDU writers to prepend the
 * basic security header that RDP standard security requires on server output. */
static TERMSRV_RDP_CRYPT *g_TermSrvActiveCrypt = NULL;

/* TRUE once the client random has been exchanged and server output must carry
 * a basic security header (empty, since ENCRYPTION_LEVEL_LOW leaves the
 * server-to-client direction unencrypted). */
static BOOL
TermSrvServerSecurityHeaderActive(VOID)
{
    return g_TermSrvActiveCrypt != NULL && g_TermSrvActiveCrypt->Enabled;
}

typedef enum _TERMSRV_PACKET_PLACEHOLDER
{
    TermSrvPacketUnknown,
    TermSrvPacketTpkt,
    TermSrvPacketFastPath
} TERMSRV_PACKET_PLACEHOLDER;

typedef struct _TERMSRV_CLIENT_CONTEXT
{
    TERMSRV_RDP_PEER Peer;
    TERMSRV_RDP_CRYPT Crypt;
    TERMSRV_CLIPRDR_CHANNEL CliprdrChannel;
    TERMSRV_CLIPRDR_WIN32_BACKEND CliprdrWin32;
    TERMSRV_CLIPRDR_BACKEND CliprdrBackend;
    BOOL CliprdrMonitorReadySent;
    BOOL CliprdrSuppressNextServerAdvertise;
    ULONG CliprdrLastServerHash;
    SIZE_T CliprdrLastServerLength;
    UINT CliprdrFormatFileGroupDescriptorW;
    UINT CliprdrFormatFileContents;
    UINT CliprdrFormatPreferredDropEffect;
    ULONG CliprdrPendingRemoteFormats[TERMSRV_CLIPRDR_MAX_FORMATS];
    ULONG CliprdrPendingLocalFormats[TERMSRV_CLIPRDR_MAX_FORMATS];
    ULONG CliprdrPendingFormatCount;
    ULONG CliprdrPendingFormatIndex;
    TERMSRV_CLIPRDR_FILE_LIST CliprdrIncomingFiles;
    WCHAR CliprdrIncomingPaths[TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS][MAX_PATH];
    HANDLE CliprdrIncomingHandle;
    ULONG CliprdrIncomingIndex;
    ULONGLONG CliprdrIncomingOffset;
    ULONG CliprdrIncomingStreamId;
    BOOL CliprdrIncomingActive;
    TERMSRV_CLIPRDR_FILE_LIST CliprdrOutgoingFiles;
    WCHAR CliprdrOutgoingPaths[TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS][MAX_PATH];
    ULONG CliprdrOutgoingCount;
    HMODULE RdpCaptureModule;
    HANDLE RdpCaptureSession;
    BOOL RdpCaptureLoadAttempted;
    BOOL RdpCaptureAvailable;
    BOOL RdpSyntheticFallbackLogged;
    HANDLE (WINAPI *NtUserRdpOpenSession)(_In_ ULONG SessionId,
                                          _In_opt_ PVOID WinStationName,
                                          _In_opt_ PVOID DesktopName);
    BOOL (WINAPI *NtUserRdpCaptureFrame)(_In_ HANDLE Session,
                                         _Out_ PVOID Frame,
                                         _Out_writes_bytes_to_opt_(PixelBufferSize, *BytesReturned) PVOID PixelBuffer,
                                         _In_ ULONG PixelBufferSize,
                                         _Out_opt_ PULONG BytesReturned);
    BOOL (WINAPI *NtUserRdpCloseSession)(_In_ HANDLE Session);
} TERMSRV_CLIENT_CONTEXT;

typedef struct _TERMSRV_UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} TERMSRV_UNICODE_STRING;

typedef struct _TERMSRV_NTUSER_RDP_FRAME
{
    ULONG Size;
    ULONG SessionId;
    ULONG Width;
    ULONG Height;
    ULONG BitsPerPixel;
    ULONG Pitch;
    ULONG Format;
    ULONG FrameId;
    ULONG RequiredBufferSize;
    ULONG Flags;
} TERMSRV_NTUSER_RDP_FRAME;

static BOOL
TermSrvListenerEnabled(VOID)
{
    WCHAR Value[2];
    DWORD Length;

    Length = GetEnvironmentVariableW(TERMSRV_LISTEN_ENV_NAME, Value, ARRAYSIZE(Value));
    if (Length == 1)
        return Value[0] != L'0';

    return TRUE;
}

static BOOL
TermSrvNoAuthEnabled(VOID)
{
    WCHAR Value[2];
    DWORD Length;

    Length = GetEnvironmentVariableW(TERMSRV_NOAUTH_ENV_NAME, Value, ARRAYSIZE(Value));
    if (Length == 1)
        return Value[0] != L'0';

    return TRUE;
}

static BOOL
TermSrvSyntheticFrameEnabled(VOID)
{
    WCHAR Value[2];

    return (GetEnvironmentVariableW(TERMSRV_SYNTHETIC_FRAME_ENV_NAME, Value, ARRAYSIZE(Value)) == 1 &&
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

static VOID
TermSrvSelectListenerBackend(
    _Inout_ TERMSRV_SESSION_MANAGER *SessionManager)
{
    WCHAR Value[32];
    DWORD Length;

    Length = GetEnvironmentVariableW(TERMSRV_BACKEND_ENV_NAME, Value, ARRAYSIZE(Value));
    if (Length == 0 || Length >= ARRAYSIZE(Value))
    {
        TermSrvSessionManagerSelectBackendByName(SessionManager, NULL);
        return;
    }

    TermSrvSessionManagerSelectBackendByName(SessionManager, Value);
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
TermSrvTryGetActivePacketLength(
    _In_reads_bytes_(Available) const UCHAR *Buffer,
    _In_ INT Available,
    _Out_ INT *PacketLength)
{
    INT Length;

    *PacketLength = 0;
    if (Available <= 0)
        return FALSE;

    if (Available >= 4 && Buffer[0] == 3)
    {
        Length = ((INT)Buffer[2] << 8) | Buffer[3];
        if (Length < 7)
        {
            TermSrvLogFailure("active TPKT packet has invalid length");
            return FALSE;
        }

        *PacketLength = Length;
        return TRUE;
    }

    if (Available >= 2 && (Buffer[0] & 0x03) == 0)
    {
        if (Buffer[1] & 0x80)
        {
            if (Available < 3)
                return TRUE;

            Length = ((INT)(Buffer[1] & 0x7f) << 8) | Buffer[2];
        }
        else
        {
            Length = Buffer[1];
        }

        if (Length < 2)
        {
            TermSrvLogFailure("active fast-path packet has invalid length");
            return FALSE;
        }

        *PacketLength = Length;
        return TRUE;
    }

    return TRUE;
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
TermSrvLogWin32Failure(
    _In_z_ const CHAR *Operation,
    _In_ DWORD Error)
{
    CHAR Message[160];

    _snprintf(Message,
              sizeof(Message),
              "%s failed: Win32 error %lu",
              Operation,
              Error);
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
TermSrvInitUnicodeString(
    _Out_ TERMSRV_UNICODE_STRING *String,
    _In_z_ PCWSTR Buffer)
{
    SIZE_T Length;

    Length = lstrlenW(Buffer) * sizeof(WCHAR);
    String->Length = (USHORT)Length;
    String->MaximumLength = (USHORT)(Length + sizeof(WCHAR));
    String->Buffer = (PWSTR)Buffer;
}

static BOOL
TermSrvIsConsoleBackend(
    _In_ const TERMSRV_CLIENT_CONTEXT *Context)
{
    const TERMSRV_SESSION_BACKEND *Backend;

    if (Context == NULL || Context->Peer.SessionManager == NULL)
        return FALSE;

    Backend = TermSrvSessionManagerGetBackend(Context->Peer.SessionManager);
    return Backend != NULL &&
           (Backend == TermSrvSessionManagerGetConsoleBackend() ||
            Backend == TermSrvSessionManagerGetSessmanBackend());
}

static BOOL
TermSrvLoadRdpCaptureApi(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context)
{
    FARPROC OpenSession;
    FARPROC CaptureFrame;
    FARPROC CloseSession;

    if (Context->RdpCaptureLoadAttempted)
        return Context->RdpCaptureAvailable;

    Context->RdpCaptureLoadAttempted = TRUE;
    Context->RdpCaptureModule = LoadLibraryW(L"win32u.dll");
    if (Context->RdpCaptureModule == NULL)
    {
        TermSrvLogFailure("RDP capture fallback: win32u.dll could not be loaded");
        return FALSE;
    }

    OpenSession = GetProcAddress(Context->RdpCaptureModule, "NtUserRdpOpenSession");
    CaptureFrame = GetProcAddress(Context->RdpCaptureModule, "NtUserRdpCaptureFrame");
    CloseSession = GetProcAddress(Context->RdpCaptureModule, "NtUserRdpCloseSession");
    if (OpenSession == NULL || CaptureFrame == NULL || CloseSession == NULL)
    {
        TermSrvLogFailure("RDP capture fallback: NtUserRdp* exports are not available");
        FreeLibrary(Context->RdpCaptureModule);
        Context->RdpCaptureModule = NULL;
        return FALSE;
    }

    Context->NtUserRdpOpenSession = (HANDLE (WINAPI *)(ULONG, PVOID, PVOID))OpenSession;
    Context->NtUserRdpCaptureFrame = (BOOL (WINAPI *)(HANDLE, PVOID, PVOID, ULONG, PULONG))CaptureFrame;
    Context->NtUserRdpCloseSession = (BOOL (WINAPI *)(HANDLE))CloseSession;
    Context->RdpCaptureAvailable = TRUE;
    return TRUE;
}

static VOID
TermSrvCloseRdpCaptureApi(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context)
{
    if (Context == NULL)
        return;

    if (Context->RdpCaptureSession != NULL && Context->NtUserRdpCloseSession != NULL)
        Context->NtUserRdpCloseSession(Context->RdpCaptureSession);

    if (Context->RdpCaptureModule != NULL)
        FreeLibrary(Context->RdpCaptureModule);

    Context->RdpCaptureSession = NULL;
    Context->RdpCaptureModule = NULL;
    Context->RdpCaptureAvailable = FALSE;
}

static BOOL
TermSrvOpenRdpCaptureSession(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context)
{
    TERMSRV_UNICODE_STRING WinStationName;
    TERMSRV_UNICODE_STRING DesktopName;

    if (Context->RdpCaptureSession != NULL)
        return TRUE;

    if (!TermSrvLoadRdpCaptureApi(Context))
        return FALSE;

    TermSrvInitUnicodeString(&WinStationName, L"WinSta0");
    TermSrvInitUnicodeString(&DesktopName, L"Default");
    Context->RdpCaptureSession = Context->NtUserRdpOpenSession(0,
                                                               &WinStationName,
                                                               &DesktopName);
    if (Context->RdpCaptureSession == NULL)
    {
        TermSrvLogFailure("RDP capture fallback: NtUserRdpOpenSession failed");
        return FALSE;
    }

    return TRUE;
}

static VOID
TermSrvLogSyntheticFallbackOnce(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_z_ const CHAR *Reason)
{
    CHAR Message[160];

    if (Context->RdpSyntheticFallbackLogged)
        return;

    _snprintf(Message,
              sizeof(Message),
              "using synthetic RDP bitmap frame fallback: %s",
              Reason);
    Message[sizeof(Message) - 1] = '\0';
    TermSrvLogFailure(Message);
    Context->RdpSyntheticFallbackLogged = TRUE;
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
    SIZE_T TotalSent = 0;

    while (TotalSent < Length)
    {
        INT Sent;
        INT Remaining;

        Remaining = (INT)min(Length - TotalSent, (SIZE_T)INT_MAX);
        Sent = send(Client, (const char *)&Buffer[TotalSent], Remaining, 0);
        if (Sent == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
            {
                Sleep(1);
                continue;
            }

            TermSrvLogSocketFailure("send");
            return FALSE;
        }

        if (Sent == 0)
        {
            TermSrvLogFailure("send returned zero bytes");
            return FALSE;
        }

        TotalSent += (SIZE_T)Sent;
    }

    return TRUE;
}

static BOOL
TermSrvReceiveTpkt(
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _Out_ INT *Received,
    _Inout_opt_ TERMSRV_RDP_CRYPT *Crypt)
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

    /* Decrypt the client-to-server stream once RDP standard security is active.
     * A no-op before the key exchange completes or for unencrypted PDUs. */
    if (Crypt != NULL)
    {
        SIZE_T DecryptedLength = (SIZE_T)*Received;
        TermSrvCryptUnwrapInbound(Crypt, Buffer, &DecryptedLength);
        *Received = (INT)DecryptedLength;
    }

    return TRUE;
}

static BOOL
TermSrvInitializeCliprdrScaffold(
    _Out_ TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Out_ TERMSRV_CLIPRDR_WIN32_BACKEND *Win32,
    _Out_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    TERMSRV_CLIPRDR_RESULT ClipResult;

    ClipResult = TermSrvCliprdrChannelInit(Channel);
    if (ClipResult != TermSrvCliprdrSuccess)
    {
        TermSrvLogCliprdrFailure("cliprdr channel init", ClipResult);
        return FALSE;
    }

    ClipResult = TermSrvCliprdrWin32BackendInit(Win32, Backend);
    if (ClipResult != TermSrvCliprdrSuccess)
    {
        TermSrvLogCliprdrFailure("cliprdr win32 backend init", ClipResult);
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
TermSrvTryParseGccStaticChannelList(
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _Out_ TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList)
{
    SIZE_T Offset;

    if (Payload == NULL || ChannelList == NULL)
        return FALSE;

    ZeroMemory(ChannelList, sizeof(*ChannelList));

    for (Offset = 0; Offset + 8 <= PayloadLength; Offset++)
    {
        USHORT HeaderType;
        USHORT HeaderLength;
        ULONG Count;
        SIZE_T DefOffset;
        SIZE_T Index;

        HeaderType = (USHORT)(Payload[Offset] | (Payload[Offset + 1] << 8));
        if (HeaderType != 0xc003)
            continue;

        HeaderLength = (USHORT)(Payload[Offset + 2] | (Payload[Offset + 3] << 8));
        if (HeaderLength < 8 || Offset + HeaderLength > PayloadLength)
            continue;

        Count = (ULONG)Payload[Offset + 4] |
                ((ULONG)Payload[Offset + 5] << 8) |
                ((ULONG)Payload[Offset + 6] << 16) |
                ((ULONG)Payload[Offset + 7] << 24);
        if (Count > TERMSRV_RDPBCGR_MAX_STATIC_CHANNELS)
            return FALSE;

        if ((SIZE_T)HeaderLength < 8 + ((SIZE_T)Count * TERMSRV_SCAFFOLD_STATIC_CHANNEL_DEF_LENGTH))
            return FALSE;

        ChannelList->Count = Count;
        DefOffset = Offset + 8;
        for (Index = 0; Index < Count; Index++)
        {
            CopyMemory(ChannelList->Channels[Index].Name,
                       &Payload[DefOffset],
                       TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH);
            DefOffset += TERMSRV_RDPBCGR_STATIC_CHANNEL_NAME_LENGTH;
            ChannelList->Channels[Index].Options =
                (ULONG)Payload[DefOffset] |
                ((ULONG)Payload[DefOffset + 1] << 8) |
                ((ULONG)Payload[DefOffset + 2] << 16) |
                ((ULONG)Payload[DefOffset + 3] << 24);
            ChannelList->Channels[Index].Index = Index;
            DefOffset += 4;
        }

        DPRINT1("termsrv: parsed %lu GCC static virtual channels\n", Count);
        return TRUE;
    }

    return FALSE;
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

/* BER definite length: 1 byte (<=0x7f), 2 bytes (0x81 xx) or 3 bytes (0x82 xx xx). */
static SIZE_T
TermSrvBerLengthSize(SIZE_T Length)
{
    if (Length <= 0x7f)
        return 1;
    if (Length <= 0xff)
        return 2;
    return 3;
}

static SIZE_T
TermSrvWriteBerLength(UCHAR *Buffer, SIZE_T Length)
{
    if (Length <= 0x7f)
    {
        Buffer[0] = (UCHAR)Length;
        return 1;
    }
    if (Length <= 0xff)
    {
        Buffer[0] = 0x81;
        Buffer[1] = (UCHAR)Length;
        return 2;
    }
    Buffer[0] = 0x82;
    Buffer[1] = (UCHAR)(Length >> 8);
    Buffer[2] = (UCHAR)Length;
    return 3;
}

/* PER length determinant: 1 byte (<0x80) or 2 bytes (0x8000 | value). */
static SIZE_T
TermSrvPerLengthSize(SIZE_T Length)
{
    return (Length < 0x80) ? 1 : 2;
}

static SIZE_T
TermSrvWritePerLength(UCHAR *Buffer, SIZE_T Length)
{
    if (Length < 0x80)
    {
        Buffer[0] = (UCHAR)Length;
        return 1;
    }
    Buffer[0] = (UCHAR)(0x80 | (Length >> 8));
    Buffer[1] = (UCHAR)Length;
    return 2;
}

static SIZE_T
TermSrvBuildMcsConnectResponsePayload(
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_opt_ const TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST *ChannelList,
    _In_ const TERMSRV_RDP_CRYPT *Crypt)
{
    /*
     * BER MCS Connect-Response wrapping a T.124 ConferenceCreateResponse. All
     * of the enclosing length fields (GCC user-data length, the userData OCTET
     * STRING length and the outer MCS length) are recomputed here because the
     * SC_SECURITY block is now variable-length: it carries the server random
     * and Server Proprietary Certificate, pushing the totals past the 0x7f
     * single-byte BER/PER boundary, so multi-byte length encodings are used.
     */
    static const UCHAR DomainParams[] =
    {
        0x0a, 0x01, 0x00,               /* result = rt-successful */
        0x02, 0x01, 0x00,               /* calledConnectId = 0 */
        0x30, 0x20,                     /* domainParameters SEQUENCE, length 32 */
        0x02, 0x02, 0x00, 0x22,
        0x02, 0x02, 0x00, 0x03,
        0x02, 0x02, 0x00, 0x00,
        0x02, 0x02, 0x00, 0x01,
        0x02, 0x02, 0x00, 0x00,
        0x02, 0x02, 0x00, 0x01,
        0x02, 0x02, 0xff, 0xff,
        0x02, 0x02, 0x00, 0x02
    };
    static const UCHAR GccHeader[] =
    {
        0x00, 0x05, 0x00, 0x14, 0x7c, 0x00, 0x01,
        0x2a, 0x14, 0x76, 0x0a, 0x01, 0x01, 0x00,
        0x01, 0xc0, 0x00, 'M', 'c', 'D', 'n'
    };
    static const UCHAR ScCore[] =
    {
        0x01, 0x0c, 0x10, 0x00,
        0x04, 0x00, 0x08, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR ScMsgChannel[] =
    {
        0x04, 0x0c, 0x06, 0x00, 0xf0, 0x03
    };
    UCHAR ScSecurity[256];
    SIZE_T ScSecurityLen;

    USHORT ChannelCount = 4;
    SIZE_T i;
    SIZE_T Pos;
    SIZE_T PadBytes;
    SIZE_T ScNetLen;
    SIZE_T ScBlocksLen;
    SIZE_T OctetStringLen;
    SIZE_T McsLen;
    SIZE_T Total;

    ScSecurityLen = TermSrvCryptBuildServerSecurityBlock(ScSecurity,
                                                         sizeof(ScSecurity),
                                                         Crypt);
    if (ScSecurityLen == 0)
        return 0;

    if (ChannelList != NULL && ChannelList->Count < ChannelCount)
        ChannelCount = (USHORT)ChannelList->Count;

    PadBytes = (ChannelCount & 1) ? 2 : 0;
    ScNetLen = 8 + (SIZE_T)ChannelCount * 2 + PadBytes;           /* includes 4-byte header */
    ScBlocksLen = sizeof(ScCore) + ScNetLen + sizeof(ScMsgChannel) + ScSecurityLen;
    OctetStringLen = sizeof(GccHeader) + TermSrvPerLengthSize(ScBlocksLen) + ScBlocksLen;
    McsLen = sizeof(DomainParams) + 1 + TermSrvBerLengthSize(OctetStringLen) + OctetStringLen;
    Total = 2 + TermSrvBerLengthSize(McsLen) + McsLen;           /* + 0x7f 0x66 <len> */

    if (Buffer == NULL || BufferLength < Total)
        return 0;

    Pos = 0;
    Buffer[Pos++] = 0x7f;
    Buffer[Pos++] = 0x66;
    Pos += TermSrvWriteBerLength(&Buffer[Pos], McsLen);
    CopyMemory(&Buffer[Pos], DomainParams, sizeof(DomainParams));
    Pos += sizeof(DomainParams);
    Buffer[Pos++] = 0x04;                                        /* userData OCTET STRING */
    Pos += TermSrvWriteBerLength(&Buffer[Pos], OctetStringLen);
    CopyMemory(&Buffer[Pos], GccHeader, sizeof(GccHeader));
    Pos += sizeof(GccHeader);
    Pos += TermSrvWritePerLength(&Buffer[Pos], ScBlocksLen);     /* GCC user-data length */
    CopyMemory(&Buffer[Pos], ScCore, sizeof(ScCore));
    Pos += sizeof(ScCore);

    /* SC_NET with exactly ChannelCount advertised static channels. */
    Buffer[Pos++] = 0x03;
    Buffer[Pos++] = 0x0c;
    Buffer[Pos++] = (UCHAR)ScNetLen;
    Buffer[Pos++] = (UCHAR)(ScNetLen >> 8);
    Buffer[Pos++] = 0xeb;                                        /* MCSChannelId (I/O channel) */
    Buffer[Pos++] = 0x03;
    Buffer[Pos++] = (UCHAR)ChannelCount;
    Buffer[Pos++] = (UCHAR)(ChannelCount >> 8);
    for (i = 0; i < ChannelCount; i++)
    {
        USHORT ChannelId = (USHORT)(0x03ec + i);
        Buffer[Pos++] = (UCHAR)ChannelId;
        Buffer[Pos++] = (UCHAR)(ChannelId >> 8);
    }
    if (PadBytes != 0)
    {
        Buffer[Pos++] = 0x00;
        Buffer[Pos++] = 0x00;
    }

    CopyMemory(&Buffer[Pos], ScMsgChannel, sizeof(ScMsgChannel));
    Pos += sizeof(ScMsgChannel);
    CopyMemory(&Buffer[Pos], ScSecurity, ScSecurityLen);
    Pos += ScSecurityLen;

    return Pos;
}

static ULONG
TermSrvReadLe32(
    _In_reads_bytes_(4) const UCHAR *Buffer)
{
    return (ULONG)Buffer[0] |
           ((ULONG)Buffer[1] << 8) |
           ((ULONG)Buffer[2] << 16) |
           ((ULONG)Buffer[3] << 24);
}

static BOOL
TermSrvWriteMcsPayloadLength(
    _Out_writes_bytes_(2) UCHAR *Buffer,
    _In_ SIZE_T PayloadLength,
    _Out_ SIZE_T *LengthBytes)
{
    if (Buffer == NULL || LengthBytes == NULL || PayloadLength > 0x7fff)
        return FALSE;

    if (PayloadLength > 0x7f)
    {
        Buffer[0] = (UCHAR)(0x80 | (PayloadLength >> 8));
        Buffer[1] = (UCHAR)PayloadLength;
        *LengthBytes = 2;
    }
    else
    {
        Buffer[0] = (UCHAR)PayloadLength;
        *LengthBytes = 1;
    }

    return TRUE;
}

static BOOL
TermSrvWriteServerChannelPayload(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT ChannelId,
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T PacketLength;
    SIZE_T PayloadOffset;
    SIZE_T LengthBytes;
    SIZE_T EffectiveLength;
    BOOL AddSecurityHeader;

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    if (Buffer == NULL || Payload == NULL || PayloadLength > 0x7fff)
        return FALSE;

    /* Virtual-channel PDUs never carry their own security header, so prepend the
     * empty basic security header once RDP standard security is negotiated. */
    AddSecurityHeader = TermSrvServerSecurityHeaderActive();
    EffectiveLength = PayloadLength + (AddSecurityHeader ? 4 : 0);
    if (EffectiveLength > 0x7fff)
        return FALSE;

    PacketLength = 7 + ((EffectiveLength > 0x7f) ? 8 : 7) + EffectiveLength;
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
    Buffer[10] = (UCHAR)(ChannelId >> 8);
    Buffer[11] = (UCHAR)ChannelId;
    Buffer[12] = 0x70;
    if (!TermSrvWriteMcsPayloadLength(&Buffer[13], EffectiveLength, &LengthBytes))
        return FALSE;

    PayloadOffset = 13 + LengthBytes;
    if (AddSecurityHeader)
    {
        Buffer[PayloadOffset] = 0x00;
        Buffer[PayloadOffset + 1] = 0x00;
        Buffer[PayloadOffset + 2] = 0x00;
        Buffer[PayloadOffset + 3] = 0x00;
        PayloadOffset += 4;
    }
    CopyMemory(&Buffer[PayloadOffset], Payload, PayloadLength);
    *BytesWritten = PacketLength;
    return TRUE;
}

static BOOL
TermSrvTryUnwrapChannelPayload(
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

    if (Payload == NULL || PayloadLength < TERMSRV_CHANNEL_PDU_HEADER_LENGTH)
        return FALSE;

    DeclaredLength = TermSrvReadLe32(&Payload[0]);
    Flags = TermSrvReadLe32(&Payload[4]);
    if ((Flags & (TERMSRV_CHANNEL_FLAG_FIRST | TERMSRV_CHANNEL_FLAG_LAST)) !=
        (TERMSRV_CHANNEL_FLAG_FIRST | TERMSRV_CHANNEL_FLAG_LAST))
    {
        return FALSE;
    }

    if (DeclaredLength > PayloadLength - TERMSRV_CHANNEL_PDU_HEADER_LENGTH)
        return FALSE;

    *CliprdrPayload = Payload + TERMSRV_CHANNEL_PDU_HEADER_LENGTH;
    *CliprdrLength = DeclaredLength;
    return TRUE;
}

static BOOL
TermSrvSendCliprdrPdu(
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_reads_bytes_(PduLength) const UCHAR *Pdu,
    _In_ SIZE_T PduLength)
{
    UCHAR WrappedReply[12288];
    UCHAR VirtualPayload[12280];
    SIZE_T WrappedBytesWritten;

    if (Channel == NULL ||
        Channel->ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID ||
        Pdu == NULL)
    {
        return FALSE;
    }

    if (PduLength > sizeof(VirtualPayload) - TERMSRV_CHANNEL_PDU_HEADER_LENGTH)
        return FALSE;

    VirtualPayload[0] = (UCHAR)PduLength;
    VirtualPayload[1] = (UCHAR)(PduLength >> 8);
    VirtualPayload[2] = (UCHAR)(PduLength >> 16);
    VirtualPayload[3] = (UCHAR)(PduLength >> 24);
    VirtualPayload[4] = (UCHAR)(TERMSRV_CHANNEL_FLAG_FIRST |
                                TERMSRV_CHANNEL_FLAG_LAST |
                                TERMSRV_CHANNEL_FLAG_SHOW_PROTOCOL);
    VirtualPayload[5] = 0;
    VirtualPayload[6] = 0;
    VirtualPayload[7] = 0;
    CopyMemory(&VirtualPayload[TERMSRV_CHANNEL_PDU_HEADER_LENGTH], Pdu, PduLength);

    if (!TermSrvWriteServerChannelPayload(WrappedReply,
                                          sizeof(WrappedReply),
                                          Channel->ChannelId,
                                          VirtualPayload,
                                          TERMSRV_CHANNEL_PDU_HEADER_LENGTH + PduLength,
                                          &WrappedBytesWritten))
    {
        TermSrvLogFailure("cliprdr MCS Send Data Indication write failed");
        return FALSE;
    }

    return TermSrvSendPacket(Client, WrappedReply, WrappedBytesWritten);
}

static ULONGLONG
TermSrvFileSizeFromDescriptor(
    _In_ const TERMSRV_CLIPRDR_FILE_DESCRIPTOR *Descriptor)
{
    return ((ULONGLONG)Descriptor->FileSizeHigh << 32) |
           Descriptor->FileSizeLow;
}

static VOID
TermSrvWriteLe64(
    _Out_writes_bytes_(8) UCHAR *Buffer,
    _In_ ULONGLONG Value)
{
    Buffer[0] = (UCHAR)Value;
    Buffer[1] = (UCHAR)(Value >> 8);
    Buffer[2] = (UCHAR)(Value >> 16);
    Buffer[3] = (UCHAR)(Value >> 24);
    Buffer[4] = (UCHAR)(Value >> 32);
    Buffer[5] = (UCHAR)(Value >> 40);
    Buffer[6] = (UCHAR)(Value >> 48);
    Buffer[7] = (UCHAR)(Value >> 56);
}

static BOOL
TermSrvIsSafeRelativeFileName(
    _In_z_ const WCHAR *Name)
{
    const WCHAR *Cursor;

    if (Name == NULL || Name[0] == 0)
        return FALSE;

    if (Name[0] == L'\\' || Name[0] == L'/' ||
        (Name[0] != 0 && Name[1] == L':'))
    {
        return FALSE;
    }

    Cursor = Name;
    while (*Cursor != 0)
    {
        if ((Cursor[0] == L'.' && Cursor[1] == L'.' &&
             (Cursor[2] == 0 || Cursor[2] == L'\\' || Cursor[2] == L'/')) ||
            ((Cursor == Name || Cursor[-1] == L'\\' || Cursor[-1] == L'/') &&
             Cursor[0] == L'.' && Cursor[1] == L'.' &&
             (Cursor[2] == 0 || Cursor[2] == L'\\' || Cursor[2] == L'/')))
        {
            return FALSE;
        }

        if (*Cursor == L'/')
            return FALSE;
        Cursor++;
    }

    return TRUE;
}

static BOOL
TermSrvDirectoryExists(
    _In_z_ const WCHAR *Path)
{
    DWORD Attributes;

    Attributes = GetFileAttributesW(Path);
    return Attributes != INVALID_FILE_ATTRIBUTES &&
           (Attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL
TermSrvEnsureDirectoryTree(
    _In_z_ const WCHAR *Path)
{
    WCHAR Temp[MAX_PATH];
    SIZE_T Index;

    if (Path == NULL || lstrlenW(Path) >= MAX_PATH)
        return FALSE;

    lstrcpyW(Temp, Path);
    for (Index = 0; Temp[Index] != 0; Index++)
    {
        if (Temp[Index] == L'\\' && Index > 2)
        {
            DWORD Error;

            Temp[Index] = 0;
            if (!CreateDirectoryW(Temp, NULL))
            {
                Error = GetLastError();
                if (Error != ERROR_ALREADY_EXISTS &&
                    !TermSrvDirectoryExists(Temp))
                {
                    TermSrvLogWin32Failure("cliprdr incoming directory create",
                                           Error);
                    Temp[Index] = L'\\';
                    return FALSE;
                }
            }
            Temp[Index] = L'\\';
        }
    }

    return TRUE;
}

static BOOL
TermSrvCreateIncomingRoot(
    _In_z_ const CHAR *Description,
    _In_z_ const WCHAR *Root)
{
    DWORD Error;

    if (!TermSrvEnsureDirectoryTree(Root))
        return FALSE;

    if (CreateDirectoryW(Root, NULL))
        return TRUE;

    Error = GetLastError();
    if (Error == ERROR_ALREADY_EXISTS || TermSrvDirectoryExists(Root))
        return TRUE;

    TermSrvLogWin32Failure(Description, Error);
    return FALSE;
}

static BOOL
TermSrvBuildIncomingTempRoot(
    _Out_writes_(MAX_PATH) WCHAR *Root)
{
    WCHAR TempPath[MAX_PATH];
    DWORD Length;

    Length = GetTempPathW(ARRAYSIZE(TempPath), TempPath);
    if (Length == 0 || Length >= ARRAYSIZE(TempPath))
    {
        TermSrvLogWin32Failure("cliprdr GetTempPath", GetLastError());
        return FALSE;
    }

    _snwprintf(Root,
               MAX_PATH,
               L"%s%s\\%lu",
               TempPath,
               TERMSRV_CLIPRDR_TEMP_ROOT,
               GetTickCount());
    Root[MAX_PATH - 1] = 0;

    if (!TermSrvCreateIncomingRoot("cliprdr temp root create", Root))
        return FALSE;

    return TRUE;
}

static BOOL
TermSrvBuildIncomingPath(
    _In_z_ const WCHAR *Root,
    _In_z_ const WCHAR *RelativeName,
    _Out_writes_(MAX_PATH) WCHAR *Path)
{
    if (!TermSrvIsSafeRelativeFileName(RelativeName))
        return FALSE;

    if (_snwprintf(Path, MAX_PATH, L"%s\\%s", Root, RelativeName) < 0)
        return FALSE;
    Path[MAX_PATH - 1] = 0;

    return lstrlenW(Path) < MAX_PATH;
}

static BOOL
TermSrvPublishHdrop(
    _In_reads_(Count) WCHAR Paths[][MAX_PATH],
    _In_ ULONG Count,
    _In_ TERMSRV_CLIPRDR_BACKEND *Backend)
{
    SIZE_T Chars;
    ULONG Index;
    SIZE_T Bytes;
    HGLOBAL Global;
    DROPFILES *Drop;
    WCHAR *Target;

    if (Count == 0)
        return FALSE;

    Chars = 1;
    for (Index = 0; Index < Count; Index++)
        Chars += (SIZE_T)lstrlenW(Paths[Index]) + 1;

    Bytes = sizeof(DROPFILES) + Chars * sizeof(WCHAR);
    Global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, Bytes);
    if (Global == NULL)
        return FALSE;

    Drop = (DROPFILES *)GlobalLock(Global);
    if (Drop == NULL)
    {
        GlobalFree(Global);
        return FALSE;
    }

    Drop->pFiles = sizeof(DROPFILES);
    Drop->fWide = TRUE;
    Target = (WCHAR *)((UCHAR *)Drop + sizeof(DROPFILES));
    for (Index = 0; Index < Count; Index++)
    {
        SIZE_T Length = (SIZE_T)lstrlenW(Paths[Index]);

        CopyMemory(Target, Paths[Index], Length * sizeof(WCHAR));
        Target += Length + 1;
    }
    *Target = 0;
    if (TermSrvCliprdrBackendSetData(Backend,
                                     TERMSRV_CLIPRDR_CF_HDROP,
                                     (const UCHAR *)Drop,
                                     Bytes) != TermSrvCliprdrSuccess)
    {
        GlobalUnlock(Global);
        GlobalFree(Global);
        return FALSE;
    }

    GlobalUnlock(Global);
    GlobalFree(Global);
    return TRUE;
}

static BOOL
TermSrvRequestClientFileRange(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST Request;
    UCHAR Pdu[64];
    SIZE_T BytesWritten;
    ULONGLONG Size;
    ULONG Chunk;

    while (Context->CliprdrIncomingIndex < Context->CliprdrIncomingFiles.Count)
    {
        TERMSRV_CLIPRDR_FILE_DESCRIPTOR *Descriptor;
        WCHAR *Path;

        Descriptor = &Context->CliprdrIncomingFiles.Descriptors[Context->CliprdrIncomingIndex];
        Path = Context->CliprdrIncomingPaths[Context->CliprdrIncomingIndex];

        if (Descriptor->Attributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            TermSrvEnsureDirectoryTree(Path);
            CreateDirectoryW(Path, NULL);
            Context->CliprdrIncomingIndex++;
            Context->CliprdrIncomingOffset = 0;
            continue;
        }

        Size = TermSrvFileSizeFromDescriptor(Descriptor);
        if (Size == 0)
        {
            HANDLE EmptyFile;

            if (!TermSrvEnsureDirectoryTree(Path))
                return FALSE;

            EmptyFile = CreateFileW(Path,
                                    GENERIC_WRITE,
                                    0,
                                    NULL,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    NULL);
            if (EmptyFile == INVALID_HANDLE_VALUE)
                return FALSE;
            CloseHandle(EmptyFile);
            Context->CliprdrIncomingIndex++;
            Context->CliprdrIncomingOffset = 0;
            continue;
        }

        if (Context->CliprdrIncomingOffset >= Size)
        {
            if (Context->CliprdrIncomingHandle != NULL &&
                Context->CliprdrIncomingHandle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(Context->CliprdrIncomingHandle);
                Context->CliprdrIncomingHandle = INVALID_HANDLE_VALUE;
            }
            Context->CliprdrIncomingIndex++;
            Context->CliprdrIncomingOffset = 0;
            continue;
        }

        if (Context->CliprdrIncomingHandle == NULL ||
            Context->CliprdrIncomingHandle == INVALID_HANDLE_VALUE)
        {
            if (!TermSrvEnsureDirectoryTree(Path))
                return FALSE;

            Context->CliprdrIncomingHandle = CreateFileW(Path,
                                                         GENERIC_WRITE,
                                                         0,
                                                         NULL,
                                                         CREATE_ALWAYS,
                                                         FILE_ATTRIBUTE_NORMAL,
                                                         NULL);
            if (Context->CliprdrIncomingHandle == INVALID_HANDLE_VALUE)
            {
                TermSrvLogFailure("cliprdr incoming file create failed");
                return FALSE;
            }
        }

        Chunk = (ULONG)min(Size - Context->CliprdrIncomingOffset,
                           (ULONGLONG)TERMSRV_CLIPRDR_FILE_CHUNK);
        ZeroMemory(&Request, sizeof(Request));
        Request.StreamId = ++Context->CliprdrIncomingStreamId;
        Request.ListIndex = Context->CliprdrIncomingIndex;
        Request.Flags = TERMSRV_CLIPRDR_FILECONTENTS_RANGE;
        Request.PositionLow = (ULONG)Context->CliprdrIncomingOffset;
        Request.PositionHigh = (ULONG)(Context->CliprdrIncomingOffset >> 32);
        Request.Requested = Chunk;

        if (TermSrvCliprdrWriteFileContentsRequest(Pdu,
                                                   sizeof(Pdu),
                                                   &Request,
                                                   &BytesWritten) != TermSrvCliprdrSuccess)
        {
            return FALSE;
        }

        return TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten);
    }

    Context->CliprdrIncomingActive = FALSE;
    return TermSrvPublishHdrop(Context->CliprdrIncomingPaths,
                               Context->CliprdrIncomingFiles.Count,
                               &Context->CliprdrBackend);
}

static BOOL
TermSrvStartIncomingFiles(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength)
{
    WCHAR Root[MAX_PATH];
    ULONG Index;

    if (TermSrvCliprdrParseFileList(Data,
                                    DataLength,
                                    &Context->CliprdrIncomingFiles) != TermSrvCliprdrSuccess)
    {
        TermSrvLogFailure("cliprdr FileGroupDescriptorW parse failed");
        return FALSE;
    }

    if (Context->CliprdrIncomingFiles.Count == 0 ||
        !TermSrvBuildIncomingTempRoot(Root))
    {
        if (Context->CliprdrIncomingFiles.Count == 0)
            TermSrvLogFailure("cliprdr FileGroupDescriptorW contains no files");
        else
            TermSrvLogFailure("cliprdr incoming temp root creation failed");
        return FALSE;
    }

    for (Index = 0; Index < Context->CliprdrIncomingFiles.Count; Index++)
    {
        if (!TermSrvBuildIncomingPath(Root,
                                      Context->CliprdrIncomingFiles.Descriptors[Index].FileName,
                                      Context->CliprdrIncomingPaths[Index]))
        {
            TermSrvLogFailure("cliprdr rejected unsafe incoming file path");
            return FALSE;
        }
    }

    if (Context->CliprdrIncomingHandle != NULL &&
        Context->CliprdrIncomingHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Context->CliprdrIncomingHandle);
    }

    Context->CliprdrIncomingHandle = INVALID_HANDLE_VALUE;
    Context->CliprdrIncomingIndex = 0;
    Context->CliprdrIncomingOffset = 0;
    Context->CliprdrIncomingStreamId = 0;
    Context->CliprdrIncomingActive = TRUE;

    return TermSrvRequestClientFileRange(Context, Client, Channel);
}

static BOOL
TermSrvHandleIncomingFileContentsResponse(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ SIZE_T DataLength)
{
    UCHAR PduBuffer[12288];
    TERMSRV_CLIPRDR_FILE_CONTENTS_RESPONSE Response;
    DWORD Written;

    if (!Context->CliprdrIncomingActive)
        return TRUE;

    if (DataLength > sizeof(PduBuffer) - TERMSRV_CLIPRDR_HEADER_LENGTH)
        return FALSE;

    PduBuffer[0] = TERMSRV_CLIPRDR_CB_FILECONTENTS_RESPONSE;
    PduBuffer[1] = 0;
    PduBuffer[2] = TERMSRV_CLIPRDR_CB_RESPONSE_OK;
    PduBuffer[3] = 0;
    PduBuffer[4] = (UCHAR)DataLength;
    PduBuffer[5] = (UCHAR)(DataLength >> 8);
    PduBuffer[6] = (UCHAR)(DataLength >> 16);
    PduBuffer[7] = (UCHAR)(DataLength >> 24);
    CopyMemory(&PduBuffer[TERMSRV_CLIPRDR_HEADER_LENGTH], Data, DataLength);

    if (TermSrvCliprdrParseFileContentsResponse(PduBuffer,
                                                TERMSRV_CLIPRDR_HEADER_LENGTH + DataLength,
                                                &Response) != TermSrvCliprdrSuccess)
    {
        return FALSE;
    }

    if (Response.StreamId != Context->CliprdrIncomingStreamId ||
        Context->CliprdrIncomingHandle == INVALID_HANDLE_VALUE)
    {
        TermSrvLogFailure("cliprdr unexpected file contents stream id");
        return FALSE;
    }

    if (Response.DataLength != 0 &&
        !WriteFile(Context->CliprdrIncomingHandle,
                   Response.Data,
                   (DWORD)Response.DataLength,
                   &Written,
                   NULL))
    {
        TermSrvLogFailure("cliprdr incoming file write failed");
        return FALSE;
    }

    Context->CliprdrIncomingOffset += Response.DataLength;
    return TermSrvRequestClientFileRange(Context, Client, Channel);
}

static BOOL
TermSrvReadHdropClipboard(
    _Out_writes_(TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS) WCHAR Paths[][MAX_PATH],
    _Out_ ULONG *Count)
{
    HDROP Drop;
    UINT FileCount;
    UINT Index;

    if (Count == NULL)
        return FALSE;
    *Count = 0;

    if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(NULL))
        return FALSE;

    Drop = (HDROP)GetClipboardData(CF_HDROP);
    if (Drop == NULL)
    {
        CloseClipboard();
        return FALSE;
    }

    FileCount = DragQueryFileW(Drop, 0xffffffff, NULL, 0);
    if (FileCount > TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS)
        FileCount = TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS;

    for (Index = 0; Index < FileCount; Index++)
    {
        if (DragQueryFileW(Drop, Index, Paths[Index], MAX_PATH) != 0)
            (*Count)++;
    }

    CloseClipboard();
    return *Count != 0;
}

static BOOL
TermSrvAddOutgoingPath(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_z_ const WCHAR *FullPath,
    _In_z_ const WCHAR *RelativeName)
{
    WIN32_FILE_ATTRIBUTE_DATA Attr;
    TERMSRV_CLIPRDR_FILE_DESCRIPTOR *Descriptor;

    if (Context->CliprdrOutgoingCount >= TERMSRV_CLIPRDR_MAX_WIRE_FILE_DESCRIPTORS)
        return FALSE;

    if (!GetFileAttributesExW(FullPath, GetFileExInfoStandard, &Attr))
        return FALSE;

    lstrcpynW(Context->CliprdrOutgoingPaths[Context->CliprdrOutgoingCount],
              FullPath,
              MAX_PATH);

    Descriptor = &Context->CliprdrOutgoingFiles.Descriptors[Context->CliprdrOutgoingCount];
    ZeroMemory(Descriptor, sizeof(*Descriptor));
    Descriptor->Flags = TERMSRV_CLIPRDR_FD_ATTRIBUTES |
                        TERMSRV_CLIPRDR_FD_WRITESTIME |
                        TERMSRV_CLIPRDR_FD_FILESIZE |
                        TERMSRV_CLIPRDR_FD_PROGRESSUI |
                        TERMSRV_CLIPRDR_FD_UNICODE;
    Descriptor->Attributes = Attr.dwFileAttributes;
    Descriptor->LastWriteTime = Attr.ftLastWriteTime;
    Descriptor->FileSizeHigh = Attr.nFileSizeHigh;
    Descriptor->FileSizeLow = Attr.nFileSizeLow;
    lstrcpynW(Descriptor->FileName,
              RelativeName,
              ARRAYSIZE(Descriptor->FileName));

    Context->CliprdrOutgoingCount++;
    Context->CliprdrOutgoingFiles.Count = Context->CliprdrOutgoingCount;
    return TRUE;
}

static BOOL
TermSrvAddOutgoingPathRecursive(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_z_ const WCHAR *FullPath,
    _In_z_ const WCHAR *RelativeName)
{
    WIN32_FILE_ATTRIBUTE_DATA Attr;
    WCHAR Pattern[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE Find;

    if (!TermSrvAddOutgoingPath(Context, FullPath, RelativeName))
        return FALSE;

    if (!GetFileAttributesExW(FullPath, GetFileExInfoStandard, &Attr) ||
        !(Attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        return TRUE;
    }

    if (_snwprintf(Pattern, MAX_PATH, L"%s\\*", FullPath) < 0)
        return FALSE;
    Pattern[MAX_PATH - 1] = 0;

    Find = FindFirstFileW(Pattern, &FindData);
    if (Find == INVALID_HANDLE_VALUE)
        return TRUE;

    do
    {
        WCHAR ChildFull[MAX_PATH];
        WCHAR ChildRelative[TERMSRV_CLIPRDR_MAX_FILE_NAME];

        if (lstrcmpW(FindData.cFileName, L".") == 0 ||
            lstrcmpW(FindData.cFileName, L"..") == 0)
        {
            continue;
        }

        if (_snwprintf(ChildFull,
                       MAX_PATH,
                       L"%s\\%s",
                       FullPath,
                       FindData.cFileName) < 0 ||
            _snwprintf(ChildRelative,
                       ARRAYSIZE(ChildRelative),
                       L"%s\\%s",
                       RelativeName,
                       FindData.cFileName) < 0)
        {
            FindClose(Find);
            return FALSE;
        }
        ChildFull[MAX_PATH - 1] = 0;
        ChildRelative[ARRAYSIZE(ChildRelative) - 1] = 0;

        if (!TermSrvAddOutgoingPathRecursive(Context, ChildFull, ChildRelative))
        {
            FindClose(Find);
            return FALSE;
        }
    }
    while (FindNextFileW(Find, &FindData));

    FindClose(Find);
    return TRUE;
}

static BOOL
TermSrvBuildOutgoingFileList(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context)
{
    WCHAR Selected[TERMSRV_CLIPRDR_MAX_FILE_DESCRIPTORS][MAX_PATH];
    ULONG SelectedCount;
    ULONG Index;

    if (!TermSrvReadHdropClipboard(Selected, &SelectedCount))
        return FALSE;

    ZeroMemory(&Context->CliprdrOutgoingFiles, sizeof(Context->CliprdrOutgoingFiles));
    ZeroMemory(Context->CliprdrOutgoingPaths, sizeof(Context->CliprdrOutgoingPaths));
    Context->CliprdrOutgoingCount = 0;

    for (Index = 0; Index < SelectedCount; Index++)
    {
        WCHAR *Leaf;

        Leaf = wcsrchr(Selected[Index], L'\\');
        Leaf = (Leaf != NULL) ? Leaf + 1 : Selected[Index];
        if (!TermSrvAddOutgoingPathRecursive(Context, Selected[Index], Leaf))
            return Context->CliprdrOutgoingCount != 0;
    }

    return Context->CliprdrOutgoingCount != 0;
}

static BOOL
TermSrvSendOutgoingFileList(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    UCHAR Payload[sizeof(ULONG) +
                  TERMSRV_CLIPRDR_MAX_WIRE_FILE_DESCRIPTORS * 592];
    UCHAR Pdu[sizeof(Payload) + TERMSRV_CLIPRDR_HEADER_LENGTH];
    SIZE_T PayloadLength;
    SIZE_T BytesWritten;

    if (!TermSrvBuildOutgoingFileList(Context))
        return FALSE;
    if (Context->CliprdrOutgoingFiles.Count > TERMSRV_CLIPRDR_MAX_WIRE_FILE_DESCRIPTORS)
        Context->CliprdrOutgoingFiles.Count = TERMSRV_CLIPRDR_MAX_WIRE_FILE_DESCRIPTORS;

    if (TermSrvCliprdrWriteFileList(Payload,
                                    sizeof(Payload),
                                    &Context->CliprdrOutgoingFiles,
                                    &PayloadLength) != TermSrvCliprdrSuccess)
    {
        return FALSE;
    }

    if (TermSrvCliprdrWriteFormatDataResponse(Pdu,
                                              sizeof(Pdu),
                                              TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                                              Payload,
                                              PayloadLength,
                                              &BytesWritten) != TermSrvCliprdrSuccess)
    {
        return FALSE;
    }

    return TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten);
}

static BOOL
TermSrvSendOutgoingFileContents(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ const TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST *Request)
{
    UCHAR Data[TERMSRV_CLIPRDR_FILE_CHUNK];
    UCHAR Pdu[TERMSRV_CLIPRDR_FILE_CHUNK + 16];
    SIZE_T BytesWritten;
    HANDLE File;
    LARGE_INTEGER Position;
    DWORD Read = 0;
    BOOL Ok;
    ULONGLONG Size;

    if (Request->ListIndex >= Context->CliprdrOutgoingCount)
    {
        if (TermSrvCliprdrWriteFileContentsResponse(Pdu,
                                                    sizeof(Pdu),
                                                    TERMSRV_CLIPRDR_CB_RESPONSE_FAIL,
                                                    Request->StreamId,
                                                    NULL,
                                                    0,
                                                    &BytesWritten) != TermSrvCliprdrSuccess)
            return FALSE;
        return TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten);
    }

    Size = TermSrvFileSizeFromDescriptor(
        &Context->CliprdrOutgoingFiles.Descriptors[Request->ListIndex]);

    if (Request->Flags & TERMSRV_CLIPRDR_FILECONTENTS_SIZE)
    {
        TermSrvWriteLe64(Data, Size);
        if (TermSrvCliprdrWriteFileContentsResponse(Pdu,
                                                    sizeof(Pdu),
                                                    TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                                                    Request->StreamId,
                                                    Data,
                                                    sizeof(ULONGLONG),
                                                    &BytesWritten) != TermSrvCliprdrSuccess)
        {
            return FALSE;
        }
        return TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten);
    }

    if (Request->Requested > TERMSRV_CLIPRDR_FILE_CHUNK ||
        (Context->CliprdrOutgoingFiles.Descriptors[Request->ListIndex].Attributes &
         FILE_ATTRIBUTE_DIRECTORY))
    {
        if (TermSrvCliprdrWriteFileContentsResponse(Pdu,
                                                    sizeof(Pdu),
                                                    TERMSRV_CLIPRDR_CB_RESPONSE_FAIL,
                                                    Request->StreamId,
                                                    NULL,
                                                    0,
                                                    &BytesWritten) != TermSrvCliprdrSuccess)
            return FALSE;
        return TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten);
    }

    File = CreateFileW(Context->CliprdrOutgoingPaths[Request->ListIndex],
                       GENERIC_READ,
                       FILE_SHARE_READ,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    Position.QuadPart = ((LONGLONG)Request->PositionHigh << 32) |
                        Request->PositionLow;
    Ok = SetFilePointerEx(File, Position, NULL, FILE_BEGIN) &&
         ReadFile(File, Data, Request->Requested, &Read, NULL);
    CloseHandle(File);
    if (!Ok)
        return FALSE;

    if (TermSrvCliprdrWriteFileContentsResponse(Pdu,
                                                sizeof(Pdu),
                                                TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                                                Request->StreamId,
                                                Data,
                                                Read,
                                                &BytesWritten) != TermSrvCliprdrSuccess)
    {
        return FALSE;
    }

    return TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten);
}

static ULONG
TermSrvHashBytes(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ SIZE_T Length)
{
    ULONG Hash = 2166136261u;
    SIZE_T Index;

    for (Index = 0; Index < Length; Index++)
    {
        Hash ^= Data[Index];
        Hash *= 16777619u;
    }

    return Hash;
}

static BOOL
TermSrvWriteAvailableClipboardFormats(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten,
    _Out_opt_ ULONG *Hash,
    _Out_opt_ SIZE_T *DataLength)
{
    UCHAR Scratch[8192];
    SIZE_T Required;
    TERMSRV_CLIPRDR_FORMAT Formats[8];
    ULONG Count = 0;
    ULONG LocalHash = 0;
    SIZE_T LocalLength = 0;
    ULONG Ids[] =
    {
        TERMSRV_CLIPRDR_CF_UNICODETEXT,
        TERMSRV_CLIPRDR_CF_TEXT,
        TERMSRV_CLIPRDR_CF_DIB,
        TERMSRV_CLIPRDR_CF_DIBV5
    };
    ULONG Index;

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    for (Index = 0; Index < RTL_NUMBER_OF(Ids); Index++)
    {
        TERMSRV_CLIPRDR_RESULT ClipResult;

        ClipResult = TermSrvCliprdrBackendGetData(&Context->CliprdrBackend,
                                                  Ids[Index],
                                                  Scratch,
                                                  sizeof(Scratch),
                                                  &Required);
        if (ClipResult != TermSrvCliprdrSuccess)
            continue;

        if (Count < RTL_NUMBER_OF(Formats))
        {
            ZeroMemory(&Formats[Count], sizeof(Formats[Count]));
            Formats[Count].FormatId = Ids[Index];
            Count++;
        }

        LocalHash ^= TermSrvHashBytes(Scratch, min(Required, sizeof(Scratch)));
        LocalLength += Required;
    }

    if (Count == 0)
    {
        if (TermSrvReadHdropClipboard(Context->CliprdrOutgoingPaths,
                                      &Context->CliprdrOutgoingCount))
        {
            if (Context->CliprdrFormatFileGroupDescriptorW == 0)
            {
                Context->CliprdrFormatFileGroupDescriptorW =
                    RegisterClipboardFormatW(L"FileGroupDescriptorW");
            }
            if (Count < RTL_NUMBER_OF(Formats))
            {
                ZeroMemory(&Formats[Count], sizeof(Formats[Count]));
                Formats[Count].FormatId = Context->CliprdrFormatFileGroupDescriptorW;
                lstrcpyW(Formats[Count].Name, L"FileGroupDescriptorW");
                Count++;
            }
            LocalHash ^= TermSrvHashBytes((const UCHAR *)Context->CliprdrOutgoingPaths,
                                          Context->CliprdrOutgoingCount * MAX_PATH * sizeof(WCHAR));
            LocalLength += Context->CliprdrOutgoingCount;
        }
    }
    else if (TermSrvReadHdropClipboard(Context->CliprdrOutgoingPaths,
                                       &Context->CliprdrOutgoingCount))
    {
        if (Context->CliprdrFormatFileGroupDescriptorW == 0)
        {
            Context->CliprdrFormatFileGroupDescriptorW =
                RegisterClipboardFormatW(L"FileGroupDescriptorW");
        }
        if (Count < RTL_NUMBER_OF(Formats))
        {
            ZeroMemory(&Formats[Count], sizeof(Formats[Count]));
            Formats[Count].FormatId = Context->CliprdrFormatFileGroupDescriptorW;
            lstrcpyW(Formats[Count].Name, L"FileGroupDescriptorW");
            Count++;
        }
        LocalHash ^= TermSrvHashBytes((const UCHAR *)Context->CliprdrOutgoingPaths,
                                      Context->CliprdrOutgoingCount * MAX_PATH * sizeof(WCHAR));
        LocalLength += Context->CliprdrOutgoingCount;
    }

    if (Count == 0)
        return TRUE;

    if (Hash != NULL)
        *Hash = LocalHash;
    if (DataLength != NULL)
        *DataLength = LocalLength;

    return TermSrvCliprdrWriteFormatList(Buffer,
                                         BufferLength,
                                         Formats,
                                         Count,
                                         BytesWritten) == TermSrvCliprdrSuccess;
}

static BOOL
TermSrvMaybeAdvertiseServerClipboard(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    UCHAR Pdu[2048];
    SIZE_T BytesWritten;
    ULONG Hash = 0;
    SIZE_T DataLength = 0;

    if (Channel == NULL ||
        Channel->ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID ||
        !Context->CliprdrMonitorReadySent)
    {
        return TRUE;
    }

    if (!TermSrvWriteAvailableClipboardFormats(Context,
                                               Pdu,
                                               sizeof(Pdu),
                                               &BytesWritten,
                                               &Hash,
                                               &DataLength))
    {
        return FALSE;
    }

    if (BytesWritten == 0)
        return TRUE;

    if (Hash == Context->CliprdrLastServerHash &&
        DataLength == Context->CliprdrLastServerLength)
    {
        return TRUE;
    }

    Context->CliprdrLastServerHash = Hash;
    Context->CliprdrLastServerLength = DataLength;
    if (Context->CliprdrSuppressNextServerAdvertise)
    {
        Context->CliprdrSuppressNextServerAdvertise = FALSE;
        return TRUE;
    }

    return TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten);
}

static BOOL
TermSrvSendInitialCliprdrMonitorReady(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    UCHAR Pdu[64];
    SIZE_T BytesWritten;

    if (Context->CliprdrMonitorReadySent ||
        Channel == NULL ||
        Channel->ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID)
    {
        return TRUE;
    }

    if (TermSrvCliprdrWriteCapabilities(Pdu,
                                         sizeof(Pdu),
                                         &BytesWritten) != TermSrvCliprdrSuccess)
    {
        return FALSE;
    }

    if (!TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten))
        return FALSE;

    DPRINT1("termsrv: cliprdr capabilities sent on channel %u\n",
            Channel->ChannelId);

    if (TermSrvCliprdrWriteMonitorReady(Pdu,
                                        sizeof(Pdu),
                                        &BytesWritten) != TermSrvCliprdrSuccess)
    {
        return FALSE;
    }

    if (!TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten))
        return FALSE;

    Context->CliprdrMonitorReadySent = TRUE;
    DPRINT1("termsrv: cliprdr monitor-ready sent on channel %u\n",
            Channel->ChannelId);
    return TRUE;
}

static BOOL
TermSrvRequestNextClientFormat(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    ULONG RemoteFormat = 0;
    ULONG LocalFormat = 0;
    UCHAR Pdu[64];
    SIZE_T BytesWritten;

    if (Context->CliprdrPendingFormatIndex >= Context->CliprdrPendingFormatCount)
        return TRUE;

    RemoteFormat = Context->CliprdrPendingRemoteFormats[Context->CliprdrPendingFormatIndex];
    LocalFormat = Context->CliprdrPendingLocalFormats[Context->CliprdrPendingFormatIndex];
    Context->CliprdrPendingFormatIndex++;

    Context->CliprdrBackend.PendingFormatId = LocalFormat;
    DPRINT1("termsrv: cliprdr requesting remote format 0x%lx as local 0x%lx\n",
            RemoteFormat,
            LocalFormat);
    if (TermSrvCliprdrWriteFormatDataRequest(Pdu,
                                             sizeof(Pdu),
                                             RemoteFormat,
                                             &BytesWritten) != TermSrvCliprdrSuccess)
    {
        return FALSE;
    }

    return TermSrvSendCliprdrPdu(Client, Channel, Pdu, BytesWritten);
}

static VOID
TermSrvQueueClientFormat(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ ULONG RemoteFormat,
    _In_ ULONG LocalFormat)
{
    ULONG Index;

    if (RemoteFormat == 0 || LocalFormat == 0)
        return;

    for (Index = 0; Index < Context->CliprdrPendingFormatCount; Index++)
    {
        if (Context->CliprdrPendingRemoteFormats[Index] == RemoteFormat &&
            Context->CliprdrPendingLocalFormats[Index] == LocalFormat)
        {
            return;
        }
    }

    if (Context->CliprdrPendingFormatCount >= TERMSRV_CLIPRDR_MAX_FORMATS)
        return;

    Index = Context->CliprdrPendingFormatCount++;
    Context->CliprdrPendingRemoteFormats[Index] = RemoteFormat;
    Context->CliprdrPendingLocalFormats[Index] = LocalFormat;
}

static BOOL
TermSrvQueueSupportedClientFormats(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _In_ const TERMSRV_CLIPRDR_FORMAT_LIST *FormatList)
{
    ULONG RemoteFormat;

    Context->CliprdrPendingFormatCount = 0;
    Context->CliprdrPendingFormatIndex = 0;
    Context->CliprdrBackend.ReplaceOnNextSet = TRUE;
    ZeroMemory(Context->CliprdrPendingRemoteFormats,
               sizeof(Context->CliprdrPendingRemoteFormats));
    ZeroMemory(Context->CliprdrPendingLocalFormats,
               sizeof(Context->CliprdrPendingLocalFormats));

    if (TermSrvCliprdrFormatListContains(FormatList, TERMSRV_CLIPRDR_CF_UNICODETEXT))
    {
        TermSrvQueueClientFormat(Context,
                                 TERMSRV_CLIPRDR_CF_UNICODETEXT,
                                 TERMSRV_CLIPRDR_CF_UNICODETEXT);
    }
    if (TermSrvCliprdrFormatListContains(FormatList, TERMSRV_CLIPRDR_CF_TEXT))
    {
        TermSrvQueueClientFormat(Context,
                                 TERMSRV_CLIPRDR_CF_TEXT,
                                 TERMSRV_CLIPRDR_CF_TEXT);
    }

    if (TermSrvCliprdrFormatListContains(FormatList, TERMSRV_CLIPRDR_CF_DIBV5))
    {
        TermSrvQueueClientFormat(Context,
                                 TERMSRV_CLIPRDR_CF_DIBV5,
                                 TERMSRV_CLIPRDR_CF_DIBV5);
    }
    else if (TermSrvCliprdrFormatListContains(FormatList, TERMSRV_CLIPRDR_CF_DIB))
    {
        TermSrvQueueClientFormat(Context,
                                 TERMSRV_CLIPRDR_CF_DIB,
                                 TERMSRV_CLIPRDR_CF_DIB);
    }

    RemoteFormat = TermSrvCliprdrFindNamedFormat(FormatList,
                                                 L"FileGroupDescriptorW");
    if (RemoteFormat != 0)
    {
        if (Context->CliprdrFormatFileGroupDescriptorW == 0)
        {
            Context->CliprdrFormatFileGroupDescriptorW =
                RegisterClipboardFormatW(L"FileGroupDescriptorW");
        }
        TermSrvQueueClientFormat(Context,
                                 RemoteFormat,
                                 Context->CliprdrFormatFileGroupDescriptorW);
    }

    RemoteFormat = TermSrvCliprdrFindNamedFormat(FormatList,
                                                 L"Preferred DropEffect");
    if (RemoteFormat != 0)
    {
        if (Context->CliprdrFormatPreferredDropEffect == 0)
        {
            Context->CliprdrFormatPreferredDropEffect =
                RegisterClipboardFormatW(L"Preferred DropEffect");
        }
        TermSrvQueueClientFormat(Context,
                                 RemoteFormat,
                                 Context->CliprdrFormatPreferredDropEffect);
    }

    return TermSrvRequestNextClientFormat(Context, Client, Channel);
}

static BOOL
TermSrvRouteOptionalCliprdrPacket(
    _In_ SOCKET Client,
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel,
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context)
{
    UCHAR CliprdrPayload[12288];
    SIZE_T BytesWritten;
    TERMSRV_CLIPRDR_RESULT ClipResult;
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD SendData;
    TERMSRV_RDPBCGR_RESULT Result;
    TERMSRV_CLIPRDR_FORMAT_LIST FormatList;
    TERMSRV_CLIPRDR_PDU ClipPdu;
    const UCHAR *ClipPayload;
    SIZE_T ClipPayloadLength;
    BOOL IsFormatList;
    BOOL IsDataResponse;
    BOOL IsFileContentsResponse;

    if (TermSrvIdentifyPacketPlaceholder(Buffer, Received) != TermSrvPacketTpkt)
    {
        TermSrvLogFailure("expected TPKT packet");
        return FALSE;
    }

    IsFormatList = FALSE;
    IsDataResponse = FALSE;
    IsFileContentsResponse = FALSE;
    ClipPayload = NULL;
    ClipPayloadLength = 0;
    Result = TermSrvRdpBcgrParseMcsSendDataPayload(Buffer,
                                                  (SIZE_T)Received,
                                                  &SendData);
    if (Result == TermSrvRdpBcgrSuccess)
    {
        ClipPayload = SendData.Payload;
        ClipPayloadLength = SendData.PayloadLength;
        TermSrvTryUnwrapChannelPayload(SendData.Payload,
                                       SendData.PayloadLength,
                                       &ClipPayload,
                                       &ClipPayloadLength);
        if (TermSrvCliprdrParseFormatList(ClipPayload,
                                          ClipPayloadLength,
                                          &FormatList) == TermSrvCliprdrSuccess)
        {
            IsFormatList = TRUE;
        }
        if (TermSrvCliprdrParsePdu(ClipPayload,
                                   ClipPayloadLength,
                                   &ClipPdu) == TermSrvCliprdrSuccess)
        {
            ULONG RequestedFormat;

            if (ClipPdu.MsgType == TERMSRV_CLIPRDR_CB_FORMAT_DATA_RESPONSE)
                IsDataResponse = TRUE;
            else if (ClipPdu.MsgType == TERMSRV_CLIPRDR_CB_FILECONTENTS_RESPONSE)
                IsFileContentsResponse = TRUE;
            else if (ClipPdu.MsgType == TERMSRV_CLIPRDR_CB_FORMAT_DATA_REQUEST &&
                     TermSrvCliprdrParseFormatDataRequest(ClipPayload,
                                                          ClipPayloadLength,
                                                          &RequestedFormat) == TermSrvCliprdrSuccess &&
                     RequestedFormat == Context->CliprdrFormatFileGroupDescriptorW &&
                     RequestedFormat != 0)
            {
                return TermSrvSendOutgoingFileList(Context, Client, Channel);
            }
            else if (ClipPdu.MsgType == TERMSRV_CLIPRDR_CB_FILECONTENTS_REQUEST)
            {
                TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST Request;

                if (TermSrvCliprdrParseFileContentsRequest(ClipPayload,
                                                           ClipPayloadLength,
                                                           &Request) != TermSrvCliprdrSuccess)
                {
                    return FALSE;
                }
                return TermSrvSendOutgoingFileContents(Context, Client, Channel, &Request);
            }

            if (ClipPdu.MsgType == TERMSRV_CLIPRDR_CB_FORMAT_DATA_RESPONSE &&
                Context->CliprdrBackend.PendingFormatId ==
                    Context->CliprdrFormatFileGroupDescriptorW &&
                Context->CliprdrFormatFileGroupDescriptorW != 0 &&
                (ClipPdu.MsgFlags & TERMSRV_CLIPRDR_CB_RESPONSE_OK))
            {
                Context->CliprdrSuppressNextServerAdvertise = TRUE;
                if (!TermSrvStartIncomingFiles(Context,
                                               Client,
                                               Channel,
                                               ClipPdu.Payload,
                                               ClipPdu.DataLength))
                {
                    TermSrvLogFailure("cliprdr incoming file clipboard import skipped");
                }
                return TRUE;
            }
        }
    }

    ClipResult = TermSrvCliprdrRouteMcsSendData(Buffer,
                                                (SIZE_T)Received,
                                                Channel,
                                                &Context->CliprdrBackend,
                                                CliprdrPayload,
                                                sizeof(CliprdrPayload),
                                                &BytesWritten);
    if (BytesWritten != 0)
    {
        if (!TermSrvSendCliprdrPdu(Client, Channel, CliprdrPayload, BytesWritten))
            return FALSE;
    }

    if (ClipResult == TermSrvCliprdrSuccess ||
        ClipResult == TermSrvCliprdrFormatNotAvailable)
    {
        if (IsFormatList &&
            !TermSrvQueueSupportedClientFormats(Context,
                                                Client,
                                                Channel,
                                                &FormatList))
        {
            return FALSE;
        }

        if (IsFileContentsResponse)
        {
            return TermSrvHandleIncomingFileContentsResponse(Context,
                                                            Client,
                                                            Channel,
                                                            ClipPdu.Payload,
                                                            ClipPdu.DataLength);
        }

        if (IsDataResponse &&
            Context->CliprdrBackend.PendingFormatId ==
                Context->CliprdrFormatFileGroupDescriptorW &&
            Context->CliprdrFormatFileGroupDescriptorW != 0 &&
            (ClipPdu.MsgFlags & TERMSRV_CLIPRDR_CB_RESPONSE_OK))
        {
            Context->CliprdrSuppressNextServerAdvertise = TRUE;
            if (!TermSrvStartIncomingFiles(Context,
                                           Client,
                                           Channel,
                                           ClipPdu.Payload,
                                           ClipPdu.DataLength))
            {
                TermSrvLogFailure("cliprdr incoming file clipboard import skipped");
            }
            return TRUE;
        }

        if (IsDataResponse &&
            !TermSrvRequestNextClientFormat(Context, Client, Channel))
        {
            return FALSE;
        }

        if (IsDataResponse &&
            Context->CliprdrPendingFormatIndex >= Context->CliprdrPendingFormatCount)
        {
            Context->CliprdrSuppressNextServerAdvertise = TRUE;
        }

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
TermSrvWriteServerGlobalPayloadEx(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _In_ BOOL PayloadHasSecurityHeader,
    _Out_ SIZE_T *BytesWritten)
{
    SIZE_T PacketLength;
    SIZE_T BodyLength;
    SIZE_T PayloadOffset;
    SIZE_T LengthBytes;
    SIZE_T EffectiveLength;
    BOOL AddSecurityHeader;

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    if (Buffer == NULL || Payload == NULL || PayloadLength > 0x7fff)
        return FALSE;

    /* Under RDP standard security every server-to-client PDU carries a basic
     * security header. Most PDUs supply the RDP payload without one, so prepend
     * an empty (flags = 0) 4-byte header. The license PDU already includes its
     * own SEC_LICENSE_PKT header, so it is passed through unchanged. */
    AddSecurityHeader = (!PayloadHasSecurityHeader && TermSrvServerSecurityHeaderActive());
    EffectiveLength = PayloadLength + (AddSecurityHeader ? 4 : 0);
    if (EffectiveLength > 0x7fff)
        return FALSE;

    BodyLength = ((EffectiveLength > 0x7f) ? 8 : 7) + EffectiveLength;
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
    if (!TermSrvWriteMcsPayloadLength(&Buffer[13], EffectiveLength, &LengthBytes))
        return FALSE;

    PayloadOffset = 13 + LengthBytes;
    if (AddSecurityHeader)
    {
        Buffer[PayloadOffset] = 0x00;
        Buffer[PayloadOffset + 1] = 0x00;
        Buffer[PayloadOffset + 2] = 0x00;
        Buffer[PayloadOffset + 3] = 0x00;
        PayloadOffset += 4;
    }
    CopyMemory(&Buffer[PayloadOffset], Payload, PayloadLength);

    *BytesWritten = PacketLength;
    return TRUE;
}

static BOOL
TermSrvWriteServerGlobalPayload(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength,
    _Out_ SIZE_T *BytesWritten)
{
    return TermSrvWriteServerGlobalPayloadEx(Buffer, BufferLength, Payload,
                                             PayloadLength, FALSE, BytesWritten);
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

    /* The payload already opens with a TS_SECURITY_HEADER (SEC_LICENSE_PKT), so
     * do not prepend the empty security header. */
    return TermSrvWriteServerGlobalPayloadEx(Buffer,
                                             BufferLength,
                                             Payload,
                                             sizeof(Payload),
                                             TRUE,
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
        0x20, 0x00,
        0x01, 0x00,
        0x01, 0x00,
        0x01, 0x00,
        0x00, 0x04,
        0x00, 0x03,
        0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x01, 0x00,
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
    UCHAR *Payload;
    SIZE_T PayloadLength;
    BOOL Ret;

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    if (Body == NULL && BodyLength != 0)
        return FALSE;

    PayloadLength = 18 + BodyLength;
    if (PayloadLength > 0x7fff)
        return FALSE;

    Payload = HeapAlloc(GetProcessHeap(), 0, PayloadLength);
    if (Payload == NULL)
        return FALSE;

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

    Ret = TermSrvWriteServerGlobalPayload(Buffer,
                                          BufferLength,
                                          Payload,
                                          PayloadLength,
                                          BytesWritten);
    HeapFree(GetProcessHeap(), 0, Payload);
    return Ret;
}

static BOOL
TermSrvWriteBitmapUpdatePacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT DestLeft,
    _In_ USHORT DestTop,
    _In_ USHORT Width,
    _In_ USHORT Height,
    _In_ USHORT BitsPerPixel,
    _In_reads_bytes_(BitmapLength) const UCHAR *Bitmap,
    _In_ SIZE_T BitmapLength,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR *Body;
    UCHAR *Rectangle;
    BOOL Ret;

    if (Bitmap == NULL ||
        BitmapLength > 0xffff ||
        BitmapLength != ((SIZE_T)Width * Height * ((BitsPerPixel + 7) / 8)))
    {
        if (BytesWritten != NULL)
            *BytesWritten = 0;
        return FALSE;
    }

    Body = HeapAlloc(GetProcessHeap(), 0, 4 + 18 + BitmapLength);
    if (Body == NULL)
    {
        if (BytesWritten != NULL)
            *BytesWritten = 0;
        return FALSE;
    }

    TermSrvWriteLe16(&Body[0], 0x0001);
    TermSrvWriteLe16(&Body[2], 1);

    Rectangle = &Body[4];
    TermSrvWriteLe16(&Rectangle[0], DestLeft);
    TermSrvWriteLe16(&Rectangle[2], DestTop);
    TermSrvWriteLe16(&Rectangle[4], DestLeft + Width - 1);
    TermSrvWriteLe16(&Rectangle[6], DestTop + Height - 1);
    TermSrvWriteLe16(&Rectangle[8], Width);
    TermSrvWriteLe16(&Rectangle[10], Height);
    TermSrvWriteLe16(&Rectangle[12], BitsPerPixel);
    TermSrvWriteLe16(&Rectangle[14], 0);
    TermSrvWriteLe16(&Rectangle[16], (USHORT)BitmapLength);
    CopyMemory(&Rectangle[18], Bitmap, BitmapLength);

    Ret = TermSrvWriteShareDataPacket(Buffer,
                                      BufferLength,
                                      TERMSRV_RDP_DATA_TYPE_UPDATE,
                                      Body,
                                      4 + 18 + BitmapLength,
                                      BytesWritten);
    HeapFree(GetProcessHeap(), 0, Body);
    return Ret;
}

static BOOL
TermSrvWriteTestBitmapUpdatePacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Pixels[TERMSRV_TEST_BITMAP_BYTES];
    ULONG X;
    ULONG Y;

    for (Y = 0; Y < TERMSRV_TEST_BITMAP_HEIGHT; Y++)
    {
        for (X = 0; X < TERMSRV_TEST_BITMAP_WIDTH; X++)
        {
            ULONG Red;
            ULONG Green;
            ULONG Blue;
            USHORT Pixel;

            Red = (X * 31) / (TERMSRV_TEST_BITMAP_WIDTH - 1);
            Green = (Y * 63) / (TERMSRV_TEST_BITMAP_HEIGHT - 1);
            Blue = (((X / 8) ^ (Y / 8)) & 1) ? 31 : 4;
            Pixel = (USHORT)((Red << 11) | (Green << 5) | Blue);
            TermSrvWriteLe16(&Pixels[((Y * TERMSRV_TEST_BITMAP_WIDTH) + X) * 2],
                             Pixel);
        }
    }

    return TermSrvWriteBitmapUpdatePacket(Buffer,
                                          BufferLength,
                                          0,
                                          0,
                                          TERMSRV_TEST_BITMAP_WIDTH,
                                          TERMSRV_TEST_BITMAP_HEIGHT,
                                          TERMSRV_TEST_BITMAP_BPP,
                                          Pixels,
                                          sizeof(Pixels),
                                          BytesWritten);
}

static BOOL
TermSrvCaptureRdpFrame(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _Out_ TERMSRV_NTUSER_RDP_FRAME *Frame,
    _Outptr_ UCHAR **Pixels)
{
    ULONG BytesReturned;
    UCHAR *CapturedPixels;

    *Pixels = NULL;
    ZeroMemory(Frame, sizeof(*Frame));

    if (!TermSrvOpenRdpCaptureSession(Context))
        return FALSE;

    Context->NtUserRdpCaptureFrame(Context->RdpCaptureSession,
                                   Frame,
                                   NULL,
                                   0,
                                   &BytesReturned);
    if (Frame->Size != sizeof(*Frame) ||
        Frame->Width == 0 ||
        Frame->Height == 0 ||
        Frame->BitsPerPixel != TERMSRV_CAPTURE_BITMAP_BPP ||
        Frame->Pitch < Frame->Width * TERMSRV_CAPTURE_BITMAP_BYTES_PER_PIXEL ||
        Frame->Format != TERMSRV_NTUSER_RDP_FRAME_FORMAT_BGRA32 ||
        Frame->RequiredBufferSize < Frame->Pitch * Frame->Height)
    {
        TermSrvLogFailure("RDP capture fallback: NtUserRdpCaptureFrame returned unsupported metadata");
        return FALSE;
    }

    CapturedPixels = HeapAlloc(GetProcessHeap(), 0, Frame->RequiredBufferSize);
    if (CapturedPixels == NULL)
    {
        TermSrvLogFailure("RDP capture failed: unable to allocate pixel buffer");
        return FALSE;
    }

    BytesReturned = 0;
    if (!Context->NtUserRdpCaptureFrame(Context->RdpCaptureSession,
                                        Frame,
                                        CapturedPixels,
                                        Frame->RequiredBufferSize,
                                        &BytesReturned) ||
        BytesReturned != Frame->RequiredBufferSize)
    {
        HeapFree(GetProcessHeap(), 0, CapturedPixels);
        TermSrvLogFailure("RDP capture fallback: NtUserRdpCaptureFrame failed");
        return FALSE;
    }

    *Pixels = CapturedPixels;
    return TRUE;
}

static BOOL
TermSrvSendCapturedBitmapUpdate(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client)
{
    enum { MaxBitmapBytesPerPacket = 0x7fff - 40 };
    TERMSRV_NTUSER_RDP_FRAME Frame;
    UCHAR *CapturedPixels;
    UCHAR *BitmapData;
    UCHAR *Packet;
    ULONG RowBytes;
    ULONG MaxRowsPerPacket;
    ULONG Top;
    BOOL Ret;

    if (!TermSrvIsConsoleBackend(Context))
        return FALSE;

    if (!TermSrvCaptureRdpFrame(Context, &Frame, &CapturedPixels))
        return FALSE;

    if (Frame.Width > 0xffff || Frame.Height > 0xffff)
    {
        HeapFree(GetProcessHeap(), 0, CapturedPixels);
        TermSrvLogFailure("RDP capture failed: frame dimensions exceed bitmap update limits");
        return FALSE;
    }

    RowBytes = Frame.Width * TERMSRV_CAPTURE_BITMAP_BYTES_PER_PIXEL;
    MaxRowsPerPacket = MaxBitmapBytesPerPacket / RowBytes;
    if (MaxRowsPerPacket == 0)
    {
        HeapFree(GetProcessHeap(), 0, CapturedPixels);
        TermSrvLogFailure("RDP capture failed: frame width exceeds uncompressed bitmap update limit");
        return FALSE;
    }

    BitmapData = HeapAlloc(GetProcessHeap(), 0, MaxRowsPerPacket * RowBytes);
    Packet = HeapAlloc(GetProcessHeap(), 0, MaxBitmapBytesPerPacket + 128);
    if (BitmapData == NULL || Packet == NULL)
    {
        HeapFree(GetProcessHeap(), 0, Packet);
        HeapFree(GetProcessHeap(), 0, BitmapData);
        HeapFree(GetProcessHeap(), 0, CapturedPixels);
        TermSrvLogFailure("RDP capture failed: unable to allocate bitmap packet buffers");
        return FALSE;
    }

    Ret = TRUE;
    for (Top = 0; Top < Frame.Height; Top += MaxRowsPerPacket)
    {
        ULONG TileHeight;
        SIZE_T BitmapLength;
        SIZE_T BytesWritten;
        const UCHAR *TileSource;

        TileHeight = Frame.Height - Top;
        if (TileHeight > MaxRowsPerPacket)
            TileHeight = MaxRowsPerPacket;

        TileSource = CapturedPixels + (Top * Frame.Pitch);
        BitmapLength = (SIZE_T)TileHeight * RowBytes;
        if (!TermSrvConvertBgra32ToRdpBitmapData(TileSource,
                                                 Frame.Width,
                                                 TileHeight,
                                                 Frame.Pitch,
                                                 BitmapData) ||
            !TermSrvWriteBitmapUpdatePacket(Packet,
                                            MaxBitmapBytesPerPacket + 128,
                                            0,
                                            (USHORT)Top,
                                            (USHORT)Frame.Width,
                                            (USHORT)TileHeight,
                                            TERMSRV_CAPTURE_BITMAP_BPP,
                                            BitmapData,
                                            BitmapLength,
                                            &BytesWritten) ||
            !TermSrvSendPacket(Client, Packet, BytesWritten))
        {
            TermSrvLogFailure("RDP capture failed: bitmap update packet write failed");
            Ret = FALSE;
            break;
        }
    }

    HeapFree(GetProcessHeap(), 0, Packet);
    HeapFree(GetProcessHeap(), 0, BitmapData);
    HeapFree(GetProcessHeap(), 0, CapturedPixels);
    return Ret;
}

static BOOL
TermSrvWritePointerMarkerPacket(
    _Out_writes_bytes_to_(BufferLength, *BytesWritten) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ USHORT PointerX,
    _In_ USHORT PointerY,
    _Out_ SIZE_T *BytesWritten)
{
    UCHAR Pixels[TERMSRV_POINTER_MARKER_BYTES];
    ULONG X;
    ULONG Y;
    USHORT DestLeft;
    USHORT DestTop;

    DestLeft = (PointerX > TERMSRV_POINTER_MARKER_SIZE / 2) ?
               PointerX - TERMSRV_POINTER_MARKER_SIZE / 2 :
               0;
    DestTop = (PointerY > TERMSRV_POINTER_MARKER_SIZE / 2) ?
              PointerY - TERMSRV_POINTER_MARKER_SIZE / 2 :
              0;

    for (Y = 0; Y < TERMSRV_POINTER_MARKER_SIZE; Y++)
    {
        for (X = 0; X < TERMSRV_POINTER_MARKER_SIZE; X++)
        {
            USHORT Pixel;

            Pixel = (X == Y ||
                     X + Y == TERMSRV_POINTER_MARKER_SIZE - 1 ||
                     X == TERMSRV_POINTER_MARKER_SIZE / 2 ||
                     Y == TERMSRV_POINTER_MARKER_SIZE / 2) ? 0xffff : 0x001f;
            TermSrvWriteLe16(&Pixels[((Y * TERMSRV_POINTER_MARKER_SIZE) + X) * 2],
                             Pixel);
        }
    }

    return TermSrvWriteBitmapUpdatePacket(Buffer,
                                          BufferLength,
                                          DestLeft,
                                          DestTop,
                                          TERMSRV_POINTER_MARKER_SIZE,
                                          TERMSRV_POINTER_MARKER_SIZE,
                                          TERMSRV_TEST_BITMAP_BPP,
                                          Pixels,
                                          sizeof(Pixels),
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
    _In_ SOCKET Client,
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received)
{
    UCHAR Reply[1024];
    SIZE_T BytesWritten;
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

    if (InputEvents.FirstMessageType == 0x8001)
    {
        CHAR PeerPacket[64];

        _snprintf(PeerPacket,
                  sizeof(PeerPacket),
                  "SLOW_INPUT mouse=1 x=%u y=%u flags=%u",
                  InputEvents.FirstPointerX,
                  InputEvents.FirstPointerY,
                  InputEvents.FirstDeviceFlags);
        PeerPacket[sizeof(PeerPacket) - 1] = '\0';
        TermSrvAdvancePeer(Context, PeerPacket, "slow-path input delivery");
    }
    else if (InputEvents.FirstMessageType == 0x0004)
    {
        CHAR PeerPacket[80];

        _snprintf(PeerPacket,
                  sizeof(PeerPacket),
                  "SLOW_INPUT scancode=%u flags=%u",
                  InputEvents.FirstKeyboardCode,
                  InputEvents.FirstDeviceFlags);
        PeerPacket[sizeof(PeerPacket) - 1] = '\0';
        TermSrvAdvancePeer(Context, PeerPacket, "slow-path input delivery");
    }
    else
    {
        TermSrvAdvancePeer(Context, "SLOW_INPUT wire=rdpbcgr", "slow-path input delivery");
    }

    if (InputEvents.FirstMessageType == 0x8001 &&
        TermSrvWritePointerMarkerPacket(Reply,
                                        sizeof(Reply),
                                        InputEvents.FirstPointerX,
                                        InputEvents.FirstPointerY,
                                        &BytesWritten))
    {
        TermSrvSendPacket(Client, Reply, BytesWritten);
    }
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
            !TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received, &Context->Crypt))
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
            TERMSRV_RDP_STATUS Status;
            BOOL SyntheticFrameEnabled;
            BOOL SentBitmap;

            Status = TermSrvRdpPeerSendBitmapUpdate(&Context->Peer,
                                                    TERMSRV_TEST_BITMAP_WIDTH,
                                                    TERMSRV_TEST_BITMAP_HEIGHT);
            if (Status != TermSrvRdpSuccess)
                TermSrvLogRdpPeerFailure("initial win32 frame capture", Status);

            if (!TermSrvWriteServerFontMapPacket(Reply, ReplyLength, &BytesWritten) ||
                !TermSrvSendPacket(Client, Reply, BytesWritten))
            {
                TermSrvLogFailure("server font-map packet write failed");
                return FALSE;
            }

            SyntheticFrameEnabled = TermSrvSyntheticFrameEnabled();
            SentBitmap = FALSE;
            if (Status == TermSrvRdpSuccess && !SyntheticFrameEnabled)
                SentBitmap = TermSrvSendCapturedBitmapUpdate(Context, Client);

            if (!SentBitmap)
            {
                if (!SyntheticFrameEnabled && Context->RdpCaptureAvailable)
                {
                    TermSrvLogFailure("server bitmap packet write failed: captured frame was unavailable");
                    return FALSE;
                }

                TermSrvLogSyntheticFallbackOnce(Context,
                                                SyntheticFrameEnabled ?
                                                "REACTOS_TERMSRV_SYNTHETIC_FRAME=1" :
                                                "private RDP capture API could not be loaded");

                if (!TermSrvWriteTestBitmapUpdatePacket(Reply, ReplyLength, &BytesWritten) ||
                    !TermSrvSendPacket(Client, Reply, BytesWritten))
                {
                    TermSrvLogFailure("server bitmap packet write failed");
                    return FALSE;
                }
            }

            SentFontMap = TRUE;
            break;
        }

        Received = 0;
    }

    return SentFontMap || !SawConfirmActive;
}

static BOOL
TermSrvHandleActivePacket(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_reads_bytes_(Received) const UCHAR *Buffer,
    _In_ INT Received,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    switch (TermSrvIdentifyPacketPlaceholder(Buffer, Received))
    {
        case TermSrvPacketFastPath:
        {
            UCHAR Reply[1024];
            SIZE_T BytesWritten;
            TERMSRV_RDPBCGR_FASTPATH_INPUT_EVENTS InputEvents;

            if (TermSrvRdpBcgrParseFastPathInputEvents(Buffer,
                                                       (SIZE_T)Received,
                                                       &InputEvents) != TermSrvRdpBcgrSuccess)
            {
                TermSrvAdvancePeer(Context,
                                   "FAST_INPUT wire=fastpath",
                                   "fast-path input delivery");
            }
            else if (InputEvents.FirstEventCode == 0x01 ||
                     InputEvents.FirstEventCode == 0x05)
            {
                CHAR PeerPacket[64];

                _snprintf(PeerPacket,
                          sizeof(PeerPacket),
                          "FAST_INPUT mouse=1 x=%u y=%u",
                          InputEvents.FirstPointerX,
                          InputEvents.FirstPointerY);
                PeerPacket[sizeof(PeerPacket) - 1] = '\0';
                TermSrvAdvancePeer(Context,
                                   PeerPacket,
                                   "fast-path input delivery");
                if (TermSrvWritePointerMarkerPacket(Reply,
                                                    sizeof(Reply),
                                                    InputEvents.FirstPointerX,
                                                    InputEvents.FirstPointerY,
                                                    &BytesWritten))
                {
                    TermSrvSendPacket(Client, Reply, BytesWritten);
                }
            }
            else if (InputEvents.FirstEventCode == 0x00)
            {
                CHAR PeerPacket[64];

                /*
                 * Fast-path scancode event: FirstEventFlags carries the
                 * fast-path keyboard flags (bit 0 = release, bit 1 = extended)
                 * which the peer state machine maps onto the RDP keyboard flags
                 * before calling the backend InjectKeyboard entry point.
                 */
                _snprintf(PeerPacket,
                          sizeof(PeerPacket),
                          "FAST_INPUT scancode=%u flags=%u",
                          InputEvents.FirstKeyboardCode,
                          InputEvents.FirstEventFlags);
                PeerPacket[sizeof(PeerPacket) - 1] = '\0';
                TermSrvAdvancePeer(Context,
                                   PeerPacket,
                                   "fast-path input delivery");
            }
            else
            {
                TermSrvAdvancePeer(Context,
                                   "FAST_INPUT wire=fastpath",
                                   "fast-path input delivery");
            }
            break;
        }

        case TermSrvPacketTpkt:
            if (TermSrvIsCliprdrMcsPacket(Buffer, Received, Channel))
            {
                if (!TermSrvRouteOptionalCliprdrPacket(Client,
                                                      Buffer,
                                                      Received,
                                                      Channel,
                                                      Context))
                {
                    return FALSE;
                }
            }
            else
            {
                TermSrvHandleSlowPathInputPacket(Context, Client, Buffer, Received);
            }
            break;

        default:
            TermSrvLogFailure("ignoring unknown active RDP packet");
            break;
    }

    return TRUE;
}

static BOOL
TermSrvRunActiveLoop(
    _Inout_ TERMSRV_CLIENT_CONTEXT *Context,
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Out_writes_bytes_(BufferLength) UCHAR *Buffer,
    _In_ INT BufferLength,
    _In_ const TERMSRV_CLIPRDR_CHANNEL *Channel)
{
    fd_set ReadSet;
    TIMEVAL Timeout;
    INT Received;
    INT SelectResult;
    INT PendingLength;

    if (!TermSrvSendInitialCliprdrMonitorReady(Context, Client, Channel))
    {
        TermSrvLogFailure("cliprdr monitor-ready send failed");
        return FALSE;
    }

    PendingLength = 0;
    while (!TermSrvStopRequested(StopEvent))
    {
        FD_ZERO(&ReadSet);
        FD_SET(Client, &ReadSet);
        Timeout.tv_sec = 0;
        Timeout.tv_usec = TERMSRV_SELECT_TIMEOUT_MS * 1000;

        SelectResult = select(0, &ReadSet, NULL, NULL, &Timeout);
        if (SelectResult == 0)
        {
            if (!TermSrvMaybeAdvertiseServerClipboard(Context, Client, Channel))
            {
                TermSrvLogFailure("server clipboard format-list send failed during idle sync");
                return FALSE;
            }
            continue;
        }

        if (SelectResult == SOCKET_ERROR)
        {
            TermSrvLogSocketFailure("select");
            return FALSE;
        }

        if (!FD_ISSET(Client, &ReadSet))
            continue;

        if (PendingLength >= BufferLength)
        {
            TermSrvLogFailure("active packet buffer is full before packet completion");
            return FALSE;
        }

        Received = recv(Client,
                        (char *)&Buffer[PendingLength],
                        BufferLength - PendingLength,
                        0);
        if (Received == 0)
            return TRUE;

        if (Received == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                continue;

            TermSrvLogSocketFailure("recv");
            return FALSE;
        }

        PendingLength += Received;
        while (PendingLength > 0)
        {
            INT PacketLength;

            if (!TermSrvTryGetActivePacketLength(Buffer,
                                                 PendingLength,
                                                 &PacketLength))
            {
                UCHAR Reply[1024];
                SIZE_T BytesWritten;
                TERMSRV_RDPBCGR_RESULT Result;
                TERMSRV_RDPBCGR_FASTPATH_INPUT_EVENTS InputEvents;

                Result = TermSrvRdpBcgrParseFastPathInputEvents(Buffer,
                                                                (SIZE_T)Received,
                                                                &InputEvents);
                if (Result == TermSrvRdpBcgrSuccess &&
                    (InputEvents.FirstEventCode == 0x01 ||
                     InputEvents.FirstEventCode == 0x05))
                {
                    CHAR PeerPacket[64];

                    _snprintf(PeerPacket,
                              sizeof(PeerPacket),
                              "FAST_INPUT mouse=1 x=%u y=%u flags=%u",
                              InputEvents.FirstPointerX,
                              InputEvents.FirstPointerY,
                              InputEvents.FirstEventFlags);
                    PeerPacket[sizeof(PeerPacket) - 1] = '\0';
                    TermSrvAdvancePeer(Context,
                                       PeerPacket,
                                       "fast-path input delivery");
                    if (TermSrvWritePointerMarkerPacket(Reply,
                                                        sizeof(Reply),
                                                        InputEvents.FirstPointerX,
                                                        InputEvents.FirstPointerY,
                                                        &BytesWritten))
                    {
                        TermSrvSendPacket(Client, Reply, BytesWritten);
                    }
                }
                else if (Result == TermSrvRdpBcgrSuccess &&
                         InputEvents.FirstEventCode == 0x00)
                {
                    CHAR PeerPacket[80];

                    _snprintf(PeerPacket,
                              sizeof(PeerPacket),
                              "FAST_INPUT scancode=%u flags=%u",
                              InputEvents.FirstKeyboardCode,
                              InputEvents.FirstEventFlags);
                    PeerPacket[sizeof(PeerPacket) - 1] = '\0';
                    TermSrvAdvancePeer(Context,
                                       PeerPacket,
                                       "fast-path input delivery");
                }
                else
                {
                    TermSrvAdvancePeer(Context,
                                       "FAST_INPUT wire=fastpath",
                                       "fast-path input delivery");
                }
                break;
            }

            if (PacketLength == 0)
                break;

            if (PacketLength > BufferLength)
            {
                TermSrvLogFailure("active packet exceeds receive buffer");
                return FALSE;
            }

            if (PendingLength < PacketLength)
                break;

            {
                /* Decrypt in place when RDP standard security is active. The
                 * decrypted PDU is shorter (MAC stripped), but the original
                 * on-wire length still governs how far to advance the buffer. */
                SIZE_T DecryptedLength = (SIZE_T)PacketLength;

                TermSrvCryptUnwrapInbound(&Context->Crypt, Buffer, &DecryptedLength);

                if (!TermSrvHandleActivePacket(Context,
                                               Client,
                                               Buffer,
                                               (INT)DecryptedLength,
                                               Channel))
                {
                    return FALSE;
                }
            }

            PendingLength -= PacketLength;
            if (PendingLength != 0)
                MoveMemory(Buffer, &Buffer[PacketLength], PendingLength);
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

    /* The Client Info PDU is the first encrypted client-to-server packet under
     * RDP standard security. Decrypt it here so the RC4 stream stays in sync
     * for every later PDU. This is idempotent if it was already unwrapped on
     * receipt (the SEC_ENCRYPT flag is cleared after the first pass). */
    {
        SIZE_T DecryptedLength = (SIZE_T)Received;
        TermSrvCryptUnwrapInbound(&Context->Crypt, Buffer, &DecryptedLength);
        Received = (INT)DecryptedLength;
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

    if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received, &Context->Crypt))
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
                                Channel);
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
                                          &Context->CliprdrWin32,
                                          &Context->CliprdrBackend))
    {
        return FALSE;
    }

    TermSrvTryAssignCliprdrFromStaticChannelList(&Context->CliprdrChannel,
                                                 StaticChannelList);

    if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received, &Context->Crypt))
        return FALSE;

    Result = TermSrvRdpBcgrParseMcsErectDomainRequest(Buffer, (SIZE_T)Received);
    if (Result != TermSrvRdpBcgrSuccess)
    {
        TermSrvLogRdpBcgrFailure("MCS erect domain request parse", Result);
        return FALSE;
    }
    TermSrvAdvancePeer(Context, "ERECT", "MCS erect domain session transition");

    if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received, &Context->Crypt))
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

        if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received, &Context->Crypt))
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
                TermSrvCryptCompleteKeyExchange(&Context->Crypt,
                                                SecurityExchange.Payload,
                                                SecurityExchange.PayloadLength);
                TermSrvAdvancePeer(Context,
                                   "SECURITY client_random=wire",
                                   "security exchange session transition");
                break;
            }

            Result = TermSrvRdpBcgrParseClientInfoPayload(Buffer,
                                                          (SIZE_T)Received,
                                                          &SecurityExchange);
            if (Result == TermSrvRdpBcgrSuccess)
            {
                InitialClientInfoReceived = Received;
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

        if (ChannelJoinRequest.ChannelId == TERMSRV_MCS_WIRE_USER_CHANNEL_ID ||
            ChannelJoinRequest.ChannelId == TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID ||
            ChannelJoinRequest.ChannelId == 1003)
        {
            CHAR PeerPacket[32];
            USHORT PeerChannelId;

            PeerChannelId = ChannelJoinRequest.ChannelId;
            if (PeerChannelId == TERMSRV_MCS_WIRE_USER_CHANNEL_ID)
                PeerChannelId = TERMSRV_MCS_SCAFFOLD_USER_CHANNEL_ID;

            _snprintf(PeerPacket,
                      sizeof(PeerPacket),
                      "JOIN id=%u",
                      PeerChannelId);
            PeerPacket[sizeof(PeerPacket) - 1] = '\0';
            TermSrvAdvancePeer(Context, PeerPacket, "MCS channel join session transition");
        }
    }

    if (!HaveSecurityExchange && InitialClientInfoReceived <= 0)
    {
        if (!TermSrvReceiveTpkt(Client, StopEvent, Buffer, BufferLength, &Received, &Context->Crypt))
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
            TermSrvCryptCompleteKeyExchange(&Context->Crypt,
                                            SecurityExchange.Payload,
                                            SecurityExchange.PayloadLength);
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
                                                         NoAuthEnabled))
    {
        return FALSE;
    }

    return TRUE;
}

static VOID
TermSrvHandleClient(
    _In_ SOCKET Client,
    _In_ HANDLE StopEvent,
    _Inout_ TERMSRV_SESSION_MANAGER *SessionManager)
{
    UCHAR Buffer[TERMSRV_ACTIVE_BUFFER_LENGTH];
    TERMSRV_CLIENT_CONTEXT Context;
    INT Received;

    ZeroMemory(&Context, sizeof(Context));
    TermSrvRdpPeerInit(&Context.Peer, SessionManager);
    TermSrvCryptInit(&Context.Crypt);
    /* Connections are handled serially (see TermSrvListenerLoop), so a single
     * pointer to the active connection's crypto state lets the low-level server
     * PDU writers add the basic security header once encryption is negotiated,
     * without threading the state through every builder. */
    g_TermSrvActiveCrypt = &Context.Crypt;

    TermSrvSetNonBlocking(Client);

    Received = TermSrvReceiveWithTimeout(Client, StopEvent, Buffer, sizeof(Buffer));

    if (Received > 0)
    {
        TERMSRV_RDPBCGR_CONNECTION_REQUEST Request;
        TERMSRV_RDPBCGR_CONNECTION_CONFIRM Confirm;
        UCHAR Reply[12288];
        UCHAR ConnectResponsePayload[512];
        SIZE_T ConnectResponsePayloadLength;
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
                HaveStaticChannelList = TermSrvTryParseGccStaticChannelList(
                    ConnectInitial.Payload,
                    ConnectInitial.PayloadLength,
                    &StaticChannelList);
                if (!HaveStaticChannelList)
                {
                    HaveStaticChannelList = TermSrvTryParseScaffoldStaticChannelList(
                        ConnectInitial.Payload,
                        ConnectInitial.PayloadLength,
                        &StaticChannelList);
                }

                ConnectResponsePayloadLength = TermSrvBuildMcsConnectResponsePayload(
                    ConnectResponsePayload,
                    sizeof(ConnectResponsePayload),
                    HaveStaticChannelList ? &StaticChannelList : NULL,
                    &Context.Crypt);
                if (ConnectResponsePayloadLength == 0)
                    goto Cleanup;

                Result = TermSrvRdpBcgrWriteMcsConnectResponse(Reply,
                                                               sizeof(Reply),
                                                               ConnectResponsePayload,
                                                               ConnectResponsePayloadLength,
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
    g_TermSrvActiveCrypt = NULL;
    TermSrvCloseRdpCaptureApi(&Context);
    closesocket(Client);
}

static DWORD
TermSrvListenerLoop(
    _In_ SOCKET ListenSocket,
    _In_ HANDLE StopEvent,
    _Inout_ TERMSRV_SESSION_MANAGER *SessionManager)
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

            TermSrvHandleClient(Client, StopEvent, SessionManager);

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
    TERMSRV_SESSION_MANAGER SessionManager;

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
    Address.sin_addr.s_addr = htonl(INADDR_ANY);
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

    TermSrvSessionManagerInit(&SessionManager);
    TermSrvSelectListenerBackend(&SessionManager);
    Result = TermSrvListenerLoop(ListenSocket, StopEvent, &SessionManager);

    closesocket(ListenSocket);
    WSACleanup();
    return Result;
}
