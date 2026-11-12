/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite - boot video ownership under a WDDM miniport
 *
 * Once a WDDM miniport has taken the boot (POST) display with
 * DxgkCbAcquirePostDisplayOwnership, the boot video driver no longer owns the
 * screen: the miniport scans out its own surface from that memory. Like for an
 * XDDM miniport (videoprt), INBV must then report the display as lost and must
 * not draw boot-video strings into the firmware framebuffer any more. If it did,
 * every HalDisplayString (e.g. the KDNET break-in trace) would land on the
 * miniport's primary as linear rows.
 */

#include <kmt_test.h>
#include <ndk/inbvfuncs.h>
#include <arc/arc.h>

/* {1CA05180-A699-450A-9A0C-DE4FBE3DDD89}, registered and enabled by dxgkrnl
 * for every started WDDM display adapter */
static const GUID GUID_DISPLAY_DEVICE_ARRIVAL_TEST =
    {0x1ca05180, 0xa699, 0x450a, {0x9a, 0x0c, 0xde, 0x4f, 0xbe, 0x3d, 0xdd, 0x89}};

BOOLEAN
NTAPI
InbvGetGopFrameBufferInfo(
    _Out_ PLOADER_PARAMETER_FRAMEBUFFER FrameBuffer);

static
ULONG
CountStartedWddmAdapters(VOID)
{
    PWSTR SymbolicLinkList = NULL;
    PWSTR Link;
    ULONG Count = 0;
    NTSTATUS Status;

    /* Only enabled interfaces, i.e. adapters whose start succeeded */
    Status = IoGetDeviceInterfaces(&GUID_DISPLAY_DEVICE_ARRIVAL_TEST, NULL, 0, &SymbolicLinkList);
    if (!NT_SUCCESS(Status))
        return 0;

    for (Link = SymbolicLinkList; *Link; Link += wcslen(Link) + 1)
    {
        trace("WDDM display adapter: %ls\n", Link);
        Count++;
    }
    ExFreePool(SymbolicLinkList);
    return Count;
}

START_TEST(InbvOwnership)
{
    LOADER_PARAMETER_FRAMEBUFFER Fb;
    ULONG Adapters;
    BOOLEAN Owned, Drawn;

    if (!InbvGetGopFrameBufferInfo(&Fb))
    {
        skip(FALSE, "No firmware (GOP) framebuffer, no POST display to hand over\n");
        return;
    }

    Adapters = CountStartedWddmAdapters();
    if (Adapters == 0)
    {
        skip(FALSE, "No started WDDM display adapter owns the POST display\n");
        return;
    }

    trace("GOP framebuffer PA 0x%I64x, %lu WDDM adapter(s)\n",
          Fb.FrameBufferBase.QuadPart, Adapters);

    /* The miniport owns the display now */
    Owned = InbvCheckDisplayOwnership();
    ok(Owned == FALSE, "INBV still owns the display under a WDDM miniport\n");

    /* A boot-video string must not be drawn into the miniport's surface */
    Drawn = InbvDisplayString("InbvOwnership kmtest\r\n");
    ok(Drawn == FALSE, "InbvDisplayString drew into the firmware framebuffer\n");
}
