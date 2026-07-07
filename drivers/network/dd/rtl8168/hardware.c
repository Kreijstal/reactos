/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Realtek 8168/8111 driver
 * FILE:        hardware.c
 * PURPOSE:     Chip bring-up, descriptor-ring programming, MAC/PHY access
 *
 * Register sequences ported from Linux r8169_main.c (GPLv2).
 */

#include "nic.h"

#define NDEBUG
#include <debug.h>

/* rtl_wait_txrx_fifo_empty -- r8169_main.c:2471.  8168G+ only: wait for
 * TxConfig.TXCFG_EMPTY and MCU RX/TX FIFO-empty flags.  Linux polls each
 * condition every 100us, 42 times. */
static VOID
RtlWaitTxRxFifoEmpty(IN PRTL_ADAPTER Adapter)
{
    UINT i;

    for (i = 0; i < 42; i++)
    {
        if (RtlReadReg32(Adapter, R_TC) & TXCFG_EMPTY)
            break;
        NdisStallExecution(100);
    }
    for (i = 0; i < 42; i++)
    {
        if ((RtlReadReg8(Adapter, R_MCU) & MCU_RXTX_EMPTY) == MCU_RXTX_EMPTY)
            break;
        NdisStallExecution(100);
    }
}

/* rtl_enable_rxdvgate -- r8169_main.c:2496: gate RX-data-valid, give the
 * chip 2ms, then wait for the FIFOs to drain. */
static VOID
RtlEnableRxdvGate(IN PRTL_ADAPTER Adapter)
{
    UINT i;

    RtlWriteReg32(Adapter, R_MISC,
                  RtlReadReg32(Adapter, R_MISC) | MISC_RXDV_GATED_EN);
    for (i = 0; i < 20; i++)
        NdisStallExecution(100);    /* fsleep(2000) */
    RtlWaitTxRxFifoEmpty(Adapter);
}

/*
 * Soft reset with the pre-reset quiesce from Linux rtl8169_cleanup
 * (r8169_main.c:3917): stop RX acceptance, then drain the TX/RX engines in
 * the chip-family-specific way before hitting CmdReset.
 *
 * Called at PASSIVE from MiniportInitialize and at DISPATCH from
 * MiniportReset, so all waits are bounded NdisStallExecution loops.
 */
NDIS_STATUS
NTAPI
NICSoftReset (
    IN PRTL_ADAPTER Adapter
    )
{
    UINT attempts;
    UINT i;

    /* rtl_rx_close */
    RtlWriteReg32(Adapter, R_RC,
                  RtlReadReg32(Adapter, R_RC) & ~RX_ACCEPT_MASK);

    if (Adapter->MacVersion == RTL_MAC_VER_28 ||
        Adapter->MacVersion == RTL_MAC_VER_31)
    {
        /* DP parts: wait for the normal-priority-queue poll bit to clear.
         * Linux polls every 20us, 2000 times. */
        for (i = 0; i < 2000; i++)
        {
            if (!(RtlReadReg8(Adapter, R_TXPOLL) & B_TXP_NPQ))
                break;
            NdisStallExecution(20);
        }
    }
    else if (Adapter->MacVersion >= RTL_MAC_VER_34 &&
             Adapter->MacVersion <= RTL_MAC_VER_38)
    {
        /* StopReq, then wait for TxConfig.TXCFG_EMPTY (100us x 666). */
        RtlWriteReg8(Adapter, R_CMD,
                     RtlReadReg8(Adapter, R_CMD) | B_CMD_STOPREQ);
        for (i = 0; i < 666; i++)
        {
            if (RtlReadReg32(Adapter, R_TC) & TXCFG_EMPTY)
                break;
            NdisStallExecution(100);
        }
    }
    else if (Adapter->MacVersion >= RTL_MAC_VER_40)
    {
        RtlEnableRxdvGate(Adapter);
        for (i = 0; i < 20; i++)
            NdisStallExecution(100);    /* fsleep(2000) */
    }
    else
    {
        RtlWriteReg8(Adapter, R_CMD,
                     RtlReadReg8(Adapter, R_CMD) | B_CMD_STOPREQ);
        NdisStallExecution(100);
    }

    /* rtl_hw_reset: CmdReset, poll clear every 100us, 100 times. */
    RtlWriteReg8(Adapter, R_CMD, B_CMD_RESET);

    for (attempts = 0; attempts < MAX_RESET_ATTEMPTS; attempts++)
    {
        if (!(RtlReadReg8(Adapter, R_CMD) & B_CMD_RESET))
            return NDIS_STATUS_SUCCESS;

        NdisStallExecution(100);
    }

    NDIS_DbgPrint(MIN_TRACE, ("RTL8168: soft reset timed out\n"));
    return NDIS_STATUS_FAILURE;
}

/* r8168g_wait_ll_share_fifo_ready -- r8169_main.c:4990: poll MCU
 * LINK_LIST_RDY every 100us, 42 times. */
static VOID
Rtl8168gWaitLlShareFifoReady(IN PRTL_ADAPTER Adapter)
{
    UINT i;

    for (i = 0; i < 42; i++)
    {
        if (RtlReadReg8(Adapter, R_MCU) & MCU_LINK_LIST_RDY)
            return;
        NdisStallExecution(100);
    }
    NDIS_DbgPrint(MIN_TRACE, ("RTL8168: link-list FIFO not ready\n"));
}

/* rtl8168ep_stop_cmac -- r8169_main.c:1193.  Linux allows up to 100s for the
 * CMAC TX path to drain (50ms x 2000); it completes in microseconds in
 * practice, so we bound the poll at 50ms to stay DISPATCH-safe. */
VOID
NTAPI
Rtl8168EpStopCmac (
    IN PRTL_ADAPTER Adapter
    )
{
    UINT i;

    RtlWriteReg8(Adapter, R_IBCR2, RtlReadReg8(Adapter, R_IBCR2) & ~0x01);
    for (i = 0; i < 500; i++)
    {
        if (RtlReadReg8(Adapter, R_IBISR0) & 0x20)
            break;
        NdisStallExecution(100);
    }
    RtlWriteReg8(Adapter, R_IBISR0, RtlReadReg8(Adapter, R_IBISR0) | 0x20);
    RtlWriteReg8(Adapter, R_IBCR0, RtlReadReg8(Adapter, R_IBCR0) & ~0x01);
}

/*
 * rtl_hw_initialize -- r8169_main.c:5096.  Takes the 8168G-and-later MCU out
 * of OOB (management) mode before the first soft reset.  Without this the
 * MAC engine stays owned by the management firmware and RX never starts.
 * Runs at PASSIVE from MiniportInitialize only.
 */
VOID
NTAPI
NICInitializeHw (
    IN PRTL_ADAPTER Adapter
    )
{
    if (Adapter->MacVersion < RTL_MAC_VER_40)
        return;

    /* rtl_hw_init_8168ep = stop_cmac + rtl_hw_init_8168g */
    if (Adapter->MacVersion >= RTL_MAC_VER_51)
        Rtl8168EpStopCmac(Adapter);

    /* rtl_hw_init_8168g -- r8169_main.c:5064 */
    RtlEnableRxdvGate(Adapter);

    RtlWriteReg8(Adapter, R_CMD,
                 RtlReadReg8(Adapter, R_CMD) & ~(B_CMD_TXENB | B_CMD_RXENB));
    NdisMSleep(1000);   /* msleep(1) */
    RtlWriteReg8(Adapter, R_MCU,
                 RtlReadReg8(Adapter, R_MCU) & ~MCU_NOW_IS_OOB);

    RtlMacOcpModify(Adapter, 0xE8DE, 0x4000, 0);        /* clear BIT(14) */
    Rtl8168gWaitLlShareFifoReady(Adapter);

    RtlMacOcpModify(Adapter, 0xE8DE, 0, 0x8000);        /* set BIT(15) */
    Rtl8168gWaitLlShareFifoReady(Adapter);
}

/*
 * Decode the chip version from TxConfig (R_TC) bits 30..23.
 * Unknown silicon fails detection -- MiniportInitialize then fails with
 * NDIS_STATUS_ADAPTER_NOT_FOUND, exactly like Linux fails the probe on an
 * unknown XID.
 *
 * Table transcribed from mac_info[] in rtl8169_get_mac_version()
 * (r8169_main.c:2024), restricted to the 8168/8411 families this driver
 * drives (Linux XID values shifted left by 20 to match raw TxConfig).
 * Order matters: first hit wins, exactly as in Linux.
 */
NDIS_STATUS
NTAPI
NICDetectChipVersion (
    IN PRTL_ADAPTER Adapter
    )
{
    static const struct {
        ULONG mask;
        ULONG val;
        RTL_MAC_VER ver;
        const char *name;
    } table[] = {
        /* 8168EP family. */
        { 0x7CF00000, 0x50200000, RTL_MAC_VER_51, "RTL8168EP/8111EP" },

        /* 8168H family. */
        { 0x7CF00000, 0x54100000, RTL_MAC_VER_46, "RTL8168H/8111H" },

        /* 8168G family. */
        { 0x7CF00000, 0x5C800000, RTL_MAC_VER_44, "RTL8411B" },
        { 0x7CF00000, 0x50900000, RTL_MAC_VER_42, "RTL8168GU/8111GU" },
        { 0x7CF00000, 0x4C000000, RTL_MAC_VER_40, "RTL8168G/8111G" },

        /* 8168F family. */
        { 0x7C800000, 0x48800000, RTL_MAC_VER_38, "RTL8411" },
        { 0x7CF00000, 0x48100000, RTL_MAC_VER_36, "RTL8168F/8111F" },
        { 0x7CF00000, 0x48000000, RTL_MAC_VER_35, "RTL8168F/8111F" },

        /* 8168E family. */
        { 0x7C800000, 0x2C800000, RTL_MAC_VER_34, "RTL8168E-VL/8111E-VL" },
        { 0x7CF00000, 0x2C100000, RTL_MAC_VER_32, "RTL8168E/8111E" },
        { 0x7C800000, 0x2C000000, RTL_MAC_VER_33, "RTL8168E/8111E" },

        /* 8168D family. */
        { 0x7CF00000, 0x28100000, RTL_MAC_VER_25, "RTL8168D/8111D" },
        { 0x7C800000, 0x28000000, RTL_MAC_VER_26, "RTL8168D/8111D" },

        /* 8168DP family. */
        { 0x7CF00000, 0x28A00000, RTL_MAC_VER_28, "RTL8168DP/8111DP" },
        { 0x7CF00000, 0x28B00000, RTL_MAC_VER_31, "RTL8168DP/8111DP" },

        /* 8168C family. */
        { 0x7CF00000, 0x3C900000, RTL_MAC_VER_23, "RTL8168CP/8111CP" },
        { 0x7CF00000, 0x3C800000, RTL_MAC_VER_18, "RTL8168CP/8111CP" },
        { 0x7C800000, 0x3C800000, RTL_MAC_VER_24, "RTL8168CP/8111CP" },
        { 0x7CF00000, 0x3C000000, RTL_MAC_VER_19, "RTL8168C/8111C" },
        { 0x7CF00000, 0x3C200000, RTL_MAC_VER_20, "RTL8168C/8111C" },
        { 0x7CF00000, 0x3C300000, RTL_MAC_VER_21, "RTL8168C/8111C" },
        { 0x7C800000, 0x3C000000, RTL_MAC_VER_22, "RTL8168C/8111C" },

        /* 8168B family. */
        { 0x7C800000, 0x38000000, RTL_MAC_VER_17, "RTL8168B/8111B" },
        { 0x7C800000, 0x30000000, RTL_MAC_VER_11, "RTL8168B/8111B" },
    };
    ULONG i;
    ULONG raw = RtlReadReg32(Adapter, R_TC);

    Adapter->TxConfigRaw = raw;
    Adapter->MacVersion = RTL_MAC_VER_UNKNOWN;
    Adapter->PhyIsGigabit = TRUE;   /* 8168 family is always gigabit */

    for (i = 0; i < ARRAYSIZE(table); i++)
    {
        if ((raw & table[i].mask) == table[i].val)
        {
            Adapter->MacVersion = table[i].ver;

            /* On 8168G and later MDIO is redirected through GPHY OCP.
             * Latch this now: RtlPllPowerUp touches MDIO before
             * RtlHwStartChipSpecific ever runs. */
            if (Adapter->MacVersion >= RTL_MAC_VER_40)
            {
                Adapter->OcpMdioRedirect = TRUE;
                Adapter->OcpBase = OCP_STD_PHY_BASE;
            }

            NDIS_DbgPrint(MIN_TRACE,
                ("RTL8168: detected %s (TxConfig=0x%08lx)\n",
                 table[i].name, raw));
            return NDIS_STATUS_SUCCESS;
        }
    }

    NDIS_DbgPrint(MIN_TRACE,
        ("RTL8168: ERROR: unknown chip version (TxConfig=0x%08lx)\n", raw));
    return NDIS_STATUS_ADAPTER_NOT_FOUND;
}

NDIS_STATUS
NTAPI
NICGetPermanentMacAddress (
    IN PRTL_ADAPTER Adapter,
    OUT PUCHAR MacAddress
    )
{
    UINT i;

    /* MAC0..MAC5 are readable directly even when Cfg9346 is locked. */
    for (i = 0; i < IEEE_802_ADDR_LENGTH; i++)
        MacAddress[i] = RtlReadReg8(Adapter, R_MAC0 + i);

    /* Reject the all-zero / all-ones case -- some chips return that when the
     * PHY hasn't latched the EEPROM yet. */
    if ((MacAddress[0] | MacAddress[1] | MacAddress[2] |
         MacAddress[3] | MacAddress[4] | MacAddress[5]) == 0 ||
        (MacAddress[0] & MacAddress[1] & MacAddress[2] &
         MacAddress[3] & MacAddress[4] & MacAddress[5]) == 0xFF)
    {
        NDIS_DbgPrint(MIN_TRACE, ("RTL8168: MAC address looks invalid\n"));
        return NDIS_STATUS_FAILURE;
    }

    return NDIS_STATUS_SUCCESS;
}

static VOID
RtlInitTxDescriptor(IN PRTL_ADAPTER A, IN ULONG Index)
{
    PRTL_DESC d = &A->TxRing[Index];
    d->opts1 = (Index == TX_DESC_COUNT - 1) ? DESC_EOR : 0;
    d->opts2 = 0;
    d->addr  = A->TxBuffersPa.QuadPart + (ULONGLONG)Index * RX_BUF_SIZE;
}

NDIS_STATUS
NTAPI
NICRefillRxDescriptor (
    IN PRTL_ADAPTER Adapter,
    IN ULONG Index
    )
{
    PRTL_DESC d = &Adapter->RxRing[Index];
    ULONGLONG bufPa = Adapter->RxBuffersPa.QuadPart + (ULONGLONG)Index * RX_BUF_SIZE;
    ULONG opts1 = DESC_OWN | (RX_BUF_SIZE & DESC_LEN_MASK);

    if (Index == RX_DESC_COUNT - 1)
        opts1 |= DESC_EOR;

    d->opts2 = 0;
    d->addr  = bufPa;
    /* opts1 last so the NIC only sees OWN after addr is committed */
    d->opts1 = opts1;
    return NDIS_STATUS_SUCCESS;
}

/* rtl_jumbo_config -- r8169_main.c:2355, non-jumbo branch only (this driver
 * never negotiates an MTU above 1500).  Explicitly clears the jumbo enable
 * bits the BIOS/a previous OS may have left set.  Does its own Cfg9346
 * unlock/lock, as Linux does. */
static VOID
RtlJumboDisable(IN PRTL_ADAPTER Adapter)
{
    RtlWriteReg8(Adapter, R_CFG9346, CFG9346_UNLOCK);

    if (Adapter->MacVersion == RTL_MAC_VER_17)
    {
        /* r8168b_1_hw_jumbo_disable */
        RtlWriteReg8(Adapter, R_CFG4, RtlReadReg8(Adapter, R_CFG4) & ~0x01);
    }
    else if (Adapter->MacVersion >= RTL_MAC_VER_18 &&
             Adapter->MacVersion <= RTL_MAC_VER_26)
    {
        /* r8168c_hw_jumbo_disable */
        RtlWriteReg8(Adapter, R_CFG3,
                     RtlReadReg8(Adapter, R_CFG3) & ~CFG3_JUMBO_EN0);
        RtlWriteReg8(Adapter, R_CFG4,
                     RtlReadReg8(Adapter, R_CFG4) & ~CFG4_JUMBO_EN1);
    }
    else if (Adapter->MacVersion == RTL_MAC_VER_28)
    {
        /* r8168dp_hw_jumbo_disable */
        RtlWriteReg8(Adapter, R_CFG3,
                     RtlReadReg8(Adapter, R_CFG3) & ~CFG3_JUMBO_EN0);
    }
    else if (Adapter->MacVersion >= RTL_MAC_VER_31 &&
             Adapter->MacVersion <= RTL_MAC_VER_33)
    {
        /* r8168e_hw_jumbo_disable */
        RtlWriteReg8(Adapter, R_MAXTXPKTSIZE, 0x3F);
        RtlWriteReg8(Adapter, R_CFG3,
                     RtlReadReg8(Adapter, R_CFG3) & ~CFG3_JUMBO_EN0);
        RtlWriteReg8(Adapter, R_CFG4,
                     RtlReadReg8(Adapter, R_CFG4) & ~0x01);
    }

    RtlWriteReg8(Adapter, R_CFG9346, CFG9346_LOCK);
}

/* rtl_init_rxcfg -- r8169_main.c:2280.  Programs the non-accept RxConfig
 * bits per chip family; the accept bits are applied afterwards by
 * NICApplyPacketFilter. */
static VOID
RtlInitRxConfig(IN PRTL_ADAPTER Adapter)
{
    ULONG rc;

    if (Adapter->MacVersion >= RTL_MAC_VER_11 &&
        Adapter->MacVersion <= RTL_MAC_VER_17)
    {
        rc = RX_FIFO_THRESH | RX_DMA_BURST;
    }
    else if ((Adapter->MacVersion >= RTL_MAC_VER_18 &&
              Adapter->MacVersion <= RTL_MAC_VER_24) ||
             (Adapter->MacVersion >= RTL_MAC_VER_34 &&
              Adapter->MacVersion <= RTL_MAC_VER_36) ||
             Adapter->MacVersion == RTL_MAC_VER_38)
    {
        rc = RX_RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST;
    }
    else if (Adapter->MacVersion >= RTL_MAC_VER_40)
    {
        rc = RX_RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF;
    }
    else
    {
        rc = RX_RX128_INT_EN | RX_DMA_BURST;
    }

    RtlWriteReg32(Adapter, R_RC, rc);
}

NDIS_STATUS
NTAPI
NICProgramRings (
    IN PRTL_ADAPTER Adapter
    )
{
    ULONG i;

    /* Wipe and initialise both rings; rings already allocated in MiniportInitialize. */
    NdisZeroMemory(Adapter->TxRing, TX_DESC_COUNT * sizeof(RTL_DESC));
    NdisZeroMemory(Adapter->RxRing, RX_DESC_COUNT * sizeof(RTL_DESC));

    for (i = 0; i < TX_DESC_COUNT; i++)
        RtlInitTxDescriptor(Adapter, i);

    for (i = 0; i < RX_DESC_COUNT; i++)
        NICRefillRxDescriptor(Adapter, i);

    Adapter->TxProducer = 0;
    Adapter->TxConsumer = 0;
    Adapter->TxFull = FALSE;
    Adapter->RxConsumer = 0;

    /* From here on, mirror Linux rtl_hw_start (r8169_main.c:3746). */

    /* Configuration writes need Cfg9346 unlocked. */
    RtlWriteReg8(Adapter, R_CFG9346, CFG9346_UNLOCK);

    /* Disable ASPM and clock request before any EPHY access. */
    RtlHwAspmClkReqDisable(Adapter);

    /* CPlusCmd: Linux keeps only the CPCMD_MASK bits it read at probe.  We
     * additionally drop RxVlan (no VLAN offload), RxChkSum (the RX path does
     * not consume hardware checksum results) and the INTT bits (IntrMitigate
     * is written as 0 below), leaving just the Normal_mode bit. */
    RtlWriteReg16(Adapter, R_CPLUSCMD,
                  RtlReadReg16(Adapter, R_CPLUSCMD) & CPCMD_NORMAL);

    /* rtl_hw_start_8168: early-TX threshold, in units of 128 bytes. */
    RtlWriteReg8(Adapter, R_MAXTXPKTSIZE,
                 (Adapter->MacVersion >= RTL_MAC_VER_34) ? TXPKT_EARLY_SIZE
                                                         : TXPKT_MAX);

    /* Per-chip-variant FIFO sizing, EPHY init, ERI/EEE tuning
     * (rtl_hw_config). */
    RtlHwStartChipSpecific(Adapter);

    /* IntrMitigate = 0: disable interrupt coalescing. */
    RtlWriteReg16(Adapter, R_IMITIGATE, 0);

    /* rtl_enable_exit_l1 */
    RtlEnableExitL1(Adapter);

    /* Linux re-enables ASPM here (rtl_hw_aspm_clkreq_enable(tp, true));
     * we deliberately leave it disabled. */

    /* RxMaxSize: Linux rtl_set_rx_max_size sets RX_BUF_SIZE + 1 -- comment
     * says "Low hurts. Let's disable the filtering." */
    RtlWriteReg16(Adapter, R_RXMAXSIZE, (USHORT)(RX_BUF_SIZE + 1));

    /* Hand descriptor ring base addresses to the chip.
     * Linux r8169_main.c rtl_set_rx_tx_desc_registers comments:
     *   "Magic spell: some iop3xx ARM board needs the TxDescAddrHigh
     *    register to be written before TxDescAddrLow to work."
     * Mirror that order for both rings. */
    RtlWriteReg32(Adapter, R_TXDESC_HI, Adapter->TxRingPa.HighPart);
    RtlWriteReg32(Adapter, R_TXDESC_LO, Adapter->TxRingPa.LowPart);
    RtlWriteReg32(Adapter, R_RXDESC_HI, Adapter->RxRingPa.HighPart);
    RtlWriteReg32(Adapter, R_RXDESC_LO, Adapter->RxRingPa.LowPart);

    RtlWriteReg8(Adapter, R_CFG9346, CFG9346_LOCK);

    /* Make sure the jumbo-frame enables are off (rtl_jumbo_config). */
    RtlJumboDisable(Adapter);

    /* The engine enable tail (ChipCmd, RxConfig, TxConfig, filter) runs in
     * NICEnableTxRx; the caller unmasks interrupts last. */
    return NDIS_STATUS_SUCCESS;
}

/*
 * Engine enable tail of Linux rtl_hw_start (r8169_main.c:3769):
 * PCI commit, CmdTxEnb|CmdRxEnb, rtl_init_rxcfg, TxConfig, RX filter.
 * The caller unmasks interrupts after this returns.
 */
NDIS_STATUS
NTAPI
NICEnableTxRx (
    IN PRTL_ADAPTER Adapter
    )
{
    ULONG tx;

    /* "Initially a 10 us delay. Turned it into a PCI commit. - FR" */
    (VOID)RtlReadReg8(Adapter, R_CMD);

    RtlWriteReg8(Adapter, R_CMD, B_CMD_TXENB | B_CMD_RXENB);

    RtlInitRxConfig(Adapter);

    /* rtl_set_tx_config_registers: DMA unlimited + normal IFG; 8168E-VL and
     * later want TXCFG_AUTO_FIFO. */
    tx = TXCFG_DMA_UNLIMITED | TXCFG_IFG_NORMAL;
    if (Adapter->MacVersion >= RTL_MAC_VER_34)
        tx |= TXCFG_AUTO_FIFO;
    RtlWriteReg32(Adapter, R_TC, tx);

    /* rtl_set_rx_mode: apply the accept bits on top of rtl_init_rxcfg. */
    NICApplyPacketFilter(Adapter);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICApplyInterruptMask (
    IN PRTL_ADAPTER Adapter
    )
{
    RtlWriteReg16(Adapter, R_IM, Adapter->InterruptMask);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICDisableInterrupts (
    IN PRTL_ADAPTER Adapter
    )
{
    RtlWriteReg16(Adapter, R_IM, 0);
    return NDIS_STATUS_SUCCESS;
}

USHORT
NTAPI
NICInterruptRecognized (
    IN PRTL_ADAPTER Adapter,
    OUT PBOOLEAN InterruptRecognized
    )
{
    USHORT status = RtlReadReg16(Adapter, R_IS);

    /* 0xFFFF means the device disappeared (D3, surprise removal). */
    if (status == 0xFFFF)
    {
        *InterruptRecognized = FALSE;
        return 0;
    }

    *InterruptRecognized = (status & Adapter->InterruptMask) != 0;
    return (status & Adapter->InterruptMask);
}

VOID
NTAPI
NICAcknowledgeInterrupts (
    IN PRTL_ADAPTER Adapter
    )
{
    RtlWriteReg16(Adapter, R_IS, Adapter->InterruptPending);
}

VOID
NTAPI
NICUpdateLinkStatus (
    IN PRTL_ADAPTER Adapter
    )
{
    UCHAR phyStatus = RtlReadReg8(Adapter, R_PHYSTS);

    Adapter->LastPhyStatus = phyStatus;
    Adapter->MediaState = (phyStatus & PHYSTS_LINK_UP) ?
                          NdisMediaStateConnected :
                          NdisMediaStateDisconnected;

    if (phyStatus & PHYSTS_1000_FD)
        Adapter->LinkSpeedMbps = 1000;
    else if (phyStatus & PHYSTS_100)
        Adapter->LinkSpeedMbps = 100;
    else if (phyStatus & PHYSTS_10)
        Adapter->LinkSpeedMbps = 10;
    else
        Adapter->LinkSpeedMbps = 0;
}

/* rtl_set_rx_mode -- r8169_main.c:2575: modify only the accept bits
 * (RX_CONFIG_ACCEPT_OK_MASK); the rest of RxConfig stays as programmed by
 * RtlInitRxConfig. */
NDIS_STATUS
NTAPI
NICApplyPacketFilter (
    IN PRTL_ADAPTER Adapter
    )
{
    ULONG rxMode = 0;
    ULONG tmp;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_DIRECTED)
        rxMode |= RX_ACCEPT_MYPHYS;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_MULTICAST)
        rxMode |= RX_ACCEPT_MCAST;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_BROADCAST)
        rxMode |= RX_ACCEPT_BCAST;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_PROMISCUOUS)
        rxMode |= RX_ACCEPT_ALLPHYS | RX_ACCEPT_MYPHYS |
                  RX_ACCEPT_BCAST | RX_ACCEPT_MCAST;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_ALL_MULTICAST)
        rxMode |= RX_ACCEPT_MCAST;

    /* Multicast filter -- accept-all bitmask for now (no per-MAC hash).
     * Linux writes MAR0+4 before MAR0. */
    RtlWriteReg32(Adapter, R_MAR0 + 4, 0xFFFFFFFF);
    RtlWriteReg32(Adapter, R_MAR0,     0xFFFFFFFF);

    tmp = RtlReadReg32(Adapter, R_RC);
    RtlWriteReg32(Adapter, R_RC, (tmp & ~RX_ACCEPT_OK_MASK) | rxMode);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICTransmitDescriptor (
    IN PRTL_ADAPTER Adapter,
    IN ULONG Index,
    IN PHYSICAL_ADDRESS BufferPa,
    IN ULONG Length,
    IN ULONG TxOpts2
    )
{
    PRTL_DESC d = &Adapter->TxRing[Index];
    ULONG opts1 = DESC_OWN | DESC_FS | DESC_LS | (Length & DESC_LEN_MASK);

    if (Index == TX_DESC_COUNT - 1)
        opts1 |= DESC_EOR;

    /* opts2 carries the per-packet checksum-offload request bits
     * (TXD2_IPv4_CS / TXD2_TCP_CS / TXD2_UDP_CS).  Caller computed them from
     * NDIS_PACKET TcpIpChecksumPacketInfo. */
    d->opts2 = TxOpts2;
    d->addr  = BufferPa.QuadPart;
    /* opts1 last (publish OWN after addr/len are visible) */
    d->opts1 = opts1;

    /* Poll-bit kicks the TX engine on the normal-priority queue. */
    RtlWriteReg8(Adapter, R_TXPOLL, B_TXP_NPQ);
    return NDIS_STATUS_SUCCESS;
}
