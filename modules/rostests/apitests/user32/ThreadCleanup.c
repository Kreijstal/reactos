/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that a thread's USER resources are released when it exits
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include "precomp.h"

#include <ndk/exfuncs.h>
#include <ndk/extypes.h>

/*
 * Every thread that touches USER gets a THREADINFO, a message queue, a
 * CLIENTTHREADINFO on the desktop heap and a reference on its desktop.  When
 * the thread exits, all of that must go away, together with whatever USER
 * objects the thread left behind (windows, hooks, timers, input attachments).
 *
 * The workers run on a private desktop so that its heap can be measured: the
 * capacity of the heap is how much window text fits into it before an
 * allocation fails.  A CLIENTTHREADINFO (or anything bigger) left behind by
 * each of a thousand threads takes a hundred kilobytes or more out of it.
 * The outstanding win32k (Us*) pool allocations are counted as well, and no
 * window may survive the thread that created it.
 */

#define THREADS_PER_MODE    1000
#define BATCH               25

#define FILL_BIG_CHARS      32000
#define FILL_MID_CHARS      1000
#define FILL_SMALL_CHARS    16

static const WCHAR TestDesktopName[] = L"ThreadCleanupTestDesktop";
static const WCHAR FillClassName[] = L"ThreadCleanupFill";
static const WCHAR WorkClassName[] = L"ThreadCleanupWork";

static HDESK hTestDesktop;
static DWORD AnchorThreadId;
static WCHAR FillText[FILL_BIG_CHARS + 1];

typedef enum _WORK_MODE
{
    ModePeekMessage,
    ModeCreateDestroyWindow,
    ModeLeaveWindow,
    ModeLeaveHook,
    ModeFocusActivate,
    ModeLeaveAttachment,
    ModeLeaveTimer,
    ModeGetMessageQuit,
    ModeCount
} WORK_MODE;

static const char *ModeNames[ModeCount] =
{
    "PeekMessage",
    "create+destroy window",
    "window left at exit",
    "hook left at exit",
    "SetFocus/SetActiveWindow",
    "AttachThreadInput left at exit",
    "timer left at exit",
    "GetMessage(WM_QUIT)",
};

static LRESULT CALLBACK
HookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static HWND
CreateWorkWindow(void)
{
    return CreateWindowExW(0, WorkClassName, L"work", WS_OVERLAPPEDWINDOW,
                           0, 0, 50, 50, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static DWORD WINAPI
WorkerThread(PVOID Param)
{
    WORK_MODE Mode = (WORK_MODE)(ULONG_PTR)Param;
    MSG msg;
    HWND hwnd;

    if (!SetThreadDesktop(hTestDesktop))
        return 1;

    switch (Mode)
    {
        case ModePeekMessage:
            PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
            break;

        case ModeCreateDestroyWindow:
            hwnd = CreateWorkWindow();
            if (!hwnd)
                return 2;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
                DispatchMessageW(&msg);
            DestroyWindow(hwnd);
            break;

        case ModeLeaveWindow:
            if (!CreateWorkWindow())
                return 2;
            break;

        case ModeLeaveHook:
            if (!SetWindowsHookExW(WH_GETMESSAGE, HookProc, NULL, GetCurrentThreadId()))
                return 2;
            PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
            break;

        case ModeFocusActivate:
            hwnd = CreateWorkWindow();
            if (!hwnd)
                return 2;
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
            PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
            break;

        case ModeLeaveAttachment:
            PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
            if (!AttachThreadInput(GetCurrentThreadId(), AnchorThreadId, TRUE))
                return 2;
            break;

        case ModeLeaveTimer:
            hwnd = CreateWorkWindow();
            if (!hwnd)
                return 2;
            if (!SetTimer(hwnd, 1, 100000, NULL))
                return 3;
            if (!SetTimer(NULL, 0, 100000, NULL))
                return 4;
            break;

        case ModeGetMessageQuit:
            PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, 0, 0);
            while (GetMessageW(&msg, NULL, 0, 0) > 0)
                DispatchMessageW(&msg);
            break;

        default:
            return 5;
    }

    return 0;
}

static DWORD WINAPI
AnchorThread(PVOID Param)
{
    HANDLE hReady = Param;
    MSG msg;

    if (!SetThreadDesktop(hTestDesktop))
        return 1;
    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    SetEvent(hReady);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}

/* Fill the test desktop heap with window text until it is full.  Returns the
   number of bytes that fitted. */
static SIZE_T
MeasureOneSize(HWND *Windows, ULONG *Count, ULONG Max, ULONG Chars)
{
    SIZE_T Bytes = 0;
    HWND hwnd;

    FillText[Chars] = UNICODE_NULL;
    while (*Count < Max)
    {
        hwnd = CreateWindowExW(0, FillClassName, NULL, WS_POPUP, 0, 0, 1, 1,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!hwnd)
            break;
        Windows[(*Count)++] = hwnd;
        if (!Chars)
        {
            Bytes += 1;
            continue;
        }

        /* Read the length back from the desktop heap rather than trusting
           the return value: DefWindowProc's WM_SETTEXT reports success even
           when the text could not be allocated */
        SetWindowTextW(hwnd, FillText);
        if (GetWindowTextLengthW(hwnd) != (int)Chars)
            break;
        Bytes += Chars * sizeof(WCHAR);
    }
    if (Chars < FILL_BIG_CHARS)
        FillText[Chars] = L'x';
    return Bytes;
}

static DWORD WINAPI
MeasureThread(PVOID Param)
{
    SIZE_T *Result = Param;
    const ULONG Max = 9000;
    HWND *Windows;
    ULONG Count = 0, i;

    *Result = 0;
    if (!SetThreadDesktop(hTestDesktop))
        return 1;
    Windows = HeapAlloc(GetProcessHeap(), 0, Max * sizeof(HWND));
    if (!Windows)
        return 2;

    *Result += MeasureOneSize(Windows, &Count, Max, FILL_BIG_CHARS);
    *Result += MeasureOneSize(Windows, &Count, Max, FILL_MID_CHARS);
    *Result += MeasureOneSize(Windows, &Count, Max, FILL_SMALL_CHARS);

    for (i = 0; i < Count; i++)
        DestroyWindow(Windows[i]);
    HeapFree(GetProcessHeap(), 0, Windows);
    return 0;
}

static SIZE_T
MeasureDesktopHeap(void)
{
    SIZE_T Result = 0;
    HANDLE hThread;
    DWORD ExitCode = 0xFFFFFFFF;

    hThread = CreateThread(NULL, 0, MeasureThread, &Result, 0, NULL);
    ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!hThread)
        return 0;
    WaitForSingleObject(hThread, INFINITE);
    GetExitCodeThread(hThread, &ExitCode);
    CloseHandle(hThread);
    ok(ExitCode == 0, "Measure thread failed: %lu\n", ExitCode);
    return Result;
}

/* Outstanding allocations of the win32k "Us??" pool tags */
static LONG
CountUserPoolAllocations(void)
{
    PSYSTEM_POOLTAG_INFORMATION Info;
    ULONG Size = 0x10000, i;
    NTSTATUS Status;
    LONG Count = 0;

    for (;;)
    {
        Info = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Info)
            return -1;
        Status = NtQuerySystemInformation(SystemPoolTagInformation, Info, Size, &Size);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            break;
        HeapFree(GetProcessHeap(), 0, Info);
        Size += 0x1000;
    }
    if (!NT_SUCCESS(Status))
    {
        HeapFree(GetProcessHeap(), 0, Info);
        return -1;
    }

    for (i = 0; i < Info->Count; i++)
    {
        PSYSTEM_POOLTAG Tag = &Info->TagInfo[i];
        if (Tag->Tag[0] == 'U' && Tag->Tag[1] == 's')
        {
            Count += (LONG)(Tag->PagedAllocs - Tag->PagedFrees);
            Count += (LONG)(Tag->NonPagedAllocs - Tag->NonPagedFrees);
        }
    }
    HeapFree(GetProcessHeap(), 0, Info);
    return Count;
}

static BOOL CALLBACK
CountWindowProc(HWND hwnd, LPARAM lParam)
{
    (*(ULONG *)lParam)++;
    return TRUE;
}

/* Top-level windows left on the test desktop */
static ULONG
CountDesktopWindows(void)
{
    ULONG Count = 0;
    EnumDesktopWindows(hTestDesktop, CountWindowProc, (LPARAM)&Count);
    return Count;
}

static ULONG
RunThreads(WORK_MODE Mode, ULONG Total)
{
    HANDLE Threads[BATCH];
    ULONG Started, i, n, Failed = 0;
    DWORD ExitCode;

    for (Started = 0; Started < Total; Started += n)
    {
        n = min(BATCH, Total - Started);
        for (i = 0; i < n; i++)
        {
            Threads[i] = CreateThread(NULL, 0, WorkerThread, (PVOID)(ULONG_PTR)Mode, 0, NULL);
            if (!Threads[i])
            {
                ok(0, "CreateThread failed: %lu\n", GetLastError());
                n = i;
                break;
            }
        }
        if (!n)
            break;
        WaitForMultipleObjects(n, Threads, TRUE, INFINITE);
        for (i = 0; i < n; i++)
        {
            if (!GetExitCodeThread(Threads[i], &ExitCode) || ExitCode != 0)
                Failed++;
            CloseHandle(Threads[i]);
        }
    }
    return Failed;
}

START_TEST(ThreadCleanup)
{
    WNDCLASSW wc = { 0 };
    HANDLE hReady, hAnchor;
    SIZE_T Base1, Base2, Before, After, Noise, Allowed;
    LONG PoolBefore, PoolAfter;
    ULONG Failed, Mode, i;

    for (i = 0; i < FILL_BIG_CHARS; i++)
        FillText[i] = L'x';

    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = FillClassName;
    ok(RegisterClassW(&wc) != 0, "RegisterClass failed: %lu\n", GetLastError());
    wc.lpszClassName = WorkClassName;
    ok(RegisterClassW(&wc) != 0, "RegisterClass failed: %lu\n", GetLastError());

    hTestDesktop = CreateDesktopW(TestDesktopName, NULL, NULL, 0, DESKTOP_ALL_ACCESS, NULL);
    ok(hTestDesktop != NULL, "CreateDesktop failed: %lu\n", GetLastError());
    if (!hTestDesktop)
        return;

    hReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    hAnchor = CreateThread(NULL, 0, AnchorThread, hReady, 0, &AnchorThreadId);
    ok(hAnchor != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!hAnchor)
        return;
    WaitForSingleObject(hReady, INFINITE);

    /* Warm up, then take two baselines to see the measurement noise */
    RunThreads(ModeCreateDestroyWindow, 50);
    Base1 = MeasureDesktopHeap();
    Base2 = MeasureDesktopHeap();
    Noise = (Base1 > Base2) ? Base1 - Base2 : Base2 - Base1;
    trace("Desktop heap capacity %Iu / %Iu bytes\n", Base1, Base2);
    ok(Base1 > 64 * 1024, "Desktop heap capacity is only %Iu bytes\n", Base1);
    ok(CountUserPoolAllocations() > 0, "Cannot count the win32k pool allocations\n");

    /* The capacity moves by a few dozen kilobytes from one fill to the next
       (where the big texts happen to fit).  A CLIENTTHREADINFO or anything
       bigger left behind by every thread is well above this allowance. */
    Allowed = Noise + 48 * 1024;

    for (Mode = 0; Mode < ModeCount; Mode++)
    {
        PoolBefore = CountUserPoolAllocations();
        Before = MeasureDesktopHeap();
        Failed = RunThreads((WORK_MODE)Mode, THREADS_PER_MODE);
        After = MeasureDesktopHeap();
        PoolAfter = CountUserPoolAllocations();

        ok(Failed == 0, "%s: %lu of %u threads failed\n", ModeNames[Mode], Failed, THREADS_PER_MODE);
        ok(CountDesktopWindows() == 0, "%s: %lu windows survived their threads\n",
           ModeNames[Mode], CountDesktopWindows());
        trace("%s: desktop heap capacity %Iu -> %Iu, Us pool allocations %ld -> %ld\n",
              ModeNames[Mode], Before, After, PoolBefore, PoolAfter);
        ok(After + Allowed >= Before,
           "%s: %u threads leaked %Iu bytes of desktop heap (capacity %Iu -> %Iu)\n",
           ModeNames[Mode], THREADS_PER_MODE, Before - After, Before, After);
        if (PoolBefore >= 0 && PoolAfter >= 0)
        {
            ok(PoolAfter - PoolBefore < THREADS_PER_MODE / 4,
               "%s: %u threads leaked %ld win32k pool allocations (%ld -> %ld)\n",
               ModeNames[Mode], THREADS_PER_MODE, PoolAfter - PoolBefore, PoolBefore, PoolAfter);
        }
    }

    PostThreadMessageW(AnchorThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(hAnchor, INFINITE);
    CloseHandle(hAnchor);
    CloseHandle(hReady);
    ok(CloseDesktop(hTestDesktop), "CloseDesktop failed: %lu\n", GetLastError());
    UnregisterClassW(WorkClassName, GetModuleHandleW(NULL));
    UnregisterClassW(FillClassName, GetModuleHandleW(NULL));
}
