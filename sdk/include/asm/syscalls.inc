
#ifdef _M_ARM

#include <ksarm.h>

    GBLA SyscallId

SyscallId SETA 0

	MACRO
	STUB_K $Name
	LCLS ZwFuncName
	LCLS ZwFuncEndName
ZwFuncName    SETS "Zw$Name"
ZwFuncEndName SETS "$ZwFuncName":CC:"_end"
	ALIGN 2
	EXPORT $ZwFuncName [FUNC]
$ZwFuncName
	ROUT
    mov r12, #SyscallId
    svc #1
    bx lr
$ZwFuncEndName
    MEND

	MACRO
	STUB_U $Name
	LCLS NtFuncName
	LCLS NtFuncEndName
	LCLS ZwFuncName
	LCLS ZwFuncEndName
NtFuncName    SETS "Nt$Name"
NtFuncEndName SETS "$NtFuncName":CC:"_end"
ZwFuncName    SETS "Zw$Name"
ZwFuncEndName SETS "$ZwFuncName":CC:"_end"
	ALIGN 2
	EXPORT $NtFuncName [FUNC]
$NtFuncName
	EXPORT $ZwFuncName [FUNC]
$ZwFuncName
	ROUT
    mov r12, #SyscallId
    svc #1
    bx lr
$NtFuncEndName
$ZwFuncEndName
    MEND

#else

#include <asm.inc>

SyscallId = 0

#ifdef _M_IX86
#ifdef BUILD_WOW6432
#define KUSER_SHARED_SYSCALL HEX(7ffe0300)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
LOCAL x, x2, x3
    StackBytes = 4 * ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    push ecx
    push edx

    mov ecx, SyscallId
    mov edx, ArgCount

    push ebp
    mov ebp, esp

    /* Align the stack, and allocate shadow space for the x64 calling convention */
    sub esp, 48
    and esp, HEX(FFFFFFF0)

    push 0
    push ebp

    ljmp HEX(33), x
x:
    /* pop rdx (64-bit opcode) */
    .byte HEX(5a)

    /* add rdx, 16 (REX.W prefix + add r/m64, imm8) */
    .byte HEX(48)
    .byte HEX(83)
    .byte HEX(c2)
    .byte HEX(10)

    call x3

    push HEX(23)
    /* We need to push the address of x2 in 64-bit mode.
       "push x2" emits ff 35 [abs32], which in 64-bit mode becomes
       push [RIP+disp32] (reads from garbage address).
       Use call/pop trick to get RIP, then adjust. */
    .byte HEX(e8)          /* call rel32 (next instruction) */
    .long 0
    /* [rsp] = addr of next byte. x2 is 7 bytes ahead (5 for add + 2 for retfq) */
    .byte HEX(48), HEX(83), HEX(04), HEX(24), 7  /* add qword [rsp], 7 */
    .byte HEX(48)
    retf
x2:
    mov esp, ebp
    pop ebp

    pop edx
    pop ecx

    ret StackBytes
x3:
    /* mov eax, fs:[0xC0] ; push rax ; ret */
    .long HEX(25048b64)
    .long HEX(000000c0)
    .byte HEX(50)
    .byte HEX(c3)
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    int 3
ENDM
#else
#if (defined(NTDDI_VERSION) && (NTDDI_VERSION >= NTDDI_WIN8)) || \
    (defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0602))
#define KUSER_SHARED_SYSCALL HEX(7ffe0308)
#else
#define KUSER_SHARED_SYSCALL HEX(7ffe0300)
#endif
#define KGDT_R0_CODE 8
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    StackBytes = 4 * ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    mov eax, SyscallId
    mov edx, KUSER_SHARED_SYSCALL
    call dword ptr [edx]
    ret StackBytes
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    StackBytes = 4 * &ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    mov eax, SyscallId
    lea edx, [esp + 4]
    pushfd
    push KGDT_R0_CODE
    call _KiSystemService
    ret StackBytes
ENDM
#endif
#elif defined(_M_AMD64)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    .ENDPROLOG
    mov eax, SyscallId
    mov r10, rcx
    syscall
    ret
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    .ENDPROLOG
    EXTERN Nt&Name:PROC
    lea rax, Nt&Name[rip]
    mov r10, ArgCount * 8
    jmp KiZwSystemService
ENDM
#elif defined(_M_PPC)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    stwu 1,-16(1)
    mflr 0
    stw  0,0(1)
    li   0, SyscallId
    sc
    lwz 0,0(1)
    mtlr 0
    addi 1,1,16
    blr
ENDM
#define STUBCODE_K STUBCODE_U
#elif defined(_M_MIPS)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    li $8, KUSER_SHARED_SYSCALL
    lw $8,0($8)
    j $8
    nop
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    j KiSystemService
    nop
ENDM
#else
#error unsupported architecture
#endif

#ifdef _M_IX86
MACRO(MAKE_LABEL, Name, StackBytes)
    PUBLIC _&Name&@&StackBytes
    _&Name&@&StackBytes:
ENDM
MACRO(START_PROC, Name, StackBytes)
    PUBLIC _&Name&@&StackBytes
    .PROC _&Name&@&StackBytes
ENDM
#else
MACRO(MAKE_LABEL, Name, StackBytes)
    PUBLIC &Name
    &Name:
ENDM
MACRO(START_PROC, Name, StackBytes)
    PUBLIC &Name
    .PROC &Name
ENDM
#endif

MACRO(STUB_U, Name, ArgCount)
    MAKE_LABEL Zw&Name, %ArgCount * 4
    START_PROC Nt&Name, %ArgCount * 4
    STUBCODE_U Name, SyscallId, %ArgCount
    .ENDP
    SyscallId = SyscallId + 1
ENDM

MACRO(STUB_K, Name, ArgCount)
    START_PROC Zw&Name, %ArgCount * 4
    STUBCODE_K Name, SyscallId, %ArgCount
    .ENDP
    SyscallId = SyscallId + 1
ENDM

#endif
