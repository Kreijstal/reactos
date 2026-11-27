/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Modern process, topology and power API entry points
 */

#include <k32.h>

BOOL WINAPI QueryUnbiasedInterruptTime(PULONGLONG Time);

BOOL
WINAPI
GetProcessMitigationPolicy(HANDLE Process, PROCESS_MITIGATION_POLICY Policy,
                           PVOID Buffer, SIZE_T Length)
{
    DWORD Flags;
    BOOL Permanent;
    PPROCESS_MITIGATION_DEP_POLICY Dep;

    if (!Buffer || Policy >= MaxProcessMitigationPolicy)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (Policy == ProcessDEPPolicy)
    {
        if (Length != sizeof(*Dep))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (!GetProcessDEPPolicy(Process, &Flags, &Permanent))
            return FALSE;
        Dep = Buffer;
        RtlZeroMemory(Dep, sizeof(*Dep));
        Dep->Enable = !!(Flags & PROCESS_DEP_ENABLE);
        Dep->DisableAtlThunkEmulation = !!(Flags & PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION);
        Dep->Permanent = Permanent;
        return TRUE;
    }
    if (Policy == ProcessMitigationOptionsMask)
    {
        if (Length != sizeof(ULONGLONG) && Length != 2 * sizeof(ULONGLONG))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        /* Only advertise the DEP options the kernel can actually enforce. */
        RtlZeroMemory(Buffer, Length);
        *(PULONGLONG)Buffer = 3;
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL
WINAPI
SetProcessMitigationPolicy(PROCESS_MITIGATION_POLICY Policy, PVOID Buffer, SIZE_T Length)
{
    PPROCESS_MITIGATION_DEP_POLICY Dep = Buffer;
    DWORD Flags = 0;

    if (!Buffer || Policy >= MaxProcessMitigationPolicy)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (Policy != ProcessDEPPolicy)
    {
        /* Do not claim to enable a protection the kernel cannot enforce. */
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (Length != sizeof(*Dep) || (Dep->Flags & ~3u))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (Dep->Enable) Flags |= PROCESS_DEP_ENABLE;
    if (Dep->DisableAtlThunkEmulation) Flags |= PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION;
    return SetProcessDEPPolicy(Flags);
}

#if (NTDDI_VERSION >= NTDDI_WIN10)
BOOL
WINAPI
GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION Information,
                          ULONG BufferLength, PULONG ReturnedLength,
                          HANDLE Process, ULONG Flags)
{
    NTSTATUS Status;

    if (!ReturnedLength || Flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *ReturnedLength = 0;
    Status = NtQuerySystemInformationEx(SystemCpuSetInformation,
                                       &Process, sizeof(Process),
                                       Information, BufferLength, ReturnedLength);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }
    return TRUE;
}
#endif

LONG
WINAPI
GetCurrentApplicationUserModelId(UINT32 *Length, PWSTR AppId)
{
    UNREFERENCED_PARAMETER(AppId);
    if (!Length)
        return ERROR_INVALID_PARAMETER;
    /* ReactOS desktop processes have no package application identity. */
    return APPMODEL_ERROR_NO_APPLICATION;
}

LONG
WINAPI
GetApplicationUserModelId(HANDLE Process, UINT32 *Length, PWSTR AppId)
{
    if (!GetProcessId(Process))
        return GetLastError();
    return GetCurrentApplicationUserModelId(Length, AppId);
}

VOID
WINAPI
QueryUnbiasedInterruptTimePrecise(PULONGLONG Time)
{
    /* Use the kernel's interrupt clock; suspend-time accounting is not yet available. */
    QueryUnbiasedInterruptTime(Time);
}

HANDLE
WINAPI
PowerCreateRequest(PREASON_CONTEXT Context)
{
    if (!Context || Context->Version != 0 || (Context->Flags != 1 && Context->Flags != 2))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    /* There is no kernel power-request object support yet. */
    SetLastError(ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE_VALUE;
}

BOOL
WINAPI
PowerSetRequest(HANDLE Request, ULONG Type)
{
    if (Type > 3)
        SetLastError(ERROR_INVALID_PARAMETER);
    else if (!Request || Request == INVALID_HANDLE_VALUE)
        SetLastError(ERROR_INVALID_HANDLE);
    else
        SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL
WINAPI
PowerClearRequest(HANDLE Request, ULONG Type)
{
    return PowerSetRequest(Request, Type);
}
