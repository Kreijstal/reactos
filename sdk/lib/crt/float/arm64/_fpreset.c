/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     AArch64 implementation of _fpreset
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "fpscr.h"

void __cdecl _fpreset(void)
{
    /* Default environment: all exceptions masked (no trap-enable bits),
       round to nearest, no flush-to-zero, and a cleared status register. */
    __setfpcr(0);
    __setfpsr(0);
}
