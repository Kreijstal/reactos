/*
 * Control panel folder
 *
 * Copyright 2003 Martin Fuchs
 * Copyright 2009 Andrew Hill
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <precomp.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell);

static const REGFOLDERINFO g_RegFolderInfo =
{
    PT_CONTROLS_NEWREGITEM,
    0, NULL,
    CLSID_ControlPanel,
    L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}",
    L"ControlPanel",
};

/***********************************************************************
*   Control Panel applet metadata cache
*
* Listing the Control Panel means asking every .cpl in system32 for its display
* name, comment and icon, and the only way to ask is to load the module and
* call its CPlApplet() export.  A plain enumeration therefore costs one
* LoadLibrary()/FreeLibrary() pair per applet plus everything each one drags in
* by way of imports - and some of those imports do real work as they
* initialise.  mmsys.cpl statically imports winmm, whose DllMain loads the
* installed MME drivers, which for wdmaud.drv means enumerating the audio
* device interfaces and walking the KS topology of the sound hardware.  That
* made opening the folder cost time proportional to the machine rather than to
* the number of icons in it, and it was paid again on every single open.
*
* Windows does not re-ask on every open either: it keeps the per-applet display
* data in a cache and reloads an applet only once its file has changed.  Do the
* same.  The registry location below is a ReactOS choice - Windows' own key
* name is not part of any contract - but the semantics follow Windows.
*
* Everything read back is treated as untrusted: a blob that does not validate
* in full is simply not a cache hit, and the applet gets loaded exactly as
* before.  That keeps a damaged or hand-edited cache from ever being able to
* break, shorten or corrupt the Control Panel.
*/

#define CPLCACHE_KEY \
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ControlPanel\\Cache"

#define CPLCACHE_MAGIC       0x314C5043  /* 'CPL1'; bump when the layout changes */
#define CPLCACHE_MAX_APPLETS 64
#define CPLCACHE_MAX_STRING  255         /* applet_info::name and ::info hold 256 WCHARs */
#define CPLCACHE_MAX_BLOB    0x20000     /* refuse to even allocate for more than this */

/* Both structures are all-ULONG so they carry no padding of their own, and
 * every item is placed on a ULONG boundary, so the blob has one single layout
 * on every architecture we build for. */
typedef struct _CPLCACHE_STAMP
{
    ULONG    Magic;
    ULONG    cbTotal;           /* size of the whole blob, cross-checked on read */
    ULONG    nFileSizeLow;
    ULONG    nFileSizeHigh;
    FILETIME ftLastWriteTime;
    FILETIME ftCreationTime;
    ULONG    LangId;            /* see CPanelCache_Stamp() */
    ULONG    cApplets;          /* may legitimately be zero */
} CPLCACHE_STAMP;

typedef struct _CPLCACHE_ITEM
{
    LONG  iIconIdx;
    ULONG cchDisplayName;       /* not counting the terminator */
    ULONG cchComment;           /* not counting the terminator */
    /* Followed by cchDisplayName + 1 and then cchComment + 1 WCHARs, then
     * padding up to the next ULONG boundary so the item after stays aligned. */
} CPLCACHE_ITEM;

static ULONG CPanelCache_ItemSize(ULONG cchDisplayName, ULONG cchComment)
{
    ULONG cb = sizeof(CPLCACHE_ITEM) + (cchDisplayName + cchComment + 2) * sizeof(WCHAR);
    return (cb + 3) & ~3UL;
}

/* Applets report an icon resource id, the pidl wants a resource index, and a
 * negative index is how a pidl asks for an id.  Kept in one place so the
 * cached value and the freshly inquired one cannot drift apart. */
static int CPanelCache_IconIndex(const struct applet_info *pInfo)
{
    return (pInfo->idIcon > 0) ? -pInfo->idIcon : 0;
}

static HKEY CPanelCache_OpenKey(BOOL bWrite)
{
    HKEY hKey;
    LONG lResult;

    if (bWrite)
    {
        lResult = RegCreateKeyExW(HKEY_CURRENT_USER, CPLCACHE_KEY, 0, NULL, 0,
                                  KEY_SET_VALUE, NULL, &hKey, NULL);
    }
    else
    {
        lResult = RegOpenKeyExW(HKEY_CURRENT_USER, CPLCACHE_KEY, 0, KEY_QUERY_VALUE, &hKey);
    }

    return (lResult == ERROR_SUCCESS) ? hKey : NULL;
}

/* Fills in everything that identifies "this exact file, read this exact way".
 * Fails when the file cannot be stat'ed, which simply means no caching. */
static BOOL CPanelCache_Stamp(LPCWSTR pszPath, const WIN32_FIND_DATAW *pFindData,
                              CPLCACHE_STAMP *pStamp)
{
    ZeroMemory(pStamp, sizeof(*pStamp));

    if (pFindData)
    {
        pStamp->nFileSizeLow = pFindData->nFileSizeLow;
        pStamp->nFileSizeHigh = pFindData->nFileSizeHigh;
        pStamp->ftLastWriteTime = pFindData->ftLastWriteTime;
        pStamp->ftCreationTime = pFindData->ftCreationTime;
    }
    else
    {
        WIN32_FILE_ATTRIBUTE_DATA FileData;

        if (!GetFileAttributesExW(pszPath, GetFileExInfoStandard, &FileData))
            return FALSE;

        pStamp->nFileSizeLow = FileData.nFileSizeLow;
        pStamp->nFileSizeHigh = FileData.nFileSizeHigh;
        pStamp->ftLastWriteTime = FileData.ftLastWriteTime;
        pStamp->ftCreationTime = FileData.ftCreationTime;
    }

    pStamp->Magic = CPLCACHE_MAGIC;

    /* The cached name and comment are localized resource strings, so the
     * language that selected them belongs in the key just as much as the file
     * does: change the UI language and the cache has to miss, or the folder
     * would keep showing the previous language.  Both languages that steer
     * resource lookup are folded in, because either one changing can change
     * which string LoadString() hands back. */
    pStamp->LangId = MAKELONG(GetUserDefaultUILanguage(), LANGIDFROMLCID(GetThreadLocale()));
    return TRUE;
}

/* Validates a blob completely - stamp and every item - before the caller adds
 * anything from it, so a rejected blob can never leave a half-filled list
 * behind for the fallback path to duplicate. */
static BOOL CPanelCache_IsUsable(const BYTE *pbData, ULONG cbData, const CPLCACHE_STAMP *pStamp)
{
    const CPLCACHE_STAMP *pCached = (const CPLCACHE_STAMP *)pbData;
    ULONG cbOffset;
    ULONG i;

    if (cbData < sizeof(CPLCACHE_STAMP))
        return FALSE;

    if (pCached->Magic != pStamp->Magic ||
        pCached->cbTotal != cbData ||
        pCached->nFileSizeLow != pStamp->nFileSizeLow ||
        pCached->nFileSizeHigh != pStamp->nFileSizeHigh ||
        pCached->LangId != pStamp->LangId ||
        CompareFileTime(&pCached->ftLastWriteTime, &pStamp->ftLastWriteTime) != 0 ||
        CompareFileTime(&pCached->ftCreationTime, &pStamp->ftCreationTime) != 0)
    {
        return FALSE;
    }

    if (pCached->cApplets > CPLCACHE_MAX_APPLETS)
        return FALSE;

    /* cbOffset never runs past cbData: it starts inside the blob and only
     * advances by an amount the blob has already been shown to hold. */
    cbOffset = sizeof(CPLCACHE_STAMP);
    for (i = 0; i < pCached->cApplets; ++i)
    {
        const CPLCACHE_ITEM *pItem;
        const WCHAR *pszStrings;
        ULONG cbItem;

        if (cbData - cbOffset < sizeof(CPLCACHE_ITEM))
            return FALSE;

        pItem = (const CPLCACHE_ITEM *)(pbData + cbOffset);
        if (pItem->cchDisplayName > CPLCACHE_MAX_STRING ||
            pItem->cchComment > CPLCACHE_MAX_STRING)
        {
            return FALSE;
        }

        cbItem = CPanelCache_ItemSize(pItem->cchDisplayName, pItem->cchComment);
        if (cbData - cbOffset < cbItem)
            return FALSE;

        /* Both strings have to end exactly where the lengths claim they do. */
        pszStrings = (const WCHAR *)(pItem + 1);
        if (pszStrings[pItem->cchDisplayName] != UNICODE_NULL ||
            pszStrings[pItem->cchDisplayName + 1 + pItem->cchComment] != UNICODE_NULL)
        {
            return FALSE;
        }

        cbOffset += cbItem;
    }

    return (cbOffset == cbData);
}

/***********************************************************************
*   control panel implementation in shell namespace
*/

class CControlPanelEnum :
    public CEnumIDListBase
{
    public:
        CControlPanelEnum();
        ~CControlPanelEnum();
        HRESULT WINAPI Initialize(DWORD dwFlags, IEnumIDList* pRegEnumerator);
        BOOL AddAppletsFromCache(LPCWSTR pszPath, const CPLCACHE_STAMP *pStamp);
        VOID SaveAppletsToCache(LPCWSTR pszPath, const CPLCACHE_STAMP *pStamp, const CPlApplet *applet);
        BOOL RegisterCPanelApp(LPCWSTR path, const WIN32_FIND_DATAW *pFindData,
                               CSimpleArray<CPlApplet *> &LoadedApplets);
        int RegisterRegistryCPanelApps(HKEY hkey_root, LPCWSTR szRepPath,
                                       CSimpleArray<CPlApplet *> &LoadedApplets);
        BOOL CreateCPanelEnumList(DWORD dwFlags);

        BEGIN_COM_MAP(CControlPanelEnum)
        COM_INTERFACE_ENTRY_IID(IID_IEnumIDList, IEnumIDList)
        END_COM_MAP()
};

/***********************************************************************
*   IShellFolder [ControlPanel] implementation
*/

static const shvheader ControlPanelSFHeader[] = {
    {IDS_SHV_COLUMN_NAME, SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 20},/*FIXME*/
    {IDS_SHV_COLUMN_COMMENTS, SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 80},/*FIXME*/
};

enum controlpanel_columns
{
    CONTROLPANEL_COL_NAME,
    CONTROLPANEL_COL_COMMENT,
    CONTROLPANEL_COL_COUNT,
};

CControlPanelEnum::CControlPanelEnum()
{
}

CControlPanelEnum::~CControlPanelEnum()
{
}

HRESULT WINAPI CControlPanelEnum::Initialize(DWORD dwFlags, IEnumIDList* pRegEnumerator)
{
    if (CreateCPanelEnumList(dwFlags) == FALSE)
        return E_FAIL;
    AppendItemsFromEnumerator(pRegEnumerator);
    return S_OK;
}

static const CLSID* IsRegItem(LPCITEMIDLIST pidl)
{
    BYTE type = _ILGetType(pidl);
    if (type == PT_CONTROLS_OLDREGITEM || type == PT_CONTROLS_NEWREGITEM)
        return (CLSID*)((BYTE*)pidl + (pidl->mkid.cb - sizeof(CLSID)));
    return NULL;
}

static LPITEMIDLIST _ILCreateCPanelApplet(LPCWSTR pszName, LPCWSTR pszDisplayName, LPCWSTR pszComment, int iIconIdx)
{
    PIDLCPanelStruct *pCP;
    LPITEMIDLIST pidl;
    LPPIDLDATA pData;
    int cchName, cchDisplayName, cchComment, cbData;

    /* Calculate lengths of given strings */
    cchName = wcslen(pszName);
    cchDisplayName = wcslen(pszDisplayName);
    cchComment = wcslen(pszComment);

    /* Allocate PIDL */
    cbData = sizeof(pidl->mkid.cb) + sizeof(pData->type) + sizeof(pData->u.cpanel) - sizeof(pData->u.cpanel.szName)
             + (cchName + cchDisplayName + cchComment + 3) * sizeof(WCHAR);
    pidl = (LPITEMIDLIST)SHAlloc(cbData + sizeof(WORD));
    if (!pidl)
        return NULL;

    /* Copy data to allocated memory */
    pidl->mkid.cb = cbData;
    pData = (PIDLDATA *)pidl->mkid.abID;
    pData->type = PT_CPLAPPLET;

    pCP = &pData->u.cpanel;
    pCP->dummy = 0;
    pCP->iconIdx = iIconIdx;
    wcscpy(pCP->szName, pszName);
    pCP->offsDispName = cchName + 1;
    wcscpy(pCP->szName + pCP->offsDispName, pszDisplayName);
    pCP->offsComment = pCP->offsDispName + cchDisplayName + 1;
    wcscpy(pCP->szName + pCP->offsComment, pszComment);

    /* Add PIDL NULL terminator */
    *(WORD*)(pCP->szName + pCP->offsComment + cchComment + 1) = 0;

    pcheck(pidl);

    return pidl;
}

/**************************************************************************
 *  _ILGetCPanelPointer()
 * gets a pointer to the control panel struct stored in the pidl
 */
static PIDLCPanelStruct *_ILGetCPanelPointer(LPCITEMIDLIST pidl)
{
    LPPIDLDATA pdata = _ILGetDataPointer(pidl);

    if (pdata && pdata->type == PT_CPLAPPLET)
        return (PIDLCPanelStruct *) & (pdata->u.cpanel);

    return NULL;
}

HRESULT CCPLExtractIcon_CreateInstance(IShellFolder * psf, LPCITEMIDLIST pidl, REFIID riid, LPVOID * ppvOut)
{
    PIDLCPanelStruct *pData = _ILGetCPanelPointer(pidl);
    if (!pData)
        return E_FAIL;

    CComPtr<IDefaultExtractIconInit> initIcon;
    HRESULT hr = SHCreateDefaultExtractIcon(IID_PPV_ARG(IDefaultExtractIconInit, &initIcon));
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    initIcon->SetNormalIcon(pData->szName, (int)pData->iconIdx != -1 ? pData->iconIdx : 0);

    return initIcon->QueryInterface(riid, ppvOut);
}

BOOL CControlPanelEnum::AddAppletsFromCache(LPCWSTR pszPath, const CPLCACHE_STAMP *pStamp)
{
    const CPLCACHE_STAMP *pCached;
    HKEY hKey;
    BYTE *pbData;
    DWORD dwType = 0, cbData = 0;
    ULONG cbOffset, i;

    hKey = CPanelCache_OpenKey(FALSE);
    if (!hKey)
        return FALSE;

    if (RegQueryValueExW(hKey, pszPath, NULL, &dwType, NULL, &cbData) != ERROR_SUCCESS ||
        dwType != REG_BINARY || cbData < sizeof(CPLCACHE_STAMP) || cbData > CPLCACHE_MAX_BLOB)
    {
        RegCloseKey(hKey);
        return FALSE;
    }

    pbData = (BYTE *)SHAlloc(cbData);
    if (!pbData)
    {
        RegCloseKey(hKey);
        return FALSE;
    }

    if (RegQueryValueExW(hKey, pszPath, NULL, &dwType, pbData, &cbData) != ERROR_SUCCESS ||
        dwType != REG_BINARY ||
        !CPanelCache_IsUsable(pbData, cbData, pStamp))
    {
        SHFree(pbData);
        RegCloseKey(hKey);
        return FALSE;
    }

    pCached = (const CPLCACHE_STAMP *)pbData;
    cbOffset = sizeof(CPLCACHE_STAMP);
    for (i = 0; i < pCached->cApplets; ++i)
    {
        const CPLCACHE_ITEM *pItem = (const CPLCACHE_ITEM *)(pbData + cbOffset);
        const WCHAR *pszName = (const WCHAR *)(pItem + 1);
        const WCHAR *pszComment = pszName + pItem->cchDisplayName + 1;
        LPITEMIDLIST pidl = _ILCreateCPanelApplet(pszPath, pszName, pszComment, pItem->iIconIdx);

        if (pidl)
            AddToEnumList(pidl);

        cbOffset += CPanelCache_ItemSize(pItem->cchDisplayName, pItem->cchComment);
    }

    SHFree(pbData);
    RegCloseKey(hKey);

    /* A hit even when the applet contributed no items: "this file yields
     * nothing" is just as much a result worth not recomputing, and it is the
     * expensive answer for an applet like bthprops.cpl that has to go and look
     * for hardware before it can say so. */
    return TRUE;
}

VOID CControlPanelEnum::SaveAppletsToCache(LPCWSTR pszPath, const CPLCACHE_STAMP *pStamp,
                                           const CPlApplet *applet)
{
    size_t cchName[CPLCACHE_MAX_APPLETS];
    size_t cchComment[CPLCACHE_MAX_APPLETS];
    CPLCACHE_STAMP *pStored;
    HKEY hKey;
    BYTE *pbData;
    ULONG cApplets = applet ? applet->count : 0;
    ULONG cbTotal = sizeof(CPLCACHE_STAMP);
    ULONG cbOffset, i;

    if (cApplets > CPLCACHE_MAX_APPLETS)
        return;

    for (i = 0; i < cApplets; ++i)
    {
        /* StringCchLengthW also proves the buffers are terminated, which the
         * CPL_NEWINQUIRE path does not guarantee on its own. */
        if (FAILED(StringCchLengthW(applet->info[i].name, _countof(applet->info[i].name), &cchName[i])) ||
            FAILED(StringCchLengthW(applet->info[i].info, _countof(applet->info[i].info), &cchComment[i])))
        {
            return;
        }

        cbTotal += CPanelCache_ItemSize((ULONG)cchName[i], (ULONG)cchComment[i]);
    }

    hKey = CPanelCache_OpenKey(TRUE);
    if (!hKey)
        return;

    pbData = (BYTE *)SHAlloc(cbTotal);
    if (!pbData)
    {
        RegCloseKey(hKey);
        return;
    }

    /* Zero first so the alignment padding is deterministic rather than heap
     * leftovers being written into the user's registry. */
    ZeroMemory(pbData, cbTotal);

    pStored = (CPLCACHE_STAMP *)pbData;
    *pStored = *pStamp;
    pStored->cbTotal = cbTotal;
    pStored->cApplets = cApplets;

    cbOffset = sizeof(CPLCACHE_STAMP);
    for (i = 0; i < cApplets; ++i)
    {
        CPLCACHE_ITEM *pItem = (CPLCACHE_ITEM *)(pbData + cbOffset);
        WCHAR *pszStrings = (WCHAR *)(pItem + 1);

        pItem->iIconIdx = CPanelCache_IconIndex(&applet->info[i]);
        pItem->cchDisplayName = (ULONG)cchName[i];
        pItem->cchComment = (ULONG)cchComment[i];
        wcscpy(pszStrings, applet->info[i].name);
        wcscpy(pszStrings + cchName[i] + 1, applet->info[i].info);

        cbOffset += CPanelCache_ItemSize((ULONG)cchName[i], (ULONG)cchComment[i]);
    }

    RegSetValueExW(hKey, pszPath, 0, REG_BINARY, pbData, cbTotal);

    SHFree(pbData);
    RegCloseKey(hKey);
}

BOOL CControlPanelEnum::RegisterCPanelApp(LPCWSTR wpath, const WIN32_FIND_DATAW *pFindData,
                                          CSimpleArray<CPlApplet *> &LoadedApplets)
{
    CPLCACHE_STAMP Stamp;
    CPlApplet *applet;
    BOOL bNotAnApplet = FALSE;
    BOOL bHaveStamp = CPanelCache_Stamp(wpath, pFindData, &Stamp);

    /* Without a stamp there is nothing that could tell a fresh cache entry from
     * a stale one, so such an applet is neither read from nor written to the
     * cache and behaves exactly as it did before. */
    if (bHaveStamp && AddAppletsFromCache(wpath, &Stamp))
        return TRUE;

    applet = Control_LoadApplet(0, wpath, NULL, &bNotAnApplet);

    if (applet)
    {
        for (UINT i = 0; i < applet->count; ++i)
        {
            LPITEMIDLIST pidl = _ILCreateCPanelApplet(wpath,
                                                      applet->info[i].name,
                                                      applet->info[i].info,
                                                      CPanelCache_IconIndex(&applet->info[i]));

            if (pidl)
                AddToEnumList(pidl);
        }
    }

    /* Only an answer that is a property of the file itself gets remembered.  A
     * load that merely failed this time round (out of memory, say) must not be
     * turned into a permanently missing applet. */
    if (bHaveStamp && (applet || bNotAnApplet))
        SaveAppletsToCache(wpath, &Stamp, applet);

    /* The module stays loaded until the whole enumeration is over.  Several
     * applets share dependencies - devmgr.dll, newdev.dll and powrprof.dll are
     * each pulled in by two or three of them - and ReactOS releases an image
     * section's pages as soon as its last reference goes, so unloading between
     * applets means reading those modules off the disk again for the next one. */
    if (applet && !LoadedApplets.Add(applet))
        Control_UnloadApplet(applet);

    return TRUE;
}

int CControlPanelEnum::RegisterRegistryCPanelApps(HKEY hkey_root, LPCWSTR szRepPath,
                                                 CSimpleArray<CPlApplet *> &LoadedApplets)
{
    WCHAR name[MAX_PATH];
    WCHAR value[MAX_PATH];
    HKEY hkey;

    int cnt = 0;

    if (RegOpenKeyW(hkey_root, szRepPath, &hkey) == ERROR_SUCCESS)
    {
        int idx = 0;

        for(; ; idx++)
        {
            DWORD nameLen = MAX_PATH;
            DWORD valueLen = MAX_PATH;
            WCHAR buffer[MAX_PATH];

            if (RegEnumValueW(hkey, idx, name, &nameLen, NULL, NULL, (LPBYTE)&value, &valueLen) != ERROR_SUCCESS)
                break;

            if (ExpandEnvironmentStringsW(value, buffer, MAX_PATH))
            {
                wcscpy(value, buffer);
            }

            /* No WIN32_FIND_DATAW to hand here, so the stamp is taken from the
             * file itself; that is one metadata query against a saving of a
             * whole DLL load, and it keeps registry-listed applets such as
             * console.dll cached on exactly the same terms as the globbed ones. */
            if (RegisterCPanelApp(value, NULL, LoadedApplets))
                ++cnt;
        }
        RegCloseKey(hkey);
    }

    return cnt;
}

/**************************************************************************
 *  CControlPanelEnum::CreateCPanelEnumList()
 */
BOOL CControlPanelEnum::CreateCPanelEnumList(DWORD dwFlags)
{
    WCHAR szPath[MAX_PATH];
    WIN32_FIND_DATAW wfd;
    HANDLE hFile;
    CSimpleArray<CPlApplet *> LoadedApplets;

    TRACE("(%p)->(flags=0x%08x)\n", this, dwFlags);

    /* enumerate the control panel applets */
    if (dwFlags & SHCONTF_NONFOLDERS)
    {
        LPWSTR p;

        GetSystemDirectoryW(szPath, MAX_PATH);
        p = PathAddBackslashW(szPath);
        wcscpy(p, L"*.cpl");

        hFile = FindFirstFileW(szPath, &wfd);

        if (hFile != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(dwFlags & SHCONTF_INCLUDEHIDDEN) && (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
                    continue;

                if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    wcscpy(p, wfd.cFileName);
                    if (wcscmp(wfd.cFileName, L"ncpa.cpl"))
                        RegisterCPanelApp(szPath, &wfd, LoadedApplets);
                }
            } while(FindNextFileW(hFile, &wfd));
            FindClose(hFile);
        }

        RegisterRegistryCPanelApps(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Cpls", LoadedApplets);
        RegisterRegistryCPanelApps(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Cpls", LoadedApplets);

        /* Now that nothing else is going to be inquired, let the modules go. */
        for (int i = 0; i < LoadedApplets.GetSize(); ++i)
            Control_UnloadApplet(LoadedApplets[i]);
    }
    return TRUE;
}

CControlPanelFolder::CControlPanelFolder()
{
    pidlRoot = NULL;    /* absolute pidl */
}

CControlPanelFolder::~CControlPanelFolder()
{
    TRACE("-- destroying IShellFolder(%p)\n", this);
    SHFree(pidlRoot);
}

/**************************************************************************
*    CControlPanelFolder::ParseDisplayName
*/
HRESULT WINAPI CControlPanelFolder::ParseDisplayName(
    HWND hwndOwner,
    LPBC pbc,
    LPOLESTR lpszDisplayName,
    DWORD *pchEaten,
    PIDLIST_RELATIVE *ppidl,
    DWORD *pdwAttributes)
{
    /* We only support parsing guid names */
    return m_regFolder->ParseDisplayName(hwndOwner, pbc, lpszDisplayName, pchEaten, ppidl, pdwAttributes);
}

/**************************************************************************
*        CControlPanelFolder::EnumObjects
*/
HRESULT WINAPI CControlPanelFolder::EnumObjects(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST *ppEnumIDList)
{
    CComPtr<IEnumIDList> pRegEnumerator;
    m_regFolder->EnumObjects(hwndOwner, dwFlags, &pRegEnumerator);

    return ShellObjectCreatorInit<CControlPanelEnum>(dwFlags, pRegEnumerator, IID_PPV_ARG(IEnumIDList, ppEnumIDList));
}

/**************************************************************************
*        CControlPanelFolder::BindToObject
*/
HRESULT WINAPI CControlPanelFolder::BindToObject(
    PCUIDLIST_RELATIVE pidl,
    LPBC pbcReserved,
    REFIID riid,
    LPVOID *ppvOut)
{
    return m_regFolder->BindToObject(pidl, pbcReserved, riid, ppvOut);
}

/**************************************************************************
*    CControlPanelFolder::BindToStorage
*/
HRESULT WINAPI CControlPanelFolder::BindToStorage(
    PCUIDLIST_RELATIVE pidl,
    LPBC pbcReserved,
    REFIID riid,
    LPVOID *ppvOut)
{
    FIXME("(%p)->(pidl=%p,%p,%s,%p) stub\n", this, pidl, pbcReserved, shdebugstr_guid(&riid), ppvOut);

    *ppvOut = NULL;
    return E_NOTIMPL;
}

/**************************************************************************
*     CControlPanelFolder::CompareIDs
*/
HRESULT WINAPI CControlPanelFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    /* Dont use SHELL32_CompareGuidItems because it would cause guid items to come first */
    if (_ILIsSpecialFolder(pidl1) || _ILIsSpecialFolder(pidl2))
    {
        return SHELL32_CompareDetails(this, lParam, pidl1, pidl2);
    }
    PIDLCPanelStruct *pData1 = _ILGetCPanelPointer(pidl1);
    PIDLCPanelStruct *pData2 = _ILGetCPanelPointer(pidl2);

    if (!pData1 || !pData2 || LOWORD(lParam) >= CONTROLPANEL_COL_COUNT)
        return E_INVALIDARG;

    int result;
    switch(LOWORD(lParam))
    {
        case CONTROLPANEL_COL_NAME:
            result = SHELL_StrCmpLogical(pData1->szName + pData1->offsDispName, pData2->szName + pData2->offsDispName);
            break;
        case CONTROLPANEL_COL_COMMENT:
            result = SHELL_StrCmpLogical(pData1->szName + pData1->offsComment, pData2->szName + pData2->offsComment);
            break;
        default:
            ERR("Got wrong lParam!\n");
            return E_INVALIDARG;
    }

    return MAKE_COMPARE_HRESULT(result);
}

/**************************************************************************
*    CControlPanelFolder::CreateViewObject
*/
HRESULT WINAPI CControlPanelFolder::CreateViewObject(HWND hwndOwner, REFIID riid, LPVOID * ppvOut)
{
    CComPtr<IShellView>                    pShellView;
    HRESULT hr = E_INVALIDARG;

    TRACE("(%p)->(hwnd=%p,%s,%p)\n", this, hwndOwner, shdebugstr_guid(&riid), ppvOut);

    if (ppvOut) {
        *ppvOut = NULL;

        if (IsEqualIID(riid, IID_IDropTarget)) {
            WARN("IDropTarget not implemented\n");
            hr = E_NOTIMPL;
        } else if (IsEqualIID(riid, IID_IContextMenu)) {
            WARN("IContextMenu not implemented\n");
            hr = E_NOTIMPL;
        } else if (IsEqualIID(riid, IID_IShellView)) {
            SFV_CREATE sfvparams = { sizeof(SFV_CREATE), this , NULL, this };
            hr = SHCreateShellFolderView(&sfvparams, (IShellView**)ppvOut);
        }
    }
    TRACE("--(%p)->(interface=%p)\n", this, ppvOut);
    return hr;
}

/**************************************************************************
*  CControlPanelFolder::GetAttributesOf
*/
HRESULT WINAPI CControlPanelFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD * rgfInOut)
{
    HRESULT hr = S_OK;
    static const DWORD dwControlPanelAttributes =
        SFGAO_HASSUBFOLDER | SFGAO_FOLDER | SFGAO_CANLINK;

    TRACE("(%p)->(cidl=%d apidl=%p mask=%p (0x%08x))\n",
          this, cidl, apidl, rgfInOut, rgfInOut ? *rgfInOut : 0);

    if (!rgfInOut)
        return E_INVALIDARG;
    if (cidl && !apidl)
        return E_INVALIDARG;

    if (*rgfInOut == 0)
        *rgfInOut = ~0;

    if (!cidl)
    {
        *rgfInOut &= dwControlPanelAttributes;
    }
    else
    {
        while(cidl > 0 && *apidl)
        {
            pdump(*apidl);
            if (_ILIsCPanelStruct(*apidl))
                *rgfInOut &= SFGAO_CANLINK;
            else if (_ILIsSpecialFolder(*apidl))
                m_regFolder->GetAttributesOf(1, apidl, rgfInOut);
            else
                ERR("Got unknown pidl\n");
            apidl++;
            cidl--;
        }
    }
    /* make sure SFGAO_VALIDATE is cleared, some apps depend on that */
    *rgfInOut &= ~SFGAO_VALIDATE;

    TRACE("-- result=0x%08x\n", *rgfInOut);
    return hr;
}

/**************************************************************************
*    CControlPanelFolder::GetUIObjectOf
*
* PARAMETERS
*  HWND           hwndOwner, //[in ] Parent window for any output
*  UINT           cidl,      //[in ] array size
*  LPCITEMIDLIST* apidl,     //[in ] simple pidl array
*  REFIID         riid,      //[in ] Requested Interface
*  UINT*          prgfInOut, //[   ] reserved
*  LPVOID*        ppvObject) //[out] Resulting Interface
*
*/
HRESULT WINAPI CControlPanelFolder::GetUIObjectOf(HWND hwndOwner,
        UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid, UINT * prgfInOut, LPVOID * ppvOut)
{
    LPVOID pObj = NULL;
    HRESULT hr = E_INVALIDARG;

    TRACE("(%p)->(%p,%u,apidl=%p,%s,%p,%p)\n",
          this, hwndOwner, cidl, apidl, shdebugstr_guid(&riid), prgfInOut, ppvOut);

    if (ppvOut) {
        *ppvOut = NULL;

        if (IsEqualIID(riid, IID_IContextMenu) && (cidl >= 1)) {
            /* HACK: We should use callbacks from CDefaultContextMenu instead of creating one on our own */
            BOOL bHasCpl = FALSE;
            for (UINT i = 0; i < cidl; i++)
            {
                if(_ILIsCPanelStruct(apidl[i]))
                {
                    bHasCpl = TRUE;
                }
            }

            if (bHasCpl)
                hr = ShellObjectCreatorInit<CCPLItemMenu>(cidl, apidl, riid, &pObj);
            else
                hr = m_regFolder->GetUIObjectOf(hwndOwner, cidl, apidl, riid, prgfInOut, &pObj);
        } else if (IsEqualIID(riid, IID_IDataObject) && (cidl >= 1)) {
            hr = IDataObject_Constructor(hwndOwner, pidlRoot, apidl, cidl, TRUE, (IDataObject **)&pObj);
        } else if ((IsEqualIID(riid, IID_IExtractIconA) || IsEqualIID(riid, IID_IExtractIconW)) && (cidl == 1)) {
            if (_ILGetCPanelPointer(apidl[0]))
                hr = CCPLExtractIcon_CreateInstance(this, apidl[0], riid, &pObj);
            else
                hr = m_regFolder->GetUIObjectOf(hwndOwner, cidl, apidl, riid, prgfInOut, &pObj);
        } else {
            hr = E_NOINTERFACE;
        }

        if (SUCCEEDED(hr) && !pObj)
            hr = E_OUTOFMEMORY;

        *ppvOut = pObj;
    }
    TRACE("(%p)->hr=0x%08x\n", this, hr);
    return hr;
}

/**************************************************************************
*    CControlPanelFolder::GetDisplayNameOf
*/
HRESULT WINAPI CControlPanelFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET strRet)
{
    if (!pidl)
        return S_FALSE;

    PIDLCPanelStruct *pCPanel = _ILGetCPanelPointer(pidl);

    if (pCPanel)
    {
        return SHSetStrRet(strRet, pCPanel->szName + pCPanel->offsDispName);
    }
    else if (_ILIsSpecialFolder(pidl))
    {
        return m_regFolder->GetDisplayNameOf(pidl, dwFlags, strRet);
    }

    return E_FAIL;
}

/**************************************************************************
*  CControlPanelFolder::SetNameOf
*  Changes the name of a file object or subfolder, possibly changing its item
*  identifier in the process.
*
* PARAMETERS
*  HWND          hwndOwner,  //[in ] Owner window for output
*  LPCITEMIDLIST pidl,       //[in ] simple pidl of item to change
*  LPCOLESTR     lpszName,   //[in ] the items new display name
*  DWORD         dwFlags,    //[in ] SHGNO formatting flags
*  LPITEMIDLIST* ppidlOut)   //[out] simple pidl returned
*/
HRESULT WINAPI CControlPanelFolder::SetNameOf(HWND hwndOwner, PCUITEMID_CHILD pidl,    /*simple pidl */
        LPCOLESTR lpName, DWORD dwFlags, PITEMID_CHILD *pPidlOut)
{
    FIXME("(%p)->(%p,pidl=%p,%s,%u,%p)\n", this, hwndOwner, pidl, debugstr_w(lpName), dwFlags, pPidlOut);
    return E_FAIL;
}

HRESULT WINAPI CControlPanelFolder::GetDefaultSearchGUID(GUID *pguid)
{
    FIXME("(%p)\n", this);
    return E_NOTIMPL;
}

HRESULT WINAPI CControlPanelFolder::EnumSearches(IEnumExtraSearch **ppenum)
{
    FIXME("(%p)\n", this);
    return E_NOTIMPL;
}

HRESULT WINAPI CControlPanelFolder::GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay)
{
    TRACE("(%p)\n", this);

    if (pSort) *pSort = 0;
    if (pDisplay) *pDisplay = 0;
    return S_OK;
}

HRESULT WINAPI CControlPanelFolder::GetDefaultColumnState(UINT iColumn, DWORD *pcsFlags)
{
    TRACE("(%p)\n", this);

    if (!pcsFlags || iColumn >= CONTROLPANEL_COL_COUNT)
        return E_INVALIDARG;
    *pcsFlags = ControlPanelSFHeader[iColumn].colstate;
    return S_OK;
}

HRESULT WINAPI CControlPanelFolder::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    if (IsRegItem(pidl))
        return m_regFolder->GetDetailsEx(pidl, pscid, pv);
    return SH32_GetDetailsOfPKeyAsVariant(this, pidl, pscid, pv, FALSE);
}

HRESULT WINAPI CControlPanelFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd)
{
    if (!psd || iColumn >= CONTROLPANEL_COL_COUNT)
        return E_INVALIDARG;

    if (!pidl)
    {
        psd->fmt = ControlPanelSFHeader[iColumn].fmt;
        psd->cxChar = ControlPanelSFHeader[iColumn].cxChar;
        return SHSetStrRet(&psd->str, shell32_hInstance, ControlPanelSFHeader[iColumn].colnameid);
    }
    else if (_ILIsSpecialFolder(pidl))
    {
        return m_regFolder->GetDetailsOf(pidl, iColumn, psd);
    }
    else
    {
        PIDLCPanelStruct *pCPanel = _ILGetCPanelPointer(pidl);

        if (!pCPanel)
            return E_FAIL;

        switch(iColumn)
        {
            case CONTROLPANEL_COL_NAME:
                return SHSetStrRet(&psd->str, pCPanel->szName + pCPanel->offsDispName);
            case CONTROLPANEL_COL_COMMENT:
                return SHSetStrRet(&psd->str, pCPanel->szName + pCPanel->offsComment);
        }
    }

    return E_FAIL;
}

HRESULT WINAPI CControlPanelFolder::MapColumnToSCID(UINT column, SHCOLUMNID *pscid)
{
    switch (column)
    {
        case CONTROLPANEL_COL_NAME: return MakeSCID(*pscid, FMTID_Storage, PID_STG_NAME);
        case CONTROLPANEL_COL_COMMENT: return MakeSCID(*pscid, FMTID_SummaryInformation, PIDSI_COMMENTS);
    }
    return E_INVALIDARG;
}

/************************************************************************
 *    CControlPanelFolder::GetClassID
 */
HRESULT WINAPI CControlPanelFolder::GetClassID(CLSID *lpClassId)
{
    TRACE("(%p)\n", this);

    if (!lpClassId)
        return E_POINTER;
    *lpClassId = CLSID_ControlPanel;

    return S_OK;
}

/************************************************************************
 *    CControlPanelFolder::Initialize
 *
 * NOTES: it makes no sense to change the pidl
 */
HRESULT WINAPI CControlPanelFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    if (pidlRoot)
        SHFree((LPVOID)pidlRoot);

    pidlRoot = ILClone(pidl);

    /* Create the inner reg folder */
    REGFOLDERINITDATA RegInit = { static_cast<IShellFolder*>(this), &g_RegFolderInfo };
    HRESULT hr;
    hr = CRegFolder_CreateInstance(&RegInit,
                                   pidlRoot,
                                   IID_PPV_ARG(IShellFolder2, &m_regFolder));
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    return S_OK;
}

/**************************************************************************
 *    CControlPanelFolder::GetCurFolder
 */
HRESULT WINAPI CControlPanelFolder::GetCurFolder(PIDLIST_ABSOLUTE * pidl)
{
    TRACE("(%p)->(%p)\n", this, pidl);

    if (!pidl)
        return E_POINTER;
    *pidl = ILClone(pidlRoot);
    return S_OK;
}

HRESULT WINAPI CControlPanelFolder::MessageSFVCB(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case SFVM_DEFVIEWMODE:
        {
        #if ROSPOLICY_CONTROLSFOLDER_DEFLARGEICONS
            *((FOLDERVIEWMODE*)lParam) = FVM_ICON;
        #else
            *((FOLDERVIEWMODE*)lParam) = IsOS(OS_SERVERADMINUI) ? FVM_LIST : FVM_ICON;
        #endif
            return S_OK;
        }
    }
    return E_NOTIMPL;
}

CCPLItemMenu::CCPLItemMenu()
{
    m_apidl = NULL;
    m_cidl = 0;
}

HRESULT WINAPI CCPLItemMenu::Initialize(UINT cidl, PCUITEMID_CHILD_ARRAY apidl)
{
    m_cidl = cidl;
    m_apidl = _ILCopyaPidl(apidl, m_cidl);
    if (m_cidl && !m_apidl)
        return E_OUTOFMEMORY;

    return S_OK;
}

CCPLItemMenu::~CCPLItemMenu()
{
    _ILFreeaPidl(m_apidl, m_cidl);
}

HRESULT WINAPI CCPLItemMenu::QueryContextMenu(
    HMENU hMenu,
    UINT indexMenu,
    UINT idCmdFirst,
    UINT idCmdLast,
    UINT uFlags)
{
    _InsertMenuItemW(hMenu, indexMenu++, TRUE, idCmdFirst, MFT_STRING, MAKEINTRESOURCEW(IDS_OPEN), MFS_DEFAULT);
    _InsertMenuItemW(hMenu, indexMenu++, TRUE, IDC_STATIC, MFT_SEPARATOR, NULL, MFS_ENABLED);
    _InsertMenuItemW(hMenu, indexMenu++, TRUE, idCmdFirst + 1, MFT_STRING, MAKEINTRESOURCEW(IDS_CREATELINK), MFS_ENABLED);

    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 2);
}

EXTERN_C
void WINAPI Control_RunDLLW(HWND hWnd, HINSTANCE hInst, LPCWSTR cmd, DWORD nCmdShow);

/**************************************************************************
* ICPanel_IContextMenu_InvokeCommand()
*/
HRESULT WINAPI CCPLItemMenu::InvokeCommand(LPCMINVOKECOMMANDINFO lpcmi)
{
    HRESULT hResult;

    PIDLCPanelStruct *pCPanel = _ILGetCPanelPointer(m_apidl[0]);
    if(!pCPanel)
        return E_FAIL;

    TRACE("(%p)->(invcom=%p verb=%p wnd=%p)\n", this, lpcmi, lpcmi->lpVerb, lpcmi->hwnd);

    if (lpcmi->lpVerb == MAKEINTRESOURCEA(0))
    {
        /* Hardcode the command here; Executing a cpl file would be fine but we also need to run things like console.dll */
        WCHAR wszParams[MAX_PATH];
        PCWSTR wszFile = L"rundll32.exe";
        PCWSTR wszFormat = L"shell32.dll,Control_RunDLL %s,%s";

        wsprintfW(wszParams, wszFormat, pCPanel->szName, pCPanel->szName + pCPanel->offsDispName);

        /* Note: we pass the applet name to Control_RunDLL to distinguish between multiple applets in one .cpl file */
        ShellExecuteW(NULL, NULL, wszFile, wszParams, NULL, 0);
    }
    else if (lpcmi->lpVerb == MAKEINTRESOURCEA(1)) //FIXME
    {
        CComPtr<IDataObject> pDataObj;
        LPITEMIDLIST pidl = _ILCreateControlPanel();

        hResult = SHCreateDataObject(pidl, m_cidl, m_apidl, NULL, IID_PPV_ARG(IDataObject, &pDataObj));
        if (FAILED(hResult))
            return hResult;

        SHFree(pidl);

        //FIXME: Use SHCreateLinks
        CComPtr<IShellFolder> psf;
        CComPtr<IDropTarget> pDT;

        hResult = SHGetDesktopFolder(&psf);
        if (FAILED(hResult))
            return hResult;

        hResult = psf->CreateViewObject(NULL, IID_PPV_ARG(IDropTarget, &pDT));
        if (FAILED(hResult))
            return hResult;

        SHSimulateDrop(pDT, pDataObj, MK_CONTROL|MK_SHIFT, NULL, NULL);
    }
    return S_OK;
}

/**************************************************************************
 *  ICPanel_IContextMenu_GetCommandString()
 *
 */
HRESULT WINAPI CCPLItemMenu::GetCommandString(
    UINT_PTR idCommand,
    UINT uFlags,
    UINT* lpReserved,
    LPSTR lpszName,
    UINT uMaxNameLen)
{
    TRACE("(%p)->(idcom=%lx flags=%x %p name=%p len=%x)\n", this, idCommand, uFlags, lpReserved, lpszName, uMaxNameLen);

    FIXME("unknown command string\n");
    return E_FAIL;
}

/**************************************************************************
* ICPanel_IContextMenu_HandleMenuMsg()
*/
HRESULT WINAPI CCPLItemMenu::HandleMenuMsg(
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    TRACE("ICPanel_IContextMenu_HandleMenuMsg (%p)->(msg=%x wp=%lx lp=%lx)\n", this, uMsg, wParam, lParam);

    return E_NOTIMPL;
}

/**************************************************************************
* COpenControlPanel
*/

static HRESULT GetParsingName(PCIDLIST_ABSOLUTE pidl, PWSTR*Name)
{
    PIDLIST_ABSOLUTE pidlFree = NULL;
    if (IS_INTRESOURCE(pidl))
    {
        HRESULT hr = SHGetSpecialFolderLocation(NULL, (UINT)(SIZE_T)pidl, &pidlFree);
        if (FAILED(hr))
            return hr;
        pidl = pidlFree;
    }
    HRESULT hr = SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, Name);
    ILFree(pidlFree);
    return hr;
}

static HRESULT CreateCplAbsoluteParsingPath(LPCWSTR Prefix, LPCWSTR InFolderParse, PWSTR Buf, UINT cchBuf)
{
    PWSTR cpfolder;
    HRESULT hr = GetParsingName((PCIDLIST_ABSOLUTE)CSIDL_CONTROLS, &cpfolder);
    if (SUCCEEDED(hr))
    {
        hr = StringCchPrintfW(Buf, cchBuf, L"%s\\%s%s", cpfolder, Prefix, InFolderParse);
        SHFree(cpfolder);
    }
    return hr;
}

static HRESULT FindExeCplClass(LPCWSTR Canonical, HKEY hKey, BOOL Wow64, LPWSTR clsid)
{
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    HKEY hNSKey;
    WCHAR key[MAX_PATH], buf[MAX_PATH];
    wsprintfW(key, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\%s\\NameSpace",
              Wow64 ? L"ControlPanelWOW64" : L"ControlPanel");
    LSTATUS error = RegOpenKeyExW(hKey, key, 0, KEY_READ, &hNSKey);
    if (error)
        return HRESULT_FROM_WIN32(error);
    for (DWORD i = 0; RegEnumKeyW(hNSKey, i, key, _countof(key)) == ERROR_SUCCESS; ++i)
    {
        IID validate;
        if (SUCCEEDED(IIDFromString(key, &validate)))
        {
            wsprintfW(buf, L"CLSID\\%s", key);
            DWORD cb = sizeof(buf);
            if (RegGetValueW(HKEY_CLASSES_ROOT, buf, L"System.ApplicationName",
                             RRF_RT_REG_SZ, NULL, buf, &cb) == ERROR_SUCCESS)
            {
                if (!lstrcmpiW(buf, Canonical))
                {
                    lstrcpyW(clsid, key);
                    hr = S_OK;
                }
            }
        }
    }
    RegCloseKey(hNSKey);
    return hr;
}

static HRESULT FindExeCplClass(LPCWSTR Canonical, LPWSTR clsid)
{
    HRESULT hr = E_FAIL;
    if (FAILED(hr))
        hr = FindExeCplClass(Canonical, HKEY_CURRENT_USER, FALSE, clsid);
    if (FAILED(hr))
        hr = FindExeCplClass(Canonical, HKEY_CURRENT_USER, TRUE, clsid);
    if (FAILED(hr))
        hr = FindExeCplClass(Canonical, HKEY_LOCAL_MACHINE, FALSE, clsid);
    if (FAILED(hr))
        hr = FindExeCplClass(Canonical, HKEY_LOCAL_MACHINE, TRUE, clsid);
    return hr;
}

HRESULT WINAPI COpenControlPanel::Open(LPCWSTR pszName, LPCWSTR pszPage, IUnknown *punkSite)
{
    WCHAR path[MAX_PATH], clspath[MAX_PATH];
    HRESULT hr = S_OK;
    SHELLEXECUTEINFOW sei = { sizeof(sei), SEE_MASK_FLAG_DDEWAIT };
    sei.lpFile = path;
    sei.nShow = SW_SHOW;
    if (!pszName)
    {
        GetSystemDirectoryW(path, _countof(path));
        PathAppendW(path, L"control.exe");
    }
    else
    {
        LPWSTR clsid = clspath + wsprintfW(clspath, L"CLSID\\");
        if (SUCCEEDED(hr = FindExeCplClass(pszName, clsid)))
        {
            if (SUCCEEDED(hr = CreateCplAbsoluteParsingPath(L"::", clsid, path, _countof(path))))
            {
                // NT6 will execute "::{26EE0668-A00A-44D7-9371-BEB064C98683}\0\::{clsid}[\pszPage]"
                // but we don't support parsing that so we force the class instead.
                sei.fMask |= SEE_MASK_CLASSNAME;
                sei.lpClass = clspath;
            }
        }
    }

    if (SUCCEEDED(hr))
    {
        DWORD error = ShellExecuteExW(&sei) ? ERROR_SUCCESS : GetLastError();
        hr = HRESULT_FROM_WIN32(error);
    }
    return hr;
}

HRESULT WINAPI COpenControlPanel::GetPath(LPCWSTR pszName, LPWSTR pszPath, UINT cchPath)
{
    HRESULT hr;
    if (!pszName)
    {
        PWSTR cpfolder;
        if (SUCCEEDED(hr = GetParsingName((PCIDLIST_ABSOLUTE)CSIDL_CONTROLS, &cpfolder)))
        {
            hr = StringCchCopyW(pszPath, cchPath, cpfolder);
            SHFree(cpfolder);
        }
    }
    else
    {
        WCHAR clsid[38 + 1];
        if (SUCCEEDED(hr = FindExeCplClass(pszName, clsid)))
        {
            hr = CreateCplAbsoluteParsingPath(L"::", clsid, pszPath, cchPath);
        }
    }
    return hr;
}

HRESULT WINAPI COpenControlPanel::GetCurrentView(CPVIEW *pView)
{
    *pView = CPVIEW_CLASSIC;
    return S_OK;
}
