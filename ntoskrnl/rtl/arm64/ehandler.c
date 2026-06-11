/*
 * PROJECT:     ReactOS Kernel (ARM64)
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     C specific exception/unwind handler for ARM64 SEH metadata
 * NOTES:       Modeled on Wine's dlls/ntdll/unwind.c __C_specific_handler
 *              (Copyright 2009, 2019 Martin Storsjo).
 *              Kernel-mode counterpart of dll/ntdll/arm64/ehandler.c;
 *              filters and __finally funclets execute with the parent
 *              frame's non-volatile registers via __C_ExecuteExceptionFilter
 *              (sdk/lib/rtl/arm64/unwind_asm.S).
 */

#include <ntoskrnl.h>
#include <ndk/rtltypes.h>

#define NDEBUG
#include <debug.h>

typedef struct _RUNTIME_FUNCTION {
    ULONG BeginAddress;
    ULONG UnwindData;
} RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;

#ifndef _ARM64_DISPATCHER_CONTEXT_DEFINED
#define _ARM64_DISPATCHER_CONTEXT_DEFINED 1

typedef struct _SCOPE_TABLE_ARM64 {
    DWORD Count;
    struct
    {
        DWORD BeginAddress;
        DWORD EndAddress;
        DWORD HandlerAddress;
        DWORD JumpTarget;
    } ScopeRecord[1];
} SCOPE_TABLE_ARM64, *PSCOPE_TABLE_ARM64;

typedef SCOPE_TABLE_ARM64 SCOPE_TABLE, *PSCOPE_TABLE;

typedef struct _DISPATCHER_CONTEXT {
    ULONG_PTR ControlPc;
    ULONG_PTR ImageBase;
    PRUNTIME_FUNCTION FunctionEntry;
    ULONG_PTR EstablisherFrame;
    ULONG_PTR TargetPc;
    PCONTEXT ContextRecord;
    PEXCEPTION_ROUTINE LanguageHandler;
    PVOID HandlerData;
    struct _UNWIND_HISTORY_TABLE *HistoryTable;
    DWORD ScopeIndex;
    BOOLEAN ControlPcIsUnwound;
    PUCHAR NonVolatileRegisters;
} DISPATCHER_CONTEXT, *PDISPATCHER_CONTEXT;
#endif

NTSYSAPI
VOID
NTAPI
RtlUnwindEx(
    _In_opt_ PVOID TargetFrame,
    _In_opt_ PVOID TargetIp,
    _In_opt_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID ReturnValue,
    _In_opt_ PCONTEXT ContextRecord,
    _In_opt_ struct _UNWIND_HISTORY_TABLE *HistoryTable);

/* sdk/lib/rtl/arm64/unwind_asm.S */
LONG
__C_ExecuteExceptionFilter(
    _In_ PVOID ExceptionPointers,
    _In_ PVOID EstablisherFrame,
    _In_ PVOID Filter,
    _In_ PUCHAR NonVolatileRegisters);

EXCEPTION_DISPOSITION
__cdecl
__C_specific_handler(
    struct _EXCEPTION_RECORD *ExceptionRecord,
    void *EstablisherFrame,
    struct _CONTEXT *ContextRecord,
    struct _DISPATCHER_CONTEXT *DispatcherContext)
{
    PSCOPE_TABLE ScopeTable = (PSCOPE_TABLE)DispatcherContext->HandlerData;
    ULONG_PTR ImageBase = DispatcherContext->ImageBase;
    ULONG_PTR ControlPc = DispatcherContext->ControlPc;
    EXCEPTION_POINTERS ExceptionPointers;
    PVOID Handler;
    ULONG i;

    if (DispatcherContext->ControlPcIsUnwound)
        ControlPc -= 4;

    if (ExceptionRecord->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        for (i = DispatcherContext->ScopeIndex; i < ScopeTable->Count; i++)
        {
            if (ControlPc < ImageBase + ScopeTable->ScopeRecord[i].BeginAddress)
                continue;
            if (ControlPc >= ImageBase + ScopeTable->ScopeRecord[i].EndAddress)
                continue;
            if (ScopeTable->ScopeRecord[i].JumpTarget)
                continue;

            if ((ExceptionRecord->ExceptionFlags & EXCEPTION_TARGET_UNWIND) &&
                (DispatcherContext->TargetPc >= ImageBase + ScopeTable->ScopeRecord[i].BeginAddress) &&
                (DispatcherContext->TargetPc < ImageBase + ScopeTable->ScopeRecord[i].EndAddress))
            {
                break;
            }

            /* __finally termination handler */
            Handler = (PVOID)(ImageBase + ScopeTable->ScopeRecord[i].HandlerAddress);
            DispatcherContext->ScopeIndex = i + 1;
            __C_ExecuteExceptionFilter(ULongToPtr(TRUE),
                                       EstablisherFrame,
                                       Handler,
                                       DispatcherContext->NonVolatileRegisters);
        }
    }
    else
    {
        for (i = DispatcherContext->ScopeIndex; i < ScopeTable->Count; i++)
        {
            if (ControlPc < ImageBase + ScopeTable->ScopeRecord[i].BeginAddress)
                continue;
            if (ControlPc >= ImageBase + ScopeTable->ScopeRecord[i].EndAddress)
                continue;
            if (!ScopeTable->ScopeRecord[i].JumpTarget)
                continue;

            if (ScopeTable->ScopeRecord[i].HandlerAddress != EXCEPTION_EXECUTE_HANDLER)
            {
                LONG FilterResult;

                ExceptionPointers.ExceptionRecord = ExceptionRecord;
                ExceptionPointers.ContextRecord = ContextRecord;

                Handler = (PVOID)(ImageBase + ScopeTable->ScopeRecord[i].HandlerAddress);
                FilterResult = __C_ExecuteExceptionFilter(&ExceptionPointers,
                                                          EstablisherFrame,
                                                          Handler,
                                                          DispatcherContext->NonVolatileRegisters);
                if (FilterResult == EXCEPTION_CONTINUE_SEARCH)
                    continue;
                if (FilterResult == EXCEPTION_CONTINUE_EXECUTION)
                    return ExceptionContinueExecution;
            }

            /* Unwind to the jump target; this call does not return */
            RtlUnwindEx(EstablisherFrame,
                        (PVOID)(ImageBase + ScopeTable->ScopeRecord[i].JumpTarget),
                        ExceptionRecord,
                        UlongToPtr(ExceptionRecord->ExceptionCode),
                        DispatcherContext->ContextRecord,
                        DispatcherContext->HistoryTable);
        }
    }

    return ExceptionContinueSearch;
}

VOID
__cdecl
_local_unwind(
    _In_opt_ PVOID TargetFrame,
    _In_opt_ PVOID TargetIp)
{
    RtlUnwind(TargetFrame, TargetIp, NULL, 0);
}
