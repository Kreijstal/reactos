/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Extended processor state (XSTATE) context helpers
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * These routines lay out, locate and copy the CONTEXT_EX structure that
 * follows a CONTEXT when CONTEXT_XSTATE is requested.  The layout follows
 * the processor's XSAVE format as described by SharedUserData->XState:
 *
 *   CONTEXT | CONTEXT_EX | pad to 64 | XSAVE_AREA_HEADER | extended components
 *
 * The legacy x87/SSE region of the XSAVE image is not part of the XState
 * chunk; it lives in CONTEXT.FltSave (amd64) / ExtendedRegisters (i386).
 * Component offsets inside the chunk are therefore the XSAVE offsets
 * minus sizeof(XSAVE_FORMAT).
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define CONTEXT_FLAGS_ARCH_I386     0x00010000
#define CONTEXT_FLAGS_ARCH_AMD64    0x00100000
#define CONTEXT_FLAGS_XSTATE        0x00000040
#define CONTEXT_FLAGS_EXTENDED      0x00000020
#define CONTEXT_FLAGS_EXCEPTION_REQUEST 0x40000000

/* Bits RtlpValidateContextFlags reports back */
#define RTLP_CONTEXT_VALID          0x1
#define RTLP_CONTEXT_HAS_XSTATE     0x2

/* Legacy (CONTEXT) sizes and the alignment of the context records */
#define I386_CONTEXT_LENGTH         0x2cc
#define I386_CONTEXT_ALIGN          4
#define I386_EXTENDED_OFFSET        0xcc
#define AMD64_CONTEXT_LENGTH        0x4d0
#define AMD64_CONTEXT_ALIGN         0x10

/* sizeof(XSAVE_FORMAT) + sizeof(XSAVE_AREA_HEADER) */
#define XSAVE_LEGACY_LENGTH         0x200
#define XSAVE_HEADER_LENGTH         0x40
#define XSAVE_COMPONENTS_OFFSET     (XSAVE_LEGACY_LENGTH + XSAVE_HEADER_LENGTH)


C_ASSERT(sizeof(CONTEXT_EX) == 0x18);
C_ASSERT(sizeof(XSAVE_AREA_HEADER) == XSAVE_HEADER_LENGTH);
C_ASSERT(sizeof(XSAVE_FORMAT) == XSAVE_LEGACY_LENGTH);
#ifdef _M_AMD64
C_ASSERT(sizeof(CONTEXT) == AMD64_CONTEXT_LENGTH);
#endif

/* PRIVATE FUNCTIONS ********************************************************/

static
ULONG64
RtlpGetUserVisibleFeatures(VOID)
{
    return SharedUserData->XState.EnabledFeatures |
           SharedUserData->XState.EnabledUserVisibleSupervisorFeatures;
}

static
BOOLEAN
RtlpIsCompactionEnabled(VOID)
{
    return SharedUserData->XState.CompactionEnabled != 0;
}

/*
 * Accepts an i386 or amd64 CONTEXT flags value. Returns RTLP_CONTEXT_VALID,
 * plus RTLP_CONTEXT_HAS_XSTATE when CONTEXT_XSTATE is requested. Requesting
 * XSTATE on a system without XSAVE support fails with STATUS_NOT_SUPPORTED.
 */
static
NTSTATUS
RtlpValidateContextFlags(
    _In_ ULONG ContextFlags,
    _Out_opt_ PULONG Result)
{
    ULONG Valid = RTLP_CONTEXT_VALID;

    if (((ContextFlags & 0x27ffff80) != CONTEXT_FLAGS_ARCH_I386) &&
        ((ContextFlags & 0x27ffffa0) != CONTEXT_FLAGS_ARCH_AMD64))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (((ContextFlags & (CONTEXT_FLAGS_ARCH_AMD64 | CONTEXT_FLAGS_XSTATE)) ==
         (CONTEXT_FLAGS_ARCH_AMD64 | CONTEXT_FLAGS_XSTATE)) ||
        ((ContextFlags & (CONTEXT_FLAGS_ARCH_I386 | CONTEXT_FLAGS_XSTATE)) ==
         (CONTEXT_FLAGS_ARCH_I386 | CONTEXT_FLAGS_XSTATE)))
    {
        if (SharedUserData->XState.EnabledFeatures == 0)
            return STATUS_NOT_SUPPORTED;

        Valid |= RTLP_CONTEXT_HAS_XSTATE;
    }

    if (Result) *Result = Valid;
    return STATUS_SUCCESS;
}

static
VOID
RtlpGetLegacyContextLength(
    _In_ ULONG ContextFlags,
    _Out_ PULONG Length,
    _Out_ PULONG Alignment)
{
    if (ContextFlags & CONTEXT_FLAGS_ARCH_I386)
    {
        *Length = I386_CONTEXT_LENGTH;
        *Alignment = I386_CONTEXT_ALIGN;
    }
    else
    {
        ASSERT(ContextFlags & CONTEXT_FLAGS_ARCH_AMD64);
        *Length = AMD64_CONTEXT_LENGTH;
        *Alignment = AMD64_CONTEXT_ALIGN;
    }
}

static
PULONG
RtlpGetContextFlagsLocation(
    _In_ PVOID Context,
    _In_ ULONG ContextFlags)
{
    /* i386 keeps ContextFlags at offset 0, amd64 at offset 0x30 */
    if (ContextFlags & CONTEXT_FLAGS_ARCH_I386)
        return (PULONG)Context;

    return (PULONG)((PUCHAR)Context + 0x30);
}

/*
 * Size of the whole XSAVE image (legacy region included) needed for the
 * given compaction mask.
 */
static
ULONG
RtlpGetEntireXStateAreaLength(
    _In_ ULONG64 CompactionMask)
{
    ULONG Length, Feature;

    if (!RtlpIsCompactionEnabled())
        return SharedUserData->XState.Size;

    Length = XSAVE_COMPONENTS_OFFSET;
    for (Feature = 2; Feature < MAXIMUM_XSTATE_FEATURES; Feature++)
    {
        ULONG64 FeatureBit = 1ULL << Feature;

        if (!(CompactionMask & FeatureBit))
            continue;

        if (SharedUserData->XState.AlignedFeatures & FeatureBit)
            Length = ALIGN_UP_BY(Length, 64);

        Length += SharedUserData->XState.AllFeatures[Feature];
    }

    return Length;
}

/* Returns the XState chunk, or NULL if it does not lie inside the All chunk */
static
PXSAVE_AREA_HEADER
RtlpLocateXStateChunk(
    _In_ PCONTEXT_EX ContextEx)
{
    if ((ContextEx->All.Offset > ContextEx->XState.Offset) ||
        ((LONG)(ContextEx->All.Offset + ContextEx->All.Length) <
         (LONG)(ContextEx->XState.Offset + ContextEx->XState.Length)))
    {
        return NULL;
    }

    return (PXSAVE_AREA_HEADER)((PUCHAR)ContextEx + ContextEx->XState.Offset);
}

static
VOID
RtlpCopyRange(
    _Out_ PUCHAR Destination,
    _In_ const UCHAR *Source,
    _In_ ULONG Start,
    _In_ ULONG End)
{
    RtlCopyMemory(Destination + Start, Source + Start, End - Start);
}

static
VOID
RtlpCopyLegacyContextI386(
    _Out_ PUCHAR Destination,
    _In_ ULONG ContextFlags,
    _In_ const UCHAR *Source)
{
    PULONG DestinationFlags = (PULONG)Destination;

    *DestinationFlags = ContextFlags & 0x67ffffff;
    if (ContextFlags & CONTEXT_FLAGS_EXCEPTION_REQUEST)
        *DestinationFlags |= *(PULONG)Source & 0x98000000;

    /* CONTEXT_CONTROL: Ebp, Eip, SegCs, EFlags, Esp, SegSs */
    if ((ContextFlags & 0x10001) == 0x10001)
        RtlpCopyRange(Destination, Source, 0xb4, 0xcc);

    /* CONTEXT_INTEGER: Edi, Esi, Ebx, Edx, Ecx, Eax */
    if ((ContextFlags & 0x10002) == 0x10002)
        RtlpCopyRange(Destination, Source, 0x9c, 0xb4);

    /* CONTEXT_SEGMENTS: SegGs, SegFs, SegEs, SegDs */
    if ((ContextFlags & 0x10004) == 0x10004)
        RtlpCopyRange(Destination, Source, 0x8c, 0x9c);

    /* CONTEXT_FLOATING_POINT: FloatSave */
    if ((ContextFlags & 0x10008) == 0x10008)
        RtlpCopyRange(Destination, Source, 0x1c, 0x8c);

    /* CONTEXT_DEBUG_REGISTERS: Dr0-Dr3, Dr6, Dr7 */
    if ((ContextFlags & 0x10010) == 0x10010)
        RtlpCopyRange(Destination, Source, 0x04, 0x1c);

    /* CONTEXT_EXTENDED_REGISTERS: ExtendedRegisters */
    if ((ContextFlags & 0x10020) == 0x10020)
        RtlpCopyRange(Destination, Source, 0xcc, 0x2cc);
}

static
VOID
RtlpCopyLegacyContextAmd64(
    _Out_ PUCHAR Destination,
    _In_ ULONG ContextFlags,
    _In_ const UCHAR *Source)
{
    PULONG DestinationFlags = (PULONG)(Destination + 0x30);

    *DestinationFlags = ContextFlags & 0x67ffffff;
    if (ContextFlags & CONTEXT_FLAGS_EXCEPTION_REQUEST)
        *DestinationFlags |= *(PULONG)(Source + 0x30) & 0x98000000;

    /* CONTEXT_CONTROL: Rip, SegCs, SegSs, Rsp, EFlags */
    if ((ContextFlags & 0x100001) == 0x100001)
    {
        RtlpCopyRange(Destination, Source, 0xf8, 0x100);
        RtlpCopyRange(Destination, Source, 0x38, 0x3a);
        RtlpCopyRange(Destination, Source, 0x42, 0x48);
        RtlpCopyRange(Destination, Source, 0x98, 0xa0);
    }

    /* CONTEXT_INTEGER: Rax-Rbx, Rbp-R15 */
    if ((ContextFlags & 0x100002) == 0x100002)
    {
        RtlpCopyRange(Destination, Source, 0x78, 0x98);
        RtlpCopyRange(Destination, Source, 0xa0, 0xf8);
    }

    /* CONTEXT_SEGMENTS: SegDs, SegEs, SegFs, SegGs */
    if ((ContextFlags & 0x100004) == 0x100004)
        RtlpCopyRange(Destination, Source, 0x3a, 0x42);

    /* CONTEXT_FLOATING_POINT: MxCsr, FltSave legacy area and Xmm0-15 */
    if ((ContextFlags & 0x100008) == 0x100008)
    {
        RtlpCopyRange(Destination, Source, 0x34, 0x38);
        RtlpCopyRange(Destination, Source, 0x100, 0x2a0);
    }

    /* CONTEXT_DEBUG_REGISTERS: Dr0-Dr7, DebugControl and branch records */
    if ((ContextFlags & 0x100010) == 0x100010)
    {
        RtlpCopyRange(Destination, Source, 0x48, 0x78);
        RtlpCopyRange(Destination, Source, 0x4b0, 0x4d0);
    }
}

static
VOID
RtlpCopyLegacyContext(
    _Out_ PVOID Destination,
    _In_ ULONG ContextFlags,
    _In_ const VOID *Source)
{
    if (ContextFlags & CONTEXT_FLAGS_ARCH_I386)
        RtlpCopyLegacyContextI386(Destination, ContextFlags, Source);
    else
        RtlpCopyLegacyContextAmd64(Destination, ContextFlags, Source);
}

/*
 * Offset, relative to the XSAVE header, of a non-legacy component in a chunk
 * laid out for the given compaction mask (ignored without compaction).
 */
static
ULONG
RtlpGetXStateFeatureOffset(
    _In_ ULONG Feature,
    _In_ ULONG64 CompactionMask)
{
    ULONG Offset, Index;

    if (!RtlpIsCompactionEnabled())
        return SharedUserData->XState.Features[Feature].Offset - XSAVE_LEGACY_LENGTH;

    Offset = XSAVE_HEADER_LENGTH;
    for (Index = 2; Index <= Feature; Index++)
    {
        ULONG64 Bit = 1ULL << Index;

        if (!(CompactionMask & Bit))
            continue;

        if (SharedUserData->XState.AlignedFeatures & Bit)
            Offset = ALIGN_UP_BY(Offset, 64);

        if (Index == Feature)
            break;

        Offset += SharedUserData->XState.AllFeatures[Index];
    }

    return Offset;
}

/*
 * Copies the non-legacy components present in the source chunk into the
 * destination chunk. Each chunk is laid out the way the processor does it
 * (standard, or compacted per its own CompactionMask). The destination keeps
 * its layout: a component it has no room for is not copied, and its Mask
 * tells which ones were. Shared with the kernel's get/set context path.
 */
NTSTATUS
NTAPI
RtlpCopyXStateChunk(
    _Out_writes_bytes_(DestinationLength) PXSAVE_AREA_HEADER Destination,
    _In_ ULONG DestinationLength,
    _In_reads_bytes_(SourceLength) const XSAVE_AREA_HEADER *Source,
    _In_ ULONG SourceLength)
{
    ULONG64 FeaturesToCopy, SourceCompaction, DestinationCompaction;
    ULONG Feature, Size, SourceOffset, DestinationOffset;

    if ((DestinationLength < XSAVE_HEADER_LENGTH) ||
        (SourceLength < XSAVE_HEADER_LENGTH))
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    FeaturesToCopy = RtlpGetUserVisibleFeatures() & Source->Mask & ~XSTATE_MASK_LEGACY;

    if (RtlpIsCompactionEnabled())
    {
        SourceCompaction = Source->CompactionMask;
        DestinationCompaction = (Destination->CompactionMask & RtlpGetUserVisibleFeatures() &
                                 ~XSTATE_MASK_LEGACY) |
                                ((ULONG64)XSTATE_COMPACTION_ENABLE_MASK);
        FeaturesToCopy &= SourceCompaction & DestinationCompaction;
    }
    else
    {
        SourceCompaction = DestinationCompaction = 0;
    }

    /* Bytes 16-63 of the header are reserved and must be zero */
    RtlZeroMemory(Destination, XSAVE_HEADER_LENGTH);
    Destination->Mask = FeaturesToCopy;
    Destination->CompactionMask = DestinationCompaction;

    for (Feature = 2; (Feature < MAXIMUM_XSTATE_FEATURES) && (FeaturesToCopy != 0); Feature++)
    {
        ULONG64 FeatureBit = 1ULL << Feature;

        if (!(FeaturesToCopy & FeatureBit))
            continue;

        Size = RtlpIsCompactionEnabled() ? SharedUserData->XState.AllFeatures[Feature] :
                                           SharedUserData->XState.Features[Feature].Size;
        SourceOffset = RtlpGetXStateFeatureOffset(Feature, SourceCompaction);
        DestinationOffset = RtlpGetXStateFeatureOffset(Feature, DestinationCompaction);

        if ((SourceOffset + Size > SourceLength) ||
            (DestinationOffset + Size > DestinationLength))
        {
            return STATUS_BUFFER_OVERFLOW;
        }

        RtlMoveMemory((PUCHAR)Destination + DestinationOffset,
                      (const UCHAR *)Source + SourceOffset,
                      Size);
        FeaturesToCopy &= ~FeatureBit;
    }

    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS *********************************************************/

ULONG64
NTAPI
RtlGetEnabledExtendedFeatures(
    _In_ ULONG64 FeatureMask)
{
    return RtlpGetUserVisibleFeatures() & FeatureMask;
}

NTSTATUS
NTAPI
RtlGetExtendedContextLength2(
    _In_ ULONG ContextFlags,
    _Out_ PULONG ContextLength,
    _In_ ULONG64 XStateCompactionMask)
{
    ULONG Valid, LegacyLength, Alignment, Length;
    NTSTATUS Status;

    Status = RtlpValidateContextFlags(ContextFlags, &Valid);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlpGetLegacyContextLength(ContextFlags, &LegacyLength, &Alignment);

    if (!(Valid & RTLP_CONTEXT_HAS_XSTATE))
    {
        /* Room to align the context record, the record and the CONTEXT_EX */
        *ContextLength = (Alignment - 1) + LegacyLength + sizeof(CONTEXT_EX);
        return STATUS_SUCCESS;
    }

    if (RtlpIsCompactionEnabled())
    {
        XStateCompactionMask &= RtlpGetUserVisibleFeatures() |
                                ((ULONG64)XSTATE_COMPACTION_ENABLE_MASK);
    }

    /* The aligned record and CONTEXT_EX, room to align the XSAVE header to
       64 bytes, then the header and the non-legacy XSAVE components */
    Length = ALIGN_UP_BY(LegacyLength + sizeof(CONTEXT_EX), Alignment);
    Length += (64 - 1) +
              RtlpGetEntireXStateAreaLength(XStateCompactionMask) - XSAVE_LEGACY_LENGTH;

    *ContextLength = Length;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlGetExtendedContextLength(
    _In_ ULONG ContextFlags,
    _Out_ PULONG ContextLength)
{
    ULONG64 CompactionMask = 0;
    ULONG Valid;
    NTSTATUS Status;

    Status = RtlpValidateContextFlags(ContextFlags, &Valid);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Valid & RTLP_CONTEXT_HAS_XSTATE)
        CompactionMask = RtlpGetUserVisibleFeatures();

    return RtlGetExtendedContextLength2(ContextFlags, ContextLength, CompactionMask);
}

NTSTATUS
NTAPI
RtlInitializeExtendedContext2(
    _Out_ PVOID Context,
    _In_ ULONG ContextFlags,
    _Out_ PCONTEXT_EX *ContextEx,
    _In_ ULONG64 XStateCompactionMask)
{
    ULONG Valid, LegacyLength, Alignment;
    PUCHAR AlignedContext;
    PCONTEXT_EX Ex;
    NTSTATUS Status;

    Status = RtlpValidateContextFlags(ContextFlags, &Valid);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlpGetLegacyContextLength(ContextFlags, &LegacyLength, &Alignment);

    AlignedContext = ALIGN_UP_POINTER_BY(Context, Alignment);
    *RtlpGetContextFlagsLocation(AlignedContext, ContextFlags) = ContextFlags;

    Ex = (PCONTEXT_EX)(AlignedContext + LegacyLength);
    Ex->All.Offset = -(LONG)LegacyLength;
    Ex->All.Length = LegacyLength + sizeof(CONTEXT_EX);
    Ex->Legacy.Offset = -(LONG)LegacyLength;
    Ex->Legacy.Length = LegacyLength;

    /* An i386 context without extended registers ends before them */
    if ((ContextFlags & CONTEXT_FLAGS_ARCH_I386) &&
        ((ContextFlags & (CONTEXT_FLAGS_ARCH_I386 | CONTEXT_FLAGS_EXTENDED)) !=
         (CONTEXT_FLAGS_ARCH_I386 | CONTEXT_FLAGS_EXTENDED)))
    {
        Ex->Legacy.Length = I386_EXTENDED_OFFSET;
    }

    if (Valid & RTLP_CONTEXT_HAS_XSTATE)
    {
        PXSAVE_AREA_HEADER Header;
        ULONG XStateLength;

        Header = ALIGN_UP_POINTER_BY((PUCHAR)Ex + sizeof(CONTEXT_EX), 64);

        if (RtlpIsCompactionEnabled())
        {
            XStateCompactionMask &= RtlpGetUserVisibleFeatures() |
                                    ((ULONG64)XSTATE_COMPACTION_ENABLE_MASK);
        }

        XStateLength = RtlpGetEntireXStateAreaLength(XStateCompactionMask) -
                       XSAVE_LEGACY_LENGTH;
        RtlZeroMemory(Header, XStateLength);

        if (RtlpIsCompactionEnabled())
            Header->CompactionMask = XStateCompactionMask | ((ULONG64)XSTATE_COMPACTION_ENABLE_MASK);

        Ex->XState.Offset = (LONG)((PUCHAR)Header - (PUCHAR)Ex);
        Ex->XState.Length = XStateLength;
        Ex->All.Length = Ex->XState.Offset - Ex->All.Offset + XStateLength;
    }
    else
    {
        /* This is what Windows reports when there is no XState chunk */
        Ex->XState.Offset = 0x19;
        Ex->XState.Length = 0;
    }

    *ContextEx = Ex;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlInitializeExtendedContext(
    _Out_ PVOID Context,
    _In_ ULONG ContextFlags,
    _Out_ PCONTEXT_EX *ContextEx)
{
    ULONG64 CompactionMask = 0;
    ULONG Valid;
    NTSTATUS Status;

    Status = RtlpValidateContextFlags(ContextFlags, &Valid);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Valid & RTLP_CONTEXT_HAS_XSTATE)
        CompactionMask = RtlpGetUserVisibleFeatures();

    return RtlInitializeExtendedContext2(Context, ContextFlags, ContextEx, CompactionMask);
}

PVOID
NTAPI
RtlLocateLegacyContext(
    _In_ PCONTEXT_EX ContextEx,
    _Out_opt_ PULONG Length)
{
    if ((ContextEx->All.Offset > ContextEx->Legacy.Offset) ||
        ((LONG)(ContextEx->All.Offset + ContextEx->All.Length) <
         (LONG)(ContextEx->Legacy.Offset + ContextEx->Legacy.Length)))
    {
        return NULL;
    }

    if (Length) *Length = ContextEx->Legacy.Length;
    return (PUCHAR)ContextEx + ContextEx->Legacy.Offset;
}

PVOID
NTAPI
RtlLocateExtendedFeature2(
    _In_ PCONTEXT_EX ContextEx,
    _In_ ULONG FeatureId,
    _In_ PXSTATE_CONFIGURATION XStateConfig,
    _Out_opt_ PULONG Length)
{
    ULONG64 FeatureBit;
    PXSAVE_AREA_HEADER Header;
    ULONG Offset;

    if ((FeatureId < 2) || (FeatureId >= MAXIMUM_XSTATE_FEATURES))
        return NULL;

    FeatureBit = 1ULL << FeatureId;
    if (!(FeatureBit & (XStateConfig->EnabledFeatures |
                        XStateConfig->EnabledUserVisibleSupervisorFeatures)))
    {
        return NULL;
    }

    /* Only the control flags known to this layout are supported */
    if (XStateConfig->ControlFlags & ~7)
        return NULL;

    Header = RtlpLocateXStateChunk(ContextEx);
    if (Header == NULL)
        return NULL;

    if (Length)
    {
        *Length = XStateConfig->CompactionEnabled ?
                  XStateConfig->AllFeatures[FeatureId] :
                  XStateConfig->Features[FeatureId].Size;
    }

    if (XStateConfig->CompactionEnabled)
    {
        ULONG Feature;

        if (!(Header->CompactionMask & FeatureBit))
            return NULL;

        Offset = XSAVE_COMPONENTS_OFFSET;
        for (Feature = 2; Feature < FeatureId; Feature++)
        {
            ULONG64 Bit = 1ULL << Feature;

            if (!(Header->CompactionMask & Bit))
                continue;

            if (XStateConfig->AlignedFeatures & Bit)
                Offset = ALIGN_UP_BY(Offset, 64);
            Offset += XStateConfig->AllFeatures[Feature];
        }

        if (XStateConfig->AlignedFeatures & FeatureBit)
            Offset = ALIGN_UP_BY(Offset, 64);
    }
    else
    {
        Offset = XStateConfig->Features[FeatureId].Offset;
    }

    return (PUCHAR)Header + Offset - XSAVE_LEGACY_LENGTH;
}

PVOID
NTAPI
RtlLocateExtendedFeature(
    _In_ PCONTEXT_EX ContextEx,
    _In_ ULONG FeatureId,
    _Out_opt_ PULONG Length)
{
    return RtlLocateExtendedFeature2(ContextEx,
                                     FeatureId,
                                     (PXSTATE_CONFIGURATION)&SharedUserData->XState,
                                     Length);
}

ULONG64
NTAPI
RtlGetExtendedFeaturesMask(
    _In_ PCONTEXT_EX ContextEx)
{
    PXSAVE_AREA_HEADER Header = RtlpLocateXStateChunk(ContextEx);

    /* Callers must pass a context initialized with CONTEXT_XSTATE */
    ASSERT(Header != NULL);
    return Header->Mask & ~XSTATE_MASK_LEGACY;
}

VOID
NTAPI
RtlSetExtendedFeaturesMask(
    _Inout_ PCONTEXT_EX ContextEx,
    _In_ ULONG64 FeatureMask)
{
    PXSAVE_AREA_HEADER Header = RtlpLocateXStateChunk(ContextEx);

    /* Callers must pass a context initialized with CONTEXT_XSTATE */
    ASSERT(Header != NULL);
    Header->Mask = RtlpGetUserVisibleFeatures() & FeatureMask & ~XSTATE_MASK_LEGACY;
}

NTSTATUS
NTAPI
RtlCopyExtendedContext(
    _Out_ PCONTEXT_EX Destination,
    _In_ ULONG ContextFlags,
    _In_ PCONTEXT_EX Source)
{
    ULONG Valid;
    NTSTATUS Status;

    Status = RtlpValidateContextFlags(ContextFlags, &Valid);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Destination->Legacy.Offset != Source->Legacy.Offset)
        return STATUS_INVALID_PARAMETER;

    if (Destination->Legacy.Length < Source->Legacy.Length)
        return STATUS_INVALID_PARAMETER;

    RtlpCopyLegacyContext((PUCHAR)Destination + Destination->Legacy.Offset,
                          ContextFlags,
                          (PUCHAR)Source + Source->Legacy.Offset);

    if (Valid & RTLP_CONTEXT_HAS_XSTATE)
    {
        Status = RtlpCopyXStateChunk(
            (PXSAVE_AREA_HEADER)((PUCHAR)Destination + Destination->XState.Offset),
            Destination->XState.Length,
            (PXSAVE_AREA_HEADER)((PUCHAR)Source + Source->XState.Offset),
            Source->XState.Length);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlCopyContext(
    _Inout_ PCONTEXT Destination,
    _In_ ULONG ContextFlags,
    _In_ PCONTEXT Source)
{
    PULONG DestinationFlagsLocation;
    ULONG DestinationFlags, SourceFlags, CopyFlags;
    ULONG SourceValid, DestinationValid;
    NTSTATUS Status;

    Status = RtlpValidateContextFlags(ContextFlags, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    DestinationFlagsLocation = RtlpGetContextFlagsLocation(Destination, ContextFlags);
    DestinationFlags = *DestinationFlagsLocation;
    SourceFlags = *RtlpGetContextFlagsLocation(Source, ContextFlags);

    /* All three must describe the same architecture */
    Status = RtlpValidateContextFlags(DestinationFlags | SourceFlags | ContextFlags, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    CopyFlags = SourceFlags & ContextFlags;
    Status = RtlpValidateContextFlags(CopyFlags, &SourceValid);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RtlpValidateContextFlags(DestinationFlags, &DestinationValid);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The destination must have room for the XState it is asked to receive */
    if (SourceValid & ~DestinationValid)
        return STATUS_BUFFER_OVERFLOW;

    RtlpCopyLegacyContext(Destination, CopyFlags, Source);
    *DestinationFlagsLocation |= DestinationFlags;

    if (DestinationValid & RTLP_CONTEXT_HAS_XSTATE)
    {
        ULONG LegacyLength, Alignment;
        PCONTEXT_EX DestinationEx, SourceEx;

        RtlpGetLegacyContextLength(ContextFlags, &LegacyLength, &Alignment);
        DestinationEx = (PCONTEXT_EX)((PUCHAR)Destination + LegacyLength);
        SourceEx = (PCONTEXT_EX)((PUCHAR)Source + LegacyLength);

        /* An i386 destination without extended registers ends before them */
        if ((ContextFlags & CONTEXT_FLAGS_ARCH_I386) &&
            ((DestinationFlags & (CONTEXT_FLAGS_ARCH_I386 | CONTEXT_FLAGS_EXTENDED)) !=
             (CONTEXT_FLAGS_ARCH_I386 | CONTEXT_FLAGS_EXTENDED)) &&
            ((CopyFlags & (CONTEXT_FLAGS_ARCH_I386 | CONTEXT_FLAGS_EXTENDED)) ==
             (CONTEXT_FLAGS_ARCH_I386 | CONTEXT_FLAGS_EXTENDED)))
        {
            DestinationEx->Legacy.Length = I386_CONTEXT_LENGTH;
        }

        if (SourceValid & RTLP_CONTEXT_HAS_XSTATE)
        {
            Status = RtlpCopyXStateChunk(
                (PXSAVE_AREA_HEADER)((PUCHAR)DestinationEx + DestinationEx->XState.Offset),
                DestinationEx->XState.Length,
                (PXSAVE_AREA_HEADER)((PUCHAR)SourceEx + SourceEx->XState.Offset),
                SourceEx->XState.Length);
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    return STATUS_SUCCESS;
}

/* EOF */
