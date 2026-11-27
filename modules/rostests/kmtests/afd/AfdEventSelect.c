/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test AFD's event select for a kernel-mode caller
 */

#include <kmt_test.h>
#include <tdikrnl.h>
#define _WINBASE_
#define _WINDOWS_H
#define _INC_WINDOWS
#include <windef.h>
#include <winsock2.h>
#include <drivers/afd/shared.h>

#define UDP_DEVICE_NAME L"\\Device\\Udp"

/* An AFD datagram endpoint, as a kernel handle */
static
NTSTATUS
CreateEndpoint(
    _Out_ PHANDLE Endpoint)
{
    NTSTATUS Status;
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\Afd\\Endpoint");
    UNICODE_STRING TransportName = RTL_CONSTANT_STRING(UDP_DEVICE_NAME);
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    PFILE_FULL_EA_INFORMATION EaBuffer;
    PAFD_CREATE_PACKET Packet;
    ULONG PacketLength, EaLength;

    PacketLength = FIELD_OFFSET(AFD_CREATE_PACKET, TransportName) + TransportName.Length + sizeof(UNICODE_NULL);
    EaLength = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + sizeof(AfdCommand) + PacketLength;
    EaBuffer = ExAllocatePoolZero(PagedPool, EaLength, 'tDfA');
    if (!EaBuffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    EaBuffer->EaNameLength = sizeof(AfdCommand) - sizeof(ANSI_NULL);
    RtlCopyMemory(EaBuffer->EaName, AfdCommand, sizeof(AfdCommand));
    EaBuffer->EaValueLength = (USHORT)PacketLength;
    Packet = (PAFD_CREATE_PACKET)(EaBuffer->EaName + sizeof(AfdCommand));
    Packet->EndpointFlags = AFD_ENDPOINT_CONNECTIONLESS;
#if (_WIN32_WINNT >= 0x0600)
    Packet->AddressFamily = 2; /* AF_INET */
    Packet->SocketType = 2; /* SOCK_DGRAM */
    Packet->Protocol = 17; /* IPPROTO_UDP */
#endif
    Packet->SizeOfTransportName = TransportName.Length;
    RtlCopyMemory(Packet->TransportName, TransportName.Buffer, TransportName.Length);

    InitializeObjectAttributes(&ObjectAttributes,
                               &DeviceName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwCreateFile(Endpoint,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN_IF,
                          0,
                          EaBuffer,
                          EaLength);

    ExFreePoolWithTag(EaBuffer, 'tDfA');
    return Status;
}

/* A request to the endpoint, waited for */
static
NTSTATUS
Request(
    _In_ HANDLE Endpoint,
    _In_ ULONG IoControlCode,
    _In_ PVOID Input,
    _In_ ULONG InputLength)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Event;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, &ObjectAttributes, NotificationEvent, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ZwDeviceIoControlFile(Endpoint, Event, NULL, NULL, &IoStatus,
                                   IoControlCode, Input, InputLength, NULL, 0);
    if (Status == STATUS_PENDING)
    {
        Status = ZwWaitForSingleObject(Event, FALSE, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = IoStatus.Status;
    }

    ZwClose(Event);
    return Status;
}

START_TEST(AfdEventSelect)
{
    NTSTATUS Status;
    HANDLE Endpoint, Event;
    OBJECT_ATTRIBUTES ObjectAttributes;
    AFD_EVENT_SELECT_INFO EventSelect;
    AFD_ENUM_NETWORK_EVENTS_INFO EnumEvents;
    LARGE_INTEGER Timeout;

    Status = CreateEndpoint(&Endpoint);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No AFD endpoint\n"))
        return;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, &ObjectAttributes, NotificationEvent, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No event\n"))
    {
        ZwClose(Endpoint);
        return;
    }

    /* A kernel-mode caller's event is a kernel handle */
    EventSelect.EventObject = Event;
    EventSelect.Events = AFD_EVENT_SEND | AFD_EVENT_RECEIVE;
    Status = Request(Endpoint, IOCTL_AFD_EVENT_SELECT, &EventSelect, sizeof(EventSelect));
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* ... and so is the event to clear, which AFD clears */
    Status = ZwSetEvent(Event, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlZeroMemory(&EnumEvents, sizeof(EnumEvents));
    EnumEvents.Event = Event;
    Status = Request(Endpoint, IOCTL_AFD_ENUM_NETWORK_EVENTS, &EnumEvents, sizeof(EnumEvents));
    ok_eq_hex(Status, STATUS_SUCCESS);

    Timeout.QuadPart = 0;
    Status = ZwWaitForSingleObject(Event, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_TIMEOUT);

    /* Not selecting any more */
    EventSelect.EventObject = NULL;
    EventSelect.Events = 0;
    Status = Request(Endpoint, IOCTL_AFD_EVENT_SELECT, &EventSelect, sizeof(EventSelect));
    ok_eq_hex(Status, STATUS_SUCCESS);

    ZwClose(Event);
    ZwClose(Endpoint);
}
