/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     SMP stress for the per-thread lock order between the APC
 *              path and the affinity/priority paths. Kernel and user APCs
 *              are queued to the threads of a process from one set of
 *              processors (KeInsertQueueApc holds the target's APC lock,
 *              which is the thread lock on the NT 6.2+ KTHREAD layout,
 *              and then signals the target) while other processors churn
 *              the process affinity (KeSetAffinityProcess), the thread
 *              affinity and priority (KeSetAffinityThread,
 *              KeSetPriorityThread, KeSetBasePriorityThread) of the same
 *              threads, and the targets flip their own system affinity
 *              (KeSetSystemAffinityThread). A kernel whose paths take the
 *              thread lock and the dispatcher lock in different orders
 *              deadlocks: with two processors both spin forever and the
 *              machine stops; with three or more the stalled workers are
 *              reported here.
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define STRESS_SECONDS      20
#define STALL_SECONDS       5
#define MAX_WORKER_CPUS     16
#define MAX_TARGETS         16
#define APCS_PER_INSERTER   32

typedef struct _APC_SLOT
{
    KAPC Apc;
    volatile LONG Busy;
} APC_SLOT, *PAPC_SLOT;

typedef struct _WORKER
{
    PKTHREAD Thread;
    ULONG Cpu;
    ULONG Seed;
    volatile LONG Ops;
    APC_SLOT Slots[APCS_PER_INSERTER];
} WORKER, *PWORKER;

typedef struct _TARGET
{
    PKTHREAD Thread;
    ULONG Seed;
    volatile LONG Ops;
    APC_SLOT UserSlot;
} TARGET, *PTARGET;

static WORKER g_Workers[MAX_WORKER_CPUS];
static TARGET g_Targets[MAX_TARGETS];
static ULONG g_nWorkers;
static ULONG g_nTargets;
static KAFFINITY g_ProcessAffinity;
static volatile LONG g_Stop;
static volatile LONG g_TargetsStop;
static volatile LONG g_Inserted;
static volatile LONG g_Delivered;
static volatile LONG g_RunDown;
static volatile LONG g_ProcessOps;
static volatile LONG g_ThreadOps;
static volatile LONG g_SystemAffinityOps;

static
ULONG
NextRand(PULONG Seed)
{
    ULONG x = *Seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *Seed = x;
    return x;
}

static
VOID
NTAPI
ApcNormalRoutine(
    _In_ PVOID NormalContext,
    _In_ PVOID SystemArgument1,
    _In_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    InterlockedIncrement(&g_Delivered);
}

static
VOID
NTAPI
ApcKernelRoutine(
    _In_ PKAPC Apc,
    _Inout_ PKNORMAL_ROUTINE *NormalRoutine,
    _Inout_ PVOID *NormalContext,
    _Inout_ PVOID *SystemArgument1,
    _Inout_ PVOID *SystemArgument2)
{
    PAPC_SLOT Slot = CONTAINING_RECORD(Apc, APC_SLOT, Apc);

    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* KiDeliverApc has copied everything it needs out of the KAPC */
    if (*NormalRoutine == NULL)
        InterlockedIncrement(&g_Delivered);
    InterlockedExchange(&Slot->Busy, 0);
}

static
VOID
NTAPI
ApcRundownRoutine(
    _In_ PKAPC Apc)
{
    PAPC_SLOT Slot = CONTAINING_RECORD(Apc, APC_SLOT, Apc);

    InterlockedIncrement(&g_RunDown);
    InterlockedExchange(&Slot->Busy, 0);
}

static
BOOLEAN
QueueApc(
    _In_ PAPC_SLOT Slot,
    _In_ PKTHREAD Target,
    _In_ KPROCESSOR_MODE Mode,
    _In_ BOOLEAN Special)
{
    if (InterlockedCompareExchange(&Slot->Busy, 1, 0) != 0)
        return FALSE;

    KeInitializeApc(&Slot->Apc,
                    Target,
                    OriginalApcEnvironment,
                    ApcKernelRoutine,
                    ApcRundownRoutine,
                    Special ? NULL : ApcNormalRoutine,
                    Mode,
                    Slot);
    if (!KeInsertQueueApc(&Slot->Apc, NULL, NULL, IO_NO_INCREMENT))
    {
        InterlockedExchange(&Slot->Busy, 0);
        return FALSE;
    }

    InterlockedIncrement(&g_Inserted);
    return TRUE;
}

/* Receives the APCs; waits most of the time, sometimes runs, sometimes
 * flips its own system affinity */
static
VOID
NTAPI
TargetThread(
    _In_ PVOID Context)
{
    PTARGET Target = Context;
    LARGE_INTEGER Interval;
    ULONG r, Spin;
    KAFFINITY Active = KeQueryActiveProcessors();
    ULONG Cpu;

    while (!g_TargetsStop)
    {
        r = NextRand(&Target->Seed);
        if ((r & 7) == 0)
        {
            /* Hop to a random processor and back */
            Cpu = (r >> 8) % (ULONG)KeNumberProcessors;
            if (Active & AFFINITY_MASK(Cpu))
            {
                KeSetSystemAffinityThread(AFFINITY_MASK(Cpu));
                KeRevertToUserAffinityThread();
                InterlockedIncrement(&g_SystemAffinityOps);
            }
        }
        else if ((r & 7) == 1)
        {
            /* Stay Running for a while */
            for (Spin = 0; Spin < 2000; Spin++)
                YieldProcessor();
        }
        else
        {
            /* Wait at PASSIVE_LEVEL so a kernel APC unwaits us */
            Interval.QuadPart = -10 * 1000;
            KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        }
        InterlockedIncrement(&Target->Ops);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Queues APCs to the targets from its own processor */
static
VOID
NTAPI
InserterThread(
    _In_ PVOID Context)
{
    PWORKER Worker = Context;
    PTARGET Target;
    ULONG i, r;

    KeSetSystemAffinityThread(AFFINITY_MASK(Worker->Cpu));

    while (!g_Stop)
    {
        for (i = 0; i < APCS_PER_INSERTER; i++)
        {
            r = NextRand(&Worker->Seed);
            Target = &g_Targets[r % g_nTargets];
            QueueApc(&Worker->Slots[i], Target->Thread, KernelMode, (r >> 16) & 1);

            /* A user APC stays queued (the targets never return to user
             * mode) until the target exits, so each target gets one */
            if (((r >> 8) & 0xFF) == 0)
                QueueApc(&Target->UserSlot, Target->Thread, UserMode, FALSE);
        }
        InterlockedIncrement(&Worker->Ops);
    }

    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Churns the process and thread affinity and priority from its own
 * processor */
static
VOID
NTAPI
ChurnerThread(
    _In_ PVOID Context)
{
    PWORKER Worker = Context;
    PTARGET Target;
    KAFFINITY Affinity;
    NTSTATUS Status;
    ULONG r;

    KeSetSystemAffinityThread(AFFINITY_MASK(Worker->Cpu));

    while (!g_Stop)
    {
        r = NextRand(&Worker->Seed);
        Target = &g_Targets[(r >> 4) % g_nTargets];
        switch (r & 3)
        {
            case 0:
                /* Same mask every time: the process affinity must stay a
                 * superset of every thread affinity set below */
                Status = ZwSetInformationProcess(ZwCurrentProcess(),
                                                 ProcessAffinityMask,
                                                 &g_ProcessAffinity,
                                                 sizeof(g_ProcessAffinity));
                ok_eq_hex(Status, STATUS_SUCCESS);
                InterlockedIncrement(&g_ProcessOps);
                break;

            case 1:
                Affinity = g_ProcessAffinity & (KAFFINITY)(r >> 8);
                if (Affinity == 0)
                    Affinity = g_ProcessAffinity;
                KeSetAffinityThread(Target->Thread, Affinity);
                InterlockedIncrement(&g_ThreadOps);
                break;

            case 2:
                KeSetPriorityThread(Target->Thread, 8 + (KPRIORITY)((r >> 8) % 4));
                InterlockedIncrement(&g_ThreadOps);
                break;

            case 3:
                KeSetBasePriorityThread(Target->Thread, (LONG)((r >> 8) % 3) - 1);
                InterlockedIncrement(&g_ThreadOps);
                break;
        }
        InterlockedIncrement(&Worker->Ops);
    }

    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Create a thread in the current (test) process, not in the System
 * process, so ZwCurrentProcess() in the churner names the targets'
 * process */
static
PKTHREAD
StartThread(
    _In_ PKSTART_ROUTINE Routine,
    _In_ PVOID Context)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle;
    PVOID Thread = NULL;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Handle,
                                  THREAD_ALL_ACCESS,
                                  &ObjectAttributes,
                                  ZwCurrentProcess(),
                                  NULL,
                                  Routine,
                                  Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;

    Status = ObReferenceObjectByHandle(Handle,
                                       SYNCHRONIZE,
                                       *PsThreadType,
                                       KernelMode,
                                       &Thread,
                                       NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ObCloseHandle(Handle, KernelMode);
    return Thread;
}

static
BOOLEAN
JoinThread(
    _In_ PKTHREAD Thread)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Timeout.QuadPart = -30LL * 1000 * 1000 * 10;
    Status = KeWaitForSingleObject(Thread, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
        return FALSE;

    ObDereferenceObject(Thread);
    return TRUE;
}

START_TEST(KeApcAffinityStress)
{
    PROCESS_BASIC_INFORMATION BasicInfo;
    LONG LastOps[MAX_WORKER_CPUS + MAX_TARGETS];
    ULONG Stalled[MAX_WORKER_CPUS + MAX_TARGETS];
    LARGE_INTEGER Interval;
    NTSTATUS Status;
    BOOLEAN Deadlock = FALSE;
    ULONG nCpus = (ULONG)KeNumberProcessors;
    ULONG i, Second;
    LONG Ops;

    if (skip(nCpus >= 2, "Needs at least two processors\n"))
        return;

    if (skip(PsGetCurrentProcess() != PsInitialSystemProcess,
             "Must run in the context of the test process\n"))
        return;

    Status = ZwQueryInformationProcess(ZwCurrentProcess(),
                                       ProcessBasicInformation,
                                       &BasicInfo,
                                       sizeof(BasicInfo),
                                       NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    g_ProcessAffinity = BasicInfo.AffinityMask;
    if (skip((g_ProcessAffinity & (g_ProcessAffinity - 1)) != 0,
             "Process affinity 0x%Ix has a single processor\n", g_ProcessAffinity))
        return;

    RtlZeroMemory(g_Workers, sizeof(g_Workers));
    RtlZeroMemory(g_Targets, sizeof(g_Targets));
    g_Stop = g_TargetsStop = 0;
    g_Inserted = g_Delivered = g_RunDown = 0;
    g_ProcessOps = g_ThreadOps = g_SystemAffinityOps = 0;
    g_nWorkers = min(nCpus, MAX_WORKER_CPUS);
    g_nTargets = min(2 * nCpus, MAX_TARGETS);

    trace("APC/affinity stress: %lu CPUs, process affinity 0x%Ix, %lu targets, %ds\n",
          nCpus, g_ProcessAffinity, g_nTargets, STRESS_SECONDS);

    for (i = 0; i < g_nTargets; i++)
    {
        g_Targets[i].Seed = 0x9E3779B9u * (i + 1) | 1;
        g_Targets[i].Thread = StartThread(TargetThread, &g_Targets[i]);
        if (!g_Targets[i].Thread)
            return;
    }

    /* Even processors queue APCs, odd ones churn affinity and priority */
    for (i = 0; i < g_nWorkers; i++)
    {
        g_Workers[i].Cpu = i;
        g_Workers[i].Seed = 0x85EBCA6Bu * (i + 1) | 1;
        g_Workers[i].Thread = StartThread((i & 1) ? ChurnerThread : InserterThread,
                                          &g_Workers[i]);
        if (!g_Workers[i].Thread)
            return;
    }

    /* Every worker and target must keep making progress */
    RtlZeroMemory(Stalled, sizeof(Stalled));
    for (i = 0; i < g_nWorkers + g_nTargets; i++)
        LastOps[i] = 0;
    for (Second = 0; Second < STRESS_SECONDS && !Deadlock; Second++)
    {
        Interval.QuadPart = -1000LL * 1000 * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);

        for (i = 0; i < g_nWorkers + g_nTargets; i++)
        {
            Ops = (i < g_nWorkers) ? g_Workers[i].Ops : g_Targets[i - g_nWorkers].Ops;
            Stalled[i] = (Ops == LastOps[i]) ? Stalled[i] + 1 : 0;
            LastOps[i] = Ops;
            if (Stalled[i] >= STALL_SECONDS)
            {
                ok(FALSE, "%s %lu made no progress for %lu s (deadlock)\n",
                   (i < g_nWorkers) ? ((i & 1) ? "Churner" : "Inserter") : "Target",
                   (i < g_nWorkers) ? i : i - g_nWorkers, Stalled[i]);
                Deadlock = TRUE;
            }
        }
    }

    trace("inserted=%ld delivered=%ld rundown=%ld process=%ld thread=%ld sysaff=%ld\n",
          g_Inserted, g_Delivered, g_RunDown, g_ProcessOps, g_ThreadOps,
          g_SystemAffinityOps);

    /* The stuck processors hold the locks; nothing below can complete */
    if (Deadlock)
        return;

    InterlockedExchange(&g_Stop, 1);
    for (i = 0; i < g_nWorkers; i++)
        JoinThread(g_Workers[i].Thread);

    /* Targets drain their kernel APCs in their waits; queued user APCs are
     * run down when they exit */
    InterlockedExchange(&g_TargetsStop, 1);
    for (i = 0; i < g_nTargets; i++)
        JoinThread(g_Targets[i].Thread);

    for (i = 0; i < g_nWorkers; i++)
    {
        ULONG s;
        for (s = 0; s < APCS_PER_INSERTER; s++)
            ok(g_Workers[i].Slots[s].Busy == 0, "Worker %lu APC %lu still queued\n", i, s);
    }
    for (i = 0; i < g_nTargets; i++)
        ok(g_Targets[i].UserSlot.Busy == 0, "Target %lu user APC still queued\n", i);

    ok(g_Inserted > 0, "No APC was queued\n");
    ok(g_ProcessOps > 0, "No process affinity change\n");
    ok(g_ThreadOps > 0, "No thread affinity/priority change\n");
}
