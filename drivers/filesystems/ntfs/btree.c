/*
*  ReactOS kernel
*  Copyright (C) 2002, 2017 ReactOS Team
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
* FILE:             drivers/filesystem/ntfs/btree.c
* PURPOSE:          NTFS filesystem driver
* PROGRAMMERS:      Trevor Thompson
*/

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

// TEMP FUNCTION for diagnostic purposes.
// Prints VCN of every node in an index allocation
VOID
PrintAllVCNs(PDEVICE_EXTENSION Vcb,
             PNTFS_ATTR_CONTEXT IndexAllocationContext,
             ULONG NodeSize)
{
    ULONGLONG CurrentOffset = 0;
    PINDEX_BUFFER CurrentNode, Buffer;
    ULONGLONG BufferSize = AttributeDataLength(IndexAllocationContext->pRecord);
    ULONG BytesRead;
    ULONGLONG i;
    int Count = 0;

    if (BufferSize == 0)
    {
        DPRINT1("Index Allocation is empty.\n");
        return;
    }

    Buffer = ExAllocatePoolWithTag(NonPagedPool, BufferSize, TAG_NTFS);

    BytesRead = ReadAttribute(Vcb, IndexAllocationContext, 0, (PCHAR)Buffer, BufferSize);

    ASSERT(BytesRead == BufferSize);

    CurrentNode = Buffer;

    // loop through all the nodes
    for (i = 0; i < BufferSize; i += NodeSize)
    {
        NTSTATUS Status = FixupUpdateSequenceArray(Vcb, &CurrentNode->Ntfs);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Fixing fixup failed!\n");
            continue;
        }

        DPRINT("Node #%d, VCN: %I64u\n", Count, CurrentNode->VCN);

        CurrentNode = (PINDEX_BUFFER)((ULONG_PTR)CurrentNode + NodeSize);
        CurrentOffset += NodeSize;
        Count++;
    }

    ExFreePoolWithTag(Buffer, TAG_NTFS);
}

/**
* @name AllocateIndexNode
* @implemented
*
* Allocates a new index record in an index allocation.
*
* @param DeviceExt
* Pointer to the target DEVICE_EXTENSION describing the volume the node will be created on.
*
* @param FileRecord
* Pointer to a copy of the file record containing the index.
*
* @param IndexBufferSize
* Size of an index record for this index, in bytes. Commonly defined as 4096.
*
* @param IndexAllocationCtx
* Pointer to an NTFS_ATTR_CONTEXT describing the index allocation attribute the node will be assigned to.
*
* @param IndexAllocationOffset
* Offset of the index allocation attribute relative to the file record.
*
* @param NewVCN
* Pointer to a ULONGLONG which will receive the VCN of the newly-assigned index record
*
* @returns
* STATUS_SUCCESS in case of success.
* STATUS_NOT_IMPLEMENTED if there's no $I30 bitmap attribute in the file record.
*
* @remarks
* AllocateIndexNode() doesn't write any data to the index record it creates. Called by UpdateIndexNode().
* Don't call PrintAllVCNs() or NtfsDumpFileRecord() after calling AllocateIndexNode() before UpdateIndexNode() finishes.
* Possible TODO: Create an empty node and write it to the allocated index node, so the index allocation is always valid.
*/
NTSTATUS
AllocateIndexNode(PDEVICE_EXTENSION DeviceExt,
                  PB_TREE Tree,
                  PFILE_RECORD_HEADER FileRecord,
                  ULONG IndexBufferSize,
                  PNTFS_ATTR_CONTEXT IndexAllocationCtx,
                  ULONG IndexAllocationOffset,
                  PULONGLONG NewVCN)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT BitmapCtx;
    ULONGLONG IndexAllocationLength, BitmapLength;
    ULONG BitmapOffset;
    ULONGLONG NextNodeNumber;
    PCHAR *BitmapMem;
    ULONG *BitmapPtr;
    RTL_BITMAP Bitmap;
    ULONG BytesWritten;
    ULONG BytesNeeded, BufferLength;
    LARGE_INTEGER DataSize;

    DPRINT("AllocateIndexNode(%p, %p, %lu, %p, %lu, %p) called.\n", DeviceExt,
            FileRecord,
            IndexBufferSize,
            IndexAllocationCtx,
            IndexAllocationOffset,
            NewVCN);

    // Get the length of the attribute allocation
    IndexAllocationLength = AttributeDataLength(IndexAllocationCtx->pRecord);

    // Find the bitmap attribute for the index
    Status = FindAttribute(DeviceExt,
                           FileRecord,
                           AttributeBitmap,
                           Tree->IndexName,
                           Tree->IndexNameLength,
                           &BitmapCtx,
                           &BitmapOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("FIXME: Need to add bitmap attribute!\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    // Get the length of the bitmap attribute
    BitmapLength = AttributeDataLength(BitmapCtx->pRecord);

    NextNodeNumber = IndexAllocationLength / DeviceExt->NtfsInfo.BytesPerIndexRecord;

    // TODO: Find unused allocation in bitmap and use that space first

    // Add another bit to bitmap

    // See how many bytes we need to store the amount of bits we'll have
    BytesNeeded = NextNodeNumber / 8;
    BytesNeeded++;

    // Windows seems to allocate the bitmap in 8-byte chunks to keep any bytes from being wasted on padding
    BytesNeeded = ALIGN_UP(BytesNeeded, ATTR_RECORD_ALIGNMENT);

    // The bitmap there is may already be longer than that; all of it is read
    BufferLength = (ULONG)max(BytesNeeded, BitmapLength);

    // Allocate memory for the bitmap, including some padding; RtlInitializeBitmap() wants a pointer
    // that's ULONG-aligned, and it wants the size of the memory allocated for it to be a ULONG-multiple.
    BitmapMem = ExAllocatePoolWithTag(NonPagedPool, BufferLength + sizeof(ULONG), TAG_NTFS);
    if (!BitmapMem)
    {
        DPRINT1("Error: failed to allocate bitmap!");
        ReleaseAttributeContext(BitmapCtx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // RtlInitializeBitmap() wants a pointer that's ULONG-aligned.
    BitmapPtr = (PULONG)ALIGN_UP_BY((ULONG_PTR)BitmapMem, sizeof(ULONG));

    RtlZeroMemory(BitmapPtr, BufferLength);

    // Read the existing bitmap data; ReadAttribute returns the bytes it read
    if (ReadAttribute(DeviceExt, BitmapCtx, 0, (PCHAR)BitmapPtr, (ULONG)BitmapLength) != BitmapLength)
    {
        DPRINT1("ERROR: Failed to read the index bitmap!\n");
        ExFreePoolWithTag(BitmapMem, TAG_NTFS);
        ReleaseAttributeContext(BitmapCtx);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    // Initialize bitmap. It has to describe the node we are about to add, so
    // it needs NextNodeNumber + 1 bits, not NextNodeNumber.
    RtlInitializeBitMap(&Bitmap, BitmapPtr, NextNodeNumber + 1);

    // Do we need to enlarge the bitmap?
    if (BytesNeeded > BitmapLength)
    {
        // TODO: handle synchronization issues that could occur from changing the directory's file record
        // Change bitmap size
        DataSize.QuadPart = BytesNeeded;
        if (BitmapCtx->pRecord->IsNonResident)
        {
            Status = SetNonResidentAttributeDataLength(DeviceExt,
                                                       BitmapCtx,
                                                       BitmapOffset,
                                                       FileRecord,
                                                       &DataSize);
        }
        else
        {
            Status = SetResidentAttributeDataLength(DeviceExt,
                                                    BitmapCtx,
                                                    BitmapOffset,
                                                    FileRecord,
                                                    &DataSize);
        }
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to set length of bitmap attribute!\n");
            ExFreePoolWithTag(BitmapMem, TAG_NTFS);
            ReleaseAttributeContext(BitmapCtx);
            return Status;
        }
    }

    // Enlarge Index Allocation attribute
    DataSize.QuadPart = IndexAllocationLength + IndexBufferSize;
    Status = SetNonResidentAttributeDataLength(DeviceExt,
                                               IndexAllocationCtx,
                                               IndexAllocationOffset,
                                               FileRecord,
                                               &DataSize);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to set length of index allocation!\n");
        ExFreePoolWithTag(BitmapMem, TAG_NTFS);
        ReleaseAttributeContext(BitmapCtx);
        return Status;
    }

    // Update file record on disk
    Status = NtfsCommitAttributeContext(DeviceExt, IndexAllocationCtx, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update file record!\n");
        ExFreePoolWithTag(BitmapMem, TAG_NTFS);
        ReleaseAttributeContext(BitmapCtx);
        return Status;
    }

    // Set the bit for the new index record
    RtlSetBits(&Bitmap, NextNodeNumber, 1);

    // Write the new bitmap attribute
    Status = WriteAttribute(DeviceExt,
                            BitmapCtx,
                            0,
                            (const PUCHAR)BitmapPtr,
                            BytesNeeded,
                            &BytesWritten,
                            FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Unable to write to the index bitmap attribute!\n");
    }

    // Calculate VCN of new node number. An index record smaller than a cluster is addressed
    // in sectors; dividing by the cluster size would give every node a VCN of zero.
    if (IndexBufferSize < DeviceExt->NtfsInfo.BytesPerCluster)
        *NewVCN = NextNodeNumber * (IndexBufferSize / DeviceExt->NtfsInfo.BytesPerSector);
    else
        *NewVCN = NextNodeNumber * (IndexBufferSize / DeviceExt->NtfsInfo.BytesPerCluster);

    DPRINT("New VCN: %I64u\n", *NewVCN);

    ExFreePoolWithTag(BitmapMem, TAG_NTFS);
    ReleaseAttributeContext(BitmapCtx);

    return Status;
}

/**
* @name CreateDummyKey
* @implemented
*
* Creates the final B_TREE_KEY for a B_TREE_FILENAME_NODE. Also creates the associated index entry.
*
* @param HasChildNode
* BOOLEAN to indicate if this key will have a LesserChild.
*
* @return
* The newly-created key.
*/
PB_TREE_KEY
CreateDummyKey(BOOLEAN HasChildNode)
{
    PINDEX_ENTRY_ATTRIBUTE NewIndexEntry;
    PB_TREE_KEY NewDummyKey;

    // Calculate max size of a dummy key
    ULONG EntrySize = ALIGN_UP_BY(FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName), 8);
    EntrySize += sizeof(ULONGLONG); // for VCN

    // Create the index entry for the key
    NewIndexEntry = ExAllocatePoolWithTag(NonPagedPool, EntrySize, TAG_NTFS);
    if (!NewIndexEntry)
    {
        DPRINT1("Couldn't allocate memory for dummy key index entry!\n");
        return NULL;
    }

    RtlZeroMemory(NewIndexEntry, EntrySize);

    if (HasChildNode)
    {
        NewIndexEntry->Flags = NTFS_INDEX_ENTRY_NODE | NTFS_INDEX_ENTRY_END;
    }
    else
    {
        NewIndexEntry->Flags = NTFS_INDEX_ENTRY_END;
        EntrySize -= sizeof(ULONGLONG); // no VCN
    }

    NewIndexEntry->Length = EntrySize;

    // Create the key
    NewDummyKey = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_KEY), TAG_NTFS);
    if (!NewDummyKey)
    {
        DPRINT1("Unable to allocate dummy key!\n");
        ExFreePoolWithTag(NewIndexEntry, TAG_NTFS);
        return NULL;
    }
    RtlZeroMemory(NewDummyKey, sizeof(B_TREE_KEY));

    NewDummyKey->IndexEntry = NewIndexEntry;

    return NewDummyKey;
}

/**
* @name SetBTreeIndex
* @implemented
*
* Records which index of its file record a tree describes and how its keys are ordered.
*/
static
VOID
SetBTreeIndex(PB_TREE Tree,
              PCWSTR IndexName,
              ULONG IndexNameLength,
              ULONG IndexedAttributeType,
              ULONG CollationRule)
{
    ASSERT(IndexNameLength <= NTFS_MAX_INDEX_NAME_LENGTH);

    RtlCopyMemory(Tree->IndexName, IndexName, IndexNameLength * sizeof(WCHAR));
    Tree->IndexNameLength = IndexNameLength;
    Tree->IndexedAttributeType = IndexedAttributeType;
    Tree->CollationRule = CollationRule;
}

/**
* @name CreateEmptyBTree
* @implemented
*
* Creates an empty B-Tree, which will contain a single root node which will contain a single dummy key.
*
* @param NewTree
* Pointer to a PB_TREE that will receive the pointer of the newly-created B-Tree.
*
* @return
* STATUS_SUCCESS on success. STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
*/
NTSTATUS
CreateEmptyBTree(PB_TREE *NewTree)
{
    PB_TREE Tree = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE), TAG_NTFS);
    PB_TREE_FILENAME_NODE RootNode = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_FILENAME_NODE), TAG_NTFS);
    PB_TREE_KEY DummyKey;

    DPRINT("CreateEmptyBTree(%p) called\n", NewTree);

    if (!Tree || !RootNode)
    {
        DPRINT1("Couldn't allocate enough memory for B-Tree!\n");
        if (Tree)
            ExFreePoolWithTag(Tree, TAG_NTFS);
        if (RootNode)
            ExFreePoolWithTag(RootNode, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Create the dummy key
    DummyKey = CreateDummyKey(FALSE);
    if (!DummyKey)
    {
        DPRINT1("ERROR: Failed to create dummy key!\n");
        ExFreePoolWithTag(Tree, TAG_NTFS);
        ExFreePoolWithTag(RootNode, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Tree, sizeof(B_TREE));
    RtlZeroMemory(RootNode, sizeof(B_TREE_FILENAME_NODE));

    // Setup the Tree. Only directories get new indexes, so it's a $I30 index.
    RootNode->FirstKey = DummyKey;
    RootNode->KeyCount = 1;
    RootNode->DiskNeedsUpdating = TRUE;
    Tree->RootNode = RootNode;
    SetBTreeIndex(Tree, L"$I30", 4, AttributeFileName, COLLATION_FILE_NAME);

    *NewTree = Tree;

    // Memory will be freed when DestroyBTree() is called

    return STATUS_SUCCESS;
}

static LONG CollateBinary(PVOID Key1, ULONG Key1Length, PVOID Key2, ULONG Key2Length);
static LONG CollateUlongs(PVOID Key1, ULONG Key1Length, PVOID Key2, ULONG Key2Length);
static PB_TREE_KEY CreateBTreeKeyForEntry(PINDEX_ENTRY_ATTRIBUTE Entry);

/**
* @name CompareTreeKeys
* @implemented
*
* Compare two B_TREE_KEY's to determine their order in the tree.
*
* @param Key1
* Pointer to a B_TREE_KEY that will be compared.
*
* @param Key2
* Pointer to the other B_TREE_KEY that will be compared.
*
* @param CollationRule
* The COLLATION_* rule of the index the keys belong to.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @returns
* 0 if the two keys are equal.
* < 0 if key1 is less thank key2
* > 0 if key1 is greater than key2
*
* @remarks
* Any other key is always less than the final (dummy) key in a node. Key1 must not be the dummy node.
*/
LONG
CompareTreeKeys(PB_TREE_KEY Key1, PB_TREE_KEY Key2, ULONG CollationRule, BOOLEAN CaseSensitive)
{
    PVOID Key1Bytes, Key2Bytes;

    // Key1 must not be the final key (AKA the dummy key)
    ASSERT(!(Key1->IndexEntry->Flags & NTFS_INDEX_ENTRY_END));

    // If Key2 is the "dummy key", key 1 will always come first
    if (Key2->NextKey == NULL)
        return -1;

    if (CollationRule == COLLATION_FILE_NAME)
    {
        UNICODE_STRING Key1Name, Key2Name;

        Key1Name.Buffer = Key1->IndexEntry->FileName.Name;
        Key1Name.Length = Key1Name.MaximumLength
            = Key1->IndexEntry->FileName.NameLength * sizeof(WCHAR);

        Key2Name.Buffer = Key2->IndexEntry->FileName.Name;
        Key2Name.Length = Key2Name.MaximumLength
            = Key2->IndexEntry->FileName.NameLength * sizeof(WCHAR);

        return CollateFileNames(&Key1Name, &Key2Name, CaseSensitive);
    }

    // A view index key is raw bytes right after the entry header
    Key1Bytes = &Key1->IndexEntry->FileName;
    Key2Bytes = &Key2->IndexEntry->FileName;

    switch (CollationRule)
    {
        case COLLATION_BINARY:
        case COLLATION_NTOFS_SID:
            /* A SID in $O of $Quota compares as bytes */
            return CollateBinary(Key1Bytes, Key1->IndexEntry->KeyLength,
                                 Key2Bytes, Key2->IndexEntry->KeyLength);

        case COLLATION_NTOFS_ULONG:
            ASSERT(Key1->IndexEntry->KeyLength == sizeof(ULONG));
            ASSERT(Key2->IndexEntry->KeyLength == sizeof(ULONG));
            return CollateUlongs(Key1Bytes, sizeof(ULONG), Key2Bytes, sizeof(ULONG));

        case COLLATION_NTOFS_ULONGS:
        case COLLATION_NTOFS_SECURITY_HASH:
            // A security hash key is the hash, then the security ID: two ULONGs
            return CollateUlongs(Key1Bytes, Key1->IndexEntry->KeyLength,
                                 Key2Bytes, Key2->IndexEntry->KeyLength);

        default:
            DPRINT1("Collation rule 0x%lx isn't implemented\n", CollationRule);
            ASSERT(FALSE);
            return CollateBinary(Key1Bytes, Key1->IndexEntry->KeyLength,
                                 Key2Bytes, Key2->IndexEntry->KeyLength);
    }
}

/**
* @name CollateBinary
* @implemented
*
* COLLATION_BINARY: byte by byte, then the shorter key first.
*/
static
LONG
CollateBinary(PVOID Key1, ULONG Key1Length, PVOID Key2, ULONG Key2Length)
{
    PUCHAR Bytes1 = Key1, Bytes2 = Key2;
    ULONG i;

    for (i = 0; i < min(Key1Length, Key2Length); i++)
    {
        if (Bytes1[i] != Bytes2[i])
            return Bytes1[i] < Bytes2[i] ? -1 : 1;
    }

    if (Key1Length == Key2Length)
        return 0;
    return Key1Length < Key2Length ? -1 : 1;
}

/**
* @name CollateUlongs
* @implemented
*
* COLLATION_NTOFS_ULONGS: an array of ULONGs, compared as numbers one by one, then the shorter
* key first. An object ID (a GUID) in $O is collated this way.
*/
static
LONG
CollateUlongs(PVOID Key1, ULONG Key1Length, PVOID Key2, ULONG Key2Length)
{
    PULONG Ulongs1 = Key1, Ulongs2 = Key2;
    ULONG i;

    ASSERT(Key1Length % sizeof(ULONG) == 0);
    ASSERT(Key2Length % sizeof(ULONG) == 0);

    for (i = 0; i < min(Key1Length, Key2Length) / sizeof(ULONG); i++)
    {
        if (Ulongs1[i] != Ulongs2[i])
            return Ulongs1[i] < Ulongs2[i] ? -1 : 1;
    }

    if (Key1Length == Key2Length)
        return 0;
    return Key1Length < Key2Length ? -1 : 1;
}

/**
* @name CollateFileNames
* @implemented
*
* Orders two file names the way a $I30 index (COLLATION_FILE_NAME) stores them.
*
* @param Name1
* Pointer to the first name.
*
* @param Name2
* Pointer to the second name.
*
* @param CaseSensitive
* TRUE if names that differ only in case are different names (POSIX semantics).
*
* @returns
* 0 if the names are the same key, < 0 if Name1 sorts first, > 0 if Name2 sorts first.
*
* @remarks
* The on-disk order is fixed by the volume and never depends on how a name is being
* created or looked up: the primary order is always the upcased one (a common prefix
* first, then the shorter name). CaseSensitive only orders names inside a run that
* upcases equal, so POSIX case variants can coexist; it must never change the primary
* order, or a case-sensitive insert lands in a node where a lookup won't descend.
*/
LONG
CollateFileNames(PUNICODE_STRING Name1, PUNICODE_STRING Name2, BOOLEAN CaseSensitive)
{
    LONG Comparison = RtlCompareUnicodeString(Name1, Name2, TRUE);

    if (Comparison == 0 && CaseSensitive)
        Comparison = RtlCompareUnicodeString(Name1, Name2, FALSE);

    return Comparison;
}

/**
* @name CountBTreeKeys
* @implemented
*
* Counts the number of linked B-Tree keys, starting with FirstKey.
*
* @param FirstKey
* Pointer to a B_TREE_KEY that will be the first key to be counted.
*
* @return
* The number of keys in a linked-list, including FirstKey and the final dummy key.
*/
ULONG
CountBTreeKeys(PB_TREE_KEY FirstKey)
{
    ULONG Count = 0;
    PB_TREE_KEY Current = FirstKey;

    while (Current != NULL)
    {
        Count++;
        Current = Current->NextKey;
    }

    return Count;
}

PB_TREE_FILENAME_NODE
CreateBTreeNodeFromIndexNode(PDEVICE_EXTENSION Vcb,
                             PINDEX_ROOT_ATTRIBUTE IndexRoot,
                             PNTFS_ATTR_CONTEXT IndexAllocationAttributeCtx,
                             PINDEX_ENTRY_ATTRIBUTE NodeEntry)
{
    PB_TREE_FILENAME_NODE NewNode;
    PINDEX_ENTRY_ATTRIBUTE CurrentNodeEntry;
    PINDEX_ENTRY_ATTRIBUTE FirstNodeEntry;
    ULONG CurrentEntryOffset = 0;
    PINDEX_BUFFER NodeBuffer;
    ULONG IndexBufferSize = Vcb->NtfsInfo.BytesPerIndexRecord;
    PULONGLONG VCN;
    PB_TREE_KEY CurrentKey;
    NTSTATUS Status;
    ULONGLONG IndexNodeOffset;
    ULONG BytesRead;

    if (IndexAllocationAttributeCtx == NULL)
    {
        DPRINT1("ERROR: Couldn't find index allocation attribute even though there should be one!\n");
        return NULL;
    }

    // Get the node number from the end of the node entry
    VCN = (PULONGLONG)((ULONG_PTR)NodeEntry + NodeEntry->Length - sizeof(ULONGLONG));

    // Create the new tree node
    NewNode = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_FILENAME_NODE), TAG_NTFS);
    if (!NewNode)
    {
        DPRINT1("ERROR: Couldn't allocate memory for new filename node.\n");
        return NULL;
    }
    RtlZeroMemory(NewNode, sizeof(B_TREE_FILENAME_NODE));

    // Create the first key
    CurrentKey = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_KEY), TAG_NTFS);
    if (!CurrentKey)
    {
        DPRINT1("ERROR: Failed to allocate memory for key!\n");
        ExFreePoolWithTag(NewNode, TAG_NTFS);
        return NULL;
    }
    RtlZeroMemory(CurrentKey, sizeof(B_TREE_KEY));
    NewNode->FirstKey = CurrentKey;

    // Allocate memory for the node buffer
    NodeBuffer = ExAllocatePoolWithTag(NonPagedPool, IndexBufferSize, TAG_NTFS);
    if (!NodeBuffer)
    {
        DPRINT1("ERROR: Couldn't allocate memory for node buffer!\n");
        ExFreePoolWithTag(CurrentKey, TAG_NTFS);
        ExFreePoolWithTag(NewNode, TAG_NTFS);
        return NULL;
    }

    // Calculate offset into index allocation
    IndexNodeOffset = GetAllocationOffsetFromVCN(Vcb, IndexBufferSize, *VCN);

    // TODO: Confirm index bitmap has this node marked as in-use

    // Read the node
    BytesRead = ReadAttribute(Vcb,
                              IndexAllocationAttributeCtx,
                              IndexNodeOffset,
                              (PCHAR)NodeBuffer,
                              IndexBufferSize);

    // Apply the fixup array to the node buffer. What is on disk at the VCN the parent
    // entry names must be that index buffer; anything else is a corrupt index.
    Status = STATUS_FILE_CORRUPT_ERROR;
    if (BytesRead == IndexBufferSize && NodeBuffer->Ntfs.Type == NRH_INDX_TYPE && NodeBuffer->VCN == *VCN)
        Status = FixupUpdateSequenceArray(Vcb, &NodeBuffer->Ntfs);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Index buffer at VCN %I64u is unreadable or corrupt!\n", *VCN);
        ExFreePoolWithTag(NodeBuffer, TAG_NTFS);
        ExFreePoolWithTag(CurrentKey, TAG_NTFS);
        ExFreePoolWithTag(NewNode, TAG_NTFS);
        return NULL;
    }

    // Walk through the index and create keys for all the entries
    FirstNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)(&NodeBuffer->Header)
                                               + NodeBuffer->Header.FirstEntryOffset);
    CurrentNodeEntry = FirstNodeEntry;
    while (CurrentEntryOffset < NodeBuffer->Header.TotalSizeOfEntries)
    {
        // Allocate memory for the current entry
        CurrentKey->IndexEntry = ExAllocatePoolWithTag(NonPagedPool, CurrentNodeEntry->Length, TAG_NTFS);
        if (!CurrentKey->IndexEntry)
        {
            DPRINT1("ERROR: Couldn't allocate memory for next key!\n");
            DestroyBTreeNode(NewNode);
            ExFreePoolWithTag(NodeBuffer, TAG_NTFS);
            return NULL;
        }

        NewNode->KeyCount++;

        // If this isn't the last entry
        if (!(CurrentNodeEntry->Flags & NTFS_INDEX_ENTRY_END))
        {
            // Create the next key
            PB_TREE_KEY NextKey = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_KEY), TAG_NTFS);
            if (!NextKey)
            {
                DPRINT1("ERROR: Couldn't allocate memory for next key!\n");
                DestroyBTreeNode(NewNode);
                ExFreePoolWithTag(NodeBuffer, TAG_NTFS);
                return NULL;
            }
            RtlZeroMemory(NextKey, sizeof(B_TREE_KEY));

            // Add NextKey to the end of the list
            CurrentKey->NextKey = NextKey;

            // Copy the current entry to its key. Its child, if any, is loaded on demand.
            RtlCopyMemory(CurrentKey->IndexEntry, CurrentNodeEntry, CurrentNodeEntry->Length);

            CurrentKey = NextKey;
        }
        else
        {
            // Copy the final entry to its key
            RtlCopyMemory(CurrentKey->IndexEntry, CurrentNodeEntry, CurrentNodeEntry->Length);
            CurrentKey->NextKey = NULL;

            break;
        }

        // Advance to the next entry
        CurrentEntryOffset += CurrentNodeEntry->Length;
        CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)CurrentNodeEntry + CurrentNodeEntry->Length);
    }

    NewNode->VCN = *VCN;
    NewNode->HasValidVCN = TRUE;

    ExFreePoolWithTag(NodeBuffer, TAG_NTFS);

    return NewNode;
}

/**
* @name CreateBTreeFromIndex
* @implemented
*
* Parse an index and create a B-Tree in memory from it.
*
* @param IndexRootContext
* Pointer to an NTFS_ATTR_CONTEXT that describes the location of the index root attribute.
*
* @param NewTree
* Pointer to a PB_TREE that will receive the pointer to a newly-created B-Tree.
*
* @returns
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
*
* @remarks
* Only the root node is read here; the nodes below it are read by NtfsLoadChildNode() as a
* descent reaches them, so a lookup, insert or remove reads one path, not the whole index.
* Caller is responsible for destroying the tree with DestroyBTree().
*/
NTSTATUS
CreateBTreeFromIndex(PDEVICE_EXTENSION Vcb,
                     PFILE_RECORD_HEADER FileRecordWithIndex,
                     /*PCWSTR IndexName,*/
                     PNTFS_ATTR_CONTEXT IndexRootContext,
                     PINDEX_ROOT_ATTRIBUTE IndexRoot,
                     PB_TREE *NewTree)
{
    PINDEX_ENTRY_ATTRIBUTE CurrentNodeEntry;
    PB_TREE Tree = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE), TAG_NTFS);
    PB_TREE_FILENAME_NODE RootNode = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_FILENAME_NODE), TAG_NTFS);
    PB_TREE_KEY CurrentKey = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_KEY), TAG_NTFS);
    ULONG CurrentOffset = IndexRoot->Header.FirstEntryOffset;
    PNTFS_ATTR_CONTEXT IndexAllocationContext = NULL;
    NTSTATUS Status;

    DPRINT("CreateBTreeFromIndex(%p, %p)\n", IndexRoot, NewTree);

    if (!Tree || !RootNode || !CurrentKey)
    {
        DPRINT1("Couldn't allocate enough memory for B-Tree!\n");
        if (Tree)
            ExFreePoolWithTag(Tree, TAG_NTFS);
        if (CurrentKey)
            ExFreePoolWithTag(CurrentKey, TAG_NTFS);
        if (RootNode)
            ExFreePoolWithTag(RootNode, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Tree, sizeof(B_TREE));
    RtlZeroMemory(RootNode, sizeof(B_TREE_FILENAME_NODE));
    RtlZeroMemory(CurrentKey, sizeof(B_TREE_KEY));

    // The tree describes the index the root belongs to
    SetBTreeIndex(Tree,
                  (PWCHAR)((ULONG_PTR)IndexRootContext->pRecord + IndexRootContext->pRecord->NameOffset),
                  IndexRootContext->pRecord->NameLength,
                  IndexRoot->AttributeType,
                  IndexRoot->CollationRule);

    // See if the file record has an attribute allocation
    Status = FindAttribute(Vcb,
                           FileRecordWithIndex,
                           AttributeIndexAllocation,
                           Tree->IndexName,
                           Tree->IndexNameLength,
                           &IndexAllocationContext,
                           NULL);
    if (!NT_SUCCESS(Status))
        IndexAllocationContext = NULL;

    // Setup the Tree. It owns the index allocation context from here on, for the child
    // nodes it loads later; DestroyBTree() releases it.
    RootNode->FirstKey = CurrentKey;
    Tree->RootNode = RootNode;
    Tree->Vcb = Vcb;
    Tree->IndexAllocationContext = IndexAllocationContext;

    // Make sure we won't try reading past the attribute-end
    if (FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header) + IndexRoot->Header.TotalSizeOfEntries > IndexRootContext->pRecord->Resident.ValueLength)
    {
        DPRINT1("Filesystem corruption detected!\n");
        DestroyBTree(Tree);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    // Start at the first node entry
    CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)IndexRoot
                                                + FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header)
                                                + IndexRoot->Header.FirstEntryOffset);

    // Create a key for each entry in the node
    while (CurrentOffset < IndexRoot->Header.TotalSizeOfEntries)
    {
        // Allocate memory for the current entry
        CurrentKey->IndexEntry = ExAllocatePoolWithTag(NonPagedPool, CurrentNodeEntry->Length, TAG_NTFS);
        if (!CurrentKey->IndexEntry)
        {
            DPRINT1("ERROR: Couldn't allocate memory for next key!\n");
            DestroyBTree(Tree);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RootNode->KeyCount++;

        // If this isn't the last entry
        if (!(CurrentNodeEntry->Flags & NTFS_INDEX_ENTRY_END))
        {
            // Create the next key
            PB_TREE_KEY NextKey = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_KEY), TAG_NTFS);
            if (!NextKey)
            {
                DPRINT1("ERROR: Couldn't allocate memory for next key!\n");
                DestroyBTree(Tree);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            RtlZeroMemory(NextKey, sizeof(B_TREE_KEY));

            // Add NextKey to the end of the list
            CurrentKey->NextKey = NextKey;

            // Copy the current entry to its key. Its child, if any, is loaded on demand.
            RtlCopyMemory(CurrentKey->IndexEntry, CurrentNodeEntry, CurrentNodeEntry->Length);

            // Advance to the next entry
            CurrentOffset += CurrentNodeEntry->Length;
            CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)CurrentNodeEntry + CurrentNodeEntry->Length);
            CurrentKey = NextKey;
        }
        else
        {
            // Copy the final entry to its key
            RtlCopyMemory(CurrentKey->IndexEntry, CurrentNodeEntry, CurrentNodeEntry->Length);
            CurrentKey->NextKey = NULL;

            break;
        }
    }

    *NewTree = Tree;

    return STATUS_SUCCESS;
}

/**
* @name GetSizeOfIndexEntries
* @implemented
*
* Sums the size of each index entry in every key in a B-Tree node.
*
* @param Node
* Pointer to a B_TREE_FILENAME_NODE. The size of this node's index entries will be returned.
*
* @returns
* The sum of the sizes of every index entry for each key in the B-Tree node.
*
* @remarks
* Gets only the size of the index entries; doesn't include the size of any headers that would be added to an index record.
*/
ULONG
GetSizeOfIndexEntries(PB_TREE_FILENAME_NODE Node)
{
    // Start summing the total size of this node's entries
    ULONG NodeSize = 0;

    // Walk through the list of Node Entries
    PB_TREE_KEY CurrentKey = Node->FirstKey;
    ULONG i;
    for (i = 0; i < Node->KeyCount; i++)
    {
        ASSERT(CurrentKey->IndexEntry->Length != 0);

        // Add the length of the current node
        NodeSize += CurrentKey->IndexEntry->Length;
        CurrentKey = CurrentKey->NextKey;
    }

    return NodeSize;
}

/**
* @name CreateIndexRootFromBTree
* @implemented
*
* Parse a B-Tree in memory and convert it into an index that can be written to disk.
*
* @param DeviceExt
* Pointer to the DEVICE_EXTENSION of the target drive.
*
* @param Tree
* Pointer to a B_TREE that describes the index to be written.
*
* @param MaxIndexSize
* Describes how large the index can be before it will take too much space in the file record.
* This is strictly the sum of the sizes of all index entries; it does not include the space
* required by the index root header (INDEX_ROOT_ATTRIBUTE), since that size will be constant.
*
* After reaching MaxIndexSize, an index can no longer be represented with just an index root
* attribute, and will require an index allocation and $I30 bitmap (TODO).
*
* @param IndexRoot
* Pointer to a PINDEX_ROOT_ATTRIBUTE that will receive a pointer to the newly-created index.
*
* @param Length
* Pointer to a ULONG which will receive the length of the new index root.
*
* @returns
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
* STATUS_NOT_IMPLEMENTED if the new index can't fit within MaxIndexSize.
*
* @remarks
* If the function succeeds, it's the caller's responsibility to free IndexRoot with ExFreePoolWithTag().
*/
NTSTATUS
CreateIndexRootFromBTree(PDEVICE_EXTENSION DeviceExt,
                         PB_TREE Tree,
                         ULONG MaxIndexSize,
                         PINDEX_ROOT_ATTRIBUTE *IndexRoot,
                         ULONG *Length)
{
    ULONG i;
    PB_TREE_KEY CurrentKey;
    PINDEX_ENTRY_ATTRIBUTE CurrentNodeEntry;
    PINDEX_ROOT_ATTRIBUTE NewIndexRoot = ExAllocatePoolWithTag(NonPagedPool,
                                                               DeviceExt->NtfsInfo.BytesPerFileRecord,
                                                               TAG_NTFS);

    DPRINT("CreateIndexRootFromBTree(%p, %p, 0x%lx, %p, %p)\n", DeviceExt, Tree, MaxIndexSize, IndexRoot, Length);

#ifndef NDEBUG
    DumpBTree(Tree);
#endif

    if (!NewIndexRoot)
    {
        DPRINT1("Failed to allocate memory for Index Root!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Setup the new index root
    RtlZeroMemory(NewIndexRoot, DeviceExt->NtfsInfo.BytesPerFileRecord);

    NewIndexRoot->AttributeType = Tree->IndexedAttributeType;
    NewIndexRoot->CollationRule = Tree->CollationRule;
    NewIndexRoot->SizeOfEntry = DeviceExt->NtfsInfo.BytesPerIndexRecord;
    // If Bytes per index record is less than cluster size, clusters per index record becomes sectors per index
    if (NewIndexRoot->SizeOfEntry < DeviceExt->NtfsInfo.BytesPerCluster)
        NewIndexRoot->ClustersPerIndexRecord = NewIndexRoot->SizeOfEntry / DeviceExt->NtfsInfo.BytesPerSector;
    else
        NewIndexRoot->ClustersPerIndexRecord = NewIndexRoot->SizeOfEntry / DeviceExt->NtfsInfo.BytesPerCluster;

    // Setup the Index node header
    NewIndexRoot->Header.FirstEntryOffset = sizeof(INDEX_HEADER_ATTRIBUTE);
    NewIndexRoot->Header.Flags = INDEX_ROOT_SMALL;

    // Start summing the total size of this node's entries
    NewIndexRoot->Header.TotalSizeOfEntries = NewIndexRoot->Header.FirstEntryOffset;

    // Setup each Node Entry
    CurrentKey = Tree->RootNode->FirstKey;
    CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)NewIndexRoot
                                                + FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header)
                                                + NewIndexRoot->Header.FirstEntryOffset);
    for (i = 0; i < Tree->RootNode->KeyCount; i++)
    {
        // Would adding the current entry to the index increase the index size beyond the limit we've set?
        ULONG IndexSize = NewIndexRoot->Header.TotalSizeOfEntries - NewIndexRoot->Header.FirstEntryOffset + CurrentKey->IndexEntry->Length;
        if (IndexSize > MaxIndexSize)
        {
            DPRINT1("TODO: Adding file would require creating an attribute list!\n");
            ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
            return STATUS_NOT_IMPLEMENTED;
        }

        ASSERT(CurrentKey->IndexEntry->Length != 0);

        // Copy the index entry
        RtlCopyMemory(CurrentNodeEntry, CurrentKey->IndexEntry, CurrentKey->IndexEntry->Length);

        DPRINT("Index Node Entry Stream Length: %u\nIndex Node Entry Length: %u\n",
                CurrentNodeEntry->KeyLength,
                CurrentNodeEntry->Length);

        // Does the current key have any sub-nodes, in memory or still on disk?
        if (CurrentKey->LesserChild ||
            BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE))
        {
            NewIndexRoot->Header.Flags = INDEX_ROOT_LARGE;
        }

        // Add Length of Current Entry to Total Size of Entries
        NewIndexRoot->Header.TotalSizeOfEntries += CurrentKey->IndexEntry->Length;

        // Go to the next node entry
        CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)CurrentNodeEntry + CurrentNodeEntry->Length);

        CurrentKey = CurrentKey->NextKey;
    }

    NewIndexRoot->Header.AllocatedSize = NewIndexRoot->Header.TotalSizeOfEntries;

    *IndexRoot = NewIndexRoot;
    *Length = NewIndexRoot->Header.AllocatedSize + FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header);

    return STATUS_SUCCESS;
}

NTSTATUS
CreateIndexBufferFromBTreeNode(PDEVICE_EXTENSION DeviceExt,
                               PB_TREE_FILENAME_NODE Node,
                               ULONG BufferSize,
                               BOOLEAN HasChildren,
                               PINDEX_BUFFER IndexBuffer)
{
    ULONG i;
    PB_TREE_KEY CurrentKey;
    PINDEX_ENTRY_ATTRIBUTE CurrentNodeEntry;
    NTSTATUS Status;

    // TODO: Fix magic, do math
    RtlZeroMemory(IndexBuffer, BufferSize);
    IndexBuffer->Ntfs.Type = NRH_INDX_TYPE;
    IndexBuffer->Ntfs.UsaOffset = 0x28;
    IndexBuffer->Ntfs.UsaCount = 9;

    // TODO: Check bitmap for VCN
    ASSERT(Node->HasValidVCN);
    IndexBuffer->VCN = Node->VCN;

    // Windows seems to alternate between using 0x28 and 0x40 for the first entry offset of each index buffer.
    // Interestingly, neither Windows nor chkdsk seem to mind if we just use 0x28 for every index record.
    IndexBuffer->Header.FirstEntryOffset = 0x28;
    IndexBuffer->Header.AllocatedSize = BufferSize - FIELD_OFFSET(INDEX_BUFFER, Header);

    // Start summing the total size of this node's entries
    IndexBuffer->Header.TotalSizeOfEntries = IndexBuffer->Header.FirstEntryOffset;

    CurrentKey = Node->FirstKey;
    CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&(IndexBuffer->Header)
                                                + IndexBuffer->Header.FirstEntryOffset);
    for (i = 0; i < Node->KeyCount; i++)
    {
        // Would adding the current entry to the index increase the node size beyond the allocation size?
        ULONG IndexSize = FIELD_OFFSET(INDEX_BUFFER, Header)
            + IndexBuffer->Header.TotalSizeOfEntries
            + CurrentKey->IndexEntry->Length;
        if (IndexSize > BufferSize)
        {
            DPRINT1("TODO: Adding file would require creating a new node!\n");
            return STATUS_NOT_IMPLEMENTED;
        }

        ASSERT(CurrentKey->IndexEntry->Length != 0);

        // Copy the index entry
        RtlCopyMemory(CurrentNodeEntry, CurrentKey->IndexEntry, CurrentKey->IndexEntry->Length);

        DPRINT("Index Node Entry Stream Length: %u\nIndex Node Entry Length: %u\n",
               CurrentNodeEntry->KeyLength,
               CurrentNodeEntry->Length);

        // Add Length of Current Entry to Total Size of Entries
        IndexBuffer->Header.TotalSizeOfEntries += CurrentNodeEntry->Length;

        // Check for child nodes
        if (HasChildren)
            IndexBuffer->Header.Flags = INDEX_NODE_LARGE;

        // Go to the next node entry
        CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)CurrentNodeEntry + CurrentNodeEntry->Length);
        CurrentKey = CurrentKey->NextKey;
    }

    Status = AddFixupArray(DeviceExt, &IndexBuffer->Ntfs);

    return Status;
}

/**
* @name DemoteBTreeRoot
* @implemented
*
* Demoting the root means first putting all the keys in the root node into a new node, and making
* the new node a child of a dummy key. The dummy key then becomes the sole contents of the root node.
* The B-Tree gets one level deeper. This operation is needed when an index root grows too large for its file record.
* Demotion is my own term; I might change the name later if I think of something more descriptive or can find
* an appropriate name for this operation in existing B-Tree literature.
*
* @param Tree
* Pointer to the B_TREE whose root is being demoted
*
* @returns
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
*/
NTSTATUS
DemoteBTreeRoot(PB_TREE Tree)
{
    PB_TREE_FILENAME_NODE NewSubNode, NewIndexRoot;
    PB_TREE_KEY DummyKey;

    DPRINT("Collapsing Index Root into sub-node.\n");

#ifndef NDEBUG
    DumpBTree(Tree);
#endif

    // Create a new node that will hold the keys currently in index root
    NewSubNode = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_FILENAME_NODE), TAG_NTFS);
    if (!NewSubNode)
    {
        DPRINT1("ERROR: Couldn't allocate memory for new sub-node.\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(NewSubNode, sizeof(B_TREE_FILENAME_NODE));

    // Copy the applicable data from the old index root node
    NewSubNode->KeyCount = Tree->RootNode->KeyCount;
    NewSubNode->FirstKey = Tree->RootNode->FirstKey;
    NewSubNode->DiskNeedsUpdating = TRUE;

    // Create a new dummy key, and make the new node it's child
    DummyKey = CreateDummyKey(TRUE);
    if (!DummyKey)
    {
        DPRINT1("ERROR: Couldn't allocate memory for new root node.\n");
        ExFreePoolWithTag(NewSubNode, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Make the new node a child of the dummy key
    DummyKey->LesserChild = NewSubNode;

    // Create a new index root node
    NewIndexRoot = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_FILENAME_NODE), TAG_NTFS);
    if (!NewIndexRoot)
    {
        DPRINT1("ERROR: Couldn't allocate memory for new index root.\n");
        ExFreePoolWithTag(NewSubNode, TAG_NTFS);
        ExFreePoolWithTag(DummyKey, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(NewIndexRoot, sizeof(B_TREE_FILENAME_NODE));

    NewIndexRoot->DiskNeedsUpdating = TRUE;

    // Insert the dummy key into the new node
    NewIndexRoot->FirstKey = DummyKey;
    NewIndexRoot->KeyCount = 1;
    NewIndexRoot->DiskNeedsUpdating = TRUE;

    // Make the new node the Tree's root node
    Tree->RootNode = NewIndexRoot;

#ifndef NDEBUG
    DumpBTree(Tree);
#endif

    return STATUS_SUCCESS;
}

/**
* @name SetIndexEntryVCN
* @implemented
*
* Sets the VCN of a given IndexEntry.
*
* @param IndexEntry
* Pointer to an INDEX_ENTRY_ATTRIBUTE structure that will have its VCN set.
*
* @param VCN
* VCN to store in the index entry.
*
* @remarks
* The index entry must have enough memory allocated to store the VCN, and must have the NTFS_INDEX_ENTRY_NODE flag set.
* The VCN of an index entry is stored at the very end of the structure, after the filename attribute. Since the filename
* attribute can be a variable size, this function makes setting this member easy.
*/
VOID
SetIndexEntryVCN(PINDEX_ENTRY_ATTRIBUTE IndexEntry, ULONGLONG VCN)
{
    PULONGLONG Destination = (PULONGLONG)((ULONG_PTR)IndexEntry + IndexEntry->Length - sizeof(ULONGLONG));

    ASSERT(IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE);

    *Destination = VCN;
}

NTSTATUS
UpdateIndexAllocation(PDEVICE_EXTENSION DeviceExt,
                      PB_TREE Tree,
                      ULONG IndexBufferSize,
                      PFILE_RECORD_HEADER FileRecord)
{
    // Find the index allocation and bitmap
    PNTFS_ATTR_CONTEXT IndexAllocationContext;
    PB_TREE_KEY CurrentKey;
    NTSTATUS Status;
    BOOLEAN HasIndexAllocation = FALSE;
    ULONG i;
    ULONG IndexAllocationOffset;

    DPRINT("UpdateIndexAllocation() called.\n");

    Status = FindAttribute(DeviceExt, FileRecord, AttributeIndexAllocation, Tree->IndexName, Tree->IndexNameLength, &IndexAllocationContext, &IndexAllocationOffset);
    if (NT_SUCCESS(Status))
    {
        HasIndexAllocation = TRUE;

#ifndef NDEBUG
        PrintAllVCNs(DeviceExt,
                     IndexAllocationContext,
                     IndexBufferSize);
#endif
    }
    // Walk through the root node and update all the sub-nodes
    CurrentKey = Tree->RootNode->FirstKey;
    for (i = 0; i < Tree->RootNode->KeyCount; i++)
    {
        if (CurrentKey->LesserChild)
        {
            if (!HasIndexAllocation)
            {
                // The index needs an allocation and its bitmap, where their types sort them
                DPRINT("Adding index allocation...\n");

                Status = AddIndexAllocation(DeviceExt, FileRecord, Tree->IndexName, Tree->IndexNameLength);
                if (NT_SUCCESS(Status))
                    Status = AddBitmap(DeviceExt, FileRecord, Tree->IndexName, Tree->IndexNameLength);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Failed to add index allocation and bitmap!\n");
                    return Status;
                }

                // Find the new attribute
                Status = FindAttribute(DeviceExt, FileRecord, AttributeIndexAllocation, Tree->IndexName, Tree->IndexNameLength, &IndexAllocationContext, &IndexAllocationOffset);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Couldn't find newly-created index allocation!\n");
                    return Status;
                }

                HasIndexAllocation = TRUE;
            }

            // Is the Index Entry large enough to store the VCN?
            if (!BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE))
            {
                // Allocate memory for the larger index entry
                PINDEX_ENTRY_ATTRIBUTE NewEntry = ExAllocatePoolWithTag(NonPagedPool,
                                                                        CurrentKey->IndexEntry->Length + sizeof(ULONGLONG),
                                                                        TAG_NTFS);
                if (!NewEntry)
                {
                    DPRINT1("ERROR: Unable to allocate memory for new index entry!\n");
                    if (HasIndexAllocation)
                        ReleaseAttributeContext(IndexAllocationContext);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                // Copy the old entry to the new one
                RtlCopyMemory(NewEntry, CurrentKey->IndexEntry, CurrentKey->IndexEntry->Length);

                NewEntry->Length += sizeof(ULONGLONG);

                // Free the old memory
                ExFreePoolWithTag(CurrentKey->IndexEntry, TAG_NTFS);

                CurrentKey->IndexEntry = NewEntry;
                CurrentKey->IndexEntry->Flags |= NTFS_INDEX_ENTRY_NODE;
            }

            // Update the sub-node
            Status = UpdateIndexNode(DeviceExt, Tree, FileRecord, CurrentKey->LesserChild, IndexBufferSize, IndexAllocationContext, IndexAllocationOffset);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR: Failed to update index node!\n");
                ReleaseAttributeContext(IndexAllocationContext);
                return Status;
            }

            // Update the VCN stored in the index entry of CurrentKey
            SetIndexEntryVCN(CurrentKey->IndexEntry, CurrentKey->LesserChild->VCN);
        }
        CurrentKey = CurrentKey->NextKey;
    }

#ifndef NDEBUG
    DumpBTree(Tree);
#endif

    if (HasIndexAllocation)
    {
#ifndef NDEBUG
        PrintAllVCNs(DeviceExt,
                     IndexAllocationContext,
                     IndexBufferSize);
#endif
        ReleaseAttributeContext(IndexAllocationContext);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
UpdateIndexNode(PDEVICE_EXTENSION DeviceExt,
                PB_TREE Tree,
                PFILE_RECORD_HEADER FileRecord,
                PB_TREE_FILENAME_NODE Node,
                ULONG IndexBufferSize,
                PNTFS_ATTR_CONTEXT IndexAllocationContext,
                ULONG IndexAllocationOffset)
{
    ULONG i;
    PB_TREE_KEY CurrentKey = Node->FirstKey;
    BOOLEAN HasChildren = FALSE;
    NTSTATUS Status;


    DPRINT("UpdateIndexNode(%p, %p, %p, %lu, %p, %lu) called for index node with VCN %I64u\n",
           DeviceExt,
           FileRecord,
           Node,
           IndexBufferSize,
           IndexAllocationContext,
           IndexAllocationOffset,
           Node->VCN);

    // Walk through the node and look for children to update
    for (i = 0; i < Node->KeyCount; i++)
    {
        ASSERT(CurrentKey);

        // If there's a child node
        if (CurrentKey->LesserChild)
        {
            HasChildren = TRUE;

            // Update the child node on disk
            Status = UpdateIndexNode(DeviceExt, Tree, FileRecord, CurrentKey->LesserChild, IndexBufferSize, IndexAllocationContext, IndexAllocationOffset);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR: Failed to update child node!\n");
                return Status;
            }

            // Is the Index Entry large enough to store the VCN?
            if (!BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE))
            {
                // Allocate memory for the larger index entry
                PINDEX_ENTRY_ATTRIBUTE NewEntry = ExAllocatePoolWithTag(NonPagedPool,
                                                                        CurrentKey->IndexEntry->Length + sizeof(ULONGLONG),
                                                                        TAG_NTFS);
                if (!NewEntry)
                {
                    DPRINT1("ERROR: Unable to allocate memory for new index entry!\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                // Copy the old entry to the new one
                RtlCopyMemory(NewEntry, CurrentKey->IndexEntry, CurrentKey->IndexEntry->Length);

                NewEntry->Length += sizeof(ULONGLONG);

                // Free the old memory
                ExFreePoolWithTag(CurrentKey->IndexEntry, TAG_NTFS);

                CurrentKey->IndexEntry = NewEntry;
            }

            // Update the VCN stored in the index entry of CurrentKey
            SetIndexEntryVCN(CurrentKey->IndexEntry, CurrentKey->LesserChild->VCN);

            CurrentKey->IndexEntry->Flags |= NTFS_INDEX_ENTRY_NODE;
        }
        else if (BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE))
        {
            // The child was never loaded, so it's unchanged on disk at the VCN the entry holds
            HasChildren = TRUE;
        }

        CurrentKey = CurrentKey->NextKey;
    }


    // Do we need to write this node to disk?
    if (Node->DiskNeedsUpdating)
    {
        ULONGLONG NodeOffset;
        ULONG LengthWritten;
        PINDEX_BUFFER IndexBuffer;

        // Does the node need to be assigned a VCN?
        if (!Node->HasValidVCN)
        {
            // Allocate the node
            Status = AllocateIndexNode(DeviceExt,
                                       Tree,
                                       FileRecord,
                                       IndexBufferSize,
                                       IndexAllocationContext,
                                       IndexAllocationOffset,
                                       &Node->VCN);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR: Failed to allocate index record in index allocation!\n");
                return Status;
            }

            Node->HasValidVCN = TRUE;
        }

        // Allocate memory for an index buffer
        IndexBuffer = ExAllocatePoolWithTag(NonPagedPool, IndexBufferSize, TAG_NTFS);
        if (!IndexBuffer)
        {
            DPRINT1("ERROR: Failed to allocate %lu bytes for index buffer!\n", IndexBufferSize);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // Create the index buffer we'll be writing to disk to represent this node
        Status = CreateIndexBufferFromBTreeNode(DeviceExt, Node, IndexBufferSize, HasChildren, IndexBuffer);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ERROR: Failed to create index buffer from node!\n");
            ExFreePoolWithTag(IndexBuffer, TAG_NTFS);
            return Status;
        }

        // Get Offset of index buffer in index allocation
        NodeOffset = GetAllocationOffsetFromVCN(DeviceExt, IndexBufferSize, Node->VCN);

        // Write the buffer to the index allocation
        Status = WriteAttribute(DeviceExt, IndexAllocationContext, NodeOffset, (const PUCHAR)IndexBuffer, IndexBufferSize, &LengthWritten, FileRecord);
        if (!NT_SUCCESS(Status) || LengthWritten != IndexBufferSize)
        {
            DPRINT1("ERROR: Failed to update index allocation!\n");
            ExFreePoolWithTag(IndexBuffer, TAG_NTFS);
            if (!NT_SUCCESS(Status))
                return Status;
            else
                return STATUS_END_OF_FILE;
        }

        Node->DiskNeedsUpdating = FALSE;

        // Free the index buffer
        ExFreePoolWithTag(IndexBuffer, TAG_NTFS);
    }

    return STATUS_SUCCESS;
}

PB_TREE_KEY
CreateBTreeKeyFromFilename(ULONGLONG FileReference, PFILENAME_ATTRIBUTE FileNameAttribute)
{
    PB_TREE_KEY NewKey;
    ULONG AttributeSize = GetFileNameAttributeLength(FileNameAttribute);
    ULONG EntrySize = ALIGN_UP_BY(AttributeSize + FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName), 8);

    // Create a new Index Entry for the file
    PINDEX_ENTRY_ATTRIBUTE NewEntry = ExAllocatePoolWithTag(NonPagedPool, EntrySize, TAG_NTFS);
    if (!NewEntry)
    {
        DPRINT1("ERROR: Failed to allocate memory for Index Entry!\n");
        return NULL;
    }

    // Setup the Index Entry
    RtlZeroMemory(NewEntry, EntrySize);
    NewEntry->Data.Directory.IndexedFile = FileReference;
    NewEntry->Length = EntrySize;
    NewEntry->KeyLength = AttributeSize;

    // Copy the FileNameAttribute
    RtlCopyMemory(&NewEntry->FileName, FileNameAttribute, AttributeSize);

    NewKey = CreateBTreeKeyForEntry(NewEntry);
    if (!NewKey)
        ExFreePoolWithTag(NewEntry, TAG_NTFS);

    return NewKey;
}

/**
* @name CreateBTreeKeyForEntry
* @implemented
*
* Wraps an index entry in a new key. The key owns Entry from then on.
*/
static
PB_TREE_KEY
CreateBTreeKeyForEntry(PINDEX_ENTRY_ATTRIBUTE Entry)
{
    PB_TREE_KEY NewKey = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_KEY), TAG_NTFS);
    if (!NewKey)
    {
        DPRINT1("ERROR: Failed to allocate memory for new key!\n");
        return NULL;
    }

    RtlZeroMemory(NewKey, sizeof(B_TREE_KEY));
    NewKey->IndexEntry = Entry;

    return NewKey;
}

/**
* @name CreateBTreeKeyFromEntry
* @implemented
*
* Creates a key holding a copy of an index entry that has no child.
*
* @param Entry
* The index entry to copy, as NtfsAllocateViewIndexEntry() or CreateBTreeKeyFromFilename() built it.
*
* @return
* The new key, or NULL if an allocation fails.
*/
PB_TREE_KEY
CreateBTreeKeyFromEntry(PINDEX_ENTRY_ATTRIBUTE Entry)
{
    PINDEX_ENTRY_ATTRIBUTE NewEntry;
    PB_TREE_KEY NewKey;

    ASSERT(!BooleanFlagOn(Entry->Flags, NTFS_INDEX_ENTRY_NODE | NTFS_INDEX_ENTRY_END));

    NewEntry = ExAllocatePoolWithTag(NonPagedPool, Entry->Length, TAG_NTFS);
    if (!NewEntry)
    {
        DPRINT1("ERROR: Failed to allocate memory for Index Entry!\n");
        return NULL;
    }
    RtlCopyMemory(NewEntry, Entry, Entry->Length);

    NewKey = CreateBTreeKeyForEntry(NewEntry);
    if (!NewKey)
        ExFreePoolWithTag(NewEntry, TAG_NTFS);

    return NewKey;
}

/**
* @name NtfsAllocateViewIndexEntry
* @implemented
*
* Builds the index entry of a view index ($O, $Q, $SII, $SDH, ...): the key bytes follow the
* entry header, and the data follows the key.
*
* @param Key
* The key bytes, compared under the index's collation rule.
*
* @param Data
* The data the entry carries, or NULL for an entry that's only used to search by Key.
*
* @return
* The entry, which the caller frees with ExFreePoolWithTag(TAG_NTFS), or NULL if the allocation fails.
*/
PINDEX_ENTRY_ATTRIBUTE
NtfsAllocateViewIndexEntry(PVOID Key,
                           USHORT KeyLength,
                           PVOID Data,
                           USHORT DataLength)
{
    PINDEX_ENTRY_ATTRIBUTE Entry;
    USHORT DataOffset = FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName) + KeyLength;
    USHORT EntrySize = ALIGN_UP_BY(DataOffset + DataLength, 8);

    Entry = ExAllocatePoolWithTag(NonPagedPool, EntrySize, TAG_NTFS);
    if (!Entry)
        return NULL;

    RtlZeroMemory(Entry, EntrySize);
    Entry->Data.ViewIndex.DataOffset = DataOffset;
    Entry->Data.ViewIndex.DataLength = DataLength;
    Entry->Length = EntrySize;
    Entry->KeyLength = KeyLength;
    RtlCopyMemory(&Entry->FileName, Key, KeyLength);
    if (Data)
        RtlCopyMemory((PUCHAR)Entry + DataOffset, Data, DataLength);

    return Entry;
}

VOID
DestroyBTreeKey(PB_TREE_KEY Key)
{
    if (Key->IndexEntry)
        ExFreePoolWithTag(Key->IndexEntry, TAG_NTFS);

    if (Key->LesserChild)
        DestroyBTreeNode(Key->LesserChild);

    ExFreePoolWithTag(Key, TAG_NTFS);
}

VOID
DestroyBTreeNode(PB_TREE_FILENAME_NODE Node)
{
    PB_TREE_KEY NextKey;
    PB_TREE_KEY CurrentKey = Node->FirstKey;
    ULONG i;
    for (i = 0; i < Node->KeyCount; i++)
    {
        NT_ASSERT(CurrentKey);
        NextKey = CurrentKey->NextKey;
        DestroyBTreeKey(CurrentKey);
        CurrentKey = NextKey;
    }

    NT_ASSERT(CurrentKey == NULL);

    ExFreePoolWithTag(Node, TAG_NTFS);
}

/**
* @name DestroyBTree
* @implemented
*
* Destroys a B-Tree.
*
* @param Tree
* Pointer to the B_TREE which will be destroyed.
*
* @remarks
* Destroys every bit of data stored in the tree.
*/
VOID
DestroyBTree(PB_TREE Tree)
{
    DestroyBTreeNode(Tree->RootNode);
    if (Tree->IndexAllocationContext)
        ReleaseAttributeContext(Tree->IndexAllocationContext);
    ExFreePoolWithTag(Tree, TAG_NTFS);
}

VOID
DumpBTreeKey(PB_TREE Tree, PB_TREE_KEY Key, ULONG Number, ULONG Depth)
{
    ULONG i;
    for (i = 0; i < Depth; i++)
        DbgPrint(" ");
    DbgPrint(" Key #%d", Number);

    if (!(Key->IndexEntry->Flags & NTFS_INDEX_ENTRY_END) && Tree->CollationRule != COLLATION_FILE_NAME)
    {
        DbgPrint(" (%u-byte key)\n", Key->IndexEntry->KeyLength);
    }
    else if (!(Key->IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
    {
        UNICODE_STRING FileName;
        FileName.Length = Key->IndexEntry->FileName.NameLength * sizeof(WCHAR);
        FileName.MaximumLength = FileName.Length;
        FileName.Buffer = Key->IndexEntry->FileName.Name;
        DbgPrint(" '%wZ'\n", &FileName);
    }
    else
    {
        DbgPrint(" (Dummy Key)\n");
    }

    // Is there a child node?
    if (Key->IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE)
    {
        if (Key->LesserChild)
            DumpBTreeNode(Tree, Key->LesserChild, Number, Depth + 1);
        else
            DbgPrint(" (child at VCN %I64u not loaded)\n", GetIndexEntryVCN(Key->IndexEntry));
    }
}

VOID
DumpBTreeNode(PB_TREE Tree,
              PB_TREE_FILENAME_NODE Node,
              ULONG Number,
              ULONG Depth)
{
    PB_TREE_KEY CurrentKey;
    ULONG i;
    for (i = 0; i < Depth; i++)
        DbgPrint(" ");
    DbgPrint("Node #%d, Depth %d, has %d key%s", Number, Depth, Node->KeyCount, Node->KeyCount == 1 ? "" : "s");

    if (Node->HasValidVCN)
        DbgPrint(" VCN: %I64u\n", Node->VCN);
    else if (Tree->RootNode == Node)
        DbgPrint(" Index Root");
    else
        DbgPrint(" NOT ASSIGNED VCN YET\n");

    CurrentKey = Node->FirstKey;
    for (i = 0; i < Node->KeyCount; i++)
    {
        DumpBTreeKey(Tree, CurrentKey, i, Depth);
        CurrentKey = CurrentKey->NextKey;
    }
}

/**
* @name DumpBTree
* @implemented
*
* Displays a B-Tree.
*
* @param Tree
* Pointer to the B_TREE which will be displayed.
*
* @remarks
* Displays a diagnostic summary of a B_TREE.
*/
VOID
DumpBTree(PB_TREE Tree)
{
    DbgPrint("B_TREE @ %p\n", Tree);
    DumpBTreeNode(Tree, Tree->RootNode, 0, 0);
}

// Calculates start of Index Buffer relative to the index allocation, given the node's VCN
ULONGLONG
GetAllocationOffsetFromVCN(PDEVICE_EXTENSION DeviceExt,
                           ULONG IndexBufferSize,
                           ULONGLONG Vcn)
{
    if (IndexBufferSize < DeviceExt->NtfsInfo.BytesPerCluster)
        return Vcn * DeviceExt->NtfsInfo.BytesPerSector;

    return Vcn * DeviceExt->NtfsInfo.BytesPerCluster;
}

ULONGLONG
GetIndexEntryVCN(PINDEX_ENTRY_ATTRIBUTE IndexEntry)
{
    PULONGLONG Destination = (PULONGLONG)((ULONG_PTR)IndexEntry + IndexEntry->Length - sizeof(ULONGLONG));

    ASSERT(IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE);

    return *Destination;
}

/**
* @name NtfsLoadChildNode
* @implemented
*
* Reads the child node of Key from the index allocation, unless it's already in memory.
*
* @param Tree
* The B_TREE Key belongs to.
*
* @param Key
* Key whose child is needed. Keys without NTFS_INDEX_ENTRY_NODE have no child and are left alone.
*
* @return
* STATUS_SUCCESS if Key's child is in memory (or Key has none), STATUS_FILE_CORRUPT_ERROR
* if the child can't be read.
*/
static
NTSTATUS
NtfsLoadChildNode(PB_TREE Tree, PB_TREE_KEY Key)
{
    if (Key->LesserChild != NULL ||
        !BooleanFlagOn(Key->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE))
    {
        return STATUS_SUCCESS;
    }

    // A child that isn't in memory came from disk, so the index has an allocation
    ASSERT(Tree->Vcb != NULL);

    Key->LesserChild = CreateBTreeNodeFromIndexNode(Tree->Vcb,
                                                    NULL,
                                                    Tree->IndexAllocationContext,
                                                    Key->IndexEntry);
    if (Key->LesserChild == NULL)
        return STATUS_FILE_CORRUPT_ERROR;

    return STATUS_SUCCESS;
}

/**
* @name NtfsInsertIndexKey
* @implemented
*
* Inserts a copy of an index entry into a B-Tree node.
*
* @param Tree
* Pointer to the B_TREE the key (filename attribute) is being inserted into.
*
* @param Entry
* The index entry to add, without a child. A copy will be made.
*
* @param Node
* Pointer to a B_TREE_FILENAME_NODE into which a new key will be inserted, in order.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param MaxIndexRootSize
* The maximum size, in bytes, of node entries that can be stored in the index root before it will grow too large for
* the file record. This number is just the size of the entries, without any headers for the attribute or index root.
*
* @param IndexRecordSize
* The size, in bytes, of an index record for this index. AKA an index buffer. Usually set to 4096.
*
* @param MedianKey
* Pointer to a PB_TREE_KEY that will receive a pointer to the median key, should the node grow too large and need to be split.
* Will be set to NULL if the node isn't split.
*
* @param NewRightHandSibling
* Pointer to a PB_TREE_FILENAME_NODE that will receive a pointer to a newly-created right-hand sibling node,
* should the node grow too large and need to be split. Will be set to NULL if the node isn't split.
*
* @remarks
* A node is always sorted, with the least comparable filename stored first and a dummy key to mark the end.
*/
NTSTATUS
NtfsInsertIndexKey(PB_TREE Tree,
                   PINDEX_ENTRY_ATTRIBUTE Entry,
                   PB_TREE_FILENAME_NODE Node,
                   BOOLEAN CaseSensitive,
                   ULONG MaxIndexRootSize,
                   ULONG IndexRecordSize,
                   PB_TREE_KEY *MedianKey,
                   PB_TREE_FILENAME_NODE *NewRightHandSibling)
{
    PB_TREE_KEY NewKey, CurrentKey, PreviousKey;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG NodeSize;
    ULONG AllocatedNodeSize;
    ULONG MaxNodeSizeWithoutHeader;
    ULONG i;

    *MedianKey = NULL;
    *NewRightHandSibling = NULL;

    DPRINT("NtfsInsertIndexKey(%p, %p, %p, %s, %lu, %lu, %p, %p)\n",
           Tree,
           Entry,
           Node,
           CaseSensitive ? "TRUE" : "FALSE",
           MaxIndexRootSize,
           IndexRecordSize,
           MedianKey,
           NewRightHandSibling);

    // Create the key for the entry
    NewKey = CreateBTreeKeyFromEntry(Entry);
    if (!NewKey)
        return STATUS_INSUFFICIENT_RESOURCES;

    // Find where to insert the key
    CurrentKey = Node->FirstKey;
    PreviousKey = NULL;
    for (i = 0; i < Node->KeyCount; i++)
    {
        // Should the New Key go before the current key?
        LONG Comparison = CompareTreeKeys(NewKey, CurrentKey, Tree->CollationRule, CaseSensitive);

        /* The key is already in this index. Inserting it again would leave
         * two entries the lookup code can never tell apart, so refuse. */
        if (Comparison == 0)
        {
            DPRINT1("Refusing duplicate key in index %.*S\n", Tree->IndexNameLength, Tree->IndexName);

            DestroyBTreeKey(NewKey);

            return STATUS_OBJECT_NAME_COLLISION;
        }

        // Is NewKey < CurrentKey?
        if (Comparison < 0)
        {
            Status = NtfsLoadChildNode(Tree, CurrentKey);
            if (!NT_SUCCESS(Status))
            {
                DestroyBTreeKey(NewKey);
                return Status;
            }

            // Does CurrentKey have a sub-node?
            if (CurrentKey->LesserChild)
            {
                PB_TREE_KEY NewLeftKey;
                PB_TREE_FILENAME_NODE NewChild;

                // Insert the key into the child node
                Status = NtfsInsertIndexKey(Tree,
                                            Entry,
                                            CurrentKey->LesserChild,
                                            CaseSensitive,
                                            MaxIndexRootSize,
                                            IndexRecordSize,
                                            &NewLeftKey,
                                            &NewChild);
                // The child made its own key for the entry; this level's isn't needed
                DestroyBTreeKey(NewKey);

                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Failed to insert key.\n");
                    return Status;
                }

                // Did the child node get split?
                if (NewLeftKey)
                {
                    ASSERT(NewChild != NULL);

                    // Insert the new left key to the left of the current key
                    NewLeftKey->NextKey = CurrentKey;

                    // Is CurrentKey the first key?
                    if (!PreviousKey)
                        Node->FirstKey = NewLeftKey;
                    else
                        PreviousKey->NextKey = NewLeftKey;

                    // CurrentKey->LesserChild will be the right-hand sibling
                    CurrentKey->LesserChild = NewChild;

                    Node->KeyCount++;
                    Node->DiskNeedsUpdating = TRUE;

#ifndef NDEBUG
                    DumpBTree(Tree);
#endif
                }
            }
            else
            {
                // Insert New Key before Current Key
                NewKey->NextKey = CurrentKey;

                // Increase KeyCount and mark node as dirty
                Node->KeyCount++;
                Node->DiskNeedsUpdating = TRUE;

                // was CurrentKey the first key?
                if (CurrentKey == Node->FirstKey)
                    Node->FirstKey = NewKey;
                else
                    PreviousKey->NextKey = NewKey;
            }
            break;
        }

        PreviousKey = CurrentKey;
        CurrentKey = CurrentKey->NextKey;
    }

    // Determine how much space the index entries will need
    NodeSize = GetSizeOfIndexEntries(Node);

    // Is Node not the root node?
    if (Node != Tree->RootNode)
    {
        // Calculate maximum size of index entries without any headers
        AllocatedNodeSize = IndexRecordSize - FIELD_OFFSET(INDEX_BUFFER, Header);

        // TODO: Replace magic with math
        MaxNodeSizeWithoutHeader = AllocatedNodeSize - 0x28;

        // Has the node grown larger than its allocated size?
        if (NodeSize > MaxNodeSizeWithoutHeader)
        {
            NTSTATUS Status;

            Status = SplitBTreeNode(Tree, Node, MedianKey, NewRightHandSibling, CaseSensitive);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR: Failed to split B-Tree node!\n");
                return Status;
            }

            return Status;
        }
    }

    // NewEntry and NewKey will be destroyed later by DestroyBTree()

    return Status;
}

/**
* @name NtfsInsertKey
* @implemented
*
* Inserts a FILENAME_ATTRIBUTE into a B-Tree node of a $I30 index. See NtfsInsertIndexKey().
*
* @param FileReference
* Reference number to the file being added. This will be a combination of the MFT index and update sequence number.
*
* @param FileNameAttribute
* Pointer to a FILENAME_ATTRIBUTE which is the data for the key that will be added to the tree. A copy will be made.
*/
NTSTATUS
NtfsInsertKey(PB_TREE Tree,
              ULONGLONG FileReference,
              PFILENAME_ATTRIBUTE FileNameAttribute,
              PB_TREE_FILENAME_NODE Node,
              BOOLEAN CaseSensitive,
              ULONG MaxIndexRootSize,
              ULONG IndexRecordSize,
              PB_TREE_KEY *MedianKey,
              PB_TREE_FILENAME_NODE *NewRightHandSibling)
{
    NTSTATUS Status;
    PB_TREE_KEY FileNameKey;

    ASSERT(Tree->CollationRule == COLLATION_FILE_NAME);

    FileNameKey = CreateBTreeKeyFromFilename(FileReference, FileNameAttribute);
    if (!FileNameKey)
    {
        *MedianKey = NULL;
        *NewRightHandSibling = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsInsertIndexKey(Tree,
                                FileNameKey->IndexEntry,
                                Node,
                                CaseSensitive,
                                MaxIndexRootSize,
                                IndexRecordSize,
                                MedianKey,
                                NewRightHandSibling);

    DestroyBTreeKey(FileNameKey);

    return Status;
}

/**
* @name FindLargestKeyInSubtree
* @implemented
*
* Finds the greatest key in the subtree rooted at Node, along with the node holding it and the
* key that precedes it in that node's list.
*
* @param Node
* Root of the subtree to search.
*
* @param OwnerNode
* Receives the node containing the greatest key, or NULL if the subtree holds no filenames.
*
* @param PreviousKey
* Receives the key before the greatest key in OwnerNode's list, or NULL if it's the first.
*
* @param LargestKey
* Receives the greatest key, or NULL if the subtree holds no filenames.
*
* @remarks
* An end marker's child holds everything sorting after the last real key, so the greatest key
* of a subtree is the last real key of the rightmost node reachable through end markers.
*/
static
NTSTATUS
FindLargestKeyInSubtree(PB_TREE Tree,
                        PB_TREE_FILENAME_NODE Node,
                        PB_TREE_FILENAME_NODE *OwnerNode,
                        PB_TREE_KEY *PreviousKey,
                        PB_TREE_KEY *LargestKey)
{
    PB_TREE_KEY CurrentKey = Node->FirstKey;
    PB_TREE_KEY Previous = NULL;
    PB_TREE_KEY BeforePrevious = NULL;
    NTSTATUS Status;

    *OwnerNode = NULL;
    *PreviousKey = NULL;
    *LargestKey = NULL;

    while (CurrentKey != NULL &&
           !BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_END))
    {
        BeforePrevious = Previous;
        Previous = CurrentKey;
        CurrentKey = CurrentKey->NextKey;
    }

    // Anything greater than the last real key lives beneath the end marker
    if (CurrentKey != NULL)
    {
        Status = NtfsLoadChildNode(Tree, CurrentKey);
        if (!NT_SUCCESS(Status))
            return Status;

        if (CurrentKey->LesserChild != NULL)
        {
            Status = FindLargestKeyInSubtree(Tree, CurrentKey->LesserChild, OwnerNode, PreviousKey, LargestKey);
            if (!NT_SUCCESS(Status) || *LargestKey != NULL)
                return Status;
        }
    }

    // Nothing but an end marker: this subtree holds no filenames
    if (Previous == NULL)
        return STATUS_SUCCESS;

    *OwnerNode = Node;
    *PreviousKey = BeforePrevious;
    *LargestKey = Previous;

    return STATUS_SUCCESS;
}

/**
* @name ReplaceKeyIndexEntry
* @implemented
*
* Replaces the index entry of Key with a copy of Source's, leaving Key's child pointer alone.
*
* @param Key
* Key whose index entry will be replaced. It keeps whatever child it had.
*
* @param Source
* Key to copy the entry from: its file reference and $FILE_NAME, or its view key and data.
*
* @return
* STATUS_SUCCESS on success, STATUS_INSUFFICIENT_RESOURCES if the allocation fails.
*
* @remarks
* Key always has a child here, so the replacement is built with room for the VCN and with
* NTFS_INDEX_ENTRY_NODE set, and keeps Key's VCN. UpdateIndexAllocation() only grows an entry
* to make room for a VCN, never the reverse.
*/
static
NTSTATUS
ReplaceKeyIndexEntry(PB_TREE_KEY Key, PB_TREE_KEY Source)
{
    PINDEX_ENTRY_ATTRIBUTE NewEntry;
    ULONG SourceSize = Source->IndexEntry->Length;
    ULONG EntrySize;

    // Source's own VCN, if it has a child, isn't part of what moves
    if (BooleanFlagOn(Source->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE))
        SourceSize -= sizeof(ULONGLONG);

    EntrySize = SourceSize + sizeof(ULONGLONG); // for the VCN of the child Key retains

    NewEntry = ExAllocatePoolWithTag(NonPagedPool, EntrySize, TAG_NTFS);
    if (!NewEntry)
    {
        DPRINT1("ERROR: Failed to allocate memory for replacement index entry!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // The header (file reference or view data location), key and data all come along
    RtlCopyMemory(NewEntry, Source->IndexEntry, SourceSize);
    NewEntry->Length = EntrySize;
    NewEntry->Flags = NTFS_INDEX_ENTRY_NODE;
    SetIndexEntryVCN(NewEntry, GetIndexEntryVCN(Key->IndexEntry));

    ExFreePoolWithTag(Key->IndexEntry, TAG_NTFS);
    Key->IndexEntry = NewEntry;

    return STATUS_SUCCESS;
}

/**
* @name RemoveKeyFromNode
* @implemented
*
* Removes a key that has already been located from the node that holds it.
*
* @param Node
* Node containing Key.
*
* @param PreviousKey
* The key before Key in Node's list, or NULL if Key is the first key.
*
* @param Key
* The key to remove.
*
* @return
* STATUS_SUCCESS on success, STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
*/
static
NTSTATUS
RemoveKeyFromNode(PB_TREE Tree,
                  PB_TREE_FILENAME_NODE Node,
                  PB_TREE_KEY PreviousKey,
                  PB_TREE_KEY Key)
{
    NTSTATUS Status;

    /* Either way this node's keys change. Only dirty nodes are written back,
     * so without this a removal from a sub-node never reaches the disk. */
    Node->DiskNeedsUpdating = TRUE;

    Status = NtfsLoadChildNode(Tree, Key);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Key->LesserChild != NULL)
    {
        PB_TREE_FILENAME_NODE OwnerNode;
        PB_TREE_KEY PreviousLargest;
        PB_TREE_KEY LargestKey;

        Status = FindLargestKeyInSubtree(Tree, Key->LesserChild, &OwnerNode, &PreviousLargest, &LargestKey);
        if (!NT_SUCCESS(Status))
            return Status;

        if (LargestKey != NULL)
        {
            // Unlinking an interior key would orphan its child, so promote the greatest key
            // beneath it and remove that one instead
            Status = ReplaceKeyIndexEntry(Key, LargestKey);
            if (!NT_SUCCESS(Status))
                return Status;

            return RemoveKeyFromNode(Tree, OwnerNode, PreviousLargest, LargestKey);
        }

        // The child holds no filenames, so there is nothing to promote; DestroyBTreeKey()
        // frees the empty child along with the key
    }

    if (PreviousKey == NULL)
        Node->FirstKey = Key->NextKey;
    else
        PreviousKey->NextKey = Key->NextKey;

    Node->KeyCount--;

    Key->NextKey = NULL;
    DestroyBTreeKey(Key);

    return STATUS_SUCCESS;
}

/**
* @name NtfsRemoveKey
* @implemented
*
* Removes the key matching a given filename from a B-Tree.
*
* @param Node
* Node to search. Callers start at the root node of the tree.
*
* @param SearchKey
* Key describing the filename to remove. Only its $FILE_NAME is used, for comparison.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @return
* STATUS_SUCCESS if the key was found and removed.
* STATUS_OBJECT_NAME_NOT_FOUND if no key in the tree matches.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
*
* @remarks
* A node left holding only its end marker is left in place. Readers descend into it and come
* back empty, whereas collapsing it would free an index buffer a parent VCN still points at.
*/
NTSTATUS
NtfsRemoveKey(PB_TREE Tree,
              PB_TREE_FILENAME_NODE Node,
              PB_TREE_KEY SearchKey,
              BOOLEAN CaseSensitive)
{
    PB_TREE_KEY PreviousKey = NULL;
    PB_TREE_KEY CurrentKey = Node->FirstKey;
    NTSTATUS Status;
    ULONG i;

    for (i = 0; i < Node->KeyCount; i++)
    {
        LONG Comparison;

        if (CurrentKey == NULL)
            break;

        if (BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_END))
            break;

        Comparison = CompareTreeKeys(SearchKey, CurrentKey, Tree->CollationRule, CaseSensitive);

        if (Comparison == 0)
            return RemoveKeyFromNode(Tree, Node, PreviousKey, CurrentKey);

        // Everything that sorts before CurrentKey is beneath it
        if (Comparison < 0)
            break;

        PreviousKey = CurrentKey;
        CurrentKey = CurrentKey->NextKey;
    }

    if (CurrentKey == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    Status = NtfsLoadChildNode(Tree, CurrentKey);
    if (!NT_SUCCESS(Status))
        return Status;

    if (CurrentKey->LesserChild != NULL)
        return NtfsRemoveKey(Tree, CurrentKey->LesserChild, SearchKey, CaseSensitive);

    return STATUS_OBJECT_NAME_NOT_FOUND;
}


/**
* @name NtfsFindKey
* @implemented
*
* Finds the key matching SearchKey in a B-Tree.
*
* @param Node
* Node to search. Callers start at the root node of the tree.
*
* @param SearchKey
* Key describing what to find. Only its key bytes are compared.
*
* @param FoundKey
* Receives the matching key, which stays owned by the tree.
*
* @return
* STATUS_SUCCESS if a key matches, STATUS_OBJECT_NAME_NOT_FOUND if none does, or the error
* reading a node returned.
*/
NTSTATUS
NtfsFindKey(PB_TREE Tree,
            PB_TREE_FILENAME_NODE Node,
            PB_TREE_KEY SearchKey,
            BOOLEAN CaseSensitive,
            PB_TREE_KEY *FoundKey)
{
    PB_TREE_KEY CurrentKey = Node->FirstKey;
    NTSTATUS Status;

    *FoundKey = NULL;

    // Everything that sorts before a key is beneath it, and the end marker covers the rest
    while (!BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_END))
    {
        LONG Comparison = CompareTreeKeys(SearchKey, CurrentKey, Tree->CollationRule, CaseSensitive);

        if (Comparison == 0)
        {
            *FoundKey = CurrentKey;
            return STATUS_SUCCESS;
        }

        if (Comparison < 0)
            break;

        CurrentKey = CurrentKey->NextKey;
    }

    Status = NtfsLoadChildNode(Tree, CurrentKey);
    if (!NT_SUCCESS(Status))
        return Status;

    if (CurrentKey->LesserChild == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    return NtfsFindKey(Tree, CurrentKey->LesserChild, SearchKey, CaseSensitive, FoundKey);
}

/**
* @name NtfsFindLastKey
* @implemented
*
* Finds the greatest key of a B-Tree, NULL if the tree holds none.
*/
NTSTATUS
NtfsFindLastKey(PB_TREE Tree,
                PB_TREE_KEY *LastKey)
{
    PB_TREE_FILENAME_NODE OwnerNode;
    PB_TREE_KEY PreviousKey;

    return FindLargestKeyInSubtree(Tree, Tree->RootNode, &OwnerNode, &PreviousKey, LastKey);
}

/**
* @name NtfsVisitKeys
* @implemented
*
* Calls Visitor, in order, for each key of a B-Tree from Low to High inclusive, reading only
* the nodes that can hold such keys.
*
* @param Low
* Key no visited key sorts before.
*
* @param High
* Key no visited key sorts after.
*
* @param Visitor
* Called with each key's index entry. Returning FALSE ends the walk.
*
* @param Stopped
* Set when the walk is over: Visitor returned FALSE, or a key past High was reached.
*/
NTSTATUS
NtfsVisitKeys(PB_TREE Tree,
              PB_TREE_FILENAME_NODE Node,
              PB_TREE_KEY Low,
              PB_TREE_KEY High,
              PNTFS_INDEX_VISITOR Visitor,
              PVOID Context,
              PBOOLEAN Stopped)
{
    PB_TREE_KEY CurrentKey;
    NTSTATUS Status;

    for (CurrentKey = Node->FirstKey; CurrentKey != NULL; CurrentKey = CurrentKey->NextKey)
    {
        BOOLEAN IsEnd = BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_END);
        LONG LowComparison = IsEnd ? -1 : CompareTreeKeys(Low, CurrentKey, Tree->CollationRule, FALSE);

        // The keys beneath a key sort before it, so they can be in range only if Low does
        if (LowComparison < 0)
        {
            Status = NtfsLoadChildNode(Tree, CurrentKey);
            if (!NT_SUCCESS(Status))
                return Status;

            if (CurrentKey->LesserChild != NULL)
            {
                Status = NtfsVisitKeys(Tree, CurrentKey->LesserChild, Low, High, Visitor, Context, Stopped);
                if (!NT_SUCCESS(Status) || *Stopped)
                    return Status;
            }
        }

        if (IsEnd)
            break;

        if (LowComparison <= 0)
        {
            if (CompareTreeKeys(High, CurrentKey, Tree->CollationRule, FALSE) < 0 ||
                !Visitor(CurrentKey->IndexEntry, Context))
            {
                *Stopped = TRUE;
                return STATUS_SUCCESS;
            }
        }
    }

    return STATUS_SUCCESS;
}

/**
* @name SplitBTreeNode
* @implemented
*
* Splits a B-Tree node that has grown too large. Finds the median key and sets up a right-hand-sibling
* node to contain the keys to the right of the median key.
*
* @param Tree
* Pointer to the B_TREE which contains the node being split
*
* @param Node
* Pointer to the B_TREE_FILENAME_NODE that needs to be split
*
* @param MedianKey
* Pointer a PB_TREE_KEY that will receive the pointer to the key in the middle of the node being split
*
* @param NewRightHandSibling
* Pointer to a PB_TREE_FILENAME_NODE that will receive a pointer to a newly-created B_TREE_FILENAME_NODE
* containing the keys to the right of MedianKey.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application created the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @return
* STATUS_SUCCESS on success.
* STATUS_INSUFFICIENT_RESOURCES if an allocation fails.
*
* @remarks
* It's the responsibility of the caller to insert the new median key into the parent node, as well as making the
* NewRightHandSibling the lesser child of the node that is currently Node's parent.
*/
NTSTATUS
SplitBTreeNode(PB_TREE Tree,
               PB_TREE_FILENAME_NODE Node,
               PB_TREE_KEY *MedianKey,
               PB_TREE_FILENAME_NODE *NewRightHandSibling,
               BOOLEAN CaseSensitive)
{
    ULONG MedianKeyIndex;
    PB_TREE_KEY LastKeyBeforeMedian, FirstKeyAfterMedian;
    ULONG KeyCount;
    ULONG HalfSize;
    ULONG SizeSum;
    ULONG i;

    DPRINT("SplitBTreeNode(%p, %p, %p, %p, %s) called\n",
            Tree,
            Node,
            MedianKey,
            NewRightHandSibling,
            CaseSensitive ? "TRUE" : "FALSE");

#ifndef NDEBUG
    DumpBTreeNode(Tree, Node, 0, 0);
#endif

    // Create the right hand sibling
    *NewRightHandSibling = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_FILENAME_NODE), TAG_NTFS);
    if (*NewRightHandSibling == NULL)
    {
        DPRINT1("Error: Failed to allocate memory for right hand sibling!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(*NewRightHandSibling, sizeof(B_TREE_FILENAME_NODE));
    (*NewRightHandSibling)->DiskNeedsUpdating = TRUE;


    // Find the last key before the median

    // This is roughly how NTFS-3G calculates median, and it's not congruent with what Windows does:
    /*
    // find the median key index
    MedianKeyIndex = (Node->KeyCount + 1) / 2;
    MedianKeyIndex--;

    LastKeyBeforeMedian = Node->FirstKey;
    for (i = 0; i < MedianKeyIndex - 1; i++)
        LastKeyBeforeMedian = LastKeyBeforeMedian->NextKey;*/

    // The method we'll use is a little bit closer to how Windows determines the median but it's not identical.
    // What Windows does is actually more complicated than this, I think because Windows allocates more slack space to Odd-numbered
    // Index Records, leaving less room for index entries in these records (I haven't discovered why this is done).
    // (Neither Windows nor chkdsk complain if we choose a different median than Windows would have chosen, as our median will be in the ballpark)

    // Use size to locate the median key / index
    LastKeyBeforeMedian = Node->FirstKey;
    MedianKeyIndex = 0;
    HalfSize = 2016; // half the allocated size after subtracting the first index entry offset (TODO: MATH)
    SizeSum = 0;
    for (i = 0; i < Node->KeyCount; i++)
    {
        SizeSum += LastKeyBeforeMedian->IndexEntry->Length;

        if (SizeSum > HalfSize)
            break;

        MedianKeyIndex++;
        LastKeyBeforeMedian = LastKeyBeforeMedian->NextKey;
    }

    // Now we can get the median key and the key that follows it
    *MedianKey = LastKeyBeforeMedian->NextKey;
    FirstKeyAfterMedian = (*MedianKey)->NextKey;

    DPRINT("%lu keys, %lu median\n", Node->KeyCount, MedianKeyIndex);
    DPRINT("\t\tMedian: %.*S\n", (*MedianKey)->IndexEntry->FileName.NameLength, (*MedianKey)->IndexEntry->FileName.Name);

    // "Node" will be the left hand sibling after the split, containing all keys prior to the median key

    // We need to create a dummy pointer at the end of the LHS. The dummy's child will be the median's child.
    LastKeyBeforeMedian->NextKey = CreateDummyKey(BooleanFlagOn((*MedianKey)->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE));
    if (LastKeyBeforeMedian->NextKey == NULL)
    {
        DPRINT1("Error: Couldn't allocate dummy key!\n");
        LastKeyBeforeMedian->NextKey = *MedianKey;
        ExFreePoolWithTag(*NewRightHandSibling, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Did the median key have a child node?
    if ((*MedianKey)->IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE)
    {
        // Set the child of the new dummy key
        LastKeyBeforeMedian->NextKey->LesserChild = (*MedianKey)->LesserChild;

        // Give the dummy key's index entry the same sub-node VCN the median
        SetIndexEntryVCN(LastKeyBeforeMedian->NextKey->IndexEntry, GetIndexEntryVCN((*MedianKey)->IndexEntry));
    }
    else
    {
        // Median key didn't have a child node, but it will. Create a new index entry large enough to store a VCN.
        PINDEX_ENTRY_ATTRIBUTE NewIndexEntry = ExAllocatePoolWithTag(NonPagedPool,
                                                                     (*MedianKey)->IndexEntry->Length + sizeof(ULONGLONG),
                                                                     TAG_NTFS);
        if (!NewIndexEntry)
        {
            DPRINT1("Unable to allocate memory for new index entry!\n");
            LastKeyBeforeMedian->NextKey = *MedianKey;
            ExFreePoolWithTag(*NewRightHandSibling, TAG_NTFS);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // Copy the old index entry to the new one
        RtlCopyMemory(NewIndexEntry, (*MedianKey)->IndexEntry, (*MedianKey)->IndexEntry->Length);

        // Use the new index entry after freeing the old one
        ExFreePoolWithTag((*MedianKey)->IndexEntry, TAG_NTFS);
        (*MedianKey)->IndexEntry = NewIndexEntry;

        // Update the length for the VCN
        (*MedianKey)->IndexEntry->Length += sizeof(ULONGLONG);

        // Set the node flag
        (*MedianKey)->IndexEntry->Flags |= NTFS_INDEX_ENTRY_NODE;
    }

    // "Node" will become the child of the median key
    (*MedianKey)->LesserChild = Node;
    SetIndexEntryVCN((*MedianKey)->IndexEntry, Node->VCN);

    // Update Node's KeyCount (remember to add 1 for the new dummy key)
    Node->KeyCount = MedianKeyIndex + 2;

    KeyCount = CountBTreeKeys(Node->FirstKey);
    ASSERT(Node->KeyCount == KeyCount);

    // everything to the right of MedianKey becomes the right hand sibling of Node
    (*NewRightHandSibling)->FirstKey = FirstKeyAfterMedian;
    (*NewRightHandSibling)->KeyCount = CountBTreeKeys(FirstKeyAfterMedian);

#ifndef NDEBUG
    DPRINT1("Left-hand node after split:\n");
    DumpBTreeNode(Tree, Node, 0, 0);

    DPRINT1("Right-hand sibling node after split:\n");
    DumpBTreeNode(Tree, *NewRightHandSibling, 0, 0);
#endif

    return STATUS_SUCCESS;
}
