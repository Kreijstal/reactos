/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Implementation of QueryUnbiasedInterruptTime (Win7+)
 */

#include "k32_vista.h"

/*
 * @implemented
 *
 * ReactOS doesn't track time spent suspended, so we return the biased
 * interrupt time (in 100-ns units). Acceptable degradation for a system
 * that currently can't suspend.
 */
BOOL
WINAPI
QueryUnbiasedInterruptTime(
    _Out_ PULONGLONG UnbiasedTime)
{
    LARGE_INTEGER InterruptTime;

    if (UnbiasedTime == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    InterruptTime = KiReadSystemTime(&SharedUserData->InterruptTime);
    *UnbiasedTime = (ULONGLONG)InterruptTime.QuadPart;
    return TRUE;
}
