/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for RpcMgmtWaitServerListen
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * Once the server has stopped listening, RpcMgmtWaitServerListen returns as
 * soon as the calls in progress are done.  A client that has made its call
 * and keeps its binding (an idle connection) does not hold it up.
 */

#include <apitest.h>
#include <rpc.h>
#include <strsafe.h>
#include "waitlisten_s.h"

#define WAIT_LISTEN_TIMEOUT_MS  (15 * 1000)

int
s_Ping(handle_t hBinding, int Value)
{
    UNREFERENCED_PARAMETER(hBinding);
    return Value + 1;
}

void __RPC_FAR * __RPC_USER
MIDL_user_allocate(SIZE_T Size)
{
    return HeapAlloc(GetProcessHeap(), 0, Size);
}

void __RPC_USER
MIDL_user_free(void __RPC_FAR *Pointer)
{
    HeapFree(GetProcessHeap(), 0, Pointer);
}

static DWORD WINAPI
WaitListenThread(LPVOID Parameter)
{
    return RpcMgmtWaitServerListen();
}

static int
CallPing(handle_t hBinding, int Value)
{
    int Result = -1;

    RpcTryExcept
    {
        Result = Ping(hBinding, Value);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        trace("Ping raised 0x%lx\n", RpcExceptionCode());
    }
    RpcEndExcept;
    return Result;
}

START_TEST(RpcMgmtWaitServerListen)
{
    WCHAR Endpoint[64];
    RPC_WSTR StringBinding = NULL;
    RPC_BINDING_HANDLE hBinding = NULL;
    RPC_STATUS Status;
    HANDLE hThread;
    DWORD Wait, Result;

    StringCchPrintfW(Endpoint, ARRAYSIZE(Endpoint), L"rostest_waitlisten_%lu",
                     GetCurrentProcessId());

    Status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
                                    (RPC_WSTR)Endpoint, NULL);
    ok(Status == RPC_S_OK, "RpcServerUseProtseqEpW returned %ld\n", Status);
    Status = RpcServerRegisterIf(s_WaitListenTest_v1_0_s_ifspec, NULL, NULL);
    ok(Status == RPC_S_OK, "RpcServerRegisterIf returned %ld\n", Status);
    Status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    ok(Status == RPC_S_OK, "RpcServerListen returned %ld\n", Status);
    if (Status != RPC_S_OK)
        return;

    /* A client connection, idle once its call is done. */
    Status = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL, (RPC_WSTR)Endpoint,
                                      NULL, &StringBinding);
    ok(Status == RPC_S_OK, "RpcStringBindingComposeW returned %ld\n", Status);
    Status = RpcBindingFromStringBindingW(StringBinding, &hBinding);
    ok(Status == RPC_S_OK, "RpcBindingFromStringBindingW returned %ld\n", Status);
    RpcStringFreeW(&StringBinding);
    if (Status != RPC_S_OK)
    {
        RpcMgmtStopServerListening(NULL);
        RpcMgmtWaitServerListen();
        return;
    }
    ok(CallPing(hBinding, 41) == 42, "Ping failed\n");

    Status = RpcMgmtStopServerListening(NULL);
    ok(Status == RPC_S_OK, "RpcMgmtStopServerListening returned %ld\n", Status);

    /* The binding is still held: the wait must not depend on it. */
    hThread = CreateThread(NULL, 0, WaitListenThread, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (hThread == NULL)
    {
        RpcBindingFree(&hBinding);
        return;
    }
    Wait = WaitForSingleObject(hThread, WAIT_LISTEN_TIMEOUT_MS);
    ok(Wait == WAIT_OBJECT_0, "RpcMgmtWaitServerListen did not return while a client held an idle connection\n");

    /* Let a stuck wait finish, so that the process can clean up. */
    RpcBindingFree(&hBinding);
    if (Wait != WAIT_OBJECT_0)
        Wait = WaitForSingleObject(hThread, WAIT_LISTEN_TIMEOUT_MS);
    if (Wait == WAIT_OBJECT_0 && GetExitCodeThread(hThread, &Result))
        ok(Result == RPC_S_OK, "RpcMgmtWaitServerListen returned %lu\n", Result);
    CloseHandle(hThread);

    Status = RpcServerUnregisterIf(s_WaitListenTest_v1_0_s_ifspec, NULL, FALSE);
    ok(Status == RPC_S_OK, "RpcServerUnregisterIf returned %ld\n", Status);
}
