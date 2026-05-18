/*
 * PROJECT:     ReactOS DirectWrite stub
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Minimal DWrite.dll surface so modern installers load.
 *
 * Modern Qt installers cache the returned IDWriteFactory pointer
 * unconditionally on a successful DWriteCreateFactory call and only
 * sanity-check it before its first vtable call, where they expect
 * QueryInterface to return a meaningful HRESULT.  When the factory
 * pointer is NULL the caller faults on the indirect vcall before any
 * graceful-fallback HRESULT check; when it's a valid object whose
 * QueryInterface returns a failure HRESULT the caller takes the
 * fallback path cleanly.
 *
 * Provide a vtable of E_NOTIMPL-returning thunks of sufficient size
 * to cover every IDWriteFactory* virtual method, so callers that
 * invoke any vtable slot get a well-defined failure rather than a
 * vtable-overrun crash.
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

typedef struct dwrite_factory_stub {
    const void *lpVtbl;
    LONG ref;
} dwrite_factory_stub_t;

static HRESULT STDMETHODCALLTYPE stub_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE stub_AddRef(void *iface)
{
    return InterlockedIncrement(&((dwrite_factory_stub_t *)iface)->ref);
}

static ULONG STDMETHODCALLTYPE stub_Release(void *iface)
{
    LONG r = InterlockedDecrement(&((dwrite_factory_stub_t *)iface)->ref);
    if (r == 0) HeapFree(GetProcessHeap(), 0, iface);
    return r;
}

static HRESULT STDMETHODCALLTYPE stub_method_e_notimpl(void *iface, ...)
{
    return E_NOTIMPL;
}

/* Pad to cover IDWriteFactory through IDWriteFactory4.  IDWriteFactory4
 * adds methods up through ordinal ~31; 64 entries is comfortable. */
static const void * const dwrite_factory_vtbl[64] = {
    stub_QueryInterface,                  /* 0  IUnknown::QueryInterface */
    stub_AddRef,                          /* 1  IUnknown::AddRef         */
    stub_Release,                         /* 2  IUnknown::Release        */
    stub_method_e_notimpl,                /* 3  GetSystemFontCollection  */
    stub_method_e_notimpl,                /* 4  CreateCustomFontCollection */
    stub_method_e_notimpl,                /* 5  RegisterFontCollectionLoader */
    stub_method_e_notimpl,                /* 6  UnregisterFontCollectionLoader */
    stub_method_e_notimpl,                /* 7  CreateFontFileReference */
    stub_method_e_notimpl,                /* 8  CreateCustomFontFileReference */
    stub_method_e_notimpl,                /* 9  CreateFontFace */
    stub_method_e_notimpl,                /* 10 CreateRenderingParams */
    stub_method_e_notimpl,                /* 11 CreateMonitorRenderingParams */
    stub_method_e_notimpl,                /* 12 CreateCustomRenderingParams */
    stub_method_e_notimpl,                /* 13 RegisterFontFileLoader */
    stub_method_e_notimpl,                /* 14 UnregisterFontFileLoader */
    stub_method_e_notimpl,                /* 15 CreateTextFormat */
    stub_method_e_notimpl,                /* 16 CreateTypography */
    stub_method_e_notimpl,                /* 17 GetGdiInterop */
    stub_method_e_notimpl,                /* 18 CreateTextLayout */
    stub_method_e_notimpl,                /* 19 CreateGdiCompatibleTextLayout */
    stub_method_e_notimpl,                /* 20 CreateEllipsisTrimmingSign */
    stub_method_e_notimpl,                /* 21 CreateTextAnalyzer */
    stub_method_e_notimpl,                /* 22 CreateNumberSubstitution */
    stub_method_e_notimpl,                /* 23 CreateGlyphRunAnalysis */
    /* IDWriteFactory1+ extensions */
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl, stub_method_e_notimpl, stub_method_e_notimpl,
    stub_method_e_notimpl,
};

HRESULT WINAPI DWriteCreateFactory(DWORD factoryType, REFIID iid, void **factory)
{
    dwrite_factory_stub_t *f;
    FIXME("(%u,%s,%p): returning E_NOTIMPL stub factory\n",
          factoryType, wine_dbgstr_guid(iid), factory);

    if (!factory) return E_POINTER;

    f = HeapAlloc(GetProcessHeap(), 0, sizeof(*f));
    if (!f) { *factory = NULL; return E_OUTOFMEMORY; }
    f->lpVtbl = dwrite_factory_vtbl;
    f->ref = 1;
    *factory = f;
    return S_OK;
}
