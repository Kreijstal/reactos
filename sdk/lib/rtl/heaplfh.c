/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            sdk/lib/rtl/heaplfh.c
 * PURPOSE:         RTL Heap low-fragmentation front-end allocator
 */

/*
 * The Low-Fragmentation Heap (LFH) is an optional front-end that sits in front
 * of the back-end heap (heap.c) and services small, frequently recycled
 * allocations from dedicated "subsegments". A subsegment is a single back-end
 * allocation divided into a run of fixed-size blocks belonging to one size
 * class (bucket).
 *
 * The important property - and the reason real software depends on it - is that
 * the LFH keeps all of its free-list bookkeeping inside the block header. The
 * back-end heap, by contrast, threads a LIST_ENTRY through the *user payload* of
 * a freed block. With the LFH active, a freed block's payload is therefore left
 * untouched until the block is handed out again, which both lowers fragmentation
 * and matches the allocator behaviour Windows exposes.
 *
 * Layout of a subsegment (carved from one back-end allocation):
 *
 *   +---------------------------+ <- back-end user pointer == HEAP_LFH_SUBSEGMENT
 *   | HEAP_LFH_SUBSEGMENT       |
 *   +---------------------------+ <- Blocks (HEAP_ENTRY_SIZE aligned)
 *   | HEAP_ENTRY | user data    |  block 0  (stride bytes)
 *   +---------------------------+
 *   | HEAP_ENTRY | user data    |  block 1
 *   +---------------------------+
 *   |            ...            |
 *   +---------------------------+
 *
 * Each block header carries:
 *   - SegmentOffset == RTL_LFH_ENTRY_MARKER (0xFF): identifies the block as LFH
 *     owned. Back-end blocks always store a real segment index (< HEAP_SEGMENTS),
 *     so this is unambiguous.
 *   - PreviousSize: byte offset (in HEAP_ENTRY units) back to the subsegment
 *     header, so a block can find its owning subsegment.
 *   - When busy: Flags has HEAP_ENTRY_BUSY, Size is the stride in HEAP_ENTRY
 *     units and UnusedBytes is HEAP_ENTRY_SIZE, so RtlSizeHeap reports the block
 *     capacity, exactly like a back-end block.
 *   - When free: Flags has HEAP_ENTRY_BUSY cleared and Size holds the index of
 *     the next free block in the subsegment (RTL_LFH_NO_BLOCK terminates it).
 *
 * All LFH operations run under the owning heap's lock (the same lock the
 * back-end uses), so the front-end needs no separate synchronisation. The
 * subsegment backing memory is obtained from, and returned to, the back-end via
 * the public RtlAllocateHeap/RtlFreeHeap with HEAP_NO_SERIALIZE (the lock is
 * already held). Because subsegment backings are far larger than
 * RTL_LFH_MAX_BLOCK_SIZE they never recurse back into the front-end.
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>
#include <heap.h>

#define NDEBUG
#include <debug.h>

/* TYPES ********************************************************************/

typedef struct _HEAP_LFH_BUCKET
{
    LIST_ENTRY SubSegments;     /* List of HEAP_LFH_SUBSEGMENT for this size class */
    USHORT BlockStride;         /* Block stride in bytes (header + payload) */
    USHORT BlockCount;          /* Blocks per freshly created subsegment */
} HEAP_LFH_BUCKET, *PHEAP_LFH_BUCKET;

typedef struct _HEAP_LFH_SUBSEGMENT
{
    LIST_ENTRY ListEntry;       /* Entry in the bucket's SubSegments list */
    PHEAP_LFH_BUCKET Bucket;    /* Owning bucket */
    PUCHAR Blocks;              /* First block header */
    USHORT BlockStride;         /* Block stride in bytes */
    USHORT BlockCount;          /* Total blocks in this subsegment */
    USHORT FreeCount;           /* Currently free blocks */
    USHORT FirstFree;           /* Index of first free block, RTL_LFH_NO_BLOCK if none */
} HEAP_LFH_SUBSEGMENT, *PHEAP_LFH_SUBSEGMENT;

typedef struct _HEAP_LFH_CONTEXT
{
    PHEAP Heap;
    HEAP_LFH_BUCKET Buckets[RTL_LFH_BUCKET_COUNT];
} HEAP_LFH_CONTEXT, *PHEAP_LFH_CONTEXT;

/* Target backing size of a freshly created subsegment. */
#define RTL_LFH_SUBSEGMENT_TARGET   0x10000
#define RTL_LFH_MIN_BLOCKS          8
#define RTL_LFH_MAX_BLOCKS          1024

/* PRIVATE FUNCTIONS ********************************************************/

/*
 * Size-class table. Each band rounds the requested block size up to its
 * granularity, bounding intra-bucket waste. The bands together produce
 * RTL_LFH_BUCKET_COUNT (64) compact bucket indices covering block sizes up to
 * RTL_LFH_MAX_BLOCK_SIZE.
 */
static const struct
{
    USHORT MaxSize;
    USHORT Granularity;
    USHORT BaseIndex;
    USHORT PrevMax;
}
RtlpLfhSizeClasses[] =
{
    { 0x100,  0x10,   0, 0x000 },   /* 16 buckets */
    { 0x200,  0x20,  16, 0x100 },   /*  8 buckets */
    { 0x400,  0x40,  24, 0x200 },   /*  8 buckets */
    { 0x800,  0x80,  32, 0x400 },   /*  8 buckets */
    { 0x1000, 0x100, 40, 0x800 },   /*  8 buckets */
    { 0x2000, 0x200, 48, 0x1000 },  /*  8 buckets */
    { 0x4000, 0x400, 56, 0x2000 },  /*  8 buckets */
};

/*
 * Map a back-end block size (header + payload, a multiple of HEAP_ENTRY_SIZE) to
 * a compact bucket index and the rounded stride used for that bucket. Returns
 * RTL_LFH_BUCKET_COUNT when the size is too large for the front-end.
 */
static ULONG
RtlpLowFragHeapBucketIndex(SIZE_T BlockSize, PUSHORT Stride)
{
    ULONG i;
    SIZE_T Rounded;

    for (i = 0; i < RTL_NUMBER_OF(RtlpLfhSizeClasses); i++)
    {
        if (BlockSize <= RtlpLfhSizeClasses[i].MaxSize)
        {
            Rounded = ROUND_UP(BlockSize, RtlpLfhSizeClasses[i].Granularity);
            *Stride = (USHORT)Rounded;
            return RtlpLfhSizeClasses[i].BaseIndex +
                   (ULONG)((Rounded - RtlpLfhSizeClasses[i].PrevMax) / RtlpLfhSizeClasses[i].Granularity) - 1;
        }
    }

    return RTL_LFH_BUCKET_COUNT;
}

/* Recover the subsegment that owns a given LFH block from its header. */
static PHEAP_LFH_SUBSEGMENT
RtlpLowFragHeapBlockSubSegment(PHEAP_ENTRY HeapEntry)
{
    return (PHEAP_LFH_SUBSEGMENT)((PUCHAR)HeapEntry -
                                  ((SIZE_T)HeapEntry->PreviousSize << HEAP_ENTRY_SHIFT));
}

/*
 * Create a new subsegment for the given bucket and link it at the head of the
 * bucket's list. Runs with the heap lock held. Returns NULL on allocation
 * failure.
 */
static PHEAP_LFH_SUBSEGMENT
RtlpLowFragHeapCreateSubSegment(PHEAP Heap,
                                PHEAP_LFH_BUCKET Bucket,
                                USHORT Stride)
{
    PHEAP_LFH_SUBSEGMENT SubSegment;
    SIZE_T HeaderSize, BackingSize, OffsetUnits;
    ULONG BlockCount, i;

    /* Pick a block count that keeps the backing near the target size */
    BlockCount = RTL_LFH_SUBSEGMENT_TARGET / Stride;
    if (BlockCount < RTL_LFH_MIN_BLOCKS)
        BlockCount = RTL_LFH_MIN_BLOCKS;
    else if (BlockCount > RTL_LFH_MAX_BLOCKS)
        BlockCount = RTL_LFH_MAX_BLOCKS;

    HeaderSize = ROUND_UP(sizeof(HEAP_LFH_SUBSEGMENT), HEAP_ENTRY_SIZE);
    BackingSize = HeaderSize + (SIZE_T)BlockCount * Stride;

    /* The backing is much larger than RTL_LFH_MAX_BLOCK_SIZE, so this stays in
       the back-end and does not recurse into the front-end. The lock is already
       held, hence HEAP_NO_SERIALIZE. */
    SubSegment = RtlAllocateHeap(Heap, HEAP_NO_SERIALIZE, BackingSize);
    if (!SubSegment)
        return NULL;

    SubSegment->Bucket = Bucket;
    SubSegment->BlockStride = Stride;
    SubSegment->BlockCount = (USHORT)BlockCount;
    SubSegment->FreeCount = (USHORT)BlockCount;
    SubSegment->FirstFree = 0;
    SubSegment->Blocks = (PUCHAR)SubSegment + HeaderSize;

    /* Initialise each block header and thread the intrusive free list */
    for (i = 0; i < BlockCount; i++)
    {
        PHEAP_ENTRY HeapEntry = (PHEAP_ENTRY)(SubSegment->Blocks + (SIZE_T)i * Stride);

        OffsetUnits = ((PUCHAR)HeapEntry - (PUCHAR)SubSegment) >> HEAP_ENTRY_SHIFT;
        HeapEntry->PreviousSize = (USHORT)OffsetUnits;
        HeapEntry->SegmentOffset = RTL_LFH_ENTRY_MARKER;
        HeapEntry->Flags = 0;
        HeapEntry->Size = (USHORT)((i + 1 < BlockCount) ? (i + 1) : RTL_LFH_NO_BLOCK);
    }

    if (Bucket->BlockStride == 0)
    {
        Bucket->BlockStride = Stride;
        Bucket->BlockCount = (USHORT)BlockCount;
    }

    InsertHeadList(&Bucket->SubSegments, &SubSegment->ListEntry);
    return SubSegment;
}

/* PUBLIC FUNCTIONS *********************************************************/

/*
 * Enable the low-fragmentation front-end for a heap by allocating its context.
 * Runs with the heap lock held. The context is disabled (FrontEndHeapType == 1)
 * while being allocated so the context allocation itself uses the back-end.
 */
BOOLEAN
NTAPI
RtlpLowFragHeapEnable(PHEAP Heap)
{
    PHEAP_LFH_CONTEXT Context;
    ULONG i;

    /* Keep the front-end disabled while we allocate the context, otherwise the
       allocation below would recurse straight back here. */
    Heap->FrontEndHeapType = 1;

    Context = RtlAllocateHeap(Heap,
                              HEAP_NO_SERIALIZE | HEAP_ZERO_MEMORY,
                              sizeof(HEAP_LFH_CONTEXT));
    if (!Context)
    {
        /* Leave the front-end disabled; we will not keep retrying */
        return FALSE;
    }

    Context->Heap = Heap;
    for (i = 0; i < RTL_LFH_BUCKET_COUNT; i++)
    {
        InitializeListHead(&Context->Buckets[i].SubSegments);
        Context->Buckets[i].BlockStride = 0;
        Context->Buckets[i].BlockCount = 0;
    }

    Heap->FrontEndHeap = Context;
    Heap->FrontEndHeapType = 2;
    return TRUE;
}

/*
 * Allocate a block of (already rounded) size AllocationSize from the
 * low-fragmentation front-end. Runs with the heap lock held. Returns the user
 * pointer, or NULL to let the caller fall back to the back-end.
 */
PVOID
NTAPI
RtlpLowFragHeapAllocate(PHEAP Heap,
                        SIZE_T AllocationSize)
{
    PHEAP_LFH_CONTEXT Context;
    PHEAP_LFH_BUCKET Bucket;
    PHEAP_LFH_SUBSEGMENT SubSegment;
    PHEAP_ENTRY HeapEntry;
    PLIST_ENTRY Entry;
    USHORT Stride;
    ULONG Index, BlockIndex;

    /* Create the context lazily on first use */
    if (Heap->FrontEndHeap == NULL)
    {
        if (!RtlpLowFragHeapEnable(Heap))
            return NULL;
    }

    Context = (PHEAP_LFH_CONTEXT)Heap->FrontEndHeap;

    Index = RtlpLowFragHeapBucketIndex(AllocationSize, &Stride);
    if (Index >= RTL_LFH_BUCKET_COUNT)
        return NULL;

    Bucket = &Context->Buckets[Index];

    /* Find a subsegment that still has a free block (non-full ones are kept at
       the head of the list). */
    SubSegment = NULL;
    for (Entry = Bucket->SubSegments.Flink;
         Entry != &Bucket->SubSegments;
         Entry = Entry->Flink)
    {
        PHEAP_LFH_SUBSEGMENT Candidate = CONTAINING_RECORD(Entry, HEAP_LFH_SUBSEGMENT, ListEntry);
        if (Candidate->FreeCount != 0)
        {
            SubSegment = Candidate;
            break;
        }
    }

    if (SubSegment == NULL)
    {
        SubSegment = RtlpLowFragHeapCreateSubSegment(Heap, Bucket, Stride);
        if (SubSegment == NULL)
            return NULL;
    }

    /* Pop the first free block */
    BlockIndex = SubSegment->FirstFree;
    HeapEntry = (PHEAP_ENTRY)(SubSegment->Blocks + (SIZE_T)BlockIndex * SubSegment->BlockStride);
    SubSegment->FirstFree = HeapEntry->Size;
    SubSegment->FreeCount--;

    /* Mark the block busy. SegmentOffset (LFH marker) and PreviousSize (offset
       to the subsegment) were set when the subsegment was carved. */
    HeapEntry->Size = (USHORT)(SubSegment->BlockStride >> HEAP_ENTRY_SHIFT);
    HeapEntry->Flags = HEAP_ENTRY_BUSY;
    HeapEntry->SmallTagIndex = 0;
    HeapEntry->UnusedBytes = (UCHAR)HEAP_ENTRY_SIZE;

    /* If it just became full, move it to the tail so future searches skip it */
    if (SubSegment->FreeCount == 0)
    {
        RemoveEntryList(&SubSegment->ListEntry);
        InsertTailList(&Bucket->SubSegments, &SubSegment->ListEntry);
    }

    return HeapEntry + 1;
}

/*
 * Free a block that belongs to the low-fragmentation front-end. Runs with the
 * heap lock held. Returns FALSE if the block is not a valid busy LFH block.
 */
BOOLEAN
NTAPI
RtlpLowFragHeapFree(PHEAP Heap,
                    PVOID Ptr)
{
    PHEAP_LFH_SUBSEGMENT SubSegment;
    PHEAP_LFH_BUCKET Bucket;
    PHEAP_ENTRY HeapEntry;
    ULONG BlockIndex;

    HeapEntry = (PHEAP_ENTRY)Ptr - 1;

    /* Reject double frees / non-busy blocks */
    if (!(HeapEntry->Flags & HEAP_ENTRY_BUSY))
    {
        DPRINT1("HEAP: Trying to free an already free LFH block %p!\n", Ptr);
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    SubSegment = RtlpLowFragHeapBlockSubSegment(HeapEntry);
    Bucket = SubSegment->Bucket;
    BlockIndex = (ULONG)(((PUCHAR)HeapEntry - SubSegment->Blocks) / SubSegment->BlockStride);

    /* Push the block back onto the subsegment free list (payload untouched) */
    HeapEntry->Flags = 0;
    HeapEntry->Size = SubSegment->FirstFree;
    SubSegment->FirstFree = (USHORT)BlockIndex;
    SubSegment->FreeCount++;

    if (SubSegment->FreeCount == SubSegment->BlockCount)
    {
        /* Fully free: release the backing to the back-end, but keep one empty
           subsegment cached to avoid create/destroy churn. */
        if (Bucket->SubSegments.Flink != Bucket->SubSegments.Blink)
        {
            RemoveEntryList(&SubSegment->ListEntry);
            RtlFreeHeap(Heap, HEAP_NO_SERIALIZE, SubSegment);
        }
    }
    else if (SubSegment->FreeCount == 1)
    {
        /* It went from full to having a free block: move it to the head so it
           is found quickly by the next allocation. */
        RemoveEntryList(&SubSegment->ListEntry);
        InsertHeadList(&Bucket->SubSegments, &SubSegment->ListEntry);
    }

    return TRUE;
}

/*
 * Reallocate a low-fragmentation front-end block. Called from RtlReAllocateHeap
 * before the heap lock is taken, so it uses the public heap routines (which lock
 * themselves).
 */
PVOID
NTAPI
RtlpLowFragHeapReAllocate(PHEAP Heap,
                          ULONG Flags,
                          PVOID Ptr,
                          SIZE_T Size)
{
    PHEAP_ENTRY HeapEntry = (PHEAP_ENTRY)Ptr - 1;
    SIZE_T AllocationSize, OldSize, CopySize;
    USHORT NewStride, OldStride;
    PVOID NewPtr;

    OldStride = (USHORT)(HeapEntry->Size << HEAP_ENTRY_SHIFT);

    /* Compute the rounded back-end block size for the new request, exactly like
       RtlAllocateHeap does. */
    AllocationSize = Size ? Size : 1;
    AllocationSize = (AllocationSize + Heap->AlignRound) & Heap->AlignMask;

    /* If the request still fits the current block, keep it in place */
    if ((AllocationSize <= RTL_LFH_MAX_BLOCK_SIZE) &&
        (RtlpLowFragHeapBucketIndex(AllocationSize, &NewStride) < RTL_LFH_BUCKET_COUNT) &&
        (NewStride <= OldStride))
    {
        return Ptr;
    }

    /* In-place-only requests that need a different block must fail */
    if (Flags & HEAP_REALLOC_IN_PLACE_ONLY)
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus(STATUS_NO_MEMORY);
        return NULL;
    }

    /* General case: allocate a new block, copy, free the old one. Passing the
       original flags lets RtlAllocateHeap honour HEAP_ZERO_MEMORY for the grown
       tail (the copy below restores the live bytes). */
    NewPtr = RtlAllocateHeap(Heap, Flags, Size);
    if (NewPtr == NULL)
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus(STATUS_NO_MEMORY);
        return NULL;
    }

    OldSize = (SIZE_T)OldStride - HEAP_ENTRY_SIZE;
    CopySize = (OldSize < Size) ? OldSize : Size;
    RtlCopyMemory(NewPtr, Ptr, CopySize);

    RtlFreeHeap(Heap, Flags & HEAP_NO_SERIALIZE, Ptr);

    return NewPtr;
}

/* EOF */
