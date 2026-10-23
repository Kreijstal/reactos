/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for GetOverlappedResultEx
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */
#include "precomp.h"

typedef BOOL (WINAPI *FN_GetOverlappedResultEx)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);

static VOID CALLBACK
ApcRoutine(ULONG_PTR Parameter)
{
    *(PLONG)Parameter += 1;
}

START_TEST(GetOverlappedResultEx)
{
    static const WCHAR PipeName[] = L"\\\\.\\pipe\\GetOverlappedResultEx_apitest";
    FN_GetOverlappedResultEx pGetOverlappedResultEx;
    HANDLE Server, Client;
    OVERLAPPED Overlapped;
    CHAR Buffer[16];
    DWORD Transferred, Written;
    LONG ApcCount = 0;
    BOOL Ret;

    pGetOverlappedResultEx = (FN_GetOverlappedResultEx)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                       "GetOverlappedResultEx");
    if (!pGetOverlappedResultEx)
    {
        skip("GetOverlappedResultEx is not available\n");
        return;
    }

    Server = CreateNamedPipeW(PipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0, 0, 0, NULL);
    ok(Server != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed with %lu\n", GetLastError());
    if (Server == INVALID_HANDLE_VALUE)
        return;
    Client = CreateFileW(PipeName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Client != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (Client == INVALID_HANDLE_VALUE)
    {
        CloseHandle(Server);
        return;
    }

    /* A read that waits for data */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), NULL, &Overlapped);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "ReadFile returned %d, error %lu\n", Ret, GetLastError());

    /* No waiting: still in progress */
    SetLastError(0xdeadbeef);
    Transferred = 0xdeadbeef;
    Ret = pGetOverlappedResultEx(Server, &Overlapped, &Transferred, 0, FALSE);
    ok(!Ret, "GetOverlappedResultEx returned %d\n", Ret);
    ok_err(ERROR_IO_INCOMPLETE);
    ok_long(Transferred, 0xdeadbeef);

    /* Waiting a little: the time runs out */
    SetLastError(0xdeadbeef);
    Ret = pGetOverlappedResultEx(Server, &Overlapped, &Transferred, 50, FALSE);
    ok(!Ret, "GetOverlappedResultEx returned %d\n", Ret);
    ok_err(WAIT_TIMEOUT);

    /* An alertable wait ends with an APC */
    ok(QueueUserAPC(ApcRoutine, GetCurrentThread(), (ULONG_PTR)&ApcCount), "QueueUserAPC failed\n");
    SetLastError(0xdeadbeef);
    Ret = pGetOverlappedResultEx(Server, &Overlapped, &Transferred, 5000, TRUE);
    ok(!Ret, "GetOverlappedResultEx returned %d\n", Ret);
    ok_err(WAIT_IO_COMPLETION);
    ok_long(ApcCount, 1);

    /* And with the data there, it completes */
    ok(WriteFile(Client, "hello", 5, &Written, NULL), "WriteFile failed with %lu\n", GetLastError());
    Transferred = 0;
    Ret = pGetOverlappedResultEx(Server, &Overlapped, &Transferred, 5000, FALSE);
    ok(Ret, "GetOverlappedResultEx failed with %lu\n", GetLastError());
    ok_long(Transferred, 5);

    /* A completed operation needs no wait */
    Transferred = 0;
    Ret = pGetOverlappedResultEx(Server, &Overlapped, &Transferred, 0, FALSE);
    ok(Ret, "GetOverlappedResultEx failed with %lu\n", GetLastError());
    ok_long(Transferred, 5);

    CloseHandle(Overlapped.hEvent);
    CloseHandle(Client);
    CloseHandle(Server);
}
