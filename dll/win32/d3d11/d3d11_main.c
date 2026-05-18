/*
 * PROJECT:     ReactOS d3d11.dll stub
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Minimal d3d11.dll surface so modern installers load.
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004L)
#endif

HRESULT WINAPI D3D11CreateDevice(void *adapter, UINT driver_type, HMODULE software,
                                 UINT flags, const UINT *feature_levels,
                                 UINT feature_levels_count, UINT sdk_version,
                                 void **device, UINT *feature_level, void **context)
{
    FIXME("stub\n");
    if (device) *device = NULL;
    if (context) *context = NULL;
    if (feature_level) *feature_level = 0;
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(void *adapter, UINT driver_type, HMODULE software,
                                             UINT flags, const UINT *feature_levels,
                                             UINT feature_levels_count, UINT sdk_version,
                                             const void *swapchain_desc, void **swapchain,
                                             void **device, UINT *feature_level, void **context)
{
    FIXME("stub\n");
    if (swapchain) *swapchain = NULL;
    if (device) *device = NULL;
    if (context) *context = NULL;
    if (feature_level) *feature_level = 0;
    return DXGI_ERROR_UNSUPPORTED;
}
