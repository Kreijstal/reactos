/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test which object IOCTL_TCP_SET_INFORMATION_EX and
 *              IOCTL_TCP_QUERY_INFORMATION_EX act on
 */

#include <kmt_test.h>
#include <tdikrnl.h>
#include <tdiinfo.h>
#include <tcpioctl.h>
#include <ndk/rtlfuncs.h>

#include "tcpip.h"

#define TAG_TEST 'iSeT'
#define MAX_ENTITIES 512

/* The TTL each step sets, none of them the default */
#define TTL_BY_ENTITY  33
#define TTL_BY_HANDLE  77
#define TTL_BY_CONNECTION 66

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

/* Opens DeviceName, with the extended attribute Name of Value if there is
 * one, as a control channel if not */
static
NTSTATUS
OpenTdiObject(
    _In_ PCWSTR DeviceName,
    _In_opt_ PCSTR Name,
    _In_ USHORT NameLength,
    _In_reads_bytes_opt_(ValueLength) const VOID *Value,
    _In_ USHORT ValueLength,
    _Out_ PHANDLE Handle,
    _Out_ PFILE_OBJECT *FileObject)
{
    NTSTATUS Status;
    UNICODE_STRING Device;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK StatusBlock;
    PFILE_FULL_EA_INFORMATION Ea = NULL;
    ULONG EaSize = 0;

    *Handle = NULL;
    *FileObject = NULL;

    if (Name)
    {
        EaSize = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + NameLength + 1 + ValueLength;
        Ea = ExAllocatePoolWithTag(NonPagedPool, EaSize, TAG_TEST);
        if (!Ea)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(Ea, EaSize);
        Ea->EaNameLength = (UCHAR)NameLength;
        Ea->EaValueLength = ValueLength;
        RtlCopyMemory(Ea->EaName, Name, NameLength);
        RtlCopyMemory(&Ea->EaName[NameLength + 1], Value, ValueLength);
    }

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
    if (Ea)
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
    _In_ USHORT Port,
    _Out_ PHANDLE Handle,
    _Out_ PFILE_OBJECT *FileObject)
{
    NTSTATUS Status;
    TA_IP_ADDRESS Address;
    LPCWSTR Terminator;
    IN_ADDR InAddr;

    RtlZeroMemory(&Address, sizeof(Address));
    Address.TAAddressCount = 1;
    Address.Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    Address.Address[0].AddressLength = TDI_ADDRESS_LENGTH_IP;
    Address.Address[0].Address[0].sin_port = TestHtons(Port);
    Status = RtlIpv4StringToAddressW(L"127.0.0.1", TRUE, &Terminator, &InAddr);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Address.Address[0].Address[0].in_addr = InAddr.S_un.S_addr;

    return OpenTdiObject(L"\\Device\\Tcp", TdiTransportAddress, TDI_TRANSPORT_ADDRESS_LENGTH,
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

/* Sends IoControlCode on FileObject, and waits for it */
static
NTSTATUS
DeviceControl(
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_(InputLength) PVOID Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(*OutputLength) PVOID Output,
    _Inout_opt_ PULONG OutputLength)
{
    PDEVICE_OBJECT DeviceObject = IoGetRelatedDeviceObject(FileObject);
    IO_STATUS_BLOCK StatusBlock;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        DeviceObject,
                                        Input,
                                        InputLength,
                                        Output,
                                        OutputLength ? *OutputLength : 0,
                                        FALSE,
                                        &Event,
                                        &StatusBlock);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(Irp)->FileObject = FileObject;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = StatusBlock.Status;
    }
    if (OutputLength)
        *OutputLength = (ULONG)StatusBlock.Information;

    return Status;
}

static
NTSTATUS
SetInformationEx(
    _In_ PFILE_OBJECT FileObject,
    _In_ TDIEntityID Entity,
    _In_ ULONG Type,
    _In_ ULONG Id,
    _In_ ULONG Value,
    _In_ ULONG InputLength)
{
    UCHAR Buffer[FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG)];
    PTCP_REQUEST_SET_INFORMATION_EX Info = (PTCP_REQUEST_SET_INFORMATION_EX)Buffer;

    RtlZeroMemory(Buffer, sizeof(Buffer));
    Info->ID.toi_entity = Entity;
    Info->ID.toi_class = INFO_CLASS_PROTOCOL;
    Info->ID.toi_type = Type;
    Info->ID.toi_id = Id;
    Info->BufferSize = sizeof(ULONG);
    RtlCopyMemory(Info->Buffer, &Value, sizeof(Value));

    ASSERT(InputLength <= sizeof(Buffer));
    return DeviceControl(FileObject, IOCTL_TCP_SET_INFORMATION_EX, Buffer, InputLength, NULL, NULL);
}

static
NTSTATUS
QueryInformationEx(
    _In_ PFILE_OBJECT FileObject,
    _In_ TDIEntityID Entity,
    _In_ ULONG Class,
    _In_ ULONG Type,
    _In_ ULONG Id,
    _Out_writes_bytes_(*Length) PVOID Output,
    _Inout_ PULONG Length)
{
    TCP_REQUEST_QUERY_INFORMATION_EX Request;

    RtlZeroMemory(&Request, sizeof(Request));
    Request.ID.toi_entity = Entity;
    Request.ID.toi_class = Class;
    Request.ID.toi_type = Type;
    Request.ID.toi_id = Id;

    return DeviceControl(FileObject, IOCTL_TCP_QUERY_INFORMATION_EX, &Request, sizeof(Request), Output, Length);
}

/* The TTL of the address object Entity names, 0xFFFFFFFF if the query fails */
static
ULONG
QueryTtl(
    _In_ PFILE_OBJECT FileObject,
    _In_ TDIEntityID Entity)
{
    NTSTATUS Status;
    ULONG Ttl = 0xFFFFFFFF, Length = sizeof(Ttl);

    Status = QueryInformationEx(FileObject, Entity, INFO_CLASS_PROTOCOL, INFO_TYPE_ADDRESS_OBJECT,
                                AO_OPTION_TTL, &Ttl, &Length);
    ok(Status == STATUS_SUCCESS, "Querying the TTL of %lx/%lx returned %lx\n",
       Entity.tei_entity, Entity.tei_instance, Status);
    ok(Length == sizeof(ULONG), "The TTL is %lu bytes\n", Length);

    return NT_SUCCESS(Status) ? (Ttl & 0xFF) : 0xFFFFFFFF;
}

/* The entities there are now */
static
ULONG
QueryEntities(
    _In_ PFILE_OBJECT ControlChannel,
    _Out_writes_(MAX_ENTITIES) TDIEntityID *Entities)
{
    NTSTATUS Status;
    ULONG Length = MAX_ENTITIES * sizeof(TDIEntityID);
    TDIEntityID Generic = { GENERIC_ENTITY, 0 };

    Status = QueryInformationEx(ControlChannel, Generic, INFO_CLASS_GENERIC, INFO_TYPE_PROVIDER,
                                ENTITY_LIST_ID, Entities, &Length);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return 0;

    return Length / sizeof(TDIEntityID);
}

/* The one entity of the type Type that is in After and not in Before */
static
BOOLEAN
FindNewEntity(
    _In_reads_(BeforeCount) const TDIEntityID *Before,
    _In_ ULONG BeforeCount,
    _In_reads_(AfterCount) const TDIEntityID *After,
    _In_ ULONG AfterCount,
    _In_ ULONG Type,
    _Out_ TDIEntityID *Entity)
{
    ULONG i, j, Found = 0;

    for (i = 0; i < AfterCount; i++)
    {
        if (After[i].tei_entity != Type)
            continue;
        for (j = 0; j < BeforeCount; j++)
        {
            if (Before[j].tei_entity == After[i].tei_entity &&
                Before[j].tei_instance == After[i].tei_instance)
            {
                break;
            }
        }
        if (j == BeforeCount)
        {
            *Entity = After[i];
            Found++;
        }
    }

    ok(Found == 1, "%lu new entities of type %lx\n", Found, Type);
    return Found == 1;
}

static
VOID
TestSetInformation(void)
{
    NTSTATUS Status;
    HANDLE ControlHandle = NULL, OtherHandle = NULL, AddressHandle = NULL, ConnectionHandle = NULL;
    PFILE_OBJECT ControlFile = NULL, OtherFile = NULL, AddressFile = NULL, ConnectionFile = NULL;
    PDEVICE_OBJECT DeviceObject;
    TDIEntityID *Entities[3] = { NULL, NULL, NULL };
    ULONG Count[3];
    TDIEntityID OtherEntity, AddressEntity;
    CONNECTION_CONTEXT Context = (CONNECTION_CONTEXT)(ULONG_PTR)0x53455449;
    KEVENT Event;
    PIRP Irp;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Entities); i++)
    {
        Entities[i] = ExAllocatePoolWithTag(PagedPool, MAX_ENTITIES * sizeof(TDIEntityID), TAG_TEST);
        if (!Entities[i])
        {
            ok(0, "Out of memory\n");
            goto Cleanup;
        }
    }

    Status = OpenTdiObject(L"\\Device\\Tcp", NULL, 0, NULL, 0, &ControlHandle, &ControlFile);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Two address objects, each with an entity of its own: the other one
     * has no connection, the address one has the connection */
    Count[0] = QueryEntities(ControlFile, Entities[0]);
    Status = OpenAddress(TEST_SET_INFO_OTHER_PORT, &OtherHandle, &OtherFile);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Count[1] = QueryEntities(ControlFile, Entities[1]);
    Status = OpenAddress(TEST_SET_INFO_ADDRESS_PORT, &AddressHandle, &AddressFile);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Count[2] = QueryEntities(ControlFile, Entities[2]);
    if (!FindNewEntity(Entities[0], Count[0], Entities[1], Count[1], CO_TL_ENTITY, &OtherEntity) ||
        !FindNewEntity(Entities[1], Count[1], Entities[2], Count[2], CO_TL_ENTITY, &AddressEntity))
    {
        goto Cleanup;
    }

    Status = OpenTdiObject(L"\\Device\\Tcp", TdiConnectionContext, TDI_CONNECTION_CONTEXT_LENGTH,
                           &Context, sizeof(Context), &ConnectionHandle, &ConnectionFile);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    DeviceObject = IoGetRelatedDeviceObject(ConnectionFile);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    ok(Irp != NULL, "IoAllocateIrp failed\n");
    if (!Irp)
        goto Cleanup;
    TdiBuildAssociateAddress(Irp, DeviceObject, ConnectionFile, NULL, NULL, AddressHandle);
    IoSetCompletionRoutine(Irp, IrpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
    IoCallDriver(DeviceObject, Irp);
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = Irp->IoStatus.Status;
    ok_eq_hex(Status, STATUS_SUCCESS);
    IoFreeIrp(Irp);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* On the control channel, the entity names the address object */
    Status = SetInformationEx(ControlFile, OtherEntity, INFO_TYPE_ADDRESS_OBJECT, AO_OPTION_TTL,
                              TTL_BY_ENTITY, FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(QueryTtl(ControlFile, OtherEntity), (ULONG)TTL_BY_ENTITY);
    ok(QueryTtl(ControlFile, AddressEntity) != TTL_BY_ENTITY, "The TTL was set on the wrong address object\n");

    /* On an address object, the request is for that address object, not
     * the one its entity names */
    Status = SetInformationEx(AddressFile, OtherEntity, INFO_TYPE_ADDRESS_OBJECT, AO_OPTION_TTL,
                              TTL_BY_HANDLE, FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(QueryTtl(ControlFile, AddressEntity), (ULONG)TTL_BY_HANDLE);
    ok_eq_ulong(QueryTtl(ControlFile, OtherEntity), (ULONG)TTL_BY_ENTITY);
    /* So is a query */
    ok_eq_ulong(QueryTtl(AddressFile, OtherEntity), (ULONG)TTL_BY_HANDLE);
    ok_eq_ulong(QueryTtl(OtherFile, AddressEntity), (ULONG)TTL_BY_ENTITY);

    /* A connection option is for the connection the request came on.  The
     * entity names an address object without connections. */
    Status = SetInformationEx(ConnectionFile, OtherEntity, INFO_TYPE_CONNECTION, TCP_SOCKET_NODELAY,
                              TRUE, FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG));
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* No entity names a connection: its address object can have many */
    Status = SetInformationEx(ControlFile, AddressEntity, INFO_TYPE_CONNECTION, TCP_SOCKET_NODELAY,
                              TRUE, FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG));
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = SetInformationEx(AddressFile, AddressEntity, INFO_TYPE_CONNECTION, TCP_SOCKET_NODELAY,
                              TRUE, FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG));
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* An address object option on a connection is the connection's own:
     * its address object can be a listener's, which many connections share */
    ok_eq_ulong(QueryTtl(ConnectionFile, OtherEntity), 128UL);
    Status = SetInformationEx(ConnectionFile, OtherEntity, INFO_TYPE_ADDRESS_OBJECT, AO_OPTION_TTL,
                              TTL_BY_CONNECTION, FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(QueryTtl(ConnectionFile, OtherEntity), (ULONG)TTL_BY_CONNECTION);
    ok_eq_ulong(QueryTtl(ConnectionFile, AddressEntity), (ULONG)TTL_BY_CONNECTION);
    ok_eq_ulong(QueryTtl(ControlFile, AddressEntity), (ULONG)TTL_BY_HANDLE);
    ok_eq_ulong(QueryTtl(ControlFile, OtherEntity), (ULONG)TTL_BY_ENTITY);

    /* A request holds as much data as it says it has */
    Status = SetInformationEx(ControlFile, OtherEntity, INFO_TYPE_ADDRESS_OBJECT, AO_OPTION_TTL,
                              TTL_BY_HANDLE, FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + 1);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = SetInformationEx(ControlFile, OtherEntity, INFO_TYPE_ADDRESS_OBJECT, AO_OPTION_TTL,
                              TTL_BY_HANDLE, sizeof(TDIObjectID));
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_ulong(QueryTtl(ControlFile, OtherEntity), (ULONG)TTL_BY_ENTITY);

    /* Disassociate before the connection goes */
    KeClearEvent(&Event);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    ok(Irp != NULL, "IoAllocateIrp failed\n");
    if (Irp)
    {
        TdiBuildDisassociateAddress(Irp, DeviceObject, ConnectionFile, NULL, NULL);
        IoSetCompletionRoutine(Irp, IrpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
        IoCallDriver(DeviceObject, Irp);
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        ok_eq_hex(Irp->IoStatus.Status, STATUS_SUCCESS);
        IoFreeIrp(Irp);
    }

Cleanup:
    CloseTdiObject(ConnectionHandle, ConnectionFile);
    CloseTdiObject(AddressHandle, AddressFile);
    CloseTdiObject(OtherHandle, OtherFile);
    CloseTdiObject(ControlHandle, ControlFile);
    for (i = 0; i < RTL_NUMBER_OF(Entities); i++)
    {
        if (Entities[i])
            ExFreePoolWithTag(Entities[i], TAG_TEST);
    }
}

static KSTART_ROUTINE RunTest;
static
VOID
NTAPI
RunTest(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    TestSetInformation();
}

KMT_MESSAGE_HANDLER TestSetInfo;
NTSTATUS
TestSetInfo(
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
