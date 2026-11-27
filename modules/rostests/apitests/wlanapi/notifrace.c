/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the first notification registrations of a process
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * Threads of a process that has not registered for notifications yet all
 * call WlanRegisterNotification at once, each on its own client handle.
 * Every registration succeeds, and none of the threads hangs or crashes.
 * A process can make its first registration only once, so each round runs
 * in a child process.  Needs WlanSvc; skipped otherwise.
 */

#include <apitest.h>
#include <wlanapi.h>
#include <strsafe.h>

#define CHILD_ROUNDS        60
#define TOTAL_BUDGET_MS     (120 * 1000)
#define CHILD_TIMEOUT_MS    (30 * 1000)
#define THREAD_TIMEOUT_MS   (20 * 1000)
#define MAX_THREADS         8

#define EXIT_CLEAN          0
#define EXIT_NO_SERVICE     1
#define EXIT_BAD_RESULT     2
#define EXIT_WORKER_HUNG    3
#define EXIT_SETUP_FAILED   4

typedef struct _RACER
{
    HANDLE Client;
    DWORD Result;
} RACER, *PRACER;

static volatile LONG g_Go;
static volatile LONG g_Ready;

static VOID WINAPI
Callback(PWLAN_NOTIFICATION_DATA Data, PVOID Context)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(Context);
}

static DWORD WINAPI
RacerThread(LPVOID Parameter)
{
    PRACER Racer = Parameter;

    /* Spin, so that all racers start the same moment. */
    InterlockedIncrement(&g_Ready);
    while (!g_Go)
        YieldProcessor();

    Racer->Result = WlanRegisterNotification(Racer->Client, WLAN_NOTIFICATION_SOURCE_ALL,
                                             FALSE, Callback, NULL, NULL, NULL);
    return 0;
}

static DECLSPEC_NORETURN VOID
RunChild(VOID)
{
    RACER Racers[MAX_THREADS];
    HANDLE Threads[MAX_THREADS];
    SYSTEM_INFO Info;
    DWORD Count, i, Negotiated, Wait;
    BOOL Bad = FALSE;

    GetSystemInfo(&Info);
    Count = Info.dwNumberOfProcessors;
    if (Count < 2)
        Count = 2;
    if (Count > MAX_THREADS)
        Count = MAX_THREADS;

    for (i = 0; i < Count; i++)
    {
        Racers[i].Result = ERROR_GEN_FAILURE;
        if (WlanOpenHandle(2, NULL, &Negotiated, &Racers[i].Client) != ERROR_SUCCESS)
            ExitProcess(i == 0 ? EXIT_NO_SERVICE : EXIT_SETUP_FAILED);
    }

    for (i = 0; i < Count; i++)
    {
        Threads[i] = CreateThread(NULL, 0, RacerThread, &Racers[i], 0, NULL);
        if (Threads[i] == NULL)
            ExitProcess(EXIT_SETUP_FAILED);
    }
    while (g_Ready != (LONG)Count)
        Sleep(0);
    InterlockedExchange(&g_Go, 1);

    Wait = WaitForMultipleObjects(Count, Threads, TRUE, THREAD_TIMEOUT_MS);
    if (Wait == WAIT_TIMEOUT)
        ExitProcess(EXIT_WORKER_HUNG);

    for (i = 0; i < Count; i++)
    {
        if (Racers[i].Result != ERROR_SUCCESS)
            Bad = TRUE;
        CloseHandle(Threads[i]);
        WlanRegisterNotification(Racers[i].Client, WLAN_NOTIFICATION_SOURCE_NONE,
                                 FALSE, NULL, NULL, NULL, NULL);
        WlanCloseHandle(Racers[i].Client, NULL);
    }

    ExitProcess(Bad ? EXIT_BAD_RESULT : EXIT_CLEAN);
}

START_TEST(notifrace)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH * 2];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    ULONG Round, Clean = 0, Crashed = 0, Hung = 0, Bad = 0, Other = 0;
    DWORD Wait, ExitCode, Start;
    PSTR *Argv;
    INT Argc;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 3 && !strcmp(Argv[2], "child"))
        RunChild();

    GetModuleFileNameW(NULL, FileName, ARRAYSIZE(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" notifrace child", FileName);

    Start = GetTickCount();
    for (Round = 0; Round < CHILD_ROUNDS && GetTickCount() - Start < TOTAL_BUDGET_MS; Round++)
    {
        if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL,
                            &StartupInfo, &ProcessInfo))
        {
            ok(0, "CreateProcess failed with %lu\n", GetLastError());
            return;
        }
        CloseHandle(ProcessInfo.hThread);

        Wait = WaitForSingleObject(ProcessInfo.hProcess, CHILD_TIMEOUT_MS);
        if (Wait != WAIT_OBJECT_0)
        {
            TerminateProcess(ProcessInfo.hProcess, EXIT_WORKER_HUNG);
            WaitForSingleObject(ProcessInfo.hProcess, 10000);
            ExitCode = EXIT_WORKER_HUNG;
        }
        else if (!GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode))
        {
            ExitCode = EXIT_SETUP_FAILED;
        }
        CloseHandle(ProcessInfo.hProcess);

        if (ExitCode == EXIT_NO_SERVICE && Round == 0)
        {
            skip("WlanSvc is not running\n");
            return;
        }

        switch (ExitCode)
        {
            case EXIT_CLEAN: Clean++; break;
            case EXIT_WORKER_HUNG: Hung++; break;
            case EXIT_BAD_RESULT: Bad++; break;
            case EXIT_NO_SERVICE:
            case EXIT_SETUP_FAILED: Other++; break;
            default:
                trace("round %lu: child died with 0x%08lx\n", Round, ExitCode);
                Crashed++;
                break;
        }
    }

    trace("notifrace: %lu rounds: %lu clean, %lu crashed, %lu hung, %lu bad results, %lu setup failures\n",
          Round, Clean, Crashed, Hung, Bad, Other);
    ok(Round > 0, "no round ran\n");
    ok(Crashed == 0, "%lu of %lu child processes crashed\n", Crashed, Round);
    ok(Hung == 0, "%lu of %lu child processes hung\n", Hung, Round);
    ok(Bad == 0, "%lu of %lu child processes had a registration fail\n", Bad, Round);
    ok(Other == 0, "%lu of %lu child processes failed to set up\n", Other, Round);
}
