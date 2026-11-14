/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the XState context functions
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"
#include <ndk/ketypes.h>

typedef DWORD64 (WINAPI *FN_GetEnabledXStateFeatures)(VOID);
typedef BOOL (WINAPI *FN_InitializeContext)(PVOID, DWORD, PCONTEXT*, PDWORD);
typedef BOOL (WINAPI *FN_InitializeContext2)(PVOID, DWORD, PCONTEXT*, PDWORD, ULONG64);
typedef BOOL (WINAPI *FN_CopyContext)(PCONTEXT, DWORD, PCONTEXT);
typedef PVOID (WINAPI *FN_LocateXStateFeature)(PCONTEXT, DWORD, PDWORD);
typedef BOOL (WINAPI *FN_GetXStateFeaturesMask)(PCONTEXT, PDWORD64);
typedef BOOL (WINAPI *FN_SetXStateFeaturesMask)(PCONTEXT, DWORD64);
typedef ULONG64 (NTAPI *FN_RtlGetEnabledExtendedFeatures)(ULONG64);

static FN_GetEnabledXStateFeatures pGetEnabledXStateFeatures;
static FN_InitializeContext pInitializeContext;
static FN_InitializeContext2 pInitializeContext2;
static FN_CopyContext pCopyContext;
static FN_LocateXStateFeature pLocateXStateFeature;
static FN_GetXStateFeaturesMask pGetXStateFeaturesMask;
static FN_SetXStateFeaturesMask pSetXStateFeaturesMask;
static FN_RtlGetEnabledExtendedFeatures pRtlGetEnabledExtendedFeatures;

#ifdef _M_AMD64

#define TEST_CONTEXT_LENGTH     0x4d0
#define TEST_CONTEXT_EX_LENGTH  0x18

static
ULONG
GetEntireXStateLength(
    _In_ ULONG64 CompactionMask)
{
    ULONG Length, Feature;

    if (!SharedUserData->XState.CompactionEnabled)
        return SharedUserData->XState.Size;

    Length = 0x240;
    for (Feature = 2; Feature < MAXIMUM_XSTATE_FEATURES; Feature++)
    {
        if (!(CompactionMask & (1ULL << Feature)))
            continue;
        if (SharedUserData->XState.AlignedFeatures & (1ULL << Feature))
            Length = (Length + 63) & ~63;
        Length += SharedUserData->XState.AllFeatures[Feature];
    }
    return Length;
}

static
VOID
TestPlainContext(VOID)
{
    DECLSPEC_ALIGN(16) UCHAR Buffer[0x600];
    PCONTEXT Context = NULL;
    PCONTEXT_EX ContextEx;
    DWORD Length;
    BOOL Ret;

    /* Querying the length */
    Length = 0;
    SetLastError(0xdeadbeef);
    Ret = pInitializeContext(NULL, CONTEXT_FULL, &Context, &Length);
    ok(!Ret, "InitializeContext succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_hex(Length, 0x4f7);

    /* Too small */
    Length = 0x4f6;
    SetLastError(0xdeadbeef);
    Ret = pInitializeContext(Buffer, CONTEXT_FULL, &Context, &Length);
    ok(!Ret, "InitializeContext succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_hex(Length, 0x4f7);

    /* An unaligned buffer gets aligned */
    Length = sizeof(Buffer) - 1;
    memset(Buffer, 0xcc, sizeof(Buffer));
    Ret = pInitializeContext(Buffer + 1, CONTEXT_FULL, &Context, &Length);
    ok(Ret, "InitializeContext failed with %lu\n", GetLastError());
    ok_hex(Length, 0x4f7);
    ok(Context == (PCONTEXT)(Buffer + 16), "Context %p, buffer %p\n", Context, Buffer);
    ok_hex(Context->ContextFlags, CONTEXT_FULL);

    ContextEx = (PCONTEXT_EX)(Context + 1);
    ok_long(ContextEx->All.Offset, -TEST_CONTEXT_LENGTH);
    ok_long(ContextEx->All.Length, TEST_CONTEXT_LENGTH + TEST_CONTEXT_EX_LENGTH);
    ok_long(ContextEx->Legacy.Offset, -TEST_CONTEXT_LENGTH);
    ok_long(ContextEx->Legacy.Length, TEST_CONTEXT_LENGTH);
    ok_long(ContextEx->XState.Offset, 0x19);
    ok_long(ContextEx->XState.Length, 0);

    /* The legacy features are part of every context */
    ok(pLocateXStateFeature(Context, XSTATE_LEGACY_FLOATING_POINT, &Length) == &Context->FltSave,
       "Wrong x87 location\n");
    ok_hex(Length, FIELD_OFFSET(XSAVE_FORMAT, XmmRegisters));
    ok(pLocateXStateFeature(Context, XSTATE_LEGACY_SSE, &Length) == &Context->FltSave.XmmRegisters,
       "Wrong SSE location\n");
    ok_hex(Length, 16 * sizeof(M128A));
    ok(pLocateXStateFeature(Context, XSTATE_AVX, &Length) == NULL, "AVX located in a plain context\n");

    /* Without an XState chunk only the legacy features can be selected */
    Context->ContextFlags = CONTEXT_CONTROL;
    ok(pSetXStateFeaturesMask(Context, XSTATE_MASK_LEGACY), "SetXStateFeaturesMask failed\n");
    ok_hex(Context->ContextFlags, CONTEXT_CONTROL | CONTEXT_FLOATING_POINT);
    SetLastError(0xdeadbeef);
    ok(!pSetXStateFeaturesMask(Context, XSTATE_MASK_AVX), "SetXStateFeaturesMask succeeded\n");
    ok_err(ERROR_INVALID_PARAMETER);
}

static
VOID
TestXStateContext(
    _In_ ULONG64 Features)
{
    PUCHAR Buffer;
    PCONTEXT Context = NULL, Copy = NULL;
    PCONTEXT_EX ContextEx;
    PUCHAR Header, Avx;
    DWORD Length, ExpectedLength, AvxLength;
    LONG ExpectedOffset;
    DWORD64 Mask;
    BOOL Ret;

    ExpectedLength = 0x4f0 + GetEntireXStateLength(Features) - 0x1c1;

    Length = 0;
    SetLastError(0xdeadbeef);
    Ret = pInitializeContext(NULL, CONTEXT_FULL | CONTEXT_XSTATE, &Context, &Length);
    ok(!Ret, "InitializeContext succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_hex(Length, ExpectedLength);

    Buffer = HeapAlloc(GetProcessHeap(), 0, 2 * Length);
    if (!Buffer)
    {
        skip("Out of memory\n");
        return;
    }
    memset(Buffer, 0xcc, 2 * Length);

    Ret = pInitializeContext(Buffer, CONTEXT_FULL | CONTEXT_XSTATE, &Context, &Length);
    ok(Ret, "InitializeContext failed with %lu\n", GetLastError());
    ok_hex(Context->ContextFlags, CONTEXT_FULL | CONTEXT_XSTATE);

    ContextEx = (PCONTEXT_EX)(Context + 1);
    Header = (PUCHAR)ContextEx + ContextEx->XState.Offset;
    ExpectedOffset = (LONG)((((ULONG_PTR)ContextEx + TEST_CONTEXT_EX_LENGTH + 63) & ~(ULONG_PTR)63) -
                            (ULONG_PTR)ContextEx);
    ok_long(ContextEx->XState.Offset, ExpectedOffset);
    ok_long(ContextEx->XState.Length, GetEntireXStateLength(Features) - 0x200);
    ok_long(ContextEx->Legacy.Offset, -TEST_CONTEXT_LENGTH);
    ok_long(ContextEx->Legacy.Length, TEST_CONTEXT_LENGTH);
    ok_long(ContextEx->All.Offset, -TEST_CONTEXT_LENGTH);
    ok_long(ContextEx->All.Length, TEST_CONTEXT_LENGTH + ExpectedOffset + ContextEx->XState.Length);
    ok(((ULONG_PTR)Header & 63) == 0, "XSAVE header %p not 64 byte aligned\n", Header);
    ok((PUCHAR)ContextEx + ContextEx->All.Offset + ContextEx->All.Length <= Buffer + Length,
       "Context overflows its buffer\n");

    /* The header starts out empty, laid out for every enabled feature */
    ok(((PXSAVE_AREA_HEADER)Header)->Mask == 0, "Mask %I64x\n", ((PXSAVE_AREA_HEADER)Header)->Mask);
    if (SharedUserData->XState.CompactionEnabled)
    {
        ok(((PXSAVE_AREA_HEADER)Header)->CompactionMask == (Features | (1ULL << 63)),
           "CompactionMask %I64x\n", ((PXSAVE_AREA_HEADER)Header)->CompactionMask);
    }
    else
    {
        ok(((PXSAVE_AREA_HEADER)Header)->CompactionMask == 0,
           "CompactionMask %I64x\n", ((PXSAVE_AREA_HEADER)Header)->CompactionMask);
    }

    ok(pGetXStateFeaturesMask(Context, &Mask), "GetXStateFeaturesMask failed\n");
    ok(Mask == XSTATE_MASK_LEGACY, "Mask %I64x\n", Mask);

    if (Features & XSTATE_MASK_AVX)
    {
        Avx = pLocateXStateFeature(Context, XSTATE_AVX, &AvxLength);
        ok(Avx != NULL, "AVX not located\n");
        ok_hex(AvxLength, 16 * sizeof(M128A));
        if (SharedUserData->XState.CompactionEnabled)
            ok(Avx == Header + 0x40, "AVX at %p, header at %p\n", Avx, Header);
        else
            ok(Avx == Header + SharedUserData->XState.Features[XSTATE_AVX].Offset - 0x200,
               "AVX at %p, header at %p\n", Avx, Header);

        ok(pSetXStateFeaturesMask(Context, XSTATE_MASK_AVX), "SetXStateFeaturesMask failed\n");
        ok(((PXSAVE_AREA_HEADER)Header)->Mask == XSTATE_MASK_AVX,
           "Mask %I64x\n", ((PXSAVE_AREA_HEADER)Header)->Mask);
        ok(pGetXStateFeaturesMask(Context, &Mask), "GetXStateFeaturesMask failed\n");
        ok(Mask == (XSTATE_MASK_LEGACY | XSTATE_MASK_AVX), "Mask %I64x\n", Mask);

        /* Copying carries the AVX state */
        memset(Avx, 0x5a, AvxLength);
        Context->Rip = 0x1234;
        Ret = pInitializeContext(Buffer + Length, CONTEXT_FULL | CONTEXT_XSTATE, &Copy, &Length);
        ok(Ret, "InitializeContext failed with %lu\n", GetLastError());
        if (Ret)
        {
            PUCHAR CopyAvx;

            Ret = pCopyContext(Copy, CONTEXT_FULL | CONTEXT_XSTATE, Context);
            ok(Ret, "CopyContext failed with %lu\n", GetLastError());
            ok(Copy->Rip == 0x1234, "Rip %I64x\n", Copy->Rip);
            ok(pGetXStateFeaturesMask(Copy, &Mask), "GetXStateFeaturesMask failed\n");
            ok(Mask == (XSTATE_MASK_LEGACY | XSTATE_MASK_AVX), "Mask %I64x\n", Mask);
            CopyAvx = pLocateXStateFeature(Copy, XSTATE_AVX, NULL);
            ok(CopyAvx && !memcmp(CopyAvx, Avx, AvxLength), "AVX state not copied\n");
        }
    }

    /* A plain context cannot receive XState */
    {
        DECLSPEC_ALIGN(16) UCHAR PlainBuffer[0x600];
        PCONTEXT Plain;

        Length = sizeof(PlainBuffer);
        Ret = pInitializeContext(PlainBuffer, CONTEXT_FULL, &Plain, &Length);
        ok(Ret, "InitializeContext failed with %lu\n", GetLastError());
        SetLastError(0xdeadbeef);
        Ret = pCopyContext(Plain, CONTEXT_FULL | CONTEXT_XSTATE, Context);
        ok(!Ret, "CopyContext succeeded\n");
        ok_err(ERROR_MORE_DATA);
    }

    HeapFree(GetProcessHeap(), 0, Buffer);
}

static volatile LONG AvxPhase;
static DECLSPEC_ALIGN(32) UCHAR AvxIn[32];
static DECLSPEC_ALIGN(32) UCHAR AvxOut[32];

#ifdef __GNUC__
static DWORD WINAPI
AvxThread(PVOID Parameter)
{
    /* Keep ymm7 loaded across the suspension, then report it */
    __asm__ __volatile__(
        "vmovdqu (%0), %%ymm7\n\t"
        "movl $1, (%2)\n\t"
        "1:\n\t"
        "pause\n\t"
        "cmpl $2, (%2)\n\t"
        "jne 1b\n\t"
        "vmovdqu %%ymm7, (%1)\n\t"
        "vzeroupper\n\t"
        :
        : "r"(AvxIn), "r"(AvxOut), "r"(&AvxPhase)
        : "memory", "xmm7");
    return 0;
}

static
VOID
TestThreadXState(VOID)
{
    PCONTEXT Context = NULL;
    PUCHAR Buffer, Avx;
    HANDLE Thread;
    DWORD Length = 0;
    ULONG i;
    BOOL Ret;

    for (i = 0; i < sizeof(AvxIn); i++)
        AvxIn[i] = (UCHAR)(i + 1);

    pInitializeContext(NULL, CONTEXT_FULL | CONTEXT_XSTATE, &Context, &Length);
    Buffer = HeapAlloc(GetProcessHeap(), 0, Length);
    if (!Buffer)
    {
        skip("Out of memory\n");
        return;
    }

    Ret = pInitializeContext(Buffer, CONTEXT_FULL | CONTEXT_XSTATE, &Context, &Length);
    ok(Ret, "InitializeContext failed with %lu\n", GetLastError());
    ok(pSetXStateFeaturesMask(Context, XSTATE_MASK_AVX), "SetXStateFeaturesMask failed\n");

    AvxPhase = 0;
    Thread = CreateThread(NULL, 0, AvxThread, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (!Thread)
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return;
    }

    while (AvxPhase != 1)
        Sleep(1);

    ok(SuspendThread(Thread) != (DWORD)-1, "SuspendThread failed with %lu\n", GetLastError());

    Ret = GetThreadContext(Thread, Context);
    ok(Ret, "GetThreadContext failed with %lu\n", GetLastError());

    /* The lower half comes with the legacy state, the upper with AVX */
    ok(!memcmp(&Context->Xmm7, AvxIn, 16), "Wrong xmm7\n");
    Avx = pLocateXStateFeature(Context, XSTATE_AVX, NULL);
    ok(Avx != NULL, "AVX not located\n");
    if (Avx)
    {
        DWORD64 Mask = 0;

        ok(pGetXStateFeaturesMask(Context, &Mask), "GetXStateFeaturesMask failed\n");
        ok(Mask & XSTATE_MASK_AVX, "AVX state reported as initial, mask %I64x\n", Mask);
        ok(!memcmp(Avx + 7 * 16, AvxIn + 16, 16), "Wrong upper half of ymm7\n");

        /* Change the upper half and give it back */
        for (i = 0; i < 16; i++)
            Avx[7 * 16 + i] = (UCHAR)(0xa0 + i);
        Ret = SetThreadContext(Thread, Context);
        ok(Ret, "SetThreadContext failed with %lu\n", GetLastError());
    }

    InterlockedExchange((PLONG)&AvxPhase, 2);
    ResumeThread(Thread);
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "Thread did not finish\n");
    CloseHandle(Thread);

    ok(!memcmp(AvxOut, AvxIn, 16), "Lower half of ymm7 changed\n");
    for (i = 0; i < 16; i++)
    {
        if (AvxOut[16 + i] != (UCHAR)(0xa0 + i))
            break;
    }
    ok(i == 16, "Upper half of ymm7 not set, byte %lu is %02x\n", i, i < 16 ? AvxOut[16 + i] : 0);

    HeapFree(GetProcessHeap(), 0, Buffer);
}
#endif /* __GNUC__ */

#endif /* _M_AMD64 */

START_TEST(XState)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    ULONG64 Features;

    pGetEnabledXStateFeatures = (FN_GetEnabledXStateFeatures)GetProcAddress(Kernel32, "GetEnabledXStateFeatures");
    pInitializeContext = (FN_InitializeContext)GetProcAddress(Kernel32, "InitializeContext");
    pInitializeContext2 = (FN_InitializeContext2)GetProcAddress(Kernel32, "InitializeContext2");
    pCopyContext = (FN_CopyContext)GetProcAddress(Kernel32, "CopyContext");
    pLocateXStateFeature = (FN_LocateXStateFeature)GetProcAddress(Kernel32, "LocateXStateFeature");
    pGetXStateFeaturesMask = (FN_GetXStateFeaturesMask)GetProcAddress(Kernel32, "GetXStateFeaturesMask");
    pSetXStateFeaturesMask = (FN_SetXStateFeaturesMask)GetProcAddress(Kernel32, "SetXStateFeaturesMask");
    pRtlGetEnabledExtendedFeatures = (FN_RtlGetEnabledExtendedFeatures)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetEnabledExtendedFeatures");

    if (!pGetEnabledXStateFeatures || !pInitializeContext || !pCopyContext ||
        !pLocateXStateFeature || !pGetXStateFeaturesMask || !pSetXStateFeaturesMask ||
        !pRtlGetEnabledExtendedFeatures)
    {
        ok(GetNTVersion() < _WIN32_WINNT_WIN7, "XState functions are not exported\n");
        return;
    }

    /* The enabled features come from the processor configuration */
    Features = pRtlGetEnabledExtendedFeatures(~(ULONG64)0);
    ok(Features == (SharedUserData->XState.EnabledFeatures |
                    SharedUserData->XState.EnabledUserVisibleSupervisorFeatures),
       "Features %I64x\n", Features);
    ok(pRtlGetEnabledExtendedFeatures(XSTATE_MASK_LEGACY) == (Features & XSTATE_MASK_LEGACY),
       "Masked features wrong\n");
    ok(pGetEnabledXStateFeatures() == (Features ? Features : XSTATE_MASK_LEGACY),
       "GetEnabledXStateFeatures %I64x, features %I64x\n", pGetEnabledXStateFeatures(), Features);

#ifdef _M_AMD64
    TestPlainContext();

    if (!Features)
    {
        DECLSPEC_ALIGN(16) UCHAR Buffer[0x600];
        PCONTEXT Context;
        DWORD Length = sizeof(Buffer);

        /* Without XSAVE an XSTATE request falls back to a plain context */
        ok(pInitializeContext(Buffer, CONTEXT_FULL | CONTEXT_XSTATE, &Context, &Length),
           "InitializeContext failed with %lu\n", GetLastError());
        ok_hex(Context->ContextFlags, CONTEXT_FULL);
        skip("XSAVE is not enabled\n");
        return;
    }

    TestXStateContext(Features);

#ifdef __GNUC__
    if (Features & XSTATE_MASK_AVX)
        TestThreadXState();
    else
        skip("AVX is not enabled\n");
#endif
#endif /* _M_AMD64 */
}
