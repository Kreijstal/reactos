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

NDIS_STATUS
NTAPI
NICPowerOn (
    IN PRTL_ADAPTER Adapter
    )
{
    /* Clear PMEnable in Config1 -- the chip otherwise stays in a PM state where
     * registers respond but the MAC engine is gated.  Cfg9346 must be unlocked
     * to touch Config1..Config5. */
    RtlWriteReg8(Adapter, R_CFG9346, CFG9346_UNLOCK);
    RtlWriteReg8(Adapter, R_CFG1, RtlReadReg8(Adapter, R_CFG1) & ~0x01);
    RtlWriteReg8(Adapter, R_CFG9346, CFG9346_LOCK);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICSoftReset (
    IN PRTL_ADAPTER Adapter
    )
{
    UINT attempts;

    RtlWriteReg8(Adapter, R_CMD, B_CMD_RESET);

    for (attempts = 0; attempts < MAX_RESET_ATTEMPTS; attempts++)
    {
        if (!(RtlReadReg8(Adapter, R_CMD) & B_CMD_RESET))
            return NDIS_STATUS_SUCCESS;

        NdisStallExecution(1000);   /* 1 ms */
    }

    NDIS_DbgPrint(MIN_TRACE, ("RTL8168: soft reset timed out\n"));
    return NDIS_STATUS_FAILURE;
}

/*
 * Decode the chip version from TxConfig (R_TC) bits 30..23.
 * The mapping is intentionally permissive: unknown silicon decodes to
 * RTL_MAC_VER_UNKNOWN which the driver still tries to drive (assuming the
 * BIOS brought up the PHY).
 *
 * Table mirrors rtl_chip_infos[] / rtl_mac_version selection in r8169_main.c.
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
        { 0x7CF00000, 0x30000000, RTL_MAC_VER_11, "RTL8168B/8111B" },
        { 0x7CF00000, 0x38000000, RTL_MAC_VER_17, "RTL8168B/8111B" },
        { 0x7C800000, 0x38000000, RTL_MAC_VER_18, "RTL8168CP" },
        { 0x7CF00000, 0x3C000000, RTL_MAC_VER_19, "RTL8168C/8111C" },
        { 0x7CF00000, 0x3C200000, RTL_MAC_VER_20, "RTL8168C/8111C" },
        { 0x7CF00000, 0x3C400000, RTL_MAC_VER_21, "RTL8168C/8111C" },
        { 0x7CF00000, 0x3C800000, RTL_MAC_VER_22, "RTL8168C/8111C" },
        { 0x7C800000, 0x3C000000, RTL_MAC_VER_23, "RTL8168CP" },
        { 0x7C800000, 0x3C800000, RTL_MAC_VER_24, "RTL8168CP" },
        { 0x7CF00000, 0x28000000, RTL_MAC_VER_25, "RTL8168D/8111D" },
        { 0x7CF00000, 0x28100000, RTL_MAC_VER_26, "RTL8168D/8111D" },
        { 0x7C800000, 0x28800000, RTL_MAC_VER_28, "RTL8168DP" },
        { 0x7CF00000, 0x28A00000, RTL_MAC_VER_31, "RTL8168DP" },
        { 0x7CF00000, 0x2C000000, RTL_MAC_VER_32, "RTL8168E/8111E" },
        { 0x7CF00000, 0x2C200000, RTL_MAC_VER_33, "RTL8168E/8111E" },
        { 0x7CF00000, 0x2C800000, RTL_MAC_VER_34, "RTL8168EVL/8111EVL" },
        { 0x7CF00000, 0x2C900000, RTL_MAC_VER_38, "RTL8411" },
        { 0x7CF00000, 0x48000000, RTL_MAC_VER_35, "RTL8168F/8111F" },
        { 0x7CF00000, 0x48100000, RTL_MAC_VER_36, "RTL8168F/8111F" },
        { 0x7CF00000, 0x48800000, RTL_MAC_VER_44, "RTL8411B" },
        { 0x7CF00000, 0x4C000000, RTL_MAC_VER_40, "RTL8168G/8111G" },
        { 0x7CF00000, 0x4C100000, RTL_MAC_VER_42, "RTL8168GU/8111GU" },
        { 0x7CF00000, 0x50000000, RTL_MAC_VER_46, "RTL8168H/8111H" },
        { 0x7CF00000, 0x54000000, RTL_MAC_VER_51, "RTL8168EP/8111EP" },
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
            NDIS_DbgPrint(MIN_TRACE,
                ("RTL8168: detected %s (TxConfig=0x%08lx)\n",
                 table[i].name, raw));
            return NDIS_STATUS_SUCCESS;
        }
    }

    NDIS_DbgPrint(MIN_TRACE,
        ("RTL8168: unknown chip version (TxConfig=0x%08lx); driving as generic 8168\n",
         raw));
    return NDIS_STATUS_SUCCESS;
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

    /* Hand descriptor ring base addresses to the chip.
     * Linux r8169_main.c rtl_set_rx_tx_desc_registers comments:
     *   "Magic spell: some iop3xx ARM board needs the TxDescAddrHigh
     *    register to be written before TxDescAddrLow to work."
     * Mirror that order for both rings. */
    RtlWriteReg32(Adapter, R_TXDESC_HI, Adapter->TxRingPa.HighPart);
    RtlWriteReg32(Adapter, R_TXDESC_LO, Adapter->TxRingPa.LowPart);
    RtlWriteReg32(Adapter, R_RXDESC_HI, Adapter->RxRingPa.HighPart);
    RtlWriteReg32(Adapter, R_RXDESC_LO, Adapter->RxRingPa.LowPart);

    /* Configuration writes need Cfg9346 unlocked. */
    RtlWriteReg8(Adapter, R_CFG9346, CFG9346_UNLOCK);

    /* RxMaxSize: Linux rtl_set_rx_max_size sets RX_BUF_SIZE + 1 -- comment
     * says "Low hurts. Let's disable the filtering." */
    RtlWriteReg16(Adapter, R_RXMAXSIZE, (USHORT)(RX_BUF_SIZE + 1));

    /* MaxTxPacketSize in units of 128 bytes (8168/8101) */
    RtlWriteReg8(Adapter, R_MAXTXPKTSIZE, 0x3F);

    /* CPlusCmd: keep manufacturer defaults + RxChksum.  Per Linux this is
     * also where RxVlan gets set; we leave VLAN off. */
    RtlWriteReg16(Adapter, R_CPLUSCMD,
                  CPCMD_NORMAL | CPCMD_RXCHKSUM | CPCMD_PCI_MRW);

    /* IntrMitigate = 0: deliver every interrupt immediately. */
    RtlWriteReg16(Adapter, R_IMITIGATE, 0);

    /* RxConfig: accept broadcast/multicast/myphys, FIFO + DMA unlimited. */
    RtlWriteReg32(Adapter, R_RC,
                  RX_FIFO_THRESH | RX_DMA_BURST |
                  RX_ACCEPT_BCAST | RX_ACCEPT_MCAST | RX_ACCEPT_MYPHYS);

    /* TxConfig: keep chip-version bits; set DMA unlimited + normal IFG.
     * For 8168E-VL and later (MAC_VER >= 34) the spec requires TXCFG_AUTO_FIFO. */
    {
        ULONG tx = (Adapter->TxConfigRaw & TXCFG_VER_MASK) |
                   TXCFG_DMA_UNLIMITED | TXCFG_IFG_NORMAL;
        if (Adapter->MacVersion >= RTL_MAC_VER_34)
            tx |= TXCFG_AUTO_FIFO;
        RtlWriteReg32(Adapter, R_TC, tx);
    }

    /* Per-chip-class init bits ported from rtl_hw_start_8168e_2 / _f / _g.
     * Without these, MCU stays in OOB mode and RX is gated off on the
     * 8168E-and-newer parts -- packets reach the chip but never the descriptor
     * ring, so the driver looks "alive" but receives nothing.
     *
     * 8168D and earlier predate the MCU/MISC layout, so guard the writes. */
    if (Adapter->MacVersion >= RTL_MAC_VER_25)   /* 8168D+ has the MCU register */
    {
        UCHAR mcu = RtlReadReg8(Adapter, R_MCU);
        RtlWriteReg8(Adapter, R_MCU, mcu & ~MCU_NOW_IS_OOB);
    }

    if (Adapter->MacVersion >= RTL_MAC_VER_32)   /* 8168E and later */
    {
        UCHAR dllpr = RtlReadReg8(Adapter, R_DLLPR);
        ULONG misc = RtlReadReg32(Adapter, R_MISC);

        /* Clear RX-data-valid gate -- failure to clear leaves RX dead on
         * 8168G/H even though every other path looks healthy. */
        misc &= ~MISC_RXDV_GATED_EN;
        /* PWM/PFM enable -- Linux always sets these on 8168E+ for power
         * management quirks; safe on AC-powered desktops/laptops. */
        misc |= MISC_PWM_EN;
        dllpr |= DLLPR_PFM_EN;

        RtlWriteReg32(Adapter, R_MISC, misc);
        RtlWriteReg8(Adapter, R_DLLPR, dllpr);
    }

    /* Per-chip-variant FIFO sizing, EPHY init, ASPM/EEE tuning. */
    RtlHwStartChipSpecific(Adapter);

    RtlWriteReg8(Adapter, R_CFG9346, CFG9346_LOCK);

    /* PHY tuning passes -- safe to do after Cfg9346 is re-locked. */
    RtlHwPhyConfig(Adapter);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICEnableTxRx (
    IN PRTL_ADAPTER Adapter
    )
{
    UCHAR cmd = RtlReadReg8(Adapter, R_CMD);
    cmd |= B_CMD_RXENB | B_CMD_TXENB;
    RtlWriteReg8(Adapter, R_CMD, cmd);
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

NDIS_STATUS
NTAPI
NICApplyPacketFilter (
    IN PRTL_ADAPTER Adapter
    )
{
    ULONG rc = RX_FIFO_THRESH | RX_DMA_BURST;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_DIRECTED)
        rc |= RX_ACCEPT_MYPHYS;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_MULTICAST)
        rc |= RX_ACCEPT_MCAST;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_BROADCAST)
        rc |= RX_ACCEPT_BCAST;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_PROMISCUOUS)
        rc |= RX_ACCEPT_ALLPHYS | RX_ACCEPT_MYPHYS |
              RX_ACCEPT_BCAST | RX_ACCEPT_MCAST;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_ALL_MULTICAST)
        rc |= RX_ACCEPT_MCAST;

    RtlWriteReg32(Adapter, R_RC, rc);

    /* Multicast filter -- accept-all bitmask for now (no per-MAC hash). */
    RtlWriteReg32(Adapter, R_MAR0,     0xFFFFFFFF);
    RtlWriteReg32(Adapter, R_MAR0 + 4, 0xFFFFFFFF);

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
