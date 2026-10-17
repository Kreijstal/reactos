/*
 * PROJECT:     ReactOS NTFS filesystem driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Directory notification registration and namespace reporting
 */

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* DirResource excludes pathname changes and makes notification completion's
 * recursive cleanup/close use the same resource order as namespace updates. */
VOID
NtfsReportPathChange(PNTFS_VCB Vcb, PCWSTR Path, ULONG Filter, ULONG Action)
{
    UNICODE_STRING Name;
    PCWSTR LastPart;
    USHORT Offset;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->DirResource) != 0);
    ASSERT(Path != NULL);
    if (Vcb->Flags & VCB_DEVICE_REMOVED)
        return;
    LastPart = wcsrchr(Path, L'\\');
    ASSERT(LastPart != NULL && LastPart[1] != UNICODE_NULL);
    RtlInitUnicodeString(&Name, Path);
    Offset = (USHORT)((LastPart + 1 - Path) * sizeof(WCHAR));
    FsRtlNotifyFullReportChange(Vcb->NotifySync, &Vcb->NotifyList,
                                (PSTRING)&Name, Offset, NULL, NULL,
                                Filter, Action, NULL);
}

VOID
NtfsNotifyDirectoryDeleted(PNTFS_FCB Fcb)
{
    PNTFS_VCB Vcb = Fcb->Vcb;

    ASSERT(NtfsFCBIsDirectory(Fcb));
    ExAcquireResourceExclusiveLite(&Vcb->DirResource, TRUE);
    FsRtlNotifyFullChangeDirectory(Vcb->NotifySync, &Vcb->NotifyList, Fcb,
                                   NULL, FALSE, FALSE, 0, NULL, NULL, NULL);
    ExReleaseResourceLite(&Vcb->DirResource);
}

NTSTATUS
NtfsNotifyChangeDirectory(PNTFS_IRP_CONTEXT IrpContext)
{
    PFILE_OBJECT FileObject = IrpContext->FileObject;
    PNTFS_VCB Vcb;
    PNTFS_FCB Fcb;
    ULONG Filter = IrpContext->Stack->Parameters.NotifyDirectory.CompletionFilter;
    NTSTATUS Status;

    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject || FileObject == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;
    Fcb = FileObject->FsContext;
    ASSERT(Fcb != NULL && Fcb->Identifier.Type == NTFS_TYPE_FCB);
    if (!NtfsFCBIsDirectory(Fcb))
        return STATUS_INVALID_PARAMETER;
    if (Filter == 0 || (Filter & ~FILE_NOTIFY_VALID_MASK))
        return STATUS_INVALID_PARAMETER;
    Vcb = Fcb->Vcb;
    ExAcquireResourceExclusiveLite(&Vcb->DirResource, TRUE);
    if ((Fcb->Flags & FCB_DELETE_PENDING) && !(FileObject->Flags & FO_CLEANUP_COMPLETE))
    {
        ExReleaseResourceLite(&Vcb->DirResource);
        return STATUS_DELETE_PENDING;
    }

    /* FsRtl can complete immediately, including from a buffered change. */
    ObReferenceObject(FileObject);
    IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;
    Status = STATUS_PENDING;
    _SEH2_TRY
    {
        FsRtlNotifyFullChangeDirectory(Vcb->NotifySync, &Vcb->NotifyList,
                                       FileObject->FsContext2, (PSTRING)&Fcb->NotifyName,
                                       BooleanFlagOn(IrpContext->Stack->Flags, SL_WATCH_TREE),
                                       FALSE, Filter, IrpContext->Irp, NULL, NULL);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        IrpContext->Flags |= IRPCONTEXT_COMPLETE;
    }
    _SEH2_END;
    ExReleaseResourceLite(&Vcb->DirResource);
    ObDereferenceObject(FileObject);
    return Status;
}
