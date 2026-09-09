/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fast AMD64 memcpy and memmove
 */

#include <asm.inc>

.code64

PUBLIC memcpy
PUBLIC memmove
memcpy:
FUNC memmove
    push rdi
    .PUSHREG rdi
    push rsi
    .PUSHREG rsi
    .ENDPROLOG

    mov rax, rcx
    mov rdi, rcx
    mov rsi, rdx
    mov rcx, r8

    /* Copy forwards unless destination starts inside the source range. */
    cmp rdi, rsi
    jbe .CopyForward
    mov rdx, rdi
    sub rdx, rsi
    cmp rdx, rcx
    jae .CopyForward

    /* REP with DF set starts at the final byte. Always restore DF for ABI. */
    test rcx, rcx
    jz .Done
    add rdi, rcx
    add rsi, rcx
    dec rdi
    dec rsi
    std
    rep movsb
    cld
    jmp .Done

.CopyForward:
    cld
    rep movsb

.Done:
    pop rsi
    pop rdi
    ret
ENDFUNC

END
