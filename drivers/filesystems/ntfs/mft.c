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
 * FILE:             drivers/filesystem/ntfs/mft.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Valentin Verkhovsky
 *                   Pierre Schweitzer (pierre@reactos.org)
 *                   Hervé Poussineau (hpoussin@reactos.org)
 *                   Trevor Thompson
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"
#include <ntintsafe.h>

#define NDEBUG
#include <debug.h>

#define NTFS_MFT_GROW_RECORDS 4096

/* FUNCTIONS ****************************************************************/

PNTFS_ATTR_CONTEXT
PrepareAttributeContext(PNTFS_ATTR_RECORD AttrRecord)
{
    PNTFS_ATTR_CONTEXT Context;

    Context = ExAllocateFromNPagedLookasideList(&NtfsGlobalData->AttrCtxtLookasideList);
    if(!Context)
    {
        DPRINT1("Error: Unable to allocate memory for context!\n");
        return NULL;
    }
    Context->MigratedToMFTIndex = 0;

    // Allocate memory for a copy of the attribute
    Context->pRecord = ExAllocatePoolWithTag(NonPagedPool, AttrRecord->Length, TAG_NTFS);
    if(!Context->pRecord)
    {
        DPRINT1("Error: Unable to allocate memory for attribute record!\n");
        ExFreeToNPagedLookasideList(&NtfsGlobalData->AttrCtxtLookasideList, Context);
        return NULL;
    }

    // Copy the attribute
    RtlCopyMemory(Context->pRecord, AttrRecord, AttrRecord->Length);

    if (AttrRecord->IsNonResident)
    {
        LONGLONG DataRunOffset;
        ULONGLONG DataRunLength;
        ULONGLONG NextVBN = 0;
        PUCHAR DataRun = (PUCHAR)((ULONG_PTR)Context->pRecord + Context->pRecord->NonResident.MappingPairsOffset);

        Context->CacheRun = DataRun;
        Context->CacheRunOffset = 0;
        Context->CacheRun = DecodeRun(Context->CacheRun, &DataRunOffset, &DataRunLength);
        Context->CacheRunLength = DataRunLength;
        if (DataRunOffset != -1)
        {
            /* Normal run. */
            Context->CacheRunStartLCN =
            Context->CacheRunLastLCN = DataRunOffset;
        }
        else
        {
            /* Sparse run. */
            Context->CacheRunStartLCN = -1;
            Context->CacheRunLastLCN = 0;
        }
        Context->CacheRunCurrentOffset = 0;

        // Convert the data runs to a map control block
        if (!NT_SUCCESS(ConvertDataRunsToLargeMCB(DataRun, &Context->DataRunsMCB, &NextVBN)))
        {
            DPRINT1("Unable to convert data runs to MCB!\n");
            ExFreePoolWithTag(Context->pRecord, TAG_NTFS);
            ExFreeToNPagedLookasideList(&NtfsGlobalData->AttrCtxtLookasideList, Context);
            return NULL;
        }
    }

    return Context;
}


VOID
ReleaseAttributeContext(PNTFS_ATTR_CONTEXT Context)
{
    /* Defense in depth: callers occasionally pass uninitialized stack
     * variables on FindAttribute failure paths.  FindAttribute writes to
     * its out-parameter only on success, so a NULL guard here turns a
     * use-of-uninitialized into a no-op rather than a pool-tracker assert
     * (or a kernel panic from freeing a wild pointer). */
    if (Context == NULL)
        return;

    if (Context->pRecord)
    {
        if (Context->pRecord->IsNonResident)
        {
            FsRtlUninitializeLargeMcb(&Context->DataRunsMCB);
        }

        ExFreePoolWithTag(Context->pRecord, TAG_NTFS);
    }

    ExFreeToNPagedLookasideList(&NtfsGlobalData->AttrCtxtLookasideList, Context);
}


/* NTFS attribute names collate case-insensitively (on-disk ordering and
 * lookups both go through the $UpCase table), so a lookup for "stream" must
 * match a stored "Stream" - creation stays case-preserving.  System names
 * ($I30, $J, ...) are stored upper-case, so they are unaffected. */
static
BOOLEAN
CompareAttributeNames(PCWSTR AttrName,
                      UCHAR AttrNameLength,
                      PCWSTR Name,
                      ULONG NameLength)
{
    UNICODE_STRING Existing, Wanted;

    Existing.Buffer = (PWSTR)AttrName;
    Existing.Length = Existing.MaximumLength = (USHORT)(AttrNameLength * sizeof(WCHAR));
    Wanted.Buffer = (PWSTR)Name;
    Wanted.Length = Wanted.MaximumLength = (USHORT)(NameLength * sizeof(WCHAR));

    return RtlEqualUnicodeString(&Existing, &Wanted, TRUE);
}

/**
* @name FindAttribute
* @implemented
*
* Searches a file record for an attribute matching the given type and name.
*
* @param Offset
* Optional pointer to a ULONG that will receive the offset of the found attribute
* from the beginning of the record. Can be set to NULL.
*/
NTSTATUS
FindAttribute(PDEVICE_EXTENSION Vcb,
              PFILE_RECORD_HEADER MftRecord,
              ULONG Type,
              PCWSTR Name,
              ULONG NameLength,
              PNTFS_ATTR_CONTEXT * AttrCtx,
              PULONG Offset)
{
    BOOLEAN Found;
    NTSTATUS Status;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    PNTFS_ATTRIBUTE_LIST_ITEM AttrListItem;

    DPRINT("FindAttribute(%p, %p, 0x%x, %S, %lu, %p, %p)\n", Vcb, MftRecord, Type, Name, NameLength, AttrCtx, Offset);

    Found = FALSE;
    Status = FindFirstAttribute(&Context, Vcb, MftRecord, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == Type && Attribute->NameLength == NameLength)
        {
            if (NameLength != 0)
            {
                PWCHAR AttrName;

                AttrName = (PWCHAR)((PCHAR)Attribute + Attribute->NameOffset);
                DPRINT("%.*S, %.*S\n", Attribute->NameLength, AttrName, NameLength, Name);
                if (CompareAttributeNames(AttrName, Attribute->NameLength, Name, NameLength))
                {
                    Found = TRUE;
                }
            }
            else
            {
                Found = TRUE;
            }

            if (Found)
            {
                /* Found it, fill up the context and return. */
                DPRINT("Found context\n");
                *AttrCtx = PrepareAttributeContext(Attribute);

                (*AttrCtx)->FileMFTIndex = MftRecord->MFTRecordNumber;

                if (Offset != NULL)
                    *Offset = Context.Offset;

                FindCloseAttribute(&Context);
                return STATUS_SUCCESS;
            }
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }

    /* No attribute found, check if it is referenced in another file record */
    Status = FindFirstAttributeListItem(&Context, &AttrListItem);
    while (NT_SUCCESS(Status))
    {
        if (AttrListItem->Type == Type && AttrListItem->NameLength == NameLength)
        {
            if (NameLength != 0)
            {
                PWCHAR AttrName;

                AttrName = (PWCHAR)((PCHAR)AttrListItem + AttrListItem->NameOffset);
                DPRINT("%.*S, %.*S\n", AttrListItem->NameLength, AttrName, NameLength, Name);
                if (CompareAttributeNames(AttrName, AttrListItem->NameLength, Name, NameLength))
                {
                    Found = TRUE;
                }
            }
            else
            {
                Found = TRUE;
            }

            if (Found == TRUE)
            {
                /* Get the MFT Index of attribute */
                ULONGLONG MftIndex;
                PFILE_RECORD_HEADER RemoteHdr;

                MftIndex = AttrListItem->MFTIndex & NTFS_MFT_MASK;
                RemoteHdr = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);

                if (RemoteHdr == NULL)
                {
                    FindCloseAttribute(&Context);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                /* Check we are not reading ourselves */
                if (MftRecord->MFTRecordNumber == MftIndex)
                {
                    DPRINT1("Attribute list references missing attribute to this file entry !");
                    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, RemoteHdr);
                    FindCloseAttribute(&Context);
                    return STATUS_OBJECT_NAME_NOT_FOUND;
                }
                /* Read the new file record */
                ReadFileRecord(Vcb, MftIndex, RemoteHdr);
                Status = FindAttribute(Vcb, RemoteHdr, Type, Name, NameLength, AttrCtx, Offset);

                /* The recursive FindAttribute on the child record set
                 * (*AttrCtx)->FileMFTIndex = RemoteHdr->MFTRecordNumber
                 * (the child's index, e.g. 27).  The caller however is working
                 * with the BASE record's FileRecord buffer (where the list
                 * lives), so callers like AllocateIndexNode that do
                 * UpdateFileRecord(AttrCtx->FileMFTIndex, FileRecord) would
                 * end up writing the base buffer to the child slot, corrupting
                 * the migrated attribute.  Restore FileMFTIndex to the base
                 * to keep the (FileMFTIndex, FileRecord) pair consistent.
                 *
                 * Phase 4A.5: also stash the child's MFT index in
                 * MigratedToMFTIndex so that AddRun on this AttrContext can
                 * re-read the child record and operate on the actual
                 * attribute slot.  Without this, AddRun would compute
                 * DestinationAttribute = base + AttrOffset and write mapping
                 * pairs into a slot that no longer holds the migrated
                 * attribute (because step 5 of MigrateAttributeToList moved
                 * trailing attributes left into that position). */
                if (NT_SUCCESS(Status))
                {
                    (*AttrCtx)->MigratedToMFTIndex = MftIndex;
                    (*AttrCtx)->FileMFTIndex = MftRecord->MFTRecordNumber;
                }

                ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, RemoteHdr);
                FindCloseAttribute(&Context);
                return Status;
            }
        }
        Status = FindNextAttributeListItem(&Context, &AttrListItem);
    }
    FindCloseAttribute(&Context);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}


ULONGLONG
AttributeAllocatedLength(PNTFS_ATTR_RECORD AttrRecord)
{
    if (AttrRecord->IsNonResident)
        return AttrRecord->NonResident.AllocatedSize;
    else
        return ALIGN_UP_BY(AttrRecord->Resident.ValueLength, ATTR_RECORD_ALIGNMENT);
}


ULONGLONG
AttributeDataLength(PNTFS_ATTR_RECORD AttrRecord)
{
    if (AttrRecord->IsNonResident)
        return AttrRecord->NonResident.DataSize;
    else
        return AttrRecord->Resident.ValueLength;
}

/**
* @name IncreaseMftSize
* @implemented
*
* Increases the size of the master file table on a volume, increasing the space available for file records.
*
* @param Vcb
* Pointer to the VCB (DEVICE_EXTENSION) of the target volume.
*
*
* @param CanWait
* Boolean indicating if the function is allowed to wait for exclusive access to the master file table.
* This will only be relevant if the MFT doesn't have any free file records and needs to be enlarged.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
* STATUS_INVALID_PARAMETER if there was an error reading the Mft's bitmap.
* STATUS_CANT_WAIT if CanWait was FALSE and the function could not get immediate, exclusive access to the MFT.
*
* @remarks
* Increases the size of the Master File Table by 64 records. Bitmap entries for the new records are cleared,
* and the bitmap is also enlarged if needed. Mimicking Windows' behavior when enlarging the mft is still TODO.
* This function will wait for exlusive access to the volume fcb.
*/
NTSTATUS
IncreaseMftSize(PDEVICE_EXTENSION Vcb, BOOLEAN CanWait)
{
    PNTFS_ATTR_CONTEXT BitmapContext;
    LARGE_INTEGER BitmapSize;
    LARGE_INTEGER DataSize;
    LONGLONG BitmapSizeDifference;
    ULONG NewRecords = NTFS_MFT_GROW_RECORDS;
    ULONG DataSizeDifference = Vcb->NtfsInfo.BytesPerFileRecord * NewRecords;
    ULONG BitmapOffset;
    PUCHAR BitmapBuffer;
    ULONGLONG BitmapBytes;
    ULONGLONG NewBitmapSize;
    ULONGLONG FirstNewMftIndex;
    ULONG BytesRead;
    ULONG LengthWritten;
    PFILE_RECORD_HEADER BlankFileRecord;
    ULONG i;
    NTSTATUS Status;

    DPRINT("IncreaseMftSize(%p, %s)\n", Vcb, CanWait ? "TRUE" : "FALSE");

    // We need exclusive access to the mft while we change its size
    if (!ExAcquireResourceExclusiveLite(&(Vcb->DirResource), CanWait))
    {
        return STATUS_CANT_WAIT;
    }

    // Create a blank file record that will be used later
    BlankFileRecord = NtfsCreateEmptyFileRecord(Vcb);
    if (!BlankFileRecord)
    {
        DPRINT1("Error: Unable to create empty file record!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Clear the flags (file record is not in use)
    BlankFileRecord->Flags = 0;

    // Find the bitmap attribute of master file table
    Status = FindAttribute(Vcb, Vcb->MasterFileTable, AttributeBitmap, L"", 0, &BitmapContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find $BITMAP attribute of Mft!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->DirResource));
        return Status;
    }

    // Get size of Bitmap Attribute
    BitmapSize.QuadPart = AttributeDataLength(BitmapContext->pRecord);

    // Calculate the new mft size
    DataSize.QuadPart = AttributeDataLength(Vcb->MFTContext->pRecord) + DataSizeDifference;

    // Find the index of the first Mft entry that will be created
    FirstNewMftIndex = AttributeDataLength(Vcb->MFTContext->pRecord) / Vcb->NtfsInfo.BytesPerFileRecord;

    // Determine how many bytes will make up the bitmap
    BitmapBytes = DataSize.QuadPart / Vcb->NtfsInfo.BytesPerFileRecord / 8;
    if ((DataSize.QuadPart / Vcb->NtfsInfo.BytesPerFileRecord) % 8 != 0)
        BitmapBytes++;

    // Windows will always keep the number of bytes in a bitmap as a multiple of 8, so no bytes are wasted on slack
    BitmapBytes = ALIGN_UP_BY(BitmapBytes, ATTR_RECORD_ALIGNMENT);

    // Determine how much we need to adjust the bitmap size (it's possible we don't)
    BitmapSizeDifference = BitmapBytes - BitmapSize.QuadPart;
    NewBitmapSize = max(BitmapSize.QuadPart + BitmapSizeDifference, BitmapSize.QuadPart);

    // Allocate memory for the bitmap
    BitmapBuffer = ExAllocatePoolWithTag(NonPagedPool, NewBitmapSize, TAG_NTFS);
    if (!BitmapBuffer)
    {
        DPRINT1("ERROR: Unable to allocate memory for bitmap attribute!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->DirResource));
        ReleaseAttributeContext(BitmapContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Zero the bytes we'll be adding
    RtlZeroMemory(BitmapBuffer, NewBitmapSize);

    // Read the bitmap attribute
    BytesRead = ReadAttribute(Vcb,
                              BitmapContext,
                              0,
                              (PCHAR)BitmapBuffer,
                              BitmapSize.LowPart);
    if (BytesRead != BitmapSize.LowPart)
    {
        DPRINT1("ERROR: Bytes read != Bitmap size!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->DirResource));
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return STATUS_INVALID_PARAMETER;
    }

    // Increase the mft size
    Status = SetNonResidentAttributeDataLength(Vcb, Vcb->MFTContext, Vcb->MftDataOffset, Vcb->MasterFileTable, &DataSize);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to set size of $MFT data attribute!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->DirResource));
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return Status;
    }

    // We'll need to find the bitmap again, because its offset will have changed after resizing the data attribute
    ReleaseAttributeContext(BitmapContext);
    Status = FindAttribute(Vcb, Vcb->MasterFileTable, AttributeBitmap, L"", 0, &BitmapContext, &BitmapOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find $BITMAP attribute of Mft!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->DirResource));
        return Status;
    }

    // If the bitmap grew
    if (BitmapSizeDifference > 0)
    {
        // Set the new bitmap size
        BitmapSize.QuadPart = NewBitmapSize;
        if (BitmapContext->pRecord->IsNonResident)
            Status = SetNonResidentAttributeDataLength(Vcb, BitmapContext, BitmapOffset, Vcb->MasterFileTable, &BitmapSize);
        else
            Status = SetResidentAttributeDataLength(Vcb, BitmapContext, BitmapOffset, Vcb->MasterFileTable, &BitmapSize);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to set size of bitmap attribute!\n");
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
            ExReleaseResourceLite(&(Vcb->DirResource));
            ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
            ReleaseAttributeContext(BitmapContext);
            return Status;
        }
    }

    //NtfsDumpFileAttributes(Vcb, Vcb->MasterFileTable);

    // Update the file record with the new attribute sizes
    Status = UpdateFileRecord(Vcb, 0, Vcb->MasterFileTable);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update $MFT file record!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->DirResource));
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return Status;
    }

    // Write out the new bitmap
    Status = WriteAttribute(Vcb, BitmapContext, 0, BitmapBuffer, NewBitmapSize, &LengthWritten, Vcb->MasterFileTable);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->DirResource));
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        DPRINT1("ERROR: Couldn't write to bitmap attribute of $MFT!\n");
        return Status;
    }

    // Create blank records for the new file record entries.
    for (i = 0; i < NewRecords; i++)
    {
        Status = UpdateFileRecord(Vcb, FirstNewMftIndex + i, BlankFileRecord);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to write blank file record!\n");
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
            ExReleaseResourceLite(&(Vcb->DirResource));
            ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
            ReleaseAttributeContext(BitmapContext);
            return Status;
        }
    }

    // Update the mft mirror
    Status = UpdateMftMirror(Vcb);

    // Cleanup
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
    ExReleaseResourceLite(&(Vcb->DirResource));
    ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
    ReleaseAttributeContext(BitmapContext);

    return Status;
}

/**
* @name MoveAttributes
* @implemented
*
* Moves a block of attributes to a new location in the file Record. The attribute at FirstAttributeToMove
* and every attribute after that will be moved to MoveTo.
*
* @param DeviceExt
* Pointer to the DEVICE_EXTENSION (VCB) of the target volume.
*
* @param FirstAttributeToMove
* Pointer to the first NTFS_ATTR_RECORD that needs to be moved. This pointer must reside within a file record.
*
* @param FirstAttributeOffset
* Offset of FirstAttributeToMove relative to the beginning of the file record.
*
* @param MoveTo
* ULONG_PTR with the memory location that will be the new location of the first attribute being moved.
*
* @return
* The new location of the final attribute (i.e. AttributeEnd marker).
*/
PNTFS_ATTR_RECORD
MoveAttributes(PDEVICE_EXTENSION DeviceExt,
               PNTFS_ATTR_RECORD FirstAttributeToMove,
               ULONG FirstAttributeOffset,
               ULONG_PTR MoveTo)
{
    // Get the size of all attributes after this one
    ULONG MemBlockSize = 0;
    PNTFS_ATTR_RECORD CurrentAttribute = FirstAttributeToMove;
    ULONG CurrentOffset = FirstAttributeOffset;
    PNTFS_ATTR_RECORD FinalAttribute;

    while (CurrentAttribute->Type != AttributeEnd && CurrentOffset < DeviceExt->NtfsInfo.BytesPerFileRecord)
    {
        CurrentOffset += CurrentAttribute->Length;
        MemBlockSize += CurrentAttribute->Length;
        CurrentAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)CurrentAttribute + CurrentAttribute->Length);
    }

    FinalAttribute = (PNTFS_ATTR_RECORD)(MoveTo + MemBlockSize);
    MemBlockSize += sizeof(ULONG) * 2;  // Add the AttributeEnd and file record end

    ASSERT(MemBlockSize % ATTR_RECORD_ALIGNMENT == 0);

    // Move the attributes after this one
    RtlMoveMemory((PCHAR)MoveTo, FirstAttributeToMove, MemBlockSize);

    return FinalAttribute;
}

NTSTATUS
InternalSetResidentAttributeLength(PDEVICE_EXTENSION DeviceExt,
                                   PNTFS_ATTR_CONTEXT AttrContext,
                                   PFILE_RECORD_HEADER FileRecord,
                                   ULONG AttrOffset,
                                   ULONG DataSize)
{
    PNTFS_ATTR_RECORD Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    PNTFS_ATTR_RECORD NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Destination + Destination->Length);
    PNTFS_ATTR_RECORD FinalAttribute;
    PNTFS_ATTR_RECORD NewContextRecord = NULL;
    ULONG OldAttributeLength = Destination->Length;
    ULONG OldValueLength = Destination->Resident.ValueLength;
    ULONG NewAttributeLength;
    ULONG NextAttributeOffset;
    ULONG_PTR RecordEnd;
    ULONG_PTR RequiredEnd;

    DPRINT("InternalSetResidentAttributeLength( %p, %p, %p, %lu, %lu )\n", DeviceExt, AttrContext, FileRecord, AttrOffset, DataSize);

    ASSERT(!AttrContext->pRecord->IsNonResident);

    // Calculate the record length and end marker offset
    NewAttributeLength = ALIGN_UP_BY(DataSize + AttrContext->pRecord->Resident.ValueOffset, ATTR_RECORD_ALIGNMENT);
    NextAttributeOffset = AttrOffset + NewAttributeLength;

    // Ensure NextAttributeOffset is aligned to an 8-byte boundary
    ASSERT(NextAttributeOffset % ATTR_RECORD_ALIGNMENT == 0);

    RecordEnd = (ULONG_PTR)FileRecord + DeviceExt->NtfsInfo.BytesPerFileRecord;
    if (NextAttribute->Type != AttributeEnd)
    {
        ULONG TrailingSize = FileRecord->BytesInUse -
                             (ULONG)((ULONG_PTR)NextAttribute - (ULONG_PTR)FileRecord);
        RequiredEnd = (ULONG_PTR)Destination + NewAttributeLength + TrailingSize;
    }
    else
    {
        RequiredEnd = (ULONG_PTR)FileRecord + NextAttributeOffset + sizeof(ULONG) * 2;
    }

    if (RequiredEnd > RecordEnd)
        return STATUS_BUFFER_OVERFLOW;

    // Will the new attribute be larger than the old one?
    if (NewAttributeLength > OldAttributeLength)
    {
        // Create a new copy of the attribute record for the context
        NewContextRecord = ExAllocatePoolWithTag(NonPagedPool, NewAttributeLength, TAG_NTFS);
        if (!NewContextRecord)
        {
            DPRINT1("Unable to allocate memory for attribute!\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory((PVOID)((ULONG_PTR)NewContextRecord + OldAttributeLength),
                      NewAttributeLength - OldAttributeLength);
        RtlCopyMemory(NewContextRecord, Destination, OldAttributeLength);
    }

    // Update ValueLength and Length fields only after all failure preflights.
    Destination->Resident.ValueLength = DataSize;
    Destination->Length = NewAttributeLength;

    NTFS_CHECK_POOL(FileRecord, "InternalSetResidentAttributeLength:pre-move");

    // Are there attributes after this one that need to be moved?
    if (NextAttribute->Type != AttributeEnd)
    {
        // Move the attributes after this one
        FinalAttribute = MoveAttributes(DeviceExt, NextAttribute, NextAttributeOffset, (ULONG_PTR)Destination + Destination->Length);
    }
    else
    {
        // advance to the final "attribute," adjust for the changed length of the attribute we're resizing
        FinalAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute - OldAttributeLength + Destination->Length);
    }

    if (DataSize > OldValueLength)
    {
        RtlZeroMemory((PCHAR)Destination + Destination->Resident.ValueOffset + OldValueLength,
                      DataSize - OldValueLength);
    }

    if (NewContextRecord)
    {
        ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);
        AttrContext->pRecord = NewContextRecord;
    }

    // Update pRecord's length
    AttrContext->pRecord->Length = Destination->Length;
    AttrContext->pRecord->Resident.ValueLength = DataSize;
    RtlCopyMemory(AttrContext->pRecord, Destination, Destination->Length);

    // set the file record end
    SetFileRecordEnd(FileRecord, FinalAttribute, FILE_RECORD_END);

    NTFS_CHECK_POOL(FileRecord, "InternalSetResidentAttributeLength:post");

    //NtfsDumpFileRecord(DeviceExt, FileRecord);

    return STATUS_SUCCESS;
}

/**
*   @parameter FileRecord
*   Pointer to a file record. Must be a full record at least
*   Fcb->Vcb->NtfsInfo.BytesPerFileRecord bytes large, not just the header.
*/
NTSTATUS
SetAttributeDataLength(PFILE_OBJECT FileObject,
                       PNTFS_FCB Fcb,
                       PNTFS_ATTR_CONTEXT AttrContext,
                       ULONG AttrOffset,
                       PFILE_RECORD_HEADER FileRecord,
                       PLARGE_INTEGER DataSize)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONGLONG OldDataSize;
    BOOLEAN Shrinking;

    NTFS_TRACE("SetAttributeDataLength(%p, %p, %p, %lu, %p, %I64u)\n",
            FileObject,
            Fcb,
            AttrContext,
            AttrOffset,
            FileRecord,
            DataSize->QuadPart);

    /* Invalidate FCB-level MFT record cache: the caller-supplied
     * FileRecord buffer (which gets written back via UpdateFileRecord at
     * the bottom of this function) is independent of our cached copy,
     * so the cache is about to be stale. */
    NtfsInvalidateCachedFileRecord(Fcb);

    OldDataSize = AttributeDataLength(AttrContext->pRecord);
    Shrinking = (DataSize->QuadPart < OldDataSize);

    // are we truncating the file?
    if (Shrinking)
    {
        if (!MmCanFileBeTruncated(FileObject->SectionObjectPointer, DataSize))
        {
            DPRINT1("Can't truncate a memory-mapped file!\n");
            return STATUS_USER_MAPPED_FILE;
        }
    }

    if (AttrContext->pRecord->IsNonResident)
    {
        NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: set nonresident begin old=%I64u new=%I64u\n",
                    OldDataSize,
                    DataSize->QuadPart);
        Status = SetNonResidentAttributeDataLength(Fcb->Vcb,
                                                   AttrContext,
                                                   AttrOffset,
                                                   FileRecord,
                                                   DataSize);
        NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: set nonresident returned 0x%lx\n", Status);
    }
    else
    {
        // resident attribute
        NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: set resident begin old=%I64u new=%I64u\n",
                    OldDataSize,
                    DataSize->QuadPart);
        Status = SetResidentAttributeDataLength(Fcb->Vcb,
                                                AttrContext,
                                                AttrOffset,
                                                FileRecord,
                                                DataSize);
        NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: set resident returned 0x%lx nr=%u\n",
                    Status,
                    AttrContext->pRecord->IsNonResident);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to set size of attribute!\n");
        return Status;
    }

    //NtfsDumpFileAttributes(Fcb->Vcb, FileRecord);

    // write the updated file record back to disk
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: update file record begin\n");
    Status = UpdateFileRecord(Fcb->Vcb, Fcb->MFTIndex, FileRecord);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: update file record returned 0x%lx\n", Status);

    if (NT_SUCCESS(Status))
    {
        if (AttrContext->pRecord->IsNonResident)
            Fcb->RFCB.AllocationSize.QuadPart = AttrContext->pRecord->NonResident.AllocatedSize;
        else
            Fcb->RFCB.AllocationSize = *DataSize;
        Fcb->RFCB.FileSize = *DataSize;
        /* ValidDataLength tracks how far real data has actually been written,
         * which is NOT the same as the (possibly larger) file size on an
         * extend.  Raising VDL to FileSize here - before the caller writes the
         * data - would tell the cache manager that the freshly allocated, still
         * uninitialized tail of the file is valid: CcRosEnsureVacbResident
         * would then read those clusters from disk (garbage/stale) instead of
         * zero-filling, and a lazy-writer/trim flush could write an unwritten
         * page tail back as committed data and mark it clean - exactly the
         * "head valid, tail zeroed to the page boundary" corruption seen under
         * memory pressure.  Match FastFat: never advance VDL past the real
         * valid extent here; only clamp it down when the file shrinks below it.
         * The cached write path advances VDL after CcCopyWrite lands the data. */
        if (Fcb->RFCB.ValidDataLength.QuadPart > DataSize->QuadPart)
            Fcb->RFCB.ValidDataLength = *DataSize;

        if (Shrinking && FileObject->SectionObjectPointer)
        {
            /*
             * Flush and purge cached data before shrinking.  Extension only
             * needs CcSetFileSizes; flushing the freshly-created stream here
             * can re-enter the filesystem before the caller writes data.
             */
            if (FileObject->SectionObjectPointer->SharedCacheMap)
            {
                IO_STATUS_BLOCK Iosb;
                CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &Iosb);
            }
            if (FileObject->SectionObjectPointer->DataSectionObject)
            {
                CcPurgeCacheSection(FileObject->SectionObjectPointer,
                                    NULL, 0, FALSE);
            }
        }
        if (FileObject->SectionObjectPointer &&
            FileObject->SectionObjectPointer->SharedCacheMap)
        {
            NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: cc set file sizes begin\n");
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
            NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: cc set file sizes done\n");
        }
    }

    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: set attribute data length return\n");
    return STATUS_SUCCESS;
}

/**
* @name SetFileRecordEnd
* @implemented
*
* This small function sets a new endpoint for the file record. It set's the final
* AttrEnd->Type to AttributeEnd and recalculates the bytes used by the file record.
*
* @param FileRecord
* Pointer to the file record whose endpoint (length) will be set.
*
* @param AttrEnd
* Pointer to section of memory that will receive the AttributeEnd marker. This must point
* to memory allocated for the FileRecord. Must be aligned to an 8-byte boundary (relative to FileRecord).
*
* @param EndMarker
* This value will be written after AttributeEnd but isn't critical at all. When Windows resizes
* a file record, it preserves the final ULONG that previously ended the record, even though this
* value is (to my knowledge) never used. We emulate this behavior.
*
*/
VOID
SetFileRecordEnd(PFILE_RECORD_HEADER FileRecord,
                 PNTFS_ATTR_RECORD AttrEnd,
                 ULONG EndMarker)
{
    // Ensure AttrEnd is aligned on an 8-byte boundary, relative to FileRecord
    ASSERT(((ULONG_PTR)AttrEnd - (ULONG_PTR)FileRecord) % ATTR_RECORD_ALIGNMENT == 0);

    // mark the end of attributes
    AttrEnd->Type = AttributeEnd;

    // Restore the "file-record-end marker." The value is never checked but this behavior is consistent with Win2k3.
    AttrEnd->Length = EndMarker;

    // recalculate bytes in use
    FileRecord->BytesInUse = (ULONG_PTR)AttrEnd - (ULONG_PTR)FileRecord + sizeof(ULONG) * 2;
}

/**
* @name SetNonResidentAttributeDataLength
* @implemented
*
* Called by SetAttributeDataLength() to set the size of a non-resident attribute. Doesn't update the file record.
*
* @param Vcb
* Pointer to a DEVICE_EXTENSION describing the target disk.
*
* @param AttrContext
* PNTFS_ATTR_CONTEXT describing the location of the attribute whose size is being set.
*
* @param AttrOffset
* Offset, from the beginning of the record, of the attribute being sized.
*
* @param FileRecord
* Pointer to a file record containing the attribute to be resized. Must be a complete file record,
* not just the header.
*
* @param DataSize
* Pointer to a LARGE_INTEGER describing the new size of the attribute's data.
*
* @return
* STATUS_SUCCESS on success;
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
* STATUS_INVALID_PARAMETER if we can't find the last cluster in the data run.
*
* @remarks
* Called by SetAttributeDataLength() and IncreaseMftSize(). Use SetAttributeDataLength() unless you have a good
* reason to use this. Doesn't update the file record on disk. Doesn't inform the cache controller of changes with
* any associated files. Synchronization is the callers responsibility.
*/
NTSTATUS
SetNonResidentAttributeDataLength(PDEVICE_EXTENSION Vcb,
                                  PNTFS_ATTR_CONTEXT AttrContext,
                                  ULONG AttrOffset,
                                  PFILE_RECORD_HEADER FileRecord,
                                  PLARGE_INTEGER DataSize)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONGLONG AllocationSize = ROUND_UP(DataSize->QuadPart, BytesPerCluster);
    PNTFS_ATTR_RECORD DestinationAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ULONG PersistedClusters = AttrContext->pRecord->NonResident.AllocatedSize / BytesPerCluster;
    ULONG ExistingClusters = PersistedClusters;
    ULONG ActualClusters = 0;
    LONGLONG LastRunLcn = 0;
    LONGLONG LastRunCount = 0;
    LONG RunCount;

    ASSERT(AttrContext->pRecord->IsNonResident);

    /* Preallocation: for a regular file's unnamed $DATA stream that is being
     * extended, reserve clusters in a geometrically-growing contiguous chunk
     * instead of rounding the allocation up to exactly the new data size.  A
     * file written by many small appends would otherwise grow its allocation
     * one cluster at a time; under concurrent writers those single-cluster
     * allocations interleave across files and shatter each file into hundreds
     * of non-contiguous runs.  Besides being slow, that can grow the encoded
     * mapping pairs past what a single MFT record can hold (~896 bytes for an
     * unnamed $DATA attribute), at which point AddRun fails.  Reserving space
     * ahead keeps the file contiguous so the run list stays small.  This
     * mirrors Windows, where AllocatedSize runs ahead of FileSize while a file
     * is being written and the excess is reclaimed when the file is closed.
     * DataSize / InitializedSize (set below) still track the real written
     * extent, so reads past valid data return zero and on-disk semantics are
     * unchanged - only AllocatedSize (reserved, cluster-aligned) is larger.
     * System metadata files (MFT, $Bitmap, ...) are sized precisely by their
     * own callers and must not be preallocated, so gate on a regular file's
     * unnamed data stream. */
    if (AttrContext->pRecord->Type == AttributeData &&
        AttrContext->pRecord->NameLength == 0 &&
        FileRecord->MFTRecordNumber >= NTFS_FILE_FIRST_USER_FILE &&
        (ULONGLONG)DataSize->QuadPart >= AttrContext->pRecord->NonResident.DataSize)
    {
        /* Growing (or rewriting in place) a regular file's data stream.
         * Reserve clusters ahead, and never drop AllocationSize below what is
         * already reserved - otherwise the truncate branch below would free
         * the preallocated tail on the very next append, thrashing the
         * allocator and re-fragmenting the file.  Only an explicit shrink
         * (DataSize below the current data size, which skips this branch) or a
         * close-time trim reclaims preallocation. */
        ULONGLONG Headroom = (ULONGLONG)DataSize->QuadPart / 4;
        ULONGLONG Reserved;

        if (Headroom > NTFS_DATA_PREALLOC_MAX_BYTES)
            Headroom = NTFS_DATA_PREALLOC_MAX_BYTES;

        Reserved = ROUND_UP((ULONGLONG)DataSize->QuadPart + Headroom, BytesPerCluster);
        if (Reserved < AttrContext->pRecord->NonResident.AllocatedSize)
            Reserved = AttrContext->pRecord->NonResident.AllocatedSize;

        AllocationSize = Reserved;
    }

    ActualClusters = 0;
    RunCount = FsRtlNumberOfRunsInLargeMcb(&AttrContext->DataRunsMCB);
    if (RunCount != 0)
    {
        LONGLONG LastVbn;
        LONGLONG LastLbn;
        LONGLONG LastCount;

        if (FsRtlGetNextLargeMcbEntry(&AttrContext->DataRunsMCB,
                                      RunCount - 1,
                                      &LastVbn,
                                      &LastLbn,
                                      &LastCount))
        {
            ActualClusters = (ULONG)(LastVbn + LastCount);
            LastRunLcn = LastLbn;
            LastRunCount = LastCount;
        }
    }

    ExistingClusters = ActualClusters;
    if (ActualClusters > PersistedClusters)
        AllocationSize = max(AllocationSize, (ULONGLONG)ActualClusters * BytesPerCluster);

    // do we need to increase the allocation size?
    if ((ULONGLONG)ExistingClusters * BytesPerCluster < AllocationSize)
    {
        ULONG ClustersNeeded = (AllocationSize / BytesPerCluster) - ExistingClusters;
        LARGE_INTEGER LastClusterInDataRun;
        ULONG NextAssignedCluster;
        ULONG AssignedClusters;

        /* Pre-set the size fields (NOT HighestVCN - that's still maintained
         * incrementally by AddRun, and the MCB-lookup code below relies on it
         * matching the current MCB state) so that anything which copies
         * AttrContext->pRecord during the AddRun loop - notably
         * MigrateAttributeToList - captures the new sizes.  Without this,
         * a migrated attribute lands on disk with stale AllocatedSize/DataSize. */
        AttrContext->pRecord->NonResident.AllocatedSize = AllocationSize;
        AttrContext->pRecord->NonResident.DataSize = DataSize->QuadPart;
        AttrContext->pRecord->NonResident.InitializedSize = DataSize->QuadPart;
        if (DestinationAttribute->Type == AttrContext->pRecord->Type)
        {
            DestinationAttribute->NonResident.AllocatedSize = AllocationSize;
            DestinationAttribute->NonResident.DataSize = DataSize->QuadPart;
            DestinationAttribute->NonResident.InitializedSize = DataSize->QuadPart;
        }

        if (ExistingClusters == 0)
        {
            LastClusterInDataRun.QuadPart = 0;
        }
        else
        {
            if (RunCount == 0 || LastRunLcn == -1 || LastRunCount <= 0)
            {
                DPRINT1("Error looking up final large MCB entry!\n");

                // Most likely, HighestVCN went above the largest mapping
                DPRINT1("Highest VCN of record: %I64u\n", AttrContext->pRecord->NonResident.HighestVCN);
                return STATUS_INVALID_PARAMETER;
            }

            LastClusterInDataRun.QuadPart = LastRunLcn + LastRunCount - 1;
        }

        DPRINT("LastClusterInDataRun: %I64u\n", LastClusterInDataRun.QuadPart);
        DPRINT("Highest VCN of record: %I64u\n", AttrContext->pRecord->NonResident.HighestVCN);

        while (ClustersNeeded > 0)
        {
            Status = NtfsAllocateClusters(Vcb,
                                          LastClusterInDataRun.LowPart + 1,
                                          ClustersNeeded,
                                          &NextAssignedCluster,
                                          &AssignedClusters);

            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Error: Unable to allocate requested clusters!\n");
                return Status;
            }

            // now we need to add the clusters we allocated to the data run
            Status = AddRun(Vcb, AttrContext, AttrOffset, FileRecord, NextAssignedCluster, AssignedClusters);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Error: Unable to add data run!\n");
                return Status;
            }

            ClustersNeeded -= AssignedClusters;
            LastClusterInDataRun.LowPart = NextAssignedCluster + AssignedClusters - 1;
        }
    }
    else if (AttrContext->pRecord->NonResident.AllocatedSize > AllocationSize)
    {
        ULONG NewClusters = AllocationSize / BytesPerCluster;

        if (ActualClusters > NewClusters)
        {
            ULONG ClustersToFree = ActualClusters - NewClusters;
            Status = FreeClusters(Vcb, AttrContext, AttrOffset, FileRecord, ClustersToFree);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR: FreeClusters failed 0x%lx (MFT %p, asked=%lu)\n",
                        Status, FileRecord, ClustersToFree);
                return Status;
            }
        }
    }

    /* Compressed / encrypted / sparse attribute sizing: slice 1a only
     * implements the compressed-$DATA READ path (NtfsCompressedReadLogical
     * in compress.c).  Resizing a compressed attribute would require
     * re-encoding the tail compression unit and re-packing the mapping
     * pairs around the compressed-tail sparse marker, which is the write
     * side (slice 1b, separate issue).  For now, non-zero Flags on the
     * attribute fall through here: the byte sizes below are still
     * persisted, but the caller (SetAttributeDataLength) already rejects
     * writes to compressed attributes upstream.  Encrypted / pure sparse
     * pre-existing behaviour is unchanged. */

    RunCount = FsRtlNumberOfRunsInLargeMcb(&AttrContext->DataRunsMCB);
    if (RunCount != 0)
    {
        LONGLONG LastVbn;
        LONGLONG LastLbn;
        LONGLONG LastCount;

        if (FsRtlGetNextLargeMcbEntry(&AttrContext->DataRunsMCB,
                                      RunCount - 1,
                                      &LastVbn,
                                      &LastLbn,
                                      &LastCount))
        {
            ActualClusters = (ULONG)(LastVbn + LastCount);
        }
    }

    AllocationSize = max(AllocationSize, (ULONGLONG)ActualClusters * BytesPerCluster);

    AttrContext->pRecord->NonResident.AllocatedSize = AllocationSize;
    AttrContext->pRecord->NonResident.DataSize = DataSize->QuadPart;
    AttrContext->pRecord->NonResident.InitializedSize = DataSize->QuadPart;

    /* The in-buffer attribute slot at FileRecord+AttrOffset may have become
     * stale if AddRun() above migrated the attribute to a child file record
     * via $ATTRIBUTE_LIST.  In that case the slot at the original offset is
     * now occupied by a *different* attribute (a moved trailing one), and
     * writing the size fields here would corrupt it.  Detect the migration
     * by comparing types: if they no longer match, the in-buffer copy is
     * now in the child record (which AddRun already wrote back to disk), so
     * skip the in-buffer mirror update. */
    if (DestinationAttribute->Type == AttrContext->pRecord->Type)
    {
        DestinationAttribute->NonResident.AllocatedSize = AllocationSize;
        DestinationAttribute->NonResident.DataSize = DataSize->QuadPart;
        DestinationAttribute->NonResident.InitializedSize = DataSize->QuadPart;
    }

    AttrContext->pRecord->NonResident.HighestVCN = ActualClusters ? ActualClusters - 1 : 0;
    if (DestinationAttribute->Type == AttrContext->pRecord->Type)
        DestinationAttribute->NonResident.HighestVCN = AttrContext->pRecord->NonResident.HighestVCN;

    DPRINT("Allocated Size: %I64u\n", AttrContext->pRecord->NonResident.AllocatedSize);

    return Status;
}

/**
* @name SetResidentAttributeDataLength
* @implemented
*
* Called by SetAttributeDataLength() to set the size of a non-resident attribute. Doesn't update the file record.
*
* @param Vcb
* Pointer to a DEVICE_EXTENSION describing the target disk.
*
* @param AttrContext
* PNTFS_ATTR_CONTEXT describing the location of the attribute whose size is being set.
*
* @param AttrOffset
* Offset, from the beginning of the record, of the attribute being sized.
*
* @param FileRecord
* Pointer to a file record containing the attribute to be resized. Must be a complete file record,
* not just the header.
*
* @param DataSize
* Pointer to a LARGE_INTEGER describing the new size of the attribute's data.
*
* @return
* STATUS_SUCCESS on success;
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
* STATUS_INVALID_PARAMETER if AttrContext describes a non-resident attribute.
* STATUS_NOT_IMPLEMENTED if requested to decrease the size of an attribute that isn't the
* last attribute listed in the file record.
*
* @remarks
* Called by SetAttributeDataLength() and IncreaseMftSize(). Use SetAttributeDataLength() unless you have a good
* reason to use this. Doesn't update the file record on disk. Doesn't inform the cache controller of changes with
* any associated files. Synchronization is the callers responsibility.
*/
NTSTATUS
SetResidentAttributeDataLength(PDEVICE_EXTENSION Vcb,
                               PNTFS_ATTR_CONTEXT AttrContext,
                               ULONG AttrOffset,
                               PFILE_RECORD_HEADER FileRecord,
                               PLARGE_INTEGER DataSize)
{
    NTSTATUS Status;

    // find the next attribute
    ULONG NextAttributeOffset = AttrOffset + AttrContext->pRecord->Length;
    PNTFS_ATTR_RECORD NextAttribute = (PNTFS_ATTR_RECORD)((PCHAR)FileRecord + NextAttributeOffset);

    ASSERT(!AttrContext->pRecord->IsNonResident);

    //NtfsDumpFileAttributes(Vcb, FileRecord);

    // Do we need to increase the data length?
    if (DataSize->QuadPart > AttrContext->pRecord->Resident.ValueLength)
    {
        // There's usually padding at the end of a record. Do we need to extend past it?
        ULONG MaxValueLength = AttrContext->pRecord->Length - AttrContext->pRecord->Resident.ValueOffset;
        if (MaxValueLength < DataSize->LowPart)
        {
            // If this is the last attribute, we could move the end marker to the very end of the file record
            MaxValueLength += Vcb->NtfsInfo.BytesPerFileRecord - NextAttributeOffset - (sizeof(ULONG) * 2);

            if (MaxValueLength < DataSize->LowPart || NextAttribute->Type != AttributeEnd)
            {
                // convert attribute to non-resident
                PNTFS_ATTR_RECORD Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
                PNTFS_ATTR_RECORD NewRecord;
                LARGE_INTEGER AttribDataSize;
                PVOID AttribData;
                ULONG NewRecordLength;
                ULONG OldRecordLength;
                ULONG TailLength;
                ULONG EndAttributeOffset;
                ULONG LengthWritten;

                DPRINT("Converting attribute to non-resident.\n");

                AttribDataSize.QuadPart = AttrContext->pRecord->Resident.ValueLength;

                // Is there existing data we need to back-up?
                if (AttribDataSize.QuadPart > 0)
                {
                    AttribData = ExAllocatePoolWithTag(NonPagedPool, AttribDataSize.QuadPart, TAG_NTFS);
                    if (AttribData == NULL)
                    {
                        DPRINT1("ERROR: Couldn't allocate memory for attribute data. Can't migrate to non-resident!\n");
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    // read data to temp buffer
                    Status = ReadAttribute(Vcb, AttrContext, 0, AttribData, AttribDataSize.QuadPart);
                    if (!NT_SUCCESS(Status))
                    {
                        DPRINT1("ERROR: Unable to read attribute before migrating!\n");
                        ExFreePoolWithTag(AttribData, TAG_NTFS);
                        return Status;
                    }
                }

                // Start by turning this attribute into a 0-length, non-resident attribute, then enlarge it.

                // The size of a 0-length, non-resident attribute will be 0x41 + the size of the attribute name, aligned to an 8-byte boundary
                NewRecordLength = ALIGN_UP_BY(0x41 + (AttrContext->pRecord->NameLength * sizeof(WCHAR)), ATTR_RECORD_ALIGNMENT);

                // Create a new attribute record that will store the 0-length, non-resident attribute
                NewRecord = ExAllocatePoolWithTag(NonPagedPool, NewRecordLength, TAG_NTFS);

                // Zero out the NonResident structure
                RtlZeroMemory(NewRecord, NewRecordLength);

                // Copy the data that's common to both non-resident and resident attributes
                RtlCopyMemory(NewRecord, AttrContext->pRecord, FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.ValueLength));

                // if there's a name
                if (AttrContext->pRecord->NameLength != 0)
                {
                    // copy the name
                    // An attribute name will be located at offset 0x18 for a resident attribute, 0x40 for non-resident
                    RtlCopyMemory((PCHAR)((ULONG_PTR)NewRecord + 0x40),
                                  (PCHAR)((ULONG_PTR)AttrContext->pRecord
                                          + AttrContext->pRecord->NameOffset),
                                  AttrContext->pRecord->NameLength * sizeof(WCHAR));
                }

                /* The header copy above brought the resident NameOffset (0x18)
                 * along; the name now lives after the non-resident header.
                 * Without this a converted named stream can no longer be
                 * found by name (its NameOffset points into LowestVCN). */
                NewRecord->NameOffset = 0x40;

                // update the mapping pairs offset, which will be 0x40 (size of a non-resident header) + length in bytes of the name
                NewRecord->NonResident.MappingPairsOffset = 0x40 + (AttrContext->pRecord->NameLength * sizeof(WCHAR));

                // update the end of the file record
                // calculate position of end markers (1 byte for empty data run)
                EndAttributeOffset = AttrOffset + NewRecord->NonResident.MappingPairsOffset + 1;
                EndAttributeOffset = ALIGN_UP_BY(EndAttributeOffset, ATTR_RECORD_ALIGNMENT);

                // Update the length
                NewRecord->Length = EndAttributeOffset - AttrOffset;

                ASSERT(NewRecord->Length == NewRecordLength);

                /* The converted attribute need not be the last one in the
                 * record - a named data stream sorts wherever its name falls,
                 * and the unnamed $DATA of a file that also has named streams
                 * always has attributes behind it.  Preserve the tail
                 * (everything from the end of the old resident attribute up
                 * to and including the AttributeEnd marker) by sliding it to
                 * the end of the new attribute; the previous code ended the
                 * record right here, silently destroying the trailing
                 * attributes. */
                OldRecordLength = Destination->Length;
                ASSERT(AttrOffset + OldRecordLength <= FileRecord->BytesInUse);
                TailLength = FileRecord->BytesInUse - (AttrOffset + OldRecordLength);

                if (NewRecordLength > OldRecordLength &&
                    FileRecord->BytesInUse + (NewRecordLength - OldRecordLength) >
                        Vcb->NtfsInfo.BytesPerFileRecord)
                {
                    DPRINT1("ERROR: No room to convert attribute to non-resident!\n");
                    if (AttribDataSize.QuadPart > 0)
                        ExFreePoolWithTag(AttribData, TAG_NTFS);
                    ExFreePoolWithTag(NewRecord, TAG_NTFS);
                    return STATUS_DISK_FULL;
                }

                RtlMoveMemory((PUCHAR)FileRecord + AttrOffset + NewRecordLength,
                              (PUCHAR)FileRecord + AttrOffset + OldRecordLength,
                              TailLength);
                FileRecord->BytesInUse = FileRecord->BytesInUse - OldRecordLength + NewRecordLength;

                // Copy the new attribute record into the file record
                RtlCopyMemory(Destination, NewRecord, NewRecord->Length);

                // Initialize the MCB, potentially catch an exception
                _SEH2_TRY
                {
                    FsRtlInitializeLargeMcb(&AttrContext->DataRunsMCB, NonPagedPool);
                }
                _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                {
                    DPRINT1("Unable to create LargeMcb!\n");
                    if (AttribDataSize.QuadPart > 0)
                        ExFreePoolWithTag(AttribData, TAG_NTFS);
                    ExFreePoolWithTag(NewRecord, TAG_NTFS);
                    _SEH2_YIELD(return _SEH2_GetExceptionCode());
                } _SEH2_END;

                // Mark the attribute as non-resident (we wait until after we know the LargeMcb was initialized)
                NewRecord->IsNonResident = Destination->IsNonResident = 1;

                // Update file record on disk
                Status = UpdateFileRecord(Vcb, AttrContext->FileMFTIndex, FileRecord);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Couldn't update file record to continue migration!\n");
                    if (AttribDataSize.QuadPart > 0)
                        ExFreePoolWithTag(AttribData, TAG_NTFS);
                    ExFreePoolWithTag(NewRecord, TAG_NTFS);
                    return Status;
                }

                // Now we need to free the old copy of the attribute record in the context and replace it with the new one
                ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);
                AttrContext->pRecord = NewRecord;

                // Now we can treat the attribute as non-resident and enlarge it normally
                Status = SetNonResidentAttributeDataLength(Vcb, AttrContext, AttrOffset, FileRecord, DataSize);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Unable to migrate resident attribute!\n");
                    if (AttribDataSize.QuadPart > 0)
                        ExFreePoolWithTag(AttribData, TAG_NTFS);
                    return Status;
                }

                // restore the back-up attribute, if we made one
                if (AttribDataSize.QuadPart > 0)
                {
                    Status = WriteAttribute(Vcb, AttrContext, 0, AttribData, AttribDataSize.QuadPart, &LengthWritten, FileRecord);
                    if (!NT_SUCCESS(Status))
                    {
                        DPRINT1("ERROR: Unable to write attribute data to non-resident clusters during migration!\n");
                        // TODO: Reverse migration so no data is lost
                        ExFreePoolWithTag(AttribData, TAG_NTFS);
                        return Status;
                    }

                    ExFreePoolWithTag(AttribData, TAG_NTFS);
                }
            }
        }
    }

    // set the new length of the resident attribute (if we didn't migrate it)
    if (!AttrContext->pRecord->IsNonResident)
        return InternalSetResidentAttributeLength(Vcb, AttrContext, FileRecord, AttrOffset, DataSize->LowPart);

    return STATUS_SUCCESS;
}

ULONG
ReadAttribute(PDEVICE_EXTENSION Vcb,
              PNTFS_ATTR_CONTEXT Context,
              ULONGLONG Offset,
              PCHAR Buffer,
              ULONG Length)
{
    ULONGLONG LastLCN;
    PUCHAR DataRun;
    LONGLONG DataRunOffset;
    ULONGLONG DataRunLength;
    LONGLONG DataRunStartLCN;
    ULONGLONG CurrentOffset;
    ULONG AlreadyRead;
    NTSTATUS Status;
    BOOLEAN DirectDiskRead;

    if (!Context->pRecord->IsNonResident)
    {
        // We need to truncate Offset to a ULONG for pointer arithmetic
        // The check below should ensure that Offset is well within the range of 32 bits
        ULONG LittleOffset = (ULONG)Offset;

        // Ensure that offset isn't beyond the end of the attribute
        if (Offset > Context->pRecord->Resident.ValueLength)
            return 0;
        if (Offset + Length > Context->pRecord->Resident.ValueLength)
            Length = (ULONG)(Context->pRecord->Resident.ValueLength - Offset);

        RtlCopyMemory(Buffer, (PVOID)((ULONG_PTR)Context->pRecord + Context->pRecord->Resident.ValueOffset + LittleOffset), Length);
        return Length;
    }

    /*
     * Non-resident attribute
     */
    /*
     * Route metadata reads (the $MFT $DATA stream, $INDEX_ALLOCATION nodes,
     * $BITMAP, etc.) through the volume stream cache just like file $DATA.
     * WriteAttribute() always writes through NtfsWriteDiskCached(), which keeps
     * that cache coherent for every attribute type, so cached reads observe the
     * bytes that last reached disk.  Reading this metadata uncached forced a
     * synchronous device IRP for every MFT record, B-tree node and bitmap
     * lookup; during install the resulting tens of thousands of tiny blocking
     * reads dominated wall-clock time.  NtfsReadDiskCached() falls back to a
     * raw NtfsReadDisk() while the volume cache is not yet initialised (mount
     * bootstrap), so early MFT reads remain correct.
     */
    DirectDiskRead = FALSE;

    /*
     * Compressed $DATA fast path (issue #35, slice 1a).  A non-zero
     * CompressionUnit means the attribute stores its clusters in
     * LZNT1-compressed "compression units" of 1 << CompressionUnit
     * clusters.  The verbatim MCB walk below would hand callers the
     * raw compressed bytes; route through NtfsCompressedReadLogical
     * instead, which decompresses per-CU and zero-fills sparse CUs.
     * Encrypted / other attribute flags still fall through to the
     * legacy walker (they currently return raw bytes - not regressed
     * by this change). */
    if (Context->pRecord->NonResident.CompressionUnit != 0)
    {
        ULONG CompBytesRead = 0;
        NTSTATUS CompStatus;

        CompStatus = NtfsCompressedReadLogical(Vcb, Context, Offset, Length,
                                               (PUCHAR)Buffer, &CompBytesRead);
        if (!NT_SUCCESS(CompStatus))
        {
            DPRINT1("NtfsCompressedReadLogical failed 0x%x\n", (unsigned)CompStatus);
            return 0;
        }
        return CompBytesRead;
    }

    /*
     * I. Find the corresponding start data run.
     */

    AlreadyRead = 0;

    /*
     * Walk the encoded mapping pairs stored inside the attribute record
     * itself.  PrepareAttributeContext() copies the on-disk attribute
     * (including its data runs) into Context->pRecord, and AddRun() keeps
     * that region in sync whenever the runlist grows, so the bytes at
     * NonResident.MappingPairsOffset are an always-current encoded runlist.
     *
     * Walking them directly avoids the per-call non-paged pool allocation
     * and full MCB <-> data-run round-trip that the previous implementation
     * performed for every ReadAttribute call.  That round-trip dominated
     * cold-load latency for paging I/O against the MFT and large
     * non-resident $DATA attributes (every 64 KB paging chunk re-decoded
     * the entire mapping pairs list from VBN 0).
     *
     * The historic CacheRun fast path on PNTFS_ATTR_CONTEXT is deliberately
     * not consulted here: PrepareAttributeContext / FindAttribute are cheap
     * relative to the old round-trip, NtfsReadFile allocates a fresh context
     * per IRP, and the shared Vcb->MFTContext is touched concurrently by
     * unsynchronized paths -- a stale or torn cache snapshot was the root
     * cause of the original "cache gives wrong results" disable from 2010.
     */
    DataRun = (PUCHAR)((ULONG_PTR)Context->pRecord +
                       Context->pRecord->NonResident.MappingPairsOffset);
    LastLCN = 0;
    CurrentOffset = 0;

    while (1)
    {
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);
        if (DataRunOffset != -1)
        {
            /* Normal data run. */
            DataRunStartLCN = LastLCN + DataRunOffset;
            LastLCN = DataRunStartLCN;
        }
        else
        {
            /* Sparse data run. */
            DataRunStartLCN = -1;
        }

        if (Offset >= CurrentOffset &&
            Offset < CurrentOffset + (DataRunLength * Vcb->NtfsInfo.BytesPerCluster))
        {
            break;
        }

        if (*DataRun == 0)
            return AlreadyRead;

        CurrentOffset += DataRunLength * Vcb->NtfsInfo.BytesPerCluster;
    }

    /*
     * II. Go through the run list and read the data.
     *
     * Coalesce physically-adjacent data runs into a single NtfsReadDiskCached
     * call.  NTFS write allocators (notably libntfs-3g's MFT growth path and
     * post-resize $DATA layouts) frequently split a single contiguous extent
     * into multiple mapping-pair entries whose LCNs are still consecutive on
     * disk.  Issuing one IRP per run on those layouts pays per-IRP BOT
     * command/data/status overhead on USB-MSC for no reason.
     *
     * Invariants enforced below:
     *   - Sparse segments (DiskOffset == -1) are flushed as RtlZeroMemory
     *     and NEVER merged with a real-disk segment.
     *   - Real-disk segments merge iff the next run starts at exactly the
     *     end of the pending real-disk segment AND the merged length stays
     *     within NTFS_MAX_COALESCED_READ.
     *   - The total pending length always corresponds to a contiguous slice
     *     of the destination Buffer; we ASSERT that flushing leaves the
     *     accumulated AlreadyRead consistent.
     */
#define NTFS_MAX_COALESCED_READ (4UL * 1024 * 1024)

    {
        LONGLONG PendingDiskOffset = -1;   /* -1 => no pending segment.    */
        ULONG    PendingLength     = 0;
        PUCHAR   PendingBuffer     = NULL;
        BOOLEAN  PendingIsSparse   = FALSE;
        ULONG    RunBytesAvailable;
        ULONG    RunBytesToConsume;
        ULONGLONG IntraRunOffset;

        Status = STATUS_SUCCESS;

        /* First iteration honours the intra-run offset; subsequent runs
         * start at their first byte. */
        IntraRunOffset = Offset - CurrentOffset;

        while (Length > 0)
        {
            RunBytesAvailable = (ULONG)min(DataRunLength * Vcb->NtfsInfo.BytesPerCluster - IntraRunOffset,
                                           (ULONGLONG)Length);
            RunBytesToConsume = RunBytesAvailable;

            if (DataRunStartLCN == -1)
            {
                /* Sparse: flush any pending real-disk segment first, then
                 * either extend a pending sparse segment or start one. */
                if (!PendingIsSparse && PendingLength != 0)
                {
                    if (DirectDiskRead)
                    {
                        Status = NtfsReadDisk(Vcb->StorageDevice,
                                              PendingDiskOffset,
                                              PendingLength,
                                              Vcb->NtfsInfo.BytesPerSector,
                                              PendingBuffer,
                                              FALSE);
                    }
                    else
                    {
                        Status = NtfsReadDiskCached(Vcb,
                                                    PendingDiskOffset,
                                                    PendingLength,
                                                    PendingBuffer);
                    }
                    if (!NT_SUCCESS(Status))
                        break;
                    AlreadyRead += PendingLength;
                    PendingLength = 0;
                }

                if (PendingLength == 0)
                {
                    PendingIsSparse = TRUE;
                    PendingBuffer = (PUCHAR)Buffer;
                    PendingDiskOffset = -1;
                }

                /* Cap the sparse segment too so a giant sparse hole doesn't
                 * cause us to issue a single multi-megabyte RtlZeroMemory
                 * while real-disk segments are queued behind it. */
                if (PendingLength + RunBytesToConsume > NTFS_MAX_COALESCED_READ)
                {
                    RtlZeroMemory(PendingBuffer, PendingLength);
                    AlreadyRead += PendingLength;
                    PendingLength = 0;
                    PendingBuffer = (PUCHAR)Buffer;
                }

                PendingLength += RunBytesToConsume;
            }
            else
            {
                LONGLONG RunDiskOffset = DataRunStartLCN * Vcb->NtfsInfo.BytesPerCluster + IntraRunOffset;

                /* If we have a pending sparse segment, flush it before
                 * starting a real read. */
                if (PendingIsSparse && PendingLength != 0)
                {
                    RtlZeroMemory(PendingBuffer, PendingLength);
                    AlreadyRead += PendingLength;
                    PendingLength = 0;
                }

                /* Merge with a pending real-disk segment when physically
                 * adjacent and within the size cap. */
                if (!PendingIsSparse &&
                    PendingLength != 0 &&
                    PendingDiskOffset + (LONGLONG)PendingLength == RunDiskOffset &&
                    PendingLength + RunBytesToConsume <= NTFS_MAX_COALESCED_READ)
                {
                    ASSERT(PendingBuffer + PendingLength == (PUCHAR)Buffer);
                    PendingLength += RunBytesToConsume;
                }
                else
                {
                    /* Flush any pending real-disk segment that can't be
                     * merged (different LCN or would overflow the cap). */
                    if (PendingLength != 0)
                    {
                        ASSERT(!PendingIsSparse);
                        if (DirectDiskRead)
                        {
                            Status = NtfsReadDisk(Vcb->StorageDevice,
                                                  PendingDiskOffset,
                                                  PendingLength,
                                                  Vcb->NtfsInfo.BytesPerSector,
                                                  PendingBuffer,
                                                  FALSE);
                        }
                        else
                        {
                            Status = NtfsReadDiskCached(Vcb,
                                                        PendingDiskOffset,
                                                        PendingLength,
                                                        PendingBuffer);
                        }
                        if (!NT_SUCCESS(Status))
                            break;
                        AlreadyRead += PendingLength;
                    }

                    PendingIsSparse = FALSE;
                    PendingBuffer = (PUCHAR)Buffer;
                    PendingDiskOffset = RunDiskOffset;
                    PendingLength = RunBytesToConsume;
                }
            }

            Length -= RunBytesToConsume;
            Buffer += RunBytesToConsume;

            /* Advance through the current run; if we consumed all of it
             * (which is always the case here, since RunBytesAvailable is
             * the whole remainder of this run unless Length capped it),
             * walk to the next run. */
            if (RunBytesToConsume != DataRunLength * Vcb->NtfsInfo.BytesPerCluster - IntraRunOffset)
            {
                /* Caller's Length ran out mid-run.  Done. */
                ASSERT(Length == 0);
                break;
            }

            /* Subsequent runs are entered from their first byte. */
            IntraRunOffset = 0;
            CurrentOffset += DataRunLength * Vcb->NtfsInfo.BytesPerCluster;

            if (Length == 0)
                break;

            if (*DataRun == 0)
                break;

            DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);
            if (DataRunOffset != -1)
            {
                DataRunStartLCN = LastLCN + DataRunOffset;
                LastLCN = DataRunStartLCN;
            }
            else
            {
                DataRunStartLCN = -1;
            }
        } /* while Length > 0 */

        /* Flush any trailing pending segment. */
        if (NT_SUCCESS(Status) && PendingLength != 0)
        {
            if (PendingIsSparse)
            {
                RtlZeroMemory(PendingBuffer, PendingLength);
                AlreadyRead += PendingLength;
            }
            else
            {
                if (DirectDiskRead)
                {
                    Status = NtfsReadDisk(Vcb->StorageDevice,
                                          PendingDiskOffset,
                                          PendingLength,
                                          Vcb->NtfsInfo.BytesPerSector,
                                          PendingBuffer,
                                          FALSE);
                }
                else
                {
                    Status = NtfsReadDiskCached(Vcb,
                                                PendingDiskOffset,
                                                PendingLength,
                                                PendingBuffer);
                }
                if (NT_SUCCESS(Status))
                    AlreadyRead += PendingLength;
            }
        }
    }

#undef NTFS_MAX_COALESCED_READ

    return AlreadyRead;
}


/**
* @name WriteAttribute
* @implemented
*
* Writes an NTFS attribute to the disk. It presently borrows a lot of code from ReadAttribute(),
* and it still needs more documentation / cleaning up.
*
* @param Vcb
* Volume Control Block indicating which volume to write the attribute to
*
* @param Context
* Pointer to an NTFS_ATTR_CONTEXT that has information about the attribute
*
* @param Offset
* Offset, in bytes, from the beginning of the attribute indicating where to start
* writing data
*
* @param Buffer
* The data that's being written to the device
*
* @param Length
* How much data will be written, in bytes
*
* @param RealLengthWritten
* Pointer to a ULONG which will receive how much data was written, in bytes
*
* @param FileRecord
* Optional pointer to a FILE_RECORD_HEADER that contains a copy of the file record
* being written to. Can be NULL, in which case the file record will be read from disk.
* If not-null, WriteAttribute() will skip reading from disk, and FileRecord
* will be updated with the newly-written attribute before the function returns.
*
* @return
* STATUS_SUCCESS if successful, an error code otherwise. STATUS_NOT_IMPLEMENTED if
* writing to a sparse file.
*
* @remarks Note that in this context the word "attribute" isn't referring read-only, hidden,
* etc. - the file's data is actually stored in an attribute in NTFS parlance.
*
*/

NTSTATUS
WriteAttribute(PDEVICE_EXTENSION Vcb,
               PNTFS_ATTR_CONTEXT Context,
               ULONGLONG Offset,
               const PUCHAR Buffer,
               ULONG Length,
               PULONG RealLengthWritten,
               PFILE_RECORD_HEADER FileRecord)
{
    ULONGLONG LastLCN;
    PUCHAR DataRun;
    LONGLONG DataRunOffset;
    ULONGLONG DataRunLength;
    LONGLONG DataRunStartLCN;
    ULONGLONG CurrentOffset;
    ULONG WriteLength;
    NTSTATUS Status;
    PUCHAR SourceBuffer = Buffer;
    LONGLONG StartingOffset;
    BOOLEAN FileRecordAllocated = FALSE;
    LARGE_INTEGER RequiredSize;
    PNTFS_ATTR_CONTEXT RepairContext;
    ULONG RepairAttrOffset;

    //TEMPTEMP
    PUCHAR TempBuffer;


    DPRINT("WriteAttribute(%p, %p, %I64u, %p, %lu, %p, %p)\n", Vcb, Context, Offset, Buffer, Length, RealLengthWritten, FileRecord);

    *RealLengthWritten = 0;

    // is this a resident attribute?
    if (!Context->pRecord->IsNonResident)
    {
        ULONG AttributeOffset;
        PNTFS_ATTR_CONTEXT FoundContext;
        PNTFS_ATTR_RECORD Destination;

        // Ensure requested data is within the bounds of the attribute
        ASSERT(Offset + Length <= Context->pRecord->Resident.ValueLength);

        if (Offset + Length > Context->pRecord->Resident.ValueLength)
        {
            DPRINT1("DRIVER ERROR: Attribute is too small!\n");
            return STATUS_INVALID_PARAMETER;
        }

        // Do we need to read the file record?
        if (FileRecord == NULL)
        {
            FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
            if (!FileRecord)
            {
                DPRINT1("Error: Couldn't allocate file record!\n");
                return STATUS_NO_MEMORY;
            }

            FileRecordAllocated = TRUE;

            // read the file record
            ReadFileRecord(Vcb, Context->FileMFTIndex, FileRecord);
        }

        // find where to write the attribute data to
        Status = FindAttribute(Vcb, FileRecord,
                               Context->pRecord->Type,
                               (PCWSTR)((ULONG_PTR)Context->pRecord + Context->pRecord->NameOffset),
                               Context->pRecord->NameLength,
                               &FoundContext,
                               &AttributeOffset);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Couldn't find matching attribute!\n");
            if(FileRecordAllocated)
                ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
            return Status;
        }

        Destination = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttributeOffset);

        DPRINT("Offset: %I64u, AttributeOffset: %u, ValueOffset: %u\n", Offset, AttributeOffset, Context->pRecord->Resident.ValueLength);

        // Will we be writing past the end of the allocated file record?
        if (Offset + Length + AttributeOffset + Context->pRecord->Resident.ValueOffset > Vcb->NtfsInfo.BytesPerFileRecord)
        {
            DPRINT1("DRIVER ERROR: Data being written extends past end of file record!\n");
            ReleaseAttributeContext(FoundContext);
            if (FileRecordAllocated)
                ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
            return STATUS_INVALID_PARAMETER;
        }

        // copy the data being written into the file record. We cast Offset to ULONG, which is safe because it's range has been verified.
        RtlCopyMemory((PCHAR)((ULONG_PTR)Destination + Context->pRecord->Resident.ValueOffset + (ULONG)Offset), Buffer, Length);

        Status = UpdateFileRecord(Vcb, Context->FileMFTIndex, FileRecord);

        // Update the context's copy of the resident attribute
        ASSERT(Context->pRecord->Length == Destination->Length);
        RtlCopyMemory((PVOID)Context->pRecord, Destination, Context->pRecord->Length);

        ReleaseAttributeContext(FoundContext);
        if (FileRecordAllocated)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

        if (NT_SUCCESS(Status))
            *RealLengthWritten = Length;

        return Status;
    }

    // This is a non-resident attribute.

    // I. Find the corresponding start data run.

    // FIXME: Cache seems to be non-working. Disable it for now
    //if(Context->CacheRunOffset <= Offset && Offset < Context->CacheRunOffset + Context->CacheRunLength * Volume->ClusterSize)
    /*if (0)
    {
    DataRun = Context->CacheRun;
    LastLCN = Context->CacheRunLastLCN;
    DataRunStartLCN = Context->CacheRunStartLCN;
    DataRunLength = Context->CacheRunLength;
    CurrentOffset = Context->CacheRunCurrentOffset;
    }
    else*/
    {
        ULONG UsedBufferSize;
        LastLCN = 0;
        CurrentOffset = 0;

        TempBuffer = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
        if (TempBuffer == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Status = ConvertLargeMCBToDataRuns(&Context->DataRunsMCB,
                                           TempBuffer,
                                           Vcb->NtfsInfo.BytesPerFileRecord,
                                           &UsedBufferSize);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("WriteAttribute: failed to encode runlist 0x%lx\n", Status);
            goto Cleanup;
        }

        DataRun = TempBuffer;

        while (1)
        {
            DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);
            if (DataRunOffset != -1)
            {
                // Normal data run.
                // DPRINT1("Writing to normal data run, LastLCN %I64u DataRunOffset %I64d\n", LastLCN, DataRunOffset);
                DataRunStartLCN = LastLCN + DataRunOffset;
                LastLCN = DataRunStartLCN;
            }
            else
            {
                if (DataRunLength == 0)
                    goto RepairTailMapping;

                /* Sparse data run - hole-fill: allocate real clusters and
                 * splice them into the MCB, then restart the walk so the
                 * write proceeds against the now-backed run.
                 *
                 * Note: ConvertLargeMCBToDataRuns() now emits sparse runs
                 * for VBN gaps in the MCB, and ConvertDataRunsToLargeMCB()
                 * drops them on read (gap = sparse). After we splice the
                 * filled clusters into the MCB the gap closes, the next
                 * encode produces a regular run, and the walk finds it.
                 *
                 * We deliberately do NOT call AddRun here: AddRun is the
                 * "extend at the end" path and would append a duplicate run
                 * past HighestVCN. Hole-fill keeps AllocatedSize/HighestVCN
                 * the same - only the mapping-pair encoding changes.
                 */
                ULONG HoleVCN = (ULONG)(CurrentOffset / Vcb->NtfsInfo.BytesPerCluster);
                ULONG HoleLen = (ULONG)DataRunLength;
                ULONG FirstAssigned = 0;
                ULONG AssignedCount = 0;
                PNTFS_ATTR_RECORD DestAttr;
                PNTFS_ATTR_CONTEXT FoundCtx = NULL;
                ULONG FoundAttrOffset = 0;
                ULONG NewRunBufSize = 0;
                ULONG SlotMaxRuns;

                DPRINT1("WriteAttribute: hole-fill at VCN %lu len %lu\n", HoleVCN, HoleLen);

                Status = NtfsAllocateClusters(Vcb,
                                              0,            /* no LCN hint */
                                              HoleLen,
                                              &FirstAssigned,
                                              &AssignedCount);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("WriteAttribute: NtfsAllocateClusters failed 0x%lx\n", Status);
                    goto Cleanup;
                }
                if (AssignedCount != HoleLen)
                {
                    /* Phase 3.1 only handles full-hole fills. Partial allocs
                     * (disk near full) would need a more elaborate splice that
                     * leaves part of the hole sparse - defer. */
                    DPRINT1("WriteAttribute: partial hole alloc not handled (%lu of %lu)\n",
                            AssignedCount, HoleLen);
                    Status = STATUS_DISK_FULL;
                    goto Cleanup;
                }

                /* Splice the new run into the MCB at HoleVCN. */
                _SEH2_TRY {
                    if (!FsRtlAddLargeMcbEntry(&Context->DataRunsMCB,
                                               (LONGLONG)HoleVCN,
                                               (LONGLONG)FirstAssigned,
                                               (LONGLONG)HoleLen))
                    {
                        ExRaiseStatus(STATUS_UNSUCCESSFUL);
                    }
                } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                    Status = _SEH2_GetExceptionCode();
                    DPRINT1("WriteAttribute: FsRtlAddLargeMcbEntry raised 0x%lx\n", Status);
                    goto Cleanup;
                } _SEH2_END;

                /* Re-encode the (now-spliced) MCB. The result goes into the
                 * same TempBuffer we'll continue walking. */
                Status = ConvertLargeMCBToDataRuns(&Context->DataRunsMCB,
                                                   TempBuffer,
                                                   Vcb->NtfsInfo.BytesPerFileRecord,
                                                   &NewRunBufSize);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("WriteAttribute: re-encode failed 0x%lx\n", Status);
                    goto Cleanup;
                }

                /* Locate the on-disk slot we need to update. The caller passed
                 * us a FileRecord which already contains the attribute (we just
                 * walked its mapping pairs above). Find it again to get a
                 * FoundCtx with a stable AttrOffset. */
                Status = FindAttribute(Vcb, FileRecord,
                                       Context->pRecord->Type,
                                       (PCWSTR)((ULONG_PTR)Context->pRecord + Context->pRecord->NameOffset),
                                       Context->pRecord->NameLength,
                                       &FoundCtx,
                                       &FoundAttrOffset);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("WriteAttribute: FindAttribute after splice failed 0x%lx\n", Status);
                    goto Cleanup;
                }

                DestAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FoundAttrOffset);
                SlotMaxRuns = DestAttr->Length - DestAttr->NonResident.MappingPairsOffset;

                if (NewRunBufSize > SlotMaxRuns)
                {
                    /* Phase 3.1 punt: if the new mapping pairs no longer fit
                     * in the existing slot, we'd need to grow the slot (and
                     * possibly migrate to $ATTRIBUTE_LIST). Defer for now -
                     * fill-and-fit covers the common case. */
                    DPRINT1("WriteAttribute: hole-fill mapping pairs grew (%lu > %lu); slot grow not yet supported\n",
                            NewRunBufSize, SlotMaxRuns);
                    ReleaseAttributeContext(FoundCtx);
                    Status = STATUS_NOT_SUPPORTED;
                    goto Cleanup;
                }

                /* Splat the new mapping pairs into both the on-disk-bound
                 * slot in FileRecord and the in-memory pRecord copy that
                 * Context holds.  Pad with zeros to the existing slot end so
                 * stale bytes from the prior encoding don't trip the decoder. */
                RtlZeroMemory((PVOID)((ULONG_PTR)DestAttr + DestAttr->NonResident.MappingPairsOffset),
                              SlotMaxRuns);
                RtlCopyMemory((PVOID)((ULONG_PTR)DestAttr + DestAttr->NonResident.MappingPairsOffset),
                              TempBuffer, NewRunBufSize);
                RtlZeroMemory((PVOID)((ULONG_PTR)Context->pRecord + Context->pRecord->NonResident.MappingPairsOffset),
                              Context->pRecord->Length - Context->pRecord->NonResident.MappingPairsOffset);
                RtlCopyMemory((PVOID)((ULONG_PTR)Context->pRecord + Context->pRecord->NonResident.MappingPairsOffset),
                              TempBuffer, NewRunBufSize);

                ReleaseAttributeContext(FoundCtx);

                Status = UpdateFileRecord(Vcb, Context->FileMFTIndex, FileRecord);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("WriteAttribute: UpdateFileRecord after hole-fill failed 0x%lx\n", Status);
                    goto Cleanup;
                }

                /* The hole is now backed. Restart the walk over the freshly
                 * re-encoded TempBuffer. */
                DataRun = TempBuffer;
                LastLCN = 0;
                CurrentOffset = 0;
                continue;
            }

            // Have we reached the data run we're trying to write to?
            if (Offset >= CurrentOffset &&
                Offset < CurrentOffset + (DataRunLength * Vcb->NtfsInfo.BytesPerCluster))
            {
                break;
            }

RepairTailMapping:
            if (*DataRun == 0 || DataRunLength == 0)
            {
                /* The runlist ended before the requested write offset. Repair
                 * the tail mapping and retry instead of failing the write. */
                RequiredSize.QuadPart = max((ULONGLONG)AttributeDataLength(Context->pRecord),
                                            Offset + Length);
                DPRINT1("WriteAttribute: extending missing tail mapping for offset %I64u len %lu (size %I64u)\n",
                        Offset, Length, RequiredSize.QuadPart);
                Status = FindAttribute(Vcb, FileRecord,
                                       Context->pRecord->Type,
                                       (PCWSTR)((ULONG_PTR)Context->pRecord + Context->pRecord->NameOffset),
                                       Context->pRecord->NameLength,
                                       &RepairContext,
                                       &RepairAttrOffset);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("WriteAttribute: couldn't re-find attribute for tail repair 0x%lx\n", Status);
                    goto Cleanup;
                }

                Status = SetNonResidentAttributeDataLength(Vcb,
                                                           Context,
                                                           RepairAttrOffset,
                                                           FileRecord,
                                                           &RequiredSize);
                ReleaseAttributeContext(RepairContext);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("WriteAttribute: tail-mapping repair failed 0x%lx\n", Status);
                    goto Cleanup;
                }

                Status = ConvertLargeMCBToDataRuns(&Context->DataRunsMCB,
                                                   TempBuffer,
                                                   Vcb->NtfsInfo.BytesPerFileRecord,
                                                   &UsedBufferSize);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("WriteAttribute: failed to re-encode repaired runlist 0x%lx\n", Status);
                    goto Cleanup;
                }

                DataRun = TempBuffer;
                LastLCN = 0;
                CurrentOffset = 0;
                continue;
            }

            CurrentOffset += DataRunLength * Vcb->NtfsInfo.BytesPerCluster;
        }
    }

    // II. Go through the run list and write the data

    /* REVIEWME -- As adapted from NtfsReadAttribute():
    We seem to be making a special case for the first applicable data run, but I'm not sure why.
    Does it have something to do with (not) caching? Is this strategy equally applicable to writing? */

    WriteLength = (ULONG)min(DataRunLength * Vcb->NtfsInfo.BytesPerCluster - (Offset - CurrentOffset), Length);

    StartingOffset = DataRunStartLCN * Vcb->NtfsInfo.BytesPerCluster + Offset - CurrentOffset;

    // Write the data to the disk
    Status = NtfsWriteDiskCached(Vcb,
                                 StartingOffset,
                                 WriteLength,
                                 (PVOID)SourceBuffer);

    // Did the write fail?
    if (!NT_SUCCESS(Status))
    {
        Context->CacheRun = DataRun;
        Context->CacheRunOffset = Offset;
        Context->CacheRunStartLCN = DataRunStartLCN;
        Context->CacheRunLength = DataRunLength;
        Context->CacheRunLastLCN = LastLCN;
        Context->CacheRunCurrentOffset = CurrentOffset;

        goto Cleanup;
    }

    Length -= WriteLength;
    SourceBuffer += WriteLength;
    *RealLengthWritten += WriteLength;

    // Did we write to the end of the data run?
    if (WriteLength == DataRunLength * Vcb->NtfsInfo.BytesPerCluster - (Offset - CurrentOffset))
    {
        // Advance to the next data run
        CurrentOffset += DataRunLength * Vcb->NtfsInfo.BytesPerCluster;
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);

        if (DataRunOffset != (ULONGLONG)-1)
        {
            DataRunStartLCN = LastLCN + DataRunOffset;
            LastLCN = DataRunStartLCN;
        }
        else
            DataRunStartLCN = -1;
    }

    // Do we have more data to write?
    while (Length > 0)
    {
        // Make sure we don't write past the end of the current data run
        WriteLength = (ULONG)min(DataRunLength * Vcb->NtfsInfo.BytesPerCluster, Length);

        // Are we dealing with a sparse data run?
        if (DataRunStartLCN == -1)
        {
            /* See the matching comment above - same MCB-roundtrip blocker.
             * Crossing into a sparse hole mid-write needs hole-fill support. */
            DPRINT1("WriteAttribute: write spans sparse hole, hole-fill not supported yet\n");
            Status = STATUS_NOT_SUPPORTED;
            goto Cleanup;
        }
        else
        {
            // write the data to the disk
            Status = NtfsWriteDiskCached(Vcb,
                                         DataRunStartLCN * Vcb->NtfsInfo.BytesPerCluster,
                                         WriteLength,
                                         (PVOID)SourceBuffer);
            if (!NT_SUCCESS(Status))
                break;
        }

        Length -= WriteLength;
        SourceBuffer += WriteLength;
        *RealLengthWritten += WriteLength;

        // We finished this request, but there's still data in this data run.
        if (Length == 0 && WriteLength != DataRunLength * Vcb->NtfsInfo.BytesPerCluster)
            break;

        // Go to next run in the list.

        if (*DataRun == 0)
        {
            // that was the last run
            if (Length > 0)
            {
                // Failed sanity check.
                DPRINT1("Encountered EOF before expected!\n");
                Status = STATUS_END_OF_FILE;
                goto Cleanup;
            }

            break;
        }

        // Advance to the next data run
        CurrentOffset += DataRunLength * Vcb->NtfsInfo.BytesPerCluster;
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);
        if (DataRunOffset != -1)
        {
            // Normal data run.
            DataRunStartLCN = LastLCN + DataRunOffset;
            LastLCN = DataRunStartLCN;
        }
        else
        {
            // Sparse data run.
            DataRunStartLCN = -1;
        }
    } // end while (Length > 0) [more data to write]

    Context->CacheRun = DataRun;
    Context->CacheRunOffset = Offset + *RealLengthWritten;
    Context->CacheRunStartLCN = DataRunStartLCN;
    Context->CacheRunLength = DataRunLength;
    Context->CacheRunLastLCN = LastLCN;
    Context->CacheRunCurrentOffset = CurrentOffset;

Cleanup:
    // TEMPTEMP
    if (Context->pRecord->IsNonResident)
        ExFreePoolWithTag(TempBuffer, TAG_NTFS);

    return Status;
}

NTSTATUS
ReadFileRecord(PDEVICE_EXTENSION Vcb,
               ULONGLONG index,
               PFILE_RECORD_HEADER file)
{
    ULONGLONG BytesRead;
    BOOLEAN MftLockHeld = FALSE;

    DPRINT("ReadFileRecord(%p, %I64x, %p)\n", Vcb, index, file);

    NTFS_CHECK_POOL(file, "ReadFileRecord:pre");

    NTFS_TRACE_IF(index == 144, "DRVIDX: ReadFileRecord begin index=%I64u offset=%I64u\n",
                index,
                index * Vcb->NtfsInfo.BytesPerFileRecord);
    /* Serialize against MFT growth: IncreaseMftSize rewrites and reallocates
     * the shared Vcb->MFTContext->pRecord runlist in place (AddRun).  This
     * ReadAttribute walks that same buffer, so without serialization a
     * concurrent grow can free/rewrite the runlist mid-walk and truncate the
     * read, yielding STATUS_PARTIAL_COPY.  MftContextResource is a dedicated
     * LEAF lock (below DirResource/IndexResource/BitmapResource): ReadFileRecord
     * is called both standalone and while holding IndexResource exclusive, so
     * reusing a higher lock here would invert the Dir->Index hierarchy.  Taken
     * SHARED here (concurrent reads allowed) and EXCLUSIVE around the grow in
     * IncreaseMftSize.  Skipped during mount bootstrap, before the resource
     * exists (MftReadLockReady == FALSE; that path is single-threaded). */
    if (Vcb->MftReadLockReady)
        MftLockHeld = ExAcquireResourceSharedLite(&Vcb->MftContextResource, TRUE);
    BytesRead = ReadAttribute(Vcb, Vcb->MFTContext, index * Vcb->NtfsInfo.BytesPerFileRecord, (PCHAR)file, Vcb->NtfsInfo.BytesPerFileRecord);
    if (MftLockHeld)
        ExReleaseResourceLite(&Vcb->MftContextResource);
    NTFS_TRACE_IF(index == 144, "DRVIDX: ReadFileRecord readattr bytes=%I64u expected=%lu\n",
                BytesRead,
                Vcb->NtfsInfo.BytesPerFileRecord);
    if (BytesRead != Vcb->NtfsInfo.BytesPerFileRecord)
    {
        DPRINT1("ReadFileRecord failed: %I64u read, %lu expected\n", BytesRead, Vcb->NtfsInfo.BytesPerFileRecord);
        return STATUS_PARTIAL_COPY;
    }

    NTFS_CHECK_POOL(file, "ReadFileRecord:post");

    /* Apply update sequence array fixups. */
    DPRINT("Sequence number: %u\n", file->SequenceNumber);
    NTFS_TRACE_IF(index == 144, "DRVIDX: ReadFileRecord fixup begin type=0x%lx\n", file->Ntfs.Type);
    {
        NTSTATUS Status = FixupUpdateSequenceArray(Vcb, &file->Ntfs);
        NTFS_TRACE_IF(index == 144, "DRVIDX: ReadFileRecord fixup returned 0x%lx\n", Status);
        return Status;
    }
}


/**
* Searches a file's parent directory (given the parent's index in the mft)
* for the given file. Upon finding an index entry for that file, updates
* Data Size and Allocated Size values in the $FILE_NAME attribute of that entry.
*
* (Most of this code was copied from NtfsFindMftRecord)
*/
NTSTATUS
UpdateFileNameRecord(PDEVICE_EXTENSION Vcb,
                     ULONGLONG ParentMFTIndex,
                     PUNICODE_STRING FileName,
                     BOOLEAN DirSearch,
                     ULONGLONG NewDataSize,
                     ULONGLONG NewAllocationSize,
                     BOOLEAN CaseSensitive)
{
    PFILE_RECORD_HEADER MftRecord;
    PNTFS_ATTR_CONTEXT IndexRootCtx;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PCHAR IndexRecord;
    PINDEX_ENTRY_ATTRIBUTE IndexEntry, IndexEntryEnd;
    NTSTATUS Status;
    ULONG CurrentEntry = 0;
    BOOLEAN IndexLockHeld;

    DPRINT("UpdateFileNameRecord(%p, %I64d, %wZ, %s, %I64u, %I64u, %s)\n",
           Vcb,
           ParentMFTIndex,
           FileName,
           DirSearch ? "TRUE" : "FALSE",
           NewDataSize,
           NewAllocationSize,
           CaseSensitive ? "TRUE" : "FALSE");

    /* Take IndexResource exclusive across the entire R/M/W of the parent
     * directory's $INDEX_ROOT and $INDEX_ALLOCATION so concurrent readers
     * (NtfsFindMftRecord -> BrowseIndexEntries) can't observe a
     * mid-update INDX block. See Kreijstal/reactos#14. */
    IndexLockHeld = ExAcquireResourceExclusiveLite(&Vcb->IndexResource, TRUE);

    MftRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (MftRecord == NULL)
    {
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, ParentMFTIndex, MftRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return Status;
    }

    ASSERT(MftRecord->Ntfs.Type == NRH_FILE_TYPE);
    Status = FindAttribute(Vcb, MftRecord, AttributeIndexRoot, L"$I30", 4, &IndexRootCtx, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return Status;
    }

    IndexRecord = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerIndexRecord, TAG_NTFS);
    if (IndexRecord == NULL)
    {
        ReleaseAttributeContext(IndexRootCtx);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadAttribute(Vcb, IndexRootCtx, 0, IndexRecord, AttributeDataLength(IndexRootCtx->pRecord));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to read Index Root!\n");
        ExFreePoolWithTag(IndexRecord, TAG_NTFS);
        ReleaseAttributeContext(IndexRootCtx);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return Status;
    }

    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)IndexRecord;
    IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
    // Index root is always resident.
    IndexEntryEnd = (PINDEX_ENTRY_ATTRIBUTE)(IndexRecord + IndexRoot->Header.TotalSizeOfEntries);

    DPRINT("IndexRecordSize: %x IndexBlockSize: %x\n", Vcb->NtfsInfo.BytesPerIndexRecord, IndexRoot->SizeOfEntry);

    Status = UpdateIndexEntryFileNameSize(Vcb,
                                          MftRecord,
                                          IndexRecord,
                                          IndexRoot->SizeOfEntry,
                                          IndexEntry,
                                          IndexEntryEnd,
                                          FileName,
                                          &CurrentEntry,
                                          &CurrentEntry,
                                          DirSearch,
                                          NewDataSize,
                                          NewAllocationSize,
                                          CaseSensitive);

    if (Status == STATUS_PENDING)
    {
        // we need to write the index root attribute back to disk
        ULONG LengthWritten;
        Status = WriteAttribute(Vcb, IndexRootCtx, 0, (PUCHAR)IndexRecord, AttributeDataLength(IndexRootCtx->pRecord), &LengthWritten, MftRecord);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Couldn't update Index Root!\n");
        }

    }

    ReleaseAttributeContext(IndexRootCtx);
    ExFreePoolWithTag(IndexRecord, TAG_NTFS);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
    if (IndexLockHeld)
        ExReleaseResourceLite(&Vcb->IndexResource);

    return Status;
}

/**
* Recursively searches directory index and applies the size update to the $FILE_NAME attribute of the
* proper index entry.
* (Heavily based on BrowseIndexEntries)
*/
NTSTATUS
UpdateIndexEntryFileNameSize(PDEVICE_EXTENSION Vcb,
                             PFILE_RECORD_HEADER MftRecord,
                             PCHAR IndexRecord,
                             ULONG IndexBlockSize,
                             PINDEX_ENTRY_ATTRIBUTE FirstEntry,
                             PINDEX_ENTRY_ATTRIBUTE LastEntry,
                             PUNICODE_STRING FileName,
                             PULONG StartEntry,
                             PULONG CurrentEntry,
                             BOOLEAN DirSearch,
                             ULONGLONG NewDataSize,
                             ULONGLONG NewAllocatedSize,
                             BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    ULONG RecordOffset;
    PINDEX_ENTRY_ATTRIBUTE IndexEntry;
    PNTFS_ATTR_CONTEXT IndexAllocationCtx;
    ULONGLONG IndexAllocationSize;
    PINDEX_BUFFER IndexBuffer;

    DPRINT("UpdateIndexEntrySize(%p, %p, %p, %lu, %p, %p, %wZ, %lu, %lu, %s, %I64u, %I64u, %s)\n",
           Vcb,
           MftRecord,
           IndexRecord,
           IndexBlockSize,
           FirstEntry,
           LastEntry,
           FileName,
           *StartEntry,
           *CurrentEntry,
           DirSearch ? "TRUE" : "FALSE",
           NewDataSize,
           NewAllocatedSize,
           CaseSensitive ? "TRUE" : "FALSE");

    // find the index entry responsible for the file we're trying to update
    IndexEntry = FirstEntry;
    while (IndexEntry < LastEntry &&
           !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
    {
        if ((IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK) > NTFS_FILE_FIRST_USER_FILE &&
            *CurrentEntry >= *StartEntry &&
            IndexEntry->FileName.NameType != NTFS_FILE_NAME_DOS &&
            CompareFileName(FileName, IndexEntry, DirSearch, CaseSensitive))
        {
            *StartEntry = *CurrentEntry;
            IndexEntry->FileName.DataSize = NewDataSize;
            IndexEntry->FileName.AllocatedSize = NewAllocatedSize;
            // indicate that the caller will still need to write the structure to the disk
            return STATUS_PENDING;
        }

        (*CurrentEntry) += 1;
        ASSERT(IndexEntry->Length >= sizeof(INDEX_ENTRY_ATTRIBUTE));
        IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)IndexEntry + IndexEntry->Length);
    }

    /* If we're already browsing a subnode */
    if (IndexRecord == NULL)
    {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    /* If there's no subnode */
    if (!(IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE))
    {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    Status = FindAttribute(Vcb, MftRecord, AttributeIndexAllocation, L"$I30", 4, &IndexAllocationCtx, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("Corrupted filesystem!\n");
        return Status;
    }

    IndexAllocationSize = AttributeDataLength(IndexAllocationCtx->pRecord);
    Status = STATUS_OBJECT_PATH_NOT_FOUND;
    for (RecordOffset = 0; RecordOffset < IndexAllocationSize; RecordOffset += IndexBlockSize)
    {
        ReadAttribute(Vcb, IndexAllocationCtx, RecordOffset, IndexRecord, IndexBlockSize);
        Status = FixupUpdateSequenceArray(Vcb, &((PFILE_RECORD_HEADER)IndexRecord)->Ntfs);
        if (!NT_SUCCESS(Status))
        {
            break;
        }

        IndexBuffer = (PINDEX_BUFFER)IndexRecord;
        ASSERT(IndexBuffer->Ntfs.Type == NRH_INDX_TYPE);
        ASSERT(IndexBuffer->Header.AllocatedSize + FIELD_OFFSET(INDEX_BUFFER, Header) == IndexBlockSize);
        FirstEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&IndexBuffer->Header + IndexBuffer->Header.FirstEntryOffset);
        LastEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&IndexBuffer->Header + IndexBuffer->Header.TotalSizeOfEntries);
        ASSERT(LastEntry <= (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)IndexBuffer + IndexBlockSize));

        Status = UpdateIndexEntryFileNameSize(NULL,
                                              NULL,
                                              NULL,
                                              0,
                                              FirstEntry,
                                              LastEntry,
                                              FileName,
                                              StartEntry,
                                              CurrentEntry,
                                              DirSearch,
                                              NewDataSize,
                                              NewAllocatedSize,
                                              CaseSensitive);
        if (Status == STATUS_PENDING)
        {
            // write the index record back to disk
            ULONG Written;

            // first we need to update the fixup values for the index block
            Status = AddFixupArray(Vcb, &((PFILE_RECORD_HEADER)IndexRecord)->Ntfs);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Error: Failed to update fixup sequence array!\n");
                break;
            }

            Status = WriteAttribute(Vcb, IndexAllocationCtx, RecordOffset, (const PUCHAR)IndexRecord, IndexBlockSize, &Written, MftRecord);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR Performing write!\n");
                break;
            }

            Status = STATUS_SUCCESS;
            break;
        }
        if (NT_SUCCESS(Status))
        {
            break;
        }
    }

    ReleaseAttributeContext(IndexAllocationCtx);
    return Status;
}

/**
* @name UpdateFileRecord
* @implemented
*
* Writes a file record to the master file table, at a given index.
*
* @param Vcb
* Pointer to the DEVICE_EXTENSION of the target drive being written to.
*
* @param MftIndex
* Target index in the master file table to store the file record.
*
* @param FileRecord
* Pointer to the complete file record which will be written to the master file table.
*
* @return
* STATUS_SUCCESSFUL on success. An error passed from WriteAttribute() otherwise.
*
*/
NTSTATUS
UpdateFileRecord(PDEVICE_EXTENSION Vcb,
                 ULONGLONG MftIndex,
                 PFILE_RECORD_HEADER FileRecord)
{
    ULONG BytesWritten;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("UpdateFileRecord(%p, 0x%I64x, %p)\n", Vcb, MftIndex, FileRecord);

    NTFS_CHECK_POOL(FileRecord, "UpdateFileRecord:pre");

    /* $LogFile write-ahead-logging hook (Kreijstal/reactos#34 slice 2).
     *
     * When Vcb->LoggingEnabled is FALSE (the default for every mounted
     * volume) this is a pass-through returning STATUS_SUCCESS that touches
     * nothing - the path below is then byte-for-byte unchanged.  When logging
     * is on, it emits a redo/undo record for this FILE_RECORD change, flushes
     * the log RCRD page + dirty restart to $LogFile, runs the $Volume DIRTY
     * handshake, and returns the assigned LSN so we can stamp it into the
     * record's Lsn field BEFORE the metadata page is written (the WAL
     * invariant).  The redo payload is the post-change record image; the
     * pre-image (undo) is left for slice 3 (replay), which is the consumer of
     * these records.  Emission failure aborts the metadata write so we never
     * write a metadata page ahead of its log record. */
    if (Vcb->LoggingEnabled)
    {
        ULONGLONG PageLsn = 0;
        USHORT RedoLen = (USHORT)min(Vcb->NtfsInfo.BytesPerFileRecord,
                                     (ULONG)0xFFFF);

        Status = NtfsLfsLogMetadataPage(Vcb,
                                        NTFS_LFS_OP_UPDATE_RESIDENT_VALUE,
                                        NTFS_LFS_OP_UPDATE_RESIDENT_VALUE,
                                        0,
                                        MftIndex,
                                        FileRecord,
                                        RedoLen,
                                        NULL,
                                        0,
                                        &PageLsn);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("UpdateFileRecord: WAL emission failed 0x%08lx; "
                    "aborting metadata write\n", Status);
            return Status;
        }

        /* Stamp the assigned LSN into the record so a future replay can tell
         * whether this on-disk image is at or past the logged change. */
        FileRecord->Ntfs.Lsn = PageLsn;
    }

    // Add the fixup array to prepare the data for writing to disk
    AddFixupArray(Vcb, &FileRecord->Ntfs);

    NTFS_CHECK_POOL(FileRecord, "UpdateFileRecord:post-fixup");

    // write the file record to the master file table
    Status = WriteAttribute(Vcb,
                            Vcb->MFTContext,
                            MftIndex * Vcb->NtfsInfo.BytesPerFileRecord,
                            (const PUCHAR)FileRecord,
                            Vcb->NtfsInfo.BytesPerFileRecord,
                            &BytesWritten,
                            Vcb->MasterFileTable);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("UpdateFileRecord failed: %lu written, %lu expected\n", BytesWritten, Vcb->NtfsInfo.BytesPerFileRecord);
    }

    // remove the fixup array (so the file record pointer can still be used)
    FixupUpdateSequenceArray(Vcb, &FileRecord->Ntfs);

    return Status;
}


NTSTATUS
FixupUpdateSequenceArray(PDEVICE_EXTENSION Vcb,
                         PNTFS_RECORD_HEADER Record)
{
    USHORT *USA;
    USHORT USANumber;
    USHORT USACount;
    USHORT *Block;
    ULONG UsaLength;

    if (Record->Type != NRH_FILE_TYPE &&
        Record->Type != NRH_INDX_TYPE &&
        Record->Type != NRH_RSTR_TYPE &&
        Record->Type != NRH_RCRD_TYPE &&
        Record->Type != NRH_CHKD_TYPE)
    {
        DPRINT("Invalid NTFS record type 0x%08lx\n", Record->Type);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    if (Record->UsaCount < 2 ||
        Record->UsaOffset < sizeof(NTFS_RECORD_HEADER))
    {
        DPRINT("Invalid USA header: offset=%u count=%u type=0x%08lx\n",
               Record->UsaOffset,
               Record->UsaCount,
               Record->Type);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    UsaLength = Record->UsaCount * sizeof(USHORT);
    if (Record->UsaOffset > Vcb->NtfsInfo.BytesPerSector ||
        UsaLength > Vcb->NtfsInfo.BytesPerSector - Record->UsaOffset)
    {
        DPRINT("Invalid USA bounds: offset=%u count=%u type=0x%08lx\n",
               Record->UsaOffset,
               Record->UsaCount,
               Record->Type);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    USA = (USHORT*)((PCHAR)Record + Record->UsaOffset);
    USANumber = *(USA++);
    USACount = Record->UsaCount - 1; /* Exclude the USA Number. */
    Block = (USHORT*)((PCHAR)Record + Vcb->NtfsInfo.BytesPerSector - 2);

    DPRINT("FixupUpdateSequenceArray(%p, %p)\nUSANumber: %u\tUSACount: %u\n", Vcb, Record, USANumber, USACount);

    while (USACount)
    {
        if (*Block != USANumber)
        {
            DPRINT1("USA mismatch in record type 0x%04lx: %u read, %u expected\n",
                    Record->Type, *Block, USANumber);
            return STATUS_FILE_CORRUPT_ERROR;
        }
        *Block = *(USA++);
        Block = (USHORT*)((PCHAR)Block + Vcb->NtfsInfo.BytesPerSector);
        USACount--;
    }

    return STATUS_SUCCESS;
}

/*
 * Update one bit in $MFT::$BITMAP.  Used to roll back failed creates and to
 * make deleted file records reusable.
 */
NTSTATUS
NtfsSetMftBitmapInUse(PDEVICE_EXTENSION DeviceExt,
                      ULONGLONG MftIndex,
                      BOOLEAN InUse,
                      BOOLEAN CanWait)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT BitmapContext;
    ULONGLONG BitmapDataSize;
    ULONGLONG AttrBytesRead;
    PUCHAR BitmapData;
    PUCHAR BitmapBuffer = NULL;
    ULONG LengthWritten;
    LARGE_INTEGER BitmapBits;
    RTL_BITMAP Bitmap;

    if (MftIndex > MAXULONG)
        return STATUS_INVALID_PARAMETER;

    if (!ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, CanWait))
        return STATUS_CANT_WAIT;

    Status = FindAttribute(DeviceExt,
                           DeviceExt->MasterFileTable,
                           AttributeBitmap,
                           L"",
                           0,
                           &BitmapContext,
                           NULL);
    if (!NT_SUCCESS(Status))
        goto CleanupLock;

    BitmapDataSize = AttributeDataLength(BitmapContext->pRecord);
    if (MftIndex >= BitmapDataSize * 8)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto CleanupContext;
    }

    if (DeviceExt->MftBitmapData && DeviceExt->MftBitmapSize == BitmapDataSize)
    {
        BitmapData = (PUCHAR)DeviceExt->MftBitmapData;
    }
    else
    {
        ULONG AllocSize = ALIGN_UP_BY(BitmapDataSize, sizeof(ULONG));

        BitmapBuffer = ExAllocatePoolWithTag(NonPagedPool,
                                             AllocSize + sizeof(ULONG),
                                             TAG_NTFS);
        if (!BitmapBuffer)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto CleanupContext;
        }

        RtlZeroMemory(BitmapBuffer, AllocSize + sizeof(ULONG));
        BitmapData = (PUCHAR)ALIGN_UP_BY((ULONG_PTR)BitmapBuffer, sizeof(ULONG));

        AttrBytesRead = ReadAttribute(DeviceExt,
                                      BitmapContext,
                                      0,
                                      (PCHAR)BitmapData,
                                      BitmapDataSize);
        if (AttrBytesRead != BitmapDataSize)
        {
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
            goto CleanupContext;
        }

        if (DeviceExt->MftBitmapBuffer)
            ExFreePoolWithTag(DeviceExt->MftBitmapBuffer, TAG_NTFS);
        DeviceExt->MftBitmapBuffer = BitmapBuffer;
        DeviceExt->MftBitmapData = (PULONG)BitmapData;
        DeviceExt->MftBitmapSize = BitmapDataSize;
        BitmapBuffer = NULL;
    }

    BitmapBits.QuadPart = AttributeDataLength(DeviceExt->MFTContext->pRecord) /
                          DeviceExt->NtfsInfo.BytesPerFileRecord;
    if (BitmapBits.HighPart != 0 || BitmapBits.LowPart == MAXULONG)
        BitmapBits.QuadPart = MAXULONG - 1;

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, BitmapBits.LowPart);
    if (InUse)
        RtlSetBits(&Bitmap, (ULONG)MftIndex, 1);
    else
        RtlClearBits(&Bitmap, (ULONG)MftIndex, 1);

    Status = WriteAttribute(DeviceExt,
                            BitmapContext,
                            0,
                            BitmapData,
                            BitmapDataSize,
                            &LengthWritten,
                            DeviceExt->MasterFileTable);
    if (NT_SUCCESS(Status) && LengthWritten != BitmapDataSize)
        Status = STATUS_UNSUCCESSFUL;

CleanupContext:
    if (BitmapBuffer)
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
    ReleaseAttributeContext(BitmapContext);

CleanupLock:
    ExReleaseResourceLite(&DeviceExt->DirResource);
    return Status;
}

/**
* @name AddNewMftEntry
* @implemented
*
* Adds a file record to the master file table of a given device.
*
* @param FileRecord
* Pointer to a complete file record which will be saved to disk.
*
* @param DeviceExt
* Pointer to the DEVICE_EXTENSION of the target drive.
*
* @param DestinationIndex
* Pointer to a ULONGLONG which will receive the MFT index where the file record was stored.
*
* @param CanWait
* Boolean indicating if the function is allowed to wait for exclusive access to the master file table.
* This will only be relevant if the MFT doesn't have any free file records and needs to be enlarged.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_OBJECT_NAME_NOT_FOUND if we can't find the MFT's $Bitmap or if we weren't able
* to read the attribute.
* STATUS_INSUFFICIENT_RESOURCES if we can't allocate enough memory for a copy of $Bitmap.
* STATUS_CANT_WAIT if CanWait was FALSE and the function could not get immediate, exclusive access to the MFT.
*/
NTSTATUS
AddNewMftEntry(PFILE_RECORD_HEADER FileRecord,
               PDEVICE_EXTENSION DeviceExt,
               PULONGLONG DestinationIndex,
               BOOLEAN CanWait)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONGLONG MftIndex;
    RTL_BITMAP Bitmap;
    ULONGLONG BitmapDataSize;
    ULONGLONG AttrBytesRead;
    PUCHAR BitmapData;
    PUCHAR BitmapBuffer;
    ULONG LengthWritten;
    PNTFS_ATTR_CONTEXT BitmapContext;
    LARGE_INTEGER BitmapBits;
    UCHAR SystemReservedBits;
    BOOLEAN MftLockHeld = FALSE;

    DPRINT("AddNewMftEntry(%p, %p, %p, %s)\n", FileRecord, DeviceExt, DestinationIndex, CanWait ? "TRUE" : "FALSE");

    if (!ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, CanWait))
        return STATUS_CANT_WAIT;
    MftLockHeld = TRUE;

    // Find the $Bitmap attribute
    Status = FindAttribute(DeviceExt, DeviceExt->MasterFileTable, AttributeBitmap, L"", 0, &BitmapContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find $Bitmap attribute of master file table!\n");
        goto CleanupLock;
    }

    BitmapDataSize = AttributeDataLength(BitmapContext->pRecord);

    // Use cached bitmap if available and size matches; otherwise read from disk
    if (DeviceExt->MftBitmapData && DeviceExt->MftBitmapSize == BitmapDataSize)
    {
        BitmapData = (PUCHAR)DeviceExt->MftBitmapData;
        BitmapBuffer = NULL;
    }
    else
    {
        ULONG AllocSize = ALIGN_UP_BY(BitmapDataSize, sizeof(ULONG));
        BitmapBuffer = ExAllocatePoolWithTag(NonPagedPool, AllocSize + sizeof(ULONG), TAG_NTFS);
        if (!BitmapBuffer)
        {
            ReleaseAttributeContext(BitmapContext);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto CleanupLock;
        }
        RtlZeroMemory(BitmapBuffer, AllocSize + sizeof(ULONG));
        BitmapData = (PUCHAR)ALIGN_UP_BY((ULONG_PTR)BitmapBuffer, sizeof(ULONG));

        AttrBytesRead = ReadAttribute(DeviceExt, BitmapContext, 0, (PCHAR)BitmapData, BitmapDataSize);
        if (AttrBytesRead != BitmapDataSize)
        {
            DPRINT1("ERROR: Unable to read $Bitmap attribute of master file table!\n");
            ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
            ReleaseAttributeContext(BitmapContext);
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
            goto CleanupLock;
        }

        if (DeviceExt->MftBitmapBuffer)
            ExFreePoolWithTag(DeviceExt->MftBitmapBuffer, TAG_NTFS);
        DeviceExt->MftBitmapBuffer = BitmapBuffer;
        DeviceExt->MftBitmapData = (PULONG)BitmapData;
        DeviceExt->MftBitmapSize = BitmapDataSize;
        BitmapBuffer = NULL;
    }

    // Backup and mask system reserved bits
    SystemReservedBits = BitmapData[2];
    BitmapData[2] = 0xff;

    // Calculate bit count.  The RTL bitmap API is 32-bit (ULONG bit
    // count, ULONG search result), so cap at MAXULONG - 1 when the MFT
    // on-disk bitmap exceeds 4 billion entries.  This only limits NEW
    // MFT allocation to the first ~4 billion records - a filesystem
    // with that many files is not a realistic scenario, and even if
    // one exists we degrade gracefully to read-only semantics for the
    // out-of-range portion instead of disabling write support entirely.
    BitmapBits.QuadPart = AttributeDataLength(DeviceExt->MFTContext->pRecord) /
                          DeviceExt->NtfsInfo.BytesPerFileRecord;
    if (BitmapBits.HighPart != 0 || BitmapBits.LowPart == MAXULONG)
    {
        DPRINT1("NTFS: MFT bitmap > 2^32 entries; clamping search range.\n");
        BitmapBits.QuadPart = MAXULONG - 1;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, BitmapBits.LowPart);

    // Search from hint for faster allocation
    MftIndex = RtlFindClearBitsAndSet(&Bitmap, 1,
                                       DeviceExt->MftNextFreeHint ? DeviceExt->MftNextFreeHint : 24);
    if ((LONG)MftIndex == -1)
    {
        DPRINT("Couldn't find free space in MFT, increasing size.\n");
        BitmapData[2] = SystemReservedBits;
        ReleaseAttributeContext(BitmapContext);

        // Invalidate cache since MFT size is changing
        if (DeviceExt->MftBitmapBuffer)
        {
            ExFreePoolWithTag(DeviceExt->MftBitmapBuffer, TAG_NTFS);
            DeviceExt->MftBitmapBuffer = NULL;
            DeviceExt->MftBitmapData = NULL;
            DeviceExt->MftBitmapSize = 0;
        }

        ExReleaseResourceLite(&DeviceExt->DirResource);
        MftLockHeld = FALSE;

        Status = IncreaseMftSize(DeviceExt, CanWait);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Couldn't increase MFT size!\n");
            return Status;
        }

        return AddNewMftEntry(FileRecord, DeviceExt, DestinationIndex, CanWait);
    }

    // Update hint for next allocation
    DeviceExt->MftNextFreeHint = MftIndex + 1;

    FileRecord->MFTRecordNumber = MftIndex;
    BitmapData[2] = SystemReservedBits;

    // Write the file record to disk
    Status = UpdateFileRecord(DeviceExt, MftIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Unable to write file record!\n");
        ReleaseAttributeContext(BitmapContext);
        goto CleanupLock;
    }

    // Write bitmap to disk only after the record body is valid on disk.
    Status = WriteAttribute(DeviceExt, BitmapContext, 0, BitmapData, BitmapDataSize, &LengthWritten, DeviceExt->MasterFileTable);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR encountered when writing $Bitmap attribute!\n");
        ReleaseAttributeContext(BitmapContext);
        goto CleanupLock;
    }

    *DestinationIndex = MftIndex;
    ReleaseAttributeContext(BitmapContext);

CleanupLock:
    if (MftLockHeld)
        ExReleaseResourceLite(&DeviceExt->DirResource);

    return Status;
}

/**
* @name NtfsAddFilenameToDirectory
* @implemented
*
* Adds a $FILE_NAME attribute to a given directory index.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param DirectoryMftIndex
* Mft index of the parent directory which will receive the file.
*
* @param FileReferenceNumber
* File reference of the file to be added to the directory. This is a combination of the
* Mft index and sequence number.
*
* @param FilenameAttribute
* Pointer to the FILENAME_ATTRIBUTE of the file being added to the directory.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
* STATUS_NOT_IMPLEMENTED if target address isn't at the end of the given file record.
*
* @remarks
* WIP - Can only support a few files in a directory.
* One FILENAME_ATTRIBUTE is added to the directory's index for each link to that file. So, each
* file which contains one FILENAME_ATTRIBUTE for a long name and another for the 8.3 name, will
* get both attributes added to its parent directory.
*/
/* Internal worker for NtfsAddFilenameToDirectory.  The public entry point is
 * the wrapper below, which takes Vcb->IndexResource exclusive across this
 * call so concurrent BrowseSubNodeIndexEntries readers can't observe a
 * mid-update INDX block (Kreijstal/reactos#14). */
static
NTSTATUS
NtfsAddFilenameToDirectoryNoLock(PDEVICE_EXTENSION DeviceExt,
                                 ULONGLONG DirectoryMftIndex,
                                 ULONGLONG FileReferenceNumber,
                                 PFILENAME_ATTRIBUTE FilenameAttribute,
                                 BOOLEAN CaseSensitive)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER ParentFileRecord;
    PNTFS_ATTR_CONTEXT IndexRootContext;
    PINDEX_ROOT_ATTRIBUTE I30IndexRoot;
    ULONG IndexRootOffset;
    ULONGLONG I30IndexRootLength;
    ULONG LengthWritten;
    PINDEX_ROOT_ATTRIBUTE NewIndexRoot;
    ULONG AttributeLength;
    PNTFS_ATTR_RECORD NextAttribute;
    PB_TREE NewTree;
    ULONG BtreeIndexLength;
    ULONG MaxIndexRootSize;
    PB_TREE_KEY NewLeftKey;
    PB_TREE_FILENAME_NODE NewRightHandNode;
    LARGE_INTEGER MinIndexRootSize;
    ULONG NewMaxIndexRootSize;
    ULONG NodeSize;
#if NTFS_ENABLE_INVESTIGATION_TRACE
    BOOLEAN TraceIndex;

    TraceIndex = (DirectoryMftIndex == 27);
#endif
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: begin dir=%I64u ref=%I64u name=%.*S\n",
                DirectoryMftIndex,
                FileReferenceNumber,
                FilenameAttribute->NameLength,
                FilenameAttribute->Name);

    // Allocate memory for the parent directory
    ParentFileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!ParentFileRecord)
    {
        DPRINT1("ERROR: Couldn't allocate memory for file record!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Open the parent directory
    Status = ReadFileRecord(DeviceExt, DirectoryMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        DPRINT1("ERROR: Couldn't read parent directory with index %I64u\n",
                DirectoryMftIndex);
        return Status;
    }

#ifndef NDEBUG
    DPRINT1("Dumping old parent file record:\n");
    NtfsDumpFileRecord(DeviceExt, ParentFileRecord);
#endif

    // Find the index root attribute for the directory
    Status = FindAttribute(DeviceExt,
                           ParentFileRecord,
                           AttributeIndexRoot,
                           L"$I30",
                           4,
                           &IndexRootContext,
                           &IndexRootOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find $I30 $INDEX_ROOT attribute for parent directory with MFT #: %I64u (FindAttr=0x%lx)!\n",
                DirectoryMftIndex, Status);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    // Find the maximum index size given what the file record can hold
    // First, find the max index size assuming index root is the last attribute
    MaxIndexRootSize = DeviceExt->NtfsInfo.BytesPerFileRecord               // Start with the size of a file record
                       - IndexRootOffset                                    // Subtract the length of everything that comes before index root
                       - IndexRootContext->pRecord->Resident.ValueOffset    // Subtract the length of the attribute header for index root
                       - sizeof(INDEX_ROOT_ATTRIBUTE)                       // Subtract the length of the index root header
                       - (sizeof(ULONG) * 2);                               // Subtract the length of the file record end marker and padding

    // Are there attributes after this one?
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)ParentFileRecord + IndexRootOffset + IndexRootContext->pRecord->Length);
    if (NextAttribute->Type != AttributeEnd)
    {
        // Find the length of all attributes after this one, not counting the end marker
        ULONG LengthOfAttributes = 0;
        PNTFS_ATTR_RECORD CurrentAttribute = NextAttribute;
        while (CurrentAttribute->Type != AttributeEnd)
        {
            LengthOfAttributes += CurrentAttribute->Length;
            CurrentAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)CurrentAttribute + CurrentAttribute->Length);
        }

        // Leave room for the existing attributes
        MaxIndexRootSize -= LengthOfAttributes;
    }

    // Allocate memory for the index root data
    I30IndexRootLength = AttributeDataLength(IndexRootContext->pRecord);
    I30IndexRoot = ExAllocatePoolWithTag(NonPagedPool, I30IndexRootLength, TAG_NTFS);
    if (!I30IndexRoot)
    {
        DPRINT1("ERROR: Couldn't allocate memory for index root attribute!\n");
        ReleaseAttributeContext(IndexRootContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Read the Index Root
    Status = ReadAttribute(DeviceExt, IndexRootContext, 0, (PCHAR)I30IndexRoot, I30IndexRootLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couln't read index root attribute for Mft index #%I64u\n", DirectoryMftIndex);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    // Convert the index to a B*Tree
    Status = CreateBTreeFromIndex(DeviceExt,
                                  ParentFileRecord,
                                  IndexRootContext,
                                  I30IndexRoot,
                                  &NewTree);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to create B-Tree from Index!\n");
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

#ifndef NDEBUG
    DumpBTree(NewTree);
#endif

    // Insert the key for the file we're adding
    Status = NtfsInsertKey(NewTree,
                           FileReferenceNumber,
                           FilenameAttribute,
                           NewTree->RootNode,
                           CaseSensitive,
                           MaxIndexRootSize,
                           I30IndexRoot->SizeOfEntry,
                           &NewLeftKey,
                           &NewRightHandNode);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to insert key into B-Tree!\n");
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: inserted key\n");

#ifndef NDEBUG
    DumpBTree(NewTree);
#endif

    // The root node can't be split
    ASSERT(NewLeftKey == NULL);
    ASSERT(NewRightHandNode == NULL);

    // Convert B*Tree back to Index

    // Updating the index allocation can change the size available for the index root,
    // And if the index root is demoted, the index allocation will need to be updated again,
    // which may change the size available for index root... etc.
    // My solution is to decrease index root to the size it would be if it was demoted,
    // then UpdateIndexAllocation will have an accurate representation of the maximum space
    // it can use in the file record. There's still a chance that the act of allocating an
    // index node after demoting the index root will increase the size of the file record beyond
    // it's limit, but if that happens, an attribute-list will most definitely be needed.
    // This a bit hacky, but it seems to be functional.

    // Calculate the minimum size of the index root attribute, considering one dummy key and one VCN
    MinIndexRootSize.QuadPart = sizeof(INDEX_ROOT_ATTRIBUTE) // size of the index root headers
                                + 0x18; // Size of dummy key with a VCN for a subnode
    ASSERT(MinIndexRootSize.QuadPart % ATTR_RECORD_ALIGNMENT == 0);

    // Temporarily shrink the index root to it's minimal size
    AttributeLength = MinIndexRootSize.LowPart;
    AttributeLength += sizeof(INDEX_ROOT_ATTRIBUTE);


    // FIXME: IndexRoot will probably be invalid until we're finished. If we fail before we finish, the directory will probably be toast.
    // The potential for catastrophic data-loss exists!!! :)

    // Update the length of the attribute in the file record of the parent directory
    Status = InternalSetResidentAttributeLength(DeviceExt,
                                                IndexRootContext,
                                                ParentFileRecord,
                                                IndexRootOffset,
                                                AttributeLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Unable to set length of index root!\n");
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: shrunk index root\n");

    // Update the index allocation
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: update index allocation 1\n");
    Status = UpdateIndexAllocation(DeviceExt, NewTree, I30IndexRoot->SizeOfEntry, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update index allocation from B-Tree!\n");
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: update index allocation 1 done\n");

#ifndef NDEBUG
    DPRINT1("Index Allocation updated\n");
    DumpBTree(NewTree);
#endif

    // Find the maximum index root size given what the file record can hold
    // First, find the max index size assuming index root is the last attribute
    NewMaxIndexRootSize =
       DeviceExt->NtfsInfo.BytesPerFileRecord                // Start with the size of a file record
        - IndexRootOffset                                    // Subtract the length of everything that comes before index root
        - IndexRootContext->pRecord->Resident.ValueOffset    // Subtract the length of the attribute header for index root
        - sizeof(INDEX_ROOT_ATTRIBUTE)                       // Subtract the length of the index root header
        - (sizeof(ULONG) * 2);                               // Subtract the length of the file record end marker and padding

    // Are there attributes after this one?
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)ParentFileRecord + IndexRootOffset + IndexRootContext->pRecord->Length);
    if (NextAttribute->Type != AttributeEnd)
    {
        // Find the length of all attributes after this one, not counting the end marker
        ULONG LengthOfAttributes = 0;
        PNTFS_ATTR_RECORD CurrentAttribute = NextAttribute;
        while (CurrentAttribute->Type != AttributeEnd)
        {
            LengthOfAttributes += CurrentAttribute->Length;
            CurrentAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)CurrentAttribute + CurrentAttribute->Length);
        }

        // Leave room for the existing attributes
        NewMaxIndexRootSize -= LengthOfAttributes;
    }

    // The index allocation and index bitmap may have grown, leaving less room for the index root,
    // so now we need to double-check that index root isn't too large
    NodeSize = GetSizeOfIndexEntries(NewTree->RootNode);
    if (NodeSize > NewMaxIndexRootSize)
    {
        DPRINT("Demoting index root.\nNodeSize: 0x%lx\nNewMaxIndexRootSize: 0x%lx\n", NodeSize, NewMaxIndexRootSize);

        Status = DemoteBTreeRoot(NewTree, I30IndexRoot->SizeOfEntry, CaseSensitive);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to demote index root!\n");
            DestroyBTree(NewTree);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            return Status;
        }

        // We need to update the index allocation once more
        NTFS_TRACE_IF(TraceIndex, "DRVIDX: update index allocation 2\n");
        Status = UpdateIndexAllocation(DeviceExt, NewTree, I30IndexRoot->SizeOfEntry, ParentFileRecord);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to update index allocation from B-Tree!\n");
            DestroyBTree(NewTree);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            return Status;
        }
        NTFS_TRACE_IF(TraceIndex, "DRVIDX: update index allocation 2 done\n");

        // re-recalculate max size of index root
        NewMaxIndexRootSize =
            // Find the maximum index size given what the file record can hold
            // First, find the max index size assuming index root is the last attribute
            DeviceExt->NtfsInfo.BytesPerFileRecord               // Start with the size of a file record
            - IndexRootOffset                                    // Subtract the length of everything that comes before index root
            - IndexRootContext->pRecord->Resident.ValueOffset    // Subtract the length of the attribute header for index root
            - sizeof(INDEX_ROOT_ATTRIBUTE)                       // Subtract the length of the index root header
            - (sizeof(ULONG) * 2);                               // Subtract the length of the file record end marker and padding

                                                                 // Are there attributes after this one?
        NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)ParentFileRecord + IndexRootOffset + IndexRootContext->pRecord->Length);
        if (NextAttribute->Type != AttributeEnd)
        {
            // Find the length of all attributes after this one, not counting the end marker
            ULONG LengthOfAttributes = 0;
            PNTFS_ATTR_RECORD CurrentAttribute = NextAttribute;
            while (CurrentAttribute->Type != AttributeEnd)
            {
                LengthOfAttributes += CurrentAttribute->Length;
                CurrentAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)CurrentAttribute + CurrentAttribute->Length);
            }

            // Leave room for the existing attributes
            NewMaxIndexRootSize -= LengthOfAttributes;
        }


    }

    // Create the Index Root from the B*Tree
    Status = CreateIndexRootFromBTree(DeviceExt, NewTree, NewMaxIndexRootSize, &NewIndexRoot, &BtreeIndexLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to create Index root from B-Tree!\n");
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: create index root done len=%lu\n", BtreeIndexLength);

    // We're done with the B-Tree now (DestroyBTree releases IndexAllocationContext)
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: destroy btree\n");
    DestroyBTree(NewTree);
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: destroy btree done\n");

    // Write back the new index root attribute to the parent directory file record.
    //
    // IMPORTANT: UpdateIndexAllocation() above may have grown $INDEX_ALLOCATION
    // and $BITMAP via AddRun, which persisted updated data runs to the on-disk
    // MFT record.  Our local ParentFileRecord is now STALE with respect to those
    // data runs.  We MUST re-read from disk before modifying and writing back,
    // otherwise we clobber the data runs that AddRun wrote.
    DPRINT("NtfsAddFilename: re-reading MFT %I64u before final write\n", DirectoryMftIndex);
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: reread parent\n");
    Status = ReadFileRecord(DeviceExt, DirectoryMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to re-read directory file record %I64u!\n", DirectoryMftIndex);
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: reread parent done\n");

    // Re-find the $INDEX_ROOT attribute since offsets may have changed
    ReleaseAttributeContext(IndexRootContext);
    Status = FindAttribute(DeviceExt, ParentFileRecord, AttributeIndexRoot, L"$I30", 4, &IndexRootContext, &IndexRootOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to re-find $INDEX_ROOT after re-read!\n");
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: refind index root done\n");

    // Resize the attribute if needed.
    AttributeLength = NewIndexRoot->Header.AllocatedSize + FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header);

    if (AttributeLength != IndexRootContext->pRecord->Resident.ValueLength)
    {
        NTFS_TRACE_IF(TraceIndex, "DRVIDX: resize final root old=%lu new=%lu\n",
                    IndexRootContext->pRecord->Resident.ValueLength,
                    AttributeLength);
        // Update the length of the attribute in the file record of the parent directory
        Status = InternalSetResidentAttributeLength(DeviceExt,
                                                    IndexRootContext,
                                                    ParentFileRecord,
                                                    IndexRootOffset,
                                                    AttributeLength);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            DPRINT1("ERROR: Unable to set resident attribute length!\n");
            return Status;
        }

    }

    // Write the new index root to disk using the freshly re-read ParentFileRecord.
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: write index root len=%lu\n", AttributeLength);
    Status = WriteAttribute(DeviceExt,
                            IndexRootContext,
                            0,
                            (PUCHAR)NewIndexRoot,
                            AttributeLength,
                            &LengthWritten,
                            ParentFileRecord);
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: write index root done status=0x%lx written=%lu\n",
                Status,
                LengthWritten);
    if (!NT_SUCCESS(Status) || LengthWritten != AttributeLength)
    {
        DPRINT1("ERROR: Unable to write new index root attribute to parent directory!\n");
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

#ifndef NDEBUG
    // re-read the parent file record for debug dumping
    Status = ReadFileRecord(DeviceExt, DirectoryMftIndex, ParentFileRecord);
    if (NT_SUCCESS(Status))
    {
        DPRINT1("Dumping new B-Tree:\n");

        Status = CreateBTreeFromIndex(DeviceExt, ParentFileRecord, IndexRootContext, NewIndexRoot, &NewTree);
        if (NT_SUCCESS(Status))
        {
            DumpBTree(NewTree);
            DestroyBTree(NewTree);
        }

        NtfsDumpFileRecord(DeviceExt, ParentFileRecord);
    }
#endif

    // Cleanup
    ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
    ReleaseAttributeContext(IndexRootContext);
    ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
    NTFS_TRACE_IF(TraceIndex, "DRVIDX: return status=0x%lx\n", Status);

    return Status;
}

/* Public entry point for NtfsAddFilenameToDirectory.  Wraps the worker
 * with Vcb->IndexResource exclusive (Kreijstal/reactos#14). */
NTSTATUS
NtfsAddFilenameToDirectory(PDEVICE_EXTENSION DeviceExt,
                           ULONGLONG DirectoryMftIndex,
                           ULONGLONG FileReferenceNumber,
                           PFILENAME_ATTRIBUTE FilenameAttribute,
                           BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    BOOLEAN IndexLockHeld;

    IndexLockHeld = ExAcquireResourceExclusiveLite(&DeviceExt->IndexResource, TRUE);
    Status = NtfsAddFilenameToDirectoryNoLock(DeviceExt,
                                              DirectoryMftIndex,
                                              FileReferenceNumber,
                                              FilenameAttribute,
                                              CaseSensitive);
    if (IndexLockHeld)
        ExReleaseResourceLite(&DeviceExt->IndexResource);
    return Status;
}

/* Internal worker for NtfsRemoveFilenameFromDirectory.  Wrapped below with
 * IndexResource exclusive (Kreijstal/reactos#14). */
static
NTSTATUS
NtfsRemoveFilenameFromDirectoryNoLock(PDEVICE_EXTENSION DeviceExt,
                                      ULONGLONG ParentMftIndex,
                                      ULONGLONG FileReferenceNumber,
                                      PUNICODE_STRING FileName,
                                      BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER ParentFileRecord;
    PNTFS_ATTR_CONTEXT IndexRootContext;
    PINDEX_ROOT_ATTRIBUTE I30IndexRoot;
    ULONG IndexRootOffset;
    ULONGLONG I30IndexRootLength;
    ULONG LengthWritten;
    PINDEX_ROOT_ATTRIBUTE NewIndexRoot;
    ULONG AttributeLength;
    PNTFS_ATTR_RECORD NextAttribute;
    PB_TREE NewTree;
    ULONG BtreeIndexLength;
    LARGE_INTEGER MinIndexRootSize;
    ULONG NewMaxIndexRootSize;
    ULONG NodeSize;

    ParentFileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!ParentFileRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, ParentMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    Status = FindAttribute(DeviceExt,
                           ParentFileRecord,
                           AttributeIndexRoot,
                           L"$I30",
                           4,
                           &IndexRootContext,
                           &IndexRootOffset);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    I30IndexRootLength = AttributeDataLength(IndexRootContext->pRecord);
    I30IndexRoot = ExAllocatePoolWithTag(NonPagedPool, I30IndexRootLength, TAG_NTFS);
    if (!I30IndexRoot)
    {
        ReleaseAttributeContext(IndexRootContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadAttribute(DeviceExt, IndexRootContext, 0, (PCHAR)I30IndexRoot, I30IndexRootLength);
    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    Status = CreateBTreeFromIndex(DeviceExt,
                                  ParentFileRecord,
                                  IndexRootContext,
                                  I30IndexRoot,
                                  &NewTree);
    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    Status = NtfsRemoveKey(NewTree, FileReferenceNumber, FileName, CaseSensitive);
    if (!NT_SUCCESS(Status))
    {
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    MinIndexRootSize.QuadPart = sizeof(INDEX_ROOT_ATTRIBUTE) + 0x18;
    AttributeLength = MinIndexRootSize.LowPart + sizeof(INDEX_ROOT_ATTRIBUTE);
    Status = InternalSetResidentAttributeLength(DeviceExt,
                                                IndexRootContext,
                                                ParentFileRecord,
                                                IndexRootOffset,
                                                AttributeLength);
    if (!NT_SUCCESS(Status))
    {
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    Status = UpdateIndexAllocation(DeviceExt, NewTree, I30IndexRoot->SizeOfEntry, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    NewMaxIndexRootSize = DeviceExt->NtfsInfo.BytesPerFileRecord
                          - IndexRootOffset
                          - IndexRootContext->pRecord->Resident.ValueOffset
                          - sizeof(INDEX_ROOT_ATTRIBUTE)
                          - (sizeof(ULONG) * 2);

    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)ParentFileRecord + IndexRootOffset + IndexRootContext->pRecord->Length);
    if (NextAttribute->Type != AttributeEnd)
    {
        ULONG LengthOfAttributes = 0;
        PNTFS_ATTR_RECORD CurrentAttribute = NextAttribute;
        while (CurrentAttribute->Type != AttributeEnd)
        {
            LengthOfAttributes += CurrentAttribute->Length;
            CurrentAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)CurrentAttribute + CurrentAttribute->Length);
        }
        NewMaxIndexRootSize -= LengthOfAttributes;
    }

    NodeSize = GetSizeOfIndexEntries(NewTree->RootNode);
    if (NodeSize > NewMaxIndexRootSize)
    {
        Status = DemoteBTreeRoot(NewTree, I30IndexRoot->SizeOfEntry, CaseSensitive);
        if (!NT_SUCCESS(Status))
        {
            DestroyBTree(NewTree);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            return Status;
        }

        Status = UpdateIndexAllocation(DeviceExt, NewTree, I30IndexRoot->SizeOfEntry, ParentFileRecord);
        if (!NT_SUCCESS(Status))
        {
            DestroyBTree(NewTree);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            return Status;
        }
    }

    Status = CreateIndexRootFromBTree(DeviceExt, NewTree, NewMaxIndexRootSize, &NewIndexRoot, &BtreeIndexLength);
    if (!NT_SUCCESS(Status))
    {
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    DestroyBTree(NewTree);

    /*
     * UpdateIndexAllocation() may have grown or rewritten $INDEX_ALLOCATION /
     * $BITMAP runs in the parent directory record.  Do not write our stale
     * pre-update ParentFileRecord back over those changes.
     */
    Status = ReadFileRecord(DeviceExt, ParentMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    ReleaseAttributeContext(IndexRootContext);
    Status = FindAttribute(DeviceExt,
                           ParentFileRecord,
                           AttributeIndexRoot,
                           L"$I30",
                           4,
                           &IndexRootContext,
                           &IndexRootOffset);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    AttributeLength = NewIndexRoot->Header.AllocatedSize + FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header);
    if (AttributeLength != IndexRootContext->pRecord->Resident.ValueLength)
    {
        Status = InternalSetResidentAttributeLength(DeviceExt,
                                                    IndexRootContext,
                                                    ParentFileRecord,
                                                    IndexRootOffset,
                                                    AttributeLength);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            return Status;
        }
    }

    Status = UpdateFileRecord(DeviceExt, ParentMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    Status = WriteAttribute(DeviceExt,
                            IndexRootContext,
                            0,
                            (PUCHAR)NewIndexRoot,
                            AttributeLength,
                            &LengthWritten,
                            ParentFileRecord);
    ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
    ReleaseAttributeContext(IndexRootContext);
    ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
    if (!NT_SUCCESS(Status) || LengthWritten != AttributeLength)
        return !NT_SUCCESS(Status) ? Status : STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

/* Public entry point for NtfsRemoveFilenameFromDirectory.  Wraps the worker
 * with Vcb->IndexResource exclusive (Kreijstal/reactos#14). */
NTSTATUS
NtfsRemoveFilenameFromDirectory(PDEVICE_EXTENSION DeviceExt,
                                ULONGLONG ParentMftIndex,
                                ULONGLONG FileReferenceNumber,
                                PUNICODE_STRING FileName,
                                BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    BOOLEAN IndexLockHeld;

    IndexLockHeld = ExAcquireResourceExclusiveLite(&DeviceExt->IndexResource, TRUE);
    Status = NtfsRemoveFilenameFromDirectoryNoLock(DeviceExt,
                                                   ParentMftIndex,
                                                   FileReferenceNumber,
                                                   FileName,
                                                   CaseSensitive);
    if (IndexLockHeld)
        ExReleaseResourceLite(&DeviceExt->IndexResource);
    return Status;
}

NTSTATUS
NtfsIsDirectoryEmpty(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG DirMFTIndex,
                     BOOLEAN CaseSensitive,
                     PBOOLEAN Empty)
{
    UNICODE_STRING Star = RTL_CONSTANT_STRING(L"*");
    ULONGLONG OutMFTIndex = 0;
    ULONG FirstEntry = 0;
    NTSTATUS Status;

    /* Enumerate the directory's $I30 index for any user entry. A wildcard
     * directory search returns the first child file/subdir (NtfsFindMftRecord
     * already skips system records and DOS-only short-name aliases), so
     * STATUS_SUCCESS means the directory still has contents. */
    Status = NtfsFindMftRecord(DeviceExt,
                               DirMFTIndex,
                               &Star,
                               &FirstEntry,
                               TRUE,
                               CaseSensitive,
                               &OutMFTIndex,
                               NULL);
    if (NT_SUCCESS(Status))
    {
        *Empty = FALSE;
        return STATUS_SUCCESS;
    }

    if (Status == STATUS_OBJECT_NAME_NOT_FOUND ||
        Status == STATUS_OBJECT_PATH_NOT_FOUND ||
        Status == STATUS_NO_MORE_FILES ||
        Status == STATUS_NO_SUCH_FILE)
    {
        *Empty = TRUE;
        return STATUS_SUCCESS;
    }

    /* A genuine error (I/O, allocation, corruption) - don't claim emptiness. */
    return Status;
}

/* A file with more hard links than fit in its base MFT record stores the
 * overflow $FILE_NAME attributes one-per-child in extension records referenced
 * by a resident $ATTRIBUTE_LIST (see NtfsSpillFileNameToChild).  A resident
 * $ATTRIBUTE_LIST in a 1KB record tops out around 29 entries; a non-resident
 * list is unbounded, so the index array is sized from the list value. */

/* Collect the MFT indices of the child records that hold this file's spilled
 * $FILE_NAME attributes.  Returns the count (0 if the file has no attribute
 * list, i.e. all names live in the base record) and, when the count is
 * non-zero, a pool-allocated index array in *IndicesOut (TAG_NTFS; caller
 * frees). */
static
ULONG
NtfsCollectFileNameChildren(PDEVICE_EXTENSION Vcb,
                            PFILE_RECORD_HEADER BaseFileRecord,
                            PULONGLONG *IndicesOut)
{
    PNTFS_ATTR_RECORD Attr;
    PNTFS_ATTR_RECORD List = NULL;
    PUCHAR Value = NULL;
    ULONG ValueLength = 0;
    PUCHAR ItemPtr, ListEnd;
    PULONGLONG Indices;
    ULONG MaxCount;
    ULONG Count = 0;

    *IndicesOut = NULL;

    Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->AttributeOffset);
    while (Attr->Type != AttributeEnd &&
           (ULONG_PTR)Attr < (ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse)
    {
        if (Attr->Length == 0)
            break;
        if (Attr->Type == AttributeAttributeList)
        {
            List = Attr;
            break;
        }
        Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
    }

    if (List == NULL)
        return 0;

    /* Works for both a resident and a non-resident list. */
    if (!NT_SUCCESS(NtfsReadAttributeListValue(Vcb, List, &Value, &ValueLength)) ||
        ValueLength == 0)
    {
        if (Value)
            ExFreePoolWithTag(Value, TAG_NTFS);
        return 0;
    }

    /* Every list item is at least 0x20 bytes (0x1A header, 8-aligned). */
    MaxCount = ValueLength / 0x20 + 1;
    Indices = ExAllocatePoolWithTag(NonPagedPool, MaxCount * sizeof(ULONGLONG), TAG_NTFS);
    if (!Indices)
    {
        ExFreePoolWithTag(Value, TAG_NTFS);
        return 0;
    }

    ItemPtr = Value;
    ListEnd = Value + ValueLength;
    while (ItemPtr + 0x1A <= ListEnd)   /* 0x1A = $ATTRIBUTE_LIST item header before the name */
    {
        PNTFS_ATTRIBUTE_LIST_ITEM Item = (PNTFS_ATTRIBUTE_LIST_ITEM)ItemPtr;
        if (Item->Length == 0)
            break;
        if (Item->Type == AttributeFileName)
        {
            ULONGLONG ChildIdx = Item->MFTIndex & NTFS_MFT_MASK;
            if (ChildIdx != BaseFileRecord->MFTRecordNumber && Count < MaxCount)
                Indices[Count++] = ChildIdx;
        }
        ItemPtr += Item->Length;
    }

    ExFreePoolWithTag(Value, TAG_NTFS);

    if (Count == 0)
    {
        ExFreePoolWithTag(Indices, TAG_NTFS);
        return 0;
    }

    *IndicesOut = Indices;
    return Count;
}

/* Full-delete helper: for every spilled $FILE_NAME child record, drop its
 * parent-directory $I30 entry and free the child MFT record.  Called when the
 * last link of a file is being removed so no dangling index entry or orphaned
 * extension record survives. */
static
NTSTATUS
NtfsRemoveSpilledNames(PDEVICE_EXTENSION DeviceExt,
                       PFILE_RECORD_HEADER BaseFileRecord,
                       ULONGLONG BaseMftIndex,
                       BOOLEAN CaseSensitive)
{
    PULONGLONG Children = NULL;
    ULONG ChildCount, i;
    PFILE_RECORD_HEADER ChildRecord;
    NTSTATUS Status = STATUS_SUCCESS;

    ChildCount = NtfsCollectFileNameChildren(DeviceExt, BaseFileRecord, &Children);
    if (ChildCount == 0)
        return STATUS_SUCCESS;

    ChildRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!ChildRecord)
    {
        ExFreePoolWithTag(Children, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < ChildCount; i++)
    {
        PNTFS_ATTR_RECORD ChildAttr;

        Status = ReadFileRecord(DeviceExt, Children[i], ChildRecord);
        if (!NT_SUCCESS(Status))
            break;

        ChildAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildRecord + ChildRecord->AttributeOffset);
        while (ChildAttr->Type != AttributeEnd &&
               (ULONG_PTR)ChildAttr < (ULONG_PTR)ChildRecord + ChildRecord->BytesInUse)
        {
            if (ChildAttr->Length == 0)
                break;
            if (ChildAttr->Type == AttributeFileName)
            {
                PFILENAME_ATTRIBUTE Fn =
                    (PFILENAME_ATTRIBUTE)((ULONG_PTR)ChildAttr + ChildAttr->Resident.ValueOffset);
                UNICODE_STRING FileName;
                ULONGLONG ParentMftIndex = Fn->DirectoryFileReferenceNumber & NTFS_MFT_MASK;

                FileName.Length = FileName.MaximumLength = Fn->NameLength * sizeof(WCHAR);
                FileName.Buffer = Fn->Name;
                Status = NtfsRemoveFilenameFromDirectory(DeviceExt,
                                                         ParentMftIndex,
                                                         BaseMftIndex,
                                                         &FileName,
                                                         CaseSensitive);
                if (!NT_SUCCESS(Status))
                    goto Done;
            }
            ChildAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildAttr + ChildAttr->Length);
        }

        /* Free the child extension record. */
        ClearFlag(ChildRecord->Flags, FRH_IN_USE);
        ChildRecord->LinkCount = 0;
        Status = UpdateFileRecord(DeviceExt, Children[i], ChildRecord);
        if (!NT_SUCCESS(Status))
            break;
        Status = NtfsSetMftBitmapInUse(DeviceExt, Children[i], FALSE, TRUE);
        if (!NT_SUCCESS(Status))
            break;
    }

Done:
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ChildRecord);
    ExFreePoolWithTag(Children, TAG_NTFS);
    return Status;
}

/* Remove the $ATTRIBUTE_LIST entry that points at ChildMftIndex from the base
 * record, shrinking the list (or removing the whole $ATTRIBUTE_LIST attribute
 * when its last entry goes).  Handles both a resident and a non-resident
 * list.  Mirrors the list-shrink half of CoalesceAttributeFromList without
 * moving an attribute back into the base. */
static
NTSTATUS
NtfsRemoveFileNameListEntry(PDEVICE_EXTENSION Vcb,
                            PFILE_RECORD_HEADER BaseFileRecord,
                            ULONGLONG ChildMftIndex)
{
    PNTFS_ATTR_RECORD Attr;
    PNTFS_ATTR_RECORD List = NULL;
    PUCHAR ListContent, ItemPtr, ListEnd;
    PNTFS_ATTRIBUTE_LIST_ITEM Match = NULL;
    ULONG ValueLen, MatchLen = 0;

    Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)BaseFileRecord + BaseFileRecord->AttributeOffset);
    while (Attr->Type != AttributeEnd &&
           (ULONG_PTR)Attr < (ULONG_PTR)BaseFileRecord + BaseFileRecord->BytesInUse)
    {
        if (Attr->Length == 0)
            break;
        if (Attr->Type == AttributeAttributeList)
        {
            List = Attr;
            break;
        }
        Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
    }

    if (List == NULL)
        return STATUS_SUCCESS;

    if (List->IsNonResident)
    {
        PUCHAR Value = NULL;
        ULONG ValueLen = 0;
        ULONG Pos = 0;
        ULONG MatchOff = MAXULONG;
        ULONG MatchLen2 = 0;
        NTSTATUS Status;

        Status = NtfsReadAttributeListValue(Vcb, List, &Value, &ValueLen);
        if (!NT_SUCCESS(Status))
            return Status;

        while (Pos + 0x1A <= ValueLen)
        {
            PNTFS_ATTRIBUTE_LIST_ITEM Item = (PNTFS_ATTRIBUTE_LIST_ITEM)(Value + Pos);
            if (Item->Length == 0)
                break;
            if (Item->Type == AttributeFileName &&
                (Item->MFTIndex & NTFS_MFT_MASK) == ChildMftIndex)
            {
                MatchOff = Pos;
                MatchLen2 = Item->Length;
                break;
            }
            Pos += Item->Length;
        }

        if (MatchOff == MAXULONG)
        {
            ExFreePoolWithTag(Value, TAG_NTFS);
            return STATUS_SUCCESS;
        }

        RtlMoveMemory(Value + MatchOff,
                      Value + MatchOff + MatchLen2,
                      ValueLen - MatchOff - MatchLen2);

        /* Shrinks the list data (freeing tail clusters), or - when this was
         * the last entry - frees the whole allocation and drops the
         * attribute from the base record. */
        Status = NtfsNonResidentListWriteValue(Vcb, BaseFileRecord, List,
                                               Value, ValueLen - MatchLen2);
        ExFreePoolWithTag(Value, TAG_NTFS);
        return Status;
    }

    ListContent = (PUCHAR)List + List->Resident.ValueOffset;
    ValueLen = List->Resident.ValueLength;
    ItemPtr = ListContent;
    ListEnd = ListContent + ValueLen;
    while (ItemPtr + 0x1A <= ListEnd)   /* 0x1A = $ATTRIBUTE_LIST item header before the name */
    {
        PNTFS_ATTRIBUTE_LIST_ITEM Item = (PNTFS_ATTRIBUTE_LIST_ITEM)ItemPtr;
        if (Item->Length == 0)
            break;
        if (Item->Type == AttributeFileName &&
            (Item->MFTIndex & NTFS_MFT_MASK) == ChildMftIndex)
        {
            Match = Item;
            MatchLen = Item->Length;
            break;
        }
        ItemPtr += Item->Length;
    }

    if (Match == NULL)
        return STATUS_SUCCESS;

    if (ValueLen == MatchLen)
    {
        /* Last entry - drop the whole $ATTRIBUTE_LIST attribute. */
        RemoveResidentAttribute(Vcb, BaseFileRecord, List);
    }
    else
    {
        PUCHAR MatchEnd = (PUCHAR)Match + MatchLen;
        SIZE_T TailLen = ListEnd - MatchEnd;
        ULONG NewValueLen = ValueLen - MatchLen;
        ULONG OldAttrLen = List->Length;
        ULONG NewAttrLen = ALIGN_UP_BY(List->Resident.ValueOffset + NewValueLen, ATTR_RECORD_ALIGNMENT);

        if (TailLen > 0)
            RtlMoveMemory(Match, MatchEnd, TailLen);
        RtlZeroMemory(ListContent + NewValueLen, MatchLen);
        List->Resident.ValueLength = NewValueLen;

        if (NewAttrLen != OldAttrLen)
        {
            ULONG ListOffset = (ULONG)((ULONG_PTR)List - (ULONG_PTR)BaseFileRecord);
            PNTFS_ATTR_RECORD AfterList = (PNTFS_ATTR_RECORD)((ULONG_PTR)List + OldAttrLen);
            List->Length = NewAttrLen;
            if (AfterList->Type != AttributeEnd)
            {
                PNTFS_ATTR_RECORD MovedFinal;
                MovedFinal = MoveAttributes(Vcb, AfterList, ListOffset + OldAttrLen,
                                            (ULONG_PTR)List + NewAttrLen);
                SetFileRecordEnd(BaseFileRecord, MovedFinal, FILE_RECORD_END);
            }
            else
            {
                PNTFS_ATTR_RECORD NewEnd = (PNTFS_ATTR_RECORD)((ULONG_PTR)List + NewAttrLen);
                SetFileRecordEnd(BaseFileRecord, NewEnd, FILE_RECORD_END);
            }
        }
    }

    return STATUS_SUCCESS;
}

/* Unlink a single hard link whose $FILE_NAME was spilled to a child extension
 * record (not found by the base-record walk in NtfsUnlinkSingleName): find the
 * matching child, drop its $I30 entry, free the child, remove its list entry,
 * and decrement the link count. */
static
NTSTATUS
NtfsUnlinkSpilledName(PDEVICE_EXTENSION DeviceExt,
                      PNTFS_FCB Fcb,
                      PFILE_RECORD_HEADER FileRecord,
                      PCUNICODE_STRING LeafName,
                      ULONGLONG LinkParentMftIndex,
                      BOOLEAN CaseSensitive)
{
    PULONGLONG Children = NULL;
    ULONG ChildCount, i;
    PFILE_RECORD_HEADER ChildRecord;
    NTSTATUS Status = STATUS_OBJECT_NAME_NOT_FOUND;
    BOOLEAN Found = FALSE;

    ChildCount = NtfsCollectFileNameChildren(DeviceExt, FileRecord, &Children);
    if (ChildCount == 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    ChildRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!ChildRecord)
    {
        ExFreePoolWithTag(Children, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < ChildCount && !Found; i++)
    {
        PNTFS_ATTR_RECORD ChildAttr;

        Status = ReadFileRecord(DeviceExt, Children[i], ChildRecord);
        if (!NT_SUCCESS(Status))
            break;

        ChildAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildRecord + ChildRecord->AttributeOffset);
        while (ChildAttr->Type != AttributeEnd &&
               (ULONG_PTR)ChildAttr < (ULONG_PTR)ChildRecord + ChildRecord->BytesInUse)
        {
            if (ChildAttr->Length == 0)
                break;
            if (ChildAttr->Type == AttributeFileName)
            {
                PFILENAME_ATTRIBUTE Fn =
                    (PFILENAME_ATTRIBUTE)((ULONG_PTR)ChildAttr + ChildAttr->Resident.ValueOffset);
                UNICODE_STRING EntryName;

                EntryName.Buffer = Fn->Name;
                EntryName.Length = EntryName.MaximumLength = Fn->NameLength * sizeof(WCHAR);
                if ((Fn->DirectoryFileReferenceNumber & NTFS_MFT_MASK) == LinkParentMftIndex &&
                    RtlCompareUnicodeString(&EntryName, LeafName, !CaseSensitive) == 0)
                {
                    Found = TRUE;
                    break;
                }
            }
            ChildAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)ChildAttr + ChildAttr->Length);
        }

        if (!Found)
            continue;

        /* Drop the directory index entry first (same commit order as the base
         * path), then free the child record and remove its list entry. */
        {
            UNICODE_STRING EntryName;
            EntryName.Buffer = ((PFILENAME_ATTRIBUTE)((ULONG_PTR)ChildAttr + ChildAttr->Resident.ValueOffset))->Name;
            EntryName.Length = EntryName.MaximumLength =
                ((PFILENAME_ATTRIBUTE)((ULONG_PTR)ChildAttr + ChildAttr->Resident.ValueOffset))->NameLength * sizeof(WCHAR);
            Status = NtfsRemoveFilenameFromDirectory(DeviceExt, LinkParentMftIndex,
                                                     Fcb->MFTIndex, &EntryName, CaseSensitive);
            if (!NT_SUCCESS(Status))
                break;
        }

        ClearFlag(ChildRecord->Flags, FRH_IN_USE);
        ChildRecord->LinkCount = 0;
        Status = UpdateFileRecord(DeviceExt, Children[i], ChildRecord);
        if (!NT_SUCCESS(Status))
            break;
        Status = NtfsSetMftBitmapInUse(DeviceExt, Children[i], FALSE, TRUE);
        if (!NT_SUCCESS(Status))
            break;

        NtfsRemoveFileNameListEntry(DeviceExt, FileRecord, Children[i]);

        FileRecord->LinkCount--;
        Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
        if (NT_SUCCESS(Status))
            Fcb->LinkCount = FileRecord->LinkCount;
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ChildRecord);
    ExFreePoolWithTag(Children, TAG_NTFS);
    return Found ? Status : STATUS_OBJECT_NAME_NOT_FOUND;
}

/* Unlink one hard link of a multi-link file: remove the matching $FILE_NAME
 * attribute(s) from the base record, drop the parent directory's $I30
 * entries, and decrement the link count.  The record itself stays in use -
 * the other links keep the file alive, matching Windows delete semantics.
 * A split WIN32/DOS 8.3 name pair in the same directory is one link (NTFS
 * allows a single DOS alias per record) and falls together.  A name spilled to
 * a child extension record is handled by NtfsUnlinkSpilledName. */
static
NTSTATUS
NtfsUnlinkSingleName(PDEVICE_EXTENSION DeviceExt,
                     PNTFS_FCB Fcb,
                     PFILE_RECORD_HEADER FileRecord,
                     PCUNICODE_STRING LeafName,
                     ULONGLONG LinkParentMftIndex,
                     BOOLEAN CaseSensitive)
{
    PNTFS_ATTR_RECORD Attribute;
    PNTFS_ATTR_RECORD Target = NULL, Partner = NULL;
    PFILENAME_ATTRIBUTE Name, TargetName = NULL, PartnerName = NULL;
    UNICODE_STRING EntryName;
    NTSTATUS Status;
    USHORT Removed;

    /* Find the link's $FILE_NAME in the base record.  (A $FILE_NAME pushed
     * out to an extension record by an attribute list cannot be edited
     * through RemoveResidentAttribute; no ReactOS-created record does that.) */
    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
    while ((ULONG_PTR)Attribute < (ULONG_PTR)FileRecord + FileRecord->BytesInUse &&
           Attribute->Type != AttributeEnd)
    {
        if (Attribute->Type == AttributeFileName)
        {
            Name = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
            if ((Name->DirectoryFileReferenceNumber & NTFS_MFT_MASK) == LinkParentMftIndex)
            {
                EntryName.Buffer = Name->Name;
                EntryName.Length = EntryName.MaximumLength = Name->NameLength * sizeof(WCHAR);
                if (Target == NULL &&
                    RtlCompareUnicodeString(&EntryName, LeafName, !CaseSensitive) == 0)
                {
                    Target = Attribute;
                    TargetName = Name;
                }
            }
        }

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
    }

    if (Target == NULL)
    {
        /* The name may have been spilled to an $ATTRIBUTE_LIST child record. */
        return NtfsUnlinkSpilledName(DeviceExt, Fcb, FileRecord, LeafName,
                                     LinkParentMftIndex, CaseSensitive);
    }

    /* A split-pair name drags its partner in the same directory along:
     * deleting the WIN32 long name removes the DOS alias and vice versa. */
    if (TargetName->NameType == NTFS_FILE_NAME_WIN32 ||
        TargetName->NameType == NTFS_FILE_NAME_DOS)
    {
        UCHAR WantedType = (TargetName->NameType == NTFS_FILE_NAME_WIN32)
                               ? NTFS_FILE_NAME_DOS : NTFS_FILE_NAME_WIN32;

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
        while ((ULONG_PTR)Attribute < (ULONG_PTR)FileRecord + FileRecord->BytesInUse &&
               Attribute->Type != AttributeEnd)
        {
            if (Attribute->Type == AttributeFileName && Attribute != Target)
            {
                Name = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
                if ((Name->DirectoryFileReferenceNumber & NTFS_MFT_MASK) == LinkParentMftIndex &&
                    Name->NameType == WantedType)
                {
                    Partner = Attribute;
                    PartnerName = Name;
                    break;
                }
            }

            Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
        }
    }

    /* Directory index entries first, then the record - same commit order as
     * the full-delete path below. */
    EntryName.Buffer = TargetName->Name;
    EntryName.Length = EntryName.MaximumLength = TargetName->NameLength * sizeof(WCHAR);
    Status = NtfsRemoveFilenameFromDirectory(DeviceExt,
                                             LinkParentMftIndex,
                                             Fcb->MFTIndex,
                                             &EntryName,
                                             CaseSensitive);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Partner != NULL)
    {
        EntryName.Buffer = PartnerName->Name;
        EntryName.Length = EntryName.MaximumLength = PartnerName->NameLength * sizeof(WCHAR);
        Status = NtfsRemoveFilenameFromDirectory(DeviceExt,
                                                 LinkParentMftIndex,
                                                 Fcb->MFTIndex,
                                                 &EntryName,
                                                 CaseSensitive);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Remove by (type, instance), re-finding each slot: the first removal
     * slides the record contents (and, when the record carries an
     * $ATTRIBUTE_LIST, shrinking the list moves even lower-offset slots), so
     * raw attribute pointers cannot be held across a removal. */
    Removed = (Partner != NULL) ? 2 : 1;
    {
        USHORT Instances[2];
        ULONG InstanceCount = 0;
        ULONG i;

        Instances[InstanceCount++] = Target->Instance;
        if (Partner != NULL)
            Instances[InstanceCount++] = Partner->Instance;

        for (i = 0; i < InstanceCount; i++)
        {
            PNTFS_ATTR_RECORD Attr =
                (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

            while (Attr->Type != AttributeEnd && Attr->Length > 0 &&
                   (ULONG_PTR)Attr < (ULONG_PTR)FileRecord + FileRecord->BytesInUse)
            {
                if (Attr->Type == AttributeFileName && Attr->Instance == Instances[i])
                {
                    RemoveResidentAttribute(DeviceExt, FileRecord, Attr);
                    break;
                }
                Attr = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attr + Attr->Length);
            }
        }
    }

    FileRecord->LinkCount -= Removed;
    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Fcb->LinkCount = FileRecord->LinkCount;

    return Status;
}

NTSTATUS
NtfsDeleteFileRecord(PDEVICE_EXTENSION DeviceExt,
                     PNTFS_FCB Fcb,
                     PCUNICODE_STRING LinkName,
                     ULONGLONG LinkParentMftIndex,
                     BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    PFILENAME_ATTRIBUTE FileNameAttribute;
    UNICODE_STRING FileName;
    ULONGLONG ParentMftIndex;
    PFILE_RECORD_HEADER FileRecord;

    /* A directory can only be deleted once it is empty. This is the choke
     * point for both the immediate (last-handle) and the deferred
     * (delete-on-close) disposition paths, so re-verify emptiness here even
     * if the caller already checked: a delete-pending directory may still be
     * the target of a create between the disposition set and the final close. */
    if (NtfsFCBIsDirectory(Fcb))
    {
        BOOLEAN Empty = FALSE;

        if (NtfsFCBIsRoot(Fcb))
            return STATUS_CANNOT_DELETE;

        Status = NtfsIsDirectoryEmpty(DeviceExt, Fcb->MFTIndex, CaseSensitive, &Empty);
        if (!NT_SUCCESS(Status))
            return Status;
        if (!Empty)
            return STATUS_DIRECTORY_NOT_EMPTY;
    }

    /* Invalidate FCB-level MFT record cache: deletion clears FRH_IN_USE
     * and LinkCount in the on-disk record. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!FileRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    /* Hard-linked file?  Deleting one name must only unlink that name; the
     * record and its remaining links survive (Windows semantics).  Count the
     * distinct links first - pure-DOS 8.3 attributes are aliases of a WIN32
     * name, not links of their own. */
    {
        FIND_ATTR_CONTXT FindContext;
        PNTFS_ATTR_RECORD Attribute;
        NTSTATUS FindStatus;
        ULONG NonDosLinks = 0;

        FindStatus = FindFirstAttribute(&FindContext, DeviceExt, FileRecord, FALSE, &Attribute);
        while (NT_SUCCESS(FindStatus))
        {
            if (Attribute->Type == AttributeFileName)
            {
                FileNameAttribute = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
                if (FileNameAttribute->NameType != NTFS_FILE_NAME_DOS)
                    NonDosLinks++;
            }

            FindStatus = FindNextAttribute(&FindContext, &Attribute);
        }
        FindCloseAttribute(&FindContext);

        /* $FILE_NAME attributes spilled to $ATTRIBUTE_LIST child records are not
         * visited by the base-only FindFirst/NextAttribute walk above; count
         * them too (every spilled name is a non-DOS hard link).  Without this a
         * heavily hard-linked file looks single-linked and gets fully deleted,
         * orphaning its child records and their directory index entries. */
        {
            PULONGLONG SpillChildren = NULL;

            NonDosLinks += NtfsCollectFileNameChildren(DeviceExt, FileRecord, &SpillChildren);
            if (SpillChildren)
                ExFreePoolWithTag(SpillChildren, TAG_NTFS);
        }

        if (NonDosLinks > 1)
        {
            UNICODE_STRING LeafName;

            /* The rename-over-existing path names the link it replaces
             * explicitly; the cleanup path deletes the FCB's own name. */
            if (LinkName != NULL)
            {
                LeafName = *LinkName;
            }
            else if (Fcb->ObjectName != NULL && Fcb->ObjectName[0] != UNICODE_NULL)
            {
                PWSTR Leaf = Fcb->ObjectName;

                if (Leaf[0] == L'\\')
                    Leaf++;
                RtlInitUnicodeString(&LeafName, Leaf);
            }
            else
            {
                RtlInitUnicodeString(&LeafName, NULL);
            }

            /* Resolve the opened name's parent directory when the caller
             * didn't pass it.  Fcb->Entry describes the record's "best"
             * name, which for a multi-link file can be a different link,
             * so walk the FCB's own parent path instead. */
            if (LinkParentMftIndex == 0)
            {
                UNICODE_STRING ParentPath;
                PFILE_RECORD_HEADER ParentRecord = NULL;

                ParentPath.Buffer = Fcb->PathName;
                if (Fcb->ObjectName != NULL && Fcb->ObjectName > Fcb->PathName)
                    ParentPath.Length = (USHORT)((ULONG_PTR)Fcb->ObjectName - (ULONG_PTR)Fcb->PathName);
                else
                    ParentPath.Length = 0;
                if (ParentPath.Length == 0)
                {
                    ParentPath.Buffer = L"\\";
                    ParentPath.Length = sizeof(WCHAR);
                }
                ParentPath.MaximumLength = ParentPath.Length;

                Status = NtfsLookupFile(DeviceExt,
                                        &ParentPath,
                                        CaseSensitive,
                                        &ParentRecord,
                                        &LinkParentMftIndex);
                if (!NT_SUCCESS(Status))
                {
                    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
                    return Status;
                }
                ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentRecord);
            }

            Status = NtfsUnlinkSingleName(DeviceExt,
                                          Fcb,
                                          FileRecord,
                                          &LeafName,
                                          LinkParentMftIndex,
                                          CaseSensitive);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return Status;
        }
    }

    /* A file record can carry more than one $FILE_NAME attribute - typically a
     * Win32 long name together with its generated DOS 8.3 short-name alias (and,
     * for hardlinked files, names in several directories) - and each of them has
     * its own entry in a parent directory's $I30 index.  Remove every one of
     * them: dropping only the "best" name (as we used to) leaves the other index
     * entries behind, and once this MFT record is freed and later reused those
     * stale entries cross-link the directory index to an unrelated record. */
    {
        FIND_ATTR_CONTXT FindContext;
        PNTFS_ATTR_RECORD Attribute;
        BOOLEAN FoundName = FALSE;
        NTSTATUS FindStatus;

        FindStatus = FindFirstAttribute(&FindContext, DeviceExt, FileRecord, FALSE, &Attribute);
        while (NT_SUCCESS(FindStatus))
        {
            if (Attribute->Type == AttributeFileName)
            {
                FileNameAttribute = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);

                ParentMftIndex = FileNameAttribute->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
                FileName.Length = FileName.MaximumLength = FileNameAttribute->NameLength * sizeof(WCHAR);
                FileName.Buffer = FileNameAttribute->Name;

                Status = NtfsRemoveFilenameFromDirectory(DeviceExt,
                                                         ParentMftIndex,
                                                         Fcb->MFTIndex,
                                                         &FileName,
                                                         CaseSensitive);
                if (!NT_SUCCESS(Status))
                {
                    FindCloseAttribute(&FindContext);
                    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
                    return Status;
                }

                FoundName = TRUE;
            }

            FindStatus = FindNextAttribute(&FindContext, &Attribute);
        }
        FindCloseAttribute(&FindContext);

        /* No $FILE_NAME in the base record is still a legitimate last-link
         * delete when the sole remaining name was spilled to an
         * $ATTRIBUTE_LIST child record (heavy hard-linking can evict every
         * base name; the unlink-single-name path above only runs while MORE
         * than one link remains).  NtfsRemoveSpilledNames below drops the
         * child's directory index entry and frees the child record.  Only a
         * record with no name anywhere is an error. */
        if (!FoundName)
        {
            PULONGLONG SpillChildren = NULL;

            if (NtfsCollectFileNameChildren(DeviceExt, FileRecord, &SpillChildren) == 0)
            {
                ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            ExFreePoolWithTag(SpillChildren, TAG_NTFS);
        }
    }

    /* Drop the parent-directory index entries for any spilled names and free
     * their child extension records before the base record is freed below. */
    Status = NtfsRemoveSpilledNames(DeviceExt, FileRecord, Fcb->MFTIndex, CaseSensitive);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    /* If the $ATTRIBUTE_LIST outgrew the record and went non-resident, its
     * cluster allocation dies with the record - free it here or the clusters
     * leak (offline chkdsk reports code 25). */
    Status = NtfsFreeAttributeListClusters(DeviceExt, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    ClearFlag(FileRecord->Flags, FRH_IN_USE);
    FileRecord->LinkCount = 0;
    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsSetMftBitmapInUse(DeviceExt, Fcb->MFTIndex, FALSE, TRUE);
    }
    if (NT_SUCCESS(Status))
        Fcb->LinkCount = 0;

    return Status;
}

/**
* @name NtfsDeleteStream
* @implemented
*
* Deletes the named $DATA attribute an alternate-data-stream FCB refers to,
* leaving the base file (and every other stream) intact.  Counterpart of
* NtfsDeleteFileRecord for stream handles: NtfsCleanupFile routes a
* delete-pending FCB here when Fcb->Stream names an alternate stream.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param Fcb
* The stream FCB (Fcb->Stream[0] != UNICODE_NULL) to delete.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_CANNOT_DELETE if the attribute was migrated to a child record via
* $ATTRIBUTE_LIST (removing it would also require rewriting the list entry,
* which is out of scope here - refuse cleanly rather than corrupt the chain).
* Lookup/IO errors otherwise.
*/
NTSTATUS
NtfsDeleteStream(PDEVICE_EXTENSION DeviceExt,
                 PNTFS_FCB Fcb)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    PNTFS_ATTR_RECORD Attribute;
    ULONG AttrOffset;

    ASSERT(Fcb->Stream[0] != UNICODE_NULL);

    /* The on-disk record is about to change. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!FileRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    Status = FindAttribute(DeviceExt, FileRecord, AttributeData,
                           Fcb->Stream, wcslen(Fcb->Stream),
                           &DataContext, &AttrOffset);
    if (!NT_SUCCESS(Status))
    {
        /* Stream already gone - treat as deleted. */
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_SUCCESS;
    }

    if (DataContext->MigratedToMFTIndex != 0)
    {
        DPRINT1("NtfsDeleteStream: '%S' lives in an $ATTRIBUTE_LIST child record, refusing\n",
                Fcb->Stream);
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_CANNOT_DELETE;
    }

    /* Release any allocated clusters before dropping the attribute record. */
    if (DataContext->pRecord->IsNonResident)
    {
        LARGE_INTEGER Zero;

        Zero.QuadPart = 0;
        Status = SetNonResidentAttributeDataLength(DeviceExt, DataContext,
                                                   AttrOffset, FileRecord, &Zero);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("NtfsDeleteStream: failed to free '%S' clusters: 0x%lx\n",
                    Fcb->Stream, Status);
            ReleaseAttributeContext(DataContext);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return Status;
        }
    }

    /* Cut the attribute out of the record.  RemoveResidentAttribute (which,
     * despite the name, just slides record memory and so works for any slot
     * type) also drops the stream's $ATTRIBUTE_LIST entry when the record
     * carries a list - e.g. a hard-link-spilled file with an ADS. */
    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    ASSERT(Attribute->Type == AttributeData);
    ASSERT(AttrOffset + Attribute->Length <= FileRecord->BytesInUse);

    RemoveResidentAttribute(DeviceExt, FileRecord, Attribute);

    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);

    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (NT_SUCCESS(Status))
    {
        /* The base file's FCB (if cached) may hold a stale copy of the
         * record that still lists this stream. */
        SIZE_T PathLen = wcslen(Fcb->PathName);
        SIZE_T StreamLen = wcslen(Fcb->Stream);

        if (PathLen > StreamLen &&
            Fcb->PathName[PathLen - StreamLen - 1] == L':')
        {
            WCHAR BasePath[MAX_PATH];
            PNTFS_FCB BaseFcb;

            RtlCopyMemory(BasePath, Fcb->PathName, (PathLen - StreamLen - 1) * sizeof(WCHAR));
            BasePath[PathLen - StreamLen - 1] = UNICODE_NULL;

            BaseFcb = NtfsGrabFCBFromTable(DeviceExt, BasePath);
            if (BaseFcb != NULL)
            {
                NtfsInvalidateCachedFileRecord(BaseFcb);
                NtfsReleaseFCB(DeviceExt, BaseFcb);
            }
        }

        /* Make sure this FCB no longer satisfies name lookups (mirrors
         * NtfsDeleteFileRecord): a later open of the same stream name must
         * create a fresh attribute instead of reusing this dead FCB. */
        Fcb->LinkCount = 0;
    }

    return Status;
}

NTSTATUS
NtfsRenameFileRecord(PDEVICE_EXTENSION DeviceExt,
                     PNTFS_FCB Fcb,
                     ULONGLONG NewParentMftIndex,
                     PUNICODE_STRING NewFileName,
                     BOOLEAN ReplaceIfExists,
                     BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PFILE_RECORD_HEADER ExistingRecord = NULL;
    PFILE_RECORD_HEADER ParentFileRecord = NULL;
    PNTFS_ATTR_CONTEXT FileNameContext = NULL;
    PNTFS_ATTR_RECORD FileNameRecord;
    PFILENAME_ATTRIBUTE ExistingName;
    PFILENAME_ATTRIBUTE CurrentName;
    UCHAR CurNameBuf[NTFS_FOUND_NAME_SIZE];
    UCHAR ExistNameBuf[NTFS_FOUND_NAME_SIZE];
    PFILENAME_ATTRIBUTE NewDirectoryEntry = NULL;
    UNICODE_STRING OldFileName;
    PWCHAR OldFileNameBuffer = NULL;
    ULONGLONG OldParentMftIndex;
    ULONGLONG ExistingMftIndex;
    ULONGLONG FileReferenceNumber;
    ULONG FileNameOffset;
    ULONG NewFileNameLength;
    USHORT ParentSequenceNumber;
    NTFS_FCB ExistingFcb;

    if (NtfsFCBIsRoot(Fcb))
        return STATUS_ACCESS_DENIED;

    if (NewFileName->Length == 0 || FsRtlDoesNameContainWildCards(NewFileName))
        return STATUS_OBJECT_NAME_INVALID;

    /* Invalidate FCB-level MFT record cache: rename rewrites $FILENAME on
     * the source record. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!FileRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    CurrentName = GetBestFileNameFromRecord(DeviceExt, FileRecord,
                                            (PFILENAME_ATTRIBUTE)CurNameBuf);
    if (CurrentName == NULL)
    {
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
        goto Cleanup;
    }

    OldParentMftIndex = CurrentName->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
    OldFileName.Length = CurrentName->NameLength * sizeof(WCHAR);
    OldFileName.MaximumLength = OldFileName.Length;
    OldFileNameBuffer = ExAllocatePoolWithTag(NonPagedPool, OldFileName.Length, TAG_NTFS);
    if (!OldFileNameBuffer)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlCopyMemory(OldFileNameBuffer, CurrentName->Name, OldFileName.Length);
    OldFileName.Buffer = OldFileNameBuffer;

    if (OldParentMftIndex == NewParentMftIndex &&
        RtlEqualUnicodeString(&OldFileName, NewFileName, FALSE))
    {
        Status = STATUS_SUCCESS;
        goto Cleanup;
    }

    Status = NtfsLookupFileAt(DeviceExt,
                              NewFileName,
                              CaseSensitive,
                              &ExistingRecord,
                              &ExistingMftIndex,
                              NewParentMftIndex);
    if (NT_SUCCESS(Status))
    {
        ExistingName = GetBestFileNameFromRecord(DeviceExt, ExistingRecord,
                                                 (PFILENAME_ATTRIBUTE)ExistNameBuf);
        if (ExistingMftIndex == Fcb->MFTIndex)
        {
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ExistingRecord);
            ExistingRecord = NULL;
        }
        else if (!ReplaceIfExists || ExistingName == NULL)
        {
            Status = STATUS_OBJECT_NAME_COLLISION;
            goto Cleanup;
        }
        else
        {
            RtlZeroMemory(&ExistingFcb, sizeof(ExistingFcb));
            /* Copy only the fixed FILENAME_ATTRIBUTE header (up to but not
             * including the Name array).  FCB::Entry embeds a FILENAME_ATTRIBUTE
             * whose Name is declared Name[1], so appending the variable-length
             * name here would overrun Entry straight into the adjacent
             * CachedFileRecord pointer - which NtfsInvalidateCachedFileRecord
             * then frees, faulting in ExFreePoolWithTag.  NtfsDeleteFileRecord
             * re-reads every $FILE_NAME from the on-disk record via MFTIndex and
             * never consults Entry.Name, so the header (which carries
             * FileAttributes for the directory check) is all it needs. */
            RtlCopyMemory(&ExistingFcb.Entry,
                          ExistingName,
                          FIELD_OFFSET(FILENAME_ATTRIBUTE, Name));
            ExistingFcb.MFTIndex = ExistingMftIndex;
            ExistingFcb.LinkCount = ExistingRecord->LinkCount;

            Status = NtfsDeleteFileRecord(DeviceExt,
                                          &ExistingFcb,
                                          NewFileName,
                                          NewParentMftIndex,
                                          CaseSensitive);
            if (!NT_SUCCESS(Status))
                goto Cleanup;

            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ExistingRecord);
            ExistingRecord = NULL;
        }
    }
    else if (Status != STATUS_OBJECT_NAME_NOT_FOUND && Status != STATUS_OBJECT_PATH_NOT_FOUND)
    {
        goto Cleanup;
    }

    ParentSequenceNumber = NTFS_FILE_ROOT;
    if (NewParentMftIndex != NTFS_FILE_ROOT)
    {
        ParentFileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        if (!ParentFileRecord)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }

        Status = ReadFileRecord(DeviceExt, NewParentMftIndex, ParentFileRecord);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        ParentSequenceNumber = ParentFileRecord->SequenceNumber;
    }

    NewFileNameLength = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + NewFileName->Length;
    NewDirectoryEntry = ExAllocatePoolWithTag(NonPagedPool, NewFileNameLength, TAG_NTFS);
    if (!NewDirectoryEntry)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    RtlZeroMemory(NewDirectoryEntry, NewFileNameLength);
    RtlCopyMemory(NewDirectoryEntry,
                  CurrentName,
                  min(GetFileNameAttributeLength(CurrentName),
                      FIELD_OFFSET(FILENAME_ATTRIBUTE, Name)));
    NewDirectoryEntry->DirectoryFileReferenceNumber = NewParentMftIndex;
    if (NewParentMftIndex == NTFS_FILE_ROOT)
        NewDirectoryEntry->DirectoryFileReferenceNumber |= (ULONGLONG)NTFS_FILE_ROOT << 48;
    else
        NewDirectoryEntry->DirectoryFileReferenceNumber |= (ULONGLONG)ParentSequenceNumber << 48;
    NewDirectoryEntry->NameLength = NewFileName->Length / sizeof(WCHAR);
    if (!CaseSensitive && RtlIsNameLegalDOS8Dot3(NewFileName, NULL, NULL))
        NewDirectoryEntry->NameType = NTFS_FILE_NAME_WIN32_AND_DOS;
    else
        NewDirectoryEntry->NameType = NTFS_FILE_NAME_POSIX;
    RtlCopyMemory(NewDirectoryEntry->Name, NewFileName->Buffer, NewFileName->Length);

    FileReferenceNumber = Fcb->MFTIndex | ((ULONGLONG)FileRecord->SequenceNumber << 48);
    Status = NtfsAddFilenameToDirectory(DeviceExt,
                                        NewParentMftIndex,
                                        FileReferenceNumber,
                                        NewDirectoryEntry,
                                        CaseSensitive);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = FindAttribute(DeviceExt,
                           FileRecord,
                           AttributeFileName,
                           NULL,
                           0,
                           &FileNameContext,
                           &FileNameOffset);
    if (!NT_SUCCESS(Status))
        goto RollbackNewName;

    Status = InternalSetResidentAttributeLength(DeviceExt,
                                                FileNameContext,
                                                FileRecord,
                                                FileNameOffset,
                                                NewFileNameLength);
    if (!NT_SUCCESS(Status))
        goto RollbackNewName;

    FileNameRecord = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileNameOffset);
    CurrentName = (PFILENAME_ATTRIBUTE)((ULONG_PTR)FileNameRecord + FileNameRecord->Resident.ValueOffset);
    RtlZeroMemory(CurrentName, NewFileNameLength);
    RtlCopyMemory(CurrentName, NewDirectoryEntry, NewFileNameLength);

    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto RollbackNewName;

    Status = NtfsRemoveFilenameFromDirectory(DeviceExt,
                                             OldParentMftIndex,
                                             Fcb->MFTIndex,
                                             &OldFileName,
                                             CaseSensitive);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RtlCopyMemory(&Fcb->Entry,
                  NewDirectoryEntry,
                  FIELD_OFFSET(FILENAME_ATTRIBUTE, NameLength));
    Fcb->Entry.NameType = NewDirectoryEntry->NameType;
    Fcb->Entry.NameLength = 0;
    Fcb->Entry.Name[0] = UNICODE_NULL;
    Status = STATUS_SUCCESS;
    goto Cleanup;

RollbackNewName:
    NtfsRemoveFilenameFromDirectory(DeviceExt,
                                    NewParentMftIndex,
                                    Fcb->MFTIndex,
                                    NewFileName,
                                    CaseSensitive);

Cleanup:
    if (OldFileNameBuffer)
        ExFreePoolWithTag(OldFileNameBuffer, TAG_NTFS);
    if (FileNameContext)
        ReleaseAttributeContext(FileNameContext);
    if (NewDirectoryEntry)
        ExFreePoolWithTag(NewDirectoryEntry, TAG_NTFS);
    if (ParentFileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
    if (ExistingRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ExistingRecord);
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

NTSTATUS
NtfsLinkFileRecord(PDEVICE_EXTENSION DeviceExt,
                   PNTFS_FCB Fcb,
                   ULONGLONG NewParentMftIndex,
                   PUNICODE_STRING NewFileName,
                   BOOLEAN ReplaceIfExists,
                   BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PFILE_RECORD_HEADER ExistingRecord = NULL;
    PFILE_RECORD_HEADER ParentFileRecord = NULL;
    PFILENAME_ATTRIBUTE CurrentName;
    UCHAR CurNameBuf[NTFS_FOUND_NAME_SIZE];
    PFILENAME_ATTRIBUTE NewDirectoryEntry = NULL;
    ULONGLONG ExistingMftIndex;
    ULONGLONG FileReferenceNumber;
    ULONG NewFileNameLength;
    USHORT ParentSequenceNumber;

    if (NtfsFCBIsRoot(Fcb) || NtfsFCBIsDirectory(Fcb) || Fcb->Stream[0] != UNICODE_NULL)
        return STATUS_INVALID_PARAMETER;

    if (NewFileName->Length == 0 || FsRtlDoesNameContainWildCards(NewFileName))
        return STATUS_OBJECT_NAME_INVALID;

    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!FileRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (FileRecord->LinkCount == MAXUSHORT)
    {
        Status = STATUS_TOO_MANY_LINKS;
        goto Cleanup;
    }

    CurrentName = GetBestFileNameFromRecord(DeviceExt, FileRecord,
                                            (PFILENAME_ATTRIBUTE)CurNameBuf);
    if (CurrentName == NULL)
    {
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
        goto Cleanup;
    }

    Status = NtfsLookupFileAt(DeviceExt,
                              NewFileName,
                              CaseSensitive,
                              &ExistingRecord,
                              &ExistingMftIndex,
                              NewParentMftIndex);
    if (NT_SUCCESS(Status))
    {
        Status = (ExistingMftIndex == Fcb->MFTIndex) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_COLLISION;
        goto Cleanup;
    }
    if (Status != STATUS_OBJECT_NAME_NOT_FOUND && Status != STATUS_OBJECT_PATH_NOT_FOUND)
        goto Cleanup;

    ParentSequenceNumber = NTFS_FILE_ROOT;
    if (NewParentMftIndex != NTFS_FILE_ROOT)
    {
        ParentFileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        if (!ParentFileRecord)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }

        Status = ReadFileRecord(DeviceExt, NewParentMftIndex, ParentFileRecord);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        if (!(ParentFileRecord->Flags & FRH_DIRECTORY))
        {
            Status = STATUS_NOT_A_DIRECTORY;
            goto Cleanup;
        }

        ParentSequenceNumber = ParentFileRecord->SequenceNumber;
    }

    NewFileNameLength = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + NewFileName->Length;
    NewDirectoryEntry = ExAllocatePoolWithTag(NonPagedPool, NewFileNameLength, TAG_NTFS);
    if (!NewDirectoryEntry)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    RtlZeroMemory(NewDirectoryEntry, NewFileNameLength);
    RtlCopyMemory(NewDirectoryEntry,
                  CurrentName,
                  min(GetFileNameAttributeLength(CurrentName),
                      FIELD_OFFSET(FILENAME_ATTRIBUTE, Name)));
    NewDirectoryEntry->DirectoryFileReferenceNumber = NewParentMftIndex;
    if (NewParentMftIndex == NTFS_FILE_ROOT)
        NewDirectoryEntry->DirectoryFileReferenceNumber |= (ULONGLONG)NTFS_FILE_ROOT << 48;
    else
        NewDirectoryEntry->DirectoryFileReferenceNumber |= (ULONGLONG)ParentSequenceNumber << 48;
    NewDirectoryEntry->NameLength = NewFileName->Length / sizeof(WCHAR);
    if (!CaseSensitive && RtlIsNameLegalDOS8Dot3(NewFileName, NULL, NULL))
        NewDirectoryEntry->NameType = NTFS_FILE_NAME_WIN32_AND_DOS;
    else
        NewDirectoryEntry->NameType = NTFS_FILE_NAME_POSIX;
    RtlCopyMemory(NewDirectoryEntry->Name, NewFileName->Buffer, NewFileName->Length);

    FileReferenceNumber = Fcb->MFTIndex | ((ULONGLONG)FileRecord->SequenceNumber << 48);
    Status = NtfsAddFilenameToDirectory(DeviceExt,
                                        NewParentMftIndex,
                                        FileReferenceNumber,
                                        NewDirectoryEntry,
                                        CaseSensitive);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = AddResidentAttribute(DeviceExt,
                                  FileRecord,
                                  AttributeFileName,
                                  NULL,
                                  0,
                                  NewDirectoryEntry,
                                  NewFileNameLength);
    if (Status == STATUS_DISK_FULL)
    {
        /* The base MFT record is already full of $FILE_NAME attributes (this file
         * has more hard links than fit in one record).  Spill the new name into a
         * child record referenced by an $ATTRIBUTE_LIST.  The spill path sets
         * RA_INDEXED on the child attribute itself. */
        Status = NtfsAddHardLinkSpill(DeviceExt, FileRecord, NewDirectoryEntry, NewFileNameLength);
        if (!NT_SUCCESS(Status))
            goto RollbackDirectory;
    }
    else if (!NT_SUCCESS(Status))
    {
        goto RollbackDirectory;
    }
    else
    {
        PNTFS_ATTR_RECORD Attribute;

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
        while (Attribute->Type != AttributeEnd)
        {
            if (Attribute->Type == AttributeFileName &&
                Attribute->Resident.ValueLength == NewFileNameLength &&
                RtlCompareMemory((PCHAR)Attribute + Attribute->Resident.ValueOffset,
                                 NewDirectoryEntry,
                                 NewFileNameLength) == NewFileNameLength)
            {
                Attribute->Resident.Flags = RA_INDEXED;
                break;
            }

            Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
        }
    }

    FileRecord->LinkCount++;
    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto RollbackDirectory;

    Fcb->LinkCount = FileRecord->LinkCount;
    Status = STATUS_SUCCESS;
    goto Cleanup;

RollbackDirectory:
    NtfsRemoveFilenameFromDirectory(DeviceExt,
                                    NewParentMftIndex,
                                    Fcb->MFTIndex,
                                    NewFileName,
                                    CaseSensitive);

Cleanup:
    if (NewDirectoryEntry)
        ExFreePoolWithTag(NewDirectoryEntry, TAG_NTFS);
    if (ParentFileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
    if (ExistingRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ExistingRecord);
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

NTSTATUS
AddFixupArray(PDEVICE_EXTENSION Vcb,
              PNTFS_RECORD_HEADER Record)
{
    USHORT *pShortToFixUp;
    ULONG ArrayEntryCount = Record->UsaCount - 1;
    ULONG Offset = Vcb->NtfsInfo.BytesPerSector - 2;
    ULONG i;

    PFIXUP_ARRAY fixupArray = (PFIXUP_ARRAY)((UCHAR*)Record + Record->UsaOffset);

    DPRINT("AddFixupArray(%p, %p)\n fixupArray->USN: %u, ArrayEntryCount: %u\n", Vcb, Record, fixupArray->USN, ArrayEntryCount);

    fixupArray->USN++;

    for (i = 0; i < ArrayEntryCount; i++)
    {
        DPRINT("USN: %u\tOffset: %u\n", fixupArray->USN, Offset);

        pShortToFixUp = (USHORT*)((PCHAR)Record + Offset);
        fixupArray->Array[i] = *pShortToFixUp;
        *pShortToFixUp = fixupArray->USN;
        Offset += Vcb->NtfsInfo.BytesPerSector;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
ReadLCN(PDEVICE_EXTENSION Vcb,
        ULONGLONG lcn,
        ULONG count,
        PVOID buffer)
{
    LARGE_INTEGER DiskSector;

    DiskSector.QuadPart = lcn;

    return NtfsReadSectors(Vcb->StorageDevice,
                           DiskSector.u.LowPart * Vcb->NtfsInfo.SectorsPerCluster,
                           count * Vcb->NtfsInfo.SectorsPerCluster,
                           Vcb->NtfsInfo.BytesPerSector,
                           buffer,
                           FALSE);
}


BOOLEAN
CompareFileName(PUNICODE_STRING FileName,
                PINDEX_ENTRY_ATTRIBUTE IndexEntry,
                BOOLEAN DirSearch,
                BOOLEAN CaseSensitive)
{
    BOOLEAN Ret, Alloc = FALSE;
    UNICODE_STRING EntryName;

    EntryName.Buffer = IndexEntry->FileName.Name;
    EntryName.Length =
    EntryName.MaximumLength = IndexEntry->FileName.NameLength * sizeof(WCHAR);

    if (DirSearch)
    {
        UNICODE_STRING IntFileName;
        if (!CaseSensitive)
        {
            NT_VERIFY(NT_SUCCESS(RtlUpcaseUnicodeString(&IntFileName, FileName, TRUE)));
            Alloc = TRUE;
        }
        else
        {
            IntFileName = *FileName;
        }

        Ret = FsRtlIsNameInExpression(&IntFileName, &EntryName, !CaseSensitive, NULL);

        if (Alloc)
        {
            RtlFreeUnicodeString(&IntFileName);
        }

        return Ret;
    }
    else
    {
        return (RtlCompareUnicodeString(FileName, &EntryName, !CaseSensitive) == 0);
    }
}

/**
* @name UpdateMftMirror
* @implemented
*
* Backs-up the first ~4 master file table entries to the $MFTMirr file.
*
* @param Vcb
* Pointer to an NTFS_VCB for the volume whose Mft mirror is being updated.
*
* @return

* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed.
* STATUS_UNSUCCESSFUL if we couldn't read the master file table.
*
* @remarks
* NTFS maintains up-to-date copies of the first several mft entries in the $MFTMirr file. Usually, the first 4 file
* records from the mft are stored. The exact number of entries is determined by the size of $MFTMirr's $DATA.
* If $MFTMirr is not up-to-date, chkdsk will reject every change it can find prior to when $MFTMirr was last updated.
* Therefore, it's recommended to call this function if the volume changes considerably. For instance, IncreaseMftSize()
* relies on this function to keep chkdsk from deleting the mft entries it creates. Note that under most instances, creating
* or deleting a file will not affect the first ~four mft entries, and so will not require updating the mft mirror.
*/
NTSTATUS
UpdateMftMirror(PNTFS_VCB Vcb)
{
    PFILE_RECORD_HEADER MirrorFileRecord;
    PNTFS_ATTR_CONTEXT MirrDataContext;
    PNTFS_ATTR_CONTEXT MftDataContext;
    PCHAR DataBuffer;
    ULONGLONG DataLength;
    NTSTATUS Status;
    ULONG BytesRead;
    ULONG LengthWritten;

    // Allocate memory for the Mft mirror file record
    MirrorFileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!MirrorFileRecord)
    {
        DPRINT1("Error: Failed to allocate memory for $MFTMirr!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Read the Mft Mirror file record
    Status = ReadFileRecord(Vcb, NTFS_FILE_MFTMIRR, MirrorFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to read $MFTMirr!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MirrorFileRecord);
        return Status;
    }

    // Find the $DATA attribute of $MFTMirr
    Status = FindAttribute(Vcb, MirrorFileRecord, AttributeData, L"", 0, &MirrDataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find $DATA attribute!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MirrorFileRecord);
        return Status;
    }

    // Find the $DATA attribute of $MFT
    Status = FindAttribute(Vcb, Vcb->MasterFileTable, AttributeData, L"", 0, &MftDataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find $DATA attribute!\n");
        ReleaseAttributeContext(MirrDataContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MirrorFileRecord);
        return Status;
    }

    // Get the size of the mirror's $DATA attribute
    DataLength = AttributeDataLength(MirrDataContext->pRecord);

    ASSERT(DataLength % Vcb->NtfsInfo.BytesPerFileRecord == 0);

    // Create buffer for the mirror's $DATA attribute
    DataBuffer = ExAllocatePoolWithTag(NonPagedPool, DataLength, TAG_NTFS);
    if (!DataBuffer)
    {
        DPRINT1("Error: Couldn't allocate memory for $DATA buffer!\n");
        ReleaseAttributeContext(MftDataContext);
        ReleaseAttributeContext(MirrDataContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MirrorFileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ASSERT(DataLength < ULONG_MAX);

    // Back up the first several entries of the Mft's $DATA Attribute
    BytesRead = ReadAttribute(Vcb, MftDataContext, 0, DataBuffer, (ULONG)DataLength);
    if (BytesRead != (ULONG)DataLength)
    {
        DPRINT1("Error: Failed to read $DATA for $MFTMirr!\n");
        ReleaseAttributeContext(MftDataContext);
        ReleaseAttributeContext(MirrDataContext);
        ExFreePoolWithTag(DataBuffer, TAG_NTFS);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MirrorFileRecord);
        return STATUS_UNSUCCESSFUL;
    }

    // Write the mirror's $DATA attribute
    Status = WriteAttribute(Vcb,
                             MirrDataContext,
                             0,
                             (PUCHAR)DataBuffer,
                             DataLength,
                             &LengthWritten,
                             MirrorFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to write $DATA attribute of $MFTMirr!\n");
    }

    // Cleanup
    ReleaseAttributeContext(MftDataContext);
    ReleaseAttributeContext(MirrDataContext);
    ExFreePoolWithTag(DataBuffer, TAG_NTFS);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MirrorFileRecord);

    return Status;
}

#if 0
static
VOID
DumpIndexEntry(PINDEX_ENTRY_ATTRIBUTE IndexEntry)
{
    DPRINT1("Entry: %p\n", IndexEntry);
    DPRINT1("\tData.Directory.IndexedFile: %I64x\n", IndexEntry->Data.Directory.IndexedFile);
    DPRINT1("\tLength: %u\n", IndexEntry->Length);
    DPRINT1("\tKeyLength: %u\n", IndexEntry->KeyLength);
    DPRINT1("\tFlags: %x\n", IndexEntry->Flags);
    DPRINT1("\tReserved: %x\n", IndexEntry->Reserved);
    DPRINT1("\t\tDirectoryFileReferenceNumber: %I64x\n", IndexEntry->FileName.DirectoryFileReferenceNumber);
    DPRINT1("\t\tCreationTime: %I64u\n", IndexEntry->FileName.CreationTime);
    DPRINT1("\t\tChangeTime: %I64u\n", IndexEntry->FileName.ChangeTime);
    DPRINT1("\t\tLastWriteTime: %I64u\n", IndexEntry->FileName.LastWriteTime);
    DPRINT1("\t\tLastAccessTime: %I64u\n", IndexEntry->FileName.LastAccessTime);
    DPRINT1("\t\tAllocatedSize: %I64u\n", IndexEntry->FileName.AllocatedSize);
    DPRINT1("\t\tDataSize: %I64u\n", IndexEntry->FileName.DataSize);
    DPRINT1("\t\tFileAttributes: %x\n", IndexEntry->FileName.FileAttributes);
    DPRINT1("\t\tNameLength: %u\n", IndexEntry->FileName.NameLength);
    DPRINT1("\t\tNameType: %x\n", IndexEntry->FileName.NameType);
    DPRINT1("\t\tName: %.*S\n", IndexEntry->FileName.NameLength, IndexEntry->FileName.Name);
}
#endif

/* Copy the matched index entry's $FILE_NAME key (fixed part + name) into the
 * caller's buffer, if one was provided.  The key carries the exact name the
 * lookup matched - for hard-linked files this can differ from any name found
 * on the target's file record, so directory enumeration must report it. */
static
VOID
NtfsCopyIndexEntryName(PINDEX_ENTRY_ATTRIBUTE IndexEntry,
                       PFILENAME_ATTRIBUTE OutFoundName)
{
    if (OutFoundName != NULL)
    {
        RtlCopyMemory(OutFoundName,
                      &IndexEntry->FileName,
                      FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) +
                          IndexEntry->FileName.NameLength * sizeof(WCHAR));
    }
}

/* Collation comparison used to NAVIGATE the $I30 B-tree during exact (non-
 * wildcard) lookups.  The on-disk order is always upcase-primary (see
 * NtfsCompareFilenameKey in btree.c); a case-sensitive lookup only refines
 * upcase-equal runs with the exact binary tiebreak so it lands on the exact-
 * cased entry among coexisting POSIX case-variants.  Descending with a plain
 * case-sensitive comparison instead (as this code used to) diverges from the
 * on-disk order at the first case difference and misses existing entries. */
static
LONG
NtfsCollateFileName(PUNICODE_STRING FileName,
                    PUNICODE_STRING EntryName,
                    BOOLEAN CaseSensitive)
{
    LONG Cmp = RtlCompareUnicodeString(FileName, EntryName, TRUE);

    if (Cmp == 0 && CaseSensitive)
        Cmp = RtlCompareUnicodeString(FileName, EntryName, FALSE);

    return Cmp;
}

NTSTATUS
BrowseSubNodeIndexEntries(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER MftRecord,
                          ULONG IndexBlockSize,
                          PUNICODE_STRING FileName,
                          PNTFS_ATTR_CONTEXT IndexAllocationContext,
                          PRTL_BITMAP Bitmap,
                          ULONGLONG VCN,
                          PULONG StartEntry,
                          PULONG CurrentEntry,
                          BOOLEAN DirSearch,
                          BOOLEAN CaseSensitive,
                          ULONGLONG *OutMFTIndex,
                          PFILENAME_ATTRIBUTE OutFoundName)
{
    PINDEX_BUFFER IndexRecord;
    ULONGLONG Offset;
    ULONG BytesRead;
    PINDEX_ENTRY_ATTRIBUTE FirstEntry;
    PINDEX_ENTRY_ATTRIBUTE LastEntry;
    PINDEX_ENTRY_ATTRIBUTE IndexEntry;
    ULONG NodeNumber;
    NTSTATUS Status;

    DPRINT("BrowseSubNodeIndexEntries: VCN=%I64d searching for '%wZ' (DirSearch=%d)\n",
            VCN, FileName, DirSearch);

    // A corrupt B-tree node can carry a sub-node VCN that lies outside the
    // $INDEX_ALLOCATION (e.g. with garbage high bits set).  NodeNumber below is
    // a ULONG, so such a VCN is silently truncated and can pass the $BITMAP
    // check - but the full 64-bit Offset (VCN * BytesPerCluster) is then turned
    // into a read far past the attribute, which translates to a wild disk
    // offset that never completes and hangs the volume.  Reject it up front.
    if (VCN >= AttributeDataLength(IndexAllocationContext->pRecord) / Vcb->NtfsInfo.BytesPerCluster)
    {
        DPRINT1("File system corruption detected, index node VCN %I64u is outside the $INDEX_ALLOCATION (%I64u bytes).\n",
                VCN, AttributeDataLength(IndexAllocationContext->pRecord));
        return STATUS_FILE_CORRUPT_ERROR;
    }

    // Calculate node number as VCN / Clusters per index record
    NodeNumber = VCN / (Vcb->NtfsInfo.BytesPerIndexRecord / Vcb->NtfsInfo.BytesPerCluster);

    // Is the bit for this node clear in the bitmap?
    if (!RtlCheckBit(Bitmap, NodeNumber))
    {
        DPRINT1("File system corruption detected, node with VCN %I64u is marked as deleted.\n", VCN);
        return STATUS_DATA_ERROR;
    }

    // Allocate memory for the index record
    IndexRecord = ExAllocatePoolWithTag(NonPagedPool, IndexBlockSize, TAG_NTFS);
    if (!IndexRecord)
    {
        DPRINT1("Unable to allocate memory for index record!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Calculate offset of index record
    Offset = VCN * Vcb->NtfsInfo.BytesPerCluster;

    // Read the index record
    BytesRead = ReadAttribute(Vcb, IndexAllocationContext, Offset, (PCHAR)IndexRecord, IndexBlockSize);
    if (BytesRead != IndexBlockSize)
    {
        DPRINT1("Unable to read index record! VCN=%I64u Offset=%I64u Expected=%lu Got=%I64u DataSize=%I64u\n",
                VCN, Offset, IndexBlockSize, BytesRead,
                IndexAllocationContext->pRecord->NonResident.DataSize);
        ExFreePoolWithTag(IndexRecord, TAG_NTFS);
        return STATUS_UNSUCCESSFUL;
    }

    // Assert that we're dealing with an index record here
    ASSERT(IndexRecord->Ntfs.Type == NRH_INDX_TYPE);

    // Apply the fixup array to the index record
    Status = FixupUpdateSequenceArray(Vcb, &((PFILE_RECORD_HEADER)IndexRecord)->Ntfs);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(IndexRecord, TAG_NTFS);
        DPRINT1("Failed to apply fixup array!\n");
        return Status;
    }

    ASSERT(IndexRecord->Header.AllocatedSize + FIELD_OFFSET(INDEX_BUFFER, Header) == IndexBlockSize);
    FirstEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&IndexRecord->Header + IndexRecord->Header.FirstEntryOffset);
    LastEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&IndexRecord->Header + IndexRecord->Header.TotalSizeOfEntries);
    ASSERT(LastEntry <= (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)IndexRecord + IndexBlockSize));

    // Loop through all Index Entries of index, starting with FirstEntry
    IndexEntry = FirstEntry;
    while (IndexEntry <= LastEntry)
    {
        // For exact lookups (non-wildcard), use B-tree key ordering to
        // navigate directly to the right sub-node instead of scanning all.
        // NTFS B-tree invariant: the sub-node VCN attached to entry[i]
        // contains keys that sort BEFORE entry[i].  The END entry's
        // sub-node contains keys that sort AFTER all real entries.
        if (!DirSearch && !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
        {
            UNICODE_STRING EntryName;
            LONG Cmp;

            EntryName.Buffer = IndexEntry->FileName.Name;
            EntryName.Length = EntryName.MaximumLength =
                IndexEntry->FileName.NameLength * sizeof(WCHAR);
            Cmp = NtfsCollateFileName(FileName, &EntryName, CaseSensitive);

            if (Cmp < 0)
            {
                // search_key < entry: the target must be in this entry's sub-node
                if ((IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE) &&
                    (IndexRecord->Header.Flags & INDEX_NODE_LARGE) &&
                    IndexAllocationContext)
                {
                    Status = BrowseSubNodeIndexEntries(Vcb, MftRecord, IndexBlockSize,
                                                       FileName, IndexAllocationContext,
                                                       Bitmap, GetIndexEntryVCN(IndexEntry),
                                                       StartEntry, CurrentEntry,
                                                       DirSearch, CaseSensitive, OutMFTIndex,
                                                       OutFoundName);
                    if (NT_SUCCESS(Status))
                    {
                        ExFreePoolWithTag(IndexRecord, TAG_NTFS);
                        return Status;
                    }
                }
                // Not in sub-node: the file doesn't exist in this branch
                ExFreePoolWithTag(IndexRecord, TAG_NTFS);
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            else if (Cmp == 0)
            {
                // Exact match on the key
                if ((IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE &&
                    *CurrentEntry >= *StartEntry &&
                    IndexEntry->FileName.NameType != NTFS_FILE_NAME_DOS)
                {
                    *StartEntry = *CurrentEntry;
                    *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
                    NtfsCopyIndexEntryName(IndexEntry, OutFoundName);
                    DPRINT("BrowseSubNode VCN=%I64d FOUND MFT=%I64u\n", VCN, *OutMFTIndex);
                    ExFreePoolWithTag(IndexRecord, TAG_NTFS);
                    return STATUS_SUCCESS;
                }
                // DOS-name or system entry match - skip, continue to next
            }
            // Cmp > 0: search_key > entry, advance to next entry
            (*CurrentEntry) += 1;
            ASSERT(IndexEntry->Length >= sizeof(INDEX_ENTRY_ATTRIBUTE));
            IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)IndexEntry + IndexEntry->Length);
            continue;
        }

        // DirSearch path (wildcard/enumeration) or END entry:
        // descend into every sub-node to enumerate all entries.
        if (IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE)
        {
            if (!(IndexRecord->Header.Flags & INDEX_NODE_LARGE) || !IndexAllocationContext)
            {
                DPRINT1("Filesystem corruption detected!\n");
            }
            else
            {
                Status = BrowseSubNodeIndexEntries(Vcb,
                                                   MftRecord,
                                                   IndexBlockSize,
                                                   FileName,
                                                   IndexAllocationContext,
                                                   Bitmap,
                                                   GetIndexEntryVCN(IndexEntry),
                                                   StartEntry,
                                                   CurrentEntry,
                                                   DirSearch,
                                                   CaseSensitive,
                                                   OutMFTIndex,
                                                   OutFoundName);
                if (NT_SUCCESS(Status))
                {
                    ExFreePoolWithTag(IndexRecord, TAG_NTFS);
                    return Status;
                }
            }
        }

        // Are we done?
        if (IndexEntry->Flags & NTFS_INDEX_ENTRY_END)
            break;

        // If we've found a file whose index is greater than or equal to StartEntry that matches the search criteria
        if ((IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE &&
            *CurrentEntry >= *StartEntry &&
            IndexEntry->FileName.NameType != NTFS_FILE_NAME_DOS &&
            CompareFileName(FileName, IndexEntry, DirSearch, CaseSensitive))
        {
            *StartEntry = *CurrentEntry;
            *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            NtfsCopyIndexEntryName(IndexEntry, OutFoundName);
            DPRINT("BrowseSubNode VCN=%I64d FOUND MFT=%I64u\n", VCN, *OutMFTIndex);
            ExFreePoolWithTag(IndexRecord, TAG_NTFS);
            return STATUS_SUCCESS;
        }

        // Advance to the next index entry
        (*CurrentEntry) += 1;
        ASSERT(IndexEntry->Length >= sizeof(INDEX_ENTRY_ATTRIBUTE));
        IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)IndexEntry + IndexEntry->Length);
    }

    DPRINT("BrowseSubNode VCN=%I64d NOT FOUND (scanned %lu entries)\n", VCN, *CurrentEntry);
    ExFreePoolWithTag(IndexRecord, TAG_NTFS);

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS
BrowseIndexEntries(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER MftRecord,
                   PINDEX_ROOT_ATTRIBUTE IndexRecord,
                   ULONG IndexBlockSize,
                   PINDEX_ENTRY_ATTRIBUTE FirstEntry,
                   PINDEX_ENTRY_ATTRIBUTE LastEntry,
                   PUNICODE_STRING FileName,
                   PULONG StartEntry,
                   PULONG CurrentEntry,
                   BOOLEAN DirSearch,
                   BOOLEAN CaseSensitive,
                   ULONGLONG *OutMFTIndex,
                   PFILENAME_ATTRIBUTE OutFoundName)
{
    NTSTATUS Status;
    PINDEX_ENTRY_ATTRIBUTE IndexEntry;
    PNTFS_ATTR_CONTEXT IndexAllocationContext;
    PNTFS_ATTR_CONTEXT BitmapContext;
    PCHAR *BitmapMem;
    ULONG *BitmapPtr;
    RTL_BITMAP  Bitmap;

    DPRINT("BrowseIndexEntries: searching for '%wZ' IndexBlockSize=%lu (DirSearch=%d)\n",
            FileName, IndexBlockSize, DirSearch);

    // Find the $I30 index allocation, if there is one
    Status = FindAttribute(Vcb, MftRecord, AttributeIndexAllocation, L"$I30", 4, &IndexAllocationContext, NULL);
    if (NT_SUCCESS(Status))
    {
        ULONGLONG BitmapLength;
        // Find the bitmap attribute for the index
        Status = FindAttribute(Vcb, MftRecord, AttributeBitmap, L"$I30", 4, &BitmapContext, NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Potential file system corruption detected!\n");
            ReleaseAttributeContext(IndexAllocationContext);
            return Status;
        }

        // Get the length of the bitmap attribute
        BitmapLength = AttributeDataLength(BitmapContext->pRecord);

        // Allocate memory for the bitmap, including some padding; RtlInitializeBitmap() wants a pointer
        // that's ULONG-aligned, and it wants the size of the memory allocated for it to be a ULONG-multiple.
        BitmapMem = ExAllocatePoolWithTag(NonPagedPool, BitmapLength + sizeof(ULONG), TAG_NTFS);
        if (!BitmapMem)
        {
            DPRINT1("Error: failed to allocate bitmap!");
            ReleaseAttributeContext(BitmapContext);
            ReleaseAttributeContext(IndexAllocationContext);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(BitmapMem, BitmapLength + sizeof(ULONG));

        // RtlInitializeBitmap() wants a pointer that's ULONG-aligned.
        BitmapPtr = (PULONG)ALIGN_UP_BY((ULONG_PTR)BitmapMem, sizeof(ULONG));

        // Read the existing bitmap data
        Status = ReadAttribute(Vcb, BitmapContext, 0, (PCHAR)BitmapPtr, BitmapLength);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to read bitmap attribute!\n");
            ExFreePoolWithTag(BitmapMem, TAG_NTFS);
            ReleaseAttributeContext(BitmapContext);
            ReleaseAttributeContext(IndexAllocationContext);
            return Status;
        }

        // Initialize bitmap
        RtlInitializeBitMap(&Bitmap, BitmapPtr, BitmapLength * 8);
    }
    else
    {
        // Couldn't find an index allocation
        IndexAllocationContext = NULL;
    }


    // Loop through all Index Entries of index, starting with FirstEntry
    IndexEntry = FirstEntry;
    while (IndexEntry <= LastEntry)
    {
        // For exact lookups (non-wildcard), use B-tree key ordering to
        // navigate directly instead of scanning all sub-nodes.
        if (!DirSearch && !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
        {
            UNICODE_STRING EntryName;
            LONG Cmp;

            EntryName.Buffer = IndexEntry->FileName.Name;
            EntryName.Length = EntryName.MaximumLength =
                IndexEntry->FileName.NameLength * sizeof(WCHAR);
            Cmp = NtfsCollateFileName(FileName, &EntryName, CaseSensitive);

            if (Cmp < 0)
            {
                // search_key < entry: target must be in this entry's sub-node
                if ((IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE) &&
                    (IndexRecord->Header.Flags & INDEX_ROOT_LARGE) &&
                    IndexAllocationContext)
                {
                    Status = BrowseSubNodeIndexEntries(Vcb, MftRecord, IndexBlockSize,
                                                       FileName, IndexAllocationContext,
                                                       &Bitmap, GetIndexEntryVCN(IndexEntry),
                                                       StartEntry, CurrentEntry,
                                                       DirSearch, CaseSensitive, OutMFTIndex,
                                                       OutFoundName);
                    if (NT_SUCCESS(Status))
                    {
                        ExFreePoolWithTag(BitmapMem, TAG_NTFS);
                        ReleaseAttributeContext(BitmapContext);
                        ReleaseAttributeContext(IndexAllocationContext);
                        return Status;
                    }
                }
                // Not in sub-node: file doesn't exist in this branch
                if (IndexAllocationContext)
                {
                    ExFreePoolWithTag(BitmapMem, TAG_NTFS);
                    ReleaseAttributeContext(BitmapContext);
                    ReleaseAttributeContext(IndexAllocationContext);
                }
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            else if (Cmp == 0)
            {
                // Exact match
                if ((IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE &&
                    *CurrentEntry >= *StartEntry &&
                    IndexEntry->FileName.NameType != NTFS_FILE_NAME_DOS)
                {
                    *StartEntry = *CurrentEntry;
                    *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
                    NtfsCopyIndexEntryName(IndexEntry, OutFoundName);
                    if (IndexAllocationContext)
                    {
                        ExFreePoolWithTag(BitmapMem, TAG_NTFS);
                        ReleaseAttributeContext(BitmapContext);
                        ReleaseAttributeContext(IndexAllocationContext);
                    }
                    return STATUS_SUCCESS;
                }
                // DOS-name or system entry match - skip, continue to next
            }
            // Cmp > 0: search_key > entry, advance to next entry
            (*CurrentEntry) += 1;
            ASSERT(IndexEntry->Length >= sizeof(INDEX_ENTRY_ATTRIBUTE));
            IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)IndexEntry + IndexEntry->Length);
            continue;
        }

        // DirSearch path (wildcard/enumeration) or END entry:
        // descend into every sub-node to enumerate all entries.
        if (IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE)
        {
            if (!(IndexRecord->Header.Flags & INDEX_ROOT_LARGE) || !IndexAllocationContext)
            {
                DPRINT1("Filesystem corruption detected!\n");
            }
            else
            {
                Status = BrowseSubNodeIndexEntries(Vcb,
                                                   MftRecord,
                                                   IndexBlockSize,
                                                   FileName,
                                                   IndexAllocationContext,
                                                   &Bitmap,
                                                   GetIndexEntryVCN(IndexEntry),
                                                   StartEntry,
                                                   CurrentEntry,
                                                   DirSearch,
                                                   CaseSensitive,
                                                   OutMFTIndex,
                                                   OutFoundName);
                if (NT_SUCCESS(Status))
                {
                    ExFreePoolWithTag(BitmapMem, TAG_NTFS);
                    ReleaseAttributeContext(BitmapContext);
                    ReleaseAttributeContext(IndexAllocationContext);
                    return Status;
                }
            }
        }

        // Are we done?
        if (IndexEntry->Flags & NTFS_INDEX_ENTRY_END)
            break;

        // If we've found a file whose index is greater than or equal to StartEntry that matches the search criteria
        if ((IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE &&
            *CurrentEntry >= *StartEntry &&
            IndexEntry->FileName.NameType != NTFS_FILE_NAME_DOS &&
            CompareFileName(FileName, IndexEntry, DirSearch, CaseSensitive))
        {
            *StartEntry = *CurrentEntry;
            *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            NtfsCopyIndexEntryName(IndexEntry, OutFoundName);
            if (IndexAllocationContext)
            {
                ExFreePoolWithTag(BitmapMem, TAG_NTFS);
                ReleaseAttributeContext(BitmapContext);
                ReleaseAttributeContext(IndexAllocationContext);
            }
            return STATUS_SUCCESS;
        }

        // Advance to the next index entry
        (*CurrentEntry) += 1;
        ASSERT(IndexEntry->Length >= sizeof(INDEX_ENTRY_ATTRIBUTE));
        IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)IndexEntry + IndexEntry->Length);
    }

    if (IndexAllocationContext)
    {
        ExFreePoolWithTag(BitmapMem, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        ReleaseAttributeContext(IndexAllocationContext);
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS
NtfsFindMftRecord(PDEVICE_EXTENSION Vcb,
                  ULONGLONG MFTIndex,
                  PUNICODE_STRING FileName,
                  PULONG FirstEntry,
                  BOOLEAN DirSearch,
                  BOOLEAN CaseSensitive,
                  ULONGLONG *OutMFTIndex,
                  PFILENAME_ATTRIBUTE OutFoundName)
{
    PFILE_RECORD_HEADER MftRecord;
    PNTFS_ATTR_CONTEXT IndexRootCtx;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PCHAR IndexRecord;
    PINDEX_ENTRY_ATTRIBUTE IndexEntry, IndexEntryEnd;
    NTSTATUS Status;
    ULONG CurrentEntry = 0;
    BOOLEAN IndexLockHeld = FALSE;

    NTFS_TRACE("NtfsFindMftRecord(%p, %I64d, %wZ, %lu, %s, %s, %p)\n",
               Vcb,
               MFTIndex,
               FileName,
               *FirstEntry,
               DirSearch ? "TRUE" : "FALSE",
               CaseSensitive ? "TRUE" : "FALSE",
               OutMFTIndex);
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: find mft record begin name=%wZ\n", FileName);

    /* Take IndexResource shared so a writer (UpdateFileNameRecord /
     * NtfsAddFilenameToDirectory / NtfsRemoveFilenameFromDirectory) can't
     * mutate any directory's $INDEX_ALLOCATION underneath us - see
     * Kreijstal/reactos#14. ERESOURCE allows recursive shared acquisition,
     * so it's safe even if a caller higher up the stack already holds it.
     * Normal kernel APCs are already disabled by FsRtlEnterFileSystem in
     * NtfsDispatch, so we don't need a critical region wrapper. */
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: acquire index shared\n");
    IndexLockHeld = ExAcquireResourceSharedLite(&Vcb->IndexResource, TRUE);
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: acquire index shared done held=%u\n", IndexLockHeld);

    MftRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (MftRecord == NULL)
    {
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, MFTIndex, MftRecord);
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: read dir record status=0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return Status;
    }

    ASSERT(MftRecord->Ntfs.Type == NRH_FILE_TYPE);
    Status = FindAttribute(Vcb, MftRecord, AttributeIndexRoot, L"$I30", 4, &IndexRootCtx, NULL);
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: find index root status=0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return Status;
    }

    IndexRecord = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerIndexRecord, TAG_NTFS);
    if (IndexRecord == NULL)
    {
        ReleaseAttributeContext(IndexRootCtx);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        if (IndexLockHeld)
            ExReleaseResourceLite(&Vcb->IndexResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ReadAttribute(Vcb, IndexRootCtx, 0, IndexRecord, Vcb->NtfsInfo.BytesPerIndexRecord);
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: read index root done\n");
    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)IndexRecord;
    IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
    /* Index root is always resident. */
    IndexEntryEnd = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);
    ReleaseAttributeContext(IndexRootCtx);

    DPRINT("IndexRecordSize: %x IndexBlockSize: %x\n", Vcb->NtfsInfo.BytesPerIndexRecord, IndexRoot->SizeOfEntry);

    Status = BrowseIndexEntries(Vcb,
                                MftRecord,
                                (PINDEX_ROOT_ATTRIBUTE)IndexRecord,
                                IndexRoot->SizeOfEntry,
                                IndexEntry,
                                IndexEntryEnd,
                                FileName,
                                FirstEntry,
                                &CurrentEntry,
                                DirSearch,
                                CaseSensitive,
                                OutMFTIndex,
                                OutFoundName);
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: browse index status=0x%lx out=%I64u\n",
                Status,
                OutMFTIndex ? *OutMFTIndex : 0);

    ExFreePoolWithTag(IndexRecord, TAG_NTFS);
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: freed index record\n");
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
    NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: freed mft record\n");
    if (IndexLockHeld)
    {
        NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: release index shared\n");
        ExReleaseResourceLite(&Vcb->IndexResource);
        NTFS_TRACE_IF(MFTIndex == 27, "DRVIDX: release index shared done\n");
    }

    return Status;
}

NTSTATUS
NtfsLookupFileAt(PDEVICE_EXTENSION Vcb,
                 PUNICODE_STRING PathName,
                 BOOLEAN CaseSensitive,
                 PFILE_RECORD_HEADER *FileRecord,
                 PULONGLONG MFTIndex,
                 ULONGLONG CurrentMFTIndex)
{
    UNICODE_STRING Current, Remaining;
    NTSTATUS Status;
    ULONG FirstEntry = 0;

    DPRINT("NtfsLookupFileAt(%p, %wZ, %s, %p, %p, %I64x)\n",
           Vcb,
           PathName,
           CaseSensitive ? "TRUE" : "FALSE",
           FileRecord,
           MFTIndex,
           CurrentMFTIndex);

    FsRtlDissectName(*PathName, &Current, &Remaining);

    while (Current.Length != 0)
    {
        DPRINT("Current: %wZ\n", &Current);

        /* FirstEntry is NtfsFindMftRecord's enumeration resume-cursor: a
         * match is only accepted at an index ordinal >= *FirstEntry, and the
         * matched ordinal is written back.  It must restart at 0 for every
         * path component (as AddFileName's walk does) - carrying the previous
         * component's ordinal forward silently misses any entry that sorts
         * earlier in the next directory: looking up "\a\b\c" failed with
         * STATUS_OBJECT_NAME_NOT_FOUND whenever "b"'s ordinal inside "a"
         * exceeded "c"'s ordinal inside "b", which made every hard-link
         * unlink in such a directory fail with 0xC0000034 because
         * NtfsDeleteFileRecord resolves the opened link's parent path through
         * this function. */
        FirstEntry = 0;
        Status = NtfsFindMftRecord(Vcb, CurrentMFTIndex, &Current, &FirstEntry, FALSE, CaseSensitive, &CurrentMFTIndex, NULL);
        NTFS_TRACE_IF(CurrentMFTIndex == 144, "DRVIDX: lookup find returned 0x%lx current=%wZ mft=%I64u remaining=%wZ\n",
                    Status,
                    &Current,
                    CurrentMFTIndex,
                    &Remaining);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        if (Remaining.Length == 0)
            break;

        FsRtlDissectName(Remaining, &Current, &Remaining);
    }

    *FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    NTFS_TRACE_IF(CurrentMFTIndex == 144, "DRVIDX: lookup allocated file record %p for mft=%I64u\n",
                *FileRecord,
                CurrentMFTIndex);
    if (*FileRecord == NULL)
    {
        DPRINT("NtfsLookupFileAt: Can't allocate MFT record\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NTFS_TRACE_IF(CurrentMFTIndex == 144, "DRVIDX: lookup read file record begin mft=%I64u\n", CurrentMFTIndex);
    Status = ReadFileRecord(Vcb, CurrentMFTIndex, *FileRecord);
    NTFS_TRACE_IF(CurrentMFTIndex == 144, "DRVIDX: lookup read file record returned 0x%lx mft=%I64u type=0x%lx flags=0x%x\n",
                Status,
                CurrentMFTIndex,
                (*FileRecord)->Ntfs.Type,
                (*FileRecord)->Flags);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("NtfsLookupFileAt: Can't read MFT record\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, *FileRecord);
        return Status;
    }

    *MFTIndex = CurrentMFTIndex;

    return STATUS_SUCCESS;
}

NTSTATUS
NtfsLookupFile(PDEVICE_EXTENSION Vcb,
               PUNICODE_STRING PathName,
               BOOLEAN CaseSensitive,
               PFILE_RECORD_HEADER *FileRecord,
               PULONGLONG MFTIndex)
{
    return NtfsLookupFileAt(Vcb, PathName, CaseSensitive, FileRecord, MFTIndex, NTFS_FILE_ROOT);
}

void
NtfsDumpData(ULONG_PTR Buffer, ULONG Length)
{
    ULONG i, j;

    if (!NTFS_TRACE_ENABLED)
        return;

    // dump binary data, 8 bytes at a time
    for (i = 0; i < Length; i += 8)
    {
        // display current offset, in hex
        DbgPrint("\t%03x\t", i);

        // display hex value of each of the next 8 bytes
        for (j = 0; j < 8; j++)
            DbgPrint("%02x ", *(PUCHAR)(Buffer + i + j));
        DbgPrint("\n");
    }
}

/**
* @name NtfsDumpFileRecord
* @implemented
*
* Provides diagnostic information about a file record. Prints a hex dump
* of the entire record (based on the size reported by FileRecord->ByesInUse),
* then prints a dump of each attribute.
*
* @param Vcb
* Pointer to a DEVICE_EXTENSION describing the volume.
*
* @param FileRecord
* Pointer to the file record to be analyzed.
*
* @remarks
* FileRecord must be a complete file record at least FileRecord->BytesAllocated
* in size, and not just the header.
*
*/
VOID
NtfsDumpFileRecord(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord)
{
    ULONG i, j;

    if (!NTFS_TRACE_ENABLED)
        return;

    // dump binary data, 8 bytes at a time
    for (i = 0; i < FileRecord->BytesInUse; i += 8)
    {
        // display current offset, in hex
        DbgPrint("\t%03x\t", i);

        // display hex value of each of the next 8 bytes
        for (j = 0; j < 8; j++)
            DbgPrint("%02x ", *(PUCHAR)((ULONG_PTR)FileRecord + i + j));
        DbgPrint("\n");
    }

    NtfsDumpFileAttributes(Vcb, FileRecord);
}

NTSTATUS
NtfsFindFileAt(PDEVICE_EXTENSION Vcb,
               PUNICODE_STRING SearchPattern,
               PULONG FirstEntry,
               PFILE_RECORD_HEADER *FileRecord,
               PULONGLONG MFTIndex,
               ULONGLONG CurrentMFTIndex,
               BOOLEAN CaseSensitive,
               PFILENAME_ATTRIBUTE OutFoundName)
{
    NTSTATUS Status;

    DPRINT("NtfsFindFileAt(%p, %wZ, %lu, %p, %p, %I64x, %s)\n",
           Vcb,
           SearchPattern,
           *FirstEntry,
           FileRecord,
           MFTIndex,
           CurrentMFTIndex,
           (CaseSensitive ? "TRUE" : "FALSE"));

    Status = NtfsFindMftRecord(Vcb, CurrentMFTIndex, SearchPattern, FirstEntry, TRUE, CaseSensitive, &CurrentMFTIndex, OutFoundName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("NtfsFindFileAt: NtfsFindMftRecord() failed with status 0x%08lx\n", Status);
        return Status;
    }

    *FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (*FileRecord == NULL)
    {
        DPRINT("NtfsFindFileAt: Can't allocate MFT record\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, CurrentMFTIndex, *FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("NtfsFindFileAt: Can't read MFT record\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, *FileRecord);
        return Status;
    }

    *MFTIndex = CurrentMFTIndex;

    return STATUS_SUCCESS;
}

/* EOF */
