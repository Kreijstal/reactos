/*
 * PROJECT:     ReactOS VirtIO MMIO transport driver
 * LICENSE:     GPL-2.0+
 * PURPOSE:     ACPI LNRO0005 probe and MMIO resource handling
 */

#include "virtiommio.h"

#define NDEBUG
#include <debug.h>

static DRIVER_ADD_DEVICE VirtioMmioAddDevice;
static DRIVER_DISPATCH VirtioMmioDispatchCreateClose;
static DRIVER_DISPATCH VirtioMmioDispatchPnp;
static DRIVER_DISPATCH VirtioMmioDispatchPower;
static DRIVER_DISPATCH VirtioMmioDispatchPassThrough;
static DRIVER_UNLOAD VirtioMmioUnload;

static NTSTATUS
VirtioMmioCompleteIrp(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS
NTAPI
VirtioMmioCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PKEVENT Event = (PKEVENT)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
VirtioMmioForwardIrpSynchronously(
    _In_ PVIRTIO_MMIO_EXTENSION Extension,
    _Inout_ PIRP Irp)
{
    KEVENT Event;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           VirtioMmioCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    Status = IoCallDriver(Extension->LowerDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }

    return Status;
}

static VOID
VirtioMmioUnmapResources(
    _Inout_ PVIRTIO_MMIO_EXTENSION Extension)
{
    if (Extension->Registers)
    {
        MmUnmapIoSpace(Extension->Registers, Extension->RegistersLength);
        Extension->Registers = NULL;
    }

    Extension->RegistersLength = 0;
}

static NTSTATUS
VirtioMmioMapResources(
    _Inout_ PVIRTIO_MMIO_EXTENSION Extension,
    _In_opt_ PCM_RESOURCE_LIST Resources)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG List;
    ULONG Index;

    if (!Resources)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (List = 0; List < Resources->Count; ++List)
    {
        PCM_FULL_RESOURCE_DESCRIPTOR Full = &Resources->List[List];

        for (Index = 0; Index < Full->PartialResourceList.Count; ++Index)
        {
            Descriptor = &Full->PartialResourceList.PartialDescriptors[Index];

            switch (Descriptor->Type)
            {
                case CmResourceTypeMemory:
                    if (!Extension->Registers)
                    {
                        Extension->RegistersPa = Descriptor->u.Memory.Start;
                        Extension->RegistersLength = Descriptor->u.Memory.Length;
                        Extension->Registers = MmMapIoSpace(Extension->RegistersPa,
                                                            Extension->RegistersLength,
                                                            MmNonCached);
                        if (!Extension->Registers)
                            return STATUS_INSUFFICIENT_RESOURCES;
                    }
                    break;

                case CmResourceTypeInterrupt:
                    Extension->InterruptVector = Descriptor->u.Interrupt.Vector;
                    Extension->InterruptLevel = (KIRQL)Descriptor->u.Interrupt.Level;
                    Extension->InterruptAffinity = Descriptor->u.Interrupt.Affinity;
                    break;

                default:
                    break;
            }
        }
    }

    if (!Extension->Registers || Extension->RegistersLength < VIRTIO_MMIO_CONFIG)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    return STATUS_SUCCESS;
}
static NTSTATUS
VirtioMmioProbe(
    _Inout_ PVIRTIO_MMIO_EXTENSION Extension)
{
    Extension->Magic = READ_REGISTER_ULONG((PULONG)(Extension->Registers + VIRTIO_MMIO_MAGIC_VALUE));
    Extension->Version = READ_REGISTER_ULONG((PULONG)(Extension->Registers + VIRTIO_MMIO_VERSION));
    Extension->DeviceId = READ_REGISTER_ULONG((PULONG)(Extension->Registers + VIRTIO_MMIO_DEVICE_ID));
    Extension->VendorId = READ_REGISTER_ULONG((PULONG)(Extension->Registers + VIRTIO_MMIO_VENDOR_ID));

    DPRINT1("virtiommio: base %I64x length %lu magic %08lx version %lu device %lu vendor %08lx irq %lu\n",
            Extension->RegistersPa.QuadPart,
            Extension->RegistersLength,
            Extension->Magic,
            Extension->Version,
            Extension->DeviceId,
            Extension->VendorId,
            Extension->InterruptVector);

    if (Extension->Magic != VIRTIO_MMIO_MAGIC)
    {
        DPRINT1("virtiommio: unexpected magic %08lx\n", Extension->Magic);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    if (Extension->Version != 1 && Extension->Version != 2)
    {
        DPRINT1("virtiommio: unsupported version %lu\n", Extension->Version);
        return STATUS_NOT_SUPPORTED;
    }

    if (Extension->DeviceId == 0)
    {
        DPRINT1("virtiommio: empty transport slot\n");
        return STATUS_SUCCESS;
    }

    WRITE_REGISTER_ULONG((PULONG)(Extension->Registers + VIRTIO_MMIO_STATUS), 0);
    (VOID)READ_REGISTER_ULONG((PULONG)(Extension->Registers + VIRTIO_MMIO_STATUS));

    DPRINT1("virtiommio: detected virtio device id %lu, child PDO support pending\n",
            Extension->DeviceId);

    return STATUS_SUCCESS;
}

static NTSTATUS
VirtioMmioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    Status = VirtioMmioForwardIrpSynchronously(Extension, Irp);
    if (!NT_SUCCESS(Status))
        return VirtioMmioCompleteIrp(Irp, Status);

    Stack = IoGetCurrentIrpStackLocation(Irp);
    Status = VirtioMmioMapResources(Extension,
                                    Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
    if (NT_SUCCESS(Status))
        Status = VirtioMmioProbe(Extension);

    if (!NT_SUCCESS(Status))
        VirtioMmioUnmapResources(Extension);
    else
        Extension->State = VirtioMmioStarted;

    return VirtioMmioCompleteIrp(Irp, Status);
}

static NTSTATUS
NTAPI
VirtioMmioDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
        return VirtioMmioCompleteIrp(Irp, Status);

    Stack = IoGetCurrentIrpStackLocation(Irp);

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = VirtioMmioStartDevice(DeviceObject, Irp);
            IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
            return Status;

        case IRP_MN_STOP_DEVICE:
            VirtioMmioUnmapResources(Extension);
            Extension->State = VirtioMmioNotStarted;
            break;

        case IRP_MN_REMOVE_DEVICE:
            VirtioMmioUnmapResources(Extension);
            Extension->State = VirtioMmioRemoved;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->LowerDevice, Irp);
            IoReleaseRemoveLockAndWait(&Extension->RemoveLock, Irp);
            IoDetachDevice(Extension->LowerDevice);
            IoDeleteDevice(DeviceObject);
            return Status;

        default:
            break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(Extension->LowerDevice, Irp);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

static NTSTATUS
NTAPI
VirtioMmioDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_EXTENSION Extension = DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Extension->LowerDevice, Irp);
}

static NTSTATUS
NTAPI
VirtioMmioDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    return VirtioMmioCompleteIrp(Irp, STATUS_SUCCESS);
}

static NTSTATUS
NTAPI
VirtioMmioDispatchPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_EXTENSION Extension = DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->LowerDevice, Irp);
}

static NTSTATUS
NTAPI
VirtioMmioAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT Fdo;
    PVIRTIO_MMIO_EXTENSION Extension;
    NTSTATUS Status;

    Status = IoCreateDevice(DriverObject,
                            sizeof(VIRTIO_MMIO_EXTENSION),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &Fdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Extension = Fdo->DeviceExtension;
    RtlZeroMemory(Extension, sizeof(*Extension));

    Extension->Self = Fdo;
    Extension->Pdo = PhysicalDeviceObject;
    Extension->State = VirtioMmioNotStarted;
    IoInitializeRemoveLock(&Extension->RemoveLock, VIRTIO_MMIO_TAG, 0, 0);

    Extension->LowerDevice = IoAttachDeviceToDeviceStack(Fdo, PhysicalDeviceObject);
    if (!Extension->LowerDevice)
    {
        IoDeleteDevice(Fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    Fdo->Flags |= Extension->LowerDevice->Flags & (DO_BUFFERED_IO |
                                                   DO_DIRECT_IO |
                                                   DO_POWER_PAGABLE);
    Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

static VOID
NTAPI
VirtioMmioUnload(
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
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverExtension->AddDevice = VirtioMmioAddDevice;
    DriverObject->DriverUnload = VirtioMmioUnload;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
        DriverObject->MajorFunction[i] = VirtioMmioDispatchPassThrough;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = VirtioMmioDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = VirtioMmioDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_PNP] = VirtioMmioDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = VirtioMmioDispatchPower;

    return STATUS_SUCCESS;
}
