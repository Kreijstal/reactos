
#include <syscalls.inc>

/* This file only has the NT5 syscalls */
#define SYSFUNCS_NT5_ONLY

#ifdef _M_ARM

    TEXTAREA

#define SVC_(name, argcount) STUB_U name
#define SVC_WRAP_(name, argcount) STUB_U name

#include <sysfuncs.h>

    END

#elif defined(_M_ARM64) || defined(_ARM64_) || defined(__aarch64__) || defined(__arm64__)

    .text

#define SVC_(name, argcount) STUB_U name, argcount
#define SVC_WRAP_(name, argcount) STUB_WRAP name, argcount

#include <sysfuncs.h>

#else

.code

#define SVC_(name, argcount) STUB_U name, argcount
#define SVC_WRAP_(name, argcount) STUB_U name, argcount

#ifdef BUILD_WOW6432
/*
 * A WoW64 guest's syscall numbers MUST match the 64-bit host's table.  The
 * host is built for a newer NT version than the 32-bit guest, so force the
 * host's (newest) NTDDI gating here.  Otherwise version-gated syscalls (e.g.
 * CreateUserProcess) are absent from the guest, shifting every later number -
 * including the appended Wow64* services - out of sync with the host's
 * dll/wow64/sysfuncnum.h dispatch.
 */
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif

#include <sysfuncs.h>

#ifdef BUILD_WOW6432
/*
 * WoW64-only cross-bitness memory syscalls.  The 32-bit guest issues these so
 * the 64-bit host (Wow64SystemServiceEx) can reach native NtReadVirtualMemory
 * with a full 64-bit address.  They MUST follow the standard table in the same
 * order as the dll/wow64/sysfuncnum.h enum so the service numbers match.
 */
    STUB_U Wow64AllocateVirtualMemory64, 7
    STUB_U Wow64ReadVirtualMemory64, 7
    STUB_U Wow64WriteVirtualMemory64, 7
#endif

#ifndef _M_ARM64
    END
#endif
#endif
