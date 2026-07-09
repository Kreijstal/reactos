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
             BOOLEAN VolumeAbsolute,
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

    /* An open-by-file-ID name has already been resolved to a volume-absolute
     * path; its RelatedFileObject only located the volume (Windows accepts
     * any handle on it, directory or not) and takes no part in the walk. */
    if (FileObject->RelatedFileObject && !VolumeAbsolute)
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
                        BOOLEAN VolumeAbsolute,
                        PNTFS_FCB *ParentFcb,
                        PULONG IoInformation)
{
    NTSTATUS Status;
    PWSTR AbsFileName = NULL;
    PNTFS_FCB TargetFcb = NULL;

    *ParentFcb = NULL;
    *IoInformation = 0;

    if (FileObject->RelatedFileObject && !VolumeAbsolute)
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


/**
* @name NtfsCreateStream
* @implemented
*
* Creates a named $DATA attribute (alternate data stream) for the file named
* by the plain part of FileObject->FileName.  If the base file itself does
* not exist it is created first (Windows semantics: FILE_CREATE of
* "newfile:stream" creates the base file AND the stream in one call).  The
* new stream is resident and empty; growth happens through the ordinary
* SetAttributeDataLength machinery once the caller re-opens and writes.
*
* FileObject->FileName must be the normalized "path\\file:stream" form
* produced by NtfsParseStreamPath; it is temporarily trimmed to the plain
* file part (both the base lookup and NtfsCreateFileRecord->AddFileName
* consume it) and restored - including the colon - before returning, so the
* caller's recursive re-open sees the full stream name again.
*/
static
NTSTATUS
NtfsCreateStream(PDEVICE_EXTENSION DeviceExt,
                 PFILE_OBJECT FileObject,
                 PUNICODE_STRING StreamName,
                 BOOLEAN CaseSensitive,
                 ULONG FileAttributes,
                 BOOLEAN CanWait)
{
    NTSTATUS Status;
    USHORT SavedLength;
    ULONG ColonIndex;
    PWSTR BaseName;
    PWSTR AbsName = NULL;
    PNTFS_FCB BaseFcb = NULL;
    PNTFS_FCB ParentFcb = NULL;
    PFILE_RECORD_HEADER FileRecord = NULL;

    ASSERT(StreamName->Length != 0);
    ASSERT(FileObject->FileName.Length > StreamName->Length);

    /* Trim "path\file:stream" to "path\file" for the duration. */
    SavedLength = FileObject->FileName.Length;
    ColonIndex = (SavedLength - StreamName->Length) / sizeof(WCHAR) - 1;
    ASSERT(FileObject->FileName.Buffer[ColonIndex] == L':');
    FileObject->FileName.Length = (USHORT)(ColonIndex * sizeof(WCHAR));
    FileObject->FileName.Buffer[ColonIndex] = UNICODE_NULL;

    /* Resolve the base file (handling relative opens like NtfsOpenFile). */
    BaseName = FileObject->FileName.Buffer;
    if (FileObject->RelatedFileObject)
    {
        Status = NtfsMakeAbsoluteFilename(FileObject->RelatedFileObject,
                                          BaseName,
                                          &AbsName);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        if (AbsName != NULL)
            BaseName = AbsName;
    }

    BaseFcb = NtfsGrabFCBFromTable(DeviceExt, BaseName);
    if (BaseFcb == NULL)
    {
        Status = NtfsGetFCBForFile(DeviceExt, &ParentFcb, &BaseFcb, BaseName, CaseSensitive);
        if (ParentFcb != NULL)
        {
            NtfsReleaseFCB(DeviceExt, ParentFcb);
            ParentFcb = NULL;
        }

        if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        {
            /* The base file doesn't exist either - create it along with the
             * stream, as Windows does. */
            Status = NtfsCreateFileRecord(DeviceExt,
                                          FileObject,
                                          CaseSensitive,
                                          FileAttributes,
                                          CanWait);
            if (!NT_SUCCESS(Status))
                goto Cleanup;

            Status = NtfsGetFCBForFile(DeviceExt, &ParentFcb, &BaseFcb, BaseName, CaseSensitive);
            if (ParentFcb != NULL)
            {
                NtfsReleaseFCB(DeviceExt, ParentFcb);
                ParentFcb = NULL;
            }
        }

        if (!NT_SUCCESS(Status))
        {
            BaseFcb = NULL;
            goto Cleanup;
        }
    }

    if (NtfsFCBIsDirectory(BaseFcb))
    {
        /* NTFS allows named $DATA streams on directories, but this driver's
         * directory FCBs have no data-stream cache plumbing, so directory
         * ADS is explicitly out of scope - fail the name cleanly. */
        Status = STATUS_OBJECT_NAME_INVALID;
        goto Cleanup;
    }

    /* Insert the (empty, resident) named $DATA attribute. */
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Status = ReadFileRecord(DeviceExt, BaseFcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = AddDataStream(DeviceExt,
                           FileRecord,
                           StreamName->Buffer,
                           (USHORT)(StreamName->Length / sizeof(WCHAR)));
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = UpdateFileRecord(DeviceExt, BaseFcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        /* The base FCB may cache a copy of the record without the stream. */
        NtfsInvalidateCachedFileRecord(BaseFcb);
    }

Cleanup:
    if (FileRecord != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (BaseFcb != NULL)
        NtfsReleaseFCB(DeviceExt, BaseFcb);

    if (AbsName != NULL)
        ExFreePoolWithTag(AbsName, TAG_NTFS);

    /* Restore the full "path\file:stream" name for the re-open. */
    FileObject->FileName.Buffer[ColonIndex] = L':';
    FileObject->FileName.Length = SavedLength;

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
    BOOLEAN TrailingBackslash = FALSE;
    PNTFS_FCB Fcb = NULL;
//    PWSTR FileName;
    NTSTATUS Status;
    UNICODE_STRING FullPath;
    UNICODE_STRING StreamName;
    NTFS_STREAM_TYPE StreamType = NtfsStreamTypeData;
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

    /* A trailing backslash names a directory; drop it so the file object's
     * stored name matches a plain open of that directory (a later
     * FileNameInformation query or name compare must not see the separator).
     * fastfat does the same in FatCommonCreate. Never touch the lone
     * backslash of the volume root, and leave open-by-id names alone.
     * Remember the strip: the separator promises a directory, so once the
     * target resolves to anything else the open must fail with
     * STATUS_OBJECT_NAME_INVALID (checked below, like fastfat's
     * TrailingBackslash). */
    if (!(RequestedOptions & FILE_OPEN_BY_FILE_ID) &&
        FileObject->FileName.Length > sizeof(WCHAR) &&
        FileObject->FileName.Buffer[FileObject->FileName.Length / sizeof(WCHAR) - 1] == L'\\')
    {
        FileObject->FileName.Length -= sizeof(WCHAR);
        FileObject->FileName.Buffer[FileObject->FileName.Length / sizeof(WCHAR)] = UNICODE_NULL;
        TrailingBackslash = TRUE;
    }

    /* Split and validate an alternate-data-stream suffix (":Stream[:$TYPE]")
     * once, before any path walk.  This normalizes FileObject->FileName in
     * place: the type suffix is stripped ("file::$DATA" == "file",
     * "dir::$INDEX_ALLOCATION" == "dir"), so the FCB table key, the
     * directory walk and the create path below all see at most one colon
     * separating the file part from the stream name.  StreamType keeps the
     * typed-open requirement ($INDEX_ALLOCATION demands a directory target,
     * an explicit "::$DATA" a non-directory), enforced once the target has
     * resolved.  Invalid stream syntax fails the create up front, like
     * Windows. */
    RtlInitEmptyUnicodeString(&StreamName, NULL, 0);
    if (!(RequestedOptions & FILE_OPEN_BY_FILE_ID) &&
        FileObject->FileName.Length != 0)
    {
        Status = NtfsParseStreamPath(&FileObject->FileName, &StreamName, &StreamType);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        /* A "::$INDEX_ALLOCATION" open demands a directory target, which
         * the FILE_NON_DIRECTORY_FILE option contradicts outright. */
        if (StreamType == NtfsStreamTypeIndexAllocation &&
            BooleanFlagOn(RequestedOptions, FILE_NON_DIRECTORY_FILE))
        {
            return STATUS_NOT_A_DIRECTORY;
        }
    }

    if (OpenTargetDir)
    {
        ULONG IoInformation;

        Status = NtfsOpenTargetDirectory(DeviceExt,
                                         FileObject,
                                         ((RequestedOptions & FILE_OPEN_BY_FILE_ID) ? FullPath.Buffer : FileObject->FileName.Buffer),
                                         BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                         BooleanFlagOn(RequestedOptions, FILE_OPEN_BY_FILE_ID),
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

        /*
         * For a target-directory open the file object now refers to the parent
         * directory, so its name must read as that directory's name (matching a
         * plain open of the same directory) rather than the full path that was
         * requested. The opened parent FCB carries that path; copy it over the
         * existing name buffer when it fits (the parent path is a prefix of the
         * absolute request, and shorter than the relative leaf in practice).
         */
        {
            USHORT ParentNameLength = (USHORT)(wcslen(Fcb->PathName) * sizeof(WCHAR));

            if (ParentNameLength <= FileObject->FileName.MaximumLength)
            {
                RtlCopyMemory(FileObject->FileName.Buffer, Fcb->PathName, ParentNameLength);
                FileObject->FileName.Length = ParentNameLength;
            }
        }

        Fcb->OpenHandleCount++;
        DeviceExt->OpenHandleCount++;
        ((PNTFS_CCB)FileObject->FsContext2)->Flags |= NTFS_CCB_FLAG_COUNTED;
        Irp->IoStatus.Information = IoInformation;
        return STATUS_SUCCESS;
    }

    /* A relative open with an empty name whose RootDirectory is an existing
     * FILE (not the volume) is a reopen-by-handle: reopen the very same file
     * with the newly requested access and options.  cygwin/msys2's unlink_nt
     * uses this (init_reopen_attr) to retry a delete with FILE_DELETE_ON_CLOSE
     * after a plain delete disposition returned STATUS_CANNOT_DELETE.  Resolve
     * the related FCB and let the normal existing-file path below run so that
     * share access, the image-section check and FILE_DELETE_ON_CLOSE all apply.
     * Without this the open falls into the volume-open path and wrongly
     * succeeds against the volume, so the in-use file is never moved aside
     * (e.g. replacing a running msys-2.0.dll during pacman fails with
     * "Can't create"). */
    if (Fcb == NULL &&
        FileObject->FileName.Length == 0 &&
        FileObject->RelatedFileObject != NULL &&
        FileObject->RelatedFileObject->FsContext != NULL &&
        ((PNTFS_FCB)FileObject->RelatedFileObject->FsContext)->Identifier.Type == NTFS_TYPE_FCB &&
        !BooleanFlagOn(((PNTFS_FCB)FileObject->RelatedFileObject->FsContext)->Flags, FCB_IS_VOLUME))
    {
        PNTFS_FCB RelatedFcb = FileObject->RelatedFileObject->FsContext;

        Fcb = NtfsGrabFCBFromTable(DeviceExt, RelatedFcb->PathName);
        if (Fcb != NULL)
        {
            Status = NtfsAttachFCBToFileObject(DeviceExt, Fcb, FileObject);
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    /* This a open operation for the volume itself */
    if (Fcb == NULL &&
        FileObject->FileName.Length == 0 &&
        (FileObject->RelatedFileObject == NULL || FileObject->RelatedFileObject->FsContext2 != NULL))
    {
        if (RequestedDisposition != FILE_OPEN &&
            RequestedDisposition != FILE_OPEN_IF)
        {
            return STATUS_ACCESS_DENIED;
        }

        /* The volume is no directory - neither for FILE_DIRECTORY_FILE nor
         * for a typed "::$INDEX_ALLOCATION" open of it. */
        if ((RequestedOptions & FILE_DIRECTORY_FILE) ||
            StreamType == NtfsStreamTypeIndexAllocation)
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
                              BooleanFlagOn(RequestedOptions, FILE_OPEN_BY_FILE_ID),
                              &Fcb);

        if (RequestedOptions & FILE_OPEN_BY_FILE_ID)
        {
            ExFreePoolWithTag(FullPath.Buffer, TAG_NTFS);
        }
    }

    DPRINT("NtfsCreateFile: NtfsOpenFile returned Status=0x%lx, Fcb=%p\n", Status, Fcb);

    if (NT_SUCCESS(Status))
    {
        /* The name carried a trailing backslash, which can only denote a
         * directory; it resolved to something else, so the NAME is invalid
         * (checked before any disposition handling, like Windows). */
        if (TrailingBackslash && !NtfsFCBIsDirectory(Fcb))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return STATUS_OBJECT_NAME_INVALID;
        }

        if (RequestedDisposition == FILE_CREATE)
        {
            Irp->IoStatus.Information = FILE_EXISTS;
            NtfsCloseFile(DeviceExt, FileObject);
            return STATUS_OBJECT_NAME_COLLISION;
        }

        /* Typed stream opens ([MS-FSCC] 2.1.5): "::$INDEX_ALLOCATION" names
         * the directory index stream and demands a directory target, while
         * an explicit default-stream "::$DATA" open demands a non-directory
         * (a directory has no unnamed $DATA attribute).  Plain opens (no
         * stream suffix) are unaffected. */
        if (StreamType == NtfsStreamTypeIndexAllocation &&
            !NtfsFCBIsDirectory(Fcb))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return STATUS_NOT_A_DIRECTORY;
        }

        if (StreamType == NtfsStreamTypeDefaultData &&
            NtfsFCBIsDirectory(Fcb))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return STATUS_FILE_IS_A_DIRECTORY;
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
         * symlink, third-party, etc.) - the tag goes into
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

            /* Overwrite/supersede truncates on-disk state; refuse it on a
             * volume the mount gate forced read-only (dirty or too-new
             * $LogFile - see NtfsMountVolume).  IRP_MJ_CREATE is not
             * covered by the NtfsDispatch read-only short-circuit, so the
             * mutating dispositions are gated here. */
            if (BooleanFlagOn(DeviceExt->Flags, VCB_VOLUME_READ_ONLY))
            {
                DPRINT("NTFS: refusing overwrite disposition on read-only volume\n");
                NtfsCloseFile(DeviceExt, FileObject);
                return STATUS_MEDIA_WRITE_PROTECTED;
            }

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

                // find the data attribute (the FCB's own stream - the unnamed
                // one for a plain open, the named one for "file:stream") and
                // set its length to 0
                Status = FindAttribute(Fcb->Vcb, fileRecord, AttributeData, Fcb->Stream, wcslen(Fcb->Stream), &dataContext, &DataAttributeOffset);
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
            /* A name with a trailing backslash can only create a
             * directory. */
            if (TrailingBackslash && !(RequestedOptions & FILE_DIRECTORY_FILE))
            {
                return STATUS_OBJECT_NAME_INVALID;
            }

            /* The file doesn't exist, so every disposition that reaches
             * this branch would create it - a mutation the read-only
             * mount gate must refuse (see the overwrite gate above). */
            if (BooleanFlagOn(DeviceExt->Flags, VCB_VOLUME_READ_ONLY))
            {
                DPRINT("NTFS: refusing create disposition on read-only volume\n");
                NtfsCloseFile(DeviceExt, FileObject);
                return STATUS_MEDIA_WRITE_PROTECTED;
            }

            if (!NtfsGlobalData->EnableWriteSupport)
            {
                DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
                NtfsCloseFile(DeviceExt, FileObject);
                return STATUS_ACCESS_DENIED;
            }

            /* A typed "::$INDEX_ALLOCATION" create is meaningful only as a
             * directory create: with FILE_DIRECTORY_FILE the (already
             * stripped) name falls through to NtfsCreateDirectory below,
             * exactly like Windows; without it there is nothing valid the
             * name could create. */
            if (StreamType == NtfsStreamTypeIndexAllocation &&
                !(RequestedOptions & FILE_DIRECTORY_FILE))
            {
                return STATUS_OBJECT_NAME_INVALID;
            }

            /* An explicit default-$DATA open can create a file, never a
             * directory. */
            if (StreamType == NtfsStreamTypeDefaultData &&
                RequestedOptions & FILE_DIRECTORY_FILE)
            {
                return STATUS_NOT_A_DIRECTORY;
            }

            if (StreamName.Length != 0)
            {
                /* A named stream can never be a directory. */
                if (RequestedOptions & FILE_DIRECTORY_FILE)
                {
                    return STATUS_NOT_A_DIRECTORY;
                }

                /* Create the named $DATA attribute on the base file (creating
                 * the base file too if it doesn't exist yet). */
                Status = NtfsCreateStream(DeviceExt,
                                          FileObject,
                                          &StreamName,
                                          BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                          Stack->Parameters.Create.FileAttributes,
                                          BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));
                DPRINT("NtfsCreateFile: NtfsCreateStream returned 0x%lx\n", Status);
            }
            // Was the user trying to create a directory?
            else if (RequestedOptions & FILE_DIRECTORY_FILE)
            {
                DPRINT("NtfsCreateFile: Creating directory...\n");
                // Create the directory on disk
                Status = NtfsCreateDirectory(DeviceExt,
                                             FileObject,
                                             BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                             Stack->Parameters.Create.FileAttributes,
                                             BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));
                DPRINT("NtfsCreateFile: NtfsCreateDirectory returned 0x%lx\n", Status);
            }
            else
            {
                // Create the file record on disk
                Status = NtfsCreateFileRecord(DeviceExt,
                                              FileObject,
                                              BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                              Stack->Parameters.Create.FileAttributes,
                                              BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));
                NTFS_TRACE("DRVIDX: top create record returned 0x%lx for %wZ\n",
                        Status,
                        &FileObject->FileName);
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
            NTFS_TRACE("DRVIDX: recursive open begin for %wZ\n", &FileObject->FileName);
            Status = NtfsCreateFile(DeviceObject, IrpContext);
            NTFS_TRACE("DRVIDX: recursive open returned 0x%lx for %wZ\n",
                    Status,
                    &FileObject->FileName);
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
        /* Like fastfat (and Windows): an open that intends to write the file
         * or delete it on close cannot proceed while the file is mapped as a
         * running executable image.  MmFlushImageSection(MmFlushForWrite)
         * returns FALSE while any view of the image section is still mapped.
         * A write open is then rejected with STATUS_SHARING_VIOLATION, a
         * delete-on-close open with STATUS_CANNOT_DELETE.
         *
         * cygwin/msys2's unlink_nt depends on the delete-on-close case: when
         * the plain delete disposition returns STATUS_CANNOT_DELETE it reopens
         * the file with FILE_DELETE_ON_CLOSE, and only when THAT open also
         * fails does it fall back to moving the in-use file into the recycle
         * bin so a replacement can be created.  Without this check the
         * delete-on-close open wrongly succeeds, the file is never moved aside,
         * and replacing a running image (e.g. msys-2.0.dll during pacman -U)
         * fails with "Can't create".  For a freshly created/superseded file
         * there is no image section, so MmFlushImageSection returns TRUE and
         * this is a no-op. */
        if (Fcb->Identifier.Type == NTFS_TYPE_FCB &&
            !BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME) &&
            !NtfsFCBIsDirectory(Fcb) &&
            (BooleanFlagOn(DesiredAccess, FILE_WRITE_DATA) ||
             BooleanFlagOn(RequestedOptions, FILE_DELETE_ON_CLOSE)) &&
            Fcb->SectionObjectPointers != NULL &&
            !MmFlushImageSection(Fcb->SectionObjectPointers, MmFlushForWrite))
        {
            Status = BooleanFlagOn(RequestedOptions, FILE_DELETE_ON_CLOSE)
                     ? STATUS_CANNOT_DELETE : STATUS_SHARING_VIOLATION;
            NtfsCloseFile(DeviceExt, FileObject);
            return Status;
        }

        if (Fcb->Identifier.Type == NTFS_TYPE_FCB &&
            !BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME))
        {
            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            if (Fcb->OpenHandleCount == 0)
            {
                /* First open of this FCB - establish share access. */
                IoSetShareAccess(DesiredAccess,
                                 ShareAccess,
                                 FileObject,
                                 &Fcb->ShareAccess);
            }
            else
            {
                /* Subsequent open - must be compatible with existing
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

        /* FILE_DELETE_ON_CLOSE: the file must be gone once the last handle is
         * cleaned up. Mark the FCB delete-pending so NtfsCleanupFile removes the
         * record and its $I30 entry, exactly as FileDispositionInformation does.
         * Windows honours this disposition at create time; without it a
         * delete-on-close open (cygwin/msys2 unlink, temp files) silently leaks
         * the file and a later same-name create wrongly collides. Directories
         * are not deletable through this driver yet, so leave them alone. */
        if ((RequestedOptions & FILE_DELETE_ON_CLOSE) &&
            Fcb->Identifier.Type == NTFS_TYPE_FCB &&
            !BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME) &&
            !NtfsFCBIsDirectory(Fcb))
        {
            SetFlag(Fcb->Flags, FCB_DELETE_PENDING);
        }
    }

    /*
     * Report the disposition outcome. A plain open reports FILE_OPENED, but the
     * FILE_OVERWRITE(_IF)/FILE_SUPERSEDE path on an existing file already set
     * Irp->IoStatus.Information to FILE_OVERWRITTEN / FILE_SUPERSEDED above;
     * Windows preserves those, so only overwrite it for the plain-open case.
     */
    if (NT_SUCCESS(Status))
    {
        if (RequestedDisposition != FILE_OVERWRITE &&
            RequestedDisposition != FILE_OVERWRITE_IF &&
            RequestedDisposition != FILE_SUPERSEDE)
        {
            Irp->IoStatus.Information = FILE_OPENED;
        }
    }
    else
    {
        Irp->IoStatus.Information = 0;
    }

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
                    ULONG FileAttributes,
                    BOOLEAN CanWait)
{

    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD NextAttribute;
    PFILENAME_ATTRIBUTE FilenameAttribute;
    ULONGLONG ParentMftIndex;
    ULONGLONG FileMftIndex;
    ULONGLONG RawFileMftIndex;
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
    AddStandardInformation(FileRecord, NextAttribute, FileAttributes);

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
    Status = AddFileName(FileRecord, NextAttribute, DeviceExt, FileObject, CaseSensitive, FileAttributes, &ParentMftIndex);
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
        RawFileMftIndex = FileMftIndex;

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
        if (!NT_SUCCESS(Status))
        {
            ClearFlag(FileRecord->Flags, FRH_IN_USE);
            FileRecord->LinkCount = 0;
            UpdateFileRecord(DeviceExt, RawFileMftIndex, FileRecord);
            NtfsSetMftBitmapInUse(DeviceExt, RawFileMftIndex, FALSE, CanWait);
        }
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
                     ULONG FileAttributes,
                     BOOLEAN CanWait)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD NextAttribute;
    PFILENAME_ATTRIBUTE FilenameAttribute;
    ULONGLONG ParentMftIndex;
    ULONGLONG FileMftIndex;
    ULONGLONG RawFileMftIndex;

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
    AddStandardInformation(FileRecord, NextAttribute, FileAttributes);

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
    Status = AddFileName(FileRecord, NextAttribute, DeviceExt, FileObject, CaseSensitive, FileAttributes, &ParentMftIndex);
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
        RawFileMftIndex = FileMftIndex;

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
        if (!NT_SUCCESS(Status))
        {
            ClearFlag(FileRecord->Flags, FRH_IN_USE);
            FileRecord->LinkCount = 0;
            UpdateFileRecord(DeviceExt, RawFileMftIndex, FileRecord);
            NtfsSetMftBitmapInUse(DeviceExt, RawFileMftIndex, FALSE, CanWait);
        }
        NTFS_TRACE_IF(ParentMftIndex == 27, "DRVIDX: create file add-name returned 0x%lx for %.*S\n",
                    Status,
                    FilenameAttribute->NameLength,
                    FilenameAttribute->Name);

        /* Emit a USN_REASON_FILE_CREATE record into the change journal
         * if one is active on this volume.  Gated so un-journalled
         * volumes stay on the fast path.  Kreijstal/reactos#33. */
        if (NT_SUCCESS(Status) && DeviceExt->UsnJournalFcb != NULL)
        {
            ULONGLONG ParentRef = FilenameAttribute->DirectoryFileReferenceNumber;
            (void)NtfsUsnEmitRecord(DeviceExt,
                                    FileMftIndex,
                                    ParentRef,
                                    USN_REASON_FILE_CREATE,
                                    0,
                                    FilenameAttribute->Name,
                                    FilenameAttribute->NameLength);
        }
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    NTFS_TRACE_IF(ParentMftIndex == 27, "DRVIDX: create file record return 0x%lx for %.*S\n",
                Status,
                FilenameAttribute->NameLength,
                FilenameAttribute->Name);

    return Status;
}

/* EOF */
