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
 * COM1 stays dedicated to the kernel debug log + DbgPrint output. Child
 * kmtest_.exe processes inherit this handle as stdout/stderr so their ok()
 * /trace() lines also reach the marker log. */
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

/* Spawn kmtest_.exe with a single test name. Inherits stdout/stderr so the
 * child's output lands on the same console (i.e., the serial line). */
static
DWORD
RunOneTest(_In_z_ PCSTR TestName)
{
    WCHAR CmdLine[MAX_PATH + 128];
    PROCESS_INFORMATION ProcInfo;
    STARTUPINFOW StartInfo;
    DWORD ExitCode = (DWORD)-1;
    int Written;

    Written = _snwprintf(CmdLine, sizeof(CmdLine) / sizeof(WCHAR) - 1,
                         L"\"%s\" %hs", g_KmtestExePath, TestName);
    if (Written < 0)
        return ExitCode;
    CmdLine[Written] = L'\0';

    ZeroMemory(&StartInfo, sizeof(StartInfo));
    StartInfo.cb = sizeof(StartInfo);
    StartInfo.dwFlags = STARTF_USESTDHANDLES;
    StartInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    /* Route child stdout/stderr to our COM1 handle so kmtest_.exe ok()/trace()
     * messages land on the serial log. Without this they would write to a
     * detached Win32 console and vanish. */
    StartInfo.hStdOutput = (g_SerialHandle != INVALID_HANDLE_VALUE)
                           ? g_SerialHandle : GetStdHandle(STD_OUTPUT_HANDLE);
    StartInfo.hStdError = StartInfo.hStdOutput;

    ZeroMemory(&ProcInfo, sizeof(ProcInfo));
    if (!CreateProcessW(NULL, CmdLine, NULL, NULL, TRUE,
                        0, NULL, NULL, &StartInfo, &ProcInfo))
    {
        EmitLine("KMTCD-SPAWN-FAIL %s err=%lu", TestName, GetLastError());
        return ExitCode;
    }

    WaitForSingleObject(ProcInfo.hProcess, INFINITE);
    GetExitCodeProcess(ProcInfo.hProcess, &ExitCode);
    CloseHandle(ProcInfo.hThread);
    CloseHandle(ProcInfo.hProcess);
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

        if (g_FilterTest[0] && strcmp(Name, g_FilterTest) != 0)
            continue;

        /* No diagnostic skips here — bisection now inside ExResource itself
         * (see ExResource.c TestResourceWithThreads conditional). */

        EmitLine("KMTCD-BEGIN %s", Name);
        ExitCode = RunOneTest(Name);
        EmitLine("KMTCD-END %s exit=%lu", Name, ExitCode);
        ++TestsRun;
        if (ExitCode != 0)
            ++TestsFailed;
    }

    EmitLine("KMTCD-SUMMARY tests_run=%ld tests_failed=%ld", TestsRun, TestsFailed);

    /* Status: 0 = all passed (QEMU exit code 1), 1 = any failure (exit code 3). */
    ExitStatus = (TestsFailed == 0 && TestsRun > 0) ? 0 : 1;
    FireExitDriver(ExitStatus);

    /* Reached only on bare hardware / non-QEMU runs. Returning here lets
     * winlogon log out the session; on QEMU we never reach this. */
    return ExitStatus;
}
