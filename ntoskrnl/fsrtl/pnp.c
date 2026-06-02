/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/fsrtl/pnp.c
 * PURPOSE:         Manages PnP support routines for file system drivers.
 * PROGRAMMERS:     Pierre Schweitzer
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#include <ioevent.h>
#include <ntddstor.h>
#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS **********************************************************/

/*++
 * @name FsRtlNotifyVolumeEvent
 * @implemented
 *
 * Notifies system (and applications) that something changed on volume.
 * FSD should call it each time volume status changes.
 *
 * @param FileObject
 *        FileObject for the volume
 *
 * @param EventCode
 *        Event that occurs one the volume
 *
 * @return STATUS_SUCCESS if notification went well
 *
 * @remarks Only present in NT 5+.
 *
 *--*/
NTSTATUS
NTAPI
FsRtlNotifyVolumeEvent(IN PFILE_OBJECT FileObject,
                       IN ULONG EventCode)
{
    NTSTATUS Status;
    LPGUID Guid = NULL;
    PDEVICE_OBJECT DeviceObject = NULL;
    TARGET_DEVICE_CUSTOM_NOTIFICATION Notification;

    Status = IoGetRelatedTargetDevice(FileObject, &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = STATUS_INVALID_PARAMETER;

    Notification.Version = 1;
    Notification.Size = sizeof(TARGET_DEVICE_CUSTOM_NOTIFICATION);
    /* MSDN says that FileObject must be null
       when calling IoReportTargetDeviceChangeAsynchronous */
    Notification.FileObject = NULL;
    Notification.NameBufferOffset = -1;
    /* Find the good GUID associated with the event */
    switch (EventCode)
    {
        case FSRTL_VOLUME_DISMOUNT:
        {
            Guid = (LPGUID)&GUID_IO_VOLUME_DISMOUNT;
            break;
        }
        case FSRTL_VOLUME_DISMOUNT_FAILED:
        {
            Guid = (LPGUID)&GUID_IO_VOLUME_DISMOUNT_FAILED;
            break;
        }
        case FSRTL_VOLUME_LOCK:
        {
            Guid = (LPGUID)&GUID_IO_VOLUME_LOCK;
            break;
        }
        case FSRTL_VOLUME_LOCK_FAILED:
        {
            Guid = (LPGUID)&GUID_IO_VOLUME_LOCK_FAILED;
            break;
        }
        case FSRTL_VOLUME_MOUNT:
        {
            Guid = (LPGUID)&GUID_IO_VOLUME_MOUNT;
            break;
        }
        case FSRTL_VOLUME_UNLOCK:
        {
            Guid = (LPGUID)&GUID_IO_VOLUME_UNLOCK;
            break;
        }
    }
    if (Guid)
    {
        /* Copy GUID to notification structure and then report the change */
        RtlCopyMemory(&(Notification.Event), Guid, sizeof(GUID));

        if (EventCode == FSRTL_VOLUME_MOUNT)
        {
            IoReportTargetDeviceChangeAsynchronous(DeviceObject,
                                                   &Notification,
                                                   NULL,
                                                   NULL);
        }
        else
        {
            IoReportTargetDeviceChange(DeviceObject,
                                       &Notification);
        }

        Status = STATUS_SUCCESS;
    }
    ObDereferenceObject(DeviceObject);

    return Status;
}

/*
 * @unimplemented
 *
 * Filter-manager hint that a volume dismount has finished. Filter manager
 * uses this to release per-volume filter state; without it the worst-case
 * effect is a small one-shot leak across the dismount path.
 */
VOID
NTAPI
FsRtlDismountComplete(IN PDEVICE_OBJECT DeviceObject,
                      IN NTSTATUS DismountStatus)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DismountStatus);
}

#if (NTDDI_VERSION >= NTDDI_WIN8)
/*
 * @implemented
 *
 * Returns sector-size geometry for a volume's underlying device by issuing
 * IOCTL_STORAGE_QUERY_PROPERTY/StorageAccessAlignmentProperty against the
 * DeviceObject. If the device honours the IOCTL, the descriptor is mapped
 * directly into FILE_FS_SECTOR_SIZE_INFORMATION; if it doesn't (older
 * device, virtual disk that only knows DeviceObject->SectorSize), we fall
 * back to the device's logical sector size for both logical and physical,
 * which is the same answer Microsoft returns for legacy devices.
 */
NTSTATUS
NTAPI
FsRtlGetSectorSizeInformation(IN PDEVICE_OBJECT DeviceObject,
                              OUT PFILE_FS_SECTOR_SIZE_INFORMATION SectorSizeInfo)
{
    STORAGE_PROPERTY_QUERY Query;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR Descriptor;
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;
    ULONG SectorSize;

    if (SectorSizeInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Sensible legacy defaults - overwritten if the IOCTL succeeds. */
    SectorSize = (DeviceObject->SectorSize != 0) ? DeviceObject->SectorSize : 512;
    SectorSizeInfo->LogicalBytesPerSector = SectorSize;
    SectorSizeInfo->PhysicalBytesPerSectorForAtomicity = SectorSize;
    SectorSizeInfo->PhysicalBytesPerSectorForPerformance = SectorSize;
    SectorSizeInfo->FileSystemEffectivePhysicalBytesPerSectorForAtomicity = SectorSize;
    SectorSizeInfo->Flags = 0;
    SectorSizeInfo->ByteOffsetForSectorAlignment = SSINFO_OFFSET_UNKNOWN;
    SectorSizeInfo->ByteOffsetForPartitionAlignment = SSINFO_OFFSET_UNKNOWN;

    RtlZeroMemory(&Query, sizeof(Query));
    Query.PropertyId = StorageAccessAlignmentProperty;
    Query.QueryType = PropertyStandardQuery;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IOCTL_STORAGE_QUERY_PROPERTY,
                                        DeviceObject,
                                        &Query,
                                        sizeof(Query),
                                        &Descriptor,
                                        sizeof(Descriptor),
                                        FALSE,
                                        &Event,
                                        &Iosb);
    if (Irp == NULL)
    {
        /* Out of memory building the IRP - defaults are already filled in. */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    if (NT_SUCCESS(Status) && Iosb.Information >= sizeof(Descriptor))
    {
        SectorSizeInfo->LogicalBytesPerSector = Descriptor.BytesPerLogicalSector;
        SectorSizeInfo->PhysicalBytesPerSectorForAtomicity = Descriptor.BytesPerPhysicalSector;
        SectorSizeInfo->PhysicalBytesPerSectorForPerformance = Descriptor.BytesPerPhysicalSector;
        SectorSizeInfo->FileSystemEffectivePhysicalBytesPerSectorForAtomicity = Descriptor.BytesPerPhysicalSector;
        SectorSizeInfo->ByteOffsetForSectorAlignment = Descriptor.BytesOffsetForSectorAlignment;
        if (Descriptor.BytesOffsetForSectorAlignment == 0)
        {
            SectorSizeInfo->Flags |= SSINFO_FLAGS_ALIGNED_DEVICE;
            SectorSizeInfo->Flags |= SSINFO_FLAGS_PARTITION_ALIGNED_ON_DEVICE;
        }
    }

    return STATUS_SUCCESS;
}
#else
NTSTATUS
NTAPI
FsRtlGetSectorSizeInformation(IN PDEVICE_OBJECT DeviceObject,
                              OUT PVOID SectorSizeInfo)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(SectorSizeInfo);
    return STATUS_NOT_IMPLEMENTED;
}
#endif

/*
 * @unimplemented
 *
 * Per-process disk-byte accounting hook used by FAT/CDFS at NTDDI_WIN8+.
 * The full implementation tags the current process's accounting block;
 * the stub is a no-op until the EPROCESS counters are wired up.
 */
VOID
NTAPI
FsRtlUpdateDiskCounters(IN ULONGLONG BytesRead,
                        IN ULONGLONG BytesWritten)
{
    UNREFERENCED_PARAMETER(BytesRead);
    UNREFERENCED_PARAMETER(BytesWritten);
}
