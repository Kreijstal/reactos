/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite cross-processor (IPI) test
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define IPI_TEST_CALLS          32
#define IPI_RENDEZVOUS_CALLS    8
#define IPI_RENDEZVOUS_STALL_US 2000000 /* 2 s per call before giving up */
#define TLB_TEST_TAG            'bTlT'
#define TLB_PATTERN_A           0xA5A5A5A5UL
#define TLB_PATTERN_B           0x5A5A5A5AUL

static volatile LONG IpiCallCount[MAXIMUM_PROCESSORS];
static volatile LONG IpiWrongIrql;
static volatile LONG IpiArrived;
static volatile LONG IpiRendezvousTimeouts;
static volatile LONG IpiRendezvousTarget;

/* Count one execution on this processor and check it runs at IPI_LEVEL */
static
ULONG_PTR
NTAPI
IpiCountWorker(
    _In_ ULONG_PTR Argument)
{
    ULONG Processor = KeGetCurrentProcessorNumber();

    UNREFERENCED_PARAMETER(Argument);

    if (KeGetCurrentIrql() != IPI_LEVEL)
        InterlockedIncrement(&IpiWrongIrql);
    if (Processor < MAXIMUM_PROCESSORS)
        InterlockedIncrement(&IpiCallCount[Processor]);

    return (ULONG_PTR)Processor + 1;
}

/*
 * Every processor must be inside the worker at the same time: each one waits
 * here until all of them have arrived. If any processor is skipped (or runs
 * the worker only after another one returned) the wait times out.
 */
static
ULONG_PTR
NTAPI
IpiRendezvousWorker(
    _In_ ULONG_PTR Argument)
{
    ULONG Stall;

    UNREFERENCED_PARAMETER(Argument);

    InterlockedIncrement(&IpiArrived);
    for (Stall = 0; Stall < IPI_RENDEZVOUS_STALL_US; Stall++)
    {
        if (InterlockedCompareExchange(&IpiArrived, 0, 0) >= IpiRendezvousTarget)
            return 0;
        KeStallExecutionProcessor(1);
    }

    InterlockedIncrement(&IpiRendezvousTimeouts);
    return 0;
}

static
VOID
TestGenericCall(
    _In_ KAFFINITY Active,
    _In_ ULONG ActiveCount)
{
    ULONG Processor, Call;
    ULONG_PTR Result;
    KAFFINITY Mask;

    RtlZeroMemory((PVOID)IpiCallCount, sizeof(IpiCallCount));
    IpiWrongIrql = 0;

    /* Initiate from every processor in turn */
    for (Processor = 0, Mask = 1; Processor < MAXIMUM_PROCESSORS; Processor++, Mask <<= 1)
    {
        if (!(Active & Mask))
            continue;

        KeSetSystemAffinityThread(Mask);
        for (Call = 0; Call < IPI_TEST_CALLS; Call++)
        {
            Result = KeIpiGenericCall(IpiCountWorker, 0);
            ok_eq_ulongptr(Result, (ULONG_PTR)Processor + 1);
        }
        KeRevertToUserAffinityThread();
    }

    ok_eq_long(IpiWrongIrql, 0L);
    for (Processor = 0, Mask = 1; Processor < MAXIMUM_PROCESSORS; Processor++, Mask <<= 1)
    {
        if (Active & Mask)
        {
            ok(IpiCallCount[Processor] == (LONG)(IPI_TEST_CALLS * ActiveCount),
               "Processor %lu ran the worker %ld times, expected %lu\n",
               Processor, IpiCallCount[Processor], IPI_TEST_CALLS * ActiveCount);
        }
        else
        {
            ok(IpiCallCount[Processor] == 0,
               "Inactive processor %lu ran the worker %ld times\n",
               Processor, IpiCallCount[Processor]);
        }
    }
}

static
VOID
TestRendezvous(
    _In_ ULONG ActiveCount)
{
    ULONG Call;

    IpiRendezvousTimeouts = 0;
    for (Call = 0; Call < IPI_RENDEZVOUS_CALLS; Call++)
    {
        IpiArrived = 0;
        IpiRendezvousTarget = (LONG)ActiveCount;
        KeIpiGenericCall(IpiRendezvousWorker, 0);
        ok_eq_long(IpiArrived, (LONG)ActiveCount);
    }
    ok_eq_long(IpiRendezvousTimeouts, 0L);
}

/*
 * A translation cached by another processor must not survive an unmap:
 * map page A at a reserved address, read it on processor Other (filling its
 * TLB), remap the same address to page B here, and read again on Other.
 */
static
VOID
TestTlbCoherence(
    _In_ KAFFINITY Active)
{
    PHYSICAL_ADDRESS Low, High, Skip;
    PVOID Reserved;
    PMDL MdlA, MdlB;
    volatile ULONG *Va;
    ULONG Here, Other, Round;
    KAFFINITY HereMask, OtherMask;
    ULONG Seen;

    Low.QuadPart = 0;
    High.QuadPart = -1;
    Skip.QuadPart = 0;

    Reserved = MmAllocateMappingAddress(PAGE_SIZE, TLB_TEST_TAG);
    ok(Reserved != NULL, "MmAllocateMappingAddress failed\n");
    if (!Reserved)
        return;

    MdlA = MmAllocatePagesForMdl(Low, High, Skip, PAGE_SIZE);
    MdlB = MmAllocatePagesForMdl(Low, High, Skip, PAGE_SIZE);
    ok(MdlA != NULL && MdlB != NULL, "MmAllocatePagesForMdl failed\n");
    if (!MdlA || !MdlB)
        goto Cleanup;

    for (Here = 0, HereMask = 1; Here < MAXIMUM_PROCESSORS; Here++, HereMask <<= 1)
    {
        if (!(Active & HereMask))
            continue;

        for (Other = 0, OtherMask = 1; Other < MAXIMUM_PROCESSORS; Other++, OtherMask <<= 1)
        {
            if (!(Active & OtherMask) || (Other == Here))
                continue;

            for (Round = 0; Round < 4; Round++)
            {
                /* Map A here and fill it */
                KeSetSystemAffinityThread(HereMask);
                Va = MmMapLockedPagesWithReservedMapping(Reserved, TLB_TEST_TAG, MdlA, MmCached);
                ok(Va != NULL, "Mapping A failed\n");
                if (!Va)
                {
                    KeRevertToUserAffinityThread();
                    goto Cleanup;
                }
                *Va = TLB_PATTERN_A + Round;

                /* Read it on the other processor, caching the translation */
                KeSetSystemAffinityThread(OtherMask);
                Seen = *Va;
                ok(Seen == TLB_PATTERN_A + Round,
                   "CPU %lu read 0x%lx through A mapped on CPU %lu, expected 0x%lx\n",
                   Other, Seen, Here, TLB_PATTERN_A + Round);

                /* Remap the same address to B here */
                KeSetSystemAffinityThread(HereMask);
                MmUnmapReservedMapping(Reserved, TLB_TEST_TAG, MdlA);
                Va = MmMapLockedPagesWithReservedMapping(Reserved, TLB_TEST_TAG, MdlB, MmCached);
                ok(Va != NULL, "Mapping B failed\n");
                if (!Va)
                {
                    KeRevertToUserAffinityThread();
                    goto Cleanup;
                }
                *Va = TLB_PATTERN_B + Round;

                /* The other processor must now see B, not its stale A entry */
                KeSetSystemAffinityThread(OtherMask);
                Seen = *Va;
                ok(Seen == TLB_PATTERN_B + Round,
                   "CPU %lu read 0x%lx after CPU %lu remapped to B, expected 0x%lx (stale TLB)\n",
                   Other, Seen, Here, TLB_PATTERN_B + Round);

                KeSetSystemAffinityThread(HereMask);
                MmUnmapReservedMapping(Reserved, TLB_TEST_TAG, MdlB);
                KeRevertToUserAffinityThread();
            }
        }
    }

Cleanup:
    if (MdlA)
    {
        MmFreePagesFromMdl(MdlA);
        ExFreePool(MdlA);
    }
    if (MdlB)
    {
        MmFreePagesFromMdl(MdlB);
        ExFreePool(MdlB);
    }
    MmFreeMappingAddress(Reserved, TLB_TEST_TAG);
}

START_TEST(KeIpi)
{
    KAFFINITY Active, Mask;
    ULONG ActiveCount;

    Active = KeQueryActiveProcessors();
    for (ActiveCount = 0, Mask = Active; Mask != 0; Mask &= Mask - 1)
        ActiveCount++;
    trace("Active processors 0x%Ix, count %lu\n", Active, ActiveCount);
    ok(Active != 0, "No active processors\n");
    ok(ActiveCount >= 1 && ActiveCount <= MAXIMUM_PROCESSORS, "Bad processor count %lu\n", ActiveCount);

    TestGenericCall(Active, ActiveCount);
    TestRendezvous(ActiveCount);

    if (ActiveCount > 1)
        TestTlbCoherence(Active);
    else
        skip(FALSE, "Single processor, skipping the cross-processor TLB check\n");
}
