/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode process create/kill churn stress. Models the
 *              rosautotest journal-building workload that intermittently
 *              trips the MM VAD-table locking asserts
 *              (ntoskrnl/mm/ARM3/vadnode.c MiDbgAssertIsLockedForRead) on
 *              SMP: many short-lived processes, killed at random points
 *              during image mapping / loader init, while other threads
 *              query the dying process' address space (VirtualQueryEx /
 *              ReadProcessMemory attach + VAD walks) and churn the own
 *              process' VAD tree with map/unmap storms. A correct kernel
 *              survives; a teardown-vs-query race asserts or bugchecks.
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 */

#include <kmt_test.h>

#define WIN32_NO_STATUS
#include <windows.h>

#define STRESS_SECONDS 55
#define MAX_SPAWNERS   8
#define MAX_VMCHURN    4

static volatile LONG g_Stop;
static volatile LONG g_Spawned;
static volatile LONG g_Killed;
static volatile LONG g_Queried;
static volatile LONG g_VmOps;
static WCHAR g_SelfPath[MAX_PATH];
static WCHAR g_CmdPath[MAX_PATH];

static
ULONG
NextRand(PULONG Seed)
{
    ULONG x = *Seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *Seed = x;
    return x;
}

/* Query the (possibly dying) process' address space: NtQueryVirtualMemory
 * attaches to the target and reads its VAD tree, racing teardown. */
static
VOID
QueryProcessSpace(HANDLE Process, PULONG Seed)
{
    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR addr = 0;
    ULONG steps = 32 + (NextRand(Seed) & 63);
    CHAR buf[64];
    SIZE_T got;

    while (steps--)
    {
        if (!VirtualQueryEx(Process, (PVOID)addr, &mbi, sizeof(mbi)))
            break;
        if (mbi.State == MEM_COMMIT)
        {
            /* Fault a read through the attach path too */
            ReadProcessMemory(Process, mbi.BaseAddress, buf, sizeof(buf), &got);
        }
        addr = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
        if (addr == 0)
            break;
    }
    InterlockedIncrement(&g_Queried);
}

static
DWORD
WINAPI
SpawnerThread(LPVOID Ctx)
{
    ULONG Seed = (ULONG)(ULONG_PTR)Ctx | 1;

    while (!InterlockedCompareExchange(&g_Stop, 0, 0))
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        WCHAR cmdline[MAX_PATH + 16];
        ULONG r = NextRand(&Seed);
        BOOL ok2;

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);

        /* Alternate children: our own exe enumerating its test list
         * (fast, like rosautotest's "<exe> --list"), or cmd /c exit
         * (bigger image set, more loader work). */
        if (r & 1)
            _snwprintf(cmdline, RTL_NUMBER_OF(cmdline), L"\"%ls\" --list", g_SelfPath);
        else
            _snwprintf(cmdline, RTL_NUMBER_OF(cmdline), L"\"%ls\" /c exit", g_CmdPath);

        ok2 = CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        if (!ok2)
        {
            Sleep(5);
            continue;
        }
        InterlockedIncrement(&g_Spawned);

        switch ((r >> 8) & 3)
        {
            case 0:
                /* Kill immediately: teardown during image mapping */
                TerminateProcess(pi.hProcess, 0);
                InterlockedIncrement(&g_Killed);
                break;
            case 1:
                /* Kill mid-init after 0..40ms */
                Sleep(NextRand(&Seed) % 40);
                QueryProcessSpace(pi.hProcess, &Seed);
                TerminateProcess(pi.hProcess, 0);
                InterlockedIncrement(&g_Killed);
                break;
            case 2:
                /* Query the dying child from here while it exits */
                QueryProcessSpace(pi.hProcess, &Seed);
                TerminateProcess(pi.hProcess, 0);
                InterlockedIncrement(&g_Killed);
                QueryProcessSpace(pi.hProcess, &Seed);
                break;
            default:
                /* Let it run to completion */
                WaitForSingleObject(pi.hProcess, 2000);
                break;
        }

        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

/* VAD insert/remove storm in our own process, concurrent with the
 * spawner threads' cross-process attaches. */
static
DWORD
WINAPI
VmChurnThread(LPVOID Ctx)
{
    ULONG Seed = (ULONG)(ULONG_PTR)Ctx | 1;

    while (!InterlockedCompareExchange(&g_Stop, 0, 0))
    {
        PVOID p[16];
        HANDLE sec[4];
        PVOID view[4];
        ULONG i;

        for (i = 0; i < 16; ++i)
        {
            SIZE_T sz = ((NextRand(&Seed) & 15) + 1) * 0x1000;
            p[i] = VirtualAlloc(NULL, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p[i])
                *(volatile ULONG *)p[i] = i;
        }
        for (i = 0; i < 4; ++i)
        {
            sec[i] = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        0, 0x10000, NULL);
            view[i] = sec[i] ? MapViewOfFile(sec[i], FILE_MAP_WRITE, 0, 0, 0) : NULL;
            if (view[i])
                *(volatile ULONG *)view[i] = i;
        }
        for (i = 0; i < 16; ++i)
            if (p[i]) VirtualFree(p[i], 0, MEM_RELEASE);
        for (i = 0; i < 4; ++i)
        {
            if (view[i]) UnmapViewOfFile(view[i]);
            if (sec[i]) CloseHandle(sec[i]);
        }
        InterlockedIncrement(&g_VmOps);
    }
    return 0;
}

START_TEST(MmProcessChurnStress)
{
    SYSTEM_INFO sysinfo;
    HANDLE spawners[MAX_SPAWNERS] = { NULL };
    HANDLE churners[MAX_VMCHURN] = { NULL };
    ULONG nSpawners, nChurners, i;

    GetSystemInfo(&sysinfo);
    nSpawners = min(2 * sysinfo.dwNumberOfProcessors, MAX_SPAWNERS);
    nChurners = min(sysinfo.dwNumberOfProcessors, MAX_VMCHURN);

    GetModuleFileNameW(NULL, g_SelfPath, MAX_PATH);
    GetSystemDirectoryW(g_CmdPath, MAX_PATH);
    wcscat(g_CmdPath, L"\\cmd.exe");

    g_Stop = 0;
    g_Spawned = g_Killed = g_Queried = g_VmOps = 0;

    trace("Process churn stress: %lu CPUs, %lu spawners, %lu vm-churners, %ds\n",
          sysinfo.dwNumberOfProcessors, nSpawners, nChurners, STRESS_SECONDS);

    for (i = 0; i < nSpawners; ++i)
        spawners[i] = CreateThread(NULL, 0, SpawnerThread,
                                   (LPVOID)(ULONG_PTR)(0xA5A5 + i), 0, NULL);
    for (i = 0; i < nChurners; ++i)
        churners[i] = CreateThread(NULL, 0, VmChurnThread,
                                   (LPVOID)(ULONG_PTR)(0x5A5A + i), 0, NULL);

    Sleep(STRESS_SECONDS * 1000);
    InterlockedExchange(&g_Stop, 1);

    for (i = 0; i < nSpawners; ++i)
    {
        if (spawners[i])
        {
            WaitForSingleObject(spawners[i], 15000);
            CloseHandle(spawners[i]);
        }
    }
    for (i = 0; i < nChurners; ++i)
    {
        if (churners[i])
        {
            WaitForSingleObject(churners[i], 15000);
            CloseHandle(churners[i]);
        }
    }

    trace("Stopped: spawned=%ld killed=%ld queried=%ld vmops=%ld\n",
          g_Spawned, g_Killed, g_Queried, g_VmOps);

    ok(g_Spawned > 0, "no processes spawned (%ld)\n", g_Spawned);
    ok(TRUE, "survived process churn stress\n");
}
