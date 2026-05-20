/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         stubs
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

ULONG ProcessCount;
SIZE_T KeXStateLength = sizeof(XSAVE_FORMAT);

PVOID
KiSwitchKernelStackHelper(
    LONG_PTR StackOffset,
    PVOID OldStackBase);

VOID
NTAPI
KiCalloutOnNewStack(
    _In_ PEXPAND_STACK_CALLOUT Callout,
    _In_opt_ PVOID Parameter,
    _In_ PVOID NewStackTop);

NTSTATUS
NTAPI
KeExpandKernelStackAndCalloutEx(
    _In_ PEXPAND_STACK_CALLOUT Callout,
    _In_opt_ PVOID Parameter,
    _In_ SIZE_T Size,
    _In_ BOOLEAN Wait,
    _In_opt_ PVOID Context)
{
    PKTHREAD CurrentThread;
    PETHREAD CurrentEthread;
    PVOID NewStackBase;
    PVOID NewStackLimit;
    PVOID SavedStackBase;
    ULONG_PTR SavedStackLimit;
    PVOID SavedInitialStack;
    BOOLEAN SavedLargeStack;
    SIZE_T CommitSize;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    if (Callout == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    CurrentThread = KeGetCurrentThread();
    CurrentEthread = CONTAINING_RECORD(CurrentThread, ETHREAD, Tcb);

    CommitSize = max(Size, KERNEL_LARGE_STACK_COMMIT);
    if (CommitSize > MAXIMUM_EXPANSION_SIZE)
    {
        CommitSize = MAXIMUM_EXPANSION_SIZE;
    }

#if (NTDDI_VERSION >= NTDDI_WIN8)
    if (CurrentEthread->LargeStack)
#else
    if (CurrentThread->LargeStack)
#endif
    {
        Status = MmGrowKernelStackEx(CurrentThread->StackBase, CommitSize);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Callout(Parameter);
        return STATUS_SUCCESS;
    }

    /*
     * Allocate a temporary large kernel stack on which to run the callout.
     * The thread's original stack is left intact so that any pointers from
     * heap structures (such as Irp->UserEvent populated by IopMountVolume)
     * into our caller's frames remain valid for the duration of the
     * callout and after it returns.
     */
    NewStackBase = MmCreateKernelStack(TRUE, 0);
    if (NewStackBase == NULL)
    {
        return STATUS_NO_MEMORY;
    }

    /* Save the thread's stack bookkeeping so we can restore it afterwards. */
    SavedStackBase = CurrentThread->StackBase;
    SavedStackLimit = (ULONG_PTR)CurrentThread->StackLimit;
    SavedInitialStack = CurrentThread->InitialStack;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    SavedLargeStack = CurrentEthread->LargeStack;
#else
    SavedLargeStack = CurrentThread->LargeStack;
#endif

    /*
     * Point the thread at the new stack while the callout runs so that
     * stack-overflow checks and unwinding queries inside the callout see a
     * consistent stack range. The callout's RSP itself is switched by the
     * KiCalloutOnNewStack assembly helper.
     */
    CurrentThread->StackBase = NewStackBase;
    NewStackLimit = Add2Ptr(NewStackBase, -KERNEL_LARGE_STACK_COMMIT);
#if (NTDDI_VERSION >= NTDDI_WIN8)
    CurrentThread->StackLimit = (volatile PVOID)NewStackLimit;
#else
    CurrentThread->StackLimit = (ULONG_PTR)NewStackLimit;
#endif
    CurrentThread->InitialStack = NewStackBase;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    CurrentEthread->LargeStack = TRUE;
#else
    CurrentThread->LargeStack = TRUE;
#endif

    Status = MmGrowKernelStackEx(NewStackBase, CommitSize);
    if (!NT_SUCCESS(Status))
    {
        CurrentThread->StackBase = SavedStackBase;
#if (NTDDI_VERSION >= NTDDI_WIN8)
        CurrentThread->StackLimit = (volatile PVOID)SavedStackLimit;
#else
        CurrentThread->StackLimit = SavedStackLimit;
#endif
        CurrentThread->InitialStack = SavedInitialStack;
#if (NTDDI_VERSION >= NTDDI_WIN8)
        CurrentEthread->LargeStack = SavedLargeStack;
#else
        CurrentThread->LargeStack = SavedLargeStack;
#endif
        MmDeleteKernelStack(NewStackBase, TRUE);
        return Status;
    }

    KiCalloutOnNewStack(Callout, Parameter, NewStackBase);

    /*
     * Restore the thread's original stack bookkeeping.  KiSwapContextResume
     * mirrors KTHREAD::InitialStack into the per-CPU Pcr->Prcb.RspBase and
     * the TSS Rsp0 on every context switch.  If the callout caused us to be
     * preempted (likely for FS mount paths), those per-CPU fields now point
     * at the temporary stack we are about to free, which would corrupt any
     * subsequent syscall entry on this processor (KiSystemCallEntry64 reads
     * gs:[PcRspBase]).  Sync them back to the old stack under interrupt
     * disable so the freed stack is never reachable from a new syscall.
     */
    {
        PKIPCR Pcr;
        ULONG Eflags;

        Eflags = __readeflags();
        _disable();

        CurrentThread->StackBase = SavedStackBase;
#if (NTDDI_VERSION >= NTDDI_WIN8)
        CurrentThread->StackLimit = (volatile PVOID)SavedStackLimit;
#else
        CurrentThread->StackLimit = SavedStackLimit;
#endif
        CurrentThread->InitialStack = SavedInitialStack;
#if (NTDDI_VERSION >= NTDDI_WIN8)
        CurrentEthread->LargeStack = SavedLargeStack;
#else
        CurrentThread->LargeStack = SavedLargeStack;
#endif

        Pcr = (PKIPCR)KeGetPcr();
        Pcr->Prcb.RspBase = SavedInitialStack;
        Pcr->TssBase->Rsp0 = (ULONG64)SavedInitialStack;

        __writeeflags(Eflags);
    }

    MmDeleteKernelStack(NewStackBase, TRUE);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KeExpandKernelStackAndCallout(
    _In_ PEXPAND_STACK_CALLOUT Callout,
    _In_opt_ PVOID Parameter,
    _In_ SIZE_T Size)
{
    return KeExpandKernelStackAndCalloutEx(Callout,
                                           Parameter,
                                           Size,
                                           TRUE,
                                           NULL);
}

/*
 * Kernel stack layout (example pointers):
 * 0xFFFFFC0F'2D008000 KTHREAD::StackBase
 *    [XSAVE_AREA size == KeXStateLength = 0x440]
 * 0xFFFFFC0F'2D007BC0 KTHREAD::StateSaveArea _XSAVE_FORMAT
 * 0xFFFFFC0F'2D007B90 KTHREAD::InitialStack
 *    [0x190 bytes KTRAP_FRAME]
 * 0xFFFFFC0F'2D007A00 KTHREAD::TrapFrame
 *    [KSTART_FRAME] or ...
 *    [KSWITCH_FRAME]
 * 0xFFFFFC0F'2D007230 KTHREAD::KernelStack
 */

PVOID
NTAPI
KiSwitchKernelStack(PVOID StackBase, PVOID StackLimit)
{
    PKTHREAD CurrentThread;
    PVOID OldStackBase;
    LONG_PTR StackOffset;
    SIZE_T StackSize;
    PKIPCR Pcr;
    ULONG Eflags;

    /* Get the current thread */
    CurrentThread = KeGetCurrentThread();

    /* Save the old stack base */
    OldStackBase = CurrentThread->StackBase;

    /* Get the size of the current stack */
    StackSize = (ULONG_PTR)CurrentThread->StackBase - (ULONG_PTR)CurrentThread->StackLimit;
    ASSERT(StackSize <= (ULONG_PTR)StackBase - (ULONG_PTR)StackLimit);

    /* Copy the current stack contents to the new stack */
    RtlCopyMemory((PUCHAR)StackBase - StackSize,
                  (PVOID)CurrentThread->StackLimit,
                  StackSize);

    /* Calculate the offset between the old and the new stack */
    StackOffset = (PUCHAR)StackBase - (PUCHAR)CurrentThread->StackBase;

    /* Disable interrupts while messing with the stack */
    Eflags = __readeflags();
    _disable();

    /* Set the new trap frame */
    CurrentThread->TrapFrame = (PKTRAP_FRAME)Add2Ptr(CurrentThread->TrapFrame,
                                                     StackOffset);

    /* Set the new initial stack */
    CurrentThread->InitialStack = Add2Ptr(CurrentThread->InitialStack,
                                          StackOffset);

    /* Switch StateSaveArea */
    CurrentThread->StateSaveArea = Add2Ptr(CurrentThread->StateSaveArea,
                                           StackOffset);

    /* Set the new stack limits */
    CurrentThread->StackBase = StackBase;
    CurrentThread->StackLimit = (ULONG_PTR)StackLimit;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    CONTAINING_RECORD(CurrentThread, ETHREAD, Tcb)->LargeStack = TRUE;
#else
    CurrentThread->LargeStack = TRUE;
#endif

    /* Adjust RspBase in the PCR */
    Pcr = (PKIPCR)KeGetPcr();
    Pcr->Prcb.RspBase += StackOffset;

    /* Adjust Rsp0 in the TSS */
    Pcr->TssBase->Rsp0 += StackOffset;

    /* Restore interrupts */
    __writeeflags(Eflags);

    return OldStackBase;
}

DECLSPEC_NORETURN
VOID
KiIdleLoop(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKTHREAD OldThread, NewThread;

    /* Now loop forever */
    while (TRUE)
    {
        /* Start of the idle loop: disable interrupts */
        _enable();
        YieldProcessor();
        YieldProcessor();
        _disable();

        /* Check for pending timers, pending DPCs, or pending ready threads */
        if ((Prcb->DpcData[0].DpcQueueDepth) ||
            (Prcb->TimerRequest) ||
            (Prcb->DeferredReadyListHead.Next))
        {
            /* Quiesce the DPC software interrupt */
            HalClearSoftwareInterrupt(DISPATCH_LEVEL);

            /* Handle it */
            KiRetireDpcList(Prcb);
        }

        /* Check if a new thread is scheduled for execution */
        if (Prcb->NextThread)
        {
            /* Enable interrupts */
            _enable();

            /* Capture current thread data */
            OldThread = Prcb->CurrentThread;
            NewThread = Prcb->NextThread;

            /* Set new thread data */
            Prcb->NextThread = NULL;
            Prcb->CurrentThread = NewThread;

            /* The thread is now running */
            NewThread->State = Running;

#ifdef CONFIG_SMP
            /* Do the swap at SYNCH_LEVEL */
            KfRaiseIrql(SYNCH_LEVEL);
#endif

            /* Switch away from the idle thread */
            KiSwapContext(APC_LEVEL, OldThread);

#ifdef CONFIG_SMP
            /* Go back to DISPATCH_LEVEL */
            KeLowerIrql(DISPATCH_LEVEL);
#endif
        }
        else
        {
            /* Continue staying idle. Note the HAL returns with interrupts on */
            Prcb->PowerState.IdleFunction(&Prcb->PowerState);
        }
    }
}

VOID
NTAPI
KiSwapProcess(IN PKPROCESS NewProcess,
              IN PKPROCESS OldProcess)
{
    PKIPCR Pcr = (PKIPCR)KeGetPcr();

#ifdef CONFIG_SMP
    /* Update active processor mask */
    InterlockedXor64((PLONG64)&NewProcess->ActiveProcessors, Pcr->Prcb.SetMember);
    InterlockedXor64((PLONG64)&OldProcess->ActiveProcessors, Pcr->Prcb.SetMember);
#endif

    /* Update CR3 */
#if (NTDDI_VERSION >= NTDDI_LONGHORN)
    __writecr3(NewProcess->DirectoryTableBase);
#else
    __writecr3(NewProcess->DirectoryTableBase[0]);
#endif

    /* Update IOPM offset */
    Pcr->TssBase->IoMapBase = NewProcess->IopmOffset;
}

NTSTATUS
NTAPI
NtSetLdtEntries(ULONG Selector1, LDT_ENTRY LdtEntry1, ULONG Selector2, LDT_ENTRY LdtEntry2)
{
    UNIMPLEMENTED;
    __debugbreak();
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
NtVdmControl(IN ULONG ControlCode,
             IN PVOID ControlData)
{
    /* Not supported */
    return STATUS_NOT_IMPLEMENTED;
}
