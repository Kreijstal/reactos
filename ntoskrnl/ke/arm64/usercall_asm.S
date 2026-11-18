#include <asm.h>
#include <ksarm64.h>

/*
 * The kernel callout frame: a KEXCEPTION_FRAME (WoA layout) followed by the
 * state the callout must restore that the exception frame has no slot for.
 * Keep in sync with KI_CALLOUT_FRAME in internal/arm64/ke.h.
 */
#define CoInitialStack          (KEXCEPTION_FRAME_LENGTH + 0x00)
#define CoCallbackStack         (KEXCEPTION_FRAME_LENGTH + 0x08)
#define CoFpcr                  (KEXCEPTION_FRAME_LENGTH + 0x10)
#define CoFpsr                  (KEXCEPTION_FRAME_LENGTH + 0x18)
#define CALLOUT_FRAME_LENGTH    (KEXCEPTION_FRAME_LENGTH + 0x20)

    .text
    .align 4

    .extern KiUserModeCallout

    .global KiCallUserMode
    .seh_proc KiCallUserMode
KiCallUserMode:
    sub     sp, sp, #CALLOUT_FRAME_LENGTH
    .seh_stackalloc CALLOUT_FRAME_LENGTH
    stp     d8, d9, [sp, #ExD8]
    .seh_save_fregp d8, ExD8
    stp     d10, d11, [sp, #ExD10]
    .seh_save_fregp d10, ExD10
    stp     d12, d13, [sp, #ExD12]
    .seh_save_fregp d12, ExD12
    stp     d14, d15, [sp, #ExD14]
    .seh_save_fregp d14, ExD14
    stp     x19, x20, [sp, #ExX19]
    .seh_save_regp x19, ExX19
    stp     x21, x22, [sp, #ExX21]
    .seh_save_regp x21, ExX21
    stp     x23, x24, [sp, #ExX23]
    .seh_save_regp x23, ExX23
    stp     x25, x26, [sp, #ExX25]
    .seh_save_regp x25, ExX25
    stp     x27, x28, [sp, #ExX27]
    .seh_save_regp x27, ExX27
    /* Fp and Return (the link register) are adjacent */
    stp     x29, x30, [sp, #ExFp]
    .seh_save_fplr ExFp
    .seh_endprologue

    mrs     x9, fpcr
    str     x9, [sp, #CoFpcr]
    mrs     x9, fpsr
    str     x9, [sp, #CoFpsr]

    str     x0, [sp, #ExOutputBuffer]
    str     x1, [sp, #ExOutputLength]

    mov     x0, sp
    bl      KiUserModeCallout

    ldr     x9, [sp, #CoFpcr]
    msr     fpcr, x9
    ldr     x9, [sp, #CoFpsr]
    msr     fpsr, x9

    ldp     d8, d9, [sp, #ExD8]
    ldp     d10, d11, [sp, #ExD10]
    ldp     d12, d13, [sp, #ExD12]
    ldp     d14, d15, [sp, #ExD14]
    ldp     x19, x20, [sp, #ExX19]
    ldp     x21, x22, [sp, #ExX21]
    ldp     x23, x24, [sp, #ExX23]
    ldp     x25, x26, [sp, #ExX25]
    ldp     x27, x28, [sp, #ExX27]
    ldp     x29, x30, [sp, #ExFp]
    add     sp, sp, #CALLOUT_FRAME_LENGTH
    ret
    .seh_endproc

    .global KiCallbackReturn
    .seh_proc KiCallbackReturn
KiCallbackReturn:
    /* SEH: This is a non-local return - restores frame from x0 */
    .seh_endprologue
    mov     sp, x0
    mov     x9, x1

    ldr     x10, [sp, #CoFpcr]
    msr     fpcr, x10
    ldr     x10, [sp, #CoFpsr]
    msr     fpsr, x10

    ldp     d8, d9, [sp, #ExD8]
    ldp     d10, d11, [sp, #ExD10]
    ldp     d12, d13, [sp, #ExD12]
    ldp     d14, d15, [sp, #ExD14]
    ldp     x19, x20, [sp, #ExX19]
    ldp     x21, x22, [sp, #ExX21]
    ldp     x23, x24, [sp, #ExX23]
    ldp     x25, x26, [sp, #ExX25]
    ldp     x27, x28, [sp, #ExX27]
    ldp     x29, x30, [sp, #ExFp]

    add     sp, sp, #CALLOUT_FRAME_LENGTH

    mov     x0, x9
    ret
    .seh_endproc
