/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Unit tests for the minimal RDP server-side state engine
 */

#include <apitest.h>
#include "termsrv.h"

static VOID
ActivatePeer(
    _Inout_ TERMSRV_RDP_PEER *Peer)
{
    ok(TermSrvRdpPeerReceive(Peer, "X224 cookie=mstshash=test") == TermSrvRdpSuccess,
       "X224 failed\n");
    ok(TermSrvRdpPeerReceive(Peer, "MCS channels=rdpdr;fast=1") == TermSrvRdpSuccess,
       "MCS failed\n");
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
TestFullHandshake(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);

    ok(Manager.Sessions[0].State == TermSrvSessionConnected,
       "Session was not connected\n");
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
TestInputAndVirtualChannels(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);

    ok(TermSrvRdpPeerReceive(&Peer, "SLOW_INPUT scancode=30") == TermSrvRdpSuccess,
       "Slow-path input failed\n");
    ok(Manager.LastInputSessionId == 1,
       "Input delivered to session %d\n", Manager.LastInputSessionId);

    ok(TermSrvRdpPeerReceive(&Peer, "FAST_INPUT mouse=1") == TermSrvRdpSuccess,
       "Fast-path input failed\n");

    ok(TermSrvRdpPeerReceive(&Peer, "VC id=1004 payload=device") == TermSrvRdpSuccess,
       "Virtual channel payload failed\n");
    ok(strcmp(Manager.LastChannelName, "rdpdr") == 0,
       "Expected rdpdr, got %s\n", Manager.LastChannelName);

    ok(TermSrvRdpPeerReceive(&Peer, "VC id=9999 payload=device") == TermSrvRdpUnknownChannel,
       "Unknown virtual channel should fail\n");
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

    TermSrvRdpPeerInit(&Peer, &Manager);
    ActivatePeer(&Peer);
    ok(Peer.SessionId == 1,
       "Reconnect did not reuse session 1, got %d\n", Peer.SessionId);

    ok(TermSrvRdpPeerReceive(&Peer, "LOGOFF") == TermSrvRdpSuccess,
       "Logoff failed\n");
    ok(Manager.Sessions[0].State == TermSrvSessionLoggedOff,
       "Logoff did not destroy session\n");
}

static VOID
TestGraphicsOutput(VOID)
{
    TERMSRV_SESSION_MANAGER Manager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&Manager);
    TermSrvRdpPeerInit(&Peer, &Manager);

    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 10, 10) == TermSrvRdpGraphicsBeforeActive,
       "Graphics before Active should fail\n");

    ActivatePeer(&Peer);

    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 10, 10) == TermSrvRdpSuccess,
       "Bitmap update failed\n");
    ok(strcmp(Peer.LastOutput, "GRAPHICS_UPDATE") == 0,
       "Expected GRAPHICS_UPDATE, got %s\n", Peer.LastOutput);

    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 4096, 10) == TermSrvRdpBitmapOutOfBounds,
       "Out-of-bounds bitmap should fail\n");
}

START_TEST(RdpPeer)
{
    TestFullHandshake();
    TestOrderingAndChannelValidation();
    TestInputAndVirtualChannels();
    TestDisconnectLogoffAndReconnect();
    TestGraphicsOutput();
}

