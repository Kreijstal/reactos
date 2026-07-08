/*
 * PROJECT:     ReactOS AMD Radeon ATOM-BIOS Framebuffer Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Videoport miniport shell: PCI probing, BAR mapping, the
 *              framebuf.dll IOCTL surface, and the AtomBIOS bring-up
 *              orchestration.  Modelled on uefifb (IOCTL surface) and
 *              virtgpu (PCI plumbing); the hardware knowledge is ported
 *              from the Linux v6.6 radeon driver.
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 *
 * Safety contract: if anything in the bring-up path fails, HwFindAdapter
 * returns ERROR_DEV_NOT_EXIST so the VBE/VGA fallback still drives the box.
 * A Radeon owner must never end up with a black screen because of a bug in
 * this driver.
 */

#include "radeon.h"

/* Default modes (VESA DMT timings) offered when no panel is present.
 * The native panel mode replaces this list on laptop panels. */
static const RADEONFB_TIMING RadeonDefaultTimings[] =
{
    /* 1024x768@60: 65MHz */
    { 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806,
      RADEONFB_MODE_FLAG_NHSYNC | RADEONFB_MODE_FLAG_NVSYNC },
    /* 1280x1024@60: 108MHz */
    { 108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, 0 },
    /* 1366x768@60: 85.5MHz */
    { 85500, 1366, 1436, 1579, 1792, 768, 771, 774, 798, 0 },
    /* 1920x1080@60: 148.5MHz */
    { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 0 },
};

/* Supported PCI device ids, transcribed from Linux v6.6
 * include/drm/drm_pciids.h (radeon_PCI_IDS).  Only families whose display
 * paths this driver actually ports (ARUBA DCE6.1, OLAND DCE6.4) plus the
 * display-less HAINAN parts that share the X550DP board. */
typedef struct _RADEONFB_PCI_ID
{
    USHORT DeviceId;
    USHORT Family;
    ULONG Flags;
} RADEONFB_PCI_ID;

static const RADEONFB_PCI_ID RadeonPciIds[] =
{
    /* Oland (DCE 6.4) */
    { 0x6600, CHIP_OLAND, 0 },
    { 0x6601, CHIP_OLAND, 0 },
    { 0x6602, CHIP_OLAND, 0 },
    { 0x6603, CHIP_OLAND, 0 },
    { 0x6604, CHIP_OLAND, 0 },
    { 0x6605, CHIP_OLAND, 0 },
    { 0x6606, CHIP_OLAND, 0 },
    { 0x6607, CHIP_OLAND, 0 },
    { 0x6608, CHIP_OLAND, 0 },
    { 0x6610, CHIP_OLAND, 0 },
    { 0x6611, CHIP_OLAND, 0 },
    { 0x6613, CHIP_OLAND, 0 },
    { 0x6617, CHIP_OLAND, 0 },
    { 0x6620, CHIP_OLAND, 0 },
    { 0x6621, CHIP_OLAND, 0 },
    { 0x6623, CHIP_OLAND, 0 },
    { 0x6631, CHIP_OLAND, 0 },
    /* Hainan (Mars/Sun; NO display block - claimed as an off-screen
     * surface so the render-only companion GPU of PX laptops gets a
     * driver instead of an error bang) */
    { 0x6660, CHIP_HAINAN, 0 },
    { 0x6663, CHIP_HAINAN, 0 },
    { 0x6664, CHIP_HAINAN, 0 },
    { 0x6665, CHIP_HAINAN, 0 },
    { 0x6667, CHIP_HAINAN, 0 },
    { 0x666F, CHIP_HAINAN, 0 },
    /* Aruba (Trinity/Richland APUs, DCE 6.1) */
    { 0x9900, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9901, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9903, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9904, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9905, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9906, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9907, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9908, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9909, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x990A, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x990B, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x990C, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x990D, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x990E, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x990F, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9910, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9913, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9917, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9918, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9919, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9990, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9991, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9992, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9993, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9994, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9995, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9996, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9997, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9998, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x9999, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x999A, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x999B, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x999C, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x999D, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x99A0, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x99A2, CHIP_ARUBA, RADEONFB_IS_IGP },
    { 0x99A4, CHIP_ARUBA, RADEONFB_IS_IGP },
};

/* REGISTER ACCESS ***********************************************************/

/* Port of r100_mm_rreg/wreg: direct for registers inside the mapped
 * aperture, MM_INDEX/MM_DATA indirection beyond it. */
ULONG
RadeonRegRead(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG Reg)
{
    if (Reg < DeviceExtension->RmmioLength)
    {
        return VideoPortReadRegisterUlong(
            (PULONG)(DeviceExtension->Rmmio + Reg));
    }

    VideoPortWriteRegisterUlong(
        (PULONG)(DeviceExtension->Rmmio + RADEON_MM_INDEX), Reg);
    return VideoPortReadRegisterUlong(
        (PULONG)(DeviceExtension->Rmmio + RADEON_MM_DATA));
}

VOID
RadeonRegWrite(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG Reg,
    _In_ ULONG Value)
{
    if (Reg < DeviceExtension->RmmioLength)
    {
        VideoPortWriteRegisterUlong(
            (PULONG)(DeviceExtension->Rmmio + Reg), Value);
        return;
    }

    VideoPortWriteRegisterUlong(
        (PULONG)(DeviceExtension->Rmmio + RADEON_MM_INDEX), Reg);
    VideoPortWriteRegisterUlong(
        (PULONG)(DeviceExtension->Rmmio + RADEON_MM_DATA), Value);
}

/* ATOM interpreter card_info callbacks (ports of the cail_* callbacks in
 * Linux radeon_device.c). */

static uint32_t
RadeonCailRegRead(struct card_info *Card, uint32_t Reg)
{
    return RadeonRegRead((PRADEONFB_DEVICE_EXTENSION)Card->dev, Reg);
}

static void
RadeonCailRegWrite(struct card_info *Card, uint32_t Reg, uint32_t Value)
{
    RadeonRegWrite((PRADEONFB_DEVICE_EXTENSION)Card->dev, Reg, Value);
}

/* No PCI I/O BAR mapping is attempted; like Linux' no-rio_mem fallback the
 * IIO accesses are routed through MMIO (register offsets are identical). */
static uint32_t
RadeonCailIoRegRead(struct card_info *Card, uint32_t Reg)
{
    return RadeonRegRead((PRADEONFB_DEVICE_EXTENSION)Card->dev, Reg);
}

static void
RadeonCailIoRegWrite(struct card_info *Card, uint32_t Reg, uint32_t Value)
{
    RadeonRegWrite((PRADEONFB_DEVICE_EXTENSION)Card->dev, Reg, Value);
}

/* MC indirect access, port of rv515_mc_rreg/wreg (rv515.c) - the accessor
 * radeon_asic.c installs for every family >= RV515. */
static uint32_t
RadeonCailMcRead(struct card_info *Card, uint32_t Reg)
{
    PRADEONFB_DEVICE_EXTENSION DeviceExtension = Card->dev;
    uint32_t Value;

    RadeonRegWrite(DeviceExtension, MC_IND_INDEX, 0x7f0000 | (Reg & 0xffff));
    Value = RadeonRegRead(DeviceExtension, MC_IND_DATA);
    RadeonRegWrite(DeviceExtension, MC_IND_INDEX, 0);
    return Value;
}

static void
RadeonCailMcWrite(struct card_info *Card, uint32_t Reg, uint32_t Value)
{
    PRADEONFB_DEVICE_EXTENSION DeviceExtension = Card->dev;

    RadeonRegWrite(DeviceExtension, MC_IND_INDEX, 0xff0000 | (Reg & 0xffff));
    RadeonRegWrite(DeviceExtension, MC_IND_DATA, Value);
    RadeonRegWrite(DeviceExtension, MC_IND_INDEX, 0);
}

/* No PLL indirect accessor exists for r5xx+ families (radeon_asic.c leaves
 * radeon_invalid_rreg installed); DCE6 command tables do not use ATOM_ARG_PLL. */
static uint32_t
RadeonCailPllRead(struct card_info *Card, uint32_t Reg)
{
    UNREFERENCED_PARAMETER(Card);
    VideoPortDebugPrint(Error,
        "radeonfb: unexpected ATOM PLL read (reg 0x%x)\n", Reg);
    return 0;
}

static void
RadeonCailPllWrite(struct card_info *Card, uint32_t Reg, uint32_t Value)
{
    UNREFERENCED_PARAMETER(Card);
    VideoPortDebugPrint(Error,
        "radeonfb: unexpected ATOM PLL write (reg 0x%x val 0x%x)\n",
        Reg, Value);
}

/* DRIVER ENTRY **************************************************************/

ULONG
NTAPI
DriverEntry(
    _In_ PVOID Context1,
    _In_ PVOID Context2)
{
    VIDEO_HW_INITIALIZATION_DATA InitData;

    VideoPortZeroMemory(&InitData, sizeof(InitData));
    InitData.HwInitDataSize = sizeof(VIDEO_HW_INITIALIZATION_DATA);
    InitData.StartingDeviceNumber = 0;
    InitData.AdapterInterfaceType = PCIBus;
    InitData.HwFindAdapter = RadeonFindAdapter;
    InitData.HwInitialize = RadeonInitialize;
    InitData.HwStartIO = RadeonStartIO;
    InitData.HwResetHw = RadeonResetHw;
    InitData.HwGetPowerState = RadeonGetPowerState;
    InitData.HwSetPowerState = RadeonSetPowerState;
    /* No interrupts, no child devices (monitor enumeration is left to the
     * default videoprt behavior). */
    InitData.HwGetVideoChildDescriptor = NULL;
    InitData.HwDeviceExtensionSize = sizeof(RADEONFB_DEVICE_EXTENSION);

    return VideoPortInitialize(Context1, Context2, &InitData, NULL);
}

/* ADAPTER PROBING ***********************************************************/

static const RADEONFB_PCI_ID *
RadeonLookupPciId(
    _In_ USHORT DeviceId)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(RadeonPciIds); i++)
    {
        if (RadeonPciIds[i].DeviceId == DeviceId)
            return &RadeonPciIds[i];
    }
    return NULL;
}

/*
 * Decode the six type-0 BARs from raw config space and pair them with the
 * lengths videoprt reported in the claimed access ranges (matched by base
 * address, like virtgpu does).
 */
static BOOLEAN
RadeonCaptureBars(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PPCI_COMMON_CONFIG PciConfig,
    _In_reads_(RangeCount) PVIDEO_ACCESS_RANGE Ranges,
    _In_ ULONG RangeCount)
{
    PHYSICAL_ADDRESS BarBase[PCI_TYPE0_ADDRESSES];
    BOOLEAN BarIsMem[PCI_TYPE0_ADDRESSES];
    BOOLEAN BarValid[PCI_TYPE0_ADDRESSES];
    ULONG BarLength[PCI_TYPE0_ADDRESSES];
    ULONG Bar, i;

    VideoPortZeroMemory(BarBase, sizeof(BarBase));
    VideoPortZeroMemory(BarIsMem, sizeof(BarIsMem));
    VideoPortZeroMemory(BarValid, sizeof(BarValid));
    VideoPortZeroMemory(BarLength, sizeof(BarLength));

    for (Bar = 0; Bar < PCI_TYPE0_ADDRESSES; Bar++)
    {
        ULONG Raw = PciConfig->u.type0.BaseAddresses[Bar];

        if (Raw == 0)
            continue;

        if (Raw & PCI_ADDRESS_IO_SPACE)
        {
            BarValid[Bar] = TRUE;
            BarIsMem[Bar] = FALSE;
            BarBase[Bar].QuadPart = Raw & PCI_ADDRESS_IO_ADDRESS_MASK;
        }
        else
        {
            BarValid[Bar] = TRUE;
            BarIsMem[Bar] = TRUE;
            BarBase[Bar].LowPart = Raw & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
            if ((Raw & PCI_ADDRESS_MEMORY_TYPE_MASK) == PCI_TYPE_64BIT &&
                (Bar + 1) < PCI_TYPE0_ADDRESSES)
            {
                BarBase[Bar].HighPart =
                    PciConfig->u.type0.BaseAddresses[Bar + 1];
                /* The next slot is the high half, not a BAR */
                Bar++;
            }
        }
    }

    /* Pair each BAR with the videoprt-reported range length */
    for (Bar = 0; Bar < PCI_TYPE0_ADDRESSES; Bar++)
    {
        if (!BarValid[Bar] || BarBase[Bar].QuadPart == 0)
            continue;

        for (i = 0; i < RangeCount; i++)
        {
            if (Ranges[i].RangeLength != 0 &&
                Ranges[i].RangeStart.QuadPart == BarBase[Bar].QuadPart &&
                (Ranges[i].RangeInIoSpace != 0) == !BarIsMem[Bar])
            {
                BarLength[Bar] = Ranges[i].RangeLength;
                break;
            }
        }
    }

    /* Frame buffer aperture: BAR 0 (64-bit on these parts).
     * Register aperture: BAR slot 2 (family < CHIP_BONAIRE, see
     * radeon_device.c rmmio_base selection). */
    if (!BarValid[0] || !BarIsMem[0] || BarBase[0].QuadPart == 0)
        return FALSE;
    if (!BarValid[2] || !BarIsMem[2] || BarBase[2].QuadPart == 0)
        return FALSE;

    DeviceExtension->FbPhysical = BarBase[0];
    DeviceExtension->FbBarLength = BarLength[0];
    DeviceExtension->RmmioPhysical = BarBase[2];
    DeviceExtension->RmmioLength = BarLength[2];

    /* If videoprt didn't report a length (legacy path), fall back to the
     * architectural minimum register aperture so MM_INDEX indirection
     * covers the rest. */
    if (DeviceExtension->RmmioLength == 0)
        DeviceExtension->RmmioLength = 0x40000;
    if (DeviceExtension->FbBarLength == 0)
        DeviceExtension->FbBarLength = RADEONFB_MAX_FB_MAP;

    return TRUE;
}

static VOID
RadeonEnablePciDecodes(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PPCI_COMMON_CONFIG PciConfig)
{
    USHORT Command;

    Command = PciConfig->Command |
              PCI_ENABLE_MEMORY_SPACE |
              PCI_ENABLE_IO_SPACE |
              PCI_ENABLE_BUS_MASTER;

    VideoPortSetBusData(DeviceExtension,
                        PCIConfiguration,
                        DeviceExtension->PciSlot,
                        &Command,
                        FIELD_OFFSET(PCI_COMMON_CONFIG, Command),
                        sizeof(Command));
}

static VOID
RadeonBuildModeInfo(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG Index)
{
    PRADEONFB_TIMING Timing = &DeviceExtension->Timings[Index];
    PVIDEO_MODE_INFORMATION Mode = &DeviceExtension->Modes[Index];
    ULONG Stride;
    ULONG RefreshNum, RefreshDen;
    const ULONG Dpi = 96;

    /* DCE4+ scanout pitch: keep the stride 256-byte aligned (matches the
     * Linux radeon pitch alignment for linear scanout surfaces). */
    Stride = (Timing->HDisplay * 4 + 255) & ~255UL;

    VideoPortZeroMemory(Mode, sizeof(*Mode));
    Mode->Length = sizeof(*Mode);
    Mode->ModeIndex = Index;
    Mode->VisScreenWidth = Timing->HDisplay;
    Mode->VisScreenHeight = Timing->VDisplay;
    Mode->ScreenStride = Stride;
    Mode->NumberOfPlanes = 1;
    Mode->BitsPerPlane = 32;

    RefreshNum = Timing->ClockKHz * 1000;
    RefreshDen = Timing->HTotal * Timing->VTotal;
    if (RefreshDen != 0)
        Mode->Frequency = (RefreshNum + RefreshDen / 2) / RefreshDen;
    if (Mode->Frequency == 0)
        Mode->Frequency = 60;

    Mode->XMillimeter =
        ((ULONGLONG)Timing->HDisplay * 254 + (Dpi * 5)) / (Dpi * 10);
    Mode->YMillimeter =
        ((ULONGLONG)Timing->VDisplay * 254 + (Dpi * 5)) / (Dpi * 10);

    Mode->NumberRedBits = 8;
    Mode->NumberGreenBits = 8;
    Mode->NumberBlueBits = 8;
    Mode->RedMask = 0x00FF0000;
    Mode->GreenMask = 0x0000FF00;
    Mode->BlueMask = 0x000000FF;
    Mode->DriverSpecificAttributeFlags = 0;

    Mode->AttributeFlags = VIDEO_MODE_GRAPHICS |
                           VIDEO_MODE_COLOR |
                           VIDEO_MODE_NO_OFF_SCREEN;

    Mode->VideoMemoryBitmapWidth = Timing->HDisplay;
    Mode->VideoMemoryBitmapHeight = Timing->VDisplay;
}

static VOID
RadeonBuildModeList(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    ULONG i;

    DeviceExtension->ModeCount = 0;

    if (DeviceExtension->HasPanel)
    {
        /* Panel: expose only the native timing; the EnableScaler table is
         * executed with SCALER_DISABLE, so non-native modes would not fill
         * the panel. */
        DeviceExtension->Timings[0] = DeviceExtension->NativeMode;
        DeviceExtension->ModeCount = 1;
    }
    else
    {
        for (i = 0; i < RTL_NUMBER_OF(RadeonDefaultTimings); i++)
        {
            const RADEONFB_TIMING *Timing = &RadeonDefaultTimings[i];
            ULONG SizeInBytes;

            /* MaxPixelClock from FirmwareInfo is in 10kHz units */
            if (DeviceExtension->MaxPixelClock != 0 &&
                (Timing->ClockKHz / 10) > DeviceExtension->MaxPixelClock)
            {
                continue;
            }

            SizeInBytes = ((Timing->HDisplay * 4 + 255) & ~255UL) *
                          Timing->VDisplay;
            if (SizeInBytes > DeviceExtension->FbMapLength)
                continue;

            DeviceExtension->Timings[DeviceExtension->ModeCount++] = *Timing;
        }
    }

    for (i = 0; i < DeviceExtension->ModeCount; i++)
        RadeonBuildModeInfo(DeviceExtension, i);
}

VP_STATUS
NTAPI
RadeonFindAdapter(
    _In_ PVOID HwDeviceExtension,
    _In_ PVOID HwContext,
    _In_ PWSTR ArgumentString,
    _Inout_ PVIDEO_PORT_CONFIG_INFO ConfigInfo,
    _Out_ PUCHAR Again)
{
    PRADEONFB_DEVICE_EXTENSION DeviceExtension = HwDeviceExtension;
    VIDEO_ACCESS_RANGE AccessRanges[RADEONFB_MAX_ACCESS_RANGES];
    PCI_COMMON_CONFIG PciConfig;
    const RADEONFB_PCI_ID *PciId;
    USHORT VendorId = 0x1002;
    VP_STATUS Status;
    ULONG Read;
    ULONG MappedLength;
    ULONG InIoSpace;
    PVOID Mapped;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(ArgumentString);

    *Again = FALSE;
    VideoPortZeroMemory(DeviceExtension, sizeof(*DeviceExtension));

    if (ConfigInfo->Length < sizeof(VIDEO_PORT_CONFIG_INFO))
        return ERROR_INVALID_PARAMETER;

    VideoPortZeroMemory(AccessRanges, sizeof(AccessRanges));
    Status = VideoPortGetAccessRanges(DeviceExtension,
                                      0,
                                      NULL,
                                      RADEONFB_MAX_ACCESS_RANGES,
                                      AccessRanges,
                                      &VendorId,
                                      NULL,
                                      &DeviceExtension->PciSlot);
    if (Status != NO_ERROR)
    {
        VideoPortDebugPrint(Trace,
            "radeonfb: VideoPortGetAccessRanges failed (0x%lx)\n", Status);
        return ERROR_DEV_NOT_EXIST;
    }

    DeviceExtension->PciBus = ConfigInfo->SystemIoBusNumber;

    Read = VideoPortGetBusData(DeviceExtension,
                               PCIConfiguration,
                               DeviceExtension->PciSlot,
                               &PciConfig,
                               0,
                               sizeof(PciConfig));
    if (Read < PCI_COMMON_HDR_LENGTH || PciConfig.VendorID != 0x1002)
        return ERROR_DEV_NOT_EXIST;

    PciId = RadeonLookupPciId(PciConfig.DeviceID);
    if (PciId == NULL)
    {
        VideoPortDebugPrint(Warn,
            "radeonfb: unsupported AMD device 0x%04x\n", PciConfig.DeviceID);
        return ERROR_DEV_NOT_EXIST;
    }

    DeviceExtension->VendorId = PciConfig.VendorID;
    DeviceExtension->DeviceId = PciConfig.DeviceID;
    DeviceExtension->Family = PciId->Family;
    DeviceExtension->ChipFlags = PciId->Flags;
    DeviceExtension->HasDce = (PciId->Family != CHIP_HAINAN);

    if (!RadeonCaptureBars(DeviceExtension, &PciConfig,
                           AccessRanges, RADEONFB_MAX_ACCESS_RANGES))
    {
        VideoPortDebugPrint(Error, "radeonfb: unusable BAR layout\n");
        return ERROR_DEV_NOT_EXIST;
    }

    RadeonEnablePciDecodes(DeviceExtension, &PciConfig);

    /* Map the register aperture (uncached device memory) */
    MappedLength = DeviceExtension->RmmioLength;
    InIoSpace = VIDEO_MEMORY_SPACE_MEMORY;
    Mapped = NULL;
    Status = VideoPortMapMemory(DeviceExtension,
                                DeviceExtension->RmmioPhysical,
                                &MappedLength,
                                &InIoSpace,
                                &Mapped);
    if (Status != NO_ERROR || Mapped == NULL)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: register BAR mapping failed (0x%lx)\n", Status);
        return ERROR_DEV_NOT_EXIST;
    }
    DeviceExtension->Rmmio = Mapped;
    DeviceExtension->RmmioLength = MappedLength;

    DeviceExtension->FbMapLength = DeviceExtension->FbBarLength;
    if (DeviceExtension->FbMapLength > RADEONFB_MAX_FB_MAP)
        DeviceExtension->FbMapLength = RADEONFB_MAX_FB_MAP;

    if (DeviceExtension->HasDce)
    {
        if (!RadeonAtomBringUp(DeviceExtension))
        {
            VideoPortDebugPrint(Error,
                "radeonfb: ATOM bring-up failed for 0x%04x, "
                "leaving the device to the VGA/VBE fallback\n",
                DeviceExtension->DeviceId);
            VideoPortUnmapMemory(DeviceExtension, DeviceExtension->Rmmio, NULL);
            DeviceExtension->Rmmio = NULL;
            if (DeviceExtension->Bios != NULL)
            {
                ExFreePoolWithTag(DeviceExtension->Bios, RADEONFB_TAG);
                DeviceExtension->Bios = NULL;
            }
            return ERROR_DEV_NOT_EXIST;
        }
    }
    else
    {
        VideoPortDebugPrint(Info,
            "radeonfb: 0x%04x has no display block (Hainan); exposing an "
            "off-screen VRAM surface only\n", DeviceExtension->DeviceId);
    }

    RadeonBuildModeList(DeviceExtension);
    if (DeviceExtension->ModeCount == 0)
    {
        VideoPortDebugPrint(Error, "radeonfb: no usable modes\n");
        VideoPortUnmapMemory(DeviceExtension, DeviceExtension->Rmmio, NULL);
        DeviceExtension->Rmmio = NULL;
        return ERROR_DEV_NOT_EXIST;
    }

    VideoPortDebugPrint(Info,
        "radeonfb: claimed 1002:%04x (family %lu%s), FB 0x%I64x/%lu KB, "
        "MMIO 0x%I64x/%lu KB, %lu mode(s)%s\n",
        DeviceExtension->DeviceId,
        DeviceExtension->Family,
        (DeviceExtension->ChipFlags & RADEONFB_IS_IGP) ? " IGP" : "",
        DeviceExtension->FbPhysical.QuadPart,
        DeviceExtension->FbBarLength / 1024,
        DeviceExtension->RmmioPhysical.QuadPart,
        DeviceExtension->RmmioLength / 1024,
        DeviceExtension->ModeCount,
        DeviceExtension->HasPanel ? ", native panel" : "");

    ConfigInfo->NumEmulatorAccessEntries = 0;
    ConfigInfo->EmulatorAccessEntries = NULL;
    ConfigInfo->EmulatorAccessEntriesContext = 0;
    ConfigInfo->HardwareStateSize = 0;
    ConfigInfo->VdmPhysicalVideoMemoryAddress.QuadPart = 0;
    ConfigInfo->VdmPhysicalVideoMemoryLength = 0;

    return NO_ERROR;
}

BOOLEAN
NTAPI
RadeonInitialize(
    _In_ PVOID HwDeviceExtension)
{
    /* The actual mode-set is deferred to the first
     * IOCTL_VIDEO_SET_CURRENT_MODE, keeping the firmware-lit console alive
     * until win32k takes over. */
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    return TRUE;
}

/* IOCTL SURFACE *************************************************************/

BOOLEAN
NTAPI
RadeonStartIO(
    _In_ PVOID HwDeviceExtension,
    _In_ PVIDEO_REQUEST_PACKET RequestPacket)
{
    PRADEONFB_DEVICE_EXTENSION DeviceExtension = HwDeviceExtension;
    VP_STATUS Status = ERROR_INVALID_FUNCTION;
    PVIDEO_MODE_INFORMATION ModeInfo;
    PVIDEO_MEMORY VideoMemory;
    PVIDEO_MEMORY_INFORMATION MemoryInfo;
    PVIDEO_MODE VideoMode;
    PVIDEO_NUM_MODES NumModes;
    PVIDEO_POINTER_CAPABILITIES PointerCaps;
    ULONG InIoSpace;
    ULONG RequestedMode;
    ULONG i;

    switch (RequestPacket->IoControlCode)
    {
        case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_NUM_MODES))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }
            NumModes = (PVIDEO_NUM_MODES)RequestPacket->OutputBuffer;
            NumModes->NumModes = DeviceExtension->ModeCount;
            NumModes->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
            RequestPacket->StatusBlock->Information = sizeof(VIDEO_NUM_MODES);
            Status = NO_ERROR;
            break;

        case IOCTL_VIDEO_QUERY_AVAIL_MODES:
            if (RequestPacket->OutputBufferLength <
                DeviceExtension->ModeCount * sizeof(VIDEO_MODE_INFORMATION))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }
            ModeInfo = (PVIDEO_MODE_INFORMATION)RequestPacket->OutputBuffer;
            for (i = 0; i < DeviceExtension->ModeCount; i++)
            {
                VideoPortMoveMemory(&ModeInfo[i],
                                    &DeviceExtension->Modes[i],
                                    sizeof(VIDEO_MODE_INFORMATION));
            }
            RequestPacket->StatusBlock->Information =
                DeviceExtension->ModeCount * sizeof(VIDEO_MODE_INFORMATION);
            Status = NO_ERROR;
            break;

        case IOCTL_VIDEO_QUERY_CURRENT_MODE:
            if (RequestPacket->OutputBufferLength <
                sizeof(VIDEO_MODE_INFORMATION))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }
            ModeInfo = (PVIDEO_MODE_INFORMATION)RequestPacket->OutputBuffer;
            VideoPortMoveMemory(ModeInfo,
                                &DeviceExtension->Modes[DeviceExtension->CurrentMode],
                                sizeof(VIDEO_MODE_INFORMATION));
            RequestPacket->StatusBlock->Information =
                sizeof(VIDEO_MODE_INFORMATION);
            Status = NO_ERROR;
            break;

        case IOCTL_VIDEO_SET_CURRENT_MODE:
            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MODE))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }
            VideoMode = (PVIDEO_MODE)RequestPacket->InputBuffer;
            RequestedMode = VideoMode->RequestedMode & ~(VIDEO_MODE_NO_ZERO_MEMORY |
                                                         VIDEO_MODE_MAP_MEM_LINEAR);
            if (RequestedMode >= DeviceExtension->ModeCount)
            {
                Status = ERROR_INVALID_PARAMETER;
                break;
            }
            if (DeviceExtension->HasDce)
            {
                if (!RadeonSetMode(DeviceExtension,
                                   &DeviceExtension->Timings[RequestedMode]))
                {
                    VideoPortDebugPrint(Error,
                        "radeonfb: ATOM mode-set failed for mode %lu\n",
                        RequestedMode);
                    Status = ERROR_INVALID_PARAMETER;
                    break;
                }
                DeviceExtension->ModeProgrammed = TRUE;
            }
            DeviceExtension->CurrentMode = RequestedMode;
            Status = NO_ERROR;
            break;

        case IOCTL_VIDEO_RESET_DEVICE:
            /* Returning to text mode is delegated to HwResetHw/HAL; do not
             * touch the hardware here. */
            Status = NO_ERROR;
            break;

        case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY) ||
                RequestPacket->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }
            VideoMemory = (PVIDEO_MEMORY)RequestPacket->InputBuffer;
            MemoryInfo = (PVIDEO_MEMORY_INFORMATION)RequestPacket->OutputBuffer;
            InIoSpace = VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_P6CACHE;
            MemoryInfo->VideoRamBase = VideoMemory->RequestedVirtualAddress;
            MemoryInfo->VideoRamLength = DeviceExtension->FbMapLength;
            Status = VideoPortMapMemory(DeviceExtension,
                                        DeviceExtension->FbPhysical,
                                        &MemoryInfo->VideoRamLength,
                                        &InIoSpace,
                                        &MemoryInfo->VideoRamBase);
            if (Status == NO_ERROR)
            {
                MemoryInfo->FrameBufferBase = MemoryInfo->VideoRamBase;
                MemoryInfo->FrameBufferLength = MemoryInfo->VideoRamLength;
                DeviceExtension->MappedFrameBuffer = MemoryInfo->VideoRamBase;
                RequestPacket->StatusBlock->Information =
                    sizeof(VIDEO_MEMORY_INFORMATION);
            }
            else
            {
                VideoPortDebugPrint(Error,
                    "radeonfb: VideoPortMapMemory failed (0x%lx)\n", Status);
            }
            break;

        case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }
            VideoMemory = (PVIDEO_MEMORY)RequestPacket->InputBuffer;
            Status = VideoPortUnmapMemory(DeviceExtension,
                                          VideoMemory->RequestedVirtualAddress,
                                          NULL);
            DeviceExtension->MappedFrameBuffer = NULL;
            break;

        case IOCTL_VIDEO_QUERY_POINTER_CAPABILITIES:
            if (RequestPacket->OutputBufferLength <
                sizeof(VIDEO_POINTER_CAPABILITIES))
            {
                Status = ERROR_INSUFFICIENT_BUFFER;
                break;
            }
            PointerCaps =
                (PVIDEO_POINTER_CAPABILITIES)RequestPacket->OutputBuffer;
            VideoPortZeroMemory(PointerCaps, sizeof(*PointerCaps));
            /* No hardware cursor - GDI falls back to software */
            RequestPacket->StatusBlock->Information =
                sizeof(VIDEO_POINTER_CAPABILITIES);
            Status = NO_ERROR;
            break;

        default:
            /* Palette/pointer/VDM passthrough intentionally unsupported;
             * framebuf.dll copes with the rejection. */
            Status = ERROR_INVALID_FUNCTION;
            break;
    }

    RequestPacket->StatusBlock->Status = Status;
    return TRUE;
}

BOOLEAN
NTAPI
RadeonResetHw(
    _In_ PVOID DeviceExtension,
    _In_ ULONG Columns,
    _In_ ULONG Rows)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(Columns);
    UNREFERENCED_PARAMETER(Rows);
    /* Let the HAL perform the INT 10h VGA reset on BIOS systems. */
    return FALSE;
}

VP_STATUS
NTAPI
RadeonGetPowerState(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG HwId,
    _In_ PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    UNREFERENCED_PARAMETER(HwId);

    if (VideoPowerControl == NULL)
        return ERROR_INVALID_PARAMETER;

    VideoPowerControl->PowerState = VideoPowerOn;
    return NO_ERROR;
}

VP_STATUS
NTAPI
RadeonSetPowerState(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG HwId,
    _In_ PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    UNREFERENCED_PARAMETER(HwId);

    if (VideoPowerControl == NULL)
        return ERROR_INVALID_PARAMETER;

    /* Accept power-on; DPMS power-down sequencing is not wired yet. */
    if (VideoPowerControl->PowerState == VideoPowerOn)
        return NO_ERROR;

    return ERROR_INVALID_FUNCTION;
}

/* ATOM BRING-UP *************************************************************/

BOOLEAN
RadeonAtomBringUp(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    struct card_info *CardInfo = &DeviceExtension->CardInfo;

    if (!RadeonGetBios(DeviceExtension))
    {
        VideoPortDebugPrint(Error, "radeonfb: no usable ATOM VBIOS found\n");
        return FALSE;
    }

    CardInfo->dev = DeviceExtension;
    CardInfo->family = DeviceExtension->Family;
    CardInfo->reg_read = RadeonCailRegRead;
    CardInfo->reg_write = RadeonCailRegWrite;
    CardInfo->ioreg_read = RadeonCailIoRegRead;
    CardInfo->ioreg_write = RadeonCailIoRegWrite;
    CardInfo->mc_read = RadeonCailMcRead;
    CardInfo->mc_write = RadeonCailMcWrite;
    CardInfo->pll_read = RadeonCailPllRead;
    CardInfo->pll_write = RadeonCailPllWrite;

    DeviceExtension->AtomContext =
        atom_parse(CardInfo, DeviceExtension->Bios);
    if (DeviceExtension->AtomContext == NULL)
    {
        VideoPortDebugPrint(Error, "radeonfb: atom_parse failed\n");
        return FALSE;
    }

    if (atom_allocate_fb_scratch(DeviceExtension->AtomContext) != 0)
    {
        VideoPortDebugPrint(Error, "radeonfb: ATOM scratch alloc failed\n");
        return FALSE;
    }

    /* The adapter must have been POSTed (it is the boot display, or the
     * system BIOS initialized it).  Programming display tables on an
     * un-POSTed adapter would require running ASIC_Init first; that is out
     * of scope - decline and leave the device alone. */
    if (RadeonRegRead(DeviceExtension, CONFIG_MEMSIZE) == 0)
    {
        VideoPortDebugPrint(Warn,
            "radeonfb: adapter not POSTed, declining\n");
        return FALSE;
    }

    if (!RadeonAtomGetClockInfo(DeviceExtension))
    {
        VideoPortDebugPrint(Error, "radeonfb: no ATOM FirmwareInfo\n");
        return FALSE;
    }

    if (!RadeonAtomParseObjectTable(DeviceExtension))
    {
        VideoPortDebugPrint(Warn,
            "radeonfb: no supported display output found\n");
        return FALSE;
    }

    if (DeviceExtension->HasPanel &&
        !RadeonAtomGetLvdsInfo(DeviceExtension))
    {
        VideoPortDebugPrint(Error,
            "radeonfb: LCD present but no LVDS_Info table\n");
        return FALSE;
    }

    return TRUE;
}
