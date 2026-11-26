/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/freeze.c
 * PURPOSE:         Routines for freezing and unfreezing processors for
 *                  kernel debugger synchronization.
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Freeze data */
KIRQL KiOldIrql;
ULONG KiFreezeFlag;

/* FUNCTIONS ******************************************************************/

BOOLEAN
NTAPI
KeFreezeExecution(IN PKTRAP_FRAME TrapFrame,
                  IN PKEXCEPTION_FRAME ExceptionFrame)
{
    BOOLEAN Enable;
    KIRQL OldIrql;

#ifndef CONFIG_SMP
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);
#endif

    /* Disable interrupts and get previous state */
    Enable = KeDisableInterrupts();

#ifndef CONFIG_SMP
    /* Raise IRQL if we have to */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL)
        OldIrql = KeRaiseIrqlToDpcLevel();
#else
    /* Raise IRQL to HIGH_LEVEL */
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
#endif

#ifdef CONFIG_SMP
    /* Architecture specific freeze code */
    KxFreezeExecution();
#endif

    /*
     * Only now that we own the freeze may we touch the freeze data: another
     * processor can be in its own KeFreezeExecution, spinning for the freeze,
     * and it writes the same globals once it gets it.
     */
    KiFreezeFlag = 4;

    /* Save the old IRQL to be restored on unfreeze */
    KiOldIrql = OldIrql;

    /* Return whether interrupts were enabled */
    return Enable;
}

VOID
NTAPI
KeThawExecution(IN BOOLEAN Enable)
{
    KIRQL OldIrql;

    /*
     * Take our old IRQL and clear the freeze flag while we still own the
     * freeze. As soon as KxThawExecution gives the freeze up, a processor that
     * was waiting for it takes it, freezes us (with an NMI, so even with our
     * interrupts disabled) and stores its own old IRQL in KiOldIrql. Reading
     * KiOldIrql after that returned us to the other processor's IRQL: a thread
     * that printed at PASSIVE_LEVEL came back from DbgPrint at DISPATCH_LEVEL.
     */
    OldIrql = KiOldIrql;
    KiFreezeFlag = 0;

#ifdef CONFIG_SMP
    /* Architecture specific thaw code */
    KxThawExecution();
#endif

    /* Cleanup CPU caches */
    KeFlushCurrentTb();

    /* Restore the old IRQL */
#ifndef CONFIG_SMP
    if (OldIrql < DISPATCH_LEVEL)
#endif
    KeLowerIrql(OldIrql);

    /* Re-enable interrupts */
    KeRestoreInterrupts(Enable);
}
