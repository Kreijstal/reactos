/*
 * PROJECT:     ReactOS System Libraries
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RtlIsNameInExpression implementation
 * COPYRIGHT:   Copyright 2026 Carl Bialorucki <carl.bialorucki@reactos.org>
 */

#include <rtl.h>
#define NDEBUG
#include <debug.h>

#define DOS_STAR  (L'<')
#define DOS_QM    (L'>')
#define DOS_DOT   (L'"')

#define EXPRESSION_IS_WILDCARD(expr, len) \
    (len == 1 && expr->Buffer[0] == L'*') \
    || (len == 3 && expr->Buffer[0] == L'*' && expr->Buffer[1] == L'.' && expr->Buffer[2] == L'*')
#define IS_LITERAL_OR_DOS_DOT(wchar) \
    (wchar == DOS_DOT || (wchar != L'*' && wchar != DOS_STAR && wchar != L'?' && wchar != DOS_QM))

/* ****** PRIVATE FUNCTIONS ****** */

static __inline BOOLEAN
RtlpContainsWildcard(
    _In_ PWCH Buffer,
    _In_ ULONG Length)
{
    for (ULONG i = 0; i < Length; i++)
    {
        WCHAR c = Buffer[i];
        if (c == L'*' || c == L'?' || c == DOS_STAR || c == DOS_QM || c == DOS_DOT)
            return TRUE;
    }
    return FALSE;
}

static __inline WCHAR
RtlpGetExprChar(
    _In_     PWCH    Buffer,
    _In_     ULONG   Position,
    _In_     ULONG   Length,
    _In_     BOOLEAN IgnoreCase,
    _In_opt_ PWCH    UpcaseTable)
{
    WCHAR Result = (Position < Length) ? Buffer[Position] : L'\0';
    if (IgnoreCase)
        Result = UpcaseTable ? UpcaseTable[Result] : RtlUpcaseUnicodeChar(Result);
    return Result;
}

/* ****** PUBLIC FUNCTIONS ****** */

BOOLEAN
NTAPI
RtlIsNameInExpression(
    _In_     PUNICODE_STRING Expression,
    _In_     PUNICODE_STRING Name,
    _In_     BOOLEAN         IgnoreCase,
    _In_opt_ PWCH            UpcaseTable
)
{
    ULONG NameLen = Name->Length / sizeof(WCHAR);
    ULONG ExprLen = Expression->Length / sizeof(WCHAR);
    ULONG NamePos = 0, ExprPos = 0;
    ULONG LastDot = MAXULONG;
    ULONG StateCount = ExprLen + 1;
    BOOLEAN StackStates[2][64];
    PBOOLEAN States, NextStates, AllocatedStates = NULL;
    BOOLEAN Result = FALSE;

    /* For more information about this algorithm, see
     * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/0b034646-4e23-4874-8488-2adac231ff23
     */

    /* *** Optimizations *** */

    /* Empty string */
    if (ExprLen == 0 || NameLen == 0)
        return (ExprLen == 0 && NameLen == 0);

    /* Wildcard only */
    if (EXPRESSION_IS_WILDCARD(Expression, ExprLen))
        return TRUE;

    /* Wildcard + literal with no other wildcards */
    if (ExprLen >= 2
        && NameLen >= ExprLen - 1
        && Expression->Buffer[0] == L'*'
        && !RtlpContainsWildcard(&Expression->Buffer[1], ExprLen - 1))
    {
        ExprPos = 1;
        NamePos = NameLen - (ExprLen - 1);

        while (ExprPos < ExprLen)
        {
            WCHAR NameChar = RtlpGetExprChar(Name->Buffer, NamePos, NameLen, IgnoreCase, UpcaseTable);
            WCHAR ExprChar = RtlpGetExprChar(Expression->Buffer, ExprPos, ExprLen, IgnoreCase, UpcaseTable);
            if (ExprChar != NameChar)
                return FALSE;
            NamePos++;
            ExprPos++;
        }

        return TRUE;
    }

    /* Keep every possible expression position. Remembering only the most
     * recent star loses alternatives when DOS_STARs occur in the same mask. */
    if (StateCount <= RTL_NUMBER_OF(StackStates[0]))
    {
        States = StackStates[0];
        NextStates = StackStates[1];
    }
    else
    {
        AllocatedStates = RtlpAllocateMemory(2 * StateCount * sizeof(BOOLEAN), 'NltR');
        if (AllocatedStates == NULL)
            RtlRaiseStatus(STATUS_NO_MEMORY);
        States = AllocatedStates;
        NextStates = States + StateCount;
    }
    RtlZeroMemory(States, StateCount * sizeof(BOOLEAN));
    States[0] = TRUE;
    for (NamePos = 0; NamePos < NameLen; NamePos++)
    {
        if (Name->Buffer[NamePos] == L'.')
            LastDot = NamePos;
    }

    for (NamePos = 0; NamePos <= NameLen; NamePos++)
    {
        WCHAR NameChar = RtlpGetExprChar(Name->Buffer, NamePos, NameLen, IgnoreCase, UpcaseTable);
        BOOLEAN HaveNextState = FALSE;
        PBOOLEAN Swap;

        RtlZeroMemory(NextStates, StateCount * sizeof(BOOLEAN));
        for (ExprPos = 0; ExprPos < ExprLen; ExprPos++)
        {
            WCHAR ExprChar;
            if (!States[ExprPos])
                continue;
            ExprChar = RtlpGetExprChar(Expression->Buffer, ExprPos, ExprLen, IgnoreCase, UpcaseTable);

            if (ExprChar == L'*' || ExprChar == DOS_STAR)
            {
                /* Both stars may match no characters. A DOS_STAR cannot
                 * remain active after consuming the final dot. */
                States[ExprPos + 1] = TRUE;
                if (NamePos < NameLen)
                {
                    NextStates[ExprPos + (ExprChar == DOS_STAR && NamePos == LastDot)] = TRUE;
                    HaveNextState = TRUE;
                }
            }
            else if (ExprChar == DOS_QM && (NamePos == NameLen || NameChar == L'.'))
            {
                States[ExprPos + 1] = TRUE;
            }
            else if (ExprChar == DOS_DOT && NamePos == NameLen)
            {
                States[ExprPos + 1] = TRUE;
            }
            else if (NamePos < NameLen &&
                     (ExprChar == L'?' || ExprChar == DOS_QM ||
                      (ExprChar == DOS_DOT && NameChar == L'.') || ExprChar == NameChar))
            {
                NextStates[ExprPos + 1] = TRUE;
                HaveNextState = TRUE;
            }
        }

        if (NamePos == NameLen)
        {
            Result = States[ExprLen];
            break;
        }
        if (!HaveNextState)
            break;
        Swap = States;
        States = NextStates;
        NextStates = Swap;
    }

    if (AllocatedStates != NULL)
        RtlpFreeMemory(AllocatedStates, 'NltR');
    return Result;
}
