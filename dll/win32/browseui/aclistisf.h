/*
 *  Shell AutoComplete list
 *
 *  Copyright 2015  Thomas Faber
 *  Copyright 2020  Katayama Hirofumi MZ
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

#pragma once

#include <atlcoll.h>

struct CACListISFWorkerCtx;

class CACListISF :
    public CComCoClass<CACListISF, &CLSID_ACListISF>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IEnumString,
    public IACList2,
    public ICurrentWorkingDirectory,
    public IShellService,
    public IPersistFolder
{
public:
    enum LOCATION_TYPE
    {
        LT_DIRECTORY,
        LT_DESKTOP,
        LT_MYCOMPUTER,
        LT_FAVORITES,
        LT_MAX
    };

private:
    DWORD m_dwOptions;
    LOCATION_TYPE m_iNextLocation;
    BOOL m_fShowHidden;
    CStringW m_szRawPath;
    CStringW m_szExpanded;
    CComHeapPtr<ITEMIDLIST> m_pidlLocation;
    CComHeapPtr<ITEMIDLIST> m_pidlCurDir;
    CComPtr<IEnumIDList> m_pEnumIDList;
    CComPtr<IShellFolder> m_pShellFolder;
    CComPtr<IBrowserService> m_pBrowserService;

    // Background enumeration. SHParseDisplayName, BindToObject and
    // EnumObjects on remote shares (SMB, UNC) can block for many
    // seconds. We move the whole pipeline off the UI thread; the
    // worker pushes plain WCHAR strings into m_Results and Next()
    // pops from there. Only string copies cross apartments; shell
    // pointers stay inside the worker.
    //
    // Per-keystroke calls to Expand() detach the previous worker
    // rather than joining it (the prior worker may still be stuck
    // inside a synchronous SMB call). Each worker carries a
    // generation; only the matching generation may publish results.
    CRITICAL_SECTION m_csState;
    BOOL m_bCsInit;
    CAtlList<CStringW> m_Results;
    CACListISFWorkerCtx *m_pCurrentWorker;  // owned by the worker; weak here
    LONG m_lActiveGeneration;

    void DetachCurrentWorker();
    HRESULT StartWorker(BOOL bUseExpand);
    static DWORD WINAPI s_WorkerThreadProc(LPVOID pv);
    void WorkerRun(CACListISFWorkerCtx *pCtx);

public:
    CACListISF();
    ~CACListISF();

    HRESULT NextLocation();
    HRESULT SetLocation(LPITEMIDLIST pidl);
    HRESULT GetDisplayName(LPCITEMIDLIST pidlChild, CComHeapPtr<WCHAR>& pszChild);
    HRESULT GetPaths(LPCITEMIDLIST pidlChild, CComHeapPtr<WCHAR>& pszRaw,
                     CComHeapPtr<WCHAR>& pszExpanded);

    // *** IEnumString methods ***
    STDMETHOD(Next)(ULONG celt, LPOLESTR *rgelt, ULONG *pceltFetched) override;
    STDMETHOD(Skip)(ULONG celt) override;
    STDMETHOD(Reset)() override;
    STDMETHOD(Clone)(IEnumString **ppenum) override;

    // *** IACList methods ***
    STDMETHOD(Expand)(LPCOLESTR pszExpand) override;

    // *** IACList2 methods ***
    STDMETHOD(SetOptions)(DWORD dwFlag) override;
    STDMETHOD(GetOptions)(DWORD* pdwFlag) override;

    // *** IShellService methods ***
    STDMETHOD(SetOwner)(IUnknown *punkOwner) override;

    // *** IPersist methods ***
    STDMETHOD(GetClassID)(CLSID *pClassID) override;

    // *** IPersistFolder methods ***
    STDMETHOD(Initialize)(PCIDLIST_ABSOLUTE pidl) override;

    // *** ICurrentWorkingDirectory methods ***
    STDMETHOD(GetDirectory)(LPWSTR pwzPath, DWORD cchSize) override;
    STDMETHOD(SetDirectory)(LPCWSTR pwzPath) override;

public:
    DECLARE_REGISTRY_RESOURCEID(IDR_ACLISTISF)
    DECLARE_NOT_AGGREGATABLE(CACListISF)

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CACListISF)
        COM_INTERFACE_ENTRY_IID(IID_IEnumString, IEnumString)
        COM_INTERFACE_ENTRY_IID(IID_IACList, IACList)
        COM_INTERFACE_ENTRY_IID(IID_IACList2, IACList2)
        COM_INTERFACE_ENTRY_IID(IID_IShellService, IShellService)
        // Windows doesn't return this
        //COM_INTERFACE_ENTRY_IID(IID_IPersist, IPersist)
        COM_INTERFACE_ENTRY_IID(IID_IPersistFolder, IPersistFolder)
        COM_INTERFACE_ENTRY_IID(IID_ICurrentWorkingDirectory, ICurrentWorkingDirectory)
    END_COM_MAP()
};
