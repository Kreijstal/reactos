/*
 * PROJECT:     ReactOS Console Text-Mode Device Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Framebuffer console backend for platforms without VGA text mode.
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 */

/* INCLUDES ******************************************************************/

#include "blue.h"
#include <arc/arc.h> /* Defines _ARC_, exposing the kernel ARC configuration API */
#include <ndk/halfuncs.h>
#include <ndk/kefuncs.h>

#define NDEBUG
#include <debug.h>

/* Boot-time (POST) display discovery helper functions (FindBootDisplay) */
#include <drivers/bootvid/framebuf.c>

/*
 * The boot display glyphs: 8x13 font shared with the framebuffer
 * boot video driver, used until a font is loaded with
 * IOCTL_CONSOLE_LOADFONT (which provides 8x8 glyphs).
 */
#define FB_DEFAULT_FONT_HEIGHT  13
#define FB_LOADED_FONT_HEIGHT   8
#define FB_FONT_WIDTH           8
extern const UCHAR VidpFontData[256 * FB_DEFAULT_FONT_HEIGHT];

/* GLOBALS *******************************************************************/

static PUCHAR ScrFbBase = NULL;     /* Mapped VA of the visible framebuffer */
static ULONG ScrFbDelta = 0;        /* Bytes per scan line */
static ULONG ScrFbWidth = 0;        /* Visible width in pixels  */
static ULONG ScrFbHeight = 0;       /* Visible height in pixels */
static ULONG ScrFbPalette[16];      /* Pixel values of the 16 text colors */
static ULONG ScrFbLastCursor = 0;   /* Last cell drawn with the cursor overlay */

/* The standard 16-color text palette, as RGB triples */
static const UCHAR ScrFbColors[16][3] =
{
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xC0}, {0x00, 0xC0, 0x00}, {0x00, 0xC0, 0xC0},
    {0xC0, 0x00, 0x00}, {0xC0, 0x00, 0xC0}, {0xC0, 0xC0, 0x00}, {0xC0, 0xC0, 0xC0},
    {0x80, 0x80, 0x80}, {0x00, 0x00, 0xFF}, {0x00, 0xFF, 0x00}, {0x00, 0xFF, 0xFF},
    {0xFF, 0x00, 0x00}, {0xFF, 0x00, 0xFF}, {0xFF, 0xFF, 0x00}, {0xFF, 0xFF, 0xFF}
};

/* PRIVATE FUNCTIONS *********************************************************/

static ULONG
ScrFbPackChannel(
    _In_ UCHAR Value,
    _In_ ULONG Mask)
{
    ULONG Shift = 0, Width = 0;

    if (Mask == 0)
        return 0;

    while (!(Mask & (1UL << Shift)))
        Shift++;
    while ((Shift + Width < 32) && (Mask & (1UL << (Shift + Width))))
        Width++;

    if (Width >= 8)
        return ((ULONG)Value << (Shift + Width - 8)) & Mask;
    return ((ULONG)(Value >> (8 - Width)) << Shift) & Mask;
}

static VOID
ScrFbInitializePalette(
    _In_ const CM_FRAMEBUF_DEVICE_DATA* VideoConfigData)
{
    ULONG RedMask   = VideoConfigData->PixelMasks.RedMask;
    ULONG GreenMask = VideoConfigData->PixelMasks.GreenMask;
    ULONG BlueMask  = VideoConfigData->PixelMasks.BlueMask;
    ULONG i;

    /* Without explicit channel masks, assume the common XRGB8888 layout */
    if ((RedMask | GreenMask | BlueMask) == 0)
    {
        RedMask   = 0x00FF0000;
        GreenMask = 0x0000FF00;
        BlueMask  = 0x000000FF;
    }

    for (i = 0; i < RTL_NUMBER_OF(ScrFbPalette); i++)
    {
        ScrFbPalette[i] = ScrFbPackChannel(ScrFbColors[i][0], RedMask)   |
                          ScrFbPackChannel(ScrFbColors[i][1], GreenMask) |
                          ScrFbPackChannel(ScrFbColors[i][2], BlueMask);
    }
}

static VOID
ScrFbRenderCell(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ ULONG Offset,
    _In_ BOOLEAN CursorOverlay)
{
    UCHAR Char = DeviceExtension->VideoMemory[Offset * 2];
    UCHAR Attr = DeviceExtension->VideoMemory[Offset * 2 + 1];
    ULONG Foreground, Background;
    const UCHAR* Glyph;
    ULONG GlyphHeight;
    ULONG CellHeight = DeviceExtension->ScanLines;
    PUCHAR CellStart;
    ULONG ScanLine, Bit;

    Foreground = ScrFbPalette[Attr & 0x0F];
    Background = ScrFbPalette[(Attr >> 4) & 0x0F];

    /* Draw the cursor as an inverted cell */
    if (CursorOverlay)
    {
        ULONG Swap = Foreground;
        Foreground = Background;
        Background = Swap;
    }

    if (DeviceExtension->FontBitfield)
    {
        GlyphHeight = FB_LOADED_FONT_HEIGHT;
        Glyph = &DeviceExtension->FontBitfield[Char * GlyphHeight];
    }
    else
    {
        GlyphHeight = FB_DEFAULT_FONT_HEIGHT;
        Glyph = &VidpFontData[Char * GlyphHeight];
    }

    CellStart = ScrFbBase +
        (Offset / DeviceExtension->Columns) * CellHeight * ScrFbDelta +
        (Offset % DeviceExtension->Columns) * FB_FONT_WIDTH * sizeof(ULONG);

    for (ScanLine = 0; ScanLine < CellHeight; ScanLine++)
    {
        UCHAR Bits = (ScanLine < GlyphHeight) ? Glyph[ScanLine] : 0;
        PULONG Pixel = (PULONG)(CellStart + ScanLine * ScrFbDelta);

        for (Bit = 0; Bit < FB_FONT_WIDTH; Bit++)
        {
            *Pixel++ = (Bits & (0x80 >> Bit)) ? Foreground : Background;
        }
    }
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * Locates and maps the boot display framebuffer. Must be called at
 * boot-driver initialization time, while the loader block (and with it
 * the ARC configuration tree describing the display) is still alive.
 */
NTSTATUS
ScrFbInitialize(VOID)
{
    PHYSICAL_ADDRESS VramAddress, FrameBuffer, TranslatedAddress;
    ULONG VramSize;
    CM_FRAMEBUF_DEVICE_DATA VideoConfigData;
    INTERFACE_TYPE Interface;
    ULONG BusNumber;
    ULONG AddressSpace;
    ULONG FrameBufferSize, MappedSize;
    PVOID FrameBufferBase;
    NTSTATUS Status;

    Status = FindBootDisplay(&VramAddress,
                             &VramSize,
                             &VideoConfigData,
                             NULL, // MonitorConfigData
                             &Interface,
                             &BusNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No boot framebuffer display found\n");
        return Status;
    }

    if (VideoConfigData.BitsPerPixel != 32)
    {
        DPRINT1("Unsupported framebuffer depth %u bpp\n",
                VideoConfigData.BitsPerPixel);
        return STATUS_NOT_SUPPORTED;
    }

    if ((VideoConfigData.ScreenWidth < FB_FONT_WIDTH) ||
        (VideoConfigData.ScreenHeight < FB_DEFAULT_FONT_HEIGHT) ||
        (VideoConfigData.ScreenWidth > VideoConfigData.PixelsPerScanLine))
    {
        DPRINT1("Unusable framebuffer geometry %lux%lu\n",
                VideoConfigData.ScreenWidth, VideoConfigData.ScreenHeight);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    FrameBuffer.QuadPart = VramAddress.QuadPart + VideoConfigData.FrameBufferOffset;
    FrameBufferSize = VideoConfigData.ScreenHeight *
                      VideoConfigData.PixelsPerScanLine * sizeof(ULONG);

    /* Verify that the framebuffer actually fits inside the video RAM */
    if (FrameBuffer.QuadPart + FrameBufferSize > VramAddress.QuadPart + VramSize)
    {
        DPRINT1("The framebuffer exceeds video memory bounds!\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Translate the framebuffer from bus-relative to physical address */
    AddressSpace = 0; /* MMIO space */
    if (!BootTranslateBusAddress(Interface,
                                 BusNumber,
                                 FrameBuffer,
                                 &AddressSpace,
                                 &TranslatedAddress))
    {
        DPRINT1("Could not translate framebuffer bus address 0x%I64X\n",
                FrameBuffer.QuadPart);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (AddressSpace == 0)
    {
        /* Calculate page-aligned address and size for MmMapIoSpace() */
        FrameBuffer.HighPart = TranslatedAddress.HighPart;
        FrameBuffer.LowPart  = ALIGN_DOWN_BY(TranslatedAddress.LowPart, PAGE_SIZE);
        MappedSize = FrameBufferSize;
        MappedSize += (ULONG)(TranslatedAddress.QuadPart - FrameBuffer.QuadPart);
        MappedSize = ROUND_TO_PAGES(MappedSize);

        FrameBufferBase = MmMapIoSpace(FrameBuffer, MappedSize, MmWriteCombined);
        if (!FrameBufferBase)
            FrameBufferBase = MmMapIoSpace(FrameBuffer, MappedSize, MmNonCached);
        if (!FrameBufferBase)
        {
            DPRINT1("Could not map framebuffer 0x%I64X (%lu bytes)\n",
                    FrameBuffer.QuadPart, MappedSize);
            return STATUS_NONE_MAPPED;
        }
        ScrFbBase = (PUCHAR)FrameBufferBase +
                    (TranslatedAddress.QuadPart - FrameBuffer.QuadPart);
    }
    else
    {
        /* The base is the translated address, no need to map */
        ScrFbBase = (PUCHAR)(ULONG_PTR)TranslatedAddress.QuadPart;
    }

    ScrFbWidth  = VideoConfigData.ScreenWidth;
    ScrFbHeight = VideoConfigData.ScreenHeight;
    ScrFbDelta  = VideoConfigData.PixelsPerScanLine * sizeof(ULONG);

    ScrFbInitializePalette(&VideoConfigData);

    DPRINT("Console framebuffer %lux%lu, %lu bytes per line\n",
           ScrFbWidth, ScrFbHeight, ScrFbDelta);
    return STATUS_SUCCESS;
}

/*
 * Framebuffer counterpart of the VGA ScrAcquireOwnership():
 * establishes the text-cell geometry of the display.
 */
VOID
ScrFbAcquireOwnership(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    if (!ScrFbBase)
    {
        /* No display: leave a zero geometry so that enabling fails */
        DeviceExtension->Columns = 0;
        DeviceExtension->Rows = 0;
        DeviceExtension->ScanLines = 1;
        return;
    }

    DeviceExtension->ScanLines = (DeviceExtension->FontBitfield
                                      ? FB_LOADED_FONT_HEIGHT
                                      : FB_DEFAULT_FONT_HEIGHT);
    DeviceExtension->Columns = (USHORT)(ScrFbWidth / FB_FONT_WIDTH);
    DeviceExtension->Rows = (USHORT)(ScrFbHeight / DeviceExtension->ScanLines);
    DeviceExtension->CursorX = 0;
    DeviceExtension->CursorY = 0;
    ScrFbLastCursor = 0;
}

/*
 * Allocates the text-cell shadow buffer that stands in for the
 * VGA text video memory, and clears the screen.
 */
PUCHAR
ScrFbAllocVideoMemory(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    PUSHORT Cell;
    ULONG Count, i;

    if (!ScrFbBase)
        return NULL;

    ASSERT(DeviceExtension->VideoMemorySize ==
           (SIZE_T)DeviceExtension->Rows * DeviceExtension->Columns * 2);

    /* Must be non-paged: snapshots are taken at high IRQL on display switch */
    Cell = ExAllocatePoolWithTag(NonPagedPool,
                                 DeviceExtension->VideoMemorySize,
                                 TAG_BLUE);
    if (!Cell)
        return NULL;

    Count = (ULONG)DeviceExtension->Rows * DeviceExtension->Columns;
    for (i = 0; i < Count; i++)
    {
        Cell[i] = ' ' | (DeviceExtension->CharAttribute << 8);
    }

    DeviceExtension->VideoMemory = (PUCHAR)Cell;
    ScrFbRenderAll(DeviceExtension);

    return (PUCHAR)Cell;
}

VOID
ScrFbFreeVideoMemory(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    ExFreePoolWithTag(DeviceExtension->VideoMemory, TAG_BLUE);
}

/* Renders a range of text cells from the shadow buffer to the framebuffer */
VOID
ScrFbRenderRange(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ ULONG First,
    _In_ ULONG Count)
{
    ULONG Total = (ULONG)DeviceExtension->Rows * DeviceExtension->Columns;
    ULONG CursorCell;
    ULONG i;

    if (!ScrFbBase || !DeviceExtension->VideoMemory || (Total == 0))
        return;

    if (First >= Total)
        return;
    Count = min(Count, Total - First);

    CursorCell = (DeviceExtension->CursorVisible
                      ? (ULONG)DeviceExtension->CursorX +
                        (ULONG)DeviceExtension->CursorY * DeviceExtension->Columns
                      : (ULONG)-1);

    for (i = First; i < First + Count; i++)
    {
        ScrFbRenderCell(DeviceExtension, i, (i == CursorCell));
    }
}

VOID
ScrFbRenderAll(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    ScrFbRenderRange(DeviceExtension,
                     0,
                     (ULONG)DeviceExtension->Rows * DeviceExtension->Columns);
}

/*
 * Software cursor: re-renders the cell that loses the cursor
 * overlay and the cell that gains it.
 */
VOID
ScrFbSetCursor(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    ULONG Old = ScrFbLastCursor;
    ULONG New = (ULONG)DeviceExtension->CursorX +
                (ULONG)DeviceExtension->CursorY * DeviceExtension->Columns;

    ScrFbLastCursor = New;
    if (Old != New)
        ScrFbRenderRange(DeviceExtension, Old, 1);
    ScrFbRenderRange(DeviceExtension, New, 1);
}

VOID
ScrFbSetCursorShape(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    /* Only visibility matters for the software cursor */
    ScrFbRenderRange(DeviceExtension, ScrFbLastCursor, 1);
    ScrFbSetCursor(DeviceExtension);
}

/* EOF */
