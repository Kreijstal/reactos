/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     C specific exception/unwind handler for AMD64
 * COPYRIGHT:   Copyright 2018-2021 Timo Kreuzer <timo.kreuzer@reactos.org>
*/

#include <precomp.h>
#include <winnt.h>

static LONG
__cdecl
CallExceptionFilter(
    PEXCEPTION_FILTER ExceptionFilter,
    PEXCEPTION_POINTERS ExceptionPointers,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord);

static VOID
__cdecl
CallTerminationHandler(
    PTERMINATION_HANDLER TerminationHandler,
    BOOLEAN AbnormalTermination,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord);

__asm__(
    ".text\n"
    ".p2align 4, 0x90\n"
    "CallExceptionFilter:\n"
    "\tpush %rbp\n"
    "\tpush %rbx\n"
    "\tpush %rsi\n"
    "\tpush %rdi\n"
    "\tpush %r12\n"
    "\tpush %r13\n"
    "\tpush %r14\n"
    "\tpush %r15\n"
    "\tmov %rcx, %r11\n"
    "\tmov %rdx, %r10\n"
    "\tmov %r8, %rax\n"
    "\tmov %r8, %rbp\n"
    "\tmov 0x98(%r9), %rbx\n"
    "\tmov 0xb0(%r9), %rsi\n"
    "\tmov 0xb8(%r9), %rdi\n"
    "\tmov 0xe0(%r9), %r12\n"
    "\tmov 0xe8(%r9), %r13\n"
    "\tmov 0xf0(%r9), %r14\n"
    "\tmov 0xf8(%r9), %r15\n"
    "\tmov %r10, %rcx\n"
    "\tmov %rax, %rdx\n"
    "\tsub $40, %rsp\n"
    "\tcall *%r11\n"
    "\tadd $40, %rsp\n"
    "\tpop %r15\n"
    "\tpop %r14\n"
    "\tpop %r13\n"
    "\tpop %r12\n"
    "\tpop %rdi\n"
    "\tpop %rsi\n"
    "\tpop %rbx\n"
    "\tpop %rbp\n"
    "\tret\n"
    ".p2align 4, 0x90\n"
    "CallTerminationHandler:\n"
    "\tpush %rbp\n"
    "\tpush %rbx\n"
    "\tpush %rsi\n"
    "\tpush %rdi\n"
    "\tpush %r12\n"
    "\tpush %r13\n"
    "\tpush %r14\n"
    "\tpush %r15\n"
    "\tmov %rcx, %r11\n"
    "\tmov %edx, %r10d\n"
    "\tmov %r8, %rax\n"
    "\tmov %r8, %rbp\n"
    "\tmov 0x98(%r9), %rbx\n"
    "\tmov 0xb0(%r9), %rsi\n"
    "\tmov 0xb8(%r9), %rdi\n"
    "\tmov 0xe0(%r9), %r12\n"
    "\tmov 0xe8(%r9), %r13\n"
    "\tmov 0xf0(%r9), %r14\n"
    "\tmov 0xf8(%r9), %r15\n"
    "\tmov %r10d, %ecx\n"
    "\tmov %rax, %rdx\n"
    "\tsub $40, %rsp\n"
    "\tcall *%r11\n"
    "\tadd $40, %rsp\n"
    "\tpop %r15\n"
    "\tpop %r14\n"
    "\tpop %r13\n"
    "\tpop %r12\n"
    "\tpop %rdi\n"
    "\tpop %rsi\n"
    "\tpop %rbx\n"
    "\tpop %rbp\n"
    "\tret\n");


_CRTIMP
EXCEPTION_DISPOSITION
__cdecl
__C_specific_handler(
    struct _EXCEPTION_RECORD *ExceptionRecord,
    void *EstablisherFrame,
    struct _CONTEXT *ContextRecord,
    struct _DISPATCHER_CONTEXT *DispatcherContext)
{
    PSCOPE_TABLE ScopeTable;
    ULONG i, BeginAddress, EndAddress, Handler;
    ULONG64 ImageBase, JumpTarget, IpOffset, TargetIpOffset;
    EXCEPTION_POINTERS ExceptionPointers;
    PTERMINATION_HANDLER TerminationHandler;
    PEXCEPTION_FILTER ExceptionFilter;
    LONG FilterResult;

    /* Set up the EXCEPTION_POINTERS */
    ExceptionPointers.ExceptionRecord = ExceptionRecord;
    ExceptionPointers.ContextRecord = ContextRecord;

    /* Get the image base */
    ImageBase = (ULONG64)DispatcherContext->ImageBase;

    /* Get the image base relative instruction pointers */
    IpOffset = DispatcherContext->ControlPc - ImageBase;
    TargetIpOffset = DispatcherContext->TargetIp - ImageBase;

    /* Get the scope table and current index */
    ScopeTable = (PSCOPE_TABLE)DispatcherContext->HandlerData;

    /* Loop while we have scope table entries */
    while (DispatcherContext->ScopeIndex < ScopeTable->Count)
    {
        /* Use i as index and update the dispatcher context */
        i = DispatcherContext->ScopeIndex++;

        /* Get the start and end of the scrope */
        BeginAddress = ScopeTable->ScopeRecord[i].BeginAddress;
        EndAddress = ScopeTable->ScopeRecord[i].EndAddress;

        /* Skip this scope if we are not within the bounds */
        if ((IpOffset < BeginAddress) || (IpOffset >= EndAddress))
        {
            continue;
        }

        /* Check if this is an unwind */
        if (ExceptionRecord->ExceptionFlags & EXCEPTION_UNWIND)
        {
            /* Check if this is a target unwind */
            if (ExceptionRecord->ExceptionFlags & EXCEPTION_TARGET_UNWIND)
            {
                /* Check if the target is within the scope itself */
                if ((TargetIpOffset >= BeginAddress) &&
                    (TargetIpOffset <  EndAddress))
                {
                    return ExceptionContinueSearch;
                }
            }

            /* Check if this is a termination handler / finally function */
            if (ScopeTable->ScopeRecord[i].JumpTarget == 0)
            {
                /* Call the handler */
                Handler = ScopeTable->ScopeRecord[i].HandlerAddress;
                TerminationHandler = (PTERMINATION_HANDLER)(ImageBase + Handler);
                CallTerminationHandler(TerminationHandler,
                                       TRUE,
                                       EstablisherFrame,
                                       DispatcherContext->ContextRecord);
            }
            else if (ScopeTable->ScopeRecord[i].JumpTarget == TargetIpOffset)
            {
                return ExceptionContinueSearch;
            }
        }
        else
        {
            /* We are only unterested in exception handlers */
            if (ScopeTable->ScopeRecord[i].JumpTarget == 0)
            {
                continue;
            }

            /* This is an exception filter, get the handler address */
            Handler = ScopeTable->ScopeRecord[i].HandlerAddress;

            /* Check for hardcoded EXCEPTION_EXECUTE_HANDLER */
            if (Handler == EXCEPTION_EXECUTE_HANDLER)
            {
                /* This is our result */
                FilterResult = EXCEPTION_EXECUTE_HANDLER;
            }
            else
            {
                /* Otherwise we need to call the handler */
                ExceptionFilter = (PEXCEPTION_FILTER)(ImageBase + Handler);
                FilterResult = CallExceptionFilter(ExceptionFilter,
                                                   &ExceptionPointers,
                                                   EstablisherFrame,
                                                   DispatcherContext->ContextRecord);
            }

            if (FilterResult < 0 /* EXCEPTION_CONTINUE_EXECUTION */)
            {
                return ExceptionContinueExecution;
            }

            if (FilterResult > 0 /* EXCEPTION_EXECUTE_HANDLER */)
            {
                JumpTarget = (ImageBase + ScopeTable->ScopeRecord[i].JumpTarget);

                /* Unwind to the target address (This does not return) */
                RtlUnwindEx(EstablisherFrame,
                            (PVOID)JumpTarget,
                            ExceptionRecord,
                            UlongToPtr(ExceptionRecord->ExceptionCode),
                            DispatcherContext->ContextRecord,
                            DispatcherContext->HistoryTable);

                /* Should not get here */
                __debugbreak();
            }
        }
    }

    /* Reached the end of the scope table */
    return ExceptionContinueSearch;
}

void __cdecl _local_unwind(void* frame, void* target)
{
    RtlUnwind(frame, target, NULL, 0);
}
