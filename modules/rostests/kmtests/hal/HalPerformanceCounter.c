/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite KeQueryPerformanceCounter test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#define ROUNDS 200

/*
 * The performance counter is one clock for the whole system: a time taken on
 * one processor and compared with one taken on another (an ICMP echo sent on
 * one processor and answered on another, say) must neither go back nor jump
 * ahead.
 */
START_TEST(HalPerformanceCounter)
{
    LARGE_INTEGER Frequency, Counter, Previous;
    LONGLONG Delta, MaxBackward = 0, MaxForward = 0;
    ULONG Round, Processor, Processors;
    ULONG Backward = 0, Jumps = 0;

    Counter = KeQueryPerformanceCounter(&Frequency);
    ok(Frequency.QuadPart > 0, "Frequency = %I64d\n", Frequency.QuadPart);
    if (skip(Frequency.QuadPart > 0, "No performance counter frequency\n"))
        return;

    Previous = KeQueryPerformanceCounter(NULL);
    ok(Previous.QuadPart >= Counter.QuadPart, "Counter went back: %I64d, then %I64d\n",
       Counter.QuadPart, Previous.QuadPart);

    Processors = (ULONG)KeNumberProcessors;
    trace("%lu processors, frequency %I64d\n", Processors, Frequency.QuadPart);

    for (Round = 0; Round < ROUNDS; Round++)
    {
        for (Processor = 0; Processor < Processors; Processor++)
        {
            KeSetSystemAffinityThread(AFFINITY_MASK(Processor));
            ok_eq_ulong(KeGetCurrentProcessorNumber(), Processor);
            Counter = KeQueryPerformanceCounter(NULL);

            Delta = Counter.QuadPart - Previous.QuadPart;
            if (Delta < 0)
            {
                Backward++;
                MaxBackward = max(MaxBackward, -Delta);
            }
            /* Switching processors takes far less than a second */
            else if (Delta > Frequency.QuadPart)
            {
                Jumps++;
                MaxForward = max(MaxForward, Delta);
            }
            Previous = Counter;
        }
    }
    KeRevertToUserAffinityThread();

    ok(Backward == 0, "Counter went back %lu times when changing processors, by up to %I64d ms\n",
       Backward, MaxBackward * 1000 / Frequency.QuadPart);
    ok(Jumps == 0, "Counter jumped ahead %lu times when changing processors, by up to %I64d ms\n",
       Jumps, MaxForward * 1000 / Frequency.QuadPart);

    /*
     * The counter and its frequency must measure time at the right rate:
     * one second of interrupt time is one second of performance counter
     * time. The tolerance is wide because in a virtual machine the interrupt
     * time lags a few percent behind; a badly calibrated frequency is off by
     * far more (0.4x and 1.3x-1.8x were seen in QEMU).
     */
    {
        LARGE_INTEGER Start, End, Delay;
        ULONGLONG InterruptStart, InterruptEnd;
        LONGLONG CounterUs, InterruptUs;

        Delay.QuadPart = -20000000LL; /* 2 s */
        InterruptStart = KeQueryInterruptTime();
        Start = KeQueryPerformanceCounter(NULL);
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        End = KeQueryPerformanceCounter(NULL);
        InterruptEnd = KeQueryInterruptTime();

        CounterUs = (End.QuadPart - Start.QuadPart) * 1000000 / Frequency.QuadPart;
        InterruptUs = (LONGLONG)(InterruptEnd - InterruptStart) / 10;
        trace("2 s delay: performance counter %I64d us, interrupt time %I64d us\n",
              CounterUs, InterruptUs);
        ok(CounterUs * 100 >= InterruptUs * 85 && CounterUs * 100 <= InterruptUs * 115,
           "Performance counter measured %I64d us while interrupt time measured %I64d us\n",
           CounterUs, InterruptUs);
    }
}
