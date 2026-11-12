/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     dxgkrnl miniport services and segment model, checked through
 *              softgpu's driver-private escapes
 * COPYRIGHT:   Copyright 2026 ReactOS WDDM Team
 *
 * softgpu (drivers/directx/softgpu) answers two D3DKMT_ESCAPE_DRIVERPRIVATE
 * escapes that let a user-mode test see dxgkrnl from the miniport's side:
 *
 *  - SOFTGPU_ESCAPE_TIMEDOP runs a DxgkServicesTimedOperation scenario in the
 *    miniport (as atikmdag uses the interface) and returns the statuses.
 *    Windows' dxgkrnl (TdrTimedOperation*): Start requires the preset Size,
 *    takes |Timeout| (0 allowed) and clamps it to 5 s; Delay(NULL) returns at
 *    once; Delay/Wait ignore the sign of their interval; past the deadline
 *    they return STATUS_TIMEOUT and set TimeoutTriggered.
 *  - SOFTGPU_ESCAPE_PRESENT_STATS counts the DxgkDdiPresent(Blt) calls the
 *    kernel present path makes.  Like Windows' CDD it must Blt only the
 *    rectangles GDI dirtied: an idle desktop gets no full-screen Blts and a
 *    small GDI draw a small Blt.
 *  - SOFTGPU_ESCAPE_SEGMENT returns the segment softgpu reported through
 *    DXGKQAITYPE_QUERYSEGMENT3, which must be what
 *    KMTQAITYPE_GETSEGMENTSIZE then reports as dedicated video memory.
 *
 * Other adapters do not know the escapes; the tests skip there.
 */

#include "precomp.h"
#include <winuser.h>
#include "../../../../drivers/directx/softgpu/softgpu_escape.h"

#define MS(x) ((long long)(x) * 10000)

static D3DKMT_HANDLE g_hAdapter, g_hDevice;

static NTSTATUS
SoftGpuEscape(void *Data, UINT Size)
{
    PFN_D3DKMTEscape pfnEscape = (PFN_D3DKMTEscape)LoadD3DKMTProc("D3DKMTEscape");
    D3DKMT_ESCAPE Escape;

    if (!pfnEscape)
        return STATUS_PROCEDURE_NOT_FOUND;

    memset(&Escape, 0, sizeof(Escape));
    Escape.hAdapter = g_hAdapter;
    Escape.hDevice = g_hDevice;
    Escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    Escape.pPrivateDriverData = Data;
    Escape.PrivateDriverDataSize = Size;
    return pfnEscape(&Escape);
}

static BOOL
RunTimedOp(ULONG Scenario, SOFTGPU_ESCAPE_TIMEDOP_DATA *Data)
{
    NTSTATUS Status;

    memset(Data, 0, sizeof(*Data));
    Data->Code = SOFTGPU_ESCAPE_TIMEDOP;
    Data->Scenario = Scenario;
    Status = SoftGpuEscape(Data, sizeof(*Data));
    ok(NT_SUCCESS(Status), "scenario %lu: escape 0x%08lx\n", Scenario, Status);
    if (!NT_SUCCESS(Status))
        return FALSE;
    ok(Data->QueryStatus == STATUS_SUCCESS,
       "scenario %lu: DxgkCbQueryServices(TimedOperation) 0x%08lx\n",
       Scenario, Data->QueryStatus);
    trace("scenario %lu: start 0x%08lx op 0x%08lx x%lu elapsed %lld00ns (fine %lld00ns) "
          "triggered %u\n",
          Scenario, Data->StartStatus, Data->OpStatus, Data->OpCount,
          Data->Elapsed100ns, Data->FineElapsed100ns, Data->TimeoutTriggered);
    return Data->QueryStatus == STATUS_SUCCESS;
}

static void
Test_TimedOperation(void)
{
    SOFTGPU_ESCAPE_TIMEDOP_DATA d;

    /* A zero timeout is accepted and is already expired. */
    if (RunTimedOp(SOFTGPU_TIMEDOP_ZERO_TIMEOUT, &d))
    {
        ok(d.StartStatus == STATUS_SUCCESS, "Start(0) returned 0x%08lx\n", d.StartStatus);
        ok(d.OpStatus == STATUS_TIMEOUT, "Delay after Start(0) returned 0x%08lx\n", d.OpStatus);
        ok(d.TimeoutTriggered, "TimeoutTriggered not set after Start(0)\n");
    }

    /* Delay(NULL) is a poll: it returns at once. */
    if (RunTimedOp(SOFTGPU_TIMEDOP_POLL, &d))
    {
        ok(d.StartStatus == STATUS_SUCCESS, "Start(2 s) returned 0x%08lx\n", d.StartStatus);
        ok(d.OpStatus == STATUS_SUCCESS, "Delay(NULL) returned 0x%08lx\n", d.OpStatus);
        ok(d.FineElapsed100ns < MS(5), "Delay(NULL) took %lld00ns, expected no wait\n",
           d.FineElapsed100ns);
        ok(!d.TimeoutTriggered, "TimeoutTriggered set before the deadline\n");
    }

    /* The sign of a timeout or interval is ignored. */
    if (RunTimedOp(SOFTGPU_TIMEDOP_NEGATIVE_TIMEOUT, &d))
    {
        ok(d.StartStatus == STATUS_SUCCESS, "Start(-2 s) returned 0x%08lx\n", d.StartStatus);
        ok(d.OpStatus == STATUS_SUCCESS, "Delay after Start(-2 s) returned 0x%08lx\n",
           d.OpStatus);
    }
    if (RunTimedOp(SOFTGPU_TIMEDOP_POSITIVE_INTERVAL, &d))
    {
        ok(d.StartStatus == STATUS_SUCCESS, "Start(2 s) returned 0x%08lx\n", d.StartStatus);
        ok(d.OpStatus == STATUS_SUCCESS, "Delay(+20 ms) returned 0x%08lx\n", d.OpStatus);
        ok(d.FineElapsed100ns >= MS(15), "Delay(+20 ms) took only %lld00ns\n",
           d.FineElapsed100ns);
    }

    /* The driver must preset Op->Size. */
    if (RunTimedOp(SOFTGPU_TIMEDOP_BAD_SIZE, &d))
    {
        ok(d.StartStatus == STATUS_INVALID_PARAMETER,
           "Start with Size 0 returned 0x%08lx\n", d.StartStatus);
    }

    /* Polling past the deadline ends in STATUS_TIMEOUT. */
    if (RunTimedOp(SOFTGPU_TIMEDOP_DEADLINE, &d))
    {
        ok(d.StartStatus == STATUS_SUCCESS, "Start(100 ms) returned 0x%08lx\n", d.StartStatus);
        ok(d.OpStatus == STATUS_TIMEOUT, "poll loop ended with 0x%08lx\n", d.OpStatus);
        ok(d.Elapsed100ns >= MS(95) && d.Elapsed100ns < MS(1000),
           "poll loop ended after %lld00ns, expected ~100 ms\n", d.Elapsed100ns);
        ok(d.TimeoutTriggered, "TimeoutTriggered not set at the deadline\n");
    }

    /* Longer than DXGK_TIMED_OPERATION_TIMEOUT_MAX_SECONDS is clamped to 5 s. */
    if (RunTimedOp(SOFTGPU_TIMEDOP_CLAMP, &d))
    {
        ok(d.StartStatus == STATUS_SUCCESS, "Start(10 s) returned 0x%08lx\n", d.StartStatus);
        ok(d.OpStatus == STATUS_TIMEOUT, "Wait(NULL) returned 0x%08lx\n", d.OpStatus);
        ok(d.Elapsed100ns >= MS(4900) && d.Elapsed100ns < MS(6000),
           "Start(10 s) expired after %lld00ns, expected the 5 s maximum\n", d.Elapsed100ns);
    }
}

static void
Test_SegmentSize(void)
{
    PFN_D3DKMTQueryAdapterInfo pfnQAI =
        (PFN_D3DKMTQueryAdapterInfo)LoadD3DKMTProc("D3DKMTQueryAdapterInfo");
    SOFTGPU_ESCAPE_SEGMENT_DATA Seg;
    D3DKMT_SEGMENTSIZEINFO SizeInfo;
    D3DKMT_QUERYADAPTERINFO Query;
    NTSTATUS Status;

    if (!pfnQAI)
    {
        skip("D3DKMTQueryAdapterInfo not exported\n");
        return;
    }

    memset(&Seg, 0, sizeof(Seg));
    Seg.Code = SOFTGPU_ESCAPE_SEGMENT;
    Status = SoftGpuEscape(&Seg, sizeof(Seg));
    ok(NT_SUCCESS(Status), "segment escape 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;
    trace("softgpu segment: flags 0x%lx base 0x%llx cpu 0x%llx size 0x%llx vram %lu\n",
          Seg.Flags, Seg.BaseAddress, Seg.CpuTranslatedAddress, Seg.Size, Seg.HasVram);

    memset(&SizeInfo, 0, sizeof(SizeInfo));
    memset(&Query, 0, sizeof(Query));
    Query.hAdapter = g_hAdapter;
    Query.Type = KMTQAITYPE_GETSEGMENTSIZE;
    Query.pPrivateDriverData = &SizeInfo;
    Query.PrivateDriverDataSize = sizeof(SizeInfo);
    Status = pfnQAI(&Query);
    ok(NT_SUCCESS(Status), "GETSEGMENTSIZE 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;
    trace("GETSEGMENTSIZE: video 0x%llx system 0x%llx shared 0x%llx\n",
          SizeInfo.DedicatedVideoMemorySize, SizeInfo.DedicatedSystemMemorySize,
          SizeInfo.SharedSystemMemorySize);

    /* What dxgkrnl parsed from the miniport's segment descriptor. */
    if (Seg.HasVram)
    {
        ok(SizeInfo.DedicatedVideoMemorySize == Seg.Size,
           "dedicated video memory 0x%llx, softgpu reported a 0x%llx-byte memory segment\n",
           SizeInfo.DedicatedVideoMemorySize, Seg.Size);
    }
    else
    {
        ok(SizeInfo.DedicatedVideoMemorySize == 0,
           "dedicated video memory 0x%llx for an aperture-only adapter\n",
           SizeInfo.DedicatedVideoMemorySize);
        ok(SizeInfo.SharedSystemMemorySize == Seg.Size,
           "shared memory 0x%llx, softgpu reported a 0x%llx-byte aperture\n",
           SizeInfo.SharedSystemMemorySize, Seg.Size);
    }
}

static BOOL
GetPresentStats(SOFTGPU_ESCAPE_PRESENT_STATS_DATA *Stats)
{
    memset(Stats, 0, sizeof(*Stats));
    Stats->Code = SOFTGPU_ESCAPE_PRESENT_STATS;
    return NT_SUCCESS(SoftGpuEscape(Stats, sizeof(*Stats)));
}

static void
Test_DirtyRectPresent(void)
{
    SOFTGPU_ESCAPE_PRESENT_STATS_DATA s0, s1, s2;
    HBRUSH Brush;
    RECT Rect = { 40, 40, 72, 56 };
    HDC hdc;

    if (!GetPresentStats(&s0))
    {
        ok(0, "present statistics escape failed\n");
        return;
    }
    if (s0.BltCount == 0)
    {
        skip("the kernel GPU present path is not active\n");
        return;
    }

    /* An idle desktop is not re-presented as a whole. */
    Sleep(2000);
    ok(GetPresentStats(&s1), "present statistics escape failed\n");
    trace("idle 2 s: %lu -> %lu Blts, %lu -> %lu full-screen\n",
          s0.BltCount, s1.BltCount, s0.FullBltCount, s1.FullBltCount);
    ok(s1.FullBltCount == s0.FullBltCount,
       "%lu full-screen Blts while idle\n", s1.FullBltCount - s0.FullBltCount);

    /* A small GDI draw is presented as a small Blt. */
    hdc = GetDC(NULL);
    Brush = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(hdc, &Rect, Brush);
    DeleteObject(Brush);
    ReleaseDC(NULL, hdc);
    Sleep(500);

    ok(GetPresentStats(&s2), "present statistics escape failed\n");
    trace("after FillRect: %lu Blts (+%lu), last (%ld,%ld)-(%ld,%ld) of %lux%lu\n",
          s2.BltCount, s2.BltCount - s1.BltCount, s2.LastLeft, s2.LastTop,
          s2.LastRight, s2.LastBottom, s2.DstWidth, s2.DstHeight);
    ok(s2.BltCount > s1.BltCount, "the draw was not presented\n");
    ok(s2.FullBltCount == s1.FullBltCount,
       "the 32x16 draw was presented with a full-screen Blt\n");
    ok((unsigned long long)(s2.LastRight - s2.LastLeft) * (s2.LastBottom - s2.LastTop) * 4 <=
           (unsigned long long)s2.DstWidth * s2.DstHeight,
       "last Blt (%ld,%ld)-(%ld,%ld) covers more than a quarter of %lux%lu\n",
       s2.LastLeft, s2.LastTop, s2.LastRight, s2.LastBottom, s2.DstWidth, s2.DstHeight);
}

/* The kernel adapter behind the primary display (D3DKMTOpenAdapterFromHdc
 * reaches dxgkrnl; the GDI display name may be opened in user mode only). */
static D3DKMT_HANDLE
OpenPrimaryAdapter(void)
{
    PFN_D3DKMTOpenAdapterFromHdc pfn =
        (PFN_D3DKMTOpenAdapterFromHdc)LoadD3DKMTProc("D3DKMTOpenAdapterFromHdc");
    D3DKMT_OPENADAPTERFROMHDC Data;
    NTSTATUS Status;

    if (!pfn)
        return 0;
    memset(&Data, 0, sizeof(Data));
    Data.hDc = GetDC(NULL);
    Status = pfn(&Data);
    ReleaseDC(NULL, Data.hDc);
    return NT_SUCCESS(Status) ? Data.hAdapter : 0;
}

START_TEST(softgpu)
{
    SOFTGPU_ESCAPE_SEGMENT_DATA Probe;

    g_hAdapter = OpenPrimaryAdapter();
    if (!g_hAdapter)
    {
        skip("no WDDM adapter behind the primary display\n");
        return;
    }
    g_hDevice = CreateTestDevice(g_hAdapter);

    memset(&Probe, 0, sizeof(Probe));
    Probe.Code = SOFTGPU_ESCAPE_SEGMENT;
    if (!g_hDevice || !NT_SUCCESS(SoftGpuEscape(&Probe, sizeof(Probe))))
    {
        skip("the primary display is not driven by softgpu (device %x)\n", g_hDevice);
    }
    else
    {
        Test_SegmentSize();
        Test_DirtyRectPresent();
        Test_TimedOperation();
    }

    DestroyTestDevice(g_hDevice);
    CloseAdapter(g_hAdapter);
}
