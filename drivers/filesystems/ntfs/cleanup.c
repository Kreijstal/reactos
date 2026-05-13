/*
 *  ReactOS kernel
 *  Copyright (C) 2016 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/cleanup.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMER:       Pierre Schweitzer (pierre@reactos.org)
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Cleans up a file
 */
NTSTATUS
NtfsCleanupFile(PDEVICE_EXTENSION DeviceExt,
                PFILE_OBJECT FileObject,
                BOOLEAN CanWait)
{
    PNTFS_FCB Fcb;
    PNTFS_CCB Ccb;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("NtfsCleanupFile(DeviceExt %p, FileObject %p, CanWait %u)\n",
           DeviceExt,
           FileObject,
           CanWait);

    Fcb = (PNTFS_FCB)(FileObject->FsContext);
    Ccb = (PNTFS_CCB)(FileObject->FsContext2);
    if (!Fcb || Ccb == NULL || !BooleanFlagOn(Ccb->Flags, NTFS_CCB_FLAG_COUNTED))
        return STATUS_SUCCESS;

    ASSERT(DeviceExt->OpenHandleCount > 0);
    DeviceExt->OpenHandleCount--;

    /* Emit USN_REASON_CLOSE if this volume has an active journal and we
     * aren't looking at the volume-stream FCB.  Gated so un-journalled
     * volumes stay on the fast path.  Per-handle CCB-cached reasons
     * (OR'd into the close record) are a follow-up slice -- for now we
     * just announce the close itself so the basic read-back contract
     * holds.  Kreijstal/reactos#33. */
    if (DeviceExt->UsnJournalFcb != NULL &&
        !BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME) &&
        !BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME_STREAM))
    {
        ULONGLONG FileRef =
            ((ULONGLONG)Fcb->Entry.DirectoryFileReferenceNumber != 0)
                ? ((ULONGLONG)Fcb->Entry.DirectoryFileReferenceNumber & NTFS_MFT_MASK)
                : 0;
        /* Build the child FileRef: Fcb->MFTIndex is just the index,
         * combine with the FCB's own sequence number if available via
         * its parent's $FILE_NAME.DirectoryFileReferenceNumber. */
        (void)NtfsUsnEmitRecord(DeviceExt,
                                Fcb->MFTIndex,
                                FileRef,
                                USN_REASON_CLOSE,
                                0,
                                Fcb->Entry.Name,
                                Fcb->Entry.NameLength);
    }

    if (Fcb->Flags & FCB_IS_VOLUME)
    {
        Fcb->OpenHandleCount--;
    }
    else
    {
        if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource, CanWait))
        {
            return STATUS_PENDING;
        }

        Fcb->OpenHandleCount--;

        /* Flush any dirty data from the cache to disk before uninitializing.
         * This is critical because FsRtlCopyWrite may have put data into the
         * cache (even for FILE_NO_INTERMEDIATE_BUFFERING opens, due to the
         * shared cache map from the internal stream file object). Without an
         * explicit flush, the paging writeback may never complete before the
         * cache map is torn down. */
        if (FileObject->SectionObjectPointer->SharedCacheMap)
        {
            IO_STATUS_BLOCK FlushIoStatus;
            CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &FlushIoStatus);
        }

        DPRINT("NtfsCleanupFile: calling CcUninitializeCacheMap for FCB %p FileSize=%I64d\n", Fcb, Fcb->RFCB.FileSize.QuadPart);
        CcUninitializeCacheMap(FileObject, &Fcb->RFCB.FileSize, NULL);
        DPRINT("NtfsCleanupFile: CcUninitializeCacheMap returned\n");

        /* Drop this file object's contribution to the FCB's share access.
         * IoRemoveShareAccess decrements the relevant Readers/Writers/
         * Deleters counts that IoSetShareAccess/IoUpdateShareAccess
         * established at create time, allowing later opens with otherwise
         * incompatible share modes once all current handles have been
         * cleaned up. */
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

        if (Fcb->OpenHandleCount == 0 &&
            BooleanFlagOn(Fcb->Flags, FCB_DELETE_PENDING) &&
            Fcb->SectionObjectPointers != NULL &&
            Fcb->SectionObjectPointers->DataSectionObject == NULL &&
            Fcb->SectionObjectPointers->ImageSectionObject == NULL &&
            Fcb->SectionObjectPointers->SharedCacheMap == NULL)
        {
            Status = NtfsDeleteFileRecord(DeviceExt, Fcb, FALSE);
            if (NT_SUCCESS(Status))
                ClearFlag(Fcb->Flags, FCB_DELETE_PENDING);
            else
                DPRINT1("NtfsCleanupFile: NtfsDeleteFileRecord failed with 0x%lx\n", Status);
        }

        FileObject->Flags |= FO_CLEANUP_COMPLETE;

        ExReleaseResourceLite(&Fcb->MainResource);
    }

    return Status;
}

NTSTATUS
NtfsCleanup(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExtension;
    PFILE_OBJECT FileObject;
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject;

    DPRINT("NtfsCleanup() called\n");

    DeviceObject = IrpContext->DeviceObject;
    if (DeviceObject == NtfsGlobalData->DeviceObject)
    {
        DPRINT("Cleaning up file system\n");
        IrpContext->Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    FileObject = IrpContext->FileObject;
    DeviceExtension = DeviceObject->DeviceExtension;

    if (!ExAcquireResourceExclusiveLite(&DeviceExtension->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    Status = NtfsCleanupFile(DeviceExtension, FileObject, BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));
    DPRINT("NtfsCleanup: NtfsCleanupFile returned 0x%lx\n", Status);

    ExReleaseResourceLite(&DeviceExtension->DirResource);
    DPRINT("NtfsCleanup: DirResource released\n");

    if (Status == STATUS_PENDING)
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    IrpContext->Irp->IoStatus.Information = 0;
    return Status;
}
