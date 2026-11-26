/*
 * PROJECT:     ReactOS Shell
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Per-window property stores
 */

#include "precomp.h"
#include <propsys.h>
#include <commctrl.h>

#if DLL_EXPORT_VERSION >= 0x601

struct WINDOW_PROPERTY_STORE
{
    WINDOW_PROPERTY_STORE *Next;
    HWND Window;
    IPropertyStore *Store;
};

static SRWLOCK WindowStoreLock = SRWLOCK_INIT;
static WINDOW_PROPERTY_STORE *WindowStores;

static LRESULT CALLBACK
WindowPropertyStoreSubclass(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam,
                            UINT_PTR Id, DWORD_PTR Data)
{
    WINDOW_PROPERTY_STORE *Context = reinterpret_cast<WINDOW_PROPERTY_STORE *>(Data);
    if (Message == WM_NCDESTROY)
        RemoveWindowSubclass(Window, WindowPropertyStoreSubclass, Id);
    LRESULT Result = DefSubclassProc(Window, Message, WParam, LParam);
    if (Message == WM_NCDESTROY)
    {
        AcquireSRWLockExclusive(&WindowStoreLock);
        WINDOW_PROPERTY_STORE **Link = &WindowStores;
        while (*Link && *Link != Context) Link = &(*Link)->Next;
        if (*Link) *Link = Context->Next;
        ReleaseSRWLockExclusive(&WindowStoreLock);
        Context->Store->Release();
        HeapFree(GetProcessHeap(), 0, Context);
    }
    return Result;
}

EXTERN_C HRESULT WINAPI
SHGetPropertyStoreForWindow(HWND Window, REFIID Iid, void **Result)
{
    DWORD Process, Thread;
    WINDOW_PROPERTY_STORE *Context;
    HRESULT Hr;
    HMODULE Module;

    if (!Result) return E_POINTER;
    *Result = NULL;
    Thread = GetWindowThreadProcessId(Window, &Process);
    if (!Thread) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    if (Process != GetCurrentProcessId()) return E_ACCESSDENIED;

    AcquireSRWLockExclusive(&WindowStoreLock);
    for (Context = WindowStores; Context; Context = Context->Next)
        if (Context->Window == Window) break;
    if (!Context)
    {
        /* Subclass installation must run on the window's owning thread. */
        if (Thread != GetCurrentThreadId())
        {
            ReleaseSRWLockExclusive(&WindowStoreLock);
            return RPC_E_WRONG_THREAD;
        }
        Context = static_cast<WINDOW_PROPERTY_STORE *>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Context)));
        if (!Context)
        {
            ReleaseSRWLockExclusive(&WindowStoreLock);
            return E_OUTOFMEMORY;
        }
        Hr = PSCreateMemoryPropertyStore(IID_IPropertyStore,
                                         reinterpret_cast<void **>(&Context->Store));
        if (SUCCEEDED(Hr))
        {
            /* The subclass can outlive the initial call into shell32. */
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   reinterpret_cast<LPCWSTR>(WindowPropertyStoreSubclass), &Module) ||
                !SetWindowSubclass(Window, WindowPropertyStoreSubclass,
                                    reinterpret_cast<UINT_PTR>(WindowPropertyStoreSubclass),
                                    reinterpret_cast<DWORD_PTR>(Context)))
                Hr = E_FAIL;
        }
        if (FAILED(Hr))
        {
            if (Context->Store) Context->Store->Release();
            HeapFree(GetProcessHeap(), 0, Context);
            ReleaseSRWLockExclusive(&WindowStoreLock);
            return Hr;
        }
        Context->Window = Window;
        Context->Next = WindowStores;
        WindowStores = Context;
    }
    Hr = Context->Store->QueryInterface(Iid, Result);
    ReleaseSRWLockExclusive(&WindowStoreLock);
    return Hr;
}

#endif
