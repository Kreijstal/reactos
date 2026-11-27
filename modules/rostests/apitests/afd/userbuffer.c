/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that AFD takes a user-mode caller's buffers only from user memory
 */

#include "precomp.h"

/*
 * The kernel's own mapping of KUSER_SHARED_PAGE, the page user mode sees
 * read-only at 0x7FFE0000.  What AFD reads from it a user can compare, and
 * what AFD writes to it a user can see.  The page is spare past the
 * structure: the tests write to offsets from 0xE00, which must hold zeroes,
 * and read zeroes from SPARE_ZERO, which nothing writes.
 */
#ifdef _WIN64
#define KERNEL_SHARED_PAGE ((PUCHAR)(ULONG_PTR)0xFFFFF78000000000ULL)
#else
#define KERNEL_SHARED_PAGE ((PUCHAR)(ULONG_PTR)0xFFDF0000UL)
#endif
#define USER_SHARED_PAGE ((const UCHAR *)(ULONG_PTR)0x7FFE0000UL)

/* NtSystemRoot: what is there does not change */
#define SHARED_SYSTEM_ROOT  0x30
#define SHARED_DATA_LENGTH  64

#define SPARE_ENUM          0xE00
#define SPARE_TDI_HANDLES   0xE40
#define SPARE_CONTEXT_SIZE  0xE60
#define SPARE_CONTEXT       0xE68
#define SPARE_PEER_NAME     0xE80
#define SPARE_RECEIVE       0xEA0
#define SPARE_FROM          0xEC0
#define SPARE_FROM_LENGTH   0xEE0
#define SPARE_ZERO          0xF00

static
BOOLEAN
SpareIsZero(
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++)
    {
        if (USER_SHARED_PAGE[Offset + i])
            return FALSE;
    }

    return TRUE;
}

/* Whether the spare bytes still hold zeroes; the test is skipped if they did
 * not to begin with */
#define START_SPARE(Offset, Length) \
    if (!SpareIsZero(Offset, Length)) \
    { \
        skip("Shared data at %x is not zero\n", Offset); \
        return; \
    }
#define ok_spare(Offset, Length, What) \
    ok(SpareIsZero(Offset, Length), "%s wrote to kernel memory at %p\n", What, KERNEL_SHARED_PAGE + (Offset))

/* A datagram send of Count buffers from Buffers to Remote */
static
NTSTATUS
SendDatagram(
    _In_ SOCKET Socket,
    _In_ PAFD_WSABUF Buffers,
    _In_ ULONG Count,
    _In_ PTRANSPORT_ADDRESS Remote,
    _In_ ULONG RemoteLength)
{
    AFD_SEND_INFO_UDP SendInfo;

    RtlZeroMemory(&SendInfo, sizeof(SendInfo));
    SendInfo.BufferArray = Buffers;
    SendInfo.BufferCount = Count;
    SendInfo.TdiConnection.RemoteAddress = Remote;
    SendInfo.TdiConnection.RemoteAddressLength = RemoteLength;

    return AfdRequest((HANDLE)Socket, IOCTL_AFD_SEND_DATAGRAM, &SendInfo, sizeof(SendInfo), NULL, 0, NULL);
}

/* A datagram receive into Buffer, and of its sender's address into From */
static
NTSTATUS
ReceiveDatagram(
    _In_ SOCKET Socket,
    _In_ PVOID Buffer,
    _In_ ULONG Length,
    _In_opt_ PVOID From,
    _In_opt_ PINT FromLength)
{
    AFD_RECV_INFO_UDP RecvInfo;
    AFD_WSABUF AfdBuffer;

    AfdBuffer.buf = Buffer;
    AfdBuffer.len = Length;
    RtlZeroMemory(&RecvInfo, sizeof(RecvInfo));
    RecvInfo.BufferArray = &AfdBuffer;
    RecvInfo.BufferCount = 1;
    RecvInfo.TdiFlags = TDI_RECEIVE_NORMAL;
    RecvInfo.Address = From;
    RecvInfo.AddressLength = FromLength;

    return AfdRequest((HANDLE)Socket, IOCTL_AFD_RECV_DATAGRAM, &RecvInfo, sizeof(RecvInfo), NULL, 0, NULL);
}

typedef struct _IP_TRANSPORT_ADDRESS
{
    TRANSPORT_ADDRESS Address;
    UCHAR More[TDI_ADDRESS_LENGTH_IP];
} IP_TRANSPORT_ADDRESS;

static
ULONG
ToTransportAddress(
    _In_ const struct sockaddr_in *Address,
    _Out_ IP_TRANSPORT_ADDRESS *TransportAddress)
{
    RtlZeroMemory(TransportAddress, sizeof(*TransportAddress));
    TransportAddress->Address.TAAddressCount = 1;
    TransportAddress->Address.Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    TransportAddress->Address.Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    /* A TDI_ADDRESS_IP is a sockaddr_in past its family */
    RtlCopyMemory(TransportAddress->Address.Address[0].Address, &Address->sin_port, TDI_ADDRESS_LENGTH_IP);

    return FIELD_OFFSET(TRANSPORT_ADDRESS, Address[0].Address) + TDI_ADDRESS_LENGTH_IP;
}

/* Two datagram sockets on 127.0.0.1 */
static
BOOLEAN
DatagramPair(
    _Out_ SOCKET *Sender,
    _Out_ SOCKET *Receiver,
    _Out_ IP_TRANSPORT_ADDRESS *ReceiverAddress,
    _Out_ PULONG ReceiverAddressLength)
{
    struct sockaddr_in Address;

    *Receiver = INVALID_SOCKET;
    *Sender = LoopbackSocket(SOCK_DGRAM, &Address);
    if (*Sender == INVALID_SOCKET)
        return FALSE;

    *Receiver = LoopbackSocket(SOCK_DGRAM, &Address);
    if (*Receiver == INVALID_SOCKET)
    {
        closesocket(*Sender);
        return FALSE;
    }

    *ReceiverAddressLength = ToTransportAddress(&Address, ReceiverAddress);
    return TRUE;
}

static
BOOLEAN
WaitReadable(
    _In_ SOCKET Socket)
{
    fd_set Readable;
    struct timeval Timeout = { 2, 0 };

    FD_ZERO(&Readable);
    FD_SET(Socket, &Readable);
    return select(0, &Readable, NULL, NULL, &Timeout) == 1;
}

/* Requests whose own structure is in kernel memory */
static
void
TestKernelRequest(void)
{
    NTSTATUS Status;
    SOCKET Socket;
    struct sockaddr_in Address;

    START_SPARE(SPARE_ZERO, sizeof(AFD_INFO));
    START_SPARE(SPARE_ENUM, sizeof(AFD_ENUM_NETWORK_EVENTS_INFO));

    Socket = LoopbackSocket(SOCK_DGRAM, &Address);
    if (Socket == INVALID_SOCKET)
        return;

    /* Read, then written back with the answer */
    Status = AfdRequest((HANDLE)Socket, IOCTL_AFD_GET_INFO, KERNEL_SHARED_PAGE + SPARE_ZERO,
                        sizeof(AFD_INFO), NULL, 0, NULL);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_GET_INFO from kernel memory returned %lx\n", Status);
    ok_spare(SPARE_ZERO, sizeof(AFD_INFO), "IOCTL_AFD_GET_INFO");

    /* A datagram socket can send: the answer is not zero */
    Status = AfdRequest((HANDLE)Socket, IOCTL_AFD_ENUM_NETWORK_EVENTS, KERNEL_SHARED_PAGE + SPARE_ENUM,
                        sizeof(AFD_ENUM_NETWORK_EVENTS_INFO), NULL, 0, NULL);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_ENUM_NETWORK_EVENTS from kernel memory returned %lx\n", Status);
    ok_spare(SPARE_ENUM, sizeof(AFD_ENUM_NETWORK_EVENTS_INFO), "IOCTL_AFD_ENUM_NETWORK_EVENTS");

    closesocket(Socket);
}

/* Sends whose data, buffer array or remote address is in kernel memory */
static
void
TestKernelSend(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    IP_TRANSPORT_ADDRESS Remote;
    ULONG RemoteLength;
    AFD_WSABUF Buffer;
    UCHAR Data[SHARED_DATA_LENGTH];
    int Received;

    START_SPARE(SPARE_ZERO, SHARED_DATA_LENGTH);

    if (!DatagramPair(&Sender, &Receiver, &Remote, &RemoteLength))
        return;

    /* From user memory, it arrives */
    memset(Data, 0x55, sizeof(Data));
    Buffer.buf = (PCHAR)Data;
    Buffer.len = sizeof(Data);
    Status = SendDatagram(Sender, &Buffer, 1, &Remote.Address, RemoteLength);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND_DATAGRAM returned %lx\n", Status);
    memset(Data, 0, sizeof(Data));
    Received = recv(Receiver, (char *)Data, sizeof(Data), 0);
    ok(Received == sizeof(Data) && Data[0] == 0x55, "recv returned %d, %x\n", Received, Data[0]);

    /* From kernel memory, it must not */
    Buffer.buf = (PCHAR)KERNEL_SHARED_PAGE + SHARED_SYSTEM_ROOT;
    Buffer.len = SHARED_DATA_LENGTH;
    Status = SendDatagram(Sender, &Buffer, 1, &Remote.Address, RemoteLength);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_SEND_DATAGRAM of kernel memory returned %lx\n", Status);
    memset(Data, 0, sizeof(Data));
    Received = recv(Receiver, (char *)Data, sizeof(Data), 0);
    ok(Received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT,
       "recv returned %d (%d)\n", Received, WSAGetLastError());
    ok(Received != sizeof(Data) || memcmp(Data, USER_SHARED_PAGE + SHARED_SYSTEM_ROOT, sizeof(Data)),
       "Kernel memory was sent\n");

    /* Nor its buffer array */
    Status = SendDatagram(Sender, (PAFD_WSABUF)(KERNEL_SHARED_PAGE + SPARE_ZERO), 1, &Remote.Address, RemoteLength);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_SEND_DATAGRAM with a kernel buffer array returned %lx\n", Status);

    /* Nor its remote address */
    Buffer.buf = (PCHAR)Data;
    Buffer.len = sizeof(Data);
    Status = SendDatagram(Sender, &Buffer, 1, (PTRANSPORT_ADDRESS)(KERNEL_SHARED_PAGE + SPARE_ZERO), RemoteLength);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_SEND_DATAGRAM to a kernel address returned %lx\n", Status);

    closesocket(Receiver);
    closesocket(Sender);

    /* Nor on a socket that is not bound yet, which the address binds */
    Sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Sender != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (Sender == INVALID_SOCKET)
        return;
    Status = SendDatagram(Sender, &Buffer, 1, (PTRANSPORT_ADDRESS)(KERNEL_SHARED_PAGE + SPARE_ZERO), RemoteLength);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_SEND_DATAGRAM to a kernel address returned %lx\n", Status);
    closesocket(Sender);

    ok_spare(SPARE_ZERO, SHARED_DATA_LENGTH, "IOCTL_AFD_SEND_DATAGRAM");
}

/* Receives into kernel memory */
static
void
TestKernelReceive(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    IP_TRANSPORT_ADDRESS Remote;
    ULONG RemoteLength;
    AFD_WSABUF Buffer;
    UCHAR Data[16], Received[16];
    UCHAR From[16];
    INT FromLength;
    int Length;

    START_SPARE(SPARE_RECEIVE, sizeof(Data));
    START_SPARE(SPARE_FROM, sizeof(From));
    START_SPARE(SPARE_FROM_LENGTH, sizeof(INT));

    if (!DatagramPair(&Sender, &Receiver, &Remote, &RemoteLength))
        return;

    memcpy(Data, "AFD user buffer", sizeof(Data));
    Buffer.buf = (PCHAR)Data;
    Buffer.len = sizeof(Data);
    Status = SendDatagram(Sender, &Buffer, 1, &Remote.Address, RemoteLength);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND_DATAGRAM returned %lx\n", Status);
    ok(WaitReadable(Receiver), "The datagram did not arrive\n");

    /* The data */
    Status = ReceiveDatagram(Receiver, KERNEL_SHARED_PAGE + SPARE_RECEIVE, sizeof(Data), NULL, NULL);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_RECV_DATAGRAM into kernel memory returned %lx\n", Status);
    ok_spare(SPARE_RECEIVE, sizeof(Data), "IOCTL_AFD_RECV_DATAGRAM");

    /* The sender's address */
    FromLength = sizeof(From);
    Status = ReceiveDatagram(Receiver, Received, sizeof(Received), KERNEL_SHARED_PAGE + SPARE_FROM, &FromLength);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_RECV_DATAGRAM of the address into kernel memory returned %lx\n", Status);
    ok_spare(SPARE_FROM, sizeof(From), "IOCTL_AFD_RECV_DATAGRAM");

    /* The address length */
    Status = ReceiveDatagram(Receiver, Received, sizeof(Received), From, (PINT)(KERNEL_SHARED_PAGE + SPARE_FROM_LENGTH));
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_RECV_DATAGRAM with a kernel address length returned %lx\n", Status);
    ok_spare(SPARE_FROM_LENGTH, sizeof(INT), "IOCTL_AFD_RECV_DATAGRAM");

    /* The datagram is still there */
    memset(Received, 0, sizeof(Received));
    Length = recv(Receiver, (char *)Received, sizeof(Received), 0);
    ok(Length == sizeof(Data) && !memcmp(Received, Data, sizeof(Data)), "recv returned %d\n", Length);

    closesocket(Receiver);
    closesocket(Sender);
}

/* Requests whose answer goes to kernel memory */
static
void
TestKernelOutput(void)
{
    NTSTATUS Status;
    SOCKET Socket, Peer;
    struct sockaddr_in Address, PeerAddress;
    ULONG Flags = AFD_ADDRESS_HANDLE | AFD_CONNECTION_HANDLE;
    UCHAR Context[4] = { 'A', 'F', 'D', '!' };

    START_SPARE(SPARE_TDI_HANDLES, sizeof(AFD_TDI_HANDLE_DATA));
    START_SPARE(SPARE_CONTEXT_SIZE, sizeof(ULONG));
    START_SPARE(SPARE_CONTEXT, sizeof(Context));
    START_SPARE(SPARE_PEER_NAME, 32);

    Socket = LoopbackSocket(SOCK_DGRAM, &Address);
    if (Socket == INVALID_SOCKET)
        return;
    Peer = LoopbackSocket(SOCK_DGRAM, &PeerAddress);
    if (Peer == INVALID_SOCKET)
    {
        closesocket(Socket);
        return;
    }

    /* A bound socket has an address handle */
    Status = AfdRequest((HANDLE)Socket, IOCTL_AFD_GET_TDI_HANDLES, &Flags, sizeof(Flags),
                        KERNEL_SHARED_PAGE + SPARE_TDI_HANDLES, sizeof(AFD_TDI_HANDLE_DATA), NULL);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_GET_TDI_HANDLES into kernel memory returned %lx\n", Status);
    ok_spare(SPARE_TDI_HANDLES, sizeof(AFD_TDI_HANDLE_DATA), "IOCTL_AFD_GET_TDI_HANDLES");

    Status = AfdRequest((HANDLE)Socket, IOCTL_AFD_SET_CONTEXT, Context, sizeof(Context), NULL, 0, NULL);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SET_CONTEXT returned %lx\n", Status);

    Status = AfdRequest((HANDLE)Socket, IOCTL_AFD_GET_CONTEXT_SIZE, NULL, 0,
                        KERNEL_SHARED_PAGE + SPARE_CONTEXT_SIZE, sizeof(ULONG), NULL);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_GET_CONTEXT_SIZE into kernel memory returned %lx\n", Status);
    ok_spare(SPARE_CONTEXT_SIZE, sizeof(ULONG), "IOCTL_AFD_GET_CONTEXT_SIZE");

    Status = AfdRequest((HANDLE)Socket, IOCTL_AFD_GET_CONTEXT, NULL, 0,
                        KERNEL_SHARED_PAGE + SPARE_CONTEXT, sizeof(Context), NULL);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_GET_CONTEXT into kernel memory returned %lx\n", Status);
    ok_spare(SPARE_CONTEXT, sizeof(Context), "IOCTL_AFD_GET_CONTEXT");

    /* A connected datagram socket has a peer */
    ok(connect(Socket, (struct sockaddr *)&PeerAddress, sizeof(PeerAddress)) == 0,
       "connect failed with %d\n", WSAGetLastError());
    Status = AfdRequest((HANDLE)Socket, IOCTL_AFD_GET_PEER_NAME, NULL, 0,
                        KERNEL_SHARED_PAGE + SPARE_PEER_NAME, 32, NULL);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_GET_PEER_NAME into kernel memory returned %lx\n", Status);
    ok_spare(SPARE_PEER_NAME, 32, "IOCTL_AFD_GET_PEER_NAME");

    closesocket(Peer);
    closesocket(Socket);
}

/* A ConnectEx to a listener, whose first data is Data.  Returns the listener,
 * to accept what arrived. */
static
NTSTATUS
ConnectEx(
    _In_ PVOID Data,
    _In_ ULONG Length,
    _Out_ SOCKET *Client,
    _Out_ SOCKET *Listener)
{
    NTSTATUS Status;
    struct sockaddr_in Address, ClientAddress;
    struct
    {
        BOOLEAN SanActive;
        IP_TRANSPORT_ADDRESS RemoteAddress;
    } Connect;

    *Client = INVALID_SOCKET;
    *Listener = LoopbackSocket(SOCK_STREAM, &Address);
    if (*Listener == INVALID_SOCKET)
        return STATUS_UNSUCCESSFUL;
    ok(listen(*Listener, 5) == 0, "listen failed with %d\n", WSAGetLastError());

    /* ConnectEx takes a bound socket */
    *Client = LoopbackSocket(SOCK_STREAM, &ClientAddress);
    if (*Client == INVALID_SOCKET)
        return STATUS_UNSUCCESSFUL;

    C_ASSERT(FIELD_OFFSET(AFD_SUPER_CONNECT_INFO, RemoteAddress) == FIELD_OFFSET(TRANSPORT_ADDRESS, Address));
    RtlZeroMemory(&Connect, sizeof(Connect));
    ToTransportAddress(&Address, &Connect.RemoteAddress);

    Status = AfdRequest((HANDLE)*Client, IOCTL_AFD_SUPER_CONNECT, &Connect,
                        FIELD_OFFSET(AFD_SUPER_CONNECT_INFO, RemoteAddress.Address[0].Address) + TDI_ADDRESS_LENGTH_IP,
                        Data, Length, NULL);
    return Status;
}

/* What the listener's connection, if one arrives, received first */
static
int
AcceptAndReceive(
    _In_ SOCKET Listener,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ int Length)
{
    SOCKET Accepted;
    int Received;

    if (!WaitReadable(Listener))
        return -1;

    Accepted = accept(Listener, NULL, NULL);
    ok(Accepted != INVALID_SOCKET, "accept failed with %d\n", WSAGetLastError());
    if (Accepted == INVALID_SOCKET)
        return -1;

    Received = WaitReadable(Accepted) ? recv(Accepted, Buffer, Length, 0) : -1;
    closesocket(Accepted);
    return Received;
}

/* A ConnectEx whose first data is in kernel memory */
static
void
TestKernelConnectEx(void)
{
    NTSTATUS Status;
    SOCKET Client, Listener;
    UCHAR Data[SHARED_DATA_LENGTH];
    int Received;

    /* From user memory, it arrives */
    memset(Data, 0x55, sizeof(Data));
    Status = ConnectEx(Data, sizeof(Data), &Client, &Listener);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SUPER_CONNECT returned %lx\n", Status);
    memset(Data, 0, sizeof(Data));
    Received = AcceptAndReceive(Listener, Data, sizeof(Data));
    ok(Received == sizeof(Data) && Data[0] == 0x55, "Received %d bytes, %x\n", Received, Data[0]);
    closesocket(Client);
    closesocket(Listener);

    /* From kernel memory, it must not; nor connect */
    Status = ConnectEx(KERNEL_SHARED_PAGE + SHARED_SYSTEM_ROOT, SHARED_DATA_LENGTH, &Client, &Listener);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_SUPER_CONNECT of kernel memory returned %lx\n", Status);
    memset(Data, 0, sizeof(Data));
    Received = AcceptAndReceive(Listener, Data, sizeof(Data));
    ok(Received == -1, "Received %d bytes\n", Received);
    ok(Received != sizeof(Data) || memcmp(Data, USER_SHARED_PAGE + SHARED_SYSTEM_ROOT, sizeof(Data)),
       "Kernel memory was sent\n");
    closesocket(Client);
    closesocket(Listener);
}

/* A ConnectEx whose first data is not there.  Last: an AFD that does not
 * guard the copy bugchecks. */
static
void
TestMissingConnectExData(void)
{
    NTSTATUS Status;
    SOCKET Client, Listener;
    PVOID Reserved;

    Reserved = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ok(Reserved != NULL, "VirtualAlloc failed with %lu\n", GetLastError());
    if (!Reserved)
        return;

    Status = ConnectEx(Reserved, SHARED_DATA_LENGTH, &Client, &Listener);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_SUPER_CONNECT of reserved memory returned %lx\n", Status);
    closesocket(Client);
    closesocket(Listener);

    VirtualFree(Reserved, 0, MEM_RELEASE);
}

/* A send of more buffers than there is memory for, which no array of the
 * caller's holds.  Last: an AFD that sizes its copy of the array in a UINT
 * allocates nothing for it. */
static
void
TestHugeBufferCount(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    IP_TRANSPORT_ADDRESS Remote;
    ULONG RemoteLength;
    AFD_WSABUF Buffer;
    UCHAR Data[16];

    if (!DatagramPair(&Sender, &Receiver, &Remote, &RemoteLength))
        return;

    Buffer.buf = (PCHAR)Data;
    Buffer.len = sizeof(Data);
    Status = SendDatagram(Sender, &Buffer, 0x80000000UL, &Remote.Address, RemoteLength);
    ok(Status == STATUS_ACCESS_VIOLATION, "IOCTL_AFD_SEND_DATAGRAM of 0x80000000 buffers returned %lx\n", Status);

    closesocket(Receiver);
    closesocket(Sender);
}

START_TEST(userbuffer)
{
    WSADATA WsaData;

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");

    TestKernelRequest();
    TestKernelSend();
    TestKernelReceive();
    TestKernelOutput();
    TestKernelConnectEx();

    /* An AFD that does not check one of these bugchecks */
    TestMissingConnectExData();
    TestHugeBufferCount();

    WSACleanup();
}
