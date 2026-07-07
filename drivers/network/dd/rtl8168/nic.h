/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Realtek 8168/8111 driver
 * FILE:        nic.h
 * PURPOSE:     RTL8168/8111 NDIS5 miniport definitions
 *
 * Structurally modeled on drivers/network/dd/rtl8139 (Cameron Gutman, 2013).
 * Chip-specific behavior ported from Linux r8169.
 */

#ifndef _RTL8168_PCH_
#define _RTL8168_PCH_

#include <ndis.h>

#include "rtlhw.h"

#define ADAPTER_TAG         '8ltr'
#define RESOURCE_LIST_TAG   'Rltr'
#define RX_BUFFER_TAG       'Brtr'
#define RING_TAG            'Grtr'

#define MAX_RESET_ATTEMPTS  100
#define DRIVER_VERSION      1

typedef struct _RTL_ADAPTER {
    NDIS_HANDLE MiniportAdapterHandle;
    NDIS_SPIN_LOCK Lock;

    /* MMIO BAR (or IO port BAR as fallback) */
    PUCHAR IoBase;
    BOOLEAN IoBaseIsMmio;
    NDIS_PHYSICAL_ADDRESS MmioPhysical;
    ULONG MmioLength;
    ULONG IoPortStart;
    ULONG IoPortLength;

    /* Interrupt */
    ULONG InterruptVector;
    ULONG InterruptLevel;
    BOOLEAN InterruptShared;
    ULONG InterruptFlags;
    NDIS_MINIPORT_INTERRUPT Interrupt;
    BOOLEAN InterruptRegistered;
    USHORT InterruptMask;
    USHORT InterruptPending;

    /* Addresses */
    UCHAR PermanentMacAddress[IEEE_802_ADDR_LENGTH];
    UCHAR CurrentMacAddress[IEEE_802_ADDR_LENGTH];
    struct {
        UCHAR MacAddress[IEEE_802_ADDR_LENGTH];
    } MulticastList[MAXIMUM_MULTICAST_ADDRESSES];

    /* Chip identification */
    ULONG TxConfigRaw;
    RTL_MAC_VER MacVersion;
    BOOLEAN PhyIsGigabit;
    BOOLEAN OcpMdioRedirect;        /* TRUE on 8168G+ -- MDIO writes route through GPHY OCP */
    USHORT OcpBase;                 /* current PHY OCP base (mirrors Linux tp->ocp_base) */

    /* TX ring */
    PRTL_DESC TxRing;
    NDIS_PHYSICAL_ADDRESS TxRingPa;
    PUCHAR TxBuffers;                       /* bounce buffers, one slot each */
    NDIS_PHYSICAL_ADDRESS TxBuffersPa;
    ULONG TxProducer;
    ULONG TxConsumer;
    BOOLEAN TxFull;

    /* RX ring */
    PRTL_DESC RxRing;
    NDIS_PHYSICAL_ADDRESS RxRingPa;
    PUCHAR RxBuffers;
    NDIS_PHYSICAL_ADDRESS RxBuffersPa;
    ULONG RxConsumer;

    /* Link state */
    ULONG LinkSpeedMbps;
    ULONG MediaState;
    BOOLEAN LinkChange;
    UCHAR LastPhyStatus;

    /* OID state */
    ULONG PacketFilter;

    /* Error-recovery state (MiniportCheckForHang / MiniportReset) */
    BOOLEAN HwHang;                 /* SYSERR seen, or VER_11 RxFIFOOver wedge */
    BOOLEAN CheckForHangTxPending;  /* TX queue was non-empty at the last poll */
    ULONG CheckForHangTxOk;         /* TransmitOk snapshot from the last poll */

    /* Stats */
    ULONG ReceiveOk;
    ULONG TransmitOk;
    ULONG ReceiveError;
    ULONG TransmitError;
    ULONG ReceiveNoBufferSpace;
    ULONG ReceiveCrcError;
    ULONG TransmitOneCollision;
    ULONG TransmitMoreCollisions;
} RTL_ADAPTER, *PRTL_ADAPTER;

/* MMIO accessors -- the chip exposes the same register window over IO or MMIO,
 * so we read/write whichever the OS handed us in MiniportInitialize. */

FORCEINLINE
UCHAR
RtlReadReg8(IN PRTL_ADAPTER A, IN ULONG Off)
{
    UCHAR v;
    if (A->IoBaseIsMmio)
        NdisReadRegisterUchar((PUCHAR)(A->IoBase + Off), &v);
    else
        NdisRawReadPortUchar(A->IoBase + Off, &v);
    return v;
}

FORCEINLINE
USHORT
RtlReadReg16(IN PRTL_ADAPTER A, IN ULONG Off)
{
    USHORT v;
    if (A->IoBaseIsMmio)
        NdisReadRegisterUshort((PUSHORT)(A->IoBase + Off), &v);
    else
        NdisRawReadPortUshort(A->IoBase + Off, &v);
    return v;
}

FORCEINLINE
ULONG
RtlReadReg32(IN PRTL_ADAPTER A, IN ULONG Off)
{
    ULONG v;
    if (A->IoBaseIsMmio)
        NdisReadRegisterUlong((PULONG)(A->IoBase + Off), &v);
    else
        NdisRawReadPortUlong(A->IoBase + Off, &v);
    return v;
}

FORCEINLINE
VOID
RtlWriteReg8(IN PRTL_ADAPTER A, IN ULONG Off, IN UCHAR Val)
{
    if (A->IoBaseIsMmio)
        NdisWriteRegisterUchar((PUCHAR)(A->IoBase + Off), Val);
    else
        NdisRawWritePortUchar(A->IoBase + Off, Val);
}

FORCEINLINE
VOID
RtlWriteReg16(IN PRTL_ADAPTER A, IN ULONG Off, IN USHORT Val)
{
    if (A->IoBaseIsMmio)
        NdisWriteRegisterUshort((PUSHORT)(A->IoBase + Off), Val);
    else
        NdisRawWritePortUshort(A->IoBase + Off, Val);
}

FORCEINLINE
VOID
RtlWriteReg32(IN PRTL_ADAPTER A, IN ULONG Off, IN ULONG Val)
{
    if (A->IoBaseIsMmio)
        NdisWriteRegisterUlong((PULONG)(A->IoBase + Off), Val);
    else
        NdisRawWritePortUlong(A->IoBase + Off, Val);
}

/* indirect.c -- ERI / EPHY / OCP / MDIO indirect register access */
VOID    NTAPI RtlEriWrite(IN PRTL_ADAPTER Adapter, IN ULONG Addr,
                          IN ULONG Mask, IN ULONG Val);
ULONG   NTAPI RtlEriRead(IN PRTL_ADAPTER Adapter, IN ULONG Addr);
VOID    NTAPI RtlEriSetBits(IN PRTL_ADAPTER Adapter, IN ULONG Addr, IN ULONG Bits);
VOID    NTAPI RtlEriClearBits(IN PRTL_ADAPTER Adapter, IN ULONG Addr, IN ULONG Bits);
VOID    NTAPI RtlW0w1Eri(IN PRTL_ADAPTER A, IN ULONG Addr, IN ULONG Set, IN ULONG Clr);

VOID    NTAPI RtlEphyWrite(IN PRTL_ADAPTER A, IN ULONG Reg, IN USHORT Val);
USHORT  NTAPI RtlEphyRead(IN PRTL_ADAPTER A, IN ULONG Reg);

/* EPHY init entry (mirrors Linux struct ephy_info). */
typedef struct _EPHY_INFO {
    ULONG  Offset;
    USHORT Mask;
    USHORT Bits;
} EPHY_INFO, *PEPHY_INFO;
VOID    NTAPI RtlEphyInit(IN PRTL_ADAPTER A, IN const EPHY_INFO *e, IN ULONG Count);

VOID    NTAPI RtlPhyOcpWrite(IN PRTL_ADAPTER A, IN ULONG Reg, IN ULONG Data);
USHORT  NTAPI RtlPhyOcpRead(IN PRTL_ADAPTER A, IN ULONG Reg);
VOID    NTAPI RtlMacOcpWrite(IN PRTL_ADAPTER A, IN ULONG Reg, IN ULONG Data);
USHORT  NTAPI RtlMacOcpRead(IN PRTL_ADAPTER A, IN ULONG Reg);
VOID    NTAPI RtlMacOcpModify(IN PRTL_ADAPTER A, IN ULONG Reg, IN USHORT Mask, IN USHORT Set);

/* MDIO access (R_PHYAR direct on <8168G, OCP-redirected on 8168G+). */
VOID    NTAPI RtlMdioWrite(IN PRTL_ADAPTER A, IN ULONG Reg, IN USHORT Val);
USHORT  NTAPI RtlMdioRead(IN PRTL_ADAPTER A, IN ULONG Reg);
VOID    NTAPI RtlPhyWritePaged(IN PRTL_ADAPTER A, IN USHORT Page,
                               IN ULONG Reg, IN USHORT Val);
USHORT  NTAPI RtlPhyReadPaged(IN PRTL_ADAPTER A, IN USHORT Page, IN ULONG Reg);
VOID    NTAPI RtlPhyModifyPaged(IN PRTL_ADAPTER A, IN USHORT Page,
                                IN ULONG Reg, IN USHORT Mask, IN USHORT Set);

/* chipinit.c */
VOID    NTAPI RtlHwStartChipSpecific(IN PRTL_ADAPTER A);
VOID    NTAPI RtlHwAspmClkReqDisable(IN PRTL_ADAPTER A);
VOID    NTAPI RtlEnableExitL1(IN PRTL_ADAPTER A);

/* phy.c */
VOID    NTAPI RtlHwPhyConfig(IN PRTL_ADAPTER A);
VOID    NTAPI RtlPllPowerUp(IN PRTL_ADAPTER A);
VOID    NTAPI RtlPllPowerDown(IN PRTL_ADAPTER A);

/* hardware.c */
NDIS_STATUS NTAPI NICSoftReset(IN PRTL_ADAPTER Adapter);
VOID        NTAPI NICInitializeHw(IN PRTL_ADAPTER Adapter);
VOID        NTAPI Rtl8168EpStopCmac(IN PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICDetectChipVersion(IN PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICGetPermanentMacAddress(IN PRTL_ADAPTER Adapter, OUT PUCHAR MacAddress);
NDIS_STATUS NTAPI NICProgramRings(IN PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICEnableTxRx(IN PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICApplyPacketFilter(IN PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICApplyInterruptMask(IN PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICDisableInterrupts(IN PRTL_ADAPTER Adapter);
USHORT      NTAPI NICInterruptRecognized(IN PRTL_ADAPTER Adapter, OUT PBOOLEAN Recognized);
VOID        NTAPI NICAcknowledgeInterrupts(IN PRTL_ADAPTER Adapter, IN USHORT Status);
VOID        NTAPI NICUpdateLinkStatus(IN PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICTransmitDescriptor(IN PRTL_ADAPTER Adapter,
                                         IN ULONG Index,
                                         IN PHYSICAL_ADDRESS BufferPa,
                                         IN ULONG Length);
NDIS_STATUS NTAPI NICRefillRxDescriptor(IN PRTL_ADAPTER Adapter, IN ULONG Index);

/* info.c */
NDIS_STATUS NTAPI MiniportSetInformation(IN NDIS_HANDLE, IN NDIS_OID, IN PVOID,
                                          IN ULONG, OUT PULONG, OUT PULONG);
NDIS_STATUS NTAPI MiniportQueryInformation(IN NDIS_HANDLE, IN NDIS_OID, IN PVOID,
                                            IN ULONG, OUT PULONG, OUT PULONG);

/* interrupt.c */
VOID NTAPI MiniportISR(OUT PBOOLEAN InterruptRecognized,
                        OUT PBOOLEAN QueueMiniportHandleInterrupt,
                        IN NDIS_HANDLE MiniportAdapterContext);
VOID NTAPI MiniportHandleInterrupt(IN NDIS_HANDLE MiniportAdapterContext);
BOOLEAN NTAPI MiniportCheckForHang(IN NDIS_HANDLE MiniportAdapterContext);

#endif /* _RTL8168_PCH_ */
