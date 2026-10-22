/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite reader/writer spin lock (EX_SPIN_LOCK) test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#define EX_SPIN_LOCK_EXCLUSIVE 0x80000000UL

#define CONTENTION_THREADS      4
#define CONTENTION_ITERATIONS   20000

/* 2 seconds in 100ns units */
#define WAITER_TIMEOUT          (2LL * 1000 * 1000 * 10)

static
VOID
TestSharedAndExclusive(VOID)
{
    EX_SPIN_LOCK Lock = 0;
    KIRQL OldIrql, OldIrql2, ApcIrql;
    LOGICAL Result;

    /* Shared acquire raises to DISPATCH_LEVEL and returns the old IRQL */
    ok_irql(PASSIVE_LEVEL);
    OldIrql = ExAcquireSpinLockShared(&Lock);
    ok_eq_uint(OldIrql, PASSIVE_LEVEL);
    ok_irql(DISPATCH_LEVEL);
    ok_eq_hex((ULONG)Lock, 1UL);

    /* Shared is shared */
    ExAcquireSpinLockSharedAtDpcLevel(&Lock);
    ok_eq_hex((ULONG)Lock, 2UL);
    ok_irql(DISPATCH_LEVEL);
    Result = ExTryAcquireSpinLockSharedAtDpcLevel(&Lock);
    ok_eq_ulong(Result, TRUE);
    ok_eq_hex((ULONG)Lock, 3UL);

    /* ...but excludes exclusive */
    Result = ExTryAcquireSpinLockExclusiveAtDpcLevel(&Lock);
    ok_eq_ulong(Result, FALSE);
    ok_eq_hex((ULONG)Lock, 3UL);

    /* Conversion requires being the only shared owner */
    Result = ExTryConvertSharedSpinLockExclusive(&Lock);
    ok_eq_ulong(Result, FALSE);
    ok_eq_hex((ULONG)Lock, 3UL);

    ExReleaseSpinLockSharedFromDpcLevel(&Lock);
    ExReleaseSpinLockSharedFromDpcLevel(&Lock);
    ok_eq_hex((ULONG)Lock, 1UL);
    ok_irql(DISPATCH_LEVEL);

    Result = ExTryConvertSharedSpinLockExclusive(&Lock);
    ok_eq_ulong(Result, TRUE);
    ok_eq_hex((ULONG)Lock, EX_SPIN_LOCK_EXCLUSIVE);

    /* Exclusive excludes everyone */
    Result = ExTryAcquireSpinLockSharedAtDpcLevel(&Lock);
    ok_eq_ulong(Result, FALSE);
    Result = ExTryAcquireSpinLockExclusiveAtDpcLevel(&Lock);
    ok_eq_ulong(Result, FALSE);
    ok_eq_hex((ULONG)Lock, EX_SPIN_LOCK_EXCLUSIVE);

    /* The converted lock is released as exclusive, restoring the IRQL */
    ExReleaseSpinLockExclusive(&Lock, OldIrql);
    ok_irql(PASSIVE_LEVEL);
    ok_eq_hex((ULONG)Lock, 0UL);

    /* Exclusive acquire */
    OldIrql = ExAcquireSpinLockExclusive(&Lock);
    ok_eq_uint(OldIrql, PASSIVE_LEVEL);
    ok_irql(DISPATCH_LEVEL);
    ok_eq_hex((ULONG)Lock, EX_SPIN_LOCK_EXCLUSIVE);
    Result = ExTryAcquireSpinLockSharedAtDpcLevel(&Lock);
    ok_eq_ulong(Result, FALSE);
    ExReleaseSpinLockExclusive(&Lock, OldIrql);
    ok_irql(PASSIVE_LEVEL);
    ok_eq_hex((ULONG)Lock, 0UL);

    /* AtDpcLevel variants do not change the IRQL */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    ExAcquireSpinLockExclusiveAtDpcLevel(&Lock);
    ok_irql(DISPATCH_LEVEL);
    ok_eq_hex((ULONG)Lock, EX_SPIN_LOCK_EXCLUSIVE);
    ExReleaseSpinLockExclusiveFromDpcLevel(&Lock);
    ok_irql(DISPATCH_LEVEL);
    ok_eq_hex((ULONG)Lock, 0UL);
    Result = ExTryAcquireSpinLockExclusiveAtDpcLevel(&Lock);
    ok_eq_ulong(Result, TRUE);
    ok_eq_hex((ULONG)Lock, EX_SPIN_LOCK_EXCLUSIVE);
    ExReleaseSpinLockExclusiveFromDpcLevel(&Lock);
    ok_eq_hex((ULONG)Lock, 0UL);
    KeLowerIrql(OldIrql);
    ok_irql(PASSIVE_LEVEL);

    /* The returned IRQL is the caller's, not always PASSIVE_LEVEL */
    KeRaiseIrql(APC_LEVEL, &ApcIrql);
    OldIrql = ExAcquireSpinLockShared(&Lock);
    ok_eq_uint(OldIrql, APC_LEVEL);
    ok_irql(DISPATCH_LEVEL);
    ExReleaseSpinLockShared(&Lock, OldIrql);
    ok_irql(APC_LEVEL);
    OldIrql = ExAcquireSpinLockExclusive(&Lock);
    ok_eq_uint(OldIrql, APC_LEVEL);
    ExReleaseSpinLockExclusive(&Lock, OldIrql);
    ok_irql(APC_LEVEL);
    KeLowerIrql(ApcIrql);

    /* Acquiring at DISPATCH_LEVEL returns DISPATCH_LEVEL */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    OldIrql2 = ExAcquireSpinLockShared(&Lock);
    ok_eq_uint(OldIrql2, DISPATCH_LEVEL);
    ExReleaseSpinLockShared(&Lock, OldIrql2);
    ok_irql(DISPATCH_LEVEL);
    KeLowerIrql(OldIrql);

    ok_eq_hex((ULONG)Lock, 0UL);
}

typedef struct _RW_CONTENTION
{
    EX_SPIN_LOCK Lock;
    volatile LONG ActiveReaders;
    volatile LONG ActiveWriters;
    volatile LONG Violations;
    volatile LONG Conversions;
    volatile LONG Started;
    volatile LONG MaxReaders;
    volatile ULONG Sequence;
    volatile ULONG SequenceInverse;
} RW_CONTENTION, *PRW_CONTENTION;

static
VOID
ContentionWrite(
    _Inout_ PRW_CONTENTION State)
{
    if (InterlockedIncrement(&State->ActiveWriters) != 1 ||
        State->ActiveReaders != 0)
    {
        InterlockedIncrement(&State->Violations);
    }

    /* Two separate writes a reader must never see half done */
    State->Sequence++;
    KeMemoryBarrier();
    State->SequenceInverse = ~State->Sequence;

    InterlockedDecrement(&State->ActiveWriters);
}

static
VOID
ContentionRead(
    _Inout_ PRW_CONTENTION State)
{
    LONG Readers = InterlockedIncrement(&State->ActiveReaders);
    LONG Max = ReadNoFence(&State->MaxReaders);

    while (Readers > Max)
    {
        LONG Previous = InterlockedCompareExchange(&State->MaxReaders, Readers, Max);
        if (Previous == Max)
            break;
        Max = Previous;
    }

    if (State->ActiveWriters != 0 ||
        State->SequenceInverse != (ULONG)~State->Sequence)
    {
        InterlockedIncrement(&State->Violations);
    }
    InterlockedDecrement(&State->ActiveReaders);
}

static
VOID
NTAPI
ContentionThread(
    _In_ PVOID Context)
{
    PRW_CONTENTION State = Context;
    KIRQL OldIrql;
    ULONG i, Index;

    /* Spread over the processors and start together, so that on SMP the
     * threads really overlap */
    Index = (ULONG)InterlockedIncrement(&State->Started) - 1;
    KeSetSystemAffinityThread(AFFINITY_MASK(Index % (ULONG)KeNumberProcessors));
    while (ReadAcquire(&State->Started) < CONTENTION_THREADS)
        YieldProcessor();

    for (i = 0; i < CONTENTION_ITERATIONS; i++)
    {
        switch (i % 5)
        {
            case 0:
                OldIrql = ExAcquireSpinLockExclusive(&State->Lock);
                ContentionWrite(State);
                ExReleaseSpinLockExclusive(&State->Lock, OldIrql);
                break;

            case 1:
                KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
                ExAcquireSpinLockExclusiveAtDpcLevel(&State->Lock);
                ContentionWrite(State);
                ExReleaseSpinLockExclusiveFromDpcLevel(&State->Lock);
                KeLowerIrql(OldIrql);
                break;

            case 2:
                KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
                ExAcquireSpinLockSharedAtDpcLevel(&State->Lock);
                ContentionRead(State);
                ExReleaseSpinLockSharedFromDpcLevel(&State->Lock);
                KeLowerIrql(OldIrql);
                break;

            case 3:
                /* Read, then upgrade if we are alone */
                OldIrql = ExAcquireSpinLockShared(&State->Lock);
                ContentionRead(State);
                if (ExTryConvertSharedSpinLockExclusive(&State->Lock))
                {
                    InterlockedIncrement(&State->Conversions);
                    ContentionWrite(State);
                    ExReleaseSpinLockExclusive(&State->Lock, OldIrql);
                }
                else
                {
                    ExReleaseSpinLockShared(&State->Lock, OldIrql);
                }
                break;

            default:
                OldIrql = ExAcquireSpinLockShared(&State->Lock);
                ContentionRead(State);
                ExReleaseSpinLockShared(&State->Lock, OldIrql);
                break;
        }
    }

    KeRevertToUserAffinityThread();
}

static
VOID
TestContention(VOID)
{
    PRW_CONTENTION State;
    PKTHREAD Threads[CONTENTION_THREADS];
    ULONG i;

    State = ExAllocatePoolZero(NonPagedPool, sizeof(*State), 'tSwR');
    if (skip(State != NULL, "Out of memory\n"))
        return;

    State->SequenceInverse = ~0UL;

    for (i = 0; i < CONTENTION_THREADS; i++)
        Threads[i] = KmtStartThread(ContentionThread, State);
    for (i = 0; i < CONTENTION_THREADS; i++)
        KmtFinishThread(Threads[i], NULL);

    ok_eq_long(State->Violations, 0L);
    ok_eq_hex((ULONG)State->Lock, 0UL);
    ok_eq_long(State->ActiveReaders, 0L);
    ok_eq_long(State->ActiveWriters, 0L);
    /* Every exclusive section ran exactly once */
    ok_eq_ulong(State->Sequence,
                (ULONG)(CONTENTION_THREADS * (2 * (CONTENTION_ITERATIONS / 5)) + State->Conversions));
    trace("%ld threads, %lu writes, %ld conversions, %ld max concurrent readers, %lu processors\n",
          (LONG)CONTENTION_THREADS, State->Sequence, State->Conversions, State->MaxReaders,
          (ULONG)KeNumberProcessors);

    ExFreePoolWithTag(State, 'tSwR');
}

typedef struct _RW_WAITER
{
    EX_SPIN_LOCK Lock;
    KAFFINITY Affinity;
    volatile LONG Ready;
    volatile LONG Go;
    volatile LONG Acquired;
} RW_WAITER, *PRW_WAITER;

static
VOID
NTAPI
WaiterThread(
    _In_ PVOID Context)
{
    PRW_WAITER State = Context;
    KIRQL OldIrql;

    KeSetSystemAffinityThread(State->Affinity);
    InterlockedExchange(&State->Ready, 1);

    while (!ReadAcquire(&State->Go))
        YieldProcessor();

    OldIrql = ExAcquireSpinLockExclusive(&State->Lock);
    ok_eq_uint(OldIrql, PASSIVE_LEVEL);
    InterlockedExchange(&State->Acquired, 1);
    ExReleaseSpinLockExclusive(&State->Lock, OldIrql);

    KeRevertToUserAffinityThread();
}

/* An exclusive waiter keeps new shared owners out */
static
VOID
TestExclusiveWaiter(VOID)
{
    PRW_WAITER State;
    PKTHREAD Thread;
    KIRQL OldIrql;
    ULONGLONG Start;
    BOOLEAN SawWaiter = FALSE;
    LOGICAL Result;

    if (skip(KeNumberProcessors >= 2, "Needs 2 processors, have %lu\n", (ULONG)KeNumberProcessors))
        return;

    State = ExAllocatePoolZero(NonPagedPool, sizeof(*State), 'tSwR');
    if (skip(State != NULL, "Out of memory\n"))
        return;

    State->Affinity = AFFINITY_MASK(1);
    KeSetSystemAffinityThread(AFFINITY_MASK(0));

    Thread = KmtStartThread(WaiterThread, State);

    /* Make sure the waiter is running on the other processor before this one
     * goes to DISPATCH_LEVEL */
    Start = KeQueryInterruptTime();
    while (!ReadAcquire(&State->Ready) && KeQueryInterruptTime() - Start < WAITER_TIMEOUT)
    {
        LARGE_INTEGER Interval;
        Interval.QuadPart = -10 * 1000;
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }
    ok_eq_long(State->Ready, 1L);

    OldIrql = ExAcquireSpinLockShared(&State->Lock);
    InterlockedExchange(&State->Go, 1);

    Start = KeQueryInterruptTime();
    while (KeQueryInterruptTime() - Start < WAITER_TIMEOUT)
    {
        if ((ULONG)ReadNoFence(&State->Lock) & EX_SPIN_LOCK_EXCLUSIVE)
        {
            SawWaiter = TRUE;
            break;
        }
        YieldProcessor();
    }

    if (SawWaiter)
    {
        /* We still own it shared; the waiter must not have it, and must
         * keep new shared owners out */
        ok_eq_hex((ULONG)State->Lock, EX_SPIN_LOCK_EXCLUSIVE | 1);
        Result = ExTryAcquireSpinLockSharedAtDpcLevel(&State->Lock);
        ok_eq_ulong(Result, FALSE);
        ok_eq_long(State->Acquired, 0L);
        /* And we can no longer convert, the waiter goes first */
        ok_eq_ulong(ExTryConvertSharedSpinLockExclusive(&State->Lock), FALSE);
    }

    ExReleaseSpinLockShared(&State->Lock, OldIrql);

    ok(SawWaiter, "Exclusive waiter never set the exclusive bit\n");

    KmtFinishThread(Thread, NULL);
    KeRevertToUserAffinityThread();

    ok_eq_long(State->Acquired, 1L);
    ok_eq_hex((ULONG)State->Lock, 0UL);

    ExFreePoolWithTag(State, 'tSwR');
}

START_TEST(ExSpinLockRw)
{
    TestSharedAndExclusive();
    TestContention();
    TestExclusiveWaiter();
}
