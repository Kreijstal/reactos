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

#include "lfswrite.h"
#include "sparse.h"
#include <ntintsafe.h>

#define NDEBUG
#include <debug.h>

/* Set to 1 to dump each data run list as it is encoded. Uses DbgPrint, so it
 * prints whatever NDEBUG is set to. */
#define NTFS_DUMP_DATA_RUNS 0

/* FUNCTIONS ****************************************************************/

/**
* @name AddBitmap
* @implemented
*
* Adds the empty $BITMAP of an index (8 bytes of zeros) to a file record, where its type and
* name sort it.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param FileRecord
* Pointer to a complete file record to add the attribute to. The caller writes it back.
*
* @param Name
* Name of the index, NameLength characters long. Most often L"$I30".
*
* @return
* STATUS_SUCCESS, or STATUS_DISK_FULL if the record has no room for it.
*/
NTSTATUS
AddBitmap(PNTFS_VCB Vcb,
          PFILE_RECORD_HEADER FileRecord,
          PCWSTR Name,
          USHORT NameLength)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG NameOffset = ResidentHeaderLength;
    ULONG ValueOffset = ALIGN_UP_BY(NameOffset + (sizeof(WCHAR) * NameLength), VALUE_OFFSET_ALIGNMENT);
    // We'll start out with 8 bytes of bitmap data
    ULONG ValueLength = 8;
    ULONG AttributeLength = ALIGN_UP_BY(ValueOffset + ValueLength, ATTR_RECORD_ALIGNMENT);
    UCHAR Buffer[0x80];
    PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)Buffer;

    ASSERT(AttributeLength <= sizeof(Buffer));

    RtlZeroMemory(Attribute, AttributeLength);
    Attribute->Type = AttributeBitmap;
    Attribute->Length = AttributeLength;
    Attribute->NameLength = NameLength;
    Attribute->NameOffset = NameOffset;
    Attribute->Resident.ValueLength = ValueLength;
    Attribute->Resident.ValueOffset = ValueOffset;
    RtlCopyMemory((PCHAR)((ULONG_PTR)Attribute + NameOffset), Name, NameLength * sizeof(WCHAR));

    return NtfsInsertAttributeRecord(Vcb, FileRecord, Attribute, NULL);
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
* @name NtfsCompareAttributeOrder
*
* Orders attributes the way NTFS keeps them in a file record: by type code, then by
* name, compared without regard to case.
*
* @return
* Less than, equal to or greater than 0 as the first attribute sorts before, with or after
* the second.
*/
static
LONG
NtfsCompareAttributeOrder(
    _In_ PNTFS_VCB Vcb,
    _In_ ULONG Type1,
    _In_ PCWSTR Name1,
    _In_ ULONG NameLength1,
    _In_ ULONG Type2,
    _In_ PCWSTR Name2,
    _In_ ULONG NameLength2)
{
    ULONG i;

    if (Type1 != Type2)
        return (Type1 < Type2) ? -1 : 1;

    for (i = 0; i < min(NameLength1, NameLength2); i++)
    {
        WCHAR Char1 = NtfsUpcaseChar(Vcb, Name1[i]);
        WCHAR Char2 = NtfsUpcaseChar(Vcb, Name2[i]);

        if (Char1 != Char2)
            return (Char1 < Char2) ? -1 : 1;
    }

    if (NameLength1 != NameLength2)
        return (NameLength1 < NameLength2) ? -1 : 1;

    return 0;
}

/**
* @name NtfsInsertAttributeRecord
* @implemented
*
* Copies an attribute record into a file record, where its type and name sort it, and
* gives it the record's next instance number.
*
* @param Vcb
* Pointer to the NTFS_VCB of the volume.
*
* @param FileRecord
* The complete file record. Modified in place; the caller writes it back.
*
* @param Attribute
* The attribute to insert, its Length set.
*
* @param Instance
* Optionally receives the instance number the attribute got.
*
* @return
* STATUS_SUCCESS, STATUS_OBJECT_NAME_COLLISION if the record already has an attribute of
* that type and name (more than one $FILE_NAME is allowed: one per link), or STATUS_DISK_FULL
* if the record has no room for it.
*/
NTSTATUS
NtfsInsertAttributeRecord(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER FileRecord,
                          PNTFS_ATTR_RECORD Attribute,
                          PUSHORT Instance)
{
    PNTFS_ATTR_RECORD Current;
    PNTFS_ATTR_RECORD InsertPoint = NULL;
    ULONG InsertOffset;

    ASSERT(Attribute->Length % ATTR_RECORD_ALIGNMENT == 0);

    Current = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
    while (Current->Type != AttributeEnd)
    {
        LONG Order;

        ASSERT((ULONG_PTR)Current + Current->Length <= (ULONG_PTR)FileRecord + FileRecord->BytesInUse);

        Order = NtfsCompareAttributeOrder(Vcb,
                                          Attribute->Type,
                                          (PCWSTR)((ULONG_PTR)Attribute + Attribute->NameOffset),
                                          Attribute->NameLength,
                                          Current->Type,
                                          (PCWSTR)((ULONG_PTR)Current + Current->NameOffset),
                                          Current->NameLength);
        if (Order == 0 && Attribute->Type != AttributeFileName)
            return STATUS_OBJECT_NAME_COLLISION;

        if (Order < 0 && InsertPoint == NULL)
            InsertPoint = Current;

        Current = (PNTFS_ATTR_RECORD)((ULONG_PTR)Current + Current->Length);
    }

    if (InsertPoint == NULL)
        InsertPoint = Current;

    if (Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse < Attribute->Length)
        return STATUS_DISK_FULL;

    // Make room: everything from the insertion point on, end markers included, moves back
    InsertOffset = (ULONG)((ULONG_PTR)InsertPoint - (ULONG_PTR)FileRecord);
    RtlMoveMemory((PUCHAR)InsertPoint + Attribute->Length, InsertPoint, FileRecord->BytesInUse - InsertOffset);
    FileRecord->BytesInUse += Attribute->Length;

    RtlCopyMemory(InsertPoint, Attribute, Attribute->Length);
    InsertPoint->Instance = FileRecord->NextAttributeNumber++;

    if (Instance != NULL)
        *Instance = InsertPoint->Instance;

    return STATUS_SUCCESS;
}

/**
* @name NtfsBuildResidentAttribute
*
* Builds a resident attribute record in a pool buffer the caller frees.
*/
static
PNTFS_ATTR_RECORD
NtfsBuildResidentAttribute(ULONG Type,
                           PCWSTR Name,
                           USHORT NameLength,
                           const VOID *Data,
                           ULONG DataLength)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG ValueOffset = ALIGN_UP_BY(ResidentHeaderLength + NameLength * sizeof(WCHAR), ATTR_RECORD_ALIGNMENT);
    ULONG AttributeLength = ALIGN_UP_BY(ValueOffset + DataLength, ATTR_RECORD_ALIGNMENT);
    PNTFS_ATTR_RECORD Attribute;

    ASSERT(NameLength <= NTFS_MAX_NAME_LENGTH);

    Attribute = ExAllocatePoolWithTag(NonPagedPool, AttributeLength, TAG_NTFS);
    if (Attribute == NULL)
        return NULL;

    RtlZeroMemory(Attribute, AttributeLength);
    Attribute->Type = Type;
    Attribute->Length = AttributeLength;
    Attribute->NameLength = (UCHAR)NameLength;
    Attribute->NameOffset = (USHORT)ResidentHeaderLength;
    Attribute->Resident.ValueLength = DataLength;
    Attribute->Resident.ValueOffset = (USHORT)ValueOffset;
    RtlCopyMemory((PUCHAR)Attribute + ResidentHeaderLength, Name, NameLength * sizeof(WCHAR));
    if (DataLength != 0)
        RtlCopyMemory((PUCHAR)Attribute + ValueOffset, Data, DataLength);

    return Attribute;
}

/**
* @name NtfsBuildFileNameAttribute
*
* Builds the resident, indexed $FILE_NAME attribute holding Value, in a pool buffer the
* caller frees.
*/
PNTFS_ATTR_RECORD
NtfsBuildFileNameAttribute(PFILENAME_ATTRIBUTE Value,
                           ULONG ValueLength)
{
    PNTFS_ATTR_RECORD Attribute = NtfsBuildResidentAttribute(AttributeFileName, NULL, 0, Value, ValueLength);

    if (Attribute != NULL)
        Attribute->Resident.Flags = RA_INDEXED;

    return Attribute;
}

/**
* @name AddResidentAttribute
* @implemented
*
* Adds a resident attribute to a file record, where its type and name sort it.
*
* @param Vcb
* Pointer to the NTFS_VCB of the volume.
*
* @param FileRecord
* The complete file record. Modified in place; the caller writes it back.
*
* @param Type
* The attribute type code.
*
* @param Name
* The attribute's name, or NULL when NameLength is 0.
*
* @param NameLength
* Number of WCHARs in Name, at most 255.
*
* @param Data
* The value, or NULL when DataLength is 0.
*
* @param DataLength
* Size of the value in bytes.
*
* @return
* STATUS_SUCCESS, STATUS_OBJECT_NAME_COLLISION if an attribute of that type and name is
* already there, STATUS_DISK_FULL if the record has no room for it.
*
* @remarks
* Only this record changes. For a file with an $ATTRIBUTE_LIST, NtfsAddAttributeToFile()
* keeps the list in step.
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
    PNTFS_ATTR_RECORD Attribute;
    NTSTATUS Status;

    Attribute = NtfsBuildResidentAttribute(Type, Name, NameLength, Data, DataLength);
    if (Attribute == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsInsertAttributeRecord(Vcb, FileRecord, Attribute, NULL);

    ExFreePoolWithTag(Attribute, TAG_NTFS);

    return Status;
}

/**
* @name NtfsRemoveAttribute
* @implemented
*
* Takes the attribute at AttrOffset out of a file record. A non-resident attribute's
* clusters must already have been freed.
*/
VOID
NtfsRemoveAttribute(PFILE_RECORD_HEADER FileRecord,
                    ULONG AttrOffset)
{
    PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONG Length = Attribute->Length;

    ASSERT(Attribute->Type != AttributeEnd);
    ASSERT(AttrOffset + Length < FileRecord->BytesInUse);

    RtlMoveMemory(Attribute,
                  (PUCHAR)Attribute + Length,
                  FileRecord->BytesInUse - AttrOffset - Length);
    FileRecord->BytesInUse -= Length;
}

/*
 * $ATTRIBUTE_LIST
 *
 * A file whose attributes don't fit its base record keeps some of them in extension
 * records, file records whose BaseFileRecord points back at the base. The base then holds an
 * $ATTRIBUTE_LIST: one entry per attribute of the file, wherever it lives, sorted by type,
 * name and starting VCN. Lookups follow it (FindAttribute); this keeps it up to date.
 *
 * The list is rebuilt from the records whenever an attribute enters or leaves an extension
 * record, rather than edited in place, so it can't drift from what the records hold.
 */

/* An entry's fixed part; the name follows it */
#define NTFS_LIST_ENTRY_NAME_OFFSET     0x1A

/* A non-resident attribute header without a name, aligned */
#define NTFS_NONRESIDENT_HEADER_LENGTH  0x48

static
ULONG
NtfsListEntryLength(UCHAR NameLength)
{
    return ALIGN_UP_BY(NTFS_LIST_ENTRY_NAME_OFFSET + NameLength * sizeof(WCHAR), ATTR_RECORD_ALIGNMENT);
}

/* The file reference of a record: its index and sequence number */
static
ULONGLONG
NtfsRecordReference(PFILE_RECORD_HEADER Record)
{
    return Record->MFTRecordNumber | ((ULONGLONG)Record->SequenceNumber << 48);
}

/**
* @name NtfsFindAttributeListRecord
*
* Finds the $ATTRIBUTE_LIST of a base record. FindAttribute() can't: the attribute walk it
* uses steps over the list, reading it in.
*
* @return
* The offset of the attribute in Record, or 0 when the record has none.
*/
static
ULONG
NtfsFindAttributeListRecord(PFILE_RECORD_HEADER Record)
{
    PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Record + Record->AttributeOffset);

    while (Attribute->Type != AttributeEnd && Attribute->Type <= AttributeAttributeList)
    {
        if (Attribute->Type == AttributeAttributeList)
            return (ULONG)((ULONG_PTR)Attribute - (ULONG_PTR)Record);

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
    }

    return 0;
}

/**
* @name NtfsRecordHasAttributeList
*
* Whether a base record carries an $ATTRIBUTE_LIST.
*/
BOOLEAN
NtfsRecordHasAttributeList(PFILE_RECORD_HEADER Record)
{
    return NtfsFindAttributeListRecord(Record) != 0;
}

/**
* @name NtfsGetAttributeListContext
*
* Makes an attribute context for the $ATTRIBUTE_LIST of a base record.
*
* @return
* STATUS_SUCCESS, STATUS_OBJECT_NAME_NOT_FOUND if the record has no list, or
* STATUS_INSUFFICIENT_RESOURCES.
*/
static
NTSTATUS
NtfsGetAttributeListContext(PFILE_RECORD_HEADER BaseRecord,
                            PNTFS_ATTR_CONTEXT *ListContext,
                            PULONG ListOffset)
{
    *ListOffset = NtfsFindAttributeListRecord(BaseRecord);
    if (*ListOffset == 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *ListContext = PrepareAttributeContext((PNTFS_ATTR_RECORD)((ULONG_PTR)BaseRecord + *ListOffset));
    if (*ListContext == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    (*ListContext)->FileMFTIndex = BaseRecord->MFTRecordNumber;

    return STATUS_SUCCESS;
}

/**
* @name NtfsReadAttributeList
*
* Reads the value of a base record's $ATTRIBUTE_LIST into a pool buffer the caller frees.
*
* @return
* STATUS_SUCCESS, STATUS_OBJECT_NAME_NOT_FOUND if the record has no list, or an error.
*/
NTSTATUS
NtfsReadAttributeList(PNTFS_VCB Vcb,
                      PFILE_RECORD_HEADER BaseRecord,
                      PNTFS_ATTRIBUTE_LIST_ITEM *List,
                      PULONG Length)
{
    PNTFS_ATTR_CONTEXT ListContext;
    ULONGLONG ListLength;
    ULONG ListOffset;
    NTSTATUS Status;

    *List = NULL;
    *Length = 0;

    Status = NtfsGetAttributeListContext(BaseRecord, &ListContext, &ListOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    ListLength = AttributeDataLength(ListContext->pRecord);
    if (ListLength > MAXULONG)
    {
        ReleaseAttributeContext(ListContext);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    /* An empty list still gets a buffer, so the caller can always free it */
    *List = ExAllocatePoolWithTag(NonPagedPool, max((ULONG)ListLength, sizeof(ULONG)), TAG_NTFS);
    if (*List == NULL)
    {
        ReleaseAttributeContext(ListContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ReadAttribute(Vcb, ListContext, 0, (PCHAR)*List, (ULONG)ListLength) != ListLength)
    {
        ExFreePoolWithTag(*List, TAG_NTFS);
        *List = NULL;
        ReleaseAttributeContext(ListContext);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ReleaseAttributeContext(ListContext);
    *Length = (ULONG)ListLength;

    return STATUS_SUCCESS;
}

/**
* @name NtfsGetExtensionRecords
*
* Collects, without duplicates, the MFT indices of the extension records a list names.
*
* @param Indices
* Receives a pool array (TAG_NTFS) of at least one element, which the caller frees.
*/
NTSTATUS
NtfsGetExtensionRecords(PNTFS_ATTRIBUTE_LIST_ITEM List,
                        ULONG Length,
                        ULONGLONG BaseIndex,
                        PULONGLONG *Indices,
                        PULONG Count)
{
    PNTFS_ATTRIBUTE_LIST_ITEM Item;
    ULONG Offset, i;

    *Count = 0;

    /* Every entry is at least NTFS_LIST_ENTRY_NAME_OFFSET long, so this bounds the count */
    *Indices = ExAllocatePoolWithTag(NonPagedPool,
                                     (Length / NTFS_LIST_ENTRY_NAME_OFFSET + 1) * sizeof(ULONGLONG),
                                     TAG_NTFS);
    if (*Indices == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Offset = 0; Offset + NTFS_LIST_ENTRY_NAME_OFFSET <= Length; Offset += Item->Length)
    {
        ULONGLONG Index;

        Item = (PNTFS_ATTRIBUTE_LIST_ITEM)((ULONG_PTR)List + Offset);
        if (Item->Length < NTFS_LIST_ENTRY_NAME_OFFSET || Offset + Item->Length > Length)
            return STATUS_FILE_CORRUPT_ERROR;

        Index = Item->MFTIndex & NTFS_MFT_MASK;
        if (Index == BaseIndex)
            continue;

        for (i = 0; i < *Count && (*Indices)[i] != Index; i++)
            ;
        if (i == *Count)
            (*Indices)[(*Count)++] = Index;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsAppendListEntries
*
* Describes every attribute of Record in list entries at Buffer + *Used, but the
* $ATTRIBUTE_LIST itself. With Buffer NULL, only counts the bytes they take.
*/
static
VOID
NtfsAppendListEntries(PFILE_RECORD_HEADER Record,
                      PUCHAR Buffer,
                      PULONG Used)
{
    PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Record + Record->AttributeOffset);
    ULONGLONG Reference = NtfsRecordReference(Record);

    while (Attribute->Type != AttributeEnd)
    {
        if (Attribute->Type != AttributeAttributeList)
        {
            ULONG EntryLength = NtfsListEntryLength(Attribute->NameLength);

            if (Buffer != NULL)
            {
                PNTFS_ATTRIBUTE_LIST_ITEM Item = (PNTFS_ATTRIBUTE_LIST_ITEM)(Buffer + *Used);

                RtlZeroMemory(Item, EntryLength);
                Item->Type = Attribute->Type;
                Item->Length = (USHORT)EntryLength;
                Item->NameLength = Attribute->NameLength;
                Item->NameOffset = NTFS_LIST_ENTRY_NAME_OFFSET;
                Item->StartingVCN = Attribute->IsNonResident ? Attribute->NonResident.LowestVCN : 0;
                Item->MFTIndex = Reference;
                Item->Instance = Attribute->Instance;
                RtlCopyMemory((PUCHAR)Item + NTFS_LIST_ENTRY_NAME_OFFSET,
                              (PUCHAR)Attribute + Attribute->NameOffset,
                              Attribute->NameLength * sizeof(WCHAR));
            }

            *Used += EntryLength;
        }

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
    }
}

/* Orders list entries by type, name and starting VCN */
static
LONG
NtfsCompareListEntries(PNTFS_VCB Vcb,
                       PNTFS_ATTRIBUTE_LIST_ITEM Item1,
                       PNTFS_ATTRIBUTE_LIST_ITEM Item2)
{
    LONG Order = NtfsCompareAttributeOrder(Vcb,
                                           Item1->Type,
                                           (PCWSTR)((ULONG_PTR)Item1 + Item1->NameOffset),
                                           Item1->NameLength,
                                           Item2->Type,
                                           (PCWSTR)((ULONG_PTR)Item2 + Item2->NameOffset),
                                           Item2->NameLength);
    if (Order != 0)
        return Order;

    if (Item1->StartingVCN != Item2->StartingVCN)
        return (Item1->StartingVCN < Item2->StartingVCN) ? -1 : 1;

    return 0;
}

/**
* @name NtfsSortListEntries
*
* Sorts the entries of a list value in place. Lists are short; an insertion sort through a
* scratch copy does.
*/
static
NTSTATUS
NtfsSortListEntries(PNTFS_VCB Vcb,
                    PUCHAR List,
                    ULONG Length)
{
    PUCHAR Sorted;
    ULONG Offset, SortedLength = 0;

    Sorted = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);
    if (Sorted == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Offset = 0; Offset < Length; )
    {
        PNTFS_ATTRIBUTE_LIST_ITEM Item = (PNTFS_ATTRIBUTE_LIST_ITEM)(List + Offset);
        ULONG Position = 0;

        // Find where it goes among the ones already sorted
        while (Position < SortedLength &&
               NtfsCompareListEntries(Vcb, (PNTFS_ATTRIBUTE_LIST_ITEM)(Sorted + Position), Item) <= 0)
        {
            Position += ((PNTFS_ATTRIBUTE_LIST_ITEM)(Sorted + Position))->Length;
        }

        RtlMoveMemory(Sorted + Position + Item->Length, Sorted + Position, SortedLength - Position);
        RtlCopyMemory(Sorted + Position, Item, Item->Length);
        SortedLength += Item->Length;
        Offset += Item->Length;
    }

    RtlCopyMemory(List, Sorted, Length);
    ExFreePoolWithTag(Sorted, TAG_NTFS);

    return STATUS_SUCCESS;
}

/**
* @name NtfsWriteAttributeList
*
* Makes List the value of the base record's $ATTRIBUTE_LIST, adding the attribute if the
* record has none. A list that outgrows the record moves to clusters.
*
* @remarks
* BaseRecord is written back when the list is resident or changes residency; otherwise the
* caller writes it.
*/
static
NTSTATUS
NtfsWriteAttributeList(PNTFS_VCB Vcb,
                       PFILE_RECORD_HEADER BaseRecord,
                       PUCHAR List,
                       ULONG Length)
{
    PNTFS_ATTR_CONTEXT ListContext;
    ULONG ListOffset;
    LARGE_INTEGER NewLength;
    ULONG Written;
    NTSTATUS Status;

    /* A new list starts empty and is grown like an existing one, which moves it to
     * clusters when the record can't hold it */
    Status = NtfsGetAttributeListContext(BaseRecord, &ListContext, &ListOffset);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        Status = AddResidentAttribute(Vcb, BaseRecord, AttributeAttributeList, NULL, 0, NULL, 0);
        if (NT_SUCCESS(Status))
            Status = NtfsGetAttributeListContext(BaseRecord, &ListContext, &ListOffset);
    }

    if (!NT_SUCCESS(Status))
        return Status;

    NewLength.QuadPart = Length;
    if (ListContext->pRecord->IsNonResident)
        Status = SetNonResidentAttributeDataLength(Vcb, ListContext, ListOffset, BaseRecord, &NewLength);
    else
        Status = SetResidentAttributeDataLength(Vcb, ListContext, ListOffset, BaseRecord, &NewLength);

    if (NT_SUCCESS(Status) && Length != 0)
    {
        PNTFS_ATTR_RECORD ListAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseRecord + ListOffset);

        /* WriteAttribute() looks a resident attribute up by type, which can't find the list */
        if (!ListAttribute->IsNonResident)
            RtlCopyMemory((PUCHAR)ListAttribute + ListAttribute->Resident.ValueOffset, List, Length);
        else
            Status = WriteAttribute(Vcb, ListContext, 0, List, Length, &Written, BaseRecord);
    }

    ReleaseAttributeContext(ListContext);

    return Status;
}

/**
* @name NtfsRebuildAttributeList
* @implemented
*
* Rewrites a file's $ATTRIBUTE_LIST from the attributes its records hold.
*
* @param Vcb
* Pointer to the NTFS_VCB of the volume.
*
* @param BaseRecord
* The file's base record, in memory. The caller writes it back afterwards.
*
* @param NewIndices
* Extension records the current list doesn't name yet, or NULL.
*
* @param NewCount
* Number of entries in NewIndices.
*
* @return
* STATUS_SUCCESS, or an allocation or I/O error.
*
* @remarks
* An extension record left without attributes goes back to the MFT and out of the list.
*/
static
NTSTATUS
NtfsRebuildAttributeListOnce(PNTFS_VCB Vcb,
                             PFILE_RECORD_HEADER BaseRecord,
                             PULONGLONG NewIndices,
                             ULONG NewCount)
{
    PNTFS_ATTRIBUTE_LIST_ITEM OldList = NULL;
    ULONG OldLength = 0;
    PULONGLONG Indices = NULL;
    PFILE_RECORD_HEADER *Records = NULL;
    ULONG Count = 0, i, j;
    PUCHAR List = NULL;
    ULONG ListLength = 0;
    NTSTATUS Status;

    // The extension records the list names now, and the new ones
    Status = NtfsReadAttributeList(Vcb, BaseRecord, &OldList, &OldLength);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        Status = STATUS_SUCCESS;

    if (NT_SUCCESS(Status))
    {
        Indices = ExAllocatePoolWithTag(NonPagedPool,
                                        (OldLength / NTFS_LIST_ENTRY_NAME_OFFSET + NewCount + 1) * sizeof(ULONGLONG),
                                        TAG_NTFS);
        if (Indices == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(Status) && OldList != NULL)
    {
        PULONGLONG Named;
        ULONG NamedCount;

        Status = NtfsGetExtensionRecords(OldList, OldLength, BaseRecord->MFTRecordNumber, &Named, &NamedCount);
        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(Indices, Named, NamedCount * sizeof(ULONGLONG));
            Count = NamedCount;
        }
        if (Named != NULL)
            ExFreePoolWithTag(Named, TAG_NTFS);
    }

    for (i = 0; NT_SUCCESS(Status) && i < NewCount; i++)
    {
        for (j = 0; j < Count && Indices[j] != NewIndices[i]; j++)
            ;
        if (j == Count)
            Indices[Count++] = NewIndices[i];
    }

    if (NT_SUCCESS(Status))
    {
        Records = ExAllocatePoolWithTag(NonPagedPool, (Count + 1) * sizeof(PFILE_RECORD_HEADER), TAG_NTFS);
        if (Records == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
        else
            RtlZeroMemory(Records, (Count + 1) * sizeof(PFILE_RECORD_HEADER));
    }

    // Read them, giving back the ones that are empty now
    for (i = 0, j = 0; NT_SUCCESS(Status) && i < Count; i++)
    {
        PFILE_RECORD_HEADER Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);

        if (Record == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        Status = ReadFileRecord(Vcb, Indices[i], Record);
        if (NT_SUCCESS(Status) &&
            ((PNTFS_ATTR_RECORD)((ULONG_PTR)Record + Record->AttributeOffset))->Type == AttributeEnd)
        {
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
            Status = FreeMftEntry(Vcb, Indices[i]);
            continue;
        }

        if (!NT_SUCCESS(Status))
        {
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
            break;
        }

        Records[j++] = Record;
    }
    Count = j;

    // Describe every attribute of every record, in order
    if (NT_SUCCESS(Status))
    {
        NtfsAppendListEntries(BaseRecord, NULL, &ListLength);
        for (i = 0; i < Count; i++)
            NtfsAppendListEntries(Records[i], NULL, &ListLength);

        List = ExAllocatePoolWithTag(NonPagedPool, max(ListLength, sizeof(ULONG)), TAG_NTFS);
        if (List == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(Status))
    {
        ListLength = 0;
        NtfsAppendListEntries(BaseRecord, List, &ListLength);
        for (i = 0; i < Count; i++)
            NtfsAppendListEntries(Records[i], List, &ListLength);

        Status = NtfsSortListEntries(Vcb, List, ListLength);
    }

    if (NT_SUCCESS(Status))
        Status = NtfsWriteAttributeList(Vcb, BaseRecord, List, ListLength);

    if (List != NULL)
        ExFreePoolWithTag(List, TAG_NTFS);
    if (Records != NULL)
    {
        for (i = 0; Records[i] != NULL; i++)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Records[i]);
        ExFreePoolWithTag(Records, TAG_NTFS);
    }
    if (Indices != NULL)
        ExFreePoolWithTag(Indices, TAG_NTFS);
    if (OldList != NULL)
        ExFreePoolWithTag(OldList, TAG_NTFS);

    return Status;
}

static
NTSTATUS
NtfsMoveAttributesOutOfBase(PNTFS_VCB Vcb,
                            PFILE_RECORD_HEADER BaseRecord,
                            ULONG BytesNeeded,
                            PULONGLONG Indices,
                            ULONG MaxIndices,
                            PULONG Count);

NTSTATUS
NtfsRebuildAttributeList(PNTFS_VCB Vcb,
                         PFILE_RECORD_HEADER BaseRecord,
                         PULONGLONG NewIndices,
                         ULONG NewCount)
{
    ULONGLONG Indices[16];
    ULONG Count, Moved, i;
    NTSTATUS Status;

    Status = NtfsRebuildAttributeListOnce(Vcb, BaseRecord, NewIndices, NewCount);
    if (Status != STATUS_DISK_FULL)
        return Status;

    /* The list grew past what the base record holds and has no room to move to clusters
     * either: make room by moving attributes out, and build it again */
    if (NewCount > RTL_NUMBER_OF(Indices) / 2)
        return Status;

    for (i = 0; i < NewCount; i++)
        Indices[i] = NewIndices[i];
    Count = NewCount;

    Status = NtfsMoveAttributesOutOfBase(Vcb,
                                         BaseRecord,
                                         2 * NTFS_NONRESIDENT_HEADER_LENGTH,
                                         Indices + Count,
                                         RTL_NUMBER_OF(Indices) - Count,
                                         &Moved);
    if (Status == STATUS_DISK_FULL &&
        Vcb->NtfsInfo.BytesPerFileRecord - BaseRecord->BytesInUse >= NTFS_NONRESIDENT_HEADER_LENGTH)
    {
        Status = STATUS_SUCCESS;
    }

    if (NT_SUCCESS(Status))
        Status = NtfsRebuildAttributeListOnce(Vcb, BaseRecord, Indices, Count + Moved);

    return Status;
}

/**
* @name NtfsCreateExtensionRecord
*
* Makes a new, empty extension record of the file BaseRecord is the base record of, and
* writes it to the MFT.
*
* @param Record
* Receives the record, from the file record lookaside list.
*
* @param Index
* Receives its MFT index.
*/
static
NTSTATUS
NtfsCreateExtensionRecord(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER BaseRecord,
                          PFILE_RECORD_HEADER *Record,
                          PULONGLONG Index)
{
    NTSTATUS Status;

    *Record = NtfsCreateEmptyFileRecord(Vcb);
    if (*Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    (*Record)->BaseFileRecord = NtfsRecordReference(BaseRecord);

    Status = AddNewMftEntry(*Record, Vcb, Index, TRUE);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, *Record);
        *Record = NULL;
    }

    return Status;
}

/* What stays in the base record: $STANDARD_INFORMATION, the $ATTRIBUTE_LIST, and one
 * $FILE_NAME, which the code that reads a file's record without its list expects there.
 * Further names, one per link, may leave it. */
static
BOOLEAN
NtfsAttributeCanLeaveBaseRecord(ULONG Type,
                                ULONG FileNameCount)
{
    if (Type == AttributeFileName)
        return FileNameCount > 1;

    return (Type != AttributeStandardInformation &&
            Type != AttributeAttributeList);
}

/**
* @name NtfsMoveAttributesOutOfBase
*
* Moves a base record's largest movable attributes into new extension records until the
* base has BytesNeeded bytes free.
*
* @param Indices
* Receives the MFT indices of the extension records made, Count of them.
*
* @return
* STATUS_SUCCESS once the room is there, STATUS_DISK_FULL when nothing is left to move
* first, or an allocation or I/O error.
*
* @remarks
* The extension records are written; the base record is only changed in memory.
*/
static
NTSTATUS
NtfsMoveAttributesOutOfBase(PNTFS_VCB Vcb,
                            PFILE_RECORD_HEADER BaseRecord,
                            ULONG BytesNeeded,
                            PULONGLONG Indices,
                            ULONG MaxIndices,
                            PULONG Count)
{
    PFILE_RECORD_HEADER Extension = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    *Count = 0;

    while (Vcb->NtfsInfo.BytesPerFileRecord - BaseRecord->BytesInUse < BytesNeeded)
    {
        PNTFS_ATTR_RECORD Attribute;
        ULONG Offset = BaseRecord->AttributeOffset;
        ULONG BestOffset = 0, BestLength = 0;
        ULONG FileNameCount = 0;

        for (Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseRecord + BaseRecord->AttributeOffset);
             Attribute->Type != AttributeEnd;
             Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length))
        {
            if (Attribute->Type == AttributeFileName)
                FileNameCount++;
        }

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseRecord + BaseRecord->AttributeOffset);
        while (Attribute->Type != AttributeEnd)
        {
            if (NtfsAttributeCanLeaveBaseRecord(Attribute->Type, FileNameCount) && Attribute->Length > BestLength)
            {
                BestLength = Attribute->Length;
                BestOffset = Offset;
            }

            Offset += Attribute->Length;
            Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
        }

        if (BestLength == 0)
        {
            Status = STATUS_DISK_FULL;
            break;
        }

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseRecord + BestOffset);

        // A full extension record is done with; the next attribute starts another
        if (Extension != NULL &&
            Vcb->NtfsInfo.BytesPerFileRecord - Extension->BytesInUse < Attribute->Length)
        {
            Status = UpdateFileRecord(Vcb, Indices[*Count - 1], Extension);
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Extension);
            Extension = NULL;
            if (!NT_SUCCESS(Status))
                break;
        }

        if (Extension == NULL)
        {
            if (*Count == MaxIndices)
            {
                Status = STATUS_DISK_FULL;
                break;
            }

            Status = NtfsCreateExtensionRecord(Vcb, BaseRecord, &Extension, &Indices[*Count]);
            if (!NT_SUCCESS(Status))
                break;
            (*Count)++;
        }

        Status = NtfsInsertAttributeRecord(Vcb, Extension, Attribute, NULL);
        if (!NT_SUCCESS(Status))
            break;

        NtfsRemoveAttribute(BaseRecord, BestOffset);
    }

    if (Extension != NULL)
    {
        NTSTATUS WriteStatus = UpdateFileRecord(Vcb, Indices[*Count - 1], Extension);

        if (NT_SUCCESS(Status))
            Status = WriteStatus;
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Extension);
    }

    return Status;
}

/**
* @name NtfsCreateAttributeList
*
* Gives a full base record an $ATTRIBUTE_LIST: moves its largest attributes out into
* extension records until the list fits resident, or at least as a non-resident header,
* then adds the list.
*/
static
NTSTATUS
NtfsCreateAttributeList(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER BaseRecord)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONGLONG Indices[8];
    ULONG Count = 0;
    ULONG Needed = 0;
    NTSTATUS Status;

    // The list describes every attribute, the moved ones too, so its size is known now
    NtfsAppendListEntries(BaseRecord, NULL, &Needed);
    Needed = ALIGN_UP_BY(ResidentHeaderLength + Needed, ATTR_RECORD_ALIGNMENT);

    Status = NtfsMoveAttributesOutOfBase(Vcb, BaseRecord, Needed, Indices, RTL_NUMBER_OF(Indices), &Count);

    /* With nothing left to move the list can still go to clusters */
    if (Status == STATUS_DISK_FULL &&
        Vcb->NtfsInfo.BytesPerFileRecord - BaseRecord->BytesInUse >= NTFS_NONRESIDENT_HEADER_LENGTH)
    {
        Status = STATUS_SUCCESS;
    }

    if (NT_SUCCESS(Status))
        Status = NtfsRebuildAttributeList(Vcb, BaseRecord, Indices, Count);

    return Status;
}

/**
* @name NtfsKeepFileNameInBase
* @implemented
*
* Brings a $FILE_NAME back into a base record that lost its last one while the file still
* has names in extension records: code that reads a file's record without its list finds
* the file's name and duplicated information in the base.
*
* @remarks
* The base record is changed in memory; the caller writes it back.
*/
NTSTATUS
NtfsKeepFileNameInBase(PNTFS_VCB Vcb,
                       PFILE_RECORD_HEADER BaseRecord)
{
    PNTFS_ATTRIBUTE_LIST_ITEM List;
    PULONGLONG Indices = NULL;
    PFILE_RECORD_HEADER Extension = NULL;
    PNTFS_ATTR_RECORD Attribute;
    ULONG ListLength, Count = 0, i;
    NTSTATUS Status;

    for (Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseRecord + BaseRecord->AttributeOffset);
         Attribute->Type != AttributeEnd;
         Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length))
    {
        if (Attribute->Type == AttributeFileName)
            return STATUS_SUCCESS;
    }

    if (!NtfsRecordHasAttributeList(BaseRecord))
        return STATUS_SUCCESS;

    Status = NtfsReadAttributeList(Vcb, BaseRecord, &List, &ListLength);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsGetExtensionRecords(List, ListLength, BaseRecord->MFTRecordNumber, &Indices, &Count);
    ExFreePoolWithTag(List, TAG_NTFS);

    if (NT_SUCCESS(Status))
    {
        Extension = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
        if (Extension == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; NT_SUCCESS(Status) && i < Count; i++)
    {
        Status = ReadFileRecord(Vcb, Indices[i], Extension);
        if (!NT_SUCCESS(Status))
            break;

        for (Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Extension + Extension->AttributeOffset);
             Attribute->Type != AttributeEnd && Attribute->Type != AttributeFileName;
             Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length))
            ;

        if (Attribute->Type != AttributeFileName)
            continue;

        Status = NtfsInsertAttributeRecord(Vcb, BaseRecord, Attribute, NULL);
        if (Status == STATUS_DISK_FULL)
        {
            ULONGLONG Moved[4];
            ULONG MovedCount;

            /* Something else makes way for it */
            Status = NtfsMoveAttributesOutOfBase(Vcb, BaseRecord, Attribute->Length, Moved, RTL_NUMBER_OF(Moved), &MovedCount);
            if (NT_SUCCESS(Status))
                Status = NtfsInsertAttributeRecord(Vcb, BaseRecord, Attribute, NULL);
            if (NT_SUCCESS(Status))
                Status = NtfsRebuildAttributeList(Vcb, BaseRecord, Moved, MovedCount);
        }

        if (NT_SUCCESS(Status))
        {
            NtfsRemoveAttribute(Extension, (ULONG)((ULONG_PTR)Attribute - (ULONG_PTR)Extension));
            Status = UpdateFileRecord(Vcb, Indices[i], Extension);
        }

        if (NT_SUCCESS(Status))
            Status = NtfsRebuildAttributeList(Vcb, BaseRecord, NULL, 0);
        break;
    }

    if (Extension != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Extension);
    if (Indices != NULL)
        ExFreePoolWithTag(Indices, TAG_NTFS);

    return Status;
}

/**
* @name NtfsAddAttributeToFile
* @implemented
*
* Adds an attribute to a file. It goes into the base record while that has room; after
* that, into an extension record through the file's $ATTRIBUTE_LIST, which this creates
* if the file has none.
*
* @param Vcb
* Pointer to the NTFS_VCB of the volume.
*
* @param BaseRecord
* The file's base record, in memory. The caller writes it back afterwards.
*
* @param Attribute
* The attribute to add, a complete attribute record.
*
* @return
* STATUS_SUCCESS, STATUS_OBJECT_NAME_COLLISION if the base record already has it,
* STATUS_DISK_FULL if no record can take it, or an allocation or I/O error.
*/
NTSTATUS
NtfsAddAttributeToFile(PNTFS_VCB Vcb,
                       PFILE_RECORD_HEADER BaseRecord,
                       PNTFS_ATTR_RECORD Attribute)
{
    PNTFS_ATTRIBUTE_LIST_ITEM List;
    PULONGLONG Indices = NULL;
    PFILE_RECORD_HEADER Extension;
    ULONG ListLength, Count = 0, i;
    ULONGLONG ExtensionIndex = 0;
    ULONG Needed;
    NTSTATUS Status;

    if (!NtfsRecordHasAttributeList(BaseRecord))
    {
        Status = NtfsInsertAttributeRecord(Vcb, BaseRecord, Attribute, NULL);
        if (Status != STATUS_DISK_FULL)
            return Status;

        Status = NtfsCreateAttributeList(Vcb, BaseRecord);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Find an extension record with room for it, or make one. The room has to hold the
     * attribute once it's non-resident too: a resident value that outgrows the record
     * moves its value to clusters, and must not be stuck because the header can't. */
    Needed = max(Attribute->Length,
                 ALIGN_UP_BY(0x48 + Attribute->NameLength * sizeof(WCHAR), ATTR_RECORD_ALIGNMENT));

    Status = NtfsReadAttributeList(Vcb, BaseRecord, &List, &ListLength);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsGetExtensionRecords(List, ListLength, BaseRecord->MFTRecordNumber, &Indices, &Count);
    ExFreePoolWithTag(List, TAG_NTFS);

    Extension = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Extension == NULL)
        Status = STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; NT_SUCCESS(Status) && i < Count; i++)
    {
        Status = ReadFileRecord(Vcb, Indices[i], Extension);
        if (NT_SUCCESS(Status) &&
            Vcb->NtfsInfo.BytesPerFileRecord - Extension->BytesInUse >= Needed)
        {
            ExtensionIndex = Indices[i];
            break;
        }
    }

    if (NT_SUCCESS(Status) && ExtensionIndex == 0)
    {
        if (Extension != NULL)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Extension);
        Status = NtfsCreateExtensionRecord(Vcb, BaseRecord, &Extension, &ExtensionIndex);
    }

    if (NT_SUCCESS(Status))
        Status = NtfsInsertAttributeRecord(Vcb, Extension, Attribute, NULL);

    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, ExtensionIndex, Extension);

    if (NT_SUCCESS(Status))
        Status = NtfsRebuildAttributeList(Vcb, BaseRecord, &ExtensionIndex, 1);

    if (Extension != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Extension);
    if (Indices != NULL)
        ExFreePoolWithTag(Indices, TAG_NTFS);

    return Status;
}

/**
* @name NtfsAddResidentAttributeToFile
* @implemented
*
* NtfsAddAttributeToFile() for an attribute described by its parts, resident.
*/
NTSTATUS
NtfsAddResidentAttributeToFile(PNTFS_VCB Vcb,
                               PFILE_RECORD_HEADER BaseRecord,
                               ULONG Type,
                               PCWSTR Name,
                               USHORT NameLength,
                               const VOID *Data,
                               ULONG DataLength)
{
    PNTFS_ATTR_RECORD Attribute;
    NTSTATUS Status;

    Attribute = NtfsBuildResidentAttribute(Type, Name, NameLength, Data, DataLength);
    if (Attribute == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsAddAttributeToFile(Vcb, BaseRecord, Attribute);

    ExFreePoolWithTag(Attribute, TAG_NTFS);

    return Status;
}

/**
* @name NtfsReadAttributeHolder
*
* A lookup through the $ATTRIBUTE_LIST finds an attribute in an extension record: its
* context and offset describe that record, not the base record that was searched. Anything
* that changes the attribute has to change that record. This returns it: BaseRecord itself
* when the attribute is there, otherwise the extension record, read into a buffer the
* caller gives back with NtfsReleaseAttributeHolder().
*/
NTSTATUS
NtfsReadAttributeHolder(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT AttrContext,
                        PFILE_RECORD_HEADER BaseRecord,
                        PFILE_RECORD_HEADER *Holder)
{
    NTSTATUS Status;

    if ((AttrContext->FileMFTIndex & NTFS_MFT_MASK) == BaseRecord->MFTRecordNumber)
    {
        *Holder = BaseRecord;
        return STATUS_SUCCESS;
    }

    *Holder = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (*Holder == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, AttrContext->FileMFTIndex & NTFS_MFT_MASK, *Holder);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, *Holder);
        *Holder = NULL;
    }

    return Status;
}

NTSTATUS
NtfsResolveAttributeHolder(PNTFS_VCB Vcb,
                           PNTFS_ATTR_CONTEXT Context,
                           PFILE_RECORD_HEADER Record,
                           PFILE_RECORD_HEADER *Holder,
                           PULONG Offset)
{
    PNTFS_ATTR_RECORD Attribute;
    NTSTATUS Status = NtfsReadAttributeHolder(Vcb, Context, Record, Holder);

    if (!NT_SUCCESS(Status))
        return Status;
    for (*Offset = (*Holder)->AttributeOffset; ; *Offset += Attribute->Length)
    {
        Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)*Holder + *Offset);
        ASSERT(Attribute->Type != AttributeEnd);
        if (Attribute->Type == Context->pRecord->Type && Attribute->Instance == Context->pRecord->Instance)
            break;
    }
    return STATUS_SUCCESS;
}

VOID
NtfsReleaseAttributeHolder(PNTFS_VCB Vcb,
                           PFILE_RECORD_HEADER BaseRecord,
                           PFILE_RECORD_HEADER Holder)
{
    if (Holder != NULL && Holder != BaseRecord)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Holder);
}

/* A colon starts a stream name (MS-FSA 2.1.5.1), so no file or directory
 * name ever contains one. */
BOOLEAN
NtfsNameHasStreamPart(PCUNICODE_STRING Name)
{
    USHORT i;

    for (i = 0; i < Name->Length / sizeof(WCHAR); i++)
    {
        if (Name->Buffer[i] == L':')
            return TRUE;
    }

    return FALSE;
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
* @param ParentReference
* File reference (MFT index and sequence number) of the directory the name is in.
*
* @param Name
* The name, a single path component.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application opened the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param FileAttributes
* The attributes $STANDARD_INFORMATION was given, duplicated here as directory listings
* report them from the index entry.
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
            ULONGLONG ParentReference,
            PCUNICODE_STRING Name,
            BOOLEAN CaseSensitive,
            ULONG FileAttributes)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    PFILENAME_ATTRIBUTE FileNameAttribute;
    LARGE_INTEGER SystemTime;
    ULONG FileRecordEnd = AttributeAddress->Length;

    ASSERT(Name->Length != 0 && Name->Length <= NTFS_MAX_NAME_LENGTH * sizeof(WCHAR));
    ASSERT(!NtfsNameHasStreamPart(Name));

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

    FileNameAttribute->FileAttributes = FileAttributes;
    if (FileRecord->Flags & FRH_DIRECTORY)
        FileNameAttribute->FileAttributes |= NTFS_FILE_TYPE_DIRECTORY;

    FileNameAttribute->DirectoryFileReferenceNumber = ParentReference;

    FileNameAttribute->NameLength = Name->Length / sizeof(WCHAR);
    RtlCopyMemory(FileNameAttribute->Name, Name->Buffer, Name->Length);

    // For now, we're emulating the way Windows behaves when 8.3 name generation is disabled:
    // a name valid in both namespaces is marked as such, any other is a Win32 name, and only
    // a case-sensitive create makes a POSIX one.
    // TODO: add DOS Filename as needed
    if (CaseSensitive)
        FileNameAttribute->NameType = NTFS_FILE_NAME_POSIX;
    else if (RtlIsNameLegalDOS8Dot3((PUNICODE_STRING)Name, NULL, NULL))
        FileNameAttribute->NameType = NTFS_FILE_NAME_WIN32_AND_DOS;
    else
        FileNameAttribute->NameType = NTFS_FILE_NAME_WIN32;

    FileRecord->LinkCount++;

    AttributeAddress->Length = ResidentHeaderLength +
        FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + Name->Length;
    AttributeAddress->Length = ALIGN_UP_BY(AttributeAddress->Length, ATTR_RECORD_ALIGNMENT);

    AttributeAddress->Resident.ValueLength = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + Name->Length;
    AttributeAddress->Resident.ValueOffset = ResidentHeaderLength;
    AttributeAddress->Resident.Flags = RA_INDEXED;

    // move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);

    return STATUS_SUCCESS;
}

/**
* @name AddIndexAllocation
* @implemented
*
* Adds an empty $INDEX_ALLOCATION of an index to a file record, where its type and name sort
* it.
*
* @param Vcb
* Pointer to an NTFS_VCB for the destination volume.
*
* @param FileRecord
* Pointer to a complete file record to add the attribute to. The caller writes it back.
*
* @param Name
* Name of the index, NameLength characters long. Most often, this will be L"$I30".
*
* @return
* STATUS_SUCCESS, or STATUS_DISK_FULL if the record has no room for it.
*/
NTSTATUS
AddIndexAllocation(PNTFS_VCB Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   PCWSTR Name,
                   USHORT NameLength)
{
    ULONG NameOffset = FIELD_OFFSET(NTFS_ATTR_RECORD, NonResident.CompressedSize);
    // The data run offset must be aligned to a 4-byte boundary
    ULONG DataRunOffset = ALIGN_UP_BY((sizeof(WCHAR) * NameLength) + NameOffset, DATA_RUN_ALIGNMENT);
    // The empty data run will consist of a single byte
    ULONG RecordLength = ALIGN_UP_BY(DataRunOffset + 1, ATTR_RECORD_ALIGNMENT);
    UCHAR Buffer[0x80];
    PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)Buffer;

    ASSERT(RecordLength <= sizeof(Buffer));

    RtlZeroMemory(Attribute, RecordLength);
    Attribute->Type = AttributeIndexAllocation;
    Attribute->Length = RecordLength;
    Attribute->IsNonResident = TRUE;
    Attribute->NameLength = NameLength;
    Attribute->NameOffset = NameOffset;
    Attribute->NonResident.MappingPairsOffset = DataRunOffset;
    Attribute->NonResident.HighestVCN = (LONGLONG)-1;
    RtlCopyMemory((PCHAR)((ULONG_PTR)Attribute + NameOffset), Name, NameLength * sizeof(WCHAR));

    return NtfsInsertAttributeRecord(Vcb, FileRecord, Attribute, NULL);
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
* STATUS_NOT_IMPLEMENTED if we need to migrate the attribute to an attribute list (TODO).
* Any other failure from ConvertLargeMCBToDataRuns() is returned as-is.
*
* @remarks
* Clusters should have been allocated previously with NtfsAllocateClusters().
*
*
*/
/* Move a growing non-resident attribute to a record of its own. Removing
 * its old header leaves enough room for even a non-resident attribute list. */
static NTSTATUS
NtfsMigrateRunAttribute(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT Context,
                        PFILE_RECORD_HEADER Base,
                        PFILE_RECORD_HEADER Backup,
                        ULONG Offset,
                        ULONGLONG VcnCount,
                        ULONG AddedClusters)
{
    PFILE_RECORD_HEADER Child = NULL;
    PNTFS_ATTR_RECORD Attribute = NULL;
    PUCHAR Runs;
    ULONG RunBytes, Length;
    ULONGLONG ChildIndex = 0;
    USHORT Instance;
    NTSTATUS Status, UndoStatus;

    if (Base->BaseFileRecord != 0 || Context->pRecord->Type == AttributeAttributeList ||
        (Base->MFTRecordNumber == NTFS_FILE_MFT && Context->pRecord->Type == AttributeData &&
         Context->pRecord->NameLength == 0))
        return STATUS_DISK_FULL;
    Runs = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (Runs == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = ConvertLargeMCBToDataRuns(&Context->DataRunsMCB, VcnCount, Runs,
                                       Vcb->NtfsInfo.BytesPerFileRecord, &RunBytes);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Length = ALIGN_UP_BY(Context->pRecord->NonResident.MappingPairsOffset + RunBytes, ATTR_RECORD_ALIGNMENT);
    Child = NtfsCreateEmptyFileRecord(Vcb);
    if (Child == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    if (Length > Vcb->NtfsInfo.BytesPerFileRecord - Child->BytesInUse)
    {
        Status = STATUS_DISK_FULL;
        goto Cleanup;
    }
    Attribute = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);
    if (Attribute == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlZeroMemory(Attribute, Length);
    RtlCopyMemory(Attribute, Context->pRecord, min(Length, Context->pRecord->Length));
    Attribute->Length = Length;
    Attribute->NonResident.AllocatedSize = VcnCount * Vcb->NtfsInfo.BytesPerCluster;
    Attribute->NonResident.HighestVCN = VcnCount - 1;
    if (Attribute->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE))
        Attribute->NonResident.CompressedSize += (ULONGLONG)AddedClusters * Vcb->NtfsInfo.BytesPerCluster;
    RtlCopyMemory((PUCHAR)Attribute + Attribute->NonResident.MappingPairsOffset, Runs, RunBytes);
    Child->BaseFileRecord = NtfsRecordReference(Base);
    Status = NtfsInsertAttributeRecord(Vcb, Child, Attribute, &Instance);
    ASSERT(NT_SUCCESS(Status));
    Attribute->Instance = Instance;
    Status = AddNewMftEntry(Child, Vcb, &ChildIndex, TRUE);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    NtfsRemoveAttribute(Base, Offset);
    Status = NtfsRebuildAttributeList(Vcb, Base, &ChildIndex, 1);
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Base->MFTRecordNumber, Base);
    if (!NT_SUCCESS(Status))
    {
        UndoStatus = UpdateFileRecord(Vcb, Backup->MFTRecordNumber, Backup);
        if (NT_SUCCESS(UndoStatus))
        {
            RtlCopyMemory(Base, Backup, Vcb->NtfsInfo.BytesPerFileRecord);
            UndoStatus = FreeMftEntry(Vcb, ChildIndex);
            if (NT_SUCCESS(UndoStatus))
                goto Cleanup;
        }
        /* Keep the data allocation claimed if an I/O error prevents undo. */
        DPRINT1("Couldn't undo attribute migration (Status %lx)\n", UndoStatus);
    }
    ExFreePoolWithTag(Context->pRecord, TAG_NTFS);
    Context->pRecord = Attribute;
    Context->FileMFTIndex = ChildIndex;
    Attribute = NULL;
Cleanup:
    if (Attribute != NULL)
        ExFreePoolWithTag(Attribute, TAG_NTFS);
    if (Child != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Child);
    ExFreePoolWithTag(Runs, TAG_NTFS);
    return Status;
}

NTSTATUS
AddRun(PNTFS_VCB Vcb,
       PNTFS_ATTR_CONTEXT AttrContext,
       ULONG AttrOffset,
       PFILE_RECORD_HEADER FileRecord,
       ULONGLONG NextAssignedCluster,
       ULONG RunLength)
{
    PFILE_RECORD_HEADER OriginalRecord = FileRecord;
    PFILE_RECORD_HEADER SavedRecord;
    PNTFS_ATTR_RECORD SavedAttribute;
    PNTFS_ATTR_RECORD Destination;
    ULONGLONG NextVcn;
    BOOLEAN Added = FALSE, Written = FALSE;
    BOOLEAN MasterContext = (AttrContext == Vcb->MFTContext);
    NTSTATUS Status;

    ASSERT(AttrContext->pRecord->IsNonResident);
    ASSERT(RunLength != 0);
    Status = NtfsResolveAttributeHolder(Vcb, AttrContext, OriginalRecord, &FileRecord, &AttrOffset);
    if (!NT_SUCCESS(Status))
        return Status;
    NextVcn = AttrContext->pRecord->NonResident.AllocatedSize / Vcb->NtfsInfo.BytesPerCluster;
    ASSERT(NextVcn == AttrContext->pRecord->NonResident.HighestVCN + 1 || NextVcn == 0);

    SavedRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (SavedRecord == NULL)
    {
        NtfsReleaseAttributeHolder(Vcb, OriginalRecord, FileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    SavedAttribute = ExAllocatePoolWithTag(NonPagedPool, AttrContext->pRecord->Length, TAG_NTFS);
    if (SavedAttribute == NULL)
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, SavedRecord);
        NtfsReleaseAttributeHolder(Vcb, OriginalRecord, FileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(SavedRecord, FileRecord, Vcb->NtfsInfo.BytesPerFileRecord);
    RtlCopyMemory(SavedAttribute, AttrContext->pRecord, AttrContext->pRecord->Length);

    if (MasterContext)
    {
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Vcb->MftContextResource, TRUE);
    }
    _SEH2_TRY
    {
        Added = FsRtlAddLargeMcbEntry(&AttrContext->DataRunsMCB, NextVcn, NextAssignedCluster, RunLength);
        Status = Added ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (NT_SUCCESS(Status))
        Status = NtfsStoreMappingPairs(Vcb, AttrContext, AttrOffset, FileRecord, NextVcn + RunLength);
    if (Status == STATUS_DISK_FULL)
    {
        ULONGLONG PreviousOwner = AttrContext->FileMFTIndex;

        Status = NtfsMigrateRunAttribute(Vcb, AttrContext, FileRecord, SavedRecord,
                                         AttrOffset, NextVcn + RunLength, RunLength);
        if (AttrContext->FileMFTIndex != PreviousOwner)
            goto Cleanup; /* The new owner already contains the complete run. */
    }
    if (NT_SUCCESS(Status))
    {
        Destination = (PNTFS_ATTR_RECORD)((PUCHAR)FileRecord + AttrOffset);
        if (AttrContext->pRecord->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE))
        {
            AttrContext->pRecord->NonResident.CompressedSize += (ULONGLONG)RunLength * Vcb->NtfsInfo.BytesPerCluster;
            Destination->NonResident.CompressedSize = AttrContext->pRecord->NonResident.CompressedSize;
        }
        Written = TRUE;
        Status = UpdateFileRecord(Vcb, AttrContext->FileMFTIndex, FileRecord);
    }

    if (!NT_SUCCESS(Status) && Written)
    {
        NTSTATUS RollbackStatus = UpdateFileRecord(Vcb, AttrContext->FileMFTIndex, SavedRecord);

        if (!NT_SUCCESS(RollbackStatus))
        {
            /* Keep the allocation claimed if the disk may still refer to it. */
            DPRINT1("Couldn't restore attribute allocation (Status %lx)\n", RollbackStatus);
            goto Cleanup;
        }
    }
    if (!NT_SUCCESS(Status))
    {
        if (Added)
            FsRtlTruncateLargeMcb(&AttrContext->DataRunsMCB, NextVcn);
        RtlCopyMemory(FileRecord, SavedRecord, Vcb->NtfsInfo.BytesPerFileRecord);
        ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);
        AttrContext->pRecord = SavedAttribute;
        SavedAttribute = NULL;
    }
Cleanup:
    if (MasterContext)
    {
        ExReleaseResourceLite(&Vcb->MftContextResource);
        KeLeaveCriticalRegion();
    }
    if (SavedAttribute != NULL)
        ExFreePoolWithTag(SavedAttribute, TAG_NTFS);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, SavedRecord);
    NtfsReleaseAttributeHolder(Vcb, OriginalRecord, FileRecord);
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
* @param FileAttributes
* The FILE_ATTRIBUTE_* flags the file starts with.
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

    RtlZeroMemory(StandardInfo, sizeof(STANDARD_INFORMATION));

    // set dates and times
    KeQuerySystemTime(&SystemTime);
    StandardInfo->CreationTime = SystemTime.QuadPart;
    StandardInfo->ChangeTime = SystemTime.QuadPart;
    StandardInfo->LastWriteTime = SystemTime.QuadPart;
    StandardInfo->LastAccessTime = SystemTime.QuadPart;
    StandardInfo->FileAttribute = FileAttributes;

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
* @param DataRunEnd
* End of the buffer holding the run data, normally the end of the attribute
* record. The runs and their terminator must lie entirely before it.
*
* @param DataRunsMCB
* Pointer to an unitialized LARGE_MCB structure.
*
* @return
* STATUS_SUCCESS on success, STATUS_INSUFFICIENT_RESOURCES or STATUS_UNSUCCESSFUL if we fail to
* initialize the mcb or add an entry. STATUS_FILE_CORRUPT_ERROR if the runs reach past
* DataRunEnd.
*
* @remarks
* Initializes the LARGE_MCB pointed to by DataRunsMCB. If this function succeeds, you
* need to call FsRtlUninitializeLargeMcb() when you're done with DataRunsMCB. This
* function will ensure the LargeMCB has been unitialized in case of failure.
*
*/
NTSTATUS
ConvertDataRunsToLargeMCB(PUCHAR DataRun,
                          PUCHAR DataRunEnd,
                          PLARGE_MCB DataRunsMCB,
                          PULONGLONG pNextVBN)
{
    LONGLONG  DataRunOffset;
    ULONGLONG DataRunLength;
    LONGLONG  DataRunStartLCN;
    ULONGLONG LastLCN = 0;
    BOOLEAN   IsSparse;

    // Initialize the MCB, potentially catch an exception
    _SEH2_TRY{
        FsRtlInitializeLargeMcb(DataRunsMCB, NonPagedPool);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    } _SEH2_END;

    while (TRUE)
    {
        // The header, the run it describes and the terminator must all be in the buffer
        if (DataRun >= DataRunEnd ||
            (*DataRun != 0 &&
             ((*DataRun & 0xF) == 0 || (*DataRun & 0xF) > sizeof(ULONGLONG) ||
              (*DataRun >> 4) > sizeof(LONGLONG) ||
              DataRunEnd - DataRun <= 1 + (*DataRun & 0xF) + (*DataRun >> 4))))
        {
            DPRINT1("Data runs reach past the end of the attribute\n");
            FsRtlUninitializeLargeMcb(DataRunsMCB);
            return STATUS_FILE_CORRUPT_ERROR;
        }

        if (*DataRun == 0)
            break;

        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength, &IsSparse);

        if (DataRunLength == 0 ||
            DataRunLength > MAXLONGLONG - *pNextVBN ||
            (!IsSparse &&
             ((DataRunOffset < 0 && LastLCN < (ULONGLONG)(-(DataRunOffset + 1)) + 1) ||
              (DataRunOffset >= 0 && LastLCN > MAXLONGLONG - DataRunOffset))))
        {
            FsRtlUninitializeLargeMcb(DataRunsMCB);
            return STATUS_FILE_CORRUPT_ERROR;
        }

        if (!IsSparse)
        {
            // Normal data run.
            DataRunStartLCN = LastLCN + DataRunOffset;
            if (DataRunLength - 1 > MAXLONGLONG - DataRunStartLCN)
            {
                FsRtlUninitializeLargeMcb(DataRunsMCB);
                return STATUS_FILE_CORRUPT_ERROR;
            }
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
* @param VcnCount
* Number of VCNs the runs must describe, HighestVCN + 1. A hole the MCB doesn't
* reach, at the end of a sparse stream, is encoded up to this point.
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
                          ULONGLONG VcnCount,
                          PUCHAR RunBuffer,
                          ULONG MaxBufferSize,
                          PULONG UsedBufferSize)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG RunBufferOffset = 0;
    LONGLONG  DataRunOffset = 0;
    ULONGLONG LastLCN = 0;
    ULONGLONG NextVcn = 0;
    LONGLONG Vbn, Lbn, Count;
    ULONG i;
    BOOLEAN Tail = FALSE;


    DPRINT("\t[Vbn, Lbn, Count]\n");

    if (MaxBufferSize == 0)
    {
        *UsedBufferSize = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    // Encode only the requested VCN range, including any trailing hole.
    for (i = 0; NextVcn < VcnCount; i++)
    {
        UCHAR DataRunOffsetSize = 0;
        UCHAR DataRunLengthSize = 0;
        UCHAR ControlByte = 0;

        if (!FsRtlGetNextLargeMcbEntry(DataRunsMCB, i, &Vbn, &Lbn, &Count))
        {
            if (NextVcn >= VcnCount)
                break;

            /* The MCB only knows mapped clusters, so a hole at the end isn't in it */
            Vbn = NextVcn;
            Lbn = -1;
            Count = VcnCount - NextVcn;
            Tail = TRUE;
        }
        else if (Vbn > (LONGLONG)NextVcn)
        {
            /* FsRtl reports a hole between two runs as a run with no LBN, but encode one
             * that isn't reported the same way; the next entry comes round again */
            Count = Vbn - NextVcn;
            Vbn = NextVcn;
            Lbn = -1;
            i--;
        }

        ASSERT((ULONGLONG)Vbn == NextVcn && Count > 0);
        Count = (LONGLONG)min((ULONGLONG)Count, VcnCount - NextVcn);

        // [vbn, lbn, count]
        DPRINT("\t[%I64d, %I64d,%I64d]\n", Vbn, Lbn, Count);

        // determine how to represent DataRunLengthSize with the minimum number of bytes
        DPRINT("Determining how many bytes needed to represent %I64x\n", Count);
        DataRunLengthSize = GetPackedByteCount(Count, TRUE);
        DPRINT("%d bytes needed.\n", DataRunLengthSize);

        /* A sparse run has no offset, and the next real run is relative to the last real one */
        if (Lbn != -1)
        {
            DataRunOffset = Lbn - LastLCN;
            LastLCN = Lbn;

            // now we need to determine how to represent DataRunOffset with the minimum number of bytes
            DPRINT("Determining how many bytes needed to represent %I64x\n", DataRunOffset);
            DataRunOffsetSize = GetPackedByteCount(DataRunOffset, TRUE);
            DPRINT("%d bytes needed.\n", DataRunOffsetSize);
        }

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

        NextVcn = Vbn + Count;

        if (Tail)
            break;
    }

    // End of data runs
    RunBuffer[RunBufferOffset++] = 0;

    *UsedBufferSize = RunBufferOffset;
    DPRINT("New Size of DataRuns: %ld\n", *UsedBufferSize);

    return Status;
}

PUCHAR
DecodeRun(PUCHAR DataRun,
          LONGLONG *DataRunOffset,
          ULONGLONG *DataRunLength,
          PBOOLEAN IsSparse)
{
    UCHAR DataRunOffsetSize;
    UCHAR DataRunLengthSize;
    CHAR i;

    DataRunOffsetSize = (*DataRun >> 4) & 0xF;
    DataRunLengthSize = *DataRun & 0xF;
    *DataRunOffset = 0;
    *DataRunLength = 0;
    *IsSparse = (DataRunOffsetSize == 0);
    DataRun++;
    for (i = 0; i < DataRunLengthSize; i++)
    {
        *DataRunLength += ((ULONG64)*DataRun) << (i * 8);
        DataRun++;
    }

    /* NTFS 3+ sparse files have no offset at all. This must be reported
     * apart from the offset: it is a signed delta, and a real run one
     * cluster below the previous one decodes to -1 too. */
    if (!*IsSparse)
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
    BOOLEAN IsSparse;

    if (vcn < NresAttr->NonResident.LowestVCN || vcn > NresAttr->NonResident.HighestVCN)
        return FALSE;

    DecodeRun((PUCHAR)((ULONG_PTR)NresAttr + NresAttr->NonResident.MappingPairsOffset), (PLONGLONG)lcn, count, &IsSparse);

    return TRUE;
}

/**
* @name NtfsFreeClusterRange
*
* Returns an allocated range to the bitmap, keeping the in-memory bitmap and
* free-cluster count unchanged if its bitmap write fails.
*/
NTSTATUS
NtfsFreeClusterRange(PDEVICE_EXTENSION Vcb, ULONG FirstCluster, ULONG ClusterCount)
{
    NTSTATUS Status;

    ASSERT(ClusterCount != 0);
    ASSERT((ULONGLONG)FirstCluster + ClusterCount <= Vcb->ClusterBitmap.SizeOfBitMap);
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->BitmapResource, TRUE);
    RtlClearBits(&Vcb->ClusterBitmap, FirstCluster, ClusterCount);
    Status = NtfsWriteVolumeBitmap(Vcb, FirstCluster, ClusterCount);
    if (NT_SUCCESS(Status))
        Vcb->FreeClusterCount += ClusterCount;
    else
        RtlSetBits(&Vcb->ClusterBitmap, FirstCluster, ClusterCount);
    /* The range may have held logged metadata (rollbacks, moves): no
     * record may be replayed over its next owner. */
    if (NT_SUCCESS(Status))
        Status = NtfsLfsCheckpoint(Vcb->LfsWriter);
    ExReleaseResourceLite(&Vcb->BitmapResource);
    KeLeaveCriticalRegion();
    return Status;
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
    PFILE_RECORD_HEADER WriteImage;
    PNTFS_ATTR_RECORD Destination;
    PUCHAR Runs;
    ULONG RunBytes, Freed = 0, Lowest = MAXULONG, Highest = 0;
    ULONGLONG OldCount, NewCount, Vcn;
    LONGLONG Lbn, Count;
    NTSTATUS Status;

    ASSERT(AttrContext->pRecord->IsNonResident);
    if (ClustersToFree == 0)
        return STATUS_SUCCESS;
    OldCount = AttrContext->pRecord->NonResident.HighestVCN + 1;
    ASSERT(ClustersToFree <= OldCount);
    NewCount = OldCount - ClustersToFree;

    /* Prepare the shorter mapping before giving away any clusters. Neither
     * allocation failure nor a failed record write may leave a live mapping
     * pointing into free space. The encoder can clip the existing MCB. */
    Runs = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (Runs == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    WriteImage = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (WriteImage == NULL)
    {
        ExFreePoolWithTag(Runs, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = ConvertLargeMCBToDataRuns(&AttrContext->DataRunsMCB, NewCount, Runs,
                                       Vcb->NtfsInfo.BytesPerFileRecord, &RunBytes);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    RtlCopyMemory(WriteImage, FileRecord, Vcb->NtfsInfo.BytesPerFileRecord);
    Destination = (PNTFS_ATTR_RECORD)((PUCHAR)WriteImage + AttrOffset);
    ASSERT(RunBytes <= Destination->Length - Destination->NonResident.MappingPairsOffset);
    RtlZeroMemory((PUCHAR)Destination + Destination->NonResident.MappingPairsOffset,
                  Destination->Length - Destination->NonResident.MappingPairsOffset);
    RtlCopyMemory((PUCHAR)Destination + Destination->NonResident.MappingPairsOffset, Runs, RunBytes);
    Destination->NonResident.HighestVCN = NewCount - 1;
    Destination->NonResident.AllocatedSize = NewCount * Vcb->NtfsInfo.BytesPerCluster;
    Destination->NonResident.DataSize = min(Destination->NonResident.DataSize, Destination->NonResident.AllocatedSize);
    Destination->NonResident.InitializedSize = min(Destination->NonResident.InitializedSize, Destination->NonResident.DataSize);

    for (Vcn = NewCount; Vcn < OldCount; Vcn += Count)
    {
        if (!FsRtlLookupLargeMcbEntry(&AttrContext->DataRunsMCB, Vcn, &Lbn, &Count, NULL, NULL, NULL))
        {
            Lbn = -1;
            Count = OldCount - Vcn;
        }
        Count = min(Count, OldCount - Vcn);
        ASSERT(Count > 0);
        if (Lbn != -1)
        {
            Freed += (ULONG)Count;
            Lowest = min(Lowest, (ULONG)Lbn);
            Highest = max(Highest, (ULONG)(Lbn + Count - 1));
        }
    }
    if (Destination->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE))
    {
        ASSERT(Destination->NonResident.CompressedSize >= (ULONGLONG)Freed * Vcb->NtfsInfo.BytesPerCluster);
        Destination->NonResident.CompressedSize -= (ULONGLONG)Freed * Vcb->NtfsInfo.BytesPerCluster;
    }

    Status = UpdateFileRecord(Vcb, AttrContext->FileMFTIndex & NTFS_MFT_MASK, WriteImage);
    if (!NT_SUCCESS(Status))
    {
        NTSTATUS RollbackStatus = UpdateFileRecord(Vcb, AttrContext->FileMFTIndex & NTFS_MFT_MASK, FileRecord);
        if (!NT_SUCCESS(RollbackStatus))
            DPRINT1("Couldn't restore allocation before free (Status %lx)\n", RollbackStatus);
        goto Cleanup;
    }

    if (Freed != 0)
    {
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Vcb->BitmapResource, TRUE);
        for (Vcn = NewCount; Vcn < OldCount; Vcn += Count)
        {
            if (!FsRtlLookupLargeMcbEntry(&AttrContext->DataRunsMCB, Vcn, &Lbn, &Count, NULL, NULL, NULL))
                break;
            Count = min(Count, OldCount - Vcn);
            if (Lbn != -1)
                RtlClearBits(&Vcb->ClusterBitmap, (ULONG)Lbn, (ULONG)Count);
        }
        Status = NtfsWriteVolumeBitmap(Vcb, Lowest, Highest - Lowest + 1);
        if (NT_SUCCESS(Status))
            Vcb->FreeClusterCount += Freed;
        else
        {
            /* The shorter record is durable. A failed bitmap write may leak
             * these clusters; it must never leave a live reference to free ones. */
            for (Vcn = NewCount; Vcn < OldCount; Vcn += Count)
            {
                if (!FsRtlLookupLargeMcbEntry(&AttrContext->DataRunsMCB, Vcn, &Lbn, &Count, NULL, NULL, NULL))
                    break;
                Count = min(Count, OldCount - Vcn);
                if (Lbn != -1)
                    RtlSetBits(&Vcb->ClusterBitmap, (ULONG)Lbn, (ULONG)Count);
            }
        }
        /* Logged images of this metadata may still target the clusters:
         * none may be replayed over a later owner's data. */
        if (NT_SUCCESS(Status) && NtfsAttributeIsMetadata(AttrContext))
            Status = NtfsLfsCheckpoint(Vcb->LfsWriter);
        ExReleaseResourceLite(&Vcb->BitmapResource);
        KeLeaveCriticalRegion();
    }

    if (AttrContext == Vcb->MFTContext)
    {
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Vcb->MftContextResource, TRUE);
    }
    FsRtlTruncateLargeMcb(&AttrContext->DataRunsMCB, NewCount);
    RtlCopyMemory(AttrContext->pRecord, Destination, Destination->Length);
    if (AttrContext == Vcb->MFTContext)
    {
        ExReleaseResourceLite(&Vcb->MftContextResource);
        KeLeaveCriticalRegion();
    }
    RtlCopyMemory(FileRecord, WriteImage, Vcb->NtfsInfo.BytesPerFileRecord);
Cleanup:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, WriteImage);
    ExFreePoolWithTag(Runs, TAG_NTFS);
    return Status;
}

/**
* @name NtfsResizeAttributeRecord
*
* Gives an attribute record a new length in place, moving the attributes behind it. The
* bytes it grows by are left for the caller to fill; AttrContext's copy follows.
*/
static
NTSTATUS
NtfsResizeAttributeRecord(PNTFS_VCB Vcb,
                          PNTFS_ATTR_CONTEXT AttrContext,
                          ULONG AttrOffset,
                          PFILE_RECORD_HEADER FileRecord,
                          ULONG NewLength)
{
    PNTFS_ATTR_RECORD Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONG OldLength = Destination->Length;
    PNTFS_ATTR_RECORD Next = (PNTFS_ATTR_RECORD)((ULONG_PTR)Destination + OldLength);
    PNTFS_ATTR_RECORD Copy;

    ASSERT(NewLength % ATTR_RECORD_ALIGNMENT == 0);

    if (NewLength == OldLength)
        return STATUS_SUCCESS;

    if (NewLength > OldLength && FileRecord->BytesInUse + (NewLength - OldLength) > Vcb->NtfsInfo.BytesPerFileRecord)
        return STATUS_DISK_FULL;

    Copy = ExAllocatePoolWithTag(NonPagedPool, NewLength, TAG_NTFS);
    if (Copy == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Next->Type != AttributeEnd)
    {
        PNTFS_ATTR_RECORD Final = MoveAttributes(Vcb, Next, AttrOffset + OldLength, (ULONG_PTR)Destination + NewLength);
        SetFileRecordEnd(FileRecord, Final, FILE_RECORD_END);
    }
    else
    {
        SetFileRecordEnd(FileRecord, (PNTFS_ATTR_RECORD)((ULONG_PTR)Destination + NewLength), FILE_RECORD_END);
    }

    Destination->Length = NewLength;

    RtlZeroMemory(Copy, NewLength);
    RtlCopyMemory(Copy, AttrContext->pRecord, min(OldLength, NewLength));
    Copy->Length = NewLength;
    ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);
    AttrContext->pRecord = Copy;

    return STATUS_SUCCESS;
}

/**
* @name NtfsStoreMappingPairs
* @implemented
*
* Writes the runs of AttrContext's MCB into the attribute, VcnCount clusters of it with any
* trailing hole, resizing the record as needed, and sets HighestVCN and AllocatedSize to
* match. The caller writes FileRecord back.
*/
NTSTATUS
NtfsStoreMappingPairs(PNTFS_VCB Vcb,
                      PNTFS_ATTR_CONTEXT AttrContext,
                      ULONG AttrOffset,
                      PFILE_RECORD_HEADER FileRecord,
                      ULONGLONG VcnCount)
{
    PNTFS_ATTR_RECORD Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    PUCHAR RunBuffer;
    ULONG RunBufferSize;
    ULONG NewLength;
    NTSTATUS Status;

    ASSERT(AttrContext->pRecord->IsNonResident);

    RunBuffer = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (RunBuffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ConvertLargeMCBToDataRuns(&AttrContext->DataRunsMCB, VcnCount, RunBuffer,
                                       Vcb->NtfsInfo.BytesPerFileRecord, &RunBufferSize);
    if (NT_SUCCESS(Status))
    {
        NewLength = ALIGN_UP_BY(Destination->NonResident.MappingPairsOffset + RunBufferSize, ATTR_RECORD_ALIGNMENT);
        Status = NtfsResizeAttributeRecord(Vcb, AttrContext, AttrOffset, FileRecord, NewLength);
    }

    if (NT_SUCCESS(Status))
    {
        Destination->NonResident.HighestVCN = VcnCount - 1;
        Destination->NonResident.AllocatedSize = VcnCount * Vcb->NtfsInfo.BytesPerCluster;
        RtlCopyMemory((PUCHAR)Destination + Destination->NonResident.MappingPairsOffset, RunBuffer, RunBufferSize);

        /* The copy follows the record, header and runs alike */
        RtlCopyMemory(AttrContext->pRecord, Destination, Destination->Length);
    }

    ExFreePoolWithTag(RunBuffer, TAG_NTFS);

    return Status;
}

/**
* @name NtfsSetAttributeUnits
* @implemented
*
* Makes a non-resident attribute sparse (NTFS_ATTR_SPARSE) or LZNT1-compressed
* (NTFS_ATTR_COMPRESSED_LZNT1): it gets the long header that counts the clusters it really
* has, an allocation unit of 16 clusters, and its allocation rounded up to whole units with
* a hole. The caller writes FileRecord back.
*/
NTSTATUS
NtfsSetAttributeUnits(PNTFS_VCB Vcb,
                      PNTFS_ATTR_CONTEXT AttrContext,
                      ULONG AttrOffset,
                      PFILE_RECORD_HEADER FileRecord,
                      USHORT Flag)
{
    const ULONG ShortHeader = FIELD_OFFSET(NTFS_ATTR_RECORD, NonResident.CompressedSize);
    const ULONG LongHeader = sizeof(NTFS_ATTR_RECORD);
    PNTFS_ATTR_RECORD Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONG OldLength = Destination->Length;
    ULONGLONG Allocated = 0, Clusters;
    LONGLONG Vbn, Lbn, Count;
    ULONG i;
    NTSTATUS Status;

    ASSERT(AttrContext->pRecord->IsNonResident);
    ASSERT(Flag == NTFS_ATTR_SPARSE || Flag == NTFS_ATTR_COMPRESSED_LZNT1);

    /* An attribute that has units already has the long header */
    if (!(Destination->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE)))
    {
        ASSERT(Destination->NonResident.MappingPairsOffset >= ShortHeader);

        Status = NtfsResizeAttributeRecord(Vcb, AttrContext, AttrOffset, FileRecord, OldLength + (LongHeader - ShortHeader));
        if (!NT_SUCCESS(Status))
            return Status;

        /* The name and runs move behind the longer header */
        RtlMoveMemory((PUCHAR)Destination + LongHeader, (PUCHAR)Destination + ShortHeader, OldLength - ShortHeader);
        if (Destination->NameLength != 0 && Destination->NameOffset >= ShortHeader)
            Destination->NameOffset += (LongHeader - ShortHeader);
        Destination->NonResident.MappingPairsOffset += (LongHeader - ShortHeader);

        for (i = 0; FsRtlGetNextLargeMcbEntry(&AttrContext->DataRunsMCB, i, &Vbn, &Lbn, &Count); i++)
        {
            if (Lbn != -1)
                Allocated += Count;
        }
        Destination->NonResident.CompressedSize = Allocated * Vcb->NtfsInfo.BytesPerCluster;
    }

    Destination->Flags |= Flag;
    Destination->NonResident.CompressionUnit = NTFS_SPARSE_UNIT_SHIFT;
    RtlCopyMemory(AttrContext->pRecord, Destination, Destination->Length);

    Clusters = Destination->NonResident.AllocatedSize / Vcb->NtfsInfo.BytesPerCluster;
    return NtfsStoreMappingPairs(Vcb, AttrContext, AttrOffset, FileRecord,
                                 ALIGN_UP_BY(Clusters, NtfsSparseUnitClusters(AttrContext)));
}

/**
* @name NtfsClearAttributeUnits
* @implemented
*
* Undoes NtfsSetAttributeUnits() for one flag: once neither is left, the attribute gets back
* the short header and no allocation unit. Only an attribute without clusters may lose its
* units. The caller writes FileRecord back.
*/
NTSTATUS
NtfsClearAttributeUnits(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT AttrContext,
                        ULONG AttrOffset,
                        PFILE_RECORD_HEADER FileRecord,
                        USHORT Flag)
{
    const ULONG ShortHeader = FIELD_OFFSET(NTFS_ATTR_RECORD, NonResident.CompressedSize);
    const ULONG LongHeader = sizeof(NTFS_ATTR_RECORD);
    PNTFS_ATTR_RECORD Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONG OldLength = Destination->Length;

    ASSERT(AttrContext->pRecord->IsNonResident);

    Destination->Flags &= ~Flag;
    if (Destination->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE))
    {
        RtlCopyMemory(AttrContext->pRecord, Destination, Destination->Length);
        return STATUS_SUCCESS;
    }

    ASSERT(Destination->NonResident.CompressedSize == 0);

    /* The name and runs move in behind the shorter header */
    RtlMoveMemory((PUCHAR)Destination + ShortHeader, (PUCHAR)Destination + LongHeader, OldLength - LongHeader);
    if (Destination->NameLength != 0 && Destination->NameOffset >= LongHeader)
        Destination->NameOffset -= (LongHeader - ShortHeader);
    Destination->NonResident.MappingPairsOffset -= (LongHeader - ShortHeader);
    Destination->NonResident.CompressionUnit = 0;
    RtlCopyMemory(AttrContext->pRecord, Destination, OldLength - (LongHeader - ShortHeader));

    return NtfsResizeAttributeRecord(Vcb, AttrContext, AttrOffset, FileRecord, OldLength - (LongHeader - ShortHeader));
}

/**
* @name NtfsMakeAttributeSparse
* @implemented
*
* Makes a non-resident attribute sparse; see NtfsSetAttributeUnits().
*/
NTSTATUS
NtfsMakeAttributeSparse(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT AttrContext,
                        ULONG AttrOffset,
                        PFILE_RECORD_HEADER FileRecord)
{
    ASSERT(!(AttrContext->pRecord->Flags & NTFS_ATTR_COMPRESSION_MASK));

    return NtfsSetAttributeUnits(Vcb, AttrContext, AttrOffset, FileRecord, NTFS_ATTR_SPARSE);
}

/**
* @name NtfsZeroClusters
*
* Writes zeros over clusters on the volume.
*/
static
NTSTATUS
NtfsZeroClusters(PNTFS_VCB Vcb,
                 ULONGLONG Lcn,
                 ULONGLONG Count)
{
    const ULONG ChunkSize = 64 * 1024;
    ULONGLONG Offset = Lcn * Vcb->NtfsInfo.BytesPerCluster;
    ULONGLONG End = (Lcn + Count) * Vcb->NtfsInfo.BytesPerCluster;
    PUCHAR Zeros;
    NTSTATUS Status = STATUS_SUCCESS;

    Zeros = ExAllocatePoolWithTag(NonPagedPool, ChunkSize, TAG_NTFS);
    if (Zeros == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Zeros, ChunkSize);

    for (; Offset < End && NT_SUCCESS(Status); Offset += ChunkSize)
    {
        Status = NtfsWriteDiskCached(Vcb, Offset, (ULONG)min(ChunkSize, End - Offset), Zeros);
    }

    ExFreePoolWithTag(Zeros, TAG_NTFS);

    return Status;
}

/**
* @name NtfsAllocateSparseRange
* @implemented
*
* Gives the holes of an attribute in [StartVcn, StartVcn + VcnCount) clusters, which read as
* zeros like the holes did. Holes belong to sparse attributes, but one without the flag that
* has them is filled the same way. The caller writes FileRecord back.
*/
NTSTATUS
NtfsAllocateSparseRange(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT AttrContext,
                        ULONG AttrOffset,
                        PFILE_RECORD_HEADER FileRecord,
                        ULONGLONG StartVcn,
                        ULONGLONG VcnCount)
{
    PNTFS_ATTR_RECORD Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONGLONG TotalVcns = Destination->NonResident.HighestVCN + 1;
    ULONGLONG Vcn = StartVcn, End = StartVcn + VcnCount;
    ULONG Desired = 0;
    BOOLEAN Changed = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(End <= TotalVcns);

    while (Vcn < End && NT_SUCCESS(Status))
    {
        LONGLONG Lbn, Length;
        ULONG Assigned, First;

        if (!FsRtlLookupLargeMcbEntry(&AttrContext->DataRunsMCB, Vcn, &Lbn, &Length, NULL, NULL, NULL))
        {
            /* Past the last mapping, the rest is a hole */
            Lbn = -1;
            Length = TotalVcns - Vcn;
        }
        Length = min(Length, (LONGLONG)(End - Vcn));

        if (Lbn != -1)
        {
            Desired = (ULONG)(Lbn + Length);
            Vcn += Length;
            continue;
        }

        Status = NtfsAllocateClusters(Vcb, Desired, (ULONG)Length, &First, &Assigned);
        if (NT_SUCCESS(Status))
            Status = NtfsZeroClusters(Vcb, First, Assigned);
        if (NT_SUCCESS(Status) && !FsRtlAddLargeMcbEntry(&AttrContext->DataRunsMCB, Vcn, First, Assigned))
            Status = STATUS_INSUFFICIENT_RESOURCES;

        if (NT_SUCCESS(Status))
        {
            /* Only the long header of a sparse or compressed attribute counts what it has */
            if (Destination->Flags & (NTFS_ATTR_SPARSE | NTFS_ATTR_COMPRESSION_MASK))
            {
                Destination->NonResident.CompressedSize += (ULONGLONG)Assigned * Vcb->NtfsInfo.BytesPerCluster;
                AttrContext->pRecord->NonResident.CompressedSize = Destination->NonResident.CompressedSize;
            }
            Desired = First + Assigned;
            Vcn += Assigned;
            Changed = TRUE;
        }
    }

    if (Changed)
    {
        NTSTATUS StoreStatus = NtfsStoreMappingPairs(Vcb, AttrContext, AttrOffset, FileRecord, TotalVcns);
        if (NT_SUCCESS(Status))
            Status = StoreStatus;
    }

    return Status;
}

/* A range remains allocated until its new mapping is committed. A caller
 * that needs a durable barrier (journal trimming) supplies one before the
 * bitmap can expose the clusters to another file. Failed edits are discarded
 * by the caller; failure never makes a still-referenced cluster reusable. */
NTSTATUS
NtfsDeallocateSparseRangeEx(PDEVICE_EXTENSION Vcb,
                            PNTFS_ATTR_CONTEXT AttrContext,
                            ULONG AttrOffset,
                            PFILE_RECORD_HEADER FileRecord,
                            ULONGLONG StartVcn,
                            ULONGLONG VcnCount,
                            PNTFS_SPARSE_COMMIT_BARRIER Barrier)
{
    typedef struct
    {
        ULONG Lcn;
        ULONG Count;
    } NTFS_RELEASED_RUN;
    NTFS_RELEASED_RUN *Runs;
    PNTFS_ATTR_RECORD Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONGLONG TotalVcns = Destination->NonResident.HighestVCN + 1;
    ULONGLONG Vcn = StartVcn, End;
    ULONG Capacity, Count = 0, i;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(Destination->Flags & (NTFS_ATTR_SPARSE | NTFS_ATTR_COMPRESSION_MASK));
    if (StartVcn >= TotalVcns || VcnCount == 0)
        return STATUS_SUCCESS;
    End = StartVcn + min(VcnCount, TotalVcns - StartVcn);
    Capacity = FsRtlNumberOfRunsInLargeMcb(&AttrContext->DataRunsMCB);
    if (Capacity == 0)
        return STATUS_SUCCESS;
    ASSERT(Capacity <= MAXULONG / sizeof(*Runs));
    Runs = ExAllocatePoolWithTag(NonPagedPool, Capacity * sizeof(*Runs), TAG_NTFS);
    if (Runs == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    while (Vcn < End)
    {
        LONGLONG Lbn, Length;

        if (!FsRtlLookupLargeMcbEntry(&AttrContext->DataRunsMCB, Vcn, &Lbn, &Length, NULL, NULL, NULL))
            break;
        Length = min(Length, (LONGLONG)(End - Vcn));
        ASSERT(Length > 0);
        if (Lbn != -1)
        {
            ASSERT(Count < Capacity);
            ASSERT((ULONGLONG)Lbn + Length <= Vcb->ClusterBitmap.SizeOfBitMap);
            Runs[Count].Lcn = (ULONG)Lbn;
            Runs[Count++].Count = (ULONG)Length;
            FsRtlRemoveLargeMcbEntry(&AttrContext->DataRunsMCB, Vcn, Length);
            ASSERT(Destination->NonResident.CompressedSize >= (ULONGLONG)Length * Vcb->NtfsInfo.BytesPerCluster);
            Destination->NonResident.CompressedSize -= (ULONGLONG)Length * Vcb->NtfsInfo.BytesPerCluster;
            AttrContext->pRecord->NonResident.CompressedSize = Destination->NonResident.CompressedSize;
        }
        Vcn += Length;
    }

    if (Count != 0)
    {
        Status = NtfsStoreMappingPairs(Vcb, AttrContext, AttrOffset, FileRecord, TotalVcns);
        if (NT_SUCCESS(Status))
            Status = NtfsCommitAttributeContext(Vcb, AttrContext, FileRecord);
        if (NT_SUCCESS(Status) && Barrier != NULL)
            Status = Barrier(Vcb);
        if (NT_SUCCESS(Status))
        {
            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&Vcb->BitmapResource, TRUE);
            for (i = 0; i < Count; i++)
            {
                RtlClearBits(&Vcb->ClusterBitmap, Runs[i].Lcn, Runs[i].Count);
                Vcb->FreeClusterCount += Runs[i].Count;
                Status = NtfsWriteVolumeBitmap(Vcb, Runs[i].Lcn, Runs[i].Count);
                if (!NT_SUCCESS(Status))
                {
                    /* Keep a failed release unavailable in memory. Its new
                     * mapping is already safe; repair can reclaim the leak. */
                    RtlSetBits(&Vcb->ClusterBitmap, Runs[i].Lcn, Runs[i].Count);
                    Vcb->FreeClusterCount -= Runs[i].Count;
                    break;
                }
            }
            if (NT_SUCCESS(Status) && NtfsAttributeIsMetadata(AttrContext))
                Status = NtfsLfsCheckpoint(Vcb->LfsWriter);
            ExReleaseResourceLite(&Vcb->BitmapResource);
            KeLeaveCriticalRegion();
        }
    }
    ExFreePoolWithTag(Runs, TAG_NTFS);
    return Status;
}

NTSTATUS
NtfsDeallocateSparseRange(PNTFS_VCB Vcb,
                          PNTFS_ATTR_CONTEXT AttrContext,
                          ULONG AttrOffset,
                          PFILE_RECORD_HEADER FileRecord,
                          ULONGLONG StartVcn,
                          ULONGLONG VcnCount)
{
    return NtfsDeallocateSparseRangeEx(Vcb, AttrContext, AttrOffset, FileRecord,
                                      StartVcn, VcnCount, NULL);
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
    ListSize = AttributeDataLength(ListContext->pRecord);
    if (ListSize > 0xFFFFFFFF)
    {
        ReleaseAttributeContext(ListContext);
        return STATUS_BUFFER_OVERFLOW;
    }

    /* A list being built starts empty: no entries to walk */
    if (ListSize < sizeof(NTFS_ATTRIBUTE_LIST_ITEM))
    {
        ReleaseAttributeContext(ListContext);
        return STATUS_SUCCESS;
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
    if (NextItem->Length == 0 || NextItem->Type == AttributeEnd)
    {
        Context->NonResidentCur = (PVOID)-1;
        return NULL;
    }

    if (NextItem < Context->NonResidentStart || NextItem > Context->NonResidentEnd)
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
    BOOLEAN IsSparse;

    if (CurrentLCN == 0)
    {
        DPRINT("Dumping data runs.\n\tData:\n\t\t");
        NtfsDumpDataRunData(StartOfRun);
        DbgPrint("\n\tRuns:\n\t\tOff\t\tLCN\t\tLength\n");
    }

    DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength, &IsSparse);

    if (!IsSparse)
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

    Status = FindFirstAttribute(&Context, Vcb, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        NtfsDumpAttribute(Vcb, Attribute);

        Status = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
}

PFILENAME_ATTRIBUTE
GetFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                      PFILE_RECORD_HEADER FileRecord,
                      UCHAR NameType)
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
            if (Name->NameType == NameType ||
                (Name->NameType == NTFS_FILE_NAME_WIN32_AND_DOS && NameType == NTFS_FILE_NAME_WIN32) ||
                (Name->NameType == NTFS_FILE_NAME_WIN32_AND_DOS && NameType == NTFS_FILE_NAME_DOS))
            {
                FindCloseAttribute(&Context);
                return Name;
            }
        }

        Status = FindNextAttribute(&Context, &Attribute);
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
    LARGE_MCB Runs;
    ULONGLONG NextVbn = 0;
    LONGLONG Vbn, Lbn, Count;
    ULONG i;
    NTSTATUS Status;

    if (!Attribute->IsNonResident)
        return STATUS_INVALID_PARAMETER;
    if (Attribute->NonResident.MappingPairsOffset >= Attribute->Length)
        return STATUS_FILE_CORRUPT_ERROR;
    Status = ConvertDataRunsToLargeMCB((PUCHAR)Attribute + Attribute->NonResident.MappingPairsOffset,
                                      (PUCHAR)Attribute + Attribute->Length, &Runs, &NextVbn);
    if (!NT_SUCCESS(Status))
        return Status;

    *LastCluster = 0;
    for (i = 0; FsRtlGetNextLargeMcbEntry(&Runs, i, &Vbn, &Lbn, &Count); i++)
    {
        if (Lbn != -1)
            *LastCluster = Lbn + Count - 1;
    }
    FsRtlUninitializeLargeMcb(&Runs);
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

/**
* @name NtfsHasNtfs3StandardInformation
* @implemented
*
* Whether a file's $STANDARD_INFORMATION has the NTFS 3.0 fields: owner, SecurityId, quota
* and USN. One written by NTFS before 3.0 ends before them.
*/
BOOLEAN
NtfsHasNtfs3StandardInformation(PDEVICE_EXTENSION Vcb,
                                PFILE_RECORD_HEADER FileRecord)
{
    PNTFS_ATTR_CONTEXT Context;
    BOOLEAN Has;

    if (!NT_SUCCESS(FindAttribute(Vcb, FileRecord, AttributeStandardInformation, L"", 0, &Context, NULL)))
        return FALSE;

    Has = AttributeDataLength(Context->pRecord) >= sizeof(STANDARD_INFORMATION);
    ReleaseAttributeContext(Context);

    return Has;
}

PFILENAME_ATTRIBUTE
GetBestFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                          PFILE_RECORD_HEADER FileRecord)
{
    PFILENAME_ATTRIBUTE FileName;

    FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_POSIX);
    if (FileName == NULL)
    {
        FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_WIN32);
        if (FileName == NULL)
        {
            FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_DOS);
        }
    }

    return FileName;
}

/* EOF */
