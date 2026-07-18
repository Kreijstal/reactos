/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     SMP stress for the KTIMER timer-table (timer wheel): hammers
 *              KeSetTimerEx/KeCancelTimer/KeWaitForSingleObject/
 *              KeDelayExecutionThread plus DPC re-arm and cross-CPU DPC
 *              targeting from every processor concurrently, to exercise the
 *              KiTimerTableListHead[].Time invariant
 *              (head.Time <= front.DueTime, asserted in KiTimerExpiration)
 *              and the insert/expire/cancel list locking. On a correct
 *              kernel this runs to completion; a timer-wheel race fires the
 *              DBG assert or corrupts the bucket lists.
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

/* Runtime of the storm phase */
#define STRESS_SECONDS      75

/* Timers per setter thread (private slice) */
#define TIMERS_PER_SETTER   16

/* Shared timers hammered by every setter (max contention on one object) */
#define N_SHARED_TIMERS     8

/* DPC self-re-arming timers (expiry-path KeSetTimerEx at DISPATCH_LEVEL) */
#define N_REARM_TIMERS      8

#define MAX_SETTERS         16
#define MAX_DELAYERS        8

/* Cap on concurrently live one-shot threads (teardown churn) */
#define ONESHOT_MAX         64

static volatile LONG g_Stop;
static volatile LONG g_SetOps;
static volatile LONG g_CancelOps;
static volatile LONG g_WaitOps;
static volatile LONG g_DelayOps;
static volatile LONG g_DpcRearms;
static volatile LONG g_OneShotLive;
static volatile LONG g_ChurnThreads;

static KTIMER g_SharedTimers[N_SHARED_TIMERS];

static KTIMER g_RearmTimers[N_REARM_TIMERS];
static KDPC g_RearmDpcs[N_REARM_TIMERS];

typedef struct _SETTER_CTX
{
    ULONG Seed;
    KTIMER Timers[TIMERS_PER_SETTER];
} SETTER_CTX, *PSETTER_CTX;

static SETTER_CTX g_Setters[MAX_SETTERS];

static
ULONG
NextRand(PULONG Seed)
{
    /* xorshift32 */
    ULONG x = *Seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *Seed = x;
    return x;
}

/* Pick a relative due time covering the interesting wheel geometry:
 * already-expired, sub-tick, few-ticks, and beyond one wheel revolution
 * (512 buckets * ~15.6ms = ~8s), which also exercises hands >= 256. */
static
LONGLONG
RandomRelativeDue(PULONG Seed)
{
    ULONG r = NextRand(Seed);
    switch (r & 3)
    {
        case 0: /* already due / immediate: 0 .. 1ms */
            return -(LONGLONG)(NextRand(Seed) % (10 * 1000 + 1));
        case 1: /* short: 1 .. 30ms */
            return -(LONGLONG)(10 * 1000 + NextRand(Seed) % (29 * 10 * 1000));
        case 2: /* medium: 30ms .. 1s */
            return -(LONGLONG)(30 * 10 * 1000 + NextRand(Seed) % (970 * 10 * 1000));
        default: /* long, wraps the wheel: 8 .. 20s */
            return -(LONGLONG)(8LL * 10 * 1000 * 1000 + NextRand(Seed) % (12ULL * 10 * 1000 * 1000));
    }
}

/* DPC routine: re-arm our own timer with a short random due time.
 * Runs at DISPATCH_LEVEL out of KiTimerExpiration/KiRetireDpcList,
 * racing the expiry scan with fresh inserts. */
static
VOID
NTAPI
RearmDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    ULONG Index = (ULONG)(ULONG_PTR)DeferredContext;
    static volatile LONG RearmSeed = 0x1234567;
    LARGE_INTEGER Due;
    LONG s;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (InterlockedCompareExchange((PLONG)&g_Stop, 0, 0))
        return;

    /* Cheap shared PRNG; collisions don't matter */
    s = InterlockedIncrement(&RearmSeed);
    Due.QuadPart = -(LONGLONG)(((ULONG)s * 2654435761u) % (20 * 10 * 1000) + 1);

    KeSetTimerEx(&g_RearmTimers[Index], Due, 0, &g_RearmDpcs[Index]);
    InterlockedIncrement((PLONG)&g_DpcRearms);
}

/* Setter: hammer private + shared timers with set/cancel/wait mixes */
static
VOID
NTAPI
SetterThread(PVOID Ctx)
{
    PSETTER_CTX Me = Ctx;
    ULONG i = 0;

    while (!InterlockedCompareExchange((PLONG)&g_Stop, 0, 0))
    {
        ULONG r = NextRand(&Me->Seed);
        PKTIMER Timer;
        LARGE_INTEGER Due;
        LONG Period = 0;

        /* 1 in 4 ops goes to a shared timer (cross-CPU same-object races) */
        if ((r & 3) == 0)
            Timer = &g_SharedTimers[NextRand(&Me->Seed) % N_SHARED_TIMERS];
        else
            Timer = &Me->Timers[i++ % TIMERS_PER_SETTER];

        Due.QuadPart = RandomRelativeDue(&Me->Seed);

        /* 1 in 8: periodic 1..15ms */
        if (((r >> 8) & 7) == 0)
            Period = 1 + (NextRand(&Me->Seed) % 15);

        KeSetTimerEx(Timer, Due, Period, NULL);
        InterlockedIncrement((PLONG)&g_SetOps);

        switch ((r >> 16) & 7)
        {
            case 0:
            case 1:
            case 2:
                /* Immediate cancel: cancel-vs-expiry race */
                KeCancelTimer(Timer);
                InterlockedIncrement((PLONG)&g_CancelOps);
                break;

            case 3:
            case 4:
            {
                /* Short bounded wait on the timer: waiter-list + thread
                 * wait-timer interplay with expiry */
                LARGE_INTEGER Timeout;
                Timeout.QuadPart = -(LONGLONG)(NextRand(&Me->Seed) % (5 * 10 * 1000) + 1);
                KeWaitForSingleObject(Timer, Executive, KernelMode, FALSE, &Timeout);
                InterlockedIncrement((PLONG)&g_WaitOps);
                /* Periodic timers must not be left running from a slice
                 * that another op may reuse immediately */
                if (Period != 0)
                    KeCancelTimer(Timer);
                break;
            }

            default:
                /* Leave it to expire on its own; cancel periodic later */
                if (Period != 0)
                    KeCancelTimer(Timer);
                break;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Delayer: tight tiny sleeps - highest-rate thread-wait timer inserts */
static
VOID
NTAPI
DelayerThread(PVOID Ctx)
{
    ULONG Seed = (ULONG)(ULONG_PTR)Ctx | 1;

    while (!InterlockedCompareExchange((PLONG)&g_Stop, 0, 0))
    {
        LARGE_INTEGER Interval;
        /* 0 .. 2ms */
        Interval.QuadPart = -(LONGLONG)(NextRand(&Seed) % (2 * 10 * 1000 + 1));
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        InterlockedIncrement((PLONG)&g_DelayOps);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* One-shot: set a short timer on the stack, wait for it, die.
 * Timer + wait block + thread teardown churn. */
static
VOID
NTAPI
OneShotThread(PVOID Ctx)
{
    KTIMER Timer;
    LARGE_INTEGER Due;
    ULONG Seed = (ULONG)(ULONG_PTR)Ctx | 1;

    KeInitializeTimer(&Timer);
    Due.QuadPart = -(LONGLONG)(NextRand(&Seed) % (3 * 10 * 1000) + 1);
    KeSetTimer(&Timer, Due, NULL);
    KeWaitForSingleObject(&Timer, Executive, KernelMode, FALSE, NULL);
    /* Stack KTIMER must be off the wheel before this frame dies */
    KeCancelTimer(&Timer);

    InterlockedDecrement((PLONG)&g_OneShotLive);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
NTAPI
ChurnerThread(PVOID Ctx)
{
    ULONG Seed = 0xC0FFEE;

    UNREFERENCED_PARAMETER(Ctx);

    while (!InterlockedCompareExchange((PLONG)&g_Stop, 0, 0))
    {
        HANDLE h;
        OBJECT_ATTRIBUTES oa;
        NTSTATUS st;

        if (InterlockedCompareExchange((PLONG)&g_OneShotLive, 0, 0) >= ONESHOT_MAX)
        {
            LARGE_INTEGER d;
            d.QuadPart = -1 * 10 * 1000; /* 1ms back-off */
            KeDelayExecutionThread(KernelMode, FALSE, &d);
            continue;
        }

        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        InterlockedIncrement((PLONG)&g_OneShotLive);
        st = PsCreateSystemThread(&h, SYNCHRONIZE, &oa, NULL, NULL,
                                  OneShotThread, (PVOID)(ULONG_PTR)NextRand(&Seed));
        if (NT_SUCCESS(st))
        {
            ObCloseHandle(h, KernelMode);
            InterlockedIncrement((PLONG)&g_ChurnThreads);
        }
        else
        {
            LARGE_INTEGER d;
            InterlockedDecrement((PLONG)&g_OneShotLive);
            d.QuadPart = -1 * 10 * 1000;
            KeDelayExecutionThread(KernelMode, FALSE, &d);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(KeTimerTableStress)
{
    PKTHREAD setters[MAX_SETTERS] = { NULL };
    PKTHREAD delayers[MAX_DELAYERS] = { NULL };
    PKTHREAD churner;
    LARGE_INTEGER run;
    ULONG nSetters, nDelayers, i;
    CCHAR nCpus = KeNumberProcessors;

    g_Stop = 0;
    g_SetOps = g_CancelOps = g_WaitOps = g_DelayOps = 0;
    g_DpcRearms = g_OneShotLive = g_ChurnThreads = 0;

    nSetters = min(2 * (ULONG)nCpus, MAX_SETTERS);
    nDelayers = min((ULONG)nCpus, MAX_DELAYERS);

    trace("Timer-table stress: %u CPUs, %lu setters, %lu delayers, %ds\n",
          (ULONG)nCpus, nSetters, nDelayers, STRESS_SECONDS);

    for (i = 0; i < N_SHARED_TIMERS; ++i)
        KeInitializeTimerEx(&g_SharedTimers[i],
                            (i & 1) ? SynchronizationTimer : NotificationTimer);

    /* Self-re-arming DPC timers, spread across all processors */
    for (i = 0; i < N_REARM_TIMERS; ++i)
    {
        LARGE_INTEGER Due;
        KeInitializeTimer(&g_RearmTimers[i]);
        KeInitializeDpc(&g_RearmDpcs[i], RearmDpcRoutine, (PVOID)(ULONG_PTR)i);
        KeSetTargetProcessorDpc(&g_RearmDpcs[i], (CCHAR)(i % nCpus));
        Due.QuadPart = -(LONGLONG)(10 * 1000 * (i + 1)); /* 1..8ms */
        KeSetTimerEx(&g_RearmTimers[i], Due, 0, &g_RearmDpcs[i]);
    }

    for (i = 0; i < nSetters; ++i)
    {
        ULONG t;
        g_Setters[i].Seed = 0x9E3779B9u * (i + 1) | 1;
        for (t = 0; t < TIMERS_PER_SETTER; ++t)
            KeInitializeTimerEx(&g_Setters[i].Timers[t],
                                (t & 1) ? SynchronizationTimer : NotificationTimer);
        setters[i] = KmtStartThread(SetterThread, &g_Setters[i]);
    }
    for (i = 0; i < nDelayers; ++i)
        delayers[i] = KmtStartThread(DelayerThread, (PVOID)(ULONG_PTR)(0xD00D + i));
    churner = KmtStartThread(ChurnerThread, NULL);

    run.QuadPart = -1LL * STRESS_SECONDS * 1000 * 1000 * 10;
    KeDelayExecutionThread(KernelMode, FALSE, &run);

    InterlockedExchange((PLONG)&g_Stop, 1);
    trace("Stopping: set=%ld cancel=%ld wait=%ld delay=%ld rearm=%ld churn=%ld\n",
          g_SetOps, g_CancelOps, g_WaitOps, g_DelayOps, g_DpcRearms,
          g_ChurnThreads);

    for (i = 0; i < nSetters; ++i)
        if (setters[i]) KmtFinishThread(setters[i], NULL);
    for (i = 0; i < nDelayers; ++i)
        if (delayers[i]) KmtFinishThread(delayers[i], NULL);
    if (churner) KmtFinishThread(churner, NULL);

    /* Let in-flight DPCs finish (they see g_Stop and won't re-arm),
     * then take everything off the wheel */
    run.QuadPart = -1 * 200 * 1000 * 10; /* 200ms */
    KeDelayExecutionThread(KernelMode, FALSE, &run);

    for (i = 0; i < N_REARM_TIMERS; ++i)
        KeCancelTimer(&g_RearmTimers[i]);
    for (i = 0; i < N_SHARED_TIMERS; ++i)
        KeCancelTimer(&g_SharedTimers[i]);
    for (i = 0; i < nSetters; ++i)
    {
        ULONG t;
        for (t = 0; t < TIMERS_PER_SETTER; ++t)
            KeCancelTimer(&g_Setters[i].Timers[t]);
    }

    /* Drain remaining one-shot threads */
    run.QuadPart = -1 * 2 * 1000 * 1000 * 10; /* 2s */
    KeDelayExecutionThread(KernelMode, FALSE, &run);

    ok(g_SetOps > 0, "no timer sets happened (%ld)\n", g_SetOps);
    ok(g_DpcRearms > 0, "no DPC re-arms happened (%ld)\n", g_DpcRearms);
    ok(TRUE, "survived timer-table stress without crashing\n");
}
