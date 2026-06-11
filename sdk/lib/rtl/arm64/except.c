/*
 * PROJECT:     ReactOS Run-Time Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ARM64 exception dispatching
 * NOTES:       Modeled on Wine's dlls/ntdll/signal_arm64.c
 *              (Copyright 1999, 2005 Alexandre Julliard).
 */

#include <rtl.h>
#include <intrin.h>

#define NDEBUG
#include <debug.h>

#ifndef UNW_FLAG_NHANDLER
#define UNW_FLAG_NHANDLER 0x0
#define UNW_FLAG_EHANDLER 0x1
#define UNW_FLAG_UHANDLER 0x2
#endif

#ifndef CONTEXT_UNWOUND_TO_CALL
#define CONTEXT_UNWOUND_TO_CALL CONTEXT_ARM64_UNWOUND_TO_CALL
#endif

NTSYSAPI
PRUNTIME_FUNCTION
NTAPI
RtlLookupFunctionEntry(
    _In_ DWORD64 ControlPc,
    _Out_ PDWORD64 ImageBase,
    _Inout_opt_ PVOID HistoryTable);

NTSYSAPI
PEXCEPTION_ROUTINE
NTAPI
RtlVirtualUnwind(
    _In_ ULONG HandlerType,
    _In_ ULONG64 ImageBase,
    _In_ ULONG64 ControlPc,
    _In_ PRUNTIME_FUNCTION FunctionEntry,
    _Inout_ PCONTEXT Context,
    _Out_ PVOID *HandlerData,
    _Out_ PULONG64 EstablisherFrame,
    _Inout_opt_ PVOID ContextPointers);

/* unwind_asm.S */
EXCEPTION_DISPOSITION
RtlpCallSehHandler(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ ULONG_PTR EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _In_ PVOID DispatcherContext,
    _In_ PEXCEPTION_ROUTINE ExceptionRoutine);

/*
 * Handler for exceptions that happen inside an exception handler call.
 * Referenced by RtlpCallSehHandler's unwind data.
 */
EXCEPTION_DISPOSITION
NTAPI
RtlpNestedExceptionHandler(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _Inout_ PVOID DispatcherContext)
{
    (VOID)EstablisherFrame;
    (VOID)ContextRecord;
    (VOID)DispatcherContext;

    if (ExceptionRecord->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
        return ExceptionContinueSearch;
    return ExceptionNestedException;
}

/*
 * Handler for exceptions that happen inside an unwind handler call.
 * Referenced by RtlpCallUnwindHandler's unwind data. Restores the original
 * dispatcher context (saved at frame[-2] by RtlpCallUnwindHandler) and
 * reports a collided unwind.
 */
EXCEPTION_DISPOSITION
NTAPI
RtlpUnwindHandler(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _Inout_ PVOID DispatcherContext)
{
    PDISPATCHER_CONTEXT Dispatch = (PDISPATCHER_CONTEXT)DispatcherContext;
    PDISPATCHER_CONTEXT OrigDispatch = ((PDISPATCHER_CONTEXT *)EstablisherFrame)[-2];

    (VOID)ExceptionRecord;
    (VOID)ContextRecord;

    /* Copy the original dispatcher into the current one, except TargetPc */
    Dispatch->ControlPc          = OrigDispatch->ControlPc;
    Dispatch->ImageBase          = OrigDispatch->ImageBase;
    Dispatch->FunctionEntry      = OrigDispatch->FunctionEntry;
    Dispatch->EstablisherFrame   = OrigDispatch->EstablisherFrame;
    Dispatch->LanguageHandler    = OrigDispatch->LanguageHandler;
    Dispatch->HandlerData        = OrigDispatch->HandlerData;
    Dispatch->HistoryTable       = OrigDispatch->HistoryTable;
    Dispatch->ScopeIndex         = OrigDispatch->ScopeIndex;
    Dispatch->ControlPcIsUnwound = OrigDispatch->ControlPcIsUnwound;
    *Dispatch->ContextRecord     = *OrigDispatch->ContextRecord;
    if (Dispatch->NonVolatileRegisters && OrigDispatch->NonVolatileRegisters)
    {
        RtlCopyMemory(Dispatch->NonVolatileRegisters,
                      OrigDispatch->NonVolatileRegisters,
                      sizeof(DISPATCHER_CONTEXT_NONVOLREG_ARM64));
    }

    return ExceptionCollidedUnwind;
}

/* Unwind one frame, refreshing the dispatcher context */
static NTSTATUS
RtlpArm64VirtualUnwindFrame(
    _In_ ULONG HandlerType,
    _Inout_ PDISPATCHER_CONTEXT DispatcherContext,
    _Inout_ PCONTEXT Context)
{
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 *NonVolRegs;
    DWORD64 Pc = Context->Pc;
    ULONG64 ImageBase;
    ULONG i;

    DispatcherContext->ScopeIndex = 0;
    DispatcherContext->ControlPc = Pc;
    DispatcherContext->ControlPcIsUnwound =
        (Context->ContextFlags & CONTEXT_UNWOUND_TO_CALL) != 0;
    if (DispatcherContext->ControlPcIsUnwound)
        Pc -= 4;

    NonVolRegs = (DISPATCHER_CONTEXT_NONVOLREG_ARM64 *)DispatcherContext->NonVolatileRegisters;
    if (NonVolRegs)
    {
        RtlCopyMemory(NonVolRegs->GpNvRegs, &Context->X19, sizeof(NonVolRegs->GpNvRegs));
        for (i = 0; i < NONVOL_FP_NUMREG_ARM64; i++)
            NonVolRegs->FpNvRegs[i] = Context->V[i + 8].D[0];
    }

    DispatcherContext->FunctionEntry = RtlLookupFunctionEntry(Pc,
                                                              &ImageBase,
                                                              DispatcherContext->HistoryTable);
    DispatcherContext->ImageBase = ImageBase;

    if (DispatcherContext->FunctionEntry == NULL)
    {
        /* Leaf function: the return address is still in Lr */
        if (Pc == Context->Lr)
        {
            /* Invalid leaf function, no way to continue */
            return STATUS_INVALID_DISPOSITION;
        }
        Context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
        Context->Pc = Context->Lr;
        DispatcherContext->EstablisherFrame = Context->Sp;
        DispatcherContext->LanguageHandler = NULL;
        DispatcherContext->HandlerData = NULL;
        return STATUS_SUCCESS;
    }

    DispatcherContext->LanguageHandler =
        RtlVirtualUnwind(HandlerType,
                         ImageBase,
                         Pc,
                         DispatcherContext->FunctionEntry,
                         Context,
                         &DispatcherContext->HandlerData,
                         (PULONG64)&DispatcherContext->EstablisherFrame,
                         NULL);

    if (Context->Pc == 0)
        return STATUS_INVALID_DISPOSITION;

    return STATUS_SUCCESS;
}

VOID
NTAPI
RtlGetCallersAddress(
    _Out_ PVOID *CallersAddress,
    _Out_ PVOID *CallersCaller)
{
    *CallersAddress = _ReturnAddress();
    *CallersCaller = NULL;
}

BOOLEAN
NTAPI
RtlDispatchException(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PCONTEXT ContextRecord)
{
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 NonVolRegs;
    DISPATCHER_CONTEXT DispatcherContext;
    CONTEXT UnwindContext;
    EXCEPTION_DISPOSITION Disposition;
    NTSTATUS Status;
    ULONG_PTR StackLow, StackHigh;
    ULONG64 Frame;
    PVOID HandlerData;

    /* Call vectored exception handlers first */
    if (RtlCallVectoredExceptionHandlers(ExceptionRecord, ContextRecord))
    {
        RtlCallVectoredContinueHandlers(ExceptionRecord, ContextRecord);
        return TRUE;
    }

    UnwindContext = *ContextRecord;
    RtlpGetStackLimits(&StackLow, &StackHigh);

    DispatcherContext.TargetPc = 0;
    DispatcherContext.ContextRecord = &UnwindContext;
    DispatcherContext.HistoryTable = NULL;
    DispatcherContext.NonVolatileRegisters = NonVolRegs.Buffer;

    for (;;)
    {
        Status = RtlpArm64VirtualUnwindFrame(UNW_FLAG_EHANDLER,
                                             &DispatcherContext,
                                             &UnwindContext);
        if (Status != STATUS_SUCCESS)
            break;

    UnwindDone:
        if (!DispatcherContext.EstablisherFrame)
            break;

        if ((DispatcherContext.EstablisherFrame < StackLow) ||
            (DispatcherContext.EstablisherFrame > StackHigh) ||
            (DispatcherContext.EstablisherFrame & 0x7))
        {
            DPRINT1("RtlDispatchException: invalid frame %p (%p-%p)\n",
                    (PVOID)(ULONG_PTR)DispatcherContext.EstablisherFrame,
                    (PVOID)StackLow,
                    (PVOID)StackHigh);
            ExceptionRecord->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        if (DispatcherContext.LanguageHandler)
        {
            Disposition = RtlpCallSehHandler(ExceptionRecord,
                                             DispatcherContext.EstablisherFrame,
                                             ContextRecord,
                                             &DispatcherContext,
                                             DispatcherContext.LanguageHandler);

            ExceptionRecord->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;

            switch (Disposition)
            {
                case ExceptionContinueExecution:
                    if (ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE)
                        return FALSE;
                    RtlCallVectoredContinueHandlers(ExceptionRecord, ContextRecord);
                    return TRUE;
                case ExceptionContinueSearch:
                    break;
                case ExceptionNestedException:
                    ExceptionRecord->ExceptionFlags |= EXCEPTION_NESTED_CALL;
                    break;
                case ExceptionCollidedUnwind:
                    /* UnwindContext was already replaced with the collided
                       dispatcher's context by RtlpUnwindHandler */
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                                     DispatcherContext.ImageBase,
                                     DispatcherContext.ControlPc,
                                     DispatcherContext.FunctionEntry,
                                     &UnwindContext,
                                     &HandlerData,
                                     &Frame,
                                     NULL);
                    goto UnwindDone;
                default:
                    return FALSE;
            }
        }

        if (UnwindContext.Sp >= StackHigh)
            break;
    }

    RtlCallVectoredContinueHandlers(ExceptionRecord, ContextRecord);
    return FALSE;
}

VOID
NTAPI
RtlInitializeContext(
    _In_ HANDLE ProcessHandle,
    _Out_ PCONTEXT ThreadContext,
    _In_opt_ PVOID ThreadStartParam,
    _In_ PTHREAD_START_ROUTINE ThreadStartAddress,
    _In_ PINITIAL_TEB InitialStack)
{
    UNREFERENCED_PARAMETER(ProcessHandle);

    RtlZeroMemory(ThreadContext, sizeof(*ThreadContext));

    ThreadContext->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_ARM64;
    ThreadContext->Pc = (ULONG64)ThreadStartAddress;
    ThreadContext->Sp = ALIGN_DOWN_BY((ULONG64)InitialStack, 16);
    ThreadContext->X0 = (ULONG64)ThreadStartParam;
    ThreadContext->Lr = (ULONG64)RtlExitUserThread;
}
