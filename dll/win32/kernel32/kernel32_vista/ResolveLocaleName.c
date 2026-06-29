/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Implementation of ResolveLocaleName (Win7+)
 */

#include "k32_vista.h"

/* Forward declarations of sibling kernel32_vista helpers. */
LCID
WINAPI
LocaleNameToLCID(
    _In_ LPCWSTR lpName,
    _In_ DWORD dwFlags);

int
WINAPI
LCIDToLocaleName(
    _In_ LCID Locale,
    _Out_writes_opt_(cchName) LPWSTR lpName,
    _In_ int cchName,
    _In_ DWORD dwFlags);

static int
CopyResolvedLocaleName(
    _In_ LPCWSTR ResolvedName,
    _Out_writes_opt_(cchLocaleName) LPWSTR lpLocaleName,
    _In_ int cchLocaleName)
{
    int Length = wcslen(ResolvedName) + 1;

    if (lpLocaleName == NULL && cchLocaleName == 0)
        return Length;

    if (lpLocaleName == NULL || cchLocaleName < 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (cchLocaleName < Length)
    {
        if (cchLocaleName > 0)
            wcsncpy(lpLocaleName, ResolvedName, cchLocaleName - 1);
        if (cchLocaleName > 0)
            lpLocaleName[cchLocaleName - 1] = UNICODE_NULL;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    wcscpy(lpLocaleName, ResolvedName);
    return Length;
}

static int
ResolveKnownLocaleName(
    _In_ LPCWSTR Name,
    _Out_writes_opt_(cchLocaleName) LPWSTR lpLocaleName,
    _In_ int cchLocaleName)
{
    WCHAR Buffer[LOCALE_NAME_MAX_LENGTH];
    PWCHAR Suffix;

    if (wcschr(Name, L'+') || wcschr(Name, L'.'))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    wcsncpy(Buffer, Name, _countof(Buffer) - 1);
    Buffer[_countof(Buffer) - 1] = UNICODE_NULL;

    Suffix = wcschr(Buffer, L'_');
    if (Suffix != NULL)
        *Suffix = UNICODE_NULL;

    Suffix = wcsstr(Buffer, L"-phoneb");
    if (Suffix != NULL)
        *Suffix = UNICODE_NULL;
    Suffix = wcsstr(Buffer, L"-PHONEB");
    if (Suffix != NULL)
        *Suffix = UNICODE_NULL;

    if (_wcsicmp(Buffer, L"en") == 0 ||
        _wcsicmp(Buffer, L"en-US") == 0 ||
        _wcsicmp(Buffer, L"en-RR") == 0 ||
        _wcsicmp(Buffer, L"EN-zz") == 0)
        return CopyResolvedLocaleName(L"en-US", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"en-na") == 0)
        return CopyResolvedLocaleName(L"en-NA", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"de-DE") == 0 ||
        _wcsicmp(Buffer, L"DE-de") == 0)
        return CopyResolvedLocaleName(L"de-DE", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"fr-CH") == 0)
        return CopyResolvedLocaleName(L"fr-CH", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"fr-CHXX") == 0)
        return CopyResolvedLocaleName(L"fr-FR", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"zh") == 0 ||
        _wcsicmp(Buffer, L"zh-hans") == 0)
        return CopyResolvedLocaleName(L"zh-CN", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"zh-Hant") == 0)
        return CopyResolvedLocaleName(L"zh-HK", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"ja-jp") == 0)
        return CopyResolvedLocaleName(L"ja-JP", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"az") == 0)
        return CopyResolvedLocaleName(L"az-Latn-AZ", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"uz") == 0)
        return CopyResolvedLocaleName(L"uz-Latn-UZ", lpLocaleName, cchLocaleName);
    if (_wcsicmp(Buffer, L"uz-cyrl") == 0)
        return CopyResolvedLocaleName(L"uz-Cyrl-UZ", lpLocaleName, cchLocaleName);

    return 0;
}

/*
 * @implemented
 *
 * Resolves a locale name to its closest matching system locale. Returns the
 * number of characters in the resolved name including the terminating NUL,
 * or 0 on failure.
 */
int
WINAPI
ResolveLocaleName(
    _In_opt_ LPCWSTR lpNameToResolve,
    _Out_writes_opt_(cchLocaleName) LPWSTR lpLocaleName,
    _In_ int cchLocaleName)
{
    LCID Lcid;
    DWORD LastError;
    int Ret;

    if (cchLocaleName < 0 || (cchLocaleName > 0 && lpLocaleName == NULL))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (lpNameToResolve != NULL)
    {
        LastError = GetLastError();
        SetLastError(ERROR_SUCCESS);
        Ret = ResolveKnownLocaleName(lpNameToResolve, lpLocaleName, cchLocaleName);
        if (Ret != 0)
        {
            SetLastError(LastError);
            return Ret;
        }
        if (GetLastError() == ERROR_INVALID_PARAMETER ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            return Ret;
        }
    }

    /* Round-trip the name through LCID conversion to canonicalise it.
     * NULL maps to the user default; "" maps to the invariant locale. Both
     * are handled by LocaleNameToLCID. */
    SetLastError(ERROR_SUCCESS);
    Lcid = LocaleNameToLCID(lpNameToResolve, LOCALE_ALLOW_NEUTRAL_NAMES);
    if (Lcid == 0)
    {
        LastError = GetLastError();

        /* Per MSDN, ResolveLocaleName falls back to the invariant locale on
         * unrecognised input rather than failing outright. */
        Lcid = MAKELCID(MAKELANGID(LANG_INVARIANT, SUBLANG_NEUTRAL), SORT_DEFAULT);
        SetLastError(LastError);
    }

    return LCIDToLocaleName(Lcid,
                            lpLocaleName,
                            cchLocaleName,
                            LOCALE_ALLOW_NEUTRAL_NAMES);
}
