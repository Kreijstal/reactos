/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test SetTimer/KillTimer on threads that are preempted and exit while the timers run
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "precomp.h"

/*
 * win32k counts the timers down on the raw input thread, which takes only
 * the timer lock, not the user lock.  A timer must therefore be complete --
 * its owning thread, window and countdown set -- before that thread can see
 * it, and must be gone from the list before its thread or window is freed.
 *
 * On every CPU a setter thread at idle priority keeps creating and killing
 * window-less and window timers, and spawns short-lived threads that exit
 * (or destroy their window) with their timers still set.  A time-critical
 * spinner pinned to the same CPU wakes up every few milliseconds and then
 * runs for longer than two timer ticks, so the setter is preempted at random
 * points inside SetTimer and stays off the CPU while the raw input thread
 * walks the timer list.
 */

#define TEST_DURATION_MS 20000
#define SPIN_MS 30
#define MAX_CPUS 8
#define TIMER_ELAPSE 10

typedef struct _CPU_CONTEXT
{
    DWORD_PTR Affinity;
    LONG SetTimerCalls;
    LONG SetTimerFailures;
    LONG KillTimerFailures;
    LONG ExitingThreads;
} CPU_CONTEXT, *PCPU_CONTEXT;

static volatile LONG g_Stop;
static LONG g_TimerMessages;
static const WCHAR g_ClassName[] = L"SetTimerTestClass";

static
VOID
CALLBACK
TimerProc(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    InterlockedIncrement(&g_TimerMessages);
}

static
LRESULT
CALLBACK
WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_TIMER)
    {
        InterlockedIncrement(&g_TimerMessages);
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static
VOID
PumpMessages(VOID)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

/* Sets timers and exits without killing them: win32k must destroy them with the thread */
static
DWORD
WINAPI
ExitingThread(PVOID Parameter)
{
    PCPU_CONTEXT Context = Parameter;
    HWND hWnd;
    UINT_PTR Id;
    ULONG i;

    hWnd = CreateWindowExW(0, g_ClassName, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    for (i = 0; i < 4; i++)
    {
        Id = SetTimer(NULL, 0, TIMER_ELAPSE, TimerProc);
        InterlockedIncrement(&Context->SetTimerCalls);
        if (!Id)
            InterlockedIncrement(&Context->SetTimerFailures);

        if (hWnd)
        {
            Id = SetTimer(hWnd, i + 1, TIMER_ELAPSE, NULL);
            InterlockedIncrement(&Context->SetTimerCalls);
            if (Id != i + 1)
                InterlockedIncrement(&Context->SetTimerFailures);
        }
    }
    Sleep(TIMER_ELAPSE * 2);
    PumpMessages();

    /* Every other thread destroys its window first, the rest leave it to thread cleanup */
    if (hWnd && (GetCurrentThreadId() & 4))
        DestroyWindow(hWnd);

    return 0;
}

static
DWORD
WINAPI
SetterThread(PVOID Parameter)
{
    PCPU_CONTEXT Context = Parameter;
    HWND hWnd;
    HANDLE hThread;
    UINT_PTR Id;
    ULONG Iteration = 0;

    SetThreadAffinityMask(GetCurrentThread(), Context->Affinity);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);

    hWnd = CreateWindowExW(0, g_ClassName, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    ok(hWnd != NULL, "CreateWindowExW failed: %lu\n", GetLastError());

    while (!g_Stop)
    {
        Id = SetTimer(NULL, 0, TIMER_ELAPSE, TimerProc);
        InterlockedIncrement(&Context->SetTimerCalls);
        if (!Id)
            InterlockedIncrement(&Context->SetTimerFailures);
        else if (!KillTimer(NULL, Id))
            InterlockedIncrement(&Context->KillTimerFailures);

        if (hWnd)
        {
            Id = SetTimer(hWnd, (Iteration & 7) + 1, TIMER_ELAPSE, NULL);
            InterlockedIncrement(&Context->SetTimerCalls);
            if (Id != (Iteration & 7) + 1)
                InterlockedIncrement(&Context->SetTimerFailures);
            else if (!KillTimer(hWnd, Id))
                InterlockedIncrement(&Context->KillTimerFailures);
        }

        if ((Iteration % 128) == 0)
        {
            hThread = CreateThread(NULL, 0, ExitingThread, Context, CREATE_SUSPENDED, NULL);
            if (hThread)
            {
                SetThreadAffinityMask(hThread, Context->Affinity);
                SetThreadPriority(hThread, THREAD_PRIORITY_IDLE);
                ResumeThread(hThread);
                WaitForSingleObject(hThread, INFINITE);
                CloseHandle(hThread);
                InterlockedIncrement(&Context->ExitingThreads);
            }
            PumpMessages();
        }
        Iteration++;
    }

    if (hWnd)
        DestroyWindow(hWnd);
    return 0;
}

static
DWORD
WINAPI
SpinnerThread(PVOID Parameter)
{
    PCPU_CONTEXT Context = Parameter;
    DWORD Start;

    SetThreadAffinityMask(GetCurrentThread(), Context->Affinity);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (!g_Stop)
    {
        /* Wake up (preempting the setter wherever it is), then keep it off the CPU */
        Sleep(1);
        Start = GetTickCount();
        while (!g_Stop && GetTickCount() - Start < SPIN_MS)
            YieldProcessor();
    }
    return 0;
}

static
VOID
TestTimerRace(VOID)
{
    CPU_CONTEXT Contexts[MAX_CPUS];
    HANDLE Threads[2 * MAX_CPUS];
    SYSTEM_INFO SystemInfo;
    ULONG Cpus, i, Count = 0;
    LONG Calls = 0, SetFailures = 0, KillFailures = 0, Exits = 0;
    DWORD Wait;

    GetSystemInfo(&SystemInfo);
    Cpus = min(SystemInfo.dwNumberOfProcessors, MAX_CPUS);

    ZeroMemory(Contexts, sizeof(Contexts));
    g_Stop = FALSE;
    for (i = 0; i < Cpus; i++)
    {
        Contexts[i].Affinity = (DWORD_PTR)1 << i;
        Threads[Count] = CreateThread(NULL, 0, SetterThread, &Contexts[i], 0, NULL);
        ok(Threads[Count] != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (Threads[Count])
            Count++;
        Threads[Count] = CreateThread(NULL, 0, SpinnerThread, &Contexts[i], 0, NULL);
        ok(Threads[Count] != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (Threads[Count])
            Count++;
    }

    Sleep(TEST_DURATION_MS);
    InterlockedExchange(&g_Stop, TRUE);

    Wait = WaitForMultipleObjects(Count, Threads, TRUE, 60000);
    ok(Wait == WAIT_OBJECT_0, "Threads did not stop: %lu\n", Wait);
    for (i = 0; i < Count; i++)
        CloseHandle(Threads[i]);

    for (i = 0; i < Cpus; i++)
    {
        Calls += Contexts[i].SetTimerCalls;
        SetFailures += Contexts[i].SetTimerFailures;
        KillFailures += Contexts[i].KillTimerFailures;
        Exits += Contexts[i].ExitingThreads;
    }
    trace("%lu CPUs: %ld SetTimer calls, %ld exiting threads, %ld timer messages\n",
          Cpus, Calls, Exits, g_TimerMessages);
    ok(Calls > 0, "No SetTimer call was made\n");
    ok(Exits > 0, "No exiting thread was run\n");
    ok(SetFailures == 0, "%ld SetTimer calls failed\n", SetFailures);
    ok(KillFailures == 0, "%ld KillTimer calls failed\n", KillFailures);
}

/* A timer that is set fires, and stops once it is killed */
static
VOID
TestTimerFires(VOID)
{
    HWND hWnd;
    UINT_PTR Id;
    DWORD Start;
    LONG Before;

    hWnd = CreateWindowExW(0, g_ClassName, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    ok(hWnd != NULL, "CreateWindowExW failed: %lu\n", GetLastError());
    if (!hWnd)
        return;

    g_TimerMessages = 0;
    Id = SetTimer(hWnd, 1, TIMER_ELAPSE, NULL);
    ok(Id == 1, "SetTimer returned %Iu\n", Id);
    Start = GetTickCount();
    while (g_TimerMessages < 3 && GetTickCount() - Start < 5000)
    {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT);
        PumpMessages();
    }
    ok(g_TimerMessages >= 3, "Got %ld WM_TIMER messages\n", g_TimerMessages);
    ok(KillTimer(hWnd, 1), "KillTimer failed: %lu\n", GetLastError());

    PumpMessages();
    Before = g_TimerMessages;
    Sleep(TIMER_ELAPSE * 10);
    PumpMessages();
    ok(g_TimerMessages == Before, "Got %ld WM_TIMER messages after KillTimer\n", g_TimerMessages - Before);

    DestroyWindow(hWnd);
}

START_TEST(SetTimer)
{
    WNDCLASSW wc = { 0 };

    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = g_ClassName;
    ok(RegisterClassW(&wc) != 0, "RegisterClassW failed: %lu\n", GetLastError());

    TestTimerFires();
    TestTimerRace();

    UnregisterClassW(g_ClassName, NULL);
}
