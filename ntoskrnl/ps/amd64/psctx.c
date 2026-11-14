/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ps/amd64/psctx.c
 * PURPOSE:         Process Manager: Set/Get Context for i386
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*
 * XSAVEC. Not _xsavec64: clang declares it only with the xsavec target
 * feature, which the kernel is not built with, and then calls it as an
 * external function.
 */
FORCEINLINE
VOID
PspXSaveC64(
    _Out_ PVOID Buffer,
    _In_ ULONG64 Mask)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("xsavec64 %0"
                         : "=m" (*(PUCHAR)Buffer)
                         : "a" ((ULONG)Mask), "d" ((ULONG)(Mask >> 32))
                         : "memory");
#else
    _xsavec64(Buffer, Mask);
#endif
}

/* FUNCTIONS ******************************************************************/

static
VOID
PspGetThreadFloatingPointContext(
    _Inout_ PCONTEXT Context)
{
    DECLSPEC_ALIGN(16) XMM_SAVE_AREA32 FloatState;

    if ((Context->ContextFlags & CONTEXT_FLOATING_POINT) !=
        CONTEXT_FLOATING_POINT)
    {
        return;
    }

    _fxsave64(&FloatState);

    RtlCopyMemory(&Context->FltSave,
                  &FloatState,
                  FIELD_OFFSET(XMM_SAVE_AREA32, MxCsr));
    Context->FltSave.MxCsr = Context->MxCsr;
    Context->FltSave.MxCsr_Mask = FloatState.MxCsr_Mask;
    RtlCopyMemory(Context->FltSave.FloatRegisters,
                  FloatState.FloatRegisters,
                  sizeof(Context->FltSave.FloatRegisters));
}

static
VOID
PspSetThreadFloatingPointContext(
    _In_ PCONTEXT Context)
{
    DECLSPEC_ALIGN(16) XMM_SAVE_AREA32 FloatState;

    if ((Context->ContextFlags & CONTEXT_FLOATING_POINT) !=
        CONTEXT_FLOATING_POINT)
    {
        return;
    }

    _fxsave64(&FloatState);
    RtlCopyMemory(&FloatState,
                  &Context->FltSave,
                  FIELD_OFFSET(XMM_SAVE_AREA32, MxCsr));
    RtlCopyMemory(FloatState.FloatRegisters,
                  Context->FltSave.FloatRegisters,
                  sizeof(FloatState.FloatRegisters));
    _fxrstor64(&FloatState);
}


#define TAG_PS_XSTATE 'sXsP'

/* Shared with ntdll, see sdk/lib/rtl/xstate.c */
NTSTATUS
NTAPI
RtlpCopyXStateChunk(
    _Out_writes_bytes_(DestinationLength) PXSAVE_AREA_HEADER Destination,
    _In_ ULONG DestinationLength,
    _In_reads_bytes_(SourceLength) const XSAVE_AREA_HEADER *Source,
    _In_ ULONG SourceLength);

/*
 * A CONTEXT_XSTATE request carries a CONTEXT_EX after the CONTEXT whose
 * XState chunk holds the XSAVE header and the non-legacy components. The
 * extended state is exchanged through a kernel copy of the XSAVE image: the
 * target thread saves its live state into it, or restores from it, in the
 * get/set APC. User state is live there: the kernel does not touch the
 * extended registers and a context switch saves and restores all of them.
 */
NTSTATUS
NTAPI
PspCaptureXStateContext(
    _Inout_ PGET_SET_CTX_CONTEXT GetSetContext,
    _In_ PCONTEXT ThreadContext,
    _In_ KPROCESSOR_MODE PreviousMode,
    _In_ BOOLEAN SetContext)
{
    PXSAVE_AREA_HEADER CallerXState;
    PCONTEXT_EX CallerContextEx;
    CONTEXT_EX ContextEx;
    PXSAVE_AREA XStateArea;
    ULONG AreaLength;
    NTSTATUS Status;

    GetSetContext->XStateArea = NULL;

    if ((GetSetContext->Context.ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE)
        return STATUS_SUCCESS;

    if (SharedUserData->XState.EnabledFeatures == 0)
        return STATUS_NOT_SUPPORTED;

    /* Capture the caller's CONTEXT_EX */
    CallerContextEx = (PCONTEXT_EX)(ThreadContext + 1);
    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForRead(CallerContextEx, sizeof(CONTEXT_EX), sizeof(ULONG));
        ContextEx = *CallerContextEx;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    /* The XState chunk follows the CONTEXT_EX and lies inside the context */
    if ((ContextEx.XState.Offset < (LONG)sizeof(CONTEXT_EX)) ||
        (ContextEx.XState.Length < sizeof(XSAVE_AREA_HEADER)) ||
        (ContextEx.All.Offset > ContextEx.XState.Offset) ||
        ((LONGLONG)ContextEx.All.Offset + ContextEx.All.Length <
         (LONGLONG)ContextEx.XState.Offset + ContextEx.XState.Length))
    {
        return STATUS_INVALID_PARAMETER;
    }

    CallerXState = (PXSAVE_AREA_HEADER)((PUCHAR)CallerContextEx + ContextEx.XState.Offset);

    /* A get writes the chunk back when the target thread has answered */
    if (!SetContext && (PreviousMode != KernelMode))
    {
        _SEH2_TRY
        {
            ProbeForWrite(CallerXState, ContextEx.XState.Length, sizeof(ULONG));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    /* Page-sized pool blocks are page aligned, as XSAVE needs 64 bytes */
    AreaLength = SharedUserData->XState.AllFeatureSize;
    XStateArea = ExAllocatePoolWithTag(NonPagedPool,
                                       ROUND_TO_PAGES(AreaLength),
                                       TAG_PS_XSTATE);
    if (XStateArea == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    ASSERT(((ULONG_PTR)XStateArea & 63) == 0);
    RtlZeroMemory(XStateArea, AreaLength);

    if (SetContext)
    {
        PXSAVE_AREA_HEADER Header = &XStateArea->Header;

        /* The kernel image has room for every user component */
        if (SharedUserData->XState.CompactionEnabled)
        {
            Header->CompactionMask = SharedUserData->XState.EnabledFeatures |
                                     SharedUserData->XState.EnabledUserVisibleSupervisorFeatures |
                                     XSTATE_COMPACTION_ENABLE_MASK;
        }

        _SEH2_TRY
        {
            if (PreviousMode != KernelMode)
                ProbeForRead(CallerXState, ContextEx.XState.Length, sizeof(ULONG));
            Status = RtlpCopyXStateChunk(Header,
                                         AreaLength - sizeof(XSAVE_FORMAT),
                                         CallerXState,
                                         ContextEx.XState.Length);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(XStateArea, TAG_PS_XSTATE);
            return Status;
        }

        /* The restore faults on components the image does not describe */
        if (SharedUserData->XState.CompactionEnabled)
            Header->Mask &= Header->CompactionMask;
    }

    GetSetContext->XStateArea = XStateArea;
    GetSetContext->XStateAreaLength = AreaLength;
    GetSetContext->CallerXState = CallerXState;
    GetSetContext->CallerXStateLength = ContextEx.XState.Length;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PspReturnXStateContext(
    _Inout_ PGET_SET_CTX_CONTEXT GetSetContext,
    _In_ PCONTEXT ThreadContext)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ThreadContext);

    if (GetSetContext->XStateArea == NULL)
        return STATUS_SUCCESS;

    /* The chunk was probed when the request was captured */
    _SEH2_TRY
    {
        Status = RtlpCopyXStateChunk(GetSetContext->CallerXState,
                                     GetSetContext->CallerXStateLength,
                                     &GetSetContext->XStateArea->Header,
                                     GetSetContext->XStateAreaLength - sizeof(XSAVE_FORMAT));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

VOID
NTAPI
PspFreeXStateContext(
    _Inout_ PGET_SET_CTX_CONTEXT GetSetContext)
{
    if (GetSetContext->XStateArea)
    {
        ExFreePoolWithTag(GetSetContext->XStateArea, TAG_PS_XSTATE);
        GetSetContext->XStateArea = NULL;
    }
}

static
VOID
PspGetThreadXStateContext(
    _Inout_ PGET_SET_CTX_CONTEXT GetSetContext)
{
    PXSAVE_AREA XStateArea = GetSetContext->XStateArea;
    ULONG64 Mask;

    if (XStateArea == NULL)
        return;

    /* XSAVE leaves the header bytes it does not own untouched */
    RtlZeroMemory(&XStateArea->Header, sizeof(XStateArea->Header));

    if (SharedUserData->XState.CompactionEnabled)
    {
        Mask = (SharedUserData->XState.EnabledFeatures |
                SharedUserData->XState.EnabledUserVisibleSupervisorFeatures) &
               ~XSTATE_MASK_LEGACY;
        if (KeFeatureBits & KF_XSAVES)
        {
            _xsaves64(XStateArea, Mask);
        }
        else
        {
            PspXSaveC64(XStateArea, Mask & SharedUserData->XState.EnabledFeatures);
        }
    }
    else
    {
        Mask = SharedUserData->XState.EnabledFeatures & ~XSTATE_MASK_LEGACY;
        _xsave64(XStateArea, Mask);
    }
}

static
VOID
PspSetThreadXStateContext(
    _Inout_ PGET_SET_CTX_CONTEXT GetSetContext)
{
    PXSAVE_AREA XStateArea = GetSetContext->XStateArea;
    ULONG64 Mask;

    if (XStateArea == NULL)
        return;

    /* Components missing from the header's mask go to their initial state.
       Supervisor state is never taken from the caller. */
    Mask = SharedUserData->XState.EnabledVolatileFeatures & ~XSTATE_MASK_LEGACY;

    /* A non-compacted restore of AVX state reloads MXCSR from the legacy
       region: keep the current value, CONTEXT_FLOATING_POINT owns it */
    XStateArea->LegacyState.MxCsr = _mm_getcsr();

    if (SharedUserData->XState.CompactionEnabled && (KeFeatureBits & KF_XSAVES))
    {
        _xrstors64(XStateArea, Mask);
    }
    else
    {
        _xrstor64(XStateArea, Mask);
    }
}

_IRQL_requires_(APC_LEVEL)
VOID
NTAPI
PspGetOrSetContextKernelRoutine(
    _In_ PKAPC Apc,
    _Inout_ PKNORMAL_ROUTINE* NormalRoutine,
    _Inout_ PVOID* NormalContext,
    _Inout_ PVOID* SystemArgument1,
    _Inout_ PVOID* SystemArgument2)
{
    PGET_SET_CTX_CONTEXT GetSetContext;
    PKTHREAD Thread;
    PKTRAP_FRAME TrapFrame = NULL;

    PAGED_CODE();

    /* Get the Context Structure */
    GetSetContext = CONTAINING_RECORD(Apc, GET_SET_CTX_CONTEXT, Apc);
    Thread = Apc->SystemArgument2;
    NT_ASSERT(KeGetCurrentThread() == Thread);

    /* If this is a kernel-mode request, grab the saved trap frame */
    if (GetSetContext->Mode == KernelMode)
    {
        TrapFrame = Thread->TrapFrame;
    }

    /* If we don't have one, grab it from the stack */
    if (TrapFrame == NULL)
    {
        /* Get the thread's base trap frame */
        TrapFrame = KeGetTrapFrame(KeGetCurrentThread());
    }

    /* Check if it's a set or get */
    if (Apc->SystemArgument1 != 0)
    {
        /* Set the nonvolatiles on the stack, target frame is the trap frame */
        KiSetTrapContext(TrapFrame, &GetSetContext->Context, GetSetContext->Mode);
        PspSetThreadFloatingPointContext(&GetSetContext->Context);
        PspSetThreadXStateContext(GetSetContext);
    }
    else
    {
        /* Get the nonvolatiles from the stack */
        KiGetTrapContext(TrapFrame, &GetSetContext->Context);
        PspGetThreadFloatingPointContext(&GetSetContext->Context);
        PspGetThreadXStateContext(GetSetContext);

        /* Report the original user stack top for a thread that has not
           entered its start routine yet. */
        if ((GetSetContext->Mode == UserMode) &&
            ((GetSetContext->Context.ContextFlags & CONTEXT_CONTROL) ==
             CONTEXT_CONTROL) &&
            (GetSetContext->Context.Rip ==
             (ULONG64)CONTAINING_RECORD(Thread, ETHREAD, Tcb)->StartAddress))
        {
            GetSetContext->Context.Rsp =
                ALIGN_UP_BY(GetSetContext->Context.Rsp, PAGE_SIZE);
        }
    }

    /* Notify the Native API that we are done */
    KeSetEvent(&GetSetContext->Event, IO_NO_INCREMENT, FALSE);
}

/* EOF */
