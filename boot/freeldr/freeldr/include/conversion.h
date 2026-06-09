/*
 * PROJECT:         EFI Windows Loader
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            boot/freeldr/freeldr/windows/conversion.c
 * PURPOSE:         Physical <-> Virtual addressing mode conversions (arch-specific)
 * PROGRAMMERS:     Aleksey Bragin (aleksey@reactos.org)
 */

#pragma once

#if defined(__clang__) && defined(_M_ARM64)
/* On ARM64/Clang, FORCEINLINE expands to a static inline. A matching extern
   prototype for these helpers is visible (ntldr/winldr.h), which promotes the
   static inline to an external definition emitted in every translation unit -
   a link-time collision. Use the gnu_inline extern-inline form so the
   definition stays a pure inline, as it does on the other targets. */
#define FRLDR_VA_INLINE extern __inline__ __attribute__((__always_inline__, __gnu_inline__))
#else
#define FRLDR_VA_INLINE FORCEINLINE
#endif

#ifndef _ZOOM2_
/* Arch-specific addresses translation implementation */
FRLDR_VA_INLINE
PVOID
VaToPa(PVOID Va)
{
    return (PVOID)((ULONG_PTR)Va & ~KSEG0_BASE);
}

FRLDR_VA_INLINE
PVOID
PaToVa(PVOID Pa)
{
    return (PVOID)((ULONG_PTR)Pa | KSEG0_BASE);
}
#else
FRLDR_VA_INLINE
PVOID
VaToPa(PVOID Va)
{
    return Va;
}

FRLDR_VA_INLINE
PVOID
PaToVa(PVOID Pa)
{
    return Pa;
}
#endif
