/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for the private RDP capture syscalls
 */

#include "../win32nt.h"

START_TEST(NtUserRdpCapture)
{
    UNICODE_STRING WinSta, Desktop, WrongDesktop;
    NTUSER_RDP_FRAME Frame;
    HANDLE Session;
    PBYTE Pixels;
    ULONG BytesReturned;
    ULONG RequiredSize;
    ULONG FirstFrameId;
    BOOL Ret;

    RtlInitUnicodeString(&WinSta, L"WinSta0");
    RtlInitUnicodeString(&Desktop, L"Default");
    RtlInitUnicodeString(&WrongDesktop, L"Other");

    Session = NtUserRdpOpenSession(0, &WinSta, &WrongDesktop);
    ok(Session == NULL, "NtUserRdpOpenSession unexpectedly opened wrong desktop %p\n", Session);

    Session = NtUserRdpOpenSession(0, &WinSta, &Desktop);
    ok(Session != NULL, "NtUserRdpOpenSession failed\n");
    if (Session == NULL)
        return;

    BytesReturned = 0xdeadbeef;
    RtlZeroMemory(&Frame, sizeof(Frame));
    Ret = NtUserRdpCaptureFrame(Session, &Frame, NULL, 0, &BytesReturned);
    ok(!Ret, "NtUserRdpCaptureFrame unexpectedly succeeded without pixels\n");
    ok(Frame.Size == sizeof(Frame), "Unexpected frame size %lu\n", Frame.Size);
    ok(Frame.SessionId == 0, "Unexpected session id %lu\n", Frame.SessionId);
    ok(Frame.Width != 0 && Frame.Height != 0, "Unexpected dimensions %lux%lu\n", Frame.Width, Frame.Height);
    ok(Frame.BitsPerPixel == 32, "Unexpected bpp %lu\n", Frame.BitsPerPixel);
    ok(Frame.Pitch == Frame.Width * 4, "Unexpected pitch %lu\n", Frame.Pitch);
    ok(Frame.Format == NTUSER_RDP_FRAME_FORMAT_BGRA32, "Unexpected format %lu\n", Frame.Format);
    ok(Frame.RequiredBufferSize == Frame.Pitch * Frame.Height,
       "Unexpected required size %lu\n", Frame.RequiredBufferSize);
    ok(BytesReturned == 0, "Unexpected bytes returned %lu\n", BytesReturned);

    RequiredSize = Frame.RequiredBufferSize;
    Pixels = HeapAlloc(GetProcessHeap(), 0, RequiredSize);
    ok(Pixels != NULL, "HeapAlloc failed for %lu bytes\n", RequiredSize);
    if (Pixels != NULL)
    {
        BytesReturned = 0;
        RtlZeroMemory(Pixels, RequiredSize);
        Ret = NtUserRdpCaptureFrame(Session, &Frame, Pixels, RequiredSize, &BytesReturned);
        ok(Ret, "NtUserRdpCaptureFrame failed with full pixel buffer\n");
        ok(BytesReturned == RequiredSize, "Unexpected bytes returned %lu\n", BytesReturned);
        ok(Pixels[3] == 0xff, "Unexpected first pixel alpha %#x\n", Pixels[3]);

        FirstFrameId = Frame.FrameId;
        BytesReturned = 0;
        RtlZeroMemory(Pixels, RequiredSize);
        Ret = NtUserRdpCaptureFrame(Session, &Frame, Pixels, RequiredSize, &BytesReturned);
        ok(Ret, "Second NtUserRdpCaptureFrame failed with full pixel buffer\n");
        ok(BytesReturned == RequiredSize, "Unexpected second bytes returned %lu\n", BytesReturned);
        ok(Frame.FrameId > FirstFrameId,
           "Expected increasing frame id, got %lu after %lu\n",
           Frame.FrameId,
           FirstFrameId);
        ok(Pixels[3] == 0xff, "Unexpected second first pixel alpha %#x\n", Pixels[3]);
        HeapFree(GetProcessHeap(), 0, Pixels);
    }

    Ret = NtUserRdpCloseSession(Session);
    ok(Ret, "NtUserRdpCloseSession failed\n");

    Ret = NtUserRdpCaptureFrame(Session, &Frame, NULL, 0, &BytesReturned);
    ok(!Ret, "NtUserRdpCaptureFrame unexpectedly succeeded after close\n");
}
