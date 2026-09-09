/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     NDIS 6.20 native-802.11 miniport for the AR9485 PCIe chip
 *              (PCI 168C:0032).  This header collects the shared adapter
 *              extension and forward declarations consumed by every
 *              ar9485 .c file.
 *
 * Phase 1a scope: load, bind to PCI\VEN_168C&DEV_0032, map BAR0, read the
 * AR_SREV register at offset 0x4020, and log the chip-revision.  No data
 * path, no PHY/RF, no scan/connect.  Subsequent phases drop the verbatim
 * Linux ath9k_hw layer under drivers/network/dd/ar9485/ath9k/ and wire it
 * to MiniportInitializeEx.
 */

#ifndef _AR9485_H_
#define _AR9485_H_

#include <ndis.h>
#include <windot11.h>
#include <wdmguid.h>

/* Pulled in from the sibling e1000e driver for now; this file backfills the
 * NDIS 6.20/6.30 symbol gaps in ROS' SDK ndis.h.  Promote to a real
 * sdk/include/ddk/ndis6_compat.h in a follow-up cleanup so each NDIS 6
 * miniport doesn't have to reach into a sibling tree. */
#include "../e1000e/ndis6_compat.h"

/* Linux-kernel-primitive compat shim for the verbatim ath9k port.  The
 * ath9k/ subdirectory carries Linux source files (reg.h, future hw.[ch],
 * ar9003_xxx) modified only at the include layer to pull this header
 * instead of the Linux kernel ones.  Pulled into the miniport-side
 * ar9485.h so any conflict between the compat types and NDIS headers
 * surfaces at build time. */
#include "linux-compat.h"

/* 802.11 frame layout shared by the transmit path and the MLME. */
#include "dot11frame.h"

/* Register offsets and bit math now live under ath9k/reg.h (slice 1) and
 * are consumed via ath9k/hw_min.h's verbatim AR_SREV_* constants
 * (slice 2).  The miniport keeps one byte-offset constant for the
 * diagnostic raw-register read after chip detection - named with an
 * AR9485_ prefix to avoid colliding with upstream ath9k/reg.h's
 * AR_SREV(_ah) function-form macro for the same register. */
#define AR9485_AR_SREV_OFFSET       0x4020

/* Chip-version constant used as the acceptance check after
 * ar9485_read_revisions() returns. */
#define AR_SREV_VERSION_9485        0x240

#define AR9485_TAG                  'A584'

#define AR9485_FLAG_HW_RECOGNIZED   0x00000001
#define AR9485_FLAG_HALTING         0x00000002

/* Length of an 802.3-format MAC address, which is what NDIS' general
 * attributes carry even for a native-802.11 miniport. */
#define AR9485_MAC_ADDRESS_LENGTH   6
#define AR9485_RX_BUFFER_COUNT      64
#define AR9485_RX_BUFFER_SIZE       4096
#define AR9485_RX_STATUS_SIZE       48
#define AR9485_MAX_BSS              64
#define AR9485_MAX_BSS_IE_SIZE      512

/* Transmit.  The AR9300 family pushes descriptors one at a time into a
 * per-QCU hardware FIFO whose depth is eight (ATH_TXFIFO_DEPTH upstream), so
 * eight is also the useful number of software slots. */
#define AR9485_TX_QCU               0
#define AR9485_TX_DCU               0
#define AR9485_TX_SLOT_COUNT        8
/* This part is 1x1: the only transmit chain, named in every ctl18 rate series. */
#define AR9485_TX_CHAINMASK         1
#define AR9485_TX_FRAME_SIZE        2432
#define AR9485_TXS_RING_COUNT       64

/* Largest 802.11 frame the receive path will hand upwards. */
#define AR9485_MAX_RX_FRAME         2400

/*
 * Hardware key cache.  Entries 0..3 are the four default (group) key IDs
 * 802.11 defines; the pairwise key negotiated with the AP goes above them.
 */
#define AR9485_KEY_PAIRWISE         4
#define AR9485_KEY_CACHE_SIZE       128

/* Background chip-worker commands. */
#define AR9485_CMD_NONE             0
#define AR9485_CMD_SCAN             1
#define AR9485_CMD_CONNECT          2
#define AR9485_CMD_DISCONNECT       3

typedef struct _AR9485_RX_BUFFER
{
    PVOID VirtualAddress;
    NDIS_PHYSICAL_ADDRESS PhysicalAddress;
} AR9485_RX_BUFFER, *PAR9485_RX_BUFFER;

typedef struct _AR9485_BSS
{
    DOT11_MAC_ADDRESS Bssid;
    ULONG ChannelMHz;
    LONG Rssi;
    USHORT BeaconPeriod;
    USHORT CapabilityInformation;
    ULONG IeLength;
    UCHAR Ies[AR9485_MAX_BSS_IE_SIZE];
} AR9485_BSS, *PAR9485_BSS;

/*
 * One in-flight transmit.  Descriptor and frame live in separate DMA
 * allocations because the hardware reads them independently and the frame
 * buffer must stay writable by the CPU while the descriptor does not.
 */
typedef struct _AR9485_TX_SLOT
{
    PVOID                   Descriptor;
    NDIS_PHYSICAL_ADDRESS   DescriptorPa;
    PVOID                   Frame;
    NDIS_PHYSICAL_ADDRESS   FramePa;
    /* The NBL to complete when the hardware reports this frame done, or
     * NULL for a frame the driver itself originated (auth, assoc, deauth). */
    PNET_BUFFER_LIST        Nbl;
    BOOLEAN                 InUse;
} AR9485_TX_SLOT, *PAR9485_TX_SLOT;

typedef enum _AR9485_MLME_STATE
{
    AR9485MlmeIdle = 0,
    AR9485MlmeJoining,
    AR9485MlmeAuthenticating,
    AR9485MlmeAssociating,
    AR9485MlmeAssociated
} AR9485_MLME_STATE;

typedef struct _AR9485_ADAPTER
{
    NDIS_HANDLE             MiniportAdapterHandle;
    NDIS_HANDLE             NdisMiniportDriverHandle;
    PDEVICE_OBJECT          PhysicalDeviceObject;

    /* PCI identity */
    USHORT                  VendorId;
    USHORT                  DeviceId;
    UCHAR                   RevisionId;

    /* BAR0 mapping */
    PHYSICAL_ADDRESS        IoAddress;
    ULONG                   IoLength;
    PVOID                   IoBase;

    /* Chip revision read from AR_SREV at init */
    ULONG                   SregRaw;
    ULONG                   MacVersion;
    ULONG                   MacRevision;

    /* Permanent MAC address recovered from the card's EEPROM/OTP in
     * Phase 2a.  MacAddressValid is FALSE until the restore has both
     * succeeded and produced a usable unicast address; the miniport
     * refuses to initialize in that case rather than report a made-up
     * one.  CurrentMacAddress tracks PermanentMacAddress until Phase 3
     * lets an upper layer override it. */
    UCHAR                   PermanentMacAddress[6];
    UCHAR                   CurrentMacAddress[6];
    BOOLEAN                 MacAddressValid;
    UCHAR                   EepromVersion;
    UCHAR                   TemplateVersion;

    /* Interrupt resource */
    ULONG                   InterruptVector;
    KIRQL                   InterruptLevel;
    KAFFINITY               InterruptAffinity;
    KINTERRUPT_MODE         InterruptModeType;
    BOOLEAN                 InterruptShared;
    BOOLEAN                 HasMessageInterrupt;
    NDIS_HANDLE             InterruptHandle;

    /* Phase 3b: opaque struct ath_hw (ath9k/hw_min.h) holding the
     * register-table wiring and current channel.  Allocated with
     * ar9485_hw_context_size() bytes so the ath9k headers stay out of
     * the miniport translation units. */
    PVOID                   HwContext;
    BOOLEAN                 PhyUp;
    USHORT                  CurrentChannelMHz;
    ULONG                   CurrentOperationMode;

    AR9485_RX_BUFFER        RxBuffers[AR9485_RX_BUFFER_COUNT];
    ULONG                   RxBufferCount;
    AR9485_BSS              Bss[AR9485_MAX_BSS];
    ULONG                   BssCount;
    BOOLEAN                 ScanInProgress;

    /*
     * The background chip worker.  Everything that touches the radio -
     * scanning, join/auth/assoc, harvesting the RX ring, reaping transmit
     * status - runs on this one thread, so no two of them can be halfway
     * through a PHY reset at the same time.  ChipBusy/ChipIdleEvent are
     * what the bring-up lab waits on before claiming the chip.
     */
    PVOID                   ChipThread;
    KEVENT                  ChipWake;
    KEVENT                  ChipIdleEvent;
    volatile LONG           ChipCommand;
    volatile LONG           ChipBusy;
    volatile LONG           ChipThreadStop;

    /* Transmit ring and its DMA status ring. */
    AR9485_TX_SLOT          TxSlots[AR9485_TX_SLOT_COUNT];
    ULONG                   TxHead;
    ULONG                   TxTail;
    ULONG                   TxPending;
    NDIS_SPIN_LOCK          TxLock;
    PVOID                   TxStatusRing;
    NDIS_PHYSICAL_ADDRESS   TxStatusRingPa;
    ULONG                   TxStatusIndex;
    BOOLEAN                 TxInitialized;
    BOOLEAN                 TxReady;

    /* Receive indication.  RxIndex is the next buffer expected to complete:
     * the AR9300 EDMA receive queue is a FIFO, so buffers retire in the
     * order they were handed to AR_LP_RXDP. */
    NDIS_HANDLE             RxNblPool;
    ULONG                   RxIndex;
    BOOLEAN                 RxArmed;

    ULONG64                 TxFrameCount;
    ULONG64                 TxFailureCount;
    ULONG64                 RxFrameCount;
    /* Frames the ring drain threw away before they were ever parsed: a bad
     * descriptor, a split frame, or AR_RxFrameOK clear (CRC/PHY junk that
     * got past the address filter).  Kept apart from the crypto drops below
     * because conflating the two is what made "InErr climbs ~25/s" look like
     * evidence about the group key when it is really the noise floor of a
     * busy 2.4 GHz channel. */
    ULONG64                 RxErrorCount;
    ULONG64                 RxCryptoErrorCount;
    /* Indications, split by destination, so InBc/InMc upstream mean
     * something.  Reporting every receive as a unicast (which this did) makes
     * "InBc = 0" true by construction and useless as evidence. */
    ULONG64                 RxUnicastCount;
    ULONG64                 RxMulticastCount;
    ULONG64                 RxBroadcastCount;
    /* Data-frame drop reasons while associated, for the log ring. */
    ULONG                   RxDropDecrypt;
    ULONG                   RxDropKeyMiss;
    ULONG                   RxDropMichael;
    ULONG                   RxDropQos;
    ULONG                   RxDropSubtype;
    ULONG                   RxDropShort;
    /* Group-TKIP software Michael path (receive.c): frames that reached it,
     * frames it passed, and frames it could not check for want of a key. */
    ULONG                   RxGroupTkipCount;
    ULONG                   RxGroupMicOkCount;
    ULONG                   RxGroupNoKeyCount;
    /* Group-addressed data frames from our own BSS that the hardware handed
     * over with AR_RxFrameOK clear -- i.e. that died before the crypto path
     * ever saw them. */
    ULONG                   RxHwErrGroupCount;
    /* Throttle for the receive-path summary the statistics query drops into
     * the log ring. */
    ULONG64                 LastStatsLogTime;

    /*
     * Association configuration, programmed by nwifi through the
     * OID_DOT11_* set surface before it issues OID_DOT11_CONNECT_REQUEST.
     */
    DOT11_SSID              DesiredSsid;
    BOOLEAN                 HaveDesiredBssid;
    UCHAR                   DesiredBssid[DOT11_ADDR_LEN];
    ULONG                   AuthAlgorithm;
    ULONG                   UnicastCipher;
    ULONG                   MulticastCipher;
    ULONG                   PacketFilter;

    /* One 802.11 sequence counter for everything this station transmits.
     * The hardware preserves what we write (AR_STA_ID1_PRESERVE_SEQNUM), so
     * a frame that leaves without a number here leaves with zero, and an AP
     * discards the second such frame as a duplicate. */
    LONG                    SequenceNumber;

    /* MLME state and the BSS currently being joined or held. */
    AR9485_MLME_STATE       MlmeState;
    UCHAR                   Bssid[DOT11_ADDR_LEN];
    USHORT                  AssociationId;
    USHORT                  BssChannelMHz;
    USHORT                  BssCapability;
    UCHAR                   BssRates[16];
    ULONG                   BssRateCount;

    /*
     * Management-frame rendezvous.  The receive path parses the response
     * and sets the flag; the MLME, running on the same thread between RX
     * polls, picks it up.  Both live on the chip worker, so a plain
     * volatile flag is the whole synchronisation.
     */
    volatile LONG           AuthResponseSeen;
    USHORT                  AuthStatus;
    volatile LONG           AssocResponseSeen;
    USHORT                  AssocStatus;
    /* Bodies of the association exchange, reported with the completion
     * (uAssocReqOffset/uAssocRespOffset): nwifi's supplicant must repeat
     * our RSN element byte for byte in the 4-way handshake. */
    UCHAR                   AssocRequestBody[128];
    ULONG                   AssocRequestLength;
    UCHAR                   AssocResponseBody[256];
    ULONG                   AssocResponseLength;
    volatile LONG           DeauthSeen;
    USHORT                  DeauthReason;

    /* Interrupt time of the last beacon from the BSS we hold.  An AP that
     * goes away, or a station carried out of range, never sends a
     * deauthentication; missing beacons is the only evidence there is. */
    ULONG64                 LastBeaconTime;

    /* Shadow of the hardware key cache. */
    BOOLEAN                 PairwiseKeyValid;
    /* Next CCMP packet number for the pairwise key.  ath9k hardware does
     * not generate the CCMP IV (mac80211 sets IEEE80211_KEY_FLAG_GENERATE_IV
     * so the stack fills it); the MAC only encrypts with the PN it finds in
     * the frame.  48 bits, per key, starts over when the key is installed. */
    ULONG64                 PairwisePn;
    BOOLEAN                 GroupKeyValid;
    UCHAR                   GroupKeyId;
    /* RX Michael key of the group TKIP key: the hardware cannot verify the
     * MIC on the group path (key search misses, see receive.c), so software
     * does, the way mac80211 does. */
    UCHAR                   GroupRxMicKey[8];
    BOOLEAN                 GroupRxMicValid;

    LONG                    Flags;
} AR9485_ADAPTER, *PAR9485_ADAPTER;

/* Default bring-up channel: 2412 MHz (2.4 GHz channel 1). */
#define AR9485_DEFAULT_CHANNEL_MHZ  2412

/* Read/write the memory-mapped register file (BAR0). */
FORCEINLINE ULONG
AR9485_READ_REG(_In_ PAR9485_ADAPTER Adapter, _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)Adapter->IoBase + Offset));
}

FORCEINLINE VOID
AR9485_WRITE_REG(_In_ PAR9485_ADAPTER Adapter, _In_ ULONG Offset, _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Adapter->IoBase + Offset), Value);
}

/* ath9k/hw_reset.c - PHY/MAC bring-up, opaque context (see hw_min.h). */
SIZE_T ar9485_hw_context_size(void);
bool ar9485_hw_start(void *hwctx, void *bar0_base, u32 bar0_len,
                     u16 devid, u32 macVersion, u16 macRev, u16 channel_mhz);
/* Hand the EDMA transmit status ring to the reset path, which re-programs
 * AR_Q_STATUS_RING_START/END on every bring-up the way ath9k_hw_set_dma()
 * does.  Writing those registers after a reset has returned is not enough;
 * see the comment on ar9485_hw_set_dma() and docs/asus.txt. */
void ar9485_hw_set_txstatus_ring(void *hwctx, u32 ts_paddr_start,
                                 u32 ring_bytes);

NDIS_STATUS AR9485InitializeReceiver(_In_ PAR9485_ADAPTER Adapter);
VOID AR9485ShutdownReceiver(_In_ PAR9485_ADAPTER Adapter);
NDIS_STATUS AR9485StartScan(_In_ PAR9485_ADAPTER Adapter);
NDIS_STATUS AR9485BuildBssList(_In_ PAR9485_ADAPTER Adapter,
                              _In_ PNDIS_OID_REQUEST Request);

/* receive.c - arm the RX ring and drain it.  Both run on the chip worker. */
VOID AR9485ArmReceiver(_In_ PAR9485_ADAPTER Adapter);
ULONG AR9485PollReceive(_In_ PAR9485_ADAPTER Adapter);
VOID AR9485RunScan(_In_ PAR9485_ADAPTER Adapter);

/* send.c - the AR9300 transmit engine. */
NDIS_STATUS AR9485InitializeTransmitter(_In_ PAR9485_ADAPTER Adapter);
VOID AR9485ShutdownTransmitter(_In_ PAR9485_ADAPTER Adapter);
VOID AR9485ResetTransmitQueue(_In_ PAR9485_ADAPTER Adapter);
VOID AR9485ReapTransmitStatus(_In_ PAR9485_ADAPTER Adapter);
VOID AR9485FlushTransmitQueue(_In_ PAR9485_ADAPTER Adapter);

/* Hand one fully built 802.11 frame to the hardware.  KeyIndex is
 * AR9485_KEY_NONE for a frame that must go out in the clear. */
#define AR9485_KEY_NONE  ((ULONG)-1)
NDIS_STATUS AR9485TransmitFrame(_In_ PAR9485_ADAPTER Adapter,
                                _In_reads_bytes_(Length) PUCHAR Frame,
                                _In_ ULONG Length,
                                _In_ ULONG KeyIndex,
                                _In_ ULONG RateCode,
                                _In_ BOOLEAN NoAck,
                                _In_opt_ PNET_BUFFER_LIST Nbl);

/* mlme.c - join, authenticate, associate, and the chip worker itself. */
NDIS_STATUS AR9485StartChipWorker(_In_ PAR9485_ADAPTER Adapter);
VOID AR9485StopChipWorker(_In_ PAR9485_ADAPTER Adapter);
VOID AR9485QueueCommand(_In_ PAR9485_ADAPTER Adapter, _In_ LONG Command);
VOID AR9485MlmeReceiveManagement(_In_ PAR9485_ADAPTER Adapter,
                                 _In_reads_bytes_(Length) PUCHAR Frame,
                                 _In_ ULONG Length);
VOID AR9485ProgramMacState(_In_ PAR9485_ADAPTER Adapter);
NDIS_STATUS AR9485SetCipherKey(_In_ PAR9485_ADAPTER Adapter,
                               _In_ ULONG Index,
                               _In_ ULONG CipherAlgorithm,
                               _In_reads_bytes_opt_(KeyLength) PUCHAR Key,
                               _In_ ULONG KeyLength,
                               _In_reads_bytes_opt_(DOT11_ADDR_LEN) PUCHAR MacAddress);
VOID AR9485IndicateLinkState(_In_ PAR9485_ADAPTER Adapter, _In_ BOOLEAN Connected);

/* driver.c */
extern NDIS_HANDLE g_NdisMiniportDriverHandle;

/* init.c */
NDIS_STATUS NTAPI
AR9485MiniportInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters);

VOID NTAPI
AR9485MiniportHaltEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction);

VOID NTAPI
AR9485MiniportDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject);

NDIS_STATUS NTAPI
AR9485MiniportPause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters);

NDIS_STATUS NTAPI
AR9485MiniportRestart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters);

VOID NTAPI
AR9485MiniportDevicePnPEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent);

VOID NTAPI
AR9485MiniportShutdownEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction);

/* interrupt.c */
NDIS_STATUS
AR9485RegisterInterrupt(_In_ PAR9485_ADAPTER Adapter);

VOID
AR9485UnregisterInterrupt(_In_ PAR9485_ADAPTER Adapter);

BOOLEAN NTAPI
AR9485Isr(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _Out_ PBOOLEAN QueueDefaultInterruptDpc,
    _Out_ PULONG TargetProcessors);

VOID NTAPI
AR9485InterruptDpc(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ PVOID MiniportDpcContext,
    _In_ PVOID ReceiveThrottleParameters,
    _In_ PVOID NdisReserved2);

VOID NTAPI
AR9485DisableInterrupt(
    _In_ NDIS_HANDLE MiniportInterruptContext);

VOID NTAPI
AR9485EnableInterrupt(
    _In_ NDIS_HANDLE MiniportInterruptContext);

/* oid.c */
NDIS_STATUS NTAPI
AR9485OidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest);

VOID NTAPI
AR9485CancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId);

/* send.c */
VOID NTAPI
AR9485SendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags);

VOID NTAPI
AR9485CancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId);

/* receive.c */
VOID NTAPI
AR9485ReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags);

/* lab.c - user-mode bring-up lab (see sdk/include/reactos/ar9485_lab.h).
 * Compiled in only when the AR9485_LAB build option is on; the stubs keep
 * the call sites free of #ifdef noise. */
#ifdef AR9485_LAB

NDIS_STATUS
AR9485LabCreateControlDevice(_In_ NDIS_HANDLE MiniportDriverHandle);

VOID
AR9485LabDeleteControlDevice(VOID);

VOID
AR9485LabAttachAdapter(_In_ PAR9485_ADAPTER Adapter);

VOID
AR9485LabDetachAdapter(_In_ PAR9485_ADAPTER Adapter);

/* TRUE while a lab tool holds the chip.  The scan path must stand down: a
 * scan resets the PHY across thirteen channels, and an experiment racing
 * that measures nothing. */
BOOLEAN
AR9485LabOwnsChip(VOID);

/* receive.c internals the lab can invoke through AR9485LAB_OP_CALL_HW, so a
 * user-mode sequence can be compared against the kernel's own. */
VOID
AR9485LabQueueRx(_In_ PAR9485_ADAPTER Adapter);

ULONG
AR9485LabHarvestRx(_In_ PAR9485_ADAPTER Adapter, _In_ ULONG Frequency);

#else /* !AR9485_LAB */

#define AR9485LabCreateControlDevice(h)     NDIS_STATUS_SUCCESS
#define AR9485LabDeleteControlDevice()      ((VOID)0)
#define AR9485LabAttachAdapter(a)           ((VOID)0)
#define AR9485LabDetachAdapter(a)           ((VOID)0)
#define AR9485LabOwnsChip()                 FALSE

#endif /* AR9485_LAB */

/* log.c - every DPRINT1 in this driver also lands in a ring the lab can
 * read (AR9485LAB_HW_LOG), because runtime DbgPrint is invisible on the
 * KDNET box.  The macro swap must follow every header that declares
 * DbgPrint; log.c #undefs it to reach the real one. */
ULONG __cdecl AR9485DbgPrint(_In_ PCSTR Format, ...);
ULONG AR9485LogCopy(_Out_writes_bytes_(Length) PUCHAR Buffer, _In_ ULONG Length);
#define DbgPrint AR9485DbgPrint

#endif /* _AR9485_H_ */
