/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Implementation of GetCurrentProcessorNumberEx (Win7+)
 */

#include "k32_vista.h"

/*
 * @implemented
 *
 * ReactOS is a single-group system, so Group is always 0.
 */
VOID
WINAPI
GetCurrentProcessorNumberEx(
    _Out_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcNumber == NULL)
        return;

    ProcNumber->Group = 0;
    ProcNumber->Number = (UCHAR)NtGetCurrentProcessorNumber();
    ProcNumber->Reserved = 0;
}
