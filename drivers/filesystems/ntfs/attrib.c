/*
 *  ReactOS kernel
 *  Copyright (C) 2002,2003 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/attrib.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Valentin Verkhovsky
 *                   Hervé Poussineau (hpoussin@reactos.org)
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"
#include <ntintsafe.h>

#define NDEBUG
#include <debug.h>

/* Forward declarations for the $ATTRIBUTE_LIST bookkeeping helpers (defined
 * further down with the rest of the attribute-list machinery); the generic
 * Add/RemoveResidentAttribute paths keep the list in sync through these. */
static
PNTFS_ATTR_RECORD
NtfsFindAttributeListSlot(PFILE_RECORD_HEADER FileRecord);

static
NTSTATUS
NtfsAttributeListAddEntry(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER BaseFileRecord,
                          PNTFS_ATTR_RECORD ListAttr,
                          ULONG AttrType,
                          PCWSTR Name,
                          UCHAR NameLength,
                          ULONGLONG StartingVCN,
                          ULONGLONG MftRef,
                          USHORT Instance);

static
NTSTATUS
NtfsAttributeListRemoveEntry(PNTFS_VCB Vcb,
                             PFILE_RECORD_HEADER BaseFileRecord,
                             PNTFS_ATTR_RECORD ListAttr,
                             ULONG AttrType,
                             PCWSTR Name,
                             UCHAR NameLength,
                             ULONGLONG MftRef,
                             USHORT Instance);

static
NTSTATUS
NtfsConvertAttributeListToNonResident(PNTFS_VCB Vcb,
                                      PFILE_RECORD_HEADER BaseFileRecord);

/* FUNCTIONS ****************************************************************/

/**
* @name AddBitmap
* @implemented
*
* Adds a $BITMAP attribute to a given FileRecord.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param FileRecord
* Pointer to a complete file record to add the attribute to.
*
* @param AttributeAddress
* Pointer to the region of memory that will receive the $INDEX_ALLOCATION attribute.
* This address must reside within FileRecord. Must be aligned to an 8-byte boundary (relative to FileRecord).
*
* @param Name
* Pointer to a string of 16-bit Unicode characters naming the attribute. Most often L"$I30".
*
* @param NameLength
* The number of wide-characters in the name. L"$I30" Would use 4 here.
*
* @return
* STATUS_SUCCESS on success. STATUS_NOT_IMPLEMENTED if target address isn't at the end
* of the given file record, or if the file record isn't large enough for the attribute.
*
* @remarks
* Only adding the attribute to the end of the file record is supported; AttributeAddress must
* be of type AttributeEnd.
* This could be improved by adding an $ATTRIBUTE_LIST to the file record if there's not enough space.
*
*/
NTSTATUS
AddBitmap(PNTFS_VCB Vcb,
          PFILE_RECORD_HEADER FileRecord,
          PNTFS_ATTR_RECORD AttributeAddress,
          PCWSTR Name,
          USHORT NameLength)
{
    ULONG AttributeLength;
    // Calculate the header length
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG FileRecordEnd = AttributeAddress->Length;
    ULONG NameOffset;
    ULONG ValueOffset;
    // We'll start out with 8 bytes of bitmap data
    ULONG ValueLength = 8;
    ULONG BytesAvailable;

    if (AttributeAddress->Type != AttributeEnd)
    {
        DPRINT1("FIXME: Can only add $BITMAP attribute to the end of a file record.\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    NameOffset = ResidentHeaderLength;

    // Calculate ValueOffset, which will be aligned to a 4-byte boundary
    ValueOffset = ALIGN_UP_BY(NameOffset + (sizeof(WCHAR) * NameLength), VALUE_OFFSET_ALIGNMENT);

    // Calculate length of attribute
    AttributeLength = ValueOffset + ValueLength;
    AttributeLength = ALIGN_UP_BY(AttributeLength, ATTR_RECORD_ALIGNMENT);

    // Make sure the file record is large enough for the new attribute
    BytesAvailable = Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse;
    if (BytesAvailable < AttributeLength)
    {
        DPRINT1("FIXME: Not enough room in file record for index allocation attribute!\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    // Set Attribute fields
    RtlZeroMemory(AttributeAddress, AttributeLength);

    AttributeAddress->Type = AttributeBitmap;
    AttributeAddress->Length = AttributeLength;
    AttributeAddress->NameLength = NameLength;
    AttributeAddress->NameOffset = NameOffset;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    AttributeAddress->Resident.ValueLength = ValueLength;
    AttributeAddress->Resident.ValueOffset = ValueOffset;

    // Set the name
    RtlCopyMemory((PCHAR)((ULONG_PTR)AttributeAddress + NameOffset), Name, NameLength * sizeof(WCHAR));

    // move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);

    return STATUS_SUCCESS;
}

/**
* @name AddData
* @implemented
*
* Adds a $DATA attribute to a given FileRecord.
*
* @param FileRecord
* Pointer to a complete file record to add the attribute to. Caller is responsible for
* ensuring FileRecord is large enough to contain $DATA.
*
* @param AttributeAddress
* Pointer to the region of memory that will receive the $DATA attribute.
* This address must reside within FileRecord. Must be aligned to an 8-byte boundary (relative to FileRecord).
*
* @return
* STATUS_SUCCESS on success. STATUS_NOT_IMPLEMENTED if target address isn't at the end
* of the given file record.
*
* @remarks
* Only adding the attribute to the end of the file record is supported; AttributeAddress must
* be of type AttributeEnd.
* As it's implemented, this function is only intended to assist in creating new file records. It
* could be made more general-purpose by considering file records with an $ATTRIBUTE_LIST.
* It's the caller's responsibility to ensure the given file record has enough memory allocated
* for the attribute.
*/
NTSTATUS
AddData(PFILE_RECORD_HEADER FileRecord,
        PNTFS_ATTR_RECORD AttributeAddress)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG FileRecordEnd = AttributeAddress->Length;

    if (AttributeAddress->Type != AttributeEnd)
    {
        DPRINT1("FIXME: Can only add $DATA attribute to the end of a file record.\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    AttributeAddress->Type = AttributeData;
    AttributeAddress->Length = ResidentHeaderLength;
    AttributeAddress->Length = ALIGN_UP_BY(AttributeAddress->Length, ATTR_RECORD_ALIGNMENT);
    AttributeAddress->Resident.ValueLength = 0;
    AttributeAddress->Resident.ValueOffset = ResidentHeaderLength;

    // for unnamed $DATA attributes, NameOffset equals header length
    AttributeAddress->NameOffset = ResidentHeaderLength;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    // move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);

    return STATUS_SUCCESS;
}

/**
* @name AddDataStream
* @implemented
*
* Adds a named $DATA attribute (an alternate data stream) to a file record,
* initially resident and empty.  Unlike the other Add* helpers this inserts
* at the correct sorted position - NTFS keeps attributes ordered by type and,
* within a type, by name (case-insensitive $UpCase collation, unnamed first) -
* rather than requiring the insertion point to be the AttributeEnd marker.
* Growth beyond the record happens later through the ordinary
* SetAttributeDataLength machinery, which converts to non-resident and
* preserves the attribute name.
*
* @param Vcb
* Pointer to the NTFS_VCB for the destination volume.
*
* @param FileRecord
* Pointer to a complete file record.  Modified in place; the caller is
* responsible for writing it back with UpdateFileRecord.
*
* @param Name
* The stream name (case-preserved on disk). Must not be empty.
*
* @param NameLength
* Number of WCHARs in Name (at most 255, the on-disk NameLength is a UCHAR).
*
* @return
* STATUS_SUCCESS on success.
* STATUS_OBJECT_NAME_INVALID for an empty or over-long name.
* STATUS_OBJECT_NAME_COLLISION if a $DATA attribute with this name exists.
* STATUS_DISK_FULL if the file record has no room for the new attribute.
* STATUS_NOT_IMPLEMENTED if the record carries an $ATTRIBUTE_LIST (keeping
* the list in sync with an insertion is out of scope for this helper).
*/
NTSTATUS
AddDataStream(PNTFS_VCB Vcb,
              PFILE_RECORD_HEADER FileRecord,
              PCWSTR Name,
              USHORT NameLength)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    PNTFS_ATTR_RECORD Attribute;
    PNTFS_ATTR_RECORD InsertPoint = NULL;
    UNICODE_STRING NewName;
    ULONG NameOffset;
    ULONG ValueOffset;
    ULONG AttributeLength;
    ULONG InsertOffset;
    ULONG TailLength;

    if (NameLength == 0 || NameLength > 255)
        return STATUS_OBJECT_NAME_INVALID;

    NewName.Buffer = (PWSTR)Name;
    NewName.Length = NewName.MaximumLength = NameLength * sizeof(WCHAR);

    /* Walk the whole record: find the sorted insertion point and make sure
     * the name isn't taken (case-insensitively, like the on-disk collation). */
    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
    while (Attribute->Type != AttributeEnd &&
           (ULONG_PTR)Attribute < (ULONG_PTR)FileRecord + FileRecord->BytesInUse)
    {
        if (Attribute->Type == AttributeAttributeList)
        {
            /* Every attribute must also be described by a list entry once a
             * record has an $ATTRIBUTE_LIST; inserting here without updating
             * the list would corrupt the chain. */
            DPRINT1("AddDataStream: record has an $ATTRIBUTE_LIST, not supported\n");
            return STATUS_NOT_IMPLEMENTED;
        }

        if (Attribute->Type == AttributeData && Attribute->NameLength != 0)
        {
            UNICODE_STRING ExistingName;
            LONG Comparison;

            ExistingName.Buffer = (PWSTR)((ULONG_PTR)Attribute + Attribute->NameOffset);
            ExistingName.Length = ExistingName.MaximumLength = Attribute->NameLength * sizeof(WCHAR);

            Comparison = RtlCompareUnicodeString(&ExistingName, &NewName, TRUE);
            if (Comparison == 0)
                return STATUS_OBJECT_NAME_COLLISION;
            if (Comparison > 0 && InsertPoint == NULL)
                InsertPoint = Attribute;
        }
        else if (Attribute->Type > AttributeData && InsertPoint == NULL)
        {
            /* Unnamed $DATA sorts before every named one, so it never
             * becomes the insertion point. */
            InsertPoint = Attribute;
        }

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
    }

    if (InsertPoint == NULL)
        InsertPoint = Attribute;

    NameOffset = ResidentHeaderLength;
    ValueOffset = ALIGN_UP_BY(NameOffset + (NameLength * sizeof(WCHAR)), VALUE_OFFSET_ALIGNMENT);
    AttributeLength = ALIGN_UP_BY(ValueOffset, ATTR_RECORD_ALIGNMENT);

    if (Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse < AttributeLength)
    {
        DPRINT1("AddDataStream: not enough room in file record (need %lu, have %lu)\n",
                AttributeLength,
                Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse);
        return STATUS_DISK_FULL;
    }

    /* Slide the tail (up to and including the AttributeEnd marker and the
     * trailing end ULONG accounted in BytesInUse) out of the way. */
    InsertOffset = (ULONG)((ULONG_PTR)InsertPoint - (ULONG_PTR)FileRecord);
    ASSERT(InsertOffset <= FileRecord->BytesInUse);
    TailLength = FileRecord->BytesInUse - InsertOffset;
    RtlMoveMemory((PUCHAR)InsertPoint + AttributeLength, InsertPoint, TailLength);

    RtlZeroMemory(InsertPoint, AttributeLength);
    InsertPoint->Type = AttributeData;
    InsertPoint->Length = AttributeLength;
    InsertPoint->IsNonResident = 0;
    InsertPoint->NameLength = (UCHAR)NameLength;
    InsertPoint->NameOffset = (USHORT)NameOffset;
    InsertPoint->Flags = 0;
    InsertPoint->Instance = FileRecord->NextAttributeNumber++;
    InsertPoint->Resident.ValueLength = 0;
    InsertPoint->Resident.ValueOffset = (USHORT)ValueOffset;
    InsertPoint->Resident.Flags = 0;
    RtlCopyMemory((PUCHAR)InsertPoint + NameOffset, Name, NameLength * sizeof(WCHAR));

    FileRecord->BytesInUse += AttributeLength;

    return STATUS_SUCCESS;
}

/**
* @name AddResidentAttribute
* @implemented
*
* Adds an arbitrary resident attribute (type + optional name + value) to a
* FileRecord.  Appends the new attribute slot immediately before the current
* AttributeEnd marker and moves the end marker forward.
*
* @param Vcb
* Pointer to the NTFS_VCB for the destination volume (for BytesPerFileRecord).
*
* @param FileRecord
* Pointer to a complete file record.  The record must currently end with an
* AttributeEnd marker; the new attribute is inserted in that slot.
*
* @param Type
* The attribute type code (e.g. AttributeReparsePoint, AttributeObjectId).
*
* @param Name
* Optional UTF-16 name for the attribute (may be NULL if NameLength == 0).
*
* @param NameLength
* Number of WCHARs in Name.
*
* @param Data
* The value bytes to store in the attribute.  May be NULL iff DataLength == 0.
*
* @param DataLength
* Size of Data in bytes.
*
* @return
* STATUS_SUCCESS on success.  STATUS_DISK_FULL if the file record does not
* have enough free space.  STATUS_NOT_IMPLEMENTED if the record does not
* currently terminate with an AttributeEnd marker (same convention as the
* other Add* helpers).
*
* @remarks
* The attribute is always added resident.  Growing a record that already
* has an $ATTRIBUTE_LIST is out of scope here; plan callers only use this to
* create fresh short attributes ($REPARSE_POINT, $OBJECT_ID) on files that
* don't yet have the slot.
*/
NTSTATUS
AddResidentAttribute(PNTFS_VCB Vcb,
                     PFILE_RECORD_HEADER FileRecord,
                     ULONG Type,
                     PCWSTR Name,
                     USHORT NameLength,
                     const VOID *Data,
                     ULONG DataLength)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG NameOffset;
    ULONG ValueOffset;
    ULONG AttributeLength;
    ULONG FileRecordEnd;
    ULONG BytesAvailable;
    PNTFS_ATTR_RECORD AttributeAddress;
    PNTFS_ATTR_RECORD InsertPoint = NULL;
    PNTFS_ATTR_RECORD NextSlot;

    /* Locate the current end marker, and the type-sorted insertion slot on
     * the way (first attribute with a higher type code; stable among equal
     * types).  Windows and ntfs-3g require a record's attributes in
     * ascending type order - appending at the end put e.g. a hardlink's
     * $FILE_NAME (0x30) after $DATA (0x80), which makes ntfs-3g reject the
     * whole record. */
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
    while (AttributeAddress->Type != AttributeEnd &&
           (ULONG_PTR)AttributeAddress < (ULONG_PTR)FileRecord + FileRecord->BytesInUse)
    {
        if (InsertPoint == NULL && AttributeAddress->Type > Type)
            InsertPoint = AttributeAddress;
        AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    }

    if (AttributeAddress->Type != AttributeEnd)
    {
        DPRINT1("AddResidentAttribute: file record has no AttributeEnd marker\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    FileRecordEnd = AttributeAddress->Length;

    NameOffset = ResidentHeaderLength;
    ValueOffset = ALIGN_UP_BY(NameOffset + (sizeof(WCHAR) * NameLength), VALUE_OFFSET_ALIGNMENT);
    AttributeLength = ALIGN_UP_BY(ValueOffset + DataLength, ATTR_RECORD_ALIGNMENT);

    BytesAvailable = Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse;
    if (BytesAvailable < AttributeLength)
    {
        /* Not DPRINT1: STATUS_DISK_FULL here is ordinary flow control - the
         * hard-link path probes with AddResidentAttribute and falls back to
         * the $ATTRIBUTE_LIST spill (NtfsAddHardLinkSpill) on every link
         * beyond base-record capacity. */
        DPRINT("AddResidentAttribute: not enough room (need %u, have %u)\n",
               AttributeLength, BytesAvailable);
        return STATUS_DISK_FULL;
    }

    if (InsertPoint != NULL)
    {
        /* Make room mid-chain: shift the higher-typed attributes (and the
         * end markers) right and build the new attribute in the hole. */
        ULONG InsertOffset = (ULONG)((ULONG_PTR)InsertPoint - (ULONG_PTR)FileRecord);
        PNTFS_ATTR_RECORD MovedFinal;

        MovedFinal = MoveAttributes(Vcb, InsertPoint, InsertOffset,
                                    (ULONG_PTR)InsertPoint + AttributeLength);
        SetFileRecordEnd(FileRecord, MovedFinal, FileRecordEnd);
        AttributeAddress = InsertPoint;
    }

    RtlZeroMemory(AttributeAddress, AttributeLength);
    AttributeAddress->Type = Type;
    AttributeAddress->Length = AttributeLength;
    AttributeAddress->IsNonResident = 0;
    AttributeAddress->NameLength = (UCHAR)NameLength;
    AttributeAddress->NameOffset = (USHORT)NameOffset;
    AttributeAddress->Flags = 0;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    AttributeAddress->Resident.ValueLength = DataLength;
    AttributeAddress->Resident.ValueOffset = (USHORT)ValueOffset;
    AttributeAddress->Resident.Flags = 0;

    if (NameLength != 0 && Name != NULL)
    {
        RtlCopyMemory((PCHAR)((ULONG_PTR)AttributeAddress + NameOffset),
                      Name,
                      NameLength * sizeof(WCHAR));
    }

    if (DataLength != 0 && Data != NULL)
    {
        RtlCopyMemory((PCHAR)((ULONG_PTR)AttributeAddress + ValueOffset),
                      Data,
                      DataLength);
    }

    if (InsertPoint == NULL)
    {
        NextSlot = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeLength);
        SetFileRecordEnd(FileRecord, NextSlot, FileRecordEnd);
    }

    /* Keep the $ATTRIBUTE_LIST complete: Windows resolves attributes
     * exclusively through the list once one exists, so every base attribute
     * must be described by a (base-pointing) entry. */
    if (Type != AttributeAttributeList)
    {
        PNTFS_ATTR_RECORD ListAttr = NtfsFindAttributeListSlot(FileRecord);

        if (ListAttr != NULL)
        {
            ULONGLONG BaseRef = ((ULONGLONG)FileRecord->SequenceNumber << 48) |
                                FileRecord->MFTRecordNumber;
            USHORT NewInstance = AttributeAddress->Instance;
            NTSTATUS Status;

            Status = NtfsAttributeListAddEntry(Vcb, FileRecord, ListAttr,
                                               Type, Name, (UCHAR)NameLength,
                                               0, BaseRef, NewInstance);
            if (Status == STATUS_DISK_FULL && !ListAttr->IsNonResident)
            {
                /* The resident list can't grow inside the record - convert it
                 * to non-resident (Windows does the same once the list
                 * outgrows the record) and retry. */
                Status = NtfsConvertAttributeListToNonResident(Vcb, FileRecord);
                if (NT_SUCCESS(Status))
                {
                    ListAttr = NtfsFindAttributeListSlot(FileRecord);
                    ASSERT(ListAttr != NULL && ListAttr->IsNonResident);
                    Status = NtfsAttributeListAddEntry(Vcb, FileRecord, ListAttr,
                                                       Type, Name, (UCHAR)NameLength,
                                                       0, BaseRef, NewInstance);
                }
            }

            if (!NT_SUCCESS(Status))
            {
                /* Roll the new attribute back out so record and list stay in
                 * sync.  The list operations may have moved the slot -
                 * re-find it by (type, instance). */
                PNTFS_ATTR_RECORD Attr =
                    (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

                while (Attr->Type != AttributeEnd && Attr->Length > 0 &&
                       (ULONG_PTR)Attr < (ULONG_PTR)FileRecord + FileRecord->BytesInUse)
                {
                    if (Attr->Type == Type && Attr->Instance == NewInstance)
                    {
                        RemoveResidentAttribute(Vcb, FileRecord, Attr);
                        break;
                    }
                    Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
                }
                return Status;
            }
        }
    }

    return STATUS_SUCCESS;
}

/**
* @name AddFileName
* @implemented
*
* Adds a $FILE_NAME attribute to a given FileRecord.
*
* @param FileRecord
* Pointer to a complete file record to add the attribute to. Caller is responsible for
* ensuring FileRecord is large enough to contain $FILE_NAME.
*
* @param AttributeAddress
* Pointer to the region of memory that will receive the $FILE_NAME attribute.
* This address must reside within FileRecord. Must be aligned to an 8-byte boundary (relative to FileRecord).
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param FileObject
* Pointer to the FILE_OBJECT which represents the new name.
* This parameter is used to determine the filename and parent directory.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application opened the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param ParentMftIndex
* Pointer to a ULONGLONG which will receive the index of the parent directory.
*
* @return
* STATUS_SUCCESS on success. STATUS_NOT_IMPLEMENTED if target address isn't at the end
* of the given file record.
*
* @remarks
* Only adding the attribute to the end of the file record is supported; AttributeAddress must
* be of type AttributeEnd.
* As it's implemented, this function is only intended to assist in creating new file records. It
* could be made more general-purpose by considering file records with an $ATTRIBUTE_LIST.
* It's the caller's responsibility to ensure the given file record has enough memory allocated
* for the attribute.
*/
NTSTATUS
AddFileName(PFILE_RECORD_HEADER FileRecord,
            PNTFS_ATTR_RECORD AttributeAddress,
            PDEVICE_EXTENSION DeviceExt,
            PFILE_OBJECT FileObject,
            BOOLEAN CaseSensitive,
            ULONG FileAttributes,
            PULONGLONG ParentMftIndex)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    PFILENAME_ATTRIBUTE FileNameAttribute;
    PFILE_RECORD_HEADER ParentFileRecord = NULL;
    LARGE_INTEGER SystemTime;
    ULONG FileRecordEnd = AttributeAddress->Length;
    ULONGLONG CurrentMFTIndex = NTFS_FILE_ROOT;
    USHORT ParentSequenceNumber;
    UNICODE_STRING Current, Remaining, FilenameNoPath;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG FirstEntry;
    PNTFS_FCB RelatedFcb;

    if (AttributeAddress->Type != AttributeEnd)
    {
        DPRINT1("FIXME: Can only add $FILE_NAME attribute to the end of a file record.\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    AttributeAddress->Type = AttributeFileName;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    FileNameAttribute = (PFILENAME_ATTRIBUTE)((LONG_PTR)AttributeAddress + ResidentHeaderLength);

    // set timestamps
    KeQuerySystemTime(&SystemTime);
    FileNameAttribute->CreationTime = SystemTime.QuadPart;
    FileNameAttribute->ChangeTime = SystemTime.QuadPart;
    FileNameAttribute->LastWriteTime = SystemTime.QuadPart;
    FileNameAttribute->LastAccessTime = SystemTime.QuadPart;

    // Honor the caller-requested attributes (SYSTEM/HIDDEN/READONLY/...),
    // keeping the structural DIRECTORY bit for directories and defaulting a
    // file to ARCHIVE when nothing else was asked for.
    if(FileRecord->Flags & FRH_DIRECTORY)
        FileNameAttribute->FileAttributes = NTFS_FILE_TYPE_DIRECTORY | (FileAttributes & NTFS_FILE_ATTRIBUTE_VALID_FLAGS);
    else
        FileNameAttribute->FileAttributes = NTFS_FILE_TYPE_ARCHIVE | (FileAttributes & NTFS_FILE_ATTRIBUTE_VALID_FLAGS);

    // we need to extract the filename from the path
    DPRINT("Pathname: %wZ\n", &FileObject->FileName);

    if (FileObject->RelatedFileObject != NULL &&
        FileObject->FileName.Length != 0 &&
        FileObject->FileName.Buffer[0] != OBJ_NAME_PATH_SEPARATOR)
    {
        RelatedFcb = FileObject->RelatedFileObject->FsContext;
        if (RelatedFcb == NULL || !NtfsFCBIsDirectory(RelatedFcb))
            return STATUS_INVALID_PARAMETER;

        CurrentMFTIndex = RelatedFcb->MFTIndex;
    }

    FsRtlDissectName(FileObject->FileName, &Current, &Remaining);

    FilenameNoPath.Buffer = Current.Buffer;
    FilenameNoPath.MaximumLength = FilenameNoPath.Length = Current.Length;

    while (Current.Length != 0)
    {
        DPRINT("Current: %wZ\n", &Current);

        if (Remaining.Length != 0)
        {
            FilenameNoPath.Buffer = Remaining.Buffer;
            FilenameNoPath.Length = FilenameNoPath.MaximumLength = Remaining.Length;
        }

        FirstEntry = 0;
        Status = NtfsFindMftRecord(DeviceExt,
                                   CurrentMFTIndex,
                                   &Current,
                                   &FirstEntry,
                                   FALSE,
                                   CaseSensitive,
                                   &CurrentMFTIndex,
                                   NULL);
        if (!NT_SUCCESS(Status))
        {
            /*
             * Only the final path component may legitimately be missing when
             * creating a new entry. Any earlier failure means the parent path
             * does not exist, and silently falling back to the last successful
             * directory misparents the new file.
             */
            if (Remaining.Length != 0 || Current.Length == 0)
                return STATUS_OBJECT_PATH_NOT_FOUND;

            Status = STATUS_SUCCESS;
            break;
        }

        if (Remaining.Length == 0 )
        {
            if (Current.Length != 0)
            {
                FilenameNoPath.Buffer = Current.Buffer;
                FilenameNoPath.Length = FilenameNoPath.MaximumLength = Current.Length;
            }
            break;
        }

        FsRtlDissectName(Remaining, &Current, &Remaining);
    }

    DPRINT("MFT Index of parent: %I64u\n", CurrentMFTIndex);

    // set reference to parent directory
    FileNameAttribute->DirectoryFileReferenceNumber = CurrentMFTIndex;
    *ParentMftIndex = CurrentMFTIndex;

    ParentSequenceNumber = NTFS_FILE_ROOT;
    if (CurrentMFTIndex != NTFS_FILE_ROOT)
    {
        ParentFileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        if (!ParentFileRecord)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = ReadFileRecord(DeviceExt, CurrentMFTIndex, ParentFileRecord);
        if (!NT_SUCCESS(Status))
        {
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            return Status;
        }

        ParentSequenceNumber = ParentFileRecord->SequenceNumber;
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        ParentFileRecord = NULL;
    }

    DPRINT("SequenceNumber: 0x%02x\n", ParentSequenceNumber);

    // The highest 2 bytes should be the sequence number, unless the parent happens to be root
    if (CurrentMFTIndex == NTFS_FILE_ROOT)
        FileNameAttribute->DirectoryFileReferenceNumber |= (ULONGLONG)NTFS_FILE_ROOT << 48;
    else
        FileNameAttribute->DirectoryFileReferenceNumber |= (ULONGLONG)ParentSequenceNumber << 48;

    DPRINT("FileNameAttribute->DirectoryFileReferenceNumber: 0x%016I64x\n", FileNameAttribute->DirectoryFileReferenceNumber);

    FileNameAttribute->NameLength = FilenameNoPath.Length / sizeof(WCHAR);
    RtlCopyMemory(FileNameAttribute->Name, FilenameNoPath.Buffer, FilenameNoPath.Length);

    // For now, we're emulating the way Windows behaves when 8.3 name generation is disabled
    // TODO: add DOS Filename as needed
    if (!CaseSensitive && RtlIsNameLegalDOS8Dot3(&FilenameNoPath, NULL, NULL))
        FileNameAttribute->NameType = NTFS_FILE_NAME_WIN32_AND_DOS;
    else
        FileNameAttribute->NameType = NTFS_FILE_NAME_POSIX;

    FileRecord->LinkCount++;

    AttributeAddress->Length = ResidentHeaderLength +
        FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + FilenameNoPath.Length;
    AttributeAddress->Length = ALIGN_UP_BY(AttributeAddress->Length, ATTR_RECORD_ALIGNMENT);

    AttributeAddress->Resident.ValueLength = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + FilenameNoPath.Length;
    AttributeAddress->Resident.ValueOffset = ResidentHeaderLength;
    AttributeAddress->Resident.Flags = RA_INDEXED;

    // move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);

    return Status;
}

/**
* @name AddIndexAllocation
* @implemented
*
* Adds an $INDEX_ALLOCATION attribute to a given FileRecord.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param FileRecord
* Pointer to a complete file record to add the attribute to.
*
* @param AttributeAddress
* Pointer to the region of memory that will receive the $INDEX_ALLOCATION attribute.
* This address must reside within FileRecord. Must be aligned to an 8-byte boundary (relative to FileRecord).
*
* @param Name
* Pointer to a string of 16-bit Unicode characters naming the attribute. Most often, this will be L"$I30".
*
* @param NameLength
* The number of wide-characters in the name. L"$I30" Would use 4 here.
*
* @return
* STATUS_SUCCESS on success. STATUS_NOT_IMPLEMENTED if target address isn't at the end
* of the given file record, or if the file record isn't large enough for the attribute.
*
* @remarks
* Only adding the attribute to the end of the file record is supported; AttributeAddress must
* be of type AttributeEnd.
* This could be improved by adding an $ATTRIBUTE_LIST to the file record if there's not enough space.
*
*/
NTSTATUS
AddIndexAllocation(PNTFS_VCB Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   PNTFS_ATTR_RECORD AttributeAddress,
                   PCWSTR Name,
                   USHORT NameLength)
{
    ULONG RecordLength;
    ULONG FileRecordEnd;
    ULONG NameOffset;
    ULONG DataRunOffset;
    ULONG BytesAvailable;

    if (AttributeAddress->Type != AttributeEnd)
    {
        DPRINT1("FIXME: Can only add $INDEX_ALLOCATION attribute to the end of a file record.\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    // Calculate the name offset
    NameOffset = FIELD_OFFSET(NTFS_ATTR_RECORD, NonResident.CompressedSize);

    // Calculate the offset to the first data run
    DataRunOffset = (sizeof(WCHAR) * NameLength) + NameOffset;
    // The data run offset must be aligned to a 4-byte boundary
    DataRunOffset = ALIGN_UP_BY(DataRunOffset, DATA_RUN_ALIGNMENT);

    // Calculate the length of the new attribute; the empty data run will consist of a single byte
    RecordLength = DataRunOffset + 1;

    // The size of the attribute itself must be aligned to an 8 - byte boundary
    RecordLength = ALIGN_UP_BY(RecordLength, ATTR_RECORD_ALIGNMENT);

    // Back up the last 4-bytes of the file record (even though this value doesn't matter)
    FileRecordEnd = AttributeAddress->Length;

    // Make sure the file record can contain the new attribute
    BytesAvailable = Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse;
    if (BytesAvailable < RecordLength)
    {
        DPRINT1("FIXME: Not enough room in file record for index allocation attribute!\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    // Set fields of attribute header
    RtlZeroMemory(AttributeAddress, RecordLength);

    AttributeAddress->Type = AttributeIndexAllocation;
    AttributeAddress->Length = RecordLength;
    AttributeAddress->IsNonResident = TRUE;
    AttributeAddress->NameLength = NameLength;
    AttributeAddress->NameOffset = NameOffset;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    AttributeAddress->NonResident.MappingPairsOffset = DataRunOffset;
    AttributeAddress->NonResident.HighestVCN = (LONGLONG)-1;

    // Set the name
    RtlCopyMemory((PCHAR)((ULONG_PTR)AttributeAddress + NameOffset), Name, NameLength * sizeof(WCHAR));

    // move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);

    return STATUS_SUCCESS;
}

/**
* @name AddIndexRoot
* @implemented
*
* Adds an $INDEX_ROOT attribute to a given FileRecord.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param FileRecord
* Pointer to a complete file record to add the attribute to. Caller is responsible for
* ensuring FileRecord is large enough to contain $INDEX_ROOT.
*
* @param AttributeAddress
* Pointer to the region of memory that will receive the $INDEX_ROOT attribute.
* This address must reside within FileRecord. Must be aligned to an 8-byte boundary (relative to FileRecord).
*
* @param NewIndexRoot
* Pointer to an INDEX_ROOT_ATTRIBUTE containing the index root that will be copied to the new attribute.
*
* @param RootLength
* The length of NewIndexRoot, in bytes.
*
* @param Name
* Pointer to a string of 16-bit Unicode characters naming the attribute. Most often, this will be L"$I30".
*
* @param NameLength
* The number of wide-characters in the name. L"$I30" Would use 4 here.
*
* @return
* STATUS_SUCCESS on success. STATUS_NOT_IMPLEMENTED if target address isn't at the end
* of the given file record.
*
* @remarks
* This function is intended to assist in creating new folders.
* Only adding the attribute to the end of the file record is supported; AttributeAddress must
* be of type AttributeEnd.
* It's the caller's responsibility to ensure the given file record has enough memory allocated
* for the attribute, and this memory must have been zeroed.
*/
NTSTATUS
AddIndexRoot(PNTFS_VCB Vcb,
             PFILE_RECORD_HEADER FileRecord,
             PNTFS_ATTR_RECORD AttributeAddress,
             PINDEX_ROOT_ATTRIBUTE NewIndexRoot,
             ULONG RootLength,
             PCWSTR Name,
             USHORT NameLength)
{
    ULONG AttributeLength;
    // Calculate the header length
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    // Back up the file record's final ULONG (even though it doesn't matter)
    ULONG FileRecordEnd = AttributeAddress->Length;
    ULONG NameOffset;
    ULONG ValueOffset;
    ULONG BytesAvailable;

    if (AttributeAddress->Type != AttributeEnd)
    {
        DPRINT1("FIXME: Can only add $DATA attribute to the end of a file record.\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    NameOffset = ResidentHeaderLength;

    // Calculate ValueOffset, which will be aligned to a 4-byte boundary
    ValueOffset = ALIGN_UP_BY(NameOffset + (sizeof(WCHAR) * NameLength), VALUE_OFFSET_ALIGNMENT);

    // Calculate length of attribute
    AttributeLength = ValueOffset + RootLength;
    AttributeLength = ALIGN_UP_BY(AttributeLength, ATTR_RECORD_ALIGNMENT);

    // Make sure the file record is large enough for the new attribute
    BytesAvailable = Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse;
    if (BytesAvailable < AttributeLength)
    {
        DPRINT1("FIXME: Not enough room in file record for index allocation attribute!\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    // Set Attribute fields
    RtlZeroMemory(AttributeAddress, AttributeLength);

    AttributeAddress->Type = AttributeIndexRoot;
    AttributeAddress->Length = AttributeLength;
    AttributeAddress->NameLength = NameLength;
    AttributeAddress->NameOffset = NameOffset;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    AttributeAddress->Resident.ValueLength = RootLength;
    AttributeAddress->Resident.ValueOffset = ValueOffset;

    // Set the name
    RtlCopyMemory((PCHAR)((ULONG_PTR)AttributeAddress + NameOffset), Name, NameLength * sizeof(WCHAR));

    // Copy the index root attribute
    RtlCopyMemory((PCHAR)((ULONG_PTR)AttributeAddress + ValueOffset), NewIndexRoot, RootLength);

    // move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Non-resident $ATTRIBUTE_LIST support (Phase A.2)
 *
 * A resident $ATTRIBUTE_LIST caps the number of spilled attributes (~22 hard
 * links in a 1KB record).  Real NTFS lets the list value itself go
 * non-resident: the base record then carries a normal non-resident attribute
 * header (LowestVCN 0, mapping pairs, AllocatedSize/DataSize) whose clusters
 * hold the entry array.  The read side (InternalReadNonResidentAttributes,
 * chkscan.c's chk_read_attr_value) already consumes that layout through the
 * generic runlist machinery; the helpers below produce it.
 *
 * The $ATTRIBUTE_LIST always stays in the base record and never describes
 * itself with a list entry, matching both Windows and our reader.
 * ========================================================================= */

typedef struct
{
    ULONGLONG Lcn;
    ULONGLONG Count;
} NTFS_ATTRLIST_RUN;

/* Upper bound on how fragmented one grow/convert step may be.  The list is a
 * few clusters at most, so hitting this means the volume is pathologically
 * fragmented - we fail cleanly (STATUS_DISK_FULL) rather than overrun. */
#define NTFS_ATTRLIST_MAX_NEW_RUNS 8

/* Locate the $ATTRIBUTE_LIST attribute slot in a file record, or NULL. */
static
PNTFS_ATTR_RECORD
NtfsFindAttributeListSlot(PFILE_RECORD_HEADER FileRecord)
{
    PNTFS_ATTR_RECORD Attr =
        (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

    while (Attr->Type != AttributeEnd &&
           (ULONG_PTR)Attr < (ULONG_PTR)FileRecord + FileRecord->BytesInUse)
    {
        if (Attr->Type == AttributeAttributeList)
            return Attr;
        if (Attr->Length == 0)
            break;
        Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
    }

    return NULL;
}

/* Carve a zeroed SlotSize-byte hole for a NEW $ATTRIBUTE_LIST attribute at
 * its type-sorted position (after $STANDARD_INFORMATION, before the
 * $FILE_NAMEs - Windows and ntfs-3g require ascending type order in a file
 * record).  The caller must have verified SlotSize fits the record; there
 * must be no existing $ATTRIBUTE_LIST.  Updates the end markers/BytesInUse
 * and returns the slot. */
static
PNTFS_ATTR_RECORD
NtfsCarveAttributeListSlot(PNTFS_VCB Vcb,
                           PFILE_RECORD_HEADER BaseFileRecord,
                           ULONG SlotSize)
{
    PNTFS_ATTR_RECORD Attr =
        (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->AttributeOffset);

    while (Attr->Type != AttributeEnd &&
           Attr->Type <= AttributeAttributeList &&
           Attr->Length > 0 &&
           (ULONG_PTR)Attr < (ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse)
    {
        Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
    }

    if (Attr->Type != AttributeEnd)
    {
        PNTFS_ATTR_RECORD MovedFinal;
        ULONG AttrOffset = (ULONG)((ULONG_PTR)Attr - (ULONG_PTR)BaseFileRecord);

        MovedFinal = MoveAttributes(Vcb, Attr, AttrOffset, (ULONG_PTR)Attr + SlotSize);
        SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
    }
    else
    {
        SetFileRecordEnd(BaseFileRecord,
                         (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + SlotSize),
                         FILE_RECORD_END);
    }

    RtlZeroMemory(Attr, SlotSize);
    return Attr;
}

/* Clear a run of clusters in the volume $Bitmap (reverse of
 * NtfsAllocateClusters; unlike FreeClusters this takes a raw LCN range and
 * doesn't need an attribute context). */
NTSTATUS
NtfsFreeClusterRange(PNTFS_VCB Vcb,
                     ULONGLONG FirstCluster,
                     ULONGLONG ClusterCount)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    ULONGLONG BitmapDataSize;
    PUCHAR BitmapData = NULL;
    RTL_BITMAP Bitmap;
    ULONG LengthWritten;
    BOOLEAN BitmapLockHeld;

    DPRINT("NtfsFreeClusterRange(%p, %I64u, %I64u)\n", Vcb, FirstCluster, ClusterCount);

    if (ClusterCount == 0)
        return STATUS_SUCCESS;

    /* Same bitmap R/M/W race as NtfsAllocateClusters: hold BitmapResource
     * exclusive across the whole read-modify-write. */
    BitmapLockHeld = ExAcquireResourceExclusiveLite(&Vcb->BitmapResource, TRUE);

    BitmapRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        if (BitmapLockHeld)
            ExReleaseResourceLite(&Vcb->BitmapResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = FindAttribute(Vcb, BitmapRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    BitmapDataSize = AttributeDataLength(DataContext->pRecord);
    BitmapDataSize = min(BitmapDataSize, 0xffffffff);
    BitmapData = ExAllocatePoolWithTag(NonPagedPool,
                                       ROUND_UP(BitmapDataSize, Vcb->NtfsInfo.BytesPerSector),
                                       TAG_NTFS);
    if (BitmapData == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    ReadAttribute(Vcb, DataContext, 0, (PCHAR)BitmapData, (ULONG)BitmapDataSize);

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, Vcb->NtfsInfo.ClusterCount);
    RtlClearBits(&Bitmap, (ULONG)FirstCluster, (ULONG)ClusterCount);

    Status = WriteAttribute(Vcb, DataContext, 0, BitmapData, (ULONG)BitmapDataSize,
                            &LengthWritten, BitmapRecord);

Cleanup:
    if (BitmapData)
        ExFreePoolWithTag(BitmapData, TAG_NTFS);
    if (DataContext)
        ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);
    if (BitmapLockHeld)
        ExReleaseResourceLite(&Vcb->BitmapResource);

    /* Cluster state changed; invalidate the cached free-clusters count. */
    InterlockedExchange(&Vcb->CachedFreeClustersValid, 0);

    return Status;
}

/* Sequential IO against a non-resident $ATTRIBUTE_LIST's clusters, from list
 * offset 0.  Goes through the volume stream cache to stay coherent with the
 * reader path (ReadAttribute/InternalReadNonResidentAttributes). */
static
NTSTATUS
NtfsAttrListDataIo(PNTFS_VCB Vcb,
                   PNTFS_ATTR_RECORD ListAttr,
                   PUCHAR Buffer,
                   ULONG Length,
                   BOOLEAN Write)
{
    PUCHAR Run;
    LONGLONG RunDelta;
    ULONGLONG RunLength;
    LONGLONG Lcn = 0;
    ULONG Remaining = Length;
    NTSTATUS Status;

    ASSERT(ListAttr->IsNonResident);

    Run = (PUCHAR)ListAttr + ListAttr->NonResident.MappingPairsOffset;

    while (Remaining > 0)
    {
        ULONGLONG RunBytes;
        ULONG Chunk;

        if (*Run == 0)
        {
            /* Ran out of mapping pairs before the end of the data. */
            DPRINT1("NtfsAttrListDataIo: runlist shorter than %u bytes\n", Length);
            return STATUS_FILE_CORRUPT_ERROR;
        }

        Run = DecodeRun(Run, &RunDelta, &RunLength);
        if (RunDelta == -1 || RunLength == 0)
        {
            /* We never create sparse $ATTRIBUTE_LIST runs. */
            DPRINT1("NtfsAttrListDataIo: unexpected sparse/empty run\n");
            return STATUS_FILE_CORRUPT_ERROR;
        }
        Lcn += RunDelta;

        RunBytes = RunLength * Vcb->NtfsInfo.BytesPerCluster;
        Chunk = (ULONG)min((ULONGLONG)Remaining, RunBytes);

        if (Write)
            Status = NtfsWriteDiskCached(Vcb, Lcn * Vcb->NtfsInfo.BytesPerCluster, Chunk, Buffer);
        else
            Status = NtfsReadDiskCached(Vcb, Lcn * Vcb->NtfsInfo.BytesPerCluster, Chunk, Buffer);
        if (!NT_SUCCESS(Status))
            return Status;

        Buffer += Chunk;
        Remaining -= Chunk;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsReadAttributeListValue
*
* Reads the complete value of a (resident or non-resident) $ATTRIBUTE_LIST
* attribute into a pool buffer (TAG_NTFS; caller frees).
*/
NTSTATUS
NtfsReadAttributeListValue(PNTFS_VCB Vcb,
                           PNTFS_ATTR_RECORD ListAttr,
                           PUCHAR *Value,
                           PULONG ValueLength)
{
    PUCHAR Buffer;
    ULONG Length;
    NTSTATUS Status;

    if (!ListAttr->IsNonResident)
    {
        Length = ListAttr->Resident.ValueLength;
        Buffer = ExAllocatePoolWithTag(NonPagedPool, Length ? Length : 1, TAG_NTFS);
        if (!Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(Buffer, (PUCHAR)ListAttr + ListAttr->Resident.ValueOffset, Length);
    }
    else
    {
        if ((ULONGLONG)ListAttr->NonResident.DataSize > 0xFFFFFFFF)
            return STATUS_FILE_CORRUPT_ERROR;
        Length = (ULONG)ListAttr->NonResident.DataSize;
        Buffer = ExAllocatePoolWithTag(NonPagedPool, Length ? Length : 1, TAG_NTFS);
        if (!Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = NtfsAttrListDataIo(Vcb, ListAttr, Buffer, Length, FALSE);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Buffer, TAG_NTFS);
            return Status;
        }
    }

    *Value = Buffer;
    *ValueLength = Length;
    return STATUS_SUCCESS;
}

/* Allocate ClustersNeeded clusters as up to MaxRuns runs.  All-or-nothing:
 * on failure any partially-allocated runs are freed again. */
static
NTSTATUS
NtfsAllocateClusterRuns(PNTFS_VCB Vcb,
                        ULONG ClustersNeeded,
                        ULONG LcnHint,
                        NTFS_ATTRLIST_RUN *Runs,
                        ULONG MaxRuns,
                        PULONG RunCount)
{
    ULONG Got = 0;
    ULONG Count = 0;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    while (Got < ClustersNeeded)
    {
        ULONG First = 0;
        ULONG Assigned = 0;

        if (Count >= MaxRuns)
        {
            Status = STATUS_DISK_FULL;
            break;
        }

        Status = NtfsAllocateClusters(Vcb, LcnHint, ClustersNeeded - Got, &First, &Assigned);
        if (!NT_SUCCESS(Status))
            break;
        if (Assigned == 0)
        {
            Status = STATUS_DISK_FULL;
            break;
        }

        Runs[Count].Lcn = First;
        Runs[Count].Count = Assigned;
        Count++;
        Got += Assigned;
        LcnHint = First + Assigned;
    }

    if (Got < ClustersNeeded)
    {
        for (i = 0; i < Count; i++)
            NtfsFreeClusterRange(Vcb, Runs[i].Lcn, Runs[i].Count);
        return NT_SUCCESS(Status) ? STATUS_DISK_FULL : Status;
    }

    *RunCount = Count;
    return STATUS_SUCCESS;
}

/* Re-encode Mcb into ListAttr's mapping pairs, resizing the attribute slot
 * (and moving trailing attributes) in the base record as needed.  Updates
 * ListAttr->Length and the record end markers; size fields are the caller's
 * job.  STATUS_DISK_FULL if the encoded runs no longer fit the record. */
static
NTSTATUS
NtfsAttrListWriteMappingPairs(PNTFS_VCB Vcb,
                              PFILE_RECORD_HEADER BaseFileRecord,
                              PNTFS_ATTR_RECORD ListAttr,
                              PLARGE_MCB Mcb)
{
    PUCHAR RunBuffer;
    ULONG RunBufferSize = 0;
    ULONG OldAttrLength = ListAttr->Length;
    ULONG NewAttrLength;
    ULONG ListOffset = (ULONG)((ULONG_PTR)ListAttr - (ULONG_PTR)BaseFileRecord);
    NTSTATUS Status;

    RunBuffer = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (!RunBuffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ConvertLargeMCBToDataRuns(Mcb, RunBuffer, Vcb->NtfsInfo.BytesPerFileRecord, &RunBufferSize);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(RunBuffer, TAG_NTFS);
        return Status;
    }

    NewAttrLength = ALIGN_UP_BY(ListAttr->NonResident.MappingPairsOffset + RunBufferSize,
                                ATTR_RECORD_ALIGNMENT);

    if (BaseFileRecord->BytesInUse - OldAttrLength + NewAttrLength > Vcb->NtfsInfo.BytesPerFileRecord)
    {
        /* The grown mapping-pair array no longer fits the base record.  The
         * $ATTRIBUTE_LIST must stay in the base record (Windows format rule),
         * so fail cleanly; the caller rolls back its fresh clusters.  Making
         * room by migrating other base attributes out first is a possible
         * future improvement. */
        DPRINT1("NtfsAttrListWriteMappingPairs: mapping pairs (%u bytes) don't fit base record\n",
                RunBufferSize);
        ExFreePoolWithTag(RunBuffer, TAG_NTFS);
        return STATUS_DISK_FULL;
    }

    if (NewAttrLength != OldAttrLength)
    {
        PNTFS_ATTR_RECORD AfterList = (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + OldAttrLength);

        ListAttr->Length = NewAttrLength;
        if (AfterList->Type != AttributeEnd)
        {
            PNTFS_ATTR_RECORD MovedFinal;
            MovedFinal = MoveAttributes(Vcb, AfterList, ListOffset + OldAttrLength,
                                        (ULONG_PTR)ListAttr + NewAttrLength);
            SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
        }
        else
        {
            SetFileRecordEnd(BaseFileRecord,
                             (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + NewAttrLength),
                             FILE_RECORD_END);
        }
    }

    RtlZeroMemory((PUCHAR)ListAttr + ListAttr->NonResident.MappingPairsOffset,
                  NewAttrLength - ListAttr->NonResident.MappingPairsOffset);
    RtlCopyMemory((PUCHAR)ListAttr + ListAttr->NonResident.MappingPairsOffset,
                  RunBuffer, RunBufferSize);

    ExFreePoolWithTag(RunBuffer, TAG_NTFS);
    return STATUS_SUCCESS;
}

/**
* @name NtfsMaterializeNonResidentList
*
* Writes Value to freshly-allocated clusters and puts a proper non-resident
* $ATTRIBUTE_LIST header (LowestVCN 0, mapping pairs, AllocatedSize /
* DataSize / InitializedSize) into the base record - either replacing the
* existing (resident) $ATTRIBUTE_LIST slot in place, or carving a new slot at
* the type-sorted position.  The base record is modified in memory; the
* caller persists it.  All-or-nothing: on failure no clusters stay allocated.
*/
static
NTSTATUS
NtfsMaterializeNonResidentList(PNTFS_VCB Vcb,
                               PFILE_RECORD_HEADER BaseFileRecord,
                               PUCHAR Value,
                               ULONG ValueLength,
                               BOOLEAN ReplaceExisting)
{
    ULONG NonResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, NonResident.CompressedSize);
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    PNTFS_ATTR_RECORD ListAttr = NULL;
    PNTFS_ATTR_RECORD TempAttr = NULL;
    PUCHAR Data = NULL;
    NTFS_ATTRLIST_RUN Runs[NTFS_ATTRLIST_MAX_NEW_RUNS];
    ULONG RunCount = 0;
    ULONG ClusterCount;
    ULONG PaddedLength;
    ULONG OldAttrLength;
    ULONG NewAttrLength;
    ULONG NewBytesInUse;
    ULONG RunBufferSize = 0;
    LARGE_MCB Mcb;
    BOOLEAN McbInitialized = FALSE;
    ULONGLONG NextVcn = 0;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    if (ValueLength == 0)
        return STATUS_INVALID_PARAMETER;

    if (ReplaceExisting)
    {
        ListAttr = NtfsFindAttributeListSlot(BaseFileRecord);
        if (ListAttr == NULL)
            return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    ClusterCount = (ValueLength + BytesPerCluster - 1) / BytesPerCluster;
    PaddedLength = ClusterCount * BytesPerCluster;

    /* Snapshot the value, zero-padded to whole clusters. */
    Data = ExAllocatePoolWithTag(NonPagedPool, PaddedLength, TAG_NTFS);
    if (!Data)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Data, PaddedLength);
    RtlCopyMemory(Data, Value, ValueLength);

    Status = NtfsAllocateClusterRuns(Vcb, ClusterCount, 0, Runs,
                                     NTFS_ATTRLIST_MAX_NEW_RUNS, &RunCount);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Data, TAG_NTFS);
        return Status;
    }

    TempAttr = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (!TempAttr)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }
    RtlZeroMemory(TempAttr, Vcb->NtfsInfo.BytesPerFileRecord);

    _SEH2_TRY
    {
        FsRtlInitializeLargeMcb(&Mcb, NonPagedPool);
        McbInitialized = TRUE;
        for (i = 0; i < RunCount; i++)
        {
            if (!FsRtlAddLargeMcbEntry(&Mcb, NextVcn, Runs[i].Lcn, Runs[i].Count))
                ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
            NextVcn += Runs[i].Count;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        goto Fail;

    Status = ConvertLargeMCBToDataRuns(&Mcb,
                                       (PUCHAR)TempAttr + NonResidentHeaderLength,
                                       Vcb->NtfsInfo.BytesPerFileRecord - NonResidentHeaderLength,
                                       &RunBufferSize);
    if (!NT_SUCCESS(Status))
        goto Fail;

    NewAttrLength = ALIGN_UP_BY(NonResidentHeaderLength + RunBufferSize, ATTR_RECORD_ALIGNMENT);
    OldAttrLength = ReplaceExisting ? ListAttr->Length : 0;

    NewBytesInUse = BaseFileRecord->BytesInUse - OldAttrLength + NewAttrLength;
    if (NewBytesInUse > Vcb->NtfsInfo.BytesPerFileRecord)
    {
        DPRINT1("NtfsMaterializeNonResidentList: no room for non-resident header\n");
        Status = STATUS_DISK_FULL;
        goto Fail;
    }

    TempAttr->Type = AttributeAttributeList;
    TempAttr->Length = NewAttrLength;
    TempAttr->IsNonResident = 1;
    TempAttr->NameLength = 0;
    TempAttr->NameOffset = (USHORT)NonResidentHeaderLength;
    TempAttr->Flags = 0;
    TempAttr->Instance = ReplaceExisting ? ListAttr->Instance
                                         : BaseFileRecord->NextAttributeNumber++;
    TempAttr->NonResident.LowestVCN = 0;
    TempAttr->NonResident.HighestVCN = ClusterCount - 1;
    TempAttr->NonResident.MappingPairsOffset = (USHORT)NonResidentHeaderLength;
    TempAttr->NonResident.CompressionUnit = 0;
    TempAttr->NonResident.AllocatedSize = (LONGLONG)PaddedLength;
    TempAttr->NonResident.DataSize = ValueLength;
    TempAttr->NonResident.InitializedSize = ValueLength;

    /* Write the list content to its clusters BEFORE publishing the new
     * header, so no reader can follow mapping pairs to unwritten disk. */
    {
        PUCHAR Chunk = Data;
        for (i = 0; i < RunCount; i++)
        {
            Status = NtfsWriteDiskCached(Vcb,
                                         (LONGLONG)Runs[i].Lcn * BytesPerCluster,
                                         (ULONG)(Runs[i].Count * BytesPerCluster),
                                         Chunk);
            if (!NT_SUCCESS(Status))
                goto Fail;
            Chunk += Runs[i].Count * BytesPerCluster;
        }
    }

    if (!ReplaceExisting)
    {
        /* Carve a fresh, type-sorted slot for the header. */
        ListAttr = NtfsCarveAttributeListSlot(Vcb, BaseFileRecord, NewAttrLength);
        RtlCopyMemory(ListAttr, TempAttr, NewAttrLength);
    }
    else if (NewAttrLength != OldAttrLength)
    {
        /* Splice the new header into the existing slot. */
        ULONG ListOffset = (ULONG)((ULONG_PTR)ListAttr - (ULONG_PTR)BaseFileRecord);
        PNTFS_ATTR_RECORD AfterList = (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + OldAttrLength);

        if (AfterList->Type != AttributeEnd)
        {
            PNTFS_ATTR_RECORD MovedFinal;
            MovedFinal = MoveAttributes(Vcb, AfterList, ListOffset + OldAttrLength,
                                        (ULONG_PTR)ListAttr + NewAttrLength);
            RtlCopyMemory(ListAttr, TempAttr, NewAttrLength);
            SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
        }
        else
        {
            RtlCopyMemory(ListAttr, TempAttr, NewAttrLength);
            SetFileRecordEnd(BaseFileRecord,
                             (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + NewAttrLength),
                             FILE_RECORD_END);
        }
    }
    else
    {
        RtlCopyMemory(ListAttr, TempAttr, NewAttrLength);
    }

    FsRtlUninitializeLargeMcb(&Mcb);
    ExFreePoolWithTag(TempAttr, TAG_NTFS);
    ExFreePoolWithTag(Data, TAG_NTFS);
    return STATUS_SUCCESS;

Fail:
    for (i = 0; i < RunCount; i++)
        NtfsFreeClusterRange(Vcb, Runs[i].Lcn, Runs[i].Count);
    if (McbInitialized)
        FsRtlUninitializeLargeMcb(&Mcb);
    if (TempAttr)
        ExFreePoolWithTag(TempAttr, TAG_NTFS);
    if (Data)
        ExFreePoolWithTag(Data, TAG_NTFS);
    return Status;
}

/**
* @name NtfsConvertAttributeListToNonResident
*
* Converts a resident $ATTRIBUTE_LIST attribute to non-resident: allocates
* clusters for the current value (rounded up to a whole cluster), writes the
* value there, and replaces the resident header in the base record with a
* proper non-resident one.  The base record is persisted on success.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_OBJECT_NAME_NOT_FOUND if the record carries no $ATTRIBUTE_LIST.
* STATUS_INVALID_PARAMETER if the list is already non-resident (or empty).
* STATUS_DISK_FULL / allocation / IO errors otherwise (no clusters leaked).
*/
static
NTSTATUS
NtfsConvertAttributeListToNonResident(PNTFS_VCB Vcb,
                                      PFILE_RECORD_HEADER BaseFileRecord)
{
    PNTFS_ATTR_RECORD ListAttr;
    NTSTATUS Status;

    ListAttr = NtfsFindAttributeListSlot(BaseFileRecord);
    if (ListAttr == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (ListAttr->IsNonResident)
        return STATUS_INVALID_PARAMETER;    /* nothing to convert */
    if (ListAttr->Resident.ValueLength == 0)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsMaterializeNonResidentList(Vcb, BaseFileRecord,
                                            (PUCHAR)ListAttr + ListAttr->Resident.ValueOffset,
                                            ListAttr->Resident.ValueLength,
                                            TRUE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UpdateFileRecord(Vcb, BaseFileRecord->MFTRecordNumber, BaseFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtfsConvertAttributeListToNonResident: UpdateFileRecord failed 0x%x\n",
                (unsigned)Status);
        return Status;
    }

    DPRINT("NtfsConvertAttributeListToNonResident: base MFT %I64u list -> non-resident\n",
           BaseFileRecord->MFTRecordNumber);
    return STATUS_SUCCESS;
}

/* Grow a non-resident $ATTRIBUTE_LIST's cluster allocation so NewDataSize
 * bytes fit.  Updates the mapping pairs, HighestVCN and AllocatedSize in the
 * base record (in memory; the caller persists).  All-or-nothing: on failure
 * no clusters stay allocated. */
static
NTSTATUS
NtfsNonResidentListEnsureSpace(PNTFS_VCB Vcb,
                               PFILE_RECORD_HEADER BaseFileRecord,
                               PNTFS_ATTR_RECORD ListAttr,
                               ULONG NewDataSize)
{
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONG NeededClusters = (NewDataSize + BytesPerCluster - 1) / BytesPerCluster;
    ULONG HaveClusters;
    NTFS_ATTRLIST_RUN Runs[NTFS_ATTRLIST_MAX_NEW_RUNS];
    ULONG RunCount = 0;
    LARGE_MCB Mcb;
    ULONGLONG NextVbn = 0;
    ULONG LcnHint = 0;
    ULONG i;
    NTSTATUS Status;

    ASSERT(ListAttr->IsNonResident);

    HaveClusters = (ULONG)((ULONGLONG)ListAttr->NonResident.AllocatedSize / BytesPerCluster);
    if (NeededClusters <= HaveClusters)
        return STATUS_SUCCESS;

    Status = ConvertDataRunsToLargeMCB((PUCHAR)ListAttr + ListAttr->NonResident.MappingPairsOffset,
                                       &Mcb, &NextVbn);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Prefer clusters right after the current last run. */
    {
        LONGLONG Vbn, Lbn, Count;
        LONG LastRun = (LONG)FsRtlNumberOfRunsInLargeMcb(&Mcb) - 1;

        if (LastRun >= 0 && FsRtlGetNextLargeMcbEntry(&Mcb, LastRun, &Vbn, &Lbn, &Count))
            LcnHint = (ULONG)(Lbn + Count);
    }

    Status = NtfsAllocateClusterRuns(Vcb, NeededClusters - HaveClusters, LcnHint,
                                     Runs, NTFS_ATTRLIST_MAX_NEW_RUNS, &RunCount);
    if (!NT_SUCCESS(Status))
    {
        FsRtlUninitializeLargeMcb(&Mcb);
        return Status;
    }

    _SEH2_TRY
    {
        for (i = 0; i < RunCount; i++)
        {
            if (!FsRtlAddLargeMcbEntry(&Mcb, NextVbn, Runs[i].Lcn, Runs[i].Count))
                ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
            NextVbn += Runs[i].Count;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (NT_SUCCESS(Status))
        Status = NtfsAttrListWriteMappingPairs(Vcb, BaseFileRecord, ListAttr, &Mcb);

    FsRtlUninitializeLargeMcb(&Mcb);

    if (!NT_SUCCESS(Status))
    {
        for (i = 0; i < RunCount; i++)
            NtfsFreeClusterRange(Vcb, Runs[i].Lcn, Runs[i].Count);
        return Status;
    }

    ListAttr->NonResident.HighestVCN = NeededClusters - 1;
    ListAttr->NonResident.AllocatedSize = (LONGLONG)NeededClusters * BytesPerCluster;
    return STATUS_SUCCESS;
}

/* Sorted insertion offset for a new entry within a list value (ordering:
 * type, then attribute name, then starting VCN - what Windows maintains;
 * stable: equal keys append after the existing ones). */
static
ULONG
NtfsListEntryInsertOffset(PUCHAR Value,
                          ULONG ValueLength,
                          ULONG AttrType,
                          PCWSTR Name,
                          UCHAR NameLength,
                          ULONGLONG StartingVCN)
{
    ULONG InsertOffset = ValueLength;
    ULONG Pos = 0;

    while (Pos + 0x1A <= ValueLength)
    {
        PNTFS_ATTRIBUTE_LIST_ITEM Item = (PNTFS_ATTRIBUTE_LIST_ITEM)(Value + Pos);
        LONG Cmp;

        if (Item->Length == 0)
            break;

        if (Item->Type > AttrType)
        {
            Cmp = 1;
        }
        else if (Item->Type < AttrType)
        {
            Cmp = -1;
        }
        else
        {
            UNICODE_STRING ItemName, NewName;

            ItemName.Buffer = (PWSTR)((PUCHAR)Item + Item->NameOffset);
            ItemName.Length = ItemName.MaximumLength = (USHORT)(Item->NameLength * sizeof(WCHAR));
            NewName.Buffer = (PWSTR)Name;
            NewName.Length = NewName.MaximumLength = (USHORT)(NameLength * sizeof(WCHAR));
            Cmp = RtlCompareUnicodeString(&ItemName, &NewName, TRUE);
            if (Cmp == 0)
                Cmp = (Item->StartingVCN > StartingVCN) ? 1 : -1;
        }

        if (Cmp > 0)
        {
            InsertOffset = Pos;
            break;
        }
        Pos += Item->Length;
    }

    return InsertOffset;
}

/* Fill one NTFS_ATTRIBUTE_LIST_ITEM (Slot must hold ItemSize zero-fillable
 * bytes). */
static
VOID
NtfsFillListEntry(PNTFS_ATTRIBUTE_LIST_ITEM NewItem,
                  ULONG ItemSize,
                  ULONG AttrType,
                  PCWSTR Name,
                  UCHAR NameLength,
                  ULONGLONG StartingVCN,
                  ULONGLONG MftRef,
                  USHORT Instance)
{
    RtlZeroMemory(NewItem, ItemSize);
    NewItem->Type = AttrType;
    NewItem->Length = (USHORT)ItemSize;
    NewItem->NameLength = NameLength;
    NewItem->NameOffset = 0x1A;
    NewItem->StartingVCN = StartingVCN;
    NewItem->MFTIndex = MftRef;
    NewItem->Instance = Instance;
    if (NameLength > 0)
        RtlCopyMemory((PUCHAR)NewItem + 0x1A, Name, NameLength * sizeof(WCHAR));
}

/* Insert one entry into a non-resident $ATTRIBUTE_LIST at its sorted position
 * (by type, then attribute name, then starting VCN - the on-disk ordering
 * Windows maintains).  The caller must have grown the allocation first
 * (NtfsNonResidentListEnsureSpace); the base record's DataSize is updated in
 * memory and persisted by the caller. */
static
NTSTATUS
NtfsNonResidentListAddEntry(PNTFS_VCB Vcb,
                            PFILE_RECORD_HEADER BaseFileRecord,
                            PNTFS_ATTR_RECORD ListAttr,
                            ULONG AttrType,
                            PCWSTR Name,
                            UCHAR NameLength,
                            ULONGLONG StartingVCN,
                            ULONGLONG MftRef,
                            USHORT Instance)
{
    ULONG ListEntryFixedSize = 0x1A;    /* NTFS_ATTRIBUTE_LIST_ITEM without trailing name */
    ULONG ItemSize = ALIGN_UP_BY(ListEntryFixedSize + NameLength * sizeof(WCHAR), 8);
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONG OldDataSize = (ULONG)ListAttr->NonResident.DataSize;
    ULONG NewDataSize = OldDataSize + ItemSize;
    ULONG PaddedSize = ROUND_UP(NewDataSize, BytesPerCluster);
    ULONG InsertOffset;
    PUCHAR Buffer;
    PNTFS_ATTRIBUTE_LIST_ITEM NewItem;
    NTSTATUS Status;

    ASSERT(ListAttr->IsNonResident);

    if ((ULONGLONG)NewDataSize > (ULONGLONG)ListAttr->NonResident.AllocatedSize)
    {
        DPRINT1("NtfsNonResidentListAddEntry: caller did not ensure space (%u > %I64u)\n",
                NewDataSize, ListAttr->NonResident.AllocatedSize);
        return STATUS_DISK_FULL;
    }

    Buffer = ExAllocatePoolWithTag(NonPagedPool, PaddedSize, TAG_NTFS);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Buffer, PaddedSize);

    if (OldDataSize != 0)
    {
        Status = NtfsAttrListDataIo(Vcb, ListAttr, Buffer, ROUND_UP(OldDataSize, BytesPerCluster), FALSE);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Buffer, TAG_NTFS);
            return Status;
        }
        /* Clear the cluster-padding tail so stale disk bytes never look like
         * an extra entry. */
        RtlZeroMemory(Buffer + OldDataSize, PaddedSize - OldDataSize);
    }

    /* Find the sorted insertion point (stable: equal keys append after). */
    InsertOffset = NtfsListEntryInsertOffset(Buffer, OldDataSize,
                                             AttrType, Name, NameLength, StartingVCN);

    RtlMoveMemory(Buffer + InsertOffset + ItemSize,
                  Buffer + InsertOffset,
                  OldDataSize - InsertOffset);

    NewItem = (PNTFS_ATTRIBUTE_LIST_ITEM)(Buffer + InsertOffset);
    NtfsFillListEntry(NewItem, ItemSize, AttrType, Name, NameLength,
                      StartingVCN, MftRef, Instance);

    Status = NtfsAttrListDataIo(Vcb, ListAttr, Buffer, PaddedSize, TRUE);
    ExFreePoolWithTag(Buffer, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return Status;

    ListAttr->NonResident.DataSize = NewDataSize;
    ListAttr->NonResident.InitializedSize = NewDataSize;
    return STATUS_SUCCESS;
}

/**
* @name NtfsAttributeListAddEntry
*
* Adds one entry to an existing $ATTRIBUTE_LIST, resident or non-resident,
* at its sorted position.  Windows requires the list to describe EVERY
* attribute of the file, so the generic attribute add path calls this too.
* The base record is modified in memory (the non-resident data write goes to
* disk immediately); the caller persists the record.
*
* @return STATUS_DISK_FULL when a resident list cannot grow inside the record
* (callers convert the list to non-resident and retry) or when a non-resident
* list's mapping pairs no longer fit.
*/
static
NTSTATUS
NtfsAttributeListAddEntry(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER BaseFileRecord,
                          PNTFS_ATTR_RECORD ListAttr,
                          ULONG AttrType,
                          PCWSTR Name,
                          UCHAR NameLength,
                          ULONGLONG StartingVCN,
                          ULONGLONG MftRef,
                          USHORT Instance)
{
    ULONG ItemSize = ALIGN_UP_BY(0x1A + NameLength * sizeof(WCHAR), 8);
    ULONG OldAttrLen;
    ULONG OldValueLen;
    ULONG NewValueLen;
    ULONG NewAttrLen;
    ULONG GrowBy;
    ULONG InsertOffset;
    PUCHAR Content;
    NTSTATUS Status;

    if (ListAttr->IsNonResident)
    {
        Status = NtfsNonResidentListEnsureSpace(Vcb, BaseFileRecord, ListAttr,
                                                (ULONG)ListAttr->NonResident.DataSize + ItemSize);
        if (!NT_SUCCESS(Status))
            return Status;
        return NtfsNonResidentListAddEntry(Vcb, BaseFileRecord, ListAttr,
                                           AttrType, Name, NameLength,
                                           StartingVCN, MftRef, Instance);
    }

    OldAttrLen = ListAttr->Length;
    OldValueLen = ListAttr->Resident.ValueLength;
    NewValueLen = OldValueLen + ItemSize;
    NewAttrLen = ALIGN_UP_BY(ListAttr->Resident.ValueOffset + NewValueLen, ATTR_RECORD_ALIGNMENT);
    GrowBy = NewAttrLen - OldAttrLen;

    if (BaseFileRecord->BytesInUse + GrowBy > Vcb->NtfsInfo.BytesPerFileRecord)
        return STATUS_DISK_FULL;

    /* Grow the attribute slot (moving trailing attributes right). */
    if (GrowBy != 0)
    {
        PNTFS_ATTR_RECORD AfterList = (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + OldAttrLen);
        ULONG ListOffset = (ULONG)((ULONG_PTR)ListAttr - (ULONG_PTR)BaseFileRecord);

        if (AfterList->Type != AttributeEnd)
        {
            PNTFS_ATTR_RECORD MovedFinal;
            MovedFinal = MoveAttributes(Vcb, AfterList, ListOffset + OldAttrLen,
                                        (ULONG_PTR)AfterList + GrowBy);
            SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
        }
        else
        {
            SetFileRecordEnd(BaseFileRecord,
                             (PNTFS_ATTR_RECORD)((ULONG_PTR)AfterList + GrowBy),
                             FILE_RECORD_END);
        }
    }

    ListAttr->Length = NewAttrLen;
    Content = (PUCHAR)ListAttr + ListAttr->Resident.ValueOffset;

    InsertOffset = NtfsListEntryInsertOffset(Content, OldValueLen,
                                             AttrType, Name, NameLength, StartingVCN);
    RtlMoveMemory(Content + InsertOffset + ItemSize,
                  Content + InsertOffset,
                  OldValueLen - InsertOffset);
    NtfsFillListEntry((PNTFS_ATTRIBUTE_LIST_ITEM)(Content + InsertOffset), ItemSize,
                      AttrType, Name, NameLength, StartingVCN, MftRef, Instance);

    ListAttr->Resident.ValueLength = NewValueLen;
    return STATUS_SUCCESS;
}

/**
* @name NtfsAttributeListRemoveEntry
*
* Removes the entry matching (type, name, instance, MFT reference) from a
* resident or non-resident $ATTRIBUTE_LIST.  A missing entry is not an error
* (STATUS_SUCCESS) so callers can use this best-effort.  When the last entry
* goes, the whole $ATTRIBUTE_LIST attribute is removed (and its clusters
* freed).  The base record is modified in memory; the caller persists it.
*/
static
NTSTATUS
NtfsAttributeListRemoveEntry(PNTFS_VCB Vcb,
                             PFILE_RECORD_HEADER BaseFileRecord,
                             PNTFS_ATTR_RECORD ListAttr,
                             ULONG AttrType,
                             PCWSTR Name,
                             UCHAR NameLength,
                             ULONGLONG MftRef,
                             USHORT Instance)
{
    PUCHAR Value = NULL;
    ULONG ValueLen = 0;
    ULONG Pos = 0;
    ULONG MatchOff = MAXULONG;
    ULONG MatchLen = 0;
    NTSTATUS Status;

    Status = NtfsReadAttributeListValue(Vcb, ListAttr, &Value, &ValueLen);
    if (!NT_SUCCESS(Status))
        return Status;

    while (Pos + 0x1A <= ValueLen)
    {
        PNTFS_ATTRIBUTE_LIST_ITEM Item = (PNTFS_ATTRIBUTE_LIST_ITEM)(Value + Pos);

        if (Item->Length == 0)
            break;
        if (Item->Type == AttrType &&
            Item->NameLength == NameLength &&
            Item->Instance == Instance &&
            Item->MFTIndex == MftRef &&
            (NameLength == 0 ||
             RtlCompareMemory((PUCHAR)Item + Item->NameOffset, Name,
                              NameLength * sizeof(WCHAR)) == NameLength * sizeof(WCHAR)))
        {
            MatchOff = Pos;
            MatchLen = Item->Length;
            break;
        }
        Pos += Item->Length;
    }

    if (MatchOff == MAXULONG)
    {
        ExFreePoolWithTag(Value, TAG_NTFS);
        return STATUS_SUCCESS;
    }

    RtlMoveMemory(Value + MatchOff, Value + MatchOff + MatchLen,
                  ValueLen - MatchOff - MatchLen);
    ValueLen -= MatchLen;

    if (ListAttr->IsNonResident)
    {
        Status = NtfsNonResidentListWriteValue(Vcb, BaseFileRecord, ListAttr, Value, ValueLen);
        ExFreePoolWithTag(Value, TAG_NTFS);
        return Status;
    }

    ExFreePoolWithTag(Value, TAG_NTFS);

    if (ValueLen == 0)
    {
        /* Last entry - drop the whole attribute slot. */
        return RemoveResidentAttribute(Vcb, BaseFileRecord, ListAttr);
    }

    /* Shrink the resident value in place. */
    {
        PUCHAR Content = (PUCHAR)ListAttr + ListAttr->Resident.ValueOffset;
        ULONG OldValueLen = ListAttr->Resident.ValueLength;
        ULONG OldAttrLen = ListAttr->Length;
        ULONG NewAttrLen = ALIGN_UP_BY(ListAttr->Resident.ValueOffset + ValueLen, ATTR_RECORD_ALIGNMENT);

        RtlMoveMemory(Content + MatchOff, Content + MatchOff + MatchLen,
                      OldValueLen - MatchOff - MatchLen);
        RtlZeroMemory(Content + ValueLen, MatchLen);
        ListAttr->Resident.ValueLength = ValueLen;

        if (NewAttrLen != OldAttrLen)
        {
            ULONG ListOffset = (ULONG)((ULONG_PTR)ListAttr - (ULONG_PTR)BaseFileRecord);
            PNTFS_ATTR_RECORD AfterList = (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + OldAttrLen);

            ListAttr->Length = NewAttrLen;
            if (AfterList->Type != AttributeEnd)
            {
                PNTFS_ATTR_RECORD MovedFinal;
                MovedFinal = MoveAttributes(Vcb, AfterList, ListOffset + OldAttrLen,
                                            (ULONG_PTR)ListAttr + NewAttrLen);
                SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
            }
            else
            {
                SetFileRecordEnd(BaseFileRecord,
                                 (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + NewAttrLen),
                                 FILE_RECORD_END);
            }
        }
    }

    return STATUS_SUCCESS;
}

/* Emit one list entry per attribute currently in the base chain (except any
 * $ATTRIBUTE_LIST) into Buffer - Windows requires the list to describe ALL
 * of the file's attributes, including the ones that stay in the base record
 * (their entries reference the base itself). */
static
NTSTATUS
NtfsBuildAttributeListValue(PFILE_RECORD_HEADER BaseFileRecord,
                            PUCHAR Buffer,
                            ULONG MaxLength,
                            PULONG OutLength)
{
    PNTFS_ATTR_RECORD Attr =
        (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->AttributeOffset);
    ULONGLONG BaseRef = ((ULONGLONG)BaseFileRecord->SequenceNumber << 48) |
                        BaseFileRecord->MFTRecordNumber;
    ULONG Length = 0;

    while (Attr->Type != AttributeEnd &&
           Attr->Length > 0 &&
           (ULONG_PTR)Attr < (ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse)
    {
        if (Attr->Type != AttributeAttributeList)
        {
            ULONG ItemSize = ALIGN_UP_BY(0x1A + Attr->NameLength * sizeof(WCHAR), 8);

            if (Length + ItemSize > MaxLength)
                return STATUS_BUFFER_TOO_SMALL;

            NtfsFillListEntry((PNTFS_ATTRIBUTE_LIST_ITEM)(Buffer + Length), ItemSize,
                              Attr->Type,
                              (PCWSTR)((PUCHAR)Attr + Attr->NameOffset),
                              Attr->NameLength,
                              Attr->IsNonResident ? Attr->NonResident.LowestVCN : 0,
                              BaseRef,
                              Attr->Instance);
            Length += ItemSize;
        }
        Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
    }

    *OutLength = Length;
    return STATUS_SUCCESS;
}

/**
* @name NtfsCreateAttributeList
*
* Creates a fresh $ATTRIBUTE_LIST for a base record: the value describes
* every attribute currently in the base chain PLUS one entry for the
* attribute that was just spilled to a child record.  Placed resident when
* the record has room, otherwise materialized non-resident directly (the
* header is only ~0x50 bytes).  The base record is modified in memory; the
* caller persists it.
*/
static
NTSTATUS
NtfsCreateAttributeList(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER BaseFileRecord,
                        ULONG SpilledType,
                        PCWSTR SpilledName,
                        UCHAR SpilledNameLength,
                        ULONGLONG SpilledVCN,
                        ULONGLONG SpilledMftRef,
                        USHORT SpilledInstance)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG ValueOffset = ALIGN_UP_BY(ResidentHeaderLength, VALUE_OFFSET_ALIGNMENT);
    ULONG ItemSize = ALIGN_UP_BY(0x1A + SpilledNameLength * sizeof(WCHAR), 8);
    ULONG BufferSize = 2 * Vcb->NtfsInfo.BytesPerFileRecord;
    PUCHAR Value;
    ULONG ValueLength = 0;
    ULONG InsertOffset;
    ULONG ResidentSlotSize;
    NTSTATUS Status;

    Value = ExAllocatePoolWithTag(NonPagedPool, BufferSize, TAG_NTFS);
    if (!Value)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Entries for everything already in the base (the chain is type-sorted,
     * so the emitted entries are too)... */
    Status = NtfsBuildAttributeListValue(BaseFileRecord, Value, BufferSize - ItemSize, &ValueLength);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Value, TAG_NTFS);
        return Status;
    }

    /* ...plus the spilled attribute's entry at its sorted position. */
    InsertOffset = NtfsListEntryInsertOffset(Value, ValueLength,
                                             SpilledType, SpilledName, SpilledNameLength,
                                             SpilledVCN);
    RtlMoveMemory(Value + InsertOffset + ItemSize, Value + InsertOffset,
                  ValueLength - InsertOffset);
    NtfsFillListEntry((PNTFS_ATTRIBUTE_LIST_ITEM)(Value + InsertOffset), ItemSize,
                      SpilledType, SpilledName, SpilledNameLength,
                      SpilledVCN, SpilledMftRef, SpilledInstance);
    ValueLength += ItemSize;

    ResidentSlotSize = ALIGN_UP_BY(ValueOffset + ValueLength, ATTR_RECORD_ALIGNMENT);
    if (BaseFileRecord->BytesInUse + ResidentSlotSize <= Vcb->NtfsInfo.BytesPerFileRecord)
    {
        PNTFS_ATTR_RECORD NewList =
            NtfsCarveAttributeListSlot(Vcb, BaseFileRecord, ResidentSlotSize);

        NewList->Type = AttributeAttributeList;
        NewList->Length = ResidentSlotSize;
        NewList->IsNonResident = 0;
        NewList->NameLength = 0;
        NewList->NameOffset = (USHORT)ResidentHeaderLength;
        NewList->Flags = 0;
        NewList->Instance = BaseFileRecord->NextAttributeNumber++;
        NewList->Resident.ValueLength = ValueLength;
        NewList->Resident.ValueOffset = (USHORT)ValueOffset;
        NewList->Resident.Flags = 0;
        RtlCopyMemory((PUCHAR)NewList + ValueOffset, Value, ValueLength);
        Status = STATUS_SUCCESS;
    }
    else
    {
        /* The complete value doesn't fit resident - create the list
         * non-resident right away (a full record can always take another
         * try after the caller evicts a name to free room for the small
         * non-resident header). */
        Status = NtfsMaterializeNonResidentList(Vcb, BaseFileRecord, Value, ValueLength, FALSE);
    }

    ExFreePoolWithTag(Value, TAG_NTFS);
    return Status;
}

/**
* @name NtfsFreeAttributeListClusters
*
* Releases every cluster owned by a non-resident $ATTRIBUTE_LIST (no-op when
* the record has no list or the list is resident).  Used by the record-delete
* path so the list's allocation cannot leak, and by the last-entry removal.
* The in-memory runlist is neutralized so a repeat call cannot double-free.
*/
NTSTATUS
NtfsFreeAttributeListClusters(PNTFS_VCB Vcb,
                              PFILE_RECORD_HEADER BaseFileRecord)
{
    PNTFS_ATTR_RECORD ListAttr;
    PUCHAR Run;
    LONGLONG RunDelta;
    ULONGLONG RunLength;
    LONGLONG Lcn = 0;
    NTSTATUS Status;

    ListAttr = NtfsFindAttributeListSlot(BaseFileRecord);
    if (ListAttr == NULL || !ListAttr->IsNonResident)
        return STATUS_SUCCESS;

    Run = (PUCHAR)ListAttr + ListAttr->NonResident.MappingPairsOffset;
    while (*Run != 0)
    {
        Run = DecodeRun(Run, &RunDelta, &RunLength);
        if (RunDelta == -1)
            continue;   /* sparse run - nothing allocated (we never create these) */
        Lcn += RunDelta;
        Status = NtfsFreeClusterRange(Vcb, (ULONGLONG)Lcn, RunLength);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    *((PUCHAR)ListAttr + ListAttr->NonResident.MappingPairsOffset) = 0;
    ListAttr->NonResident.AllocatedSize = 0;
    ListAttr->NonResident.DataSize = 0;
    ListAttr->NonResident.InitializedSize = 0;
    ListAttr->NonResident.HighestVCN = 0;
    return STATUS_SUCCESS;
}

/**
* @name NtfsNonResidentListWriteValue
*
* Replaces the content of a non-resident $ATTRIBUTE_LIST with Value (which
* must be no larger than the current DataSize - this is the shrink path used
* when a list entry is removed).  With ValueLength == 0 the attribute is
* removed from the base record entirely and its clusters are freed.  Whole
* clusters past the new tail are released.  The base record is modified in
* memory; the caller persists it via UpdateFileRecord.
*
* Windows converts a list back to resident once it fits the record again; we
* keep it non-resident (consistent, just not minimal) because re-packing the
* base record belongs to the CoalesceAttributeFromList machinery, which does
* not handle the list attribute itself yet.
*/
NTSTATUS
NtfsNonResidentListWriteValue(PNTFS_VCB Vcb,
                              PFILE_RECORD_HEADER BaseFileRecord,
                              PNTFS_ATTR_RECORD ListAttr,
                              PUCHAR Value,
                              ULONG ValueLength)
{
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONG NeededClusters;
    ULONG HaveClusters;
    NTSTATUS Status;

    ASSERT(ListAttr->IsNonResident);
    ASSERT((ULONGLONG)ValueLength <= (ULONGLONG)ListAttr->NonResident.DataSize);

    if (ValueLength == 0)
    {
        /* Last entry gone - free the clusters and drop the whole attribute
         * (RemoveResidentAttribute only slides record memory, so it works
         * for any slot type). */
        Status = NtfsFreeAttributeListClusters(Vcb, BaseFileRecord);
        if (!NT_SUCCESS(Status))
            return Status;
        return RemoveResidentAttribute(Vcb, BaseFileRecord, ListAttr);
    }

    NeededClusters = (ValueLength + BytesPerCluster - 1) / BytesPerCluster;
    HaveClusters = (ULONG)((ULONGLONG)ListAttr->NonResident.AllocatedSize / BytesPerCluster);

    /* Write back the shrunk content (whole clusters, zero-padded tail). */
    {
        ULONG PaddedLength = NeededClusters * BytesPerCluster;
        PUCHAR Padded = ExAllocatePoolWithTag(NonPagedPool, PaddedLength, TAG_NTFS);

        if (!Padded)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(Padded, PaddedLength);
        RtlCopyMemory(Padded, Value, ValueLength);
        Status = NtfsAttrListDataIo(Vcb, ListAttr, Padded, PaddedLength, TRUE);
        ExFreePoolWithTag(Padded, TAG_NTFS);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    ListAttr->NonResident.DataSize = ValueLength;
    ListAttr->NonResident.InitializedSize = ValueLength;

    /* Release whole clusters past the new tail. */
    if (NeededClusters < HaveClusters)
    {
        LARGE_MCB Mcb;
        ULONGLONG NextVbn = 0;
        LONGLONG Vbn, Lbn, Count;
        int RunIdx = 0;

        Status = ConvertDataRunsToLargeMCB((PUCHAR)ListAttr + ListAttr->NonResident.MappingPairsOffset,
                                           &Mcb, &NextVbn);
        if (!NT_SUCCESS(Status))
            return Status;

        while (FsRtlGetNextLargeMcbEntry(&Mcb, RunIdx, &Vbn, &Lbn, &Count))
        {
            if (Vbn + Count > (LONGLONG)NeededClusters)
            {
                LONGLONG FreeFrom = max(Vbn, (LONGLONG)NeededClusters);
                NtfsFreeClusterRange(Vcb,
                                     (ULONGLONG)(Lbn + (FreeFrom - Vbn)),
                                     (ULONGLONG)(Count - (FreeFrom - Vbn)));
            }
            RunIdx++;
        }
        FsRtlTruncateLargeMcb(&Mcb, NeededClusters);

        Status = NtfsAttrListWriteMappingPairs(Vcb, BaseFileRecord, ListAttr, &Mcb);
        FsRtlUninitializeLargeMcb(&Mcb);
        if (!NT_SUCCESS(Status))
            return Status;

        ListAttr->NonResident.HighestVCN = NeededClusters - 1;
        ListAttr->NonResident.AllocatedSize = (LONGLONG)NeededClusters * BytesPerCluster;
    }

    return STATUS_SUCCESS;
}

/**
* @name MigrateAttributeToList
* @implemented
*
* Migrates a non-resident attribute from a base file record to a freshly-allocated
* child file record, replacing the attribute slot in the base record with an
* $ATTRIBUTE_LIST entry that points at the child. This is used by AddRun() when a
* non-resident attribute's mapping pairs grow too large to fit in the base record's
* attribute slot - by relocating the entire attribute (header + mapping pairs) to a
* fresh, mostly-empty child record, we get a full file record's worth of room for
* the mapping pairs to grow into.
*
* The reader path (FindAttribute in mft.c) already understands $ATTRIBUTE_LIST and
* will follow the indirection to the child record, so no reader-side changes are
* needed.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param BaseFileRecord
* Pointer to a complete copy of the base file record containing the attribute being
* migrated. On return this buffer is modified in place: the attribute slot is
* replaced with an $ATTRIBUTE_LIST attribute (or, if the list already existed, a
* new entry is appended). The caller is responsible for writing the modified base
* record back to disk.
*
* @param AttrContext
* Pointer to an NTFS_ATTR_CONTEXT describing the attribute being migrated. On
* successful return: FileMFTIndex is updated to the child's MFT index, and pRecord
* is reallocated to hold a copy of the migrated attribute as it now lives in the
* child record.
*
* @param AttrOffset
* Byte offset of the migrated attribute within BaseFileRecord, before migration.
*
* @param OutChildRecord
* On success, receives a pointer to a newly-allocated file record buffer containing
* the migrated attribute. The caller is responsible for freeing this via
* ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, *OutChildRecord) after it
* has written any further updates and called UpdateFileRecord() with the child's
* MFT index.
*
* @param OutNewAttrOffset
* On success, receives the byte offset of the migrated attribute within the child
* record buffer. Combined with *OutChildRecord this lets the caller continue
* manipulating the attribute (e.g. to write fresh mapping pairs into it).
*
* @return
* STATUS_SUCCESS on success.
* STATUS_INVALID_PARAMETER if AttrContext describes a resident attribute.
* STATUS_INSUFFICIENT_RESOURCES on allocation failure.
* STATUS_NOT_IMPLEMENTED if the base record cannot accommodate the new $ATTRIBUTE_LIST
*   slot - this is the "Phase A.2" follow-up case where we'd need to either spill the
*   attribute list itself or pick a different attribute to migrate.
* Other status codes propagated from AddNewMftEntry / UpdateFileRecord.
*
* @remarks
* This is "Phase A" of $ATTRIBUTE_LIST support - whole-attribute migration only,
* no per-VCN-range splitting, no coalescing back on shrink, no migration of
* $STANDARD_INFORMATION/$FILE_NAME (which are never spilled in canonical NTFS).
*/
NTSTATUS
MigrateAttributeToList(PNTFS_VCB Vcb,
                       PFILE_RECORD_HEADER BaseFileRecord,
                       PNTFS_ATTR_CONTEXT AttrContext,
                       ULONG AttrOffset,
                       PFILE_RECORD_HEADER *OutChildRecord,
                       PULONG OutNewAttrOffset)
{
    NTSTATUS Status;
    PNTFS_ATTR_RECORD AttrInBase;
    PNTFS_ATTR_RECORD AttrInChild;
    PNTFS_ATTR_RECORD ExistingList;
    PNTFS_ATTR_RECORD AttrAfter;
    PFILE_RECORD_HEADER ChildRecord;
    ULONGLONG ChildMftIndex;
    ULONG MigratedAttrLength;
    ULONG ChildAttrOffset;
    USHORT MigratedInstance;
    USHORT MigratedNameLength;
    PWCHAR MigratedNameSrc;
    WCHAR MigratedNameBuf[256];
    BOOLEAN ListExisted;

    DPRINT("MigrateAttributeToList: base MFT=%I64u attr type=0x%x at offset 0x%x len=%u\n",
           BaseFileRecord->MFTRecordNumber,
           ((PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + AttrOffset))->Type,
           AttrOffset,
           ((PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + AttrOffset))->Length);

    *OutChildRecord = NULL;
    *OutNewAttrOffset = 0;

    AttrInBase = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + AttrOffset);
    if (!AttrInBase->IsNonResident)
    {
        DPRINT1("MigrateAttributeToList: refusing to migrate resident attribute\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Don't migrate the very attributes the spec forbids spilling. */
    if (AttrInBase->Type == AttributeStandardInformation ||
        AttrInBase->Type == AttributeFileName ||
        AttrInBase->Type == AttributeAttributeList)
    {
        DPRINT1("MigrateAttributeToList: refusing to migrate type 0x%x\n", AttrInBase->Type);
        return STATUS_INVALID_PARAMETER;
    }

    MigratedAttrLength = AttrInBase->Length;
    MigratedInstance = AttrInBase->Instance;
    MigratedNameLength = AttrInBase->NameLength;

    /* Snapshot the attribute name into the function-scope buffer NOW.  Step 5
     * below compacts the base record by moving trailing attributes left, which
     * overwrites the original name bytes at AttrInBase + NameOffset.  By the
     * time we build the new $ATTRIBUTE_LIST entry in step 6, a pointer into
     * AttrInBase points at unrelated data from a moved attribute. */
    if (MigratedNameLength > 0)
    {
        if (MigratedNameLength > 255)
        {
            DPRINT1("MigrateAttributeToList: name length %u exceeds snapshot buffer\n",
                    MigratedNameLength);
            return STATUS_INVALID_PARAMETER;
        }
        RtlCopyMemory(MigratedNameBuf,
                      (PCHAR)AttrInBase + AttrInBase->NameOffset,
                      MigratedNameLength * sizeof(WCHAR));
        MigratedNameSrc = MigratedNameBuf;
    }
    else
    {
        MigratedNameSrc = NULL;
    }

    /* Step 1: create the child file record buffer (in memory). */
    ChildRecord = NtfsCreateEmptyFileRecord(Vcb);
    if (!ChildRecord)
    {
        DPRINT1("MigrateAttributeToList: NtfsCreateEmptyFileRecord failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Step 2: link child back to base via BaseFileRecord field.
     * The reference encodes (sequence_number << 48) | mft_index. */
    ChildRecord->BaseFileRecord =
        ((ULONGLONG)BaseFileRecord->SequenceNumber << 48) | BaseFileRecord->MFTRecordNumber;

    /* Step 3: copy the migrated attribute into the child record at its
     * AttributeOffset.  The child is fresh and has no other attributes,
     * so the destination is always at AttributeOffset. */
    ChildAttrOffset = ChildRecord->AttributeOffset;
    if (ChildAttrOffset + MigratedAttrLength + 2 * sizeof(ULONG) > Vcb->NtfsInfo.BytesPerFileRecord)
    {
        DPRINT1("MigrateAttributeToList: migrated attribute too large for child record (len=%u)\n",
                MigratedAttrLength);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    AttrInChild = (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildRecord + ChildAttrOffset);
    /* Copy from AttrContext->pRecord (the authoritative in-memory copy) when
     * its length matches, since the in-buffer slot at FileRecord+AttrOffset
     * may not yet reflect updates the caller already made to AttrContext (e.g.
     * AllocatedSize/DataSize/InitializedSize set by SetNonResidentAttributeDataLength
     * before the AddRun call). */
    if (AttrContext->pRecord && AttrContext->pRecord->Length == MigratedAttrLength)
        RtlCopyMemory(AttrInChild, AttrContext->pRecord, MigratedAttrLength);
    else
        RtlCopyMemory(AttrInChild, AttrInBase, MigratedAttrLength);

    /* Make the child's NextAttributeNumber not collide with the migrated instance. */
    if (ChildRecord->NextAttributeNumber <= MigratedInstance)
        ChildRecord->NextAttributeNumber = MigratedInstance + 1;

    /* Mark the end of attributes in the child record. */
    AttrAfter = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttrInChild + MigratedAttrLength);
    SetFileRecordEnd(ChildRecord, AttrAfter, FILE_RECORD_END);

    /* Step 4: allocate the child its own MFT index and write it to disk. */
    Status = AddNewMftEntry(ChildRecord, Vcb, &ChildMftIndex, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MigrateAttributeToList: AddNewMftEntry failed 0x%x\n", (unsigned)Status);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return Status;
    }
    DPRINT("MigrateAttributeToList: type=0x%x migrated to child MFT %I64u\n", AttrInChild->Type, ChildMftIndex);

    /* Record the migration so subsequent AddRun calls on this AttrContext
     * re-target the child record instead of stomping on the base. */
    AttrContext->MigratedToMFTIndex = ChildMftIndex;

    /* Step 5: in the base record, REMOVE the migrated attribute slot.
     * Compact: move any trailing attributes left to fill the hole. */
    {
        PNTFS_ATTR_RECORD NextAfterMigrated =
            (PNTFS_ATTR_RECORD)((ULONG_PTR)AttrInBase + MigratedAttrLength);
        ULONG NextAfterOffset = AttrOffset + MigratedAttrLength;

        if (NextAfterMigrated->Type != AttributeEnd)
        {
            /* Move trailing attributes (and the end marker) left by MigratedAttrLength. */
            PNTFS_ATTR_RECORD MovedFinal;
            MovedFinal = MoveAttributes(Vcb, NextAfterMigrated, NextAfterOffset, (ULONG_PTR)AttrInBase);
            SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
        }
        else
        {
            /* No trailing attributes - just collapse the end markers to where the
             * migrated attribute used to start. */
            SetFileRecordEnd(BaseFileRecord, AttrInBase, FILE_RECORD_END);
        }
    }

    /* Step 6: describe the migrated attribute in the base record's
     * $ATTRIBUTE_LIST.  When the list already exists it carries a
     * base-pointing entry for the migrated attribute (the list describes
     * EVERY attribute of the file) - replace that entry with the
     * child-pointing one.  Without a list, create a fresh one covering all
     * remaining base attributes plus the migrated child. */
    {
        ULONGLONG BaseRef = ((ULONGLONG)BaseFileRecord->SequenceNumber << 48) |
                            BaseFileRecord->MFTRecordNumber;
        ULONGLONG ChildRef = ((ULONGLONG)ChildRecord->SequenceNumber << 48) | ChildMftIndex;

        ExistingList = NtfsFindAttributeListSlot(BaseFileRecord);
        ListExisted = (ExistingList != NULL);

        if (ListExisted)
        {
            /* Drop the migrated attribute's base-pointing entry (best-effort;
             * pre-existing volumes written before the complete-list convention
             * may not carry one). */
            NtfsAttributeListRemoveEntry(Vcb, BaseFileRecord, ExistingList,
                                         AttrInChild->Type,
                                         MigratedNameSrc, (UCHAR)MigratedNameLength,
                                         BaseRef, MigratedInstance);
            ExistingList = NtfsFindAttributeListSlot(BaseFileRecord);
            ListExisted = (ExistingList != NULL);
        }

        if (ListExisted)
        {
            Status = NtfsAttributeListAddEntry(Vcb, BaseFileRecord, ExistingList,
                                               AttrInChild->Type,
                                               MigratedNameSrc, (UCHAR)MigratedNameLength,
                                               AttrInChild->NonResident.LowestVCN,
                                               ChildRef, MigratedInstance);
            if (Status == STATUS_DISK_FULL && !ExistingList->IsNonResident)
            {
                /* The resident list can't grow inside the record: convert it
                 * to non-resident (Windows does the same once the list
                 * outgrows the record) and retry. */
                Status = NtfsConvertAttributeListToNonResident(Vcb, BaseFileRecord);
                if (NT_SUCCESS(Status))
                {
                    ExistingList = NtfsFindAttributeListSlot(BaseFileRecord);
                    ASSERT(ExistingList != NULL && ExistingList->IsNonResident);
                    Status = NtfsAttributeListAddEntry(Vcb, BaseFileRecord, ExistingList,
                                                       AttrInChild->Type,
                                                       MigratedNameSrc, (UCHAR)MigratedNameLength,
                                                       AttrInChild->NonResident.LowestVCN,
                                                       ChildRef, MigratedInstance);
                }
            }
        }
        else
        {
            Status = NtfsCreateAttributeList(Vcb, BaseFileRecord,
                                             AttrInChild->Type,
                                             MigratedNameSrc, (UCHAR)MigratedNameLength,
                                             AttrInChild->NonResident.LowestVCN,
                                             ChildRef, MigratedInstance);
        }

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("MigrateAttributeToList: recording the migrated attribute in the list failed 0x%x\n",
                    (unsigned)Status);
            /* Best-effort rollback: the child's MFT bit stays allocated (one
             * wasted entry), the volume stays consistent. */
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
            return Status;
        }
    }

    /* Step 7: update AttrContext->pRecord to point at the migrated attribute
     * as it now lives in the child record.
     *
     * NOTE: We DO NOT update AttrContext->FileMFTIndex.  The caller's other
     * code paths (AllocateIndexNode, etc.) use AttrContext->FileMFTIndex with
     * their own FileRecord buffer (which is still the base), so leaving the
     * field pointing at the base keeps those callers correct.  AddRun's own
     * post-migration UpdateFileRecord uses FileRecord->MFTRecordNumber (the
     * child's index) instead - see the AddRun code path. */
    if (AttrContext->pRecord)
        ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);
    AttrContext->pRecord = ExAllocatePoolWithTag(NonPagedPool, MigratedAttrLength, TAG_NTFS);
    if (!AttrContext->pRecord)
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(AttrContext->pRecord, AttrInChild, MigratedAttrLength);

    *OutChildRecord = ChildRecord;
    *OutNewAttrOffset = ChildAttrOffset;

    /* Persist the modified base record now.  The reader path needs to see the
     * $ATTRIBUTE_LIST entry the moment we leave this function, otherwise any
     * subsequent FindAttribute() that touches the base record on disk would
     * fail to locate the migrated attribute. */
    Status = UpdateFileRecord(Vcb, BaseFileRecord->MFTRecordNumber, BaseFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MigrateAttributeToList: failed to write base record back: 0x%x\n", (unsigned)Status);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        *OutChildRecord = NULL;
        return Status;
    }

    DPRINT("MigrateAttributeToList: success; attr type 0x%x now in MFT %I64u offset 0x%x (was MFT %I64u offset 0x%x)\n",
           AttrInChild->Type, ChildMftIndex, ChildAttrOffset,
           BaseFileRecord->MFTRecordNumber, AttrOffset);

    return STATUS_SUCCESS;
}

/**
* @name CoalesceAttributeFromList
* @implemented
*
* Reverse of MigrateAttributeToList. If a previously-migrated attribute has shrunk
* enough that its (header + mapping pairs / value) fits back inside the base file
* record after also accounting for the bytes reclaimed from the $ATTRIBUTE_LIST
* entry, this function moves the attribute back into the base record and removes
* the corresponding entry from the list.
*
* When removing the last entry from $ATTRIBUTE_LIST, the entire $ATTRIBUTE_LIST
* attribute slot is removed from the base record.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param BaseFileRecord
* Pointer to a complete copy of the base file record. Modified in place. Caller
* is responsible for writing it back via UpdateFileRecord (this function calls
* UpdateFileRecord on the base before returning).
*
* @param AttributeType
* The attribute type code to coalesce (e.g., AttributeData, AttributeIndexAllocation).
*
* @param Name
* The attribute name (UTF-16). Pass an empty string for unnamed attributes.
*
* @param NameLength
* Length of Name in WCHARs (not bytes).
*
* @return
* STATUS_SUCCESS on success.
* STATUS_OBJECT_NAME_NOT_FOUND if no matching $ATTRIBUTE_LIST entry exists.
* STATUS_NOT_IMPLEMENTED if the existing $ATTRIBUTE_LIST is non-resident, if
*   multiple list entries reference the same child MFT (multi-extent attribute),
*   or if the coalesced layout would not fit in the base record.
* Other status codes propagated from ReadFileRecord / UpdateFileRecord.
*
* @remarks
* Phase A scope mirror of MigrateAttributeToList: this function does NOT free
* the child MFT bit (the driver currently has no FreeMftEntry primitive). The
* child record becomes orphaned, wasting one MFT slot but leaving the volume
* consistent. Production use needs $LogFile journaling for crash safety AND a
* working FreeMftEntry - both are out of scope for the harness-testable core.
*/
NTSTATUS
CoalesceAttributeFromList(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER BaseFileRecord,
                          ULONG AttributeType,
                          PCWSTR Name,
                          USHORT NameLength)
{
    NTSTATUS Status;
    PNTFS_ATTR_RECORD ListAttr = NULL;
    PNTFS_ATTR_RECORD Walker;
    PUCHAR ListContent;
    PUCHAR ListEnd;
    PNTFS_ATTRIBUTE_LIST_ITEM Item;
    PNTFS_ATTRIBUTE_LIST_ITEM MatchItem = NULL;
    ULONGLONG ChildMftIndex = 0;
    USHORT MatchEntryLen = 0;
    PFILE_RECORD_HEADER ChildRecord = NULL;
    PNTFS_ATTR_RECORD ChildAttr;
    ULONG ChildAttrLen;
    ULONG OtherEntriesUsingChild = 0;
    ULONG NewListAttrLen;
    ULONG ListAttrShrinkBy;
    ULONG NewBytesInUse;
    BOOLEAN RemoveListAttribute;
    PNTFS_ATTR_RECORD InsertionPoint;
    PNTFS_ATTR_RECORD NewEnd;

    DPRINT("CoalesceAttributeFromList: base MFT=%I64u type=0x%x namelen=%u\n",
           BaseFileRecord->MFTRecordNumber, AttributeType, NameLength);

    if (AttributeType == AttributeStandardInformation ||
        AttributeType == AttributeFileName ||
        AttributeType == AttributeAttributeList)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Step 1: locate the $ATTRIBUTE_LIST attribute in the base record. */
    Walker = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->AttributeOffset);
    while (Walker->Type != AttributeEnd && Walker->Length > 0 &&
           (ULONG_PTR)Walker < (ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse)
    {
        if (Walker->Type == AttributeAttributeList)
        {
            ListAttr = Walker;
            break;
        }
        Walker = (PNTFS_ATTR_RECORD)((ULONG_PTR)Walker + Walker->Length);
    }
    if (!ListAttr)
    {
        DPRINT("CoalesceAttributeFromList: no $ATTRIBUTE_LIST in base record\n");
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (ListAttr->IsNonResident)
    {
        DPRINT1("CoalesceAttributeFromList: non-resident $ATTRIBUTE_LIST not supported\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    /* Step 2: walk the list, find the matching entry. Also count how many
     * entries (if any) reference the same child MFT - multi-extent attributes
     * (e.g. an attribute that itself spans several child records) cannot be
     * coalesced as a single block, so bail in that case. */
    ListContent = (PUCHAR)ListAttr + ListAttr->Resident.ValueOffset;
    ListEnd = ListContent + ListAttr->Resident.ValueLength;
    Item = (PNTFS_ATTRIBUTE_LIST_ITEM)ListContent;
    while ((PUCHAR)Item < ListEnd && Item->Length > 0)
    {
        if (Item->Type == AttributeType && Item->NameLength == NameLength)
        {
            BOOLEAN NameOk = TRUE;
            if (NameLength > 0)
            {
                PCWSTR ItemName = (PCWSTR)((PUCHAR)Item + Item->NameOffset);
                if (RtlCompareMemory(ItemName, Name, NameLength * sizeof(WCHAR)) != NameLength * sizeof(WCHAR))
                    NameOk = FALSE;
            }
            if (NameOk && MatchItem == NULL)
            {
                MatchItem = Item;
                MatchEntryLen = Item->Length;
                ChildMftIndex = Item->MFTIndex & 0x0000FFFFFFFFFFFFULL;
            }
        }
        Item = (PNTFS_ATTRIBUTE_LIST_ITEM)((PUCHAR)Item + Item->Length);
    }
    if (!MatchItem)
    {
        DPRINT("CoalesceAttributeFromList: no matching list entry for type 0x%x\n", AttributeType);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    /* Second pass: count other entries pointing at ChildMftIndex. If any other
     * entry also lives in the same child record, this attribute is multi-extent
     * and we can't coalesce just this slice. */
    Item = (PNTFS_ATTRIBUTE_LIST_ITEM)ListContent;
    while ((PUCHAR)Item < ListEnd && Item->Length > 0)
    {
        if (Item != MatchItem &&
            (Item->MFTIndex & 0x0000FFFFFFFFFFFFULL) == ChildMftIndex)
        {
            OtherEntriesUsingChild++;
        }
        Item = (PNTFS_ATTRIBUTE_LIST_ITEM)((PUCHAR)Item + Item->Length);
    }
    if (OtherEntriesUsingChild > 0)
    {
        DPRINT1("CoalesceAttributeFromList: child MFT %I64u referenced by %u other entries; multi-extent coalesce not supported\n",
                ChildMftIndex, OtherEntriesUsingChild);
        return STATUS_NOT_IMPLEMENTED;
    }

    /* Step 3: read the child record and find the attribute inside it. */
    ChildRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!ChildRecord)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadFileRecord(Vcb, ChildMftIndex, ChildRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("CoalesceAttributeFromList: ReadFileRecord(child=%I64u) failed 0x%x\n",
                ChildMftIndex, Status);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return Status;
    }

    ChildAttr = NULL;
    Walker = (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildRecord + ChildRecord->AttributeOffset);
    while (Walker->Type != AttributeEnd && Walker->Length > 0 &&
           (ULONG_PTR)Walker < (ULONG_PTR)ChildRecord + ChildRecord->BytesInUse)
    {
        if (Walker->Type == AttributeType && Walker->NameLength == NameLength)
        {
            BOOLEAN NameOk = TRUE;
            if (NameLength > 0)
            {
                PCWSTR AttrName = (PCWSTR)((ULONG_PTR)Walker + Walker->NameOffset);
                if (RtlCompareMemory(AttrName, Name, NameLength * sizeof(WCHAR)) != NameLength * sizeof(WCHAR))
                    NameOk = FALSE;
            }
            if (NameOk)
            {
                ChildAttr = Walker;
                break;
            }
        }
        Walker = (PNTFS_ATTR_RECORD)((ULONG_PTR)Walker + Walker->Length);
    }
    if (!ChildAttr)
    {
        DPRINT1("CoalesceAttributeFromList: matching attribute missing from child MFT %I64u\n", ChildMftIndex);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return STATUS_FILE_CORRUPT_ERROR;
    }
    ChildAttrLen = ChildAttr->Length;

    /* Step 4: compute new layout. Two cases:
     *   a) MatchItem is the only entry → remove the entire $ATTRIBUTE_LIST attribute
     *   b) Other entries remain        → shrink the list attribute by MatchEntryLen
     */
    if (ListAttr->Resident.ValueLength == MatchEntryLen)
    {
        /* List would become empty; remove the whole attribute slot. */
        RemoveListAttribute = TRUE;
        ListAttrShrinkBy = ListAttr->Length;  /* the entire $ATTRIBUTE_LIST attr is going away */
        NewListAttrLen = 0;
    }
    else
    {
        ULONG NewValueLen = ListAttr->Resident.ValueLength - MatchEntryLen;
        ULONG NewAttrLen = ALIGN_UP_BY(ListAttr->Resident.ValueOffset + NewValueLen, ATTR_RECORD_ALIGNMENT);
        RemoveListAttribute = FALSE;
        NewListAttrLen = NewAttrLen;
        ListAttrShrinkBy = ListAttr->Length - NewAttrLen;
    }

    NewBytesInUse = BaseFileRecord->BytesInUse - ListAttrShrinkBy + ChildAttrLen;
    if (NewBytesInUse > Vcb->NtfsInfo.BytesPerFileRecord)
    {
        DPRINT1("CoalesceAttributeFromList: would not fit (need %u, max %u)\n",
                NewBytesInUse, Vcb->NtfsInfo.BytesPerFileRecord);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return STATUS_NOT_IMPLEMENTED;
    }

    /* Step 5a: shrink or remove the list attribute. */
    if (RemoveListAttribute)
    {
        ULONG ListAttrOffset = (ULONG)((ULONG_PTR)ListAttr - (ULONG_PTR)BaseFileRecord);
        PNTFS_ATTR_RECORD AfterList = (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + ListAttr->Length);
        if (AfterList->Type != AttributeEnd)
        {
            PNTFS_ATTR_RECORD MovedFinal;
            MovedFinal = MoveAttributes(Vcb, AfterList,
                                        ListAttrOffset + ListAttr->Length,
                                        (ULONG_PTR)ListAttr);
            SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
        }
        else
        {
            SetFileRecordEnd(BaseFileRecord, ListAttr, FILE_RECORD_END);
        }
    }
    else
    {
        /* Slide the entries after MatchItem left by MatchEntryLen, and shrink
         * the list attribute's Length / ValueLength accordingly. */
        PUCHAR MatchEnd = (PUCHAR)MatchItem + MatchEntryLen;
        SIZE_T TailLen = ListEnd - MatchEnd;
        if (TailLen > 0)
            RtlMoveMemory(MatchItem, MatchEnd, TailLen);
        /* Zero the freshly-vacated tail bytes inside the value. */
        RtlZeroMemory((PUCHAR)ListContent + (ListAttr->Resident.ValueLength - MatchEntryLen),
                      MatchEntryLen);
        ListAttr->Resident.ValueLength -= MatchEntryLen;

        /* Slide trailing attributes (after the now-shrunk list attr) left if needed. */
        if (NewListAttrLen != ListAttr->Length)
        {
            ULONG ListAttrOffset = (ULONG)((ULONG_PTR)ListAttr - (ULONG_PTR)BaseFileRecord);
            PNTFS_ATTR_RECORD AfterList = (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + ListAttr->Length);
            ULONG OldListLen = ListAttr->Length;
            ListAttr->Length = NewListAttrLen;
            if (AfterList->Type != AttributeEnd)
            {
                PNTFS_ATTR_RECORD MovedFinal;
                MovedFinal = MoveAttributes(Vcb, AfterList,
                                            ListAttrOffset + OldListLen,
                                            (ULONG_PTR)ListAttr + NewListAttrLen);
                SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
            }
            else
            {
                PNTFS_ATTR_RECORD AdjustedEnd =
                    (PNTFS_ATTR_RECORD)((ULONG_PTR)ListAttr + NewListAttrLen);
                SetFileRecordEnd(BaseFileRecord, AdjustedEnd, FILE_RECORD_END);
            }
        }
    }

    /* Step 5b: insert the coalesced attribute at the end (just before AttributeEnd). */
    if (BaseFileRecord->BytesInUse + ChildAttrLen > Vcb->NtfsInfo.BytesPerFileRecord)
    {
        /* Defensive: shouldn't happen - we checked NewBytesInUse above. */
        DPRINT1("CoalesceAttributeFromList: insert would overflow despite size check\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return STATUS_NOT_IMPLEMENTED;
    }
    InsertionPoint = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse - 2 * sizeof(ULONG));
    ASSERT(InsertionPoint->Type == AttributeEnd);
    RtlCopyMemory(InsertionPoint, ChildAttr, ChildAttrLen);
    NewEnd = (PNTFS_ATTR_RECORD)((ULONG_PTR)InsertionPoint + ChildAttrLen);
    SetFileRecordEnd(BaseFileRecord, NewEnd, FILE_RECORD_END);

    /* Step 6: persist the modified base. The orphaned child record stays on disk
     * (no FreeMftEntry yet) - see remarks. */
    Status = UpdateFileRecord(Vcb, BaseFileRecord->MFTRecordNumber, BaseFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("CoalesceAttributeFromList: UpdateFileRecord(base) failed 0x%x\n", Status);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return Status;
    }

    DPRINT("CoalesceAttributeFromList: type 0x%x coalesced from MFT %I64u back into base MFT %I64u (child orphaned)\n",
           AttributeType, ChildMftIndex, BaseFileRecord->MFTRecordNumber);

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
    return STATUS_SUCCESS;
}

/**
* @name NtfsSpillFileNameToChild
*
* Places a single resident $FILE_NAME attribute (value = FnValue[FnValueLen])
* into a freshly-allocated child file record and appends a resident
* $ATTRIBUTE_LIST entry to BaseFileRecord that points at it.  This is how a file
* with more hard links than fit in one 1KB MFT record stores the overflow names:
* one $FILE_NAME per child (which keeps FindAttribute(AttributeFileName, "")
* unambiguous when it follows the list into a child), enumerated through the
* $ATTRIBUTE_LIST.
*
* The base record is modified in place (the list attribute is created or grown);
* the caller is responsible for writing BaseFileRecord back to disk.  The child
* record is allocated an MFT index and persisted here (by AddNewMftEntry).
*
* @return
* STATUS_SUCCESS on success.
* STATUS_DISK_FULL if the base record has no room for the new list entry - the
*   caller is expected to free room (e.g. by evicting an existing base $FILE_NAME
*   with NtfsEvictOneBaseFileName) and retry.
* STATUS_NOT_IMPLEMENTED if the base already carries a non-resident $ATTRIBUTE_LIST
*   (deferred; not produced for realistic hard-link counts).
* STATUS_INSUFFICIENT_RESOURCES / errors propagated from AddNewMftEntry.
*/
static
NTSTATUS
NtfsSpillFileNameToChild(PNTFS_VCB Vcb,
                         PFILE_RECORD_HEADER BaseFileRecord,
                         PFILENAME_ATTRIBUTE FnValue,
                         ULONG FnValueLen,
                         USHORT Instance,
                         BOOLEAN Indexed)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG ChildValueOffset = ALIGN_UP_BY(ResidentHeaderLength, VALUE_OFFSET_ALIGNMENT);
    ULONG ChildAttrLen = ALIGN_UP_BY(ChildValueOffset + FnValueLen, ATTR_RECORD_ALIGNMENT);
    ULONG ListEntryFixedSize = 0x1A;    /* NTFS_ATTRIBUTE_LIST_ITEM without trailing name */
    ULONG NewListItemSize = ALIGN_UP_BY(ListEntryFixedSize, 8);  /* $FILE_NAME attr name len 0 */
    PFILE_RECORD_HEADER ChildRecord;
    PNTFS_ATTR_RECORD AttrInChild;
    PNTFS_ATTR_RECORD AttrAfter;
    PNTFS_ATTR_RECORD ExistingList;
    ULONGLONG ChildMftIndex;
    NTSTATUS Status;

    /* Step 1: verify the base record can hold the new list entry BEFORE we
     * allocate a child MFT record (so a "no room" answer leaves no orphan). */
    ExistingList = NULL;
    {
        PNTFS_ATTR_RECORD Walker = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->AttributeOffset);
        while (Walker->Type != AttributeEnd &&
               (ULONG_PTR)Walker < (ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse)
        {
            if (Walker->Type == AttributeAttributeList)
            {
                ExistingList = Walker;
                break;
            }
            if (Walker->Length == 0)
                break;
            Walker = (PNTFS_ATTR_RECORD)((ULONG_PTR)Walker + Walker->Length);
        }
    }

    if (ExistingList != NULL)
    {
        if (ExistingList->IsNonResident)
        {
            /* Grow the non-resident list's cluster allocation for one more
             * entry NOW, before the child MFT record is allocated, so a
             * failure leaves no orphan.  The entry itself is inserted in
             * step 3. */
            Status = NtfsNonResidentListEnsureSpace(Vcb, BaseFileRecord, ExistingList,
                                                    (ULONG)ExistingList->NonResident.DataSize + NewListItemSize);
            if (!NT_SUCCESS(Status))
                return Status;
        }
        else
        {
            ULONG NewAttrLen = ALIGN_UP_BY(ExistingList->Resident.ValueOffset +
                                           ExistingList->Resident.ValueLength + NewListItemSize,
                                           ATTR_RECORD_ALIGNMENT);
            ULONG GrowBy = NewAttrLen - ExistingList->Length;
            if (BaseFileRecord->BytesInUse + GrowBy > Vcb->NtfsInfo.BytesPerFileRecord)
                return STATUS_DISK_FULL;
        }
    }
    else
    {
        /* Fresh list: its value must describe EVERY base attribute plus the
         * spilled name (Windows resolves attributes exclusively through the
         * list).  Size the resident placement, and fall back to the small
         * non-resident header when the full value doesn't fit. */
        ULONG ListValueOff = ALIGN_UP_BY(ResidentHeaderLength, VALUE_OFFSET_ALIGNMENT);
        ULONG NonResidentSlot = ALIGN_UP_BY(FIELD_OFFSET(NTFS_ATTR_RECORD, NonResident.CompressedSize) + 0x11,
                                            ATTR_RECORD_ALIGNMENT);
        ULONG ValueLen = NewListItemSize;
        ULONG ResidentSlot;
        PNTFS_ATTR_RECORD Walker =
            (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->AttributeOffset);

        while (Walker->Type != AttributeEnd && Walker->Length > 0 &&
               (ULONG_PTR)Walker < (ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse)
        {
            ValueLen += ALIGN_UP_BY(ListEntryFixedSize + Walker->NameLength * sizeof(WCHAR), 8);
            Walker = (PNTFS_ATTR_RECORD)((ULONG_PTR)Walker + Walker->Length);
        }
        ResidentSlot = ALIGN_UP_BY(ListValueOff + ValueLen, ATTR_RECORD_ALIGNMENT);

        if (BaseFileRecord->BytesInUse + min(ResidentSlot, NonResidentSlot) >
                Vcb->NtfsInfo.BytesPerFileRecord)
            return STATUS_DISK_FULL;
    }

    /* Step 2: build a child record carrying just this $FILE_NAME. */
    ChildRecord = NtfsCreateEmptyFileRecord(Vcb);
    if (!ChildRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    ChildRecord->BaseFileRecord =
        ((ULONGLONG)BaseFileRecord->SequenceNumber << 48) | BaseFileRecord->MFTRecordNumber;

    AttrInChild = (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildRecord + ChildRecord->AttributeOffset);
    RtlZeroMemory(AttrInChild, ChildAttrLen);
    AttrInChild->Type = AttributeFileName;
    AttrInChild->Length = ChildAttrLen;
    AttrInChild->IsNonResident = 0;
    AttrInChild->NameLength = 0;
    AttrInChild->NameOffset = (USHORT)ResidentHeaderLength;
    AttrInChild->Flags = 0;
    AttrInChild->Instance = Instance;
    AttrInChild->Resident.ValueLength = FnValueLen;
    AttrInChild->Resident.ValueOffset = (USHORT)ChildValueOffset;
    AttrInChild->Resident.Flags = Indexed ? RA_INDEXED : 0;
    RtlCopyMemory((PUCHAR)AttrInChild + ChildValueOffset, FnValue, FnValueLen);

    if (ChildRecord->NextAttributeNumber <= Instance)
        ChildRecord->NextAttributeNumber = Instance + 1;

    AttrAfter = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttrInChild + ChildAttrLen);
    SetFileRecordEnd(ChildRecord, AttrAfter, FILE_RECORD_END);

    Status = AddNewMftEntry(ChildRecord, Vcb, &ChildMftIndex, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtfsSpillFileNameToChild: AddNewMftEntry failed 0x%x\n", (unsigned)Status);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return Status;
    }

    /* Step 3: create or extend the $ATTRIBUTE_LIST in the base record
     * (room already verified above), adding an entry for the spilled name. */
    if (ExistingList != NULL)
    {
        Status = NtfsAttributeListAddEntry(Vcb, BaseFileRecord, ExistingList,
                                           AttributeFileName, NULL, 0,
                                           0,
                                           ((ULONGLONG)ChildRecord->SequenceNumber << 48) | ChildMftIndex,
                                           Instance);
    }
    else
    {
        /* Fresh list describing all base attributes plus the spilled name;
         * goes non-resident directly when the value doesn't fit resident. */
        Status = NtfsCreateAttributeList(Vcb, BaseFileRecord,
                                         AttributeFileName, NULL, 0,
                                         0,
                                         ((ULONGLONG)ChildRecord->SequenceNumber << 48) | ChildMftIndex,
                                         Instance);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtfsSpillFileNameToChild: recording the spilled name in the list failed 0x%x\n",
                (unsigned)Status);
        /* The child record is already persisted; it stays orphaned (one
         * wasted MFT bit) - same error contract as MigrateAttributeToList. */
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
        return Status;
    }

    DPRINT("NtfsSpillFileNameToChild: name spilled to child MFT %I64u (base MFT %I64u)\n",
           ChildMftIndex, BaseFileRecord->MFTRecordNumber);

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
    return STATUS_SUCCESS;
}

/**
* @name NtfsEvictOneBaseFileName
*
* Moves one resident $FILE_NAME out of the base record into a child (via
* NtfsSpillFileNameToChild) to free room in the base.  The SECOND $FILE_NAME
* found is preferred as the eviction victim (least churn); when the base holds
* only ONE $FILE_NAME it is evicted too - a base record with every name spilled
* to children is legal NTFS (GetFileNameFromRecord follows the $ATTRIBUTE_LIST),
* and refusing this case made the FIRST hard link to a file whose record is
* dominated by a large resident $DATA fail with STATUS_DISK_FULL (pacman's
* tzdata alias groups: ~700-byte zoneinfo files, "Can't create ..." for every
* link).  Called on the hard-link spill path when the base has no room even
* for a new $ATTRIBUTE_LIST entry.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_DISK_FULL if no resident $FILE_NAME remains in the base (nothing
*   left to evict).
*/
static
NTSTATUS
NtfsEvictOneBaseFileName(PNTFS_VCB Vcb,
                         PFILE_RECORD_HEADER BaseFileRecord)
{
    PNTFS_ATTR_RECORD Attr;
    PNTFS_ATTR_RECORD Victim = NULL;
    PNTFS_ATTR_RECORD FirstFn = NULL;
    PFILENAME_ATTRIBUTE Snapshot;
    ULONG FnValueLen;
    USHORT Instance;
    BOOLEAN Indexed;
    ULONG FnSeen = 0;
    NTSTATUS Status;

    Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->AttributeOffset);
    while (Attr->Type != AttributeEnd &&
           (ULONG_PTR)Attr < (ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse)
    {
        if (Attr->Length == 0)
            break;
        if (Attr->Type == AttributeFileName && !Attr->IsNonResident)
        {
            FnSeen++;
            if (FnSeen == 1)
            {
                FirstFn = Attr;
            }
            else
            {
                Victim = Attr;
                break;
            }
        }
        Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
    }

    /* Only one name left in the base: evict it as well. */
    if (Victim == NULL)
        Victim = FirstFn;

    if (Victim == NULL)
        return STATUS_DISK_FULL;

    FnValueLen = Victim->Resident.ValueLength;
    Instance = Victim->Instance;
    Indexed = (Victim->Resident.Flags & RA_INDEXED) != 0;

    Snapshot = ExAllocatePoolWithTag(NonPagedPool, FnValueLen, TAG_NTFS);
    if (!Snapshot)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Snapshot, (PUCHAR)Victim + Victim->Resident.ValueOffset, FnValueLen);

    /* Remove from the base first: this frees the victim's slot so the (much
     * smaller) list entry is guaranteed to fit. */
    RemoveResidentAttribute(Vcb, BaseFileRecord, Victim);

    Status = NtfsSpillFileNameToChild(Vcb, BaseFileRecord, Snapshot, FnValueLen, Instance, Indexed);
    if (!NT_SUCCESS(Status))
    {
        /* Best-effort rollback: put the name back in the base (room was just
         * freed).  This only runs on the spill error path, which does not occur
         * for realistic hard-link counts. */
        AddResidentAttribute(Vcb, BaseFileRecord, AttributeFileName, NULL, 0, Snapshot, FnValueLen);
    }

    ExFreePoolWithTag(Snapshot, TAG_NTFS);
    return Status;
}

/**
* @name NtfsAddHardLinkSpill
*
* Adds a new hard-link $FILE_NAME to a file whose base MFT record is already
* full (AddResidentAttribute returned STATUS_DISK_FULL).  The new name is stored
* in a child record referenced by a resident $ATTRIBUTE_LIST; base records are
* evicted as needed to keep room for the list.  Sets RA_INDEXED on the spilled
* attribute (every hard link is indexed in its parent directory).
*
* @return STATUS_SUCCESS, or an error (STATUS_DISK_FULL only if the record can no
* longer be extended - no resident $FILE_NAME left to evict and the
* $ATTRIBUTE_LIST is already non-resident).
*/
NTSTATUS
NtfsAddHardLinkSpill(PNTFS_VCB Vcb,
                     PFILE_RECORD_HEADER FileRecord,
                     PFILENAME_ATTRIBUTE NewFileName,
                     ULONG NewFileNameLength)
{
    NTSTATUS Status;
    USHORT NewInstance;
    BOOLEAN TriedNonResidentList = FALSE;

    for (;;)
    {
        NewInstance = FileRecord->NextAttributeNumber;
        Status = NtfsSpillFileNameToChild(Vcb, FileRecord, NewFileName, NewFileNameLength,
                                          NewInstance, TRUE);
        if (Status != STATUS_DISK_FULL)
        {
            if (NT_SUCCESS(Status))
                FileRecord->NextAttributeNumber = NewInstance + 1;
            return Status;
        }

        /* No room for even a list entry: evict an existing base name and retry. */
        Status = NtfsEvictOneBaseFileName(Vcb, FileRecord);
        if (Status == STATUS_DISK_FULL && !TriedNonResidentList)
        {
            /* Nothing left to evict: the resident $ATTRIBUTE_LIST itself is
             * what no longer fits.  Convert the list to non-resident (Windows
             * does the same once the list outgrows the record) and retry -
             * the base record then only carries the small non-resident
             * header, and further entries grow the list's clusters. */
            TriedNonResidentList = TRUE;
            Status = NtfsConvertAttributeListToNonResident(Vcb, FileRecord);
            if (!NT_SUCCESS(Status))
                return (Status == STATUS_INVALID_PARAMETER ||
                        Status == STATUS_OBJECT_NAME_NOT_FOUND) ? STATUS_DISK_FULL : Status;
            continue;
        }
        if (!NT_SUCCESS(Status))
            return Status;
    }
}

/**
* @name AddRun
* @implemented
*
* Adds a run of allocated clusters to a non-resident attribute.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param AttrContext
* Pointer to an NTFS_ATTR_CONTEXT describing the destination attribute.
*
* @param AttrOffset
* Byte offset of the destination attribute relative to its file record.
*
* @param FileRecord
* Pointer to a complete copy of the file record containing the destination attribute. Must be at least
* Vcb->NtfsInfo.BytesPerFileRecord bytes long.
*
* @param NextAssignedCluster
* Logical cluster number of the start of the data run being added.
*
* @param RunLength
* How many clusters are in the data run being added. Can't be 0.
*
* @return
* STATUS_SUCCESS on success. STATUS_INVALID_PARAMETER if AttrContext describes a resident attribute.
* STATUS_INSUFFICIENT_RESOURCES if ConvertDataRunsToLargeMCB() fails or if we fail to allocate a
* buffer for the new data runs.
* STATUS_INSUFFICIENT_RESOURCES or STATUS_UNSUCCESSFUL if FsRtlAddLargeMcbEntry() fails.
* STATUS_BUFFER_TOO_SMALL if ConvertLargeMCBToDataRuns() fails.
* STATUS_NOT_IMPLEMENTED if we need to migrate the attribute to an attribute list (TODO).
*
* @remarks
* Clusters should have been allocated previously with NtfsAllocateClusters().
*
*
*/
NTSTATUS
AddRun(PNTFS_VCB Vcb,
       PNTFS_ATTR_CONTEXT AttrContext,
       ULONG AttrOffset,
       PFILE_RECORD_HEADER FileRecord,
       ULONGLONG NextAssignedCluster,
       ULONG RunLength)
{
    NTSTATUS Status;
    int DataRunMaxLength;
    PNTFS_ATTR_RECORD DestinationAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONG NextAttributeOffset = AttrOffset + AttrContext->pRecord->Length;
    ULONGLONG NextVBN = 0;
    /* Tracks a child file record buffer borrowed via MigrateAttributeToList(); we
     * own it for the rest of AddRun and must free it before returning. */
    PFILE_RECORD_HEADER MigratedChildRecord = NULL;

    PUCHAR RunBuffer;
    ULONG RunBufferSize;
    BOOLEAN MftCtxLockHeld = FALSE;

    if (!AttrContext->pRecord->IsNonResident)
        return STATUS_INVALID_PARAMETER;

    /* Phase 4A.5: if a previous AddRun call already migrated this attribute
     * to a child record, the caller's FileRecord/AttrOffset point at the
     * BASE record where the slot is gone (or replaced by a moved trailing
     * attribute).  Re-read the child from disk and re-target our local
     * FileRecord/AttrOffset there.  We write the child back via
     * UpdateFileRecord at the bottom of the function and free the local
     * buffer before returning. */
    if (AttrContext->MigratedToMFTIndex != 0)
    {
        PFILE_RECORD_HEADER ChildBuf;
        NTSTATUS ChildStatus;

        ChildBuf = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
        if (!ChildBuf)
            return STATUS_INSUFFICIENT_RESOURCES;

        ChildStatus = ReadFileRecord(Vcb, AttrContext->MigratedToMFTIndex, ChildBuf);
        if (!NT_SUCCESS(ChildStatus))
        {
            DPRINT1("AddRun: ReadFileRecord(child MFT %I64u) failed 0x%x\n",
                    AttrContext->MigratedToMFTIndex, (unsigned)ChildStatus);
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildBuf);
            return ChildStatus;
        }

        /* Phase A: the migrated attribute is the first (and only) attribute
         * in the child record, sitting at AttributeOffset. */
        MigratedChildRecord = ChildBuf;
        FileRecord = ChildBuf;
        AttrOffset = ChildBuf->AttributeOffset;
        DestinationAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
        NextAttributeOffset = AttrOffset + AttrContext->pRecord->Length;

        /* The child was just re-read from disk, so its size fields are
         * whatever the last write-back persisted.  The caller maintains
         * AllocatedSize/DataSize/InitializedSize on pRecord only - its own
         * FileRecord/AttrOffset point at the BASE record where the slot is
         * gone, so its in-buffer mirror update skips them.  Mirror them
         * here, or every post-migration extension persists new runs and
         * HighestVCN with the migration-time sizes frozen (observed live:
         * a $I30 $INDEX_ALLOCATION with HighestVCN 194 but AllocatedSize
         * covering 192 clusters, making every index node past VCN 191 be
         * rejected as corrupt and every insert fail with a collision). */
        DestinationAttribute->NonResident.AllocatedSize = AttrContext->pRecord->NonResident.AllocatedSize;
        DestinationAttribute->NonResident.DataSize = AttrContext->pRecord->NonResident.DataSize;
        DestinationAttribute->NonResident.InitializedSize = AttrContext->pRecord->NonResident.InitializedSize;
    }

    if (FsRtlNumberOfRunsInLargeMcb(&AttrContext->DataRunsMCB) != 0)
    {
        LONGLONG LastVbn;
        LONGLONG LastLbn;
        LONGLONG LastCount;
        LONG LastRun = FsRtlNumberOfRunsInLargeMcb(&AttrContext->DataRunsMCB) - 1;

        if (FsRtlGetNextLargeMcbEntry(&AttrContext->DataRunsMCB,
                                      LastRun,
                                      &LastVbn,
                                      &LastLbn,
                                      &LastCount))
        {
            NextVBN = LastVbn + LastCount;
        }
        else
        {
            NextVBN = AttrContext->pRecord->NonResident.HighestVCN + 1;
        }
    }

    // Add newly-assigned clusters to mcb
    _SEH2_TRY
    {
        if (!FsRtlAddLargeMcbEntry(&AttrContext->DataRunsMCB,
                                   NextVBN,
                                   NextAssignedCluster,
                                   RunLength))
        {
            ExRaiseStatus(STATUS_UNSUCCESSFUL);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        DPRINT1("Failed to add LargeMcb Entry!\n");
        if (MigratedChildRecord)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MigratedChildRecord);
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    RunBuffer = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (!RunBuffer)
    {
        DPRINT1("ERROR: Couldn't allocate memory for data runs!\n");
        if (MigratedChildRecord)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MigratedChildRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Convert the map control block back to encoded data runs
    /* On STATUS_BUFFER_TOO_SMALL the conversion still terminates the buffer and
     * reports the SHORT length, so ignoring the status here would persist a
     * runlist with every remaining run silently dropped - the attribute's tail
     * clusters become unreachable while HighestVCN still claims them.  That is
     * on-disk corruption, so fail the extension instead. */
    Status = ConvertLargeMCBToDataRuns(&AttrContext->DataRunsMCB, RunBuffer, Vcb->NtfsInfo.BytesPerFileRecord, &RunBufferSize);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to convert MCB to data runs (0x%x)!\n", (unsigned)Status);
        ExFreePoolWithTag(RunBuffer, TAG_NTFS);
        if (MigratedChildRecord)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MigratedChildRecord);
        return Status;
    }

    // Get the amount of free space between the start of the of the first data run and the attribute end
    DataRunMaxLength = AttrContext->pRecord->Length - AttrContext->pRecord->NonResident.MappingPairsOffset;

    /* From here on we free/realloc and/or rewrite AttrContext->pRecord's encoded
     * runlist.  When this is the shared $MFT $DATA context, take the MftContext
     * LEAF lock EXCLUSIVE so a concurrent ReadFileRecord (which walks the same
     * buffer under the SHARED lock) cannot observe a freed or half-rewritten
     * runlist and short-read (STATUS_PARTIAL_COPY).  This is reached only after
     * the caller already allocated clusters and dropped BitmapResource, so the
     * leaf invariant (nothing else held) holds.  Released on every exit below. */
    if (AttrContext == Vcb->MFTContext && Vcb->MftReadLockReady)
        MftCtxLockHeld = ExAcquireResourceExclusiveLite(&Vcb->MftContextResource, TRUE);

    // Do we need to extend the attribute (or convert to attribute list)?
    if (DataRunMaxLength < RunBufferSize)
    {
        PNTFS_ATTR_RECORD NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + NextAttributeOffset);
        PNTFS_ATTR_RECORD NewRecord;

        // Add free space at the end of the file record to DataRunMaxLength
        DataRunMaxLength += Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse;

        // Can we resize the attribute?
        if (DataRunMaxLength < RunBufferSize)
        {
            /* Not enough room even after eating all the slack in the base record.
             * Migrate this attribute to a child file record via $ATTRIBUTE_LIST. */
            PFILE_RECORD_HEADER ChildRecord = NULL;
            ULONG NewAttrOffsetInChild = 0;
            NTSTATUS MigrateStatus;

            DPRINT("AddRun: base record full (need %u, have %d); migrating attr type 0x%x to $ATTRIBUTE_LIST\n",
                   RunBufferSize, DataRunMaxLength,
                   AttrContext->pRecord->Type);

            MigrateStatus = MigrateAttributeToList(Vcb,
                                                   FileRecord,
                                                   AttrContext,
                                                   AttrOffset,
                                                   &ChildRecord,
                                                   &NewAttrOffsetInChild);
            if (!NT_SUCCESS(MigrateStatus))
            {
                DPRINT1("AddRun: MigrateAttributeToList failed 0x%x\n", (unsigned)MigrateStatus);
                ExFreePoolWithTag(RunBuffer, TAG_NTFS);
                if (MftCtxLockHeld)
                    ExReleaseResourceLite(&Vcb->MftContextResource);
                return MigrateStatus;
            }

            /* From here on we operate on the CHILD record's copy of the attribute,
             * not the base record's slot.  Re-do the slot-resize check against the
             * child record (which has plenty of room since it's nearly empty), then
             * fall through to the normal mapping-pair write path. */
            MigratedChildRecord = ChildRecord;
            FileRecord = ChildRecord;
            AttrOffset = NewAttrOffsetInChild;
            DestinationAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
            NextAttributeOffset = AttrOffset + AttrContext->pRecord->Length;
            NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + NextAttributeOffset);

            /* Recompute available room - should be the entire rest of the child record. */
            DataRunMaxLength = AttrContext->pRecord->Length - AttrContext->pRecord->NonResident.MappingPairsOffset
                             + (Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse);

            if (DataRunMaxLength < (int)RunBufferSize)
            {
                /* Even the child record can't fit the mapping pairs.  This means a
                 * single attribute's mapping pairs exceed an entire file record's
                 * worth - extremely unlikely with realistic file sizes, but bail
                 * cleanly if it ever happens. */
                DPRINT1("AddRun: child record also too small (need %u, have %d)\n",
                        RunBufferSize, DataRunMaxLength);
                ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
                ExFreePoolWithTag(RunBuffer, TAG_NTFS);
                if (MftCtxLockHeld)
                    ExReleaseResourceLite(&Vcb->MftContextResource);
                return STATUS_NOT_IMPLEMENTED;
            }

            /* The child record is fresh (BytesInUse covers only the migrated
             * attribute and the end markers), so the trailing-attribute logic
             * below doesn't need to move anything - fall through into the rest of
             * the existing AddRun path with the new (FileRecord, AttrOffset). */
        }

        // Are there more attributes after the one we're resizing?
        if (NextAttribute->Type != AttributeEnd)
        {
            PNTFS_ATTR_RECORD FinalAttribute;
            ULONG TrailingBytes;

            // Calculate where to move the trailing attributes
            ULONG_PTR MoveTo = (ULONG_PTR)DestinationAttribute + AttrContext->pRecord->NonResident.MappingPairsOffset + RunBufferSize;
            MoveTo = ALIGN_UP_BY(MoveTo, ATTR_RECORD_ALIGNMENT);

            // Calculate total size of trailing attributes (including end marker)
            TrailingBytes = FileRecord->BytesInUse - NextAttributeOffset;

            // Verify the move won't overflow the file record buffer
            if (MoveTo + TrailingBytes > (ULONG_PTR)FileRecord + Vcb->NtfsInfo.BytesPerFileRecord)
            {
                DPRINT1("FIXME: Not enough space in file record for data runs + trailing attributes! "
                        "MoveTo=0x%Ix TrailingBytes=%lu RecordEnd=0x%Ix\n",
                        MoveTo, TrailingBytes,
                        (ULONG_PTR)FileRecord + Vcb->NtfsInfo.BytesPerFileRecord);
                ExFreePoolWithTag(RunBuffer, TAG_NTFS);
                if (MftCtxLockHeld)
                    ExReleaseResourceLite(&Vcb->MftContextResource);
                return STATUS_NOT_IMPLEMENTED;
            }

            DPRINT("Moving attribute(s) after this one starting with type 0x%lx\n", NextAttribute->Type);

            // Move the trailing attributes; FinalAttribute will point to the end marker
            FinalAttribute = MoveAttributes(Vcb, NextAttribute, NextAttributeOffset, MoveTo);

            // set the file record end
            SetFileRecordEnd(FileRecord, FinalAttribute, FILE_RECORD_END);
        }

        // calculate position of end markers
        NextAttributeOffset = AttrOffset + AttrContext->pRecord->NonResident.MappingPairsOffset + RunBufferSize;
        NextAttributeOffset = ALIGN_UP_BY(NextAttributeOffset, ATTR_RECORD_ALIGNMENT);

        // Update the length of the destination attribute
        DestinationAttribute->Length = NextAttributeOffset - AttrOffset;

        // Create a new copy of the attribute record
        NewRecord = ExAllocatePoolWithTag(NonPagedPool, DestinationAttribute->Length, TAG_NTFS);
        RtlCopyMemory(NewRecord, AttrContext->pRecord, AttrContext->pRecord->Length);
        NewRecord->Length = DestinationAttribute->Length;

        // Free the old copy of the attribute record, which won't be large enough
        ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);

        // Set the attribute context's record to the new copy
        AttrContext->pRecord = NewRecord;

        // if NextAttribute is the AttributeEnd marker
        if (NextAttribute->Type == AttributeEnd)
        {
            // End the file record
            NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + NextAttributeOffset);
            SetFileRecordEnd(FileRecord, NextAttribute, FILE_RECORD_END);
        }
    }

    // Update HighestVCN
    DestinationAttribute->NonResident.HighestVCN =
    AttrContext->pRecord->NonResident.HighestVCN = max(NextVBN - 1 + RunLength,
                                                     AttrContext->pRecord->NonResident.HighestVCN);

    /* Both copies below write RunBufferSize bytes into a pool allocation: the
     * file record buffer (BytesPerFileRecord, from FileRecLookasideList) and the
     * standalone pRecord copy (pRecord->Length).  The resize/migrate logic above
     * is supposed to have guaranteed the room, but if it ever does not, these
     * memcpys run off the end of NonPagedPool and scribble encoded mapping pairs
     * over whatever follows - observed live as an ExpCheckPoolBlocks assert with
     * a 179-run runlist sitting in an unrelated pool page, and (because NTFS
     * caches the $MFT $BITMAP in that same pool) as the allocator then handing
     * out file records that were still in use.  Verify the invariant at the
     * choke point instead of corrupting the volume. */
    if (DestinationAttribute->NonResident.MappingPairsOffset + RunBufferSize > DestinationAttribute->Length ||
        AttrContext->pRecord->NonResident.MappingPairsOffset + RunBufferSize > AttrContext->pRecord->Length)
    {
        DPRINT1("ERROR: encoded runs (%u bytes) overflow the attribute record "
                "(dest %u/%u, pRecord %u/%u)!\n",
                RunBufferSize,
                DestinationAttribute->NonResident.MappingPairsOffset, DestinationAttribute->Length,
                AttrContext->pRecord->NonResident.MappingPairsOffset, AttrContext->pRecord->Length);
        ASSERT(FALSE);
        ExFreePoolWithTag(RunBuffer, TAG_NTFS);
        if (MftCtxLockHeld)
            ExReleaseResourceLite(&Vcb->MftContextResource);
        if (MigratedChildRecord)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MigratedChildRecord);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    // Write data runs to destination attribute
    RtlCopyMemory((PVOID)((ULONG_PTR)DestinationAttribute + DestinationAttribute->NonResident.MappingPairsOffset),
                  RunBuffer,
                  RunBufferSize);

    // Update the attribute record in the attribute context
    RtlCopyMemory((PVOID)((ULONG_PTR)AttrContext->pRecord + AttrContext->pRecord->NonResident.MappingPairsOffset),
                  RunBuffer,
                  RunBufferSize);

    /* pRecord is now fully consistent again: drop the MftContext leaf so
     * readers can resume.  The disk write-back below touches the on-disk record,
     * not the in-memory runlist a reader walks. */
    if (MftCtxLockHeld)
    {
        ExReleaseResourceLite(&Vcb->MftContextResource);
        MftCtxLockHeld = FALSE;
    }

    /* Write the (possibly migrated) file record back.  Use FileRecord's own
     * MFTRecordNumber rather than AttrContext->FileMFTIndex: when migration
     * happened above, FileRecord points at the child record buffer (which has
     * its own MFTRecordNumber set by AddNewMftEntry), while AttrContext still
     * carries the BASE record's index so other callers continue to function. */
    Status = UpdateFileRecord(Vcb, FileRecord->MFTRecordNumber, FileRecord);

    ExFreePoolWithTag(RunBuffer, TAG_NTFS);

    NtfsDumpDataRuns((PUCHAR)((ULONG_PTR)DestinationAttribute + DestinationAttribute->NonResident.MappingPairsOffset), 0);

    /* If we migrated the attribute to a child record above, FileRecord points at
     * the child buffer we own - release it here.  The caller's original base
     * record buffer was already written back inside MigrateAttributeToList(). */
    if (MigratedChildRecord)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MigratedChildRecord);

    return Status;
}

/**
* @name AddStandardInformation
* @implemented
*
* Adds a $STANDARD_INFORMATION attribute to a given FileRecord.
*
* @param FileRecord
* Pointer to a complete file record to add the attribute to. Caller is responsible for
* ensuring FileRecord is large enough to contain $STANDARD_INFORMATION.
*
* @param AttributeAddress
* Pointer to the region of memory that will receive the $STANDARD_INFORMATION attribute.
* This address must reside within FileRecord. Must be aligned to an 8-byte boundary (relative to FileRecord).
*
* @return
* STATUS_SUCCESS on success. STATUS_NOT_IMPLEMENTED if target address isn't at the end
* of the given file record.
*
* @remarks
* Only adding the attribute to the end of the file record is supported; AttributeAddress must
* be of type AttributeEnd.
* As it's implemented, this function is only intended to assist in creating new file records. It
* could be made more general-purpose by considering file records with an $ATTRIBUTE_LIST.
* It's the caller's responsibility to ensure the given file record has enough memory allocated
* for the attribute.
*/
NTSTATUS
AddStandardInformation(PFILE_RECORD_HEADER FileRecord,
                       PNTFS_ATTR_RECORD AttributeAddress,
                       ULONG FileAttributes)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    PSTANDARD_INFORMATION StandardInfo = (PSTANDARD_INFORMATION)((LONG_PTR)AttributeAddress + ResidentHeaderLength);
    LARGE_INTEGER SystemTime;
    ULONG FileRecordEnd = AttributeAddress->Length;

    if (AttributeAddress->Type != AttributeEnd)
    {
        DPRINT1("FIXME: Can only add $STANDARD_INFORMATION attribute to the end of a file record.\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    AttributeAddress->Type = AttributeStandardInformation;
    AttributeAddress->Length = sizeof(STANDARD_INFORMATION) + ResidentHeaderLength;
    AttributeAddress->Length = ALIGN_UP_BY(AttributeAddress->Length, ATTR_RECORD_ALIGNMENT);
    AttributeAddress->Resident.ValueLength = sizeof(STANDARD_INFORMATION);
    AttributeAddress->Resident.ValueOffset = ResidentHeaderLength;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    // set dates and times
    KeQuerySystemTime(&SystemTime);
    StandardInfo->CreationTime = SystemTime.QuadPart;
    StandardInfo->ChangeTime = SystemTime.QuadPart;
    StandardInfo->LastWriteTime = SystemTime.QuadPart;
    StandardInfo->LastAccessTime = SystemTime.QuadPart;
    StandardInfo->FileAttribute = NTFS_FILE_TYPE_ARCHIVE | (FileAttributes & NTFS_FILE_ATTRIBUTE_VALID_FLAGS);

    // move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);

    return STATUS_SUCCESS;
}

/**
* @name ConvertDataRunsToLargeMCB
* @implemented
*
* Converts binary data runs to a map control block.
*
* @param DataRun
* Pointer to the run data
*
* @param DataRunsMCB
* Pointer to an unitialized LARGE_MCB structure.
*
* @return
* STATUS_SUCCESS on success, STATUS_INSUFFICIENT_RESOURCES or STATUS_UNSUCCESSFUL if we fail to
* initialize the mcb or add an entry.
*
* @remarks
* Initializes the LARGE_MCB pointed to by DataRunsMCB. If this function succeeds, you
* need to call FsRtlUninitializeLargeMcb() when you're done with DataRunsMCB. This
* function will ensure the LargeMCB has been unitialized in case of failure.
*
*/
NTSTATUS
ConvertDataRunsToLargeMCB(PUCHAR DataRun,
                          PLARGE_MCB DataRunsMCB,
                          PULONGLONG pNextVBN)
{
    LONGLONG  DataRunOffset;
    ULONGLONG DataRunLength;
    LONGLONG  DataRunStartLCN;
    ULONGLONG LastLCN = 0;

    // Initialize the MCB, potentially catch an exception
    _SEH2_TRY{
        FsRtlInitializeLargeMcb(DataRunsMCB, NonPagedPool);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    } _SEH2_END;

    while (*DataRun != 0)
    {
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);

        if (DataRunOffset != -1)
        {
            // Normal data run.
            DataRunStartLCN = LastLCN + DataRunOffset;
            LastLCN = DataRunStartLCN;

            _SEH2_TRY{
                if (!FsRtlAddLargeMcbEntry(DataRunsMCB,
                                           *pNextVBN,
                                           DataRunStartLCN,
                                           DataRunLength))
                {
                    ExRaiseStatus(STATUS_UNSUCCESSFUL);
                }
            } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                FsRtlUninitializeLargeMcb(DataRunsMCB);
                _SEH2_YIELD(return _SEH2_GetExceptionCode());
            } _SEH2_END;

        }

        *pNextVBN += DataRunLength;
    }

    return STATUS_SUCCESS;
}

/**
* @name ConvertLargeMCBToDataRuns
* @implemented
*
* Converts a map control block to a series of encoded data runs (used by non-resident attributes).
*
* @param DataRunsMCB
* Pointer to a LARGE_MCB structure describing the data runs.
*
* @param RunBuffer
* Pointer to the buffer that will receive the encoded data runs.
*
* @param MaxBufferSize
* Size of RunBuffer, in bytes.
*
* @param UsedBufferSize
* Pointer to a ULONG that will receive the size of the data runs in bytes. Can't be NULL.
*
* @return
* STATUS_SUCCESS on success, STATUS_BUFFER_TOO_SMALL if RunBuffer is too small to contain the
* complete output.
*
*/
NTSTATUS
ConvertLargeMCBToDataRuns(PLARGE_MCB DataRunsMCB,
                          PUCHAR RunBuffer,
                          ULONG MaxBufferSize,
                          PULONG UsedBufferSize)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG RunBufferOffset = 0;
    LONGLONG  DataRunOffset;
    ULONGLONG LastLCN = 0;
    LONGLONG ExpectedVbn = 0;
    LONGLONG Vbn, Lbn, Count;
    ULONG i;


    DPRINT("\t[Vbn, Lbn, Count]\n");

    // convert each mcb entry to a data run
    for (i = 0; FsRtlGetNextLargeMcbEntry(DataRunsMCB, i, &Vbn, &Lbn, &Count); i++)
    {
        UCHAR DataRunOffsetSize = 0;
        UCHAR DataRunLengthSize = 0;
        UCHAR ControlByte = 0;

        // [vbn, lbn, count]
        DPRINT("\t[%I64d, %I64d,%I64d]\n", Vbn, Lbn, Count);

        // If there's a Vbn gap before this entry, emit a sparse run for the
        // hole.  The MCB doesn't store explicit sparse markers (we drop them
        // in ConvertDataRunsToLargeMCB) - gaps in Vbn ARE the sparse holes,
        // and round-trip stability requires re-emitting them here.
        if (Vbn > ExpectedVbn)
        {
            LONGLONG SparseLength = Vbn - ExpectedVbn;
            UCHAR SparseLenSize = GetPackedByteCount(SparseLength, TRUE);

            if (RunBufferOffset + 2 + SparseLenSize > MaxBufferSize)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                DPRINT1("FIXME: Ran out of room in buffer for sparse run!\n");
                break;
            }
            // Sparse run: DataRunOffsetSize=0, DataRunLengthSize=SparseLenSize.
            RunBuffer[RunBufferOffset++] = SparseLenSize & 0x0F;
            RtlCopyMemory(RunBuffer + RunBufferOffset, &SparseLength, SparseLenSize);
            RunBufferOffset += SparseLenSize;
        }
        ExpectedVbn = Vbn + Count;

        DataRunOffset = Lbn - LastLCN;
        LastLCN = Lbn;

        // now we need to determine how to represent DataRunOffset with the minimum number of bytes
        DPRINT("Determining how many bytes needed to represent %I64x\n", DataRunOffset);
        DataRunOffsetSize = GetPackedByteCount(DataRunOffset, TRUE);
        DPRINT("%d bytes needed.\n", DataRunOffsetSize);

        // determine how to represent DataRunLengthSize with the minimum number of bytes
        DPRINT("Determining how many bytes needed to represent %I64x\n", Count);
        DataRunLengthSize = GetPackedByteCount(Count, TRUE);
        DPRINT("%d bytes needed.\n", DataRunLengthSize);

        // ensure the next data run + end marker would be <= Max buffer size
        if (RunBufferOffset + 2 + DataRunLengthSize + DataRunOffsetSize > MaxBufferSize)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
            DPRINT1("FIXME: Ran out of room in buffer for data runs!\n");
            break;
        }

        // pack and copy the control byte
        ControlByte = (DataRunOffsetSize << 4) + DataRunLengthSize;
        RunBuffer[RunBufferOffset++] = ControlByte;

        // copy DataRunLength
        RtlCopyMemory(RunBuffer + RunBufferOffset, &Count, DataRunLengthSize);
        RunBufferOffset += DataRunLengthSize;

        // copy DataRunOffset
        RtlCopyMemory(RunBuffer + RunBufferOffset, &DataRunOffset, DataRunOffsetSize);
        RunBufferOffset += DataRunOffsetSize;
    }

    // End of data runs
    RunBuffer[RunBufferOffset++] = 0;

    *UsedBufferSize = RunBufferOffset;
    DPRINT("New Size of DataRuns: %ld\n", *UsedBufferSize);

    return Status;
}

/**
 * @name RemoveResidentAttribute
 * @implemented
 *
 * Removes the attribute slot pointed to by AttrAddress from FileRecord by
 * shifting any following attributes (and the AttributeEnd marker) down into
 * the vacated space.  Caller must have located the slot already (e.g. with
 * FindAttribute + pointer arithmetic against pRecord / AttrOffset).
 *
 * @param Vcb
 * Unused; retained for symmetry with AddResidentAttribute.
 *
 * @param FileRecord
 * Pointer to the file record holding the slot.
 *
 * @param AttrAddress
 * Pointer to the attribute to remove; must lie within FileRecord.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER if AttrAddress is already the
 * end marker.
 */
NTSTATUS
RemoveResidentAttribute(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER FileRecord,
                        PNTFS_ATTR_RECORD AttrAddress)
{
    PNTFS_ATTR_RECORD NextAttr;
    PNTFS_ATTR_RECORD FinalAttribute;
    ULONG AttrOffset;

    if (AttrAddress->Type == AttributeEnd)
        return STATUS_INVALID_PARAMETER;

    /* Keep the $ATTRIBUTE_LIST complete (see AddResidentAttribute): drop this
     * attribute's base-pointing entry first.  The list shrink slides the
     * record contents around, so re-find the slot by (type, instance)
     * afterwards.  A missing entry is fine - volumes written before the
     * complete-list convention only listed spilled attributes. */
    if (AttrAddress->Type != AttributeAttributeList)
    {
        PNTFS_ATTR_RECORD ListAttr = NtfsFindAttributeListSlot(FileRecord);

        if (ListAttr != NULL)
        {
            ULONG Type = AttrAddress->Type;
            USHORT Instance = AttrAddress->Instance;
            UCHAR NameLength = AttrAddress->NameLength;
            WCHAR NameBuf[255];
            PNTFS_ATTR_RECORD Attr;

            if (NameLength > 0)
                RtlCopyMemory(NameBuf,
                              (PUCHAR)AttrAddress + AttrAddress->NameOffset,
                              NameLength * sizeof(WCHAR));

            NtfsAttributeListRemoveEntry(Vcb, FileRecord, ListAttr,
                                         Type, NameBuf, NameLength,
                                         ((ULONGLONG)FileRecord->SequenceNumber << 48) |
                                             FileRecord->MFTRecordNumber,
                                         Instance);

            Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
            AttrAddress = NULL;
            while (Attr->Type != AttributeEnd && Attr->Length > 0 &&
                   (ULONG_PTR)Attr < (ULONG_PTR)FileRecord + FileRecord->BytesInUse)
            {
                if (Attr->Type == Type && Attr->Instance == Instance)
                {
                    AttrAddress = Attr;
                    break;
                }
                Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
            }
            if (AttrAddress == NULL)
            {
                DPRINT1("RemoveResidentAttribute: slot lost after list-entry removal\n");
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
        }
    }

    NextAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttrAddress + AttrAddress->Length);
    AttrOffset = (ULONG)((ULONG_PTR)AttrAddress - (ULONG_PTR)FileRecord);

    if (NextAttr->Type == AttributeEnd)
    {
        /* Nothing trailing; just write a fresh end marker at the slot. */
        SetFileRecordEnd(FileRecord, AttrAddress, FILE_RECORD_END);
    }
    else
    {
        FinalAttribute = MoveAttributes(Vcb,
                                        NextAttr,
                                        AttrOffset + AttrAddress->Length,
                                        (ULONG_PTR)AttrAddress);
        SetFileRecordEnd(FileRecord, FinalAttribute, FILE_RECORD_END);
    }

    return STATUS_SUCCESS;
}

/*
 * Walk a LARGE_MCB runlist starting at StartingVcn and fill a
 * RETRIEVAL_POINTERS_BUFFER per the Win32 FSCTL_GET_RETRIEVAL_POINTERS
 * contract.  Extracted from the IRP-bound wrapper so userspace tests can
 * exercise the walker against a synthetic in-memory MCB.
 *
 * Per MSDN:
 *   - OutBuffer->StartingVcn is the VCN at which the first returned run
 *     begins (may be greater than the caller's StartingVcn if that fell in
 *     the middle of a run).
 *   - Each extent's NextVcn is the VCN of the cluster immediately past
 *     the extent.  Lcn.QuadPart == -1 denotes a sparse/hole run.
 *   - STATUS_END_OF_FILE if StartingVcn lies past the last run.
 *   - STATUS_BUFFER_TOO_SMALL if the output buffer cannot hold even the
 *     header + one extent.
 *   - STATUS_BUFFER_OVERFLOW (with a partial map) if the buffer fills
 *     before the walk completes.
 *
 * Sparse-hole handling.  This driver's ConvertDataRunsToLargeMCB stores
 * sparse runs as VBN gaps rather than explicit Lbn==-1 entries (see
 * attrib.c:1872) -- so if we only enumerated MCB entries, holes would
 * silently vanish from the map.  The walker therefore tracks ExpectedVcn
 * and, whenever the next real entry's Vbn exceeds it, synthesizes a
 * hole extent.  Any trailing hole that extends past the last real entry
 * can only be detected when the caller passes AttributeLastVcn (the
 * attribute's HighestVCN); pass -1 to skip trailing-hole synthesis.
 */
NTSTATUS
NtfsFillRetrievalPointersFromMcb(PLARGE_MCB Mcb,
                                 LONGLONG StartingVcn,
                                 LONGLONG AttributeLastVcn,
                                 PRETRIEVAL_POINTERS_BUFFER OutBuffer,
                                 ULONG OutBufferLength,
                                 PULONG BytesReturned)
{
    ULONG Header = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents);
    ULONG MaxExtents;
    ULONG ExtentIndex = 0;
    ULONG RunIndex = 0;
    LONGLONG Vbn, Lbn, Count;
    LONGLONG ExpectedVcn = StartingVcn;
    BOOLEAN FoundStart = FALSE;

    if (BytesReturned != NULL)
        *BytesReturned = 0;

    if (OutBuffer == NULL || OutBufferLength < Header + sizeof(OutBuffer->Extents[0]))
        return STATUS_BUFFER_TOO_SMALL;

    MaxExtents = (OutBufferLength - Header) / sizeof(OutBuffer->Extents[0]);

    OutBuffer->ExtentCount = 0;
    OutBuffer->StartingVcn.QuadPart = 0;

    while (FsRtlGetNextLargeMcbEntry(Mcb, RunIndex, &Vbn, &Lbn, &Count))
    {
        LONGLONG RunEnd;
        LONGLONG EffectiveLcn;
        LONGLONG EffectiveStart;

        RunIndex++;

        if (Count <= 0)
            continue;

        RunEnd = Vbn + Count;

        /* Skip runs entirely before the caller's starting point, but keep
         * ExpectedVcn advancing so a later gap-check still emits any hole
         * that starts within [StartingVcn..nextEntry.Vbn). */
        if (StartingVcn >= RunEnd)
        {
            ExpectedVcn = RunEnd;
            continue;
        }

        /* Gap before this entry => sparse hole implied by VBN skip. */
        if (Vbn > ExpectedVcn)
        {
            LONGLONG HoleStart = (ExpectedVcn < StartingVcn) ? StartingVcn : ExpectedVcn;
            if (HoleStart < Vbn)
            {
                if (!FoundStart)
                {
                    OutBuffer->StartingVcn.QuadPart = HoleStart;
                    FoundStart = TRUE;
                }
                if (ExtentIndex >= MaxExtents)
                {
                    OutBuffer->ExtentCount = ExtentIndex;
                    if (BytesReturned != NULL)
                        *BytesReturned = Header + ExtentIndex * sizeof(OutBuffer->Extents[0]);
                    return STATUS_BUFFER_OVERFLOW;
                }
                OutBuffer->Extents[ExtentIndex].Lcn.QuadPart = -1;
                OutBuffer->Extents[ExtentIndex].NextVcn.QuadPart = Vbn;
                ExtentIndex++;
            }
        }

        EffectiveStart = Vbn;
        EffectiveLcn = Lbn;
        if (!FoundStart)
        {
            /* First emitted run may straddle StartingVcn. */
            if (StartingVcn > Vbn)
            {
                EffectiveStart = StartingVcn;
                if (Lbn != -1)
                    EffectiveLcn = Lbn + (StartingVcn - Vbn);
            }
            OutBuffer->StartingVcn.QuadPart = EffectiveStart;
            FoundStart = TRUE;
        }

        if (ExtentIndex >= MaxExtents)
        {
            OutBuffer->ExtentCount = ExtentIndex;
            if (BytesReturned != NULL)
                *BytesReturned = Header + ExtentIndex * sizeof(OutBuffer->Extents[0]);
            return STATUS_BUFFER_OVERFLOW;
        }

        OutBuffer->Extents[ExtentIndex].Lcn.QuadPart = EffectiveLcn;
        OutBuffer->Extents[ExtentIndex].NextVcn.QuadPart = RunEnd;
        ExtentIndex++;
        ExpectedVcn = RunEnd;
    }

    /* Trailing hole: attribute extends past the last real MCB entry. */
    if (AttributeLastVcn >= 0 && ExpectedVcn <= AttributeLastVcn)
    {
        LONGLONG HoleStart = (ExpectedVcn < StartingVcn) ? StartingVcn : ExpectedVcn;
        if (HoleStart <= AttributeLastVcn)
        {
            if (!FoundStart)
            {
                OutBuffer->StartingVcn.QuadPart = HoleStart;
                FoundStart = TRUE;
            }
            if (ExtentIndex >= MaxExtents)
            {
                OutBuffer->ExtentCount = ExtentIndex;
                if (BytesReturned != NULL)
                    *BytesReturned = Header + ExtentIndex * sizeof(OutBuffer->Extents[0]);
                return STATUS_BUFFER_OVERFLOW;
            }
            OutBuffer->Extents[ExtentIndex].Lcn.QuadPart = -1;
            OutBuffer->Extents[ExtentIndex].NextVcn.QuadPart = AttributeLastVcn + 1;
            ExtentIndex++;
        }
    }

    if (!FoundStart)
        return STATUS_END_OF_FILE;

    OutBuffer->ExtentCount = ExtentIndex;
    if (BytesReturned != NULL)
        *BytesReturned = Header + ExtentIndex * sizeof(OutBuffer->Extents[0]);
    return STATUS_SUCCESS;
}

/*
 * NtfsMoveRunInMcb
 *
 * Pure MCB surgery: relocate [StartingVcn, StartingVcn+ClusterCount) from
 * SourceLcn to TargetLcn.  Does NOT touch disk, bitmap, or file records --
 * callers (typically NtfsRelocateAttributeRange) are responsible for those
 * side effects.  Splitting a run that straddles the boundary is handled by
 * FsRtlRemoveLargeMcbEntry + re-insert via FsRtlAddLargeMcbEntry, which
 * coalesces back with neighbours where appropriate.
 *
 * SourceLcn is validated against the current mapping; if the MCB does not
 * report SourceLcn at the head of [StartingVcn, StartingVcn+ClusterCount),
 * we return STATUS_INVALID_PARAMETER rather than silently remapping
 * mismatched runs.  See Kreijstal/reactos#32.
 */
NTSTATUS
NtfsMoveRunInMcb(PLARGE_MCB Mcb,
                 LONGLONG StartingVcn,
                 LONGLONG SourceLcn,
                 LONGLONG TargetLcn,
                 ULONG ClusterCount)
{
    LONGLONG ProbeLcn = 0;
    LONGLONG CountFromLcn = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("NtfsMoveRunInMcb(Vcn=%I64d, Src=%I64d, Tgt=%I64d, Count=%lu)\n",
           StartingVcn, SourceLcn, TargetLcn, ClusterCount);

    if (ClusterCount == 0)
        return STATUS_INVALID_PARAMETER;

    /* Validate that the caller's view of the source LCN matches the MCB.
     * FsRtlLookupLargeMcbEntry returns -1 for a sparse hole -- which is
     * not relocatable via this path. */
    if (!FsRtlLookupLargeMcbEntry(Mcb, StartingVcn, &ProbeLcn, &CountFromLcn,
                                   NULL, NULL, NULL))
    {
        DPRINT1("NtfsMoveRunInMcb: VCN %I64d not mapped\n", StartingVcn);
        return STATUS_INVALID_PARAMETER;
    }

    if (ProbeLcn == -1)
    {
        DPRINT1("NtfsMoveRunInMcb: refusing to relocate sparse range @ VCN %I64d\n",
                StartingVcn);
        return STATUS_INVALID_PARAMETER;
    }

    if (ProbeLcn != SourceLcn)
    {
        DPRINT1("NtfsMoveRunInMcb: caller SourceLcn=%I64d but MCB maps VCN %I64d -> %I64d\n",
                SourceLcn, StartingVcn, ProbeLcn);
        return STATUS_INVALID_PARAMETER;
    }

    if (CountFromLcn < (LONGLONG)ClusterCount)
    {
        /* Underlying run is shorter than the slice we were asked to move.
         * The FSCTL_MOVE_FILE contract allows this -- we just move the
         * contiguous prefix -- but for the first slice we decline and let
         * the caller split the request.  Failing loud is safer than
         * partially moving and leaving the bitmap/disk out of sync. */
        DPRINT1("NtfsMoveRunInMcb: run at VCN %I64d is only %I64d clusters, caller asked %lu\n",
                StartingVcn, CountFromLcn, ClusterCount);
        return STATUS_INVALID_PARAMETER;
    }

    _SEH2_TRY
    {
        FsRtlRemoveLargeMcbEntry(Mcb, StartingVcn, (LONGLONG)ClusterCount);
        if (!FsRtlAddLargeMcbEntry(Mcb, StartingVcn, TargetLcn, (LONGLONG)ClusterCount))
        {
            ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        DPRINT1("NtfsMoveRunInMcb: MCB mutation raised 0x%lx\n", (unsigned long)Status);
    }
    _SEH2_END;

    return Status;
}

/*
 * NtfsExtendVolumeBitmap
 *
 * Minimal EXTEND_VOLUME path (see Kreijstal/reactos#32): grow the volume
 * $Bitmap's $DATA so it can describe NewClusterCount clusters, zero any
 * freshly-appended bytes, clear any trailing bits that describe LCNs past
 * NewClusterCount, and write the bitmap back.  Caller (ExtendVolume IRP
 * wrapper) is responsible for updating Vcb->NtfsInfo.ClusterCount AFTER
 * this returns success.
 *
 * TODO: This intentionally does NOT rewrite the boot sector or $Volume.
 * Real physical-disk extends need those; the first-slice contract (fresh
 * test image, no remount) does not.  Follow-up tracked in #32.
 */
NTSTATUS
NtfsExtendVolumeBitmap(PDEVICE_EXTENSION Vcb,
                       ULONGLONG OldClusterCount,
                       ULONGLONG NewClusterCount)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER BitmapRecord = NULL;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    ULONG BitmapAttrOffset = 0;
    ULONGLONG OldBitmapSize;
    ULONGLONG NewBitmapSize;
    ULONGLONG AllocatedSize;
    PUCHAR BitmapData = NULL;
    RTL_BITMAP Bitmap;
    ULONG LengthWritten = 0;
    BOOLEAN BitmapLockHeld = FALSE;

    DPRINT("NtfsExtendVolumeBitmap(%p, Old=%I64u, New=%I64u)\n",
           Vcb, OldClusterCount, NewClusterCount);

    if (NewClusterCount <= OldClusterCount)
        return STATUS_INVALID_PARAMETER;

    /* Each byte of $Bitmap describes 8 clusters.  Round up. */
    OldBitmapSize = (OldClusterCount + 7) / 8;
    NewBitmapSize = (NewClusterCount + 7) / 8;

    BitmapLockHeld = ExAcquireResourceExclusiveLite(&Vcb->BitmapResource, TRUE);

    BitmapRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Status = ReadFileRecord(Vcb, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = FindAttribute(Vcb, BitmapRecord, AttributeData, L"", 0, &DataContext, &BitmapAttrOffset);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (!DataContext->pRecord->IsNonResident)
    {
        /* The driver formats $Bitmap non-resident; a resident bitmap would
         * require an entirely different resize path (grow-then-convert).
         * Bail here rather than paper over an unexpected on-disk layout. */
        DPRINT1("NtfsExtendVolumeBitmap: $Bitmap is resident, cannot grow\n");
        Status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }

    /* AllocatedSize covers entire allocated clusters; DataSize is the
     * logical bitmap length we round-trip through.  Grow whichever limits
     * us. */
    AllocatedSize = (ULONGLONG)DataContext->pRecord->NonResident.AllocatedSize;
    if (AllocatedSize < NewBitmapSize)
    {
        /* Need to append clusters.  AddRun allocates, splices the MCB, and
         * rewrites the mapping pairs + file record.  We allocate from the
         * tail (past OldClusterCount) so the new $Bitmap-backing clusters
         * are themselves marked allocated when we write the bitmap below. */
        ULONG ClustersNeeded;
        ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
        ULONG FirstAssignedCluster = 0;
        ULONG AssignedClusters = 0;
        ULONGLONG NeededAllocatedSize =
            ((NewBitmapSize + BytesPerCluster - 1) / BytesPerCluster) * BytesPerCluster;

        ClustersNeeded = (ULONG)((NeededAllocatedSize - AllocatedSize) / BytesPerCluster);
        while (ClustersNeeded > 0)
        {
            Status = NtfsAllocateClusters(Vcb,
                                          (ULONG)OldClusterCount,
                                          ClustersNeeded,
                                          &FirstAssignedCluster,
                                          &AssignedClusters);
            if (!NT_SUCCESS(Status) || AssignedClusters == 0)
            {
                DPRINT1("NtfsExtendVolumeBitmap: NtfsAllocateClusters failed 0x%lx\n",
                        (unsigned long)Status);
                if (NT_SUCCESS(Status))
                    Status = STATUS_DISK_FULL;
                goto Cleanup;
            }

            Status = AddRun(Vcb, DataContext, BitmapAttrOffset, BitmapRecord,
                            FirstAssignedCluster, AssignedClusters);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("NtfsExtendVolumeBitmap: AddRun failed 0x%lx\n",
                        (unsigned long)Status);
                goto Cleanup;
            }

            ClustersNeeded -= AssignedClusters;
        }

        /* Update logical sizes on the attribute record now that the run
         * list has grown. */
        DataContext->pRecord->NonResident.AllocatedSize = (LONGLONG)NeededAllocatedSize;
        DataContext->pRecord->NonResident.DataSize = (LONGLONG)NewBitmapSize;
        DataContext->pRecord->NonResident.InitializedSize = (LONGLONG)NewBitmapSize;

        /* Mirror into FileRecord's slot so UpdateFileRecord picks it up
         * -- AddRun already wrote the record back, but we changed three
         * more fields after that. */
        {
            PNTFS_ATTR_RECORD Slot = (PNTFS_ATTR_RECORD)((ULONG_PTR)BitmapRecord + BitmapAttrOffset);
            Slot->NonResident.AllocatedSize = DataContext->pRecord->NonResident.AllocatedSize;
            Slot->NonResident.DataSize = DataContext->pRecord->NonResident.DataSize;
            Slot->NonResident.InitializedSize = DataContext->pRecord->NonResident.InitializedSize;
        }

        Status = UpdateFileRecord(Vcb, NTFS_FILE_BITMAP, BitmapRecord);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }
    else if ((ULONGLONG)DataContext->pRecord->NonResident.DataSize < NewBitmapSize)
    {
        /* Pre-existing slack in the allocated clusters is enough; just
         * bump DataSize/InitializedSize so WriteAttribute accepts the
         * extended range below. */
        DataContext->pRecord->NonResident.DataSize = (LONGLONG)NewBitmapSize;
        DataContext->pRecord->NonResident.InitializedSize = (LONGLONG)NewBitmapSize;

        {
            PNTFS_ATTR_RECORD Slot = (PNTFS_ATTR_RECORD)((ULONG_PTR)BitmapRecord + BitmapAttrOffset);
            Slot->NonResident.DataSize = DataContext->pRecord->NonResident.DataSize;
            Slot->NonResident.InitializedSize = DataContext->pRecord->NonResident.InitializedSize;
        }

        Status = UpdateFileRecord(Vcb, NTFS_FILE_BITMAP, BitmapRecord);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }

    /* Read the bitmap contents into memory; then zero any appended bytes
     * and clear any trailing bits past NewClusterCount. */
    BitmapData = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)NewBitmapSize, TAG_NTFS);
    if (BitmapData == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    RtlZeroMemory(BitmapData, (SIZE_T)NewBitmapSize);
    if (OldBitmapSize > 0)
    {
        ULONG BytesToRead = (ULONG)min(OldBitmapSize, NewBitmapSize);
        ReadAttribute(Vcb, DataContext, 0, (PCHAR)BitmapData, BytesToRead);
    }

    /* Clear trailing bits in the byte that straddles NewClusterCount. */
    if ((NewClusterCount % 8) != 0 && NewBitmapSize > 0)
    {
        ULONG Byte = (ULONG)((NewClusterCount / 8));
        UCHAR Mask = (UCHAR)((1U << (NewClusterCount % 8)) - 1);
        BitmapData[Byte] &= Mask;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, (ULONG)NewClusterCount);
    (void)Bitmap; /* retained for symmetry with NtfsAllocateClusters/NtfsGetFreeClusters */

    Status = WriteAttribute(Vcb, DataContext, 0, BitmapData,
                            (ULONG)NewBitmapSize, &LengthWritten, BitmapRecord);

Cleanup:
    if (BitmapData != NULL)
        ExFreePoolWithTag(BitmapData, TAG_NTFS);
    if (DataContext != NULL)
        ReleaseAttributeContext(DataContext);
    if (BitmapRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);
    if (BitmapLockHeld)
        ExReleaseResourceLite(&Vcb->BitmapResource);
    return Status;
}

/*
 * NtfsRelocateAttributeRange
 *
 * Orchestrator for FSCTL_MOVE_FILE (see Kreijstal/reactos#32): copy the
 * ClusterCount clusters at [StartingVcn ..] from their current LCNs to
 * TargetLcn, update the volume $Bitmap in one R/M/W cycle, then rewrite
 * the attribute's mapping pairs to point at TargetLcn.
 *
 * Deliberate limits for the first slice:
 *   - Compressed and encrypted attributes -> STATUS_NOT_SUPPORTED.
 *     Their mapping-pairs format carries extra metadata the MCB surgery
 *     below does not preserve; we do not attempt to work around that.
 *   - The source range must resolve to a single contiguous LCN run in
 *     the MCB.  NtfsMoveRunInMcb enforces this by calling
 *     FsRtlLookupLargeMcbEntry first; callers issuing oversized requests
 *     get STATUS_INVALID_PARAMETER.
 *   - We do not touch $LogFile; all writes land on disk directly, same
 *     as every other mutation the driver currently performs.
 */
NTSTATUS
NtfsRelocateAttributeRange(PDEVICE_EXTENSION Vcb,
                           PNTFS_ATTR_CONTEXT AttrContext,
                           ULONG AttrOffset,
                           PFILE_RECORD_HEADER FileRecord,
                           LONGLONG StartingVcn,
                           LONGLONG TargetLcn,
                           ULONG ClusterCount)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER BitmapRecord = NULL;
    PNTFS_ATTR_CONTEXT BitmapContext = NULL;
    PUCHAR BitmapData = NULL;
    PUCHAR CopyBuffer = NULL;
    ULONGLONG BitmapDataSize = 0;
    ULONGLONG CopyBytes;
    ULONG BytesPerCluster;
    ULONG LengthWritten = 0;
    BOOLEAN BitmapLockHeld = FALSE;
    LONGLONG SourceLcn = 0;
    LONGLONG CountFromLcn = 0;
    RTL_BITMAP Bitmap;
    PNTFS_ATTR_RECORD Slot;
    PUCHAR RunBuffer = NULL;
    ULONG RunBufferSize = 0;
    ULONG DataRunMaxLength;

    DPRINT("NtfsRelocateAttributeRange(Vcn=%I64d, Tgt=%I64d, Count=%lu)\n",
           StartingVcn, TargetLcn, ClusterCount);

    if (ClusterCount == 0 || AttrContext == NULL || FileRecord == NULL)
        return STATUS_INVALID_PARAMETER;

    if (!AttrContext->pRecord->IsNonResident)
        return STATUS_INVALID_PARAMETER;

    /* Reject compressed / encrypted attributes.  The mapping-pairs format
     * for compressed runs encodes compression-unit boundaries in a way
     * that would not survive this MCB-surgery approach, and encrypted
     * content must be re-encrypted under the destination key, which we
     * have no access to.  See Kreijstal/reactos#32. */
    if (AttrContext->pRecord->Flags != 0)
    {
        DPRINT1("NtfsRelocateAttributeRange: attribute has Flags=0x%x (compressed/encrypted), refusing\n",
                AttrContext->pRecord->Flags);
        return STATUS_NOT_SUPPORTED;
    }

    /* Look up the source LCN and verify the range is single-run. */
    if (!FsRtlLookupLargeMcbEntry(&AttrContext->DataRunsMCB,
                                   StartingVcn, &SourceLcn, &CountFromLcn,
                                   NULL, NULL, NULL))
    {
        DPRINT1("NtfsRelocateAttributeRange: VCN %I64d not mapped\n", StartingVcn);
        return STATUS_INVALID_PARAMETER;
    }

    if (SourceLcn == -1 || CountFromLcn < (LONGLONG)ClusterCount)
    {
        DPRINT1("NtfsRelocateAttributeRange: bad source range (Lcn=%I64d, Count=%I64d)\n",
                SourceLcn, CountFromLcn);
        return STATUS_INVALID_PARAMETER;
    }

    BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    CopyBytes = (ULONGLONG)ClusterCount * BytesPerCluster;
    if (CopyBytes > 0xFFFFFFFFULL)
        return STATUS_INVALID_PARAMETER;

    /* --- Phase 1: read source clusters straight from disk --------------- */
    CopyBuffer = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)CopyBytes, TAG_NTFS);
    if (CopyBuffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsReadDisk(Vcb->StorageDevice,
                          (LONGLONG)SourceLcn * BytesPerCluster,
                          (ULONG)CopyBytes,
                          Vcb->NtfsInfo.BytesPerSector,
                          CopyBuffer,
                          FALSE);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* --- Phase 2: bitmap R/M/W: mark target allocated, source free ----- */
    BitmapLockHeld = ExAcquireResourceExclusiveLite(&Vcb->BitmapResource, TRUE);

    BitmapRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Status = ReadFileRecord(Vcb, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = FindAttribute(Vcb, BitmapRecord, AttributeData, L"", 0, &BitmapContext, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    BitmapDataSize = AttributeDataLength(BitmapContext->pRecord);
    if (BitmapDataSize == 0 || BitmapDataSize > 0x7FFFFFFFULL)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    BitmapData = ExAllocatePoolWithTag(NonPagedPool,
                                       ROUND_UP((SIZE_T)BitmapDataSize, Vcb->NtfsInfo.BytesPerSector),
                                       TAG_NTFS);
    if (BitmapData == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    ReadAttribute(Vcb, BitmapContext, 0, (PCHAR)BitmapData, (ULONG)BitmapDataSize);

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, Vcb->NtfsInfo.ClusterCount);

    /* Verify target extent is currently clear. */
    {
        ULONG i;
        for (i = 0; i < ClusterCount; i++)
        {
            if (RtlCheckBit(&Bitmap, (ULONG)(TargetLcn + i)))
            {
                DPRINT1("NtfsRelocateAttributeRange: target LCN %I64d is not free\n",
                        TargetLcn + i);
                Status = STATUS_INVALID_PARAMETER;
                goto Cleanup;
            }
        }
    }

    RtlSetBits(&Bitmap, (ULONG)TargetLcn, ClusterCount);
    RtlClearBits(&Bitmap, (ULONG)SourceLcn, ClusterCount);

    Status = WriteAttribute(Vcb, BitmapContext, 0, BitmapData,
                            (ULONG)BitmapDataSize, &LengthWritten, BitmapRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (BitmapLockHeld)
    {
        ExReleaseResourceLite(&Vcb->BitmapResource);
        BitmapLockHeld = FALSE;
    }

    /* --- Phase 3: raw-disk copy to TargetLcn --------------------------- *
     * We go through NtfsWriteDisk rather than WriteAttribute because the
     * attribute's runlist still points at SourceLcn at this moment --
     * WriteAttribute would simply overwrite the source we just read. */
    Status = NtfsWriteDisk(Vcb->StorageDevice,
                           (LONGLONG)TargetLcn * BytesPerCluster,
                           (ULONG)CopyBytes,
                           Vcb->NtfsInfo.BytesPerSector,
                           CopyBuffer);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* --- Phase 4: splice the MCB + re-encode mapping pairs ------------- */
    Status = NtfsMoveRunInMcb(&AttrContext->DataRunsMCB,
                              StartingVcn, SourceLcn, TargetLcn, ClusterCount);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RunBuffer = ExAllocatePoolWithTag(NonPagedPool,
                                      Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (RunBuffer == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Status = ConvertLargeMCBToDataRuns(&AttrContext->DataRunsMCB, RunBuffer,
                                       Vcb->NtfsInfo.BytesPerFileRecord,
                                       &RunBufferSize);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Slot = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);

    DataRunMaxLength = Slot->Length - Slot->NonResident.MappingPairsOffset;
    if (RunBufferSize > DataRunMaxLength)
    {
        /* The relocated runlist doesn't fit in the slot's mapping-pairs
         * region.  Growing the slot / splitting to an $ATTRIBUTE_LIST is
         * doable via the AddRun trailing-attribute logic, but that path
         * is tuned for APPENDING not REPLACING runs.  Defer -- first
         * slice only handles fits-in-place. */
        DPRINT1("NtfsRelocateAttributeRange: new mapping pairs (%lu) don't fit in slot (%lu)\n",
                RunBufferSize, DataRunMaxLength);
        Status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }

    /* Clear the old mapping pairs first (zero the tail) so stale bytes
     * don't poison DecodeRun on next mount. */
    RtlZeroMemory((PUCHAR)Slot + Slot->NonResident.MappingPairsOffset,
                  DataRunMaxLength);
    RtlCopyMemory((PUCHAR)Slot + Slot->NonResident.MappingPairsOffset,
                  RunBuffer, RunBufferSize);

    /* Keep the AttrContext's private copy in sync. */
    RtlZeroMemory((PUCHAR)AttrContext->pRecord + AttrContext->pRecord->NonResident.MappingPairsOffset,
                  AttrContext->pRecord->Length - AttrContext->pRecord->NonResident.MappingPairsOffset);
    RtlCopyMemory((PUCHAR)AttrContext->pRecord + AttrContext->pRecord->NonResident.MappingPairsOffset,
                  RunBuffer, RunBufferSize);

    Status = UpdateFileRecord(Vcb, FileRecord->MFTRecordNumber, FileRecord);

Cleanup:
    if (RunBuffer != NULL)
        ExFreePoolWithTag(RunBuffer, TAG_NTFS);
    if (BitmapData != NULL)
        ExFreePoolWithTag(BitmapData, TAG_NTFS);
    if (BitmapContext != NULL)
        ReleaseAttributeContext(BitmapContext);
    if (BitmapRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);
    if (CopyBuffer != NULL)
        ExFreePoolWithTag(CopyBuffer, TAG_NTFS);
    if (BitmapLockHeld)
        ExReleaseResourceLite(&Vcb->BitmapResource);
    return Status;
}

PUCHAR
DecodeRun(PUCHAR DataRun,
          LONGLONG *DataRunOffset,
          ULONGLONG *DataRunLength)
{
    UCHAR DataRunOffsetSize;
    UCHAR DataRunLengthSize;
    CHAR i;

    DataRunOffsetSize = (*DataRun >> 4) & 0xF;
    DataRunLengthSize = *DataRun & 0xF;
    *DataRunOffset = 0;
    *DataRunLength = 0;
    DataRun++;
    for (i = 0; i < DataRunLengthSize; i++)
    {
        *DataRunLength += ((ULONG64)*DataRun) << (i * 8);
        DataRun++;
    }

    /* NTFS 3+ sparse files */
    if (DataRunOffsetSize == 0)
    {
        *DataRunOffset = -1;
    }
    else
    {
        for (i = 0; i < DataRunOffsetSize - 1; i++)
        {
            *DataRunOffset += ((ULONG64)*DataRun) << (i * 8);
            DataRun++;
        }
        /* The last byte contains sign so we must process it different way. */
        *DataRunOffset = ((LONG64)(CHAR)(*(DataRun++)) << (i * 8)) + *DataRunOffset;
    }

    DPRINT("DataRunOffsetSize: %x\n", DataRunOffsetSize);
    DPRINT("DataRunLengthSize: %x\n", DataRunLengthSize);
    DPRINT("DataRunOffset: %x\n", *DataRunOffset);
    DPRINT("DataRunLength: %x\n", *DataRunLength);

    return DataRun;
}

BOOLEAN
FindRun(PNTFS_ATTR_RECORD NresAttr,
        ULONGLONG vcn,
        PULONGLONG lcn,
        PULONGLONG count)
{
    if (vcn < NresAttr->NonResident.LowestVCN || vcn > NresAttr->NonResident.HighestVCN)
        return FALSE;

    DecodeRun((PUCHAR)((ULONG_PTR)NresAttr + NresAttr->NonResident.MappingPairsOffset), (PLONGLONG)lcn, count);

    return TRUE;
}

/**
* @name FreeClusters
* @implemented
*
* Shrinks the allocation size of a non-resident attribute by a given number of clusters.
* Frees the clusters from the volume's $BITMAP file as well as the attribute's data runs.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param AttrContext
* Pointer to an NTFS_ATTR_CONTEXT describing the attribute from which the clusters will be freed.
*
* @param AttrOffset
* Byte offset of the destination attribute relative to its file record.
*
* @param FileRecord
* Pointer to a complete copy of the file record containing the attribute. Must be at least
* Vcb->NtfsInfo.BytesPerFileRecord bytes long.
*
* @param ClustersToFree
* Number of clusters that should be freed from the end of the data stream. Must be no more
* Than the number of clusters assigned to the attribute (HighestVCN + 1).
*
* @return
* STATUS_SUCCESS on success. STATUS_INVALID_PARAMETER if AttrContext describes a resident attribute,
* or if the caller requested more clusters be freed than the attribute has been allocated.
* STATUS_INSUFFICIENT_RESOURCES if allocating a buffer for the data runs fails or
* if ConvertDataRunsToLargeMCB() fails.
* STATUS_BUFFER_TOO_SMALL if ConvertLargeMCBToDataRuns() fails.
*
*
*/
NTSTATUS
FreeClusters(PNTFS_VCB Vcb,
             PNTFS_ATTR_CONTEXT AttrContext,
             ULONG AttrOffset,
             PFILE_RECORD_HEADER FileRecord,
             ULONG ClustersToFree)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG ClustersLeftToFree = ClustersToFree;

    PNTFS_ATTR_RECORD DestinationAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONG NextAttributeOffset = AttrOffset + AttrContext->pRecord->Length;
    PNTFS_ATTR_RECORD NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + NextAttributeOffset);

    PUCHAR RunBuffer;
    ULONG RunBufferSize = 0;

    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONGLONG BitmapDataSize;
    PUCHAR BitmapData;
    RTL_BITMAP Bitmap;
    ULONG LengthWritten;
    BOOLEAN BitmapLockHeld;

    if (!AttrContext->pRecord->IsNonResident)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Same race as NtfsAllocateClusters: an unsynchronized R/M/W of the
     * volume bitmap lets a concurrent allocator hand out an LCN we are
     * about to mark free, leaving two attributes pointing at the same
     * cluster. Hold BitmapResource exclusive across the entire function;
     * the runlist updates that follow the bitmap writeback don't need
     * the lock, but releasing mid-function would require an extra exit
     * label and the contention is low enough that holding it through is
     * cheaper than the bookkeeping. See ntfs.h:BitmapResource. */
    BitmapLockHeld = ExAcquireResourceExclusiveLite(&Vcb->BitmapResource, TRUE);

    // Read the $Bitmap file
    BitmapRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        DPRINT1("Error: Unable to allocate memory for bitmap file record!\n");
        if (BitmapLockHeld)
            ExReleaseResourceLite(&Vcb->BitmapResource);
        return STATUS_NO_MEMORY;
    }

    Status = ReadFileRecord(Vcb, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Error: Unable to read file record for bitmap!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&Vcb->BitmapResource);
        return 0;
    }

    Status = FindAttribute(Vcb, BitmapRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Error: Unable to find data attribute for bitmap file!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&Vcb->BitmapResource);
        return 0;
    }

    BitmapDataSize = AttributeDataLength(DataContext->pRecord);
    BitmapDataSize = min(BitmapDataSize, ULONG_MAX);
    ASSERT((BitmapDataSize * 8) >= Vcb->NtfsInfo.ClusterCount);
    BitmapData = ExAllocatePoolWithTag(NonPagedPool, ROUND_UP(BitmapDataSize, Vcb->NtfsInfo.BytesPerSector), TAG_NTFS);
    if (BitmapData == NULL)
    {
        DPRINT1("Error: Unable to allocate memory for bitmap file data!\n");
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&Vcb->BitmapResource);
        return 0;
    }

    ReadAttribute(Vcb, DataContext, 0, (PCHAR)BitmapData, (ULONG)BitmapDataSize);

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, Vcb->NtfsInfo.ClusterCount);

    /* free clusters in $BITMAP file. Track HighestVCN as signed so it can
     * step from 0 to -1 (sentinel for "no clusters") on the final iteration
     * without unsigned wrap. */
    {
        LONGLONG NextHighestVcn = (LONGLONG)AttrContext->pRecord->NonResident.HighestVCN;

        while (ClustersLeftToFree > 0)
        {
            LONGLONG LargeVbn, LargeLbn;

            if (!FsRtlLookupLastLargeMcbEntry(&AttrContext->DataRunsMCB, &LargeVbn, &LargeLbn))
            {
                Status = STATUS_INVALID_PARAMETER;
                DPRINT1("DRIVER ERROR: FreeClusters called to free %lu clusters, which is %lu more clusters than are assigned to attribute!\n",
                        ClustersToFree,
                        ClustersLeftToFree);
                ASSERT(FALSE);
                break;
            }

            if (LargeLbn != -1)
            {
                // deallocate this cluster
                RtlClearBits(&Bitmap, LargeLbn, 1);
            }
            FsRtlTruncateLargeMcb(&AttrContext->DataRunsMCB, NextHighestVcn);

            NextHighestVcn--;
            ClustersLeftToFree--;
        }

        AttrContext->pRecord->NonResident.HighestVCN = (ULONGLONG)NextHighestVcn;
    }

    // update $BITMAP file on disk
    Status = WriteAttribute(Vcb, DataContext, 0, BitmapData, (ULONG)BitmapDataSize, &LengthWritten, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(DataContext);
        ExFreePoolWithTag(BitmapData, TAG_NTFS);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&Vcb->BitmapResource);
        return Status;
    }

    ReleaseAttributeContext(DataContext);
    ExFreePoolWithTag(BitmapData, TAG_NTFS);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BitmapRecord);

    // Save updated data runs to file record

    // Allocate some memory for a new RunBuffer
    RunBuffer = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (!RunBuffer)
    {
        DPRINT1("ERROR: Couldn't allocate memory for data runs!\n");
        if (BitmapLockHeld)
            ExReleaseResourceLite(&Vcb->BitmapResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Convert the map control block back to encoded data runs
    /* A truncated runlist here would strand the clusters we just freed from the
     * MCB but did not re-encode; see the matching check in AddRun. */
    Status = ConvertLargeMCBToDataRuns(&AttrContext->DataRunsMCB, RunBuffer, Vcb->NtfsInfo.BytesPerFileRecord, &RunBufferSize);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to convert MCB to data runs (0x%x)!\n", (unsigned)Status);
        ExFreePoolWithTag(RunBuffer, TAG_NTFS);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&Vcb->BitmapResource);
        return Status;
    }

    // Update HighestVCN
    DestinationAttribute->NonResident.HighestVCN = AttrContext->pRecord->NonResident.HighestVCN;

    // Write data runs to destination attribute
    RtlCopyMemory((PVOID)((ULONG_PTR)DestinationAttribute + DestinationAttribute->NonResident.MappingPairsOffset),
                  RunBuffer,
                  RunBufferSize);

    // Is DestinationAttribute the last attribute in the file record?
    if (NextAttribute->Type == AttributeEnd)
    {
        // update attribute length
        DestinationAttribute->Length = ALIGN_UP_BY(AttrContext->pRecord->NonResident.MappingPairsOffset + RunBufferSize,
                                                 ATTR_RECORD_ALIGNMENT);

        ASSERT(DestinationAttribute->Length <= AttrContext->pRecord->Length);

        AttrContext->pRecord->Length = DestinationAttribute->Length;

        // write end markers
        NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)DestinationAttribute + DestinationAttribute->Length);
        SetFileRecordEnd(FileRecord, NextAttribute, FILE_RECORD_END);
    }

    // Update the file record
    Status = UpdateFileRecord(Vcb, AttrContext->FileMFTIndex, FileRecord);

    ExFreePoolWithTag(RunBuffer, TAG_NTFS);

    NtfsDumpDataRuns((PUCHAR)((ULONG_PTR)DestinationAttribute + DestinationAttribute->NonResident.MappingPairsOffset), 0);

    if (BitmapLockHeld)
        ExReleaseResourceLite(&Vcb->BitmapResource);

    /* Cluster state changed; invalidate the cached free-clusters count. */
    InterlockedExchange(&Vcb->CachedFreeClustersValid, 0);

    return Status;
}

static
NTSTATUS
InternalReadNonResidentAttributes(PFIND_ATTR_CONTXT Context)
{
    ULONGLONG ListSize;
    PNTFS_ATTR_RECORD Attribute;
    PNTFS_ATTR_CONTEXT ListContext;

    DPRINT("InternalReadNonResidentAttributes(%p)\n", Context);

    Attribute = Context->CurrAttr;
    ASSERT(Attribute->Type == AttributeAttributeList);

    if (Context->OnlyResident)
    {
        Context->NonResidentStart = NULL;
        Context->NonResidentEnd = NULL;
        return STATUS_SUCCESS;
    }

    if (Context->NonResidentStart != NULL)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ListContext = PrepareAttributeContext(Attribute);
    if (ListContext == NULL)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ListSize = AttributeDataLength(ListContext->pRecord);
    if (ListSize > 0xFFFFFFFF)
    {
        ReleaseAttributeContext(ListContext);
        return STATUS_BUFFER_OVERFLOW;
    }

    Context->NonResidentStart = ExAllocatePoolWithTag(NonPagedPool, (ULONG)ListSize, TAG_NTFS);
    if (Context->NonResidentStart == NULL)
    {
        ReleaseAttributeContext(ListContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ReadAttribute(Context->Vcb, ListContext, 0, (PCHAR)Context->NonResidentStart, (ULONG)ListSize) != ListSize)
    {
        ExFreePoolWithTag(Context->NonResidentStart, TAG_NTFS);
        Context->NonResidentStart = NULL;
        ReleaseAttributeContext(ListContext);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ReleaseAttributeContext(ListContext);
    Context->NonResidentEnd = (PNTFS_ATTRIBUTE_LIST_ITEM)((PCHAR)Context->NonResidentStart + ListSize);
    return STATUS_SUCCESS;
}

static
PNTFS_ATTRIBUTE_LIST_ITEM
InternalGetNextAttributeListItem(PFIND_ATTR_CONTXT Context)
{
    PNTFS_ATTRIBUTE_LIST_ITEM NextItem;

    if (Context->NonResidentCur == (PVOID)-1)
    {
        return NULL;
    }

    if (Context->NonResidentCur == NULL || Context->NonResidentCur->Type == AttributeEnd)
    {
        Context->NonResidentCur = (PVOID)-1;
        return NULL;
    }

    if (Context->NonResidentCur->Length == 0)
    {
        DPRINT1("Broken length list entry length !");
        Context->NonResidentCur = (PVOID)-1;
        return NULL;
    }

    NextItem = (PNTFS_ATTRIBUTE_LIST_ITEM)((PCHAR)Context->NonResidentCur + Context->NonResidentCur->Length);

    /* Bounds check FIRST - the next item may be exactly at NonResidentEnd
     * (one-past-the-last byte) when the previous item was the last in the list.
     * The original code dereferenced NextItem->Length/Type before validating
     * the pointer, which is OOB when the list contains exactly the right number
     * of entries to fill the attribute (no AttributeEnd sentinel needed). */
    if (NextItem < Context->NonResidentStart ||
        (PCHAR)NextItem + sizeof(*NextItem) > (PCHAR)Context->NonResidentEnd)
    {
        Context->NonResidentCur = (PVOID)-1;
        return NULL;
    }

    if (NextItem->Length == 0 || NextItem->Type == AttributeEnd)
    {
        Context->NonResidentCur = (PVOID)-1;
        return NULL;
    }

    Context->NonResidentCur = NextItem;
    return NextItem;
}

NTSTATUS
FindFirstAttributeListItem(PFIND_ATTR_CONTXT Context,
                           PNTFS_ATTRIBUTE_LIST_ITEM *Item)
{
    if (Context->NonResidentStart == NULL || Context->NonResidentStart->Type == AttributeEnd)
    {
        return STATUS_UNSUCCESSFUL;
    }

    Context->NonResidentCur = Context->NonResidentStart;
    *Item = Context->NonResidentCur;
    return STATUS_SUCCESS;
}

NTSTATUS
FindNextAttributeListItem(PFIND_ATTR_CONTXT Context,
                          PNTFS_ATTRIBUTE_LIST_ITEM *Item)
{
    *Item = InternalGetNextAttributeListItem(Context);
    if (*Item == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static
PNTFS_ATTR_RECORD
InternalGetNextAttribute(PFIND_ATTR_CONTXT Context)
{
    PNTFS_ATTR_RECORD NextAttribute;

    if (Context->CurrAttr == (PVOID)-1)
    {
        return NULL;
    }

    if (Context->CurrAttr >= Context->FirstAttr &&
        Context->CurrAttr < Context->LastAttr)
    {
        if (Context->CurrAttr->Length == 0)
        {
            DPRINT1("Broken length!\n");
            Context->CurrAttr = (PVOID)-1;
            return NULL;
        }

        NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Context->CurrAttr + Context->CurrAttr->Length);

        if (NextAttribute > Context->LastAttr || NextAttribute < Context->FirstAttr)
        {
            DPRINT1("Broken length: 0x%lx!\n", Context->CurrAttr->Length);
            Context->CurrAttr = (PVOID)-1;
            return NULL;
        }

        Context->Offset += ((ULONG_PTR)NextAttribute - (ULONG_PTR)Context->CurrAttr);
        Context->CurrAttr = NextAttribute;

        if (Context->CurrAttr < Context->LastAttr &&
            Context->CurrAttr->Type != AttributeEnd)
        {
            return Context->CurrAttr;
        }
    }

    if (Context->NonResidentStart == NULL)
    {
        Context->CurrAttr = (PVOID)-1;
        return NULL;
    }

    Context->CurrAttr = (PVOID)-1;
    return NULL;
}

NTSTATUS
FindFirstAttribute(PFIND_ATTR_CONTXT Context,
                   PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   BOOLEAN OnlyResident,
                   PNTFS_ATTR_RECORD * Attribute)
{
    NTSTATUS Status;

    DPRINT("INSTRUMENT: FindFirstAttribute entering Vcb=%p FileRecord=%p OnlyResident=%d AttrOffset=%u\n",
            Vcb, FileRecord, OnlyResident, FileRecord->AttributeOffset);

    DPRINT("FindFistAttribute(%p, %p, %p, %p, %u, %p)\n", Context, Vcb, FileRecord, OnlyResident, Attribute);

    Context->Vcb = Vcb;
    Context->OnlyResident = OnlyResident;
    Context->FirstAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
    Context->CurrAttr = Context->FirstAttr;
    Context->LastAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->BytesInUse);
    Context->NonResidentStart = NULL;
    Context->NonResidentEnd = NULL;
    Context->Offset = FileRecord->AttributeOffset;

    if (Context->FirstAttr->Type == AttributeEnd)
    {
        Context->CurrAttr = (PVOID)-1;
        return STATUS_END_OF_FILE;
    }
    else if (Context->FirstAttr->Type == AttributeAttributeList)
    {
        Status = InternalReadNonResidentAttributes(Context);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        *Attribute = InternalGetNextAttribute(Context);
        if (*Attribute == NULL)
        {
            return STATUS_END_OF_FILE;
        }
    }
    else
    {
        *Attribute = Context->CurrAttr;
        Context->Offset = (UCHAR*)Context->CurrAttr - (UCHAR*)FileRecord;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
FindNextAttribute(PFIND_ATTR_CONTXT Context,
                  PNTFS_ATTR_RECORD * Attribute)
{
    NTSTATUS Status;

    DPRINT("INSTRUMENT: FindNextAttribute entering CurrAttr=%p Type=0x%x Offset=%u\n",
            Context->CurrAttr, Context->CurrAttr ? Context->CurrAttr->Type : 0, Context->Offset);

    DPRINT("FindNextAttribute(%p, %p)\n", Context, Attribute);

    *Attribute = InternalGetNextAttribute(Context);
    if (*Attribute == NULL)
    {
        return STATUS_END_OF_FILE;
    }

    if (Context->CurrAttr->Type != AttributeAttributeList)
    {
        return STATUS_SUCCESS;
    }

    Status = InternalReadNonResidentAttributes(Context);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    *Attribute = InternalGetNextAttribute(Context);
    if (*Attribute == NULL)
    {
        return STATUS_END_OF_FILE;
    }

    return STATUS_SUCCESS;
}

VOID
FindCloseAttribute(PFIND_ATTR_CONTXT Context)
{
    if (Context->NonResidentStart != NULL)
    {
        ExFreePoolWithTag(Context->NonResidentStart, TAG_NTFS);
        Context->NonResidentStart = NULL;
    }
}

static
VOID
NtfsDumpFileNameAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PFILENAME_ATTRIBUTE FileNameAttr;

    DbgPrint("  $FILE_NAME ");

//    DbgPrint(" Length %lu  Offset %hu ", Attribute->Resident.ValueLength, Attribute->Resident.ValueOffset);

    FileNameAttr = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
    DbgPrint(" (%x) '%.*S' ", FileNameAttr->NameType, FileNameAttr->NameLength, FileNameAttr->Name);
    DbgPrint(" '%x' \n", FileNameAttr->FileAttributes);
    DbgPrint(" AllocatedSize: %I64u\nDataSize: %I64u\n", FileNameAttr->AllocatedSize, FileNameAttr->DataSize);
    DbgPrint(" File reference: 0x%016I64x\n", FileNameAttr->DirectoryFileReferenceNumber);
}


static
VOID
NtfsDumpStandardInformationAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PSTANDARD_INFORMATION StandardInfoAttr;

    DbgPrint("  $STANDARD_INFORMATION ");

//    DbgPrint(" Length %lu  Offset %hu ", Attribute->Resident.ValueLength, Attribute->Resident.ValueOffset);

    StandardInfoAttr = (PSTANDARD_INFORMATION)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
    DbgPrint(" '%x' ", StandardInfoAttr->FileAttribute);
}


static
VOID
NtfsDumpVolumeNameAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PWCHAR VolumeName;

    DbgPrint("  $VOLUME_NAME ");

//    DbgPrint(" Length %lu  Offset %hu ", Attribute->Resident.ValueLength, Attribute->Resident.ValueOffset);

    VolumeName = (PWCHAR)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
    DbgPrint(" '%.*S' ", Attribute->Resident.ValueLength / sizeof(WCHAR), VolumeName);
}


static
VOID
NtfsDumpVolumeInformationAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PVOLINFO_ATTRIBUTE VolInfoAttr;

    DbgPrint("  $VOLUME_INFORMATION ");

//    DbgPrint(" Length %lu  Offset %hu ", Attribute->Resident.ValueLength, Attribute->Resident.ValueOffset);

    VolInfoAttr = (PVOLINFO_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
    DbgPrint(" NTFS Version %u.%u  Flags 0x%04hx ",
             VolInfoAttr->MajorVersion,
             VolInfoAttr->MinorVersion,
             VolInfoAttr->Flags);
}


static
VOID
NtfsDumpIndexRootAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PINDEX_ROOT_ATTRIBUTE IndexRootAttr;
    ULONG CurrentOffset;
    ULONG CurrentNode;

    IndexRootAttr = (PINDEX_ROOT_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);

    if (IndexRootAttr->AttributeType == AttributeFileName)
        ASSERT(IndexRootAttr->CollationRule == COLLATION_FILE_NAME);

    DbgPrint("  $INDEX_ROOT (%u bytes per index record, %u clusters) ", IndexRootAttr->SizeOfEntry, IndexRootAttr->ClustersPerIndexRecord);

    if (IndexRootAttr->Header.Flags == INDEX_ROOT_SMALL)
    {
        DbgPrint(" (small)\n");
    }
    else
    {
        ASSERT(IndexRootAttr->Header.Flags == INDEX_ROOT_LARGE);
        DbgPrint(" (large)\n");
    }

    DbgPrint("   Offset to first index: 0x%lx\n   Total size of index entries: 0x%lx\n   Allocated size of node: 0x%lx\n",
             IndexRootAttr->Header.FirstEntryOffset,
             IndexRootAttr->Header.TotalSizeOfEntries,
             IndexRootAttr->Header.AllocatedSize);
    CurrentOffset = IndexRootAttr->Header.FirstEntryOffset;
    CurrentNode = 0;
    // print details of every node in the index
    while (CurrentOffset < IndexRootAttr->Header.TotalSizeOfEntries)
    {
        PINDEX_ENTRY_ATTRIBUTE currentIndexExtry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)IndexRootAttr + 0x10 + CurrentOffset);
        DbgPrint("   Index Node Entry %lu", CurrentNode++);
        if (BooleanFlagOn(currentIndexExtry->Flags, NTFS_INDEX_ENTRY_NODE))
            DbgPrint(" (Branch)");
        else
            DbgPrint(" (Leaf)");
        if (BooleanFlagOn(currentIndexExtry->Flags, NTFS_INDEX_ENTRY_END))
        {
            DbgPrint(" (Dummy Key)");
        }
        DbgPrint("\n    File Reference: 0x%016I64x\n", currentIndexExtry->Data.Directory.IndexedFile);
        DbgPrint("    Index Entry Length: 0x%x\n", currentIndexExtry->Length);
        DbgPrint("    Index Key Length: 0x%x\n", currentIndexExtry->KeyLength);

        // if this isn't the final (dummy) node, print info about the key (Filename attribute)
        if (!(currentIndexExtry->Flags & NTFS_INDEX_ENTRY_END))
        {
            UNICODE_STRING Name;
            DbgPrint("     Parent File Reference: 0x%016I64x\n", currentIndexExtry->FileName.DirectoryFileReferenceNumber);
            DbgPrint("     $FILENAME indexed: ");
            Name.Length = currentIndexExtry->FileName.NameLength * sizeof(WCHAR);
            Name.MaximumLength = Name.Length;
            Name.Buffer = currentIndexExtry->FileName.Name;
            DbgPrint("'%wZ'\n", &Name);
        }

        // if this node has a sub-node beneath it
        if (currentIndexExtry->Flags & NTFS_INDEX_ENTRY_NODE)
        {
            // Print the VCN of the sub-node
            PULONGLONG SubNodeVCN = (PULONGLONG)((ULONG_PTR)currentIndexExtry + currentIndexExtry->Length - sizeof(ULONGLONG));
            DbgPrint("    VCN of sub-node: 0x%llx\n", *SubNodeVCN);
        }

        CurrentOffset += currentIndexExtry->Length;
        ASSERT(currentIndexExtry->Length);
    }

}


static
VOID
NtfsDumpAttribute(PDEVICE_EXTENSION Vcb,
                  PNTFS_ATTR_RECORD Attribute)
{
    UNICODE_STRING Name;

    ULONGLONG lcn = 0;
    ULONGLONG runcount = 0;

    if (!NTFS_TRACE_ENABLED)
        return;

    switch (Attribute->Type)
    {
        case AttributeFileName:
            NtfsDumpFileNameAttribute(Attribute);
            break;

        case AttributeStandardInformation:
            NtfsDumpStandardInformationAttribute(Attribute);
            break;

        case AttributeObjectId:
            DbgPrint("  $OBJECT_ID ");
            break;

        case AttributeSecurityDescriptor:
            DbgPrint("  $SECURITY_DESCRIPTOR ");
            break;

        case AttributeVolumeName:
            NtfsDumpVolumeNameAttribute(Attribute);
            break;

        case AttributeVolumeInformation:
            NtfsDumpVolumeInformationAttribute(Attribute);
            break;

        case AttributeData:
            DbgPrint("  $DATA ");
            //DataBuf = ExAllocatePool(NonPagedPool,AttributeLengthAllocated(Attribute));
            break;

        case AttributeIndexRoot:
            NtfsDumpIndexRootAttribute(Attribute);
            break;

        case AttributeIndexAllocation:
            DbgPrint("  $INDEX_ALLOCATION ");
            break;

        case AttributeBitmap:
            DbgPrint("  $BITMAP ");
            break;

        case AttributeReparsePoint:
            DbgPrint("  $REPARSE_POINT ");
            break;

        case AttributeEAInformation:
            DbgPrint("  $EA_INFORMATION ");
            break;

        case AttributeEA:
            DbgPrint("  $EA ");
            break;

        case AttributePropertySet:
            DbgPrint("  $PROPERTY_SET ");
            break;

        case AttributeLoggedUtilityStream:
            DbgPrint("  $LOGGED_UTILITY_STREAM ");
            break;

        default:
            DbgPrint("  Attribute %lx ",
                     Attribute->Type);
            break;
    }

    if (Attribute->Type != AttributeAttributeList)
    {
        if (Attribute->NameLength != 0)
        {
            Name.Length = Attribute->NameLength * sizeof(WCHAR);
            Name.MaximumLength = Name.Length;
            Name.Buffer = (PWCHAR)((ULONG_PTR)Attribute + Attribute->NameOffset);

            DbgPrint("'%wZ' ", &Name);
        }

        DbgPrint("(%s)\n",
                 Attribute->IsNonResident ? "non-resident" : "resident");

        if (Attribute->IsNonResident)
        {
            FindRun(Attribute,0,&lcn, &runcount);

            DbgPrint("  AllocatedSize %I64u  DataSize %I64u InitilizedSize %I64u\n",
                     Attribute->NonResident.AllocatedSize, Attribute->NonResident.DataSize, Attribute->NonResident.InitializedSize);
            DbgPrint("  logical clusters: %I64u - %I64u\n",
                     lcn, lcn + runcount - 1);
        }
        else
            DbgPrint("    %u bytes of data\n", Attribute->Resident.ValueLength);
    }
}


VOID NtfsDumpDataRunData(PUCHAR DataRun)
{
    UCHAR DataRunOffsetSize;
    UCHAR DataRunLengthSize;
    CHAR i;

    if (!NTFS_TRACE_ENABLED)
        return;

    DbgPrint("%02x ", *DataRun);

    if (*DataRun == 0)
        return;

    DataRunOffsetSize = (*DataRun >> 4) & 0xF;
    DataRunLengthSize = *DataRun & 0xF;

    DataRun++;
    for (i = 0; i < DataRunLengthSize; i++)
    {
        DbgPrint("%02x ", *DataRun);
        DataRun++;
    }

    for (i = 0; i < DataRunOffsetSize; i++)
    {
        DbgPrint("%02x ", *DataRun);
        DataRun++;
    }

    NtfsDumpDataRunData(DataRun);
}


VOID
NtfsDumpDataRuns(PVOID StartOfRun,
                 ULONGLONG CurrentLCN)
{
    PUCHAR DataRun = StartOfRun;
    LONGLONG DataRunOffset;
    ULONGLONG DataRunLength;

    if (!NTFS_TRACE_ENABLED)
        return;

    if (CurrentLCN == 0)
    {
        NTFS_TRACE("Dumping data runs.\n\tData:\n\t\t");
        NtfsDumpDataRunData(StartOfRun);
        DbgPrint("\n\tRuns:\n\t\tOff\t\tLCN\t\tLength\n");
    }

    DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);

    if (DataRunOffset != -1)
        CurrentLCN += DataRunOffset;

    DbgPrint("\t\t%I64d\t", DataRunOffset);
    if (DataRunOffset < 99999)
        DbgPrint("\t");
    DbgPrint("%I64u\t", CurrentLCN);
    if (CurrentLCN < 99999)
        DbgPrint("\t");
    DbgPrint("%I64u\n", DataRunLength);

    if (*DataRun == 0)
        DbgPrint("\t\t00\n");
    else
        NtfsDumpDataRuns(DataRun, CurrentLCN);
}


VOID
NtfsDumpFileAttributes(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER FileRecord)
{
    NTSTATUS Status;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;

    if (!NTFS_TRACE_ENABLED)
        return;

    Status = FindFirstAttribute(&Context, Vcb, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        NtfsDumpAttribute(Vcb, Attribute);

        Status = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
}

/* Does a $FILE_NAME value match the requested namespace? */
FORCEINLINE
BOOLEAN
NtfsFileNameTypeMatches(PFILENAME_ATTRIBUTE Name,
                        UCHAR NameType)
{
    return (Name->NameType == NameType ||
            (Name->NameType == NTFS_FILE_NAME_WIN32_AND_DOS && NameType == NTFS_FILE_NAME_WIN32) ||
            (Name->NameType == NTFS_FILE_NAME_WIN32_AND_DOS && NameType == NTFS_FILE_NAME_DOS));
}

/**
* @name GetFileNameFromRecord
* @implemented
*
* Returns the first $FILE_NAME of the requested namespace.  The base record
* is searched first; when the caller supplies SpillBuffer (an
* NTFS_FOUND_NAME_SIZE-byte buffer), the search continues into the
* $ATTRIBUTE_LIST child records - a heavily hard-linked file can have EVERY
* $FILE_NAME spilled to children (the base legally carries none), and treating
* such names as nonexistent made every open/stat of them fail with
* STATUS_OBJECT_NAME_NOT_FOUND (the msys 'hl2' dangling-directory bug).
*
* @return
* Pointer into FileRecord when the name was found in the base record,
* SpillBuffer (filled with a copy) when it was found in a child record,
* NULL when the record has no such name (or it is only in a child and no
* SpillBuffer was provided).
*/
PFILENAME_ATTRIBUTE
GetFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                      PFILE_RECORD_HEADER FileRecord,
                      UCHAR NameType,
                      PFILENAME_ATTRIBUTE SpillBuffer)
{
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    PFILENAME_ATTRIBUTE Name;
    NTSTATUS Status;

    Status = FindFirstAttribute(&Context, Vcb, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == AttributeFileName)
        {
            Name = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
            if (NtfsFileNameTypeMatches(Name, NameType))
            {
                FindCloseAttribute(&Context);
                return Name;
            }
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }

    /* Not in the base record: follow the $ATTRIBUTE_LIST into child records
     * (the full base iteration above already loaded the list value into the
     * context if the record has one). */
    if (SpillBuffer != NULL)
    {
        PNTFS_ATTRIBUTE_LIST_ITEM ListItem;
        PFILE_RECORD_HEADER ChildRecord = NULL;
        ULONGLONG LastChildMft = FileRecord->MFTRecordNumber;

        Status = FindFirstAttributeListItem(&Context, &ListItem);
        while (NT_SUCCESS(Status))
        {
            ULONGLONG ChildMft = ListItem->MFTIndex & NTFS_MFT_MASK;

            if (ListItem->Type == AttributeFileName &&
                ChildMft != FileRecord->MFTRecordNumber &&
                ChildMft != LastChildMft)
            {
                LastChildMft = ChildMft;

                if (ChildRecord == NULL)
                {
                    ChildRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
                    if (ChildRecord == NULL)
                        break;
                }

                if (NT_SUCCESS(ReadFileRecord(Vcb, ChildMft, ChildRecord)) &&
                    (ChildRecord->Flags & FRH_IN_USE) &&
                    (ChildRecord->BaseFileRecord & NTFS_MFT_MASK) == FileRecord->MFTRecordNumber)
                {
                    PNTFS_ATTR_RECORD ChildAttr =
                        (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildRecord + ChildRecord->AttributeOffset);

                    while (ChildAttr->Type != AttributeEnd &&
                           ChildAttr->Length != 0 &&
                           (ULONG_PTR)ChildAttr < (ULONG_PTR)ChildRecord + ChildRecord->BytesInUse)
                    {
                        if (ChildAttr->Type == AttributeFileName && !ChildAttr->IsNonResident)
                        {
                            Name = (PFILENAME_ATTRIBUTE)((ULONG_PTR)ChildAttr +
                                                         ChildAttr->Resident.ValueOffset);
                            if (NtfsFileNameTypeMatches(Name, NameType))
                            {
                                RtlCopyMemory(SpillBuffer,
                                              Name,
                                              min(GetFileNameAttributeLength(Name),
                                                  NTFS_FOUND_NAME_SIZE));
                                ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
                                FindCloseAttribute(&Context);
                                return SpillBuffer;
                            }
                        }
                        ChildAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildAttr + ChildAttr->Length);
                    }
                }
            }

            Status = FindNextAttributeListItem(&Context, &ListItem);
        }

        if (ChildRecord != NULL)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, ChildRecord);
    }

    FindCloseAttribute(&Context);
    return NULL;
}

/**
* GetPackedByteCount
* Returns the minimum number of bytes needed to represent the value of a
* 64-bit number. Used to encode data runs.
*/
UCHAR
GetPackedByteCount(LONGLONG NumberToPack,
                   BOOLEAN IsSigned)
{
    if (!IsSigned)
    {
        if (NumberToPack >= 0x0100000000000000)
            return 8;
        if (NumberToPack >= 0x0001000000000000)
            return 7;
        if (NumberToPack >= 0x0000010000000000)
            return 6;
        if (NumberToPack >= 0x0000000100000000)
            return 5;
        if (NumberToPack >= 0x0000000001000000)
            return 4;
        if (NumberToPack >= 0x0000000000010000)
            return 3;
        if (NumberToPack >= 0x0000000000000100)
            return 2;
        return 1;
    }

    if (NumberToPack > 0)
    {
        // we have to make sure the number that gets encoded won't be interpreted as negative
        if (NumberToPack >= 0x0080000000000000)
            return 8;
        if (NumberToPack >= 0x0000800000000000)
            return 7;
        if (NumberToPack >= 0x0000008000000000)
            return 6;
        if (NumberToPack >= 0x0000000080000000)
            return 5;
        if (NumberToPack >= 0x0000000000800000)
            return 4;
        if (NumberToPack >= 0x0000000000008000)
            return 3;
        if (NumberToPack >= 0x0000000000000080)
            return 2;
    }
    else
    {
        // negative number
        if (NumberToPack <= 0xff80000000000000)
            return 8;
        if (NumberToPack <= 0xffff800000000000)
            return 7;
        if (NumberToPack <= 0xffffff8000000000)
            return 6;
        if (NumberToPack <= 0xffffffff80000000)
            return 5;
        if (NumberToPack <= 0xffffffffff800000)
            return 4;
        if (NumberToPack <= 0xffffffffffff8000)
            return 3;
        if (NumberToPack <= 0xffffffffffffff80)
            return 2;
    }
    return 1;
}

NTSTATUS
GetLastClusterInDataRun(PDEVICE_EXTENSION Vcb, PNTFS_ATTR_RECORD Attribute, PULONGLONG LastCluster)
{
    LONGLONG DataRunOffset;
    ULONGLONG DataRunLength;
    LONGLONG DataRunStartLCN;

    ULONGLONG LastLCN = 0;
    PUCHAR DataRun = (PUCHAR)Attribute + Attribute->NonResident.MappingPairsOffset;

    if (!Attribute->IsNonResident)
        return STATUS_INVALID_PARAMETER;

    while (1)
    {
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);

        if (DataRunOffset != -1)
        {
            // Normal data run.
            DataRunStartLCN = LastLCN + DataRunOffset;
            LastLCN = DataRunStartLCN;
            *LastCluster = LastLCN + DataRunLength - 1;
        }

        if (*DataRun == 0)
            break;
    }

    return STATUS_SUCCESS;
}

PSTANDARD_INFORMATION
GetStandardInformationFromRecord(PDEVICE_EXTENSION Vcb,
                                 PFILE_RECORD_HEADER FileRecord)
{
    NTSTATUS Status;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    PSTANDARD_INFORMATION StdInfo;

    Status = FindFirstAttribute(&Context, Vcb, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == AttributeStandardInformation)
        {
            StdInfo = (PSTANDARD_INFORMATION)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
            FindCloseAttribute(&Context);
            return StdInfo;
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
    return NULL;
}

/**
* @name GetFileNameAttributeLength
* @implemented
*
* Returns the size of a given FILENAME_ATTRIBUTE, in bytes.
*
* @param FileNameAttribute
* Pointer to a FILENAME_ATTRIBUTE to determine the size of.
*
* @remarks
* The length of a FILENAME_ATTRIBUTE is variable and is dependent on the length of the file name stored at the end.
* This function operates on the FILENAME_ATTRIBUTE proper, so don't try to pass it a PNTFS_ATTR_RECORD.
*/
ULONG GetFileNameAttributeLength(PFILENAME_ATTRIBUTE FileNameAttribute)
{
    ULONG Length = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + (FileNameAttribute->NameLength * sizeof(WCHAR));
    return Length;
}

/* See GetFileNameFromRecord for the SpillBuffer contract (finds names spilled
 * to $ATTRIBUTE_LIST children when the caller provides the copy buffer). */
PFILENAME_ATTRIBUTE
GetBestFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                          PFILE_RECORD_HEADER FileRecord,
                          PFILENAME_ATTRIBUTE SpillBuffer)
{
    PFILENAME_ATTRIBUTE FileName;

    FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_POSIX, SpillBuffer);
    if (FileName == NULL)
    {
        FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_WIN32, SpillBuffer);
        if (FileName == NULL)
        {
            FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_DOS, SpillBuffer);
        }
    }

    return FileName;
}

/* EOF */
