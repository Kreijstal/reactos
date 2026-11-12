/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/tsc.c
 * PURPOSE:         HAL Routines for TSC handling
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include "tsc.h"
#include "apicp.h"
#define NDEBUG
#include <debug.h>

LARGE_INTEGER HalpCpuClockFrequency = {{INITIAL_STALL_COUNT * 1000000}};

/* PRIVATE FUNCTIONS *********************************************************/

/* PIT channel 2, gated through the system control port B (0x61) */
#define SYSTEM_CONTROL_PORT_B   0x61
#define PORTB_TIMER2_GATE       0x01
#define PORTB_SPEAKER_DATA      0x02
#define PORTB_TIMER2_OUTPUT     0x20

/* About 50 ms of PIT counts: long enough for an exact TSC rate */
#define TSC_CALIBRATION_MS      50
#define TSC_CALIBRATION_COUNT   (PIT_FREQUENCY * TSC_CALIBRATION_MS / 1000)
C_ASSERT(TSC_CALIBRATION_COUNT <= 0xFFFF);
#define TSC_CALIBRATION_RUNS    3

/*
 * Measure the TSC rate against PIT channel 2, by polling it with interrupts
 * disabled. Counting the TSC between a few RTC periodic interrupts instead
 * depends on when those interrupts are delivered: in a virtual machine the
 * ticks raised while interrupts were off arrive bunched up (or late), and the
 * performance counter ran at less than half or nearly twice the real rate.
 */
static
ULONG64
HalpMeasureTscWithPit(VOID)
{
    UCHAR PortB;
    ULONG64 Start, End;

    /* Enable the gate of timer 2, with the speaker off */
    PortB = __inbyte(SYSTEM_CONTROL_PORT_B);
    __outbyte(SYSTEM_CONTROL_PORT_B, (PortB & ~PORTB_SPEAKER_DATA) | PORTB_TIMER2_GATE);

    /* Timer 2, low then high byte, mode 0: the output goes high at terminal count */
    __outbyte(TIMER_CONTROL_PORT, 0xB0);
    __outbyte(TIMER_CHANNEL2_DATA_PORT, TSC_CALIBRATION_COUNT & 0xFF);
    Start = __rdtsc();
    __outbyte(TIMER_CHANNEL2_DATA_PORT, TSC_CALIBRATION_COUNT >> 8);

    /* Wait for the count to run out */
    while (!(__inbyte(SYSTEM_CONTROL_PORT_B) & PORTB_TIMER2_OUTPUT))
        NOTHING;
    End = __rdtsc();

    /* Restore port B */
    __outbyte(SYSTEM_CONTROL_PORT_B, PortB);

    return End - Start;
}

VOID
NTAPI
HalpInitializeTsc(VOID)
{
    ULONG_PTR Flags;
    ULONG64 Ticks, MinTicks = ~0ULL;
    ULONG i;

    /* Check if the CPU supports RDTSC */
    if (!(KeGetCurrentPrcb()->FeatureBits & KF_RDTSC))
    {
        KeBugCheck(HAL_INITIALIZATION_FAILED);
    }

    /* Save flags and disable interrupts */
    Flags = __readeflags();
    _disable();

    /* A measurement can only come out too long (the end of the count is
     * noticed late, e.g. when a virtual CPU is held up), so keep the
     * shortest of a few */
    for (i = 0; i < TSC_CALIBRATION_RUNS; i++)
    {
        Ticks = HalpMeasureTscWithPit();
        MinTicks = min(MinTicks, Ticks);
    }

    HalpCpuClockFrequency.QuadPart = MinTicks * PIT_FREQUENCY / TSC_CALIBRATION_COUNT;
    ASSERT(HalpCpuClockFrequency.QuadPart != 0);

    /* Restore flags */
    __writeeflags(Flags);
}

CODE_SEG("INIT")
VOID
NTAPI
HalpCalibrateStallExecution(VOID)
{
    // Timer interrupt is now active

    HalpInitializeTsc();

    KeGetPcr()->StallScaleFactor = (ULONG)(HalpCpuClockFrequency.QuadPart / 1000000);
}

/* PUBLIC FUNCTIONS ***********************************************************/

LARGE_INTEGER
NTAPI
KeQueryPerformanceCounter(
    OUT PLARGE_INTEGER PerformanceFrequency OPTIONAL)
{
    LARGE_INTEGER Result;

    /* Make sure it's calibrated */
    ASSERT(HalpCpuClockFrequency.QuadPart != 0);

    /* Does the caller want the frequency? */
    if (PerformanceFrequency)
    {
        /* Return tsc frequency */
        *PerformanceFrequency = HalpCpuClockFrequency;
    }

    /* Return the current value */
    Result.QuadPart = __rdtsc();
    return Result;
}

VOID
NTAPI
KeStallExecutionProcessor(ULONG MicroSeconds)
{
    ULONG64 StartTime, EndTime;

    /* Get the initial time */
    StartTime = __rdtsc();

    /* Calculate the ending time */
    EndTime = StartTime + KeGetPcr()->StallScaleFactor * MicroSeconds;

    /* Loop until time is elapsed */
    while (__rdtsc() < EndTime);
}

VOID
NTAPI
HalCalibratePerformanceCounter(
    IN volatile PLONG Count,
    IN ULONGLONG NewCount)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
}

