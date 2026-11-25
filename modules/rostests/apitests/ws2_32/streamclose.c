/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test large blocking TCP sends and closesocket linger behaviour
 */

#include "ws2_32.h"

/* Every blocking call runs on a worker thread and is given this long (or a
 * call-specific cap) before it counts as hung, so a hang fails the test
 * instead of hanging it */
#define HANG_MS 30000

typedef enum _STREAM_OP
{
    OpSend,
    OpSendUntilError,
    OpRecvAll,
    OpClose
} STREAM_OP;

typedef struct _STREAM_CALL
{
    STREAM_OP Op;
    SOCKET Socket;
    char *Buffer;
    int Length;
    BOOL Verify;
    int Result;
    int Error;
    int Iterations;
    SIZE_T Total;
    SSIZE_T Mismatch;
    DWORD Elapsed;
    HANDLE Thread;
} STREAM_CALL, *PSTREAM_CALL;

/* Set once a call never returned; its socket is then left alone and the
 * process exit cleans up */
static BOOL g_Hung;

static
BYTE
Pattern(SIZE_T Offset)
{
    return (BYTE)((Offset * 131) ^ (Offset >> 9) ^ (Offset >> 17));
}

static
DWORD
WINAPI
StreamThread(PVOID Parameter)
{
    PSTREAM_CALL Call = Parameter;
    static const int ChunkSize = 64 * 1024;
    char *Chunk;
    DWORD Start;
    int i;

    Start = GetTickCount();
    switch (Call->Op)
    {
        case OpSend:
            Call->Result = send(Call->Socket, Call->Buffer, Call->Length, 0);
            break;

        case OpSendUntilError:
            for (Call->Iterations = 0; Call->Iterations < 1024; Call->Iterations++)
            {
                Start = GetTickCount();
                Call->Result = send(Call->Socket, Call->Buffer, Call->Length, 0);
                if (Call->Result == SOCKET_ERROR)
                    break;
            }
            break;

        case OpRecvAll:
            Call->Mismatch = -1;
            Chunk = HeapAlloc(GetProcessHeap(), 0, ChunkSize);
            if (!Chunk)
            {
                Call->Result = SOCKET_ERROR;
                break;
            }
            for (;;)
            {
                Call->Result = recv(Call->Socket, Chunk, ChunkSize, 0);
                if (Call->Result <= 0)
                    break;
                if (Call->Verify && Call->Mismatch < 0)
                {
                    for (i = 0; i < Call->Result; i++)
                    {
                        if ((BYTE)Chunk[i] != Pattern(Call->Total + i))
                        {
                            Call->Mismatch = Call->Total + i;
                            break;
                        }
                    }
                }
                Call->Total += Call->Result;
            }
            HeapFree(GetProcessHeap(), 0, Chunk);
            break;

        case OpClose:
            Call->Result = closesocket(Call->Socket);
            break;
    }
    Call->Error = (Call->Result == SOCKET_ERROR) ? WSAGetLastError() : 0;
    Call->Elapsed = GetTickCount() - Start;
    return 0;
}

static
PSTREAM_CALL
StartCall(STREAM_OP Op, SOCKET Socket, char *Buffer, int Length, BOOL Verify)
{
    PSTREAM_CALL Call;

    Call = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Call));
    ok(Call != NULL, "HeapAlloc failed\n");
    if (!Call)
        return NULL;
    Call->Op = Op;
    Call->Socket = Socket;
    Call->Buffer = Buffer;
    Call->Length = Length;
    Call->Verify = Verify;
    Call->Thread = CreateThread(NULL, 0, StreamThread, Call, 0, NULL);
    ok(Call->Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Call->Thread)
    {
        HeapFree(GetProcessHeap(), 0, Call);
        return NULL;
    }
    return Call;
}

/* TRUE once the call has returned; FALSE if it is still running after Ms */
static
BOOL
WaitCall(PSTREAM_CALL Call, DWORD Ms)
{
    return WaitForSingleObject(Call->Thread, Ms) == WAIT_OBJECT_0;
}

/* Waits for the call; a call still running after Ms has hung. Its thread,
 * socket, buffers and call block are then left as they are. */
static
BOOL
FinishCall(PSTREAM_CALL Call, DWORD Ms, PCSTR What)
{
    if (!Call)
        return FALSE;
    if (!WaitCall(Call, Ms))
    {
        ok(0, "%s did not return within %lu ms\n", What, Ms);
        g_Hung = TRUE;
        return FALSE;
    }
    CloseHandle(Call->Thread);
    Call->Thread = NULL;
    return TRUE;
}

static
void
FreeCall(PSTREAM_CALL Call)
{
    if (Call && !Call->Thread)
        HeapFree(GetProcessHeap(), 0, Call);
}

static
void
SetTimeout(SOCKET Socket, int Option, DWORD Ms)
{
    int Err;

    Err = setsockopt(Socket, SOL_SOCKET, Option, (char *)&Ms, sizeof(Ms));
    ok(Err == 0, "setsockopt(%d) failed: %d\n", Option, WSAGetLastError());
}

static
BOOL
MakePair(SOCKET *Client, SOCKET *Server)
{
    SOCKET Listener;
    struct sockaddr_in Addr;
    int AddrLen = sizeof(Addr);
    int Err;

    *Client = *Server = INVALID_SOCKET;

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Listener == INVALID_SOCKET)
        return FALSE;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Err = bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Err == 0, "bind failed: %d\n", WSAGetLastError());
    Err = getsockname(Listener, (struct sockaddr *)&Addr, &AddrLen);
    ok(Err == 0, "getsockname failed: %d\n", WSAGetLastError());
    Err = listen(Listener, 1);
    ok(Err == 0, "listen failed: %d\n", WSAGetLastError());

    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(*Client != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    Err = connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Err == 0, "connect failed: %d\n", WSAGetLastError());
    *Server = accept(Listener, NULL, NULL);
    ok(*Server != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    closesocket(Listener);

    if (Err != 0 || *Server == INVALID_SOCKET)
    {
        closesocket(*Client);
        if (*Server != INVALID_SOCKET)
            closesocket(*Server);
        *Client = *Server = INVALID_SOCKET;
        return FALSE;
    }

    /* The reader must never wait forever either */
    SetTimeout(*Server, SO_RCVTIMEO, 20000);
    return TRUE;
}

static
char *
AllocPattern(SIZE_T Size)
{
    char *Buffer;
    SIZE_T i;

    Buffer = VirtualAlloc(NULL, Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok(Buffer != NULL, "VirtualAlloc(%Iu) failed: %lu\n", Size, GetLastError());
    if (Buffer)
    {
        for (i = 0; i < Size; i++)
            Buffer[i] = (char)Pattern(i);
    }
    return Buffer;
}

/*
 * A blocking send() on a stream socket does not return until the transport has
 * taken every byte, however much larger than the send buffer the request is,
 * and the peer receives all of it, in order.
 */
static
void
Test_LargeBlockingSend(void)
{
    const int Size = 64 * 1024 * 1024;
    SOCKET Client, Server;
    PSTREAM_CALL Sender, Reader;
    char *Big;

    Big = AllocPattern(Size);
    if (!Big)
        return;
    if (!MakePair(&Client, &Server))
        return;

    Reader = StartCall(OpRecvAll, Server, NULL, 0, TRUE);
    Sender = StartCall(OpSend, Client, Big, Size, FALSE);
    if (!FinishCall(Sender, 120000, "64 MB blocking send"))
    {
        /* Say whether the data stopped flowing or only the sender is stuck */
        if (Reader)
        {
            trace("reader %s, %Iu bytes received, result %d, error %d\n",
                  WaitCall(Reader, 0) ? "finished" : "still running",
                  Reader->Total, Reader->Result, Reader->Error);
        }
        return;
    }
    ok(Sender->Result == Size, "send returned %d (error %d), expected %d\n",
       Sender->Result, Sender->Error, Size);

    shutdown(Client, SD_SEND);
    if (!FinishCall(Reader, HANG_MS, "reader"))
        return;
    ok(Reader->Total == (SIZE_T)Size, "peer received %Iu bytes, expected %d\n", Reader->Total, Size);
    ok(Reader->Mismatch == -1, "peer received wrong data at offset %Id\n", Reader->Mismatch);
    ok(Reader->Result == 0, "peer recv ended with %d (error %d), expected a graceful close\n",
       Reader->Result, Reader->Error);

    FreeCall(Sender);
    FreeCall(Reader);
    closesocket(Client);
    closesocket(Server);
    VirtualFree(Big, 0, MEM_RELEASE);
}

/*
 * A blocking send() larger than the send buffer, issued while the buffer is
 * completely full, waits for room instead of failing with WSAEWOULDBLOCK.
 */
static
void
Test_BlockingSendOnFullBuffer(void)
{
    const int FillSize = 1024 * 1024;
    const int Size = 256 * 1024;
    SOCKET Client, Server;
    PSTREAM_CALL Filler, Sender, Reader;
    char *Fill, *Data;

    Fill = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, FillSize);
    Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    ok(Fill && Data, "HeapAlloc failed\n");
    if (!Fill || !Data)
        return;
    if (!MakePair(&Client, &Server))
        return;

    /* Fill every buffer between the two ends: the peer does not read, so
     * sooner or later a send has to wait, and it times out */
    SetTimeout(Client, SO_SNDTIMEO, 500);
    Filler = StartCall(OpSendUntilError, Client, Fill, FillSize, FALSE);
    if (!FinishCall(Filler, HANG_MS, "filling send"))
        return;
    ok(Filler->Result == SOCKET_ERROR && Filler->Error == WSAETIMEDOUT,
       "filling send returned %d, error %d after %d sends\n",
       Filler->Result, Filler->Error, Filler->Iterations);

    /* No timeout any more: this send has to wait for the peer */
    SetTimeout(Client, SO_SNDTIMEO, 0);
    Sender = StartCall(OpSend, Client, Data, Size, FALSE);
    if (!Sender)
        return;
    ok(!WaitCall(Sender, 1000), "blocking send on a full buffer returned %d (error %d) without waiting\n",
       Sender->Result, Sender->Error);

    /* Once the peer reads, it goes through in full */
    Reader = StartCall(OpRecvAll, Server, NULL, 0, FALSE);
    if (!FinishCall(Sender, HANG_MS, "blocking send on a full buffer"))
        return;
    ok(Sender->Result == Size, "send returned %d (error %d), expected %d\n",
       Sender->Result, Sender->Error, Size);

    shutdown(Client, SD_SEND);
    if (!FinishCall(Reader, HANG_MS, "reader"))
        return;
    ok(Reader->Total >= (SIZE_T)Size, "peer received %Iu bytes, expected at least %d\n", Reader->Total, Size);
    ok(Reader->Result == 0, "peer recv ended with %d (error %d)\n", Reader->Result, Reader->Error);

    FreeCall(Filler);
    FreeCall(Sender);
    FreeCall(Reader);
    closesocket(Client);
    closesocket(Server);
    HeapFree(GetProcessHeap(), 0, Fill);
    HeapFree(GetProcessHeap(), 0, Data);
}

/*
 * closesocket() with the default SO_DONTLINGER returns at once even though
 * sent data is still waiting for a peer that does not read: the graceful close
 * finishes in the background, and the peer gets the data, then the FIN.  Here
 * the queued data is what a timed-out blocking send left behind.
 */
static
void
Test_CloseAfterTimedOutSend(void)
{
    const int FillSize = 1024 * 1024;
    SOCKET Client, Server;
    PSTREAM_CALL Filler, Closer, Reader;
    char *Fill;

    Fill = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, FillSize);
    ok(Fill != NULL, "HeapAlloc failed\n");
    if (!Fill)
        return;
    if (!MakePair(&Client, &Server))
        return;

    SetTimeout(Client, SO_SNDTIMEO, 500);
    Filler = StartCall(OpSendUntilError, Client, Fill, FillSize, FALSE);
    if (!FinishCall(Filler, HANG_MS, "filling send"))
        return;
    ok(Filler->Result == SOCKET_ERROR && Filler->Error == WSAETIMEDOUT,
       "filling send returned %d, error %d after %d sends\n",
       Filler->Result, Filler->Error, Filler->Iterations);

    Closer = StartCall(OpClose, Client, NULL, 0, FALSE);
    if (!FinishCall(Closer, 10000, "closesocket with unsent data"))
        return;
    ok(Closer->Result == 0, "closesocket returned %d, error %d\n", Closer->Result, Closer->Error);
    ok(Closer->Elapsed < 1000, "closesocket took %lu ms, expected it to return at once\n", Closer->Elapsed);

    Reader = StartCall(OpRecvAll, Server, NULL, 0, FALSE);
    if (!FinishCall(Reader, HANG_MS, "reader"))
        return;
    ok(Reader->Total > 0, "peer received no data\n");
    ok(Reader->Result == 0, "peer recv ended with %d (error %d), expected a graceful close\n",
       Reader->Result, Reader->Error);
    trace("peer received %Iu bytes\n", Reader->Total);

    FreeCall(Filler);
    FreeCall(Closer);
    FreeCall(Reader);
    closesocket(Server);
    HeapFree(GetProcessHeap(), 0, Fill);
}

typedef enum _LINGER_MODE
{
    LingerDefault,      /* SO_DONTLINGER: graceful close in the background */
    LingerTimeout,      /* { 1, 1 }: wait up to a second, then reset */
    LingerAbort         /* { 1, 0 }: reset at once */
} LINGER_MODE;

/*
 * closesocket() on a connected socket whose peer is not reading, with an
 * overlapped send still waiting for buffer space.
 */
static
void
Test_CloseWithQueuedData(LINGER_MODE Mode)
{
    static const PCSTR Names[] = { "SO_DONTLINGER", "SO_LINGER {1,1}", "SO_LINGER {1,0}" };
    const int Size = 4 * 1024 * 1024;
    SOCKET Client, Server;
    PSTREAM_CALL Closer, Reader;
    WSAOVERLAPPED Overlapped;
    WSABUF WsaBuf;
    DWORD Sent, Wait;
    struct linger Linger;
    char *Big;
    int Err;

    Big = AllocPattern(Size);
    if (!Big)
        return;
    if (!MakePair(&Client, &Server))
        return;

    if (Mode != LingerDefault)
    {
        Linger.l_onoff = 1;
        Linger.l_linger = (Mode == LingerTimeout) ? 1 : 0;
        Err = setsockopt(Client, SOL_SOCKET, SO_LINGER, (char *)&Linger, sizeof(Linger));
        ok(Err == 0, "setsockopt(SO_LINGER) failed: %d\n", WSAGetLastError());
    }

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = WSACreateEvent();
    WsaBuf.buf = Big;
    WsaBuf.len = Size;
    Err = WSASend(Client, &WsaBuf, 1, &Sent, 0, &Overlapped, NULL);
    ok(Err == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING,
       "%s: WSASend returned %d, error %d\n", Names[Mode], Err, WSAGetLastError());
    Wait = WaitForSingleObject(Overlapped.hEvent, 500);
    ok(Wait == WAIT_TIMEOUT, "%s: the overlapped send finished (%lu) although the peer does not read\n",
       Names[Mode], Wait);

    Closer = StartCall(OpClose, Client, NULL, 0, FALSE);
    if (!FinishCall(Closer, 10000, Names[Mode]))
        return;
    ok(Closer->Result == 0, "%s: closesocket returned %d, error %d\n",
       Names[Mode], Closer->Result, Closer->Error);
    switch (Mode)
    {
        case LingerDefault:
        case LingerAbort:
            ok(Closer->Elapsed < 1000, "%s: closesocket took %lu ms, expected it to return at once\n",
               Names[Mode], Closer->Elapsed);
            break;
        case LingerTimeout:
            ok(Closer->Elapsed >= 800 && Closer->Elapsed <= 3000,
               "%s: closesocket took %lu ms, expected about 1000 ms\n", Names[Mode], Closer->Elapsed);
            break;
    }

    /* The overlapped send does not outlive the socket */
    Wait = WaitForSingleObject(Overlapped.hEvent, 5000);
    ok(Wait == WAIT_OBJECT_0, "%s: the overlapped send did not complete after closesocket (%lu)\n",
       Names[Mode], Wait);
    trace("%s: overlapped send status 0x%lx, %Iu bytes\n",
          Names[Mode], (ULONG)Overlapped.Internal, Overlapped.InternalHigh);

    Reader = StartCall(OpRecvAll, Server, NULL, 0, TRUE);
    if (!FinishCall(Reader, HANG_MS, "reader"))
        return;
    ok(Reader->Mismatch == -1, "%s: peer received wrong data at offset %Id\n", Names[Mode], Reader->Mismatch);
    if (Mode == LingerDefault)
    {
        /* Everything that was buffered arrives, then the FIN */
        ok(Reader->Total > 0, "%s: peer received no data\n", Names[Mode]);
        if (Overlapped.Internal == 0 /* STATUS_SUCCESS */)
            ok(Reader->Total == (SIZE_T)Size, "%s: peer received %Iu of %d bytes\n",
               Names[Mode], Reader->Total, Size);
        ok(Reader->Result == 0, "%s: peer recv ended with %d (error %d), expected a graceful close\n",
           Names[Mode], Reader->Result, Reader->Error);
    }
    else
    {
        ok(Reader->Result == SOCKET_ERROR && Reader->Error == WSAECONNRESET,
           "%s: peer recv ended with %d (error %d), expected WSAECONNRESET\n",
           Names[Mode], Reader->Result, Reader->Error);
    }
    trace("%s: peer received %Iu bytes\n", Names[Mode], Reader->Total);

    FreeCall(Closer);
    FreeCall(Reader);
    WSACloseEvent(Overlapped.hEvent);
    closesocket(Server);
    VirtualFree(Big, 0, MEM_RELEASE);
}

/*
 * closesocket() with SO_LINGER {1,0} resets the connection even with nothing
 * queued.
 */
static
void
Test_AbortiveClose(void)
{
    SOCKET Client, Server;
    PSTREAM_CALL Reader;
    struct linger Linger = { 1, 0 };
    int Err;

    if (!MakePair(&Client, &Server))
        return;
    Err = setsockopt(Client, SOL_SOCKET, SO_LINGER, (char *)&Linger, sizeof(Linger));
    ok(Err == 0, "setsockopt(SO_LINGER) failed: %d\n", WSAGetLastError());
    Err = closesocket(Client);
    ok(Err == 0, "closesocket returned %d, error %d\n", Err, WSAGetLastError());

    Reader = StartCall(OpRecvAll, Server, NULL, 0, FALSE);
    if (!FinishCall(Reader, HANG_MS, "reader"))
        return;
    ok(Reader->Result == SOCKET_ERROR && Reader->Error == WSAECONNRESET,
       "peer recv ended with %d (error %d), expected WSAECONNRESET\n", Reader->Result, Reader->Error);
    FreeCall(Reader);
    closesocket(Server);
}

START_TEST(streamclose)
{
    WSADATA WsaData;
    int Err;

    Err = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Err == 0, "WSAStartup failed: %d\n", Err);
    if (Err != 0)
        return;

    /* Each test uses its own connection, so one that hangs does not stop
     * the others from running */
    Test_LargeBlockingSend();
    Test_BlockingSendOnFullBuffer();
    Test_CloseAfterTimedOutSend();
    Test_CloseWithQueuedData(LingerDefault);
    Test_CloseWithQueuedData(LingerTimeout);
    Test_CloseWithQueuedData(LingerAbort);
    Test_AbortiveClose();

    if (!g_Hung)
        WSACleanup();
}
