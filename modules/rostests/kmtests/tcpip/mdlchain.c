/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that TDI sends and receives use all of an MDL chain
 */

#include <kmt_test.h>
#include <tdikrnl.h>
#include <ndk/rtlfuncs.h>

#include "tcpip.h"

#define TAG_TEST 'cMeT'

/* Each piece of a chain is at its own place in a region of REGION_SIZE
 * bytes, with room between the pieces: a transport that takes the first
 * MDL for all of the chain reads, or writes, the room after it */
#define REGION_SIZE 64
#define PIECES 3
static const ULONG PieceOffset[PIECES] = { 0, 16, 32 };

#define DATA_LENGTH 12
static const UCHAR SendPieces[] = TEST_MDL_CHAIN_TCP_DATA;
static const ULONG SendPieceLength[PIECES] = { 5, 4, 3 };
static const ULONG ReceivePieceLength[PIECES] = { 3, 4, 5 };

#define FILLER 0xCC
#define WAIT_SECONDS 10

static
USHORT
TestHtons(
    _In_ USHORT Value)
{
    return (USHORT)(((Value & 0x00FF) << 8) | ((Value & 0xFF00) >> 8));
}

static
NTSTATUS
NTAPI
IrpCompletionRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NETWORK_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
VOID
FillAddress(
    _Out_ PTA_IP_ADDRESS Address,
    _In_ USHORT Port)
{
    NTSTATUS Status;
    LPCWSTR Terminator;
    IN_ADDR InAddr;

    RtlZeroMemory(Address, sizeof(*Address));
    Address->TAAddressCount = 1;
    Address->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    Address->Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    Address->Address[0].Address[0].sin_port = TestHtons(Port);
    Status = RtlIpv4StringToAddressW(L"127.0.0.1", TRUE, &Terminator, &InAddr);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Address->Address[0].Address[0].in_addr = InAddr.S_un.S_addr;
}

/* Opens DeviceName with the extended attribute Name of Value */
static
NTSTATUS
OpenTdiObject(
    _In_ PCWSTR DeviceName,
    _In_ PCSTR Name,
    _In_ USHORT NameLength,
    _In_reads_bytes_(ValueLength) const VOID *Value,
    _In_ USHORT ValueLength,
    _Out_ PHANDLE Handle,
    _Out_ PFILE_OBJECT *FileObject)
{
    NTSTATUS Status;
    UNICODE_STRING Device;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK StatusBlock;
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG EaSize;

    *Handle = NULL;
    *FileObject = NULL;

    EaSize = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + NameLength + 1 + ValueLength;
    Ea = ExAllocatePoolWithTag(NonPagedPool, EaSize, TAG_TEST);
    if (!Ea)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Ea, EaSize);
    Ea->EaNameLength = (UCHAR)NameLength;
    Ea->EaValueLength = ValueLength;
    RtlCopyMemory(Ea->EaName, Name, NameLength);
    RtlCopyMemory(&Ea->EaName[NameLength + 1], Value, ValueLength);

    RtlInitUnicodeString(&Device, DeviceName);
    InitializeObjectAttributes(&ObjectAttributes, &Device, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile(Handle,
                          GENERIC_READ | GENERIC_WRITE,
                          &ObjectAttributes,
                          &StatusBlock,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN_IF,
                          0,
                          Ea,
                          EaSize);
    ExFreePoolWithTag(Ea, TAG_TEST);
    if (!NT_SUCCESS(Status))
    {
        *Handle = NULL;
        return Status;
    }

    Status = ObReferenceObjectByHandle(*Handle, GENERIC_READ, *IoFileObjectType, KernelMode, (PVOID *)FileObject, NULL);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(*Handle);
        *Handle = NULL;
        *FileObject = NULL;
    }
    return Status;
}

static
NTSTATUS
OpenAddress(
    _In_ PCWSTR DeviceName,
    _In_ USHORT Port,
    _Out_ PHANDLE Handle,
    _Out_ PFILE_OBJECT *FileObject)
{
    TA_IP_ADDRESS Address;

    FillAddress(&Address, Port);
    return OpenTdiObject(DeviceName, TdiTransportAddress, TDI_TRANSPORT_ADDRESS_LENGTH,
                         &Address, sizeof(Address), Handle, FileObject);
}

static
VOID
CloseTdiObject(
    _In_opt_ HANDLE Handle,
    _In_opt_ PFILE_OBJECT FileObject)
{
    if (FileObject)
        ObDereferenceObject(FileObject);
    if (Handle)
        ZwClose(Handle);
}

/* Waits at most WAIT_SECONDS for Irp, sent down already, before cancelling
 * it.  The IRP stays the caller's. */
static
NTSTATUS
WaitForIrp(
    _In_ PIRP Irp,
    _In_ PKEVENT Event)
{
    NTSTATUS Status;
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10000000LL * WAIT_SECONDS;
    Status = KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout);
    if (Status == STATUS_TIMEOUT)
    {
        ok(0, "The request did not complete in %d seconds\n", WAIT_SECONDS);
        IoCancelIrp(Irp);
        KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
    }
    return Irp->IoStatus.Status;
}

/* Sends Irp down, and waits for it as WaitForIrp() does */
static
NTSTATUS
CallAndWait(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PKEVENT Event)
{
    IoCallDriver(DeviceObject, Irp);
    return WaitForIrp(Irp, Event);
}

/* A chain of PIECES MDLs, piece i of PieceLength[i] bytes at
 * Region + PieceOffset[i] */
static
PMDL
BuildChain(
    _In_ PUCHAR Region,
    _In_reads_(PIECES) const ULONG *PieceLength)
{
    PMDL First = NULL, *Next = &First, Mdl;
    ULONG i;

    for (i = 0; i < PIECES; i++)
    {
        Mdl = IoAllocateMdl(Region + PieceOffset[i], PieceLength[i], FALSE, FALSE, NULL);
        if (!Mdl)
        {
            while (First)
            {
                Mdl = First->Next;
                IoFreeMdl(First);
                First = Mdl;
            }
            return NULL;
        }
        MmBuildMdlForNonPagedPool(Mdl);
        *Next = Mdl;
        Next = &Mdl->Next;
    }

    return First;
}

static
VOID
FreeChain(
    _In_opt_ PMDL Mdl)
{
    PMDL Next;

    while (Mdl)
    {
        Next = Mdl->Next;
        IoFreeMdl(Mdl);
        Mdl = Next;
    }
}

/* Fills Region with FILLER, and the pieces of a send with Data */
static
VOID
FillSendRegion(
    _Out_writes_bytes_(REGION_SIZE) PUCHAR Region,
    _In_reads_bytes_(DATA_LENGTH) const UCHAR *Data)
{
    ULONG i, Offset = 0;

    RtlFillMemory(Region, REGION_SIZE, FILLER);
    for (i = 0; i < PIECES; i++)
    {
        RtlCopyMemory(Region + PieceOffset[i], Data + Offset, SendPieceLength[i]);
        Offset += SendPieceLength[i];
    }
}

/* Checks that the first Length bytes of Expected went to the pieces of a
 * receive in Region in their order, and nothing else was written */
static
VOID
CheckReceiveRegion(
    _In_reads_bytes_(REGION_SIZE) const UCHAR *Region,
    _In_reads_bytes_(Length) const UCHAR *Expected,
    _In_ ULONG Length)
{
    ULONG i, Offset = 0, Piece = 0;

    for (i = 0; i < REGION_SIZE; i++)
    {
        while (Piece < PIECES && i >= PieceOffset[Piece] + ReceivePieceLength[Piece])
            Piece++;
        if (Piece < PIECES && i >= PieceOffset[Piece] && Offset < Length)
        {
            ok(Region[i] == Expected[Offset], "Byte %lu of the received data at %lu is %02x, expected %02x\n",
               Offset, i, Region[i], Expected[Offset]);
            Offset++;
        }
        else
        {
            ok(Region[i] == FILLER, "The receive wrote %02x at %lu, outside its data\n", Region[i], i);
        }
    }
    ok_eq_ulong(Offset, Length);
}

/* A datagram sent from a chain is all of the chain, and one received into a
 * chain fills all of it */
static
VOID
TestUdpChain(void)
{
    NTSTATUS Status;
    HANDLE SenderHandle, ReceiverHandle;
    PFILE_OBJECT SenderFile, ReceiverFile;
    PDEVICE_OBJECT DeviceObject;
    PUCHAR SendRegion = NULL, ReceiveRegion = NULL;
    PMDL SendChain = NULL, ReceiveChain = NULL;
    PIRP SendIrp = NULL, ReceiveIrp = NULL;
    KEVENT SendEvent, ReceiveEvent;
    TA_IP_ADDRESS ReceiverAddress, FromAddress;
    TDI_CONNECTION_INFORMATION SendInfo, ReceiveInfo, ReturnInfo;
    ULONG Round;

    Status = OpenAddress(L"\\Device\\Udp", TEST_MDL_CHAIN_UDP_SENDER_PORT, &SenderHandle, &SenderFile);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = OpenAddress(L"\\Device\\Udp", TEST_MDL_CHAIN_UDP_RECEIVER_PORT, &ReceiverHandle, &ReceiverFile);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        CloseTdiObject(SenderHandle, SenderFile);
        return;
    }
    DeviceObject = IoGetRelatedDeviceObject(ReceiverFile);

    SendRegion = ExAllocatePoolWithTag(NonPagedPool, REGION_SIZE, TAG_TEST);
    ReceiveRegion = ExAllocatePoolWithTag(NonPagedPool, REGION_SIZE, TAG_TEST);
    if (!SendRegion || !ReceiveRegion)
    {
        ok(0, "Out of memory\n");
        goto Cleanup;
    }

    /* Round 0: a chain to a chain.  Round 1: a datagram larger than the
     * chain it is received into, which takes what fits. */
    for (Round = 0; Round < 2; Round++)
    {
        ULONG ReceiveLength = (Round == 0) ? DATA_LENGTH : 3 + 4 + 2;
        ULONG Expected = min(ReceiveLength, DATA_LENGTH);

        FillSendRegion(SendRegion, SendPieces);
        RtlFillMemory(ReceiveRegion, REGION_SIZE, FILLER);
        SendChain = BuildChain(SendRegion, SendPieceLength);
        ReceiveChain = BuildChain(ReceiveRegion, ReceivePieceLength);
        if (!SendChain || !ReceiveChain)
        {
            ok(0, "Out of memory\n");
            goto Cleanup;
        }
        if (Round == 1)
        {
            /* The last piece of the receive is two bytes */
            ReceiveChain->Next->Next->ByteCount = 2;
        }

        /* The receive waits first */
        KeInitializeEvent(&ReceiveEvent, NotificationEvent, FALSE);
        ReceiveIrp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
        ok(ReceiveIrp != NULL, "IoAllocateIrp failed\n");
        if (!ReceiveIrp)
            goto Cleanup;
        RtlZeroMemory(&ReceiveInfo, sizeof(ReceiveInfo));
        RtlZeroMemory(&ReturnInfo, sizeof(ReturnInfo));
        RtlZeroMemory(&FromAddress, sizeof(FromAddress));
        ReturnInfo.RemoteAddressLength = sizeof(FromAddress);
        ReturnInfo.RemoteAddress = &FromAddress;
        TdiBuildReceiveDatagram(ReceiveIrp, DeviceObject, ReceiverFile, NULL, NULL,
                                ReceiveChain, ReceiveLength, &ReceiveInfo, &ReturnInfo, TDI_RECEIVE_NORMAL);
        IoSetCompletionRoutine(ReceiveIrp, IrpCompletionRoutine, &ReceiveEvent, TRUE, TRUE, TRUE);
        Status = IoCallDriver(DeviceObject, ReceiveIrp);
        ok(Status == STATUS_PENDING, "TDI_RECEIVE_DATAGRAM returned %lx\n", Status);

        /* Round 1 sends one buffer, the receive is the chain */
        if (Round == 1)
        {
            FreeChain(SendChain);
            RtlCopyMemory(SendRegion, SendPieces, DATA_LENGTH);
            SendChain = IoAllocateMdl(SendRegion, DATA_LENGTH, FALSE, FALSE, NULL);
            if (!SendChain)
            {
                ok(0, "Out of memory\n");
                IoCancelIrp(ReceiveIrp);
                KeWaitForSingleObject(&ReceiveEvent, Executive, KernelMode, FALSE, NULL);
                goto Cleanup;
            }
            MmBuildMdlForNonPagedPool(SendChain);
        }

        KeInitializeEvent(&SendEvent, NotificationEvent, FALSE);
        SendIrp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
        ok(SendIrp != NULL, "IoAllocateIrp failed\n");
        if (!SendIrp)
        {
            IoCancelIrp(ReceiveIrp);
            KeWaitForSingleObject(&ReceiveEvent, Executive, KernelMode, FALSE, NULL);
            goto Cleanup;
        }
        FillAddress(&ReceiverAddress, TEST_MDL_CHAIN_UDP_RECEIVER_PORT);
        RtlZeroMemory(&SendInfo, sizeof(SendInfo));
        SendInfo.RemoteAddressLength = sizeof(ReceiverAddress);
        SendInfo.RemoteAddress = &ReceiverAddress;
        TdiBuildSendDatagram(SendIrp, DeviceObject, SenderFile, NULL, NULL,
                             SendChain, DATA_LENGTH, &SendInfo);
        IoSetCompletionRoutine(SendIrp, IrpCompletionRoutine, &SendEvent, TRUE, TRUE, TRUE);
        Status = CallAndWait(DeviceObject, SendIrp, &SendEvent);
        ok(Status == STATUS_SUCCESS, "Round %lu: TDI_SEND_DATAGRAM returned %lx\n", Round, Status);
        ok(SendIrp->IoStatus.Information == DATA_LENGTH, "Round %lu: TDI_SEND_DATAGRAM sent %Iu bytes\n",
           Round, SendIrp->IoStatus.Information);

        Status = WaitForIrp(ReceiveIrp, &ReceiveEvent);
        ok(Status == ((Round == 0) ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW),
           "Round %lu: TDI_RECEIVE_DATAGRAM returned %lx\n", Round, Status);
        ok(ReceiveIrp->IoStatus.Information == Expected, "Round %lu: TDI_RECEIVE_DATAGRAM received %Iu bytes\n",
           Round, ReceiveIrp->IoStatus.Information);
        /* In round 1 the last piece holds two bytes: the bytes after them
         * are outside the data */
        CheckReceiveRegion(ReceiveRegion, SendPieces, Expected);
        ok(SendRegion[0] == 'A', "The send wrote its buffer\n");

        ReceiveIrp->MdlAddress = NULL;
        IoFreeIrp(ReceiveIrp);
        ReceiveIrp = NULL;
        SendIrp->MdlAddress = NULL;
        IoFreeIrp(SendIrp);
        SendIrp = NULL;
        FreeChain(SendChain);
        SendChain = NULL;
        FreeChain(ReceiveChain);
        ReceiveChain = NULL;
    }

Cleanup:
    if (ReceiveIrp)
    {
        ReceiveIrp->MdlAddress = NULL;
        IoFreeIrp(ReceiveIrp);
    }
    if (SendIrp)
    {
        SendIrp->MdlAddress = NULL;
        IoFreeIrp(SendIrp);
    }
    FreeChain(SendChain);
    FreeChain(ReceiveChain);
    if (SendRegion)
        ExFreePoolWithTag(SendRegion, TAG_TEST);
    if (ReceiveRegion)
        ExFreePoolWithTag(ReceiveRegion, TAG_TEST);
    CloseTdiObject(ReceiverHandle, ReceiverFile);
    CloseTdiObject(SenderHandle, SenderFile);
}

/* A stream send from a chain sends all of it, and a stream receive into a
 * chain fills all of it.  The peer, in user mode, accepts the connection on
 * TEST_MDL_CHAIN_TCP_SERVER_PORT, expects the sent data and answers with
 * TEST_MDL_CHAIN_TCP_ANSWER. */
static
VOID
TestTcpChain(void)
{
    NTSTATUS Status;
    HANDLE AddressHandle, ConnectionHandle;
    PFILE_OBJECT AddressFile, ConnectionFile;
    PDEVICE_OBJECT DeviceObject;
    CONNECTION_CONTEXT Context = (CONNECTION_CONTEXT)(ULONG_PTR)0x4D444C43;
    PUCHAR SendRegion = NULL, ReceiveRegion = NULL;
    PMDL SendChain = NULL, ReceiveChain = NULL;
    PIRP Irp;
    KEVENT Event;
    TA_IP_ADDRESS ServerAddress, ReturnAddress;
    TDI_CONNECTION_INFORMATION RequestInfo, ReturnInfo;

    Status = OpenAddress(L"\\Device\\Tcp", TEST_MDL_CHAIN_TCP_CLIENT_PORT, &AddressHandle, &AddressFile);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = OpenTdiObject(L"\\Device\\Tcp", TdiConnectionContext, TDI_CONNECTION_CONTEXT_LENGTH,
                           &Context, sizeof(Context), &ConnectionHandle, &ConnectionFile);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        CloseTdiObject(AddressHandle, AddressFile);
        return;
    }
    DeviceObject = IoGetRelatedDeviceObject(ConnectionFile);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    SendRegion = ExAllocatePoolWithTag(NonPagedPool, REGION_SIZE, TAG_TEST);
    ReceiveRegion = ExAllocatePoolWithTag(NonPagedPool, REGION_SIZE, TAG_TEST);
    if (!SendRegion || !ReceiveRegion)
    {
        ok(0, "Out of memory\n");
        goto Cleanup;
    }
    FillSendRegion(SendRegion, SendPieces);
    RtlFillMemory(ReceiveRegion, REGION_SIZE, FILLER);
    SendChain = BuildChain(SendRegion, SendPieceLength);
    ReceiveChain = BuildChain(ReceiveRegion, ReceivePieceLength);
    if (!SendChain || !ReceiveChain)
    {
        ok(0, "Out of memory\n");
        goto Cleanup;
    }

    /* Associate */
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    ok(Irp != NULL, "IoAllocateIrp failed\n");
    if (!Irp)
        goto Cleanup;
    TdiBuildAssociateAddress(Irp, DeviceObject, ConnectionFile, NULL, NULL, AddressHandle);
    IoSetCompletionRoutine(Irp, IrpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
    Status = CallAndWait(DeviceObject, Irp, &Event);
    ok_eq_hex(Status, STATUS_SUCCESS);
    IoFreeIrp(Irp);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Connect */
    KeClearEvent(&Event);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    ok(Irp != NULL, "IoAllocateIrp failed\n");
    if (!Irp)
        goto Cleanup;
    FillAddress(&ServerAddress, TEST_MDL_CHAIN_TCP_SERVER_PORT);
    RtlZeroMemory(&RequestInfo, sizeof(RequestInfo));
    RequestInfo.RemoteAddressLength = sizeof(ServerAddress);
    RequestInfo.RemoteAddress = &ServerAddress;
    RtlZeroMemory(&ReturnInfo, sizeof(ReturnInfo));
    RtlZeroMemory(&ReturnAddress, sizeof(ReturnAddress));
    ReturnInfo.RemoteAddressLength = sizeof(ReturnAddress);
    ReturnInfo.RemoteAddress = &ReturnAddress;
    TdiBuildConnect(Irp, DeviceObject, ConnectionFile, NULL, NULL, NULL, &RequestInfo, &ReturnInfo);
    IoSetCompletionRoutine(Irp, IrpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
    Status = CallAndWait(DeviceObject, Irp, &Event);
    ok_eq_hex(Status, STATUS_SUCCESS);
    IoFreeIrp(Irp);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Send the chain */
    KeClearEvent(&Event);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    ok(Irp != NULL, "IoAllocateIrp failed\n");
    if (!Irp)
        goto Cleanup;
    TdiBuildSend(Irp, DeviceObject, ConnectionFile, NULL, NULL, SendChain, 0, DATA_LENGTH);
    IoSetCompletionRoutine(Irp, IrpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
    Status = CallAndWait(DeviceObject, Irp, &Event);
    ok(Status == STATUS_SUCCESS, "TDI_SEND returned %lx\n", Status);
    ok(Irp->IoStatus.Information == DATA_LENGTH, "TDI_SEND sent %Iu bytes\n", Irp->IoStatus.Information);
    ok(SendRegion[0] == 'A', "The send wrote its buffer\n");
    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);

    /* Receive the answer into the chain */
    KeClearEvent(&Event);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    ok(Irp != NULL, "IoAllocateIrp failed\n");
    if (!Irp)
        goto Cleanup;
    TdiBuildReceive(Irp, DeviceObject, ConnectionFile, NULL, NULL, ReceiveChain,
                    TDI_RECEIVE_NORMAL, DATA_LENGTH);
    IoSetCompletionRoutine(Irp, IrpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
    Status = CallAndWait(DeviceObject, Irp, &Event);
    ok(Status == STATUS_SUCCESS, "TDI_RECEIVE returned %lx\n", Status);
    ok(Irp->IoStatus.Information == DATA_LENGTH, "TDI_RECEIVE received %Iu bytes\n", Irp->IoStatus.Information);
    CheckReceiveRegion(ReceiveRegion, (const UCHAR *)TEST_MDL_CHAIN_TCP_ANSWER,
                       (ULONG)min(Irp->IoStatus.Information, DATA_LENGTH));
    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);

Cleanup:
    FreeChain(SendChain);
    FreeChain(ReceiveChain);
    if (SendRegion)
        ExFreePoolWithTag(SendRegion, TAG_TEST);
    if (ReceiveRegion)
        ExFreePoolWithTag(ReceiveRegion, TAG_TEST);
    CloseTdiObject(ConnectionHandle, ConnectionFile);
    CloseTdiObject(AddressHandle, AddressFile);
}

static KSTART_ROUTINE RunTest;
static
VOID
NTAPI
RunTest(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    TestUdpChain();
    TestTcpChain();
}

KMT_MESSAGE_HANDLER TestMdlChain;
NTSTATUS
TestMdlChain(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    PKTHREAD Thread;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(ControlCode);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    Thread = KmtStartThread(RunTest, NULL);
    KmtFinishThread(Thread, NULL);

    return STATUS_SUCCESS;
}
