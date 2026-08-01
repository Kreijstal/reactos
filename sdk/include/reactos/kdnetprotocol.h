/*
 * PROJECT:     ReactOS KDNET transport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Allocation-free KDNET packet protection and framing
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KDNET_CONTROL_KEY_SIZE       32
#define KDNET_DATA_KEY_SIZE          32
#define KDNET_HMAC_KEY_SIZE          32
#define KDNET_AUTHENTICATION_SIZE    16
#define KDNET_HEADER_SIZE             6
#define KDNET_DATA_SIZE               8
#define KDNET_BLOCK_SIZE             16
#define KDNET_CLIENT_KEY_SIZE         32
#define KDNET_HOST_KEY_SIZE           32
#define KDNET_CONTROL_POKE_SIZE       34
#define KDNET_CONTROL_RESPONSE_SIZE  322

#define KDNET_PACKET_TYPE_DATA        0
#define KDNET_PACKET_TYPE_CONTROL     1

#define KDNET_DIRECTION_TARGET        0
#define KDNET_DIRECTION_DEBUGGER      8

typedef enum _KDNET_STATUS
{
    KdNetStatusSuccess = 0,
    KdNetStatusInvalidParameter = -1,
    KdNetStatusInvalidKey = -2,
    KdNetStatusBufferTooSmall = -3,
    KdNetStatusInvalidPacket = -4,
    KdNetStatusAuthenticationFailed = -5,
    KdNetStatusDataKeyUnavailable = -6,
    KdNetStatusCryptoFailure = -7,
    KdNetStatusInvalidHandshake = -8
} KDNET_STATUS;

typedef struct _KDNET_CRYPTO_CONTEXT
{
    uint8_t ControlKey[KDNET_CONTROL_KEY_SIZE];
    uint8_t DataKey[KDNET_DATA_KEY_SIZE];
    uint8_t HmacKey[KDNET_HMAC_KEY_SIZE];
    uint8_t DataKeyValid;
} KDNET_CRYPTO_CONTEXT;

typedef struct _KDNET_PACKET_INFO
{
    uint8_t Version;
    uint8_t Type;
    uint8_t Direction;
    uint8_t PaddingLength;
    uint64_t SequenceNumber;
    uint8_t *Payload;
    size_t PayloadLength;
} KDNET_PACKET_INFO;

/* Initialize the control and HMAC keys from the four-part base-36 key. */
KDNET_STATUS
KdNetInitializeCryptoContext(
    KDNET_CRYPTO_CONTEXT *Context,
    const char *KeyString);

/* DataKey = SHA256(ControlKey || ResponsePayload). */
KDNET_STATUS
KdNetDeriveDataKey(
    KDNET_CRYPTO_CONTEXT *Context,
    const uint8_t *ResponsePayload,
    size_t ResponsePayloadLength);

/* Build the minimal target-to-debugger control Poke (01 01 || ClientKey). */
KDNET_STATUS
KdNetBuildPokePayload(
    const uint8_t ClientKey[KDNET_CLIENT_KEY_SIZE],
    uint8_t *Payload,
    size_t PayloadCapacity,
    size_t *PayloadLength);

/* Validate a Radare-compatible Response and activate the session data key. */
KDNET_STATUS
KdNetProcessResponsePayload(
    KDNET_CRYPTO_CONTEXT *Context,
    const uint8_t ClientKey[KDNET_CLIENT_KEY_SIZE],
    const uint8_t *ResponsePayload,
    size_t ResponsePayloadLength);

/* Return the exact output size needed by KdNetEncodePacket. */
KDNET_STATUS
KdNetGetPacketSize(
    size_t PayloadLength,
    size_t *PacketLength);

/*
 * Produce a complete UDP payload. The caller supplies all storage and no
 * allocation or operating-system service is used.
 */
KDNET_STATUS
KdNetEncodePacket(
    const KDNET_CRYPTO_CONTEXT *Context,
    uint8_t Version,
    uint8_t Type,
    uint64_t SequenceNumber,
    uint8_t Direction,
    const uint8_t *Payload,
    size_t PayloadLength,
    uint8_t *Packet,
    size_t PacketCapacity,
    size_t *PacketLength);

/*
 * Authenticate and decrypt a UDP payload in place. PacketInfo->Payload points
 * into Packet and remains valid as long as Packet does.
 */
KDNET_STATUS
KdNetDecodePacket(
    const KDNET_CRYPTO_CONTEXT *Context,
    uint8_t *Packet,
    size_t PacketLength,
    KDNET_PACKET_INFO *PacketInfo);

#ifdef __cplusplus
}
#endif
