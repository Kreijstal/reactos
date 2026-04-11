/*
 *  ReactOS kernel
 *  Copyright (C) 2002, 2014 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/create.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

static PCWSTR MftIdToName[] = {
    L"$MFT",
    L"$MFTMirr",
    L"$LogFile",
    L"$Volume",
    L"AttrDef",
    L".",
    L"$Bitmap",
    L"$Boot",
    L"$BadClus",
    L"$Quota",
    L"$UpCase",
    L"$Extended",
};

/* FUNCTIONS ****************************************************************/

static
NTSTATUS
NtfsMakeAbsoluteFilename(PFILE_OBJECT pFileObject,
                         PWSTR pRelativeFileName,
                         PWSTR *pAbsoluteFilename)
{
    PWSTR rcName;
    PNTFS_FCB Fcb;

    DPRINT("try related for %S\n", pRelativeFileName);
    Fcb = pFileObject->FsContext;
    ASSERT(Fcb);

    if (Fcb->Flags & FCB_IS_VOLUME)
    {
        /* This is likely to be an opening by ID, return ourselves */
        if (pRelativeFileName[0] == L'\\')
        {
            *pAbsoluteFilename = NULL;
            return STATUS_SUCCESS;
        }

        return STATUS_INVALID_PARAMETER;
    }

    /* verify related object is a directory and target name
       don't start with \. */
    if (NtfsFCBIsDirectory(Fcb) == FALSE ||
        pRelativeFileName[0] == L'\\')
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* construct absolute path name */
    ASSERT(wcslen (Fcb->PathName) + 1 + wcslen (pRelativeFileName) + 1 <= MAX_PATH);
    rcName = ExAllocatePoolWithTag(NonPagedPool, MAX_PATH * sizeof(WCHAR), TAG_NTFS);
    if (!rcName)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    wcscpy(rcName, Fcb->PathName);
    if (!NtfsFCBIsRoot(Fcb))
        wcscat (rcName, L"\\");
    wcscat (rcName, pRelativeFileName);
    *pAbsoluteFilename = rcName;

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsMoonWalkID(PDEVICE_EXTENSION DeviceExt,
               ULONGLONG Id,
               PUNICODE_STRING OutPath)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER MftRecord;
    PFILENAME_ATTRIBUTE FileName;
    WCHAR FullPath[MAX_PATH];
    ULONG WritePosition = MAX_PATH - 1;

    DPRINT("NtfsMoonWalkID(%p, %I64x, %p)\n", DeviceExt, Id, OutPath);

    RtlZeroMemory(FullPath, sizeof(FullPath));
    MftRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (MftRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (TRUE)
    {
        Status = ReadFileRecord(DeviceExt, Id, MftRecord);
        if (!NT_SUCCESS(Status))
            break;

        ASSERT(MftRecord->Ntfs.Type == NRH_FILE_TYPE);
        if (!(MftRecord->Flags & FRH_IN_USE))
        {
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
            break;
        }

        FileName = GetBestFileNameFromRecord(DeviceExt, MftRecord);
        if (FileName == NULL)
        {
            DPRINT1("$FILE_NAME attribute not found for %I64x\n", Id);
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
            break;
        }

        WritePosition -= FileName->NameLength;
        ASSERT(WritePosition < MAX_PATH);
        RtlCopyMemory(FullPath + WritePosition, FileName->Name, FileName->NameLength * sizeof(WCHAR));
        WritePosition -= 1;
        ASSERT(WritePosition < MAX_PATH);
        FullPath[WritePosition] = L'\\';

        Id = FileName->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
        if (Id == NTFS_FILE_ROOT)
            break;
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, MftRecord);

    if (!NT_SUCCESS(Status))
        return Status;

    OutPath->Length = (MAX_PATH - WritePosition - 1) * sizeof(WCHAR);
    OutPath->MaximumLength = (MAX_PATH - WritePosition) * sizeof(WCHAR);
    OutPath->Buffer = ExAllocatePoolWithTag(NonPagedPool, OutPath->MaximumLength, TAG_NTFS);
    if (OutPath->Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(OutPath->Buffer, FullPath + WritePosition, OutPath->MaximumLength);

    return Status;
}

static
NTSTATUS
NtfsOpenFileById(PDEVICE_EXTENSION DeviceExt,
                 PFILE_OBJECT FileObject,
                 ULONGLONG MftId,
                 PNTFS_FCB * FoundFCB)
{
    NTSTATUS Status;
    PNTFS_FCB FCB;
    PFILE_RECORD_HEADER MftRecord;

    DPRINT("NtfsOpenFileById(%p, %p, %I64x, %p)\n", DeviceExt, FileObject, MftId, FoundFCB);

    ASSERT(MftId < NTFS_FILE_FIRST_USER_FILE);
    if (MftId > 0xb) /* No entries are used yet beyond this */
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    MftRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (MftRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, MftId, MftRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, MftRecord);
        return Status;
    }

    if (!(MftRecord->Flags & FRH_IN_USE))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, MftRecord);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    FCB = NtfsGrabFCBFromTable(DeviceExt, MftIdToName[MftId]);
    if (FCB == NULL)
    {
        UNICODE_STRING Name;

        RtlInitUnicodeString(&Name, MftIdToName[MftId]);
        Status = NtfsMakeFCBFromDirEntry(DeviceExt, NULL, &Name, NULL, MftRecord, MftId, &FCB);
        if (!NT_SUCCESS(Status))
        {
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, MftRecord);
            return Status;
        }
    }

    ASSERT(FCB != NULL);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, MftRecord);

    Status = NtfsAttachFCBToFileObject(DeviceExt,
                                       FCB,
                                       FileObject);
    *FoundFCB = FCB;

    return Status;
}

/*
 * FUNCTION: Opens a file
 */
static
NTSTATUS
NtfsOpenFile(PDEVICE_EXTENSION DeviceExt,
             PFILE_OBJECT FileObject,
             PWSTR FileName,
             BOOLEAN CaseSensitive,
             PNTFS_FCB * FoundFCB)
{
    PNTFS_FCB ParentFcb;
    PNTFS_FCB Fcb;
    NTSTATUS Status;
    PWSTR AbsFileName = NULL;

    DPRINT("NtfsOpenFile(%p, %p, %S, %s, %p)\n",
            DeviceExt,
            FileObject,
            FileName,
            CaseSensitive ? "TRUE" : "FALSE",
            FoundFCB);

    *FoundFCB = NULL;

    if (FileObject->RelatedFileObject)
    {
        DPRINT("Converting relative filename to absolute filename\n");

        Status = NtfsMakeAbsoluteFilename(FileObject->RelatedFileObject,
                                          FileName,
                                          &AbsFileName);
        if (AbsFileName) FileName = AbsFileName;
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }
    }

    //FIXME: Get canonical path name (remove .'s, ..'s and extra separators)

    DPRINT("PathName to open: %S\n", FileName);

    /*  try first to find an existing FCB in memory  */
    DPRINT("Checking for existing FCB in memory\n");
    Fcb = NtfsGrabFCBFromTable(DeviceExt,
                               FileName);
    if (Fcb == NULL)
    {
        DPRINT("No existing FCB found, making a new one if file exists.\n");
        Status = NtfsGetFCBForFile(DeviceExt,
                                   &ParentFcb,
                                   &Fcb,
                                   FileName,
                                   CaseSensitive);
        if (ParentFcb != NULL)
        {
            NtfsReleaseFCB(DeviceExt,
                           ParentFcb);
        }

        if (!NT_SUCCESS(Status))
        {
            DPRINT("Could not make a new FCB, status: %x\n", Status);

            if (AbsFileName)
                ExFreePoolWithTag(AbsFileName, TAG_NTFS);

            return Status;
        }
    }

    DPRINT("Attaching FCB to fileObject\n");
    Status = NtfsAttachFCBToFileObject(DeviceExt,
                                       Fcb,
                                       FileObject);

    if (AbsFileName)
        ExFreePool(AbsFileName);

    *FoundFCB = Fcb;

    return Status;
}

static
NTSTATUS
NtfsOpenTargetDirectory(PDEVICE_EXTENSION DeviceExt,
                        PFILE_OBJECT FileObject,
                        PWSTR FileName,
                        BOOLEAN CaseSensitive,
                        PNTFS_FCB *ParentFcb,
                        PULONG IoInformation)
{
    NTSTATUS Status;
    PWSTR AbsFileName = NULL;
    PNTFS_FCB TargetFcb = NULL;

    *ParentFcb = NULL;
    *IoInformation = 0;

    if (FileObject->RelatedFileObject)
    {
        Status = NtfsMakeAbsoluteFilename(FileObject->RelatedFileObject,
                                          FileName,
                                          &AbsFileName);
        if (!NT_SUCCESS(Status))
            return Status;

        if (AbsFileName != NULL)
            FileName = AbsFileName;
    }

    if (wcscmp(FileName, L"\\") == 0)
    {
        if (AbsFileName != NULL)
            ExFreePoolWithTag(AbsFileName, TAG_NTFS);
        return STATUS_INVALID_PARAMETER;
    }

    Status = NtfsGetFCBForFile(DeviceExt,
                               ParentFcb,
                               &TargetFcb,
                               FileName,
                               CaseSensitive);
    if (NT_SUCCESS(Status))
    {
        if (*ParentFcb == NULL)
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        else
        {
            *IoInformation = FILE_EXISTS;
            Status = STATUS_SUCCESS;
        }
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND && *ParentFcb != NULL)
    {
        *IoInformation = FILE_DOES_NOT_EXIST;
        Status = STATUS_SUCCESS;
    }

    if (TargetFcb != NULL)
        NtfsReleaseFCB(DeviceExt, TargetFcb);

    if (AbsFileName != NULL)
        ExFreePoolWithTag(AbsFileName, TAG_NTFS);

    return Status;
}


/*
 * FUNCTION: Opens a file
 */
static
NTSTATUS
NtfsCreateFile(PDEVICE_OBJECT DeviceObject,
               PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExt;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    ULONG RequestedDisposition;
    ULONG RequestedOptions;
    BOOLEAN OpenTargetDir;
    PNTFS_FCB Fcb = NULL;
//    PWSTR FileName;
    NTSTATUS Status;
    UNICODE_STRING FullPath;
    PIRP Irp = IrpContext->Irp;

    DPRINT("NtfsCreateFile(%p, %p) called\n", DeviceObject, IrpContext);

    DeviceExt = DeviceObject->DeviceExtension;
    ASSERT(DeviceExt);
    Stack = IoGetCurrentIrpStackLocation(Irp);
    ASSERT(Stack);

    DPRINT("NtfsCreateFile: FileName='%wZ', Disposition=%lu, Options=0x%lx\n",
            &Stack->FileObject->FileName,
            ((Stack->Parameters.Create.Options >> 24) & 0xff),
            Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS);

    RequestedDisposition = ((Stack->Parameters.Create.Options >> 24) & 0xff);
    RequestedOptions = Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS;
    OpenTargetDir = BooleanFlagOn(Stack->Flags, SL_OPEN_TARGET_DIRECTORY);
//  PagingFileCreate = (Stack->Flags & SL_OPEN_PAGING_FILE) ? TRUE : FALSE;
    if (RequestedOptions & FILE_DIRECTORY_FILE &&
        RequestedDisposition == FILE_SUPERSEDE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Deny create if the volume is locked */
    if (DeviceExt->Flags & VCB_VOLUME_LOCKED)
    {
        return STATUS_ACCESS_DENIED;
    }

    FileObject = Stack->FileObject;

    if ((RequestedOptions & FILE_OPEN_BY_FILE_ID) == FILE_OPEN_BY_FILE_ID)
    {
        ULONGLONG MFTId;

        if (FileObject->FileName.Length != sizeof(ULONGLONG))
            return STATUS_INVALID_PARAMETER;

        MFTId = (*(PULONGLONG)FileObject->FileName.Buffer) & NTFS_MFT_MASK;
        if (MFTId < NTFS_FILE_FIRST_USER_FILE)
        {
            Status = NtfsOpenFileById(DeviceExt, FileObject, MFTId, &Fcb);
        }
        else
        {
            Status = NtfsMoonWalkID(DeviceExt, MFTId, &FullPath);
        }

        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        DPRINT1("Open by ID: %I64x -> %wZ\n", (*(PULONGLONG)FileObject->FileName.Buffer) & NTFS_MFT_MASK, &FullPath);
    }

    if (OpenTargetDir)
    {
        ULONG IoInformation;

        Status = NtfsOpenTargetDirectory(DeviceExt,
                                         FileObject,
                                         ((RequestedOptions & FILE_OPEN_BY_FILE_ID) ? FullPath.Buffer : FileObject->FileName.Buffer),
                                         BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                         &Fcb,
                                         &IoInformation);

        if (RequestedOptions & FILE_OPEN_BY_FILE_ID)
        {
            ExFreePoolWithTag(FullPath.Buffer, TAG_NTFS);
        }

        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Status = NtfsAttachFCBToFileObject(DeviceExt,
                                           Fcb,
                                           FileObject);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Fcb->OpenHandleCount++;
        DeviceExt->OpenHandleCount++;
        ((PNTFS_CCB)FileObject->FsContext2)->Flags |= NTFS_CCB_FLAG_COUNTED;
        Irp->IoStatus.Information = IoInformation;
        return STATUS_SUCCESS;
    }

    /* This a open operation for the volume itself */
    if (FileObject->FileName.Length == 0 &&
        (FileObject->RelatedFileObject == NULL || FileObject->RelatedFileObject->FsContext2 != NULL))
    {
        if (RequestedDisposition != FILE_OPEN &&
            RequestedDisposition != FILE_OPEN_IF)
        {
            return STATUS_ACCESS_DENIED;
        }

        if (RequestedOptions & FILE_DIRECTORY_FILE)
        {
            return STATUS_NOT_A_DIRECTORY;
        }

        if (OpenTargetDir)
        {
            return STATUS_INVALID_PARAMETER;
        }

        NtfsAttachFCBToFileObject(DeviceExt, DeviceExt->VolumeFcb, FileObject);
        DeviceExt->VolumeFcb->RefCount++;
        /* Volume opens must be counted: NtfsCleanupFile (FCB_IS_VOLUME branch)
         * and NtfsCloseFile both decrement these on the corresponding cleanup/
         * close, so without matching increments here both counters underflow.
         * That breaks FSCTL_LOCK_VOLUME, whose check requires
         * DeviceExt->OpenHandleCount == 1 (just the locking handle). */
        DeviceExt->VolumeFcb->OpenHandleCount++;
        DeviceExt->OpenHandleCount++;
        ((PNTFS_CCB)FileObject->FsContext2)->Flags |= NTFS_CCB_FLAG_COUNTED;

        Irp->IoStatus.Information = FILE_OPENED;
        return STATUS_SUCCESS;
    }

    if (Fcb == NULL)
    {
        Status = NtfsOpenFile(DeviceExt,
                              FileObject,
                              ((RequestedOptions & FILE_OPEN_BY_FILE_ID) ? FullPath.Buffer : FileObject->FileName.Buffer),
                              BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                              &Fcb);

        if (RequestedOptions & FILE_OPEN_BY_FILE_ID)
        {
            ExFreePoolWithTag(FullPath.Buffer, TAG_NTFS);
        }
    }

    DPRINT("NtfsCreateFile: NtfsOpenFile returned Status=0x%lx, Fcb=%p\n", Status, Fcb);

    if (NT_SUCCESS(Status))
    {
        if (RequestedDisposition == FILE_CREATE)
        {
            Irp->IoStatus.Information = FILE_EXISTS;
            NtfsCloseFile(DeviceExt, FileObject);
            return STATUS_OBJECT_NAME_COLLISION;
        }

        if (RequestedOptions & FILE_NON_DIRECTORY_FILE &&
            NtfsFCBIsDirectory(Fcb))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return STATUS_FILE_IS_A_DIRECTORY;
        }

        if (RequestedOptions & FILE_DIRECTORY_FILE &&
            !NtfsFCBIsDirectory(Fcb))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return STATUS_NOT_A_DIRECTORY;
        }

        /*
         * If it is a reparse point & FILE_OPEN_REPARSE_POINT, then allow
         * opening it as a normal file.  Otherwise, read the reparse data
         * and hand it to the I/O manager with STATUS_REPARSE so the
         * upper layer can resolve the reparse.
         *
         * Return STATUS_REPARSE for any reparse tag (mount point,
         * symlink, third-party, etc.) — the tag goes into
         * IoStatus.Information and the reparse data stays in
         * Irp->Tail.Overlay.AuxiliaryBuffer for the I/O manager, which
         * owns (and frees) that buffer once the reparse is handled.
         * It is not our job to whitelist tags here.
         */
        if (NtfsFCBIsReparsePoint(Fcb) &&
            ((RequestedOptions & FILE_OPEN_REPARSE_POINT) != FILE_OPEN_REPARSE_POINT))
        {
            PREPARSE_DATA_BUFFER ReparseData = NULL;

            Status = NtfsReadFCBAttribute(DeviceExt, Fcb,
                                          AttributeReparsePoint, L"", 0,
                                          (PVOID *)&Irp->Tail.Overlay.AuxiliaryBuffer);
            if (NT_SUCCESS(Status))
            {
                ReparseData = (PREPARSE_DATA_BUFFER)Irp->Tail.Overlay.AuxiliaryBuffer;
                Irp->IoStatus.Information = ReparseData->ReparseTag;
                Status = STATUS_REPARSE;
            }
            else
            {
                Irp->IoStatus.Information = 0;
            }

            NtfsCloseFile(DeviceExt, FileObject);
            return Status;
        }

        if (RequestedDisposition == FILE_OVERWRITE ||
            RequestedDisposition == FILE_OVERWRITE_IF ||
            RequestedDisposition == FILE_SUPERSEDE)
        {
            PFILE_RECORD_HEADER fileRecord = NULL;
            PNTFS_ATTR_CONTEXT dataContext = NULL;
            ULONG DataAttributeOffset;
            LARGE_INTEGER Zero;
            Zero.QuadPart = 0;

            if (!NtfsGlobalData->EnableWriteSupport)
            {
                DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
                NtfsCloseFile(DeviceExt, FileObject);
                return STATUS_ACCESS_DENIED;
            }

            // TODO: check for appropriate access

            ExAcquireResourceExclusiveLite(&(Fcb->MainResource), TRUE);

            fileRecord = ExAllocateFromNPagedLookasideList(&Fcb->Vcb->FileRecLookasideList);
            if (fileRecord)
            {

                Status = ReadFileRecord(Fcb->Vcb,
                                        Fcb->MFTIndex,
                                        fileRecord);
                if (!NT_SUCCESS(Status))
                    goto DoneOverwriting;

                // find the data attribute and set it's length to 0 (TODO: Handle Alternate Data Streams)
                Status = FindAttribute(Fcb->Vcb, fileRecord, AttributeData, L"", 0, &dataContext, &DataAttributeOffset);
                if (!NT_SUCCESS(Status))
                    goto DoneOverwriting;

                Status = SetAttributeDataLength(FileObject, Fcb, dataContext, DataAttributeOffset, fileRecord, &Zero);
            }
            else
            {
                Status = STATUS_NO_MEMORY;
            }

        DoneOverwriting:
            if (fileRecord)
                ExFreeToNPagedLookasideList(&Fcb->Vcb->FileRecLookasideList, fileRecord);
            if (dataContext)
                ReleaseAttributeContext(dataContext);

            ExReleaseResourceLite(&(Fcb->MainResource));

            if (!NT_SUCCESS(Status))
            {
                NtfsCloseFile(DeviceExt, FileObject);
                return Status;
            }

            if (RequestedDisposition == FILE_SUPERSEDE)
            {
                Irp->IoStatus.Information = FILE_SUPERSEDED;
            }
            else
            {
                Irp->IoStatus.Information = FILE_OVERWRITTEN;
            }
        }
    }
    else
    {
        /* HUGLY HACK: Can't create new files yet... */
        if (RequestedDisposition == FILE_CREATE ||
            RequestedDisposition == FILE_OPEN_IF ||
            RequestedDisposition == FILE_OVERWRITE_IF ||
            RequestedDisposition == FILE_SUPERSEDE)
        {
            if (!NtfsGlobalData->EnableWriteSupport)
            {
                DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
                NtfsCloseFile(DeviceExt, FileObject);
                return STATUS_ACCESS_DENIED;
            }

            // Was the user trying to create a directory?
            if (RequestedOptions & FILE_DIRECTORY_FILE)
            {
                DPRINT("NtfsCreateFile: Creating directory...\n");
                // Create the directory on disk
                Status = NtfsCreateDirectory(DeviceExt,
                                             FileObject,
                                             BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                             BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));
                DPRINT("NtfsCreateFile: NtfsCreateDirectory returned 0x%lx\n", Status);
            }
            else
            {
                // Create the file record on disk
                Status = NtfsCreateFileRecord(DeviceExt,
                                              FileObject,
                                              BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                              BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));
            }

            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR: Couldn't create file record! Status = 0x%lx\n", Status);
                return Status;
            }

            // Before we open the file/directory we just created, we need to change the disposition (upper 8 bits of ULONG)
            // from create to open, since we already created the file
            Stack->Parameters.Create.Options = (ULONG)FILE_OPEN << 24 | RequestedOptions;

            // Now we should be able to open the file using NtfsCreateFile()
            DPRINT("NtfsCreateFile: Recursive call to re-open created file/dir\n");
            Status = NtfsCreateFile(DeviceObject, IrpContext);
            if (NT_SUCCESS(Status))
            {
                // We need to change Irp->IoStatus.Information to reflect creation
                Irp->IoStatus.Information = FILE_CREATED;
            }
            return Status;
        }
    }

    if (NT_SUCCESS(Status))
    {
        ACCESS_MASK DesiredAccess;
        ULONG ShareAccess;

        DesiredAccess = Stack->Parameters.Create.SecurityContext != NULL
            ? Stack->Parameters.Create.SecurityContext->DesiredAccess
            : 0;
        ShareAccess = Stack->Parameters.Create.ShareAccess;

        /* Maintain SHARE_ACCESS so the MM filter callback can tell how
         * many writers a file has, and so concurrent opens of the same
         * file get the share-mode semantics callers depend on (e.g. the
         * loader opens images with FILE_SHARE_READ|FILE_SHARE_DELETE and
         * expects subsequent writers to be rejected).
         *
         * Hold Fcb->MainResource exclusive around the ShareAccess update.
         * This is the same resource NtfsFilterCallbackAcquireForCreateSection
         * acquires when MM probes for writers via
         * PreAcquireForSectionSynchronization, and the same resource
         * NtfsCleanupFile holds around IoRemoveShareAccess.  Without it the
         * filter callback can observe a stale Writers count and grant /
         * reject a section creation mid-update (issue #11 P1 follow-up).
         *
         * Lock order: DirResource (held by NtfsCreate) -> MainResource here.
         * Matches NtfsCleanup -> NtfsCleanupFile.  The filter callback
         * never acquires DirResource, so no AB-BA is possible. */
        if (Fcb->Identifier.Type == NTFS_TYPE_FCB &&
            !BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME))
        {
            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            if (Fcb->OpenHandleCount == 0)
            {
                /* First open of this FCB — establish share access. */
                IoSetShareAccess(DesiredAccess,
                                 ShareAccess,
                                 FileObject,
                                 &Fcb->ShareAccess);
            }
            else
            {
                /* Subsequent open — must be compatible with existing
                 * sharing.  IoCheckShareAccess validates and (when the
                 * fourth argument is TRUE) updates the access counters
                 * atomically. */
                Status = IoCheckShareAccess(DesiredAccess,
                                            ShareAccess,
                                            FileObject,
                                            &Fcb->ShareAccess,
                                            TRUE);
            }
            ExReleaseResourceLite(&Fcb->MainResource);

            if (!NT_SUCCESS(Status))
            {
                DPRINT1("NtfsCreateFile: share access conflict for FCB %p (Desired=0x%lx Share=0x%lx): 0x%lx\n",
                        Fcb, DesiredAccess, ShareAccess, Status);
                NtfsCloseFile(DeviceExt, FileObject);
                return Status;
            }
        }

        Fcb->OpenHandleCount++;
        DeviceExt->OpenHandleCount++;
        ((PNTFS_CCB)FileObject->FsContext2)->Flags |= NTFS_CCB_FLAG_COUNTED;
    }

    /*
     * If the directory containing the file to open doesn't exist then
     * fail immediately
     */
    Irp->IoStatus.Information = (NT_SUCCESS(Status)) ? FILE_OPENED : 0;

    return Status;
}


NTSTATUS
NtfsCreate(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExt;
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject;

    DeviceObject = IrpContext->DeviceObject;
    if (DeviceObject == NtfsGlobalData->DeviceObject)
    {
        /* DeviceObject represents FileSystem instead of logical volume */
        DPRINT1("NtfsCreate: Opening file system device object\n");
        IrpContext->Irp->IoStatus.Information = FILE_OPENED;
        return STATUS_SUCCESS;
    }

    DeviceExt = DeviceObject->DeviceExtension;
    DPRINT("NtfsCreate: Processing IRP for volume, FileName='%wZ'\n",
            &IoGetCurrentIrpStackLocation(IrpContext->Irp)->FileObject->FileName);

    if (!(IrpContext->Flags & IRPCONTEXT_CANWAIT))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    ExAcquireResourceExclusiveLite(&DeviceExt->DirResource,
                                   TRUE);
    Status = NtfsCreateFile(DeviceObject,
                            IrpContext);
    ExReleaseResourceLite(&DeviceExt->DirResource);

    return Status;
}

/**
* @name NtfsCreateDirectory()
* @implemented
*
* Creates a file record for a new directory and saves it to the MFT. Adds the filename attribute of the
* created directory to the parent directory's index.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param FileObject
* Pointer to a FILE_OBJECT describing the directory to be created
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the folder with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param CanWait
* Boolean indicating if the function is allowed to wait for exclusive access to the master file table.
* This will only be relevant if the MFT doesn't have any free file records and needs to be enlarged.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if unable to allocate memory for the file record.
* STATUS_CANT_WAIT if CanWait was FALSE and the function needed to resize the MFT but
* couldn't get immediate, exclusive access to it.
*/
NTSTATUS
NtfsCreateDirectory(PDEVICE_EXTENSION DeviceExt,
                    PFILE_OBJECT FileObject,
                    BOOLEAN CaseSensitive,
                    BOOLEAN CanWait)
{

    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD NextAttribute;
    PFILENAME_ATTRIBUTE FilenameAttribute;
    ULONGLONG ParentMftIndex;
    ULONGLONG FileMftIndex;
    PB_TREE Tree;
    PINDEX_ROOT_ATTRIBUTE NewIndexRoot;
    ULONG MaxIndexRootSize;
    ULONG RootLength;

    DPRINT("NtfsCreateFileRecord(%p, %p, %s, %s)\n",
            DeviceExt,
            FileObject,
            CaseSensitive ? "TRUE" : "FALSE",
            CanWait ? "TRUE" : "FALSE");

    // Start with an empty file record
    FileRecord = NtfsCreateEmptyFileRecord(DeviceExt);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Set the directory flag
    FileRecord->Flags |= FRH_DIRECTORY;

    // find where the first attribute will be added
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

    // add first attribute, $STANDARD_INFORMATION
    AddStandardInformation(FileRecord, NextAttribute);

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
    Status = AddFileName(FileRecord, NextAttribute, DeviceExt, FileObject, CaseSensitive, &ParentMftIndex);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // save a pointer to the filename attribute
    FilenameAttribute = (PFILENAME_ATTRIBUTE)((ULONG_PTR)NextAttribute + NextAttribute->Resident.ValueOffset);

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Create an empty b-tree to represent our new index
    Status = CreateEmptyBTree(&Tree);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to create empty B-Tree!\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // Calculate maximum size of index root
    MaxIndexRootSize = DeviceExt->NtfsInfo.BytesPerFileRecord
                       - ((ULONG_PTR)NextAttribute - (ULONG_PTR)FileRecord)
                       - sizeof(ULONG) * 2;

    // Create a new index record from the tree
    Status = CreateIndexRootFromBTree(DeviceExt,
                                      Tree,
                                      MaxIndexRootSize,
                                      &NewIndexRoot,
                                      &RootLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Unable to create empty index root!\n");
        DestroyBTree(Tree);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // We're done with the B-Tree
    DestroyBTree(Tree);

    // add the $INDEX_ROOT attribute
    Status = AddIndexRoot(DeviceExt, FileRecord, NextAttribute, NewIndexRoot, RootLength, L"$I30", 4);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to add index root to new file record!\n");
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }


#ifndef NDEBUG
    NtfsDumpFileRecord(DeviceExt, FileRecord);
#endif

    // Now that we've built the file record in memory, we need to store it in the MFT.
    Status = AddNewMftEntry(FileRecord, DeviceExt, &FileMftIndex, CanWait);
    if (NT_SUCCESS(Status))
    {
        // The highest 2 bytes should be the sequence number, unless the parent happens to be root
        if (FileMftIndex == NTFS_FILE_ROOT)
            FileMftIndex = FileMftIndex + ((ULONGLONG)NTFS_FILE_ROOT << 48);
        else
            FileMftIndex = FileMftIndex + ((ULONGLONG)FileRecord->SequenceNumber << 48);

        DPRINT("New File Reference: 0x%016I64x\n", FileMftIndex);

        // Add the filename attribute to the filename-index of the parent directory
        Status = NtfsAddFilenameToDirectory(DeviceExt,
                                            ParentMftIndex,
                                            FileMftIndex,
                                            FilenameAttribute,
                                            CaseSensitive);
    }

    ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsCreateEmptyFileRecord
* @implemented
*
* Creates a new, empty file record, with no attributes.
*
* @param DeviceExt
* Pointer to the DEVICE_EXTENSION of the target volume the file record will be stored on.
*
* @return
* A pointer to the newly-created FILE_RECORD_HEADER if the function succeeds, NULL otherwise.
*/
PFILE_RECORD_HEADER
NtfsCreateEmptyFileRecord(PDEVICE_EXTENSION DeviceExt)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD NextAttribute;

    DPRINT("NtfsCreateEmptyFileRecord(%p)\n", DeviceExt);

    // allocate memory for file record
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return NULL;
    }

    RtlZeroMemory(FileRecord, DeviceExt->NtfsInfo.BytesPerFileRecord);

    FileRecord->Ntfs.Type = NRH_FILE_TYPE;

    // calculate USA offset and count
    FileRecord->Ntfs.UsaOffset = FIELD_OFFSET(FILE_RECORD_HEADER, MFTRecordNumber) + sizeof(ULONG);

    // size of USA (in ULONG's) will be 1 (for USA number) + 1 for every sector the file record uses
    FileRecord->BytesAllocated = DeviceExt->NtfsInfo.BytesPerFileRecord;
    FileRecord->Ntfs.UsaCount = (FileRecord->BytesAllocated / DeviceExt->NtfsInfo.BytesPerSector) + 1;

    // setup other file record fields
    FileRecord->SequenceNumber = 1;
    FileRecord->AttributeOffset = FileRecord->Ntfs.UsaOffset + (2 * FileRecord->Ntfs.UsaCount);
    FileRecord->AttributeOffset = ALIGN_UP_BY(FileRecord->AttributeOffset, ATTR_RECORD_ALIGNMENT);
    FileRecord->Flags = FRH_IN_USE;
    FileRecord->BytesInUse = FileRecord->AttributeOffset + sizeof(ULONG) * 2;

    // find where the first attribute will be added
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

    // mark the (temporary) end of the file-record
    NextAttribute->Type = AttributeEnd;
    NextAttribute->Length = FILE_RECORD_END;

    return FileRecord;
}


/**
* @name NtfsCreateFileRecord()
* @implemented
*
* Creates a file record and saves it to the MFT. Adds the filename attribute of the
* created file to the parent directory's index.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param FileObject
* Pointer to a FILE_OBJECT describing the file to be created
*
* @param CanWait
* Boolean indicating if the function is allowed to wait for exclusive access to the master file table.
* This will only be relevant if the MFT doesn't have any free file records and needs to be enlarged.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if unable to allocate memory for the file record.
* STATUS_CANT_WAIT if CanWait was FALSE and the function needed to resize the MFT but
* couldn't get immediate, exclusive access to it.
*/
NTSTATUS
NtfsCreateFileRecord(PDEVICE_EXTENSION DeviceExt,
                     PFILE_OBJECT FileObject,
                     BOOLEAN CaseSensitive,
                     BOOLEAN CanWait)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD NextAttribute;
    PFILENAME_ATTRIBUTE FilenameAttribute;
    ULONGLONG ParentMftIndex;
    ULONGLONG FileMftIndex;

    DPRINT("NtfsCreateFileRecord(%p, %p, %s, %s)\n",
            DeviceExt,
            FileObject,
            CaseSensitive ? "TRUE" : "FALSE",
            CanWait ? "TRUE" : "FALSE");

    // allocate memory for file record
    FileRecord = NtfsCreateEmptyFileRecord(DeviceExt);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // find where the first attribute will be added
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

    // add first attribute, $STANDARD_INFORMATION
    AddStandardInformation(FileRecord, NextAttribute);

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
    Status = AddFileName(FileRecord, NextAttribute, DeviceExt, FileObject, CaseSensitive, &ParentMftIndex);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // save a pointer to the filename attribute
    FilenameAttribute = (PFILENAME_ATTRIBUTE)((ULONG_PTR)NextAttribute + NextAttribute->Resident.ValueOffset);

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // add the $DATA attribute
    AddData(FileRecord, NextAttribute);

#ifndef NDEBUG
    // dump file record in memory (for debugging)
    NtfsDumpFileRecord(DeviceExt, FileRecord);
#endif

    // Now that we've built the file record in memory, we need to store it in the MFT.
    Status = AddNewMftEntry(FileRecord, DeviceExt, &FileMftIndex, CanWait);
    if (NT_SUCCESS(Status))
    {
        // The highest 2 bytes should be the sequence number, unless the parent happens to be root
        if (FileMftIndex == NTFS_FILE_ROOT)
            FileMftIndex = FileMftIndex + ((ULONGLONG)NTFS_FILE_ROOT << 48);
        else
            FileMftIndex = FileMftIndex + ((ULONGLONG)FileRecord->SequenceNumber << 48);

        DPRINT("New File Reference: 0x%016I64x\n", FileMftIndex);

        // Add the filename attribute to the filename-index of the parent directory
        Status = NtfsAddFilenameToDirectory(DeviceExt,
                                            ParentMftIndex,
                                            FileMftIndex,
                                            FilenameAttribute,
                                            CaseSensitive);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/* EOF */
