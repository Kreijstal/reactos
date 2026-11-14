/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test process creation from and to 32-bit (WoW64) processes
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"
#include <pseh/pseh2.h>

typedef BOOL (WINAPI *PFN_ISWOW64PROCESS)(HANDLE, PBOOL);
typedef BOOL (WINAPI *PFN_WOW64DISABLEWOW64FSREDIRECTION)(PVOID*);
typedef BOOL (WINAPI *PFN_WOW64REVERTWOW64FSREDIRECTION)(PVOID);

static PFN_ISWOW64PROCESS pIsWow64Process;
static PFN_WOW64DISABLEWOW64FSREDIRECTION pWow64DisableWow64FsRedirection;
static PFN_WOW64REVERTWOW64FSREDIRECTION pWow64RevertWow64FsRedirection;

static BOOL
IsWow64(HANDLE Process)
{
    BOOL Wow64 = FALSE;

    if (pIsWow64Process && !pIsWow64Process(Process, &Wow64))
    {
        ok(0, "IsWow64Process failed: %lu\n", GetLastError());
        return FALSE;
    }
    return Wow64;
}

/*
 * Run CommandLine with its stdout on a pipe; check that the child is
 * (or isn't) a WoW64 process, that it prints Expected and exits with
 * ExpectedExit.
 */
static VOID
RunChild(
    _In_ PCWSTR Name,
    _In_ PCWSTR CommandLine,
    _In_ BOOL ExpectWow64,
    _In_ PCSTR Expected,
    _In_ DWORD ExpectedExit)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE ReadPipe, WritePipe;
    WCHAR Command[MAX_PATH * 3];
    CHAR Output[256];
    DWORD Total = 0, Read, ExitCode = 0xDEADBEEF, Wait;
    BOOL Success;

    Success = CreatePipe(&ReadPipe, &WritePipe, &sa, 0);
    ok(Success, "%ls: CreatePipe failed: %lu\n", Name, GetLastError());
    if (!Success)
        return;
    SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0);

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = WritePipe;
    si.hStdError = WritePipe;

    StringCchCopyW(Command, _countof(Command), CommandLine);
    Success = CreateProcessW(NULL, Command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    ok(Success, "%ls: CreateProcessW(%ls) failed: %lu\n", Name, CommandLine, GetLastError());
    CloseHandle(WritePipe);
    if (!Success)
    {
        CloseHandle(ReadPipe);
        return;
    }

    ok(IsWow64(pi.hProcess) == ExpectWow64, "%ls: child is %sa WoW64 process\n",
       Name, ExpectWow64 ? "not " : "");

    while (Total < sizeof(Output) - 1 &&
           ReadFile(ReadPipe, Output + Total, sizeof(Output) - 1 - Total, &Read, NULL) &&
           Read != 0)
    {
        Total += Read;
    }
    Output[Total] = ANSI_NULL;
    CloseHandle(ReadPipe);

    Wait = WaitForSingleObject(pi.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "%ls: WaitForSingleObject returned %lu\n", Name, Wait);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, 1);
    ok(GetExitCodeProcess(pi.hProcess, &ExitCode), "%ls: GetExitCodeProcess failed: %lu\n",
       Name, GetLastError());
    ok(ExitCode == ExpectedExit, "%ls: exit code %lu (0x%lx), expected %lu\n",
       Name, ExitCode, ExitCode, ExpectedExit);
    ok(strstr(Output, Expected) != NULL, "%ls: output '%s', expected '%s'\n",
       Name, Output, Expected);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/*
 * Console calls whose data is too big for the message travel in a CSR
 * capture buffer; check a write and a read-back of more than that through
 * a screen buffer of our own.
 */
static VOID
TestConsoleCapture(VOID)
{
    WCHAR Text[200], ReadBack[200];
    COORD Origin = { 0, 0 };
    DWORD Count, i;
    HANDLE Buffer;
    BOOL Success;

    if (!GetConsoleWindow() && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR)
    {
        AllocConsole();
    }

    Buffer = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                       CONSOLE_TEXTMODE_BUFFER, NULL);
    ok(Buffer != INVALID_HANDLE_VALUE, "CreateConsoleScreenBuffer failed: %lu\n", GetLastError());
    if (Buffer == INVALID_HANDLE_VALUE)
        return;

    for (i = 0; i < _countof(Text); i++)
        Text[i] = L'A' + (i % 26);

    Count = 0;
    Success = WriteConsoleW(Buffer, Text, _countof(Text), &Count, NULL);
    ok(Success, "WriteConsoleW failed: %lu\n", GetLastError());
    ok(Count == _countof(Text), "WriteConsoleW wrote %lu characters\n", Count);

    Count = 0;
    RtlFillMemory(ReadBack, sizeof(ReadBack), 0xCC);
    Success = ReadConsoleOutputCharacterW(Buffer, ReadBack, _countof(ReadBack), Origin, &Count);
    ok(Success, "ReadConsoleOutputCharacterW failed: %lu\n", GetLastError());
    ok(Count == _countof(ReadBack), "ReadConsoleOutputCharacterW read %lu characters\n", Count);
    ok(!memcmp(ReadBack, Text, sizeof(Text)), "Read back '%.40ls...'\n", ReadBack);

    CloseHandle(Buffer);
}

/*
 * A 32-bit process creating a process first has to survive the exceptions
 * on the way: a raised one (NtRaiseException) and a fault, both handled.
 */
static VOID
TestExceptions(VOID)
{
    volatile LONG *Null = NULL;
    NTSTATUS Code = 0;

    _SEH2_TRY
    {
        RaiseException(0xE0000079, 0, 0, NULL);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Code = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok(Code == 0xE0000079, "RaiseException caught 0x%lx\n", Code);

    Code = 0;
    _SEH2_TRY
    {
        *Null = 1;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Code = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok(Code == STATUS_ACCESS_VIOLATION, "Access violation caught 0x%lx\n", Code);
}

START_TEST(CreateProcessWow64)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    WCHAR WinDir[MAX_PATH], Cmd32[MAX_PATH], Cmd64[MAX_PATH], Command[MAX_PATH * 3];
    PVOID OldRedirection = NULL;
    BOOL Redirected = FALSE;

    pIsWow64Process = (PFN_ISWOW64PROCESS)GetProcAddress(Kernel32, "IsWow64Process");
    pWow64DisableWow64FsRedirection =
        (PFN_WOW64DISABLEWOW64FSREDIRECTION)GetProcAddress(Kernel32, "Wow64DisableWow64FsRedirection");
    pWow64RevertWow64FsRedirection =
        (PFN_WOW64REVERTWOW64FSREDIRECTION)GetProcAddress(Kernel32, "Wow64RevertWow64FsRedirection");

    TestExceptions();
    TestConsoleCapture();

    if (!GetSystemWindowsDirectoryW(WinDir, _countof(WinDir)))
    {
        skip("GetSystemWindowsDirectoryW failed: %lu\n", GetLastError());
        return;
    }
    StringCchPrintfW(Cmd32, _countof(Cmd32), L"%ls\\SysWOW64\\cmd.exe", WinDir);
    StringCchPrintfW(Cmd64, _countof(Cmd64), L"%ls\\System32\\cmd.exe", WinDir);

    if (IsWow64(GetCurrentProcess()))
    {
        /* We are the 32-bit parent: reach the native System32 */
        if (pWow64DisableWow64FsRedirection)
            Redirected = pWow64DisableWow64FsRedirection(&OldRedirection);

        trace("32-bit parent, children %ls and %ls\n", Cmd32, Cmd64);
        StringCchPrintfW(Command, _countof(Command), L"\"%ls\" /c echo from-32-to-32& exit 6", Cmd32);
        RunChild(L"32->32", Command, TRUE, "from-32-to-32", 6);
        StringCchPrintfW(Command, _countof(Command), L"\"%ls\" /c echo from-32-to-64& exit 4", Cmd64);
        RunChild(L"32->64", Command, FALSE, "from-32-to-64", 4);
    }
    else
    {
#ifdef _WIN64
        if (GetFileAttributesW(Cmd32) == INVALID_FILE_ATTRIBUTES)
        {
            skip("No %ls\n", Cmd32);
            return;
        }

        /* We are native: let a 32-bit cmd.exe be the parent of both kinds of children */
        trace("64-bit parent, 32-bit cmd parent %ls\n", Cmd32);
        StringCchPrintfW(Command, _countof(Command), L"\"%ls\" /c echo wow64-own& exit 7", Cmd32);
        RunChild(L"64->32", Command, TRUE, "wow64-own", 7);
        StringCchPrintfW(Command, _countof(Command),
                         L"\"%ls\" /c \"\"%ls\" /c echo from-32-to-32& exit 6\"", Cmd32, Cmd32);
        RunChild(L"64->32->32", Command, TRUE, "from-32-to-32", 6);
        StringCchPrintfW(Command, _countof(Command),
                         L"\"%ls\" /c \"\"%ls\" /c echo from-32-to-64& exit 4\"", Cmd32, Cmd64);
        RunChild(L"64->32->64", Command, TRUE, "from-32-to-64", 4);
#else
        skip("Not a WoW64 process and no WoW64 on this system\n");
#endif
        return;
    }

    if (Redirected && pWow64RevertWow64FsRedirection)
        pWow64RevertWow64FsRedirection(OldRedirection);
}
