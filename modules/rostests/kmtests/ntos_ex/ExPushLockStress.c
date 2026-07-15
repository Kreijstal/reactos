/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Stress ExfWakePushLock wait-chain walking + thread teardown +
 *              concurrent nonpaged-pool churn, to try to reproduce the
 *              intermittent 0xC0000005 in ExfWakePushLock+0xf1 seen during the
 *              cygwin gcc/ld toolchain churn. A contended
 *              exclusive push lock constantly builds and collapses its wait
 *              block chain (each wait block lives on a waiter's kernel stack);
 *              rapidly created/destroyed one-shot waiter threads exercise the
 *              teardown path; concurrent NTFS/MM-tagged pool alloc/free stirs
 *              the nonpaged pool the way the real workload does. If the kernel
 *              bug is present this should crash walking a wild wait-block Next
 *              pointer; a clean kernel completes and passes.
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

/* Executive push lock primitives (not in the public DDK) */
NTKERNELAPI VOID FASTCALL ExfAcquirePushLockExclusive(_Inout_ PEX_PUSH_LOCK PushLock);
NTKERNELAPI VOID FASTCALL ExfReleasePushLockExclusive(_Inout_ PEX_PUSH_LOCK PushLock);
NTKERNELAPI VOID FASTCALL ExfAcquirePushLockShared(_Inout_ PEX_PUSH_LOCK PushLock);
NTKERNELAPI VOID FASTCALL ExfReleasePushLockShared(_Inout_ PEX_PUSH_LOCK PushLock);

static EX_PUSH_LOCK g_Lock;
static volatile LONG g_Stop;
static volatile LONG g_ContendIters;
static volatile LONG g_ChurnThreads;
static volatile LONG g_PoolOps;

#define N_CONTENDERS    6       /* persistent exclusive contenders */
#define N_SHARERS       4       /* persistent shared contenders */
#define POOL_TAGS_N     4
static const ULONG PoolTags[POOL_TAGS_N] =
{
    'PAMR',   /* TAG_RMAP "RMAP" */
    ' ftN',   /* NTFS-ish "Ntf " */
    'tSmM',   /* MM section "MmSt" */
    'SSMM',   /* "MMSS" */
};

/* Persistent exclusive contender: tight acquire/release to keep the wait
 * chain churning through ExfWakePushLock on every release. */
static VOID NTAPI ContendExclusive(PVOID Ctx)
{
    ULONG Spin;
    UNREFERENCED_PARAMETER(Ctx);
    while (!InterlockedCompareExchange((PLONG)&g_Stop, 0, 0))
    {
        ExfAcquirePushLockExclusive(&g_Lock);
        /* tiny critical section so others queue behind us */
        for (Spin = 0; Spin < 20; ++Spin)
            KeMemoryBarrier();
        ExfReleasePushLockExclusive(&g_Lock);
        InterlockedIncrement((PLONG)&g_ContendIters);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID NTAPI ContendShared(PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    while (!InterlockedCompareExchange((PLONG)&g_Stop, 0, 0))
    {
        ExfAcquirePushLockShared(&g_Lock);
        KeMemoryBarrier();
        ExfReleasePushLockShared(&g_Lock);
        InterlockedIncrement((PLONG)&g_ContendIters);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* One-shot waiter: acquires once (usually blocking behind the exclusive
 * contenders, forming a wait block on THIS thread's stack) then exits
 * immediately -- exercising thread teardown right after wait-chain use. */
static VOID NTAPI OneShotWaiter(PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    ExfAcquirePushLockExclusive(&g_Lock);
    ExfReleasePushLockExclusive(&g_Lock);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Churner: rapidly create + wait-and-forget one-shot waiter threads. */
static VOID NTAPI Churner(PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    while (!InterlockedCompareExchange((PLONG)&g_Stop, 0, 0))
    {
        HANDLE h;
        OBJECT_ATTRIBUTES oa;
        NTSTATUS st;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        st = PsCreateSystemThread(&h, SYNCHRONIZE, &oa, NULL, NULL, OneShotWaiter, NULL);
        if (NT_SUCCESS(st))
        {
            ObCloseHandle(h, KernelMode);   /* detach; let it run + self-terminate */
            InterlockedIncrement((PLONG)&g_ChurnThreads);
        }
        else
        {
            LARGE_INTEGER d; d.QuadPart = -1 * 10 * 1000; /* 1ms back-off */
            KeDelayExecutionThread(KernelMode, FALSE, &d);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Pool churn: allocate/free nonpaged pool with the exact tags observed in the
 * corruption (RMAP/Ntf/MmSt/MMSS), stirring the pool concurrently. */
static VOID NTAPI PoolChurn(PVOID Ctx)
{
    ULONG seed = (ULONG)(ULONG_PTR)Ctx | 1;
    while (!InterlockedCompareExchange((PLONG)&g_Stop, 0, 0))
    {
        PVOID p[8];
        ULONG i;
        for (i = 0; i < 8; ++i)
        {
            SIZE_T sz = 16 + ((seed >> 3) & 0x3FF);
            seed = seed * 1103515245 + 12345;
            p[i] = ExAllocatePoolWithTag(NonPagedPool, sz, PoolTags[i & (POOL_TAGS_N - 1)]);
            if (p[i]) RtlFillMemory(p[i], sz, (UCHAR)seed);
        }
        for (i = 0; i < 8; ++i)
            if (p[i]) ExFreePoolWithTag(p[i], PoolTags[i & (POOL_TAGS_N - 1)]);
        InterlockedIncrement((PLONG)&g_PoolOps);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(ExPushLockStress)
{
    PKTHREAD contenders[N_CONTENDERS];
    PKTHREAD sharers[N_SHARERS];
    PKTHREAD churner, pool0, pool1;
    LARGE_INTEGER run;
    ULONG i;

    RtlZeroMemory(&g_Lock, sizeof(g_Lock));  /* EX_PUSH_LOCK: 0 == unowned */
    g_Stop = 0;
    g_ContendIters = g_ChurnThreads = g_PoolOps = 0;

    trace("Starting push-lock wait-chain / teardown / pool stress\n");

    for (i = 0; i < N_CONTENDERS; ++i)
        contenders[i] = KmtStartThread(ContendExclusive, NULL);
    for (i = 0; i < N_SHARERS; ++i)
        sharers[i] = KmtStartThread(ContendShared, NULL);
    churner = KmtStartThread(Churner, NULL);
    pool0 = KmtStartThread(PoolChurn, (PVOID)1);
    pool1 = KmtStartThread(PoolChurn, (PVOID)2);

    /* Run the storm for ~30 seconds. */
    run.QuadPart = -1 * 30 * 1000 * 1000 * 10; /* 30s */
    KeDelayExecutionThread(KernelMode, FALSE, &run);

    InterlockedExchange((PLONG)&g_Stop, 1);
    trace("Stopping: contendIters=%ld churnThreads=%ld poolOps=%ld\n",
          g_ContendIters, g_ChurnThreads, g_PoolOps);

    /* Join persistent workers. */
    for (i = 0; i < N_CONTENDERS; ++i)
        if (contenders[i]) { KmtFinishThread(contenders[i], NULL); }
    for (i = 0; i < N_SHARERS; ++i)
        if (sharers[i]) { KmtFinishThread(sharers[i], NULL); }
    if (churner) KmtFinishThread(churner, NULL);
    if (pool0) KmtFinishThread(pool0, NULL);
    if (pool1) KmtFinishThread(pool1, NULL);

    /* Give any still-running one-shot waiters time to drain. */
    run.QuadPart = -1 * 2 * 1000 * 1000 * 10; /* 2s */
    KeDelayExecutionThread(KernelMode, FALSE, &run);

    /* If we got here without a bugcheck the wait-chain survived the storm. */
    ok(g_ContendIters > 0, "no contention happened (%ld)\n", g_ContendIters);
    ok(TRUE, "survived push-lock stress without crashing\n");
}
