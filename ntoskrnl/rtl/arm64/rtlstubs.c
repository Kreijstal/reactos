/*
 * PROJECT:     ReactOS Kernel ARM64
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Kernel-mode helper stubs for the ARM64 RTL
 *
 * The PE unwinder (RtlVirtualUnwind, RtlUnwindEx, RtlUnwind,
 * RtlLookupFunctionEntry, RtlRestoreContext) is shared with user mode and
 * lives in sdk/lib/rtl/arm64/unwind.c; the kernel-specific piece is
 * RtlPcToFileHeader (ntoskrnl/rtl/libsupp.c), which resolves code addresses
 * against PsLoadedModuleList.
 */

#include <ntoskrnl.h>

/* i386 pseh compatibility symbols referenced by shared code */
void _local_unwind2(void) {}
void _global_unwind2(void) {}
int _except_handler2(void) { return 1; }
int _except_handler3(void) { return 1; }

__asm__(
    ".text\n"
    ".globl _abnormal_termination\n"
    ".def _abnormal_termination; .scl 2; .type 32; .endef\n"
    "_abnormal_termination:\n"
    "    mov w0, wzr\n"
    "    ret\n");
