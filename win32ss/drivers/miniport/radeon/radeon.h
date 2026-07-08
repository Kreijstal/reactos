/*
 * PROJECT:     ReactOS AMD Radeon ATOM-BIOS Framebuffer Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared declarations for the radeonfb video miniport.
 *              The driver reads the VBIOS, executes AtomBIOS command tables
 *              (interpreter imported from the Linux v6.6 radeon driver) to
 *              light up the panel/monitor at its native mode, and exposes
 *              the linear VRAM scanout to the generic framebuf.dll display
 *              driver.
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#ifndef _RADEONFB_PCH_
#define _RADEONFB_PCH_

#include "atom.h"           /* pulls in atom_os.h (ntddk/miniport/video) */
#include "radeon_family.h"
#include "radeon_regs.h"

#define RADEONFB_TAG 'FdaR'

#define RADEONFB_MAX_ACCESS_RANGES  8
#define RADEONFB_MAX_MODES          8

/* Cap on how much of the (potentially 256MB+) VRAM BAR is handed to the
 * display driver: enough for the largest supported mode with slack. */
#define RADEONFB_MAX_FB_MAP         (32 * 1024 * 1024)

/* Chip flags, from Linux radeon_family.h (RADEON_IS_IGP et al.) - only the
 * ones the miniport consumes. */
#define RADEONFB_IS_IGP             0x00020000

/* Display timing (subset of drm_display_mode; all in pixels/kHz). */
typedef struct _RADEONFB_TIMING
{
    ULONG ClockKHz;
    ULONG HDisplay;
    ULONG HSyncStart;
    ULONG HSyncEnd;
    ULONG HTotal;
    ULONG VDisplay;
    ULONG VSyncStart;
    ULONG VSyncEnd;
    ULONG VTotal;
    ULONG Flags;
} RADEONFB_TIMING, *PRADEONFB_TIMING;

/* RADEONFB_TIMING.Flags - same semantics as the DRM_MODE_FLAG_* values the
 * imported parsing code assigns. */
#define RADEONFB_MODE_FLAG_NHSYNC     0x00000001
#define RADEONFB_MODE_FLAG_NVSYNC     0x00000002
#define RADEONFB_MODE_FLAG_CSYNC      0x00000004
#define RADEONFB_MODE_FLAG_INTERLACE  0x00000008
#define RADEONFB_MODE_FLAG_DBLSCAN    0x00000010

/* PLL limits/state, narrow port of struct radeon_pll (radeon_mode.h) with
 * the values radeon_compute_pll_avivo consumes. */
typedef struct _RADEONFB_PLL
{
    ULONG ReferenceFreq;       /* in 10kHz units, from ATOM FirmwareInfo */
    ULONG ReferenceDiv;
    ULONG PostDiv;
    ULONG PllInMin;
    ULONG PllInMax;
    ULONG PllOutMin;           /* in 10kHz units */
    ULONG PllOutMax;
    ULONG LcdPllOutMin;
    ULONG LcdPllOutMax;
    ULONG MinRefDiv;
    ULONG MaxRefDiv;
    ULONG MinPostDiv;
    ULONG MaxPostDiv;
    ULONG MinFeedbackDiv;
    ULONG MaxFeedbackDiv;
    ULONG Flags;
} RADEONFB_PLL, *PRADEONFB_PLL;

/* RADEONFB_PLL.Flags - RADEON_PLL_* from radeon_mode.h (only the ones the
 * avivo computation path uses). */
#define RADEONFB_PLL_USE_REF_DIV           (1 << 2)
#define RADEONFB_PLL_PREFER_MINM_OVER_MAXP (1 << 8)
#define RADEONFB_PLL_USE_FRAC_FB_DIV       (1 << 10)
#define RADEONFB_PLL_USE_POST_DIV          (1 << 12)
#define RADEONFB_PLL_IS_LCD                (1 << 13)

typedef struct _RADEONFB_DEVICE_EXTENSION
{
    /* PCI identity */
    ULONG PciBus;
    ULONG PciSlot;              /* PCI_SLOT_NUMBER.u.AsULONG */
    USHORT VendorId;
    USHORT DeviceId;
    ULONG Family;               /* enum radeon_family */
    ULONG ChipFlags;            /* RADEONFB_IS_IGP */

    /* BARs */
    PHYSICAL_ADDRESS FbPhysical;    /* BAR0: VRAM aperture */
    ULONG FbBarLength;
    ULONG FbMapLength;              /* capped length handed to framebuf */
    PHYSICAL_ADDRESS RmmioPhysical; /* register aperture (BAR slot 2) */
    ULONG RmmioLength;
    PUCHAR Rmmio;                   /* kernel mapping of the register BAR */

    /* VBIOS / ATOM state */
    PUCHAR Bios;
    ULONG BiosLength;
    struct card_info CardInfo;
    struct atom_context *AtomContext;
    BOOLEAN DispEngPllInitDone;

    /* Clock/PLL info from ATOM FirmwareInfo */
    RADEONFB_PLL P1Pll;
    ULONG DefaultDispClk;       /* 10kHz units */
    ULONG DpExtClk;             /* 10kHz units */
    ULONG MaxPixelClock;        /* 10kHz units */

    /* Output routing from the ATOM object table */
    BOOLEAN HasDce;             /* FALSE on CHIP_HAINAN (no display block) */
    BOOLEAN HasOutput;
    USHORT EncoderObjId;        /* ENCODER_OBJECT_ID_INTERNAL_UNIPHYx */
    BOOLEAN LinkB;
    USHORT ConnectorObjId;
    ULONG DeviceTag;            /* ATOM_DEVICE_*_SUPPORT bit */
    LONG DigEncoder;            /* DIG frontend index */
    ULONG EncoderMode;          /* ATOM_ENCODER_MODE_* */
    LONG PllId;                 /* ATOM_PPLLx */

    /* Panel */
    BOOLEAN HasPanel;
    RADEONFB_TIMING NativeMode;
    UCHAR LcdMisc;

    /* Modes exposed to framebuf */
    RADEONFB_TIMING Timings[RADEONFB_MAX_MODES];
    VIDEO_MODE_INFORMATION Modes[RADEONFB_MAX_MODES];
    ULONG ModeCount;
    ULONG CurrentMode;
    BOOLEAN ModeProgrammed;

    PVOID MappedFrameBuffer;
} RADEONFB_DEVICE_EXTENSION, *PRADEONFB_DEVICE_EXTENSION;

/* Register access (radeon.c) */
ULONG
RadeonRegRead(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG Reg);

VOID
RadeonRegWrite(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG Reg,
    _In_ ULONG Value);

/* VBIOS acquisition (radeon_bios.c) */
BOOLEAN
RadeonGetBios(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension);

/* ATOM data-table parsing (radeon_atombios.c) */
BOOLEAN
RadeonAtomGetClockInfo(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension);

BOOLEAN
RadeonAtomGetLvdsInfo(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension);

BOOLEAN
RadeonAtomParseObjectTable(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension);

/* ATOM mode-setting (radeon_modeset.c) */
BOOLEAN
RadeonSetMode(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PRADEONFB_TIMING Timing);

/* Bring-up orchestration (radeon.c) */
BOOLEAN
RadeonAtomBringUp(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension);

/* Miniport entry points (radeon.c) */
VP_STATUS
NTAPI
RadeonFindAdapter(
    _In_ PVOID HwDeviceExtension,
    _In_ PVOID HwContext,
    _In_ PWSTR ArgumentString,
    _Inout_ PVIDEO_PORT_CONFIG_INFO ConfigInfo,
    _Out_ PUCHAR Again);

BOOLEAN
NTAPI
RadeonInitialize(
    _In_ PVOID HwDeviceExtension);

BOOLEAN
NTAPI
RadeonStartIO(
    _In_ PVOID HwDeviceExtension,
    _In_ PVIDEO_REQUEST_PACKET RequestPacket);

BOOLEAN
NTAPI
RadeonResetHw(
    _In_ PVOID DeviceExtension,
    _In_ ULONG Columns,
    _In_ ULONG Rows);

VP_STATUS
NTAPI
RadeonGetPowerState(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG HwId,
    _In_ PVIDEO_POWER_MANAGEMENT VideoPowerControl);

VP_STATUS
NTAPI
RadeonSetPowerState(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG HwId,
    _In_ PVIDEO_POWER_MANAGEMENT VideoPowerControl);

#endif /* _RADEONFB_PCH_ */
