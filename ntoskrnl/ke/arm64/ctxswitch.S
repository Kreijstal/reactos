#include <asm.h>
#include <ksarm64.h>

    .text
    .align 4

	.global KiThreadStartup
	.global KiSwapContext
	.global KiSwapContextInternal
	.extern KfLowerIrql
	.extern KiRetireDpcList
	.extern KiTrapReturn

    .global KiRetireDpcListInDpcStack
    .seh_proc KiRetireDpcListInDpcStack
KiRetireDpcListInDpcStack:
    stp     x29, x30, [sp, #-16]!
    .seh_save_fplr_x 16
    mov     x29, sp
    .seh_set_fp
    .seh_endprologue

    cbz     x1, .Lretire_dpc_current_stack

    mov     sp, x1
    sub     sp, sp, #16
    bl      KiRetireDpcList
    mov     sp, x29
    b       .Lretire_dpc_done

.Lretire_dpc_current_stack:
    bl      KiRetireDpcList

.Lretire_dpc_done:
    ldp     x29, x30, [sp], #16
    ret
    .seh_endproc

/*
 * KiThreadStartup
 *
 * This is the entry point for all new threads on ARM64. When a thread is first
 * scheduled, KiSwapContext returns to this function (via the Return slot
 * of its KSWITCH_FRAME), after popping that frame.
 *
 * On entry:
 *   - SP points to the KSTART_FRAME
 *   - IRQL is typically HIGH_LEVEL (from idle loop context switch)
 *
 * This function must:
 *   1. Lower IRQL to APC_LEVEL (matching AMD64 behavior)
 *   2. Load startup parameters from KSTART_FRAME
 *   3. Call SystemRoutine(StartRoutine, StartContext)
 *
 * For system threads, SystemRoutine is PspSystemThreadStartup which will
 * further lower IRQL to PASSIVE_LEVEL before calling StartRoutine.
 */
    .seh_proc KiThreadStartup
KiThreadStartup:
    /*
     * SEH: This is a special entry point - no prologue in traditional sense.
     * SP already points to the KSTART_FRAME set up by KiInitializeContextThread.
     */
    .seh_endprologue

    /*
     * ARM64 ABI: keep x18 as the kernel platform register while still in EL1.
     *
     * User-mode TEB state is programmed only in KiTrapReturn immediately
     * before ERET to EL0. Clobbering x18 with KTHREAD.Teb here corrupts the
     * EL1 platform register and can redirect reserved x18-based accesses into
     * the user TEB mapping.
     */
    mrs     x18, tpidr_el1

    /* Clear non-volatile registers to prevent stale data leaking to new thread */
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr

    /* Load startup parameters from KSTART_FRAME (SP points to it).
     * Preserve them in callee-saved registers across KfLowerIrql. */
    ldr     x21, [sp, #SfSystemRoutine]
    ldr     x19, [sp, #SfStartRoutine]
    ldr     x20, [sp, #SfStartContext]

    /* Pop the start frame before invoking the system routine; SP stays aligned */
    add     sp, sp, #KSTART_FRAME_LENGTH

    /*
     * CRITICAL: Lower IRQL to APC_LEVEL before calling the system routine.
     *
     * On AMD64, KiThreadStartup does: mov cr8, APC_LEVEL
     * We must do the equivalent on ARM64 by calling KfLowerIrql.
     *
     * The idle loop switches to us at HIGH_LEVEL (after KfRaiseIrql(HIGH_LEVEL)
     * in KiInitializeKernel). We need to lower to APC_LEVEL so that:
     * 1. The system routine can execute at a safe IRQL
     * 2. PspSystemThreadStartup can further lower to PASSIVE_LEVEL
     * 3. Kernel APCs can be delivered when appropriate
     */
    mov     w0, #APC_LEVEL
    bl      KfLowerIrql

    /* ARM64: x2 = SystemRoutine, x0 = StartRoutine, x1 = StartContext */
    mov     x0, x19
    mov     x1, x20
    mov     x2, x21

    /* Validate SystemRoutine is non-NULL before calling */
    cbz     x21, .Lbad_sysroutine

    /* Start the thread - call SystemRoutine(StartRoutine, StartContext) */
    blr     x2

	    /*
	     * If the system routine returned, this must be a user thread.
	     * Return to user-mode by restoring the prepared trap/exception frames.
	     *
	     * For system threads, returning is a fatal error.
	     */
	    mrs     x16, tpidr_el1
	    cbz     x16, .Lthreadstart_bad_return
	    ldr     x16, [x16, #PcCurrentThread]
	    cbz     x16, .Lthreadstart_bad_return
	    ldr     x0, [x16, #ThTrapFrame]     /* x0 = TrapFrame */
	    cbz     x0, .Lthreadstart_bad_return

	    sub     x1, x0, #KEXCEPTION_FRAME_LENGTH /* x1 = ExceptionFrame (user threads) */

	    b       KiTrapReturn                /* Never returns (ERET) */

.Lthreadstart_bad_return:
	    brk     #0xf000

.Lbad_sysroutine:
    /* x21 was NULL - print error and hang */
    ldr     x9, =0xFFFFFC0009000000
    mov     w10, #'N'
    strb    w10, [x9]
    mov     w10, #'U'
    strb    w10, [x9]
    mov     w10, #'L'
    strb    w10, [x9]
    mov     w10, #'L'
    strb    w10, [x9]
    mov     w10, #'\n'
    strb    w10, [x9]
    brk     #0xf001

    /* Should never return */
    brk     #0xf000
    .seh_endproc


    .extern KiSwapContextResume

/*
 * BOOLEAN
 * KiSwapContext(
 *     _In_ KIRQL WaitIrql,
 *     _Inout_ PKTHREAD CurrentThread);
 */
    .seh_proc KiSwapContext
KiSwapContext:
    /* Reserve the switch frame and save the non-volatile registers */
    sub     sp, sp, #KSWITCH_FRAME_LENGTH
    .seh_stackalloc KSWITCH_FRAME_LENGTH
    stp     d8, d9, [sp, #SwD8]
    .seh_save_fregp d8, SwD8
    stp     d10, d11, [sp, #SwD10]
    .seh_save_fregp d10, SwD10
    stp     d12, d13, [sp, #SwD12]
    .seh_save_fregp d12, SwD12
    stp     d14, d15, [sp, #SwD14]
    .seh_save_fregp d14, SwD14
    stp     x19, x20, [sp, #SwX19]
    .seh_save_regp x19, SwX19
    stp     x21, x22, [sp, #SwX21]
    .seh_save_regp x21, SwX21
    stp     x23, x24, [sp, #SwX23]
    .seh_save_regp x23, SwX23
    stp     x25, x26, [sp, #SwX25]
    .seh_save_regp x25, SwX25
    stp     x27, x28, [sp, #SwX27]
    .seh_save_regp x27, SwX27
    /* Fp and Return (our return address) are adjacent */
    stp     x29, x30, [sp, #SwFp]
    .seh_save_fplr SwFp
    .seh_endprologue

    /*
     * Locate the target (new) thread from the per-CPU PCR. TPIDR_EL1 holds the
     * PCR pointer (set in KiInitializePcr); the global KeArm64CurrentPcr is
     * shared and wrong on SMP.
     */
    mrs     x9, tpidr_el1
    cbz     x9, .Lbad_current_thread
    ldr     x2, [x9, #PcCurrentThread]
    cbz     x2, .Lbad_current_thread

    /* Switches stacks; returns here on the NEW thread's stack */
    bl      KiSwapContextInternal

    /* Restore the new thread's non-volatile register bank */
    ldp     d8, d9, [sp, #SwD8]
    ldp     d10, d11, [sp, #SwD10]
    ldp     d12, d13, [sp, #SwD12]
    ldp     d14, d15, [sp, #SwD14]
    ldp     x19, x20, [sp, #SwX19]
    ldp     x21, x22, [sp, #SwX21]
    ldp     x23, x24, [sp, #SwX23]
    ldp     x25, x26, [sp, #SwX25]
    ldp     x27, x28, [sp, #SwX27]
    ldp     x29, x30, [sp, #SwFp]

    /* x18 is the EL1 platform register (the PCR) across context switches */
    mrs     x18, tpidr_el1

    /*
     * Return to where the new thread called KiSwapContext from, or, for a
     * thread that never ran, to KiThreadStartup with SP at its KSTART_FRAME.
     */
    add     sp, sp, #KSWITCH_FRAME_LENGTH
    ret

.Lbad_current_thread:
    brk     #0xf000
    b       .Lbad_current_thread
    .seh_endproc

/*
 * BOOLEAN
 * KiSwapContextInternal(
 *     _In_ KIRQL WaitIrql,
 *     _Inout_ PKTHREAD OldThread,
 *     _Inout_ PKTHREAD NewThread);
 *
 * This function switches from the old thread's stack to the new thread's stack.
 *
 * IMPORTANT: The caller (KiSwapContext) has already created a KSWITCH_FRAME
 * on the stack with all callee-saved registers and the return address.
 * This function operates directly on that frame - it does NOT create its own.
 *
 * After the stack switch this function returns to KiSwapContext (every
 * thread enters it from the same call site there), which restores the
 * non-volatile registers from the NEW thread's KSWITCH_FRAME and returns
 * through its Return slot.
 */
    .align 4
    .seh_proc KiSwapContextInternal
KiSwapContextInternal:
    /*
     * SEH: This function operates on the caller's stack frame (KSWITCH_FRAME).
     * It doesn't create its own frame - it modifies SP to switch stacks.
     * We declare a minimal frame to allow unwinding.
     */
    .seh_endprologue

    /* The return address into KiSwapContext; x19 is restored from the
       new frame afterwards */
    mov     x19, x30

    /* Wait until the incoming thread clears its SwapBusy bit */
.Lswapbusy_wait:
    ldrb    w3, [x2, #ThSwapBusy]
    cbz     w3, .Lswapbusy_clear
    yield
    wfe
    b       .Lswapbusy_wait

.Lswapbusy_clear:
    /* Record the wait IRQL in the outgoing switch frame */
    strb    w0, [sp, #SwApcBypass]

    /* Publish the outgoing kernel stack */
    mov     x11, sp
    str     x11, [x1, #ThKernelStack]

    /* Switch to the incoming kernel stack */
    ldr     x3, [x2, #ThKernelStack]
    cbz     x3, .Lresume_call      /* Should not happen, but avoid faulting */
    mov     sp, x3

    /* Reload the wait IRQL requested by the target thread */
    ldrb    w0, [sp, #SwApcBypass]

.Lresume_call:
    bl      KiSwapContextResume
    ret     x19
    .seh_endproc
