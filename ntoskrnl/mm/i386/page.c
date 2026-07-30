/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/mm/i386/page.c
 * PURPOSE:         Low level memory management manipulation
 *
 * PROGRAMMERS:     David Welch (welch@cwcom.net)
 */

/* INCLUDES ***************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include <mm/ARM3/miarm.h>

#ifndef _MI_PAGING_LEVELS
#error "Dude, fix your stuff before using this file"
#endif

/* GLOBALS *****************************************************************/
const
ULONG_PTR
MmProtectToPteMask[32] =
{
    //
    // These are the base MM_ protection flags
    //
    0,
    PTE_READONLY            | PTE_ENABLE_CACHE,
    PTE_EXECUTE             | PTE_ENABLE_CACHE,
    PTE_EXECUTE_READ        | PTE_ENABLE_CACHE,
    PTE_READWRITE           | PTE_ENABLE_CACHE,
    PTE_WRITECOPY           | PTE_ENABLE_CACHE,
    PTE_EXECUTE_READWRITE   | PTE_ENABLE_CACHE,
    PTE_EXECUTE_WRITECOPY   | PTE_ENABLE_CACHE,
    //
    // These OR in the MM_NOCACHE flag
    //
    0,
    PTE_READONLY            | PTE_DISABLE_CACHE,
    PTE_EXECUTE             | PTE_DISABLE_CACHE,
    PTE_EXECUTE_READ        | PTE_DISABLE_CACHE,
    PTE_READWRITE           | PTE_DISABLE_CACHE,
    PTE_WRITECOPY           | PTE_DISABLE_CACHE,
    PTE_EXECUTE_READWRITE   | PTE_DISABLE_CACHE,
    PTE_EXECUTE_WRITECOPY   | PTE_DISABLE_CACHE,
    //
    // These OR in the MM_DECOMMIT flag, which doesn't seem supported on x86/64/ARM
    //
    0,
    PTE_READONLY            | PTE_ENABLE_CACHE,
    PTE_EXECUTE             | PTE_ENABLE_CACHE,
    PTE_EXECUTE_READ        | PTE_ENABLE_CACHE,
    PTE_READWRITE           | PTE_ENABLE_CACHE,
    PTE_WRITECOPY           | PTE_ENABLE_CACHE,
    PTE_EXECUTE_READWRITE   | PTE_ENABLE_CACHE,
    PTE_EXECUTE_WRITECOPY   | PTE_ENABLE_CACHE,
    //
    // These OR in the MM_NOACCESS flag, which seems to enable WriteCombining?
    //
    0,
    PTE_READONLY            | PTE_WRITECOMBINED_CACHE,
    PTE_EXECUTE             | PTE_WRITECOMBINED_CACHE,
    PTE_EXECUTE_READ        | PTE_WRITECOMBINED_CACHE,
    PTE_READWRITE           | PTE_WRITECOMBINED_CACHE,
    PTE_WRITECOPY           | PTE_WRITECOMBINED_CACHE,
    PTE_EXECUTE_READWRITE   | PTE_WRITECOMBINED_CACHE,
    PTE_EXECUTE_WRITECOPY   | PTE_WRITECOMBINED_CACHE,
};

const
ULONG MmProtectToValue[32] =
{
    PAGE_NOACCESS,
    PAGE_READONLY,
    PAGE_EXECUTE,
    PAGE_EXECUTE_READ,
    PAGE_READWRITE,
    PAGE_WRITECOPY,
    PAGE_EXECUTE_READWRITE,
    PAGE_EXECUTE_WRITECOPY,
    PAGE_NOACCESS,
    PAGE_NOCACHE | PAGE_READONLY,
    PAGE_NOCACHE | PAGE_EXECUTE,
    PAGE_NOCACHE | PAGE_EXECUTE_READ,
    PAGE_NOCACHE | PAGE_READWRITE,
    PAGE_NOCACHE | PAGE_WRITECOPY,
    PAGE_NOCACHE | PAGE_EXECUTE_READWRITE,
    PAGE_NOCACHE | PAGE_EXECUTE_WRITECOPY,
    PAGE_NOACCESS,
    PAGE_GUARD | PAGE_READONLY,
    PAGE_GUARD | PAGE_EXECUTE,
    PAGE_GUARD | PAGE_EXECUTE_READ,
    PAGE_GUARD | PAGE_READWRITE,
    PAGE_GUARD | PAGE_WRITECOPY,
    PAGE_GUARD | PAGE_EXECUTE_READWRITE,
    PAGE_GUARD | PAGE_EXECUTE_WRITECOPY,
    PAGE_NOACCESS,
    PAGE_WRITECOMBINE | PAGE_READONLY,
    PAGE_WRITECOMBINE | PAGE_EXECUTE,
    PAGE_WRITECOMBINE | PAGE_EXECUTE_READ,
    PAGE_WRITECOMBINE | PAGE_READWRITE,
    PAGE_WRITECOMBINE | PAGE_WRITECOPY,
    PAGE_WRITECOMBINE | PAGE_EXECUTE_READWRITE,
    PAGE_WRITECOMBINE | PAGE_EXECUTE_WRITECOPY
};

/* FUNCTIONS ***************************************************************/

#ifdef CONFIG_SMP
static
ULONG_PTR
NTAPI
MiInvalidateSingleTbWorker(IN ULONG_PTR Address)
{
    /* Runs on every processor via KeIpiGenericCall: drop the stale entry */
    KeInvalidateTlbEntry((PVOID)Address);
    return 0;
}
#endif

/*
 * Invalidate the TLB entry for Address on ALL processors.
 *
 * KeInvalidateTlbEntry / KeFlushProcessTb only touch the current processor's
 * TLB.  That is fine for a protection change, but a page that is being
 * UNMAPPED here may be released to the free list and handed to a different
 * owner moments later, possibly on another processor.  If that processor still
 * holds a stale (writable) translation for the frame it will corrupt the new
 * owner - a cross-processor page double-use that manifests as the rotating
 * MEMORY_MANAGEMENT / heap / pool corruption only ever seen on MP.  Broadcast
 * the invalidation so no processor keeps a translation to a page we are about
 * to free.
 *
 * NOTE: this issues a synchronous IPI per call.  That is the right trade-off
 * for the single-page eviction paths (the pageout trimmer in rmap.c, the
 * copy-on-write/protect path in section.c), which steal one page at a time
 * from a still-running process and must drop that one translation everywhere
 * before the frame can be reused.  Bulk teardown loops (MmFreeMemoryArea)
 * instead unmap with MmDeleteVirtualMappingNoBroadcast and issue a single
 * KeFlushEntireTb shootdown for the whole batch.
 */
VOID
MiInvalidateTlbEntryAllProcessors(IN PVOID Address)
{
#ifdef CONFIG_SMP
    if (KeActiveProcessors & ~KeGetCurrentPrcb()->SetMember)
    {
        KeIpiGenericCall(MiInvalidateSingleTbWorker, (ULONG_PTR)Address);
        return;
    }
#endif
    KeInvalidateTlbEntry(Address);
}

NTSTATUS
NTAPI
MiFillSystemPageDirectory(IN PVOID Base,
                          IN SIZE_T NumberOfBytes);

static
BOOLEAN
MiIsPageTablePresent(PVOID Address)
{
#if _MI_PAGING_LEVELS == 2
    BOOLEAN Ret = MmWorkingSetList->UsedPageTableEntries[MiGetPdeOffset(Address)] != 0;

    /* Some sanity check while we're here */
    ASSERT(Ret == (MiAddressToPde(Address)->u.Hard.Valid != 0));

    return Ret;
#else
    PMMPDE PointerPde;
    PMMPPE PointerPpe;
#if _MI_PAGING_LEVELS == 4
    PMMPXE PointerPxe;
#endif
    PMMPFN Pfn;

    /* Make sure we're locked */
    ASSERT((PsGetCurrentThread()->OwnsProcessWorkingSetExclusive) || (PsGetCurrentThread()->OwnsProcessWorkingSetShared));

    /* Must not hold the PFN lock! */
    ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);

        /* Check if PXE or PPE have references first. */
#if _MI_PAGING_LEVELS == 4
    PointerPxe = MiAddressToPxe(Address);
    if ((PointerPxe->u.Hard.Valid == 1) || (PointerPxe->u.Soft.Transition == 1))
    {
        Pfn = MiGetPfnEntry(PFN_FROM_PXE(PointerPxe));
        if (Pfn->OriginalPte.u.Soft.UsedPageTableEntries == 0)
            return FALSE;
    }
    else if (PointerPxe->u.Soft.UsedPageTableEntries == 0)
    {
        return FALSE;
    }

    if (PointerPxe->u.Hard.Valid == 0)
    {
        MiMakeSystemAddressValid(MiPteToAddress(PointerPxe), PsGetCurrentProcess());
    }
#endif

    PointerPpe = MiAddressToPpe(Address);
    if ((PointerPpe->u.Hard.Valid == 1) || (PointerPpe->u.Soft.Transition == 1))
    {
        Pfn = MiGetPfnEntry(PFN_FROM_PPE(PointerPpe));
        if (Pfn->OriginalPte.u.Soft.UsedPageTableEntries == 0)
            return FALSE;
    }
    else if (PointerPpe->u.Soft.UsedPageTableEntries == 0)
    {
        return FALSE;
    }

    if (PointerPpe->u.Hard.Valid == 0)
    {
        MiMakeSystemAddressValid(MiPteToAddress(PointerPpe), PsGetCurrentProcess());
    }

    PointerPde = MiAddressToPde(Address);
    if ((PointerPde->u.Hard.Valid == 0) && (PointerPde->u.Soft.Transition == 0))
    {
        return PointerPde->u.Soft.UsedPageTableEntries != 0;
    }

    /* This lies on the PFN */
    Pfn = MiGetPfnEntry(PFN_FROM_PDE(PointerPde));
    return Pfn->OriginalPte.u.Soft.UsedPageTableEntries != 0;
#endif
}

PFN_NUMBER
NTAPI
MmGetPfnForProcess(PEPROCESS Process,
                   PVOID Address)
{
    PMMPTE PointerPte;
    PFN_NUMBER Page;

    /* Must be called for user mode only */
    ASSERT(Process != NULL);
    ASSERT(Address < MmSystemRangeStart);

    /* And for our process */
    ASSERT(Process == PsGetCurrentProcess());

    /* Lock for reading */
    MiLockProcessWorkingSetShared(Process, PsGetCurrentThread());

    if (!MiIsPageTablePresent(Address))
    {
        MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());
        return 0;
    }

    /* Make sure we can read the PTE */
    MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);

    PointerPte = MiAddressToPte(Address);
    Page = PointerPte->u.Hard.Valid ? PFN_FROM_PTE(PointerPte) : 0;

    MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());
    return Page;
}

/**
 * @brief Deletes the virtual mapping and optionally gives back the page & dirty bit.
 *
 * @param Process - The process this address belongs to, or NULL if system address.
 * @param Address - The address to unmap.
 * @param WasDirty - Optional param receiving the dirty bit of the PTE.
 * @param Page - Optional param receiving the page number previously mapped to this address.
 *
 * @return Whether there was actually a page mapped at the given address.
 */
_Success_(return)
BOOLEAN
MmDeleteVirtualMappingEx(
    _Inout_opt_ PEPROCESS Process,
    _In_ PVOID Address,
    _Out_opt_ BOOLEAN* WasDirty,
    _Out_opt_ PPFN_NUMBER Page,
    _In_ BOOLEAN IsPhysical,
    _In_ BOOLEAN BroadcastTlb)
{
    PMMPTE PointerPte;
    MMPTE OldPte;
    BOOLEAN ValidPde;

    OldPte.u.Long = 0;

    DPRINT("MmDeleteVirtualMapping(%p, %p, %p, %p)\n", Process, Address, WasDirty, Page);

    ASSERT(((ULONG_PTR)Address % PAGE_SIZE) == 0);

    /* And we should be at low IRQL */
    ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);

    /* Make sure our PDE is valid, and that everything is going fine */
    if (Process == NULL)
    {
        if (Address < MmSystemRangeStart)
        {
            DPRINT1("NULL process given for user-mode mapping at %p\n", Address);
            KeBugCheck(MEMORY_MANAGEMENT);
        }
#if (_MI_PAGING_LEVELS == 2)
        ValidPde = MiSynchronizeSystemPde(MiAddressToPde(Address));
#else
        ValidPde = MiIsPdeForAddressValid(Address);
#endif
    }
    else
    {
        if ((Address >= MmSystemRangeStart) || Add2Ptr(Address, PAGE_SIZE) >= MmSystemRangeStart)
        {
            DPRINT1("Process %p given for kernel-mode mapping at %p\n", Process, Address);
            KeBugCheck(MEMORY_MANAGEMENT);
        }

        /* Only for current process !!! */
        ASSERT(Process == PsGetCurrentProcess());
        MiLockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());

        ValidPde = MiIsPageTablePresent(Address);
        if (ValidPde)
        {
            MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);
        }
    }

    /* Get the PTE if we're having anything */
    if (ValidPde)
    {
        PointerPte = MiAddressToPte(Address);
        OldPte.u.Long = InterlockedExchangePte(PointerPte, 0);
#if DBG
        if (OldPte.u.Long != 0)
            MmTracePte('D', Address, OldPte.u.Long, 0, _ReturnAddress());
#endif

        /*
         * The frame this PTE referenced may be freed and reused right after we
         * return, so the stale translation must be dropped on every processor,
         * not just this one (see MiInvalidateTlbEntryAllProcessors).  A batched
         * caller (BroadcastTlb == FALSE) takes responsibility for the cross-
         * processor shootdown itself, after it has unmapped the whole run, and
         * here only needs the cheap local invalidation.
         */
        if (BroadcastTlb)
            MiInvalidateTlbEntryAllProcessors(Address);
        else
            KeInvalidateTlbEntry(Address);

        if (OldPte.u.Long != 0)
        {
            /* It must have been present, or not a swap entry */
            ASSERT(OldPte.u.Hard.Valid || !FlagOn(OldPte.u.Long, 0x800));
            if (WasDirty != NULL)
            {
                *WasDirty = !!OldPte.u.Hard.Dirty;
            }
            if (Page != NULL)
            {
                *Page = OldPte.u.Hard.PageFrameNumber;
            }
        }
    }

    if (Process != NULL)
    {
        /* Remove PDE reference, if needed */
        if (OldPte.u.Long != 0)
        {
            if (MiDecrementPageTableReferences(Address) == 0)
            {
                KIRQL OldIrql = MiAcquirePfnLock();
                MiDeletePde(MiAddressToPde(Address), Process);

                /*
                 * The page-table page(s) just freed become reusable the
                 * moment the PFN lock is released, but other processors may
                 * have re-populated their paging-structure caches for this
                 * region since the per-PTE invalidation above (any access to
                 * a neighboring page in the region re-caches the PDE).  A
                 * stale PDE-cache entry would make their hardware walker
                 * read "PTEs" from the freed, reused frame.  INVLPG
                 * invalidates the paging-structure-cache entries for this
                 * address at every level, so broadcast it while the PFN
                 * lock still prevents the frame from being handed out.
                 */
                MiInvalidateTlbEntryAllProcessors(Address);
                MiReleasePfnLock(OldIrql);
            }
        }

        if (!IsPhysical && OldPte.u.Hard.Valid)
        {
            PMMPFN Pfn1;
            KIRQL OldIrql;

            OldIrql = MiAcquirePfnLock();
            Pfn1 = &MmPfnDatabase[OldPte.u.Hard.PageFrameNumber];

            /*
             * Always mirror the ShareCount bump that
             * MmCreateVirtualMappingUnsafeEx performed for this PTE,
             * regardless of PFN flavour, otherwise every map/unmap cycle
             * on ROS-managed pages leaks +1 ShareCount and silently grows
             * the count past the number of live mappings.
             *
             * For ROS-managed PFNs, however, do NOT flip PageLocation to
             * TransitionPage on the last drop: ROS pages use ReferenceCount
             * via MmDereferencePage for liveness, and a storage MDL can
             * legitimately hold the page while ShareCount falls to zero --
             * setting PageLocation here would trip MmProbeAndLockPages's
             * ActiveAndValid assertion.
             */
            ASSERT(Pfn1->u3.e1.PageLocation == ActiveAndValid);
            ASSERT(Pfn1->u2.ShareCount > 0);
            if (--Pfn1->u2.ShareCount == 0 && !MI_IS_ROS_PFN(Pfn1))
            {
                Pfn1->u3.e1.PageLocation = TransitionPage;
            }
            MiReleasePfnLock(OldIrql);
        }

        MiUnlockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());
    }

    return OldPte.u.Long != 0;
}

_Success_(return)
BOOLEAN
MmDeleteVirtualMapping(
    _Inout_opt_ PEPROCESS Process,
    _In_ PVOID Address,
    _Out_opt_ BOOLEAN * WasDirty,
    _Out_opt_ PPFN_NUMBER Page)
{
    return MmDeleteVirtualMappingEx(Process, Address, WasDirty, Page, FALSE, TRUE);
}

_Success_(return)
BOOLEAN
MmDeletePhysicalMapping(
    _Inout_opt_ PEPROCESS Process,
    _In_ PVOID Address,
    _Out_opt_ BOOLEAN * WasDirty,
    _Out_opt_ PPFN_NUMBER Page)
{
    return MmDeleteVirtualMappingEx(Process, Address, WasDirty, Page, TRUE, TRUE);
}

/*
 * Like MmDeleteVirtualMapping, but only invalidates the local TLB.  The caller
 * is unmapping a batch of pages and MUST issue a cross-processor shootdown
 * (KeFlushEntireTb(TRUE, TRUE)) for the whole run before any of the unmapped
 * frames are released, so that no other processor keeps a stale translation to
 * a page that is about to be reused.
 */
_Success_(return)
BOOLEAN
MmDeleteVirtualMappingNoBroadcast(
    _Inout_opt_ PEPROCESS Process,
    _In_ PVOID Address,
    _Out_opt_ BOOLEAN * WasDirty,
    _Out_opt_ PPFN_NUMBER Page)
{
    return MmDeleteVirtualMappingEx(Process, Address, WasDirty, Page, FALSE, FALSE);
}

VOID
NTAPI
MmDeletePageFileMapping(
    PEPROCESS Process,
    PVOID Address,
    SWAPENTRY* SwapEntry)
{
    PMMPTE PointerPte;
    MMPTE OldPte;

    /* This should not be called for kernel space anymore */
    ASSERT(Process != NULL);
    ASSERT(Address < MmSystemRangeStart);

    /* And we don't support deleting for other process */
    ASSERT(Process == PsGetCurrentProcess());

    /* And we should be at low IRQL */
    ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);

    /* We are tinkering with the PDE here. Ensure it will be there */
    MiLockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());

    /* Callers must ensure there is actually something there */
    ASSERT(MiAddressToPde(Address)->u.Long != 0);

    MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);

    PointerPte = MiAddressToPte(Address);
    OldPte.u.Long = InterlockedExchangePte(PointerPte, 0);
#if DBG
    if (OldPte.u.Long != 0)
        MmTracePte('F', Address, OldPte.u.Long, 0, _ReturnAddress());
#endif
    /* This must be a swap entry ! */
    if (!FlagOn(OldPte.u.Long, 0x800) || OldPte.u.Hard.Valid)
    {
        KeBugCheckEx(MEMORY_MANAGEMENT, OldPte.u.Long, (ULONG_PTR)Process, (ULONG_PTR)Address, 0);
    }

    /* This used to be a non-zero PTE, now we can let the PDE go. */
    if (MiDecrementPageTableReferences(Address) == 0)
    {
        /* We can let it go */
        KIRQL OldIrql = MiAcquirePfnLock();
        MiDeletePde(MiPteToPde(PointerPte), Process);

        /* Same as in MmDeleteVirtualMappingEx: drop stale paging-structure
         * cache entries on all processors before the freed page-table
         * page(s) become reusable at PFN-lock release. */
        MiInvalidateTlbEntryAllProcessors(Address);
        MiReleasePfnLock(OldIrql);
    }

    MiUnlockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());

    *SwapEntry = OldPte.u.Long >> 1;
}

BOOLEAN
NTAPI
MmIsPagePresent(PEPROCESS Process, PVOID Address)
{
    BOOLEAN Ret;

    if (Address >= MmSystemRangeStart)
    {
        ASSERT(Process == NULL);
#if _MI_PAGING_LEVELS == 2
        if (!MiSynchronizeSystemPde(MiAddressToPde(Address)))
#else
        if (!MiIsPdeForAddressValid(Address))
#endif
        {
            /* It can't be present if there is no PDE */
            return FALSE;
        }

        return MiAddressToPte(Address)->u.Hard.Valid;
    }

    ASSERT(Process != NULL);
    ASSERT(Process == PsGetCurrentProcess());

    MiLockProcessWorkingSetShared(Process, PsGetCurrentThread());

    if (!MiIsPageTablePresent(Address))
    {
        /* It can't be present if there is no PDE */
        MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());
        return FALSE;
    }

    MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);

    Ret = MiAddressToPte(Address)->u.Hard.Valid;

    MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());

    return Ret;
}

BOOLEAN
NTAPI
MmIsDisabledPage(PEPROCESS Process, PVOID Address)
{
    BOOLEAN Ret;
    PMMPTE PointerPte;

    if (Address >= MmSystemRangeStart)
    {
        ASSERT(Process == NULL);
#if _MI_PAGING_LEVELS == 2
        if (!MiSynchronizeSystemPde(MiAddressToPde(Address)))
#else
        if (!MiIsPdeForAddressValid(Address))
#endif
        {
            /* It's not disabled if it's not present */
            return FALSE;
        }
    }
    else
    {
        ASSERT(Process != NULL);
        ASSERT(Process == PsGetCurrentProcess());

        MiLockProcessWorkingSetShared(Process, PsGetCurrentThread());

        if (!MiIsPageTablePresent(Address))
        {
            /* It can't be disabled if there is no PDE */
            MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());
            return FALSE;
        }

        MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);
    }

    PointerPte = MiAddressToPte(Address);
    Ret = !PointerPte->u.Hard.Valid
        && !FlagOn(PointerPte->u.Long, 0x800)
        && (PointerPte->u.Hard.PageFrameNumber != 0);

    if (Address < MmSystemRangeStart)
        MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());

    return Ret;
}

BOOLEAN
NTAPI
MmIsPageSwapEntry(PEPROCESS Process, PVOID Address)
{
    BOOLEAN Ret;
    PMMPTE PointerPte;

    /* We never set swap entries for kernel addresses */
    if (Address >= MmSystemRangeStart)
    {
        ASSERT(Process == NULL);
        return FALSE;
    }

    ASSERT(Process != NULL);
    ASSERT(Process == PsGetCurrentProcess());

    MiLockProcessWorkingSetShared(Process, PsGetCurrentThread());

    if (!MiIsPageTablePresent(Address))
    {
        /* There can't be a swap entry if there is no PDE */
        MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());
        return FALSE;
    }

    MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);

    PointerPte = MiAddressToPte(Address);
    Ret = !PointerPte->u.Hard.Valid && FlagOn(PointerPte->u.Long, 0x800);

    MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());

    return Ret;
}

VOID
NTAPI
MmGetPageFileMapping(PEPROCESS Process, PVOID Address, SWAPENTRY* SwapEntry)
{
    PMMPTE PointerPte;

    /* We never set swap entries for kernel addresses */
    if (Address >= MmSystemRangeStart)
    {
        ASSERT(Process == NULL);
        *SwapEntry = 0;
        return;
    }

    ASSERT(Process != NULL);
    ASSERT(Process == PsGetCurrentProcess());

    MiLockProcessWorkingSetShared(Process, PsGetCurrentThread());

    if (!MiIsPageTablePresent(Address))
    {
        /* There can't be a swap entry if there is no PDE */
        MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());
        *SwapEntry = 0;
        return;
    }

    MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);

    PointerPte = MiAddressToPte(Address);
    if (!PointerPte->u.Hard.Valid && FlagOn(PointerPte->u.Long, 0x800))
        *SwapEntry = PointerPte->u.Long >> 1;
    else
        *SwapEntry = 0;

    MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());
}

NTSTATUS
NTAPI
MmCreatePageFileMapping(PEPROCESS Process,
                        PVOID Address,
                        SWAPENTRY SwapEntry)
{
    PMMPTE PointerPte;
    ULONG_PTR Pte;

    /* This should not be called for kernel space anymore */
    ASSERT(Process != NULL);
    ASSERT(Address < MmSystemRangeStart);

    /* And we don't support creating for other process */
    ASSERT(Process == PsGetCurrentProcess());

    if (SwapEntry & ((ULONG_PTR)1 << (RTL_BITS_OF(SWAPENTRY) - 1)))
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    /* We are tinkering with the PDE here. Ensure it will be there */
    ASSERT(Process == PsGetCurrentProcess());
    MiLockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());

    MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);

    PointerPte = MiAddressToPte(Address);
    Pte = InterlockedExchangePte(PointerPte, SwapEntry << 1);
    if (Pte != 0)
    {
        KeBugCheckEx(MEMORY_MANAGEMENT, SwapEntry, (ULONG_PTR)Process, (ULONG_PTR)Address, 0);
    }

    /* This used to be a 0 PTE, now we need a valid PDE to keep it around */
    MiIncrementPageTableReferences(Address);
    MiUnlockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());

    return STATUS_SUCCESS;
}


#if _MI_PAGING_LEVELS >= 3
/*
 * Materialize the page table hierarchy that maps a system address.
 *
 * Legacy Mm hands out kernel virtual addresses from any free gap of the
 * kernel address space, but only the ARM3 ranges get their page directories
 * built at boot time (MiMapPPEs). Storing a PTE for an address outside of
 * those ranges goes through the self-mapping and faults, and the ARM3 fault
 * handler has no way to repair a missing *system* PPE/PDE: it bugchecks with
 * PAGE_FAULT_IN_NONPAGED_AREA. So create whatever is missing beforehand.
 *
 * This is the 3/4-level counterpart of the MiFillSystemPageDirectory() call
 * the two-level code makes for the very same reason.
 */
static
NTSTATUS
MiMakeSystemPageTablesValid(
    _In_ PVOID Address)
{
    PMMPDE Levels[_MI_PAGING_LEVELS - 2];
    ULONG Level, Count = 0;
    KIRQL OldIrql;
    NTSTATUS Status;

    ASSERT(Address >= MmSystemRangeStart);

#if _MI_PAGING_LEVELS == 4
    /*
     * Every system PXE is created once and for all at boot time, in
     * MiInitializePageTable(). It has to be: the top level is per-process, so
     * one created now would only ever be seen by the current address space.
     */
    ASSERT(MiAddressToPxe(Address)->u.Hard.Valid == 1);
    Levels[Count++] = MiAddressToPpe(Address);
#else
    /* PAE has a fixed, boot-time page directory pointer table */
    ASSERT(MiAddressToPpe(Address)->u.Hard.Valid == 1);
#endif
    Levels[Count++] = MiAddressToPde(Address);
    ASSERT(Count == RTL_NUMBER_OF(Levels));

    for (Level = 0; Level < Count; Level++)
    {
        /*
         * Each level is itself addressed through the level above it, so they
         * have to be validated strictly top-down.
         */
        if (Levels[Level]->u.Hard.Valid == 1)
            continue;

        OldIrql = MiAcquirePfnLock();

        /* Re-check now that we are serialized against any other creator */
        if (Levels[Level]->u.Hard.Valid == 0)
        {
            /* System page tables are never paged out, so nothing else fits here */
            ASSERT(Levels[Level]->u.Long == 0);

            /* Page table pages must stay executable: they are never the
             * target of a NoExecute mapping, and MI_WRITE_VALID_PTE asserts
             * on it. */
            Status = MiResolveDemandZeroFault(MiPteToAddress(Levels[Level]),
                                              Levels[Level],
                                              MM_EXECUTE_READWRITE,
                                              NULL,
                                              OldIrql);
            if (!NT_SUCCESS(Status))
            {
                /* The PFN lock was already released for us */
                return Status;
            }

            ASSERT(Levels[Level]->u.Hard.Valid == 1);
        }

        MiReleasePfnLock(OldIrql);
    }

    return STATUS_SUCCESS;
}
#endif /* _MI_PAGING_LEVELS >= 3 */

NTSTATUS
NTAPI
MmCreateVirtualMappingUnsafeEx(
    _Inout_opt_ PEPROCESS Process,
    _In_ PVOID Address,
    _In_ ULONG flProtect,
    _In_ PFN_NUMBER Page,
    _In_ BOOLEAN IsPhysical)
{
    ULONG ProtectionMask;
    PMMPTE PointerPte;
    MMPTE TempPte;
    ULONG_PTR Pte;

    DPRINT("MmCreateVirtualMappingUnsafe(%p, %p, %lu, %x)\n",
           Process, Address, flProtect, Page);

    ASSERT(((ULONG_PTR)Address % PAGE_SIZE) == 0);

    ProtectionMask = MiMakeProtectionMask(flProtect);
    /* Caller must have checked ! */
    ASSERT(ProtectionMask != MM_INVALID_PROTECTION);
    ASSERT(ProtectionMask != MM_NOACCESS);
    ASSERT(ProtectionMask != MM_ZERO_ACCESS);

    /* Make sure our PDE is valid, and that everything is going fine */
    if (Process == NULL)
    {
        /* We don't support this in legacy Mm for kernel mappings */
        ASSERT(ProtectionMask != MM_WRITECOPY);
        ASSERT(ProtectionMask != MM_EXECUTE_WRITECOPY);

        if (Address < MmSystemRangeStart)
        {
            DPRINT1("NULL process given for user-mode mapping at %p\n", Address);
            KeBugCheck(MEMORY_MANAGEMENT);
        }
#if _MI_PAGING_LEVELS == 2
        if (!MiSynchronizeSystemPde(MiAddressToPde(Address)))
            MiFillSystemPageDirectory(Address, PAGE_SIZE);
#else
        NTSTATUS Status = MiMakeSystemPageTablesValid(Address);
        if (!NT_SUCCESS(Status))
            return Status;
#endif
    }
    else
    {
        if ((Address >= MmSystemRangeStart) || Add2Ptr(Address, PAGE_SIZE) >= MmSystemRangeStart)
        {
            DPRINT1("Process %p given for kernel-mode mapping at %p -- 1 page starting at %Ix\n",
                    Process, Address, Page);
            KeBugCheck(MEMORY_MANAGEMENT);
        }

        /* Only for current process !!! */
        ASSERT(Process == PsGetCurrentProcess());
        MiLockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());

        MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);
    }

    PointerPte = MiAddressToPte(Address);

    MI_MAKE_HARDWARE_PTE(&TempPte, PointerPte, ProtectionMask, Page);

    Pte = InterlockedExchangePte(PointerPte, TempPte.u.Long);
    /* There should not have been anything valid here */
    if (Pte != 0)
    {
        DPRINT1("Bad PTE %lx at %p for %p\n", Pte, PointerPte, Address);
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    if (!IsPhysical)
    {
        PMMPFN Pfn1;
        KIRQL OldIrql;

        OldIrql = MiAcquirePfnLock();
        Pfn1 = &MmPfnDatabase[TempPte.u.Hard.PageFrameNumber];
        Pfn1->u2.ShareCount++;
        Pfn1->u3.e1.PageLocation = ActiveAndValid;
        MiReleasePfnLock(OldIrql);
    }

    /* We don't need to flush the TLB here because it only caches valid translations
     * and we're moving this PTE from invalid to valid so it can't be cached right now */

    if (Address < MmSystemRangeStart)
    {
        /* Add PDE reference */
        MiIncrementPageTableReferences(Address);
        MiUnlockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());
    }

    return(STATUS_SUCCESS);
}

NTSTATUS
NTAPI
MmCreateVirtualMappingUnsafe(
    _Inout_opt_ PEPROCESS Process,
    _In_ PVOID Address,
    _In_ ULONG flProtect,
    _In_ PFN_NUMBER Page)
{
    return MmCreateVirtualMappingUnsafeEx(Process, Address, flProtect, Page, FALSE);
}

NTSTATUS
NTAPI
MmCreatePhysicalMapping(
    _Inout_opt_ PEPROCESS Process,
    _In_ PVOID Address,
    _In_ ULONG flProtect,
    _In_ PFN_NUMBER Page)
{
    return MmCreateVirtualMappingUnsafeEx(Process, Address, flProtect, Page, TRUE);
}

NTSTATUS
NTAPI
MmCreateVirtualMapping(PEPROCESS Process,
                       PVOID Address,
                       ULONG flProtect,
                       PFN_NUMBER Page)
{
    ASSERT((ULONG_PTR)Address % PAGE_SIZE == 0);
    if (!MmIsPageInUse(Page))
    {
        DPRINT1("Page %lx is not in use\n", Page);
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    return MmCreateVirtualMappingUnsafe(Process, Address, flProtect, Page);
}

ULONG
NTAPI
MmGetPageProtect(PEPROCESS Process, PVOID Address)
{
    PMMPTE PointerPte;
    ULONG Protect;

    if (Address >= MmSystemRangeStart)
    {
        ASSERT(Process == NULL);

#if _MI_PAGING_LEVELS == 2
        if (!MiSynchronizeSystemPde(MiAddressToPde(Address)))
#else
        if (!MiIsPdeForAddressValid(Address))
#endif
        {
            return PAGE_NOACCESS;
        }
    }
    else
    {
        ASSERT(Address < MmSystemRangeStart);
        ASSERT(Process != NULL);

        ASSERT(Process == PsGetCurrentProcess());

        MiLockProcessWorkingSetShared(Process, PsGetCurrentThread());

        if (!MiIsPageTablePresent(Address))
        {
            /* It can't be present if there is no PDE */
            MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());
            return PAGE_NOACCESS;
        }

        MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);
    }

    PointerPte = MiAddressToPte(Address);

    if (!PointerPte->u.Flush.Valid)
    {
        Protect = PAGE_NOACCESS;
    }
    else
    {
        if (PointerPte->u.Flush.CopyOnWrite)
            Protect = PAGE_WRITECOPY;
        else if (PointerPte->u.Flush.Write)
            Protect = PAGE_READWRITE;
        else
            Protect = PAGE_READONLY;
#if _MI_PAGING_LEVELS >= 3
        /* PAE & AMD64 long mode support NoExecute bit */
        if (!PointerPte->u.Flush.NoExecute)
            Protect <<= 4;
#endif
        if (PointerPte->u.Flush.CacheDisable)
            Protect |= PAGE_NOCACHE;
        if (PointerPte->u.Flush.WriteThrough)
            Protect |= PAGE_WRITETHROUGH;
    }

    if (Address < MmSystemRangeStart)
        MiUnlockProcessWorkingSetShared(Process, PsGetCurrentThread());

    return(Protect);
}

VOID
NTAPI
MmSetPageProtect(PEPROCESS Process, PVOID Address, ULONG flProtect)
{
    ULONG ProtectionMask;
    PMMPTE PointerPte;
    MMPTE TempPte, OldPte;

    DPRINT("MmSetPageProtect(Process %p  Address %p  flProtect %x)\n",
           Process, Address, flProtect);

    ASSERT(Process != NULL);
    ASSERT(Address < MmSystemRangeStart);

    ASSERT(Process == PsGetCurrentProcess());

    ProtectionMask = MiMakeProtectionMask(flProtect);
    /* Caller must have checked ! */
    ASSERT(ProtectionMask != MM_INVALID_PROTECTION);

    MiLockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());

    MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);

    PointerPte = MiAddressToPte(Address);

    /* Sanity check */
    ASSERT(PointerPte->u.Hard.Owner == 1);

    TempPte.u.Long = 0;
    TempPte.u.Hard.PageFrameNumber = PointerPte->u.Hard.PageFrameNumber;
    TempPte.u.Long |= MmProtectToPteMask[ProtectionMask];
    TempPte.u.Hard.Owner = 1;

    /* Only set valid bit if we have to */
    if ((ProtectionMask != MM_NOACCESS) && !FlagOn(ProtectionMask, MM_GUARDPAGE))
        TempPte.u.Hard.Valid = 1;

    /* Keep dirty & accessed bits.  They must be captured and installed in
     * one atomic step: the other processor's page walker sets Dirty with a
     * locked RMW at any time, and a plain exchange built from a stale
     * snapshot would erase that set -- pageout then discards a dirty page
     * as clean and its modifications are lost. */
    do
    {
        OldPte.u.Long = PointerPte->u.Long;
        TempPte.u.Hard.Accessed = OldPte.u.Hard.Accessed;
        TempPte.u.Hard.Dirty = OldPte.u.Hard.Dirty;
    } while (InterlockedCompareExchangePte(PointerPte, TempPte.u.Long, OldPte.u.Long) != (LONG_PTR)OldPte.u.Long);

    // We should be able to bring a page back from PAGE_NOACCESS
    if (!OldPte.u.Hard.Valid && (FlagOn(OldPte.u.Long, 0x800) || (OldPte.u.Hard.PageFrameNumber == 0)))
    {
        DPRINT1("Invalid Pte %lx\n", OldPte.u.Long);
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    if (OldPte.u.Long != TempPte.u.Long)
        KeInvalidateTlbEntry(Address);

    MiUnlockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());
}

VOID
NTAPI
MmSetDirtyBit(PEPROCESS Process, PVOID Address, BOOLEAN Bit)
{
    PMMPTE PointerPte;

    DPRINT("MmSetDirtyBit(Process %p  Address %p  Bit %x)\n",
           Process, Address, Bit);

    ASSERT(Process != NULL);
    ASSERT(Address < MmSystemRangeStart);

    ASSERT(Process == PsGetCurrentProcess());

    MiLockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());

    MiMakePdeExistAndMakeValid(MiAddressToPde(Address), Process, MM_NOIRQL);

    PointerPte = MiAddressToPte(Address);
    // We shouldnl't set dirty bit on non-mapped addresses
    if (!PointerPte->u.Hard.Valid && (FlagOn(PointerPte->u.Long, 0x800) || (PointerPte->u.Hard.PageFrameNumber == 0)))
    {
        DPRINT1("Invalid Pte %lx\n", PointerPte->u.Long);
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    /* Interlocked: a plain bitfield RMW on a live PTE races the hardware's
     * locked Accessed/Dirty updates from the other processor. */
    {
        MMPTE OldPte, NewPte;
        do
        {
            OldPte.u.Long = PointerPte->u.Long;
            NewPte = OldPte;
            NewPte.u.Hard.Dirty = !!Bit;
        } while (InterlockedCompareExchangePte(PointerPte, NewPte.u.Long, OldPte.u.Long) != (LONG_PTR)OldPte.u.Long);
    }

    if (!Bit)
        KeInvalidateTlbEntry(Address);

    MiUnlockProcessWorkingSetUnsafe(Process, PsGetCurrentThread());
}

CODE_SEG("INIT")
VOID
NTAPI
MmInitGlobalKernelPageDirectory(VOID)
{
    /* Nothing to do here */
}

#ifdef _M_IX86
BOOLEAN
Mmi386MakeKernelPageTableGlobal(PVOID Address)
{
    PMMPDE PointerPde = MiAddressToPde(Address);
    PMMPTE PointerPte = MiAddressToPte(Address);

    if (PointerPde->u.Hard.Valid == 0)
    {
        if (!MiSynchronizeSystemPde(PointerPde))
            return FALSE;
        return PointerPte->u.Hard.Valid != 0;
    }
    return FALSE;
}
#endif

/* EOF */
