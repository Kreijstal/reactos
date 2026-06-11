/*
 * PROJECT:     ReactOS Run-Time Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ARM64 exception dispatch helpers
 * NOTES:       Modeled on Wine's dlls/ntdll/signal_arm64.c helpers.
 */

#include <asm.h>
#include <ksarm64.h>

    .text
    .align 4

    /*
     * EXCEPTION_DISPOSITION
     * RtlpCallSehHandler(
     *     x0 = PEXCEPTION_RECORD ExceptionRecord,
     *     x1 = ULONG_PTR EstablisherFrame,
     *     x2 = PCONTEXT ContextRecord,
     *     x3 = PVOID DispatcherContext,
     *     x4 = PEXCEPTION_ROUTINE ExceptionRoutine);
     *
     * Calls an exception handler, protected by RtlpNestedExceptionHandler
     * so that a new exception inside the handler reports as nested.
     */
    .global RtlpCallSehHandler
    .seh_proc RtlpCallSehHandler
RtlpCallSehHandler:
    stp x29, x30, [sp, #-16]!
    .seh_save_fplr_x 16
    .seh_endprologue
    .seh_handler RtlpNestedExceptionHandler, @except
    blr x4
    ldp x29, x30, [sp], #16
    ret
    .seh_endproc

    /*
     * EXCEPTION_DISPOSITION
     * RtlpCallUnwindHandler(
     *     x0 = PEXCEPTION_RECORD ExceptionRecord,
     *     x1 = ULONG_PTR EstablisherFrame,
     *     x2 = PCONTEXT ContextRecord,
     *     x3 = PVOID DispatcherContext,
     *     x4 = PEXCEPTION_ROUTINE ExceptionRoutine);
     *
     * Calls an unwind handler. The dispatcher context is saved at a known
     * position relative to this frame's establisher frame (frame[-2]) so
     * RtlpUnwindHandler can recover it on a collided unwind.
     */
    .global RtlpCallUnwindHandler
    .seh_proc RtlpCallUnwindHandler
RtlpCallUnwindHandler:
    stp x29, x30, [sp, #-32]!
    .seh_save_fplr_x 32
    .seh_endprologue
    .seh_handler RtlpUnwindHandler, @except
    str x3, [sp, #16]           /* frame[-2] = DispatcherContext */
    blr x4
    ldp x29, x30, [sp], #32
    ret
    .seh_endproc

    /*
     * LONG
     * __C_ExecuteExceptionFilter(
     *     x0 = PEXCEPTION_POINTERS ExceptionPointers (or TRUE/FALSE for __finally),
     *     x1 = PVOID EstablisherFrame,
     *     x2 = PVOID Filter funclet,
     *     x3 = PUCHAR NonVolatileRegisters);
     *
     * Calls an SEH filter or termination funclet with the parent function's
     * non-volatile registers (x19-x28) restored, and the parent's frame
     * pointer in x1 per the ARM64 funclet calling convention.
     */
    .global __C_ExecuteExceptionFilter
    .seh_proc __C_ExecuteExceptionFilter
__C_ExecuteExceptionFilter:
    stp x29, x30, [sp, #-96]!
    .seh_save_fplr_x 96
    stp x19, x20, [sp, #16]
    .seh_save_regp x19, 16
    stp x21, x22, [sp, #32]
    .seh_save_regp x21, 32
    stp x23, x24, [sp, #48]
    .seh_save_regp x23, 48
    stp x25, x26, [sp, #64]
    .seh_save_regp x25, 64
    stp x27, x28, [sp, #80]
    .seh_save_regp x27, 80
    .seh_endprologue
    ldp x19, x20, [x3, #0]      /* non-volatile regs of the parent frame */
    ldp x21, x22, [x3, #16]
    ldp x23, x24, [x3, #32]
    ldp x25, x26, [x3, #48]
    ldp x27, x28, [x3, #64]
    ldr x1, [x3, #80]           /* x1 = parent x29 (frame pointer) */
    blr x2
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #96
    ret
    .seh_endproc

    /*
     * VOID
     * RtlpRestoreContextInternal(
     *     x0 = PCONTEXT Context);
     *
     * Restores the full register state from a CONTEXT and transfers control
     * to Context->Pc. Does not return. x16/x17 are clobbered (they are
     * intra-procedure-call scratch registers, legal at a control transfer).
     */
    .global RtlpRestoreContextInternal
    .seh_proc RtlpRestoreContextInternal
RtlpRestoreContextInternal:
    .seh_endprologue
    ldr w1, [x0, #CxCpsr]
    msr nzcv, x1
    ldr w1, [x0, #CxFpcr]
    msr fpcr, x1
    ldr w1, [x0, #CxFpsr]
    msr fpsr, x1

    add x1, x0, #CxV
    ldp q0,  q1,  [x1, #0x000]
    ldp q2,  q3,  [x1, #0x020]
    ldp q4,  q5,  [x1, #0x040]
    ldp q6,  q7,  [x1, #0x060]
    ldp q8,  q9,  [x1, #0x080]
    ldp q10, q11, [x1, #0x0a0]
    ldp q12, q13, [x1, #0x0c0]
    ldp q14, q15, [x1, #0x0e0]
    ldp q16, q17, [x1, #0x100]
    ldp q18, q19, [x1, #0x120]
    ldp q20, q21, [x1, #0x140]
    ldp q22, q23, [x1, #0x160]
    ldp q24, q25, [x1, #0x180]
    ldp q26, q27, [x1, #0x1a0]
    ldp q28, q29, [x1, #0x1c0]
    ldp q30, q31, [x1, #0x1e0]

    ldr x1, [x0, #CxSp]
    mov sp, x1
    ldr x17, [x0, #CxPc]

    ldp x2,  x3,  [x0, #CxX2]
    ldp x4,  x5,  [x0, #CxX4]
    ldp x6,  x7,  [x0, #CxX6]
    ldp x8,  x9,  [x0, #CxX8]
    ldp x10, x11, [x0, #CxX10]
    ldp x12, x13, [x0, #CxX12]
    ldp x14, x15, [x0, #CxX14]
    ldr x18,      [x0, #CxX18]
    ldp x19, x20, [x0, #CxX19]
    ldp x21, x22, [x0, #CxX21]
    ldp x23, x24, [x0, #CxX23]
    ldp x25, x26, [x0, #CxX25]
    ldp x27, x28, [x0, #CxX27]
    ldp x29, x30, [x0, #CxFp]
    ldp x0,  x1,  [x0, #CxX0]
    br x17
    .seh_endproc
