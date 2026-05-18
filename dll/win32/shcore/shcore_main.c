/*
 * PROJECT:     ReactOS shcore.dll
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Windows 8+ shell-core helper APIs (DPI scaling, AppUserModelID).
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(shcore);

/* CommandLineToArgvW is implemented in shell32; re-export the same behaviour. */
LPWSTR* WINAPI shcore_CommandLineToArgvW(LPCWSTR lpCmdLine, int *numargs)
{
    return CommandLineToArgvW(lpCmdLine, numargs);
}

HRESULT WINAPI GetDpiForMonitor(HMONITOR hmonitor, MONITOR_DPI_TYPE dpiType,
                                UINT *dpiX, UINT *dpiY)
{
    FIXME("(%p,%u,%p,%p): returning system DPI\n", hmonitor, dpiType, dpiX, dpiY);

    if (!dpiX || !dpiY)
        return E_INVALIDARG;

    *dpiX = *dpiY = 96;
    return S_OK;
}

HRESULT WINAPI GetProcessDpiAwareness(HANDLE hprocess, PROCESS_DPI_AWARENESS *value)
{
    FIXME("(%p,%p): stub\n", hprocess, value);
    if (!value) return E_INVALIDARG;
    *value = PROCESS_DPI_UNAWARE;
    return S_OK;
}

HRESULT WINAPI SetProcessDpiAwareness(PROCESS_DPI_AWARENESS value)
{
    FIXME("(%u): stub\n", value);
    return S_OK;
}

DEVICE_SCALE_FACTOR WINAPI GetScaleFactorForDevice(DISPLAY_DEVICE_TYPE deviceType)
{
    FIXME("(%u): stub\n", deviceType);
    return SCALE_100_PERCENT;
}

HRESULT WINAPI GetScaleFactorForMonitor(HMONITOR hMon, DEVICE_SCALE_FACTOR *pScale)
{
    FIXME("(%p,%p): stub\n", hMon, pScale);
    if (!pScale) return E_INVALIDARG;
    *pScale = SCALE_100_PERCENT;
    return S_OK;
}

HRESULT WINAPI SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID)
{
    FIXME("(%s): stub\n", wine_dbgstr_w(AppID));
    return S_OK;
}

HRESULT WINAPI GetCurrentProcessExplicitAppUserModelID(PWSTR *AppID)
{
    FIXME("(%p): stub\n", AppID);
    if (AppID) *AppID = NULL;
    return E_FAIL;
}
