/*
 * PROJECT:     ReactOS Kernel Debugger over Network
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Boot-time AMD64 KDNET transport for Ethernet adapters
 */

#define NOEXTAPI
#include <ntifs.h>
#include <ntstrsafe.h>
#include <intrin.h>
#include <stdarg.h>
#include <ndk/halfuncs.h>
#include <ndk/inbvfuncs.h>
#include <ndk/ldrtypes.h>
#include <arc/arc.h>
#include <reactos/windbgkd.h>
#include <reactos/kddll.h>
#include <reactos/kdprotocol.h>
#include <reactos/kdnetprotocol.h>
#include <reactos/kdnetshare.h>

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
#define KDNET_RTL_DMA_LENGTH      (2 * KDNET_DMA_ALIGNMENT +                 \
                                   TX_DESC_COUNT * sizeof(RTL_DESC) +        \
                                   RX_DESC_COUNT * sizeof(RTL_DESC) +        \
                                   TX_DESC_COUNT * RX_BUF_SIZE +             \
                                   RX_DESC_COUNT * RX_BUF_SIZE)
#define KDNET_E1000_DMA_LENGTH    (2 * KDNET_DMA_ALIGNMENT +                 \
                                   2 * KDNET_E1000_DESC_COUNT * 16 +         \
                                   2 * KDNET_E1000_DESC_COUNT *              \
                                       KDNET_E1000_BUFFER_SIZE)
#define KDNET_DMA_LENGTH          ((KDNET_RTL_DMA_LENGTH >                   \
                                    KDNET_E1000_DMA_LENGTH) ?                \
                                   KDNET_RTL_DMA_LENGTH :                    \
                                   KDNET_E1000_DMA_LENGTH)
#define KDNET_POLL_DELAY_US         50
#define KDNET_SHORT_WAIT_POLLS    5000
#define KDNET_LONG_WAIT_POLLS    20000
/*
 * Early firmware/bridge setup and a userspace debugger can add seconds of
 * latency before the first receive descriptor becomes visible.  Keep the
 * retry window bounded, but long enough that a debugger prepared before boot
 * does not lose the session merely because its first response was delayed.
 */
#define KDNET_HANDSHAKE_RETRIES     30
#define KDNET_DIAGNOSTIC_PORT    50001
#define KDNET_DIAGNOSTIC_CHUNK    1024
#define KDNET_TRACE_BUFFER_SIZE   8192
#define KDNET_HARVEST_LINE_SIZE    768
#define KDNET_HARVEST_COMMAND_SIZE 128
#define KDNET_HARVEST_PREFIX       "KDNET-HARVEST/1 "

#define KDNET_HARVEST_STATUS       0x00000001
#define KDNET_HARVEST_NIC          0x00000002
#define KDNET_HARVEST_PCI          0x00000004
#define KDNET_HARVEST_LOADER       0x00000008
#define KDNET_HARVEST_COMMANDS     0x00000010
#define KDNET_HARVEST_USB          0x00000020
#define KDNET_HARVEST_ALL          (KDNET_HARVEST_STATUS | \
                                    KDNET_HARVEST_NIC | \
                                    KDNET_HARVEST_PCI | \
                                    KDNET_HARVEST_LOADER | \
                                    KDNET_HARVEST_COMMANDS | \
                                    KDNET_HARVEST_USB)

#define KDNET_E1000_DESC_COUNT      128
#define KDNET_E1000_BUFFER_SIZE    2048

#define KDNET_E1000_REG_CTRL     0x0000
#define KDNET_E1000_REG_STATUS   0x0008
#define KDNET_E1000_REG_EERD     0x0014
#define KDNET_E1000_REG_ICR      0x00c0
#define KDNET_E1000_REG_IMC      0x00d8
#define KDNET_E1000_REG_RCTL     0x0100
#define KDNET_E1000_REG_TCTL     0x0400
#define KDNET_E1000_REG_TIPG     0x0410
#define KDNET_E1000_REG_RDBAL    0x2800
#define KDNET_E1000_REG_RDBAH    0x2804
#define KDNET_E1000_REG_RDLEN    0x2808
#define KDNET_E1000_REG_RDH      0x2810
#define KDNET_E1000_REG_RDT      0x2818
#define KDNET_E1000_REG_TDBAL    0x3800
#define KDNET_E1000_REG_TDBAH    0x3804
#define KDNET_E1000_REG_TDLEN    0x3808
#define KDNET_E1000_REG_TDH      0x3810
#define KDNET_E1000_REG_TDT      0x3818
#define KDNET_E1000_REG_RAL      0x5400
#define KDNET_E1000_REG_RAH      0x5404

#define KDNET_E1000_CTRL_ASDE        (1UL << 5)
#define KDNET_E1000_CTRL_SLU         (1UL << 6)
#define KDNET_E1000_CTRL_LRST        (1UL << 3)
#define KDNET_E1000_CTRL_RST         (1UL << 26)
#define KDNET_E1000_CTRL_VME         (1UL << 30)
#define KDNET_E1000_RCTL_EN          (1UL << 1)
#define KDNET_E1000_RCTL_BAM         (1UL << 15)
#define KDNET_E1000_RCTL_SECRC       (1UL << 26)
#define KDNET_E1000_TCTL_EN          (1UL << 1)
#define KDNET_E1000_TCTL_PSP         (1UL << 3)
#define KDNET_E1000_RAH_AV           (1UL << 31)
#define KDNET_E1000_EERD_START       (1UL << 0)
#define KDNET_E1000_EERD_DONE        (1UL << 4)
#define KDNET_E1000_EERD_ADDR_SHIFT  8
#define KDNET_E1000_EERD_DATA_SHIFT 16
#define KDNET_E1000_RX_DD             0x01
#define KDNET_E1000_RX_EOP            0x02
#define KDNET_E1000_TX_DD             0x01
#define KDNET_E1000_TX_EOP            0x01
#define KDNET_E1000_TX_IFCS           0x02
#define KDNET_E1000_TX_RS             0x08

#define KDNET_E1000_TIPG_DEFAULT ((10UL << 0) | (10UL << 10) | (10UL << 20))

#define ETHERTYPE_IPV4          0x0800
#define ETHERTYPE_ARP           0x0806
#define ARP_HARDWARE_ETHERNET   0x0001
#define ARP_OPERATION_REQUEST   0x0001
#define ARP_OPERATION_REPLY     0x0002
#define IP_PROTOCOL_UDP             17
#define IP_FLAG_MORE_FRAGMENTS  0x2000
#define IP_FRAGMENT_OFFSET_MASK 0x1fff

typedef enum _KDNET_BACKEND
{
    KdNetBackendNone,
    KdNetBackendRtl8168,
    KdNetBackendE1000
} KDNET_BACKEND;

#include <pshpack1.h>
typedef struct _KDNET_E1000_RX_DESCRIPTOR
{
    ULONGLONG Address;
    USHORT Length;
    USHORT Checksum;
    UCHAR Status;
    UCHAR Errors;
    USHORT Special;
} KDNET_E1000_RX_DESCRIPTOR, *PKDNET_E1000_RX_DESCRIPTOR;

typedef struct _KDNET_E1000_TX_DESCRIPTOR
{
    ULONGLONG Address;
    USHORT Length;
    UCHAR ChecksumOffset;
    UCHAR Command;
    UCHAR Status;
    UCHAR ChecksumStart;
    USHORT Special;
} KDNET_E1000_TX_DESCRIPTOR, *PKDNET_E1000_TX_DESCRIPTOR;
#include <poppack.h>

C_ASSERT(sizeof(KDNET_E1000_RX_DESCRIPTOR) == 16);
C_ASSERT(sizeof(KDNET_E1000_TX_DESCRIPTOR) == 16);

typedef struct _KDNET_E1000_ADAPTER
{
    PUCHAR IoBase;
    PUCHAR IoPort;
    PKDNET_E1000_TX_DESCRIPTOR TxRing;
    PHYSICAL_ADDRESS TxRingPa;
    PKDNET_E1000_RX_DESCRIPTOR RxRing;
    PHYSICAL_ADDRESS RxRingPa;
    PUCHAR TxBuffers;
    PHYSICAL_ADDRESS TxBuffersPa;
    PUCHAR RxBuffers;
    PHYSICAL_ADDRESS RxBuffersPa;
    ULONG TxProducer;
    ULONG RxConsumer;
} KDNET_E1000_ADAPTER;

typedef struct _KDNET_TRANSPORT
{
    PLOADER_PARAMETER_BLOCK LoaderBlock;
    DEBUG_DEVICE_DESCRIPTOR Device;
    KDNET_BACKEND Backend;
    RTL_ADAPTER Rtl8168;
    KDNET_E1000_ADAPTER E1000;
    KDNET_CRYPTO_CONTEXT Crypto;
    KD_PACKET_SESSION Session;

    ULONG TargetIp;
    ULONG HostIp;
    USHORT Port;
    USHORT IpIdentifier;
    ULONGLONG SendSequence;
    UCHAR HostMac[IEEE_802_ADDR_LENGTH];
    UCHAR TargetMac[IEEE_802_ADDR_LENGTH];
    UCHAR ClientKey[KDNET_CLIENT_KEY_SIZE];
    BOOLEAN HostMacValid;
    BOOLEAN Initialized;
    BOOLEAN BreakInPending;

    /* Adapter sharing.  ShareEnabled reflects /KDNETSHARE and is decided once,
     * during option parsing; Share is allocated only when a miniport actually
     * registers, so a boot without the option carries no share state at all
     * and the only cost on the debugger's paths is one NULL pointer test. */
    BOOLEAN ShareEnabled;
    struct _KDNET_SHARE_STATE *Share;

    ULONG HarvestFlags;
    ULONG HarvestRequestId;
    ULONG HarvestActiveId;
    ULONG HarvestCommandsReceived;
    ULONG HarvestResponsesSent;
    ULONGLONG FramesSent;
    ULONGLONG FramesReceived;
    ULONGLONG FramesDropped;

    CHAR TraceBuffer[KDNET_TRACE_BUFFER_SIZE];
    ULONG TraceLength;
    ULONG TraceFlushed;

    UCHAR Packet[KDNET_PACKET_CAPACITY];
    UCHAR SendPacket[KDNET_PACKET_CAPACITY];
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

static VOID
KdNetEmitTrace(_In_ PCSTR Message)
{
    InbvEnableDisplayString(TRUE);
    HalDisplayString(Message);

    while (*Message != ANSI_NULL && KdNet.TraceLength < sizeof(KdNet.TraceBuffer))
        KdNet.TraceBuffer[KdNet.TraceLength++] = *Message++;
}

static VOID
KdNetTrace(_In_ PCSTR Message)
{
    CHAR Buffer[256];

    if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                      sizeof(Buffer),
                                      "[KDNET] %s\r\n",
                                      Message)))
    {
        KdNetEmitTrace(Buffer);
    }

}

static VOID
KdNetTraceStatus(_In_ PCSTR Stage, _In_ NTSTATUS Status)
{
    CHAR Buffer[192];

    if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                      sizeof(Buffer),
                                      "[KDNET] %s: 0x%08lx\r\n",
                                      Stage,
                                      Status)))
    {
        KdNetEmitTrace(Buffer);
    }

}

static VOID
KdNetTraceConfiguration(VOID)
{
    CHAR Buffer[192];

    if (NT_SUCCESS(RtlStringCbPrintfA(
            Buffer,
            sizeof(Buffer),
            "[KDNET] static IPv4 (no DHCP): target %lu.%lu.%lu.%lu "
            "-> host %lu.%lu.%lu.%lu UDP %u\r\n",
            (KdNet.TargetIp >> 24) & 0xff,
            (KdNet.TargetIp >> 16) & 0xff,
            (KdNet.TargetIp >> 8) & 0xff,
            KdNet.TargetIp & 0xff,
            (KdNet.HostIp >> 24) & 0xff,
            (KdNet.HostIp >> 16) & 0xff,
            (KdNet.HostIp >> 8) & 0xff,
            KdNet.HostIp & 0xff,
            KdNet.Port)))
    {
        KdNetEmitTrace(Buffer);
    }

    if (NT_SUCCESS(RtlStringCbPrintfA(
            Buffer,
            sizeof(Buffer),
            "[KDNET] harvest flags 0x%08lx, command UDP %u\r\n",
            KdNet.HarvestFlags,
            KDNET_DIAGNOSTIC_PORT)))
    {
        KdNetEmitTrace(Buffer);
    }
}

static VOID
KdNetTraceDevice(_In_ PCSTR Name)
{
    CHAR Buffer[192];
    ULONG Index;

    if (NT_SUCCESS(RtlStringCbPrintfA(
            Buffer,
            sizeof(Buffer),
            "[KDNET] %s found: PCI %lu:%lu vendor %04x device %04x, "
            "DMA VA %p PA %08lx:%08lx length %lu\r\n",
            Name,
            KdNet.Device.Bus,
            KdNet.Device.Slot,
            KdNet.Device.VendorID,
            KdNet.Device.DeviceID,
            KdNet.Device.Memory.VirtualAddress,
            KdNet.Device.Memory.Start.HighPart,
            KdNet.Device.Memory.Start.LowPart,
            KdNet.Device.Memory.Length)))
    {
        KdNetEmitTrace(Buffer);
    }

    for (Index = 0; Index < MAXIMUM_DEBUG_BARS; ++Index)
    {
        if (!KdNet.Device.BaseAddress[Index].Valid)
            continue;
        if (NT_SUCCESS(RtlStringCbPrintfA(
                Buffer,
                sizeof(Buffer),
                "[KDNET] BAR%lu: type %u address %p length 0x%lx\r\n",
                Index,
                KdNet.Device.BaseAddress[Index].Type,
                KdNet.Device.BaseAddress[Index].TranslatedAddress,
                KdNet.Device.BaseAddress[Index].Length)))
        {
            KdNetEmitTrace(Buffer);
        }
    }
}

static VOID
KdNetTraceMac(_In_ PCSTR Label,
              _In_reads_(IEEE_802_ADDR_LENGTH) const UCHAR *Address)
{
    CHAR Buffer[128];

    if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                      sizeof(Buffer),
                                      "[KDNET] %s %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                                      Label,
                                      Address[0], Address[1], Address[2],
                                      Address[3], Address[4], Address[5])))
    {
        KdNetEmitTrace(Buffer);
    }
}

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

/* KdNetFindOption only matches "NAME=value" forms.  Valueless switches such as
 * /KDNETSHARE need their own lookup: the name must be followed by a separator
 * (KdNetIsSpace counts the terminating NUL and the next option's '/'), so
 * /KDNETSHARE does not match a hypothetical /KDNETSHAREFOO. */
static BOOLEAN
KdNetHasFlag(_In_opt_ const CHAR *Options, _In_ const CHAR *Name)
{
    const CHAR *Current;
    ULONG Index;

    if (Options == NULL)
        return FALSE;

    for (Current = Options; *Current != 0; ++Current)
    {
        if (Current != Options && !KdNetIsSpace(Current[-1]))
            continue;

        for (Index = 0; Name[Index] != 0; ++Index)
        {
            if (KdNetUpper(Current[Index]) != KdNetUpper(Name[Index]))
                break;
        }
        if (Name[Index] == 0 && KdNetIsSpace(Current[Index]))
            return TRUE;
    }
    return FALSE;
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

static BOOLEAN
KdNetOptionTokenEquals(_In_reads_(Length) const CHAR *Token,
                       _In_ ULONG Length,
                       _In_ PCSTR Expected)
{
    ULONG Index;

    for (Index = 0; Index < Length && Expected[Index] != ANSI_NULL; ++Index)
    {
        if (KdNetUpper(Token[Index]) != KdNetUpper(Expected[Index]))
            return FALSE;
    }
    return Index == Length && Expected[Index] == ANSI_NULL;
}

static BOOLEAN
KdNetParseHarvestFlags(_In_ const CHAR *Text, _Out_ PULONG Flags)
{
    const CHAR *Token;
    ULONG Length, Result = 0;

    while (!KdNetIsSpace(*Text) && *Text != ANSI_NULL)
    {
        Token = Text;
        while (*Text != ',' && *Text != '+' &&
               !KdNetIsSpace(*Text) && *Text != ANSI_NULL)
        {
            ++Text;
        }
        Length = (ULONG)(Text - Token);
        if (Length == 0)
            return FALSE;

        if (KdNetOptionTokenEquals(Token, Length, "ALL"))
            Result |= KDNET_HARVEST_ALL;
        else if (KdNetOptionTokenEquals(Token, Length, "STATUS"))
            Result |= KDNET_HARVEST_STATUS;
        else if (KdNetOptionTokenEquals(Token, Length, "NIC"))
            Result |= KDNET_HARVEST_NIC;
        else if (KdNetOptionTokenEquals(Token, Length, "PCI"))
            Result |= KDNET_HARVEST_PCI;
        else if (KdNetOptionTokenEquals(Token, Length, "USB"))
            Result |= KDNET_HARVEST_USB;
        else if (KdNetOptionTokenEquals(Token, Length, "LOADER"))
            Result |= KDNET_HARVEST_LOADER;
        else if (KdNetOptionTokenEquals(Token, Length, "COMMANDS"))
            Result |= KDNET_HARVEST_COMMANDS;
        else if (!KdNetOptionTokenEquals(Token, Length, "NONE") &&
                 !KdNetOptionTokenEquals(Token, Length, "OFF"))
            return FALSE;

        if (*Text == ',' || *Text == '+')
            ++Text;
    }

    *Flags = Result;
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
KdNetRtl8168Send(_In_reads_bytes_(Length) const UCHAR *Frame, _In_ ULONG Length)
{
    PRTL_DESC Descriptor;
    PUCHAR Buffer;
    PHYSICAL_ADDRESS Address;
    ULONG Index, Poll;

    if (Length > MAXIMUM_FRAME_SIZE)
        return FALSE;

    Index = KdNet.Rtl8168.TxProducer;
    Descriptor = &KdNet.Rtl8168.TxRing[Index];
    for (Poll = 0; Poll < KDNET_SHORT_WAIT_POLLS; ++Poll)
    {
        if ((*(volatile ULONG *)&Descriptor->opts1 & DESC_OWN) == 0)
            break;
        KeStallExecutionProcessor(KDNET_POLL_DELAY_US);
    }
    if (Poll == KDNET_SHORT_WAIT_POLLS)
        return FALSE;

    Buffer = KdNet.Rtl8168.TxBuffers + Index * RX_BUF_SIZE;
    RtlCopyMemory(Buffer, Frame, Length);
    if (Length < MINIMUM_FRAME_SIZE)
    {
        RtlZeroMemory(Buffer + Length, MINIMUM_FRAME_SIZE - Length);
        Length = MINIMUM_FRAME_SIZE;
    }
    Address.QuadPart = KdNet.Rtl8168.TxBuffersPa.QuadPart +
                       (ULONGLONG)Index * RX_BUF_SIZE;
    if (!NT_SUCCESS(NICTransmitDescriptor(&KdNet.Rtl8168, Index, Address, Length)))
        return FALSE;

    KdNet.Rtl8168.TxProducer = (Index + 1) % TX_DESC_COUNT;
    return TRUE;
}

static BOOLEAN
KdNetRtl8168Receive(_Outptr_result_bytebuffer_(*Length) const UCHAR **Frame,
                    _Out_ PULONG Length)
{
    PRTL_DESC Descriptor;
    ULONG Options, FrameLength, Index;

    Index = KdNet.Rtl8168.RxConsumer;
    Descriptor = &KdNet.Rtl8168.RxRing[Index];
    Options = *(volatile ULONG *)&Descriptor->opts1;
    if (Options & DESC_OWN)
        return FALSE;

    KeMemoryBarrier();
    FrameLength = Options & RXD_LEN_MASK;
    if ((Options & (RXD_RES | DESC_FS | DESC_LS)) != (DESC_FS | DESC_LS) ||
        FrameLength < KDNET_ETH_HEADER_SIZE + 4 || FrameLength > RX_BUF_SIZE)
    {
        NICRefillRxDescriptor(&KdNet.Rtl8168, Index);
        KdNet.Rtl8168.RxConsumer = (Index + 1) % RX_DESC_COUNT;
        return FALSE;
    }

    *Frame = KdNet.Rtl8168.RxBuffers + Index * RX_BUF_SIZE;
    *Length = FrameLength - 4;
    return TRUE;
}

static VOID
KdNetRtl8168ReleaseReceive(VOID)
{
    ULONG Index = KdNet.Rtl8168.RxConsumer;

    NICRefillRxDescriptor(&KdNet.Rtl8168, Index);
    KdNet.Rtl8168.RxConsumer = (Index + 1) % RX_DESC_COUNT;
}

static ULONG
KdNetE1000Read(_In_ ULONG Register)
{
    return READ_REGISTER_ULONG((PULONG)(KdNet.E1000.IoBase + Register));
}

static VOID
KdNetE1000Write(_In_ ULONG Register, _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(KdNet.E1000.IoBase + Register), Value);

    /*
     * PCI MMIO writes are posted.  Flush them before the transport uses the
     * programmed ring or consumes a reset result; otherwise a fast guest can
     * queue its first packet before QEMU (or real hardware) has observed TDT.
     */
    if (Register != KDNET_E1000_REG_STATUS)
        (void)KdNetE1000Read(KDNET_E1000_REG_STATUS);
}

static VOID
KdNetE1000WriteIo(_In_ ULONG Register, _In_ ULONG Value)
{
    WRITE_PORT_ULONG((PULONG)KdNet.E1000.IoPort, Register);
    WRITE_PORT_ULONG((PULONG)(KdNet.E1000.IoPort + sizeof(ULONG)), Value);
    (void)KdNetE1000Read(KDNET_E1000_REG_STATUS);
}

static BOOLEAN
KdNetE1000Send(_In_reads_bytes_(Length) const UCHAR *Frame, _In_ ULONG Length)
{
    volatile KDNET_E1000_TX_DESCRIPTOR *Descriptor;
    PUCHAR Buffer;
    ULONG Index, Poll;

    if (Length > MAXIMUM_FRAME_SIZE)
        return FALSE;

    Index = KdNet.E1000.TxProducer;
    Descriptor = &KdNet.E1000.TxRing[Index];
    for (Poll = 0; Poll < KDNET_SHORT_WAIT_POLLS; ++Poll)
    {
        if ((Descriptor->Status & KDNET_E1000_TX_DD) != 0)
            break;
        KeStallExecutionProcessor(KDNET_POLL_DELAY_US);
    }
    if (Poll == KDNET_SHORT_WAIT_POLLS)
        return FALSE;

    Buffer = KdNet.E1000.TxBuffers + Index * KDNET_E1000_BUFFER_SIZE;
    RtlCopyMemory(Buffer, Frame, Length);
    Descriptor->Address = KdNet.E1000.TxBuffersPa.QuadPart +
                          (ULONGLONG)Index * KDNET_E1000_BUFFER_SIZE;
    Descriptor->Length = (USHORT)Length;
    Descriptor->ChecksumOffset = 0;
    Descriptor->Command = KDNET_E1000_TX_RS | KDNET_E1000_TX_IFCS |
                          KDNET_E1000_TX_EOP;
    Descriptor->Status = 0;
    Descriptor->ChecksumStart = 0;
    Descriptor->Special = 0;
    KeMemoryBarrier();

    KdNet.E1000.TxProducer = (Index + 1) % KDNET_E1000_DESC_COUNT;
    KdNetE1000Write(KDNET_E1000_REG_TDT, KdNet.E1000.TxProducer);
    return TRUE;
}

static BOOLEAN
KdNetE1000Receive(_Outptr_result_bytebuffer_(*Length) const UCHAR **Frame,
                  _Out_ PULONG Length)
{
    volatile KDNET_E1000_RX_DESCRIPTOR *Descriptor;
    ULONG Index;

    Index = KdNet.E1000.RxConsumer;
    Descriptor = &KdNet.E1000.RxRing[Index];
    if ((Descriptor->Status & KDNET_E1000_RX_DD) == 0)
    {
        /*
         * Keep the polled device making progress.  On KVM, repeatedly
         * examining coherent guest RAM does not necessarily give QEMU's
         * e1000 model an opportunity to deliver a packet from the tap.  A
         * harmless MMIO status read provides that progress point.  It also
         * orders a real adapter's DMA writes before the next descriptor
         * status check.
         */
        (void)KdNetE1000Read(KDNET_E1000_REG_STATUS);
        return FALSE;
    }

    KeMemoryBarrier();
    if ((Descriptor->Status & (KDNET_E1000_RX_DD | KDNET_E1000_RX_EOP)) !=
            (KDNET_E1000_RX_DD | KDNET_E1000_RX_EOP) ||
        Descriptor->Errors != 0 || Descriptor->Length < KDNET_ETH_HEADER_SIZE ||
        Descriptor->Length > MAXIMUM_FRAME_SIZE)
    {
        Descriptor->Status = 0;
        KeMemoryBarrier();
        KdNetE1000Write(KDNET_E1000_REG_RDT, Index);
        KdNet.E1000.RxConsumer = (Index + 1) % KDNET_E1000_DESC_COUNT;
        return FALSE;
    }

    *Frame = KdNet.E1000.RxBuffers + Index * KDNET_E1000_BUFFER_SIZE;
    *Length = Descriptor->Length;
    return TRUE;
}

static VOID
KdNetE1000ReleaseReceive(VOID)
{
    ULONG Index = KdNet.E1000.RxConsumer;
    volatile KDNET_E1000_RX_DESCRIPTOR *Descriptor;

    Descriptor = &KdNet.E1000.RxRing[Index];
    Descriptor->Status = 0;
    KeMemoryBarrier();
    KdNetE1000Write(KDNET_E1000_REG_RDT, Index);
    KdNet.E1000.RxConsumer = (Index + 1) % KDNET_E1000_DESC_COUNT;
}

static BOOLEAN
KdNetHardwareSend(_In_reads_bytes_(Length) const UCHAR *Frame, _In_ ULONG Length)
{
    BOOLEAN Sent;

    if (KdNet.Backend == KdNetBackendE1000)
        Sent = KdNetE1000Send(Frame, Length);
    else
        Sent = KdNetRtl8168Send(Frame, Length);
    if (Sent)
        ++KdNet.FramesSent;
    else
        ++KdNet.FramesDropped;
    return Sent;
}

/*
 * Is the producer transmit descriptor already reclaimed by the adapter?
 *
 * Both backends open their send with a KDNET_SHORT_WAIT_POLLS spin on exactly
 * this bit - a quarter of a second at KDNET_POLL_DELAY_US.  That is the right
 * trade for the debugger, which has the machine to itself and would rather wait
 * than lose a packet.  It is the wrong trade entirely for the OS data path,
 * which runs at HIGH_LEVEL under the share lock, where a 250 ms spin would
 * stall every processor and hold off a debugger break-in for the same time.
 * So the shared path tests first and gives up early, and by the time it calls
 * the send the spin inside it is guaranteed to exit on its first iteration.
 */
static BOOLEAN
KdNetHardwareTxSlotFree(VOID)
{
    if (KdNet.Backend == KdNetBackendE1000)
    {
        volatile KDNET_E1000_TX_DESCRIPTOR *Descriptor =
            &KdNet.E1000.TxRing[KdNet.E1000.TxProducer];

        return (BOOLEAN)((Descriptor->Status & KDNET_E1000_TX_DD) != 0);
    }
    else
    {
        PRTL_DESC Descriptor = &KdNet.Rtl8168.TxRing[KdNet.Rtl8168.TxProducer];

        return (BOOLEAN)((*(volatile ULONG *)&Descriptor->opts1 & DESC_OWN) == 0);
    }
}

static BOOLEAN
KdNetHardwareReceive(_Outptr_result_bytebuffer_(*Length) const UCHAR **Frame,
                     _Out_ PULONG Length)
{
    BOOLEAN Received;

    if (KdNet.Backend == KdNetBackendE1000)
        Received = KdNetE1000Receive(Frame, Length);
    else
        Received = KdNetRtl8168Receive(Frame, Length);
    if (Received)
        ++KdNet.FramesReceived;
    return Received;
}

static VOID
KdNetHardwareReleaseReceive(VOID)
{
    if (KdNet.Backend == KdNetBackendE1000)
        KdNetE1000ReleaseReceive();
    else
        KdNetRtl8168ReleaseReceive();
}

static VOID
KdNetBuildEthernetHeader(_Out_writes_(KDNET_ETH_HEADER_SIZE) UCHAR *Frame,
                         _In_reads_(IEEE_802_ADDR_LENGTH) const UCHAR *Destination,
                         _In_ USHORT EtherType)
{
    RtlCopyMemory(Frame, Destination, IEEE_802_ADDR_LENGTH);
    RtlCopyMemory(Frame + IEEE_802_ADDR_LENGTH,
                  KdNet.TargetMac,
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
    RtlCopyMemory(Arp + 8, KdNet.TargetMac, IEEE_802_ADDR_LENGTH);
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
KdNetSendUdpToPort(_In_reads_bytes_(PayloadLength) const UCHAR *Payload,
                   _In_ ULONG PayloadLength,
                   _In_ USHORT Port)
{
    UCHAR *Ip, *Udp;
    ULONG DatagramLength, Offset, FragmentLength, FrameLength;
    USHORT FragmentField, Identifier;

    if (!KdNet.HostMacValid || PayloadLength > KDNET_PACKET_CAPACITY)
        return FALSE;

    Udp = KdNet.UdpDatagram;
    KdNetWriteBe16(Udp, Port);
    KdNetWriteBe16(Udp + 2, Port);
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

static BOOLEAN
KdNetSendUdp(_In_reads_bytes_(PayloadLength) const UCHAR *Payload,
             _In_ ULONG PayloadLength)
{
    return KdNetSendUdpToPort(Payload, PayloadLength, KdNet.Port);
}

static VOID
KdNetFlushDiagnostics(VOID)
{
    ULONG Length;

    if (!KdNet.HostMacValid || KdNet.Backend == KdNetBackendNone)
        return;

    while (KdNet.TraceFlushed < KdNet.TraceLength)
    {
        Length = KdNet.TraceLength - KdNet.TraceFlushed;
        if (Length > KDNET_DIAGNOSTIC_CHUNK)
            Length = KDNET_DIAGNOSTIC_CHUNK;
        if (!KdNetSendUdpToPort((const UCHAR *)KdNet.TraceBuffer +
                                    KdNet.TraceFlushed,
                                Length,
                                KDNET_DIAGNOSTIC_PORT))
        {
            return;
        }
        KdNet.TraceFlushed += Length;
    }

    /* Reuse the buffer after a complete drain instead of going silent at 8 KiB. */
    KdNet.TraceLength = 0;
    KdNet.TraceFlushed = 0;
}

static VOID
KdNetHarvestPrintf(_In_ PCSTR Format, ...)
{
    CHAR Body[KDNET_HARVEST_LINE_SIZE - 64];
    CHAR Line[KDNET_HARVEST_LINE_SIZE];
    va_list Arguments;

    va_start(Arguments, Format);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(Body,
                                        sizeof(Body),
                                        Format,
                                        Arguments)))
    {
        va_end(Arguments);
        return;
    }
    va_end(Arguments);

    if (!NT_SUCCESS(RtlStringCbPrintfA(Line,
                                       sizeof(Line),
                                       "[KDNET-HARVEST/%lu] %s\r\n",
                                       KdNet.HarvestActiveId,
                                       Body)))
    {
        return;
    }

    if (KdNetSendUdpToPort((const UCHAR *)Line,
                           (ULONG)strlen(Line),
                           KDNET_DIAGNOSTIC_PORT))
    {
        ++KdNet.HarvestResponsesSent;
    }
    else
    {
        KdNetEmitTrace(Line);
    }
}

static VOID
KdNetHarvestUnicodeString(_In_opt_ PCUNICODE_STRING Source,
                          _Out_writes_(Capacity) PCHAR Destination,
                          _In_ ULONG Capacity)
{
    ULONG Characters, Index;
    WCHAR Character;

    if (Capacity == 0)
        return;
    Destination[0] = ANSI_NULL;
    if (Source == NULL || Source->Buffer == NULL)
        return;

    Characters = Source->Length / sizeof(WCHAR);
    if (Characters >= Capacity)
        Characters = Capacity - 1;
    for (Index = 0; Index < Characters; ++Index)
    {
        Character = Source->Buffer[Index];
        Destination[Index] = Character >= 0x20 && Character <= 0x7e ?
                                 (CHAR)Character : '?';
    }
    Destination[Characters] = ANSI_NULL;
}

static PCSTR
KdNetHarvestBackendName(VOID)
{
    if (KdNet.Backend == KdNetBackendRtl8168)
        return "rtl8168";
    if (KdNet.Backend == KdNetBackendE1000)
        return "e1000";
    return "none";
}

static VOID
KdNetHarvestStatus(VOID)
{
    KdNetHarvestPrintf("STATUS backend=%s initialized=%u breakin=%u flags=0x%08lx",
                       KdNetHarvestBackendName(),
                       KdNet.Initialized,
                       KdNet.BreakInPending,
                       KdNet.HarvestFlags);
    KdNetHarvestPrintf("STATUS pci=%lu:%lu ven=%04x dev=%04x class=%02x/%02x/%02x",
                       KdNet.Device.Bus,
                       KdNet.Device.Slot,
                       KdNet.Device.VendorID,
                       KdNet.Device.DeviceID,
                       KdNet.Device.BaseClass,
                       KdNet.Device.SubClass,
                       KdNet.Device.ProgIf);
    KdNetHarvestPrintf("STATUS frames tx=%I64u rx=%I64u drop=%I64u commands=%lu responses=%lu",
                       KdNet.FramesSent,
                       KdNet.FramesReceived,
                       KdNet.FramesDropped,
                       KdNet.HarvestCommandsReceived,
                       KdNet.HarvestResponsesSent);
}

static VOID
KdNetHarvestNic(VOID)
{
    ULONG Index;

    KdNetHarvestPrintf("NIC backend=%s mac=%02x:%02x:%02x:%02x:%02x:%02x dma_va=%p dma_pa=%08lx:%08lx dma_len=0x%lx",
                       KdNetHarvestBackendName(),
                       KdNet.TargetMac[0], KdNet.TargetMac[1],
                       KdNet.TargetMac[2], KdNet.TargetMac[3],
                       KdNet.TargetMac[4], KdNet.TargetMac[5],
                       KdNet.Device.Memory.VirtualAddress,
                       KdNet.Device.Memory.Start.HighPart,
                       KdNet.Device.Memory.Start.LowPart,
                       KdNet.Device.Memory.Length);
    for (Index = 0; Index < MAXIMUM_DEBUG_BARS; ++Index)
    {
        if (KdNet.Device.BaseAddress[Index].Valid)
        {
            KdNetHarvestPrintf("NIC bar=%lu type=%u address=%p length=0x%lx",
                               Index,
                               KdNet.Device.BaseAddress[Index].Type,
                               KdNet.Device.BaseAddress[Index].TranslatedAddress,
                               KdNet.Device.BaseAddress[Index].Length);
        }
    }

    if (KdNet.Backend == KdNetBackendRtl8168)
    {
        KdNetHarvestPrintf("NIC rtl cmd=%02x im=%04x is=%04x txcfg=%08lx rxcfg=%08lx cplus=%04x phy=%02x macver=%u",
                           RtlReadReg8(&KdNet.Rtl8168, R_CMD),
                           RtlReadReg16(&KdNet.Rtl8168, R_IM),
                           RtlReadReg16(&KdNet.Rtl8168, R_IS),
                           RtlReadReg32(&KdNet.Rtl8168, R_TC),
                           RtlReadReg32(&KdNet.Rtl8168, R_RC),
                           RtlReadReg16(&KdNet.Rtl8168, R_CPLUSCMD),
                           RtlReadReg8(&KdNet.Rtl8168, R_PHYSTS),
                           KdNet.Rtl8168.MacVersion);
        KdNetHarvestPrintf("NIC rtl tx_producer=%lu rx_consumer=%lu tx_ring=%p/%I64x rx_ring=%p/%I64x",
                           KdNet.Rtl8168.TxProducer,
                           KdNet.Rtl8168.RxConsumer,
                           KdNet.Rtl8168.TxRing,
                           KdNet.Rtl8168.TxRingPa.QuadPart,
                           KdNet.Rtl8168.RxRing,
                           KdNet.Rtl8168.RxRingPa.QuadPart);
    }
    else if (KdNet.Backend == KdNetBackendE1000)
    {
        KdNetHarvestPrintf("NIC e1000 ctrl=%08lx rctl=%08lx tctl=%08lx icr=%08lx rdh=%lu rdt=%lu tdh=%lu tdt=%lu",
                           KdNetE1000Read(KDNET_E1000_REG_CTRL),
                           KdNetE1000Read(KDNET_E1000_REG_RCTL),
                           KdNetE1000Read(KDNET_E1000_REG_TCTL),
                           KdNetE1000Read(KDNET_E1000_REG_ICR),
                           KdNetE1000Read(KDNET_E1000_REG_RDH),
                           KdNetE1000Read(KDNET_E1000_REG_RDT),
                           KdNetE1000Read(KDNET_E1000_REG_TDH),
                           KdNetE1000Read(KDNET_E1000_REG_TDT));
        KdNetHarvestPrintf("NIC e1000 tx_producer=%lu rx_consumer=%lu tx_ring=%p/%I64x rx_ring=%p/%I64x",
                           KdNet.E1000.TxProducer,
                           KdNet.E1000.RxConsumer,
                           KdNet.E1000.TxRing,
                           KdNet.E1000.TxRingPa.QuadPart,
                           KdNet.E1000.RxRing,
                           KdNet.E1000.RxRingPa.QuadPart);
    }
}

static VOID
KdNetHarvestLoader(VOID)
{
    PLOADER_PARAMETER_BLOCK LoaderBlock = KdNet.LoaderBlock;
    PLIST_ENTRY Entry;
    ULONG Count;
    CHAR Name[256];

    if (LoaderBlock == NULL)
    {
        KdNetHarvestPrintf("LOADER unavailable");
        return;
    }

    KdNetHarvestPrintf("LOADER block=%p registry=%p/0x%lx options=%s",
                       LoaderBlock,
                       LoaderBlock->RegistryBase,
                       LoaderBlock->RegistryLength,
                       LoaderBlock->LoadOptions != NULL ?
                           LoaderBlock->LoadOptions : "<null>");

    Count = 0;
    for (Entry = LoaderBlock->MemoryDescriptorListHead.Flink;
         Entry != &LoaderBlock->MemoryDescriptorListHead && Count < 256;
         Entry = Entry->Flink, ++Count)
    {
        PMEMORY_ALLOCATION_DESCRIPTOR Descriptor;

        Descriptor = CONTAINING_RECORD(Entry,
                                       MEMORY_ALLOCATION_DESCRIPTOR,
                                       ListEntry);
        KdNetHarvestPrintf("MEM index=%lu type=%u base_pfn=%I64x pages=%I64x end_pfn=%I64x",
                           Count,
                           Descriptor->MemoryType,
                           (ULONGLONG)Descriptor->BasePage,
                           (ULONGLONG)Descriptor->PageCount,
                           (ULONGLONG)(Descriptor->BasePage +
                                      Descriptor->PageCount));
    }
    KdNetHarvestPrintf("MEM count=%lu truncated=%u",
                       Count,
                       Entry != &LoaderBlock->MemoryDescriptorListHead);

    Count = 0;
    for (Entry = LoaderBlock->LoadOrderListHead.Flink;
         Entry != &LoaderBlock->LoadOrderListHead && Count < 256;
         Entry = Entry->Flink, ++Count)
    {
        PLDR_DATA_TABLE_ENTRY Module;

        Module = CONTAINING_RECORD(Entry,
                                   LDR_DATA_TABLE_ENTRY,
                                   InLoadOrderLinks);
        KdNetHarvestUnicodeString(&Module->BaseDllName, Name, sizeof(Name));
        KdNetHarvestPrintf("MODULE index=%lu base=%p size=0x%lx entry=%p timestamp=%08lx name=%s",
                           Count,
                           Module->DllBase,
                           Module->SizeOfImage,
                           Module->EntryPoint,
                           Module->TimeDateStamp,
                           Name);
    }
    KdNetHarvestPrintf("MODULE count=%lu truncated=%u",
                       Count,
                       Entry != &LoaderBlock->LoadOrderListHead);

    Count = 0;
    for (Entry = LoaderBlock->BootDriverListHead.Flink;
         Entry != &LoaderBlock->BootDriverListHead && Count < 256;
         Entry = Entry->Flink, ++Count)
    {
        PBOOT_DRIVER_LIST_ENTRY Driver;

        Driver = CONTAINING_RECORD(Entry, BOOT_DRIVER_LIST_ENTRY, Link);
        KdNetHarvestUnicodeString(&Driver->FilePath, Name, sizeof(Name));
        KdNetHarvestPrintf("BOOTDRIVER index=%lu ldr=%p path=%s",
                           Count,
                           Driver->LdrEntry,
                           Name);
    }
    KdNetHarvestPrintf("BOOTDRIVER count=%lu truncated=%u",
                       Count,
                       Entry != &LoaderBlock->BootDriverListHead);
}

static ULONG
KdNetHarvestReadPciConfig(_In_ ULONG Bus,
                          _In_ ULONG Device,
                          _In_ ULONG Function,
                          _Out_writes_bytes_(Length) PVOID Buffer,
                          _In_ ULONG Length)
{
#if defined(_M_IX86) || defined(_M_AMD64)
    PUCHAR Bytes = Buffer;
    ULONG Address, Offset, Value, CopyLength;

    if (Bus > 0xff || Device >= PCI_MAX_DEVICES ||
        Function >= PCI_MAX_FUNCTION || Length > 0x100)
    {
        return 0;
    }

    /*
     * The command runs while KD has frozen the other processors, so legacy
     * PCI mechanism 1 cannot race a HAL config-space transaction.  This path
     * remains usable at the initial debugger break, before the HAL has made
     * HalGetBusDataByOffset's PCI bus handlers available.
     */
    for (Offset = 0; Offset < Length; Offset += sizeof(ULONG))
    {
        Address = 0x80000000 | (Bus << 16) | (Device << 11) |
                  (Function << 8) | (Offset & 0xfc);
        WRITE_PORT_ULONG((PULONG)(ULONG_PTR)0xcf8, Address);
        Value = READ_PORT_ULONG((PULONG)(ULONG_PTR)0xcfc);
        CopyLength = min(sizeof(Value), Length - Offset);
        RtlCopyMemory(Bytes + Offset, &Value, CopyLength);
    }
    WRITE_PORT_ULONG((PULONG)(ULONG_PTR)0xcf8, 0);
    return Length;
#else
    PCI_SLOT_NUMBER Slot;

    Slot.u.AsULONG = 0;
    Slot.u.bits.DeviceNumber = Device;
    Slot.u.bits.FunctionNumber = Function;
    return HalGetBusDataByOffset(PCIConfiguration,
                                 Bus,
                                 Slot.u.AsULONG,
                                 Buffer,
                                 0,
                                 Length);
#endif
}

static VOID
KdNetHarvestPci(_In_ BOOLEAN UsbOnly)
{
    UCHAR PendingBuses[256 / 8];
    PCI_COMMON_CONFIG Config;
    ULONG Bus, Device, Function, Length, Count = 0, Matched = 0;
    ULONG HeaderType;
    BOOLEAN Multifunction;
    PCSTR Record = UsbOnly ? "USB" : "PCI";

    RtlZeroMemory(PendingBuses, sizeof(PendingBuses));
    PendingBuses[0] = 1;

    for (Bus = 0; Bus <= PCI_MAX_BRIDGE_NUMBER; ++Bus)
    {
        if ((PendingBuses[Bus / 8] & (1 << (Bus % 8))) == 0)
            continue;

        for (Device = 0; Device < PCI_MAX_DEVICES; ++Device)
        {
            Multifunction = FALSE;
            for (Function = 0; Function < PCI_MAX_FUNCTION; ++Function)
            {
                if (Function != 0 && !Multifunction)
                    break;

                RtlZeroMemory(&Config, sizeof(Config));
                Length = KdNetHarvestReadPciConfig(Bus,
                                                   Device,
                                                   Function,
                                                   &Config,
                                                   PCI_COMMON_HDR_LENGTH);
                if (Length < sizeof(Config.VendorID) ||
                    Config.VendorID == PCI_INVALID_VENDORID)
                {
                    if (Function == 0)
                        break;
                    continue;
                }

                Multifunction = PCI_MULTIFUNCTION_DEVICE(&Config);
                HeaderType = PCI_CONFIGURATION_TYPE(&Config);
                ++Count;

                if (!UsbOnly ||
                    (Config.BaseClass == 0x0c && Config.SubClass == 0x03))
                {
                    ++Matched;
                    KdNetHarvestPrintf("%s bdf=%02lx:%02lx.%lu ven=%04x dev=%04x class=%02x/%02x/%02x rev=%02x cmd=%04x sts=%04x hdr=%02x",
                                       Record, Bus, Device, Function,
                                       Config.VendorID, Config.DeviceID,
                                       Config.BaseClass, Config.SubClass,
                                       Config.ProgIf, Config.RevisionID,
                                       Config.Command, Config.Status,
                                       Config.HeaderType);
                    if (HeaderType == PCI_DEVICE_TYPE)
                    {
                        KdNetHarvestPrintf("%s bdf=%02lx:%02lx.%lu bars=%08lx,%08lx,%08lx,%08lx,%08lx,%08lx irq=%u/%u subsys=%04x:%04x",
                                           Record, Bus, Device, Function,
                                           Config.u.type0.BaseAddresses[0],
                                           Config.u.type0.BaseAddresses[1],
                                           Config.u.type0.BaseAddresses[2],
                                           Config.u.type0.BaseAddresses[3],
                                           Config.u.type0.BaseAddresses[4],
                                           Config.u.type0.BaseAddresses[5],
                                           Config.u.type0.InterruptLine,
                                           Config.u.type0.InterruptPin,
                                           Config.u.type0.SubVendorID,
                                           Config.u.type0.SubSystemID);
                    }
                }

                if (HeaderType == PCI_BRIDGE_TYPE &&
                    Config.u.type1.SecondaryBus != 0 &&
                    Config.u.type1.SecondaryBus <=
                        Config.u.type1.SubordinateBus)
                {
                    PendingBuses[Config.u.type1.SecondaryBus / 8] |=
                        (UCHAR)(1 << (Config.u.type1.SecondaryBus % 8));
                }
            }
        }
    }
    KdNetHarvestPrintf("%s matched=%lu discovered=%lu", Record, Matched, Count);
}

static BOOLEAN
KdNetHarvestCommandEquals(_In_ PCSTR Command, _In_ PCSTR Expected)
{
    ULONG CommandLength = (ULONG)strlen(Command);
    return KdNetOptionTokenEquals(Command, CommandLength, Expected);
}

static VOID
KdNetHandleHarvestCommand(_In_reads_bytes_(Length) const UCHAR *Payload,
                          _In_ ULONG Length)
{
    CHAR Command[KDNET_HARVEST_COMMAND_SIZE];
    ULONG PrefixLength = sizeof(KDNET_HARVEST_PREFIX) - 1;

    if ((KdNet.HarvestFlags & KDNET_HARVEST_COMMANDS) == 0 ||
        Length <= PrefixLength ||
        Length - PrefixLength >= sizeof(Command) ||
        RtlCompareMemory(Payload,
                         KDNET_HARVEST_PREFIX,
                         PrefixLength) != PrefixLength)
    {
        return;
    }

    Length -= PrefixLength;
    RtlCopyMemory(Command, Payload + PrefixLength, Length);
    while (Length != 0 &&
           (Command[Length - 1] == '\r' || Command[Length - 1] == '\n' ||
            Command[Length - 1] == ' ' || Command[Length - 1] == '\t'))
    {
        --Length;
    }
    Command[Length] = ANSI_NULL;
    if (Length == 0)
        return;

    ++KdNet.HarvestCommandsReceived;
    KdNet.HarvestActiveId = ++KdNet.HarvestRequestId;
    KdNetHarvestPrintf("BEGIN command=%s", Command);

    if (KdNetHarvestCommandEquals(Command, "PING"))
        KdNetHarvestPrintf("PONG");
    else if (KdNetHarvestCommandEquals(Command, "HELP"))
        KdNetHarvestPrintf("HELP commands=PING,STATUS,NIC,PCI,USB,LOADER,ALL");
    else if (KdNetHarvestCommandEquals(Command, "STATUS"))
        KdNetHarvestStatus();
    else if (KdNetHarvestCommandEquals(Command, "NIC"))
        KdNetHarvestNic();
    else if (KdNetHarvestCommandEquals(Command, "PCI"))
        KdNetHarvestPci(FALSE);
    else if (KdNetHarvestCommandEquals(Command, "USB"))
        KdNetHarvestPci(TRUE);
    else if (KdNetHarvestCommandEquals(Command, "LOADER"))
        KdNetHarvestLoader();
    else if (KdNetHarvestCommandEquals(Command, "ALL"))
    {
        KdNetHarvestStatus();
        KdNetHarvestNic();
        KdNetHarvestPci(FALSE);
        KdNetHarvestPci(TRUE);
        KdNetHarvestLoader();
    }
    else
        KdNetHarvestPrintf("ERROR unknown command=%s", Command);

    KdNetHarvestPrintf("END command=%s", Command);
    KdNet.HarvestActiveId = 0;
}

static VOID
KdNetRunAutomaticHarvest(_In_ ULONG Flags, _In_ PCSTR Phase)
{
    Flags &= KdNet.HarvestFlags;
    if (Flags == 0)
        return;

    KdNet.HarvestActiveId = ++KdNet.HarvestRequestId;
    KdNetHarvestPrintf("BEGIN automatic=%s flags=0x%08lx", Phase, Flags);
    if (Flags & KDNET_HARVEST_STATUS)
        KdNetHarvestStatus();
    if (Flags & KDNET_HARVEST_NIC)
        KdNetHarvestNic();
    if (Flags & KDNET_HARVEST_PCI)
        KdNetHarvestPci(FALSE);
    if (Flags & KDNET_HARVEST_USB)
        KdNetHarvestPci(TRUE);
    if (Flags & KDNET_HARVEST_LOADER)
        KdNetHarvestLoader();
    KdNetHarvestPrintf("END automatic=%s", Phase);
    KdNet.HarvestActiveId = 0;
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
    USHORT SourcePort, DestinationPort;

    if (DatagramLength < KDNET_UDP_HEADER_SIZE)
        return FALSE;
    SourcePort = KdNetReadBe16(Datagram);
    DestinationPort = KdNetReadBe16(Datagram + 2);
    UdpLength = KdNetReadBe16(Datagram + 4);
    if (UdpLength < KDNET_UDP_HEADER_SIZE || UdpLength > DatagramLength)
    {
        return FALSE;
    }

    if (SourcePort == KDNET_DIAGNOSTIC_PORT &&
        DestinationPort == KDNET_DIAGNOSTIC_PORT)
    {
        KdNetHandleHarvestCommand(Datagram + KDNET_UDP_HEADER_SIZE,
                                  UdpLength - KDNET_UDP_HEADER_SIZE);
        return FALSE;
    }

    if (SourcePort != KdNet.Port || DestinationPort != KdNet.Port ||
        UdpLength - KDNET_UDP_HEADER_SIZE > Capacity || Payload == NULL)
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

/*
 * Adapter sharing, part 1.
 *
 * The one rule that matters here: the OS-side poll never takes a frame that
 * belongs to the debugger.  An earlier version buffered such frames and replayed
 * them into KdNetPollDatagram, which looked like the careful thing to do and was
 * not - the debugger's protocol is a synchronous request/response over the wire,
 * so a side buffer feeds it stale packets ahead of live ones and desynchronises
 * the session.  Observed: the miniport registered and the debug link went dead
 * three hundred milliseconds later, while the machine itself carried on booting.
 *
 * Instead the OS poll peeks at each frame and, on finding one the debugger might
 * want, stops draining and leaves it in the ring with its descriptor unreleased.
 * The debugger's own poll then reads it straight from the hardware, in order,
 * exactly as if no one else had been looking.  The cost is head-of-line blocking
 * of OS traffic behind a debugger frame until the debugger next polls, which it
 * does constantly.  That is the correct way round: this whole feature is only
 * worth having if the debug channel stays trustworthy.
 */

/* Ceiling on how long a shared transmit may hold the machine at HIGH_LEVEL
 * waiting for a descriptor: 8 * KDNET_POLL_DELAY_US = 400 us. */
#define KDNET_SHARE_TX_POLLS 8

typedef struct _KDNET_SHARE_STATE
{
    PKDNET_SHARE_RECEIVE_CALLBACK Receive;
    PVOID Context;

    ULONGLONG OsFramesSent;
    ULONGLONG OsFramesReceived;
    ULONGLONG OsFramesDropped;
    ULONGLONG DebuggerFramesYielded;

    /* Frames handed over from the debugger's own poll rather than the OS poll.
     * Interlocked, because that path holds no lock (see below). */
    LONG OsFramesFromDebuggerPoll;
} KDNET_SHARE_STATE, *PKDNET_SHARE_STATE;

/*
 * The share lock.  It is deliberately hand-rolled rather than a KSPIN_LOCK,
 * because of an asymmetry that a normal lock cannot express:
 *
 *   - The OS side takes it at HIGH_LEVEL.  Raising to HIGH_LEVEL masks the
 *     freeze IPI, so KeFreezeExecution cannot complete while a processor is
 *     inside the critical section; a debugger break-in therefore always finds
 *     the descriptor rings in a consistent state, at the cost of waiting the
 *     few microseconds the section lasts.
 *
 *   - The debugger side does NOT take it, and must not: it runs with every
 *     other processor frozen, and one of those frozen processors could be the
 *     holder.  Exclusion comes from the freeze itself, which the rule above
 *     guarantees cannot land mid-section.
 *
 * Nothing inside the critical section may print.  DbgPrint reaches KdSendPacket
 * and would re-enter the transport whose ring is being manipulated.
 */
static volatile LONG KdNetShareBusy;

static VOID
KdNetShareEnter(_Out_ PKIRQL OldIrql)
{
    KeRaiseIrql(HIGH_LEVEL, OldIrql);
    while (InterlockedCompareExchange(&KdNetShareBusy, 1, 0) != 0)
        YieldProcessor();
}

static VOID
KdNetShareLeave(_In_ KIRQL OldIrql)
{
    InterlockedExchange(&KdNetShareBusy, 0);
    KeLowerIrql(OldIrql);
}

static BOOLEAN
KdNetShareIsDebuggerFrame(_In_reads_bytes_(Length) const UCHAR *Frame,
                          _In_ ULONG Length);

/*
 * Hand the OS a frame the debugger does not want, from the debugger's own poll.
 *
 * This poll used to drop such frames, on the stated reasoning that it "only
 * ever runs with the machine frozen, where there is no one to deliver them to".
 * That reasoning is wrong, and the sharing feature does not work without this
 * function.  The break-in poll - KdNetReceiveExpected with
 * PACKET_TYPE_KD_POLL_BREAKIN - runs on a live, unfrozen machine and runs
 * constantly, so both polls compete for the same ring and the debugger's wins
 * nearly every time.  Measured before this fix: ten broadcast ARP requests for
 * the target's address produced ten releases here and not one indication to the
 * OS, so the guest answered nothing and inbound traffic was dead.
 *
 * No lock is taken and none may be.  This can run with every other processor
 * frozen, and one of them could be holding the share lock; that is the same
 * asymmetry the lock comment above describes.  The callback is safe under that
 * rule because it claims a receive slot with a single interlocked compare-
 * exchange and copies into non-paged memory - no allocation, no blocking, no
 * re-entry into the transport, and no printing.
 */
static volatile LONG KdNetShareCallbackDepth;

static VOID
KdNetShareDeliverFromDebuggerPoll(_In_reads_bytes_(Length) const UCHAR *Frame,
                                  _In_ ULONG Length)
{
    PKDNET_SHARE_STATE Share = KdNet.Share;
    PKDNET_SHARE_RECEIVE_CALLBACK Receive;
    PVOID Context;

    if (Share == NULL)
        return;
    if (Length < KDNET_ETH_HEADER_SIZE)
        return;

    /* Raised before the callback pointer is read, so that a deregistration
     * which clears it can tell whether this path is still inside the callback.
     * See KdNetShareDeregister for the other half. */
    InterlockedIncrement(&KdNetShareCallbackDepth);

    Receive = Share->Receive;
    if (Receive != NULL)
    {
        /* A deregistration racing us can clear Context between these two
         * reads, so the callback may be handed a NULL context.  That is
         * defined: the miniport's callback returns immediately on one. */
        Context = Share->Context;
        Receive(Context, Frame, Length);
        InterlockedIncrement(&Share->OsFramesFromDebuggerPoll);
    }

    InterlockedDecrement(&KdNetShareCallbackDepth);
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

    /* No special case for adapter sharing: the OS-side poll leaves anything
     * that might be ours in the ring, so the hardware is still the one and only
     * source of debugger frames, and they arrive here in wire order. */
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

            /* Not the debugger's, and about to be released: this is the OS's
             * last chance to see it.  Classified rather than inferred from
             * !Received, so that a debugger frame this poll could not use -
             * a malformed datagram, or any frame at all when Payload is NULL -
             * is still never duplicated into the OS. */
            if (!Received && !KdNetShareIsDebuggerFrame(Frame, Length))
                KdNetShareDeliverFromDebuggerPoll(Frame, Length);
        }
        KdNetHardwareReleaseReceive();
        if (Received)
            return TRUE;
    }
    return FALSE;
}

/*
 * Adapter sharing, part 2: frame ownership and the exported interface.
 *
 * Note the asymmetry in what the two sides do with a frame that is not theirs.
 * The OS-side poll leaves debugger frames untouched in the ring, because the
 * debugger is free to be idle and must not lose a break-in - and because only
 * the hardware can preserve their order.  The debugger's own poll simply drops
 * OS frames, because it only ever runs with the machine frozen, where there is
 * no one to deliver them to and the sender will retransmit anyway.
 *
 * "Might be the debugger's" is deliberately generous.  Guessing wrong towards
 * the debugger costs one deferred OS frame; guessing wrong the other way costs
 * a lost debugger packet, which is the failure this whole design exists to
 * avoid.
 */
static BOOLEAN
KdNetShareIsDebuggerFrame(_In_reads_bytes_(Length) const UCHAR *Frame,
                          _In_ ULONG Length)
{
    const UCHAR *Ip, *Arp;
    USHORT EtherType;

    if (Length < KDNET_ETH_HEADER_SIZE)
        return FALSE;
    EtherType = KdNetReadBe16(Frame + 12);

    if (EtherType == ETHERTYPE_ARP)
    {
        /* ARP about either end of the debug conversation is the debugger's:
         * it is how the host MAC gets resolved when /HOSTMAC is not given, and
         * how the host finds us.  Any other ARP is the OS's business. */
        if (Length < KDNET_ETH_HEADER_SIZE + 28)
            return FALSE;
        Arp = Frame + KDNET_ETH_HEADER_SIZE;
        if (KdNetReadBe16(Arp) != ARP_HARDWARE_ETHERNET ||
            KdNetReadBe16(Arp + 2) != ETHERTYPE_IPV4 ||
            Arp[4] != IEEE_802_ADDR_LENGTH || Arp[5] != 4)
        {
            return FALSE;
        }
        return (BOOLEAN)(KdNetReadBe32(Arp + 14) == KdNet.HostIp ||
                         KdNetReadBe32(Arp + 24) == KdNet.TargetIp);
    }

    if (EtherType != ETHERTYPE_IPV4 ||
        Length < KDNET_ETH_HEADER_SIZE + KDNET_IPV4_HEADER_SIZE)
    {
        return FALSE;
    }

    Ip = Frame + KDNET_ETH_HEADER_SIZE;
    if ((Ip[0] >> 4) != 4 || Ip[9] != IP_PROTOCOL_UDP)
        return FALSE;
    if ((ULONG)((Ip[0] & 0xf) * 4) < KDNET_IPV4_HEADER_SIZE)
        return FALSE;

    /* Exactly the address pair KdNetHandleIpv4 accepts, and deliberately not a
     * port test: a non-initial IP fragment carries no UDP header, and those
     * fragments still belong to the debugger's reassembly. */
    return (BOOLEAN)(KdNetReadBe32(Ip + 12) == KdNet.HostIp &&
                     KdNetReadBe32(Ip + 16) == KdNet.TargetIp);
}

NTSTATUS
NTAPI
KdNetShareRegister(_In_ PKDNET_SHARE_REGISTRATION Registration)
{
    PKDNET_SHARE_STATE Share;
    KIRQL OldIrql;

    if (Registration == NULL ||
        Registration->Version != KDNET_SHARE_INTERFACE_VERSION ||
        Registration->Receive == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KdNet.ShareEnabled)
        return STATUS_NOT_SUPPORTED;
    if (!KdNet.Initialized || KdNet.Backend == KdNetBackendNone)
        return STATUS_DEVICE_NOT_READY;

    if (KdNet.Share == NULL)
    {
        Share = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Share), 'ShdK');
        if (Share == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(Share, sizeof(*Share));

        if (InterlockedCompareExchangePointer((PVOID volatile *)&KdNet.Share,
                                              Share, NULL) != NULL)
        {
            /* Another registration published first; ours is surplus. */
            ExFreePoolWithTag(Share, 'ShdK');
        }
    }

    /* The state block itself is never freed, for the whole life of the boot.
     * KdNetPollDatagram reads it from a bugcheck path with no lock and no way
     * to be told the memory went away, and a few kilobytes retained is a far
     * better trade than a use-after-free inside the debugger. */
    KdNetShareEnter(&OldIrql);
    if (KdNet.Share->Receive != NULL)
    {
        KdNetShareLeave(OldIrql);
        return STATUS_DEVICE_BUSY;
    }
    KdNet.Share->Context = Registration->Context;
    KdNet.Share->Receive = Registration->Receive;
    KdNetShareLeave(OldIrql);

    return STATUS_SUCCESS;
}

VOID
NTAPI
KdNetShareDeregister(VOID)
{
    KIRQL OldIrql;

    if (KdNet.Share == NULL)
        return;

    /* The callback is only ever invoked with this lock held, so once we have
     * held it and cleared the pointer, no invocation is in progress and none
     * can begin.  That is the guarantee the caller frees its context on. */
    KdNetShareEnter(&OldIrql);
    KdNet.Share->Receive = NULL;
    KdNet.Share->Context = NULL;
    KdNetShareLeave(OldIrql);

    /* The lock alone is no longer proof of quiescence.  The debugger's poll
     * also invokes the callback and deliberately takes no lock, so wait for
     * that path to drain as well before the caller frees its context.
     *
     * This terminates: the callback is a slot claim and a copy, and the only
     * way to be inside it is to be running, which a processor frozen in the
     * debugger is not - and while any processor is frozen, this one is too. */
    while (InterlockedCompareExchange(&KdNetShareCallbackDepth, 0, 0) != 0)
        YieldProcessor();
}

NTSTATUS
NTAPI
KdNetShareQuery(_Out_ PKDNET_SHARE_INFO Info)
{
    if (Info == NULL)
        return STATUS_INVALID_PARAMETER;
    if (!KdNet.ShareEnabled)
        return STATUS_NOT_SUPPORTED;
    if (!KdNet.Initialized || KdNet.Backend == KdNetBackendNone)
        return STATUS_DEVICE_NOT_READY;

    RtlZeroMemory(Info, sizeof(*Info));
    Info->Version = KDNET_SHARE_INTERFACE_VERSION;
    RtlCopyMemory(Info->MacAddress, KdNet.TargetMac, IEEE_802_ADDR_LENGTH);
    Info->DebuggerHostIp = KdNet.HostIp;
    Info->DebuggerTargetIp = KdNet.TargetIp;
    Info->DebuggerPort = KdNet.Port;
    Info->LinkSpeedMbps = 1000;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdNetShareTransmit(_In_reads_bytes_(Length) const UCHAR *Frame,
                   _In_ ULONG Length)
{
    KIRQL OldIrql;
    BOOLEAN Sent;
    ULONG Poll;

    if (KdNet.Share == NULL || !KdNet.Initialized)
        return STATUS_DEVICE_NOT_READY;
    if (Frame == NULL || Length < KDNET_ETH_HEADER_SIZE ||
        Length > KDNET_FRAME_CAPACITY)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KdNetShareEnter(&OldIrql);

    /* See KdNetHardwareTxSlotFree: bound the wait to something an OS data path
     * can afford at HIGH_LEVEL, and report a full ring as a drop rather than
     * spinning on it.  The sender above us retransmits; the debugger cannot
     * afford for us to hold the machine while we wait. */
    for (Poll = 0; Poll < KDNET_SHARE_TX_POLLS; ++Poll)
    {
        if (KdNetHardwareTxSlotFree())
            break;
        KeStallExecutionProcessor(KDNET_POLL_DELAY_US);
    }
    if (Poll == KDNET_SHARE_TX_POLLS)
    {
        ++KdNet.Share->OsFramesDropped;
        KdNetShareLeave(OldIrql);
        return STATUS_DEVICE_BUSY;
    }

    Sent = KdNetHardwareSend(Frame, Length);
    if (Sent)
        ++KdNet.Share->OsFramesSent;
    else
        ++KdNet.Share->OsFramesDropped;
    KdNetShareLeave(OldIrql);

    return Sent ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

ULONG
NTAPI
KdNetSharePoll(_In_ ULONG MaxFrames)
{
    PKDNET_SHARE_STATE Share = KdNet.Share;
    const UCHAR *Frame;
    ULONG Length, Polled = 0, Delivered = 0;
    KIRQL OldIrql;

    if (Share == NULL || !KdNet.Initialized || Share->Receive == NULL)
        return 0;

    KdNetShareEnter(&OldIrql);
    while (Polled < MaxFrames && Share->Receive != NULL)
    {
        if (!KdNetHardwareReceive(&Frame, &Length))
            break;
        ++Polled;

        /* The debugger's.  Stop here and leave the descriptor unreleased, so
         * the frame is still sitting at the head of the ring when the debugger
         * next polls - in wire order, untouched, and not decoded by anyone
         * else.  Everything behind it waits, which is the intended priority. */
        if (KdNetShareIsDebuggerFrame(Frame, Length))
        {
            ++Share->DebuggerFramesYielded;
            break;
        }

        if (Length >= KDNET_ETH_HEADER_SIZE)
        {
            Share->Receive(Share->Context, Frame, Length);
            ++Delivered;
        }

        KdNetHardwareReleaseReceive();
    }
    Share->OsFramesReceived += Delivered;
    KdNetShareLeave(OldIrql);

    return Delivered;
}

static BOOLEAN
KdNetResolveHost(VOID)
{
    static const UCHAR Broadcast[IEEE_802_ADDR_LENGTH] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const UCHAR Empty[IEEE_802_ADDR_LENGTH] = { 0 };
    ULONG Attempt, IgnoredLength;

    if (KdNet.HostMacValid)
    {
        KdNetTraceMac("using configured host MAC", KdNet.HostMac);
        KdNetFlushDiagnostics();
        return TRUE;
    }
    KdNetTrace("resolving debugger host with ARP");
    for (Attempt = 0; Attempt < KDNET_HANDSHAKE_RETRIES; ++Attempt)
    {
        CHAR Buffer[96];
        BOOLEAN Sent;

        if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                          sizeof(Buffer),
                                          "ARP attempt %lu/%u",
                                          Attempt + 1,
                                          KDNET_HANDSHAKE_RETRIES)))
        {
            KdNetTrace(Buffer);
        }
        Sent = KdNetSendArp(ARP_OPERATION_REQUEST, Broadcast, Empty);
        if (!Sent)
        {
            KdNetTrace("ARP frame transmit failed");
            continue;
        }
        KdNetTrace("ARP frame transmitted; waiting for reply");
        KdNetPollDatagram(NULL, 0, &IgnoredLength, KDNET_SHORT_WAIT_POLLS);
        if (KdNet.HostMacValid)
        {
            KdNetTraceMac("ARP resolved host MAC", KdNet.HostMac);
            KdNetFlushDiagnostics();
            return TRUE;
        }
    }
    KdNetTrace("ARP resolution timed out");
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
        State ^= (ULONGLONG)KdNet.TargetMac[Index] << (Index * 8);

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
    KdNet.ShareEnabled = KdNetHasFlag(Options, "KDNETSHARE");
    Value = KdNetFindOption(Options, "KDNET-HARVEST");
    if (Value != NULL && !KdNetParseHarvestFlags(Value, &KdNet.HarvestFlags))
        return STATUS_INVALID_PARAMETER;
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
KdNetSetupDebugDevice(_In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
                      _In_ USHORT VendorId,
                      _In_ USHORT DeviceId)
{
    RtlZeroMemory(&KdNet.Device, sizeof(KdNet.Device));
    KdNet.Device.Bus = MAXULONG;
    KdNet.Device.Slot = MAXULONG;
    KdNet.Device.VendorID = VendorId;
    KdNet.Device.DeviceID = DeviceId;
    KdNet.Device.BaseClass = 0xff;
    KdNet.Device.SubClass = 0xff;
    KdNet.Device.ProgIf = 0xff;
    KdNet.Device.Memory.Length = KDNET_DMA_LENGTH;
    KdNet.Device.Memory.MaxEnd.QuadPart = MAXULONG;
    KdNet.Device.Memory.Cached = FALSE;
    KdNet.Device.Memory.Aligned = TRUE;
    return KdSetupPciDeviceForDebugging(LoaderBlock, &KdNet.Device);
}

static PDEBUG_DEVICE_ADDRESS
KdNetFindMemoryBar(_In_ ULONG MinimumLength)
{
    ULONG Index;

    for (Index = 0; Index < MAXIMUM_DEBUG_BARS; ++Index)
    {
        if (KdNet.Device.BaseAddress[Index].Valid &&
            KdNet.Device.BaseAddress[Index].Type == CmResourceTypeMemory &&
            KdNet.Device.BaseAddress[Index].Length >= MinimumLength)
        {
            return &KdNet.Device.BaseAddress[Index];
        }
    }
    return NULL;
}

static NTSTATUS
KdNetInitializeRtl8168(VOID)
{
    NTSTATUS Status;
    ULONG Index;
    ULONG_PTR Base, Aligned;
    ULONGLONG Offset;
    PDEBUG_DEVICE_ADDRESS Address;

    RtlZeroMemory(&KdNet.Rtl8168, sizeof(KdNet.Rtl8168));
    KdNetTrace("RTL8168 selecting register BAR and DMA memory");
    Address = KdNetFindMemoryBar(0x100);
    if (Address != NULL)
    {
        KdNet.Rtl8168.IoBaseIsMmio = TRUE;
    }
    else
    {
        for (Index = 0; Index < MAXIMUM_DEBUG_BARS; ++Index)
        {
            if (KdNet.Device.BaseAddress[Index].Valid &&
                KdNet.Device.BaseAddress[Index].Type == CmResourceTypePort &&
                KdNet.Device.BaseAddress[Index].Length >= 0x100)
            {
                Address = &KdNet.Device.BaseAddress[Index];
                break;
            }
        }
    }
    if (Address == NULL || KdNet.Device.Memory.VirtualAddress == NULL)
    {
        KdNetTrace("RTL8168 missing usable BAR or DMA mapping");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    KdNet.Rtl8168.IoBase = Address->TranslatedAddress;

    Base = (ULONG_PTR)KdNet.Device.Memory.VirtualAddress;
    Aligned = (Base + KDNET_DMA_ALIGNMENT - 1) & ~(KDNET_DMA_ALIGNMENT - 1);
    Offset = Aligned - Base;

#define KDNET_ASSIGN_RTL_DMA(_VaField, _PaField, _Bytes)                    \
    do                                                                       \
    {                                                                        \
        KdNet.Rtl8168._VaField = (PVOID)(Base + (ULONG_PTR)Offset);          \
        KdNet.Rtl8168._PaField.QuadPart =                                   \
            KdNet.Device.Memory.Start.QuadPart + Offset;                     \
        Offset += (_Bytes);                                                  \
        Offset = (Offset + KDNET_DMA_ALIGNMENT - 1) &                       \
                 ~(ULONGLONG)(KDNET_DMA_ALIGNMENT - 1);                      \
    } while (0)

    KDNET_ASSIGN_RTL_DMA(TxRing, TxRingPa, TX_DESC_COUNT * sizeof(RTL_DESC));
    KDNET_ASSIGN_RTL_DMA(RxRing, RxRingPa, RX_DESC_COUNT * sizeof(RTL_DESC));
    KDNET_ASSIGN_RTL_DMA(TxBuffers, TxBuffersPa, TX_DESC_COUNT * RX_BUF_SIZE);
    KDNET_ASSIGN_RTL_DMA(RxBuffers, RxBuffersPa, RX_DESC_COUNT * RX_BUF_SIZE);
#undef KDNET_ASSIGN_RTL_DMA

    if (Offset > KdNet.Device.Memory.Length)
    {
        KdNetTrace("RTL8168 DMA allocation is too small");
        return STATUS_BUFFER_TOO_SMALL;
    }

    KdNetTrace("RTL8168 detecting chip revision");
    Status = NICDetectChipVersion(&KdNet.Rtl8168);
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("RTL8168 chip detection failed", Status);
        return Status;
    }
    {
        CHAR Buffer[128];
        if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                          sizeof(Buffer),
                                          "RTL8168 chip detected: MAC version %u, TxConfig 0x%08lx",
                                          KdNet.Rtl8168.MacVersion,
                                          KdNet.Rtl8168.TxConfigRaw)))
        {
            KdNetTrace(Buffer);
        }
    }
    NICInitializeHw(&KdNet.Rtl8168);
    KdNetTrace("RTL8168 soft reset");
    Status = NICSoftReset(&KdNet.Rtl8168);
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("RTL8168 soft reset failed", Status);
        return Status;
    }
    KdNetTrace("RTL8168 PHY power-up and MAC read");
    RtlPllPowerUp(&KdNet.Rtl8168);
    Status = NICGetPermanentMacAddress(&KdNet.Rtl8168,
                                       KdNet.Rtl8168.PermanentMacAddress);
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("RTL8168 MAC read failed", Status);
        return Status;
    }
    RtlCopyMemory(KdNet.Rtl8168.CurrentMacAddress,
                  KdNet.Rtl8168.PermanentMacAddress,
                  IEEE_802_ADDR_LENGTH);
    RtlCopyMemory(KdNet.TargetMac,
                  KdNet.Rtl8168.CurrentMacAddress,
                  IEEE_802_ADDR_LENGTH);
    KdNetTraceMac("target MAC", KdNet.TargetMac);
    KdNetTrace("RTL8168 configuring PHY");
    RtlHwPhyConfig(&KdNet.Rtl8168);
    KdNet.Rtl8168.PacketFilter = NDIS_PACKET_TYPE_DIRECTED |
                                 NDIS_PACKET_TYPE_BROADCAST;
    KdNetTrace("RTL8168 programming DMA rings");
    Status = NICProgramRings(&KdNet.Rtl8168);
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("RTL8168 ring programming failed", Status);
        return Status;
    }
    NICDisableInterrupts(&KdNet.Rtl8168);
    NICAcknowledgeInterrupts(&KdNet.Rtl8168, 0xffff);
    KdNetTrace("RTL8168 enabling transmit and receive");
    Status = NICEnableTxRx(&KdNet.Rtl8168);
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("RTL8168 TX/RX enable failed", Status);
        return Status;
    }
    NICUpdateLinkStatus(&KdNet.Rtl8168);
    {
        CHAR Buffer[128];
        if (NT_SUCCESS(RtlStringCbPrintfA(
                Buffer,
                sizeof(Buffer),
                "RTL8168 link: %s, speed %lu Mbps, PHY status 0x%02x",
                KdNet.Rtl8168.MediaState == NdisMediaStateConnected
                    ? "up" : "down",
                KdNet.Rtl8168.LinkSpeedMbps,
                KdNet.Rtl8168.LastPhyStatus)))
        {
            KdNetTrace(Buffer);
        }
    }
    return STATUS_SUCCESS;
}

static BOOLEAN
KdNetE1000ReadEeprom(_In_ ULONG Word, _Out_ PUSHORT Value)
{
    ULONG Data, Poll;

    KdNetE1000Write(KDNET_E1000_REG_EERD,
                    KDNET_E1000_EERD_START |
                    (Word << KDNET_E1000_EERD_ADDR_SHIFT));
    for (Poll = 0; Poll < 10000; ++Poll)
    {
        Data = KdNetE1000Read(KDNET_E1000_REG_EERD);
        if ((Data & KDNET_E1000_EERD_DONE) != 0)
        {
            *Value = (USHORT)(Data >> KDNET_E1000_EERD_DATA_SHIFT);
            return TRUE;
        }
        KeStallExecutionProcessor(5);
    }
    return FALSE;
}

static NTSTATUS
KdNetInitializeE1000(VOID)
{
    PDEBUG_DEVICE_ADDRESS Address;
    ULONG_PTR Base, Aligned;
    ULONGLONG Offset;
    ULONG Control, Index, Poll, Ral, Rah;
    USHORT Word;

    Address = KdNetFindMemoryBar(0x6000);
    if (Address == NULL || KdNet.Device.Memory.VirtualAddress == NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    RtlZeroMemory(&KdNet.E1000, sizeof(KdNet.E1000));
    KdNet.E1000.IoBase = Address->TranslatedAddress;
    for (Index = 0; Index < MAXIMUM_DEBUG_BARS; ++Index)
    {
        if (KdNet.Device.BaseAddress[Index].Valid &&
            KdNet.Device.BaseAddress[Index].Type == CmResourceTypePort &&
            KdNet.Device.BaseAddress[Index].Length >= 8)
        {
            KdNet.E1000.IoPort =
                KdNet.Device.BaseAddress[Index].TranslatedAddress;
            break;
        }
    }
    Base = (ULONG_PTR)KdNet.Device.Memory.VirtualAddress;
    Aligned = (Base + KDNET_DMA_ALIGNMENT - 1) & ~(KDNET_DMA_ALIGNMENT - 1);
    Offset = Aligned - Base;

#define KDNET_ASSIGN_E1000_DMA(_VaField, _PaField, _Bytes)                  \
    do                                                                       \
    {                                                                        \
        KdNet.E1000._VaField = (PVOID)(Base + (ULONG_PTR)Offset);            \
        KdNet.E1000._PaField.QuadPart =                                     \
            KdNet.Device.Memory.Start.QuadPart + Offset;                     \
        Offset += (_Bytes);                                                  \
        Offset = (Offset + KDNET_DMA_ALIGNMENT - 1) &                       \
                 ~(ULONGLONG)(KDNET_DMA_ALIGNMENT - 1);                      \
    } while (0)

    KDNET_ASSIGN_E1000_DMA(TxRing, TxRingPa,
                           KDNET_E1000_DESC_COUNT * sizeof(KDNET_E1000_TX_DESCRIPTOR));
    KDNET_ASSIGN_E1000_DMA(RxRing, RxRingPa,
                           KDNET_E1000_DESC_COUNT * sizeof(KDNET_E1000_RX_DESCRIPTOR));
    KDNET_ASSIGN_E1000_DMA(TxBuffers, TxBuffersPa,
                           KDNET_E1000_DESC_COUNT * KDNET_E1000_BUFFER_SIZE);
    KDNET_ASSIGN_E1000_DMA(RxBuffers, RxBuffersPa,
                           KDNET_E1000_DESC_COUNT * KDNET_E1000_BUFFER_SIZE);
#undef KDNET_ASSIGN_E1000_DMA

    if (Offset > KdNet.Device.Memory.Length)
        return STATUS_BUFFER_TOO_SMALL;

    KdNetE1000Write(KDNET_E1000_REG_IMC, MAXULONG);
    KdNetE1000Write(KDNET_E1000_REG_RCTL, 0);
    KdNetE1000Write(KDNET_E1000_REG_TCTL, 0);
    Control = KdNetE1000Read(KDNET_E1000_REG_CTRL);
    if (KdNet.E1000.IoPort != NULL)
        KdNetE1000WriteIo(KDNET_E1000_REG_CTRL,
                          Control | KDNET_E1000_CTRL_RST);
    else
        KdNetE1000Write(KDNET_E1000_REG_CTRL,
                        Control | KDNET_E1000_CTRL_RST);
    for (Poll = 0; Poll < 1000; ++Poll)
    {
        KeStallExecutionProcessor(10);
        Control = KdNetE1000Read(KDNET_E1000_REG_CTRL);
        if ((Control & KDNET_E1000_CTRL_RST) == 0)
            break;
    }
    if (Poll == 1000)
        return STATUS_IO_TIMEOUT;

    KdNetE1000Write(KDNET_E1000_REG_IMC, MAXULONG);
    (void)KdNetE1000Read(KDNET_E1000_REG_ICR);
    Control &= ~(KDNET_E1000_CTRL_RST | KDNET_E1000_CTRL_LRST |
                 KDNET_E1000_CTRL_VME);
    Control |= KDNET_E1000_CTRL_ASDE | KDNET_E1000_CTRL_SLU;
    KdNetE1000Write(KDNET_E1000_REG_CTRL, Control);

    Ral = KdNetE1000Read(KDNET_E1000_REG_RAL);
    Rah = KdNetE1000Read(KDNET_E1000_REG_RAH);
    if ((Rah & KDNET_E1000_RAH_AV) != 0 && (Ral != 0 || (Rah & 0xffff) != 0))
    {
        KdNet.TargetMac[0] = (UCHAR)Ral;
        KdNet.TargetMac[1] = (UCHAR)(Ral >> 8);
        KdNet.TargetMac[2] = (UCHAR)(Ral >> 16);
        KdNet.TargetMac[3] = (UCHAR)(Ral >> 24);
        KdNet.TargetMac[4] = (UCHAR)Rah;
        KdNet.TargetMac[5] = (UCHAR)(Rah >> 8);
    }
    else
    {
        for (Index = 0; Index < 3; ++Index)
        {
            if (!KdNetE1000ReadEeprom(Index, &Word))
                return STATUS_DEVICE_DATA_ERROR;
            KdNet.TargetMac[Index * 2] = (UCHAR)Word;
            KdNet.TargetMac[Index * 2 + 1] = (UCHAR)(Word >> 8);
        }
        Ral = (ULONG)KdNet.TargetMac[0] |
              ((ULONG)KdNet.TargetMac[1] << 8) |
              ((ULONG)KdNet.TargetMac[2] << 16) |
              ((ULONG)KdNet.TargetMac[3] << 24);
        Rah = (ULONG)KdNet.TargetMac[4] |
              ((ULONG)KdNet.TargetMac[5] << 8) |
              KDNET_E1000_RAH_AV;
        KdNetE1000Write(KDNET_E1000_REG_RAL, Ral);
        KdNetE1000Write(KDNET_E1000_REG_RAH, Rah);
    }

    RtlZeroMemory(KdNet.E1000.TxRing,
                  KDNET_E1000_DESC_COUNT * sizeof(KDNET_E1000_TX_DESCRIPTOR));
    RtlZeroMemory(KdNet.E1000.RxRing,
                  KDNET_E1000_DESC_COUNT * sizeof(KDNET_E1000_RX_DESCRIPTOR));
    for (Index = 0; Index < KDNET_E1000_DESC_COUNT; ++Index)
    {
        KdNet.E1000.TxRing[Index].Status = KDNET_E1000_TX_DD;
        KdNet.E1000.RxRing[Index].Address =
            KdNet.E1000.RxBuffersPa.QuadPart +
            (ULONGLONG)Index * KDNET_E1000_BUFFER_SIZE;
    }
    KeMemoryBarrier();

    KdNetE1000Write(KDNET_E1000_REG_TDBAH, KdNet.E1000.TxRingPa.HighPart);
    KdNetE1000Write(KDNET_E1000_REG_TDBAL, KdNet.E1000.TxRingPa.LowPart);
    KdNetE1000Write(KDNET_E1000_REG_TDLEN,
                    KDNET_E1000_DESC_COUNT * sizeof(KDNET_E1000_TX_DESCRIPTOR));
    KdNetE1000Write(KDNET_E1000_REG_TDH, 0);
    KdNetE1000Write(KDNET_E1000_REG_TDT, 0);
    KdNetE1000Write(KDNET_E1000_REG_TIPG, KDNET_E1000_TIPG_DEFAULT);
    KdNetE1000Write(KDNET_E1000_REG_TCTL,
                    KDNET_E1000_TCTL_EN | KDNET_E1000_TCTL_PSP);

    KdNetE1000Write(KDNET_E1000_REG_RDBAH, KdNet.E1000.RxRingPa.HighPart);
    KdNetE1000Write(KDNET_E1000_REG_RDBAL, KdNet.E1000.RxRingPa.LowPart);
    KdNetE1000Write(KDNET_E1000_REG_RDLEN,
                    KDNET_E1000_DESC_COUNT * sizeof(KDNET_E1000_RX_DESCRIPTOR));
    KdNetE1000Write(KDNET_E1000_REG_RDH, 0);
    KdNetE1000Write(KDNET_E1000_REG_RDT, KDNET_E1000_DESC_COUNT - 1);
    KdNetE1000Write(KDNET_E1000_REG_RCTL,
                    KDNET_E1000_RCTL_EN | KDNET_E1000_RCTL_BAM |
                    KDNET_E1000_RCTL_SECRC);
    return STATUS_SUCCESS;
}

static NTSTATUS
KdNetInitializeHardware(_In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    NTSTATUS Status;

    KdNetTrace("searching PCI for Realtek 10ec:8168");
    Status = KdNetSetupDebugDevice(LoaderBlock, 0x10ec, 0x8168);
    if (NT_SUCCESS(Status))
    {
        KdNetTraceDevice("RTL8168/8111");
        KdNet.Backend = KdNetBackendRtl8168;
        Status = KdNetInitializeRtl8168();
        KdNetTraceStatus("RTL8168 hardware initialization", Status);
        return Status;
    }

    KdNetTraceStatus("RTL8168 PCI setup failed", Status);
    KdNetTrace("searching PCI for QEMU Intel 8086:100e fallback");
    Status = KdNetSetupDebugDevice(LoaderBlock, 0x8086, 0x100e);
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("e1000 PCI setup failed", Status);
        return Status;
    }
    KdNetTraceDevice("e1000");
    KdNet.Backend = KdNetBackendE1000;
    Status = KdNetInitializeE1000();
    KdNetTraceStatus("e1000 hardware initialization", Status);
    return Status;
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
                               KdNet.SendPacket,
                               sizeof(KdNet.SendPacket),
                               &PacketLength);
    if (Status != KdNetStatusSuccess)
    {
        CHAR Buffer[128];

        if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                          sizeof(Buffer),
                                          "outer encode failed: type %u status %u",
                                          Type,
                                          (ULONG)Status)))
        {
            KdNetTrace(Buffer);
        }
        return FALSE;
    }
    if (!KdNetSendUdp(KdNet.SendPacket, (ULONG)PacketLength))
    {
        CHAR Buffer[128];

        if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                          sizeof(Buffer),
                                          "UDP transmit failed: outer type %u length %Iu",
                                          Type,
                                          PacketLength)))
        {
            KdNetTrace(Buffer);
        }
        return FALSE;
    }
    return TRUE;
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

    KdNetTrace("starting debugger handshake");
    if (!KdNetResolveHost())
        return STATUS_HOST_UNREACHABLE;
    KdNetFlushDiagnostics();

    KdNetGenerateClientKey();
    Status = KdNetBuildPokePayload(KdNet.ClientKey,
                                   Poke,
                                   sizeof(Poke),
                                   &PokeLength);
    if (Status != KdNetStatusSuccess)
    {
        KdNetTrace("could not build handshake Poke payload");
        KdNetFlushDiagnostics();
        return STATUS_UNSUCCESSFUL;
    }

    KdNet.SendSequence = 1;
    for (Attempt = 0; Attempt < KDNET_HANDSHAKE_RETRIES; ++Attempt)
    {
        CHAR Buffer[96];

        if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                          sizeof(Buffer),
                                          "handshake attempt %lu/%u",
                                          Attempt + 1,
                                          KDNET_HANDSHAKE_RETRIES)))
        {
            KdNetTrace(Buffer);
        }
        if (!KdNetSendProtected(KDNET_PACKET_TYPE_CONTROL,
                                Poke,
                                (ULONG)PokeLength))
        {
            KdNetTrace("handshake control packet transmit failed");
            KdNetFlushDiagnostics();
            continue;
        }
        KdNetTrace("handshake control packet sent; waiting for debugger");
        KdNetFlushDiagnostics();
        if (!KdNetPollDatagram(KdNet.Packet,
                               sizeof(KdNet.Packet),
                               &PacketLength,
                               KDNET_LONG_WAIT_POLLS))
        {
            KdNetTrace("no debugger response received");
            KdNetFlushDiagnostics();
            continue;
        }
        if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                          sizeof(Buffer),
                                          "received debugger UDP payload: %lu bytes",
                                          PacketLength)))
        {
            KdNetTrace(Buffer);
        }
        Status = KdNetDecodePacket(&KdNet.Crypto,
                                   KdNet.Packet,
                                   PacketLength,
                                   &PacketInfo);
        if (NT_SUCCESS(RtlStringCbPrintfA(
                Buffer,
                sizeof(Buffer),
                "outer decode: status %u type %u direction %u payload %Iu",
                (ULONG)Status,
                Status == KdNetStatusSuccess ? PacketInfo.Type : 0xff,
                Status == KdNetStatusSuccess ? PacketInfo.Direction : 0xff,
                Status == KdNetStatusSuccess ? PacketInfo.PayloadLength : 0)))
        {
            KdNetTrace(Buffer);
        }
        if (Status != KdNetStatusSuccess ||
            PacketInfo.Type != KDNET_PACKET_TYPE_CONTROL ||
            PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER)
        {
            KdNetTrace("discarded invalid debugger response");
            KdNetFlushDiagnostics();
            continue;
        }
        Status = KdNetProcessResponsePayload(&KdNet.Crypto,
                                             KdNet.ClientKey,
                                             PacketInfo.Payload,
                                             PacketInfo.PayloadLength);
        if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                          sizeof(Buffer),
                                          "control response authentication status %u",
                                          (ULONG)Status)))
        {
            KdNetTrace(Buffer);
        }
        if (Status == KdNetStatusSuccess)
        {
            HandshakeComplete = TRUE;
            KdNetTrace("debugger handshake authenticated");
            KdNetFlushDiagnostics();
            break;
        }
        KdNetTrace("debugger handshake authentication failed");
        KdNetFlushDiagnostics();
    }
    if (!HandshakeComplete)
    {
        KdNetTrace("debugger handshake timed out");
        KdNetFlushDiagnostics();
        return STATUS_IO_TIMEOUT;
    }

    KdStatus = KdPacketEncodeUnused(KdNet.InnerPacket,
                                    sizeof(KdNet.InnerPacket),
                                    &InnerLength);
    if (KdStatus != KdPacketStatusSuccess ||
        !KdNetSendProtected(KDNET_PACKET_TYPE_DATA,
                            KdNet.InnerPacket,
                            (ULONG)InnerLength))
    {
        KdNetTrace("failed to send initial inner KD packet");
        KdNetFlushDiagnostics();
        return STATUS_UNSUCCESSFUL;
    }
    KdNetTrace("inner KD session ready");
    KdNetFlushDiagnostics();
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
            CHAR Buffer[192];

            if (NT_SUCCESS(RtlStringCbPrintfA(
                    Buffer,
                    sizeof(Buffer),
                    "inner receive outer reject: status %u type %u direction %u",
                    (ULONG)NetStatus,
                    NetStatus == KdNetStatusSuccess ? PacketInfo.Type : 0xff,
                    NetStatus == KdNetStatusSuccess ? PacketInfo.Direction : 0xff)))
            {
                KdNetTrace(Buffer);
            }
            KdNetFlushDiagnostics();
            if (ExpectedType == PACKET_TYPE_KD_POLL_BREAKIN)
                PollCount = 1;
            continue;
        }

        if (PacketInfo.PayloadLength == 1 &&
            PacketInfo.Payload[0] == BREAKIN_PACKET_BYTE)
        {
            KdNetTrace("debugger break-in byte received");
            KdNetFlushDiagnostics();
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
            CHAR Buffer[128];

            if (NT_SUCCESS(RtlStringCbPrintfA(Buffer,
                                              sizeof(Buffer),
                                              "inner KD decode failed: status %u payload %Iu",
                                              (ULONG)PacketStatus,
                                              PacketInfo.PayloadLength)))
            {
                KdNetTrace(Buffer);
            }
            KdPacketSessionRejectMalformed(&Result);
            KdNetSendControl(Result.ControlType, Result.ControlId);
            KdNetFlushDiagnostics();
            if (ExpectedType == PACKET_TYPE_KD_POLL_BREAKIN)
                return KdPacketTimedOut;
            continue;
        }
        PacketStatus = KdPacketSessionProcess(&KdNet.Session, &View, &Result);
        if (PacketStatus != KdPacketStatusSuccess)
            continue;

        if (Result.Event == KdPacketSessionEventResendLast ||
            Result.Event == KdPacketSessionEventReset)
        {
            if (Result.ControlType != KD_PACKET_SESSION_NO_CONTROL)
                KdNetSendControl(Result.ControlType, Result.ControlId);
            return KdPacketNeedsResend;
        }
        if (Result.Event == KdPacketSessionEventSendAcknowledged)
        {
            if (Result.ControlType != KD_PACKET_SESSION_NO_CONTROL)
                KdNetSendControl(Result.ControlType, Result.ControlId);
            if (ExpectedType == PACKET_TYPE_KD_ACKNOWLEDGE)
                return KdPacketReceived;
            continue;
        }
        if (Result.Event != KdPacketSessionEventDataReceived ||
            View.Type != ExpectedType)
        {
            if (Result.ControlType != KD_PACKET_SESSION_NO_CONTROL)
                KdNetSendControl(Result.ControlType, Result.ControlId);
            if (ExpectedType == PACKET_TYPE_KD_POLL_BREAKIN)
                return KdPacketTimedOut;
            continue;
        }

        if (MessageHeader == NULL)
        {
            if (Result.ControlType != KD_PACKET_SESSION_NO_CONTROL)
                KdNetSendControl(Result.ControlType, Result.ControlId);
            return KdPacketReceived;
        }
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

        /*
         * View.Payload points into KdNet.Packet.  Copy the request before
         * acknowledging it so future transmit-buffer changes cannot invalidate
         * the decoded view before the kernel consumes it.
         */
        if (Result.ControlType != KD_PACKET_SESSION_NO_CONTROL)
            KdNetSendControl(Result.ControlType, Result.ControlId);
        return KdPacketReceived;
    }
}

NTSTATUS
NTAPI
KdDebuggerInitialize0(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    NTSTATUS Status;

    RtlZeroMemory(&KdNet, sizeof(KdNet));
    KdNet.LoaderBlock = LoaderBlock;
    InbvEnableDisplayString(TRUE);
    KdNetTrace("transport initialization entered");
    if (LoaderBlock == NULL)
    {
        KdNetTrace("loader block is missing");
        return STATUS_INVALID_PARAMETER;
    }
    Status = KdNetConfigureOptions(LoaderBlock->LoadOptions);
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("boot option parsing failed", Status);
        return Status;
    }
    KdNetTraceConfiguration();
    Status = KdNetInitializeHardware(LoaderBlock);
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("hardware initialization failed", Status);
        return Status;
    }

    KdPacketSessionInitialize(&KdNet.Session, 3);
    Status = KdNetHandshake();
    if (!NT_SUCCESS(Status))
    {
        KdNetTraceStatus("debugger handshake failed", Status);
        KdNetFlushDiagnostics();
        return Status;
    }
    KdNet.Initialized = TRUE;
    KdNetTrace("transport initialized successfully");
    KdNetFlushDiagnostics();
    KdNetRunAutomaticHarvest(KDNET_HARVEST_STATUS |
                             KDNET_HARVEST_NIC |
                             KDNET_HARVEST_LOADER,
                             "phase0");
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdDebuggerInitialize1(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    if (!KdNet.Initialized)
        return STATUS_DEVICE_NOT_READY;
    if (LoaderBlock != NULL)
        KdNet.LoaderBlock = LoaderBlock;
    KdNetRunAutomaticHarvest(KDNET_HARVEST_STATUS |
                             KDNET_HARVEST_NIC |
                             KDNET_HARVEST_PCI,
                             "phase1");
    return STATUS_SUCCESS;
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
    {
        KdNetTrace("KdSendPacket: session rejected begin-send");
        KdNetFlushDiagnostics();
        return;
    }
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
        KdNetTrace("KdSendPacket: inner packet encoding failed");
        KdNetFlushDiagnostics();
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
        {
            return;
        }
        if (ReceiveStatus == KdPacketTimedOut)
        {
            KdNetTrace("KdSendPacket: acknowledgement timed out");
            KdNetFlushDiagnostics();
            KdPacketSessionTimeout(&KdNet.Session, &Result);
            if (Result.Event == KdPacketSessionEventRetryLimit)
            {
                KdNetTrace("KdSendPacket: session retry limit reached");
                KdNetFlushDiagnostics();
                KdPacketSessionInitialize(&KdNet.Session, 3);
                return;
            }
        }
    }
    KdNetTrace("KdSendPacket: retries exhausted; resetting inner session");
    KdNetFlushDiagnostics();
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
