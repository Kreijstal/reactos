/*
 * PROJECT:     GCC C++ support library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     fegetenv/fesetenv for the UCRT64 libwinpthread
 * COPYRIGHT:   Encoding from Wine's msvcrt math.c (Copyright 2000 Jon Griffiths and others)
 */

/*
 * Since 14.0.0.r598, MSYS2's UCRT64 libwinpthread captures the creating
 * thread's floating point environment with fegetenv and applies it to the new
 * thread with fesetenv. In that environment these come from ucrtbase, which
 * ReactOS executables do not link; msvcrt.dll does not export them on Windows
 * either. For msvcrt-based MinGW targets they come from a static library, and
 * this is that library for our link line.
 *
 * libwinpthread is compiled against mingw-w64's UCRT fenv.h, so fenv_t is the
 * 8-byte UCRT layout, not the 32-byte one in our sdk/include/crt/fenv.h, and
 * it is stored inline in the thread structure. The control and status words
 * use the ucrtbase encoding, so an environment means the same as on Windows.
 */

#include <float.h>
#include <xmmintrin.h>

#ifndef __x86_64__
#error fenv_compat is only needed for the x86_64 UCRT64 toolchain
#endif

typedef struct
{
    unsigned long _Fe_ctl;
    unsigned long _Fe_stat;
} ucrt_fenv_t;

/* ucrtbase encoding of the SSE control/status word on x64 */
enum
{
    FENV_Y_INVALID = 0x10000010,
    FENV_Y_DENORMAL = 0x20000020,
    FENV_Y_ZERODIVIDE = 0x08000008,
    FENV_Y_OVERFLOW = 0x04000004,
    FENV_Y_UNDERFLOW = 0x02000002,
    FENV_Y_INEXACT = 0x01000001,
    FENV_Y_UP = 0x80000200,
    FENV_Y_DOWN = 0x40000100,
    FENV_Y_FLUSH = 0x00000400,
    FENV_Y_FLUSH_SAVE = 0x00000800
};

static unsigned long fenv_encode(unsigned int y)
{
    unsigned long ret = 0;

    if (y & _EM_INVALID) ret |= FENV_Y_INVALID;
    if (y & _EM_DENORMAL) ret |= FENV_Y_DENORMAL;
    if (y & _EM_ZERODIVIDE) ret |= FENV_Y_ZERODIVIDE;
    if (y & _EM_OVERFLOW) ret |= FENV_Y_OVERFLOW;
    if (y & _EM_UNDERFLOW) ret |= FENV_Y_UNDERFLOW;
    if (y & _EM_INEXACT) ret |= FENV_Y_INEXACT;
    if (y & _RC_UP) ret |= FENV_Y_UP;
    if (y & _RC_DOWN) ret |= FENV_Y_DOWN;
    if (y & _DN_FLUSH) ret |= FENV_Y_FLUSH;
    if (y & _DN_FLUSH_OPERANDS_SAVE_RESULTS) ret |= FENV_Y_FLUSH_SAVE;
    return ret;
}

/* Returns 0 when enc is not something fenv_encode produces */
static int fenv_decode(unsigned long enc, unsigned int *y)
{
    *y = 0;
    if ((enc & FENV_Y_INVALID) == FENV_Y_INVALID) *y |= _EM_INVALID;
    if ((enc & FENV_Y_DENORMAL) == FENV_Y_DENORMAL) *y |= _EM_DENORMAL;
    if ((enc & FENV_Y_ZERODIVIDE) == FENV_Y_ZERODIVIDE) *y |= _EM_ZERODIVIDE;
    if ((enc & FENV_Y_OVERFLOW) == FENV_Y_OVERFLOW) *y |= _EM_OVERFLOW;
    if ((enc & FENV_Y_UNDERFLOW) == FENV_Y_UNDERFLOW) *y |= _EM_UNDERFLOW;
    if ((enc & FENV_Y_INEXACT) == FENV_Y_INEXACT) *y |= _EM_INEXACT;
    if ((enc & FENV_Y_UP) == FENV_Y_UP) *y |= _RC_UP;
    if ((enc & FENV_Y_DOWN) == FENV_Y_DOWN) *y |= _RC_DOWN;
    if ((enc & FENV_Y_FLUSH) == FENV_Y_FLUSH) *y |= _DN_FLUSH;
    if ((enc & FENV_Y_FLUSH_SAVE) == FENV_Y_FLUSH_SAVE) *y |= _DN_FLUSH_OPERANDS_SAVE_RESULTS;
    return fenv_encode(*y) == enc;
}

int __cdecl fegetenv(ucrt_fenv_t *env)
{
    env->_Fe_ctl = fenv_encode(_control87(0, 0));
    env->_Fe_stat = fenv_encode(_statusfp());
    return 0;
}

int __cdecl fesetenv(const ucrt_fenv_t *env)
{
    unsigned int ctl, stat, csr;

    /* An all-zero environment is ucrtbase's FE_DFL_ENV */
    if (!env->_Fe_ctl && !env->_Fe_stat)
    {
        _fpreset();
        return 0;
    }

    if (!fenv_decode(env->_Fe_ctl, &ctl) || !fenv_decode(env->_Fe_stat, &stat))
        return 1;

    _control87(ctl, _MCW_EM | _MCW_RC | _MCW_DN);

    /* The status flags are set exactly; no CRT routine raises them, so the
     * ones to keep go straight into MXCSR after clearing the rest */
    _clearfp();
    csr = _mm_getcsr();
    if (stat & _SW_INVALID) csr |= 0x01;
    if (stat & _SW_DENORMAL) csr |= 0x02;
    if (stat & _SW_ZERODIVIDE) csr |= 0x04;
    if (stat & _SW_OVERFLOW) csr |= 0x08;
    if (stat & _SW_UNDERFLOW) csr |= 0x10;
    if (stat & _SW_INEXACT) csr |= 0x20;
    _mm_setcsr(csr);
    return 0;
}
