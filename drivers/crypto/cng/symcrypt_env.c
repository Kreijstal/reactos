/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation Provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SymCrypt environment for ReactOS kernel mode
 * COPYRIGHT:   Copyright (c) Microsoft Corporation (env_windowsKernelModeWin8_1.c)
 *              Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * This is modelled after SymCrypt's env_windowsKernelModeWin8_1.c.
 *
 * ReactOS currently builds SymCrypt with SYMCRYPT_IGNORE_PLATFORM, so only
 * its portable C code is used and none of the code below that deals with the
 * CPU is compiled in. Should the platform code be enabled, two differences
 * from Windows that come from the ReactOS kernel matter:
 *
 * - There is no KeSaveExtendedProcessorState, so the Ymm and Zmm registers
 *   cannot be saved. AVX2, AVX-512 and VAES code paths are therefore disabled
 *   and SymCrypt only uses the Xmm registers.
 * - On x86 the Xmm registers belong to the thread's NPX state and would have
 *   to be saved with KeSaveFloatingPointState, which allocates pool. Instead
 *   SymCrypt is told that saving fails, so it falls back to its portable C
 *   code. On amd64 the Xmm registers are part of the calling convention and
 *   of the trap frame, so no save is needed and the AES-NI and PCLMULQDQ code
 *   can be used at any IRQL up to DISPATCH_LEVEL, like on Windows 8.1 and later.
 */

#include <ntddk.h>
#include <symcrypt.h>
#include <sc_lib.h>

#ifndef CRYPTO_LIBRARY_INTERNAL_ERROR
#define CRYPTO_LIBRARY_INTERNAL_ERROR 0x00000171
#endif

SYMCRYPT_ENVIRONMENT_DEFS(ReactOSKernelmode);

#if SYMCRYPT_CPU_X86 | SYMCRYPT_CPU_AMD64
#define CNG_FEATURES_NEVER_PRESENT (SYMCRYPT_CPU_FEATURE_AVX2 | \
                                    SYMCRYPT_CPU_FEATURE_AVX512 | \
                                    SYMCRYPT_CPU_FEATURE_VAES)
#endif

SYMCRYPT_CPU_FEATURES
SYMCRYPT_CALL
SymCryptCpuFeaturesNeverPresentEnvReactOSKernelmode(VOID)
{
#if SYMCRYPT_CPU_X86
    return CNG_FEATURES_NEVER_PRESENT | SYMCRYPT_CPU_FEATURE_SAVEXMM_NOFAIL;
#elif SYMCRYPT_CPU_AMD64
    return CNG_FEATURES_NEVER_PRESENT;
#else
    return 0;
#endif
}

VOID
SYMCRYPT_CALL
SymCryptInitEnvReactOSKernelmode(
    UINT32 Version)
{
    if (g_SymCryptFlags & SYMCRYPT_FLAG_LIB_INITIALIZED)
        return;

#if SYMCRYPT_CPU_X86 | SYMCRYPT_CPU_AMD64
    SymCryptDetectCpuFeaturesByCpuid(SYMCRYPT_CPUID_DETECT_FLAG_CHECK_OS_SUPPORT_FOR_YMM);
    g_SymCryptCpuFeaturesNotPresent |= CNG_FEATURES_NEVER_PRESENT;
#if SYMCRYPT_CPU_AMD64
    /* SymCryptSaveXmm never fails on amd64 */
    g_SymCryptCpuFeaturesNotPresent &= ~SYMCRYPT_CPU_FEATURE_SAVEXMM_NOFAIL;
#endif
#endif

    SymCryptInitEnvCommon(Version);
}

_Analysis_noreturn_
VOID
SYMCRYPT_CALL
SymCryptFatalEnvReactOSKernelmode(
    UINT32 FatalCode)
{
    SymCryptFatalIntercept(FatalCode);
    KeBugCheckEx(CRYPTO_LIBRARY_INTERNAL_ERROR, FatalCode, 0, 0, 0);
    SymCryptFatalHang(FatalCode);
}

VOID
SYMCRYPT_CALL
SymCryptTestInjectErrorEnvReactOSKernelmode(
    PBYTE Buffer,
    SIZE_T Length)
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
}

#if SYMCRYPT_CPU_X86 | SYMCRYPT_CPU_AMD64

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptSaveXmmEnvReactOSKernelmode(
    _Out_ PSYMCRYPT_EXTENDED_SAVE_DATA SaveData)
{
    UNREFERENCED_PARAMETER(SaveData);
#if SYMCRYPT_CPU_AMD64
    return SYMCRYPT_NO_ERROR;
#else
    return SYMCRYPT_EXTERNAL_FAILURE;
#endif
}

VOID
SYMCRYPT_CALL
SymCryptRestoreXmmEnvReactOSKernelmode(
    _Inout_ PSYMCRYPT_EXTENDED_SAVE_DATA SaveData)
{
    UNREFERENCED_PARAMETER(SaveData);
}

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptSaveYmmEnvReactOSKernelmode(
    _Out_ PSYMCRYPT_EXTENDED_SAVE_DATA SaveData)
{
    UNREFERENCED_PARAMETER(SaveData);
    return SYMCRYPT_EXTERNAL_FAILURE;
}

VOID
SYMCRYPT_CALL
SymCryptRestoreYmmEnvReactOSKernelmode(
    _Inout_ PSYMCRYPT_EXTENDED_SAVE_DATA SaveData)
{
    UNREFERENCED_PARAMETER(SaveData);
}

VOID
SYMCRYPT_CALL
SymCryptCpuidExFuncEnvReactOSKernelmode(
    int CpuInfo[4],
    int FunctionId,
    int SubFunctionId)
{
    __cpuidex(CpuInfo, FunctionId, SubFunctionId);
}

#endif /* SYMCRYPT_CPU_X86 | SYMCRYPT_CPU_AMD64 */
