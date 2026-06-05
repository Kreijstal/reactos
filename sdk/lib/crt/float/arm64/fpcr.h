/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 floating-point control helpers
 */

#pragma once

#include <float.h>

#define ARM64_FP_EXCEPTION_MASK 0x9F

#define ARM64_FPCR_IOE (1u << 8)
#define ARM64_FPCR_DZE (1u << 9)
#define ARM64_FPCR_OFE (1u << 10)
#define ARM64_FPCR_UFE (1u << 11)
#define ARM64_FPCR_IXE (1u << 12)
#define ARM64_FPCR_IDE (1u << 15)
#define ARM64_FPCR_RMODE_SHIFT 22
#define ARM64_FPCR_RMODE_MASK (3u << ARM64_FPCR_RMODE_SHIFT)

static inline unsigned int __getfpcr(void)
{
    unsigned long long Value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(Value));
    return (unsigned int)Value;
}

static inline void __setfpcr(unsigned int Value)
{
    unsigned long long RegisterValue = Value;
    __asm__ volatile("msr fpcr, %0" :: "r"(RegisterValue));
}

static inline unsigned int __getfpsr(void)
{
    unsigned long long Value;
    __asm__ volatile("mrs %0, fpsr" : "=r"(Value));
    return (unsigned int)Value;
}

static inline void __setfpsr(unsigned int Value)
{
    unsigned long long RegisterValue = Value;
    __asm__ volatile("msr fpsr, %0" :: "r"(RegisterValue));
}
