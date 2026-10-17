

/* INCLUDES *****************************************************************/

#include "ntfs.h"

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
    NTSTATUS Status = STATUS_SUCCESS;

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
    return Status;
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
        return STATUS_VOLUME_DISMOUNTED;
    }

    if (IrpContext->FileObject != NULL)
    {
        NTSTATUS Status = NtfsFlushFileCache(IrpContext->FileObject, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    return NtfsFlushVolume(DeviceExt);
}

/* EOF */
