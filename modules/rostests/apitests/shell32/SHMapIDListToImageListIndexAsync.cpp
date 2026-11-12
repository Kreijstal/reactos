/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for SHMapIDListToImageListIndexAsync and the shell task scheduler
 */

#include "shelltest.h"
#include <shellutils.h>
#include <undocshell.h>
#include <strsafe.h>

typedef HRESULT (WINAPI *FN_SHMapIDListToImageListIndexAsync)(IShellTaskScheduler *, IShellFolder *, LPCITEMIDLIST,
                                                              UINT, PFNASYNCICONTASKBALLBACK, void *, void *,
                                                              int *, int *);

static const GUID TOID_Test = { 0x1d2e3f40, 0x5a6b, 0x4c7d, { 0x8e, 0x9f, 0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5 } };

/* A task that records its run, optionally waiting for a gate first */
class CTestTask : public IRunnableTask
{
    LONG m_Refs;
public:
    HANDLE m_Gate, m_Started, m_Done;
    LONG m_Runs, m_Kills;
    DWORD m_ThreadId;

    CTestTask(HANDLE Gate) : m_Refs(1), m_Gate(Gate), m_Runs(0), m_Kills(0), m_ThreadId(0)
    {
        m_Started = CreateEventW(NULL, TRUE, FALSE, NULL);
        m_Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    }
    virtual ~CTestTask()
    {
        CloseHandle(m_Started);
        CloseHandle(m_Done);
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IRunnableTask))
        {
            *ppv = static_cast<IRunnableTask *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_Refs); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG Refs = InterlockedDecrement(&m_Refs);
        if (!Refs)
            delete this;
        return Refs;
    }
    STDMETHODIMP Run() override
    {
        m_ThreadId = GetCurrentThreadId();
        InterlockedIncrement(&m_Runs);
        SetEvent(m_Started);
        if (m_Gate)
            WaitForSingleObject(m_Gate, 10000);
        SetEvent(m_Done);
        return S_OK;
    }
    STDMETHODIMP Kill(BOOL fWait) override { InterlockedIncrement(&m_Kills); return S_OK; }
    STDMETHODIMP Suspend() override { return E_NOTIMPL; }
    STDMETHODIMP Resume() override { return E_NOTIMPL; }
    STDMETHODIMP_(ULONG) IsRunning() override { return 0; }
};

static void TestScheduler(IShellTaskScheduler *pts)
{
    HANDLE Gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    CTestTask *First = new CTestTask(Gate), *Second = new CTestTask(NULL), *Third = new CTestTask(NULL);
    HRESULT hr;

    hr = pts->AddTask(First, TOID_Test, 1, ITSAT_DEFAULT_PRIORITY);
    ok_hr(hr, S_OK);
    ok(WaitForSingleObject(First->m_Started, 5000) == WAIT_OBJECT_0, "the task did not start\n");
    ok(First->m_ThreadId != GetCurrentThreadId(), "the task ran on the caller's thread\n");

    /* Queued behind the running one */
    ok_hr(pts->AddTask(Second, TOID_Test, 2, ITSAT_DEFAULT_PRIORITY), S_OK);
    ok_hr(pts->AddTask(Third, TOID_Test, 3, ITSAT_DEFAULT_PRIORITY), S_OK);
    ok_int(pts->CountTasks(TOID_Test), 3);
    ok_int(pts->CountTasks(TOID_NULL), 3);

    /* The one with lParam 2 is dropped before it runs */
    ok_hr(pts->RemoveTasks(TOID_Test, 2, FALSE), S_OK);
    ok_int(pts->CountTasks(TOID_Test), 2);

    SetEvent(Gate);
    ok(WaitForSingleObject(Third->m_Done, 5000) == WAIT_OBJECT_0, "the queued task did not run\n");
    ok_long(First->m_Runs, 1);
    ok_long(Second->m_Runs, 0);
    ok_long(Third->m_Runs, 1);
    ok_long(Third->m_ThreadId, First->m_ThreadId);

    First->Release();
    Second->Release();
    Third->Release();
    CloseHandle(Gate);
}

struct ICON_RESULT
{
    HANDLE Event;
    LPITEMIDLIST Pidl;
    PVOID Hint;
    INT Index, OpenIndex;
    DWORD ThreadId;
};

static void CALLBACK IconCallback(LPCITEMIDLIST pidl, LPVOID pvData, LPVOID pvHint, INT iIconIndex, INT iOpenIconIndex)
{
    ICON_RESULT *Result = (ICON_RESULT *)pvData;

    Result->Pidl = ILClone(pidl);
    Result->Hint = pvHint;
    Result->Index = iIconIndex;
    Result->OpenIndex = iOpenIconIndex;
    Result->ThreadId = GetCurrentThreadId();
    SetEvent(Result->Event);
}

static void TestAsyncIcon(FN_SHMapIDListToImageListIndexAsync pfnAsync, IShellTaskScheduler *pts, PCWSTR pszPath)
{
    CComHeapPtr<ITEMIDLIST> pidlFull;
    CComPtr<IShellFolder> psf;
    PCUITEMID_CHILD pidl;
    ICON_RESULT Result = { 0 };
    INT iIndex = -2, iSync;
    HRESULT hr;

    hr = SHParseDisplayName(pszPath, NULL, &pidlFull, 0, NULL);
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;
    hr = SHBindToParent(pidlFull, IID_PPV_ARG(IShellFolder, &psf), &pidl);
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;

    Result.Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    hr = pfnAsync(pts, psf, pidl, 0, IconCallback, &Result, (PVOID)0x1234, &iIndex, NULL);
    ok(hr == S_OK || hr == E_PENDING, "%S: hr = 0x%lx\n", pszPath, hr);
    if (hr == E_PENDING)
    {
        ok(WaitForSingleObject(Result.Event, 10000) == WAIT_OBJECT_0, "%S: no callback\n", pszPath);
        ok(Result.ThreadId != GetCurrentThreadId(), "%S: the callback came on the caller's thread\n", pszPath);
        ok(Result.Pidl && ILIsEqual(Result.Pidl, pidl), "%S: the callback has another pidl\n", pszPath);
        ok(Result.Hint == (PVOID)0x1234, "%S: hint %p\n", pszPath, Result.Hint);
        ok_int(Result.OpenIndex, -1);
        iIndex = Result.Index;
    }
    iSync = SHMapPIDLToSystemImageListIndex(psf, pidl, NULL);
    ok(iIndex != -1, "%S: no icon\n", pszPath);
    ok_int(iIndex, iSync);

    /* Again: the same icon, answered at once or by the task (an extractor
     * may not say where its icon is without reading the item) */
    iIndex = -2;
    ResetEvent(Result.Event);
    ILFree(Result.Pidl);
    Result.Pidl = NULL;
    Result.Index = -2;
    hr = pfnAsync(pts, psf, pidl, 0, IconCallback, &Result, NULL, &iIndex, NULL);
    ok(hr == S_OK || hr == E_PENDING, "%S: hr = 0x%lx\n", pszPath, hr);
    if (hr == E_PENDING)
    {
        ok(WaitForSingleObject(Result.Event, 10000) == WAIT_OBJECT_0, "%S: no callback\n", pszPath);
        iIndex = Result.Index;
    }
    ok_int(iIndex, iSync);

    /* Without a scheduler there is no task to wait for */
    iIndex = -2;
    hr = pfnAsync(NULL, psf, pidl, 0, NULL, NULL, NULL, &iIndex, NULL);
    ok(hr == S_OK || hr == E_FAIL, "%S: hr = 0x%lx\n", pszPath, hr);
    if (hr == S_OK)
        ok_int(iIndex, iSync);

    ILFree(Result.Pidl);
    CloseHandle(Result.Event);
}

START_TEST(SHMapIDListToImageListIndexAsync)
{
    FN_SHMapIDListToImageListIndexAsync pfnAsync;
    CComPtr<IShellTaskScheduler> pts;
    WCHAR szPath[MAX_PATH];
    HRESULT hr;

    CoInitialize(NULL);
    FileIconInit(TRUE);

    hr = CoCreateInstance(CLSID_ShellTaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellTaskScheduler, &pts));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
        TestScheduler(pts);

    pfnAsync = (FN_SHMapIDListToImageListIndexAsync)GetProcAddress(GetModuleHandleW(L"shell32.dll"), MAKEINTRESOURCEA(148));
    if (!pfnAsync || !pts)
    {
        skip("SHMapIDListToImageListIndexAsync or the scheduler is not available\n");
        CoUninitialize();
        return;
    }

    /* Executables with icons of their own, unlikely to be cached yet */
    GetWindowsDirectoryW(szPath, _countof(szPath));
    PathAppendW(szPath, L"regedit.exe");
    TestAsyncIcon(pfnAsync, pts, szPath);
    GetSystemDirectoryW(szPath, _countof(szPath));
    PathAppendW(szPath, L"calc.exe");
    TestAsyncIcon(pfnAsync, pts, szPath);

    pts.Release();
    CoUninitialize();
}
