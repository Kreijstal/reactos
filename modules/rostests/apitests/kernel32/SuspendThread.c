/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Tests for SuspendThread parking across resume cycles and
 *              trap-flag handling across user exception dispatch
 * COPYRIGHT:   Copyright 2026 Kreijstal (rainb@tfwno.gf)
 */
#include "precomp.h"

#define TRAP_FLAG 0x100

static volatile LONG SpinCounter;
static volatile LONG SpinExit;

static DWORD WINAPI SpinThreadProc(LPVOID Parameter)
{
    while (!SpinExit)
        InterlockedIncrement(&SpinCounter);
    return 0;
}

static BOOL WaitForSpin(void)
{
    LONG Start = SpinCounter;
    int i;

    for (i = 0; i < 500; i++)
    {
        if (SpinCounter != Start)
            return TRUE;
        Sleep(10);
    }
    return FALSE;
}

/*
 * A thread must stop running on every suspension, not only on its first.
 * The Win8+ kernel parks a suspended thread on an event that a resume
 * signals; failing to rearm that state after a resume makes every later
 * SuspendThread a no-op while the caller believes the thread is parked,
 * which then corrupts SetThreadContext-based signal delivery (cygwin) and
 * debuggers. Cycle 0 passes even on a broken kernel - the later cycles are
 * the regression.
 */
static void Test_SuspendActuallyStops(void)
{
    HANDLE Thread;
    DWORD PrevCount, ResumeCount;
    LONG Before, After;
    int Cycle;

    SpinCounter = 0;
    SpinExit = 0;
    Thread = CreateThread(NULL, 0, SpinThreadProc, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (!Thread)
        return;
    ok(WaitForSpin(), "spinner thread did not start running\n");

    for (Cycle = 0; Cycle < 5; Cycle++)
    {
        PrevCount = SuspendThread(Thread);
        ok(PrevCount == 0, "cycle %d: SuspendThread returned %lu\n", Cycle, PrevCount);

        /* Generous settle margin, then the thread must be fully stopped */
        Sleep(50);
        Before = SpinCounter;
        Sleep(100);
        After = SpinCounter;
        ok(After == Before,
           "cycle %d: thread kept running while suspended (%ld increments)\n",
           Cycle, After - Before);

        ResumeCount = ResumeThread(Thread);
        ok(ResumeCount == 1, "cycle %d: ResumeThread returned %lu\n", Cycle, ResumeCount);
        ok(WaitForSpin(), "cycle %d: thread did not run after resume\n", Cycle);
    }

    SpinExit = 1;
    ok(WaitForSingleObject(Thread, 5000) == WAIT_OBJECT_0, "spinner did not exit\n");
    CloseHandle(Thread);
}

#if defined(_M_IX86) || defined(_M_AMD64)

static PVOID FaultPage;
static ULONG_PTR NtdllBase, NtdllEnd;
static volatile LONG AvHits;
static volatile LONG StepHits;
static volatile LONG StepsInNtdll;
static volatile LONG RecoveryReached;
static ULONG AvEFlags;

static DECLSPEC_NORETURN VOID RecoveryThreadExit(VOID)
{
    InterlockedExchange(&RecoveryReached, 1);
    ExitThread(0);
}

static LONG CALLBACK TfVectoredHandler(PEXCEPTION_POINTERS Pointers)
{
    DWORD Code = Pointers->ExceptionRecord->ExceptionCode;
    ULONG_PTR Address = (ULONG_PTR)Pointers->ExceptionRecord->ExceptionAddress;
    PCONTEXT Context = Pointers->ContextRecord;

    if (Code == EXCEPTION_SINGLE_STEP)
    {
        InterlockedIncrement(&StepHits);
        /* A single-step must be reported at the stepped application
         * instruction. Landing inside ntdll means the kernel let the
         * dispatcher itself run with the trap flag live. */
        if (Address >= NtdllBase && Address < NtdllEnd)
            InterlockedIncrement(&StepsInNtdll);
        Context->EFlags &= ~TRAP_FLAG;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (Code == EXCEPTION_ACCESS_VIOLATION && Address == (ULONG_PTR)FaultPage)
    {
        InterlockedIncrement(&AvHits);
        AvEFlags = Context->EFlags;
        Context->EFlags &= ~TRAP_FLAG;
#ifdef _M_AMD64
        Context->Rip = (ULONG_PTR)RecoveryThreadExit;
        Context->Rsp = ((Context->Rsp - 0x100) & ~(ULONG_PTR)15) - 8;
#else
        Context->Eip = (ULONG_PTR)RecoveryThreadExit;
        Context->Esp = (Context->Esp - 0x100) & ~(ULONG_PTR)3;
#endif
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static void ArmTrapFlag(HANDLE Thread, PVOID RedirectTo)
{
    CONTEXT Context;
    DWORD PrevCount;
    BOOL Ret;

    PrevCount = SuspendThread(Thread);
    ok(PrevCount == 0, "SuspendThread returned %lu\n", PrevCount);

    ZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_CONTROL;
    Ret = GetThreadContext(Thread, &Context);
    ok(Ret, "GetThreadContext failed, error %lu\n", GetLastError());

    Context.EFlags |= TRAP_FLAG;
    if (RedirectTo != NULL)
    {
#ifdef _M_AMD64
        Context.Rip = (ULONG_PTR)RedirectTo;
#else
        Context.Eip = (ULONG_PTR)RedirectTo;
#endif
    }
    Ret = SetThreadContext(Thread, &Context);
    ok(Ret, "SetThreadContext failed, error %lu\n", GetLastError());

    ResumeThread(Thread);
}

/*
 * Arming TF on another thread via SetThreadContext is how debuggers and
 * cygwin's signal delivery single-step a target. Two invariants of the
 * user exception dispatch, matching Windows:
 *
 * 1. A single-step over an ordinary instruction is reported at the
 *    application instruction, never from inside KiUserExceptionDispatcher.
 *
 * 2. Single-stepping onto an instruction that FAULTS reaches the kernel
 *    with TF still live in the trap frame. The kernel must not let the
 *    user dispatcher inherit it (the dispatcher would trap on its own
 *    first instruction and nest), while the captured CONTEXT keeps TF.
 */
static void Test_TrapFlagAcrossExceptionDispatch(void)
{
    HMODULE Ntdll;
    PIMAGE_NT_HEADERS NtHeaders;
    HANDLE Thread;
    PVOID Handler;
    int i;

    Ntdll = GetModuleHandleW(L"ntdll.dll");
    ok(Ntdll != NULL, "no ntdll?\n");
    NtHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)Ntdll + ((PIMAGE_DOS_HEADER)Ntdll)->e_lfanew);
    NtdllBase = (ULONG_PTR)Ntdll;
    NtdllEnd = NtdllBase + NtHeaders->OptionalHeader.SizeOfImage;

    FaultPage = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    ok(FaultPage != NULL, "VirtualAlloc failed, error %lu\n", GetLastError());

    Handler = AddVectoredExceptionHandler(TRUE, TfVectoredHandler);
    ok(Handler != NULL, "AddVectoredExceptionHandler failed\n");

    /* Invariant 1: plain single-step of the spinner */
    AvHits = StepHits = StepsInNtdll = 0;
    SpinCounter = 0;
    SpinExit = 0;
    Thread = CreateThread(NULL, 0, SpinThreadProc, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    ok(WaitForSpin(), "spinner thread did not start running\n");

    ArmTrapFlag(Thread, NULL);
    for (i = 0; i < 500 && StepHits == 0; i++)
        Sleep(10);
    ok(StepHits >= 1, "armed trap flag never delivered a single-step\n");
    ok(StepsInNtdll == 0, "%ld single-step(s) reported inside ntdll\n", StepsInNtdll);

    SpinExit = 1;
    ok(WaitForSingleObject(Thread, 5000) == WAIT_OBJECT_0, "spinner did not exit\n");
    CloseHandle(Thread);

    /* Invariant 2: single-step onto a faulting instruction */
    AvHits = StepHits = StepsInNtdll = RecoveryReached = 0;
    AvEFlags = 0;
    SpinCounter = 0;
    SpinExit = 0;
    Thread = CreateThread(NULL, 0, SpinThreadProc, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    ok(WaitForSpin(), "spinner thread did not start running\n");

    ArmTrapFlag(Thread, FaultPage);
    for (i = 0; i < 500 && RecoveryReached == 0; i++)
        Sleep(10);
    ok(RecoveryReached == 1, "faulted thread never reached recovery\n");
    ok(AvHits == 1, "expected exactly one fault, got %ld\n", AvHits);
    ok(StepsInNtdll == 0,
       "dispatcher self-stepped: TF leaked into KiUserExceptionDispatcher (%ld hit(s))\n",
       StepsInNtdll);
    ok(AvEFlags & TRAP_FLAG,
       "TF missing from the interrupted context (EFlags %lx)\n", AvEFlags);

    ok(WaitForSingleObject(Thread, 5000) == WAIT_OBJECT_0, "fault thread did not exit\n");
    CloseHandle(Thread);

    RemoveVectoredExceptionHandler(Handler);
    VirtualFree(FaultPage, 0, MEM_RELEASE);
}

#endif /* _M_IX86 || _M_AMD64 */

START_TEST(SuspendThread)
{
    Test_SuspendActuallyStops();
#if defined(_M_IX86) || defined(_M_AMD64)
    Test_TrapFlagAcrossExceptionDispatch();
#endif
}
