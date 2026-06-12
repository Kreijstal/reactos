#pragma once

/*
 * WoW64 32-bit -> 64-bit system-call dispatch numbers.
 *
 * These MUST match the service numbers the 32-bit guest issues (eax) and the
 * order of the Wow64SystemServiceEx() dispatch table / mapping[] array.  Both
 * are generated from ntoskrnl/include/sysfuncs.h, so derive the Num<Name>
 * constants from the very same list instead of a hand-maintained parallel copy
 * (which drifts out of sync as NTDDI-gated syscalls are added/removed, shifting
 * every subsequent number and breaking dispatch).
 */
enum _WOW64_SYSCALL_NUMBER
{
#define SVC_(name, argc) Num##name,
#define SVC_WRAP_(name, argc) Num##name,
#include "../../../ntoskrnl/include/sysfuncs.h"
#undef SVC_WRAP_
#undef SVC_

    /* WoW64-only services the 32-bit ntdll appends after the standard table
     * (NtWow64* cross-bitness memory access).  Keep them in this order. */
    NumWow64AllocateVirtualMemory64,
    NumWow64ReadVirtualMemory64,
    NumWow64WriteVirtualMemory64,
};
