/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     KDNET protocol compatibility vectors
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <apitest.h>
#include <ndk/ntndk.h>
#include <reactos/kdprotocol.h>
#include <reactos/kdnetprotocol.h>
#include <reactos/windbgkd.h>

static const unsigned char ControlPacketVector[] =
{
    0x4d, 0x44, 0x42, 0x47, 0x01, 0x01, 0x4e, 0x8d,
    0x33, 0x6f, 0x89, 0xb7, 0xf3, 0x8d, 0xc1, 0xf2,
    0xd1, 0xfa, 0x1f, 0x18, 0xdd, 0x3c, 0xdf, 0x95,
    0x98, 0x71, 0xb8, 0x9d, 0x34, 0x28, 0xb6, 0x0d,
    0x87, 0x35, 0x84, 0x2a, 0xe6, 0x58, 0x4d, 0xb3,
    0x8f, 0x80, 0x0d, 0xd2, 0x21, 0x1b, 0x0b, 0x08,
    0x1a, 0x41, 0xbf, 0xb8, 0x54, 0xd6
};

static const unsigned char DataKeyVector[] =
{
    0x9e, 0xf2, 0xa5, 0xae, 0x49, 0x15, 0x1a, 0xd8,
    0xce, 0xa3, 0x8a, 0xad, 0x96, 0x01, 0xba, 0x8d,
    0xa2, 0x41, 0xd6, 0xcd, 0x63, 0x49, 0x3d, 0x5c,
    0x2a, 0x5a, 0x72, 0xf5, 0x51, 0x00, 0x74, 0x5c
};

static const unsigned char DataPacketVector[] =
{
    0x4d, 0x44, 0x42, 0x47, 0x01, 0x00, 0x57, 0x66,
    0x66, 0x34, 0x68, 0x5d, 0x35, 0x5d, 0xb8, 0x3b,
    0x79, 0xf2, 0x25, 0x2c, 0xa6, 0x14, 0xa0, 0xf4,
    0x24, 0x02, 0xe4, 0xde, 0x52, 0x93, 0x99, 0xd1,
    0x1e, 0x25, 0x36, 0x3a, 0x93, 0xc6
};

static const unsigned char HandshakeDataKeyVector[] =
{
    0xfa, 0xa5, 0x94, 0xd4, 0x81, 0x3f, 0x0c, 0x9b,
    0x1f, 0xa4, 0x8c, 0xd9, 0x49, 0x71, 0x3d, 0x87,
    0x22, 0xcf, 0x5d, 0x4d, 0xaf, 0xec, 0x65, 0xc7,
    0x3b, 0xc6, 0xd2, 0x3f, 0xfb, 0x22, 0xa6, 0x96
};

static const unsigned char KdDataPacketVector[] =
{
    0x30, 0x30, 0x30, 0x30, 0x02, 0x00, 0x05, 0x00,
    0x00, 0x08, 0x80, 0x80, 0x0f, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05
};

static const unsigned char KdAcknowledgePacketVector[] =
{
    0x69, 0x69, 0x69, 0x69, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00
};

static const uint32_t KdManipulateApis[] =
{
    DbgKdWriteVirtualMemoryApi,
    DbgKdGetContextApi,
    DbgKdSetContextApi,
    DbgKdWriteBreakPointApi,
    DbgKdRestoreBreakPointApi,
    DbgKdContinueApi,
    DbgKdContinueApi2
};

static int
BytesEqual(const unsigned char *Left, const unsigned char *Right, size_t Length)
{
    while (Length-- != 0)
    {
        if (*Left++ != *Right++)
            return 0;
    }
    return 1;
}

static void
CopyBytes(unsigned char *Destination, const unsigned char *Source, size_t Length)
{
    while (Length-- != 0)
        *Destination++ = *Source++;
}

static void
WriteLittleEndian32(unsigned char *Buffer, uint32_t Value)
{
    Buffer[0] = (unsigned char)Value;
    Buffer[1] = (unsigned char)(Value >> 8);
    Buffer[2] = (unsigned char)(Value >> 16);
    Buffer[3] = (unsigned char)(Value >> 24);
}

START_TEST(protocol)
{
    KDNET_CRYPTO_CONTEXT Context;
    KDNET_PACKET_INFO PacketInfo;
    KDNET_STATUS Status;
    KD_PACKET_STATUS KdStatus;
    KD_PACKET_VIEW KdPacketView;
    KD_PACKET_SESSION KdSession;
    KD_PACKET_SESSION_RESULT KdSessionResult;
    unsigned char Response[KDNET_CONTROL_RESPONSE_SIZE];
    unsigned char ClientKey[KDNET_CLIENT_KEY_SIZE];
    unsigned char Poke[KDNET_CONTROL_POKE_SIZE];
    unsigned char Payload[21];
    unsigned char Packet[sizeof(ControlPacketVector)];
    unsigned char DataPacket[sizeof(DataPacketVector)];
    unsigned char KdPacket[KD_PACKET_HEADER_SIZE + 56 + 16];
    unsigned char KdHeader[] = {1, 2};
    unsigned char KdData[] = {3, 4, 5};
    unsigned char ManipulateHeader[56];
    unsigned char ManipulateData[16];
    size_t PacketLength;
    size_t Index;
    uint32_t KdPacketId;

    ok(KdPacketCalculateChecksum(KdData, sizeof(KdData)) == 12,
       "inner KD checksum is incorrect\n");
    ok(KdPacketCalculateChecksum(NULL, 0) == 0,
       "empty inner KD checksum is incorrect\n");

    KdStatus = KdPacketEncodeData(KD_PACKET_TYPE_STATE_MANIPULATE,
                                  KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID,
                                  KdHeader,
                                  sizeof(KdHeader),
                                  KdData,
                                  sizeof(KdData),
                                  KdPacket,
                                  sizeof(KdDataPacketVector) - 1,
                                  &PacketLength);
    ok(KdStatus == KdPacketStatusBufferTooSmall,
       "undersized inner KD buffer returned %d\n", KdStatus);
    ok(PacketLength == sizeof(KdDataPacketVector),
       "inner KD required length was %Iu\n", PacketLength);

    KdStatus = KdPacketEncodeData(KD_PACKET_TYPE_STATE_MANIPULATE,
                                  KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID,
                                  KdHeader,
                                  sizeof(KdHeader),
                                  KdData,
                                  sizeof(KdData),
                                  KdPacket,
                                  sizeof(KdPacket),
                                  &PacketLength);
    ok(KdStatus == KdPacketStatusSuccess,
       "inner KD data encode returned %d\n", KdStatus);
    ok(PacketLength == sizeof(KdDataPacketVector) &&
       BytesEqual(KdPacket, KdDataPacketVector, sizeof(KdDataPacketVector)),
       "inner KD data packet differs from the reference vector\n");

    KdStatus = KdPacketDecode(KdPacket, PacketLength, &KdPacketView);
    ok(KdStatus == KdPacketStatusSuccess,
       "inner KD data decode returned %d\n", KdStatus);
    ok(KdPacketView.Leader == KD_PACKET_LEADER_DATA &&
       KdPacketView.Type == KD_PACKET_TYPE_STATE_MANIPULATE &&
       KdPacketView.Id == (KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID) &&
       KdPacketView.PayloadLength == 5 &&
       BytesEqual(KdPacketView.Payload, KdDataPacketVector + 16, 5),
       "decoded inner KD data is incorrect\n");

    KdPacket[16] ^= 1;
    KdStatus = KdPacketDecode(KdPacket, PacketLength, &KdPacketView);
    ok(KdStatus == KdPacketStatusChecksumMismatch,
       "bad inner KD checksum returned %d\n", KdStatus);

    KdStatus = KdPacketEncodeControl(KD_PACKET_TYPE_ACKNOWLEDGE,
                                     KD_PACKET_INITIAL_ID,
                                     KdPacket,
                                     sizeof(KdPacket),
                                     &PacketLength);
    ok(KdStatus == KdPacketStatusSuccess,
       "inner KD ACK encode returned %d\n", KdStatus);
    ok(PacketLength == sizeof(KdAcknowledgePacketVector) &&
       BytesEqual(KdPacket,
                  KdAcknowledgePacketVector,
                  sizeof(KdAcknowledgePacketVector)),
       "inner KD ACK differs from the reference vector\n");

    KdStatus = KdPacketDecode(KdPacket, PacketLength, &KdPacketView);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdPacketView.Leader == KD_PACKET_LEADER_CONTROL &&
       KdPacketView.Type == KD_PACKET_TYPE_ACKNOWLEDGE &&
       KdPacketView.PayloadLength == 0,
       "inner KD ACK decode returned %d\n", KdStatus);

    KdStatus = KdPacketEncodeControl(KD_PACKET_TYPE_STATE_MANIPULATE,
                                     0,
                                     KdPacket,
                                     sizeof(KdPacket),
                                     &PacketLength);
    ok(KdStatus == KdPacketStatusInvalidParameter,
       "invalid inner KD control type returned %d\n", KdStatus);

    KdStatus = KdPacketEncodeUnused(KdPacket,
                                    KD_PACKET_UNUSED_SIZE - 1,
                                    &PacketLength);
    ok(KdStatus == KdPacketStatusBufferTooSmall &&
       PacketLength == KD_PACKET_UNUSED_SIZE,
       "undersized unused packet returned %d, length %Iu\n",
       KdStatus, PacketLength);
    KdStatus = KdPacketEncodeUnused(KdPacket,
                                    sizeof(KdPacket),
                                    &PacketLength);
    ok(KdStatus == KdPacketStatusSuccess &&
       PacketLength == KD_PACKET_UNUSED_SIZE,
       "unused packet encode returned %d\n", KdStatus);
    KdStatus = KdPacketDecode(KdPacket, PacketLength, &KdPacketView);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdPacketView.Leader == KD_PACKET_LEADER_UNUSED,
       "unused packet decode returned %d\n", KdStatus);
    KdPacket[11] = 1;
    KdStatus = KdPacketDecode(KdPacket, PacketLength, &KdPacketView);
    ok(KdStatus == KdPacketStatusInvalidPacket,
       "malformed unused packet returned %d\n", KdStatus);

    for (Index = 0; Index < sizeof(ManipulateHeader); ++Index)
        ManipulateHeader[Index] = 0;
    for (Index = 0; Index < sizeof(ManipulateData); ++Index)
        ManipulateData[Index] = (unsigned char)(0xa0 + Index);

    for (Index = 0;
         Index < sizeof(KdManipulateApis) / sizeof(KdManipulateApis[0]);
         ++Index)
    {
        WriteLittleEndian32(ManipulateHeader, KdManipulateApis[Index]);
        KdStatus = KdPacketEncodeData(KD_PACKET_TYPE_STATE_MANIPULATE,
                                      KD_PACKET_INITIAL_ID,
                                      ManipulateHeader,
                                      sizeof(ManipulateHeader),
                                      ManipulateData,
                                      sizeof(ManipulateData),
                                      KdPacket,
                                      sizeof(KdPacket),
                                      &PacketLength);
        ok(KdStatus == KdPacketStatusSuccess,
           "manipulate API %08lx encode returned %d\n",
           (unsigned long)KdManipulateApis[Index], KdStatus);
        KdStatus = KdPacketDecode(KdPacket, PacketLength, &KdPacketView);
        ok(KdStatus == KdPacketStatusSuccess &&
           KdPacketView.Type == KD_PACKET_TYPE_STATE_MANIPULATE &&
           KdPacketView.PayloadLength ==
               sizeof(ManipulateHeader) + sizeof(ManipulateData) &&
           BytesEqual(KdPacketView.Payload,
                      ManipulateHeader,
                      sizeof(ManipulateHeader)) &&
           BytesEqual(KdPacketView.Payload + sizeof(ManipulateHeader),
                      ManipulateData,
                      sizeof(ManipulateData)),
           "manipulate API %08lx did not round-trip, status %d\n",
           (unsigned long)KdManipulateApis[Index], KdStatus);
    }

    KdPacketSessionInitialize(&KdSession, 2);
    ok(KdSession.NextSendId ==
           (KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID) &&
       KdSession.NextReceiveId == KD_PACKET_INITIAL_ID &&
       !KdSession.WaitingForAcknowledge,
       "inner KD session initialized with incorrect state\n");

    KdStatus = KdPacketSessionBeginSend(&KdSession, &KdPacketId);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdPacketId == (KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID) &&
       KdSession.WaitingForAcknowledge,
       "first inner KD send returned %d, id %08lx\n",
       KdStatus, (unsigned long)KdPacketId);
    KdStatus = KdPacketSessionBeginSend(&KdSession, &KdPacketId);
    ok(KdStatus == KdPacketStatusInvalidPacket,
       "overlapping inner KD send returned %d\n", KdStatus);

    KdStatus = KdPacketEncodeControl(KD_PACKET_TYPE_ACKNOWLEDGE,
                                     KD_PACKET_INITIAL_ID,
                                     KdPacket,
                                     sizeof(KdPacket),
                                     &PacketLength);
    ok(KdStatus == KdPacketStatusSuccess,
       "sync-clearing ACK encode returned %d\n", KdStatus);
    KdStatus = KdPacketDecode(KdPacket, PacketLength, &KdPacketView);
    ok(KdStatus == KdPacketStatusSuccess,
       "sync-clearing ACK decode returned %d\n", KdStatus);
    KdStatus = KdPacketSessionProcess(&KdSession,
                                      &KdPacketView,
                                      &KdSessionResult);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdSessionResult.Event == KdPacketSessionEventSendAcknowledged &&
       KdSession.NextSendId == (KD_PACKET_INITIAL_ID ^ 1) &&
       !KdSession.WaitingForAcknowledge,
       "sync-clearing ACK was not accepted, status %d, event %d\n",
       KdStatus, KdSessionResult.Event);

    KdStatus = KdPacketSessionBeginSend(&KdSession, &KdPacketId);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdPacketId == (KD_PACKET_INITIAL_ID ^ 1),
       "second inner KD send returned %d, id %08lx\n",
       KdStatus, (unsigned long)KdPacketId);
    KdStatus = KdPacketSessionTimeout(&KdSession, &KdSessionResult);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdSessionResult.Event == KdPacketSessionEventResendLast &&
       KdSession.RetriesRemaining == 1,
       "first timeout returned status %d, event %d, retries %lu\n",
       KdStatus,
       KdSessionResult.Event,
       (unsigned long)KdSession.RetriesRemaining);
    KdPacketSessionTimeout(&KdSession, &KdSessionResult);
    ok(KdSessionResult.Event == KdPacketSessionEventResendLast &&
       KdSession.RetriesRemaining == 0,
       "second timeout returned event %d, retries %lu\n",
       KdSessionResult.Event,
       (unsigned long)KdSession.RetriesRemaining);
    KdPacketSessionTimeout(&KdSession, &KdSessionResult);
    ok(KdSessionResult.Event == KdPacketSessionEventRetryLimit,
       "exhausted retry budget returned event %d\n", KdSessionResult.Event);

    KdPacketView.Leader = KD_PACKET_LEADER_CONTROL;
    KdPacketView.Type = KD_PACKET_TYPE_RESET;
    KdPacketView.PayloadLength = 0;
    KdPacketView.Id = 0;
    KdPacketView.Checksum = 0;
    KdPacketView.Payload = NULL;
    KdStatus = KdPacketSessionProcess(&KdSession,
                                      &KdPacketView,
                                      &KdSessionResult);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdSessionResult.Event == KdPacketSessionEventReset &&
       KdSessionResult.ControlType == KD_PACKET_TYPE_RESET &&
       KdSessionResult.ControlId == 0 &&
       KdSession.NextSendId ==
           (KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID),
       "inner KD RESET returned status %d, event %d\n",
       KdStatus, KdSessionResult.Event);

    KdPacketSessionInitialize(&KdSession, 1);
    KdPacketSessionBeginSend(&KdSession, &KdPacketId);
    KdPacketView.Leader = KD_PACKET_LEADER_CONTROL;
    KdPacketView.Type = KD_PACKET_TYPE_RESEND;
    KdPacketView.Id = 0;
    KdStatus = KdPacketSessionProcess(&KdSession,
                                      &KdPacketView,
                                      &KdSessionResult);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdSessionResult.Event == KdPacketSessionEventResendLast &&
       KdSession.RetriesRemaining == 0,
       "RESEND control returned status %d, event %d\n",
       KdStatus, KdSessionResult.Event);
    KdPacketSessionProcess(&KdSession,
                           &KdPacketView,
                           &KdSessionResult);
    ok(KdSessionResult.Event == KdPacketSessionEventRetryLimit,
       "repeated RESEND returned event %d\n", KdSessionResult.Event);

    KdPacketSessionInitialize(&KdSession, 2);

    KdPacketView.Leader = KD_PACKET_LEADER_DATA;
    KdPacketView.Type = KD_PACKET_TYPE_STATE_MANIPULATE;
    KdPacketView.Id = KD_PACKET_INITIAL_ID;
    KdStatus = KdPacketSessionProcess(&KdSession,
                                      &KdPacketView,
                                      &KdSessionResult);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdSessionResult.Event == KdPacketSessionEventDataReceived &&
       KdSessionResult.ControlType == KD_PACKET_TYPE_ACKNOWLEDGE &&
       KdSessionResult.ControlId == KD_PACKET_INITIAL_ID &&
       KdSession.NextReceiveId == (KD_PACKET_INITIAL_ID ^ 1),
       "first received data returned status %d, event %d\n",
       KdStatus, KdSessionResult.Event);
    KdStatus = KdPacketSessionProcess(&KdSession,
                                      &KdPacketView,
                                      &KdSessionResult);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdSessionResult.Event == KdPacketSessionEventDuplicateData &&
       KdSessionResult.ControlType == KD_PACKET_TYPE_ACKNOWLEDGE,
       "duplicate data returned status %d, event %d\n",
       KdStatus, KdSessionResult.Event);

    KdPacketView.Id = KD_PACKET_INITIAL_ID ^ 1;
    KdStatus = KdPacketSessionProcess(&KdSession,
                                      &KdPacketView,
                                      &KdSessionResult);
    ok(KdStatus == KdPacketStatusSuccess &&
       KdSessionResult.Event == KdPacketSessionEventDataReceived &&
       KdSession.NextReceiveId == KD_PACKET_INITIAL_ID,
       "next received data returned status %d, event %d\n",
       KdStatus, KdSessionResult.Event);

    KdPacketSessionRejectMalformed(&KdSessionResult);
    ok(KdSessionResult.Event == KdPacketSessionEventNone &&
       KdSessionResult.ControlType == KD_PACKET_TYPE_RESEND &&
       KdSessionResult.ControlId == 0,
       "malformed packet did not request a resend\n");

    Status = KdNetInitializeCryptoContext(&Context, "1.2.3.4");
    ok(Status == KdNetStatusSuccess, "initialization returned %d\n", Status);
    ok(Context.ControlKey[0] == 1 && Context.ControlKey[8] == 2 &&
       Context.ControlKey[16] == 3 && Context.ControlKey[24] == 4,
       "base-36 key was decoded incorrectly\n");
    ok(Context.HmacKey[0] == 0xfe && Context.HmacKey[8] == 0xfd &&
       Context.HmacKey[16] == 0xfc && Context.HmacKey[24] == 0xfb,
       "HMAC key was derived incorrectly\n");

    Status = KdNetGetPacketSize(21, &PacketLength);
    ok(Status == KdNetStatusSuccess, "size calculation returned %d\n", Status);
    ok(PacketLength == 54, "packet length was %Iu, expected 54\n", PacketLength);

    Status = KdNetInitializeCryptoContext(&Context, "1.2.3");
    ok(Status == KdNetStatusInvalidKey, "short key returned %d\n", Status);
    Status = KdNetInitializeCryptoContext(&Context, "1.2.3.4.5");
    ok(Status == KdNetStatusInvalidKey, "long key returned %d\n", Status);
    Status = KdNetInitializeCryptoContext(&Context, "1.2.*.4");
    ok(Status == KdNetStatusInvalidKey, "invalid digit returned %d\n", Status);
    Status = KdNetInitializeCryptoContext(&Context, "3w5e11264sgsg.2.3.4");
    ok(Status == KdNetStatusInvalidKey, "overflowing part returned %d\n", Status);

    Status = KdNetInitializeCryptoContext(&Context, "1.2.3.4");
    ok(Status == KdNetStatusSuccess, "reinitialization returned %d\n", Status);

    for (Index = 0; Index < sizeof(ClientKey); ++Index)
        ClientKey[Index] = (unsigned char)Index;
    Status = KdNetBuildPokePayload(ClientKey,
                                   Poke,
                                   sizeof(Poke) - 1,
                                   &PacketLength);
    ok(Status == KdNetStatusBufferTooSmall,
       "undersized Poke buffer returned %d\n", Status);
    ok(PacketLength == KDNET_CONTROL_POKE_SIZE,
       "Poke length was %Iu\n", PacketLength);
    Status = KdNetBuildPokePayload(ClientKey,
                                   Poke,
                                   sizeof(Poke),
                                   &PacketLength);
    ok(Status == KdNetStatusSuccess, "Poke creation returned %d\n", Status);
    ok(Poke[0] == 1 && Poke[1] == 1 &&
       BytesEqual(Poke + 2, ClientKey, sizeof(ClientKey)),
       "Poke contents are incorrect\n");

    Response[0] = 1;
    Response[1] = 2;
    CopyBytes(Response + 2, ClientKey, sizeof(ClientKey));
    for (Index = 0; Index < KDNET_HOST_KEY_SIZE; ++Index)
        Response[2 + KDNET_CLIENT_KEY_SIZE + Index] = (unsigned char)(0xa0 + Index);
    for (Index = 2 + KDNET_CLIENT_KEY_SIZE + KDNET_HOST_KEY_SIZE;
         Index < sizeof(Response);
         ++Index)
    {
        Response[Index] = 0;
    }
    Status = KdNetProcessResponsePayload(&Context,
                                         ClientKey,
                                         Response,
                                         sizeof(Response));
    ok(Status == KdNetStatusSuccess,
       "Response processing returned %d\n", Status);
    ok(BytesEqual(Context.DataKey,
                  HandshakeDataKeyVector,
                  sizeof(HandshakeDataKeyVector)),
       "handshake data key differs from the reference vector\n");

    Response[2] ^= 1;
    Status = KdNetProcessResponsePayload(&Context,
                                         ClientKey,
                                         Response,
                                         sizeof(Response));
    ok(Status == KdNetStatusInvalidHandshake,
       "bad echoed client key returned %d\n", Status);
    Response[2] ^= 1;
    Response[sizeof(Response) - 1] = 1;
    Status = KdNetProcessResponsePayload(&Context,
                                         ClientKey,
                                         Response,
                                         sizeof(Response));
    ok(Status == KdNetStatusInvalidHandshake,
       "bad Response trailer returned %d\n", Status);

    /* Return to the original deterministic data-key vector below. */
    Status = KdNetInitializeCryptoContext(&Context, "1.2.3.4");
    ok(Status == KdNetStatusSuccess, "context reset returned %d\n", Status);

    PacketLength = 0;
    Status = KdNetEncodePacket(&Context,
                               1,
                               KDNET_PACKET_TYPE_DATA,
                               1,
                               KDNET_DIRECTION_TARGET,
                               (const unsigned char *)"x",
                               1,
                               DataPacket,
                               sizeof(DataPacket),
                               &PacketLength);
    ok(Status == KdNetStatusDataKeyUnavailable,
       "data packet without a data key returned %d\n", Status);

    for (Index = 0; Index < sizeof(Payload); ++Index)
        Payload[Index] = (unsigned char)Index;

    Status = KdNetEncodePacket(&Context,
                               1,
                               KDNET_PACKET_TYPE_CONTROL,
                               0x01020304050607ULL,
                               KDNET_DIRECTION_TARGET,
                               Payload,
                               sizeof(Payload),
                               Packet,
                               sizeof(Packet) - 1,
                               &PacketLength);
    ok(Status == KdNetStatusBufferTooSmall,
       "undersized buffer returned %d\n", Status);
    ok(PacketLength == sizeof(ControlPacketVector),
       "required length was %Iu\n", PacketLength);

    Status = KdNetEncodePacket(&Context,
                               1,
                               KDNET_PACKET_TYPE_CONTROL,
                               0x01020304050607ULL,
                               KDNET_DIRECTION_TARGET,
                               Payload,
                               sizeof(Payload),
                               Packet,
                               sizeof(Packet),
                               &PacketLength);
    ok(Status == KdNetStatusSuccess, "control encode returned %d\n", Status);
    ok(PacketLength == sizeof(ControlPacketVector),
       "encoded length was %Iu\n", PacketLength);
    ok(BytesEqual(Packet, ControlPacketVector, sizeof(ControlPacketVector)),
       "control packet differs from the reference vector\n");

    Status = KdNetDecodePacket(&Context, Packet, PacketLength, &PacketInfo);
    ok(Status == KdNetStatusSuccess, "control decode returned %d\n", Status);
    ok(PacketInfo.Version == 1 &&
       PacketInfo.Type == KDNET_PACKET_TYPE_CONTROL &&
       PacketInfo.Direction == KDNET_DIRECTION_TARGET &&
       PacketInfo.SequenceNumber == 0x01020304050607ULL &&
       PacketInfo.PaddingLength == 3 &&
       PacketInfo.PayloadLength == sizeof(Payload),
       "decoded control metadata is incorrect\n");
    ok(BytesEqual(PacketInfo.Payload, Payload, sizeof(Payload)),
       "decoded control payload is incorrect\n");

    CopyBytes(Packet, ControlPacketVector, sizeof(ControlPacketVector));
    Packet[10] ^= 0x01;
    Status = KdNetDecodePacket(&Context, Packet, sizeof(Packet), &PacketInfo);
    ok(Status == KdNetStatusAuthenticationFailed,
       "corrupt packet returned %d\n", Status);

    CopyBytes(Packet, ControlPacketVector, sizeof(ControlPacketVector));
    Status = KdNetDecodePacket(&Context, Packet, sizeof(Packet) - 1, &PacketInfo);
    ok(Status == KdNetStatusInvalidPacket,
       "misaligned packet returned %d\n", Status);

    CopyBytes(Packet, ControlPacketVector, sizeof(ControlPacketVector));
    Packet[0] = 0;
    Status = KdNetDecodePacket(&Context, Packet, sizeof(Packet), &PacketInfo);
    ok(Status == KdNetStatusInvalidPacket,
       "bad magic returned %d\n", Status);

    CopyBytes(Packet, ControlPacketVector, sizeof(ControlPacketVector));
    Packet[5] = 2;
    Status = KdNetDecodePacket(&Context, Packet, sizeof(Packet), &PacketInfo);
    ok(Status == KdNetStatusInvalidPacket,
       "unknown packet type returned %d\n", Status);

    for (Index = 0; Index < sizeof(Response); ++Index)
        Response[Index] = (unsigned char)(Index * 7 + 3);
    Status = KdNetDeriveDataKey(&Context, Response, sizeof(Response));
    ok(Status == KdNetStatusSuccess, "data-key derivation returned %d\n", Status);
    ok(BytesEqual(Context.DataKey, DataKeyVector, sizeof(DataKeyVector)),
       "derived data key differs from the reference vector\n");

    Status = KdNetEncodePacket(&Context,
                               1,
                               KDNET_PACKET_TYPE_DATA,
                               9,
                               KDNET_DIRECTION_TARGET,
                               (const unsigned char *)"b",
                               1,
                               DataPacket,
                               sizeof(DataPacket),
                               &PacketLength);
    ok(Status == KdNetStatusSuccess, "data encode returned %d\n", Status);
    ok(PacketLength == sizeof(DataPacketVector),
       "data packet length was %Iu\n", PacketLength);
    ok(BytesEqual(DataPacket, DataPacketVector, sizeof(DataPacketVector)),
       "data packet differs from the reference vector\n");

    Status = KdNetDecodePacket(&Context, DataPacket, PacketLength, &PacketInfo);
    ok(Status == KdNetStatusSuccess, "data decode returned %d\n", Status);
    ok(PacketInfo.Type == KDNET_PACKET_TYPE_DATA &&
       PacketInfo.SequenceNumber == 9 &&
       PacketInfo.PaddingLength == 7 &&
       PacketInfo.PayloadLength == 1 && PacketInfo.Payload[0] == 'b',
       "decoded data packet is incorrect\n");
}
