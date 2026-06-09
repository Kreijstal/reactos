/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     AArch64 implementation of _statusfp
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "fpscr.h"

unsigned int __cdecl _statusfp(void)
{
    unsigned int fpsr = __getfpsr();
    unsigned int flags = 0;

    if (fpsr & ARM64_FPSR_IOC) flags |= _SW_INVALID;
    if (fpsr & ARM64_FPSR_DZC) flags |= _SW_ZERODIVIDE;
    if (fpsr & ARM64_FPSR_OFC) flags |= _SW_OVERFLOW;
    if (fpsr & ARM64_FPSR_UFC) flags |= _SW_UNDERFLOW;
    if (fpsr & ARM64_FPSR_IXC) flags |= _SW_INEXACT;
    if (fpsr & ARM64_FPSR_IDC) flags |= _SW_DENORMAL;

    return flags;
}
