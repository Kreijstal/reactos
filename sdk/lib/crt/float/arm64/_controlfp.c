/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     AArch64 implementation of _control87 / _controlfp
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "fpscr.h"

unsigned int __cdecl _control87(unsigned int newval, unsigned int mask)
{
    unsigned int fpcr = __getfpcr();
    unsigned int flags = 0;

    /* Sanitize the mask to the bits we model */
    mask &= (_MCW_EM | _MCW_RC | _MCW_DN);

    /* Convert the native control word to abstract flags. A trap-enable bit
       that is clear means the exception is masked in the abstract model. */
    if (!(fpcr & ARM64_FPCR_IOE)) flags |= _EM_INVALID;
    if (!(fpcr & ARM64_FPCR_DZE)) flags |= _EM_ZERODIVIDE;
    if (!(fpcr & ARM64_FPCR_OFE)) flags |= _EM_OVERFLOW;
    if (!(fpcr & ARM64_FPCR_UFE)) flags |= _EM_UNDERFLOW;
    if (!(fpcr & ARM64_FPCR_IXE)) flags |= _EM_INEXACT;
    if (!(fpcr & ARM64_FPCR_IDE)) flags |= _EM_DENORMAL;

    switch ((fpcr & ARM64_FPCR_RMODE_MASK) >> ARM64_FPCR_RMODE_SHIFT)
    {
        case ARM64_RMODE_UP:   flags |= _RC_UP; break;
        case ARM64_RMODE_DOWN: flags |= _RC_DOWN; break;
        case ARM64_RMODE_ZERO: flags |= _RC_CHOP; break;
        default:               flags |= _RC_NEAR; break;
    }

    if (fpcr & ARM64_FPCR_FZ) flags |= _DN_FLUSH;

    /* Merge in the requested changes */
    flags = (flags & ~mask) | (newval & mask);

    /* Convert the abstract flags back into the native control word */
    fpcr &= ~(ARM64_FPCR_IOE | ARM64_FPCR_DZE | ARM64_FPCR_OFE |
              ARM64_FPCR_UFE | ARM64_FPCR_IXE | ARM64_FPCR_IDE |
              ARM64_FPCR_FZ | ARM64_FPCR_RMODE_MASK);

    if (!(flags & _EM_INVALID))    fpcr |= ARM64_FPCR_IOE;
    if (!(flags & _EM_ZERODIVIDE)) fpcr |= ARM64_FPCR_DZE;
    if (!(flags & _EM_OVERFLOW))   fpcr |= ARM64_FPCR_OFE;
    if (!(flags & _EM_UNDERFLOW))  fpcr |= ARM64_FPCR_UFE;
    if (!(flags & _EM_INEXACT))    fpcr |= ARM64_FPCR_IXE;
    if (!(flags & _EM_DENORMAL))   fpcr |= ARM64_FPCR_IDE;

    switch (flags & _MCW_RC)
    {
        case _RC_UP:   fpcr |= ARM64_RMODE_UP   << ARM64_FPCR_RMODE_SHIFT; break;
        case _RC_DOWN: fpcr |= ARM64_RMODE_DOWN << ARM64_FPCR_RMODE_SHIFT; break;
        case _RC_CHOP: fpcr |= ARM64_RMODE_ZERO << ARM64_FPCR_RMODE_SHIFT; break;
        default:       fpcr |= ARM64_RMODE_NEAREST << ARM64_FPCR_RMODE_SHIFT; break;
    }

    if (flags & _DN_FLUSH) fpcr |= ARM64_FPCR_FZ;

    __setfpcr(fpcr);

    return flags;
}

unsigned int __cdecl _controlfp(unsigned int newval, unsigned int mask)
{
    return _control87(newval, mask & ~_EM_DENORMAL);
}
