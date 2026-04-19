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

/*
 * IRP_MJ_QUERY_SECURITY handler.  Looks up the FCB from
 * IrpSp->FileObject->FsContext, reads the owning file record, calls the
 * NtfsGetSecurityFromRecord worker (security.c), and surfaces the result
 * back through the IRP.  Kreijstal/reactos#36 — per-file $SECURITY_DESCRIPTOR
 * (NOT \$Secure:$SDS, that's a follow-up slice).
 *
 * IoStatus contract:
 *   - success:  Information = bytes copied out.
 *   - overflow: Information = required length, Status = STATUS_BUFFER_OVERFLOW.
 *   - other:    Information = 0.
 */
static
NTSTATUS
NtfsQuerySecurity(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PFILE_OBJECT FileObject = IrpContext->FileObject;
    PNTFS_FCB Fcb;
    PNTFS_VCB Vcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PVOID OutBuffer;
    ULONG OutLength;
    ULONG LengthOut = 0;
    NTSTATUS Status;

    DPRINT("NtfsQuerySecurity(%p)\n", IrpContext);

    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;

    Fcb = (PNTFS_FCB)FileObject->FsContext;
    if (Fcb->Identifier.Type != NTFS_TYPE_FCB)
        return STATUS_INVALID_DEVICE_REQUEST;

    Vcb = (PNTFS_VCB)IrpContext->DeviceObject->DeviceExtension;

    /* For direct I/O IRPs the user buffer is owned by the caller — dereference
     * via the Mdl when one was supplied, else the direct UserBuffer pointer.
     * Mirrors btrfs's map_user_buffer helper. */
    if (Irp->MdlAddress != NULL)
    {
        OutBuffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (OutBuffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        OutBuffer = Irp->UserBuffer;
    }
    OutLength = Stack->Parameters.QuerySecurity.Length;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return Status;
    }

    Status = NtfsGetSecurityFromRecord(Vcb, FileRecord, OutBuffer, OutLength, &LengthOut);

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
        Irp->IoStatus.Information = LengthOut;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}

/*
 * IRP_MJ_SET_SECURITY handler.  The IO manager has already captured and
 * validated the caller-supplied SD; it arrives as a self-relative blob at
 * IrpSp->Parameters.SetSecurity.SecurityDescriptor.  We persist it into the
 * file's $SECURITY_DESCRIPTOR attribute via NtfsSetSecurityOnRecord.
 *
 * TODO (follow-up slice): honour SECURITY_INFORMATION flags (OWNER / GROUP /
 * DACL / SACL) by merging the incoming SD against the existing one instead
 * of wholesale replacement.
 */
static
NTSTATUS
NtfsSetSecurity(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PFILE_OBJECT FileObject = IrpContext->FileObject;
    PNTFS_FCB Fcb;
    PNTFS_VCB Vcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PSECURITY_DESCRIPTOR InSd;
    ULONG InLength;
    NTSTATUS Status;

    DPRINT("NtfsSetSecurity(%p)\n", IrpContext);

    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;

    Fcb = (PNTFS_FCB)FileObject->FsContext;
    if (Fcb->Identifier.Type != NTFS_TYPE_FCB)
        return STATUS_INVALID_DEVICE_REQUEST;

    Vcb = (PNTFS_VCB)IrpContext->DeviceObject->DeviceExtension;

    InSd = Stack->Parameters.SetSecurity.SecurityDescriptor;
    if (InSd == NULL)
        return STATUS_INVALID_PARAMETER;

    /* RtlLengthSecurityDescriptor returns the serialized byte count of a
     * self-relative SD — the form the IO manager always gives us. */
    InLength = RtlLengthSecurityDescriptor(InSd);
    if (InLength == 0)
        return STATUS_INVALID_SECURITY_DESCR;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return Status;
    }

    Status = NtfsSetSecurityOnRecord(Vcb, FileRecord, InSd, InLength);
    if (NT_SUCCESS(Status))
    {
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    }

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    Irp->IoStatus.Information = 0;
    return Status;
}

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

        case IRP_MJ_QUERY_SECURITY:
            /* Read the on-disk $SECURITY_DESCRIPTOR (or synthesise the
             * default world-readable SD if absent) into the caller's
             * buffer.  Kreijstal/reactos#36 — see security.c. */
            Status = NtfsQuerySecurity(IrpContext);
            break;

        case IRP_MJ_SET_SECURITY:
            /* Write the supplied self-relative SD into the file's
             * $SECURITY_DESCRIPTOR attribute.  Creation of a brand-new
             * attribute is supported; in-place overwrite of same size
             * is supported; resize is not yet (delete + re-set). */
            if (!NtfsGlobalData->EnableWriteSupport)
            {
                DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
                Status = STATUS_ACCESS_DENIED;
            }
            else
            {
                Status = NtfsSetSecurity(IrpContext);
            }
            break;

        case IRP_MJ_QUERY_QUOTA:
        case IRP_MJ_SET_QUOTA:
            /* Disk quota (Kreijstal/reactos#37).  This slice plumbs the
             * IRP_MJ slots so callers see a stable STATUS_NOT_SUPPORTED
             * return code on un-quota'd volumes (and on quota'd volumes
             * too -- the on-disk $Q/$O indexes are blocked on btree.c
             * growing non-$I30 collation support; see quota.c banner).
             * Windows drivers return STATUS_NOT_SUPPORTED on this major
             * when they don't maintain quota, so this matches the
             * expected shape. */
            IrpContext->Irp->IoStatus.Information = 0;
            Status = STATUS_NOT_SUPPORTED;
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
