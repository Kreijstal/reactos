/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 implementation of _statusfp
 */

#include "fpcr.h"

unsigned int _statusfp(void)
{
    unsigned int fpsr = __getfpsr();
    unsigned int flags = 0;

    if (fpsr & (1u << 0)) flags |= _SW_INVALID;
    if (fpsr & (1u << 1)) flags |= _SW_ZERODIVIDE;
    if (fpsr & (1u << 2)) flags |= _SW_OVERFLOW;
    if (fpsr & (1u << 3)) flags |= _SW_UNDERFLOW;
    if (fpsr & (1u << 4)) flags |= _SW_INEXACT;
    if (fpsr & (1u << 7)) flags |= _SW_DENORMAL;

    return flags;
}
