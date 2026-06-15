
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

/*
 * A WoW64 guest's syscall numbers MUST match the 64-bit host's KiServiceTable
 * and wow64.dll's Wow64SystemServiceEx() dispatch table.  All three are
 * generated from sysfuncs.h, and the guest (ARCH=i386 SARCH=wow64) and host
 * (ARCH=amd64) are configured with the same project NTDDI_VERSION, so they
 * already agree -- do NOT override NTDDI_VERSION here.  Forcing a different
 * (newer) value adds version-gated syscalls absent from the host table,
 * shifting every later number out of sync and, in particular, dispatching the
 * appended Wow64* services (below) to the wrong handler.
 */
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
