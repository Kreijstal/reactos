/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Interrupt object routines keep the caller's system affinity
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * KeConnectInterrupt and KeDisconnectInterrupt run on the interrupt's
 * processor for a moment. A caller that has a system affinity of its own must
 * get it back afterwards, not the thread's user affinity: the shutdown thread
 * pins itself to the boot processor, and a driver that disconnected its
 * interrupt during the shutdown device walk used to send the thread, and the
 * HAL reset after it, to whichever processor it landed on (#315).
 *
 * The interrupt objects here are never connected, so no vector is touched;
 * disconnecting one still switches to its processor and back.
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

typedef KAFFINITY (NTAPI *PFN_SET_SYSTEM_AFFINITY_EX)(KAFFINITY);

static KSERVICE_ROUTINE DummyIsr;

static
BOOLEAN
NTAPI
DummyIsr(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(ServiceContext);
    return FALSE;
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
TestDisconnectOn(
    _In_ CCHAR Processor,
    _In_opt_ PFN_SET_SYSTEM_AFFINITY_EX pSetSystemAffinityEx)
{
    KINTERRUPT Interrupt;
    KAFFINITY Previous;

    KeSetSystemAffinityThread(1);
    ok_eq_ulong(KeGetCurrentProcessorNumber(), 0UL);

    RtlZeroMemory(&Interrupt, sizeof(Interrupt));
    KeInitializeInterrupt(&Interrupt,
                          DummyIsr,
                          NULL,
                          NULL,
                          0x80,
                          (KIRQL)((0x80) >> 4),
                          (KIRQL)((0x80) >> 4),
                          LevelSensitive,
                          FALSE,
                          Processor,
                          FALSE);
    ok_eq_bool(Interrupt.Connected, FALSE);

    (VOID)KeDisconnectInterrupt(&Interrupt);
    ok_eq_bool(Interrupt.Connected, FALSE);

    /* The system affinity must still be the caller's */
    LetSchedulerRun();
    ok(KeGetCurrentProcessorNumber() == 0,
       "Disconnect on processor %d: thread runs on processor %lu, expected 0\n",
       Processor, KeGetCurrentProcessorNumber());

    if (pSetSystemAffinityEx)
    {
        /* Returns the system affinity in force before the call, 0 if none */
        Previous = pSetSystemAffinityEx(1);
        ok(Previous == 1,
           "Disconnect on processor %d: system affinity 0x%Ix, expected 0x1\n",
           Processor, Previous);
    }

    KeRevertToUserAffinityThread();
}

START_TEST(KeInterruptAffinity)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"KeSetSystemAffinityThreadEx");
    PFN_SET_SYSTEM_AFFINITY_EX pSetSystemAffinityEx;
    CCHAR Processor;

    pSetSystemAffinityEx = (PFN_SET_SYSTEM_AFFINITY_EX)MmGetSystemRoutineAddress(&Name);
    if (!pSetSystemAffinityEx)
        trace("KeSetSystemAffinityThreadEx not exported, checking the processor only\n");

    if (KeNumberProcessors < 2)
        trace("One processor: only the system affinity itself can be checked\n");

    for (Processor = 0; Processor < KeNumberProcessors; Processor++)
        TestDisconnectOn(Processor, pSetSystemAffinityEx);
}
