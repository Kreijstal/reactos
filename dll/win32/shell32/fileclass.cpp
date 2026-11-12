/*
 * PROJECT:     ReactOS shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Per-extension file class cache
 *
 * Listing a folder asks the registry the same questions for every file of a
 * type: is it a shell folder (CLSID\ShellFolder), what is its type name, its
 * DefaultIcon, its IconHandler. Like Windows' file class cache, the answers are
 * kept per extension and per process, and dropped when file associations
 * change: SHChangeNotify(SHCNE_ASSOCCHANGED) bumps a global counter that
 * every process checks before it trusts its cache.
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

EXTERN_C HANDLE SHELL_GetCachedGlobalCounter(HANDLE *phGlobalCounter, REFGUID rguid);

static const GUID SHGCGUID_FileClass = { 0x0d3a3d61, 0x2b84, 0x4e43, { 0x9c, 0x2a, 0x6e, 0x1b, 0x6f, 0x51, 0x88, 0x4a } };
static HANDLE g_hFileClassCounter = NULL;

#define FILECLASS_MAX_ENTRIES 512

struct FILECLASS_ENTRY
{
    FILECLASS_ENTRY *pNext;
    UINT Kind;
    UINT cbData;
    WCHAR szExt[MAX_PATH];
    BYTE Data[ANYSIZE_ARRAY];
};

static FILECLASS_ENTRY *g_pFileClasses = NULL;
static UINT g_cFileClasses = 0;
static LONG g_FileClassCounter = -1;

namespace
{
extern CRITICAL_SECTION g_FileClassCS;
CRITICAL_SECTION_DEBUG g_FileClassCSDebug =
{
    0, 0, &g_FileClassCS,
    { &g_FileClassCSDebug.ProcessLocksList, &g_FileClassCSDebug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": g_FileClassCS") }
};
CRITICAL_SECTION g_FileClassCS = { &g_FileClassCSDebug, -1, 0, 0, 0, 0 };
}

static void SHELL32_FreeFileClassesLocked(void)
{
    while (g_pFileClasses)
    {
        FILECLASS_ENTRY *pNext = g_pFileClasses->pNext;
        LocalFree(g_pFileClasses);
        g_pFileClasses = pNext;
    }
    g_cFileClasses = 0;
}

/* Drop the cache if associations changed since it was filled */
static void SHELL32_CheckFileClassesLocked(void)
{
    LONG Counter = SHGlobalCounterGetValue(SHELL_GetCachedGlobalCounter(&g_hFileClassCounter, SHGCGUID_FileClass));
    if (Counter != g_FileClassCounter)
    {
        SHELL32_FreeFileClassesLocked();
        g_FileClassCounter = Counter;
    }
}

/* Called by SHChangeNotify for SHCNE_ASSOCCHANGED: invalidates the caches of all processes */
EXTERN_C void SHELL32_FileClassesChanged(void)
{
    SHGlobalCounterIncrement(SHELL_GetCachedGlobalCounter(&g_hFileClassCounter, SHGCGUID_FileClass));
}

EXTERN_C BOOL SHELL32_LookupFileClass(UINT Kind, PCWSTR pszExt, PVOID pData, UINT cbData)
{
    BOOL bFound = FALSE;
    if (!pszExt || !*pszExt || wcslen(pszExt) >= MAX_PATH)
        return FALSE;

    EnterCriticalSection(&g_FileClassCS);
    SHELL32_CheckFileClassesLocked();
    for (FILECLASS_ENTRY *p = g_pFileClasses; p; p = p->pNext)
    {
        if (p->Kind == Kind && !_wcsicmp(p->szExt, pszExt))
        {
            ASSERT(p->cbData == cbData);
            CopyMemory(pData, p->Data, cbData);
            bFound = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_FileClassCS);
    return bFound;
}

EXTERN_C void SHELL32_StoreFileClass(UINT Kind, PCWSTR pszExt, LPCVOID pData, UINT cbData)
{
    if (!pszExt || !*pszExt || wcslen(pszExt) >= MAX_PATH)
        return;

    FILECLASS_ENTRY *pEntry = (FILECLASS_ENTRY *)LocalAlloc(LMEM_FIXED, FIELD_OFFSET(FILECLASS_ENTRY, Data) + cbData);
    if (!pEntry)
        return;
    pEntry->Kind = Kind;
    pEntry->cbData = cbData;
    StringCchCopyW(pEntry->szExt, _countof(pEntry->szExt), pszExt);
    CopyMemory(pEntry->Data, pData, cbData);

    EnterCriticalSection(&g_FileClassCS);
    SHELL32_CheckFileClassesLocked();
    if (g_cFileClasses >= FILECLASS_MAX_ENTRIES)
        SHELL32_FreeFileClassesLocked();
    pEntry->pNext = g_pFileClasses;
    g_pFileClasses = pEntry;
    g_cFileClasses++;
    LeaveCriticalSection(&g_FileClassCS);
}
