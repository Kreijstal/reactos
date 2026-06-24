/*
 * PROJECT:   Registry manipulation library
 * LICENSE:   GPL - See COPYING in the top level directory
 * COPYRIGHT: Copyright 2005 Filip Navara <navaraf@reactos.org>
 *            Copyright 2005 Hartmut Birr
 *            Copyright 2001 - 2005 Eric Kohl
 */

#include "cmlib.h"

PHBIN CMAPI
HvpAddBin(
    PHHIVE RegistryHive,
    ULONG Size,
    HSTORAGE_TYPE Storage)
{
    PHMAP_ENTRY BlockList;
    PHBIN Bin;
    ULONG BinSize;
    ULONG i;
    ULONG BitmapSize;
    ULONG BlockCount;
    ULONG OldBlockListSize;
    ULONG NewBlockListSize;
    PHCELL Block;

    BinSize = ROUND_UP(Size + sizeof(HBIN), HBLOCK_SIZE);
    BlockCount = BinSize / HBLOCK_SIZE;

    Bin = RegistryHive->Allocate(BinSize, TRUE, TAG_CM);
    if (Bin == NULL)
        return NULL;
    RtlZeroMemory(Bin, BinSize);

    Bin->Signature = HV_HBIN_SIGNATURE;
    Bin->FileOffset = RegistryHive->Storage[Storage].Length *
                      HBLOCK_SIZE;
    Bin->Size = BinSize;

    /*
     * Grow the block list.  It is over-allocated to BlockListCapacity entries
     * and grown by doubling, so the common case reuses the existing array with
     * no reallocation - and therefore no window in which a concurrent lock-free
     * reader (HvIsCellAllocated()/HvpGetCellData(), possibly on another CPU)
     * could observe a freed BlockList.  When a real grow is required the old
     * array is superseded but NOT freed inline (a reader may still hold it); it
     * is retired onto StaleBlockLists and released only at hive teardown.  This
     * fixes the SMP 0x50 (PAGE_FAULT_IN_NONPAGED_AREA) seen in HvIsCellAllocated
     * <- HvpGetCellData <- CmpFindSubKeyByName during boot-time PnP key creation.
     */
    OldBlockListSize = RegistryHive->Storage[Storage].Length;
    NewBlockListSize = OldBlockListSize + BlockCount;

    if (RegistryHive->Storage[Storage].BlockList == NULL ||
        NewBlockListSize > RegistryHive->Storage[Storage].BlockListCapacity)
    {
        ULONG NewCapacity = RegistryHive->Storage[Storage].BlockListCapacity * 2;

        if (NewCapacity < NewBlockListSize)
            NewCapacity = NewBlockListSize;

        BlockList = RegistryHive->Allocate(sizeof(HMAP_ENTRY) * NewCapacity,
                                           TRUE, TAG_CM);
        if (BlockList == NULL)
        {
            RegistryHive->Free(Bin, 0);
            return NULL;
        }

        /* PagedPool is not zeroed: clear the whole array so the spare tail
           (indices >= Length) reads as unallocated for any racing reader. */
        RtlZeroMemory(BlockList, sizeof(HMAP_ENTRY) * NewCapacity);

        if (OldBlockListSize > 0)
        {
            PHMAP_ENTRY OldBlockList = RegistryHive->Storage[Storage].BlockList;
            PHV_STALE_BLOCKLIST Stale;

            RtlCopyMemory(BlockList, OldBlockList,
                          OldBlockListSize * sizeof(HMAP_ENTRY));

            /* Retire the old array instead of freeing it: a reader on another
               CPU may still hold the pointer.  Freed in HvpFreeHiveBins(). */
            Stale = RegistryHive->Allocate(sizeof(HV_STALE_BLOCKLIST), TRUE, TAG_CM);
            if (Stale != NULL)
            {
                Stale->BlockList = OldBlockList;
                Stale->Next = RegistryHive->Storage[Storage].StaleBlockLists;
                RegistryHive->Storage[Storage].StaleBlockLists = Stale;
            }
            /* If the bookkeeping node can't be allocated we still must not free
               the in-use old array; accept the bounded one-array leak. */
        }

        /*
         * Publish the new array (and capacity) BEFORE Length is bumped below.
         * On the amd64 TSO memory model a reader that observes the grown Length
         * has necessarily already observed the new BlockList pointer, so it can
         * never pair the old (shorter) pointer with the new (larger) Length.
         */
        RegistryHive->Storage[Storage].BlockList = BlockList;
        RegistryHive->Storage[Storage].BlockListCapacity = NewCapacity;
    }
    /* else: reuse the existing over-allocated array (no realloc, no free). */

    RegistryHive->Storage[Storage].Length += BlockCount;

    for (i = 0; i < BlockCount; i++)
    {
        RegistryHive->Storage[Storage].BlockList[OldBlockListSize + i].BlockAddress =
            ((ULONG_PTR)Bin + (i * HBLOCK_SIZE));
        RegistryHive->Storage[Storage].BlockList[OldBlockListSize + i].BinAddress = (ULONG_PTR)Bin;
    }

    /* Initialize a free block in this heap. */
    Block = (PHCELL)(Bin + 1);
    Block->Size = (LONG)(BinSize - sizeof(HBIN));

    if (Storage == Stable)
    {
        /* Calculate bitmap size in bytes (always a multiple of 32 bits). */
        BitmapSize = ROUND_UP(RegistryHive->Storage[Stable].Length,
                              sizeof(ULONG) * 8) / 8;

        /* Grow bitmap if necessary. */
        if (BitmapSize > RegistryHive->DirtyVector.SizeOfBitMap / 8)
        {
            PULONG BitmapBuffer;

            BitmapBuffer = RegistryHive->Allocate(BitmapSize, TRUE, TAG_CM);
            RtlZeroMemory(BitmapBuffer, BitmapSize);
            if (RegistryHive->DirtyVector.SizeOfBitMap > 0)
            {
                ASSERT(RegistryHive->DirtyVector.Buffer);
                RtlCopyMemory(BitmapBuffer,
                              RegistryHive->DirtyVector.Buffer,
                              RegistryHive->DirtyVector.SizeOfBitMap / 8);
                RegistryHive->Free(RegistryHive->DirtyVector.Buffer, 0);
            }
            RtlInitializeBitMap(&RegistryHive->DirtyVector, BitmapBuffer,
                                BitmapSize * 8);
        }

        /* Mark new bin dirty. */
        RtlSetBits(&RegistryHive->DirtyVector,
                   Bin->FileOffset / HBLOCK_SIZE,
                   BlockCount);

        /* Update size in the base block */
        RegistryHive->BaseBlock->Length += BinSize;
    }

    return Bin;
}
