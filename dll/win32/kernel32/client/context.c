/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Extended (XSTATE) thread context functions
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/* INCLUDES *****************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

#if (_WIN32_WINNT >= 0x0601)

/* GLOBALS ******************************************************************/

#define K32_CONTEXT_I386            0x00010000
#define K32_CONTEXT_AMD64           0x00100000
#define K32_CONTEXT_XSTATE_FLAG     0x00000040
#define K32_CONTEXT_EXTENDED_REGISTERS (K32_CONTEXT_I386 | 0x00000020)
#define K32_CONTEXT_FLOATING_POINT  (K32_CONTEXT_AMD64 | 0x00000008)

#ifdef _WIN64
#define K32_CONTEXT_NATIVE          K32_CONTEXT_AMD64
#else
#define K32_CONTEXT_NATIVE          K32_CONTEXT_I386
#endif

#define K32_I386_CONTEXT_LENGTH     0x2cc
#define K32_I386_EXTENDED_OFFSET    0xcc
#define K32_AMD64_CONTEXT_LENGTH    0x4d0
#define K32_AMD64_FLTSAVE_OFFSET    0x100

/* PRIVATE FUNCTIONS ********************************************************/

/* The ContextFlags of a native CONTEXT: at 0x30 for amd64, first for i386 */
static
ULONG
BasepGetContextFlags(
    _In_ PCONTEXT Context,
    _Out_ PBOOLEAN IsI386)
{
#ifdef _WIN64
    *IsI386 = FALSE;
    return *(PULONG)((PUCHAR)Context + 0x30);
#else
    *IsI386 = TRUE;
    return *(PULONG)Context;
#endif
}

/* Returns the CONTEXT_EX of a context that carries CONTEXT_XSTATE, or NULL */
static
PCONTEXT_EX
BasepGetContextEx(
    _In_ PCONTEXT Context,
    _In_ ULONG Flags,
    _In_ BOOLEAN IsI386)
{
    if (IsI386)
    {
        if ((Flags & (K32_CONTEXT_I386 | K32_CONTEXT_XSTATE_FLAG)) !=
            (K32_CONTEXT_I386 | K32_CONTEXT_XSTATE_FLAG))
        {
            return NULL;
        }
        return (PCONTEXT_EX)((PUCHAR)Context + K32_I386_CONTEXT_LENGTH);
    }

    if ((Flags & (K32_CONTEXT_AMD64 | K32_CONTEXT_XSTATE_FLAG)) !=
        (K32_CONTEXT_AMD64 | K32_CONTEXT_XSTATE_FLAG))
    {
        return NULL;
    }
    return (PCONTEXT_EX)((PUCHAR)Context + K32_AMD64_CONTEXT_LENGTH);
}

/* PUBLIC FUNCTIONS *********************************************************/

/*
 * @implemented
 */
DWORD64
WINAPI
GetEnabledXStateFeatures(VOID)
{
    DWORD64 Features;

    Features = RtlGetEnabledExtendedFeatures(~(ULONG64)0);

    /* x87 and SSE state are always there, even without XSAVE */
    return Features ? Features : XSTATE_MASK_LEGACY;
}

static
BOOL
BasepInitializeContext(
    _Out_writes_bytes_opt_(*ContextLength) PVOID Buffer,
    _In_ DWORD ContextFlags,
    _Out_ PCONTEXT *Context,
    _Inout_ PDWORD ContextLength,
    _In_ ULONG64 XStateCompactionMask)
{
    PCONTEXT_EX ContextEx;
    ULONG Length;
    NTSTATUS Status;

    /* Without XSAVE support, an XSTATE request falls back to a plain context */
    if ((RtlGetEnabledExtendedFeatures(~(ULONG64)0) == 0) &&
        (ContextFlags & (K32_CONTEXT_I386 | K32_CONTEXT_AMD64)))
    {
        ContextFlags &= ~K32_CONTEXT_XSTATE_FLAG;
    }

#if (_WIN32_WINNT >= 0x0A00)
    Status = RtlGetExtendedContextLength2(ContextFlags, &Length, XStateCompactionMask);
#else
    /* Before Windows 10 the context always covered every enabled feature */
    ASSERT(XStateCompactionMask == RtlGetEnabledExtendedFeatures(~(ULONG64)0));
    Status = RtlGetExtendedContextLength(ContextFlags, &Length);
#endif
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if ((Buffer == NULL) || (*ContextLength < Length))
    {
        *ContextLength = Length;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

#if (_WIN32_WINNT >= 0x0A00)
    Status = RtlInitializeExtendedContext2(Buffer, ContextFlags, &ContextEx, XStateCompactionMask);
#else
    Status = RtlInitializeExtendedContext(Buffer, ContextFlags, &ContextEx);
#endif
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    *Context = RtlLocateLegacyContext(ContextEx, NULL);
    *ContextLength = Length;
    return TRUE;
}

#if (_WIN32_WINNT >= 0x0A00)
/*
 * @implemented
 */
BOOL
WINAPI
InitializeContext2(
    _Out_writes_bytes_opt_(*ContextLength) PVOID Buffer,
    _In_ DWORD ContextFlags,
    _Out_ PCONTEXT *Context,
    _Inout_ PDWORD ContextLength,
    _In_ ULONG64 XStateCompactionMask)
{
    return BasepInitializeContext(Buffer,
                                  ContextFlags,
                                  Context,
                                  ContextLength,
                                  XStateCompactionMask);
}
#endif /* _WIN32_WINNT >= 0x0A00 */

/*
 * @implemented
 */
BOOL
WINAPI
InitializeContext(
    _Out_writes_bytes_opt_(*ContextLength) PVOID Buffer,
    _In_ DWORD ContextFlags,
    _Out_ PCONTEXT *Context,
    _Inout_ PDWORD ContextLength)
{
    /* Lay the context out for every enabled feature */
    return BasepInitializeContext(Buffer,
                                  ContextFlags,
                                  Context,
                                  ContextLength,
                                  RtlGetEnabledExtendedFeatures(~(ULONG64)0));
}

/*
 * @implemented
 */
BOOL
WINAPI
CopyContext(
    _Inout_ PCONTEXT Destination,
    _In_ DWORD ContextFlags,
    _In_ PCONTEXT Source)
{
    NTSTATUS Status;

    Status = RtlCopyContext(Destination, ContextFlags, Source);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetXStateFeaturesMask(
    _In_ PCONTEXT Context,
    _Out_ PDWORD64 FeatureMask)
{
    PCONTEXT_EX ContextEx;
    BOOLEAN IsI386;
    DWORD64 Mask = 0;
    ULONG Flags;

    /* Like Windows, only a context of the native architecture is accepted;
       anything else fails without touching the last error */
    Flags = BasepGetContextFlags(Context, &IsI386);
    if (!(Flags & K32_CONTEXT_NATIVE))
        return FALSE;

    ContextEx = BasepGetContextEx(Context, Flags, IsI386);

    /* The legacy x87/SSE state travels in the CONTEXT itself */
    if (IsI386)
    {
        if ((Flags & K32_CONTEXT_EXTENDED_REGISTERS) == K32_CONTEXT_EXTENDED_REGISTERS)
            Mask = XSTATE_MASK_LEGACY;
    }
    else
    {
        if ((Flags & K32_CONTEXT_FLOATING_POINT) == K32_CONTEXT_FLOATING_POINT)
            Mask = XSTATE_MASK_LEGACY;
    }

    if (ContextEx)
        Mask |= RtlGetExtendedFeaturesMask(ContextEx);

    *FeatureMask = Mask;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetXStateFeaturesMask(
    _Inout_ PCONTEXT Context,
    _In_ DWORD64 FeatureMask)
{
    PCONTEXT_EX ContextEx;
    BOOLEAN IsI386;
    PULONG FlagsLocation;
    ULONG Flags;

    /* Only a context of the native architecture is accepted */
    Flags = BasepGetContextFlags(Context, &IsI386);
    if (!(Flags & K32_CONTEXT_NATIVE))
        return FALSE;

    FlagsLocation = IsI386 ? (PULONG)Context : (PULONG)((PUCHAR)Context + 0x30);
    ContextEx = BasepGetContextEx(Context, Flags, IsI386);

    /* Asking for the legacy state selects the part of CONTEXT carrying it */
    if (FeatureMask & XSTATE_MASK_LEGACY)
    {
        if (IsI386)
        {
            *FlagsLocation = Flags | K32_CONTEXT_EXTENDED_REGISTERS;

            /* The legacy chunk now includes the extended registers */
            if (ContextEx && (ContextEx->Legacy.Length < K32_I386_CONTEXT_LENGTH))
                ContextEx->Legacy.Length = K32_I386_CONTEXT_LENGTH;
        }
        else
        {
            *FlagsLocation = Flags | K32_CONTEXT_FLOATING_POINT;
        }
    }

    if (ContextEx == NULL)
    {
        /* Without an XState chunk only the legacy features can be selected */
        if (FeatureMask & ~XSTATE_MASK_LEGACY)
        {
            BaseSetLastNTError(STATUS_INVALID_PARAMETER);
            return FALSE;
        }
        return TRUE;
    }

    RtlSetExtendedFeaturesMask(ContextEx, FeatureMask);
    return TRUE;
}

/*
 * @implemented
 */
PVOID
WINAPI
LocateXStateFeature(
    _In_ PCONTEXT Context,
    _In_ DWORD FeatureId,
    _Out_opt_ PDWORD Length)
{
    PCONTEXT_EX ContextEx;
    BOOLEAN IsI386;
    PUCHAR LegacyArea;
    ULONG Flags;

    /* Only a context of the native architecture is accepted */
    Flags = BasepGetContextFlags(Context, &IsI386);
    if (!(Flags & K32_CONTEXT_NATIVE))
        return NULL;

    ContextEx = BasepGetContextEx(Context, Flags, IsI386);

    /* The legacy XSAVE region: i386 ExtendedRegisters, amd64 FltSave */
    LegacyArea = (PUCHAR)Context +
                 (IsI386 ? K32_I386_EXTENDED_OFFSET : K32_AMD64_FLTSAVE_OFFSET);

    if (FeatureId == XSTATE_LEGACY_FLOATING_POINT)
    {
        if (Length) *Length = FIELD_OFFSET(XSAVE_FORMAT, XmmRegisters);
        return LegacyArea;
    }

    if (FeatureId == XSTATE_LEGACY_SSE)
    {
        /* i386 has 8 XMM registers, amd64 has 16 */
        if (Length) *Length = (IsI386 ? 8 : 16) * sizeof(M128A);
        return LegacyArea + FIELD_OFFSET(XSAVE_FORMAT, XmmRegisters);
    }

    if (ContextEx == NULL)
        return NULL;

    return RtlLocateExtendedFeature(ContextEx, FeatureId, (PULONG)Length);
}

#endif /* _WIN32_WINNT >= 0x0601 */

/* EOF */
