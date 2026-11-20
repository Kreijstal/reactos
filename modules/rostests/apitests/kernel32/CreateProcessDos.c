/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test CreateProcess of a DOS program
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"

/* mov ax, 4C05h ; int 21h -- exit with code 5 */
static const UCHAR DosProgram[] = { 0xB8, 0x05, 0x4C, 0xCD, 0x21 };

START_TEST(CreateProcessDos)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR TempPath[MAX_PATH], ComPath[MAX_PATH], Command[MAX_PATH + 2];
    DWORD Written, Error, ExitCode, Wait;
    HANDLE File;
    BOOL Success;

    GetTempPathW(_countof(TempPath), TempPath);
    StringCchPrintfW(ComPath, _countof(ComPath), L"%lsdos%lu.com", TempPath, GetCurrentProcessId());
    File = CreateFileW(ComPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW(%ls) failed: %lu\n", ComPath, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    Success = WriteFile(File, DosProgram, sizeof(DosProgram), &Written, NULL);
    ok(Success && Written == sizeof(DosProgram), "WriteFile failed: %lu\n", GetLastError());
    CloseHandle(File);

    /*
     * 64-bit Windows has no NTVDM and refuses DOS programs, from 32-bit
     * callers as well; 32-bit Windows runs them in NTVDM.
     */
    StringCchPrintfW(Command, _countof(Command), L"\"%ls\"", ComPath);
    SetLastError(0xDEADBEEF);
    Success = CreateProcessW(ComPath, Command, NULL, NULL, FALSE,
                             CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
    Error = GetLastError();
    if (Success)
    {
        Wait = WaitForSingleObject(pi.hProcess, 60000);
        ok(Wait == WAIT_OBJECT_0, "The DOS program did not exit: %lu\n", Wait);
        if (Wait != WAIT_OBJECT_0)
            TerminateProcess(pi.hProcess, 1);
        ExitCode = 0xDEADBEEF;
        GetExitCodeProcess(pi.hProcess, &ExitCode);
        ok(ExitCode == 5, "Exit code %lu\n", ExitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
    {
        ok(Error == ERROR_BAD_EXE_FORMAT, "CreateProcessW failed with %lu\n", Error);
    }

    DeleteFileW(ComPath);
}
