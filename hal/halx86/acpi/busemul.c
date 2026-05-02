/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            hal/halx86/acpi/busemul.c
 * PURPOSE:         ACPI HAL Bus Handler Emulation Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* PRIVATE FUNCTIONS **********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
HalpRegisterKdSupportFunctions(VOID)
{
    /* Register PCI Device Functions */
    KdSetupPciDeviceForDebugging = HalpSetupPciDeviceForDebugging;
    KdReleasePciDeviceforDebugging = HalpReleasePciDeviceForDebugging;

    /* Register memory functions */
#ifndef _MINIHAL_
#if (NTDDI_VERSION >= NTDDI_VISTASP1)
    KdMapPhysicalMemory64 = HalpMapPhysicalMemory64Vista;
    KdUnmapVirtualAddress = HalpUnmapVirtualAddressVista;
#else
    KdMapPhysicalMemory64 = HalpMapPhysicalMemory64;
    KdUnmapVirtualAddress = HalpUnmapVirtualAddress;
#endif
#endif

    /* Register ACPI stub */
    KdCheckPowerButton = HalpCheckPowerButton;
}

NTSTATUS
NTAPI
HalpAssignSlotResources(IN PUNICODE_STRING RegistryPath,
                        IN PUNICODE_STRING DriverClassName,
                        IN PDRIVER_OBJECT DriverObject,
                        IN PDEVICE_OBJECT DeviceObject,
                        IN INTERFACE_TYPE BusType,
                        IN ULONG BusNumber,
                        IN ULONG SlotNumber,
                        IN OUT PCM_RESOURCE_LIST *AllocatedResources)
{
    BUS_HANDLER BusHandler;
    PAGED_CODE();

    /* Only PCI is supported */
    if (BusType != PCIBus) return STATUS_NOT_IMPLEMENTED;

    /* Setup fake PCI Bus handler */
    RtlCopyMemory(&BusHandler, &HalpFakePciBusHandler, sizeof(BUS_HANDLER));
    BusHandler.BusNumber = BusNumber;

    /* Call the PCI function */
    return HalpAssignPCISlotResources(&BusHandler,
                                      &BusHandler,
                                      RegistryPath,
                                      DriverClassName,
                                      DriverObject,
                                      DeviceObject,
                                      SlotNumber,
                                      AllocatedResources);
}

BOOLEAN
NTAPI
HalpTranslateBusAddress(IN INTERFACE_TYPE InterfaceType,
                        IN ULONG BusNumber,
                        IN PHYSICAL_ADDRESS BusAddress,
                        IN OUT PULONG AddressSpace,
                        OUT PPHYSICAL_ADDRESS TranslatedAddress)
{
    /* Translation is easy */
    TranslatedAddress->QuadPart = BusAddress.QuadPart;
    return TRUE;
}

BOOLEAN
NTAPI
HalpFindBusAddressTranslation(IN PHYSICAL_ADDRESS BusAddress,
                              IN OUT PULONG AddressSpace,
                              OUT PPHYSICAL_ADDRESS TranslatedAddress,
                              IN OUT PULONG_PTR Context,
                              IN BOOLEAN NextBus)
{
    /* Make sure we have a context */
    if (!Context) return FALSE;

    /* If we have data in the context, then this shouldn't be a new lookup */
    if ((*Context != 0) && (NextBus != FALSE)) return FALSE;

    /* Return bus data */
    TranslatedAddress->QuadPart = BusAddress.QuadPart;

    /* Set context value and return success */
    *Context = 1;
    return TRUE;
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 *
 * Walks the IO_RESOURCE_REQUIREMENTS_LIST and constrains every interrupt
 * descriptor's vector range to one the underlying interrupt controller can
 * actually program. Without this, a generic PCI bus driver requirement of
 * "any vector in [0, 0xFF]" can be satisfied by the PnP arbiter with a vector
 * above the IO-APIC's redirection-entry count, which then asserts in
 * IOApicWrite when the kernel tries to route the interrupt (e.g. on hot-plug,
 * where no BIOS has programmed PCI Configuration Space InterruptLine and no
 * boot-resources interrupt entry is supplied).
 *
 * The maximum reachable system vector comes from the linked HAL component:
 * for the APIC HAL it is the IO-APIC's redirection-entry count read at boot
 * from the IO-APIC Version Register; for the PIC HAL it is the 8259 cascade
 * ceiling of 15. Clamping at the requirements stage matches the Windows
 * behavior where the platform HAL is the authority on which vectors are
 * valid system resources.
 */
NTSTATUS
NTAPI
HalAdjustResourceList(IN OUT PIO_RESOURCE_REQUIREMENTS_LIST* pRequirementsList)
{
    PIO_RESOURCE_REQUIREMENTS_LIST List;
    PIO_RESOURCE_LIST AltList;
    PIO_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Alt, Idx;
    ULONG MaxVector;

    if (!pRequirementsList || !*pRequirementsList)
        return STATUS_SUCCESS;

    List = *pRequirementsList;
    MaxVector = HalpGetSystemInterruptMaxVector();
    if (MaxVector == 0)
        return STATUS_SUCCESS;

    AltList = &List->List[0];
    for (Alt = 0; Alt < List->AlternativeLists; Alt++)
    {
        for (Idx = 0; Idx < AltList->Count; Idx++)
        {
            Descriptor = &AltList->Descriptors[Idx];
            if (Descriptor->Type != CmResourceTypeInterrupt)
                continue;

            if (Descriptor->u.Interrupt.MaximumVector > MaxVector)
            {
                DPRINT1("HAL: clamping IRQ requirement [%lu..%lu] to MaxVector=%lu\n",
                        Descriptor->u.Interrupt.MinimumVector,
                        Descriptor->u.Interrupt.MaximumVector,
                        MaxVector);
                Descriptor->u.Interrupt.MaximumVector = MaxVector;
            }
            if (Descriptor->u.Interrupt.MinimumVector > MaxVector)
            {
                /* Whole range is unreachable - flatten to MaxVector so the
                 * arbiter at least knows what the platform can offer; if the
                 * device truly demands a vector this HAL cannot supply, the
                 * arbitrate-and-conflict path will surface the failure to the
                 * user-mode PnP manager rather than asserting in HAL. */
                Descriptor->u.Interrupt.MinimumVector = MaxVector;
            }
        }
        AltList = (PIO_RESOURCE_LIST)(AltList->Descriptors + AltList->Count);
    }

    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
HalAssignSlotResources(IN PUNICODE_STRING RegistryPath,
                       IN PUNICODE_STRING DriverClassName,
                       IN PDRIVER_OBJECT DriverObject,
                       IN PDEVICE_OBJECT DeviceObject,
                       IN INTERFACE_TYPE BusType,
                       IN ULONG BusNumber,
                       IN ULONG SlotNumber,
                       IN OUT PCM_RESOURCE_LIST *AllocatedResources)
{
    /* Check the bus type */
    if (BusType != PCIBus)
    {
        /* Call our internal handler */
        return HalpAssignSlotResources(RegistryPath,
                                       DriverClassName,
                                       DriverObject,
                                       DeviceObject,
                                       BusType,
                                       BusNumber,
                                       SlotNumber,
                                       AllocatedResources);
    }
    else
    {
        /* Call the PCI registered function */
        return HalPciAssignSlotResources(RegistryPath,
                                         DriverClassName,
                                         DriverObject,
                                         DeviceObject,
                                         PCIBus,
                                         BusNumber,
                                         SlotNumber,
                                         AllocatedResources);
    }
}

/*
 * @implemented
 */
ULONG
NTAPI
HalGetBusData(IN BUS_DATA_TYPE BusDataType,
              IN ULONG BusNumber,
              IN ULONG SlotNumber,
              IN PVOID Buffer,
              IN ULONG Length)
{
    /* Call the extended function */
    return HalGetBusDataByOffset(BusDataType,
                                 BusNumber,
                                 SlotNumber,
                                 Buffer,
                                 0,
                                 Length);
}

/*
 * @implemented
 */
ULONG
NTAPI
HalGetBusDataByOffset(IN BUS_DATA_TYPE BusDataType,
                      IN ULONG BusNumber,
                      IN ULONG SlotNumber,
                      IN PVOID Buffer,
                      IN ULONG Offset,
                      IN ULONG Length)
{
    BUS_HANDLER BusHandler;

    /* Look as the bus type */
    if (BusDataType == Cmos)
    {
        /* Call CMOS Function */
        return HalpGetCmosData(0, SlotNumber, Buffer, Length);
    }
    else if (BusDataType == EisaConfiguration)
    {
        /* FIXME: TODO */
        ASSERT(FALSE);
    }
    else if ((BusDataType == PCIConfiguration) &&
             (HalpPCIConfigInitialized) &&
             ((BusNumber >= HalpMinPciBus) && (BusNumber <= HalpMaxPciBus)))
    {
        /* Setup fake PCI Bus handler */
        RtlCopyMemory(&BusHandler, &HalpFakePciBusHandler, sizeof(BUS_HANDLER));
        BusHandler.BusNumber = BusNumber;

        /* Call PCI function */
        return HalpGetPCIData(&BusHandler,
                              &BusHandler,
                              SlotNumber,
                              Buffer,
                              Offset,
                              Length);
    }

    /* Invalid bus */
    return 0;
}

/*
 * @implemented
 */
ULONG
NTAPI
HalGetInterruptVector(IN INTERFACE_TYPE InterfaceType,
                      IN ULONG BusNumber,
                      IN ULONG BusInterruptLevel,
                      IN ULONG BusInterruptVector,
                      OUT PKIRQL Irql,
                      OUT PKAFFINITY Affinity)
{
    /* Call the system bus translator */
    return HalpGetRootInterruptVector(BusInterruptLevel,
                                      BusInterruptVector,
                                      Irql,
                                      Affinity);
}

/*
 * @implemented
 */
ULONG
NTAPI
HalSetBusData(IN BUS_DATA_TYPE BusDataType,
              IN ULONG BusNumber,
              IN ULONG SlotNumber,
              IN PVOID Buffer,
              IN ULONG Length)
{
    /* Call the extended function */
    return HalSetBusDataByOffset(BusDataType,
                                 BusNumber,
                                 SlotNumber,
                                 Buffer,
                                 0,
                                 Length);
}

/*
 * @implemented
 */
ULONG
NTAPI
HalSetBusDataByOffset(IN BUS_DATA_TYPE BusDataType,
                      IN ULONG BusNumber,
                      IN ULONG SlotNumber,
                      IN PVOID Buffer,
                      IN ULONG Offset,
                      IN ULONG Length)
{
    BUS_HANDLER BusHandler;

    /* Look as the bus type */
    if (BusDataType == Cmos)
    {
        /* Call CMOS Function */
        return HalpSetCmosData(0, SlotNumber, Buffer, Length);
    }
    else if ((BusDataType == PCIConfiguration) && (HalpPCIConfigInitialized))
    {
        /* Setup fake PCI Bus handler */
        RtlCopyMemory(&BusHandler, &HalpFakePciBusHandler, sizeof(BUS_HANDLER));
        BusHandler.BusNumber = BusNumber;

        /* Call PCI function */
        return HalpSetPCIData(&BusHandler,
                              &BusHandler,
                              SlotNumber,
                              Buffer,
                              Offset,
                              Length);
    }

    /* Invalid bus */
    return 0;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalTranslateBusAddress(IN INTERFACE_TYPE InterfaceType,
                       IN ULONG BusNumber,
                       IN PHYSICAL_ADDRESS BusAddress,
                       IN OUT PULONG AddressSpace,
                       OUT PPHYSICAL_ADDRESS TranslatedAddress)
{
    /* Look as the bus type */
    if (InterfaceType == PCIBus)
    {
        /* Call the PCI registered function */
        return HalPciTranslateBusAddress(PCIBus,
                                         BusNumber,
                                         BusAddress,
                                         AddressSpace,
                                         TranslatedAddress);
    }
    else
    {
        /* Translation is easy */
        TranslatedAddress->QuadPart = BusAddress.QuadPart;
        return TRUE;
    }
}

/* EOF */
