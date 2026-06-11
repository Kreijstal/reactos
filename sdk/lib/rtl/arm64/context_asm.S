/*
 * PROJECT:     ReactOS Run-Time Library
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     ARM64 context capture
 */

#include <asm.h>
#include <ksarm64.h>

    .text
    .align 4

    .global RtlCaptureContext
    .seh_proc RtlCaptureContext
RtlCaptureContext:
    .seh_endprologue

    /*
     * Capture the caller-visible register state without calling back into C.
     * x0 is both the incoming argument and the value that belongs in Context.X0.
     * Save x9-x11 before using them as scratch so the context keeps the true
     * volatile register values seen by this routine's caller.
     */
    str     x9,  [x0, #CxX9]
    str     x10, [x0, #CxX10]
    str     x11, [x0, #CxX11]

    mov     x9, x0

    stp     x0,  x1,  [x9, #CxX0]
    stp     x2,  x3,  [x9, #CxX2]
    stp     x4,  x5,  [x9, #CxX4]
    stp     x6,  x7,  [x9, #CxX6]
    str     x8,        [x9, #CxX8]
    stp     x12, x13, [x9, #CxX12]
    stp     x14, x15, [x9, #CxX14]
    stp     x16, x17, [x9, #CxX16]
    stp     x18, x19, [x9, #CxX18]
    stp     x20, x21, [x9, #CxX20]
    stp     x22, x23, [x9, #CxX22]
    stp     x24, x25, [x9, #CxX24]
    stp     x26, x27, [x9, #CxX26]
    stp     x28, x29, [x9, #CxX28]
    str     x30,       [x9, #CxLr]

    mov     x10, sp
    str     x10, [x9, #CxSp]
    str     x30, [x9, #CxPc]

    mov     w10, #0x7
    movk    w10, #0x40, lsl #16
    str     w10, [x9, #CxContextFlags]

    mrs     x10, nzcv
    str     w10, [x9, #CxCpsr]

    /*
     * Capture the full SIMD register file. RtlUnwindEx restores the captured
     * context at the unwind target, so the callee-saved d8-d15 (and the rest
     * of the V file) must hold their live values, not zeros.
     */
    add     x10, x9, #CxV
    stp     q0,  q1,  [x10, #0x000]
    stp     q2,  q3,  [x10, #0x020]
    stp     q4,  q5,  [x10, #0x040]
    stp     q6,  q7,  [x10, #0x060]
    stp     q8,  q9,  [x10, #0x080]
    stp     q10, q11, [x10, #0x0a0]
    stp     q12, q13, [x10, #0x0c0]
    stp     q14, q15, [x10, #0x0e0]
    stp     q16, q17, [x10, #0x100]
    stp     q18, q19, [x10, #0x120]
    stp     q20, q21, [x10, #0x140]
    stp     q22, q23, [x10, #0x160]
    stp     q24, q25, [x10, #0x180]
    stp     q26, q27, [x10, #0x1a0]
    stp     q28, q29, [x10, #0x1c0]
    stp     q30, q31, [x10, #0x1e0]

    mrs     x10, fpcr
    str     w10, [x9, #CxFpcr]
    mrs     x10, fpsr
    str     w10, [x9, #CxFpsr]

    ret
    .seh_endproc
