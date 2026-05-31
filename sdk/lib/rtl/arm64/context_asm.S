/*
 * PROJECT:     ReactOS Run-Time Library
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     ARM64 context capture
 */

#include <asm.h>
#include <ksarm64.h>

#ifndef CxContextFlags
#define CxContextFlags 0
#define CxCpsr 4
#define CxX0 8
#define CxX2 24
#define CxX4 40
#define CxX6 56
#define CxX8 72
#define CxX9 80
#define CxX10 88
#define CxX11 96
#define CxX12 104
#define CxX14 120
#define CxX16 136
#define CxX18 152
#define CxX20 168
#define CxX22 184
#define CxX24 200
#define CxX26 216
#define CxX28 232
#define CxLr 248
#define CxSp 256
#define CxPc 264
#define CxV 272
#define CxFpcr 784
#define CxFpsr 788
#define CONTEXT_FRAME_LENGTH 912
#endif

    .text
    .align 4

    .macro STR_X reg, base, offset
    ldr     x12, =\offset
    str     \reg, [\base, x12]
    .endm

    .macro STR_W reg, base, offset
    ldr     x12, =\offset
    str     \reg, [\base, x12]
    .endm

    .macro STP_X reg1, reg2, base, offset
    ldr     x12, =\offset
    add     x12, \base, x12
    stp     \reg1, \reg2, [x12]
    .endm

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
    sub     sp, sp, #32
    stp     x12, x13, [sp]
    stp     x14, x15, [sp, #16]

    STR_X   x9,  x0, CxX9
    STR_X   x10, x0, CxX10
    STR_X   x11, x0, CxX11

    mov     x9, x0

    STP_X   x0,  x1,  x9, CxX0
    STP_X   x2,  x3,  x9, CxX2
    STP_X   x4,  x5,  x9, CxX4
    STP_X   x6,  x7,  x9, CxX6
    STR_X   x8,        x9, CxX8

    ldp     x13, x14, [sp]
    STP_X   x13, x14, x9, CxX12
    ldp     x13, x14, [sp, #16]
    STP_X   x13, x14, x9, CxX14

    STP_X   x16, x17, x9, CxX16
    STP_X   x18, x19, x9, CxX18
    STP_X   x20, x21, x9, CxX20
    STP_X   x22, x23, x9, CxX22
    STP_X   x24, x25, x9, CxX24
    STP_X   x26, x27, x9, CxX26
    STP_X   x28, x29, x9, CxX28
    STR_X   x30,       x9, CxLr

    add     x10, sp, #32
    STR_X   x10, x9, CxSp
    STR_X   x30, x9, CxPc

    mov     w10, #0x7
    movk    w10, #0x40, lsl #16
    STR_W   w10, x9, CxContextFlags

    mrs     x10, nzcv
    STR_W   w10, x9, CxCpsr

    /*
     * Clear FP/vector and debug state so callers do not see stale stack data
     * in portions of CONTEXT that are not captured here.
     */
    ldr     x12, =CxV
    add     x10, x9, x12
    mov     x11, #(CONTEXT_FRAME_LENGTH - CxV)
1:
    stp     xzr, xzr, [x10], #16
    subs    x11, x11, #16
    b.ne    1b

    mrs     x10, fpcr
    STR_W   w10, x9, CxFpcr
    mrs     x10, fpsr
    STR_W   w10, x9, CxFpsr

    add     sp, sp, #32
    ret
    .seh_endproc
