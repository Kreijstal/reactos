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

#ifndef E_NOT_SUFFICIENT_BUFFER
#define E_NOT_SUFFICIENT_BUFFER ((HRESULT)0x8007007A)
#endif


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
