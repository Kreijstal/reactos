/*
 * PROJECT:     ReactOS kernel-mode test orchestrator (Phase 3)
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Win32 console binary launched by winlogon (as the Shell) on a
 *              kmtestcd boot. Drives kmtest_.exe across every non-hidden test
 *              name reported by `kmtest_.exe --list`, including the standalone
 *              driver targets (ntcreatesection_drv, mmmaplockedpagesspecify-
 *              cache_drv, cccopyread_drv, etc.) which require a userland half
 *              that the native-mode kmtestrunner.exe can't run.
 *
 *              Output flows to the console (winlogon's session has the COM1
 *              serial console attached on /SOS boots). After every test ran,
 *              loads KmtExit via SCM and fires IOCTL_KMT_EXIT_QEMU so QEMU
 *              terminates with a status-derived exit code; this avoids the
 *              SMSS-bugcheck-on-shell-exit path that would otherwise leave
 *              QEMU hanging on -no-reboot.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <winioctl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../modules/rostests/kmtests/kmt_exit_drv/kmt_exit_ioctl.h"

/* When non-empty, run only the test whose name matches this string and skip
 * the rest of the kmtest_.exe --list output. Set from argv[1] so the launch
 * line in kmtestcd_setup.inf can name a single test; that lets the smoke
 * boot of kmtestcd target one known-clean test instead of churning through
 * every test in the suite and tripping latent kernel ASSERTs that hijack
 * COM1 via KDB and hang the runner. Empty == "run everything" (legacy). */
static CHAR g_FilterTest[64] = "";

/* kmtest_.exe and its standalone *_drv.sys all live in %SystemRoot%\bin
 * (where add_rostests_file installs FOR all). We invoke it by absolute path
 * because there's no guarantee bin\ is in PATH for winlogon's shell, and we
 * want KmtCreateService inside the child to resolve drivers relative to its
 * own EXE dir. The KMTEST_DIR macro expands at startup once we've resolved
 * the actual SystemRoot via GetWindowsDirectoryW. */
static WCHAR g_KmtestExePath[MAX_PATH];

/* When launched from explorer's Run key, kmtcdrunner is a Win32 process with
 * no inherited console — stdout vanishes. We attach a serial port directly so
 * the structured KMTCD-* markers reach the qemu -serial line. COM1 is owned
 * exclusively by the kernel debugger (/DEBUGPORT=COM1 in freeldr.ini), so we
 * use COM2 — the second qemu -serial backend collects our marker stream while
 * COM1 stays dedicated to the kernel debug log + DbgPrint output. Each child
 * kmtest_.exe is run with its stdout/stderr on a pipe we drain (see
 * RunOneTest): we forward every byte here so its ok()/trace() lines reach the
 * marker log, and tally the failure markers as they stream past. */
static HANDLE g_SerialHandle = INVALID_HANDLE_VALUE;

static
VOID
OpenSerialPort(VOID)
{
    SECURITY_ATTRIBUTES SecAttrs;
    SecAttrs.nLength = sizeof(SecAttrs);
    SecAttrs.bInheritHandle = TRUE;
    SecAttrs.lpSecurityDescriptor = NULL;
    g_SerialHandle = CreateFileW(L"\\\\.\\COM2",
                                 GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &SecAttrs, OPEN_EXISTING, 0, NULL);
}

static
VOID
ResolveKmtestPath(VOID)
{
    UINT Len = GetSystemWindowsDirectoryW(g_KmtestExePath, MAX_PATH);
    if (Len == 0 || Len >= MAX_PATH - 32)
    {
        wcscpy(g_KmtestExePath, L"C:\\ReactOS");
    }
    wcscat(g_KmtestExePath, L"\\bin\\kmtest_.exe");
}

static
VOID
EmitLine(PCSTR Fmt, ...)
{
    CHAR Buf[512];
    va_list Args;
    int Len;
    DWORD Written;

    va_start(Args, Fmt);
    Len = _vsnprintf(Buf, sizeof(Buf) - 2, Fmt, Args);
    va_end(Args);
    if (Len < 0 || Len > (int)(sizeof(Buf) - 2))
        Len = sizeof(Buf) - 2;
    Buf[Len++] = '\n';
    Buf[Len] = '\0';

    if (g_SerialHandle != INVALID_HANDLE_VALUE)
        WriteFile(g_SerialHandle, Buf, Len, &Written, NULL);

    OutputDebugStringA(Buf);
    fputs(Buf, stdout);
    fflush(stdout);
}

/* Pipe-and-read the output of "kmtest_.exe --list" into Buffer. Returns the
 * number of bytes read, or 0 on failure. The output is a header line
 * ("Valid test names:") plus indented test names. */
static
DWORD
CaptureList(_Out_writes_(BufferSize) PCHAR Buffer,
            _In_ DWORD BufferSize)
{
    HANDLE ReadPipe = NULL, WritePipe = NULL;
    SECURITY_ATTRIBUTES SecAttrs;
    PROCESS_INFORMATION ProcInfo;
    STARTUPINFOW StartInfo;
    WCHAR CmdLine[MAX_PATH + 32];
    DWORD TotalRead = 0;

    _snwprintf(CmdLine, sizeof(CmdLine) / sizeof(WCHAR) - 1,
               L"\"%s\" --list", g_KmtestExePath);
    CmdLine[sizeof(CmdLine) / sizeof(WCHAR) - 1] = L'\0';
    DWORD JustRead = 0;
    BOOL Ok;

    SecAttrs.nLength = sizeof(SecAttrs);
    SecAttrs.bInheritHandle = TRUE;
    SecAttrs.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&ReadPipe, &WritePipe, &SecAttrs, 0))
    {
        EmitLine("KMTCD-LIST-CREATEPIPE-FAIL %lu", GetLastError());
        return 0;
    }
    SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&StartInfo, sizeof(StartInfo));
    StartInfo.cb = sizeof(StartInfo);
    StartInfo.dwFlags = STARTF_USESTDHANDLES;
    StartInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    StartInfo.hStdOutput = WritePipe;
    StartInfo.hStdError = WritePipe;

    ZeroMemory(&ProcInfo, sizeof(ProcInfo));
    Ok = CreateProcessW(NULL, CmdLine, NULL, NULL, TRUE,
                        0, NULL, NULL, &StartInfo, &ProcInfo);
    CloseHandle(WritePipe);
    if (!Ok)
    {
        EmitLine("KMTCD-LIST-CREATEPROC-FAIL %lu", GetLastError());
        CloseHandle(ReadPipe);
        return 0;
    }

    while (TotalRead + 1 < BufferSize &&
           ReadFile(ReadPipe, Buffer + TotalRead,
                    BufferSize - TotalRead - 1, &JustRead, NULL) &&
           JustRead > 0)
    {
        TotalRead += JustRead;
    }
    Buffer[TotalRead] = '\0';
    CloseHandle(ReadPipe);

    WaitForSingleObject(ProcInfo.hProcess, INFINITE);
    CloseHandle(ProcInfo.hThread);
    CloseHandle(ProcInfo.hProcess);
    return TotalRead;
}

/* The kmtest framework prints exactly one ": Test failed: " line per failing
 * ok()/ok_*() check (kmt_test.h). Counting these is the only reliable failure
 * signal: kmtest_.exe's *exit code* is a Win32 error reflecting whether the
 * test could be *run* (service start, device open) — not whether its assertions
 * passed. A test can fail dozens of ok()s and still exit 0, or exit non-zero
 * (e.g. 128) without running a single check. So we report both, distinctly. */
#define KMT_FAIL_MARKER      ": Test failed: "
#define KMT_FAIL_MARKER_LEN  (sizeof(KMT_FAIL_MARKER) - 1)

/* Spawn kmtest_.exe with a single test name. Child stdout/stderr is piped
 * through us so we can both forward every byte to the serial log AND count the
 * failure markers. Returns the child Win32 exit code; the number of failed
 * assertions is written to *OutFailures. */
static
DWORD
RunOneTest(_In_z_ PCSTR TestName, _Out_ PLONG OutFailures)
{
    WCHAR CmdLine[MAX_PATH + 128];
    PROCESS_INFORMATION ProcInfo;
    STARTUPINFOW StartInfo;
    SECURITY_ATTRIBUTES SecAttrs;
    HANDLE ReadPipe = NULL, WritePipe = NULL;
    DWORD ExitCode = (DWORD)-1;
    LONG Failures = 0;
    int Written;
    BOOL Ok;
    /* The last (marker-1) bytes of each chunk are carried to the front of the
     * next read so a marker split across two reads is still found. A whole
     * marker can never fit inside the carry (carry < marker length), so
     * re-scanning [0..Total) every iteration cannot double-count. */
    CHAR Scan[4096 + KMT_FAIL_MARKER_LEN];
    DWORD CarryLen = 0;

    *OutFailures = 0;

    Written = _snwprintf(CmdLine, sizeof(CmdLine) / sizeof(WCHAR) - 1,
                         L"\"%s\" %hs", g_KmtestExePath, TestName);
    if (Written < 0)
        return ExitCode;
    CmdLine[Written] = L'\0';

    SecAttrs.nLength = sizeof(SecAttrs);
    SecAttrs.bInheritHandle = TRUE;
    SecAttrs.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&ReadPipe, &WritePipe, &SecAttrs, 0))
    {
        EmitLine("KMTCD-SPAWN-FAIL %s pipe-err=%lu", TestName, GetLastError());
        return ExitCode;
    }
    SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&StartInfo, sizeof(StartInfo));
    StartInfo.cb = sizeof(StartInfo);
    StartInfo.dwFlags = STARTF_USESTDHANDLES;
    StartInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    StartInfo.hStdOutput = WritePipe;
    StartInfo.hStdError = WritePipe;

    ZeroMemory(&ProcInfo, sizeof(ProcInfo));
    Ok = CreateProcessW(NULL, CmdLine, NULL, NULL, TRUE,
                        0, NULL, NULL, &StartInfo, &ProcInfo);
    CloseHandle(WritePipe);
    if (!Ok)
    {
        EmitLine("KMTCD-SPAWN-FAIL %s err=%lu", TestName, GetLastError());
        CloseHandle(ReadPipe);
        return ExitCode;
    }

    /* Drain the pipe continuously — a stalled reader would deadlock the child
     * once the ~4 KB pipe buffer fills. Forward each fresh chunk to the serial
     * log, then count failure markers over carry+chunk. */
    for (;;)
    {
        DWORD JustRead = 0;
        DWORD Total, i;

        if (!ReadFile(ReadPipe, Scan + CarryLen, 4096, &JustRead, NULL) ||
            JustRead == 0)
        {
            break;  /* child closed its end / pipe broken => child exiting */
        }

        if (g_SerialHandle != INVALID_HANDLE_VALUE)
        {
            DWORD SerWritten;
            WriteFile(g_SerialHandle, Scan + CarryLen, JustRead, &SerWritten, NULL);
        }

        Total = CarryLen + JustRead;
        if (Total >= KMT_FAIL_MARKER_LEN)
        {
            for (i = 0; i + KMT_FAIL_MARKER_LEN <= Total; ++i)
            {
                if (Scan[i] == ':' &&
                    memcmp(Scan + i, KMT_FAIL_MARKER, KMT_FAIL_MARKER_LEN) == 0)
                {
                    ++Failures;
                }
            }
            /* Carry the last (marker-1) bytes into the next read. */
            CarryLen = KMT_FAIL_MARKER_LEN - 1;
            memmove(Scan, Scan + Total - CarryLen, CarryLen);
        }
        else
        {
            CarryLen = Total;  /* not enough bytes to scan yet; carry them all */
        }
    }
    CloseHandle(ReadPipe);

    WaitForSingleObject(ProcInfo.hProcess, INFINITE);
    GetExitCodeProcess(ProcInfo.hProcess, &ExitCode);
    CloseHandle(ProcInfo.hThread);
    CloseHandle(ProcInfo.hProcess);

    *OutFailures = Failures;
    return ExitCode;
}

/* Bring up the KmtExit service via SCM if it isn't already running, then open
 * its device and fire IOCTL_KMT_EXIT_QEMU. The kernel side writes Status to
 * QEMU's isa-debug-exit port and the VM terminates with (Status << 1) | 1. */
static
VOID
FireExitDriver(_In_ UCHAR Status)
{
    SC_HANDLE Scm = NULL, Service = NULL;
    HANDLE Device = INVALID_HANDLE_VALUE;
    DWORD BytesReturned;

    Scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!Scm)
    {
        EmitLine("KMTCD-EXIT-SCM-FAIL %lu", GetLastError());
        goto Cleanup;
    }

    Service = OpenServiceW(Scm, L"KmtExit", SERVICE_ALL_ACCESS);
    if (!Service)
    {
        EmitLine("KMTCD-EXIT-OPENSVC-FAIL %lu", GetLastError());
        goto Cleanup;
    }

    if (!StartServiceW(Service, 0, NULL))
    {
        DWORD Err = GetLastError();
        if (Err != ERROR_SERVICE_ALREADY_RUNNING)
        {
            EmitLine("KMTCD-EXIT-STARTSVC-FAIL %lu", Err);
            goto Cleanup;
        }
    }

    Device = CreateFileW(L"\\\\.\\KmtExit",
                         GENERIC_READ | GENERIC_WRITE,
                         0, NULL, OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE)
    {
        EmitLine("KMTCD-EXIT-OPENDEV-FAIL %lu", GetLastError());
        goto Cleanup;
    }

    DeviceIoControl(Device, IOCTL_KMT_EXIT_QEMU,
                    &Status, sizeof(Status),
                    NULL, 0, &BytesReturned, NULL);
    /* On QEMU with isa-debug-exit, the VM exits before DeviceIoControl returns. */

Cleanup:
    if (Device != INVALID_HANDLE_VALUE)
        CloseHandle(Device);
    if (Service)
        CloseServiceHandle(Service);
    if (Scm)
        CloseServiceHandle(Scm);
}

int
main(void)
{
    static CHAR ListBuffer[64 * 1024];
    DWORD Read;
    PCHAR Cursor;
    LONG TestsRun = 0;
    LONG TestsFailed = 0;
    LONG TestsErrored = 0;
    LONG TotalAssertFailures = 0;
    UCHAR ExitStatus;
    int ArgC = 0;
    LPWSTR *ArgV = CommandLineToArgvW(GetCommandLineW(), &ArgC);

    if (ArgV && ArgC >= 2 && ArgV[1] && ArgV[1][0])
    {
        WideCharToMultiByte(CP_UTF8, 0, ArgV[1], -1,
                            g_FilterTest, sizeof(g_FilterTest) - 1,
                            NULL, NULL);
        g_FilterTest[sizeof(g_FilterTest) - 1] = '\0';
    }
    if (ArgV)
        LocalFree(ArgV);

    OpenSerialPort();
    ResolveKmtestPath();
    EmitLine("KMTCD-RUNNER start kmtest=%ls serial=%d filter=%s",
             g_KmtestExePath,
             g_SerialHandle != INVALID_HANDLE_VALUE,
             g_FilterTest[0] ? g_FilterTest : "<all>");

    Read = CaptureList(ListBuffer, sizeof(ListBuffer));
    if (Read == 0)
    {
        EmitLine("KMTCD-RUNNER no list");
        FireExitDriver(1);
        return 1;
    }

    /* kmtest_.exe --list prints a header line followed by indented test names
     * (4-space indent). Any line not starting with whitespace is a heading or
     * blank and is skipped. */
    Cursor = ListBuffer;
    while (*Cursor)
    {
        PCHAR LineStart = Cursor;
        PCHAR LineEnd;
        PCHAR Name;
        DWORD ExitCode;
        LONG Failures = 0;

        LineEnd = strpbrk(LineStart, "\r\n");
        if (LineEnd)
        {
            *LineEnd = '\0';
            Cursor = LineEnd + 1;
            while (*Cursor == '\r' || *Cursor == '\n')
                ++Cursor;
        }
        else
        {
            Cursor = LineStart + strlen(LineStart);
        }

        /* Skip non-indented (headers) and empty lines. Indented lines are
         * test names with a leading "    " (4 spaces). */
        if (LineStart[0] != ' ' && LineStart[0] != '\t')
            continue;
        Name = LineStart;
        while (*Name == ' ' || *Name == '\t')
            ++Name;
        if (*Name == '\0')
            continue;

        /* Prefix match: argv[1]="KeMutex" runs exactly that test, argv[1]="Ke"
         * runs every Ke* test. A leading '!' inverts it: argv[1]="!IoFilesystem"
         * runs every test EXCEPT IoFilesystem* (lets one boot cover the whole
         * suite minus a slow/hanging test). Empty filter runs all. */
        if (g_FilterTest[0] == '!')
        {
            if (strncmp(Name, &g_FilterTest[1], strlen(&g_FilterTest[1])) == 0)
                continue;
        }
        else if (g_FilterTest[0] && strncmp(Name, g_FilterTest, strlen(g_FilterTest)) != 0)
            continue;

        /* No diagnostic skips here — bisection now inside ExResource itself
         * (see ExResource.c TestResourceWithThreads conditional). */

        EmitLine("KMTCD-BEGIN %s", Name);
        ExitCode = RunOneTest(Name, &Failures);
        EmitLine("KMTCD-END %s exit=%lu failures=%ld", Name, ExitCode, Failures);
        ++TestsRun;
        TotalAssertFailures += Failures;
        if (Failures > 0)
            ++TestsFailed;
        if (ExitCode != 0)
            ++TestsErrored;
    }

    /* tests_failed counts tests with >=1 failed ok() assertion (the real
     * pass/fail signal); errored counts tests kmtest_.exe could not run at all
     * (non-zero Win32 exit, no assertions executed); assert_failures is the
     * grand total of failed checks across the suite. */
    EmitLine("KMTCD-SUMMARY tests_run=%ld tests_failed=%ld errored=%ld assert_failures=%ld",
             TestsRun, TestsFailed, TestsErrored, TotalAssertFailures);

    /* Status: 0 = fully clean (QEMU exit code 1); 1 = any assertion failure or
     * a test that could not be run (exit code 3). */
    ExitStatus = (TestsFailed == 0 && TestsErrored == 0 && TestsRun > 0) ? 0 : 1;
    FireExitDriver(ExitStatus);

    /* Reached only on bare hardware / non-QEMU runs. Returning here lets
     * winlogon log out the session; on QEMU we never reach this. */
    return ExitStatus;
}
