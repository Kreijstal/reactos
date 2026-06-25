/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Kernel-Mode Test Suite VAD read-lock assertion vs. push-lock
 *              owner tracking.
 *
 * Deterministic reproduction of the intermittent process-churn bugcheck at
 * ntoskrnl/mm/ARM3/vadnode.c (MiDbgAssertIsLockedForRead).
 *
 * On Vista+ EPROCESS.AddressCreationLock is an EX_PUSH_LOCK, which has no
 * Owner field.  MiInsertVadEx() reads the process VAD tree
 * (MiFindEmptyAddressRangeInTree / MiCheckForConflictingNode) while holding
 * ONLY the AddressCreationLock -- before it locks the working set -- so the
 * DBG read-lock assert is satisfied through ExPushLockIsOwnedByCurrentThread(),
 * which consults the per-thread ETHREAD.HeldPushLocks[] tracking array.
 *
 * That array has room for only RTL_NUMBER_OF(HeldPushLocks) (== 8) entries and
 * silently stops recording on overflow.  A thread that already holds 8 push
 * locks (Ob directory / handle-table / FsRtl FCB / win32k callout locks all
 * are EX_PUSH_LOCKs and nest deeply under load) therefore does NOT get the
 * AddressCreationLock recorded when MiInsertVadEx acquires it.  The helper then
 * returns FALSE for a lock that is genuinely held, MI_WS_OWNER() is also FALSE
 * (the working set is not locked yet), and the read-lock assert false-fires.
 *
 * This test makes that race deterministic: it fills the tracking array, then
 * performs an ordinary private reservation.  On a kernel whose ownership
 * tracking does not account for overflow it bugchecks inside ZwAllocate-
 * VirtualMemory; on a fixed kernel the reservation completes and the test
 * passes.
 */

#include <kmt_test.h>

/* ExPushLockIsOwnedByCurrentThread() is ReactOS-internal and not exported, so
 * reproduce its DBG-only logic locally: scan the per-thread tracking array. */
static
BOOLEAN
IsTrackedHeld(_In_ PETHREAD Thread, _In_ PVOID PushLock)
{
    UCHAR i;

    for (i = 0; i < Thread->HeldPushLockCount; i++)
    {
        if (Thread->HeldPushLocks[i] == PushLock)
            return TRUE;
    }

    return FALSE;
}

static
VOID
DoReserve(_In_ PCSTR What)
{
    PVOID Base = NULL;
    SIZE_T Size = 0x10000;
    NTSTATUS Status;

    /* A private reservation with no base address drives MiInsertVadEx down the
     * MiFindEmptyAddressRangeInTree() path, which reads the VAD tree under the
     * AddressCreationLock only. */
    Status = ZwAllocateVirtualMemory(ZwCurrentProcess(),
                                     &Base,
                                     0,
                                     &Size,
                                     MEM_RESERVE,
                                     PAGE_READWRITE);
    ok(NT_SUCCESS(Status), "%s: ZwAllocateVirtualMemory returned %lx\n", What, Status);

    if (NT_SUCCESS(Status))
    {
        Size = 0;
        Status = ZwFreeVirtualMemory(ZwCurrentProcess(),
                                     &Base,
                                     &Size,
                                     MEM_RELEASE);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
}

START_TEST(MmVadLock)
{
    PETHREAD Thread = PsGetCurrentThread();
    EX_PUSH_LOCK Locks[RTL_NUMBER_OF(Thread->HeldPushLocks)];
    UCHAR Baseline;
    ULONG i;

    if (KmtIsCheckedBuild == FALSE)
    {
        /* The read-lock assert and the owner tracking it relies on are
         * DBG-only; on a free build there is nothing to verify. */
        trace("Free build: VAD read-lock assert is compiled out, skipping\n");
        return;
    }

    /* The tracking array must agree with reality while it has room. */
    RtlZeroMemory(&Locks[0], sizeof(Locks[0]));
    KeEnterCriticalRegion();
    ExfAcquirePushLockExclusive(&Locks[0]);
    ok_bool_true(IsTrackedHeld(Thread, &Locks[0]),
                 "tracking records a held push lock");
    ExfReleasePushLockExclusive(&Locks[0]);
    KeLeaveCriticalRegion();
    ok_bool_false(IsTrackedHeld(Thread, &Locks[0]),
                  "tracking drops a released push lock");

    /* Baseline: a VAD-inserting allocation with no extra push locks held
     * exercises the same MiInsertVadEx read path and must succeed. */
    DoReserve("baseline");

    /* Fill the per-thread push-lock tracking array. */
    Baseline = Thread->HeldPushLockCount;
    for (i = 0; i < RTL_NUMBER_OF(Locks); i++)
    {
        RtlZeroMemory(&Locks[i], sizeof(Locks[i]));
        KeEnterCriticalRegion();
        ExfAcquirePushLockExclusive(&Locks[i]);
    }

    /* The array caps at its size; further acquisitions are not recorded. */
    ok_eq_uint(Thread->HeldPushLockCount, (UCHAR)RTL_NUMBER_OF(Thread->HeldPushLocks));

    /* The AddressCreationLock that MiInsertVadEx acquires internally is now the
     * (9th) push lock and overflows the tracker.  On a kernel that cannot
     * account for the overflow the VAD read-lock assert false-fires here even
     * though the lock IS held; a corrected kernel completes the reservation.
     * Wrap it so a regressed kernel reports a clean failure instead of
     * unwinding the assert through our held critical regions. */
    {
        PVOID Base = NULL;
        SIZE_T RegionSize = 0x10000;
        NTSTATUS AllocStatus = STATUS_UNSUCCESSFUL;

        KmtStartSeh()
            AllocStatus = ZwAllocateVirtualMemory(ZwCurrentProcess(),
                                                  &Base,
                                                  0,
                                                  &RegionSize,
                                                  MEM_RESERVE,
                                                  PAGE_READWRITE);
        KmtEndSeh(STATUS_SUCCESS);

        ok(NT_SUCCESS(AllocStatus),
           "tracker-overflow: ZwAllocateVirtualMemory returned %lx\n", AllocStatus);
        if (NT_SUCCESS(AllocStatus))
        {
            RegionSize = 0;
            ZwFreeVirtualMemory(ZwCurrentProcess(), &Base, &RegionSize, MEM_RELEASE);
        }
    }

    for (i = 0; i < RTL_NUMBER_OF(Locks); i++)
    {
        ExfReleasePushLockExclusive(&Locks[i]);
        KeLeaveCriticalRegion();
    }

    /* Tracker must be back to the baseline once everything is released. */
    ok_eq_uint(Thread->HeldPushLockCount, Baseline);
    ok_bool_false(IsTrackedHeld(Thread, &Locks[0]),
                  "tracking is clean after release");
}
