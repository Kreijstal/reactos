/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite user-mode test for critical sections
 *              initialized and deleted by several threads at once
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

/*
 * Every critical section gets a debug object of its own, in the process'
 * list of locks. Threads that initialize and delete critical sections at the
 * same time (thread attach and detach of DLLs that keep per-thread data, such
 * as rpcrt4) must never be handed the same one: the second thread's delete
 * then unlinks a debug object the first one freed and zeroed, faults with the
 * process' critical section list lock held, and every later critical section
 * initialization or deletion in the process waits for good.
 */

#define CS_THREADS 16
#define CS_RUN_MS 5000

typedef struct _CS_WORKER
{
    volatile LONG *Stop;
    ULONG Rounds;
    ULONG Shared;
} CS_WORKER, *PCS_WORKER;

static
DWORD
WINAPI
CsWorker(
    _In_ PVOID Context)
{
    PCS_WORKER Worker = Context;
    CRITICAL_SECTION Cs[16];
    ULONG i;

    while (!*Worker->Stop)
    {
        for (i = 0; i < RTL_NUMBER_OF(Cs); i++)
            InitializeCriticalSection(&Cs[i]);

        for (i = 0; i < RTL_NUMBER_OF(Cs); i++)
        {
            EnterCriticalSection(&Cs[i]);
            /* Our own debug object still points back to our own lock */
            if (Cs[i].DebugInfo->CriticalSection != (PRTL_CRITICAL_SECTION)&Cs[i])
                Worker->Shared++;
            LeaveCriticalSection(&Cs[i]);
        }

        for (i = 0; i < RTL_NUMBER_OF(Cs); i++)
            DeleteCriticalSection(&Cs[i]);
        Worker->Rounds++;
    }
    return 0;
}

START_TEST(RtlCriticalSection)
{
    CS_WORKER Workers[CS_THREADS];
    HANDLE Threads[CS_THREADS];
    volatile LONG Stop = 0;
    ULONG i, Started = 0, Rounds = 0, Shared = 0;
    CRITICAL_SECTION Probe;

    for (i = 0; i < CS_THREADS; i++)
    {
        Workers[i].Stop = &Stop;
        Workers[i].Rounds = 0;
        Workers[i].Shared = 0;
        Threads[i] = CreateThread(NULL, 0, CsWorker, &Workers[i], 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (Threads[i])
            Started++;
    }

    Sleep(CS_RUN_MS);
    InterlockedExchange(&Stop, 1);
    for (i = 0; i < CS_THREADS; i++)
    {
        if (!Threads[i])
            continue;
        ok_eq_ulong(WaitForSingleObject(Threads[i], 30000), WAIT_OBJECT_0);
        CloseHandle(Threads[i]);
        Rounds += Workers[i].Rounds;
        Shared += Workers[i].Shared;
    }
    trace("%lu threads, %lu rounds\n", Started, Rounds);
    ok(Rounds > 0, "No round completed\n");
    ok_eq_ulong(Shared, 0UL);

    /* The process' list of locks still works */
    InitializeCriticalSection(&Probe);
    EnterCriticalSection(&Probe);
    LeaveCriticalSection(&Probe);
    DeleteCriticalSection(&Probe);
}
