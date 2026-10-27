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
#include "security.h"

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
            return STATUS_INVALID_PARAMETER;
        }
        else if (Name->Length != 0 && Name->Buffer[0] == L'\\')
        {
            return STATUS_INVALID_PARAMETER;
        }
        else if (Name->Length != 0 && !NtfsFCBIsDirectory(RelatedFcb))
        {
            return STATUS_OBJECT_PATH_NOT_FOUND;
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

    /* NTFS accepts one extra separator immediately after the volume name. */
    if (Path->Length >= 2 * sizeof(WCHAR) && Path->Buffer[1] == L'\\')
    {
        if (Path->Length >= 3 * sizeof(WCHAR) && Path->Buffer[2] == L'\\')
            return STATUS_OBJECT_NAME_INVALID;
        Path->Length -= sizeof(WCHAR);
        RtlMoveMemory(Path->Buffer, Path->Buffer + 1, Path->Length);
        Path->Buffer[Path->Length / sizeof(WCHAR)] = UNICODE_NULL;
    }

    /* The root directory */
    if (Path->Length == sizeof(WCHAR))
        return STATUS_SUCCESS;

    if (Path->Buffer[Path->Length / sizeof(WCHAR) - 1] == L'\\')
    {
        if (Path->Buffer[Path->Length / sizeof(WCHAR) - 2] == L'\\')
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

        /* Check separators and dot components before walking the namespace.
         * Other illegal characters are checked when their parent is reached,
         * so a missing ancestor takes precedence over an invalid final name. */
        if (Component.Length == 0 ||
            (Component.Length == 2 * sizeof(WCHAR) &&
             Component.Buffer[0] == L'.' && Component.Buffer[1] == L'.') ||
            (Remaining.Length == 0 && Component.Length == sizeof(WCHAR) &&
             Component.Buffer[0] == L'.'))
            return STATUS_OBJECT_NAME_INVALID;
        if (Remaining.Length != 0 &&
            !(Component.Length == sizeof(WCHAR) && Component.Buffer[0] == L'.') &&
            !NtfsIsValidFileName(&Component))
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

/* What the stream part of a create's last component names */
typedef enum _NTFS_STREAM_KIND
{
    NtfsNoStream,           /* no stream part: the file itself */
    NtfsNamedStream,        /* "file:name" or "file:name:$DATA" */
    NtfsUnnamedDataStream,  /* "file::$DATA": the file's unnamed data, which a directory hasn't */
    NtfsIndexStream         /* "dir::$INDEX_ALLOCATION" or "dir:$I30:$INDEX_ALLOCATION" */
} NTFS_STREAM_KIND;

/**
* @name NtfsSplitStreamName
*
* Cuts the stream part off the last component of an absolute path (MS-FSA 2.1.5.1):
* "name:stream:type", where the stream name may be empty and the type is $DATA unless given.
*
* @param Path
* The path, NUL-terminated. Cut down to the file's path when it has a stream part.
*
* @param StreamName
* Receives the stream name, pointing into Path's buffer past the new terminator.
*
* @param Kind
* Receives what the stream part names.
*
* @return
* STATUS_SUCCESS or STATUS_OBJECT_NAME_INVALID.
*/
static
NTSTATUS
NtfsSplitStreamName(
    _Inout_ PUNICODE_STRING Path,
    _Out_ PUNICODE_STRING StreamName,
    _Out_ NTFS_STREAM_KIND *Kind)
{
    static const UNICODE_STRING DataType = RTL_CONSTANT_STRING(L"$DATA");
    static const UNICODE_STRING IndexType = RTL_CONSTANT_STRING(L"$INDEX_ALLOCATION");
    static const UNICODE_STRING IndexName = RTL_CONSTANT_STRING(L"$I30");
    USHORT Count = Path->Length / sizeof(WCHAR);
    USHORT Colon, i;
    UNICODE_STRING Type = {0, 0, NULL};

    RtlInitEmptyUnicodeString(StreamName, NULL, 0);
    *Kind = NtfsNoStream;

    for (Colon = Count; Colon > 0 && Path->Buffer[Colon - 1] != L'\\'; Colon--)
        ;
    while (Colon < Count && Path->Buffer[Colon] != L':')
        Colon++;

    if (Colon == Count)
        return STATUS_SUCCESS;

    StreamName->Buffer = Path->Buffer + Colon + 1;
    for (i = Colon + 1; i < Count && Path->Buffer[i] != L':'; i++)
        ;
    StreamName->Length = (i - Colon - 1) * sizeof(WCHAR);
    StreamName->MaximumLength = StreamName->Length;

    if (i < Count)
    {
        Type.Buffer = Path->Buffer + i + 1;
        Type.Length = (Count - i - 1) * sizeof(WCHAR);
        Type.MaximumLength = Type.Length;
    }

    /* The file's path ends where its stream part starts */
    Path->Length = Colon * sizeof(WCHAR);
    Path->Buffer[Colon] = UNICODE_NULL;

    if (Type.Buffer == NULL)
    {
        /* "file:" names nothing */
        if (StreamName->Length == 0)
            return STATUS_OBJECT_NAME_INVALID;
    }
    else if (RtlEqualUnicodeString(&Type, &IndexType, TRUE))
    {
        if (StreamName->Length != 0 && !RtlEqualUnicodeString(StreamName, &IndexName, TRUE))
            return STATUS_OBJECT_NAME_INVALID;

        *Kind = NtfsIndexStream;
        return STATUS_SUCCESS;
    }
    else if (!RtlEqualUnicodeString(&Type, &DataType, TRUE))
    {
        /* Any other type (and a further ':') isn't a stream that can be opened */
        return STATUS_OBJECT_NAME_INVALID;
    }

    if (StreamName->Length == 0)
    {
        *Kind = NtfsUnnamedDataStream;
        return STATUS_SUCCESS;
    }

    if (StreamName->Length > NTFS_MAX_NAME_LENGTH * sizeof(WCHAR) ||
        FsRtlDoesNameContainWildCards(StreamName))
    {
        return STATUS_OBJECT_NAME_INVALID;
    }

    for (i = 0; i < StreamName->Length / sizeof(WCHAR); i++)
    {
        if (StreamName->Buffer[i] == UNICODE_NULL || StreamName->Buffer[i] == L'/')
            return STATUS_OBJECT_NAME_INVALID;
    }

    *Kind = NtfsNamedStream;
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
*
* @param ReparseOffset
* With STATUS_REPARSE, receives the number of characters of FileName up to the end of
* the reparse point the walk ran into, whose FCB FoundFcb receives.
*/
static
NTSTATUS
NtfsOpenFile(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PCWSTR FileName,
    _In_ BOOLEAN CaseSensitive,
    _Out_ PNTFS_FCB *FoundFcb,
    _Out_ PNTFS_FCB *ParentFcb,
    _Out_opt_ PULONG ReparseOffset)
{
    PNTFS_FCB Parent = NULL;
    PNTFS_FCB Fcb;
    ULONG Offset = 0;
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

    Status = NtfsGetFCBForFile(DeviceExt, &Parent, &Fcb, FileName, CaseSensitive, &Offset);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        *ParentFcb = Parent;
        return Status;
    }

    if (Parent != NULL)
        NtfsReleaseFCB(DeviceExt, Parent);

    /* A walk that ran into a reparse point returns it, with where the rest of the path
     * starts. A caller that can't follow it finds no path. */
    if (Status == STATUS_REPARSE)
    {
        if (ReparseOffset == NULL)
        {
            NtfsReleaseFCB(DeviceExt, Fcb);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        *ReparseOffset = Offset;
    }

    if (NT_SUCCESS(Status))
        *FoundFcb = Fcb;

    return Status;
}

/**
* @name NtfsCountOpen
*
* Counts a successful open in its FCB's and its volume's open handle counts. The
* cleanup of the handle takes it back; a volume can only be locked by its only handle.
*/
static
VOID
NtfsCountOpen(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_FCB Fcb,
    _In_ PFILE_OBJECT FileObject,
    _In_ PIO_STACK_LOCATION Stack)
{
    PNTFS_CCB Ccb = FileObject->FsContext2;
    PIO_SECURITY_CONTEXT Security = Stack->Parameters.Create.SecurityContext;
    PRIVILEGE_SET Required;

    Fcb->OpenHandleCount++;
    DeviceExt->OpenHandleCount++;
    Ccb->Flags |= CCB_COUNTED;

    /* SetFileValidData requires the privilege to have been enabled when
     * the handle was opened. Use the captured caller, including queued creates. */
    if (Security && Security->AccessState)
    {
        Required.PrivilegeCount = 1;
        Required.Control = PRIVILEGE_SET_ALL_NECESSARY;
        Required.Privilege[0].Luid = RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE);
        Required.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (SePrivilegeCheck(&Required, &Security->AccessState->SubjectSecurityContext, UserMode))
            Ccb->Flags |= CCB_MANAGE_VOLUME;
    }
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
        NtfsCountOpen(DeviceExt, Fcb, FileObject, Stack);

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
        ULONG ReparseOffset;

        Status = NtfsGetFCBForFile(DeviceExt, &ParentFcb, &Fcb, DirectoryPath, CaseSensitive, &ReparseOffset);
        if (ParentFcb != NULL)
            NtfsReleaseFCB(DeviceExt, ParentFcb);

        /* A rename target behind a reparse point isn't followed */
        if (Status == STATUS_REPARSE)
        {
            NtfsReleaseFCB(DeviceExt, Fcb);
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
        }

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

    Status = NtfsFindMftRecord(DeviceExt, Fcb->MFTIndex, &FinalName, &FirstEntry, FALSE, CaseSensitive, &ExistingMft, NULL);
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
    _In_ ACCESS_MASK DesiredAccess,
    _In_ BOOLEAN PagingFile)
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

    /* NtCreatePagingFile supersedes the paging file delete-on-close without
     * DELETE access, as on Windows; it skips the I/O manager's parameter
     * check (IO_NO_PARAMETER_CHECKING) for that reason. */
    if (BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE) &&
        !BooleanFlagOn(DesiredAccess, DELETE) &&
        !PagingFile)
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

    if (BooleanFlagOn(Fcb->Flags, FCB_DELETE_PENDING))
        return STATUS_DELETE_PENDING;

    if (BooleanFlagOn(Options, FILE_NON_DIRECTORY_FILE) && IsDirectory)
        return STATUS_FILE_IS_A_DIRECTORY;

    if (BooleanFlagOn(Options, FILE_DIRECTORY_FILE) && !IsDirectory)
        return STATUS_NOT_A_DIRECTORY;

    if (Disposition == FILE_CREATE)
        return NtfsFCBIsRoot(Fcb) ? STATUS_ACCESS_DENIED :
               (TrailingBackslash && !IsDirectory) ? STATUS_OBJECT_NAME_INVALID : STATUS_OBJECT_NAME_COLLISION;

    /* A trailing separator names a directory */
    if (TrailingBackslash && !IsDirectory)
        return STATUS_OBJECT_NAME_INVALID;

    /* The read-only attribute forbids changing a file's data, not a directory's */
    if (ReadOnly && !IsDirectory &&
        (BooleanFlagOn(DesiredAccess, FILE_WRITE_DATA | FILE_APPEND_DATA) ||
         NtfsDispositionOverwrites(Disposition)))
    {
        return STATUS_ACCESS_DENIED;
    }

    if (BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE) &&
        (ReadOnly || NtfsFCBIsRoot(Fcb)))
    {
        return STATUS_CANNOT_DELETE;
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
        Status = FindAttribute(Fcb->Vcb, FileRecord, AttributeData, Fcb->Stream, wcslen(Fcb->Stream), &DataContext, &DataAttributeOffset);
    }

    /* Only the record holding the stream changes; read it if that isn't the base */
    if (NT_SUCCESS(Status) && (DataContext->FileMFTIndex & NTFS_MFT_MASK) != Fcb->MFTIndex)
    {
        Status = ReadFileRecord(Fcb->Vcb, DataContext->FileMFTIndex & NTFS_MFT_MASK, FileRecord);
        if (!NT_SUCCESS(Status))
            ReleaseAttributeContext(DataContext);
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
* @name NtfsReparseOpen
*
* Answers an open that reached a reparse point it isn't to open itself: STATUS_REPARSE with
* the tag, and the reparse data for the I/O manager to act on. A mount point it follows by
* putting its target in front of the part of the name not parsed yet; any other tag goes
* to whoever handles it, or fails with STATUS_IO_REPARSE_TAG_NOT_HANDLED. Consumes the
* reference on Fcb.
*
* @param UnparsedBytes
* Bytes at the end of the file object's name past the reparse point.
*
* @param UnparsedHasSeparator
* Whether those bytes start with a path separator.
*/
static
NTSTATUS
NtfsReparseOpen(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PIRP Irp,
    _In_ PNTFS_FCB Fcb,
    _In_ ULONG UnparsedBytes,
    _In_ BOOLEAN UnparsedHasSeparator)
{
    PREPARSE_DATA_BUFFER ReparseData;
    NTSTATUS Status;

    Status = NtfsReadFCBAttribute(DeviceExt, Fcb, AttributeReparsePoint, L"", 0, (PVOID *)&ReparseData);
    NtfsReleaseFCB(DeviceExt, Fcb);
    if (!NT_SUCCESS(Status))
        return Status;

    /* A target naming a directory with a trailing separator takes the rest without one */
    if (ReparseData->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT && UnparsedHasSeparator)
    {
        PCWSTR Target = (PCWSTR)((ULONG_PTR)ReparseData->MountPointReparseBuffer.PathBuffer +
                                 ReparseData->MountPointReparseBuffer.SubstituteNameOffset);
        USHORT TargetLength = ReparseData->MountPointReparseBuffer.SubstituteNameLength / sizeof(WCHAR);

        if (TargetLength != 0 && Target[TargetLength - 1] == L'\\')
            UnparsedBytes -= sizeof(WCHAR);
    }

    ASSERT(UnparsedBytes < MAXUSHORT);
    ReparseData->Reserved = (USHORT)UnparsedBytes;

    Irp->Tail.Overlay.AuxiliaryBuffer = (PVOID)ReparseData;
    Irp->IoStatus.Information = ReparseData->ReparseTag;

    return STATUS_REPARSE;
}

static KPROCESSOR_MODE
NtfsCreateAccessMode(PIRP Irp, PIO_STACK_LOCATION Stack)
{
    return (Stack->Flags & SL_FORCE_ACCESS_CHECK) ? UserMode : Irp->RequestorMode;
}

static NTSTATUS
NtfsCheckParentAccess(PDEVICE_EXTENSION Vcb,
                       ULONGLONG Parent,
                       PACCESS_STATE AccessState,
                       ACCESS_MASK Desired,
                       KPROCESSOR_MODE Mode)
{
    ACCESS_MASK Granted;

    if ((AccessState->Flags & TOKEN_HAS_RESTORE_PRIVILEGE) &&
        !(Desired & ~(FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD)))
        return STATUS_SUCCESS;
    return NtfsCheckAccess(Vcb, Parent, AccessState, Desired, 0, Mode, &Granted);
}

/* A cached target FCB does not authorize its ancestors. Check the path before
 * consulting the FCB table when the effective token lacks traverse bypass. */
static NTSTATUS
NtfsCheckCreateTraverse(PDEVICE_EXTENSION Vcb,
                         PCUNICODE_STRING Path,
                         PACCESS_STATE AccessState,
                         KPROCESSOR_MODE Mode,
                         BOOLEAN CaseSensitive)
{
    ULONGLONG Parent = NTFS_FILE_ROOT, Index;
    ULONG Position = 1, End = Path->Length / sizeof(WCHAR), Next;
    UNICODE_STRING Name;
    PFILE_RECORD_HEADER Record;
    PSTANDARD_INFORMATION Info;
    ACCESS_MASK Granted;
    BOOLEAN Directory, Reparse;
    NTSTATUS Status;

    if (Mode == KernelMode || (AccessState->Flags & TOKEN_HAS_TRAVERSE_PRIVILEGE) ||
        (AccessState->PreviouslyGrantedAccess & FILE_TRAVERSE))
        return STATUS_SUCCESS;
    while (Position < End)
    {
        Status = NtfsCheckAccess(Vcb, Parent, AccessState, FILE_TRAVERSE, 0, Mode, &Granted);
        if (!NT_SUCCESS(Status))
            return Status;
        for (Next = Position; Next < End && Path->Buffer[Next] != L'\\'; Next++)
            ;
        if (Next == End)
            return STATUS_SUCCESS;
        if (Next == Position)
        {
            Position++;
            continue;
        }
        Name.Buffer = Path->Buffer + Position;
        Name.Length = Name.MaximumLength = (USHORT)((Next - Position) * sizeof(WCHAR));
        Status = NtfsLookupFileAt(Vcb, &Name, CaseSensitive, &Record, &Index, Parent);
        if (!NT_SUCCESS(Status))
            return Status == STATUS_OBJECT_NAME_NOT_FOUND ? STATUS_OBJECT_PATH_NOT_FOUND : Status;
        Info = GetStandardInformationFromRecord(Vcb, Record);
        ASSERT(Info != NULL);
        Reparse = BooleanFlagOn(Info->FileAttribute, FILE_ATTRIBUTE_REPARSE_POINT);
        Directory = BooleanFlagOn(Record->Flags, FRH_DIRECTORY);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
        /* The I/O manager reparses the redirected path with the same access
         * state. Do not interpret a reparse point's contents as an index. */
        if (Reparse)
            return STATUS_SUCCESS;
        if (!Directory)
            return STATUS_OBJECT_PATH_NOT_FOUND;
        Parent = Index;
        Position = Next + 1;
    }
    return STATUS_SUCCESS;
}

/* A parent can independently authorize deletion and reading attributes
 * (MS-FSA 2.1.5.1.2.1), even when the file's DACL denies those rights. */
static NTSTATUS
NtfsCheckOpenAccess(PDEVICE_EXTENSION Vcb,
                     PNTFS_FCB Fcb,
                     PACCESS_STATE AccessState,
                     ACCESS_MASK Desired,
                     ACCESS_MASK Previous,
                     KPROCESSOR_MODE Mode,
                     PACCESS_MASK Granted)
{
    ACCESS_MASK ParentGranted = 0;
    NTSTATUS Status;

    Status = NtfsCheckAccess(Vcb, Fcb->MFTIndex, AccessState, Desired, Previous, Mode, Granted);
    if ((Status == STATUS_ACCESS_DENIED || NT_SUCCESS(Status)) &&
        (Desired & (DELETE | MAXIMUM_ALLOWED)) &&
        (!NT_SUCCESS(Status) || !(*Granted & DELETE)) &&
        NT_SUCCESS(NtfsCheckParentAccess(Vcb, Fcb->ParentMFTIndex, AccessState, FILE_DELETE_CHILD, Mode)))
        ParentGranted |= DELETE;
    if ((Status == STATUS_ACCESS_DENIED || NT_SUCCESS(Status)) &&
        (Desired & (FILE_READ_ATTRIBUTES | MAXIMUM_ALLOWED)) &&
        (!NT_SUCCESS(Status) || !(*Granted & FILE_READ_ATTRIBUTES)) &&
        NT_SUCCESS(NtfsCheckParentAccess(Vcb, Fcb->ParentMFTIndex, AccessState, FILE_LIST_DIRECTORY, Mode)))
        ParentGranted |= FILE_READ_ATTRIBUTES;
    if (ParentGranted != 0)
        Status = NtfsCheckAccess(Vcb, Fcb->MFTIndex, AccessState, Desired & ~ParentGranted,
                                  Previous | ParentGranted, Mode, Granted);
    return Status;
}

static NTSTATUS
NtfsAuthorizeOpen(PDEVICE_EXTENSION Vcb, PNTFS_FCB Fcb, PIRP Irp,
                   PIO_STACK_LOCATION Stack, BOOLEAN ExistingStream)
{
    PIO_SECURITY_CONTEXT Security = Stack->Parameters.Create.SecurityContext;
    PACCESS_STATE AccessState = Security->AccessState;
    KPROCESSOR_MODE Mode = NtfsCreateAccessMode(Irp, Stack);
    ULONG Disposition = Stack->Parameters.Create.Options >> 24;
    ACCESS_MASK Desired = AccessState->RemainingDesiredAccess;
    ACCESS_MASK Requested = Desired | AccessState->PreviouslyGrantedAccess;
    ACCESS_MASK Granted = 0, Extra = 0;
    NTSTATUS Status;

    if (!ExistingStream)
        Extra = FILE_WRITE_DATA;
    else if (Disposition == FILE_SUPERSEDE || Disposition == FILE_OVERWRITE || Disposition == FILE_OVERWRITE_IF)
    {
        Extra = (Disposition == FILE_SUPERSEDE) ? DELETE : FILE_WRITE_DATA;
        if (Fcb->Stream[0] == UNICODE_NULL)
            Extra |= FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
    }
    Desired |= Extra & ~AccessState->PreviouslyGrantedAccess;
    Status = NtfsCheckOpenAccess(Vcb, Fcb, AccessState, Desired,
                                  AccessState->PreviouslyGrantedAccess, Mode, &Granted);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((Desired & MAXIMUM_ALLOWED) && (Vcb->Flags & VCB_VOLUME_READ_ONLY))
        Granted &= ~(FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                     FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER);
    if ((Desired & MAXIMUM_ALLOWED) && (Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_READ_ONLY))
        Granted &= ~(FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD);
    if (Desired & ~(Granted | MAXIMUM_ALLOWED))
        return STATUS_ACCESS_DENIED;
    /* Overwriting requires additional rights, but those rights were not
     * requested for the returned handle. */
    if (!(Requested & MAXIMUM_ALLOWED))
        Granted &= Requested;
    AccessState->PreviouslyGrantedAccess = Granted;
    AccessState->RemainingDesiredAccess = 0;
    Security->DesiredAccess = Granted;
    return STATUS_SUCCESS;
}

/* Creating a new file grants the requested access before its new DACL takes
 * effect on subsequent opens (MS-FSA 2.1.5.1.1). */
static VOID
NtfsGrantCreatedAccess(PIO_STACK_LOCATION Stack)
{
    PIO_SECURITY_CONTEXT Security = Stack->Parameters.Create.SecurityContext;
    PACCESS_STATE AccessState = Security->AccessState;
    ACCESS_MASK Desired = AccessState->RemainingDesiredAccess;

    AccessState->PreviouslyGrantedAccess |= Desired & ~MAXIMUM_ALLOWED;
    if (Desired & MAXIMUM_ALLOWED)
        AccessState->PreviouslyGrantedAccess |= FILE_ALL_ACCESS;
    AccessState->RemainingDesiredAccess = 0;
    Security->DesiredAccess = AccessState->PreviouslyGrantedAccess;
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
    _In_ BOOLEAN TrailingBackslash,
    _In_ ULONG UnparsedBytes)
{
    PFILE_OBJECT FileObject = Stack->FileObject;
    ULONG Disposition = (Stack->Parameters.Create.Options >> 24) & 0xff;
    ULONG Options = Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS;
    ACCESS_MASK DesiredAccess = Stack->Parameters.Create.SecurityContext->DesiredAccess;
    ULONG_PTR Information = FILE_OPENED;
    NTSTATUS Status;

    /* A reparse point is opened itself only with FILE_OPEN_REPARSE_POINT */
    if (NtfsFCBIsReparsePoint(Fcb) && !BooleanFlagOn(Options, FILE_OPEN_REPARSE_POINT))
        return NtfsReparseOpen(DeviceExt, Irp, Fcb, UnparsedBytes, TrailingBackslash);

    Status = NtfsCheckExistingFile(Fcb, DesiredAccess, Options, Disposition, TrailingBackslash);
    if (NT_SUCCESS(Status))
        Status = NtfsAuthorizeOpen(DeviceExt, Fcb, Irp, Stack, TRUE);
    if (NT_SUCCESS(Status))
        Status = NtfsAttachFCBToFileObject(DeviceExt, Fcb, FileObject);

    if (!NT_SUCCESS(Status))
    {
        if (Disposition == FILE_CREATE && Status == STATUS_OBJECT_NAME_COLLISION)
            Irp->IoStatus.Information = FILE_EXISTS;

        NtfsReleaseFCB(DeviceExt, Fcb);
        return Status;
    }

    DesiredAccess = Stack->Parameters.Create.SecurityContext->DesiredAccess;

    if (NtfsFCBIsDirectory(Fcb) &&
        FileObject->FileName.Length > sizeof(WCHAR) &&
        FileObject->FileName.Buffer[FileObject->FileName.Length / sizeof(WCHAR) - 1] == L'\\')
    {
        FileObject->FileName.Length -= sizeof(WCHAR);
        FileObject->FileName.Buffer[FileObject->FileName.Length / sizeof(WCHAR)] = UNICODE_NULL;
    }

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
        /* Check the final granted rights before any destructive operation.
         * DirResource keeps the share state fixed until FinishOpen commits it. */
        ACCESS_MASK OverwriteAccess = DesiredAccess |
            ((Disposition == FILE_SUPERSEDE) ? DELETE : FILE_WRITE_DATA);
        Status = IoCheckShareAccess(OverwriteAccess, Stack->Parameters.Create.ShareAccess,
                                     FileObject, &Fcb->ShareAccess, FALSE);
        if (!NT_SUCCESS(Status))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return Status;
        }
        Status = NtfsOverwriteFile(Fcb, FileObject);
        if (!NT_SUCCESS(Status))
        {
            NtfsCloseFile(DeviceExt, FileObject);
            return Status;
        }

        Information = (Disposition == FILE_SUPERSEDE) ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
    }

    Status = NtfsFinishOpen(DeviceExt, FileObject, Stack);
    ASSERT(!NtfsDispositionOverwrites(Disposition) || NT_SUCCESS(Status));
    if (NT_SUCCESS(Status))
    {
        Irp->IoStatus.Information = Information;
        if (Information == FILE_SUPERSEDED || Information == FILE_OVERWRITTEN)
            NtfsReportDataChange(Fcb, FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    }

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
    ULONG Attributes, SecurityId;
    NTSTATUS Status;

    if (DeviceExt->Flags & VCB_VOLUME_READ_ONLY)
        return STATUS_MEDIA_WRITE_PROTECTED;

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

    /* EAs given with the create go on the new file; a bad list fails it first */
    if (Stack->Parameters.Create.EaLength != 0)
    {
        ULONG ErrorOffset;

        Status = NtfsCheckEaList(Irp->AssociatedIrp.SystemBuffer, Stack->Parameters.Create.EaLength, &ErrorOffset);
        if (!NT_SUCCESS(Status))
        {
            Irp->IoStatus.Information = ErrorOffset;
            return Status;
        }
    }

    if (BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE) &&
        BooleanFlagOn(Attributes, FILE_ATTRIBUTE_READONLY))
    {
        return STATUS_CANNOT_DELETE;
    }

    Status = NtfsCheckParentAccess(DeviceExt, ParentFcb->MFTIndex,
                                    Stack->Parameters.Create.SecurityContext->AccessState,
                                    IsDirectory ? FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE,
                                    NtfsCreateAccessMode(Irp, Stack));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsAssignSecurity(DeviceExt,
                                ParentFcb->MFTIndex,
                                Stack->Parameters.Create.SecurityContext->AccessState,
                                IsDirectory,
                                &SecurityId);
    if (!NT_SUCCESS(Status))
        return Status;

    ASSERT(NtfsFCBIsDirectory(ParentFcb));
    Status = NtfsCreateFileRecord(DeviceExt,
                                  NtfsFileReference(ParentFcb),
                                  &FinalName,
                                  Attributes,
                                  IsDirectory,
                                  SecurityId,
                                  CaseSensitive,
                                  BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT),
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't create '%wZ' (Status %lx)\n", Path, Status);
        return Status;
    }

    /* The caller holds DirResource exclusively, so the name is still there */
    Status = NtfsOpenFile(DeviceExt, Path->Buffer, CaseSensitive, &Fcb, &UnusedParent, NULL);
    ASSERT(NT_SUCCESS(Status));
    if (!NT_SUCCESS(Status))
        return Status;

    if (Stack->Parameters.Create.EaLength != 0)
    {
        ULONG ErrorOffset;

        Status = NtfsSetEas(DeviceExt, Fcb, Irp->AssociatedIrp.SystemBuffer, Stack->Parameters.Create.EaLength, &ErrorOffset);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Couldn't give '%wZ' its EAs (Status %lx)\n", Path, Status);
            NtfsReleaseFCB(DeviceExt, Fcb);
            return Status;
        }
    }

    /* A file created in a compressed directory starts compressed (MS-FSA 2.1.5.1.1) */
    if (NtfsFCBIsCompressed(ParentFcb))
    {
        Status = NtfsSetCompression(DeviceExt, Fcb, COMPRESSION_FORMAT_LZNT1);
        if (!NT_SUCCESS(Status))
        {
            NtfsReleaseFCB(DeviceExt, Fcb);
            return Status;
        }
    }

    NtfsGrantCreatedAccess(Stack);
    Status = NtfsAttachFCBToFileObject(DeviceExt, Fcb, FileObject);
    if (!NT_SUCCESS(Status))
    {
        NtfsReleaseFCB(DeviceExt, Fcb);
        return Status;
    }

    Status = NtfsFinishOpen(DeviceExt, FileObject, Stack);
    if (NT_SUCCESS(Status))
    {
        Irp->IoStatus.Information = FILE_CREATED;
        NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_FILE_CREATE);
        NtfsReportPathChange(DeviceExt, Fcb->PathName,
                              NtfsFCBIsDirectory(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                              FILE_ACTION_ADDED);
    }

    return Status;
}

/**
* @name NtfsAddStream
*
* Adds an empty named data stream to the file BaseFcb is open on.
*/
static
NTSTATUS
NtfsAddStream(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_FCB BaseFcb,
    _In_ PCUNICODE_STRING StreamName)
{
    USHORT NameLength = StreamName->Length / sizeof(WCHAR);
    PFILE_RECORD_HEADER FileRecord;
    NTSTATUS Status;

    if (!NtfsGlobalData->EnableWriteSupport)
    {
        DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
        return STATUS_ACCESS_DENIED;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    ExAcquireResourceExclusiveLite(&BaseFcb->MainResource, TRUE);

    Status = ReadFileRecord(DeviceExt, BaseFcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        /* Past what the base record holds, the stream goes into an extension record */
        Status = NtfsAddResidentAttributeToFile(DeviceExt, FileRecord, AttributeData, StreamName->Buffer, NameLength, NULL, 0);
    }

    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(DeviceExt, BaseFcb->MFTIndex, FileRecord);

    ExReleaseResourceLite(&BaseFcb->MainResource);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsOpenBaseOfStream
*
* Finds the file a stream create names, creating it first when it's missing and the
* disposition creates. Created reports whether it did.
*/
static
NTSTATUS
NtfsOpenBaseOfStream(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_IRP_CONTEXT IrpContext,
    _In_ PCUNICODE_STRING Path,
    _Out_ PNTFS_FCB *BaseFcb,
    _Out_ PBOOLEAN Created,
    _Out_ PULONG ReparseOffset)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(IrpContext->Irp);
    ULONG Disposition = (Stack->Parameters.Create.Options >> 24) & 0xff;
    BOOLEAN CaseSensitive = BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE);
    UNICODE_STRING FinalName;
    PNTFS_FCB ParentFcb;
    ULONG SecurityId;
    NTSTATUS Status;

    *Created = FALSE;

    Status = NtfsOpenFile(DeviceExt, Path->Buffer, CaseSensitive, BaseFcb, &ParentFcb, ReparseOffset);
    if (Status != STATUS_OBJECT_NAME_NOT_FOUND)
        return Status;

    if (Disposition == FILE_OPEN || Disposition == FILE_OVERWRITE)
    {
        NtfsReleaseFCB(DeviceExt, ParentFcb);
        return Status;
    }

    if (DeviceExt->Flags & VCB_VOLUME_READ_ONLY)
    {
        NtfsReleaseFCB(DeviceExt, ParentFcb);
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    if (!NtfsGlobalData->EnableWriteSupport)
    {
        NtfsReleaseFCB(DeviceExt, ParentFcb);
        return STATUS_ACCESS_DENIED;
    }

    NtfsGetFinalComponent(Path, &FinalName);
    Status = NtfsCheckParentAccess(DeviceExt, ParentFcb->MFTIndex,
                                    Stack->Parameters.Create.SecurityContext->AccessState,
                                    FILE_ADD_FILE, NtfsCreateAccessMode(IrpContext->Irp, Stack));
    if (!NT_SUCCESS(Status))
    {
        NtfsReleaseFCB(DeviceExt, ParentFcb);
        return Status;
    }
    Status = NtfsAssignSecurity(DeviceExt,
                                ParentFcb->MFTIndex,
                                Stack->Parameters.Create.SecurityContext->AccessState,
                                FALSE,
                                &SecurityId);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsCreateFileRecord(DeviceExt,
                                      NtfsFileReference(ParentFcb),
                                      &FinalName,
                                      NtfsNewFileAttributes(Stack->Parameters.Create.FileAttributes, FALSE),
                                      FALSE,
                                      SecurityId,
                                      CaseSensitive,
                                      BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT),
                                      NULL);
    }
    NtfsReleaseFCB(DeviceExt, ParentFcb);
    if (!NT_SUCCESS(Status))
        return Status;

    *Created = TRUE;

    Status = NtfsOpenFile(DeviceExt, Path->Buffer, CaseSensitive, BaseFcb, &ParentFcb, NULL);
    ASSERT(NT_SUCCESS(Status));
    NtfsReportPathChange(DeviceExt, (*BaseFcb)->PathName,
                          FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED);

    return Status;
}

/**
* @name NtfsOpenStream
*
* Opens, or creates, a named data stream of a file (MS-FSA 2.1.5.1). A stream is opened like
* a file: the disposition applies to it, and creating one on a missing file creates the file.
*
* @param Path
* Path of the file the stream belongs to.
*
* @param StreamName
* Name of the stream.
*/
static
NTSTATUS
NtfsOpenStream(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_IRP_CONTEXT IrpContext,
    _In_ PCUNICODE_STRING Path,
    _In_ PCUNICODE_STRING StreamName,
    _In_ ULONG NameLength)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG Disposition = (Stack->Parameters.Create.Options >> 24) & 0xff;
    ULONG Options = Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS;
    PNTFS_FCB BaseFcb, Fcb;
    BOOLEAN BaseCreated;
    ULONG ReparseOffset;
    NTSTATUS Status;

    /* A stream is never a directory */
    if (BooleanFlagOn(Options, FILE_DIRECTORY_FILE))
        return STATUS_NOT_A_DIRECTORY;

    Status = NtfsOpenBaseOfStream(DeviceExt, IrpContext, Path, &BaseFcb, &BaseCreated, &ReparseOffset);
    if (Status == STATUS_REPARSE)
        return NtfsReparseOpen(DeviceExt, Irp, BaseFcb, (NameLength - ReparseOffset) * sizeof(WCHAR), TRUE);

    if (!NT_SUCCESS(Status))
        return Status;

    /* The stream of a reparse point is past it, like a path through it */
    if (NtfsFCBIsReparsePoint(BaseFcb) && !BooleanFlagOn(Options, FILE_OPEN_REPARSE_POINT))
        return NtfsReparseOpen(DeviceExt, Irp, BaseFcb, NameLength * sizeof(WCHAR) - Path->Length, FALSE);

    if (BooleanFlagOn(BaseFcb->Flags, FCB_DELETE_PENDING))
    {
        NtfsReleaseFCB(DeviceExt, BaseFcb);
        return STATUS_DELETE_PENDING;
    }

    Status = NtfsMakeStreamFCB(DeviceExt, BaseFcb, StreamName, &Fcb);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        if (Disposition == FILE_OPEN || Disposition == FILE_OVERWRITE)
        {
            NtfsReleaseFCB(DeviceExt, BaseFcb);
            return Status;
        }

        if (DeviceExt->Flags & VCB_VOLUME_READ_ONLY)
        {
            NtfsReleaseFCB(DeviceExt, BaseFcb);
            return STATUS_MEDIA_WRITE_PROTECTED;
        }

        if (!BaseCreated)
            Status = NtfsAuthorizeOpen(DeviceExt, BaseFcb, Irp, Stack, FALSE);
        else
        {
            NtfsGrantCreatedAccess(Stack);
            Status = STATUS_SUCCESS;
        }
        if (NT_SUCCESS(Status))
            Status = NtfsAddStream(DeviceExt, BaseFcb, StreamName);
        if (NT_SUCCESS(Status))
            Status = NtfsMakeStreamFCB(DeviceExt, BaseFcb, StreamName, &Fcb);

        NtfsReleaseFCB(DeviceExt, BaseFcb);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = NtfsAttachFCBToFileObject(DeviceExt, Fcb, Stack->FileObject);
        if (!NT_SUCCESS(Status))
        {
            NtfsReleaseFCB(DeviceExt, Fcb);
            return Status;
        }

        Status = NtfsFinishOpen(DeviceExt, Stack->FileObject, Stack);
        if (NT_SUCCESS(Status))
        {
            Irp->IoStatus.Information = FILE_CREATED;
            NtfsReportChange(Fcb, FILE_NOTIFY_CHANGE_STREAM_NAME, FILE_ACTION_ADDED_STREAM);
        }

        return Status;
    }

    NtfsReleaseFCB(DeviceExt, BaseFcb);
    if (!NT_SUCCESS(Status))
        return Status;

    ASSERT(!BaseCreated);

    return NtfsOpenExistingFile(DeviceExt, Irp, Stack, Fcb, FALSE, 0);
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
    NtfsCountOpen(DeviceExt, DeviceExt->VolumeFcb, FileObject, IoGetCurrentIrpStackLocation(Irp));

    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

/**
* @name NtfsResolveFileId
*
* Turns a FILE_OPEN_BY_FILE_ID name, a file ID or an object ID, into the FCB of a system file
* or the path of a user file.
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
    NTSTATUS Status;

    *Fcb = NULL;
    Path->Buffer = NULL;

    /* A file ID is 8 bytes, an object ID 16, which $O of $Extend\$ObjId maps to the file */
    if (FileObject->FileName.Length == sizeof(ULONGLONG))
    {
        MftId = (*(PULONGLONG)FileObject->FileName.Buffer) & NTFS_MFT_MASK;
    }
    else if (FileObject->FileName.Length == RTL_FIELD_SIZE(FILE_OBJECTID_BUFFER, ObjectId))
    {
        Status = NtfsFindFileByObjectId(DeviceExt, (PUCHAR)FileObject->FileName.Buffer, &MftId);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (MftId < NTFS_FILE_FIRST_USER_FILE)
        return NtfsOpenFileById(DeviceExt, MftId, Fcb);

    return NtfsMoonWalkID(DeviceExt, MftId, Path);
}

/**
* @name NtfsApplyStreamKind
*
* A typed stream part of a create's path says which part of the file is wanted: "::$DATA"
* the unnamed data stream, which a directory doesn't have, "::$INDEX_ALLOCATION" the
* directory index, which only a directory has. That's what FILE_NON_DIRECTORY_FILE and
* FILE_DIRECTORY_FILE ask for, so the create continues with that option.
*/
static
NTSTATUS
NtfsApplyStreamKind(
    _Inout_ PIO_STACK_LOCATION Stack,
    _In_ NTFS_STREAM_KIND StreamKind,
    _In_ BOOLEAN TrailingBackslash,
    _Inout_ PULONG Options)
{
    ULONG Wanted;

    switch (StreamKind)
    {
        case NtfsNamedStream:
            return TrailingBackslash ? STATUS_OBJECT_NAME_INVALID : STATUS_SUCCESS;

        case NtfsUnnamedDataStream:
            if (TrailingBackslash || BooleanFlagOn(*Options, FILE_DIRECTORY_FILE))
                return STATUS_OBJECT_NAME_INVALID;
            Wanted = FILE_NON_DIRECTORY_FILE;
            break;

        case NtfsIndexStream:
            if (BooleanFlagOn(*Options, FILE_NON_DIRECTORY_FILE))
                return STATUS_OBJECT_NAME_INVALID;
            Wanted = FILE_DIRECTORY_FILE;
            break;

        default:
            ASSERT(FALSE);
            return STATUS_SUCCESS;
    }

    *Options |= Wanted;
    Stack->Parameters.Create.Options |= Wanted;

    return STATUS_SUCCESS;
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
    UNICODE_STRING StreamName;
    NTFS_STREAM_KIND StreamKind = NtfsNoStream;
    BOOLEAN TrailingBackslash = FALSE;
    ULONG NameLength = 0, ReparseOffset = 0;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    DPRINT("NtfsCreateFile(%p, %p) called\n", DeviceObject, IrpContext);

    ASSERT(DeviceExt);

    Status = NtfsCheckCreateParameters(Disposition, Options, DesiredAccess,
                                       BooleanFlagOn(Stack->Flags, SL_OPEN_PAGING_FILE));
    if (!NT_SUCCESS(Status))
        return Status;

    /* Deny create if the volume is locked */
    if (DeviceExt->Flags & VCB_VOLUME_LOCKED)
    {
        return STATUS_ACCESS_DENIED;
    }

    /* Raw volume access is authorized by the device's security descriptor.
     * Offline format/repair must be able to lock and dismount even when
     * ordinary file mutations are refused by the log-recovery gate. */
    if (!(Options & FILE_OPEN_BY_FILE_ID) && !(Stack->Flags & SL_OPEN_TARGET_DIRECTORY) &&
        FileObject->FileName.Length == 0 &&
        (FileObject->RelatedFileObject == NULL ||
         BooleanFlagOn(((PNTFS_FCB)FileObject->RelatedFileObject->FsContext)->Flags, FCB_IS_VOLUME)))
        return NtfsOpenVolume(DeviceExt, Irp, FileObject, Disposition, Options);

    if ((DeviceExt->Flags & VCB_VOLUME_READ_ONLY) &&
        ((Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF) || (Options & FILE_DELETE_ON_CLOSE) ||
         (DesiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                           FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER))))
        return STATUS_MEDIA_WRITE_PROTECTED;

    /* The I/O manager opening a rename's destination directory */
    if (Stack->Flags & SL_OPEN_TARGET_DIRECTORY)
    {
        if (Options & FILE_OPEN_BY_FILE_ID)
            return STATUS_INVALID_PARAMETER;

        Status = NtfsBuildCreatePath(FileObject, &Path);
        if (!NT_SUCCESS(Status))
            return Status;
        Status = NtfsCheckCreateTraverse(DeviceExt, &Path, Stack->Parameters.Create.SecurityContext->AccessState,
                                          NtfsCreateAccessMode(Irp, Stack), CaseSensitive);
        ExFreePoolWithTag(Path.Buffer, TAG_NTFS);
        if (!NT_SUCCESS(Status))
            return Status;
        Status = NtfsOpenTargetDirectory(DeviceExt, FileObject, CaseSensitive, &Information);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = NtfsAuthorizeOpen(DeviceExt, FileObject->FsContext, Irp, Stack, TRUE);
        if (NT_SUCCESS(Status))
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
            return NtfsOpenExistingFile(DeviceExt, Irp, Stack, Fcb, FALSE, 0);

        DPRINT("Open by ID: %I64x -> %wZ\n", (*(PULONGLONG)FileObject->FileName.Buffer) & NTFS_MFT_MASK, &Path);
    }
    else
    {
        Status = NtfsBuildCreatePath(FileObject, &Path);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* What follows a reparse point in the name is measured against the whole of it */
    NameLength = Path.Length / sizeof(WCHAR);

    Status = NtfsCheckCreatePath(&Path, Options, &TrailingBackslash);
    if (NT_SUCCESS(Status))
        Status = NtfsSplitStreamName(&Path, &StreamName, &StreamKind);

    if (NT_SUCCESS(Status) && !(Options & FILE_OPEN_BY_FILE_ID))
        Status = NtfsCheckCreateTraverse(DeviceExt, &Path, Stack->Parameters.Create.SecurityContext->AccessState,
                                          NtfsCreateAccessMode(Irp, Stack), CaseSensitive);

    if (NT_SUCCESS(Status) && StreamKind != NtfsNoStream)
    {
        Status = NtfsApplyStreamKind(Stack, StreamKind, TrailingBackslash, &Options);
        if (NT_SUCCESS(Status) && StreamKind == NtfsNamedStream)
        {
            Status = NtfsOpenStream(DeviceExt, IrpContext, &Path, &StreamName, NameLength);
            ExFreePoolWithTag(Path.Buffer, TAG_NTFS);
            return Status;
        }
    }

    if (NT_SUCCESS(Status))
        Status = NtfsOpenFile(DeviceExt, Path.Buffer, CaseSensitive, &Fcb, &ParentFcb, &ReparseOffset);

    if (Status == STATUS_REPARSE)
    {
        Status = NtfsReparseOpen(DeviceExt, Irp, Fcb, (NameLength - ReparseOffset) * sizeof(WCHAR), TRUE);
    }
    else if (NT_SUCCESS(Status))
    {
        Status = NtfsOpenExistingFile(DeviceExt, Irp, Stack, Fcb, TrailingBackslash,
                                      NameLength * sizeof(WCHAR) - Path.Length);
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

    /* Mm opens its paging files this way, and only Mm does I/O to them */
    if (Status == STATUS_SUCCESS && (Stack->Flags & SL_OPEN_PAGING_FILE) &&
        FileObject->FsContext != NULL)
    {
        ((PNTFS_FCB)FileObject->FsContext)->Flags |= FCB_IS_PAGING_FILE;
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
    /* AddNewMftEntry replaces this initial value when reusing an old slot. */
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
* @param ParentReference
* File reference of the directory the file is created in.
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
* @param SecurityId
* The SecurityId of the file's descriptor in $Secure, from NtfsAssignSecurity().
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param CanWait
* Boolean indicating if the function is allowed to wait for exclusive access to the master file table.
* This will only be relevant if the MFT doesn't have any free file records and needs to be enlarged.
*
* @param FileReference
* Optionally receives the file reference of the new file.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if unable to allocate memory for the file record.
* STATUS_CANT_WAIT if CanWait was FALSE and the function needed to resize the MFT but
* couldn't get immediate, exclusive access to it.
*/
NTSTATUS
NtfsCreateFileRecord(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG ParentReference,
                     PCUNICODE_STRING Name,
                     ULONG FileAttributes,
                     BOOLEAN IsDirectory,
                     ULONG SecurityId,
                     BOOLEAN CaseSensitive,
                     BOOLEAN CanWait,
                     PULONGLONG NewFileReference)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD NextAttribute;
    PFILENAME_ATTRIBUTE FilenameAttribute, DosAttribute = NULL;
    ULONGLONG FileMftIndex;
    ULONGLONG FileReference;
    WCHAR DosBuffer[13];
    UNICODE_STRING DosName = {0, sizeof(DosBuffer), DosBuffer};
    GENERATE_NAME_CONTEXT NameContext = {0};

    DPRINT("NtfsCreateFileRecord(%p, 0x%I64x, '%wZ', 0x%lx, %s, %s, %s)\n",
           DeviceExt,
           ParentReference,
           Name,
           FileAttributes,
           IsDirectory ? "TRUE" : "FALSE",
           CaseSensitive ? "TRUE" : "FALSE",
           CanWait ? "TRUE" : "FALSE");

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
    ((PSTANDARD_INFORMATION)((ULONG_PTR)NextAttribute + NextAttribute->Resident.ValueOffset))->SecurityId = SecurityId;

    // advance NextAttribute pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
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

    if (FilenameAttribute->NameType == NTFS_FILE_NAME_WIN32)
    {
        ULONG FirstEntry, AttributeLength;
        ULONGLONG Existing;
        UNICODE_STRING AliasSource = *Name;

        /* Leading periods are not the extension separator of an NTFS
         * short name (for example, ".config" has a base, not an extension). */
        while (AliasSource.Length > sizeof(WCHAR) && AliasSource.Buffer[0] == L'.')
        {
            AliasSource.Buffer++;
            AliasSource.Length -= sizeof(WCHAR);
        }
        AliasSource.MaximumLength = AliasSource.Length;

        /* The parent is protected by DirResource across name generation
         * and both index insertions, so an alias cannot be stolen between them. */
        do
        {
            RtlGenerate8dot3Name(&AliasSource, FALSE, &NameContext, &DosName);
            FirstEntry = 0;
            Status = NtfsFindMftRecord(DeviceExt, ParentReference & NTFS_MFT_MASK,
                                       &DosName, &FirstEntry, FALSE, FALSE, &Existing, NULL);
        } while (NT_SUCCESS(Status));
        if (Status != STATUS_OBJECT_NAME_NOT_FOUND && Status != STATUS_OBJECT_PATH_NOT_FOUND)
            goto FreeRecord;

        NextAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)NextAttribute + NextAttribute->Length);
        AttributeLength = ALIGN_UP_BY(FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR) +
                                      FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + DosName.Length,
                                      ATTR_RECORD_ALIGNMENT);
        if ((PUCHAR)NextAttribute - (PUCHAR)FileRecord + AttributeLength + 2 * sizeof(ULONG) >
            DeviceExt->NtfsInfo.BytesPerFileRecord)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto FreeRecord;
        }
        Status = AddFileName(FileRecord, NextAttribute, ParentReference, &DosName, FALSE, FileAttributes);
        if (!NT_SUCCESS(Status))
            goto FreeRecord;
        DosAttribute = (PFILENAME_ATTRIBUTE)((PUCHAR)NextAttribute + NextAttribute->Resident.ValueOffset);
        RtlCopyMemory(DosAttribute, FilenameAttribute, FIELD_OFFSET(FILENAME_ATTRIBUTE, NameLength));
        DosAttribute->NameType = NTFS_FILE_NAME_DOS;
        FileRecord->LinkCount--; /* The alias is the same directory link. */
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
                                            ParentReference & NTFS_MFT_MASK,
                                            FileReference,
                                            FilenameAttribute,
                                            CaseSensitive);
        if (NT_SUCCESS(Status) && DosAttribute)
        {
            Status = NtfsAddFilenameToDirectory(DeviceExt, ParentReference & NTFS_MFT_MASK,
                                                 FileReference, DosAttribute, FALSE);
            if (!NT_SUCCESS(Status) &&
                !NT_SUCCESS(NtfsRemoveFilenameFromDirectory(DeviceExt, ParentReference & NTFS_MFT_MASK,
                                                            FilenameAttribute, CaseSensitive)))
            {
                NtfsMarkVolumeCorrupt(DeviceExt);
                goto FreeRecord;
            }
        }
        if (!NT_SUCCESS(Status))
        {
            /* Nothing refers to the record, so give it back */
            FreeMftEntry(DeviceExt, FileMftIndex);
        }
        else if (NewFileReference != NULL)
        {
            *NewFileReference = FileReference;
        }
    }

FreeRecord:
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/* EOF */
