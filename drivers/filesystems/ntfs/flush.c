

/* INCLUDES *****************************************************************/

#include "lfswrite.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* Cc may issue paging writes while flushing. Hold the directory resource,
 * which stabilizes the FCB table, but never its spin lock across Cc calls. */
NTSTATUS
NtfsFlushVolume(PDEVICE_EXTENSION Vcb)
{
    PLIST_ENTRY Entry;
    PNTFS_FCB Fcb;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    Status = NtfsCheckpointUsnJournal(Vcb);
    if (!NT_SUCCESS(Status))
        return Status;
    ExAcquireResourceExclusiveLite(&Vcb->DirResource, TRUE);
    for (Entry = Vcb->FcbListHead.Flink;
         Entry != &Vcb->FcbListHead;
         Entry = Entry->Flink)
    {
        Fcb = CONTAINING_RECORD(Entry, NTFS_FCB, FcbListEntry);
        if (NtfsFCBIsDirectory(Fcb))
            continue;
        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &IoStatus);
        ExReleaseResourceLite(&Fcb->MainResource);
        if (!NT_SUCCESS(IoStatus.Status) && NT_SUCCESS(Status))
            Status = IoStatus.Status;
    }

    /* Data writeback can itself dirty metadata. Flush the volume stream last. */
    if (Vcb->StreamFileObject != NULL)
    {
        CcFlushCache(Vcb->StreamFileObject->SectionObjectPointer, NULL, 0, &IoStatus);
        if (!NT_SUCCESS(IoStatus.Status) && NT_SUCCESS(Status))
            Status = IoStatus.Status;
    }
    ExReleaseResourceLite(&Vcb->DirResource);

    if (NT_SUCCESS(Status))
        Status = NtfsFlushDevice(Vcb->StorageDevice);
    /* Everything logged so far is on disk: a checkpoint. */
    if (NT_SUCCESS(Status))
        Status = NtfsLfsCheckpoint(Vcb->LfsWriter);
    return Status;
}

/* Make what this volume's writes have put in the volume stream durable.
 * File data reaches storage through the volume stream, so a file whose own
 * cache has been flushed is on the media once the volume stream and the
 * device are. Other files' caches are not touched. */
NTSTATUS
NtfsFlushVolumeStream(PDEVICE_EXTENSION Vcb)
{
    IO_STATUS_BLOCK IoStatus;

    CcFlushCache(Vcb->StreamFileObject->SectionObjectPointer, NULL, 0, &IoStatus);
    if (!NT_SUCCESS(IoStatus.Status))
        return IoStatus.Status;
    return NtfsFlushDevice(Vcb->StorageDevice);
}

/* Flush before invalidation, and leave dirty pages intact on an I/O error. */
NTSTATUS
NtfsFlushFileCache(PFILE_OBJECT FileObject, BOOLEAN Purge)
{
    IO_STATUS_BLOCK IoStatus;

    CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &IoStatus);
    if (!NT_SUCCESS(IoStatus.Status))
        return IoStatus.Status;
    if (Purge)
        CcPurgeCacheSection(FileObject->SectionObjectPointer, NULL, 0, FALSE);
    return STATUS_SUCCESS;
}

/**
* @name NtfsFlushBuffers
* @implemented
*
* Handles IRP_MJ_FLUSH_BUFFERS.
*
* @param IrpContext
* Points to an NTFS_IRP_CONTEXT which describes the request
*
* @return
* STATUS_SUCCESS if everything reached the media, otherwise the status the
* storage stack returned.
*
*/
NTSTATUS
NtfsFlushBuffers(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExt;

    DPRINT("NtfsFlushBuffers(%p)\n", IrpContext);

    IrpContext->Irp->IoStatus.Information = 0;

    /* Nothing is mounted on the main device object */
    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
    {
        return STATUS_SUCCESS;
    }

    DeviceExt = IrpContext->DeviceObject->DeviceExtension;

    if (DeviceExt->Flags & VCB_VOLUME_DISMOUNTED)
    {
        /* Offline format/repair writes use the existing raw volume handle.
         * Its flush must reach storage without revisiting stale metadata. */
        if (IrpContext->FileObject != NULL)
        {
            PNTFS_FCB Fcb = IrpContext->FileObject->FsContext;

            ASSERT(Fcb != NULL);
            if (Fcb->Flags & FCB_IS_VOLUME)
                return NtfsFlushDevice(DeviceExt->StorageDevice);
        }
        return STATUS_VOLUME_DISMOUNTED;
    }

    /* A volume handle flushes the volume. A file handle flushes that file
     * and the metadata describing it, as on Windows: not every other file. */
    if (IrpContext->FileObject != NULL &&
        !(((PNTFS_FCB)IrpContext->FileObject->FsContext)->Flags & FCB_IS_VOLUME))
    {
        NTSTATUS Status = NtfsFlushFileCache(IrpContext->FileObject, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;
        return NtfsFlushVolumeStream(DeviceExt);
    }
    return NtfsFlushVolume(DeviceExt);
}

/* EOF */
