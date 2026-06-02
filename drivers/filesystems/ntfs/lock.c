/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/lock.c
 * PURPOSE:          NTFS filesystem driver - IRP_MJ_LOCK_CONTROL handler
 *
 * Byte-range file locks (LockFile / UnlockFile / LockFileEx). All four
 * minor codes (IRP_MN_LOCK, IRP_MN_UNLOCK_SINGLE, IRP_MN_UNLOCK_ALL,
 * IRP_MN_UNLOCK_ALL_BY_KEY) are dispatched through the FsRtl helper
 * FsRtlProcessFileLock, which uses the per-FCB FILE_LOCK state that
 * fcb.c initializes via FsRtlInitializeFileLock.
 *
 * FsRtlProcessFileLock owns the IRP from the moment we call it - it
 * either completes the IRP synchronously (most cases) or queues it
 * waiting for a conflicting lock to drop. Either way, NtfsDispatch
 * must NOT touch the IRP afterwards, so we clear IRPCONTEXT_COMPLETE
 * before returning so the post-switch IoCompleteRequest in dispatch.c
 * is skipped.
 *
 * Note that just registering the lock IRP isn't enough to actually
 * enforce locks against concurrent reads / writes - those have to
 * consult the FILE_LOCK via FsRtlCheckLockForReadAccess /
 * FsRtlCheckLockForWriteAccess in NtfsRead / NtfsWrite. That part is
 * also done as part of this change so byte-range locks behave the way
 * Win32 callers expect.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

NTSTATUS
NtfsLockControl(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PFILE_OBJECT FileObject = IrpContext->FileObject;
    PNTFS_FCB Fcb;
    NTSTATUS Status;

    DPRINT("NtfsLockControl(): MinorFunction=%u\n", IrpContext->MinorFunction);

    if (FileObject == NULL || FileObject->FsContext == NULL)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    Fcb = (PNTFS_FCB)FileObject->FsContext;

    /* Locks only make sense on file streams, not directories or the
     * volume stream. Reject anything else with the conventional
     * status fastfat returns. */
    if (Fcb->Identifier.Type != NTFS_TYPE_FCB)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    if (NtfsFCBIsDirectory(Fcb) || BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME_STREAM))
    {
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* Hand the IRP to FsRtl. From here on the IRP is owned by the
     * lock package - it will be completed (synchronously, in the
     * common case) before this call returns, or queued waiting for
     * a conflicting lock. Either way, the dispatch loop in
     * dispatch.c must not touch it. */
    Status = FsRtlProcessFileLock(&Fcb->FileLock, Irp, NULL);

    /* Tell the dispatch loop the IRP is gone. */
    IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;

    return Status;
}

/* EOF */
