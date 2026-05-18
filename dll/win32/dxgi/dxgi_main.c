/*
 * PROJECT:     ReactOS DXGI stub
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Minimal dxgi.dll surface so modern installers load.
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002L)
#endif
#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004L)
#endif

HRESULT WINAPI CreateDXGIFactory(REFIID iid, void **factory)
{
    FIXME("(%s,%p): stub\n", wine_dbgstr_guid(iid), factory);
    if (factory) *factory = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID iid, void **factory)
{
    FIXME("(%s,%p): stub\n", wine_dbgstr_guid(iid), factory);
    if (factory) *factory = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID iid, void **factory)
{
    FIXME("(%08x,%s,%p): stub\n", flags, wine_dbgstr_guid(iid), factory);
    if (factory) *factory = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}
