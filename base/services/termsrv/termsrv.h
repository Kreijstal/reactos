/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal server-side RDP state engine
 */

#pragma once

#include <windef.h>

#define TERMSRV_SERVICE_NAME_W L"TermService"
#define TERMSRV_RDP_MAX_CHANNELS 31
#define TERMSRV_RDP_MAX_CALLS 64

typedef enum _TERMSRV_RDP_STATE
{
    TermSrvRdpStateIdle = 0,
    TermSrvRdpStateX224Connected,
    TermSrvRdpStateMcsConnected,
    TermSrvRdpStateUserAttached,
    TermSrvRdpStateChannelsJoined,
    TermSrvRdpStateSecurityComplete,
    TermSrvRdpStateCapabilitiesDemanded,
    TermSrvRdpStateActive,
    TermSrvRdpStateClosed
} TERMSRV_RDP_STATE;

typedef enum _TERMSRV_RDP_STATUS
{
    TermSrvRdpSuccess = 0,
    TermSrvRdpInvalidPacket,
    TermSrvRdpOutOfOrder,
    TermSrvRdpTooManyChannels,
    TermSrvRdpDuplicateChannel,
    TermSrvRdpUnknownChannel,
    TermSrvRdpAuthenticationFailed,
    TermSrvRdpAttachFailed,
    TermSrvRdpFastPathNotNegotiated,
    TermSrvRdpGraphicsBeforeActive,
    TermSrvRdpBitmapOutOfBounds
} TERMSRV_RDP_STATUS;

typedef enum _TERMSRV_SESSION_STATE
{
    TermSrvSessionIdle = 0,
    TermSrvSessionConnected,
    TermSrvSessionDisconnected,
    TermSrvSessionLoggedOff
} TERMSRV_SESSION_STATE;

typedef struct _TERMSRV_SESSION
{
    INT SessionId;
    WCHAR UserIdentity[64];
    WCHAR WinStationName[32];
    WCHAR DesktopName[32];
    WCHAR ClientName[64];
    TERMSRV_SESSION_STATE State;
    INT DesktopWidth;
    INT DesktopHeight;
    INT ColorDepth;
    BOOL Win32Attached;
    BOOL FramePending;
    BOOL HasPointer;
    INT LastPointerX;
    INT LastPointerY;
} TERMSRV_SESSION;

typedef struct _TERMSRV_SESSION_FRAME
{
    INT SessionId;
    INT DesktopWidth;
    INT DesktopHeight;
    INT ColorDepth;
    BOOL HasPointer;
    INT PointerX;
    INT PointerY;
    BOOL FramePending;
} TERMSRV_SESSION_FRAME;

typedef struct _TERMSRV_SESSION_MANAGER
{
    TERMSRV_SESSION Sessions[8];
    INT SessionCount;
    BOOL RejectAuthentication;
    BOOL RejectAttach;
    CHAR Calls[TERMSRV_RDP_MAX_CALLS][64];
    INT CallCount;
    INT LastInputSessionId;
    INT LastChannelSessionId;
    CHAR LastChannelName[8];
} TERMSRV_SESSION_MANAGER;

typedef struct _TERMSRV_RDP_PEER
{
    TERMSRV_RDP_STATE State;
    TERMSRV_SESSION_MANAGER *SessionManager;
    INT SessionId;
    INT UserChannelId;
    INT IoChannelId;
    INT ChannelIds[TERMSRV_RDP_MAX_CHANNELS];
    CHAR ChannelNames[TERMSRV_RDP_MAX_CHANNELS][8];
    INT ChannelCount;
    INT JoinedChannels[TERMSRV_RDP_MAX_CHANNELS + 2];
    INT JoinedCount;
    BOOL FastPathInput;
    INT DesktopWidth;
    INT DesktopHeight;
    INT ColorDepth;
    CHAR LastOutput[64];
} TERMSRV_RDP_PEER;

VOID
TermSrvSessionManagerInit(
    _Out_ TERMSRV_SESSION_MANAGER *Manager);

VOID
TermSrvSessionManagerAddDisconnected(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity);

TERMSRV_SESSION *
TermSrvSessionManagerFindSession(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId);

BOOL
TermSrvSessionManagerAttachWin32Session(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName);

BOOL
TermSrvSessionManagerDetachWin32Session(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ TERMSRV_SESSION_STATE NewState);

BOOL
TermSrvSessionManagerRecordWin32Input(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ BOOL HasPointer,
    _In_ INT PointerX,
    _In_ INT PointerY);

BOOL
TermSrvSessionManagerCaptureWin32Frame(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _Out_ TERMSRV_SESSION_FRAME *Frame);

VOID
TermSrvRdpPeerInit(
    _Out_ TERMSRV_RDP_PEER *Peer,
    _Inout_ TERMSRV_SESSION_MANAGER *Manager);

TERMSRV_RDP_STATUS
TermSrvRdpPeerReceive(
    _Inout_ TERMSRV_RDP_PEER *Peer,
    _In_z_ PCSTR Packet);

TERMSRV_RDP_STATUS
TermSrvRdpPeerSendBitmapUpdate(
    _Inout_ TERMSRV_RDP_PEER *Peer,
    _In_ INT Width,
    _In_ INT Height);

PCSTR
TermSrvRdpStateName(
    _In_ TERMSRV_RDP_STATE State);

PCSTR
TermSrvRdpStatusName(
    _In_ TERMSRV_RDP_STATUS Status);
