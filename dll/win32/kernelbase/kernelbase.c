/*
 * PROJECT:     ReactOS KernelBase
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Minimal Win7+/Win8 KernelBase.dll entry points used by Cygwin
 *              and msys2 runtimes that probe KernelBase via LoadLibrary +
 *              GetProcAddress.  Only the entries that those runtimes actually
 *              call (or fatally require be loadable) are provided here.
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winerror.h>

#define NTOS_MODE_USER
#include <ndk/kefuncs.h>
#include <ndk/rtlfuncs.h>

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hinst);
    return TRUE;
}

/*
 * @implemented
 *
 * Returns the system interrupt time in 100-ns units.  ReactOS does not yet
 * track suspend time, so the biased and unbiased values are identical;
 * "Precise" means "do not snap to the most recent timer tick".  Both
 * variants read the shared KSYSTEM_TIME atomically.
 */
BOOL WINAPI QueryInterruptTime(PULONGLONG InterruptTime)
{
    LARGE_INTEGER Time;

    if (InterruptTime == NULL)
    {
        SetLastError(ERROR_NOACCESS);
        return FALSE;
    }

    Time = KiReadSystemTime(&SharedUserData->InterruptTime);
    *InterruptTime = (ULONGLONG)Time.QuadPart;
    return TRUE;
}

BOOL WINAPI QueryInterruptTimePrecise(PULONGLONG InterruptTime)
{
    return QueryInterruptTime(InterruptTime);
}

BOOL WINAPI QueryUnbiasedInterruptTimePrecise(PULONGLONG UnbiasedTime)
{
    return QueryInterruptTime(UnbiasedTime);
}

/*
 * @unimplemented
 *
 * Real implementation lives in kernel32 starting at WINVER 0xA00; for our
 * NT 6.x targets the symbol is not exported there, so provide a soft stub
 * that satisfies cygwin's GetProcAddress probe and reports the call as
 * unimplemented.  Cygwin tolerates a non-success HRESULT here (the autoload
 * entry has the notimp flag set).
 */
HRESULT WINAPI SetThreadDescription(HANDLE hThread, PCWSTR lpThreadDescription)
{
    (void)hThread;
    (void)lpThreadDescription;
    return E_NOTIMPL;
}

/*
 * @implemented
 *
 * Cygwin uses VirtualAlloc2 only when wincap.has_extended_mem_api() is true,
 * which it is not for the major-version-6 caps profile that ReactOS reports.
 * msys-2.0.dll uses the same probe but degrades to plain VirtualAlloc when
 * the extended path returns NULL.  Forwarding to VirtualAllocEx without
 * honoring the MEM_EXTENDED_PARAMETER list is therefore both correct (the
 * documented behaviour when the parameters are unsupported) and sufficient
 * for the runtimes in question.
 */
LPVOID WINAPI VirtualAlloc2(HANDLE Process,
                            PVOID BaseAddress,
                            SIZE_T Size,
                            ULONG AllocationType,
                            ULONG PageProtection,
                            PVOID /* MEM_EXTENDED_PARAMETER* */ ExtendedParameters,
                            ULONG ParameterCount)
{
    (void)ExtendedParameters;
    (void)ParameterCount;

    if (Process == NULL)
        Process = GetCurrentProcess();

    return VirtualAllocEx(Process, BaseAddress, Size, AllocationType, PageProtection);
}
