/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Unit tests for the minimal RDP server-side state engine
 */

#include <apitest.h>
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

    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 1024, 768) == TermSrvRdpSuccess,
       "Desktop-sized bitmap update failed\n");
    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 4096, 10) == TermSrvRdpBitmapOutOfBounds,
       "Out-of-bounds bitmap width should fail\n");
    ok(TermSrvRdpPeerSendBitmapUpdate(&Peer, 10, 4096) == TermSrvRdpBitmapOutOfBounds,
       "Out-of-bounds bitmap height should fail\n");
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
        0x7f, 0x66, 0x03, 0x01, 0x02
    };
    static const UCHAR ExpectedResponse[] =
    {
        0x03, 0x00, 0x00, 0x0c,
        0x02, 0xf0, 0x80,
        0x7f, 0x66, 0x03, 0x01, 0x02
    };
    UCHAR Buffer[32];
    SIZE_T BytesWritten = 0;

    ok(TermSrvRdpBcgrWriteMcsConnectResponse(Buffer,
                                             sizeof(Buffer),
                                             Payload,
                                             sizeof(Payload),
                                             &BytesWritten) == TermSrvRdpBcgrSuccess,
       "MCS Connect Response serialization failed\n");
    ok(BytesWritten == sizeof(ExpectedResponse),
       "Unexpected serialized MCS Connect Response size %Iu\n", BytesWritten);
    ok(memcmp(Buffer, ExpectedResponse, sizeof(ExpectedResponse)) == 0,
       "Serialized MCS Connect Response did not match expected bytes\n");

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

START_TEST(RdpPeer)
{
    TestFullHandshake();
    TestOrderingAndChannelValidation();
    TestTooManyChannels();
    TestAuthenticationFailure();
    TestAttachFailure();
    TestClientInfoBeforeSecurity();
    TestInputAndVirtualChannels();
    TestFastPathNotNegotiated();
    TestDisconnectLogoffAndReconnect();
    TestGraphicsOutput();
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
    TestRdpBcgrWriteMcsChannelJoinConfirm();
}
