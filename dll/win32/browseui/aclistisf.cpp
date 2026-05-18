/*
 *  Shell AutoComplete list
 *
 *  Copyright 2015  Thomas Faber
 *  Copyright 2020-2021 Katayama Hirofumi MZ
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

#include "precomp.h"

//
// Per-worker context. Each enumeration run owns its own copy of the
// inputs and its own shell pointers, so detaching a stuck worker
// (e.g. one blocked inside a multi-second SMB roundtrip) is safe:
// the next worker shares no mutable state with it.
//
// The worker holds a reference to its owner (AddRef in StartWorker)
// so the owner cannot be destroyed while a worker is still running.
//
struct CACListISFWorkerCtx
{
    CACListISF *m_pOwner;
    LONG m_lGeneration;
    LONG m_lCancel;
    HANDLE m_hThread;

    // Snapshot of the relevant CACListISF members at the time the
    // worker was started. The worker reads only these, never the
    // owner's members.
    DWORD m_dwOptions;
    BOOL m_fShowHidden;
    BOOL m_bUseExpand;
    CStringW m_szRawPath;
    CStringW m_szExpanded;
    CComHeapPtr<ITEMIDLIST> m_pidlCurDir;

    // Per-worker shell state (never aliased into the owner).
    CACListISF::LOCATION_TYPE m_iNextLocation;
    CACListISF::LOCATION_TYPE m_iLastLocation;
    CComPtr<IShellFolder> m_pShellFolder;
    CComPtr<IEnumIDList> m_pEnumIDList;

    CACListISFWorkerCtx()
        : m_pOwner(NULL)
        , m_lGeneration(0)
        , m_lCancel(0)
        , m_hThread(NULL)
        , m_dwOptions(0)
        , m_fShowHidden(FALSE)
        , m_bUseExpand(FALSE)
        , m_iNextLocation(CACListISF::LT_DIRECTORY)
        , m_iLastLocation(CACListISF::LT_MAX)
    {
    }

    ~CACListISFWorkerCtx()
    {
        if (m_hThread)
            CloseHandle(m_hThread);
    }

    BOOL IsCancelled() const
    {
        return InterlockedCompareExchange(const_cast<LONG *>(&m_lCancel), 0, 0) != 0;
    }
};

CACListISF::CACListISF()
    : m_dwOptions(ACLO_CURRENTDIR | ACLO_MYCOMPUTER)
    , m_iNextLocation(LT_DIRECTORY)
    , m_fShowHidden(FALSE)
    , m_bCsInit(FALSE)
    , m_pCurrentWorker(NULL)
    , m_lActiveGeneration(0)
{
    InitializeCriticalSection(&m_csState);
    m_bCsInit = TRUE;
}

CACListISF::~CACListISF()
{
    // The worker holds a ref on us via AddRef() in StartWorker(). If
    // one were still alive, our refcount could not have reached zero
    // and this destructor would not run. The invariant therefore is:
    // no live worker.
    ATLASSERT(m_pCurrentWorker == NULL);
    if (m_bCsInit)
        DeleteCriticalSection(&m_csState);
}

// Signal the current worker (if any) to stop, then forget it. The
// worker thread frees its own context once it observes the cancel
// flag and releases its owner ref at that point.
void CACListISF::DetachCurrentWorker()
{
    // Bump generation and cancel under the CS. The worker's exit
    // path also takes the CS before deleting its context, so as long
    // as we touch m_lCancel here we are guaranteed the context is
    // still alive: the worker cannot have run its "delete pCtx" yet.
    //
    // Do NOT WaitForSingleObject here. The worker may be stuck
    // inside a synchronous SMB / shell call that the cancel flag
    // cannot interrupt; blocking the UI thread on that wait is
    // exactly the bug this change is fixing.
    EnterCriticalSection(&m_csState);
    InterlockedIncrement(&m_lActiveGeneration);
    if (m_pCurrentWorker)
        InterlockedExchange(&m_pCurrentWorker->m_lCancel, 1);
    m_pCurrentWorker = NULL;
    m_Results.RemoveAll();
    LeaveCriticalSection(&m_csState);
}

HRESULT CACListISF::NextLocation()
{
    // Retained for source compatibility with the published class
    // signature. The production path uses an inline worker-local
    // switch (see WorkerNextLocation in this file). This entry
    // point is no longer driven by Next().
    return S_FALSE;
}

HRESULT CACListISF::SetLocation(LPITEMIDLIST pidl)
{
    // See NextLocation() comment - unused on the production path.
    if (pidl)
        ILFree(pidl);
    return E_NOTIMPL;
}

HRESULT CACListISF::GetDisplayName(LPCITEMIDLIST, CComHeapPtr<WCHAR>&)
{
    return E_NOTIMPL;
}

HRESULT
CACListISF::GetPaths(LPCITEMIDLIST, CComHeapPtr<WCHAR>&, CComHeapPtr<WCHAR>&)
{
    return E_NOTIMPL;
}

//
// Worker-side helpers. These mirror the previous synchronous
// NextLocation / SetLocation / GetPaths / GetDisplayName logic but
// operate exclusively on the worker context.
//
static HRESULT WorkerSetLocation(CACListISFWorkerCtx *pCtx, LPITEMIDLIST pidl)
{
    pCtx->m_pEnumIDList.Release();
    pCtx->m_pShellFolder.Release();

    if (!pidl)
        return E_FAIL;

    CComHeapPtr<ITEMIDLIST> pidlOwned;
    pidlOwned.Attach(pidl);

    CComPtr<IShellFolder> pFolder;
    HRESULT hr = SHGetDesktopFolder(&pFolder);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    if (!ILIsEmpty(pidlOwned))
    {
        hr = pFolder->BindToObject(pidlOwned, NULL,
                                   IID_PPV_ARG(IShellFolder, &pCtx->m_pShellFolder));
        if (FAILED_UNEXPECTEDLY(hr))
            return hr;
    }
    else
    {
        pCtx->m_pShellFolder.Attach(pFolder.Detach());
    }

    if (!pCtx->m_pShellFolder)
        return E_FAIL;

    SHCONTF Flags = SHCONTF_FOLDERS | SHCONTF_INIT_ON_FIRST_NEXT;
    if (pCtx->m_fShowHidden)
        Flags |= SHCONTF_INCLUDEHIDDEN;
    if (!(pCtx->m_dwOptions & ACLO_FILESYSDIRS))
        Flags |= SHCONTF_NONFOLDERS;

    hr = pCtx->m_pShellFolder->EnumObjects(NULL, Flags, &pCtx->m_pEnumIDList);
    if (hr != S_OK)
    {
        ERR("EnumObjects failed: 0x%lX\n", hr);
        pCtx->m_pEnumIDList.Release();
        pCtx->m_pShellFolder.Release();
        return E_FAIL;
    }
    if (!pCtx->m_pEnumIDList)
    {
        pCtx->m_pShellFolder.Release();
        return E_FAIL;
    }
    return S_OK;
}

static HRESULT WorkerNextLocation(CACListISFWorkerCtx *pCtx)
{
    HRESULT hr;
    for (;;)
    {
        CACListISF::LOCATION_TYPE current = pCtx->m_iNextLocation;
        switch (current)
        {
            case CACListISF::LT_DIRECTORY:
                pCtx->m_iNextLocation = CACListISF::LT_DESKTOP;
                if (pCtx->m_pidlCurDir && !ILIsEmpty(pCtx->m_pidlCurDir) &&
                    (pCtx->m_dwOptions & ACLO_CURRENTDIR))
                {
                    CComHeapPtr<ITEMIDLIST> pidl(ILClone(pCtx->m_pidlCurDir));
                    hr = WorkerSetLocation(pCtx, pidl.Detach());
                    if (SUCCEEDED(hr))
                    {
                        pCtx->m_iLastLocation = CACListISF::LT_DIRECTORY;
                        return hr;
                    }
                }
                break;
            case CACListISF::LT_DESKTOP:
                pCtx->m_iNextLocation = CACListISF::LT_MYCOMPUTER;
                if (pCtx->m_dwOptions & ACLO_DESKTOP)
                {
                    CComHeapPtr<ITEMIDLIST> pidl;
                    hr = SHGetSpecialFolderLocation(NULL, CSIDL_DESKTOP, &pidl);
                    if (FAILED_UNEXPECTEDLY(hr))
                        return S_FALSE;
                    hr = WorkerSetLocation(pCtx, pidl.Detach());
                    if (SUCCEEDED(hr))
                    {
                        pCtx->m_iLastLocation = CACListISF::LT_DESKTOP;
                        return hr;
                    }
                }
                break;
            case CACListISF::LT_MYCOMPUTER:
                pCtx->m_iNextLocation = CACListISF::LT_FAVORITES;
                if (pCtx->m_dwOptions & ACLO_MYCOMPUTER)
                {
                    CComHeapPtr<ITEMIDLIST> pidl;
                    hr = SHGetSpecialFolderLocation(NULL, CSIDL_DRIVES, &pidl);
                    if (FAILED_UNEXPECTEDLY(hr))
                        return S_FALSE;
                    hr = WorkerSetLocation(pCtx, pidl.Detach());
                    if (SUCCEEDED(hr))
                    {
                        pCtx->m_iLastLocation = CACListISF::LT_MYCOMPUTER;
                        return hr;
                    }
                }
                break;
            case CACListISF::LT_FAVORITES:
                pCtx->m_iNextLocation = CACListISF::LT_MAX;
                if (pCtx->m_dwOptions & ACLO_FAVORITES)
                {
                    CComHeapPtr<ITEMIDLIST> pidl;
                    hr = SHGetSpecialFolderLocation(NULL, CSIDL_FAVORITES, &pidl);
                    if (FAILED_UNEXPECTEDLY(hr))
                        return S_FALSE;
                    hr = WorkerSetLocation(pCtx, pidl.Detach());
                    if (SUCCEEDED(hr))
                    {
                        pCtx->m_iLastLocation = CACListISF::LT_FAVORITES;
                        return hr;
                    }
                }
                break;
            case CACListISF::LT_MAX:
            default:
                return S_FALSE;
        }
        if (pCtx->IsCancelled())
            return S_FALSE;
    }
}

static HRESULT WorkerGetDisplayName(CACListISFWorkerCtx *pCtx, LPCITEMIDLIST pidlChild,
                                    CComHeapPtr<WCHAR>& pszChild)
{
    pszChild.Free();

    STRRET StrRet;
    DWORD dwFlags = SHGDN_INFOLDER | SHGDN_FORPARSING | SHGDN_FORADDRESSBAR;
    HRESULT hr = pCtx->m_pShellFolder->GetDisplayNameOf(pidlChild, dwFlags, &StrRet);
    if (FAILED(hr))
    {
        dwFlags = SHGDN_INFOLDER | SHGDN_FORPARSING;
        hr = pCtx->m_pShellFolder->GetDisplayNameOf(pidlChild, dwFlags, &StrRet);
        if (FAILED_UNEXPECTEDLY(hr))
            return hr;
    }

    return StrRetToStrW(&StrRet, NULL, &pszChild);
}

static HRESULT WorkerGetPath(CACListISFWorkerCtx *pCtx, LPCITEMIDLIST pidlChild,
                             CStringW& strRaw)
{
    CComHeapPtr<WCHAR> pszChild;
    HRESULT hr = WorkerGetDisplayName(pCtx, pidlChild, pszChild);
    if (FAILED(hr) || !pszChild)
        return FAILED(hr) ? hr : E_FAIL;

    strRaw.Empty();
    if (pCtx->m_szRawPath.GetLength() &&
        pCtx->m_iLastLocation == CACListISF::LT_DIRECTORY)
    {
        INT cchExpand = pCtx->m_szRawPath.GetLength();
        if (StrCmpNIW(pszChild, pCtx->m_szRawPath, cchExpand) != 0 ||
            pszChild[0] != L'\\' || pszChild[1] != L'\\')
        {
            strRaw = pCtx->m_szRawPath;
        }
    }
    strRaw += static_cast<LPCWSTR>(pszChild);
    return S_OK;
}

void CACListISF::WorkerRun(CACListISFWorkerCtx *pCtx)
{
    HRESULT hr;

    if (pCtx->m_bUseExpand)
    {
        // Resolve the user-typed path here. SHParseDisplayName and
        // BindToObject can each block on SMB; both stay off-thread.
        CComHeapPtr<ITEMIDLIST> pidl;
        hr = SHParseDisplayName(pCtx->m_szExpanded, NULL, &pidl, NULL, NULL);
        if (FAILED(hr) || pCtx->IsCancelled())
            return;
        hr = WorkerSetLocation(pCtx, pidl.Detach());
        if (FAILED(hr))
            return;
        pCtx->m_iLastLocation = CACListISF::LT_DIRECTORY;
    }
    else
    {
        if (WorkerNextLocation(pCtx) != S_OK)
            return;
    }

    CComHeapPtr<ITEMIDLIST> pidlChild;
    for (;;)
    {
        if (pCtx->IsCancelled())
            return;

        if (!pCtx->m_pEnumIDList || !pCtx->m_pShellFolder)
        {
            if (WorkerNextLocation(pCtx) != S_OK)
                return;
            continue;
        }

        pidlChild.Free();
        hr = pCtx->m_pEnumIDList->Next(1, &pidlChild, NULL);
        if (hr != S_OK)
        {
            // Drained this location; advance.
            if (WorkerNextLocation(pCtx) != S_OK)
                return;
            continue;
        }

        CStringW strRaw;
        if (FAILED(WorkerGetPath(pCtx, pidlChild, strRaw)))
            continue;
        if (strRaw.IsEmpty())
            continue;

        DWORD attrs = SFGAO_FOLDER | SFGAO_FILESYSTEM;
        LPCITEMIDLIST pidlRef = pidlChild;
        if (FAILED(pCtx->m_pShellFolder->GetAttributesOf(1, &pidlRef, &attrs)))
            continue;

        if ((pCtx->m_dwOptions & ACLO_FILESYSDIRS) && !(attrs & SFGAO_FOLDER))
            continue;
        if ((pCtx->m_dwOptions & (ACLO_FILESYSONLY | ACLO_FILESYSDIRS)) &&
            !(attrs & SFGAO_FILESYSTEM))
        {
            continue;
        }

        // Publish iff we are still the active generation. Inside the
        // CS so a concurrent DetachCurrentWorker() either sees us
        // before we publish (and bumps generation, so we drop the
        // result) or after we publish (and clears m_Results
        // unconditionally).
        EnterCriticalSection(&m_csState);
        if (pCtx->m_lGeneration ==
            InterlockedCompareExchange(&m_lActiveGeneration, 0, 0))
        {
            m_Results.AddTail(strRaw);
        }
        LeaveCriticalSection(&m_csState);
    }
}

DWORD WINAPI CACListISF::s_WorkerThreadProc(LPVOID pv)
{
    CACListISFWorkerCtx *pCtx = static_cast<CACListISFWorkerCtx *>(pv);
    CACListISF *pOwner = pCtx->m_pOwner;

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    pOwner->WorkerRun(pCtx);
    if (SUCCEEDED(hrCo))
        CoUninitialize();

    // Clear the registered current-worker pointer if it still
    // refers to us; otherwise we were already detached.
    EnterCriticalSection(&pOwner->m_csState);
    if (pOwner->m_pCurrentWorker == pCtx)
        pOwner->m_pCurrentWorker = NULL;
    LeaveCriticalSection(&pOwner->m_csState);

    delete pCtx;
    pOwner->Release(); // matches AddRef() in StartWorker
    return 0;
}

HRESULT CACListISF::StartWorker(BOOL bUseExpand)
{
    // Caller must have already detached any prior worker.
    ATLASSERT(m_pCurrentWorker == NULL);

    CACListISFWorkerCtx *pCtx = new CACListISFWorkerCtx();
    if (!pCtx)
        return E_OUTOFMEMORY;

    pCtx->m_pOwner = this;
    pCtx->m_lGeneration = InterlockedIncrement(&m_lActiveGeneration);
    pCtx->m_dwOptions = m_dwOptions;
    pCtx->m_fShowHidden = m_fShowHidden;
    pCtx->m_bUseExpand = bUseExpand;
    pCtx->m_szRawPath = m_szRawPath;
    pCtx->m_szExpanded = m_szExpanded;
    pCtx->m_iNextLocation = m_iNextLocation;
    if (m_pidlCurDir)
        pCtx->m_pidlCurDir.Attach(ILClone(m_pidlCurDir));

    AddRef();
    pCtx->m_hThread = CreateThread(NULL, 0, s_WorkerThreadProc, pCtx, 0, NULL);
    if (!pCtx->m_hThread)
    {
        Release();
        delete pCtx;
        return E_FAIL;
    }

    EnterCriticalSection(&m_csState);
    m_pCurrentWorker = pCtx;
    LeaveCriticalSection(&m_csState);
    return S_OK;
}

// *** IEnumString methods ***
STDMETHODIMP CACListISF::Next(ULONG celt, LPOLESTR *rgelt, ULONG *pceltFetched)
{
    TRACE("(%p, %d, %p, %p)\n", this, celt, rgelt, pceltFetched);

    if (celt == 0)
        return S_OK;
    if (!rgelt)
        return S_FALSE;

    *rgelt = NULL;
    if (pceltFetched)
        *pceltFetched = 0;

    CStringW str;
    BOOL bHaveResult = FALSE;
    EnterCriticalSection(&m_csState);
    if (!m_Results.IsEmpty())
    {
        str = m_Results.RemoveHead();
        bHaveResult = TRUE;
    }
    LeaveCriticalSection(&m_csState);

    if (!bHaveResult)
    {
        // Worker has not produced anything yet (or has finished).
        // IEnumString::Next is allowed to return S_FALSE without
        // having exhausted the source; the autocomplete client will
        // re-poll on the next keystroke.
        return S_FALSE;
    }

    HRESULT hr = SHStrDupW(static_cast<LPCWSTR>(str), rgelt);
    if (FAILED(hr) || !*rgelt)
        return S_FALSE;

    if (pceltFetched)
        *pceltFetched = 1;

    TRACE("*rgelt: %S\n", *rgelt);
    return S_OK;
}

STDMETHODIMP CACListISF::Reset()
{
    TRACE("(%p)\n", this);

    DetachCurrentWorker();

    m_iNextLocation = LT_DIRECTORY;
    m_szRawPath = L"";
    m_szExpanded = L"";

    SHELLSTATE ss = { 0 };
    SHGetSetSettings(&ss, SSF_SHOWALLOBJECTS, FALSE);
    m_fShowHidden = ss.fShowAllObjects;

    if (m_dwOptions & ACLO_CURRENTDIR)
    {
        if (m_pBrowserService)
        {
            CComHeapPtr<ITEMIDLIST> pidl;
            m_pBrowserService->GetPidl(&pidl);
            if (pidl)
                Initialize(pidl);
        }
    }

    StartWorker(FALSE);
    return S_OK;
}

STDMETHODIMP CACListISF::Skip(ULONG celt)
{
    TRACE("(%p, %d)\n", this, celt);
    return E_NOTIMPL;
}

STDMETHODIMP CACListISF::Clone(IEnumString **ppOut)
{
    TRACE("(%p, %p)\n", this, ppOut);
    *ppOut = NULL;
    return E_NOTIMPL;
}

// *** IACList methods ***
STDMETHODIMP CACListISF::Expand(LPCOLESTR pszExpand)
{
    TRACE("(%p, %ls)\n", this, pszExpand);

    // Detach (do not join) the prior keystroke's worker. It may be
    // stuck inside a synchronous SMB call we cannot interrupt;
    // joining would reintroduce the UI hang.
    DetachCurrentWorker();

    m_szRawPath = pszExpand;
    m_iNextLocation = LT_DIRECTORY;

    // skip left space
    while (*pszExpand == L' ')
        ++pszExpand;

    // expand environment variables (%WINDIR% etc.)
    WCHAR szExpanded[MAX_PATH], szPath1[MAX_PATH], szPath2[MAX_PATH];
    ExpandEnvironmentStringsW(pszExpand, szExpanded, _countof(szExpanded));
    pszExpand = szExpanded;

    // Resolve to a full path. PathFileExistsW on a UNC root can
    // still hit the network, but that was already part of the
    // address bar's typed-path validation prior to this change;
    // the worker takes the brunt of latency in SHParseDisplayName
    // + EnumObjects.
    if (szExpanded[0] && szExpanded[1] == L':' && szExpanded[2] == 0)
    {
        // 'C:' --> 'C:\'
        szExpanded[2] = L'\\';
        szExpanded[3] = 0;
    }
    else
    {
        if (PathIsRelativeW(pszExpand) &&
            SHGetPathFromIDListW(m_pidlCurDir, szPath1) &&
            PathCombineW(szPath2, szPath1, pszExpand) &&
            PathFileExistsW(szPath2))
        {
            pszExpand = szPath2;
        }
        else if (PathFileExistsW(pszExpand))
        {
            GetFullPathNameW(pszExpand, _countof(szPath1), szPath1, NULL);
            pszExpand = szPath1;
        }
    }

    m_szExpanded = pszExpand;
    StartWorker(TRUE);
    return S_OK;
}

// *** IACList2 methods ***
STDMETHODIMP CACListISF::SetOptions(DWORD dwFlag)
{
    TRACE("(%p, %lu)\n", this, dwFlag);
    m_dwOptions = dwFlag;
    return S_OK;
}

STDMETHODIMP CACListISF::GetOptions(DWORD* pdwFlag)
{
    TRACE("(%p, %p)\n", this, pdwFlag);
    if (pdwFlag)
    {
        *pdwFlag = m_dwOptions;
        return S_OK;
    }
    return E_INVALIDARG;
}

// *** IShellService methods ***
STDMETHODIMP CACListISF::SetOwner(IUnknown *punkOwner)
{
    TRACE("(%p, %p)\n", this, punkOwner);
    m_pBrowserService.Release();
    punkOwner->QueryInterface(IID_PPV_ARG(IBrowserService, &m_pBrowserService));
    return S_OK;
}

// *** IPersist methods ***
STDMETHODIMP CACListISF::GetClassID(CLSID *pClassID)
{
    TRACE("(%p, %p)\n", this, pClassID);
    if (pClassID == NULL)
        return E_POINTER;
    *pClassID = CLSID_ACListISF;
    return S_OK;
}

// *** IPersistFolder methods ***
STDMETHODIMP CACListISF::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    TRACE("(%p, %p)\n", this, pidl);
    m_pidlCurDir.Free();
    if (!pidl)
        return S_OK;

    LPITEMIDLIST pidlClone = ILClone(pidl);
    if (!pidlClone)
    {
        ERR("Out of memory\n");
        return E_OUTOFMEMORY;
    }
    m_pidlCurDir.Attach(pidlClone);
    return S_OK;
}

// *** ICurrentWorkingDirectory methods ***
STDMETHODIMP CACListISF::GetDirectory(LPWSTR pwzPath, DWORD cchSize)
{
    TRACE("(%p, %p, %ld)\n", this, pwzPath, cchSize);
    return E_NOTIMPL;
}

STDMETHODIMP CACListISF::SetDirectory(LPCWSTR pwzPath)
{
    TRACE("(%p, %ls, %ld)\n", this, pwzPath);
    LPITEMIDLIST pidl = ILCreateFromPathW(pwzPath);
    if (!pidl)
    {
        ERR("Out of memory\n");
        return E_OUTOFMEMORY;
    }
    m_pidlCurDir.Attach(pidl);
    return S_OK;
}
