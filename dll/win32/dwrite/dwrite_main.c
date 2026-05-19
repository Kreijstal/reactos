/*
 * PROJECT:     ReactOS DirectWrite stub
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Minimal DWrite.dll surface so modern installers load.
 *
 * The Qt 6 installer (msys2 setup, etc.) cascades DWriteCreateFactory
 * calls from IDWriteFactory6 down to plain IDWriteFactory and uses the
 * first non-NULL result as its IDWriteFactory*.  It then calls
 *   factory->GetGdiInterop(&interop)
 *   factory->QueryInterface(IDWriteFactory6, ...)
 *   factory6->GetSystemFontCollection(false, FAMILY_MODEL_TYPOGRAPHIC, &col)
 * and bails out (returning early from QWindowsDirectWriteFontDatabase::
 * populateFontDatabase) if any of the above fail.
 *
 * To let Qt's bitmap-font fallback path (EnumFontFamiliesEx on the same
 * function, populating raster fonts via GDI) run, we must succeed
 * GetGdiInterop and QI(IDWriteFactory6) but allow GetSystemFontCollection
 * to fail.  Failing GetSystemFontCollection lets Qt skip the DirectWrite
 * family loop and proceed to the bitmap fallback at the bottom of
 * populateFontDatabase.
 *
 * GDI interop's CreateFontFaceFromHdc is exercised by QWindowsFontDatabase::
 * createEngine when an HFONT exists; returning E_NOTIMPL there leaves the
 * GDI engine path intact (Qt's createEngine constructs QWindowsFontEngine
 * when DirectWrite fails).
 *
 * Everything past that surface returns E_NOTIMPL.
 */

#include <stdarg.h>

#include "dwrite_private.h"

#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

/* DirectWrite interface IIDs we accept on QueryInterface. */
const GUID g_iid_IUnknown =
    {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID g_iid_IDWriteFactory =
    {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
static const GUID g_iid_IDWriteFactory1 =
    {0x30572f99, 0xdac6, 0x41db, {0xa1, 0x6e, 0x04, 0x86, 0x30, 0x7e, 0x60, 0x6a}};
static const GUID g_iid_IDWriteFactory2 =
    {0x0439fc60, 0xca44, 0x4994, {0x8d, 0xee, 0x3a, 0x9a, 0xf7, 0xb7, 0x32, 0xec}};
static const GUID g_iid_IDWriteFactory3 =
    {0x9a1b41c3, 0xd3bb, 0x466a, {0x87, 0xfc, 0xfe, 0x67, 0x55, 0x6a, 0x3b, 0x65}};
static const GUID g_iid_IDWriteFactory4 =
    {0x4b0b5bd3, 0x0797, 0x4549, {0x8a, 0xc5, 0xfe, 0x91, 0x5c, 0xc5, 0x38, 0x56}};
static const GUID g_iid_IDWriteFactory5 =
    {0x958db99a, 0xbe2a, 0x4f09, {0xaf, 0x7d, 0x65, 0x18, 0x98, 0x03, 0xd1, 0xd3}};
static const GUID g_iid_IDWriteFactory6 =
    {0xf3744d80, 0x21f7, 0x42eb, {0xb3, 0x5d, 0x99, 0x5b, 0xc7, 0x2f, 0xc2, 0x23}};
static const GUID g_iid_IDWriteFactory7 =
    {0x35d0e0b3, 0x9076, 0x4d2e, {0xa0, 0x16, 0xa9, 0x1b, 0x56, 0x8a, 0x06, 0xb4}};
static const GUID g_iid_IDWriteGdiInterop =
    {0x1edd9491, 0x9853, 0x4299, {0x89, 0x8f, 0x64, 0x32, 0x98, 0x3b, 0x6f, 0x3a}};
static const GUID g_iid_IDWriteGdiInterop1 =
    {0x4556be70, 0x3abd, 0x4f70, {0x90, 0xbe, 0x42, 0x17, 0x80, 0xa6, 0xf5, 0x15}};

int dwrite_iid_equals(REFIID a, const GUID *b)
{
    return a && b &&
           a->Data1 == b->Data1 && a->Data2 == b->Data2 && a->Data3 == b->Data3 &&
           a->Data4[0] == b->Data4[0] && a->Data4[1] == b->Data4[1] &&
           a->Data4[2] == b->Data4[2] && a->Data4[3] == b->Data4[3] &&
           a->Data4[4] == b->Data4[4] && a->Data4[5] == b->Data4[5] &&
           a->Data4[6] == b->Data4[6] && a->Data4[7] == b->Data4[7];
}

/* Back-compat aliases for the original static-named helpers used below. */
#define iid_equals(a, b) dwrite_iid_equals((a), (b))

/* --- Common AddRef / Release --- */
ULONG STDMETHODCALLTYPE dwrite_common_AddRef(void *iface)
{
    return InterlockedIncrement(&((dwrite_obj_header_t *)iface)->ref);
}

ULONG STDMETHODCALLTYPE dwrite_common_Release(void *iface)
{
    LONG r = InterlockedDecrement(&((dwrite_obj_header_t *)iface)->ref);
    /* Singleton-style objects (factory/collection/family/font) floor at 0
     * and are not freed.  Heap-allocated faces have their own Release. */
    if (r < 0) ((dwrite_obj_header_t *)iface)->ref = 0;
    return r < 0 ? 0 : (ULONG)r;
}

HRESULT STDMETHODCALLTYPE dwrite_common_method_e_notimpl(void *iface, ...)
{
    (void)iface;
    return E_NOTIMPL;
}

/* Aliases used by the rest of this translation unit (the factory/
 * gdi-interop vtables) — keep the historical short names. */
#define common_AddRef           dwrite_common_AddRef
#define common_Release          dwrite_common_Release
#define common_method_e_notimpl dwrite_common_method_e_notimpl

/* --- IDWriteGdiInterop / IDWriteGdiInterop1 stub --- */
static const void * const dwrite_gdi_interop_vtbl[24];

typedef struct dwrite_gdi_interop {
    const void *lpVtbl;
    LONG ref;
} dwrite_gdi_interop_t;

static dwrite_gdi_interop_t g_gdi_interop = { dwrite_gdi_interop_vtbl, 1 };

static HRESULT STDMETHODCALLTYPE gdi_interop_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (iid_equals(iid, &g_iid_IUnknown) ||
        iid_equals(iid, &g_iid_IDWriteGdiInterop) ||
        iid_equals(iid, &g_iid_IDWriteGdiInterop1)) {
        *out = iface;
        common_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static const void * const dwrite_gdi_interop_vtbl[24] = {
    gdi_interop_QueryInterface,           /* 0  QueryInterface */
    common_AddRef,                        /* 1  AddRef */
    common_Release,                       /* 2  Release */
    common_method_e_notimpl,              /* 3  CreateFontFromLOGFONT */
    common_method_e_notimpl,              /* 4  ConvertFontToLOGFONT */
    common_method_e_notimpl,              /* 5  ConvertFontFaceToLOGFONT */
    common_method_e_notimpl,              /* 6  CreateFontFaceFromHdc */
    common_method_e_notimpl,              /* 7  CreateBitmapRenderTarget */
    /* GdiInterop1 extensions */
    common_method_e_notimpl,               /* 8 CreateFontFromLOGFONT (variant) */
    common_method_e_notimpl,               /* 9 GetFontSignature */
    common_method_e_notimpl,               /*10 GetFontSignature */
    common_method_e_notimpl,               /*11 GetMatchingFontsByLOGFONT */
    common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl,
};

/* --- IDWriteFactory[0-7] stub --- */
static const void * const dwrite_factory_vtbl[80];

typedef struct dwrite_factory {
    const void *lpVtbl;
    LONG ref;
} dwrite_factory_t;

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (iid_equals(iid, &g_iid_IUnknown) ||
        iid_equals(iid, &g_iid_IDWriteFactory) ||
        iid_equals(iid, &g_iid_IDWriteFactory1) ||
        iid_equals(iid, &g_iid_IDWriteFactory2) ||
        iid_equals(iid, &g_iid_IDWriteFactory3) ||
        iid_equals(iid, &g_iid_IDWriteFactory4) ||
        iid_equals(iid, &g_iid_IDWriteFactory5) ||
        iid_equals(iid, &g_iid_IDWriteFactory6) ||
        iid_equals(iid, &g_iid_IDWriteFactory7)) {
        *out = iface;
        common_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* GetGdiInterop(IDWriteGdiInterop **gdiInterop) */
static HRESULT STDMETHODCALLTYPE factory_GetGdiInterop(void *iface, void **gdiInterop)
{
    (void)iface;
    if (!gdiInterop) return E_POINTER;
    *gdiInterop = &g_gdi_interop;
    common_AddRef(&g_gdi_interop);
    return S_OK;
}

/* IDWriteFactory6::GetSystemFontCollection(BOOL include_downloadable,
 *     DWRITE_FONT_FAMILY_MODEL family_model, IDWriteFontCollection2 **out)
 *
 * Vtable slot 51.  Qt 6.x (built with QT_CONFIG(directwrite3)) calls this
 * from QWindowsDirectWriteFontDatabase::populateFontDatabase.  Returning
 * a populated collection here is the difference between Qt's font db
 * holding real family names and Qt falling through to QFontEngineBox. */
static HRESULT STDMETHODCALLTYPE factory6_GetSystemFontCollection(
    void *iface, BOOL include_downloadable, DWORD family_model, void **out)
{
    (void)iface;
    (void)include_downloadable;
    TRACE("(downloadable=%d, model=%lu, %p)\n",
          include_downloadable, family_model, out);
    return dwrite_get_system_font_collection(family_model, out);
}

/* IDWriteFactory::CreateGlyphRunAnalysis(DWRITE_GLYPH_RUN const *,
 *     FLOAT pixels_per_dip, DWRITE_MATRIX const *,
 *     DWRITE_RENDERING_MODE, DWRITE_MEASURING_MODE,
 *     FLOAT baseline_x, FLOAT baseline_y,
 *     IDWriteGlyphRunAnalysis **out) — slot 23.
 *
 * Qt 6's QFontEngineDirectWrite::imageForGlyph calls this to rasterize
 * each glyph into an alpha texture for blitting.  Without it Qt logs
 * "CreateGlyphRunAnalysis failed (0x80004001)" and returns an empty
 * QImage, leaving the dialog body blank even after the font collection
 * populates correctly. */
static HRESULT STDMETHODCALLTYPE factory_CreateGlyphRunAnalysis(
    void *iface, void const *run, FLOAT ppd, DWRITE_MATRIX const *transform,
    DWORD rendering_mode, DWORD measuring_mode,
    FLOAT baseline_x, FLOAT baseline_y, void **out)
{
    (void)iface;
    return dwrite_create_glyph_run_analysis(
        (DWRITE_GLYPH_RUN const *)run, ppd, transform,
        (DWRITE_RENDERING_MODE)rendering_mode,
        (DWRITE_MEASURING_MODE)measuring_mode,
        baseline_x, baseline_y, out);
}

/* IDWriteFactory2::CreateGlyphRunAnalysis — slot 30.
 * Signature drops the pixels-per-dip argument and adds grid_fit_mode +
 * antialias_mode (both ignored in Phase 2.5).  Qt 6 prefers this
 * overload when QI(IDWriteFactory2) succeeded, which it does for us. */
static HRESULT STDMETHODCALLTYPE factory2_CreateGlyphRunAnalysis(
    void *iface, void const *run, DWRITE_MATRIX const *transform,
    DWORD rendering_mode, DWORD measuring_mode,
    DWORD grid_fit_mode, DWORD antialias_mode,
    FLOAT baseline_x, FLOAT baseline_y, void **out)
{
    (void)iface; (void)grid_fit_mode; (void)antialias_mode;
    return dwrite_create_glyph_run_analysis(
        (DWRITE_GLYPH_RUN const *)run, 1.0f, transform,
        (DWRITE_RENDERING_MODE)rendering_mode,
        (DWRITE_MEASURING_MODE)measuring_mode,
        baseline_x, baseline_y, out);
}

static const void * const dwrite_factory_vtbl[80] = {
    factory_QueryInterface,               /* 0  QueryInterface */
    common_AddRef,                        /* 1  AddRef */
    common_Release,                       /* 2  Release */
    common_method_e_notimpl,              /* 3  GetSystemFontCollection */
    common_method_e_notimpl,              /* 4  CreateCustomFontCollection */
    common_method_e_notimpl,              /* 5  RegisterFontCollectionLoader */
    common_method_e_notimpl,              /* 6  UnregisterFontCollectionLoader */
    common_method_e_notimpl,              /* 7  CreateFontFileReference */
    common_method_e_notimpl,              /* 8  CreateCustomFontFileReference */
    common_method_e_notimpl,              /* 9  CreateFontFace */
    common_method_e_notimpl,              /* 10 CreateRenderingParams */
    common_method_e_notimpl,              /* 11 CreateMonitorRenderingParams */
    common_method_e_notimpl,              /* 12 CreateCustomRenderingParams */
    common_method_e_notimpl,              /* 13 RegisterFontFileLoader */
    common_method_e_notimpl,              /* 14 UnregisterFontFileLoader */
    common_method_e_notimpl,              /* 15 CreateTextFormat */
    common_method_e_notimpl,              /* 16 CreateTypography */
    factory_GetGdiInterop,                /* 17 GetGdiInterop */
    common_method_e_notimpl,              /* 18 CreateTextLayout */
    common_method_e_notimpl,              /* 19 CreateGdiCompatibleTextLayout */
    common_method_e_notimpl,              /* 20 CreateEllipsisTrimmingSign */
    common_method_e_notimpl,              /* 21 CreateTextAnalyzer */
    common_method_e_notimpl,              /* 22 CreateNumberSubstitution */
    factory_CreateGlyphRunAnalysis,       /* 23 CreateGlyphRunAnalysis */
    /* IDWriteFactory1 (slots 24-25) */
    common_method_e_notimpl,              /* 24 GetEudcFontCollection */
    common_method_e_notimpl,              /* 25 CreateCustomRenderingParams */
    /* IDWriteFactory2 (slots 26-30) */
    common_method_e_notimpl,              /* 26 GetSystemFontFallback */
    common_method_e_notimpl,              /* 27 CreateFontFallbackBuilder */
    common_method_e_notimpl,              /* 28 TranslateColorGlyphRun */
    common_method_e_notimpl,              /* 29 CreateCustomRenderingParams */
    factory2_CreateGlyphRunAnalysis,      /* 30 CreateGlyphRunAnalysis (FF2) */
    /* IDWriteFactory3 (slots 31-39) */
    common_method_e_notimpl,              /* 31 CreateGlyphRunAnalysis */
    common_method_e_notimpl,              /* 32 CreateCustomRenderingParams */
    common_method_e_notimpl,              /* 33 CreateFontFaceReference */
    common_method_e_notimpl,              /* 34 CreateFontFaceReference */
    common_method_e_notimpl,              /* 35 GetSystemFontSet */
    common_method_e_notimpl,              /* 36 CreateFontSetBuilder */
    common_method_e_notimpl,              /* 37 CreateFontCollectionFromFontSet */
    common_method_e_notimpl,              /* 38 GetSystemFontCollection */
    common_method_e_notimpl,              /* 39 GetFontDownloadQueue */
    /* IDWriteFactory4 (slots 40-42) */
    common_method_e_notimpl,              /* 40 TranslateColorGlyphRun */
    common_method_e_notimpl,              /* 41 ComputeGlyphOrigins */
    common_method_e_notimpl,              /* 42 ComputeGlyphOrigins */
    /* IDWriteFactory5 (slots 43-47) */
    common_method_e_notimpl,              /* 43 CreateFontSetBuilder */
    common_method_e_notimpl,              /* 44 CreateInMemoryFontFileLoader */
    common_method_e_notimpl,              /* 45 CreateHttpFontFileLoader */
    common_method_e_notimpl,              /* 46 AnalyzeContainerType */
    common_method_e_notimpl,              /* 47 UnpackFontFile (Factory5 last) */
    /* IDWriteFactory6 (slots 48-54) */
    common_method_e_notimpl,              /* 48 CreateFontFaceReference */
    common_method_e_notimpl,              /* 49 CreateFontResource */
    common_method_e_notimpl,              /* 50 GetSystemFontSet */
    factory6_GetSystemFontCollection,     /* 51 GetSystemFontCollection (Qt 6 hits this) */
    common_method_e_notimpl,              /* 52 CreateFontCollectionFromFontSet */
    common_method_e_notimpl,              /* 53 CreateFontSetBuilder */
    common_method_e_notimpl,              /* 54 CreateTextFormat */
    /* IDWriteFactory7 (slots 55-56) */
    common_method_e_notimpl,              /* 55 GetSystemFontSet */
    factory6_GetSystemFontCollection,     /* 56 GetSystemFontCollection (Factory7 — same impl) */
    /* IDWriteFactory8 reserve */
    common_method_e_notimpl,              /* 57 reserved */
    /* tail padding */
    common_method_e_notimpl, common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl, common_method_e_notimpl, common_method_e_notimpl,
    common_method_e_notimpl,
};

HRESULT WINAPI DWriteCreateFactory(DWORD factoryType, REFIID iid, IUnknown **factory)
{
    dwrite_factory_t *f;
    TRACE("(%u,%p,%p)\n", factoryType, iid, factory);

    if (!factory) return E_POINTER;
    *factory = NULL;

    f = HeapAlloc(GetProcessHeap(), 0, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->lpVtbl = dwrite_factory_vtbl;
    f->ref = 1;
    *factory = (IUnknown *)f;
    return S_OK;
}
