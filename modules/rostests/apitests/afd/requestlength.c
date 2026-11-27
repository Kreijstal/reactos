/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that AFD fails a request whose input is shorter than its structure
 */

#include "precomp.h"

/* A request that pends past this is cancelled, and counts as having pended */
#define REQUEST_TIMEOUT_MS 5000

static
NTSTATUS
AfdRequest(
    _In_ HANDLE SocketHandle,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(InputLength) PVOID Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID Output,
    _In_ ULONG OutputLength)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus, CancelStatus;
    LARGE_INTEGER Timeout;
    HANDLE Event;

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok(Status == STATUS_SUCCESS, "NtCreateEvent failed with %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtDeviceIoControlFile(SocketHandle,
                                   Event,
                                   NULL,
                                   NULL,
                                   &IoStatus,
                                   IoControlCode,
                                   Input,
                                   InputLength,
                                   Output,
                                   OutputLength);
    if (Status == STATUS_PENDING)
    {
        Timeout.QuadPart = -(LONGLONG)REQUEST_TIMEOUT_MS * 10000;
        if (NtWaitForSingleObject(Event, FALSE, &Timeout) == STATUS_TIMEOUT)
        {
            NtCancelIoFile(SocketHandle, &CancelStatus);
            NtWaitForSingleObject(Event, FALSE, &Timeout);
        }
        else
        {
            Status = IoStatus.Status;
        }
    }

    NtClose(Event);
    return Status;
}

static
HANDLE
CreateSocket(
    _In_ int SocketType,
    _In_ int Protocol)
{
    NTSTATUS Status;
    HANDLE SocketHandle;

    Status = AfdCreateSocket(&SocketHandle, AF_INET, SocketType, Protocol);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed with %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return NULL;

    return SocketHandle;
}

typedef struct _REQUEST_LENGTH
{
    PCSTR Name;
    ULONG IoControlCode;
    /* The least input the request takes */
    ULONG Length;
    /* What a shorter input fails with */
    NTSTATUS ShortStatus;
} REQUEST_LENGTH;

#define REQUEST(Code, Length, ShortStatus) { #Code, Code, Length, ShortStatus }

/* Each request that reads a structure from its input.  The datagram send
 * comes last: one an AFD reads past the end of takes a pointer from beyond
 * it. */
static const REQUEST_LENGTH Requests[] =
{
    REQUEST(IOCTL_AFD_BIND, FIELD_OFFSET(AFD_BIND_DATA, Address.Address[0].Address), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_CONNECT, FIELD_OFFSET(AFD_CONNECT_INFO, RemoteAddress.Address[0].Address), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SUPER_CONNECT, FIELD_OFFSET(AFD_SUPER_CONNECT_INFO, RemoteAddress.Address[0].Address), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_START_LISTEN, sizeof(AFD_LISTEN_DATA), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_ACCEPT, sizeof(AFD_ACCEPT_DATA), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SUPER_ACCEPT, sizeof(AFD_SUPER_ACCEPT_INFO), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_DEFER_ACCEPT, sizeof(AFD_DEFER_ACCEPT_DATA), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_RECV, sizeof(AFD_RECV_INFO), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_RECV_DATAGRAM, sizeof(AFD_RECV_INFO_UDP), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SEND, sizeof(AFD_SEND_INFO), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SELECT, FIELD_OFFSET(AFD_POLL_INFO, Handles), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_DISCONNECT, sizeof(AFD_DISCONNECT_INFO), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_GET_INFO, sizeof(AFD_INFO), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SET_INFO, sizeof(AFD_INFO), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_EVENT_SELECT, sizeof(AFD_EVENT_SELECT_INFO), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_ENUM_NETWORK_EVENTS, sizeof(AFD_ENUM_NETWORK_EVENTS_INFO), STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SET_CONNECT_DATA_SIZE, sizeof(UINT), STATUS_BUFFER_TOO_SMALL),
    REQUEST(IOCTL_AFD_SET_CONNECT_OPTIONS_SIZE, sizeof(UINT), STATUS_BUFFER_TOO_SMALL),
    REQUEST(IOCTL_AFD_SET_DISCONNECT_DATA_SIZE, sizeof(UINT), STATUS_BUFFER_TOO_SMALL),
    REQUEST(IOCTL_AFD_SET_DISCONNECT_OPTIONS_SIZE, sizeof(UINT), STATUS_BUFFER_TOO_SMALL),
    REQUEST(IOCTL_AFD_GET_TDI_HANDLES, sizeof(ULONG), STATUS_BUFFER_TOO_SMALL),
    REQUEST(IOCTL_AFD_SET_CONTEXT, 1, STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SET_CONNECT_DATA, 1, STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SET_CONNECT_OPTIONS, 1, STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SET_DISCONNECT_DATA, 1, STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SET_DISCONNECT_OPTIONS, 1, STATUS_INVALID_PARAMETER),
    REQUEST(IOCTL_AFD_SEND_DATAGRAM, sizeof(AFD_SEND_INFO_UDP), STATUS_INVALID_PARAMETER),
};

/* Requests one byte short of their structure, which is all zeroes */
static
void
TestShortRequests(void)
{
    NTSTATUS Status;
    HANDLE SocketHandle;
    ULONG i;
    union
    {
        UCHAR Bytes[256];
        AFD_TDI_HANDLE_DATA TdiHandles;
    } Buffer;

    for (i = 0; i < RTL_NUMBER_OF(Requests); i++)
    {
        /* The data and options themselves cannot be short of anything but
         * empty, which TestEmptyRequests() tests */
        if (Requests[i].Length < 2)
            continue;

        ok(Requests[i].Length <= sizeof(Buffer), "%s: %lu bytes\n", Requests[i].Name, Requests[i].Length);

        SocketHandle = CreateSocket(SOCK_STREAM, IPPROTO_TCP);
        if (!SocketHandle)
            continue;

        RtlZeroMemory(&Buffer, sizeof(Buffer));
        Status = AfdRequest(SocketHandle,
                            Requests[i].IoControlCode,
                            &Buffer,
                            Requests[i].Length - 1,
                            &Buffer,
                            Requests[i].IoControlCode == IOCTL_AFD_GET_TDI_HANDLES ? sizeof(Buffer.TdiHandles) :
                            Requests[i].IoControlCode == IOCTL_AFD_SELECT ? Requests[i].Length - 1 : 0);
        ok(Status == Requests[i].ShortStatus,
           "%s with %lu bytes returned %lx, expected %lx\n",
           Requests[i].Name, Requests[i].Length - 1, Status, Requests[i].ShortStatus);

        NtClose(SocketHandle);
    }
}

/* A bind, a connect and a ConnectEx whose transport address ends early */
static
void
TestShortAddress(void)
{
    NTSTATUS Status;
    HANDLE SocketHandle;
    struct sockaddr_in addr;
    union
    {
        AFD_BIND_DATA Bind;
        AFD_CONNECT_INFO Connect;
        AFD_SUPER_CONNECT_INFO SuperConnect;
        UCHAR Bytes[128];
    } Buffer;
    PTRANSPORT_ADDRESS Address;
    ULONG AddressOffset;
    ULONG i;
    static const struct
    {
        PCSTR Name;
        ULONG IoControlCode;
        int SocketType;
        int Protocol;
        ULONG AddressOffset;
    } Tests[] =
    {
        { "IOCTL_AFD_BIND", IOCTL_AFD_BIND, SOCK_STREAM, IPPROTO_TCP, FIELD_OFFSET(AFD_BIND_DATA, Address) },
        { "IOCTL_AFD_CONNECT", IOCTL_AFD_CONNECT, SOCK_DGRAM, IPPROTO_UDP, FIELD_OFFSET(AFD_CONNECT_INFO, RemoteAddress) },
        { "IOCTL_AFD_SUPER_CONNECT", IOCTL_AFD_SUPER_CONNECT, SOCK_DGRAM, IPPROTO_UDP, FIELD_OFFSET(AFD_SUPER_CONNECT_INFO, RemoteAddress) },
    };

    for (i = 0; i < RTL_NUMBER_OF(Tests); i++)
    {
        SocketHandle = CreateSocket(Tests[i].SocketType, Tests[i].Protocol);
        if (!SocketHandle)
            continue;

        /* An IPv4 address whose length says 14 bytes, with only the port
         * there */
        AddressOffset = Tests[i].AddressOffset;
        RtlZeroMemory(&Buffer, sizeof(Buffer));
        Buffer.Bind.ShareType = AFD_SHARE_UNIQUE;
        Address = (PTRANSPORT_ADDRESS)&Buffer.Bytes[AddressOffset];
        Address->TAAddressCount = 1;
        Address->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
        Address->Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
        Status = AfdRequest(SocketHandle,
                            Tests[i].IoControlCode,
                            &Buffer,
                            AddressOffset + FIELD_OFFSET(TRANSPORT_ADDRESS, Address[0].Address) + sizeof(USHORT),
                            NULL,
                            0);
        ok(Status == STATUS_INVALID_PARAMETER, "%s with a short address returned %lx\n", Tests[i].Name, Status);

        /* One byte short */
        Status = AfdRequest(SocketHandle,
                            Tests[i].IoControlCode,
                            &Buffer,
                            AddressOffset + FIELD_OFFSET(TRANSPORT_ADDRESS, Address[0].Address) + TDI_ADDRESS_LENGTH_IP - 1,
                            NULL,
                            0);
        ok(Status == STATUS_INVALID_PARAMETER, "%s one byte short returned %lx\n", Tests[i].Name, Status);

        if (Tests[i].IoControlCode == IOCTL_AFD_BIND)
        {
            /* The socket is as it was: it binds */
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            addr.sin_port = htons(0);
            Status = AfdBind(SocketHandle, (const struct sockaddr *)&addr, sizeof(addr));
            ok(Status == STATUS_SUCCESS, "AfdBind failed with %lx\n", Status);
        }

        NtClose(SocketHandle);
    }
}

/* A select whose handle count goes past its input, or past its output */
static
void
TestShortSelect(void)
{
    NTSTATUS Status;
    HANDLE SocketHandle;
    struct
    {
        AFD_POLL_INFO Info;
        AFD_HANDLE MoreHandles[1];
    } Poll;
    ULONG OneHandle = FIELD_OFFSET(AFD_POLL_INFO, Handles[1]);

    SocketHandle = CreateSocket(SOCK_STREAM, IPPROTO_TCP);
    if (!SocketHandle)
        return;

    RtlZeroMemory(&Poll, sizeof(Poll));
    Poll.Info.Timeout.QuadPart = 0;
    Poll.Info.Exclusive = FALSE;
    Poll.Info.Handles[0].Handle = (SOCKET)SocketHandle;
    Poll.Info.Handles[0].Events = AFD_EVENT_SEND | AFD_EVENT_RECEIVE;
    Poll.MoreHandles[0] = Poll.Info.Handles[0];

    /* Two handles counted, one there */
    Poll.Info.HandleCount = 2;
    Status = AfdRequest(SocketHandle, IOCTL_AFD_SELECT, &Poll, OneHandle, &Poll, OneHandle);
    ok(Status == STATUS_INVALID_PARAMETER, "IOCTL_AFD_SELECT with 2 handles in room for 1 returned %lx\n", Status);

    /* A count whose size wraps around */
    Poll.Info.HandleCount = 0x80000000UL / sizeof(AFD_HANDLE) * 2 + 1;
    Status = AfdRequest(SocketHandle, IOCTL_AFD_SELECT, &Poll, OneHandle, &Poll, OneHandle);
    ok(Status == STATUS_INVALID_PARAMETER, "IOCTL_AFD_SELECT with %lu handles returned %lx\n", Poll.Info.HandleCount, Status);

    /* Room for both in the input, but not in the output */
    Poll.Info.HandleCount = 2;
    Status = AfdRequest(SocketHandle, IOCTL_AFD_SELECT, &Poll, sizeof(Poll), &Poll, OneHandle);
    ok(Status == STATUS_INVALID_PARAMETER, "IOCTL_AFD_SELECT with room for 1 of 2 handles in its output returned %lx\n", Status);

    /* Room for it: the poll is taken, and times out at once if nothing is signalled */
    Poll.Info.HandleCount = 1;
    Status = AfdRequest(SocketHandle, IOCTL_AFD_SELECT, &Poll, OneHandle, &Poll, OneHandle);
    ok(Status == STATUS_SUCCESS || Status == STATUS_TIMEOUT, "IOCTL_AFD_SELECT with 1 handle returned %lx\n", Status);

    NtClose(SocketHandle);
}

/* Requests with no input at all.  Last: an AFD that does not check takes them
 * apart. */
static
void
TestEmptyRequests(void)
{
    NTSTATUS Status;
    HANDLE SocketHandle;
    ULONG i;
    AFD_TDI_HANDLE_DATA TdiHandles;

    for (i = 0; i < RTL_NUMBER_OF(Requests); i++)
    {
        SocketHandle = CreateSocket(SOCK_STREAM, IPPROTO_TCP);
        if (!SocketHandle)
            continue;

        Status = AfdRequest(SocketHandle,
                            Requests[i].IoControlCode,
                            NULL,
                            0,
                            Requests[i].IoControlCode == IOCTL_AFD_GET_TDI_HANDLES ? &TdiHandles : NULL,
                            Requests[i].IoControlCode == IOCTL_AFD_GET_TDI_HANDLES ? sizeof(TdiHandles) : 0);
        ok(Status == Requests[i].ShortStatus,
           "%s with no input returned %lx, expected %lx\n",
           Requests[i].Name, Status, Requests[i].ShortStatus);

        NtClose(SocketHandle);
    }
}

START_TEST(requestlength)
{
    TestShortRequests();
    TestShortAddress();
    TestShortSelect();
    TestEmptyRequests();
}
