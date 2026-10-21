/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation Provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Random number generation
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * The system RNG is a SymCrypt AES-CTR DRBG (SP 800-90A). It is seeded from
 * the CPU's RDSEED and RDRAND instructions when they are present, and from
 * timer jitter, and it is reseeded from the same sources on every request.
 * Without RDSEED/RDRAND the only entropy is timing, which is weak early in
 * boot; ReactOS has no entropy pool that could be used instead yet.
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#if defined(_M_IX86) || defined(_M_AMD64)
#include <immintrin.h>
#endif

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define CNG_TIMER_SAMPLES       32
#define CNG_HARDWARE_WORDS      16
#define CNG_HARDWARE_RETRIES    10

static SYMCRYPT_RNG_AES_STATE CngRngState;
static KSPIN_LOCK CngRngLock;
static BOOLEAN CngHaveRdrand;
static BOOLEAN CngHaveRdseed;

/* FUNCTIONS ******************************************************************/

#if defined(_M_IX86) || defined(_M_AMD64)

static
VOID
CngDetectHardwareRng(VOID)
{
    int CpuInfo[4];

    __cpuid(CpuInfo, 0);
    if (CpuInfo[0] >= 7)
    {
        __cpuidex(CpuInfo, 7, 0);
        CngHaveRdseed = !!(CpuInfo[1] & (1 << 18));
    }

    __cpuid(CpuInfo, 1);
    CngHaveRdrand = !!(CpuInfo[2] & (1 << 30));
}

/* Returns the number of words that were filled */
static
ULONG
CngReadHardwareRng(
    _Out_writes_(Count) PULONG Buffer,
    _In_ ULONG Count)
{
    ULONG i, Try;
    unsigned int Value;

    for (i = 0; i < Count; i++)
    {
        for (Try = 0; Try < CNG_HARDWARE_RETRIES; Try++)
        {
            if (CngHaveRdseed && _rdseed32_step(&Value))
                break;
            if (CngHaveRdrand && _rdrand32_step(&Value))
                break;
        }

        if (Try == CNG_HARDWARE_RETRIES)
            break;

        Buffer[i] = Value;
    }

    return i;
}

#endif /* _M_IX86 || _M_AMD64 */

/* Condenses everything we can find into 64 bytes of seed material */
static
VOID
CngCollectEntropy(
    _Out_writes_bytes_(SYMCRYPT_SHA512_RESULT_SIZE) PUCHAR Seed)
{
    SYMCRYPT_SHA512_STATE State;
    struct
    {
        LARGE_INTEGER PerformanceCounter;
        LARGE_INTEGER SystemTime;
        ULONGLONG InterruptTime;
        ULONG64 TimeStamp;
        HANDLE ProcessId;
        HANDLE ThreadId;
        ULONG Processor;
        PVOID StackAddress;
    } Sample;
#if defined(_M_IX86) || defined(_M_AMD64)
    ULONG Hardware[CNG_HARDWARE_WORDS];
    ULONG Count;
#endif
    ULONG i;

    SymCryptSha512Init(&State);

#if defined(_M_IX86) || defined(_M_AMD64)
    if (CngHaveRdseed || CngHaveRdrand)
    {
        Count = CngReadHardwareRng(Hardware, RTL_NUMBER_OF(Hardware));
        SymCryptSha512Append(&State, (PCBYTE)Hardware, Count * sizeof(ULONG));
        SymCryptWipeKnownSize(Hardware, sizeof(Hardware));
    }
#endif

    for (i = 0; i < CNG_TIMER_SAMPLES; i++)
    {
        Sample.PerformanceCounter = KeQueryPerformanceCounter(NULL);
        KeQuerySystemTime(&Sample.SystemTime);
        Sample.InterruptTime = KeQueryInterruptTime();
#if defined(_M_IX86) || defined(_M_AMD64)
        Sample.TimeStamp = __rdtsc();
#else
        Sample.TimeStamp = 0;
#endif
        Sample.ProcessId = PsGetCurrentProcessId();
        Sample.ThreadId = PsGetCurrentThreadId();
        Sample.Processor = KeGetCurrentProcessorNumber();
        Sample.StackAddress = &Sample;
        SymCryptSha512Append(&State, (PCBYTE)&Sample, sizeof(Sample));
    }

    SymCryptSha512Result(&State, Seed);
    SymCryptWipeKnownSize(&Sample, sizeof(Sample));
}

NTSTATUS
CngInitializeRng(VOID)
{
    UCHAR Seed[SYMCRYPT_SHA512_RESULT_SIZE];
    SYMCRYPT_ERROR Error;

    KeInitializeSpinLock(&CngRngLock);

#if defined(_M_IX86) || defined(_M_AMD64)
    CngDetectHardwareRng();
#endif
    DPRINT("RDSEED %u, RDRAND %u\n", CngHaveRdseed, CngHaveRdrand);

    CngCollectEntropy(Seed);
    Error = SymCryptRngAesInstantiate(&CngRngState, Seed, sizeof(Seed));
    SymCryptWipeKnownSize(Seed, sizeof(Seed));

    /* Only fails for a bad seed size */
    ASSERT(Error == SYMCRYPT_NO_ERROR);
    return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INTERNAL_ERROR;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
CngGenerateRandom(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ SIZE_T Length)
{
    UCHAR Seed[SYMCRYPT_SHA512_RESULT_SIZE];
    KIRQL OldIrql;

    CngEnsureInitialized();
    CngCollectEntropy(Seed);

    KeAcquireSpinLock(&CngRngLock, &OldIrql);
    SymCryptRngAesReseed(&CngRngState, Seed, sizeof(Seed));
    SymCryptRngAesGenerate(&CngRngState, Buffer, Length);
    KeReleaseSpinLock(&CngRngLock, OldIrql);

    SymCryptWipeKnownSize(Seed, sizeof(Seed));
}

SYMCRYPT_ERROR
SYMCRYPT_CALL
SymCryptCallbackRandom(
    PBYTE Buffer,
    SIZE_T Length)
{
    CngGenerateRandom(Buffer, Length);
    return SYMCRYPT_NO_ERROR;
}

NTSTATUS
WINAPI
BCryptGenRandom(
    _Inout_opt_ BCRYPT_ALG_HANDLE hAlgorithm,
    _Out_writes_bytes_(cbBuffer) PUCHAR pbBuffer,
    _In_ ULONG cbBuffer,
    _In_ ULONG dwFlags)
{
    const CNG_ALGORITHM *Algorithm;

    if (dwFlags & ~(BCRYPT_RNG_USE_ENTROPY_IN_BUFFER | BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        return STATUS_INVALID_PARAMETER;

    if (dwFlags & BCRYPT_USE_SYSTEM_PREFERRED_RNG)
    {
        if (hAlgorithm)
            return STATUS_INVALID_PARAMETER;
    }
    else
    {
        Algorithm = CngGetAlgorithm(hAlgorithm);
        if (!Algorithm || Algorithm->Id != CngAlgRng)
            return STATUS_INVALID_HANDLE;
    }

    if (!cbBuffer)
        return STATUS_SUCCESS;
    if (!pbBuffer)
        return STATUS_INVALID_PARAMETER;

    /* BCRYPT_RNG_USE_ENTROPY_IN_BUFFER is ignored, as on Windows 8 and later */
    CngGenerateRandom(pbBuffer, cbBuffer);
    return STATUS_SUCCESS;
}

/* Exported for other kernel components, like on Windows */
BOOLEAN
NTAPI
SystemPrng(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ SIZE_T Length)
{
    CngGenerateRandom(Buffer, Length);
    return TRUE;
}
