/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Implementation of GetProcessGroupAffinity (Win7+)
 */

#include "k32_vista.h"

/*
 * @implemented
 *
 * Single-group system: every process is in group 0.
 */
BOOL
WINAPI
GetProcessGroupAffinity(
    _In_ HANDLE hProcess,
    _Inout_ PUSHORT GroupCount,
    _Out_writes_(*GroupCount) PUSHORT GroupArray)
{
    UNREFERENCED_PARAMETER(hProcess);

    if (GroupCount == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (*GroupCount < 1 || GroupArray == NULL)
    {
        *GroupCount = 1;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    GroupArray[0] = 0;
    *GroupCount = 1;
    return TRUE;
}
