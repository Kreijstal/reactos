/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     DPI functions for user32 and user32_vista.
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <cbialo2@outlook.com>
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <wingdi.h>
#include <winuser.h>

HDC APIENTRY
NtUserGetDC(HWND hWnd);

#define NDEBUG
#include <debug.h>

/*
 * @stub
 */
UINT
WINAPI
GetDpiForSystem(VOID)
{
    HDC hDC;
    UINT Dpi;
    hDC = NtUserGetDC(NULL);
    Dpi = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(NULL, hDC);
    return Dpi;
}

/*
 * @stub
 */
UINT
WINAPI
GetDpiForWindow(
    _In_ HWND hWnd)
{
    UNIMPLEMENTED_ONCE;
    UNREFERENCED_PARAMETER(hWnd);
    return GetDpiForSystem();
}

/*
 * @stub
 */
BOOL
WINAPI
IsProcessDPIAware(VOID)
{
    UNIMPLEMENTED;
    return FALSE;
}

/*
 * @stub
 */
BOOL
WINAPI
SetProcessDPIAware(VOID)
{
    UNIMPLEMENTED;
    return FALSE;
}

/*
 * @stub
 */
BOOL
WINAPI
SetProcessDpiAwarenessContext(
    _In_ DPI_AWARENESS_CONTEXT context)
{
    switch ((LONG_PTR)context)
    {
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_UNAWARE:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_SYSTEM_AWARE:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED:
            return TRUE;
    }

    return FALSE;
}

/*
 * @stub
 */
BOOL
WINAPI
GetProcessDpiAwarenessInternal(
    _In_  HANDLE process,
    _Out_ DPI_AWARENESS *awareness)
{
    UNIMPLEMENTED;
    return FALSE;
}

/*
 * @stub
 */
BOOL
WINAPI
SetProcessDpiAwarenessInternal(
    _In_ DPI_AWARENESS awareness)
{
    UNIMPLEMENTED;
    return FALSE;
}

BOOL
WINAPI
GetDpiForMonitorInternal(
    _In_  HMONITOR monitor,
    _In_  UINT type,
    _Out_ UINT *x,
    _Out_ UINT *y)
{
    HDC hDC;
    UINT dpiX, dpiY;

    UNREFERENCED_PARAMETER(monitor);

    /* MONITOR_DPI_TYPE: MDT_EFFECTIVE_DPI(0), MDT_ANGULAR_DPI(1), MDT_RAW_DPI(2) */
    if (type > 2 || x == NULL || y == NULL)
        return FALSE;

    /* ReactOS does not virtualize per-monitor DPI, so report the system DPI
     * (LOGPIXELS from the display DC) for every monitor and DPI type. */
    hDC = NtUserGetDC(NULL);
    dpiX = GetDeviceCaps(hDC, LOGPIXELSX);
    dpiY = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(NULL, hDC);

    if (dpiX == 0)
        dpiX = USER_DEFAULT_SCREEN_DPI;
    if (dpiY == 0)
        dpiY = USER_DEFAULT_SCREEN_DPI;

    *x = dpiX;
    *y = dpiY;
    return TRUE;
}

/*
 * @stub
 */
BOOL
WINAPI
LogicalToPhysicalPoint(
    _In_ HWND hwnd, 
    _Inout_ POINT *point )
{
    UNIMPLEMENTED;
    return TRUE;
}
