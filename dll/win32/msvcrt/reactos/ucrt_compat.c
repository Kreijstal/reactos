/*
 * PROJECT:     ReactOS msvcrt UCRT compatibility shim
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     UCRT-only names that the api-ms-win-crt-*.dll apisets expose
 *              and that callers reach via the msvcrt host redirect.
 */

#include <math.h>
#include <stdio.h>

extern FILE * __cdecl __iob_func(void);

/* MSVCRT-internal globals exposed by data.c / time.c.  See msvcrt.spec
 * for the @ extern aliases that already publish them under their
 * legacy names (_environ, _timezone, _tzname, _daylight). */
extern long  MSVCRT___timezone;
extern char *MSVCRT__tzname[2];

/* UCRT shape: these names are getters, not bare variables, so a caller
 * that thinks `__tzname()` is a function does `call qword ptr [iat]`
 * and lands in data when we export them as @ extern.  Provide the
 * getter functions so the IAT slot resolves to a callable here. */
char ** __cdecl __ucrt_tzname(void)  { return MSVCRT__tzname; }
long *  __cdecl __ucrt_timezone(void){ return &MSVCRT___timezone; }

/* ---------------- C99 math ---------------- *
 *
 * Provided unconditionally so the api-ms-win-crt-math-l1-1-0.dll
 * apiset, which resolves to msvcrt.dll on ReactOS, can satisfy
 * imports from UCRT-linked binaries.  Implementations are minimal
 * wrappers over already-exported msvcrt routines.
 */

double __cdecl cbrt(double x)
{
    if (x == 0.0) return x;
    return (x < 0) ? -pow(-x, 1.0/3.0) : pow(x, 1.0/3.0);
}

/* All C99 math wrappers are defined under internal names and exported
 * via msvcrt.spec aliases.  This sidesteps MSVC's CRT-header intrinsic
 * declarations (log2 is an intrinsic on amd64 Release builds and not
 * on i386, so neither defining it directly nor pragma-toggling works
 * portably). */
static double __cdecl ucrt_acosh(double x) { return log(x + sqrt(x*x - 1.0)); }
static double __cdecl ucrt_asinh(double x) { return log(x + sqrt(x*x + 1.0)); }
static double __cdecl ucrt_atanh(double x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
static double __cdecl ucrt_exp2 (double x) { return pow(2.0, x); }
static double __cdecl ucrt_expm1(double x) { return exp(x) - 1.0; }
static double __cdecl ucrt_log1p(double x) { return log(1.0 + x); }
static double __cdecl ucrt_log2 (double x) { return log(x) / 0.69314718055994530942; }
static double __cdecl ucrt_round(double x) { return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5); }
static float  __cdecl ucrt_roundf(float x) { return (x >= 0.0f) ? (float)floor(x + 0.5f) : (float)ceil(x - 0.5f); }
static double __cdecl ucrt_trunc(double x) { return (x >= 0.0) ? floor(x) : ceil(x); }

/* Expose the static helpers under their plain C names via aliases the
 * spec exports.  spec2def emits .def entries that pick up these
 * extern symbols at link time; the implementations behind them are the
 * static functions above. */
double __cdecl _msvcrt_ucrt_acosh(double x) { return ucrt_acosh(x); }
double __cdecl _msvcrt_ucrt_asinh(double x) { return ucrt_asinh(x); }
double __cdecl _msvcrt_ucrt_atanh(double x) { return ucrt_atanh(x); }
double __cdecl _msvcrt_ucrt_exp2 (double x) { return ucrt_exp2(x); }
double __cdecl _msvcrt_ucrt_expm1(double x) { return ucrt_expm1(x); }
double __cdecl _msvcrt_ucrt_log1p(double x) { return ucrt_log1p(x); }
double __cdecl _msvcrt_ucrt_log2 (double x) { return ucrt_log2(x); }
double __cdecl _msvcrt_ucrt_round(double x) { return ucrt_round(x); }
float  __cdecl _msvcrt_ucrt_roundf(float x) { return ucrt_roundf(x); }
double __cdecl _msvcrt_ucrt_trunc(double x) { return ucrt_trunc(x); }

/* hypot stays separate (already exported via the existing
 * "hypot" spec line that aliases to _msvcrt_ucrt_hypot). */
double __cdecl _msvcrt_ucrt_hypot(double x, double y) { return _hypot(x, y); }

/*********************************************************************
 *      __acrt_iob_func (UCRTBASE.@)
 *
 * UCRT replaces the old "return the stdin/stdout/stderr array base"
 * __iob_func() with a per-index accessor.  Forward to the existing
 * msvcrt implementation.
 */
FILE * __cdecl __acrt_iob_func(unsigned ix)
{
    return __iob_func() + ix;
}
