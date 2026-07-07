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

/* rtl_disable_rxdvgate -- r8169_main.c:2491 */
static VOID
RtlDisableRxdvGate(IN PRTL_ADAPTER A)
{
    ULONG misc = RtlReadReg32(A, R_MISC);
    RtlWriteReg32(A, R_MISC, misc & ~MISC_RXDV_GATED_EN);
}

/* rtl_hw_aspm_clkreq_enable(tp, false) -- r8169_main.c:2745.  Disables ASPM
 * and PCIe ClkReq before EPHY access; we never re-enable them. */
VOID
NTAPI
RtlHwAspmClkReqDisable (
    IN PRTL_ADAPTER A
    )
{
    if (A->MacVersion < RTL_MAC_VER_32)
        return;

    if (A->MacVersion == RTL_MAC_VER_46 || A->MacVersion == RTL_MAC_VER_48)
        RtlMacOcpModify(A, 0xE092, 0x00FF, 0);

    RtlWriteReg8(A, R_CFG2, RtlReadReg8(A, R_CFG2) & ~CFG2_CLKREQEN);
    RtlWriteReg8(A, R_CFG5, RtlReadReg8(A, R_CFG5) & ~CFG5_ASPM_EN);
}

/* rtl_enable_exit_l1 -- r8169_main.c:2706.  Selects which events may wake
 * the link out of ASPM L1. */
VOID
NTAPI
RtlEnableExitL1 (
    IN PRTL_ADAPTER A
    )
{
    if (A->MacVersion >= RTL_MAC_VER_34 && A->MacVersion <= RTL_MAC_VER_36)
        RtlEriSetBits(A, 0xD4, 0x1F00);
    else if (A->MacVersion >= RTL_MAC_VER_37 && A->MacVersion <= RTL_MAC_VER_38)
        RtlEriSetBits(A, 0xD4, 0x0C00);
    else if (A->MacVersion >= RTL_MAC_VER_40)
        RtlMacOcpModify(A, 0xC0AC, 0, 0x1F80);
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

static const EPHY_INFO e_info_8168cp[] = {
    { 0x01, 0x0000, 0x0001 }, { 0x02, 0x0800, 0x1000 },
    { 0x03, 0x0000, 0x0042 }, { 0x06, 0x0080, 0x0000 },
    { 0x07, 0x0000, 0x2000 },
};

static const EPHY_INFO e_info_8168c_1[] = {
    { 0x02, 0x0800, 0x1000 }, { 0x03, 0x0000, 0x0002 },
    { 0x06, 0x0080, 0x0000 },
};

static const EPHY_INFO e_info_8168c_2[] = {
    { 0x01, 0x0000, 0x0001 }, { 0x03, 0x0400, 0x0020 },
};

static const EPHY_INFO e_info_8168d_4[] = {
    { 0x0b, 0x0000, 0x0048 }, { 0x19, 0x0020, 0x0050 },
    { 0x0c, 0x0100, 0x0020 }, { 0x10, 0x0004, 0x0000 },
};

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

/* e_info_8411_2 from rtl_hw_start_8411_2 -- r8169_main.c:3090 */
static const EPHY_INFO e_info_8411_2[] = {
    { 0x00, 0x0008, 0x0000 }, { 0x0c, 0x37d0, 0x0820 },
    { 0x1e, 0x0000, 0x0001 }, { 0x19, 0x8021, 0x0000 },
    { 0x1e, 0x0000, 0x2000 }, { 0x0d, 0x0100, 0x0200 },
    { 0x00, 0x0000, 0x0080 }, { 0x06, 0x0000, 0x0010 },
    { 0x04, 0x0000, 0x0010 }, { 0x1d, 0x0000, 0x4000 },
};

/* RTL8168H / RTL8111H -- e_info_8168h_1 from r8169_main.c:3246 */
static const EPHY_INFO e_info_8168h_1[] = {
    { 0x1e, 0x0800, 0x0001 }, { 0x1d, 0x0000, 0x0800 },
    { 0x05, 0xffff, 0x2089 }, { 0x06, 0xffff, 0x5881 },
    { 0x04, 0xffff, 0x854a }, { 0x01, 0xffff, 0x068b },
};

/* e_info_8168ep_3 from rtl_hw_start_8168ep_3 -- r8169_main.c:3334 */
static const EPHY_INFO e_info_8168ep_3[] = {
    { 0x00, 0x0000, 0x0080 }, { 0x0d, 0x0100, 0x0200 },
    { 0x19, 0x8021, 0x0000 }, { 0x1e, 0x0000, 0x2000 },
};

/*===========================================================================
 *  Per-chip hw_start sequences.
 *  All write the Cfg9346 unlock dance externally (caller in NICProgramRings).
 *==========================================================================*/

/* rtl_set_def_aspm_entry_latency and rtl_disable/enable_clock_request are
 * PCI-config-space operations (ECAM offset 0x070f / PCIe LNKCTL); they are
 * not ported.  The chip-register parts of each hw_start are transcribed
 * verbatim below. */

/* rtl_hw_start_8168b -- r8169_main.c:2806 */
static VOID
Rtl8168HwStart_B(IN PRTL_ADAPTER A)
{
    RtlWriteReg8(A, R_CFG3, RtlReadReg8(A, R_CFG3) & ~CFG3_BEACON_EN);
}

/* __rtl_hw_start_8168cp -- r8169_main.c:2811 */
static VOID
Rtl8168HwStart_CP_Common(IN PRTL_ADAPTER A)
{
    RtlWriteReg8(A, R_CFG1, RtlReadReg8(A, R_CFG1) | CFG1_SPEED_DOWN);

    RtlWriteReg8(A, R_CFG3, RtlReadReg8(A, R_CFG3) & ~CFG3_BEACON_EN);
}

/* rtl_hw_start_8168cp_1 -- r8169_main.c:2820 */
static VOID
Rtl8168HwStart_CP1(IN PRTL_ADAPTER A)
{
    RtlEphyInit(A, e_info_8168cp, ARRAYSIZE(e_info_8168cp));
    Rtl8168HwStart_CP_Common(A);
}

/* rtl_hw_start_8168cp_2 -- r8169_main.c:2837 */
static VOID
Rtl8168HwStart_CP2(IN PRTL_ADAPTER A)
{
    RtlWriteReg8(A, R_CFG3, RtlReadReg8(A, R_CFG3) & ~CFG3_BEACON_EN);
}

/* rtl_hw_start_8168cp_3 -- r8169_main.c:2844 */
static VOID
Rtl8168HwStart_CP3(IN PRTL_ADAPTER A)
{
    RtlWriteReg8(A, R_CFG3, RtlReadReg8(A, R_CFG3) & ~CFG3_BEACON_EN);

    /* Magic. */
    RtlWriteReg8(A, R_DBGREG, 0x20);
}

/* rtl_hw_start_8168c_1 -- r8169_main.c:2854 */
static VOID
Rtl8168HwStart_C1(IN PRTL_ADAPTER A)
{
    RtlWriteReg8(A, R_DBGREG, 0x06 | DBGREG_FIX_NAK_1 | DBGREG_FIX_NAK_2);

    RtlEphyInit(A, e_info_8168c_1, ARRAYSIZE(e_info_8168c_1));
    Rtl8168HwStart_CP_Common(A);
}

/* rtl_hw_start_8168c_2 -- r8169_main.c:2871 */
static VOID
Rtl8168HwStart_C2(IN PRTL_ADAPTER A)
{
    RtlEphyInit(A, e_info_8168c_2, ARRAYSIZE(e_info_8168c_2));
    Rtl8168HwStart_CP_Common(A);
}

/* rtl_hw_start_8168c_4 -- r8169_main.c:2885 */
static VOID
Rtl8168HwStart_C4(IN PRTL_ADAPTER A)
{
    Rtl8168HwStart_CP_Common(A);
}

/* rtl_hw_start_8168d_4 -- r8169_main.c:2899 */
static VOID
Rtl8168HwStart_D4(IN PRTL_ADAPTER A)
{
    RtlEphyInit(A, e_info_8168d_4, ARRAYSIZE(e_info_8168d_4));
}

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

    /* rtl_disable_clock_request: PCIe-capability write, not ported. */

    RtlWriteReg8(A, R_MCU, RtlReadReg8(A, R_MCU) & ~MCU_NOW_IS_OOB);

    Rtl8168ConfigEeeMac(A);

    RtlWriteReg8(A, R_DLLPR, RtlReadReg8(A, R_DLLPR) | DLLPR_PFM_EN);
    RtlWriteReg32(A, R_MISC, RtlReadReg32(A, R_MISC) | MISC_PWM_EN);
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

    /* rtl_disable_clock_request: PCIe-capability write, not ported. */

    RtlWriteReg8(A, R_MCU, RtlReadReg8(A, R_MCU) & ~MCU_NOW_IS_OOB);
    RtlWriteReg8(A, R_DLLPR, RtlReadReg8(A, R_DLLPR) | DLLPR_PFM_EN);
    RtlWriteReg32(A, R_MISC, RtlReadReg32(A, R_MISC) | MISC_PWM_EN);
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

/* rtl_hw_start_8411_2 -- r8169_main.c:3088 (RTL8411B).  Includes the
 * mandatory Realtek MCU patch: "The following Realtek-provided magic fixes
 * an issue with the RX unit getting confused after the PHY having been
 * powered-down."  The patch program is a contiguous block of 16-bit MAC-OCP
 * writes at 0xF800..0xF8DC, applied in Linux order. */
static const USHORT rtl8411b_mcu_patch[] = {
    0xE008, 0xE00A, 0xE00C, 0xE00E, 0xE027, 0xE04F, 0xE05E, 0xE065,
    0xC602, 0xBE00, 0x0000, 0xC502, 0xBD00, 0x074C, 0xC302, 0xBB00,
    0x080A, 0x6420, 0x48C2, 0x8C20, 0xC516, 0x64A4, 0x49C0, 0xF009,
    0x74A2, 0x8CA5, 0x74A0, 0xC50E, 0x9CA2, 0x1C11, 0x9CA0, 0xE006,
    0x74F8, 0x48C4, 0x8CF8, 0xC404, 0xBC00, 0xC403, 0xBC00, 0x0BF2,
    0x0C0A, 0xE434, 0xD3C0, 0x49D9, 0xF01F, 0xC526, 0x64A5, 0x1400,
    0xF007, 0x0C01, 0x8CA5, 0x1C15, 0xC51B, 0x9CA0, 0xE013, 0xC519,
    0x74A0, 0x48C4, 0x8CA0, 0xC516, 0x74A4, 0x48C8, 0x48CA, 0x9CA4,
    0xC512, 0x1B00, 0x9BA0, 0x1B1C, 0x483F, 0x9BA2, 0x1B04, 0xC508,
    0x9BA0, 0xC505, 0xBD00, 0xC502, 0xBD00, 0x0300, 0x051E, 0xE434,
    0xE018, 0xE092, 0xDE20, 0xD3C0, 0xC50F, 0x76A4, 0x49E3, 0xF007,
    0x49C0, 0xF103, 0xC607, 0xBE00, 0xC606, 0xBE00, 0xC602, 0xBE00,
    0x0C4C, 0x0C28, 0x0C2C, 0xDC00, 0xC707, 0x1D00, 0x8DE2, 0x48C1,
    0xC502, 0xBD00, 0x00AA, 0xE0C0, 0xC502, 0xBD00, 0x0132,
};

static VOID
Rtl8168HwStart_8411B(IN PRTL_ADAPTER A)
{
    ULONG i;

    Rtl8168HwStart_G_Common(A);

    RtlEphyInit(A, e_info_8411_2, ARRAYSIZE(e_info_8411_2));

    /* Park the MCU: clear the patch break-points, wait, release. */
    RtlMacOcpWrite(A, 0xFC28, 0x0000);
    RtlMacOcpWrite(A, 0xFC2A, 0x0000);
    RtlMacOcpWrite(A, 0xFC2C, 0x0000);
    RtlMacOcpWrite(A, 0xFC2E, 0x0000);
    RtlMacOcpWrite(A, 0xFC30, 0x0000);
    RtlMacOcpWrite(A, 0xFC32, 0x0000);
    RtlMacOcpWrite(A, 0xFC34, 0x0000);
    RtlMacOcpWrite(A, 0xFC36, 0x0000);
    for (i = 0; i < 30; i++)            /* mdelay(3) */
        NdisStallExecution(100);
    RtlMacOcpWrite(A, 0xFC26, 0x0000);

    for (i = 0; i < ARRAYSIZE(rtl8411b_mcu_patch); i++)
        RtlMacOcpWrite(A, 0xF800 + i * 2, rtl8411b_mcu_patch[i]);

    RtlMacOcpWrite(A, 0xFC26, 0x8000);

    RtlMacOcpWrite(A, 0xFC2A, 0x0743);
    RtlMacOcpWrite(A, 0xFC2C, 0x0801);
    RtlMacOcpWrite(A, 0xFC2E, 0x0BE9);
    RtlMacOcpWrite(A, 0xFC30, 0x02FD);
    RtlMacOcpWrite(A, 0xFC32, 0x0C25);
    RtlMacOcpWrite(A, 0xFC34, 0x00A9);
    RtlMacOcpWrite(A, 0xFC36, 0x012D);
}

/* rtl_hw_start_8168h_1 -- r8169_main.c:3244 (8168H/8111H) */
static VOID
Rtl8168HwStart_H1(IN PRTL_ADAPTER A)
{
    USHORT rgSawCnt;

    RtlEphyInit(A, e_info_8168h_1, ARRAYSIZE(e_info_8168h_1));

    RtlSetFifoSize(A, 0x08, 0x10, 0x02, 0x06);
    RtlSetPauseThresholds(A, 0x38, 0x48);

    RtlResetPacketFilter(A);

    RtlEriSetBits(A, 0xDC, 0x001C);

    RtlEriWrite(A, 0x5F0, ERIAR_MASK_0011, 0x4F87);

    RtlDisableRxdvGate(A);

    RtlEriWrite(A, 0xC0, ERIAR_MASK_0011, 0x0000);
    RtlEriWrite(A, 0xB8, ERIAR_MASK_0011, 0x0000);

    Rtl8168ConfigEeeMac(A);

    RtlWriteReg8(A, R_DLLPR, RtlReadReg8(A, R_DLLPR) & ~DLLPR_PFM_EN);
    RtlWriteReg8(A, R_MISC1, RtlReadReg8(A, R_MISC1) & ~MISC1_PFM_D3COLD_EN);

    RtlWriteReg8(A, R_DLLPR, RtlReadReg8(A, R_DLLPR) & ~DLLPR_TX_10M_PS_EN);

    RtlEriClearBits(A, 0x1B0, 0x1000);          /* BIT(12) */

    RtlPcieStateL2l3Disable(A);

    /* rg_saw_cnt = phy_read_paged(tp->phydev, 0x0c42, 0x13) & 0x3fff */
    rgSawCnt = (USHORT)(RtlPhyReadPaged(A, 0x0C42, 0x13) & 0x3FFF);
    if (rgSawCnt > 0)
    {
        USHORT swCnt1msIni = (USHORT)(16000000 / rgSawCnt);
        swCnt1msIni &= 0x0FFF;
        RtlMacOcpModify(A, 0xD412, 0x0FFF, swCnt1msIni);
    }

    RtlMacOcpModify(A, 0xE056, 0x00F0, 0x0070);
    RtlMacOcpModify(A, 0xE052, 0x6000, 0x8008);
    RtlMacOcpModify(A, 0xE0D6, 0x01FF, 0x017F);
    RtlMacOcpModify(A, 0xD420, 0x0FFF, 0x047F);

    RtlMacOcpWrite(A, 0xE63E, 0x0001);
    RtlMacOcpWrite(A, 0xE63E, 0x0000);
    RtlMacOcpWrite(A, 0xC094, 0x0000);
    RtlMacOcpWrite(A, 0xC09E, 0x0000);
}

/* rtl_hw_start_8168ep -- r8169_main.c:3305 (common EP tail) */
static VOID
Rtl8168HwStart_EP_Common(IN PRTL_ADAPTER A)
{
    Rtl8168EpStopCmac(A);

    RtlSetFifoSize(A, 0x08, 0x10, 0x02, 0x06);
    RtlSetPauseThresholds(A, 0x2F, 0x5F);

    RtlResetPacketFilter(A);

    RtlEriWrite(A, 0x5F0, ERIAR_MASK_0011, 0x4F87);

    RtlDisableRxdvGate(A);

    RtlEriWrite(A, 0xC0, ERIAR_MASK_0011, 0x0000);
    RtlEriWrite(A, 0xB8, ERIAR_MASK_0011, 0x0000);

    Rtl8168ConfigEeeMac(A);

    RtlW0w1Eri(A, 0x2FC, 0x01, 0x06);

    RtlWriteReg8(A, R_DLLPR, RtlReadReg8(A, R_DLLPR) & ~DLLPR_TX_10M_PS_EN);

    RtlPcieStateL2l3Disable(A);
}

/* rtl_hw_start_8168ep_3 -- r8169_main.c:3332 */
static VOID
Rtl8168HwStart_EP3(IN PRTL_ADAPTER A)
{
    RtlEphyInit(A, e_info_8168ep_3, ARRAYSIZE(e_info_8168ep_3));

    Rtl8168HwStart_EP_Common(A);

    RtlWriteReg8(A, R_DLLPR, RtlReadReg8(A, R_DLLPR) & ~DLLPR_PFM_EN);
    RtlWriteReg8(A, R_MISC1, RtlReadReg8(A, R_MISC1) & ~MISC1_PFM_D3COLD_EN);

    RtlMacOcpModify(A, 0xD3E2, 0x0FFF, 0x0271);
    RtlMacOcpModify(A, 0xD3E4, 0x00FF, 0x0000);
    RtlMacOcpModify(A, 0xE860, 0x0000, 0x0080);
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
    /* Mirrors hw_configs[] in rtl_hw_config (r8169_main.c:3658).
     * Caller (NICProgramRings) holds Cfg9346 unlocked across this call. */
    switch (A->MacVersion)
    {
    case RTL_MAC_VER_11:    /* RTL8168B/8111B */
    case RTL_MAC_VER_17:
        Rtl8168HwStart_B(A);
        break;

    case RTL_MAC_VER_18:    /* RTL8168CP/8111CP */
        Rtl8168HwStart_CP1(A);
        break;

    case RTL_MAC_VER_19:    /* RTL8168C/8111C */
        Rtl8168HwStart_C1(A);
        break;

    case RTL_MAC_VER_20:
    case RTL_MAC_VER_21:
        Rtl8168HwStart_C2(A);
        break;

    case RTL_MAC_VER_22:
        Rtl8168HwStart_C4(A);
        break;

    case RTL_MAC_VER_23:    /* RTL8168CP/8111CP */
        Rtl8168HwStart_CP2(A);
        break;

    case RTL_MAC_VER_24:
        Rtl8168HwStart_CP3(A);
        break;

    case RTL_MAC_VER_25:    /* RTL8168D/8111D */
    case RTL_MAC_VER_26:
    case RTL_MAC_VER_31:    /* RTL8168DP */
        /* rtl_hw_start_8168d: only PCI-config-space operations (ASPM entry
         * latency, ClkReq); nothing register-level to program. */
        break;

    case RTL_MAC_VER_28:    /* RTL8168DP */
        Rtl8168HwStart_D4(A);
        break;

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
        Rtl8168HwStart_8411(A);
        break;

    case RTL_MAC_VER_40:    /* RTL8168G/8111G */
        Rtl8168HwStart_G1(A);
        break;

    case RTL_MAC_VER_42:    /* RTL8168GU/8111GU */
    case RTL_MAC_VER_43:
        Rtl8168HwStart_G2(A);
        break;

    case RTL_MAC_VER_44:    /* RTL8411B */
        Rtl8168HwStart_8411B(A);
        break;

    case RTL_MAC_VER_46:    /* RTL8168H/8111H */
    case RTL_MAC_VER_48:
        Rtl8168HwStart_H1(A);
        break;

    case RTL_MAC_VER_51:    /* RTL8168EP/8111EP */
        Rtl8168HwStart_EP3(A);
        break;

    default:
        break;
    }
}
