/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The register-program interpreter
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 *
 * The whole reason this is a batch interpreter and not a read/write ioctl pair:
 * a PHY initvals table is thousands of writes, and the reset paths around it
 * poll on microsecond deadlines.  One syscall per register would be slow; one
 * network round trip per register -- which is what this ends up being, driven
 * from a laptop over luagent -- would make the timing meaningless.  So the
 * program crosses the boundary once and the timing-sensitive part never leaves
 * the kernel.
 *
 * Nothing here prints in the loop.  A DPRINT costs milliseconds on a serial
 * port, which is enough on its own to change whether a poll succeeds.
 */

#include "rospoke_int.h"

#ifndef ROSPOKE_HOST_HARNESS
#define NDEBUG
#include <debug.h>
#endif

typedef struct _ROSPOKE_CLOCK
{
    LARGE_INTEGER Start;
    LONGLONG Frequency;
} ROSPOKE_CLOCK;

static
VOID
RospokeClockStart(
    _Out_ ROSPOKE_CLOCK *Clock)
{
    LARGE_INTEGER Frequency;

    Clock->Start = KeQueryPerformanceCounter(&Frequency);
    Clock->Frequency = Frequency.QuadPart ? Frequency.QuadPart : 1;
}

static
ULONGLONG
RospokeElapsedUs(
    _In_ const ROSPOKE_CLOCK *Clock)
{
    LARGE_INTEGER Now = KeQueryPerformanceCounter(NULL);
    LONGLONG Delta = Now.QuadPart - Clock->Start.QuadPart;

    if (Delta <= 0)
        return 0;

    return (ULONGLONG)((Delta * 1000000LL) / Clock->Frequency);
}

VOID
RospokeStallUs(
    _In_ ULONG Micros)
{
    /* KeStallExecutionProcessor is not meant to be handed large values; chunk
     * it so a long delay does not depend on the HAL's tolerance. */
    while (Micros > 1000)
    {
        KeStallExecutionProcessor(1000);
        Micros -= 1000;
    }
    if (Micros != 0)
        KeStallExecutionProcessor(Micros);
}

static
VOID
RospokeDelayUs(
    _In_ ULONG Micros)
{
    LARGE_INTEGER Interval;

    if (Micros == 0)
        return;

    /*
     * Below the threshold, busy-wait.  KeDelayExecutionThread rounds up to a
     * clock tick, so asking it for 2 ms yields ~15 ms -- which silently turns a
     * faithful replay of a hardware bring-up sequence into a different one.
     */
    if (Micros <= ROSPOKE_STALL_LIMIT_US)
    {
        RospokeStallUs(Micros);
        return;
    }

    Interval.QuadPart = -((LONGLONG)Micros * 10);
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static
ULONG
RospokeAccessCheck(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ ULONG Target,
    _In_ ULONG Offset,
    _In_ ULONG Width,
    _Out_ PROSPOKE_SLOT *SlotOut)
{
    PROSPOKE_SLOT Slot;

    *SlotOut = NULL;

    if (Target == ROSPOKE_TARGET_CFG)
    {
        if (!Ext->DeviceOpen)
            return ROSPOKE_EXEC_NO_DEVICE;
        if ((Offset % Width) != 0)
            return ROSPOKE_EXEC_UNALIGNED;
        if (Offset >= 0x100u || Width > 0x100u - Offset)
            return ROSPOKE_EXEC_OUT_OF_BOUNDS;
        return ROSPOKE_EXEC_OK;
    }

    Slot = RospokeResolveTarget(Ext, Target);
    if (Slot == NULL)
        return ROSPOKE_EXEC_BAD_TARGET;
    if ((Offset % Width) != 0)
        return ROSPOKE_EXEC_UNALIGNED;
    if (Offset > Slot->Length || Width > Slot->Length - Offset)
        return ROSPOKE_EXEC_OUT_OF_BOUNDS;

    *SlotOut = Slot;
    return ROSPOKE_EXEC_OK;
}

static
ULONG
RospokeLoad(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_opt_ PROSPOKE_SLOT Slot,
    _In_ ULONG Offset,
    _In_ ULONG Width,
    _Out_ PULONG Value)
{
    PUCHAR Base;

    if (Slot == NULL)
    {
        ULONG Raw = 0;

        if (!RospokeCfgRead(Ext, Offset, &Raw, Width))
            return ROSPOKE_EXEC_CFG_FAILED;
        *Value = Raw;
        return ROSPOKE_EXEC_OK;
    }

    Base = (PUCHAR)Slot->VirtualAddress + Offset;

    if (Slot->Kind == ROSPOKE_SLOT_DMA)
    {
        switch (Width)
        {
            case 4: *Value = *(volatile ULONG *)Base; break;
            case 2: *Value = *(volatile USHORT *)Base; break;
            default: *Value = *(volatile UCHAR *)Base; break;
        }
        return ROSPOKE_EXEC_OK;
    }

    switch (Width)
    {
        case 4: *Value = READ_REGISTER_ULONG((PULONG)Base); break;
        case 2: *Value = READ_REGISTER_USHORT((PUSHORT)Base); break;
        default: *Value = READ_REGISTER_UCHAR(Base); break;
    }
    return ROSPOKE_EXEC_OK;
}

static
ULONG
RospokeStore(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_opt_ PROSPOKE_SLOT Slot,
    _In_ ULONG Offset,
    _In_ ULONG Width,
    _In_ ULONG Value)
{
    PUCHAR Base;

    if (Slot == NULL)
    {
        if (!RospokeCfgWrite(Ext, Offset, &Value, Width))
            return ROSPOKE_EXEC_CFG_FAILED;
        return ROSPOKE_EXEC_OK;
    }

    Base = (PUCHAR)Slot->VirtualAddress + Offset;

    if (Slot->Kind == ROSPOKE_SLOT_DMA)
    {
        switch (Width)
        {
            case 4: *(volatile ULONG *)Base = Value; break;
            case 2: *(volatile USHORT *)Base = (USHORT)Value; break;
            default: *(volatile UCHAR *)Base = (UCHAR)Value; break;
        }
        return ROSPOKE_EXEC_OK;
    }

    switch (Width)
    {
        case 4: WRITE_REGISTER_ULONG((PULONG)Base, Value); break;
        case 2: WRITE_REGISTER_USHORT((PUSHORT)Base, (USHORT)Value); break;
        default: WRITE_REGISTER_UCHAR(Base, (UCHAR)Value); break;
    }
    return ROSPOKE_EXEC_OK;
}

static
ULONG
RospokePoll(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_opt_ PROSPOKE_SLOT Slot,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Wanted,
    _In_ ULONG TimeoutUs,
    _Out_ PULONG Value)
{
    ROSPOKE_CLOCK Clock;
    ULONG Status;

    RospokeClockStart(&Clock);

    for (;;)
    {
        ULONGLONG Elapsed;

        Status = RospokeLoad(Ext, Slot, Offset, 4, Value);
        if (Status != ROSPOKE_EXEC_OK)
            return Status;

        if ((*Value & Mask) == Wanted)
            return ROSPOKE_EXEC_OK;

        Elapsed = RospokeElapsedUs(&Clock);
        if (Elapsed >= TimeoutUs)
            return ROSPOKE_EXEC_POLL_TIMEOUT;

        /* Tight for the first stretch so short hardware deadlines are honoured,
         * then back off to the scheduler so a long poll does not hold a CPU. */
        if (Elapsed < ROSPOKE_STALL_LIMIT_US)
            KeStallExecutionProcessor(1);
        else
            RospokeDelayUs(1000);
    }
}

static
ULONG
RospokeOpWidth(
    _In_ ULONG Opcode)
{
    switch (Opcode)
    {
        case ROSPOKE_OP_WRITE8:
        case ROSPOKE_OP_READ8:
            return 1;
        case ROSPOKE_OP_WRITE16:
        case ROSPOKE_OP_READ16:
            return 2;
        default:
            return 4;
    }
}

VOID
RospokeExec(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_reads_(OpCount) const ROSPOKE_OP *Ops,
    _In_ ULONG OpCount,
    _Out_writes_to_(ResultCapacity, *ResultCount) PULONG Results,
    _In_ ULONG ResultCapacity,
    _Out_ PULONG ResultCount,
    _Out_ PROSPOKE_EXEC_OUT Out)
{
    ROSPOKE_CLOCK Clock;
    ULONG Index, Produced = 0;
    ULONG Status = ROSPOKE_EXEC_OK;

    RospokeClockStart(&Clock);

    for (Index = 0; Index < OpCount; Index++)
    {
        const ROSPOKE_OP *Op = &Ops[Index];
        PROSPOKE_SLOT Slot = NULL;
        ULONG Width, Value = 0;
        BOOLEAN HasResult = FALSE;

        if (Op->Opcode == ROSPOKE_OP_END)
            break;

        if (Op->Opcode >= ROSPOKE_OP_MAX)
        {
            Status = ROSPOKE_EXEC_BAD_OPCODE;
            break;
        }

        /* Bound the whole batch, not just the individual waits: a script that
         * loops a thousand short polls can still wedge the machine. */
        if (RospokeElapsedUs(&Clock) > ROSPOKE_BUDGET_US)
        {
            Status = ROSPOKE_EXEC_BUDGET;
            break;
        }

        switch (Op->Opcode)
        {
            case ROSPOKE_OP_DELAY_US:
                RospokeDelayUs(min(Op->Arg0, ROSPOKE_MAX_DELAY_US));
                break;

            case ROSPOKE_OP_MARK:
                Value = Op->Arg0;
                HasResult = TRUE;
                break;

            case ROSPOKE_OP_WRITE32:
            case ROSPOKE_OP_WRITE16:
            case ROSPOKE_OP_WRITE8:
                Width = RospokeOpWidth(Op->Opcode);
                Status = RospokeAccessCheck(Ext, Op->Target, Op->Offset, Width, &Slot);
                if (Status != ROSPOKE_EXEC_OK)
                    break;
                Status = RospokeStore(Ext, Slot, Op->Offset, Width, Op->Arg0);
                break;

            case ROSPOKE_OP_READ32:
            case ROSPOKE_OP_READ16:
            case ROSPOKE_OP_READ8:
                Width = RospokeOpWidth(Op->Opcode);
                Status = RospokeAccessCheck(Ext, Op->Target, Op->Offset, Width, &Slot);
                if (Status != ROSPOKE_EXEC_OK)
                    break;
                Status = RospokeLoad(Ext, Slot, Op->Offset, Width, &Value);
                HasResult = TRUE;
                break;

            case ROSPOKE_OP_RMW32:
                Status = RospokeAccessCheck(Ext, Op->Target, Op->Offset, 4, &Slot);
                if (Status != ROSPOKE_EXEC_OK)
                    break;
                Status = RospokeLoad(Ext, Slot, Op->Offset, 4, &Value);
                if (Status != ROSPOKE_EXEC_OK)
                    break;
                HasResult = TRUE;
                Status = RospokeStore(Ext, Slot, Op->Offset, 4,
                                      (Value & Op->Arg0) | Op->Arg1);
                break;

            case ROSPOKE_OP_POLL32:
                Status = RospokeAccessCheck(Ext, Op->Target, Op->Offset, 4, &Slot);
                if (Status != ROSPOKE_EXEC_OK)
                    break;
                Status = RospokePoll(Ext, Slot, Op->Offset, Op->Arg0, Op->Arg1,
                                     Op->Arg2, &Value);
                /* Report the value even on timeout: what the register actually
                 * held is the entire point of the failure. */
                HasResult = TRUE;
                break;

            default:
                Status = ROSPOKE_EXEC_BAD_OPCODE;
                break;
        }

        if (HasResult)
        {
            if (Produced >= ResultCapacity)
            {
                Status = ROSPOKE_EXEC_RESULT_OVERFLOW;
                break;
            }
            Results[Produced++] = Value;
        }

        if (Status != ROSPOKE_EXEC_OK)
            break;
    }

    Out->ExecStatus = Status;
    Out->OpsExecuted = Index;
    Out->ResultCount = Produced;
    Out->ElapsedUs = (ULONG)min(RospokeElapsedUs(&Clock), 0xFFFFFFFFull);
    *ResultCount = Produced;

    if (Status != ROSPOKE_EXEC_OK)
    {
        DPRINT1("rospoke: batch failed at op %lu (opcode %lu target %lx offset %lx): status %lu\n",
                Index,
                (Index < OpCount) ? Ops[Index].Opcode : 0,
                (Index < OpCount) ? Ops[Index].Target : 0,
                (Index < OpCount) ? Ops[Index].Offset : 0,
                Status);
    }
}
