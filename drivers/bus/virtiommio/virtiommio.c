/*
 * PROJECT:     ReactOS VirtIO MMIO transport driver
 * LICENSE:     GPL-2.0+
 * PURPOSE:     ACPI LNRO0005 probe and child PDO enumeration
 */

#include "virtiommio.h"
#include <initguid.h>
#include <ntstrsafe.h>
#include <wdmguid.h>

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

static PWCHAR
VirtioMmioDuplicateString(
    _In_ PCWSTR String)
{
    PWCHAR Buffer;
    SIZE_T Size = (wcslen(String) + 1) * sizeof(WCHAR);

    Buffer = ExAllocatePoolWithTag(PagedPool, Size, VIRTIO_MMIO_TAG);
    if (!Buffer)
        return NULL;

    RtlCopyMemory(Buffer, String, Size);
    return Buffer;
}

static NTSTATUS
VirtioMmioBuildMultiSz2(
    _In_ PCWSTR First,
    _In_ PCWSTR Second,
    _Out_ PWCHAR *OutBuffer)
{
    PWCHAR Buffer;
    SIZE_T FirstSize = (wcslen(First) + 1) * sizeof(WCHAR);
    SIZE_T SecondSize = (wcslen(Second) + 1) * sizeof(WCHAR);
    SIZE_T Size = FirstSize + SecondSize + sizeof(WCHAR);

    Buffer = ExAllocatePoolWithTag(PagedPool, Size, VIRTIO_MMIO_TAG);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Buffer, Size);
    RtlCopyMemory(Buffer, First, FirstSize);
    RtlCopyMemory((PUCHAR)Buffer + FirstSize, Second, SecondSize);
    *OutBuffer = Buffer;
    return STATUS_SUCCESS;
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
    _In_opt_ PCM_RESOURCE_LIST RawResources,
    _In_opt_ PCM_RESOURCE_LIST TranslatedResources)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG List;
    ULONG Index;

    if (!RawResources || !TranslatedResources)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (List = 0; List < RawResources->Count; ++List)
    {
        PCM_FULL_RESOURCE_DESCRIPTOR Full = &RawResources->List[List];

        for (Index = 0; Index < Full->PartialResourceList.Count; ++Index)
        {
            Descriptor = &Full->PartialResourceList.PartialDescriptors[Index];

            if (Descriptor->Type == CmResourceTypeInterrupt)
            {
                Extension->RawInterruptVector = Descriptor->u.Interrupt.Vector;
                Extension->RawInterruptLevel = Descriptor->u.Interrupt.Level;
                Extension->RawInterruptAffinity = Descriptor->u.Interrupt.Affinity;
            }
        }
    }

    for (List = 0; List < TranslatedResources->Count; ++List)
    {
        PCM_FULL_RESOURCE_DESCRIPTOR Full = &TranslatedResources->List[List];

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

static VOID
VirtioMmioDeleteChild(
    _Inout_ PVIRTIO_MMIO_EXTENSION Extension)
{
    if (Extension->ChildPdo)
    {
        IoDeleteDevice(Extension->ChildPdo);
        Extension->ChildPdo = NULL;
    }
}

static NTSTATUS
VirtioMmioCreateChild(
    _In_ PDEVICE_OBJECT Fdo)
{
    PVIRTIO_MMIO_EXTENSION Extension = Fdo->DeviceExtension;
    PVIRTIO_MMIO_PDO_EXTENSION PdoExtension;
    PDEVICE_OBJECT Pdo;
    NTSTATUS Status;

    if (Extension->ChildPdo || Extension->DeviceId == 0)
        return STATUS_SUCCESS;

    Status = IoCreateDevice(Fdo->DriverObject,
                            sizeof(VIRTIO_MMIO_PDO_EXTENSION),
                            NULL,
                            FILE_DEVICE_CONTROLLER,
                            FILE_AUTOGENERATED_DEVICE_NAME,
                            FALSE,
                            &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;

    PdoExtension = Pdo->DeviceExtension;
    RtlZeroMemory(PdoExtension, sizeof(*PdoExtension));

    PdoExtension->Common.Type = VirtioMmioPdo;
    PdoExtension->Common.Self = Pdo;
    PdoExtension->ParentFdo = Fdo;
    PdoExtension->Present = TRUE;
    PdoExtension->DeviceId = Extension->DeviceId;
    PdoExtension->VendorId = Extension->VendorId;
    PdoExtension->Version = Extension->Version;
    PdoExtension->RegistersPa = Extension->RegistersPa;
    PdoExtension->RegistersLength = Extension->RegistersLength;
    PdoExtension->RawInterruptVector = Extension->RawInterruptVector;
    PdoExtension->RawInterruptLevel = Extension->RawInterruptLevel;
    PdoExtension->RawInterruptAffinity = Extension->RawInterruptAffinity;
    PdoExtension->InterruptVector = Extension->InterruptVector;
    PdoExtension->InterruptLevel = Extension->InterruptLevel;
    PdoExtension->InterruptAffinity = Extension->InterruptAffinity;

    Pdo->Flags |= DO_DIRECT_IO | DO_BUS_ENUMERATED_DEVICE;
    Pdo->Flags &= ~DO_DEVICE_INITIALIZING;
    Extension->ChildPdo = Pdo;

    DPRINT1("virtiommio: created child PDO for device id %lu\n", Extension->DeviceId);
    return STATUS_SUCCESS;
}

static NTSTATUS
VirtioMmioProbe(
    _In_ PDEVICE_OBJECT Fdo)
{
    PVIRTIO_MMIO_EXTENSION Extension = Fdo->DeviceExtension;

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

    return VirtioMmioCreateChild(Fdo);
}

static NTSTATUS
VirtioMmioFdoQueryBusRelations(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_EXTENSION Extension = DeviceObject->DeviceExtension;
    PDEVICE_RELATIONS Relations;
    ULONG Count;
    ULONG Size;
    NTSTATUS Status;

    if (Extension->State == VirtioMmioStarted &&
        Extension->DeviceId != 0 &&
        !Extension->ChildPdo)
    {
        Status = VirtioMmioCreateChild(DeviceObject);
        if (!NT_SUCCESS(Status))
            return VirtioMmioCompleteIrp(Irp, Status);
    }

    Count = Extension->ChildPdo ? 1 : 0;

    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) +
           Count * sizeof(PDEVICE_OBJECT);

    Relations = ExAllocatePoolWithTag(PagedPool, Size, VIRTIO_MMIO_TAG);
    if (!Relations)
        return VirtioMmioCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);

    Relations->Count = Count;
    if (Count)
    {
        PVIRTIO_MMIO_PDO_EXTENSION PdoExtension = Extension->ChildPdo->DeviceExtension;
        PdoExtension->Reported = TRUE;
        Extension->ChildPdo->Flags |= DO_BUS_ENUMERATED_DEVICE;
        ObReferenceObject(Extension->ChildPdo);
        Relations->Objects[0] = Extension->ChildPdo;
    }

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    return VirtioMmioCompleteIrp(Irp, STATUS_SUCCESS);
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
                                    Stack->Parameters.StartDevice.AllocatedResources,
                                    Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
    if (NT_SUCCESS(Status))
        Status = VirtioMmioProbe(DeviceObject);

    if (!NT_SUCCESS(Status))
        VirtioMmioUnmapResources(Extension);
    else
    {
        Extension->State = VirtioMmioStarted;
        IoInvalidateDeviceRelations(Extension->Pdo, BusRelations);
    }

    return VirtioMmioCompleteIrp(Irp, Status);
}

static NTSTATUS
VirtioMmioFdoPnp(
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

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (Stack->Parameters.QueryDeviceRelations.Type == BusRelations)
            {
                Status = VirtioMmioFdoQueryBusRelations(DeviceObject, Irp);
                IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
                return Status;
            }
            break;

        case IRP_MN_STOP_DEVICE:
            VirtioMmioUnmapResources(Extension);
            Extension->State = VirtioMmioNotStarted;
            break;

        case IRP_MN_REMOVE_DEVICE:
            VirtioMmioDeleteChild(Extension);
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
VirtioMmioPdoQueryId(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack)
{
    PVIRTIO_MMIO_PDO_EXTENSION PdoExtension = DeviceObject->DeviceExtension;
    WCHAR DeviceId[32];
    WCHAR HardwareId[40];
    PWCHAR Buffer;
    NTSTATUS Status;

    switch (Stack->Parameters.QueryId.IdType)
    {
        case BusQueryDeviceID:
            Status = RtlStringCbPrintfW(DeviceId,
                                        sizeof(DeviceId),
                                        L"VIRTIO\\DEV_%04lX",
                                        PdoExtension->DeviceId);
            if (!NT_SUCCESS(Status))
                return Status;

            Buffer = VirtioMmioDuplicateString(DeviceId);
            if (!Buffer)
                return STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = (ULONG_PTR)Buffer;
            return STATUS_SUCCESS;

        case BusQueryHardwareIDs:
            Status = RtlStringCbPrintfW(DeviceId,
                                        sizeof(DeviceId),
                                        L"VIRTIO\\DEV_%04lX",
                                        PdoExtension->DeviceId);
            if (!NT_SUCCESS(Status))
                return Status;

            Status = RtlStringCbPrintfW(HardwareId,
                                        sizeof(HardwareId),
                                        L"VIRTIO\\MMIO&DEV_%04lX",
                                        PdoExtension->DeviceId);
            if (!NT_SUCCESS(Status))
                return Status;

            Status = VirtioMmioBuildMultiSz2(DeviceId, HardwareId, &Buffer);
            if (!NT_SUCCESS(Status))
                return Status;
            Irp->IoStatus.Information = (ULONG_PTR)Buffer;
            return STATUS_SUCCESS;

        case BusQueryCompatibleIDs:
            Status = VirtioMmioBuildMultiSz2(L"VIRTIO\\MMIO", L"VIRTIO", &Buffer);
            if (!NT_SUCCESS(Status))
                return Status;
            Irp->IoStatus.Information = (ULONG_PTR)Buffer;
            return STATUS_SUCCESS;

        case BusQueryInstanceID:
            Buffer = VirtioMmioDuplicateString(L"0");
            if (!Buffer)
                return STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = (ULONG_PTR)Buffer;
            return STATUS_SUCCESS;

        default:
            return Irp->IoStatus.Status;
    }
}

static NTSTATUS
VirtioMmioPdoQueryDeviceText(
    _Inout_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack)
{
    PWCHAR Buffer;

    if (Stack->Parameters.QueryDeviceText.DeviceTextType != DeviceTextDescription)
        return Irp->IoStatus.Status;

    Buffer = VirtioMmioDuplicateString(L"VirtIO MMIO device");
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    Irp->IoStatus.Information = (ULONG_PTR)Buffer;
    return STATUS_SUCCESS;
}

static NTSTATUS
VirtioMmioPdoQueryResources(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_PDO_EXTENSION PdoExtension = DeviceObject->DeviceExtension;
    PCM_RESOURCE_LIST ResourceList;
    PCM_PARTIAL_RESOURCE_LIST PartialList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Count = PdoExtension->RawInterruptVector ? 2 : 1;
    ULONG Size;

    Size = FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors) +
           Count * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);

    ResourceList = ExAllocatePoolWithTag(PagedPool, Size, VIRTIO_MMIO_TAG);
    if (!ResourceList)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(ResourceList, Size);
    ResourceList->Count = 1;
    ResourceList->List[0].InterfaceType = Internal;
    ResourceList->List[0].BusNumber = 0;

    PartialList = &ResourceList->List[0].PartialResourceList;
    PartialList->Version = 1;
    PartialList->Revision = 1;
    PartialList->Count = Count;

    Descriptor = &PartialList->PartialDescriptors[0];
    Descriptor->Type = CmResourceTypeMemory;
    Descriptor->ShareDisposition = CmResourceShareShared;
    Descriptor->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
    Descriptor->u.Memory.Start = PdoExtension->RegistersPa;
    Descriptor->u.Memory.Length = PdoExtension->RegistersLength;

    if (PdoExtension->RawInterruptVector)
    {
        Descriptor++;
        Descriptor->Type = CmResourceTypeInterrupt;
        Descriptor->ShareDisposition = CmResourceShareShared;
        Descriptor->Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
        Descriptor->u.Interrupt.Level = PdoExtension->RawInterruptLevel;
        Descriptor->u.Interrupt.Vector = PdoExtension->RawInterruptVector;
        Descriptor->u.Interrupt.Affinity = PdoExtension->RawInterruptAffinity;
    }

    Irp->IoStatus.Information = (ULONG_PTR)ResourceList;
    return STATUS_SUCCESS;
}

static NTSTATUS
VirtioMmioPdoQueryResourceRequirements(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_PDO_EXTENSION PdoExtension = DeviceObject->DeviceExtension;
    PIO_RESOURCE_REQUIREMENTS_LIST Requirements;
    PIO_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Count = PdoExtension->RawInterruptVector ? 2 : 1;
    ULONG Size;

    Size = sizeof(IO_RESOURCE_REQUIREMENTS_LIST) +
           (Count - 1) * sizeof(IO_RESOURCE_DESCRIPTOR);

    Requirements = ExAllocatePoolWithTag(PagedPool, Size, VIRTIO_MMIO_TAG);
    if (!Requirements)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Requirements, Size);
    Requirements->ListSize = Size;
    Requirements->InterfaceType = Internal;
    Requirements->BusNumber = 0;
    Requirements->SlotNumber = 0;
    Requirements->AlternativeLists = 1;
    Requirements->List[0].Version = 1;
    Requirements->List[0].Revision = 1;
    Requirements->List[0].Count = Count;

    Descriptor = &Requirements->List[0].Descriptors[0];
    Descriptor->Option = IO_RESOURCE_PREFERRED;
    Descriptor->Type = CmResourceTypeMemory;
    Descriptor->ShareDisposition = CmResourceShareShared;
    Descriptor->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
    Descriptor->u.Memory.Length = PdoExtension->RegistersLength;
    Descriptor->u.Memory.Alignment = 1;
    Descriptor->u.Memory.MinimumAddress = PdoExtension->RegistersPa;
    Descriptor->u.Memory.MaximumAddress.QuadPart =
        PdoExtension->RegistersPa.QuadPart + PdoExtension->RegistersLength - 1;

    if (PdoExtension->RawInterruptVector)
    {
        Descriptor++;
        Descriptor->Option = IO_RESOURCE_PREFERRED;
        Descriptor->Type = CmResourceTypeInterrupt;
        Descriptor->ShareDisposition = CmResourceShareShared;
        Descriptor->Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
        Descriptor->u.Interrupt.MinimumVector = PdoExtension->RawInterruptVector;
        Descriptor->u.Interrupt.MaximumVector = PdoExtension->RawInterruptVector;
    }

    Irp->IoStatus.Information = (ULONG_PTR)Requirements;
    return STATUS_SUCCESS;
}

static NTSTATUS
VirtioMmioPdoQueryCapabilities(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIO_STACK_LOCATION Stack)
{
    PVIRTIO_MMIO_PDO_EXTENSION PdoExtension = DeviceObject->DeviceExtension;
    PDEVICE_CAPABILITIES Caps = Stack->Parameters.DeviceCapabilities.Capabilities;

    if (Caps->Version != 1 || Caps->Size < sizeof(DEVICE_CAPABILITIES))
        return STATUS_UNSUCCESSFUL;

    Caps->Removable = FALSE;
    Caps->EjectSupported = FALSE;
    Caps->SurpriseRemovalOK = FALSE;
    Caps->UniqueID = TRUE;
    Caps->Address = PdoExtension->DeviceId;
    Caps->UINumber = MAXULONG;
    Caps->DeviceState[PowerSystemWorking] = PowerDeviceD0;
    Caps->DeviceState[PowerSystemSleeping1] = PowerDeviceD3;
    Caps->DeviceState[PowerSystemSleeping2] = PowerDeviceD3;
    Caps->DeviceState[PowerSystemSleeping3] = PowerDeviceD3;
    Caps->DeviceState[PowerSystemHibernate] = PowerDeviceD3;
    Caps->DeviceState[PowerSystemShutdown] = PowerDeviceD3;

    return STATUS_SUCCESS;
}

static NTSTATUS
VirtioMmioPdoQueryDeviceRelations(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack)
{
    PDEVICE_RELATIONS Relations;

    if (Stack->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
        return Irp->IoStatus.Status;

    Relations = ExAllocatePoolWithTag(PagedPool,
                                      sizeof(DEVICE_RELATIONS),
                                      VIRTIO_MMIO_TAG);
    if (!Relations)
        return STATUS_INSUFFICIENT_RESOURCES;

    Relations->Count = 1;
    Relations->Objects[0] = DeviceObject;
    ObReferenceObject(DeviceObject);
    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    return STATUS_SUCCESS;
}

static NTSTATUS
VirtioMmioPdoQueryBusInformation(
    _Inout_ PIRP Irp)
{
    PPNP_BUS_INFORMATION BusInfo;

    BusInfo = ExAllocatePoolWithTag(PagedPool,
                                    sizeof(PNP_BUS_INFORMATION),
                                    VIRTIO_MMIO_TAG);
    if (!BusInfo)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(BusInfo, sizeof(PNP_BUS_INFORMATION));
    BusInfo->BusTypeGuid = GUID_BUS_TYPE_INTERNAL;
    BusInfo->LegacyBusType = Internal;
    BusInfo->BusNumber = 0;

    Irp->IoStatus.Information = (ULONG_PTR)BusInfo;
    return STATUS_SUCCESS;
}

static NTSTATUS
VirtioMmioPdoPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_PDO_EXTENSION PdoExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_ID:
            Status = VirtioMmioPdoQueryId(DeviceObject, Irp, Stack);
            break;

        case IRP_MN_QUERY_DEVICE_TEXT:
            Status = VirtioMmioPdoQueryDeviceText(Irp, Stack);
            break;

        case IRP_MN_QUERY_RESOURCES:
            Status = VirtioMmioPdoQueryResources(DeviceObject, Irp);
            break;

        case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
            Status = VirtioMmioPdoQueryResourceRequirements(DeviceObject, Irp);
            break;

        case IRP_MN_QUERY_CAPABILITIES:
            Status = VirtioMmioPdoQueryCapabilities(DeviceObject, Stack);
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            Status = VirtioMmioPdoQueryDeviceRelations(DeviceObject, Irp, Stack);
            break;

        case IRP_MN_QUERY_BUS_INFORMATION:
            Status = VirtioMmioPdoQueryBusInformation(Irp);
            break;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_STOP_DEVICE:
            Status = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
            PdoExtension->Present = FALSE;
            if (PdoExtension->ParentFdo)
            {
                PVIRTIO_MMIO_EXTENSION ParentExtension = PdoExtension->ParentFdo->DeviceExtension;

                if (ParentExtension->ChildPdo == DeviceObject)
                    ParentExtension->ChildPdo = NULL;
            }
            Status = STATUS_SUCCESS;
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            IoDeleteDevice(DeviceObject);
            return Status;

        default:
            Status = Irp->IoStatus.Status;
            break;
    }

    return VirtioMmioCompleteIrp(Irp, Status);
}

static NTSTATUS
NTAPI
VirtioMmioDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_COMMON_EXTENSION Common = DeviceObject->DeviceExtension;

    if (Common->Type == VirtioMmioPdo)
        return VirtioMmioPdoPnp(DeviceObject, Irp);

    return VirtioMmioFdoPnp(DeviceObject, Irp);
}

static NTSTATUS
NTAPI
VirtioMmioDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVIRTIO_MMIO_COMMON_EXTENSION Common = DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);

    if (Common->Type == VirtioMmioPdo)
        return VirtioMmioCompleteIrp(Irp, STATUS_SUCCESS);

    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(((PVIRTIO_MMIO_EXTENSION)Common)->LowerDevice, Irp);
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
    PVIRTIO_MMIO_COMMON_EXTENSION Common = DeviceObject->DeviceExtension;

    if (Common->Type == VirtioMmioPdo)
        return VirtioMmioCompleteIrp(Irp, Irp->IoStatus.Status);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(((PVIRTIO_MMIO_EXTENSION)Common)->LowerDevice, Irp);
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

    Extension->Common.Type = VirtioMmioFdo;
    Extension->Common.Self = Fdo;
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
