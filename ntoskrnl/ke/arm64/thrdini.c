/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/arm64/thrdini.c
 * PURPOSE:         Thread startup stubs for ARM64
 */

#include <ntoskrnl.h>
#include <fpstate.h>
#define NDEBUG
#include <debug.h>

/*
 * Stack layout used during context acquisition:
 *
 *   +-------------------------------+
 *   |   KTRAP_FRAME (user threads)  |
 *   +-------------------------------+
 *   | KEXCEPTION_FRAME (user only)  |
 *   +-------------------------------+
 *   |        KSTART_FRAME           |
 *   +-------------------------------+
 *   |       KSWITCH_FRAME           |  <- Thread->KernelStack
 *   +-------------------------------+
 */

typedef struct _KUINIT_FRAME
{
    KSWITCH_FRAME SwitchFrame;
    KSTART_FRAME StartFrame;
    KEXCEPTION_FRAME ExceptionFrame;
    KTRAP_FRAME TrapFrame;
} KUINIT_FRAME, *PKUINIT_FRAME;

typedef struct _KKINIT_FRAME
{
    KSWITCH_FRAME SwitchFrame;
    KSTART_FRAME StartFrame;
} KKINIT_FRAME, *PKKINIT_FRAME;

VOID
NTAPI
KiThreadStartup(VOID);

VOID
NTAPI
KiRetireDpcListInDpcStack(
    _In_ PKPRCB Prcb,
    _In_opt_ PVOID DpcStack);

static
BOOLEAN
KiArm64ShouldTraceIdle(VOID)
{
    return FALSE;
}

static
VOID
KiArm64SetupStartFrame(
    _Inout_ PKSTART_FRAME StartFrame,
    _In_ PKSYSTEM_ROUTINE SystemRoutine,
    _In_ PKSTART_ROUTINE StartRoutine,
    _In_opt_ PVOID StartContext)
{
    RtlZeroMemory(StartFrame, sizeof(*StartFrame));
    StartFrame->StartRoutine = (ULONG64)StartRoutine;
    StartFrame->StartContext = (ULONG64)StartContext;
    StartFrame->SystemRoutine = (ULONG64)SystemRoutine;
}

VOID
NTAPI
KiInitializeContextThread(_Inout_ PKTHREAD Thread,
                          _In_ PKSYSTEM_ROUTINE SystemRoutine,
                          _In_ PKSTART_ROUTINE StartRoutine,
                          _In_opt_ PVOID StartContext,
                          _In_opt_ PCONTEXT ContextPointer)
{
    ULONG_PTR StackTop;
    PKSWITCH_FRAME SwitchFrame;
    PKSTART_FRAME StartFrame;

    ASSERT(Thread != NULL);
    ASSERT(SystemRoutine != NULL);

    StackTop = (ULONG_PTR)ALIGN_DOWN_POINTER_BY(Thread->InitialStack, 16);
    Thread->InitialStack = (PVOID)StackTop;

    if (ContextPointer != NULL)
    {
        PKUINIT_FRAME InitFrame;
        PKEXCEPTION_FRAME ExceptionFrame;
        PKTRAP_FRAME TrapFrame;

        {
            SIZE_T FrameSize = ALIGN_UP_BY(sizeof(*InitFrame), 16);
            InitFrame = (PKUINIT_FRAME)(StackTop - FrameSize);
        }
        RtlZeroMemory(InitFrame, sizeof(*InitFrame));

        SwitchFrame = &InitFrame->SwitchFrame;
        StartFrame = &InitFrame->StartFrame;
        ExceptionFrame = &InitFrame->ExceptionFrame;
        TrapFrame = &InitFrame->TrapFrame;

        KiArm64SetupStartFrame(StartFrame,
                               SystemRoutine,
                               StartRoutine,
                               StartContext);

        Thread->PreviousMode = UserMode;
        Thread->KernelStack = SwitchFrame;
        Thread->TrapFrame = TrapFrame;

        KeContextToTrapFrame(ContextPointer,
                             ExceptionFrame,
                             TrapFrame,
                             ContextPointer->ContextFlags | CONTEXT_CONTROL,
                             UserMode);

        TrapFrame->PreviousMode = UserMode;
        TrapFrame->ContextFromKFramesUnwound = FALSE;
        TrapFrame->DebugRegistersValid = FALSE;

        ExceptionFrame->Return = (ULONG64)KiThreadStartup;
    }
    else
    {
        PKKINIT_FRAME InitFrame;

        {
            SIZE_T FrameSize = ALIGN_UP_BY(sizeof(*InitFrame), 16);
            InitFrame = (PKKINIT_FRAME)(StackTop - FrameSize);
        }
        RtlZeroMemory(InitFrame, sizeof(*InitFrame));

        SwitchFrame = &InitFrame->SwitchFrame;
        StartFrame = &InitFrame->StartFrame;

        KiArm64SetupStartFrame(StartFrame,
                               SystemRoutine,
                               StartRoutine,
                               StartContext);

        Thread->PreviousMode = KernelMode;
        Thread->KernelStack = SwitchFrame;
        Thread->TrapFrame = NULL;
    }

    SwitchFrame->Return = (ULONG64)KiThreadStartup;
    SwitchFrame->ApcBypass = APC_LEVEL;
}

#ifdef CONFIG_SMP
/*
 * Multiprocessor idle loop.
 *
 * As on x64, the idle thread runs at DISPATCH_LEVEL. Other processors hand it
 * work by setting NextThread or by inserting into this processor's ready
 * queues, both under the PRCB lock, so the idle thread claims work under that
 * lock too. A thread first scheduled from here starts in KiThreadStartup,
 * which lowers to APC_LEVEL, so it must be entered at DISPATCH_LEVEL, never at
 * PASSIVE_LEVEL. The clock, device and IPI interrupts are all above
 * DISPATCH_LEVEL and wake the WFE; DPCs requested for this processor are
 * polled through DpcInterruptRequested, since software interrupts are only
 * delivered by KfLowerIrql and nothing here lowers below DISPATCH_LEVEL.
 */
static
DECLSPEC_NORETURN
VOID
KiIdleLoopMp(
    _In_ PKPRCB Prcb)
{
    PKTHREAD OldThread, NewThread;
    KIRQL Irql;

    /* Run at DISPATCH_LEVEL, whatever level the start-up path left */
    Irql = KeGetCurrentIrql();
    if (Irql > DISPATCH_LEVEL)
        KeLowerIrql(DISPATCH_LEVEL);
    else if (Irql < DISPATCH_LEVEL)
        KfRaiseIrql(DISPATCH_LEVEL);

    for (;;)
    {
        ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);
        ASSERT(Prcb->CurrentThread == Prcb->IdleThread);

        /* Retire DPCs, timers and deferred-ready threads with interrupts off */
        _disable();
        __asm__ __volatile__("dmb ish" ::: "memory");
        if (Prcb->DpcData[0].DpcQueueDepth ||
            Prcb->TimerRequest ||
            Prcb->DeferredReadyListHead.Next ||
            Prcb->DpcInterruptRequested)
        {
            HalClearSoftwareInterrupt(DISPATCH_LEVEL);
            Prcb->DpcInterruptRequested = FALSE;
            KiRetireDpcList(Prcb);
        }
        _enable();

        /* Claim the next thread, or a ready one that did not preempt us */
        KiAcquirePrcbLock(Prcb);
        NewThread = Prcb->NextThread;
        if (NewThread != NULL)
        {
            Prcb->NextThread = NULL;
        }
        else
        {
            NewThread = KiSelectReadyThread(0, Prcb);
        }

        if (NewThread != NULL)
        {
            OldThread = Prcb->CurrentThread;
            Prcb->CurrentThread = NewThread;
            NewThread->State = Running;
            KiReleasePrcbLock(Prcb);

            /* Switch away; the idle thread resumes here at DISPATCH_LEVEL */
            KiSwapContext(APC_LEVEL, OldThread);
            continue;
        }
        KiReleasePrcbLock(Prcb);

        /* Nothing to run: wait for an interrupt or an event */
        __asm__ __volatile__("wfe" ::: "memory");
    }
}
#endif

DECLSPEC_NORETURN
VOID
KiIdleLoop(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();

    /*
     * ARM64 UP kernel: APs must NOT touch scheduler state.
     *
     * On a UP kernel (no CONFIG_SMP), spinlocks are no-ops, scheduler data
     * structures are not protected, and there is only one logical CPU as
     * far as the kernel is concerned. APs simply spin in WFI.
     *
     * On an SMP kernel (CONFIG_SMP), APs run the full idle loop below
     * so they can receive IPIs and dispatch threads.
     */
#ifdef CONFIG_SMP
    KiIdleLoopMp(Prcb);
#else
    if (Prcb != NULL && Prcb->Number != 0)
    {
        __asm__ __volatile__("msr daifset, #3" ::: "memory");
        __asm__ __volatile__("isb" ::: "memory");
        for (;;)
        {
            __asm__ __volatile__("wfi" ::: "memory");
        }
    }
#endif

    for (;;)
    {
        /*
         * ARM64 CRITICAL: Issue a data memory barrier before checking scheduler
         * state variables. This ensures we see updates from other CPUs/threads
         * that may have modified DPC queues or ready lists.
         *
         * Without this barrier, the weakly-ordered ARM64 memory model could cause
         * us to see stale values and miss ready threads or pending DPCs.
         */
        __asm__ __volatile__("dmb ishld" ::: "memory");

        /*
         * Check for pending DPC work. On ARM64, also check DpcInterruptRequested
         * which is set by HalRequestSoftwareInterrupt when a DPC is queued.
         */
        if (Prcb->DpcData[0].DpcQueueDepth ||
            Prcb->TimerRequest ||
            Prcb->DeferredReadyListHead.Next ||
            Prcb->DpcInterruptRequested)
        {
            if (KiArm64ShouldTraceIdle())
            {
                DbgPrintEx(DPFLTR_DEFAULT_ID,
                           DPFLTR_ERROR_LEVEL,
                           "[arm64][IDLE] dispatch work: DpcDepth=%lu TimerReq=%p Deferred=%p DpcReq=%u Next=%p Ready=0x%Ix\n",
                           Prcb->DpcData[0].DpcQueueDepth,
                           Prcb->TimerRequest,
                           Prcb->DeferredReadyListHead.Next,
                           Prcb->DpcInterruptRequested,
                           Prcb->NextThread,
                           Prcb->ReadySummary);
            }

            HalClearSoftwareInterrupt(DISPATCH_LEVEL);
            Prcb->DpcInterruptRequested = FALSE;
            KiRetireDpcList(Prcb);

            /* ARM64: Memory barrier after KiRetireDpcList to ensure NextThread is visible */
            __asm__ __volatile__("dmb ish" ::: "memory");
            continue;
        }

        if (Prcb->NextThread)
        {
            PKTHREAD OldThread, NewThread;
            KIRQL OldIrql = KeGetCurrentIrql();

            /* Take the next thread under the PRCB lock: other processors
             * replace it, or ready it elsewhere, under that lock while it is
             * on standby (KiUpdateEffectiveAffinityThread,
             * KiSetPriorityThread, KiDeferredReadyThread), and a thread taken
             * here without it runs on two processors */
            if (OldIrql < DISPATCH_LEVEL)
                KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
            KiAcquirePrcbLock(Prcb);
            OldThread = Prcb->CurrentThread;
            NewThread = Prcb->NextThread;
            if (NewThread == NULL)
            {
                KiReleasePrcbLock(Prcb);
                KeLowerIrql(OldIrql);
                continue;
            }

            if (KiArm64ShouldTraceIdle())
            {
                DbgPrintEx(DPFLTR_DEFAULT_ID,
                           DPFLTR_ERROR_LEVEL,
                           "[arm64][IDLE] swap: Old=%p OldPri=%d OldWait=%u OldBusy=%u New=%p NewPri=%d NewBusy=%u\n",
                           OldThread,
                           OldThread ? OldThread->Priority : -1,
                           OldThread ? OldThread->WaitIrql : 0xFF,
#if (NTDDI_VERSION >= NTDDI_WIN7)
                           0,
#else
                           OldThread ? OldThread->SwapBusy : 0xFF,
#endif
                           NewThread,
                           NewThread ? NewThread->Priority : -1,
#if (NTDDI_VERSION >= NTDDI_WIN7)
                           0);
#else
                           NewThread ? NewThread->SwapBusy : 0xFF);
#endif
            }

            Prcb->NextThread = NULL;
            Prcb->CurrentThread = NewThread;
            NewThread->State = Running;
            KiReleasePrcbLock(Prcb);

            ASSERT(OldThread != NULL);
            KiSwapContext(OldThread->WaitIrql, OldThread);
            continue;
        }

        /*
         * No thread was handed to us via NextThread, but a runnable thread may
         * still be sitting in the ready queue at a priority that did not
         * preempt the idle thread -- for example the priority-0 zero-page
         * thread, readied while we run as the priority-0 idle thread. The
         * reschedule path only promotes a thread to NextThread when it can
         * preempt the current thread, so such a thread would otherwise never be
         * dispatched and the box would idle forever with runnable work pending
         * (this is exactly what hangs system shutdown, which waits on a worker
         * APC delivered to the zero-page thread). Select and dispatch it here,
         * the same way KiSwapThread does, under the PRCB lock: other
         * processors queue threads to this processor's ready lists under it.
         */
        if (Prcb->ReadySummary)
        {
            PKTHREAD OldThread, NewThread;
            KIRQL OldIrql = KeGetCurrentIrql();

            if (OldIrql < DISPATCH_LEVEL)
                KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
            KiAcquirePrcbLock(Prcb);
            OldThread = Prcb->CurrentThread;
            NewThread = KiSelectReadyThread(0, Prcb);

            if (NewThread != NULL)
            {
                NewThread->State = Running;
                Prcb->CurrentThread = NewThread;
                KiReleasePrcbLock(Prcb);

                ASSERT(OldThread != NULL);
                KiSwapContext(OldThread->WaitIrql, OldThread);
                continue;
            }

            KiReleasePrcbLock(Prcb);
            KeLowerIrql(OldIrql);
        }

        /*
         * KiInitializeKernel enters the idle loop after raising to HIGH_LEVEL.
         * On ARM64, clearing DAIF.I alone is not enough because the GIC PMR may
         * still be masking every interrupt. Drop the logical IRQL before opening
         * the idle interrupt window so timer/device interrupts can schedule work.
         */
        if (KeGetCurrentIrql() > PASSIVE_LEVEL)
        {
            KeLowerIrql(PASSIVE_LEVEL);
        }

        _enable();
        __asm__ __volatile__("wfe" ::: "memory");
        _disable();
    }
}

/*
 * Debug trace function called from assembly at key points.
 * Uses DbgPrintEx which should work regardless of NDEBUG.
 */
VOID
FASTCALL
KiDebugTracePoint(
    _In_ ULONG Point,
    _In_ ULONG64 Value1,
    _In_ ULONG64 Value2)
{
    UNREFERENCED_PARAMETER(Point);
    UNREFERENCED_PARAMETER(Value1);
    UNREFERENCED_PARAMETER(Value2);
}

/*
 * Debug function to dump KSWITCH_FRAME contents before returning.
 * Called from assembly to verify stack frame integrity.
 */
VOID
FASTCALL
KiDebugDumpSwitchFrame(
    _In_ PVOID StackPointer,
    _In_ ULONG64 SwLrValue)
{
    /* Disable verbose logging to reduce log spam - only log on errors */
    UNREFERENCED_PARAMETER(StackPointer);
    UNREFERENCED_PARAMETER(SwLrValue);

#if 0  /* Enable for detailed context switch debugging */
    PKSWITCH_FRAME Frame = (PKSWITCH_FRAME)StackPointer;
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKTHREAD IdleThread = Prcb ? Prcb->IdleThread : NULL;
    PKTHREAD CurrentThread = Prcb ? Prcb->CurrentThread : NULL;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
               "[KiDebugDumpSwitchFrame] SP=%p SwLr=0x%llx\n",
               StackPointer, SwLrValue);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
               "[KiDebugDumpSwitchFrame] CurrentThread=%p IdleThread=%p IsIdle=%d\n",
               CurrentThread, IdleThread, (CurrentThread == IdleThread) ? 1 : 0);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
               "[KiDebugDumpSwitchFrame] Frame->Lr=0x%llx Frame->ReturnAddress=0x%llx ApcBypass=%u\n",
               Frame->Lr, Frame->ReturnAddress, Frame->ApcBypass);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
               "[KiDebugDumpSwitchFrame] X19=0x%llx X20=0x%llx Fp=0x%llx\n",
               Frame->X19, Frame->X20, Frame->Fp);
#endif
}

/*
 * Debug function called at .Lrestore_context to confirm we reached it.
 */
VOID
FASTCALL
KiDebugRestoreContext(
    _In_ PVOID StackPointer)
{
    UNREFERENCED_PARAMETER(StackPointer);
}

/*
 * Debug function called right before ret x9 to confirm final return.
 */
VOID
FASTCALL
KiDebugBeforeReturn(
    _In_ ULONG64 ReturnAddr)
{
    UNREFERENCED_PARAMETER(ReturnAddr);
}

BOOLEAN
FASTCALL
KiSwapContextResume(
    _In_ KIRQL WaitIrql,
    _Inout_ PKTHREAD OldThread,
    _Inout_ PKTHREAD NewThread)
{
    PKPRCB Prcb;
    PKPROCESS OldProcess, NewProcess;

    ASSERT(OldThread != NULL);
    ASSERT(NewThread != NULL);

    if (KiArm64ShouldTraceIdle())
    {
        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "[arm64][CTX] Resume entry: WaitIrql=%u CurIrql=%u Old=%p(S=%u,P=%d,Busy=%u) New=%p(S=%u,P=%d,Busy=%u)\n",
                   WaitIrql,
                   KeGetCurrentIrql(),
                   OldThread,
                   OldThread->State,
                   OldThread->Priority,
#if (NTDDI_VERSION >= NTDDI_WIN7)
                   0,
#else
                   OldThread->SwapBusy,
#endif
                   NewThread,
                   NewThread->State,
                   NewThread->Priority,
#if (NTDDI_VERSION >= NTDDI_WIN7)
                   0);
#else
                   NewThread->SwapBusy);
#endif
    }

    Prcb = KeGetCurrentPrcb();

    NewThread->ContextSwitches++;

    /*
     * Update per-CPU current thread pointer.
     * On SMP, only write the PRCB (per-CPU) - the global is a UP-only fallback.
     * Writing the global on SMP corrupts other CPUs' view of their current thread.
     */
    if (Prcb != NULL)
    {
        Prcb->KeContextSwitches++;
        Prcb->CurrentThread = NewThread;
    }
#ifndef CONFIG_SMP
    KeArm64CurrentThread = NewThread;
#endif

    /*
     * The switch may have been initiated from an interrupt handler, where
     * the exception entry set PSTATE.I. That CPU-level mask belongs to the
     * interrupted context, not to the incoming thread: IRQL is represented by
     * the GIC PMR alone, and a thread blocks and resumes with interrupts
     * enabled, as on x64. Restore that invariant here, or the resumed thread
     * keeps running with interrupts hard-masked at any IRQL. The PMR still
     * gates interrupts by the IRQL the thread resumes at.
     */
    __asm__ __volatile__("msr daifclr, #0xb" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");

    /* Old thread is no longer busy; release the swap-busy flag the switch-to
       spin waits on, now that its KernelStack has been saved. Win7+ dropped the
       dedicated SwapBusy byte and reuses Running (see KiSetThreadSwapBusy). */
#if (NTDDI_VERSION < NTDDI_WIN7)
    OldThread->SwapBusy = FALSE;
#else
    OldThread->Running = FALSE;
#endif
#ifdef _M_ARM64
    /* ARM64 has weak ordering: publish the KernelStack store before the flag
       release, and wake any processor parked (WFE) on the flag. */
    __asm__ __volatile__("dmb ishst" ::: "memory");
    __asm__ __volatile__("sev" ::: "memory");
#endif

    /*
     * ARM64: Switch address space (TTBR0) when moving to a different process.
     * This is critical for user-mode execution - without this, user-mode code
     * would execute with the wrong page tables and access the wrong memory.
     *
     * KiSwapProcess will:
     * 1. Update process ActiveProcessors for SMP
     * 2. Write the new process's DirectoryTableBase[0] to TTBR0_EL1
     * 3. Perform necessary TLB invalidation
     */
    /* NtCurrentTeb in kernel mode reads it from the PCR (NtTib.Self) */
    KeGetPcr()->Used_Self = NewThread->Teb;

    OldProcess = OldThread->ApcState.Process;
    NewProcess = NewThread->ApcState.Process;

    if (OldProcess != NewProcess)
    {
        KiSwapProcess(NewProcess, OldProcess);

        if (KiArm64ShouldTraceIdle())
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "[arm64][CTX] Resume after KiSwapProcess\n");
        }
    }

    /*
     * FP/SIMD trap-on-first-use remains deferred until the ARM64 context-switch
     * path has validated IRQL-safe state handling.
     */

    /*
     * ARM64 ABI: Do NOT set x18=TEB or tpidr_el0=TEB here.
     *
     * Per Microsoft's ARM64 Windows ABI conventions:
     *   - In kernel mode (EL1): x18 = KPCR  (platform register)
     *   - In user mode  (EL0): x18 = TEB
     *   - TPIDR_EL0: reserved (not part of the ABI contract)
     *
     * Setting x18=TEB while still executing in EL1 violates the ABI and
     * corrupts the kernel's platform register.  The correct location for
     * x18=TEB and tpidr_el0=TEB is KiTrapReturn (trapret.S), which sets
     * them immediately before ERET to EL0.
     *
     * TPIDR_EL0 is written in trapret.S as an internal convenience for
     * user-mode NtCurrentTeb() implementations, but it is NOT backed by
     * Microsoft's ABI contract (they mark it as "reserved").
     */

    if (NewThread->ApcState.KernelApcPending &&
        !NewThread->SpecialApcDisable &&
        !WaitIrql)
    {
        if (KiArm64ShouldTraceIdle())
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "[arm64][CTX] Resume exit: return TRUE for APC delivery\n");
        }
        return TRUE;
    }

    if (NewThread->ApcState.KernelApcPending)
    {
        if (KiArm64ShouldTraceIdle())
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "[arm64][CTX] Resume: New thread has KernelApcPending, WaitIrql=%u\n",
                       WaitIrql);
        }

        HalRequestSoftwareInterrupt(APC_LEVEL);
    }

    if (KiArm64ShouldTraceIdle())
    {
        ULONG64 TpidrEl1;

        __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(TpidrEl1));

        DbgPrintEx(DPFLTR_DEFAULT_ID,
                   DPFLTR_ERROR_LEVEL,
                   "[arm64][CTX] Resume exit: return FALSE CurThread=%p tpidr=%p pcr=%p fallbackThread=%p\n",
                   KeGetCurrentThread(),
                   (PVOID)(ULONG_PTR)TpidrEl1,
                   KeArm64CurrentPcr,
                   KeArm64CurrentThread);
    }

    return FALSE;
}

VOID
NTAPI
KiDispatchInterrupt(VOID)
{
    PKIPCR Pcr = (PKIPCR)KeGetPcr();
    PKPRCB Prcb = &Pcr->Prcb;
    PKTHREAD NewThread, OldThread;
    KIRQL OldIrql;

    /*
     * ARM64 DPC/Dispatch Interrupt Handler
     *
     * This function processes pending DPCs and handles thread scheduling.
     * It follows the same pattern as AMD64's KiDpcInterruptHandler:
     *
     * 1. Save current IRQL and raise to DISPATCH_LEVEL
     * 2. Process DPCs (retire DPC list)
     * 3. Handle quantum end or thread switch if needed
     * 4. Restore original IRQL at the END
     *
     * CRITICAL: The IRQL restoration MUST happen at the very end of this
     * function, AFTER any KiQuantumEnd() or context switch operations.
     * This is because:
     * - KiQuantumEnd() raises IRQL to SYNCH_LEVEL then lowers to DISPATCH_LEVEL
     * - KiSwapContext may change threads and IRQL state
     *
     * If we restore IRQL before these operations, the function will return
     * with IRQL stuck at DISPATCH_LEVEL instead of the original IRQL,
     * causing PAGED_CODE assertions to fail in subsequent code.
     *
     * This function is called from two contexts:
     * 1. Hardware IRQ handler (interrupt.c) - already at elevated IRQL
     * 2. KfLowerIrql (irql.c) - at the newly lowered IRQL
     *
     * We use direct IRQL manipulation (KiSetCurrentIrql/KiApplyIrqMaskForIrqlTransition)
     * instead of KfLowerIrql() to avoid infinite recursion, since KfLowerIrql()
     * calls this function when DpcInterruptRequested is set.
     */

    /* Save current IRQL and raise to DISPATCH_LEVEL for DPC processing */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL)
    {
        KfRaiseIrql(DISPATCH_LEVEL);
    }

    _disable();

    /* Process pending DPCs, timers, and deferred ready threads */
    if ((Prcb->DpcData[0].DpcQueueDepth) ||
        (Prcb->TimerRequest) ||
        (Prcb->DeferredReadyListHead.Next))
    {
        KiRetireDpcListInDpcStack(Prcb, Prcb->DpcStack);
    }

    _enable();

    /* Handle quantum end - this may raise/lower IRQL internally */
    if (Prcb->QuantumEnd)
    {
        Prcb->QuantumEnd = FALSE;
        KiQuantumEnd();
    }
    else if (Prcb->NextThread)
    {
        /* Thread switch needed - acquire PRCB lock and swap context */
        KiAcquirePrcbLock(Prcb);

        OldThread = Prcb->CurrentThread;
        NewThread = Prcb->NextThread;

        /* The old thread is queued ready below, and another processor may
         * pick it up before its context is saved here: mark it swap busy so
         * that processor waits for the save (KiSwapContext) */
        KiSetThreadSwapBusy(OldThread);

        Prcb->NextThread = NULL;
        Prcb->CurrentThread = NewThread;

        NewThread->State = Running;
        OldThread->WaitReason = WrDispatchInt;

        KxQueueReadyThread(OldThread, Prcb);

        KiSwapContext(APC_LEVEL, OldThread);
    }

    /*
     * CRITICAL: Restore original IRQL at the END, after all dispatch work.
     *
     * Following the AMD64 KiDpcInterruptHandler pattern:
     * 1. Disable interrupts to prevent new hardware interrupts during lowering
     * 2. Call KeLowerIrql to restore original IRQL
     * 3. Re-enable interrupts (ARM64-specific: DAIF.I must be explicitly cleared)
     *
     * Note: KeLowerIrql may check for DpcInterruptRequested and call back into
     * KiDispatchInterrupt, but this is safe because:
     * - DpcInterruptRequested is cleared BEFORE calling KiDispatchInterrupt in irql.c
     * - So we won't get infinite recursion, just one level of "retry" if new DPCs
     *   were queued during our dispatch processing
     *
     * If no new DPCs were queued, KeLowerIrql simply lowers the IRQL.
     *
     * ARM64 CRITICAL: Unlike x86/AMD64 where IRET restores RFLAGS.IF automatically,
     * ARM64's DAIF.I bit persists after function return. We must explicitly call
     * _enable() to unmask interrupts after KeLowerIrql() completes. Without this,
     * the timer interrupt (PPI 27) would remain masked and never fire, causing
     * massive delays in timer expiration (100+ second gaps between timer ticks).
     */
    _disable();
    KeLowerIrql(OldIrql);
    _enable();
}
