/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     amd64 Application Processor bring-up
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2023 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* External helper from ntoskrnl/ke/amd64/kiinit.c.  Initializes the
 * per-CPU KIPCR + KTSS64 pair, including the KGDT64_SYS_TSS descriptor
 * in the per-CPU GDT and the Tss->Rsp0 + Tss->Ist[1..3] fields. */
VOID
KiInitializeProcessorBootStructures(
    _In_ ULONG ProcessorNumber,
    _Out_ PKIPCR Pcr,
    _In_ PKGDTENTRY64 GdtBase,
    _In_ PKIDTENTRY64 IdtBase,
    _In_ PKTSS64 TssBase,
    _In_ PKTHREAD IdleThread,
    _In_ PVOID KernelStack,
    _In_ PVOID DpcStack,
    _In_ PVOID DoubleFaultStack,
    _In_ PVOID NmiStack);

/*
 * Per-AP allocation block. One contiguous NonPagedPool chunk per CPU,
 * page-aligned so the IDT and GDT each get their own page and the IST
 * stacks stay clear of the structures behind them. Lifetime is the
 * runtime of the AP -- never freed (last iteration's allocation may be
 * cleaned up if HalStartNextProcessor fails, see KeStartAllProcessors).
 *
 * amd64 uses IST-based stack switching for double-fault and NMI rather
 * than the i386 task-gate-TSS pattern, so we only need one KTSS64 per
 * CPU (vs. three on i386). The IST stacks live inline in this struct
 * and are referenced from Tss.Ist[].
 *
 * NUM_APINFO_GDT_ENTRIES matches the boot CPU's effective GDT size --
 * freeldr's amd64 boot.S/winldr.c only populates entries up to
 * KGDT64_R3_CMTEB (selector 0x50) but rounds up generously. 128 is
 * the same value i386 uses for NUM_GDT and is well past what amd64
 * needs.
 */
#define NUM_APINFO_GDT_ENTRIES 128

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY64 Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY64 Gdt[NUM_APINFO_GDT_ENTRIES];
    DECLSPEC_ALIGN(16) UCHAR DoubleFaultStackData[DOUBLE_FAULT_STACK_SIZE];
    DECLSPEC_ALIGN(16) UCHAR NmiStackData[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS64 Tss;
} APINFO, *PAPINFO;

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack = NULL;
    PVOID DpcStack = NULL;
    PAPINFO APInfo = NULL;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;
    KDESCRIPTOR BspGdt = {{0}, 0, 0};
    KDESCRIPTOR BspIdt = {{0}, 0, 0};
    PKPROCESSOR_STATE ProcessorState;
    ULONG_PTR DoubleFaultStackTop;
    ULONG_PTR NmiStackTop;
    ULONG64 AlignedRsp;

    /* NT6+ HAL exports HalEnumerateProcessors() / HalQueryMaximumProcessorCount()
     * that help determine the number of detected processors. ROS doesn't have
     * them yet -- HalStartNextProcessor bails on its own once we exceed
     * HalpApicInfoTable.ProcessorCount, so iterate up to the global cap. */
    MaximumProcessors = KeMaximumProcessors;
    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);
    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    /* Snapshot the boot CPU's GDT and IDT descriptors once -- both are
     * shared by every CPU on amd64 today (the per-CPU copy here is
     * solely so each CPU can have its own KGDT64_SYS_TSS slot pointing
     * at its own KTSS64). */
    __sgdt(&BspGdt.Limit);
    __sidt(&BspIdt.Limit);

    /* Start ProcessorCount at 1 -- the boot CPU is already running. */
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        KernelStack = NULL;
        DpcStack = NULL;
        APInfo = NULL;

        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (!APInfo)
            break;
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);
        ASSERT((BspGdt.Limit + 1) <= sizeof(APInfo->Gdt));
        ASSERT((BspIdt.Limit + 1) <= sizeof(APInfo->Idt));

        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (!KernelStack)
            break;

        DpcStack = MmCreateKernelStack(FALSE, 0);
        if (!DpcStack)
            break;

        /* Copy the boot CPU's GDT and IDT into the AP's per-CPU pages. */
        RtlCopyMemory(APInfo->Gdt, (PVOID)BspGdt.Base, BspGdt.Limit + 1);
        RtlCopyMemory(APInfo->Idt, (PVOID)BspIdt.Base, BspIdt.Limit + 1);

        /* IST stack tops (stack grows down, so use highest address). */
        DoubleFaultStackTop =
            (ULONG_PTR)&APInfo->DoubleFaultStackData[DOUBLE_FAULT_STACK_SIZE];
        NmiStackTop =
            (ULONG_PTR)&APInfo->NmiStackData[DOUBLE_FAULT_STACK_SIZE];

        /* Wire the per-CPU PCR + TSS together. The helper repoints the
         * AP's GDT KGDT64_SYS_TSS descriptor at &APInfo->Tss, sets
         * Tss->Rsp0 = KernelStack and Tss->Ist[1]/[3] to the IST tops. */
        KiInitializeProcessorBootStructures(ProcessorCount,
                                            &APInfo->Pcr,
                                            APInfo->Gdt,
                                            APInfo->Idt,
                                            &APInfo->Tss,
                                            (PKTHREAD)&APInfo->Thread,
                                            KernelStack,
                                            DpcStack,
                                            (PVOID)DoubleFaultStackTop,
                                            (PVOID)NmiStackTop);

        /* Now build the KPROCESSOR_STATE that the amd64 apentry.S
         * trampoline consumes via the AP's PRCB. */
        ProcessorState = &APInfo->Pcr.Prcb.ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        /* Capture the boot CPU's control registers so the AP wakes up
         * with the same paging mode, CR0 bits, CR4 features (FXSR,
         * XMMEXCPT, OSXSAVE etc.) and IRQL/CR8 as the BSP. CR3 is
         * captured for completeness but apentry.S deliberately does
         * NOT reload it -- see the trampoline header for why. */
        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();
        ProcessorState->SpecialRegisters.Cr8 = __readcr8();
        ProcessorState->SpecialRegisters.MxCsr = INITIAL_MXCSR;

        /* GS base MSRs: publish this AP's KIPCR. KiInitializeCpu does the
         * same wrmsr(MSR_GS_BASE/MSR_GS_SWAP, Pcr) for the BSP at
         * kiinit.c:169-170; here the trampoline does it instead, before
         * any kernel C code on the AP can run. */
        ProcessorState->SpecialRegisters.MsrGsBase = (ULONG64)&APInfo->Pcr;
        ProcessorState->SpecialRegisters.MsrGsSwap = (ULONG64)&APInfo->Pcr;

        /* Per-CPU descriptor tables. KDESCRIPTOR has a 6-byte pad before
         * the Limit/Base pair specifically so &Limit can be handed to
         * lgdt/lidt; apentry.S consumes them at offset SrGdtr/SrIdtr+6. */
        ProcessorState->SpecialRegisters.Gdtr.Limit =
            (USHORT)(sizeof(APInfo->Gdt) - 1);
        ProcessorState->SpecialRegisters.Gdtr.Base = APInfo->Gdt;
        ProcessorState->SpecialRegisters.Idtr.Limit =
            (USHORT)(sizeof(APInfo->Idt) - 1);
        ProcessorState->SpecialRegisters.Idtr.Base = APInfo->Idt;
        ProcessorState->SpecialRegisters.Tr = KGDT64_SYS_TSS;
        ProcessorState->SpecialRegisters.Ldtr = 0;

        /* Segment registers. In long mode DS/ES/SS bases are ignored
         * but the selectors must be valid; FS/GS use MSRs and we leave
         * the selectors at 0 (matches Windows). */
        ProcessorState->ContextFrame.SegCs = KGDT64_R0_CODE;
        ProcessorState->ContextFrame.SegSs = KGDT64_R0_DATA;
        ProcessorState->ContextFrame.SegDs = KGDT64_R3_DATA | RPL_MASK;
        ProcessorState->ContextFrame.SegEs = KGDT64_R3_DATA | RPL_MASK;
        ProcessorState->ContextFrame.SegFs = 0;
        ProcessorState->ContextFrame.SegGs = 0;
        ProcessorState->ContextFrame.EFlags =
            __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        ProcessorState->ContextFrame.Rip = (ULONG64)KiSystemStartup;

        /* Win64 ABI expects (RSP+8) to be 16-byte aligned at function
         * entry (as if a CALL pushed an 8-byte return address). The
         * trampoline lands at KiSystemStartup via lretq, which does
         * NOT push a return address, so we have to fake that
         * alignment by handing over an RSP that ends in 8. */
        AlignedRsp = ((ULONG64)KernelStack) & ~(ULONG64)0xF;
        ProcessorState->ContextFrame.Rsp = AlignedRsp - 8;

        /* Hand off through the loader block:
         *   Prcb        -> KiSystemStartup pulls the AP's PCR from this.
         *   Thread      -> consumed as the AP's initial thread pointer.
         *   KernelStack -> seeds the per-thread switch in KiSwitchToBootStack. */
        KeLoaderBlock->Prcb = (ULONG_PTR)&APInfo->Pcr.Prcb;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread;
        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;

        DPRINT1("KeStartAllProcessors: starting AP %lu (PCR %p)\n",
                ProcessorCount, &APInfo->Pcr);

        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
            break;

        /* Wait for the AP to consume LoaderBlock->Prcb and zero it on
         * the far side. KiInitializeKernel does the zero at
         * ke/amd64/krnlinit.c:209 right before dropping into KiIdleLoop.
         * No timeout here -- if an AP gets stuck on amd64 SMP it's a
         * bug we want to see, not silently paper over. */
        while (KeLoaderBlock->Prcb != 0)
        {
            KeMemoryBarrier();
            YieldProcessor();
        }

        /* The AP is alive and now owns APInfo / KernelStack / DpcStack.
         * Drop our local references so the cleanup below doesn't free
         * structures that are still in use. */
        APInfo = NULL;
        KernelStack = NULL;
        DpcStack = NULL;
    }

    /* The last iteration may have broken out before HalStartNextProcessor
     * succeeded (either we hit the MADT-reported CPU cap or an allocation
     * failed). Reclaim whatever it managed to allocate. */
    if (APInfo)
        ExFreePoolWithTag(APInfo, TAG_KERNEL);
    if (KernelStack)
        MmDeleteKernelStack(KernelStack, FALSE);
    if (DpcStack)
        MmDeleteKernelStack(DpcStack, FALSE);

    /* KeNumberProcessors is bumped from inside KiSystemStartup as each
     * AP comes online (kiinit.c:500), so by the time we exit the loop
     * it already reflects the true count. Report it for visibility. */
    DPRINT1("KeStartAllProcessors: %lu processor(s) online\n",
            (ULONG)KeNumberProcessors);
}
