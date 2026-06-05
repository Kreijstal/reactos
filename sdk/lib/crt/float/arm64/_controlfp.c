/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 implementation of _controlfp
 */

#include <precomp.h>
#include "fpcr.h"

unsigned int CDECL _controlfp(unsigned int newval, unsigned int mask)
{
    return _control87(newval, mask & ~_EM_DENORMAL);
}

unsigned int CDECL _control87(unsigned int newval, unsigned int mask)
{
    unsigned int fpcr = __getfpcr();
    unsigned int flags = 0;

    if (!(fpcr & ARM64_FPCR_IOE)) flags |= _EM_INVALID;
    if (!(fpcr & ARM64_FPCR_DZE)) flags |= _EM_ZERODIVIDE;
    if (!(fpcr & ARM64_FPCR_OFE)) flags |= _EM_OVERFLOW;
    if (!(fpcr & ARM64_FPCR_UFE)) flags |= _EM_UNDERFLOW;
    if (!(fpcr & ARM64_FPCR_IXE)) flags |= _EM_INEXACT;
    if (!(fpcr & ARM64_FPCR_IDE)) flags |= _EM_DENORMAL;

    switch ((fpcr & ARM64_FPCR_RMODE_MASK) >> ARM64_FPCR_RMODE_SHIFT)
    {
        case 1: flags |= _RC_DOWN; break;
        case 2: flags |= _RC_UP; break;
        case 3: flags |= _RC_CHOP; break;
        default: flags |= _RC_NEAR; break;
    }

    flags = (flags & ~mask) | (newval & mask);

    fpcr &= ~(ARM64_FPCR_IOE |
              ARM64_FPCR_DZE |
              ARM64_FPCR_OFE |
              ARM64_FPCR_UFE |
              ARM64_FPCR_IXE |
              ARM64_FPCR_IDE |
              ARM64_FPCR_RMODE_MASK);

    if (!(flags & _EM_INVALID)) fpcr |= ARM64_FPCR_IOE;
    if (!(flags & _EM_ZERODIVIDE)) fpcr |= ARM64_FPCR_DZE;
    if (!(flags & _EM_OVERFLOW)) fpcr |= ARM64_FPCR_OFE;
    if (!(flags & _EM_UNDERFLOW)) fpcr |= ARM64_FPCR_UFE;
    if (!(flags & _EM_INEXACT)) fpcr |= ARM64_FPCR_IXE;
    if (!(flags & _EM_DENORMAL)) fpcr |= ARM64_FPCR_IDE;

    switch (flags & _MCW_RC)
    {
        case _RC_DOWN:
            fpcr |= (1u << ARM64_FPCR_RMODE_SHIFT);
            break;
        case _RC_UP:
            fpcr |= (2u << ARM64_FPCR_RMODE_SHIFT);
            break;
        case _RC_CHOP:
            fpcr |= (3u << ARM64_FPCR_RMODE_SHIFT);
            break;
    }

    __setfpcr(fpcr);
    return flags;
}
