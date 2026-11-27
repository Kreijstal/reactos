/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test which of their buffers sends and receives use
 */

#include "precomp.h"

#define DATAGRAM "datagram"
#define DATAGRAM_LENGTH (sizeof(DATAGRAM) - 1)

/* A receive of Count buffers, and of the sender's address into From */
static
NTSTATUS
ReceiveDatagram(
    _In_ SOCKET Socket,
    _In_opt_ PAFD_WSABUF Buffers,
    _In_ ULONG Count,
    _In_opt_ PVOID From,
    _In_opt_ PINT FromLength,
    _Out_ PULONG_PTR Information)
{
    AFD_RECV_INFO_UDP RecvInfo;

    RtlZeroMemory(&RecvInfo, sizeof(RecvInfo));
    RecvInfo.BufferArray = Buffers;
    RecvInfo.BufferCount = Count;
    RecvInfo.AfdFlags = AFD_IMMEDIATE;
    RecvInfo.TdiFlags = TDI_RECEIVE_NORMAL;
    RecvInfo.Address = From;
    RecvInfo.AddressLength = FromLength;

    return AfdRequest((HANDLE)Socket, IOCTL_AFD_RECV_DATAGRAM, &RecvInfo, sizeof(RecvInfo), NULL, 0, Information);
}

/* A datagram send of Count buffers to To */
static
NTSTATUS
SendDatagram(
    _In_ SOCKET Socket,
    _In_opt_ PAFD_WSABUF Buffers,
    _In_ ULONG Count,
    _In_ const struct sockaddr_in *To,
    _Out_ PULONG_PTR Information)
{
    AFD_SEND_INFO_UDP SendInfo;
    struct
    {
        TRANSPORT_ADDRESS Address;
        UCHAR More[TDI_ADDRESS_LENGTH_IP];
    } Remote;

    RtlZeroMemory(&Remote, sizeof(Remote));
    Remote.Address.TAAddressCount = 1;
    Remote.Address.Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    Remote.Address.Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    RtlCopyMemory(Remote.Address.Address[0].Address, &To->sin_port, TDI_ADDRESS_LENGTH_IP);

    RtlZeroMemory(&SendInfo, sizeof(SendInfo));
    SendInfo.BufferArray = Buffers;
    SendInfo.BufferCount = Count;
    SendInfo.TdiConnection.RemoteAddress = &Remote.Address;
    SendInfo.TdiConnection.RemoteAddressLength = sizeof(Remote);

    return AfdRequest((HANDLE)Socket, IOCTL_AFD_SEND_DATAGRAM, &SendInfo, sizeof(SendInfo), NULL, 0, Information);
}

/* A send or receive (IOCTL_AFD_SEND or IOCTL_AFD_RECV) of Count buffers */
static
NTSTATUS
Transfer(
    _In_ SOCKET Socket,
    _In_ ULONG IoControlCode,
    _In_opt_ PAFD_WSABUF Buffers,
    _In_ ULONG Count,
    _Out_ PULONG_PTR Information)
{
    AFD_SEND_INFO Info;

    C_ASSERT(sizeof(AFD_SEND_INFO) == sizeof(AFD_RECV_INFO));

    RtlZeroMemory(&Info, sizeof(Info));
    Info.BufferArray = Buffers;
    Info.BufferCount = Count;
    Info.AfdFlags = AFD_IMMEDIATE;
    Info.TdiFlags = TDI_RECEIVE_NORMAL;

    return AfdRequest((HANDLE)Socket, IoControlCode, &Info, sizeof(Info), NULL, 0, Information);
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

/* Two datagram sockets on 127.0.0.1, the receiver with DATAGRAM waiting */
static
BOOLEAN
DatagramWaiting(
    _Out_ SOCKET *Sender,
    _Out_ struct sockaddr_in *SenderAddress,
    _Out_ SOCKET *Receiver)
{
    struct sockaddr_in ReceiverAddress;

    *Receiver = INVALID_SOCKET;
    *Sender = LoopbackSocket(SOCK_DGRAM, SenderAddress);
    if (*Sender == INVALID_SOCKET)
        return FALSE;

    *Receiver = LoopbackSocket(SOCK_DGRAM, &ReceiverAddress);
    if (*Receiver == INVALID_SOCKET)
    {
        closesocket(*Sender);
        return FALSE;
    }

    ok(sendto(*Sender, DATAGRAM, DATAGRAM_LENGTH, 0, (struct sockaddr *)&ReceiverAddress, sizeof(ReceiverAddress)) == DATAGRAM_LENGTH,
       "sendto failed with %d\n", WSAGetLastError());
    if (!WaitReadable(*Receiver))
    {
        ok(0, "The datagram did not arrive\n");
        closesocket(*Receiver);
        closesocket(*Sender);
        return FALSE;
    }

    return TRUE;
}

#define ok_from(From, FromLength, Sender) \
    do { \
        ok((FromLength) == sizeof(struct sockaddr_in), "Address length %d\n", (FromLength)); \
        ok((From).sin_family == AF_INET, "Address family %u\n", (From).sin_family); \
        ok((From).sin_port == (Sender).sin_port, "Port %u, expected %u\n", \
           ntohs((From).sin_port), ntohs((Sender).sin_port)); \
        ok((From).sin_addr.s_addr == (Sender).sin_addr.s_addr, "Address %lx, expected %lx\n", \
           (From).sin_addr.s_addr, (Sender).sin_addr.s_addr); \
    } while (0)

/* A datagram receive of two buffers.  The datagram goes to the first, the
 * sender's address to the address buffer, not to the second buffer. */
static
void
TestTwoBufferReceive(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    struct sockaddr_in SenderAddress, From;
    INT FromLength;
    AFD_WSABUF Buffers[2];
    CHAR First[DATAGRAM_LENGTH], Second[DATAGRAM_LENGTH], Untouched[DATAGRAM_LENGTH];
    ULONG_PTR Information;

    if (!DatagramWaiting(&Sender, &SenderAddress, &Receiver))
        return;

    memset(First, 0xCC, sizeof(First));
    memset(Second, 0xCC, sizeof(Second));
    memset(Untouched, 0xCC, sizeof(Untouched));
    memset(&From, 0xCC, sizeof(From));
    FromLength = sizeof(From);
    Buffers[0].buf = First;
    Buffers[0].len = sizeof(First);
    Buffers[1].buf = Second;
    Buffers[1].len = sizeof(Second);
    Status = ReceiveDatagram(Receiver, Buffers, 2, &From, &FromLength, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_RECV_DATAGRAM of two buffers returned %lx\n", Status);
    ok(Information == DATAGRAM_LENGTH, "Received %Iu bytes\n", Information);
    ok(!memcmp(First, DATAGRAM, DATAGRAM_LENGTH), "The first buffer holds %.8s\n", First);
    ok(!memcmp(Second, Untouched, sizeof(Second)), "The second buffer was written: %02x %02x %02x %02x\n",
       (UCHAR)Second[0], (UCHAR)Second[1], (UCHAR)Second[2], (UCHAR)Second[3]);
    ok_from(From, FromLength, SenderAddress);

    closesocket(Receiver);
    closesocket(Sender);
}

/* A datagram receive of no buffers: the datagram does not fit, its sender's
 * address is still told */
static
void
TestNoBufferReceive(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    struct sockaddr_in SenderAddress, From;
    INT FromLength;
    ULONG_PTR Information;
    CHAR Buffer[DATAGRAM_LENGTH];

    if (!DatagramWaiting(&Sender, &SenderAddress, &Receiver))
        return;

    memset(&From, 0xCC, sizeof(From));
    FromLength = sizeof(From);
    Status = ReceiveDatagram(Receiver, NULL, 0, &From, &FromLength, &Information);
    ok(Status == STATUS_BUFFER_OVERFLOW, "IOCTL_AFD_RECV_DATAGRAM of no buffers returned %lx\n", Status);
    ok(Information == 0, "Received %Iu bytes\n", Information);
    ok(memcmp(&From, DATAGRAM, DATAGRAM_LENGTH), "The datagram went to the address\n");
    ok_from(From, FromLength, SenderAddress);

    /* It was taken */
    ok(recv(Receiver, Buffer, sizeof(Buffer), 0) == SOCKET_ERROR, "A datagram was left\n");

    closesocket(Receiver);
    closesocket(Sender);
}

/* A datagram send of no buffers, to an address and on a connected socket:
 * the empty datagram */
static
void
TestNoBufferSend(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    struct sockaddr_in SenderAddress, ReceiverAddress;
    AFD_SEND_INFO_UDP SendInfo;
    struct
    {
        TRANSPORT_ADDRESS Address;
        UCHAR More[TDI_ADDRESS_LENGTH_IP];
    } Remote;
    AFD_WSABUF Buffer;
    ULONG_PTR Information;

    Sender = LoopbackSocket(SOCK_DGRAM, &SenderAddress);
    if (Sender == INVALID_SOCKET)
        return;
    Receiver = LoopbackSocket(SOCK_DGRAM, &ReceiverAddress);
    if (Receiver == INVALID_SOCKET)
    {
        closesocket(Sender);
        return;
    }

    RtlZeroMemory(&Remote, sizeof(Remote));
    Remote.Address.TAAddressCount = 1;
    Remote.Address.Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    Remote.Address.Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    RtlCopyMemory(Remote.Address.Address[0].Address, &ReceiverAddress.sin_port, TDI_ADDRESS_LENGTH_IP);

    RtlZeroMemory(&SendInfo, sizeof(SendInfo));
    SendInfo.BufferArray = NULL;
    SendInfo.BufferCount = 0;
    SendInfo.TdiConnection.RemoteAddress = &Remote.Address;
    SendInfo.TdiConnection.RemoteAddressLength = sizeof(Remote);
    Status = AfdRequest((HANDLE)Sender, IOCTL_AFD_SEND_DATAGRAM, &SendInfo, sizeof(SendInfo), NULL, 0, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND_DATAGRAM of no buffers returned %lx\n", Status);
    ok(Information == 0, "Sent %Iu bytes\n", Information);

    /* An array that is there, of no buffers */
    SendInfo.BufferArray = &Buffer;
    Status = AfdRequest((HANDLE)Sender, IOCTL_AFD_SEND_DATAGRAM, &SendInfo, sizeof(SendInfo), NULL, 0, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND_DATAGRAM of no buffers returned %lx\n", Status);
    ok(Information == 0, "Sent %Iu bytes\n", Information);

    /* Connected */
    ok(connect(Sender, (struct sockaddr *)&ReceiverAddress, sizeof(ReceiverAddress)) == 0,
       "connect failed with %d\n", WSAGetLastError());
    Status = Transfer(Sender, IOCTL_AFD_SEND, NULL, 0, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND of no buffers returned %lx\n", Status);
    ok(Information == 0, "Sent %Iu bytes\n", Information);

    closesocket(Receiver);
    closesocket(Sender);
}

/* A stream send and receive of no buffers: they transfer nothing, and the
 * data waiting stays */
static
void
TestNoBufferStream(void)
{
    NTSTATUS Status;
    SOCKET Listener, Client, Server;
    struct sockaddr_in Address;
    ULONG_PTR Information;
    CHAR Buffer[DATAGRAM_LENGTH];

    Listener = LoopbackSocket(SOCK_STREAM, &Address);
    if (Listener == INVALID_SOCKET)
        return;
    ok(listen(Listener, 1) == 0, "listen failed with %d\n", WSAGetLastError());
    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Client != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    ok(connect(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0,
       "connect failed with %d\n", WSAGetLastError());
    Server = accept(Listener, NULL, NULL);
    ok(Server != INVALID_SOCKET, "accept failed with %d\n", WSAGetLastError());
    if (Server == INVALID_SOCKET)
    {
        closesocket(Client);
        closesocket(Listener);
        return;
    }

    Status = Transfer(Client, IOCTL_AFD_SEND, NULL, 0, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND of no buffers returned %lx\n", Status);
    ok(Information == 0, "Sent %Iu bytes\n", Information);

    ok(send(Client, DATAGRAM, DATAGRAM_LENGTH, 0) == DATAGRAM_LENGTH, "send failed with %d\n", WSAGetLastError());
    ok(WaitReadable(Server), "The data did not arrive\n");

    Status = Transfer(Server, IOCTL_AFD_RECV, NULL, 0, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_RECV of no buffers returned %lx\n", Status);
    ok(Information == 0, "Received %Iu bytes\n", Information);

    memset(Buffer, 0, sizeof(Buffer));
    ok(recv(Server, Buffer, sizeof(Buffer), 0) == DATAGRAM_LENGTH, "recv failed with %d\n", WSAGetLastError());
    ok(!memcmp(Buffer, DATAGRAM, DATAGRAM_LENGTH), "Received %.8s\n", Buffer);

    closesocket(Server);
    closesocket(Client);
    closesocket(Listener);
}

/* A receive of less than its buffers hold writes what it received, and leaves
 * the rest of the buffers alone */
static
void
TestPartialReceive(void)
{
    NTSTATUS Status;
    SOCKET Listener, Client, Server, Sender, Receiver;
    struct sockaddr_in Address;
    AFD_WSABUF Buffers[2];
    CHAR First[2 * DATAGRAM_LENGTH], Second[DATAGRAM_LENGTH], Untouched[2 * DATAGRAM_LENGTH];
    ULONG_PTR Information;

    memset(Untouched, 0xCC, sizeof(Untouched));

    /* A stream */
    Listener = LoopbackSocket(SOCK_STREAM, &Address);
    if (Listener == INVALID_SOCKET)
        return;
    ok(listen(Listener, 1) == 0, "listen failed with %d\n", WSAGetLastError());
    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Client != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    ok(connect(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0,
       "connect failed with %d\n", WSAGetLastError());
    Server = accept(Listener, NULL, NULL);
    ok(Server != INVALID_SOCKET, "accept failed with %d\n", WSAGetLastError());
    if (Server != INVALID_SOCKET)
    {
        ok(send(Client, DATAGRAM, DATAGRAM_LENGTH, 0) == DATAGRAM_LENGTH, "send failed with %d\n", WSAGetLastError());
        ok(WaitReadable(Server), "The data did not arrive\n");

        memset(First, 0xCC, sizeof(First));
        memset(Second, 0xCC, sizeof(Second));
        Buffers[0].buf = First;
        Buffers[0].len = sizeof(First);
        Buffers[1].buf = Second;
        Buffers[1].len = sizeof(Second);
        Status = Transfer(Server, IOCTL_AFD_RECV, Buffers, 2, &Information);
        ok(Status == STATUS_SUCCESS, "IOCTL_AFD_RECV returned %lx\n", Status);
        ok(Information == DATAGRAM_LENGTH, "Received %Iu bytes\n", Information);
        ok(!memcmp(First, DATAGRAM, DATAGRAM_LENGTH), "Received %.8s\n", First);
        ok(!memcmp(First + DATAGRAM_LENGTH, Untouched, DATAGRAM_LENGTH),
           "The first buffer was written past the data: %02x\n", (UCHAR)First[DATAGRAM_LENGTH]);
        ok(!memcmp(Second, Untouched, sizeof(Second)),
           "The second buffer was written: %02x\n", (UCHAR)Second[0]);

        closesocket(Server);
    }
    closesocket(Client);
    closesocket(Listener);

    /* A datagram */
    if (!DatagramWaiting(&Sender, &Address, &Receiver))
        return;

    memset(First, 0xCC, sizeof(First));
    Buffers[0].buf = First;
    Buffers[0].len = sizeof(First);
    Status = ReceiveDatagram(Receiver, Buffers, 1, NULL, NULL, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_RECV_DATAGRAM returned %lx\n", Status);
    ok(Information == DATAGRAM_LENGTH, "Received %Iu bytes\n", Information);
    ok(!memcmp(First, DATAGRAM, DATAGRAM_LENGTH), "Received %.8s\n", First);
    ok(!memcmp(First + DATAGRAM_LENGTH, Untouched, DATAGRAM_LENGTH),
       "The buffer was written past the datagram: %02x\n", (UCHAR)First[DATAGRAM_LENGTH]);

    closesocket(Receiver);
    closesocket(Sender);
}

/* Receives the datagram waiting on Receiver whole, and checks it is Expected
 * from Sender */
static
void
ExpectDatagram(
    _In_ SOCKET Receiver,
    _In_reads_bytes_(ExpectedLength) const CHAR *Expected,
    _In_ INT ExpectedLength,
    _In_ const struct sockaddr_in *Sender)
{
    CHAR Buffer[2 * DATAGRAM_LENGTH];
    struct sockaddr_in From;
    INT FromLength, Received;

    ok(WaitReadable(Receiver), "The datagram did not arrive\n");
    memset(Buffer, 0xCC, sizeof(Buffer));
    memset(&From, 0xCC, sizeof(From));
    FromLength = sizeof(From);
    Received = recvfrom(Receiver, Buffer, sizeof(Buffer), 0, (struct sockaddr *)&From, &FromLength);
    ok(Received == ExpectedLength, "recvfrom returned %d (%d), expected %d\n",
       Received, WSAGetLastError(), ExpectedLength);
    if (Received == ExpectedLength)
    {
        ok(!memcmp(Buffer, Expected, ExpectedLength), "Received %.*s\n", Received, Buffer);
        ok_from(From, FromLength, *Sender);
    }
}

/* A datagram send of several buffers sends one datagram of all of them, in
 * their order, whether it is to an address or on a connected socket */
static
void
TestGatherSend(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    struct sockaddr_in SenderAddress, ReceiverAddress;
    AFD_WSABUF Buffers[4];
    WSABUF WsaBuffers[3];
    CHAR First[] = "da", Third[] = "tag", Fourth[] = "ram";
    ULONG_PTR Information;
    DWORD Sent;

    Sender = LoopbackSocket(SOCK_DGRAM, &SenderAddress);
    if (Sender == INVALID_SOCKET)
        return;
    Receiver = LoopbackSocket(SOCK_DGRAM, &ReceiverAddress);
    if (Receiver == INVALID_SOCKET)
    {
        closesocket(Sender);
        return;
    }

    /* With an empty one among them */
    Buffers[0].buf = First;
    Buffers[0].len = 2;
    Buffers[1].buf = Third;
    Buffers[1].len = 0;
    Buffers[2].buf = Third;
    Buffers[2].len = 3;
    Buffers[3].buf = Fourth;
    Buffers[3].len = 3;
    Status = SendDatagram(Sender, Buffers, 4, &ReceiverAddress, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND_DATAGRAM of four buffers returned %lx\n", Status);
    ok(Information == DATAGRAM_LENGTH, "Sent %Iu bytes\n", Information);
    ExpectDatagram(Receiver, DATAGRAM, DATAGRAM_LENGTH, &SenderAddress);

    WsaBuffers[0].buf = First;
    WsaBuffers[0].len = 2;
    WsaBuffers[1].buf = Third;
    WsaBuffers[1].len = 3;
    WsaBuffers[2].buf = Fourth;
    WsaBuffers[2].len = 3;
    Sent = 0;
    ok(WSASendTo(Sender, WsaBuffers, 3, &Sent, 0, (struct sockaddr *)&ReceiverAddress, sizeof(ReceiverAddress),
                 NULL, NULL) == 0,
       "WSASendTo failed with %d\n", WSAGetLastError());
    ok(Sent == DATAGRAM_LENGTH, "WSASendTo sent %lu bytes\n", Sent);
    ExpectDatagram(Receiver, DATAGRAM, DATAGRAM_LENGTH, &SenderAddress);

    /* Connected */
    ok(connect(Sender, (struct sockaddr *)&ReceiverAddress, sizeof(ReceiverAddress)) == 0,
       "connect failed with %d\n", WSAGetLastError());
    Status = Transfer(Sender, IOCTL_AFD_SEND, Buffers, 4, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND of four buffers returned %lx\n", Status);
    ok(Information == DATAGRAM_LENGTH, "Sent %Iu bytes\n", Information);
    ExpectDatagram(Receiver, DATAGRAM, DATAGRAM_LENGTH, &SenderAddress);

    Sent = 0;
    ok(WSASend(Sender, WsaBuffers, 3, &Sent, 0, NULL, NULL) == 0, "WSASend failed with %d\n", WSAGetLastError());
    ok(Sent == DATAGRAM_LENGTH, "WSASend sent %lu bytes\n", Sent);
    ExpectDatagram(Receiver, DATAGRAM, DATAGRAM_LENGTH, &SenderAddress);

    closesocket(Receiver);
    closesocket(Sender);
}

/* A datagram receive of several buffers fills them in their order, and the
 * datagram is truncated only when it does not fit all of them */
static
void
TestScatterReceive(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    struct sockaddr_in SenderAddress, From;
    INT FromLength;
    AFD_WSABUF Buffers[4];
    WSABUF WsaBuffers[2];
    CHAR First[3], Second[3], Third[DATAGRAM_LENGTH], Untouched[DATAGRAM_LENGTH];
    ULONG_PTR Information;
    DWORD Received, Flags;

    memset(Untouched, 0xCC, sizeof(Untouched));

    /* The datagram fits: the first two get three bytes each, the empty one
     * none, the last the two left */
    if (!DatagramWaiting(&Sender, &SenderAddress, &Receiver))
        return;
    memset(First, 0xCC, sizeof(First));
    memset(Second, 0xCC, sizeof(Second));
    memset(Third, 0xCC, sizeof(Third));
    memset(&From, 0xCC, sizeof(From));
    FromLength = sizeof(From);
    Buffers[0].buf = First;
    Buffers[0].len = sizeof(First);
    Buffers[1].buf = Second;
    Buffers[1].len = sizeof(Second);
    Buffers[2].buf = Untouched;
    Buffers[2].len = 0;
    Buffers[3].buf = Third;
    Buffers[3].len = sizeof(Third);
    Status = ReceiveDatagram(Receiver, Buffers, 4, &From, &FromLength, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_RECV_DATAGRAM of four buffers returned %lx\n", Status);
    ok(Information == DATAGRAM_LENGTH, "Received %Iu bytes\n", Information);
    ok(!memcmp(First, "dat", 3), "The first buffer holds %.3s\n", First);
    ok(!memcmp(Second, "agr", 3), "The second buffer holds %.3s\n", Second);
    ok(!memcmp(Third, "am", 2), "The last buffer holds %.2s\n", Third);
    ok(!memcmp(Third + 2, Untouched, sizeof(Third) - 2),
       "The last buffer was written past the datagram: %02x\n", (UCHAR)Third[2]);
    ok_from(From, FromLength, SenderAddress);
    closesocket(Receiver);
    closesocket(Sender);

    /* It does not: what fits is received, and the rest is lost */
    if (!DatagramWaiting(&Sender, &SenderAddress, &Receiver))
        return;
    memset(First, 0xCC, sizeof(First));
    memset(Second, 0xCC, sizeof(Second));
    memset(&From, 0xCC, sizeof(From));
    FromLength = sizeof(From);
    Status = ReceiveDatagram(Receiver, Buffers, 2, &From, &FromLength, &Information);
    ok(Status == STATUS_BUFFER_OVERFLOW, "IOCTL_AFD_RECV_DATAGRAM of six bytes returned %lx\n", Status);
    ok(Information == 6, "Received %Iu bytes\n", Information);
    ok(!memcmp(First, "dat", 3), "The first buffer holds %.3s\n", First);
    ok(!memcmp(Second, "agr", 3), "The second buffer holds %.3s\n", Second);
    ok_from(From, FromLength, SenderAddress);
    ok(recv(Receiver, Third, sizeof(Third), 0) == SOCKET_ERROR, "A datagram was left\n");
    closesocket(Receiver);
    closesocket(Sender);

    /* Through Winsock */
    if (!DatagramWaiting(&Sender, &SenderAddress, &Receiver))
        return;
    memset(First, 0xCC, sizeof(First));
    memset(Third, 0xCC, sizeof(Third));
    memset(&From, 0xCC, sizeof(From));
    FromLength = sizeof(From);
    WsaBuffers[0].buf = First;
    WsaBuffers[0].len = sizeof(First);
    WsaBuffers[1].buf = Third;
    WsaBuffers[1].len = sizeof(Third);
    Received = 0;
    Flags = 0;
    ok(WSARecvFrom(Receiver, WsaBuffers, 2, &Received, &Flags, (struct sockaddr *)&From, &FromLength,
                   NULL, NULL) == 0,
       "WSARecvFrom failed with %d\n", WSAGetLastError());
    ok(Received == DATAGRAM_LENGTH, "WSARecvFrom received %lu bytes\n", Received);
    ok(!memcmp(First, "dat", 3), "The first buffer holds %.3s\n", First);
    ok(!memcmp(Third, "agram", 5), "The second buffer holds %.5s\n", Third);
    ok_from(From, FromLength, SenderAddress);
    closesocket(Receiver);
    closesocket(Sender);
}

/* A datagram of no bytes is sent, and received as one */
static
void
TestEmptyDatagram(void)
{
    NTSTATUS Status;
    SOCKET Sender, Receiver;
    struct sockaddr_in SenderAddress, ReceiverAddress;
    CHAR Data[] = DATAGRAM;
    AFD_WSABUF Buffer;
    ULONG_PTR Information;

    Sender = LoopbackSocket(SOCK_DGRAM, &SenderAddress);
    if (Sender == INVALID_SOCKET)
        return;
    Receiver = LoopbackSocket(SOCK_DGRAM, &ReceiverAddress);
    if (Receiver == INVALID_SOCKET)
    {
        closesocket(Sender);
        return;
    }

    ok(sendto(Sender, Data, 0, 0, (struct sockaddr *)&ReceiverAddress, sizeof(ReceiverAddress)) == 0,
       "sendto failed with %d\n", WSAGetLastError());
    ExpectDatagram(Receiver, Data, 0, &SenderAddress);

    Buffer.buf = Data;
    Buffer.len = 0;
    Status = SendDatagram(Sender, &Buffer, 1, &ReceiverAddress, &Information);
    ok(Status == STATUS_SUCCESS, "IOCTL_AFD_SEND_DATAGRAM of no bytes returned %lx\n", Status);
    ok(Information == 0, "Sent %Iu bytes\n", Information);
    ExpectDatagram(Receiver, Data, 0, &SenderAddress);

    /* Connected */
    ok(connect(Sender, (struct sockaddr *)&ReceiverAddress, sizeof(ReceiverAddress)) == 0,
       "connect failed with %d\n", WSAGetLastError());
    ok(send(Sender, Data, 0, 0) == 0, "send failed with %d\n", WSAGetLastError());
    ExpectDatagram(Receiver, Data, 0, &SenderAddress);

    /* The datagram after it is still its own */
    ok(send(Sender, Data, DATAGRAM_LENGTH, 0) == DATAGRAM_LENGTH, "send failed with %d\n", WSAGetLastError());
    ExpectDatagram(Receiver, Data, DATAGRAM_LENGTH, &SenderAddress);

    closesocket(Receiver);
    closesocket(Sender);
}

START_TEST(buffercount)
{
    WSADATA WsaData;

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");

    TestPartialReceive();

    TestTwoBufferReceive();

    /* An AFD that allocates nothing for no buffers, or reads past them,
     * bugchecks */
    TestNoBufferReceive();
    TestNoBufferSend();
    TestNoBufferStream();

    TestGatherSend();
    TestScatterReceive();
    TestEmptyDatagram();

    WSACleanup();
}
