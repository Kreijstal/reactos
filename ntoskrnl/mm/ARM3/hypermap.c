/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/mm/ARM3/hypermap.c
 * PURPOSE:         ARM Memory Manager Hyperspace Mapping Functionality
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

PMMPTE MmFirstReservedMappingPte, MmLastReservedMappingPte;
PMMPTE MiFirstReservedZeroingPte;
MMPTE HyperTemplatePte;

/* PRIVATE FUNCTIONS **********************************************************/

PVOID
NTAPI
MiMapPageInHyperSpace(IN PEPROCESS Process,
                      IN PFN_NUMBER Page,
                      IN PKIRQL OldIrql)
{
    MMPTE TempPte;
    PMMPTE PointerPte;
    PFN_NUMBER Offset;

    //
    // Never accept page 0 or non-physical pages
    //
    ASSERT(Page != 0);
    ASSERT(MiGetPfnEntry(Page) != NULL);

    //
    // Build the PTE
    //
    TempPte = ValidKernelPteLocal;
    TempPte.u.Hard.PageFrameNumber = Page;

    //
    // Pick the first hyperspace PTE
    //
    PointerPte = MmFirstReservedMappingPte;

    //
    // Acquire the hyperlock
    //
    ASSERT(Process == PsGetCurrentProcess());
#if (NTDDI_VERSION >= NTDDI_LONGHORN)
    //
    // Vista+: EPROCESS no longer contains HyperSpaceLock, so reserve a
    // hyperspace slot atomically.  Raising IRQL only prevents migration of
    // this thread; another processor can still run a thread in the same
    // process and reserve a slot concurrently.
    //
    MMPTE OldPte, NewPte;
    BOOLEAN FlushEntireHyperSpace;

    KeRaiseIrql(DISPATCH_LEVEL, OldIrql);

    //
    // The first PTE stores the FIFO counter.  Updating its PageFrameNumber
    // bit-field with a normal assignment is a non-atomic read/modify/write
    // and lets two CPUs select the same mapping PTE.  If that happens, one
    // page copy can silently change the physical page below another copy.
    //
    do
    {
        OldPte = *PointerPte;
        NewPte = OldPte;
        Offset = PFN_FROM_PTE(&OldPte);
        FlushEntireHyperSpace = (Offset == 0);
        if (!Offset)
            Offset = MI_HYPERSPACE_PTES;
        NewPte.u.Hard.PageFrameNumber = Offset - 1;
    }
    while ((ULONG_PTR)InterlockedCompareExchangePte(PointerPte,
                                                     NewPte.u.Long,
                                                     OldPte.u.Long) != OldPte.u.Long);

    /*
     * Reusing the FIFO after its complete cycle requires invalidating all
     * stale hyperspace translations, not merely the one slot selected below.
     * Do this only after the successful reservation; flushing inside the CAS
     * retry loop would leave a race window and needlessly flush on failure.
     */
    if (FlushEntireHyperSpace)
        KeFlushProcessTb();

#else
    KeAcquireSpinLock(&Process->HyperSpaceLock, OldIrql);

    //
    // Now get the first free PTE
    //
    Offset = PFN_FROM_PTE(PointerPte);
    if (!Offset)
    {
        //
        // Reset the PTEs
        //
        Offset = MI_HYPERSPACE_PTES;
        KeFlushProcessTb();
    }

    //
    // Prepare the next PTE
    //
    PointerPte->u.Hard.PageFrameNumber = Offset - 1;
#endif

    //
    // Write the current PTE
    //
    PointerPte += Offset;
#if (NTDDI_VERSION >= NTDDI_LONGHORN)
    //
    // A slot can have a stale translation on the processor that reserved it
    // during an earlier FIFO pass.  Invalidate locally before publishing the
    // new mapping; the returned address is only used on this processor while
    // IRQL remains raised.
    //
    KeInvalidateTlbEntry(MiPteToAddress(PointerPte));
#endif
    MI_WRITE_VALID_PTE(PointerPte, TempPte);

    //
    // Return the address
    //
    return MiPteToAddress(PointerPte);
}

VOID
NTAPI
MiUnmapPageInHyperSpace(IN PEPROCESS Process,
                        IN PVOID Address,
                        IN KIRQL OldIrql)
{
    ASSERT(Process == PsGetCurrentProcess());

    //
    // Blow away the mapping
    //
    MiAddressToPte(Address)->u.Long = 0;
#if (NTDDI_VERSION >= NTDDI_LONGHORN)
    KeInvalidateTlbEntry(Address);
#endif

    //
    // Release the hyperlock
    //
    ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);
#if (NTDDI_VERSION >= NTDDI_LONGHORN)
    KeLowerIrql(OldIrql);
#else
    KeReleaseSpinLock(&Process->HyperSpaceLock, OldIrql);
#endif
}

PVOID
NTAPI
MiMapPagesInZeroSpace(IN PMMPFN Pfn1,
                      IN PFN_NUMBER NumberOfPages)
{
    MMPTE TempPte;
    PMMPTE PointerPte;
    PFN_NUMBER Offset, PageFrameIndex;

    //
    // Sanity checks
    //
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    ASSERT(NumberOfPages != 0);
    ASSERT(NumberOfPages <= MI_ZERO_PTES);

    //
    // Pick the first zeroing PTE
    //
    PointerPte = MiFirstReservedZeroingPte;

    //
    // Now get the first free PTE
    //
    Offset = PFN_FROM_PTE(PointerPte);
    if (NumberOfPages > Offset)
    {
        //
        // Reset the PTEs
        //
        Offset = MI_ZERO_PTES;
        PointerPte->u.Hard.PageFrameNumber = Offset;
        KeFlushProcessTb();
    }

    //
    // Prepare the next PTE
    //
    PointerPte->u.Hard.PageFrameNumber = Offset - NumberOfPages;

    /* Choose the correct PTE to use, and which template */
    PointerPte += (Offset + 1);
    TempPte = ValidKernelPte;

    /* Disable cache. Write through */
    MI_PAGE_DISABLE_CACHE(&TempPte);
    MI_PAGE_WRITE_THROUGH(&TempPte);

    /* Make sure the list isn't empty and loop it */
    ASSERT(Pfn1 != (PVOID)LIST_HEAD);
    while (Pfn1 != (PVOID)LIST_HEAD)
    {
        /* Get the page index for this PFN */
        PageFrameIndex = MiGetPfnEntryIndex(Pfn1);

        //
        // Write the PFN
        //
        TempPte.u.Hard.PageFrameNumber = PageFrameIndex;

        //
        // Set the correct PTE to write to, and set its new value
        //
        PointerPte--;
        MI_WRITE_VALID_PTE(PointerPte, TempPte);

        /* Move to the next PFN */
        Pfn1 = (PMMPFN)Pfn1->u1.Flink;
    }

    //
    // Return the address
    //
    return MiPteToAddress(PointerPte);
}

VOID
NTAPI
MiUnmapPagesInZeroSpace(IN PVOID VirtualAddress,
                        IN PFN_NUMBER NumberOfPages)
{
    PMMPTE PointerPte;

    //
    // Sanity checks
    //
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    ASSERT (NumberOfPages != 0);
    ASSERT(NumberOfPages <= MI_ZERO_PTES);

    //
    // Get the first PTE for the mapped zero VA
    //
    PointerPte = MiAddressToPte(VirtualAddress);

    //
    // Blow away the mapped zero PTEs
    //
    RtlZeroMemory(PointerPte, NumberOfPages * sizeof(MMPTE));
}
