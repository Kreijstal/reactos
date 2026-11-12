/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Test for the Toolhelp32 module snapshot
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"

#include <tlhelp32.h>

#define CHILD_EVENT_NAME L"Local\\kernel32_apitest_Toolhelp_%lu"

static
VOID
CheckModules(
    _In_ DWORD ProcessId,
    _In_ PCSTR Who)
{
    HANDLE Snapshot;
    MODULEENTRY32W Entry;
    ULONG Count = 0, Ntdll = 0, Kernel32 = 0;

    SetLastError(0xdeadbeef);
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ProcessId);
    ok(Snapshot != NULL && Snapshot != INVALID_HANDLE_VALUE,
       "%s: CreateToolhelp32Snapshot returned %p, error %lu\n", Who, Snapshot, GetLastError());
    if (Snapshot == NULL || Snapshot == INVALID_HANDLE_VALUE)
        return;

    Entry.dwSize = sizeof(Entry);
    if (Module32FirstW(Snapshot, &Entry))
    {
        do
        {
            ok(Entry.th32ProcessID == ProcessId, "%s: th32ProcessID %lu\n", Who, Entry.th32ProcessID);
            ok(Entry.modBaseAddr != NULL && Entry.modBaseSize != 0,
               "%s: %ls base %p size %lu\n", Who, Entry.szModule, Entry.modBaseAddr, Entry.modBaseSize);
            if (!_wcsicmp(Entry.szModule, L"ntdll.dll"))
                Ntdll++;
            if (!_wcsicmp(Entry.szModule, L"kernel32.dll"))
                Kernel32++;
            Count++;
        } while (Module32NextW(Snapshot, &Entry));
    }
    ok(Count >= 3, "%s: %lu modules listed\n", Who, Count);
    ok(Ntdll == 1, "%s: ntdll.dll listed %lu times\n", Who, Ntdll);
    ok(Kernel32 == 1, "%s: kernel32.dll listed %lu times\n", Who, Kernel32);
    CloseHandle(Snapshot);
}

static
VOID
RunChild(VOID)
{
    WCHAR EventName[64];
    HANDLE Event;

    StringCbPrintfW(EventName, sizeof(EventName), CHILD_EVENT_NAME, GetCurrentProcessId());
    Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName);
    if (Event)
    {
        SetEvent(Event);
        CloseHandle(Event);
    }
    /* The parent terminates us */
    Sleep(30000);
}

/* The structure sizes of the Windows SDK, which applications pass in dwSize */
#ifdef _WIN64
#define WIN_PROCESSENTRY32W_SIZE 568
#define WIN_PROCESSENTRY32_SIZE  304
#define WIN_HEAPLIST32_SIZE      32
#define WIN_HEAPENTRY32_SIZE     56
#define WIN_MODULEENTRY32W_SIZE  1080
#else
#define WIN_PROCESSENTRY32W_SIZE 556
#define WIN_PROCESSENTRY32_SIZE  296
#define WIN_HEAPLIST32_SIZE      16
#define WIN_HEAPENTRY32_SIZE     36
#define WIN_MODULEENTRY32W_SIZE  1064
#endif

static
VOID
CheckStructureSizes(VOID)
{
    HANDLE Snapshot;
    union
    {
        PROCESSENTRY32W ProcessW;
        PROCESSENTRY32 Process;
        BYTE Buffer[2048];
    } u;

    ok(sizeof(PROCESSENTRY32W) == WIN_PROCESSENTRY32W_SIZE, "sizeof(PROCESSENTRY32W) = %Iu\n", sizeof(PROCESSENTRY32W));
    ok(sizeof(PROCESSENTRY32) == WIN_PROCESSENTRY32_SIZE, "sizeof(PROCESSENTRY32) = %Iu\n", sizeof(PROCESSENTRY32));
    ok(sizeof(HEAPLIST32) == WIN_HEAPLIST32_SIZE, "sizeof(HEAPLIST32) = %Iu\n", sizeof(HEAPLIST32));
    ok(sizeof(HEAPENTRY32) == WIN_HEAPENTRY32_SIZE, "sizeof(HEAPENTRY32) = %Iu\n", sizeof(HEAPENTRY32));
    ok(sizeof(MODULEENTRY32W) == WIN_MODULEENTRY32W_SIZE, "sizeof(MODULEENTRY32W) = %Iu\n", sizeof(MODULEENTRY32W));

    /* What a program built with the Windows SDK passes must be accepted */
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    ok(Snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
    if (Snapshot == INVALID_HANDLE_VALUE)
        return;
    ZeroMemory(&u, sizeof(u));
    u.ProcessW.dwSize = WIN_PROCESSENTRY32W_SIZE;
    SetLastError(0xdeadbeef);
    ok(Process32FirstW(Snapshot, &u.ProcessW), "Process32FirstW failed with %lu\n", GetLastError());
    ZeroMemory(&u, sizeof(u));
    u.Process.dwSize = WIN_PROCESSENTRY32_SIZE;
    SetLastError(0xdeadbeef);
    ok(Process32First(Snapshot, &u.Process), "Process32First failed with %lu\n", GetLastError());
    CloseHandle(Snapshot);
}

START_TEST(Toolhelp)
{
    WCHAR FileName[MAX_PATH], CommandLine[MAX_PATH + 32], EventName[64];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    HANDLE Event, Snapshot;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        RunChild();
        return;
    }

    CheckStructureSizes();

    /* The current process */
    CheckModules(GetCurrentProcessId(), "self");

    /* Another running process */
    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" Toolhelp child", FileName);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
    }
    else
    {
        StringCbPrintfW(EventName, sizeof(EventName), CHILD_EVENT_NAME, ProcessInfo.dwProcessId);
        Event = CreateEventW(NULL, TRUE, FALSE, EventName);
        ok(Event != NULL, "CreateEvent failed with %lu\n", GetLastError());
        ResumeThread(ProcessInfo.hThread);
        ok(WaitForSingleObject(Event, 10000) == WAIT_OBJECT_0, "Child did not start\n");
        CheckModules(ProcessInfo.dwProcessId, "child");
        TerminateProcess(ProcessInfo.hProcess, 0);
        WaitForSingleObject(ProcessInfo.hProcess, 5000);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
        if (Event)
            CloseHandle(Event);
    }

    /* A failed snapshot is INVALID_HANDLE_VALUE, not NULL */
    SetLastError(0xdeadbeef);
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0xFFFFFFF0);
    ok(Snapshot == INVALID_HANDLE_VALUE, "Snapshot of a missing process returned %p\n", Snapshot);
    ok(GetLastError() != 0xdeadbeef && GetLastError() != ERROR_SUCCESS,
       "GetLastError() %lu\n", GetLastError());
    if (Snapshot != NULL && Snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(Snapshot);
}
