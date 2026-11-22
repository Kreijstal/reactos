/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite per-processor clock tick test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#define SPIN_MS 500

/*
 * Every processor takes its own clock tick: the thread running there is
 * charged run time and its quantum runs out. A processor whose local timer
 * never fires charges nothing, so a thread spinning on it is never preempted
 * by an equal-priority thread queued behind it.
 */
START_TEST(KeRunTime)
{
    KAFFINITY Active = KeQueryActiveProcessors();
    ULONG Processor;
    PKTHREAD Thread = KeGetCurrentThread();

    for (Processor = 0; Processor < sizeof(KAFFINITY) * 8; Processor++)
    {
        KAFFINITY Mask = (KAFFINITY)1 << Processor;
        ULONG Before, After, UserTime;
        ULONGLONG Start, Ticks = 0;

        if (!(Active & Mask))
            continue;

        KeSetSystemAffinityThread(Mask);
        ok_eq_ulong(KeGetCurrentProcessorNumber(), Processor);

        Before = KeQueryRuntimeThread(Thread, &UserTime);
        Start = KeQueryInterruptTime();
        while (KeQueryInterruptTime() - Start < SPIN_MS * 10000ULL)
        {
            YieldProcessor();
            Ticks++;
        }
        After = KeQueryRuntimeThread(Thread, &UserTime);

        KeRevertToUserAffinityThread();

        /* At least a tenth of the ticks of the spin; a 15.6 ms tick gives 32 */
        ok(After - Before >= SPIN_MS / 16 / 10,
           "Processor %lu: spinning %u ms was charged %lu ticks\n",
           Processor, SPIN_MS, After - Before);
        trace("Processor %lu: %lu ticks in %u ms (%I64u spins)\n",
              Processor, After - Before, SPIN_MS, Ticks);
    }
}
