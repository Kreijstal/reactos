/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of GetSystemDefaultLocaleName
 */

#include "k32_vista.h"
#include <ndk/rtlfuncs.h>

INT
WINAPI
GetSystemDefaultLocaleName(
    LPWSTR lpLocaleName,
    INT cchLocaleName)
{
    UNICODE_STRING LocaleNameString;
    NTSTATUS Status;

    if (lpLocaleName == NULL || cchLocaleName <= 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    cchLocaleName = min(cchLocaleName, LOCALE_NAME_MAX_LENGTH);
    LocaleNameString.Buffer = lpLocaleName;
    LocaleNameString.Length = 0;
    LocaleNameString.MaximumLength = (USHORT)(cchLocaleName * sizeof(WCHAR));

    Status = RtlLcidToLocaleName(LOCALE_SYSTEM_DEFAULT, &LocaleNameString, 0, FALSE);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return 0;
    }

    return LocaleNameString.Length / sizeof(WCHAR) + 1;
}
