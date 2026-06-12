/*
 * PROJECT:     ReactOS Storport Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Storport FDO code
 * COPYRIGHT:   Copyright 2017 Eric Kohl (eric.kohl@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

static
BOOLEAN
NTAPI
PortFdoInterruptRoutine(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    PFDO_DEVICE_EXTENSION DeviceExtension;

    DPRINT1("PortFdoInterruptRoutine(%p %p)\n",
            Interrupt, ServiceContext);

    DeviceExtension = (PFDO_DEVICE_EXTENSION)ServiceContext;

    return MiniportHwInterrupt(&DeviceExtension->Miniport);
}


static
NTSTATUS
PortFdoConnectInterrupt(
    _In_ PFDO_DEVICE_EXTENSION DeviceExtension)
{
    ULONG Vector;
    KIRQL Irql;
    KINTERRUPT_MODE InterruptMode;
    BOOLEAN ShareVector;
    KAFFINITY Affinity;
    NTSTATUS Status;

    DPRINT1("PortFdoConnectInterrupt(%p)\n",
            DeviceExtension);

    /* No resources, no interrupt. Done! */
    if (DeviceExtension->AllocatedResources == NULL ||
        DeviceExtension->TranslatedResources == NULL)
    {
        DPRINT1("Checkpoint\n");
        return STATUS_SUCCESS;
    }

    /* Get the interrupt data from the resource list */
    Status = GetResourceListInterrupt(DeviceExtension,
                                      &Vector,
                                      &Irql,
                                      &InterruptMode,
                                      &ShareVector,
                                      &Affinity);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GetResourceListInterrupt() failed (Status 0x%08lx)\n", Status);
        return Status;
    }

    DPRINT1("Vector: %lu\n", Vector);
    DPRINT1("Irql: %lu\n", Irql);

    DPRINT1("Affinity: 0x%08lx\n", Affinity);

    /* Connect the interrupt */
    Status = IoConnectInterrupt(&DeviceExtension->Interrupt,
                                PortFdoInterruptRoutine,
                                DeviceExtension,
                                NULL,
                                Vector,
                                Irql,
                                Irql,
                                InterruptMode,
                                ShareVector,
                                Affinity,
                                FALSE);
    if (NT_SUCCESS(Status))
    {
        DeviceExtension->InterruptIrql = Irql;
    }
    else
    {
        DPRINT1("IoConnectInterrupt() failed (Status 0x%08lx)\n", Status);
    }

    return Status;
}


static
NTSTATUS
PortFdoStartMiniport(
    _In_ PFDO_DEVICE_EXTENSION DeviceExtension)
{
    PHW_INITIALIZATION_DATA InitData;
    INTERFACE_TYPE InterfaceType;
    NTSTATUS Status;

    DPRINT1("PortFdoStartDevice(%p)\n", DeviceExtension);

    /* Get the interface type of the lower device */
    InterfaceType = GetBusInterface(DeviceExtension->LowerDevice);
    if (InterfaceType == InterfaceTypeUndefined)
        return STATUS_NO_SUCH_DEVICE;

    /* Get the driver init data for the given interface type */
    InitData = PortGetDriverInitData(DeviceExtension->DriverExtension,
                                     InterfaceType);
    if (InitData == NULL)
        return STATUS_NO_SUCH_DEVICE;

    /* Initialize the miniport */
    Status = MiniportInitialize(&DeviceExtension->Miniport,
                                DeviceExtension,
                                InitData);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MiniportInitialize() failed (Status 0x%08lx)\n", Status);
        return Status;
    }

    /* Call the miniports FindAdapter function */
    Status = MiniportFindAdapter(&DeviceExtension->Miniport);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MiniportFindAdapter() failed (Status 0x%08lx)\n", Status);
        return Status;
    }

    /* Connect the configured interrupt */
    Status = PortFdoConnectInterrupt(DeviceExtension);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PortFdoConnectInterrupt() failed (Status 0x%08lx)\n", Status);
        return Status;
    }

    /* Call the miniports HwInitialize function */
    Status = MiniportHwInitialize(&DeviceExtension->Miniport);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MiniportHwInitialize() failed (Status 0x%08lx)\n", Status);
        return Status;
    }

    /* Call the HwPassiveInitRoutine function, if available */
    if (DeviceExtension->HwPassiveInitRoutine != NULL)
    {
        DPRINT1("Calling HwPassiveInitRoutine()\n");
        if (!DeviceExtension->HwPassiveInitRoutine(&DeviceExtension->Miniport.MiniportExtension->HwDeviceExtension))
        {
            DPRINT1("HwPassiveInitRoutine() failed\n");
            return STATUS_UNSUCCESSFUL;
        }
    }

    return STATUS_SUCCESS;
}


static
NTSTATUS
NTAPI
PortFdoStartDevice(
    _In_ PFDO_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    DPRINT1("PortFdoStartDevice(%p %p)\n",
            DeviceExtension, Irp);

    ASSERT(DeviceExtension->ExtensionType == FdoExtension);

    /* Get the current stack location */
    Stack = IoGetCurrentIrpStackLocation(Irp);

    /* Start the lower device if the FDO is in 'stopped' state */
    if (DeviceExtension->PnpState == dsStopped)
    {
        if (IoForwardIrpSynchronously(DeviceExtension->LowerDevice, Irp))
        {
            Status = Irp->IoStatus.Status;
        }
        else
        {
            Status = STATUS_UNSUCCESSFUL;
        }

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Lower device failed the IRP (Status 0x%08lx)\n", Status);
            return Status;
        }
    }

    /* Change to the 'started' state */
    DeviceExtension->PnpState = dsStarted;

    /* Copy the raw and translated resource lists into the device extension */
    if (Stack->Parameters.StartDevice.AllocatedResources != NULL &&
        Stack->Parameters.StartDevice.AllocatedResourcesTranslated != NULL)
    {
        DeviceExtension->AllocatedResources = CopyResourceList(NonPagedPool,
                                                               Stack->Parameters.StartDevice.AllocatedResources);
        if (DeviceExtension->AllocatedResources == NULL)
            return STATUS_NO_MEMORY;

        DeviceExtension->TranslatedResources = CopyResourceList(NonPagedPool,
                                                                Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
        if (DeviceExtension->TranslatedResources == NULL)
            return STATUS_NO_MEMORY;
    }

    /* Get the bus interface of the lower (bus) device */
    Status = QueryBusInterface(DeviceExtension->LowerDevice,
                               (PGUID)&GUID_BUS_INTERFACE_STANDARD,
                               sizeof(BUS_INTERFACE_STANDARD),
                               1,
                               &DeviceExtension->BusInterface,
                               NULL);
    DPRINT1("Status: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        DPRINT1("Context: %p\n", DeviceExtension->BusInterface.Context);
        DeviceExtension->BusInitialized = TRUE;
    }

    /* Start the miniport (FindAdapter & Initialize) */
    Status = PortFdoStartMiniport(DeviceExtension);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("FdoStartMiniport() failed (Status 0x%08lx)\n", Status);
        DeviceExtension->PnpState = dsStopped;
    }

    return Status;
}


static
NTSTATUS
PortSendInquiry(
    _In_ PPDO_DEVICE_EXTENSION PdoExtension)
{
    IO_STATUS_BLOCK IoStatusBlock;
    PIO_STACK_LOCATION IrpStack;
    KEVENT Event;
//    KIRQL Irql;
    PIRP Irp;
    NTSTATUS Status;
    PSENSE_DATA SenseBuffer;
    BOOLEAN KeepTrying = TRUE;
    ULONG RetryCount = 0;
    SCSI_REQUEST_BLOCK Srb;
    PCDB Cdb;
//    PSCSI_PORT_LUN_EXTENSION LunExtension;
//    PFDO_DEVICE_EXTENSION DeviceExtension;

    DPRINT("PortSendInquiry(%p)\n", PdoExtension);

    if (PdoExtension->InquiryBuffer == NULL)
    {
        PdoExtension->InquiryBuffer = ExAllocatePoolWithTag(NonPagedPool, INQUIRYDATABUFFERSIZE, TAG_INQUIRY_DATA);
        if (PdoExtension->InquiryBuffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    SenseBuffer = ExAllocatePoolWithTag(NonPagedPool, SENSE_BUFFER_SIZE, TAG_SENSE_DATA);
    if (SenseBuffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (KeepTrying)
    {
        /* Initialize event for waiting */
        KeInitializeEvent(&Event,
                          NotificationEvent,
                          FALSE);

        /* Create an IRP */
        Irp = IoBuildDeviceIoControlRequest(IOCTL_SCSI_EXECUTE_IN,
                                            PdoExtension->Device,
                                            NULL,
                                            0,
                                            PdoExtension->InquiryBuffer,
                                            INQUIRYDATABUFFERSIZE,
                                            TRUE,
                                            &Event,
                                            &IoStatusBlock);
        if (Irp == NULL)
        {
            DPRINT("IoBuildDeviceIoControlRequest() failed\n");

            /* Quit the loop */
            Status = STATUS_INSUFFICIENT_RESOURCES;
            KeepTrying = FALSE;
            continue;
        }

        /* Prepare SRB */
        RtlZeroMemory(&Srb, sizeof(SCSI_REQUEST_BLOCK));

        Srb.Length = sizeof(SCSI_REQUEST_BLOCK);
        Srb.OriginalRequest = Irp;
        Srb.PathId = PdoExtension->Bus;
        Srb.TargetId = PdoExtension->Target;
        Srb.Lun = PdoExtension->Lun;
        Srb.Function = SRB_FUNCTION_EXECUTE_SCSI;
        Srb.SrbFlags = SRB_FLAGS_DATA_IN | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
        Srb.TimeOutValue = 4;
        Srb.CdbLength = 6;

        Srb.SenseInfoBuffer = SenseBuffer;
        Srb.SenseInfoBufferLength = SENSE_BUFFER_SIZE;

        Srb.DataBuffer = PdoExtension->InquiryBuffer;
        Srb.DataTransferLength = INQUIRYDATABUFFERSIZE;

        /* Attach Srb to the Irp */
        IrpStack = IoGetNextIrpStackLocation(Irp);
        IrpStack->Parameters.Scsi.Srb = &Srb;

        /* Fill in CDB */
        Cdb = (PCDB)Srb.Cdb;
        Cdb->CDB6INQUIRY.OperationCode = SCSIOP_INQUIRY;
        Cdb->CDB6INQUIRY.LogicalUnitNumber = PdoExtension->Lun;
        Cdb->CDB6INQUIRY.AllocationLength = INQUIRYDATABUFFERSIZE;

        /* Call the driver */
        Status = IoCallDriver(PdoExtension->Device, Irp);

        /* Wait for it to complete */
        if (Status == STATUS_PENDING)
        {
            DPRINT1("PortSendInquiry(): Waiting for the driver to process request...\n");
            KeWaitForSingleObject(&Event,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  NULL);
            Status = IoStatusBlock.Status;
        }

        DPRINT("PortSendInquiry(): Request processed by driver, status = 0x%08X\n", Status);

        if (SRB_STATUS(Srb.SrbStatus) == SRB_STATUS_SUCCESS)
        {
            DPRINT("Found a device!\n");

            /* Quit the loop */
            Status = STATUS_SUCCESS;
            KeepTrying = FALSE;
            continue;
        }

        DPRINT("Inquiry SRB failed with SrbStatus 0x%08X\n", Srb.SrbStatus);

        /* Check if the queue is frozen */
        if (Srb.SrbStatus & SRB_STATUS_QUEUE_FROZEN)
        {
            /* Something weird happened, deal with it (unfreeze the queue) */
            KeepTrying = FALSE;

            DPRINT("SpiSendInquiry(): the queue is frozen at TargetId %d\n", Srb.TargetId);

//            LunExtension = SpiGetLunExtension(DeviceExtension,
//                                              LunInfo->PathId,
//                                              LunInfo->TargetId,
//                                              LunInfo->Lun);

            /* Clear frozen flag */
//            LunExtension->Flags &= ~LUNEX_FROZEN_QUEUE;

            /* Acquire the spinlock */
//            KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);

            /* Process the request */
//            SpiGetNextRequestFromLun(DeviceObject->DeviceExtension, LunExtension);

            /* SpiGetNextRequestFromLun() releases the spinlock,
                so we just lower irql back to what it was before */
//            KeLowerIrql(Irql);
        }

        /* Check if data overrun happened */
        if (SRB_STATUS(Srb.SrbStatus) == SRB_STATUS_DATA_OVERRUN)
        {
            DPRINT("Data overrun at TargetId %d\n", PdoExtension->Target);

            /* Quit the loop */
            Status = STATUS_SUCCESS;
            KeepTrying = FALSE;
        }
        else if ((Srb.SrbStatus & SRB_STATUS_AUTOSENSE_VALID) &&
                 SenseBuffer->SenseKey == SCSI_SENSE_ILLEGAL_REQUEST)
        {
            /* LUN is not valid, but some device responds there.
                Mark it as invalid anyway */

            /* Quit the loop */
            Status = STATUS_INVALID_DEVICE_REQUEST;
            KeepTrying = FALSE;
        }
        else
        {
            /* Retry a couple of times if no timeout happened */
            if ((RetryCount < 2) &&
                (SRB_STATUS(Srb.SrbStatus) != SRB_STATUS_NO_DEVICE) &&
                (SRB_STATUS(Srb.SrbStatus) != SRB_STATUS_SELECTION_TIMEOUT))
            {
                RetryCount++;
                KeepTrying = TRUE;
            }
            else
            {
                /* That's all, quit the loop */
                KeepTrying = FALSE;

                /* Set status according to SRB status */
                if (SRB_STATUS(Srb.SrbStatus) == SRB_STATUS_BAD_FUNCTION ||
                    SRB_STATUS(Srb.SrbStatus) == SRB_STATUS_BAD_SRB_BLOCK_LENGTH)
                {
                    Status = STATUS_INVALID_DEVICE_REQUEST;
                }
                else
                {
                    Status = STATUS_IO_DEVICE_ERROR;
                }
            }
        }
    }

    /* Free the sense buffer */
    ExFreePoolWithTag(SenseBuffer, TAG_SENSE_DATA);

    DPRINT("PortSendInquiry() done with Status 0x%08X\n", Status);

    return Status;
}



static
NTSTATUS
PortFdoScanBus(
    _In_ PFDO_DEVICE_EXTENSION DeviceExtension)
{
    PPDO_DEVICE_EXTENSION PdoExtension;
    ULONG Bus, Target; //, Lun;
    NTSTATUS Status;

    DPRINT("PortFdoScanBus(%p)\n", DeviceExtension);

    DPRINT("NumberOfBuses: %lu\n", DeviceExtension->Miniport.PortConfig.NumberOfBuses);
    DPRINT("MaximumNumberOfTargets: %lu\n", DeviceExtension->Miniport.PortConfig.MaximumNumberOfTargets);
    DPRINT("MaximumNumberOfLogicalUnits: %lu\n", DeviceExtension->Miniport.PortConfig.MaximumNumberOfLogicalUnits);

    /* Scan all buses */
    for (Bus = 0; Bus < DeviceExtension->Miniport.PortConfig.NumberOfBuses; Bus++)
    {
        DPRINT("Scanning bus %ld\n", Bus);

        /* Scan all targets */
        for (Target = 0; Target < DeviceExtension->Miniport.PortConfig.MaximumNumberOfTargets; Target++)
        {
            DPRINT("  Scanning target %ld:%ld\n", Bus, Target);

            DPRINT("    Scanning logical unit %ld:%ld:%ld\n", Bus, Target, 0);
            Status = PortCreatePdo(DeviceExtension, Bus, Target, 0, &PdoExtension);
            if (NT_SUCCESS(Status))
            {
                /* Scan LUN 0 */
                Status = PortSendInquiry(PdoExtension);
                DPRINT("PortSendInquiry returned 0x%08lx\n", Status);
                if (!NT_SUCCESS(Status))
                {
                    PortDeletePdo(PdoExtension);
                }
                else
                {
                    DPRINT("VendorId: %.8s\n", PdoExtension->InquiryBuffer->VendorId);
                    DPRINT("ProductId: %.16s\n", PdoExtension->InquiryBuffer->ProductId);
                    DPRINT("ProductRevisionLevel: %.4s\n", PdoExtension->InquiryBuffer->ProductRevisionLevel);
                    DPRINT("VendorSpecific: %.20s\n", PdoExtension->InquiryBuffer->VendorSpecific);
                }
            }

#if 0
            /* Scan all logical units */
            for (Lun = 1; Lun < DeviceExtension->Miniport.PortConfig.MaximumNumberOfLogicalUnits; Lun++)
            {
                DPRINT("    Scanning logical unit %ld:%ld:%ld\n", Bus, Target, Lun);
                Status = PortSendInquiry(DeviceExtension->Device, Bus, Target, Lun);
                DPRINT("PortSendInquiry returned 0x%08lx\n", Status);
                if (!NT_SUCCESS(Status))
                    break;
            }
#endif
        }
    }

    DPRINT("PortFdoScanBus() done!\n");

    return STATUS_SUCCESS;
}


static
NTSTATUS
PortFdoQueryBusRelations(
    _In_ PFDO_DEVICE_EXTENSION DeviceExtension,
    _Out_ PULONG_PTR Information)
{
    PDEVICE_RELATIONS Relations;
    PPDO_DEVICE_EXTENSION PdoExt;
    PLIST_ENTRY Entry;
    KLOCK_QUEUE_HANDLE LockHandle;
    ULONG Count, Index;
    SIZE_T Size;
    NTSTATUS Status;

    DPRINT1("PortFdoQueryBusRelations(%p %p)\n",
            DeviceExtension, Information);

    /* Probe for LUs on first call. PdoCount==0 means we haven't scanned yet
     * (BusInitialized doesn't quite capture this since the FDO can be started
     * before children get enumerated). For now: scan when empty. */
    if (DeviceExtension->PdoCount == 0)
    {
        Status = PortFdoScanBus(DeviceExtension);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    DPRINT1("Units found: %lu\n", DeviceExtension->PdoCount);

    Count = DeviceExtension->PdoCount;
    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects[Count]);
    if (Count == 0)
        Size = sizeof(DEVICE_RELATIONS);

    Relations = ExAllocatePoolWithTag(PagedPool, Size, TAG_GLOBAL_DATA);
    if (Relations == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Relations, Size);

    KeAcquireInStackQueuedSpinLock(&DeviceExtension->PdoListLock, &LockHandle);

    Index = 0;
    for (Entry = DeviceExtension->PdoListHead.Flink;
         Entry != &DeviceExtension->PdoListHead && Index < Count;
         Entry = Entry->Flink)
    {
        PdoExt = CONTAINING_RECORD(Entry, PDO_DEVICE_EXTENSION, PdoListEntry);
        Relations->Objects[Index] = PdoExt->Device;
        ObReferenceObject(PdoExt->Device);
        Index++;
    }
    Relations->Count = Index;

    KeReleaseInStackQueuedSpinLock(&LockHandle);

    *Information = (ULONG_PTR)Relations;
    return STATUS_SUCCESS;
}


static
NTSTATUS
PortFdoFilterRequirements(
    PFDO_DEVICE_EXTENSION DeviceExtension,
    PIRP Irp)
{
    PIO_RESOURCE_REQUIREMENTS_LIST RequirementsList;

    DPRINT1("PortFdoFilterRequirements(%p %p)\n", DeviceExtension, Irp);

    /* Get the bus number and the slot number */
    RequirementsList =(PIO_RESOURCE_REQUIREMENTS_LIST)Irp->IoStatus.Information;
    if (RequirementsList != NULL)
    {
        DeviceExtension->BusNumber = RequirementsList->BusNumber;
        DeviceExtension->SlotNumber = RequirementsList->SlotNumber;
    }

    return STATUS_SUCCESS;
}


NTSTATUS
NTAPI
PortFdoScsi(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PFDO_DEVICE_EXTENSION DeviceExtension;
    ULONG_PTR Information = 0;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    DPRINT("PortFdoScsi(%p %p)\n", DeviceObject, Irp);

    DeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(DeviceExtension);
    ASSERT(DeviceExtension->ExtensionType == FdoExtension);

    /* SCSI IRPs are normally sent to PDOs (LUs), not the FDO. Anything that
     * reaches us here is malformed; reject without claiming success. */
    Irp->IoStatus.Information = Information;
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}


/*
 * Translate an SRB_STATUS into an NTSTATUS for the I/O manager.
 */
static
NTSTATUS
SrbStatusToNtStatus(
    _In_ UCHAR SrbStatus)
{
    switch (SRB_STATUS(SrbStatus))
    {
        case SRB_STATUS_SUCCESS:
        case SRB_STATUS_DATA_OVERRUN:
            return STATUS_SUCCESS;

        case SRB_STATUS_PENDING:
            return STATUS_PENDING;

        case SRB_STATUS_INVALID_LUN:
        case SRB_STATUS_INVALID_TARGET_ID:
        case SRB_STATUS_NO_DEVICE:
        case SRB_STATUS_NO_HBA:
            return STATUS_DEVICE_DOES_NOT_EXIST;

        case SRB_STATUS_SELECTION_TIMEOUT:
        case SRB_STATUS_TIMEOUT:
        case SRB_STATUS_COMMAND_TIMEOUT:
            return STATUS_IO_TIMEOUT;

        case SRB_STATUS_INVALID_REQUEST:
        case SRB_STATUS_INVALID_PATH_ID:
        case SRB_STATUS_BAD_FUNCTION:
        case SRB_STATUS_BAD_SRB_BLOCK_LENGTH:
            return STATUS_INVALID_PARAMETER;

        case SRB_STATUS_ABORTED:
        case SRB_STATUS_ABORT_FAILED:
            return STATUS_REQUEST_ABORTED;

        case SRB_STATUS_BUSY:
            return STATUS_DEVICE_BUSY;

        default:
            return STATUS_IO_DEVICE_ERROR;
    }
}


/*
 * Build a single-list scatter/gather table for the SRB's data buffer.
 * Works for both DO_DIRECT_IO IRPs (Irp->MdlAddress non-NULL) and
 * port-internal kernel buffers (no MDL, contiguous virtual address only).
 *
 * With an MDL the physical pages come from the MDL's PFN array, never from
 * virtual address translation: paging-read MDLs built straight from PFNs
 * (MmBuildMdlFromPages) carry StartVa == NULL, so Srb->DataBuffer is only
 * meaningful as an offset relative to MmGetMdlVirtualAddress(Mdl).
 */
static
NTSTATUS
PortBuildScatterGatherList(
    _Inout_ PSRB_PORT_CONTEXT Context,
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR Va;
    ULONG BytesRemaining;
    ULONG Count = 0;
    PHYSICAL_ADDRESS Phys;
    ULONG OffsetInPage;
    ULONG BytesThisPage;
    PMDL Mdl = Irp->MdlAddress;

    Context->Sgl.NumberOfElements = 0;
    Context->Sgl.Reserved = 0;

    if (Srb->DataTransferLength == 0)
        return STATUS_SUCCESS;

    BytesRemaining = Srb->DataTransferLength;

    if (Mdl != NULL)
    {
        PPFN_NUMBER PfnArray = MmGetMdlPfnArray(Mdl);
        ULONG_PTR MdlOffset;

        /* Byte position of the transfer start within the MDL's pages */
        MdlOffset = ((ULONG_PTR)Srb->DataBuffer -
                     (ULONG_PTR)MmGetMdlVirtualAddress(Mdl)) +
                    Mdl->ByteOffset;

        if (MdlOffset + BytesRemaining >
            (ULONG_PTR)Mdl->ByteOffset + Mdl->ByteCount)
        {
            DPRINT1("PortBuildScatterGatherList: transfer exceeds MDL (offset %Iu length %lu MDL %lu+%lu)\n",
                    MdlOffset, BytesRemaining, Mdl->ByteOffset, Mdl->ByteCount);
            return STATUS_INVALID_PARAMETER;
        }

        while (BytesRemaining > 0)
        {
            OffsetInPage = (ULONG)(MdlOffset & (PAGE_SIZE - 1));
            Phys.QuadPart =
                ((ULONGLONG)PfnArray[MdlOffset >> PAGE_SHIFT] << PAGE_SHIFT) +
                OffsetInPage;
            BytesThisPage = PAGE_SIZE - OffsetInPage;
            if (BytesThisPage > BytesRemaining)
                BytesThisPage = BytesRemaining;

            /* Coalesce with the previous element if physically contiguous. */
            if (Count > 0 &&
                Context->Sgl.List[Count - 1].PhysicalAddress.QuadPart +
                    Context->Sgl.List[Count - 1].Length == Phys.QuadPart)
            {
                Context->Sgl.List[Count - 1].Length += BytesThisPage;
            }
            else
            {
                if (Count >= STORPORT_MAX_SGL_ENTRIES)
                    return STATUS_INSUFFICIENT_RESOURCES;

                Context->Sgl.List[Count].PhysicalAddress = Phys;
                Context->Sgl.List[Count].Length = BytesThisPage;
                Count++;
            }

            MdlOffset += BytesThisPage;
            BytesRemaining -= BytesThisPage;
        }

        Context->Sgl.NumberOfElements = Count;
        return STATUS_SUCCESS;
    }

    if (Srb->DataBuffer == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Kernel-mode buffer in non-paged pool. Trust DataBuffer as VA. */
    Va = (PUCHAR)Srb->DataBuffer;

    while (BytesRemaining > 0)
    {
        Phys = MmGetPhysicalAddress(Va);
        OffsetInPage = (ULONG)((ULONG_PTR)Va & (PAGE_SIZE - 1));
        BytesThisPage = PAGE_SIZE - OffsetInPage;
        if (BytesThisPage > BytesRemaining)
            BytesThisPage = BytesRemaining;

        /* Coalesce with the previous element if physically contiguous. */
        if (Count > 0 &&
            Context->Sgl.List[Count - 1].PhysicalAddress.QuadPart +
                Context->Sgl.List[Count - 1].Length == Phys.QuadPart)
        {
            Context->Sgl.List[Count - 1].Length += BytesThisPage;
        }
        else
        {
            if (Count >= STORPORT_MAX_SGL_ENTRIES)
                return STATUS_INSUFFICIENT_RESOURCES;

            Context->Sgl.List[Count].PhysicalAddress = Phys;
            Context->Sgl.List[Count].Length = BytesThisPage;
            Count++;
        }

        Va += BytesThisPage;
        BytesRemaining -= BytesThisPage;
    }

    Context->Sgl.NumberOfElements = Count;
    return STATUS_SUCCESS;
}


/*
 * Build a port context for an SRB and hand the SRB to the miniport's HwStartIo.
 * The IRP is left pending; the miniport completes it later via
 * StorPortNotification(RequestComplete, ...).
 *
 * Returns STATUS_PENDING on the happy path, or a failure status if we can't
 * even reach HwStartIo (in which case the caller must complete the IRP).
 */
NTSTATUS
PortDispatchSrb(
    _In_ PPDO_DEVICE_EXTENSION PdoExtension,
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PFDO_DEVICE_EXTENSION FdoExtension = PdoExtension->FdoExtension;
    PSRB_PORT_CONTEXT Context;
    PVOID RawAlloc;
    SIZE_T ContextSize;
    SIZE_T RawAllocSize;
    ULONG MiniportExtSize;
    NTSTATUS Status;
    BOOLEAN Result;

    MiniportExtSize = FdoExtension->Miniport.InitData->SrbExtensionSize;
    ContextSize = FIELD_OFFSET(SRB_PORT_CONTEXT, MiniportExtension) + MiniportExtSize;

    /* Over-allocate by (alignment - 1) so we can shift the struct base to a
     * 128-byte boundary. AHCI command tables placed at MiniportExtension
     * require 128-byte alignment; ExAllocatePoolWithTag only gives 16. */
    RawAllocSize = ContextSize + SRB_EXTENSION_ALIGNMENT - 1;
    RawAlloc = ExAllocatePoolWithTag(NonPagedPool, RawAllocSize, TAG_SRB_CONTEXT);
    if (RawAlloc == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Context = (PSRB_PORT_CONTEXT)
        (((ULONG_PTR)RawAlloc + SRB_EXTENSION_ALIGNMENT - 1) &
         ~((ULONG_PTR)SRB_EXTENSION_ALIGNMENT - 1));

    RtlZeroMemory(Context, ContextSize);
    Context->RawAlloc = RawAlloc;
    Context->Magic = SRB_CONTEXT_MAGIC;

    /* Sanity check: MiniportExtension must be 128-byte aligned (see
     * SRB_PORT_CONTEXT comments). DECLSPEC_ALIGN on the field handles the
     * intra-struct offset; the over-allocate+align above handles the base. */
    ASSERT(((ULONG_PTR)Context->MiniportExtension & (SRB_EXTENSION_ALIGNMENT - 1)) == 0);
    Context->Irp = Irp;
    Context->Srb = Srb;
    Context->FdoExtension = FdoExtension;
    Context->PdoExtension = PdoExtension;

    /* Force the SRB's path/target/lun to match the PDO regardless of what the
     * caller filled in. Class drivers route via the PDO, not by triple. */
    Srb->PathId = (UCHAR)PdoExtension->Bus;
    Srb->TargetId = (UCHAR)PdoExtension->Target;
    Srb->Lun = (UCHAR)PdoExtension->Lun;

    /* The miniport sees an opaque blob of SrbExtensionSize bytes. */
    Srb->SrbExtension = (MiniportExtSize > 0) ? (PVOID)Context->MiniportExtension : NULL;

    /* OriginalRequest is the canonical link back to the IRP. We mirror it in
     * the context so the completion path doesn't have to trust caller-set
     * SRB fields. */
    Srb->OriginalRequest = Irp;

    /* Pre-set SrbStatus so the miniport's HwStartIo sees a fresh state. */
    Srb->SrbStatus = SRB_STATUS_PENDING;

    Status = PortBuildScatterGatherList(Context, Irp, Srb);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PortBuildScatterGatherList() failed (Status 0x%08lx)\n", Status);
        ExFreePoolWithTag(Context->RawAlloc, TAG_SRB_CONTEXT);
        return Status;
    }

    IoMarkIrpPending(Irp);

    Result = MiniportStartIo(&FdoExtension->Miniport, Srb);
    if (!Result)
    {
        DPRINT1("HwStartIo returned FALSE; completing SRB inline\n");

        /* The miniport refused the SRB outright. It is responsible for setting
         * SrbStatus before returning FALSE; if it didn't, treat as a generic
         * failure. Either way, we own completion. */
        if (Srb->SrbStatus == SRB_STATUS_PENDING)
            Srb->SrbStatus = SRB_STATUS_ERROR;

        PortCompleteSrb(Context);
    }

    return STATUS_PENDING;
}


/*
 * Complete the SRB initiated via PortDispatchSrb. Called either inline from
 * PortDispatchSrb when HwStartIo refuses the request, or from
 * StorPortNotification(RequestComplete, ...) once the miniport finishes.
 */
static
VOID
NTAPI
PortCompleteSrbDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* Replay the completion at DISPATCH_LEVEL where ExFreePool and
     * IoCompleteRequest are both legal. */
    PortCompleteSrb((PSRB_PORT_CONTEXT)DeferredContext);
}

VOID
PortCompleteSrb(
    _In_ PSRB_PORT_CONTEXT Context)
{
    PIRP Irp;
    PSCSI_REQUEST_BLOCK Srb;
    PVOID RawAlloc;
    NTSTATUS Status;

    ASSERT(Context->Magic == SRB_CONTEXT_MAGIC);

    /* If we're above DISPATCH_LEVEL the miniport called RequestComplete from
     * an interrupt-locked path. Free + IoCompleteRequest must run at <=
     * DISPATCH, so defer via a per-SRB DPC and bounce back here. */
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        KeInitializeDpc(&Context->CompleteDpc, PortCompleteSrbDpc, Context);
        KeInsertQueueDpc(&Context->CompleteDpc, NULL, NULL);
        return;
    }

    Irp = Context->Irp;
    Srb = Context->Srb;
    RawAlloc = Context->RawAlloc;

    Status = SrbStatusToNtStatus(Srb->SrbStatus);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Srb->DataTransferLength : 0;

    /* Detach the SRB extension so a stale pointer can't reach our pool. */
    Srb->SrbExtension = NULL;
    Context->Magic = 0;

    /* Context points at an aligned offset inside RawAlloc; free RawAlloc. */
    ExFreePoolWithTag(RawAlloc, TAG_SRB_CONTEXT);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}


NTSTATUS
NTAPI
PortFdoPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PFDO_DEVICE_EXTENSION DeviceExtension;
    PIO_STACK_LOCATION Stack;
    ULONG_PTR Information = 0;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    DPRINT1("PortFdoPnp(%p %p)\n",
            DeviceObject, Irp);

    DeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(DeviceExtension);
    ASSERT(DeviceExtension->ExtensionType == FdoExtension);

    Stack = IoGetCurrentIrpStackLocation(Irp);

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE: /* 0x00 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_START_DEVICE\n");
            Status = PortFdoStartDevice(DeviceExtension, Irp);
            break;

        case IRP_MN_QUERY_REMOVE_DEVICE: /* 0x01 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_QUERY_REMOVE_DEVICE\n");
            break;

        case IRP_MN_REMOVE_DEVICE: /* 0x02 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_REMOVE_DEVICE\n");
            break;

        case IRP_MN_CANCEL_REMOVE_DEVICE: /* 0x03 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_CANCEL_REMOVE_DEVICE\n");
            break;

        case IRP_MN_STOP_DEVICE: /* 0x04 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_STOP_DEVICE\n");
            break;

        case IRP_MN_QUERY_STOP_DEVICE: /* 0x05 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_QUERY_STOP_DEVICE\n");
            break;

        case IRP_MN_CANCEL_STOP_DEVICE: /* 0x06 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_CANCEL_STOP_DEVICE\n");
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS: /* 0x07 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_RELATIONS\n");
            switch (Stack->Parameters.QueryDeviceRelations.Type)
            {
                case BusRelations:
                    DPRINT1("    IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_RELATIONS / BusRelations\n");
                    Status = PortFdoQueryBusRelations(DeviceExtension, &Information);
                    break;

                case RemovalRelations:
                    DPRINT1("    IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_RELATIONS / RemovalRelations\n");
                    return ForwardIrpAndForget(DeviceExtension->LowerDevice, Irp);

                default:
                    DPRINT1("    IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_RELATIONS / Unknown type 0x%lx\n",
                            Stack->Parameters.QueryDeviceRelations.Type);
                    return ForwardIrpAndForget(DeviceExtension->LowerDevice, Irp);
            }
            break;

        case IRP_MN_FILTER_RESOURCE_REQUIREMENTS: /* 0x0d */
            DPRINT1("IRP_MJ_PNP / IRP_MN_FILTER_RESOURCE_REQUIREMENTS\n");
            PortFdoFilterRequirements(DeviceExtension, Irp);
            return ForwardIrpAndForget(DeviceExtension->LowerDevice, Irp);

        case IRP_MN_QUERY_PNP_DEVICE_STATE: /* 0x14 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_QUERY_PNP_DEVICE_STATE\n");
            break;

        case IRP_MN_DEVICE_USAGE_NOTIFICATION: /* 0x16 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_DEVICE_USAGE_NOTIFICATION\n");
            break;

        case IRP_MN_SURPRISE_REMOVAL: /* 0x17 */
            DPRINT1("IRP_MJ_PNP / IRP_MN_SURPRISE_REMOVAL\n");
            break;

        default:
            DPRINT1("IRP_MJ_PNP / Unknown IOCTL 0x%lx\n", Stack->MinorFunction);
            return ForwardIrpAndForget(DeviceExtension->LowerDevice, Irp);
    }

    Irp->IoStatus.Information = Information;
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}

/* EOF */
