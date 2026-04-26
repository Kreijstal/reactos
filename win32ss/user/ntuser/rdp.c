/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private RDP frame-capture API
 */

#include <win32k.h>

#define RDP_CAPTURE_MAX_NAME 32
#define RDP_CAPTURE_DEFAULT_WIDTH 1024
#define RDP_CAPTURE_DEFAULT_HEIGHT 768
#define RDP_CAPTURE_BYTES_PER_PIXEL 4
#define RDP_INPUT_NORMALIZED_MAX 65535

typedef struct _RDP_CAPTURE_CONTEXT
{
    LIST_ENTRY Entry;
    HANDLE OwnerProcessId;
    ULONG SessionId;
    ULONG FrameId;
    WCHAR WinStationName[RDP_CAPTURE_MAX_NAME];
    WCHAR DesktopName[RDP_CAPTURE_MAX_NAME];
} RDP_CAPTURE_CONTEXT, *PRDP_CAPTURE_CONTEXT;

static LIST_ENTRY gRdpCaptureContexts = { &gRdpCaptureContexts, &gRdpCaptureContexts };

static
NTSTATUS
RdpCaptureUnicodeString(
    _In_opt_ PUNICODE_STRING UnsafeString,
    _In_z_ PCWSTR DefaultString,
    _Out_writes_(NameLength) PWSTR Name,
    _In_ ULONG NameLength)
{
    UNICODE_STRING CapturedString;
    ULONG CharsToCopy;

    if (NameLength == 0)
        return STATUS_INVALID_PARAMETER;

    if (UnsafeString == NULL)
    {
        RtlStringCchCopyW(Name, NameLength, DefaultString);
        return STATUS_SUCCESS;
    }

    _SEH2_TRY
    {
        ProbeForRead(UnsafeString, sizeof(*UnsafeString), 1);
        CapturedString = *UnsafeString;

        if (CapturedString.Length == 0 || CapturedString.Buffer == NULL)
        {
            RtlStringCchCopyW(Name, NameLength, DefaultString);
        }
        else
        {
            if ((CapturedString.Length % sizeof(WCHAR)) != 0 ||
                CapturedString.Length >= NameLength * sizeof(WCHAR))
            {
                _SEH2_YIELD(return STATUS_NAME_TOO_LONG);
            }

            ProbeForRead(CapturedString.Buffer, CapturedString.Length, sizeof(WCHAR));
            CharsToCopy = CapturedString.Length / sizeof(WCHAR);
            RtlCopyMemory(Name, CapturedString.Buffer, CapturedString.Length);
            Name[CharsToCopy] = UNICODE_NULL;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

static
PRDP_CAPTURE_CONTEXT
RdpFindCaptureContext(
    _In_ HANDLE hRdpSession)
{
    PLIST_ENTRY Entry;
    HANDLE ProcessId = PsGetCurrentProcessId();

    for (Entry = gRdpCaptureContexts.Flink;
         Entry != &gRdpCaptureContexts;
         Entry = Entry->Flink)
    {
        PRDP_CAPTURE_CONTEXT Context = CONTAINING_RECORD(Entry, RDP_CAPTURE_CONTEXT, Entry);

        if ((HANDLE)Context == hRdpSession && Context->OwnerProcessId == ProcessId)
            return Context;
    }

    return NULL;
}

static
BOOL
RdpGetPrimarySurfaceSize(
    _Out_ PULONG Width,
    _Out_ PULONG Height)
{
    PPDEVOBJ ppdev;
    PSURFACE Surface;
    BOOL Ret = FALSE;

    ppdev = EngpGetPDEV(NULL);
    if (ppdev == NULL)
        return FALSE;

    EngAcquireSemaphore(ppdev->hsemDevLock);

    Surface = ppdev->pSurface;
    if (Surface != NULL &&
        Surface->SurfObj.sizlBitmap.cx > 0 &&
        Surface->SurfObj.sizlBitmap.cy > 0)
    {
        *Width = Surface->SurfObj.sizlBitmap.cx;
        *Height = Surface->SurfObj.sizlBitmap.cy;
        Ret = TRUE;
    }

    EngReleaseSemaphore(ppdev->hsemDevLock);
    PDEVOBJ_vRelease(ppdev);

    return Ret;
}

static
VOID
RdpBuildFrameInfo(
    _In_ ULONG SessionId,
    _In_ ULONG FrameId,
    _Out_ PNTUSER_RDP_FRAME Frame)
{
    ULONG Width = RDP_CAPTURE_DEFAULT_WIDTH;
    ULONG Height = RDP_CAPTURE_DEFAULT_HEIGHT;
    ULONG Pitch, RequiredBufferSize;

    if (!RdpGetPrimarySurfaceSize(&Width, &Height) && gpsi != NULL)
    {
        if (gpsi->aiSysMet[SM_CXSCREEN] > 0)
            Width = gpsi->aiSysMet[SM_CXSCREEN];
        if (gpsi->aiSysMet[SM_CYSCREEN] > 0)
            Height = gpsi->aiSysMet[SM_CYSCREEN];
    }

    if (!NT_SUCCESS(RtlULongMult(Width, RDP_CAPTURE_BYTES_PER_PIXEL, &Pitch)) ||
        !NT_SUCCESS(RtlULongMult(Pitch, Height, &RequiredBufferSize)))
    {
        Width = RDP_CAPTURE_DEFAULT_WIDTH;
        Height = RDP_CAPTURE_DEFAULT_HEIGHT;
        Pitch = Width * RDP_CAPTURE_BYTES_PER_PIXEL;
        RequiredBufferSize = Pitch * Height;
    }

    RtlZeroMemory(Frame, sizeof(*Frame));
    Frame->Size = sizeof(*Frame);
    Frame->SessionId = SessionId;
    Frame->Width = Width;
    Frame->Height = Height;
    Frame->BitsPerPixel = 32;
    Frame->Pitch = Pitch;
    Frame->Format = NTUSER_RDP_FRAME_FORMAT_BGRA32;
    Frame->FrameId = FrameId;
    Frame->RequiredBufferSize = RequiredBufferSize;
}

static
VOID
RdpFillDeterministicFrame(
    _In_ const NTUSER_RDP_FRAME *Frame,
    _Out_writes_bytes_(Frame->RequiredBufferSize) PBYTE Pixels)
{
    ULONG X, Y;

    for (Y = 0; Y < Frame->Height; Y++)
    {
        PBYTE Row = Pixels + (Y * Frame->Pitch);

        for (X = 0; X < Frame->Width; X++)
        {
            PBYTE Pixel = Row + (X * RDP_CAPTURE_BYTES_PER_PIXEL);

            Pixel[0] = (BYTE)((X + Frame->FrameId) & 0xff);
            Pixel[1] = (BYTE)((Y + Frame->SessionId) & 0xff);
            Pixel[2] = (BYTE)((X ^ Y) & 0xff);
            Pixel[3] = 0xff;
        }
    }
}

static
VOID
RdpMakeFrameOpaque(
    _In_ const NTUSER_RDP_FRAME *Frame,
    _Inout_updates_bytes_(Frame->RequiredBufferSize) PBYTE Pixels)
{
    ULONG X, Y;

    for (Y = 0; Y < Frame->Height; Y++)
    {
        PBYTE Row = Pixels + (Y * Frame->Pitch);

        for (X = 0; X < Frame->Width; X++)
            Row[(X * RDP_CAPTURE_BYTES_PER_PIXEL) + 3] = 0xff;
    }
}

static
BOOL
RdpCapturePrimarySurfaceFrame(
    _In_ const NTUSER_RDP_FRAME *Frame,
    _Out_writes_bytes_(Frame->RequiredBufferSize) PBYTE Pixels)
{
    PPDEVOBJ ppdev;
    PSURFACE SourceSurface, TargetSurface;
    RECTL DestRect;
    POINTL SourcePoint = { 0, 0 };
    EXLATEOBJ XlateObj;
    BOOL Ret = FALSE;
    BOOL XlateInitialized = FALSE;
    BOOL MouseSafetyActive = FALSE;

    ppdev = EngpGetPDEV(NULL);
    if (ppdev == NULL)
        return FALSE;

    TargetSurface = SURFACE_AllocSurface(STYPE_BITMAP,
                                         Frame->Width,
                                         Frame->Height,
                                         BMF_32BPP,
                                         BMF_TOPDOWN,
                                         Frame->Pitch,
                                         Frame->RequiredBufferSize,
                                         Pixels);
    if (TargetSurface == NULL)
    {
        PDEVOBJ_vRelease(ppdev);
        return FALSE;
    }

    EngAcquireSemaphore(ppdev->hsemDevLock);

    SourceSurface = ppdev->pSurface;
    if (SourceSurface == NULL ||
        SourceSurface->SurfObj.sizlBitmap.cx < (LONG)Frame->Width ||
        SourceSurface->SurfObj.sizlBitmap.cy < (LONG)Frame->Height)
    {
        goto Cleanup;
    }

    DestRect.left = 0;
    DestRect.top = 0;
    DestRect.right = Frame->Width;
    DestRect.bottom = Frame->Height;

    EXLATEOBJ_vInitialize(&XlateObj,
                          SourceSurface->ppal,
                          TargetSurface->ppal,
                          CLR_INVALID,
                          CLR_INVALID,
                          CLR_INVALID);
    XlateInitialized = TRUE;

    MouseSafetyOnDrawStart(ppdev,
                           DestRect.left,
                           DestRect.top,
                           DestRect.right,
                           DestRect.bottom);
    MouseSafetyActive = TRUE;

    Ret = EngCopyBits(&TargetSurface->SurfObj,
                      &SourceSurface->SurfObj,
                      NULL,
                      &XlateObj.xlo,
                      &DestRect,
                      &SourcePoint);

Cleanup:
    if (MouseSafetyActive)
        MouseSafetyOnDrawEnd(ppdev);
    if (XlateInitialized)
        EXLATEOBJ_vCleanup(&XlateObj);

    EngReleaseSemaphore(ppdev->hsemDevLock);
    PDEVOBJ_vRelease(ppdev);

    if (Ret)
        RdpMakeFrameOpaque(Frame, Pixels);

    GDIOBJ_vDeleteObject(&TargetSurface->BaseObject);

    return Ret;
}

static
ULONG
RdpGetScreenMetric(
    _In_ INT Index,
    _In_ ULONG Fallback)
{
    if (gpsi != NULL && gpsi->aiSysMet[Index] > 0)
        return gpsi->aiSysMet[Index];

    return Fallback;
}

static
LONG
RdpNormalizeCoordinate(
    _In_ USHORT Coordinate,
    _In_ ULONG Extent)
{
    ULONG Clamped;

    if (Extent <= 1)
        return 0;

    Clamped = Coordinate;
    if (Clamped >= Extent)
        Clamped = Extent - 1;

    return (LONG)((Clamped * RDP_INPUT_NORMALIZED_MAX) / (Extent - 1));
}

static
DWORD
RdpMouseButtonFlagsToMouseInputFlags(
    _In_ ULONG PointerFlags)
{
    DWORD Flags = 0;
    BOOL Down = (PointerFlags & NTUSER_RDP_MOUSE_FLAG_DOWN) != 0;

    if (PointerFlags & NTUSER_RDP_MOUSE_FLAG_BUTTON1)
        Flags |= Down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;

    if (PointerFlags & NTUSER_RDP_MOUSE_FLAG_BUTTON2)
        Flags |= Down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;

    if (PointerFlags & NTUSER_RDP_MOUSE_FLAG_BUTTON3)
        Flags |= Down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;

    return Flags;
}

HANDLE
APIENTRY
NtUserRdpOpenSession(
    _In_ ULONG SessionId,
    _In_opt_ PUNICODE_STRING WinStationName,
    _In_opt_ PUNICODE_STRING DesktopName)
{
    NTSTATUS Status;
    PRDP_CAPTURE_CONTEXT Context;
    WCHAR CapturedWinStationName[RDP_CAPTURE_MAX_NAME];
    WCHAR CapturedDesktopName[RDP_CAPTURE_MAX_NAME];

    Status = RdpCaptureUnicodeString(WinStationName,
                                     L"WinSta0",
                                     CapturedWinStationName,
                                     ARRAYSIZE(CapturedWinStationName));
    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return NULL;
    }

    Status = RdpCaptureUnicodeString(DesktopName,
                                     L"Default",
                                     CapturedDesktopName,
                                     ARRAYSIZE(CapturedDesktopName));
    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return NULL;
    }

    if (SessionId != 0 ||
        _wcsicmp(CapturedWinStationName, L"WinSta0") != 0 ||
        _wcsicmp(CapturedDesktopName, L"Default") != 0)
    {
        SetLastNtError(STATUS_OBJECT_NAME_NOT_FOUND);
        return NULL;
    }

    Context = ExAllocatePoolWithTag(PagedPool, sizeof(*Context), USERTAG_RDP);
    if (Context == NULL)
    {
        SetLastNtError(STATUS_NO_MEMORY);
        return NULL;
    }

    RtlZeroMemory(Context, sizeof(*Context));
    Context->OwnerProcessId = PsGetCurrentProcessId();
    Context->SessionId = SessionId;
    RtlStringCchCopyW(Context->WinStationName,
                      ARRAYSIZE(Context->WinStationName),
                      CapturedWinStationName);
    RtlStringCchCopyW(Context->DesktopName,
                      ARRAYSIZE(Context->DesktopName),
                      CapturedDesktopName);

    UserEnterExclusive();
    InsertTailList(&gRdpCaptureContexts, &Context->Entry);
    UserLeave();

    return (HANDLE)Context;
}

BOOL
APIENTRY
NtUserRdpCaptureFrame(
    _In_ HANDLE hRdpSession,
    _Out_ PNTUSER_RDP_FRAME Frame,
    _Out_writes_bytes_to_opt_(PixelBufferSize, *BytesReturned) PVOID PixelBuffer,
    _In_ ULONG PixelBufferSize,
    _Out_opt_ PULONG BytesReturned)
{
    NTSTATUS Status = STATUS_SUCCESS;
    BOOL Ret = FALSE;
    PRDP_CAPTURE_CONTEXT Context;
    NTUSER_RDP_FRAME LocalFrame;
    ULONG LocalBytesReturned = 0;
    ULONG SessionId, FrameId;
    PBYTE CapturePixels = NULL;
    BOOL CapturedRealFrame = FALSE;

    if (Frame == NULL)
    {
        SetLastNtError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    UserEnterExclusive();
    Context = RdpFindCaptureContext(hRdpSession);
    if (Context == NULL)
    {
        UserLeave();
        SetLastNtError(STATUS_INVALID_HANDLE);
        return FALSE;
    }

    Context->FrameId++;
    SessionId = Context->SessionId;
    FrameId = Context->FrameId;
    UserLeave();

    RdpBuildFrameInfo(SessionId, FrameId, &LocalFrame);

    _SEH2_TRY
    {
        ProbeForWrite(Frame, sizeof(*Frame), 1);
        *Frame = LocalFrame;

        if (BytesReturned != NULL)
        {
            ProbeForWrite(BytesReturned, sizeof(*BytesReturned), 1);
            *BytesReturned = 0;
        }

        if (PixelBuffer != NULL && PixelBufferSize != 0)
            ProbeForWrite(PixelBuffer, PixelBufferSize, 1);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return FALSE;
    }

    if (PixelBufferSize < LocalFrame.RequiredBufferSize || PixelBuffer == NULL)
    {
        SetLastNtError(STATUS_BUFFER_TOO_SMALL);
        return FALSE;
    }

    _SEH2_TRY
    {
        CapturePixels = ExAllocatePoolWithTag(PagedPool,
                                              LocalFrame.RequiredBufferSize,
                                              USERTAG_RDP);
        if (CapturePixels != NULL)
        {
            CapturedRealFrame = RdpCapturePrimarySurfaceFrame(&LocalFrame, CapturePixels);
            if (CapturedRealFrame)
            {
                RtlCopyMemory(PixelBuffer,
                              CapturePixels,
                              LocalFrame.RequiredBufferSize);
            }
        }

        if (!CapturedRealFrame)
            RdpFillDeterministicFrame(&LocalFrame, PixelBuffer);

        LocalBytesReturned = LocalFrame.RequiredBufferSize;

        if (BytesReturned != NULL)
            *BytesReturned = LocalBytesReturned;

        Ret = TRUE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (CapturePixels != NULL)
        ExFreePoolWithTag(CapturePixels, USERTAG_RDP);

    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return FALSE;
    }

    return Ret;
}

BOOL
APIENTRY
NtUserRdpCloseSession(
    _In_ HANDLE hRdpSession)
{
    PRDP_CAPTURE_CONTEXT Context;

    UserEnterExclusive();
    Context = RdpFindCaptureContext(hRdpSession);
    if (Context == NULL)
    {
        UserLeave();
        SetLastNtError(STATUS_INVALID_HANDLE);
        return FALSE;
    }

    RemoveEntryList(&Context->Entry);
    UserLeave();

    ExFreePoolWithTag(Context, USERTAG_RDP);
    return TRUE;
}

BOOL
APIENTRY
NtUserRdpInjectMouse(
    _In_ ULONG SessionId,
    _In_ PNTUSER_RDP_MOUSE_INPUT Input)
{
    NTSTATUS Status = STATUS_SUCCESS;
    NTUSER_RDP_MOUSE_INPUT LocalInput;
    MOUSEINPUT MouseInput;
    BOOL Ret;

    if (Input == NULL)
    {
        SetLastNtError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    _SEH2_TRY
    {
        ProbeForRead(Input, sizeof(*Input), 1);
        LocalInput = *Input;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return FALSE;
    }

    if (LocalInput.Size != sizeof(LocalInput) ||
        SessionId != 0 ||
        LocalInput.SessionId != SessionId)
    {
        SetLastNtError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    RtlZeroMemory(&MouseInput, sizeof(MouseInput));
    if (LocalInput.PointerFlags & NTUSER_RDP_MOUSE_FLAG_MOVE)
    {
        MouseInput.dx = RdpNormalizeCoordinate(LocalInput.PointerX,
                                               RdpGetScreenMetric(SM_CXSCREEN,
                                                                  RDP_CAPTURE_DEFAULT_WIDTH));
        MouseInput.dy = RdpNormalizeCoordinate(LocalInput.PointerY,
                                               RdpGetScreenMetric(SM_CYSCREEN,
                                                                  RDP_CAPTURE_DEFAULT_HEIGHT));
        MouseInput.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    }

    MouseInput.dwFlags |= RdpMouseButtonFlagsToMouseInputFlags(LocalInput.PointerFlags);
    if (MouseInput.dwFlags == 0)
    {
        SetLastNtError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    UserEnterExclusive();
    Ret = UserSendMouseInput(&MouseInput, TRUE);
    UserLeave();

    if (!Ret)
        SetLastNtError(STATUS_UNSUCCESSFUL);

    return Ret;
}

BOOL
APIENTRY
NtUserRdpInjectKeyboard(
    _In_ ULONG SessionId,
    _In_ PNTUSER_RDP_KEYBOARD_INPUT Input)
{
    NTSTATUS Status = STATUS_SUCCESS;
    NTUSER_RDP_KEYBOARD_INPUT LocalInput;
    KEYBDINPUT KeyboardInput;
    BOOL Ret;

    if (Input == NULL)
    {
        SetLastNtError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    _SEH2_TRY
    {
        ProbeForRead(Input, sizeof(*Input), 1);
        LocalInput = *Input;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return FALSE;
    }

    if (LocalInput.Size != sizeof(LocalInput) ||
        SessionId != 0 ||
        LocalInput.SessionId != SessionId)
    {
        SetLastNtError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    RtlZeroMemory(&KeyboardInput, sizeof(KeyboardInput));
    KeyboardInput.wScan = LocalInput.KeyCode;
    KeyboardInput.dwFlags = KEYEVENTF_SCANCODE;
    if (LocalInput.KeyboardFlags & NTUSER_RDP_KEYBOARD_FLAG_EXTENDED)
        KeyboardInput.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (LocalInput.KeyboardFlags & NTUSER_RDP_KEYBOARD_FLAG_RELEASE)
        KeyboardInput.dwFlags |= KEYEVENTF_KEYUP;

    UserEnterExclusive();
    Ret = UserSendKeyboardInput(&KeyboardInput, TRUE);
    UserLeave();

    if (!Ret)
        SetLastNtError(STATUS_UNSUCCESSFUL);

    return Ret;
}
