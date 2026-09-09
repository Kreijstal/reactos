/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Gen3 firmware self-init DMA image preparation.
 */

#include "../iwlwifi.h"

#define NDEBUG
#include <debug.h>

#pragma pack(push, 1)
typedef struct _IWL_RX_TRANSFER_DESC
{
    USHORT Rbid;
    USHORT Reserved[3];
    ULONGLONG Address;
} IWL_RX_TRANSFER_DESC;

typedef struct _IWL_RX_COMPLETION_DESC
{
    ULONG Reserved;
    USHORT Rbid;
    UCHAR Flags;
    UCHAR Reserved2[25];
} IWL_RX_COMPLETION_DESC;

typedef struct _IWL_RX_PACKET
{
    ULONG LengthAndFlags;
    UCHAR Command;
    UCHAR Group;
    USHORT Sequence;
    UCHAR Data[1];
} IWL_RX_PACKET;

typedef struct _IWL_LMAC_ALIVE
{
    ULONG Major, Minor;
    UCHAR Subtype, Type, Mac, Optional;
    ULONG Timestamp;
    ULONG DebugAddresses[8];
} IWL_LMAC_ALIVE;

typedef struct _IWL_UMAC_ALIVE
{
    ULONG Major, Minor;
    ULONG DebugAddresses[2];
} IWL_UMAC_ALIVE;

typedef struct _IWL_ALIVE_V6
{
    USHORT Status, Flags;
    IWL_LMAC_ALIVE Lmac[2];
    IWL_UMAC_ALIVE Umac;
    ULONG SkuId[3];
    ULONGLONG ImrBase;
    ULONG ImrSize, ImrEnabled;
} IWL_ALIVE_V6;

typedef struct _IWL_TFH_TB
{
    USHORT Length;
    ULONGLONG Address;
} IWL_TFH_TB;

typedef struct _IWL_TFH_TFD
{
    USHORT NumberOfTbs;
    IWL_TFH_TB Tb[25];
    ULONG Padding;
} IWL_TFH_TFD;

typedef struct _IWL_WIDE_COMMAND
{
    UCHAR Command, Group;
    USHORT Sequence, Length;
    UCHAR Reserved, Version;
    ULONG Payload;
} IWL_WIDE_COMMAND;

typedef struct _IWL_NVM_INFO_V4
{
    ULONG GeneralFlags;
    USHORT NvmVersion;
    UCHAR BoardType, HardwareAddressCount;
    ULONG MacSkuFlags;
    ULONG TxChains, RxChains;
    ULONG LarEnabled, ChannelCount;
    ULONG ChannelProfile[110];
} IWL_NVM_INFO_V4;

typedef struct _IWL_SCAN_CONFIG_V5
{
    UCHAR CamMode, PromiscuousMode, BroadcastStationId, Reserved;
    ULONG TxChains, RxChains;
} IWL_SCAN_CONFIG_V5;

typedef struct _IWL_SCAN_CHANNEL_CFG_UMAC
{
    ULONG Flags;
    UCHAR ChannelNumber, Psd20, IterationCount, IterationInterval;
} IWL_SCAN_CHANNEL_CFG_UMAC;

typedef struct _IWL_SCAN_GENERAL_PARAMS_V11
{
    USHORT Flags;
    UCHAR Reserved, ScanStartMacId;
    UCHAR ActiveDwell[2];
    UCHAR AdaptiveDwell2G, AdaptiveDwell5G, AdaptiveDwellSocial, Flags2;
    USHORT AdaptiveDwellMaxBudget;
    ULONG MaxOutOfTime[2], SuspendTime[2], ScanPriority;
    UCHAR PassiveDwell[2], FragmentCount[2];
} IWL_SCAN_GENERAL_PARAMS_V11;

typedef struct _IWL_SCAN_CHANNEL_PARAMS_V7
{
    UCHAR Flags, Count, NumberOfApsOverride[2];
    IWL_SCAN_CHANNEL_CFG_UMAC Channel[67];
} IWL_SCAN_CHANNEL_PARAMS_V7;

typedef struct _IWL_SCAN_PERIODIC_PARAMS_V1
{
    struct { USHORT Interval; UCHAR IterationCount, Reserved; } Schedule[2];
    USHORT Delay, Reserved;
} IWL_SCAN_PERIODIC_PARAMS_V1;

typedef struct _IWL_SCAN_PROBE_SEGMENT
{
    USHORT Offset, Length;
} IWL_SCAN_PROBE_SEGMENT;

typedef struct _IWL_SCAN_PROBE_REQUEST
{
    IWL_SCAN_PROBE_SEGMENT MacHeader, BandData[3], CommonData;
    UCHAR Buffer[512];
} IWL_SCAN_PROBE_REQUEST;

typedef struct _IWL_SSID_IE
{
    UCHAR Id, Length, Ssid[32];
} IWL_SSID_IE;

typedef struct _IWL_SCAN_PROBE_PARAMS_V4
{
    IWL_SCAN_PROBE_REQUEST Request;
    UCHAR ShortSsidCount, BssidCount;
    USHORT Reserved;
    IWL_SSID_IE DirectScan[20];
    ULONG ShortSsid[8];
    UCHAR Bssid[16][6];
} IWL_SCAN_PROBE_PARAMS_V4;

typedef struct _IWL_SCAN_REQ_UMAC_V17
{
    ULONG Uid, OutOfChannelPriority;
    IWL_SCAN_GENERAL_PARAMS_V11 General;
    IWL_SCAN_CHANNEL_PARAMS_V7 Channels;
    IWL_SCAN_PERIODIC_PARAMS_V1 Periodic;
    IWL_SCAN_PROBE_PARAMS_V4 Probe;
} IWL_SCAN_REQ_UMAC_V17;

typedef struct _IWL_MAC_CONFIG_V2
{
    ULONG IdAndColor, Action, MacType;
    UCHAR LocalMldAddress[6];
    USHORT ReservedAddress;
    ULONG FilterFlags;
    USHORT HeSupport, HeApSupport;
    ULONG EhtSupport, NicNotAckEnabled;
    struct
    {
        UCHAR IsAssociated, EsrTransitionTimeout;
        USHORT MediumSyncDelay, AssociationId, Reserved1;
        USHORT DataPolicy, Reserved2;
        ULONG CtWindow;
    } Client;
} IWL_MAC_CONFIG_V2;

typedef struct _IWL_AC_QOS
{
    USHORT CwMin, CwMax;
    UCHAR Aifsn, FifosMask;
    USHORT EdcaTxop;
} IWL_AC_QOS;

typedef struct _IWL_HE_BACKOFF_CONFIG
{
    USHORT CwMin, CwMax, Aifsn, MuTime;
} IWL_HE_BACKOFF_CONFIG;

typedef struct _IWL_LINK_CONFIG_V2
{
    ULONG Action, LinkId, MacId, PhyId;
    UCHAR LocalLinkAddress[6];
    USHORT ReservedAddress;
    ULONG ModifyMask, Active, ListenLmac;
    ULONG CckRates, OfdmRates, CckShortPreamble, ShortSlot;
    ULONG ProtectionFlags, QosFlags;
    IWL_AC_QOS Ac[5];
    UCHAR HtcPacketExtension, RandomCwMin, RandomCwMax, FeedbackThreshold;
    IWL_HE_BACKOFF_CONFIG TriggerBackoff[4];
    ULONG BeaconInterval, DtimInterval;
    USHORT PunctureMask, FrameTimeRtsThreshold;
    ULONG Flags, FlagsMask;
    UCHAR ReferenceBssid[6];
    USHORT ReservedReferenceBssid;
    UCHAR BssidIndex, BssColor, SpecificationLinkId, Reserved;
    UCHAR IbssBssid[6];
    USHORT ReservedIbssBssid;
    ULONG ReservedTail[8];
} IWL_LINK_CONFIG_V2;

typedef struct _IWL_PHY_CONTEXT_V4
{
    ULONG IdAndColor, Action;
    struct
    {
        ULONG Channel;
        UCHAR Band, Width, ControlPosition, Reserved;
    } ChannelInfo;
    ULONG LmacId, RxChainInfo, DspConfigFlags, Reserved;
} IWL_PHY_CONTEXT_V4;

typedef struct _IWL_RLC_CONFIG_V2
{
    ULONG PhyId, RxChainInfo, RlcReserved;
    ULONG SadChainA, SadChainB, SadMacId, SadReserved;
    UCHAR Flags, Reserved[3];
} IWL_RLC_CONFIG_V2;

typedef struct _IWL_STA_CONFIG_V1
{
    ULONG StaId, LinkId;
    UCHAR PeerMldAddress[6];
    USHORT ReservedMldAddress;
    UCHAR PeerLinkAddress[6];
    USHORT ReservedLinkAddress;
    ULONG StationType, AssociationId, BeamformFlags, Mfp, Mimo;
    ULONG MimoProtection, AckEnabled, TriggerRandomAllocation;
    ULONG TxAmpduSpacing, TxAmpduMaxSize, ServicePeriodLength, UapsdAcs;
    UCHAR PacketExtensionThreshold[2][5][2];
    ULONG HtcFlags;
} IWL_STA_CONFIG_V1;

typedef struct _IWL_SCD_QUEUE_CONFIG_V3
{
    ULONG Operation, StationMask;
    UCHAR Tid, Reserved[3];
    ULONG Flags, CircularBufferSize;
    ULONGLONG ByteCountAddress, TfdAddress;
} IWL_SCD_QUEUE_CONFIG_V3;

typedef struct _IWL_TX_QUEUE_CONFIG_RESPONSE
{
    USHORT QueueNumber, Flags, WritePointer, Reserved;
} IWL_TX_QUEUE_CONFIG_RESPONSE;

typedef struct _IWL_TX_COMMAND_GEN3
{
    USHORT Length, Flags;
    ULONG OffloadAssist;
    ULONG PacketNumberLow;
    USHORT PacketNumberHigh, AuxInfo;
    ULONG RateAndFlags;
    UCHAR Reserved[8];
} IWL_TX_COMMAND_GEN3;

typedef struct _IWL_TLC_CONFIG_V4
{
    UCHAR StationId, Reserved[3];
    UCHAR MaxChannelWidth, Mode, Chains, ShortGuardIntervalWidths;
    USHORT Flags, NonHtRates;
    USHORT HtRates[2][3];
    USHORT MaxMpduLength, MaxTxOp;
} IWL_TLC_CONFIG_V4;

typedef struct _IWL_SESSION_PROTECTION_V2
{
    ULONG LinkId, Action, ConfigurationId, DurationTu;
    ULONG RepetitionCount, Interval;
} IWL_SESSION_PROTECTION_V2;

typedef struct _IWL_SEC_KEY_CMD_V1
{
    ULONG Action;
    ULONG StationMask;
    ULONG KeyId;
    ULONG KeyFlags;
    UCHAR Key[32];
    UCHAR TkipMicRxKey[8];
    UCHAR TkipMicTxKey[8];
    ULONGLONG RxSequence;
    ULONGLONG TxSequence;
} IWL_SEC_KEY_CMD_V1;

typedef struct _IWL_IEEE80211_AUTH
{
    USHORT FrameControl, Duration;
    UCHAR Destination[6], Source[6], Bssid[6];
    USHORT SequenceControl, Algorithm, Transaction, Status;
} IWL_IEEE80211_AUTH;

typedef struct _IWL_MCC_UPDATE_COMMAND
{
    USHORT Mcc;
    UCHAR SourceId, Reserved;
    ULONG Key;
    UCHAR Reserved2[20];
} IWL_MCC_UPDATE_COMMAND;

typedef struct _IWL_CONTEXT_INFO_DRAM
{
    ULONGLONG Umac[64];
    ULONGLONG Lmac[64];
    ULONGLONG Paging[64];
} IWL_CONTEXT_INFO_DRAM;

typedef struct _IWL_PRPH_SCRATCH
{
    USHORT MacId, Version, Size, VersionReserved;
    ULONG ControlFlags, ControlReserved;
    ULONGLONG PnvmBase;
    ULONG PnvmSize, PnvmReserved;
    ULONGLONG HwmBase;
    ULONG HwmSize, DebugToken;
    ULONGLONG FreeRbdAddress;
    ULONG RbdReserved;
    ULONGLONG ReducePowerBase;
    ULONG ReducePowerSize, ReducePowerReserved;
    ULONG StepMailbox0, StepMailbox1;
    ULONG FseqOverride, StepAnalogParams, Reserved[8];
    IWL_CONTEXT_INFO_DRAM Dram;
} IWL_PRPH_SCRATCH;

typedef struct _IWL_CONTEXT_INFO_GEN3
{
    USHORT Version, Size;
    ULONG Config;
    ULONGLONG PrphInfoBase;
    ULONGLONG CrHeadIndexBase;
    ULONGLONG TrTailIndexBase;
    ULONGLONG CrTailIndexBase;
    ULONGLONG TrHeadIndexBase;
    USHORT CrIndexArraySize, TrIndexArraySize;
    ULONGLONG MtrBase, McrBase;
    USHORT MtrSize, McrSize;
    USHORT MtrDoorbellVector, McrDoorbellVector;
    USHORT MtrMsiVector, McrMsiVector;
    UCHAR MtrOptionalHeaderSize, MtrOptionalFooterSize;
    UCHAR McrOptionalHeaderSize, McrOptionalFooterSize;
    USHORT MessageRingControlFlags, PrphInfoMsiVector;
    ULONGLONG PrphScratchBase;
    ULONG PrphScratchSize, Reserved;
} IWL_CONTEXT_INFO_GEN3;
#pragma pack(pop)

#define IWL_PRPH_SCRATCH_MTR_MODE       (1UL << 17)
#define IWL_PRPH_MTR_FORMAT_256B        0x000c0000
#define IWL_GEN3_INFO_PAGE_SIZE         4096
#define IWL_FIRST_TB_STRIDE              64
#define IWL_COMMAND_DATA_STRIDE          4096

C_ASSERT(sizeof(IWL_SCAN_REQ_UMAC_V17) == 1940);
C_ASSERT(sizeof(IWL_MAC_CONFIG_V2) == 52);
C_ASSERT(sizeof(IWL_LINK_CONFIG_V2) == 208);
C_ASSERT(sizeof(IWL_PHY_CONTEXT_V4) == 32);
C_ASSERT(sizeof(IWL_RLC_CONFIG_V2) == 32);
C_ASSERT(sizeof(IWL_SCD_QUEUE_CONFIG_V3) == 36);
C_ASSERT(sizeof(IWL_TX_COMMAND_GEN3) == 28);
C_ASSERT(sizeof(IWL_TLC_CONFIG_V4) == 28);
C_ASSERT(sizeof(IWL_SESSION_PROTECTION_V2) == 24);
C_ASSERT(sizeof(IWL_TX_QUEUE_CONFIG_RESPONSE) == 8);
C_ASSERT(sizeof(IWL_IEEE80211_AUTH) == 30);
C_ASSERT(sizeof(IWL_STA_CONFIG_V1) == 96);
C_ASSERT(sizeof(IWL_MCC_UPDATE_COMMAND) == 28);

VOID
IwlRecycleRxBuffer(_In_ PIWL_ADAPTER Adapter, _In_ USHORT Rbid)
{
    IWL_RX_TRANSFER_DESC *Transfer =
        (IWL_RX_TRANSFER_DESC *)Adapter->RxTransferRing.VirtualAddress;
    ULONG Slot, Actual;

    if (Rbid == 0 || Rbid > Adapter->RxBufferCount)
        return;
    Slot = Adapter->RxTransferWriteIndex;
    Transfer[Slot].Rbid = Rbid;
    Transfer[Slot].Address =
        Adapter->RxBuffers[Rbid - 1].PhysicalAddress.QuadPart;
    Adapter->RxTransferWriteIndex =
        (Slot + 1) & (IWL_GEN3_RX_QUEUE_SIZE - 1);
    Actual = Adapter->RxTransferWriteIndex & ~7UL;
    if (Actual != Adapter->RxTransferWriteActual)
    {
        KeMemoryBarrier();
        IwlWrite32(Adapter, RFH_Q0_FRBDCB_WIDX_TRG, Actual);
        Adapter->RxTransferWriteActual = Actual;
    }
}

static IWL_RX_PACKET *
IwlSendCommand(
    _In_ PIWL_ADAPTER Adapter,
    _In_ UCHAR Group,
    _In_ UCHAR Opcode,
    _In_ UCHAR Version,
    _In_reads_bytes_opt_(PayloadLength) const VOID *Payload,
    _In_ USHORT PayloadLength,
    _Out_opt_ PUSHORT RxBefore)
{
    ULONG Slot, i, Total, FirstLength;
    PUCHAR First, Data;
    IWL_TFH_TFD *Tfd;
    USHORT Before, After;
    IWL_RX_COMPLETION_DESC *Completion;
    IWL_RX_PACKET *MatchedResponse = NULL;

    Total = 8 + PayloadLength;
    if (Total > IWL_COMMAND_DATA_STRIDE)
        return NULL;
    Slot = Adapter->CommandWriteIndex & (IWL_GEN3_CMD_QUEUE_SIZE - 1);
    First = (PUCHAR)Adapter->CommandFirstTb.VirtualAddress +
            Slot * IWL_FIRST_TB_STRIDE;
    Data = (PUCHAR)Adapter->CommandData.VirtualAddress +
           Slot * IWL_COMMAND_DATA_STRIDE;
    Tfd = &((IWL_TFH_TFD *)Adapter->CommandRing.VirtualAddress)[Slot];
    Before = *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
    if (RxBefore != NULL)
        *RxBefore = Before;
    NdisZeroMemory(First, IWL_FIRST_TB_STRIDE);
    NdisZeroMemory(Data, IWL_COMMAND_DATA_STRIDE);
    NdisZeroMemory(Tfd, sizeof(*Tfd));
    Data[0] = Opcode;
    Data[1] = Group;
    *(UNALIGNED USHORT *)(Data + 2) = (USHORT)Slot;
    *(UNALIGNED USHORT *)(Data + 4) = PayloadLength;
    /* The TLV command version selects the payload ABI.  It is not the
     * wide-header version: upstream WIDE_ID(group, opcode) leaves the
     * header version at zero unless the command ID was explicitly built
     * with iwl_cmd_id(..., version).  In particular SCAN_REQ_UMAC has a
     * v17 payload and a zero wire-header version. */
    Data[7] = 0;
    if (PayloadLength != 0)
        NdisMoveMemory(Data + 8, Payload, PayloadLength);
    FirstLength = min(Total, 20UL);
    NdisMoveMemory(First, Data, FirstLength);
    Tfd->NumberOfTbs = Total > FirstLength ? 2 : 1;
    Tfd->Tb[0].Length = (USHORT)FirstLength;
    Tfd->Tb[0].Address = Adapter->CommandFirstTb.PhysicalAddress.QuadPart +
                         Slot * IWL_FIRST_TB_STRIDE;
    if (Total > FirstLength)
    {
        Tfd->Tb[1].Length = (USHORT)(Total - FirstLength);
        Tfd->Tb[1].Address = Adapter->CommandData.PhysicalAddress.QuadPart +
                            Slot * IWL_COMMAND_DATA_STRIDE + FirstLength;
    }
    KeMemoryBarrier();
    Adapter->CommandWriteIndex = (Slot + 1) &
                                 (IWL_GEN3_CMD_QUEUE_SIZE - 1);
    IwlWrite32(Adapter, HBUS_TARG_WRPTR, Adapter->CommandWriteIndex);
    for (i = 0; i < 500; i++)
    {
        if (*(volatile USHORT *)Adapter->RxStatus.VirtualAddress != Before)
            break;
        if (KeGetCurrentIrql() == PASSIVE_LEVEL)
            NdisMSleep(1000);
        else
            NdisStallExecution(10);
    }
    After = *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
    Completion = (IWL_RX_COMPLETION_DESC *)
        Adapter->RxCompletionRing.VirtualAddress;
    for (i = Before; i < After; i++)
    {
        USHORT Rbid = Completion[i & (IWL_GEN3_RX_QUEUE_SIZE - 1)].Rbid;
        IWL_RX_PACKET *Packet;
        ULONG PacketLength;
        BOOLEAN Matched;
        if (Rbid == 0 || Rbid > Adapter->RxBufferCount)
            continue;
        Packet = (IWL_RX_PACKET *)Adapter->RxBuffers[Rbid - 1].VirtualAddress;
        PacketLength = Packet->LengthAndFlags & 0x3fff;
        Matched = Packet->Command == Opcode && Packet->Group == Group &&
                  Packet->Sequence == (USHORT)Slot;
        if (Matched && PacketLength + sizeof(ULONG) <=
                           sizeof(Adapter->CommandResponse))
        {
            NdisMoveMemory(Adapter->CommandResponse, Packet,
                           PacketLength + sizeof(ULONG));
            MatchedResponse = (IWL_RX_PACKET *)Adapter->CommandResponse;
        }
        IwlRecycleRxBuffer(Adapter, Rbid);
    }
    if (InterlockedCompareExchange(&Adapter->DataPathReady, 0, 0))
        Adapter->RxReadIndex = After;
    if (MatchedResponse != NULL)
    {
        return MatchedResponse;
    }
    DPRINT1("iwlwifi: command %02x.%02x v%u timed out/mismatched "
            "(RX %u -> %u)\n", Group, Opcode, Version, Before, After);
    return NULL;
}

NDIS_STATUS
IwlInstallKey(_In_ PIWL_ADAPTER Adapter, _In_ ULONG KeyId,
              _In_ DOT11_CIPHER_ALGORITHM Algorithm,
              _In_reads_bytes_(KeyLength) const UCHAR *Key,
              _In_ ULONG KeyLength, _In_ BOOLEAN Multicast)
{
    IWL_SEC_KEY_CMD_V1 Command;
    UCHAR Version;

    if (KeyLength == 0 || KeyLength > sizeof(Command.Key) || KeyId > 7)
        return NDIS_STATUS_INVALID_LENGTH;
    NdisZeroMemory(&Command, sizeof(Command));
    Command.Action = 1; /* FW_CTXT_ACTION_ADD */
    Command.StationMask = 1; /* AP station ID zero */
    Command.KeyId = KeyId;
    if (Algorithm == DOT11_CIPHER_ALGO_CCMP)
        Command.KeyFlags = 0x02;
    else if (Algorithm == DOT11_CIPHER_ALGO_TKIP)
        Command.KeyFlags = 0x03;
    else
        return NDIS_STATUS_NOT_SUPPORTED;
    if (Multicast)
        Command.KeyFlags |= 0x40;
    NdisMoveMemory(Command.Key, Key, KeyLength);
    if (Algorithm == DOT11_CIPHER_ALGO_TKIP && KeyLength == 32)
    {
        /* mac80211/RSNA TKIP layout is TK[16], TX-MIC[8], RX-MIC[8].
         * SEC_KEY_CMD retains the complete key and also requires the two MIC
         * subkeys in their dedicated firmware fields. */
        NdisMoveMemory(Command.TkipMicTxKey, Key + 16, 8);
        NdisMoveMemory(Command.TkipMicRxKey, Key + 24, 8);
    }
    Version = IwlFwLookupCommandVersion(Adapter->FwParsed, 0x05, 0x18, 0);
    if (Version != 1)
        return NDIS_STATUS_NOT_SUPPORTED;
    if (IwlSendCommand(Adapter, 0x05, 0x18, Version, &Command,
                       sizeof(Command), NULL) == NULL)
        return NDIS_STATUS_FAILURE;
    DPRINT1("iwlwifi: installed %s key id %lu cipher %lu\n",
            Multicast ? "group" : "pairwise", KeyId, Algorithm);
    return NDIS_STATUS_SUCCESS;
}

static IWL_RX_PACKET *
IwlSendSmallCommand(
    _In_ PIWL_ADAPTER Adapter,
    _In_ UCHAR Group,
    _In_ UCHAR Opcode,
    _In_ UCHAR Version,
    _In_reads_bytes_opt_(PayloadLength) const VOID *Payload,
    _In_ USHORT PayloadLength)
{
    if (PayloadLength > IWL_FIRST_TB_STRIDE - 8)
        return NULL;
    return IwlSendCommand(Adapter, Group, Opcode, Version, Payload,
                          PayloadLength, NULL);
}

static VOID
IwlRememberScanMpdu(
    _In_ PIWL_ADAPTER Adapter,
    _In_ IWL_RX_PACKET *Packet,
    _In_ UCHAR RxMpduVersion)
{
    ULONG PacketLength = Packet->LengthAndFlags & 0x3fff;
    /* RX_MPDU notification v4 uses the compact v1 tail (48 bytes total).
     * AX210 v3/v5 spans DW2 through DW17: exactly 16 dwords, or 64 bytes. */
    ULONG DescriptorLength = RxMpduVersion == 4 ? 48 : 64;
    PUCHAR Descriptor = Packet->Data;
    USHORT MpduLength, FrameControl;
    PUCHAR Frame, Ie, End;
    PIWL_BSS Bss = NULL;
    ULONG i, IeLength;
    UCHAR Channel;
    CHAR EnergyA, EnergyB;

    if (PacketLength < 4 + DescriptorLength + 24)
        return;
    MpduLength = *(UNALIGNED USHORT *)Descriptor;
    if (MpduLength < 36 || 4 + DescriptorLength + MpduLength > PacketLength)
        return;
    Frame = Descriptor + DescriptorLength;
    FrameControl = *(UNALIGNED USHORT *)Frame;
    if ((FrameControl & 0x00fc) != 0x0080 &&
        (FrameControl & 0x00fc) != 0x0050)
        return;
    for (i = 0; i < Adapter->BssCount; i++)
    {
        if (RtlCompareMemory(Adapter->Bss[i].Bssid, Frame + 16,
                             sizeof(DOT11_MAC_ADDRESS)) ==
            sizeof(DOT11_MAC_ADDRESS))
        {
            Bss = &Adapter->Bss[i];
            break;
        }
    }
    if (Bss == NULL)
    {
        if (Adapter->BssCount == IWL_MAX_BSS)
            return;
        Bss = &Adapter->Bss[Adapter->BssCount++];
        NdisZeroMemory(Bss, sizeof(*Bss));
        NdisMoveMemory(Bss->Bssid, Frame + 16, sizeof(Bss->Bssid));
    }
    Channel = RxMpduVersion == 4 ? 0 : Descriptor[42];
    Bss->ChannelFrequency = Channel == 14 ? 2484 :
        (Channel <= 14 ? 2407 + Channel * 5 : 5000 + Channel * 5);
    EnergyA = (CHAR)Descriptor[40];
    EnergyB = (CHAR)Descriptor[41];
    Bss->Rssi = -(LONG)min((UCHAR)EnergyA, (UCHAR)EnergyB);
    Bss->BeaconPeriod = *(UNALIGNED USHORT *)(Frame + 32);
    Bss->CapabilityInformation = *(UNALIGNED USHORT *)(Frame + 34);
    IeLength = min(MpduLength - 36, (ULONG)IWL_MAX_BSS_IE_SIZE);
    Bss->IeLength = IeLength;
    NdisMoveMemory(Bss->Ies, Frame + 36, IeLength);
    Ie = Frame + 36;
    End = Frame + MpduLength;
    while (Ie + 2 <= End && Ie + 2 + Ie[1] <= End)
    {
        if (Ie[0] == 0 && Ie[1] <= 32)
        {
            CHAR Ssid[33];
            NdisZeroMemory(Ssid, sizeof(Ssid));
            NdisMoveMemory(Ssid, Ie + 2, Ie[1]);
            DPRINT1("iwlwifi: scanned SSID '%s' BSSID "
                    "%02x:%02x:%02x:%02x:%02x:%02x\n", Ssid,
                    Frame[16], Frame[17], Frame[18], Frame[19],
                    Frame[20], Frame[21]);
            return;
        }
        Ie += 2 + Ie[1];
    }
}

static NDIS_STATUS
IwlRunPassiveScan(_In_ PIWL_ADAPTER Adapter)
{
    static const UCHAR ChannelNumber[51] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,
        100,104,108,112,116,120,124,128,132,136,140,144,
        149,153,157,161,165,169,173,177,181
    };
    IWL_SCAN_REQ_UMAC_V17 Request;
    IWL_RX_PACKET *Response;
    IWL_RX_COMPLETION_DESC *Completion;
    UCHAR Version, RxMpduVersion;
    USHORT Before, After, Cursor;
    ULONG i, Count = 0;

    Adapter->BssCount = 0;
    NdisZeroMemory(&Request, sizeof(Request));
    /* Firmware scan UID zero is the first free regular-scan slot. */
    Request.Uid = 0;
    Request.OutOfChannelPriority = 6;
    Request.General.Flags = (1U << 1) | (1U << 7) | (1U << 11);
    Request.General.ActiveDwell[0] = Request.General.ActiveDwell[1] = 10;
    Request.General.AdaptiveDwell2G = 2;
    Request.General.AdaptiveDwell5G = 8;
    Request.General.AdaptiveDwellSocial = 10;
    Request.General.AdaptiveDwellMaxBudget = 300;
    Request.General.ScanPriority = 6;
    Request.General.PassiveDwell[0] = Request.General.PassiveDwell[1] = 110;
    Request.Periodic.Schedule[0].IterationCount = 1;
    Request.Channels.Flags = 1U << 5; /* preserve channel order */
    Request.Channels.NumberOfApsOverride[0] = 10;
    Request.Channels.NumberOfApsOverride[1] = 2;
    for (i = 0; i < RTL_NUMBER_OF(ChannelNumber) &&
                i < Adapter->NvmChannelCount; i++)
    {
        if (!(Adapter->NvmChannelFlags[i] & 1))
            continue;
        /* Firmware PHY bands are encoded as 1 for 2.4 GHz and 0 for
         * 5 GHz (not the nl80211 band numbering). */
        Request.Channels.Channel[Count].Flags = (i < 14 ? 1UL : 0UL) << 30;
        Request.Channels.Channel[Count].ChannelNumber = ChannelNumber[i];
        Request.Channels.Channel[Count].IterationCount = 1;
        Count++;
    }
    Request.Channels.Count = (UCHAR)Count;
    Version = IwlFwLookupCommandVersion(Adapter->FwParsed, 0x01, 0x0d, 17);
    RxMpduVersion = IwlFwLookupNotificationVersion(Adapter->FwParsed,
                                                    0x00, 0xc1, 4);
    DPRINT1("iwlwifi: scan radio state GP_CNTRL=0x%08x (%s)\n",
            IwlRead32(Adapter, CSR_GP_CNTRL),
            (IwlRead32(Adapter, CSR_GP_CNTRL) &
             CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW) ? "enabled" : "RF-killed");
    Response = IwlSendCommand(Adapter, 0x01, 0x0d, Version, &Request,
                              sizeof(Request), &Before);
    DPRINT1("iwlwifi: SCAN_REQ_UMAC v%u, %lu passive channels: %s; "
            "RX_MPDU v%u\n", Version, Count,
            Response != NULL ? "accepted" : "awaiting notifications",
            RxMpduVersion);
    Completion = (IWL_RX_COMPLETION_DESC *)Adapter->RxCompletionRing.VirtualAddress;
    Cursor = Before;
    for (i = 0; i < 1500; i++)
    {
        After = *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
        while (Cursor != After)
        {
            USHORT Rbid = Completion[Cursor &
                (IWL_GEN3_RX_QUEUE_SIZE - 1)].Rbid;
            if (Rbid != 0 && Rbid <= Adapter->RxBufferCount)
            {
                IWL_RX_PACKET *Packet = (IWL_RX_PACKET *)
                    Adapter->RxBuffers[Rbid - 1].VirtualAddress;
                if (Packet->Command == 0xc1 && Packet->Group == 0)
                    IwlRememberScanMpdu(Adapter, Packet, RxMpduVersion);
                /* Legacy notifications use group zero even though the scan
                 * request itself is in the long-command scan group. */
                else if (Packet->Command == 0x0f && Packet->Group == 0)
                {
                    DPRINT1("iwlwifi: passive scan complete, RX closed=%u\n",
                            After);
                    IwlRecycleRxBuffer(Adapter, Rbid);
                    return NDIS_STATUS_SUCCESS;
                }
                else
                {
                    DPRINT1("iwlwifi: scan RX cmd=0x%02x group=0x%02x "
                            "len=%lu seq=0x%04x\n", Packet->Command,
                            Packet->Group,
                            Packet->LengthAndFlags & 0x3fff,
                            Packet->Sequence);
                }
                IwlRecycleRxBuffer(Adapter, Rbid);
            }
            Cursor++;
        }
        NdisMSleep(10000);
    }
    DPRINT1("iwlwifi: passive scan timed out, RX closed=%u\n",
            *(volatile USHORT *)Adapter->RxStatus.VirtualAddress);
    return NDIS_STATUS_FAILURE;
}

static VOID NTAPI
IwlScanWorker(_In_ PVOID Context, _In_ NDIS_HANDLE WorkItem)
{
    PIWL_ADAPTER Adapter = Context;
    NDIS_STATUS Status;
    NDIS_STATUS_INDICATION Indication;

    UNREFERENCED_PARAMETER(WorkItem);
    Status = IwlRunPassiveScan(Adapter);
    if (!(Adapter->Flags & IWL_FLAG_HALTING))
    {
        NdisZeroMemory(&Indication, sizeof(Indication));
        Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
        Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
        Indication.Header.Size = sizeof(Indication);
        Indication.SourceHandle = Adapter->MiniportAdapterHandle;
        Indication.StatusCode = NDIS_STATUS_DOT11_SCAN_CONFIRM;
        Indication.StatusBuffer = &Status;
        Indication.StatusBufferSize = sizeof(Status);
        DPRINT1("iwlwifi: real scan complete: %lu BSSes status 0x%08x\n",
                Adapter->BssCount, Status);
        NdisMIndicateStatusEx(Adapter->MiniportAdapterHandle, &Indication);
    }
    InterlockedExchange(&Adapter->ScanQueued, 0);
    KeSetEvent(&Adapter->ScanIdleEvent, IO_NO_INCREMENT, FALSE);
}

static BOOLEAN
IwlBssHasSsid(_In_ const IWL_BSS *Bss, _In_ const DOT11_SSID *Ssid)
{
    const UCHAR *Ie = Bss->Ies;
    const UCHAR *End = Bss->Ies + Bss->IeLength;

    while (Ie + 2 <= End && Ie + 2 + Ie[1] <= End)
    {
        if (Ie[0] == 0 && Ie[1] == Ssid->uSSIDLength &&
            RtlCompareMemory(Ie + 2, Ssid->ucSSID, Ie[1]) == Ie[1])
            return TRUE;
        Ie += 2 + Ie[1];
    }
    return FALSE;
}

static PIWL_BSS
IwlSelectAssociationBss(_In_ PIWL_ADAPTER Adapter)
{
    PIWL_BSS Best = NULL;
    ULONG Index;

    for (Index = 0; Index < Adapter->BssCount; Index++)
    {
        PIWL_BSS Candidate = &Adapter->Bss[Index];
        if (!IwlBssHasSsid(Candidate, &Adapter->DesiredSsid))
            continue;
        if (Adapter->DesiredBssidValid &&
            RtlCompareMemory(Candidate->Bssid, Adapter->DesiredBssid,
                             sizeof(DOT11_MAC_ADDRESS)) !=
                sizeof(DOT11_MAC_ADDRESS))
            continue;
        if (Best == NULL || Candidate->Rssi > Best->Rssi)
            Best = Candidate;
    }
    return Best;
}

static BOOLEAN
IwlPrepareAssociationContext(
    _In_ PIWL_ADAPTER Adapter,
    _In_ const IWL_BSS *Bss)
{
    IWL_PHY_CONTEXT_V4 Phy;
    IWL_RLC_CONFIG_V2 Rlc;
    IWL_LINK_CONFIG_V2 Link;
    IWL_STA_CONFIG_V1 Station;
    IWL_RX_PACKET *Response;
    ULONG Frequency = Bss->ChannelFrequency;
    ULONG Channel;
    UCHAR PhyVersion, RlcVersion, LinkVersion;
    ULONG RxChains = Adapter->ValidRxAntennas;
    ULONG ChainCount = ((RxChains & 1) != 0) + ((RxChains & 2) != 0);
    const UCHAR *Ie, *End;

    if (Frequency == 2484)
        Channel = 14;
    else if (Frequency >= 2412 && Frequency <= 2472)
        Channel = (Frequency - 2407) / 5;
    else if (Frequency >= 5000 && Frequency <= 5900)
        Channel = (Frequency - 5000) / 5;
    else
    {
        DPRINT1("iwlwifi: unsupported association frequency %lu MHz\n",
                Frequency);
        return FALSE;
    }

    PhyVersion = IwlFwLookupCommandVersion(Adapter->FwParsed,
                                            0x01, 0x08, 1);
    RlcVersion = IwlFwLookupCommandVersion(Adapter->FwParsed,
                                            0x05, 0x08, 0);
    LinkVersion = IwlFwLookupCommandVersion(Adapter->FwParsed,
                                             0x03, 0x09, 1);
    if (PhyVersion != 4 || RlcVersion != 2 || LinkVersion != 2)
    {
        DPRINT1("iwlwifi: association ABI mismatch PHY=%u RLC=%u LINK=%u\n",
                PhyVersion, RlcVersion, LinkVersion);
        return FALSE;
    }

    NdisZeroMemory(&Phy, sizeof(Phy));
    Phy.Action = 1; /* FW_CTXT_ACTION_ADD */
    Phy.ChannelInfo.Channel = Channel;
    Phy.ChannelInfo.Band = Frequency < 3000 ? 1 : 0; /* PHY_BAND_24/5 */
    Phy.ChannelInfo.Width = 0; /* IWL_PHY_CHANNEL_MODE20 */
    Phy.LmacId = Frequency < 3000 ? 0 : 1;
    Response = IwlSendSmallCommand(Adapter, 0x01, 0x08, PhyVersion,
                                   &Phy, sizeof(Phy));
    DPRINT1("iwlwifi: PHY_CONTEXT v%u add channel %lu (%lu MHz): %s\n",
            PhyVersion, Channel, Frequency,
            Response != NULL ? "accepted" : "failed");
    if (Response == NULL)
        return FALSE;

    NdisZeroMemory(&Rlc, sizeof(Rlc));
    Rlc.RxChainInfo = (RxChains << 1) | (ChainCount << 10) |
                      (ChainCount << 12);
    Response = IwlSendSmallCommand(Adapter, 0x05, 0x08, RlcVersion,
                                   &Rlc, sizeof(Rlc));
    DPRINT1("iwlwifi: RLC_CONFIG v%u: %s\n", RlcVersion,
            Response != NULL ? "accepted" : "failed");
    if (Response == NULL)
        return FALSE;

    NdisZeroMemory(&Link, sizeof(Link));
    Link.Action = 2; /* FW_CTXT_ACTION_MODIFY */
    Link.LinkId = 0;
    Link.MacId = 0;
    Link.PhyId = 0;
    NdisMoveMemory(Link.LocalLinkAddress, Adapter->CurrentMacAddress, 6);
    /* Firmware validates the full link object even when modify_mask selects
     * only a subset.  Match mac80211's default station EDCA object and beacon
     * timing rather than leaving non-selected fields structurally invalid. */
    Link.QosFlags = 2;
    Link.Ac[0].CwMin = Link.Ac[1].CwMin =
        Link.Ac[2].CwMin = Link.Ac[3].CwMin = 15;
    Link.Ac[0].CwMax = Link.Ac[1].CwMax =
        Link.Ac[2].CwMax = Link.Ac[3].CwMax = 1023;
    Link.Ac[0].Aifsn = Link.Ac[1].Aifsn =
        Link.Ac[2].Aifsn = Link.Ac[3].Aifsn = 2;
    Link.Ac[0].FifosMask = 2;
    Link.Ac[1].FifosMask = 4;
    Link.Ac[2].FifosMask = 8;
    Link.Ac[3].FifosMask = 16;
    Link.BeaconInterval = Bss->BeaconPeriod;
    /* Firmware only permits assigning a PHY while the link remains inactive.
     * Linux therefore sends this zero-mask modification before activation. */
    Response = IwlSendCommand(Adapter, 0x03, 0x09, LinkVersion,
                              &Link, sizeof(Link), NULL);
    DPRINT1("iwlwifi: LINK_CONFIG v%u assign PHY while inactive: %s\n",
            LinkVersion,
            Response != NULL ? "accepted" : "failed");
    if (Response == NULL)
        return FALSE;

    /* Convert the AP's Basic Rate Set to the two firmware ACK-rate masks.
     * Rate codes are in 500-kbit/s units with bit 7 marking a basic rate. */
    Ie = Bss->Ies;
    End = Bss->Ies + Bss->IeLength;
    while (Ie + 2 <= End && Ie + 2 + Ie[1] <= End)
    {
        ULONG Index;
        if (Ie[0] != 1 && Ie[0] != 50)
        {
            Ie += 2 + Ie[1];
            continue;
        }
        for (Index = 0; Index < Ie[1]; Index++)
        {
            UCHAR Rate = Ie[2 + Index];
            if (!(Rate & 0x80))
                continue;
            switch (Rate & 0x7f)
            {
                case 2:  Link.CckRates |= 1U << 0; break;
                case 4:  Link.CckRates |= 1U << 1; break;
                case 11: Link.CckRates |= 1U << 2; break;
                case 22: Link.CckRates |= 1U << 3; break;
                case 12: Link.OfdmRates |= 1U << 0; break;
                case 18: Link.OfdmRates |= 1U << 1; break;
                case 24: Link.OfdmRates |= 1U << 2; break;
                case 36: Link.OfdmRates |= 1U << 3; break;
                case 48: Link.OfdmRates |= 1U << 4; break;
                case 72: Link.OfdmRates |= 1U << 5; break;
                case 96: Link.OfdmRates |= 1U << 6; break;
                case 108: Link.OfdmRates |= 1U << 7; break;
            }
        }
        Ie += 2 + Ie[1];
    }
    /* Mandatory fallback rates used by mac80211's iwl_mvm_ack_rates(). */
    Link.OfdmRates |= 1U << 0; /* 6 Mbit/s */
    Link.CckRates |= 1U << 0;  /* 1 Mbit/s */
    Link.ModifyMask = 3; /* ACTIVE | RATES_INFO */
    Link.Active = 1;
    Response = IwlSendCommand(Adapter, 0x03, 0x09, LinkVersion,
                              &Link, sizeof(Link), NULL);
    DPRINT1("iwlwifi: LINK_CONFIG v%u activate rates cck=%lx ofdm=%lx: %s\n",
            LinkVersion, Link.CckRates, Link.OfdmRates,
            Response != NULL ? "accepted" : "failed");
    if (Response == NULL)
        return FALSE;

    /* Add the AP as peer station zero before management TX.  AssociationId
     * intentionally remains zero until an association response assigns it. */
    NdisZeroMemory(&Station, sizeof(Station));
    Station.StaId = 0;
    Station.LinkId = 0;
    NdisMoveMemory(Station.PeerMldAddress, Bss->Bssid, 6);
    NdisMoveMemory(Station.PeerLinkAddress, Bss->Bssid, 6);
    Station.Mfp = 1;
    Station.Mimo = 1;
    Response = IwlSendCommand(
        Adapter, 0x03, 0x0a,
        IwlFwLookupCommandVersion(Adapter->FwParsed, 0x03, 0x0a, 1),
        &Station, sizeof(Station), NULL);
    DPRINT1("iwlwifi: STA_CONFIG v1 peer zero %s\n",
            Response != NULL ? "accepted" : "failed");
    return Response != NULL;
}

static VOID
IwlIndicateAssociation(
    _In_ PIWL_ADAPTER Adapter,
    _In_ const IWL_BSS *Bss,
    _In_ BOOLEAN Start,
    _In_ DOT11_ASSOC_STATUS Status)
{
    NDIS_STATUS_INDICATION Indication;
    DOT11_ASSOCIATION_START_PARAMETERS StartParameters;
    UCHAR CompletionBuffer[sizeof(DOT11_ASSOCIATION_COMPLETION_PARAMETERS) +
                           IWL_MAX_BSS_IE_SIZE];
    PDOT11_ASSOCIATION_COMPLETION_PARAMETERS CompletionParameters;

    NdisZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size = sizeof(Indication);
    Indication.SourceHandle = Adapter->MiniportAdapterHandle;
    if (Start)
    {
        NdisZeroMemory(&StartParameters, sizeof(StartParameters));
        StartParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        StartParameters.Header.Revision =
            DOT11_ASSOCIATION_START_PARAMETERS_REVISION_1;
        StartParameters.Header.Size = sizeof(StartParameters);
        NdisMoveMemory(StartParameters.MacAddr, Bss->Bssid, 6);
        StartParameters.SSID = Adapter->DesiredSsid;
        Indication.StatusCode = NDIS_STATUS_DOT11_ASSOCIATION_START;
        Indication.StatusBuffer = &StartParameters;
        Indication.StatusBufferSize = sizeof(StartParameters);
        NdisMIndicateStatusEx(Adapter->MiniportAdapterHandle, &Indication);
        return;
    }

    NdisZeroMemory(CompletionBuffer, sizeof(CompletionBuffer));
    CompletionParameters =
        (PDOT11_ASSOCIATION_COMPLETION_PARAMETERS)CompletionBuffer;
    CompletionParameters->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    CompletionParameters->Header.Revision =
        DOT11_ASSOCIATION_COMPLETION_PARAMETERS_REVISION_1;
    CompletionParameters->Header.Size = sizeof(*CompletionParameters);
    NdisMoveMemory(CompletionParameters->MacAddr, Bss->Bssid, 6);
    CompletionParameters->uStatus = Status;
    CompletionParameters->uIHVDataOffset = sizeof(*CompletionParameters);
    CompletionParameters->uIHVDataSize = Bss->IeLength;
    NdisMoveMemory(CompletionBuffer + sizeof(*CompletionParameters), Bss->Ies,
                   Bss->IeLength);
    Indication.StatusCode = NDIS_STATUS_DOT11_ASSOCIATION_COMPLETION;
    Indication.StatusBuffer = CompletionBuffer;
    Indication.StatusBufferSize = sizeof(*CompletionParameters) + Bss->IeLength;
    NdisMIndicateStatusEx(Adapter->MiniportAdapterHandle, &Indication);
}

static BOOLEAN
IwlConfigureManagementTx(_In_ PIWL_ADAPTER Adapter, _In_ PIWL_BSS Bss)
{
    IWL_TLC_CONFIG_V4 Tlc;
    IWL_SESSION_PROTECTION_V2 Protection;
    IWL_RX_PACKET *Response;
    const UCHAR *Ie = Bss->Ies, *End = Bss->Ies + Bss->IeLength;
    UCHAR TlcVersion, ProtectionVersion;

    TlcVersion = IwlFwLookupCommandVersion(Adapter->FwParsed,
                                            0x05, 0x0f, 0);
    ProtectionVersion = IwlFwLookupCommandVersion(Adapter->FwParsed,
                                                   0x03, 0x05, 1);
    if (TlcVersion != 4 || ProtectionVersion != 2)
    {
        DPRINT1("iwlwifi: TX prerequisite ABI mismatch TLC=%u "
                "SESSION_PROTECTION=%u\n", TlcVersion, ProtectionVersion);
        return FALSE;
    }

    NdisZeroMemory(&Tlc, sizeof(Tlc));
    Tlc.StationId = 0;
    Tlc.MaxChannelWidth = 0; /* authentication starts at 20 MHz */
    Tlc.Mode = 0; /* non-HT until capabilities are negotiated */
    Tlc.Chains = (UCHAR)(Adapter->ValidTxAntennas & 3);
    while (Ie + 2 <= End && Ie + 2 + Ie[1] <= End)
    {
        ULONG Index;
        if (Ie[0] == 1 || Ie[0] == 50)
        {
            for (Index = 0; Index < Ie[1]; Index++)
            {
                switch (Ie[2 + Index] & 0x7f)
                {
                    case 2: Tlc.NonHtRates |= 1U << 0; break;
                    case 4: Tlc.NonHtRates |= 1U << 1; break;
                    case 11: Tlc.NonHtRates |= 1U << 2; break;
                    case 22: Tlc.NonHtRates |= 1U << 3; break;
                    case 12: Tlc.NonHtRates |= 1U << 4; break;
                    case 18: Tlc.NonHtRates |= 1U << 5; break;
                    case 24: Tlc.NonHtRates |= 1U << 6; break;
                    case 36: Tlc.NonHtRates |= 1U << 7; break;
                    case 48: Tlc.NonHtRates |= 1U << 8; break;
                    case 72: Tlc.NonHtRates |= 1U << 9; break;
                    case 96: Tlc.NonHtRates |= 1U << 10; break;
                    case 108: Tlc.NonHtRates |= 1U << 11; break;
                }
            }
        }
        Ie += 2 + Ie[1];
    }
    if (Tlc.Chains == 0 || Tlc.NonHtRates == 0)
    {
        DPRINT1("iwlwifi: invalid TLC chains=%u legacy-rates=%x\n",
                Tlc.Chains, Tlc.NonHtRates);
        return FALSE;
    }
    Response = IwlSendCommand(Adapter, 0x05, 0x0f, TlcVersion,
                              &Tlc, sizeof(Tlc), NULL);
    DPRINT1("iwlwifi: TLC_CONFIG v4 chains=%x rates=%x: %s\n",
            Tlc.Chains, Tlc.NonHtRates,
            Response != NULL ? "accepted" : "failed");
    if (Response == NULL)
        return FALSE;

    NdisZeroMemory(&Protection, sizeof(Protection));
    Protection.LinkId = 0;
    Protection.Action = 1; /* FW_CTXT_ACTION_ADD */
    Protection.ConfigurationId = 0; /* SESSION_PROTECT_CONF_ASSOC */
    Protection.DurationTu = 900UL * 1000 / 1024;
    Response = IwlSendCommand(Adapter, 0x03, 0x05, ProtectionVersion,
                              &Protection, sizeof(Protection), NULL);
    DPRINT1("iwlwifi: SESSION_PROTECTION v2 duration=%lu TU: %s\n",
            Protection.DurationTu,
            Response != NULL ? "accepted" : "failed");
    return Response != NULL;
}

static BOOLEAN
IwlAllocateTxQueue(_In_ PIWL_ADAPTER Adapter, _In_ UCHAR Tid)
{
    IWL_SCD_QUEUE_CONFIG_V3 Command;
    IWL_TX_QUEUE_CONFIG_RESPONSE *QueueResponse;
    IWL_RX_PACKET *Response;

    if (Adapter->TxQueueValid)
        return TRUE;
    NdisZeroMemory(&Command, sizeof(Command));
    Command.Operation = 0; /* IWL_SCD_QUEUE_ADD */
    Command.StationMask = 1; /* station zero */
    Command.Tid = Tid;
    Command.CircularBufferSize = 4; /* log2(128) - 3 */
    Command.ByteCountAddress = Adapter->TxByteCount.PhysicalAddress.QuadPart;
    Command.TfdAddress = Adapter->TxQueueRing.PhysicalAddress.QuadPart;
    Response = IwlSendCommand(Adapter, 0x05, 0x17, 3, &Command,
                              sizeof(Command), NULL);
    if (Response == NULL ||
        (Response->LengthAndFlags & 0x3fff) <
            4 + sizeof(*QueueResponse))
    {
        DPRINT1("iwlwifi: SCD_QUEUE_CONFIG v3 failed\n");
        return FALSE;
    }
    QueueResponse = (IWL_TX_QUEUE_CONFIG_RESPONSE *)Response->Data;
    if (QueueResponse->QueueNumber >= 512)
    {
        DPRINT1("iwlwifi: firmware returned invalid TX queue %u\n",
                QueueResponse->QueueNumber);
        return FALSE;
    }
    Adapter->TxQueueId = QueueResponse->QueueNumber;
    Adapter->TxWriteIndex = QueueResponse->WritePointer &
                            (IWL_GEN3_TX_QUEUE_SIZE - 1);
    Adapter->TxQueueValid = TRUE;
    DPRINT1("iwlwifi: TX queue %u TID %u flags=%x starts at %u\n",
            Adapter->TxQueueId, Tid, QueueResponse->Flags,
            Adapter->TxWriteIndex);
    return TRUE;
}

static BOOLEAN
IwlSwitchToDataQueue(_In_ PIWL_ADAPTER Adapter)
{
    IWL_SCD_QUEUE_CONFIG_V3 Command;
    IWL_RX_PACKET *Response;

    if (!Adapter->TxQueueValid)
        return FALSE;
    NdisZeroMemory(&Command, sizeof(Command));
    Command.Operation = 1; /* IWL_SCD_QUEUE_REMOVE */
    Command.StationMask = 1;
    /* The remove union carries TID as a 32-bit value at this offset. */
    *(UNALIGNED ULONG *)&Command.Tid = 15; /* IWL_MGMT_TID */
    Response = IwlSendCommand(Adapter, 0x05, 0x17, 3, &Command,
                              sizeof(Command), NULL);
    if (Response == NULL)
    {
        DPRINT1("iwlwifi: failed to remove management TX queue\n");
        return FALSE;
    }
    Adapter->TxQueueValid = FALSE;
    /* New-TX-API IWL_TID_NON_QOS is zero.  EAPOL and ordinary non-QoS data
     * frames produced by nwifi both use this scheduler classification. */
    if (!IwlAllocateTxQueue(Adapter, 0))
        return FALSE;
    DPRINT1("iwlwifi: switched scheduler to non-QoS data TX\n");
    return TRUE;
}

BOOLEAN
IwlTransmitFrame(_In_ PIWL_ADAPTER Adapter,
                 _In_reads_bytes_(FrameLength) const UCHAR *Frame,
                 _In_ USHORT FrameLength,
                 _In_ BOOLEAN Encrypt,
                 _Out_opt_ PUSHORT RxBefore)
{
    ULONG Slot, HeaderRemainder, BodyLength;
    PUCHAR First, Data;
    IWL_TFH_TFD *Tfd;
    IWL_TX_COMMAND_GEN3 *Tx;
    volatile USHORT *ByteCount;
    ULONGLONG FirstPhysical, DataPhysical;

    if (!Adapter->TxQueueValid || FrameLength < 24 ||
        4 + sizeof(*Tx) + FrameLength > IWL_COMMAND_DATA_STRIDE)
        return FALSE;
    if (RxBefore != NULL)
        *RxBefore = *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
    NdisAcquireSpinLock(&Adapter->TxLock);
    Slot = Adapter->TxWriteIndex & (IWL_GEN3_TX_QUEUE_SIZE - 1);
    First = (PUCHAR)Adapter->TxQueueFirstTb.VirtualAddress +
            Slot * IWL_FIRST_TB_STRIDE;
    Data = (PUCHAR)Adapter->TxQueueData.VirtualAddress +
           Slot * IWL_COMMAND_DATA_STRIDE;
    Tfd = &((IWL_TFH_TFD *)Adapter->TxQueueRing.VirtualAddress)[Slot];
    NdisZeroMemory(First, IWL_FIRST_TB_STRIDE);
    NdisZeroMemory(Data, IWL_COMMAND_DATA_STRIDE);
    NdisZeroMemory(Tfd, sizeof(*Tfd));

    Data[0] = 0x1c; /* TX_CMD */
    Data[1] = 0; /* LEGACY_GROUP */
    *(UNALIGNED USHORT *)(Data + 2) =
        (USHORT)((Adapter->TxQueueId << 8) | Slot);
    Tx = (IWL_TX_COMMAND_GEN3 *)(Data + 4);
    Tx->Length = FrameLength;
    Tx->Flags = (Encrypt ? 0 : (1 << 1)) | (1 << 2);
    /* Header length is encoded in 16-bit words at bit 8.  Linux emits
     * 0x0c00 for every ordinary 24-byte management header. */
    Tx->OffloadAssist = (24 / 2) << 8;
    NdisMoveMemory(Tx + 1, Frame, FrameLength);

    NdisMoveMemory(First, Data, 20);
    /* Linux's gen2 transport makes TB2 contain the remainder of TX_CMD plus
     * only the 802.11 MAC header; the frame body is a separate TB3.  Firmware
     * does not treat an equivalent concatenation as interchangeable here. */
    HeaderRemainder = 4 + sizeof(*Tx) + 24 - 20;
    BodyLength = FrameLength - 24;
    FirstPhysical = Adapter->TxQueueFirstTb.PhysicalAddress.QuadPart +
                    Slot * IWL_FIRST_TB_STRIDE;
    DataPhysical = Adapter->TxQueueData.PhysicalAddress.QuadPart +
                   Slot * IWL_COMMAND_DATA_STRIDE + 20;
    Tfd->NumberOfTbs = 3;
    Tfd->Tb[0].Length = 20;
    Tfd->Tb[0].Address = FirstPhysical;
    Tfd->Tb[1].Length = (USHORT)HeaderRemainder;
    Tfd->Tb[1].Address = DataPhysical;
    Tfd->Tb[2].Length = (USHORT)BodyLength;
    Tfd->Tb[2].Address = DataPhysical + HeaderRemainder;
    ByteCount = (volatile USHORT *)Adapter->TxByteCount.VirtualAddress;
    ByteCount[Slot] = FrameLength; /* AX210 byte units, one TFD chunk */
    KeMemoryBarrier();
    Adapter->TxWriteIndex = (USHORT)((Slot + 1) &
                                     (IWL_GEN3_TX_QUEUE_SIZE - 1));
    IwlWrite32(Adapter, HBUS_TARG_WRPTR,
               ((ULONG)Adapter->TxQueueId << 16) |
               Adapter->TxWriteIndex);
    NdisReleaseSpinLock(&Adapter->TxLock);
    return TRUE;
}

static BOOLEAN
IwlTransmitAuthentication(_In_ PIWL_ADAPTER Adapter, _In_ PIWL_BSS Bss,
                          _Out_ PUSHORT RxBefore)
{
    IWL_IEEE80211_AUTH Auth;
    BOOLEAN Sent;

    NdisZeroMemory(&Auth, sizeof(Auth));
    Auth.FrameControl = 0x00b0;
    NdisMoveMemory(Auth.Destination, Bss->Bssid, 6);
    NdisMoveMemory(Auth.Source, Adapter->CurrentMacAddress, 6);
    NdisMoveMemory(Auth.Bssid, Bss->Bssid, 6);
    Auth.Algorithm = 0;
    Auth.Transaction = 1;
    Sent = IwlTransmitFrame(Adapter, (const UCHAR *)&Auth,
                            sizeof(Auth), FALSE, RxBefore);
    if (Sent)
        DPRINT1("iwlwifi: transmitted open-auth request on queue %u\n",
                Adapter->TxQueueId);
    return Sent;
}

static BOOLEAN
IwlTransmitAssociationRequest(_In_ PIWL_ADAPTER Adapter, _In_ PIWL_BSS Bss,
                              _Out_ PUSHORT RxBefore)
{
    UCHAR Frame[24 + 4 + 2 + 32 + IWL_MAX_BSS_IE_SIZE];
    PUCHAR Cursor = Frame, Ie = Bss->Ies, End = Bss->Ies + Bss->IeLength;
    USHORT FrameLength;

    NdisZeroMemory(Frame, sizeof(Frame));
    *(UNALIGNED USHORT *)Cursor = 0x0000; /* association request */
    NdisMoveMemory(Cursor + 4, Bss->Bssid, 6);
    NdisMoveMemory(Cursor + 10, Adapter->CurrentMacAddress, 6);
    NdisMoveMemory(Cursor + 16, Bss->Bssid, 6);
    Cursor += 24;
    *(UNALIGNED USHORT *)Cursor = Bss->CapabilityInformation;
    *(UNALIGNED USHORT *)(Cursor + 2) = 10; /* listen interval */
    Cursor += 4;
    *Cursor++ = 0;
    *Cursor++ = (UCHAR)Adapter->DesiredSsid.uSSIDLength;
    NdisMoveMemory(Cursor, Adapter->DesiredSsid.ucSSID,
                   Adapter->DesiredSsid.uSSIDLength);
    Cursor += Adapter->DesiredSsid.uSSIDLength;
    while (Ie + 2 <= End && Ie + 2 + Ie[1] <= End)
    {
        ULONG Length = 2 + Ie[1];
        if ((Ie[0] == 1 || Ie[0] == 50 || Ie[0] == 48) &&
            Cursor + Length <= Frame + sizeof(Frame))
        {
            NdisMoveMemory(Cursor, Ie, Length);
            Cursor += Length;
        }
        Ie += Length;
    }
    FrameLength = (USHORT)(Cursor - Frame);
    if (!IwlTransmitFrame(Adapter, Frame, FrameLength, FALSE, RxBefore))
        return FALSE;
    DPRINT1("iwlwifi: transmitted association request (%u bytes) on "
            "queue %u\n", FrameLength, Adapter->TxQueueId);
    return TRUE;
}

static BOOLEAN
IwlWaitAuthenticationResponse(_In_ PIWL_ADAPTER Adapter,
                              _In_ PIWL_BSS Bss,
                              _In_ USHORT Cursor)
{
    IWL_RX_COMPLETION_DESC *Completion =
        (IWL_RX_COMPLETION_DESC *)Adapter->RxCompletionRing.VirtualAddress;
    ULONG Wait;

    for (Wait = 0; Wait < 100; Wait++)
    {
        USHORT After = *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
        while (Cursor != After)
        {
            USHORT Rbid = Completion[Cursor &
                (IWL_GEN3_RX_QUEUE_SIZE - 1)].Rbid;
            if (Rbid != 0 && Rbid <= Adapter->RxBufferCount)
            {
                IWL_RX_PACKET *Packet = (IWL_RX_PACKET *)
                    Adapter->RxBuffers[Rbid - 1].VirtualAddress;
                ULONG PacketLength = Packet->LengthAndFlags & 0x3fff;
                if (Packet->Command == 0xc1 && Packet->Group == 0 &&
                    PacketLength >= 4 + 64 + sizeof(IWL_IEEE80211_AUTH))
                {
                    IWL_IEEE80211_AUTH *Auth =
                        (IWL_IEEE80211_AUTH *)(Packet->Data + 64);
                    USHORT MpduLength =
                        *(UNALIGNED USHORT *)Packet->Data;
                    if (MpduLength >= sizeof(*Auth) &&
                        (Auth->FrameControl & 0x00fc) == 0x00b0 &&
                        RtlCompareMemory(Auth->Destination,
                                         Adapter->CurrentMacAddress, 6) == 6 &&
                        RtlCompareMemory(Auth->Source, Bss->Bssid, 6) == 6 &&
                        Auth->Algorithm == 0 && Auth->Transaction == 2)
                    {
                        USHORT Status = Auth->Status;
                        IwlRecycleRxBuffer(Adapter, Rbid);
                        DPRINT1("iwlwifi: open-auth response status %u from "
                                "%02x:%02x:%02x:%02x:%02x:%02x\n", Status,
                                Bss->Bssid[0], Bss->Bssid[1], Bss->Bssid[2],
                                Bss->Bssid[3], Bss->Bssid[4], Bss->Bssid[5]);
                        return Status == 0;
                    }
                }
                else
                    DPRINT1("iwlwifi: post-auth RX cmd=0x%02x group=0x%02x "
                            "len=%lu seq=0x%04x\n", Packet->Command,
                            Packet->Group, PacketLength, Packet->Sequence);
                IwlRecycleRxBuffer(Adapter, Rbid);
            }
            Cursor++;
        }
        NdisMSleep(10000);
    }
    DPRINT1("iwlwifi: timed out waiting for open-auth response\n");
    return FALSE;
}

static BOOLEAN
IwlWaitAssociationResponse(_In_ PIWL_ADAPTER Adapter, _In_ PIWL_BSS Bss,
                           _In_ USHORT Cursor, _Out_ PUSHORT AssociationId)
{
    IWL_RX_COMPLETION_DESC *Completion =
        (IWL_RX_COMPLETION_DESC *)Adapter->RxCompletionRing.VirtualAddress;
    ULONG Wait;

    for (Wait = 0; Wait < 100; Wait++)
    {
        USHORT After = *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
        while (Cursor != After)
        {
            USHORT Rbid = Completion[Cursor &
                (IWL_GEN3_RX_QUEUE_SIZE - 1)].Rbid;
            if (Rbid != 0 && Rbid <= Adapter->RxBufferCount)
            {
                IWL_RX_PACKET *Packet = (IWL_RX_PACKET *)
                    Adapter->RxBuffers[Rbid - 1].VirtualAddress;
                ULONG PacketLength = Packet->LengthAndFlags & 0x3fff;
                if (Packet->Command == 0xc1 && Packet->Group == 0 &&
                    PacketLength >= 4 + 64 + 30)
                {
                    PUCHAR Frame = Packet->Data + 64;
                    USHORT MpduLength = *(UNALIGNED USHORT *)Packet->Data;
                    if (MpduLength >= 30 &&
                        ((*(UNALIGNED USHORT *)Frame) & 0x00fc) == 0x0010 &&
                        RtlCompareMemory(Frame + 4,
                                         Adapter->CurrentMacAddress, 6) == 6 &&
                        RtlCompareMemory(Frame + 10, Bss->Bssid, 6) == 6)
                    {
                        USHORT Status = *(UNALIGNED USHORT *)(Frame + 26);
                        *AssociationId =
                            *(UNALIGNED USHORT *)(Frame + 28) & 0x3fff;
                        IwlRecycleRxBuffer(Adapter, Rbid);
                        DPRINT1("iwlwifi: association response status %u "
                                "AID %u\n", Status, *AssociationId);
                        return Status == 0 && *AssociationId != 0;
                    }
                }
                else
                    DPRINT1("iwlwifi: post-association RX cmd=0x%02x "
                            "group=0x%02x len=%lu seq=0x%04x\n",
                            Packet->Command, Packet->Group, PacketLength,
                            Packet->Sequence);
                IwlRecycleRxBuffer(Adapter, Rbid);
            }
            Cursor++;
        }
        NdisMSleep(10000);
    }
    DPRINT1("iwlwifi: timed out waiting for association response\n");
    return FALSE;
}

static BOOLEAN
IwlCommitAssociation(_In_ PIWL_ADAPTER Adapter, _In_ PIWL_BSS Bss,
                     _In_ USHORT AssociationId)
{
    IWL_STA_CONFIG_V1 Station;
    IWL_MAC_CONFIG_V2 Mac;
    IWL_RX_PACKET *Response;

    NdisZeroMemory(&Station, sizeof(Station));
    Station.StaId = 0;
    Station.LinkId = 0;
    NdisMoveMemory(Station.PeerMldAddress, Bss->Bssid, 6);
    NdisMoveMemory(Station.PeerLinkAddress, Bss->Bssid, 6);
    Station.AssociationId = AssociationId;
    Station.Mfp = 1;
    Station.Mimo = 1;
    Response = IwlSendCommand(
        Adapter, 0x03, 0x0a,
        IwlFwLookupCommandVersion(Adapter->FwParsed, 0x03, 0x0a, 1),
        &Station, sizeof(Station), NULL);
    DPRINT1("iwlwifi: STA_CONFIG associated AID %u: %s\n", AssociationId,
            Response != NULL ? "accepted" : "failed");
    if (Response == NULL)
        return FALSE;

    NdisZeroMemory(&Mac, sizeof(Mac));
    Mac.Action = 2; /* FW_CTXT_ACTION_MODIFY */
    Mac.MacType = 5; /* FW_MAC_TYPE_BSS_STA */
    NdisMoveMemory(Mac.LocalMldAddress, Adapter->CurrentMacAddress, 6);
    Mac.FilterFlags = (1UL << 2) | (1UL << 3);
    Mac.Client.IsAssociated = 1;
    Mac.Client.AssociationId = AssociationId;
    Response = IwlSendCommand(
        Adapter, 0x03, 0x08,
        IwlFwLookupCommandVersion(Adapter->FwParsed, 0x03, 0x08, 2),
        &Mac, sizeof(Mac), NULL);
    DPRINT1("iwlwifi: MAC_CONFIG associated AID %u: %s\n", AssociationId,
            Response != NULL ? "accepted" : "failed");
    return Response != NULL;
}

static VOID NTAPI
IwlConnectWorker(_In_ PVOID Context, _In_ NDIS_HANDLE WorkItem)
{
    PIWL_ADAPTER Adapter = Context;
    PIWL_BSS Bss;
    USHORT RxBefore, AssociationId;
    BOOLEAN Associated = FALSE;

    UNREFERENCED_PARAMETER(WorkItem);
    if (InterlockedCompareExchange(&Adapter->ScanQueued, 0, 0))
        KeWaitForSingleObject(&Adapter->ScanIdleEvent, Executive,
                              KernelMode, FALSE, NULL);
    Bss = IwlSelectAssociationBss(Adapter);
    if (Bss == NULL)
        DPRINT1("iwlwifi: no scanned BSS matches the desired network\n");
    else
    {
        IwlIndicateAssociation(Adapter, Bss, TRUE, 0);
        if (IwlPrepareAssociationContext(Adapter, Bss) &&
            IwlConfigureManagementTx(Adapter, Bss) &&
            IwlAllocateTxQueue(Adapter, 15))
        {
            DPRINT1("iwlwifi: pre-auth firmware contexts ready for "
                    "%02x:%02x:%02x:%02x:%02x:%02x\n",
                    Bss->Bssid[0], Bss->Bssid[1], Bss->Bssid[2],
                    Bss->Bssid[3], Bss->Bssid[4], Bss->Bssid[5]);
            if (IwlTransmitAuthentication(Adapter, Bss, &RxBefore) &&
                IwlWaitAuthenticationResponse(Adapter, Bss, RxBefore) &&
                IwlTransmitAssociationRequest(Adapter, Bss, &RxBefore) &&
                IwlWaitAssociationResponse(Adapter, Bss, RxBefore,
                                           &AssociationId) &&
                IwlCommitAssociation(Adapter, Bss, AssociationId) &&
                IwlSwitchToDataQueue(Adapter))
            {
                Adapter->RxReadIndex =
                    *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
                InterlockedExchange(&Adapter->DataPathReady, 1);
                if (Adapter->RxPollTimer != NULL)
                {
                    LARGE_INTEGER DueTime;
                    DueTime.QuadPart = -100000; /* 10 ms */
                    NdisSetTimerObject(Adapter->RxPollTimer, DueTime, 10, NULL);
                }
                IwlProcessReceiveCompletions(Adapter, FALSE);
                Associated = TRUE;
            }
        }
        /* Management TX is the next stage.  Until an authentication and
         * association response have actually arrived, report failure rather
         * than synthesising a successful link. */
        IwlIndicateAssociation(Adapter, Bss, FALSE,
            Associated ? DOT11_ASSOC_STATUS_SUCCESS :
                         DOT11_ASSOC_STATUS_SYSTEM_ERROR);
    }
    InterlockedExchange(&Adapter->ConnectQueued, 0);
    KeSetEvent(&Adapter->ConnectIdleEvent, IO_NO_INCREMENT, FALSE);
}

NDIS_STATUS
IwlStartConnect(_In_ PIWL_ADAPTER Adapter)
{
    if (Adapter->DesiredSsid.uSSIDLength == 0)
        return NDIS_STATUS_INVALID_DATA;
    if (Adapter->ConnectWorkItem == NULL ||
        InterlockedCompareExchange(&Adapter->ConnectQueued, 1, 0))
        return NDIS_STATUS_MEDIA_BUSY;
    KeClearEvent(&Adapter->ConnectIdleEvent);
    NdisQueueIoWorkItem(Adapter->ConnectWorkItem, IwlConnectWorker, Adapter);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
IwlInitializeScan(_In_ PIWL_ADAPTER Adapter)
{
    KeInitializeEvent(&Adapter->ScanIdleEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&Adapter->ConnectIdleEvent, NotificationEvent, TRUE);
    Adapter->ScanWorkItem =
        NdisAllocateIoWorkItem(Adapter->MiniportAdapterHandle);
    Adapter->ConnectWorkItem =
        NdisAllocateIoWorkItem(Adapter->MiniportAdapterHandle);
    if (Adapter->ScanWorkItem == NULL || Adapter->ConnectWorkItem == NULL)
    {
        IwlShutdownScan(Adapter);
        return NDIS_STATUS_RESOURCES;
    }
    return NDIS_STATUS_SUCCESS;
}

VOID
IwlShutdownScan(_In_ PIWL_ADAPTER Adapter)
{
    if (InterlockedCompareExchange(&Adapter->ConnectQueued, 0, 0))
        KeWaitForSingleObject(&Adapter->ConnectIdleEvent, Executive,
                              KernelMode, FALSE, NULL);
    if (InterlockedCompareExchange(&Adapter->ScanQueued, 0, 0))
        KeWaitForSingleObject(&Adapter->ScanIdleEvent, Executive,
                              KernelMode, FALSE, NULL);
    if (Adapter->ScanWorkItem != NULL)
    {
        NdisFreeIoWorkItem(Adapter->ScanWorkItem);
        Adapter->ScanWorkItem = NULL;
    }
    if (Adapter->ConnectWorkItem != NULL)
    {
        NdisFreeIoWorkItem(Adapter->ConnectWorkItem);
        Adapter->ConnectWorkItem = NULL;
    }
}

NDIS_STATUS
IwlStartScan(_In_ PIWL_ADAPTER Adapter)
{
    if (Adapter->ScanWorkItem == NULL ||
        InterlockedCompareExchange(&Adapter->ScanQueued, 1, 0))
        return NDIS_STATUS_MEDIA_BUSY;
    KeClearEvent(&Adapter->ScanIdleEvent);
    NdisQueueIoWorkItem(Adapter->ScanWorkItem, IwlScanWorker, Adapter);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
IwlBuildBssList(
    _In_ PIWL_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST Request)
{
    ULONG i, Payload = 0, Total, BufferLength;
    PUCHAR Cursor;
    PDOT11_BYTE_ARRAY Array;

    BufferLength = Request->DATA.QUERY_INFORMATION.InformationBufferLength;
    for (i = 0; i < Adapter->BssCount; i++)
        Payload += FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer) +
                   Adapter->Bss[i].IeLength;
    Total = FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer) + Payload;
    Request->DATA.QUERY_INFORMATION.BytesNeeded = Total;
    if (BufferLength < Total)
    {
        Request->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    Array = Request->DATA.QUERY_INFORMATION.InformationBuffer;
    NdisZeroMemory(Array, Total);
    Array->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Array->Header.Revision = DOT11_BYTE_ARRAY_REVISION_1;
    Array->Header.Size = sizeof(DOT11_BYTE_ARRAY);
    Array->uNumOfBytes = Array->uTotalNumOfBytes = Payload;
    Cursor = Array->ucBuffer;
    for (i = 0; i < Adapter->BssCount; i++)
    {
        PIWL_BSS Source = &Adapter->Bss[i];
        PDOT11_BSS_ENTRY Entry = (PDOT11_BSS_ENTRY)Cursor;
        Entry->PhySpecificInfo.uChCenterFrequency = Source->ChannelFrequency;
        NdisMoveMemory(Entry->dot11BSSID, Source->Bssid,
                       sizeof(Source->Bssid));
        Entry->dot11BSSType = dot11_BSS_type_infrastructure;
        Entry->lRSSI = Source->Rssi;
        Entry->uLinkQuality = Source->Rssi >= -50 ? 100 :
                              Source->Rssi <= -100 ? 0 :
                              (ULONG)(2 * (Source->Rssi + 100));
        Entry->bInRegDomain = TRUE;
        Entry->usBeaconPeriod = Source->BeaconPeriod;
        Entry->ullHostTimestamp = KeQueryInterruptTime();
        Entry->usCapabilityInformation = Source->CapabilityInformation;
        Entry->uBufferLength = Source->IeLength;
        NdisMoveMemory(Entry->ucBuffer, Source->Ies, Source->IeLength);
        Cursor += FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer) + Source->IeLength;
    }
    Request->DATA.QUERY_INFORMATION.BytesWritten = Total;
    return NDIS_STATUS_SUCCESS;
}

static BOOLEAN
IwlAddUnassociatedStationContext(_In_ PIWL_ADAPTER Adapter)
{
    IWL_MAC_CONFIG_V2 Mac;
    IWL_LINK_CONFIG_V2 Link;
    IWL_RX_PACKET *Response;
    UCHAR MacVersion, LinkVersion;

    MacVersion = IwlFwLookupCommandVersion(Adapter->FwParsed,
                                            0x03, 0x08, 99);
    LinkVersion = IwlFwLookupCommandVersion(Adapter->FwParsed,
                                             0x03, 0x09, 99);
    if (MacVersion != 2 || LinkVersion != 2 || !Adapter->MacAddressValid)
    {
        DPRINT1("iwlwifi: cannot create scan context: MAC_CONFIG v%u, "
                "LINK_CONFIG v%u, address-valid=%u\n", MacVersion,
                LinkVersion, Adapter->MacAddressValid);
        return FALSE;
    }

    NdisZeroMemory(&Mac, sizeof(Mac));
    Mac.Action = 1; /* FW_CTXT_ACTION_ADD */
    Mac.MacType = 5; /* FW_MAC_TYPE_BSS_STA */
    NdisMoveMemory(Mac.LocalMldAddress, Adapter->CurrentMacAddress, 6);
    Mac.FilterFlags = (1UL << 2) | (1UL << 3); /* group + beacon */
    Response = IwlSendSmallCommand(Adapter, 0x03, 0x08, MacVersion,
                                   &Mac, sizeof(Mac));
    DPRINT1("iwlwifi: MAC_CONFIG v2 station context %s\n",
            Response != NULL ? "accepted" : "failed");
    if (Response == NULL)
        return FALSE;

    NdisZeroMemory(&Link, sizeof(Link));
    Link.Action = 1; /* FW_CTXT_ACTION_ADD */
    Link.LinkId = 0;
    Link.MacId = 0;
    Link.PhyId = 0xffffffff; /* FW_CTXT_INVALID until association */
    NdisMoveMemory(Link.LocalLinkAddress, Adapter->CurrentMacAddress, 6);
    Response = IwlSendCommand(Adapter, 0x03, 0x09, LinkVersion,
                              &Link, sizeof(Link), NULL);
    DPRINT1("iwlwifi: LINK_CONFIG v2 inactive link %s\n",
            Response != NULL ? "accepted" : "failed");
    return Response != NULL;
}

static VOID
IwlSendNvmGetInfo(_In_ PIWL_ADAPTER Adapter)
{
    ULONG Slot = Adapter->CommandWriteIndex & (IWL_GEN3_CMD_QUEUE_SIZE - 1);
    IWL_WIDE_COMMAND *Command = (IWL_WIDE_COMMAND *)
        ((PUCHAR)Adapter->CommandFirstTb.VirtualAddress +
         Slot * IWL_FIRST_TB_STRIDE);
    IWL_TFH_TFD *Tfd = &((IWL_TFH_TFD *)
        Adapter->CommandRing.VirtualAddress)[Slot];
    USHORT Before = *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
    ULONG i;

    NdisZeroMemory(Command, IWL_FIRST_TB_STRIDE);
    NdisZeroMemory(Tfd, sizeof(*Tfd));
    Command->Command = 2; /* NVM_GET_INFO */
    Command->Group = 0x0c; /* REGULATORY_AND_NVM_GROUP */
    Command->Sequence = (USHORT)Slot; /* DQA command queue is queue zero. */
    Command->Length = sizeof(Command->Payload);
    Tfd->NumberOfTbs = 1;
    Tfd->Tb[0].Length = sizeof(*Command);
    Tfd->Tb[0].Address = Adapter->CommandFirstTb.PhysicalAddress.QuadPart +
                         Slot * IWL_FIRST_TB_STRIDE;
    KeMemoryBarrier();
    Adapter->CommandWriteIndex = (Slot + 1) &
                                 (IWL_GEN3_CMD_QUEUE_SIZE - 1);
    IwlWrite32(Adapter, HBUS_TARG_WRPTR, Adapter->CommandWriteIndex);

    for (i = 0; i < 500; i++)
    {
        if (*(volatile USHORT *)Adapter->RxStatus.VirtualAddress != Before)
            break;
        NdisMSleep(1000);
    }
    DPRINT1("iwlwifi: NVM_GET_INFO command: RX closed %u -> %u\n", Before,
            *(volatile USHORT *)Adapter->RxStatus.VirtualAddress);
    if (*(volatile USHORT *)Adapter->RxStatus.VirtualAddress != Before)
    {
        IWL_RX_COMPLETION_DESC *Completion =
            (IWL_RX_COMPLETION_DESC *)Adapter->RxCompletionRing.VirtualAddress;
        USHORT Rbid = Completion[Before &
            (IWL_GEN3_RX_QUEUE_SIZE - 1)].Rbid;
        if (Rbid != 0 && Rbid <= Adapter->RxBufferCount)
        {
            IWL_RX_PACKET *Packet = (IWL_RX_PACKET *)
                Adapter->RxBuffers[Rbid - 1].VirtualAddress;
            DPRINT1("iwlwifi: NVM_GET_INFO RX rbid=%u len=%lu cmd=0x%02x "
                    "group=0x%02x seq=0x%04x\n", Rbid,
                    Packet->LengthAndFlags & 0x3fff, Packet->Command,
                    Packet->Group, Packet->Sequence);
            if ((Packet->LengthAndFlags & 0x3fff) ==
                    sizeof(Packet->Command) + sizeof(Packet->Group) +
                    sizeof(Packet->Sequence) + sizeof(IWL_NVM_INFO_V4) &&
                Packet->Command == 2 && Packet->Group == 0x0c)
            {
                IWL_NVM_INFO_V4 *Nvm = (IWL_NVM_INFO_V4 *)Packet->Data;
                ULONG Valid = 0;
                Adapter->NvmVersion = Nvm->NvmVersion;
                Adapter->ValidTxAntennas = (UCHAR)Nvm->TxChains;
                Adapter->ValidRxAntennas = (UCHAR)Nvm->RxChains;
                Adapter->NvmChannelCount =
                    min(Nvm->ChannelCount, RTL_NUMBER_OF(Adapter->NvmChannelFlags));
                for (i = 0; i < Adapter->NvmChannelCount; i++)
                {
                    Adapter->NvmChannelFlags[i] = Nvm->ChannelProfile[i];
                    if (Nvm->ChannelProfile[i] & 1)
                        Valid++;
                }
                DPRINT1("iwlwifi: NVM v4 version=0x%04x flags=0x%08x "
                        "MAC-SKU=0x%08x TX/RX chains=0x%x/0x%x, "
                        "%lu/%lu valid channels, LAR=%lu\n",
                        Nvm->NvmVersion, Nvm->GeneralFlags,
                        Nvm->MacSkuFlags, Nvm->TxChains, Nvm->RxChains,
                        Valid, Adapter->NvmChannelCount, Nvm->LarEnabled);
            }
        }
    }
}

static BOOLEAN
IwlMacAddressValid(_In_reads_(6) const UCHAR *Address)
{
    ULONG i;
    BOOLEAN Any = FALSE, AllFf = TRUE;
    if (Address[0] & 1)
        return FALSE;
    for (i = 0; i < 6; i++)
    {
        Any |= Address[i] != 0;
        AllFf &= Address[i] == 0xff;
    }
    return Any && !AllFf;
}

static VOID
IwlReadCsrMac(_In_ PIWL_ADAPTER Adapter)
{
    ULONG A = IwlRead32(Adapter, CSR_MAC_ADDR0_STRAP);
    ULONG B = IwlRead32(Adapter, CSR_MAC_ADDR1_STRAP);
    UCHAR *Mac = Adapter->PermanentMacAddress;
Retry:
    Mac[0] = (UCHAR)(A >> 24); Mac[1] = (UCHAR)(A >> 16);
    Mac[2] = (UCHAR)(A >> 8);  Mac[3] = (UCHAR)A;
    Mac[4] = (UCHAR)(B >> 8);  Mac[5] = (UCHAR)B;
    if (!IwlMacAddressValid(Mac) && A != IwlRead32(Adapter, CSR_MAC_ADDR0_OTP))
    {
        A = IwlRead32(Adapter, CSR_MAC_ADDR0_OTP);
        B = IwlRead32(Adapter, CSR_MAC_ADDR1_OTP);
        goto Retry;
    }
    Adapter->MacAddressValid = IwlMacAddressValid(Mac);
    if (Adapter->MacAddressValid)
    {
        NdisMoveMemory(Adapter->CurrentMacAddress, Mac, 6);
        DPRINT1("iwlwifi: permanent CSR MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]);
    }
}

static NDIS_STATUS
IwlAllocateDmaZero(_In_ PIWL_ADAPTER Adapter, _In_ ULONG Length,
                   _Out_ PIWL_DMA_BLOCK Block)
{
    NdisMAllocateSharedMemory(Adapter->MiniportAdapterHandle, Length, FALSE,
                              &Block->VirtualAddress,
                              &Block->PhysicalAddress);
    if (Block->VirtualAddress == NULL)
        return NDIS_STATUS_RESOURCES;
    Block->Length = Length;
    NdisZeroMemory(Block->VirtualAddress, Length);
    return NDIS_STATUS_SUCCESS;
}

static VOID
IwlFreeDmaBlock(_In_ PIWL_ADAPTER Adapter, _Inout_ PIWL_DMA_BLOCK Block)
{
    if (Block->VirtualAddress != NULL)
        NdisMFreeSharedMemory(Adapter->MiniportAdapterHandle, Block->Length,
                              FALSE, Block->VirtualAddress,
                              Block->PhysicalAddress);
    NdisZeroMemory(Block, sizeof(*Block));
}

static NDIS_STATUS
IwlAllocateDmaCopy(
    _In_ PIWL_ADAPTER Adapter,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length,
    _Out_ PIWL_DMA_BLOCK Block)
{
    if (Length == 0)
        return NDIS_STATUS_INVALID_DATA;

    NdisMAllocateSharedMemory(Adapter->MiniportAdapterHandle,
                              Length,
                              FALSE,
                              &Block->VirtualAddress,
                              &Block->PhysicalAddress);
    if (Block->VirtualAddress == NULL)
        return NDIS_STATUS_RESOURCES;

    Block->Length = Length;
    NdisMoveMemory(Block->VirtualAddress, Data, Length);
    return NDIS_STATUS_SUCCESS;
}

VOID
IwlGen3FreeFirmwareDma(_In_ PIWL_ADAPTER Adapter)
{
    ULONG i;

    for (i = 0; i < Adapter->FwDmaCount; i++)
    {
        PIWL_DMA_BLOCK Block = &Adapter->FwDma[i];
        if (Block->VirtualAddress != NULL)
        {
            NdisMFreeSharedMemory(Adapter->MiniportAdapterHandle,
                                  Block->Length,
                                  FALSE,
                                  Block->VirtualAddress,
                                  Block->PhysicalAddress);
        }
    }

    IwlFreeDmaBlock(Adapter, &Adapter->ContextInfo);
    IwlFreeDmaBlock(Adapter, &Adapter->PnvmDescriptor);
    for (i = 0; i < Adapter->PnvmDmaCount; i++)
        IwlFreeDmaBlock(Adapter, &Adapter->PnvmDma[i]);
    Adapter->PnvmDmaCount = 0;
    IwlFreeDmaBlock(Adapter, &Adapter->PrphInfoPage);
    IwlFreeDmaBlock(Adapter, &Adapter->PrphScratch);
    IwlFreeDmaBlock(Adapter, &Adapter->CommandRing);
    IwlFreeDmaBlock(Adapter, &Adapter->CommandFirstTb);
    IwlFreeDmaBlock(Adapter, &Adapter->CommandData);
    IwlFreeDmaBlock(Adapter, &Adapter->TxQueueRing);
    IwlFreeDmaBlock(Adapter, &Adapter->TxQueueFirstTb);
    IwlFreeDmaBlock(Adapter, &Adapter->TxQueueData);
    IwlFreeDmaBlock(Adapter, &Adapter->TxByteCount);
    Adapter->TxQueueValid = FALSE;
    IwlFreeDmaBlock(Adapter, &Adapter->RxStatus);
    IwlFreeDmaBlock(Adapter, &Adapter->RxCompletionRing);
    IwlFreeDmaBlock(Adapter, &Adapter->RxTransferRing);
    if (Adapter->RxBuffers != NULL)
    {
        for (i = 0; i < Adapter->RxBufferCount; i++)
            IwlFreeDmaBlock(Adapter, &Adapter->RxBuffers[i]);
        NdisFreeMemory(Adapter->RxBuffers,
                       sizeof(IWL_DMA_BLOCK) *
                       (IWL_GEN3_RX_QUEUE_SIZE - 1), 0);
        Adapter->RxBuffers = NULL;
        Adapter->RxBufferCount = 0;
    }
    IwlFreeDmaBlock(Adapter, &Adapter->ImlDma);

    NdisZeroMemory(Adapter->FwDma, sizeof(Adapter->FwDma));
    NdisZeroMemory(&Adapter->ImlDma, sizeof(Adapter->ImlDma));
    Adapter->FwDmaCount = 0;
    Adapter->FwLmacCount = 0;
    Adapter->FwUmacCount = 0;
    Adapter->FwPagingCount = 0;
    InterlockedAnd(&Adapter->Flags,
                   ~(IWL_FLAG_FW_DMA_READY | IWL_FLAG_GEN3_BOOT_READY));
}

NDIS_STATUS
IwlGen3StartFirmware(_In_ PIWL_ADAPTER Adapter)
{
    IWL_PRPH_SCRATCH *Scratch;
    IWL_CONTEXT_INFO_GEN3 *Context;
    ULONG i, Index, LastLtr, Loops;
    LARGE_INTEGER Frequency, Start, Now;
    NDIS_STATUS Status;
    IWL_RX_TRANSFER_DESC *Transfer;

    if (!(Adapter->Flags & IWL_FLAG_FW_DMA_READY) ||
        Adapter->Cfg->Family < IWL_DEVICE_FAMILY_AX210)
        return NDIS_STATUS_NOT_SUPPORTED;

#define IWL_ALLOC(_member, _size) do { \
    Status = IwlAllocateDmaZero(Adapter, (_size), &Adapter->_member); \
    if (Status != NDIS_STATUS_SUCCESS) goto Fail; \
} while (0)
    IWL_ALLOC(RxTransferRing,
              sizeof(IWL_RX_TRANSFER_DESC) * IWL_GEN3_RX_QUEUE_SIZE);
    IWL_ALLOC(RxCompletionRing, 32 * IWL_GEN3_RX_QUEUE_SIZE);
    IWL_ALLOC(RxStatus, sizeof(USHORT));
    IWL_ALLOC(CommandRing, IWL_GEN3_TFD_SIZE * IWL_GEN3_CMD_QUEUE_SIZE);
    IWL_ALLOC(CommandFirstTb,
              IWL_FIRST_TB_STRIDE * IWL_GEN3_CMD_QUEUE_SIZE);
    IWL_ALLOC(CommandData,
              IWL_COMMAND_DATA_STRIDE * IWL_GEN3_CMD_QUEUE_SIZE);
    IWL_ALLOC(TxQueueRing,
              IWL_GEN3_TFD_SIZE * IWL_GEN3_TX_QUEUE_SIZE);
    IWL_ALLOC(TxQueueFirstTb,
              IWL_FIRST_TB_STRIDE * IWL_GEN3_TX_QUEUE_SIZE);
    IWL_ALLOC(TxQueueData,
              IWL_COMMAND_DATA_STRIDE * IWL_GEN3_TX_QUEUE_SIZE);
    IWL_ALLOC(TxByteCount,
              sizeof(USHORT) * IWL_GEN3_TX_BC_ENTRIES);
    IWL_ALLOC(PrphScratch, sizeof(IWL_PRPH_SCRATCH));
    IWL_ALLOC(PrphInfoPage, IWL_GEN3_INFO_PAGE_SIZE);
    IWL_ALLOC(ContextInfo, sizeof(IWL_CONTEXT_INFO_GEN3));
#undef IWL_ALLOC

    Adapter->RxBuffers = NdisAllocateMemoryWithTagPriority(
        Adapter->MiniportAdapterHandle,
        sizeof(IWL_DMA_BLOCK) * (IWL_GEN3_RX_QUEUE_SIZE - 1),
        IWL_TAG, NormalPoolPriority);
    if (Adapter->RxBuffers == NULL)
    {
        Status = NDIS_STATUS_RESOURCES;
        goto Fail;
    }
    NdisZeroMemory(Adapter->RxBuffers,
                   sizeof(IWL_DMA_BLOCK) * (IWL_GEN3_RX_QUEUE_SIZE - 1));
    Transfer = (IWL_RX_TRANSFER_DESC *)
        Adapter->RxTransferRing.VirtualAddress;
    for (i = 0; i < IWL_GEN3_RX_QUEUE_SIZE - 1; i++)
    {
        Status = IwlAllocateDmaZero(Adapter, IWL_GEN3_RX_BUFFER_SIZE,
                                    &Adapter->RxBuffers[i]);
        if (Status != NDIS_STATUS_SUCCESS)
            goto Fail;
        Adapter->RxBufferCount++;
        Transfer[i].Rbid = (USHORT)(i + 1);
        Transfer[i].Address =
            Adapter->RxBuffers[i].PhysicalAddress.QuadPart;
    }
    Adapter->RxTransferWriteIndex = IWL_GEN3_RX_QUEUE_SIZE - 1;
    Adapter->RxTransferWriteActual =
        (IWL_GEN3_RX_QUEUE_SIZE - 1) & ~7UL;

    Scratch = (IWL_PRPH_SCRATCH *)Adapter->PrphScratch.VirtualAddress;
    Scratch->MacId = (USHORT)Adapter->HwRev;
    Scratch->Size = sizeof(*Scratch) / sizeof(ULONG);
    Scratch->ControlFlags = IWL_PRPH_SCRATCH_MTR_MODE |
                            IWL_PRPH_MTR_FORMAT_256B;
    Scratch->FreeRbdAddress = Adapter->RxTransferRing.PhysicalAddress.QuadPart;

    Index = 0;
    for (i = 0; i < Adapter->FwLmacCount; i++, Index++)
        Scratch->Dram.Lmac[i] = Adapter->FwDma[Index].PhysicalAddress.QuadPart;
    for (i = 0; i < Adapter->FwUmacCount; i++, Index++)
        Scratch->Dram.Umac[i] = Adapter->FwDma[Index].PhysicalAddress.QuadPart;
    for (i = 0; i < Adapter->FwPagingCount; i++, Index++)
        Scratch->Dram.Paging[i] = Adapter->FwDma[Index].PhysicalAddress.QuadPart;

    Context = (IWL_CONTEXT_INFO_GEN3 *)Adapter->ContextInfo.VirtualAddress;
    Context->PrphInfoBase = Adapter->PrphInfoPage.PhysicalAddress.QuadPart;
    Context->PrphScratchBase = Adapter->PrphScratch.PhysicalAddress.QuadPart;
    Context->PrphScratchSize = sizeof(*Scratch);
    Context->CrHeadIndexBase = Adapter->RxStatus.PhysicalAddress.QuadPart;
    Context->TrTailIndexBase =
        Adapter->PrphInfoPage.PhysicalAddress.QuadPart + 2048;
    Context->CrTailIndexBase =
        Adapter->PrphInfoPage.PhysicalAddress.QuadPart + 3072;
    Context->MtrBase = Adapter->CommandRing.PhysicalAddress.QuadPart;
    Context->McrBase = Adapter->RxCompletionRing.PhysicalAddress.QuadPart;
    /* so-a0-gf-a0 has min_txq_size=128 and num_rbds=4096 in upstream
     * cfg/ax210.c.  Context fields hold log2(queue entries). */
    Context->MtrSize = 7;
    Context->McrSize = 12;

    /* Publish all coherent contents before permitting device DMA. */
    KeMemoryBarrier();
    IwlWrite32(Adapter, CSR_INT, 0xffffffff);
    IwlWrite32(Adapter, CSR_INT_MASK,
               CSR_INT_BIT_ALIVE | CSR_INT_BIT_FH_RX |
               CSR_INT_BIT_SW_RX | CSR_INT_BIT_HW_ERR |
               CSR_INT_BIT_SW_ERR);
    IwlWrite32(Adapter, CSR_CTXT_INFO_ADDR,
               Adapter->ContextInfo.PhysicalAddress.LowPart);
    IwlWrite32(Adapter, CSR_CTXT_INFO_ADDR + 4,
               Adapter->ContextInfo.PhysicalAddress.HighPart);
    IwlWrite32(Adapter, CSR_IML_DATA_ADDR,
               Adapter->ImlDma.PhysicalAddress.LowPart);
    IwlWrite32(Adapter, CSR_IML_DATA_ADDR + 4,
               Adapter->ImlDma.PhysicalAddress.HighPart);
    IwlWrite32(Adapter, CSR_IML_SIZE_ADDR, Adapter->ImlDma.Length);
    /* Integrated AX210 has a ROM LTR bug: upstream must keep both CPU and
     * device busy while IML runs or self-load can silently stop. */
    IwlWrite32(Adapter, CSR_MSIX_HW_INT_CAUSES_AD,
               MSIX_HW_INT_CAUSES_REG_IML);
    IwlSetBit(Adapter, CSR_CTXT_INFO_BOOT_CTRL, CSR_AUTO_FUNC_BOOT_ENA);

    /* AX210 UMAC peripheral write: address is 24-bit and byte-enabled. */
    IwlWrite32(Adapter, HBUS_TARG_PRPH_WADDR,
               ((UREG_CPU_INIT_RUN + AX210_UMAC_PRPH_OFFSET) & 0x00ffffff) |
               (3UL << 24));
    IwlWrite32(Adapter, HBUS_TARG_PRPH_WDAT, 1);
    Start = KeQueryPerformanceCounter(&Frequency);
    LastLtr = IwlRead32(Adapter, CSR_LTR_LAST_MSG);
    Loops = 0;
    do
    {
        if (IwlRead32(Adapter, CSR_MSIX_HW_INT_CAUSES_AD) &
            MSIX_HW_INT_CAUSES_REG_IML)
            break;
        LastLtr = IwlRead32(Adapter, CSR_LTR_LAST_MSG);
        Loops++;
        Now = KeQueryPerformanceCounter(NULL);
    } while ((Now.QuadPart - Start.QuadPart) * 10 < Frequency.QuadPart);
    DPRINT1("iwlwifi: IML poll: cause=0x%08x loops=%lu LTR=0x%08x\n",
            IwlRead32(Adapter, CSR_MSIX_HW_INT_CAUSES_AD), Loops, LastLtr);
    InterlockedOr(&Adapter->Flags, IWL_FLAG_GEN3_BOOT_READY);
    DPRINT1("iwlwifi: Gen3 self-load kicked, context PA 0x%I64x\n",
            Adapter->ContextInfo.PhysicalAddress.QuadPart);
    for (i = 0; i < 500 && !Adapter->FirmwareAlive; i++)
    {
        ULONG Cause = IwlRead32(Adapter, CSR_INT);
        if (Cause & CSR_INT_BIT_ALIVE)
        {
            Adapter->LastInterruptCause |= Cause;
            InterlockedExchange(&Adapter->FirmwareAlive, 1);
            /* The ALIVE edge means firmware has configured RFH.  Publish
             * the prefilled default queue exactly as gen2's ALIVE ISR
             * calls iwl_pcie_rxmq_restock(): 4095 entries rounds down to
             * the required multiple-of-eight write index, 4088. */
            KeMemoryBarrier();
            IwlWrite32(Adapter, RFH_Q0_FRBDCB_WIDX_TRG,
                       (IWL_GEN3_RX_QUEUE_SIZE - 1) & ~7UL);
            IwlWrite32(Adapter, CSR_INT, Cause);
            DPRINT1("iwlwifi: ALIVE observed by CSR fallback; IRQ line "
                    "was not delivered (mask=0x%08x)\n",
                    IwlRead32(Adapter, CSR_INT_MASK));
            break;
        }
        NdisMSleep(10000);
    }
    if (!Adapter->FirmwareAlive)
    {
        DPRINT1("iwlwifi: firmware ALIVE timed out; CSR_INT=0x%08x, "
                "seen=0x%08x\n", IwlRead32(Adapter, CSR_INT),
                Adapter->LastInterruptCause);
        return NDIS_STATUS_HARD_ERRORS;
    }
    DPRINT1("iwlwifi: firmware reported ALIVE\n");
    DPRINT1("iwlwifi: command versions: NVM_GET_INFO=%u SCAN_CFG=%u "
            "SCAN_REQ_UMAC=%u\n",
            IwlFwLookupCommandVersion(Adapter->FwParsed, 0x0c, 0x02, 99),
            IwlFwLookupCommandVersion(Adapter->FwParsed, 0x01, 0x0c, 99),
            IwlFwLookupCommandVersion(Adapter->FwParsed, 0x01, 0x0d, 99));
    for (i = 0; i < 100 && *(volatile USHORT *)Adapter->RxStatus.VirtualAddress == 0; i++)
        NdisMSleep(1000);
    if (*(volatile USHORT *)Adapter->RxStatus.VirtualAddress != 0)
    {
        IWL_RX_COMPLETION_DESC *Completion =
            (IWL_RX_COMPLETION_DESC *)Adapter->RxCompletionRing.VirtualAddress;
        USHORT Rbid = Completion[0].Rbid;
        if (Rbid != 0 && Rbid <= Adapter->RxBufferCount)
        {
            IWL_RX_PACKET *Packet = (IWL_RX_PACKET *)
                Adapter->RxBuffers[Rbid - 1].VirtualAddress;
            DPRINT1("iwlwifi: first RX notification: closed=%u rbid=%u "
                    "len=%lu cmd=0x%02x group=0x%02x seq=0x%04x\n",
                    *(volatile USHORT *)Adapter->RxStatus.VirtualAddress,
                    Rbid, Packet->LengthAndFlags & 0x3fff,
                    Packet->Command, Packet->Group, Packet->Sequence);
            if ((Packet->LengthAndFlags & 0x3fff) ==
                    sizeof(Packet->Command) + sizeof(Packet->Group) +
                    sizeof(Packet->Sequence) + sizeof(IWL_ALIVE_V6) &&
                Packet->Command == 1 && Packet->Group == 0)
            {
                IWL_ALIVE_V6 *Alive = (IWL_ALIVE_V6 *)Packet->Data;
                const IWL_PNVM_BLOCK *Block;
                Adapter->FirmwareSkuId[0] = Alive->SkuId[0];
                Adapter->FirmwareSkuId[1] = Alive->SkuId[1];
                Adapter->FirmwareSkuId[2] = Alive->SkuId[2];
                Adapter->FirmwareSkuValid = (Alive->Status == 0xcafe);
                DPRINT1("iwlwifi: ALIVE v6 status=0x%04x SKU "
                        "%08x-%08x-%08x\n", Alive->Status,
                        Alive->SkuId[0], Alive->SkuId[1], Alive->SkuId[2]);
                Block = IwlPnvmSelectBlock(Adapter->PnvmParsed,
                                           Adapter->FirmwareSkuId);
                if (Block != NULL)
                {
                    ULONGLONG *Addresses;
                    ULONG Total = 0, Before;
                    DPRINT1("iwlwifi: selected PNVM 0x%08x, %lu "
                            "section(s), %lu bytes\n", Block->Version,
                            Block->SectionCount, Block->TotalDataLength);
                    Status = IwlAllocateDmaZero(Adapter,
                                                64 * sizeof(ULONGLONG),
                                                &Adapter->PnvmDescriptor);
                    if (Status != NDIS_STATUS_SUCCESS)
                        goto Fail;
                    Addresses = (ULONGLONG *)
                        Adapter->PnvmDescriptor.VirtualAddress;
                    for (Index = 0; Index < Block->SectionCount; Index++)
                    {
                        Status = IwlAllocateDmaCopy(
                            Adapter, Block->Section[Index].Data,
                            Block->Section[Index].Length,
                            &Adapter->PnvmDma[Index]);
                        if (Status != NDIS_STATUS_SUCCESS)
                            goto Fail;
                        Adapter->PnvmDmaCount++;
                        Addresses[Index] = Adapter->PnvmDma[Index].
                            PhysicalAddress.QuadPart;
                        Total += Adapter->PnvmDma[Index].Length;
                    }
                    Scratch->PnvmBase =
                        Adapter->PnvmDescriptor.PhysicalAddress.QuadPart;
                    Scratch->PnvmSize = Total;
                    KeMemoryBarrier();
                    Before = *(volatile USHORT *)
                        Adapter->RxStatus.VirtualAddress;
                    IwlWrite32(Adapter, HBUS_TARG_PRPH_WADDR,
                        ((UREG_DOORBELL_TO_ISR6 + AX210_UMAC_PRPH_OFFSET) &
                         0x00ffffff) | (3UL << 24));
                    IwlWrite32(Adapter, HBUS_TARG_PRPH_WDAT,
                               UREG_DOORBELL_TO_ISR6_PNVM);
                    for (Index = 0; Index < 500; Index++)
                    {
                        if (*(volatile USHORT *)Adapter->RxStatus.VirtualAddress !=
                            Before)
                            break;
                        NdisMSleep(1000);
                    }
                    DPRINT1("iwlwifi: PNVM doorbell: RX closed %lu -> %u\n",
                            Before, *(volatile USHORT *)
                            Adapter->RxStatus.VirtualAddress);
                    IwlReadCsrMac(Adapter);
                    for (Index = Before;
                         Index < *(volatile USHORT *)
                                     Adapter->RxStatus.VirtualAddress;
                         Index++)
                    {
                        USHORT NewRbid = Completion[Index &
                            (IWL_GEN3_RX_QUEUE_SIZE - 1)].Rbid;
                        if (NewRbid != 0 &&
                            NewRbid <= Adapter->RxBufferCount)
                        {
                            IWL_RX_PACKET *NewPacket = (IWL_RX_PACKET *)
                                Adapter->RxBuffers[NewRbid - 1].VirtualAddress;
                            DPRINT1("iwlwifi: post-PNVM RX[%lu] rbid=%u "
                                    "len=%lu cmd=0x%02x group=0x%02x\n",
                                    Index, NewRbid,
                                    NewPacket->LengthAndFlags & 0x3fff,
                                    NewPacket->Command, NewPacket->Group);
                        }
                    }
                    {
                        ULONG InitFlags = 1UL << 1; /* IWL_INIT_NVM */
                        ULONG Zero = 0;
                        IWL_SCAN_CONFIG_V5 ScanConfig;
                        IWL_MCC_UPDATE_COMMAND MccUpdate;
                        IWL_RX_PACKET *Response;

                        Response = IwlSendSmallCommand(
                            Adapter, 0x02, 0x03,
                            IwlFwLookupCommandVersion(Adapter->FwParsed,
                                                      0x02, 0x03, 0),
                            &InitFlags, sizeof(InitFlags));
                        DPRINT1("iwlwifi: INIT_EXTENDED_CFG %s\n",
                                Response != NULL ? "accepted" : "no response");
                        IwlSendNvmGetInfo(Adapter);
                        Response = IwlSendSmallCommand(
                            Adapter, 0x0c, 0x00,
                            IwlFwLookupCommandVersion(Adapter->FwParsed,
                                                      0x0c, 0x00, 0),
                            &Zero, sizeof(Zero));
                        DPRINT1("iwlwifi: NVM_ACCESS_COMPLETE %s\n",
                                Response != NULL ? "accepted" :
                                                   "sent; no direct response");
                        /* LAR blocks scans until the firmware has a
                         * regulatory profile.  Linux initially requests its
                         * current/default profile with MCC "ZZ" and source
                         * MCC_SOURCE_GET_CURRENT. */
                        NdisZeroMemory(&MccUpdate, sizeof(MccUpdate));
                        MccUpdate.Mcc = ((USHORT)'Z' << 8) | 'Z';
                        MccUpdate.SourceId = 0x10;
                        Response = IwlSendSmallCommand(
                            Adapter, 0x01, 0xc8,
                            IwlFwLookupCommandVersion(Adapter->FwParsed,
                                                      0x01, 0xc8, 1),
                            &MccUpdate, sizeof(MccUpdate));
                        if (Response != NULL)
                        {
                            DPRINT1("iwlwifi: MCC_UPDATE current profile "
                                    "accepted (len=%lu)\n",
                                    Response->LengthAndFlags & 0x3fff);
                        }
                        else
                            DPRINT1("iwlwifi: MCC_UPDATE current profile failed\n");
                        NdisZeroMemory(&ScanConfig, sizeof(ScanConfig));
                        ScanConfig.TxChains = Adapter->ValidTxAntennas;
                        ScanConfig.RxChains = Adapter->ValidRxAntennas;
                        Response = IwlSendSmallCommand(
                            Adapter, 0x01, 0x0c,
                            IwlFwLookupCommandVersion(Adapter->FwParsed,
                                                      0x01, 0x0c, 5),
                            &ScanConfig, sizeof(ScanConfig));
                        DPRINT1("iwlwifi: SCAN_CFG v5 %s\n",
                                Response != NULL ? "accepted" : "failed");
                        if (Response != NULL &&
                            IwlAddUnassociatedStationContext(Adapter))
                            IwlRunPassiveScan(Adapter);
                    }
                }
                else
                    DPRINT1("iwlwifi: no PNVM block matches firmware SKU\n");
            }
        }
    }
    else
    {
        DPRINT1("iwlwifi: ALIVE CSR arrived without an RX notification\n");
    }
    return NDIS_STATUS_SUCCESS;

Fail:
    DPRINT1("iwlwifi: Gen3 boot-contract DMA allocation failed 0x%08x\n",
            Status);
    IwlGen3FreeFirmwareDma(Adapter);
    return Status;
}

NDIS_STATUS
IwlGen3AllocateFirmwareDma(_In_ PIWL_ADAPTER Adapter)
{
    const IWL_FW_IMAGE *Image;
    ULONG i;
    ULONG Region = 0;
    NDIS_STATUS Status;

    if (Adapter->FwParsed == NULL || Adapter->FwParsed->ImlLength == 0)
    {
        DPRINT1("iwlwifi: Gen3 firmware has no IML TLV\n");
        return NDIS_STATUS_INVALID_DATA;
    }

    Image = &Adapter->FwParsed->Image[IWL_UCODE_REGULAR];
    for (i = 0; i < Image->SectionCount; i++)
    {
        const IWL_FW_SECTION *Section = &Image->Section[i];
        PIWL_DMA_BLOCK Block;

        if (Section->Offset == CPU1_CPU2_SEPARATOR_SECTION)
        {
            Region = 1;
            continue;
        }
        if (Section->Offset == PAGING_SEPARATOR_SECTION)
        {
            Region = 2;
            continue;
        }
        if (Adapter->FwDmaCount == IWL_MAX_FW_DMA_BLOCKS)
        {
            Status = NDIS_STATUS_RESOURCES;
            goto Fail;
        }

        Block = &Adapter->FwDma[Adapter->FwDmaCount];
        Status = IwlAllocateDmaCopy(Adapter, Section->Data,
                                    Section->Length, Block);
        if (Status != NDIS_STATUS_SUCCESS)
            goto Fail;

        Adapter->FwDmaCount++;
        if (Region == 0)
            Adapter->FwLmacCount++;
        else if (Region == 1)
            Adapter->FwUmacCount++;
        else
            Adapter->FwPagingCount++;
    }

    Status = IwlAllocateDmaCopy(Adapter,
                                Adapter->FwParsed->ImlData,
                                Adapter->FwParsed->ImlLength,
                                &Adapter->ImlDma);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    InterlockedOr(&Adapter->Flags, IWL_FLAG_FW_DMA_READY);
    DPRINT1("iwlwifi: Gen3 DMA ready: %lu LMAC, %lu UMAC, %lu paging "
            "sections; IML %lu bytes at PA 0x%I64x\n",
            Adapter->FwLmacCount, Adapter->FwUmacCount,
            Adapter->FwPagingCount, Adapter->ImlDma.Length,
            Adapter->ImlDma.PhysicalAddress.QuadPart);
    return NDIS_STATUS_SUCCESS;

Fail:
    IwlGen3FreeFirmwareDma(Adapter);
    return Status;
}
