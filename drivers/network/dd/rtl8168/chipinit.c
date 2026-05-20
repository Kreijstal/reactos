/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Realtek 8168/8111 driver
 * FILE:        chipinit.c
 * PURPOSE:     Per-chip-variant MAC bring-up (FIFO sizing, EPHY init tables,
 *              ERI tunes, EEE-MAC) -- the "rtl_hw_start_8168X" family from
 *              Linux r8169_main.c.
 *
 * Each of these orchestrates the chip-specific quirks Realtek expects to be
 * programmed before the TX/RX engines are enabled.  Without them the chip
 * usually links but drops RX silently, or stays in management-engine mode.
 */

#include "nic.h"

#define NDEBUG
#include <debug.h>

/* Mirror Linux ARRAY_SIZE so the EPHY table macros read identically. */
#ifndef ARRAYSIZE
#define ARRAYSIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif

/*===========================================================================
 *  Helper sequences ported verbatim from r8169_main.c
 *==========================================================================*/

/* rtl_set_fifo_size -- r8169_main.c:3020.  rx/tx_stat are 16-bit FIFO sizes,
 * rx/tx_dyn are dynamic-mode thresholds. */
static VOID
RtlSetFifoSize(IN PRTL_ADAPTER A,
               IN USHORT RxStat, IN USHORT TxStat,
               IN USHORT RxDyn,  IN USHORT TxDyn)
{
    RtlEriWrite(A, 0xC8, ERIAR_MASK_1111, ((ULONG)RxStat << 16) | RxDyn);
    RtlEriWrite(A, 0xE8, ERIAR_MASK_1111, ((ULONG)TxStat << 16) | TxDyn);
}

/* rtl8168g_set_pause_thresholds -- r8169_main.c:3030 */
static VOID
RtlSetPauseThresholds(IN PRTL_ADAPTER A, IN UCHAR Low, IN UCHAR High)
{
    RtlEriWrite(A, 0xCC, ERIAR_MASK_0001, Low);
    RtlEriWrite(A, 0xD0, ERIAR_MASK_0001, High);
}

/* rtl_reset_packet_filter -- r8169_main.c:1469 */
static VOID
RtlResetPacketFilter(IN PRTL_ADAPTER A)
{
    RtlEriClearBits(A, 0xDC, 0x01);
    RtlEriSetBits(A,   0xDC, 0x01);
}

/* rtl8168_config_eee_mac -- r8169_main.c:2380.  Skips the EEE_LED tweak for
 * MAC_VER_38 which uses a different LED layout. */
static VOID
Rtl8168ConfigEeeMac(IN PRTL_ADAPTER A)
{
    /* Adjust EEE LED frequency (offset 0xDB).  Linux only touches it when the
     * chip is not MAC_VER_38, but reading-then-writing is safe regardless. */
    if (A->MacVersion != RTL_MAC_VER_38)
    {
        UCHAR led = RtlReadReg8(A, 0xDB);
        RtlWriteReg8(A, 0xDB, led & ~0x07);
    }

    RtlEriSetBits(A, 0x1B0, 0x0003);
}

/* rtl_disable_rxdvgate -- r8169_main.c:2702.  Already mirrored inline in
 * NICProgramRings; exposed here for completeness on 8168G+. */
static VOID
RtlDisableRxdvGate(IN PRTL_ADAPTER A)
{
    ULONG misc = RtlReadReg32(A, R_MISC);
    RtlWriteReg32(A, R_MISC, misc & ~MISC_RXDV_GATED_EN);
}

/* rtl_pcie_state_l2l3_disable -- r8169_main.c:2912.  Disables Rdy_to_L23
 * (PCIe L2/L3 entry-ready) to work around a chip-reset-during-low-power bug. */
static VOID
RtlPcieStateL2l3Disable(IN PRTL_ADAPTER A)
{
    UCHAR cfg3 = RtlReadReg8(A, R_CFG3);
    RtlWriteReg8(A, R_CFG3, cfg3 & ~CFG3_RDY_TO_L23);
}

/*===========================================================================
 *  Per-chip EPHY (PCIe analog PHY) initialisation tables.
 *  Verbatim ports of the e_info_8168X[] arrays in r8169_main.c.
 *==========================================================================*/

static const EPHY_INFO e_info_8168e_1[] = {
    { 0x00, 0x0200, 0x0100 }, { 0x00, 0x0000, 0x0004 },
    { 0x06, 0x0002, 0x0001 }, { 0x06, 0x0000, 0x0030 },
    { 0x07, 0x0000, 0x2000 }, { 0x00, 0x0000, 0x0020 },
    { 0x03, 0x5800, 0x2000 }, { 0x03, 0x0000, 0x0001 },
    { 0x01, 0x0800, 0x1000 }, { 0x07, 0x0000, 0x4000 },
    { 0x1e, 0x0000, 0x2000 }, { 0x19, 0xffff, 0xfe6c },
    { 0x0a, 0x0000, 0x0040 },
};

static const EPHY_INFO e_info_8168e_2[] = {
    { 0x09, 0x0000, 0x0080 }, { 0x19, 0x0000, 0x0224 },
    { 0x00, 0x0000, 0x0004 }, { 0x0c, 0x3df0, 0x0200 },
};

static const EPHY_INFO e_info_8168f_1[] = {
    { 0x06, 0x00c0, 0x0020 }, { 0x08, 0x0001, 0x0002 },
    { 0x09, 0x0000, 0x0080 }, { 0x19, 0x0000, 0x0224 },
    { 0x00, 0x0000, 0x0008 }, { 0x0c, 0x3df0, 0x0200 },
};

/* 8411 (also referred to as "f_2" in some sources) */
static const EPHY_INFO e_info_8411_1[] = {
    { 0x06, 0x00c0, 0x0020 }, { 0x0f, 0xffff, 0x5200 },
    { 0x19, 0x0000, 0x0224 }, { 0x00, 0x0000, 0x0008 },
    { 0x0c, 0x3df0, 0x0200 },
};

static const EPHY_INFO e_info_8168g_1[] = {
    { 0x00, 0x0008, 0x0000 }, { 0x0c, 0x3ff0, 0x0820 },
    { 0x1e, 0x0000, 0x0001 }, { 0x19, 0x8000, 0x0000 },
};

static const EPHY_INFO e_info_8168g_2[] = {
    { 0x00, 0x0008, 0x0000 }, { 0x0c, 0x3ff0, 0x0820 },
    { 0x19, 0xffff, 0x7c00 }, { 0x1e, 0xffff, 0x20eb },
    { 0x0d, 0xffff, 0x1666 }, { 0x00, 0xffff, 0x10a3 },
    { 0x06, 0xffff, 0xf050 }, { 0x04, 0x0000, 0x0010 },
    { 0x1d, 0x4000, 0x0000 },
};

/* RTL8168H / RTL8111H -- e_info_8168h_2 from r8169_main.c:3438 */
static const EPHY_INFO e_info_8168h_2[] = {
    { 0x1e, 0x0800, 0x0001 }, { 0x1d, 0x0000, 0x0800 },
    { 0x05, 0xffff, 0x2089 }, { 0x06, 0xffff, 0x5881 },
    { 0x04, 0xffff, 0x854a }, { 0x01, 0xffff, 0x068b },
};

/*===========================================================================
 *  Per-chip hw_start sequences.
 *  All write the Cfg9346 unlock dance externally (caller in NICProgramRings).
 *==========================================================================*/

/* rtl_hw_start_8168e_1 -- r8169_main.c:3147 */
static VOID
Rtl8168HwStart_E1(IN PRTL_ADAPTER A)
{
    ULONG misc;

    RtlEphyInit(A, e_info_8168e_1, ARRAYSIZE(e_info_8168e_1));

    /* Reset TX FIFO pointer */
    misc = RtlReadReg32(A, R_MISC);
    RtlWriteReg32(A, R_MISC, misc | MISC_TXPLA_RST);
    RtlWriteReg32(A, R_MISC, misc & ~MISC_TXPLA_RST);

    /* Cfg5: clear Spi_en */
    RtlWriteReg8(A, R_CFG5, RtlReadReg8(A, R_CFG5) & ~CFG5_SPI_EN);
}

/* rtl_hw_start_8168e_2 -- r8169_main.c:3178.  Baseline target for the ROS
 * driver -- this is the variant most desktop boards (incl. ASUS X550DP era)
 * ship with. */
static VOID
Rtl8168HwStart_E2(IN PRTL_ADAPTER A)
{
    RtlEphyInit(A, e_info_8168e_2, ARRAYSIZE(e_info_8168e_2));

    RtlEriWrite(A, 0xC0, ERIAR_MASK_0011, 0x0000);
    RtlEriWrite(A, 0xB8, ERIAR_MASK_1111, 0x0000);
    RtlSetFifoSize(A, 0x10, 0x10, 0x02, 0x06);
    RtlEriSetBits(A, 0x1D0, 0x02);              /* BIT(1) */
    RtlResetPacketFilter(A);
    RtlEriSetBits(A, 0x1B0, 0x10);              /* BIT(4) */
    RtlEriWrite(A, 0xCC, ERIAR_MASK_1111, 0x00000050);
    RtlEriWrite(A, 0xD0, ERIAR_MASK_1111, 0x07FF0060);

    Rtl8168ConfigEeeMac(A);

    /* MCU/DLLPR/MISC already touched by NICProgramRings; tolerated. */
    RtlWriteReg8(A, R_CFG5, RtlReadReg8(A, R_CFG5) & ~CFG5_SPI_EN);
}

/* rtl_hw_start_8168f -- r8169_main.c:3211 (helper used by f_1, f_2, 8411) */
static VOID
Rtl8168HwStart_F_Common(IN PRTL_ADAPTER A)
{
    RtlEriWrite(A, 0xC0, ERIAR_MASK_0011, 0x0000);
    RtlEriWrite(A, 0xB8, ERIAR_MASK_1111, 0x0000);
    RtlSetFifoSize(A, 0x10, 0x10, 0x02, 0x06);
    RtlResetPacketFilter(A);
    RtlEriSetBits(A, 0x1B0, 0x10);              /* BIT(4) */
    RtlEriSetBits(A, 0x1D0, 0x10 | 0x02);       /* BIT(4) | BIT(1) */
    RtlEriWrite(A, 0xCC, ERIAR_MASK_1111, 0x00000050);
    RtlEriWrite(A, 0xD0, ERIAR_MASK_1111, 0x00000060);

    RtlWriteReg8(A, R_CFG5, RtlReadReg8(A, R_CFG5) & ~CFG5_SPI_EN);

    Rtl8168ConfigEeeMac(A);
}

static VOID
Rtl8168HwStart_F1(IN PRTL_ADAPTER A)
{
    Rtl8168HwStart_F_Common(A);
    RtlEphyInit(A, e_info_8168f_1, ARRAYSIZE(e_info_8168f_1));
}

static VOID
Rtl8168HwStart_8411(IN PRTL_ADAPTER A)
{
    Rtl8168HwStart_F_Common(A);
    RtlPcieStateL2l3Disable(A);
    RtlEphyInit(A, e_info_8411_1, ARRAYSIZE(e_info_8411_1));
}

/* rtl_hw_start_8168g -- r8169_main.c:3266.  Common across the G variants. */
static VOID
Rtl8168HwStart_G_Common(IN PRTL_ADAPTER A)
{
    RtlSetFifoSize(A, 0x08, 0x10, 0x02, 0x06);
    RtlSetPauseThresholds(A, 0x38, 0x48);

    RtlResetPacketFilter(A);
    RtlEriWrite(A, 0x2F8, ERIAR_MASK_0011, 0x1D8F);

    RtlDisableRxdvGate(A);

    RtlEriWrite(A, 0xC0, ERIAR_MASK_0011, 0x0000);
    RtlEriWrite(A, 0xB8, ERIAR_MASK_0011, 0x0000);

    Rtl8168ConfigEeeMac(A);

    RtlW0w1Eri(A, 0x2FC, 0x01, 0x06);
    RtlEriClearBits(A, 0x1B0, 0x1000);          /* BIT(12) */

    RtlPcieStateL2l3Disable(A);
}

static VOID
Rtl8168HwStart_G1(IN PRTL_ADAPTER A)
{
    Rtl8168HwStart_G_Common(A);
    RtlEphyInit(A, e_info_8168g_1, ARRAYSIZE(e_info_8168g_1));
}

static VOID
Rtl8168HwStart_G2(IN PRTL_ADAPTER A)
{
    Rtl8168HwStart_G_Common(A);
    RtlEphyInit(A, e_info_8168g_2, ARRAYSIZE(e_info_8168g_2));
}

/* rtl_hw_start_8168h_1 -- r8169_main.c:3409 (8168H/8111H).  The full Linux
 * sequence touches several MAC OCP registers via the EEE/L1.2 timer path;
 * this is the safe-default subset that covers binding + RX. */
static VOID
Rtl8168HwStart_H2(IN PRTL_ADAPTER A)
{
    RtlSetFifoSize(A, 0x08, 0x10, 0x02, 0x06);
    RtlSetPauseThresholds(A, 0x38, 0x48);

    RtlResetPacketFilter(A);
    RtlEriWrite(A, 0x2F8, ERIAR_MASK_0011, 0x1D8F);

    RtlDisableRxdvGate(A);

    RtlEriWrite(A, 0xC0, ERIAR_MASK_0011, 0x0000);
    RtlEriWrite(A, 0xB8, ERIAR_MASK_0011, 0x0000);

    RtlEphyInit(A, e_info_8168h_2, ARRAYSIZE(e_info_8168h_2));

    RtlW0w1Eri(A, 0x2FC, 0x01, 0x06);
    RtlEriClearBits(A, 0x1B0, 0x1000);

    /* L1.2 disable (rtl_disable_exit_l1 partial) */
    RtlMacOcpModify(A, 0xC0AC, 0x1F80, 0);

    /* EEE-MAC on 8168G+ */
    Rtl8168ConfigEeeMac(A);

    RtlPcieStateL2l3Disable(A);
}

/*===========================================================================
 *  Dispatcher
 *==========================================================================*/

VOID
NTAPI
RtlHwStartChipSpecific (
    IN PRTL_ADAPTER A
    )
{
    /* On 8168G+ MDIO is redirected through GPHY OCP; default the page select
     * to the standard PHY base so the first 0x1F write doesn't latch a stale
     * value. */
    if (A->MacVersion >= RTL_MAC_VER_40)
    {
        A->OcpMdioRedirect = TRUE;
        A->OcpBase = OCP_STD_PHY_BASE;
    }

    /* Caller (NICProgramRings) holds Cfg9346 unlocked across this call. */
    switch (A->MacVersion)
    {
    case RTL_MAC_VER_32:    /* RTL8168E/8111E */
    case RTL_MAC_VER_33:
        Rtl8168HwStart_E1(A);
        break;

    case RTL_MAC_VER_34:    /* RTL8168E-VL */
        Rtl8168HwStart_E2(A);
        break;

    case RTL_MAC_VER_35:    /* RTL8168F/8111F */
    case RTL_MAC_VER_36:
        Rtl8168HwStart_F1(A);
        break;

    case RTL_MAC_VER_38:    /* RTL8411 */
    case RTL_MAC_VER_44:    /* RTL8411B */
        Rtl8168HwStart_8411(A);
        break;

    case RTL_MAC_VER_40:    /* RTL8168G/8111G */
        Rtl8168HwStart_G1(A);
        break;

    case RTL_MAC_VER_42:    /* RTL8168GU/8111GU */
    case RTL_MAC_VER_43:
        Rtl8168HwStart_G2(A);
        break;

    case RTL_MAC_VER_46:    /* RTL8168H/8111H */
    case RTL_MAC_VER_48:
        Rtl8168HwStart_H2(A);
        break;

    default:
        /* Older chips (B/C/CP/D series) and unknown silicon -- the baseline
         * register writes in NICProgramRings already cover them; do nothing. */
        break;
    }
}
