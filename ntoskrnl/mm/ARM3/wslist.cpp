/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD-3-Clause (https://spdx.org/licenses/BSD-3-Clause)
 * FILE:            ntoskrnl/mm/ARM3/wslist.cpp
 * PURPOSE:         Working set list management
 * PROGRAMMERS:     Jérôme Gardou
 */

/* INCLUDES *******************************************************************/
#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include "miarm.h"

/* GLOBALS ********************************************************************/
PMMWSL MmWorkingSetList;
KEVENT MmWorkingSetManagerEvent;

/* How many pages one visit to a process may take away from it. They go out in
 * MI_PAGEFILE_WRITE_CLUSTER sized writes, so this is a handful of IRPs -- big
 * enough to be worth the visit, small enough that the process is not kept out
 * of its own working set for long. */
#define MI_TRIM_CLUSTER_SIZE 128

/* How many times MmWorkingSetManager may sweep the process list before
 * returning to its caller, so that a pathological allocator cannot keep the
 * pager here indefinitely. */
#define MI_TRIM_MAX_PASSES 64

/* How many working set entries one visit to a process may examine. The whole
 * scan runs under that process' working set lock, and a working set can hold
 * as many entries as the machine has pages. */
#define MI_TRIM_MAX_SCAN 8192

/* How many visits a page survives untouched before it may be taken. */
#define MI_TRIM_MIN_AGE 3

/* How many available pages the pager works towards. The same watermark that
 * wakes it from the allocation path, so that it stops exactly where it was
 * asked to start rather than chasing a second, unrelated number. */
static PFN_NUMBER MiTrimGoalPages(VOID)
{
    return MmPagerReserve;
}

/* LOCAL FUNCTIONS ************************************************************/

static MMPTE GetPteTemplateForWsList(PMMWSL WsList)
{
    return (WsList == MmSystemCacheWorkingSetList) ? ValidKernelPte : ValidKernelPteLocal;
}

static ULONG GetNextPageColorForWsList(PMMWSL WsList)
{
    return (WsList == MmSystemCacheWorkingSetList) ? MI_GET_NEXT_COLOR() : MI_GET_NEXT_PROCESS_COLOR(PsGetCurrentProcess());
}

/* Releasing the tail of the WSLE array takes the PFN lock. Callers that
 * already hold it (the page-deletion path) must pass CanShrink = FALSE;
 * the array is then simply left at its current size and shrinks on the
 * next unlocked removal. */
static void FreeWsleIndex(PMMWSL WsList, ULONG Index, BOOLEAN CanShrink)
{
    PMMWSLE Wsle = WsList->Wsle;
    ULONG& LastEntry = WsList->LastEntry;
    ULONG& FirstFree = WsList->FirstFree;
    ULONG& LastInitializedWsle = WsList->LastInitializedWsle;

    /* Erase it now */
    Wsle[Index].u1.Long = 0;

    if (Index == (LastEntry - 1))
    {
        /* We're freeing the last index of our list. */
        while (Wsle[Index].u1.e1.Valid == 0)
            Index--;

        /* Should we bother about the Free entries */
        if (FirstFree < Index)
        {
            /* Try getting the index of the last free entry */
            ASSERT(Wsle[Index + 1].u1.Free.MustBeZero == 0);
            ULONG PreviousFree = Wsle[Index + 1].u1.Free.PreviousFree;
            ASSERT(PreviousFree < LastEntry);
            ULONG LastFree = Index + 1 - PreviousFree;
#ifdef MMWSLE_PREVIOUS_FREE_JUMP
            while (Wsle[LastFree].u1.e1.Valid)
            {
                ASSERT(LastFree > MMWSLE_PREVIOUS_FREE_JUMP);
                LastFree -= MMWSLE_PREVIOUS_FREE_JUMP;
            }
#endif
            /* Update */
            ASSERT(LastFree >= FirstFree);
            Wsle[FirstFree].u1.Free.PreviousFree = (Index + 1 - LastFree) & MMWSLE_PREVIOUS_FREE_MASK;
            Wsle[LastFree].u1.Free.NextFree = 0;
        }
        else
        {
            /* No more free entries in our array */
            FirstFree = ULONG_MAX;
        }
        /* This is the new size of our array */
        LastEntry = Index + 1;
        /* Should we shrink the alloc? */
        while (CanShrink &&
               ((LastInitializedWsle - LastEntry) > (PAGE_SIZE / sizeof(MMWSLE))))
        {
            PMMPTE PointerPte = MiAddressToPte(Wsle + LastInitializedWsle - 1);
            /* We must not free ourself! */
            ASSERT(MiPteToAddress(PointerPte) != WsList);

            PFN_NUMBER Page = PFN_FROM_PTE(PointerPte);

            {
                ntoskrnl::MiPfnLockGuard PfnLock;

                PMMPFN Pfn = MiGetPfnEntry(Page);
                MI_SET_PFN_DELETED(Pfn);
                MiDecrementShareCount(MiGetPfnEntry(Pfn->u4.PteFrame), Pfn->u4.PteFrame);
                MiDecrementShareCount(Pfn, Page);
            }

#if DBG
            {
                ULONG_PTR _oldpte = PointerPte->u.Long;
                PointerPte->u.Long = 0;
                MmTracePte('W', MiPteToAddress(PointerPte), _oldpte, 0, _ReturnAddress());
            }
#else
            PointerPte->u.Long = 0;
#endif

            KeInvalidateTlbEntry(Wsle + LastInitializedWsle - 1);
            LastInitializedWsle -= PAGE_SIZE / sizeof(MMWSLE);
        }
        return;
    }

    if (FirstFree == ULONG_MAX)
    {
        /* We're the first one. */
        FirstFree = Index;
        Wsle[FirstFree].u1.Free.PreviousFree = (LastEntry - FirstFree) & MMWSLE_PREVIOUS_FREE_MASK;
        return;
    }

    /* We must find where to place ourself */
    ULONG NextFree = FirstFree;
    ULONG PreviousFree = 0;
    while (NextFree < Index)
    {
        ASSERT(Wsle[NextFree].u1.Free.MustBeZero == 0);
        if (Wsle[NextFree].u1.Free.NextFree == 0)
            break;
        PreviousFree = NextFree;
        NextFree += Wsle[NextFree].u1.Free.NextFree;
    }

    if (NextFree < Index)
    {
        /* This is actually the last free entry */
        Wsle[NextFree].u1.Free.NextFree = Index - NextFree;
        Wsle[Index].u1.Free.PreviousFree = (Index - NextFree) & MMWSLE_PREVIOUS_FREE_MASK;
        Wsle[FirstFree].u1.Free.PreviousFree = (LastEntry - Index) & MMWSLE_PREVIOUS_FREE_MASK;
        return;
    }

    if (PreviousFree == 0)
    {
        /* This is the first free */
        Wsle[Index].u1.Free.NextFree = FirstFree - Index;
        Wsle[Index].u1.Free.PreviousFree = Wsle[FirstFree].u1.Free.PreviousFree;
        Wsle[FirstFree].u1.Free.PreviousFree = (FirstFree - Index) & MMWSLE_PREVIOUS_FREE_MASK;
        FirstFree = Index;
        return;
    }

    /* Insert */
    Wsle[PreviousFree].u1.Free.NextFree = (Index - PreviousFree);
    Wsle[Index].u1.Free.PreviousFree = (Index - PreviousFree) & MMWSLE_PREVIOUS_FREE_MASK;
    Wsle[Index].u1.Free.NextFree = NextFree - Index;
    Wsle[NextFree].u1.Free.PreviousFree = (NextFree - Index) & MMWSLE_PREVIOUS_FREE_MASK;
}

/*
 * How far the WSLE array may grow.
 *
 * It used to stop at the end of the single hyperspace page table that maps the
 * MMWSL -- around 130 000 entries, half a gigabyte of pages. Every page a
 * process mapped beyond that was left out of its working set and therefore
 * could never be trimmed, which is how a 4 GB machine ran itself out of memory
 * with a 4 GB page file barely touched. Where the array is allowed to span
 * page tables (GrowWslePageTable builds them on demand) the only real limits
 * are hyperspace itself and the fact that a working set cannot hold more pages
 * than the machine has.
 */
static ULONG MaxWsleForWsList(PMMWSL WsList)
{
    ULONG_PTR Start = (ULONG_PTR)(WsList + 1);
    ULONG_PTR End;

#if (_MI_PAGING_LEVELS == 4)
    /* Stay within the page directory that hyperspace already owns: creating a
     * PPE here is a problem we do not need, and one page directory spans a
     * gigabyte of address space -- far more entries than there are pages of
     * RAM to put in them. */
    ULONG_PTR PdRegion = (ULONG_PTR)PTE_PER_PAGE * PTE_PER_PAGE * PAGE_SIZE;

    End = ((ULONG_PTR)WsList & ~(PdRegion - 1)) + PdRegion;
    if (End > (ULONG_PTR)HYPER_SPACE_END + 1)
        End = (ULONG_PTR)HYPER_SPACE_END + 1;
#else
    /* Elsewhere the array stays in the page table it started in. */
    End = ((ULONG_PTR)WsList | (PTE_PER_PAGE * PAGE_SIZE - 1)) + 1;
#endif

    SIZE_T Max = (End - Start) / sizeof(MMWSLE);

    /* A working set can never hold more pages than physically exist. */
    if (Max > MmNumberOfPhysicalPages)
        Max = MmNumberOfPhysicalPages;

    return (ULONG)Max;
}

/*
 * Builds the page table for the next chunk of the WSLE array.
 *
 * The array may outgrow the page table that maps the MMWSL, so the next one
 * has to exist before any of its PTEs can even be read: walking through an
 * invalid PDE would fault, and the caller is about to take the PFN lock.
 * Returning FALSE simply leaves the caller's page out of the working set.
 */
static BOOLEAN GrowWslePageTable(PMMWSL WsList, PVOID WsleAddress)
{
    PMMPDE PointerPde = MiAddressToPde(WsleAddress);

    if (PointerPde->u.Hard.Valid == 1)
        return TRUE;

    MMPDE TempPde = (WsList == MmSystemCacheWorkingSetList)
                  ? ValidKernelPde : ValidKernelPdeLocal;

    ntoskrnl::MiPfnLockGuard PfnLock;

    /* Zeroed, so that every PTE in the new table reads invalid. */
    PFN_NUMBER Page = MiRemoveZeroPage(GetNextPageColorForWsList(WsList));
    if (Page == 0)
        return FALSE;

    TempPde.u.Hard.PageFrameNumber = Page;
    MiInitializePfnAndMakePteValid(Page, PointerPde, TempPde);

    return TRUE;
}

/* Returns ULONG_MAX when no entry can be handed out. The caller then simply
 * leaves the page out of the working set: it stays where it is, exactly as
 * every page did before the working set list was populated at all. */
static ULONG GetFreeWsleIndex(PMMWSL WsList)
{
    ULONG Index;
    if (WsList->FirstFree != ULONG_MAX)
    {
        Index = WsList->FirstFree;
        ASSERT(Index < WsList->LastInitializedWsle);
        MMWSLE_FREE_ENTRY& FreeWsle = WsList->Wsle[Index].u1.Free;
        ASSERT(FreeWsle.MustBeZero == 0);
        if (FreeWsle.NextFree != 0)
        {
            WsList->FirstFree += FreeWsle.NextFree;
            WsList->Wsle[WsList->FirstFree].u1.Free.PreviousFree = FreeWsle.PreviousFree;
        }
        else
        {
            WsList->FirstFree = ULONG_MAX;
        }
    }
    else
    {
        Index = WsList->LastEntry;
        if (Index >= WsList->LastInitializedWsle)
        {
            /* Grow our array */
            if (Index >= MaxWsleForWsList(WsList))
                return ULONG_MAX;

            PVOID WsleAddress = &WsList->Wsle[WsList->LastInitializedWsle];

            /* The array can cross out of the page table that maps the MMWSL,
             * so make sure the next one exists before reading its PTEs. */
            if (!GrowWslePageTable(WsList, WsleAddress))
                return ULONG_MAX;

            PMMPTE PointerPte = MiAddressToPte(WsleAddress);
            ASSERT(PointerPte->u.Hard.Valid == 0);
            MMPTE TempPte = GetPteTemplateForWsList(WsList);
            {
                ntoskrnl::MiPfnLockGuard PfnLock;

                PFN_NUMBER Page = MiRemoveAnyPage(GetNextPageColorForWsList(WsList));
                if (Page == 0)
                {
                    /* Growing the working set list is not worth failing a
                     * page fault over: we are being asked to remember a page
                     * at the exact moment there is no page to remember it
                     * with. */
                    return ULONG_MAX;
                }

                TempPte.u.Hard.PageFrameNumber = Page;
                MiInitializePfnAndMakePteValid(Page, PointerPte, TempPte);
            }

            WsList->LastInitializedWsle += PAGE_SIZE / sizeof(MMWSLE);
        }
        WsList->LastEntry++;
    }

    WsList->Wsle[Index].u1.Long = 0;
    return Index;
}

static
VOID
RemoveFromWsList(PMMWSL WsList, PVOID Address, BOOLEAN CanShrink)
{
    /* Make sure that we are holding the right locks. */
    ASSERT(MM_ANY_WS_LOCK_HELD_EXCLUSIVE(PsGetCurrentThread()));

    PMMPTE PointerPte = MiAddressToPte(Address);

    /* Make sure we are removing a paged-in address */
    ASSERT(PointerPte->u.Hard.Valid == 1);
    PMMPFN Pfn1 = MiGetPfnEntry(PFN_FROM_PTE(PointerPte));
    ASSERT(Pfn1->u3.e1.PageLocation == ActiveAndValid);

    /* Shared pages not supported yet */
    ASSERT(Pfn1->u3.e1.PrototypePte == 0);

    /* Nor are "ROS PFN" */
    ASSERT(MI_IS_ROS_PFN(Pfn1) == FALSE);

    /* And we should have a valid index here */
    ASSERT(Pfn1->u1.WsIndex != 0);

    FreeWsleIndex(WsList, Pfn1->u1.WsIndex, CanShrink);

    /* u1 is a union: leaving a stale index behind would be read back as an
     * in-page event pointer or a list link once the page leaves the working
     * set. */
    Pfn1->u1.WsIndex = 0;
}

static
ULONG
TrimWsList(PMMWSL WsList, PFN_NUMBER *Victims, ULONG MaxVictims, BOOLEAN Aggressive,
           PULONG ScannedOut)
{
    /* This should be done under WS lock */
    ASSERT(MM_ANY_WS_LOCK_HELD(PsGetCurrentThread()));

    ULONG Ret = 0;
    ULONG FirstDynamic = WsList->FirstDynamic;
    ULONG Scanned;

    /* Resume where the last visit stopped. Restarting at FirstDynamic every
     * time meant re-examining the head of the list over and over while the
     * rest of a large working set was never even looked at. */
    ULONG i = WsList->NextSlot;
    if ((i < FirstDynamic) || (i >= WsList->LastEntry))
        i = FirstDynamic;

    ULONG Limit = (WsList->LastEntry > FirstDynamic)
                ? (WsList->LastEntry - FirstDynamic) : 0;

    /* Cap the work of one visit. Most of a sweep yields no victim at all --
     * clearing accessed bits so that the *next* sweep can tell hot pages from
     * cold ones is the whole point of it -- and all of it runs under the
     * working set lock. NextSlot carries the cursor to the next visit. */
    if (Limit > MI_TRIM_MAX_SCAN)
        Limit = MI_TRIM_MAX_SCAN;

    /* Walk the array, wrapping once around at most */
    for (Scanned = 0; (Scanned < Limit) && (Ret < MaxVictims); Scanned++)
    {
        /* Taking entries away shrinks LastEntry underneath us, so re-check the
         * cursor on every step rather than trusting the bound we started with. */
        if (i >= WsList->LastEntry)
        {
            i = FirstDynamic;
            if (i >= WsList->LastEntry)
                break;
        }

        ULONG Current = i++;

        MMWSLE& Entry = WsList->Wsle[Current];
        if (!Entry.u1.e1.Valid)
            continue;

        /* Only direct entries for now */
        ASSERT(Entry.u1.e1.Direct == 1);

        /* Check the PTE */
        PMMPTE PointerPte = MiAddressToPte(Entry.u1.VirtualAddress);

        /* This must be valid */
        ASSERT(PointerPte->u.Hard.Valid);

        /*
         * If the PTE was accessed since the last visit, clear the bit and
         * leave the page alone.
         *
         * This is not negotiable however short of memory we are. Taking a page
         * that is in use only writes it out so that its owner can fault it
         * straight back in -- and while it is in flight every fault on it
         * blocks in MiResolveTransitionFault. Doing that under pressure was
         * measured at a gigabyte written to the page file for thirty-seven
         * megabytes of it that stayed there, with the machine making no
         * forward progress at all.
         */
        if (PointerPte->u.Hard.Accessed)
        {
            Entry.u1.e1.Age = 0;
            PointerPte->u.Hard.Accessed = 0;
            KeInvalidateTlbEntry(Entry.u1.VirtualAddress);
            continue;
        }

        /* Untouched since the last visit. Normally it has to stay untouched
         * for a few visits running before we believe it, but a real shortage
         * cannot afford to wait for that: one clean sweep is the evidence we
         * get. */
        if (!Aggressive && (Entry.u1.e1.Age < MI_TRIM_MIN_AGE))
        {
            Entry.u1.e1.Age++;
            continue;
        }

        if ((Entry.u1.e1.LockedInMemory) || (Entry.u1.e1.LockedInWs))
        {
            /* This one is locked. Next time, maybe... */
            continue;
        }

        /* FIXME: Invalidating PDEs breaks legacy MMs */
        if (MI_IS_PAGE_TABLE_ADDRESS(Entry.u1.VirtualAddress))
            continue;

        /* Please put yourself aside and make place for the younger ones */
        PFN_NUMBER Page = PFN_FROM_PTE(PointerPte);
        PMMPFN Pfn = MiGetPfnEntry(Page);

        /* Only private, page-file backed pages can be written out by the
         * caller. Shared (prototype) pages and legacy ReactOS PFNs belong to
         * the section/rmap pager, so leave them where they are. */
        if ((Pfn->u3.e1.PrototypePte != 0) || MI_IS_ROS_PFN(Pfn))
            continue;

        /* FIXME: Remove this hack when possible */
        if (Pfn->Wsle.u1.e1.LockedInMemory || (Pfn->Wsle.u1.e1.LockedInWs))
            continue;

        /* Save what we need before the WSLE is erased. The protection comes
         * from the PFN, not from the WSLE: MiProtectVirtualMemory keeps
         * OriginalPte up to date for private pages but has no reason to know
         * about our copy, which would go stale under VirtualProtect. */
        ULONG Protection = Pfn->OriginalPte.u.Soft.Protection;
        PVOID VirtualAddress = PAGE_ALIGN(Entry.u1.VirtualAddress);

        {
            ntoskrnl::MiPfnLockGuard PfnLock;

            /* A page that somebody else holds a reference to -- an in-flight
             * DMA transfer, a probed-and-locked buffer -- must stay where it
             * is. Only the mapping we are about to take away may reference
             * it. This has to be decided under the PFN lock, together with
             * the removal itself. */
            if ((Pfn->u2.ShareCount != 1) || (Pfn->u3.e2.ReferenceCount != 1))
                continue;

            /* Take it out of the working set while the PTE is still valid:
             * the removal path validates the mapping it is handed. It must
             * not give the tail of the WSLE array back here, because that
             * would try to take the PFN lock we are already holding. */
            RemoveFromWsList(WsList, VirtualAddress, FALSE);

            /* Dirtify the page, if needed */
            if (PointerPte->u.Hard.Dirty)
                Pfn->u3.e1.Modified = 1;

            /* Make this a transition PTE */
            MI_MAKE_TRANSITION_PTE(PointerPte, Page, Protection);

            /* Hand the page to the pager rather than to a page list: a
             * private page has no backing store yet, so it must reach the
             * page file before it can be reused. We keep the single
             * reference the mapping held, mark the write as pending and take
             * the page off the address space. A fault on the transition PTE
             * now blocks in MiResolveTransitionFault until the write-out
             * publishes the page-file PTE. */
            Pfn->u2.ShareCount = 0;
            Pfn->u3.e1.PageLocation = TransitionPage;
            Pfn->u3.e1.Modified = 1;
            Pfn->u3.e1.WriteInProgress = 1;
        }

        /* Other processors may still hold a TLB entry for this address; they
         * never walk the page tables again on their own, so a local
         * invalidation would let them keep writing to a page we are about to
         * write out and free. */
        MiInvalidateTlbEntryAllProcessors(VirtualAddress);

        Victims[Ret++] = Page;
    }

    /* Next visit carries on from here */
    WsList->NextSlot = i;

    *ScannedOut = Scanned;
    return Ret;
}

/*
 * Ends the pager's ownership of a page taken by TrimWsList.
 *
 * WrittenOut says whether SwapEntry now holds a good copy of the page. If it
 * does, the mapping becomes a page-file PTE and the page is freed; if it does
 * not, the page goes onto the modified list, still reachable through its
 * transition PTE.
 */
static
VOID
MiReleaseTrimmedPage(PFN_NUMBER Page, SWAPENTRY SwapEntry, BOOLEAN WrittenOut)
{
    ntoskrnl::MiPfnLockGuard PfnLock;

    PMMPFN Pfn = MiGetPfnEntry(Page);

    ASSERT(Pfn->u3.e1.WriteInProgress == 1);
    ASSERT(Pfn->u2.ShareCount == 0);
    ASSERT(Pfn->u3.e2.ReferenceCount == 1);

    Pfn->u3.e1.WriteInProgress = 0;

    /* Wake whoever faulted on the transition PTE while we were writing */
    if (Pfn->u1.Event != NULL)
    {
        KeSetEvent(Pfn->u1.Event, IO_NO_INCREMENT, FALSE);
        Pfn->u1.Event = NULL;
    }

    if (MI_IS_PFN_DELETED(Pfn))
    {
        /* The mapping went away while we were writing. MiDeletePte saw our
         * outstanding reference and left the freeing to us; the saved
         * contents are worthless now. */
        if (SwapEntry != 0)
            MmFreeSwapPage(SwapEntry);
        MiDecrementReferenceCount(Pfn, Page);
        return;
    }

    if (!WrittenOut)
    {
        /* Nothing was saved, so the page must stay reachable. Its transition
         * PTE is still in place, so a fault will pull it back off this
         * list. */
        ASSERT(SwapEntry == 0);
        Pfn->u3.e2.ReferenceCount = 0;
        MiInsertPageInList(&MmModifiedPageListHead, Page);
        return;
    }

    PMMPTE PointerPte = Pfn->PteAddress;
    ASSERT(PointerPte->u.Hard.Valid == 0);
    ASSERT(PointerPte->u.Soft.Transition == 1);
    ASSERT(PointerPte->u.Trans.PageFrameNumber == Page);

    /* Publish the page file PTE. MiReadPageFile normalises the slot number
     * itself, so what has to be stored is the *biased* offset -- and a
     * PageFileHigh of zero is exactly what means "not paged out". */
    MMPTE TempPte;
    TempPte.u.Long = 0;
    TempPte.u.Soft.Protection = PointerPte->u.Trans.Protection;
    TempPte.u.Soft.PageFileLow = FILE_FROM_ENTRY(SwapEntry);
    TempPte.u.Soft.PageFileHigh = OFFSET_FROM_ENTRY(SwapEntry);
    ASSERT(TempPte.u.Soft.PageFileHigh != 0);
    MI_WRITE_INVALID_PTE(PointerPte, TempPte);

    /* The transition PTE held a share on the page table; the page-file PTE
     * does not reference the page anymore. The PTE update dirtied the page
     * table itself. Keep an empty private page table on the modified path,
     * since the standby path only supports prototype-backed PFNs. */
    PMMPFN PtePfn = MiGetPfnEntry(Pfn->u4.PteFrame);
    PtePfn->u3.e1.Modified = 1;
    MiDecrementShareCount(PtePfn, Pfn->u4.PteFrame);

    /* And now the page itself is free */
    MI_SET_PFN_DELETED(Pfn);
    Pfn->u3.e1.Modified = 0;
    MiDecrementReferenceCount(Pfn, Page);
}

/*
 * Writes pages handed over by TrimWsList to the page file and frees them.
 *
 * Must run at PASSIVE_LEVEL, attached to the process that owns the pages and
 * with its working set lock dropped: the page file write is synchronous, and
 * Pfn->PteAddress only resolves in the owning address space.
 */
static
ULONG
WriteTrimmedPages(PFN_NUMBER *Victims, ULONG Count)
{
    ULONG Written = 0;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    ASSERT(!MM_ANY_WS_LOCK_HELD(PsGetCurrentThread()));

    ULONG i = 0;

    while (i < Count)
    {
        ULONG Batch = min(Count - i, (ULONG)MI_PAGEFILE_WRITE_CLUSTER);
        SWAPENTRY FirstEntry;
        NTSTATUS Status;

        /* One write for as many pages as the page file will give us in a row.
         * A fragmented file yields a shorter run, never none while there is
         * space left. */
        ULONG Got = MiAllocSwapPageRun(Batch, &FirstEntry);
        if (Got == 0)
        {
            /* Out of page file space. Park the pages on the modified list so a
             * fault can still bring them back, and stop: the next victims
             * would fail the same way. */
            MmShowOutOfSpaceMessagePagingFile();
            for (; i < Count; i++)
                MiReleaseTrimmedPage(Victims[i], 0, FALSE);
            break;
        }

        Status = MmWriteToSwapPages(FirstEntry, &Victims[i], Got);

        if ((Status == STATUS_DEVICE_DATA_ERROR) && (Got > 1))
        {
            /* The file system wrote less than the cluster it was handed. The
             * slots are ours and inside the file, so nothing is wrong except
             * the size of the request -- reissue it one page at a time, which
             * is the shape that has always worked. */
            Status = STATUS_SUCCESS;
            for (ULONG j = 0; j < Got; j++)
            {
                SWAPENTRY One = ENTRY_FROM_FILE_OFFSET(FILE_FROM_ENTRY(FirstEntry),
                                                       OFFSET_FROM_ENTRY(FirstEntry) + j);
                NTSTATUS OneStatus = MmWriteToSwapPage(One, Victims[i + j]);

                if (NT_SUCCESS(OneStatus))
                {
                    MiReleaseTrimmedPage(Victims[i + j], One, TRUE);
                    Written++;
                }
                else
                {
                    MmFreeSwapPage(One);
                    MiReleaseTrimmedPage(Victims[i + j], 0, FALSE);
                }
            }
            i += Got;
            continue;
        }

        for (ULONG j = 0; j < Got; j++)
        {
            /* The slots of a run are consecutive, so the entry for each page
             * follows from the first one. */
            SWAPENTRY Entry = ENTRY_FROM_FILE_OFFSET(FILE_FROM_ENTRY(FirstEntry),
                                                     OFFSET_FROM_ENTRY(FirstEntry) + j);

            if (NT_SUCCESS(Status))
            {
                /* The contents are safe on disk now: publish the page file PTE
                 * and give the page back. */
                MiReleaseTrimmedPage(Victims[i + j], Entry, TRUE);
                Written++;
            }
            else
            {
                MmFreeSwapPage(Entry);
                MiReleaseTrimmedPage(Victims[i + j], 0, FALSE);
            }
        }

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to write %lu pages to the page file: 0x%08lx\n",
                    Got, Status);
        }

        i += Got;
    }

    return Written;
}

/* GLOBAL FUNCTIONS ***********************************************************/
extern "C"
{

_Use_decl_annotations_
VOID
NTAPI
MiInsertInWorkingSetList(
    _Inout_ PMMSUPPORT Vm,
    _In_ PVOID Address,
    _In_ ULONG Protection)
{
    PMMWSL WsList = Vm->VmWorkingSetList;

    /* Make sure that we are holding the WS lock. */
    ASSERT(MM_ANY_WS_LOCK_HELD_EXCLUSIVE(PsGetCurrentThread()));

    PMMPTE PointerPte = MiAddressToPte(Address);

    /* Make sure we are adding a paged-in address */
    ASSERT(PointerPte->u.Hard.Valid == 1);
    PMMPFN Pfn1 = MiGetPfnEntry(PFN_FROM_PTE(PointerPte));
    ASSERT(Pfn1->u3.e1.PageLocation == ActiveAndValid);

    /* Shared pages not supported yet */
    ASSERT(Pfn1->u1.WsIndex == 0);
    ASSERT(Pfn1->u3.e1.PrototypePte == 0);

    /* Nor are "ROS PFN" */
    ASSERT(MI_IS_ROS_PFN(Pfn1) == FALSE);

    ULONG Index = GetFreeWsleIndex(WsList);
    if (Index == ULONG_MAX)
    {
        /* No room. The page keeps working, it just cannot be trimmed. */
        return;
    }

    Pfn1->u1.WsIndex = Index;
    MMWSLENTRY& NewWsle = WsList->Wsle[Pfn1->u1.WsIndex].u1.e1;
    NewWsle.VirtualPageNumber = reinterpret_cast<ULONG_PTR>(Address) >> PAGE_SHIFT;
    NewWsle.Protection = Protection;
    NewWsle.Direct = 1;
    NewWsle.Hashed = 0;
    NewWsle.LockedInMemory = 0;
    NewWsle.LockedInWs = 0;
    NewWsle.Age = 0;
    NewWsle.Valid = 1;

    Vm->WorkingSetSize += PAGE_SIZE;
    if (Vm->WorkingSetSize > Vm->PeakWorkingSetSize)
        Vm->PeakWorkingSetSize = Vm->WorkingSetSize;
}

_Use_decl_annotations_
VOID
NTAPI
MiRemoveFromWorkingSetList(
    _Inout_ PMMSUPPORT Vm,
    _In_ PVOID Address)
{
    RemoveFromWsList(Vm->VmWorkingSetList, Address, TRUE);

    Vm->WorkingSetSize -= PAGE_SIZE;
}

_Use_decl_annotations_
VOID
NTAPI
MiRemoveFromWorkingSetListPfnHeld(
    _Inout_ PMMSUPPORT Vm,
    _In_ PVOID Address)
{
    /* Same thing, but the caller already owns the PFN lock, so the WSLE array
     * must not try to give its tail back. */
    RemoveFromWsList(Vm->VmWorkingSetList, Address, FALSE);

    Vm->WorkingSetSize -= PAGE_SIZE;
}

_Use_decl_annotations_
VOID
NTAPI
MiDestroyWorkingSetList(_Inout_ PMMSUPPORT Vm)
{
    PMMWSL WsList = Vm->VmWorkingSetList;

    ASSERT(MM_ANY_WS_LOCK_HELD_EXCLUSIVE(PsGetCurrentThread()));

    /* The first page of the array shares the page holding the MMWSL itself,
     * which process teardown frees separately as Process->WorkingSetPage.
     * Only what was grown on top of it belongs here. The page tables are not
     * freed explicitly: every page freed drops a reference on the table that
     * maps it, so a table goes away with its last entry. */
    ULONG_PTR Address = (ULONG_PTR)WsList + PAGE_SIZE;
    ULONG_PTR End = (ULONG_PTR)&WsList->Wsle[WsList->LastInitializedWsle];

    while (Address < End)
    {
        PMMPDE PointerPde = MiAddressToPde((PVOID)Address);

        /* Whole 2 MB chunks of the range may never have been built. */
        if (PointerPde->u.Hard.Valid == 0)
        {
            Address = (Address | (PTE_PER_PAGE * PAGE_SIZE - 1)) + 1;
            continue;
        }

        PMMPTE PointerPte = MiAddressToPte((PVOID)Address);
        if (PointerPte->u.Hard.Valid == 1)
        {
            PFN_NUMBER Page = PFN_FROM_PTE(PointerPte);

            {
                ntoskrnl::MiPfnLockGuard PfnLock;

                PMMPFN Pfn = MiGetPfnEntry(Page);
                MI_SET_PFN_DELETED(Pfn);
                MiDecrementShareCount(MiGetPfnEntry(Pfn->u4.PteFrame), Pfn->u4.PteFrame);
                MiDecrementShareCount(Pfn, Page);
            }

            PointerPte->u.Long = 0;
            KeInvalidateTlbEntry((PVOID)Address);
        }

        Address += PAGE_SIZE;
    }

    /* This is all the array that is still backed. */
    WsList->LastInitializedWsle = (PAGE_SIZE - sizeof(*WsList)) / sizeof(MMWSLE);
}

_Use_decl_annotations_
VOID
NTAPI
MiInitializeWorkingSetList(_Inout_ PMMSUPPORT WorkingSet)
{
    PMMWSL WsList = WorkingSet->VmWorkingSetList;

    /* Initialize some fields */
    WsList->FirstFree = ULONG_MAX;
    WsList->Wsle = reinterpret_cast<PMMWSLE>(WsList + 1);
    WsList->LastEntry = 0;
    /* The first page is already allocated */
    WsList->LastInitializedWsle = (PAGE_SIZE - sizeof(*WsList)) / sizeof(MMWSLE);

    /* Insert the address we already know: our PDE base and the Working Set List */
    if (MI_IS_PROCESS_WORKING_SET(WorkingSet))
    {
        ASSERT(WorkingSet->VmWorkingSetList == MmWorkingSetList);
#if _MI_PAGING_LEVELS == 4
        MiInsertInWorkingSetList(WorkingSet, (PVOID)PXE_BASE, 0U);
#elif _MI_PAGING_LEVELS == 3
        MiInsertInWorkingSetList(WorkingSet, (PVOID)PPE_BASE, 0U);
#elif _MI_PAGING_LEVELS == 2
        MiInsertInWorkingSetList(WorkingSet, (PVOID)PDE_BASE, 0U);
#endif
    }

#if _MI_PAGING_LEVELS == 4
    MiInsertInWorkingSetList(WorkingSet, MiAddressToPpe(WorkingSet->VmWorkingSetList), 0UL);
#endif
#if _MI_PAGING_LEVELS >= 3
    MiInsertInWorkingSetList(WorkingSet, MiAddressToPde(WorkingSet->VmWorkingSetList), 0UL);
#endif
    MiInsertInWorkingSetList(WorkingSet, (PVOID)MiAddressToPte(WorkingSet->VmWorkingSetList), 0UL);
    MiInsertInWorkingSetList(WorkingSet, (PVOID)WorkingSet->VmWorkingSetList, 0UL);

    /* From now on, every added page can be trimmed at any time */
    WsList->FirstDynamic = WsList->LastEntry;

    /* We can add this to our list */
    ExInterlockedInsertTailList(&MmWorkingSetExpansionHead, &WorkingSet->WorkingSetExpansionLinks, &MmExpansionLock);
}

static
ULONG
MiTrimWorkingSetsPass(PULONG ScannedOut)
{
    PLIST_ENTRY VmListEntry;
    PMMSUPPORT Vm = NULL;
    KIRQL OldIrql;
    ULONG TotalWritten = 0;
    ULONG TotalScanned = 0;

    OldIrql = MiAcquireExpansionLock();

    for (VmListEntry = MmWorkingSetExpansionHead.Flink;
         VmListEntry != &MmWorkingSetExpansionHead;
         VmListEntry = VmListEntry->Flink)
    {
        BOOLEAN TrimHard = MmAvailablePages < MmMinimumFreePages;
        PEPROCESS Process = NULL;

        /* Don't do anything if the reserve is back. */
        if ((MmAvailablePages + MmModifiedPageListHead.Total) >= MiTrimGoalPages())
            break;

        Vm = CONTAINING_RECORD(VmListEntry, MMSUPPORT, WorkingSetExpansionLinks);

        /* Let the legacy Mm System space alone */
        if (Vm == MmGetKernelAddressSpace())
            continue;

        if (MI_IS_PROCESS_WORKING_SET(Vm))
        {
            Process = CONTAINING_RECORD(Vm, EPROCESS, Vm);

            /* Make sure the process is not terminating abd attach to it */
            if (!ExAcquireRundownProtection(&Process->RundownProtect))
                continue;
            ASSERT(!KeIsAttachedProcess());
            KeAttachProcess(&Process->Pcb);
        }
        else
        {
            /* FIXME: Session & system space unsupported */
            continue;
        }

        MiReleaseExpansionLock(OldIrql);

        /* Vm->WorkingSetSize is a byte count here, while the limits are page
         * counts, as the quota interface reports them. */
        SIZE_T WsPages = Vm->WorkingSetSize / PAGE_SIZE;
        SIZE_T MinimumWs = Vm->MinimumWorkingSetSize;

        if (MinimumWs == 0) MinimumWs = MmMinimumWorkingSetSize;

        /* Once the reserve is half gone, aging alone can no longer keep up:
         * take pages whether or not they were recently touched. Above that
         * mark the ordinary age-based choice reclaims idle pages gently. */
        BOOLEAN Aggressive = MmAvailablePages < (MiTrimGoalPages() / 2);

        /* Every page taken costs one synchronous page file write, so take few
         * of them per visit while the shortage is mild and the whole cluster
         * once the reserve is genuinely running out. */
        ULONG ClusterSize = (TrimHard || Aggressive) ? MI_TRIM_CLUSTER_SIZE
                                                     : MI_TRIM_CLUSTER_SIZE / 4;

        PFN_NUMBER Victims[MI_TRIM_CLUSTER_SIZE];
        ULONG Trimmed = 0;

        /* Share-lock for now, we're only reading */
        MiLockWorkingSetShared(PsGetCurrentThread(), Vm);

        /* Reaching this point already means the system is short of pages -- the
         * loop breaks out otherwise -- so a process may be trimmed back toward
         * its minimum working set. Waiting for it to exceed its *maximum*
         * first would mean never trimming at all: the default maximum is very
         * nearly the whole of RAM, so no single process reaches it before the
         * free list runs dry. */
        if ((WsPages > MinimumWs) &&
            MiConvertSharedWorkingSetLockToExclusive(PsGetCurrentThread(), Vm))
        {
            /* We're done */
            Vm->Flags.BeingTrimmed = 1;

            ULONG Scanned = 0;

            Trimmed = TrimWsList(Vm->VmWorkingSetList, Victims, ClusterSize,
                                 Aggressive, &Scanned);
            TotalScanned += Scanned;

            /* We're done */
            Vm->WorkingSetSize -= Trimmed * PAGE_SIZE;
            Vm->Flags.BeingTrimmed = 0;
            MiUnlockWorkingSet(PsGetCurrentThread(), Vm);
        }
        else
        {
            MiUnlockWorkingSetShared(PsGetCurrentThread(), Vm);
        }

        /* The page file write is synchronous, so it has to happen with the
         * working set lock dropped -- but still attached to the process,
         * because Pfn->PteAddress only means anything in its address space. */
        if (Trimmed != 0)
            TotalWritten += WriteTrimmedPages(Victims, Trimmed);

        /* Lock again */
        OldIrql = MiAcquireExpansionLock();

        if (Process)
        {
            KeDetachProcess();
            ExReleaseRundownProtection(&Process->RundownProtect);
        }
    }

    MiReleaseExpansionLock(OldIrql);

    *ScannedOut = TotalScanned;
    return TotalWritten;
}

ULONG
NTAPI
MmWorkingSetManager(VOID)
{
    ULONG TotalWritten = 0;

    /* Taking one cluster from each process and then going back to sleep for a
     * second reclaims far less than a running process dirties in that second,
     * so the free list could only ever go down. Keep sweeping until the
     * reserve is restored, a pass finds nothing left to take, or we have spent
     * long enough here -- the caller has a shortage to answer for either way. */
    for (ULONG Pass = 0; Pass < MI_TRIM_MAX_PASSES; Pass++)
    {
        if (MmAvailablePages >= MiTrimGoalPages())
            break;

        ULONG Scanned = 0;
        ULONG Written = MiTrimWorkingSetsPass(&Scanned);

        /* A pass that wrote nothing has not necessarily failed: the first
         * sweep over a large working set finds every page freshly accessed
         * and does no more than clear those bits, which is what lets the next
         * sweep tell a cold page from a hot one. Only give up when there was
         * nothing left even to look at. */
        if ((Written == 0) && (Scanned == 0))
            break;

        TotalWritten += Written;
    }

    return TotalWritten;
}

} // extern "C"
