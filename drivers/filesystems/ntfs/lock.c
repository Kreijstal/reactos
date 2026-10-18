/*
 * PROJECT:     ReactOS NTFS filesystem driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Byte-range locks shared by every path to a data stream
 */

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
NtfsCompleteLockIrp(PVOID Context, PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PNTFS_FCB Fcb = FileObject->FsContext;
    NTSTATUS Status = Irp->IoStatus.Status;

    UNREFERENCED_PARAMETER(Context);
    ASSERT(Fcb != NULL);
    if (Fcb->Vcb->Flags & VCB_DEVICE_REMOVED)
        Status = STATUS_DEVICE_REMOVED;
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
    return Status;
}

/* Called with removal owning IoResource and DirResource exclusively. Close
 * from a completed waiter can reenter, so pin one context at a time and restart
 * the list search after completion instead of retaining an unpinned next link. */
VOID
NtfsRemoveFileLocks(PNTFS_VCB Vcb)
{
    PNTFS_FILE_LOCK FileLock, Found;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    for (;;)
    {
        Found = NULL;
        KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);
        for (Entry = Vcb->FileLockList.Flink; Entry != &Vcb->FileLockList; Entry = Entry->Flink)
        {
            FileLock = CONTAINING_RECORD(Entry, NTFS_FILE_LOCK, ListEntry);
            if (!FileLock->Removed)
            {
                FileLock->Removed = TRUE;
                FileLock->References++;
                Found = FileLock;
                break;
            }
        }
        KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
        if (Found == NULL)
            break;
        ExAcquireResourceExclusiveLite(&Found->Resource, TRUE);
        FsRtlUninitializeFileLock(&Found->Lock);
        /* Cleanup and final close still use a valid, empty FILE_LOCK. */
        FsRtlInitializeFileLock(&Found->Lock, NtfsCompleteLockIrp, NULL);
        ExReleaseResourceLite(&Found->Resource);
        NtfsReleaseFileLock(Found);
    }
}

BOOLEAN
NtfsCheckFileLock(PNTFS_FCB Fcb,
                  PIRP Irp,
                  PLARGE_INTEGER Offset,
                  ULONG Length,
                  BOOLEAN Write)
{
    PNTFS_FILE_LOCK FileLock = Fcb->FileLock;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    LARGE_INTEGER LockLength;
    BOOLEAN Allowed;

    /* Mapped access is not subject to byte locks. Its paging requests must
     * still populate and write back pages intersecting locked ranges. */
    if ((Irp->Flags & IRP_PAGING_IO) || Length == 0)
        return TRUE;
    ASSERT(FileLock != NULL);
    LockLength.QuadPart = Length;

    /* FsRtl's current range table needs external serialization, including
     * lookup. The resource belongs to the stream, not a hard-link path. */
    ExAcquireResourceExclusiveLite(&FileLock->Resource, TRUE);
    if (Write)
    {
        Allowed = FsRtlFastCheckLockForWrite(&FileLock->Lock, Offset, &LockLength,
                                            Stack->Parameters.Write.Key, Stack->FileObject,
                                            IoGetRequestorProcess(Irp));
    }
    else
    {
        Allowed = FsRtlFastCheckLockForRead(&FileLock->Lock, Offset, &LockLength,
                                           Stack->Parameters.Read.Key, Stack->FileObject,
                                           IoGetRequestorProcess(Irp));
    }
    ExReleaseResourceLite(&FileLock->Resource);
    return Allowed;
}

/* FileLock->Resource serializes this list, including across hard-link FCBs. */
static
NTSTATUS
NtfsRememberLockOwner(PFILE_OBJECT FileObject, PEPROCESS Process)
{
    PNTFS_CCB Ccb = FileObject->FsContext2;
    PNTFS_LOCK_OWNER Owner;

    ASSERT(Ccb != NULL);
    for (Owner = Ccb->LockOwners; Owner != NULL; Owner = Owner->Next)
    {
        if (Owner->Process == Process)
            return STATUS_SUCCESS;
    }
    Owner = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Owner), TAG_CCB);
    if (Owner == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Owner->Process = Process;
    if (Process != NULL)
        ObReferenceObject(Process);
    Owner->Next = Ccb->LockOwners;
    Ccb->LockOwners = Owner;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsLockControl(PNTFS_IRP_CONTEXT IrpContext)
{
    PFILE_OBJECT FileObject = IrpContext->FileObject;
    PNTFS_FCB Fcb;
    PNTFS_VCB Vcb;
    PNTFS_FILE_LOCK FileLock;
    NTSTATUS Status;

    IrpContext->Irp->IoStatus.Information = 0;
    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject || FileObject == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;
    Fcb = FileObject->FsContext;
    ASSERT(Fcb != NULL && Fcb->Identifier.Type == NTFS_TYPE_FCB);
    if (NtfsFCBIsDirectory(Fcb))
        return STATUS_INVALID_PARAMETER;
    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
        return STATUS_INVALID_DEVICE_REQUEST;
    if (FileObject->Flags & FO_CLEANUP_COMPLETE)
        return STATUS_FILE_CLOSED;

    Vcb = Fcb->Vcb;
    /* Completion may close the last file object. Keep the same ordering as
     * cleanup/removal, including any recursive close from the callback. */
    ExAcquireResourceExclusiveLite(&Vcb->DirResource, TRUE);
    FileLock = Fcb->FileLock;
    ASSERT(FileLock != NULL);
    /* Completion can release the last file-object/FCB reference before the
     * helper returns. Keep the shared lock and its resource alive separately. */
    NtfsReferenceFileLock(FileLock);
    ExAcquireResourceExclusiveLite(&FileLock->Resource, TRUE);
    if (IrpContext->MinorFunction == IRP_MN_LOCK)
    {
        Status = NtfsRememberLockOwner(FileObject, IoGetRequestorProcess(IrpContext->Irp));
        if (!NT_SUCCESS(Status))
            goto Unlock;
        FileObject->LockOperation = TRUE;
    }
    IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;
    Status = FsRtlProcessFileLock(&FileLock->Lock, IrpContext->Irp, NULL);
Unlock:
    ExReleaseResourceLite(&FileLock->Resource);
    NtfsReleaseFileLock(FileLock);
    ExReleaseResourceLite(&Vcb->DirResource);
    return Status;
}
