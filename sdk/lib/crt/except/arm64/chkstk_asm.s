/*
 * ARM64 ABI: x15 holds the allocation size in 16-byte units. Touch every
 * page between the current SP and the new SP so the guard-page mechanism
 * can grow the stack; only x16/x17 may be clobbered.
 */
    .text
    .align 2
    .global __chkstk
__chkstk:
    lsl     x16, x15, #4
    mov     x17, sp
1:
    sub     x17, x17, #4096
    subs    x16, x16, #4096
    ldr     xzr, [x17]
    b.gt    1b
    ret

    .align 2
    .global __alloca_probe
__alloca_probe:
    b       __chkstk

    .section .drectve,"yn"
    .ascii " -export:__chkstk"
    .ascii " -export:__alloca_probe"
