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

/**
* @name NtfsBuildCreatePath
*
* Builds the volume-absolute path a create names: the file object's name, or that name
* appended to the path of the directory its related file object is open on.
*
* @param FileObject
* File object of the create.
*
* @param Path
* Receives the path in a NUL-terminated pool buffer, which the caller frees with
* ExFreePoolWithTag(..., TAG_NTFS).
*
* @return
* STATUS_SUCCESS, STATUS_INVALID_PARAMETER if the name can't be relative to the related
* file object, or STATUS_INSUFFICIENT_RESOURCES.
*/
static
NTSTATUS
NtfsBuildCreatePath(
    _In_ PFILE_OBJECT FileObject,
    _Out_ PUNICODE_STRING Path)
{
    PCUNICODE_STRING Name = &FileObject->FileName;
    PNTFS_FCB RelatedFcb = NULL;
    SIZE_T PrefixLength = 0;
    SIZE_T Length;
    PWCHAR Buffer;

    if (FileObject->RelatedFileObject != NULL)
    {
        RelatedFcb = FileObject->RelatedFileObject->FsContext;
        ASSERT(RelatedFcb != NULL);

        if (BooleanFlagOn(RelatedFcb->Flags, FCB_IS_VOLUME))
        {
            /* A volume handle roots an absolute path */
            if (Name->Length == 0 || Name->Buffer[0] != L'\\')
                return STATUS_INVALID_PARAMETER;

            RelatedFcb = NULL;
        }
        else if (Name->Length != 0 &&
                 (!NtfsFCBIsDirectory(RelatedFcb) || Name->Buffer[0] == L'\\'))
        {
            return STATUS_INVALID_PARAMETER;
        }
        else
        {
            PrefixLength = wcslen(RelatedFcb->PathName);
        }
    }

    /* The prefix, a separator unless the prefix is the root or nothing is appended, the name */
    Length = PrefixLength + 1 + Name->Length / sizeof(WCHAR);
    if (Length >= MAXUSHORT / sizeof(WCHAR))
        return STATUS_OBJECT_NAME_INVALID;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, (Length + 1) * sizeof(WCHAR), TAG_NTFS);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Path->Buffer = Buffer;
    Path->Length = 0;
    Path->MaximumLength = (USHORT)((Length + 1) * sizeof(WCHAR));

    if (RelatedFcb != NULL)
    {
        RtlCopyMemory(Buffer, RelatedFcb->PathName, PrefixLength * sizeof(WCHAR));
        Path->Length = (USHORT)(PrefixLength * sizeof(WCHAR));

        if (Name->Length != 0 && !NtfsFCBIsRoot(RelatedFcb))
        {
            Buffer[PrefixLength] = L'\\';
            Path->Length += sizeof(WCHAR);
        }
    }

    RtlCopyMemory((PUCHAR)Buffer + Path->Length, Name->Buffer, Name->Length);
    Path->Length += Name->Length;
    Buffer[Path->Length / sizeof(WCHAR)] = UNICODE_NULL;

    return STATUS_SUCCESS;
}

/**
* @name NtfsCheckCreatePath
*
* Validates an absolute create path the way NTFS does (MS-FSA 2.1.5.1): every component
* must be a legal NTFS name; only the last may name a stream. A trailing separator says the
* caller wants a directory; it's cut off here and reported in TrailingBackslash.
*
* @return
* STATUS_SUCCESS or STATUS_OBJECT_NAME_INVALID.
*/
static
NTSTATUS
NtfsCheckCreatePath(
    _Inout_ PUNICODE_STRING Path,
    _In_ ULONG Options,
    _Out_ PBOOLEAN TrailingBackslash)
{
    UNICODE_STRING Remaining, Component;
    USHORT i;

    *TrailingBackslash = FALSE;

    if (Path->Length == 0 || Path->Buffer[0] != L'\\')
        return STATUS_OBJECT_NAME_INVALID;

    /* The root directory */
    if (Path->Length == sizeof(WCHAR))
        return STATUS_SUCCESS;

    if (Path->Buffer[Path->Length / sizeof(WCHAR) - 1] == L'\\')
    {
        if (BooleanFlagOn(Options, FILE_NON_DIRECTORY_FILE))
            return STATUS_OBJECT_NAME_INVALID;

        Path->Length -= sizeof(WCHAR);
        Path->Buffer[Path->Length / sizeof(WCHAR)] = UNICODE_NULL;
        *TrailingBackslash = TRUE;
    }

    Remaining.Buffer = Path->Buffer + 1;
    Remaining.Length = Path->Length - sizeof(WCHAR);
    Remaining.MaximumLength = Remaining.Length;

    while (Remaining.Length != 0)
    {
        Component = Remaining;
        for (i = 0; i < Remaining.Length / sizeof(WCHAR); i++)
        {
            if (Remaining.Buffer[i] == L'\\')
                break;
        }
        Component.Length = i * sizeof(WCHAR);

        if (Component.Length == Remaining.Length)
        {
            /* The last component: only its name part has to be a file name */
            Remaining.Length = 0;
            for (i = 0; i < Component.Length / sizeof(WCHAR); i++)
            {
                if (Component.Buffer[i] == L':')
                {
                    Component.Length = i * sizeof(WCHAR);
                    break;
                }
            }
        }
        else
        {
            Remaining.Buffer += i + 1;
            Remaining.Length -= Component.Length + sizeof(WCHAR);
        }

        if (!NtfsIsValidFileName(&Component))
            return STATUS_OBJECT_NAME_INVALID;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsGetFinalComponent
*
* Returns the last component of an absolute path that isn't the root.
*/
static
VOID
NtfsGetFinalComponent(
    _In_ PCUNICODE_STRING Path,
    _Out_ PUNICODE_STRING FinalName)
{
    USHORT Index = Path->Length / sizeof(WCHAR);

    ASSERT(Index > 1);

    while (Path->Buffer[Index - 1] != L'\\')
        Index--;

    FinalName->Buffer = Path->Buffer + Index;
    FinalName->Length = Path->Length - Index * sizeof(WCHAR);
    FinalName->MaximumLength = FinalName->Length;
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
    PWCHAR FullPath;
    ULONG WritePosition;

    DPRINT("NtfsMoonWalkID(%p, %I64x, %p)\n", DeviceExt, Id, OutPath);

    /* The path is assembled backwards from the end of a buffer as long as any path can be */
    FullPath = ExAllocatePoolWithTag(NonPagedPool, NTFS_MAX_PATH_LENGTH * sizeof(WCHAR), TAG_NTFS);
    if (FullPath == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    MftRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (MftRecord == NULL)
    {
        ExFreePoolWithTag(FullPath, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WritePosition = NTFS_MAX_PATH_LENGTH - 1;
    FullPath[WritePosition] = UNICODE_NULL;

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

        if (WritePosition < FileName->NameLength + 1U)
        {
            Status = STATUS_OBJECT_NAME_INVALID;
            break;
        }

        WritePosition -= FileName->NameLength;
        RtlCopyMemory(FullPath + WritePosition, FileName->Name, FileName->NameLength * sizeof(WCHAR));
        WritePosition -= 1;
        FullPath[WritePosition] = L'\\';

        Id = FileName->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
        if (Id == NTFS_FILE_ROOT)
            break;
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, MftRecord);

    if (NT_SUCCESS(Status))
    {
        OutPath->Length = (USHORT)((NTFS_MAX_PATH_LENGTH - WritePosition - 1) * sizeof(WCHAR));
        OutPath->MaximumLength = OutPath->Length + sizeof(WCHAR);
        OutPath->Buffer = ExAllocatePoolWithTag(NonPagedPool, OutPath->MaximumLength, TAG_NTFS);
        if (OutPath->Buffer == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
        else
            RtlCopyMemory(OutPath->Buffer, FullPath + WritePosition, OutPath->MaximumLength);
    }

    ExFreePoolWithTag(FullPath, TAG_NTFS);

    return Status;
}

static
NTSTATUS
NtfsOpenFileById(PDEVICE_EXTENSION DeviceExt,
                 ULONGLONG MftId,
                 PNTFS_FCB * FoundFCB)
{
    NTSTATUS Status;
    PNTFS_FCB FCB;
    PFILE_RECORD_HEADER MftRecord;

    DPRINT("NtfsOpenFileById(%p, %I64x, %p)\n", DeviceExt, MftId, FoundFCB);

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

    *FoundFCB = FCB;

    return STATUS_SUCCESS;
}

/**
* @name NtfsOpenFile
*
* Finds the FCB of an absolute path, from the FCB table or from the disk.
*
* @param FoundFcb
* Receives the FCB, referenced, on success.
*
* @param ParentFcb
* Receives the FCB of the directory that would hold the file, referenced, when the
* last component is all that's missing (STATUS_OBJECT_NAME_NOT_FOUND); NULL otherwise.
*/
static
NTSTATUS
NtfsOpenFile(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PCWSTR FileName,
    _In_ BOOLEAN CaseSensitive,
    _Out_ PNTFS_FCB *FoundFcb,
    _Out_ PNTFS_FCB *ParentFcb)
{
    PNTFS_FCB Parent = NULL;
    PNTFS_FCB Fcb;
    NTSTATUS Status;

    DPRINT("NtfsOpenFile(%p, %S, %s)\n", DeviceExt, FileName, CaseSensitive ? "TRUE" : "FALSE");

    *FoundFcb = NULL;
    *ParentFcb = NULL;

    Fcb = NtfsGrabFCBFromTable(DeviceExt, FileName);
    if (Fcb != NULL)
    {
        *FoundFcb = Fcb;
        return STATUS_SUCCESS;
    }

    Status = NtfsGetFCBForFile(DeviceExt, &Parent, &Fcb, FileName, CaseSensitive);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        *ParentFcb = Parent;
        return Status;
    }

    if (Parent != NULL)
        NtfsReleaseFCB(DeviceExt, Parent);

    if (NT_SUCCESS(Status))
        *FoundFcb = Fcb;

    return Status;
}

/**
* @name NtfsGrantShareAccess
*
* Records a new open of Fcb against its share access, or fails it with a
* sharing violation.
*/
static
NTSTATUS
NtfsGrantShareAccess(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_FCB Fcb,
    _In_ PFILE_OBJECT FileObject,
    _In_ PIO_STACK_LOCATION Stack)
{
    ACCESS_MASK DesiredAccess = 0;
    ULONG ShareAccess = Stack->Parameters.Create.ShareAccess;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Stack->Parameters.Create.SecurityContext != NULL)
        DesiredAccess = Stack->Parameters.Create.SecurityContext->DesiredAccess;

    if (Fcb->OpenHandleCount > 0)
    {
        Status = IoCheckShareAccess(DesiredAccess,
                                    ShareAccess,
                                    FileObject,
                                    &Fcb->ShareAccess,
                                    TRUE);
    }
    else
    {
        IoSetShareAccess(DesiredAccess,
                         ShareAccess,
                         FileObject,
                         &Fcb->ShareAccess);
    }

    if (NT_SUCCESS(Status))
    {
        Fcb->OpenHandleCount++;
        DeviceExt->OpenHandleCount++;
    }

    return Status;
}

/**
* @name NtfsOpenTargetDirectory
*
* Serves SL_OPEN_TARGET_DIRECTORY, the open the I/O manager does ahead of a
* rename: opens the parent of the named file, reports whether the final
* component exists, and leaves only that component in the file object's name,
* where the rename reads it.
*
* @param DeviceExt
* Volume being opened on.
*
* @param FileObject
* File object of the create, naming the rename target.
*
* @param CaseSensitive
* TRUE for POSIX semantics.
*
* @param Information
* Receives FILE_EXISTS or FILE_DOES_NOT_EXIST for the final component.
*
* @return
* STATUS_SUCCESS with the directory attached to FileObject, or an error.
*/
static
NTSTATUS
NtfsOpenTargetDirectory(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PFILE_OBJECT FileObject,
    _In_ BOOLEAN CaseSensitive,
    _Out_ PULONG_PTR Information)
{
    UNICODE_STRING Path;
    UNICODE_STRING FinalName;
    PCWSTR DirectoryPath;
    PWSTR LastSeparator;
    ULONGLONG ExistingMft;
    ULONG FirstEntry = 0;
    PNTFS_FCB ParentFcb = NULL;
    PNTFS_FCB Fcb;
    NTSTATUS Status;

    *Information = 0;

    if (FileObject->FileName.Length == 0)
        return STATUS_OBJECT_NAME_INVALID;

    Status = NtfsBuildCreatePath(FileObject, &Path);
    if (!NT_SUCCESS(Status))
        return Status;

    /* A trailing separator names nothing */
    while (Path.Length > sizeof(WCHAR) && Path.Buffer[Path.Length / sizeof(WCHAR) - 1] == L'\\')
    {
        Path.Length -= sizeof(WCHAR);
        Path.Buffer[Path.Length / sizeof(WCHAR)] = UNICODE_NULL;
    }

    LastSeparator = wcsrchr(Path.Buffer, L'\\');
    if (LastSeparator == NULL || LastSeparator[1] == UNICODE_NULL)
    {
        ExFreePoolWithTag(Path.Buffer, TAG_NTFS);
        return STATUS_OBJECT_NAME_INVALID;
    }

    RtlInitUnicodeString(&FinalName, LastSeparator + 1);

    /* Cut the path down to the directory */
    if (LastSeparator == Path.Buffer)
    {
        DirectoryPath = L"\\";
    }
    else
    {
        *LastSeparator = UNICODE_NULL;
        DirectoryPath = Path.Buffer;
    }

    Fcb = NtfsGrabFCBFromTable(DeviceExt, DirectoryPath);
    if (Fcb == NULL)
    {
        Status = NtfsGetFCBForFile(DeviceExt, &ParentFcb, &Fcb, DirectoryPath, CaseSensitive);
        if (ParentFcb != NULL)
            NtfsReleaseFCB(DeviceExt, ParentFcb);

        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Path.Buffer, TAG_NTFS);
            return (Status == STATUS_OBJECT_NAME_NOT_FOUND) ? STATUS_OBJECT_PATH_NOT_FOUND : Status;
        }
    }

    if (!NtfsFCBIsDirectory(Fcb))
    {
        NtfsReleaseFCB(DeviceExt, Fcb);
        ExFreePoolWithTag(Path.Buffer, TAG_NTFS);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    Status = NtfsFindMftRecord(DeviceExt, Fcb->MFTIndex, &FinalName, &FirstEntry, FALSE, CaseSensitive, &ExistingMft);
    *Information = NT_SUCCESS(Status) ? FILE_EXISTS : FILE_DOES_NOT_EXIST;

    Status = NtfsAttachFCBToFileObject(DeviceExt, Fcb, FileObject);
    if (!NT_SUCCESS(Status))
    {
        NtfsReleaseFCB(DeviceExt, Fcb);
        ExFreePoolWithTag(Path.Buffer, TAG_NTFS);
        return Status;
    }

    /* The final component is a suffix of the original name, so it fits */
    ASSERT(FinalName.Length <= FileObject->FileName.Length);
    RtlMoveMemory(FileObject->FileName.Buffer, FinalName.Buffer, FinalName.Length);
    FileObject->FileName.Length = FinalName.Length;

    ExFreePoolWithTag(Path.Buffer, TAG_NTFS);

    return STATUS_SUCCESS;
}


/**
* @name NtfsCheckCreateParameters
*
* Rejects option and disposition combinations no open can satisfy (MS-FSA 2.1.5.1).
*/
static
NTSTATUS
NtfsCheckCreateParameters(
    _In_ ULONG Disposition,
    _In_ ULONG Options,
    _In_ ACCESS_MASK DesiredAccess)
{
    if (Disposition > FILE_MAXIMUM_DISPOSITION)
        return STATUS_INVALID_PARAMETER;

    if (BooleanFlagOn(Options, FILE_DIRECTORY_FILE) &&
        BooleanFlagOn(Options, FILE_NON_DIRECTORY_FILE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* A directory can only be created or opened */
    if (BooleanFlagOn(Options, FILE_DIRECTORY_FILE) &&
        Disposition != FILE_CREATE &&
        Disposition != FILE_OPEN &&
        Disposition != FILE_OPEN_IF)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE) &&
        !BooleanFlagOn(DesiredAccess, DELETE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static
BOOLEAN
NtfsDispositionOverwrites(
    _In_ ULONG Disposition)
{
    return (Disposition == FILE_OVERWRITE ||
            Disposition == FILE_OVERWRITE_IF ||
            Disposition == FILE_SUPERSEDE);
}

/**
* @name NtfsCheckExistingFile
*
* Decides whether an open of a file that exists may go ahead (MS-FSA 2.1.5.1.2): the
* disposition, the type the caller asked for, a pending delete and the read-only attribute.
*/
static
NTSTATUS
NtfsCheckExistingFile(
    _In_ PNTFS_FCB Fcb,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG Options,
    _In_ ULONG Disposition,
    _In_ BOOLEAN TrailingBackslash)
{
    BOOLEAN IsDirectory = NtfsFCBIsDirectory(Fcb);
    BOOLEAN ReadOnly = BooleanFlagOn(Fcb->Entry.FileAttributes, NTFS_FILE_TYPE_READ_ONLY);

    if (Disposition == FILE_CREATE)
        return STATUS_OBJECT_NAME_COLLISION;

    if (BooleanFlagOn(Fcb->Flags, FCB_DELETE_PENDING))
        return STATUS_DELETE_PENDING;

    if (BooleanFlagOn(Options, FILE_NON_DIRECTORY_FILE) && IsDirectory)
        return STATUS_FILE_IS_A_DIRECTORY;

    if (BooleanFlagOn(Options, FILE_DIRECTORY_FILE) && !IsDirectory)
        return STATUS_NOT_A_DIRECTORY;

    /* A trailing separator names a directory */
    if (TrailingBackslash && !IsDirectory)
        return STATUS_OBJECT_NAME_INVALID;

    if (BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE) &&
        (ReadOnly || NtfsFCBIsRoot(Fcb)))
    {
        return STATUS_CANNOT_DELETE;
    }

    /* The read-only attribute forbids changing a file's data, not a directory's */
    if (ReadOnly && !IsDirectory &&
        (BooleanFlagOn(DesiredAccess, FILE_WRITE_DATA | FILE_APPEND_DATA) ||
         NtfsDispositionOverwrites(Disposition)))
    {
        return STATUS_ACCESS_DENIED;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsOverwriteFile
*
* Truncates the unnamed data stream of an existing file for FILE_OVERWRITE(_IF) and
* FILE_SUPERSEDE.
*/
static
NTSTATUS
NtfsOverwriteFile(
    _In_ PNTFS_FCB Fcb,
    _In_ PFILE_OBJECT FileObject)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    ULONG DataAttributeOffset;
    LARGE_INTEGER Zero;
    NTSTATUS Status;

    if (!NtfsGlobalData->EnableWriteSupport)
    {
        DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
        return STATUS_ACCESS_DENIED;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&Fcb->Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Zero.QuadPart = 0;

    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);

    Status = ReadFileRecord(Fcb->Vcb, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        // TODO: Handle Alternate Data Streams
        Status = FindAttribute(Fcb->Vcb, FileRecord, AttributeData, L"", 0, &DataContext, &DataAttributeOffset);
    }

    if (NT_SUCCESS(Status))
    {
        Status = SetAttributeDataLength(FileObject, Fcb, DataContext, DataAttributeOffset, FileRecord, &Zero);
        ReleaseAttributeContext(DataContext);
    }

    ExReleaseResourceLite(&Fcb->MainResource);

    ExFreeToNPagedLookasideList(&Fcb->Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsFinishOpen
*
* Last step of every successful open: records the open against the file's share access
* and remembers FILE_DELETE_ON_CLOSE for the handle's cleanup. Closes the file object if
* the share access doesn't allow the open.
*/
static
NTSTATUS
NtfsFinishOpen(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PFILE_OBJECT FileObject,
    _In_ PIO_STACK_LOCATION Stack)
{
    PNTFS_CCB Ccb = FileObject->FsContext2;
    NTSTATUS Status;

    Status = NtfsGrantShareAccess(DeviceExt, FileObject->FsContext, FileObject, Stack);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Sharing violation opening '%wZ'\n", &FileObject->FileName);
        NtfsCloseFile(DeviceExt, FileObject);
        return Status;
    }

    if (BooleanFlagOn(Stack->Parameters.Create.Options, FILE_DELETE_ON_CLOSE))
        Ccb->Flags |= CCB_DELETE_ON_CLOSE;

    return STATUS_SUCCESS;
}

/**
* @name NtfsOpenReparsePoint
*
* Hands the I/O manager the target of a mount point, the only reparse point
* that is followed. Consumes the open of FileObject.
*/
static
NTSTATUS
NtfsOpenReparsePoint(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_FCB Fcb,
    _In_ PFILE_OBJECT FileObject,
    _In_ PIRP Irp)
{
    PREPARSE_DATA_BUFFER ReparseData = NULL;
    NTSTATUS Status;

    Status = NtfsReadFCBAttribute(DeviceExt, Fcb,
                                  AttributeReparsePoint, L"", 0,
                                  (PVOID *)&Irp->Tail.Overlay.AuxiliaryBuffer);
    if (NT_SUCCESS(Status))
    {
        ReparseData = (PREPARSE_DATA_BUFFER)Irp->Tail.Overlay.AuxiliaryBuffer;
        if (ReparseData->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
        {
            Status = STATUS_REPARSE;
        }
        else
        {
            Status = STATUS_NOT_IMPLEMENTED;
            ExFreePoolWithTag(ReparseData, TAG_NTFS);
            Irp->Tail.Overlay.AuxiliaryBuffer = NULL;
        }
    }

    Irp->IoStatus.Information = ((Status == STATUS_REPARSE) ? ReparseData->ReparseTag : 0);

    NtfsCloseFile(DeviceExt, FileObject);

    return Status;
}

/**
* @name NtfsOpenExistingFile
*
* Opens a file that exists, applying the disposition. Consumes the reference on Fcb.
*/
static
NTSTATUS
NtfsOpenExistingFile(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack,
    _In_ PNTFS_FCB Fcb,
    _In_ BOOLEAN TrailingBackslash)
{
    PFILE_OBJECT FileObject = Stack->FileObject;
    ULONG Disposition = (Stack->Parameters.Create.Options >> 24) & 0xff;
    ULONG Options = Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS;
    ACCESS_MASK DesiredAccess = Stack->Parameters.Create.SecurityContext->DesiredAccess;
    ULONG_PTR Information = FILE_OPENED;
    NTSTATUS Status;

    Status = NtfsCheckExistingFile(Fcb, DesiredAccess, Options, Disposition, TrailingBackslash);
    if (NT_SUCCESS(Status))
        Status = NtfsAttachFCBToFileObject(DeviceExt, Fcb, FileObject);

    if (!NT_SUCCESS(Status))
    {
        if (Disposition == FILE_CREATE && Status == STATUS_OBJECT_NAME_COLLISION)
            Irp->IoStatus.Information = FILE_EXISTS;

        NtfsReleaseFCB(DeviceExt, Fcb);
        return Status;
    }

    /*
     * If it is a reparse point & FILE_OPEN_REPARSE_POINT, then allow opening it
     * as a normal file.
     * Otherwise, attempt to read reparse data and hand them to the Io manager
     * with status reparse to force a reparse.
     */
    if (NtfsFCBIsReparsePoint(Fcb) && !BooleanFlagOn(Options, FILE_OPEN_REPARSE_POINT))
        return NtfsOpenReparsePoint(DeviceExt, Fcb, FileObject, Irp);

    /* A file mapped as a running image can't be written or deleted */
    if (!NtfsFCBIsDirectory(Fcb) &&
        (BooleanFlagOn(DesiredAccess, FILE_WRITE_DATA) || BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE)) &&
        !MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForWrite))
    {
        NtfsCloseFile(DeviceExt, FileObject);
        return BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE) ? STATUS_CANNOT_DELETE : STATUS_SHARING_VIOLATION;
    }

    if (NtfsDispositionOverwrites(Disposition))
    {
        Status = NtfsOverwriteFile(Fcb, FileObject);
        if (!NT_SUCCESS(Status))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return Status;
        }

        Information = (Disposition == FILE_SUPERSEDE) ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
    }

    Status = NtfsFinishOpen(DeviceExt, FileObject, Stack);
    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = Information;

    return Status;
}

/**
* @name NtfsNewFileAttributes
*
* The attributes a new file or directory starts with (MS-FSA 2.1.5.1.1): what the caller
* asked for, less what can't be set; a file is also marked for archiving.
*/
static
ULONG
NtfsNewFileAttributes(
    _In_ ULONG Requested,
    _In_ BOOLEAN IsDirectory)
{
    ULONG Attributes = Requested & FILE_ATTRIBUTE_VALID_SET_FLAGS & ~FILE_ATTRIBUTE_NORMAL;

    if (!IsDirectory)
        Attributes |= FILE_ATTRIBUTE_ARCHIVE;

    return Attributes;
}

/**
* @name NtfsCreateNewFile
*
* Creates the file or directory a create names, in ParentFcb, and opens it.
*/
static
NTSTATUS
NtfsCreateNewFile(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_IRP_CONTEXT IrpContext,
    _In_ PNTFS_FCB ParentFcb,
    _In_ PCUNICODE_STRING Path,
    _In_ BOOLEAN TrailingBackslash)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    ULONG Options = Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS;
    BOOLEAN CaseSensitive = BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE);
    BOOLEAN IsDirectory = BooleanFlagOn(Options, FILE_DIRECTORY_FILE);
    UNICODE_STRING FinalName;
    PNTFS_FCB Fcb, UnusedParent;
    ULONG Attributes;
    NTSTATUS Status;

    if (!NtfsGlobalData->EnableWriteSupport)
    {
        DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
        return STATUS_ACCESS_DENIED;
    }

    NtfsGetFinalComponent(Path, &FinalName);

    /* Creating a stream means adding a named $DATA attribute to a
     * record, which isn't implemented: the volume doesn't report
     * FILE_NAMED_STREAMS. Answer the way a volume without named streams
     * does, rather than making "file:stream" the name of a new file
     * that no open could ever reach again. */
    if (NtfsNameHasStreamPart(&FinalName))
    {
        DPRINT1("Creating stream '%wZ' is not supported\n", Path);
        return STATUS_OBJECT_NAME_INVALID;
    }

    /* A trailing separator names a directory */
    if (TrailingBackslash && !IsDirectory)
        return STATUS_OBJECT_NAME_INVALID;

    Attributes = NtfsNewFileAttributes(Stack->Parameters.Create.FileAttributes, IsDirectory);

    if (BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE) &&
        BooleanFlagOn(Attributes, FILE_ATTRIBUTE_READONLY))
    {
        return STATUS_CANNOT_DELETE;
    }

    Status = NtfsCreateFileRecord(DeviceExt,
                                  ParentFcb,
                                  &FinalName,
                                  Attributes,
                                  IsDirectory,
                                  CaseSensitive,
                                  BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't create '%wZ' (Status %lx)\n", Path, Status);
        return Status;
    }

    /* The caller holds DirResource exclusively, so the name is still there */
    Status = NtfsOpenFile(DeviceExt, Path->Buffer, CaseSensitive, &Fcb, &UnusedParent);
    ASSERT(NT_SUCCESS(Status));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsAttachFCBToFileObject(DeviceExt, Fcb, FileObject);
    if (!NT_SUCCESS(Status))
    {
        NtfsReleaseFCB(DeviceExt, Fcb);
        return Status;
    }

    Status = NtfsFinishOpen(DeviceExt, FileObject, Stack);
    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = FILE_CREATED;

    return Status;
}

/**
* @name NtfsOpenVolume
*
* Opens the volume itself: a create with no name, absolute or relative to a volume handle.
*/
static
NTSTATUS
NtfsOpenVolume(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PIRP Irp,
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG Disposition,
    _In_ ULONG Options)
{
    NTSTATUS Status;

    if (Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF)
        return STATUS_ACCESS_DENIED;

    if (BooleanFlagOn(Options, FILE_DIRECTORY_FILE))
        return STATUS_NOT_A_DIRECTORY;

    Status = NtfsAttachFCBToFileObject(DeviceExt, DeviceExt->VolumeFcb, FileObject);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceExt->VolumeFcb->RefCount++;

    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

/**
* @name NtfsResolveFileId
*
* Turns a FILE_OPEN_BY_FILE_ID name into the FCB of a system file or the path of a user file.
*/
static
NTSTATUS
NtfsResolveFileId(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PFILE_OBJECT FileObject,
    _Out_ PNTFS_FCB *Fcb,
    _Out_ PUNICODE_STRING Path)
{
    ULONGLONG MftId;

    *Fcb = NULL;
    Path->Buffer = NULL;

    /* An object ID (16 bytes) needs the $ObjId index, which isn't read */
    if (FileObject->FileName.Length != sizeof(ULONGLONG))
        return STATUS_INVALID_PARAMETER;

    MftId = (*(PULONGLONG)FileObject->FileName.Buffer) & NTFS_MFT_MASK;
    if (MftId < NTFS_FILE_FIRST_USER_FILE)
        return NtfsOpenFileById(DeviceExt, MftId, Fcb);

    return NtfsMoonWalkID(DeviceExt, MftId, Path);
}

/*
 * FUNCTION: Opens a file
 */
static
NTSTATUS
NtfsCreateFile(PDEVICE_OBJECT DeviceObject,
               PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExt = DeviceObject->DeviceExtension;
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    ULONG Disposition = (Stack->Parameters.Create.Options >> 24) & 0xff;
    ULONG Options = Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS;
    BOOLEAN CaseSensitive = BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE);
    ACCESS_MASK DesiredAccess = Stack->Parameters.Create.SecurityContext->DesiredAccess;
    PNTFS_FCB Fcb = NULL, ParentFcb = NULL;
    UNICODE_STRING Path = {0, 0, NULL};
    BOOLEAN TrailingBackslash = FALSE;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    DPRINT("NtfsCreateFile(%p, %p) called\n", DeviceObject, IrpContext);

    ASSERT(DeviceExt);

    Status = NtfsCheckCreateParameters(Disposition, Options, DesiredAccess);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Deny create if the volume is locked */
    if (DeviceExt->Flags & VCB_VOLUME_LOCKED)
    {
        return STATUS_ACCESS_DENIED;
    }

    /* The I/O manager opening a rename's destination directory */
    if (Stack->Flags & SL_OPEN_TARGET_DIRECTORY)
    {
        if (Options & FILE_OPEN_BY_FILE_ID)
            return STATUS_INVALID_PARAMETER;

        Status = NtfsOpenTargetDirectory(DeviceExt, FileObject, CaseSensitive, &Information);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = NtfsGrantShareAccess(DeviceExt, FileObject->FsContext, FileObject, Stack);
        if (!NT_SUCCESS(Status))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return Status;
        }

        Irp->IoStatus.Information = Information;
        return STATUS_SUCCESS;
    }

    if (BooleanFlagOn(Options, FILE_OPEN_BY_FILE_ID))
    {
        Status = NtfsResolveFileId(DeviceExt, FileObject, &Fcb, &Path);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Fcb != NULL)
            return NtfsOpenExistingFile(DeviceExt, Irp, Stack, Fcb, FALSE);

        DPRINT("Open by ID: %I64x -> %wZ\n", (*(PULONGLONG)FileObject->FileName.Buffer) & NTFS_MFT_MASK, &Path);
    }
    else
    {
        /* This a open operation for the volume itself */
        if (FileObject->FileName.Length == 0 &&
            (FileObject->RelatedFileObject == NULL ||
             BooleanFlagOn(((PNTFS_FCB)FileObject->RelatedFileObject->FsContext)->Flags, FCB_IS_VOLUME)))
        {
            return NtfsOpenVolume(DeviceExt, Irp, FileObject, Disposition, Options);
        }

        Status = NtfsBuildCreatePath(FileObject, &Path);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = NtfsCheckCreatePath(&Path, Options, &TrailingBackslash);
    if (NT_SUCCESS(Status))
        Status = NtfsOpenFile(DeviceExt, Path.Buffer, CaseSensitive, &Fcb, &ParentFcb);

    if (NT_SUCCESS(Status))
    {
        Status = NtfsOpenExistingFile(DeviceExt, Irp, Stack, Fcb, TrailingBackslash);
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        ASSERT(ParentFcb != NULL);

        if (Disposition == FILE_CREATE ||
            Disposition == FILE_OPEN_IF ||
            Disposition == FILE_OVERWRITE_IF ||
            Disposition == FILE_SUPERSEDE)
        {
            Status = NtfsCreateNewFile(DeviceExt, IrpContext, ParentFcb, &Path, TrailingBackslash);
        }

        NtfsReleaseFCB(DeviceExt, ParentFcb);
    }

    ExFreePoolWithTag(Path.Buffer, TAG_NTFS);

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
        DPRINT("Opening file system\n");
        IrpContext->Irp->IoStatus.Information = FILE_OPENED;
        return STATUS_SUCCESS;
    }

    DeviceExt = DeviceObject->DeviceExtension;

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
* @name NtfsAddEmptyIndex
*
* Adds the empty $I30 $INDEX_ROOT of a new directory at the end of FileRecord.
*/
static
NTSTATUS
NtfsAddEmptyIndex(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _Inout_ PFILE_RECORD_HEADER FileRecord,
    _In_ PNTFS_ATTR_RECORD AttributeAddress)
{
    PB_TREE Tree;
    PINDEX_ROOT_ATTRIBUTE NewIndexRoot;
    ULONG MaxIndexRootSize;
    ULONG RootLength;
    NTSTATUS Status;

    Status = CreateEmptyBTree(&Tree);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to create empty B-Tree!\n");
        return Status;
    }

    // Calculate maximum size of index root
    MaxIndexRootSize = DeviceExt->NtfsInfo.BytesPerFileRecord
                       - ((ULONG_PTR)AttributeAddress - (ULONG_PTR)FileRecord)
                       - sizeof(ULONG) * 2;

    Status = CreateIndexRootFromBTree(DeviceExt, Tree, MaxIndexRootSize, &NewIndexRoot, &RootLength);
    DestroyBTree(Tree);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Unable to create empty index root!\n");
        return Status;
    }

    Status = AddIndexRoot(DeviceExt, FileRecord, AttributeAddress, NewIndexRoot, RootLength, L"$I30", 4);
    ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);

    return Status;
}

/**
* @name NtfsCreateFileRecord()
* @implemented
*
* Creates the file record of a new file or directory, saves it to the MFT and adds its name
* to the index of the directory it's created in.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param ParentFcb
* FCB of the directory the file is created in.
*
* @param Name
* Name of the new file, a single path component.
*
* @param FileAttributes
* FILE_ATTRIBUTE_* flags the file starts with.
*
* @param IsDirectory
* TRUE to create a directory (an empty $I30 index), FALSE for a file (an empty $DATA).
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
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
                     PNTFS_FCB ParentFcb,
                     PCUNICODE_STRING Name,
                     ULONG FileAttributes,
                     BOOLEAN IsDirectory,
                     BOOLEAN CaseSensitive,
                     BOOLEAN CanWait)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD NextAttribute;
    PFILENAME_ATTRIBUTE FilenameAttribute;
    ULONGLONG ParentReference;
    ULONGLONG FileMftIndex;
    ULONGLONG FileReference;

    DPRINT("NtfsCreateFileRecord(%p, %p, '%wZ', 0x%lx, %s, %s, %s)\n",
           DeviceExt,
           ParentFcb,
           Name,
           FileAttributes,
           IsDirectory ? "TRUE" : "FALSE",
           CaseSensitive ? "TRUE" : "FALSE",
           CanWait ? "TRUE" : "FALSE");

    ASSERT(NtfsFCBIsDirectory(ParentFcb));

    FileRecord = NtfsCreateEmptyFileRecord(DeviceExt);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (IsDirectory)
        FileRecord->Flags |= FRH_DIRECTORY;

    // find where the first attribute will be added
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

    // add first attribute, $STANDARD_INFORMATION
    AddStandardInformation(FileRecord, NextAttribute, FileAttributes);

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
    ParentReference = ParentFcb->MFTIndex | ((ULONGLONG)ParentFcb->SequenceNumber << 48);
    Status = AddFileName(FileRecord, NextAttribute, ParentReference, Name, CaseSensitive, FileAttributes);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to add the $FILE_NAME attribute for '%wZ'!\n", Name);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // save a pointer to the filename attribute
    FilenameAttribute = (PFILENAME_ATTRIBUTE)((ULONG_PTR)NextAttribute + NextAttribute->Resident.ValueOffset);

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // a directory gets an empty index, a file an empty $DATA attribute
    if (IsDirectory)
        Status = NtfsAddEmptyIndex(DeviceExt, FileRecord, NextAttribute);
    else
        Status = AddData(FileRecord, NextAttribute);

    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

#ifndef NDEBUG
    // dump file record in memory (for debugging)
    NtfsDumpFileRecord(DeviceExt, FileRecord);
#endif

    // Now that we've built the file record in memory, we need to store it in the MFT.
    Status = AddNewMftEntry(FileRecord, DeviceExt, &FileMftIndex, CanWait);
    if (NT_SUCCESS(Status))
    {
        FileReference = FileMftIndex | ((ULONGLONG)FileRecord->SequenceNumber << 48);

        DPRINT("New File Reference: 0x%016I64x\n", FileReference);

        // Add the filename attribute to the filename-index of the parent directory
        Status = NtfsAddFilenameToDirectory(DeviceExt,
                                            ParentFcb->MFTIndex,
                                            FileReference,
                                            FilenameAttribute,
                                            CaseSensitive);
        if (!NT_SUCCESS(Status))
        {
            /* Nothing refers to the record, so give it back */
            FreeMftEntry(DeviceExt, FileMftIndex);
        }
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/* EOF */
