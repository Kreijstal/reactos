/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for NtOpenProcess / NtOpenThread on processes and threads being created
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A process or thread gets its client ID before the object manager inserts
 * it, and IDs are reused right away. Openers that guess IDs (anything that
 * keeps an old process ID around) must not reach an object that is still
 * being created: that would hand out a handle to a half-made object, and
 * the creator's insert would then find an object that already has handles
 * and fail. Here openers spin over the IDs the creators had last while the
 * creators create and destroy processes and threads; every creation must
 * succeed.
 */

#include "precomp.h"

#define CREATIONS 200
#define RECENT_IDS 16

static volatile LONG Done;
static volatile ULONG_PTR RecentIds[RECENT_IDS];
static volatile LONG RecentIndex;
static volatile LONG Opened;

static
VOID
RememberId(
    _In_ ULONG_PTR Id)
{
    LONG Index = InterlockedIncrement(&RecentIndex);
    RecentIds[Index % RECENT_IDS] = Id;
}

static
DWORD
WINAPI
OpenerThread(
    _In_ LPVOID Parameter)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    CLIENT_ID ClientId;
    HANDLE Handle;
    NTSTATUS Status;
    ULONG_PTR Id;
    ULONG i;
    LONG Delta;

    UNREFERENCED_PARAMETER(Parameter);
    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);

    while (!Done)
    {
        for (i = 0; i < RECENT_IDS; i++)
        {
            /* The IDs that were just freed, and their neighbours */
            for (Delta = -8; Delta <= 8; Delta += 4)
            {
                Id = RecentIds[i] + Delta;
                if (Id == 0 || Id == (ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess)
                    continue;

                ClientId.UniqueProcess = (HANDLE)Id;
                ClientId.UniqueThread = NULL;
                Status = NtOpenProcess(&Handle, PROCESS_QUERY_INFORMATION, &ObjectAttributes, &ClientId);
                if (NT_SUCCESS(Status))
                {
                    InterlockedIncrement(&Opened);
                    NtClose(Handle);
                }

                ClientId.UniqueProcess = NULL;
                ClientId.UniqueThread = (HANDLE)Id;
                Status = NtOpenThread(&Handle, THREAD_QUERY_INFORMATION, &ObjectAttributes, &ClientId);
                if (NT_SUCCESS(Status))
                {
                    InterlockedIncrement(&Opened);
                    NtClose(Handle);
                }
            }
        }

        /* Leave the creators some time on a single processor */
        SwitchToThread();
    }

    return 0;
}

static
DWORD
WINAPI
EmptyThread(
    _In_ LPVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    return 0;
}

START_TEST(NtOpenProcess)
{
    WCHAR Path[MAX_PATH];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    HANDLE Openers[3], Thread;
    DWORD ThreadId;
    ULONG i, ProcessFailures = 0, ThreadFailures = 0;
    BOOL Ret;

    if (!GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path)))
    {
        skip("GetModuleFileNameW failed: %lu\n", GetLastError());
        return;
    }

    RecentIds[0] = (ULONG_PTR)NtCurrentTeb()->ClientId.UniqueThread;
    for (i = 0; i < ARRAYSIZE(Openers); i++)
    {
        Openers[i] = CreateThread(NULL, 0, OpenerThread, NULL, 0, NULL);
        ok(Openers[i] != NULL, "CreateThread failed: %lu\n", GetLastError());
    }

    for (i = 0; i < CREATIONS; i++)
    {
        /* A process that never runs */
        ZeroMemory(&StartupInfo, sizeof(StartupInfo));
        StartupInfo.cb = sizeof(StartupInfo);
        Ret = CreateProcessW(Path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED,
                             NULL, NULL, &StartupInfo, &ProcessInfo);
        if (!Ret)
        {
            if (ProcessFailures++ < 5)
                ok(0, "CreateProcessW %lu failed: %lu\n", i, GetLastError());
        }
        else
        {
            RememberId(ProcessInfo.dwProcessId);
            RememberId(ProcessInfo.dwThreadId);
            TerminateProcess(ProcessInfo.hProcess, 0);
            WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
            CloseHandle(ProcessInfo.hThread);
            CloseHandle(ProcessInfo.hProcess);
        }

        if ((i + 1) % 50 == 0)
            trace("%lu creations, %ld objects opened\n", i + 1, Opened);

        /* And a thread of this process */
        Thread = CreateThread(NULL, 0, EmptyThread, NULL, 0, &ThreadId);
        if (!Thread)
        {
            if (ThreadFailures++ < 5)
                ok(0, "CreateThread %lu failed: %lu\n", i, GetLastError());
        }
        else
        {
            RememberId(ThreadId);
            WaitForSingleObject(Thread, INFINITE);
            CloseHandle(Thread);
        }
    }

    Done = TRUE;
    WaitForMultipleObjects(ARRAYSIZE(Openers), Openers, TRUE, INFINITE);
    for (i = 0; i < ARRAYSIZE(Openers); i++)
        CloseHandle(Openers[i]);

    ok(ProcessFailures == 0, "%lu of %u process creations failed\n", ProcessFailures, CREATIONS);
    ok(ThreadFailures == 0, "%lu of %u thread creations failed\n", ThreadFailures, CREATIONS);
    trace("Openers opened %ld objects\n", Opened);
}
