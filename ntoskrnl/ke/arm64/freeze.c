/*
 * PROJECT:         ReactOS Kernel (ARM64)
 * PURPOSE:         Kernel-debugger freeze helpers
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

extern BOOLEAN KiHalInitialized;

#if DBG
VOID
KiPmrTraceLog(
    _In_ PVOID Inner,
    _In_ KIRQL OldIrql,
    _In_ KIRQL NewIrql,
    _In_ UCHAR Path);
#endif

KIRQL
NTAPI
KxFreezeExecutionRaiseIrql(
    VOID)
{
    KIRQL OldIrql;

    /*
     * Bypass the generic IRQL path while entering KDBG freeze on ARM64.
     * The debugger only needs a stable HIGH_LEVEL mask across DAIF and PMR.
     */
    OldIrql = KeGetCurrentIrql();
    __asm__ __volatile__("msr daifset, #0xf" ::: "memory");
    __asm__ __volatile__("dsb sy" ::: "memory");
    if (KiHalInitialized)
    {
        HalSetGicPriorityMask(HIGH_LEVEL);
#if DBG
        KiPmrTraceLog(_ReturnAddress(), OldIrql, HIGH_LEVEL, 6);
#endif
    }
    KiSetCurrentIrql(HIGH_LEVEL);
    __asm__ __volatile__("isb" ::: "memory");

    return OldIrql;
}

VOID
NTAPI
KxFreezeExecutionLowerIrql(
    _In_ KIRQL OldIrql)
{
    /*
     * Restore the saved logical IRQL and matching GIC PMR.
     * KeRestoreInterrupts() handles the final DAIF state on the shared path.
     */
    KiSetCurrentIrql(OldIrql);
    if (KiHalInitialized)
    {
        HalSetGicPriorityMask(OldIrql);
#if DBG
        KiPmrTraceLog(_ReturnAddress(), HIGH_LEVEL, OldIrql, 7);
#endif
    }
    __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
}

#ifdef CONFIG_SMP

static PKPRCB KiFreezeOwner;

/*
 * Snapshot of the processors taking part in the current freeze, including the
 * freeze owner itself. Captured once in KxFreezeExecution and used unchanged
 * by the mark loop, the IPI, the wait loop, KxSwitchKdProcessor and the thaw,
 * so that all of those operate on provably the same set.
 *
 * KiSystemStartup (ke/arm64/kiinit.c) sets the KeActiveProcessors bit and
 * bumps KeNumberProcessors as two separate stores, and it publishes
 * KiProcessorBlock[N] earlier still (KiInitializePcr). Mixing a
 * "i < KeNumberProcessors" loop with a "KeActiveProcessors" IPI mask therefore
 * lets the marked set, the IPI'd set and the waited-on set disagree while a
 * processor is coming online, which hangs the freeze owner forever with no
 * bugcheck. Deriving everything from one snapshot removes that entirely.
 */
static KAFFINITY KiFrozenProcessors;

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB CurrentPrcb;

    CurrentPrcb = KeGetCurrentPrcb();
    if ((CurrentPrcb == NULL) ||
        (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE))
    {
        return FALSE;
    }

    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;
    KiSaveProcessorState(TrapFrame, ExceptionFrame);

    for (;;)
    {
        if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
        {
            break;
        }

        if (CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE)
        {
            KCONTINUE_STATUS ContinueStatus;

            ContinueStatus = KdReportProcessorChange();
            CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

            if ((ContinueStatus == ContinueSuccess) && (KiFreezeOwner != NULL))
            {
                KiFreezeOwner->IpiFrozen = IPI_FROZEN_STATE_THAW;
            }
        }

        YieldProcessor();
        KeMemoryBarrier();
    }

    KiRestoreProcessorState(TrapFrame, ExceptionFrame);
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;
    return TRUE;
}

static
VOID
KiArm64WaitForFrozenTargets(
    _In_ KAFFINITY TargetProcessors)
{
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (TargetProcessors & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }
}

static
VOID
KiArm64RequestThaw(
    _In_ PKPRCB CurrentPrcb)
{
    /* Thaw exactly the set that KxFreezeExecution froze -- re-deriving it from
       KeActiveProcessors would pick up a processor that came online while we
       were in the debugger and was therefore never frozen. */
    KAFFINITY TargetProcessors = KiFrozenProcessors & ~CurrentPrcb->SetMember;

    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (TargetProcessors & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_FROZEN);
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_THAW;
        }
    }

    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (TargetProcessors & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }

    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;
    InterlockedExchangePointer((PVOID *)&KiFreezeOwner, NULL);
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;

    if ((ProcessorIndex >= KeNumberProcessors) ||
        (ProcessorIndex >= MAXIMUM_PROCESSORS) ||
        (CurrentPrcb == NULL))
    {
        return ContinueProcessorReselected;
    }

    TargetPrcb = KiProcessorBlock[ProcessorIndex];
    if ((TargetPrcb == NULL) || (TargetPrcb == CurrentPrcb))
    {
        return ContinueProcessorReselected;
    }

    /* We can only hand control to a processor that takes part in this freeze
       (a frozen target, or the freeze owner). One that was still coming online
       when the freeze started is running freely and would never hand back. */
    if (!(KiFrozenProcessors & TargetPrcb->SetMember))
    {
        return ContinueProcessorReselected;
    }

    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);
    CurrentPrcb->IpiFrozen &= ~IPI_FROZEN_FLAG_ACTIVE;
    TargetPrcb->IpiFrozen |= IPI_FROZEN_FLAG_ACTIVE;

    if (KiFreezeOwner != CurrentPrcb)
    {
        return ContinueNextProcessor;
    }

    while (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_OWNER)
    {
        YieldProcessor();
        KeMemoryBarrier();
    }

    if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
    {
        CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;
        return ContinueSuccess;
    }

    ASSERT(CurrentPrcb->IpiFrozen ==
           (IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE));
    return ContinueProcessorReselected;
}

VOID
NTAPI
KxFreezeExecution(
    VOID)
{
    PKPRCB CurrentPrcb;
    KAFFINITY TargetProcessors;

    CurrentPrcb = KeGetCurrentPrcb();
    if (CurrentPrcb == NULL)
    {
        return;
    }

    if (CurrentPrcb == KiFreezeOwner)
    {
        return;
    }

    while (InterlockedCompareExchangePointer((PVOID *)&KiFreezeOwner,
                                              CurrentPrcb,
                                              NULL) != NULL)
    {
        while (KiFreezeOwner != NULL)
        {
            YieldProcessor();
            KeMemoryBarrier();
        }
    }

    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;

    /* Take a single snapshot of the processors taking part in this freeze.
       Only processors already in KeActiveProcessors have a live GIC CPU
       interface and can answer the freeze SGI, so they are the only ones we
       may mark and wait for. We add ourselves unconditionally in case we are
       freezing from within our own bring-up. Everything below is derived from
       this snapshot and never re-reads the globals. */
    KiFrozenProcessors = KeActiveProcessors | CurrentPrcb->SetMember;
    TargetProcessors = KiFrozenProcessors & ~CurrentPrcb->SetMember;

    /* Publish the set before the first target can observe TARGET_FREEZE */
    KeMemoryBarrier();

    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (TargetProcessors & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_TARGET_FREEZE;
        }
    }

    /*
     * Send IPI directly via HalRequestIpi, NOT through KiIpiSend.
     *
     * KiIpiSend uses InterlockedBitTestAndSet on IpiFrozen to signal the
     * IPI type, but the freeze code uses IpiFrozen as a STATE value
     * (IPI_FROZEN_STATE_TARGET_FREEZE = 5). KiIpiSend would corrupt the
     * state by setting bit IPI_FREEZE (bit 4), changing 5 → 21.
     *
     * The freeze state is already communicated via IpiFrozen assignment above.
     * We just need the SGI to interrupt the target CPUs.
     */
    HalRequestIpi(TargetProcessors);
    KiArm64WaitForFrozenTargets(TargetProcessors);
}

VOID
NTAPI
KxThawExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    if ((CurrentPrcb == NULL) || !(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE))
    {
        return;
    }

    KiArm64RequestThaw(CurrentPrcb);
}

#else /* !CONFIG_SMP */

static PKPRCB KiSingleProcessorFreezeOwner;

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);
    return FALSE;
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
    UNREFERENCED_PARAMETER(ProcessorIndex);
    return ContinueProcessorReselected;
}

VOID
NTAPI
KxFreezeExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    if (CurrentPrcb == NULL)
    {
        return;
    }

    if (KiSingleProcessorFreezeOwner == CurrentPrcb)
    {
        return;
    }

    KiSingleProcessorFreezeOwner = CurrentPrcb;
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;
}

VOID
NTAPI
KxThawExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    if (CurrentPrcb != NULL)
    {
        CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;
    }

    KiSingleProcessorFreezeOwner = NULL;
}

#endif /* CONFIG_SMP */
