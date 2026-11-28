/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     KeFlushQueuedDpcs keeps the caller's system affinity
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * KeFlushQueuedDpcs visits the processors that have DPCs queued and then
 * gives the thread back its affinity. A caller with a system affinity of its
 * own must keep it: the shutdown thread pins itself to the boot processor
 * before the device power walk, and a KMDF driver that stops a timer with
 * WdfTimerStop(Wait = TRUE) in its D0 exit (the xHCI) flushes the DPCs on
 * that thread. Dropping the pin there sent the HAL reset to an application
 * processor (#315).
 */

#include <kmt_test.h>

typedef KAFFINITY (NTAPI *PFN_SET_SYSTEM_AFFINITY_EX)(KAFFINITY);

static KDEFERRED_ROUTINE DummyDpc;

static
VOID
NTAPI
DummyDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    InterlockedIncrement((volatile LONG *)DeferredContext);
}

/* Sleeps a few times so that a thread without a system affinity gets a
 * chance to be scheduled elsewhere */
static
VOID
LetSchedulerRun(VOID)
{
    LARGE_INTEGER Interval;
    ULONG i;

    Interval.QuadPart = -10 * 1000; /* 1 ms */
    for (i = 0; i < 5; i++)
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static
VOID
TestFlush(
    _In_ BOOLEAN QueueDpcs,
    _In_opt_ PFN_SET_SYSTEM_AFFINITY_EX pSetSystemAffinityEx)
{
    KDPC Dpcs[MAXIMUM_PROCESSORS];
    volatile LONG Ran = 0;
    LONG Queued = 0;
    CCHAR Processor;
    KIRQL OldIrql;
    KAFFINITY Previous;

    KeSetSystemAffinityThread(1);
    ok_eq_ulong(KeGetCurrentProcessorNumber(), 0UL);

    if (QueueDpcs)
    {
        /* Queue one on every other processor, so that the flush goes there */
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        for (Processor = 1; Processor < KeNumberProcessors && Processor < MAXIMUM_PROCESSORS; Processor++)
        {
            KeInitializeDpc(&Dpcs[(ULONG)Processor], DummyDpc, (PVOID)&Ran);
            KeSetImportanceDpc(&Dpcs[(ULONG)Processor], LowImportance);
            KeSetTargetProcessorDpc(&Dpcs[(ULONG)Processor], Processor);
            if (KeInsertQueueDpc(&Dpcs[(ULONG)Processor], NULL, NULL))
                Queued++;
        }
        KeLowerIrql(OldIrql);
    }

    KeFlushQueuedDpcs();

    /* Every DPC queued before the flush has run */
    ok_eq_long(Ran, Queued);

    /* The system affinity must still be the caller's */
    LetSchedulerRun();
    ok(KeGetCurrentProcessorNumber() == 0,
       "Flush (DPCs queued: %ld): thread runs on processor %lu, expected 0\n",
       Queued, KeGetCurrentProcessorNumber());

    if (pSetSystemAffinityEx)
    {
        /* Returns the system affinity in force before the call, 0 if none */
        Previous = pSetSystemAffinityEx(1);
        ok(Previous == 1,
           "Flush (DPCs queued: %ld): system affinity 0x%Ix, expected 0x1\n",
           Queued, Previous);
    }

    KeRevertToUserAffinityThread();
}

START_TEST(KeFlushDpcsAffinity)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"KeSetSystemAffinityThreadEx");
    PFN_SET_SYSTEM_AFFINITY_EX pSetSystemAffinityEx;

    pSetSystemAffinityEx = (PFN_SET_SYSTEM_AFFINITY_EX)MmGetSystemRoutineAddress(&Name);
    if (!pSetSystemAffinityEx)
        trace("KeSetSystemAffinityThreadEx not exported, checking the processor only\n");

    if (KeNumberProcessors < 2)
        trace("One processor: only the system affinity itself can be checked\n");

    TestFlush(FALSE, pSetSystemAffinityEx);
    TestFlush(TRUE, pSetSystemAffinityEx);
}
