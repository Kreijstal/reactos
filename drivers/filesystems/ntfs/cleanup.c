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

#include "lfswrite.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsUncountOpen
*
* Takes a handle out of its FCB's and its volume's open handle counts, if its open was
* counted. A stream file object the driver made for itself never was.
*/
static
VOID
NtfsUncountOpen(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_FCB Fcb,
    _In_ PFILE_OBJECT FileObject)
{
    PNTFS_CCB Ccb = FileObject->FsContext2;

    if (Ccb == NULL || !BooleanFlagOn(Ccb->Flags, CCB_COUNTED))
        return;

    ASSERT(Fcb->OpenHandleCount != 0);
    ASSERT(DeviceExt->OpenHandleCount != 0);

    Ccb->Flags &= ~CCB_COUNTED;
    Fcb->OpenHandleCount--;
    DeviceExt->OpenHandleCount--;
}

/**
* @name NtfsApplyDeleteOnClose
*
* A handle opened with FILE_DELETE_ON_CLOSE marks its file for deletion when it's cleaned
* up; the file goes once its last handle is. A directory that isn't empty by then stays.
*/
static
VOID
NtfsApplyDeleteOnClose(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_FCB Fcb,
    _In_ PFILE_OBJECT FileObject)
{
    PNTFS_CCB Ccb = FileObject->FsContext2;

    if (Ccb == NULL || !BooleanFlagOn(Ccb->Flags, CCB_DELETE_ON_CLOSE))
        return;

    Ccb->Flags &= ~CCB_DELETE_ON_CLOSE;

    if (NtfsFCBIsDirectory(Fcb))
    {
        BOOLEAN Empty;
        NTSTATUS Status;

        KeEnterCriticalRegion();
        ExAcquireResourceSharedLite(&DeviceExt->IndexResource, TRUE);
        Status = NtfsIsDirectoryEmpty(DeviceExt, Fcb->MFTIndex, &Empty);
        ExReleaseResourceLite(&DeviceExt->IndexResource);
        KeLeaveCriticalRegion();

        if (!NT_SUCCESS(Status) || !Empty)
            return;
    }

    Fcb->Flags |= FCB_DELETE_PENDING;
    FileObject->DeletePending = TRUE;
}

/* The caller owns DirResource, MainResource and PagingIoResource. Cleanup
 * stamps the final times once cached writes through this open are complete. */
static NTSTATUS
NtfsStampWriteTimes(PDEVICE_EXTENSION Vcb, PNTFS_FCB Fcb, PFILE_OBJECT FileObject)
{
    PNTFS_CCB Ccb = FileObject->FsContext2;
    PFILE_RECORD_HEADER Record;
    PSTANDARD_INFORMATION Info;
    LARGE_INTEGER Now;
    NTSTATUS Status;

    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    ExAcquireResourceExclusiveLite(&Vcb->IndexResource, TRUE);
    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, Record);
    if (NT_SUCCESS(Status))
    {
        Info = GetStandardInformationFromRecord(Vcb, Record);
        ASSERT(Info != NULL);
        KeQuerySystemTime(&Now);
        if ((FileObject->Flags & FO_FILE_MODIFIED) && !(Ccb->Flags & CCB_USER_SET_WRITE_TIME))
            Info->LastWriteTime = Now.QuadPart;
        if (!(Ccb->Flags & CCB_USER_SET_CHANGE_TIME))
            Info->ChangeTime = Now.QuadPart;
        if (!NtfsFCBIsDirectory(Fcb))
            Info->FileAttribute |= FILE_ATTRIBUTE_ARCHIVE;
        Status = NtfsUpdateDuplicatedInformation(Vcb, Record, Fcb->MFTIndex,
                                                 NTFS_FILENAME_UPDATE_TIMES |
                                                 NTFS_FILENAME_UPDATE_ATTRS, FALSE);
        if (NT_SUCCESS(Status))
        {
            Fcb->Entry.LastWriteTime = Info->LastWriteTime;
            Fcb->Entry.ChangeTime = Info->ChangeTime;
            Fcb->Entry.FileAttributes = Info->FileAttribute |
                                       (Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_DIRECTORY);
            FileObject->Flags &= ~FO_FILE_MODIFIED;
            Ccb->Flags &= ~CCB_UPDATE_CHANGE_TIME;
        }
    }
    ExReleaseResourceLite(&Vcb->IndexResource);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    return Status;
}

/*
 * FUNCTION: Cleans up a file
 */
NTSTATUS
NtfsCleanupFile(PDEVICE_EXTENSION DeviceExt,
                PFILE_OBJECT FileObject,
                BOOLEAN CanWait)
{
    PNTFS_FCB Fcb;
    BOOLEAN DeletePending = FALSE;

    DPRINT("NtfsCleanupFile(DeviceExt %p, FileObject %p, CanWait %u)\n",
           DeviceExt,
           FileObject,
           CanWait);

    Fcb = (PNTFS_FCB)(FileObject->FsContext);
    if (!Fcb)
        return STATUS_SUCCESS;

    FsRtlNotifyCleanup(DeviceExt->NotifySync, &DeviceExt->NotifyList, FileObject->FsContext2);

    if (Fcb->Flags & FCB_IS_VOLUME)
    {
        NtfsUncountOpen(DeviceExt, Fcb, FileObject);
    }
    else
    {
        if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource, CanWait))
        {
            return STATUS_PENDING;
        }

        ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

        if (!(DeviceExt->Flags & (VCB_DEVICE_REMOVED | VCB_VOLUME_READ_ONLY)))
            NtfsApplyDeleteOnClose(DeviceExt, Fcb, FileObject);

        NtfsUncountOpen(DeviceExt, Fcb, FileObject);

        if (Fcb->FileLock != NULL)
        {
            PNTFS_CCB Ccb = FileObject->FsContext2;
            PNTFS_LOCK_OWNER Owner;

            ExAcquireResourceExclusiveLite(&Fcb->FileLock->Resource, TRUE);
            for (Owner = Ccb->LockOwners; Owner != NULL; Owner = Owner->Next)
                (VOID)FsRtlFastUnlockAll(&Fcb->FileLock->Lock, FileObject, Owner->Process, NULL);
            ExReleaseResourceLite(&Fcb->FileLock->Resource);
        }

        if (Fcb->ShareAccess.OpenCount > 0)
        {
            IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        }

        /* A file marked for deletion goes on the last handle. Truncate it first, so the cache
         * manager drops its pages while there are still clusters to write them back to. */
        if (Fcb->OpenHandleCount == 0 &&
            BooleanFlagOn(Fcb->Flags, FCB_DELETE_PENDING) &&
            !(DeviceExt->Flags & (VCB_DEVICE_REMOVED | VCB_VOLUME_READ_ONLY)))
        {
            DeletePending = TRUE;

            if (!NtfsFCBIsDirectory(Fcb))
            {
                Fcb->RFCB.FileSize.QuadPart = 0;
                Fcb->RFCB.ValidDataLength.QuadPart = 0;
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
            }
        }

        if (!DeletePending &&
            ((FileObject->Flags & FO_FILE_MODIFIED) ||
             (((PNTFS_CCB)FileObject->FsContext2)->Flags & CCB_UPDATE_CHANGE_TIME)) &&
            !(DeviceExt->Flags & (VCB_VOLUME_DISMOUNTED | VCB_DEVICE_REMOVED | VCB_VOLUME_READ_ONLY)))
        {
            NTSTATUS Status = NtfsStampWriteTimes(DeviceExt, Fcb, FileObject);
            if (!NT_SUCCESS(Status))
                DPRINT1("Unable to stamp write times for '%wS': %lx\n", Fcb->PathName, Status);
        }

        /* Give back what writes reserved past the end of the file */
        if (Fcb->OpenHandleCount == 0 &&
            !DeletePending &&
            !NtfsFCBIsDirectory(Fcb) &&
            !(DeviceExt->Flags & (VCB_VOLUME_DISMOUNTED | VCB_DEVICE_REMOVED | VCB_VOLUME_READ_ONLY)) &&
            Fcb->RFCB.AllocationSize.QuadPart > (LONGLONG)ROUND_UP(Fcb->RFCB.FileSize.QuadPart, DeviceExt->NtfsInfo.BytesPerCluster))
        {
            NTSTATUS Status;

            Status = NtfsTrimAllocation(Fcb);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Couldn't trim the allocation of '%wS' (Status %lx)\n", Fcb->ObjectName, Status);
            }
            else
            {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
            }
        }

        /* Pass a truncate size only when the file really is being truncated,
         * as on delete. The current file size is not a truncation: Cc purges
         * from the page holding it, which would discard the dirty, still valid
         * head of a partial last page that only a mapped view has written (a
         * file grown by a data section and filled through its view). */
        CcUninitializeCacheMap(FileObject, DeletePending ? &Fcb->RFCB.FileSize : NULL, NULL);

        /* Only once the cache map is gone. Freeing the record while a section still refers to
         * it faults Mm on the next page-in. */
        if (DeletePending)
        {
            NTSTATUS Status;

            MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForDelete);

            /* Something may have been created in the directory since the delete
             * was requested; freeing it now would orphan that child */
            Status = STATUS_SUCCESS;
            if (NtfsFCBIsDirectory(Fcb))
            {
                BOOLEAN Empty;

                KeEnterCriticalRegion();
                ExAcquireResourceSharedLite(&DeviceExt->IndexResource, TRUE);
                Status = NtfsIsDirectoryEmpty(DeviceExt, Fcb->MFTIndex, &Empty);
                ExReleaseResourceLite(&DeviceExt->IndexResource);
                KeLeaveCriticalRegion();

                if (NT_SUCCESS(Status) && !Empty)
                    Status = STATUS_DIRECTORY_NOT_EMPTY;
            }

            /* Deleting a named stream takes only that stream, deleting a name only that
             * name while the file has others */
            if (NT_SUCCESS(Status))
            {
                NtfsPostUsnClose(DeviceExt,
                                 Fcb,
                                 (Fcb->Stream[0] != UNICODE_NULL) ? USN_REASON_STREAM_CHANGE :
                                 (Fcb->LinkCount > 1) ? USN_REASON_HARD_LINK_CHANGE : USN_REASON_FILE_DELETE);

                if (Fcb->Stream[0] != UNICODE_NULL)
                {
                    Status = NtfsDeleteStream(DeviceExt, Fcb->MFTIndex, Fcb->Stream);
                }
                else
                {
                    UNICODE_STRING Name;
                    USHORT LinksLeft;

                    RtlInitUnicodeString(&Name, Fcb->ObjectName + 1);
                    Status = NtfsDeleteUserFileLink(DeviceExt,
                                                Fcb->MFTIndex,
                                                Fcb->ParentMFTIndex,
                                                &Name,
                                                FALSE,
                                                &LinksLeft);
                    if (NT_SUCCESS(Status) && LinksLeft != 0)
                        NtfsSetLinkCount(DeviceExt, Fcb->MFTIndex, LinksLeft);
                }
            }

            /* A cached FCB for a path that no longer exists would make it look present */
            if (NT_SUCCESS(Status))
            {
                if (Fcb->Stream[0] == UNICODE_NULL)
                    NtfsReportPathChange(DeviceExt, Fcb->PathName,
                                          NtfsFCBIsDirectory(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                                          FILE_ACTION_REMOVED);
                else
                    NtfsReportChange(Fcb, FILE_NOTIFY_CHANGE_STREAM_NAME, FILE_ACTION_REMOVED_STREAM);
                NtfsRemoveFCBFromTable(DeviceExt, Fcb);
            }

            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR: Failed to delete '%wS' (MFT record %I64u), Status %lx\n",
                        Fcb->ObjectName, Fcb->MFTIndex, Status);
            }

            Fcb->Flags &= ~FCB_DELETE_PENDING;
        }

        /* The last handle closes the file for the change journal */
        if (Fcb->OpenHandleCount == 0 && !DeletePending && Fcb->UsnReasons != 0 &&
            !(DeviceExt->Flags & (VCB_DEVICE_REMOVED | VCB_VOLUME_READ_ONLY)))
            NtfsPostUsnClose(DeviceExt, Fcb, 0);

        if (Fcb->OpenHandleCount != 0)
        {
            // Remove share access when handled
        }

        FileObject->Flags |= FO_CLEANUP_COMPLETE;

        ExReleaseResourceLite(&Fcb->PagingIoResource);
        ExReleaseResourceLite(&Fcb->MainResource);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NtfsCleanup(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExtension;
    PFILE_OBJECT FileObject;
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject;
    NTFS_LOG_REQUEST LogRequest;

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

    /* A deletion on close is one transaction.  DirResource keeps the
     * writer from being torn down under it. */
    NtfsLfsBeginRequest(DeviceExtension->LfsWriter, &LogRequest);
    Status = NtfsCleanupFile(DeviceExtension, FileObject,
                              BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));
    NtfsLfsEndRequest(DeviceExtension->LfsWriter, &LogRequest, FALSE);

    ExReleaseResourceLite(&DeviceExtension->DirResource);

    if (Status == STATUS_PENDING)
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    IrpContext->Irp->IoStatus.Information = 0;
    return Status;
}
