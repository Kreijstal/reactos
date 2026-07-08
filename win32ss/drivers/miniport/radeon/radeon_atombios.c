/*
 * PROJECT:     ReactOS AMD Radeon ATOM-BIOS Framebuffer Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ATOM data-table parsing: FirmwareInfo (clock/PLL limits),
 *              LVDS_Info (native panel timing) and a narrow walk of the
 *              Object_Header display-path table to find the connector ->
 *              encoder routing.  Ported from the corresponding paths of
 *              Linux v6.6 drivers/gpu/drm/radeon/radeon_atombios.c and
 *              radeon_clocks.c.
 * COPYRIGHT:   Copyright 2007-2013 Advanced Micro Devices, Inc.
 *              Copyright 2008 Red Hat Inc.
 *              Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "radeon.h"

union firmware_info {
	ATOM_FIRMWARE_INFO info;
	ATOM_FIRMWARE_INFO_V1_2 info_12;
	ATOM_FIRMWARE_INFO_V1_3 info_13;
	ATOM_FIRMWARE_INFO_V1_4 info_14;
	ATOM_FIRMWARE_INFO_V2_1 info_21;
	ATOM_FIRMWARE_INFO_V2_2 info_22;
};

union lvds_info {
	struct _ATOM_LVDS_INFO info;
	struct _ATOM_LVDS_INFO_V12 info_12;
};

/*
 * Port of radeon_atom_get_clock_info (pixel-PLL portion) plus the AVIVO
 * divider limits from radeon_get_clock_info (radeon_clocks.c).  All clock
 * values are kept in the 10kHz units the tables use.
 */
BOOLEAN
RadeonAtomGetClockInfo(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    struct atom_context *ctx = DeviceExtension->AtomContext;
    int index = GetIndexIntoMasterTable(DATA, FirmwareInfo);
    union firmware_info *firmware_info;
    uint8_t frev, crev;
    uint16_t data_offset;
    PRADEONFB_PLL p1pll = &DeviceExtension->P1Pll;

    if (!atom_parse_data_header(ctx, index, NULL, &frev, &crev, &data_offset))
        return FALSE;

    firmware_info =
        (union firmware_info *)((PUCHAR)ctx->bios + data_offset);

    p1pll->ReferenceFreq =
        le16_to_cpu(firmware_info->info.usReferenceClock);
    p1pll->ReferenceDiv = 0;

    if ((frev < 2) && (crev < 2))
        p1pll->PllOutMin =
            le16_to_cpu(firmware_info->info.usMinPixelClockPLL_Output);
    else
        p1pll->PllOutMin =
            le32_to_cpu(firmware_info->info_12.ulMinPixelClockPLL_Output);
    p1pll->PllOutMax =
        le32_to_cpu(firmware_info->info.ulMaxPixelClockPLL_Output);

    if (((frev < 2) && (crev >= 4)) || (frev >= 2))
    {
        p1pll->LcdPllOutMin =
            le16_to_cpu(firmware_info->info_14.usLcdMinPixelClockPLL_Output) * 100;
        if (p1pll->LcdPllOutMin == 0)
            p1pll->LcdPllOutMin = p1pll->PllOutMin;
        p1pll->LcdPllOutMax =
            le16_to_cpu(firmware_info->info_14.usLcdMaxPixelClockPLL_Output) * 100;
        if (p1pll->LcdPllOutMax == 0)
            p1pll->LcdPllOutMax = p1pll->PllOutMax;
    }
    else
    {
        p1pll->LcdPllOutMin = p1pll->PllOutMin;
        p1pll->LcdPllOutMax = p1pll->PllOutMax;
    }

    if (p1pll->PllOutMin == 0)
        p1pll->PllOutMin = 64800;   /* ASIC_IS_AVIVO default */

    p1pll->PllInMin =
        le16_to_cpu(firmware_info->info.usMinPixelClockPLL_Input);
    p1pll->PllInMax =
        le16_to_cpu(firmware_info->info.usMaxPixelClockPLL_Input);

    /* DCE4+ display engine clock */
    DeviceExtension->DefaultDispClk =
        le32_to_cpu(firmware_info->info_21.ulDefaultDispEngineClkFreq);
    if (DeviceExtension->DefaultDispClk == 0)
        DeviceExtension->DefaultDispClk = 60000;    /* 600 MHz (DCE6) */
    /* set a reasonable default for DP (DCE6) */
    if (DeviceExtension->DefaultDispClk < 53900)
    {
        VideoPortDebugPrint(Info,
            "radeonfb: changing default dispclk from %luMHz to 600MHz\n",
            DeviceExtension->DefaultDispClk / 100);
        DeviceExtension->DefaultDispClk = 60000;
    }
    DeviceExtension->DpExtClk =
        le16_to_cpu(firmware_info->info_21.usUniphyDPModeExtClkFreq);

    DeviceExtension->MaxPixelClock =
        le16_to_cpu(firmware_info->info.usMaxPixelClock);
    if (DeviceExtension->MaxPixelClock == 0)
        DeviceExtension->MaxPixelClock = 40000;

    /* AVIVO divider limits (radeon_clocks.c) */
    p1pll->MinPostDiv = 2;
    p1pll->MaxPostDiv = 0x7f;
    p1pll->MinRefDiv = 2;
    p1pll->MaxRefDiv = 0x3ff;
    p1pll->MinFeedbackDiv = 4;
    p1pll->MaxFeedbackDiv = 0x7ff;

    VideoPortDebugPrint(Trace,
        "radeonfb: refclk %lu0kHz, pll out %lu0-%lu0kHz (lcd %lu0-%lu0), "
        "dispclk %lu0kHz, max pixclk %lu0kHz\n",
        p1pll->ReferenceFreq, p1pll->PllOutMin, p1pll->PllOutMax,
        p1pll->LcdPllOutMin, p1pll->LcdPllOutMax,
        DeviceExtension->DefaultDispClk, DeviceExtension->MaxPixelClock);

    return TRUE;
}

/*
 * Port of radeon_atombios_get_lvds_info: fetch the panel's native timing
 * from the LVDS_Info/LCD_Info data table.  The LCD record patch table
 * (fake EDID etc.) is intentionally not parsed - the native DTD timing is
 * all this driver consumes.
 */
BOOLEAN
RadeonAtomGetLvdsInfo(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    struct atom_context *ctx = DeviceExtension->AtomContext;
    int index = GetIndexIntoMasterTable(DATA, LVDS_Info);
    union lvds_info *lvds_info;
    uint8_t frev, crev;
    uint16_t data_offset;
    uint16_t misc;
    PRADEONFB_TIMING Mode = &DeviceExtension->NativeMode;

    if (!atom_parse_data_header(ctx, index, NULL, &frev, &crev, &data_offset))
        return FALSE;

    lvds_info = (union lvds_info *)((PUCHAR)ctx->bios + data_offset);

    Mode->ClockKHz =
        le16_to_cpu(lvds_info->info.sLCDTiming.usPixClk) * 10;
    Mode->HDisplay =
        le16_to_cpu(lvds_info->info.sLCDTiming.usHActive);
    Mode->VDisplay =
        le16_to_cpu(lvds_info->info.sLCDTiming.usVActive);
    Mode->HTotal = Mode->HDisplay +
        le16_to_cpu(lvds_info->info.sLCDTiming.usHBlanking_Time);
    Mode->HSyncStart = Mode->HDisplay +
        le16_to_cpu(lvds_info->info.sLCDTiming.usHSyncOffset);
    Mode->HSyncEnd = Mode->HSyncStart +
        le16_to_cpu(lvds_info->info.sLCDTiming.usHSyncWidth);
    Mode->VTotal = Mode->VDisplay +
        le16_to_cpu(lvds_info->info.sLCDTiming.usVBlanking_Time);
    Mode->VSyncStart = Mode->VDisplay +
        le16_to_cpu(lvds_info->info.sLCDTiming.usVSyncOffset);
    Mode->VSyncEnd = Mode->VSyncStart +
        le16_to_cpu(lvds_info->info.sLCDTiming.usVSyncWidth);

    DeviceExtension->LcdMisc = lvds_info->info.ucLVDS_Misc;

    Mode->Flags = 0;
    misc = le16_to_cpu(lvds_info->info.sLCDTiming.susModeMiscInfo.usAccess);
    if (misc & ATOM_VSYNC_POLARITY)
        Mode->Flags |= RADEONFB_MODE_FLAG_NVSYNC;
    if (misc & ATOM_HSYNC_POLARITY)
        Mode->Flags |= RADEONFB_MODE_FLAG_NHSYNC;
    if (misc & ATOM_COMPOSITESYNC)
        Mode->Flags |= RADEONFB_MODE_FLAG_CSYNC;
    if (misc & ATOM_INTERLACE)
        Mode->Flags |= RADEONFB_MODE_FLAG_INTERLACE;
    if (misc & ATOM_DOUBLE_CLOCK_MODE)
        Mode->Flags |= RADEONFB_MODE_FLAG_DBLSCAN;

    if (Mode->ClockKHz == 0 ||
        Mode->HDisplay == 0 || Mode->VDisplay == 0 ||
        Mode->HTotal <= Mode->HDisplay || Mode->VTotal <= Mode->VDisplay ||
        Mode->HSyncEnd > Mode->HTotal || Mode->VSyncEnd > Mode->VTotal)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: LVDS_Info holds an implausible native timing\n");
        return FALSE;
    }

    VideoPortDebugPrint(Info,
        "radeonfb: panel native mode %lux%lu@%lukHz (misc 0x%02x)\n",
        Mode->HDisplay, Mode->VDisplay, Mode->ClockKHz,
        DeviceExtension->LcdMisc);

    return TRUE;
}

/*
 * Narrow port of radeon_get_atom_connector_info_from_object_table: walk
 * the display paths and pick one supported output - an LCD path when the
 * machine has an internal panel, otherwise the first digital (DFP) path
 * driven by a UNIPHY encoder.  Analog (DAC) outputs are left to the
 * VBE/VGA fallback and DP outputs are not supported (no link training).
 */
BOOLEAN
RadeonAtomParseObjectTable(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    struct atom_context *ctx = DeviceExtension->AtomContext;
    int index = GetIndexIntoMasterTable(DATA, Object_Header);
    uint8_t frev, crev;
    uint16_t size, data_offset;
    ATOM_OBJECT_HEADER *obj_header;
    ATOM_DISPLAY_OBJECT_PATH_TABLE *path_obj;
    int i, path_size, device_support;
    BOOLEAN FoundLcd = FALSE;
    BOOLEAN FoundDfp = FALSE;
    BOOLEAN LcdIsEdp = FALSE;

    if (!atom_parse_data_header(ctx, index, &size, &frev, &crev, &data_offset))
        return FALSE;

    if (crev < 2)
        return FALSE;

    obj_header = (ATOM_OBJECT_HEADER *)((PUCHAR)ctx->bios + data_offset);
    path_obj = (ATOM_DISPLAY_OBJECT_PATH_TABLE *)
        ((PUCHAR)ctx->bios + data_offset +
         le16_to_cpu(obj_header->usDisplayPathTableOffset));
    device_support = le16_to_cpu(obj_header->usDeviceSupport);

    path_size = 0;
    for (i = 0; i < path_obj->ucNumOfDispPath; i++)
    {
        uint8_t *addr = (uint8_t *)path_obj->asDispPath;
        ATOM_DISPLAY_OBJECT_PATH *path;
        uint16_t device_tag;
        uint8_t con_obj_id;
        int j;

        addr += path_size;
        path = (ATOM_DISPLAY_OBJECT_PATH *)addr;
        path_size += le16_to_cpu(path->usSize);

        device_tag = le16_to_cpu(path->usDeviceTag);
        if ((device_support & device_tag) == 0)
            continue;

        /* No TV/CV/CRT handling here */
        if (device_tag & (ATOM_DEVICE_TV_SUPPORT | ATOM_DEVICE_CV_SUPPORT))
            continue;
        if ((device_tag &
             (ATOM_DEVICE_LCD_SUPPORT | ATOM_DEVICE_DFP_SUPPORT)) == 0)
            continue;

        con_obj_id =
            (uint8_t)((le16_to_cpu(path->usConnObjectId) & OBJECT_ID_MASK)
                      >> OBJECT_ID_SHIFT);

        /* eDP panels need DP link training, which this driver does not
         * implement.  Remember it so the whole adapter is declined and the
         * VBE fallback keeps the panel lit. */
        if ((device_tag & ATOM_DEVICE_LCD_SUPPORT) &&
            (con_obj_id == CONNECTOR_OBJECT_ID_eDP))
        {
            LcdIsEdp = TRUE;
            continue;
        }
        if (con_obj_id == CONNECTOR_OBJECT_ID_DISPLAYPORT ||
            con_obj_id == CONNECTOR_OBJECT_ID_eDP)
        {
            continue;
        }

        /* Prefer the LCD path; take the first DFP otherwise */
        if (FoundLcd)
            continue;
        if (!(device_tag & ATOM_DEVICE_LCD_SUPPORT) && FoundDfp)
            continue;

        for (j = 0; j < ((int)le16_to_cpu(path->usSize) - 8) / 2; j++)
        {
            uint16_t graphic_obj = le16_to_cpu(path->usGraphicObjIds[j]);
            uint8_t grph_obj_type =
                (uint8_t)((graphic_obj & OBJECT_TYPE_MASK)
                          >> OBJECT_TYPE_SHIFT);
            uint8_t grph_obj_id =
                (uint8_t)((graphic_obj & OBJECT_ID_MASK) >> OBJECT_ID_SHIFT);
            uint8_t grph_obj_num =
                (uint8_t)((graphic_obj & ENUM_ID_MASK) >> ENUM_ID_SHIFT);

            if (grph_obj_type != GRAPH_OBJECT_TYPE_ENCODER)
                continue;

            if (grph_obj_id != ENCODER_OBJECT_ID_INTERNAL_UNIPHY &&
                grph_obj_id != ENCODER_OBJECT_ID_INTERNAL_UNIPHY1 &&
                grph_obj_id != ENCODER_OBJECT_ID_INTERNAL_UNIPHY2 &&
                grph_obj_id != ENCODER_OBJECT_ID_INTERNAL_UNIPHY3)
            {
                continue;
            }

            DeviceExtension->EncoderObjId = grph_obj_id;
            DeviceExtension->LinkB = (grph_obj_num == GRAPH_OBJECT_ENUM_ID2);
            DeviceExtension->ConnectorObjId = con_obj_id;
            DeviceExtension->DeviceTag = device_tag;
            if (device_tag & ATOM_DEVICE_LCD_SUPPORT)
                FoundLcd = TRUE;
            else
                FoundDfp = TRUE;
            break;
        }
    }

    if (LcdIsEdp)
    {
        VideoPortDebugPrint(Warn,
            "radeonfb: internal panel is eDP (link training unsupported), "
            "declining the adapter\n");
        return FALSE;
    }

    if (!FoundLcd && !FoundDfp)
        return FALSE;

    DeviceExtension->HasPanel = FoundLcd;
    DeviceExtension->HasOutput = TRUE;
    DeviceExtension->EncoderMode =
        FoundLcd ? ATOM_ENCODER_MODE_LVDS : ATOM_ENCODER_MODE_DVI;

    /* DIG frontend selection, port of radeon_atom_pick_dig_encoder's DCE6
     * mapping. */
    switch (DeviceExtension->EncoderObjId)
    {
        case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:
            DeviceExtension->DigEncoder = DeviceExtension->LinkB ? 1 : 0;
            break;
        case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:
            DeviceExtension->DigEncoder = DeviceExtension->LinkB ? 3 : 2;
            break;
        case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:
            DeviceExtension->DigEncoder = DeviceExtension->LinkB ? 5 : 4;
            break;
        case ENCODER_OBJECT_ID_INTERNAL_UNIPHY3:
        default:
            DeviceExtension->DigEncoder = 6;
            break;
    }

    /* PPLL selection, port of radeon_atom_pick_pll:
     * DCE6.1 (ARUBA IGP): PPLL2 is exclusive to UNIPHY A; UNIPHY B/C/D/E/F
     * use PPLL0/PPLL1 (single-head: PPLL0).
     * DCE6.0/6.4 (OLAND): non-DP outputs use PPLL1/PPLL2 (single-head:
     * PPLL1). */
    if (DeviceExtension->Family == CHIP_ARUBA)
    {
        if (DeviceExtension->EncoderObjId ==
                ENCODER_OBJECT_ID_INTERNAL_UNIPHY &&
            !DeviceExtension->LinkB)
        {
            DeviceExtension->PllId = ATOM_PPLL2;
        }
        else
        {
            DeviceExtension->PllId = ATOM_PPLL0;
        }
    }
    else
    {
        DeviceExtension->PllId = ATOM_PPLL1;
    }

    VideoPortDebugPrint(Info,
        "radeonfb: output %s on UNIPHY obj 0x%02x link %c "
        "(DIG%ld, PPLL id %ld, connector obj 0x%02x)\n",
        FoundLcd ? "LCD" : "DFP",
        DeviceExtension->EncoderObjId,
        DeviceExtension->LinkB ? 'B' : 'A',
        DeviceExtension->DigEncoder + 1,
        DeviceExtension->PllId,
        DeviceExtension->ConnectorObjId);

    return TRUE;
}
