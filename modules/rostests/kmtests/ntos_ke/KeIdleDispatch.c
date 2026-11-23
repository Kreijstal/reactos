/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for readying a thread on an idle
 *              processor rather than on a busy one
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

/*
 * A thread that becomes ready goes to an idle processor when one is in its
 * affinity, before its ideal processor if that one is busy. Here the ideal
 * processor of the woken thread runs a higher-priority thread that does not
 * give it up, and another processor is idle: the woken thread must run at
 * once there, not queue up behind the busy thread.
 */

#define SPIN_MS 1500
#define WAKE_LIMIT_MS 200

typedef struct _IDLE_DISPATCH
{
    ULONG Busy;
    KEVENT Ready;
    KEVENT Wake;
    volatile LONG SpinnerRunning;
    volatile LONG StopSpinner;
    volatile LONG Woken;
    ULONGLONG WakeTime;
    ULONG WokenOn;
} IDLE_DISPATCH, *PIDLE_DISPATCH;

static
VOID
NTAPI
SpinnerThread(
    _In_ PVOID Context)
{
    PIDLE_DISPATCH Data = Context;
    ULONGLONG Until;

    KeSetSystemAffinityThread(AFFINITY_MASK(Data->Busy));
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY - 1);
    InterlockedExchange(&Data->SpinnerRunning, 1);

    /* Keep the processor at passive level: the clock keeps ticking */
    Until = KeQueryInterruptTime() + SPIN_MS * 10000ULL;
    while (!Data->StopSpinner && KeQueryInterruptTime() < Until)
        YieldProcessor();

    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
NTAPI
SleeperThread(
    _In_ PVOID Context)
{
    PIDLE_DISPATCH Data = Context;

    KeSetIdealProcessorThread(KeGetCurrentThread(), (UCHAR)Data->Busy);
    KeSetEvent(&Data->Ready, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(&Data->Wake, Executive, KernelMode, FALSE, NULL);

    Data->WakeTime = KeQueryInterruptTime();
    Data->WokenOn = KeGetCurrentProcessorNumber();
    InterlockedExchange(&Data->Woken, 1);

    PsTerminateSystemThread(STATUS_SUCCESS);
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
TestBusyIdealProcessor(
    _In_ ULONG Busy)
{
    static IDLE_DISPATCH Data;
    PKTHREAD Spinner, Sleeper;
    LARGE_INTEGER Delay;
    ULONGLONG SetTime;
    ULONG i;

    RtlZeroMemory(&Data, sizeof(Data));
    Data.Busy = Busy;
    KeInitializeEvent(&Data.Ready, NotificationEvent, FALSE);
    KeInitializeEvent(&Data.Wake, SynchronizationEvent, FALSE);

    /* The sleeper waits, with the busy processor as its ideal one */
    Sleeper = StartThread(SleeperThread, &Data);
    if (!Sleeper)
        return;
    KeWaitForSingleObject(&Data.Ready, Executive, KernelMode, FALSE, NULL);

    /* The busy processor gets a thread it keeps */
    Spinner = StartThread(SpinnerThread, &Data);
    for (i = 0; Spinner && !Data.SpinnerRunning && i < 100; i++)
    {
        Delay.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    ok(Data.SpinnerRunning, "The spinner did not start\n");

    /* Let the other processors go idle, then wake the sleeper */
    Delay.QuadPart = -50 * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    SetTime = KeQueryInterruptTime();
    KeSetEvent(&Data.Wake, IO_NO_INCREMENT, FALSE);

    for (i = 0; !Data.Woken && i < SPIN_MS / 10 + 100; i++)
    {
        Delay.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    InterlockedExchange(&Data.StopSpinner, 1);
    FinishThread(Spinner);
    FinishThread(Sleeper);

    ok(Data.Woken, "The sleeper never ran\n");
    if (Data.Woken)
    {
        ULONG Latency = (ULONG)((Data.WakeTime - SetTime) / 10000);

        ok(Latency < WAKE_LIMIT_MS,
           "Busy processor %lu: the woken thread ran %lu ms late, on processor %lu\n",
           Busy, Latency, Data.WokenOn);
        ok(Data.WokenOn != Busy, "The woken thread ran on the busy processor %lu\n", Busy);
    }
}

START_TEST(KeIdleDispatch)
{
    ULONG Busy;

    /*
     * Processor 0 wakes the sleeper and one other is busy: a third one is
     * needed to be idle. (With two, the sleeper can only queue up on its busy
     * ideal processor until processor 0 waits and takes it from there,
     * which needs an idle processor to look into the others' ready queues.)
     */
    if (KeNumberProcessors < 3)
    {
        skip(FALSE, "Needs three processors\n");
        return;
    }

    /* Wake from processor 0; every other processor in turn is the busy one */
    KeSetSystemAffinityThread(AFFINITY_MASK(0));
    for (Busy = 1; Busy < (ULONG)min(KeNumberProcessors, 4); Busy++)
        TestBusyIdealProcessor(Busy);
    KeRevertToUserAffinityThread();
}
