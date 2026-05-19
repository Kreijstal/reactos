/*
 * PROJECT:     ReactOS DirectWrite
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     IDWriteGlyphRunAnalysis backed by GetGlyphOutlineW
 *              (GGO_GRAY8_BITMAP).
 *
 * Qt 6's QFontEngineDirectWrite::imageForGlyph rasterizes each glyph
 * through IDWriteFactory::CreateGlyphRunAnalysis +
 * IDWriteGlyphRunAnalysis::GetAlphaTextureBounds + CreateAlphaTexture.
 * Without this path Qt logs "CreateGlyphRunAnalysis failed (0x80004001)"
 * and the QImage it returns is empty — every text-rendering call ends
 * up drawing nothing, leaving the dialog body blank even when the font
 * collection populates correctly.
 *
 * We rasterize each glyph via GDI at the requested em pixel size and
 * composite the resulting 6-bit alpha bitmap into the output texture
 * at the per-glyph pen origin.  Both ALIASED_1x1 (8 bpp grayscale) and
 * CLEARTYPE_3x1 (24 bpp RGB) are supported; for CLEARTYPE we triplicate
 * the grayscale value into the R/G/B subpixel triple — Qt averages the
 * three channels back into a single alpha value, so the visual result
 * matches grayscale.
 */

#include <stdarg.h>

#include "dwrite_private.h"

#include <wine/debug.h>
WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

/* IDWriteGlyphRunAnalysis IID from Wine's dwrite.idl. */
static const GUID g_iid_IDWriteGlyphRunAnalysis =
    {0x7d97dbf7, 0xe085, 0x42d4, {0x81, 0xe3, 0x6a, 0x88, 0x3b, 0xde, 0xd1, 0x18}};

typedef struct glyph_data
{
    INT  origin_x;  /* pixel origin of black box, top-left, x */
    INT  origin_y;  /* pixel origin of black box, top-left, y */
    UINT bbx;       /* black-box width */
    UINT bby;       /* black-box height */
    BYTE *bits;     /* gray8 data, padded rows to 4 bytes, 0..64 */
    UINT  stride;   /* row stride in bytes */
} glyph_data_t;

typedef struct glyph_run_analysis
{
    const void *lpVtbl;
    LONG ref;
    /* Rendering setup. */
    HDC    hdc;
    HFONT  hfont;
    HGDIOBJ prev_obj;
    UINT32 glyph_count;
    glyph_data_t *glyphs;
    /* Computed bounds — union of all glyph black-boxes. */
    BOOL    bounds_ready;
    RECT    bounds;
    BOOL    empty;
} glyph_run_analysis_t;

static const void *const g_gra_vtbl[6];

ULONG STDMETHODCALLTYPE dwrite_common_AddRef(void *iface);
ULONG STDMETHODCALLTYPE dwrite_common_Release(void *iface);

extern const GUID g_iid_IUnknown;

static HRESULT STDMETHODCALLTYPE gra_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (dwrite_iid_equals(iid, &g_iid_IUnknown) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteGlyphRunAnalysis))
    {
        *out = iface;
        dwrite_common_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE gra_Release(void *iface)
{
    glyph_run_analysis_t *self = (glyph_run_analysis_t *)iface;
    LONG r = InterlockedDecrement(&self->ref);
    if (r == 0)
    {
        UINT32 i;
        if (self->glyphs)
        {
            for (i = 0; i < self->glyph_count; ++i)
            {
                if (self->glyphs[i].bits)
                    HeapFree(GetProcessHeap(), 0, self->glyphs[i].bits);
            }
            HeapFree(GetProcessHeap(), 0, self->glyphs);
        }
        if (self->hdc)
        {
            if (self->prev_obj) SelectObject(self->hdc, self->prev_obj);
            DeleteDC(self->hdc);
        }
        if (self->hfont) DeleteObject(self->hfont);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return r < 0 ? 0 : (ULONG)r;
}

static void union_rect_with_glyph(RECT *r, glyph_data_t *g, BOOL *empty)
{
    LONG l, t, rt, b;
    if (g->bbx == 0 || g->bby == 0) return;
    l  = g->origin_x;
    t  = g->origin_y;
    rt = g->origin_x + (LONG)g->bbx;
    b  = g->origin_y + (LONG)g->bby;
    if (*empty)
    {
        r->left = l; r->top = t; r->right = rt; r->bottom = b;
        *empty = FALSE;
    }
    else
    {
        if (l  < r->left)   r->left   = l;
        if (t  < r->top)    r->top    = t;
        if (rt > r->right)  r->right  = rt;
        if (b  > r->bottom) r->bottom = b;
    }
}

static HRESULT STDMETHODCALLTYPE gra_GetAlphaTextureBounds(
    void *iface, DWRITE_TEXTURE_TYPE type, RECT *out)
{
    glyph_run_analysis_t *self = (glyph_run_analysis_t *)iface;
    if (!out) return E_POINTER;
    (void)type;
    if (!self->bounds_ready)
    {
        UINT32 i;
        BOOL empty = TRUE;
        SetRectEmpty(&self->bounds);
        for (i = 0; i < self->glyph_count; ++i)
            union_rect_with_glyph(&self->bounds, &self->glyphs[i], &empty);
        self->bounds_ready = TRUE;
        self->empty = empty;
    }
    if (self->empty)
    {
        SetRectEmpty(out);
        return S_OK;
    }
    *out = self->bounds;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE gra_CreateAlphaTexture(
    void *iface, DWRITE_TEXTURE_TYPE type, RECT const *rect,
    BYTE *buf, UINT32 buf_size)
{
    glyph_run_analysis_t *self = (glyph_run_analysis_t *)iface;
    UINT32 i;
    LONG w, h;
    UINT32 bpp;
    UINT32 expected;

    if (!rect || !buf) return E_POINTER;
    w = rect->right - rect->left;
    h = rect->bottom - rect->top;
    if (w <= 0 || h <= 0) return E_INVALIDARG;
    bpp = (type == DWRITE_TEXTURE_CLEARTYPE_3x1) ? 3 : 1;
    expected = (UINT32)w * (UINT32)h * bpp;
    if (buf_size < expected) return E_NOT_SUFFICIENT_BUFFER;
    memset(buf, 0, expected);

    for (i = 0; i < self->glyph_count; ++i)
    {
        glyph_data_t *g = &self->glyphs[i];
        UINT y, x;
        if (!g->bits || g->bbx == 0 || g->bby == 0) continue;
        for (y = 0; y < g->bby; ++y)
        {
            LONG dst_y = g->origin_y + (LONG)y - rect->top;
            const BYTE *src_row;
            BYTE *dst_row;
            if (dst_y < 0 || dst_y >= h) continue;
            src_row = g->bits + (size_t)y * g->stride;
            dst_row = buf + (size_t)dst_y * (size_t)w * bpp;
            for (x = 0; x < g->bbx; ++x)
            {
                LONG dst_x = g->origin_x + (LONG)x - rect->left;
                BYTE v;
                if (dst_x < 0 || dst_x >= w) continue;
                v = src_row[x];
                /* GGO_GRAY8_BITMAP gives 0..64.  Scale to 0..255. */
                if (v > 64) v = 64;
                v = (BYTE)((v * 255 + 32) / 64);
                if (bpp == 1)
                {
                    BYTE cur = dst_row[dst_x];
                    if (v > cur) dst_row[dst_x] = v;
                }
                else
                {
                    BYTE *p = dst_row + (size_t)dst_x * 3;
                    if (v > p[0]) p[0] = v;
                    if (v > p[1]) p[1] = v;
                    if (v > p[2]) p[2] = v;
                }
            }
        }
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE gra_GetAlphaBlendParams(
    void *iface, void *params, FLOAT *gamma, FLOAT *contrast,
    FLOAT *cleartype_level)
{
    (void)iface; (void)params;
    if (gamma)            *gamma = 1.8f;
    if (contrast)         *contrast = 0.5f;
    if (cleartype_level)  *cleartype_level = 1.0f;
    return S_OK;
}

static const void *const g_gra_vtbl[6] =
{
    gra_QueryInterface,         /* 0 QueryInterface */
    dwrite_common_AddRef,       /* 1 AddRef */
    gra_Release,                /* 2 Release */
    gra_GetAlphaTextureBounds,  /* 3 GetAlphaTextureBounds */
    gra_CreateAlphaTexture,     /* 4 CreateAlphaTexture */
    gra_GetAlphaBlendParams,    /* 5 GetAlphaBlendParams */
};

/* Build the glyph rasterization cache from a glyph run.  We render each
 * glyph once into its own gray8 buffer and remember the pixel origin
 * relative to (baseline_x, baseline_y).  Caller advances the pen
 * along advances + offsets the same way DirectWrite would. */
static HRESULT rasterize_run(glyph_run_analysis_t *self,
                             const DWRITE_GLYPH_RUN *run,
                             FLOAT ppd,
                             FLOAT baseline_x, FLOAT baseline_y)
{
    static const MAT2 ident = { {0,1}, {0,0}, {0,0}, {0,1} };
    LOGFONTW lf;
    HRESULT hr;
    UINT16 em_units;
    INT em_pixels;
    FLOAT pen_x, pen_y;
    UINT32 i;

    if (!run->fontFace || run->glyphCount == 0) return S_OK;

    em_units = dwrite_face_get_em_units(run->fontFace);
    if (!em_units) em_units = 2048;
    em_pixels = (INT)(run->fontEmSize * ppd + 0.5f);
    if (em_pixels < 1) em_pixels = 1;

    hr = dwrite_face_get_logfont(run->fontFace, &lf);
    if (FAILED(hr)) return hr;
    lf.lfHeight = -em_pixels;

    self->hdc = CreateCompatibleDC(NULL);
    if (!self->hdc) return E_FAIL;
    self->hfont = CreateFontIndirectW(&lf);
    if (!self->hfont)
    {
        DeleteDC(self->hdc);
        self->hdc = NULL;
        return E_FAIL;
    }
    self->prev_obj = SelectObject(self->hdc, self->hfont);

    self->glyph_count = run->glyphCount;
    self->glyphs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                             run->glyphCount * sizeof(glyph_data_t));
    if (!self->glyphs) return E_OUTOFMEMORY;

    pen_x = baseline_x;
    pen_y = baseline_y;
    for (i = 0; i < run->glyphCount; ++i)
    {
        GLYPHMETRICS gm;
        DWORD bsize;
        FLOAT ox = pen_x;
        FLOAT oy = pen_y;

        if (run->glyphOffsets)
        {
            ox += run->glyphOffsets[i].advanceOffset;
            oy -= run->glyphOffsets[i].ascenderOffset;
        }

        bsize = GetGlyphOutlineW(self->hdc, run->glyphIndices[i],
                                 GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX,
                                 &gm, 0, NULL, &ident);
        if (bsize != GDI_ERROR && bsize > 0)
        {
            BYTE *bits = HeapAlloc(GetProcessHeap(), 0, bsize);
            if (bits)
            {
                DWORD got = GetGlyphOutlineW(self->hdc, run->glyphIndices[i],
                                             GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX,
                                             &gm, bsize, bits, &ident);
                if (got != GDI_ERROR)
                {
                    self->glyphs[i].bits   = bits;
                    self->glyphs[i].bbx    = gm.gmBlackBoxX;
                    self->glyphs[i].bby    = gm.gmBlackBoxY;
                    self->glyphs[i].stride = (gm.gmBlackBoxX + 3) & ~3u;
                    /* GDI's gmptGlyphOrigin is in y-up coordinates relative
                     * to the baseline.  Origin.x = left bearing (pixels).
                     * Origin.y = ascent above baseline. */
                    self->glyphs[i].origin_x = (INT)(ox + (FLOAT)gm.gmptGlyphOrigin.x + 0.5f);
                    self->glyphs[i].origin_y = (INT)(oy - (FLOAT)gm.gmptGlyphOrigin.y + 0.5f);
                }
                else
                {
                    HeapFree(GetProcessHeap(), 0, bits);
                }
            }
        }

        {
            FLOAT advance;
            if (run->glyphAdvances)
                advance = run->glyphAdvances[i];
            else
                advance = (bsize != GDI_ERROR) ? (FLOAT)gm.gmCellIncX : 0.0f;
            if (run->bidiLevel & 1) pen_x -= advance;
            else                    pen_x += advance;
        }
    }
    return S_OK;
}

HRESULT dwrite_create_glyph_run_analysis(
    DWRITE_GLYPH_RUN const *run, FLOAT ppd,
    DWRITE_MATRIX const *transform,
    DWRITE_RENDERING_MODE rendering_mode,
    DWRITE_MEASURING_MODE measuring_mode,
    FLOAT baseline_x, FLOAT baseline_y, void **out)
{
    glyph_run_analysis_t *self;
    HRESULT hr;
    (void)rendering_mode; (void)measuring_mode;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!run) return E_INVALIDARG;

    self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self) return E_OUTOFMEMORY;
    self->lpVtbl = g_gra_vtbl;
    self->ref = 1;

    /* If a transform is given, apply only its translation to the
     * baseline.  Phase 2.5 doesn't shear/scale glyphs; Qt's typical
     * imageForGlyph call passes a translate-only transform anyway. */
    if (transform)
    {
        baseline_x += transform->dx;
        baseline_y += transform->dy;
    }

    hr = rasterize_run(self, run, ppd, baseline_x, baseline_y);
    if (FAILED(hr))
    {
        gra_Release(self);
        return hr;
    }

    *out = self;
    return S_OK;
}
