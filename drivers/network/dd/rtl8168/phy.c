/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Realtek 8168/8111 driver
 * FILE:        phy.c
 * PURPOSE:     PHY register tuning, EEE config, PLL power down/up
 *
 * Ported from drivers/net/ethernet/realtek/r8169_phy_config.c (GPLv2):
 *   rtl8168e_1_hw_phy_config (:538)
 *   rtl8168e_2_hw_phy_config (:583)
 *   rtl8168f_hw_phy_config   (:625)
 *   rtl8411_hw_phy_config    (:677)
 *   rtl8168g_1_hw_phy_config (:741)
 *   rtl8168g_2_hw_phy_config (:789)
 *   rtl8168h_2_hw_phy_config (:796)
 *   rtl8168f_config_eee_phy  (:73)
 *   rtl8168g_config_eee_phy  (:79)
 *
 * PLL power: rtl_set_d3_pll_down (r8169_main.c:1452).
 *
 * Notes vs. Linux:
 *   - We skip r8169_apply_firmware(): Linux's chip-specific PHY firmware
 *     patches are loaded from /lib/firmware/rtl_nic and applied via the
 *     OCP MCU patch RAM mechanism.  Auto-neg comes up without them (BIOS has
 *     already brought the PHY up); they only fix marginal-corner gigabit
 *     issues.  Adding firmware loading needs the rtl_fw_loader module from
 *     r8169_firmware.c and a way to publish .fw blobs through ROS's setup.
 *   - We skip the efuse-conditional branches in 8168d_1/d_2 -- those parts
 *     pre-date the EFUSEAR plumbing in the ROS driver.
 *
 * All PHY-config functions are best-effort: if the PHY MDIO interface is
 * timing out we just return; the chip will fall back to BIOS-programmed
 * defaults and the link will still come up.
 */

#include "nic.h"

#define NDEBUG
#include <debug.h>

/*===========================================================================
 *  Linux phy-page helpers re-expressed for the ROS MDIO accessors
 *==========================================================================*/

/* r8168d_modify_extpage(phydev, extpage, reg, mask, val) is:
 *   select page 0x0007, write 0x1e=extpage, modify reg, restore page. */
static VOID
RtlExtPageModify(IN PRTL_ADAPTER A, IN USHORT ExtPage, IN ULONG Reg,
                 IN USHORT Mask, IN USHORT Val)
{
    USHORT cur;

    RtlMdioWrite(A, 0x1F, 0x0007);
    RtlMdioWrite(A, 0x1E, ExtPage);
    cur = RtlMdioRead(A, Reg);
    RtlMdioWrite(A, Reg, (USHORT)((cur & ~Mask) | Val));
    RtlMdioWrite(A, 0x1F, 0);
}

/* r8168d_phy_param(phydev, parm, mask, val): page 0x0005, idx 0x05=parm,
 * modify 0x06. */
static VOID
RtlPhyParamD(IN PRTL_ADAPTER A, IN USHORT Parm, IN USHORT Mask, IN USHORT Val)
{
    USHORT cur;

    RtlMdioWrite(A, 0x1F, 0x0005);
    RtlMdioWrite(A, 0x05, Parm);
    cur = RtlMdioRead(A, 0x06);
    RtlMdioWrite(A, 0x06, (USHORT)((cur & ~Mask) | Val));
    RtlMdioWrite(A, 0x1F, 0);
}

/* r8168g_phy_param(phydev, parm, mask, val): page 0x0a43, idx 0x13=parm,
 * modify 0x14. */
static VOID
RtlPhyParamG(IN PRTL_ADAPTER A, IN USHORT Parm, IN USHORT Mask, IN USHORT Val)
{
    USHORT cur;

    RtlMdioWrite(A, 0x1F, 0x0A43);
    RtlMdioWrite(A, 0x13, Parm);
    cur = RtlMdioRead(A, 0x14);
    RtlMdioWrite(A, 0x14, (USHORT)((cur & ~Mask) | Val));
    RtlMdioWrite(A, 0x1F, 0);
}

/* phy_set_bits / phy_clear_bits at current page */
static VOID
RtlPhySetBits(IN PRTL_ADAPTER A, IN ULONG Reg, IN USHORT Bits)
{
    USHORT v = RtlMdioRead(A, Reg);
    RtlMdioWrite(A, Reg, (USHORT)(v | Bits));
}

static VOID
RtlPhyClearBits(IN PRTL_ADAPTER A, IN ULONG Reg, IN USHORT Bits)
{
    USHORT v = RtlMdioRead(A, Reg);
    RtlMdioWrite(A, Reg, (USHORT)(v & ~Bits));
}

/* phy_write_mmd via the clause-22 indirect registers (mmd_phy_indirect,
 * drivers/net/phy/phy-core.c) -- caller must be at page 0. */
static VOID
RtlPhyWriteMmd(IN PRTL_ADAPTER A, IN USHORT DevAd, IN USHORT RegNum, IN USHORT Val)
{
    /* Write the desired MMD Devad */
    RtlMdioWrite(A, MII_MMD_CTRL, DevAd);
    /* Write the desired MMD register address */
    RtlMdioWrite(A, MII_MMD_DATA, RegNum);
    /* Select the Function : DATA with no post increment */
    RtlMdioWrite(A, MII_MMD_CTRL, (USHORT)(DevAd | MII_MMD_CTRL_NOINCR));
    /* Write the data into MMD's selected register */
    RtlMdioWrite(A, MII_MMD_DATA, Val);
}

/*===========================================================================
 *  EEE PHY config
 *==========================================================================*/

/* rtl8168f_config_eee_phy -- r8169_phy_config.c:73 */
static VOID
Rtl8168FConfigEeePhy(IN PRTL_ADAPTER A)
{
    RtlExtPageModify(A, 0x0020, 0x15, 0, 0x0100);   /* BIT(8) */
    RtlPhyParamD(A, 0x8B85, 0, 0x2000);             /* BIT(13) */
}

/* rtl8168g_config_eee_phy -- r8169_phy_config.c:79 */
static VOID
Rtl8168GConfigEeePhy(IN PRTL_ADAPTER A)
{
    RtlPhyModifyPaged(A, 0x0A43, 0x11, 0, 0x0010);  /* BIT(4) */
}

/*===========================================================================
 *  Per-chip PHY config
 *==========================================================================*/

/* rtl8168e_1_hw_phy_config -- r8169_phy_config.c:538.
 * Firmware-apply call dropped (see file-header note).  Note: unlike the
 * E-VL (e_2) path this one DISABLES EEE advertisement at the end. */
static VOID
RtlHwPhyConfig_E1(IN PRTL_ADAPTER A)
{
    /* Enable Delay cap */
    RtlPhyParamD(A, 0x8B80, 0xFFFF, 0xC896);

    /* Channel estimation fine tune */
    RtlMdioWrite(A, 0x1F, 0x0001);
    RtlMdioWrite(A, 0x0B, 0x6C20);
    RtlMdioWrite(A, 0x07, 0x2872);
    RtlMdioWrite(A, 0x1C, 0xEFFF);
    RtlMdioWrite(A, 0x1F, 0x0003);
    RtlMdioWrite(A, 0x14, 0x6420);
    RtlMdioWrite(A, 0x1F, 0x0000);

    /* Update PFM & 10M TX idle timer */
    RtlExtPageModify(A, 0x002F, 0x15, 0xFFFF, 0x1919);

    RtlExtPageModify(A, 0x00AC, 0x18, 0xFFFF, 0x0006);

    /* DCO enable for 10M IDLE Power */
    RtlExtPageModify(A, 0x0023, 0x17, 0x0000, 0x0006);

    /* For impedance matching */
    RtlPhyModifyPaged(A, 0x0002, 0x08, 0x7F00, 0x8000);

    /* PHY auto speed down */
    RtlExtPageModify(A, 0x002D, 0x18, 0x0000, 0x0050);
    RtlPhySetBits(A, 0x14, 0x8000);                 /* BIT(15) */

    RtlPhyParamD(A, 0x8B86, 0x0000, 0x0001);
    RtlPhyParamD(A, 0x8B85, 0x2000, 0x0000);

    RtlExtPageModify(A, 0x0020, 0x15, 0x1100, 0x0000);
    RtlPhyWritePaged(A, 0x0006, 0x00, 0x5A00);

    RtlPhyWriteMmd(A, MDIO_MMD_AN, MDIO_AN_EEE_ADV, 0x0000);
}

/* rtl8168e_2_hw_phy_config -- r8169_phy_config.c:583.
 * Firmware-apply call dropped (see file-header note). */
static VOID
RtlHwPhyConfig_E2(IN PRTL_ADAPTER A)
{
    /* Enable delay cap */
    RtlExtPageModify(A, 0x00AC, 0x18, 0xFFFF, 0x0006);

    /* Channel estimation fine tune */
    RtlPhyWritePaged(A, 0x0003, 0x09, 0xA20F);

    /* Green setting */
    RtlPhyParamD(A, 0x8B5B, 0xFFFF, 0x9222);
    RtlPhyParamD(A, 0x8B6D, 0xFFFF, 0x8000);
    RtlPhyParamD(A, 0x8B76, 0xFFFF, 0x8000);

    /* 4-corner perf */
    RtlMdioWrite(A, 0x1F, 0x0005);
    RtlMdioWrite(A, 0x05, 0x8B80);
    RtlPhySetBits(A, 0x17, 0x0006);
    RtlMdioWrite(A, 0x1F, 0x0000);

    /* Auto speed down */
    RtlExtPageModify(A, 0x002D, 0x18, 0x0000, 0x0010);
    RtlPhySetBits(A, 0x14, 0x8000);

    /* 10M EEE waveform */
    RtlPhyParamD(A, 0x8B86, 0x0000, 0x0001);

    /* 2-pair detect */
    RtlPhyParamD(A, 0x8B85, 0x0000, 0x4000);

    Rtl8168FConfigEeePhy(A);

    /* Green feature */
    RtlMdioWrite(A, 0x1F, 0x0003);
    RtlPhySetBits(A, 0x19, 0x0001);
    RtlPhySetBits(A, 0x10, 0x0400);                 /* BIT(10) */
    RtlMdioWrite(A, 0x1F, 0x0000);
    RtlPhyModifyPaged(A, 0x0005, 0x01, 0, 0x0100); /* BIT(8) */
}

/* rtl8168f_hw_phy_config -- r8169_phy_config.c:625 */
static VOID
RtlHwPhyConfig_F(IN PRTL_ADAPTER A)
{
    RtlPhyParamD(A, 0x8B80, 0x0000, 0x0006);
    RtlExtPageModify(A, 0x002D, 0x18, 0x0000, 0x0010);
    RtlPhySetBits(A, 0x14, 0x8000);
    RtlPhyParamD(A, 0x8B86, 0x0000, 0x0001);
    Rtl8168FConfigEeePhy(A);
}

/* rtl8168f_1_hw_phy_config -- r8169_phy_config.c:641.  Firmware-apply
 * dropped. */
static VOID
RtlHwPhyConfig_F1(IN PRTL_ADAPTER A)
{
    RtlPhyWritePaged(A, 0x0003, 0x09, 0xA20F);

    /* Modify green table for giga & fnet */
    RtlPhyParamD(A, 0x8B55, 0xFFFF, 0x0000);
    RtlPhyParamD(A, 0x8B5E, 0xFFFF, 0x0000);
    RtlPhyParamD(A, 0x8B67, 0xFFFF, 0x0000);
    RtlPhyParamD(A, 0x8B70, 0xFFFF, 0x0000);
    RtlExtPageModify(A, 0x0078, 0x17, 0xFFFF, 0x0000);
    RtlExtPageModify(A, 0x0078, 0x19, 0xFFFF, 0x00FB);

    /* Modify green table for 10M */
    RtlPhyParamD(A, 0x8B79, 0xFFFF, 0xAA00);

    /* Disable hiimpedance detection (RTCT) */
    RtlPhyWritePaged(A, 0x0003, 0x01, 0x328A);

    RtlHwPhyConfig_F(A);

    RtlPhyParamD(A, 0x8B85, 0x0000, 0x4000);
}

/* rtl8411_hw_phy_config -- r8169_phy_config.c:677.  Firmware-apply
 * dropped.  F-family base plus 8411-specific green-table params; the
 * green feature is CLEARED at the end (F_1 sets it on E-VL only). */
static VOID
RtlHwPhyConfig_8411(IN PRTL_ADAPTER A)
{
    RtlHwPhyConfig_F(A);

    /* Improve 2-pair detection performance */
    RtlPhyParamD(A, 0x8B85, 0x0000, 0x4000);

    /* Channel estimation fine tune */
    RtlPhyWritePaged(A, 0x0003, 0x09, 0xA20F);

    /* Modify green table for giga & fnet */
    RtlPhyParamD(A, 0x8B55, 0xFFFF, 0x0000);
    RtlPhyParamD(A, 0x8B5E, 0xFFFF, 0x0000);
    RtlPhyParamD(A, 0x8B67, 0xFFFF, 0x0000);
    RtlPhyParamD(A, 0x8B70, 0xFFFF, 0x0000);
    RtlExtPageModify(A, 0x0078, 0x17, 0xFFFF, 0x0000);
    RtlExtPageModify(A, 0x0078, 0x19, 0xFFFF, 0x00AA);

    /* Modify green table for 10M */
    RtlPhyParamD(A, 0x8B79, 0xFFFF, 0xAA00);

    /* Disable hiimpedance detection (RTCT) */
    RtlPhyWritePaged(A, 0x0003, 0x01, 0x328A);

    /* Modify green table for giga */
    RtlPhyParamD(A, 0x8B54, 0x0800, 0x0000);
    RtlPhyParamD(A, 0x8B5D, 0x0800, 0x0000);
    RtlPhyParamD(A, 0x8A7C, 0x0100, 0x0000);
    RtlPhyParamD(A, 0x8A7F, 0x0000, 0x0100);
    RtlPhyParamD(A, 0x8A82, 0x0100, 0x0000);
    RtlPhyParamD(A, 0x8A85, 0x0100, 0x0000);
    RtlPhyParamD(A, 0x8A88, 0x0100, 0x0000);

    /* uc same-seed solution */
    RtlPhyParamD(A, 0x8B85, 0x0000, 0x8000);

    /* Green feature */
    RtlMdioWrite(A, 0x1F, 0x0003);
    RtlPhyClearBits(A, 0x19, 0x0001);               /* BIT(0) */
    RtlPhyClearBits(A, 0x10, 0x0400);               /* BIT(10) */
    RtlMdioWrite(A, 0x1F, 0x0000);
}

/* PHY helpers used by 8168g / 8168h configs */
static VOID
Rtl8168GDisableAldps(IN PRTL_ADAPTER A)
{
    /* phy_modify_paged(phydev, 0x0a43, 0x10, BIT(2), 0) */
    RtlPhyModifyPaged(A, 0x0A43, 0x10, 0x0004, 0);
}

static VOID
Rtl8168GEnableGphy10m(IN PRTL_ADAPTER A)
{
    /* phy_modify_paged(phydev, 0x0a44, 0x11, 0, BIT(11)) */
    RtlPhyModifyPaged(A, 0x0A44, 0x11, 0, 0x0800);
}

/* rtl8168g_phy_adjust_10m_aldps -- r8169_phy_config.c:733 */
static VOID
Rtl8168GPhyAdjust10mAldps(IN PRTL_ADAPTER A)
{
    RtlPhyModifyPaged(A, 0x0BCC, 0x14, 0x0100, 0);  /* BIT(8) */
    RtlPhyModifyPaged(A, 0x0A44, 0x11, 0, 0x00C0);  /* BIT(7) | BIT(6) */
    RtlPhyParamG(A, 0x8084, 0x6000, 0x0000);
    RtlPhyModifyPaged(A, 0x0A43, 0x10, 0x0000, 0x1003);
}

/* rtl8168g_1_hw_phy_config -- r8169_phy_config.c:741.  Firmware-apply
 * dropped. */
static VOID
RtlHwPhyConfig_G1(IN PRTL_ADAPTER A)
{
    USHORT v;

    v = RtlPhyReadPaged(A, 0x0A46, 0x10);
    if (v & 0x0100)                                 /* BIT(8) */
        RtlPhyModifyPaged(A, 0x0BCC, 0x12, 0x8000, 0);
    else
        RtlPhyModifyPaged(A, 0x0BCC, 0x12, 0, 0x8000);

    v = RtlPhyReadPaged(A, 0x0A46, 0x13);
    if (v & 0x0100)                                 /* BIT(8) */
        RtlPhyModifyPaged(A, 0x0C41, 0x15, 0, 0x0002);
    else
        RtlPhyModifyPaged(A, 0x0C41, 0x15, 0x0002, 0);

    /* Enable PHY auto speed down */
    RtlPhyModifyPaged(A, 0x0A44, 0x11, 0, 0x000C);  /* BIT(3) | BIT(2) */

    Rtl8168GPhyAdjust10mAldps(A);

    /* EEE auto-fallback function */
    RtlPhyModifyPaged(A, 0x0A4B, 0x11, 0, 0x0004);  /* BIT(2) */

    /* Enable UC LPF tune function */
    RtlPhyParamG(A, 0x8012, 0x0000, 0x8000);

    RtlPhyModifyPaged(A, 0x0C42, 0x11, 0x2000, 0x4000); /* ~BIT(13), BIT(14) */

    /* Improve SWR Efficiency */
    RtlMdioWrite(A, 0x1F, 0x0BCD);
    RtlMdioWrite(A, 0x14, 0x5065);
    RtlMdioWrite(A, 0x14, 0xD065);
    RtlMdioWrite(A, 0x1F, 0x0BC8);
    RtlMdioWrite(A, 0x11, 0x5655);
    RtlMdioWrite(A, 0x1F, 0x0BCD);
    RtlMdioWrite(A, 0x14, 0x1065);
    RtlMdioWrite(A, 0x14, 0x9065);
    RtlMdioWrite(A, 0x14, 0x1065);
    RtlMdioWrite(A, 0x1F, 0x0000);

    Rtl8168GDisableAldps(A);
    Rtl8168GConfigEeePhy(A);
}

/* rtl8168g_2_hw_phy_config -- r8169_phy_config.c:789.  Firmware dropped,
 * leaving the EEE setup. */
static VOID
RtlHwPhyConfig_G2(IN PRTL_ADAPTER A)
{
    Rtl8168GConfigEeePhy(A);
}

/* rtl8168h_2_hw_phy_config -- r8169_phy_config.c:796.  Adapter-bias ioffset
 * read (rtl8168h_2_get_adc_bias_ioffset) requires extra EFUSE plumbing that
 * isn't in this minimal port; we skip it and let the default 0xffff path
 * (== "no ioffset adjustment") apply. */
static VOID
RtlHwPhyConfig_H2(IN PRTL_ADAPTER A)
{
    USHORT data, rlen;

    /* CHIN EST parameter update */
    RtlPhyParamG(A, 0x808A, 0x003F, 0x000A);

    /* Enable R-tune + PGA-retune */
    RtlPhyParamG(A, 0x0811, 0x0000, 0x0800);
    RtlPhyModifyPaged(A, 0x0A42, 0x16, 0x0000, 0x0002);

    Rtl8168GEnableGphy10m(A);

    /* Modify rlen (TX LPF corner frequency) level */
    data = RtlPhyReadPaged(A, 0x0BCD, 0x16);
    data &= 0x000F;
    rlen = 0;
    if (data > 3)
        rlen = (USHORT)(data - 3);
    data = (USHORT)(rlen | (rlen << 4) | (rlen << 8) | (rlen << 12));
    RtlPhyWritePaged(A, 0x0BCD, 0x17, data);

    /* Disable PHY PFM mode */
    RtlPhyModifyPaged(A, 0x0A44, 0x11, 0x0080, 0);
    /* Disable 10m PLL off */
    RtlPhyModifyPaged(A, 0x0A43, 0x10, 0x0001, 0);

    Rtl8168GDisableAldps(A);
    Rtl8168GConfigEeePhy(A);
}

/*===========================================================================
 *  Dispatcher
 *==========================================================================*/

VOID
NTAPI
RtlHwPhyConfig (
    IN PRTL_ADAPTER A
    )
{
    switch (A->MacVersion)
    {
    case RTL_MAC_VER_32:    /* 8168E */
    case RTL_MAC_VER_33:
        RtlHwPhyConfig_E1(A);
        break;

    case RTL_MAC_VER_34:    /* 8168E-VL */
        RtlHwPhyConfig_E2(A);
        break;

    case RTL_MAC_VER_35:    /* 8168F-1 */
        RtlHwPhyConfig_F1(A);
        break;

    case RTL_MAC_VER_36:    /* 8168F-2 -- Linux rtl8168f_2_hw_phy_config is
                             * just firmware + the shared F base. */
        RtlHwPhyConfig_F(A);
        break;

    case RTL_MAC_VER_38:    /* 8411 */
        RtlHwPhyConfig_8411(A);
        break;

    case RTL_MAC_VER_40:    /* 8168G */
        RtlHwPhyConfig_G1(A);
        break;

    case RTL_MAC_VER_42:    /* 8168GU */
    case RTL_MAC_VER_43:
    case RTL_MAC_VER_44:    /* 8411B -- OCP MDIO chip, G_2 config */
        RtlHwPhyConfig_G2(A);
        break;

    case RTL_MAC_VER_46:    /* 8168H */
    case RTL_MAC_VER_48:
        RtlHwPhyConfig_H2(A);
        break;

    default:
        /* Older chips (B/C/CP/D series) -- BIOS leaves the PHY in a working
         * state.  No tunes applied; gigabit may run on a sub-optimal SI
         * profile but link comes up. */
        break;
    }
}

/*===========================================================================
 *  PLL power up/down -- rtl_set_d3_pll_down (r8169_main.c:1452)
 *
 *  Linux only programs PMCH on the chips listed below; everything else has
 *  no PMCH-style power gate.  enable=TRUE means "let the PLL drop on D3"
 *  (low-power); enable=FALSE means "keep PLL spinning" (driver active).
 *==========================================================================*/

static BOOLEAN
RtlChipNeedsPllControl(IN PRTL_ADAPTER A)
{
    RTL_MAC_VER v = A->MacVersion;
    /* Mirror the ranges from rtl_set_d3_pll_down. */
    if (v == RTL_MAC_VER_25 || v == RTL_MAC_VER_26) return TRUE;
    if (v == RTL_MAC_VER_29 || v == RTL_MAC_VER_30) return TRUE;
    if (v >= RTL_MAC_VER_32 && v <= RTL_MAC_VER_37) return TRUE;
    if (v >= RTL_MAC_VER_39) return TRUE;
    return FALSE;
}

VOID
NTAPI
RtlPllPowerUp (
    IN PRTL_ADAPTER A
    )
{
    UCHAR pmch;

    if (!RtlChipNeedsPllControl(A))
        return;

    /* "PLL up while in D3" -- set the D3_NO_PLL_DOWN bits. */
    pmch = RtlReadReg8(A, R_PMCH);
    RtlWriteReg8(A, R_PMCH, pmch | D3_NO_PLL_DOWN);

    /* Take the PHY out of BMCR power-down if BIOS / wake left it asleep. */
    {
        USHORT bmcr = RtlMdioRead(A, MII_BMCR);
        if (bmcr & BMCR_PDOWN)
        {
            RtlMdioWrite(A, MII_BMCR, (USHORT)(bmcr & ~BMCR_PDOWN));
            NdisStallExecution(100);
            /* Kick auto-neg restart so the link re-trains. */
            RtlMdioWrite(A, MII_BMCR,
                         (USHORT)((bmcr & ~BMCR_PDOWN) | BMCR_ANENABLE | BMCR_ANRESTART));
        }
    }
}

VOID
NTAPI
RtlPllPowerDown (
    IN PRTL_ADAPTER A
    )
{
    UCHAR pmch;

    if (!RtlChipNeedsPllControl(A))
        return;

    /* Allow PLL to drop on D3 entry (clear D3_NO_PLL_DOWN). */
    pmch = RtlReadReg8(A, R_PMCH);
    RtlWriteReg8(A, R_PMCH, pmch & ~D3_NO_PLL_DOWN);

    /* Put PHY in power-down via BMCR.PDOWN; saves ~150mW on idle. */
    {
        USHORT bmcr = RtlMdioRead(A, MII_BMCR);
        RtlMdioWrite(A, MII_BMCR, (USHORT)(bmcr | BMCR_PDOWN));
    }
}
