/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            Copied from advapi32/reg/reg.c
 * PURPOSE:         Registry functions
 * PROGRAMMER:      Ariadne ( ariadne@xs4all.nl)
 *                  Thomas Weidenmueller <w3seek@reactos.com>
 * UPDATE HISTORY:
 *                  Created 01/11/98
 *                  19990309 EA Stubs
 *                  20050502 Fireball imported some stuff from WINE
 */

#include <stdarg.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>

#include <wine/debug.h>
#include <wine/unicode.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/******************************************************************************
 * load_string [Internal]
 *
 * This is basically a copy of user32/resource.c's LoadStringW. Necessary to
 * avoid importing user32, which is higher level than advapi32. Helper for
 * RegLoadMUIString.
 */
static int load_string(HINSTANCE hModule, UINT resId, LPWSTR pwszBuffer, INT cMaxChars)
{
    HGLOBAL hMemory;
    HRSRC hResource;
    WCHAR *pString;
    int idxString;

    /* Negative values have to be inverted. */
    if (HIWORD(resId) == 0xffff)
        resId = (UINT)(-((INT)resId));

    /* Load the resource into memory and get a pointer to it. */
    hResource = FindResourceW(hModule, MAKEINTRESOURCEW(LOWORD(resId >> 4) + 1), (LPWSTR)RT_STRING);
    if (!hResource) return 0;
    hMemory = LoadResource(hModule, hResource);
    if (!hMemory) return 0;
    pString = LockResource(hMemory);

    /* Strings are length-prefixed. Lowest nibble of resId is an index. */
    idxString = resId & 0xf;
    while (idxString--) pString += *pString + 1;

    /* If no buffer is given, return length of the string. */
    if (!pwszBuffer) return *pString;

    /* Else copy over the string, respecting the buffer size. */
    cMaxChars = (*pString < cMaxChars) ? *pString : (cMaxChars - 1);
    if (cMaxChars >= 0)
    {
        memcpy(pwszBuffer, pString+1, cMaxChars * sizeof(WCHAR));
        pwszBuffer[cMaxChars] = L'\0';
    }

    return cMaxChars;
}

/************************************************************************
 *  RegLoadMUIStringW
 *
 * @implemented
 */
LONG WINAPI
RegLoadMUIStringW(IN HKEY hKey,
                  IN LPCWSTR pszValue  OPTIONAL,
                  OUT LPWSTR pszOutBuf,
                  IN DWORD cbOutBuf,
                  OUT LPDWORD pcbData OPTIONAL,
                  IN DWORD Flags,
                  IN LPCWSTR pszDirectory  OPTIONAL)
{
    DWORD dwValueType, cbData;
    LPWSTR pwszTempBuffer = NULL, pwszExpandedBuffer = NULL;
    LONG result;

    /* Parameter sanity checks. */
    if (!hKey || !pszOutBuf)
        return ERROR_INVALID_PARAMETER;

    if (pszDirectory && *pszDirectory)
    {
        FIXME("BaseDir parameter not yet supported!\n");
        return ERROR_INVALID_PARAMETER;
    }

    /* Check for value existence and correctness of it's type, allocate a buffer and load it. */
    result = RegQueryValueExW(hKey, pszValue, NULL, &dwValueType, NULL, &cbData);
    if (result != ERROR_SUCCESS) goto cleanup;
    if (!(dwValueType == REG_SZ || dwValueType == REG_EXPAND_SZ) || !cbData)
    {
        result = ERROR_FILE_NOT_FOUND;
        goto cleanup;
    }
    pwszTempBuffer = HeapAlloc(GetProcessHeap(), 0, cbData);
    if (!pwszTempBuffer)
    {
        result = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }
    result = RegQueryValueExW(hKey, pszValue, NULL, &dwValueType, (LPBYTE)pwszTempBuffer, &cbData);
    if (result != ERROR_SUCCESS) goto cleanup;

    /* Expand environment variables, if appropriate, or copy the original string over. */
    if (dwValueType == REG_EXPAND_SZ)
    {
        cbData = ExpandEnvironmentStringsW(pwszTempBuffer, NULL, 0) * sizeof(WCHAR);
        if (!cbData) goto cleanup;
        pwszExpandedBuffer = HeapAlloc(GetProcessHeap(), 0, cbData);
        if (!pwszExpandedBuffer)
        {
            result = ERROR_NOT_ENOUGH_MEMORY;
            goto cleanup;
        }
        ExpandEnvironmentStringsW(pwszTempBuffer, pwszExpandedBuffer, cbData);
    }
    else
    {
        pwszExpandedBuffer = HeapAlloc(GetProcessHeap(), 0, cbData);
        memcpy(pwszExpandedBuffer, pwszTempBuffer, cbData);
    }

    /* If the value references a resource based string, parse the value and load the string.
     * Else just copy over the original value. */
    result = ERROR_SUCCESS;
    if (*pwszExpandedBuffer != L'@') /* '@' is the prefix for resource based string entries. */
    {
        lstrcpynW(pszOutBuf, pwszExpandedBuffer, cbOutBuf / sizeof(WCHAR));
    }
    else
    {
        WCHAR *pComma = wcsrchr(pwszExpandedBuffer, L',');
        UINT uiStringId;
        HMODULE hModule;

        /* Format of the expanded value is 'path_to_dll,-resId' */
        if (!pComma || pComma[1] != L'-')
        {
            result = ERROR_BADKEY;
            goto cleanup;
        }

        uiStringId = _wtoi(pComma+2);
        *pComma = L'\0';

        hModule = LoadLibraryExW(pwszExpandedBuffer + 1, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (!hModule || !load_string(hModule, uiStringId, pszOutBuf, cbOutBuf / sizeof(WCHAR)))
            result = ERROR_BADKEY;
        FreeLibrary(hModule);
    }

cleanup:
    HeapFree(GetProcessHeap(), 0, pwszTempBuffer);
    HeapFree(GetProcessHeap(), 0, pwszExpandedBuffer);
    return result;
}

/************************************************************************
 *  RegLoadMUIStringA
 *
 * @implemented
 */
LONG WINAPI
RegLoadMUIStringA(IN HKEY hKey,
                  IN LPCSTR pszValue  OPTIONAL,
                  OUT LPSTR pszOutBuf,
                  IN DWORD cbOutBuf,
                  OUT LPDWORD pcbData OPTIONAL,
                  IN DWORD Flags,
                  IN LPCSTR pszDirectory  OPTIONAL)
{
    UNICODE_STRING valueW, baseDirW;
    WCHAR *pwszBuffer;
    DWORD cbData = cbOutBuf * sizeof(WCHAR);
    LONG result;

    valueW.Buffer = baseDirW.Buffer = pwszBuffer = NULL;
    if (!RtlCreateUnicodeStringFromAsciiz(&valueW, pszValue) ||
        !RtlCreateUnicodeStringFromAsciiz(&baseDirW, pszDirectory) ||
        !(pwszBuffer = HeapAlloc(GetProcessHeap(), 0, cbData)))
    {
        result = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }

    result = RegLoadMUIStringW(hKey, valueW.Buffer, pwszBuffer, cbData, NULL, Flags,
                               baseDirW.Buffer);

    if (result == ERROR_SUCCESS)
    {
        cbData = WideCharToMultiByte(CP_ACP, 0, pwszBuffer, -1, pszOutBuf, cbOutBuf, NULL, NULL);
        if (pcbData)
            *pcbData = cbData;
    }

cleanup:
    HeapFree(GetProcessHeap(), 0, pwszBuffer);
    RtlFreeUnicodeString(&baseDirW);
    RtlFreeUnicodeString(&valueW);

    return result;
}

#include <shlobj.h>

/*************************************************************************
 * SHGetKnownFolderPath           [SHELL32.@]
 *
 * Maps known folder GUIDs to legacy CSIDL values.
 */
HRESULT WINAPI SHGetKnownFolderPath(REFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken, PWSTR *ppszPath)
{
    WCHAR path[MAX_PATH];
    HRESULT hr;
    int csidl = -1;

    FIXME("(%p, 0x%lx, %p, %p): semi-stub\n", rfid, dwFlags, hToken, ppszPath);

    if (!ppszPath)
        return E_INVALIDARG;

    *ppszPath = NULL;

    if (IsEqualGUID(rfid, &FOLDERID_LocalAppData))
        csidl = CSIDL_LOCAL_APPDATA;
    else if (IsEqualGUID(rfid, &FOLDERID_Profile))
        csidl = CSIDL_PROFILE;
    else if (IsEqualGUID(rfid, &FOLDERID_RoamingAppData))
        csidl = CSIDL_APPDATA;
    else if (IsEqualGUID(rfid, &FOLDERID_Desktop))
        csidl = CSIDL_DESKTOP;
    else if (IsEqualGUID(rfid, &FOLDERID_Documents))
        csidl = CSIDL_PERSONAL;
    else if (IsEqualGUID(rfid, &FOLDERID_ProgramFiles))
        csidl = CSIDL_PROGRAM_FILES;
    else if (IsEqualGUID(rfid, &FOLDERID_System))
        csidl = CSIDL_SYSTEM;
    else if (IsEqualGUID(rfid, &FOLDERID_Windows))
        csidl = CSIDL_WINDOWS;
    else if (IsEqualGUID(rfid, &FOLDERID_Music))
        csidl = CSIDL_MYMUSIC;
    else if (IsEqualGUID(rfid, &FOLDERID_Pictures))
        csidl = CSIDL_MYPICTURES;
    else if (IsEqualGUID(rfid, &FOLDERID_Videos))
        csidl = CSIDL_MYVIDEO;
    else if (IsEqualGUID(rfid, &FOLDERID_Programs))
        csidl = CSIDL_PROGRAMS;
    else if (IsEqualGUID(rfid, &FOLDERID_ProgramData))
        csidl = CSIDL_COMMON_APPDATA;
    else if (IsEqualGUID(rfid, &FOLDERID_PublicDocuments))
        csidl = CSIDL_COMMON_DOCUMENTS;
    else if (IsEqualGUID(rfid, &FOLDERID_PublicDesktop))
        csidl = CSIDL_COMMON_DESKTOPDIRECTORY;
    else if (IsEqualGUID(rfid, &FOLDERID_CommonPrograms))
        csidl = CSIDL_COMMON_PROGRAMS;
    else if (IsEqualGUID(rfid, &FOLDERID_CommonStartMenu))
        csidl = CSIDL_COMMON_STARTMENU;
    else if (IsEqualGUID(rfid, &FOLDERID_StartMenu))
        csidl = CSIDL_STARTMENU;
    else if (IsEqualGUID(rfid, &FOLDERID_Startup))
        csidl = CSIDL_STARTUP;
    else if (IsEqualGUID(rfid, &FOLDERID_CommonStartup))
        csidl = CSIDL_COMMON_STARTUP;
    else if (IsEqualGUID(rfid, &FOLDERID_Fonts))
        csidl = CSIDL_FONTS;
    else if (IsEqualGUID(rfid, &FOLDERID_Downloads))
    {
        /* No legacy CSIDL for Downloads — synthesize %USERPROFILE%\Downloads. */
        WCHAR profile[MAX_PATH];
        static const WCHAR downloads[] = L"\\Downloads";
        hr = SHGetFolderPathW(NULL, CSIDL_PROFILE, hToken, 0, profile);
        if (SUCCEEDED(hr))
        {
            *ppszPath = CoTaskMemAlloc((lstrlenW(profile) + ARRAYSIZE(downloads)) * sizeof(WCHAR));
            if (*ppszPath)
            {
                lstrcpyW(*ppszPath, profile);
                lstrcatW(*ppszPath, downloads);
                return S_OK;
            }
            hr = E_OUTOFMEMORY;
        }
        return hr;
    }
    else
    {
        FIXME("Unknown folder ID %s, returning E_INVALIDARG\n", debugstr_guid(rfid));
        return E_INVALIDARG;
    }

    hr = SHGetFolderPathW(NULL, csidl, hToken, 0, path);
    if (SUCCEEDED(hr))
    {
        *ppszPath = CoTaskMemAlloc((lstrlenW(path) + 1) * sizeof(WCHAR));
        if (*ppszPath)
            lstrcpyW(*ppszPath, path);
        else
            hr = E_OUTOFMEMORY;
    }
    return hr;
}

/*************************************************************************
 * SHGetKnownFolderIDList         [SHELL32.@]
 */
HRESULT WINAPI SHGetKnownFolderIDList(REFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken, LPITEMIDLIST *ppidl)
{
    FIXME("(%p, 0x%lx, %p, %p): stub\n", rfid, dwFlags, hToken, ppidl);
    if (ppidl) *ppidl = NULL;
    return E_NOTIMPL;
}

/*************************************************************************
 * SHSetKnownFolderPath           [SHELL32.@]
 */
HRESULT WINAPI SHSetKnownFolderPath(REFKNOWNFOLDERID rfid, DWORD dwFlags, HANDLE hToken, LPCWSTR pszPath)
{
    FIXME("(%p, 0x%lx, %p, %s): stub\n", rfid, dwFlags, hToken, debugstr_w(pszPath));
    return E_NOTIMPL;
}

/*************************************************************************
 * SHGetStockIconInfo           [SHELL32.@]
 *
 * Returns the path and icon index for a stock system icon.
 */
/* SHSTOCKICONINFO is gated by NTDDI_VISTA in shellapi.h. This file does
 * not pull that gate so declare the bits we need locally. */
typedef struct _SH_STOCKICONINFO
{
    DWORD cbSize;
    HICON hIcon;
    int   iSysImageIndex;
    int   iIcon;
    WCHAR szPath[MAX_PATH];
} SH_STOCKICONINFO;

#ifndef SHGSI_ICON
#define SHGSI_ICON       0x000000100  /* same value as SHGFI_ICON */
#endif
#ifndef SHGSI_SMALLICON
#define SHGSI_SMALLICON  0x000000001  /* same value as SHGFI_SMALLICON */
#endif

HRESULT WINAPI SHGetStockIconInfo(int siid, UINT uFlags, void *psii_v)
{
    SH_STOCKICONINFO *psii = psii_v;

    TRACE("(%d, 0x%x, %p)\n", siid, uFlags, psii);

    if (!psii || psii->cbSize != sizeof(*psii))
        return E_INVALIDARG;
    if (siid < 0)
        return E_INVALIDARG;

    /* The stock-icon index in shell32.dll matches the SIID value for the
     * range of icons that ReactOS shell32 actually ships. Apps such as
     * NSIS-based installers only need the resource path/index pair to be
     * filled in so they can hand it to ExtractIconEx; they do not require
     * the loaded HICON unless SHGSI_ICON is requested. */
    GetSystemDirectoryW(psii->szPath, MAX_PATH);
    lstrcatW(psii->szPath, L"\\shell32.dll");
    psii->iIcon = -(siid + 1);
    psii->iSysImageIndex = -1;
    psii->hIcon = NULL;

    if (uFlags & SHGSI_ICON)
    {
        UINT cx = (uFlags & SHGSI_SMALLICON) ? GetSystemMetrics(SM_CXSMICON)
                                             : GetSystemMetrics(SM_CXICON);
        UINT cy = (uFlags & SHGSI_SMALLICON) ? GetSystemMetrics(SM_CYSMICON)
                                             : GetSystemMetrics(SM_CYICON);
        psii->hIcon = (HICON)LoadImageW(GetModuleHandleW(L"shell32.dll"),
                                        MAKEINTRESOURCEW(siid + 1),
                                        IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
        if (!psii->hIcon)
            return S_FALSE;
    }
    return S_OK;
}

/*************************************************************************
 * Shell_NotifyIconGetRect          [SHELL32.@]
 *
 * Returns the bounding rect on screen of a notify-area icon.
 * ReactOS does not yet plumb identifier-keyed notify-icon lookup, so
 * answer "not found" and let callers fall back to default placement.
 */
HRESULT WINAPI Shell_NotifyIconGetRect(const void *identifier, RECT *iconLocation)
{
    FIXME("(%p, %p): stub\n", identifier, iconLocation);
    if (iconLocation)
        SetRectEmpty(iconLocation);
    return E_NOTIMPL;
}
