/*
 * PROJECT:     ReactOS Kernel Debugger over Network
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Boot-time AMD64 KDNET transport for RTL8168/RTL8111
 */

#define NOEXTAPI
#include <ntifs.h>
#include <intrin.h>
#include <ndk/halfuncs.h>
#include <arc/arc.h>
#include <reactos/windbgkd.h>
#include <reactos/kddll.h>
#include <reactos/kdprotocol.h>
#include <reactos/kdnetprotocol.h>

#include "nic.h"

#define KDNET_ETH_HEADER_SIZE       14
#define KDNET_IPV4_HEADER_SIZE      20
#define KDNET_UDP_HEADER_SIZE        8
#define KDNET_ETH_MTU             1500
#define KDNET_IP_FRAGMENT_DATA    1480
#define KDNET_FRAME_CAPACITY      1514
#define KDNET_PACKET_CAPACITY     4128
#define KDNET_DATAGRAM_CAPACITY   (KDNET_PACKET_CAPACITY + KDNET_UDP_HEADER_SIZE)
#define KDNET_REASSEMBLY_CAPACITY 8192
#define KDNET_REASSEMBLY_BLOCKS   (KDNET_REASSEMBLY_CAPACITY / 8)
#define KDNET_REASSEMBLY_MAP_SIZE (KDNET_REASSEMBLY_BLOCKS / 8)
#define KDNET_DMA_ALIGNMENT        256
#define KDNET_DMA_LENGTH          (2 * KDNET_DMA_ALIGNMENT +                 \
                                   TX_DESC_COUNT * sizeof(RTL_DESC) +        \
                                   RX_DESC_COUNT * sizeof(RTL_DESC) +        \
                                   TX_DESC_COUNT * RX_BUF_SIZE +             \
                                   RX_DESC_COUNT * RX_BUF_SIZE)
#define KDNET_POLL_DELAY_US         50
#define KDNET_SHORT_WAIT_POLLS    5000
#define KDNET_LONG_WAIT_POLLS    20000
#define KDNET_HANDSHAKE_RETRIES      5

#define ETHERTYPE_IPV4          0x0800
#define ETHERTYPE_ARP           0x0806
#define ARP_HARDWARE_ETHERNET   0x0001
#define ARP_OPERATION_REQUEST   0x0001
#define ARP_OPERATION_REPLY     0x0002
#define IP_PROTOCOL_UDP             17
#define IP_FLAG_MORE_FRAGMENTS  0x2000
#define IP_FRAGMENT_OFFSET_MASK 0x1fff

typedef struct _KDNET_TRANSPORT
{
    DEBUG_DEVICE_DESCRIPTOR Device;
    RTL_ADAPTER Adapter;
    KDNET_CRYPTO_CONTEXT Crypto;
    KD_PACKET_SESSION Session;

    ULONG TargetIp;
    ULONG HostIp;
    USHORT Port;
    USHORT IpIdentifier;
    ULONGLONG SendSequence;
    UCHAR HostMac[IEEE_802_ADDR_LENGTH];
    UCHAR ClientKey[KDNET_CLIENT_KEY_SIZE];
    BOOLEAN HostMacValid;
    BOOLEAN Initialized;
    BOOLEAN BreakInPending;

    UCHAR Packet[KDNET_PACKET_CAPACITY];
    UCHAR InnerPacket[KD_PACKET_HEADER_SIZE + KD_PACKET_MAX_PAYLOAD];
    UCHAR LastInnerPacket[KD_PACKET_HEADER_SIZE + KD_PACKET_MAX_PAYLOAD];
    ULONG LastInnerPacketLength;
    UCHAR UdpDatagram[KDNET_DATAGRAM_CAPACITY];
    UCHAR Frame[KDNET_FRAME_CAPACITY];

    UCHAR Reassembly[KDNET_REASSEMBLY_CAPACITY];
    UCHAR ReassemblyMap[KDNET_REASSEMBLY_MAP_SIZE];
    USHORT ReassemblyId;
    ULONG ReassemblySource;
    ULONG ReassemblyLength;
    BOOLEAN ReassemblyActive;
    BOOLEAN ReassemblyLastSeen;
} KDNET_TRANSPORT;

static KDNET_TRANSPORT KdNet;

static USHORT
KdNetReadBe16(_In_reads_(2) const UCHAR *Buffer)
{
    return (USHORT)(((USHORT)Buffer[0] << 8) | Buffer[1]);
}

static ULONG
KdNetReadBe32(_In_reads_(4) const UCHAR *Buffer)
{
    return ((ULONG)Buffer[0] << 24) | ((ULONG)Buffer[1] << 16) |
           ((ULONG)Buffer[2] << 8) | Buffer[3];
}

static VOID
KdNetWriteBe16(_Out_writes_(2) UCHAR *Buffer, _In_ USHORT Value)
{
    Buffer[0] = (UCHAR)(Value >> 8);
    Buffer[1] = (UCHAR)Value;
}

static VOID
KdNetWriteBe32(_Out_writes_(4) UCHAR *Buffer, _In_ ULONG Value)
{
    Buffer[0] = (UCHAR)(Value >> 24);
    Buffer[1] = (UCHAR)(Value >> 16);
    Buffer[2] = (UCHAR)(Value >> 8);
    Buffer[3] = (UCHAR)Value;
}

static BOOLEAN
KdNetIsSpace(_In_ CHAR Character)
{
    return Character == 0 || Character == ' ' || Character == '\t' ||
           Character == '\r' || Character == '\n' || Character == '/';
}

static CHAR
KdNetUpper(_In_ CHAR Character)
{
    if (Character >= 'a' && Character <= 'z')
        return (CHAR)(Character - ('a' - 'A'));
    return Character;
}

static const CHAR *
KdNetFindOption(_In_opt_ const CHAR *Options, _In_ const CHAR *Name)
{
    const CHAR *Current;
    ULONG Index;

    if (Options == NULL)
        return NULL;

    for (Current = Options; *Current != 0; ++Current)
    {
        if (Current != Options && !KdNetIsSpace(Current[-1]))
            continue;

        for (Index = 0; Name[Index] != 0; ++Index)
        {
            if (KdNetUpper(Current[Index]) != KdNetUpper(Name[Index]))
                break;
        }
        if (Name[Index] == 0 && Current[Index] == '=')
            return Current + Index + 1;
    }
    return NULL;
}

static BOOLEAN
KdNetParseUnsigned(_In_ const CHAR *Text, _Out_ PULONG Value)
{
    ULONG Result = 0;
    ULONG Digits = 0;

    while (*Text >= '0' && *Text <= '9')
    {
        if (Result > (MAXULONG - (ULONG)(*Text - '0')) / 10)
            return FALSE;
        Result = Result * 10 + (ULONG)(*Text++ - '0');
        ++Digits;
    }
    if (Digits == 0 || (!KdNetIsSpace(*Text) && *Text != 0))
        return FALSE;
    *Value = Result;
    return TRUE;
}

static BOOLEAN
KdNetParseIpv4(_In_ const CHAR *Text, _Out_ PULONG Address)
{
    ULONG Part, Result = 0;
    ULONG Index;

    for (Index = 0; Index < 4; ++Index)
    {
        Part = 0;
        if (*Text < '0' || *Text > '9')
            return FALSE;
        do
        {
            Part = Part * 10 + (ULONG)(*Text++ - '0');
            if (Part > 255)
                return FALSE;
        } while (*Text >= '0' && *Text <= '9');

        Result = (Result << 8) | Part;
        if (Index != 3)
        {
            if (*Text++ != '.')
                return FALSE;
        }
    }
    if (!KdNetIsSpace(*Text) && *Text != 0)
        return FALSE;
    *Address = Result;
    return TRUE;
}

static LONG
KdNetHexDigit(_In_ CHAR Character)
{
    if (Character >= '0' && Character <= '9')
        return Character - '0';
    Character = KdNetUpper(Character);
    if (Character >= 'A' && Character <= 'F')
        return Character - 'A' + 10;
    return -1;
}

static BOOLEAN
KdNetParseMac(_In_ const CHAR *Text,
              _Out_writes_(IEEE_802_ADDR_LENGTH) UCHAR *Address)
{
    ULONG Index;
    LONG High, Low;

    for (Index = 0; Index < IEEE_802_ADDR_LENGTH; ++Index)
    {
        High = KdNetHexDigit(*Text++);
        Low = KdNetHexDigit(*Text++);
        if (High < 0 || Low < 0)
            return FALSE;
        Address[Index] = (UCHAR)((High << 4) | Low);
        if (Index != IEEE_802_ADDR_LENGTH - 1)
        {
            if (*Text != ':' && *Text != '-')
                return FALSE;
            ++Text;
        }
    }
    return KdNetIsSpace(*Text) || *Text == 0;
}

static BOOLEAN
KdNetCopyOption(_In_ const CHAR *Text,
                _Out_writes_(Capacity) CHAR *Buffer,
                _In_ ULONG Capacity)
{
    ULONG Length = 0;

    if (Capacity == 0)
        return FALSE;
    while (!KdNetIsSpace(*Text) && *Text != 0)
    {
        if (Length + 1 >= Capacity)
            return FALSE;
        Buffer[Length++] = *Text++;
    }
    if (Length == 0)
        return FALSE;
    Buffer[Length] = 0;
    return TRUE;
}

static USHORT
KdNetIpChecksum(_In_reads_bytes_(Length) const UCHAR *Buffer, _In_ ULONG Length)
{
    ULONG Sum = 0;

    while (Length >= 2)
    {
        Sum += KdNetReadBe16(Buffer);
        Buffer += 2;
        Length -= 2;
    }
    if (Length != 0)
        Sum += (ULONG)*Buffer << 8;
    while ((Sum >> 16) != 0)
        Sum = (Sum & 0xffff) + (Sum >> 16);
    return (USHORT)~Sum;
}

static BOOLEAN
KdNetHardwareSend(_In_reads_bytes_(Length) const UCHAR *Frame, _In_ ULONG Length)
{
    PRTL_DESC Descriptor;
    PUCHAR Buffer;
    PHYSICAL_ADDRESS Address;
    ULONG Index, Poll;

    if (Length > MAXIMUM_FRAME_SIZE)
        return FALSE;

    Index = KdNet.Adapter.TxProducer;
    Descriptor = &KdNet.Adapter.TxRing[Index];
    for (Poll = 0; Poll < KDNET_SHORT_WAIT_POLLS; ++Poll)
    {
        if ((*(volatile ULONG *)&Descriptor->opts1 & DESC_OWN) == 0)
            break;
        KeStallExecutionProcessor(KDNET_POLL_DELAY_US);
    }
    if (Poll == KDNET_SHORT_WAIT_POLLS)
        return FALSE;

    Buffer = KdNet.Adapter.TxBuffers + Index * RX_BUF_SIZE;
    RtlCopyMemory(Buffer, Frame, Length);
    if (Length < MINIMUM_FRAME_SIZE)
    {
        RtlZeroMemory(Buffer + Length, MINIMUM_FRAME_SIZE - Length);
        Length = MINIMUM_FRAME_SIZE;
    }
    Address.QuadPart = KdNet.Adapter.TxBuffersPa.QuadPart +
                       (ULONGLONG)Index * RX_BUF_SIZE;
    if (!NT_SUCCESS(NICTransmitDescriptor(&KdNet.Adapter, Index, Address, Length)))
        return FALSE;

    KdNet.Adapter.TxProducer = (Index + 1) % TX_DESC_COUNT;
    return TRUE;
}

static BOOLEAN
KdNetHardwareReceive(_Outptr_result_bytebuffer_(*Length) const UCHAR **Frame,
                     _Out_ PULONG Length)
{
    PRTL_DESC Descriptor;
    ULONG Options, FrameLength, Index;

    Index = KdNet.Adapter.RxConsumer;
    Descriptor = &KdNet.Adapter.RxRing[Index];
    Options = *(volatile ULONG *)&Descriptor->opts1;
    if (Options & DESC_OWN)
        return FALSE;

    KeMemoryBarrier();
    FrameLength = Options & RXD_LEN_MASK;
    if ((Options & (RXD_RES | DESC_FS | DESC_LS)) != (DESC_FS | DESC_LS) ||
        FrameLength < KDNET_ETH_HEADER_SIZE + 4 || FrameLength > RX_BUF_SIZE)
    {
        NICRefillRxDescriptor(&KdNet.Adapter, Index);
        KdNet.Adapter.RxConsumer = (Index + 1) % RX_DESC_COUNT;
        return FALSE;
    }

    *Frame = KdNet.Adapter.RxBuffers + Index * RX_BUF_SIZE;
    *Length = FrameLength - 4;
    return TRUE;
}

static VOID
KdNetHardwareReleaseReceive(VOID)
{
    ULONG Index = KdNet.Adapter.RxConsumer;

    NICRefillRxDescriptor(&KdNet.Adapter, Index);
    KdNet.Adapter.RxConsumer = (Index + 1) % RX_DESC_COUNT;
}

static VOID
KdNetBuildEthernetHeader(_Out_writes_(KDNET_ETH_HEADER_SIZE) UCHAR *Frame,
                         _In_reads_(IEEE_802_ADDR_LENGTH) const UCHAR *Destination,
                         _In_ USHORT EtherType)
{
    RtlCopyMemory(Frame, Destination, IEEE_802_ADDR_LENGTH);
    RtlCopyMemory(Frame + IEEE_802_ADDR_LENGTH,
                  KdNet.Adapter.CurrentMacAddress,
                  IEEE_802_ADDR_LENGTH);
    KdNetWriteBe16(Frame + 12, EtherType);
}

static BOOLEAN
KdNetSendArp(_In_ USHORT Operation,
             _In_reads_(IEEE_802_ADDR_LENGTH) const UCHAR *Destination,
             _In_reads_(IEEE_802_ADDR_LENGTH) const UCHAR *TargetHardware)
{
    UCHAR *Arp = KdNet.Frame + KDNET_ETH_HEADER_SIZE;

    KdNetBuildEthernetHeader(KdNet.Frame, Destination, ETHERTYPE_ARP);
    KdNetWriteBe16(Arp, ARP_HARDWARE_ETHERNET);
    KdNetWriteBe16(Arp + 2, ETHERTYPE_IPV4);
    Arp[4] = IEEE_802_ADDR_LENGTH;
    Arp[5] = 4;
    KdNetWriteBe16(Arp + 6, Operation);
    RtlCopyMemory(Arp + 8, KdNet.Adapter.CurrentMacAddress, IEEE_802_ADDR_LENGTH);
    KdNetWriteBe32(Arp + 14, KdNet.TargetIp);
    RtlCopyMemory(Arp + 18, TargetHardware, IEEE_802_ADDR_LENGTH);
    KdNetWriteBe32(Arp + 24, KdNet.HostIp);
    return KdNetHardwareSend(KdNet.Frame, KDNET_ETH_HEADER_SIZE + 28);
}

static VOID
KdNetHandleArp(_In_reads_bytes_(Length) const UCHAR *Frame, _In_ ULONG Length)
{
    const UCHAR *Arp;
    USHORT Operation;

    if (Length < KDNET_ETH_HEADER_SIZE + 28)
        return;
    Arp = Frame + KDNET_ETH_HEADER_SIZE;
    if (KdNetReadBe16(Arp) != ARP_HARDWARE_ETHERNET ||
        KdNetReadBe16(Arp + 2) != ETHERTYPE_IPV4 ||
        Arp[4] != IEEE_802_ADDR_LENGTH || Arp[5] != 4)
    {
        return;
    }

    Operation = KdNetReadBe16(Arp + 6);
    if (KdNetReadBe32(Arp + 14) == KdNet.HostIp)
    {
        RtlCopyMemory(KdNet.HostMac, Arp + 8, IEEE_802_ADDR_LENGTH);
        KdNet.HostMacValid = TRUE;
    }

    if (Operation == ARP_OPERATION_REQUEST &&
        KdNetReadBe32(Arp + 14) == KdNet.HostIp &&
        KdNetReadBe32(Arp + 24) == KdNet.TargetIp)
    {
        KdNetSendArp(ARP_OPERATION_REPLY, Arp + 8, Arp + 8);
    }
}

static BOOLEAN
KdNetSendUdp(_In_reads_bytes_(PayloadLength) const UCHAR *Payload,
             _In_ ULONG PayloadLength)
{
    UCHAR *Ip, *Udp;
    ULONG DatagramLength, Offset, FragmentLength, FrameLength;
    USHORT FragmentField, Identifier;

    if (!KdNet.HostMacValid || PayloadLength > KDNET_PACKET_CAPACITY)
        return FALSE;

    Udp = KdNet.UdpDatagram;
    KdNetWriteBe16(Udp, KdNet.Port);
    KdNetWriteBe16(Udp + 2, KdNet.Port);
    KdNetWriteBe16(Udp + 4, (USHORT)(KDNET_UDP_HEADER_SIZE + PayloadLength));
    KdNetWriteBe16(Udp + 6, 0);
    RtlCopyMemory(Udp + KDNET_UDP_HEADER_SIZE, Payload, PayloadLength);
    DatagramLength = KDNET_UDP_HEADER_SIZE + PayloadLength;
    Identifier = ++KdNet.IpIdentifier;

    for (Offset = 0; Offset < DatagramLength; Offset += FragmentLength)
    {
        FragmentLength = DatagramLength - Offset;
        if (FragmentLength > KDNET_IP_FRAGMENT_DATA)
            FragmentLength = KDNET_IP_FRAGMENT_DATA;

        KdNetBuildEthernetHeader(KdNet.Frame, KdNet.HostMac, ETHERTYPE_IPV4);
        Ip = KdNet.Frame + KDNET_ETH_HEADER_SIZE;
        RtlZeroMemory(Ip, KDNET_IPV4_HEADER_SIZE);
        Ip[0] = 0x45;
        KdNetWriteBe16(Ip + 2, (USHORT)(KDNET_IPV4_HEADER_SIZE + FragmentLength));
        KdNetWriteBe16(Ip + 4, Identifier);
        FragmentField = (USHORT)(Offset / 8);
        if (Offset + FragmentLength < DatagramLength)
            FragmentField |= IP_FLAG_MORE_FRAGMENTS;
        KdNetWriteBe16(Ip + 6, FragmentField);
        Ip[8] = 64;
        Ip[9] = IP_PROTOCOL_UDP;
        KdNetWriteBe32(Ip + 12, KdNet.TargetIp);
        KdNetWriteBe32(Ip + 16, KdNet.HostIp);
        KdNetWriteBe16(Ip + 10, KdNetIpChecksum(Ip, KDNET_IPV4_HEADER_SIZE));
        RtlCopyMemory(Ip + KDNET_IPV4_HEADER_SIZE,
                      KdNet.UdpDatagram + Offset,
                      FragmentLength);
        FrameLength = KDNET_ETH_HEADER_SIZE + KDNET_IPV4_HEADER_SIZE + FragmentLength;
        if (!KdNetHardwareSend(KdNet.Frame, FrameLength))
            return FALSE;
    }
    return TRUE;
}

static VOID
KdNetResetReassembly(_In_ USHORT Identifier, _In_ ULONG Source)
{
    RtlZeroMemory(KdNet.ReassemblyMap, sizeof(KdNet.ReassemblyMap));
    KdNet.ReassemblyId = Identifier;
    KdNet.ReassemblySource = Source;
    KdNet.ReassemblyLength = 0;
    KdNet.ReassemblyActive = TRUE;
    KdNet.ReassemblyLastSeen = FALSE;
}

static VOID
KdNetMarkReassembly(_In_ ULONG Offset, _In_ ULONG Length)
{
    ULONG First = Offset / 8;
    ULONG Last = (Offset + Length + 7) / 8;
    ULONG Block;

    for (Block = First; Block < Last && Block < KDNET_REASSEMBLY_BLOCKS; ++Block)
        KdNet.ReassemblyMap[Block / 8] |= (UCHAR)(1 << (Block % 8));
}

static BOOLEAN
KdNetReassemblyComplete(VOID)
{
    ULONG Blocks, Block;

    if (!KdNet.ReassemblyActive || !KdNet.ReassemblyLastSeen)
        return FALSE;
    Blocks = (KdNet.ReassemblyLength + 7) / 8;
    for (Block = 0; Block < Blocks; ++Block)
    {
        if ((KdNet.ReassemblyMap[Block / 8] & (1 << (Block % 8))) == 0)
            return FALSE;
    }
    return TRUE;
}

static BOOLEAN
KdNetExtractUdp(_In_reads_bytes_(DatagramLength) const UCHAR *Datagram,
                _In_ ULONG DatagramLength,
                _Out_writes_bytes_to_(Capacity, *PayloadLength) UCHAR *Payload,
                _In_ ULONG Capacity,
                _Out_ PULONG PayloadLength)
{
    ULONG UdpLength;

    if (DatagramLength < KDNET_UDP_HEADER_SIZE ||
        KdNetReadBe16(Datagram) != KdNet.Port ||
        KdNetReadBe16(Datagram + 2) != KdNet.Port)
    {
        return FALSE;
    }
    UdpLength = KdNetReadBe16(Datagram + 4);
    if (UdpLength < KDNET_UDP_HEADER_SIZE || UdpLength > DatagramLength ||
        UdpLength - KDNET_UDP_HEADER_SIZE > Capacity)
    {
        return FALSE;
    }
    *PayloadLength = UdpLength - KDNET_UDP_HEADER_SIZE;
    RtlCopyMemory(Payload, Datagram + KDNET_UDP_HEADER_SIZE, *PayloadLength);
    return TRUE;
}

static BOOLEAN
KdNetHandleIpv4(_In_reads_bytes_(Length) const UCHAR *Frame,
                _In_ ULONG Length,
                _Out_writes_bytes_to_(Capacity, *PayloadLength) UCHAR *Payload,
                _In_ ULONG Capacity,
                _Out_ PULONG PayloadLength)
{
    const UCHAR *Ip, *Fragment;
    ULONG HeaderLength, TotalLength, FragmentLength, FragmentOffset;
    ULONG Source, Destination;
    USHORT Identifier, FragmentField;

    if (Length < KDNET_ETH_HEADER_SIZE + KDNET_IPV4_HEADER_SIZE)
        return FALSE;
    Ip = Frame + KDNET_ETH_HEADER_SIZE;
    if ((Ip[0] >> 4) != 4 || Ip[9] != IP_PROTOCOL_UDP)
        return FALSE;
    HeaderLength = (Ip[0] & 0xf) * 4;
    TotalLength = KdNetReadBe16(Ip + 2);
    if (HeaderLength < KDNET_IPV4_HEADER_SIZE || TotalLength < HeaderLength ||
        KDNET_ETH_HEADER_SIZE + TotalLength > Length ||
        KdNetIpChecksum(Ip, HeaderLength) != 0)
    {
        return FALSE;
    }
    Source = KdNetReadBe32(Ip + 12);
    Destination = KdNetReadBe32(Ip + 16);
    if (Source != KdNet.HostIp || Destination != KdNet.TargetIp)
        return FALSE;

    Identifier = KdNetReadBe16(Ip + 4);
    FragmentField = KdNetReadBe16(Ip + 6);
    FragmentOffset = (FragmentField & IP_FRAGMENT_OFFSET_MASK) * 8;
    FragmentLength = TotalLength - HeaderLength;
    Fragment = Ip + HeaderLength;

    if ((FragmentField & IP_FLAG_MORE_FRAGMENTS) != 0 &&
        (FragmentLength & 7) != 0)
    {
        return FALSE;
    }

    if (FragmentOffset == 0 &&
        (FragmentField & IP_FLAG_MORE_FRAGMENTS) == 0)
    {
        return KdNetExtractUdp(Fragment, FragmentLength,
                               Payload, Capacity, PayloadLength);
    }

    if (FragmentOffset + FragmentLength > KDNET_REASSEMBLY_CAPACITY)
        return FALSE;
    if (!KdNet.ReassemblyActive || KdNet.ReassemblyId != Identifier ||
        KdNet.ReassemblySource != Source)
    {
        KdNetResetReassembly(Identifier, Source);
    }

    RtlCopyMemory(KdNet.Reassembly + FragmentOffset, Fragment, FragmentLength);
    KdNetMarkReassembly(FragmentOffset, FragmentLength);
    if ((FragmentField & IP_FLAG_MORE_FRAGMENTS) == 0)
    {
        KdNet.ReassemblyLength = FragmentOffset + FragmentLength;
        KdNet.ReassemblyLastSeen = TRUE;
    }
    if (!KdNetReassemblyComplete())
        return FALSE;

    KdNet.ReassemblyActive = FALSE;
    return KdNetExtractUdp(KdNet.Reassembly, KdNet.ReassemblyLength,
                           Payload, Capacity, PayloadLength);
}

static BOOLEAN
KdNetPollDatagram(_Out_writes_bytes_to_(Capacity, *PayloadLength) UCHAR *Payload,
                  _In_ ULONG Capacity,
                  _Out_ PULONG PayloadLength,
                  _In_ ULONG PollCount)
{
    const UCHAR *Frame;
    ULONG Length, Poll;
    USHORT EtherType;
    BOOLEAN Received;

    for (Poll = 0; Poll < PollCount; ++Poll)
    {
        if (!KdNetHardwareReceive(&Frame, &Length))
        {
            KeStallExecutionProcessor(KDNET_POLL_DELAY_US);
            continue;
        }

        Received = FALSE;
        if (Length >= KDNET_ETH_HEADER_SIZE)
        {
            EtherType = KdNetReadBe16(Frame + 12);
            if (EtherType == ETHERTYPE_ARP)
                KdNetHandleArp(Frame, Length);
            else if (EtherType == ETHERTYPE_IPV4 && Payload != NULL)
                Received = KdNetHandleIpv4(Frame, Length, Payload,
                                           Capacity, PayloadLength);
        }
        KdNetHardwareReleaseReceive();
        if (Received)
            return TRUE;
    }
    return FALSE;
}

static BOOLEAN
KdNetResolveHost(VOID)
{
    static const UCHAR Broadcast[IEEE_802_ADDR_LENGTH] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const UCHAR Empty[IEEE_802_ADDR_LENGTH] = { 0 };
    ULONG Attempt, IgnoredLength;

    if (KdNet.HostMacValid)
        return TRUE;
    for (Attempt = 0; Attempt < KDNET_HANDSHAKE_RETRIES; ++Attempt)
    {
        KdNetSendArp(ARP_OPERATION_REQUEST, Broadcast, Empty);
        KdNetPollDatagram(NULL, 0, &IgnoredLength, KDNET_SHORT_WAIT_POLLS);
        if (KdNet.HostMacValid)
            return TRUE;
    }
    return FALSE;
}

static VOID
KdNetGenerateClientKey(VOID)
{
    LARGE_INTEGER Counter, Frequency;
    ULONGLONG State;
    ULONG Index;

    Counter = KeQueryPerformanceCounter(&Frequency);
    State = (ULONGLONG)Counter.QuadPart ^ (ULONGLONG)Frequency.QuadPart ^
            (ULONGLONG)(ULONG_PTR)&KdNet;
    for (Index = 0; Index < IEEE_802_ADDR_LENGTH; ++Index)
        State ^= (ULONGLONG)KdNet.Adapter.CurrentMacAddress[Index] << (Index * 8);

    for (Index = 0; Index < KDNET_CLIENT_KEY_SIZE; ++Index)
    {
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        State += __rdtsc();
        KdNet.ClientKey[Index] = (UCHAR)(State >> ((Index & 7) * 8));
    }
}

static NTSTATUS
KdNetConfigureOptions(_In_opt_ const CHAR *Options)
{
    const CHAR *Value;
    CHAR Key[128];
    ULONG Port;
    KDNET_STATUS Status;

    KdNet.HostIp = 0xc0a8fa01;   /* 192.168.250.1 */
    KdNet.TargetIp = 0xc0a8fa02; /* 192.168.250.2 */
    KdNet.Port = 50000;

    Value = KdNetFindOption(Options, "HOSTIP");
    if (Value != NULL && !KdNetParseIpv4(Value, &KdNet.HostIp))
        return STATUS_INVALID_PARAMETER;
    Value = KdNetFindOption(Options, "TARGETIP");
    if (Value != NULL && !KdNetParseIpv4(Value, &KdNet.TargetIp))
        return STATUS_INVALID_PARAMETER;
    Value = KdNetFindOption(Options, "PORT");
    if (Value != NULL)
    {
        if (!KdNetParseUnsigned(Value, &Port) || Port == 0 || Port > 65535)
            return STATUS_INVALID_PARAMETER;
        KdNet.Port = (USHORT)Port;
    }
    Value = KdNetFindOption(Options, "HOSTMAC");
    if (Value != NULL)
    {
        if (!KdNetParseMac(Value, KdNet.HostMac))
            return STATUS_INVALID_PARAMETER;
        KdNet.HostMacValid = TRUE;
    }
    Value = KdNetFindOption(Options, "KEY");
    if (Value == NULL || !KdNetCopyOption(Value, Key, sizeof(Key)))
        return STATUS_INVALID_PARAMETER;
    Status = KdNetInitializeCryptoContext(&KdNet.Crypto, Key);
    RtlSecureZeroMemory(Key, sizeof(Key));
    if (Status != KdNetStatusSuccess)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS
KdNetInitializeHardware(_In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    NTSTATUS Status;
    ULONG Index;
    ULONG_PTR Base, Aligned;
    ULONGLONG Offset;
    PDEBUG_DEVICE_ADDRESS Address = NULL;

    RtlZeroMemory(&KdNet.Device, sizeof(KdNet.Device));
    KdNet.Device.Bus = MAXULONG;
    KdNet.Device.Slot = MAXULONG;
    KdNet.Device.VendorID = 0x10ec;
    KdNet.Device.DeviceID = 0x8168;
    KdNet.Device.BaseClass = 0xff;
    KdNet.Device.SubClass = 0xff;
    KdNet.Device.ProgIf = 0xff;
    KdNet.Device.Memory.Length = KDNET_DMA_LENGTH;
    KdNet.Device.Memory.MaxEnd.QuadPart = MAXULONG;
    KdNet.Device.Memory.Cached = FALSE;
    KdNet.Device.Memory.Aligned = TRUE;

    Status = KdSetupPciDeviceForDebugging(LoaderBlock, &KdNet.Device);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&KdNet.Adapter, sizeof(KdNet.Adapter));
    for (Index = 0; Index < MAXIMUM_DEBUG_BARS; ++Index)
    {
        if (KdNet.Device.BaseAddress[Index].Valid &&
            KdNet.Device.BaseAddress[Index].Type == CmResourceTypeMemory &&
            KdNet.Device.BaseAddress[Index].Length >= 0x100)
        {
            Address = &KdNet.Device.BaseAddress[Index];
            KdNet.Adapter.IoBaseIsMmio = TRUE;
            break;
        }
    }
    if (Address == NULL)
    {
        for (Index = 0; Index < MAXIMUM_DEBUG_BARS; ++Index)
        {
            if (KdNet.Device.BaseAddress[Index].Valid &&
                KdNet.Device.BaseAddress[Index].Type == CmResourceTypePort &&
                KdNet.Device.BaseAddress[Index].Length >= 0x100)
            {
                Address = &KdNet.Device.BaseAddress[Index];
                KdNet.Adapter.IoBaseIsMmio = FALSE;
                break;
            }
        }
    }
    if (Address == NULL || KdNet.Device.Memory.VirtualAddress == NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    KdNet.Adapter.IoBase = Address->TranslatedAddress;

    Base = (ULONG_PTR)KdNet.Device.Memory.VirtualAddress;
    Aligned = (Base + KDNET_DMA_ALIGNMENT - 1) & ~(KDNET_DMA_ALIGNMENT - 1);
    Offset = Aligned - Base;

#define KDNET_ASSIGN_DMA(_VaField, _PaField, _Bytes)                         \
    do                                                                       \
    {                                                                        \
        KdNet.Adapter._VaField = (PVOID)(Base + (ULONG_PTR)Offset);          \
        KdNet.Adapter._PaField.QuadPart =                                    \
            KdNet.Device.Memory.Start.QuadPart + Offset;                     \
        Offset += (_Bytes);                                                   \
        Offset = (Offset + KDNET_DMA_ALIGNMENT - 1) &                        \
                 ~(ULONGLONG)(KDNET_DMA_ALIGNMENT - 1);                       \
    } while (0)

    KDNET_ASSIGN_DMA(TxRing, TxRingPa, TX_DESC_COUNT * sizeof(RTL_DESC));
    KDNET_ASSIGN_DMA(RxRing, RxRingPa, RX_DESC_COUNT * sizeof(RTL_DESC));
    KDNET_ASSIGN_DMA(TxBuffers, TxBuffersPa, TX_DESC_COUNT * RX_BUF_SIZE);
    KDNET_ASSIGN_DMA(RxBuffers, RxBuffersPa, RX_DESC_COUNT * RX_BUF_SIZE);
#undef KDNET_ASSIGN_DMA

    if (Offset > KdNet.Device.Memory.Length)
        return STATUS_BUFFER_TOO_SMALL;

    Status = NICDetectChipVersion(&KdNet.Adapter);
    if (!NT_SUCCESS(Status))
        return Status;
    NICInitializeHw(&KdNet.Adapter);
    Status = NICSoftReset(&KdNet.Adapter);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlPllPowerUp(&KdNet.Adapter);
    Status = NICGetPermanentMacAddress(&KdNet.Adapter,
                                       KdNet.Adapter.PermanentMacAddress);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlCopyMemory(KdNet.Adapter.CurrentMacAddress,
                  KdNet.Adapter.PermanentMacAddress,
                  IEEE_802_ADDR_LENGTH);
    RtlHwPhyConfig(&KdNet.Adapter);
    KdNet.Adapter.PacketFilter = NDIS_PACKET_TYPE_DIRECTED |
                                 NDIS_PACKET_TYPE_BROADCAST;
    Status = NICProgramRings(&KdNet.Adapter);
    if (!NT_SUCCESS(Status))
        return Status;
    NICDisableInterrupts(&KdNet.Adapter);
    NICAcknowledgeInterrupts(&KdNet.Adapter, 0xffff);
    Status = NICEnableTxRx(&KdNet.Adapter);
    if (!NT_SUCCESS(Status))
        return Status;
    NICUpdateLinkStatus(&KdNet.Adapter);
    return STATUS_SUCCESS;
}

static BOOLEAN
KdNetSendProtected(_In_ UCHAR Type,
                   _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
                   _In_ ULONG PayloadLength)
{
    KDNET_STATUS Status;
    size_t PacketLength;

    Status = KdNetEncodePacket(&KdNet.Crypto,
                               1,
                               Type,
                               KdNet.SendSequence++,
                               KDNET_DIRECTION_TARGET,
                               Payload,
                               PayloadLength,
                               KdNet.Packet,
                               sizeof(KdNet.Packet),
                               &PacketLength);
    if (Status != KdNetStatusSuccess)
        return FALSE;
    return KdNetSendUdp(KdNet.Packet, (ULONG)PacketLength);
}

static NTSTATUS
KdNetHandshake(VOID)
{
    UCHAR Poke[KDNET_CONTROL_POKE_SIZE];
    KDNET_PACKET_INFO PacketInfo;
    KDNET_STATUS Status;
    KD_PACKET_STATUS KdStatus;
    size_t PokeLength, InnerLength;
    ULONG PacketLength, Attempt;
    BOOLEAN HandshakeComplete = FALSE;

    if (!KdNetResolveHost())
        return STATUS_HOST_UNREACHABLE;

    KdNetGenerateClientKey();
    Status = KdNetBuildPokePayload(KdNet.ClientKey,
                                   Poke,
                                   sizeof(Poke),
                                   &PokeLength);
    if (Status != KdNetStatusSuccess)
        return STATUS_UNSUCCESSFUL;

    KdNet.SendSequence = 1;
    for (Attempt = 0; Attempt < KDNET_HANDSHAKE_RETRIES; ++Attempt)
    {
        if (!KdNetSendProtected(KDNET_PACKET_TYPE_CONTROL,
                                Poke,
                                (ULONG)PokeLength))
        {
            continue;
        }
        if (!KdNetPollDatagram(KdNet.Packet,
                               sizeof(KdNet.Packet),
                               &PacketLength,
                               KDNET_LONG_WAIT_POLLS))
        {
            continue;
        }
        Status = KdNetDecodePacket(&KdNet.Crypto,
                                   KdNet.Packet,
                                   PacketLength,
                                   &PacketInfo);
        if (Status != KdNetStatusSuccess ||
            PacketInfo.Type != KDNET_PACKET_TYPE_CONTROL ||
            PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER)
        {
            continue;
        }
        Status = KdNetProcessResponsePayload(&KdNet.Crypto,
                                             KdNet.ClientKey,
                                             PacketInfo.Payload,
                                             PacketInfo.PayloadLength);
        if (Status == KdNetStatusSuccess)
        {
            HandshakeComplete = TRUE;
            break;
        }
    }
    if (!HandshakeComplete)
        return STATUS_IO_TIMEOUT;

    KdStatus = KdPacketEncodeUnused(KdNet.InnerPacket,
                                    sizeof(KdNet.InnerPacket),
                                    &InnerLength);
    if (KdStatus != KdPacketStatusSuccess ||
        !KdNetSendProtected(KDNET_PACKET_TYPE_DATA,
                            KdNet.InnerPacket,
                            (ULONG)InnerLength))
    {
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static BOOLEAN
KdNetSendControl(_In_ USHORT Type, _In_ ULONG Id)
{
    KD_PACKET_STATUS Status;
    size_t Length;

    Status = KdPacketEncodeControl(Type, Id,
                                   KdNet.InnerPacket,
                                   sizeof(KdNet.InnerPacket),
                                   &Length);
    return Status == KdPacketStatusSuccess &&
           KdNetSendProtected(KDNET_PACKET_TYPE_DATA,
                              KdNet.InnerPacket,
                              (ULONG)Length);
}

static KDSTATUS
KdNetReceiveExpected(_In_ ULONG ExpectedType,
                     _Out_opt_ PSTRING MessageHeader,
                     _Out_opt_ PSTRING MessageData,
                     _Out_opt_ PULONG DataLength,
                     _Inout_opt_ PKD_CONTEXT Context,
                     _In_ ULONG PollCount)
{
    KDNET_PACKET_INFO PacketInfo;
    KD_PACKET_VIEW View;
    KD_PACKET_SESSION_RESULT Result;
    KDNET_STATUS NetStatus;
    KD_PACKET_STATUS PacketStatus;
    ULONG PacketLength, HeaderLength, Remaining;

    if (ExpectedType == PACKET_TYPE_KD_POLL_BREAKIN && KdNet.BreakInPending)
    {
        KdNet.BreakInPending = FALSE;
        return KdPacketReceived;
    }

    for (;;)
    {
        if (!KdNetPollDatagram(KdNet.Packet,
                               sizeof(KdNet.Packet),
                               &PacketLength,
                               PollCount))
        {
            return KdPacketTimedOut;
        }
        NetStatus = KdNetDecodePacket(&KdNet.Crypto,
                                      KdNet.Packet,
                                      PacketLength,
                                      &PacketInfo);
        if (NetStatus != KdNetStatusSuccess ||
            PacketInfo.Type != KDNET_PACKET_TYPE_DATA ||
            PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER)
        {
            if (ExpectedType == PACKET_TYPE_KD_POLL_BREAKIN)
                PollCount = 1;
            continue;
        }

        if (PacketInfo.PayloadLength == 1 &&
            PacketInfo.Payload[0] == BREAKIN_PACKET_BYTE)
        {
            if (ExpectedType == PACKET_TYPE_KD_POLL_BREAKIN)
                return KdPacketReceived;
            KdNet.BreakInPending = TRUE;
            if (Context != NULL)
                Context->KdpControlCPending = TRUE;
            continue;
        }

        PacketStatus = KdPacketDecode(PacketInfo.Payload,
                                      PacketInfo.PayloadLength,
                                      &View);
        if (PacketStatus != KdPacketStatusSuccess)
        {
            KdPacketSessionRejectMalformed(&Result);
            KdNetSendControl(Result.ControlType, Result.ControlId);
            if (ExpectedType == PACKET_TYPE_KD_POLL_BREAKIN)
                return KdPacketTimedOut;
            continue;
        }
        PacketStatus = KdPacketSessionProcess(&KdNet.Session, &View, &Result);
        if (PacketStatus != KdPacketStatusSuccess)
            continue;

        if (Result.ControlType != KD_PACKET_SESSION_NO_CONTROL)
            KdNetSendControl(Result.ControlType, Result.ControlId);

        if (Result.Event == KdPacketSessionEventResendLast ||
            Result.Event == KdPacketSessionEventReset)
        {
            return KdPacketNeedsResend;
        }
        if (Result.Event == KdPacketSessionEventSendAcknowledged)
        {
            if (ExpectedType == PACKET_TYPE_KD_ACKNOWLEDGE)
                return KdPacketReceived;
            continue;
        }
        if (Result.Event != KdPacketSessionEventDataReceived ||
            View.Type != ExpectedType)
        {
            if (ExpectedType == PACKET_TYPE_KD_POLL_BREAKIN)
                return KdPacketTimedOut;
            continue;
        }

        if (MessageHeader == NULL)
            return KdPacketReceived;
        HeaderLength = MessageHeader->MaximumLength;
        if (View.PayloadLength < HeaderLength)
        {
            KdNetSendControl(PACKET_TYPE_KD_RESEND, 0);
            continue;
        }
        Remaining = View.PayloadLength - HeaderLength;
        if (MessageData != NULL && Remaining > MessageData->MaximumLength)
        {
            KdNetSendControl(PACKET_TYPE_KD_RESEND, 0);
            continue;
        }

        RtlCopyMemory(MessageHeader->Buffer, View.Payload, HeaderLength);
        MessageHeader->Length = (USHORT)HeaderLength;
        if (MessageData != NULL)
        {
            RtlCopyMemory(MessageData->Buffer,
                          View.Payload + HeaderLength,
                          Remaining);
            MessageData->Length = (USHORT)Remaining;
        }
        if (DataLength != NULL)
            *DataLength = Remaining;
        return KdPacketReceived;
    }
}

NTSTATUS
NTAPI
KdDebuggerInitialize0(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    NTSTATUS Status;

    if (LoaderBlock == NULL)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&KdNet, sizeof(KdNet));

    Status = KdNetConfigureOptions(LoaderBlock->LoadOptions);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = KdNetInitializeHardware(LoaderBlock);
    if (!NT_SUCCESS(Status))
        return Status;

    KdPacketSessionInitialize(&KdNet.Session, 3);
    Status = KdNetHandshake();
    if (!NT_SUCCESS(Status))
        return Status;
    KdNet.Initialized = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdDebuggerInitialize1(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNREFERENCED_PARAMETER(LoaderBlock);
    return KdNet.Initialized ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

VOID
NTAPI
KdSendPacket(_In_ ULONG PacketType,
             _In_ PSTRING MessageHeader,
             _In_opt_ PSTRING MessageData,
             _Inout_ PKD_CONTEXT Context)
{
    KD_PACKET_STATUS Status;
    KD_PACKET_SESSION_RESULT Result;
    KDSTATUS ReceiveStatus;
    uint32_t PacketId;
    ULONG Retry, Retries;
    size_t InnerLength;

    if (!KdNet.Initialized || MessageHeader == NULL)
        return;
    Status = KdPacketSessionBeginSend(&KdNet.Session, &PacketId);
    if (Status != KdPacketStatusSuccess)
        return;
    Status = KdPacketEncodeData((USHORT)PacketType,
                                PacketId,
                                (const UCHAR *)MessageHeader->Buffer,
                                MessageHeader->Length,
                                MessageData != NULL ?
                                    (const UCHAR *)MessageData->Buffer : NULL,
                                MessageData != NULL ? MessageData->Length : 0,
                                KdNet.LastInnerPacket,
                                sizeof(KdNet.LastInnerPacket),
                                &InnerLength);
    if (Status != KdPacketStatusSuccess)
    {
        KdPacketSessionInitialize(&KdNet.Session, 3);
        return;
    }
    KdNet.LastInnerPacketLength = (ULONG)InnerLength;

    Retries = Context != NULL ? Context->KdpDefaultRetries : 3;
    if (Retries == 0 || Retries > 10)
        Retries = 3;
    for (Retry = 0; Retry < Retries; ++Retry)
    {
        if (!KdNetSendProtected(KDNET_PACKET_TYPE_DATA,
                                KdNet.LastInnerPacket,
                                KdNet.LastInnerPacketLength))
        {
            continue;
        }
        ReceiveStatus = KdNetReceiveExpected(PACKET_TYPE_KD_ACKNOWLEDGE,
                                             NULL, NULL, NULL,
                                             Context,
                                             KDNET_LONG_WAIT_POLLS);
        if (ReceiveStatus == KdPacketReceived)
            return;
        if (ReceiveStatus == KdPacketTimedOut)
        {
            KdPacketSessionTimeout(&KdNet.Session, &Result);
            if (Result.Event == KdPacketSessionEventRetryLimit)
            {
                KdPacketSessionInitialize(&KdNet.Session, 3);
                return;
            }
        }
    }
    KdPacketSessionInitialize(&KdNet.Session, 3);
}

KDSTATUS
NTAPI
KdReceivePacket(_In_ ULONG PacketType,
                _Out_opt_ PSTRING MessageHeader,
                _Out_opt_ PSTRING MessageData,
                _Out_opt_ PULONG DataLength,
                _Inout_opt_ PKD_CONTEXT Context)
{
    ULONG PollCount;

    if (!KdNet.Initialized)
        return KdPacketTimedOut;
    PollCount = (PacketType == PACKET_TYPE_KD_POLL_BREAKIN) ? 1 :
                                                              KDNET_LONG_WAIT_POLLS;
    return KdNetReceiveExpected(PacketType,
                                MessageHeader,
                                MessageData,
                                DataLength,
                                Context,
                                PollCount);
}

NTSTATUS NTAPI KdD0Transition(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI KdD3Transition(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI KdSave(_In_ BOOLEAN SleepTransition)
{
    UNREFERENCED_PARAMETER(SleepTransition);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI KdRestore(_In_ BOOLEAN SleepTransition)
{
    UNREFERENCED_PARAMETER(SleepTransition);
    return STATUS_SUCCESS;
}
