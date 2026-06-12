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

    lea ecx, [return32]
    mov [rsp + 16], ecx

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

/*
 * KiWow64SystemCall32 - 32-bit -> 64-bit system-call thunk ("heaven's gate").
 *
 * The 32-bit guest ntdll syscall stub (built with BUILD_WOW6432) does:
 *     mov eax, ServiceNumber
 *     lea edx, [esp+4]            ; edx -> argument array
 *     call dword ptr fs:[0C0h]    ; fs:[0C0h] = TEB32->WOW32Reserved = this thunk
 *     ret  N
 * On entry we are in compat mode (CS=0x23) with eax=ServiceNumber, edx=pArgs and
 * the guest near-call return address on top of the 32-bit stack.  We far-jump up
 * into 64-bit mode, dispatch through Wow64SystemServiceEx(ServiceNumber, pArgs),
 * then far-return back to compat mode and near-ret to the guest with the NTSTATUS
 * still in eax.  wow64.dll is mapped below 4 GB, so the 64-bit landing pad and the
 * 32-bit re-entry stub are reachable through 32-bit far-return offsets.
 */
EXTERN Wow64SystemServiceEx:PROC

PUBLIC KiWow64SystemCall32
KiWow64SystemCall32:
    /* 32-bit (CS=0x23): eax=ServiceNumber, edx=pArgs, [esp]=guest return addr */
    .byte HEX(6A), HEX(33)              /* push 0x33  (long-mode code selector) */
    .byte HEX(0E8), 0, 0, 0, 0          /* call $+5 -> push EIP of next insn */
KiWow64ScFixup:
    .byte HEX(81), HEX(04), HEX(24)     /* add dword ptr [esp], (land64 - here) */
    .long KiWow64ScLand64 - KiWow64ScFixup
    .byte HEX(0CB)                      /* retf -> CS=0x33, RIP=KiWow64ScLand64 */
KiWow64ScLand64:
    /* 64-bit (CS=0x33): rax=ServiceNumber, rdx=pArgs, rsp=&(guest return addr) */
    mov r10, rsp                        /* original 32-bit stack pointer (scratch) */
    mov ecx, eax                        /* rcx = ServiceNumber (zero-extended) */
    and rsp, -16                        /* 16-byte align for the x64 ABI */
    sub rsp, 48                         /* 32 home/shadow + 16 for our own save slot */
    mov [rsp + 40], r10                 /* spill the 32-bit rsp ABOVE the shadow area;
                                           a volatile reg would be clobbered by the call
                                           (and by the syscall inside it, which uses r10) */
    call Wow64SystemServiceEx           /* eax = NTSTATUS */
    mov rsp, [rsp + 40]                 /* restore the 32-bit stack pointer */
    lea r11, [rip + KiWow64ScBack32]
    sub rsp, 8                          /* scratch far-return frame below guest ret */
    mov dword ptr [rsp], r11d           /* offset: KiWow64ScBack32 (below 4 GB) */
    mov dword ptr [rsp + 4], HEX(23)    /* selector: compat-mode code (CS=0x23) */
    .byte HEX(0CB)                      /* retf -> CS=0x23, EIP=KiWow64ScBack32 */
KiWow64ScBack32:
    /* 32-bit (CS=0x23): eax=NTSTATUS, [esp]=guest return addr */
    .byte HEX(0C3)                      /* ret -> back to the 32-bit caller */

END
