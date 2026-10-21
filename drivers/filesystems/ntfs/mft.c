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

#include "lfswrite.h"
#include <ntintsafe.h>

#define NDEBUG
#include <debug.h>

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

    Context->DirectIo = FALSE;

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
        BOOLEAN IsSparse;
        PUCHAR DataRun = (PUCHAR)((ULONG_PTR)Context->pRecord + Context->pRecord->NonResident.MappingPairsOffset);

        // Convert the data runs to a map control block. This also checks
        // that they lie within the attribute, before anything decodes them.
        if (!NT_SUCCESS(ConvertDataRunsToLargeMCB(DataRun,
                                                  (PUCHAR)Context->pRecord + Context->pRecord->Length,
                                                  &Context->DataRunsMCB,
                                                  &NextVBN)))
        {
            DPRINT1("Unable to convert data runs to MCB!\n");
            ExFreePoolWithTag(Context->pRecord, TAG_NTFS);
            ExFreeToNPagedLookasideList(&NtfsGlobalData->AttrCtxtLookasideList, Context);
            return NULL;
        }

        Context->CacheRun = DataRun;
        Context->CacheRunOffset = 0;
        Context->CacheRun = DecodeRun(Context->CacheRun, &DataRunOffset, &DataRunLength, &IsSparse);
        Context->CacheRunLength = DataRunLength;
        if (!IsSparse)
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
    }

    return Context;
}


VOID
ReleaseAttributeContext(PNTFS_ATTR_CONTEXT Context)
{
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


/**
* @name NtfsAttributeNamesEqual
*
* Attribute names, stream names among them, compare without regard to case, the way
* NTFS folds it.
*/
BOOLEAN
NtfsAttributeNamesEqual(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PCWSTR Name1,
    _In_ PCWSTR Name2,
    _In_ ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++)
    {
        if (NtfsUpcaseChar(Vcb, Name1[i]) != NtfsUpcaseChar(Vcb, Name2[i]))
            return FALSE;
    }

    return TRUE;
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
                if (NtfsAttributeNamesEqual(Vcb, AttrName, Name, NameLength))
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
                if (NtfsAttributeNamesEqual(Vcb, AttrName, Name, NameLength))
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
    ULONG NewRecords = ATTR_RECORD_ALIGNMENT * 8;  // Allocate one new record for every bit of every byte we'll be adding to the bitmap
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
    if (!ExAcquireResourceExclusiveLite(&(Vcb->MftAllocationResource), CanWait))
    {
        return STATUS_CANT_WAIT;
    }

    // Create a blank file record that will be used later
    BlankFileRecord = NtfsCreateEmptyFileRecord(Vcb);
    if (!BlankFileRecord)
    {
        DPRINT1("Error: Unable to create empty file record!\n");
        ExReleaseResourceLite(&Vcb->MftAllocationResource);
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
        ExReleaseResourceLite(&(Vcb->MftAllocationResource));
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
        ExReleaseResourceLite(&(Vcb->MftAllocationResource));
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
        ExReleaseResourceLite(&(Vcb->MftAllocationResource));
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
        ExReleaseResourceLite(&(Vcb->MftAllocationResource));
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
        ExReleaseResourceLite(&(Vcb->MftAllocationResource));
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
            ExReleaseResourceLite(&(Vcb->MftAllocationResource));
            ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
            ReleaseAttributeContext(BitmapContext);
            return Status;
        }
    }

    NtfsDumpFileAttributes(Vcb, Vcb->MasterFileTable);

    // Update the file record with the new attribute sizes
    Status = UpdateFileRecord(Vcb, Vcb->VolumeFcb->MFTIndex, Vcb->MasterFileTable);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update $MFT file record!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->MftAllocationResource));
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return Status;
    }

    // Write out the new bitmap
    Status = WriteAttribute(Vcb, BitmapContext, 0, BitmapBuffer, NewBitmapSize, &LengthWritten, Vcb->MasterFileTable);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
        ExReleaseResourceLite(&(Vcb->MftAllocationResource));
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
            ExReleaseResourceLite(&(Vcb->MftAllocationResource));
            ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
            ReleaseAttributeContext(BitmapContext);
            return Status;
        }
    }

    // Update the mft mirror
    Status = UpdateMftMirror(Vcb);

    // Cleanup
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BlankFileRecord);
    ExReleaseResourceLite(&(Vcb->MftAllocationResource));
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
    ULONG TrailingSize;

    ASSERT(!AttrContext->pRecord->IsNonResident);
    ASSERT(FileRecord->BytesInUse >= AttrOffset + OldAttributeLength);

    /* Preflight the complete record before changing either copy. */
    if (DataSize > DeviceExt->NtfsInfo.BytesPerFileRecord)
        return STATUS_BUFFER_OVERFLOW;
    NewAttributeLength = ALIGN_UP_BY(DataSize + Destination->Resident.ValueOffset,
                                     ATTR_RECORD_ALIGNMENT);
    NextAttributeOffset = AttrOffset + NewAttributeLength;
    TrailingSize = FileRecord->BytesInUse - AttrOffset - OldAttributeLength;
    if (NextAttributeOffset + TrailingSize > DeviceExt->NtfsInfo.BytesPerFileRecord)
        return STATUS_BUFFER_OVERFLOW;

    if (NewAttributeLength > OldAttributeLength)
    {
        NewContextRecord = ExAllocatePoolWithTag(NonPagedPool, NewAttributeLength, TAG_NTFS);
        if (NewContextRecord == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    Destination->Resident.ValueLength = DataSize;
    Destination->Length = NewAttributeLength;

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

    // A value that grew reads as zeros past its old end
    if (DataSize > OldValueLength)
    {
        RtlZeroMemory((PUCHAR)Destination + Destination->Resident.ValueOffset + OldValueLength,
                      DataSize - OldValueLength);
    }

    if (NewContextRecord != NULL)
    {
        ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);
        AttrContext->pRecord = NewContextRecord;
    }
    RtlCopyMemory(AttrContext->pRecord, Destination, Destination->Length);

    // set the file record end
    SetFileRecordEnd(FileRecord, FinalAttribute, FILE_RECORD_END);

    //NtfsDumpFileRecord(DeviceExt, FileRecord);

    return STATUS_SUCCESS;
}

/**
*   @parameter FileRecord
*   Pointer to a file record. Must be a full record at least
*   Fcb->Vcb->NtfsInfo.BytesPerFileRecord bytes large, not just the header.
*/
/* A run-list resize can move its attribute while this operation still owns
 * the original record buffer. Commit the current owner, not the old slot. */
NTSTATUS
NtfsCommitAttributeContext(PDEVICE_EXTENSION Vcb,
                           PNTFS_ATTR_CONTEXT Context,
                           PFILE_RECORD_HEADER Record)
{
    PFILE_RECORD_HEADER Holder;
    PNTFS_ATTR_RECORD Attribute;
    ULONG Offset;
    NTSTATUS Status;

    Status = NtfsResolveAttributeHolder(Vcb, Context, Record, &Holder, &Offset);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Holder != Record)
    {
        Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Holder + Offset);
        ASSERT(Attribute->Length == Context->pRecord->Length);
        RtlCopyMemory(Attribute, Context->pRecord, Attribute->Length);
    }
    Status = UpdateFileRecord(Vcb, Context->FileMFTIndex & NTFS_MFT_MASK, Holder);
    NtfsReleaseAttributeHolder(Vcb, Record, Holder);
    return Status;
}

NTSTATUS
SetAttributeDataLength(PFILE_OBJECT FileObject,
                       PNTFS_FCB Fcb,
                       PNTFS_ATTR_CONTEXT AttrContext,
                       ULONG AttrOffset,
                       PFILE_RECORD_HEADER FileRecord,
                       PLARGE_INTEGER DataSize)
{
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("SetAttributeDataLength(%p, %p, %p, %lu, %p, %I64u)\n",
            FileObject,
            Fcb,
            AttrContext,
            AttrOffset,
            FileRecord,
            DataSize->QuadPart);

    // are we truncating the file?
    if (DataSize->QuadPart < AttributeDataLength(AttrContext->pRecord))
    {
        if (!MmCanFileBeTruncated(FileObject->SectionObjectPointer, DataSize))
        {
            DPRINT1("Can't truncate a memory-mapped file!\n");
            return STATUS_USER_MAPPED_FILE;
        }
    }

    if (AttrContext->pRecord->IsNonResident)
    {
        Status = SetNonResidentAttributeDataLength(Fcb->Vcb,
                                                   AttrContext,
                                                   AttrOffset,
                                                   FileRecord,
                                                   DataSize);
    }
    else
    {
        // resident attribute
        Status = SetResidentAttributeDataLength(Fcb->Vcb,
                                                AttrContext,
                                                AttrOffset,
                                                FileRecord,
                                                DataSize);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to set size of attribute!\n");
        return Status;
    }

    //NtfsDumpFileAttributes(Fcb->Vcb, FileRecord);

    // write the updated file record back to disk: the record that holds the attribute
    Status = NtfsCommitAttributeContext(Fcb->Vcb, AttrContext, FileRecord);

    if (NT_SUCCESS(Status))
    {
        if (AttrContext->pRecord->IsNonResident)
            Fcb->RFCB.AllocationSize.QuadPart = AttrContext->pRecord->NonResident.AllocatedSize;
        else
            Fcb->RFCB.AllocationSize = *DataSize;
        Fcb->RFCB.FileSize = *DataSize;
        if (Fcb->RFCB.ValidDataLength.QuadPart > DataSize->QuadPart)
            Fcb->RFCB.ValidDataLength = *DataSize;
        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
    }

    return Status;
}

/**
* @name NtfsZeroAttributeRange
* @implemented
*
* Makes [Start, End) of an attribute's value read as zeros: the range a file grows by,
* which would otherwise show whatever its clusters held before.
*
* @param Vcb
* Pointer to a DEVICE_EXTENSION describing the target disk.
*
* @param AttrContext
* Context of the attribute, already sized to at least End.
*
* @param FileRecord
* The complete file record holding the attribute.
*
* @return
* STATUS_SUCCESS, or the status of an allocation or write that failed.
*
* @remarks
* A resident value is zeroed by InternalSetResidentAttributeLength() as it grows. A sparse
* stream's holes read as zeros already; its allocated runs are written over like any other.
*/
NTSTATUS
NtfsZeroAttributeRange(PDEVICE_EXTENSION Vcb,
                       PNTFS_ATTR_CONTEXT AttrContext,
                       PFILE_RECORD_HEADER FileRecord,
                       ULONGLONG Start,
                       ULONGLONG End)
{
    const ULONG ChunkSize = 64 * 1024;
    PUCHAR Zeros;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(End <= AttributeDataLength(AttrContext->pRecord));

    /* A compressed stream is zeroed unit by unit, NtfsZeroStreamRange() */
    ASSERT(!NtfsAttributeIsCompressed(AttrContext));

    if (Start >= End || !AttrContext->pRecord->IsNonResident)
        return STATUS_SUCCESS;

    Zeros = ExAllocatePoolWithTag(NonPagedPool, ChunkSize, TAG_NTFS);
    if (Zeros == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Zeros, ChunkSize);

    while (Start < End && NT_SUCCESS(Status))
    {
        ULONG Length = (ULONG)min(End - Start, ChunkSize);
        ULONG Written;

        /* The holes of a sparse stream read as zeros already */
        if (NtfsAttributeIsSparse(AttrContext))
        {
            ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
            LONGLONG Lbn, Count;
            ULONGLONG RunEnd;

            if (!FsRtlLookupLargeMcbEntry(&AttrContext->DataRunsMCB, Start / BytesPerCluster, &Lbn, &Count, NULL, NULL, NULL))
                break;

            RunEnd = (Start / BytesPerCluster + Count) * BytesPerCluster;
            if (Lbn == -1)
            {
                Start = min(RunEnd, End);
                continue;
            }
            Length = (ULONG)min(Length, RunEnd - Start);
        }

        Status = WriteAttribute(Vcb, AttrContext, Start, Zeros, Length, &Written, FileRecord);
        if (NT_SUCCESS(Status) && Written != Length)
            Status = STATUS_UNEXPECTED_IO_ERROR;

        Start += Length;
    }

    ExFreePoolWithTag(Zeros, TAG_NTFS);

    return Status;
}

/**
* @name NtfsSetAllocationSize
* @implemented
*
* FileAllocationInformation: gives a stream at least AllocationSize bytes of clusters, or
* gives back what it has beyond that. NTFS rounds the request to clusters before
* deciding whether EOF must move down (MS-FSA 2.1.5.15.1, footnote 178).
*
* @param FileObject
* File object the request came on.
*
* @param Fcb
* FCB of the stream.
*
* @param AttrContext
* Context of the stream's $DATA attribute.
*
* @param AttrOffset
* Offset of the attribute in FileRecord.
*
* @param FileRecord
* The complete file record. Written back to disk.
*
* @param AllocationSize
* The allocation asked for.
*/
NTSTATUS
NtfsSetAllocationSize(PFILE_OBJECT FileObject,
                      PNTFS_FCB Fcb,
                      PNTFS_ATTR_CONTEXT AttrContext,
                      ULONG AttrOffset,
                      PFILE_RECORD_HEADER FileRecord,
                      ULONGLONG AllocationSize)
{
    PDEVICE_EXTENSION Vcb = Fcb->Vcb;
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONGLONG DataSize = AttributeDataLength(AttrContext->pRecord);
    ULONGLONG Wanted;
    NTSTATUS Status = STATUS_SUCCESS;

    Wanted = ROUND_UP(AllocationSize, BytesPerCluster);
    if (Wanted < DataSize)
    {
        LARGE_INTEGER NewSize;

        NewSize.QuadPart = Wanted;
        return SetAttributeDataLength(FileObject, Fcb, AttrContext, AttrOffset, FileRecord, &NewSize);
    }

    if (!AttrContext->pRecord->IsNonResident)
    {
        LARGE_INTEGER CurrentSize;

        /* A resident value has no allocation of its own to grow */
        if (AllocationSize <= DataSize)
            return STATUS_SUCCESS;

        CurrentSize.QuadPart = DataSize;
        Status = ConvertResidentAttributeToNonResident(Vcb, AttrContext, AttrOffset, FileRecord, &CurrentSize);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Compressed and sparse streams count their allocation differently; leave them be */
    if (AttrContext->pRecord->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE))
        return STATUS_SUCCESS;

    if (Wanted > AttrContext->pRecord->NonResident.AllocatedSize)
    {
        Status = NtfsExtendAllocation(Vcb, AttrContext, AttrOffset, FileRecord, Wanted);
    }
    else if (Wanted < AttrContext->pRecord->NonResident.AllocatedSize)
    {
        Status = FreeClusters(Vcb,
                              AttrContext,
                              AttrOffset,
                              FileRecord,
                              (ULONG)((AttrContext->pRecord->NonResident.AllocatedSize - Wanted) / BytesPerCluster));
        if (NT_SUCCESS(Status))
            AttrContext->pRecord->NonResident.AllocatedSize = Wanted;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsCommitAttributeContext(Vcb, AttrContext, FileRecord);
    if (NT_SUCCESS(Status))
    {
        Fcb->RFCB.AllocationSize.QuadPart = AttrContext->pRecord->NonResident.AllocatedSize;
        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
    }

    return Status;
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
* @name NtfsExtendAllocation
* @implemented
*
* Assigns clusters to a non-resident attribute until its allocation reaches AllocationSize.
* Data size is left alone, so this also reserves room ahead of a growing file.
*
* @param Vcb
* Pointer to a DEVICE_EXTENSION describing the target disk.
*
* @param AttrContext
* PNTFS_ATTR_CONTEXT describing the attribute to extend.
*
* @param AttrOffset
* Offset, from the beginning of the record, of the attribute.
*
* @param FileRecord
* Pointer to a complete copy of the file record containing the attribute.
*
* @param AllocationSize
* New allocation in bytes. Must be a multiple of the cluster size.
*
* @return
* STATUS_SUCCESS on success, or the error from NtfsAllocateClusters() or AddRun().
*
* @remarks
* AddRun() commits the mapping pairs and their allocation size together for each
* successfully assigned run. The caller commits any change to the logical size.
*/
NTSTATUS
NtfsExtendAllocation(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_ATTR_CONTEXT AttrContext,
    _In_ ULONG AttrOffset,
    _In_ PFILE_RECORD_HEADER FileRecord,
    _In_ ULONGLONG AllocationSize)
{
    NTSTATUS Status;
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONGLONG ExistingClusters = AttrContext->pRecord->NonResident.AllocatedSize / BytesPerCluster;
    ULONGLONG TotalNeeded;
    ULONG ClustersNeeded;
    ULONG DesiredCluster = 0;
    LONGLONG LastVbn;
    LONGLONG LastLbn;
    ULONG NextAssignedCluster;
    ULONG AssignedClusters;

    ASSERT(AttrContext->pRecord->IsNonResident);
    ASSERT(AllocationSize % BytesPerCluster == 0);

    if (AttrContext->pRecord->NonResident.AllocatedSize >= AllocationSize)
        return STATUS_SUCCESS;

    /* The cluster bitmap is indexed by ULONG; no volume has more free clusters than that */
    TotalNeeded = AllocationSize / BytesPerCluster - ExistingClusters;
    if (TotalNeeded > MAXULONG)
        return STATUS_DISK_FULL;

    ClustersNeeded = (ULONG)TotalNeeded;

    /* Ask for the clusters right after the last one the stream owns. The
     * tail of a sparse stream is a hole, so look at the last mapping, not
     * at HighestVCN. */
    if (ExistingClusters != 0 &&
        FsRtlLookupLastLargeMcbEntry(&AttrContext->DataRunsMCB, &LastVbn, &LastLbn) &&
        LastLbn != -1)
    {
        DesiredCluster = (ULONG)LastLbn + 1;
    }

    DPRINT("DesiredCluster: %lu\n", DesiredCluster);
    DPRINT("Highest VCN of record: %I64u\n", AttrContext->pRecord->NonResident.HighestVCN);

    while (ClustersNeeded > 0)
    {
        ULONGLONG BeforeRun;

        Status = NtfsAllocateClusters(Vcb,
                                      DesiredCluster,
                                      ClustersNeeded,
                                      &NextAssignedCluster,
                                      &AssignedClusters);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Error: Unable to allocate requested clusters!\n");
            return Status;
        }

        // now we need to add the clusters we allocated to the data run
        BeforeRun = AttrContext->pRecord->NonResident.AllocatedSize;
        Status = AddRun(Vcb, AttrContext, AttrOffset, FileRecord, NextAssignedCluster, AssignedClusters);
        if (!NT_SUCCESS(Status))
        {
            NTSTATUS RollbackStatus;

            DPRINT1("Error: Unable to add data run!\n");
            if (AttrContext->pRecord->NonResident.AllocatedSize == BeforeRun)
            {
                RollbackStatus = NtfsFreeClusterRange(Vcb, NextAssignedCluster, AssignedClusters);
                if (!NT_SUCCESS(RollbackStatus))
                    DPRINT1("Couldn't release unlinked allocation (Status %lx)\n", RollbackStatus);
            }
            return Status;
        }

        /* AddRun persisted the allocation size together with its mappings. */

        ClustersNeeded -= AssignedClusters;
        DesiredCluster = NextAssignedCluster + AssignedClusters;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsTrimAllocation
* @implemented
*
* Gives back clusters a growing file reserved past its end. Called when the last
* handle goes away.
*
* @param Fcb
* FCB of the file. The caller holds its MainResource exclusively.
*
* @return
* STATUS_SUCCESS on success, including when there's nothing to trim.
*/
NTSTATUS
NtfsTrimAllocation(
    _In_ PNTFS_FCB Fcb)
{
    PDEVICE_EXTENSION Vcb = Fcb->Vcb;
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    PNTFS_ATTR_RECORD DestinationAttribute;
    ULONG AttributeOffset;
    ULONGLONG NeededSize = 0;
    NTSTATUS Status;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Paging writes look the runs up again; keep them out while the tail goes */
    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = FindAttribute(Vcb, FileRecord, AttributeData, Fcb->Stream, wcslen(Fcb->Stream), &DataContext, &AttributeOffset);
    if (!NT_SUCCESS(Status))
        goto Quit;

    /* Only the record holding the stream changes; read it if that isn't the base */
    if ((DataContext->FileMFTIndex & NTFS_MFT_MASK) != Fcb->MFTIndex)
    {
        Status = ReadFileRecord(Vcb, DataContext->FileMFTIndex & NTFS_MFT_MASK, FileRecord);
        if (!NT_SUCCESS(Status))
        {
            ReleaseAttributeContext(DataContext);
            goto Quit;
        }
    }

    if (DataContext->pRecord->IsNonResident)
        NeededSize = ROUND_UP(DataContext->pRecord->NonResident.DataSize, BytesPerCluster);

    /* Compressed and sparse streams round their allocation differently; leave them be */
    if (DataContext->pRecord->IsNonResident &&
        !(DataContext->pRecord->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE)) &&
        DataContext->pRecord->NonResident.AllocatedSize > NeededSize)
    {
        Status = FreeClusters(Vcb,
                              DataContext,
                              AttributeOffset,
                              FileRecord,
                              (ULONG)((DataContext->pRecord->NonResident.AllocatedSize - NeededSize) / BytesPerCluster));
        if (NT_SUCCESS(Status))
        {
            DestinationAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttributeOffset);
            DestinationAttribute->NonResident.AllocatedSize = NeededSize;

            Status = UpdateFileRecord(Vcb, DataContext->FileMFTIndex & NTFS_MFT_MASK, FileRecord);
            if (NT_SUCCESS(Status))
                Fcb->RFCB.AllocationSize.QuadPart = NeededSize;
        }
    }

    ReleaseAttributeContext(DataContext);

Quit:
    ExReleaseResourceLite(&Fcb->PagingIoResource);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsResizeSparseAllocation
*
* Sizes the allocation of a sparse attribute for DataSize bytes, in whole units: growth is a
* hole, and the units a shrink leaves past the end give their clusters back.
*/
static
NTSTATUS
NtfsResizeSparseAllocation(PDEVICE_EXTENSION Vcb,
                           PNTFS_ATTR_CONTEXT AttrContext,
                           ULONG AttrOffset,
                           PFILE_RECORD_HEADER FileRecord,
                           ULONGLONG DataSize)
{
    ULONG UnitBytes = NtfsSparseUnitClusters(AttrContext) * Vcb->NtfsInfo.BytesPerCluster;
    ULONGLONG NewVcns = ROUND_UP(DataSize, UnitBytes) / Vcb->NtfsInfo.BytesPerCluster;
    ULONGLONG OldVcns = AttrContext->pRecord->NonResident.AllocatedSize / Vcb->NtfsInfo.BytesPerCluster;
    NTSTATUS Status;

    if (NewVcns == OldVcns)
        return STATUS_SUCCESS;

    if (NewVcns < OldVcns)
    {
        Status = NtfsDeallocateSparseRange(Vcb, AttrContext, AttrOffset, FileRecord, NewVcns, OldVcns - NewVcns);
        if (!NT_SUCCESS(Status))
            return Status;
        FsRtlTruncateLargeMcb(&AttrContext->DataRunsMCB, NewVcns);
    }

    return NtfsStoreMappingPairs(Vcb, AttrContext, AttrOffset, FileRecord, NewVcns);
}

/**
* @name SetNonResidentAttributeDataLength
* @implemented
*
* Called by SetAttributeDataLength() and IncreaseMftSize() to set the size of a non-resident attribute. Doesn't update
* the file record.
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
* Growing never gives back clusters reserved by NtfsExtendAllocation(); only a truncate does.
*/
NTSTATUS
SetNonResidentAttributeDataLength(PDEVICE_EXTENSION Vcb,
                                  PNTFS_ATTR_CONTEXT AttrContext,
                                  ULONG AttrOffset,
                                  PFILE_RECORD_HEADER FileRecord,
                                  PLARGE_INTEGER DataSize)
{
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN MasterContext = (AttrContext == Vcb->MFTContext);
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONGLONG AllocationSize = ROUND_UP(DataSize->QuadPart, BytesPerCluster);
    PNTFS_ATTR_RECORD DestinationAttribute;
    ULONG ExistingClusters = AttrContext->pRecord->NonResident.AllocatedSize / BytesPerCluster;

    ASSERT(AttrContext->pRecord->IsNonResident);

    /* A sparse or compressed stream grows by a hole, and shrinks by giving back its units
     * past the end */
    if (NtfsAttributeHasUnits(AttrContext))
    {
        Status = NtfsResizeSparseAllocation(Vcb, AttrContext, AttrOffset, FileRecord, DataSize->QuadPart);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    // do we need to increase the allocation size?
    else if (AttrContext->pRecord->NonResident.AllocatedSize < AllocationSize)
    {
        ULONGLONG ReserveSize = AllocationSize;

        if (AttrContext->pRecord->Type == AttributeIndexAllocation)
        {
            ULONGLONG Ahead = min(max(AllocationSize, NTFS_GROW_AHEAD_MIN),
                                  NTFS_GROW_AHEAD_MAX);
            ReserveSize = ROUND_UP(AllocationSize + Ahead, BytesPerCluster);
        }
        Status = NtfsExtendAllocation(Vcb, AttrContext, AttrOffset, FileRecord, ReserveSize);
        if (!NT_SUCCESS(Status) && ReserveSize != AllocationSize)
            Status = NtfsExtendAllocation(Vcb, AttrContext, AttrOffset, FileRecord, AllocationSize);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else if (AttrContext->pRecord->NonResident.AllocatedSize > AllocationSize &&
             (ULONGLONG)DataSize->QuadPart < AttrContext->pRecord->NonResident.DataSize)
    {
        // shrink allocation size
        ULONG ClustersToFree = ExistingClusters - (AllocationSize / BytesPerCluster);
        ASSERT(ExistingClusters == AttrContext->pRecord->NonResident.HighestVCN + 1);
        Status = FreeClusters(Vcb, AttrContext, AttrOffset, FileRecord, ClustersToFree);
        if (!NT_SUCCESS(Status))
            return Status;
        AttrContext->pRecord->NonResident.AllocatedSize = AllocationSize;
    }

    // TODO: is the file compressed, encrypted, or sparse?

    if (MasterContext)
    {
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Vcb->MftContextResource, TRUE);
    }
    AttrContext->pRecord->NonResident.DataSize = DataSize->QuadPart;
    AttrContext->pRecord->NonResident.InitializedSize = DataSize->QuadPart;

    if ((AttrContext->FileMFTIndex & NTFS_MFT_MASK) != FileRecord->MFTRecordNumber)
    {
        Status = NtfsCommitAttributeContext(Vcb, AttrContext, FileRecord);
        goto UnlockContext;
    }

    DestinationAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)FileRecord + AttrOffset);
    DestinationAttribute->NonResident.AllocatedSize = AttrContext->pRecord->NonResident.AllocatedSize;
    DestinationAttribute->NonResident.DataSize = DataSize->QuadPart;
    DestinationAttribute->NonResident.InitializedSize = DataSize->QuadPart;

    /* AddRun() and FreeClusters() keep HighestVCN on the last VCN the runs
     * describe, holes included, so it's carried over rather than recomputed */
    DestinationAttribute->NonResident.HighestVCN = AttrContext->pRecord->NonResident.HighestVCN;

    DPRINT("Allocated Size: %I64u\n", DestinationAttribute->NonResident.AllocatedSize);

UnlockContext:
    if (MasterContext)
    {
        ExReleaseResourceLite(&Vcb->MftContextResource);
        KeLeaveCriticalRegion();
    }
    return Status;
}

/**
* @name ConvertResidentAttributeToNonResident
* @implemented
*
* Moves the value of a resident attribute into clusters of its own and sizes it to DataSize.
*
* @param Vcb
* Pointer to a DEVICE_EXTENSION describing the target disk.
*
* @param AttrContext
* Context of the resident attribute. Describes the non-resident attribute on return.
*
* @param AttrOffset
* Offset of the attribute in FileRecord.
*
* @param FileRecord
* The complete file record holding the attribute. Written back to disk.
*
* @param DataSize
* The size the attribute's value is to have, at least its current one.
*
* @return
* STATUS_SUCCESS, STATUS_DISK_FULL if the record can't hold the non-resident header, or an
* allocation or I/O error.
*
* @remarks
* The attribute need not be the last in the record: a named stream sorts wherever its name
* falls, and an unnamed $DATA has the named ones behind it. Whatever follows it is moved to
* follow the new, non-resident header.
*/
static NTSTATUS
NtfsMakeRoomToConvert(PDEVICE_EXTENSION Vcb,
                      PNTFS_ATTR_CONTEXT AttrContext,
                      PULONG AttrOffset,
                      PFILE_RECORD_HEADER FileRecord,
                      ULONG NewRecordLength);

NTSTATUS
ConvertResidentAttributeToNonResident(PDEVICE_EXTENSION Vcb,
                                      PNTFS_ATTR_CONTEXT AttrContext,
                                      ULONG AttrOffset,
                                      PFILE_RECORD_HEADER FileRecord,
                                      PLARGE_INTEGER DataSize)
{
    PNTFS_ATTR_RECORD Destination, SavedRecord, NewRecord = NULL;
    PFILE_RECORD_HEADER WriteImage = NULL;
    PVOID Data = NULL;
    ULONG ValueLength, NewLength, OldLength, TailLength;
    ULONG FirstCluster, AssignedClusters, Written, RunBytes, Clusters, Vcn = 0;
    LONGLONG RunVbn, RunLbn, RunCount;
    ULONG i;
    BOOLEAN McbInitialized = FALSE, KeepAllocation = FALSE;
    NTSTATUS Status;

    ASSERT(!AttrContext->pRecord->IsNonResident);
    ASSERT((ULONGLONG)DataSize->QuadPart >= AttrContext->pRecord->Resident.ValueLength);

    /* Stage the existing value first. Growth beyond it is a separate step:
     * a failed reservation must leave the original bytes and EOF intact. */
    Destination = (PNTFS_ATTR_RECORD)((PUCHAR)FileRecord + AttrOffset);
    ValueLength = Destination->Resident.ValueLength;
    ASSERT(!(Destination->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE)));
    Clusters = ROUND_UP(ValueLength, Vcb->NtfsInfo.BytesPerCluster) / Vcb->NtfsInfo.BytesPerCluster;
    NewLength = Vcb->NtfsInfo.BytesPerFileRecord;
    NewRecord = ExAllocatePoolWithTag(NonPagedPool, NewLength, TAG_NTFS);
    WriteImage = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (NewRecord == NULL || WriteImage == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    if (ValueLength != 0)
    {
        Data = ExAllocatePoolWithTag(NonPagedPool, ValueLength, TAG_NTFS);
        if (Data == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        RtlCopyMemory(Data, (PUCHAR)Destination + Destination->Resident.ValueOffset, ValueLength);
    }
    RtlZeroMemory(NewRecord, NewLength);
    RtlCopyMemory(NewRecord, Destination, FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.ValueLength));
    NewRecord->Length = NewLength;
    NewRecord->IsNonResident = 1;
    NewRecord->NameOffset = 0x40;
    RtlCopyMemory((PUCHAR)NewRecord + NewRecord->NameOffset,
                  (PUCHAR)Destination + Destination->NameOffset,
                  Destination->NameLength * sizeof(WCHAR));
    NewRecord->NonResident.MappingPairsOffset = 0x40 + Destination->NameLength * sizeof(WCHAR);
    NewRecord->NonResident.HighestVCN = (ULONGLONG)Clusters - 1;
    NewRecord->NonResident.AllocatedSize = (ULONGLONG)Clusters * Vcb->NtfsInfo.BytesPerCluster;
    NewRecord->NonResident.DataSize = ValueLength;
    NewRecord->NonResident.InitializedSize = ValueLength;
    /* LARGE_MCB's allocation table points back to its containing BASE_MCB.
     * Initialize it at the final address; it cannot be moved by assignment. */
    _SEH2_TRY
    {
        FsRtlInitializeLargeMcb(&AttrContext->DataRunsMCB, NonPagedPool);
        McbInitialized = TRUE;
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    while (Vcn < Clusters)
    {
        Status = NtfsAllocateClusters(Vcb, 0, Clusters - Vcn, &FirstCluster, &AssignedClusters);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
        _SEH2_TRY
        {
            Status = FsRtlAddLargeMcbEntry(&AttrContext->DataRunsMCB, Vcn, FirstCluster, AssignedClusters) ?
                     STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Status))
        {
            (VOID)NtfsFreeClusterRange(Vcb, FirstCluster, AssignedClusters);
            goto Cleanup;
        }
        Vcn += AssignedClusters;
    }
    Status = ConvertLargeMCBToDataRuns(&AttrContext->DataRunsMCB, Clusters,
                                       (PUCHAR)NewRecord + NewRecord->NonResident.MappingPairsOffset,
                                       NewLength - NewRecord->NonResident.MappingPairsOffset, &RunBytes);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    NewLength = ALIGN_UP_BY(NewRecord->NonResident.MappingPairsOffset + RunBytes, ATTR_RECORD_ALIGNMENT);
    NewRecord->Length = NewLength;
    Status = NtfsMakeRoomToConvert(Vcb, AttrContext, &AttrOffset, FileRecord, NewLength);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Destination = (PNTFS_ATTR_RECORD)((PUCHAR)FileRecord + AttrOffset);
    OldLength = Destination->Length;

    if (ValueLength != 0)
    {
        SavedRecord = AttrContext->pRecord;
        AttrContext->pRecord = NewRecord;
        _SEH2_TRY
        {
            Status = WriteAttribute(Vcb, AttrContext, 0, Data, ValueLength, &Written, NULL);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        AttrContext->pRecord = SavedRecord;
        if (NT_SUCCESS(Status) && Written != ValueLength)
            Status = STATUS_UNEXPECTED_IO_ERROR;
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }

    /* Publish only after the new allocation contains the complete old value. */
    RtlCopyMemory(WriteImage, FileRecord, Vcb->NtfsInfo.BytesPerFileRecord);
    TailLength = WriteImage->BytesInUse - AttrOffset - OldLength;
    RtlMoveMemory((PUCHAR)WriteImage + AttrOffset + NewLength,
                  (PUCHAR)WriteImage + AttrOffset + OldLength, TailLength);
    WriteImage->BytesInUse = WriteImage->BytesInUse - OldLength + NewLength;
    RtlCopyMemory((PUCHAR)WriteImage + AttrOffset, NewRecord, NewLength);
    ASSERT(WriteImage->BytesInUse <= Vcb->NtfsInfo.BytesPerFileRecord);
    Status = UpdateFileRecord(Vcb, AttrContext->FileMFTIndex, WriteImage);
    if (!NT_SUCCESS(Status))
    {
        NTSTATUS RestoreStatus = UpdateFileRecord(Vcb, AttrContext->FileMFTIndex, FileRecord);
        if (!NT_SUCCESS(RestoreStatus))
        {
            KeepAllocation = TRUE;
            DPRINT1("Couldn't restore resident record after conversion failure: %lx\n", RestoreStatus);
        }
        goto Cleanup;
    }
    RtlCopyMemory(FileRecord, WriteImage, Vcb->NtfsInfo.BytesPerFileRecord);
    ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);
    AttrContext->pRecord = NewRecord;
    NewRecord = NULL;
    McbInitialized = FALSE;
    Status = SetNonResidentAttributeDataLength(Vcb, AttrContext, AttrOffset, FileRecord, DataSize);

Cleanup:
    if (McbInitialized && !KeepAllocation)
    {
        for (i = 0; FsRtlGetNextLargeMcbEntry(&AttrContext->DataRunsMCB, i, &RunVbn, &RunLbn, &RunCount); i++)
        {
            ASSERT(RunLbn != -1 && RunCount > 0);
            (VOID)NtfsFreeClusterRange(Vcb, (ULONG)RunLbn, (ULONG)RunCount);
        }
    }
    if (McbInitialized)
        FsRtlUninitializeLargeMcb(&AttrContext->DataRunsMCB);
    if (Data != NULL)
        ExFreePoolWithTag(Data, TAG_NTFS);
    if (NewRecord != NULL)
        ExFreePoolWithTag(NewRecord, TAG_NTFS);
    if (WriteImage != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, WriteImage);
    return Status;
}

/*
 * NTFS keeps $STANDARD_INFORMATION, $FILE_NAME, $OBJECT_ID, $INDEX_ROOT,
 * $VOLUME_NAME, $VOLUME_INFORMATION and $EA_INFORMATION resident at all times;
 * everything else may have its value moved into clusters of its own.
 */
static
BOOLEAN
NtfsAttributeCanBeNonResident(ULONG Type)
{
    switch (Type)
    {
        case AttributeSecurityDescriptor:
        case AttributeData:
        case AttributeBitmap:
        case AttributeReparsePoint:
        case AttributeEA:
        case AttributeLoggedUtilityStream:
            return TRUE;

        default:
            return FALSE;
    }
}

/**
* @name NtfsMakeRoomInFileRecord
* @implemented
*
* Frees space in a file record by moving resident values out into clusters of their own,
* biggest gain first, until the record has BytesNeeded bytes free.
*
* @param DeviceExt
* Pointer to a DEVICE_EXTENSION describing the target disk.
*
* @param FileRecord
* The complete base file record. Updated in place and on disk; attribute offsets the caller
* held are stale afterwards.
*
* @param BytesNeeded
* Number of free bytes the record needs to end up with.
*
* @param KeepInstance
* Instance of an attribute to leave resident, MAXULONG for none.
*
* @return
* STATUS_SUCCESS once the record has the room, STATUS_DISK_FULL if no resident value is
* left to move out, or the status of a conversion that failed.
*/
NTSTATUS
NtfsMakeRoomInFileRecord(PDEVICE_EXTENSION DeviceExt,
                         PFILE_RECORD_HEADER FileRecord,
                         ULONG BytesNeeded,
                         ULONG KeepInstance)
{
    while (DeviceExt->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse < BytesNeeded)
    {
        PNTFS_ATTR_CONTEXT AttrContext;
        PNTFS_ATTR_RECORD Attribute;
        LARGE_INTEGER AttributeSize;
        ULONG Offset = FileRecord->AttributeOffset;
        ULONG BestOffset = 0;
        ULONG BestGain = 0;
        NTSTATUS Status;

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + Offset);
        while (Attribute->Type != AttributeEnd)
        {
            /* Compressed, encrypted and sparse values would need their own conversion */
            if (!Attribute->IsNonResident &&
                Attribute->Flags == 0 &&
                Attribute->Instance != KeepInstance &&
                NtfsAttributeCanBeNonResident(Attribute->Type))
            {
                ULONG NonResidentLength = ALIGN_UP_BY(0x41 + Attribute->NameLength * sizeof(WCHAR),
                                                      ATTR_RECORD_ALIGNMENT);

                if (Attribute->Length > NonResidentLength &&
                    Attribute->Length - NonResidentLength > BestGain)
                {
                    BestGain = Attribute->Length - NonResidentLength;
                    BestOffset = Offset;
                }
            }

            Offset += Attribute->Length;
            Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + Offset);
        }

        if (BestGain == 0)
            return STATUS_DISK_FULL;

        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + BestOffset);
        AttrContext = PrepareAttributeContext(Attribute);
        if (AttrContext == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        AttrContext->FileMFTIndex = FileRecord->MFTRecordNumber;
        AttributeSize.QuadPart = Attribute->Resident.ValueLength;

        Status = ConvertResidentAttributeToNonResident(DeviceExt, AttrContext, BestOffset, FileRecord, &AttributeSize);
        ReleaseAttributeContext(AttrContext);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsGrowStream
*
* Makes a stream of a file at least Size bytes long; what it grows by reads as zeros.
*/
static
NTSTATUS
NtfsGrowStream(PDEVICE_EXTENSION Vcb,
               PNTFS_ATTR_CONTEXT AttrContext,
               ULONG AttrOffset,
               PFILE_RECORD_HEADER Holder,
               ULONGLONG Size)
{
    ULONGLONG OldSize = AttributeDataLength(AttrContext->pRecord);
    LARGE_INTEGER NewSize;
    NTSTATUS Status;

    if (Size <= OldSize)
        return STATUS_SUCCESS;

    NewSize.QuadPart = Size;
    if (AttrContext->pRecord->IsNonResident)
        Status = SetNonResidentAttributeDataLength(Vcb, AttrContext, AttrOffset, Holder, &NewSize);
    else
        Status = SetResidentAttributeDataLength(Vcb, AttrContext, AttrOffset, Holder, &NewSize);

    if (NT_SUCCESS(Status))
        Status = NtfsCommitAttributeContext(Vcb, AttrContext, Holder);
    if (NT_SUCCESS(Status))
        Status = NtfsZeroAttributeRange(Vcb, AttrContext, Holder, OldSize, Size);

    return Status;
}

/**
* @name NtfsWriteStreamAt
* @implemented
*
* Writes to a stream of a metadata file ($SDS of $Secure, $J of $UsnJrnl, ...) that has no
* FCB, growing it as needed. The caller serializes the writes to the stream.
*
* @param MftIndex
* The file's base record.
*/
NTSTATUS
NtfsWriteStreamAt(PDEVICE_EXTENSION Vcb,
                  ULONGLONG MftIndex,
                  PCWSTR StreamName,
                  ULONG StreamNameLength,
                  ULONGLONG Offset,
                  PVOID Buffer,
                  ULONG Length)
{
    PFILE_RECORD_HEADER BaseRecord, Holder = NULL;
    PNTFS_ATTR_CONTEXT AttrContext;
    ULONG AttrOffset, Written;
    NTSTATUS Status;

    BaseRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (BaseRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, MftIndex, BaseRecord);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, BaseRecord, AttributeData, StreamName, StreamNameLength, &AttrContext, &AttrOffset);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BaseRecord);
        return Status;
    }

    Status = NtfsReadAttributeHolder(Vcb, AttrContext, BaseRecord, &Holder);
    if (NT_SUCCESS(Status))
        Status = NtfsGrowStream(Vcb, AttrContext, AttrOffset, Holder, Offset + Length);
    if (NT_SUCCESS(Status))
        Status = WriteAttribute(Vcb, AttrContext, Offset, Buffer, Length, &Written, Holder);
    if (NT_SUCCESS(Status) && Written != Length)
        Status = STATUS_UNEXPECTED_IO_ERROR;

    NtfsReleaseAttributeHolder(Vcb, BaseRecord, Holder);
    ReleaseAttributeContext(AttrContext);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, BaseRecord);

    return Status;
}

/**
* @name NtfsMakeRoomToConvert
*
* Makes sure a record has room for a resident attribute to become non-resident, whose header
* is bigger than an empty value's: other values move out to clusters of their own first.
*
* @param AttrOffset
* Offset of the attribute in FileRecord. Updated, along with AttrContext's copy of the
* attribute, when other attributes move.
*/
static
NTSTATUS
NtfsMakeRoomToConvert(PDEVICE_EXTENSION Vcb,
                      PNTFS_ATTR_CONTEXT AttrContext,
                      PULONG AttrOffset,
                      PFILE_RECORD_HEADER FileRecord,
                      ULONG NewRecordLength)
{
    PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + *AttrOffset);
    USHORT Instance = Attribute->Instance;
    PNTFS_ATTR_RECORD Copy;
    ULONG Offset;
    NTSTATUS Status;

    if (NewRecordLength <= Attribute->Length ||
        FileRecord->BytesInUse + (NewRecordLength - Attribute->Length) <= Vcb->NtfsInfo.BytesPerFileRecord)
    {
        return STATUS_SUCCESS;
    }

    Status = NtfsMakeRoomInFileRecord(Vcb, FileRecord, NewRecordLength - Attribute->Length, Instance);
    if (!NT_SUCCESS(Status))
        return Status;

    // Find the attribute again: what was before it may have shrunk
    for (Offset = FileRecord->AttributeOffset; ; Offset += Attribute->Length)
    {
        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + Offset);
        ASSERT(Attribute->Type != AttributeEnd);
        if (Attribute->Instance == Instance)
            break;
    }

    ASSERT(!Attribute->IsNonResident);

    Copy = ExAllocatePoolWithTag(NonPagedPool, Attribute->Length, TAG_NTFS);
    if (Copy == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Copy, Attribute, Attribute->Length);
    ExFreePoolWithTag(AttrContext->pRecord, TAG_NTFS);
    AttrContext->pRecord = Copy;
    *AttrOffset = Offset;

    return STATUS_SUCCESS;
}

/**
* @name SetResidentAttributeDataLength
* @implemented
*
* Called by SetAttributeDataLength() to set the size of a resident attribute, moving it out
* of the record when the record can't hold it.
*/
NTSTATUS
SetResidentAttributeDataLength(PDEVICE_EXTENSION Vcb,
                               PNTFS_ATTR_CONTEXT AttrContext,
                               ULONG AttrOffset,
                               PFILE_RECORD_HEADER FileRecord,
                               PLARGE_INTEGER DataSize)
{
    NTSTATUS Status;
    ULONG FreeBytes = Vcb->NtfsInfo.BytesPerFileRecord - FileRecord->BytesInUse;
    ULONG MaxValueLength;

    ASSERT(!AttrContext->pRecord->IsNonResident);

    // The value can grow into the attribute's own padding and whatever the record has free;
    // the attributes behind it move along (InternalSetResidentAttributeLength)
    MaxValueLength = AttrContext->pRecord->Length - AttrContext->pRecord->Resident.ValueOffset + FreeBytes;

    if (DataSize->QuadPart > AttrContext->pRecord->Resident.ValueLength &&
        (ULONGLONG)DataSize->QuadPart > MaxValueLength)
    {
        // The data no longer fits in the file record: push the attribute out of it, after
        // moving other values out if even its non-resident header doesn't fit
        Status = ConvertResidentAttributeToNonResident(Vcb,
                                                       AttrContext,
                                                       AttrOffset,
                                                       FileRecord,
                                                       DataSize);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    // set the new length of the resident attribute (if we didn't migrate it)
    if (!AttrContext->pRecord->IsNonResident)
        return InternalSetResidentAttributeLength(Vcb, AttrContext, FileRecord, AttrOffset, DataSize->LowPart);

    return STATUS_SUCCESS;
}

/**
* @name NtfsMapAttributeRuns
* @implemented
*
* Maps a byte range of a non-resident attribute onto the volume, collecting the
* physically contiguous runs it spans into a run list.
*
* @param Vcb
* Volume the attribute lives on
*
* @param Context
* Attribute to map. Its data run cache is updated to reflect where the mapping
* stopped.
*
* @param Offset
* Byte offset into the attribute to start mapping at
*
* @param Length
* How many bytes to map
*
* @param RunList
* Receives the runs. Must have been initialized by NtfsInitIoRunList().
*
* @param MappedLength
* Receives how many bytes were actually mapped, which is less than Length if
* the attribute's allocation ends first.
*
* @return
* STATUS_SUCCESS if the range was mapped, STATUS_END_OF_FILE if Offset lies
* beyond the last allocated cluster, or STATUS_INSUFFICIENT_RESOURCES.
*
* @remarks Holes are recorded as NTFS_SPARSE_LBO runs rather than rejected
* here: reads satisfy them by zeroing and writes fail on them, so the policy
* belongs to the caller.
*
*/
static
NTSTATUS
NtfsMapAttributeRuns(PDEVICE_EXTENSION Vcb,
                     PNTFS_ATTR_CONTEXT Context,
                     ULONGLONG Offset,
                     ULONG Length,
                     PNTFS_IO_RUN_LIST RunList,
                     PULONG MappedLength)
{
    ULONGLONG LastLCN = 0;
    ULONGLONG CurrentOffset = 0;
    PUCHAR DataRun;
    LONGLONG DataRunOffset;
    ULONGLONG DataRunLength = 0;
    LONGLONG DataRunStartLCN = -1;
    BOOLEAN IsSparse;
    ULONG RunLength;
    ULONG Mapped = 0;
    ULONG UsedBufferSize;
    NTSTATUS Status = STATUS_SUCCESS;

    //TEMPTEMP
    PUCHAR TempBuffer;

    *MappedLength = 0;

    // This will be rewritten in the next iteration to just use the DataRuns MCB directly
    TempBuffer = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerFileRecord, TAG_NTFS);
    if (TempBuffer == NULL)
    {
        DPRINT1("Not enough memory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(TempBuffer, Vcb->NtfsInfo.BytesPerFileRecord);

    Status = ConvertLargeMCBToDataRuns(&Context->DataRunsMCB,
                                       Context->pRecord->NonResident.HighestVCN + 1,
                                       TempBuffer,
                                       Vcb->NtfsInfo.BytesPerFileRecord,
                                       &UsedBufferSize);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    DataRun = TempBuffer;

    // I. Walk forward to the data run that holds Offset.
    while (1)
    {
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength, &IsSparse);
        if (!IsSparse)
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
        {
            /* Offset is past the last cluster the attribute has allocated. */
            Status = STATUS_END_OF_FILE;
            goto Cleanup;
        }

        CurrentOffset += DataRunLength * Vcb->NtfsInfo.BytesPerCluster;
    }

    // II. Collect the runs. The first is special only in that the request
    // usually starts partway into it.

    RunLength = (ULONG)min(DataRunLength * Vcb->NtfsInfo.BytesPerCluster - (Offset - CurrentOffset), Length);

    Status = NtfsAddIoRun(RunList,
                          (DataRunStartLCN == -1) ? NTFS_SPARSE_LBO :
                              DataRunStartLCN * Vcb->NtfsInfo.BytesPerCluster + Offset - CurrentOffset,
                          RunLength);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Length -= RunLength;
    Mapped += RunLength;

    /* Did that consume the rest of this data run? */
    if (RunLength == DataRunLength * Vcb->NtfsInfo.BytesPerCluster - (Offset - CurrentOffset))
    {
        /* Never decode past the terminator; the caller gets a short map */
        if (*DataRun == 0)
            goto Cleanup;

        CurrentOffset += DataRunLength * Vcb->NtfsInfo.BytesPerCluster;
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength, &IsSparse);
        if (!IsSparse)
        {
            DataRunStartLCN = LastLCN + DataRunOffset;
            LastLCN = DataRunStartLCN;
        }
        else
            DataRunStartLCN = -1;
    }

    while (Length > 0)
    {
        RunLength = (ULONG)min(DataRunLength * Vcb->NtfsInfo.BytesPerCluster, Length);

        Status = NtfsAddIoRun(RunList,
                              (DataRunStartLCN == -1) ? NTFS_SPARSE_LBO :
                                  DataRunStartLCN * Vcb->NtfsInfo.BytesPerCluster,
                              RunLength);
        if (!NT_SUCCESS(Status))
            goto Cleanup;

        Length -= RunLength;
        Mapped += RunLength;

        /* We satisfied the request, but there is still data in this run. */
        if (Length == 0 && RunLength != DataRunLength * Vcb->NtfsInfo.BytesPerCluster)
            break;

        /* Last run the attribute has, so the caller gets a short map */
        if (*DataRun == 0)
            break;

        CurrentOffset += DataRunLength * Vcb->NtfsInfo.BytesPerCluster;
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength, &IsSparse);
        if (!IsSparse)
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
    }

Cleanup:
    Context->CacheRun = DataRun;
    Context->CacheRunOffset = Offset + Mapped;
    Context->CacheRunStartLCN = DataRunStartLCN;
    Context->CacheRunLength = DataRunLength;
    Context->CacheRunLastLCN = LastLCN;
    Context->CacheRunCurrentOffset = CurrentOffset;

    //TEMPTEMP
    ExFreePoolWithTag(TempBuffer, TAG_NTFS);

    *MappedLength = Mapped;

    return Status;
}

/**
* @name ReadAttributeToIrp
* @implemented
*
* Reads a range of an attribute, serving it out of the given IRP where the
* shape of the request allows it.
*
* @param Vcb
* Volume the attribute lives on
*
* @param Context
* Attribute to read from
*
* @param Offset
* Byte offset into the attribute to start reading at
*
* @param Buffer
* System-space buffer receiving the data
*
* @param Length
* How much to read, in bytes
*
* @param Irp
* The request being served, or NULL. When supplied and the range is a single
* aligned contiguous piece of the volume, the IRP goes straight to the storage
* stack and the disk transfers directly into the caller's pages.
*
* @return
* The number of bytes read, or zero on failure.
*
* @remarks An IRP may only be passed when Buffer is the start of that IRP's
* buffer and nothing has been realigned or bounced, since a forwarded transfer
* lands at the beginning of the IRP's MDL.
*
*/
/* Keep attribute reads and writes coherent with metadata in the volume cache.
 * Before mount has initialized that cache, retain the IO-run bootstrap path. */
static
NTSTATUS
NtfsTransferAttributeRuns(PDEVICE_EXTENSION Vcb,
                          UCHAR MajorFunction,
                          PIRP Irp,
                          PUCHAR Buffer,
                          PNTFS_IO_RUN_LIST RunList,
                          BOOLEAN Direct,
                          PULONG Transferred)
{
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    *Transferred = 0;
    /* Paging file I/O goes straight to the disk. It is how Mm makes memory
     * free, so it must not need cache pages of its own, and nothing but Mm
     * ever reads or writes those clusters. */
    if (Vcb->StreamFileObject == NULL || Direct)
    {
        return NtfsPerformIrpIoRuns(Vcb->StorageDevice, MajorFunction,
                                    Vcb->NtfsInfo.BytesPerSector, Irp, Buffer,
                                    RunList, FALSE, Transferred);
    }

    for (Index = 0; Index < RunList->Count; Index++)
    {
        PNTFS_IO_RUN Run = &RunList->Runs[Index];
        PUCHAR Data = Buffer + Run->BufferOffset;

        if (Run->Lbo == NTFS_SPARSE_LBO)
        {
            ASSERT(MajorFunction == IRP_MJ_READ);
            RtlZeroMemory(Data, Run->ByteCount);
        }
        else if (MajorFunction == IRP_MJ_READ)
        {
            Status = NtfsReadDiskCached(Vcb, Run->Lbo, Run->ByteCount, Data);
        }
        else
        {
            Status = NtfsWriteDiskCached(Vcb, Run->Lbo, Run->ByteCount, Data);
        }
        if (!NT_SUCCESS(Status))
            break;
        *Transferred += Run->ByteCount;
    }
    return Status;
}

ULONG
ReadAttributeToIrp(PDEVICE_EXTENSION Vcb,
                   PNTFS_ATTR_CONTEXT Context,
                   ULONGLONG Offset,
                   PCHAR Buffer,
                   ULONG Length,
                   PIRP Irp)
{
    ULONG AlreadyRead = 0;
    ULONG Transferred = 0;
    NTSTATUS Status;
    NTFS_IO_RUN_LIST RunList;

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
     * Non-resident attribute: map the requested range onto the volume, then
     * issue every run it spans in a single parallel batch.
     */

    NtfsInitIoRunList(&RunList);

    Status = NtfsMapAttributeRuns(Vcb, Context, Offset, Length, &RunList, &AlreadyRead);

    if (NT_SUCCESS(Status) && AlreadyRead != 0)
    {
        Status = NtfsTransferAttributeRuns(Vcb, IRP_MJ_READ, Irp, (PUCHAR)Buffer,
                                            &RunList, Context->DirectIo, &Transferred);
    }

    NtfsFreeIoRunList(&RunList);

    if (!NT_SUCCESS(Status))
    {
        /* Our callers read the return value as a byte count and treat zero as
         * failure, so we cannot report a partial transfer here. */
        DPRINT1("Read failure (Status %lx)\n", Status);
        return 0;
    }

    if (Transferred < AlreadyRead)
    {
        DPRINT1("Short read: expected %lu bytes, got %lu\n", AlreadyRead, Transferred);
        AlreadyRead = Transferred;
    }

    return AlreadyRead;
}

ULONG
ReadAttribute(PDEVICE_EXTENSION Vcb,
              PNTFS_ATTR_CONTEXT Context,
              ULONGLONG Offset,
              PCHAR Buffer,
              ULONG Length)
{
    return ReadAttributeToIrp(Vcb, Context, Offset, Buffer, Length, NULL);
}


/**
* @name NtfsFillHolesForWrite
*
* Gives the holes of a sparse attribute that a write of Length bytes at Offset lands in
* their clusters, in whole allocation units, and writes the record holding the attribute.
*
* @param FileRecord
* The record holding the attribute, or NULL to read it.
*/
static
NTSTATUS
NtfsFillHolesForWrite(PDEVICE_EXTENSION Vcb,
                      PNTFS_ATTR_CONTEXT Context,
                      PFILE_RECORD_HEADER FileRecord,
                      ULONGLONG Offset,
                      ULONG Length)
{
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONG Unit = NtfsAttributeIsSparse(Context) ? NtfsSparseUnitClusters(Context) : 1;
    ULONGLONG TotalVcns = Context->pRecord->NonResident.HighestVCN + 1;
    ULONGLONG StartVcn = (Offset / BytesPerCluster) / Unit * Unit;
    ULONGLONG EndVcn = min(ALIGN_UP_BY((Offset + Length + BytesPerCluster - 1) / BytesPerCluster, Unit), TotalVcns);
    ULONGLONG Vcn;
    PFILE_RECORD_HEADER Holder = FileRecord;
    PNTFS_ATTR_RECORD Attribute;
    ULONG AttrOffset;
    NTSTATUS Status;

    /* Nothing to do unless part of the range is a hole */
    for (Vcn = StartVcn; Vcn < EndVcn; )
    {
        LONGLONG Lbn, Count;

        if (!FsRtlLookupLargeMcbEntry(&Context->DataRunsMCB, Vcn, &Lbn, &Count, NULL, NULL, NULL) || Lbn == -1)
            break;
        Vcn += Count;
    }
    if (Vcn >= EndVcn)
        return STATUS_SUCCESS;

    if (Holder == NULL)
    {
        Holder = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
        if (Holder == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = ReadFileRecord(Vcb, Context->FileMFTIndex & NTFS_MFT_MASK, Holder);
        if (!NT_SUCCESS(Status))
        {
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Holder);
            return Status;
        }
    }

    /* The attribute is the one of the record with its instance number */
    for (AttrOffset = Holder->AttributeOffset; ; AttrOffset += Attribute->Length)
    {
        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Holder + AttrOffset);
        ASSERT(Attribute->Type != AttributeEnd);
        if (Attribute->Type == AttributeEnd || Attribute->Instance == Context->pRecord->Instance)
            break;
    }

    if (Attribute->Type == AttributeEnd)
        Status = STATUS_FILE_CORRUPT_ERROR;
    else
        Status = NtfsAllocateSparseRange(Vcb, Context, AttrOffset, Holder, StartVcn, EndVcn - StartVcn);

    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Context->FileMFTIndex & NTFS_MFT_MASK, Holder);

    if (Holder != FileRecord)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Holder);

    return Status;
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

/* Non-resident metadata, which is logged here: everything but file data,
 * and the file records themselves, whose writes UpdateFileRecord logs. */
BOOLEAN
NtfsAttributeIsMetadata(PNTFS_ATTR_CONTEXT Context)
{
    return Context->pRecord->Type != AttributeData ||
           Context->FileMFTIndex < NTFS_FILE_FIRST_USER_FILE;
}

static NTSTATUS
NtfsLogMetadataRuns(PDEVICE_EXTENSION Vcb,
                    PNTFS_IO_RUN_LIST RunList,
                    const UCHAR *Buffer,
                    ULONGLONG Offset,
                    PNTFS_LOG_METADATA_WRITE LogWrite)
{
    NTFS_LOG_EXTENT Extents[NTFS_LOG_METADATA_EXTENTS];
    ULONG i;
    NTSTATUS Status;

    if (RunList->Count > RTL_NUMBER_OF(Extents))
    {
        /* Too fragmented to log: the volume is checked after a crash. */
        LogWrite->Pending.Lsn = 0;
        Status = NtfsLfsBeginUnloggedWrite(Vcb->LfsWriter);
        LogWrite->Unlogged = NT_SUCCESS(Status);
        return Status;
    }
    for (i = 0; i < RunList->Count; i++)
    {
        /* Holes are filled before a write maps its runs. */
        ASSERT(RunList->Runs[i].Lbo != NTFS_SPARSE_LBO);
        Extents[i].DiskOffset = RunList->Runs[i].Lbo;
        Extents[i].Length = RunList->Runs[i].ByteCount;
        Extents[i].BufferOffset = RunList->Runs[i].BufferOffset;
    }
    return NtfsLfsBeginMetadataWrite(Vcb->LfsWriter, Extents, RunList->Count, Buffer, Offset, LogWrite);
}

static NTSTATUS
WriteAttributeInternal(PDEVICE_EXTENSION Vcb,
                       PNTFS_ATTR_CONTEXT Context,
                       ULONGLONG Offset,
                       const PUCHAR Buffer,
                       ULONG Length,
                       PULONG RealLengthWritten,
                       PFILE_RECORD_HEADER FileRecord,
                       PIRP Irp,
                       BOOLEAN Logged)
{
    ULONG MappedLength = 0;
    ULONG Transferred = 0;
    NTSTATUS Status;
    BOOLEAN FileRecordAllocated = FALSE;
    NTFS_IO_RUN_LIST RunList;


    DPRINT("WriteAttribute(%p, %p, %I64u, %p, %lu, %p, %p)\n", Vcb, Context, Offset, Buffer, Length, RealLengthWritten, FileRecord);

    *RealLengthWritten = 0;
    NtfsInitIoRunList(&RunList);

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

    // This is a non-resident attribute: map the requested range onto the
    // volume, then issue every run it spans in a single parallel batch.

    /* A write into a hole gives the attribute clusters first. Holes belong to sparse
     * attributes; a compressed one stores its units differently. */
    if (!(Context->pRecord->Flags & NTFS_ATTR_COMPRESSION_MASK))
    {
        Status = NtfsFillHolesForWrite(Vcb, Context, FileRecord, Offset, Length);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = NtfsMapAttributeRuns(Vcb, Context, Offset, Length, &RunList, &MappedLength);

    if (NT_SUCCESS(Status) && MappedLength < Length)
    {
        // The attribute's allocation ends before the request does.
        // TODO: assign new clusters to the end of the file.
        // [We can reach here by creating a new file record when the MFT isn't large enough]
        DPRINT1("FIXME: Attribute needs to be enlarged (mapped %lu of %lu bytes).\n",
                MappedLength, Length);
        Status = STATUS_END_OF_FILE;
    }

    if (NT_SUCCESS(Status))
    {
        BOOLEAN Metadata = !Logged && !Context->DirectIo && NtfsAttributeIsMetadata(Context);
        NTFS_LOG_METADATA_WRITE LogWrite;

        /* Metadata is logged before it is written, as file records are. */
        if (Metadata)
            Status = NtfsLogMetadataRuns(Vcb, &RunList, Buffer, Offset, &LogWrite);
        /* NtfsPerformIrpIoRuns() rejects sparse runs on IRP_MJ_WRITE: holes are
         * filled above, and only a compressed attribute still has them */
        if (NT_SUCCESS(Status))
        {
            Status = NtfsTransferAttributeRuns(Vcb, IRP_MJ_WRITE, Irp, (PUCHAR)Buffer,
                                                &RunList, Context->DirectIo, &Transferred);
            if (Metadata)
                NtfsLfsEndMetadataWrite(Vcb->LfsWriter, &LogWrite);
        }
    }

    NtfsFreeIoRunList(&RunList);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Write failure (Status %lx)\n", Status);
        *RealLengthWritten = 0;
        return Status;
    }

    *RealLengthWritten = Transferred;

    if (Transferred < Length)
    {
        DPRINT1("Short write: expected %lu bytes, wrote %lu\n", Length, Transferred);
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    return Status;
}

NTSTATUS
WriteAttributeFromIrp(PDEVICE_EXTENSION Vcb,
                      PNTFS_ATTR_CONTEXT Context,
                      ULONGLONG Offset,
                      const PUCHAR Buffer,
                      ULONG Length,
                      PULONG RealLengthWritten,
                      PFILE_RECORD_HEADER FileRecord,
                      PIRP Irp)
{
    return WriteAttributeInternal(Vcb, Context, Offset, Buffer, Length,
                                  RealLengthWritten, FileRecord, Irp, FALSE);
}

NTSTATUS
WriteAttribute(PDEVICE_EXTENSION Vcb,
               PNTFS_ATTR_CONTEXT Context,
               ULONGLONG Offset,
               const PUCHAR Buffer,
               ULONG Length,
               PULONG RealLengthWritten,
               PFILE_RECORD_HEADER FileRecord)
{
    return WriteAttributeInternal(Vcb, Context, Offset, Buffer, Length,
                                  RealLengthWritten, FileRecord, NULL, FALSE);
}

/**
* @name NtfsReadUpCaseTable
* @implemented
*
* Loads the volume's $UpCase file, which defines how NTFS folds case.
*
* @param Vcb
* Volume to load the table for
*
* @return
* STATUS_SUCCESS if the table was loaded, otherwise the failure that stopped
* us. A volume without a usable $UpCase is not a valid NTFS volume.
*
* @remarks Every NTFS volume carries this file at a fixed MFT index. It is one
* upcased code point per UCS-2 value, so exactly 128 KB.
*
*/
static
NTSTATUS
NtfsReadUpCaseTable(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    NTSTATUS Status;
    ULONGLONG Size;
    ULONG Read;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, NTFS_FILE_UPCASE, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Couldn't read the $UpCase file record!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return Status;
    }

    Status = FindAttribute(Vcb, FileRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("$UpCase has no data stream!\n");
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return Status;
    }

    Size = AttributeDataLength(DataContext->pRecord);
    if (Size != (0x10000 * sizeof(WCHAR)))
    {
        DPRINT1("$UpCase is %I64u bytes, expected %u\n", Size, 0x10000 * sizeof(WCHAR));
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Vcb->UpCaseTable = ExAllocatePoolWithTag(NonPagedPool, (ULONG)Size, TAG_NTFS);
    if (Vcb->UpCaseTable == NULL)
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Read = ReadAttribute(Vcb, DataContext, 0, (PCHAR)Vcb->UpCaseTable, (ULONG)Size);

    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    if (Read != Size)
    {
        DPRINT1("Short read of $UpCase: %lu of %I64u bytes\n", Read, Size);
        ExFreePoolWithTag(Vcb->UpCaseTable, TAG_NTFS);
        Vcb->UpCaseTable = NULL;
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NtfsLoadUpCaseTable(PDEVICE_EXTENSION Vcb)
{
    return NtfsReadUpCaseTable(Vcb);
}

static NTSTATUS
ReadFileRecordInternal(PDEVICE_EXTENSION Vcb,
               ULONGLONG index,
               PFILE_RECORD_HEADER file,
               BOOLEAN AllowUnused)
{
    ULONGLONG BytesRead;
    ULONG i, RecordSize = Vcb->NtfsInfo.BytesPerFileRecord;

    DPRINT("ReadFileRecord(%p, %I64x, %p)\n", Vcb, index, file);

    BytesRead = ReadAttribute(Vcb, Vcb->MFTContext, index * Vcb->NtfsInfo.BytesPerFileRecord, (PCHAR)file, Vcb->NtfsInfo.BytesPerFileRecord);
    if (BytesRead != Vcb->NtfsInfo.BytesPerFileRecord)
    {
        DPRINT1("ReadFileRecord failed: %I64u read, %lu expected\n", BytesRead, Vcb->NtfsInfo.BytesPerFileRecord);
        return STATUS_PARTIAL_COPY;
    }

    /* A bitmap-selected free slot can be uninitialized. It has no sequence
     * number or update sequence array to recover yet. */
    if (AllowUnused)
    {
        for (i = 0; i < RecordSize; ++i)
            if (((PUCHAR)file)[i] != 0)
                break;
        if (i == RecordSize)
            return STATUS_SUCCESS;
    }

    ASSERT(Vcb->NtfsInfo.BytesPerSector != 0);
    if (file->Ntfs.Type != NRH_FILE_TYPE ||
        file->Ntfs.UsaCount != RecordSize / Vcb->NtfsInfo.BytesPerSector + 1 ||
        file->Ntfs.UsaOffset < sizeof(NTFS_RECORD_HEADER) ||
        (ULONG)file->Ntfs.UsaOffset + file->Ntfs.UsaCount * sizeof(USHORT) > RecordSize)
        return STATUS_FILE_CORRUPT_ERROR;

    /* Apply update sequence array fixups. */
    DPRINT("Sequence number: %u\n", file->SequenceNumber);
    return FixupUpdateSequenceArray(Vcb, &file->Ntfs);
}

NTSTATUS
NtfsReadFileRecordEx(PDEVICE_EXTENSION Vcb, ULONGLONG Index,
                     PFILE_RECORD_HEADER Record, BOOLEAN AllowUnused)
{
    NTSTATUS Status;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&Vcb->MftContextResource, TRUE);
    _SEH2_TRY
    {
        Status = ReadFileRecordInternal(Vcb, Index, Record, AllowUnused);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        ExReleaseResourceLite(&Vcb->MftContextResource);
        KeLeaveCriticalRegion();
        _SEH2_YIELD(ExRaiseStatus(Status));
    }
    _SEH2_END;
    ExReleaseResourceLite(&Vcb->MftContextResource);
    KeLeaveCriticalRegion();
    return Status;
}

NTSTATUS
ReadFileRecord(PDEVICE_EXTENSION Vcb, ULONGLONG Index, PFILE_RECORD_HEADER Record)
{
    return NtfsReadFileRecordEx(Vcb, Index, Record, FALSE);
}

/**
* @name NtfsUpdateDuplicatedInformation
* @implemented
*
* Pushes the fields NTFS keeps in more than one place out to all of them.
*
* @param Vcb
* Volume the file lives on
*
* @param FileRecord
* The file's record, already holding the new values in $STANDARD_INFORMATION
* and its $DATA attribute
*
* @param MFTIndex
* MFT index of the file
*
* @param Flags
* Which groups of fields changed; see NTFS_FILENAME_UPDATE_*
*
* @param CaseSensitive
* Whether the parent's index should be searched case-sensitively
*
* @return
* STATUS_SUCCESS once every copy agrees, or the status of the failing update.
*
* @remarks NTFS stores a file's sizes, timestamps and attributes three times:
* in $STANDARD_INFORMATION, in the record's own $FILE_NAME, and again in the
* parent directory's index entry. Windows funnels all three through one
* routine (NtfsUpdateDuplicateInfo) precisely so they cannot drift; this is
* that funnel. Callers change $STANDARD_INFORMATION and then say what moved.
*
* The file record is written back here, so callers need not write it again.
*
* The directory flag is the one field that does not come from
* $STANDARD_INFORMATION -- NTFS masks it out of there and keeps it in the
* record header -- so it is restored from the header on the way past.
*/
NTSTATUS
NtfsUpdateDuplicatedInformation(PDEVICE_EXTENSION Vcb,
                                PFILE_RECORD_HEADER FileRecord,
                                ULONGLONG MFTIndex,
                                ULONG Flags,
                                BOOLEAN CaseSensitive)
{
    NTFS_FILENAME_UPDATE Update;
    PSTANDARD_INFORMATION StdInfo;
    PFILENAME_ATTRIBUTE FileName;
    UNICODE_STRING Name;
    ULONGLONG ParentMFTIndex;
    ULONG DirFlag;
    NTSTATUS Status;

    if (Flags == 0)
        return STATUS_SUCCESS;

    StdInfo = GetStandardInformationFromRecord(Vcb, FileRecord);
    FileName = GetBestFileNameFromRecord(Vcb, FileRecord);
    if (StdInfo == NULL || FileName == NULL)
    {
        DPRINT1("Record %I64u is missing $STANDARD_INFORMATION or $FILE_NAME\n", MFTIndex);
        return STATUS_INVALID_PARAMETER;
    }

    DirFlag = (FileRecord->Flags & FRH_DIRECTORY) ? NTFS_FILE_TYPE_DIRECTORY : 0;

    RtlZeroMemory(&Update, sizeof(Update));
    Update.Flags = Flags;

    if (Flags & NTFS_FILENAME_UPDATE_TIMES)
    {
        Update.CreationTime = StdInfo->CreationTime;
        Update.LastWriteTime = StdInfo->LastWriteTime;
        Update.ChangeTime = StdInfo->ChangeTime;
        Update.LastAccessTime = StdInfo->LastAccessTime;

        FileName->CreationTime = StdInfo->CreationTime;
        FileName->LastWriteTime = StdInfo->LastWriteTime;
        FileName->ChangeTime = StdInfo->ChangeTime;
        FileName->LastAccessTime = StdInfo->LastAccessTime;
    }

    if (Flags & NTFS_FILENAME_UPDATE_ATTRS)
    {
        Update.FileAttributes = StdInfo->FileAttribute | DirFlag;
        FileName->FileAttributes = Update.FileAttributes;
    }

    if (Flags & NTFS_FILENAME_UPDATE_SIZES)
    {
        /* Sizes come from the unnamed $DATA attribute, the same way the times
         * come from $STANDARD_INFORMATION: one source of truth per field. */
        Update.DataSize = NtfsGetFileSize(Vcb, FileRecord, L"", 0, &Update.AllocatedSize);

        FileName->DataSize = Update.DataSize;
        FileName->AllocatedSize = Update.AllocatedSize;
    }

    /* Persist the $FILE_NAME just modified. Callers have no reason to write the record again,
     * and leaving it unwritten would let the copy in the parent index disagree with it. */
    Status = UpdateFileRecord(Vcb, MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to write file record %I64u (Status %lx)\n", MFTIndex, Status);
        return Status;
    }

    /* Now the parent's index entry, which carries the same values */
    ParentMFTIndex = FileName->DirectoryFileReferenceNumber & NTFS_MFT_MASK;

    Name.Buffer = FileName->Name;
    Name.Length = FileName->NameLength * sizeof(WCHAR);
    Name.MaximumLength = Name.Length;

    return UpdateFileNameRecord(Vcb, ParentMFTIndex, &Name, FALSE, &Update, CaseSensitive);
}

/**
* Searches a file's parent directory (given the parent's index in the mft)
* for the given file. Upon finding an index entry for that file, refreshes the
* fields of that entry's $FILE_NAME attribute named by Update->Flags.
*
* (Most of this code was copied from NtfsFindMftRecord)
*/
static
NTSTATUS
UpdateFileNameRecordWorker(PDEVICE_EXTENSION Vcb,
                           ULONGLONG ParentMFTIndex,
                           PUNICODE_STRING FileName,
                           BOOLEAN DirSearch,
                           PNTFS_FILENAME_UPDATE Update,
                           BOOLEAN CaseSensitive)
{
    PFILE_RECORD_HEADER MftRecord;
    PNTFS_ATTR_CONTEXT IndexRootCtx;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PCHAR IndexRecord;
    PINDEX_ENTRY_ATTRIBUTE IndexEntry, IndexEntryEnd;
    NTSTATUS Status;
    ULONG CurrentEntry = 0;

    DPRINT("UpdateFileNameRecord(%p, %I64d, %wZ, %s, %lx, %s)\n",
           Vcb,
           ParentMFTIndex,
           FileName,
           DirSearch ? "TRUE" : "FALSE",
           Update->Flags,
           CaseSensitive ? "TRUE" : "FALSE");

    if (Update->Flags == 0)
        return STATUS_SUCCESS;

    MftRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (MftRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, ParentMFTIndex, MftRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return Status;
    }

    ASSERT(MftRecord->Ntfs.Type == NRH_FILE_TYPE);
    Status = FindAttribute(Vcb, MftRecord, AttributeIndexRoot, L"$I30", 4, &IndexRootCtx, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return Status;
    }

    IndexRecord = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerIndexRecord, TAG_NTFS);
    if (IndexRecord == NULL)
    {
        ReleaseAttributeContext(IndexRootCtx);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // ReadAttribute returns the bytes it read
    if (ReadAttribute(Vcb, IndexRootCtx, 0, IndexRecord, (ULONG)AttributeDataLength(IndexRootCtx->pRecord)) !=
        AttributeDataLength(IndexRootCtx->pRecord))
    {
        Status = STATUS_UNEXPECTED_IO_ERROR;
        DPRINT1("ERROR: Failed to read Index Root!\n");
        ExFreePoolWithTag(IndexRecord, TAG_NTFS);
        ReleaseAttributeContext(IndexRootCtx);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return Status;
    }

    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)IndexRecord;
    IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
    // Index root is always resident.
    /* TotalSizeOfEntries is measured from the index header, not from the
     * start of the attribute, and FirstEntryOffset above already is. */
    IndexEntryEnd = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);

    DPRINT("IndexRecordSize: %x IndexBlockSize: %x\n", Vcb->NtfsInfo.BytesPerIndexRecord, IndexRoot->SizeOfEntry);

    Status = UpdateIndexEntryFileName(Vcb,
                                      MftRecord,
                                      IndexRecord,
                                      IndexRoot->SizeOfEntry,
                                      IndexEntry,
                                      IndexEntryEnd,
                                      FileName,
                                      &CurrentEntry,
                                      &CurrentEntry,
                                      DirSearch,
                                      Update,
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

    return Status;
}

/* The worker rewrites the directory record from a copy it reads, so it
 * must not overlap any other update of a directory index */
NTSTATUS
UpdateFileNameRecord(PDEVICE_EXTENSION Vcb,
                     ULONGLONG ParentMFTIndex,
                     PUNICODE_STRING FileName,
                     BOOLEAN DirSearch,
                     PNTFS_FILENAME_UPDATE Update,
                     BOOLEAN CaseSensitive)
{
    NTSTATUS Status;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->IndexResource, TRUE);
    Status = UpdateFileNameRecordWorker(Vcb, ParentMFTIndex, FileName, DirSearch, Update, CaseSensitive);
    ExReleaseResourceLite(&Vcb->IndexResource);
    KeLeaveCriticalRegion();

    return Status;
}

/**
* Recursively searches a directory index and applies the requested update to the $FILE_NAME
* attribute of the proper index entry.
* (Heavily based on BrowseIndexEntries)
*/
NTSTATUS
UpdateIndexEntryFileName(PDEVICE_EXTENSION Vcb,
                         PFILE_RECORD_HEADER MftRecord,
                         PCHAR IndexRecord,
                         ULONG IndexBlockSize,
                         PINDEX_ENTRY_ATTRIBUTE FirstEntry,
                         PINDEX_ENTRY_ATTRIBUTE LastEntry,
                         PUNICODE_STRING FileName,
                         PULONG StartEntry,
                         PULONG CurrentEntry,
                         BOOLEAN DirSearch,
                         PNTFS_FILENAME_UPDATE Update,
                         BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    ULONG RecordOffset;
    PINDEX_ENTRY_ATTRIBUTE IndexEntry;
    PNTFS_ATTR_CONTEXT IndexAllocationCtx;
    ULONGLONG IndexAllocationSize;
    PINDEX_BUFFER IndexBuffer;

    DPRINT("UpdateIndexEntryFileName(%p, %p, %p, %lu, %p, %p, %wZ, %lu, %lu, %s, %lx, %s)\n",
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
           Update->Flags,
           CaseSensitive ? "TRUE" : "FALSE");

    // find the index entry responsible for the file we're trying to update
    IndexEntry = FirstEntry;
    while (IndexEntry < LastEntry &&
           !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
    {
        if ((IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK) > NTFS_FILE_FIRST_USER_FILE &&
            *CurrentEntry >= *StartEntry &&
            IndexEntry->FileName.NameType != NTFS_FILE_NAME_DOS &&
            CompareFileName(Vcb, FileName, IndexEntry, DirSearch, CaseSensitive))
        {
            *StartEntry = *CurrentEntry;

            if (Update->Flags & NTFS_FILENAME_UPDATE_REPLACE)
            {
                /* The name collates to the existing key, so its place and
                 * length in the index do not change. */
                ASSERT(IndexEntry->FileName.NameLength == Update->FileName->NameLength);
                IndexEntry->Data.Directory.IndexedFile = Update->FileReference;
                RtlCopyMemory(&IndexEntry->FileName, Update->FileName,
                              FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) +
                              Update->FileName->NameLength * sizeof(WCHAR));
            }

            if (Update->Flags & NTFS_FILENAME_UPDATE_SIZES)
            {
                IndexEntry->FileName.DataSize = Update->DataSize;
                IndexEntry->FileName.AllocatedSize = Update->AllocatedSize;
            }

            if (Update->Flags & NTFS_FILENAME_UPDATE_TIMES)
            {
                IndexEntry->FileName.CreationTime = Update->CreationTime;
                IndexEntry->FileName.ChangeTime = Update->ChangeTime;
                IndexEntry->FileName.LastWriteTime = Update->LastWriteTime;
                IndexEntry->FileName.LastAccessTime = Update->LastAccessTime;
            }

            if (Update->Flags & NTFS_FILENAME_UPDATE_ATTRS)
            {
                IndexEntry->FileName.FileAttributes = Update->FileAttributes;
            }

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

        Status = UpdateIndexEntryFileName(NULL,
                                          NULL,
                                          NULL,
                                          0,
                                          FirstEntry,
                                          LastEntry,
                                          FileName,
                                          StartEntry,
                                          CurrentEntry,
                                          DirSearch,
                                          Update,
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
/* $MFTMirr holds copies of the first records of the MFT ($MFT, $MFTMirr,
 * $LogFile, $Volume); Windows writes a mirrored record to both, so the copy
 * never falls behind. Image is the on-disk form of record MftIndex. The
 * caller holds MftContextResource. */
static NTSTATUS
NtfsMirrorFileRecord(PDEVICE_EXTENSION Vcb,
                     ULONGLONG MftIndex,
                     PFILE_RECORD_HEADER Image)
{
    PFILE_RECORD_HEADER Mirror;
    PNTFS_ATTR_CONTEXT Data = NULL;
    ULONG RecordSize = Vcb->NtfsInfo.BytesPerFileRecord;
    ULONG Written;
    NTSTATUS Status;

    /* The mirror is four records, or one cluster when a cluster holds more;
     * its $DATA length below has the last word. */
    if (MftIndex >= max(4, Vcb->NtfsInfo.BytesPerCluster / RecordSize))
        return STATUS_SUCCESS;

    Mirror = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Mirror == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadFileRecordInternal(Vcb, NTFS_FILE_MFTMIRR, Mirror, FALSE);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Mirror, AttributeData, L"", 0, &Data, NULL);
    /* Logged as the metadata write it is: recovery replays it with the
     * record's own write. */
    if (NT_SUCCESS(Status) &&
        (MftIndex + 1) * RecordSize <= AttributeDataLength(Data->pRecord))
    {
        Status = WriteAttributeInternal(Vcb, Data, MftIndex * RecordSize, (const PUCHAR)Image,
                                        RecordSize, &Written, Mirror, NULL, FALSE);
    }
    if (Data != NULL)
        ReleaseAttributeContext(Data);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Mirror);
    return Status;
}

/* Recovery's write of a file record image (on-disk form) to its mirror. */
NTSTATUS
NtfsWriteFileRecordMirror(PDEVICE_EXTENSION Vcb,
                          ULONGLONG MftIndex,
                          PFILE_RECORD_HEADER Image)
{
    NTSTATUS Status;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&Vcb->MftContextResource, TRUE);
    Status = NtfsMirrorFileRecord(Vcb, MftIndex, Image);
    ExReleaseResourceLite(&Vcb->MftContextResource);
    KeLeaveCriticalRegion();
    return Status;
}

static NTSTATUS
UpdateFileRecordInternal(PDEVICE_EXTENSION Vcb,
                 ULONGLONG MftIndex,
                 PFILE_RECORD_HEADER FileRecord,
                 PNTFS_LOG_PENDING LogPending)
{
    ULONG BytesWritten;
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER WriteImage;

    DPRINT("UpdateFileRecord(%p, 0x%I64x, %p)\n", Vcb, MftIndex, FileRecord);

    // Stamp the fixup array into a private copy: the caller's record may be
    // shared, like Vcb->MasterFileTable, and must never hold the on-disk form
    WriteImage = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (WriteImage == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(WriteImage, FileRecord, Vcb->NtfsInfo.BytesPerFileRecord);

    // Add the fixup array to prepare the data for writing to disk
    AddFixupArray(Vcb, &WriteImage->Ntfs);

    // The log describes the bytes the disk will hold
    Status = NtfsLfsLogFileRecord(Vcb->LfsWriter, MftIndex, WriteImage, LogPending);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, WriteImage);
        return Status;
    }
    if (LogPending->Lsn != 0)
        FileRecord->Ntfs.Lsn = WriteImage->Ntfs.Lsn = LogPending->Lsn;

    // Keep the caller's update sequence number in step with the disk
    ((PFIXUP_ARRAY)((ULONG_PTR)FileRecord + FileRecord->Ntfs.UsaOffset))->USN =
        ((PFIXUP_ARRAY)((ULONG_PTR)WriteImage + WriteImage->Ntfs.UsaOffset))->USN;

    // write the file record to the master file table
    Status = WriteAttributeInternal(Vcb,
                                    Vcb->MFTContext,
                                    MftIndex * Vcb->NtfsInfo.BytesPerFileRecord,
                                    (const PUCHAR)WriteImage,
                                    Vcb->NtfsInfo.BytesPerFileRecord,
                                    &BytesWritten,
                                    FileRecord,
                                    NULL,
                                    TRUE);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("UpdateFileRecord failed: %lu written, %lu expected\n", BytesWritten, Vcb->NtfsInfo.BytesPerFileRecord);
    }
    else
    {
        Status = NtfsMirrorFileRecord(Vcb, MftIndex, WriteImage);
    }
    // Issued (or failed): a checkpoint's flush now covers it
    NtfsLfsFileRecordWritten(Vcb->LfsWriter, LogPending);

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, WriteImage);

    return Status;
}

NTSTATUS
UpdateFileRecord(PDEVICE_EXTENSION Vcb, ULONGLONG Index, PFILE_RECORD_HEADER Record)
{
    NTSTATUS Status;
    NTFS_LOG_PENDING LogPending;

    LogPending.Lsn = 0;
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&Vcb->MftContextResource, TRUE);
    _SEH2_TRY
    {
        Status = UpdateFileRecordInternal(Vcb, Index, Record, &LogPending);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        NtfsLfsFileRecordWritten(Vcb->LfsWriter, &LogPending);
        ExReleaseResourceLite(&Vcb->MftContextResource);
        KeLeaveCriticalRegion();
        _SEH2_YIELD(ExRaiseStatus(Status));
    }
    _SEH2_END;
    ExReleaseResourceLite(&Vcb->MftContextResource);
    KeLeaveCriticalRegion();
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

    USA = (USHORT*)((PCHAR)Record + Record->UsaOffset);
    USANumber = *(USA++);
    USACount = Record->UsaCount - 1; /* Exclude the USA Number. */
    Block = (USHORT*)((PCHAR)Record + Vcb->NtfsInfo.BytesPerSector - 2);

    DPRINT("FixupUpdateSequenceArray(%p, %p)\nUSANumber: %u\tUSACount: %u\n", Vcb, Record, USANumber, USACount);

    while (USACount)
    {
        if (*Block != USANumber)
        {
            DPRINT1("Mismatch with USA: %u read, %u expected\n" , *Block, USANumber);
            return STATUS_UNSUCCESSFUL;
        }
        *Block = *(USA++);
        Block = (USHORT*)((PCHAR)Block + Vcb->NtfsInfo.BytesPerSector);
        USACount--;
    }

    return STATUS_SUCCESS;
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
static NTSTATUS
AddNewMftEntryInternal(PFILE_RECORD_HEADER FileRecord,
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

    DPRINT("AddNewMftEntry(%p, %p, %p, %s)\n", FileRecord, DeviceExt, DestinationIndex, CanWait ? "TRUE" : "FALSE");

    // First, we have to read the mft's $Bitmap attribute

    // Find the attribute
    Status = FindAttribute(DeviceExt, DeviceExt->MasterFileTable, AttributeBitmap, L"", 0, &BitmapContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find $Bitmap attribute of master file table!\n");
        return Status;
    }

    // Get size of bitmap
    BitmapDataSize = AttributeDataLength(BitmapContext->pRecord);

    // RtlInitializeBitmap wants a ULONG-aligned pointer, and wants the memory passed to it to be a ULONG-multiple
    // Allocate a buffer for the $Bitmap attribute plus enough to ensure we can get a ULONG-aligned pointer
    BitmapBuffer = ExAllocatePoolWithTag(NonPagedPool, BitmapDataSize + sizeof(ULONG), TAG_NTFS);
    if (!BitmapBuffer)
    {
        ReleaseAttributeContext(BitmapContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(BitmapBuffer, BitmapDataSize + sizeof(ULONG));

    // Get a ULONG-aligned pointer for the bitmap itself
    BitmapData = (PUCHAR)ALIGN_UP_BY((ULONG_PTR)BitmapBuffer, sizeof(ULONG));

    // read $Bitmap attribute
    AttrBytesRead = ReadAttribute(DeviceExt, BitmapContext, 0, (PCHAR)BitmapData, BitmapDataSize);

    if (AttrBytesRead != BitmapDataSize)
    {
        DPRINT1("ERROR: Unable to read $Bitmap attribute of master file table!\n");
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    // We need to backup the bits for records 0x10 - 0x17 (3rd byte of bitmap) and mark these records
    // as in-use so we don't assign files to those indices. They're reserved for the system (e.g. ChkDsk).
    SystemReservedBits = BitmapData[2];
    BitmapData[2] = 0xff;

    // Calculate bit count
    BitmapBits.QuadPart = AttributeDataLength(DeviceExt->MFTContext->pRecord) /
                          DeviceExt->NtfsInfo.BytesPerFileRecord;
    if (BitmapBits.HighPart != 0)
    {
        DPRINT1("\tFIXME: bitmap sizes beyond 32bits are not yet supported! (Your NTFS volume is too large)\n");
        NtfsGlobalData->EnableWriteSupport = FALSE;
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return STATUS_NOT_IMPLEMENTED;
    }

    // convert buffer into bitmap
    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, BitmapBits.LowPart);

    // set next available bit, preferrably after 23rd bit
    MftIndex = RtlFindClearBitsAndSet(&Bitmap, 1, 24);
    if ((LONG)MftIndex == -1)
    {
        DPRINT1("Couldn't find free space in MFT for file record, increasing MFT size.\n");

        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);

        // Couldn't find a free record in the MFT, add some blank records and try again
        Status = IncreaseMftSize(DeviceExt, CanWait);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Couldn't find space in MFT for file or increase MFT size!\n");
            return Status;
        }

        return AddNewMftEntry(FileRecord, DeviceExt, DestinationIndex, CanWait);
    }

    DPRINT("Creating file record at MFT index: %I64u\n", MftIndex);

    // update file record with index
    FileRecord->MFTRecordNumber = MftIndex;

    // [BitmapData should have been updated via RtlFindClearBitsAndSet()]

    // Restore the system reserved bits
    BitmapData[2] = SystemReservedBits;

    /* FreeMftEntry already advanced this slot's sequence number. Preserve it
     * rather than the new-record template's 1, without advancing it twice. */
    {
        PFILE_RECORD_HEADER PreviousRecord;

        PreviousRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        if (PreviousRecord == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            Status = NtfsReadFileRecordEx(DeviceExt, MftIndex, PreviousRecord, TRUE);
            if (NT_SUCCESS(Status))
            {
                FileRecord->SequenceNumber = PreviousRecord->SequenceNumber;
                if (FileRecord->SequenceNumber == 0)
                    FileRecord->SequenceNumber = 1;
            }
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PreviousRecord);
        }
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
            ReleaseAttributeContext(BitmapContext);
            return Status;
        }
    }

    // write the bitmap back to the MFT's $Bitmap attribute
    Status = WriteAttribute(DeviceExt, BitmapContext, 0, BitmapData, BitmapDataSize,
                             &LengthWritten, DeviceExt->MasterFileTable);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR encountered when writing $Bitmap attribute!\n");
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return Status;
    }

    // update the file record (write it to disk)
    Status = UpdateFileRecord(DeviceExt, MftIndex, FileRecord);

    if (!NT_SUCCESS(Status))
    {
        NTSTATUS RollbackStatus;

        DPRINT1("ERROR: Unable to write file record!\n");
        RtlClearBits(&Bitmap, (ULONG)MftIndex, 1);
        RollbackStatus = WriteAttribute(DeviceExt, BitmapContext, 0, BitmapData, BitmapDataSize,
                                        &LengthWritten, DeviceExt->MasterFileTable);
        if (!NT_SUCCESS(RollbackStatus))
            DPRINT1("Couldn't roll back MFT bitmap allocation %I64u (Status %lx)\n", MftIndex, RollbackStatus);
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return Status;
    }

    *DestinationIndex = MftIndex;

    ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
    ReleaseAttributeContext(BitmapContext);

    return Status;
}

NTSTATUS
AddNewMftEntry(PFILE_RECORD_HEADER Record, PDEVICE_EXTENSION Vcb, PULONGLONG Index, BOOLEAN CanWait)
{
    NTSTATUS Status;

    KeEnterCriticalRegion();
    if (!ExAcquireResourceExclusiveLite(&Vcb->MftAllocationResource, CanWait))
    {
        KeLeaveCriticalRegion();
        return STATUS_CANT_WAIT;
    }
    _SEH2_TRY
    {
        Status = AddNewMftEntryInternal(Record, Vcb, Index, CanWait);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        ExReleaseResourceLite(&Vcb->MftAllocationResource);
        KeLeaveCriticalRegion();
        _SEH2_YIELD(ExRaiseStatus(Status));
    }
    _SEH2_END;
    ExReleaseResourceLite(&Vcb->MftAllocationResource);
    KeLeaveCriticalRegion();
    return Status;
}

/**
* @name GetMaxIndexRootSize
* @implemented
*
* Calculates how many bytes of index entries the index root can hold, given the space
* left in the file record.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param FileRecord
* Pointer to the file record containing the index root.
*
* @param IndexRootOffset
* Offset of the index root attribute within FileRecord.
*
* @param IndexRootRecord
* Pointer to the index root attribute record.
*
* @return
* The maximum size of the index root's entries, leaving room for any attributes that
* follow the index root.
*/
static
ULONG
GetMaxIndexRootSize(PDEVICE_EXTENSION DeviceExt,
                    PFILE_RECORD_HEADER FileRecord,
                    ULONG IndexRootOffset,
                    PNTFS_ATTR_RECORD IndexRootRecord)
{
    ULONG MaxIndexRootSize;
    PNTFS_ATTR_RECORD CurrentAttribute;

    // First, find the max index size assuming index root is the last attribute
    MaxIndexRootSize = DeviceExt->NtfsInfo.BytesPerFileRecord   // Start with the size of a file record
                       - IndexRootOffset                        // Subtract the length of everything that comes before index root
                       - IndexRootRecord->Resident.ValueOffset  // Subtract the length of the attribute header for index root
                       - sizeof(INDEX_ROOT_ATTRIBUTE)           // Subtract the length of the index root header
                       - (sizeof(ULONG) * 2);                   // Subtract the length of the file record end marker and padding

    // Leave room for any attributes after this one, not counting the end marker
    CurrentAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + IndexRootOffset + IndexRootRecord->Length);
    while (CurrentAttribute->Type != AttributeEnd)
    {
        MaxIndexRootSize -= CurrentAttribute->Length;
        CurrentAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)CurrentAttribute + CurrentAttribute->Length);
    }

    return MaxIndexRootSize;
}

/**
* @name InsertIndexEntryWorker
* @implemented
*
* Adds an entry to an index of a file record: a $FILE_NAME to a directory's $I30, or a view
* index entry, such as an object ID to $O of $Extend\$ObjId.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param DirectoryMftIndex
* Mft index of the file record holding the index.
*
* @param IndexName
* Name of the index ($I30, $O, ...), IndexNameLength characters long.
*
* @param Entry
* The index entry to add, without a child. A copy is made.
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
/* Index growth may commit the temporarily shortened root. Restore its original
 * record on failure; any newly orphaned allocations can then be reclaimed. */
static VOID
NtfsRestoreDirectoryRecord(PDEVICE_EXTENSION Vcb,
                           ULONGLONG MftIndex,
                           PFILE_RECORD_HEADER Record)
{
    NTSTATUS Status;

    if (Record == NULL)
        return;
    Status = UpdateFileRecord(Vcb, MftIndex, Record);
    if (!NT_SUCCESS(Status))
        DPRINT1("Unable to restore directory record %I64u: %lx\n", MftIndex, Status);
}

static NTSTATUS
NtfsRefreshIndexRootContext(PDEVICE_EXTENSION Vcb,
                            PFILE_RECORD_HEADER Record,
                            PCWSTR IndexName,
                            ULONG IndexNameLength,
                            PNTFS_ATTR_CONTEXT *Context,
                            PULONG Offset)
{
    PNTFS_ATTR_CONTEXT NewContext;
    NTSTATUS Status;

    Status = FindAttribute(Vcb, Record, AttributeIndexRoot, IndexName,
                           IndexNameLength, &NewContext, Offset);
    if (NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(*Context);
        *Context = NewContext;
    }
    return Status;
}

static
NTSTATUS
InsertIndexEntryWorker(PDEVICE_EXTENSION DeviceExt,
                       ULONGLONG DirectoryMftIndex,
                       PCWSTR IndexName,
                       ULONG IndexNameLength,
                       PINDEX_ENTRY_ATTRIBUTE Entry,
                       BOOLEAN CaseSensitive)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_RECORD_HEADER ParentFileRecord;
    PFILE_RECORD_HEADER PristineRecord = NULL;
    PNTFS_ATTR_CONTEXT IndexRootContext;
    PINDEX_ROOT_ATTRIBUTE I30IndexRoot;
    ULONG IndexRootOffset;
    ULONGLONG I30IndexRootLength;
    ULONG LengthWritten;
    PINDEX_ROOT_ATTRIBUTE NewIndexRoot;
    ULONG AttributeLength;
    PB_TREE NewTree;
    ULONG BtreeIndexLength;
    ULONG MaxIndexRootSize;
    PB_TREE_KEY NewLeftKey;
    PB_TREE_FILENAME_NODE NewRightHandNode;
    LARGE_INTEGER MinIndexRootSize;
    ULONG NewMaxIndexRootSize;
    ULONG NodeSize;

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

    // Find the index root attribute for the index
    Status = FindAttribute(DeviceExt,
                           ParentFileRecord,
                           AttributeIndexRoot,
                           IndexName,
                           IndexNameLength,
                           &IndexRootContext,
                           &IndexRootOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find %.*S $INDEX_ROOT attribute in MFT #: %I64u!\n",
                IndexNameLength, IndexName, DirectoryMftIndex);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    // Find the maximum index size given what the file record can hold
    MaxIndexRootSize = GetMaxIndexRootSize(DeviceExt, ParentFileRecord, IndexRootOffset, IndexRootContext->pRecord);

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
    // ReadAttribute returns the bytes it read
    if (ReadAttribute(DeviceExt, IndexRootContext, 0, (PCHAR)I30IndexRoot, I30IndexRootLength) != I30IndexRootLength)
    {
        Status = STATUS_UNEXPECTED_IO_ERROR;
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

    // Insert the key for the entry we're adding
    Status = NtfsInsertIndexKey(NewTree,
                                Entry,
                                NewTree->RootNode,
                                CaseSensitive,
                                MaxIndexRootSize,
                                I30IndexRoot->SizeOfEntry,
                                &NewLeftKey,
                                &NewRightHandNode);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to insert into index %.*S of MFT record %I64u (Status %lx)\n",
                IndexNameLength,
                IndexName,
                DirectoryMftIndex,
                Status);
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

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

    PristineRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (PristineRecord == NULL)
    {
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(PristineRecord, ParentFileRecord, DeviceExt->NtfsInfo.BytesPerFileRecord);

    // Temporarily shrink the index root to it's minimal size
    AttributeLength = MinIndexRootSize.LowPart;


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
        NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
        if (PristineRecord != NULL)
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
        return Status;
    }

    // Update the index allocation
    Status = UpdateIndexAllocation(DeviceExt, NewTree, I30IndexRoot->SizeOfEntry, ParentFileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsRefreshIndexRootContext(DeviceExt, ParentFileRecord, IndexName,
                                             IndexNameLength, &IndexRootContext, &IndexRootOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update index allocation from B-Tree!\n");
        DestroyBTree(NewTree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
        if (PristineRecord != NULL)
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
        return Status;
    }

#ifndef NDEBUG
    DPRINT1("Index Allocation updated\n");
    DumpBTree(NewTree);
#endif

    // Find the maximum index root size given what the file record can hold
    NewMaxIndexRootSize = GetMaxIndexRootSize(DeviceExt, ParentFileRecord, IndexRootOffset, IndexRootContext->pRecord);

    // The index allocation and index bitmap may have grown, leaving less room for the index root,
    // so now we need to double-check that index root isn't too large
    NodeSize = GetSizeOfIndexEntries(NewTree->RootNode);
    if (NodeSize > NewMaxIndexRootSize)
    {
        DPRINT("Demoting index root.\nNodeSize: 0x%lx\nNewMaxIndexRootSize: 0x%lx\n", NodeSize, NewMaxIndexRootSize);

        Status = DemoteBTreeRoot(NewTree);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to demote index root!\n");
            DestroyBTree(NewTree);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
            if (PristineRecord != NULL)
                ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
            return Status;
        }

        // We need to update the index allocation once more
        Status = UpdateIndexAllocation(DeviceExt, NewTree, I30IndexRoot->SizeOfEntry, ParentFileRecord);
        if (NT_SUCCESS(Status))
            Status = NtfsRefreshIndexRootContext(DeviceExt, ParentFileRecord, IndexName,
                                                 IndexNameLength, &IndexRootContext, &IndexRootOffset);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to update index allocation from B-Tree!\n");
            DestroyBTree(NewTree);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
            if (PristineRecord != NULL)
                ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
            return Status;
        }

        // re-recalculate max size of index root
        NewMaxIndexRootSize = GetMaxIndexRootSize(DeviceExt, ParentFileRecord, IndexRootOffset, IndexRootContext->pRecord);
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
        NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
        if (PristineRecord != NULL)
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
        return Status;
    }

    // We're done with the B-Tree now
    DestroyBTree(NewTree);

    // Write back the new index root attribute to the parent directory file record

    // First, we need to resize the attribute.
    // CreateIndexRootFromBTree() should have verified that the index root fits within MaxIndexSize.
    // We can't set the size as we normally would, because $INDEX_ROOT must always be resident.
    AttributeLength = NewIndexRoot->Header.AllocatedSize + FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header);

    if (AttributeLength != IndexRootContext->pRecord->Resident.ValueLength)
    {
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
            NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
            if (PristineRecord != NULL)
                ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
            return Status;
        }

    }

    NT_ASSERT(ParentFileRecord->BytesInUse <= DeviceExt->NtfsInfo.BytesPerFileRecord);

    Status = UpdateFileRecord(DeviceExt, DirectoryMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update file record of directory with index: %llx\n", DirectoryMftIndex);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
        if (PristineRecord != NULL)
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
        return Status;
    }

    // Write the new index root to disk
    Status = WriteAttribute(DeviceExt,
                            IndexRootContext,
                            0,
                            (PUCHAR)NewIndexRoot,
                            AttributeLength,
                            &LengthWritten,
                            ParentFileRecord);
    if (!NT_SUCCESS(Status) || LengthWritten != AttributeLength)
    {
        DPRINT1("ERROR: Unable to write new index root attribute to parent directory!\n");
        ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
        if (PristineRecord != NULL)
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
        return Status;
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
    PristineRecord = NULL;

    // re-read the parent file record, so we can dump it
    Status = ReadFileRecord(DeviceExt, DirectoryMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't read parent directory after messing with it!\n");
    }
    else
    {
#ifndef NDEBUG
        DPRINT1("Dumping new B-Tree:\n");

        Status = CreateBTreeFromIndex(DeviceExt, ParentFileRecord, IndexRootContext, NewIndexRoot, &NewTree);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Couldn't re-create b-tree\n");
            NtfsRestoreDirectoryRecord(DeviceExt, DirectoryMftIndex, PristineRecord);
            if (PristineRecord != NULL)
                ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, PristineRecord);
            return Status;
        }

        DumpBTree(NewTree);

        DestroyBTree(NewTree);

        NtfsDumpFileRecord(DeviceExt, ParentFileRecord);
#endif
    }

    // Cleanup
    ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
    ReleaseAttributeContext(IndexRootContext);
    ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);

    return Status;
}

/* The worker rewrites the record from a copy it reads, so it must not
 * overlap any other update of an index */
NTSTATUS
NtfsInsertIndexEntry(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG MftIndex,
                     PCWSTR IndexName,
                     ULONG IndexNameLength,
                     PINDEX_ENTRY_ATTRIBUTE Entry,
                     BOOLEAN CaseSensitive)
{
    NTSTATUS Status;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&DeviceExt->IndexResource, TRUE);
    Status = InsertIndexEntryWorker(DeviceExt, MftIndex, IndexName, IndexNameLength, Entry, CaseSensitive);
    ExReleaseResourceLite(&DeviceExt->IndexResource);
    KeLeaveCriticalRegion();

    return Status;
}

/**
* @name NtfsAddFilenameToDirectory
* @implemented
*
* Adds a $FILE_NAME attribute to a given directory index.
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
* @remarks
* One FILENAME_ATTRIBUTE is added to the directory's index for each link to that file. So, each
* file which contains one FILENAME_ATTRIBUTE for a long name and another for the 8.3 name, will
* get both attributes added to its parent directory.
*/
NTSTATUS
NtfsAddFilenameToDirectory(PDEVICE_EXTENSION DeviceExt,
                           ULONGLONG DirectoryMftIndex,
                           ULONGLONG FileReferenceNumber,
                           PFILENAME_ATTRIBUTE FilenameAttribute,
                           BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    PB_TREE_KEY FileNameKey = CreateBTreeKeyFromFilename(FileReferenceNumber, FilenameAttribute);

    if (!FileNameKey)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsInsertIndexEntry(DeviceExt, DirectoryMftIndex, L"$I30", 4, FileNameKey->IndexEntry, CaseSensitive);

    DestroyBTreeKey(FileNameKey);

    return Status;
}

/**
* @name NtfsRemoveFilenameFromDirectory
* @implemented
*
* Removes a $FILE_NAME attribute from a given directory index.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param DirectoryMftIndex
* Mft index of the parent directory the file is being removed from.
*
* @param FilenameAttribute
* Pointer to the FILENAME_ATTRIBUTE of the file being removed from the directory.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_OBJECT_NAME_NOT_FOUND if the name isn't in the directory's index.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
*
* @remarks
* The inverse of NtfsAddFilenameToDirectory(). One $FILE_NAME attribute is indexed per link, so
* a file carrying both a long name and an 8.3 name needs one call for each.
*/
static
NTSTATUS
RemoveIndexEntryWorker(PDEVICE_EXTENSION DeviceExt,
                       ULONGLONG DirectoryMftIndex,
                       PCWSTR IndexName,
                       ULONG IndexNameLength,
                       PINDEX_ENTRY_ATTRIBUTE Entry,
                       BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER ParentFileRecord;
    PNTFS_ATTR_CONTEXT IndexRootContext;
    PINDEX_ROOT_ATTRIBUTE I30IndexRoot;
    PINDEX_ROOT_ATTRIBUTE NewIndexRoot;
    ULONG IndexRootOffset;
    ULONGLONG I30IndexRootLength;
    ULONG LengthWritten;
    ULONG AttributeLength;
    ULONG BtreeIndexLength;
    ULONG MaxIndexRootSize;
    PB_TREE Tree;
    PB_TREE_KEY SearchKey;

    DPRINT("RemoveIndexEntryWorker(%p, %I64u, %.*S, %p, %s)\n",
           DeviceExt, DirectoryMftIndex, IndexNameLength, IndexName, Entry, CaseSensitive ? "TRUE" : "FALSE");

    ParentFileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!ParentFileRecord)
    {
        DPRINT1("ERROR: Couldn't allocate memory for file record!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, DirectoryMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't read parent directory with index %I64u\n", DirectoryMftIndex);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    Status = FindAttribute(DeviceExt,
                           ParentFileRecord,
                           AttributeIndexRoot,
                           IndexName,
                           IndexNameLength,
                           &IndexRootContext,
                           &IndexRootOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find %.*S $INDEX_ROOT attribute in MFT #: %I64u!\n",
                IndexNameLength, IndexName, DirectoryMftIndex);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    I30IndexRootLength = AttributeDataLength(IndexRootContext->pRecord);
    I30IndexRoot = ExAllocatePoolWithTag(NonPagedPool, I30IndexRootLength, TAG_NTFS);
    if (!I30IndexRoot)
    {
        DPRINT1("ERROR: Couldn't allocate memory for index root attribute!\n");
        ReleaseAttributeContext(IndexRootContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // ReadAttribute returns the bytes it read
    if (ReadAttribute(DeviceExt, IndexRootContext, 0, (PCHAR)I30IndexRoot, I30IndexRootLength) != I30IndexRootLength)
    {
        Status = STATUS_UNEXPECTED_IO_ERROR;
        DPRINT1("ERROR: Couldn't read index root attribute for Mft index #%I64u\n", DirectoryMftIndex);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    Status = CreateBTreeFromIndex(DeviceExt, ParentFileRecord, IndexRootContext, I30IndexRoot, &Tree);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to create B-Tree from Index!\n");
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    // NtfsRemoveKey() compares only the key, so a $FILE_NAME's file reference is only a placeholder
    SearchKey = CreateBTreeKeyFromEntry(Entry);
    if (!SearchKey)
    {
        DestroyBTree(Tree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsRemoveKey(Tree, Tree->RootNode, SearchKey, CaseSensitive);

    DestroyBTreeKey(SearchKey);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to remove a key from index %.*S of MFT record %I64u (Status %lx)\n",
                IndexNameLength,
                IndexName,
                DirectoryMftIndex,
                Status);
        DestroyBTree(Tree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    // Push the tree back out to the index allocation before sizing the index root, for the same
    // reason the insert path does: growing or shrinking the allocation changes how much of the
    // file record is left for the root.
    Status = UpdateIndexAllocation(DeviceExt, Tree, I30IndexRoot->SizeOfEntry, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update index allocation from B-Tree!\n");
        DestroyBTree(Tree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    // Find the maximum index root size given what the file record can hold
    MaxIndexRootSize = GetMaxIndexRootSize(DeviceExt, ParentFileRecord, IndexRootOffset, IndexRootContext->pRecord);

    Status = CreateIndexRootFromBTree(DeviceExt, Tree, MaxIndexRootSize, &NewIndexRoot, &BtreeIndexLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to create Index root from B-Tree!\n");
        DestroyBTree(Tree);
        ReleaseAttributeContext(IndexRootContext);
        ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
        return Status;
    }

    DestroyBTree(Tree);

    // $INDEX_ROOT must always be resident, so its length is set directly rather than through
    // the usual attribute-resizing path.
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
            DPRINT1("ERROR: Unable to set resident attribute length!\n");
            ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
            ReleaseAttributeContext(IndexRootContext);
            ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
            return Status;
        }
    }

    NT_ASSERT(ParentFileRecord->BytesInUse <= DeviceExt->NtfsInfo.BytesPerFileRecord);

    Status = UpdateFileRecord(DeviceExt, DirectoryMftIndex, ParentFileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update file record of directory with index: %I64x\n", DirectoryMftIndex);
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
    if (!NT_SUCCESS(Status) || LengthWritten != AttributeLength)
    {
        DPRINT1("ERROR: Unable to write new index root attribute to parent directory!\n");
        if (NT_SUCCESS(Status))
            Status = STATUS_UNSUCCESSFUL;
    }

    ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
    ReleaseAttributeContext(IndexRootContext);
    ExFreePoolWithTag(I30IndexRoot, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);

    return Status;
}

/* The worker rewrites the record from a copy it reads, so it must not
 * overlap any other update of an index */
NTSTATUS
NtfsRemoveIndexEntry(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG MftIndex,
                     PCWSTR IndexName,
                     ULONG IndexNameLength,
                     PINDEX_ENTRY_ATTRIBUTE Entry,
                     BOOLEAN CaseSensitive)
{
    NTSTATUS Status;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&DeviceExt->IndexResource, TRUE);
    Status = RemoveIndexEntryWorker(DeviceExt, MftIndex, IndexName, IndexNameLength, Entry, CaseSensitive);
    ExReleaseResourceLite(&DeviceExt->IndexResource);
    KeLeaveCriticalRegion();

    return Status;
}

NTSTATUS
NtfsRemoveFilenameFromDirectory(PDEVICE_EXTENSION DeviceExt,
                                ULONGLONG DirectoryMftIndex,
                                PFILENAME_ATTRIBUTE FilenameAttribute,
                                BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    PB_TREE_KEY FileNameKey = CreateBTreeKeyFromFilename(0, FilenameAttribute);

    if (!FileNameKey)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsRemoveIndexEntry(DeviceExt, DirectoryMftIndex, L"$I30", 4, FileNameKey->IndexEntry, CaseSensitive);

    DestroyBTreeKey(FileNameKey);

    return Status;
}

/* What a read-only walk of an index is after */
typedef enum _NTFS_INDEX_QUERY_KIND
{
    NtfsIndexFindKey,
    NtfsIndexFindLastKey,
    NtfsIndexVisitRange
} NTFS_INDEX_QUERY_KIND;

typedef struct _NTFS_INDEX_QUERY
{
    NTFS_INDEX_QUERY_KIND Kind;
    PINDEX_ENTRY_ATTRIBUTE SearchEntry;     /* the key, or the low end of the range */
    PINDEX_ENTRY_ATTRIBUTE HighEntry;       /* the high end of the range */
    PNTFS_INDEX_VISITOR Visitor;
    PVOID Context;
    PINDEX_ENTRY_ATTRIBUTE FoundEntry;      /* a copy of the entry found */
} NTFS_INDEX_QUERY, *PNTFS_INDEX_QUERY;

/**
* @name CopyIndexEntry
*
* A pool copy of an index entry without the VCN of its child, which callers don't look at.
*/
static
PINDEX_ENTRY_ATTRIBUTE
CopyIndexEntry(PINDEX_ENTRY_ATTRIBUTE Entry)
{
    PINDEX_ENTRY_ATTRIBUTE Copy;
    USHORT Length = Entry->Length;

    if (BooleanFlagOn(Entry->Flags, NTFS_INDEX_ENTRY_NODE))
        Length -= sizeof(ULONGLONG);

    Copy = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);
    if (!Copy)
        return NULL;

    RtlCopyMemory(Copy, Entry, Length);
    Copy->Length = Length;
    Copy->Flags = 0;

    return Copy;
}

/**
* @name QueryIndexTree
*
* Answers an index query from the B-Tree of the index.
*/
static
NTSTATUS
QueryIndexTree(PB_TREE Tree,
               PNTFS_INDEX_QUERY Query)
{
    PB_TREE_KEY SearchKey = NULL, HighKey = NULL, FoundKey = NULL;
    BOOLEAN Stopped = FALSE;
    NTSTATUS Status;

    if (Query->SearchEntry)
    {
        SearchKey = CreateBTreeKeyFromEntry(Query->SearchEntry);
        if (!SearchKey)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    switch (Query->Kind)
    {
        case NtfsIndexFindKey:
            Status = NtfsFindKey(Tree, Tree->RootNode, SearchKey, FALSE, &FoundKey);
            break;

        case NtfsIndexFindLastKey:
            Status = NtfsFindLastKey(Tree, &FoundKey);
            if (NT_SUCCESS(Status) && FoundKey == NULL)
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
            break;

        default:
            ASSERT(Query->Kind == NtfsIndexVisitRange);
            HighKey = CreateBTreeKeyFromEntry(Query->HighEntry);
            if (!HighKey)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            Status = NtfsVisitKeys(Tree, Tree->RootNode, SearchKey, HighKey, Query->Visitor, Query->Context, &Stopped);
            break;
    }

    if (NT_SUCCESS(Status) && FoundKey)
    {
        Query->FoundEntry = CopyIndexEntry(FoundKey->IndexEntry);
        if (!Query->FoundEntry)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (SearchKey)
        DestroyBTreeKey(SearchKey);
    if (HighKey)
        DestroyBTreeKey(HighKey);

    return Status;
}

/**
* @name QueryIndexWorker
*
* Builds the B-Tree of an index of a file record and answers a query from it.
*/
static
NTSTATUS
QueryIndexWorker(PDEVICE_EXTENSION DeviceExt,
                 PFILE_RECORD_HEADER FileRecord,
                 PCWSTR IndexName,
                 ULONG IndexNameLength,
                 PNTFS_INDEX_QUERY Query)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT IndexRootContext;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    ULONG IndexRootLength;
    PB_TREE Tree;

    Status = FindAttribute(DeviceExt, FileRecord, AttributeIndexRoot, IndexName, IndexNameLength, &IndexRootContext, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    IndexRootLength = AttributeDataLength(IndexRootContext->pRecord);
    IndexRoot = ExAllocatePoolWithTag(NonPagedPool, IndexRootLength, TAG_NTFS);
    if (!IndexRoot)
    {
        ReleaseAttributeContext(IndexRootContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ReadAttribute(DeviceExt, IndexRootContext, 0, (PCHAR)IndexRoot, IndexRootLength) != IndexRootLength)
        Status = STATUS_FILE_CORRUPT_ERROR;
    else
        Status = CreateBTreeFromIndex(DeviceExt, FileRecord, IndexRootContext, IndexRoot, &Tree);

    if (NT_SUCCESS(Status))
    {
        Status = QueryIndexTree(Tree, Query);
        DestroyBTree(Tree);
    }

    ExFreePoolWithTag(IndexRoot, TAG_NTFS);
    ReleaseAttributeContext(IndexRootContext);
    return Status;
}

/**
* @name QueryIndex
*
* Answers a query about an index of the file record MftIndex, with the index held still.
*/
static
NTSTATUS
QueryIndex(PDEVICE_EXTENSION DeviceExt,
           ULONGLONG MftIndex,
           PCWSTR IndexName,
           ULONG IndexNameLength,
           PNTFS_INDEX_QUERY Query)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER FileRecord;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!FileRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&DeviceExt->IndexResource, TRUE);

    Status = ReadFileRecord(DeviceExt, MftIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = QueryIndexWorker(DeviceExt, FileRecord, IndexName, IndexNameLength, Query);

    ExReleaseResourceLite(&DeviceExt->IndexResource);
    KeLeaveCriticalRegion();

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsFindIndexEntry
* @implemented
*
* Looks a key up in an index of a file record.
*
* @param MftIndex
* Mft index of the file record holding the index.
*
* @param SearchEntry
* An entry carrying the key to look up, as NtfsAllocateViewIndexEntry() builds it.
*
* @param FoundEntry
* Receives a copy of the matching entry, which the caller frees with ExFreePoolWithTag(TAG_NTFS).
*
* @return
* STATUS_SUCCESS if the key is in the index, STATUS_OBJECT_NAME_NOT_FOUND if it isn't.
*/
NTSTATUS
NtfsFindIndexEntry(PDEVICE_EXTENSION DeviceExt,
                   ULONGLONG MftIndex,
                   PCWSTR IndexName,
                   ULONG IndexNameLength,
                   PINDEX_ENTRY_ATTRIBUTE SearchEntry,
                   PINDEX_ENTRY_ATTRIBUTE *FoundEntry)
{
    NTFS_INDEX_QUERY Query = {NtfsIndexFindKey, SearchEntry, NULL, NULL, NULL, NULL};
    NTSTATUS Status;

    Status = QueryIndex(DeviceExt, MftIndex, IndexName, IndexNameLength, &Query);
    *FoundEntry = Query.FoundEntry;

    return Status;
}

/**
* @name NtfsFindLastIndexEntry
* @implemented
*
* Like NtfsFindIndexEntry(), for the greatest key of the index: STATUS_OBJECT_NAME_NOT_FOUND
* if the index is empty.
*/
NTSTATUS
NtfsFindLastIndexEntry(PDEVICE_EXTENSION DeviceExt,
                       ULONGLONG MftIndex,
                       PCWSTR IndexName,
                       ULONG IndexNameLength,
                       PINDEX_ENTRY_ATTRIBUTE *FoundEntry)
{
    NTFS_INDEX_QUERY Query = {NtfsIndexFindLastKey, NULL, NULL, NULL, NULL, NULL};
    NTSTATUS Status;

    Status = QueryIndex(DeviceExt, MftIndex, IndexName, IndexNameLength, &Query);
    *FoundEntry = Query.FoundEntry;

    return Status;
}

/**
* @name NtfsVisitIndexEntries
* @implemented
*
* Calls Visitor, in key order, for each entry of an index whose key is between the keys of
* LowEntry and HighEntry, inclusive, until it returns FALSE. The index can't change meanwhile.
*/
NTSTATUS
NtfsVisitIndexEntries(PDEVICE_EXTENSION DeviceExt,
                      ULONGLONG MftIndex,
                      PCWSTR IndexName,
                      ULONG IndexNameLength,
                      PINDEX_ENTRY_ATTRIBUTE LowEntry,
                      PINDEX_ENTRY_ATTRIBUTE HighEntry,
                      PNTFS_INDEX_VISITOR Visitor,
                      PVOID Context)
{
    NTFS_INDEX_QUERY Query = {NtfsIndexVisitRange, LowEntry, HighEntry, Visitor, Context, NULL};

    return QueryIndex(DeviceExt, MftIndex, IndexName, IndexNameLength, &Query);
}

/**
* @name FreeMftEntry
* @implemented
*
* Releases an MFT record back to the volume, clearing its bit in the master file table's $Bitmap
* attribute and marking the record itself as no longer in use.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param MftIndex
* Mft index of the record to release.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_OBJECT_NAME_NOT_FOUND if the master file table has no $Bitmap attribute.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
*
* @remarks
* The record's sequence number is incremented before it's written back, so any file reference
* still pointing at the old incarnation can be recognised as stale.
*/
static NTSTATUS
FreeMftEntryInternal(PDEVICE_EXTENSION DeviceExt,
             ULONGLONG MftIndex)
{
    NTSTATUS Status;
    RTL_BITMAP Bitmap;
    ULONGLONG BitmapDataSize;
    ULONGLONG AttrBytesRead;
    PUCHAR BitmapData;
    PUCHAR BitmapBuffer;
    ULONG LengthWritten;
    PNTFS_ATTR_CONTEXT BitmapContext;
    LARGE_INTEGER BitmapBits;
    PFILE_RECORD_HEADER FileRecord;

    DPRINT("FreeMftEntry(%p, %I64u)\n", DeviceExt, MftIndex);

    // Mark the record itself as free first, so a bitmap bit is never handed out for a record
    // that still looks in use.
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!FileRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, MftIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't read file record %I64u to free it!\n", MftIndex);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    FileRecord->Flags &= ~FRH_IN_USE;
    FileRecord->SequenceNumber++;

    // Sequence number zero is reserved to mean "no reference"
    if (FileRecord->SequenceNumber == 0)
        FileRecord->SequenceNumber = 1;

    Status = UpdateFileRecord(DeviceExt, MftIndex, FileRecord);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't write file record %I64u while freeing it!\n", MftIndex);
        return Status;
    }

    Status = FindAttribute(DeviceExt, DeviceExt->MasterFileTable, AttributeBitmap, L"", 0, &BitmapContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't find $Bitmap attribute of master file table!\n");
        return Status;
    }

    BitmapDataSize = AttributeDataLength(BitmapContext->pRecord);

    // RtlInitializeBitmap wants a ULONG-aligned pointer and a ULONG-multiple of memory
    BitmapBuffer = ExAllocatePoolWithTag(NonPagedPool, BitmapDataSize + sizeof(ULONG), TAG_NTFS);
    if (!BitmapBuffer)
    {
        ReleaseAttributeContext(BitmapContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(BitmapBuffer, BitmapDataSize + sizeof(ULONG));

    BitmapData = (PUCHAR)ALIGN_UP_BY((ULONG_PTR)BitmapBuffer, sizeof(ULONG));

    AttrBytesRead = ReadAttribute(DeviceExt, BitmapContext, 0, (PCHAR)BitmapData, BitmapDataSize);
    if (AttrBytesRead != BitmapDataSize)
    {
        DPRINT1("ERROR: Unable to read $Bitmap attribute of master file table!\n");
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    BitmapBits.QuadPart = AttributeDataLength(DeviceExt->MFTContext->pRecord) /
                          DeviceExt->NtfsInfo.BytesPerFileRecord;
    if (BitmapBits.HighPart != 0)
    {
        DPRINT1("\tFIXME: bitmap sizes beyond 32bits are not yet supported! (Your NTFS volume is too large)\n");
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (MftIndex >= BitmapBits.QuadPart)
    {
        DPRINT1("ERROR: Mft index %I64u is beyond the end of the master file table!\n", MftIndex);
        ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        return STATUS_INVALID_PARAMETER;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, BitmapBits.LowPart);
    RtlClearBits(&Bitmap, (ULONG)MftIndex, 1);

    Status = WriteAttribute(DeviceExt, BitmapContext, 0, BitmapData, BitmapDataSize, &LengthWritten, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR encountered when writing $Bitmap attribute!\n");
    }

    ExFreePoolWithTag(BitmapBuffer, TAG_NTFS);
    ReleaseAttributeContext(BitmapContext);

    return Status;
}

NTSTATUS
FreeMftEntry(PDEVICE_EXTENSION Vcb, ULONGLONG Index)
{
    NTSTATUS Status;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->MftAllocationResource, TRUE);
    _SEH2_TRY
    {
        Status = FreeMftEntryInternal(Vcb, Index);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        ExReleaseResourceLite(&Vcb->MftAllocationResource);
        KeLeaveCriticalRegion();
        _SEH2_YIELD(ExRaiseStatus(Status));
    }
    _SEH2_END;
    ExReleaseResourceLite(&Vcb->MftAllocationResource);
    KeLeaveCriticalRegion();
    return Status;
}

static NTSTATUS NtfsFreeRecordClusters(PDEVICE_EXTENSION DeviceExt, PFILE_RECORD_HEADER FileRecord);

/**
* @name NtfsDeleteFileRecord
* @implemented
*
* Deletes a file or empty directory: releases every cluster its non-resident attributes hold,
* removes each of its names from the parent index, and frees its MFT record.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param MftIndex
* Mft index of the file to delete.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @return
* STATUS_SUCCESS on success.
*
* @remarks
* Callers must have established that the file may be deleted, in particular that a directory
* is empty. The names leave the index before anything is freed, so a failure there leaves the
* file whole; then the extension records, the clusters of the base record's attributes and
* the record itself are released.
*/
NTSTATUS
NtfsDeleteFileRecord(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG MftIndex,
                     BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER FileRecord;
    PFILENAME_ATTRIBUTE *Names = NULL;
    ULONG NameCount = 0, i;

    DPRINT("NtfsDeleteFileRecord(%p, %I64u, %s)\n", DeviceExt, MftIndex, CaseSensitive ? "TRUE" : "FALSE");

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (!FileRecord)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, MftIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't read file record %I64u to delete it!\n", MftIndex);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // Remove every name this file has from its parent's index before anything is freed, so a
    // failure leaves the file whole instead of an entry pointing at a released record. A file
    // with both a long name and an 8.3 name is indexed twice, a file with several links once
    // per link, and the names can be in extension records.
    Status = NtfsGetFileNames(DeviceExt, FileRecord, &Names, &NameCount);
    for (i = 0; NT_SUCCESS(Status) && i < NameCount; i++)
    {
        Status = NtfsRemoveFilenameFromDirectory(DeviceExt,
                                                 Names[i]->DirectoryFileReferenceNumber & NTFS_MFT_MASK,
                                                 Names[i],
                                                 CaseSensitive);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to remove '%.*S' from directory %I64u!\n",
                    Names[i]->NameLength,
                    Names[i]->Name,
                    Names[i]->DirectoryFileReferenceNumber & NTFS_MFT_MASK);
        }
    }

    if (Names != NULL)
        NtfsFreeFileNames(Names, NameCount);

    // Its object ID is free for another file now
    if (NT_SUCCESS(Status))
        Status = NtfsReleaseObjectId(DeviceExt, FileRecord);

    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    /* The extension records go with the base record. They are found through the
     * $ATTRIBUTE_LIST, so they go before the list's own clusters do. */
    if (NtfsRecordHasAttributeList(FileRecord))
    {
        Status = NtfsDeleteExtensionRecords(DeviceExt, FileRecord);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to free the extension records of file %I64u!\n", MftIndex);
        }
    }

    /* Then the clusters of every non-resident attribute of the base record. That
     * includes a non-resident $ATTRIBUTE_LIST, which FindAttribute() never returns. */
    Status = NtfsFreeRecordClusters(DeviceExt, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to free the clusters of file %I64u!\n", MftIndex);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return FreeMftEntry(DeviceExt, MftIndex);
}

/**
* @name NtfsGetFileNames
* @implemented
*
* Collects copies of every $FILE_NAME of a file, in its base record and, through its
* $ATTRIBUTE_LIST, its extension records: one per link, plus a DOS name where the file
* has one.
*
* @param Names
* Receives a pool array (TAG_NTFS) of pool copies, freed with NtfsFreeFileNames().
*
* @param Count
* Receives the number of names.
*/
NTSTATUS
NtfsGetFileNames(PDEVICE_EXTENSION DeviceExt,
                 PFILE_RECORD_HEADER BaseRecord,
                 PFILENAME_ATTRIBUTE **Names,
                 PULONG Count)
{
    PNTFS_ATTRIBUTE_LIST_ITEM List = NULL;
    PULONGLONG Indices = NULL;
    PFILE_RECORD_HEADER Record = BaseRecord;
    ULONG ListLength = 0, ExtensionCount = 0, RecordIndex = 0;
    ULONG Capacity = 16;
    NTSTATUS Status = STATUS_SUCCESS;

    *Count = 0;
    *Names = ExAllocatePoolWithTag(NonPagedPool, Capacity * sizeof(PFILENAME_ATTRIBUTE), TAG_NTFS);
    if (*Names == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (NtfsRecordHasAttributeList(BaseRecord))
    {
        Status = NtfsReadAttributeList(DeviceExt, BaseRecord, &List, &ListLength);
        if (NT_SUCCESS(Status))
            Status = NtfsGetExtensionRecords(List, ListLength, BaseRecord->MFTRecordNumber, &Indices, &ExtensionCount);
    }

    while (NT_SUCCESS(Status))
    {
        PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Record + Record->AttributeOffset);

        while (Attribute->Type != AttributeEnd && NT_SUCCESS(Status))
        {
            if (Attribute->Type == AttributeFileName && !Attribute->IsNonResident)
            {
                PFILENAME_ATTRIBUTE Copy;

                if (*Count == Capacity)
                {
                    PFILENAME_ATTRIBUTE *Bigger = ExAllocatePoolWithTag(NonPagedPool, 2 * Capacity * sizeof(PFILENAME_ATTRIBUTE), TAG_NTFS);

                    if (Bigger == NULL)
                    {
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        break;
                    }

                    RtlCopyMemory(Bigger, *Names, Capacity * sizeof(PFILENAME_ATTRIBUTE));
                    ExFreePoolWithTag(*Names, TAG_NTFS);
                    *Names = Bigger;
                    Capacity *= 2;
                }

                Copy = ExAllocatePoolWithTag(NonPagedPool, Attribute->Resident.ValueLength, TAG_NTFS);
                if (Copy == NULL)
                {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }

                RtlCopyMemory(Copy, (PUCHAR)Attribute + Attribute->Resident.ValueOffset, Attribute->Resident.ValueLength);
                (*Names)[(*Count)++] = Copy;
            }

            Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
        }

        if (!NT_SUCCESS(Status) || RecordIndex == ExtensionCount)
            break;

        // On to the next extension record
        if (Record == BaseRecord)
        {
            Record = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
            if (Record == NULL)
            {
                Record = BaseRecord;
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
        }

        Status = ReadFileRecord(DeviceExt, Indices[RecordIndex++], Record);
    }

    if (Record != BaseRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Record);
    if (Indices != NULL)
        ExFreePoolWithTag(Indices, TAG_NTFS);
    if (List != NULL)
        ExFreePoolWithTag(List, TAG_NTFS);

    if (!NT_SUCCESS(Status))
    {
        NtfsFreeFileNames(*Names, *Count);
        *Names = NULL;
        *Count = 0;
    }

    return Status;
}

VOID
NtfsFreeFileNames(PFILENAME_ATTRIBUTE *Names,
                  ULONG Count)
{
    ULONG i;

    for (i = 0; i < Count; i++)
        ExFreePoolWithTag(Names[i], TAG_NTFS);

    ExFreePoolWithTag(Names, TAG_NTFS);
}

/* Whether a $FILE_NAME is Name in the directory ParentIndex */
static
BOOLEAN
NtfsFileNameMatches(PDEVICE_EXTENSION DeviceExt,
                    PFILENAME_ATTRIBUTE FileName,
                    ULONGLONG ParentIndex,
                    PCUNICODE_STRING Name)
{
    return ((FileName->DirectoryFileReferenceNumber & NTFS_MFT_MASK) == ParentIndex &&
            FileName->NameLength * sizeof(WCHAR) == Name->Length &&
            NtfsAttributeNamesEqual(DeviceExt, FileName->Name, Name->Buffer, FileName->NameLength));
}

/**
* @name NtfsFindFileNameRecord
*
* Finds the record and attribute offset of a particular link. Release Holder with
* NtfsReleaseAttributeHolder; the base record is borrowed, extension buffers are owned.
*/
static NTSTATUS
NtfsFindFileNameRecordInternal(PDEVICE_EXTENSION DeviceExt,
                       PFILE_RECORD_HEADER BaseRecord,
                       ULONGLONG ParentIndex,
                       PCUNICODE_STRING Name,
                       PFILE_RECORD_HEADER *Holder,
                       PULONG Offset,
                       BOOLEAN ExactCase)
{
    PNTFS_ATTRIBUTE_LIST_ITEM List = NULL;
    PULONGLONG Indices = NULL;
    PFILE_RECORD_HEADER Record = BaseRecord;
    ULONG ListLength = 0, ExtensionCount = 0, RecordIndex = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN Found = FALSE;

    *Holder = NULL;

    if (NtfsRecordHasAttributeList(BaseRecord))
    {
        Status = NtfsReadAttributeList(DeviceExt, BaseRecord, &List, &ListLength);
        if (NT_SUCCESS(Status))
            Status = NtfsGetExtensionRecords(List, ListLength, BaseRecord->MFTRecordNumber, &Indices, &ExtensionCount);
    }

    while (NT_SUCCESS(Status) && !Found)
    {
        PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Record + Record->AttributeOffset);

        while (Attribute->Type != AttributeEnd)
        {
            if (Attribute->Type == AttributeFileName &&
                NtfsFileNameMatches(DeviceExt,
                                    (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset),
                                    ParentIndex,
                                    Name) &&
                (!ExactCase || RtlCompareMemory((PUCHAR)Attribute + Attribute->Resident.ValueOffset +
                                               FIELD_OFFSET(FILENAME_ATTRIBUTE, Name),
                                              Name->Buffer, Name->Length) == Name->Length))
            {
                *Offset = (ULONG)((PUCHAR)Attribute - (PUCHAR)Record);
                Found = TRUE;
                break;
            }

            Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);
        }

        if (Found || RecordIndex == ExtensionCount)
            break;

        if (Record == BaseRecord)
        {
            Record = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
            if (Record == NULL)
            {
                Record = BaseRecord;
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
        }

        Status = ReadFileRecord(DeviceExt, Indices[RecordIndex++], Record);
    }

    if (NT_SUCCESS(Status) && !Found)
        Status = STATUS_OBJECT_NAME_NOT_FOUND;

    if (NT_SUCCESS(Status))
    {
        *Holder = Record;
        Record = BaseRecord; /* The caller owns an extension buffer on success. */
    }

    if (Record != BaseRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Record);
    if (Indices != NULL)
        ExFreePoolWithTag(Indices, TAG_NTFS);
    if (List != NULL)
        ExFreePoolWithTag(List, TAG_NTFS);

    return Status;
}

NTSTATUS
NtfsFindFileNameRecord(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER BaseRecord,
                       ULONGLONG ParentIndex,
                       PCUNICODE_STRING Name,
                       PFILE_RECORD_HEADER *Holder,
                       PULONG Offset)
{
    NTSTATUS Status;

    /* During a case-only rename both spellings can be staged in the list.
     * Prefer the counted exact spelling across all records before folding. */
    Status = NtfsFindFileNameRecordInternal(Vcb, BaseRecord, ParentIndex, Name, Holder, Offset, TRUE);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        Status = NtfsFindFileNameRecordInternal(Vcb, BaseRecord, ParentIndex, Name, Holder, Offset, FALSE);
    return Status;
}

NTSTATUS
NtfsRemoveFileNameAttribute(PDEVICE_EXTENSION Vcb,
                            PFILE_RECORD_HEADER BaseRecord,
                            ULONGLONG ParentIndex,
                            PCUNICODE_STRING Name)
{
    PFILE_RECORD_HEADER Holder;
    ULONG Offset;
    NTSTATUS Status;

    Status = NtfsFindFileNameRecord(Vcb, BaseRecord, ParentIndex, Name, &Holder, &Offset);
    if (!NT_SUCCESS(Status))
        return Status;
    NtfsRemoveAttribute(Holder, Offset);
    if (Holder != BaseRecord)
    {
        Status = UpdateFileRecord(Vcb, Holder->MFTRecordNumber, Holder);
    }
    /* Base-record names also have list entries. Removing one invalidates
     * its instance entry even when an extension record did not change. */
    if (NT_SUCCESS(Status) && NtfsRecordHasAttributeList(BaseRecord))
        Status = NtfsRebuildAttributeList(Vcb, BaseRecord, NULL, 0);
    NtfsReleaseAttributeHolder(Vcb, BaseRecord, Holder);
    return Status;
}

/**
* @name NtfsUnlinkFileName
* @implemented
*
* Removes one name of a file that has others: the index entry in its directory and the
* $FILE_NAME, and the DOS name paired with it, if it has one. The file, its data and its
* other names stay.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param MftIndex
* MFT index of the file.
*
* @param ParentIndex
* MFT index of the directory holding the name.
*
* @param Name
* The name.
*
* @param CaseSensitive
* TRUE for a POSIX-semantics lookup of the index entry.
*/
NTSTATUS
NtfsUnlinkFileName(PDEVICE_EXTENSION DeviceExt,
                   ULONGLONG MftIndex,
                   ULONGLONG ParentIndex,
                   PCUNICODE_STRING Name,
                   BOOLEAN CaseSensitive)
{
    PFILE_RECORD_HEADER FileRecord;
    PFILENAME_ATTRIBUTE *Names = NULL;
    ULONG Count = 0, i;
    NTSTATUS Status;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, MftIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsGetFileNames(DeviceExt, FileRecord, &Names, &Count);

    for (i = 0; NT_SUCCESS(Status) && i < Count; i++)
    {
        UNICODE_STRING DosName;

        if (!NtfsFileNameMatches(DeviceExt, Names[i], ParentIndex, Name))
            continue;

        Status = NtfsRemoveFilenameFromDirectory(DeviceExt, ParentIndex, Names[i], CaseSensitive);
        if (NT_SUCCESS(Status))
            Status = NtfsRemoveFileNameAttribute(DeviceExt, FileRecord, ParentIndex, Name);

        /* A Win32 name with an 8.3 name of its own counts as one link with it */
        if (NT_SUCCESS(Status) && Names[i]->NameType == NTFS_FILE_NAME_WIN32)
        {
            ULONG j;

            for (j = 0; j < Count; j++)
            {
                if (Names[j]->NameType != NTFS_FILE_NAME_DOS ||
                    (Names[j]->DirectoryFileReferenceNumber & NTFS_MFT_MASK) != ParentIndex)
                {
                    continue;
                }

                DosName.Buffer = Names[j]->Name;
                DosName.Length = DosName.MaximumLength = Names[j]->NameLength * sizeof(WCHAR);
                Status = NtfsRemoveFilenameFromDirectory(DeviceExt, ParentIndex, Names[j], CaseSensitive);
                if (NT_SUCCESS(Status))
                    Status = NtfsRemoveFileNameAttribute(DeviceExt, FileRecord, ParentIndex, &DosName);
                break;
            }
        }

        if (NT_SUCCESS(Status))
            Status = NtfsKeepFileNameInBase(DeviceExt, FileRecord);

        if (NT_SUCCESS(Status))
        {
            ASSERT(FileRecord->LinkCount > 1);
            FileRecord->LinkCount--;
            Status = UpdateFileRecord(DeviceExt, MftIndex, FileRecord);
        }
        break;
    }

    if (NT_SUCCESS(Status) && i == Count)
        Status = STATUS_OBJECT_NAME_NOT_FOUND;

    if (Names != NULL)
        NtfsFreeFileNames(Names, Count);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsDeleteFileLink
* @implemented
*
* Deletes the name a file was opened by: only that name while the file has others, the
* whole file with its last one.
*
* @param LinksLeft
* Receives the number of links the file has left.
*/
NTSTATUS
NtfsDeleteFileLink(PDEVICE_EXTENSION DeviceExt,
                   ULONGLONG MftIndex,
                   ULONGLONG ParentIndex,
                   PCUNICODE_STRING Name,
                   BOOLEAN CaseSensitive,
                   PUSHORT LinksLeft)
{
    PFILE_RECORD_HEADER FileRecord;
    USHORT LinkCount;
    NTSTATUS Status;

    *LinksLeft = 0;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, MftIndex, FileRecord);
    LinkCount = FileRecord->LinkCount;
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (!NT_SUCCESS(Status))
        return Status;

    if (LinkCount <= 1)
        return NtfsDeleteFileRecord(DeviceExt, MftIndex, CaseSensitive);

    Status = NtfsUnlinkFileName(DeviceExt, MftIndex, ParentIndex, Name, CaseSensitive);
    if (NT_SUCCESS(Status))
        *LinksLeft = LinkCount - 1;

    return Status;
}

/**
* @name NtfsFreeRecordClusters
*
* Gives back the clusters of every non-resident attribute a file record holds.
*/
static
NTSTATUS
NtfsFreeRecordClusters(PDEVICE_EXTENSION DeviceExt,
                       PFILE_RECORD_HEADER FileRecord)
{
    PNTFS_ATTR_RECORD Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
    ULONG AttributeOffset = FileRecord->AttributeOffset;
    NTSTATUS Status = STATUS_SUCCESS;

    while (Attribute->Type != AttributeEnd)
    {
        if (Attribute->IsNonResident && Attribute->NonResident.HighestVCN != (ULONGLONG)-1)
        {
            PNTFS_ATTR_CONTEXT AttributeContext = PrepareAttributeContext(Attribute);

            if (AttributeContext == NULL)
                return STATUS_INSUFFICIENT_RESOURCES;

            AttributeContext->FileMFTIndex = FileRecord->MFTRecordNumber;
            Status = FreeClusters(DeviceExt,
                                  AttributeContext,
                                  AttributeOffset,
                                  FileRecord,
                                  (ULONG)(Attribute->NonResident.HighestVCN - Attribute->NonResident.LowestVCN + 1));
            ReleaseAttributeContext(AttributeContext);
            if (!NT_SUCCESS(Status))
                return Status;
        }

        AttributeOffset += Attribute->Length;
        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttributeOffset);
    }

    return Status;
}

/**
* @name NtfsDeleteExtensionRecords
*
* Frees the extension records of a file being deleted, and the clusters of the attributes
* they hold.
*/
NTSTATUS
NtfsDeleteExtensionRecords(PDEVICE_EXTENSION DeviceExt,
                           PFILE_RECORD_HEADER BaseRecord)
{
    PNTFS_ATTRIBUTE_LIST_ITEM List;
    PULONGLONG Indices = NULL;
    PFILE_RECORD_HEADER Extension;
    ULONG ListLength, Count = 0, i;
    NTSTATUS Status;

    Status = NtfsReadAttributeList(DeviceExt, BaseRecord, &List, &ListLength);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsGetExtensionRecords(List, ListLength, BaseRecord->MFTRecordNumber, &Indices, &Count);
    ExFreePoolWithTag(List, TAG_NTFS);

    Extension = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (Extension == NULL)
        Status = STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; NT_SUCCESS(Status) && i < Count; i++)
    {
        Status = ReadFileRecord(DeviceExt, Indices[i], Extension);
        if (NT_SUCCESS(Status))
            Status = NtfsFreeRecordClusters(DeviceExt, Extension);
        if (NT_SUCCESS(Status))
            Status = FreeMftEntry(DeviceExt, Indices[i]);
    }

    if (Extension != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Extension);
    if (Indices != NULL)
        ExFreePoolWithTag(Indices, TAG_NTFS);

    return Status;
}

/**
* @name NtfsDeleteAttributeFromFile
* @implemented
*
* Takes an attribute out of a file, wherever it lives: its clusters go back to the volume,
* and an extension record that held it leaves the $ATTRIBUTE_LIST if it's empty now.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param BaseRecord
* The file's base record, in memory. The caller writes it back afterwards.
*
* @return
* STATUS_SUCCESS, STATUS_OBJECT_NAME_NOT_FOUND if the file has no such attribute, or an
* allocation or I/O error.
*/
NTSTATUS
NtfsDeleteAttributeFromFile(PDEVICE_EXTENSION DeviceExt,
                            PFILE_RECORD_HEADER BaseRecord,
                            ULONG Type,
                            PCWSTR Name,
                            ULONG NameLength)
{
    PFILE_RECORD_HEADER Holder = NULL;
    PNTFS_ATTR_CONTEXT AttrContext;
    ULONG AttrOffset;
    NTSTATUS Status;

    Status = FindAttribute(DeviceExt, BaseRecord, Type, Name, NameLength, &AttrContext, &AttrOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsReadAttributeHolder(DeviceExt, AttrContext, BaseRecord, &Holder);

    // Give back the attribute's clusters first
    if (NT_SUCCESS(Status) &&
        AttrContext->pRecord->IsNonResident &&
        AttrContext->pRecord->NonResident.HighestVCN != (ULONGLONG)-1)
    {
        ULONGLONG ClusterCount = AttrContext->pRecord->NonResident.HighestVCN + 1;

        Status = FreeClusters(DeviceExt, AttrContext, AttrOffset, Holder, (ULONG)ClusterCount);
    }

    if (NT_SUCCESS(Status))
        NtfsRemoveAttribute(Holder, AttrOffset);

    /* An extension record is written here, and the list loses the attribute */
    if (NT_SUCCESS(Status) && Holder != BaseRecord)
    {
        Status = UpdateFileRecord(DeviceExt, AttrContext->FileMFTIndex & NTFS_MFT_MASK, Holder);
        if (NT_SUCCESS(Status))
            Status = NtfsRebuildAttributeList(DeviceExt, BaseRecord, NULL, 0);
    }

    ReleaseAttributeContext(AttrContext);
    NtfsReleaseAttributeHolder(DeviceExt, BaseRecord, Holder);

    return Status;
}

/**
* @name NtfsSetAttributeValue
* @implemented
*
* Gives a file an attribute with the given value, replacing the one it has: resident while
* the records have room, in clusters otherwise.
*
* @param BaseRecord
* The file's base record, in memory. Written back.
*/
NTSTATUS
NtfsSetAttributeValue(PDEVICE_EXTENSION DeviceExt,
                      PFILE_RECORD_HEADER BaseRecord,
                      ULONG Type,
                      PCWSTR Name,
                      USHORT NameLength,
                      const VOID *Value,
                      ULONG ValueLength)
{
    PFILE_RECORD_HEADER Holder;
    PNTFS_ATTR_CONTEXT AttrContext;
    ULONG AttrOffset, Written;
    LARGE_INTEGER Size;
    NTSTATUS Status;

    Status = NtfsDeleteAttributeFromFile(DeviceExt, BaseRecord, Type, Name, NameLength);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND)
        return Status;

    Status = NtfsAddResidentAttributeToFile(DeviceExt, BaseRecord, Type, Name, NameLength, Value, ValueLength);
    if (NT_SUCCESS(Status) || Status != STATUS_DISK_FULL)
    {
        if (NT_SUCCESS(Status))
            Status = UpdateFileRecord(DeviceExt, BaseRecord->MFTRecordNumber, BaseRecord);
        return Status;
    }

    /* Too big for a record: add it empty, then size it, which moves it to clusters */
    Status = NtfsAddResidentAttributeToFile(DeviceExt, BaseRecord, Type, Name, NameLength, NULL, 0);
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(DeviceExt, BaseRecord->MFTRecordNumber, BaseRecord);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = FindAttribute(DeviceExt, BaseRecord, Type, Name, NameLength, &AttrContext, &AttrOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsReadAttributeHolder(DeviceExt, AttrContext, BaseRecord, &Holder);
    if (NT_SUCCESS(Status))
    {
        Size.QuadPart = ValueLength;
        Status = SetResidentAttributeDataLength(DeviceExt, AttrContext, AttrOffset, Holder, &Size);
        if (NT_SUCCESS(Status) && !AttrContext->pRecord->IsNonResident)
            Status = UpdateFileRecord(DeviceExt, AttrContext->FileMFTIndex & NTFS_MFT_MASK, Holder);
        if (NT_SUCCESS(Status))
            Status = WriteAttribute(DeviceExt, AttrContext, 0, (PUCHAR)Value, ValueLength, &Written, Holder);

        NtfsReleaseAttributeHolder(DeviceExt, BaseRecord, Holder);
    }

    ReleaseAttributeContext(AttrContext);

    return Status;
}

/**
* @name NtfsDeleteStream
* @implemented
*
* Deletes a named data stream, leaving the file and its other streams alone.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param MftIndex
* MFT index of the file the stream belongs to.
*
* @param StreamName
* Name of the stream, NUL-terminated.
*
* @return
* STATUS_SUCCESS, STATUS_OBJECT_NAME_NOT_FOUND if the file has no such stream, or an
* allocation or I/O error.
*/
NTSTATUS
NtfsDeleteStream(PDEVICE_EXTENSION DeviceExt,
                 ULONGLONG MftIndex,
                 PCWSTR StreamName)
{
    PFILE_RECORD_HEADER FileRecord;
    NTSTATUS Status;

    ASSERT(StreamName[0] != UNICODE_NULL);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, MftIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsDeleteAttributeFromFile(DeviceExt, FileRecord, AttributeData, StreamName, wcslen(StreamName));
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(DeviceExt, MftIndex, FileRecord);

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
CompareFileName(PDEVICE_EXTENSION Vcb,
                PUNICODE_STRING FileName,
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

        /* With a real upcase table the matcher folds both sides through it,
         * which is what NTFS means by case-insensitive. */
        Ret = FsRtlIsNameInExpression(&IntFileName, &EntryName, !CaseSensitive,
                                      Vcb ? Vcb->UpCaseTable : NULL);

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

/* An exact-name lookup matches one entry: the file's own long name, never a
 * DOS-only alias nor a system file's entry. */
static
BOOLEAN
IsLookupMatch(PINDEX_ENTRY_ATTRIBUTE IndexEntry)
{
    return (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE &&
           IndexEntry->FileName.NameType != NTFS_FILE_NAME_DOS;
}

/* Exact-name lookups follow the $I30 B-tree by collation instead of visiting
 * every node: an entry's sub-node holds the keys that sort before it, the END
 * entry's sub-node those that sort after every real entry. Returns the entry
 * holding FileName (*Found = TRUE), or else the first entry that sorts after
 * it, whose sub-node is the only place left that can hold the name. */
static
PINDEX_ENTRY_ATTRIBUTE
SeekIndexEntry(PUNICODE_STRING FileName,
               PINDEX_ENTRY_ATTRIBUTE FirstEntry,
               PINDEX_ENTRY_ATTRIBUTE LastEntry,
               BOOLEAN CaseSensitive,
               PBOOLEAN Found)
{
    PINDEX_ENTRY_ATTRIBUTE IndexEntry;
    UNICODE_STRING EntryName;
    LONG Comparison;

    *Found = FALSE;

    for (IndexEntry = FirstEntry;
         !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END);
         IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)IndexEntry + IndexEntry->Length))
    {
        ASSERT(IndexEntry < LastEntry);
        ASSERT(IndexEntry->Length >= sizeof(INDEX_ENTRY_ATTRIBUTE));

        EntryName.Buffer = IndexEntry->FileName.Name;
        EntryName.Length = EntryName.MaximumLength = IndexEntry->FileName.NameLength * sizeof(WCHAR);

        Comparison = CollateFileNames(FileName, &EntryName, CaseSensitive);
        if (Comparison < 0)
            break;

        if (Comparison == 0 && IsLookupMatch(IndexEntry))
        {
            *Found = TRUE;
            break;
        }
    }

    return IndexEntry;
}

/* Copies the $FILE_NAME key of a matched index entry for a caller that wants the name it
 * matched, which for a file with several links is that link's own name */
static
VOID
NtfsCopyFoundName(PINDEX_ENTRY_ATTRIBUTE IndexEntry,
                  PFILENAME_ATTRIBUTE OutFoundName)
{
    if (OutFoundName == NULL)
        return;

    ASSERT(IndexEntry->KeyLength <= NTFS_FOUND_NAME_SIZE);
    RtlCopyMemory(OutFoundName, &IndexEntry->FileName, min(IndexEntry->KeyLength, NTFS_FOUND_NAME_SIZE));
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

    DPRINT("BrowseSubNodeIndexEntries(%p, %p, %lu, %wZ, %p, %p, %I64d, %lu, %lu, %s, %s, %p)\n",
           Vcb,
           MftRecord,
           IndexBlockSize,
           FileName,
           IndexAllocationContext,
           Bitmap,
           VCN,
           *StartEntry,
           *CurrentEntry,
           "FALSE",
           DirSearch ? "TRUE" : "FALSE",
           CaseSensitive ? "TRUE" : "FALSE",
           OutMFTIndex);

    // Calculate node number as VCN / Clusters per index record. An index record smaller than
    // a cluster is addressed in sectors, matching GetAllocationOffsetFromVCN().
    if (IndexBlockSize < Vcb->NtfsInfo.BytesPerCluster)
        NodeNumber = VCN / (IndexBlockSize / Vcb->NtfsInfo.BytesPerSector);
    else
        NodeNumber = VCN / (IndexBlockSize / Vcb->NtfsInfo.BytesPerCluster);

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

    // Calculate offset of index record, using the same helper the write path does
    Offset = GetAllocationOffsetFromVCN(Vcb, IndexBlockSize, VCN);

    // Read the index record
    BytesRead = ReadAttribute(Vcb, IndexAllocationContext, Offset, (PCHAR)IndexRecord, IndexBlockSize);
    if (BytesRead != IndexBlockSize)
    {
        DPRINT1("Unable to read index record!\n");
        ExFreePoolWithTag(IndexRecord, TAG_NTFS);
        return STATUS_UNSUCCESSFUL;
    }

    if (IndexRecord->Ntfs.Type != NRH_INDX_TYPE)
    {
        DPRINT1("File system corruption detected, no index record at VCN %I64u of MFT record %I64u "
                "(offset 0x%I64x, block size %lu, cluster %lu, index record %lu, signature 0x%08lx)\n",
                VCN,
                MftRecord->MFTRecordNumber & NTFS_MFT_MASK,
                Offset,
                IndexBlockSize,
                Vcb->NtfsInfo.BytesPerCluster,
                Vcb->NtfsInfo.BytesPerIndexRecord,
                IndexRecord->Ntfs.Type);
        ExFreePoolWithTag(IndexRecord, TAG_NTFS);
        return STATUS_DATA_ERROR;
    }

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

    if (!DirSearch)
    {
        BOOLEAN Found;

        IndexEntry = SeekIndexEntry(FileName, FirstEntry, LastEntry, CaseSensitive, &Found);
        if (Found)
        {
            *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            NtfsCopyFoundName(IndexEntry, OutFoundName);
            Status = STATUS_SUCCESS;
        }
        else if (IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE)
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
        }
        else
        {
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
        }

        ExFreePoolWithTag(IndexRecord, TAG_NTFS);
        return Status;
    }

    // Loop through all Index Entries of index, starting with FirstEntry
    IndexEntry = FirstEntry;
    while (IndexEntry <= LastEntry)
    {
        // Does IndexEntry have a sub-node?
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
            CompareFileName(Vcb, FileName, IndexEntry, DirSearch, CaseSensitive))
        {
            *StartEntry = *CurrentEntry;
            *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            NtfsCopyFoundName(IndexEntry, OutFoundName);
            ExFreePoolWithTag(IndexRecord, TAG_NTFS);
            return STATUS_SUCCESS;
        }

        // Advance to the next index entry
        (*CurrentEntry) += 1;
        ASSERT(IndexEntry->Length >= sizeof(INDEX_ENTRY_ATTRIBUTE));
        IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)IndexEntry + IndexEntry->Length);
    }

    ExFreePoolWithTag(IndexRecord, TAG_NTFS);

    return STATUS_OBJECT_PATH_NOT_FOUND;
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

    DPRINT("BrowseIndexEntries(%p, %p, %p, %lu, %p, %p, %wZ, %lu, %lu, %s, %s, %p)\n",
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
           CaseSensitive ? "TRUE" : "FALSE",
           OutMFTIndex);

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
        // ReadAttribute returns the bytes it read
        if (ReadAttribute(Vcb, BitmapContext, 0, (PCHAR)BitmapPtr, (ULONG)BitmapLength) != BitmapLength)
        {
            Status = STATUS_UNEXPECTED_IO_ERROR;
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

    if (!DirSearch)
    {
        BOOLEAN Found;

        // Exact lookups always start from the first entry
        ASSERT(*StartEntry == 0);

        IndexEntry = SeekIndexEntry(FileName, FirstEntry, LastEntry, CaseSensitive, &Found);
        if (Found)
        {
            *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            NtfsCopyFoundName(IndexEntry, OutFoundName);
            Status = STATUS_SUCCESS;
        }
        else if ((IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE) && IndexAllocationContext)
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
        }
        else
        {
            if (IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE)
                DPRINT1("Filesystem corruption detected, index root points at a sub-node but there is no $INDEX_ALLOCATION!\n");
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
        }

        if (IndexAllocationContext)
        {
            ExFreePoolWithTag(BitmapMem, TAG_NTFS);
            ReleaseAttributeContext(BitmapContext);
            ReleaseAttributeContext(IndexAllocationContext);
        }
        return Status;
    }


    // Loop through all Index Entries of index, starting with FirstEntry
    IndexEntry = FirstEntry;
    while (IndexEntry <= LastEntry)
    {
        // Does IndexEntry have a sub-node?
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

                /* Anything other than "not in this subtree" means we gave up on
                 * a subtree that may well have held the entry */
                if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_PATH_NOT_FOUND)
                {
                    DPRINT1("Sub-node at VCN %I64u abandoned while looking for '%wZ' (Status %lx)\n",
                            GetIndexEntryVCN(IndexEntry), FileName, Status);
                }

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
            CompareFileName(Vcb, FileName, IndexEntry, DirSearch, CaseSensitive))
        {
            *StartEntry = *CurrentEntry;
            *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            NtfsCopyFoundName(IndexEntry, OutFoundName);
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
        DPRINT("'%wZ' not found after scanning %lu entries of MFT record %I64u "
               "(index allocation present)\n",
               FileName, *CurrentEntry, MftRecord->MFTRecordNumber & NTFS_MFT_MASK);

        ExFreePoolWithTag(BitmapMem, TAG_NTFS);
        ReleaseAttributeContext(BitmapContext);
        ReleaseAttributeContext(IndexAllocationContext);
    }

    return STATUS_OBJECT_PATH_NOT_FOUND;
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

    DPRINT("NtfsFindMftRecord(%p, %I64d, %wZ, %lu, %s, %s, %p)\n",
           Vcb,
           MFTIndex,
           FileName,
           *FirstEntry,
           DirSearch ? "TRUE" : "FALSE",
           CaseSensitive ? "TRUE" : "FALSE",
           OutMFTIndex);

    MftRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (MftRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, MFTIndex, MftRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return Status;
    }

    ASSERT(MftRecord->Ntfs.Type == NRH_FILE_TYPE);
    Status = FindAttribute(Vcb, MftRecord, AttributeIndexRoot, L"$I30", 4, &IndexRootCtx, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return Status;
    }

    IndexRecord = ExAllocatePoolWithTag(NonPagedPool, Vcb->NtfsInfo.BytesPerIndexRecord, TAG_NTFS);
    if (IndexRecord == NULL)
    {
        ReleaseAttributeContext(IndexRootCtx);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ReadAttribute(Vcb, IndexRootCtx, 0, IndexRecord, Vcb->NtfsInfo.BytesPerIndexRecord);
    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)IndexRecord;
    IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((PCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
    /* Index root is always resident. */
    /* TotalSizeOfEntries is measured from the index header, not from the
     * start of the attribute, and FirstEntryOffset above already is. */
    IndexEntryEnd = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);
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

    ExFreePoolWithTag(IndexRecord, TAG_NTFS);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);

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

        Status = NtfsFindMftRecord(Vcb, CurrentMFTIndex, &Current, &FirstEntry, FALSE, CaseSensitive, &CurrentMFTIndex, NULL);
        if (!NT_SUCCESS(Status))
        {
            /* The index walk reports a miss as a missing path. A missing last
             * component is a missing name (MS-FSA 2.1.5.1). */
            if (Status == STATUS_OBJECT_PATH_NOT_FOUND && Remaining.Length == 0)
                Status = STATUS_OBJECT_NAME_NOT_FOUND;

            return Status;
        }

        if (Remaining.Length == 0)
            break;

        /* Split what is left of the path, not the component we just resolved */
        FsRtlDissectName(Remaining, &Current, &Remaining);
    }

    *FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (*FileRecord == NULL)
    {
        DPRINT("NtfsLookupFileAt: Can't allocate MFT record\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, CurrentMFTIndex, *FileRecord);
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


/* Deeper than any index this driver or Windows builds; past it the tree loops */
#define NTFS_MAX_INDEX_DEPTH 32

static
NTSTATUS
NtfsIndexEntriesAreEmpty(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_ATTR_CONTEXT IndexAllocationContext,
    _In_ ULONG IndexBlockSize,
    _In_ PINDEX_HEADER_ATTRIBUTE Header,
    _In_ ULONG Depth,
    _Out_ PBOOLEAN Empty);

/**
* @name NtfsIndexNodeIsEmpty
*
* Reads the index node at Vcn and reports whether its subtree holds any entry.
*/
static
NTSTATUS
NtfsIndexNodeIsEmpty(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_ATTR_CONTEXT IndexAllocationContext,
    _In_ ULONG IndexBlockSize,
    _In_ ULONGLONG Vcn,
    _In_ ULONG Depth,
    _Out_ PBOOLEAN Empty)
{
    PINDEX_BUFFER IndexBuffer;
    ULONG BytesRead;
    NTSTATUS Status;

    if (IndexAllocationContext == NULL || Depth >= NTFS_MAX_INDEX_DEPTH)
        return STATUS_FILE_CORRUPT_ERROR;

    IndexBuffer = ExAllocatePoolWithTag(NonPagedPool, IndexBlockSize, TAG_NTFS);
    if (IndexBuffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    BytesRead = ReadAttribute(Vcb,
                              IndexAllocationContext,
                              GetAllocationOffsetFromVCN(Vcb, IndexBlockSize, Vcn),
                              (PCHAR)IndexBuffer,
                              IndexBlockSize);
    if (BytesRead != IndexBlockSize || IndexBuffer->Ntfs.Type != NRH_INDX_TYPE)
    {
        DPRINT1("Unable to read index node at VCN %I64u\n", Vcn);
        ExFreePoolWithTag(IndexBuffer, TAG_NTFS);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Status = FixupUpdateSequenceArray(Vcb, &((PFILE_RECORD_HEADER)IndexBuffer)->Ntfs);
    if (NT_SUCCESS(Status) &&
        FIELD_OFFSET(INDEX_BUFFER, Header) + IndexBuffer->Header.TotalSizeOfEntries > IndexBlockSize)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
    }

    if (NT_SUCCESS(Status))
    {
        Status = NtfsIndexEntriesAreEmpty(Vcb,
                                          IndexAllocationContext,
                                          IndexBlockSize,
                                          &IndexBuffer->Header,
                                          Depth + 1,
                                          Empty);
    }

    ExFreePoolWithTag(IndexBuffer, TAG_NTFS);
    return Status;
}

/**
* @name NtfsIndexEntriesAreEmpty
*
* Checks the entries under Header. Entries are sorted with the terminator last,
* so the first one decides: anything but the terminator is a child, and a
* terminator that points down only leads to more entries to check.
*/
static
NTSTATUS
NtfsIndexEntriesAreEmpty(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_ATTR_CONTEXT IndexAllocationContext,
    _In_ ULONG IndexBlockSize,
    _In_ PINDEX_HEADER_ATTRIBUTE Header,
    _In_ ULONG Depth,
    _Out_ PBOOLEAN Empty)
{
    PINDEX_ENTRY_ATTRIBUTE IndexEntry;

    /* A terminator has no key, so only the fixed header is guaranteed */
    if (Header->FirstEntryOffset + FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName) > Header->TotalSizeOfEntries)
        return STATUS_FILE_CORRUPT_ERROR;

    IndexEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)Header + Header->FirstEntryOffset);

    if (Header->FirstEntryOffset + IndexEntry->Length > Header->TotalSizeOfEntries)
        return STATUS_FILE_CORRUPT_ERROR;

    if (!(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
    {
        *Empty = FALSE;
        return STATUS_SUCCESS;
    }

    if (IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE)
    {
        /* The child's VCN sits in the last 8 bytes of the entry */
        if (IndexEntry->Length < FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName) + sizeof(ULONGLONG))
            return STATUS_FILE_CORRUPT_ERROR;

        return NtfsIndexNodeIsEmpty(Vcb,
                                    IndexAllocationContext,
                                    IndexBlockSize,
                                    GetIndexEntryVCN(IndexEntry),
                                    Depth,
                                    Empty);
    }

    *Empty = TRUE;
    return STATUS_SUCCESS;
}

/**
* @name NtfsIsDirectoryEmpty
* @implemented
*
* Decides whether a directory may be deleted. Unlike a name lookup, a sub-node
* that can't be read is an error here rather than a miss, since it may hold children.
*
* @param Vcb
* Volume holding the directory.
*
* @param MftIndex
* MFT record of the directory.
*
* @param Empty
* Receives TRUE if the index holds no entries.
*
* @return
* STATUS_SUCCESS if Empty was set, or an error if the index couldn't be fully read.
*/
NTSTATUS
NtfsIsDirectoryEmpty(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ ULONGLONG MftIndex,
    _Out_ PBOOLEAN Empty)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT IndexRootContext = NULL;
    PNTFS_ATTR_CONTEXT IndexAllocationContext = NULL;
    PINDEX_ROOT_ATTRIBUTE IndexRoot = NULL;
    ULONG IndexRootLength;
    NTSTATUS Status;

    *Empty = FALSE;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, MftIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = FindAttribute(Vcb, FileRecord, AttributeIndexRoot, L"$I30", 4, &IndexRootContext, NULL);
    if (!NT_SUCCESS(Status))
        goto Quit;

    IndexRootLength = (ULONG)AttributeDataLength(IndexRootContext->pRecord);
    if (IndexRootLength < sizeof(*IndexRoot))
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Quit;
    }

    IndexRoot = ExAllocatePoolWithTag(NonPagedPool, IndexRootLength, TAG_NTFS);
    if (IndexRoot == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Quit;
    }

    if (ReadAttribute(Vcb, IndexRootContext, 0, (PCHAR)IndexRoot, IndexRootLength) != IndexRootLength)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Quit;
    }

    if (FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header) + IndexRoot->Header.TotalSizeOfEntries > IndexRootLength)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Quit;
    }

    /* Only a root that points down needs the allocation */
    if (IndexRoot->Header.Flags & INDEX_ROOT_LARGE)
    {
        Status = FindAttribute(Vcb, FileRecord, AttributeIndexAllocation, L"$I30", 4, &IndexAllocationContext, NULL);
        if (!NT_SUCCESS(Status))
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Quit;
        }
    }

    Status = NtfsIndexEntriesAreEmpty(Vcb,
                                      IndexAllocationContext,
                                      IndexRoot->SizeOfEntry,
                                      &IndexRoot->Header,
                                      0,
                                      Empty);

Quit:
    if (IndexRoot != NULL)
        ExFreePoolWithTag(IndexRoot, TAG_NTFS);
    if (IndexAllocationContext != NULL)
        ReleaseAttributeContext(IndexAllocationContext);
    if (IndexRootContext != NULL)
        ReleaseAttributeContext(IndexRootContext);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/* EOF */
