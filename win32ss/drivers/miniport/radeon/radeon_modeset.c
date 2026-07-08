/*
 * PROJECT:     ReactOS AMD Radeon ATOM-BIOS Framebuffer Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ATOM-driven mode-setting for DCE6-class parts (Trinity/
 *              Richland ARUBA DCE6.1 and OLAND DCE6.4).  Narrow port of the
 *              CRTC0 + single DIG/UNIPHY path of Linux v6.6
 *              drivers/gpu/drm/radeon/atombios_crtc.c,
 *              atombios_encoders.c and radeon_display.c
 *              (radeon_compute_pll_avivo).  The command tables executed
 *              are, in call order:
 *                UNIPHYTransmitterControl (INIT, once)
 *                SetPixelClock (display engine clock, once)
 *                AdjustDisplayPll
 *                SelectCRTC_Source
 *                EnableDispPowerGating (disable)
 *                UpdateCRTC_DoubleBufferRegisters (lock/unlock)
 *                BlankCRTC / EnableCRTC
 *                EnableSpreadSpectrumOnPPLL (disable)
 *                SetPixelClock (pixel PLL)
 *                SetCRTC_UsingDTDTiming
 *                SetCRTC_OverScan
 *                EnableScaler (disable)
 *                DIGxEncoderControl (SETUP, SETUP_PANEL_MODE)
 *                UNIPHYTransmitterControl (DISABLE/ENABLE/LCD_BLOFF/BLON)
 *              The scanout surface itself is programmed via direct GRPH_*
 *              register writes (port of dce4_crtc_do_set_base, linear
 *              XRGB8888 only).
 * COPYRIGHT:   Copyright 2007-2013 Advanced Micro Devices, Inc.
 *              Copyright 2008 Red Hat Inc.
 *              Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "radeon.h"

#define RADEON_CRTC_ID      0
#define RADEON_CRTC_OFFSET  EVERGREEN_CRTC0_REGISTER_OFFSET

static BOOLEAN
RadeonIsDce61(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    return (DeviceExtension->Family >= CHIP_ARUBA) &&
           (DeviceExtension->ChipFlags & RADEONFB_IS_IGP) != 0;
}

/* Port of radeon_dig_monitor_is_duallink for the outputs we drive: DVI is
 * dual-link above 165MHz; LVDS panels return false (the VBIOS transmitter
 * table handles dual-link LVDS internally, as in Linux). */
static BOOLEAN
RadeonIsDualLink(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG PixelClockKHz)
{
    if (DeviceExtension->EncoderMode == ATOM_ENCODER_MODE_DVI)
        return PixelClockKHz > 165000;
    return FALSE;
}

/* PLL COMPUTATION (port of radeon_display.c) ********************************/

static unsigned
RadeonGcd(unsigned a, unsigned b)
{
    while (b != 0)
    {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

#define DIV_ROUND_UP_U(n, d)      (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST_U(n, d) (((n) + (d) / 2) / (d))

/* Port of avivo_reduce_ratio */
static void
avivo_reduce_ratio(unsigned *nom, unsigned *den,
                   unsigned nom_min, unsigned den_min)
{
    unsigned tmp;

    /* reduce the numbers to a simpler ratio */
    tmp = RadeonGcd(*nom, *den);
    *nom /= tmp;
    *den /= tmp;

    /* make sure nominator is large enough */
    if (*nom < nom_min)
    {
        tmp = DIV_ROUND_UP_U(nom_min, *nom);
        *nom *= tmp;
        *den *= tmp;
    }

    /* make sure the denominator is large enough */
    if (*den < den_min)
    {
        tmp = DIV_ROUND_UP_U(den_min, *den);
        *nom *= tmp;
        *den *= tmp;
    }
}

/* Port of avivo_get_fb_ref_div */
static void
avivo_get_fb_ref_div(unsigned nom, unsigned den, unsigned post_div,
                     unsigned fb_div_max, unsigned ref_div_max,
                     unsigned *fb_div, unsigned *ref_div)
{
    /* limit reference * post divider to a maximum:
     * ref_div_max = max(min(100 / post_div, ref_div_max), 1u) */
    if (ref_div_max > 100 / post_div)
        ref_div_max = 100 / post_div;
    if (ref_div_max < 1)
        ref_div_max = 1;

    /* get matching reference and feedback divider:
     * *ref_div = min(max(den / post_div, 1u), ref_div_max) */
    *ref_div = den / post_div;
    if (*ref_div < 1)
        *ref_div = 1;
    if (*ref_div > ref_div_max)
        *ref_div = ref_div_max;
    *fb_div = DIV_ROUND_CLOSEST_U(nom * *ref_div * post_div, den);

    /* limit fb divider to its maximum */
    if (*fb_div > fb_div_max)
    {
        *ref_div = (*ref_div * fb_div_max) / (*fb_div);
        *fb_div = fb_div_max;
    }
}

/* Port of radeon_compute_pll_avivo.
 * dot_clock = (ref_freq * feedback_div) / (ref_div * post_div) */
static void
RadeonComputePllAvivo(
    _In_ PRADEONFB_PLL pll,
    _In_ ULONG freq,
    _Out_ PULONG dot_clock_p,
    _Out_ PULONG fb_div_p,
    _Out_ PULONG frac_fb_div_p,
    _Out_ PULONG ref_div_p,
    _Out_ PULONG post_div_p)
{
    unsigned target_clock = (pll->Flags & RADEONFB_PLL_USE_FRAC_FB_DIV) ?
        freq : freq / 10;

    unsigned fb_div_min, fb_div_max, fb_div;
    unsigned post_div_min, post_div_max, post_div;
    unsigned ref_div_min, ref_div_max, ref_div;
    unsigned post_div_best, diff_best;
    unsigned nom, den;

    /* determine allowed feedback divider range */
    fb_div_min = pll->MinFeedbackDiv;
    fb_div_max = pll->MaxFeedbackDiv;

    if (pll->Flags & RADEONFB_PLL_USE_FRAC_FB_DIV)
    {
        fb_div_min *= 10;
        fb_div_max *= 10;
    }

    /* determine allowed ref divider range */
    if (pll->Flags & RADEONFB_PLL_USE_REF_DIV)
        ref_div_min = pll->ReferenceDiv;
    else
        ref_div_min = pll->MinRefDiv;

    if ((pll->Flags & RADEONFB_PLL_USE_FRAC_FB_DIV) &&
        (pll->Flags & RADEONFB_PLL_USE_REF_DIV))
        ref_div_max = pll->ReferenceDiv;
    else if (pll->Flags & RADEONFB_PLL_PREFER_MINM_OVER_MAXP)
        /* fix for problems on RS880 */
        ref_div_max = (pll->MaxRefDiv < 7) ? pll->MaxRefDiv : 7;
    else
        ref_div_max = pll->MaxRefDiv;

    /* determine allowed post divider range */
    if (pll->Flags & RADEONFB_PLL_USE_POST_DIV)
    {
        post_div_min = pll->PostDiv;
        post_div_max = pll->PostDiv;
    }
    else
    {
        unsigned vco_min, vco_max;

        if (pll->Flags & RADEONFB_PLL_IS_LCD)
        {
            vco_min = pll->LcdPllOutMin;
            vco_max = pll->LcdPllOutMax;
        }
        else
        {
            vco_min = pll->PllOutMin;
            vco_max = pll->PllOutMax;
        }

        if (pll->Flags & RADEONFB_PLL_USE_FRAC_FB_DIV)
        {
            vco_min *= 10;
            vco_max *= 10;
        }

        post_div_min = vco_min / target_clock;
        if ((target_clock * post_div_min) < vco_min)
            ++post_div_min;
        if (post_div_min < pll->MinPostDiv)
            post_div_min = pll->MinPostDiv;

        post_div_max = vco_max / target_clock;
        if ((target_clock * post_div_max) > vco_max)
            --post_div_max;
        if (post_div_max > pll->MaxPostDiv)
            post_div_max = pll->MaxPostDiv;
    }

    /* represent the searched ratio as fractional number */
    nom = target_clock;
    den = pll->ReferenceFreq;

    /* reduce the numbers to a simpler ratio */
    avivo_reduce_ratio(&nom, &den, fb_div_min, post_div_min);

    /* now search for a post divider */
    if (pll->Flags & RADEONFB_PLL_PREFER_MINM_OVER_MAXP)
        post_div_best = post_div_min;
    else
        post_div_best = post_div_max;
    diff_best = ~0u;

    for (post_div = post_div_min; post_div <= post_div_max; ++post_div)
    {
        unsigned diff;
        unsigned clk;

        avivo_get_fb_ref_div(nom, den, post_div, fb_div_max,
                             ref_div_max, &fb_div, &ref_div);
        clk = (pll->ReferenceFreq * fb_div) / (ref_div * post_div);
        diff = (clk > target_clock) ? clk - target_clock : target_clock - clk;

        if (diff < diff_best ||
            (diff == diff_best &&
             !(pll->Flags & RADEONFB_PLL_PREFER_MINM_OVER_MAXP)))
        {
            post_div_best = post_div;
            diff_best = diff;
        }
    }
    post_div = post_div_best;

    /* get the feedback and reference divider for the optimal value */
    avivo_get_fb_ref_div(nom, den, post_div, fb_div_max, ref_div_max,
                         &fb_div, &ref_div);

    /* reduce the numbers to a simpler ratio once more */
    /* this also makes sure that the reference divider is large enough */
    avivo_reduce_ratio(&fb_div, &ref_div, fb_div_min, ref_div_min);

    /* avoid high jitter with small fractional dividers */
    if ((pll->Flags & RADEONFB_PLL_USE_FRAC_FB_DIV) && (fb_div % 10))
    {
        fb_div_min = (9 - (fb_div % 10)) * 20 + 50;
        if (fb_div_min < pll->MinFeedbackDiv * 10)
            fb_div_min = pll->MinFeedbackDiv * 10;
        if (fb_div < fb_div_min)
        {
            unsigned tmp = DIV_ROUND_UP_U(fb_div_min, fb_div);
            fb_div *= tmp;
            ref_div *= tmp;
        }
    }

    /* and finally save the result */
    if (pll->Flags & RADEONFB_PLL_USE_FRAC_FB_DIV)
    {
        *fb_div_p = fb_div / 10;
        *frac_fb_div_p = fb_div % 10;
    }
    else
    {
        *fb_div_p = fb_div;
        *frac_fb_div_p = 0;
    }

    *dot_clock_p = ((pll->ReferenceFreq * *fb_div_p * 10) +
                    (pll->ReferenceFreq * *frac_fb_div_p)) /
                   (ref_div * post_div * 10);
    *ref_div_p = ref_div;
    *post_div_p = post_div;

    VideoPortDebugPrint(Trace,
        "radeonfb: %lu - %lu, pll dividers - fb: %lu.%lu ref: %lu, post %lu\n",
        freq, *dot_clock_p * 10, *fb_div_p, *frac_fb_div_p,
        (ULONG)ref_div, (ULONG)post_div);
}

/* ATOM TABLE WRAPPERS *******************************************************/

static BOOLEAN
RadeonExecuteTable(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ int Index,
    _In_ PVOID Args)
{
    if (atom_execute_table(DeviceExtension->AtomContext, Index,
                           (uint32_t *)Args) != 0)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: ATOM command table %d failed\n", Index);
        return FALSE;
    }
    return TRUE;
}

/* Port of atombios_lock_crtc */
static BOOLEAN
RadeonLockCrtc(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ int Lock)
{
    int index =
        GetIndexIntoMasterTable(COMMAND, UpdateCRTC_DoubleBufferRegisters);
    ENABLE_CRTC_PS_ALLOCATION args;

    memset(&args, 0, sizeof(args));
    args.ucCRTC = RADEON_CRTC_ID;
    args.ucEnable = (UCHAR)Lock;
    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* Port of atombios_enable_crtc */
static BOOLEAN
RadeonEnableCrtc(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ int State)
{
    int index = GetIndexIntoMasterTable(COMMAND, EnableCRTC);
    ENABLE_CRTC_PS_ALLOCATION args;

    memset(&args, 0, sizeof(args));
    args.ucCRTC = RADEON_CRTC_ID;
    args.ucEnable = (UCHAR)State;
    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* Port of atombios_blank_crtc (no DCE8 VGA-control dance needed) */
static BOOLEAN
RadeonBlankCrtc(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ int State)
{
    int index = GetIndexIntoMasterTable(COMMAND, BlankCRTC);
    BLANK_CRTC_PS_ALLOCATION args;

    memset(&args, 0, sizeof(args));
    args.ucCRTC = RADEON_CRTC_ID;
    args.ucBlanking = (UCHAR)State;
    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* Port of atombios_powergate_crtc */
static BOOLEAN
RadeonPowerGateCrtc(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ int State)
{
    int index = GetIndexIntoMasterTable(COMMAND, EnableDispPowerGating);
    ENABLE_DISP_POWER_GATING_PARAMETERS_V2_1 args;

    memset(&args, 0, sizeof(args));
    args.ucDispPipeId = RADEON_CRTC_ID;
    args.ucEnable = (UCHAR)State;
    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* Port of atombios_set_crtc_dtd_timing (no borders) */
static BOOLEAN
RadeonSetCrtcDtdTiming(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PRADEONFB_TIMING Mode)
{
    SET_CRTC_USING_DTD_TIMING_PARAMETERS args;
    int index = GetIndexIntoMasterTable(COMMAND, SetCRTC_UsingDTDTiming);
    uint16_t misc = 0;

    memset(&args, 0, sizeof(args));
    args.usH_Size = cpu_to_le16((USHORT)Mode->HDisplay);
    args.usH_Blanking_Time =
        cpu_to_le16((USHORT)(Mode->HTotal - Mode->HDisplay));
    args.usV_Size = cpu_to_le16((USHORT)Mode->VDisplay);
    args.usV_Blanking_Time =
        cpu_to_le16((USHORT)(Mode->VTotal - Mode->VDisplay));
    args.usH_SyncOffset =
        cpu_to_le16((USHORT)(Mode->HSyncStart - Mode->HDisplay));
    args.usH_SyncWidth =
        cpu_to_le16((USHORT)(Mode->HSyncEnd - Mode->HSyncStart));
    args.usV_SyncOffset =
        cpu_to_le16((USHORT)(Mode->VSyncStart - Mode->VDisplay));
    args.usV_SyncWidth =
        cpu_to_le16((USHORT)(Mode->VSyncEnd - Mode->VSyncStart));
    args.ucH_Border = 0;
    args.ucV_Border = 0;

    if (Mode->Flags & RADEONFB_MODE_FLAG_NVSYNC)
        misc |= ATOM_VSYNC_POLARITY;
    if (Mode->Flags & RADEONFB_MODE_FLAG_NHSYNC)
        misc |= ATOM_HSYNC_POLARITY;
    if (Mode->Flags & RADEONFB_MODE_FLAG_CSYNC)
        misc |= ATOM_COMPOSITESYNC;
    if (Mode->Flags & RADEONFB_MODE_FLAG_INTERLACE)
        misc |= ATOM_INTERLACE;
    if (Mode->Flags & RADEONFB_MODE_FLAG_DBLSCAN)
        misc |= ATOM_H_REPLICATIONBY2 | ATOM_V_REPLICATIONBY2;

    args.susModeMiscInfo.usAccess = cpu_to_le16(misc);
    args.ucCRTC = RADEON_CRTC_ID;

    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* Port of atombios_overscan_setup (RMX_OFF: all zero) */
static BOOLEAN
RadeonOverscanSetup(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    SET_CRTC_OVERSCAN_PS_ALLOCATION args;
    int index = GetIndexIntoMasterTable(COMMAND, SetCRTC_OverScan);

    memset(&args, 0, sizeof(args));
    args.ucCRTC = RADEON_CRTC_ID;
    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* Port of atombios_scaler_setup (non-TV, RMX off => scaler disable) */
static BOOLEAN
RadeonScalerSetup(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    ENABLE_SCALER_PS_ALLOCATION args;
    int index = GetIndexIntoMasterTable(COMMAND, EnableScaler);

    memset(&args, 0, sizeof(args));
    args.ucScaler = RADEON_CRTC_ID;
    args.ucEnable = ATOM_SCALER_DISABLE;
    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* Port of atombios_crtc_program_ss, DCE5+/v3 branch, disable only (no
 * spread spectrum is enabled by this driver). */
static BOOLEAN
RadeonDisableSpreadSpectrum(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    int index = GetIndexIntoMasterTable(COMMAND, EnableSpreadSpectrumOnPPLL);
    ENABLE_SPREAD_SPECTRUM_ON_PPLL_V3 args;

    memset(&args, 0, sizeof(args));
    args.usSpreadSpectrumAmountFrac = cpu_to_le16(0);
    switch (DeviceExtension->PllId)
    {
        case ATOM_PPLL1:
            args.ucSpreadSpectrumType = ATOM_PPLL_SS_TYPE_V3_P1PLL;
            break;
        case ATOM_PPLL2:
            args.ucSpreadSpectrumType = ATOM_PPLL_SS_TYPE_V3_P2PLL;
            break;
        case ATOM_DCPLL: /* == ATOM_PPLL0 on DCE6 */
            args.ucSpreadSpectrumType = ATOM_PPLL_SS_TYPE_V3_DCPLL;
            break;
        default:
            return TRUE;
    }
    args.usSpreadSpectrumAmount = cpu_to_le16(0);
    args.usSpreadSpectrumStep = cpu_to_le16(0);
    args.ucEnable = ATOM_DISABLE;
    return RadeonExecuteTable(DeviceExtension, index, &args);
}

union adjust_pixel_clock {
	ADJUST_DISPLAY_PLL_PS_ALLOCATION v1;
	ADJUST_DISPLAY_PLL_PS_ALLOCATION_V3 v3;
};

/*
 * Port of atombios_adjust_pll: sets the base PLL flags and runs the
 * AdjustDisplayPll command table so the VBIOS can apply encoder-specific
 * pixel clock fixups.  Returns the adjusted clock in kHz.
 */
static ULONG
RadeonAdjustPll(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PRADEONFB_TIMING Mode)
{
    PRADEONFB_PLL pll = &DeviceExtension->P1Pll;
    ULONG adjusted_clock = Mode->ClockKHz;
    ULONG clock = Mode->ClockKHz;
    int encoder_mode = DeviceExtension->EncoderMode;
    BOOLEAN is_duallink = RadeonIsDualLink(DeviceExtension, Mode->ClockKHz);
    union adjust_pixel_clock args;
    uint8_t frev, crev;
    int index;

    /* reset the pll flags */
    pll->Flags = 0;
    pll->ReferenceDiv = 0;
    pll->PostDiv = 0;

    /* use frac fb div on APUs (ASIC_IS_DCE61) */
    if (RadeonIsDce61(DeviceExtension))
        pll->Flags |= RADEONFB_PLL_USE_FRAC_FB_DIV;
    /* ASIC_IS_DCE32 && clock > 165000 */
    if (Mode->ClockKHz > 165000)
        pll->Flags |= RADEONFB_PLL_USE_FRAC_FB_DIV;

    if (DeviceExtension->HasPanel)
        pll->Flags |= RADEONFB_PLL_IS_LCD;

    /* DCE3+ AdjustDisplayPll */
    index = GetIndexIntoMasterTable(COMMAND, AdjustDisplayPll);
    if (!atom_parse_cmd_header(DeviceExtension->AtomContext, index,
                               &frev, &crev))
        return adjusted_clock;

    memset(&args, 0, sizeof(args));

    switch (frev)
    {
    case 1:
        switch (crev)
        {
        case 1:
        case 2:
            args.v1.usPixelClock = cpu_to_le16((USHORT)(clock / 10));
            args.v1.ucTransmitterID = (UCHAR)DeviceExtension->EncoderObjId;
            args.v1.ucEncodeMode = (UCHAR)encoder_mode;

            if (!RadeonExecuteTable(DeviceExtension, index, &args))
                return 0;
            adjusted_clock = le16_to_cpu(args.v1.usPixelClock) * 10;
            break;
        case 3:
            args.v3.sInput.usPixelClock = cpu_to_le16((USHORT)(clock / 10));
            args.v3.sInput.ucTransmitterID =
                (UCHAR)DeviceExtension->EncoderObjId;
            args.v3.sInput.ucEncodeMode = (UCHAR)encoder_mode;
            args.v3.sInput.ucDispPllConfig = 0;
            if (DeviceExtension->EncoderMode == ATOM_ENCODER_MODE_DVI)
            {
                /* DFP: coherent mode (Linux default for TMDS) */
                args.v3.sInput.ucDispPllConfig |=
                    DISPPLL_CONFIG_COHERENT_MODE;
                if (is_duallink)
                    args.v3.sInput.ucDispPllConfig |=
                        DISPPLL_CONFIG_DUAL_LINK;
            }
            args.v3.sInput.ucExtTransmitterID = 0;

            if (!RadeonExecuteTable(DeviceExtension, index, &args))
                return 0;
            adjusted_clock =
                le32_to_cpu(args.v3.sOutput.ulDispPllFreq) * 10;
            if (args.v3.sOutput.ucRefDiv)
            {
                pll->Flags |= RADEONFB_PLL_USE_FRAC_FB_DIV;
                pll->Flags |= RADEONFB_PLL_USE_REF_DIV;
                pll->ReferenceDiv = args.v3.sOutput.ucRefDiv;
            }
            if (args.v3.sOutput.ucPostDiv)
            {
                pll->Flags |= RADEONFB_PLL_USE_FRAC_FB_DIV;
                pll->Flags |= RADEONFB_PLL_USE_POST_DIV;
                pll->PostDiv = args.v3.sOutput.ucPostDiv;
            }
            break;
        default:
            VideoPortDebugPrint(Error,
                "radeonfb: unknown AdjustDisplayPll version %d %d\n",
                frev, crev);
            return adjusted_clock;
        }
        break;
    default:
        VideoPortDebugPrint(Error,
            "radeonfb: unknown AdjustDisplayPll version %d %d\n",
            frev, crev);
        return adjusted_clock;
    }
    return adjusted_clock;
}

union set_pixel_clock {
	SET_PIXEL_CLOCK_PS_ALLOCATION base;
	PIXEL_CLOCK_PARAMETERS v1;
	PIXEL_CLOCK_PARAMETERS_V2 v2;
	PIXEL_CLOCK_PARAMETERS_V3 v3;
	PIXEL_CLOCK_PARAMETERS_V5 v5;
	PIXEL_CLOCK_PARAMETERS_V6 v6;
};

/* Port of atombios_crtc_set_disp_eng_pll (DCE6 paths) */
static BOOLEAN
RadeonSetDispEngPll(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    ULONG dispclk = DeviceExtension->DefaultDispClk;
    uint8_t frev, crev;
    int index;
    union set_pixel_clock args;

    memset(&args, 0, sizeof(args));

    index = GetIndexIntoMasterTable(COMMAND, SetPixelClock);
    if (!atom_parse_cmd_header(DeviceExtension->AtomContext, index,
                               &frev, &crev))
        return FALSE;

    switch (frev)
    {
    case 1:
        switch (crev)
        {
        case 5:
            /* if the default dcpll clock is specified,
             * SetPixelClock provides the dividers
             */
            args.v5.ucCRTC = ATOM_CRTC_INVALID;
            args.v5.usPixelClock = cpu_to_le16((USHORT)dispclk);
            args.v5.ucPpll = ATOM_DCPLL;
            break;
        case 6:
            /* if the default dcpll clock is specified,
             * SetPixelClock provides the dividers
             */
            args.v6.ulDispEngClkFreq = cpu_to_le32(dispclk);
            if (RadeonIsDce61(DeviceExtension))
                args.v6.ucPpll = ATOM_EXT_PLL1;
            else
                args.v6.ucPpll = ATOM_PPLL0;    /* ASIC_IS_DCE6 */
            break;
        default:
            VideoPortDebugPrint(Error,
                "radeonfb: unknown SetPixelClock version %d %d\n",
                frev, crev);
            return FALSE;
        }
        break;
    default:
        VideoPortDebugPrint(Error,
            "radeonfb: unknown SetPixelClock version %d %d\n", frev, crev);
        return FALSE;
    }

    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* Port of atombios_crtc_program_pll (crev 5/6, the DCE4+/DCE6 encodings) */
static BOOLEAN
RadeonProgramPll(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG ClockKHz,
    _In_ ULONG RefDiv,
    _In_ ULONG FbDiv,
    _In_ ULONG FracFbDiv,
    _In_ ULONG PostDiv)
{
    uint8_t frev, crev;
    int index = GetIndexIntoMasterTable(COMMAND, SetPixelClock);
    union set_pixel_clock args;

    memset(&args, 0, sizeof(args));

    if (!atom_parse_cmd_header(DeviceExtension->AtomContext, index,
                               &frev, &crev))
        return FALSE;

    switch (frev)
    {
    case 1:
        switch (crev)
        {
        case 5:
            args.v5.ucCRTC = RADEON_CRTC_ID;
            args.v5.usPixelClock = cpu_to_le16((USHORT)(ClockKHz / 10));
            args.v5.ucRefDiv = (UCHAR)RefDiv;
            args.v5.usFbDiv = cpu_to_le16((USHORT)FbDiv);
            args.v5.ulFbDivDecFrac = cpu_to_le32(FracFbDiv * 100000);
            args.v5.ucPostDiv = (UCHAR)PostDiv;
            args.v5.ucMiscInfo = 0; /* HDMI depth, etc. */
            args.v5.ucTransmitterID = (UCHAR)DeviceExtension->EncoderObjId;
            args.v5.ucEncoderMode = (UCHAR)DeviceExtension->EncoderMode;
            args.v5.ucPpll = (UCHAR)DeviceExtension->PllId;
            break;
        case 6:
            args.v6.ulDispEngClkFreq =
                cpu_to_le32(((ULONG)RADEON_CRTC_ID << 24) | (ClockKHz / 10));
            args.v6.ucRefDiv = (UCHAR)RefDiv;
            args.v6.usFbDiv = cpu_to_le16((USHORT)FbDiv);
            args.v6.ulFbDivDecFrac = cpu_to_le32(FracFbDiv * 100000);
            args.v6.ucPostDiv = (UCHAR)PostDiv;
            args.v6.ucMiscInfo = 0; /* HDMI depth, etc. */
            args.v6.ucTransmitterID = (UCHAR)DeviceExtension->EncoderObjId;
            args.v6.ucEncoderMode = (UCHAR)DeviceExtension->EncoderMode;
            args.v6.ucPpll = (UCHAR)DeviceExtension->PllId;
            break;
        default:
            VideoPortDebugPrint(Error,
                "radeonfb: unknown SetPixelClock version %d %d\n",
                frev, crev);
            return FALSE;
        }
        break;
    default:
        VideoPortDebugPrint(Error,
            "radeonfb: unknown SetPixelClock version %d %d\n", frev, crev);
        return FALSE;
    }

    return RadeonExecuteTable(DeviceExtension, index, &args);
}

union crtc_source_param {
	SELECT_CRTC_SOURCE_PS_ALLOCATION v1;
	SELECT_CRTC_SOURCE_PARAMETERS_V2 v2;
};

/* Port of atombios_set_encoder_crtc_source (v2, DIG encoders) */
static BOOLEAN
RadeonSelectCrtcSource(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    union crtc_source_param args;
    int index = GetIndexIntoMasterTable(COMMAND, SelectCRTC_Source);
    uint8_t frev, crev;

    memset(&args, 0, sizeof(args));

    if (!atom_parse_cmd_header(DeviceExtension->AtomContext, index,
                               &frev, &crev))
        return FALSE;

    if (frev != 1 || crev < 2)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: unknown SelectCRTC_Source version %d %d\n",
            frev, crev);
        return FALSE;
    }

    args.v2.ucCRTC = RADEON_CRTC_ID;
    args.v2.ucEncodeMode = (UCHAR)DeviceExtension->EncoderMode;
    switch (DeviceExtension->DigEncoder)
    {
        case 0:
            args.v2.ucEncoderID = ASIC_INT_DIG1_ENCODER_ID;
            break;
        case 1:
            args.v2.ucEncoderID = ASIC_INT_DIG2_ENCODER_ID;
            break;
        case 2:
            args.v2.ucEncoderID = ASIC_INT_DIG3_ENCODER_ID;
            break;
        case 3:
            args.v2.ucEncoderID = ASIC_INT_DIG4_ENCODER_ID;
            break;
        case 4:
            args.v2.ucEncoderID = ASIC_INT_DIG5_ENCODER_ID;
            break;
        case 5:
            args.v2.ucEncoderID = ASIC_INT_DIG6_ENCODER_ID;
            break;
        case 6:
            args.v2.ucEncoderID = ASIC_INT_DIG7_ENCODER_ID;
            break;
    }

    return RadeonExecuteTable(DeviceExtension, index, &args);
}

union dig_encoder_control {
	DIG_ENCODER_CONTROL_PS_ALLOCATION v1;
	DIG_ENCODER_CONTROL_PARAMETERS_V2 v2;
	DIG_ENCODER_CONTROL_PARAMETERS_V3 v3;
	DIG_ENCODER_CONTROL_PARAMETERS_V4 v4;
};

/* Port of atombios_dig_encoder_setup2 (DCE4+ paths, non-DP) */
static BOOLEAN
RadeonDigEncoderSetup(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG PixelClockKHz,
    _In_ int Action,
    _In_ int PanelMode)
{
    union dig_encoder_control args;
    int index = GetIndexIntoMasterTable(COMMAND, DIGxEncoderControl);
    uint8_t frev, crev;
    BOOLEAN is_duallink = RadeonIsDualLink(DeviceExtension, PixelClockKHz);

    memset(&args, 0, sizeof(args));

    if (!atom_parse_cmd_header(DeviceExtension->AtomContext, index,
                               &frev, &crev))
        return FALSE;

    if (frev != 1)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: unknown DIGxEncoderControl version %d %d\n",
            frev, crev);
        return FALSE;
    }

    switch (crev)
    {
    case 2:
    case 3:
        args.v3.ucAction = (UCHAR)Action;
        args.v3.usPixelClock = cpu_to_le16((USHORT)(PixelClockKHz / 10));
        if (Action == ATOM_ENCODER_CMD_SETUP_PANEL_MODE)
            args.v3.ucPanelMode = (UCHAR)PanelMode;
        else
            args.v3.ucEncoderMode = (UCHAR)DeviceExtension->EncoderMode;

        if (is_duallink)
            args.v3.ucLaneNum = 8;
        else
            args.v3.ucLaneNum = 4;

        args.v3.acConfig.ucDigSel = (UCHAR)DeviceExtension->DigEncoder;
        args.v3.ucBitPerColor = PANEL_8BIT_PER_COLOR;
        break;
    case 4:
        args.v4.ucAction = (UCHAR)Action;
        args.v4.usPixelClock = cpu_to_le16((USHORT)(PixelClockKHz / 10));
        if (Action == ATOM_ENCODER_CMD_SETUP_PANEL_MODE)
            args.v4.ucPanelMode = (UCHAR)PanelMode;
        else
            args.v4.ucEncoderMode = (UCHAR)DeviceExtension->EncoderMode;

        if (is_duallink)
            args.v4.ucLaneNum = 8;
        else
            args.v4.ucLaneNum = 4;

        args.v4.acConfig.ucDigSel = (UCHAR)DeviceExtension->DigEncoder;
        args.v4.ucBitPerColor = PANEL_8BIT_PER_COLOR;
        args.v4.ucHPD_ID = 0;   /* RADEON_HPD_NONE */
        break;
    default:
        VideoPortDebugPrint(Error,
            "radeonfb: unknown DIGxEncoderControl version %d %d\n",
            frev, crev);
        return FALSE;
    }

    return RadeonExecuteTable(DeviceExtension, index, &args);
}

union dig_transmitter_control {
	DIG_TRANSMITTER_CONTROL_PS_ALLOCATION v1;
	DIG_TRANSMITTER_CONTROL_PARAMETERS_V2 v2;
	DIG_TRANSMITTER_CONTROL_PARAMETERS_V3 v3;
	DIG_TRANSMITTER_CONTROL_PARAMETERS_V4 v4;
	DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5 v5;
};

/* Port of atombios_dig_transmitter_setup2 (UNIPHY, crev 3/4/5, non-DP) */
static BOOLEAN
RadeonTransmitterControl(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG PixelClockKHz,
    _In_ int Action)
{
    union dig_transmitter_control args;
    int index = GetIndexIntoMasterTable(COMMAND, UNIPHYTransmitterControl);
    uint8_t frev, crev;
    BOOLEAN is_duallink = RadeonIsDualLink(DeviceExtension, PixelClockKHz);
    int dig_encoder = DeviceExtension->DigEncoder;
    int pll_id = DeviceExtension->PllId;

    memset(&args, 0, sizeof(args));

    if (!atom_parse_cmd_header(DeviceExtension->AtomContext, index,
                               &frev, &crev))
        return FALSE;

    if (frev != 1)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: unknown UNIPHYTransmitterControl version %d %d\n",
            frev, crev);
        return FALSE;
    }

    switch (crev)
    {
    case 3:
        args.v3.ucAction = (UCHAR)Action;
        if (Action == ATOM_TRANSMITTER_ACTION_INIT)
        {
            args.v3.usInitInfo =
                cpu_to_le16(DeviceExtension->ConnectorObjId);
        }
        else if (is_duallink)
        {
            args.v3.usPixelClock =
                cpu_to_le16((USHORT)((PixelClockKHz / 2) / 10));
        }
        else
        {
            args.v3.usPixelClock =
                cpu_to_le16((USHORT)(PixelClockKHz / 10));
        }

        args.v3.ucLaneNum = is_duallink ? 8 : 4;

        if (DeviceExtension->LinkB)
            args.v3.acConfig.ucLinkSel = 1;
        if (dig_encoder & 1)
            args.v3.acConfig.ucEncoderSel = 1;

        args.v3.acConfig.ucRefClkSource = (UCHAR)pll_id;

        switch (DeviceExtension->EncoderObjId)
        {
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:
                args.v3.acConfig.ucTransmitterSel = 0;
                break;
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:
                args.v3.acConfig.ucTransmitterSel = 1;
                break;
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:
                args.v3.acConfig.ucTransmitterSel = 2;
                break;
        }

        if (DeviceExtension->EncoderMode == ATOM_ENCODER_MODE_DVI)
        {
            args.v3.acConfig.fCoherentMode = 1;
            if (is_duallink)
                args.v3.acConfig.fDualLinkConnector = 1;
        }
        break;
    case 4:
        args.v4.ucAction = (UCHAR)Action;
        if (Action == ATOM_TRANSMITTER_ACTION_INIT)
        {
            args.v4.usInitInfo =
                cpu_to_le16(DeviceExtension->ConnectorObjId);
        }
        else if (is_duallink)
        {
            args.v4.usPixelClock =
                cpu_to_le16((USHORT)((PixelClockKHz / 2) / 10));
        }
        else
        {
            args.v4.usPixelClock =
                cpu_to_le16((USHORT)(PixelClockKHz / 10));
        }

        args.v4.ucLaneNum = is_duallink ? 8 : 4;

        if (DeviceExtension->LinkB)
            args.v4.acConfig.ucLinkSel = 1;
        if (dig_encoder & 1)
            args.v4.acConfig.ucEncoderSel = 1;

        args.v4.acConfig.ucRefClkSource = (UCHAR)pll_id;

        switch (DeviceExtension->EncoderObjId)
        {
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:
                args.v4.acConfig.ucTransmitterSel = 0;
                break;
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:
                args.v4.acConfig.ucTransmitterSel = 1;
                break;
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:
                args.v4.acConfig.ucTransmitterSel = 2;
                break;
        }

        if (DeviceExtension->EncoderMode == ATOM_ENCODER_MODE_DVI)
        {
            args.v4.acConfig.fCoherentMode = 1;
            if (is_duallink)
                args.v4.acConfig.fDualLinkConnector = 1;
        }
        break;
    case 5:
        args.v5.ucAction = (UCHAR)Action;
        args.v5.usSymClock = cpu_to_le16((USHORT)(PixelClockKHz / 10));

        switch (DeviceExtension->EncoderObjId)
        {
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:
                if (DeviceExtension->LinkB)
                    args.v5.ucPhyId = ATOM_PHY_ID_UNIPHYB;
                else
                    args.v5.ucPhyId = ATOM_PHY_ID_UNIPHYA;
                break;
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:
                if (DeviceExtension->LinkB)
                    args.v5.ucPhyId = ATOM_PHY_ID_UNIPHYD;
                else
                    args.v5.ucPhyId = ATOM_PHY_ID_UNIPHYC;
                break;
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:
                if (DeviceExtension->LinkB)
                    args.v5.ucPhyId = ATOM_PHY_ID_UNIPHYF;
                else
                    args.v5.ucPhyId = ATOM_PHY_ID_UNIPHYE;
                break;
            case ENCODER_OBJECT_ID_INTERNAL_UNIPHY3:
                args.v5.ucPhyId = ATOM_PHY_ID_UNIPHYG;
                break;
        }
        args.v5.ucLaneNum = is_duallink ? 8 : 4;
        args.v5.ucConnObjId = (UCHAR)DeviceExtension->ConnectorObjId;
        args.v5.ucDigMode = (UCHAR)DeviceExtension->EncoderMode;

        args.v5.asConfig.ucPhyClkSrcId = (UCHAR)pll_id;

        if (DeviceExtension->EncoderMode == ATOM_ENCODER_MODE_DVI)
            args.v5.asConfig.ucCoherentMode = 1;
        args.v5.asConfig.ucHPDSel = 0;  /* RADEON_HPD_NONE */
        args.v5.ucDigEncoderSel = (UCHAR)(1 << dig_encoder);
        args.v5.ucDPLaneSet = 0;
        break;
    default:
        VideoPortDebugPrint(Error,
            "radeonfb: unknown UNIPHYTransmitterControl version %d %d\n",
            frev, crev);
        return FALSE;
    }

    return RadeonExecuteTable(DeviceExtension, index, &args);
}

/* SCANOUT SURFACE (port of dce4_crtc_do_set_base, linear XRGB8888) *********/

static VOID
RadeonSetScanoutBase(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PRADEONFB_TIMING Mode)
{
    ULONGLONG FbLocation;
    ULONG FbFormat;
    ULONG FbPitchPixels;
    ULONG Stride;
    ULONG ViewportW, ViewportH;

    /* The GPU-internal VRAM base: MC_VM_FB_LOCATION[15:0] << 24 (the
     * scanout engine uses internal addresses, not the PCI BAR address). */
    FbLocation =
        ((ULONGLONG)(RadeonRegRead(DeviceExtension, MC_VM_FB_LOCATION)
                     & 0xFFFF)) << 24;

    FbFormat = EVERGREEN_GRPH_DEPTH(EVERGREEN_GRPH_DEPTH_32BPP) |
               EVERGREEN_GRPH_FORMAT(EVERGREEN_GRPH_FORMAT_ARGB8888);
    /* linear scanout */
    FbFormat |= EVERGREEN_GRPH_ARRAY_MODE(EVERGREEN_GRPH_ARRAY_LINEAR_ALIGNED);
    /* pipe config for the SI-class parts (OLAND); ARUBA needs none */
    if (DeviceExtension->Family == CHIP_OLAND)
        FbFormat |= SI_GRPH_PIPE_CONFIG(SI_ADDR_SURF_P4_8x16);

    /* Disable the legacy VGA path of this CRTC */
    RadeonRegWrite(DeviceExtension, D1VGA_CONTROL, 0);

    /* Make sure surface address is updated at vertical blank rather than
     * horizontal blank */
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_FLIP_CONTROL + RADEON_CRTC_OFFSET, 0);

    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + RADEON_CRTC_OFFSET,
                   (ULONG)(FbLocation >> 32));
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_SECONDARY_SURFACE_ADDRESS_HIGH + RADEON_CRTC_OFFSET,
                   (ULONG)(FbLocation >> 32));
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_PRIMARY_SURFACE_ADDRESS + RADEON_CRTC_OFFSET,
                   (ULONG)FbLocation & EVERGREEN_GRPH_SURFACE_ADDRESS_MASK);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_SECONDARY_SURFACE_ADDRESS + RADEON_CRTC_OFFSET,
                   (ULONG)FbLocation & EVERGREEN_GRPH_SURFACE_ADDRESS_MASK);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_CONTROL + RADEON_CRTC_OFFSET, FbFormat);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_SWAP_CONTROL + RADEON_CRTC_OFFSET,
                   EVERGREEN_GRPH_ENDIAN_SWAP(EVERGREEN_GRPH_ENDIAN_NONE));

    /* 8bpc scanout: keep the LUT in the pipeline */
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_LUT_10BIT_BYPASS_CONTROL + RADEON_CRTC_OFFSET,
                   RadeonRegRead(DeviceExtension,
                                 EVERGREEN_GRPH_LUT_10BIT_BYPASS_CONTROL +
                                 RADEON_CRTC_OFFSET) &
                   ~EVERGREEN_LUT_10BIT_BYPASS_EN);

    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_SURFACE_OFFSET_X + RADEON_CRTC_OFFSET, 0);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_SURFACE_OFFSET_Y + RADEON_CRTC_OFFSET, 0);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_X_START + RADEON_CRTC_OFFSET, 0);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_Y_START + RADEON_CRTC_OFFSET, 0);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_X_END + RADEON_CRTC_OFFSET, Mode->HDisplay);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_Y_END + RADEON_CRTC_OFFSET, Mode->VDisplay);

    Stride = (Mode->HDisplay * 4 + 255) & ~255UL;
    FbPitchPixels = Stride / 4;
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_PITCH + RADEON_CRTC_OFFSET, FbPitchPixels);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_GRPH_ENABLE + RADEON_CRTC_OFFSET, 1);

    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_DESKTOP_HEIGHT + RADEON_CRTC_OFFSET,
                   Mode->VDisplay);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_VIEWPORT_START + RADEON_CRTC_OFFSET, 0);
    ViewportW = Mode->HDisplay;
    ViewportH = (Mode->VDisplay + 1) & ~1UL;
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_VIEWPORT_SIZE + RADEON_CRTC_OFFSET,
                   (ViewportW << 16) | ViewportH);

    /* set pageflip to happen anywhere in vblank interval */
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_MASTER_UPDATE_MODE + RADEON_CRTC_OFFSET, 0);
}

/* MODE-SET ORCHESTRATION ****************************************************/

/*
 * Full mode-set on CRTC0 + the parsed DIG/UNIPHY output, following the DRM
 * helper call order the Linux radeon KMS driver uses:
 * encoder prepare (SelectCRTC_Source), crtc prepare (powergate/lock/off),
 * crtc mode_set (PLL, DTD timing, base, overscan, scaler), encoder
 * mode_set (encoder off + DATA_FORMAT), crtc commit (on), encoder commit
 * (encoder setup + transmitter enable + backlight).
 */
BOOLEAN
RadeonSetMode(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PRADEONFB_TIMING Timing)
{
    ULONG AdjustedClock;
    ULONG DotClock, FbDiv, FracFbDiv, RefDiv, PostDiv;
    BOOLEAN Ok = TRUE;

    if (DeviceExtension->AtomContext == NULL)
        return FALSE;

    if (DeviceExtension->MaxPixelClock != 0 &&
        (Timing->ClockKHz / 10) > DeviceExtension->MaxPixelClock)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: mode clock %lukHz above VBIOS limit\n",
            Timing->ClockKHz);
        return FALSE;
    }

    /* One-time init: transmitter INIT (radeon_atom_encoder_init) and the
     * display engine clock (radeon_atom_disp_eng_pll_init). */
    if (!DeviceExtension->DispEngPllInitDone)
    {
        Ok &= RadeonTransmitterControl(DeviceExtension, Timing->ClockKHz,
                                       ATOM_TRANSMITTER_ACTION_INIT);
        Ok &= RadeonSetDispEngPll(DeviceExtension);
        DeviceExtension->DispEngPllInitDone = TRUE;
    }

    /* Pixel clock adjustment (atombios_crtc_prepare_pll / adjust_pll) */
    AdjustedClock = RadeonAdjustPll(DeviceExtension, Timing);
    if (AdjustedClock == 0)
        return FALSE;

    /* encoder prepare */
    Ok &= RadeonSelectCrtcSource(DeviceExtension);

    /* crtc prepare: disable crtc pair power gating before programming,
     * lock the double-buffered registers, then crtc dpms off */
    Ok &= RadeonPowerGateCrtc(DeviceExtension, ATOM_DISABLE);
    Ok &= RadeonLockCrtc(DeviceExtension, ATOM_ENABLE);
    Ok &= RadeonBlankCrtc(DeviceExtension, ATOM_ENABLE);
    Ok &= RadeonEnableCrtc(DeviceExtension, ATOM_DISABLE);

    /* crtc mode_set: PLL */
    RadeonComputePllAvivo(&DeviceExtension->P1Pll, AdjustedClock,
                          &DotClock, &FbDiv, &FracFbDiv, &RefDiv, &PostDiv);
    Ok &= RadeonDisableSpreadSpectrum(DeviceExtension);
    Ok &= RadeonProgramPll(DeviceExtension, Timing->ClockKHz,
                           RefDiv, FbDiv, FracFbDiv, PostDiv);

    /* crtc mode_set: timing + scanout surface */
    Ok &= RadeonSetCrtcDtdTiming(DeviceExtension, Timing);
    RadeonSetScanoutBase(DeviceExtension, Timing);
    Ok &= RadeonOverscanSetup(DeviceExtension);
    Ok &= RadeonScalerSetup(DeviceExtension);

    /* encoder mode_set: encoder dpms off first (DCE4+: transmitter only),
     * then the progressive DATA_FORMAT quirk */
    if (DeviceExtension->HasPanel)
        Ok &= RadeonTransmitterControl(DeviceExtension, Timing->ClockKHz,
                                       ATOM_TRANSMITTER_ACTION_LCD_BLOFF);
    Ok &= RadeonTransmitterControl(DeviceExtension, Timing->ClockKHz,
                                   ATOM_TRANSMITTER_ACTION_DISABLE);
    RadeonRegWrite(DeviceExtension,
                   EVERGREEN_DATA_FORMAT + RADEON_CRTC_OFFSET, 0);

    /* crtc commit: crtc dpms on */
    Ok &= RadeonEnableCrtc(DeviceExtension, ATOM_ENABLE);
    Ok &= RadeonBlankCrtc(DeviceExtension, ATOM_DISABLE);

    /* encoder commit: encoder dpms on (the ASIC_IS_DCE41/DCE5 path both
     * DCE6 families take): setup + panel mode, transmitter enable,
     * backlight on */
    Ok &= RadeonDigEncoderSetup(DeviceExtension, Timing->ClockKHz,
                                ATOM_ENCODER_CMD_SETUP, 0);
    Ok &= RadeonDigEncoderSetup(DeviceExtension, Timing->ClockKHz,
                                ATOM_ENCODER_CMD_SETUP_PANEL_MODE,
                                DP_PANEL_MODE_EXTERNAL_DP_MODE);
    Ok &= RadeonTransmitterControl(DeviceExtension, Timing->ClockKHz,
                                   ATOM_TRANSMITTER_ACTION_ENABLE);
    if (DeviceExtension->HasPanel)
        Ok &= RadeonTransmitterControl(DeviceExtension, Timing->ClockKHz,
                                       ATOM_TRANSMITTER_ACTION_LCD_BLON);

    Ok &= RadeonLockCrtc(DeviceExtension, ATOM_DISABLE);

    VideoPortDebugPrint(Info,
        "radeonfb: mode-set %lux%lu@%lukHz %s (pll %lu.%lu/%lu/%lu -> %lu0kHz)\n",
        Timing->HDisplay, Timing->VDisplay, Timing->ClockKHz,
        Ok ? "done" : "FAILED",
        FbDiv, FracFbDiv, RefDiv, PostDiv, DotClock);

    return Ok;
}
