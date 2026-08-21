/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor idle state (C-state) selection, entry and accounting
 * COPYRIGHT:   Copyright 2026 The ReactOS Project
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/*
 * The kernel owns the POLICY -- which idle state to enter next, and the
 * residency accounting that decides it.  The processor driver owns the
 * MECHANISM -- how to enter a state on this particular platform -- and
 * registers it through NtPowerInformation(ProcessorStateHandler2).
 *
 * That split is NT's, and it is the right one: the entry sequence for a C-state
 * is firmware and chipset specific (a HLT, a read from the ACPI P_LVLx I/O
 * register, or an MWAIT carrying a vendor hint), while the question of when
 * entering it actually pays off is not.
 *
 * Handlers are invoked as Handler(Index, &IdleTimes), where Index is the
 * zero-based index of the state being entered.  PROCESSOR_IDLE_HANDLER_INFO
 * carries no context field of its own, so the index IS the context: the driver
 * keeps its per-state entry data (port addresses and so on) in a table indexed
 * the same way.
 *
 * A handler is entered with interrupts DISABLED and must return with them
 * ENABLED, exactly like HalProcessorIdle.  KiIdleLoop disables interrupts
 * before it tests the scheduler state so that a wakeup raised between the test
 * and the halt cannot be lost; it is the handler that closes that window, with
 * the atomic STI;HLT pair for a halt, or by doing the I/O read first and
 * enabling interrupts only after the processor wakes for an ACPI state.
 */

/* Consecutive idles longer than the next state's break-even before promoting */
#define POP_PROMOTION_SAMPLES 4

typedef struct _POP_IDLE_HANDLER
{
    PPROCESSOR_IDLE_HANDLER Handler;
    /* Worst case time to resume execution from this state, in microseconds */
    ULONG HardwareLatency;
    /*
     * Residency below which entering this state was a loss, in performance
     * counter ticks.  Precomputed at registration: deciding this in the idle
     * path itself would mean a 64-bit divide per idle, on the one path in the
     * kernel where the work must disappear into the noise.
     */
    ULONGLONG BreakEvenTicks;
} POP_IDLE_HANDLER, *PPOP_IDLE_HANDLER;

typedef struct _POP_IDLE_TABLE
{
    ULONG Count;
    POP_IDLE_HANDLER Handler[MAX_IDLE_HANDLERS];
} POP_IDLE_TABLE, *PPOP_IDLE_TABLE;

/*
 * Per-processor policy state.  PROCESSOR_POWER_STATE::IdleState is the field NT
 * reserves for exactly this, so it is what we hang it off; the residency
 * counters live in the PROCESSOR_POWER_STATE fields NT already defines for them
 * (TotalIdleStateTime, TotalIdleTransitions, PromotionCount, DemotionCount).
 */
typedef struct _POP_IDLE_STATE
{
    ULONG CurrentState;
    ULONG PromotionSamples;
} POP_IDLE_STATE, *PPOP_IDLE_STATE;

/* One machine, one table: every package we support is homogeneous */
static POP_IDLE_TABLE PopIdleTable;
static BOOLEAN PopIdleHandlersRegistered = FALSE;

/* PRIVATE FUNCTIONS *********************************************************/

/*
 * The idle function installed once a driver has registered real idle states.
 * Runs at APC_LEVEL or DISPATCH_LEVEL on the idle thread with interrupts off.
 */
VOID
FASTCALL
PopProcessorIdle(
    _Inout_ PPROCESSOR_POWER_STATE PState)
{
    PPOP_IDLE_TABLE Table = (PPOP_IDLE_TABLE)PState->IdleHandlers;
    PPOP_IDLE_STATE State = (PPOP_IDLE_STATE)PState->IdleState;
    PROCESSOR_IDLE_TIMES IdleTimes;
    ULONGLONG Residency;
    NTSTATUS Status;
    ULONG Index;

    /*
     * Registration publishes the table before the function pointer, so seeing
     * this routine at all means the table is there.  Check anyway: this runs on
     * every processor with interrupts disabled, and a wrong pointer here would
     * be a hang with no way to report it.
     */
    if (Table == NULL || State == NULL || Table->Count == 0)
    {
        HalProcessorIdle();
        return;
    }

    Index = State->CurrentState;
    if (Index >= Table->Count)
        Index = 0;

    /*
     * The handler measures its own residency: only it knows which clock stayed
     * running across the state it entered.
     */
    IdleTimes.StartTime = 0;
    IdleTimes.EndTime = 0;
    Status = Table->Handler[Index].Handler(Index, &IdleTimes);

    if (!NT_SUCCESS(Status))
    {
        /*
         * The platform refused the transition.  Drop to the shallowest state,
         * which is a plain halt and cannot fail, rather than retrying a state
         * the firmware has just told us it will not enter.
         */
        PState->ErrorCount++;
        State->CurrentState = 0;
        State->PromotionSamples = 0;
        return;
    }

    /*
     * A handler that could not time itself, or a clock that went backwards over
     * the transition, leaves the policy no evidence to act on.  Accounting for
     * a bogus residency would corrupt the promotion decision for every idle
     * that follows, so account for the transition and leave the state alone.
     */
    PState->TotalIdleTransitions[Index]++;
    if (IdleTimes.EndTime <= IdleTimes.StartTime)
        return;

    Residency = IdleTimes.EndTime - IdleTimes.StartTime;
    PState->TotalIdleStateTime[Index] += Residency;

    if (Index > 0 && Residency < Table->Handler[Index].BreakEvenTicks)
    {
        /*
         * We slept for less than it cost to get in and out of this state, so
         * the deeper state was the wrong call.  Demote immediately -- one bad
         * sample is enough, because the cost is paid on every idle until the
         * workload changes back.
         */
        State->CurrentState = Index - 1;
        State->PromotionSamples = 0;
        PState->DemotionCount++;
        return;
    }

    if (Index + 1 < Table->Count &&
        Residency >= Table->Handler[Index + 1].BreakEvenTicks)
    {
        /*
         * This idle would have paid for the next state down.  Require a run of
         * them before promoting: demotion is cheap to undo, but promoting on a
         * single long idle makes the policy oscillate against bursty work.
         */
        State->PromotionSamples++;
        if (State->PromotionSamples >= POP_PROMOTION_SAMPLES)
        {
            State->CurrentState = Index + 1;
            State->PromotionSamples = 0;
            PState->PromotionCount++;
        }
        return;
    }

    State->PromotionSamples = 0;
}

/*
 * Installs a driver-supplied idle handler table on every processor.
 *
 * Called with the handler array already validated and copied into kernel space
 * by the caller.
 */
NTSTATUS
NTAPI
PopRegisterProcessorIdleHandlers(
    _In_reads_(Count) PPROCESSOR_IDLE_HANDLER_INFO Handlers,
    _In_ ULONG Count)
{
    LARGE_INTEGER Frequency;
    ULONGLONG TicksPerMicrosecond;
    PPOP_IDLE_STATE State;
    PKPRCB Prcb;
    ULONG i;

    PAGED_CODE();

    if (Count == 0 || Count > MAX_IDLE_HANDLERS)
        return STATUS_INVALID_PARAMETER;

    /*
     * Registering twice would have to swap the table out from under processors
     * that are executing out of it right now.  Nothing needs that, so refuse it
     * instead of building the synchronisation to make it safe.
     */
    if (PopIdleHandlersRegistered)
        return STATUS_DEVICE_ALREADY_ATTACHED;

    for (i = 0; i < Count; i++)
    {
        if (Handlers[i].Handler == NULL)
            return STATUS_INVALID_PARAMETER;
    }

    KeQueryPerformanceCounter(&Frequency);
    if (Frequency.QuadPart <= 0)
        return STATUS_DEVICE_NOT_READY;

    TicksPerMicrosecond = (ULONGLONG)Frequency.QuadPart / 1000000ULL;
    if (TicksPerMicrosecond == 0)
        TicksPerMicrosecond = 1;

    RtlZeroMemory(&PopIdleTable, sizeof(PopIdleTable));
    for (i = 0; i < Count; i++)
    {
        PopIdleTable.Handler[i].Handler = Handlers[i].Handler;
        PopIdleTable.Handler[i].HardwareLatency = Handlers[i].HardwareLatency;

        /*
         * Break even at twice the exit latency: the transition costs roughly
         * that much in lost execution either side of the sleep, so a residency
         * below it means the state lost us time as well as saving no power.
         * State 0 is a plain halt with no meaningful entry cost, and demotion
         * out of it is impossible anyway, so its threshold is never consulted.
         */
        PopIdleTable.Handler[i].BreakEvenTicks =
            2ULL * (ULONGLONG)Handlers[i].HardwareLatency * TicksPerMicrosecond;
    }
    PopIdleTable.Count = Count;

    /*
     * Commit before publishing.  From the first processor switched over, the
     * table is live and can never be rewritten, so the "already registered"
     * flag has to be set here rather than after the loop: a partial failure
     * below still leaves processors running out of this table, and a later
     * registration that found the flag clear would edit it underneath them.
     */
    PopIdleHandlersRegistered = TRUE;

    for (i = 0; i < (ULONG)KeNumberProcessors; i++)
    {
        State = ExAllocatePoolZero(NonPagedPool, sizeof(POP_IDLE_STATE), 'IdoP');
        if (State == NULL)
        {
            /*
             * Processors already switched over keep running the new idle
             * function against a table that stays valid for the life of the
             * system, so there is nothing to unwind: the remaining processors
             * simply stay on PopIdle0.
             */
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Prcb = KiProcessorBlock[i];

        /*
         * Publish the payload before the pointer that makes it reachable: the
         * target processor may be inside its idle loop right now and will pick
         * up IdleFunction on its very next iteration.
         */
        Prcb->PowerState.IdleHandlers = &PopIdleTable;
        Prcb->PowerState.IdleHandlersCount = Count;
        Prcb->PowerState.IdleState = State;
        KeMemoryBarrier();
        Prcb->PowerState.IdleFunction = PopProcessorIdle;
    }

    DPRINT1("Po: %lu processor idle state(s) registered, exit latency:", Count);
    for (i = 0; i < Count; i++)
        DPRINT1(" C%lu=%luus", i + 1, PopIdleTable.Handler[i].HardwareLatency);
    DPRINT1("\n");

    return STATUS_SUCCESS;
}

/*
 * Reports the deepest state available and the one the given processor is
 * currently settling on, for NtPowerInformation(ProcessorInformation).
 */
VOID
NTAPI
PopQueryProcessorIdleState(
    _In_ PKPRCB Prcb,
    _Out_ PULONG MaxIdleState,
    _Out_ PULONG CurrentIdleState)
{
    PPOP_IDLE_STATE State = (PPOP_IDLE_STATE)Prcb->PowerState.IdleState;
    ULONG Count = Prcb->PowerState.IdleHandlersCount;

    if (Count == 0 || State == NULL)
    {
        /* Only the unconditional halt is available */
        *MaxIdleState = 0;
        *CurrentIdleState = 0;
        return;
    }

    *MaxIdleState = Count - 1;
    *CurrentIdleState = State->CurrentState;
}

/* EOF */
