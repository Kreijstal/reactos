/*
 * PROJECT:     ReactOS NTFS filesystem driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Storage removal and PnP forwarding
 */

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

static
NTSTATUS
NTAPI
NtfsForwardCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Keep ownership until the lower stack finishes. In particular, a query
 * accepted here must be rolled back if a lower driver vetoes removal. */
NTSTATUS
NtfsForwardIrpSynchronous(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    KEVENT Event;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, NtfsForwardCompletion, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    return Irp->IoStatus.Status;
}

/* IoResource and DirResource are held exclusive. A section retains its file
 * object after cleanup, so handle counts alone do not establish quiescence. */
static
BOOLEAN
NtfsVolumeIsBusy(PNTFS_VCB Vcb, ULONG AllowedHandles)
{
    PLIST_ENTRY Entry;
    PNTFS_FCB Fcb;
    KIRQL OldIrql;
    BOOLEAN Busy = (Vcb->OpenHandleCount != AllowedHandles ||
                    Vcb->FileObjectCount != AllowedHandles);

    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);
    for (Entry = Vcb->FcbListHead.Flink; !Busy && Entry != &Vcb->FcbListHead; Entry = Entry->Flink)
    {
        Fcb = CONTAINING_RECORD(Entry, NTFS_FCB, FcbListEntry);
        if (Fcb->Flags & FCB_IS_VOLUME)
            continue;
        /* The internal cache stream is ours; any other reference can belong
         * to a mapped file, an image, or an outstanding external file object. */
        Busy = Fcb->RefCount > (Fcb->FileObject != NULL ? 1 : 0);
    }
    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
    return Busy;
}

static
NTSTATUS
NtfsPrepareForRemoval(PNTFS_VCB Vcb, ULONG AllowedHandles)
{
    if (Vcb->Flags & VCB_DEVICE_REMOVED)
        return STATUS_DEVICE_REMOVED;
    if (Vcb->Flags & VCB_QUERY_REMOVE_PENDING)
        return STATUS_SUCCESS;
    if (NtfsVolumeIsBusy(Vcb, AllowedHandles))
        return STATUS_ACCESS_DENIED;
    if (Vcb->Flags & VCB_VOLUME_DISMOUNTED)
        return STATUS_SUCCESS;
    return NtfsFlushVolume(Vcb);
}

static
VOID
NtfsMarkDeviceRemoved(PNTFS_VCB Vcb)
{
    PFILE_OBJECT Stream;
    PNTFS_FCB Fcb;
    KIRQL OldIrql;

    if (Vcb->Flags & VCB_DEVICE_REMOVED)
        return;

    /* An accepted query has already flushed; surprise removal cannot flush.
     * Refuse new paging requests before waiting for ones already in flight.
     * Those requests may recursively issue volume-stream paging I/O, which
     * must fail instead of waiting behind the exclusive acquisition here. */
    Vcb->Flags |= VCB_DEVICE_REMOVED;
    Vcb->Flags &= ~VCB_QUERY_REMOVE_PENDING;
    ExAcquireResourceExclusiveLite(&Vcb->PagingResource, TRUE);
    ExReleaseResourceLite(&Vcb->PagingResource);
    NtfsRemoveFileLocks(Vcb);

    Stream = Vcb->StreamFileObject;
    if (Stream != NULL)
    {
        Fcb = Stream->FsContext;
        CcPurgeCacheSection(Stream->SectionObjectPointer, NULL, 0, FALSE);
        CcUninitializeCacheMap(Stream, NULL, NULL);
        Vcb->StreamFileObject = NULL;
        Fcb->FileObject = NULL;
        ObDereferenceObject(Stream);
    }

    /* Retain the upstream VCB and outstanding FCBs for their old handles.
     * Unlike a manual dismount, even raw volume I/O is now prohibited. */
    IoAcquireVpbSpinLock(&OldIrql);
    Vcb->Vpb->Flags &= ~(VPB_MOUNTED | VPB_DIRECT_WRITES_ALLOWED);
    IoReleaseVpbSpinLock(OldIrql);
}

NTSTATUS
NtfsPnp(PNTFS_IRP_CONTEXT IrpContext)
{
    PNTFS_VCB Vcb;
    PDEVICE_OBJECT StorageDevice;
    PIRP Irp = IrpContext->Irp;
    NTSTATUS Status;
    BOOLEAN WasPending;

    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
        return STATUS_NOT_SUPPORTED;

    Vcb = IrpContext->DeviceObject->DeviceExtension;
    switch (IrpContext->MinorFunction)
    {
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_EJECT:
            break;

        default:
            /* FsRtlNotifyVolumeEvent can ask for TargetDeviceRelation while
             * its caller already owns IoResource shared. Only removal needs
             * exclusive ownership; upgrading here would deadlock dismount.
             * Shared ownership still keeps storage alive through completion. */
            ExAcquireResourceSharedLite(&Vcb->IoResource, TRUE);
            if ((Vcb->Flags & VCB_DEVICE_REMOVED) || Vcb->StorageDevice == NULL)
                Status = STATUS_DEVICE_REMOVED;
            else
                Status = NtfsForwardIrpSynchronous(Vcb->StorageDevice, Irp);
            ExReleaseResourceLite(&Vcb->IoResource);
            return Status;
    }

    ExAcquireResourceExclusiveLite(&Vcb->IoResource, TRUE);
    ExAcquireResourceExclusiveLite(&Vcb->DirResource, TRUE);
    StorageDevice = Vcb->StorageDevice;
    if (StorageDevice == NULL)
    {
        Status = (IrpContext->MinorFunction == IRP_MN_REMOVE_DEVICE ||
                  IrpContext->MinorFunction == IRP_MN_CANCEL_REMOVE_DEVICE) ?
                 STATUS_SUCCESS : STATUS_DEVICE_REMOVED;
        goto Done;
    }

    /* REMOVE can make the lower object delete-pending before it completes. */
    ObReferenceObject(StorageDevice);
    switch (IrpContext->MinorFunction)
    {
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_EJECT:
            Status = NtfsPrepareForRemoval(Vcb, 0);
            if (!NT_SUCCESS(Status))
                break;
            WasPending = BooleanFlagOn(Vcb->Flags, VCB_QUERY_REMOVE_PENDING);
            Vcb->Flags |= VCB_QUERY_REMOVE_PENDING;
            ExAcquireResourceExclusiveLite(&Vcb->PagingResource, TRUE);
            ExReleaseResourceLite(&Vcb->PagingResource);
            Status = NtfsForwardIrpSynchronous(StorageDevice, Irp);
            if (!NT_SUCCESS(Status) && !WasPending)
                Vcb->Flags &= ~VCB_QUERY_REMOVE_PENDING;
            else if (NT_SUCCESS(Status) && IrpContext->MinorFunction == IRP_MN_EJECT)
            {
                NtfsMarkDeviceRemoved(Vcb);
                Vcb->StorageDevice = NULL;
            }
            break;

        case IRP_MN_CANCEL_REMOVE_DEVICE:
            if (Vcb->Flags & VCB_DEVICE_REMOVED)
            {
                Status = STATUS_SUCCESS;
                break;
            }
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = NtfsForwardIrpSynchronous(StorageDevice, Irp);
            /* Cancellation is not resurrection after surprise removal. */
            Vcb->Flags &= ~VCB_QUERY_REMOVE_PENDING;
            break;

        case IRP_MN_REMOVE_DEVICE:
            if (!(Vcb->Flags & (VCB_DEVICE_REMOVED | VCB_QUERY_REMOVE_PENDING | VCB_VOLUME_DISMOUNTED)))
            {
                Status = NtfsFlushVolume(Vcb);
                if (!NT_SUCCESS(Status))
                    DPRINT1("Removal flush failed: %lx\n", Status);
            }
            NtfsMarkDeviceRemoved(Vcb);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = NtfsForwardIrpSynchronous(StorageDevice, Irp);
            Vcb->StorageDevice = NULL;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            NtfsMarkDeviceRemoved(Vcb);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = NtfsForwardIrpSynchronous(StorageDevice, Irp);
            break;

        default:
            /* Relations, capabilities, IDs, etc. belong to the storage stack.
             * Success with no DEVICE_RELATIONS would break PnP enumeration. */
            if (Vcb->Flags & VCB_DEVICE_REMOVED)
                Status = STATUS_DEVICE_REMOVED;
            else
                Status = NtfsForwardIrpSynchronous(StorageDevice, Irp);
            break;
    }
    ObDereferenceObject(StorageDevice);
Done:
    ExReleaseResourceLite(&Vcb->DirResource);
    ExReleaseResourceLite(&Vcb->IoResource);
    return Status;
}

NTSTATUS
NtfsEjectMedia(PNTFS_IRP_CONTEXT IrpContext)
{
    PNTFS_VCB Vcb = IrpContext->DeviceObject->DeviceExtension;
    PNTFS_FCB Fcb;
    NTSTATUS Status;
    BOOLEAN WasPending;

    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject ||
        IrpContext->FileObject == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;
    Fcb = IrpContext->FileObject->FsContext;
    if (!(Fcb->Flags & FCB_IS_VOLUME))
        return STATUS_INVALID_PARAMETER;

    ExAcquireResourceExclusiveLite(&Vcb->IoResource, TRUE);
    ExAcquireResourceExclusiveLite(&Vcb->DirResource, TRUE);
    Status = NtfsPrepareForRemoval(Vcb, 1);
    if (NT_SUCCESS(Status))
    {
        WasPending = BooleanFlagOn(Vcb->Flags, VCB_QUERY_REMOVE_PENDING);
        Vcb->Flags |= VCB_QUERY_REMOVE_PENDING;
        ExAcquireResourceExclusiveLite(&Vcb->PagingResource, TRUE);
        ExReleaseResourceLite(&Vcb->PagingResource);
        Status = NtfsForwardIrpSynchronous(Vcb->StorageDevice, IrpContext->Irp);
        if (NT_SUCCESS(Status))
        {
            NtfsMarkDeviceRemoved(Vcb);
            Vcb->StorageDevice = NULL;
        }
        else if (!WasPending)
            Vcb->Flags &= ~VCB_QUERY_REMOVE_PENDING;
    }
    ExReleaseResourceLite(&Vcb->DirResource);
    ExReleaseResourceLite(&Vcb->IoResource);
    return Status;
}
