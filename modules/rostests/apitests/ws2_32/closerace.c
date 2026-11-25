/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test socket calls racing a closesocket() of the same socket on another thread
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "ws2_32.h"
#include <strsafe.h>

/*
 * Threads keep calling recv, send, getsockopt, ioctlsocket, getsockname and
 * select on a connected socket while another thread closes it.  A call that
 * races the close either completes or fails (WSAENOTSOCK once the socket is
 * gone); it must never use the provider's state of the socket after
 * closesocket() freed it.  A spray thread keeps reallocating freed heap
 * blocks filled with 0xCC, so a call still using such state faults on the
 * garbage instead of silently reading stale memory.
 *
 * The stress runs in child processes so a crash is counted, not fatal.
 */

#define CHILD_ROUNDS 6
#define CHILD_ITERATIONS 300
#define CHILD_TIMEOUT_MS 25000
#define WORKER_HANG_MS 8000
/* Stay well inside the test runner's time limit */
#define TOTAL_BUDGET_MS 100000

#define EXIT_CLEAN 0
#define EXIT_SETUP_FAILED 0x5e700001
#define EXIT_WORKER_HUNG 0x5e700002
#define EXIT_BAD_RESULT 0x5e700003

typedef enum _WORKER_KIND
{
    WorkerRecv,
    WorkerSend,
    WorkerInfo,
    WorkerSelect,
    WorkerKinds
} WORKER_KIND;

typedef struct _WORKER
{
    WORKER_KIND Kind;
    SOCKET Socket;
    HANDLE Started;
    LONG Calls;
    int LastError;
} WORKER, *PWORKER;

static volatile LONG g_StopSpray;
static LONG g_BadResults;

static
LONG
WINAPI
CrashFilter(PEXCEPTION_POINTERS ExceptionInfo)
{
    /* Report the exception code to the parent instead of a message box */
    TerminateProcess(GetCurrentProcess(), ExceptionInfo->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

static
DWORD
WINAPI
SprayThread(PVOID Parameter)
{
    static PVOID Blocks[64];
    HANDLE Heap = GetProcessHeap();
    ULONG i, Size, Pass = 0;

    UNREFERENCED_PARAMETER(Parameter);

    while (!g_StopSpray)
    {
        /* Over the passes every size up to 4 KB, so whatever block was
         * freed last is soon reused */
        for (i = 0; i < _countof(Blocks); i++)
        {
            Size = ((Pass * _countof(Blocks) + i) % 512 + 1) * 8;
            Blocks[i] = HeapAlloc(Heap, 0, Size);
            if (Blocks[i])
                FillMemory(Blocks[i], Size, 0xCC);
        }
        for (i = 0; i < _countof(Blocks); i++)
        {
            if (Blocks[i])
                HeapFree(Heap, 0, Blocks[i]);
        }
        Pass++;
        SwitchToThread();
    }
    return 0;
}

static
BOOL
IsCloseError(int Error)
{
    switch (Error)
    {
        case WSAENOTSOCK:
        case WSAEINTR:
        case WSA_OPERATION_ABORTED:
        case WSAECONNABORTED:
        case WSAECONNRESET:
        case WSAESHUTDOWN:
        case WSAENOTCONN:
        case WSAEINVAL:
            return TRUE;
    }
    return FALSE;
}

static
DWORD
WINAPI
WorkerThread(PVOID Parameter)
{
    PWORKER Worker = Parameter;
    SOCKET Socket = Worker->Socket;
    char Buffer[512];
    struct sockaddr_in Addr;
    int AddrLen, OptLen, Value, Result;
    u_long Available;
    fd_set ReadFds, ExceptFds;
    struct timeval Timeout = { 0, 0 };

    ZeroMemory(Buffer, sizeof(Buffer));
    SetEvent(Worker->Started);

    for (;;)
    {
        InterlockedIncrement(&Worker->Calls);
        switch (Worker->Kind)
        {
            case WorkerRecv:
                /* The peer never sends: this blocks until the close */
                Result = recv(Socket, Buffer, sizeof(Buffer), 0);
                if (Result == 0)
                    Result = SOCKET_ERROR;
                break;

            case WorkerSend:
                /* The peer never reads: this fills the window, then blocks */
                Result = send(Socket, Buffer, sizeof(Buffer), 0);
                break;

            case WorkerInfo:
                OptLen = sizeof(Value);
                Result = getsockopt(Socket, SOL_SOCKET, SO_RCVBUF, (char *)&Value, &OptLen);
                if (Result == SOCKET_ERROR)
                    break;
                Result = ioctlsocket(Socket, FIONREAD, &Available);
                if (Result == SOCKET_ERROR)
                    break;
                AddrLen = sizeof(Addr);
                Result = getsockname(Socket, (struct sockaddr *)&Addr, &AddrLen);
                if (Result == SOCKET_ERROR)
                    break;
                AddrLen = sizeof(Addr);
                Result = getpeername(Socket, (struct sockaddr *)&Addr, &AddrLen);
                break;

            case WorkerSelect:
                FD_ZERO(&ReadFds);
                FD_ZERO(&ExceptFds);
                FD_SET(Socket, &ReadFds);
                FD_SET(Socket, &ExceptFds);
                Result = select(0, &ReadFds, NULL, &ExceptFds, &Timeout);
                /* A closed socket selects as readable, then the next call fails */
                break;

            default:
                Result = SOCKET_ERROR;
                break;
        }

        if (Result == SOCKET_ERROR)
        {
            Worker->LastError = WSAGetLastError();
            if (!IsCloseError(Worker->LastError) &&
                InterlockedIncrement(&g_BadResults) == 1)
            {
                char Message[96];

                StringCbPrintfA(Message, sizeof(Message),
                                "closerace: worker kind %d failed with %d\n",
                                Worker->Kind, Worker->LastError);
                OutputDebugStringA(Message);
            }
            return 0;
        }
    }
}

static
SOCKET
CreateListener(void)
{
    SOCKET Socket;
    struct sockaddr_in Addr;

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(Socket, (struct sockaddr *)&Addr, sizeof(Addr)) != 0 ||
        listen(Socket, SOMAXCONN) != 0)
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }
    return Socket;
}

static
BOOL
MakePair(SOCKET Listener, SOCKET *Client, SOCKET *Server)
{
    struct sockaddr_in Addr;
    int AddrLen = sizeof(Addr);

    *Server = INVALID_SOCKET;
    if (getsockname(Listener, (struct sockaddr *)&Addr, &AddrLen) != 0)
        return FALSE;

    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*Client == INVALID_SOCKET)
        return FALSE;
    if (connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr)) != 0)
    {
        closesocket(*Client);
        return FALSE;
    }
    *Server = accept(Listener, NULL, NULL);
    if (*Server == INVALID_SOCKET)
    {
        closesocket(*Client);
        return FALSE;
    }
    return TRUE;
}

static
DECLSPEC_NORETURN
VOID
RunChild(void)
{
    WSADATA WsaData;
    WORKER Workers[WorkerKinds];
    HANDLE Threads[WorkerKinds];
    HANDLE Spray;
    SOCKET Listener, Client, Server;
    ULONG Iteration, i;
    DWORD Wait;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(CrashFilter);

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
        ExitProcess(EXIT_SETUP_FAILED);
    Listener = CreateListener();
    if (Listener == INVALID_SOCKET)
        ExitProcess(EXIT_SETUP_FAILED);

    Spray = CreateThread(NULL, 0, SprayThread, NULL, 0, NULL);
    if (!Spray)
        ExitProcess(EXIT_SETUP_FAILED);

    for (Iteration = 0; Iteration < CHILD_ITERATIONS; Iteration++)
    {
        if (!MakePair(Listener, &Client, &Server))
            ExitProcess(EXIT_SETUP_FAILED);

        for (i = 0; i < _countof(Workers); i++)
        {
            Workers[i].Kind = (WORKER_KIND)(i % WorkerKinds);
            Workers[i].Socket = Client;
            Workers[i].Started = CreateEventW(NULL, TRUE, FALSE, NULL);
            Workers[i].Calls = 0;
            Workers[i].LastError = 0;
            Threads[i] = CreateThread(NULL, 0, WorkerThread, &Workers[i], 0, NULL);
            if (!Workers[i].Started || !Threads[i])
                ExitProcess(EXIT_SETUP_FAILED);
        }
        for (i = 0; i < _countof(Workers); i++)
            WaitForSingleObject(Workers[i].Started, INFINITE);

        /* Close while the calls are in full swing, at varying points */
        switch (Iteration % 4)
        {
            case 0: break;
            case 1: SwitchToThread(); break;
            case 2: Sleep(1); break;
            default: Sleep(Iteration % 7); break;
        }
        closesocket(Client);

        Wait = WaitForMultipleObjects(_countof(Threads), Threads, TRUE, WORKER_HANG_MS);
        if (Wait >= WAIT_OBJECT_0 + _countof(Threads))
        {
            char Message[128];

            /* Say which calls the close did not end */
            for (i = 0; i < _countof(Workers); i++)
            {
                if (WaitForSingleObject(Threads[i], 0) == WAIT_OBJECT_0)
                    continue;
                StringCbPrintfA(Message, sizeof(Message),
                                "closerace: iteration %lu: worker kind %d hung after %ld calls\n",
                                Iteration, Workers[i].Kind, Workers[i].Calls);
                OutputDebugStringA(Message);
            }
            ExitProcess(EXIT_WORKER_HUNG);
        }

        for (i = 0; i < _countof(Workers); i++)
        {
            CloseHandle(Threads[i]);
            CloseHandle(Workers[i].Started);
        }
        closesocket(Server);
    }

    InterlockedExchange(&g_StopSpray, TRUE);
    WaitForSingleObject(Spray, INFINITE);
    CloseHandle(Spray);
    closesocket(Listener);
    WSACleanup();

    ExitProcess(g_BadResults ? EXIT_BAD_RESULT : EXIT_CLEAN);
}

START_TEST(closerace)
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

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" closerace child", FileName);

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

        switch (ExitCode)
        {
            case EXIT_CLEAN: Clean++; break;
            case EXIT_WORKER_HUNG: Hung++; break;
            case EXIT_BAD_RESULT: Bad++; break;
            case EXIT_SETUP_FAILED: Other++; break;
            default:
                trace("round %lu: child died with 0x%08lx\n", Round, ExitCode);
                {
                    char Message[64];
                    StringCbPrintfA(Message, sizeof(Message), "closerace: round %lu: child died with 0x%08lx\n", Round, ExitCode);
                    OutputDebugStringA(Message);
                }
                Crashed++;
                break;
        }
    }

    {
        char Summary[160];

        /* Also into the debug log, where the runner does not copy traces */
        StringCbPrintfA(Summary, sizeof(Summary),
                        "closerace: %lu rounds of %d closes: %lu clean, %lu crashed, %lu hung, %lu bad results, %lu setup failures\n",
                        Round, CHILD_ITERATIONS, Clean, Crashed, Hung, Bad, Other);
        trace("%s", Summary);
        OutputDebugStringA(Summary);
    }
    ok(Round > 0, "no round ran\n");
    ok(Crashed == 0, "%lu of %lu child processes crashed\n", Crashed, Round);
    ok(Hung == 0, "%lu of %lu child processes hung\n", Hung, Round);
    ok(Bad == 0, "%lu of %lu child processes saw an unexpected error\n", Bad, Round);
    ok(Other == 0, "%lu of %lu child processes failed to set up\n", Other, Round);
}
