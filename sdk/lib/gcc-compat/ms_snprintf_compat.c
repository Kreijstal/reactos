/*
 * PROJECT:     GCC C++ support library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     __ms_snprintf/__ms_vsnprintf for MSYS2's libwinpthread
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <stdio.h>
#include <stdarg.h>

/* MinGW maps snprintf to __ms_snprintf when __USE_MINGW_ANSI_STDIO is off;
 * MSYS2's libwinpthread is built that way and its import normally comes from
 * mingw-w64's libmsvcrt.a, which ReactOS's -nostdlib link replaces. The MS
 * semantics are those of msvcrt _snprintf (no guaranteed null termination). */

int __cdecl __ms_vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    return _vsnprintf(s, n, fmt, ap);
}

int __cdecl __ms_snprintf(char *s, size_t n, const char *fmt, ...)
{
    int ret;
    va_list ap;
    va_start(ap, fmt);
    ret = _vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return ret;
}
