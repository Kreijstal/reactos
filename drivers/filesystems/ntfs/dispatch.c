/*
 *  ReactOS kernel
 *  Copyright (C) 2008 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/dispatch.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMER:       Pierre Schweitzer
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

static LONG QueueCount = 0;

/* FUNCTIONS ****************************************************************/

static WORKER_THREAD_ROUTINE NtfsDoRequest;

static
NTSTATUS
NtfsQueueRequest(PNTFS_IRP_CONTEXT IrpContext)
{
    InterlockedIncrement(&QueueCount);
    DPRINT("NtfsQueueRequest(IrpContext %p), %d\n", IrpContext, QueueCount);

    ASSERT(!(IrpContext->Flags & IRPCONTEXT_QUEUE) &&
           (IrpContext->Flags & IRPCONTEXT_COMPLETE));
    IrpContext->Flags |= IRPCONTEXT_CANWAIT;
    IoMarkIrpPending(IrpContext->Irp);
    ExInitializeWorkItem(&IrpContext->WorkQueueItem, NtfsDoRequest, IrpContext);
    ExQueueWorkItem(&IrpContext->WorkQueueItem, CriticalWorkQueue);

    return STATUS_PENDING;
}

static
NTSTATUS
NtfsDispatch(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    TRACE_(NTFS, "NtfsDispatch()\n");

    FsRtlEnterFileSystem();

    NtfsIsIrpTopLevel(Irp);

    switch (IrpContext->MajorFunction)
    {
        case IRP_MJ_QUERY_VOLUME_INFORMATION:
            Status = NtfsQueryVolumeInformation(IrpContext);
            break;

        case IRP_MJ_SET_VOLUME_INFORMATION:
            Status = NtfsSetVolumeInformation(IrpContext);
            break;

        case IRP_MJ_QUERY_INFORMATION:
            Status = NtfsQueryInformation(IrpContext);
            break;

        case IRP_MJ_SET_INFORMATION:
            if (!NtfsGlobalData->EnableWriteSupport)
            {
                DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
                Status = STATUS_ACCESS_DENIED;
            }
            else
            {
                Status = NtfsSetInformation(IrpContext);
            }
            break;

        case IRP_MJ_DIRECTORY_CONTROL:
            Status = NtfsDirectoryControl(IrpContext);
            break;

        case IRP_MJ_READ:
            Status = NtfsRead(IrpContext);
            break;

        case IRP_MJ_DEVICE_CONTROL:
            Status = NtfsDeviceControl(IrpContext);
             break;

        case IRP_MJ_WRITE:
            if (!NtfsGlobalData->EnableWriteSupport)
            {
                DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
                Status = STATUS_ACCESS_DENIED;
            }
            else
            {
                Status = NtfsWrite(IrpContext);
            }
            break;

        case IRP_MJ_CLOSE:
            Status = NtfsClose(IrpContext);
            break;

        case IRP_MJ_CLEANUP:
            Status = NtfsCleanup(IrpContext);
            break;

        case IRP_MJ_CREATE:
            Status = NtfsCreate(IrpContext);
            break;

        case IRP_MJ_FLUSH_BUFFERS:
            /* Flush cached data for the file */
            if (IrpContext->FileObject)
            {
                CcFlushCache(IrpContext->FileObject->SectionObjectPointer, NULL, 0, &IrpContext->Irp->IoStatus);
                Status = IrpContext->Irp->IoStatus.Status;
            }
            else
            {
                Status = STATUS_SUCCESS;
            }
            IrpContext->Irp->IoStatus.Information = 0;
            break;

        case IRP_MJ_FILE_SYSTEM_CONTROL:
            Status = NtfsFileSystemControl(IrpContext);
            break;

        case IRP_MJ_SHUTDOWN:
            /* Flush every mounted volume so that dirty cached writes
             * (notably registry hive updates) actually reach the disk
             * before the kernel finishes the shutdown. See shutdown.c. */
            Status = NtfsShutdown(IrpContext);
            break;

        case IRP_MJ_LOCK_CONTROL:
            /* Byte-range file locks via FsRtlProcessFileLock against
             * the per-FCB FILE_LOCK initialized in NtfsCreateFCB. The
             * helper takes ownership of the IRP, so NtfsLockControl
             * clears IRPCONTEXT_COMPLETE before returning. */
            Status = NtfsLockControl(IrpContext);
            break;

        case IRP_MJ_QUERY_EA:
            /* Read the on-disk $EA attribute and copy matching
             * FILE_FULL_EA_INFORMATION entries into the user buffer.
             * See ea.c. */
            Status = NtfsQueryEa(IrpContext);
            break;

        case IRP_MJ_SET_EA:
            /* Validate the input list with IoCheckEaBufferValidity and
             * write it back into the file's $EA attribute. Creating a
             * brand-new $EA on a file that has none yet is not yet
             * implemented (see ea.c TODO). */
            Status = NtfsSetEa(IrpContext);
            break;

        case IRP_MJ_PNP:
            /* Forward most PNP minor codes to the underlying storage
             * device. NtfsPnp clears IRPCONTEXT_COMPLETE on the
             * forwarded path so the dispatch loop below doesn't try
             * to complete an IRP that the lower driver now owns. */
            Status = NtfsPnp(IrpContext);
            break;
    }

    ASSERT((!(IrpContext->Flags & IRPCONTEXT_COMPLETE) && !(IrpContext->Flags & IRPCONTEXT_QUEUE)) ||
           ((IrpContext->Flags & IRPCONTEXT_COMPLETE) && !(IrpContext->Flags & IRPCONTEXT_QUEUE)) ||
           (!(IrpContext->Flags & IRPCONTEXT_COMPLETE) && (IrpContext->Flags & IRPCONTEXT_QUEUE)));

    if (IrpContext->Flags & IRPCONTEXT_COMPLETE)
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IrpContext->PriorityBoost);
    }

    if (IrpContext->Flags & IRPCONTEXT_QUEUE)
    {
        /* Reset our status flags before queueing the IRP */
        IrpContext->Flags |= IRPCONTEXT_COMPLETE;
        IrpContext->Flags &= ~IRPCONTEXT_QUEUE;
        Status = NtfsQueueRequest(IrpContext);
    }
    else
    {
        ExFreeToNPagedLookasideList(&NtfsGlobalData->IrpContextLookasideList, IrpContext);
    }

    IoSetTopLevelIrp(NULL);
    FsRtlExitFileSystem();

    return Status;
}

static
VOID
NTAPI
NtfsDoRequest(PVOID IrpContext)
{
    InterlockedDecrement(&QueueCount);
    DPRINT("NtfsDoRequest(IrpContext %p), MajorFunction %x, %d\n",
           IrpContext, ((PNTFS_IRP_CONTEXT)IrpContext)->MajorFunction, QueueCount);
    NtfsDispatch((PNTFS_IRP_CONTEXT)IrpContext);
}

/*
 * FUNCTION: This function manages IRP for various major functions
 * ARGUMENTS:
 *           DriverObject = object describing this driver
 *           Irp = IRP to be passed to internal functions
 * RETURNS: Status of I/O Request
 */
NTSTATUS
NTAPI
NtfsFsdDispatch(PDEVICE_OBJECT DeviceObject,
                PIRP Irp)
{
    PNTFS_IRP_CONTEXT IrpContext = NULL;
    NTSTATUS Status;
    TRACE_(NTFS, "NtfsFsdDispatch()\n");

    IrpContext = NtfsAllocateIrpContext(DeviceObject, Irp);
    if (IrpContext == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    else
    {
        Status = NtfsDispatch(IrpContext);
    }

     return Status;
}
