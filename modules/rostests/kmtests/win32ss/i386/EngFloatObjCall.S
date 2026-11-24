/*
 * PROJECT:     ReactOS Kernel-Mode Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Call a two-argument stdcall function and report whether it
 *              preserved the caller's frame pointer
 */

#include <asm.inc>

.code

/*
 * BOOLEAN
 * NTAPI
 * KmtCallPreservesFramePointer(PVOID Function, PVOID Argument1, ULONG Argument2);
 *
 * EBP is a callee-saved register. The call is made with a known EBP and a
 * private copy of it in ESI, so a callee that returns with another EBP is
 * reported instead of crashing the code that called us.
 */
PUBLIC _KmtCallPreservesFramePointer@12
_KmtCallPreservesFramePointer@12:
    push ebp
    push esi
    mov esi, esp
    mov ebp, esp

    push dword ptr [esi + 20]   /* Argument2 */
    push dword ptr [esi + 16]   /* Argument1 */
    call dword ptr [esi + 12]   /* Function */

    xor eax, eax
    cmp ebp, esi
    sete al

    mov esp, esi
    pop esi
    pop ebp
    ret 12

END
