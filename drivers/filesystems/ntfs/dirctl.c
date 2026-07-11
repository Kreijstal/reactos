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


#define ULONG_ROUND_UP(x)   ROUND_UP((x), (sizeof(ULONG)))


static NTSTATUS
NtfsGetNamesInformation(PDEVICE_EXTENSION DeviceExt,
                        PFILE_RECORD_HEADER FileRecord,
                        ULONGLONG MFTIndex,
                        PFILENAME_ATTRIBUTE MatchedName,
                        PFILE_NAMES_INFORMATION Info,
                        ULONG BufferLength,
                        PULONG Written,
                        BOOLEAN First)
{
    ULONG Length;
    NTSTATUS Status;
    ULONG BytesToCopy = 0;
    PFILENAME_ATTRIBUTE FileName;

    DPRINT("NtfsGetNamesInformation() called\n");

    *Written = 0;
    Status = STATUS_BUFFER_OVERFLOW;
    if (FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) > BufferLength)
    {
        return Status;
    }

    /* Report the name of the directory index entry that matched the search.
     * The record's own "best" name can be a different hard link. */
    FileName = MatchedName;
    if (FileName == NULL)
        FileName = GetBestFileNameFromRecord(DeviceExt, FileRecord, NULL);
    if (FileName == NULL)
    {
        DPRINT1("No name information for file ID: %#I64x\n", MFTIndex);
        NtfsDumpFileAttributes(DeviceExt, FileRecord);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Length = FileName->NameLength * sizeof (WCHAR);
    if (First || (BufferLength >= FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) + Length))
    {
        Info->FileNameLength = Length;

        *Written = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
        Info->NextEntryOffset = 0;
        if (BufferLength > FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName))
        {
            BytesToCopy = min(Length, BufferLength - FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName));
            RtlCopyMemory(Info->FileName, FileName->Name, BytesToCopy);
            *Written += BytesToCopy;

            if (BytesToCopy == Length)
            {
                Info->NextEntryOffset = ULONG_ROUND_UP(sizeof(FILE_NAMES_INFORMATION) +
                                                       BytesToCopy);
                Status = STATUS_SUCCESS;
            }
        }
    }

    return Status;
}


static NTSTATUS
NtfsGetDirectoryInformation(PDEVICE_EXTENSION DeviceExt,
                            PFILE_RECORD_HEADER FileRecord,
                            ULONGLONG MFTIndex,
                            PFILENAME_ATTRIBUTE MatchedName,
                            PFILE_DIRECTORY_INFORMATION Info,
                            ULONG BufferLength,
                            PULONG Written,
                            BOOLEAN First)
{
    ULONG Length;
    NTSTATUS Status;
    ULONG BytesToCopy = 0;
    PFILENAME_ATTRIBUTE FileName;
    PSTANDARD_INFORMATION StdInfo;

    DPRINT("NtfsGetDirectoryInformation() called\n");

    *Written = 0;
    Status = STATUS_BUFFER_OVERFLOW;
    if (FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) > BufferLength)
    {
        return Status;
    }

    /* Report the name of the directory index entry that matched the search.
     * The record's own "best" name can be a different hard link. */
    FileName = MatchedName;
    if (FileName == NULL)
        FileName = GetBestFileNameFromRecord(DeviceExt, FileRecord, NULL);
    if (FileName == NULL)
    {
        DPRINT1("No name information for file ID: %#I64x\n", MFTIndex);
        NtfsDumpFileAttributes(DeviceExt, FileRecord);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    ASSERT(StdInfo != NULL);

    Length = FileName->NameLength * sizeof (WCHAR);
    if (First || (BufferLength >= FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) + Length))
    {
        Info->FileNameLength = Length;

        *Written = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
        Info->NextEntryOffset = 0;
        if (BufferLength > FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName))
        {
            BytesToCopy = min(Length, BufferLength - FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName));
            RtlCopyMemory(Info->FileName, FileName->Name, BytesToCopy);
            *Written += BytesToCopy;

            if (BytesToCopy == Length)
            {
                Info->NextEntryOffset = ULONG_ROUND_UP(sizeof(FILE_DIRECTORY_INFORMATION) +
                                                       BytesToCopy);
                Status = STATUS_SUCCESS;
            }
        }

        Info->CreationTime.QuadPart = FileName->CreationTime;
        Info->LastAccessTime.QuadPart = FileName->LastAccessTime;
        Info->LastWriteTime.QuadPart = FileName->LastWriteTime;
        Info->ChangeTime.QuadPart = FileName->ChangeTime;

        /* Convert file flags */
        NtfsFileFlagsToAttributes(FileName->FileAttributes | StdInfo->FileAttribute, &Info->FileAttributes);

        Info->EndOfFile.QuadPart = NtfsGetFileSize(DeviceExt, FileRecord, L"", 0, (PULONGLONG)&Info->AllocationSize.QuadPart);

        Info->FileIndex = MFTIndex;
    }

    return Status;
}


static NTSTATUS
NtfsGetFullDirectoryInformation(PDEVICE_EXTENSION DeviceExt,
                                PFILE_RECORD_HEADER FileRecord,
                                ULONGLONG MFTIndex,
                                PFILENAME_ATTRIBUTE MatchedName,
                                PFILE_FULL_DIRECTORY_INFORMATION Info,
                                ULONG BufferLength,
                                PULONG Written,
                                BOOLEAN First)
{
    ULONG Length;
    NTSTATUS Status;
    ULONG BytesToCopy = 0;
    PFILENAME_ATTRIBUTE FileName;
    PSTANDARD_INFORMATION StdInfo;

    DPRINT("NtfsGetFullDirectoryInformation() called\n");

    *Written = 0;
    Status = STATUS_BUFFER_OVERFLOW;
    if (FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) > BufferLength)
    {
        return Status;
    }

    /* Report the name of the directory index entry that matched the search.
     * The record's own "best" name can be a different hard link. */
    FileName = MatchedName;
    if (FileName == NULL)
        FileName = GetBestFileNameFromRecord(DeviceExt, FileRecord, NULL);
    if (FileName == NULL)
    {
        DPRINT1("No name information for file ID: %#I64x\n", MFTIndex);
        NtfsDumpFileAttributes(DeviceExt, FileRecord);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    ASSERT(StdInfo != NULL);

    Length = FileName->NameLength * sizeof (WCHAR);
    if (First || (BufferLength >= FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) + Length))
    {
        Info->FileNameLength = Length;

        *Written = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
        Info->NextEntryOffset = 0;
        if (BufferLength > FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName))
        {
            BytesToCopy = min(Length, BufferLength - FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName));
            RtlCopyMemory(Info->FileName, FileName->Name, BytesToCopy);
            *Written += BytesToCopy;

            if (BytesToCopy == Length)
            {
                Info->NextEntryOffset = ULONG_ROUND_UP(sizeof(FILE_FULL_DIR_INFORMATION) +
                                                       BytesToCopy);
                Status = STATUS_SUCCESS;
            }
        }

        Info->CreationTime.QuadPart = FileName->CreationTime;
        Info->LastAccessTime.QuadPart = FileName->LastAccessTime;
        Info->LastWriteTime.QuadPart = FileName->LastWriteTime;
        Info->ChangeTime.QuadPart = FileName->ChangeTime;

        /* Convert file flags */
        NtfsFileFlagsToAttributes(FileName->FileAttributes | StdInfo->FileAttribute, &Info->FileAttributes);

        Info->EndOfFile.QuadPart = NtfsGetFileSize(DeviceExt, FileRecord, L"", 0, (PULONGLONG)&Info->AllocationSize.QuadPart);

        Info->FileIndex = MFTIndex;
        Info->EaSize = 0;
    }

    return Status;
}


static NTSTATUS
NtfsGetBothDirectoryInformation(PDEVICE_EXTENSION DeviceExt,
                                PFILE_RECORD_HEADER FileRecord,
                                ULONGLONG MFTIndex,
                                PFILENAME_ATTRIBUTE MatchedName,
                                PFILE_BOTH_DIR_INFORMATION Info,
                                ULONG BufferLength,
                                PULONG Written,
                                BOOLEAN First)
{
    ULONG Length;
    NTSTATUS Status;
    ULONG BytesToCopy = 0;
    PFILENAME_ATTRIBUTE FileName, ShortFileName;
    PSTANDARD_INFORMATION StdInfo;
    UCHAR ShortNameBuf[NTFS_FOUND_NAME_SIZE];

    DPRINT("NtfsGetBothDirectoryInformation() called\n");

    *Written = 0;
    Status = STATUS_BUFFER_OVERFLOW;
    if (FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) > BufferLength)
    {
        return Status;
    }

    /* Report the name of the directory index entry that matched the search.
     * The record's own "best" name can be a different hard link. */
    FileName = MatchedName;
    if (FileName == NULL)
        FileName = GetBestFileNameFromRecord(DeviceExt, FileRecord, NULL);
    if (FileName == NULL)
    {
        DPRINT1("No name information for file ID: %#I64x\n", MFTIndex);
        NtfsDumpFileAttributes(DeviceExt, FileRecord);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    ShortFileName = GetFileNameFromRecord(DeviceExt, FileRecord, NTFS_FILE_NAME_DOS,
                                           (PFILENAME_ATTRIBUTE)ShortNameBuf);

    /* A DOS 8.3 alias pairs with the WIN32 name in its own directory only:
     * for a hard-linked record, don't attach another link's short name. */
    if (ShortFileName != NULL &&
        (ShortFileName->DirectoryFileReferenceNumber & NTFS_MFT_MASK) !=
            (FileName->DirectoryFileReferenceNumber & NTFS_MFT_MASK))
    {
        ShortFileName = NULL;
    }

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    ASSERT(StdInfo != NULL);

    Length = FileName->NameLength * sizeof (WCHAR);
    if (First || (BufferLength >= FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) + Length))
    {
        Info->FileNameLength = Length;

        *Written = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
        Info->NextEntryOffset = 0;
        if (BufferLength > FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName))
        {
            BytesToCopy = min(Length, BufferLength - FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName));
            RtlCopyMemory(Info->FileName, FileName->Name, BytesToCopy);
            *Written += BytesToCopy;

            if (BytesToCopy == Length)
            {
                Info->NextEntryOffset = ULONG_ROUND_UP(sizeof(FILE_BOTH_DIR_INFORMATION) +
                                                       BytesToCopy);
                Status = STATUS_SUCCESS;
            }
        }

        if (ShortFileName)
        {
            /* Should we upcase the filename? */
            ASSERT(ShortFileName->NameLength <= ARRAYSIZE(Info->ShortName));
            Info->ShortNameLength = ShortFileName->NameLength * sizeof(WCHAR);
            RtlCopyMemory(Info->ShortName, ShortFileName->Name, Info->ShortNameLength);
        }
        else
        {
            Info->ShortName[0] = 0;
            Info->ShortNameLength = 0;
        }

        Info->CreationTime.QuadPart = FileName->CreationTime;
        Info->LastAccessTime.QuadPart = FileName->LastAccessTime;
        Info->LastWriteTime.QuadPart = FileName->LastWriteTime;
        Info->ChangeTime.QuadPart = FileName->ChangeTime;

        /* Convert file flags */
        NtfsFileFlagsToAttributes(FileName->FileAttributes | StdInfo->FileAttribute, &Info->FileAttributes);

        Info->EndOfFile.QuadPart = NtfsGetFileSize(DeviceExt, FileRecord, L"", 0, (PULONGLONG)&Info->AllocationSize.QuadPart);

        Info->FileIndex = MFTIndex;
        Info->EaSize = 0;
    }

    return Status;
}


NTSTATUS
NtfsQueryDirectory(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_EXTENSION DeviceExtension;
    LONG BufferLength = 0;
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
    UNICODE_STRING Pattern;
    ULONG Written;
    PFILENAME_ATTRIBUTE FoundName;

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

    if (NtfsFCBIsCompressed(Fcb))
    {
        DPRINT1("Compressed directory!\n");
        UNIMPLEMENTED;
        return STATUS_NOT_IMPLEMENTED;
    }

    /* Acquire DirResource first to maintain consistent lock ordering
       with NtfsCreate (DirResource -> MainResource). */
    if (!ExAcquireResourceExclusiveLite(&DeviceExtension->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return STATUS_PENDING;
    }

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
    }
    else if (First || (Stack->Flags & SL_RESTART_SCAN))
    {
        Ccb->Entry = 0;
    }

    /* Get Buffer for result */
    Buffer = NtfsGetUserBuffer(Irp, FALSE);

    DPRINT("Buffer=%p tofind=%S\n", Buffer, Ccb->DirectorySearchPattern);

    /* Synthesize '.' and '..' entries (indices 0 and 1).
     * NTFS does not store them in the $I30 index - the Windows NTFS
     * driver generates them on the fly.  Without them, FindFirstFile
     * on an empty directory returns ERROR_FILE_NOT_FOUND instead of
     * finding at least '.' and '..', which breaks CopyDirectory. */
    Written = 0;
    while (Ccb->Entry < 2 && Status == STATUS_SUCCESS && BufferLength > 0)
    {
        WCHAR DotName[3];
        ULONG DotLen;
        ULONGLONG DotMft;
        UNICODE_STRING DotStr;

        if (Ccb->Entry == 0)
        {
            DotName[0] = L'.'; DotLen = 1;
            DotMft = Fcb->MFTIndex;
        }
        else
        {
            DotName[0] = L'.'; DotName[1] = L'.'; DotLen = 2;
            /* Root directory's parent is itself */
            if (Fcb->MFTIndex == NTFS_FILE_ROOT)
                DotMft = NTFS_FILE_ROOT;
            else
            {
                /* Get parent from $FILE_NAME in this directory's MFT record */
                PFILE_RECORD_HEADER DirRec;
                PFILENAME_ATTRIBUTE DirFn;
                DotMft = NTFS_FILE_ROOT; /* fallback */
                DirRec = ExAllocateFromNPagedLookasideList(&DeviceExtension->FileRecLookasideList);
                if (DirRec && NT_SUCCESS(ReadFileRecord(DeviceExtension, Fcb->MFTIndex, DirRec)))
                {
                    DirFn = GetBestFileNameFromRecord(DeviceExtension, DirRec, NULL);
                    if (DirFn)
                        DotMft = DirFn->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
                }
                if (DirRec)
                    ExFreeToNPagedLookasideList(&DeviceExtension->FileRecLookasideList, DirRec);
            }
        }
        DotName[DotLen] = 0;
        RtlInitUnicodeString(&DotStr, DotName);

        /* Emit '.' and '..' for queries that list all files.
         * The I/O manager translates '*.*' into DOS wildcards (<.")
         * before we see it, and FsRtlIsNameInExpression with those
         * translated wildcards does not match '.' or '..'.
         * We detect "list all" patterns: single '*', or any pattern
         * that is purely DOS wildcards (<, >, ") and dots. */
        if ((Pattern.Length == sizeof(WCHAR) && Pattern.Buffer[0] == L'*') ||
            (Pattern.Length == 3 * sizeof(WCHAR) &&
             (Pattern.Buffer[0] == L'*' || Pattern.Buffer[0] == DOS_STAR) &&
             (Pattern.Buffer[1] == L'.' || Pattern.Buffer[1] == DOS_DOT) &&
             (Pattern.Buffer[2] == L'*' || Pattern.Buffer[2] == DOS_STAR)) ||
            FsRtlIsNameInExpression(&Pattern, &DotStr,
                                    !BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                    NULL))
        {
            /* Read the directory's own MFT record for timestamps/attributes */
            FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExtension->FileRecLookasideList);
            if (FileRecord)
            {
                Status = ReadFileRecord(DeviceExtension, DotMft, FileRecord);
                if (NT_SUCCESS(Status))
                {
                    PFILENAME_ATTRIBUTE OrigFn = GetBestFileNameFromRecord(DeviceExtension, FileRecord, NULL);
                    PSTANDARD_INFORMATION StdInfo = GetStandardInformationFromRecord(DeviceExtension, FileRecord);

                    /* Build a synthetic $FILE_NAME for '.' or '..' so we can
                     * populate any FILE_xxx_INFORMATION structure generically. */
                    struct {
                        FILENAME_ATTRIBUTE Fn;
                        WCHAR ExtraChar; /* space for ".." (2 chars, Fn.Name has 1) */
                    } SyntheticFn;
                    PFILENAME_ATTRIBUTE FnSrc = OrigFn;

                    if (FnSrc == NULL)
                    {
                        /* Directory has no $FILE_NAME (shouldn't happen, but be safe) */
                        RtlZeroMemory(&SyntheticFn, sizeof(SyntheticFn));
                        SyntheticFn.Fn.FileAttributes = NTFS_FILE_TYPE_DIRECTORY;
                        FnSrc = &SyntheticFn.Fn;
                    }
                    else
                    {
                        /* Copy the real attributes so we get correct timestamps */
                        RtlCopyMemory(&SyntheticFn.Fn, FnSrc,
                                      FIELD_OFFSET(FILENAME_ATTRIBUTE, Name));
                    }

                    /* Overwrite name with "." or ".." */
                    SyntheticFn.Fn.NameLength = (UCHAR)DotLen;
                    SyntheticFn.Fn.NameType = NTFS_FILE_NAME_WIN32;
                    SyntheticFn.Fn.Name[0] = L'.';
                    if (DotLen == 2)
                        *(&SyntheticFn.Fn.Name[0] + 1) = L'.';

                    if (StdInfo)
                        SyntheticFn.Fn.FileAttributes |= StdInfo->FileAttribute;

                    /* Build a minimal temporary file record containing only
                     * $STANDARD_INFORMATION and our synthetic $FILE_NAME.
                     * This is passed to the NtfsGetXxxInformation helpers
                     * which call GetBestFileNameFromRecord/GetStandardInformation
                     * on it. We reuse the original FileRecord since it already
                     * has $STANDARD_INFORMATION; we just need to make
                     * GetBestFileNameFromRecord return our synthetic name.
                     *
                     * Instead of building a fake record, directly fill the
                     * output structures ourselves - it's simpler and avoids
                     * touching shared state. */
                    {
                        ULONG DotNameBytes = DotLen * sizeof(WCHAR);
                        LARGE_INTEGER CTime = {.QuadPart = SyntheticFn.Fn.CreationTime};
                        LARGE_INTEGER ATime = {.QuadPart = SyntheticFn.Fn.LastAccessTime};
                        LARGE_INTEGER WTime = {.QuadPart = SyntheticFn.Fn.LastWriteTime};
                        LARGE_INTEGER MTime = {.QuadPart = SyntheticFn.Fn.ChangeTime};
                        ULONG Attrs = FILE_ATTRIBUTE_DIRECTORY;
                        if (StdInfo)
                            NtfsFileFlagsToAttributes(SyntheticFn.Fn.FileAttributes, &Attrs);

                        Status = STATUS_BUFFER_OVERFLOW;

                        switch (FileInformationClass)
                        {
                        case FileNamesInformation:
                        {
                            ULONG Needed = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) + DotNameBytes;
                            if (BufferLength >= (LONG)FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) + (LONG)DotNameBytes)
                            {
                                PFILE_NAMES_INFORMATION Info = (PFILE_NAMES_INFORMATION)Buffer;
                                Info->NextEntryOffset = ULONG_ROUND_UP(Needed);
                                Info->FileIndex = 0;
                                Info->FileNameLength = DotNameBytes;
                                RtlCopyMemory(Info->FileName, DotName, DotNameBytes);
                                Written = Needed;
                                Status = STATUS_SUCCESS;
                            }
                            break;
                        }
                        case FileDirectoryInformation:
                        {
                            ULONG Needed = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) + DotNameBytes;
                            if (BufferLength >= (LONG)Needed)
                            {
                                PFILE_DIRECTORY_INFORMATION Info = (PFILE_DIRECTORY_INFORMATION)Buffer;
                                RtlZeroMemory(Info, FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName));
                                Info->NextEntryOffset = ULONG_ROUND_UP(Needed);
                                Info->FileIndex = 0;
                                Info->CreationTime = CTime;
                                Info->LastAccessTime = ATime;
                                Info->LastWriteTime = WTime;
                                Info->ChangeTime = MTime;
                                Info->FileAttributes = Attrs;
                                Info->FileNameLength = DotNameBytes;
                                RtlCopyMemory(Info->FileName, DotName, DotNameBytes);
                                Written = Needed;
                                Status = STATUS_SUCCESS;
                            }
                            break;
                        }
                        case FileFullDirectoryInformation:
                        {
                            ULONG Needed = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) + DotNameBytes;
                            if (BufferLength >= (LONG)Needed)
                            {
                                PFILE_FULL_DIR_INFORMATION Info = (PFILE_FULL_DIR_INFORMATION)Buffer;
                                RtlZeroMemory(Info, FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName));
                                Info->NextEntryOffset = ULONG_ROUND_UP(Needed);
                                Info->FileIndex = 0;
                                Info->CreationTime = CTime;
                                Info->LastAccessTime = ATime;
                                Info->LastWriteTime = WTime;
                                Info->ChangeTime = MTime;
                                Info->FileAttributes = Attrs;
                                Info->FileNameLength = DotNameBytes;
                                RtlCopyMemory(Info->FileName, DotName, DotNameBytes);
                                Written = Needed;
                                Status = STATUS_SUCCESS;
                            }
                            break;
                        }
                        case FileBothDirectoryInformation:
                        {
                            ULONG Needed = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) + DotNameBytes;
                            if (BufferLength >= (LONG)Needed)
                            {
                                PFILE_BOTH_DIR_INFORMATION Info = (PFILE_BOTH_DIR_INFORMATION)Buffer;
                                RtlZeroMemory(Info, FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName));
                                Info->NextEntryOffset = ULONG_ROUND_UP(Needed);
                                Info->FileIndex = 0;
                                Info->CreationTime = CTime;
                                Info->LastAccessTime = ATime;
                                Info->LastWriteTime = WTime;
                                Info->ChangeTime = MTime;
                                Info->FileAttributes = Attrs;
                                Info->ShortNameLength = 0;
                                Info->FileNameLength = DotNameBytes;
                                RtlCopyMemory(Info->FileName, DotName, DotNameBytes);
                                Written = Needed;
                                Status = STATUS_SUCCESS;
                            }
                            break;
                        }
                        default:
                            Status = STATUS_INVALID_INFO_CLASS;
                            break;
                        }
                    }

                    if (NT_SUCCESS(Status))
                    {
                        Buffer0 = (PFILE_NAMES_INFORMATION)Buffer;
                        Buffer0->FileIndex = FileIndex++;
                        BufferLength -= Buffer0->NextEntryOffset;
                        Buffer += Buffer0->NextEntryOffset;
                    }
                }
                ExFreeToNPagedLookasideList(&DeviceExtension->FileRecLookasideList, FileRecord);
            }
        }

        Ccb->Entry++;

        if (Stack->Flags & SL_RETURN_SINGLE_ENTRY)
            break;
    }

    /* Buffer receiving a copy of the matched index entry's $FILE_NAME key.
     * The enumeration must report the entry's own name: for hard-linked
     * files the record's names differ from (and cannot identify) the index
     * entry that matched. */
    FoundName = ExAllocatePoolWithTag(NonPagedPool, NTFS_FOUND_NAME_SIZE, TAG_NTFS);
    if (FoundName == NULL)
    {
        ExReleaseResourceLite(&Fcb->MainResource);
        ExReleaseResourceLite(&DeviceExtension->DirResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Ccb->Entry now includes the 2 synthetic dot entries.
     * NtfsFindFileAt uses a 0-based index into the B-tree,
     * so subtract 2 to get the real B-tree offset. */
    {
        ULONG BTreeEntry = (Ccb->Entry >= 2) ? Ccb->Entry - 2 : 0;
    /* Only enter the B-tree loop if we haven't already satisfied
     * a single-entry request with a dot entry. */
    while (Status == STATUS_SUCCESS && BufferLength > 0
           && !(Buffer0 != NULL && (Stack->Flags & SL_RETURN_SINGLE_ENTRY)))
    {
        Status = NtfsFindFileAt(DeviceExtension,
                                &Pattern,
                                &BTreeEntry,
                                &FileRecord,
                                &MFTRecord,
                                Fcb->MFTIndex,
                                BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                FoundName);

        if (NT_SUCCESS(Status))
        {
            switch (FileInformationClass)
            {
                case FileNamesInformation:
                    Status = NtfsGetNamesInformation(DeviceExtension,
                                                     FileRecord,
                                                     MFTRecord,
                                                     FoundName,
                                                     (PFILE_NAMES_INFORMATION)Buffer,
                                                     BufferLength,
                                                     &Written,
                                                     Buffer0 == NULL);
                    break;

                case FileDirectoryInformation:
                    Status = NtfsGetDirectoryInformation(DeviceExtension,
                                                         FileRecord,
                                                         MFTRecord,
                                                         FoundName,
                                                         (PFILE_DIRECTORY_INFORMATION)Buffer,
                                                         BufferLength,
                                                         &Written,
                                                         Buffer0 == NULL);
                    break;

                case FileFullDirectoryInformation:
                    Status = NtfsGetFullDirectoryInformation(DeviceExtension,
                                                             FileRecord,
                                                             MFTRecord,
                                                             FoundName,
                                                             (PFILE_FULL_DIRECTORY_INFORMATION)Buffer,
                                                             BufferLength,
                                                             &Written,
                                                             Buffer0 == NULL);
                    break;

                case FileBothDirectoryInformation:
                    Status = NtfsGetBothDirectoryInformation(DeviceExtension,
                                                             FileRecord,
                                                             MFTRecord,
                                                             FoundName,
                                                             (PFILE_BOTH_DIR_INFORMATION)Buffer,
                                                             BufferLength,
                                                             &Written,
                                                             Buffer0 == NULL);
                    break;

                default:
                    Status = STATUS_INVALID_INFO_CLASS;
            }

            if (Status == STATUS_BUFFER_OVERFLOW || Status == STATUS_INVALID_INFO_CLASS)
            {
                break;
            }
        }
        else
        {
            BOOLEAN ReallyFirst = First && (Buffer0 == NULL);
            Status = (ReallyFirst ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES);
            break;
        }

        Buffer0 = (PFILE_NAMES_INFORMATION)Buffer;
        Buffer0->FileIndex = FileIndex++;
        BTreeEntry++;
        BufferLength -= Buffer0->NextEntryOffset;

        ExFreeToNPagedLookasideList(&DeviceExtension->FileRecLookasideList, FileRecord);

        if (Stack->Flags & SL_RETURN_SINGLE_ENTRY)
        {
            break;
        }

        Buffer += Buffer0->NextEntryOffset;
    }

    /* Sync Ccb->Entry back: 2 dots + however many B-tree entries we consumed */
    Ccb->Entry = BTreeEntry + 2;
    }

    ExFreePoolWithTag(FoundName, TAG_NTFS);

    if (Buffer0)
    {
        Buffer0->NextEntryOffset = 0;
        Status = STATUS_SUCCESS;
        IrpContext->Irp->IoStatus.Information = Stack->Parameters.QueryDirectory.Length - BufferLength;
    }
    else
    {
        ASSERT(Status != STATUS_SUCCESS || BufferLength == 0);
        ASSERT(Written <= Stack->Parameters.QueryDirectory.Length);
        IrpContext->Irp->IoStatus.Information = Written;
    }

    ExReleaseResourceLite(&Fcb->MainResource);
    ExReleaseResourceLite(&DeviceExtension->DirResource);

    return Status;
}


NTSTATUS
NtfsDirectoryControl(PNTFS_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    DPRINT("NtfsDirectoryControl() called\n");

    switch (IrpContext->MinorFunction)
    {
        case IRP_MN_QUERY_DIRECTORY:
            Status = NtfsQueryDirectory(IrpContext);
            break;

        case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        {
            PDEVICE_EXTENSION DeviceExt = IrpContext->DeviceObject->DeviceExtension;
            PIO_STACK_LOCATION Stack = IrpContext->Stack;
            PFILE_OBJECT FileObject = IrpContext->FileObject;

            DPRINT("IRP_MN_NOTIFY_CHANGE_DIRECTORY\n");

            /* Hand the request to the FsRtl directory-notify package, like
             * FastFat/CDFS.  The package holds the IRP pending until a change
             * is reported (NtfsReportChange) or the handle is cleaned up
             * (FsRtlNotifyCleanup in NtfsCleanupFile).  It synchronises on
             * DeviceExt->NotifySync internally, and only does byte-matching on
             * the name, so casting the UNICODE FileName to a STRING is fine.
             * Previously this returned STATUS_NOT_IMPLEMENTED, which made the
             * shell's overlapped ReadDirectoryChangesW fail synchronously
             * (error 1) instead of pending like on FAT/Windows. */
            FsRtlNotifyFullChangeDirectory(DeviceExt->NotifySync,
                                           &DeviceExt->NotifyList,
                                           FileObject->FsContext2,
                                           (PSTRING)&FileObject->FileName,
                                           BooleanFlagOn(Stack->Flags, SL_WATCH_TREE),
                                           FALSE,
                                           Stack->Parameters.NotifyDirectory.CompletionFilter,
                                           IrpContext->Irp,
                                           NULL,
                                           NULL);

            /* The notify package now owns the IRP; don't let NtfsDispatch
             * complete or re-queue it. */
            IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;
            Status = STATUS_PENDING;
            break;
        }

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
