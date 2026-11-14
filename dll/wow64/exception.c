/*
 * Wine WOW64 ReactOS port 
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         wow64.dll
 * FILE:            dll/wow64/exception.c
 * PROGRAMMER:      Marcin Jabłoński
 */

#include "ros_wow64_private.h"

LONG
Wow64UnhandledExceptionHandler(IN PEXCEPTION_POINTERS ExceptionInfo);

BOOLEAN
Wow64PassExceptionToGuest(IN PCONTEXT Context,
                          IN PEXCEPTION_RECORD ExceptionRecord)
{
    EXCEPTION_POINTERS exceptionInfo;

    exceptionInfo.ContextRecord = Context;
    exceptionInfo.ExceptionRecord = ExceptionRecord;
    
    Wow64UnhandledExceptionHandler(&exceptionInfo);
    
    ASSERT(FALSE);
    return FALSE;
}

NTSTATUS
WINAPI
wow64_NtRaiseException(UINT* pArgs)
{
    PEXCEPTION_RECORD32 pRecord32 = get_ptr(&pArgs);
    PI386_CONTEXT pContext32 = get_ptr(&pArgs);
    BOOLEAN FirstChance = get_ulong(&pArgs);
    EXCEPTION_RECORD Record;
    CONTEXT Context;
    ULONG i;

    Record.ExceptionCode = pRecord32->ExceptionCode;
    Record.ExceptionFlags = pRecord32->ExceptionFlags;
    Record.ExceptionRecord = UlongToPtr(pRecord32->ExceptionRecord);
    Record.ExceptionAddress = UlongToPtr(pRecord32->ExceptionAddress);
    Record.NumberParameters = pRecord32->NumberParameters;
    for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
    {
        Record.ExceptionInformation[i] = pRecord32->ExceptionInformation[i];
    }

    /* The guest context goes to the kernel as a compatibility-mode (CS 0x23)
       context, the same shape a fault taken in 32-bit code has: the kernel
       reports the exception to the debugger, then hands it to the native
       KiUserExceptionDispatcher, which passes it on to the guest's
       KiUserExceptionDispatcher (LdrpTryWow64Exception). A second chance
       (raised by the guest dispatcher when no 32-bit handler took it) goes
       to the debugger and the exception port, as for a native process. */
    Wow64CopyContext32To64(&Context, pContext32);

    /* The guest context only carries floating point state if it says so,
       don't let the zeroed native one load over the thread's */
    if (!(pContext32->ContextFlags & (WOW64_CONTEXT_FLOATING_POINT & ~WOW64_CONTEXT_i386)) &&
        !(pContext32->ContextFlags & (WOW64_CONTEXT_EXTENDED_REGISTERS & ~WOW64_CONTEXT_i386)))
    {
        Context.ContextFlags &= ~(CONTEXT_FLOATING_POINT & ~CONTEXT_AMD64);
    }

    if (!FirstChance)
    {
        DPRINT1("WOW64: unhandled 32-bit exception %08lx at %08lx\n",
                pRecord32->ExceptionCode, pRecord32->ExceptionAddress);
    }

    return NtRaiseException(&Record, &Context, FirstChance);
}

LONG
Wow64UnhandledExceptionHandler(IN PEXCEPTION_POINTERS ExceptionInfo)
{    
    ULONG i;
    EXCEPTION_RECORD32 Record32;
    I386_CONTEXT Context32;
    PCONTEXT pContext;
    PEXCEPTION_RECORD pRecord;
    
    ULONG DispatcherArgs[2];
    
    pRecord = ExceptionInfo->ExceptionRecord;
    pContext = ExceptionInfo->ContextRecord;
    
    Record32.ExceptionCode = pRecord->ExceptionCode;
    Record32.ExceptionFlags = pRecord->ExceptionFlags;
    Record32.ExceptionRecord = PtrToUlong(pRecord->ExceptionRecord);
    Record32.ExceptionAddress = PtrToUlong(pRecord->ExceptionAddress);
    Record32.NumberParameters = pRecord->NumberParameters;
        
    for (i = 0; i < Record32.NumberParameters; i++)
    {
        Record32.ExceptionInformation[i] = pRecord->ExceptionInformation[i];
    }
    
    Wow64CopyContext64To32(&Context32, pContext);
    
    DispatcherArgs[0] = PtrToUlong(&Record32);
    DispatcherArgs[1] = PtrToUlong(&Context32);
    
    Jump32(PtrToUlong(NtDll32KiUserExceptionDispatcher), 2, DispatcherArgs);
    
    return EXCEPTION_EXECUTE_HANDLER;
}
