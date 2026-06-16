/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IPI code for x64
 * COPYRIGHT:   Copyright 2023 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

/*
 * Generic-call IPI packet.  Only a single packet can be in flight at a time:
 * KeIpiGenericCall serialises all senders on KiReverseStallIpiLock and does not
 * return until every target has processed the packet, so plain globals are
 * sufficient and no stale packet can ever be observed by a late interrupt.
 */
static PKIPI_BROADCAST_WORKER KiIpiWorkerRoutine;
static ULONG_PTR KiIpiWorkerArgument;
static volatile KAFFINITY KiIpiTargetSet;

/* FUNCTIONS *****************************************************************/

VOID
FASTCALL
KiIpiSend(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
    /* Check if we can send the IPI directly */
    if (IpiRequest == IPI_APC)
    {
        HalSendSoftwareInterrupt(TargetSet, APC_LEVEL);
    }
    else if (IpiRequest == IPI_DPC)
    {
        HalSendSoftwareInterrupt(TargetSet, DISPATCH_LEVEL);
    }
    else if (IpiRequest == IPI_FREEZE)
    {
        /* On x64 the freeze IPI is an NMI */
        HalSendNMI(TargetSet);
    }
    else
    {
        ASSERT(FALSE);
    }
}

/*
 * Target-side handler for the generic-call IPI (APIC vector 0xE1, dispatched
 * from KiIpiInterrupt in trap.S at IPI_LEVEL with the APIC EOI already sent).
 */
VOID
NTAPI
KiIpiInterruptHandler(VOID)
{
#ifdef CONFIG_SMP
    KAFFINITY SetMember = KeGetCurrentPrcb()->SetMember;

    /*
     * Process the packet only if this processor is still an outstanding target.
     * This both ignores spurious interrupts and makes a duplicated IPI a no-op
     * once our bit has been cleared.
     */
    if (KiIpiTargetSet & SetMember)
    {
        /* The worker and argument were published before our bit was set */
        KiIpiWorkerRoutine(KiIpiWorkerArgument);

        /* Acknowledge: clearing our bit may release the waiting sender */
        InterlockedAnd64((volatile LONG64*)&KiIpiTargetSet, ~(LONG64)SetMember);
    }
#endif
}

/*
 * @implemented
 */
ULONG_PTR
NTAPI
KeIpiGenericCall(
    _In_ PKIPI_BROADCAST_WORKER Function,
    _In_ ULONG_PTR Argument)
{
    ULONG_PTR Status;
    KIRQL OldIrql, OldIrql2;
#ifdef CONFIG_SMP
    KAFFINITY TargetSet;
#endif

    /* Raise to DPC level if required */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* Serialise all generic calls: only one packet may be in flight */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

#ifdef CONFIG_SMP
    /* Target every active processor except ourselves */
    TargetSet = KeActiveProcessors & ~KeGetCurrentPrcb()->SetMember;
    if (TargetSet != 0)
    {
        /* Publish the packet, then the target set, then send the interrupt */
        KiIpiWorkerRoutine = Function;
        KiIpiWorkerArgument = Argument;
        InterlockedExchange64((volatile LONG64*)&KiIpiTargetSet, (LONG64)TargetSet);
        HalRequestIpi(TargetSet);
    }
#endif

    /* Run the worker on this processor at IPI_LEVEL too */
    KeRaiseIrql(IPI_LEVEL, &OldIrql2);
    Status = Function(Argument);
    KeLowerIrql(OldIrql2);

#ifdef CONFIG_SMP
    /* Wait for every target to acknowledge before releasing the packet */
    if (TargetSet != 0)
    {
        while (InterlockedCompareExchange64((volatile LONG64*)&KiIpiTargetSet, 0, 0) != 0)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }
    }
#endif

    /* Release the lock and lower IRQL back */
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);
    KeLowerIrql(OldIrql);
    return Status;
}
