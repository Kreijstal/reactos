/*
 * PROJECT:     ReactOS KDNET transport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Allocation-free KDNET packet protection and framing
 *
 * The wire behavior implemented here was independently derived from packet
 * captures and the interoperable host-side implementation in radare2. No
 * radare2 source code is included in this file.
 */

#include <reactos/kdnetprotocol.h>

#include <tomcrypt.h>

#include "sha256.h"

#define KDNET_MAGIC_0 'M'
#define KDNET_MAGIC_1 'D'
#define KDNET_MAGIC_2 'B'
#define KDNET_MAGIC_3 'G'

static void
KdNetCopyBytes(
    uint8_t *Destination,
    const uint8_t *Source,
    size_t Length)
{
    while (Length-- != 0)
        *Destination++ = *Source++;
}

static void
KdNetZeroBytes(
    void *Buffer,
    size_t Length)
{
    volatile uint8_t *Byte = (volatile uint8_t *)Buffer;

    while (Length-- != 0)
        *Byte++ = 0;
}

static uint64_t
KdNetReadBigEndian64(
    const uint8_t *Buffer)
{
    uint64_t Value = 0;
    size_t Index;

    for (Index = 0; Index < 8; ++Index)
        Value = (Value << 8) | Buffer[Index];
    return Value;
}

static void
KdNetWriteBigEndian64(
    uint8_t *Buffer,
    uint64_t Value)
{
    size_t Index;

    for (Index = 0; Index < 8; ++Index)
    {
        Buffer[7 - Index] = (uint8_t)Value;
        Value >>= 8;
    }
}

static void
KdNetWriteLittleEndian64(
    uint8_t *Buffer,
    uint64_t Value)
{
    size_t Index;

    for (Index = 0; Index < 8; ++Index)
    {
        Buffer[Index] = (uint8_t)Value;
        Value >>= 8;
    }
}

static int
KdNetBase36Digit(
    char Character)
{
    if (Character >= '0' && Character <= '9')
        return Character - '0';
    if (Character >= 'a' && Character <= 'z')
        return Character - 'a' + 10;
    if (Character >= 'A' && Character <= 'Z')
        return Character - 'A' + 10;
    return -1;
}

static KDNET_STATUS
KdNetDecodeKeyPart(
    const char *First,
    const char *Last,
    uint64_t *Decoded)
{
    uint64_t Value = 0;
    int Digit;
    size_t Length;

    if (First == Last)
        return KdNetStatusInvalidKey;
    Length = (size_t)(Last - First);
    if (Length > 13)
        return KdNetStatusInvalidKey;

    while (First != Last)
    {
        Digit = KdNetBase36Digit(*First++);
        if (Digit < 0 || Value > (UINT64_MAX - (uint64_t)Digit) / 36)
            return KdNetStatusInvalidKey;
        Value = Value * 36 + (uint64_t)Digit;
    }

    *Decoded = Value;
    return KdNetStatusSuccess;
}

static void
KdNetHmacSha256(
    const uint8_t Key[KDNET_HMAC_KEY_SIZE],
    const uint8_t *Input,
    size_t InputLength,
    uint8_t Digest[32])
{
    KDNET_SHA256_CONTEXT ShaContext;
    uint8_t Pad[64];
    uint8_t InnerDigest[32];
    size_t Index;

    for (Index = 0; Index < sizeof(Pad); ++Index)
        Pad[Index] = 0x36;
    for (Index = 0; Index < KDNET_HMAC_KEY_SIZE; ++Index)
        Pad[Index] ^= Key[Index];

    KdNetSha256Initialize(&ShaContext);
    KdNetSha256Update(&ShaContext, Pad, sizeof(Pad));
    KdNetSha256Update(&ShaContext, Input, InputLength);
    KdNetSha256Finish(&ShaContext, InnerDigest);

    for (Index = 0; Index < sizeof(Pad); ++Index)
        Pad[Index] = 0x5c;
    for (Index = 0; Index < KDNET_HMAC_KEY_SIZE; ++Index)
        Pad[Index] ^= Key[Index];

    KdNetSha256Initialize(&ShaContext);
    KdNetSha256Update(&ShaContext, Pad, sizeof(Pad));
    KdNetSha256Update(&ShaContext, InnerDigest, sizeof(InnerDigest));
    KdNetSha256Finish(&ShaContext, Digest);

    KdNetZeroBytes(Pad, sizeof(Pad));
    KdNetZeroBytes(InnerDigest, sizeof(InnerDigest));
}

static KDNET_STATUS
KdNetGetEncryptionKey(
    const KDNET_CRYPTO_CONTEXT *Context,
    uint8_t Type,
    const uint8_t **Key)
{
    if (Type == KDNET_PACKET_TYPE_CONTROL)
    {
        *Key = Context->ControlKey;
        return KdNetStatusSuccess;
    }

    if (Type == KDNET_PACKET_TYPE_DATA)
    {
        if (!Context->DataKeyValid)
            return KdNetStatusDataKeyUnavailable;
        *Key = Context->DataKey;
        return KdNetStatusSuccess;
    }

    return KdNetStatusInvalidParameter;
}

static KDNET_STATUS
KdNetAesCbcEncrypt(
    const uint8_t Key[KDNET_CONTROL_KEY_SIZE],
    const uint8_t InitializationVector[KDNET_BLOCK_SIZE],
    uint8_t *Buffer,
    size_t Length)
{
    aes_key AesKey;
    uint8_t Block[KDNET_BLOCK_SIZE];
    uint8_t Previous[KDNET_BLOCK_SIZE];
    size_t Offset, Index;

    if (aes_setup(Key, KDNET_CONTROL_KEY_SIZE, 0, &AesKey) != CRYPT_OK)
        return KdNetStatusCryptoFailure;

    KdNetCopyBytes(Previous, InitializationVector, sizeof(Previous));
    for (Offset = 0; Offset < Length; Offset += KDNET_BLOCK_SIZE)
    {
        for (Index = 0; Index < KDNET_BLOCK_SIZE; ++Index)
            Block[Index] = Buffer[Offset + Index] ^ Previous[Index];
        aes_ecb_encrypt(Block, Buffer + Offset, &AesKey);
        KdNetCopyBytes(Previous, Buffer + Offset, sizeof(Previous));
    }

    KdNetZeroBytes(&AesKey, sizeof(AesKey));
    KdNetZeroBytes(Block, sizeof(Block));
    KdNetZeroBytes(Previous, sizeof(Previous));
    return KdNetStatusSuccess;
}

static KDNET_STATUS
KdNetAesCbcDecrypt(
    const uint8_t Key[KDNET_CONTROL_KEY_SIZE],
    const uint8_t InitializationVector[KDNET_BLOCK_SIZE],
    uint8_t *Buffer,
    size_t Length)
{
    aes_key AesKey;
    uint8_t Block[KDNET_BLOCK_SIZE];
    uint8_t Ciphertext[KDNET_BLOCK_SIZE];
    uint8_t Previous[KDNET_BLOCK_SIZE];
    size_t Offset, Index;

    if (aes_setup(Key, KDNET_CONTROL_KEY_SIZE, 0, &AesKey) != CRYPT_OK)
        return KdNetStatusCryptoFailure;

    KdNetCopyBytes(Previous, InitializationVector, sizeof(Previous));
    for (Offset = 0; Offset < Length; Offset += KDNET_BLOCK_SIZE)
    {
        KdNetCopyBytes(Ciphertext, Buffer + Offset, sizeof(Ciphertext));
        aes_ecb_decrypt(Buffer + Offset, Block, &AesKey);
        for (Index = 0; Index < KDNET_BLOCK_SIZE; ++Index)
            Buffer[Offset + Index] = Block[Index] ^ Previous[Index];
        KdNetCopyBytes(Previous, Ciphertext, sizeof(Previous));
    }

    KdNetZeroBytes(&AesKey, sizeof(AesKey));
    KdNetZeroBytes(Block, sizeof(Block));
    KdNetZeroBytes(Ciphertext, sizeof(Ciphertext));
    KdNetZeroBytes(Previous, sizeof(Previous));
    return KdNetStatusSuccess;
}

KDNET_STATUS
KdNetInitializeCryptoContext(
    KDNET_CRYPTO_CONTEXT *Context,
    const char *KeyString)
{
    const char *First, *Last;
    uint64_t Decoded;
    KDNET_STATUS Status;
    size_t Part, Index;

    if (Context == NULL || KeyString == NULL)
        return KdNetStatusInvalidParameter;

    KdNetZeroBytes(Context, sizeof(*Context));
    First = KeyString;
    for (Part = 0; Part < 4; ++Part)
    {
        Last = First;
        while (*Last != '\0' && *Last != '.')
            ++Last;

        Status = KdNetDecodeKeyPart(First, Last, &Decoded);
        if (Status != KdNetStatusSuccess)
            goto Failure;

        KdNetWriteLittleEndian64(Context->ControlKey + Part * 8, Decoded);

        if (Part == 3)
        {
            if (*Last != '\0')
            {
                Status = KdNetStatusInvalidKey;
                goto Failure;
            }
        }
        else
        {
            if (*Last != '.')
            {
                Status = KdNetStatusInvalidKey;
                goto Failure;
            }
            First = Last + 1;
        }
    }

    for (Index = 0; Index < KDNET_HMAC_KEY_SIZE; ++Index)
        Context->HmacKey[Index] = (uint8_t)~Context->ControlKey[Index];
    return KdNetStatusSuccess;

Failure:
    KdNetZeroBytes(Context, sizeof(*Context));
    return Status;
}

KDNET_STATUS
KdNetDeriveDataKey(
    KDNET_CRYPTO_CONTEXT *Context,
    const uint8_t *ResponsePayload,
    size_t ResponsePayloadLength)
{
    KDNET_SHA256_CONTEXT ShaContext;

    if (Context == NULL ||
        (ResponsePayload == NULL && ResponsePayloadLength != 0))
    {
        return KdNetStatusInvalidParameter;
    }

    KdNetSha256Initialize(&ShaContext);
    KdNetSha256Update(&ShaContext,
                      Context->ControlKey,
                      sizeof(Context->ControlKey));
    KdNetSha256Update(&ShaContext, ResponsePayload, ResponsePayloadLength);
    KdNetSha256Finish(&ShaContext, Context->DataKey);
    Context->DataKeyValid = 1;
    return KdNetStatusSuccess;
}

KDNET_STATUS
KdNetBuildPokePayload(
    const uint8_t ClientKey[KDNET_CLIENT_KEY_SIZE],
    uint8_t *Payload,
    size_t PayloadCapacity,
    size_t *PayloadLength)
{
    if (ClientKey == NULL || Payload == NULL || PayloadLength == NULL)
        return KdNetStatusInvalidParameter;

    *PayloadLength = KDNET_CONTROL_POKE_SIZE;
    if (PayloadCapacity < KDNET_CONTROL_POKE_SIZE)
        return KdNetStatusBufferTooSmall;

    Payload[0] = 0x01;
    Payload[1] = 0x01;
    KdNetCopyBytes(Payload + 2, ClientKey, KDNET_CLIENT_KEY_SIZE);
    return KdNetStatusSuccess;
}

KDNET_STATUS
KdNetProcessResponsePayload(
    KDNET_CRYPTO_CONTEXT *Context,
    const uint8_t ClientKey[KDNET_CLIENT_KEY_SIZE],
    const uint8_t *ResponsePayload,
    size_t ResponsePayloadLength)
{
    uint8_t Difference = 0;
    size_t Index;

    if (Context == NULL || ClientKey == NULL || ResponsePayload == NULL)
        return KdNetStatusInvalidParameter;

    if (ResponsePayloadLength != KDNET_CONTROL_RESPONSE_SIZE ||
        ResponsePayload[0] != 0x01 || ResponsePayload[1] != 0x02)
    {
        return KdNetStatusInvalidHandshake;
    }

    for (Index = 0; Index < KDNET_CLIENT_KEY_SIZE; ++Index)
        Difference |= ClientKey[Index] ^ ResponsePayload[2 + Index];

    for (Index = 2 + KDNET_CLIENT_KEY_SIZE + KDNET_HOST_KEY_SIZE;
         Index < KDNET_CONTROL_RESPONSE_SIZE;
         ++Index)
    {
        Difference |= ResponsePayload[Index];
    }

    if (Difference != 0)
        return KdNetStatusInvalidHandshake;

    return KdNetDeriveDataKey(Context,
                              ResponsePayload,
                              ResponsePayloadLength);
}

KDNET_STATUS
KdNetGetPacketSize(
    size_t PayloadLength,
    size_t *PacketLength)
{
    size_t ProtectedLength;
    size_t PaddingLength;

    if (PacketLength == NULL)
        return KdNetStatusInvalidParameter;

    if (PayloadLength > (size_t)-1 - KDNET_DATA_SIZE -
                        (KDNET_BLOCK_SIZE - 1) - KDNET_HEADER_SIZE -
                        KDNET_AUTHENTICATION_SIZE)
    {
        return KdNetStatusInvalidParameter;
    }

    ProtectedLength = KDNET_DATA_SIZE + PayloadLength;
    PaddingLength = (KDNET_BLOCK_SIZE -
                     (ProtectedLength & (KDNET_BLOCK_SIZE - 1))) &
                    (KDNET_BLOCK_SIZE - 1);
    *PacketLength = KDNET_HEADER_SIZE + ProtectedLength + PaddingLength +
                    KDNET_AUTHENTICATION_SIZE;
    return KdNetStatusSuccess;
}

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
    size_t *PacketLength)
{
    const uint8_t *EncryptionKey;
    uint8_t Digest[32];
    uint64_t PacketData;
    size_t RequiredLength, ProtectedLength, PaddingLength, Index;
    KDNET_STATUS Status;

    if (Context == NULL || Packet == NULL || PacketLength == NULL ||
        (Payload == NULL && PayloadLength != 0) ||
        (Direction != KDNET_DIRECTION_TARGET &&
         Direction != KDNET_DIRECTION_DEBUGGER) ||
        SequenceNumber > (UINT64_MAX >> 8))
    {
        return KdNetStatusInvalidParameter;
    }

    Status = KdNetGetEncryptionKey(Context, Type, &EncryptionKey);
    if (Status != KdNetStatusSuccess)
        return Status;

    Status = KdNetGetPacketSize(PayloadLength, &RequiredLength);
    if (Status != KdNetStatusSuccess)
        return Status;
    *PacketLength = RequiredLength;
    if (PacketCapacity < RequiredLength)
        return KdNetStatusBufferTooSmall;

    ProtectedLength = RequiredLength - KDNET_HEADER_SIZE -
                      KDNET_AUTHENTICATION_SIZE;
    PaddingLength = ProtectedLength - KDNET_DATA_SIZE - PayloadLength;

    Packet[0] = KDNET_MAGIC_0;
    Packet[1] = KDNET_MAGIC_1;
    Packet[2] = KDNET_MAGIC_2;
    Packet[3] = KDNET_MAGIC_3;
    Packet[4] = Version;
    Packet[5] = Type;

    PacketData = (SequenceNumber << 8) |
                 ((uint64_t)Direction << 4) |
                 PaddingLength;
    KdNetWriteBigEndian64(Packet + KDNET_HEADER_SIZE, PacketData);
    KdNetCopyBytes(Packet + KDNET_HEADER_SIZE + KDNET_DATA_SIZE,
                   Payload,
                   PayloadLength);
    for (Index = 0; Index < PaddingLength; ++Index)
    {
        Packet[KDNET_HEADER_SIZE + KDNET_DATA_SIZE + PayloadLength + Index] = 0;
    }

    KdNetHmacSha256(Context->HmacKey,
                    Packet,
                    KDNET_HEADER_SIZE + ProtectedLength,
                    Digest);
    KdNetCopyBytes(Packet + KDNET_HEADER_SIZE + ProtectedLength,
                   Digest,
                   KDNET_AUTHENTICATION_SIZE);

    Status = KdNetAesCbcEncrypt(
        EncryptionKey,
        Packet + KDNET_HEADER_SIZE + ProtectedLength,
        Packet + KDNET_HEADER_SIZE,
        ProtectedLength);
    KdNetZeroBytes(Digest, sizeof(Digest));
    return Status;
}

KDNET_STATUS
KdNetDecodePacket(
    const KDNET_CRYPTO_CONTEXT *Context,
    uint8_t *Packet,
    size_t PacketLength,
    KDNET_PACKET_INFO *PacketInfo)
{
    const uint8_t *EncryptionKey;
    uint8_t Digest[32];
    uint8_t Difference = 0;
    uint64_t PacketData;
    size_t ProtectedLength, PaddingLength, PayloadLength, Index;
    KDNET_STATUS Status;

    if (Context == NULL || Packet == NULL || PacketInfo == NULL)
        return KdNetStatusInvalidParameter;

    if (PacketLength < KDNET_HEADER_SIZE + KDNET_BLOCK_SIZE +
                       KDNET_AUTHENTICATION_SIZE ||
        Packet[0] != KDNET_MAGIC_0 || Packet[1] != KDNET_MAGIC_1 ||
        Packet[2] != KDNET_MAGIC_2 || Packet[3] != KDNET_MAGIC_3)
    {
        return KdNetStatusInvalidPacket;
    }

    ProtectedLength = PacketLength - KDNET_HEADER_SIZE -
                      KDNET_AUTHENTICATION_SIZE;
    if ((ProtectedLength & (KDNET_BLOCK_SIZE - 1)) != 0)
        return KdNetStatusInvalidPacket;

    Status = KdNetGetEncryptionKey(Context, Packet[5], &EncryptionKey);
    if (Status == KdNetStatusInvalidParameter)
        return KdNetStatusInvalidPacket;
    if (Status != KdNetStatusSuccess)
        return Status;

    Status = KdNetAesCbcDecrypt(
        EncryptionKey,
        Packet + KDNET_HEADER_SIZE + ProtectedLength,
        Packet + KDNET_HEADER_SIZE,
        ProtectedLength);
    if (Status != KdNetStatusSuccess)
        return Status;

    KdNetHmacSha256(Context->HmacKey,
                    Packet,
                    KDNET_HEADER_SIZE + ProtectedLength,
                    Digest);
    for (Index = 0; Index < KDNET_AUTHENTICATION_SIZE; ++Index)
    {
        Difference |= Digest[Index] ^
                      Packet[KDNET_HEADER_SIZE + ProtectedLength + Index];
    }
    KdNetZeroBytes(Digest, sizeof(Digest));
    if (Difference != 0)
        return KdNetStatusAuthenticationFailed;

    PacketData = KdNetReadBigEndian64(Packet + KDNET_HEADER_SIZE);
    PaddingLength = (size_t)(PacketData & 0x0f);
    if (PaddingLength > ProtectedLength - KDNET_DATA_SIZE)
        return KdNetStatusInvalidPacket;

    PayloadLength = ProtectedLength - KDNET_DATA_SIZE - PaddingLength;
    for (Index = 0; Index < PaddingLength; ++Index)
    {
        if (Packet[KDNET_HEADER_SIZE + KDNET_DATA_SIZE +
                   PayloadLength + Index] != 0)
        {
            return KdNetStatusInvalidPacket;
        }
    }

    PacketInfo->Version = Packet[4];
    PacketInfo->Type = Packet[5];
    PacketInfo->Direction = (uint8_t)((PacketData >> 4) & 0x0f);
    if (PacketInfo->Direction != KDNET_DIRECTION_TARGET &&
        PacketInfo->Direction != KDNET_DIRECTION_DEBUGGER)
    {
        return KdNetStatusInvalidPacket;
    }
    PacketInfo->PaddingLength = (uint8_t)PaddingLength;
    PacketInfo->SequenceNumber = PacketData >> 8;
    PacketInfo->Payload = Packet + KDNET_HEADER_SIZE + KDNET_DATA_SIZE;
    PacketInfo->PayloadLength = PayloadLength;
    return KdNetStatusSuccess;
}
