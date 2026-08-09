/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shortcuts for sending different IRP_MJ_PNP requests
 * COPYRIGHT:   Copyright 2010 Sir Richard <sir_richard@svn.reactos.org>
 *              Copyright 2020 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

static BOOLEAN
PiKdHarvestEnabled(VOID)
{
    return KeLoaderBlock != NULL &&
           KeLoaderBlock->LoadOptions != NULL &&
           strstr(KeLoaderBlock->LoadOptions, "PNP-HARVEST") != NULL;
}

static VOID
PiKdHarvestDeviceStack(_In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT DeviceObject;
    PDRIVER_OBJECT DriverObject;
    ULONG Depth;

    if (!PiKdHarvestEnabled())
        return;

    for (DeviceObject = PhysicalDeviceObject, Depth = 0;
         DeviceObject != NULL && Depth < 16;
         DeviceObject = DeviceObject->AttachedDevice, ++Depth)
    {
        DriverObject = DeviceObject->DriverObject;
        if (DriverObject != NULL)
        {
            DPRINT1("PNPHARVEST: stack[%lu] device=%p extension=%p driver=%p name=\"%wZ\" type=0x%lx flags=0x%08lx characteristics=0x%08lx stack=%u attached=%p\n",
                    Depth,
                    DeviceObject,
                    DeviceObject->DeviceExtension,
                    DriverObject,
                    &DriverObject->DriverName,
                    DeviceObject->DeviceType,
                    DeviceObject->Flags,
                    DeviceObject->Characteristics,
                    DeviceObject->StackSize,
                    DeviceObject->AttachedDevice);
        }
        else
        {
            DPRINT1("PNPHARVEST: stack[%lu] device=%p extension=%p driver=<null> type=0x%lx flags=0x%08lx characteristics=0x%08lx stack=%u attached=%p\n",
                    Depth,
                    DeviceObject,
                    DeviceObject->DeviceExtension,
                    DeviceObject->DeviceType,
                    DeviceObject->Flags,
                    DeviceObject->Characteristics,
                    DeviceObject->StackSize,
                    DeviceObject->AttachedDevice);
        }
    }
    if (DeviceObject != NULL)
        DPRINT1("PNPHARVEST: device stack truncated after %lu entries\n", Depth);
}

static VOID
PiKdHarvestResourceList(_In_ PCSTR Label,
                        _In_opt_ PCM_RESOURCE_LIST ResourceList)
{
    PCM_FULL_RESOURCE_DESCRIPTOR Full;
    PCM_PARTIAL_RESOURCE_LIST Partial;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG FullIndex, PartialIndex;

    if (!PiKdHarvestEnabled())
        return;
    if (ResourceList == NULL)
    {
        DPRINT1("PNPHARVEST: %s resources=<null>\n", Label);
        return;
    }

    DPRINT1("PNPHARVEST: %s list=%p full_count=%lu\n",
            Label,
            ResourceList,
            ResourceList->Count);
    Full = &ResourceList->List[0];
    for (FullIndex = 0;
         FullIndex < ResourceList->Count && FullIndex < 16;
         ++FullIndex)
    {
        Partial = &Full->PartialResourceList;
        DPRINT1("PNPHARVEST: %s full[%lu] interface=%u bus=%lu version=%u revision=%u partial_count=%lu\n",
                Label,
                FullIndex,
                Full->InterfaceType,
                Full->BusNumber,
                Partial->Version,
                Partial->Revision,
                Partial->Count);
        Descriptor = &Partial->PartialDescriptors[0];
        for (PartialIndex = 0; PartialIndex < Partial->Count; ++PartialIndex)
        {
            if (PartialIndex < 64)
            {
                DPRINT1("PNPHARVEST: %s full[%lu] partial[%lu] type=%u share=%u flags=0x%04x\n",
                        Label,
                        FullIndex,
                        PartialIndex,
                        Descriptor->Type,
                        Descriptor->ShareDisposition,
                        Descriptor->Flags);
                switch (Descriptor->Type)
                {
                    case CmResourceTypePort:
                        DPRINT1("PNPHARVEST: %s port start=%I64x length=0x%lx\n",
                                Label,
                                Descriptor->u.Port.Start.QuadPart,
                                Descriptor->u.Port.Length);
                        break;
                    case CmResourceTypeInterrupt:
                        DPRINT1("PNPHARVEST: %s interrupt level=%lu vector=%lu affinity=%p\n",
                                Label,
                                Descriptor->u.Interrupt.Level,
                                Descriptor->u.Interrupt.Vector,
                                (PVOID)Descriptor->u.Interrupt.Affinity);
                        break;
                    case CmResourceTypeMemory:
                        DPRINT1("PNPHARVEST: %s memory start=%I64x length=0x%lx\n",
                                Label,
                                Descriptor->u.Memory.Start.QuadPart,
                                Descriptor->u.Memory.Length);
                        break;
                    case CmResourceTypeDma:
                        DPRINT1("PNPHARVEST: %s dma channel=%lu port=%lu\n",
                                Label,
                                Descriptor->u.Dma.Channel,
                                Descriptor->u.Dma.Port);
                        break;
                    case CmResourceTypeBusNumber:
                        DPRINT1("PNPHARVEST: %s bus start=%lu length=%lu\n",
                                Label,
                                Descriptor->u.BusNumber.Start,
                                Descriptor->u.BusNumber.Length);
                        break;
                    case CmResourceTypeDeviceSpecific:
                        DPRINT1("PNPHARVEST: %s device_specific bytes=%lu\n",
                                Label,
                                Descriptor->u.DeviceSpecificData.DataSize);
                        break;
                    default:
                        break;
                }
            }

            if (Descriptor->Type == CmResourceTypeDeviceSpecific)
            {
                Descriptor = (PCM_PARTIAL_RESOURCE_DESCRIPTOR)
                    ((PUCHAR)(Descriptor + 1) +
                     Descriptor->u.DeviceSpecificData.DataSize);
            }
            else
            {
                ++Descriptor;
            }
        }
        Full = (PCM_FULL_RESOURCE_DESCRIPTOR)Descriptor;
    }
}

NTSTATUS
IopSynchronousCall(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIO_STACK_LOCATION IoStackLocation,
    _Out_ PVOID *Information)
{
    PIRP Irp;
    PIO_STACK_LOCATION IrpStack;
    IO_STATUS_BLOCK IoStatusBlock;
    KEVENT Event;
    NTSTATUS Status;
    PDEVICE_OBJECT TopDeviceObject;
    PAGED_CODE();

#if defined(_M_ARM64)
    {
        static LONG EntryDiagCount = 0;
        ULONG64 EntryDaif;
        __asm__ __volatile__("mrs %0, daif" : "=r"(EntryDaif));
        if ((EntryDaif & (1ULL << 7)) && (EntryDiagCount < 4))
        {
            EntryDiagCount++;
            DPRINT1("SYNC-ENTRY-DIAG: masked at entry! Daif=%llx Irql=%u thread=%p\n",
                    EntryDaif, KeGetCurrentIrql(), KeGetCurrentThread());
        }
    }
#endif

    /* Call the top of the device stack */
    TopDeviceObject = IoGetAttachedDeviceReference(DeviceObject);

    /* Allocate an IRP */
    Irp = IoAllocateIrp(TopDeviceObject->StackSize, FALSE);
    if (!Irp)
    {
        ObDereferenceObject(TopDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initialize to failure */
    Irp->IoStatus.Status = IoStatusBlock.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = IoStatusBlock.Information = 0;

    /* Special case for IRP_MN_FILTER_RESOURCE_REQUIREMENTS */
    if ((IoStackLocation->MajorFunction == IRP_MJ_PNP) &&
        (IoStackLocation->MinorFunction == IRP_MN_FILTER_RESOURCE_REQUIREMENTS))
    {
        /* Copy the resource requirements list into the IOSB */
        Irp->IoStatus.Information =
        IoStatusBlock.Information = (ULONG_PTR)IoStackLocation->Parameters.FilterResourceRequirements.IoResourceRequirementList;
    }

    /* Initialize the event */
    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    /* Set them up */
    Irp->UserIosb = &IoStatusBlock;
    Irp->UserEvent = &Event;

    /* Queue the IRP */
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    IoQueueThreadIrp(Irp);

    /* Copy-in the stack */
    IrpStack = IoGetNextIrpStackLocation(Irp);
    *IrpStack = *IoStackLocation;

    /* Call the driver */
    if (PiKdHarvestEnabled())
    {
        DPRINT1("PNPHARVEST: dispatch irp=%p major=0x%02x minor=0x%02x device=%p top=%p driver=%p name=\"%wZ\"\n",
                Irp,
                IrpStack->MajorFunction,
                IrpStack->MinorFunction,
                DeviceObject,
                TopDeviceObject,
                TopDeviceObject->DriverObject,
                &TopDeviceObject->DriverObject->DriverName);
    }
    Status = IoCallDriver(TopDeviceObject, Irp);
    if (PiKdHarvestEnabled())
    {
        DPRINT1("PNPHARVEST: IoCallDriver returned 0x%08lx irp=%p pending=%u iosb=0x%08lx information=%p\n",
                Status,
                Irp,
                Status == STATUS_PENDING,
                IoStatusBlock.Status,
                (PVOID)IoStatusBlock.Information);
    }
    /* Otherwise we may get stuck here or have IoStatusBlock not populated */
    ASSERT(!KeAreAllApcsDisabled());
#if defined(_M_ARM64)
    if ((Status != STATUS_PENDING) &&
        (KeGetCurrentThread()->ApcState.KernelApcPending))
    {
        ULONG64 DiagDaif;
        __asm__ __volatile__("mrs %0, daif" : "=r"(DiagDaif));
        DPRINT1("SYNC-DIAG: APC still pending after IoCallDriver! Irql=%u Daif=%llx SpecialApcDisable=%d KernelApcDisable=%d Signal=%ld\n",
                KeGetCurrentIrql(), DiagDaif,
                KeGetCurrentThread()->SpecialApcDisable,
                KeGetCurrentThread()->KernelApcDisable,
                Event.Header.SignalState);
    }
#endif
    if (Status == STATUS_PENDING)
    {
        if (PiKdHarvestEnabled())
            DPRINT1("PNPHARVEST: waiting for irp=%p completion event=%p\n", Irp, &Event);
        /* Wait for it */
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatusBlock.Status;
        if (PiKdHarvestEnabled())
        {
            DPRINT1("PNPHARVEST: wait completed irp=%p status=0x%08lx information=%p\n",
                    Irp,
                    Status,
                    (PVOID)IoStatusBlock.Information);
        }
    }

    /* Remove the reference */
    ObDereferenceObject(TopDeviceObject);

    /* Return the information */
    *Information = (PVOID)IoStatusBlock.Information;
    return Status;
}

// IRP_MN_START_DEVICE (0x00)
NTSTATUS
PiIrpStartDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeResourcesAssigned);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_START_DEVICE,
        .Parameters.StartDevice.AllocatedResources = DeviceNode->ResourceList,
        .Parameters.StartDevice.AllocatedResourcesTranslated = DeviceNode->ResourceListTranslated
    };

    DPRINT("PNPTRACE: sending IRP_MN_START_DEVICE to \"%wZ\"\n",
            &DeviceNode->InstancePath);

    if (PiKdHarvestEnabled())
    {
        DPRINT1("PNPHARVEST: start devnode=%p instance=\"%wZ\" service=\"%wZ\" state=%u previous=%u flags=0x%08lx problem=%lu completion=0x%08lx pdo=%p\n",
                DeviceNode,
                &DeviceNode->InstancePath,
                &DeviceNode->ServiceName,
                DeviceNode->State,
                DeviceNode->PreviousState,
                DeviceNode->Flags,
                DeviceNode->Problem,
                DeviceNode->CompletionStatus,
                DeviceNode->PhysicalDeviceObject);
        PiKdHarvestDeviceStack(DeviceNode->PhysicalDeviceObject);
        PiKdHarvestResourceList("raw", DeviceNode->ResourceList);
        PiKdHarvestResourceList("translated", DeviceNode->ResourceListTranslated);
    }

    // Vista+ does an asynchronous call
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    DPRINT("PNPTRACE: IRP_MN_START_DEVICE completed 0x%08lx for \"%wZ\"\n",
            status,
            &DeviceNode->InstancePath);
    DeviceNode->CompletionStatus = status;
    return status;
}

// IRP_MN_STOP_DEVICE (0x04)
NTSTATUS
PiIrpStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeQueryStopped);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_STOP_DEVICE
    };

    // Drivers should never fail a IRP_MN_STOP_DEVICE request
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    ASSERT(NT_SUCCESS(status));
    return status;
}

// IRP_MN_QUERY_STOP_DEVICE (0x05)
NTSTATUS
PiIrpQueryStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStarted);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_STOP_DEVICE
    };

    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    DeviceNode->CompletionStatus = status;
    return status;
}

// IRP_MN_CANCEL_STOP_DEVICE (0x06)
NTSTATUS
PiIrpCancelStopDevice(
    _In_ PDEVICE_NODE DeviceNode)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeQueryStopped);

    PVOID info;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_CANCEL_STOP_DEVICE
    };

    // in fact we don't care which status is returned here
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, &info);
    ASSERT(NT_SUCCESS(status));
    return status;
}

// IRP_MN_QUERY_DEVICE_RELATIONS (0x07)
NTSTATUS
PiIrpQueryDeviceRelations(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ DEVICE_RELATION_TYPE Type)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStarted);

    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS,
        .Parameters.QueryDeviceRelations.Type = Type
    };

    // Vista+ does an asynchronous call
    NTSTATUS status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject,
                                         &stack,
                                         (PVOID)&DeviceNode->OverUsed1.PendingDeviceRelations);
    DeviceNode->CompletionStatus = status;
    return status;
}

// IRP_MN_QUERY_RESOURCES (0x0A)
NTSTATUS
PiIrpQueryResources(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PCM_RESOURCE_LIST *Resources)
{
    PAGED_CODE();

    ASSERT(DeviceNode);

    ULONG_PTR longRes;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_RESOURCES
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longRes);
    if (NT_SUCCESS(status))
    {
        *Resources = (PVOID)longRes;
    }

    return status;
}

// IRP_MN_QUERY_RESOURCE_REQUIREMENTS (0x0B)
NTSTATUS
PiIrpQueryResourceRequirements(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PIO_RESOURCE_REQUIREMENTS_LIST *Resources)
{
    PAGED_CODE();

    ASSERT(DeviceNode);

    ULONG_PTR longRes;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_RESOURCE_REQUIREMENTS
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longRes);
    if (NT_SUCCESS(status))
    {
        *Resources = (PVOID)longRes;
    }

    return status;
}

// IRP_MN_QUERY_DEVICE_TEXT (0x0C)
NTSTATUS
PiIrpQueryDeviceText(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ LCID LocaleId,
    _In_ DEVICE_TEXT_TYPE Type,
    _Out_ PWSTR *DeviceText)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeUninitialized);

    ULONG_PTR longText;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_DEVICE_TEXT,
        .Parameters.QueryDeviceText.DeviceTextType = Type,
        .Parameters.QueryDeviceText.LocaleId = LocaleId
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longText);
    if (NT_SUCCESS(status))
    {
        *DeviceText = (PVOID)longText;
    }

    return status;
}

// IRP_MN_QUERY_PNP_DEVICE_STATE (0x14)
NTSTATUS
PiIrpQueryPnPDeviceState(
    _In_ PDEVICE_NODE DeviceNode,
    _Out_ PPNP_DEVICE_STATE DeviceState)
{
    PAGED_CODE();

    ASSERT(DeviceNode);
    ASSERT(DeviceNode->State == DeviceNodeStartPostWork ||
           DeviceNode->State == DeviceNodeStarted);

    ULONG_PTR longState;
    IO_STACK_LOCATION stack = {
        .MajorFunction = IRP_MJ_PNP,
        .MinorFunction = IRP_MN_QUERY_PNP_DEVICE_STATE
    };

    NTSTATUS status;
    status = IopSynchronousCall(DeviceNode->PhysicalDeviceObject, &stack, (PVOID)&longState);
    if (NT_SUCCESS(status))
    {
        *DeviceState = longState;
    }

    return status;
}
