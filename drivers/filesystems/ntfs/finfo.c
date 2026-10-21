/*
 *  ReactOS kernel
 *  Copyright (C) 2002 ReactOS Team
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
 *                   Hervé Poussineau (hpoussin@reactos.org)
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"
#include "security.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Retrieve the standard file information
 */
static
NTSTATUS
NtfsGetStandardInformation(PNTFS_FCB Fcb,
                           PDEVICE_OBJECT DeviceObject,
                           PFILE_STANDARD_INFORMATION StandardInfo,
                           PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    DPRINT("NtfsGetStandardInformation(%p, %p, %p, %p)\n", Fcb, DeviceObject, StandardInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_STANDARD_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    /* PRECONDITION */
    ASSERT(StandardInfo != NULL);
    ASSERT(Fcb != NULL);

    RtlZeroMemory(StandardInfo,
                  sizeof(FILE_STANDARD_INFORMATION));

    StandardInfo->AllocationSize = Fcb->RFCB.AllocationSize;
    StandardInfo->EndOfFile = Fcb->RFCB.FileSize;
    StandardInfo->NumberOfLinks = Fcb->LinkCount;
    StandardInfo->DeletePending = BooleanFlagOn(Fcb->Flags, FCB_DELETE_PENDING);
    StandardInfo->Directory = NtfsFCBIsDirectory(Fcb);

    *BufferLength -= sizeof(FILE_STANDARD_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetPositionInformation(PFILE_OBJECT FileObject,
                           PFILE_POSITION_INFORMATION PositionInfo,
                           PULONG BufferLength)
{
    DPRINT1("NtfsGetPositionInformation(%p, %p, %p)\n", FileObject, PositionInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_POSITION_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    PositionInfo->CurrentByteOffset.QuadPart = FileObject->CurrentByteOffset.QuadPart;

    DPRINT("Getting position %I64x\n",
           PositionInfo->CurrentByteOffset.QuadPart);

    *BufferLength -= sizeof(FILE_POSITION_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetBasicInformation(PFILE_OBJECT FileObject,
                        PNTFS_FCB Fcb,
                        PDEVICE_OBJECT DeviceObject,
                        PFILE_BASIC_INFORMATION BasicInfo,
                        PULONG BufferLength)
{
    PDEVICE_EXTENSION Vcb = Fcb->Vcb;
    PFILE_RECORD_HEADER Record;
    PSTANDARD_INFORMATION Info;
    NTSTATUS Status;

    if (*BufferLength < sizeof(FILE_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(BasicInfo, sizeof(*BasicInfo));
    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
    {
        BasicInfo->CreationTime.QuadPart = Fcb->Entry.CreationTime;
        BasicInfo->LastAccessTime.QuadPart = Fcb->Entry.LastAccessTime;
        BasicInfo->LastWriteTime.QuadPart = Fcb->Entry.LastWriteTime;
        BasicInfo->ChangeTime.QuadPart = Fcb->Entry.ChangeTime;
        NtfsFileFlagsToAttributes(Fcb->Entry.FileAttributes, &BasicInfo->FileAttributes);
    }
    else
    {
        /* Hard-link and stream FCBs share the stored basic information. */
        Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
        if (Record == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = ReadFileRecord(Vcb, Fcb->MFTIndex, Record);
        if (NT_SUCCESS(Status))
        {
            Info = GetStandardInformationFromRecord(Vcb, Record);
            ASSERT(Info != NULL);
            BasicInfo->CreationTime.QuadPart = Info->CreationTime;
            BasicInfo->LastAccessTime.QuadPart = Info->LastAccessTime;
            BasicInfo->LastWriteTime.QuadPart = Info->LastWriteTime;
            BasicInfo->ChangeTime.QuadPart = Info->ChangeTime;
            NtfsFileFlagsToAttributes(Info->FileAttribute |
                                      (NtfsFCBIsDirectory(Fcb) ? NTFS_FILE_TYPE_DIRECTORY : 0),
                                      &BasicInfo->FileAttributes);
        }
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    *BufferLength -= sizeof(FILE_BASIC_INFORMATION);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsGetAttributeTagInformation(PNTFS_FCB Fcb,
                               PFILE_ATTRIBUTE_TAG_INFORMATION AttributeTagInfo,
                               PULONG BufferLength)
{
    PFILENAME_ATTRIBUTE FileName = &Fcb->Entry;

    DPRINT("NtfsGetAttributeTagInformation(%p, %p, %p)\n", Fcb, AttributeTagInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(AttributeTagInfo, sizeof(FILE_ATTRIBUTE_TAG_INFORMATION));

    NtfsFileFlagsToAttributes(FileName->FileAttributes, &AttributeTagInfo->FileAttributes);

    /* The FCB keeps the tag of a reparse point with its attributes */
    AttributeTagInfo->ReparseTag = NtfsFCBIsReparsePoint(Fcb) ? FileName->Extended.ReparseTag : 0;

    *BufferLength -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);

    return STATUS_SUCCESS;
}


/**
* @name NtfsGetCompressionInformation
*
* FileCompressionInformation (MS-FSA 2.1.5.11.7): a compressed or sparse stream reports the
* clusters it really has, any other its size. No stream is compressed by this driver.
*/
static
NTSTATUS
NtfsGetCompressionInformation(PNTFS_FCB Fcb,
                              PFILE_COMPRESSION_INFORMATION Info,
                              PULONG BufferLength)
{
    PDEVICE_EXTENSION Vcb = Fcb->Vcb;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT Context;
    NTSTATUS Status;

    if (*BufferLength < sizeof(FILE_COMPRESSION_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    if (NtfsFCBIsDirectory(Fcb) || (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM)))
        return STATUS_INVALID_PARAMETER;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Info, sizeof(*Info));

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, FileRecord, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (NT_SUCCESS(Status))
    {
        if (Context->pRecord->IsNonResident &&
            (Context->pRecord->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE)))
        {
            Info->CompressedFileSize.QuadPart = Context->pRecord->NonResident.CompressedSize;
        }
        else
        {
            Info->CompressedFileSize.QuadPart = AttributeDataLength(Context->pRecord);
        }

        /* LZNT1 in 64 KiB units of 4 KiB chunks */
        if (NtfsAttributeIsCompressed(Context))
        {
            ULONG ClusterShift = 0;

            while ((1UL << ClusterShift) < Vcb->NtfsInfo.BytesPerCluster)
                ClusterShift++;

            Info->CompressionFormat = COMPRESSION_FORMAT_LZNT1;
            Info->CompressionUnitShift = (UCHAR)(ClusterShift + Context->pRecord->NonResident.CompressionUnit);
            Info->ChunkShift = 12;
            Info->ClusterShift = (UCHAR)ClusterShift;
        }
        ReleaseAttributeContext(Context);
    }

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    if (NT_SUCCESS(Status))
        *BufferLength -= sizeof(FILE_COMPRESSION_INFORMATION);

    return Status;
}

static
NTSTATUS
NtfsGetEaInformation(PNTFS_FCB Fcb,
                     PFILE_EA_INFORMATION EaInfo,
                     PULONG BufferLength)
{
    PFILE_RECORD_HEADER FileRecord;
    NTSTATUS Status;

    DPRINT("NtfsGetEaInformation(%p, %p, %p)\n", Fcb, EaInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_EA_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    /* A volume open has no EAs */
    EaInfo->EaSize = 0;
    if (!(Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM)))
    {
        FileRecord = ExAllocateFromNPagedLookasideList(&Fcb->Vcb->FileRecLookasideList);
        if (FileRecord == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = ReadFileRecord(Fcb->Vcb, Fcb->MFTIndex, FileRecord);
        if (NT_SUCCESS(Status))
            EaInfo->EaSize = NtfsGetEaSize(Fcb->Vcb, FileRecord);

        ExFreeToNPagedLookasideList(&Fcb->Vcb->FileRecLookasideList, FileRecord);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    *BufferLength -= sizeof(FILE_EA_INFORMATION);

    return STATUS_SUCCESS;
}


/*
 * FUNCTION: Retrieve the file name information
 */
static
NTSTATUS
NtfsGetNameInformation(PFILE_OBJECT FileObject,
                       PNTFS_FCB Fcb,
                       PDEVICE_OBJECT DeviceObject,
                       PFILE_NAME_INFORMATION NameInfo,
                       PULONG BufferLength)
{
    ULONG BytesToCopy;

    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DeviceObject);

    DPRINT("NtfsGetNameInformation(%p, %p, %p, %p, %p)\n", FileObject, Fcb, DeviceObject, NameInfo, BufferLength);

    ASSERT(NameInfo != NULL);
    ASSERT(Fcb != NULL);

    /* If buffer can't hold at least the file name length, bail out */
    if (*BufferLength < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
        return STATUS_BUFFER_TOO_SMALL;

    /* Save file name length, and as much file len, as buffer length allows */
    NameInfo->FileNameLength = wcslen(Fcb->PathName) * sizeof(WCHAR);

    /* Calculate amount of bytes to copy not to overflow the buffer */
    BytesToCopy = min(NameInfo->FileNameLength,
                      *BufferLength - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]));

    /* Fill in the bytes */
    RtlCopyMemory(NameInfo->FileName, Fcb->PathName, BytesToCopy);

    /* Check if we could write more but are not able to */
    if (*BufferLength < NameInfo->FileNameLength + (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
    {
        /* Return number of bytes written */
        *BufferLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + BytesToCopy;
        return STATUS_BUFFER_OVERFLOW;
    }

    /* We filled up as many bytes, as needed */
    *BufferLength -= (FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + NameInfo->FileNameLength);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetInternalInformation(PNTFS_FCB Fcb,
                           PFILE_INTERNAL_INFORMATION InternalInfo,
                           PULONG BufferLength)
{
    DPRINT("NtfsGetInternalInformation(%p, %p, %p)\n", Fcb, InternalInfo, BufferLength);

    ASSERT(InternalInfo);
    ASSERT(Fcb);

    if (*BufferLength < sizeof(FILE_INTERNAL_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    /* The file reference: MFT index and sequence number, which is what FileId means too */
    InternalInfo->IndexNumber.QuadPart = Fcb->MFTIndex | ((ULONGLONG)Fcb->SequenceNumber << 48);

    *BufferLength -= sizeof(FILE_INTERNAL_INFORMATION);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsGetNetworkOpenInformation(PNTFS_FCB Fcb,
                              PDEVICE_EXTENSION DeviceExt,
                              PFILE_NETWORK_OPEN_INFORMATION NetworkInfo,
                              PULONG BufferLength)
{
    FILE_BASIC_INFORMATION Basic;
    ULONG BasicLength = sizeof(Basic);
    NTSTATUS Status;

    if (*BufferLength < sizeof(FILE_NETWORK_OPEN_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;
    Status = NtfsGetBasicInformation(NULL, Fcb, NULL, &Basic, &BasicLength);
    if (!NT_SUCCESS(Status))
        return Status;
    NetworkInfo->CreationTime = Basic.CreationTime;
    NetworkInfo->LastAccessTime = Basic.LastAccessTime;
    NetworkInfo->LastWriteTime = Basic.LastWriteTime;
    NetworkInfo->ChangeTime = Basic.ChangeTime;
    NetworkInfo->FileAttributes = Basic.FileAttributes;
    NetworkInfo->EndOfFile = Fcb->RFCB.FileSize;
    NetworkInfo->AllocationSize = Fcb->RFCB.AllocationSize;

    *BufferLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);
    return STATUS_SUCCESS;
}

/**
* @name NtfsAppendStreamEntry
*
* Adds the FILE_STREAM_INFORMATION entry of one $DATA attribute at *Offset, if it fits.
*/
static
NTSTATUS
NtfsAppendStreamEntry(PFILE_STREAM_INFORMATION StreamInfo,
                      ULONG BufferLength,
                      PNTFS_ATTR_RECORD Attribute,
                      PFILE_STREAM_INFORMATION *Previous,
                      PULONG Offset,
                      PULONG Used)
{
    static const WCHAR DataSuffix[] = L":$DATA";
    PFILE_STREAM_INFORMATION CurrentInfo = (PFILE_STREAM_INFORMATION)((ULONG_PTR)StreamInfo + *Offset);
    ULONG NameBytes = Attribute->NameLength * sizeof(WCHAR);
    ULONG StreamNameLength = sizeof(WCHAR) + NameBytes + sizeof(DataSuffix) - sizeof(UNICODE_NULL);
    ULONG CurrentSize = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) + StreamNameLength;

    /* An entry that doesn't fit isn't returned at all */
    if (*Offset > BufferLength || CurrentSize > BufferLength - *Offset)
        return STATUS_BUFFER_OVERFLOW;

    CurrentInfo->NextEntryOffset = 0;
    CurrentInfo->StreamNameLength = StreamNameLength;
    CurrentInfo->StreamSize.QuadPart = AttributeDataLength(Attribute);
    CurrentInfo->StreamAllocationSize.QuadPart = AttributeAllocatedLength(Attribute);
    CurrentInfo->StreamName[0] = L':';
    RtlCopyMemory(&CurrentInfo->StreamName[1], (PWCHAR)((ULONG_PTR)Attribute + Attribute->NameOffset), NameBytes);
    RtlCopyMemory(&CurrentInfo->StreamName[1 + Attribute->NameLength], DataSuffix, sizeof(DataSuffix) - sizeof(UNICODE_NULL));

    /* Entries start on 8-byte boundaries */
    if (*Previous != NULL)
        (*Previous)->NextEntryOffset = (ULONG)((ULONG_PTR)CurrentInfo - (ULONG_PTR)*Previous);

    *Previous = CurrentInfo;
    *Used = *Offset + CurrentSize;
    *Offset += ALIGN_UP_BY(CurrentSize, sizeof(LONGLONG));

    return STATUS_SUCCESS;
}

/**
* @name NtfsGetStreamInformation
*
* FileStreamInformation: one entry per $DATA attribute, ":name:$DATA", the unnamed one
* "::$DATA". A file with an $ATTRIBUTE_LIST is enumerated in list order, which puts the
* unnamed stream first and takes in the streams in extension records.
*/
static
NTSTATUS
NtfsGetStreamInformation(PNTFS_FCB Fcb,
                         PDEVICE_EXTENSION DeviceExt,
                         PFILE_STREAM_INFORMATION StreamInfo,
                         PULONG BufferLength)
{
    PFILE_RECORD_HEADER FileRecord;
    PFILE_STREAM_INFORMATION Previous = NULL;
    PNTFS_ATTRIBUTE_LIST_ITEM List = NULL;
    ULONG ListLength = 0;
    ULONG Offset = 0;
    ULONG Used = 0;
    NTSTATUS Status;

    if (*BufferLength < sizeof(FILE_STREAM_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Not enough memory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status) && NtfsRecordHasAttributeList(FileRecord))
        Status = NtfsReadAttributeList(DeviceExt, FileRecord, &List, &ListLength);

    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    if (List != NULL)
    {
        ULONG ItemOffset;
        PNTFS_ATTRIBUTE_LIST_ITEM Item;

        for (ItemOffset = 0; NT_SUCCESS(Status) && ItemOffset < ListLength; ItemOffset += Item->Length)
        {
            PNTFS_ATTR_CONTEXT DataContext;

            Item = (PNTFS_ATTRIBUTE_LIST_ITEM)((ULONG_PTR)List + ItemOffset);
            if (Item->Length == 0)
            {
                Status = STATUS_FILE_CORRUPT_ERROR;
                break;
            }

            /* A stream split over records is listed once per extent; report it once */
            if (Item->Type != AttributeData || Item->StartingVCN != 0)
                continue;

            Status = FindAttribute(DeviceExt,
                                   FileRecord,
                                   AttributeData,
                                   (PCWSTR)((ULONG_PTR)Item + Item->NameOffset),
                                   Item->NameLength,
                                   &DataContext,
                                   NULL);
            if (NT_SUCCESS(Status))
            {
                Status = NtfsAppendStreamEntry(StreamInfo, *BufferLength, DataContext->pRecord, &Previous, &Offset, &Used);
                ReleaseAttributeContext(DataContext);
            }
        }

        ExFreePoolWithTag(List, TAG_NTFS);
    }
    else
    {
        PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

        while (NT_SUCCESS(Status) && Attribute->Type != AttributeEnd)
        {
            if (Attribute->Type == AttributeData)
                Status = NtfsAppendStreamEntry(StreamInfo, *BufferLength, Attribute, &Previous, &Offset, &Used);

            Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
        }
    }

    *BufferLength -= Used;

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

/**
* @name NtfsGetAlternateNameInformation
*
* FileAlternateNameInformation: the file's 8.3 name, which is its DOS $FILE_NAME or a
* name valid in both namespaces. A file with neither has no short name.
*/
static
NTSTATUS
NtfsGetAlternateNameInformation(PNTFS_FCB Fcb,
                                PDEVICE_EXTENSION DeviceExt,
                                PFILE_NAME_INFORMATION NameInfo,
                                PULONG BufferLength)
{
    ULONG Fixed = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
    PFILE_RECORD_HEADER FileRecord;
    PFILENAME_ATTRIBUTE ShortName;
    ULONG NameBytes, BytesToCopy;
    NTSTATUS Status;

    if (*BufferLength < Fixed)
        return STATUS_INFO_LENGTH_MISMATCH;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        ShortName = GetFileNameFromRecord(DeviceExt, FileRecord, NTFS_FILE_NAME_DOS);
        if (ShortName == NULL)
        {
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
        }
        else
        {
            NameBytes = ShortName->NameLength * sizeof(WCHAR);
            BytesToCopy = min(NameBytes, *BufferLength - Fixed);

            NameInfo->FileNameLength = NameBytes;
            RtlCopyMemory(NameInfo->FileName, ShortName->Name, BytesToCopy);

            *BufferLength -= Fixed + BytesToCopy;
            Status = (BytesToCopy < NameBytes) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
        }
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsGetAllInformation
*
* FileAllInformation: every fixed-size class the file system keeps, then the name. The
* fixed part is always filled; a name that doesn't fit is cut short with
* STATUS_BUFFER_OVERFLOW, as FileNameInformation does.
*/
static
NTSTATUS
NtfsGetAllInformation(PFILE_OBJECT FileObject,
                      PNTFS_FCB Fcb,
                      PDEVICE_OBJECT DeviceObject,
                      PFILE_ALL_INFORMATION AllInfo,
                      PULONG BufferLength)
{
    ULONG NameOffset = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation);
    ULONG Length;
    NTSTATUS Status;

    if (*BufferLength < NameOffset + FIELD_OFFSET(FILE_NAME_INFORMATION, FileName))
        return STATUS_INFO_LENGTH_MISMATCH;

    RtlZeroMemory(AllInfo, NameOffset);

    Length = sizeof(FILE_BASIC_INFORMATION);
    NtfsGetBasicInformation(FileObject, Fcb, DeviceObject, &AllInfo->BasicInformation, &Length);
    Length = sizeof(FILE_STANDARD_INFORMATION);
    NtfsGetStandardInformation(Fcb, DeviceObject, &AllInfo->StandardInformation, &Length);
    Length = sizeof(FILE_INTERNAL_INFORMATION);
    NtfsGetInternalInformation(Fcb, &AllInfo->InternalInformation, &Length);
    Length = sizeof(FILE_EA_INFORMATION);
    NtfsGetEaInformation(Fcb, &AllInfo->EaInformation, &Length);
    Length = sizeof(FILE_POSITION_INFORMATION);
    NtfsGetPositionInformation(FileObject, &AllInfo->PositionInformation, &Length);

    /* Access, mode and alignment belong to the handle and the device; the I/O
     * manager reports those */

    Length = *BufferLength - NameOffset;
    Status = NtfsGetNameInformation(FileObject, Fcb, DeviceObject, &AllInfo->NameInformation, &Length);

    *BufferLength = Length;

    return Status;
}

/**
* @name NtfsGetHardLinkInformation
* @implemented
*
* FileHardLinkInformation: one entry per link, its name and its directory. A DOS name is
* the short alias of the Win32 name beside it, not a link of its own, and isn't listed.
* Entries that don't fit aren't returned, but BytesNeeded counts them
* (STATUS_BUFFER_OVERFLOW).
*/
static
NTSTATUS
NtfsGetHardLinkInformation(PNTFS_FCB Fcb,
                           PDEVICE_EXTENSION DeviceExt,
                           PFILE_LINKS_INFORMATION LinksInfo,
                           PULONG BufferLength)
{
    ULONG Fixed = FIELD_OFFSET(FILE_LINKS_INFORMATION, Entry);
    PFILE_LINK_ENTRY_INFORMATION Previous = NULL;
    PFILE_RECORD_HEADER FileRecord;
    PFILENAME_ATTRIBUTE *Names;
    ULONG Count, i;
    ULONG Offset = Fixed, Used = Fixed;
    BOOLEAN Truncated = FALSE;
    NTSTATUS Status;

    if (*BufferLength < Fixed)
        return STATUS_INFO_LENGTH_MISMATCH;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsGetFileNames(DeviceExt, FileRecord, &Names, &Count);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (!NT_SUCCESS(Status))
        return Status;

    LinksInfo->BytesNeeded = Fixed;
    LinksInfo->EntriesReturned = 0;

    for (i = 0; i < Count; i++)
    {
        ULONG EntryLength = FIELD_OFFSET(FILE_LINK_ENTRY_INFORMATION, FileName) + Names[i]->NameLength * sizeof(WCHAR);
        PFILE_LINK_ENTRY_INFORMATION Entry;

        if (Names[i]->NameType == NTFS_FILE_NAME_DOS)
            continue;

        Offset = ALIGN_UP_BY(Offset, sizeof(LONGLONG));
        LinksInfo->BytesNeeded = Offset + EntryLength;

        if (Truncated || Offset + EntryLength > *BufferLength)
        {
            Truncated = TRUE;
            Offset += EntryLength;
            continue;
        }

        Entry = (PFILE_LINK_ENTRY_INFORMATION)((ULONG_PTR)LinksInfo + Offset);
        Entry->NextEntryOffset = 0;
        Entry->ParentFileId = (LONGLONG)Names[i]->DirectoryFileReferenceNumber;
        Entry->FileNameLength = Names[i]->NameLength;
        RtlCopyMemory(Entry->FileName, Names[i]->Name, Names[i]->NameLength * sizeof(WCHAR));

        if (Previous != NULL)
            Previous->NextEntryOffset = (ULONG)((ULONG_PTR)Entry - (ULONG_PTR)Previous);

        Previous = Entry;
        LinksInfo->EntriesReturned++;
        Used = Offset + EntryLength;
        Offset += EntryLength;
    }

    NtfsFreeFileNames(Names, Count);

    *BufferLength -= Used;

    return Truncated ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

// Convert enum value to friendly name
const PCSTR
GetInfoClassName(FILE_INFORMATION_CLASS infoClass)
{
    const PCSTR fileInfoClassNames[] = { "???????",
        "FileDirectoryInformation",
        "FileFullDirectoryInformation",
        "FileBothDirectoryInformation",
        "FileBasicInformation",
        "FileStandardInformation",
        "FileInternalInformation",
        "FileEaInformation",
        "FileAccessInformation",
        "FileNameInformation",
        "FileRenameInformation",
        "FileLinkInformation",
        "FileNamesInformation",
        "FileDispositionInformation",
        "FilePositionInformation",
        "FileFullEaInformation",
        "FileModeInformation",
        "FileAlignmentInformation",
        "FileAllInformation",
        "FileAllocationInformation",
        "FileEndOfFileInformation",
        "FileAlternateNameInformation",
        "FileStreamInformation",
        "FilePipeInformation",
        "FilePipeLocalInformation",
        "FilePipeRemoteInformation",
        "FileMailslotQueryInformation",
        "FileMailslotSetInformation",
        "FileCompressionInformation",
        "FileObjectIdInformation",
        "FileCompletionInformation",
        "FileMoveClusterInformation",
        "FileQuotaInformation",
        "FileReparsePointInformation",
        "FileNetworkOpenInformation",
        "FileAttributeTagInformation",
        "FileTrackingInformation",
        "FileIdBothDirectoryInformation",
        "FileIdFullDirectoryInformation",
        "FileValidDataLengthInformation",
        "FileShortNameInformation",
        "FileIoCompletionNotificationInformation",
        "FileIoStatusBlockRangeInformation",
        "FileIoPriorityHintInformation",
        "FileSfioReserveInformation",
        "FileSfioVolumeInformation",
        "FileHardLinkInformation",
        "FileProcessIdsUsingFileInformation",
        "FileNormalizedNameInformation",
        "FileNetworkPhysicalNameInformation",
        "FileIdGlobalTxDirectoryInformation",
        "FileIsRemoteDeviceInformation",
        "FileAttributeCacheInformation",
        "FileNumaNodeInformation",
        "FileStandardLinkInformation",
        "FileRemoteProtocolInformation",
        "FileReplaceCompletionInformation",
        "FileMaximumInformation",
        "FileDirectoryInformation",
        "FileFullDirectoryInformation",
        "FileBothDirectoryInformation",
        "FileBasicInformation",
        "FileStandardInformation",
        "FileInternalInformation",
        "FileEaInformation",
        "FileAccessInformation",
        "FileNameInformation",
        "FileRenameInformation",
        "FileLinkInformation",
        "FileNamesInformation",
        "FileDispositionInformation",
        "FilePositionInformation",
        "FileFullEaInformation",
        "FileModeInformation",
        "FileAlignmentInformation",
        "FileAllInformation",
        "FileAllocationInformation",
        "FileEndOfFileInformation",
        "FileAlternateNameInformation",
        "FileStreamInformation",
        "FilePipeInformation",
        "FilePipeLocalInformation",
        "FilePipeRemoteInformation",
        "FileMailslotQueryInformation",
        "FileMailslotSetInformation",
        "FileCompressionInformation",
        "FileObjectIdInformation",
        "FileCompletionInformation",
        "FileMoveClusterInformation",
        "FileQuotaInformation",
        "FileReparsePointInformation",
        "FileNetworkOpenInformation",
        "FileAttributeTagInformation",
        "FileTrackingInformation",
        "FileIdBothDirectoryInformation",
        "FileIdFullDirectoryInformation",
        "FileValidDataLengthInformation",
        "FileShortNameInformation",
        "FileIoCompletionNotificationInformation",
        "FileIoStatusBlockRangeInformation",
        "FileIoPriorityHintInformation",
        "FileSfioReserveInformation",
        "FileSfioVolumeInformation",
        "FileHardLinkInformation",
        "FileProcessIdsUsingFileInformation",
        "FileNormalizedNameInformation",
        "FileNetworkPhysicalNameInformation",
        "FileIdGlobalTxDirectoryInformation",
        "FileIsRemoteDeviceInformation",
        "FileAttributeCacheInformation",
        "FileNumaNodeInformation",
        "FileStandardLinkInformation",
        "FileRemoteProtocolInformation",
        "FileReplaceCompletionInformation",
        "FileMaximumInformation" };
    return fileInfoClassNames[infoClass];
}

/*
 * FUNCTION: Retrieve the specified file information
 */
NTSTATUS
NtfsQueryInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN NeedDirResource;

    DPRINT("NtfsQueryInformation(%p)\n", IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceObject = IrpContext->DeviceObject;
    FileInformationClass = Stack->Parameters.QueryFile.FileInformationClass;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = Stack->Parameters.QueryFile.Length;

    NeedDirResource = FileInformationClass == FileHardLinkInformation ||
                      FileInformationClass == FileNameInformation ||
                      FileInformationClass == FileAlternateNameInformation ||
                      FileInformationClass == FileAllInformation;
    if (NeedDirResource && !ExAcquireResourceSharedLite(&Fcb->Vcb->DirResource,
                                                       BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
        return NtfsMarkIrpContextForQueue(IrpContext);

    if (!ExAcquireResourceSharedLite(&Fcb->MainResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        if (NeedDirResource)
            ExReleaseResourceLite(&Fcb->Vcb->DirResource);
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    switch (FileInformationClass)
    {
        case FileStandardInformation:
            Status = NtfsGetStandardInformation(Fcb,
                                                DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileEaInformation:
            Status = NtfsGetEaInformation(Fcb,
                                          SystemBuffer,
                                          &BufferLength);
            break;

        case FilePositionInformation:
            Status = NtfsGetPositionInformation(FileObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileCompressionInformation:
            Status = NtfsGetCompressionInformation(Fcb,
                                                   SystemBuffer,
                                                   &BufferLength);
            break;

        case FileBasicInformation:
            Status = NtfsGetBasicInformation(FileObject,
                                             Fcb,
                                             DeviceObject,
                                             SystemBuffer,
                                             &BufferLength);
            break;

        case FileNameInformation:
            Status = NtfsGetNameInformation(FileObject,
                                            Fcb,
                                            DeviceObject,
                                            SystemBuffer,
                                            &BufferLength);
            break;

        case FileInternalInformation:
            Status = NtfsGetInternalInformation(Fcb,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileNetworkOpenInformation:
            Status = NtfsGetNetworkOpenInformation(Fcb,
                                                   DeviceObject->DeviceExtension,
                                                   SystemBuffer,
                                                   &BufferLength);
            break;

        case FileStreamInformation:
            Status = NtfsGetStreamInformation(Fcb,
                                              DeviceObject->DeviceExtension,
                                              SystemBuffer,
                                              &BufferLength);
            break;

        case FileAttributeTagInformation:
            Status = NtfsGetAttributeTagInformation(Fcb,
                                                    SystemBuffer,
                                                    &BufferLength);
            break;

        case FileAlternateNameInformation:
            Status = NtfsGetAlternateNameInformation(Fcb,
                                                     DeviceObject->DeviceExtension,
                                                     SystemBuffer,
                                                     &BufferLength);
            break;

        case FileHardLinkInformation:
            Status = NtfsGetHardLinkInformation(Fcb,
                                                DeviceObject->DeviceExtension,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileAllInformation:
            Status = NtfsGetAllInformation(FileObject,
                                           Fcb,
                                           DeviceObject,
                                           SystemBuffer,
                                           &BufferLength);
            break;

        default:
            DPRINT1("Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_INVALID_PARAMETER;
    }

    ExReleaseResourceLite(&Fcb->MainResource);
    if (NeedDirResource)
        ExReleaseResourceLite(&Fcb->Vcb->DirResource);

    /* An overflow still filled the fixed part and what fit of the rest */
    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
        Irp->IoStatus.Information =
            Stack->Parameters.QueryFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}

/**
* @name NtfsSetEndOfFile
* @implemented
*
* Sets the end of file (file size) for a given file.
*
* @param Fcb
* Pointer to an NTFS_FCB which describes the target file. Fcb->MainResource should have been
* acquired with ExAcquireResourceSharedLite().
*
* @param FileObject
* Pointer to a FILE_OBJECT describing the target file.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param IrpFlags
* ULONG describing the flags of the original IRP request (Irp->Flags).
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application opened the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param NewFileSize
* Pointer to a LARGE_INTEGER which indicates the new end of file (file size).
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_USER_MAPPED_FILE if trying to truncate a file but MmCanFileBeTruncated() returned false,
* STATUS_OBJECT_NAME_NOT_FOUND if there was no $DATA attribute associated with the target file,
* STATUS_INVALID_PARAMETER if there was no $FILENAME attribute associated with the target file,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_ACCESS_DENIED if target file is a volume or if paging is involved.
*
* @remarks As this function sets the size of a file at the file-level
* (and not at the attribute level) it's not recommended to use this
* function alongside functions that operate on the data attribute directly.
*
*/
NTSTATUS
NtfsSetEndOfFile(PNTFS_FCB Fcb,
                 PFILE_OBJECT FileObject,
                 PDEVICE_EXTENSION DeviceExt,
                 ULONG IrpFlags,
                 BOOLEAN CaseSensitive,
                 PLARGE_INTEGER NewFileSize)
{
    LARGE_INTEGER CurrentFileSize;
    PFILE_RECORD_HEADER FileRecord;
    PFILE_RECORD_HEADER Holder;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONG AttributeOffset;
    NTSTATUS Status = STATUS_SUCCESS;
    PFILENAME_ATTRIBUTE FileNameAttribute;


    /* Flush before reading the record: a resident paging write updates it. */
    if (NewFileSize->QuadPart < Fcb->RFCB.FileSize.QuadPart)
    {
        Status = NtfsFlushFileCache(FileObject, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    // Allocate non-paged memory for the file record
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Couldn't allocate memory for file record!");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // read the file record
    DPRINT("Reading file record...\n");
    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        // We couldn't get the file's record. Free the memory and return the error
        DPRINT1("Can't find record for %wS!\n", Fcb->ObjectName);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    DPRINT("Found record for %wS\n", Fcb->ObjectName);

    CurrentFileSize.QuadPart = NtfsGetFileSize(DeviceExt, FileRecord, L"", 0, NULL);

    // Are we trying to decrease the file size?
    if (NewFileSize->QuadPart < CurrentFileSize.QuadPart)
    {
        // Is the file mapped?
        if (!MmCanFileBeTruncated(FileObject->SectionObjectPointer,
                                  NewFileSize))
        {
            DPRINT1("Couldn't decrease file size!\n");
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_USER_MAPPED_FILE;
        }
    }

    // Find the attribute with the data stream for our file
    DPRINT("Finding Data Attribute...\n");
    Status = FindAttribute(DeviceExt,
                           FileRecord,
                           AttributeData,
                           Fcb->Stream,
                           wcslen(Fcb->Stream),
                           &DataContext,
                           &AttributeOffset);

    // Did we fail to find the attribute?
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No '%S' data stream associated with file!\n", Fcb->Stream);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // Get the size of the data attribute
    CurrentFileSize.QuadPart = AttributeDataLength(DataContext->pRecord);

    /* Through an $ATTRIBUTE_LIST the stream can live in another record than the base */
    Status = NtfsReadAttributeHolder(DeviceExt, DataContext, FileRecord, &Holder);
    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // Are we enlarging the attribute?
    if (NewFileSize->QuadPart > CurrentFileSize.QuadPart)
    {
        // is increasing the stream size not allowed?
        if ((Fcb->Flags & FCB_IS_VOLUME) ||
            (IrpFlags & IRP_PAGING_IO))
        {
            // TODO - just fail for now
            ReleaseAttributeContext(DataContext);
            NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_ACCESS_DENIED;
        }
    }

    // set the attribute data length
    Status = SetAttributeDataLength(FileObject, Fcb, DataContext, AttributeOffset, Holder, NewFileSize);

    /* The range the file grew by reads as zeros. Not for a paging file: Mm
     * writes every page of it before it reads one back, and zeroing the
     * gigabytes NtCreatePagingFile sizes it to would have to pass through
     * memory before there is any page file to relieve it. */
    if (NT_SUCCESS(Status) && NewFileSize->QuadPart > CurrentFileSize.QuadPart &&
        !(Fcb->Flags & FCB_IS_PAGING_FILE))
    {
        Status = NtfsZeroStreamRange(DeviceExt,
                                     DataContext,
                                     AttributeOffset,
                                     Holder,
                                     CurrentFileSize.QuadPart,
                                     NewFileSize->QuadPart);
    }

    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(DataContext);
        NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    /* Zeroing has completed; only now can Cc treat the new tail as valid. */
    Fcb->RFCB.ValidDataLength = Fcb->RFCB.FileSize;
    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);

    // now we need to update this file's size in every directory index entry that references it
    // TODO: expand to work with every filename / hardlink stored in the file record.
    FileNameAttribute = GetBestFileNameFromRecord(Fcb->Vcb, FileRecord);
    if (FileNameAttribute == NULL)
    {
        DPRINT1("Unable to find FileName attribute associated with file!\n");
        ReleaseAttributeContext(DataContext);
        NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_INVALID_PARAMETER;
    }

    Status = NtfsUpdateDuplicatedInformation(Fcb->Vcb,
                                             FileRecord,
                                             Fcb->MFTIndex,
                                             NTFS_FILENAME_UPDATE_SIZES,
                                             CaseSensitive);

    ReleaseAttributeContext(DataContext);
    NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsSetInformation
* @implemented
*
* Sets the specified file information.
*
* @param IrpContext
* Points to an NTFS_IRP_CONTEXT which describes the set operation
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_NOT_IMPLEMENTED if trying to set an unimplemented information class,
* STATUS_USER_MAPPED_FILE if trying to truncate a file but MmCanFileBeTruncated() returned false,
* STATUS_OBJECT_NAME_NOT_FOUND if there was no $DATA attribute associated with the target file,
* STATUS_INVALID_PARAMETER if there was no $FILENAME attribute associated with the target file,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_ACCESS_DENIED if target file is a volume or if paging is involved.
*
* @remarks Called by NtfsDispatch() in response to an IRP_MJ_SET_INFORMATION request.
* Only the FileEndOfFileInformation InformationClass is fully implemented. FileAllocationInformation
* is a hack and not a true implementation, but it's enough to make SetEndOfFile() work.
* All other information classes are TODO.
*
*/
/**
* @name NtfsSetAllocationInformation
* @implemented
*
* FileAllocationInformation: sets how much space a stream holds on disk. Growing it
* leaves the end of file alone; shrinking it below the end of file truncates the file
* (MS-FSA 2.1.5.15.1).
*/
static
NTSTATUS
NtfsSetAllocationInformation(PNTFS_FCB Fcb,
                             PFILE_OBJECT FileObject,
                             PDEVICE_EXTENSION DeviceExt,
                             ULONG IrpFlags,
                             BOOLEAN CaseSensitive,
                             PFILE_ALLOCATION_INFORMATION AllocationInfo)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONG AttributeOffset;
    NTSTATUS Status;

    if (AllocationInfo->AllocationSize.QuadPart < 0)
        return STATUS_INVALID_PARAMETER;

    if ((Fcb->Flags & FCB_IS_VOLUME) || (IrpFlags & IRP_PAGING_IO))
        return STATUS_ACCESS_DENIED;

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_ACCESS_DENIED;

    if (AllocationInfo->AllocationSize.QuadPart < Fcb->RFCB.FileSize.QuadPart)
    {
        Status = NtfsFlushFileCache(FileObject, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(DeviceExt, FileRecord, AttributeData, Fcb->Stream, wcslen(Fcb->Stream), &DataContext, &AttributeOffset);

    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    if ((ULONGLONG)AllocationInfo->AllocationSize.QuadPart < AttributeDataLength(DataContext->pRecord) &&
        !MmCanFileBeTruncated(FileObject->SectionObjectPointer, &AllocationInfo->AllocationSize))
    {
        Status = STATUS_USER_MAPPED_FILE;
    }
    else
    {
        PFILE_RECORD_HEADER Holder;

        Status = NtfsReadAttributeHolder(DeviceExt, DataContext, FileRecord, &Holder);
        if (NT_SUCCESS(Status))
        {
            Status = NtfsSetAllocationSize(FileObject,
                                           Fcb,
                                           DataContext,
                                           AttributeOffset,
                                           Holder,
                                           AllocationInfo->AllocationSize.QuadPart);
            NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
        }
    }

    if (NT_SUCCESS(Status))
    {
        Status = NtfsUpdateDuplicatedInformation(DeviceExt,
                                                 FileRecord,
                                                 Fcb->MFTIndex,
                                                 NTFS_FILENAME_UPDATE_SIZES,
                                                 CaseSensitive);
    }

    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsSetBasicInformation
* @implemented
*
* Applies a FILE_BASIC_INFORMATION to a file: its four timestamps and its
* attribute flags.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param Fcb
* File control block of the file being changed
*
* @param CaseSensitive
* Whether the parent directory's index should be searched case-sensitively
*
* @param BasicInfo
* The timestamps and attributes to apply
*
* @return
* STATUS_SUCCESS on success, STATUS_INSUFFICIENT_RESOURCES if an allocation
* failed, STATUS_INVALID_PARAMETER if the file record has no
* $STANDARD_INFORMATION, or whatever status reading or writing the file record
* returned.
*
* @remarks Zero leaves a timestamp unchanged. -1 suppresses automatic updates
* through this open, -2 resumes them, and an explicit positive value also pins
* the time until updates are resumed. FileAttributes of zero means no change.
*
* NTFS keeps a second copy of these in $FILE_NAME and a third in the parent
* directory's index entry; all three are updated here.
*
*/
static
NTSTATUS
NtfsSetBasicInformation(PDEVICE_EXTENSION DeviceExt,
                        PNTFS_FCB Fcb,
                        PFILE_OBJECT FileObject,
                        BOOLEAN CaseSensitive,
                        PFILE_BASIC_INFORMATION BasicInfo,
                        PULONG NotifyFilter)
{
    PFILE_RECORD_HEADER FileRecord;
    PSTANDARD_INFORMATION StdInfo;
    PNTFS_CCB Ccb = FileObject->FsContext2;
    ULONG OldAttributes, UserSetFlags = Ccb->Flags;
    ULONG ValidAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                            FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE |
                            FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    LARGE_INTEGER Now;
    NTSTATUS Status;

    if (BasicInfo->CreationTime.QuadPart < -2 || BasicInfo->LastAccessTime.QuadPart < -2 ||
        BasicInfo->LastWriteTime.QuadPart < -2 || BasicInfo->ChangeTime.QuadPart < -2 ||
        (!NtfsFCBIsDirectory(Fcb) && (BasicInfo->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ||
        (NtfsFCBIsDirectory(Fcb) && (BasicInfo->FileAttributes & FILE_ATTRIBUTE_TEMPORARY)))
        return STATUS_INVALID_PARAMETER;

    KeQuerySystemTime(&Now);
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Not enough memory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Can't find record for %wS!\n", Fcb->ObjectName);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    if (StdInfo == NULL)
    {
        DPRINT1("No $STANDARD_INFORMATION for %wS!\n", Fcb->ObjectName);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_INVALID_PARAMETER;
    }

    OldAttributes = StdInfo->FileAttribute;
    if (BasicInfo->FileAttributes != 0)
    {
        if (Fcb->MFTIndex == NTFS_FILE_ROOT)
            ValidAttributes &= ~(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        StdInfo->FileAttribute = (OldAttributes & ~ValidAttributes & ~FILE_ATTRIBUTE_NORMAL) |
                                (BasicInfo->FileAttributes & ValidAttributes);
        if (StdInfo->FileAttribute != OldAttributes &&
            !(UserSetFlags & CCB_USER_SET_CHANGE_TIME) && BasicInfo->ChangeTime.QuadPart != -1)
            StdInfo->ChangeTime = Now.QuadPart;
    }

    if (BasicInfo->ChangeTime.QuadPart == -2)
        UserSetFlags &= ~CCB_USER_SET_CHANGE_TIME;
    else if (BasicInfo->ChangeTime.QuadPart != 0)
        UserSetFlags |= CCB_USER_SET_CHANGE_TIME;
    if (BasicInfo->ChangeTime.QuadPart > 0)
        StdInfo->ChangeTime = BasicInfo->ChangeTime.QuadPart;

    if (BasicInfo->CreationTime.QuadPart > 0)
        StdInfo->CreationTime = BasicInfo->CreationTime.QuadPart;
    if (BasicInfo->LastAccessTime.QuadPart == -2)
        UserSetFlags &= ~CCB_USER_SET_ACCESS_TIME;
    else if (BasicInfo->LastAccessTime.QuadPart != 0)
        UserSetFlags |= CCB_USER_SET_ACCESS_TIME;
    if (BasicInfo->LastAccessTime.QuadPart > 0)
        StdInfo->LastAccessTime = BasicInfo->LastAccessTime.QuadPart;
    if (BasicInfo->LastWriteTime.QuadPart == -2)
        UserSetFlags &= ~CCB_USER_SET_WRITE_TIME;
    else if (BasicInfo->LastWriteTime.QuadPart != 0)
        UserSetFlags |= CCB_USER_SET_WRITE_TIME;
    if (BasicInfo->LastWriteTime.QuadPart > 0)
        StdInfo->LastWriteTime = BasicInfo->LastWriteTime.QuadPart;

    if ((BasicInfo->CreationTime.QuadPart > 0 || BasicInfo->LastAccessTime.QuadPart > 0 ||
         BasicInfo->LastWriteTime.QuadPart > 0) &&
        !(UserSetFlags & CCB_USER_SET_CHANGE_TIME) && BasicInfo->ChangeTime.QuadPart != -1)
        StdInfo->ChangeTime = Now.QuadPart;

    /* Everything else NTFS keeps a second and third copy of goes out through the one funnel,
     * which also writes the file record back. */
    Status = NtfsUpdateDuplicatedInformation(DeviceExt,
                                             FileRecord,
                                             Fcb->MFTIndex,
                                             NTFS_FILENAME_UPDATE_TIMES |
                                             NTFS_FILENAME_UPDATE_ATTRS,
                                             CaseSensitive);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to refresh the duplicated information for %wS (Status %lx)\n",
                Fcb->ObjectName, Status);
    }
    else
    {
        Ccb->Flags = UserSetFlags;
        Fcb->Entry.CreationTime = StdInfo->CreationTime;
        Fcb->Entry.LastAccessTime = StdInfo->LastAccessTime;
        Fcb->Entry.LastWriteTime = StdInfo->LastWriteTime;
        Fcb->Entry.ChangeTime = StdInfo->ChangeTime;
        Fcb->Entry.FileAttributes = StdInfo->FileAttribute |
                                   (NtfsFCBIsDirectory(Fcb) ? NTFS_FILE_TYPE_DIRECTORY : 0);
        if (BasicInfo->CreationTime.QuadPart > 0)
            *NotifyFilter |= FILE_NOTIFY_CHANGE_CREATION;
        if (BasicInfo->LastAccessTime.QuadPart > 0)
            *NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
        if (BasicInfo->LastWriteTime.QuadPart > 0)
            *NotifyFilter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
        if (OldAttributes != StdInfo->FileAttribute)
            *NotifyFilter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsSetPositionInformation
* @implemented
*
* Moves the file object's current byte offset (MS-FSA 2.1.5.14.9). The I/O manager
* does this itself for a synchronous file object; other handles reach the driver.
*
* @return
* STATUS_SUCCESS on success. STATUS_INFO_LENGTH_MISMATCH for a short buffer,
* STATUS_INVALID_PARAMETER if the file was opened without intermediate buffering and
* the offset isn't a multiple of the sector size.
*/
static
NTSTATUS
NtfsSetPositionInformation(PDEVICE_EXTENSION DeviceExt,
                           PFILE_OBJECT FileObject,
                           PFILE_POSITION_INFORMATION PositionInfo,
                           ULONG BufferLength)
{
    if (BufferLength < sizeof(FILE_POSITION_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    /* Unbuffered I/O goes straight to the disk, a sector at a time */
    if ((FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&
        (PositionInfo->CurrentByteOffset.QuadPart & (DeviceExt->NtfsInfo.BytesPerSector - 1)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    FileObject->CurrentByteOffset = PositionInfo->CurrentByteOffset;
    return STATUS_SUCCESS;
}

/**
* @name NtfsSetDispositionInformation
* @implemented
*
* Marks a file for deletion, or clears an existing mark.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param Fcb
* Pointer to the NTFS_FCB of the file. Fcb->MainResource should have been acquired.
*
* @param FileObject
* Pointer to the FILE_OBJECT the request arrived on.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param DispositionInfo
* Pointer to the FILE_DISPOSITION_INFORMATION supplied by the caller.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_CANNOT_DELETE if the file is read-only or is the volume root.
* STATUS_DIRECTORY_NOT_EMPTY if the file is a directory that still holds entries.
*
* @remarks
* Only records the intent; the file is deleted at cleanup, once its last handle is closed.
*/
static
NTSTATUS
NtfsSetDispositionInformation(PDEVICE_EXTENSION DeviceExt,
                              PNTFS_FCB Fcb,
                              PFILE_OBJECT FileObject,
                              BOOLEAN CaseSensitive,
                              PFILE_DISPOSITION_INFORMATION DispositionInfo)
{
    DPRINT("NtfsSetDispositionInformation(%p, %p, %p, %s, %p)\n",
           DeviceExt, Fcb, FileObject, CaseSensitive ? "TRUE" : "FALSE", DispositionInfo);

    if (!DispositionInfo->DeleteFile)
    {
        Fcb->Flags &= ~FCB_DELETE_PENDING;
        FileObject->DeletePending = FALSE;
        return STATUS_SUCCESS;
    }

    if (!NtfsGlobalData->EnableWriteSupport)
    {
        DPRINT1("NTFS write-support is EXPERIMENTAL and is disabled by default!\n");
        return STATUS_ACCESS_DENIED;
    }

    /* The volume root has no parent index to be removed from */
    if (NtfsFCBIsRoot(Fcb))
        return STATUS_CANNOT_DELETE;

    if (Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_READ_ONLY)
        return STATUS_CANNOT_DELETE;

    if (NtfsFCBIsDirectory(Fcb))
    {
        BOOLEAN Empty;
        NTSTATUS Status;

        /* A lookup treats an unreadable sub-node as a miss, which here would
         * delete a directory that still has children. This check fails instead. */
        KeEnterCriticalRegion();
        ExAcquireResourceSharedLite(&DeviceExt->IndexResource, TRUE);
        Status = NtfsIsDirectoryEmpty(DeviceExt, Fcb->MFTIndex, &Empty);
        ExReleaseResourceLite(&DeviceExt->IndexResource);
        KeLeaveCriticalRegion();

        if (!NT_SUCCESS(Status))
            return Status;

        if (!Empty)
            return STATUS_DIRECTORY_NOT_EMPTY;
    }

    /* Deleting a mapped file would free its record out from under the section */
    if (!MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForDelete))
        return STATUS_CANNOT_DELETE;

    Fcb->Flags |= FCB_DELETE_PENDING;
    FileObject->DeletePending = TRUE;

    return STATUS_SUCCESS;
}

/* A rename replaces one link. Its DOS alias, when present, belongs to the
 * same parent and is not a separate hard link. */
static BOOLEAN
NtfsRenameAttributeMatches(PNTFS_ATTR_RECORD Attribute,
                           USHORT Instance,
                           ULONGLONG Parent,
                           BOOLEAN RemoveDos)
{
    PFILENAME_ATTRIBUTE Name;

    if (Attribute->Type != AttributeFileName)
        return FALSE;
    if (Attribute->Instance == Instance)
        return TRUE;
    Name = (PFILENAME_ATTRIBUTE)((PUCHAR)Attribute + Attribute->Resident.ValueOffset);
    return RemoveDos && Name->NameType == NTFS_FILE_NAME_DOS &&
           (Name->DirectoryFileReferenceNumber & NTFS_MFT_MASK) == Parent;
}

/**
* @name NtfsSetFileNameAttribute
*
* Replaces the selected $FILE_NAME with Value, retaining all other hard links
* and the attribute instance referenced by an existing $ATTRIBUTE_LIST.
*
* @return
* STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES if the new name doesn't fit
* in the record.
*/
static
NTSTATUS
NtfsSetFileNameAttribute(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _Inout_ PFILE_RECORD_HEADER FileRecord,
    _In_ PFILENAME_ATTRIBUTE Value,
    _In_ ULONG ValueLength,
    _In_ USHORT Instance,
    _In_ ULONGLONG OldParent,
    _In_ BOOLEAN RemoveDos)
{
    ULONG RecordSize = DeviceExt->NtfsInfo.BytesPerFileRecord;
    ULONG HeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG NewLength = ALIGN_UP_BY(HeaderLength + ValueLength, ATTR_RECORD_ALIGNMENT);
    PFILE_RECORD_HEADER NewRecord;
    PNTFS_ATTR_RECORD Source;
    PNTFS_ATTR_RECORD Dest;
    ULONG SourceOffset = FileRecord->AttributeOffset;
    ULONG DestOffset = FileRecord->AttributeOffset;
    BOOLEAN Inserted = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    NewRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (NewRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(NewRecord, RecordSize);
    RtlCopyMemory(NewRecord, FileRecord, FileRecord->AttributeOffset);

    for (;;)
    {
        BOOLEAN AtEnd;

        Source = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + SourceOffset);
        AtEnd = (SourceOffset + sizeof(ULONG) > RecordSize || Source->Type == AttributeEnd);

        /* The new name goes ahead of the first attribute that sorts after it */
        if (!Inserted && (AtEnd || Source->Type > AttributeFileName))
        {
            if (DestOffset + NewLength + 2 * sizeof(ULONG) > RecordSize)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            Dest = (PNTFS_ATTR_RECORD)((ULONG_PTR)NewRecord + DestOffset);
            Dest->Type = AttributeFileName;
            Dest->Length = NewLength;
            Dest->Instance = Instance;
            Dest->Resident.ValueLength = ValueLength;
            Dest->Resident.ValueOffset = (USHORT)HeaderLength;
            Dest->Resident.Flags = RA_INDEXED;
            RtlCopyMemory((PUCHAR)Dest + HeaderLength, Value, ValueLength);

            DestOffset += NewLength;
            Inserted = TRUE;
        }

        if (AtEnd)
            break;

        if (Source->Length == 0 || SourceOffset + Source->Length > RecordSize)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            break;
        }

        if (!NtfsRenameAttributeMatches(Source, Instance, OldParent, RemoveDos))
        {
            if (DestOffset + Source->Length + 2 * sizeof(ULONG) > RecordSize)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            RtlCopyMemory((PUCHAR)NewRecord + DestOffset, Source, Source->Length);
            DestOffset += Source->Length;
        }

        SourceOffset += Source->Length;
    }

    if (NT_SUCCESS(Status))
    {
        SetFileRecordEnd(NewRecord, (PNTFS_ATTR_RECORD)((ULONG_PTR)NewRecord + DestOffset), FILE_RECORD_END);
        RtlCopyMemory(FileRecord, NewRecord, RecordSize);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, NewRecord);
    return Status;
}

/* A path an FCB takes once a rename is committed */
typedef struct _NTFS_RENAMED_FCB
{
    struct _NTFS_RENAMED_FCB *Next;
    PNTFS_FCB Fcb;
    PWCHAR NewPath;
} NTFS_RENAMED_FCB, *PNTFS_RENAMED_FCB;

static
VOID
NtfsFreeRenamedFcbs(
    _In_opt_ PNTFS_RENAMED_FCB List)
{
    while (List != NULL)
    {
        PNTFS_RENAMED_FCB Next = List->Next;

        if (List->NewPath != NULL)
            ExFreePoolWithTag(List->NewPath, TAG_NTFS);
        ExFreePoolWithTag(List, TAG_NTFS);
        List = Next;
    }
}

/**
* @name NtfsPrepareRenamedFcbs
*
* Builds the path every FCB moved by renaming Fcb to NewPath takes: the renamed FCB, its
* other streams, and for a directory every FCB cached beneath it. Done before the rename
* is committed, so running out of memory can still fail it.
*
* @remarks
* The caller holds DirResource exclusively, which keeps FCBs from entering or leaving
* the table.
*/
static
NTSTATUS
NtfsPrepareRenamedFcbs(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_FCB Fcb,
    _In_ PCWSTR NewPath,
    _Out_ PNTFS_RENAMED_FCB *List)
{
    SIZE_T OldLength = wcslen(Fcb->PathName);
    SIZE_T NewLength = wcslen(NewPath);
    PLIST_ENTRY Entry;

    *List = NULL;

    for (Entry = DeviceExt->FcbListHead.Flink; Entry != &DeviceExt->FcbListHead; Entry = Entry->Flink)
    {
        PNTFS_FCB Other = CONTAINING_RECORD(Entry, NTFS_FCB, FcbListEntry);
        PNTFS_RENAMED_FCB Renamed;
        SIZE_T TailLength;

        if (_wcsnicmp(Other->PathName, Fcb->PathName, OldLength) != 0 ||
            (Other->PathName[OldLength] != UNICODE_NULL && Other->PathName[OldLength] != L'\\' &&
             Other->PathName[OldLength] != L':'))
        {
            continue;
        }

        TailLength = wcslen(Other->PathName + OldLength);
        if (NewLength + TailLength >= NTFS_MAX_PATH_LENGTH)
        {
            NtfsFreeRenamedFcbs(*List);
            *List = NULL;
            return STATUS_NAME_TOO_LONG;
        }

        Renamed = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Renamed), TAG_NTFS);
        if (Renamed != NULL)
        {
            Renamed->Next = *List;
            Renamed->Fcb = Other;
            Renamed->NewPath = ExAllocatePoolWithTag(NonPagedPool, (NewLength + TailLength + 1) * sizeof(WCHAR), TAG_NTFS);
            *List = Renamed;
        }

        if (Renamed == NULL || Renamed->NewPath == NULL)
        {
            NtfsFreeRenamedFcbs(*List);
            *List = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(Renamed->NewPath, NewPath, NewLength * sizeof(WCHAR));
        RtlCopyMemory(Renamed->NewPath + NewLength, Other->PathName + OldLength, (TailLength + 1) * sizeof(WCHAR));
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsRenameFcbs
*
* Moves the FCBs NtfsPrepareRenamedFcbs() found to their new paths, and frees List.
*/
static
VOID
NtfsRenameFcbs(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_RENAMED_FCB List,
    _In_ ULONGLONG SourceMft,
    _In_ ULONGLONG TargetParent)
{
    PNTFS_RENAMED_FCB Renamed;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExt->FcbListLock, &OldIrql);

    for (Renamed = List; Renamed != NULL; Renamed = Renamed->Next)
    {
        if (Renamed->Fcb->MFTIndex == SourceMft)
            Renamed->Fcb->ParentMFTIndex = TargetParent;
        NtfsReplaceFCBPath(Renamed->Fcb, Renamed->NewPath);
        Renamed->NewPath = NULL;
    }

    KeReleaseSpinLock(&DeviceExt->FcbListLock, OldIrql);

    NtfsFreeRenamedFcbs(List);
}

/**
* @name NtfsRelinkFileNames
*
* Adds back the selected link's first Count index entries, undoing an update
* that failed after removing them.
*/
static
VOID
NtfsRelinkFileNames(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PFILE_RECORD_HEADER FileRecord,
    _In_ ULONGLONG FileReference,
    _In_ ULONG Count,
    _In_ BOOLEAN CaseSensitive,
    _In_ USHORT Instance,
    _In_ ULONGLONG OldParent,
    _In_ BOOLEAN RemoveDos)
{
    PNTFS_ATTR_RECORD Attribute;
    ULONG Offset = FileRecord->AttributeOffset;

    while (Count != 0 && Offset + sizeof(ULONG) <= DeviceExt->NtfsInfo.BytesPerFileRecord)
    {
        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + Offset);
        if (Attribute->Type == AttributeEnd || Attribute->Length == 0)
            break;

        if (NtfsRenameAttributeMatches(Attribute, Instance, OldParent, RemoveDos))
        {
            PFILENAME_ATTRIBUTE Name = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);

            if (!NT_SUCCESS(NtfsAddFilenameToDirectory(DeviceExt,
                                                       Name->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                                       FileReference,
                                                       Name,
                                                       CaseSensitive)))
            {
                DPRINT1("Couldn't restore '%.*S' in directory %I64u\n",
                        Name->NameLength, Name->Name, Name->DirectoryFileReferenceNumber & NTFS_MFT_MASK);
                NtfsMarkVolumeCorrupt(DeviceExt);
            }

            Count--;
        }

        Offset += Attribute->Length;
    }
}

/**
* @name NtfsBuildRenamePath
*
* Works out the new name and full path of a rename, and checks both.
*
* @param TargetDirMft
* Receives the MFT index of the target directory, or 0 for a rename within
* the file's current directory.
*/
static
NTSTATUS
NtfsBuildRenamePath(
    _In_ PNTFS_FCB Fcb,
    _In_opt_ PFILE_OBJECT TargetFileObject,
    _In_ PFILE_RENAME_INFORMATION RenameInfo,
    _In_ ULONG BufferLength,
    _Out_ PUNICODE_STRING NewName,
    _Outptr_ PWSTR *NewPath,
    _Out_ PULONGLONG TargetDirMft)
{
    PCWSTR TargetDirPath;
    SIZE_T TargetDirLength;
    SIZE_T NameLength;

    if (TargetFileObject != NULL)
    {
        PNTFS_FCB TargetFcb = TargetFileObject->FsContext;

        *NewName = TargetFileObject->FileName;
        *TargetDirMft = TargetFcb->MFTIndex;
        TargetDirPath = TargetFcb->PathName;
        TargetDirLength = NtfsFCBIsRoot(TargetFcb) ? 0 : wcslen(TargetFcb->PathName);
    }
    else
    {
        if (BufferLength < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) ||
            RenameInfo->FileNameLength > BufferLength - FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) ||
            RenameInfo->FileNameLength > MAXUSHORT)
        {
            return STATUS_INVALID_PARAMETER;
        }

        NewName->Buffer = RenameInfo->FileName;
        NewName->Length = (USHORT)RenameInfo->FileNameLength;
        NewName->MaximumLength = NewName->Length;

        *TargetDirMft = 0;
        TargetDirPath = Fcb->PathName;
        TargetDirLength = Fcb->ObjectName - Fcb->PathName;
    }

    if (!NtfsIsValidFileName(NewName))
        return STATUS_OBJECT_NAME_INVALID;

    NameLength = NewName->Length / sizeof(WCHAR);
    if (TargetDirLength + 1 + NameLength >= NTFS_MAX_PATH_LENGTH)
        return STATUS_NAME_TOO_LONG;

    /* A directory can't become its own descendant */
    if (NtfsFCBIsDirectory(Fcb))
    {
        SIZE_T OldLength = wcslen(Fcb->PathName);

        if (TargetDirLength >= OldLength &&
            _wcsnicmp(TargetDirPath, Fcb->PathName, OldLength) == 0 &&
            (TargetDirLength == OldLength || TargetDirPath[OldLength] == L'\\'))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    *NewPath = ExAllocatePoolWithTag(NonPagedPool, (TargetDirLength + 1 + NameLength + 1) * sizeof(WCHAR), TAG_NTFS);
    if (*NewPath == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(*NewPath, TargetDirPath, TargetDirLength * sizeof(WCHAR));
    (*NewPath)[TargetDirLength] = L'\\';
    RtlCopyMemory(*NewPath + TargetDirLength + 1, NewName->Buffer, NewName->Length);
    (*NewPath)[TargetDirLength + 1 + NameLength] = UNICODE_NULL;

    return STATUS_SUCCESS;
}

/**
* @name NtfsClearRenameTarget
*
* Makes sure NewName is free in the target directory, deleting the file
* already there when that's allowed.
*
* @param Scratch
* File record sized buffer the function may overwrite.
*/
static
NTSTATUS
NtfsClearRenameTarget(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PIRP Irp,
    _In_ PNTFS_FCB Fcb,
    _In_ ULONGLONG TargetDirMft,
    _In_ PUNICODE_STRING NewName,
    _In_z_ PCWSTR NewPath,
    _In_ BOOLEAN ReplaceIfExists,
    _In_ BOOLEAN CaseSensitive,
    _Out_opt_ PULONGLONG ReplacementMft,
    _Out_ PFILE_RECORD_HEADER Scratch)
{
    PNTFS_FCB ExistingFcb;
    PSTANDARD_INFORMATION Info;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    BOOLEAN Open = FALSE;
    ULONGLONG ExistingMft;
    ULONG FirstEntry = 0;
    NTSTATUS Status;

    if (ReplacementMft != NULL)
        *ReplacementMft = 0;

    Status = NtfsFindMftRecord(DeviceExt, TargetDirMft, NewName, &FirstEntry, FALSE, CaseSensitive, &ExistingMft, NULL);
    if (Status == STATUS_OBJECT_PATH_NOT_FOUND || Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_SUCCESS;

    if (!NT_SUCCESS(Status))
        return Status;

    /* A case-only rename finds the file itself */
    if (ExistingMft == Fcb->MFTIndex)
        return STATUS_SUCCESS;

    if (!ReplaceIfExists)
        return STATUS_OBJECT_NAME_COLLISION;

    Status = ReadFileRecord(DeviceExt, ExistingMft, Scratch);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Scratch->Flags & FRH_DIRECTORY)
        return STATUS_ACCESS_DENIED;

    Info = GetStandardInformationFromRecord(DeviceExt, Scratch);
    ASSERT(Info != NULL);
    if (Info->FileAttribute & FILE_ATTRIBUTE_READONLY)
        return STATUS_ACCESS_DENIED;

    Status = NtfsCheckNamespaceAccess(DeviceExt, ExistingMft, TargetDirMft, DELETE, Irp);
    if (!NT_SUCCESS(Status))
        return Status;

    /* An open through any hard link or named stream keeps the target open. */
    KeAcquireSpinLock(&DeviceExt->FcbListLock, &OldIrql);
    for (Entry = DeviceExt->FcbListHead.Flink; Entry != &DeviceExt->FcbListHead; Entry = Entry->Flink)
    {
        ExistingFcb = CONTAINING_RECORD(Entry, NTFS_FCB, FcbListEntry);
        if (ExistingFcb->MFTIndex == ExistingMft && ExistingFcb->OpenHandleCount != 0)
        {
            Open = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&DeviceExt->FcbListLock, OldIrql);
    if (Open)
        return STATUS_ACCESS_DENIED;

    /* A cached FCB means it's open or still mapped */
    ExistingFcb = NtfsGrabFCBFromTable(DeviceExt, NewPath);
    if (ExistingFcb != NULL)
    {
        NtfsReleaseFCB(DeviceExt, ExistingFcb);
        return STATUS_ACCESS_DENIED;
    }

    if (ReplacementMft != NULL)
    {
        *ReplacementMft = ExistingMft;
        return STATUS_SUCCESS;
    }

    /* Only that name goes; a file with other links keeps them */
    {
        USHORT LinksLeft;

        Status = NtfsDeleteUserFileLink(DeviceExt, ExistingMft, TargetDirMft, NewName, CaseSensitive, &LinksLeft);
        if (NT_SUCCESS(Status) && LinksLeft != 0)
            NtfsSetLinkCount(DeviceExt, ExistingMft, LinksLeft);
    }

    return Status;
}

/**
* @name NtfsBuildRenamedFileName
*
* Allocates the $FILE_NAME value for the new name. It keeps the duplicated
* information of OldName. Free it with ExFreePoolWithTag.
*
* @param Scratch
* File record sized buffer the function may overwrite.
*/
static
NTSTATUS
NtfsBuildRenamedFileName(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PFILENAME_ATTRIBUTE OldName,
    _In_ ULONGLONG TargetDirMft,
    _In_ PUNICODE_STRING NewName,
    _In_ BOOLEAN CaseSensitive,
    _Out_ PFILE_RECORD_HEADER Scratch,
    _Outptr_ PFILENAME_ATTRIBUTE *NewValue,
    _Out_ PULONG NewValueLength)
{
    PFILENAME_ATTRIBUTE Value;
    ULONG Length;
    NTSTATUS Status;

    *NewValue = NULL;
    *NewValueLength = 0;

    /* The parent reference needs the directory's sequence number */
    Status = ReadFileRecord(DeviceExt, TargetDirMft, Scratch);
    if (!NT_SUCCESS(Status))
        return Status;

    Length = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + NewName->Length;
    Value = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);
    if (Value == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Value, OldName, FIELD_OFFSET(FILENAME_ATTRIBUTE, Name));
    Value->DirectoryFileReferenceNumber = TargetDirMft | ((ULONGLONG)Scratch->SequenceNumber << 48);
    Value->NameLength = (UCHAR)(NewName->Length / sizeof(WCHAR));
    RtlCopyMemory(Value->Name, NewName->Buffer, NewName->Length);

    if (CaseSensitive)
        Value->NameType = NTFS_FILE_NAME_POSIX;
    else if (RtlIsNameLegalDOS8Dot3(NewName, NULL, NULL))
        Value->NameType = NTFS_FILE_NAME_WIN32_AND_DOS;
    else
        Value->NameType = NTFS_FILE_NAME_WIN32;

    *NewValue = Value;
    *NewValueLength = Length;
    return STATUS_SUCCESS;
}

/**
* @name NtfsUnlinkFileNames
*
* Removes the selected link and its DOS alias from the directory index.
* Removed receives how many entries were taken out, even on failure.
*/
static
NTSTATUS
NtfsUnlinkFileNames(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PFILE_RECORD_HEADER FileRecord,
    _In_ ULONGLONG ParentMft,
    _In_ BOOLEAN CaseSensitive,
    _Out_ PULONG Removed,
    _In_ USHORT Instance,
    _In_ BOOLEAN RemoveDos)
{
    PNTFS_ATTR_RECORD Attribute;
    ULONG Offset = FileRecord->AttributeOffset;
    NTSTATUS Status = STATUS_SUCCESS;

    *Removed = 0;

    while (Offset + sizeof(ULONG) <= DeviceExt->NtfsInfo.BytesPerFileRecord)
    {
        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + Offset);
        if (Attribute->Type == AttributeEnd || Attribute->Length == 0)
            break;

        if (NtfsRenameAttributeMatches(Attribute, Instance, ParentMft, RemoveDos))
        {
            Status = NtfsRemoveFilenameFromDirectory(DeviceExt,
                                                     ParentMft,
                                                     (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset),
                                                     CaseSensitive);
            if (!NT_SUCCESS(Status))
                break;

            (*Removed)++;
        }

        Offset += Attribute->Length;
    }

    return Status;
}

/* Restore names after a failed preparation without replaying an obsolete full
 * record: preparation may have moved data attributes to extension records. */
static NTSTATUS
NtfsRestoreExpandedRename(PDEVICE_EXTENSION Vcb,
                           PNTFS_FCB Fcb,
                           PFILENAME_ATTRIBUTE OldValue,
                           PFILENAME_ATTRIBUTE DosValue,
                           PFILENAME_ATTRIBUTE NewValue)
{
    PFILE_RECORD_HEADER Base, Holder = NULL;
    PNTFS_ATTR_RECORD Attribute, Copy = NULL;
    UNICODE_STRING OldName, NewName, DosName;
    ULONG Offset;
    NTSTATUS Status;

    Base = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Base == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    OldName.Buffer = OldValue->Name;
    OldName.Length = OldName.MaximumLength = OldValue->NameLength * sizeof(WCHAR);
    NewName.Buffer = NewValue->Name;
    NewName.Length = NewName.MaximumLength = NewValue->NameLength * sizeof(WCHAR);
    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, Base);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = NtfsFindFileNameRecord(Vcb, Base, OldValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                    &OldName, &Holder, &Offset);
    NtfsReleaseAttributeHolder(Vcb, Base, Holder);
    Holder = NULL;
    if (NT_SUCCESS(Status))
    {
        Status = NtfsFindFileNameRecord(Vcb, Base,
                                        NewValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                        &NewName, &Holder, &Offset);
        if (NT_SUCCESS(Status))
        {
            PFILENAME_ATTRIBUTE Value;
            BOOLEAN Exact;

            Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Holder + Offset);
            Value = (PFILENAME_ATTRIBUTE)((PUCHAR)Attribute + Attribute->Resident.ValueOffset);
            Exact = Value->NameLength == NewValue->NameLength &&
                    RtlCompareMemory(Value->Name, NewName.Buffer, NewName.Length) == NewName.Length;
            NtfsReleaseAttributeHolder(Vcb, Base, Holder);
            Holder = NULL;
            /* A failed case-only preparation may leave just the original
             * spelling. The folded lookup must not remove that old name. */
            if (Exact)
                Status = NtfsRemoveFileNameAttribute(Vcb, Base,
                                                     NewValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                                     &NewName);
        }
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            Status = STATUS_SUCCESS;
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        Status = NtfsFindFileNameRecord(Vcb, Base, NewValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                        &NewName, &Holder, &Offset);
        if (NT_SUCCESS(Status))
        {
            Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Holder + Offset);
            Status = NtfsSetFileNameAttribute(Vcb, Holder, OldValue,
                                              FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + OldName.Length,
                                              Attribute->Instance,
                                              NewValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK, FALSE);
            if (NT_SUCCESS(Status))
                Status = UpdateFileRecord(Vcb, Holder->MFTRecordNumber, Holder);
        }
        NtfsReleaseAttributeHolder(Vcb, Base, Holder);
        Holder = NULL;
        if (NT_SUCCESS(Status))
            Status = ReadFileRecord(Vcb, Fcb->MFTIndex, Base);
    }
    if (NT_SUCCESS(Status) && DosValue != NULL)
    {
        DosName.Buffer = DosValue->Name;
        DosName.Length = DosName.MaximumLength = DosValue->NameLength * sizeof(WCHAR);
        Status = NtfsFindFileNameRecord(Vcb, Base, DosValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                        &DosName, &Holder, &Offset);
        NtfsReleaseAttributeHolder(Vcb, Base, Holder);
        Holder = NULL;
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        {
            Copy = NtfsBuildFileNameAttribute(DosValue, FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + DosName.Length);
            Status = Copy != NULL ? NtfsAddAttributeToFile(Vcb, Base, Copy) : STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (NT_SUCCESS(Status))
        Status = NtfsKeepFileNameInBase(Vcb, Base);
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Base);
Cleanup:
    if (Copy != NULL)
        ExFreePoolWithTag(Copy, TAG_NTFS);
    NtfsReleaseAttributeHolder(Vcb, Base, Holder);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Base);
    return Status;
}

/* Build the final name metadata before touching either directory index. The
 * logical link count stays unchanged: these are two representations of the
 * same link while DirResource excludes namespace observers. */
static NTSTATUS
NtfsPrepareExpandedRename(PDEVICE_EXTENSION Vcb,
                           PNTFS_FCB Fcb,
                           PFILE_RECORD_HEADER Base,
                           PFILENAME_ATTRIBUTE OldValue,
                           PFILENAME_ATTRIBUTE DosValue,
                           PFILENAME_ATTRIBUTE NewValue,
                           ULONG NewLength)
{
    PNTFS_ATTR_RECORD Attribute;
    UNICODE_STRING Name;
    NTSTATUS Status;

    Attribute = NtfsBuildFileNameAttribute(NewValue, NewLength);
    if (Attribute == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = NtfsAddAttributeToFile(Vcb, Base, Attribute);
    ExFreePoolWithTag(Attribute, TAG_NTFS);
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Base);
    if (NT_SUCCESS(Status))
    {
        Name.Buffer = OldValue->Name;
        Name.Length = Name.MaximumLength = OldValue->NameLength * sizeof(WCHAR);
        Status = NtfsRemoveFileNameAttribute(Vcb, Base,
                                             OldValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK, &Name);
    }
    if (NT_SUCCESS(Status) && DosValue != NULL)
    {
        Name.Buffer = DosValue->Name;
        Name.Length = Name.MaximumLength = DosValue->NameLength * sizeof(WCHAR);
        Status = NtfsRemoveFileNameAttribute(Vcb, Base,
                                             DosValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK, &Name);
    }
    if (NT_SUCCESS(Status))
        Status = NtfsKeepFileNameInBase(Vcb, Base);
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Base);
    if (!NT_SUCCESS(Status))
    {
        NTSTATUS RestoreStatus = NtfsRestoreExpandedRename(Vcb, Fcb, OldValue, DosValue, NewValue);
        if (!NT_SUCCESS(RestoreStatus))
        {
            DPRINT1("Couldn't restore names after rename preparation: %lx\n", RestoreStatus);
            NtfsMarkVolumeCorrupt(Vcb);
        }
    }
    return Status;
}

static NTSTATUS
NtfsCommitExpandedRename(PDEVICE_EXTENSION Vcb,
                          PNTFS_FCB Fcb,
                          PFILENAME_ATTRIBUTE OldValue,
                          PFILENAME_ATTRIBUTE DosValue,
                          PFILENAME_ATTRIBUTE NewValue,
                          BOOLEAN CaseSensitive)
{
    ULONGLONG Reference = NtfsFileReference(Fcb);
    ULONG Removed = 0;
    NTSTATUS Status;

    Status = NtfsRemoveFilenameFromDirectory(Vcb,
                                             OldValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                             OldValue, CaseSensitive);
    if (NT_SUCCESS(Status))
        Removed++;
    if (NT_SUCCESS(Status) && DosValue != NULL)
    {
        Status = NtfsRemoveFilenameFromDirectory(Vcb,
                                                 DosValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                                 DosValue, CaseSensitive);
        if (NT_SUCCESS(Status))
            Removed++;
    }
    if (NT_SUCCESS(Status))
        Status = NtfsAddFilenameToDirectory(Vcb,
                                             NewValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                             Reference, NewValue, CaseSensitive);
    if (!NT_SUCCESS(Status))
    {
        NTSTATUS RestoreStatus = NtfsRestoreExpandedRename(Vcb, Fcb, OldValue, DosValue, NewValue);
        if (NT_SUCCESS(RestoreStatus) && Removed != 0)
            RestoreStatus = NtfsAddFilenameToDirectory(Vcb,
                                                       OldValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                                       Reference, OldValue, CaseSensitive);
        if (NT_SUCCESS(RestoreStatus) && Removed > 1)
            RestoreStatus = NtfsAddFilenameToDirectory(Vcb,
                                                       DosValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                                       Reference, DosValue, CaseSensitive);
        if (!NT_SUCCESS(RestoreStatus))
        {
            DPRINT1("Couldn't restore directory entries after rename: %lx\n", RestoreStatus);
            NtfsMarkVolumeCorrupt(Vcb);
        }
    }
    return Status;
}

/**
* @name NtfsCommitRename
*
* Swaps the file's names on disk for NewValue. On failure everything is put
* back as it was in Original.
*
* @param FileRecord
* The file's record, rewritten in place.
*
* @param Original
* Unchanged copy of FileRecord.
*/
static
NTSTATUS
NtfsCommitRename(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PNTFS_FCB Fcb,
    _Inout_ PFILE_RECORD_HEADER FileRecord,
    _In_ PFILE_RECORD_HEADER Original,
    _In_ ULONGLONG OldParentMft,
    _In_ ULONGLONG TargetDirMft,
    _In_ PFILENAME_ATTRIBUTE NewValue,
    _In_ BOOLEAN CaseSensitive,
    _In_ USHORT Instance,
    _In_ BOOLEAN RemoveDos)
{
    ULONGLONG FileReference = Fcb->MFTIndex | ((ULONGLONG)Fcb->SequenceNumber << 48);
    BOOLEAN RecordChanged = FALSE;
    ULONG Removed;
    NTSTATUS Status;

    /* Take the old names out of the index first, so a case-only rename in the
     * same directory doesn't collide with itself */
    Status = NtfsUnlinkFileNames(DeviceExt, Original, OldParentMft, CaseSensitive, &Removed, Instance, RemoveDos);

    if (NT_SUCCESS(Status))
    {
        RecordChanged = TRUE;
        Status = UpdateFileRecord(DeviceExt, FileRecord->MFTRecordNumber, FileRecord);
    }

    if (NT_SUCCESS(Status))
        Status = NtfsAddFilenameToDirectory(DeviceExt, TargetDirMft, FileReference, NewValue, CaseSensitive);

    if (!NT_SUCCESS(Status))
    {
        /* Put the old record back, then the names that were taken out */
        if (RecordChanged && !NT_SUCCESS(UpdateFileRecord(DeviceExt, Original->MFTRecordNumber, Original)))
            NtfsMarkVolumeCorrupt(DeviceExt);

        NtfsRelinkFileNames(DeviceExt, Original, FileReference, Removed, CaseSensitive, Instance, OldParentMft, RemoveDos);
    }

    return Status;
}

/* Replace an existing key in place: the destination directory needs no new
 * index entry or allocation, and the old target stays whole until commit. */
static NTSTATUS
NtfsReplaceIndexName(PDEVICE_EXTENSION Vcb,
                     ULONGLONG Parent,
                     PFILENAME_ATTRIBUTE Key,
                     ULONGLONG Reference,
                     PFILENAME_ATTRIBUTE Value,
                     BOOLEAN CaseSensitive)
{
    NTFS_FILENAME_UPDATE Update;
    UNICODE_STRING Name;

    RtlZeroMemory(&Update, sizeof(Update));
    Update.Flags = NTFS_FILENAME_UPDATE_REPLACE;
    Update.FileReference = Reference;
    Update.FileName = Value;
    Name.Buffer = Key->Name;
    Name.Length = Name.MaximumLength = Key->NameLength * sizeof(WCHAR);
    return UpdateFileNameRecord(Vcb, Parent, &Name, FALSE, &Update, CaseSensitive);
}

/* The directory entry already belongs to the source. Retire only the replaced
 * link's old metadata; failure must not remove the newly committed name. */
static NTSTATUS
NtfsRetireReplacedLink(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER Target,
                       PFILENAME_ATTRIBUTE OldValue,
                       PFILENAME_ATTRIBUTE DosValue,
                       BOOLEAN CaseSensitive)
{
    UNICODE_STRING Name;
    ULONGLONG Parent = OldValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
    NTSTATUS Status;

    if (DosValue != NULL)
    {
        Status = NtfsRemoveFilenameFromDirectory(Vcb, Parent, DosValue, CaseSensitive);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    Name.Buffer = OldValue->Name;
    Name.Length = Name.MaximumLength = OldValue->NameLength * sizeof(WCHAR);
    Status = NtfsRemoveFileNameAttribute(Vcb, Target, Parent, &Name);
    if (NT_SUCCESS(Status) && DosValue != NULL)
    {
        Name.Buffer = DosValue->Name;
        Name.Length = Name.MaximumLength = DosValue->NameLength * sizeof(WCHAR);
        Status = NtfsRemoveFileNameAttribute(Vcb, Target, Parent, &Name);
    }
    if (!NT_SUCCESS(Status))
        return Status;
    ASSERT(Target->LinkCount != 0);
    Target->LinkCount--;
    if (Target->LinkCount != 0)
        Status = NtfsKeepFileNameInBase(Vcb, Target);
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Target->MFTRecordNumber, Target);
    if (!NT_SUCCESS(Status))
        return Status;
    NtfsSetLinkCount(Vcb, Target->MFTRecordNumber, Target->LinkCount);
    if (Target->LinkCount == 0)
    {
        Status = NtfsDeleteFileRecord(Vcb, Target->MFTRecordNumber, CaseSensitive);
        if (NT_SUCCESS(Status))
            NtfsReleaseFileQuota(Vcb, Target);
    }
    return Status;
}

static NTSTATUS
NtfsCommitReplacingRename(PDEVICE_EXTENSION Vcb,
                           PNTFS_FCB Fcb,
                           PFILE_RECORD_HEADER NameRecord,
                           PFILE_RECORD_HEADER Original,
                           PFILE_RECORD_HEADER Target,
                           PFILENAME_ATTRIBUTE OldValue,
                           PFILENAME_ATTRIBUTE DosValue,
                           PFILENAME_ATTRIBUTE NewValue,
                           PUNICODE_STRING TargetName,
                           PCWSTR NewPath,
                           PWSTR *PreviousTargetPath,
                           PBOOLEAN ExactCaseMatch,
                           BOOLEAN Expanded,
                           BOOLEAN CaseSensitive)
{
    PFILENAME_ATTRIBUTE *Names = NULL, TargetValue = NULL, TargetDos = NULL;
    ULONG Count = 0, i, Removed = 0;
    ULONGLONG Parent = NewValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
    ULONGLONG OldParent = OldValue->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
    ULONGLONG Reference = NtfsFileReference(Fcb);
    BOOLEAN IndexChanged = FALSE;
    NTSTATUS Status, RestoreStatus = STATUS_SUCCESS;

    Status = NtfsGetFileNames(Vcb, Target, &Names, &Count);
    if (!NT_SUCCESS(Status))
        goto Restore;
    for (i = 0; i < Count; i++)
    {
        if ((Names[i]->DirectoryFileReferenceNumber & NTFS_MFT_MASK) != Parent)
            continue;
        if (Names[i]->NameLength * sizeof(WCHAR) == TargetName->Length &&
            NtfsAttributeNamesEqual(Vcb, Names[i]->Name, TargetName->Buffer, Names[i]->NameLength) &&
            (TargetValue == NULL || RtlCompareMemory(Names[i]->Name, TargetName->Buffer,
                                                      TargetName->Length) == TargetName->Length))
            TargetValue = Names[i];
        if (Names[i]->NameType == NTFS_FILE_NAME_DOS)
            TargetDos = Names[i];
    }
    ASSERT(TargetValue != NULL);
    if (TargetValue->NameType != NTFS_FILE_NAME_WIN32)
        TargetDos = NULL;
    *ExactCaseMatch = RtlCompareMemory(TargetValue->Name, TargetName->Buffer,
                                        TargetName->Length) == TargetName->Length;
    *PreviousTargetPath = ExAllocatePoolWithTag(NonPagedPool,
                                                (wcslen(NewPath) + 1) * sizeof(WCHAR), TAG_NTFS);
    if (*PreviousTargetPath == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Restore;
    }
    RtlCopyMemory(*PreviousTargetPath, NewPath, (wcslen(NewPath) + 1) * sizeof(WCHAR));
    RtlCopyMemory(*PreviousTargetPath + wcslen(NewPath) - TargetValue->NameLength,
                  TargetValue->Name, TargetName->Length);
    if (!Expanded)
    {
        Status = UpdateFileRecord(Vcb, NameRecord->MFTRecordNumber, NameRecord);
        if (!NT_SUCCESS(Status))
            goto Restore;
    }
    IndexChanged = TRUE;
    Status = NtfsReplaceIndexName(Vcb, Parent, TargetValue, Reference, NewValue, CaseSensitive);
    if (!NT_SUCCESS(Status))
        goto Restore;
    Status = NtfsRemoveFilenameFromDirectory(Vcb, OldParent, OldValue, CaseSensitive);
    if (NT_SUCCESS(Status))
        Removed++;
    if (NT_SUCCESS(Status) && DosValue != NULL)
    {
        Status = NtfsRemoveFilenameFromDirectory(Vcb, OldParent, DosValue, CaseSensitive);
        if (NT_SUCCESS(Status))
            Removed++;
    }
    if (!NT_SUCCESS(Status))
        goto Restore;

    /* The namespace change is committed. Reclaiming the unreachable target
     * is best effort, as in the source port's 90215937680. */
    Status = NtfsRetireReplacedLink(Vcb, Target, TargetValue, TargetDos, CaseSensitive);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Couldn't retire replaced link in MFT %lu: %lx\n", Target->MFTRecordNumber, Status);
        NtfsMarkVolumeCorrupt(Vcb);
    }
    Status = STATUS_SUCCESS;
    goto Cleanup;

Restore:
    if (IndexChanged)
        RestoreStatus = NtfsReplaceIndexName(Vcb, Parent, NewValue,
                                             Target->MFTRecordNumber | ((ULONGLONG)Target->SequenceNumber << 48),
                                             TargetValue, CaseSensitive);
    if (Expanded)
    {
        NTSTATUS RecordStatus = NtfsRestoreExpandedRename(Vcb, Fcb, OldValue, DosValue, NewValue);
        if (!NT_SUCCESS(RecordStatus))
            RestoreStatus = RecordStatus;
    }
    else
    {
        NTSTATUS RecordStatus = UpdateFileRecord(Vcb, Original->MFTRecordNumber, Original);
        if (!NT_SUCCESS(RecordStatus))
            RestoreStatus = RecordStatus;
    }
    if (Removed != 0)
    {
        NTSTATUS IndexStatus = NtfsAddFilenameToDirectory(Vcb, OldParent, Reference, OldValue, CaseSensitive);
        if (!NT_SUCCESS(IndexStatus))
            RestoreStatus = IndexStatus;
    }
    if (Removed > 1)
    {
        NTSTATUS IndexStatus = NtfsAddFilenameToDirectory(Vcb, OldParent, Reference, DosValue, CaseSensitive);
        if (!NT_SUCCESS(IndexStatus))
            RestoreStatus = IndexStatus;
    }
    if (!NT_SUCCESS(RestoreStatus))
        NtfsMarkVolumeCorrupt(Vcb);
Cleanup:
    if (Names != NULL)
        NtfsFreeFileNames(Names, Count);
    return Status;
}

/* NTFS defers the change timestamp until cleanup (MS-FSA Appendix A
 * <187>). Archive is visible as soon as the namespace operation completes. */
static VOID
NtfsCompleteRename(PDEVICE_EXTENSION Vcb, PNTFS_FCB Fcb, PNTFS_CCB Ccb)
{
    PFILE_RECORD_HEADER Record;
    PSTANDARD_INFORMATION Info;
    NTSTATUS Status;

    Ccb->Flags |= CCB_UPDATE_CHANGE_TIME;
    if (NtfsFCBIsDirectory(Fcb))
        return;
    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL)
        return; /* Cleanup will persist the pending attributes and timestamp. */
    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, Record);
    if (NT_SUCCESS(Status))
    {
        Info = GetStandardInformationFromRecord(Vcb, Record);
        ASSERT(Info != NULL);
        Info->FileAttribute |= FILE_ATTRIBUTE_ARCHIVE;
        Status = NtfsUpdateDuplicatedInformation(Vcb, Record, Fcb->MFTIndex,
                                                 NTFS_FILENAME_UPDATE_ATTRS, FALSE);
        Fcb->Entry.FileAttributes |= FILE_ATTRIBUTE_ARCHIVE;
    }
    if (!NT_SUCCESS(Status))
        NtfsMarkVolumeCorrupt(Vcb);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
}

/**
* @name NtfsSetRenameInformation
* @implemented
*
* Renames or moves a file or directory within the volume.
*
* @param DeviceExt
* Volume holding the file.
*
* @param Fcb
* FCB of the file being renamed. The caller holds DirResource and the FCB's
* MainResource exclusively.
*
* @param TargetFileObject
* Directory the I/O manager opened with SL_OPEN_TARGET_DIRECTORY, whose
* FileName holds the new name. NULL for a rename within the same directory,
* in which case the name comes from RenameInfo.
*
* @param ReplaceIfExists
* Whether an existing file with the new name may be deleted.
*
* @param CaseSensitive
* TRUE for POSIX semantics.
*
* @param RenameInfo
* The caller's FILE_RENAME_INFORMATION.
*
* @param BufferLength
* Size of RenameInfo in bytes.
*
* @return
* STATUS_SUCCESS on success. STATUS_OBJECT_NAME_COLLISION if the name is taken
* and may not be replaced, STATUS_ACCESS_DENIED if the existing file can't be
* replaced, STATUS_OBJECT_NAME_INVALID for a bad name, or the error that
* stopped the update.
*/
static
NTSTATUS
NtfsSetRenameInformation(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PIRP Irp,
    _In_ PNTFS_FCB Fcb,
    _In_ PNTFS_CCB Ccb,
    _In_opt_ PFILE_OBJECT TargetFileObject,
    _In_ BOOLEAN ReplaceIfExists,
    _In_ BOOLEAN CaseSensitive,
    _In_ PFILE_RENAME_INFORMATION RenameInfo,
    _In_ ULONG BufferLength)
{
    UNICODE_STRING NewName;
    PWSTR NewPath = NULL, PreviousTargetPath = NULL;
    PNTFS_RENAMED_FCB RenamedFcbs = NULL;
    ULONGLONG TargetDirMft;
    ULONGLONG OldParentMft;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PFILE_RECORD_HEADER Original = NULL;
    PFILE_RECORD_HEADER NameRecord = NULL, Scratch = NULL;
    PNTFS_ATTR_RECORD NameAttribute;
    UNICODE_STRING OldLinkName;
    ULONG NameOffset, FirstEntry = 0;
    ULONGLONG ExistingMft, ReplacementMft = 0;
    USHORT Instance;
    BOOLEAN RemoveDos, Expanded = FALSE, ExactCaseMatch = FALSE;
    PFILENAME_ATTRIBUTE *Names = NULL, OldValue = NULL, DosValue = NULL;
    ULONG NameCount = 0, i;
    PFILENAME_ATTRIBUTE OldName;
    PFILENAME_ATTRIBUTE NewValue = NULL;
    ULONG NewValueLength, NewAttributeLength;
    NTSTATUS Status;

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_ACCESS_DENIED;

    if ((Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM)) || NtfsFCBIsRoot(Fcb))
        return STATUS_INVALID_PARAMETER;

    /* Renaming an alternate stream is a different operation */
    if (Fcb->Stream[0] != UNICODE_NULL)
        return STATUS_NOT_IMPLEMENTED;

    Status = NtfsBuildRenamePath(Fcb, TargetFileObject, RenameInfo, BufferLength, &NewName, &NewPath, &TargetDirMft);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsPrepareRenamedFcbs(DeviceExt, Fcb, NewPath, &RenamedFcbs);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(NewPath, TAG_NTFS);
        return Status;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    Original = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    Scratch = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL || Original == NULL || Scratch == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Quit;
    }

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Quit;

    OldParentMft = Fcb->ParentMFTIndex;
    RtlInitUnicodeString(&OldLinkName, Fcb->ObjectName + 1);
    Status = NtfsFindFileNameRecord(DeviceExt, FileRecord, OldParentMft,
                                    &OldLinkName, &NameRecord, &NameOffset);
    if (!NT_SUCCESS(Status))
        goto Quit;
    NameAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)NameRecord + NameOffset);
    OldName = (PFILENAME_ATTRIBUTE)((PUCHAR)NameAttribute + NameAttribute->Resident.ValueOffset);
    Instance = NameAttribute->Instance;
    RemoveDos = OldName->NameType == NTFS_FILE_NAME_WIN32;
    if (TargetFileObject == NULL)
        TargetDirMft = OldParentMft;
    if (TargetDirMft == OldParentMft && NewName.Length == OldName->NameLength * sizeof(WCHAR) &&
        RtlEqualMemory(NewName.Buffer, OldName->Name, NewName.Length))
    {
        Status = STATUS_SUCCESS;
        goto Quit;
    }

    Status = NtfsFindMftRecord(DeviceExt, TargetDirMft, &NewName, &FirstEntry,
                               FALSE, CaseSensitive, &ExistingMft, NULL);
    if (NT_SUCCESS(Status) && ExistingMft == Fcb->MFTIndex &&
        !(TargetDirMft == OldParentMft && NewName.Length == OldName->NameLength * sizeof(WCHAR) &&
          NtfsAttributeNamesEqual(DeviceExt, OldName->Name, NewName.Buffer, OldName->NameLength)))
    {
        /* Renaming onto another existing link to this file removes the source
         * link, even without ReplaceIfExists (MS-FSA 2.1.5.15.12). */
        OldLinkName.Buffer = OldName->Name;
        OldLinkName.Length = OldLinkName.MaximumLength = OldName->NameLength * sizeof(WCHAR);
        NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_RENAME_OLD_NAME);
        Status = NtfsUnlinkFileName(DeviceExt, Fcb->MFTIndex, OldParentMft, &OldLinkName, CaseSensitive);
        if (NT_SUCCESS(Status))
        {
            NtfsSetLinkCount(DeviceExt, Fcb->MFTIndex, FileRecord->LinkCount - 1);
            NtfsReportPathChange(DeviceExt, Fcb->PathName, FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_REMOVED);
            NtfsRenameFcbs(DeviceExt, RenamedFcbs, Fcb->MFTIndex, TargetDirMft);
            RenamedFcbs = NULL;
            NtfsCompleteRename(DeviceExt, Fcb, Ccb);
            NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_RENAME_NEW_NAME);
        }
        goto Quit;
    }
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND &&
        Status != STATUS_OBJECT_PATH_NOT_FOUND)
        goto Quit;

    /* Absolute targets were opened with the add right by the I/O manager.
     * A relative rename must authorize adding the new link here. Exact
     * no-ops and removal of the source in a same-file rename returned above. */
    if (TargetFileObject == NULL)
    {
        Status = NtfsCheckNamespaceAccess(DeviceExt, TargetDirMft, 0,
                                           NtfsFCBIsDirectory(Fcb) ? FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE, Irp);
        if (!NT_SUCCESS(Status))
            goto Quit;
    }

    Status = NtfsBuildRenamedFileName(DeviceExt, OldName, TargetDirMft, &NewName,
                                     CaseSensitive, Scratch, &NewValue, &NewValueLength);
    if (!NT_SUCCESS(Status))
        goto Quit;
    if (OldName->NameType == NTFS_FILE_NAME_POSIX)
        NewValue->NameType = NTFS_FILE_NAME_POSIX;
    if (!NtfsFCBIsDirectory(Fcb))
        NewValue->FileAttributes |= FILE_ATTRIBUTE_ARCHIVE;
    Status = NtfsGetFileNames(DeviceExt, FileRecord, &Names, &NameCount);
    if (!NT_SUCCESS(Status))
        goto Quit;
    for (i = 0; i < NameCount; i++)
    {
        if ((Names[i]->DirectoryFileReferenceNumber & NTFS_MFT_MASK) != OldParentMft)
            continue;
        if (Names[i]->NameLength == OldName->NameLength &&
            RtlEqualMemory(Names[i]->Name, OldName->Name, OldName->NameLength * sizeof(WCHAR)))
            OldValue = Names[i];
        if (RemoveDos && Names[i]->NameType == NTFS_FILE_NAME_DOS)
            DosValue = Names[i];
    }
    ASSERT(OldValue != NULL);
    NewAttributeLength = ALIGN_UP_BY(FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR) +
                                     NewValueLength, ATTR_RECORD_ALIGNMENT);
    if (NewAttributeLength > NameAttribute->Length &&
        NewAttributeLength - NameAttribute->Length > DeviceExt->NtfsInfo.BytesPerFileRecord - NameRecord->BytesInUse)
    {
        Status = NtfsMakeRoomInFileRecord(DeviceExt, NameRecord,
                                          NewAttributeLength - NameAttribute->Length, Instance);
        if (Status == STATUS_DISK_FULL)
        {
            Status = NtfsPrepareExpandedRename(DeviceExt, Fcb, FileRecord, OldValue,
                                                DosValue, NewValue, NewValueLength);
            Expanded = NT_SUCCESS(Status);
        }
        if (!NT_SUCCESS(Status))
            goto Quit;
    }
    if (DosValue != NULL && !Expanded)
    {
        Status = NtfsPrepareExpandedRename(DeviceExt, Fcb, FileRecord, OldValue,
                                            DosValue, NewValue, NewValueLength);
        Expanded = NT_SUCCESS(Status);
        if (!NT_SUCCESS(Status))
            goto Quit;
    }
    RtlCopyMemory(Original, NameRecord, DeviceExt->NtfsInfo.BytesPerFileRecord);
    if (!Expanded)
    {
        Status = NtfsSetFileNameAttribute(DeviceExt, NameRecord, NewValue, NewValueLength,
                                         Instance, OldParentMft, RemoveDos);
        if (!NT_SUCCESS(Status))
            goto Quit;
    }

    /* Allocate the new record image before removing a replacement target. */
    Status = NtfsClearRenameTarget(DeviceExt, Irp, Fcb, TargetDirMft, &NewName, NewPath,
                                   ReplaceIfExists, CaseSensitive, &ReplacementMft, Scratch);
    if (!NT_SUCCESS(Status))
    {
        if (Expanded && !NT_SUCCESS(NtfsRestoreExpandedRename(DeviceExt, Fcb, OldValue, DosValue, NewValue)))
            NtfsMarkVolumeCorrupt(DeviceExt);
        goto Quit;
    }
    NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_RENAME_OLD_NAME);

    if (ReplacementMft != 0)
        Status = NtfsCommitReplacingRename(DeviceExt, Fcb, NameRecord, Original, Scratch,
                                            OldValue, DosValue, NewValue, &NewName, NewPath,
                                            &PreviousTargetPath, &ExactCaseMatch, Expanded, CaseSensitive);
    else if (Expanded)
        Status = NtfsCommitExpandedRename(DeviceExt, Fcb, OldValue, DosValue, NewValue, CaseSensitive);
    else
        Status = NtfsCommitRename(DeviceExt,
                                  Fcb,
                                  NameRecord,
                                  Original,
                                  OldParentMft,
                                  TargetDirMft,
                                  NewValue,
                                  CaseSensitive,
                                  Instance,
                                  RemoveDos);
    if (!NT_SUCCESS(Status))
        goto Quit;

    if (ReplacementMft != 0 && !ExactCaseMatch)
        NtfsReportPathChange(DeviceExt, PreviousTargetPath, FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_REMOVED);
    NtfsReportPathChange(DeviceExt, Fcb->PathName,
                          NtfsFCBIsDirectory(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                          OldParentMft != TargetDirMft || (ReplacementMft != 0 && ExactCaseMatch) ?
                          FILE_ACTION_REMOVED : FILE_ACTION_RENAMED_OLD_NAME);
    NtfsRenameFcbs(DeviceExt, RenamedFcbs, Fcb->MFTIndex, TargetDirMft);
    RenamedFcbs = NULL;
    Fcb->ParentMFTIndex = TargetDirMft;

    RtlCopyMemory(&Fcb->Entry, NewValue, FIELD_OFFSET(FILENAME_ATTRIBUTE, NameLength));
    Fcb->Entry.NameType = NewValue->NameType;

    NtfsCompleteRename(DeviceExt, Fcb, Ccb);
    NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_RENAME_NEW_NAME);
    if (ReplacementMft != 0 && ExactCaseMatch)
        NtfsReportPathChange(DeviceExt, Fcb->PathName,
                              FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                              FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_LAST_ACCESS |
                              FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY | FILE_NOTIFY_CHANGE_EA,
                              FILE_ACTION_MODIFIED);
    else
        NtfsReportPathChange(DeviceExt, Fcb->PathName,
                              NtfsFCBIsDirectory(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                              OldParentMft == TargetDirMft ? FILE_ACTION_RENAMED_NEW_NAME : FILE_ACTION_ADDED);

Quit:
    if (PreviousTargetPath != NULL)
        ExFreePoolWithTag(PreviousTargetPath, TAG_NTFS);
    if (Names != NULL)
        NtfsFreeFileNames(Names, NameCount);
    NtfsFreeRenamedFcbs(RenamedFcbs);
    ExFreePoolWithTag(NewPath, TAG_NTFS);
    if (NewValue != NULL)
        ExFreePoolWithTag(NewValue, TAG_NTFS);
    NtfsReleaseAttributeHolder(DeviceExt, FileRecord, NameRecord);
    if (Scratch != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Scratch);
    if (Original != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Original);
    if (FileRecord != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsSetLinkInformation
* @implemented
*
* FileLinkInformation: gives a file one more name, a hard link, in the directory the
* I/O manager opened as the target or in the file's own directory (MS-FSA 2.1.5.15.6).
*
* @return
* STATUS_SUCCESS; STATUS_FILE_IS_A_DIRECTORY for a directory, which can't have links;
* STATUS_OBJECT_NAME_COLLISION when the name is taken and ReplaceIfExists isn't set, or
* when it already names this file; STATUS_ACCESS_DENIED when the file it names is a
* directory or open; or the status of the change that failed.
*
* @remarks
* The caller holds DirResource and the FCB's MainResource exclusively.
*/
static
NTSTATUS
NtfsSetLinkInformation(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ PIRP Irp,
    _In_ PNTFS_FCB Fcb,
    _In_opt_ PFILE_OBJECT TargetFileObject,
    _In_ BOOLEAN ReplaceIfExists,
    _In_ BOOLEAN CaseSensitive,
    _In_ PFILE_LINK_INFORMATION LinkInfo,
    _In_ ULONG BufferLength)
{
    UNICODE_STRING NewName;
    PWSTR NewPath = NULL;
    ULONGLONG TargetDirMft;
    ULONGLONG ExistingMft;
    ULONG FirstEntry = 0;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PFILE_RECORD_HEADER Scratch = NULL;
    PFILENAME_ATTRIBUTE OldName, NewValue = NULL;
    PNTFS_ATTR_RECORD Attribute = NULL;
    ULONG NewValueLength;
    NTSTATUS Status;

    C_ASSERT(FIELD_OFFSET(FILE_LINK_INFORMATION, FileName) == FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName));

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_ACCESS_DENIED;

    if ((Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM)) || Fcb->Stream[0] != UNICODE_NULL)
        return STATUS_INVALID_PARAMETER;

    if (NtfsFCBIsDirectory(Fcb))
        return STATUS_FILE_IS_A_DIRECTORY;

    Status = NtfsBuildRenamePath(Fcb, TargetFileObject, (PFILE_RENAME_INFORMATION)LinkInfo, BufferLength,
                                 &NewName, &NewPath, &TargetDirMft);
    if (!NT_SUCCESS(Status))
        return Status;

    if (TargetFileObject == NULL)
        TargetDirMft = Fcb->ParentMFTIndex;

    /* A link can't take the name of the file itself, whatever ReplaceIfExists says */
    Status = NtfsFindMftRecord(DeviceExt, TargetDirMft, &NewName, &FirstEntry, FALSE, CaseSensitive, &ExistingMft, NULL);
    if (NT_SUCCESS(Status) && ExistingMft == Fcb->MFTIndex)
        Status = STATUS_OBJECT_NAME_COLLISION;
    else if (NT_SUCCESS(Status) || Status == STATUS_OBJECT_PATH_NOT_FOUND || Status == STATUS_OBJECT_NAME_NOT_FOUND)
        Status = STATUS_SUCCESS;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    Scratch = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (NT_SUCCESS(Status) && (FileRecord == NULL || Scratch == NULL))
        Status = STATUS_INSUFFICIENT_RESOURCES;

    if (NT_SUCCESS(Status))
        Status = NtfsClearRenameTarget(DeviceExt, Irp, Fcb, TargetDirMft, &NewName, NewPath, ReplaceIfExists, CaseSensitive, NULL, Scratch);

    if (NT_SUCCESS(Status))
        Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);

    /* The new name carries the duplicated information an existing one has */
    if (NT_SUCCESS(Status))
    {
        OldName = GetBestFileNameFromRecord(DeviceExt, FileRecord);
        if (OldName == NULL)
            Status = STATUS_FILE_CORRUPT_ERROR;
    }

    if (NT_SUCCESS(Status))
        Status = NtfsBuildRenamedFileName(DeviceExt, OldName, TargetDirMft, &NewName, CaseSensitive, Scratch, &NewValue, &NewValueLength);

    if (NT_SUCCESS(Status))
    {
        NewValue->NameType = NTFS_FILE_NAME_POSIX;
        Attribute = NtfsBuildFileNameAttribute(NewValue, NewValueLength);
        if (Attribute == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Past what the base record holds, the name goes into an extension record */
    if (NT_SUCCESS(Status))
        Status = NtfsAddAttributeToFile(DeviceExt, FileRecord, Attribute);

    if (NT_SUCCESS(Status))
    {
        FileRecord->LinkCount++;
        Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    }

    if (NT_SUCCESS(Status))
    {
        Status = NtfsAddFilenameToDirectory(DeviceExt,
                                            TargetDirMft,
                                            Fcb->MFTIndex | ((ULONGLONG)FileRecord->SequenceNumber << 48),
                                            NewValue,
                                            CaseSensitive);
        if (NT_SUCCESS(Status))
        {
            NtfsSetLinkCount(DeviceExt, Fcb->MFTIndex, FileRecord->LinkCount);
            NtfsReportPathChange(DeviceExt, NewPath, FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED);
        }
        else if (NT_SUCCESS(ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord)) &&
                 NT_SUCCESS(NtfsRemoveFileNameAttribute(DeviceExt, FileRecord, TargetDirMft, &NewName)))
        {
            /* The name never reached the index: the file doesn't get it */
            FileRecord->LinkCount--;
            UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
        }
    }

    if (Attribute != NULL)
        ExFreePoolWithTag(Attribute, TAG_NTFS);
    if (NewValue != NULL)
        ExFreePoolWithTag(NewValue, TAG_NTFS);
    if (Scratch != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Scratch);
    if (FileRecord != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    ExFreePoolWithTag(NewPath, TAG_NTFS);

    return Status;
}

/**
* @name NtfsJournalSetInformation
*
* Tells the change journal what a successful IRP_MJ_SET_INFORMATION changed. A rename
* reports its old and new names itself, a delete when it happens.
*/
static
VOID
NtfsJournalSetInformation(PDEVICE_EXTENSION DeviceExt,
                          PNTFS_FCB Fcb,
                          FILE_INFORMATION_CLASS FileInformationClass,
                          LONGLONG OldSize)
{
    LONGLONG NewSize = Fcb->RFCB.FileSize.QuadPart;

    switch (FileInformationClass)
    {
        case FileBasicInformation:
            NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_BASIC_INFO_CHANGE);
            break;

        case FileAllocationInformation:
        case FileEndOfFileInformation:
            if (NewSize < OldSize)
                NtfsPostUsnChange(DeviceExt, Fcb, NtfsUsnDataReason(Fcb, USN_REASON_DATA_TRUNCATION));
            else if (NewSize > OldSize)
                NtfsPostUsnChange(DeviceExt, Fcb, NtfsUsnDataReason(Fcb, USN_REASON_DATA_EXTEND));
            break;

        case FileLinkInformation:
            NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_HARD_LINK_CHANGE);
            break;

        default:
            break;
    }
}

NTSTATUS
NtfsSetInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PIO_STACK_LOCATION Stack;
    PDEVICE_EXTENSION DeviceExt;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    BOOLEAN NeedDirResource;
    LONGLONG OldSize;
    ULONG NotifyFilter = 0;
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

    DPRINT("NtfsSetInformation(%p)\n", IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceObject = IrpContext->DeviceObject;
    DeviceExt = DeviceObject->DeviceExtension;
    FileInformationClass = Stack->Parameters.QueryFile.FileInformationClass;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = Stack->Parameters.QueryFile.Length;

    /* A rename or a link changes directories and the FCB table, which creates
     * rely on under DirResource. Take it first, the same order create does. */
    NeedDirResource = (FileInformationClass == FileRenameInformation ||
                       FileInformationClass == FileLinkInformation);
    if (NeedDirResource &&
        !ExAcquireResourceExclusiveLite(&DeviceExt->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    /* Every class below rewrites the file record from a copy it reads, so
     * two of them at once on the same file would lose one update */
    if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        if (NeedDirResource)
            ExReleaseResourceLite(&DeviceExt->DirResource);

        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
    OldSize = Fcb->RFCB.FileSize.QuadPart;

    switch (FileInformationClass)
    {
        PFILE_END_OF_FILE_INFORMATION EndOfFileInfo;

        case FileBasicInformation:
            if (BufferLength < sizeof(FILE_BASIC_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            /* A directory's record carries its index, which a create in it
             * could be changing; IndexResource keeps the two apart */
            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&DeviceExt->IndexResource, TRUE);
            Status = NtfsSetBasicInformation(DeviceExt,
                                             Fcb,
                                             FileObject,
                                             BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                             (PFILE_BASIC_INFORMATION)SystemBuffer,
                                             &NotifyFilter);
            ExReleaseResourceLite(&DeviceExt->IndexResource);
            KeLeaveCriticalRegion();
            break;

        case FilePositionInformation:
            Status = NtfsSetPositionInformation(DeviceExt,
                                                FileObject,
                                                (PFILE_POSITION_INFORMATION)SystemBuffer,
                                                BufferLength);
            break;

        case FileAllocationInformation:
            if (BufferLength < sizeof(FILE_ALLOCATION_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Status = NtfsSetAllocationInformation(Fcb,
                                                  FileObject,
                                                  DeviceExt,
                                                  Irp->Flags,
                                                  BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                                  (PFILE_ALLOCATION_INFORMATION)SystemBuffer);
            break;

        case FileEndOfFileInformation:
            Status = NtfsCheckQuotaGrowth(IrpContext);
            if (!NT_SUCCESS(Status))
                break;
            EndOfFileInfo = (PFILE_END_OF_FILE_INFORMATION)SystemBuffer;
            Status = NtfsSetEndOfFile(Fcb,
                                      FileObject,
                                      DeviceExt,
                                      Irp->Flags,
                                      BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                      &EndOfFileInfo->EndOfFile);
            break;

        case FileDispositionInformation:
            if (BufferLength < sizeof(FILE_DISPOSITION_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Status = NtfsSetDispositionInformation(DeviceExt,
                                                   Fcb,
                                                   FileObject,
                                                   BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                                   (PFILE_DISPOSITION_INFORMATION)SystemBuffer);
            break;

        case FileLinkInformation:
            if (BufferLength < FIELD_OFFSET(FILE_LINK_INFORMATION, FileName))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Status = NtfsSetLinkInformation(DeviceExt,
                                            Irp,
                                            Fcb,
                                            Stack->Parameters.SetFile.FileObject,
                                            Stack->Parameters.SetFile.ReplaceIfExists,
                                            BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                            (PFILE_LINK_INFORMATION)SystemBuffer,
                                            BufferLength);
            break;

        case FileRenameInformation:
            if (BufferLength < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Status = NtfsSetRenameInformation(DeviceExt,
                                              Irp,
                                              Fcb,
                                              FileObject->FsContext2,
                                              Stack->Parameters.SetFile.FileObject,
                                              Stack->Parameters.SetFile.ReplaceIfExists,
                                              BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                              (PFILE_RENAME_INFORMATION)SystemBuffer,
                                              BufferLength);
            break;

        // TODO: all other information classes

        default:
            DPRINT1("FIXME: Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_NOT_IMPLEMENTED;
    }

    if (NT_SUCCESS(Status))
    {
        NtfsJournalSetInformation(DeviceExt, Fcb, FileInformationClass, OldSize);
        if (FileInformationClass == FileEndOfFileInformation ||
            (FileInformationClass == FileAllocationInformation && OldSize != Fcb->RFCB.FileSize.QuadPart))
            FileObject->Flags |= FO_FILE_MODIFIED;
        else if (FileInformationClass == FileAllocationInformation)
            ((PNTFS_CCB)FileObject->FsContext2)->Flags |= CCB_UPDATE_CHANGE_TIME;
        if ((FileInformationClass == FileEndOfFileInformation ||
             FileInformationClass == FileAllocationInformation) &&
            OldSize != Fcb->RFCB.FileSize.QuadPart)
            NotifyFilter |= FILE_NOTIFY_CHANGE_SIZE;
    }

    ExReleaseResourceLite(&Fcb->PagingIoResource);
    ExReleaseResourceLite(&Fcb->MainResource);

    if (NeedDirResource)
        ExReleaseResourceLite(&DeviceExt->DirResource);

    if (NotifyFilter != 0)
    {
        if (FileInformationClass == FileBasicInformation)
            NtfsReportChange(Fcb, NotifyFilter, FILE_ACTION_MODIFIED);
        else
            NtfsReportDataChange(Fcb, NotifyFilter);
    }

    if (NT_SUCCESS(Status) && FileInformationClass == FileDispositionInformation &&
        NtfsFCBIsDirectory(Fcb) && (Fcb->Flags & FCB_DELETE_PENDING))
        NtfsNotifyDirectoryDeleted(Fcb);

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information =
        Stack->Parameters.QueryFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}
/* EOF */
