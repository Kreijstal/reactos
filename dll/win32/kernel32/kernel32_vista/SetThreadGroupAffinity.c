/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Implementation of SetThreadGroupAffinity (Win7+)
 */

#include "k32_vista.h"

/*
 * @implemented
 *
 * Single-group system: only Group == 0 is valid. ReactOS doesn't yet handle
 * ThreadGroupInformation in NtSetInformationThread, so we degrade by reading
 * the current affinity (for PreviousGroupAffinity) and applying the new mask
 * via SetThreadAffinityMask.
 */
BOOL
WINAPI
SetThreadGroupAffinity(
    _In_ HANDLE hThread,
    _In_ const GROUP_AFFINITY *GroupAffinity,
    _Out_opt_ PGROUP_AFFINITY PreviousGroupAffinity)
{
    THREAD_BASIC_INFORMATION ThreadBasic;
    NTSTATUS Status;
    DWORD_PTR PrevMask;

    if (GroupAffinity == NULL)
    {
        SetLastError(ERROR_NOACCESS);
        return FALSE;
    }

    if (GroupAffinity->Group != 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (PreviousGroupAffinity != NULL)
    {
        Status = NtQueryInformationThread(hThread,
                                          ThreadBasicInformation,
                                          &ThreadBasic,
                                          sizeof(ThreadBasic),
                                          NULL);
        if (!NT_SUCCESS(Status))
        {
            BaseSetLastNTError(Status);
            return FALSE;
        }
    }
    else
    {
        ThreadBasic.AffinityMask = 0;
    }

    PrevMask = SetThreadAffinityMask(hThread, (DWORD_PTR)GroupAffinity->Mask);
    if (PrevMask == 0)
    {
        /* SetThreadAffinityMask already set the last error. */
        return FALSE;
    }

    if (PreviousGroupAffinity != NULL)
    {
        PreviousGroupAffinity->Mask = ThreadBasic.AffinityMask;
        PreviousGroupAffinity->Group = 0;
        PreviousGroupAffinity->Reserved[0] = 0;
        PreviousGroupAffinity->Reserved[1] = 0;
        PreviousGroupAffinity->Reserved[2] = 0;
    }

    return TRUE;
}
