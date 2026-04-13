/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hardware detection routines
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <uefildr.h>
#include "../vidfb.h"

#include <debug.h>
DBG_DEFAULT_CHANNEL(HWDETECT);

/* GLOBALS *******************************************************************/

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

/* From uefivid.c */
extern ULONG_PTR VramAddress;
extern ULONG VramSize;
extern PCM_FRAMEBUF_DEVICE_DATA FrameBufferData;

BOOLEAN AcpiPresent = FALSE;

/**
 * @brief Register an ISA bus in the hardware configuration tree.
 *
 * ScsiPort and other drivers use IoQueryDeviceDescription with
 * InterfaceType=Isa to verify the bus exists before scanning.
 */
static
VOID
DetectIsaBus(
    _In_ PCONFIGURATION_COMPONENT_DATA SystemKey,
    _Inout_ PULONG BusNumber)
{
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCONFIGURATION_COMPONENT_DATA BusKey;
    ULONG Size;

    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (!PartialResourceList)
        return;

    RtlZeroMemory(PartialResourceList, Size);
    PartialResourceList->Version = 1;
    PartialResourceList->Revision = 1;
    PartialResourceList->Count = 0;

    FldrCreateComponentKey(SystemKey,
                           AdapterClass,
                           MultiFunctionAdapter,
                           0, 0, 0xFFFFFFFF,
                           "ISA",
                           PartialResourceList,
                           Size,
                           &BusKey);
    (*BusNumber)++;
}

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/**
 * @brief Detect PCI bus presence by probing config space mechanism 1
 * and register the PCI bus in the hardware configuration tree.
 *
 * This replaces the BIOS INT 1Ah PCI detection for UEFI boots.
 * PCI config mechanism 1 (I/O ports 0xCF8/0xCFC) is architecture-defined
 * and works identically on BIOS and UEFI.
 */
static
VOID
DetectPci(
    _In_ PCONFIGURATION_COMPONENT_DATA SystemKey,
    _Inout_ PULONG BusNumber)
{
    PCI_REGISTRY_INFO BusData;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PCONFIGURATION_COMPONENT_DATA BiosKey;
    PCONFIGURATION_COMPONENT_DATA BusKey;
    ULONG Size;
    ULONG OldValue;
    ULONG TestValue;
    ULONG i;

    /*
     * Probe PCI configuration mechanism 1:
     * Save old value at 0xCF8, write a known pattern, read back.
     */
    OldValue = __indword(PCI_CONFIG_ADDRESS);
    __outdword(PCI_CONFIG_ADDRESS, 0x80000000);
    TestValue = __indword(PCI_CONFIG_ADDRESS);
    __outdword(PCI_CONFIG_ADDRESS, OldValue);

    if (TestValue != 0x80000000)
    {
        TRACE("PCI mechanism 1 not detected (got 0x%lx)\n", TestValue);
        return;
    }

    /* Read vendor ID at bus 0, device 0, function 0 to confirm PCI works */
    __outdword(PCI_CONFIG_ADDRESS, 0x80000000);
    TestValue = __indword(PCI_CONFIG_DATA);
    if ((TestValue & 0xFFFF) == 0xFFFF)
    {
        TRACE("No PCI device at 0:0.0\n");
        return;
    }

    TRACE("PCI mechanism 1 detected, host bridge vendor:device = %04lx:%04lx\n",
          TestValue & 0xFFFF, (TestValue >> 16) & 0xFFFF);

    /* Count PCI buses by scanning bus numbers for valid devices */
    BusData.NoBuses = 1;
    for (i = 1; i < 256; i++)
    {
        __outdword(PCI_CONFIG_ADDRESS, 0x80000000 | (i << 16));
        TestValue = __indword(PCI_CONFIG_DATA);
        if ((TestValue & 0xFFFF) != 0xFFFF)
            BusData.NoBuses = (UCHAR)(i + 1);
    }

    BusData.MajorRevision = 2;
    BusData.MinorRevision = 0;
    BusData.HardwareMechanism = 1;

    TRACE("Found %u PCI bus(es)\n", (ULONG)BusData.NoBuses);

    /* Create the "PCI BIOS" MultiFunctionAdapter key (same as BIOS path) */
    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (!PartialResourceList)
        return;

    RtlZeroMemory(PartialResourceList, Size);

    FldrCreateComponentKey(SystemKey,
                           AdapterClass,
                           MultiFunctionAdapter,
                           0,
                           0,
                           0xFFFFFFFF,
                           "PCI BIOS",
                           PartialResourceList,
                           Size,
                           &BiosKey);
    (*BusNumber)++;

    /* Report PCI buses */
    for (i = 0; i < (ULONG)BusData.NoBuses; i++)
    {
        if (i == 0)
        {
            /* First bus gets the PCI_REGISTRY_INFO data */
            Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[1]) +
                   sizeof(BusData);
            PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
            if (!PartialResourceList)
                return;

            RtlZeroMemory(PartialResourceList, Size);
            PartialResourceList->Version = 1;
            PartialResourceList->Revision = 1;
            PartialResourceList->Count = 1;

            PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
            PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
            PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
            PartialDescriptor->u.DeviceSpecificData.DataSize = sizeof(BusData);

            RtlCopyMemory(&PartialResourceList->PartialDescriptors[1],
                          &BusData, sizeof(BusData));
        }
        else
        {
            Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
            PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
            if (!PartialResourceList)
                return;

            RtlZeroMemory(PartialResourceList, Size);
        }

        FldrCreateComponentKey(SystemKey,
                               AdapterClass,
                               MultiFunctionAdapter,
                               0,
                               0,
                               0xFFFFFFFF,
                               "PCI",
                               PartialResourceList,
                               Size,
                               &BusKey);
        (*BusNumber)++;
    }
}
static EFI_EVENT IdleTimerEvent = NULL;

/* FUNCTIONS *****************************************************************/

VOID
StallExecutionProcessor(ULONG Microseconds)
{
    GlobalSystemTable->BootServices->Stall(Microseconds);
}

VOID
UefiHwIdle(VOID)
{
    UINTN Index;
    EFI_STATUS Status;
    EFI_BOOT_SERVICES *BootServices = GlobalSystemTable->BootServices;

    /* Keep one timer event around and arm it each idle tick */
    if (IdleTimerEvent == NULL)
    {
        Status = BootServices->CreateEvent(EVT_TIMER,
                                           TPL_APPLICATION,
                                           NULL,
                                           NULL,
                                           &IdleTimerEvent);
        if (EFI_ERROR(Status))
        {
            StallExecutionProcessor(10000); /* 10 ms fallback */
            return;
        }
    }

    /* Set a 10ms (100,000 * 100ns) relative timer */
    Status = BootServices->SetTimer(IdleTimerEvent, TimerRelative, 100000);
    if (!EFI_ERROR(Status))
        Status = BootServices->WaitForEvent(1, &IdleTimerEvent, &Index);
    if (EFI_ERROR(Status))
        StallExecutionProcessor(10000); /* 10 ms fallback */
}

BOOLEAN IsAcpiPresent(VOID)
{
    return AcpiPresent;
}

static
PRSDP_DESCRIPTOR
FindAcpiBios(VOID)
{
    UINTN i;
    RSDP_DESCRIPTOR* rsdp = NULL;
    EFI_GUID acpi2_guid = EFI_ACPI_20_TABLE_GUID;

    for (i = 0; i < GlobalSystemTable->NumberOfTableEntries; i++)
    {
        if (!memcmp(&GlobalSystemTable->ConfigurationTable[i].VendorGuid,
                    &acpi2_guid, sizeof(acpi2_guid)))
        {
            rsdp = (RSDP_DESCRIPTOR*)GlobalSystemTable->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    return rsdp;
}

PDESCRIPTION_HEADER
UefiFindAcpiTable(
    _In_ ULONG Signature)
{
    UINTN Index, Count;
    PRSDP_DESCRIPTOR Rsdp;

    Rsdp = FindAcpiBios();
    if (Rsdp == NULL)
        return NULL;

    if ((Rsdp->revision > 0) && (Rsdp->xsdt_physical_address != 0))
    {
        PXSDT Xsdt = (PXSDT)(ULONG_PTR)Rsdp->xsdt_physical_address;

        if ((Xsdt != NULL) && (Xsdt->Header.Length >= sizeof(Xsdt->Header)))
        {
            Count = (Xsdt->Header.Length - sizeof(Xsdt->Header)) / sizeof(Xsdt->Tables[0]);
            for (Index = 0; Index < Count; ++Index)
            {
                PDESCRIPTION_HEADER Header =
                    (PDESCRIPTION_HEADER)(ULONG_PTR)Xsdt->Tables[Index].QuadPart;

                if ((Header != NULL) && (Header->Signature == Signature))
                    return Header;
            }
        }
    }

    if (Rsdp->rsdt_physical_address != 0)
    {
        PRSDT Rsdt = (PRSDT)(ULONG_PTR)Rsdp->rsdt_physical_address;

        if ((Rsdt != NULL) && (Rsdt->Header.Length >= sizeof(Rsdt->Header)))
        {
            Count = (Rsdt->Header.Length - sizeof(Rsdt->Header)) / sizeof(Rsdt->Tables[0]);
            for (Index = 0; Index < Count; ++Index)
            {
                PDESCRIPTION_HEADER Header =
                    (PDESCRIPTION_HEADER)(ULONG_PTR)Rsdt->Tables[Index];

                if ((Header != NULL) && (Header->Signature == Signature))
                    return Header;
            }
        }
    }

    return NULL;
}

VOID
DetectAcpiBios(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCONFIGURATION_COMPONENT_DATA BiosKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PRSDP_DESCRIPTOR Rsdp;
    PACPI_BIOS_DATA AcpiBiosData;
    ULONG TableSize, Size;

    Rsdp = FindAcpiBios();

    if (Rsdp)
    {
        /* Set up the flag in the loader block */
        AcpiPresent = TRUE;

        /* Calculate the table size */
        TableSize = sizeof(ACPI_BIOS_DATA);

        /* Set 'Configuration Data' value */
        Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[1]) + TableSize;
        PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
        if (PartialResourceList == NULL)
        {
            ERR("Failed to allocate resource descriptor\n");
            return;
        }

        RtlZeroMemory(PartialResourceList, Size);
        PartialResourceList->Version = 0;
        PartialResourceList->Revision = 0;
        PartialResourceList->Count = 1;

        PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
        PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
        PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
        PartialDescriptor->u.DeviceSpecificData.DataSize = TableSize;

        /* Fill the table */
        AcpiBiosData = (PACPI_BIOS_DATA)(PartialDescriptor + 1);

        if (Rsdp->revision > 0)
        {
            TRACE("ACPI >1.0, using XSDT address\n");
            AcpiBiosData->RSDTAddress.QuadPart = Rsdp->xsdt_physical_address;
        }
        else
        {
            TRACE("ACPI 1.0, using RSDT address\n");
            AcpiBiosData->RSDTAddress.LowPart = Rsdp->rsdt_physical_address;
        }

        AcpiBiosData->Count = 0;

        TRACE("RSDT %p, data size %x\n", Rsdp->rsdt_physical_address, TableSize);

        /* Create new bus key */
        FldrCreateComponentKey(SystemKey,
                               AdapterClass,
                               MultiFunctionAdapter,
                               0x0,
                               0x0,
                               0xFFFFFFFF,
                               "ACPI BIOS",
                               PartialResourceList,
                               Size,
                               &BiosKey);

        /* Increment bus number */
        (*BusNumber)++;
    }
}

static VOID
DetectDisplayController(
    _In_ PCONFIGURATION_COMPONENT_DATA BusKey)
{
    PCONFIGURATION_COMPONENT_DATA ControllerKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PCM_FRAMEBUF_DEVICE_DATA FramebufData;
    ULONG Size;

    if (!VramAddress || (VramSize == 0) || !FrameBufferData)
        return;

    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[2]) + sizeof(*FramebufData);
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (PartialResourceList == NULL)
    {
        ERR("Failed to allocate resource descriptor\n");
        return;
    }

    /* Initialize resource descriptor */
    RtlZeroMemory(PartialResourceList, Size);
    PartialResourceList->Version  = 1;
    PartialResourceList->Revision = 2;
    PartialResourceList->Count = 2;

    /* Set Memory */
    PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
    PartialDescriptor->Type = CmResourceTypeMemory;
    PartialDescriptor->ShareDisposition = CmResourceShareDeviceExclusive;
    PartialDescriptor->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
    PartialDescriptor->u.Memory.Start.QuadPart = VramAddress;
    PartialDescriptor->u.Memory.Length = VramSize;

    /* Set framebuffer-specific data */
    PartialDescriptor = &PartialResourceList->PartialDescriptors[1];
    PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
    PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
    PartialDescriptor->Flags = 0;
    PartialDescriptor->u.DeviceSpecificData.DataSize = sizeof(*FramebufData);

    /* Get pointer to framebuffer-specific data */
    FramebufData = (PCM_FRAMEBUF_DEVICE_DATA)(PartialDescriptor + 1);
    RtlCopyMemory(FramebufData, FrameBufferData, sizeof(*FrameBufferData));
    FramebufData->Version  = 1;
    FramebufData->Revision = 3;
    FramebufData->VideoClock = 0; // FIXME: Use EDID

    FldrCreateComponentKey(BusKey,
                           ControllerClass,
                           DisplayController,
                           Output | ConsoleOut,
                           0,
                           0xFFFFFFFF,
                           "UEFI GOP Framebuffer",
                           PartialResourceList,
                           Size,
                           &ControllerKey);

    // NOTE: Don't add a MonitorPeripheral for now.
    // We should use EDID data for it.
}

static
VOID
DetectInternal(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCONFIGURATION_COMPONENT_DATA BusKey;
    ULONG Size;

    /* Set 'Configuration Data' value */
    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (PartialResourceList == NULL)
    {
        ERR("Failed to allocate resource descriptor\n");
        return;
    }

    /* Initialize resource descriptor */
    RtlZeroMemory(PartialResourceList, Size);
    PartialResourceList->Version  = 1;
    PartialResourceList->Revision = 1;
    PartialResourceList->Count = 0;

    /* Create new bus key */
    FldrCreateComponentKey(SystemKey,
                           AdapterClass,
                           MultiFunctionAdapter,
                           0,
                           0,
                           0xFFFFFFFF,
                           "UEFI Internal",
                           PartialResourceList,
                           Size,
                           &BusKey);

    /* Increment bus number */
    (*BusNumber)++;

    /* Detect devices that do not belong to "standard" buses */
    DetectDisplayController(BusKey);

    /* FIXME: Detect more devices */
}

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(
    _In_opt_ PCSTR Options)
{
    PCONFIGURATION_COMPONENT_DATA SystemKey;
    ULONG BusNumber = 0;

    TRACE("DetectHardware()\n");

    /* Create the 'System' key */
#if defined(_M_IX86) || defined(_M_AMD64)
    FldrCreateSystemKey(&SystemKey, "AT/AT COMPATIBLE");
#elif defined(_M_IA64)
    FldrCreateSystemKey(&SystemKey, "Intel Itanium processor family");
#elif defined(_M_ARM) || defined(_M_ARM64)
    FldrCreateSystemKey(&SystemKey, "ARM processor family");
#else
    #error Please define a system key for your architecture
#endif

    /* Detect buses */
    DetectInternal(SystemKey, &BusNumber);
    DetectPci(SystemKey, &BusNumber);
    DetectIsaBus(SystemKey, &BusNumber);
    DetectAcpiBios(SystemKey, &BusNumber);

    /* TODO: Detect ISA sub-devices (serial ports, keyboard, etc.) */

    TRACE("DetectHardware() Done\n");
    return SystemKey;
}
