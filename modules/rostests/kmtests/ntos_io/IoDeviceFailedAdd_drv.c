/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Restart of a device whose AddDevice failed, test driver
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <ntddk.h>

#include "IoDeviceFailedAdd.h"

#define TAG_FAILEDADD 'AFtK'

typedef enum _FAILEDADD_KIND
{
    BusFdo,
    ChildPdo,
    ChildFdo,
} FAILEDADD_KIND;

typedef struct _FAILEDADD_EXTENSION
{
    FAILEDADD_KIND Kind;
    /* BusFdo, ChildFdo: the device below */
    PDEVICE_OBJECT LowerDevice;
    /* BusFdo: its child */
    PDEVICE_OBJECT Child;
} FAILEDADD_EXTENSION, *PFAILEDADD_EXTENSION;

static LONG FailedAdds;
static LONG DeviceAdds;
static LONG Starts;
static WCHAR ParametersPath[256];

static
VOID
SaveCounter(
    _In_ PCWSTR Name,
    _In_ LONG Value)
{
    ULONG Data = (ULONG)Value;

    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, ParametersPath, Name,
                          REG_DWORD, &Data, sizeof(Data));
}

/* Whether the "installation" wrote the child's registry value */
static
BOOLEAN
ChildInstalled(
    _In_ PDEVICE_OBJECT Pdo)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(IOFAILEDADD_VALUE_INSTALLED);
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    HANDLE Key;
    ULONG Length;
    NTSTATUS Status;

    Status = IoOpenDeviceRegistryKey(Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_QUERY_VALUE, &Key);
    if (!NT_SUCCESS(Status))
        return FALSE;
    Status = ZwQueryValueKey(Key, &Name, KeyValuePartialInformation, Info, sizeof(Buffer), &Length);
    ZwClose(Key);
    return NT_SUCCESS(Status) && Info->Type == REG_DWORD && *(PULONG)Info->Data != 0;
}

static
NTSTATUS
NTAPI
FailedAddAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT Fdo;
    PFAILEDADD_EXTENSION Extension;
    BOOLEAN IsChild = (Pdo->DriverObject == DriverObject);
    NTSTATUS Status;

    if (IsChild && !ChildInstalled(Pdo))
    {
        /* As NDIS without the adapter's Linkage key */
        SaveCounter(IOFAILEDADD_VALUE_FAILED_ADDS, InterlockedIncrement(&FailedAdds));
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Status = IoCreateDevice(DriverObject, sizeof(FAILEDADD_EXTENSION), NULL,
                            FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &Fdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Extension = Fdo->DeviceExtension;
    RtlZeroMemory(Extension, sizeof(*Extension));
    Extension->Kind = IsChild ? ChildFdo : BusFdo;
    Extension->LowerDevice = IoAttachDeviceToDeviceStack(Fdo, Pdo);
    if (Extension->LowerDevice == NULL)
    {
        IoDeleteDevice(Fdo);
        return STATUS_NO_SUCH_DEVICE;
    }
    Fdo->Flags |= Extension->LowerDevice->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    if (IsChild)
        SaveCounter(IOFAILEDADD_VALUE_DEVICE_ADDS, InterlockedIncrement(&DeviceAdds));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
FailedAddCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Passes the IRP down and waits for it: IRP_MN_START_DEVICE goes to the bus first */
static
NTSTATUS
ForwardAndWait(
    _In_ PDEVICE_OBJECT LowerDevice,
    _In_ PIRP Irp)
{
    KEVENT Event;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, FailedAddCompletion, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(LowerDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }
    return Status;
}

static
PWCHAR
AllocateId(
    _In_ PCWSTR Id,
    _In_ BOOLEAN MultiSz)
{
    SIZE_T Length = (wcslen(Id) + (MultiSz ? 2 : 1)) * sizeof(WCHAR);
    PWCHAR Buffer;

    Buffer = ExAllocatePoolWithTag(PagedPool, Length, TAG_FAILEDADD);
    if (Buffer)
    {
        RtlZeroMemory(Buffer, Length);
        RtlCopyMemory(Buffer, Id, wcslen(Id) * sizeof(WCHAR));
    }
    return Buffer;
}

static
NTSTATUS
ChildPdoPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = Irp->IoStatus.Status;
    PDEVICE_RELATIONS Relations;
    PDEVICE_CAPABILITIES Capabilities;
    PWCHAR Id = NULL;

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        /* The child is still there: its PDO stays, as a PCI device's does */
        case IRP_MN_REMOVE_DEVICE:
            Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_ID:
            switch (IoStack->Parameters.QueryId.IdType)
            {
                case BusQueryDeviceID:
                    Id = AllocateId(IOFAILEDADD_CHILD_DEVICE_ID, FALSE);
                    break;
                case BusQueryHardwareIDs:
                    Id = AllocateId(IOFAILEDADD_CHILD_DEVICE_ID, TRUE);
                    break;
                case BusQueryInstanceID:
                    Id = AllocateId(IOFAILEDADD_CHILD_INSTANCE_ID, FALSE);
                    break;
                default:
                    goto Complete;
            }
            Status = Id ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = (ULONG_PTR)Id;
            break;

        case IRP_MN_QUERY_DEVICE_TEXT:
            if (IoStack->Parameters.QueryDeviceText.DeviceTextType != DeviceTextDescription)
                break;
            Id = AllocateId(L"Failed AddDevice test child", FALSE);
            Status = Id ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = (ULONG_PTR)Id;
            break;

        case IRP_MN_QUERY_CAPABILITIES:
            Capabilities = IoStack->Parameters.DeviceCapabilities.Capabilities;
            Capabilities->UniqueID = TRUE;
            Capabilities->SilentInstall = TRUE;
            Capabilities->RawDeviceOK = FALSE;
            Capabilities->Removable = FALSE;
            Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (IoStack->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
                break;
            Relations = ExAllocatePoolWithTag(PagedPool, sizeof(*Relations), TAG_FAILEDADD);
            if (!Relations)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            ObReferenceObject(DeviceObject);
            Relations->Count = 1;
            Relations->Objects[0] = DeviceObject;
            Irp->IoStatus.Information = (ULONG_PTR)Relations;
            Status = STATUS_SUCCESS;
            break;
    }

Complete:
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
BusFdoPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PFAILEDADD_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT LowerDevice = Extension->LowerDevice;
    PFAILEDADD_EXTENSION ChildExtension;
    PDEVICE_RELATIONS Relations;
    NTSTATUS Status;

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = ForwardAndWait(LowerDevice, Irp);
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (IoStack->Parameters.QueryDeviceRelations.Type != BusRelations)
                break;
            if (Extension->Child == NULL)
            {
                Status = IoCreateDevice(DeviceObject->DriverObject, sizeof(FAILEDADD_EXTENSION), NULL,
                                        FILE_DEVICE_UNKNOWN,
                                        FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN,
                                        FALSE, &Extension->Child);
                if (!NT_SUCCESS(Status))
                {
                    Extension->Child = NULL;
                    break;
                }
                ChildExtension = Extension->Child->DeviceExtension;
                RtlZeroMemory(ChildExtension, sizeof(*ChildExtension));
                ChildExtension->Kind = ChildPdo;
                Extension->Child->Flags &= ~DO_DEVICE_INITIALIZING;
            }
            Relations = ExAllocatePoolWithTag(PagedPool, sizeof(*Relations), TAG_FAILEDADD);
            if (!Relations)
            {
                Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            ObReferenceObject(Extension->Child);
            Relations->Count = 1;
            Relations->Objects[0] = Extension->Child;
            Irp->IoStatus.Information = (ULONG_PTR)Relations;
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
            /* The child goes with its bus */
            if (Extension->Child)
            {
                IoDeleteDevice(Extension->Child);
                Extension->Child = NULL;
            }
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(LowerDevice, Irp);
            IoDetachDevice(LowerDevice);
            IoDeleteDevice(DeviceObject);
            return Status;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(LowerDevice, Irp);
}

static
NTSTATUS
ChildFdoPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PFAILEDADD_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT LowerDevice = Extension->LowerDevice;
    NTSTATUS Status;

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = ForwardAndWait(LowerDevice, Irp);
            if (NT_SUCCESS(Status))
                SaveCounter(IOFAILEDADD_VALUE_STARTS, InterlockedIncrement(&Starts));
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;

        case IRP_MN_REMOVE_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(LowerDevice, Irp);
            IoDetachDevice(LowerDevice);
            IoDeleteDevice(DeviceObject);
            return Status;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(LowerDevice, Irp);
}

static
NTSTATUS
NTAPI
FailedAddDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PFAILEDADD_EXTENSION Extension = DeviceObject->DeviceExtension;

    switch (Extension->Kind)
    {
        case BusFdo:
            return BusFdoPnp(DeviceObject, Irp);
        case ChildPdo:
            return ChildPdoPnp(DeviceObject, Irp);
        default:
            return ChildFdoPnp(DeviceObject, Irp);
    }
}

static
NTSTATUS
NTAPI
FailedAddDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PFAILEDADD_EXTENSION Extension = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    PoStartNextPowerIrp(Irp);
    if (Extension->Kind == ChildPdo)
    {
        Status = Irp->IoStatus.Status;
        if (IoGetCurrentIrpStackLocation(Irp)->MinorFunction == IRP_MN_SET_POWER ||
            IoGetCurrentIrpStackLocation(Irp)->MinorFunction == IRP_MN_QUERY_POWER)
        {
            Status = STATUS_SUCCESS;
        }
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Extension->LowerDevice, Irp);
}

static
NTSTATUS
NTAPI
FailedAddDispatchOther(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PFAILEDADD_EXTENSION Extension = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    if (Extension->Kind == ChildPdo)
    {
        Status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->LowerDevice, Irp);
}

static
VOID
NTAPI
FailedAddUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    static const WCHAR Suffix[] = L"\\Parameters";
    ULONG i;

    if (RegistryPath->Length + sizeof(Suffix) > sizeof(ParametersPath))
        return STATUS_NAME_TOO_LONG;
    RtlCopyMemory(ParametersPath, RegistryPath->Buffer, RegistryPath->Length);
    RtlCopyMemory((PUCHAR)ParametersPath + RegistryPath->Length, Suffix, sizeof(Suffix));
    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, ParametersPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = FailedAddDispatchOther;
    DriverObject->MajorFunction[IRP_MJ_PNP] = FailedAddDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = FailedAddDispatchPower;
    DriverObject->DriverExtension->AddDevice = FailedAddAddDevice;
    DriverObject->DriverUnload = FailedAddUnload;

    return STATUS_SUCCESS;
}
