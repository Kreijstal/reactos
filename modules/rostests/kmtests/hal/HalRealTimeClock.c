/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite HalQueryRealTimeClock test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>
#include <ndk/halfuncs.h>

/* How long to read the clock on each processor, in 100 ns units */
#define READ_TIME (10 * 1000 * 1000)

/*
 * The RTC and its CMOS index/data ports are shared with the clock interrupt
 * (the APIC HAL takes its clock from the RTC and acknowledges it by reading
 * register C). A clock interrupt on another processor that touches the index
 * port between our index write and data read makes HalQueryRealTimeClock
 * return another register's value. Read the clock from every processor while
 * the clock interrupt runs at its fastest rate: the time must never go back
 * and must advance one second at a time. When the race makes the clock
 * interrupt read another register instead of register C, the RTC never
 * interrupts again and the interrupt time stops, so the reads are timed with
 * the performance counter and the interrupt time is checked separately.
 */
START_TEST(HalRealTimeClock)
{
    TIME_FIELDS Fields;
    LARGE_INTEGER Time, Previous;
    LARGE_INTEGER Frequency, Start, Now;
    ULONGLONG InterruptStart;
    LONGLONG Delta;
    ULONG Processor, Processors, Resolution;
    ULONG Reads = 0, Invalid = 0, Backward = 0, Jumps = 0, Ticks = 0;
    ULONG MaxTicks = 0;

    /* Make the clock interrupt fire as often as possible */
    Resolution = ExSetTimerResolution(10000, TRUE);
    trace("Timer resolution %lu\n", Resolution);

    Processors = (ULONG)KeNumberProcessors;
    for (Processor = 0; Processor < Processors; Processor++)
    {
        ULONG ProcessorTicks = 0;

        KeSetSystemAffinityThread(AFFINITY_MASK(Processor));

        ok(HalQueryRealTimeClock(&Fields) == TRUE, "HalQueryRealTimeClock failed\n");
        if (!RtlTimeFieldsToTime(&Fields, &Previous))
        {
            ok(0, "Invalid time %u-%u-%u %u:%u:%u\n", Fields.Year, Fields.Month,
               Fields.Day, Fields.Hour, Fields.Minute, Fields.Second);
            continue;
        }

        InterruptStart = KeQueryInterruptTime();
        Start = KeQueryPerformanceCounter(&Frequency);
        for (;;)
        {
            Now = KeQueryPerformanceCounter(NULL);
            if ((Now.QuadPart - Start.QuadPart) * 10000000 / Frequency.QuadPart >= READ_TIME)
                break;

            HalQueryRealTimeClock(&Fields);
            Reads++;
            if (!RtlTimeFieldsToTime(&Fields, &Time))
            {
                if (Invalid++ < 5)
                    trace("CPU %lu: invalid time %u-%u-%u %u:%u:%u\n", Processor,
                          Fields.Year, Fields.Month, Fields.Day,
                          Fields.Hour, Fields.Minute, Fields.Second);
                continue;
            }

            Delta = Time.QuadPart - Previous.QuadPart;
            if (Delta < 0)
            {
                if (Backward++ < 5)
                    trace("CPU %lu: time went back %I64d s, now %u:%u:%u\n", Processor,
                          -Delta / 10000000, Fields.Hour, Fields.Minute, Fields.Second);
            }
            else if (Delta > 10000000)
            {
                if (Jumps++ < 5)
                    trace("CPU %lu: time jumped %I64d s, now %u:%u:%u\n", Processor,
                          Delta / 10000000, Fields.Hour, Fields.Minute, Fields.Second);
            }
            else if (Delta > 0)
            {
                ProcessorTicks++;
            }
            Previous = Time;
        }
        ok(KeQueryInterruptTime() - InterruptStart >= READ_TIME / 2,
           "CPU %lu: the interrupt time advanced %I64u ms in one second\n", Processor,
           (KeQueryInterruptTime() - InterruptStart) / 10000);
        Ticks += ProcessorTicks;
        MaxTicks = max(MaxTicks, ProcessorTicks);
    }
    KeRevertToUserAffinityThread();
    ExSetTimerResolution(0, FALSE);

    trace("%lu processors, %lu reads, %lu second ticks\n", Processors, Reads, Ticks);
    ok(Reads > Processors, "Only %lu reads\n", Reads);
    ok(Invalid == 0, "%lu reads returned an invalid time\n", Invalid);
    ok(Backward == 0, "Time went back %lu times\n", Backward);
    ok(Jumps == 0, "Time jumped ahead %lu times\n", Jumps);
    /* One second of reading sees at most two second boundaries */
    ok(MaxTicks <= 2, "The seconds changed %lu times in one second\n", MaxTicks);
}
