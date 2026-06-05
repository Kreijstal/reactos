/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 implementation of _clearfp
 */

#include "fpcr.h"

unsigned int _clearfp(void)
{
    unsigned int status = _statusfp();
    __setfpsr(__getfpsr() & ~ARM64_FP_EXCEPTION_MASK);
    return status;
}
