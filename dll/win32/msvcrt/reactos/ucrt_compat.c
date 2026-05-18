/*
 * PROJECT:     ReactOS msvcrt UCRT compatibility shim
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     UCRT-only names that the api-ms-win-crt-*.dll apisets expose
 *              and that callers reach via the msvcrt host redirect.
 */

#include <stdio.h>

extern FILE * __cdecl __iob_func(void);

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
