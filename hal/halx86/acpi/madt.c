/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Source File for MADT Table parsing
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2023 Serge Gautherie <reactos-git_serge_171003@gautherie.fr>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include <acpi.h>
/* ACPI_BIOS_ERROR defined in acoutput.h and bugcodes.h */
#undef ACPI_BIOS_ERROR
#include <smp.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

HALP_APIC_INFO_TABLE HalpApicInfoTable;

PROCESSOR_IDENTITY HalpProcessorIdentity[MAXIMUM_PROCESSORS];

extern ULONG HalpPicVectorRedirect[16];

typedef struct _HALP_ISA_INTERRUPT_OVERRIDE
{
    BOOLEAN Present;
    BOOLEAN Applied;
    BOOLEAN PolarityValid;
    BOOLEAN ActiveLow;
    BOOLEAN TriggerValid;
    BOOLEAN LevelTriggered;
    ULONG Gsi;
    USHORT IntiFlags;
} HALP_ISA_INTERRUPT_OVERRIDE, *PHALP_ISA_INTERRUPT_OVERRIDE;

static HALP_ISA_INTERRUPT_OVERRIDE
HalpIsaInterruptOverrides[RTL_NUMBER_OF(HalpPicVectorRedirect)];

/* The table is parsed before debug output works, so problems are reported later */
static ULONG HalpMadtIgnoredEntries;
static BOOLEAN HalpMadtTruncated;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Adds the processor of a local APIC or local x2APIC entry.
 *
 * @param[in] ApicId
 * Local APIC ID of the processor.
 *
 * @param[in] ProcessorId
 * ACPI processor ID of the processor.
 *
 * @param[in] Flags
 * Local APIC flags of the entry.
 */
static
VOID
HalpMadtAddProcessor(
    _In_ ULONG ApicId,
    _In_ ULONG ProcessorId,
    _In_ ULONG Flags)
{
    ULONG Index;

    if (!(Flags & ACPI_MADT_ENABLED))
        return;

    /* TODO: LapicId holds 8 bits, and 0xFF is the broadcast ID, we eventually want to support x2APIC however. */
    if (ApicId >= 0xFF)
    {
        HalpMadtIgnoredEntries++;
        return;
    }

    /* Firmware can describe a processor with both a local APIC and a local x2APIC entry */
    for (Index = 0; Index < HalpApicInfoTable.ProcessorCount; Index++)
    {
        if (HalpProcessorIdentity[Index].LapicId == ApicId)
            return;
    }

    if (Index >= RTL_NUMBER_OF(HalpProcessorIdentity))
    {
        HalpMadtIgnoredEntries++;
        return;
    }

    /* FIXME: Only extend tracking when we get x2APIC support */
    HalpProcessorIdentity[Index].ProcessorId = ProcessorId;
    HalpProcessorIdentity[Index].LapicId = ApicId;
    HalpApicInfoTable.ProcessorCount++;
}

/**
 * @brief
 * Records the I/O APIC of an I/O APIC entry.
 *
 * @param[in] IoApic
 * The I/O APIC entry.
 */
static
VOID
HalpMadtAddIoApic(
    _In_ ACPI_MADT_IO_APIC *IoApic)
{
    C_ASSERT(RTL_FIELD_SIZE(ACPI_MADT_IO_APIC, Id) == 1 &&
             RTL_NUMBER_OF_FIELD(HALP_APIC_INFO_TABLE, IoApicPA) >= 256);

    /* Keep the first unit when an ID is duplicated, because apparently this happens on Dell PowerEdge's sometimes. */
    if ((IoApic->Address == 0) || (HalpApicInfoTable.IoApicPA[IoApic->Id] != 0))
    {
        HalpMadtIgnoredEntries++;
        return;
    }

    HalpApicInfoTable.IoApicPA[IoApic->Id] = IoApic->Address;
    HalpApicInfoTable.IoApicIrqBase[IoApic->Id] = IoApic->GlobalIrqBase;
    HalpApicInfoTable.IOAPICCount++;
}

/**
 * @brief
 * Records the routing of an interrupt source override entry.
 *
 * @param[in] Override
 * The interrupt source override entry.
 */
static
VOID
HalpMadtAddInterruptOverride(
    _In_ ACPI_MADT_INTERRUPT_OVERRIDE *InterruptOverride)
{
    PHALP_ISA_INTERRUPT_OVERRIDE Override;
    HALP_ISA_INTERRUPT_OVERRIDE NewOverride;
    USHORT Polarity;
    USHORT Trigger;

    /* Overrides only exist for ISA IRQs */
    if ((InterruptOverride->Bus != 0) ||
        (InterruptOverride->SourceIrq >= RTL_NUMBER_OF(HalpPicVectorRedirect)))
    {
        HalpMadtIgnoredEntries++;
        return;
    }

    /* Keep the first override when a source IRQ is described twice */
    Override = &HalpIsaInterruptOverrides[InterruptOverride->SourceIrq];
    if (Override->Present)
    {
        HalpMadtIgnoredEntries++;
        return;
    }

    RtlZeroMemory(&NewOverride, sizeof(NewOverride));
    NewOverride.Gsi = InterruptOverride->GlobalIrq;
    NewOverride.IntiFlags = InterruptOverride->IntiFlags;

    Polarity = InterruptOverride->IntiFlags & ACPI_MADT_POLARITY_MASK;
    switch (Polarity)
    {
        case ACPI_MADT_POLARITY_ACTIVE_HIGH:
            NewOverride.PolarityValid = TRUE;
            NewOverride.ActiveLow = FALSE;
            break;

        case ACPI_MADT_POLARITY_ACTIVE_LOW:
            NewOverride.PolarityValid = TRUE;
            NewOverride.ActiveLow = TRUE;
            break;

        default:
            break;
    }

    Trigger = InterruptOverride->IntiFlags & ACPI_MADT_TRIGGER_MASK;
    switch (Trigger)
    {
        case ACPI_MADT_TRIGGER_EDGE:
            NewOverride.TriggerValid = TRUE;
            NewOverride.LevelTriggered = FALSE;
            break;

        case ACPI_MADT_TRIGGER_LEVEL:
            NewOverride.TriggerValid = TRUE;
            NewOverride.LevelTriggered = TRUE;
            break;

        default:
            break;
    }

    /* Reject overrides with reserved polarity or trigger bits */
    if ((Polarity == ACPI_MADT_POLARITY_RESERVED) ||
        (Trigger == ACPI_MADT_TRIGGER_RESERVED))
    {
        HalpMadtIgnoredEntries++;
        return;
    }

    NewOverride.Present = TRUE;

    /* The APIC HAL still wires its RTC clock vector to input 8.
     * Keep the pre-existing identity route until that clock path
     * can consume a non-identity IRQ 8 override end to end. */
    if ((InterruptOverride->SourceIrq == 8) && (NewOverride.Gsi != 8))
    {
        *Override = NewOverride;
        return;
    }

    NewOverride.Applied = TRUE;
    *Override = NewOverride;
    HalpPicVectorRedirect[InterruptOverride->SourceIrq] = NewOverride.Gsi;
}

/**
 * @brief
 * Walks the interrupt controller entries of the MADT.
 *
 * @param[in] MadtTable
 * The MADT, with a header already validated.
 */
static
VOID
HalpMadtParseEntries(
    _In_ ACPI_TABLE_MADT *MadtTable)
{
    ACPI_SUBTABLE_HEADER *Entry;
    ULONG_PTR TableEnd;

    TableEnd = (ULONG_PTR)MadtTable + MadtTable->Header.Length;
    Entry = (ACPI_SUBTABLE_HEADER *)(MadtTable + 1);

    while ((ULONG_PTR)Entry < TableEnd)
    {
        if (((ULONG_PTR)(Entry + 1) > TableEnd) ||
            (Entry->Length < sizeof(*Entry)) ||
            ((ULONG_PTR)Entry + Entry->Length > TableEnd))
        {
            HalpMadtTruncated = TRUE;
            return;
        }

        switch (Entry->Type)
        {
            case ACPI_MADT_TYPE_LOCAL_APIC:
            {
                ACPI_MADT_LOCAL_APIC *LocalApic = (ACPI_MADT_LOCAL_APIC *)Entry;

                if (Entry->Length < sizeof(*LocalApic))
                {
                    HalpMadtIgnoredEntries++;
                    break;
                }

                HalpMadtAddProcessor(LocalApic->Id,
                                     LocalApic->ProcessorId,
                                     LocalApic->LapicFlags);
                break;
            }

            case ACPI_MADT_TYPE_LOCAL_X2APIC:
            {
                ACPI_MADT_LOCAL_X2APIC *LocalX2Apic = (ACPI_MADT_LOCAL_X2APIC *)Entry;

                if (Entry->Length < sizeof(*LocalX2Apic))
                {
                    HalpMadtIgnoredEntries++;
                    break;
                }

                HalpMadtAddProcessor(LocalX2Apic->LocalApicId,
                                     LocalX2Apic->Uid,
                                     LocalX2Apic->LapicFlags);
                break;
            }

            case ACPI_MADT_TYPE_IO_APIC:
            {
                if (Entry->Length < sizeof(ACPI_MADT_IO_APIC))
                {
                    HalpMadtIgnoredEntries++;
                    break;
                }

                HalpMadtAddIoApic((ACPI_MADT_IO_APIC *)Entry);
                break;
            }

            case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE:
            {
                if (Entry->Length < sizeof(ACPI_MADT_INTERRUPT_OVERRIDE))
                {
                    HalpMadtIgnoredEntries++;
                    break;
                }

                HalpMadtAddInterruptOverride((ACPI_MADT_INTERRUPT_OVERRIDE *)Entry);
                break;
            }

            /* Other types, including reserved and OEM ones, are skipped */
            default:
                break;
        }

        Entry = (ACPI_SUBTABLE_HEADER *)((ULONG_PTR)Entry + Entry->Length);
    }
}

/**
 * @brief
 * Moves the boot processor to the first entry, since NT processor numbers
 * follow the entry order.
 *
 * @remarks
 * When the MADT does not describe the boot processor, only the boot
 * processor is kept so that no other processor is started.
 */
static
VOID
HalpMadtPlaceBootProcessor(VOID)
{
    PROCESSOR_IDENTITY BootProcessor;
    ULONG Index;
    UCHAR BootApicId;

    /* The kernel fills this from CPUID before initializing the processor with the HAL */
    BootApicId = (UCHAR)KeGetCurrentPrcb()->InitialApicId;

    for (Index = 0; Index < HalpApicInfoTable.ProcessorCount; Index++)
    {
        if (HalpProcessorIdentity[Index].LapicId == BootApicId)
            break;
    }

    if (Index == HalpApicInfoTable.ProcessorCount)
    {
        /* The MADT is missing the boot processor, so we can't trust it and only run the boot processor */
        RtlZeroMemory(&BootProcessor, sizeof(BootProcessor));
        BootProcessor.LapicId = BootApicId;
        HalpApicInfoTable.ProcessorCount = 1;
    }
    else
    {
        /* A conforming MADT lists the boot processor first, so this only moves entries on non-conforming ones */
        BootProcessor = HalpProcessorIdentity[Index];
        RtlMoveMemory(&HalpProcessorIdentity[1],
                      &HalpProcessorIdentity[0],
                      Index * sizeof(BootProcessor));
    }

    BootProcessor.BSPCheck = TRUE;
    HalpProcessorIdentity[0] = BootProcessor;
}

VOID
HalpParseApicTables(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ACPI_TABLE_MADT *MadtTable;

    MadtTable = HalAcpiGetTable(LoaderBlock, APIC_SIGNATURE);
    if (MadtTable && (MadtTable->Header.Length >= sizeof(*MadtTable)))
    {
        HalpApicInfoTable.LocalApicPA = MadtTable->Address;
        HalpMadtParseEntries(MadtTable);
    }
    else
    {
        HalpMadtTruncated = TRUE;
    }

    HalpMadtPlaceBootProcessor();
}

BOOLEAN
NTAPI
HalpGetIsaInterruptOverride(
    _In_ ULONG SourceIrq,
    _Out_ PULONG Gsi,
    _Out_ PBOOLEAN PolarityValid,
    _Out_ PBOOLEAN ActiveLow,
    _Out_ PBOOLEAN TriggerValid,
    _Out_ PBOOLEAN LevelTriggered)
{
    PHALP_ISA_INTERRUPT_OVERRIDE Override;

    if (SourceIrq >= RTL_NUMBER_OF(HalpIsaInterruptOverrides))
        return FALSE;

    *Gsi = HalpPicVectorRedirect[SourceIrq];
    *PolarityValid = FALSE;
    *ActiveLow = FALSE;
    *TriggerValid = FALSE;
    *LevelTriggered = FALSE;

    Override = &HalpIsaInterruptOverrides[SourceIrq];
    if (!Override->Present)
        return TRUE;

    if (!Override->Applied)
        return FALSE;

    *PolarityValid = Override->PolarityValid;
    *ActiveLow = Override->ActiveLow;
    *TriggerValid = Override->TriggerValid;
    *LevelTriggered = Override->LevelTriggered;
    return TRUE;
}

VOID
HalpPrintApicTables(VOID)
{
#if DBG
    ULONG i;

    DPRINT1("Physical processor count: %lu\n", HalpApicInfoTable.ProcessorCount);
    for (i = 0; i < HalpApicInfoTable.ProcessorCount; i++)
    {
        DPRINT1(" Processor %lu: ProcessorId %u, LapicId %u, ProcessorStarted %u, BSPCheck %u, ProcessorPrcb %p\n",
                i,
                HalpProcessorIdentity[i].ProcessorId,
                HalpProcessorIdentity[i].LapicId,
                HalpProcessorIdentity[i].ProcessorStarted,
                HalpProcessorIdentity[i].BSPCheck,
                HalpProcessorIdentity[i].ProcessorPrcb);
    }

    for (i = 0; i < HALP_APIC_INFO_TABLE_IOAPIC_NUMBER; i++)
    {
        if (HalpApicInfoTable.IoApicPA[i] != 0)
        {
            DPRINT1(" I/O APIC %lu: Address 0x%08lx, GlobalIrqBase %lu\n",
                    i,
                    HalpApicInfoTable.IoApicPA[i],
                    HalpApicInfoTable.IoApicIrqBase[i]);
        }
    }

    if (HalpMadtTruncated || (HalpMadtIgnoredEntries != 0))
    {
        DPRINT1("MADT missing or malformed: %u, entries ignored: %lu\n",
                HalpMadtTruncated,
                HalpMadtIgnoredEntries);
    }

    for (i = 0; i < RTL_NUMBER_OF(HalpIsaInterruptOverrides); i++)
    {
        PHALP_ISA_INTERRUPT_OVERRIDE Override = &HalpIsaInterruptOverrides[i];

        if (!Override->Present)
            continue;

        DPRINT1(" MADT ISO: IRQ %lu -> GSI %lu flags %04x "
                "polarity=%s trigger=%s applied=%u\n",
                i,
                Override->Gsi,
                Override->IntiFlags,
                Override->PolarityValid ?
                    (Override->ActiveLow ? "low" : "high") : "conforms",
                Override->TriggerValid ?
                    (Override->LevelTriggered ? "level" : "edge") : "conforms",
                Override->Applied);
    }
#endif
}
