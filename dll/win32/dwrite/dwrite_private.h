/*
 * PROJECT:     ReactOS DirectWrite
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Internal declarations shared across the dwrite stub units.
 */

#ifndef DWRITE_PRIVATE_H
#define DWRITE_PRIVATE_H

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <wingdi.h>

/* ---------------------------------------------------------------------
 * Minimal DirectWrite typedefs used in Phase 1.
 *
 * ReactOS' SDK does not ship dwrite.h yet (vendoring it from Wine is on
 * the Phase 2/3 list).  For Phase 1 we only need the handful of enum
 * constants used to fill stub return values, so declare them inline
 * with the same numeric values as the Microsoft / Wine headers.
 * ------------------------------------------------------------------- */

typedef enum DWRITE_FONT_FAMILY_MODEL
{
    DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC = 0,
    DWRITE_FONT_FAMILY_MODEL_WEIGHT_STRETCH_STYLE = 1,
} DWRITE_FONT_FAMILY_MODEL;

typedef enum DWRITE_FONT_WEIGHT
{
    DWRITE_FONT_WEIGHT_NORMAL = 400,
} DWRITE_FONT_WEIGHT;

typedef enum DWRITE_FONT_STRETCH
{
    DWRITE_FONT_STRETCH_NORMAL = 5,
} DWRITE_FONT_STRETCH;

typedef enum DWRITE_FONT_STYLE
{
    DWRITE_FONT_STYLE_NORMAL = 0,
    DWRITE_FONT_STYLE_OBLIQUE = 1,
    DWRITE_FONT_STYLE_ITALIC = 2,
} DWRITE_FONT_STYLE;

typedef enum DWRITE_FONT_FACE_TYPE
{
    DWRITE_FONT_FACE_TYPE_CFF = 0,
    DWRITE_FONT_FACE_TYPE_TRUETYPE = 1,
    DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION = 2,
    DWRITE_FONT_FACE_TYPE_TYPE1 = 3,
    DWRITE_FONT_FACE_TYPE_VECTOR = 4,
    DWRITE_FONT_FACE_TYPE_BITMAP = 5,
    DWRITE_FONT_FACE_TYPE_UNKNOWN = 6,
    DWRITE_FONT_FACE_TYPE_RAW_CFF = 7,
} DWRITE_FONT_FACE_TYPE;

typedef enum DWRITE_FONT_SIMULATIONS
{
    DWRITE_FONT_SIMULATIONS_NONE = 0x0,
    DWRITE_FONT_SIMULATIONS_BOLD = 0x1,
    DWRITE_FONT_SIMULATIONS_OBLIQUE = 0x2,
} DWRITE_FONT_SIMULATIONS;

typedef enum DWRITE_LOCALITY
{
    DWRITE_LOCALITY_REMOTE = 0,
    DWRITE_LOCALITY_PARTIAL = 1,
    DWRITE_LOCALITY_LOCAL = 2,
} DWRITE_LOCALITY;

typedef enum DWRITE_MEASURING_MODE
{
    DWRITE_MEASURING_MODE_NATURAL = 0,
    DWRITE_MEASURING_MODE_GDI_CLASSIC = 1,
    DWRITE_MEASURING_MODE_GDI_NATURAL = 2,
} DWRITE_MEASURING_MODE;

typedef enum DWRITE_RENDERING_MODE
{
    DWRITE_RENDERING_MODE_DEFAULT = 0,
    DWRITE_RENDERING_MODE_ALIASED = 1,
    DWRITE_RENDERING_MODE_GDI_CLASSIC = 2,
    DWRITE_RENDERING_MODE_GDI_NATURAL = 3,
    DWRITE_RENDERING_MODE_NATURAL = 4,
    DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC = 5,
    DWRITE_RENDERING_MODE_OUTLINE = 6,
} DWRITE_RENDERING_MODE;

typedef struct DWRITE_FONT_METRICS
{
    UINT16 designUnitsPerEm;
    UINT16 ascent;
    UINT16 descent;
    INT16  lineGap;
    UINT16 capHeight;
    UINT16 xHeight;
    INT16  underlinePosition;
    UINT16 underlineThickness;
    INT16  strikethroughPosition;
    UINT16 strikethroughThickness;
} DWRITE_FONT_METRICS;

typedef struct DWRITE_FONT_METRICS1
{
    UINT16 designUnitsPerEm;
    UINT16 ascent;
    UINT16 descent;
    INT16  lineGap;
    UINT16 capHeight;
    UINT16 xHeight;
    INT16  underlinePosition;
    UINT16 underlineThickness;
    INT16  strikethroughPosition;
    UINT16 strikethroughThickness;
    INT16  glyphBoxLeft;
    INT16  glyphBoxTop;
    INT16  glyphBoxRight;
    INT16  glyphBoxBottom;
    INT16  subscriptPositionX;
    INT16  subscriptPositionY;
    INT16  subscriptSizeX;
    INT16  subscriptSizeY;
    INT16  superscriptPositionX;
    INT16  superscriptPositionY;
    INT16  superscriptSizeX;
    INT16  superscriptSizeY;
    BOOL   hasTypographicMetrics;
} DWRITE_FONT_METRICS1;

typedef struct DWRITE_GLYPH_METRICS
{
    INT32  leftSideBearing;
    UINT32 advanceWidth;
    INT32  rightSideBearing;
    INT32  topSideBearing;
    UINT32 advanceHeight;
    INT32  bottomSideBearing;
    INT32  verticalOriginY;
} DWRITE_GLYPH_METRICS;

typedef struct DWRITE_GLYPH_OFFSET
{
    FLOAT advanceOffset;
    FLOAT ascenderOffset;
} DWRITE_GLYPH_OFFSET;

typedef struct DWRITE_MATRIX
{
    FLOAT m11;
    FLOAT m12;
    FLOAT m21;
    FLOAT m22;
    FLOAT dx;
    FLOAT dy;
} DWRITE_MATRIX;

/* ---------- D2D types needed for IDWriteGeometrySink ----------------- */

typedef struct D2D1_POINT_2F
{
    FLOAT x;
    FLOAT y;
} D2D1_POINT_2F;

typedef struct D2D1_BEZIER_SEGMENT
{
    D2D1_POINT_2F point1;
    D2D1_POINT_2F point2;
    D2D1_POINT_2F point3;
} D2D1_BEZIER_SEGMENT;

typedef enum D2D1_FIGURE_BEGIN
{
    D2D1_FIGURE_BEGIN_FILLED = 0,
    D2D1_FIGURE_BEGIN_HOLLOW = 1,
} D2D1_FIGURE_BEGIN;

typedef enum D2D1_FIGURE_END
{
    D2D1_FIGURE_END_OPEN = 0,
    D2D1_FIGURE_END_CLOSED = 1,
} D2D1_FIGURE_END;

typedef enum D2D1_FILL_MODE
{
    D2D1_FILL_MODE_ALTERNATE = 0,
    D2D1_FILL_MODE_WINDING = 1,
} D2D1_FILL_MODE;

typedef enum D2D1_PATH_SEGMENT
{
    D2D1_PATH_SEGMENT_NONE = 0x0,
    D2D1_PATH_SEGMENT_FORCE_UNSTROKED = 0x1,
    D2D1_PATH_SEGMENT_FORCE_ROUND_LINE_JOIN = 0x2,
} D2D1_PATH_SEGMENT;

/* IDWriteGeometrySink == ID2D1SimplifiedGeometrySink — 10 vtable slots.
 * Phase 2 GetGlyphRunOutline calls slots 5..9 to emit each glyph as a
 * filled, closed figure consisting of line segments and cubic Beziers
 * (quadratics from TrueType outlines are converted to cubics). */
typedef struct dwrite_sink_vtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG   (STDMETHODCALLTYPE *Release)(void *);
    void    (STDMETHODCALLTYPE *SetFillMode)(void *, D2D1_FILL_MODE);
    void    (STDMETHODCALLTYPE *SetSegmentFlags)(void *, D2D1_PATH_SEGMENT);
    void    (STDMETHODCALLTYPE *BeginFigure)(void *, D2D1_POINT_2F, D2D1_FIGURE_BEGIN);
    void    (STDMETHODCALLTYPE *AddLines)(void *, const D2D1_POINT_2F *, UINT32);
    void    (STDMETHODCALLTYPE *AddBeziers)(void *, const D2D1_BEZIER_SEGMENT *, UINT32);
    void    (STDMETHODCALLTYPE *EndFigure)(void *, D2D1_FIGURE_END);
    HRESULT (STDMETHODCALLTYPE *Close)(void *);
} dwrite_sink_vtbl_t;

#ifndef E_NOT_SUFFICIENT_BUFFER
#define E_NOT_SUFFICIENT_BUFFER ((HRESULT)0x8007007A)
#endif

/* DirectWrite tag matches MAKETAG (little-endian 4 chars). */
#define DWRITE_MAKE_TAG(a, b, c, d) \
    ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | \
     ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))


/* Standard COM AddRef / Release on a leading "const void *lpVtbl; LONG ref;"
 * header.  We use this layout for every object in the stub. */
typedef struct dwrite_obj_header
{
    const void *lpVtbl;
    LONG ref;
} dwrite_obj_header_t;

ULONG STDMETHODCALLTYPE dwrite_common_AddRef(void *iface);
ULONG STDMETHODCALLTYPE dwrite_common_Release(void *iface);
HRESULT STDMETHODCALLTYPE dwrite_common_method_e_notimpl(void *iface, ...);

int dwrite_iid_equals(REFIID a, const GUID *b);

/* IIDs shared between translation units. */
extern const GUID g_iid_IUnknown;

/* ----- Localized strings ---------------------------------------------- */
/* A single en-us string is enough for the populate path. */
HRESULT dwrite_localized_strings_create(const WCHAR *value, void **out);

/* ----- Font collection ------------------------------------------------ */
/* Build (or reuse) the singleton system font collection.  AddRefs.
 * `family_model` is ignored in Phase 1 (typographic and WSS return the
 * same collection). */
HRESULT dwrite_get_system_font_collection(DWORD family_model, void **out);

/* ----- Glyph-run analysis (Phase 2.5) -------------------------------- */

typedef enum DWRITE_TEXTURE_TYPE
{
    DWRITE_TEXTURE_ALIASED_1x1 = 0,
    DWRITE_TEXTURE_CLEARTYPE_3x1 = 1,
} DWRITE_TEXTURE_TYPE;

typedef struct DWRITE_GLYPH_RUN
{
    void *fontFace;
    FLOAT fontEmSize;
    UINT32 glyphCount;
    UINT16 const *glyphIndices;
    FLOAT  const *glyphAdvances;
    DWRITE_GLYPH_OFFSET const *glyphOffsets;
    BOOL isSideways;
    UINT32 bidiLevel;
} DWRITE_GLYPH_RUN;

/* CreateGlyphRunAnalysis implementation lives in dwrite_glyphrun.c;
 * dwrite_main.c's factory slot 23 wraps this. */
HRESULT dwrite_create_glyph_run_analysis(
    DWRITE_GLYPH_RUN const *run, FLOAT pixels_per_dip,
    DWRITE_MATRIX const *transform,
    DWRITE_RENDERING_MODE rendering_mode,
    DWRITE_MEASURING_MODE measuring_mode,
    FLOAT baseline_x, FLOAT baseline_y, void **out);

/* Accessor for the font face's design EM units — used by glyph-run
 * analysis when the family/emsize ratio matters.  Implemented in
 * dwrite_collection.c. */
UINT16 dwrite_face_get_em_units(void *face);
HRESULT dwrite_face_get_logfont(void *face, LOGFONTW *lf);

/* Slot ordinals (cumulative across base + extension interfaces).
 * Counted from the IDWriteFactory IDL chain in Wine's
 * include/dwrite{,_1,_2,_3}.idl as of 2026-05 — keep these in sync
 * with the IDLs when bumping. */
#define DW_FACTORY_GetGdiInterop                   17
/* IDWriteFactory6 block starts at slot 48. */
#define DW_FACTORY6_CreateFontFaceReference        48
#define DW_FACTORY6_CreateFontResource             49
#define DW_FACTORY6_GetSystemFontSet               50
#define DW_FACTORY6_GetSystemFontCollection        51
#define DW_FACTORY6_CreateFontCollectionFromFontSet 52
#define DW_FACTORY6_CreateFontSetBuilder           53
#define DW_FACTORY6_CreateTextFormat               54
/* IDWriteFactory7 block continues at slot 55. */
#define DW_FACTORY7_GetSystemFontSet               55
#define DW_FACTORY7_GetSystemFontCollection        56

#endif /* DWRITE_PRIVATE_H */
