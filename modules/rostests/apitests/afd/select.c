/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for IOCTL_AFD_SELECT completion when a polled socket is closed
 */

#include "precomp.h"

#define POLL_HANDLES 3

typedef struct _POLL_BUFFER
{
    AFD_POLL_INFO Info;
    AFD_HANDLE MoreHandles[POLL_HANDLES - 1];
} POLL_BUFFER;

static
HANDLE
CreateTcpSocket(void)
{
    NTSTATUS Status;
    HANDLE SocketHandle;

    Status = AfdCreateSocket(&SocketHandle, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed with %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return NULL;

    return SocketHandle;
}

/* Starts a poll on Count handles, all of them SocketHandle */
static
NTSTATUS
StartPoll(
    _In_ HANDLE SocketHandle,
    _In_ ULONG Count,
    _In_ LONGLONG Timeout,
    _In_ HANDLE Event,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _Out_ POLL_BUFFER *Poll)
{
    ULONG i, Length;

    ASSERT(Count <= POLL_HANDLES);
    RtlZeroMemory(Poll, sizeof(*Poll));
    Poll->Info.Timeout.QuadPart = Timeout;
    Poll->Info.HandleCount = Count;
    Poll->Info.Exclusive = FALSE;
    for (i = 0; i < Count; i++)
    {
        Poll->Info.Handles[i].Handle = (SOCKET)SocketHandle;
        /* What ws2_32 asks for a socket in readfds, writefds and exceptfds */
        Poll->Info.Handles[i].Events = (i == 0) ? (AFD_EVENT_RECEIVE | AFD_EVENT_DISCONNECT |
                                                   AFD_EVENT_ABORT | AFD_EVENT_CLOSE |
                                                   AFD_EVENT_ACCEPT) :
                                       (i == 1) ? AFD_EVENT_OOB_RECEIVE :
                                                  AFD_EVENT_CONNECT_FAIL;
    }
    Length = FIELD_OFFSET(AFD_POLL_INFO, Handles) + Count * sizeof(AFD_HANDLE);

    IoStatus->Status = 0xdeadbeef;
    IoStatus->Information = 0xdeadbeef;
    return NtDeviceIoControlFile(SocketHandle,
                                 Event,
                                 NULL,
                                 NULL,
                                 IoStatus,
                                 IOCTL_AFD_SELECT,
                                 &Poll->Info,
                                 Length,
                                 &Poll->Info,
                                 Length);
}

/* A socket named several times in one poll and closed while it waits */
static
void
TestCloseWakesPoll(ULONG Count)
{
    NTSTATUS Status;
    HANDLE SocketHandle, Event;
    IO_STATUS_BLOCK IoStatus;
    POLL_BUFFER Poll;
    LARGE_INTEGER Wait;
    ULONG i;

    SocketHandle = CreateTcpSocket();
    if (!SocketHandle)
        return;

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok(Status == STATUS_SUCCESS, "NtCreateEvent failed with %lx\n", Status);

    /* 10 s relative */
    Status = StartPoll(SocketHandle, Count, -100000000LL, Event, &IoStatus, &Poll);
    ok(Status == STATUS_PENDING, "[%lu] IOCTL_AFD_SELECT returned %lx\n", Count, Status);
    if (Status != STATUS_PENDING)
    {
        NtClose(SocketHandle);
        NtClose(Event);
        return;
    }

    Sleep(100);
    Status = NtClose(SocketHandle);
    ok(Status == STATUS_SUCCESS, "NtClose failed with %lx\n", Status);

    /* Closing the socket completes the poll at once, not after its timeout */
    Wait.QuadPart = -30000000LL;
    Status = NtWaitForSingleObject(Event, FALSE, &Wait);
    ok(Status == STATUS_SUCCESS, "[%lu] poll not completed by the close: %lx\n", Count, Status);
    if (Status != STATUS_SUCCESS)
    {
        /* Do not leave the kernel writing into our stack */
        NtWaitForSingleObject(Event, FALSE, NULL);
    }

    ok(IoStatus.Status == STATUS_SUCCESS, "[%lu] IoStatus.Status = %lx\n", Count, IoStatus.Status);
    ok(Poll.Info.HandleCount >= 1 && Poll.Info.HandleCount <= Count,
       "[%lu] HandleCount = %lu\n", Count, Poll.Info.HandleCount);
    ok(IoStatus.Information == FIELD_OFFSET(AFD_POLL_INFO, Handles) + Poll.Info.HandleCount * sizeof(AFD_HANDLE),
       "[%lu] Information = %Iu\n", Count, IoStatus.Information);

    /* The local close is reported for the closed socket in the read set */
    ok(Poll.Info.HandleCount >= 1 && (Poll.Info.Handles[0].Events & AFD_EVENT_CLOSE),
       "[%lu] Events[0] = %lx\n", Count, Poll.Info.Handles[0].Events);
    for (i = 0; i < Poll.Info.HandleCount && i < Count; i++)
    {
        ok(Poll.Info.Handles[i].Handle == (SOCKET)SocketHandle,
           "[%lu] Handles[%lu].Handle = %Ix\n", Count, i, Poll.Info.Handles[i].Handle);
    }

    NtClose(Event);
}

/* A poll that names a socket several times times out normally */
static
void
TestPollTimeout(ULONG Count)
{
    NTSTATUS Status;
    HANDLE SocketHandle, Event;
    IO_STATUS_BLOCK IoStatus;
    POLL_BUFFER Poll;
    LARGE_INTEGER Wait;
    DWORD Ticks;

    SocketHandle = CreateTcpSocket();
    if (!SocketHandle)
        return;

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok(Status == STATUS_SUCCESS, "NtCreateEvent failed with %lx\n", Status);

    Ticks = GetTickCount();
    /* 200 ms relative */
    Status = StartPoll(SocketHandle, Count, -2000000LL, Event, &IoStatus, &Poll);
    ok(Status == STATUS_PENDING, "[%lu] IOCTL_AFD_SELECT returned %lx\n", Count, Status);
    if (Status == STATUS_PENDING)
    {
        Wait.QuadPart = -50000000LL;
        Status = NtWaitForSingleObject(Event, FALSE, &Wait);
        ok(Status == STATUS_SUCCESS, "[%lu] poll did not time out: %lx\n", Count, Status);
        if (Status != STATUS_SUCCESS)
            NtWaitForSingleObject(Event, FALSE, NULL);
        Ticks = GetTickCount() - Ticks;
        ok(IoStatus.Status == STATUS_TIMEOUT, "[%lu] IoStatus.Status = %lx\n", Count, IoStatus.Status);
        ok(Ticks >= 150, "[%lu] timed out after %lu ms\n", Count, Ticks);
    }

    NtClose(SocketHandle);
    NtClose(Event);
}

START_TEST(select)
{
    WSADATA WsaData;
    ULONG Count;

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");

    for (Count = 1; Count <= POLL_HANDLES; Count++)
    {
        TestPollTimeout(Count);
        TestCloseWakesPoll(Count);
    }

    WSACleanup();
}
