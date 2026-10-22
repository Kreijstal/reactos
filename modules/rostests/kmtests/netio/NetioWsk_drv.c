/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-mode test for the WSK provider in netio.sys
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>
#include <ws2def.h>
#include <wsk.h>

#include "NetioWsk.h"

#define TAG_TEST 'kWtN'
#define LOOPBACK 0x0100007F /* 127.0.0.1 in network order */

/* Calling a WSK routine synchronously, the way a client waits for its IRP */
typedef struct _SYNC_CALL
{
    PIRP Irp;
    KEVENT Event;
} SYNC_CALL, *PSYNC_CALL;

static
NTSTATUS
NTAPI
SyncCallComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
PIRP
SyncCallBegin(
    _Out_ PSYNC_CALL Call)
{
    KeInitializeEvent(&Call->Event, NotificationEvent, FALSE);
    Call->Irp = IoAllocateIrp(1, FALSE);
    if (Call->Irp)
        IoSetCompletionRoutine(Call->Irp, SyncCallComplete, &Call->Event, TRUE, TRUE, TRUE);
    return Call->Irp;
}

/* Returns the IRP's final status; the provider completes the IRP whatever
 * the routine returned */
static
NTSTATUS
SyncCallEnd(
    _Inout_ PSYNC_CALL Call,
    _In_ NTSTATUS Returned,
    _Out_opt_ PULONG_PTR Information)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Timeout.QuadPart = -10LL * 1000 * 1000 * 10;
    Status = KeWaitForSingleObject(&Call->Event, Executive, KernelMode, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "IRP not completed (wait 0x%lx, returned 0x%lx)\n", Status, Returned);
    if (Status != STATUS_SUCCESS)
    {
        /* Leak the IRP rather than free one the provider may still own */
        return STATUS_IO_TIMEOUT;
    }

    Status = Call->Irp->IoStatus.Status;
    if (Information)
        *Information = Call->Irp->IoStatus.Information;
    IoFreeIrp(Call->Irp);
    return Status;
}

static
NTSTATUS
WSKAPI
ReceiveFromEvent(
    _In_opt_ PVOID SocketContext,
    _In_ ULONG Flags,
    _In_opt_ PWSK_DATAGRAM_INDICATION DataIndication)
{
    return STATUS_SUCCESS;
}

static const WSK_CLIENT_DISPATCH ClientDispatch = { MAKE_WSK_VERSION(1, 0), 0, NULL };
static const WSK_CLIENT_DATAGRAM_DISPATCH DatagramDispatch = { ReceiveFromEvent };

typedef struct _TEST_BUFFER
{
    PVOID Data;
    PMDL Mdl;
} TEST_BUFFER, *PTEST_BUFFER;

static
BOOLEAN
AllocateBuffer(
    _Out_ PTEST_BUFFER Buffer,
    _In_ ULONG Length,
    _In_ UCHAR Fill)
{
    Buffer->Mdl = NULL;
    Buffer->Data = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_TEST);
    if (!Buffer->Data)
        return FALSE;
    RtlFillMemory(Buffer->Data, Length, Fill);
    Buffer->Mdl = IoAllocateMdl(Buffer->Data, Length, FALSE, FALSE, NULL);
    if (!Buffer->Mdl)
    {
        ExFreePoolWithTag(Buffer->Data, TAG_TEST);
        Buffer->Data = NULL;
        return FALSE;
    }
    MmBuildMdlForNonPagedPool(Buffer->Mdl);
    return TRUE;
}

static
VOID
FreeBuffer(
    _Inout_ PTEST_BUFFER Buffer)
{
    if (Buffer->Mdl)
        IoFreeMdl(Buffer->Mdl);
    if (Buffer->Data)
        ExFreePoolWithTag(Buffer->Data, TAG_TEST);
}

static
PWSK_SOCKET
CreateSocket(
    _In_ PWSK_PROVIDER_NPI Npi,
    _In_ USHORT Type,
    _In_ ULONG Protocol,
    _In_ ULONG Flags,
    _In_opt_ const VOID *Dispatch)
{
    SYNC_CALL Call;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    if (!SyncCallBegin(&Call))
        return NULL;
    Status = Npi->Dispatch->WskSocket(Npi->Client, AF_INET, Type, Protocol, Flags, NULL,
                                      Dispatch, NULL, NULL, NULL, Call.Irp);
    Status = SyncCallEnd(&Call, Status, &Information);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status) ? (PWSK_SOCKET)Information : NULL;
}

static
VOID
CloseSocket(
    _In_ PWSK_SOCKET Socket)
{
    PWSK_PROVIDER_BASIC_DISPATCH Basic = (PWSK_PROVIDER_BASIC_DISPATCH)Socket->Dispatch;
    SYNC_CALL Call;
    NTSTATUS Status;

    if (!SyncCallBegin(&Call))
        return;
    Status = Basic->WskCloseSocket(Socket, Call.Irp);
    Status = SyncCallEnd(&Call, Status, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static
NTSTATUS
BindAndGetLocal(
    _In_ PWSK_SOCKET Socket,
    _In_ PFN_WSK_BIND Bind,
    _In_ PFN_WSK_GET_LOCAL_ADDRESS GetLocalAddress,
    _Out_ PSOCKADDR_IN Local)
{
    SOCKADDR_IN Address;
    SYNC_CALL Call;
    NTSTATUS Status;

    RtlZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = LOOPBACK;
    Address.sin_port = 0; /* any */

    if (!SyncCallBegin(&Call))
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = Bind(Socket, (PSOCKADDR)&Address, 0, Call.Irp);
    Status = SyncCallEnd(&Call, Status, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlFillMemory(Local, sizeof(*Local), 0xCC);
    if (!SyncCallBegin(&Call))
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = GetLocalAddress(Socket, (PSOCKADDR)Local, Call.Irp);
    Status = SyncCallEnd(&Call, Status, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return Status;

    ok_eq_uint(Local->sin_family, AF_INET);
    ok_eq_hex(Local->sin_addr.s_addr, (ULONG)LOOPBACK);
    ok(Local->sin_port != 0, "The transport did not pick a port\n");
    return STATUS_SUCCESS;
}

static
VOID
TestDatagram(
    _In_ PWSK_PROVIDER_NPI Npi)
{
    PWSK_PROVIDER_DATAGRAM_DISPATCH Dispatch;
    WSK_EVENT_CALLBACK_CONTROL EventControl;
    TEST_BUFFER Buffers[3] = { { 0 } };
    static const ULONG Lengths[3] = { 10, 20, 30 };
    WSK_BUF_LIST List[3];
    SOCKADDR_IN Local;
    PWSK_SOCKET Socket;
    WSK_BUF WskBuf;
    SYNC_CALL Call;
    ULONG_PTR Information;
    NTSTATUS Status;
    ULONG i;

    Socket = CreateSocket(Npi, SOCK_DGRAM, IPPROTO_UDP, WSK_FLAG_DATAGRAM_SOCKET, &DatagramDispatch);
    if (skip(Socket != NULL, "No datagram socket\n"))
        return;
    Dispatch = (PWSK_PROVIDER_DATAGRAM_DISPATCH)Socket->Dispatch;

    /* Clients built for Windows 8 and later call this */
    ok(Dispatch->WskSendMessages != NULL, "No WskSendMessages in the datagram dispatch\n");

    Status = BindAndGetLocal(Socket, Dispatch->WskBind, Dispatch->WskGetLocalAddress, &Local);
    if (skip(NT_SUCCESS(Status), "Bind failed\n"))
        goto Close;

    for (i = 0; i < RTL_NUMBER_OF(Buffers); i++)
    {
        if (!AllocateBuffer(&Buffers[i], Lengths[i], (UCHAR)(0x41 + i)))
            goto Free;
        List[i].Next = (i + 1 < RTL_NUMBER_OF(List)) ? &List[i + 1] : NULL;
        List[i].Buffer.Mdl = Buffers[i].Mdl;
        List[i].Buffer.Offset = 0;
        List[i].Buffer.Length = Lengths[i];
    }

    /* Without a destination, sending is an error */
    WskBuf = List[0].Buffer;
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskSendTo(Socket, &WskBuf, 0, NULL, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    }

    /* Event callbacks are enabled without an IRP */
    RtlZeroMemory(&EventControl, sizeof(EventControl));
    EventControl.NpiId = &NPI_WSK_INTERFACE_ID;
    EventControl.EventMask = WSK_EVENT_RECEIVE_FROM;
    Status = Dispatch->WskControlSocket(Socket, WskSetOption, SO_WSK_EVENT_CALLBACK, SOL_SOCKET,
                                        sizeof(EventControl), &EventControl, 0, NULL, NULL, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Fix the destination: ourselves */
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskControlSocket(Socket, WskIoctl, SIO_WSK_SET_REMOTE_ADDRESS, 0,
                                            sizeof(Local), &Local, 0, NULL, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    /* WskSendTo without an address now goes there */
    if (SyncCallBegin(&Call))
    {
        Information = 0;
        Status = Dispatch->WskSendTo(Socket, &WskBuf, 0, NULL, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, Lengths[0]);
    }

    /* WskSendMessages sends every buffer, with an explicit address... */
    if (SyncCallBegin(&Call))
    {
        Information = 0;
        Status = Dispatch->WskSendMessages(Socket, List, 0, (PSOCKADDR)&Local, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, Lengths[0] + Lengths[1] + Lengths[2]);
    }

    /* ...or with the fixed one */
    if (SyncCallBegin(&Call))
    {
        Information = 0;
        Status = Dispatch->WskSendMessages(Socket, &List[1], 0, NULL, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, &Information);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Information, Lengths[1] + Lengths[2]);
    }

    /* Removing the fixed address makes address-less sends fail again */
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskControlSocket(Socket, WskIoctl, SIO_WSK_SET_REMOTE_ADDRESS, 0,
                                            0, NULL, 0, NULL, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (SyncCallBegin(&Call))
    {
        Status = Dispatch->WskSendMessages(Socket, List, 0, NULL, 0, NULL, Call.Irp);
        Status = SyncCallEnd(&Call, Status, NULL);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    }

Free:
    for (i = 0; i < RTL_NUMBER_OF(Buffers); i++)
        FreeBuffer(&Buffers[i]);
Close:
    CloseSocket(Socket);
}

static
NTSTATUS
TestMessageHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    WSK_CLIENT_NPI ClientNpi;
    WSK_REGISTRATION Registration;
    WSK_PROVIDER_NPI Npi;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    if (ControlCode != IOCTL_NETIO_WSK_RUN_TESTS)
        return STATUS_NOT_SUPPORTED;

    ok_irql(PASSIVE_LEVEL);

    ClientNpi.ClientContext = NULL;
    ClientNpi.Dispatch = &ClientDispatch;
    Status = WskRegister(&ClientNpi, &Registration);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    Status = WskCaptureProviderNPI(&Registration, WSK_INFINITE_WAIT, &Npi);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        /* Stream sockets are not tested: closing a TCP socket that never
         * connected trips an assertion in tcpip (TCPFinEventHandler) */
        TestDatagram(&Npi);
        WskReleaseProviderNPI(&Registration);
    }

    WskDeregister(&Registration);
    return STATUS_SUCCESS;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"NetioWsk";
    return KmtRegisterMessageHandler(IOCTL_NETIO_WSK_RUN_TESTS, NULL, TestMessageHandler);
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
}
