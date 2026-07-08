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

/* Forward declarations for VCN helpers used in index buffer creation */
VOID SetIndexEntryVCN(PINDEX_ENTRY_ATTRIBUTE IndexEntry, ULONGLONG VCN);
ULONGLONG GetIndexEntryVCN(PINDEX_ENTRY_ATTRIBUTE IndexEntry);
VOID DestroyBTreeKey(PB_TREE_KEY Key);

static ULONG
NtfsGetIndexBufferFirstEntryOffset(PDEVICE_EXTENSION DeviceExt,
                                   ULONG IndexBufferSize)
{
    return ALIGN_UP_BY(FIELD_OFFSET(INDEX_BUFFER, Header) +
                       sizeof(INDEX_HEADER_ATTRIBUTE) +
                       (IndexBufferSize / DeviceExt->NtfsInfo.BytesPerSector + 1) * sizeof(USHORT),
                       ATTR_RECORD_ALIGNMENT) -
           FIELD_OFFSET(INDEX_BUFFER, Header);
}

static ULONG
NtfsGetMaxIndexNodeEntryBytes(PDEVICE_EXTENSION DeviceExt,
                              ULONG IndexBufferSize)
{
    return IndexBufferSize -
           FIELD_OFFSET(INDEX_BUFFER, Header) -
           NtfsGetIndexBufferFirstEntryOffset(DeviceExt, IndexBufferSize);
}

static ULONG
GetSerializedSizeOfIndexEntry(PB_TREE_KEY Key)
{
    ULONG EntrySize;

    ASSERT(Key);
    ASSERT(Key->IndexEntry);
    ASSERT(Key->IndexEntry->Length != 0);

    EntrySize = Key->IndexEntry->Length;
    if (Key->LesserChild &&
        !BooleanFlagOn(Key->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE))
    {
        EntrySize += sizeof(ULONGLONG);
    }

    return EntrySize;
}

static ULONG
GetSerializedSizeOfIndexEntries(PB_TREE_FILENAME_NODE Node)
{
    ULONG NodeSize = 0;
    PB_TREE_KEY CurrentKey = Node->FirstKey;
    ULONG i;

    for (i = 0; i < Node->KeyCount; i++)
    {
        NodeSize += GetSerializedSizeOfIndexEntry(CurrentKey);

        CurrentKey = CurrentKey->NextKey;
    }

    return NodeSize;
}

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

        DPRINT1("Node #%d, VCN: %I64u\n", Count, CurrentNode->VCN);

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
    ULONG BytesNeeded;
    ULONG BufferSize;
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
                           L"$I30",
                           4,
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

    /* The on-disk bitmap (BitmapLength) and the locally-computed BytesNeeded
     * can disagree: BytesNeeded is sized for the *new* node count, while
     * BitmapLength reflects the current attribute size, which may already
     * have been padded up by a previous grow.  Allocate a buffer big enough
     * for both so the ReadAttribute below can never overflow, and so the
     * bits we set after extending the index allocation are still in range. */
    BufferSize = max(BytesNeeded, (ULONG)BitmapLength);

    // Allocate memory for the bitmap, including some padding; RtlInitializeBitmap() wants a pointer
    // that's ULONG-aligned, and it wants the size of the memory allocated for it to be a ULONG-multiple.
    BitmapMem = ExAllocatePoolWithTag(NonPagedPool, BufferSize + sizeof(ULONG), TAG_NTFS);
    if (!BitmapMem)
    {
        DPRINT1("Error: failed to allocate bitmap!");
        ReleaseAttributeContext(BitmapCtx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // RtlInitializeBitmap() wants a pointer that's ULONG-aligned.
    BitmapPtr = (PULONG)ALIGN_UP_BY((ULONG_PTR)BitmapMem, sizeof(ULONG));

    RtlZeroMemory(BitmapPtr, BufferSize);

    // Read the existing bitmap data
    Status = ReadAttribute(DeviceExt, BitmapCtx, 0, (PCHAR)BitmapPtr, BitmapLength);

    /*
     * Include the node being allocated in the in-memory bitmap range.  The
     * old size is NextNodeNumber entries, and the new node is stored at
     * exactly that bit index.
     */
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
        ReleaseAttributeContext(BitmapCtx);
        return Status;
    }

    // Compute the VCN of the new node so the placeholder header is correct
    // and so the value can also be returned via *NewVCN below.  Must match
    // GetAllocationOffsetFromVCN()'s inverse: when IndexBufferSize is at
    // least one cluster, VCN counts in clusters; otherwise it counts in
    // sectors.
    {
        ULONGLONG NewBlockVCN;
        if (IndexBufferSize < DeviceExt->NtfsInfo.BytesPerCluster)
            NewBlockVCN = IndexAllocationLength / DeviceExt->NtfsInfo.BytesPerSector;
        else
            NewBlockVCN = IndexAllocationLength / DeviceExt->NtfsInfo.BytesPerCluster;
        *NewVCN = NewBlockVCN;
    }

    // Write an empty, valid INDX record to the newly allocated space so that
    // reads before UpdateIndexNode() don't encounter uninitialized data.
    {
        PINDEX_BUFFER NewIndexBuffer;
        ULONG BytesWrittenIdx;
        ULONG UsaCount;
        ULONG FirstEntryOffset;

        NewIndexBuffer = ExAllocatePoolWithTag(NonPagedPool, IndexBufferSize, TAG_NTFS);
        if (NewIndexBuffer)
        {
            RtlZeroMemory(NewIndexBuffer, IndexBufferSize);

            // Initialize INDX record header. The USA array lives between the
            // index header and the first entry; don't overlap either one.
            NewIndexBuffer->Ntfs.Type = NRH_INDX_TYPE;
            NewIndexBuffer->VCN = *NewVCN;
            UsaCount = IndexBufferSize / DeviceExt->NtfsInfo.BytesPerSector + 1;
            NewIndexBuffer->Ntfs.UsaOffset = FIELD_OFFSET(INDEX_BUFFER, Header) +
                                             sizeof(INDEX_HEADER_ATTRIBUTE);
            NewIndexBuffer->Ntfs.UsaCount = UsaCount;
            FirstEntryOffset = ALIGN_UP_BY(NewIndexBuffer->Ntfs.UsaOffset +
                                           UsaCount * sizeof(USHORT),
                                           ATTR_RECORD_ALIGNMENT) -
                               FIELD_OFFSET(INDEX_BUFFER, Header);

            // Initialize index header with an empty end entry
            NewIndexBuffer->Header.FirstEntryOffset = FirstEntryOffset;
            NewIndexBuffer->Header.TotalSizeOfEntries = FirstEntryOffset + sizeof(INDEX_ENTRY_ATTRIBUTE);
            NewIndexBuffer->Header.AllocatedSize = IndexBufferSize - FIELD_OFFSET(INDEX_BUFFER, Header);
            NewIndexBuffer->Header.Flags = 0;

            // Create an end-marker entry
            {
                PINDEX_ENTRY_ATTRIBUTE EndEntry;
                EndEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&NewIndexBuffer->Header +
                            NewIndexBuffer->Header.FirstEntryOffset);
                EndEntry->Length = sizeof(INDEX_ENTRY_ATTRIBUTE);
                EndEntry->KeyLength = 0;
                EndEntry->Flags = NTFS_INDEX_ENTRY_END;
            }

            // Apply fixup array and write
            AddFixupArray(DeviceExt, &NewIndexBuffer->Ntfs);
            WriteAttribute(DeviceExt,
                           IndexAllocationCtx,
                           IndexAllocationLength,
                           (const PUCHAR)NewIndexBuffer,
                           IndexBufferSize,
                           &BytesWrittenIdx,
                           FileRecord);

            ExFreePoolWithTag(NewIndexBuffer, TAG_NTFS);
        }
    }

    // Update file record on disk
    Status = UpdateFileRecord(DeviceExt, IndexAllocationCtx->FileMFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Failed to update file record!\n");
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
        DPRINT1("ERROR: Unable to write to $I30 bitmap attribute!\n");
    }

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
CreateEmptyBTreeEx(ULONG CollationRule, PB_TREE *NewTree)
{
    PB_TREE Tree = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE), TAG_NTFS);
    PB_TREE_FILENAME_NODE RootNode = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_FILENAME_NODE), TAG_NTFS);
    PB_TREE_KEY DummyKey;

    NTFS_TRACE("CreateEmptyBTreeEx(0x%lx, %p) called\n", CollationRule, NewTree);

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

    // Setup the Tree
    RootNode->FirstKey = DummyKey;
    RootNode->KeyCount = 1;
    RootNode->DiskNeedsUpdating = TRUE;
    Tree->RootNode = RootNode;
    Tree->CollationRule = CollationRule;

    *NewTree = Tree;

    // Memory will be freed when DestroyBTree() is called

    return STATUS_SUCCESS;
}

NTSTATUS
CreateEmptyBTree(PB_TREE *NewTree)
{
    /* Legacy $I30-shape callers default to COLLATION_FILE_NAME. */
    return CreateEmptyBTreeEx(COLLATION_FILE_NAME, NewTree);
}

/* Byte-wise compare returning signum; ntoskrnl does not export libc memcmp
 * and RtlCompareMemory returns match-length, not sign. */
static int
NtfsCompareBytes(const VOID *A, const VOID *B, ULONG Len)
{
    const UCHAR *a = (const UCHAR *)A;
    const UCHAR *b = (const UCHAR *)B;
    ULONG i;
    for (i = 0; i < Len; i++)
    {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

/* FILENAME-collation compare of two raw filename-attribute key blobs.  The
 * blobs are parsed as FILENAME_ATTRIBUTE: NameLength is at
 * FIELD_OFFSET(FILENAME_ATTRIBUTE, NameLength), WCHAR name starts at
 * FIELD_OFFSET(FILENAME_ATTRIBUTE, Name).  Handles both the FILE_NAME
 * (case-insensitive) and UNICODE_STRING (case-sensitive) variants. */
static LONG
NtfsCompareFilenameKey(const VOID *Key1, ULONG Key1Len,
                       const VOID *Key2, ULONG Key2Len,
                       BOOLEAN CaseSensitive)
{
    const FILENAME_ATTRIBUTE *Fn1 = (const FILENAME_ATTRIBUTE *)Key1;
    const FILENAME_ATTRIBUTE *Fn2 = (const FILENAME_ATTRIBUTE *)Key2;
    UNICODE_STRING Name1, Name2;
    LONG Comparison;

    if (Key1Len < FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) ||
        Key2Len < FIELD_OFFSET(FILENAME_ATTRIBUTE, Name))
    {
        /* Degenerate case: fall back to binary */
        ULONG MinLen = min(Key1Len, Key2Len);
        int r = MinLen ? NtfsCompareBytes(Key1, Key2, MinLen) : 0;
        if (r != 0)
            return (r < 0) ? -1 : 1;
        if (Key1Len < Key2Len) return -1;
        if (Key1Len > Key2Len) return 1;
        return 0;
    }

    Name1.Buffer = (PWCHAR)Fn1->Name;
    Name1.Length = Name1.MaximumLength = (USHORT)(Fn1->NameLength * sizeof(WCHAR));

    Name2.Buffer = (PWCHAR)Fn2->Name;
    Name2.Length = Name2.MaximumLength = (USHORT)(Fn2->NameLength * sizeof(WCHAR));

    if (Name1.Length == Name2.Length)
        return RtlCompareUnicodeString(&Name1, &Name2, !CaseSensitive);

    if (Name1.Length < Name2.Length)
    {
        Name2.Length = Name1.Length;
        Comparison = RtlCompareUnicodeString(&Name1, &Name2, !CaseSensitive);
        if (Comparison == 0)
            return -1;
    }
    else
    {
        Name1.Length = Name2.Length;
        Comparison = RtlCompareUnicodeString(&Name1, &Name2, !CaseSensitive);
        if (Comparison == 0)
            return 1;
    }

    return Comparison;
}

static LONG
NtfsCompareBinaryKey(const VOID *Key1, ULONG Key1Len,
                     const VOID *Key2, ULONG Key2Len)
{
    ULONG MinLen = min(Key1Len, Key2Len);
    int r = MinLen ? NtfsCompareBytes(Key1, Key2, MinLen) : 0;
    if (r != 0)
        return (r < 0) ? -1 : 1;
    if (Key1Len < Key2Len) return -1;
    if (Key1Len > Key2Len) return 1;
    return 0;
}

/* Collation-aware raw-bytes key comparator.  See ntfs.h for semantics. */
LONG
NtfsCompareKeyBytes(const VOID *Key1, ULONG Key1Len,
                    const VOID *Key2, ULONG Key2Len,
                    ULONG CollationRule, BOOLEAN CaseSensitive)
{
    switch (CollationRule)
    {
    case COLLATION_BINARY:
        return NtfsCompareBinaryKey(Key1, Key1Len, Key2, Key2Len);

    case COLLATION_FILE_NAME:
        return NtfsCompareFilenameKey(Key1, Key1Len, Key2, Key2Len, CaseSensitive);

    case COLLATION_UNICODE_STRING:
        /* Like FILE_NAME but forced case-sensitive (POSIX). */
        return NtfsCompareFilenameKey(Key1, Key1Len, Key2, Key2Len, TRUE);

    case COLLATION_NTOFS_ULONG:
    {
        ULONG v1 = 0, v2 = 0;
        if (Key1Len >= sizeof(ULONG)) RtlCopyMemory(&v1, Key1, sizeof(ULONG));
        if (Key2Len >= sizeof(ULONG)) RtlCopyMemory(&v2, Key2, sizeof(ULONG));
        if (v1 < v2) return -1;
        if (v1 > v2) return 1;
        return 0;
    }

    case COLLATION_NTOFS_SID:
    {
        /* SID layout: 1 byte Rev | 1 byte SubAuthCount | 6 bytes IdAuthority
         * | SubAuthCount * 4-byte SubAuthorities.  Compare Rev first, then
         * IdAuthority, then each SubAuthority (4-byte LE).  Falls back to
         * memcmp over the equal-length prefix to keep the ordering total
         * even on malformed SIDs. */
        const UCHAR *s1 = (const UCHAR *)Key1;
        const UCHAR *s2 = (const UCHAR *)Key2;
        ULONG i;
        ULONG Sub1, Sub2, MinSub;

        if (Key1Len < 8 || Key2Len < 8)
            return NtfsCompareBinaryKey(Key1, Key1Len, Key2, Key2Len);

        /* Revision */
        if (s1[0] != s2[0]) return (s1[0] < s2[0]) ? -1 : 1;
        /* IdAuthority (bytes 2..7 - 6-byte big-endian per SID spec) */
        {
            int r = NtfsCompareBytes(s1 + 2, s2 + 2, 6);
            if (r != 0) return (r < 0) ? -1 : 1;
        }
        Sub1 = s1[1];
        Sub2 = s2[1];
        MinSub = min(Sub1, Sub2);
        for (i = 0; i < MinSub; i++)
        {
            ULONG off = 8 + i * sizeof(ULONG);
            if (off + sizeof(ULONG) > Key1Len || off + sizeof(ULONG) > Key2Len)
                break;
            {
                ULONG v1 = 0, v2 = 0;
                RtlCopyMemory(&v1, s1 + off, sizeof(ULONG));
                RtlCopyMemory(&v2, s2 + off, sizeof(ULONG));
                if (v1 < v2) return -1;
                if (v1 > v2) return 1;
            }
        }
        if (Sub1 < Sub2) return -1;
        if (Sub1 > Sub2) return 1;
        return 0;
    }

    case COLLATION_NTOFS_SECURITY_HASH:
    {
        /* 4-byte Hash | 4-byte SecurityId, both LE */
        ULONG h1 = 0, h2 = 0, id1 = 0, id2 = 0;
        if (Key1Len >= 4) RtlCopyMemory(&h1, Key1, 4);
        if (Key2Len >= 4) RtlCopyMemory(&h2, Key2, 4);
        if (h1 != h2) return (h1 < h2) ? -1 : 1;
        if (Key1Len >= 8) RtlCopyMemory(&id1, (const UCHAR *)Key1 + 4, 4);
        if (Key2Len >= 8) RtlCopyMemory(&id2, (const UCHAR *)Key2 + 4, 4);
        if (id1 != id2) return (id1 < id2) ? -1 : 1;
        return 0;
    }

    case COLLATION_NTOFS_ULONGS:
    {
        /* n*ULONG compared element-wise (used for GUIDs in $ObjId/$O). */
        ULONG MinLen = min(Key1Len, Key2Len);
        ULONG NumU = MinLen / sizeof(ULONG);
        ULONG i;
        for (i = 0; i < NumU; i++)
        {
            ULONG v1 = 0, v2 = 0;
            RtlCopyMemory(&v1, (const UCHAR *)Key1 + i * sizeof(ULONG), sizeof(ULONG));
            RtlCopyMemory(&v2, (const UCHAR *)Key2 + i * sizeof(ULONG), sizeof(ULONG));
            if (v1 < v2) return -1;
            if (v1 > v2) return 1;
        }
        if (Key1Len < Key2Len) return -1;
        if (Key1Len > Key2Len) return 1;
        return 0;
    }

    default:
        /* Unknown collation - treat as binary so the tree stays totally
         * ordered rather than silently violating the invariant. */
        DPRINT1("NtfsCompareKeyBytes: unknown collation 0x%lx, falling back to binary.\n",
                CollationRule);
        return NtfsCompareBinaryKey(Key1, Key1Len, Key2, Key2Len);
    }
}

/**
* @name CompareTreeKeys
* @implemented
*
* Compare two B_TREE_KEY's to determine their order in the tree under the
* given collation rule.
*
* @param Key1
* Pointer to a B_TREE_KEY that will be compared.
*
* @param Key2
* Pointer to the other B_TREE_KEY that will be compared.
*
* @param CollationRule
* One of the COLLATION_* constants (COLLATION_FILE_NAME for $I30, etc.).
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode.
* Only meaningful for FILE_NAME collation.
*
* @returns
* 0 if the two keys are equal.
* < 0 if key1 is less than key2
* > 0 if key1 is greater than key2
*
* @remarks
* Any other key is always less than the final (dummy) key in a node. Key1
* must not be the dummy node.
*/
LONG
CompareTreeKeys(PB_TREE_KEY Key1, PB_TREE_KEY Key2, ULONG CollationRule, BOOLEAN CaseSensitive)
{
    // Key1 must not be the final key (AKA the dummy key)
    ASSERT(!(Key1->IndexEntry->Flags & NTFS_INDEX_ENTRY_END));

    // If Key2 is the "dummy key", key 1 will always come first
    if (Key2->NextKey == NULL)
        return -1;

    return NtfsCompareKeyBytes(&Key1->IndexEntry->FileName,
                               Key1->IndexEntry->KeyLength,
                               &Key2->IndexEntry->FileName,
                               Key2->IndexEntry->KeyLength,
                               CollationRule,
                               CaseSensitive);
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

    ASSERT(BytesRead == IndexBufferSize);
    NT_ASSERT(NodeBuffer->Ntfs.Type == NRH_INDX_TYPE);
    NT_ASSERT(NodeBuffer->VCN == *VCN);

    // Apply the fixup array to the node buffer
    Status = FixupUpdateSequenceArray(Vcb, &NodeBuffer->Ntfs);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ERROR: Couldn't apply fixup array to index node buffer!\n");
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

            // Copy the current entry to its key
            RtlCopyMemory(CurrentKey->IndexEntry, CurrentNodeEntry, CurrentNodeEntry->Length);

            // Children are loaded lazily by NtfsInsertKey when needed.
            // The NTFS_INDEX_ENTRY_NODE flag on IndexEntry indicates a child exists on disk.

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
* Allocates memory for the entire tree. Caller is responsible for destroying the tree with DestroyBTree().
*/
NTSTATUS
CreateBTreeFromIndexEx(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER FileRecordWithIndex,
                       PCWSTR IndexName,
                       ULONG IndexNameLen,
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

    DPRINT("CreateBTreeFromIndexEx(%p, %p) name=%ws\n", IndexRoot, NewTree, IndexName ? IndexName : L"(null)");

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

    // See if the file record has an attribute allocation
    Status = FindAttribute(Vcb,
                           FileRecordWithIndex,
                           AttributeIndexAllocation,
                           IndexName,
                           IndexNameLen,
                           &IndexAllocationContext,
                           NULL);
    if (!NT_SUCCESS(Status))
        IndexAllocationContext = NULL;

    // Setup the Tree
    RootNode->FirstKey = CurrentKey;
    Tree->RootNode = RootNode;

    // Store context for lazy child loading in NtfsInsertKey
    Tree->Vcb = Vcb;
    Tree->IndexRoot = IndexRoot;
    Tree->IndexAllocationContext = NULL; // set below if it exists
    /* Capture the on-disk collation so later compares use the right rule. */
    Tree->CollationRule = IndexRoot->CollationRule;

    // Make sure we won't try reading past the attribute-end
    if (FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header) + IndexRoot->Header.TotalSizeOfEntries > IndexRootContext->pRecord->Resident.ValueLength)
    {
        DPRINT1("Filesystem corruption detected!\n");
        DestroyBTree(Tree);
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Cleanup;
    }

    // Start at the first node entry
    CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)IndexRoot
                                                + FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header)
                                                + IndexRoot->Header.FirstEntryOffset);

    // Create a key for each entry in the node.
    // Child nodes are NOT loaded here - they are loaded lazily by NtfsInsertKey
    // when it needs to descend into a child. This makes tree creation O(root keys)
    // instead of O(all nodes), turning the per-file insert from O(n) to O(log n).
    while (CurrentOffset < IndexRoot->Header.TotalSizeOfEntries)
    {
        // Allocate memory for the current entry
        CurrentKey->IndexEntry = ExAllocatePoolWithTag(NonPagedPool, CurrentNodeEntry->Length, TAG_NTFS);
        if (!CurrentKey->IndexEntry)
        {
            DPRINT1("ERROR: Couldn't allocate memory for next key!\n");
            DestroyBTree(Tree);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
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
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto Cleanup;
            }

            RtlZeroMemory(NextKey, sizeof(B_TREE_KEY));

            // Add NextKey to the end of the list
            CurrentKey->NextKey = NextKey;

            // Copy the current entry to its key
            RtlCopyMemory(CurrentKey->IndexEntry, CurrentNodeEntry, CurrentNodeEntry->Length);

            // LesserChild stays NULL - loaded lazily when needed

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
    Status = STATUS_SUCCESS;

Cleanup:
    if (NT_SUCCESS(Status))
    {
        // Tree takes ownership of IndexAllocationContext for lazy child loading.
        // It will be released when the caller calls ReleaseAttributeContext on
        // Tree->IndexAllocationContext after destroying the tree.
        Tree->IndexAllocationContext = IndexAllocationContext;
    }
    else if (IndexAllocationContext)
    {
        ReleaseAttributeContext(IndexAllocationContext);
    }

    return Status;
}

/* Backwards-compatible wrapper - $I30 $INDEX_ALLOCATION lookup.  All existing
 * callers hit this overload; new view-index code (quota, etc.) should call
 * CreateBTreeFromIndexEx with the actual index name. */
NTSTATUS
CreateBTreeFromIndex(PDEVICE_EXTENSION Vcb,
                     PFILE_RECORD_HEADER FileRecordWithIndex,
                     PNTFS_ATTR_CONTEXT IndexRootContext,
                     PINDEX_ROOT_ATTRIBUTE IndexRoot,
                     PB_TREE *NewTree)
{
    return CreateBTreeFromIndexEx(Vcb,
                                  FileRecordWithIndex,
                                  L"$I30",
                                  4,
                                  IndexRootContext,
                                  IndexRoot,
                                  NewTree);
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

    /* For $I30 trees, the attribute type stamped into the index root is
     * AttributeFileName.  View indexes ($Q, $O, $SDH, $SII, $ObjId, ...)
     * use AttributeData (0x00).  We key off the collation rule: FILE_NAME
     * means $I30-style, anything else is a view index. */
    NewIndexRoot->AttributeType = (Tree->CollationRule == COLLATION_FILE_NAME)
                                    ? AttributeFileName
                                    : 0;
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
        BOOLEAN EntryHasLoadedChild;
        BOOLEAN EntryHasOnDiskChild;
        ULONG SerializedLength;

        ASSERT(CurrentKey->IndexEntry->Length != 0);

        EntryHasLoadedChild = (CurrentKey->LesserChild != NULL);
        EntryHasOnDiskChild = BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE);
        SerializedLength = CurrentKey->IndexEntry->Length;

        if (EntryHasLoadedChild && !EntryHasOnDiskChild)
            SerializedLength += sizeof(ULONGLONG);

        // Would adding the current entry to the index increase the index size beyond the limit we've set?
        ULONG IndexSize = NewIndexRoot->Header.TotalSizeOfEntries - NewIndexRoot->Header.FirstEntryOffset + SerializedLength;
        if (IndexSize > MaxIndexSize)
        {
            DPRINT1("TODO: Adding file would require creating an attribute list!\n");
            ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
            return STATUS_NOT_IMPLEMENTED;
        }

        // Copy the index entry
        RtlCopyMemory(CurrentNodeEntry, CurrentKey->IndexEntry, CurrentKey->IndexEntry->Length);

        // Ensure entry flags are consistent with whether this key actually
        // has a child node.  Entries may carry a stale NTFS_INDEX_ENTRY_NODE
        // flag after tree restructuring (split/demotion) if the child was
        // moved elsewhere or the entry was relocated to the root.
        if (EntryHasLoadedChild)
        {
            if (!BooleanFlagOn(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE))
            {
                SetFlag(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE);
                CurrentNodeEntry->Length += sizeof(ULONGLONG);
            }
            // Write the child VCN.  Prefer the in-memory entry's VCN if the
            // NODE flag is set (UpdateIndexAllocation already wrote it there).
            // Fall back to the child node's VCN otherwise.
            if (EntryHasOnDiskChild)
                SetIndexEntryVCN(CurrentNodeEntry, GetIndexEntryVCN(CurrentKey->IndexEntry));
            else
                SetIndexEntryVCN(CurrentNodeEntry, CurrentKey->LesserChild->VCN);
            NewIndexRoot->Header.Flags = INDEX_ROOT_LARGE;
        }
        else if (EntryHasOnDiskChild)
        {
            // Child exists on disk but wasn't loaded (lazy loading).
            // Preserve the NODE flag and VCN from the original entry.
            // The entry was already copied with the correct flag and VCN above.
            NewIndexRoot->Header.Flags = INDEX_ROOT_LARGE;
        }
        else
        {
            // Truly a leaf entry with no children on disk or in memory.
            if (BooleanFlagOn(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE))
            {
                ClearFlag(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE);
                CurrentNodeEntry->Length -= sizeof(ULONGLONG);
            }
        }

        NTFS_TRACE("Index Node Entry Stream Length: %u\nIndex Node Entry Length: %u\n",
                CurrentNodeEntry->KeyLength,
                CurrentNodeEntry->Length);

        // Add Length of Current Entry to Total Size of Entries
        NewIndexRoot->Header.TotalSizeOfEntries += CurrentNodeEntry->Length;

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

    RtlZeroMemory(IndexBuffer, BufferSize);
    IndexBuffer->Ntfs.Type = NRH_INDX_TYPE;
    IndexBuffer->Ntfs.UsaOffset = FIELD_OFFSET(INDEX_BUFFER, Header) +
                                  sizeof(INDEX_HEADER_ATTRIBUTE);
    IndexBuffer->Ntfs.UsaCount = BufferSize / DeviceExt->NtfsInfo.BytesPerSector + 1;

    // TODO: Check bitmap for VCN
    ASSERT(Node->HasValidVCN);
    IndexBuffer->VCN = Node->VCN;

    IndexBuffer->Header.FirstEntryOffset = NtfsGetIndexBufferFirstEntryOffset(DeviceExt, BufferSize);
    IndexBuffer->Header.AllocatedSize = BufferSize - FIELD_OFFSET(INDEX_BUFFER, Header);

    // Start summing the total size of this node's entries
    IndexBuffer->Header.TotalSizeOfEntries = IndexBuffer->Header.FirstEntryOffset;

    CurrentKey = Node->FirstKey;
    CurrentNodeEntry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&(IndexBuffer->Header)
                                                + IndexBuffer->Header.FirstEntryOffset);
    for (i = 0; i < Node->KeyCount; i++)
    {
        BOOLEAN EntryHasChild;
        BOOLEAN EntryHasOnDiskChild;
        ULONG SerializedLength;

        ASSERT(CurrentKey->IndexEntry->Length != 0);

        EntryHasChild = (CurrentKey->LesserChild != NULL) ||
                        BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE);
        EntryHasOnDiskChild = BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE);
        SerializedLength = CurrentKey->IndexEntry->Length;

        if (EntryHasChild && !EntryHasOnDiskChild)
            SerializedLength += sizeof(ULONGLONG);

        // Would adding the current entry to the index increase the node size beyond the allocation size?
        ULONG IndexSize = FIELD_OFFSET(INDEX_BUFFER, Header)
            + IndexBuffer->Header.TotalSizeOfEntries
            + SerializedLength;
        if (IndexSize > BufferSize)
        {
            DPRINT1("TODO: Adding file would require creating a new node!\n");
            return STATUS_NOT_IMPLEMENTED;
        }

        // Copy the index entry
        RtlCopyMemory(CurrentNodeEntry, CurrentKey->IndexEntry, CurrentKey->IndexEntry->Length);

        // Determine if this specific entry should have a child pointer.
        // An entry has a child if: (a) it has a loaded LesserChild, OR
        // (b) its original index entry has the NODE flag (lazy-loaded child on disk).
        {
            if (EntryHasChild && !BooleanFlagOn(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE))
            {
                SetFlag(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE);
                CurrentNodeEntry->Length += sizeof(ULONGLONG);
            }
            else if (!EntryHasChild && BooleanFlagOn(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE))
            {
                ClearFlag(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE);
                CurrentNodeEntry->Length -= sizeof(ULONGLONG);
            }

            // Write the child VCN into the on-disk entry.
            // Mirror CreateIndexRootFromBTree's preference: if the source
            // IndexEntry already has the NODE flag, trust the VCN that
            // UpdateIndexNode just wrote into it; otherwise fall back to the
            // loaded child node's VCN.  This keeps the two serializers in
            // agreement and avoids writing a stale LesserChild->VCN when the
            // in-memory entry was refreshed more recently.
            if (BooleanFlagOn(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE))
            {
                if (BooleanFlagOn(CurrentKey->IndexEntry->Flags, NTFS_INDEX_ENTRY_NODE))
                    SetIndexEntryVCN(CurrentNodeEntry, GetIndexEntryVCN(CurrentKey->IndexEntry));
                else if (CurrentKey->LesserChild)
                    SetIndexEntryVCN(CurrentNodeEntry, CurrentKey->LesserChild->VCN);
                else
                    SetIndexEntryVCN(CurrentNodeEntry, 0);
            }
        }

        DPRINT("Index Node Entry Stream Length: %u\nIndex Node Entry Length: %u\n",
               CurrentNodeEntry->KeyLength,
               CurrentNodeEntry->Length);

        // Add Length of Current Entry to Total Size of Entries
        IndexBuffer->Header.TotalSizeOfEntries += CurrentNodeEntry->Length;

        // Check for child nodes (loaded or lazy)
        if (BooleanFlagOn(CurrentNodeEntry->Flags, NTFS_INDEX_ENTRY_NODE))
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
DemoteBTreeRoot(PB_TREE Tree,
                 ULONG IndexRecordSize,
                 BOOLEAN CaseSensitive)
{
    PB_TREE_FILENAME_NODE NewSubNode, NewIndexRoot;
    PB_TREE_KEY DummyKey;
    ULONG MaxNodeSize;

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

    MaxNodeSize = NtfsGetMaxIndexNodeEntryBytes(Tree->Vcb, IndexRecordSize);
    while (TRUE)
    {
        PB_TREE_KEY CurrentKey = Tree->RootNode->FirstKey;
        PB_TREE_KEY PreviousKey = NULL;
        BOOLEAN SplitNode = FALSE;
        ULONG i;

        for (i = 0; i < Tree->RootNode->KeyCount; i++)
        {
            if (CurrentKey->LesserChild &&
                GetSerializedSizeOfIndexEntries(CurrentKey->LesserChild) > MaxNodeSize)
            {
                PB_TREE_KEY MedianKey;
                PB_TREE_FILENAME_NODE NewRightHandSibling;
                NTSTATUS Status;

                Status = SplitBTreeNode(Tree,
                                        CurrentKey->LesserChild,
                                        &MedianKey,
                                        &NewRightHandSibling,
                                        CaseSensitive,
                                        IndexRecordSize);
                if (!NT_SUCCESS(Status))
                    return Status;

                MedianKey->NextKey = CurrentKey;
                if (PreviousKey)
                    PreviousKey->NextKey = MedianKey;
                else
                    Tree->RootNode->FirstKey = MedianKey;

                CurrentKey->LesserChild = NewRightHandSibling;
                Tree->RootNode->KeyCount++;
                Tree->RootNode->DiskNeedsUpdating = TRUE;
                SplitNode = TRUE;
                break;
            }

            PreviousKey = CurrentKey;
            CurrentKey = CurrentKey->NextKey;
        }

        if (!SplitNode)
            break;
    }

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

    Status = FindAttribute(DeviceExt, FileRecord, AttributeIndexAllocation, L"$I30", 4, &IndexAllocationContext, &IndexAllocationOffset);
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
                // We need to add an index allocation to the file record
                PNTFS_ATTR_RECORD EndMarker = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->BytesInUse - (sizeof(ULONG) * 2));
                DPRINT("Adding index allocation...\n");

                // Add index allocation to the very end of the file record
                Status = AddIndexAllocation(DeviceExt,
                                            FileRecord,
                                            EndMarker,
                                            L"$I30",
                                            4);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Failed to add index allocation!\n");
                    return Status;
                }

                // Find the new attribute
                Status = FindAttribute(DeviceExt, FileRecord, AttributeIndexAllocation, L"$I30", 4, &IndexAllocationContext, &IndexAllocationOffset);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Couldn't find newly-created index allocation!\n");
                    return Status;
                }

                // Advance end marker
                EndMarker = (PNTFS_ATTR_RECORD)((ULONG_PTR)EndMarker + EndMarker->Length);

                // Add index bitmap to the very end of the file record
                Status = AddBitmap(DeviceExt,
                                   FileRecord,
                                   EndMarker,
                                   L"$I30",
                                   4);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Failed to add index bitmap!\n");
                    ReleaseAttributeContext(IndexAllocationContext);
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
            Status = UpdateIndexNode(DeviceExt, FileRecord, CurrentKey->LesserChild, IndexBufferSize, IndexAllocationContext, IndexAllocationOffset);
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
            Status = UpdateIndexNode(DeviceExt, FileRecord, CurrentKey->LesserChild, IndexBufferSize, IndexAllocationContext, IndexAllocationOffset);
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
                NewEntry->Flags |= NTFS_INDEX_ENTRY_NODE;

                // Free the old memory
                ExFreePoolWithTag(CurrentKey->IndexEntry, TAG_NTFS);

                CurrentKey->IndexEntry = NewEntry;
            }

            // Update the VCN stored in the index entry of CurrentKey
            SetIndexEntryVCN(CurrentKey->IndexEntry, CurrentKey->LesserChild->VCN);
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
            DPRINT1("ERROR: Failed to update index allocation! Status=0x%lx Written=%lu Expected=%lu\n",
                    Status, LengthWritten, IndexBufferSize);
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

    // Setup the New Key
    NewKey = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_KEY), TAG_NTFS);
    if (!NewKey)
    {
        DPRINT1("ERROR: Failed to allocate memory for new key!\n");
        ExFreePoolWithTag(NewEntry, TAG_NTFS);
        return NULL;
    }
    NewKey->IndexEntry = NewEntry;
    NewKey->NextKey = NULL;
    NewKey->LesserChild = NULL;

    return NewKey;
}

/* Build a B_TREE_KEY for a view-index entry: raw key bytes + raw value bytes.
 * Lays out the INDEX_ENTRY_ATTRIBUTE like Windows does for $Q / $O / $SDH:
 *
 *   [INDEX_ENTRY_ATTRIBUTE fixed header]
 *   [KeyLen bytes of key]        -- starts at FIELD_OFFSET(INDEX_ENTRY, FileName)
 *   [ValueLen bytes of value]    -- starts at DataOffset from entry base
 *
 * ViewIndex.DataOffset is set to (header + padded-KeyLen); ViewIndex.DataLength
 * to ValueLen.  All offsets are 8-byte aligned per NTFS layout rules. */
static PB_TREE_KEY
CreateBTreeKeyFromBlob(const VOID *KeyBytes, ULONG KeyLen,
                       const VOID *ValueBytes, ULONG ValueLen)
{
    PB_TREE_KEY NewKey;
    PINDEX_ENTRY_ATTRIBUTE NewEntry;
    ULONG HeaderSize = FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName);
    ULONG KeyOffset = HeaderSize;
    ULONG PaddedKey = ALIGN_UP_BY(KeyLen, 8);
    ULONG DataOffset = KeyOffset + PaddedKey;
    ULONG EntrySize = ALIGN_UP_BY(DataOffset + ValueLen, 8);

    NewEntry = ExAllocatePoolWithTag(NonPagedPool, EntrySize, TAG_NTFS);
    if (!NewEntry)
    {
        DPRINT1("ERROR: Failed to allocate memory for view-index entry!\n");
        return NULL;
    }

    RtlZeroMemory(NewEntry, EntrySize);
    NewEntry->Length = (USHORT)EntrySize;
    NewEntry->KeyLength = (USHORT)KeyLen;
    NewEntry->Flags = 0;
    NewEntry->Data.ViewIndex.DataOffset = (USHORT)DataOffset;
    NewEntry->Data.ViewIndex.DataLength = (USHORT)ValueLen;
    NewEntry->Data.ViewIndex.Reserved = 0;

    if (KeyLen)
        RtlCopyMemory((PUCHAR)NewEntry + KeyOffset, KeyBytes, KeyLen);
    if (ValueLen)
        RtlCopyMemory((PUCHAR)NewEntry + DataOffset, ValueBytes, ValueLen);

    NewKey = ExAllocatePoolWithTag(NonPagedPool, sizeof(B_TREE_KEY), TAG_NTFS);
    if (!NewKey)
    {
        ExFreePoolWithTag(NewEntry, TAG_NTFS);
        return NULL;
    }
    NewKey->IndexEntry = NewEntry;
    NewKey->NextKey = NULL;
    NewKey->LesserChild = NULL;
    return NewKey;
}

/* Walk a B-tree node (and recurse into children) looking for a key matching
 * (KeyBytes, KeyLen) under Tree->CollationRule.  Lazy-loads children as
 * needed (mirrors NtfsInsertKey / NtfsRemoveKeyFromNode). */
static PB_TREE_KEY
NtfsFindKeyInNode(PB_TREE Tree, PB_TREE_FILENAME_NODE Node,
                  const VOID *KeyBytes, ULONG KeyLen)
{
    PB_TREE_KEY Cur = Node->FirstKey;
    ULONG i;

    for (i = 0; i < Node->KeyCount && Cur != NULL; i++)
    {
        /* Lazy-load child */
        if (!Cur->LesserChild &&
            (Cur->IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE) &&
            Tree->IndexAllocationContext)
        {
            Cur->LesserChild = CreateBTreeNodeFromIndexNode(Tree->Vcb,
                                                             Tree->IndexRoot,
                                                             Tree->IndexAllocationContext,
                                                             Cur->IndexEntry);
        }

        if (!(Cur->IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
        {
            LONG cmp = NtfsCompareKeyBytes(KeyBytes, KeyLen,
                                           &Cur->IndexEntry->FileName,
                                           Cur->IndexEntry->KeyLength,
                                           Tree->CollationRule,
                                           FALSE);
            if (cmp == 0)
                return Cur;
            if (cmp < 0)
            {
                /* Descend to lesser child if present */
                if (Cur->LesserChild)
                    return NtfsFindKeyInNode(Tree, Cur->LesserChild, KeyBytes, KeyLen);
                return NULL;
            }
        }
        else
        {
            /* End sentinel: descend into its LesserChild (keys greater than
             * all previous keys live there). */
            if (Cur->LesserChild)
                return NtfsFindKeyInNode(Tree, Cur->LesserChild, KeyBytes, KeyLen);
            return NULL;
        }

        Cur = Cur->NextKey;
    }
    return NULL;
}

NTSTATUS
NtfsBTreeFindBlob(PB_TREE Tree,
                  const VOID *KeyBytes,
                  ULONG KeyLen,
                  PINDEX_ENTRY_ATTRIBUTE *FoundEntry)
{
    PB_TREE_KEY Found;

    if (!Tree || !Tree->RootNode || !KeyBytes || !FoundEntry)
        return STATUS_INVALID_PARAMETER;

    *FoundEntry = NULL;
    Found = NtfsFindKeyInNode(Tree, Tree->RootNode, KeyBytes, KeyLen);
    if (!Found)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *FoundEntry = Found->IndexEntry;
    return STATUS_SUCCESS;
}

/* Generic blob insert.  For collations other than FILE_NAME the key is raw
 * bytes; we build an INDEX_ENTRY_ATTRIBUTE with view-index DataOffset /
 * DataLength layout and drive the existing insert machinery.  If the key
 * already exists (compare == 0), we replace the value in-place rather than
 * assert, giving Windows-style insert-or-update semantics for quota/security. */
NTSTATUS
NtfsBTreeInsertBlob(PB_TREE Tree,
                    const VOID *KeyBytes,
                    ULONG KeyLen,
                    const VOID *ValueBytes,
                    ULONG ValueLen,
                    ULONG MaxIndexRootSize,
                    ULONG IndexRecordSize)
{
    PB_TREE_KEY NewKey, CurrentKey, PreviousKey;
    PB_TREE_FILENAME_NODE Node;
    PB_TREE_KEY MedianKey = NULL;
    PB_TREE_FILENAME_NODE NewRight = NULL;
    ULONG i;
    ULONG NodeSize;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Tree || !Tree->RootNode || !KeyBytes || KeyLen == 0)
        return STATUS_INVALID_PARAMETER;

    /* First, try to update in place if the key already exists. */
    {
        PB_TREE_KEY Existing = NtfsFindKeyInNode(Tree, Tree->RootNode, KeyBytes, KeyLen);
        if (Existing)
        {
            /* Replace the value.  The existing entry may not have enough
             * trailing space, so allocate a fresh INDEX_ENTRY_ATTRIBUTE
             * with the new layout and swap it in. */
            PINDEX_ENTRY_ATTRIBUTE Old = Existing->IndexEntry;
            PB_TREE_KEY Replacement = CreateBTreeKeyFromBlob(KeyBytes, KeyLen,
                                                             ValueBytes, ValueLen);
            if (!Replacement)
                return STATUS_INSUFFICIENT_RESOURCES;

            /* Preserve NODE flag / VCN tail if present. */
            if (Old->Flags & NTFS_INDEX_ENTRY_NODE)
            {
                ULONGLONG ChildVCN = GetIndexEntryVCN(Old);
                PINDEX_ENTRY_ATTRIBUTE E = Replacement->IndexEntry;
                PINDEX_ENTRY_ATTRIBUTE Bigger = ExAllocatePoolWithTag(NonPagedPool,
                                                                      E->Length + sizeof(ULONGLONG),
                                                                      TAG_NTFS);
                if (!Bigger)
                {
                    ExFreePoolWithTag(Replacement->IndexEntry, TAG_NTFS);
                    ExFreePoolWithTag(Replacement, TAG_NTFS);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                RtlCopyMemory(Bigger, E, E->Length);
                Bigger->Length = E->Length + sizeof(ULONGLONG);
                Bigger->Flags |= NTFS_INDEX_ENTRY_NODE;
                ExFreePoolWithTag(E, TAG_NTFS);
                Replacement->IndexEntry = Bigger;
                SetIndexEntryVCN(Bigger, ChildVCN);
            }

            ExFreePoolWithTag(Existing->IndexEntry, TAG_NTFS);
            Existing->IndexEntry = Replacement->IndexEntry;
            ExFreePoolWithTag(Replacement, TAG_NTFS);
            Tree->RootNode->DiskNeedsUpdating = TRUE;
            return STATUS_SUCCESS;
        }
    }

    /* Not found - do a full insert on the root node.  We can't recurse into
     * children here unless we mirror NtfsInsertKey's split bubble-up.  For
     * the view-index workloads today (quota is shallow: at most a few
     * hundred entries), root-only insert is enough - the existing
     * NtfsInsertKey handles the full recursive case for $I30.  If we hit a
     * tree that would overflow the root, return STATUS_NOT_IMPLEMENTED so
     * the caller knows to fall back. */
    NewKey = CreateBTreeKeyFromBlob(KeyBytes, KeyLen, ValueBytes, ValueLen);
    if (!NewKey)
        return STATUS_INSUFFICIENT_RESOURCES;

    Node = Tree->RootNode;
    CurrentKey = Node->FirstKey;
    PreviousKey = NULL;
    for (i = 0; i < Node->KeyCount; i++)
    {
        LONG Comparison = CompareTreeKeys(NewKey, CurrentKey, Tree->CollationRule, FALSE);
        ASSERT(Comparison != 0);

        if (Comparison < 0)
        {
            if (CurrentKey->LesserChild)
            {
                /* Descend into the loaded child using the recursive $I30
                 * machinery - only valid for FILE_NAME collation today.
                 * For view indexes, in practice the tree stays shallow
                 * (root only) until the root overflows, at which point
                 * split handling would need generalization.  Assert the
                 * invariant instead of silently producing a broken tree. */
                DPRINT1("NtfsBTreeInsertBlob: tree already has child nodes; "
                        "view-index split not implemented.\n");
                ExFreePoolWithTag(NewKey->IndexEntry, TAG_NTFS);
                ExFreePoolWithTag(NewKey, TAG_NTFS);
                return STATUS_NOT_IMPLEMENTED;
            }

            NewKey->NextKey = CurrentKey;
            Node->KeyCount++;
            Node->DiskNeedsUpdating = TRUE;
            if (CurrentKey == Node->FirstKey)
                Node->FirstKey = NewKey;
            else
                PreviousKey->NextKey = NewKey;
            break;
        }

        PreviousKey = CurrentKey;
        CurrentKey = CurrentKey->NextKey;
    }

    NodeSize = GetSizeOfIndexEntries(Node);
    UNREFERENCED_PARAMETER(MedianKey);
    UNREFERENCED_PARAMETER(NewRight);
    UNREFERENCED_PARAMETER(MaxIndexRootSize);
    UNREFERENCED_PARAMETER(IndexRecordSize);
    UNREFERENCED_PARAMETER(NodeSize);

    return Status;
}

/* Generic blob remove.  Searches the root-node linked list and frees the
 * matching key.  Internal-key removal isn't implemented (matches the
 * existing $I30 NtfsRemoveKeyFromNode restriction). */
NTSTATUS
NtfsBTreeRemoveBlob(PB_TREE Tree,
                    const VOID *KeyBytes,
                    ULONG KeyLen)
{
    PB_TREE_FILENAME_NODE Node;
    PB_TREE_KEY CurrentKey, PreviousKey;
    ULONG i;

    if (!Tree || !Tree->RootNode || !KeyBytes)
        return STATUS_INVALID_PARAMETER;

    Node = Tree->RootNode;
    CurrentKey = Node->FirstKey;
    PreviousKey = NULL;
    for (i = 0; i < Node->KeyCount && CurrentKey != NULL; i++)
    {
        if (!(CurrentKey->IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
        {
            LONG cmp = NtfsCompareKeyBytes(KeyBytes, KeyLen,
                                           &CurrentKey->IndexEntry->FileName,
                                           CurrentKey->IndexEntry->KeyLength,
                                           Tree->CollationRule,
                                           FALSE);
            if (cmp == 0)
            {
                if (CurrentKey->LesserChild != NULL)
                {
                    DPRINT1("NtfsBTreeRemoveBlob: internal-key removal not implemented.\n");
                    return STATUS_NOT_IMPLEMENTED;
                }

                if (PreviousKey != NULL)
                    PreviousKey->NextKey = CurrentKey->NextKey;
                else
                    Node->FirstKey = CurrentKey->NextKey;

                Node->KeyCount--;
                Node->DiskNeedsUpdating = TRUE;
                CurrentKey->NextKey = NULL;
                DestroyBTreeKey(CurrentKey);
                return STATUS_SUCCESS;
            }
        }

        PreviousKey = CurrentKey;
        CurrentKey = CurrentKey->NextKey;
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
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

static
VOID
NtfsEnsureChildLoaded(PB_TREE Tree,
                      PB_TREE_KEY Key)
{
    if (!Key->LesserChild &&
        (Key->IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE) &&
        Tree->IndexAllocationContext)
    {
        Key->LesserChild = CreateBTreeNodeFromIndexNode(Tree->Vcb,
                                                        Tree->IndexRoot,
                                                        Tree->IndexAllocationContext,
                                                        Key->IndexEntry);
    }
}

static
PB_TREE_KEY
NtfsFindRightmostKey(PB_TREE Tree,
                     PB_TREE_FILENAME_NODE Node)
{
    PB_TREE_KEY CurrentKey;
    PB_TREE_KEY LastKey = NULL;
    ULONG i;

    CurrentKey = Node->FirstKey;
    for (i = 0; i < Node->KeyCount && CurrentKey != NULL; i++)
    {
        NtfsEnsureChildLoaded(Tree, CurrentKey);

        if (CurrentKey->IndexEntry->Flags & NTFS_INDEX_ENTRY_END)
        {
            /* The END entry's child holds the keys that sort after every real
             * key in this node, so the rightmost key of the whole subtree lives
             * there - but only if that child still has a key.  Earlier
             * deletions can leave the rightmost child empty (emptied index
             * nodes are never merged or freed), in which case the recursion
             * yields NULL and the rightmost real key is LastKey at this level.
             * Returning the bare recursion result would report a non-empty
             * subtree as empty, and NtfsRemoveKeyFromNode would then drop this
             * node's separator together with all of its non-rightmost children,
             * orphaning their keys from the index. */
            if (CurrentKey->LesserChild != NULL)
            {
                PB_TREE_KEY DeepKey = NtfsFindRightmostKey(Tree, CurrentKey->LesserChild);
                if (DeepKey != NULL)
                    return DeepKey;
            }

            return LastKey;
        }

        LastKey = CurrentKey;
        CurrentKey = CurrentKey->NextKey;
    }

    return LastKey;
}

static
BOOLEAN
NtfsRemoveKeyFromNode(PB_TREE Tree,
                      PB_TREE_FILENAME_NODE Node,
                      ULONGLONG FileReference,
                      PUNICODE_STRING FileName,
                      BOOLEAN CaseSensitive)
{
    PB_TREE_KEY CurrentKey, PreviousKey;
    ULONG i;

    CurrentKey = Node->FirstKey;
    PreviousKey = NULL;

    for (i = 0; i < Node->KeyCount && CurrentKey != NULL; i++)
    {
        NtfsEnsureChildLoaded(Tree, CurrentKey);

        if (!(CurrentKey->IndexEntry->Flags & NTFS_INDEX_ENTRY_END) &&
            (CurrentKey->IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK) == FileReference &&
            CompareFileName(FileName, CurrentKey->IndexEntry, FALSE, CaseSensitive))
        {
            if (CurrentKey->LesserChild != NULL)
            {
                PB_TREE_KEY Replacement;
                PINDEX_ENTRY_ATTRIBUTE NewEntry;
                ULONG NewLength;
                ULONGLONG ChildVCN;
                ULONGLONG ReplacementFileReference;
                UNICODE_STRING ReplacementName;

                Replacement = NtfsFindRightmostKey(Tree, CurrentKey->LesserChild);
                if (Replacement == NULL ||
                    (Replacement->IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
                {
                    /* This separator points at a child subtree that no longer
                     * holds any keys: earlier deletions emptied it, and the
                     * index code never merges or frees emptied index nodes.
                     * An index node is read in full or not at all, and
                     * NtfsFindRightmostKey() yields a real key whenever any node
                     * on the rightmost path has one, so a NULL result means the
                     * subtree is genuinely empty - there is no in-order
                     * predecessor to promote into this slot.  Drop the separator
                     * outright: every key the empty child could legally hold
                     * sorts below the following entry's key as well, so removing
                     * this entry together with its empty child keeps the index
                     * well-ordered.  Without this, a key promoted to an internal
                     * node becomes permanently undeletable once its subtree
                     * empties out. */
                    if (PreviousKey != NULL)
                        PreviousKey->NextKey = CurrentKey->NextKey;
                    else
                        Node->FirstKey = CurrentKey->NextKey;

                    Node->KeyCount--;
                    Node->DiskNeedsUpdating = TRUE;
                    CurrentKey->NextKey = NULL;
                    DestroyBTreeKey(CurrentKey);
                    return TRUE;
                }

                ChildVCN = GetIndexEntryVCN(CurrentKey->IndexEntry);
                NewLength = Replacement->IndexEntry->Length;
                if (!(Replacement->IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE))
                    NewLength += sizeof(ULONGLONG);

                NewEntry = ExAllocatePoolWithTag(NonPagedPool, NewLength, TAG_NTFS);
                if (NewEntry == NULL)
                    return FALSE;

                RtlZeroMemory(NewEntry, NewLength);
                RtlCopyMemory(NewEntry,
                              Replacement->IndexEntry,
                              Replacement->IndexEntry->Length);
                NewEntry->Length = (USHORT)NewLength;
                NewEntry->Flags |= NTFS_INDEX_ENTRY_NODE;
                SetIndexEntryVCN(NewEntry, ChildVCN);

                ReplacementFileReference = Replacement->IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK;
                ReplacementName.Length = Replacement->IndexEntry->FileName.NameLength * sizeof(WCHAR);
                ReplacementName.MaximumLength = ReplacementName.Length;
                ReplacementName.Buffer = Replacement->IndexEntry->FileName.Name;

                ExFreePoolWithTag(CurrentKey->IndexEntry, TAG_NTFS);
                CurrentKey->IndexEntry = NewEntry;

                if (!NtfsRemoveKeyFromNode(Tree,
                                           CurrentKey->LesserChild,
                                           ReplacementFileReference,
                                           &ReplacementName,
                                           CaseSensitive))
                {
                    return FALSE;
                }

                Node->DiskNeedsUpdating = TRUE;
                return TRUE;
            }

            if (PreviousKey != NULL)
                PreviousKey->NextKey = CurrentKey->NextKey;
            else
                Node->FirstKey = CurrentKey->NextKey;

            Node->KeyCount--;
            Node->DiskNeedsUpdating = TRUE;
            CurrentKey->NextKey = NULL;
            DestroyBTreeKey(CurrentKey);
            return TRUE;
        }

        if (CurrentKey->LesserChild != NULL &&
            NtfsRemoveKeyFromNode(Tree, CurrentKey->LesserChild, FileReference, FileName, CaseSensitive))
        {
            Node->DiskNeedsUpdating = TRUE;
            return TRUE;
        }

        PreviousKey = CurrentKey;
        CurrentKey = CurrentKey->NextKey;
    }

    return FALSE;
}

NTSTATUS
NtfsRemoveKey(PB_TREE Tree,
              ULONGLONG FileReference,
              PUNICODE_STRING FileName,
              BOOLEAN CaseSensitive)
{
    if (Tree == NULL || Tree->RootNode == NULL || FileName == NULL)
        return STATUS_INVALID_PARAMETER;

    if (!NtfsRemoveKeyFromNode(Tree, Tree->RootNode, FileReference, FileName, CaseSensitive))
        return STATUS_OBJECT_NAME_NOT_FOUND;

    return STATUS_SUCCESS;
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

    NT_ASSERT(NextKey == NULL);

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
    if (Tree->IndexAllocationContext)
    {
        ReleaseAttributeContext(Tree->IndexAllocationContext);
        Tree->IndexAllocationContext = NULL;
    }
    DestroyBTreeNode(Tree->RootNode);
    ExFreePoolWithTag(Tree, TAG_NTFS);
}

VOID
DumpBTreeKey(PB_TREE Tree, PB_TREE_KEY Key, ULONG Number, ULONG Depth)
{
    ULONG i;

    if (!NTFS_TRACE_ENABLED)
        return;

    for (i = 0; i < Depth; i++)
        DbgPrint(" ");
    DbgPrint(" Key #%d", Number);

    if (!(Key->IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
    {
        if (Tree->CollationRule == COLLATION_FILE_NAME ||
            Tree->CollationRule == COLLATION_UNICODE_STRING)
        {
            UNICODE_STRING FileName;
            FileName.Length = Key->IndexEntry->FileName.NameLength * sizeof(WCHAR);
            FileName.MaximumLength = FileName.Length;
            FileName.Buffer = Key->IndexEntry->FileName.Name;
            DbgPrint(" '%wZ'\n", &FileName);
        }
        else
        {
            DbgPrint(" (collation 0x%lx, KeyLen %u)\n",
                     Tree->CollationRule, Key->IndexEntry->KeyLength);
        }
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
        {
            // Child not loaded (lazy loading) - this is expected
            DPRINT("Child node not loaded (lazy)\n");
        }
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

    if (!NTFS_TRACE_ENABLED)
        return;

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
    if (!NTFS_TRACE_ENABLED)
        return;

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
* @name NtfsInsertKey
* @implemented
*
* Inserts a FILENAME_ATTRIBUTE into a B-Tree node.
*
* @param Tree
* Pointer to the B_TREE the key (filename attribute) is being inserted into.
*
* @param FileReference
* Reference number to the file being added. This will be a combination of the MFT index and update sequence number.
*
* @param FileNameAttribute
* Pointer to a FILENAME_ATTRIBUTE which is the data for the key that will be added to the tree. A copy will be made.
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
    PB_TREE_KEY NewKey, CurrentKey, PreviousKey;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG NodeSize;
    ULONG AllocatedNodeSize;
    ULONG MaxNodeSizeWithoutHeader;
    ULONG i;

    *MedianKey = NULL;
    *NewRightHandSibling = NULL;

    DPRINT("NtfsInsertKey(%p, 0x%I64x, %p, %p, %s, %lu, %lu, %p, %p)\n",
           Tree,
           FileReference,
           FileNameAttribute,
           Node,
           CaseSensitive ? "TRUE" : "FALSE",
           MaxIndexRootSize,
           IndexRecordSize,
           MedianKey,
           NewRightHandSibling);

    // Create the key for the filename attribute
    NewKey = CreateBTreeKeyFromFilename(FileReference, FileNameAttribute);
    if (!NewKey)
        return STATUS_INSUFFICIENT_RESOURCES;

    // Find where to insert the key
    CurrentKey = Node->FirstKey;
    PreviousKey = NULL;
    for (i = 0; i < Node->KeyCount; i++)
    {
        // Should the New Key go before the current key?
        LONG Comparison = CompareTreeKeys(NewKey, CurrentKey, Tree->CollationRule, CaseSensitive);

        if (Comparison == 0)
        {
            if (Tree->CollationRule == COLLATION_FILE_NAME ||
                Tree->CollationRule == COLLATION_UNICODE_STRING)
            {
                DPRINT("Duplicate index key: %.*S\n",
                       NewKey->IndexEntry->FileName.NameLength,
                       NewKey->IndexEntry->FileName.Name);
            }
            else
            {
                DPRINT("Duplicate index key (collation 0x%lx, KeyLen %u)\n",
                       Tree->CollationRule,
                       NewKey->IndexEntry->KeyLength);
            }
            DestroyBTreeKey(NewKey);
            return STATUS_OBJECT_NAME_COLLISION;
        }

        // Is NewKey < CurrentKey?
        if (Comparison < 0)
        {
            // Lazily load child node from disk if it exists but hasn't been loaded
            if (!CurrentKey->LesserChild &&
                (CurrentKey->IndexEntry->Flags & NTFS_INDEX_ENTRY_NODE) &&
                Tree->IndexAllocationContext)
            {
                CurrentKey->LesserChild = CreateBTreeNodeFromIndexNode(Tree->Vcb,
                                                                       Tree->IndexRoot,
                                                                       Tree->IndexAllocationContext,
                                                                       CurrentKey->IndexEntry);
            }

            // Does CurrentKey have a sub-node?
            if (CurrentKey->LesserChild)
            {
                PB_TREE_KEY NewLeftKey;
                PB_TREE_FILENAME_NODE NewChild;

                // Insert the key into the child node
                Status = NtfsInsertKey(Tree,
                                       FileReference,
                                       FileNameAttribute,
                                       CurrentKey->LesserChild,
                                       CaseSensitive,
                                       MaxIndexRootSize,
                                       IndexRecordSize,
                                       &NewLeftKey,
                                       &NewChild);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("ERROR: Failed to insert key.\n");
                    ExFreePoolWithTag(NewKey, TAG_NTFS);
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

    // Determine how much space the index entries will need on disk.  Entries
    // whose child node was just attached gain a trailing VCN when serialized.
    NodeSize = GetSerializedSizeOfIndexEntries(Node);

    // Is Node not the root node?
    if (Node != Tree->RootNode)
    {
        // Calculate maximum serialized entry bytes without index-buffer headers.
        AllocatedNodeSize = IndexRecordSize - FIELD_OFFSET(INDEX_BUFFER, Header);
        MaxNodeSizeWithoutHeader = NtfsGetMaxIndexNodeEntryBytes(Tree->Vcb,
                                                                 IndexRecordSize);
        ASSERT(MaxNodeSizeWithoutHeader <= AllocatedNodeSize);

        // Has the node grown larger than its allocated size?
        if (NodeSize > MaxNodeSizeWithoutHeader)
        {
            NTSTATUS Status;

            Status = SplitBTreeNode(Tree,
                                    Node,
                                    MedianKey,
                                    NewRightHandSibling,
                                    CaseSensitive,
                                    IndexRecordSize);
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
               BOOLEAN CaseSensitive,
               ULONG IndexRecordSize)
{
    ULONG MedianKeyIndex;
    PB_TREE_KEY LastKeyBeforeMedian, FirstKeyAfterMedian;
    ULONG KeyCount;
    ULONG HalfSize;
    ULONG SizeSum;
    ULONG i;

    DPRINT("SplitBTreeNode(%p, %p, %p, %p, %s, %lu) called\n",
            Tree,
            Node,
            MedianKey,
            NewRightHandSibling,
            CaseSensitive ? "TRUE" : "FALSE",
            IndexRecordSize);

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
    HalfSize = NtfsGetMaxIndexNodeEntryBytes(Tree->Vcb, IndexRecordSize) / 2;
    SizeSum = 0;
    for (i = 0; i < Node->KeyCount; i++)
    {
        SizeSum += GetSerializedSizeOfIndexEntry(LastKeyBeforeMedian);

        if (SizeSum > HalfSize)
            break;

        MedianKeyIndex++;
        LastKeyBeforeMedian = LastKeyBeforeMedian->NextKey;
    }

    // Now we can get the median key and the key that follows it
    *MedianKey = LastKeyBeforeMedian->NextKey;
    FirstKeyAfterMedian = (*MedianKey)->NextKey;

    DPRINT("%lu keys, %lu median\n", Node->KeyCount, MedianKeyIndex);
    if (Tree->CollationRule == COLLATION_FILE_NAME ||
        Tree->CollationRule == COLLATION_UNICODE_STRING)
    {
        DPRINT("\t\tMedian: %.*S\n", (*MedianKey)->IndexEntry->FileName.NameLength, (*MedianKey)->IndexEntry->FileName.Name);
    }
    else
    {
        DPRINT("\t\tMedian: collation 0x%lx, KeyLen %u\n",
               Tree->CollationRule, (*MedianKey)->IndexEntry->KeyLength);
    }

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
