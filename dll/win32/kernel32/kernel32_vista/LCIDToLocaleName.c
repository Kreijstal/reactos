/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of LCIDToLocaleName
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

#include "k32_vista.h"
#include <ndk/rtlfuncs.h>

int
WINAPI
LCIDToLocaleName(
    _In_ LCID Locale,
    _Out_writes_opt_(cchName) LPWSTR lpName,
    _In_ int cchName,
    _In_ DWORD dwFlags)
{
    WCHAR Buffer[LOCALE_NAME_MAX_LENGTH];
    UNICODE_STRING LocaleNameString;
    DWORD RtlFlags = 0;
    NTSTATUS Status;
    K32_RTL_LCID_TO_LOCALE_NAME pRtlLcidToLocaleName;

    if (cchName < 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (dwFlags & LOCALE_ALLOW_NEUTRAL_NAMES)
    {
        RtlFlags |= RTL_LOCALE_ALLOW_NEUTRAL_NAMES;
    }

    switch (Locale)
    {
        case LOCALE_NEUTRAL:
        case LOCALE_USER_DEFAULT:
        case LOCALE_CUSTOM_DEFAULT:
            Locale = GetUserDefaultLCID();
            break;

        case LOCALE_SYSTEM_DEFAULT:
            Locale = GetSystemDefaultLCID();
            break;

        case LOCALE_CUSTOM_UI_DEFAULT:
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;

        case MAKELCID(MAKELANGID(LANG_CHINESE, SUBLANG_NEUTRAL), SORT_DEFAULT):
        case 0x07804:
            Locale = MAKELCID(MAKELANGID(LANG_CHINESE_SIMPLIFIED, SUBLANG_CHINESE_SIMPLIFIED), SORT_DEFAULT);
            break;

        case 0x07C04:
            Locale = MAKELCID(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_HONGKONG), SORT_DEFAULT);
            break;
    }

    if (lpName != NULL)
    {
        cchName = min(cchName, LOCALE_NAME_MAX_LENGTH);
        LocaleNameString.Buffer = lpName;
        LocaleNameString.Length = 0;
        LocaleNameString.MaximumLength = (USHORT)(cchName * sizeof(WCHAR));
    }
    else
    {
        LocaleNameString.Buffer = Buffer;
        LocaleNameString.Length = 0;
        LocaleNameString.MaximumLength = sizeof(Buffer);
    }

    pRtlLcidToLocaleName = (K32_RTL_LCID_TO_LOCALE_NAME)K32VistaGetNtdllProc("RtlLcidToLocaleName");
    if (!pRtlLcidToLocaleName)
    {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }

    Status = pRtlLcidToLocaleName(Locale, &LocaleNameString, RtlFlags, FALSE);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return 0;
    }

    /* Return the length including the terminating null */
    return (LocaleNameString.Length / sizeof(WCHAR)) + 1;
}
