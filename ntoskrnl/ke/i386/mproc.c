/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2023 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY Gdt[128];
    DECLSPEC_ALIGN(16) UINT8 NMIStackData[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS Tss;
    KTSS TssDoubleFault;
    KTSS TssNMI;
} APINFO, *PAPINFO;

typedef struct _AP_SETUP_STACK
{
    PVOID ReturnAddr;
    PVOID KxLoaderBlock;
} AP_SETUP_STACK, *PAP_SETUP_STACK; // Note: expected layout only for 32-bit x86

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack = NULL, DPCStack = NULL;
    PAPINFO APInfo = NULL;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;
    ULONG_PTR BootPrcb, BootThread, BootKernelStack;

    /* NOTE: NT6+ HAL exports HalEnumerateProcessors() and
     * HalQueryMaximumProcessorCount() that help determining
     * the number of detected processors on the system. */
    MaximumProcessors = KeMaximumProcessors;

    /* Limit the number of processors we can start at run-time */
    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);

    /* Limit also the number of processors we can start during boot-time */
    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    // TODO: Support processor nodes

    /* Snapshot the boot CPU's hand-off fields. The loop below repurposes
     * Prcb/Thread/KernelStack in the loader block as the hand-off slot for
     * each AP in turn, so we have to put the boot CPU's values back once we
     * are done with them -- see the restore at the bottom of this function.
     * Prcb normally reads 0 here, because the boot CPU already cleared it at
     * the end of KiInitializeKernel (ke/i386/kiinit.c), which is what the
     * hand-off protocol means by "no processor start is pending". */
    BootPrcb = KeLoaderBlock->Prcb;
    BootThread = KeLoaderBlock->Thread;
    BootKernelStack = KeLoaderBlock->KernelStack;

    /* Start ProcessorCount at 1 because we already have the boot CPU */
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        APInfo = NULL;
        KernelStack = NULL;
        DPCStack = NULL;

        // Allocate structures for a new CPU.
        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (!APInfo)
            break;
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);

        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (!KernelStack)
            break;

        DPCStack = MmCreateKernelStack(FALSE, 0);
        if (!DPCStack)
            break;

        // Initalize a new PCR for the specific AP
        KiInitializePcr(ProcessorCount,
                        &APInfo->Pcr,
                        &APInfo->Idt[0],
                        &APInfo->Gdt[0],
                        &APInfo->Tss,
                        (PKTHREAD)&APInfo->Thread,
                        DPCStack);

        // Prepare descriptor tables
        KDESCRIPTOR bspGdt, bspIdt;
        __sgdt(&bspGdt.Limit);
        __sidt(&bspIdt.Limit);
        RtlCopyMemory(&APInfo->Gdt, (PVOID)bspGdt.Base, bspGdt.Limit + 1);
        RtlCopyMemory(&APInfo->Idt, (PVOID)bspIdt.Base, bspIdt.Limit + 1);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_R0_PCR), (ULONG_PTR)&APInfo->Pcr);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_DF_TSS), (ULONG_PTR)&APInfo->TssDoubleFault);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_NMI_TSS), (ULONG_PTR)&APInfo->TssNMI);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS), (ULONG_PTR)&APInfo->Tss);
        // Clear TSS Busy flag (aka set the type to "TSS (Available)")
        KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS)->HighWord.Bits.Type = I386_TSS;

        APInfo->TssDoubleFault.Esp0 = (ULONG_PTR)&APInfo->NMIStackData;
        APInfo->TssDoubleFault.Esp = (ULONG_PTR)&APInfo->NMIStackData;

        APInfo->TssNMI.Esp0 = (ULONG_PTR)&APInfo->NMIStackData;
        APInfo->TssNMI.Esp = (ULONG_PTR)&APInfo->NMIStackData;

        // Fill the processor state
        PKPROCESSOR_STATE ProcessorState = &APInfo->Pcr.Prcb->ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();

        ProcessorState->ContextFrame.SegCs = KGDT_R0_CODE;
        ProcessorState->ContextFrame.SegDs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegEs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegSs = KGDT_R0_DATA;
        ProcessorState->ContextFrame.SegFs = KGDT_R0_PCR;

        ProcessorState->SpecialRegisters.Gdtr.Base = (ULONG_PTR)APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = sizeof(APInfo->Gdt) - 1;
        ProcessorState->SpecialRegisters.Idtr.Base = (ULONG_PTR)APInfo->Idt;
        ProcessorState->SpecialRegisters.Idtr.Limit = sizeof(APInfo->Idt) - 1;

        ProcessorState->SpecialRegisters.Tr = KGDT_TSS;

        ProcessorState->ContextFrame.Esp = (ULONG_PTR)KernelStack;
        ProcessorState->ContextFrame.Eip = (ULONG_PTR)KiSystemStartup;
        ProcessorState->ContextFrame.EFlags = __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        ProcessorState->ContextFrame.Esp = (ULONG)((ULONG_PTR)ProcessorState->ContextFrame.Esp - sizeof(AP_SETUP_STACK));
        PAP_SETUP_STACK ApStack = (PAP_SETUP_STACK)ProcessorState->ContextFrame.Esp;
        ApStack->KxLoaderBlock = KeLoaderBlock;
        ApStack->ReturnAddr = NULL;

        // Update the LOADER_PARAMETER_BLOCK structure for the new processor.
        // Note that on x86 the PCR holds a *pointer* to its PRCB, so the PRCB
        // to hand over is Pcr.PrcbData, not the address of the Pcr.Prcb field.
        // Likewise, KiSystemStartup dereferences Thread on the AP side, so it
        // must be the ETHREAD we handed to KiInitializePcr as the idle thread.
        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)&APInfo->Pcr.PrcbData;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread;

        // Start the CPU
        DPRINT("Attempting to Start a CPU with number: %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            break;
        }

        // And wait for it to start
        while (KeLoaderBlock->Prcb != 0)
        {
            //TODO: Add a time out so we don't wait forever
            KeMemoryBarrier();
            YieldProcessor();
        }

        // The AP is alive and now owns APInfo (its PCR, PRCB, GDT, IDT and
        // TSSes), its kernel stack and its DPC stack. Drop our references so
        // that the cleanup below cannot free a running processor's data,
        // no matter which way we leave this loop.
        APInfo = NULL;
        KernelStack = NULL;
        DPCStack = NULL;
    }

    // If anything is left here, the last iteration failed to start its CPU
    // (an allocation failed, or the HAL has no more processors to start),
    // so nothing else references it - clean the data. KiInitializePcr has
    // already published this PRCB in KiProcessorBlock[], so retract that
    // reference too, otherwise the array points into freed pool.
    if (APInfo)
    {
        KiProcessorBlock[ProcessorCount] = NULL;
        ExFreePoolWithTag(APInfo, TAG_KERNEL);
    }
    if (KernelStack)
        MmDeleteKernelStack(KernelStack, FALSE);
    if (DPCStack)
        MmDeleteKernelStack(DPCStack, FALSE);

    // ProcessorCount is one past the last AP we attempted: either the number
    // of the CPU that didn't start, or MaximumProcessors when the loop ran to
    // completion. Either way the number of APs that did start is one less.
    ProcessorCount--;

    // Put the boot CPU's context back into the loader block. On the paths
    // where the last attempt failed we just freed the structures the fields
    // above point to, and on the other paths they name an AP that is already
    // running, which is not what these fields are supposed to describe
    // either. Restoring makes the post-condition exactly the pre-condition:
    // the loader block describes the boot CPU and Prcb reads 0, that is,
    // no processor start is pending.
    KeLoaderBlock->Prcb = BootPrcb;
    KeLoaderBlock->Thread = BootThread;
    KeLoaderBlock->KernelStack = BootKernelStack;

    DPRINT1("KeStartAllProcessors: Successful AP startup count is %lu\n", ProcessorCount);
}
