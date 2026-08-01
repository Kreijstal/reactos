/*
 * PROJECT:     ReactOS KD protocol
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Allocation-free inner KD packet framing
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KD_PACKET_HEADER_SIZE              16
#define KD_PACKET_UNUSED_SIZE              12
#define KD_PACKET_MAX_PAYLOAD            4000

#define KD_PACKET_LEADER_UNUSED    0x00000000UL
#define KD_PACKET_LEADER_DATA      0x30303030UL
#define KD_PACKET_LEADER_CONTROL   0x69696969UL

#define KD_PACKET_INITIAL_ID       0x80800000UL
#define KD_PACKET_SYNC_ID          0x00000800UL

#define KD_PACKET_TYPE_UNUSED                   0
#define KD_PACKET_TYPE_STATE_CHANGE32            1
#define KD_PACKET_TYPE_STATE_MANIPULATE          2
#define KD_PACKET_TYPE_DEBUG_IO                  3
#define KD_PACKET_TYPE_ACKNOWLEDGE               4
#define KD_PACKET_TYPE_RESEND                    5
#define KD_PACKET_TYPE_RESET                     6
#define KD_PACKET_TYPE_STATE_CHANGE64            7
#define KD_PACKET_TYPE_POLL_BREAKIN              8
#define KD_PACKET_TYPE_TRACE_IO                  9
#define KD_PACKET_TYPE_CONTROL_REQUEST          10
#define KD_PACKET_TYPE_FILE_IO                  11
#define KD_PACKET_TYPE_MAX                      12

typedef enum _KD_PACKET_STATUS
{
    KdPacketStatusSuccess = 0,
    KdPacketStatusInvalidParameter = -1,
    KdPacketStatusBufferTooSmall = -2,
    KdPacketStatusPayloadTooLarge = -3,
    KdPacketStatusInvalidPacket = -4,
    KdPacketStatusChecksumMismatch = -5
} KD_PACKET_STATUS;

typedef struct _KD_PACKET_VIEW
{
    uint32_t Leader;
    uint16_t Type;
    uint16_t PayloadLength;
    uint32_t Id;
    uint32_t Checksum;
    const uint8_t *Payload;
} KD_PACKET_VIEW;

typedef struct _KD_PACKET_SESSION
{
    uint32_t NextSendId;
    uint32_t NextReceiveId;
    uint32_t PendingSendId;
    uint32_t MaximumRetries;
    uint32_t RetriesRemaining;
    uint8_t WaitingForAcknowledge;
} KD_PACKET_SESSION;

typedef enum _KD_PACKET_SESSION_EVENT
{
    KdPacketSessionEventNone = 0,
    KdPacketSessionEventDataReceived,
    KdPacketSessionEventDuplicateData,
    KdPacketSessionEventSendAcknowledged,
    KdPacketSessionEventResendLast,
    KdPacketSessionEventRetryLimit,
    KdPacketSessionEventReset
} KD_PACKET_SESSION_EVENT;

typedef struct _KD_PACKET_SESSION_RESULT
{
    KD_PACKET_SESSION_EVENT Event;
    uint16_t ControlType;
    uint32_t ControlId;
} KD_PACKET_SESSION_RESULT;

#define KD_PACKET_SESSION_NO_CONTROL ((uint16_t)0xffff)

uint32_t
KdPacketCalculateChecksum(
    const uint8_t *Buffer,
    size_t Length);

/* Encode a normal KD data packet from the two buffers used by KdSendPacket. */
KD_PACKET_STATUS
KdPacketEncodeData(
    uint16_t Type,
    uint32_t Id,
    const uint8_t *MessageHeader,
    size_t MessageHeaderLength,
    const uint8_t *MessageData,
    size_t MessageDataLength,
    uint8_t *Packet,
    size_t PacketCapacity,
    size_t *PacketLength);

/* Encode an ACK, RESEND, or RESET control packet. */
KD_PACKET_STATUS
KdPacketEncodeControl(
    uint16_t Type,
    uint32_t Id,
    uint8_t *Packet,
    size_t PacketCapacity,
    size_t *PacketLength);

/* KDNET's initial unused packet omits the four-byte checksum field. */
KD_PACKET_STATUS
KdPacketEncodeUnused(
    uint8_t *Packet,
    size_t PacketCapacity,
    size_t *PacketLength);

/* Decode one complete inner KD packet. The returned payload aliases Packet. */
KD_PACKET_STATUS
KdPacketDecode(
    const uint8_t *Packet,
    size_t PacketLength,
    KD_PACKET_VIEW *PacketView);

/* Initialize the reliable inner-KD packet-id state. */
void
KdPacketSessionInitialize(
    KD_PACKET_SESSION *Session,
    uint32_t MaximumRetries);

/* Reserve the ID for one outbound data packet until it is acknowledged. */
KD_PACKET_STATUS
KdPacketSessionBeginSend(
    KD_PACKET_SESSION *Session,
    uint32_t *PacketId);

/* Process one validated packet and describe the required control action. */
KD_PACKET_STATUS
KdPacketSessionProcess(
    KD_PACKET_SESSION *Session,
    const KD_PACKET_VIEW *PacketView,
    KD_PACKET_SESSION_RESULT *Result);

/* Request retransmission after a receive timeout. */
KD_PACKET_STATUS
KdPacketSessionTimeout(
    KD_PACKET_SESSION *Session,
    KD_PACKET_SESSION_RESULT *Result);

/* Reject a malformed packet with a RESEND control packet. */
void
KdPacketSessionRejectMalformed(
    KD_PACKET_SESSION_RESULT *Result);

#ifdef __cplusplus
}
#endif
