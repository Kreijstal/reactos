/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal server-side RDP state engine
 *
 * This is deliberately a protocol/session-manager scaffold. It models the
 * classic RDP server phases and the TermService/session-manager boundary, but
 * it does not parse MS-RDPBCGR byte streams yet.
 */

#define WIN32_NO_STATUS
#include "termsrv.h"

#include <windows.h>
#undef WIN32_NO_STATUS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTOS_MODE_USER
#include <ndk/lpctypes.h>
#include <ndk/lpcfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <sm/smmsg.h>

#define TERMSRV_RDP_MOUSE_FLAG_MOVE 0x00000800
#define TERMSRV_RDP_KEYBOARD_FLAG_EXTENDED 0x00000100
#define TERMSRV_RDP_KEYBOARD_FLAG_RELEASE 0x00008000
#define TERMSRV_FASTPATH_KEYBOARD_FLAG_RELEASE 0x01
#define TERMSRV_FASTPATH_KEYBOARD_FLAG_EXTENDED 0x02

#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#endif

typedef struct _TERMSRV_RDP_MOUSE_INPUT
{
    ULONG Size;
    ULONG SessionId;
    ULONG PointerFlags;
    USHORT PointerX;
    USHORT PointerY;
    ULONG Flags;
} TERMSRV_RDP_MOUSE_INPUT;

typedef struct _TERMSRV_RDP_KEYBOARD_INPUT
{
    ULONG Size;
    ULONG SessionId;
    USHORT KeyboardFlags;
    USHORT KeyCode;
    ULONG Flags;
} TERMSRV_RDP_KEYBOARD_INPUT;

static VOID
RecordCall(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_z_ PCSTR Name)
{
    if (Manager->CallCount < TERMSRV_RDP_MAX_CALLS)
    {
        strncpy(Manager->Calls[Manager->CallCount], Name, sizeof(Manager->Calls[0]) - 1);
        Manager->Calls[Manager->CallCount][sizeof(Manager->Calls[0]) - 1] = ANSI_NULL;
        Manager->CallCount++;
    }
}

static BOOL
PacketStartsWith(
    _In_z_ PCSTR Packet,
    _In_z_ PCSTR Prefix)
{
    return strncmp(Packet, Prefix, strlen(Prefix)) == 0;
}

static INT
ParseIntAfter(
    _In_z_ PCSTR Packet,
    _In_z_ PCSTR Key,
    _In_ INT Fallback)
{
    PCSTR Found = strstr(Packet, Key);
    if (Found == NULL)
        return Fallback;

    return atoi(Found + strlen(Key));
}

static VOID
ParseUserIdentity(
    _In_z_ PCSTR Packet,
    _Out_writes_(OutputLength) PWSTR Output,
    _In_ SIZE_T OutputLength)
{
    PCSTR Found;
    SIZE_T Index = 0;

    if (OutputLength == 0)
        return;

    Found = strstr(Packet, "user=");
    if (Found == NULL)
    {
        wcsncpy(Output, L"DOMAIN\\alice", OutputLength - 1);
        Output[OutputLength - 1] = UNICODE_NULL;
        return;
    }

    Found += 5;
    while (*Found != ANSI_NULL && *Found != ';' && Index + 1 < OutputLength)
    {
        Output[Index++] = (WCHAR)(UCHAR)*Found++;
    }
    Output[Index] = UNICODE_NULL;
}

static VOID
SetOutput(
    _Inout_ TERMSRV_RDP_PEER *Peer,
    _In_z_ PCSTR Output)
{
    strncpy(Peer->LastOutput, Output, sizeof(Peer->LastOutput) - 1);
    Peer->LastOutput[sizeof(Peer->LastOutput) - 1] = ANSI_NULL;
}

static BOOL
ChannelIsJoined(
    _In_ const TERMSRV_RDP_PEER *Peer,
    _In_ INT ChannelId)
{
    INT i;

    for (i = 0; i < Peer->JoinedCount; i++)
    {
        if (Peer->JoinedChannels[i] == ChannelId)
            return TRUE;
    }

    return FALSE;
}

static INT
StaticChannelIndexById(
    _In_ const TERMSRV_RDP_PEER *Peer,
    _In_ INT ChannelId)
{
    INT i;

    for (i = 0; i < Peer->ChannelCount; i++)
    {
        if (Peer->ChannelIds[i] == ChannelId)
            return i;
    }

    return -1;
}

static TERMSRV_RDP_STATUS
RequireState(
    _In_ const TERMSRV_RDP_PEER *Peer,
    _In_ TERMSRV_RDP_STATE State)
{
    return (Peer->State == State) ? TermSrvRdpSuccess : TermSrvRdpOutOfOrder;
}

static TERMSRV_SESSION *
FindSessionInternal(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    INT i;

    for (i = 0; i < Manager->SessionCount; i++)
    {
        if (Manager->Sessions[i].SessionId == SessionId)
            return &Manager->Sessions[i];
    }

    return NULL;
}

static VOID
InitializeSessionRecord(
    _Out_ TERMSRV_SESSION *Session,
    _In_ INT SessionId,
    _In_ TERMSRV_SESSION_STATE State)
{
    memset(Session, 0, sizeof(*Session));
    Session->SessionId = SessionId;
    Session->Win32SessionId = (ULONG)SessionId;
    Session->State = State;
    Session->DesktopWidth = 1024;
    Session->DesktopHeight = 768;
    Session->ColorDepth = 16;
    wcsncpy(Session->WinStationName,
            L"RDP-Tcp",
            ARRAYSIZE(Session->WinStationName) - 1);
    wcsncpy(Session->DesktopName,
            L"Default",
            ARRAYSIZE(Session->DesktopName) - 1);
}

static VOID
SetRdpWinStationName(
    _Out_writes_(NameLength) PWSTR Name,
    _In_ SIZE_T NameLength,
    _In_ INT SessionId)
{
    if (NameLength == 0)
        return;

    _snwprintf(Name, NameLength, L"RDP-Tcp#%d", SessionId);
    Name[NameLength - 1] = UNICODE_NULL;
}

static VOID
SetConsoleSessionBinding(
    _Out_ TERMSRV_SESSION *Session,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName)
{
    Session->SessionId = 0;
    Session->Win32SessionId = 0;
    wcsncpy(Session->UserIdentity, UserIdentity, ARRAYSIZE(Session->UserIdentity) - 1);
    Session->UserIdentity[ARRAYSIZE(Session->UserIdentity) - 1] = UNICODE_NULL;
    wcsncpy(Session->ClientName, ClientName, ARRAYSIZE(Session->ClientName) - 1);
    Session->ClientName[ARRAYSIZE(Session->ClientName) - 1] = UNICODE_NULL;
    wcsncpy(Session->WinStationName, L"WinSta0", ARRAYSIZE(Session->WinStationName) - 1);
    Session->WinStationName[ARRAYSIZE(Session->WinStationName) - 1] = UNICODE_NULL;
    wcsncpy(Session->DesktopName, L"Default", ARRAYSIZE(Session->DesktopName) - 1);
    Session->DesktopName[ARRAYSIZE(Session->DesktopName) - 1] = UNICODE_NULL;
    Session->State = TermSrvSessionConnected;
    Session->Win32Attached = TRUE;
    Session->FramePending = TRUE;
}

static VOID
PrepareConsoleSessionRecord(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager)
{
    memset(Manager->Sessions, 0, sizeof(Manager->Sessions));
    Manager->SessionCount = 1;
    InitializeSessionRecord(&Manager->Sessions[0], 0, TermSrvSessionIdle);
    wcsncpy(Manager->Sessions[0].WinStationName,
            L"WinSta0",
            ARRAYSIZE(Manager->Sessions[0].WinStationName) - 1);
    wcsncpy(Manager->Sessions[0].DesktopName,
            L"Default",
            ARRAYSIZE(Manager->Sessions[0].DesktopName) - 1);
}

static BOOL
DummyCreateOrAttach(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId);

static BOOL
DummyDisconnect(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId);

static BOOL
DummyLogoff(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId);

static BOOL
DummyCaptureFrame(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _Out_ TERMSRV_SESSION_FRAME *Frame);

static BOOL
DummyInjectMouse(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ ULONG PointerFlags,
    _In_ BOOL HasPointer,
    _In_ INT PointerX,
    _In_ INT PointerY);

static BOOL
DummyInjectKeyboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ UINT VirtualKey,
    _In_ UINT KeyboardFlags,
    _In_ BOOL KeyDown);

static BOOL
DummyClipboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_reads_bytes_opt_(InputLength) const VOID *Input,
    _In_ SIZE_T InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *BytesWritten) VOID *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten);

static BOOL
ConsoleCreateOrAttach(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId);

static BOOL
ConsoleDisconnect(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId);

static BOOL
ConsoleLogoff(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId);

static BOOL
ConsoleCaptureFrame(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _Out_ TERMSRV_SESSION_FRAME *Frame);

static BOOL
ConsoleInjectMouse(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ ULONG PointerFlags,
    _In_ BOOL HasPointer,
    _In_ INT PointerX,
    _In_ INT PointerY);

static BOOL
ConsoleInjectKeyboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ UINT VirtualKey,
    _In_ UINT KeyboardFlags,
    _In_ BOOL KeyDown);

static BOOL
ConsoleClipboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_reads_bytes_opt_(InputLength) const VOID *Input,
    _In_ SIZE_T InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *BytesWritten) VOID *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten);

static BOOL
SessmanCreateOrAttach(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId);

static BOOL
SessmanDisconnect(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId);

static BOOL
SessmanLogoff(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId);

static const TERMSRV_SESSION_BACKEND DummySessionBackend =
{
    "dummy",
    DummyCreateOrAttach,
    DummyDisconnect,
    DummyLogoff,
    DummyCaptureFrame,
    DummyInjectMouse,
    DummyInjectKeyboard,
    DummyClipboard
};

static const TERMSRV_SESSION_BACKEND ConsoleSessionBackend =
{
    "console",
    ConsoleCreateOrAttach,
    ConsoleDisconnect,
    ConsoleLogoff,
    ConsoleCaptureFrame,
    ConsoleInjectMouse,
    ConsoleInjectKeyboard,
    ConsoleClipboard
};

static const TERMSRV_SESSION_BACKEND SessmanSessionBackend =
{
    "sessman",
    SessmanCreateOrAttach,
    SessmanDisconnect,
    SessmanLogoff,
    ConsoleCaptureFrame,
    ConsoleInjectMouse,
    ConsoleInjectKeyboard,
    DummyClipboard
};

TERMSRV_SESSION *
TermSrvSessionManagerFindSession(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    if (Manager == NULL)
        return NULL;

    return FindSessionInternal(Manager, SessionId);
}

static INT
SelectSession(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager)
{
    INT i;

    RecordCall(Manager, "select_session");

    if (TermSrvSessionManagerGetBackend(Manager) == &ConsoleSessionBackend)
    {
        if (FindSessionInternal(Manager, 0) == NULL)
            PrepareConsoleSessionRecord(Manager);

        return 0;
    }

    for (i = 0; i < Manager->SessionCount; i++)
    {
        if (Manager->Sessions[i].State == TermSrvSessionIdle)
            return Manager->Sessions[i].SessionId;
    }

    if (Manager->SessionCount < ARRAYSIZE(Manager->Sessions))
    {
        TERMSRV_SESSION *Session = &Manager->Sessions[Manager->SessionCount];
        INT SessionId = Manager->SessionCount + 1;

        InitializeSessionRecord(Session, SessionId, TermSrvSessionIdle);
        Manager->SessionCount++;
        return SessionId;
    }

    return -1;
}

static INT
Authenticate(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_z_ PCWSTR UserIdentity,
    _In_ INT SelectedSessionId)
{
    INT i;

    RecordCall(Manager, "authenticate");

    if (Manager->RejectAuthentication)
        return -1;

    for (i = 0; i < Manager->SessionCount; i++)
    {
        TERMSRV_SESSION *Session = &Manager->Sessions[i];
        if (Session->State == TermSrvSessionDisconnected &&
            wcscmp(Session->UserIdentity, UserIdentity) == 0)
        {
            return Session->SessionId;
        }
    }

    return SelectedSessionId;
}

static TERMSRV_RDP_STATUS
AttachConnection(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _Out_ INT *AttachedSessionId)
{
    TERMSRV_SESSION *Session;
    const TERMSRV_SESSION_BACKEND *Backend;

    RecordCall(Manager, "attach_connection");

    if (AttachedSessionId == NULL || Manager->RejectAttach)
        return TermSrvRdpAttachFailed;

    Session = FindSessionInternal(Manager, SessionId);
    if (Session == NULL)
        return TermSrvRdpAttachFailed;

    Backend = TermSrvSessionManagerGetBackend(Manager);
    if (Backend->CreateOrAttach == NULL ||
        !Backend->CreateOrAttach(Manager,
                                 SessionId,
                                 UserIdentity,
                                 L"RDP client",
                                 AttachedSessionId))
    {
        return TermSrvRdpAttachFailed;
    }

    return TermSrvRdpSuccess;
}

VOID
TermSrvSessionManagerInit(
    _Out_ TERMSRV_SESSION_MANAGER *Manager)
{
    memset(Manager, 0, sizeof(*Manager));
    Manager->Backend = TermSrvSessionManagerGetDefaultBackend();
    Manager->LastSessmanStatus = STATUS_SUCCESS;
    Manager->SessionCount = 1;
    InitializeSessionRecord(&Manager->Sessions[0], 1, TermSrvSessionIdle);
    Manager->LastInputSessionId = -1;
    Manager->LastChannelSessionId = -1;
}

const TERMSRV_SESSION_BACKEND *
TermSrvSessionManagerGetDefaultBackend(VOID)
{
    return &SessmanSessionBackend;
}

const TERMSRV_SESSION_BACKEND *
TermSrvSessionManagerGetConsoleBackend(VOID)
{
    return &ConsoleSessionBackend;
}

const TERMSRV_SESSION_BACKEND *
TermSrvSessionManagerGetSessmanBackend(VOID)
{
    return &SessmanSessionBackend;
}

BOOL
TermSrvSessionManagerSelectBackendByName(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_opt_z_ PCWSTR BackendName)
{
    if (Manager == NULL)
        return FALSE;

    if (BackendName != NULL && BackendName[0] != UNICODE_NULL)
    {
        if (_wcsicmp(BackendName, L"console") == 0)
        {
            TermSrvSessionManagerSetBackend(Manager, &ConsoleSessionBackend, NULL);
            PrepareConsoleSessionRecord(Manager);
            return TRUE;
        }

        if (_wcsicmp(BackendName, L"sessman") == 0)
        {
            TermSrvSessionManagerSetBackend(Manager, &SessmanSessionBackend, NULL);
            return TRUE;
        }

        if (_wcsicmp(BackendName, L"dummy") == 0)
        {
            TermSrvSessionManagerSetBackend(Manager, &DummySessionBackend, NULL);
            return TRUE;
        }

        TermSrvSessionManagerSetBackend(Manager, NULL, NULL);
        return FALSE;
    }

    TermSrvSessionManagerSetBackend(Manager, NULL, NULL);
    return TRUE;
}

VOID
TermSrvSessionManagerSetBackend(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_opt_ const TERMSRV_SESSION_BACKEND *Backend,
    _In_opt_ PVOID Context)
{
    if (Manager == NULL)
        return;

    Manager->Backend = (Backend != NULL) ? Backend : TermSrvSessionManagerGetDefaultBackend();
    Manager->BackendContext = Context;
}

const TERMSRV_SESSION_BACKEND *
TermSrvSessionManagerGetBackend(
    _In_ const TERMSRV_SESSION_MANAGER *Manager)
{
    if (Manager == NULL || Manager->Backend == NULL)
        return TermSrvSessionManagerGetDefaultBackend();

    return Manager->Backend;
}

VOID
TermSrvSessionManagerAddDisconnected(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity)
{
    TERMSRV_SESSION *Session;

    if (Manager->SessionCount >= ARRAYSIZE(Manager->Sessions))
        return;

    Session = &Manager->Sessions[Manager->SessionCount++];
    InitializeSessionRecord(Session, SessionId, TermSrvSessionDisconnected);
    wcsncpy(Session->UserIdentity, UserIdentity, ARRAYSIZE(Session->UserIdentity) - 1);
    Session->UserIdentity[ARRAYSIZE(Session->UserIdentity) - 1] = UNICODE_NULL;
}

BOOL
TermSrvSessionManagerAttachWin32Session(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName)
{
    TERMSRV_SESSION *Session;

    if (Manager == NULL || UserIdentity == NULL || ClientName == NULL)
        return FALSE;

    Session = FindSessionInternal(Manager, SessionId);
    if (Session == NULL)
        return FALSE;

    wcsncpy(Session->UserIdentity, UserIdentity, ARRAYSIZE(Session->UserIdentity) - 1);
    Session->UserIdentity[ARRAYSIZE(Session->UserIdentity) - 1] = UNICODE_NULL;
    wcsncpy(Session->ClientName, ClientName, ARRAYSIZE(Session->ClientName) - 1);
    Session->ClientName[ARRAYSIZE(Session->ClientName) - 1] = UNICODE_NULL;
    SetRdpWinStationName(Session->WinStationName,
                         ARRAYSIZE(Session->WinStationName),
                         SessionId);
    wcsncpy(Session->DesktopName, L"Default", ARRAYSIZE(Session->DesktopName) - 1);
    Session->DesktopName[ARRAYSIZE(Session->DesktopName) - 1] = UNICODE_NULL;
    Session->State = TermSrvSessionConnected;
    Session->Win32Attached = TRUE;
    Session->FramePending = TRUE;
    return TRUE;
}

BOOL
TermSrvSessionManagerDetachWin32Session(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ TERMSRV_SESSION_STATE NewState)
{
    TERMSRV_SESSION *Session;

    if (Manager == NULL)
        return FALSE;

    Session = FindSessionInternal(Manager, SessionId);
    if (Session == NULL)
        return FALSE;

    Session->State = NewState;
    Session->Win32Attached = FALSE;
    Session->FramePending = FALSE;
    return TRUE;
}

BOOL
TermSrvSessionManagerRecordWin32Input(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ BOOL HasPointer,
    _In_ INT PointerX,
    _In_ INT PointerY)
{
    TERMSRV_SESSION *Session;

    if (Manager == NULL)
        return FALSE;

    RecordCall(Manager, "deliver_input");
    Manager->LastInputSessionId = SessionId;

    Session = FindSessionInternal(Manager, SessionId);
    if (Session == NULL || Session->State != TermSrvSessionConnected)
        return FALSE;

    if (HasPointer)
    {
        Session->HasPointer = TRUE;
        Session->LastPointerX = PointerX;
        Session->LastPointerY = PointerY;
    }

    Session->FramePending = TRUE;
    return TRUE;
}

BOOL
TermSrvSessionManagerCaptureWin32Frame(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _Out_ TERMSRV_SESSION_FRAME *Frame)
{
    TERMSRV_SESSION *Session;

    if (Manager == NULL || Frame == NULL)
        return FALSE;

    Session = FindSessionInternal(Manager, SessionId);
    if (Session == NULL || Session->State != TermSrvSessionConnected)
        return FALSE;

    memset(Frame, 0, sizeof(*Frame));
    Frame->SessionId = Session->SessionId;
    Frame->DesktopWidth = Session->DesktopWidth;
    Frame->DesktopHeight = Session->DesktopHeight;
    Frame->ColorDepth = Session->ColorDepth;
    Frame->HasPointer = Session->HasPointer;
    Frame->PointerX = Session->LastPointerX;
    Frame->PointerY = Session->LastPointerY;
    Frame->FramePending = Session->FramePending;
    Session->FramePending = FALSE;
    return TRUE;
}

static BOOL
DummyCreateOrAttach(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId)
{
    if (AttachedSessionId == NULL)
        return FALSE;

    if (!TermSrvSessionManagerAttachWin32Session(Manager,
                                                 SessionId,
                                                 UserIdentity,
                                                 ClientName))
    {
        return FALSE;
    }

    *AttachedSessionId = SessionId;
    return TRUE;
}

static BOOL
DummyDisconnect(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    return TermSrvSessionManagerDetachWin32Session(Manager,
                                                  SessionId,
                                                  TermSrvSessionDisconnected);
}

static BOOL
DummyLogoff(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    return TermSrvSessionManagerDetachWin32Session(Manager,
                                                  SessionId,
                                                  TermSrvSessionLoggedOff);
}

static BOOL
DummyCaptureFrame(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _Out_ TERMSRV_SESSION_FRAME *Frame)
{
    return TermSrvSessionManagerCaptureWin32Frame(Manager, SessionId, Frame);
}

static BOOL
DummyInjectMouse(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ ULONG PointerFlags,
    _In_ BOOL HasPointer,
    _In_ INT PointerX,
    _In_ INT PointerY)
{
    UNREFERENCED_PARAMETER(PointerFlags);

    return TermSrvSessionManagerRecordWin32Input(Manager,
                                                SessionId,
                                                HasPointer,
                                                PointerX,
                                                PointerY);
}

static BOOL
DummyInjectKeyboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ UINT VirtualKey,
    _In_ UINT KeyboardFlags,
    _In_ BOOL KeyDown)
{
    UNREFERENCED_PARAMETER(VirtualKey);
    UNREFERENCED_PARAMETER(KeyboardFlags);
    UNREFERENCED_PARAMETER(KeyDown);
    return TermSrvSessionManagerRecordWin32Input(Manager, SessionId, FALSE, 0, 0);
}

static BOOL
DummyClipboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_reads_bytes_opt_(InputLength) const VOID *Input,
    _In_ SIZE_T InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *BytesWritten) VOID *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten)
{
    UNREFERENCED_PARAMETER(Manager);
    UNREFERENCED_PARAMETER(SessionId);
    UNREFERENCED_PARAMETER(Input);
    UNREFERENCED_PARAMETER(InputLength);
    UNREFERENCED_PARAMETER(Output);
    UNREFERENCED_PARAMETER(OutputLength);

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    return TRUE;
}

static BOOL
SessmanConnect(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager)
{
    HANDLE SmApiPort = NULL;
    NTSTATUS Status;

    if (Manager == NULL)
        return FALSE;

    if (Manager->SessmanConnected)
        return TRUE;

    if (Manager->SessmanUnavailable)
        return FALSE;

    Status = SmConnectToSm(NULL, NULL, IMAGE_SUBSYSTEM_UNKNOWN, &SmApiPort);
    Manager->LastSessmanStatus = Status;
    if (!NT_SUCCESS(Status))
    {
        Manager->SessmanUnavailable = TRUE;
        return FALSE;
    }

    Manager->SmApiPort = SmApiPort;
    Manager->SessmanConnected = TRUE;
    return TRUE;
}

static BOOL
SessmanFallbackAttach(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId)
{
    Manager->SessmanFallback = TRUE;
    if (Manager->LastSessmanStatus == STATUS_SUCCESS)
        Manager->LastSessmanStatus = STATUS_OBJECT_NAME_NOT_FOUND;

    return DummyCreateOrAttach(Manager,
                               SessionId,
                               UserIdentity,
                               ClientName,
                               AttachedSessionId);
}

static BOOL
SessmanAttachExisting(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _Inout_ TERMSRV_SESSION *Session,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId)
{
    if (!TermSrvSessionManagerAttachWin32Session(Manager,
                                                 Session->SessionId,
                                                 UserIdentity,
                                                 ClientName))
    {
        return FALSE;
    }

    *AttachedSessionId = Session->SessionId;
    return TRUE;
}

static BOOL
SessmanCreateOrAttach(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId)
{
    TERMSRV_SESSION *Session;
    ULONG MuSessionId;
    HANDLE WindowsSubSysProcessId = NULL;
    HANDLE InitialCommandProcessId = NULL;
    NTSTATUS Status;

    if (Manager == NULL || UserIdentity == NULL || ClientName == NULL || AttachedSessionId == NULL)
        return FALSE;

    Session = FindSessionInternal(Manager, SessionId);
    if (Session == NULL)
        return FALSE;

    if (Session->SessmanStarted)
        return SessmanAttachExisting(Manager, Session, UserIdentity, ClientName, AttachedSessionId);

    if (!SessmanConnect(Manager))
        return SessmanFallbackAttach(Manager, SessionId, UserIdentity, ClientName, AttachedSessionId);

    MuSessionId = (ULONG)SessionId;
    Status = SmStartCsr((HANDLE)Manager->SmApiPort,
                        &MuSessionId,
                        NULL,
                        &WindowsSubSysProcessId,
                        &InitialCommandProcessId);
    Manager->LastSessmanStatus = Status;
    if (!NT_SUCCESS(Status))
        return SessmanFallbackAttach(Manager, SessionId, UserIdentity, ClientName, AttachedSessionId);

    if ((INT)MuSessionId != SessionId)
    {
        TERMSRV_SESSION *ExistingSession = FindSessionInternal(Manager, (INT)MuSessionId);
        if (ExistingSession != NULL)
        {
            Session = ExistingSession;
        }
        else
        {
            Session->SessionId = (INT)MuSessionId;
        }
    }

    Session->Win32SessionId = MuSessionId;
    Session->SessmanStarted = TRUE;
    Session->WindowsSubSysProcessId = WindowsSubSysProcessId;
    Session->InitialCommandProcessId = InitialCommandProcessId;

    if (!SessmanAttachExisting(Manager, Session, UserIdentity, ClientName, AttachedSessionId))
        return FALSE;

    Manager->SessmanFallback = FALSE;
    return TRUE;
}

static BOOL
SessmanDisconnect(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    return TermSrvSessionManagerDetachWin32Session(Manager,
                                                  SessionId,
                                                  TermSrvSessionDisconnected);
}

static BOOL
SessmanLogoff(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    TERMSRV_SESSION *Session;
    NTSTATUS Status;

    if (Manager == NULL)
        return FALSE;

    Session = FindSessionInternal(Manager, SessionId);
    if (Session != NULL &&
        Session->SessmanStarted &&
        Manager->SessmanConnected &&
        Manager->SmApiPort != NULL)
    {
        Status = SmStopCsr((HANDLE)Manager->SmApiPort, Session->Win32SessionId);
        Manager->LastSessmanStatus = Status;
        if (NT_SUCCESS(Status))
            Session->SessmanStarted = FALSE;
    }

    return TermSrvSessionManagerDetachWin32Session(Manager,
                                                  SessionId,
                                                  TermSrvSessionLoggedOff);
}

static BOOL
ConsoleCreateOrAttach(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId)
{
    TERMSRV_SESSION *Session;

    UNREFERENCED_PARAMETER(SessionId);

    if (Manager == NULL || UserIdentity == NULL || ClientName == NULL || AttachedSessionId == NULL)
        return FALSE;

    Session = FindSessionInternal(Manager, 0);
    if (Session == NULL)
    {
        PrepareConsoleSessionRecord(Manager);
        Session = FindSessionInternal(Manager, 0);
        if (Session == NULL)
            return FALSE;
    }

    SetConsoleSessionBinding(Session, UserIdentity, ClientName);
    *AttachedSessionId = 0;
    return TRUE;
}

static BOOL
ConsoleDisconnect(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    UNREFERENCED_PARAMETER(SessionId);

    return TermSrvSessionManagerDetachWin32Session(Manager,
                                                  0,
                                                  TermSrvSessionDisconnected);
}

static BOOL
ConsoleLogoff(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    UNREFERENCED_PARAMETER(SessionId);

    return TermSrvSessionManagerDetachWin32Session(Manager,
                                                  0,
                                                  TermSrvSessionLoggedOff);
}

static BOOL
ConsoleCaptureFrame(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _Out_ TERMSRV_SESSION_FRAME *Frame)
{
    UNREFERENCED_PARAMETER(SessionId);

    return TermSrvSessionManagerCaptureWin32Frame(Manager, 0, Frame);
}

static BOOL
ConsoleTryInjectMouseThroughWin32u(
    _In_ ULONG SessionId,
    _In_ ULONG PointerFlags,
    _In_ INT PointerX,
    _In_ INT PointerY)
{
    HMODULE Win32u;
    FARPROC Proc;
    TERMSRV_RDP_MOUSE_INPUT Input;
    BOOL (WINAPI *NtUserRdpInjectMouse)(_In_ ULONG, _In_ TERMSRV_RDP_MOUSE_INPUT *);
    BOOL Ret;

    Win32u = LoadLibraryW(L"win32u.dll");
    if (Win32u == NULL)
        return FALSE;

    Proc = GetProcAddress(Win32u, "NtUserRdpInjectMouse");
    if (Proc == NULL)
    {
        FreeLibrary(Win32u);
        return FALSE;
    }

    ZeroMemory(&Input, sizeof(Input));
    Input.Size = sizeof(Input);
    Input.SessionId = SessionId;
    Input.PointerFlags = PointerFlags | TERMSRV_RDP_MOUSE_FLAG_MOVE;
    Input.PointerX = (USHORT)PointerX;
    Input.PointerY = (USHORT)PointerY;

    NtUserRdpInjectMouse = (BOOL (WINAPI *)(ULONG, TERMSRV_RDP_MOUSE_INPUT *))Proc;
    Ret = NtUserRdpInjectMouse(SessionId, &Input);
    FreeLibrary(Win32u);
    return Ret;
}

static BOOL
ConsoleInjectMouse(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ ULONG PointerFlags,
    _In_ BOOL HasPointer,
    _In_ INT PointerX,
    _In_ INT PointerY)
{
    UNREFERENCED_PARAMETER(SessionId);

    ConsoleTryInjectMouseThroughWin32u(0, PointerFlags, PointerX, PointerY);

    return TermSrvSessionManagerRecordWin32Input(Manager,
                                                0,
                                                HasPointer,
                                                PointerX,
                                                PointerY);
}

static BOOL
ConsoleTryInjectKeyboardThroughWin32u(
    _In_ ULONG SessionId,
    _In_ UINT ScanCode,
    _In_ UINT KeyboardFlags)
{
    HMODULE Win32u;
    FARPROC Proc;
    TERMSRV_RDP_KEYBOARD_INPUT Input;
    BOOL (WINAPI *NtUserRdpInjectKeyboard)(_In_ ULONG, _In_ TERMSRV_RDP_KEYBOARD_INPUT *);
    BOOL Ret;

    Win32u = LoadLibraryW(L"win32u.dll");
    if (Win32u == NULL)
        return FALSE;

    Proc = GetProcAddress(Win32u, "NtUserRdpInjectKeyboard");
    if (Proc == NULL)
    {
        FreeLibrary(Win32u);
        return FALSE;
    }

    ZeroMemory(&Input, sizeof(Input));
    Input.Size = sizeof(Input);
    Input.SessionId = SessionId;
    Input.KeyboardFlags = (USHORT)KeyboardFlags;
    Input.KeyCode = (USHORT)ScanCode;

    NtUserRdpInjectKeyboard = (BOOL (WINAPI *)(ULONG, TERMSRV_RDP_KEYBOARD_INPUT *))Proc;
    Ret = NtUserRdpInjectKeyboard(SessionId, &Input);
    FreeLibrary(Win32u);
    return Ret;
}

static BOOL
ConsoleInjectKeyboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ UINT VirtualKey,
    _In_ UINT KeyboardFlags,
    _In_ BOOL KeyDown)
{
    UNREFERENCED_PARAMETER(SessionId);
    UNREFERENCED_PARAMETER(KeyDown);

    ConsoleTryInjectKeyboardThroughWin32u(0,
                                          VirtualKey,
                                          KeyboardFlags);

    return TermSrvSessionManagerRecordWin32Input(Manager, 0, FALSE, 0, 0);
}

static BOOL
ConsoleClipboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_reads_bytes_opt_(InputLength) const VOID *Input,
    _In_ SIZE_T InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *BytesWritten) VOID *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten)
{
    UNREFERENCED_PARAMETER(Manager);
    UNREFERENCED_PARAMETER(SessionId);
    UNREFERENCED_PARAMETER(Input);
    UNREFERENCED_PARAMETER(InputLength);
    UNREFERENCED_PARAMETER(Output);
    UNREFERENCED_PARAMETER(OutputLength);

    if (BytesWritten == NULL)
        return FALSE;

    *BytesWritten = 0;
    return TRUE;
}

VOID
TermSrvRdpPeerInit(
    _Out_ TERMSRV_RDP_PEER *Peer,
    _Inout_ TERMSRV_SESSION_MANAGER *Manager)
{
    memset(Peer, 0, sizeof(*Peer));
    Peer->State = TermSrvRdpStateIdle;
    Peer->SessionManager = Manager;
    Peer->SessionId = -1;
    Peer->UserChannelId = 1001;
    Peer->IoChannelId = 1003;
    Peer->DesktopWidth = 1024;
    Peer->DesktopHeight = 768;
    Peer->ColorDepth = 16;
}

TERMSRV_RDP_STATUS
TermSrvRdpPeerReceive(
    _Inout_ TERMSRV_RDP_PEER *Peer,
    _In_z_ PCSTR Packet)
{
    TERMSRV_SESSION_MANAGER *Manager = Peer->SessionManager;

    if (PacketStartsWith(Packet, "X224"))
    {
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateIdle);
        if (Status != TermSrvRdpSuccess)
            return Status;

        RecordCall(Manager, "preflight_connection");
        Peer->State = TermSrvRdpStateX224Connected;
        SetOutput(Peer, "X224_CONFIRM");
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "MCS"))
    {
        CHAR Channels[128] = "";
        PCHAR Token;
        INT Count = 0;
        PCSTR Found;
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateX224Connected);

        if (Status != TermSrvRdpSuccess)
            return Status;

        Peer->FastPathInput = (strstr(Packet, "fast=1") != NULL);
        Peer->SessionId = SelectSession(Manager);
        if (Peer->SessionId < 0)
            return TermSrvRdpAttachFailed;

        Found = strstr(Packet, "channels=");
        if (Found != NULL)
        {
            SIZE_T i = 0;
            Found += strlen("channels=");
            while (Found[i] != ANSI_NULL && Found[i] != ';' && i + 1 < sizeof(Channels))
            {
                Channels[i] = Found[i];
                i++;
            }
            Channels[i] = ANSI_NULL;
        }

        Token = strtok(Channels, ",");
        while (Token != NULL && Token[0] != ANSI_NULL)
        {
            INT i;

            if (Count >= TERMSRV_RDP_MAX_CHANNELS)
                return TermSrvRdpTooManyChannels;

            for (i = 0; i < Count; i++)
            {
                if (strncmp(Peer->ChannelNames[i], Token, sizeof(Peer->ChannelNames[i])) == 0)
                    return TermSrvRdpDuplicateChannel;
            }

            Peer->ChannelIds[Count] = 1004 + Count;
            strncpy(Peer->ChannelNames[Count], Token, sizeof(Peer->ChannelNames[Count]) - 1);
            Peer->ChannelNames[Count][sizeof(Peer->ChannelNames[Count]) - 1] = ANSI_NULL;
            Count++;
            Token = strtok(NULL, ",");
        }

        Peer->ChannelCount = Count;
        Peer->State = TermSrvRdpStateMcsConnected;
        SetOutput(Peer, "MCS_RESPONSE");
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "ERECT"))
    {
        return RequireState(Peer, TermSrvRdpStateMcsConnected);
    }

    if (PacketStartsWith(Packet, "ATTACH"))
    {
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateMcsConnected);
        if (Status != TermSrvRdpSuccess)
            return Status;

        Peer->State = TermSrvRdpStateUserAttached;
        SetOutput(Peer, "ATTACH_CONFIRM user=1001");
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "JOIN"))
    {
        INT ChannelId = ParseIntAfter(Packet, "id=", -1);
        BOOL IsKnown = (ChannelId == Peer->UserChannelId) ||
                       (ChannelId == Peer->IoChannelId) ||
                       (StaticChannelIndexById(Peer, ChannelId) >= 0);

        if (Peer->State != TermSrvRdpStateUserAttached &&
            Peer->State != TermSrvRdpStateChannelsJoined)
        {
            return TermSrvRdpOutOfOrder;
        }

        if (!IsKnown)
            return TermSrvRdpUnknownChannel;

        if (!ChannelIsJoined(Peer, ChannelId) &&
            Peer->JoinedCount < ARRAYSIZE(Peer->JoinedChannels))
        {
            Peer->JoinedChannels[Peer->JoinedCount++] = ChannelId;
        }

        if (ChannelIsJoined(Peer, Peer->UserChannelId) &&
            ChannelIsJoined(Peer, Peer->IoChannelId))
        {
            INT i;
            BOOL AllStaticJoined = TRUE;

            for (i = 0; i < Peer->ChannelCount; i++)
            {
                if (!ChannelIsJoined(Peer, Peer->ChannelIds[i]))
                {
                    AllStaticJoined = FALSE;
                    break;
                }
            }

            if (AllStaticJoined)
                Peer->State = TermSrvRdpStateChannelsJoined;
        }

        SetOutput(Peer, "JOIN_CONFIRM");
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "SECURITY"))
    {
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateChannelsJoined);
        if (Status != TermSrvRdpSuccess)
            return Status;

        Peer->State = TermSrvRdpStateSecurityComplete;
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "CLIENT_INFO"))
    {
        WCHAR UserIdentity[64];
        INT SessionId;
        INT AttachedSessionId;
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateSecurityComplete);

        if (Status != TermSrvRdpSuccess)
            return Status;

        ParseUserIdentity(Packet, UserIdentity, ARRAYSIZE(UserIdentity));
        SessionId = Authenticate(Manager, UserIdentity, Peer->SessionId);
        if (SessionId < 0)
            return TermSrvRdpAuthenticationFailed;

        Peer->SessionId = SessionId;
        Status = AttachConnection(Manager, SessionId, UserIdentity, &AttachedSessionId);
        if (Status != TermSrvRdpSuccess)
            return Status;

        Peer->SessionId = AttachedSessionId;
        Peer->State = TermSrvRdpStateCapabilitiesDemanded;
        SetOutput(Peer, "DEMAND_ACTIVE");
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "CONFIRM_ACTIVE"))
    {
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateCapabilitiesDemanded);
        if (Status != TermSrvRdpSuccess)
            return Status;

        Peer->State = TermSrvRdpStateActive;
        SetOutput(Peer, "FONT_MAP");
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "SLOW_INPUT"))
    {
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateActive);
        const TERMSRV_SESSION_BACKEND *Backend;
        BOOL HasMouse;
        if (Status != TermSrvRdpSuccess)
            return Status;

        Backend = TermSrvSessionManagerGetBackend(Manager);
        HasMouse = (strstr(Packet, "mouse=1") != NULL);
        if (HasMouse)
        {
            ULONG PointerFlags;

            PointerFlags = (ULONG)ParseIntAfter(Packet,
                                                "flags=",
                                                TERMSRV_RDP_MOUSE_FLAG_MOVE);
            if (Backend->InjectMouse != NULL)
            {
                Backend->InjectMouse(Manager,
                                     Peer->SessionId,
                                     PointerFlags,
                                     TRUE,
                                     ParseIntAfter(Packet, "x=", 0),
                                     ParseIntAfter(Packet, "y=", 0));
            }
        }
        else if (Backend->InjectKeyboard != NULL)
        {
            UINT KeyboardFlags;

            KeyboardFlags = (UINT)ParseIntAfter(Packet, "flags=", 0);
            Backend->InjectKeyboard(Manager,
                                    Peer->SessionId,
                                    (UINT)ParseIntAfter(Packet, "scancode=", 0),
                                    KeyboardFlags,
                                    (KeyboardFlags & TERMSRV_RDP_KEYBOARD_FLAG_RELEASE) == 0);
        }
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "FAST_INPUT"))
    {
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateActive);
        const TERMSRV_SESSION_BACKEND *Backend;
        BOOL HasMouse;
        if (Status != TermSrvRdpSuccess)
            return Status;

        if (!Peer->FastPathInput)
            return TermSrvRdpFastPathNotNegotiated;

        Backend = TermSrvSessionManagerGetBackend(Manager);
        HasMouse = (strstr(Packet, "mouse=1") != NULL);
        if (HasMouse)
        {
            ULONG PointerFlags;

            PointerFlags = (ULONG)ParseIntAfter(Packet,
                                                "flags=",
                                                TERMSRV_RDP_MOUSE_FLAG_MOVE);
            if (Backend->InjectMouse != NULL)
            {
                Backend->InjectMouse(Manager,
                                     Peer->SessionId,
                                     PointerFlags,
                                     TRUE,
                                     ParseIntAfter(Packet, "x=", 0),
                                     ParseIntAfter(Packet, "y=", 0));
            }
        }
        else if (Backend->InjectKeyboard != NULL)
        {
            UINT KeyboardFlags;
            UINT FastPathKeyboardFlags;

            FastPathKeyboardFlags = (UINT)ParseIntAfter(Packet, "flags=", 0);
            KeyboardFlags = 0;
            if (FastPathKeyboardFlags & TERMSRV_FASTPATH_KEYBOARD_FLAG_RELEASE)
                KeyboardFlags |= TERMSRV_RDP_KEYBOARD_FLAG_RELEASE;
            if (FastPathKeyboardFlags & TERMSRV_FASTPATH_KEYBOARD_FLAG_EXTENDED)
                KeyboardFlags |= TERMSRV_RDP_KEYBOARD_FLAG_EXTENDED;
            Backend->InjectKeyboard(Manager,
                                    Peer->SessionId,
                                    (UINT)ParseIntAfter(Packet, "scancode=", 0),
                                    KeyboardFlags,
                                    (KeyboardFlags & TERMSRV_RDP_KEYBOARD_FLAG_RELEASE) == 0);
        }
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "VC"))
    {
        INT ChannelId = ParseIntAfter(Packet, "id=", -1);
        INT Index;
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateActive);

        if (Status != TermSrvRdpSuccess)
            return Status;

        Index = StaticChannelIndexById(Peer, ChannelId);
        if (Index < 0 || !ChannelIsJoined(Peer, ChannelId))
            return TermSrvRdpUnknownChannel;

        RecordCall(Manager, "deliver_static_channel_data");
        Manager->LastChannelSessionId = Peer->SessionId;
        strncpy(Manager->LastChannelName, Peer->ChannelNames[Index], sizeof(Manager->LastChannelName) - 1);
        Manager->LastChannelName[sizeof(Manager->LastChannelName) - 1] = ANSI_NULL;
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "DISCONNECT"))
    {
        const TERMSRV_SESSION_BACKEND *Backend = TermSrvSessionManagerGetBackend(Manager);

        RecordCall(Manager, "disconnect");
        if (Backend->Disconnect != NULL)
            Backend->Disconnect(Manager, Peer->SessionId);

        Peer->State = TermSrvRdpStateClosed;
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "LOGOFF"))
    {
        const TERMSRV_SESSION_BACKEND *Backend = TermSrvSessionManagerGetBackend(Manager);

        RecordCall(Manager, "logoff");
        if (Backend->Logoff != NULL)
            Backend->Logoff(Manager, Peer->SessionId);

        Peer->State = TermSrvRdpStateClosed;
        return TermSrvRdpSuccess;
    }

    return TermSrvRdpInvalidPacket;
}

TERMSRV_RDP_STATUS
TermSrvRdpPeerSendBitmapUpdate(
    _Inout_ TERMSRV_RDP_PEER *Peer,
    _In_ INT Width,
    _In_ INT Height)
{
    if (Peer->State != TermSrvRdpStateActive)
        return TermSrvRdpGraphicsBeforeActive;

    if (Width > Peer->DesktopWidth || Height > Peer->DesktopHeight)
        return TermSrvRdpBitmapOutOfBounds;

    if (Peer->SessionManager != NULL)
    {
        TERMSRV_SESSION_FRAME Frame;
        const TERMSRV_SESSION_BACKEND *Backend = TermSrvSessionManagerGetBackend(Peer->SessionManager);

        if (Backend->CaptureFrame == NULL ||
            !Backend->CaptureFrame(Peer->SessionManager,
                                   Peer->SessionId,
                                   &Frame))
        {
            return TermSrvRdpGraphicsBeforeActive;
        }
    }

    SetOutput(Peer, "GRAPHICS_UPDATE");
    return TermSrvRdpSuccess;
}

PCSTR
TermSrvRdpStateName(
    _In_ TERMSRV_RDP_STATE State)
{
    switch (State)
    {
        case TermSrvRdpStateIdle: return "Idle";
        case TermSrvRdpStateX224Connected: return "X224Connected";
        case TermSrvRdpStateMcsConnected: return "McsConnected";
        case TermSrvRdpStateUserAttached: return "UserAttached";
        case TermSrvRdpStateChannelsJoined: return "ChannelsJoined";
        case TermSrvRdpStateSecurityComplete: return "SecurityComplete";
        case TermSrvRdpStateCapabilitiesDemanded: return "CapabilitiesDemanded";
        case TermSrvRdpStateActive: return "Active";
        case TermSrvRdpStateClosed: return "Closed";
        default: return "Unknown";
    }
}

PCSTR
TermSrvRdpStatusName(
    _In_ TERMSRV_RDP_STATUS Status)
{
    switch (Status)
    {
        case TermSrvRdpSuccess: return "TermSrvRdpSuccess";
        case TermSrvRdpInvalidPacket: return "TermSrvRdpInvalidPacket";
        case TermSrvRdpOutOfOrder: return "TermSrvRdpOutOfOrder";
        case TermSrvRdpTooManyChannels: return "TermSrvRdpTooManyChannels";
        case TermSrvRdpDuplicateChannel: return "TermSrvRdpDuplicateChannel";
        case TermSrvRdpUnknownChannel: return "TermSrvRdpUnknownChannel";
        case TermSrvRdpAuthenticationFailed: return "TermSrvRdpAuthenticationFailed";
        case TermSrvRdpAttachFailed: return "TermSrvRdpAttachFailed";
        case TermSrvRdpFastPathNotNegotiated: return "TermSrvRdpFastPathNotNegotiated";
        case TermSrvRdpGraphicsBeforeActive: return "TermSrvRdpGraphicsBeforeActive";
        case TermSrvRdpBitmapOutOfBounds: return "TermSrvRdpBitmapOutOfBounds";
        default: return "TermSrvRdpUnknownStatus";
    }
}
