/*
 * PROJECT:     ReactOS DirectWrite
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     IDWriteLocalizedStrings — single-entry en-us holder.
 *
 * The Qt 6 font-database populate path retrieves family/face names by
 * GetString(index=0, ...).  It does not enumerate locales, so a single
 * "en-us" entry per object is enough.
 */

#include <stdarg.h>

#include "dwrite_private.h"

#include <wine/debug.h>
WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

static const GUID g_iid_IDWriteLocalizedStrings =
    {0x08256209, 0x099a, 0x4b34, {0xb8, 0x6d, 0xc2, 0x2b, 0x11, 0x0e, 0x77, 0x71}};

typedef struct dwrite_localized_strings
{
    const void *lpVtbl;
    LONG ref;
    WCHAR *value;       /* Heap-allocated copy of the single string. */
    UINT32 value_len;   /* Length in WCHARs, NOT including NUL. */
} dwrite_localized_strings_t;

static const WCHAR g_locale_enus[] = {'e','n','-','u','s',0};
static const UINT32 g_locale_enus_len = 5;  /* "en-us" */

static HRESULT STDMETHODCALLTYPE locstrs_QueryInterface(void *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (dwrite_iid_equals(iid, &g_iid_IUnknown) ||
        dwrite_iid_equals(iid, &g_iid_IDWriteLocalizedStrings))
    {
        *out = iface;
        dwrite_common_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* UINT32 GetCount(void) */
static UINT32 STDMETHODCALLTYPE locstrs_GetCount(void *iface)
{
    (void)iface;
    return 1;
}

/* HRESULT FindLocaleName(WCHAR const *locale, UINT32 *index, BOOL *exists) */
static HRESULT STDMETHODCALLTYPE locstrs_FindLocaleName(
    void *iface, const WCHAR *locale, UINT32 *index, BOOL *exists)
{
    (void)iface;
    if (!index || !exists) return E_POINTER;
    *index = 0;
    *exists = (locale != NULL);  /* Always pretend we have whatever locale you want. */
    return S_OK;
}

/* HRESULT GetLocaleNameLength(UINT32 index, UINT32 *length) */
static HRESULT STDMETHODCALLTYPE locstrs_GetLocaleNameLength(
    void *iface, UINT32 index, UINT32 *length)
{
    (void)iface;
    if (!length) return E_POINTER;
    if (index != 0) return E_INVALIDARG;
    *length = g_locale_enus_len;
    return S_OK;
}

/* HRESULT GetLocaleName(UINT32 index, WCHAR *buf, UINT32 size) */
static HRESULT STDMETHODCALLTYPE locstrs_GetLocaleName(
    void *iface, UINT32 index, WCHAR *buf, UINT32 size)
{
    (void)iface;
    if (!buf) return E_POINTER;
    if (index != 0) return E_INVALIDARG;
    if (size < g_locale_enus_len + 1) return E_NOT_SUFFICIENT_BUFFER;
    memcpy(buf, g_locale_enus, (g_locale_enus_len + 1) * sizeof(WCHAR));
    return S_OK;
}

/* HRESULT GetStringLength(UINT32 index, UINT32 *length) */
static HRESULT STDMETHODCALLTYPE locstrs_GetStringLength(
    void *iface, UINT32 index, UINT32 *length)
{
    dwrite_localized_strings_t *self = (dwrite_localized_strings_t *)iface;
    if (!length) return E_POINTER;
    if (index != 0) return E_INVALIDARG;
    *length = self->value_len;
    return S_OK;
}

/* HRESULT GetString(UINT32 index, WCHAR *buf, UINT32 size) */
static HRESULT STDMETHODCALLTYPE locstrs_GetString(
    void *iface, UINT32 index, WCHAR *buf, UINT32 size)
{
    dwrite_localized_strings_t *self = (dwrite_localized_strings_t *)iface;
    if (!buf) return E_POINTER;
    if (index != 0) return E_INVALIDARG;
    if (size < self->value_len + 1) return E_NOT_SUFFICIENT_BUFFER;
    memcpy(buf, self->value, (self->value_len + 1) * sizeof(WCHAR));
    return S_OK;
}

static ULONG STDMETHODCALLTYPE locstrs_Release(void *iface)
{
    dwrite_localized_strings_t *self = (dwrite_localized_strings_t *)iface;
    LONG r = InterlockedDecrement(&self->ref);
    if (r == 0)
    {
        HeapFree(GetProcessHeap(), 0, self->value);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return r < 0 ? 0 : (ULONG)r;
}

static const void * const g_localized_strings_vtbl[9] =
{
    locstrs_QueryInterface,
    dwrite_common_AddRef,
    locstrs_Release,
    locstrs_GetCount,
    locstrs_FindLocaleName,
    locstrs_GetLocaleNameLength,
    locstrs_GetLocaleName,
    locstrs_GetStringLength,
    locstrs_GetString,
};

HRESULT dwrite_localized_strings_create(const WCHAR *value, void **out)
{
    dwrite_localized_strings_t *self;
    size_t len;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!value) return E_INVALIDARG;

    len = 0;
    while (value[len]) ++len;

    self = HeapAlloc(GetProcessHeap(), 0, sizeof(*self));
    if (!self) return E_OUTOFMEMORY;

    self->lpVtbl = g_localized_strings_vtbl;
    self->ref = 1;
    self->value_len = (UINT32)len;
    self->value = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (!self->value)
    {
        HeapFree(GetProcessHeap(), 0, self);
        return E_OUTOFMEMORY;
    }
    memcpy(self->value, value, (len + 1) * sizeof(WCHAR));
    *out = self;
    return S_OK;
}
