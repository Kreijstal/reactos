//////////////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2007  Qumranet All Rights Reserved
//
// Module Name:
// osdep.h
//
// Abstract:
// Windows OS dependent definitions of data types
//
// Author:
// Yan Vugenfirer  - February 2007.
//
//////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <ntddk.h>

#ifdef __REACTOS__
#ifdef __GNUC__
#undef FORCEINLINE
/* GCC rejects always_inline on a function that is not also declared inline
 * (-Werror=attributes), so keep the inline keyword here. */
#define FORCEINLINE __inline__ __attribute__((__always_inline__))
#endif
#endif

#ifndef __REACTOS__
#if !defined(ENOSPC)
#define ENOSPC 1
#endif
#endif

#if !defined(__cplusplus) && !defined(bool)
// Important note: in MSFT C++ bool length is 1 bytes
// C++ does not define length of bool
// inconsistent definition of 'bool' may create compatibility problems
#define bool u8
#define false FALSE
#define true TRUE
#endif

#define SMP_CACHE_BYTES 64
