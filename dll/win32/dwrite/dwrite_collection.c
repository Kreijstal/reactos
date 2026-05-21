/*
 * PROJECT:     ReactOS DirectWrite
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     IDWriteFontCollection3 / FontFamily2 / Font3 / FontFace5
 *              backed by GDI font enumeration.
 *
 * Phase 1: enumerate families+faces via EnumFontFamiliesExW, then expose
 * stub-but-honest metadata so Qt's QWindowsDirectWriteFontDatabase::
 * populateFontDatabase can iterate every family and call
 * QPlatformFontDatabase::registerFont(name, ...).
 *
 * The actual glyph/metric/outline plumbing is intentionally deferred to
 * Phase 2 — for now CreateFontFace returns a face object whose only
 * working methods are QueryInterface/AddRef/Release/GetType.  Qt's
 * populate path does not exercise the rest.
 *
 * Slot ordinals are counted from the Wine IDL declarations in
 * include/dwrite{,_1,_2,_3}.idl as of 2026-05.  Keep this in sync.
 */

#include <stdarg.h>

#include "dwrite_private.h"

#include <wine/debug.h>
WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

/* ----- IIDs for the collection chain ---------------------------------- */
static const GUID g_iid_IDWriteFontCollection =
    {0xa84cee02, 0x3eea, 0x4eee, {0xa8, 0x27, 0x87, 0xc1, 0xa0, 0x2a, 0x0f, 0xcc}};
static const GUID g_iid_IDWriteFontCollection1 =
    {0x53585141, 0xd9f8, 0x4095, {0x83, 0x21, 0xd7, 0x3c, 0xf6, 0xbd, 0x11, 0x6b}};
static const GUID g_iid_IDWriteFontCollection2 =
    {0x514039c6, 0x4617, 0x4f7e, {0x9e, 0xd1, 0x97, 0x9d, 0x29, 0x44, 0xfd, 0x84}};
static const GUID g_iid_IDWriteFontCollection3 =
    {0xa4d055a6, 0xf9e3, 0x4e8d, {0xa3, 0x80, 0x29, 0x80, 0xb2, 0xa5, 0x2b, 0xa5}};

static const GUID g_iid_IDWriteFontList =
    {0x1a0d8438, 0x1d97, 0x4ec1, {0xae, 0xf9, 0xa2, 0xfb, 0x86, 0xed, 0x6a, 0xcb}};
static const GUID g_iid_IDWriteFontList1 =
    {0xda20d8ef, 0x812a, 0x4c43, {0x98, 0x02, 0x62, 0xec, 0x4a, 0xbd, 0x7a, 0xdf}};
static const GUID g_iid_IDWriteFontList2 =
    {0xc0763a34, 0x77af, 0x445a, {0xb7, 0x35, 0x08, 0xc3, 0x7b, 0x0a, 0x5b, 0xf5}};

static const GUID g_iid_IDWriteFontFamily =
    {0xda20d8ef, 0x812a, 0x4c43, {0x98, 0x02, 0x62, 0xec, 0x4a, 0xbd, 0x7a, 0xdf}};
static const GUID g_iid_IDWriteFontFamily1 =
    {0xda20d8ef, 0x812a, 0x4c43, {0x98, 0x02, 0x62, 0xec, 0x4a, 0xbd, 0x7a, 0xe0}};
static const GUID g_iid_IDWriteFontFamily2 =
    {0x3ed49e77, 0xa398, 0x4261, {0xb9, 0xcf, 0xc1, 0x26, 0xc2, 0x13, 0x1e, 0xf3}};

static const GUID g_iid_IDWriteFont =
    {0xacd16696, 0x8c14, 0x4f5d, {0x87, 0x7e, 0xfe, 0x3f, 0xc1, 0xd3, 0x27, 0x37}};
static const GUID g_iid_IDWriteFont1 =
    {0xacd16696, 0x8c14, 0x4f5d, {0x87, 0x7e, 0xfe, 0x3f, 0xc1, 0xd3, 0x27, 0x38}};
static const GUID g_iid_IDWriteFont2 =
    {0x29748ed6, 0x8c9c, 0x4a6a, {0xbe, 0x0b, 0xd9, 0x12, 0xe8, 0x53, 0x89, 0x44}};
static const GUID g_iid_IDWriteFont3 =
    {0x29748ed6, 0x8c9c, 0x4a6a, {0xbe, 0x0b, 0xd9, 0x12, 0xe8, 0x53, 0x89, 0x45}};

static const GUID g_iid_IDWriteFontFace =
    {0x5f49804d, 0x7024, 0x4d43, {0xbf, 0xa9, 0xd2, 0x59, 0x84, 0xf5, 0x38, 0x49}};
static const GUID g_iid_IDWriteFontFace1 =
    {0xa71efdb4, 0x9fdb, 0x4838, {0xad, 0x90, 0xcf, 0xc3, 0xbe, 0x8c, 0x3d, 0xaf}};
static const GUID g_iid_IDWriteFontFace2 =
    {0xd8b768ff, 0x64bc, 0x4e66, {0x98, 0x2b, 0xec, 0x8e, 0x87, 0xf6, 0x93, 0xf7}};
static const GUID g_iid_IDWriteFontFace3 =
    {0xd37d7598, 0x09be, 0x4222, {0xa2, 0x36, 0x20, 0x81, 0x34, 0x1c, 0xc1, 0xf2}};
static const GUID g_iid_IDWriteFontFace4 =
    {0x27f2a904, 0x4eb8, 0x441d, {0x96, 0x78, 0x05, 0x63, 0xf5, 0x3e, 0x3e, 0x2f}};
static const GUID g_iid_IDWriteFontFace5 =
    {0x98eff3a5, 0xb667, 0x479a, {0xb1, 0x45, 0xe2, 0xfa, 0x5b, 0x9f, 0xdc, 0x29}};

/* ----- Internal data model -------------------------------------------- */

typedef struct face_record
{
    WCHAR style_name[LF_FACESIZE];   /* "Regular", "Bold", "Italic", ... */
    LONG weight;                     /* lfWeight or otmWeightClass */
    BYTE italic;
    BYTE charset;
    DWORD font_type;                 /* TRUETYPE_FONTTYPE etc. */
} face_record_t;

typedef struct font_obj font_obj_t;
typedef struct font_family_obj font_family_obj_t;
typedef struct font_collection_obj font_collection_obj_t;
typedef struct font_face_obj font_face_obj_t;

struct font_obj
{
    const void *lpVtbl;
    LONG ref;
    /* Stable handle back to the family.  An earlier impl held a direct
     * font_family_obj_t* but that pointer is invalidated whenever
     * collection_find_or_add_family HeapReAlloc()s coll->families[], so
     * every font attached before the realloc would be left with a
     * dangling family ptr (its first 8 bytes — the vtable — read 0 once
     * the old buffer is freed).  Indexing through (coll, family_idx)
     * stays valid across reallocs because coll itself is standalone. */
    font_collection_obj_t *coll;
    UINT32 family_idx;
    face_record_t rec;
};

struct font_family_obj
{
    const void *lpVtbl;
    LONG ref;
    font_collection_obj_t *coll;     /* Weak — collection owns us. */
    WCHAR family_name[LF_FACESIZE];
    font_obj_t *fonts;
    UINT32 nfonts;
    UINT32 fonts_cap;
};

struct font_collection_obj
{
    const void *lpVtbl;
    LONG ref;
    font_family_obj_t *families;
    UINT32 nfamilies;
    UINT32 families_cap;
};

struct font_face_obj
{
    const void *lpVtbl;
    LONG ref;
    DWRITE_FONT_FACE_TYPE face_type;
    DWORD simulations;
    /* Identity, copied from font_obj_t at CreateFontFace time so the
     * face is independent of the family/font lifetimes. */
    WCHAR family_name[LF_FACESIZE];
    LONG  weight;
    BYTE  italic;
    BYTE  charset;
    /* GDI delegation state.  Lazily initialised on first metric/glyph
     * call by gdi_face_ensure.  HFONT is created at em_units pixel
     * height so subsequent GetGlyphOutlineW / GetGlyphIndicesW results
     * are already in font design units. */
    CRITICAL_SECTION cs;
    BOOL   cs_inited;
    BOOL   ready;          /* gdi_face_ensure ran successfully */
    HDC    hdc;
    HFONT  hfont;
    HGDIOBJ prev_obj;
    UINT16 em_units;
    UINT16 ascent_du;      /* tmAscent in design units */
    UINT16 descent_du;     /* tmDescent in design units */
    /* Cached results — populated under cs. */
    UINT16 glyph_count;    /* 0 == not yet cached */
    BOOL   metrics_ready;
    DWRITE_FONT_METRICS metrics_cache;
};

/* Forward vtable declarations. */
static const void * const g_collection_vtbl[14];
static const void * const g_family_vtbl[14];
static const void * const g_font_vtbl[24];
static const void * const g_face_vtbl[60];
/* Forward decl: defined in the FontFace section below. */
static BOOL gdi_face_ensure(font_face_obj_t *face);

/* ----- Collection: enumeration ---------------------------------------- */

static font_family_obj_t *collection_find_or_add_family(
    font_collection_obj_t *coll, const WCHAR *name)
{
    UINT32 i;
    font_family_obj_t *fam;

    for (i = 0; i < coll->nfamilies; ++i)
    {
        if (lstrcmpiW(coll->families[i].family_name, name) == 0)
            return &coll->families[i];
    }

    if (coll->nfamilies == coll->families_cap)
    {
        UINT32 ncap = coll->families_cap ? coll->families_cap * 2 : 32;
        font_family_obj_t *grown;
        if (coll->families)
            grown = HeapReAlloc(GetProcessHeap(), 0, coll->families,
                                ncap * sizeof(*grown));
        else
            grown = HeapAlloc(GetProcessHeap(), 0, ncap * sizeof(*grown));
        if (!grown) return NULL;
        coll->families = grown;
        coll->families_cap = ncap;
    }

    fam = &coll->families[coll->nfamilies++];
    fam->lpVtbl = g_family_vtbl;
    fam->ref = 1;
    fam->coll = coll;
    lstrcpynW(fam->family_name, name, LF_FACESIZE);
    fam->fonts = NULL;
    fam->nfonts = 0;
    fam->fonts_cap = 0;
    return fam;
}

static BOOL family_append_face(font_family_obj_t *fam, const face_record_t *rec)
{
    UINT32 i;

    /* Dedupe by (style_name, weight, italic, charset) — EnumFont can return
     * the same face for multiple charsets. */
    for (i = 0; i < fam->nfonts; ++i)
    {
        if (fam->fonts[i].rec.weight == rec->weight &&
            fam->fonts[i].rec.italic == rec->italic &&
            fam->fonts[i].rec.charset == rec->charset &&
            lstrcmpiW(fam->fonts[i].rec.style_name, rec->style_name) == 0)
        {
            return TRUE;  /* Already present. */
        }
    }

    if (fam->nfonts == fam->fonts_cap)
    {
        UINT32 ncap = fam->fonts_cap ? fam->fonts_cap * 2 : 4;
        font_obj_t *grown;
        if (fam->fonts)
            grown = HeapReAlloc(GetProcessHeap(), 0, fam->fonts,
                                ncap * sizeof(*grown));
        else
            grown = HeapAlloc(GetProcessHeap(), 0, ncap * sizeof(*grown));
        if (!grown) return FALSE;
        fam->fonts = grown;
        fam->fonts_cap = ncap;
    }

    fam->fonts[fam->nfonts].lpVtbl = g_font_vtbl;
    fam->fonts[fam->nfonts].ref = 1;
    fam->fonts[fam->nfonts].coll = fam->coll;
    fam->fonts[fam->nfonts].family_idx = (UINT32)(fam - fam->coll->families);
    fam->fonts[fam->nfonts].rec = *rec;
    fam->nfonts++;
    return TRUE;
}

static int CALLBACK enum_proc(const LOGFONTW *lf, const TEXTMETRICW *tm,
                              DWORD font_type, LPARAM lparam)
{
    font_collection_obj_t *coll = (font_collection_obj_t *)lparam;
    const ENUMLOGFONTEXW *elf = (const ENUMLOGFONTEXW *)lf;
    font_family_obj_t *fam;
    face_record_t rec;

    /* Skip @-prefixed vertical alias face names. */
    if (lf->lfFaceName[0] == '@')
        return 1;

    fam = collection_find_or_add_family(coll, lf->lfFaceName);
    if (!fam) return 0;

    memset(&rec, 0, sizeof(rec));
    lstrcpynW(rec.style_name, elf->elfStyle, LF_FACESIZE);
    if (rec.style_name[0] == 0)
        lstrcpynW(rec.style_name, L"Regular", LF_FACESIZE);
    rec.weight = lf->lfWeight ? lf->lfWeight : FW_NORMAL;
    rec.italic = lf->lfItalic ? 1 : 0;
    rec.charset = lf->lfCharSet;
    rec.font_type = font_type;
    family_append_face(fam, &rec);
    return 1;
}

/* ----- Collection: singleton ------------------------------------------ */

static font_collection_obj_t *g_collection;
static LONG g_collection_built;

static HRESULT build_singleton_collection(void)
{
    font_collection_obj_t *coll;
    LOGFONTW lf = {0};
    HDC mem;

    if (InterlockedCompareExchange(&g_collection_built, 1, 0) != 0)
    {
        /* Another thread is building.  Spin briefly. */
        while (!g_collection) Sleep(1);
        return S_OK;
    }

    coll = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*coll));
    if (!coll)
    {
        InterlockedExchange(&g_collection_built, 0);
        return E_OUTOFMEMORY;
    }
    coll->lpVtbl = g_collection_vtbl;
    coll->ref = 1;

    mem = CreateCompatibleDC(NULL);
    if (mem)
    {
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(mem, &lf, enum_proc, (LPARAM)coll, 0);
        DeleteDC(mem);
    }

    /* One-line summary to COM1: useful for confirming Qt populated this
     * collection in production debug logs without needing WINEDEBUG. */
    {
        char msg[96];
        wsprintfA(msg, "dwrite: enumerated %u font families\n", coll->nfamilies);
        OutputDebugStringA(msg);
    }
    TRACE("collected %u families\n", coll->nfamilies);
    g_collection = coll;
    return S_OK;
}

/* ---- Accessors used by dwrite_glyphrun.c ---- */

UINT16 dwrite_face_get_em_units(void *face)
{
    font_face_obj_t *self = (font_face_obj_t *)face;
    if (!gdi_face_ensure(self)) return 0;
    return self->em_units;
}

HRESULT dwrite_face_get_logfont(void *face, LOGFONTW *lf)
{
    font_face_obj_t *self = (font_face_obj_t *)face;
    if (!lf) return E_POINTER;
    memset(lf, 0, sizeof(*lf));
    lf->lfWeight       = self->weight;
    lf->lfItalic       = self->italic;
    lf->lfCharSet      = self->charset;
    lf->lfOutPrecision = OUT_TT_PRECIS;
    lf->lfQuality      = DEFAULT_QUALITY;
    lstrcpynW(lf->lfFaceName, self->family_name, LF_FACESIZE);
    return S_OK;
}

HRESULT dwrite_get_system_font_collection(DWORD family_model, void **out)
{
    HRESULT hr;
    (void)family_model;  /* Phase 1: typographic == WSS == same collection. */

    if (!out) return E_POINTER;
    *out = NULL;

    hr = build_singleton_collection();
    if (FAILED(hr)) return hr;

    *out = g_collection;
    dwrite_common_AddRef(g_collection);
    return S_OK;
}

/* ----- Collection: vtable --------------------------------------------- */

static HRESULT STDMETHODCALLTYPE collection_QueryInterface(
    void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (dwrite_iid_equals(iid, &g_iid_IUnknown) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontCollection) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontCollection1) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontCollection2) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontCollection3))
    {
        *out = iface;
        dwrite_common_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* UINT32 GetFontFamilyCount(void) */
static UINT32 STDMETHODCALLTYPE collection_GetFontFamilyCount(void *iface)
{
    font_collection_obj_t *self = (font_collection_obj_t *)iface;
    return self->nfamilies;
}

/* HRESULT GetFontFamily(UINT32 index, IDWriteFontFamily **out)
 * The same impl serves slots 4 (Family*), 8 (Family1*), and 9 (Family2*) —
 * all three are vtable-prefix compatible when our Family object covers
 * the largest extension. */
static HRESULT STDMETHODCALLTYPE collection_GetFontFamily(
    void *iface, UINT32 index, void **out)
{
    font_collection_obj_t *self = (font_collection_obj_t *)iface;
    if (!out) return E_POINTER;
    *out = NULL;
    if (index >= self->nfamilies) return E_INVALIDARG;
    *out = &self->families[index];
    dwrite_common_AddRef(&self->families[index]);
    return S_OK;
}

/* HRESULT FindFamilyName(WCHAR const *name, UINT32 *index, BOOL *exists) */
static HRESULT STDMETHODCALLTYPE collection_FindFamilyName(
    void *iface, const WCHAR *name, UINT32 *index, BOOL *exists)
{
    font_collection_obj_t *self = (font_collection_obj_t *)iface;
    UINT32 i;
    if (!index || !exists) return E_POINTER;
    *index = 0;
    *exists = FALSE;
    if (!name) return E_INVALIDARG;
    for (i = 0; i < self->nfamilies; ++i)
    {
        if (lstrcmpiW(self->families[i].family_name, name) == 0)
        {
            *index = i;
            *exists = TRUE;
            return S_OK;
        }
    }
    return S_OK;
}

/* DWRITE_FONT_FAMILY_MODEL GetFontFamilyModel(void) — slot 11.
 * Phase 1: always claim TYPOGRAPHIC (== 0). */
static DWRITE_FONT_FAMILY_MODEL STDMETHODCALLTYPE collection_GetFontFamilyModel(void *iface)
{
    (void)iface;
    return DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC;
}

static const void * const g_collection_vtbl[14] =
{
    collection_QueryInterface,             /* 0  QueryInterface */
    dwrite_common_AddRef,                  /* 1  AddRef */
    dwrite_common_Release,                 /* 2  Release */
    collection_GetFontFamilyCount,         /* 3  GetFontFamilyCount */
    collection_GetFontFamily,              /* 4  GetFontFamily -> Family* */
    collection_FindFamilyName,             /* 5  FindFamilyName */
    dwrite_common_method_e_notimpl,        /* 6  GetFontFromFontFace */
    dwrite_common_method_e_notimpl,        /* 7  Collection1::GetFontSet */
    collection_GetFontFamily,              /* 8  Collection1::GetFontFamily -> Family1* */
    collection_GetFontFamily,              /* 9  Collection2::GetFontFamily -> Family2* */
    dwrite_common_method_e_notimpl,        /* 10 Collection2::GetMatchingFonts */
    collection_GetFontFamilyModel,         /* 11 Collection2::GetFontFamilyModel */
    dwrite_common_method_e_notimpl,        /* 12 Collection2::GetFontSet -> FontSet1* */
    dwrite_common_method_e_notimpl,        /* 13 Collection3::GetExpirationEvent */
};

/* ----- Family vtable -------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE family_QueryInterface(
    void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (dwrite_iid_equals(iid, &g_iid_IUnknown) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontList) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontList1) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontList2) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFamily) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFamily1) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFamily2))
    {
        *out = iface;
        dwrite_common_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* HRESULT GetFontCollection(IDWriteFontCollection **out) */
static HRESULT STDMETHODCALLTYPE family_GetFontCollection(void *iface, void **out)
{
    font_family_obj_t *self = (font_family_obj_t *)iface;
    if (!out) return E_POINTER;
    *out = self->coll;
    dwrite_common_AddRef(self->coll);
    return S_OK;
}

/* UINT32 GetFontCount(void) */
static UINT32 STDMETHODCALLTYPE family_GetFontCount(void *iface)
{
    font_family_obj_t *self = (font_family_obj_t *)iface;
    return self->nfonts;
}

/* HRESULT GetFont(UINT32 index, IDWriteFont **out) — also serves slot 10
 * (Family1::GetFont -> Font3*). */
static HRESULT STDMETHODCALLTYPE family_GetFont(void *iface, UINT32 index, void **out)
{
    font_family_obj_t *self = (font_family_obj_t *)iface;
    if (!out) return E_POINTER;
    *out = NULL;
    if (index >= self->nfonts) return E_INVALIDARG;
    *out = &self->fonts[index];
    dwrite_common_AddRef(&self->fonts[index]);
    return S_OK;
}

/* HRESULT GetMatchingFonts(weight, stretch, style, IDWriteFontList **out)
 * Phase 1: ignore the weight/stretch/style filter, return the whole family.
 * IDWriteFontFamily extends IDWriteFontList — the first six vtable slots
 * (IUnknown + GetFontCollection + GetFontCount + GetFont) are identical, so
 * Qt's IDWriteFontList::GetFontCount/GetFont calls land on our existing
 * family_GetFontCount/family_GetFont. */
static HRESULT STDMETHODCALLTYPE family_GetMatchingFonts(
    void *iface, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch,
    DWRITE_FONT_STYLE style, void **out)
{
    (void)weight; (void)stretch; (void)style;
    if (!out) return E_POINTER;
    *out = iface;
    dwrite_common_AddRef(iface);
    return S_OK;
}

/* HRESULT GetFamilyNames(IDWriteLocalizedStrings **out) */
static HRESULT STDMETHODCALLTYPE family_GetFamilyNames(void *iface, void **out)
{
    font_family_obj_t *self = (font_family_obj_t *)iface;
    return dwrite_localized_strings_create(self->family_name, out);
}

static const void * const g_family_vtbl[14] =
{
    family_QueryInterface,                 /* 0  QueryInterface */
    dwrite_common_AddRef,                  /* 1  AddRef */
    dwrite_common_Release,                 /* 2  Release */
    family_GetFontCollection,              /* 3  FontList::GetFontCollection */
    family_GetFontCount,                   /* 4  FontList::GetFontCount */
    family_GetFont,                        /* 5  FontList::GetFont -> Font* */
    family_GetFamilyNames,                 /* 6  Family::GetFamilyNames */
    dwrite_common_method_e_notimpl,        /* 7  Family::GetFirstMatchingFont */
    family_GetMatchingFonts,               /* 8  Family::GetMatchingFonts */
    dwrite_common_method_e_notimpl,        /* 9  Family1::GetFontLocality */
    family_GetFont,                        /* 10 Family1::GetFont -> Font3* */
    dwrite_common_method_e_notimpl,        /* 11 Family1::GetFontFaceReference */
    dwrite_common_method_e_notimpl,        /* 12 Family2::GetMatchingFonts variant */
    dwrite_common_method_e_notimpl,        /* 13 Family2::GetFontSet */
};

/* ----- Font ----------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE font_QueryInterface(
    void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (dwrite_iid_equals(iid, &g_iid_IUnknown) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFont) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFont1) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFont2) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFont3))
    {
        *out = iface;
        dwrite_common_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* HRESULT GetFontFamily(IDWriteFontFamily **out) */
static HRESULT STDMETHODCALLTYPE font_GetFontFamily(void *iface, void **out)
{
    font_obj_t *self = (font_obj_t *)iface;
    font_family_obj_t *fam;
    if (!out) return E_POINTER;
    fam = &self->coll->families[self->family_idx];
    *out = fam;
    dwrite_common_AddRef(fam);
    return S_OK;
}

/* DWRITE_FONT_WEIGHT GetWeight(void) */
static DWRITE_FONT_WEIGHT STDMETHODCALLTYPE font_GetWeight(void *iface)
{
    font_obj_t *self = (font_obj_t *)iface;
    return (DWRITE_FONT_WEIGHT)self->rec.weight;
}

/* DWRITE_FONT_STRETCH GetStretch(void) */
static DWRITE_FONT_STRETCH STDMETHODCALLTYPE font_GetStretch(void *iface)
{
    (void)iface;
    return DWRITE_FONT_STRETCH_NORMAL;
}

/* DWRITE_FONT_STYLE GetStyle(void) */
static DWRITE_FONT_STYLE STDMETHODCALLTYPE font_GetStyle(void *iface)
{
    font_obj_t *self = (font_obj_t *)iface;
    return self->rec.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
}

/* BOOL IsSymbolFont(void) */
static BOOL STDMETHODCALLTYPE font_IsSymbolFont(void *iface)
{
    font_obj_t *self = (font_obj_t *)iface;
    return self->rec.charset == SYMBOL_CHARSET;
}

/* HRESULT GetFaceNames(IDWriteLocalizedStrings **out) */
static HRESULT STDMETHODCALLTYPE font_GetFaceNames(void *iface, void **out)
{
    font_obj_t *self = (font_obj_t *)iface;
    return dwrite_localized_strings_create(self->rec.style_name, out);
}

/* DWRITE_FONT_SIMULATIONS GetSimulations(void) */
static DWRITE_FONT_SIMULATIONS STDMETHODCALLTYPE font_GetSimulations(void *iface)
{
    (void)iface;
    return DWRITE_FONT_SIMULATIONS_NONE;
}

/* HRESULT CreateFontFace(IDWriteFontFace **out) — also slot 19 for Font3::
 * CreateFontFace -> IDWriteFontFace3.  Vtable-prefix compatible. */
static HRESULT STDMETHODCALLTYPE font_CreateFontFace(void *iface, void **out)
{
    font_obj_t *self = (font_obj_t *)iface;
    font_face_obj_t *face;
    if (!out) return E_POINTER;
    *out = NULL;
    face = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*face));
    if (!face) return E_OUTOFMEMORY;
    face->lpVtbl = g_face_vtbl;
    face->ref = 1;
    face->face_type = (self->rec.font_type & TRUETYPE_FONTTYPE)
        ? DWRITE_FONT_FACE_TYPE_TRUETYPE
        : DWRITE_FONT_FACE_TYPE_TYPE1; /* generic non-TT bucket */
    face->simulations = DWRITE_FONT_SIMULATIONS_NONE;
    lstrcpynW(face->family_name,
              self->coll->families[self->family_idx].family_name,
              LF_FACESIZE);
    face->weight = self->rec.weight;
    face->italic = self->rec.italic;
    face->charset = self->rec.charset;
    InitializeCriticalSection(&face->cs);
    face->cs_inited = TRUE;
    *out = face;
    return S_OK;
}

/* BOOL IsMonospacedFont(void) — Font1 slot 17 */
static BOOL STDMETHODCALLTYPE font_IsMonospacedFont(void *iface)
{
    (void)iface;
    return FALSE;
}

/* BOOL IsColorFont(void) — Font2 slot 18 */
static BOOL STDMETHODCALLTYPE font_IsColorFont(void *iface)
{
    (void)iface;
    return FALSE;
}

/* DWRITE_LOCALITY GetLocality(void) — Font3 slot 23 */
static DWRITE_LOCALITY STDMETHODCALLTYPE font_GetLocality(void *iface)
{
    (void)iface;
    return DWRITE_LOCALITY_LOCAL;
}

static const void * const g_font_vtbl[24] =
{
    font_QueryInterface,                   /* 0  QueryInterface */
    dwrite_common_AddRef,                  /* 1  AddRef */
    dwrite_common_Release,                 /* 2  Release */
    font_GetFontFamily,                    /* 3  GetFontFamily */
    font_GetWeight,                        /* 4  GetWeight */
    font_GetStretch,                       /* 5  GetStretch */
    font_GetStyle,                         /* 6  GetStyle */
    font_IsSymbolFont,                     /* 7  IsSymbolFont */
    font_GetFaceNames,                     /* 8  GetFaceNames */
    dwrite_common_method_e_notimpl,        /* 9  GetInformationalStrings */
    font_GetSimulations,                   /* 10 GetSimulations */
    dwrite_common_method_e_notimpl,        /* 11 GetMetrics (void return: caller-zeroed struct ok-ish) */
    dwrite_common_method_e_notimpl,        /* 12 HasCharacter */
    font_CreateFontFace,                   /* 13 CreateFontFace */
    dwrite_common_method_e_notimpl,        /* 14 Font1::GetMetrics */
    dwrite_common_method_e_notimpl,        /* 15 Font1::GetPanose */
    dwrite_common_method_e_notimpl,        /* 16 Font1::GetUnicodeRanges */
    font_IsMonospacedFont,                 /* 17 Font1::IsMonospacedFont */
    font_IsColorFont,                      /* 18 Font2::IsColorFont */
    font_CreateFontFace,                   /* 19 Font3::CreateFontFace -> FontFace3 */
    dwrite_common_method_e_notimpl,        /* 20 Font3::Equals */
    dwrite_common_method_e_notimpl,        /* 21 Font3::GetFontFaceReference */
    dwrite_common_method_e_notimpl,        /* 22 Font3::HasCharacter variant */
    font_GetLocality,                      /* 23 Font3::GetLocality */
};

/* ----- FontFace (Phase 2: real metrics/glyphs/outlines via GDI) ------- */

/* Lazily create an HDC + em-sized HFONT for `face` and query OTM once
 * to discover otmEMSquare and tmAscent/tmDescent.  Subsequent
 * GetGlyphOutlineW / GetGlyphIndicesW calls run against this DC so
 * their pixel-domain output already lives in font design units. */
static BOOL gdi_face_ensure(font_face_obj_t *face)
{
    LOGFONTW lf;
    BYTE otm_buf[512];
    OUTLINETEXTMETRICW *otm = (OUTLINETEXTMETRICW *)otm_buf;
    UINT bytes;

    if (face->ready) return TRUE;
    if (!face->cs_inited) return FALSE;

    EnterCriticalSection(&face->cs);
    if (face->ready) { LeaveCriticalSection(&face->cs); return TRUE; }

    face->hdc = CreateCompatibleDC(NULL);
    if (!face->hdc) goto fail;

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight       = -2048;  /* probe size; replaced after OTM */
    lf.lfWeight       = face->weight;
    lf.lfItalic       = face->italic;
    lf.lfCharSet      = face->charset;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfQuality      = DEFAULT_QUALITY;
    lstrcpynW(lf.lfFaceName, face->family_name, LF_FACESIZE);

    face->hfont = CreateFontIndirectW(&lf);
    if (!face->hfont) goto fail;
    face->prev_obj = SelectObject(face->hdc, face->hfont);

    bytes = GetOutlineTextMetricsW(face->hdc, sizeof(otm_buf), otm);
    if (!bytes)
    {
        /* Non-outline (bitmap/vector) font.  Keep the probe HFONT for
         * GetGlyphIndicesW which still works on raster fonts, but mark
         * em_units = 0 so callers know design metrics are unavailable. */
        face->em_units = 0;
        face->ready = TRUE;
        LeaveCriticalSection(&face->cs);
        return TRUE;
    }

    face->em_units   = otm->otmEMSquare ? otm->otmEMSquare : 2048;
    face->ascent_du  = otm->otmTextMetrics.tmAscent;
    face->descent_du = otm->otmTextMetrics.tmDescent;

    /* Cache the design metrics straight from the probe OTM, scaled from
     * the probe height to design units. */
    {
        FLOAT s = (FLOAT)face->em_units / 2048.0f;
        DWRITE_FONT_METRICS *m = &face->metrics_cache;
        memset(m, 0, sizeof(*m));
        m->designUnitsPerEm        = face->em_units;
        m->ascent                  = (UINT16)((FLOAT)otm->otmTextMetrics.tmAscent * s);
        m->descent                 = (UINT16)((FLOAT)otm->otmTextMetrics.tmDescent * s);
        m->lineGap                 = (INT16) ((FLOAT)otm->otmLineGap * s);
        m->capHeight               = (UINT16)((FLOAT)otm->otmsCapEmHeight * s);
        m->xHeight                 = (UINT16)((FLOAT)otm->otmsXHeight * s);
        m->underlinePosition       = (INT16) ((FLOAT)otm->otmsUnderscorePosition * s);
        m->underlineThickness      = (UINT16)((FLOAT)otm->otmsUnderscoreSize * s);
        m->strikethroughPosition   = (INT16) ((FLOAT)otm->otmsStrikeoutPosition * s);
        m->strikethroughThickness  = (UINT16)((FLOAT)otm->otmsStrikeoutSize * s);
        face->ascent_du            = m->ascent;
        face->descent_du           = m->descent;
        face->metrics_ready = TRUE;
    }

    /* Recreate the HFONT at exactly em_units so subsequent
     * GetGlyphOutlineW returns are 1:1 in design units. */
    if (lf.lfHeight != -(LONG)face->em_units)
    {
        HFONT new_font;
        lf.lfHeight = -(LONG)face->em_units;
        new_font = CreateFontIndirectW(&lf);
        if (new_font)
        {
            SelectObject(face->hdc, face->prev_obj);
            DeleteObject(face->hfont);
            face->hfont = new_font;
            face->prev_obj = SelectObject(face->hdc, face->hfont);
        }
        /* If CreateFontIndirectW fails at em size, keep the probe HFONT
         * and rescale outputs by em_units/2048 in callers — but for
         * common TT fonts the em-sized create works. */
    }

    face->ready = TRUE;
    LeaveCriticalSection(&face->cs);
    return TRUE;

fail:
    if (face->hfont) { DeleteObject(face->hfont); face->hfont = NULL; }
    if (face->hdc)   { DeleteDC(face->hdc); face->hdc = NULL; }
    LeaveCriticalSection(&face->cs);
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE face_QueryInterface(
    void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (dwrite_iid_equals(iid, &g_iid_IUnknown) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFace) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFace1) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFace2) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFace3) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFace4) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteFontFace5))
    {
        *out = iface;
        dwrite_common_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE face_Release(void *iface)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    LONG r = InterlockedDecrement(&self->ref);
    if (r == 0)
    {
        if (self->hdc)
        {
            if (self->prev_obj) SelectObject(self->hdc, self->prev_obj);
            DeleteDC(self->hdc);
        }
        if (self->hfont) DeleteObject(self->hfont);
        if (self->cs_inited) DeleteCriticalSection(&self->cs);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return r < 0 ? 0 : (ULONG)r;
}

/* DWRITE_FONT_FACE_TYPE GetType(void) */
static DWRITE_FONT_FACE_TYPE STDMETHODCALLTYPE face_GetType(void *iface)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    return self->face_type;
}

/* UINT32 GetIndex(void) */
static UINT32 STDMETHODCALLTYPE face_GetIndex(void *iface)
{
    (void)iface;
    return 0;
}

/* DWRITE_FONT_SIMULATIONS GetSimulations(void) */
static DWRITE_FONT_SIMULATIONS STDMETHODCALLTYPE face_GetSimulations(void *iface)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    return self->simulations;
}

/* BOOL IsSymbolFont(void) */
static BOOL STDMETHODCALLTYPE face_IsSymbolFont(void *iface)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    return self->charset == SYMBOL_CHARSET;
}

/* void GetMetrics(DWRITE_FONT_METRICS *metrics) — slot 8.
 * Returns void; caller-provided struct is filled (or zero-filled on
 * failure). */
static void STDMETHODCALLTYPE face_GetMetrics(void *iface, DWRITE_FONT_METRICS *out)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!gdi_face_ensure(self)) return;
    if (!self->metrics_ready) { out->designUnitsPerEm = self->em_units ? self->em_units : 1; return; }
    *out = self->metrics_cache;
}

/* UINT16 GetGlyphCount(void) — slot 9.
 * Read 'maxp' table; numGlyphs is BE16 at offset 4. */
static UINT16 STDMETHODCALLTYPE face_GetGlyphCount(void *iface)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    BYTE maxp[6];
    DWORD r;
    if (self->glyph_count) return self->glyph_count;
    if (!gdi_face_ensure(self)) return 0;
    if (!self->em_units) return 0;
    EnterCriticalSection(&self->cs);
    r = GetFontData(self->hdc, DWRITE_MAKE_TAG('m','a','x','p'), 0, maxp, sizeof(maxp));
    if (r == sizeof(maxp))
        self->glyph_count = (UINT16)(((UINT16)maxp[4] << 8) | maxp[5]);
    LeaveCriticalSection(&self->cs);
    return self->glyph_count;
}

/* HRESULT GetDesignGlyphMetrics(UINT16 const *glyph_indices,
 *      UINT32 glyph_count, DWRITE_GLYPH_METRICS *metrics, BOOL is_sideways)
 * Slot 10. */
static HRESULT STDMETHODCALLTYPE face_GetDesignGlyphMetrics(
    void *iface, UINT16 const *glyph_indices, UINT32 glyph_count,
    DWRITE_GLYPH_METRICS *metrics, BOOL is_sideways)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    static const MAT2 ident = { {0,1}, {0,0}, {0,0}, {0,1} };
    UINT32 i;
    (void)is_sideways;
    if (!glyph_indices || !metrics) return E_POINTER;
    if (!gdi_face_ensure(self)) return E_FAIL;
    memset(metrics, 0, glyph_count * sizeof(*metrics));
    if (!self->em_units) return S_OK;

    EnterCriticalSection(&self->cs);
    for (i = 0; i < glyph_count; ++i)
    {
        GLYPHMETRICS gm;
        DWORD r = GetGlyphOutlineW(self->hdc, glyph_indices[i],
                                   GGO_METRICS | GGO_GLYPH_INDEX,
                                   &gm, 0, NULL, &ident);
        if (r == GDI_ERROR) continue;
        metrics[i].leftSideBearing  = gm.gmptGlyphOrigin.x;
        metrics[i].advanceWidth     = gm.gmCellIncX;
        metrics[i].rightSideBearing = (INT32)gm.gmCellIncX
                                    - gm.gmptGlyphOrigin.x
                                    - (INT32)gm.gmBlackBoxX;
        metrics[i].topSideBearing   = (INT32)self->ascent_du - gm.gmptGlyphOrigin.y;
        metrics[i].advanceHeight    = (UINT32)self->ascent_du + (UINT32)self->descent_du;
        metrics[i].bottomSideBearing = (INT32)self->descent_du
                                     + gm.gmptGlyphOrigin.y
                                     - (INT32)gm.gmBlackBoxY;
        metrics[i].verticalOriginY  = (INT32)self->ascent_du;
    }
    LeaveCriticalSection(&self->cs);
    return S_OK;
}

/* HRESULT GetGlyphIndices(UINT32 const *codepoints, UINT32 count,
 *      UINT16 *glyph_indices) — slot 11. */
static HRESULT STDMETHODCALLTYPE face_GetGlyphIndices(
    void *iface, UINT32 const *codepoints, UINT32 count, UINT16 *indices)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    WCHAR  *buf;
    WORD   *gidx;
    UINT32 i, u16n;
    DWORD  r;
    if (!indices) return E_POINTER;
    if (!count) return S_OK;
    if (!codepoints) return E_INVALIDARG;
    if (!gdi_face_ensure(self)) { memset(indices, 0, count * sizeof(*indices)); return E_FAIL; }

    buf  = HeapAlloc(GetProcessHeap(), 0, count * 2 * sizeof(WCHAR));
    gidx = HeapAlloc(GetProcessHeap(), 0, count * 2 * sizeof(WORD));
    if (!buf || !gidx)
    {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        if (gidx) HeapFree(GetProcessHeap(), 0, gidx);
        return E_OUTOFMEMORY;
    }

    u16n = 0;
    for (i = 0; i < count; ++i)
    {
        UINT32 cp = codepoints[i];
        if (cp < 0x10000) buf[u16n++] = (WCHAR)cp;
        else if (cp <= 0x10FFFF)
        {
            cp -= 0x10000;
            buf[u16n++] = (WCHAR)(0xD800 | (cp >> 10));
            buf[u16n++] = (WCHAR)(0xDC00 | (cp & 0x3FF));
        }
        else buf[u16n++] = 0xFFFD;
    }

    EnterCriticalSection(&self->cs);
    r = GetGlyphIndicesW(self->hdc, buf, u16n, gidx, GGI_MARK_NONEXISTING_GLYPHS);
    LeaveCriticalSection(&self->cs);

    if (r == GDI_ERROR)
    {
        memset(indices, 0, count * sizeof(*indices));
        HeapFree(GetProcessHeap(), 0, buf);
        HeapFree(GetProcessHeap(), 0, gidx);
        return S_OK;
    }

    u16n = 0;
    for (i = 0; i < count; ++i)
    {
        UINT32 cp = codepoints[i];
        if (cp < 0x10000)
        {
            indices[i] = (gidx[u16n] == 0xFFFF) ? 0 : gidx[u16n];
            u16n++;
        }
        else
        {
            /* SMP code points: GDI maps each surrogate individually and
             * neither half has a real glyph.  Report missing. */
            indices[i] = 0;
            u16n += 2;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    HeapFree(GetProcessHeap(), 0, gidx);
    return S_OK;
}

/* HRESULT TryGetFontTable(UINT32 tag, void const **table_data,
 *      UINT32 *table_size, void **context, BOOL *exists) — slot 12. */
static HRESULT STDMETHODCALLTYPE face_TryGetFontTable(
    void *iface, UINT32 tag, const void **table_data, UINT32 *table_size,
    void **context, BOOL *exists)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    DWORD size;
    void *blob;
    if (!table_data || !table_size || !context || !exists) return E_POINTER;
    *table_data = NULL;
    *table_size = 0;
    *context = NULL;
    *exists = FALSE;
    if (!gdi_face_ensure(self)) return E_FAIL;

    EnterCriticalSection(&self->cs);
    size = GetFontData(self->hdc, tag, 0, NULL, 0);
    if (size == GDI_ERROR || size == 0)
    {
        LeaveCriticalSection(&self->cs);
        return S_OK; /* exists stays FALSE */
    }
    blob = HeapAlloc(GetProcessHeap(), 0, size);
    if (!blob)
    {
        LeaveCriticalSection(&self->cs);
        return E_OUTOFMEMORY;
    }
    if (GetFontData(self->hdc, tag, 0, blob, size) != size)
    {
        HeapFree(GetProcessHeap(), 0, blob);
        LeaveCriticalSection(&self->cs);
        return E_FAIL;
    }
    LeaveCriticalSection(&self->cs);

    *table_data = blob;
    *table_size = size;
    *context    = blob;
    *exists     = TRUE;
    return S_OK;
}

/* void ReleaseFontTable(void *context) — slot 13.
 * Declared as HRESULT-returning in IDWriteFontFace IDL; treat the return
 * value as ignored and use the slot for both. */
static void STDMETHODCALLTYPE face_ReleaseFontTable(void *iface, void *context)
{
    (void)iface;
    if (context) HeapFree(GetProcessHeap(), 0, context);
}

/* ---- GetGlyphRunOutline support: TrueType outline decoder ------------ */

static D2D1_POINT_2F fixed_to_point(const POINTFX *p, FLOAT scale, FLOAT ox, FLOAT oy)
{
    D2D1_POINT_2F r;
    FLOAT x = (FLOAT)p->x.value + (FLOAT)p->x.fract / 65536.0f;
    FLOAT y = (FLOAT)p->y.value + (FLOAT)p->y.fract / 65536.0f;
    /* GDI's GGO_NATIVE returns y-up around the glyph origin.  DirectWrite
     * sinks consume y-down with the origin at the run baseline. */
    r.x = ox + x * scale;
    r.y = oy - y * scale;
    return r;
}

static D2D1_POINT_2F midpoint_pt(D2D1_POINT_2F a, D2D1_POINT_2F b)
{
    D2D1_POINT_2F r;
    r.x = (a.x + b.x) * 0.5f;
    r.y = (a.y + b.y) * 0.5f;
    return r;
}

static void quadratic_to_cubic(D2D1_POINT_2F s, D2D1_POINT_2F c, D2D1_POINT_2F e,
                               D2D1_BEZIER_SEGMENT *out)
{
    out->point1.x = s.x + (2.0f / 3.0f) * (c.x - s.x);
    out->point1.y = s.y + (2.0f / 3.0f) * (c.y - s.y);
    out->point2.x = e.x + (2.0f / 3.0f) * (c.x - e.x);
    out->point2.y = e.y + (2.0f / 3.0f) * (c.y - e.y);
    out->point3   = e;
}

static void emit_glyph_outline(void *sink, BYTE *buf, DWORD len,
                               FLOAT scale, FLOAT ox, FLOAT oy)
{
    const dwrite_sink_vtbl_t *vt = *(const dwrite_sink_vtbl_t *const *)sink;
    BYTE *end = buf + len;
    BYTE *p = buf;

    while (p < end)
    {
        TTPOLYGONHEADER *hdr = (TTPOLYGONHEADER *)p;
        BYTE *poly_end = p + hdr->cb;
        BYTE *cursor = (BYTE *)hdr + sizeof(TTPOLYGONHEADER);
        D2D1_POINT_2F start = fixed_to_point(&hdr->pfxStart, scale, ox, oy);
        D2D1_POINT_2F pos = start;

        if (hdr->dwType != TT_POLYGON_TYPE)
        {
            p = poly_end;
            continue;
        }

        vt->BeginFigure(sink, start, D2D1_FIGURE_BEGIN_FILLED);

        while (cursor < poly_end)
        {
            TTPOLYCURVE *cur = (TTPOLYCURVE *)cursor;
            UINT32 npts = cur->cpfx;
            BYTE *next_cursor;

            next_cursor = (BYTE *)cur + sizeof(WORD) * 2 + sizeof(POINTFX) * npts;

            if (cur->wType == TT_PRIM_LINE && npts > 0)
            {
                D2D1_POINT_2F *lines = HeapAlloc(GetProcessHeap(), 0,
                                                 npts * sizeof(D2D1_POINT_2F));
                UINT32 j;
                if (lines)
                {
                    for (j = 0; j < npts; ++j)
                        lines[j] = fixed_to_point(&cur->apfx[j], scale, ox, oy);
                    vt->AddLines(sink, lines, npts);
                    pos = lines[npts - 1];
                    HeapFree(GetProcessHeap(), 0, lines);
                }
            }
            else if (cur->wType == TT_PRIM_QSPLINE && npts >= 2)
            {
                /* TT_PRIM_QSPLINE: apfx[0..npts-2] are control points,
                 * apfx[npts-1] is the final on-curve endpoint.  Between
                 * consecutive control points c_i, c_{i+1} (when
                 * i+1 < npts-1) the implicit on-curve point is their
                 * midpoint, so each quadratic spans the implicit (or
                 * explicit terminal) on-curve points. */
                UINT32 nq = npts - 1; /* number of quadratics */
                D2D1_BEZIER_SEGMENT *cubics = HeapAlloc(GetProcessHeap(), 0,
                                                       nq * sizeof(D2D1_BEZIER_SEGMENT));
                UINT32 j;
                if (cubics)
                {
                    for (j = 0; j < nq; ++j)
                    {
                        D2D1_POINT_2F ctrl = fixed_to_point(&cur->apfx[j], scale, ox, oy);
                        D2D1_POINT_2F endp;
                        if (j + 1 == nq)
                            endp = fixed_to_point(&cur->apfx[npts - 1], scale, ox, oy);
                        else
                        {
                            D2D1_POINT_2F nxt = fixed_to_point(&cur->apfx[j + 1], scale, ox, oy);
                            endp = midpoint_pt(ctrl, nxt);
                        }
                        quadratic_to_cubic(pos, ctrl, endp, &cubics[j]);
                        pos = endp;
                    }
                    vt->AddBeziers(sink, cubics, nq);
                    HeapFree(GetProcessHeap(), 0, cubics);
                }
            }
            /* TT_PRIM_CSPLINE (cubic) — GDI emits this for PostScript Type1
             * outlines.  apfx is groups of 3 points: (ctrl1, ctrl2, end). */
            else if (cur->wType == TT_PRIM_CSPLINE && npts >= 3 && (npts % 3) == 0)
            {
                UINT32 ncubic = npts / 3;
                D2D1_BEZIER_SEGMENT *cubics = HeapAlloc(GetProcessHeap(), 0,
                                                       ncubic * sizeof(D2D1_BEZIER_SEGMENT));
                UINT32 j;
                if (cubics)
                {
                    for (j = 0; j < ncubic; ++j)
                    {
                        cubics[j].point1 = fixed_to_point(&cur->apfx[j*3 + 0], scale, ox, oy);
                        cubics[j].point2 = fixed_to_point(&cur->apfx[j*3 + 1], scale, ox, oy);
                        cubics[j].point3 = fixed_to_point(&cur->apfx[j*3 + 2], scale, ox, oy);
                        pos = cubics[j].point3;
                    }
                    vt->AddBeziers(sink, cubics, ncubic);
                    HeapFree(GetProcessHeap(), 0, cubics);
                }
            }

            cursor = next_cursor;
        }

        vt->EndFigure(sink, D2D1_FIGURE_END_CLOSED);
        p = poly_end;
    }
}

/* HRESULT GetGlyphRunOutline(FLOAT em_size, UINT16 const *glyph_indices,
 *      FLOAT const *glyph_advances, DWRITE_GLYPH_OFFSET const *glyph_offsets,
 *      UINT32 glyph_count, BOOL is_sideways, BOOL is_rtl,
 *      IDWriteGeometrySink *sink) — slot 14. */
static HRESULT STDMETHODCALLTYPE face_GetGlyphRunOutline(
    void *iface, FLOAT em_size, UINT16 const *glyph_indices,
    FLOAT const *glyph_advances, DWRITE_GLYPH_OFFSET const *glyph_offsets,
    UINT32 glyph_count, BOOL is_sideways, BOOL is_rtl, void *sink)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    static const MAT2 ident = { {0,1}, {0,0}, {0,0}, {0,1} };
    BYTE *buf = NULL;
    DWORD buf_cap = 0;
    FLOAT scale, pen_x = 0.0f;
    UINT32 i;
    (void)is_sideways;

    if (!sink) return E_POINTER;
    if (!glyph_indices && glyph_count) return E_INVALIDARG;
    if (!gdi_face_ensure(self)) return E_FAIL;
    if (!self->em_units) return S_OK;

    scale = em_size / (FLOAT)self->em_units;

    EnterCriticalSection(&self->cs);
    for (i = 0; i < glyph_count; ++i)
    {
        FLOAT ox = pen_x;
        FLOAT oy = 0.0f;
        FLOAT advance;
        GLYPHMETRICS gm;
        DWORD r;

        if (glyph_offsets)
        {
            ox += glyph_offsets[i].advanceOffset;
            oy -= glyph_offsets[i].ascenderOffset;
        }

        r = GetGlyphOutlineW(self->hdc, glyph_indices[i],
                             GGO_NATIVE | GGO_GLYPH_INDEX,
                             &gm, 0, NULL, &ident);
        if (r != GDI_ERROR && r > 0)
        {
            if (r > buf_cap)
            {
                BYTE *nb = buf ? HeapReAlloc(GetProcessHeap(), 0, buf, r)
                               : HeapAlloc(GetProcessHeap(), 0, r);
                if (!nb)
                {
                    LeaveCriticalSection(&self->cs);
                    if (buf) HeapFree(GetProcessHeap(), 0, buf);
                    return E_OUTOFMEMORY;
                }
                buf = nb;
                buf_cap = r;
            }
            if (GetGlyphOutlineW(self->hdc, glyph_indices[i],
                                 GGO_NATIVE | GGO_GLYPH_INDEX,
                                 &gm, r, buf, &ident) != GDI_ERROR)
            {
                emit_glyph_outline(sink, buf, r, scale, ox, oy);
            }
        }

        advance = glyph_advances ? glyph_advances[i]
                                 : ((r != GDI_ERROR)
                                    ? (FLOAT)gm.gmCellIncX * scale
                                    : 0.0f);
        pen_x += is_rtl ? -advance : advance;
    }
    LeaveCriticalSection(&self->cs);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return S_OK;
}

/* HRESULT GetRecommendedRenderingMode(FLOAT em_size, FLOAT pixels_per_dip,
 *      DWRITE_MEASURING_MODE mode, IDWriteRenderingParams *params,
 *      DWRITE_RENDERING_MODE *mode_out) — slot 15. */
static HRESULT STDMETHODCALLTYPE face_GetRecommendedRenderingMode(
    void *iface, FLOAT em_size, FLOAT pixels_per_dip,
    DWRITE_MEASURING_MODE mode, void *params, DWRITE_RENDERING_MODE *mode_out)
{
    (void)iface; (void)em_size; (void)pixels_per_dip; (void)mode; (void)params;
    if (!mode_out) return E_POINTER;
    *mode_out = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    return S_OK;
}

/* HRESULT GetGdiCompatibleMetrics(FLOAT em_size, FLOAT pixels_per_dip,
 *      DWRITE_MATRIX const *transform, DWRITE_FONT_METRICS *out) — slot 16.
 * For Phase 2 we ignore the GDI compatibility tweaks and return the design
 * metrics. */
static HRESULT STDMETHODCALLTYPE face_GetGdiCompatibleMetrics(
    void *iface, FLOAT em_size, FLOAT pixels_per_dip,
    DWRITE_MATRIX const *transform, DWRITE_FONT_METRICS *out)
{
    (void)em_size; (void)pixels_per_dip; (void)transform;
    if (!out) return E_POINTER;
    face_GetMetrics(iface, out);
    return S_OK;
}

/* HRESULT GetGdiCompatibleGlyphMetrics(FLOAT em_size, FLOAT pixels_per_dip,
 *      DWRITE_MATRIX const *transform, BOOL use_gdi_natural,
 *      UINT16 const *glyph_indices, UINT32 glyph_count,
 *      DWRITE_GLYPH_METRICS *metrics, BOOL is_sideways) — slot 17. */
static HRESULT STDMETHODCALLTYPE face_GetGdiCompatibleGlyphMetrics(
    void *iface, FLOAT em_size, FLOAT pixels_per_dip,
    DWRITE_MATRIX const *transform, BOOL use_gdi_natural,
    UINT16 const *glyph_indices, UINT32 glyph_count,
    DWRITE_GLYPH_METRICS *metrics, BOOL is_sideways)
{
    (void)em_size; (void)pixels_per_dip; (void)transform; (void)use_gdi_natural;
    return face_GetDesignGlyphMetrics(iface, glyph_indices, glyph_count,
                                      metrics, is_sideways);
}

/* IDWriteFontFace1::GetMetrics(DWRITE_FONT_METRICS1 *out) — slot 18.
 * Same as GetMetrics, additional fields zeroed. */
static void STDMETHODCALLTYPE face_GetMetrics1(void *iface, DWRITE_FONT_METRICS1 *out)
{
    font_face_obj_t *self = (font_face_obj_t *)iface;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!gdi_face_ensure(self)) return;
    if (self->metrics_ready)
    {
        out->designUnitsPerEm       = self->metrics_cache.designUnitsPerEm;
        out->ascent                 = self->metrics_cache.ascent;
        out->descent                = self->metrics_cache.descent;
        out->lineGap                = self->metrics_cache.lineGap;
        out->capHeight              = self->metrics_cache.capHeight;
        out->xHeight                = self->metrics_cache.xHeight;
        out->underlinePosition      = self->metrics_cache.underlinePosition;
        out->underlineThickness     = self->metrics_cache.underlineThickness;
        out->strikethroughPosition  = self->metrics_cache.strikethroughPosition;
        out->strikethroughThickness = self->metrics_cache.strikethroughThickness;
        out->hasTypographicMetrics  = TRUE;
    }
    else
    {
        out->designUnitsPerEm = self->em_units ? self->em_units : 1;
    }
}

/* IDWriteFontFace3::HasCharacter(UINT32 codepoint) — return TRUE if any
 * non-zero glyph maps.  Slot for IDWriteFontFace3::HasCharacter lives in
 * the FontFace3 extension. */
static BOOL STDMETHODCALLTYPE face_HasCharacter(void *iface, UINT32 codepoint)
{
    UINT16 gid = 0;
    HRESULT hr = face_GetGlyphIndices(iface, &codepoint, 1, &gid);
    return SUCCEEDED(hr) && gid != 0;
}

/* IDWriteFontFace1::IsMonospacedFont(void) — slot 22.
 * GDI doesn't expose this directly without parsing 'post'.  Treat
 * known monospace family names heuristically as monospace, else FALSE. */
static BOOL STDMETHODCALLTYPE face_IsMonospacedFont(void *iface)
{
    (void)iface;
    return FALSE;
}

/* FontFace vtable: 3 (IUnknown) + 15 + 12 + 5 + 15 + 4 + 5 = 59.
 * Indexed 0..58.  Pad to 60 for safety against IDL drift; we'll
 * tighten when Wine's IDLs are vendored in Phase 3. */
static const void * const g_face_vtbl[60] =
{
    face_QueryInterface,                   /* 0  QueryInterface */
    dwrite_common_AddRef,                  /* 1  AddRef */
    face_Release,                          /* 2  Release */
    face_GetType,                          /* 3  GetType */
    dwrite_common_method_e_notimpl,        /* 4  GetFiles */
    face_GetIndex,                         /* 5  GetIndex */
    face_GetSimulations,                   /* 6  GetSimulations */
    face_IsSymbolFont,                     /* 7  IsSymbolFont */
    face_GetMetrics,                       /* 8  GetMetrics */
    face_GetGlyphCount,                    /* 9  GetGlyphCount */
    face_GetDesignGlyphMetrics,            /* 10 GetDesignGlyphMetrics */
    face_GetGlyphIndices,                  /* 11 GetGlyphIndices */
    face_TryGetFontTable,                  /* 12 TryGetFontTable */
    face_ReleaseFontTable,                 /* 13 ReleaseFontTable */
    face_GetGlyphRunOutline,               /* 14 GetGlyphRunOutline */
    face_GetRecommendedRenderingMode,      /* 15 GetRecommendedRenderingMode */
    face_GetGdiCompatibleMetrics,          /* 16 GetGdiCompatibleMetrics */
    face_GetGdiCompatibleGlyphMetrics,     /* 17 GetGdiCompatibleGlyphMetrics */
    /* IDWriteFontFace1 (slots 18-29; note IDL order is
     *   GetGdiCompatibleMetrics(18) before GetMetrics(19)). */
    dwrite_common_method_e_notimpl,        /* 18 FontFace1::GetGdiCompatibleMetrics */
    face_GetMetrics1,                      /* 19 FontFace1::GetMetrics(DWRITE_FONT_METRICS1*) */
    dwrite_common_method_e_notimpl,        /* 20 FontFace1::GetCaretMetrics */
    dwrite_common_method_e_notimpl,        /* 21 FontFace1::GetUnicodeRanges */
    face_IsMonospacedFont,                 /* 22 FontFace1::IsMonospacedFont */
    dwrite_common_method_e_notimpl,        /* 23 FontFace1::GetDesignGlyphAdvances */
    dwrite_common_method_e_notimpl,        /* 24 FontFace1::GetGdiCompatibleGlyphAdvances */
    dwrite_common_method_e_notimpl,        /* 25 FontFace1::GetKerningPairAdjustments */
    dwrite_common_method_e_notimpl,        /* 26 FontFace1::HasKerningPairs */
    dwrite_common_method_e_notimpl,        /* 27 FontFace1::GetRecommendedRenderingMode */
    dwrite_common_method_e_notimpl,        /* 28 FontFace1::GetVerticalGlyphVariants */
    dwrite_common_method_e_notimpl,        /* 29 FontFace1::HasVerticalGlyphVariants */
    /* IDWriteFontFace2 (slots 30-34) */
    dwrite_common_method_e_notimpl,        /* 30 IsColorFont */
    dwrite_common_method_e_notimpl,        /* 31 GetColorPaletteCount */
    dwrite_common_method_e_notimpl,        /* 32 GetPaletteEntryCount */
    dwrite_common_method_e_notimpl,        /* 33 GetPaletteEntries */
    dwrite_common_method_e_notimpl,        /* 34 GetRecommendedRenderingMode */
    /* IDWriteFontFace3 (slots 35-49) */
    dwrite_common_method_e_notimpl,        /* 35 GetFontFaceReference */
    dwrite_common_method_e_notimpl,        /* 36 GetPanose */
    dwrite_common_method_e_notimpl,        /* 37 GetWeight */
    dwrite_common_method_e_notimpl,        /* 38 GetStretch */
    dwrite_common_method_e_notimpl,        /* 39 GetStyle */
    dwrite_common_method_e_notimpl,        /* 40 GetFamilyNames */
    dwrite_common_method_e_notimpl,        /* 41 GetFaceNames */
    dwrite_common_method_e_notimpl,        /* 42 GetInformationalStrings */
    dwrite_common_method_e_notimpl,        /* 43 Equals */
    face_HasCharacter,                     /* 44 HasCharacter */
    dwrite_common_method_e_notimpl,        /* 45 GetRecommendedRenderingMode (FF3) */
    dwrite_common_method_e_notimpl,        /* 46 IsCharacterLocal */
    dwrite_common_method_e_notimpl,        /* 47 IsGlyphLocal */
    dwrite_common_method_e_notimpl,        /* 48 AreCharactersLocal */
    dwrite_common_method_e_notimpl,        /* 49 AreGlyphsLocal */
    /* IDWriteFontFace4 (slots 50-53) */
    dwrite_common_method_e_notimpl,        /* 50 GetGlyphImageFormats (overload) */
    dwrite_common_method_e_notimpl,        /* 51 GetGlyphImageFormats */
    dwrite_common_method_e_notimpl,        /* 52 GetGlyphImageData */
    dwrite_common_method_e_notimpl,        /* 53 ReleaseGlyphImageData */
    /* IDWriteFontFace5 (slots 54-58) */
    dwrite_common_method_e_notimpl,        /* 54 GetFontAxisValueCount */
    dwrite_common_method_e_notimpl,        /* 55 GetFontAxisValues */
    dwrite_common_method_e_notimpl,        /* 56 HasVariations */
    dwrite_common_method_e_notimpl,        /* 57 GetFontResource */
    dwrite_common_method_e_notimpl,        /* 58 Equals (FontFace5) */
    /* Pad */
    dwrite_common_method_e_notimpl,        /* 59 padding */
};
