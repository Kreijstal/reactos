/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Implementation of PrefetchVirtualMemory (Win8+)
 */

#include "k32_vista.h"

/*
 * MSDN documents PrefetchVirtualMemory as advisory: a hint to the memory
 * manager that the listed ranges will be touched soon. It is allowed to be
 * a no-op, so we sanity-check the inputs and return TRUE.
 */
typedef struct _WIN32_MEMORY_RANGE_ENTRY
{
    PVOID VirtualAddress;
    SIZE_T NumberOfBytes;
} WIN32_MEMORY_RANGE_ENTRY, *PWIN32_MEMORY_RANGE_ENTRY;

/*
 * @implemented
 */
BOOL
WINAPI
PrefetchVirtualMemory(
    _In_ HANDLE hProcess,
    _In_ ULONG_PTR NumberOfEntries,
    _In_ PWIN32_MEMORY_RANGE_ENTRY VirtualAddresses,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Flags);

    if (hProcess == NULL)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (NumberOfEntries != 0 && VirtualAddresses == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Advisory hint — accept and do nothing. */
    return TRUE;
}
