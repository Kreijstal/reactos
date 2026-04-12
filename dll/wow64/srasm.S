#include <asm.inc>

.code64

PUBLIC __readfsdword
__readfsdword:
    mov eax, fs:[rcx]
    ret

PUBLIC EnterApc32
EnterApc32:
    push r12
    mov r12, rsp

    sub rsp, rdx

    push rdi
    push rsi

    mov rsi, rcx
    mov rcx, rdx
    lea rdi, [rsp + 16]
    cld
    rep movsb

    pop rsi
    pop rdi

    lea rcx, [return32]
    mov [rsp + 16], rcx

    .byte HEX(48)
    retf
return32:
    call l
l:
    pop rax
    add eax, return64 - l
    push HEX(33)
    push rax
    retf
return64:
    mov rsp, r12
    pop r12
    ret

PUBLIC SetupFs
SetupFs:
    mov fs, rcx
    ret

END
