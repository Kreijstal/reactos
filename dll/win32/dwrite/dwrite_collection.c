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
    font_family_obj_t *family;       /* Weak — family owns us. */
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
};

/* Forward vtable declarations. */
static const void * const g_collection_vtbl[14];
static const void * const g_family_vtbl[14];
static const void * const g_font_vtbl[24];
static const void * const g_face_vtbl[58];

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
    fam->fonts[fam->nfonts].family = fam;
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
    if (!out) return E_POINTER;
    *out = self->family;
    dwrite_common_AddRef(self->family);
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

/* ----- FontFace (Phase 1: minimal) ------------------------------------ */

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
    (void)iface;
    return FALSE;
}

/* UINT16 GetGlyphCount(void) — Phase 1: 0. */
static UINT16 STDMETHODCALLTYPE face_GetGlyphCount(void *iface)
{
    (void)iface;
    return 0;
}

/* The FontFace vtable size = 3 (IUnknown) + 15 + 12 + 5 + 14 + 4 + 5 = 58.
 * Slots beyond what we implement return E_NOTIMPL.  Qt's Phase-1 path
 * does not invoke them. */
static const void * const g_face_vtbl[58] =
{
    face_QueryInterface,                   /* 0  QueryInterface */
    dwrite_common_AddRef,                  /* 1  AddRef */
    face_Release,                          /* 2  Release */
    face_GetType,                          /* 3  GetType */
    dwrite_common_method_e_notimpl,        /* 4  GetFiles */
    face_GetIndex,                         /* 5  GetIndex */
    face_GetSimulations,                   /* 6  GetSimulations */
    face_IsSymbolFont,                     /* 7  IsSymbolFont */
    dwrite_common_method_e_notimpl,        /* 8  GetMetrics */
    face_GetGlyphCount,                    /* 9  GetGlyphCount (returns 0 in Phase 1) */
    /* 10..57: rest of FontFace0..5 extensions */
    dwrite_common_method_e_notimpl,        /* 10 GetDesignGlyphMetrics */
    dwrite_common_method_e_notimpl,        /* 11 GetGlyphIndices */
    dwrite_common_method_e_notimpl,        /* 12 TryGetFontTable */
    dwrite_common_method_e_notimpl,        /* 13 ReleaseFontTable (return value ignored) */
    dwrite_common_method_e_notimpl,        /* 14 GetGlyphRunOutline */
    dwrite_common_method_e_notimpl,        /* 15 GetRecommendedRenderingMode */
    dwrite_common_method_e_notimpl,        /* 16 GetGdiCompatibleMetrics */
    dwrite_common_method_e_notimpl,        /* 17 GetGdiCompatibleGlyphMetrics */
    /* FontFace1..5: tail E_NOTIMPL */
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
    dwrite_common_method_e_notimpl, dwrite_common_method_e_notimpl,
};
