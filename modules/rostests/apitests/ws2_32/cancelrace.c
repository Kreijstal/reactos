/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test cancelling pending receives and accepts while data and connections arrive and the socket is closed
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "ws2_32.h"
#include <mswsock.h>
#include <strsafe.h>

/*
 * Overlapped receives and accepts are cancelled -- by CancelIo, by CancelIoEx
 * from another thread, or by their thread exiting -- at the same moment the
 * transport completes them with arriving data or connections and another
 * thread closes the socket.  Every request must complete exactly once, with
 * its data or as cancelled/closed, and nothing may hang.
 */

#define ITERATIONS 300
#define PENDING 4
#define HANG_MS 15000

typedef enum _CANCEL_MODE
{
    CancelByCancelIo,
    CancelByCancelIoEx,
    CancelByThreadExit,
    CancelByClose,
    CancelModes
} CANCEL_MODE;

typedef BOOL (WINAPI *PFN_CANCELIOEX)(HANDLE, LPOVERLAPPED);

typedef struct _ISSUER
{
    SOCKET Socket;
    SOCKET AcceptSockets[PENDING];
    LPFN_ACCEPTEX AcceptEx;
    BOOL Accept;
    CANCEL_MODE Mode;
    WSAOVERLAPPED Overlapped[PENDING];
    char Buffers[PENDING][64 + 2 * (sizeof(struct sockaddr_in) + 16)];
    HANDLE Issued;
    HANDLE Go;
    HANDLE Done;
    int IssueErrors;
} ISSUER, *PISSUER;

typedef struct _FEEDER
{
    SOCKET Socket;
    struct sockaddr_in Target;
    BOOL Connect;
    volatile LONG Stop;
    LONG Count;
} FEEDER, *PFEEDER;

static PFN_CANCELIOEX pCancelIoEx;
static ULONG g_Hung, g_BadStatus, g_IssueErrors;
static ULONG g_Completed, g_Cancelled, g_Closed, g_Other;

static
DWORD
WINAPI
IssuerThread(PVOID Parameter)
{
    PISSUER Issuer = Parameter;
    WSABUF Buffer;
    DWORD Flags, Bytes;
    ULONG i;
    int Result;

    for (i = 0; i < PENDING; i++)
    {
        if (Issuer->Accept)
        {
            if (!Issuer->AcceptEx(Issuer->Socket, Issuer->AcceptSockets[i],
                                  Issuer->Buffers[i], 0,
                                  sizeof(struct sockaddr_in) + 16,
                                  sizeof(struct sockaddr_in) + 16,
                                  &Bytes, &Issuer->Overlapped[i]) &&
                WSAGetLastError() != ERROR_IO_PENDING)
            {
                Issuer->IssueErrors++;
            }
        }
        else
        {
            Buffer.buf = Issuer->Buffers[i];
            Buffer.len = 64;
            Flags = 0;
            Result = WSARecv(Issuer->Socket, &Buffer, 1, NULL, &Flags,
                             &Issuer->Overlapped[i], NULL);
            if (Result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
                Issuer->IssueErrors++;
        }
    }

    SetEvent(Issuer->Issued);
    WaitForSingleObject(Issuer->Go, INFINITE);

    switch (Issuer->Mode)
    {
        case CancelByCancelIo:
            CancelIo((HANDLE)Issuer->Socket);
            break;

        case CancelByThreadExit:
            /* Exiting cancels everything this thread has pending */
            return 0;

        default:
            break;
    }

    /* Stay alive until the main thread is done with the requests */
    WaitForSingleObject(Issuer->Done, INFINITE);
    return 0;
}

static
DWORD
WINAPI
FeederThread(PVOID Parameter)
{
    PFEEDER Feeder = Parameter;
    SOCKET Socket;
    char Chunk[16];

    FillMemory(Chunk, sizeof(Chunk), 'x');
    while (!Feeder->Stop)
    {
        if (Feeder->Connect)
        {
            Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (Socket == INVALID_SOCKET)
                break;
            if (connect(Socket, (struct sockaddr *)&Feeder->Target, sizeof(Feeder->Target)) == 0)
                InterlockedIncrement(&Feeder->Count);
            closesocket(Socket);
        }
        else
        {
            if (send(Feeder->Socket, Chunk, sizeof(Chunk), 0) != sizeof(Chunk))
                break;
            InterlockedIncrement(&Feeder->Count);
            if (Feeder->Count % 4 == 0)
                Sleep(0);
        }
    }
    return 0;
}

static
SOCKET
CreateListener(struct sockaddr_in *Addr)
{
    SOCKET Socket;
    int AddrLen = sizeof(*Addr);

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;

    ZeroMemory(Addr, sizeof(*Addr));
    Addr->sin_family = AF_INET;
    Addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(Socket, (struct sockaddr *)Addr, sizeof(*Addr)) != 0 ||
        listen(Socket, SOMAXCONN) != 0 ||
        getsockname(Socket, (struct sockaddr *)Addr, &AddrLen) != 0)
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }
    return Socket;
}

static
BOOL
MakePair(SOCKET *Client, SOCKET *Server)
{
    struct sockaddr_in Addr;
    SOCKET Listener;

    *Client = *Server = INVALID_SOCKET;
    Listener = CreateListener(&Addr);
    if (Listener == INVALID_SOCKET)
        return FALSE;

    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*Client != INVALID_SOCKET &&
        connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr)) == 0)
    {
        *Server = accept(Listener, NULL, NULL);
    }
    closesocket(Listener);

    if (*Server == INVALID_SOCKET)
    {
        if (*Client != INVALID_SOCKET)
            closesocket(*Client);
        *Client = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

static
void
CountStatus(NTSTATUS Status)
{
    switch (Status)
    {
        case STATUS_SUCCESS:
            g_Completed++;
            break;
        case STATUS_CANCELLED:
            g_Cancelled++;
            break;
        case STATUS_FILE_CLOSED:
        case STATUS_LOCAL_DISCONNECT:
        case STATUS_REMOTE_DISCONNECT:
        case STATUS_CONNECTION_RESET:
        case STATUS_CONNECTION_ABORTED:
        case STATUS_INVALID_HANDLE:
            g_Closed++;
            break;
        case STATUS_PENDING:
            g_BadStatus++;
            break;
        default:
            if (g_Other++ < 5)
                trace("request completed with 0x%08lx\n", Status);
            break;
    }
}

static
BOOL
RunIteration(ULONG Iteration, BOOL Accept, LPFN_ACCEPTEX AcceptEx)
{
    PISSUER Issuer;
    FEEDER Feeder;
    HANDLE Threads[2], Events[PENDING];
    SOCKET Client = INVALID_SOCKET, Server = INVALID_SOCKET;
    ULONG i;
    DWORD Wait;

    Issuer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Issuer));
    if (!Issuer)
        return FALSE;
    ZeroMemory(&Feeder, sizeof(Feeder));

    if (Accept)
    {
        Issuer->Socket = CreateListener(&Feeder.Target);
        if (Issuer->Socket == INVALID_SOCKET)
            return FALSE;
        for (i = 0; i < PENDING; i++)
            Issuer->AcceptSockets[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Feeder.Connect = TRUE;
    }
    else
    {
        if (!MakePair(&Client, &Server))
            return FALSE;
        Issuer->Socket = Client;
        Feeder.Socket = Server;
        for (i = 0; i < PENDING; i++)
            Issuer->AcceptSockets[i] = INVALID_SOCKET;
    }

    Issuer->Accept = Accept;
    Issuer->AcceptEx = AcceptEx;
    Issuer->Mode = (CANCEL_MODE)(Iteration % CancelModes);
    if (Issuer->Mode == CancelByCancelIoEx && !pCancelIoEx)
        Issuer->Mode = CancelByCancelIo;
    for (i = 0; i < PENDING; i++)
    {
        Issuer->Overlapped[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        Events[i] = Issuer->Overlapped[i].hEvent;
    }
    Issuer->Issued = CreateEventW(NULL, TRUE, FALSE, NULL);
    Issuer->Go = CreateEventW(NULL, TRUE, FALSE, NULL);
    Issuer->Done = CreateEventW(NULL, TRUE, FALSE, NULL);

    Threads[0] = CreateThread(NULL, 0, IssuerThread, Issuer, 0, NULL);
    WaitForSingleObject(Issuer->Issued, INFINITE);
    g_IssueErrors += Issuer->IssueErrors;

    /* Data or connections arrive while the requests are being cancelled */
    Threads[1] = CreateThread(NULL, 0, FeederThread, &Feeder, 0, NULL);
    if (Iteration % 2)
        Sleep(0);

    SetEvent(Issuer->Go);
    if (Issuer->Mode == CancelByCancelIoEx)
        pCancelIoEx((HANDLE)Issuer->Socket, NULL);

    /* ...and the socket is closed under them */
    switch (Iteration % 3)
    {
        case 0: break;
        case 1: SwitchToThread(); break;
        default: Sleep(Iteration % 5); break;
    }
    closesocket(Issuer->Socket);

    Wait = WaitForMultipleObjects(PENDING, Events, TRUE, HANG_MS);
    if (Wait >= WAIT_OBJECT_0 + PENDING)
    {
        /* Leave everything alone; the process exit cleans up */
        g_Hung++;
        return FALSE;
    }
    for (i = 0; i < PENDING; i++)
        CountStatus((NTSTATUS)Issuer->Overlapped[i].Internal);

    InterlockedExchange(&Feeder.Stop, TRUE);
    SetEvent(Issuer->Done);
    Wait = WaitForMultipleObjects(_countof(Threads), Threads, TRUE, HANG_MS);
    if (Wait >= WAIT_OBJECT_0 + _countof(Threads))
    {
        g_Hung++;
        return FALSE;
    }

    for (i = 0; i < _countof(Threads); i++)
        CloseHandle(Threads[i]);
    for (i = 0; i < PENDING; i++)
    {
        CloseHandle(Events[i]);
        if (Issuer->AcceptSockets[i] != INVALID_SOCKET)
            closesocket(Issuer->AcceptSockets[i]);
    }
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    CloseHandle(Issuer->Issued);
    CloseHandle(Issuer->Go);
    CloseHandle(Issuer->Done);
    HeapFree(GetProcessHeap(), 0, Issuer);
    return TRUE;
}

static
void
Test_CancelRace(BOOL Accept)
{
    GUID AcceptExGuid = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX AcceptEx = NULL;
    SOCKET Socket;
    DWORD Bytes;
    ULONG Iteration;

    g_Hung = g_BadStatus = g_IssueErrors = 0;
    g_Completed = g_Cancelled = g_Closed = g_Other = 0;

    if (Accept)
    {
        Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Socket == INVALID_SOCKET ||
            WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &AcceptExGuid, sizeof(AcceptExGuid),
                     &AcceptEx, sizeof(AcceptEx), &Bytes, NULL, NULL) != 0)
        {
            skip("No AcceptEx: %d\n", WSAGetLastError());
            return;
        }
        closesocket(Socket);
    }

    for (Iteration = 0; Iteration < ITERATIONS; Iteration++)
    {
        if (!RunIteration(Iteration, Accept, AcceptEx))
            break;
    }

    {
        char Summary[160];

        /* Also into the debug log, where the runner does not copy traces */
        StringCbPrintfA(Summary, sizeof(Summary),
                        "cancelrace: %s: %lu iterations: %lu completed, %lu cancelled, %lu closed, %lu other\n",
                        Accept ? "accept" : "recv", Iteration, g_Completed, g_Cancelled, g_Closed, g_Other);
        trace("%s", Summary);
        OutputDebugStringA(Summary);
    }
    ok(Iteration == ITERATIONS, "%s: stopped after %lu iterations\n",
       Accept ? "accept" : "recv", Iteration);
    ok(g_Hung == 0, "%s: %lu requests never completed\n", Accept ? "accept" : "recv", g_Hung);
    ok(g_BadStatus == 0, "%s: %lu requests completed as pending\n",
       Accept ? "accept" : "recv", g_BadStatus);
    ok(g_IssueErrors == 0, "%s: %lu requests failed to start\n",
       Accept ? "accept" : "recv", g_IssueErrors);
}

START_TEST(cancelrace)
{
    WSADATA WsaData;
    int Err;

    pCancelIoEx = (PFN_CANCELIOEX)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CancelIoEx");

    Err = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Err == 0, "WSAStartup failed: %d\n", Err);
    if (Err != 0)
        return;

    Test_CancelRace(FALSE);
    Test_CancelRace(TRUE);

    WSACleanup();
}
