/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for readying threads on a
 *              processor whose next thread is its idle thread
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

/*
 * When the thread on standby for a processor may no longer run there (its
 * affinity changed) and nothing else is ready there, the processor gets its
 * idle thread as the next thread. That processor can be idle already, so its
 * next thread is then its current thread, and it keeps running DPCs that ready
 * threads for it. Or it runs a thread the one on standby was about to
 * preempt, and that thread now gives the processor up to the idle thread.
 *
 * A DPC on the target processor sets this up without a race: it wakes a thread
 * that may only run there, which goes on standby there, moves that thread to
 * another processor, and then (in some of the cases) wakes a second thread
 * that may only run there. Both threads must run, each where it may, and a
 * thread that ran on the target processor before must go on running at once.
 */

#define DPC_RETRIES 200
#define WAKE_LIMIT_MS 500
#define RUN_LIMIT_MS 500
#define BUSY_LIMIT_MS 100
/* Below one clock tick: a thread nobody takes off the ready list runs when
 * some tick-driven work reaches the processor */
#define BUSY_RESUME_US 10000

typedef struct _IDLE_STANDBY
{
    ULONG Target;
    ULONG Other;
    BOOLEAN WakeSecond;
    PKTHREAD Standby;
    PKTHREAD Busy;
    KDPC Dpc;
    KEVENT StandbyReady;
    KEVENT StandbyWake;
    KEVENT SecondReady;
    KEVENT SecondWake;
    KEVENT BusyReady;
    volatile LONG DpcState;
    volatile LONG StandbyRan;
    volatile LONG SecondRan;
    volatile LONG StopBusy;
    volatile LONG BusyCount;
    LONG BusyAtDpc;
    LONGLONG DpcCounter;
    ULONG StandbyRanOn;
    ULONG SecondRanOn;
    ULONGLONG DpcTime;
    ULONGLONG StandbyTime;
    ULONGLONG SecondTime;
} IDLE_STANDBY, *PIDLE_STANDBY;

#define DPC_PENDING 0
#define DPC_RETRY 1
#define DPC_DONE 2

static
VOID
NTAPI
StandbyThread(
    _In_ PVOID Context)
{
    PIDLE_STANDBY Data = Context;

    KeSetAffinityThread(KeGetCurrentThread(), AFFINITY_MASK(Data->Target));
    KeSetEvent(&Data->StandbyReady, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(&Data->StandbyWake, Executive, KernelMode, FALSE, NULL);

    Data->StandbyTime = KeQueryInterruptTime();
    Data->StandbyRanOn = KeGetCurrentProcessorNumber();
    InterlockedExchange(&Data->StandbyRan, 1);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
NTAPI
SecondThread(
    _In_ PVOID Context)
{
    PIDLE_STANDBY Data = Context;

    KeSetAffinityThread(KeGetCurrentThread(), AFFINITY_MASK(Data->Target));
    KeSetEvent(&Data->SecondReady, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(&Data->SecondWake, Executive, KernelMode, FALSE, NULL);

    Data->SecondTime = KeQueryInterruptTime();
    Data->SecondRanOn = KeGetCurrentProcessorNumber();
    InterlockedExchange(&Data->SecondRan, 1);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
NTAPI
BusyThread(
    _In_ PVOID Context)
{
    PIDLE_STANDBY Data = Context;

    /* Below the woken threads, which then preempt it */
    KeSetAffinityThread(KeGetCurrentThread(), AFFINITY_MASK(Data->Target));
    KeSetPriorityThread(KeGetCurrentThread(), 2);
    KeSetEvent(&Data->BusyReady, IO_NO_INCREMENT, FALSE);

    /* Keep the processor at passive level */
    while (!Data->StopBusy)
    {
        InterlockedIncrement(&Data->BusyCount);
        YieldProcessor();
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
NTAPI
TargetDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PIDLE_STANDBY Data = DeferredContext;
    PKTHREAD Interrupted = KeGetCurrentThread();
    BOOLEAN Wanted;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* Run on top of the thread the case is about: the idle thread (that of
     * process 0) or the busy one */
    if (Data->Busy)
        Wanted = (Interrupted == Data->Busy);
    else
        Wanted = (PsGetCurrentProcessId() == (HANDLE)0);
    if (!Wanted || KeGetCurrentProcessorNumber() != Data->Target)
    {
        InterlockedExchange(&Data->DpcState, DPC_RETRY);
        return;
    }

    Data->DpcTime = KeQueryInterruptTime();

    /* This thread may only run here: it goes on standby here */
    KeSetEvent(&Data->StandbyWake, IO_NO_INCREMENT, FALSE);

    /* Now it may not run here any more: nothing else is ready here, so the
     * idle thread becomes the next thread */
    KeSetAffinityThread(Data->Standby, AFFINITY_MASK(Data->Other));

    /* Ready another thread for this processor */
    if (Data->WakeSecond)
        KeSetEvent(&Data->SecondWake, IO_NO_INCREMENT, FALSE);

    /* The busy thread is not running while this DPC is */
    Data->BusyAtDpc = Data->BusyCount;
    Data->DpcCounter = KeQueryPerformanceCounter(NULL).QuadPart;
    InterlockedExchange(&Data->DpcState, DPC_DONE);
}

static
PKTHREAD
StartThread(
    _In_ PKSTART_ROUTINE Routine,
    _In_ PVOID Context)
{
    HANDLE Handle;
    PVOID Thread;
    NTSTATUS Status;

    Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, NULL, NULL, NULL, Routine, Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;
    Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, *PsThreadType, KernelMode, &Thread, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Handle);
    return NT_SUCCESS(Status) ? Thread : NULL;
}

static
VOID
FinishThread(
    _In_opt_ PKTHREAD Thread)
{
    if (Thread)
    {
        KeWaitForSingleObject(Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Thread);
    }
}

static
VOID
Sleep10ms(VOID)
{
    LARGE_INTEGER Delay;

    Delay.QuadPart = -10 * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

static
VOID
TestIdleStandby(
    _In_ ULONG Target,
    _In_ ULONG Other,
    _In_ BOOLEAN WithBusy,
    _In_ BOOLEAN WakeSecond)
{
    static IDLE_STANDBY Data;
    PKTHREAD Second = NULL;
    LONG BusyBefore, BusyAfter;
    ULONGLONG Until, BusyResume;
    ULONG i;

    RtlZeroMemory(&Data, sizeof(Data));
    Data.Target = Target;
    Data.Other = Other;
    Data.WakeSecond = WakeSecond;
    KeInitializeEvent(&Data.StandbyReady, NotificationEvent, FALSE);
    KeInitializeEvent(&Data.StandbyWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Data.SecondReady, NotificationEvent, FALSE);
    KeInitializeEvent(&Data.SecondWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Data.BusyReady, NotificationEvent, FALSE);
    KeInitializeDpc(&Data.Dpc, TargetDpc, &Data);
    KeSetTargetProcessorDpc(&Data.Dpc, (CCHAR)Target);
    KeSetImportanceDpc(&Data.Dpc, HighImportance);

    /* The threads to wake wait, pinned to the target processor */
    Data.Standby = StartThread(StandbyThread, &Data);
    if (!Data.Standby)
        return;
    KeWaitForSingleObject(&Data.StandbyReady, Executive, KernelMode, FALSE, NULL);
    if (WakeSecond)
    {
        Second = StartThread(SecondThread, &Data);
        if (Second)
            KeWaitForSingleObject(&Data.SecondReady, Executive, KernelMode, FALSE, NULL);
    }
    if (WithBusy)
    {
        Data.Busy = StartThread(BusyThread, &Data);
        if (Data.Busy)
            KeWaitForSingleObject(&Data.BusyReady, Executive, KernelMode, FALSE, NULL);
    }

    /* Let the target processor settle, then run the DPC on it until it lands
     * on top of the wanted thread */
    Sleep10ms();
    for (i = 0; i < DPC_RETRIES; i++)
    {
        InterlockedExchange(&Data.DpcState, DPC_PENDING);
        KeInsertQueueDpc(&Data.Dpc, NULL, NULL);
        while (Data.DpcState == DPC_PENDING)
            YieldProcessor();
        if (Data.DpcState == DPC_DONE)
            break;
        Sleep10ms();
    }
    ok(Data.DpcState == DPC_DONE, "Target %lu busy %d: the DPC never ran on the wanted thread\n",
       Target, WithBusy);
    if (Data.DpcState != DPC_DONE)
    {
        /* Wake the threads to clean up */
        KeSetEvent(&Data.StandbyWake, IO_NO_INCREMENT, FALSE);
        if (Second)
            KeSetEvent(&Data.SecondWake, IO_NO_INCREMENT, FALSE);
    }

    /* The busy thread must run again at once: the processor has nothing else
     * to run once the second thread, if any, is done. Watch it without
     * waiting, as a thread waiting here can unblock the processor. */
    BusyBefore = Data.BusyAtDpc;
    BusyResume = 0;
    if (Data.Busy && Data.DpcState == DPC_DONE)
    {
        LARGE_INTEGER Frequency, Now;

        KeQueryPerformanceCounter(&Frequency);
        Until = Data.DpcCounter + Frequency.QuadPart * BUSY_LIMIT_MS / 1000;
        do
        {
            YieldProcessor();
            Now = KeQueryPerformanceCounter(NULL);
        } while (Data.BusyCount == BusyBefore && (ULONGLONG)Now.QuadPart < Until);

        /* In microseconds */
        if (Data.BusyCount != BusyBefore)
            BusyResume = (Now.QuadPart - Data.DpcCounter) * 1000000 / Frequency.QuadPart;
        else
            BusyResume = MAXULONGLONG;
    }

    /* Every thread must run soon */
    for (i = 0; i < RUN_LIMIT_MS / 10; i++)
    {
        if (Data.StandbyRan &&
            (!Second || Data.SecondRan) &&
            (!Data.Busy || Data.BusyCount != BusyBefore))
            break;
        Sleep10ms();
    }
    BusyAfter = Data.BusyCount;

    if (Data.DpcState == DPC_DONE)
    {
        ok(Data.StandbyRan, "Target %lu busy %d: the moved thread did not run\n", Target, WithBusy);
        if (Data.StandbyRan)
        {
            ok(Data.StandbyRanOn == Other, "Target %lu busy %d: the moved thread ran on %lu, not %lu\n",
               Target, WithBusy, Data.StandbyRanOn, Other);
            ok((Data.StandbyTime - Data.DpcTime) / 10000 < WAKE_LIMIT_MS,
               "Target %lu busy %d: the moved thread ran %I64u ms late\n",
               Target, WithBusy, (Data.StandbyTime - Data.DpcTime) / 10000);
        }
        if (Second)
        {
            ok(Data.SecondRan, "Target %lu busy %d: the second thread did not run\n", Target, WithBusy);
            if (Data.SecondRan)
            {
                ok(Data.SecondRanOn == Target, "Target %lu busy %d: the second thread ran on %lu\n",
                   Target, WithBusy, Data.SecondRanOn);
                ok((Data.SecondTime - Data.DpcTime) / 10000 < WAKE_LIMIT_MS,
                   "Target %lu busy %d: the second thread ran %I64u ms late\n",
                   Target, WithBusy, (Data.SecondTime - Data.DpcTime) / 10000);
            }
        }
        if (Data.Busy)
        {
            ok(BusyAfter != BusyBefore,
               "Target %lu second %d: the preempted thread did not run again in %u ms\n",
               Target, WakeSecond, RUN_LIMIT_MS);
            ok(BusyResume < BUSY_RESUME_US,
               "Target %lu second %d: the preempted thread ran again after %I64u us\n",
               Target, WakeSecond, BusyResume);
        }
    }

    /* Clean up; a starved busy thread gets the processor back from the
     * waits here at the latest */
    InterlockedExchange(&Data.StopBusy, 1);
    FinishThread(Data.Busy);
    FinishThread(Second);
    FinishThread(Data.Standby);
}

START_TEST(KeIdleStandby)
{
    ULONG Target, Round;

    if (KeNumberProcessors < 2)
    {
        skip(FALSE, "Needs two processors\n");
        return;
    }

    /* Run from processor 0, which also takes the moved thread */
    KeSetSystemAffinityThread(AFFINITY_MASK(0));
    for (Round = 0; Round < 5; Round++)
    {
        for (Target = 1; Target < (ULONG)min(KeNumberProcessors, 4); Target++)
        {
            TestIdleStandby(Target, 0, FALSE, TRUE);
            TestIdleStandby(Target, 0, TRUE, FALSE);
            TestIdleStandby(Target, 0, TRUE, TRUE);
        }
    }
    KeRevertToUserAffinityThread();
}
