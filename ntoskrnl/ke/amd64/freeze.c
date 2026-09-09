/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor freeze support for x64
 * COPYRIGHT:   Copyright 2023-2024 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/*

 IpiFrozen state graph (based on Windows behavior):

    +-----------------+     Freeze request      +-----------------+
    | RUNNING         |------------------------>| TARGET_FREEZE   |
    +-----------------+<---------               +-----------------+
            |^                  | Resume                |
     Freeze || Thaw        +-----------+ Thaw request   | Freeze IPI
            v|             | THAW      |<-----------\   v
    +-----------------+    +-----------+        +-----------------+
    | OWNER + ACTIVE  |         ^               | FROZEN          |
    +-----------------+         |               +-----------------+
            ^                   |                       ^
            | Kd proc switch    |                       | Kd proc switch
            v                   |                       v
    +-----------------+         |               +-----------------+
    | OWNER           |---------+               | FROZEN + ACTIVE |
    +-----------------+ Thaw request            +-----------------+

 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* NOT INCLUDES ANYMORE ******************************************************/

PKPRCB KiFreezeOwner;

/*
 * Snapshot of the processors taking part in the current freeze, including the
 * freeze owner itself. It is captured exactly once, in KxFreezeExecution, and
 * is used unchanged by the mark loop, the freeze IPI, the wait loop,
 * KxSwitchKdProcessor and KxThawExecution. Deriving all of them from this one
 * value is what guarantees that the set of PRCBs marked TARGET_FREEZE, the set
 * the freeze IPI is sent to, the set that is waited on and the set that is
 * thawed are the same set.
 *
 * This matters because KeNumberProcessors, KiProcessorBlock[] and
 * KeActiveProcessors do not become valid at the same instant: KiSystemStartup
 * bumps KeNumberProcessors and publishes KiProcessorBlock[Cpu] before the
 * processor runs HalInitializeProcessor, and only sets the KeActiveProcessors
 * bit afterwards. A processor inside that window has a visible PRCB but no
 * initialized local APIC, so it can neither receive nor answer the freeze NMI.
 * Marking it TARGET_FREEZE and then waiting for it (which is what looping over
 * KeNumberProcessors while sending the IPI to KeActiveProcessors used to do)
 * hangs the freeze owner forever, with no bugcheck and no debugger prompt.
 */
KAFFINITY KiFrozenProcessors;

/* FUNCTIONS *****************************************************************/

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* Make sure this is a freeze request */
    if (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        /* Not a freeze request, return FALSE to signal it is unhandled */
        return FALSE;
    }

    /* We are frozen now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

    /* Save the processor state */
    KiSaveProcessorState(TrapFrame, ExceptionFrame);

    /* Wait for the freeze owner to release us */
    while (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_THAW)
    {
        /* Check for Kd processor switch */
        if (CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE)
        {
            KCONTINUE_STATUS ContinueStatus;

            /* Enter the debugger */
            ContinueStatus = KdReportProcessorChange();

            /* Set the state back to frozen */
            CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

            /* If the status is ContinueSuccess, we need to release the freeze owner */
            if (ContinueStatus == ContinueSuccess)
            {
                /* Release the freeze owner */
                KiFreezeOwner->IpiFrozen = IPI_FROZEN_STATE_THAW;
            }
        }

        YieldProcessor();
        KeMemoryBarrier();
    }

    /* Restore the processor state */
    KiRestoreProcessorState(TrapFrame, ExceptionFrame);

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* Return TRUE to signal that we handled the freeze */
    return TRUE;
}

VOID
NTAPI
KxFreezeExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    KAFFINITY TargetProcessors;

    /* Avoid blocking on recursive debug action */
    if (KiFreezeOwner == CurrentPrcb)
    {
        return;
    }

    /* Try to acquire the freeze owner */
    while (InterlockedCompareExchangePointer((volatile PVOID *)&KiFreezeOwner, CurrentPrcb, NULL))
    {
        /* Someone else was faster. We expect an NMI to freeze any time.
           Spin here until the freeze owner is available. */
        while (KiFreezeOwner != NULL)
        {
            YieldProcessor();
            KeMemoryBarrier();
        }
    }

    /* We are the owner now and active */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;

    /* Take a single snapshot of the processors taking part in this freeze.
       Only processors that are already in KeActiveProcessors can answer the
       freeze NMI, so they are the only ones we are allowed to mark and wait
       for. We add ourselves unconditionally, because we might be freezing
       from within our own bring-up, before our own bit went live. Everything
       below is derived from this snapshot and never re-reads the globals, so
       a processor that comes online while we are frozen cannot join the set
       halfway through. */
    KiFrozenProcessors = KeActiveProcessors | CurrentPrcb->SetMember;
    TargetProcessors = KiFrozenProcessors & ~CurrentPrcb->SetMember;

    /* Publish the set before the first target can observe TARGET_FREEZE */
    KeMemoryBarrier();

    /* Loop all processors in the frozen set */
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (TargetProcessors & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];

            /* Only the active processor is allowed to change IpiFrozen */
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_RUNNING);

            /* Request target to freeze */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_TARGET_FREEZE;
        }
    }

    /* Send the freeze IPI to exactly the processors we marked */
    KiIpiSend(TargetProcessors, IPI_FREEZE);

    /* Wait for all targets to be frozen */
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (TargetProcessors & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];

            /* Wait for the target to be frozen */
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }

    /* All targets are frozen, we can continue */
}

VOID
NTAPI
KxThawExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    KAFFINITY TargetProcessors;

    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);

    /* Thaw exactly the set that KxFreezeExecution froze. Re-deriving it from
       KeActiveProcessors here would be wrong: a processor that finished its
       bring-up while we were in the debugger is now active but was never
       frozen, and thawing it would trip the ASSERT below and then hang us
       waiting for a state transition that nobody will ever make. */
    TargetProcessors = KiFrozenProcessors & ~CurrentPrcb->SetMember;

    /* Loop all processors in the frozen set */
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (TargetProcessors & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];

            /* Make sure they are still frozen */
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_FROZEN);

            /* Request target to thaw */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_THAW;
        }
    }

    /* Wait for all targets to be running */
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (TargetProcessors & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];

            /* Wait for the target to be running again */
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* Release the freeze owner */
    InterlockedExchangePointer((volatile PVOID *)&KiFreezeOwner, NULL);
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;

    /* Make sure that the processor index is valid */
    ASSERT(ProcessorIndex < KeNumberProcessors);
    if (ProcessorIndex >= MAXIMUM_PROCESSORS)
    {
        return ContinueProcessorReselected;
    }

    /* We can only hand control to a processor that takes part in this freeze
       (a frozen target, or the freeze owner). A processor that was still
       coming online when the freeze started is neither, is running freely and
       would never hand control back, so refuse the switch and stay here. */
    TargetPrcb = KiProcessorBlock[ProcessorIndex];
    if ((TargetPrcb == NULL) || !(KiFrozenProcessors & TargetPrcb->SetMember))
    {
        return ContinueProcessorReselected;
    }

    /* We are no longer active */
    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);
    CurrentPrcb->IpiFrozen &= ~IPI_FROZEN_FLAG_ACTIVE;

    /* Inform the target processor that it's his turn now */
    TargetPrcb->IpiFrozen |= IPI_FROZEN_FLAG_ACTIVE;

    /* If we are not the freeze owner, we return back to the freeze loop */
    if (KiFreezeOwner != CurrentPrcb)
    {
        return ContinueNextProcessor;
    }

    /* Loop until it's our turn again */
    while (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_OWNER)
    {
        YieldProcessor();
        KeMemoryBarrier();
    }

    /* Check if we have been thawed */
    if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
    {
        /* Another CPU has completed, we can leave the debugger now */
        KdpDprintf("[%u] KxSwitchKdProcessor: ContinueSuccess\n", KeGetCurrentProcessorNumber());
        CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;
        return ContinueSuccess;
    }

    /* We have been reselected, return to Kd to continue in the debugger */
    ASSERT(CurrentPrcb->IpiFrozen == (IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE));

    return ContinueProcessorReselected;
}
