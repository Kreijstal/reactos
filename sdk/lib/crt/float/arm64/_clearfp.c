/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     AArch64 implementation of _clearfp
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "fpscr.h"

unsigned int __cdecl _clearfp(void)
{
    unsigned int status;

    /* Capture the abstract status before clearing */
    status = _statusfp();

    /* Clear the cumulative exception flags in FPSR */
    __setfpsr(__getfpsr() & ~ARM64_FPSR_EXC_MASK);

    return status;
}
