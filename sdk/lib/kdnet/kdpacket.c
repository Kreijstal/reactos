/*
 * PROJECT:     ReactOS KD protocol
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Allocation-free inner KD packet framing
 */

#include <reactos/kdprotocol.h>

static uint16_t
KdPacketReadLittleEndian16(
    const uint8_t *Buffer)
{
    return (uint16_t)((uint16_t)Buffer[0] |
                      ((uint16_t)Buffer[1] << 8));
}

static uint32_t
KdPacketReadLittleEndian32(
    const uint8_t *Buffer)
{
    return (uint32_t)Buffer[0] |
           ((uint32_t)Buffer[1] << 8) |
           ((uint32_t)Buffer[2] << 16) |
           ((uint32_t)Buffer[3] << 24);
}

static void
KdPacketWriteLittleEndian16(
    uint8_t *Buffer,
    uint16_t Value)
{
    Buffer[0] = (uint8_t)Value;
    Buffer[1] = (uint8_t)(Value >> 8);
}

static void
KdPacketWriteLittleEndian32(
    uint8_t *Buffer,
    uint32_t Value)
{
    Buffer[0] = (uint8_t)Value;
    Buffer[1] = (uint8_t)(Value >> 8);
    Buffer[2] = (uint8_t)(Value >> 16);
    Buffer[3] = (uint8_t)(Value >> 24);
}

static void
KdPacketCopyBytes(
    uint8_t *Destination,
    const uint8_t *Source,
    size_t Length)
{
    while (Length-- != 0)
        *Destination++ = *Source++;
}

static int
KdPacketIsControlType(
    uint16_t Type)
{
    return Type == KD_PACKET_TYPE_ACKNOWLEDGE ||
           Type == KD_PACKET_TYPE_RESEND ||
           Type == KD_PACKET_TYPE_RESET;
}

uint32_t
KdPacketCalculateChecksum(
    const uint8_t *Buffer,
    size_t Length)
{
    uint32_t Checksum = 0;

    if (Buffer == NULL)
        return 0;

    while (Length-- != 0)
        Checksum += *Buffer++;
    return Checksum;
}

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
    size_t *PacketLength)
{
    size_t PayloadLength;
    uint32_t Checksum;

    if (Packet == NULL || PacketLength == NULL ||
        (MessageHeader == NULL && MessageHeaderLength != 0) ||
        (MessageData == NULL && MessageDataLength != 0) ||
        Type == KD_PACKET_TYPE_UNUSED || Type >= KD_PACKET_TYPE_MAX)
    {
        return KdPacketStatusInvalidParameter;
    }

    if (MessageHeaderLength > KD_PACKET_MAX_PAYLOAD ||
        MessageDataLength > KD_PACKET_MAX_PAYLOAD - MessageHeaderLength)
    {
        return KdPacketStatusPayloadTooLarge;
    }

    PayloadLength = MessageHeaderLength + MessageDataLength;
    *PacketLength = KD_PACKET_HEADER_SIZE + PayloadLength;
    if (PacketCapacity < *PacketLength)
        return KdPacketStatusBufferTooSmall;

    Checksum = KdPacketCalculateChecksum(MessageHeader, MessageHeaderLength) +
               KdPacketCalculateChecksum(MessageData, MessageDataLength);

    KdPacketWriteLittleEndian32(Packet, KD_PACKET_LEADER_DATA);
    KdPacketWriteLittleEndian16(Packet + 4, Type);
    KdPacketWriteLittleEndian16(Packet + 6, (uint16_t)PayloadLength);
    KdPacketWriteLittleEndian32(Packet + 8, Id);
    KdPacketWriteLittleEndian32(Packet + 12, Checksum);
    KdPacketCopyBytes(Packet + KD_PACKET_HEADER_SIZE,
                      MessageHeader,
                      MessageHeaderLength);
    KdPacketCopyBytes(Packet + KD_PACKET_HEADER_SIZE + MessageHeaderLength,
                      MessageData,
                      MessageDataLength);
    return KdPacketStatusSuccess;
}

KD_PACKET_STATUS
KdPacketEncodeControl(
    uint16_t Type,
    uint32_t Id,
    uint8_t *Packet,
    size_t PacketCapacity,
    size_t *PacketLength)
{
    if (Packet == NULL || PacketLength == NULL || !KdPacketIsControlType(Type))
        return KdPacketStatusInvalidParameter;

    *PacketLength = KD_PACKET_HEADER_SIZE;
    if (PacketCapacity < KD_PACKET_HEADER_SIZE)
        return KdPacketStatusBufferTooSmall;

    KdPacketWriteLittleEndian32(Packet, KD_PACKET_LEADER_CONTROL);
    KdPacketWriteLittleEndian16(Packet + 4, Type);
    KdPacketWriteLittleEndian16(Packet + 6, 0);
    KdPacketWriteLittleEndian32(Packet + 8, Id);
    KdPacketWriteLittleEndian32(Packet + 12, 0);
    return KdPacketStatusSuccess;
}

KD_PACKET_STATUS
KdPacketEncodeUnused(
    uint8_t *Packet,
    size_t PacketCapacity,
    size_t *PacketLength)
{
    size_t Index;

    if (Packet == NULL || PacketLength == NULL)
        return KdPacketStatusInvalidParameter;

    *PacketLength = KD_PACKET_UNUSED_SIZE;
    if (PacketCapacity < KD_PACKET_UNUSED_SIZE)
        return KdPacketStatusBufferTooSmall;

    for (Index = 0; Index < KD_PACKET_UNUSED_SIZE; ++Index)
        Packet[Index] = 0;
    return KdPacketStatusSuccess;
}

KD_PACKET_STATUS
KdPacketDecode(
    const uint8_t *Packet,
    size_t PacketLength,
    KD_PACKET_VIEW *PacketView)
{
    uint32_t Leader, Checksum;
    uint16_t Type, PayloadLength;

    if (Packet == NULL || PacketView == NULL)
        return KdPacketStatusInvalidParameter;

    if (PacketLength == KD_PACKET_UNUSED_SIZE)
    {
        size_t Index;

        for (Index = 0; Index < KD_PACKET_UNUSED_SIZE; ++Index)
        {
            if (Packet[Index] != 0)
                return KdPacketStatusInvalidPacket;
        }

        PacketView->Leader = KD_PACKET_LEADER_UNUSED;
        PacketView->Type = KD_PACKET_TYPE_UNUSED;
        PacketView->PayloadLength = 0;
        PacketView->Id = 0;
        PacketView->Checksum = 0;
        PacketView->Payload = NULL;
        return KdPacketStatusSuccess;
    }

    if (PacketLength < KD_PACKET_HEADER_SIZE)
        return KdPacketStatusInvalidPacket;

    Leader = KdPacketReadLittleEndian32(Packet);
    Type = KdPacketReadLittleEndian16(Packet + 4);
    PayloadLength = KdPacketReadLittleEndian16(Packet + 6);
    Checksum = KdPacketReadLittleEndian32(Packet + 12);

    if (PayloadLength > KD_PACKET_MAX_PAYLOAD ||
        PacketLength != (size_t)KD_PACKET_HEADER_SIZE + PayloadLength)
    {
        return KdPacketStatusInvalidPacket;
    }

    if (Leader == KD_PACKET_LEADER_CONTROL)
    {
        if (!KdPacketIsControlType(Type) || PayloadLength != 0 || Checksum != 0)
            return KdPacketStatusInvalidPacket;
    }
    else if (Leader == KD_PACKET_LEADER_DATA)
    {
        if (Type == KD_PACKET_TYPE_UNUSED || Type >= KD_PACKET_TYPE_MAX)
            return KdPacketStatusInvalidPacket;
        if (Checksum != KdPacketCalculateChecksum(Packet + KD_PACKET_HEADER_SIZE,
                                                   PayloadLength))
        {
            return KdPacketStatusChecksumMismatch;
        }
    }
    else
    {
        return KdPacketStatusInvalidPacket;
    }

    PacketView->Leader = Leader;
    PacketView->Type = Type;
    PacketView->PayloadLength = PayloadLength;
    PacketView->Id = KdPacketReadLittleEndian32(Packet + 8);
    PacketView->Checksum = Checksum;
    PacketView->Payload = Packet + KD_PACKET_HEADER_SIZE;
    return KdPacketStatusSuccess;
}
