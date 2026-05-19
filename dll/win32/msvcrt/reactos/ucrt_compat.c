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

/* MSVC's CRT headers can declare some of these as intrinsics; disable
 * the intrinsic translation so we can provide real bodies that the
 * msvcrt apiset host needs to export. */
#ifdef _MSC_VER
#pragma function(acosh, asinh, atanh, exp2, expm1, log1p, log2, round, roundf, trunc)
#endif

double __cdecl acosh(double x) { return log(x + sqrt(x*x - 1.0)); }
double __cdecl asinh(double x) { return log(x + sqrt(x*x + 1.0)); }
double __cdecl atanh(double x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
double __cdecl exp2 (double x) { return pow(2.0, x); }
double __cdecl expm1(double x) { return exp(x) - 1.0; }
double __cdecl log1p(double x) { return log(1.0 + x); }
double __cdecl log2 (double x) { return log(x) / 0.69314718055994530942; }
double __cdecl round(double x) { return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5); }
float  __cdecl roundf(float x) { return (x >= 0.0f) ? (float)floor(x + 0.5f) : (float)ceil(x - 0.5f); }
double __cdecl trunc(double x) { return (x >= 0.0) ? floor(x) : ceil(x); }
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
