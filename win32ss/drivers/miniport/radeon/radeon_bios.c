/*
 * PROJECT:     ReactOS AMD Radeon ATOM-BIOS Framebuffer Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     VBIOS acquisition; a port of the strategy ladder in Linux
 *              v6.6 drivers/gpu/drm/radeon/radeon_bios.c, adapted to what a
 *              videoport miniport can reach:
 *                1. ACPI VFCT table (the only source on pure-UEFI boots for
 *                   IGPs; located by walking RSDP->RSDT/XSDT from physical
 *                   memory because ReactOS implements neither the ACPI
 *                   firmware-table provider of NtQuerySystemInformation nor
 *                   an AuxKlib equivalent).
 *                2. Start of VRAM (IGP case: the system BIOS copies the
 *                   integrated ROM there on POST).
 *                3. PCI ROM BAR (discrete cards).
 *                4. Physical 0xC0000 shadow (the adapter that owns the boot
 *                   display on CSM/BIOS boots), gated on a PCIR vendor
 *                   match so a foreign primary VGA ROM is never mistaken
 *                   for ours.
 *              Note: Int10/VideoPortGetRomImage are useless here - Int10 is
 *              stubbed on amd64 and GetRomImage only reaches the 0xC0000
 *              shadow, which case 4 covers with validation.
 * COPYRIGHT:   Copyright 2008 Advanced Micro Devices, Inc.
 *              Copyright 2008 Red Hat Inc.
 *              Copyright 2009 Jerome Glisse.
 *              Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "radeon.h"

#define RADEON_VBIOS_MAX_SIZE   (256 * 1024)
#define RADEON_SHADOW_BASE      0xC0000
#define RADEON_SHADOW_SIZE      (128 * 1024)

/* Minimal ACPI structures (ACPI spec layout) */
#include <pshpack1.h>
typedef struct _RADEON_ACPI_RSDP
{
    CHAR Signature[8];          /* "RSD PTR " */
    UCHAR Checksum;
    CHAR OemId[6];
    UCHAR Revision;
    ULONG RsdtAddress;
    /* ACPI 2.0+ */
    ULONG Length;
    ULONGLONG XsdtAddress;
    UCHAR ExtendedChecksum;
    UCHAR Reserved[3];
} RADEON_ACPI_RSDP, *PRADEON_ACPI_RSDP;

typedef struct _RADEON_ACPI_SDT_HEADER
{
    CHAR Signature[4];
    ULONG Length;
    UCHAR Revision;
    UCHAR Checksum;
    CHAR OemId[6];
    CHAR OemTableId[8];
    ULONG OemRevision;
    ULONG CreatorId;
    ULONG CreatorRevision;
} RADEON_ACPI_SDT_HEADER, *PRADEON_ACPI_SDT_HEADER;
#include <poppack.h>

/* MAPPING HELPERS ***********************************************************/

static PVOID
RadeonMapPhysical(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONGLONG PhysicalAddress,
    _In_ ULONG Length)
{
    PHYSICAL_ADDRESS Address;
    ULONG InIoSpace = VIDEO_MEMORY_SPACE_MEMORY;
    PVOID Mapped = NULL;
    ULONG MappedLength = Length;

    Address.QuadPart = (LONGLONG)PhysicalAddress;
    if (VideoPortMapMemory(DeviceExtension,
                           Address,
                           &MappedLength,
                           &InIoSpace,
                           &Mapped) != NO_ERROR)
    {
        return NULL;
    }
    return Mapped;
}

static VOID
RadeonUnmapPhysical(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PVOID Mapped)
{
    if (Mapped != NULL)
        VideoPortUnmapMemory(DeviceExtension, Mapped, NULL);
}

static VOID
RadeonCopyFromMapped(
    _Out_writes_bytes_(Length) PVOID Destination,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ ULONG Length)
{
    /* Device/firmware memory: plain byte copy (READ_REGISTER granularity
     * is not required for these RAM-backed regions). */
    RtlCopyMemory(Destination, Source, Length);
}

/* ACPI VFCT *****************************************************************/

static UCHAR
RadeonAcpiChecksum(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    UCHAR Sum = 0;
    ULONG i;

    for (i = 0; i < Length; i++)
        Sum = (UCHAR)(Sum + Data[i]);
    return Sum;
}

static BOOLEAN
RadeonScanRangeForRsdp(
    _In_reads_bytes_(Length) const UCHAR *Base,
    _In_ ULONG Length,
    _Out_ RADEON_ACPI_RSDP *Rsdp)
{
    ULONG Offset;

    for (Offset = 0; Offset + 20 <= Length; Offset += 16)
    {
        const UCHAR *Candidate = Base + Offset;

        if (RtlCompareMemory(Candidate, "RSD PTR ", 8) != 8)
            continue;
        if (RadeonAcpiChecksum(Candidate, 20) != 0)
            continue;

        RtlZeroMemory(Rsdp, sizeof(*Rsdp));
        RadeonCopyFromMapped(Rsdp, Candidate, 20);
        if (Rsdp->Revision >= 2 && Offset + 36 <= Length)
        {
            if (RadeonAcpiChecksum(Candidate, 36) == 0)
                RadeonCopyFromMapped(Rsdp, Candidate, 36);
            else
                Rsdp->Revision = 0;     /* fall back to the RSDT */
        }
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN
RadeonFindRsdp(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _Out_ RADEON_ACPI_RSDP *Rsdp)
{
    PUCHAR Mapped;
    BOOLEAN Found = FALSE;
    USHORT EbdaSegment = 0;

    /* EBDA pointer lives at BDA 0x40E */
    Mapped = RadeonMapPhysical(DeviceExtension, 0x400, 0x100);
    if (Mapped != NULL)
    {
        EbdaSegment = *(USHORT UNALIGNED *)(Mapped + 0xE);
        RadeonUnmapPhysical(DeviceExtension, Mapped);
    }

    if (EbdaSegment != 0)
    {
        ULONG Ebda = (ULONG)EbdaSegment << 4;

        if (Ebda >= 0x80000 && Ebda < 0xA0000)
        {
            Mapped = RadeonMapPhysical(DeviceExtension, Ebda, 1024);
            if (Mapped != NULL)
            {
                Found = RadeonScanRangeForRsdp(Mapped, 1024, Rsdp);
                RadeonUnmapPhysical(DeviceExtension, Mapped);
            }
        }
    }

    if (!Found)
    {
        Mapped = RadeonMapPhysical(DeviceExtension, 0xE0000, 0x20000);
        if (Mapped != NULL)
        {
            Found = RadeonScanRangeForRsdp(Mapped, 0x20000, Rsdp);
            RadeonUnmapPhysical(DeviceExtension, Mapped);
        }
    }

    return Found;
}

/*
 * Locate an ACPI table by signature by walking the RSDT (or XSDT when the
 * RSDP is rev >= 2).  Returns the table's physical address and length.
 */
static BOOLEAN
RadeonFindAcpiTable(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_reads_(4) const CHAR *Signature,
    _Out_ PULONGLONG TablePhysical,
    _Out_ PULONG TableLength)
{
    RADEON_ACPI_RSDP Rsdp;
    RADEON_ACPI_SDT_HEADER Header;
    PUCHAR Mapped;
    ULONGLONG RootPhysical;
    ULONG RootLength;
    ULONG EntrySize;
    ULONG EntryCount;
    ULONG i;
    BOOLEAN Found = FALSE;

    if (!RadeonFindRsdp(DeviceExtension, &Rsdp))
    {
        VideoPortDebugPrint(Trace,
            "radeonfb: no RSDP found (UEFI boot without low-memory "
            "mirror?)\n");
        return FALSE;
    }

    if (Rsdp.Revision >= 2 && Rsdp.XsdtAddress != 0)
    {
        RootPhysical = Rsdp.XsdtAddress;
        EntrySize = 8;
    }
    else
    {
        RootPhysical = Rsdp.RsdtAddress;
        EntrySize = 4;
    }
    if (RootPhysical == 0)
        return FALSE;

    Mapped = RadeonMapPhysical(DeviceExtension, RootPhysical, sizeof(Header));
    if (Mapped == NULL)
        return FALSE;
    RadeonCopyFromMapped(&Header, Mapped, sizeof(Header));
    RadeonUnmapPhysical(DeviceExtension, Mapped);

    RootLength = Header.Length;
    if (RootLength < sizeof(Header) || RootLength > 0x10000)
        return FALSE;

    Mapped = RadeonMapPhysical(DeviceExtension, RootPhysical, RootLength);
    if (Mapped == NULL)
        return FALSE;

    EntryCount = (RootLength - sizeof(Header)) / EntrySize;
    for (i = 0; i < EntryCount && !Found; i++)
    {
        ULONGLONG EntryPhysical;
        PUCHAR EntryMapped;

        if (EntrySize == 8)
            EntryPhysical = *(ULONGLONG UNALIGNED *)
                (Mapped + sizeof(Header) + i * 8);
        else
            EntryPhysical = *(ULONG UNALIGNED *)
                (Mapped + sizeof(Header) + i * 4);
        if (EntryPhysical == 0)
            continue;

        EntryMapped = RadeonMapPhysical(DeviceExtension, EntryPhysical,
                                        sizeof(RADEON_ACPI_SDT_HEADER));
        if (EntryMapped == NULL)
            continue;
        RadeonCopyFromMapped(&Header, EntryMapped, sizeof(Header));
        RadeonUnmapPhysical(DeviceExtension, EntryMapped);

        if (RtlCompareMemory(Header.Signature, Signature, 4) == 4 &&
            Header.Length >= sizeof(Header))
        {
            *TablePhysical = EntryPhysical;
            *TableLength = Header.Length;
            Found = TRUE;
        }
    }

    RadeonUnmapPhysical(DeviceExtension, Mapped);
    return Found;
}

/* Port of radeon_acpi_vfct_bios (radeon_bios.c); VFCT structure layout
 * comes from atombios.h (UEFI_ACPI_VFCT/VFCT_IMAGE_HEADER). */
static BOOLEAN
RadeonAcpiVfctBios(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    ULONGLONG TablePhysical;
    ULONG TableLength;
    PUCHAR Mapped;
    UEFI_ACPI_VFCT Vfct;
    ULONG Offset;
    PCI_SLOT_NUMBER Slot;
    BOOLEAN Result = FALSE;

    if (!RadeonFindAcpiTable(DeviceExtension, "VFCT",
                             &TablePhysical, &TableLength))
        return FALSE;

    if (TableLength < sizeof(UEFI_ACPI_VFCT))
    {
        VideoPortDebugPrint(Error,
            "radeonfb: ACPI VFCT table present but broken (too short #1)\n");
        return FALSE;
    }

    Mapped = RadeonMapPhysical(DeviceExtension, TablePhysical, TableLength);
    if (Mapped == NULL)
        return FALSE;

    RadeonCopyFromMapped(&Vfct, Mapped, sizeof(Vfct));
    Offset = Vfct.VBIOSImageOffset;
    Slot.u.AsULONG = DeviceExtension->PciSlot;

    while (Offset < TableLength)
    {
        VFCT_IMAGE_HEADER VbiosHeader;

        if (Offset + sizeof(VFCT_IMAGE_HEADER) > TableLength)
        {
            VideoPortDebugPrint(Error,
                "radeonfb: ACPI VFCT image header truncated\n");
            break;
        }
        RadeonCopyFromMapped(&VbiosHeader, Mapped + Offset,
                             sizeof(VbiosHeader));
        Offset += sizeof(VFCT_IMAGE_HEADER);

        if (Offset + VbiosHeader.ImageLength > TableLength)
        {
            VideoPortDebugPrint(Error,
                "radeonfb: ACPI VFCT image truncated\n");
            break;
        }

        if (VbiosHeader.ImageLength != 0 &&
            VbiosHeader.PCIBus == DeviceExtension->PciBus &&
            VbiosHeader.PCIDevice == Slot.u.bits.DeviceNumber &&
            VbiosHeader.PCIFunction == Slot.u.bits.FunctionNumber &&
            VbiosHeader.VendorID == DeviceExtension->VendorId &&
            VbiosHeader.DeviceID == DeviceExtension->DeviceId)
        {
            PUCHAR Bios = ExAllocatePoolWithTag(NonPagedPool,
                                                VbiosHeader.ImageLength,
                                                RADEONFB_TAG);
            if (Bios != NULL)
            {
                RadeonCopyFromMapped(Bios, Mapped + Offset,
                                     VbiosHeader.ImageLength);
                DeviceExtension->Bios = Bios;
                DeviceExtension->BiosLength = VbiosHeader.ImageLength;
                Result = TRUE;
                VideoPortDebugPrint(Info,
                    "radeonfb: VBIOS from ACPI VFCT (%lu bytes)\n",
                    VbiosHeader.ImageLength);
            }
            break;
        }

        Offset += VbiosHeader.ImageLength;
    }

    RadeonUnmapPhysical(DeviceExtension, Mapped);
    return Result;
}

/* VRAM SHADOW (IGP) *********************************************************/

/* Port of igp_read_bios_from_vram: on IGPs the system BIOS places a copy of
 * the integrated ROM at the start of VRAM. */
static BOOLEAN
RadeonIgpVramBios(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    PUCHAR Mapped;
    PUCHAR Bios;
    ULONG Size = RADEON_VBIOS_MAX_SIZE;

    if ((DeviceExtension->ChipFlags & RADEONFB_IS_IGP) == 0)
        return FALSE;

    if (DeviceExtension->FbBarLength != 0 &&
        Size > DeviceExtension->FbBarLength)
    {
        Size = DeviceExtension->FbBarLength;
    }

    Mapped = RadeonMapPhysical(DeviceExtension,
                               (ULONGLONG)DeviceExtension->FbPhysical.QuadPart,
                               Size);
    if (Mapped == NULL)
        return FALSE;

    if (Size < 2 || Mapped[0] != 0x55 || Mapped[1] != 0xAA)
    {
        RadeonUnmapPhysical(DeviceExtension, Mapped);
        return FALSE;
    }

    Bios = ExAllocatePoolWithTag(NonPagedPool, Size, RADEONFB_TAG);
    if (Bios == NULL)
    {
        RadeonUnmapPhysical(DeviceExtension, Mapped);
        return FALSE;
    }

    RadeonCopyFromMapped(Bios, Mapped, Size);
    RadeonUnmapPhysical(DeviceExtension, Mapped);

    DeviceExtension->Bios = Bios;
    DeviceExtension->BiosLength = Size;
    VideoPortDebugPrint(Info,
        "radeonfb: VBIOS from IGP VRAM shadow (%lu bytes)\n", Size);
    return TRUE;
}

/* PCI ROM BAR ***************************************************************/

/* Port of radeon_read_bios (pci_map_rom path): enable ROM decode, copy the
 * image, restore the previous decode state. */
static BOOLEAN
RadeonRomBarBios(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    ULONG RomBar = 0;
    ULONG RomEnabled;
    ULONG RomBase;
    ULONG RomLength;
    PUCHAR Mapped;
    PUCHAR Bios = NULL;
    BOOLEAN Result = FALSE;

    if (VideoPortGetBusData(DeviceExtension,
                            PCIConfiguration,
                            DeviceExtension->PciSlot,
                            &RomBar,
                            FIELD_OFFSET(PCI_COMMON_CONFIG,
                                         u.type0.ROMBaseAddress),
                            sizeof(RomBar)) != sizeof(RomBar))
    {
        return FALSE;
    }

    RomBase = RomBar & PCI_ADDRESS_ROM_ADDRESS_MASK;
    if (RomBase == 0)
        return FALSE;    /* firmware assigned no ROM window */

    /* Enable ROM decode */
    RomEnabled = RomBar | PCI_ROMADDRESS_ENABLED;
    VideoPortSetBusData(DeviceExtension,
                        PCIConfiguration,
                        DeviceExtension->PciSlot,
                        &RomEnabled,
                        FIELD_OFFSET(PCI_COMMON_CONFIG,
                                     u.type0.ROMBaseAddress),
                        sizeof(RomEnabled));

    Mapped = RadeonMapPhysical(DeviceExtension, RomBase, 0x1000);
    if (Mapped != NULL)
    {
        if (Mapped[0] == 0x55 && Mapped[1] == 0xAA)
        {
            RomLength = (ULONG)Mapped[2] * 512;
            if (RomLength > RADEON_VBIOS_MAX_SIZE)
                RomLength = RADEON_VBIOS_MAX_SIZE;
            if (RomLength != 0)
            {
                RadeonUnmapPhysical(DeviceExtension, Mapped);
                Mapped = RadeonMapPhysical(DeviceExtension, RomBase, RomLength);
                if (Mapped != NULL)
                {
                    Bios = ExAllocatePoolWithTag(NonPagedPool, RomLength,
                                                 RADEONFB_TAG);
                    if (Bios != NULL)
                    {
                        RadeonCopyFromMapped(Bios, Mapped, RomLength);
                        DeviceExtension->Bios = Bios;
                        DeviceExtension->BiosLength = RomLength;
                        Result = TRUE;
                        VideoPortDebugPrint(Info,
                            "radeonfb: VBIOS from PCI ROM BAR (%lu bytes)\n",
                            RomLength);
                    }
                }
            }
        }
        RadeonUnmapPhysical(DeviceExtension, Mapped);
    }

    /* Restore the previous ROM decode state */
    VideoPortSetBusData(DeviceExtension,
                        PCIConfiguration,
                        DeviceExtension->PciSlot,
                        &RomBar,
                        FIELD_OFFSET(PCI_COMMON_CONFIG,
                                     u.type0.ROMBaseAddress),
                        sizeof(RomBar));

    return Result;
}

/* LEGACY 0xC0000 SHADOW *****************************************************/

/* Port of radeon_read_platform_bios, hardened with a PCI data structure
 * vendor check: the 0xC0000 shadow belongs to whichever adapter was the
 * boot display, which is not necessarily this one. */
static BOOLEAN
RadeonShadowBios(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    PUCHAR Mapped;
    PUCHAR Bios;
    ULONG RomLength;
    USHORT PcirOffset;
    USHORT PcirVendor;
    BOOLEAN Result = FALSE;

    Mapped = RadeonMapPhysical(DeviceExtension, RADEON_SHADOW_BASE,
                               RADEON_SHADOW_SIZE);
    if (Mapped == NULL)
        return FALSE;

    if (Mapped[0] != 0x55 || Mapped[1] != 0xAA)
        goto Done;

    RomLength = (ULONG)Mapped[2] * 512;
    if (RomLength == 0 || RomLength > RADEON_SHADOW_SIZE)
        goto Done;

    /* PCI data structure pointer at 0x18; vendor id at PCIR+4 */
    PcirOffset = *(USHORT UNALIGNED *)(Mapped + 0x18);
    if (PcirOffset == 0 || (ULONG)PcirOffset + 8 > RomLength)
        goto Done;
    if (RtlCompareMemory(Mapped + PcirOffset, "PCIR", 4) != 4)
        goto Done;
    PcirVendor = *(USHORT UNALIGNED *)(Mapped + PcirOffset + 4);
    if (PcirVendor != DeviceExtension->VendorId)
    {
        VideoPortDebugPrint(Trace,
            "radeonfb: 0xC0000 shadow belongs to vendor 0x%04x, skipping\n",
            PcirVendor);
        goto Done;
    }

    Bios = ExAllocatePoolWithTag(NonPagedPool, RomLength, RADEONFB_TAG);
    if (Bios == NULL)
        goto Done;

    RadeonCopyFromMapped(Bios, Mapped, RomLength);
    DeviceExtension->Bios = Bios;
    DeviceExtension->BiosLength = RomLength;
    Result = TRUE;
    VideoPortDebugPrint(Info,
        "radeonfb: VBIOS from 0xC0000 shadow (%lu bytes)\n", RomLength);

Done:
    RadeonUnmapPhysical(DeviceExtension, Mapped);
    return Result;
}

/* LADDER ********************************************************************/

static USHORT
RadeonBios16(
    _In_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG Offset)
{
    if (Offset + 2 > DeviceExtension->BiosLength)
        return 0;
    return *(USHORT UNALIGNED *)(DeviceExtension->Bios + Offset);
}

BOOLEAN
RadeonGetBios(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    BOOLEAN Found;
    USHORT Tmp;
    USHORT HeaderStart;

    Found = RadeonAcpiVfctBios(DeviceExtension);
    if (!Found)
        Found = RadeonIgpVramBios(DeviceExtension);
    if (!Found)
        Found = RadeonRomBarBios(DeviceExtension);
    if (!Found)
        Found = RadeonShadowBios(DeviceExtension);
    if (!Found || DeviceExtension->Bios == NULL)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: unable to locate a VBIOS ROM\n");
        return FALSE;
    }

    /* Validation, port of the tail of radeon_get_bios() */
    if (DeviceExtension->BiosLength < 0x60 ||
        DeviceExtension->Bios[0] != 0x55 ||
        DeviceExtension->Bios[1] != 0xAA)
    {
        VideoPortDebugPrint(Error, "radeonfb: BIOS signature incorrect\n");
        goto FreeBios;
    }

    Tmp = RadeonBios16(DeviceExtension, 0x18);
    if (Tmp + 0x14u >= DeviceExtension->BiosLength ||
        DeviceExtension->Bios[Tmp + 0x14] != 0x0)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: not an x86 BIOS ROM, not using\n");
        goto FreeBios;
    }

    HeaderStart = RadeonBios16(DeviceExtension, ATOM_ROM_TABLE_PTR);
    if (HeaderStart == 0 ||
        (ULONG)HeaderStart + 8 >= DeviceExtension->BiosLength)
    {
        goto FreeBios;
    }
    if (memcmp(DeviceExtension->Bios + HeaderStart + 4, "ATOM", 4) != 0 &&
        memcmp(DeviceExtension->Bios + HeaderStart + 4, "MOTA", 4) != 0)
    {
        VideoPortDebugPrint(Error,
            "radeonfb: COM BIOS detected, only ATOM BIOS is supported\n");
        goto FreeBios;
    }

    return TRUE;

FreeBios:
    ExFreePoolWithTag(DeviceExtension->Bios, RADEONFB_TAG);
    DeviceExtension->Bios = NULL;
    DeviceExtension->BiosLength = 0;
    return FALSE;
}
