/*
 * PROJECT:     ReactOS NTFS filesystem driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Flush mounted volumes at system shutdown
 */

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

static
NTSTATUS
NtfsShutdownStorageDevice(PDEVICE_OBJECT DeviceObject)
{
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_SHUTDOWN, DeviceObject,
                                       NULL, 0, NULL, &Event, &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }
    return Status;
}

NTSTATUS
NtfsShutdown(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT DeviceObject;
    PNTFS_VCB Vcb;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS VolumeStatus;
    BOOLEAN AlreadyStarted;

    IrpContext->Irp->IoStatus.Information = 0;
    if (IrpContext->DeviceObject != NtfsGlobalData->DeviceObject)
        return STATUS_INVALID_DEVICE_REQUEST;

    /* Mount takes this same resource. The driver-owned device list remains
     * stable while we flush, and no new mount can escape the shutdown pass. */
    ExAcquireResourceExclusiveLite(&NtfsGlobalData->Resource, TRUE);
    AlreadyStarted = NtfsGlobalData->ShutdownStarted;
    NtfsGlobalData->ShutdownStarted = TRUE;

    for (DeviceObject = NtfsGlobalData->DriverObject->DeviceObject;
         DeviceObject != NULL;
         DeviceObject = DeviceObject->NextDevice)
    {
        if (DeviceObject == NtfsGlobalData->DeviceObject)
            continue;
        Vcb = DeviceObject->DeviceExtension;
        ASSERT(Vcb->Identifier.Type == NTFS_TYPE_VCB);
        if (Vcb->Flags & VCB_VOLUME_DISMOUNTED)
            continue;

        /* Flush file data before the volume-stream metadata. Shutdown must
         * leave sections intact: eviction can re-enter MM during rundown. */
        VolumeStatus = NtfsFlushVolume(Vcb);
        if (NT_SUCCESS(VolumeStatus))
            VolumeStatus = NtfsShutdownStorageDevice(Vcb->StorageDevice);
        if (!NT_SUCCESS(VolumeStatus))
        {
            DPRINT1("Shutdown flush failed for volume %p: %lx\n", Vcb, VolumeStatus);
            if (NT_SUCCESS(Status))
                Status = VolumeStatus;
        }
    }

    ExReleaseResourceLite(&NtfsGlobalData->Resource);
    /* The I/O manager holds its registration lock while invoking mount;
     * unregister only after dropping the resource those mount requests need. */
    if (!AlreadyStarted)
        IoUnregisterFileSystem(NtfsGlobalData->DeviceObject);
    return Status;
}
