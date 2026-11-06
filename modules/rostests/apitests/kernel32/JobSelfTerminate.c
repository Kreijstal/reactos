/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for job termination reaching the terminating process
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "precomp.h"

/*
 * A job kill that sweeps over the calling process meets the current thread.
 * That thread must get the exit APC like every other one, not exit on the
 * spot inside the job lock: doing so bugchecked with
 * KERNEL_APC_PENDING_DURING_EXIT (0x20) on the ASUS during shutdown, when
 * luagent closed the last handle to its own KILL_ON_JOB_CLOSE job.
 */

#define EXIT_CODE_EXIT      0x1234
#define EXIT_CODE_TERMINATE 0x4321
#define EXIT_CODE_RETURNED  0x0bad

static
HANDLE
CreateJobForSelf(
    _In_ DWORD LimitFlags)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits = { 0 };
    HANDLE hJob;

    hJob = CreateJobObjectW(NULL, NULL);
    if (!hJob)
        ExitProcess(0xdead0001);

    Limits.BasicLimitInformation.LimitFlags = LimitFlags;
    if (!SetInformationJobObject(hJob,
                                 JobObjectExtendedLimitInformation,
                                 &Limits,
                                 sizeof(Limits)))
        ExitProcess(0xdead0002);

    if (!AssignProcessToJobObject(hJob, GetCurrentProcess()))
        ExitProcess(0xdead0003);

    return hJob;
}

static
VOID
RunChild(
    _In_ PCSTR Mode)
{
    HANDLE hJob;

    if (!strcmp(Mode, "exit"))
    {
        /* The last handle closes while this process is already exiting */
        hJob = CreateJobForSelf(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
        (void)hJob;
        ExitProcess(EXIT_CODE_EXIT);
    }
    else if (!strcmp(Mode, "terminate"))
    {
        /* The kill reaches us through the exit APC and never returns */
        hJob = CreateJobForSelf(0);
        TerminateJobObject(hJob, EXIT_CODE_TERMINATE);
        ExitProcess(EXIT_CODE_RETURNED);
    }
    ExitProcess(0xdead0000);
}

static
VOID
TestChild(
    _In_ PCWSTR Mode,
    _In_ DWORD ExpectedCode)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH * 2];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode = 0;
    BOOL Success;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine),
                    L"\"%ls\" JobSelfTerminate %ls", FileName, Mode);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    Success = CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE,
                             0, NULL, NULL, &StartupInfo, &ProcessInfo);
    ok(Success, "CreateProcess(%ls) failed with %lu\n", Mode, GetLastError());
    if (!Success)
        return;
    CloseHandle(ProcessInfo.hThread);

    Wait = WaitForSingleObject(ProcessInfo.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "%ls: wait returned %lu\n", Mode, Wait);
    ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode),
       "%ls: GetExitCodeProcess failed with %lu\n", Mode, GetLastError());
    ok(ExitCode == ExpectedCode, "%ls: exit code 0x%lx, expected 0x%lx\n",
       Mode, ExitCode, ExpectedCode);
    CloseHandle(ProcessInfo.hProcess);
}

START_TEST(JobSelfTerminate)
{
    PSTR *Argv;
    INT Argc;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 3)
    {
        RunChild(Argv[2]);
        return;
    }

    TestChild(L"exit", EXIT_CODE_EXIT);
    TestChild(L"terminate", EXIT_CODE_TERMINATE);
}
