/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a console control event reaches the handler
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"

static HANDLE HandlerEvent;
static DWORD HandlerThreadId;
static DWORD HandlerCtrlType;

static
BOOL
WINAPI
CtrlHandler(
    _In_ DWORD CtrlType)
{
    if (CtrlType != CTRL_C_EVENT)
        return FALSE;

    HandlerCtrlType = CtrlType;
    HandlerThreadId = GetCurrentThreadId();
    SetEvent(HandlerEvent);
    return TRUE;
}

START_TEST(ConsoleCtrlHandler)
{
    HANDLE StdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE StdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE StdErr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD Wait;
    BOOL Success;

    HandlerEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(HandlerEvent != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!HandlerEvent)
        return;

    /* A console of our own, so that nobody else gets the event */
    FreeConsole();
    Success = AllocConsole();
    ok(Success, "AllocConsole failed: %lu\n", GetLastError());
    if (!Success)
    {
        CloseHandle(HandlerEvent);
        return;
    }

    /* AllocConsole replaced them: let the test output go where it went before */
    SetStdHandle(STD_INPUT_HANDLE, StdIn);
    SetStdHandle(STD_OUTPUT_HANDLE, StdOut);
    SetStdHandle(STD_ERROR_HANDLE, StdErr);

    /* Ctrl+C may be ignored by inheritance */
    ok(SetConsoleCtrlHandler(NULL, FALSE), "SetConsoleCtrlHandler(NULL) failed: %lu\n", GetLastError());
    ok(SetConsoleCtrlHandler(CtrlHandler, TRUE), "SetConsoleCtrlHandler failed: %lu\n", GetLastError());

    HandlerThreadId = 0;
    HandlerCtrlType = 0xDEADBEEF;
    Success = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    ok(Success, "GenerateConsoleCtrlEvent failed: %lu\n", GetLastError());
    if (Success)
    {
        /* The console starts a thread in us to call the handlers */
        Wait = WaitForSingleObject(HandlerEvent, 10000);
        ok(Wait == WAIT_OBJECT_0, "The handler was not called: %lu\n", Wait);
        ok(HandlerCtrlType == CTRL_C_EVENT, "Handler got %lu\n", HandlerCtrlType);
        ok(HandlerThreadId != 0 && HandlerThreadId != GetCurrentThreadId(),
           "Handler ran on thread %lu\n", HandlerThreadId);
    }

    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    FreeConsole();

    CloseHandle(HandlerEvent);
}
