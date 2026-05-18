/*
 * PROJECT:     ReactOS d3d12.dll stub
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Minimal d3d12.dll surface so modern installers load.
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(d3d12);

#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004L)
#endif

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, UINT minimum_feature_level,
                                 REFIID iid, void **device)
{
    FIXME("(%p,%u,%s,%p): stub\n", adapter, minimum_feature_level,
          wine_dbgstr_guid(iid), device);
    if (device) *device = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID iid, void **debug)
{
    FIXME("(%s,%p): stub\n", wine_dbgstr_guid(iid), debug);
    if (debug) *debug = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(const void *root_signature,
                                                    void **blob, void **error_blob)
{
    FIXME("(%p,%p,%p): stub\n", root_signature, blob, error_blob);
    if (blob) *blob = NULL;
    if (error_blob) *error_blob = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}
