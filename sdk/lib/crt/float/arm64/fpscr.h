/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     AArch64 FPCR/FPSR helpers for the floating-point control API
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#pragma once

#include <float.h>

/* FPCR (control) - exception trap enables. A set bit traps the exception,
   which is the opposite of the abstract _EM_* "masked" convention. */
#define ARM64_FPCR_IOE  (1u << 8)   /* Invalid operation       */
#define ARM64_FPCR_DZE  (1u << 9)   /* Divide by zero          */
#define ARM64_FPCR_OFE  (1u << 10)  /* Overflow                */
#define ARM64_FPCR_UFE  (1u << 11)  /* Underflow               */
#define ARM64_FPCR_IXE  (1u << 12)  /* Inexact                 */
#define ARM64_FPCR_IDE  (1u << 15)  /* Input denormal          */
#define ARM64_FPCR_FZ   (1u << 24)  /* Flush-to-zero           */

#define ARM64_FPCR_RMODE_SHIFT 22
#define ARM64_FPCR_RMODE_MASK  (3u << ARM64_FPCR_RMODE_SHIFT)
#define ARM64_RMODE_NEAREST 0u  /* Round to nearest, ties to even */
#define ARM64_RMODE_UP      1u  /* Round towards +infinity        */
#define ARM64_RMODE_DOWN    2u  /* Round towards -infinity        */
#define ARM64_RMODE_ZERO    3u  /* Round towards zero (chop)       */

/* FPSR (status) - cumulative exception flags */
#define ARM64_FPSR_IOC  (1u << 0)   /* Invalid operation       */
#define ARM64_FPSR_DZC  (1u << 1)   /* Divide by zero          */
#define ARM64_FPSR_OFC  (1u << 2)   /* Overflow                */
#define ARM64_FPSR_UFC  (1u << 3)   /* Underflow               */
#define ARM64_FPSR_IXC  (1u << 4)   /* Inexact                 */
#define ARM64_FPSR_IDC  (1u << 7)   /* Input denormal          */
#define ARM64_FPSR_EXC_MASK \
    (ARM64_FPSR_IOC | ARM64_FPSR_DZC | ARM64_FPSR_OFC | \
     ARM64_FPSR_UFC | ARM64_FPSR_IXC | ARM64_FPSR_IDC)

static __inline unsigned int __getfpcr(void)
{
    return (unsigned int)__builtin_arm_rsr64("fpcr");
}

static __inline void __setfpcr(unsigned int value)
{
    __builtin_arm_wsr64("fpcr", value);
}

static __inline unsigned int __getfpsr(void)
{
    return (unsigned int)__builtin_arm_rsr64("fpsr");
}

static __inline void __setfpsr(unsigned int value)
{
    __builtin_arm_wsr64("fpsr", value);
}
