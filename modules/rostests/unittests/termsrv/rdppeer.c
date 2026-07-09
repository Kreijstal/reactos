/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Unit tests for the minimal RDP server-side state engine
 */

#include <apitest.h>
#include "cliprdr.h"
#include "rdpbcgr.h"
#include "termsrv.h"

static VOID
JoinBaseChannels(
    _Inout_ TERMSRV_RDP_PEER *Peer,
    _In_ BOOL FastPathInput)
{
    ok(TermSrvRdpPeerReceive(Peer, "X224 cookie=mstshash=test") == TermSrvRdpSuccess,
       "X224 failed\n");
    if (FastPathInput)
    {
        ok(TermSrvRdpPeerReceive(Peer, "MCS channels=rdpdr;fast=1") == TermSrvRdpSuccess,
           "MCS failed\n");
    }
    else
    {
        ok(TermSrvRdpPeerReceive(Peer, "MCS channels=rdpdr") == TermSrvRdpSuccess,
           "MCS failed\n");
    }

    ok(TermSrvRdpPeerReceive(Peer, "ERECT") == TermSrvRdpSuccess,
       "ERECT failed\n");
    ok(TermSrvRdpPeerReceive(Peer, "ATTACH") == TermSrvRdpSuccess,
       "ATTACH failed\n");
    ok(TermSrvRdpPeerReceive(Peer, "JOIN id=1001") == TermSrvRdpSuccess,
       "JOIN user failed\n");
    ok(TermSrvRdpPeerReceive(Peer, "JOIN id=1003") == TermSrvRdpSuccess,
       "JOIN IO failed\n");
    ok(TermSrvRdpPeerReceive(Peer, "JOIN id=1004") == TermSrvRdpSuccess,
       "JOIN rdpdr failed\n");
    ok(Peer->State == TermSrvRdpStateChannelsJoined,
       "Expected ChannelsJoined, got %s\n", TermSrvRdpStateName(Peer->State));
}

static VOID
ActivatePeerWithFastPath(
    _Inout_ TERMSRV_RDP_PEER *Peer,
    _In_ BOOL FastPathInput)
{
    JoinBaseChannels(Peer, FastPathInput);
    ok(TermSrvRdpPeerReceive(Peer, "SECURITY client_random=test") == TermSrvRdpSuccess,
       "SECURITY failed\n");
    ok(TermSrvRdpPeerReceive(Peer, "CLIENT_INFO user=DOMAIN\\alice") == TermSrvRdpSuccess,
       "CLIENT_INFO failed\n");
    ok(TermSrvRdpPeerReceive(Peer, "CONFIRM_ACTIVE") == TermSrvRdpSuccess,
       "CONFIRM_ACTIVE failed\n");
    ok(Peer->State == TermSrvRdpStateActive,
       "Expected Active, got %s\n", TermSrvRdpStateName(Peer->State));
}

static VOID
ActivatePeer(
    _Inout_ TERMSRV_RDP_PEER *Peer)
{
    ActivatePeerWithFastPath(Peer, TRUE);
}

static VOID
InitTestSessionManager(
    _Out_ TERMSRV_SESSION_MANAGER *Manager)
{
    TermSrvSessionManagerInit(Manager);
    Manager->SessmanUnavailable = TRUE;
}

#define TermSrvSessionManagerInit InitTestSessionManager

typedef struct _TEST_SESSION_BACKEND_CONTEXT
{
    INT CreateOrAttachCount;
    INT DisconnectCount;
    INT LogoffCount;
    INT CaptureFrameCount;
    INT InjectMouseCount;
    INT InjectKeyboardCount;
    INT ClipboardCount;
    INT LastSessionId;
    ULONG LastPointerFlags;
    UINT LastKeyboardCode;
    UINT LastKeyboardFlags;
    BOOL LastKeyDown;
    BOOL FailCreateOrAttach;
} TEST_SESSION_BACKEND_CONTEXT;

static BOOL
TestBackendCreateOrAttach(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_z_ PCWSTR UserIdentity,
    _In_z_ PCWSTR ClientName,
    _Out_ INT *AttachedSessionId)
{
    TEST_SESSION_BACKEND_CONTEXT *Context = (TEST_SESSION_BACKEND_CONTEXT *)Manager->BackendContext;

    Context->CreateOrAttachCount++;
    Context->LastSessionId = SessionId;
    if (Context->FailCreateOrAttach)
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
TestBackendDisconnect(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    TEST_SESSION_BACKEND_CONTEXT *Context = (TEST_SESSION_BACKEND_CONTEXT *)Manager->BackendContext;

    Context->DisconnectCount++;
    Context->LastSessionId = SessionId;
    return TermSrvSessionManagerDetachWin32Session(Manager,
                                                  SessionId,
                                                  TermSrvSessionDisconnected);
}

static BOOL
TestBackendLogoff(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId)
{
    TEST_SESSION_BACKEND_CONTEXT *Context = (TEST_SESSION_BACKEND_CONTEXT *)Manager->BackendContext;

    Context->LogoffCount++;
    Context->LastSessionId = SessionId;
    return TermSrvSessionManagerDetachWin32Session(Manager,
                                                  SessionId,
                                                  TermSrvSessionLoggedOff);
}

static BOOL
TestBackendCaptureFrame(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _Out_ TERMSRV_SESSION_FRAME *Frame)
{
    TEST_SESSION_BACKEND_CONTEXT *Context = (TEST_SESSION_BACKEND_CONTEXT *)Manager->BackendContext;

    Context->CaptureFrameCount++;
    Context->LastSessionId = SessionId;
    return TermSrvSessionManagerCaptureWin32Frame(Manager, SessionId, Frame);
}

static BOOL
TestBackendInjectMouse(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ ULONG PointerFlags,
    _In_ BOOL HasPointer,
    _In_ INT PointerX,
    _In_ INT PointerY)
{
    TEST_SESSION_BACKEND_CONTEXT *Context = (TEST_SESSION_BACKEND_CONTEXT *)Manager->BackendContext;

    Context->InjectMouseCount++;
    Context->LastSessionId = SessionId;
    Context->LastPointerFlags = PointerFlags;
    return TermSrvSessionManagerRecordWin32Input(Manager,
                                                SessionId,
                                                HasPointer,
                                                PointerX,
                                                PointerY);
}

static BOOL
TestBackendInjectKeyboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_ UINT VirtualKey,
    _In_ UINT KeyboardFlags,
    _In_ BOOL KeyDown)
{
    TEST_SESSION_BACKEND_CONTEXT *Context = (TEST_SESSION_BACKEND_CONTEXT *)Manager->BackendContext;

    Context->InjectKeyboardCount++;
    Context->LastSessionId = SessionId;
    Context->LastKeyboardCode = VirtualKey;
    Context->LastKeyboardFlags = KeyboardFlags;
    Context->LastKeyDown = KeyDown;
    return TermSrvSessionManagerRecordWin32Input(Manager, SessionId, FALSE, 0, 0);
}

static BOOL
TestBackendClipboard(
    _Inout_ TERMSRV_SESSION_MANAGER *Manager,
    _In_ INT SessionId,
    _In_reads_bytes_opt_(InputLength) const VOID *Input,
    _In_ SIZE_T InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *BytesWritten) VOID *Output,
    _In_ SIZE_T OutputLength,
    _Out_ SIZE_T *BytesWritten)
{
    TEST_SESSION_BACKEND_CONTEXT *Context = (TEST_SESSION_BACKEND_CONTEXT *)Manager->BackendContext;

    UNREFERENCED_PARAMETER(SessionId);
    UNREFERENCED_PARAMETER(Input);
    UNREFERENCED_PARAMETER(InputLength);
    UNREFERENCED_PARAMETER(Output);
    UNREFERENCED_PARAMETER(OutputLength);

    Context->ClipboardCount++;
    *BytesWritten = 0;
    return TRUE;
}

static const TERMSRV_SESSION_BACKEND TestSessionBackend =
{
    "test",
    TestBackendCreateOrAttach,
    TestBackendDisconnect,
    TestBackendLogoff,
    TestBackendCaptureFrame,
    TestBackendInjectMouse,
    TestBackendInjectKeyboard,
    TestBackendClipboard
};

static VOID
TestFullHandshake(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;
    TERMSRV_SESSION *Session;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);

    Session = TermSrvSessionManagerFindSession(&Manager, 1);
    ok(Session != NULL,
       "Session 1 was not found\n");
    ok(Session != NULL && Session->State == TermSrvSessionConnected,
       "Session was not connected\n");
    ok(Session != NULL && Session->Win32Attached,
       "Win32 session was not attached\n");
    ok(Session != NULL && wcscmp(Session->WinStationName, L"RDP-Tcp#1") == 0,
       "Unexpected WinStation binding\n");
    ok(Session != NULL && wcscmp(Session->DesktopName, L"Default") == 0,
       "Unexpected desktop binding\n");
    ok(strcmp(Peer.LastOutput, "FONT_MAP") == 0,
       "Expected FONT_MAP, got %s\n", Peer.LastOutput);
}

static VOID
TestOrderingAndChannelValidation(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);

    ok(TermSrvRdpPeerReceive(&Peer, "MCS channels=rdpdr") == TermSrvRdpOutOfOrder,
       "MCS before X224 should fail\n");
    ok(Peer.State == TermSrvRdpStateIdle,
       "State changed after rejected packet\n");

    ok(TermSrvRdpPeerReceive(&Peer, "X224") == TermSrvRdpSuccess,
       "X224 failed\n");
    ok(TermSrvRdpPeerReceive(&Peer, "MCS channels=rdpdr,rdpdr") == TermSrvRdpDuplicateChannel,
       "Duplicate channels should fail\n");
}

static VOID
TestTooManyChannels(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;
    CHAR Packet[160] = "MCS channels=";
    INT i;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);

    ok(TermSrvRdpPeerReceive(&Peer, "X224") == TermSrvRdpSuccess,
       "X224 failed\n");

    for (i = 0; i <= TERMSRV_RDP_MAX_CHANNELS; i++)
    {
        CHAR Channel[8];

        sprintf(Channel, "%sc%d", (i == 0) ? "" : ",", i);
        strcat(Packet, Channel);
    }

    ok(TermSrvRdpPeerReceive(&Peer, Packet) == TermSrvRdpTooManyChannels,
       "Too many static channels should fail\n");
    ok(Peer.State == TermSrvRdpStateX224Connected,
       "State changed after rejected channel list\n");
}

static VOID
TestAuthenticationFailure(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    Manager.RejectAuthentication = TRUE;
    TermSrvRdpPeerInit(&Peer, &Manager);
    JoinBaseChannels(&Peer, TRUE);

    ok(TermSrvRdpPeerReceive(&Peer, "SECURITY client_random=test") == TermSrvRdpSuccess,
       "SECURITY failed\n");
    ok(TermSrvRdpPeerReceive(&Peer, "CLIENT_INFO user=DOMAIN\\alice") == TermSrvRdpAuthenticationFailed,
       "Rejected authentication should fail\n");
    ok(Peer.State == TermSrvRdpStateSecurityComplete,
       "State changed after rejected authentication\n");
    ok(Manager.Sessions[0].State == TermSrvSessionIdle,
       "Authentication failure attached the session\n");
}

static VOID
TestAttachFailure(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    Manager.RejectAttach = TRUE;
    TermSrvRdpPeerInit(&Peer, &Manager);
    JoinBaseChannels(&Peer, TRUE);

    ok(TermSrvRdpPeerReceive(&Peer, "SECURITY client_random=test") == TermSrvRdpSuccess,
       "SECURITY failed\n");
    ok(TermSrvRdpPeerReceive(&Peer, "CLIENT_INFO user=DOMAIN\\alice") == TermSrvRdpAttachFailed,
       "Rejected attach should fail\n");
    ok(Peer.State == TermSrvRdpStateSecurityComplete,
       "State changed after rejected attach\n");
    ok(Manager.Sessions[0].State == TermSrvSessionIdle,
       "Attach failure connected the session\n");
}

static VOID
TestBackendSelectionAndBehavior(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;
    TEST_SESSION_BACKEND_CONTEXT Context;

    memset(&Context, 0, sizeof(Context));
    TermSrvSessionManagerInit(&Manager);

    ok(TermSrvSessionManagerGetBackend(&Manager) == TermSrvSessionManagerGetConsoleBackend(),
       "Console backend was not selected by default\n");
    ok(TermSrvSessionManagerGetDefaultBackend() == TermSrvSessionManagerGetConsoleBackend(),
       "Default backend is not console\n");

    TermSrvSessionManagerSetBackend(&Manager, &TestSessionBackend, &Context);
    ok(TermSrvSessionManagerGetBackend(&Manager) == &TestSessionBackend,
       "Custom backend was not selected\n");

    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);
    ok(Context.CreateOrAttachCount == 1,
       "CreateOrAttach was called %d times\n", Context.CreateOrAttachCount);
    ok(Context.LastSessionId == 1,
       "Backend attached unexpected session %d\n", Context.LastSessionId);

    ok(TermSrvRdpPeerReceive(&Peer, "SLOW_INPUT mouse=1 x=4 y=5") == TermSrvRdpSuccess,
       "Mouse input failed\n");
    ok(Context.InjectMouseCount == 1,
       "InjectMouse was called %d times\n", Context.InjectMouseCount);
    ok(Context.LastPointerFlags == 0x0800,
       "Unexpected mouse flags 0x%lx\n", Context.LastPointerFlags);

    ok(TermSrvRdpPeerReceive(&Peer, "SLOW_INPUT scancode=30") == TermSrvRdpSuccess,
       "Keyboard input failed\n");
    ok(Context.InjectKeyboardCount == 1,
       "InjectKeyboard was called %d times\n", Context.InjectKeyboardCount);
    ok(Context.LastKeyboardCode == 30 && Context.LastKeyboardFlags == 0 && Context.LastKeyDown,
       "Unexpected keyboard input code=%u flags=0x%x down=%d\n",
       Context.LastKeyboardCode,
       Context.LastKeyboardFlags,
       Context.LastKeyDown);
    ok(TermSrvRdpPeerReceive(&Peer, "SLOW_INPUT scancode=30 flags=32768") == TermSrvRdpSuccess,
       "Keyboard release input failed\n");
    ok(Context.InjectKeyboardCount == 2,
       "InjectKeyboard was called %d times after release\n", Context.InjectKeyboardCount);
    ok(Context.LastKeyboardCode == 30 && Context.LastKeyboardFlags == 32768 && !Context.LastKeyDown,
       "Unexpected keyboard release code=%u flags=0x%x down=%d\n",
       Context.LastKeyboardCode,
       Context.LastKeyboardFlags,
       Context.LastKeyDown);
    ok(TermSrvRdpPeerReceive(&Peer, "FAST_INPUT scancode=30 flags=3") == TermSrvRdpSuccess,
       "Fast keyboard release input failed\n");
    ok(Context.LastKeyboardCode == 30 &&
       Context.LastKeyboardFlags == 0x8100 &&
       !Context.LastKeyDown,
       "Unexpected fast keyboard release code=%u flags=0x%x down=%d\n",
       Context.LastKeyboardCode,
       Context.LastKeyboardFlags,
       Context.LastKeyDown);

    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 10, 10) == TermSrvRdpSuccess,
       "Bitmap update through backend failed\n");
    ok(Context.CaptureFrameCount == 1,
       "CaptureFrame was called %d times\n", Context.CaptureFrameCount);

    ok(TermSrvRdpPeerReceive(&Peer, "DISCONNECT") == TermSrvRdpSuccess,
       "Disconnect failed\n");
    ok(Context.DisconnectCount == 1,
       "Disconnect was called %d times\n", Context.DisconnectCount);

    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);
    ok(TermSrvRdpPeerReceive(&Peer, "LOGOFF") == TermSrvRdpSuccess,
       "Logoff failed\n");
    ok(Context.LogoffCount == 1,
       "Logoff was called %d times\n", Context.LogoffCount);

    TermSrvSessionManagerSetBackend(&Manager, NULL, NULL);
    ok(TermSrvSessionManagerGetBackend(&Manager) == TermSrvSessionManagerGetDefaultBackend(),
       "NULL backend did not restore default backend\n");
}

static VOID
TestSessmanBackendFallback(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;
    TERMSRV_SESSION *Session;

    TermSrvSessionManagerInit(&Manager);
    ok(TermSrvSessionManagerSelectBackendByName(&Manager, L"sessman"),
       "Sessman backend selection failed\n");
    ok(TermSrvSessionManagerGetBackend(&Manager) == TermSrvSessionManagerGetSessmanBackend(),
       "Sessman backend was not selected by name\n");

    Manager.SessmanUnavailable = TRUE;
    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);

    ok(Manager.SessmanFallback,
       "Sessman backend did not report deterministic fallback\n");
    ok(Peer.SessionId == 1,
       "Sessman fallback attached peer to session %d\n", Peer.SessionId);
    Session = TermSrvSessionManagerFindSession(&Manager, Peer.SessionId);
    ok(Session != NULL && Session->State == TermSrvSessionConnected,
       "Sessman fallback did not connect the session\n");
    ok(Session != NULL && Session->Win32Attached,
       "Sessman fallback did not attach Win32 state\n");
    ok(Session != NULL && !Session->SessmanStarted,
       "Sessman fallback should not mark a real SMSS session started\n");
}

static VOID
TestConsoleBackendBinding(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;
    TERMSRV_SESSION *Session;
    TERMSRV_SESSION_FRAME Frame;
    const TERMSRV_SESSION_BACKEND *Backend;

    TermSrvSessionManagerInit(&Manager);
    TermSrvSessionManagerSetBackend(&Manager,
                                    TermSrvSessionManagerGetConsoleBackend(),
                                    NULL);
    Backend = TermSrvSessionManagerGetBackend(&Manager);
    ok(Backend == TermSrvSessionManagerGetConsoleBackend(),
       "Console backend was not selected directly\n");

    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);

    ok(Peer.SessionId == 0,
       "Console backend attached peer to session %d\n", Peer.SessionId);
    Session = TermSrvSessionManagerFindSession(&Manager, 0);
    ok(Session != NULL,
       "Console session was not found\n");
    ok(Session != NULL && Session->State == TermSrvSessionConnected,
       "Console session was not connected\n");
    ok(Session != NULL && Session->Win32Attached,
       "Console Win32 session was not attached\n");
    ok(Session != NULL && wcscmp(Session->WinStationName, L"WinSta0") == 0,
       "Unexpected console WinStation binding\n");
    ok(Session != NULL && wcscmp(Session->DesktopName, L"Default") == 0,
       "Unexpected console desktop binding\n");
    ok(Manager.SessionCount == 1,
       "Console backend created %d session records\n", Manager.SessionCount);

    ok(TermSrvRdpPeerReceive(&Peer, "FAST_INPUT mouse=1 x=22 y=33") == TermSrvRdpSuccess,
       "Console input failed\n");
    ok(Backend->CaptureFrame(&Manager, Peer.SessionId, &Frame),
       "Console frame capture failed\n");
    ok(Frame.SessionId == 0,
       "Console frame came from session %d\n", Frame.SessionId);
    ok(Frame.DesktopWidth == 1024 && Frame.DesktopHeight == 768 && Frame.ColorDepth == 16,
       "Unexpected console frame metadata %dx%dx%d\n",
       Frame.DesktopWidth,
       Frame.DesktopHeight,
       Frame.ColorDepth);
    ok(Frame.HasPointer && Frame.PointerX == 22 && Frame.PointerY == 33,
       "Unexpected console pointer metadata %d,%d\n", Frame.PointerX, Frame.PointerY);

    ok(TermSrvRdpPeerReceive(&Peer, "DISCONNECT") == TermSrvRdpSuccess,
       "Console disconnect failed\n");
    ok(Session != NULL && Session->State == TermSrvSessionDisconnected,
       "Console disconnect did not mark session disconnected\n");
    ok(Session != NULL && !Session->Win32Attached,
       "Console disconnect kept Win32 attached\n");

    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);
    ok(Peer.SessionId == 0,
       "Console reconnect attached peer to session %d\n", Peer.SessionId);
    ok(Manager.SessionCount == 1,
       "Console reconnect created %d session records\n", Manager.SessionCount);

    ok(TermSrvRdpPeerReceive(&Peer, "LOGOFF") == TermSrvRdpSuccess,
       "Console logoff failed\n");
    ok(Session != NULL && Session->State == TermSrvSessionLoggedOff,
       "Console logoff did not mark session logged off\n");
    ok(Session != NULL && !Session->Win32Attached,
       "Console logoff kept Win32 attached\n");
}

static VOID
TestBackendSelectionFallback(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;

    TermSrvSessionManagerInit(&Manager);
    ok(TermSrvSessionManagerSelectBackendByName(&Manager, L"console"),
       "Console backend selection failed\n");
    ok(TermSrvSessionManagerGetBackend(&Manager) == TermSrvSessionManagerGetConsoleBackend(),
       "Console backend was not selected by name\n");
    ok(Manager.SessionCount == 1 && Manager.Sessions[0].SessionId == 0,
       "Console selection did not prepare session 0\n");

    ok(!TermSrvSessionManagerSelectBackendByName(&Manager, L"unknown"),
       "Unknown backend selection should report fallback\n");
    ok(TermSrvSessionManagerGetBackend(&Manager) == TermSrvSessionManagerGetDefaultBackend(),
       "Unknown backend did not fall back to default\n");

    ok(TermSrvSessionManagerSelectBackendByName(&Manager, L"dummy"),
       "Dummy backend selection failed\n");
    ok(TermSrvSessionManagerGetBackend(&Manager) != TermSrvSessionManagerGetDefaultBackend(),
       "Dummy backend unexpectedly matched default\n");

    ok(TermSrvSessionManagerSelectBackendByName(&Manager, NULL),
       "NULL backend selection should select default\n");
    ok(TermSrvSessionManagerGetBackend(&Manager) == TermSrvSessionManagerGetDefaultBackend(),
       "NULL backend did not select default\n");
}

static VOID
TestClientInfoBeforeSecurity(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);
    JoinBaseChannels(&Peer, TRUE);

    ok(TermSrvRdpPeerReceive(&Peer, "CLIENT_INFO user=DOMAIN\\alice") == TermSrvRdpOutOfOrder,
       "CLIENT_INFO before SECURITY should fail\n");
    ok(Peer.State == TermSrvRdpStateChannelsJoined,
       "State changed after early CLIENT_INFO\n");
    ok(Manager.Sessions[0].State == TermSrvSessionIdle,
       "Early CLIENT_INFO attached the session\n");
}

static VOID
TestInputAndVirtualChannels(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;
    TERMSRV_SESSION *Session;
    TERMSRV_SESSION_FRAME Frame;

    TermSrvSessionManagerInit(&Manager);
    ok(TermSrvSessionManagerSelectBackendByName(&Manager, L"dummy"),
       "Dummy backend selection failed\n");
    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);
    Session = TermSrvSessionManagerFindSession(&Manager, Peer.SessionId);

    ok(TermSrvRdpPeerReceive(&Peer, "SLOW_INPUT mouse=1 x=12 y=34") == TermSrvRdpSuccess,
       "Slow-path input failed\n");
    ok(Manager.LastInputSessionId == 1,
       "Input delivered to session %d\n", Manager.LastInputSessionId);
    ok(Session != NULL && Session->HasPointer,
       "Pointer state was not recorded\n");
    ok(Session != NULL && Session->LastPointerX == 12 && Session->LastPointerY == 34,
       "Unexpected pointer state %d,%d\n",
       Session != NULL ? Session->LastPointerX : -1,
       Session != NULL ? Session->LastPointerY : -1);

    ok(TermSrvRdpPeerReceive(&Peer, "FAST_INPUT mouse=1 x=56 y=78") == TermSrvRdpSuccess,
       "Fast-path input failed\n");
    ok(Session != NULL && Session->LastPointerX == 56 && Session->LastPointerY == 78,
       "Unexpected fast-path pointer state %d,%d\n",
       Session != NULL ? Session->LastPointerX : -1,
       Session != NULL ? Session->LastPointerY : -1);
    ok(TermSrvSessionManagerCaptureWin32Frame(&Manager, Peer.SessionId, &Frame),
       "Win32 frame capture failed\n");
    ok(Frame.SessionId == 1 && Frame.DesktopWidth == 1024 && Frame.DesktopHeight == 768,
       "Unexpected frame metadata session=%d size=%dx%d\n",
       Frame.SessionId,
       Frame.DesktopWidth,
       Frame.DesktopHeight);
    ok(Frame.HasPointer && Frame.PointerX == 56 && Frame.PointerY == 78,
       "Unexpected frame pointer %d,%d\n", Frame.PointerX, Frame.PointerY);

    ok(TermSrvRdpPeerReceive(&Peer, "VC id=1004 payload=device") == TermSrvRdpSuccess,
       "Virtual channel payload failed\n");
    ok(strcmp(Manager.LastChannelName, "rdpdr") == 0,
       "Expected rdpdr, got %s\n", Manager.LastChannelName);

    ok(TermSrvRdpPeerReceive(&Peer, "VC id=9999 payload=device") == TermSrvRdpUnknownChannel,
       "Unknown virtual channel should fail\n");
}

static VOID
TestFastPathNotNegotiated(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeerWithFastPath(&Peer, FALSE);

    ok(TermSrvRdpPeerReceive(&Peer, "FAST_INPUT mouse=1") == TermSrvRdpFastPathNotNegotiated,
       "Fast-path input without negotiation should fail\n");
    ok(Manager.LastInputSessionId == -1,
       "Unnegotiated fast-path input was delivered to session %d\n", Manager.LastInputSessionId);

    ok(TermSrvRdpPeerReceive(&Peer, "SLOW_INPUT scancode=30") == TermSrvRdpSuccess,
       "Slow-path input should still work\n");
}

static VOID
TestDisconnectLogoffAndReconnect(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);

    ok(TermSrvRdpPeerReceive(&Peer, "DISCONNECT") == TermSrvRdpSuccess,
       "Disconnect failed\n");
    ok(Peer.State == TermSrvRdpStateClosed,
       "Peer did not close on disconnect\n");
    ok(Manager.Sessions[0].State == TermSrvSessionDisconnected,
       "Disconnect did not preserve session\n");
    ok(!Manager.Sessions[0].Win32Attached,
       "Disconnect kept the Win32 session attached\n");

    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);
    ok(Peer.SessionId == 1,
       "Reconnect did not reuse session 1, got %d\n", Peer.SessionId);

    ok(TermSrvRdpPeerReceive(&Peer, "LOGOFF") == TermSrvRdpSuccess,
       "Logoff failed\n");
    ok(Manager.Sessions[0].State == TermSrvSessionLoggedOff,
       "Logoff did not destroy session\n");
    ok(!Manager.Sessions[0].Win32Attached,
       "Logoff kept the Win32 session attached\n");
}

static VOID
TestGraphicsOutput(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    ok(TermSrvSessionManagerSelectBackendByName(&Manager, L"dummy"),
       "Dummy backend selection failed\n");
    TermSrvRdpPeerInit(&Peer, &Manager);

    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 10, 10) == TermSrvRdpGraphicsBeforeActive,
       "Graphics before Active should fail\n");

    ActivatePeer(&Peer);

    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 10, 10) == TermSrvRdpSuccess,
       "Bitmap update failed\n");
    ok(strcmp(Peer.LastOutput, "GRAPHICS_UPDATE") == 0,
       "Expected GRAPHICS_UPDATE, got %s\n", Peer.LastOutput);

    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 1024, 768) == TermSrvRdpSuccess,
       "Desktop-sized bitmap update failed\n");
    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 4096, 10) == TermSrvRdpBitmapOutOfBounds,
       "Out-of-bounds bitmap width should fail\n");
    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 10, 4096) == TermSrvRdpBitmapOutOfBounds,
       "Out-of-bounds bitmap height should fail\n");
}

static VOID
TestBgra32ToRdpBitmapData(VOID)
{
    static const UCHAR Source[] =
    {
        0x10, 0x11, 0x12, 0xff, 0x20, 0x21, 0x22, 0xff,
        0x30, 0x31, 0x32, 0xff, 0x40, 0x41, 0x42, 0xff
    };
    static const UCHAR Expected[] =
    {
        0x30, 0x31, 0x32, 0xff, 0x40, 0x41, 0x42, 0xff,
        0x10, 0x11, 0x12, 0xff, 0x20, 0x21, 0x22, 0xff
    };
    UCHAR Destination[sizeof(Source)];

    memset(Destination, 0, sizeof(Destination));
    ok(TermSrvConvertBgra32ToRdpBitmapData(Source,
                                           2,
                                           2,
                                           2 * 4,
                                           Destination),
       "BGRA32 conversion failed\n");
    ok(memcmp(Destination, Expected, sizeof(Expected)) == 0,
       "BGRA32 conversion did not produce bottom-up RDP bitmap data\n");

    ok(!TermSrvConvertBgra32ToRdpBitmapData(Source,
                                            2,
                                            2,
                                            2 * 4 - 1,
                                            Destination),
       "Invalid source pitch should fail\n");
}

static VOID
TestUnknownPacket(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);

    ok(TermSrvRdpPeerReceive(&Peer, "BOGUS") == TermSrvRdpInvalidPacket,
       "Unknown packet should fail\n");
    ok(Peer.State == TermSrvRdpStateIdle,
       "State changed after unknown packet\n");
}

static VOID
TestRdpBcgrParseConnectionRequest(VOID)
{
    static const UCHAR BasicRequest[] =
    {
        0x03, 0x00, 0x00, 0x0b,
        0x06, 0xe0, 0x00, 0x00, 0x12, 0x34, 0x00
    };
    static const UCHAR NegotiatedRequest[] =
    {
        0x03, 0x00, 0x00, 0x13,
        0x0e, 0xe0, 0x00, 0x00, 0x12, 0x34, 0x00,
        0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00
    };
    static const UCHAR CookieRequest[] =
    {
        0x03, 0x00, 0x00, 0x22,
        0x1d, 0xe0, 0x00, 0x00, 0x12, 0x34, 0x00,
        'C', 'o', 'o', 'k', 'i', 'e', ':', ' ',
        'm', 's', 't', 's', 'h', 'a', 's', 'h',
        '=', 't', 'e', 's', 't', '\r', '\n'
    };
    TERMSRV_RDPBCGR_CONNECTION_REQUEST Request;

    ok(TermSrvRdpBcgrParseConnectionRequest(BasicRequest,
                                            sizeof(BasicRequest),
                                            &Request) == TermSrvRdpBcgrSuccess,
       "Basic request parse failed\n");
    ok(Request.SourceReference == 0x1234,
       "Unexpected source reference 0x%x\n", Request.SourceReference);
    ok(!Request.HasCookie && !Request.HasNegotiation,
       "Basic request unexpectedly had cookie or negotiation data\n");

    ok(TermSrvRdpBcgrParseConnectionRequest(NegotiatedRequest,
                                            sizeof(NegotiatedRequest),
                                            &Request) == TermSrvRdpBcgrSuccess,
       "Negotiated request parse failed\n");
    ok(Request.HasNegotiation,
       "Negotiation request was not detected\n");
    ok(Request.Negotiation.Protocols == TermSrvRdpBcgrProtocolSsl,
       "Unexpected negotiated protocols 0x%lx\n", Request.Negotiation.Protocols);

    ok(TermSrvRdpBcgrParseConnectionRequest(CookieRequest,
                                            sizeof(CookieRequest),
                                            &Request) == TermSrvRdpBcgrSuccess,
       "Cookie request parse failed\n");
    ok(Request.HasCookie,
       "Cookie was not detected\n");
    ok(Request.CookieLength == 21,
       "Unexpected cookie length %u\n", Request.CookieLength);

    ok(TermSrvRdpBcgrParseConnectionRequest(BasicRequest,
                                            sizeof(BasicRequest) - 1,
                                            &Request) == TermSrvRdpBcgrNeedMoreData,
       "Truncated packet should request more data\n");
}

static VOID
TestRdpBcgrParseMcsConnectInitial(VOID)
{
    static const UCHAR ConnectInitial[] =
    {
        0x03, 0x00, 0x00, 0x0c,
        0x02, 0xf0, 0x80,
        0x7f, 0x65, 0x03, 0x01, 0x02
    };
    static const UCHAR BadTpkt[] =
    {
        0x02, 0x00, 0x00, 0x0c,
        0x02, 0xf0, 0x80,
        0x7f, 0x65, 0x03, 0x01, 0x02
    };
    static const UCHAR BadX224Type[] =
    {
        0x03, 0x00, 0x00, 0x0c,
        0x02, 0xe0, 0x80,
        0x7f, 0x65, 0x03, 0x01, 0x02
    };
    static const UCHAR LengthMismatch[] =
    {
        0x03, 0x00, 0x00, 0x0b,
        0x02, 0xf0, 0x80,
        0x7f, 0x65, 0x03, 0x01, 0x02
    };
    TERMSRV_RDPBCGR_MCS_CONNECT_INITIAL Parsed;

    ok(TermSrvRdpBcgrParseMcsConnectInitial(ConnectInitial,
                                            sizeof(ConnectInitial),
                                            &Parsed) == TermSrvRdpBcgrSuccess,
       "MCS Connect Initial parse failed\n");
    ok(Parsed.Payload == &ConnectInitial[7],
       "Unexpected MCS payload pointer\n");
    ok(Parsed.PayloadLength == 5,
       "Unexpected MCS payload length %Iu\n", Parsed.PayloadLength);
    ok(memcmp(Parsed.Payload, "\x7f\x65\x03\x01\x02", Parsed.PayloadLength) == 0,
       "Unexpected opaque MCS payload bytes\n");

    ok(TermSrvRdpBcgrParseMcsConnectInitial(ConnectInitial,
                                            6,
                                            &Parsed) == TermSrvRdpBcgrNeedMoreData,
       "Short MCS Connect Initial should request more data\n");

    ok(TermSrvRdpBcgrParseMcsConnectInitial(BadTpkt,
                                            sizeof(BadTpkt),
                                            &Parsed) == TermSrvRdpBcgrInvalidHeader,
       "Bad TPKT header should fail\n");

    ok(TermSrvRdpBcgrParseMcsConnectInitial(BadX224Type,
                                            sizeof(BadX224Type),
                                            &Parsed) == TermSrvRdpBcgrUnsupportedPdu,
       "Bad X.224 type should fail\n");

    ok(TermSrvRdpBcgrParseMcsConnectInitial(LengthMismatch,
                                            sizeof(LengthMismatch),
                                            &Parsed) == TermSrvRdpBcgrInvalidLength,
       "Mismatched TPKT length should fail\n");
}

static VOID
TestRdpBcgrWriteConnectionConfirm(VOID)
{
    static const UCHAR ExpectedConfirm[] =
    {
        0x03, 0x00, 0x00, 0x13,
        0x0e, 0xd0, 0x12, 0x34, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    TERMSRV_RDPBCGR_CONNECTION_CONFIRM Confirm;
    UCHAR Buffer[32];
    SIZE_T BytesWritten = 0;

    memset(&Confirm, 0, sizeof(Confirm));
    Confirm.DestinationReference = 0x1234;
    Confirm.HasNegotiation = TRUE;
    Confirm.Negotiation.Type = TermSrvRdpBcgrNegResponse;
    Confirm.Negotiation.Length = 8;
    Confirm.Negotiation.Protocols = TermSrvRdpBcgrProtocolStandard;

    ok(TermSrvRdpBcgrWriteConnectionConfirm(Buffer,
                                            sizeof(Buffer),
                                            &Confirm,
                                            &BytesWritten) == TermSrvRdpBcgrSuccess,
       "Connection Confirm serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedConfirm),
       "Unexpected serialized size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedConfirm, sizeof(ExpectedConfirm)) == 0,
       "Serialized Connection Confirm did not match expected bytes\n");

    ok(TermSrvRdpBcgrWriteConnectionConfirm(Buffer,
                                            4,
                                            &Confirm,
                                            &BytesWritten) == TermSrvRdpBcgrBufferTooSmall,
       "Short output buffer should fail\n");
}

static VOID
TestRdpBcgrWriteMcsConnectResponse(VOID)
{
    static const UCHAR Payload[] =
    {
        0x7f, 0x66, 0x72,
        0x0a, 0x01, 0x00,
        0x02, 0x01, 0x00,
        0x30, 0x20,
        0x02, 0x02, 0x00, 0x22,
        0x02, 0x02, 0x00, 0x03,
        0x02, 0x02, 0x00, 0x00,
        0x02, 0x02, 0x00, 0x01,
        0x02, 0x02, 0x00, 0x00,
        0x02, 0x02, 0x00, 0x01,
        0x02, 0x02, 0xff, 0xff,
        0x02, 0x02, 0x00, 0x02,
        0x04, 0x40,
        0x00, 0x05, 0x00, 0x14, 0x7c, 0x00, 0x01,
        0x2a, 0x14, 0x76, 0x0a, 0x01, 0x01, 0x00,
        0x01, 0xc0, 0x00, 'M', 'c', 'D', 'n', 0x32,
        0x01, 0x0c, 0x10, 0x00,
        0x04, 0x00, 0x08, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x03, 0x0c, 0x10, 0x00,
        0xeb, 0x03, 0x04, 0x00,
        0xec, 0x03, 0xed, 0x03,
        0xee, 0x03, 0xef, 0x03,
        0x04, 0x0c, 0x06, 0x00,
        0xf0, 0x03,
        0x02, 0x0c, 0x0c, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR ExpectedResponse[] =
    {
        0x03, 0x00, 0x00, 0x7c,
        0x02, 0xf0, 0x80,
        0x7f, 0x66, 0x72,
        0x0a, 0x01, 0x00,
        0x02, 0x01, 0x00,
        0x30, 0x20,
        0x02, 0x02, 0x00, 0x22,
        0x02, 0x02, 0x00, 0x03,
        0x02, 0x02, 0x00, 0x00,
        0x02, 0x02, 0x00, 0x01,
        0x02, 0x02, 0x00, 0x00,
        0x02, 0x02, 0x00, 0x01,
        0x02, 0x02, 0xff, 0xff,
        0x02, 0x02, 0x00, 0x02,
        0x04, 0x40,
        0x00, 0x05, 0x00, 0x14, 0x7c, 0x00, 0x01,
        0x2a, 0x14, 0x76, 0x0a, 0x01, 0x01, 0x00,
        0x01, 0xc0, 0x00, 'M', 'c', 'D', 'n', 0x32,
        0x01, 0x0c, 0x10, 0x00,
        0x04, 0x00, 0x08, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x03, 0x0c, 0x10, 0x00,
        0xeb, 0x03, 0x04, 0x00,
        0xec, 0x03, 0xed, 0x03,
        0xee, 0x03, 0xef, 0x03,
        0x04, 0x0c, 0x06, 0x00,
        0xf0, 0x03,
        0x02, 0x0c, 0x0c, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    UCHAR Buffer[128];
    SIZE_T BytesWritten = 0;

    ok(TermSrvRdpBcgrWriteMcsConnectResponse(Buffer,
                                             sizeof(Buffer),
                                             Payload,
                                             sizeof(Payload),
                                             &BytesWritten) == TermSrvRdpBcgrSuccess,
       "MCS Connect Response serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedResponse),
       "Unexpected serialized MCS Connect Response size %Iu\n", BytesWritten);
    if (memcmp(Buffer, ExpectedResponse, sizeof(ExpectedResponse)) == 0)
    {
        ok(TRUE,
           "Serialized MCS Connect Response matched expected bytes\n");
    }
    else
    {
        SIZE_T Index;

        for (Index = 0; Index < sizeof(ExpectedResponse); Index++)
        {
            if (Buffer[Index] != ExpectedResponse[Index])
            {
                ok(FALSE,
                   "Serialized MCS Connect Response differed at byte %Iu: got 0x%02x expected 0x%02x\n",
                   Index,
                   Buffer[Index],
                   ExpectedResponse[Index]);
                break;
            }
        }
    }

    ok(TermSrvRdpBcgrWriteMcsConnectResponse(Buffer,
                                             sizeof(ExpectedResponse) - 1,
                                             Payload,
                                             sizeof(Payload),
                                             &BytesWritten) == TermSrvRdpBcgrBufferTooSmall,
       "Short MCS Connect Response output buffer should fail\n");
    ok(BytesWritten == 0,
       "Short MCS Connect Response write returned %Iu bytes\n", BytesWritten);
}

static VOID
TestRdpBcgrParseMcsErectDomainRequest(VOID)
{
    static const UCHAR ErectDomainRequest[] =
    {
        0x03, 0x00, 0x00, 0x0c,
        0x02, 0xf0, 0x80,
        0x04, 0x01, 0x00, 0x01, 0x00
    };
    static const UCHAR BadBody[] =
    {
        0x03, 0x00, 0x00, 0x0c,
        0x02, 0xf0, 0x80,
        0x05, 0x01, 0x00, 0x01, 0x00
    };

    ok(TermSrvRdpBcgrParseMcsErectDomainRequest(ErectDomainRequest,
                                                sizeof(ErectDomainRequest)) == TermSrvRdpBcgrSuccess,
       "MCS Erect Domain Request parse failed\n");

    ok(TermSrvRdpBcgrParseMcsErectDomainRequest(ErectDomainRequest,
                                                6) == TermSrvRdpBcgrNeedMoreData,
       "Short MCS Erect Domain Request should request more data\n");

    ok(TermSrvRdpBcgrParseMcsErectDomainRequest(BadBody,
                                                sizeof(BadBody)) == TermSrvRdpBcgrUnsupportedPdu,
       "Malformed MCS Erect Domain Request body should fail\n");
}

static VOID
TestRdpBcgrParseMcsAttachUserRequest(VOID)
{
    static const UCHAR AttachUserRequest[] =
    {
        0x03, 0x00, 0x00, 0x08,
        0x02, 0xf0, 0x80,
        0x28
    };
    static const UCHAR BadLength[] =
    {
        0x03, 0x00, 0x00, 0x09,
        0x02, 0xf0, 0x80,
        0x28, 0x00
    };

    ok(TermSrvRdpBcgrParseMcsAttachUserRequest(AttachUserRequest,
                                               sizeof(AttachUserRequest)) == TermSrvRdpBcgrSuccess,
       "MCS Attach User Request parse failed\n");

    ok(TermSrvRdpBcgrParseMcsAttachUserRequest(AttachUserRequest,
                                               7) == TermSrvRdpBcgrNeedMoreData,
       "Short MCS Attach User Request should request more data\n");

    ok(TermSrvRdpBcgrParseMcsAttachUserRequest(BadLength,
                                               sizeof(BadLength)) == TermSrvRdpBcgrInvalidLength,
       "Malformed MCS Attach User Request length should fail\n");
}

static VOID
TestRdpBcgrWriteMcsAttachUserConfirm(VOID)
{
    static const UCHAR ExpectedConfirm[] =
    {
        0x03, 0x00, 0x00, 0x0b,
        0x02, 0xf0, 0x80,
        0x2e, 0x00, 0x03, 0xe9
    };
    TERMSRV_RDPBCGR_MCS_ATTACH_USER_CONFIRM Confirm;
    UCHAR Buffer[16];
    SIZE_T BytesWritten = 0;

    Confirm.UserChannelId = 1001;

    ok(TermSrvRdpBcgrWriteMcsAttachUserConfirm(Buffer,
                                               sizeof(Buffer),
                                               &Confirm,
                                               &BytesWritten) == TermSrvRdpBcgrSuccess,
       "MCS Attach User Confirm serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedConfirm),
       "Unexpected serialized MCS Attach User Confirm size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedConfirm, sizeof(ExpectedConfirm)) == 0,
       "Serialized MCS Attach User Confirm did not match expected bytes\n");

    ok(TermSrvRdpBcgrWriteMcsAttachUserConfirm(Buffer,
                                               sizeof(ExpectedConfirm) - 1,
                                               &Confirm,
                                               &BytesWritten) == TermSrvRdpBcgrBufferTooSmall,
       "Short MCS Attach User Confirm output buffer should fail\n");
    ok(BytesWritten == 0,
       "Short MCS Attach User Confirm write returned %Iu bytes\n", BytesWritten);
}

static VOID
TestRdpBcgrParseMcsChannelJoinRequest(VOID)
{
    static const UCHAR ChannelJoinRequest[] =
    {
        0x03, 0x00, 0x00, 0x0c,
        0x02, 0xf0, 0x80,
        0x38, 0x03, 0xe9, 0x03, 0xeb
    };
    static const UCHAR BadTpktLength[] =
    {
        0x03, 0x00, 0x00, 0x0b,
        0x02, 0xf0, 0x80,
        0x38, 0x03, 0xe9, 0x03, 0xeb
    };
    TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_REQUEST Request;

    ok(TermSrvRdpBcgrParseMcsChannelJoinRequest(ChannelJoinRequest,
                                                sizeof(ChannelJoinRequest),
                                                &Request) == TermSrvRdpBcgrSuccess,
       "MCS Channel Join Request parse failed\n");
    ok(Request.Initiator == 1001,
       "Unexpected MCS Channel Join Request initiator %u\n", Request.Initiator);
    ok(Request.ChannelId == 1003,
       "Unexpected MCS Channel Join Request channel %u\n", Request.ChannelId);

    ok(TermSrvRdpBcgrParseMcsChannelJoinRequest(ChannelJoinRequest,
                                                6,
                                                &Request) == TermSrvRdpBcgrNeedMoreData,
       "Short MCS Channel Join Request should request more data\n");

    ok(TermSrvRdpBcgrParseMcsChannelJoinRequest(BadTpktLength,
                                                sizeof(BadTpktLength),
                                                &Request) == TermSrvRdpBcgrInvalidLength,
       "Mismatched MCS Channel Join Request TPKT length should fail\n");
}

static VOID
TestRdpBcgrParseSecurityExchangePayload(VOID)
{
    static const UCHAR SecurityExchange[] =
    {
        0x03, 0x00, 0x00, 0x17,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x08,
        0x01, 0x00, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0xdd
    };
    static const UCHAR BadTpkt[] =
    {
        0x02, 0x00, 0x00, 0x17,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x08,
        0x01, 0x00, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0xdd
    };
    static const UCHAR BadX224Type[] =
    {
        0x03, 0x00, 0x00, 0x17,
        0x02, 0xe0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x08,
        0x01, 0x00, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0xdd
    };
    static const UCHAR BadMcsMarker[] =
    {
        0x03, 0x00, 0x00, 0x17,
        0x02, 0xf0, 0x80,
        0x65, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x08,
        0x01, 0x00, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0xdd
    };
    static const UCHAR BadPayloadLength[] =
    {
        0x03, 0x00, 0x00, 0x17,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x09,
        0x01, 0x00, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0xdd
    };
    static const UCHAR ShortMcsBody[] =
    {
        0x03, 0x00, 0x00, 0x0c,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb
    };
    TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD Parsed;

    ok(TermSrvRdpBcgrParseSecurityExchangePayload(SecurityExchange,
                                                  sizeof(SecurityExchange),
                                                  &Parsed) == TermSrvRdpBcgrSuccess,
       "Security Exchange payload parse failed\n");
    ok(Parsed.Initiator == 1001,
       "Unexpected Security Exchange initiator %u\n", Parsed.Initiator);
    ok(Parsed.ChannelId == 1003,
       "Unexpected Security Exchange channel %u\n", Parsed.ChannelId);
    ok(Parsed.Priority == 0x70,
       "Unexpected Security Exchange priority 0x%x\n", Parsed.Priority);
    ok(Parsed.Flags == 0x00000001,
       "Unexpected Security Exchange flags 0x%lx\n", Parsed.Flags);
    ok(Parsed.Payload == &SecurityExchange[19],
       "Unexpected Security Exchange payload pointer\n");
    ok(Parsed.PayloadLength == 4,
       "Unexpected Security Exchange payload length %Iu\n", Parsed.PayloadLength);
    ok(memcmp(Parsed.Payload, "\xaa\xbb\xcc\xdd", Parsed.PayloadLength) == 0,
       "Unexpected Security Exchange opaque bytes\n");

    ok(TermSrvRdpBcgrParseSecurityExchangePayload(NULL,
                                                  sizeof(SecurityExchange),
                                                  &Parsed) == TermSrvRdpBcgrInvalidHeader,
       "NULL Security Exchange buffer should fail\n");
    ok(TermSrvRdpBcgrParseSecurityExchangePayload(SecurityExchange,
                                                  sizeof(SecurityExchange),
                                                  NULL) == TermSrvRdpBcgrInvalidHeader,
       "NULL Security Exchange output should fail\n");
    ok(TermSrvRdpBcgrParseSecurityExchangePayload(SecurityExchange,
                                                  6,
                                                  &Parsed) == TermSrvRdpBcgrNeedMoreData,
       "Short Security Exchange packet should request more data\n");
    ok(TermSrvRdpBcgrParseSecurityExchangePayload(BadTpkt,
                                                  sizeof(BadTpkt),
                                                  &Parsed) == TermSrvRdpBcgrInvalidHeader,
       "Bad Security Exchange TPKT should fail\n");
    ok(TermSrvRdpBcgrParseSecurityExchangePayload(BadX224Type,
                                                  sizeof(BadX224Type),
                                                  &Parsed) == TermSrvRdpBcgrUnsupportedPdu,
       "Bad Security Exchange X.224 type should fail\n");
    ok(TermSrvRdpBcgrParseSecurityExchangePayload(BadMcsMarker,
                                                  sizeof(BadMcsMarker),
                                                  &Parsed) == TermSrvRdpBcgrUnsupportedPdu,
       "Bad Security Exchange MCS marker should fail\n");
    ok(TermSrvRdpBcgrParseSecurityExchangePayload(BadPayloadLength,
                                                  sizeof(BadPayloadLength),
                                                  &Parsed) == TermSrvRdpBcgrInvalidLength,
       "Bad Security Exchange payload length should fail\n");
    ok(TermSrvRdpBcgrParseSecurityExchangePayload(ShortMcsBody,
                                                  sizeof(ShortMcsBody),
                                                  &Parsed) == TermSrvRdpBcgrInvalidLength,
       "Short Security Exchange MCS body should fail\n");
}

static VOID
TestRdpBcgrParseClientInfoPayload(VOID)
{
    static const UCHAR ClientInfo[] =
    {
        0x03, 0x00, 0x00, 0x16,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x07,
        0x40, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33
    };
    static const UCHAR BadFlags[] =
    {
        0x03, 0x00, 0x00, 0x16,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x07,
        0x01, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33
    };
    static const UCHAR ShortFlags[] =
    {
        0x03, 0x00, 0x00, 0x12,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x03,
        0x40, 0x00, 0x00
    };
    TERMSRV_RDPBCGR_OPAQUE_SECURITY_PAYLOAD Parsed;

    ok(TermSrvRdpBcgrParseClientInfoPayload(ClientInfo,
                                            sizeof(ClientInfo),
                                            &Parsed) == TermSrvRdpBcgrSuccess,
       "Client Info payload parse failed\n");
    ok(Parsed.Initiator == 1001,
       "Unexpected Client Info initiator %u\n", Parsed.Initiator);
    ok(Parsed.ChannelId == 1003,
       "Unexpected Client Info channel %u\n", Parsed.ChannelId);
    ok(Parsed.Priority == 0x70,
       "Unexpected Client Info priority 0x%x\n", Parsed.Priority);
    ok(Parsed.Flags == 0x00000040,
       "Unexpected Client Info flags 0x%lx\n", Parsed.Flags);
    ok(Parsed.Payload == &ClientInfo[19],
       "Unexpected Client Info payload pointer\n");
    ok(Parsed.PayloadLength == 3,
       "Unexpected Client Info payload length %Iu\n", Parsed.PayloadLength);
    ok(memcmp(Parsed.Payload, "\x11\x22\x33", Parsed.PayloadLength) == 0,
       "Unexpected Client Info opaque bytes\n");

    ok(TermSrvRdpBcgrParseClientInfoPayload(ClientInfo,
                                            6,
                                            &Parsed) == TermSrvRdpBcgrNeedMoreData,
       "Short Client Info packet should request more data\n");
    ok(TermSrvRdpBcgrParseClientInfoPayload(BadFlags,
                                            sizeof(BadFlags),
                                            &Parsed) == TermSrvRdpBcgrUnsupportedPdu,
       "Bad Client Info flags should fail\n");
    ok(TermSrvRdpBcgrParseClientInfoPayload(ShortFlags,
                                            sizeof(ShortFlags),
                                            &Parsed) == TermSrvRdpBcgrInvalidLength,
       "Short Client Info flags should fail\n");
}

static VOID
TestRdpBcgrParseMcsSendDataPayload(VOID)
{
    static const UCHAR SendData[] =
    {
        0x03, 0x00, 0x00, 0x12,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xef, 0x70, 0x00, 0x03,
        0xaa, 0xbb, 0xcc
    };
    static const UCHAR LongFormSendDataLength[] =
    {
        0x03, 0x00, 0x00, 0x12,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xef, 0x70, 0x80, 0x03,
        0xaa, 0xbb, 0xcc
    };
    static const UCHAR BadPayloadLength[] =
    {
        0x03, 0x00, 0x00, 0x12,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xef, 0x70, 0x00, 0x04,
        0xaa, 0xbb, 0xcc
    };
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD Parsed;

    ok(TermSrvRdpBcgrParseMcsSendDataPayload(SendData,
                                             sizeof(SendData),
                                             &Parsed) == TermSrvRdpBcgrSuccess,
       "MCS Send Data payload parse failed\n");
    ok(Parsed.Initiator == 1001,
       "Unexpected MCS Send Data initiator %u\n", Parsed.Initiator);
    ok(Parsed.ChannelId == 1007,
       "Unexpected MCS Send Data channel %u\n", Parsed.ChannelId);
    ok(Parsed.Priority == 0x70,
       "Unexpected MCS Send Data priority 0x%x\n", Parsed.Priority);
    ok(Parsed.Payload == &SendData[15],
       "Unexpected MCS Send Data payload pointer\n");
    ok(Parsed.PayloadLength == 3,
       "Unexpected MCS Send Data payload length %Iu\n", Parsed.PayloadLength);
    ok(memcmp(Parsed.Payload, "\xaa\xbb\xcc", Parsed.PayloadLength) == 0,
       "Unexpected MCS Send Data payload bytes\n");

    ok(TermSrvRdpBcgrParseMcsSendDataPayload(LongFormSendDataLength,
                                             sizeof(LongFormSendDataLength),
                                             &Parsed) == TermSrvRdpBcgrSuccess,
       "MCS Send Data long-form length parse failed\n");
    ok(Parsed.PayloadLength == 3,
       "Unexpected long-form MCS Send Data payload length %Iu\n", Parsed.PayloadLength);
    ok(memcmp(Parsed.Payload, "\xaa\xbb\xcc", Parsed.PayloadLength) == 0,
       "Unexpected long-form MCS Send Data payload bytes\n");

    ok(TermSrvRdpBcgrParseMcsSendDataPayload(NULL,
                                             sizeof(SendData),
                                             &Parsed) == TermSrvRdpBcgrInvalidHeader,
       "NULL MCS Send Data buffer should fail\n");
    ok(TermSrvRdpBcgrParseMcsSendDataPayload(SendData,
                                             sizeof(SendData),
                                             NULL) == TermSrvRdpBcgrInvalidHeader,
       "NULL MCS Send Data output should fail\n");
    ok(TermSrvRdpBcgrParseMcsSendDataPayload(BadPayloadLength,
                                             sizeof(BadPayloadLength),
                                             &Parsed) == TermSrvRdpBcgrInvalidLength,
       "Bad MCS Send Data payload length should fail\n");
}

static VOID
TestRdpBcgrWriteMcsSendDataPayload(VOID)
{
    static const UCHAR Payload[] = { 0xaa, 0xbb, 0xcc };
    static const UCHAR ExpectedSendData[] =
    {
        0x03, 0x00, 0x00, 0x12,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xef, 0x70, 0x00, 0x03,
        0xaa, 0xbb, 0xcc
    };
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD SendData;
    TERMSRV_RDPBCGR_MCS_SEND_DATA_PAYLOAD Parsed;
    UCHAR Buffer[sizeof(ExpectedSendData)];
    SIZE_T BytesWritten = 0;

    SendData.Initiator = 1001;
    SendData.ChannelId = 1007;
    SendData.Priority = 0x70;
    SendData.Payload = Payload;
    SendData.PayloadLength = sizeof(Payload);

    ok(TermSrvRdpBcgrWriteMcsSendDataPayload(Buffer,
                                             sizeof(Buffer),
                                             &SendData,
                                             &BytesWritten) == TermSrvRdpBcgrSuccess,
       "MCS Send Data payload serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedSendData),
       "Unexpected serialized MCS Send Data payload size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedSendData, sizeof(ExpectedSendData)) == 0,
       "Serialized MCS Send Data payload did not match expected bytes\n");

    ok(TermSrvRdpBcgrParseMcsSendDataPayload(Buffer,
                                             BytesWritten,
                                             &Parsed) == TermSrvRdpBcgrSuccess,
       "Serialized MCS Send Data payload did not parse\n");
    ok(Parsed.Initiator == SendData.Initiator,
       "Round-tripped MCS Send Data initiator mismatch %u\n", Parsed.Initiator);
    ok(Parsed.ChannelId == SendData.ChannelId,
       "Round-tripped MCS Send Data channel mismatch %u\n", Parsed.ChannelId);
    ok(Parsed.Priority == SendData.Priority,
       "Round-tripped MCS Send Data priority mismatch 0x%x\n", Parsed.Priority);
    ok(Parsed.PayloadLength == SendData.PayloadLength,
       "Round-tripped MCS Send Data payload length mismatch %Iu\n", Parsed.PayloadLength);
    ok(memcmp(Parsed.Payload, Payload, sizeof(Payload)) == 0,
       "Round-tripped MCS Send Data payload mismatch\n");

    ok(TermSrvRdpBcgrWriteMcsSendDataPayload(Buffer,
                                             sizeof(ExpectedSendData) - 1,
                                             &SendData,
                                             &BytesWritten) == TermSrvRdpBcgrBufferTooSmall,
       "Short MCS Send Data output buffer should fail\n");
    ok(BytesWritten == 0,
       "Short MCS Send Data write returned %Iu bytes\n", BytesWritten);

    ok(TermSrvRdpBcgrWriteMcsSendDataPayload(NULL,
                                             sizeof(Buffer),
                                             &SendData,
                                             &BytesWritten) == TermSrvRdpBcgrInvalidHeader,
       "NULL MCS Send Data output buffer should fail\n");
    ok(BytesWritten == 0,
       "NULL MCS Send Data output buffer wrote %Iu bytes\n", BytesWritten);

    ok(TermSrvRdpBcgrWriteMcsSendDataPayload(Buffer,
                                             sizeof(Buffer),
                                             NULL,
                                             &BytesWritten) == TermSrvRdpBcgrInvalidHeader,
       "NULL MCS Send Data descriptor should fail\n");
    ok(BytesWritten == 0,
       "NULL MCS Send Data descriptor wrote %Iu bytes\n", BytesWritten);

    ok(TermSrvRdpBcgrWriteMcsSendDataPayload(Buffer,
                                             sizeof(Buffer),
                                             &SendData,
                                             NULL) == TermSrvRdpBcgrInvalidHeader,
       "NULL MCS Send Data byte count should fail\n");

    SendData.Payload = NULL;
    SendData.PayloadLength = sizeof(Payload);
    ok(TermSrvRdpBcgrWriteMcsSendDataPayload(Buffer,
                                             sizeof(Buffer),
                                             &SendData,
                                             &BytesWritten) == TermSrvRdpBcgrInvalidHeader,
       "NULL non-empty MCS Send Data payload should fail\n");
    ok(BytesWritten == 0,
       "NULL non-empty MCS Send Data payload wrote %Iu bytes\n", BytesWritten);

    SendData.Payload = Payload;
    SendData.PayloadLength = 0xFFFF;
    ok(TermSrvRdpBcgrWriteMcsSendDataPayload(Buffer,
                                             sizeof(Buffer),
                                             &SendData,
                                             &BytesWritten) == TermSrvRdpBcgrInvalidLength,
       "Oversized MCS Send Data payload should fail\n");
    ok(BytesWritten == 0,
       "Oversized MCS Send Data payload wrote %Iu bytes\n", BytesWritten);
}

static VOID
TestRdpBcgrParseInputEvents(VOID)
{
    static const UCHAR InputEvents[] =
    {
        0x01, 0x00, 0x00, 0x00,
        0x78, 0x56, 0x34, 0x12,
        0x04, 0x00,
        0x1e, 0x00,
        0x1e, 0x00, 0x00, 0x00
    };
    static const UCHAR TwoInputEvents[] =
    {
        0x02, 0x00, 0x00, 0x00,
        0x78, 0x56, 0x34, 0x12,
        0x04, 0x00,
        0x1e, 0x00,
        0x1e, 0x00, 0x00, 0x00,
        0x79, 0x56, 0x34, 0x12,
        0x04, 0x00,
        0x9e, 0x00,
        0x1e, 0x00, 0x00, 0x00
    };
    static const UCHAR ExtraByte[] =
    {
        0x00, 0x00, 0x00, 0x00, 0xff
    };
    static const UCHAR MouseInputEvent[] =
    {
        0x01, 0x00, 0x00, 0x00,
        0x79, 0x56, 0x34, 0x12,
        0x01, 0x80,
        0x00, 0x08,
        0x34, 0x12,
        0x78, 0x56
    };
    TERMSRV_RDPBCGR_INPUT_EVENTS Parsed;

    ok(TermSrvRdpBcgrParseInputEvents(InputEvents,
                                      sizeof(InputEvents),
                                      &Parsed) == TermSrvRdpBcgrSuccess,
       "Input event parse failed\n");
    ok(Parsed.NumberEvents == 1,
       "Unexpected input event count %u\n", Parsed.NumberEvents);
    ok(Parsed.FirstEventTime == 0x12345678,
       "Unexpected first input event time 0x%lx\n", Parsed.FirstEventTime);
    ok(Parsed.FirstMessageType == 0x0004,
       "Unexpected first input message type 0x%x\n", Parsed.FirstMessageType);
    ok(Parsed.FirstDeviceFlags == 0x001e,
       "Unexpected first input device flags 0x%x\n", Parsed.FirstDeviceFlags);
    ok(Parsed.FirstKeyboardCode == 0x001e,
       "Unexpected first input keyboard code 0x%x\n", Parsed.FirstKeyboardCode);

    ok(TermSrvRdpBcgrParseInputEvents(TwoInputEvents,
                                      sizeof(TwoInputEvents),
                                      &Parsed) == TermSrvRdpBcgrSuccess,
       "Two input events should parse\n");
    ok(Parsed.NumberEvents == 2,
       "Unexpected two-event input count %u\n", Parsed.NumberEvents);
    ok(Parsed.FirstDeviceFlags == 0x001e,
       "Unexpected first two-event input flags 0x%x\n", Parsed.FirstDeviceFlags);
    ok(TermSrvRdpBcgrParseInputEvents(MouseInputEvent,
                                      sizeof(MouseInputEvent),
                                      &Parsed) == TermSrvRdpBcgrSuccess,
       "Mouse input event should parse\n");
    ok(Parsed.FirstMessageType == 0x8001,
       "Unexpected mouse input message type 0x%x\n", Parsed.FirstMessageType);
    ok(Parsed.FirstDeviceFlags == 0x0800,
       "Unexpected mouse input flags 0x%x\n", Parsed.FirstDeviceFlags);
    ok(Parsed.FirstPointerX == 0x1234,
       "Unexpected mouse input X %u\n", Parsed.FirstPointerX);
    ok(Parsed.FirstPointerY == 0x5678,
       "Unexpected mouse input Y %u\n", Parsed.FirstPointerY);

    ok(TermSrvRdpBcgrParseInputEvents(InputEvents,
                                      3,
                                      &Parsed) == TermSrvRdpBcgrNeedMoreData,
       "Short input event header should request more data\n");
    ok(TermSrvRdpBcgrParseInputEvents(InputEvents,
                                      sizeof(InputEvents) - 1,
                                      &Parsed) == TermSrvRdpBcgrNeedMoreData,
       "Short input event body should request more data\n");
    ok(TermSrvRdpBcgrParseInputEvents(ExtraByte,
                                      sizeof(ExtraByte),
                                      &Parsed) == TermSrvRdpBcgrInvalidLength,
       "Trailing input event bytes should fail\n");
    ok(TermSrvRdpBcgrParseInputEvents(NULL,
                                      sizeof(InputEvents),
                                      &Parsed) == TermSrvRdpBcgrInvalidHeader,
       "NULL input event buffer should fail\n");
    ok(TermSrvRdpBcgrParseInputEvents(InputEvents,
                                      sizeof(InputEvents),
                                      NULL) == TermSrvRdpBcgrInvalidHeader,
       "NULL input event output should fail\n");
}

static VOID
TestRdpBcgrParseShareDataInputPdu(VOID)
{
    static const UCHAR SlowPathInput[] =
    {
        0x03, 0x00, 0x00, 0x31,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x22,
        0x22, 0x00,
        0x17, 0x00,
        0xe9, 0x03,
        0xe9, 0x03, 0x01, 0x00,
        0x00, 0x00,
        0x10, 0x00,
        0x1c, 0x00,
        0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x78, 0x56, 0x34, 0x12,
        0x04, 0x00,
        0x1e, 0x00,
        0x1e, 0x00, 0x00, 0x00
    };
    static const UCHAR ControlPdu[] =
    {
        0x03, 0x00, 0x00, 0x29,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x1a,
        0x1a, 0x00,
        0x17, 0x00,
        0xe9, 0x03,
        0xe9, 0x03, 0x01, 0x00,
        0x00, 0x00,
        0x08, 0x00,
        0x14, 0x00,
        0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR BadBodyLength[] =
    {
        0x03, 0x00, 0x00, 0x31,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xeb, 0x70, 0x00, 0x22,
        0x22, 0x00,
        0x17, 0x00,
        0xe9, 0x03,
        0xe9, 0x03, 0x01, 0x00,
        0x00, 0x00,
        0x11, 0x00,
        0x1c, 0x00,
        0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x78, 0x56, 0x34, 0x12,
        0x04, 0x00,
        0x1e, 0x00,
        0x1e, 0x00, 0x00, 0x00
    };
    TERMSRV_RDPBCGR_SHARE_DATA_PDU ShareData;
    TERMSRV_RDPBCGR_INPUT_EVENTS InputEvents;

    ok(TermSrvRdpBcgrParseShareDataPdu(SlowPathInput,
                                       sizeof(SlowPathInput),
                                       &ShareData) == TermSrvRdpBcgrSuccess,
       "Share Data input PDU parse failed\n");
    ok(ShareData.Initiator == 1001,
       "Unexpected Share Data initiator %u\n", ShareData.Initiator);
    ok(ShareData.ChannelId == 1003,
       "Unexpected Share Data channel %u\n", ShareData.ChannelId);
    ok(ShareData.PduType == 0x07,
       "Unexpected Share Data PDU type 0x%x\n", ShareData.PduType);
    ok(ShareData.DataType == 0x1c,
       "Unexpected Share Data data type 0x%x\n", ShareData.DataType);
    ok(ShareData.BodyLength == 16,
       "Unexpected Share Data body length %u\n", ShareData.BodyLength);

    ok(TermSrvRdpBcgrParseSlowPathInputPdu(SlowPathInput,
                                           sizeof(SlowPathInput),
                                           &InputEvents) == TermSrvRdpBcgrSuccess,
       "Slow-path input PDU parse failed\n");
    ok(InputEvents.NumberEvents == 1,
       "Unexpected slow-path input event count %u\n", InputEvents.NumberEvents);
    ok(InputEvents.FirstMessageType == 0x0004,
       "Unexpected slow-path input message type 0x%x\n", InputEvents.FirstMessageType);
    ok(InputEvents.FirstKeyboardCode == 0x001e,
       "Unexpected slow-path input keyboard code 0x%x\n", InputEvents.FirstKeyboardCode);

    ok(TermSrvRdpBcgrParseShareDataPdu(ControlPdu,
                                       sizeof(ControlPdu),
                                       &ShareData) == TermSrvRdpBcgrSuccess,
       "Share Data control PDU parse failed\n");
    ok(ShareData.DataType == 0x14,
       "Unexpected control data type 0x%x\n", ShareData.DataType);
    ok(ShareData.ControlAction == 1,
       "Unexpected control action %u\n", ShareData.ControlAction);
    ok(TermSrvRdpBcgrParseSlowPathInputPdu(ControlPdu,
                                           sizeof(ControlPdu),
                                           &InputEvents) == TermSrvRdpBcgrUnsupportedPdu,
       "Control PDU should not parse as slow-path input\n");
    ok(TermSrvRdpBcgrParseShareDataPdu(BadBodyLength,
                                       sizeof(BadBodyLength),
                                       &ShareData) == TermSrvRdpBcgrInvalidLength,
       "Bad Share Data body length should fail\n");
    ok(TermSrvRdpBcgrParseShareDataPdu(NULL,
                                       sizeof(SlowPathInput),
                                       &ShareData) == TermSrvRdpBcgrInvalidHeader,
       "NULL Share Data buffer should fail\n");
    ok(TermSrvRdpBcgrParseShareDataPdu(SlowPathInput,
                                       sizeof(SlowPathInput),
                                       NULL) == TermSrvRdpBcgrInvalidHeader,
       "NULL Share Data output should fail\n");
}

static VOID
TestRdpBcgrParseFastPathInputEvents(VOID)
{
    static const UCHAR MouseInput[] =
    {
        0x04, 0x09,
        0x20,
        0x00, 0x08,
        0x34, 0x12,
        0x78, 0x56
    };
    static const UCHAR LongLengthMouseInput[] =
    {
        0x04, 0x80, 0x0a,
        0x20,
        0x00, 0x08,
        0x34, 0x12,
        0x78, 0x56
    };
    static const UCHAR ShortMouseInput[] =
    {
        0x04, 0x09,
        0x20,
        0x00, 0x08,
        0x34, 0x12
    };
    static const UCHAR KeyboardInput[] =
    {
        0x04, 0x04,
        0x01,
        0x1e
    };
    TERMSRV_RDPBCGR_FASTPATH_INPUT_EVENTS Parsed;

    ok(TermSrvRdpBcgrParseFastPathInputEvents(MouseInput,
                                              sizeof(MouseInput),
                                              &Parsed) == TermSrvRdpBcgrSuccess,
       "Fast-path mouse input should parse\n");
    ok(Parsed.NumberEvents == 1,
       "Unexpected fast-path event count %u\n", Parsed.NumberEvents);
    ok(Parsed.FirstEventCode == 1,
       "Unexpected fast-path event code %u\n", Parsed.FirstEventCode);
    ok(Parsed.FirstEventFlags == 0x0800,
       "Unexpected fast-path pointer flags 0x%x\n", Parsed.FirstEventFlags);
    ok(Parsed.FirstPointerX == 0x1234,
       "Unexpected fast-path pointer X %u\n", Parsed.FirstPointerX);
    ok(Parsed.FirstPointerY == 0x5678,
       "Unexpected fast-path pointer Y %u\n", Parsed.FirstPointerY);

    ok(TermSrvRdpBcgrParseFastPathInputEvents(LongLengthMouseInput,
                                              sizeof(LongLengthMouseInput),
                                              &Parsed) == TermSrvRdpBcgrSuccess,
       "Long-length fast-path mouse input should parse\n");
    ok(TermSrvRdpBcgrParseFastPathInputEvents(KeyboardInput,
                                              sizeof(KeyboardInput),
                                              &Parsed) == TermSrvRdpBcgrSuccess,
       "Fast-path keyboard input should parse\n");
    ok(Parsed.FirstEventCode == 0 && Parsed.FirstEventFlags == 1 && Parsed.FirstKeyboardCode == 0x1e,
       "Unexpected fast-path keyboard event code=%u flags=0x%x key=0x%x\n",
       Parsed.FirstEventCode,
       Parsed.FirstEventFlags,
       Parsed.FirstKeyboardCode);
    ok(TermSrvRdpBcgrParseFastPathInputEvents(ShortMouseInput,
                                              sizeof(ShortMouseInput),
                                              &Parsed) == TermSrvRdpBcgrNeedMoreData,
       "Short fast-path mouse input should request more data\n");
    ok(TermSrvRdpBcgrParseFastPathInputEvents(NULL,
                                              sizeof(MouseInput),
                                              &Parsed) == TermSrvRdpBcgrInvalidHeader,
       "NULL fast-path input buffer should fail\n");
    ok(TermSrvRdpBcgrParseFastPathInputEvents(MouseInput,
                                              sizeof(MouseInput),
                                              NULL) == TermSrvRdpBcgrInvalidHeader,
       "NULL fast-path input output should fail\n");
}

static VOID
TestRdpBcgrWriteMcsChannelJoinConfirm(VOID)
{
    static const UCHAR ExpectedConfirm[] =
    {
        0x03, 0x00, 0x00, 0x0f,
        0x02, 0xf0, 0x80,
        0x3e, 0x00, 0x03, 0xe9, 0x03, 0xeb, 0x03, 0xeb
    };
    TERMSRV_RDPBCGR_MCS_CHANNEL_JOIN_CONFIRM Confirm;
    UCHAR Buffer[16];
    SIZE_T BytesWritten = 0;

    Confirm.Initiator = 1001;
    Confirm.RequestedChannelId = 1003;
    Confirm.ConfirmedChannelId = 1003;

    ok(TermSrvRdpBcgrWriteMcsChannelJoinConfirm(Buffer,
                                                sizeof(Buffer),
                                                &Confirm,
                                                &BytesWritten) == TermSrvRdpBcgrSuccess,
       "MCS Channel Join Confirm serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedConfirm),
       "Unexpected serialized MCS Channel Join Confirm size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedConfirm, sizeof(ExpectedConfirm)) == 0,
       "Serialized MCS Channel Join Confirm did not match expected bytes\n");

    ok(TermSrvRdpBcgrWriteMcsChannelJoinConfirm(Buffer,
                                                sizeof(ExpectedConfirm) - 1,
                                                &Confirm,
                                                &BytesWritten) == TermSrvRdpBcgrBufferTooSmall,
       "Short MCS Channel Join Confirm output buffer should fail\n");
    ok(BytesWritten == 0,
       "Short MCS Channel Join Confirm write returned %Iu bytes\n", BytesWritten);
}

static VOID
TestRdpBcgrParseStaticChannelList(VOID)
{
    static const UCHAR ChannelList[] =
    {
        0x03,
        'r', 'd', 'p', 'd', 'r', 0x00, 0x00, 0x00,
        0x04, 0x03, 0x02, 0x01,
        'c', 'l', 'i', 'p', 'r', 'd', 'r', 0x00,
        0xd4, 0xc3, 0xb2, 0xa1,
        'd', 'r', 'd', 'y', 'n', 'v', 'c', 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR NoCliprdrList[] =
    {
        0x01,
        'r', 'd', 'p', 'd', 'r', 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR ShortList[] =
    {
        0x02,
        'r', 'd', 'p', 'd', 'r', 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR TooManyChannels[] =
    {
        TERMSRV_RDPBCGR_MAX_STATIC_CHANNELS + 1
    };
    static const UCHAR TrailingBytes[] =
    {
        0x01,
        'r', 'd', 'p', 'd', 'r', 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0xff
    };
    TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST Parsed;
    SIZE_T ChannelIndex = 99;

    ok(TermSrvRdpBcgrParseStaticChannelList(ChannelList,
                                            sizeof(ChannelList),
                                            &Parsed) == TermSrvRdpBcgrSuccess,
       "Static channel list parse failed\n");
    ok(Parsed.Count == 3,
       "Unexpected static channel count %Iu\n", Parsed.Count);
    ok(Parsed.Channels[0].Index == 0,
       "Unexpected first static channel index %Iu\n", Parsed.Channels[0].Index);
    ok(memcmp(Parsed.Channels[0].Name, "rdpdr", 5) == 0,
       "Unexpected first static channel name\n");
    ok(Parsed.Channels[0].Name[5] == 0 &&
       Parsed.Channels[0].Name[6] == 0 &&
       Parsed.Channels[0].Name[7] == 0,
       "First static channel name was not NUL padded\n");
    ok(Parsed.Channels[0].Options == 0x01020304,
       "Unexpected first static channel options 0x%lx\n", Parsed.Channels[0].Options);
    ok(Parsed.Channels[1].Index == 1,
       "Unexpected cliprdr static channel index %Iu\n", Parsed.Channels[1].Index);
    ok(memcmp(Parsed.Channels[1].Name, "cliprdr", 7) == 0,
       "Unexpected cliprdr static channel name\n");
    ok(Parsed.Channels[1].Name[7] == 0,
       "cliprdr static channel name was not NUL padded\n");
    ok(Parsed.Channels[1].Options == 0xa1b2c3d4,
       "Unexpected cliprdr static channel options 0x%lx\n", Parsed.Channels[1].Options);

    ok(TermSrvRdpBcgrFindStaticChannelByName(&Parsed,
                                             "rdpdr",
                                             5,
                                             &ChannelIndex),
       "Generic static channel lookup did not find rdpdr\n");
    ok(ChannelIndex == 0,
       "Generic static channel lookup returned index %Iu\n", ChannelIndex);

    ChannelIndex = 99;
    ok(TermSrvCliprdrFindStaticChannel(&Parsed, &ChannelIndex),
       "cliprdr static channel lookup failed\n");
    ok(ChannelIndex == 1,
       "cliprdr static channel lookup returned index %Iu\n", ChannelIndex);

    ok(!TermSrvRdpBcgrFindStaticChannelByName(&Parsed,
                                              "rdpsnd",
                                              6,
                                              &ChannelIndex),
       "Generic static channel lookup unexpectedly found rdpsnd\n");

    ok(TermSrvRdpBcgrParseStaticChannelList(NoCliprdrList,
                                            sizeof(NoCliprdrList),
                                            &Parsed) == TermSrvRdpBcgrSuccess,
       "Static channel list without cliprdr did not parse\n");
    ok(!TermSrvCliprdrFindStaticChannel(&Parsed, &ChannelIndex),
       "cliprdr lookup unexpectedly matched a list without cliprdr\n");

    ok(TermSrvRdpBcgrParseStaticChannelList(NULL,
                                            sizeof(ChannelList),
                                            &Parsed) == TermSrvRdpBcgrInvalidHeader,
       "NULL static channel list buffer should fail\n");
    ok(TermSrvRdpBcgrParseStaticChannelList(ChannelList,
                                            sizeof(ChannelList),
                                            NULL) == TermSrvRdpBcgrInvalidHeader,
       "NULL static channel list output should fail\n");
    ok(TermSrvRdpBcgrParseStaticChannelList(ChannelList,
                                            0,
                                            &Parsed) == TermSrvRdpBcgrNeedMoreData,
       "Empty static channel list buffer should request more data\n");
    ok(TermSrvRdpBcgrParseStaticChannelList(ShortList,
                                            sizeof(ShortList),
                                            &Parsed) == TermSrvRdpBcgrNeedMoreData,
       "Short static channel list should request more data\n");
    ok(TermSrvRdpBcgrParseStaticChannelList(TooManyChannels,
                                            sizeof(TooManyChannels),
                                            &Parsed) == TermSrvRdpBcgrInvalidLength,
       "Too many static channels should fail\n");
    ok(TermSrvRdpBcgrParseStaticChannelList(TrailingBytes,
                                            sizeof(TrailingBytes),
                                            &Parsed) == TermSrvRdpBcgrInvalidLength,
       "Static channel list with trailing bytes should fail\n");

    ok(!TermSrvRdpBcgrFindStaticChannelByName(NULL,
                                              "rdpdr",
                                              5,
                                              &ChannelIndex),
       "NULL static channel list lookup should fail\n");
    ok(!TermSrvRdpBcgrFindStaticChannelByName(&Parsed,
                                              NULL,
                                              5,
                                              &ChannelIndex),
       "NULL static channel name lookup should fail\n");
    ok(!TermSrvRdpBcgrFindStaticChannelByName(&Parsed,
                                              "toolongname",
                                              11,
                                              &ChannelIndex),
       "Overlong static channel name lookup should fail\n");
    ok(!TermSrvRdpBcgrFindStaticChannelByName(&Parsed,
                                              "rdpdr",
                                              5,
                                              NULL),
       "NULL static channel lookup output should fail\n");
    ok(!TermSrvCliprdrFindStaticChannel(NULL, &ChannelIndex),
       "NULL cliprdr static channel list lookup should fail\n");
    ok(!TermSrvCliprdrFindStaticChannel(&Parsed, NULL),
       "NULL cliprdr static channel lookup output should fail\n");
}

static VOID
TestCliprdrStaticChannelName(VOID)
{
    static const CHAR FixedName[TERMSRV_CLIPRDR_MAX_CHANNEL_NAME_LENGTH] =
    {
        'c', 'l', 'i', 'p', 'r', 'd', 'r', '\0'
    };
    static const CHAR WrongName[TERMSRV_CLIPRDR_MAX_CHANNEL_NAME_LENGTH] =
    {
        'r', 'd', 'p', 'd', 'r', '\0', '\0', '\0'
    };
    static const CHAR PrefixName[TERMSRV_CLIPRDR_MAX_CHANNEL_NAME_LENGTH] =
    {
        'c', 'l', 'i', 'p', 'r', 'd', 'x', '\0'
    };
    static const CHAR UnpaddedName[TERMSRV_CLIPRDR_MAX_CHANNEL_NAME_LENGTH] =
    {
        'c', 'l', 'i', 'p', 'r', 'd', 'r', 'x'
    };

    ok(TermSrvCliprdrIsStaticChannelName(TERMSRV_CLIPRDR_CHANNEL_NAME,
                                         sizeof(TERMSRV_CLIPRDR_CHANNEL_NAME) - 1),
       "NUL-terminated cliprdr name without NUL length should match\n");
    ok(TermSrvCliprdrIsStaticChannelName(TERMSRV_CLIPRDR_CHANNEL_NAME,
                                         sizeof(TERMSRV_CLIPRDR_CHANNEL_NAME)),
       "NUL-terminated cliprdr name should match\n");
    ok(TermSrvCliprdrIsStaticChannelName(FixedName,
                                         sizeof(FixedName)),
       "Fixed-width NUL-padded cliprdr name should match\n");
    ok(!TermSrvCliprdrIsStaticChannelName(WrongName,
                                          sizeof(WrongName)),
       "Wrong static channel name should not match\n");
    ok(!TermSrvCliprdrIsStaticChannelName(PrefixName,
                                          sizeof(PrefixName)),
       "Different static channel prefix should not match\n");
    ok(!TermSrvCliprdrIsStaticChannelName(UnpaddedName,
                                          sizeof(UnpaddedName)),
       "Unpadded fixed-width cliprdr prefix should not match\n");
    ok(!TermSrvCliprdrIsStaticChannelName(NULL,
                                          sizeof(FixedName)),
       "NULL static channel name should not match\n");
    ok(!TermSrvCliprdrIsStaticChannelName(TERMSRV_CLIPRDR_CHANNEL_NAME,
                                          3),
       "Short static channel name should not match\n");
}

static VOID
TestCliprdrChannelDescriptor(VOID)
{
    TERMSRV_CLIPRDR_CHANNEL Channel;

    Channel.Enabled = TRUE;
    Channel.ChannelId = 1004;

    ok(TermSrvCliprdrChannelReset(NULL) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr channel reset should fail\n");
    ok(TermSrvCliprdrAssignChannelId(NULL, 1004) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr channel assignment should fail\n");

    ok(TermSrvCliprdrChannelInit(&Channel) == TermSrvCliprdrSuccess,
       "cliprdr channel init failed\n");
    ok(!Channel.Enabled,
       "cliprdr channel should be disabled after init\n");
    ok(Channel.ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID,
       "Unexpected cliprdr channel id after init %u\n", Channel.ChannelId);
    ok(!TermSrvCliprdrIsChannelId(&Channel, 1004),
       "Unassigned cliprdr channel id should not match\n");
    ok(!TermSrvCliprdrIsChannelId(NULL, 1004),
       "NULL cliprdr channel descriptor should not match\n");

    ok(TermSrvCliprdrAssignChannelId(&Channel,
                                     TERMSRV_CLIPRDR_INVALID_CHANNEL_ID) == TermSrvCliprdrInvalidHeader,
       "Invalid cliprdr channel id assignment should fail\n");
    ok(!Channel.Enabled,
       "Failed cliprdr channel assignment should not enable channel\n");

    ok(TermSrvCliprdrAssignChannelId(&Channel, 1007) == TermSrvCliprdrSuccess,
       "cliprdr channel assignment failed\n");
    ok(Channel.Enabled,
       "cliprdr channel should be enabled after assignment\n");
    ok(Channel.ChannelId == 1007,
       "Unexpected assigned cliprdr channel id %u\n", Channel.ChannelId);
    ok(TermSrvCliprdrIsChannelId(&Channel, 1007),
       "Assigned cliprdr channel id should match\n");
    ok(!TermSrvCliprdrIsChannelId(&Channel, 1008),
       "Different cliprdr channel id should not match\n");
    ok(!TermSrvCliprdrIsChannelId(&Channel,
                                  TERMSRV_CLIPRDR_INVALID_CHANNEL_ID),
       "Invalid cliprdr channel id should not match\n");

    ok(TermSrvCliprdrChannelReset(&Channel) == TermSrvCliprdrSuccess,
       "cliprdr channel reset failed\n");
    ok(!Channel.Enabled,
       "cliprdr channel should be disabled after reset\n");
    ok(Channel.ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID,
       "Unexpected cliprdr channel id after reset %u\n", Channel.ChannelId);
    ok(!TermSrvCliprdrIsChannelId(&Channel, 1007),
       "Reset cliprdr channel id should not match\n");
}

static VOID
TestCliprdrAssignFromStaticChannelList(VOID)
{
    static const UCHAR ChannelList[] =
    {
        0x03,
        'r', 'd', 'p', 'd', 'r', 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        'c', 'l', 'i', 'p', 'r', 'd', 'r', 0x00,
        0x00, 0x00, 0x00, 0x00,
        'r', 'd', 'p', 's', 'n', 'd', 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR NoCliprdrList[] =
    {
        0x01,
        'r', 'd', 'p', 'd', 'r', 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST Parsed;
    TERMSRV_RDPBCGR_STATIC_CHANNEL_LIST NoCliprdr;
    TERMSRV_CLIPRDR_CHANNEL Channel;

    ok(TermSrvRdpBcgrParseStaticChannelList(ChannelList,
                                            sizeof(ChannelList),
                                            &Parsed) == TermSrvRdpBcgrSuccess,
       "Static channel list parse failed\n");
    ok(TermSrvRdpBcgrParseStaticChannelList(NoCliprdrList,
                                            sizeof(NoCliprdrList),
                                            &NoCliprdr) == TermSrvRdpBcgrSuccess,
       "Static channel list without cliprdr parse failed\n");

    ok(TermSrvCliprdrChannelInit(&Channel) == TermSrvCliprdrSuccess,
       "cliprdr channel init failed\n");
    ok(TermSrvCliprdrAssignFromStaticChannelList(&Channel,
                                                 &Parsed,
                                                 1004) == TermSrvCliprdrSuccess,
       "cliprdr static channel derived assignment failed\n");
    ok(Channel.Enabled,
       "cliprdr channel should be enabled after derived assignment\n");
    ok(Channel.ChannelId == 1005,
       "Expected derived cliprdr channel id 1005, got %u\n", Channel.ChannelId);
    ok(TermSrvCliprdrIsChannelId(&Channel, 1005),
       "Derived cliprdr channel id should match\n");

    ok(TermSrvCliprdrAssignFromStaticChannelList(NULL,
                                                 &Parsed,
                                                 1004) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr channel derived assignment should fail\n");

    ok(TermSrvCliprdrAssignChannelId(&Channel, 1007) == TermSrvCliprdrSuccess,
       "cliprdr channel assignment before NULL list failed\n");
    ok(TermSrvCliprdrAssignFromStaticChannelList(&Channel,
                                                 NULL,
                                                 1004) == TermSrvCliprdrInvalidHeader,
       "NULL static channel list derived assignment should fail\n");
    ok(!Channel.Enabled &&
       Channel.ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID,
       "NULL static channel list failure should disable cliprdr channel\n");

    ok(TermSrvCliprdrAssignChannelId(&Channel, 1007) == TermSrvCliprdrSuccess,
       "cliprdr channel assignment before base validation failed\n");
    ok(TermSrvCliprdrAssignFromStaticChannelList(&Channel,
                                                 &Parsed,
                                                 TERMSRV_CLIPRDR_INVALID_CHANNEL_ID) ==
       TermSrvCliprdrInvalidHeader,
       "Invalid first static channel id should fail\n");
    ok(!Channel.Enabled &&
       Channel.ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID,
       "Invalid first static channel id failure should disable cliprdr channel\n");

    ok(TermSrvCliprdrAssignChannelId(&Channel, 1007) == TermSrvCliprdrSuccess,
       "cliprdr channel assignment before missing channel failed\n");
    ok(TermSrvCliprdrAssignFromStaticChannelList(&Channel,
                                                 &NoCliprdr,
                                                 1004) == TermSrvCliprdrUnsupportedPdu,
       "Missing cliprdr static channel should be unsupported\n");
    ok(!Channel.Enabled &&
       Channel.ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID,
       "Missing cliprdr static channel should disable cliprdr channel\n");

    ok(TermSrvCliprdrAssignChannelId(&Channel, 1007) == TermSrvCliprdrSuccess,
       "cliprdr channel assignment before overflow failed\n");
    ok(TermSrvCliprdrAssignFromStaticChannelList(&Channel,
                                                 &Parsed,
                                                 0xffff) == TermSrvCliprdrInvalidLength,
       "Overflowing derived cliprdr channel id should fail\n");
    ok(!Channel.Enabled &&
       Channel.ChannelId == TERMSRV_CLIPRDR_INVALID_CHANNEL_ID,
       "Overflowing derived assignment should disable cliprdr channel\n");
}

static VOID
TestCliprdrParsePdu(VOID)
{
    static const UCHAR FormatList[] =
    {
        0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x11, 0x22, 0x33
    };
    static const UCHAR ShortHeader[] =
    {
        0x02, 0x00, 0x00, 0x00, 0x03
    };
    static const UCHAR NeedMoreData[] =
    {
        0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x11, 0x22, 0x33
    };
    static const UCHAR LengthMismatch[] =
    {
        0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x11, 0x22, 0x33
    };
    TERMSRV_CLIPRDR_PDU Pdu;

    ok(TermSrvCliprdrParsePdu(FormatList,
                              sizeof(FormatList),
                              &Pdu) == TermSrvCliprdrSuccess,
       "cliprdr Format List PDU parse failed\n");
    ok(Pdu.MsgType == TERMSRV_CLIPRDR_CB_FORMAT_LIST,
       "Unexpected cliprdr msgType 0x%x\n", Pdu.MsgType);
    ok(Pdu.MsgFlags == 0,
       "Unexpected cliprdr msgFlags 0x%x\n", Pdu.MsgFlags);
    ok(Pdu.DataLength == 3,
       "Unexpected cliprdr dataLen %lu\n", Pdu.DataLength);
    ok(Pdu.Payload == &FormatList[8],
       "Unexpected cliprdr payload pointer\n");
    ok(memcmp(Pdu.Payload, "\x11\x22\x33", Pdu.DataLength) == 0,
       "Unexpected cliprdr payload bytes\n");

    ok(TermSrvCliprdrParsePdu(NULL,
                              sizeof(FormatList),
                              &Pdu) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr buffer should fail\n");
    ok(TermSrvCliprdrParsePdu(FormatList,
                              sizeof(FormatList),
                              NULL) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr output should fail\n");
    ok(TermSrvCliprdrParsePdu(ShortHeader,
                              sizeof(ShortHeader),
                              &Pdu) == TermSrvCliprdrNeedMoreData,
       "Short cliprdr header should request more data\n");
    ok(TermSrvCliprdrParsePdu(NeedMoreData,
                              sizeof(NeedMoreData),
                              &Pdu) == TermSrvCliprdrNeedMoreData,
       "Short cliprdr payload should request more data\n");
    ok(TermSrvCliprdrParsePdu(LengthMismatch,
                              sizeof(LengthMismatch),
                              &Pdu) == TermSrvCliprdrInvalidLength,
       "Trailing cliprdr bytes should fail\n");
}

static VOID
TestCliprdrWriteMonitorReady(VOID)
{
    static const UCHAR Expected[] =
    {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    UCHAR Buffer[16];
    SIZE_T BytesWritten = 0;

    ok(TermSrvCliprdrWriteMonitorReady(Buffer,
                                       sizeof(Buffer),
                                       &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Monitor Ready serialization failed\n");
    ok(BytesWritten == sizeof(Expected),
       "Unexpected cliprdr Monitor Ready size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, Expected, sizeof(Expected)) == 0,
       "Serialized cliprdr Monitor Ready did not match expected bytes\n");

    ok(TermSrvCliprdrWriteMonitorReady(Buffer,
                                       sizeof(Expected) - 1,
                                       &BytesWritten) == TermSrvCliprdrBufferTooSmall,
       "Short cliprdr Monitor Ready buffer should fail\n");
    ok(BytesWritten == 0,
       "Short cliprdr Monitor Ready write returned %Iu bytes\n", BytesWritten);
}

static VOID
TestCliprdrWriteFormatListResponse(VOID)
{
    static const UCHAR ExpectedOk[] =
    {
        0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR ExpectedFail[] =
    {
        0x03, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    UCHAR Buffer[16];
    SIZE_T BytesWritten = 0;

    ok(TermSrvCliprdrWriteFormatListResponse(Buffer,
                                             sizeof(Buffer),
                                             TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                                             &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Format List Response OK serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedOk),
       "Unexpected cliprdr Format List Response OK size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedOk, sizeof(ExpectedOk)) == 0,
       "Serialized cliprdr Format List Response OK did not match expected bytes\n");

    ok(TermSrvCliprdrWriteFormatListResponse(Buffer,
                                             sizeof(Buffer),
                                             TERMSRV_CLIPRDR_CB_RESPONSE_FAIL,
                                             &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Format List Response FAIL serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedFail),
       "Unexpected cliprdr Format List Response FAIL size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedFail, sizeof(ExpectedFail)) == 0,
       "Serialized cliprdr Format List Response FAIL did not match expected bytes\n");

    ok(TermSrvCliprdrWriteFormatListResponse(Buffer,
                                             sizeof(Buffer),
                                             0,
                                             &BytesWritten) == TermSrvCliprdrInvalidHeader,
       "Invalid cliprdr Format List Response flags should fail\n");
}

static VOID
TestCliprdrFormatListHelpers(VOID)
{
    static const UCHAR LongList[] =
    {
        0x02, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
        0x0d, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x34, 0x12, 0x00, 0x00, 'F', 0x00, 'o', 0x00, 'o', 0x00, 0x00, 0x00
    };
    static const UCHAR ExpectedRequest[] =
    {
        0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x34, 0x12, 0x00, 0x00
    };
    TERMSRV_CLIPRDR_FORMAT_LIST List;
    TERMSRV_CLIPRDR_FORMAT Formats[2];
    UCHAR Buffer[64];
    SIZE_T BytesWritten;

    ok(TermSrvCliprdrParseFormatList(LongList,
                                     sizeof(LongList),
                                     &List) == TermSrvCliprdrSuccess,
       "cliprdr long Format List parse failed\n");
    ok(List.Count == 2,
       "Unexpected cliprdr long Format List count %lu\n", List.Count);
    ok(TermSrvCliprdrFormatListContains(&List, TERMSRV_CLIPRDR_CF_UNICODETEXT),
       "cliprdr long Format List did not include CF_UNICODETEXT\n");
    ok(TermSrvCliprdrFindNamedFormat(&List, L"Foo") == 0x1234,
       "cliprdr named format lookup failed\n");

    ZeroMemory(Formats, sizeof(Formats));
    Formats[0].FormatId = TERMSRV_CLIPRDR_CF_UNICODETEXT;
    Formats[1].FormatId = 0x1234;
    lstrcpyW(Formats[1].Name, L"Foo");
    ok(TermSrvCliprdrWriteFormatList(Buffer,
                                     sizeof(Buffer),
                                     Formats,
                                     2,
                                     &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Format List serialization failed\n");
    ok(BytesWritten == sizeof(LongList),
       "Unexpected cliprdr Format List size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, LongList, sizeof(LongList)) == 0,
       "Serialized cliprdr Format List did not match expected bytes\n");

    ok(TermSrvCliprdrWriteFormatDataRequest(Buffer,
                                            sizeof(Buffer),
                                            0x1234,
                                            &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Format Data Request serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedRequest),
       "Unexpected cliprdr Format Data Request size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedRequest, sizeof(ExpectedRequest)) == 0,
       "Serialized cliprdr Format Data Request did not match expected bytes\n");
}

static VOID
TestCliprdrParseFormatDataRequest(VOID)
{
    static const UCHAR Request[] =
    {
        0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x0d, 0x00, 0x00, 0x00
    };
    static const UCHAR BadType[] =
    {
        0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x0d, 0x00, 0x00, 0x00
    };
    static const UCHAR BadLength[] =
    {
        0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x0d, 0x00, 0x00
    };
    ULONG FormatId = 0;

    ok(TermSrvCliprdrParseFormatDataRequest(Request,
                                            sizeof(Request),
                                            &FormatId) == TermSrvCliprdrSuccess,
       "cliprdr Format Data Request parse failed\n");
    ok(FormatId == 13,
       "Unexpected cliprdr requested format id %lu\n", FormatId);

    ok(TermSrvCliprdrParseFormatDataRequest(Request,
                                            sizeof(Request),
                                            NULL) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr Format Data Request output should fail\n");
    ok(TermSrvCliprdrParseFormatDataRequest(BadType,
                                            sizeof(BadType),
                                            &FormatId) == TermSrvCliprdrUnsupportedPdu,
       "Wrong cliprdr Format Data Request type should fail\n");
    ok(TermSrvCliprdrParseFormatDataRequest(BadLength,
                                            sizeof(BadLength),
                                            &FormatId) == TermSrvCliprdrInvalidLength,
       "Bad cliprdr Format Data Request length should fail\n");
}

static VOID
TestCliprdrWriteFormatDataResponse(VOID)
{
    static const UCHAR Data[] =
    {
        't', 'e', 'x', 't'
    };
    static const UCHAR ExpectedOk[] =
    {
        0x05, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
        't', 'e', 'x', 't'
    };
    static const UCHAR ExpectedFail[] =
    {
        0x05, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    UCHAR Buffer[16];
    SIZE_T BytesWritten = 0;

    ok(TermSrvCliprdrWriteFormatDataResponse(Buffer,
                                             sizeof(Buffer),
                                             TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                                             Data,
                                             sizeof(Data),
                                             &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Format Data Response OK serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedOk),
       "Unexpected cliprdr Format Data Response OK size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedOk, sizeof(ExpectedOk)) == 0,
       "Serialized cliprdr Format Data Response OK did not match expected bytes\n");

    ok(TermSrvCliprdrWriteFormatDataResponse(Buffer,
                                             sizeof(Buffer),
                                             TERMSRV_CLIPRDR_CB_RESPONSE_FAIL,
                                             NULL,
                                             0,
                                             &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Format Data Response FAIL serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedFail),
       "Unexpected cliprdr Format Data Response FAIL size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedFail, sizeof(ExpectedFail)) == 0,
       "Serialized cliprdr Format Data Response FAIL did not match expected bytes\n");

    ok(TermSrvCliprdrWriteFormatDataResponse(Buffer,
                                             sizeof(ExpectedOk) - 1,
                                             TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                                             Data,
                                             sizeof(Data),
                                             &BytesWritten) == TermSrvCliprdrBufferTooSmall,
       "Short cliprdr Format Data Response buffer should fail\n");
    ok(BytesWritten == 0,
       "Short cliprdr Format Data Response write returned %Iu bytes\n", BytesWritten);

    ok(TermSrvCliprdrWriteFormatDataResponse(Buffer,
                                             sizeof(Buffer),
                                             TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                                             NULL,
                                             sizeof(Data),
                                             &BytesWritten) == TermSrvCliprdrInvalidHeader,
       "NULL non-empty cliprdr Format Data Response payload should fail\n");
}

static VOID
TestCliprdrFileListHelpers(VOID)
{
    TERMSRV_CLIPRDR_FILE_LIST List;
    TERMSRV_CLIPRDR_FILE_LIST Parsed;
    UCHAR Buffer[sizeof(ULONG) + 592];
    SIZE_T BytesWritten = 0;

    ZeroMemory(&List, sizeof(List));
    List.Count = 1;
    List.Descriptors[0].Flags = TERMSRV_CLIPRDR_FD_ATTRIBUTES |
                                TERMSRV_CLIPRDR_FD_FILESIZE |
                                TERMSRV_CLIPRDR_FD_UNICODE;
    List.Descriptors[0].Attributes = FILE_ATTRIBUTE_NORMAL;
    List.Descriptors[0].FileSizeHigh = 0x11223344;
    List.Descriptors[0].FileSizeLow = 0x55667788;
    lstrcpyW(List.Descriptors[0].FileName, L"foo.txt");

    ok(TermSrvCliprdrWriteFileList(Buffer,
                                   sizeof(Buffer),
                                   &List,
                                   &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr FileGroupDescriptorW write failed\n");
    ok(BytesWritten == sizeof(Buffer),
       "Unexpected FileGroupDescriptorW size %Iu\n", BytesWritten);
    ok(TermSrvCliprdrParseFileList(Buffer,
                                   BytesWritten,
                                   &Parsed) == TermSrvCliprdrSuccess,
       "cliprdr FileGroupDescriptorW parse failed\n");
    ok(Parsed.Count == 1,
       "Unexpected parsed file descriptor count %lu\n", Parsed.Count);
    ok(Parsed.Descriptors[0].Attributes == FILE_ATTRIBUTE_NORMAL,
       "Unexpected parsed file attributes 0x%lx\n", Parsed.Descriptors[0].Attributes);
    ok(Parsed.Descriptors[0].FileSizeHigh == 0x11223344 &&
       Parsed.Descriptors[0].FileSizeLow == 0x55667788,
       "Unexpected parsed file size\n");
    ok(lstrcmpW(Parsed.Descriptors[0].FileName, L"foo.txt") == 0,
       "Unexpected parsed file name\n");

    ok(TermSrvCliprdrParseFileList(Buffer,
                                   sizeof(ULONG) + 591,
                                   &Parsed) == TermSrvCliprdrInvalidLength,
       "Short FileGroupDescriptorW payload should fail\n");
}

static VOID
TestCliprdrFileContentsHelpers(VOID)
{
    TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST Request;
    TERMSRV_CLIPRDR_FILE_CONTENTS_REQUEST ParsedRequest;
    TERMSRV_CLIPRDR_FILE_CONTENTS_RESPONSE ParsedResponse;
    UCHAR Buffer[64];
    UCHAR Data[] = { 'a', 'b', 'c' };
    SIZE_T BytesWritten = 0;

    ZeroMemory(&Request, sizeof(Request));
    Request.StreamId = 0x1234;
    Request.ListIndex = 2;
    Request.Flags = TERMSRV_CLIPRDR_FILECONTENTS_SIZE;
    Request.Requested = sizeof(ULONGLONG);

    ok(TermSrvCliprdrWriteFileContentsRequest(Buffer,
                                              sizeof(Buffer),
                                              &Request,
                                              &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr FileContents SIZE request write failed\n");
    ok(TermSrvCliprdrParseFileContentsRequest(Buffer,
                                              BytesWritten,
                                              &ParsedRequest) == TermSrvCliprdrSuccess,
       "cliprdr FileContents SIZE request parse failed\n");
    ok(ParsedRequest.StreamId == 0x1234 &&
       ParsedRequest.ListIndex == 2 &&
       ParsedRequest.Flags == TERMSRV_CLIPRDR_FILECONTENTS_SIZE,
       "Unexpected parsed FileContents SIZE request\n");

    Request.Flags = TERMSRV_CLIPRDR_FILECONTENTS_RANGE;
    Request.PositionLow = 4;
    Request.Requested = 3;
    ok(TermSrvCliprdrWriteFileContentsRequest(Buffer,
                                              sizeof(Buffer),
                                              &Request,
                                              &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr FileContents RANGE request write failed\n");
    ok(TermSrvCliprdrParseFileContentsRequest(Buffer,
                                              BytesWritten,
                                              &ParsedRequest) == TermSrvCliprdrSuccess,
       "cliprdr FileContents RANGE request parse failed\n");
    ok(ParsedRequest.PositionLow == 4 && ParsedRequest.Requested == 3,
       "Unexpected parsed FileContents RANGE request\n");

    Request.Flags = TERMSRV_CLIPRDR_FILECONTENTS_SIZE |
                    TERMSRV_CLIPRDR_FILECONTENTS_RANGE;
    ok(TermSrvCliprdrWriteFileContentsRequest(Buffer,
                                              sizeof(Buffer),
                                              &Request,
                                              &BytesWritten) == TermSrvCliprdrInvalidHeader,
       "Invalid FileContents flags should fail\n");

    ok(TermSrvCliprdrWriteFileContentsResponse(Buffer,
                                               sizeof(Buffer),
                                               TERMSRV_CLIPRDR_CB_RESPONSE_OK,
                                               0x9876,
                                               Data,
                                               sizeof(Data),
                                               &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr FileContents response write failed\n");
    ok(TermSrvCliprdrParseFileContentsResponse(Buffer,
                                               BytesWritten,
                                               &ParsedResponse) == TermSrvCliprdrSuccess,
       "cliprdr FileContents response parse failed\n");
    ok(ParsedResponse.StreamId == 0x9876 &&
       ParsedResponse.DataLength == sizeof(Data) &&
       memcmp(ParsedResponse.Data, Data, sizeof(Data)) == 0,
       "Unexpected parsed FileContents response\n");
}

static VOID
TestCliprdrDummyBackend(VOID)
{
    static const UCHAR TextData[] =
    {
        'h', 0x00, 'i', 0x00, 0x00, 0x00
    };
    static const UCHAR BinaryData[] =
    {
        0xde, 0xad, 0xbe, 0xef
    };
    TERMSRV_CLIPRDR_DUMMY_BACKEND Dummy;
    TERMSRV_CLIPRDR_BACKEND Backend;
    UCHAR Buffer[sizeof(TextData)];
    SIZE_T RequiredLength = 0;

    ok(TermSrvCliprdrDummyBackendInit(NULL, &Backend) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr dummy backend state should fail init\n");
    ok(TermSrvCliprdrDummyBackendInit(&Dummy, NULL) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr backend output should fail init\n");

    ok(TermSrvCliprdrDummyBackendInit(&Dummy, &Backend) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend init failed\n");
    ok(!Dummy.HasData,
       "cliprdr dummy backend should start empty\n");

    ok(TermSrvCliprdrBackendGetData(&Backend,
                                    13,
                                    Buffer,
                                    sizeof(Buffer),
                                    &RequiredLength) == TermSrvCliprdrFormatNotAvailable,
       "Empty cliprdr dummy backend should report missing format\n");
    ok(RequiredLength == 0,
       "Missing cliprdr dummy data should require %Iu bytes\n", RequiredLength);

    ok(TermSrvCliprdrBackendSetData(&Backend,
                                    13,
                                    TextData,
                                    sizeof(TextData)) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend text set failed\n");
    ok(TermSrvCliprdrBackendGetData(&Backend,
                                    13,
                                    Buffer,
                                    sizeof(Buffer),
                                    &RequiredLength) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend exact text query failed\n");
    ok(RequiredLength == sizeof(TextData),
       "Unexpected cliprdr dummy text size %Iu\n", RequiredLength);
    ok(memcmp(Buffer, TextData, sizeof(TextData)) == 0,
       "Unexpected cliprdr dummy text bytes\n");

    ok(TermSrvCliprdrBackendGetData(&Backend,
                                    13,
                                    Buffer,
                                    sizeof(TextData) - 1,
                                    &RequiredLength) == TermSrvCliprdrBufferTooSmall,
       "Short cliprdr dummy backend query should fail\n");
    ok(RequiredLength == sizeof(TextData),
       "Short cliprdr dummy query should require %Iu bytes\n", RequiredLength);

    ok(TermSrvCliprdrBackendGetData(&Backend,
                                    1,
                                    Buffer,
                                    sizeof(Buffer),
                                    &RequiredLength) == TermSrvCliprdrFormatNotAvailable,
       "Different cliprdr dummy format should be missing\n");
    ok(RequiredLength == 0,
       "Different cliprdr dummy format should require %Iu bytes\n", RequiredLength);

    ok(TermSrvCliprdrBackendClear(&Backend) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend clear failed\n");
    ok(!Dummy.HasData,
       "cliprdr dummy backend should be empty after clear\n");
    ok(TermSrvCliprdrBackendGetData(&Backend,
                                    13,
                                    Buffer,
                                    sizeof(Buffer),
                                    &RequiredLength) == TermSrvCliprdrFormatNotAvailable,
       "Cleared cliprdr dummy backend should report missing format\n");

    ok(TermSrvCliprdrBackendSetData(&Backend,
                                    0xcafe,
                                    BinaryData,
                                    sizeof(BinaryData)) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend binary set failed\n");
    ok(Dummy.FormatId == 0xcafe,
       "Unexpected cliprdr dummy binary format 0x%lx\n", Dummy.FormatId);
    ok(Dummy.DataLength == sizeof(BinaryData),
       "Unexpected cliprdr dummy binary size %Iu\n", Dummy.DataLength);

    ok(TermSrvCliprdrDummyBackendReset(&Dummy) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend reset failed\n");
    ok(TermSrvCliprdrBackendGetData(&Backend,
                                    0xcafe,
                                    Buffer,
                                    sizeof(Buffer),
                                    &RequiredLength) == TermSrvCliprdrFormatNotAvailable,
       "Reset cliprdr dummy backend should report missing format\n");

    ok(TermSrvCliprdrBackendSetData(NULL,
                                    13,
                                    TextData,
                                    sizeof(TextData)) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr backend set should fail\n");
    ok(TermSrvCliprdrBackendSetData(&Backend,
                                    13,
                                    NULL,
                                    sizeof(TextData)) == TermSrvCliprdrInvalidHeader,
       "NULL non-empty cliprdr backend payload should fail\n");
    ok(TermSrvCliprdrBackendGetData(&Backend,
                                    13,
                                    Buffer,
                                    sizeof(Buffer),
                                    NULL) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr backend required length should fail\n");
    ok(TermSrvCliprdrBackendClear(NULL) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr backend clear should fail\n");
}

static VOID
TestCliprdrHandlePdu(VOID)
{
    static const UCHAR FormatList[] =
    {
        0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0xaa, 0xbb
    };
    static const UCHAR ExpectedFormatListResponse[] =
    {
        0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR FormatDataRequest[] =
    {
        0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x0d, 0x00, 0x00, 0x00
    };
    static const UCHAR ClipboardData[] =
    {
        'a', 'b', 'c'
    };
    static const UCHAR ExpectedFormatDataResponse[] =
    {
        0x05, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00,
        'a', 'b', 'c'
    };
    static const UCHAR ExpectedMissingFormatResponse[] =
    {
        0x05, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR UnsupportedPdu[] =
    {
        0xff, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    TERMSRV_CLIPRDR_DUMMY_BACKEND Dummy;
    TERMSRV_CLIPRDR_BACKEND Backend;
    UCHAR Buffer[32];
    SIZE_T BytesWritten = 0;

    ok(TermSrvCliprdrDummyBackendInit(&Dummy, &Backend) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend init failed\n");

    ok(TermSrvCliprdrHandlePdu(FormatList,
                               sizeof(FormatList),
                               &Backend,
                               Buffer,
                               sizeof(Buffer),
                               &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Format List handler failed\n");
    ok(BytesWritten == sizeof(ExpectedFormatListResponse),
       "Unexpected cliprdr Format List handler response size %Iu\n", BytesWritten);
    ok(memcmp(Buffer,
              ExpectedFormatListResponse,
              sizeof(ExpectedFormatListResponse)) == 0,
       "cliprdr Format List handler response mismatch\n");

    ok(TermSrvCliprdrBackendSetData(&Backend,
                                    13,
                                    ClipboardData,
                                    sizeof(ClipboardData)) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend data set failed\n");
    ok(TermSrvCliprdrHandlePdu(FormatDataRequest,
                               sizeof(FormatDataRequest),
                               &Backend,
                               Buffer,
                               sizeof(Buffer),
                               &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr Format Data Request handler failed\n");
    ok(BytesWritten == sizeof(ExpectedFormatDataResponse),
       "Unexpected cliprdr Format Data Response size %Iu\n", BytesWritten);
    ok(memcmp(Buffer,
              ExpectedFormatDataResponse,
              sizeof(ExpectedFormatDataResponse)) == 0,
       "cliprdr Format Data Response mismatch\n");

    ok(TermSrvCliprdrHandlePdu(FormatDataRequest,
                               sizeof(FormatDataRequest),
                               &Backend,
                               Buffer,
                               sizeof(ExpectedFormatDataResponse) - 1,
                               &BytesWritten) == TermSrvCliprdrBufferTooSmall,
       "Short cliprdr handler output buffer should fail\n");
    ok(BytesWritten == 0,
       "Short cliprdr handler output buffer wrote %Iu bytes\n", BytesWritten);

    ok(TermSrvCliprdrBackendClear(&Backend) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend clear failed\n");
    ok(TermSrvCliprdrHandlePdu(FormatDataRequest,
                               sizeof(FormatDataRequest),
                               &Backend,
                               Buffer,
                               sizeof(Buffer),
                               &BytesWritten) == TermSrvCliprdrFormatNotAvailable,
       "Missing cliprdr format should be reported\n");
    ok(BytesWritten == sizeof(ExpectedMissingFormatResponse),
       "Unexpected missing cliprdr format response size %Iu\n", BytesWritten);
    ok(memcmp(Buffer,
              ExpectedMissingFormatResponse,
              sizeof(ExpectedMissingFormatResponse)) == 0,
       "Missing cliprdr format response mismatch\n");

    ok(TermSrvCliprdrHandlePdu(UnsupportedPdu,
                               sizeof(UnsupportedPdu),
                               &Backend,
                               Buffer,
                               sizeof(Buffer),
                               &BytesWritten) == TermSrvCliprdrUnsupportedPdu,
       "Unsupported cliprdr PDU should fail\n");
    ok(BytesWritten == 0,
       "Unsupported cliprdr PDU wrote %Iu bytes\n", BytesWritten);

    ok(TermSrvCliprdrHandlePdu(FormatList,
                               sizeof(FormatList),
                               NULL,
                               Buffer,
                               sizeof(Buffer),
                               &BytesWritten) == TermSrvCliprdrInvalidHeader,
       "NULL cliprdr handler backend should fail\n");
}

static VOID
TestCliprdrRouteMcsSendData(VOID)
{
    static const UCHAR FormatListPacket[] =
    {
        0x03, 0x00, 0x00, 0x19,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xef, 0x70, 0x00, 0x0a,
        0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0xaa, 0xbb
    };
    static const UCHAR ExpectedFormatListResponse[] =
    {
        0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const UCHAR FormatDataRequestPacket[] =
    {
        0x03, 0x00, 0x00, 0x1b,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xef, 0x70, 0x00, 0x0c,
        0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x0d, 0x00, 0x00, 0x00
    };
    static const UCHAR NonCliprdrChannelPacket[] =
    {
        0x03, 0x00, 0x00, 0x19,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xf0, 0x70, 0x00, 0x0a,
        0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0xaa, 0xbb
    };
    static const UCHAR MalformedPacket[] =
    {
        0x03, 0x00, 0x00, 0x19,
        0x02, 0xf0, 0x80,
        0x64, 0x03, 0xe9, 0x03, 0xef, 0x70, 0x00, 0x0b,
        0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0xaa, 0xbb
    };
    static const UCHAR ClipboardData[] =
    {
        'r', 'd', 'p'
    };
    static const UCHAR ExpectedFormatDataResponse[] =
    {
        0x05, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00,
        'r', 'd', 'p'
    };
    static const UCHAR ExpectedMissingFormatResponse[] =
    {
        0x05, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    TERMSRV_CLIPRDR_CHANNEL Channel;
    TERMSRV_CLIPRDR_CHANNEL UnassignedChannel;
    TERMSRV_CLIPRDR_DUMMY_BACKEND Dummy;
    TERMSRV_CLIPRDR_BACKEND Backend;
    UCHAR Buffer[32];
    SIZE_T BytesWritten = 0;

    ok(TermSrvCliprdrChannelInit(&Channel) == TermSrvCliprdrSuccess,
       "cliprdr channel init failed\n");
    ok(TermSrvCliprdrAssignChannelId(&Channel, 1007) == TermSrvCliprdrSuccess,
       "cliprdr channel assignment failed\n");
    ok(TermSrvCliprdrChannelInit(&UnassignedChannel) == TermSrvCliprdrSuccess,
       "unassigned cliprdr channel init failed\n");
    ok(TermSrvCliprdrDummyBackendInit(&Dummy, &Backend) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend init failed\n");

    ok(TermSrvCliprdrRouteMcsSendData(FormatListPacket,
                                      sizeof(FormatListPacket),
                                      &Channel,
                                      &Backend,
                                      Buffer,
                                      sizeof(Buffer),
                                      &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr MCS Format List route failed\n");
    ok(BytesWritten == sizeof(ExpectedFormatListResponse),
       "Unexpected routed cliprdr Format List response size %Iu\n", BytesWritten);
    ok(memcmp(Buffer,
              ExpectedFormatListResponse,
              sizeof(ExpectedFormatListResponse)) == 0,
       "Routed cliprdr Format List response mismatch\n");

    ok(TermSrvCliprdrBackendSetData(&Backend,
                                    13,
                                    ClipboardData,
                                    sizeof(ClipboardData)) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend data set failed\n");
    ok(TermSrvCliprdrRouteMcsSendData(FormatDataRequestPacket,
                                      sizeof(FormatDataRequestPacket),
                                      &Channel,
                                      &Backend,
                                      Buffer,
                                      sizeof(Buffer),
                                      &BytesWritten) == TermSrvCliprdrSuccess,
       "cliprdr MCS Format Data Request route failed\n");
    ok(BytesWritten == sizeof(ExpectedFormatDataResponse),
       "Unexpected routed cliprdr Format Data Response size %Iu\n", BytesWritten);
    ok(memcmp(Buffer,
              ExpectedFormatDataResponse,
              sizeof(ExpectedFormatDataResponse)) == 0,
       "Routed cliprdr Format Data Response mismatch\n");

    ok(TermSrvCliprdrBackendClear(&Backend) == TermSrvCliprdrSuccess,
       "cliprdr dummy backend clear failed\n");
    ok(TermSrvCliprdrRouteMcsSendData(FormatDataRequestPacket,
                                      sizeof(FormatDataRequestPacket),
                                      &Channel,
                                      &Backend,
                                      Buffer,
                                      sizeof(Buffer),
                                      &BytesWritten) == TermSrvCliprdrFormatNotAvailable,
       "Missing routed cliprdr format should be reported\n");
    ok(BytesWritten == sizeof(ExpectedMissingFormatResponse),
       "Unexpected missing routed cliprdr format response size %Iu\n", BytesWritten);
    ok(memcmp(Buffer,
              ExpectedMissingFormatResponse,
              sizeof(ExpectedMissingFormatResponse)) == 0,
       "Missing routed cliprdr format response mismatch\n");

    ok(TermSrvCliprdrRouteMcsSendData(NonCliprdrChannelPacket,
                                      sizeof(NonCliprdrChannelPacket),
                                      &Channel,
                                      &Backend,
                                      Buffer,
                                      sizeof(Buffer),
                                      &BytesWritten) == TermSrvCliprdrUnsupportedPdu,
       "Non-cliprdr MCS channel should be unsupported\n");
    ok(BytesWritten == 0,
       "Non-cliprdr MCS channel wrote %Iu bytes\n", BytesWritten);

    ok(TermSrvCliprdrRouteMcsSendData(FormatListPacket,
                                      sizeof(FormatListPacket),
                                      &UnassignedChannel,
                                      &Backend,
                                      Buffer,
                                      sizeof(Buffer),
                                      &BytesWritten) == TermSrvCliprdrUnsupportedPdu,
       "Unassigned cliprdr MCS channel should be unsupported\n");
    ok(BytesWritten == 0,
       "Unassigned cliprdr MCS channel wrote %Iu bytes\n", BytesWritten);

    ok(TermSrvCliprdrRouteMcsSendData(MalformedPacket,
                                      sizeof(MalformedPacket),
                                      &Channel,
                                      &Backend,
                                      Buffer,
                                      sizeof(Buffer),
                                      &BytesWritten) == TermSrvCliprdrInvalidLength,
       "Malformed routed cliprdr MCS packet should fail\n");
    ok(BytesWritten == 0,
       "Malformed routed cliprdr MCS packet wrote %Iu bytes\n", BytesWritten);
}

START_TEST(RdpPeer)
{
    TestFullHandshake();
    TestOrderingAndChannelValidation();
    TestTooManyChannels();
    TestAuthenticationFailure();
    TestAttachFailure();
    TestBackendSelectionAndBehavior();
    TestSessmanBackendFallback();
    TestConsoleBackendBinding();
    TestBackendSelectionFallback();
    TestClientInfoBeforeSecurity();
    TestInputAndVirtualChannels();
    TestFastPathNotNegotiated();
    TestDisconnectLogoffAndReconnect();
    TestGraphicsOutput();
    TestBgra32ToRdpBitmapData();
    TestUnknownPacket();
    TestRdpBcgrParseConnectionRequest();
    TestRdpBcgrParseMcsConnectInitial();
    TestRdpBcgrWriteConnectionConfirm();
    TestRdpBcgrWriteMcsConnectResponse();
    TestRdpBcgrParseMcsErectDomainRequest();
    TestRdpBcgrParseMcsAttachUserRequest();
    TestRdpBcgrWriteMcsAttachUserConfirm();
    TestRdpBcgrParseMcsChannelJoinRequest();
    TestRdpBcgrParseSecurityExchangePayload();
    TestRdpBcgrParseClientInfoPayload();
    TestRdpBcgrParseMcsSendDataPayload();
    TestRdpBcgrWriteMcsSendDataPayload();
    TestRdpBcgrParseInputEvents();
    TestRdpBcgrParseShareDataInputPdu();
    TestRdpBcgrParseFastPathInputEvents();
    TestRdpBcgrWriteMcsChannelJoinConfirm();
    TestRdpBcgrParseStaticChannelList();
    TestCliprdrStaticChannelName();
    TestCliprdrChannelDescriptor();
    TestCliprdrAssignFromStaticChannelList();
    TestCliprdrParsePdu();
    TestCliprdrWriteMonitorReady();
    TestCliprdrWriteFormatListResponse();
    TestCliprdrFormatListHelpers();
    TestCliprdrParseFormatDataRequest();
    TestCliprdrWriteFormatDataResponse();
    TestCliprdrFileListHelpers();
    TestCliprdrFileContentsHelpers();
    TestCliprdrDummyBackend();
    TestCliprdrHandlePdu();
    TestCliprdrRouteMcsSendData();
}
