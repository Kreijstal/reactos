/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite Exception test
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 */

#define KMT_EMULATE_KERNEL
#include <kmt_test.h>

static
VOID
PossiblyRaise(
    _In_ BOOLEAN Raise)
{
    if (Raise)
    {
        ExRaiseStatus(STATUS_ASSERTION_FAILURE);
    }
}

static
VOID
InnerFunction(
    _Inout_ PULONG State,
    _In_ BOOLEAN Raise)
{
    _SEH2_VOLATILE INT Var = 123;
    static _SEH2_VOLATILE INT *AddressOfVar;

    AddressOfVar = &Var;
    ok_eq_ulong(*State, 1);
    _SEH2_TRY
    {
        *State = 2;
        PossiblyRaise(Raise);
        ok_eq_ulong(*State, 2);
        *State = 3;
    }
    _SEH2_FINALLY
    {
        ok_eq_int(Var, 123);
        ok_eq_pointer(&Var, AddressOfVar);
        if (Raise)
            ok_eq_ulong(*State, 2);
        else
            ok_eq_ulong(*State, 3);
        *State = 4;
    }
    _SEH2_END;

    ok_eq_int(Var, 123);
    ok_eq_pointer(&Var, AddressOfVar);
    ok_eq_ulong(*State, 4);
    *State = 5;
}

static
VOID
OuterFunction(
    _Inout_ PULONG State,
    _In_ BOOLEAN Raise)
{
    _SEH2_VOLATILE INT Var = 456;
    static _SEH2_VOLATILE INT *AddressOfVar;

    AddressOfVar = &Var;
    ok_eq_ulong(*State, 0);
    _SEH2_TRY
    {
        *State = 1;
        InnerFunction(State, Raise);
        ok_eq_ulong(*State, 5);
        *State = 6;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ok_eq_int(Var, 456);
        ok_eq_pointer(&Var, AddressOfVar);
        ok_eq_ulong(*State, 4);
        *State = 7;
    }
    _SEH2_END;

    ok_eq_int(Var, 456);
    ok_eq_pointer(&Var, AddressOfVar);
    if (Raise)
        ok_eq_ulong(*State, 7);
    else
        ok_eq_ulong(*State, 6);
    *State = 8;
}

static
VOID
TestNestedExceptionHandler(VOID)
{
    ULONG State;

    State = 0;
    OuterFunction(&State, FALSE);
    ok_eq_ulong(State, 8);

    State = 0;
    OuterFunction(&State, TRUE);
    ok_eq_ulong(State, 8);
}

/*
 * Locals that are set before a __try and not changed in it must still hold
 * their values in the __except block. Values that live across calls go to
 * callee-saved registers (ebx, esi, edi on x86); the exception handler jumps
 * to the __except block from deep inside the dispatcher, where those
 * registers hold the dispatcher's values, not the ones the function had.
 * IopCreateFile freed such a register ("OpenPacket") in its __except block.
 */
static volatile ULONG_PTR NonVolatileSeed[3] = { 0x1111, 0x2222, 0x3333 };

static
VOID
TestNonVolatilesInExcept(
    _In_ BOOLEAN AccessViolation)
{
    ULONG_PTR First, Second, Third;
    BOOLEAN Handled = FALSE;

    First = NonVolatileSeed[0];
    Second = NonVolatileSeed[1];
    Third = NonVolatileSeed[2];
    /* A call, so the three values have to survive one */
    PossiblyRaise(FALSE);

    _SEH2_TRY
    {
        if (AccessViolation)
            (void)*(volatile CHAR *)NULL;
        else
            PossiblyRaise(TRUE);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Handled = TRUE;
        ok_eq_ulongptr(First, 0x1111);
        ok_eq_ulongptr(Second, 0x2222);
        ok_eq_ulongptr(Third, 0x3333);
    }
    _SEH2_END;

    ok_eq_bool(Handled, TRUE);
    ok_eq_ulongptr(First, 0x1111);
    ok_eq_ulongptr(Second, 0x2222);
    ok_eq_ulongptr(Third, 0x3333);
}

START_TEST(RtlException)
{
    PCHAR Buffer[128];

    /* Access a valid pointer - must not trigger SEH */
    KmtStartSeh()
        RtlFillMemory(Buffer, sizeof(Buffer), 0x12);
    KmtEndSeh(STATUS_SUCCESS);

    /* Read from a NULL pointer - must cause an access violation */
    KmtStartSeh()
        (void)*(volatile CHAR *)NULL;
    KmtEndSeh(STATUS_ACCESS_VIOLATION);

    /* Write to a NULL pointer - must cause an access violation */
    KmtStartSeh()
        *(volatile CHAR *)NULL = 5;
    KmtEndSeh(STATUS_ACCESS_VIOLATION);

    /* TODO: Find where MmBadPointer is defined - gives an unresolved external */
#if 0 //def KMT_KERNEL_MODE
    /* Read from MmBadPointer - must cause an access violation */
    KmtStartSeh()
        (void)*(volatile CHAR *)MmBadPointer;
    KmtEndSeh(STATUS_ACCESS_VIOLATION);

    /* Write to MmBadPointer - must cause an access violation */
    KmtStartSeh()
        *(volatile CHAR *)MmBadPointer = 5;
    KmtEndSeh(STATUS_ACCESS_VIOLATION);
#endif

    KmtStartSeh()
        ExRaiseStatus(STATUS_ACCESS_VIOLATION);
    KmtEndSeh(STATUS_ACCESS_VIOLATION);

    KmtStartSeh()
        ExRaiseStatus(STATUS_TIMEOUT);
    KmtEndSeh(STATUS_TIMEOUT);

    KmtStartSeh()
        ExRaiseStatus(STATUS_STACK_OVERFLOW);
    KmtEndSeh(STATUS_STACK_OVERFLOW);

    KmtStartSeh()
        ExRaiseStatus(STATUS_GUARD_PAGE_VIOLATION);
    KmtEndSeh(STATUS_GUARD_PAGE_VIOLATION);

    TestNestedExceptionHandler();
    TestNonVolatilesInExcept(FALSE);
    TestNonVolatilesInExcept(TRUE);

    /* We cannot test this in kernel mode easily - the stack is just "somewhere"
     * in system space, and there's no guard page below it */
#ifdef KMT_USER_MODE
    /* Overflow the stack - must cause a special exception */
    KmtStartSeh()
        volatile CHAR *Pointer;

        while (1)
        {
            Pointer = _alloca(1024);
            *Pointer = 5;
        }
    KmtEndSeh(STATUS_STACK_OVERFLOW);
#endif
}
