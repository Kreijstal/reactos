/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS user32.dll
 * FILE:            win32ss/user/user32/misc/stubs.c
 * PURPOSE:         User32.dll stubs
 * PROGRAMMER:      Casper S. Hornstrup (chorns@users.sourceforge.net)
 * NOTES:           If you implement a function, remove it from this file
 * UPDATE HISTORY:
 *      08-F05-2001  CSH  Created
 */

#include <user32.h>

WINE_DEFAULT_DEBUG_CHANNEL(user32);

#define MAX_RAWINPUT_REGISTRATIONS 32

static RAWINPUTDEVICE RegisteredRawInputDevices[MAX_RAWINPUT_REGISTRATIONS];
static UINT RegisteredRawInputDeviceCount;

static INT
CompareRawInputDevices(const RAWINPUTDEVICE *Left, const RAWINPUTDEVICE *Right)
{
    if (Left->usUsagePage != Right->usUsagePage)
        return (INT)Left->usUsagePage - (INT)Right->usUsagePage;

    return (INT)Left->usUsage - (INT)Right->usUsage;
}

static UINT
FindRegisteredRawInputDevice(USHORT UsagePage, USHORT Usage)
{
    UINT i;

    for (i = 0; i < RegisteredRawInputDeviceCount; ++i)
    {
        if (RegisteredRawInputDevices[i].usUsagePage == UsagePage &&
            RegisteredRawInputDevices[i].usUsage == Usage)
        {
            return i;
        }
    }

    return MAX_RAWINPUT_REGISTRATIONS;
}

static VOID
RemoveRegisteredRawInputDevice(UINT Index)
{
    if (Index + 1 < RegisteredRawInputDeviceCount)
    {
        MoveMemory(&RegisteredRawInputDevices[Index],
                   &RegisteredRawInputDevices[Index + 1],
                   (RegisteredRawInputDeviceCount - Index - 1) * sizeof(RAWINPUTDEVICE));
    }

    --RegisteredRawInputDeviceCount;
}

static VOID
CopyRegisteredRawInputDevices(PRAWINPUTDEVICE Devices)
{
    BOOL Used[MAX_RAWINPUT_REGISTRATIONS] = { FALSE };
    UINT i, j, Best;

    for (i = 0; i < RegisteredRawInputDeviceCount; ++i)
    {
        Best = MAX_RAWINPUT_REGISTRATIONS;
        for (j = 0; j < RegisteredRawInputDeviceCount; ++j)
        {
            if (Used[j])
                continue;

            if (Best == MAX_RAWINPUT_REGISTRATIONS ||
                CompareRawInputDevices(&RegisteredRawInputDevices[j],
                                       &RegisteredRawInputDevices[Best]) < 0)
            {
                Best = j;
            }
        }

        Devices[i] = RegisteredRawInputDevices[Best];
        Used[Best] = TRUE;
    }
}

/*
 * @unimplemented
 */
DWORD
WINAPI
WaitForInputIdle(
  HANDLE hProcess,
  DWORD dwMilliseconds)
{
// Need to call NtQueryInformationProcess and send ProcessId not hProcess.
  return NtUserWaitForInputIdle(hProcess, dwMilliseconds, FALSE);
}

/******************************************************************************
 * SetDebugErrorLevel [USER32.@]
 * Sets the minimum error level for generating debugging events
 *
 * PARAMS
 *    dwLevel [I] Debugging error level
 *
 * @unimplemented
 */
VOID
WINAPI
SetDebugErrorLevel( DWORD dwLevel )
{
    FIXME("(%lu): stub\n", dwLevel);
}


/*
 * @implemented
 */
DWORD
WINAPI
GetAppCompatFlags(HTASK hTask)
{
    PCLIENTINFO pci = GetWin32ClientInfo();

    return pci->dwCompatFlags;
}

/*
 * @implemented
 */
DWORD
WINAPI
GetAppCompatFlags2(HTASK hTask)
{
    PCLIENTINFO pci = GetWin32ClientInfo();

    return pci->dwCompatFlags2;
}

/*
 * @unimplemented
 */
VOID
WINAPI
LoadLocalFonts ( VOID )
{
  UNIMPLEMENTED;
}

/*
 * @unimplemented
 */
VOID
WINAPI
LoadRemoteFonts ( VOID )
{
  UNIMPLEMENTED;
}

/*
 * @unimplemented
 */
VOID
WINAPI
RegisterSystemThread ( DWORD flags, DWORD reserved )
{
  UNIMPLEMENTED;
}


/*
 * @implemented
 */
UINT
WINAPI
UserRealizePalette ( HDC hDC )
{
  return NtUserxRealizePalette(hDC);
}


/*************************************************************************
 *		SetSysColorsTemp (USER32.@) (Wine 10/22/2008)
 *
 * UNDOCUMENTED !!
 *
 * Called by W98SE desk.cpl Control Panel Applet:
 * handle = SetSysColorsTemp(ptr, ptr, nCount);     ("set" call)
 * result = SetSysColorsTemp(NULL, NULL, handle);   ("restore" call)
 *
 * pPens is an array of COLORREF values, which seems to be used
 * to indicate the color values to create new pens with.
 *
 * pBrushes is an array of solid brush handles (returned by a previous
 * CreateSolidBrush), which seems to contain the brush handles to set
 * for the system colors.
 *
 * n seems to be used for
 *   a) indicating the number of entries to operate on (length of pPens,
 *      pBrushes)
 *   b) passing the handle that points to the previously used color settings.
 *      I couldn't figure out in hell what kind of handle this is on
 *      Windows. I just use a heap handle instead. Shouldn't matter anyway.
 *
 * RETURNS
 *     heap handle of our own copy of the current syscolors in case of
 *                 "set" call, i.e. pPens, pBrushes != NULL.
 *     TRUE (unconditionally !) in case of "restore" call,
 *          i.e. pPens, pBrushes == NULL.
 *     FALSE in case of either pPens != NULL and pBrushes == NULL
 *          or pPens == NULL and pBrushes != NULL.
 *
 * I'm not sure whether this implementation is 100% correct. [AM]
 */

static HPEN SysColorPens[COLOR_MENUBAR + 1];
static HBRUSH SysColorBrushes[COLOR_MENUBAR + 1];

DWORD_PTR
WINAPI
SetSysColorsTemp(const COLORREF *pPens,
                 const HBRUSH *pBrushes,
				 DWORD_PTR n)
{
    DWORD i;

    if (pPens && pBrushes) /* "set" call */
    {
        /* allocate our structure to remember old colors */
        LPVOID pOldCol = HeapAlloc(GetProcessHeap(), 0, sizeof(DWORD)+n*sizeof(HPEN)+n*sizeof(HBRUSH));
        LPVOID p = pOldCol;
        *(DWORD_PTR *)p = n; p = (char*)p + sizeof(DWORD);
        memcpy(p, SysColorPens, n*sizeof(HPEN)); p = (char*)p + n*sizeof(HPEN);
        memcpy(p, SysColorBrushes, n*sizeof(HBRUSH)); p = (char*)p + n*sizeof(HBRUSH);

        for (i=0; i < n; i++)
        {
            SysColorPens[i] = CreatePen( PS_SOLID, 1, pPens[i] );
            SysColorBrushes[i] = pBrushes[i];
        }

        return (DWORD_PTR) pOldCol;
    }
    if (!pPens && !pBrushes) /* "restore" call */
    {
        LPVOID pOldCol = (LPVOID)n;
        LPVOID p = pOldCol;
        DWORD nCount = *(DWORD *)p;
        p = (char*)p + sizeof(DWORD);

        for (i=0; i < nCount; i++)
        {
            DeleteObject(SysColorPens[i]);
            SysColorPens[i] = *(HPEN *)p; p = (char*)p + sizeof(HPEN);
        }
        for (i=0; i < nCount; i++)
        {
            SysColorBrushes[i] = *(HBRUSH *)p; p = (char*)p + sizeof(HBRUSH);
        }
        /* get rid of storage structure */
        HeapFree(GetProcessHeap(), 0, pOldCol);

        return TRUE;
    }
    return FALSE;
}

/*
 * @unimplemented
 */
HDESK
WINAPI
GetInputDesktop ( VOID )
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
GetAccCursorInfo ( PCURSORINFO pci )
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
UINT
WINAPI
GetRawInputDeviceInfoW(
    HANDLE hDevice,
    UINT uiCommand,
    LPVOID pData,
    PUINT pcbSize)
{
  UNIMPLEMENTED;
  return 0;
}

/*
 * @unimplemented
 */
LONG
WINAPI
CsrBroadcastSystemMessageExW(
    DWORD dwflags,
    LPDWORD lpdwRecipients,
    UINT uiMessage,
    WPARAM wParam,
    LPARAM lParam,
    PBSMINFO pBSMInfo)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
UINT
WINAPI
GetRawInputDeviceInfoA(
    HANDLE hDevice,
    UINT uiCommand,
    LPVOID pData,
    PUINT pcbSize)
{
  UNIMPLEMENTED;
  return 0;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
AlignRects(LPRECT rect, DWORD b, DWORD c, DWORD d)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @implemented
 */
LRESULT
WINAPI
DefRawInputProc(
    PRAWINPUT* paRawInput,
    INT nInput,
    UINT cbSizeHeader)
{
  if (cbSizeHeader == sizeof(RAWINPUTHEADER))
     return S_OK;
  return 1;
}

/*
 * @implemented
 */
UINT
WINAPI
DECLSPEC_HOTPATCH
GetRawInputBuffer(
    PRAWINPUT pData,
    PUINT pcbSize,
    UINT cbSizeHeader)
{
    if (!pcbSize || cbSizeHeader != sizeof(RAWINPUTHEADER))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }

    /*
     * ReactOS does not queue raw input packets yet. Preserve the observable
     * no-packet behavior instead of reporting a successful read of stale data.
     */
    *pcbSize = 0;
    return 0;
}

/*
 * @implemented
 */
UINT
WINAPI
GetRawInputData(
    HRAWINPUT hRawInput,
    UINT uiCommand,
    LPVOID pData,
    PUINT pcbSize,
    UINT cbSizeHeader)
{
    if (!hRawInput)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return (UINT)-1;
    }

    if (!pcbSize || cbSizeHeader != sizeof(RAWINPUTHEADER) ||
        (uiCommand != RID_HEADER && uiCommand != RID_INPUT))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }

    SetLastError(ERROR_INVALID_HANDLE);
    return (UINT)-1;
}

/*
 * @unimplemented
 */
UINT
WINAPI
GetRawInputDeviceList(
    PRAWINPUTDEVICELIST pRawInputDeviceList,
    PUINT puiNumDevices,
    UINT cbSize)
{
    if(pRawInputDeviceList)
        memset(pRawInputDeviceList, 0, sizeof *pRawInputDeviceList);
    if(puiNumDevices)
       *puiNumDevices = 0;

    UNIMPLEMENTED;
    return 0;
}

/*
 * @implemented
 */
UINT
WINAPI
DECLSPEC_HOTPATCH
GetRegisteredRawInputDevices(
    PRAWINPUTDEVICE pRawInputDevices,
    PUINT puiNumDevices,
    UINT cbSize)
{
    if (cbSize != sizeof(RAWINPUTDEVICE) || !puiNumDevices)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }

    if (!pRawInputDevices)
    {
        *puiNumDevices = RegisteredRawInputDeviceCount;
        return 0;
    }

    if (!*puiNumDevices)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }

    if (*puiNumDevices < RegisteredRawInputDeviceCount)
    {
        *puiNumDevices = RegisteredRawInputDeviceCount;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return (UINT)-1;
    }

    CopyRegisteredRawInputDevices(pRawInputDevices);
    *puiNumDevices = RegisteredRawInputDeviceCount;
    return RegisteredRawInputDeviceCount;
}

/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
RegisterRawInputDevices(
    PCRAWINPUTDEVICE pRawInputDevices,
    UINT uiNumDevices,
    UINT cbSize)
{
    UINT i, Index;

    if (cbSize != sizeof(RAWINPUTDEVICE) || !pRawInputDevices || !uiNumDevices)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (i = 0; i < uiNumDevices; ++i)
    {
        if ((pRawInputDevices[i].dwFlags & RIDEV_INPUTSINK) &&
            !pRawInputDevices[i].hwndTarget)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        if ((pRawInputDevices[i].dwFlags & RIDEV_REMOVE) &&
            pRawInputDevices[i].hwndTarget)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    for (i = 0; i < uiNumDevices; ++i)
    {
        Index = FindRegisteredRawInputDevice(pRawInputDevices[i].usUsagePage,
                                             pRawInputDevices[i].usUsage);

        if (pRawInputDevices[i].dwFlags & RIDEV_REMOVE)
        {
            if (Index != MAX_RAWINPUT_REGISTRATIONS)
                RemoveRegisteredRawInputDevice(Index);

            continue;
        }

        if (Index == MAX_RAWINPUT_REGISTRATIONS)
        {
            if (RegisteredRawInputDeviceCount == MAX_RAWINPUT_REGISTRATIONS)
            {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return FALSE;
            }

            Index = RegisteredRawInputDeviceCount++;
        }

        RegisteredRawInputDevices[Index] = pRawInputDevices[i];

        if ((pRawInputDevices[i].dwFlags & RIDEV_DEVNOTIFY) &&
            pRawInputDevices[i].hwndTarget)
        {
            PostMessageW(pRawInputDevices[i].hwndTarget,
                         WM_INPUT_DEVICE_CHANGE,
                         GIDC_ARRIVAL,
                         0);
        }
    }

    return TRUE;
}

/*
 * @unimplemented
 */
BOOL WINAPI DisplayExitWindowsWarnings(ULONG flags)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
BOOL WINAPI ReasonCodeNeedsBugID(ULONG reasoncode)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
BOOL WINAPI ReasonCodeNeedsComment(ULONG reasoncode)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
BOOL WINAPI CtxInitUser32(VOID)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
BOOL WINAPI EnterReaderModeHelper(HWND hwnd)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
VOID WINAPI InitializeLpkHooks(FARPROC *hookfuncs)
{
  UNIMPLEMENTED;
}

/*
 * @unimplemented
 */
WORD WINAPI InitializeWin32EntryTable(UCHAR* EntryTablePlus0x1000)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
BOOL WINAPI IsServerSideWindow(HWND wnd)
{
  UNIMPLEMENTED;
  return FALSE;
}

/*
 * @unimplemented
 */
VOID WINAPI AllowForegroundActivation(VOID)
{
  UNIMPLEMENTED;
}

/*
 * @unimplemented
 */
VOID WINAPI ShowStartGlass(DWORD unknown)
{
  UNIMPLEMENTED;
}

/*
 * @implemented
 */
DWORD WINAPI GetMenuIndex(HMENU hMenu, HMENU hSubMenu)
{
  return NtUserGetMenuIndex(hMenu, hSubMenu);
}

/*
 * @unimplemented
 */
DWORD WINAPI UserRegisterWowHandlers(PVOID Unknown1, PVOID Unknown2)
{
  UNIMPLEMENTED;
  return 0;
}

BOOL
WINAPI
BuildReasonArray(PVOID Pointer)
{
    UNIMPLEMENTED;
    return FALSE;
}

VOID
WINAPI
CreateSystemThreads(DWORD Unused)
{
    /* Thread call for remote processes (non-CSRSS) only */
    NtUserxCreateSystemThreads(TRUE);
    ExitThread(0);
}

BOOL
WINAPI
DestroyReasons(PVOID Pointer)
{
    UNIMPLEMENTED;
    return FALSE;
}

NTSTATUS
WINAPI
DeviceEventWorker(HWND hwnd, WPARAM wParam, LPARAM lParam, DWORD Data, ULONG_PTR *uResult)
{
    USER_API_MESSAGE ApiMessage;
    PUSER_DEVICE_EVENT_MSG pusem = &ApiMessage.Data.DeviceEventMsg;

    pusem->hwnd = hwnd;
    pusem->wParam = wParam;
    pusem->lParam = lParam;
    pusem->Data = Data;
    pusem->Result = 0;

    TRACE("DeviceEventWorker : hwnd %p, wParam %d, lParam %d, Data %d, uResult %p\n", hwnd, wParam, lParam, Data, uResult);

    if ( lParam == 0 )
    {
        CsrClientCallServer( (PCSR_API_MESSAGE)&ApiMessage,
                              NULL,
                              CSR_CREATE_API_NUMBER( USERSRV_SERVERDLL_INDEX, UserpDeviceEvent ),
                              sizeof(*pusem) );
    }
    else
    {
        PCSR_CAPTURE_BUFFER pcsrcb = NULL;
        PDEV_BROADCAST_HDR pdev_br = (PDEV_BROADCAST_HDR)lParam;
        ULONG BufferSize = pdev_br->dbch_size;

        pcsrcb = CsrAllocateCaptureBuffer( 1, BufferSize );

        if ( !pcsrcb )
        {
            return STATUS_NO_MEMORY;
        }

        CsrCaptureMessageBuffer( pcsrcb, (PVOID)lParam, BufferSize, (PVOID*)&pusem->lParam );

        CsrClientCallServer( (PCSR_API_MESSAGE)&ApiMessage,
                              pcsrcb,
                              CSR_CREATE_API_NUMBER( USERSRV_SERVERDLL_INDEX, UserpDeviceEvent ),
                              sizeof(*pusem) );

        CsrFreeCaptureBuffer( pcsrcb );
    }

    if (NT_SUCCESS(ApiMessage.Status))
    {
        *uResult = pusem->Result;
    }

    return ApiMessage.Status;
}

BOOL
WINAPI
GetReasonTitleFromReasonCode(DWORD dw1, DWORD dw2, DWORD dw3)
{
    UNIMPLEMENTED;
    return FALSE;
}

BOOL
WINAPI
IsSETEnabled(VOID)
{
    /*
     * Determines whether the Shutdown Event Tracker is enabled.
     *
     * See http://undoc.airesoft.co.uk/user32.dll/IsSETEnabled.php
     * for more information.
     */
    UNIMPLEMENTED;
    return FALSE;
}

BOOL
WINAPI
RecordShutdownReason(DWORD dw0)
{
    UNIMPLEMENTED;
    return FALSE;
}

BOOL
WINAPI
UserLpkTabbedTextOut(
    DWORD dw1,
    DWORD dw2,
    DWORD dw3,
    DWORD dw4,
    DWORD dw5,
    DWORD dw6,
    DWORD dw7,
    DWORD dw8,
    DWORD dw9,
    DWORD dw10,
    DWORD dw11,
    DWORD dw12)
{
    UNIMPLEMENTED;
    return FALSE;
}

BOOL
WINAPI
Win32PoolAllocationStats(DWORD dw1, DWORD dw2, DWORD dw3, DWORD dw4, DWORD dw5)
{
    UNIMPLEMENTED;
    return FALSE;
}


/* DPI-aware variants of pre-existing window APIs and the timer extension.
 * Real per-thread/window DPI is in win32k; these forward to the
 * non-DPI primitives so callers operate as if at the default 96-DPI. */

BOOL WINAPI AdjustWindowRectExForDpi(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle, UINT dpi)
{
    UNREFERENCED_PARAMETER(dpi);
    return AdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle);
}

BOOL WINAPI AreDpiAwarenessContextsEqual(HANDLE dpiContextA, HANDLE dpiContextB)
{
    return dpiContextA == dpiContextB;
}

BOOL WINAPI IsValidDpiAwarenessContext(DPI_AWARENESS_CONTEXT dpiContext)
{
    switch ((LONG_PTR)dpiContext)
    {
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_UNAWARE:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_SYSTEM_AWARE:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2:
        case (LONG_PTR)DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED:
            return TRUE;

        default:
            return FALSE;
    }
}

BOOL WINAPI EnableNonClientDpiScaling(HWND hwnd)
{
    UNREFERENCED_PARAMETER(hwnd);
    return TRUE;
}

int WINAPI GetSystemMetricsForDpi(int nIndex, UINT dpi)
{
    UNREFERENCED_PARAMETER(dpi);
    return GetSystemMetrics(nIndex);
}

HANDLE WINAPI GetThreadDpiAwarenessContext(VOID)
{
    /* DPI_AWARENESS_CONTEXT_UNAWARE is documented as the literal -1. */
    return (HANDLE)(LONG_PTR)-1;
}

HANDLE WINAPI GetWindowDpiAwarenessContext(HWND hwnd)
{
    UNREFERENCED_PARAMETER(hwnd);
    return (HANDLE)(LONG_PTR)-1;
}

UINT_PTR WINAPI SetCoalescableTimer(HWND hwnd, UINT_PTR id, UINT msec, TIMERPROC proc, ULONG tolerance)
{
    UNREFERENCED_PARAMETER(tolerance);
    return SetTimer(hwnd, id, msec, proc);
}

BOOL WINAPI SystemParametersInfoForDpi(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni, UINT dpi)
{
    UNREFERENCED_PARAMETER(dpi);
    return SystemParametersInfoW(uiAction, uiParam, pvParam, fWinIni);
}

/* Win8/10 pointer-input API stubs. The installer probes for these to
 * detect touch/pen capability; reporting failure makes it fall back to
 * mouse-only input. */

BOOL WINAPI GetPointerDeviceRects(HANDLE device, RECT *pointerDeviceRect, RECT *displayRect)
{
    UNREFERENCED_PARAMETER(device);
    if (pointerDeviceRect) SetRectEmpty(pointerDeviceRect);
    if (displayRect) SetRectEmpty(displayRect);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI GetPointerFrameTouchInfo(UINT32 pointerId, UINT32 *pointerCount, PVOID touchInfo)
{
    UNREFERENCED_PARAMETER(pointerId);
    UNREFERENCED_PARAMETER(touchInfo);
    if (pointerCount) *pointerCount = 0;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI GetPointerFrameTouchInfoHistory(UINT32 pointerId, UINT32 *entriesCount, UINT32 *pointerCount, PVOID touchInfo)
{
    UNREFERENCED_PARAMETER(pointerId);
    UNREFERENCED_PARAMETER(touchInfo);
    if (entriesCount) *entriesCount = 0;
    if (pointerCount) *pointerCount = 0;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI GetPointerPenInfo(UINT32 pointerId, PVOID penInfo)
{
    UNREFERENCED_PARAMETER(pointerId);
    UNREFERENCED_PARAMETER(penInfo);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI GetPointerPenInfoHistory(UINT32 pointerId, UINT32 *entriesCount, PVOID penInfo)
{
    UNREFERENCED_PARAMETER(pointerId);
    UNREFERENCED_PARAMETER(penInfo);
    if (entriesCount) *entriesCount = 0;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI SkipPointerFrameMessages(UINT32 pointerId)
{
    UNREFERENCED_PARAMETER(pointerId);
    return TRUE;
}
