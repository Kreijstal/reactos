/*
 * PROJECT:     ReactOS NCrypt stub
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Minimal NCrypt.dll surface so modern installers load.
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(ncrypt);

#ifndef NTE_NOT_SUPPORTED
#define NTE_NOT_SUPPORTED ((LONG)0x80090030L)
#endif

LONG WINAPI NCryptExportKey(ULONG_PTR hKey, ULONG_PTR hExportKey, LPCWSTR pszBlobType,
                            void *pParameterList, PBYTE pbOutput, DWORD cbOutput,
                            DWORD *pcbResult, DWORD dwFlags)
{
    FIXME("(%p,%p,%s,%p,%p,%u,%p,%08x): stub\n", (void*)hKey, (void*)hExportKey,
          wine_dbgstr_w(pszBlobType), pParameterList, pbOutput, cbOutput, pcbResult, dwFlags);
    if (pcbResult) *pcbResult = 0;
    return NTE_NOT_SUPPORTED;
}

LONG WINAPI NCryptSetProperty(ULONG_PTR hObject, LPCWSTR pszProperty, PBYTE pbInput,
                              DWORD cbInput, DWORD dwFlags)
{
    FIXME("(%p,%s,%p,%u,%08x): stub\n", (void*)hObject, wine_dbgstr_w(pszProperty),
          pbInput, cbInput, dwFlags);
    return NTE_NOT_SUPPORTED;
}
