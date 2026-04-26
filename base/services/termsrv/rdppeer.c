/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal server-side RDP state engine
 *
 * This is deliberately a protocol/session-manager scaffold. It models the
 * classic RDP server phases and the TermService/session-manager boundary, but
 * it does not parse MS-RDPBCGR byte streams yet.
 */

#include "termsrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    _In_z_ PCWSTR UserIdentity)
{
    TERMSRV_SESSION *Session;

    RecordCall(Manager, "attach_connection");

    if (Manager->RejectAttach)
        return TermSrvRdpAttachFailed;

    Session = FindSessionInternal(Manager, SessionId);
    if (Session == NULL)
        return TermSrvRdpAttachFailed;

    if (!TermSrvSessionManagerAttachWin32Session(Manager,
                                                 SessionId,
                                                 UserIdentity,
                                                 L"RDP client"))
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
    Manager->SessionCount = 1;
    InitializeSessionRecord(&Manager->Sessions[0], 1, TermSrvSessionIdle);
    Manager->LastInputSessionId = -1;
    Manager->LastChannelSessionId = -1;
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
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateSecurityComplete);

        if (Status != TermSrvRdpSuccess)
            return Status;

        ParseUserIdentity(Packet, UserIdentity, ARRAYSIZE(UserIdentity));
        SessionId = Authenticate(Manager, UserIdentity, Peer->SessionId);
        if (SessionId < 0)
            return TermSrvRdpAuthenticationFailed;

        Peer->SessionId = SessionId;
        Status = AttachConnection(Manager, SessionId, UserIdentity);
        if (Status != TermSrvRdpSuccess)
            return Status;

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
        if (Status != TermSrvRdpSuccess)
            return Status;

        TermSrvSessionManagerRecordWin32Input(Manager,
                                              Peer->SessionId,
                                              strstr(Packet, "mouse=1") != NULL,
                                              ParseIntAfter(Packet, "x=", 0),
                                              ParseIntAfter(Packet, "y=", 0));
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "FAST_INPUT"))
    {
        TERMSRV_RDP_STATUS Status = RequireState(Peer, TermSrvRdpStateActive);
        if (Status != TermSrvRdpSuccess)
            return Status;

        if (!Peer->FastPathInput)
            return TermSrvRdpFastPathNotNegotiated;

        TermSrvSessionManagerRecordWin32Input(Manager,
                                              Peer->SessionId,
                                              strstr(Packet, "mouse=1") != NULL,
                                              ParseIntAfter(Packet, "x=", 0),
                                              ParseIntAfter(Packet, "y=", 0));
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
        RecordCall(Manager, "disconnect");
        TermSrvSessionManagerDetachWin32Session(Manager,
                                                Peer->SessionId,
                                                TermSrvSessionDisconnected);

        Peer->State = TermSrvRdpStateClosed;
        return TermSrvRdpSuccess;
    }

    if (PacketStartsWith(Packet, "LOGOFF"))
    {
        RecordCall(Manager, "logoff");
        TermSrvSessionManagerDetachWin32Session(Manager,
                                                Peer->SessionId,
                                                TermSrvSessionLoggedOff);

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

        if (!TermSrvSessionManagerCaptureWin32Frame(Peer->SessionManager,
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
