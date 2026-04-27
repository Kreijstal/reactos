/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Implementation of GetThreadGroupAffinity (Win7+)
 */

#include "k32_vista.h"

/*
 * @implemented
 *
 * ReactOS doesn't yet handle ThreadGroupInformation in NtQueryInformationThread,
 * so we degrade by reading ThreadBasicInformation.AffinityMask and synthesising
 * a single-group GROUP_AFFINITY (Group = 0).
 */
BOOL
WINAPI
GetThreadGroupAffinity(
    _In_ HANDLE hThread,
    _Out_ PGROUP_AFFINITY GroupAffinity)
{
    THREAD_BASIC_INFORMATION ThreadBasic;
    NTSTATUS Status;

    if (GroupAffinity == NULL)
    {
        SetLastError(ERROR_NOACCESS);
        return FALSE;
    }

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

    GroupAffinity->Mask = ThreadBasic.AffinityMask;
    GroupAffinity->Group = 0;
    GroupAffinity->Reserved[0] = 0;
    GroupAffinity->Reserved[1] = 0;
    GroupAffinity->Reserved[2] = 0;
    return TRUE;
}
