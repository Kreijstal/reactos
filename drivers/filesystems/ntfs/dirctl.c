/*
 *  ReactOS kernel
 *  Copyright (C) 2002, 2003, 2014 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/dirctl.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Pierre Schweitzer (pierre@reactos.org)
 *                   Hervé Poussineau (hpoussin@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

ULONGLONG
NtfsGetFileSize(PDEVICE_EXTENSION DeviceExt,
                PFILE_RECORD_HEADER FileRecord,
                PCWSTR Stream,
                ULONG StreamLength,
                PULONGLONG AllocatedSize)
{
    ULONGLONG Size = 0ULL;
    ULONGLONG Allocated = 0ULL;
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT DataContext;

    Status = FindAttribute(DeviceExt, FileRecord, AttributeData, Stream, StreamLength, &DataContext, NULL);
    if (NT_SUCCESS(Status))
    {
        Size = AttributeDataLength(DataContext->pRecord);
        Allocated = AttributeAllocatedLength(DataContext->pRecord);
        ReleaseAttributeContext(DataContext);
    }

    if (AllocatedSize != NULL) *AllocatedSize = Allocated;

    return Size;
}


/**
* @name NtfsDirEntryNameOffset
*
* Offset of FileName in an entry of a directory information class, or 0 for a class
* this driver doesn't enumerate.
*/
static
ULONG
NtfsDirEntryNameOffset(
    _In_ FILE_INFORMATION_CLASS FileInformationClass)
{
    switch (FileInformationClass)
    {
        case FileNamesInformation:
            return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
        case FileDirectoryInformation:
            return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
        case FileFullDirectoryInformation:
            return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
        case FileBothDirectoryInformation:
            return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
        case FileIdFullDirectoryInformation:
            return FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName);
        case FileIdBothDirectoryInformation:
            return FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName);
        default:
            return 0;
    }
}

/**
* @name NtfsFillDirEntryFixedPart
*
* Fills what every class but FileNamesInformation reports about a file, all of it laid
* out the same way FILE_DIRECTORY_INFORMATION lays it out, plus the class's own fields.
*/
static
VOID
NtfsFillDirEntryFixedPart(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ FILE_INFORMATION_CLASS FileInformationClass,
    _In_ PFILE_RECORD_HEADER FileRecord,
    _In_ ULONGLONG MFTIndex,
    _In_ PFILENAME_ATTRIBUTE FileName,
    _In_ BOOLEAN ReportShortName,
    _Out_ PVOID Buffer)
{
    PFILE_DIRECTORY_INFORMATION Info = Buffer;
    PSTANDARD_INFORMATION StdInfo;
    PFILENAME_ATTRIBUTE ShortName;
    LARGE_INTEGER FileId;

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    ASSERT(StdInfo != NULL);

    Info->CreationTime.QuadPart = StdInfo->CreationTime;
    Info->LastAccessTime.QuadPart = StdInfo->LastAccessTime;
    Info->LastWriteTime.QuadPart = StdInfo->LastWriteTime;
    Info->ChangeTime.QuadPart = StdInfo->ChangeTime;
    NtfsFileFlagsToAttributes(FileName->FileAttributes | StdInfo->FileAttribute, &Info->FileAttributes);
    Info->EndOfFile.QuadPart = NtfsGetFileSize(DeviceExt, FileRecord, L"", 0, (PULONGLONG)&Info->AllocationSize.QuadPart);

    /* The file reference, as FileInternalInformation reports it */
    FileId.QuadPart = MFTIndex | ((ULONGLONG)FileRecord->SequenceNumber << 48);

    /* A reparse point reports its tag where other files report their EA size */
    if (FileInformationClass != FileDirectoryInformation)
    {
        ((PFILE_FULL_DIR_INFORMATION)Buffer)->EaSize = (StdInfo->FileAttribute & NTFS_FILE_TYPE_REPARSE) ?
            NtfsGetReparseTag(DeviceExt, FileRecord) : NtfsGetEaSize(DeviceExt, FileRecord);
    }

    /* A short name is only reported for a real entry, not for "." or "..". */
    ShortName = ReportShortName ? NtfsGetDosNameFromRecord(DeviceExt, FileRecord, FileName) : NULL;
    if (ShortName != NULL && ShortName->NameType == NTFS_FILE_NAME_DOS)
    {
        PFILE_BOTH_DIR_INFORMATION Both = Buffer;

        ASSERT(ShortName->NameLength <= RTL_NUMBER_OF(Both->ShortName));
        if (FileInformationClass == FileBothDirectoryInformation ||
            FileInformationClass == FileIdBothDirectoryInformation)
        {
            Both->ShortNameLength = ShortName->NameLength * sizeof(WCHAR);
            RtlCopyMemory(Both->ShortName, ShortName->Name, Both->ShortNameLength);
        }
    }

    if (FileInformationClass == FileIdFullDirectoryInformation)
        ((PFILE_ID_FULL_DIR_INFORMATION)Buffer)->FileId = FileId;
    else if (FileInformationClass == FileIdBothDirectoryInformation)
        ((PFILE_ID_BOTH_DIR_INFORMATION)Buffer)->FileId = FileId;
}

/**
* @name NtfsGetDirEntryInformation
*
* Describes one file as an entry of a directory information class.
*
* @param OverrideName
* The name to report: the one the index entry holds, or "." or "..". NULL reports the
* file's best name.
*
* @param Written
* Receives the bytes of Buffer the entry takes, name included.
*
* @param First
* TRUE for the first entry of a query, which is returned cut short if the buffer can't
* hold its name; later entries that don't fit are left for the next query.
*
* @param IsDotEntry
* TRUE for "." and "..", which have no short name.
*
* @return
* STATUS_SUCCESS, STATUS_BUFFER_OVERFLOW if the entry doesn't fit, or
* STATUS_INVALID_INFO_CLASS.
*/
static NTSTATUS
NtfsGetDirEntryInformation(PDEVICE_EXTENSION DeviceExt,
                           FILE_INFORMATION_CLASS FileInformationClass,
                           PFILE_RECORD_HEADER FileRecord,
                           ULONGLONG MFTIndex,
                           PCUNICODE_STRING OverrideName,
                           PVOID Buffer,
                           ULONG BufferLength,
                           PULONG Written,
                           BOOLEAN First,
                           BOOLEAN IsDotEntry)
{
    ULONG NameOffset = NtfsDirEntryNameOffset(FileInformationClass);
    PFILENAME_ATTRIBUTE FileName;
    PCWSTR NameBuffer;
    ULONG Length;
    ULONG BytesToCopy;

    *Written = 0;

    if (NameOffset == 0)
        return STATUS_INVALID_INFO_CLASS;

    if (BufferLength < NameOffset)
        return STATUS_BUFFER_OVERFLOW;

    FileName = GetBestFileNameFromRecord(DeviceExt, FileRecord);
    if (FileName == NULL)
    {
        DPRINT1("No name information for file ID: %#I64x\n", MFTIndex);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if (OverrideName != NULL)
    {
        NameBuffer = OverrideName->Buffer;
        Length = OverrideName->Length;
    }
    else
    {
        NameBuffer = FileName->Name;
        Length = FileName->NameLength * sizeof(WCHAR);
    }

    if (!First && BufferLength < NameOffset + Length)
        return STATUS_BUFFER_OVERFLOW;

    RtlZeroMemory(Buffer, NameOffset);

    if (FileInformationClass == FileNamesInformation)
    {
        ((PFILE_NAMES_INFORMATION)Buffer)->FileNameLength = Length;
    }
    else
    {
        NtfsFillDirEntryFixedPart(DeviceExt, FileInformationClass, FileRecord, MFTIndex,
                                  FileName, !IsDotEntry, Buffer);
        ((PFILE_DIRECTORY_INFORMATION)Buffer)->FileNameLength = Length;
    }

    BytesToCopy = min(Length, BufferLength - NameOffset);
    RtlCopyMemory((PUCHAR)Buffer + NameOffset, NameBuffer, BytesToCopy);
    *Written = NameOffset + BytesToCopy;

    if (BytesToCopy < Length)
        return STATUS_BUFFER_OVERFLOW;

    /* Entries start on 8-byte boundaries, as the classes hold LARGE_INTEGERs */
    ((PFILE_NAMES_INFORMATION)Buffer)->NextEntryOffset = ALIGN_UP_BY(NameOffset + Length, sizeof(LONGLONG));

    return STATUS_SUCCESS;
}

/*
 * NTFS keeps no "." or ".." entries in its indices, so enumeration has to make
 * them up. ntfs.sys emits them ahead of the real entries and filters them
 * through the caller's search pattern; these do the same.
 */
static BOOLEAN
NtfsDotNameMatches(PUNICODE_STRING Pattern,
                   PUNICODE_STRING Name,
                   BOOLEAN CaseSensitive)
{
    UNICODE_STRING Expression;
    UNICODE_STRING DosName = RTL_CONSTANT_STRING(L".");
    BOOLEAN Ret;

    if (!FsRtlDoesNameContainWildCards(Pattern))
    {
        return (RtlCompareUnicodeString(Pattern, Name, !CaseSensitive) == 0);
    }

    if (Pattern->Length > MAXUSHORT - sizeof(WCHAR))
        return FALSE;
    Expression.Length = Pattern->Length;
    Expression.MaximumLength = Pattern->Length + sizeof(WCHAR);
    Expression.Buffer = ExAllocatePoolWithTag(PagedPool, Expression.MaximumLength, TAG_NTFS);
    if (Expression.Buffer == NULL)
        return FALSE;

    if (CaseSensitive)
        RtlCopyMemory(Expression.Buffer, Pattern->Buffer, Pattern->Length);
    else
        RtlUpcaseUnicodeString(&Expression, Pattern, FALSE);

    /* Both pseudoentries have an empty DOS base and extension. Match a
     * single separator with an optional final dot, including DOS_QM masks. */
    Expression.Buffer[Expression.Length / sizeof(WCHAR)] = L'"';
    Expression.Length += sizeof(WCHAR);
    Ret = FsRtlIsNameInExpression(&Expression, &DosName, !CaseSensitive, NULL);
    ExFreePoolWithTag(Expression.Buffer, TAG_NTFS);

    return Ret;
}

/* Describes MFTIndex under the given name. "." reports the directory itself,
 * ".." its parent, so both take their times and attributes from a real record. */
static NTSTATUS
NtfsGetDotEntry(PDEVICE_EXTENSION DeviceExt,
                ULONGLONG MFTIndex,
                PCUNICODE_STRING Name,
                FILE_INFORMATION_CLASS FileInformationClass,
                PVOID Buffer,
                ULONG BufferLength,
                PULONG Written,
                BOOLEAN First)
{
    PFILE_RECORD_HEADER FileRecord;
    NTSTATUS Status;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsGetDirEntryInformation(DeviceExt, FileInformationClass, FileRecord,
                                            MFTIndex, Name, Buffer, BufferLength,
                                            Written, First, TRUE);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

static ULONGLONG
NtfsGetParentMftIndex(PDEVICE_EXTENSION DeviceExt,
                      ULONGLONG MFTIndex)
{
    PFILE_RECORD_HEADER FileRecord;
    PFILENAME_ATTRIBUTE FileName;
    ULONGLONG Parent = MFTIndex;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        return Parent;
    }

    if (NT_SUCCESS(ReadFileRecord(DeviceExt, MFTIndex, FileRecord)))
    {
        FileName = GetBestFileNameFromRecord(DeviceExt, FileRecord);
        if (FileName != NULL)
        {
            Parent = FileName->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
        }
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Parent;
}

NTSTATUS
NtfsQueryDirectory(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_EXTENSION DeviceExtension;
    ULONG BufferLength = 0;
    ULONG Offset = 0, LastOffset = 0, LastWritten = 0;
    PUNICODE_STRING SearchPattern = NULL;
    FILE_INFORMATION_CLASS FileInformationClass;
    ULONG FileIndex = 0;
    PUCHAR Buffer = NULL;
    PFILE_NAMES_INFORMATION Buffer0 = NULL;
    PNTFS_FCB Fcb;
    PNTFS_CCB Ccb;
    BOOLEAN First = FALSE;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER FileRecord;
    ULONGLONG MFTRecord;
    PFILENAME_ATTRIBUTE FoundName;
    UNICODE_STRING Pattern;
    ULONG Written;

    DPRINT("NtfsQueryDirectory() called\n");

    ASSERT(IrpContext);
    Irp = IrpContext->Irp;
    DeviceObject = IrpContext->DeviceObject;

    DeviceExtension = DeviceObject->DeviceExtension;
    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;

    Ccb = (PNTFS_CCB)FileObject->FsContext2;
    Fcb = (PNTFS_FCB)FileObject->FsContext;

    /* Obtain the callers parameters */
    BufferLength = Stack->Parameters.QueryDirectory.Length;
    SearchPattern = Stack->Parameters.QueryDirectory.FileName;
    FileInformationClass = Stack->Parameters.QueryDirectory.FileInformationClass;
    FileIndex = Stack->Parameters.QueryDirectory.FileIndex;

    /* Match create, cleanup, rename and notification completion: volume
     * directory resource first, then the FCB resource. */
    if (!ExAcquireResourceExclusiveLite(&DeviceExtension->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
        return STATUS_PENDING;
    if (!ExAcquireResourceSharedLite(&Fcb->MainResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        ExReleaseResourceLite(&DeviceExtension->DirResource);
        return STATUS_PENDING;
    }

    if (SearchPattern != NULL)
    {
        if (!Ccb->DirectorySearchPattern)
        {
            First = TRUE;
            Pattern.Length = 0;
            Pattern.MaximumLength = SearchPattern->Length + sizeof(WCHAR);
            Ccb->DirectorySearchPattern = Pattern.Buffer =
                ExAllocatePoolWithTag(NonPagedPool, Pattern.MaximumLength, TAG_NTFS);
            if (!Ccb->DirectorySearchPattern)
            {
                ExReleaseResourceLite(&Fcb->MainResource);
                ExReleaseResourceLite(&DeviceExtension->DirResource);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            memcpy(Ccb->DirectorySearchPattern, SearchPattern->Buffer, SearchPattern->Length);
            Ccb->DirectorySearchPattern[SearchPattern->Length / sizeof(WCHAR)] = 0;
        }
    }
    else if (!Ccb->DirectorySearchPattern)
    {
        First = TRUE;
        Ccb->DirectorySearchPattern = ExAllocatePoolWithTag(NonPagedPool, 2 * sizeof(WCHAR), TAG_NTFS);
        if (!Ccb->DirectorySearchPattern)
        {
            ExReleaseResourceLite(&Fcb->MainResource);
            ExReleaseResourceLite(&DeviceExtension->DirResource);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Ccb->DirectorySearchPattern[0] = L'*';
        Ccb->DirectorySearchPattern[1] = 0;
    }

    RtlInitUnicodeString(&Pattern, Ccb->DirectorySearchPattern);
    DPRINT("Search pattern '%S'\n", Ccb->DirectorySearchPattern);
    DPRINT("In: '%S'\n", Fcb->PathName);

    /* Determine directory index */
    if (Stack->Flags & SL_INDEX_SPECIFIED)
    {
        Ccb->Entry = FileIndex;
        SetFlag(Ccb->Flags, CCB_RETURNED_DOT | CCB_RETURNED_DOTDOT);
    }
    else if (First || (Stack->Flags & SL_RESTART_SCAN))
    {
        Ccb->Entry = 0;
        ClearFlag(Ccb->Flags, CCB_RETURNED_DOT | CCB_RETURNED_DOTDOT);
    }

    /* The volume root has no parent and does not enumerate pseudoentries. */
    if (NtfsFCBIsRoot(Fcb))
        SetFlag(Ccb->Flags, CCB_RETURNED_DOT | CCB_RETURNED_DOTDOT);

    /* Get Buffer for result */
    Buffer = NtfsGetUserBuffer(Irp, FALSE);

    FoundName = ExAllocatePoolWithTag(NonPagedPool, NTFS_FOUND_NAME_SIZE, TAG_NTFS);
    if (FoundName == NULL)
    {
        ExReleaseResourceLite(&Fcb->MainResource);
        ExReleaseResourceLite(&DeviceExtension->DirResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DPRINT("Buffer=%p tofind=%S\n", Buffer, Ccb->DirectorySearchPattern);

    Written = 0;
    while (Status == STATUS_SUCCESS && Offset < BufferLength)
    {
        PUCHAR Entry = Buffer + Offset;

        if (!BooleanFlagOn(Ccb->Flags, CCB_RETURNED_DOT) ||
            !BooleanFlagOn(Ccb->Flags, CCB_RETURNED_DOTDOT))
        {
            BOOLEAN DotDot = BooleanFlagOn(Ccb->Flags, CCB_RETURNED_DOT);
            UNICODE_STRING DotName;
            ULONGLONG DotIndex;

            RtlInitUnicodeString(&DotName, DotDot ? L".." : L".");

            if (!NtfsDotNameMatches(&Pattern, &DotName,
                                    BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE)))
            {
                SetFlag(Ccb->Flags, DotDot ? CCB_RETURNED_DOTDOT : CCB_RETURNED_DOT);
                continue;
            }

            DotIndex = DotDot ? NtfsGetParentMftIndex(DeviceExtension, Fcb->MFTIndex)
                              : Fcb->MFTIndex;

            Status = NtfsGetDotEntry(DeviceExtension, DotIndex, &DotName,
                                     FileInformationClass, Entry, BufferLength - Offset,
                                     &Written, Buffer0 == NULL);

            /* The entry did not fit. Leave the flag alone so the next call,
             * with a bigger buffer, hands it out instead of skipping it. */
            if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_INVALID_INFO_CLASS)
            {
                break;
            }

            SetFlag(Ccb->Flags, DotDot ? CCB_RETURNED_DOTDOT : CCB_RETURNED_DOT);

            if (!NT_SUCCESS(Status))
            {
                /* Losing "." is not worth failing the whole enumeration over */
                Status = STATUS_SUCCESS;
                continue;
            }
        }
        else
        {
            UNICODE_STRING EntryName;

            /* The index has one entry per name; a DOS-only name is skipped by the
             * search, and every other entry is reported under the name it holds,
             * which for a file with several links is that link's */
            Status = NtfsFindFileAt(DeviceExtension,
                                    &Pattern,
                                    &Ccb->Entry,
                                    &FileRecord,
                                    &MFTRecord,
                                    Fcb->MFTIndex,
                                    BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                    FoundName);
            if (!NT_SUCCESS(Status))
            {
                Status = (First ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES);
                break;
            }

            EntryName.Buffer = FoundName->Name;
            EntryName.Length = EntryName.MaximumLength = FoundName->NameLength * sizeof(WCHAR);

            Status = NtfsGetDirEntryInformation(DeviceExtension,
                                                FileInformationClass,
                                                FileRecord,
                                                MFTRecord,
                                                &EntryName,
                                                Entry,
                                                BufferLength - Offset,
                                                &Written,
                                                Buffer0 == NULL,
                                                FALSE);

            ExFreeToNPagedLookasideList(&DeviceExtension->FileRecLookasideList, FileRecord);

            if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_INVALID_INFO_CLASS)
            {
                break;
            }

            Ccb->Entry++;
        }

        Buffer0 = (PFILE_NAMES_INFORMATION)Entry;
        Buffer0->FileIndex = FileIndex++;
        LastOffset = Offset;
        LastWritten = Written;

        if (Stack->Flags & SL_RETURN_SINGLE_ENTRY)
        {
            break;
        }

        Offset += Buffer0->NextEntryOffset;
    }

    if (Buffer0)
    {
        /* Whatever stopped the scan, what was already returned stands */
        Buffer0->NextEntryOffset = 0;
        Status = STATUS_SUCCESS;
        IrpContext->Irp->IoStatus.Information = LastOffset + LastWritten;
    }
    else
    {
        /* Nothing returned: a first entry too big for the buffer comes back cut short */
        ASSERT(Status != STATUS_SUCCESS);
        ASSERT(Written <= BufferLength);
        IrpContext->Irp->IoStatus.Information = (Status == STATUS_BUFFER_OVERFLOW) ? Written : 0;
    }

    ExFreePoolWithTag(FoundName, TAG_NTFS);
    ExReleaseResourceLite(&Fcb->MainResource);
    ExReleaseResourceLite(&DeviceExtension->DirResource);

    return Status;
}


NTSTATUS
NtfsDirectoryControl(PNTFS_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    DPRINT("NtfsDirectoryControl() called\n");

    /* Handlers that fill the caller's buffer overwrite this with the byte count */
    IrpContext->Irp->IoStatus.Information = 0;

    switch (IrpContext->MinorFunction)
    {
        case IRP_MN_QUERY_DIRECTORY:
            Status = NtfsQueryDirectory(IrpContext);
            break;

        case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
            Status = NtfsNotifyChangeDirectory(IrpContext);
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    if (Status == STATUS_PENDING && IrpContext->Flags & IRPCONTEXT_COMPLETE)
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    return Status;
}

/* EOF */
