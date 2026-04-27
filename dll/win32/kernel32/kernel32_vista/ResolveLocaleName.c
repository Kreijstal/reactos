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

    if (cchLocaleName < 0 || (cchLocaleName > 0 && lpLocaleName == NULL))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
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
