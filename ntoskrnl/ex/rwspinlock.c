/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Reader/writer spin locks (EX_SPIN_LOCK)
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * An EX_SPIN_LOCK is a LONG. Bit 31 is set while a thread owns the lock
 * exclusively, or is waiting for the shared owners to drain before it does.
 * The remaining bits count the shared owners. New shared owners are refused
 * while bit 31 is set, so a waiting exclusive owner cannot be starved.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define EX_SPIN_LOCK_EXCLUSIVE      0x80000000L
#define EX_SPIN_LOCK_SHARED_MASK    0x7FFFFFFFL

/* PRIVATE FUNCTIONS *********************************************************/

FORCEINLINE
BOOLEAN
ExpTryAcquireSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    LONG Value = ReadNoFence(SpinLock);

    while (!(Value & EX_SPIN_LOCK_EXCLUSIVE))
    {
        LONG Previous = InterlockedCompareExchange(SpinLock, Value + 1, Value);
        if (Previous == Value)
            return TRUE;

        /* Another shared owner came or went; retry with the new count */
        Value = Previous;
    }

    return FALSE;
}

/* PUBLIC FUNCTIONS **********************************************************/

VOID
NTAPI
ExAcquireSpinLockSharedAtDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    while (!ExpTryAcquireSpinLockShared(SpinLock))
    {
        /* Wait for the exclusive owner, or exclusive waiter, to go away */
        while (ReadNoFence(SpinLock) & EX_SPIN_LOCK_EXCLUSIVE)
            YieldProcessor();
    }
}

KIRQL
NTAPI
ExAcquireSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    KIRQL OldIrql = KeRaiseIrqlToDpcLevel();

    ExAcquireSpinLockSharedAtDpcLevel(SpinLock);
    return OldIrql;
}

LOGICAL
NTAPI
ExTryAcquireSpinLockSharedAtDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    return ExpTryAcquireSpinLockShared(SpinLock);
}

VOID
NTAPI
ExReleaseSpinLockSharedFromDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);
    ASSERT((ReadNoFence(SpinLock) & EX_SPIN_LOCK_SHARED_MASK) != 0);

    InterlockedDecrementRelease(SpinLock);
}

VOID
NTAPI
ExReleaseSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    ExReleaseSpinLockSharedFromDpcLevel(SpinLock);
    KeLowerIrql(OldIrql);
}

VOID
NTAPI
ExAcquireSpinLockExclusiveAtDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    /* Claim the exclusive bit, which also stops new shared owners */
    while (InterlockedBitTestAndSet(SpinLock, 31))
    {
        while (ReadNoFence(SpinLock) & EX_SPIN_LOCK_EXCLUSIVE)
            YieldProcessor();
    }

    /* Wait for the existing shared owners to release the lock */
    while (ReadAcquire(SpinLock) & EX_SPIN_LOCK_SHARED_MASK)
        YieldProcessor();
}

KIRQL
NTAPI
ExAcquireSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    KIRQL OldIrql = KeRaiseIrqlToDpcLevel();

    ExAcquireSpinLockExclusiveAtDpcLevel(SpinLock);
    return OldIrql;
}

LOGICAL
NTAPI
ExTryAcquireSpinLockExclusiveAtDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    return (InterlockedCompareExchange(SpinLock, EX_SPIN_LOCK_EXCLUSIVE, 0) == 0);
}

VOID
NTAPI
ExReleaseSpinLockExclusiveFromDpcLevel(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);
    ASSERT(ReadNoFence(SpinLock) == EX_SPIN_LOCK_EXCLUSIVE);

    /* No shared owner can join while the exclusive bit is set */
    InterlockedAndRelease(SpinLock, ~EX_SPIN_LOCK_EXCLUSIVE);
}

VOID
NTAPI
ExReleaseSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    ExReleaseSpinLockExclusiveFromDpcLevel(SpinLock);
    KeLowerIrql(OldIrql);
}

LOGICAL
NTAPI
ExTryConvertSharedSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock)
{
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    /*
     * Only the sole shared owner can convert. If another shared owner or an
     * exclusive waiter is present, the caller must release and reacquire.
     */
    return (InterlockedCompareExchange(SpinLock, EX_SPIN_LOCK_EXCLUSIVE, 1) == 1);
}
