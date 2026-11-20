/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that the floating point state survives an exception the
 *              handler continues from, and that the handler can change it
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"
#include <pseh/pseh2.h>

#if defined(_M_IX86) || defined(_M_AMD64)

#ifdef _M_AMD64
#include <xmmintrin.h>
#endif

#define TEST_EXCEPTION 0xE0000086

/* Round toward zero, all exceptions masked */
#define TEST_MXCSR 0x7F80
/* Set by the handler: round down, all exceptions masked */
#define HANDLER_MXCSR 0x3F80

#ifdef _M_IX86
/* 53-bit precision, round toward zero, all exceptions masked */
#define TEST_X87_CW 0x0E7F
#endif

static BOOL ChangeInHandler;
static BOOL HandlerChanged;
static ULONG HandlerSawMxCsr;

static
LONG
FpuFilter(
    _In_ PEXCEPTION_POINTERS ExceptionPointers)
{
    PCONTEXT Context = ExceptionPointers->ContextRecord;

    switch (ExceptionPointers->ExceptionRecord->ExceptionCode)
    {
        case TEST_EXCEPTION:
            break;
#ifdef _M_IX86
        case STATUS_INTEGER_DIVIDE_BY_ZERO:
            /* Continue after the 2-byte "div ecx" */
            Context->Eip += 2;
            break;
#endif
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }

#ifdef _M_IX86
    if (Context->ContextFlags & (CONTEXT_EXTENDED_REGISTERS & ~CONTEXT_i386))
    {
        /* The FXSAVE image: MXCSR is at offset 24 */
        PULONG MxCsr = (PULONG)&Context->ExtendedRegisters[24];

        HandlerSawMxCsr = *MxCsr;
        if (ChangeInHandler)
        {
            *MxCsr = HANDLER_MXCSR;
            HandlerChanged = TRUE;
        }
    }
#elif defined(_M_AMD64)
    if (Context->ContextFlags & (CONTEXT_FLOATING_POINT & ~CONTEXT_AMD64))
    {
        HandlerSawMxCsr = Context->MxCsr;
        if (ChangeInHandler)
        {
            Context->MxCsr = HANDLER_MXCSR;
            Context->FltSave.MxCsr = HANDLER_MXCSR;
            HandlerChanged = TRUE;
        }
    }
#endif

    return EXCEPTION_CONTINUE_EXECUTION;
}

#ifdef _M_IX86
/* Not the intrinsics: the x86 build does not target SSE */
static
ULONG
GetMxCsr(VOID)
{
    ULONG MxCsr;
#if defined(__GNUC__)
    __asm__ __volatile__("stmxcsr %0" : "=m"(MxCsr));
#else
    __asm stmxcsr MxCsr
#endif
    return MxCsr;
}

static
VOID
SetMxCsr(ULONG MxCsr)
{
#if defined(__GNUC__)
    __asm__ __volatile__("ldmxcsr %0" : : "m"(MxCsr));
#else
    __asm ldmxcsr MxCsr
#endif
}

static
USHORT
GetX87ControlWord(VOID)
{
    USHORT ControlWord;
#if defined(__GNUC__)
    __asm__ __volatile__("fnstcw %0" : "=m"(ControlWord));
#else
    __asm fnstcw ControlWord
#endif
    return ControlWord;
}

static
VOID
SetX87ControlWord(USHORT ControlWord)
{
#if defined(__GNUC__)
    __asm__ __volatile__("fldcw %0" : : "m"(ControlWord));
#else
    __asm fldcw ControlWord
#endif
}
#else
#define GetMxCsr() _mm_getcsr()
#define SetMxCsr(MxCsr) _mm_setcsr(MxCsr)
#endif

static
VOID
RaiseContinuable(VOID)
{
    _SEH2_TRY
    {
        RaiseException(TEST_EXCEPTION, 0, 0, NULL);
    }
    _SEH2_EXCEPT(FpuFilter(_SEH2_GetExceptionInformation()))
    {
        ok(0, "The exception was not continued\n");
    }
    _SEH2_END;
}

#ifdef _M_IX86
static
VOID
RaiseFault(VOID)
{
    _SEH2_TRY
    {
        /* Divide by zero; the handler continues after the 2-byte div */
#if defined(__GNUC__)
        ULONG Eax = 1, Edx = 0;
        __asm__ __volatile__("xorl %%ecx, %%ecx\n\t"
                             "divl %%ecx"
                             : "+a"(Eax), "+d"(Edx) : : "ecx");
#else
        __asm
        {
            mov eax, 1
            xor edx, edx
            xor ecx, ecx
            div ecx
        }
#endif
    }
    _SEH2_EXCEPT(FpuFilter(_SEH2_GetExceptionInformation()))
    {
        ok(0, "The fault was not continued\n");
    }
    _SEH2_END;
}
#endif

START_TEST(ExceptionFpuState)
{
    ULONG OldMxCsr = GetMxCsr(), MxCsr;
#ifdef _M_IX86
    USHORT OldControlWord = GetX87ControlWord(), ControlWord;
#endif

    /* State the handler leaves alone comes back unchanged */
    ChangeInHandler = FALSE;
    HandlerSawMxCsr = 0;
    SetMxCsr(TEST_MXCSR);
#ifdef _M_IX86
    SetX87ControlWord(TEST_X87_CW);
#endif
    RaiseContinuable();
    MxCsr = GetMxCsr();
#ifdef _M_IX86
    ControlWord = GetX87ControlWord();
    SetX87ControlWord(OldControlWord);
#endif
    SetMxCsr(OldMxCsr);

    ok(MxCsr == TEST_MXCSR, "MXCSR 0x%lx after the exception, expected 0x%x\n", MxCsr, TEST_MXCSR);
#ifdef _M_IX86
    ok(ControlWord == TEST_X87_CW, "x87 control word 0x%x after the exception, expected 0x%x\n",
       ControlWord, TEST_X87_CW);
#endif
    if (HandlerSawMxCsr)
    {
        ok(HandlerSawMxCsr == TEST_MXCSR, "The handler saw MXCSR 0x%lx, expected 0x%x\n",
           HandlerSawMxCsr, TEST_MXCSR);
    }

    /* A change the handler makes to the context is applied */
    ChangeInHandler = TRUE;
    HandlerChanged = FALSE;
    SetMxCsr(TEST_MXCSR);
    RaiseContinuable();
    MxCsr = GetMxCsr();
    SetMxCsr(OldMxCsr);

    if (HandlerChanged)
    {
        ok(MxCsr == HANDLER_MXCSR, "MXCSR 0x%lx after the handler set it, expected 0x%x\n",
           MxCsr, HANDLER_MXCSR);
    }
    else
    {
        /* No floating point state in the context, nothing to change */
        ok(MxCsr == TEST_MXCSR, "MXCSR 0x%lx after the exception, expected 0x%x\n",
           MxCsr, TEST_MXCSR);
    }

#ifdef _M_IX86
    /* The same for a fault, whose context the kernel captured */
    ChangeInHandler = TRUE;
    HandlerChanged = FALSE;
    SetMxCsr(TEST_MXCSR);
    SetX87ControlWord(TEST_X87_CW);
    RaiseFault();
    MxCsr = GetMxCsr();
    ControlWord = GetX87ControlWord();
    SetX87ControlWord(OldControlWord);
    SetMxCsr(OldMxCsr);

    ok(MxCsr == (HandlerChanged ? HANDLER_MXCSR : TEST_MXCSR),
       "MXCSR 0x%lx after the fault, expected 0x%x\n",
       MxCsr, HandlerChanged ? HANDLER_MXCSR : TEST_MXCSR);
    ok(ControlWord == TEST_X87_CW, "x87 control word 0x%x after the fault, expected 0x%x\n",
       ControlWord, TEST_X87_CW);
    trace("The fault's context %s the extended registers\n", HandlerChanged ? "has" : "does not have");
#endif
}

#else

START_TEST(ExceptionFpuState)
{
    skip("x86 and x64 only\n");
}

#endif
