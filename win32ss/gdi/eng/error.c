#include <win32k.h>

/*
 * @implemented
 * https://learn.microsoft.com/en-us/windows/win32/api/winddi/nf-winddi-enggetlasterror
 */
ULONG
APIENTRY
EngGetLastError(VOID)
{
    PTEB pTeb = PsGetCurrentThreadTeb();
    ULONG LastError = ERROR_SUCCESS;

    if (pTeb)
    {
        _SEH2_TRY
        {
            LastError = pTeb->LastErrorValue;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        _SEH2_END;
    }

    return LastError;
}

/*
 * @implemented
 * https://learn.microsoft.com/en-us/windows/win32/api/winddi/nf-winddi-engsetlasterror
 */
VOID
APIENTRY
EngSetLastError(_In_ ULONG iError)
{
    PTEB pTeb = PsGetCurrentThreadTeb();

    if (pTeb)
    {
        _SEH2_TRY
        {
            pTeb->LastErrorValue = iError;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        _SEH2_END;
    }
}

VOID
FASTCALL
SetLastNtError(_In_ NTSTATUS Status)
{
    EngSetLastError(RtlNtStatusToDosError(Status));
}
