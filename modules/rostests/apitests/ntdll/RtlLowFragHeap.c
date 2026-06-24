/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test for the low-fragmentation heap front-end
 * PROGRAMMER:      ReactOS Portable Systems Group
 *
 * The low-fragmentation heap (LFH) keeps a freed small block's free-list
 * bookkeeping in the block HEADER, leaving the user payload untouched.  The
 * plain back-end allocator instead threads its free list THROUGH the freed
 * payload, overwriting the first bytes of the block with LIST_ENTRY linkage.
 *
 * A C++ object's vtable pointer lives at offset 0 of its payload, so when an
 * object is freed and then (incorrectly, but as real software does) used again,
 * the back-end behaviour smashes that vtable pointer and a subsequent virtual
 * call jumps through heap free-list linkage into the wild.  This is exactly the
 * QtIFW msys2 installer's finalization access violation: Windows masks it
 * because its LFH preserves the freed payload, and ReactOS used to crash
 * because its back-end did not.  These tests pin that LFH invariant so the
 * masking behaviour cannot regress.
 */

#include "precomp.h"

#include <pseh/pseh2.h>

/* A minimal stand-in for a C++ object: a vtable pointer at offset 0 followed
   by a data member, and a vtable holding one method slot. */
typedef struct _TEST_OBJECT TEST_OBJECT;

typedef struct _TEST_VTABLE
{
    ULONG (NTAPI *GetMagic)(TEST_OBJECT *This);
} TEST_VTABLE;

struct _TEST_OBJECT
{
    const TEST_VTABLE *Vtable;
    ULONG Magic;
};

#define TEST_MAGIC 0x00C0FFEEUL

static
ULONG
NTAPI
ObjectGetMagic(TEST_OBJECT *This)
{
    return This->Magic;
}

static const TEST_VTABLE TestVtable = { ObjectGetMagic };

static
HANDLE
CreateLfhHeap(VOID)
{
    HANDLE Heap;
    NTSTATUS Status;
    ULONG Compat;
    SIZE_T ReturnLength;

    /* A growable, serialised heap is eligible for the LFH front-end. */
    Heap = RtlCreateHeap(HEAP_GROWABLE, NULL, 0, 0, NULL, NULL);
    if (Heap == NULL)
    {
        skip("Could not create a heap\n");
        return NULL;
    }

    /* Enabling the LFH is the only supported RtlSetHeapInformation operation
       (magic value 2 == HeapCompatibilityInformation: LFH). */
    Compat = 2;
    Status = RtlSetHeapInformation(Heap, HeapCompatibilityInformation, &Compat, sizeof(Compat));
    ok(Status == STATUS_SUCCESS, "RtlSetHeapInformation failed: 0x%lx\n", Status);

    /* And it must read back as enabled. */
    Compat = 0;
    ReturnLength = 0;
    Status = RtlQueryHeapInformation(Heap, HeapCompatibilityInformation, &Compat, sizeof(Compat), &ReturnLength);
    ok(Status == STATUS_SUCCESS, "RtlQueryHeapInformation failed: 0x%lx\n", Status);
    ok(Compat == 2, "Heap compatibility is %lu, expected 2 (LFH)\n", Compat);
    ok(ReturnLength == sizeof(ULONG), "ReturnLength is %Iu, expected %Iu\n", ReturnLength, sizeof(ULONG));

    return Heap;
}

/* The core regression: once a size class is served by the low-fragmentation
   front-end, a freed block's free-list bookkeeping lives in the block header,
   so the user payload survives the free.  This is what masks the installer's
   use-after-free.  The plain back-end instead threads the free list through the
   payload and would clobber the object's vtable pointer.

   The front-end only takes over a size class after it has seen enough live
   allocations of that size, so we first warm the bucket with a pool of live
   objects, then free one (while its neighbours keep the subsegment busy) and
   check that its payload was left intact. */
#define POOL_COUNT 64

static
VOID
TestFreedPayloadSurvives(HANDLE Heap)
{
    volatile TEST_OBJECT *Pool[POOL_COUNT];
    volatile TEST_OBJECT *Object;
    const TEST_VTABLE *VtableAfterFree;
    ULONG MagicAfterFree;
    ULONG CallResult = 0;
    ULONG i;
    BOOLEAN Faulted = FALSE;
    BOOLEAN AllAllocated = TRUE;

    /* Warm the size class so the LFH front-end owns it. */
    for (i = 0; i < POOL_COUNT; i++)
    {
        Pool[i] = RtlAllocateHeap(Heap, 0, sizeof(TEST_OBJECT));
        if (Pool[i] == NULL)
            AllAllocated = FALSE;
    }
    ok(AllAllocated, "Could not fill the object pool\n");

    /* The victim: a live object surrounded by other live objects, so freeing
       it cannot hand its subsegment back to the back-end. */
    Object = Pool[POOL_COUNT / 2];
    if (Object == NULL)
        goto Cleanup;

    Object->Vtable = &TestVtable;
    Object->Magic = TEST_MAGIC;

    /* Free the object, then deliberately use it again (the installer bug). */
    RtlFreeHeap(Heap, 0, (PVOID)Object);
    Pool[POOL_COUNT / 2] = NULL;

    VtableAfterFree = Object->Vtable;
    MagicAfterFree = Object->Magic;

    ok(VtableAfterFree == &TestVtable,
       "Freed object's vtable pointer was clobbered: %p, expected %p "
       "(free-list linkage smashed the payload)\n",
       (PVOID)VtableAfterFree, (PVOID)&TestVtable);
    ok(MagicAfterFree == TEST_MAGIC,
       "Freed object's data member was clobbered: 0x%lx, expected 0x%lx\n",
       MagicAfterFree, TEST_MAGIC);

    /* Reproduce the actual fault: a virtual call through the freed object.  If
       the vtable pointer survived this dispatches to ObjectGetMagic; if it was
       smashed this jumps through heap linkage and faults, which we catch so the
       failure is reported as a test result instead of crashing the runner. */
    _SEH2_TRY
    {
        CallResult = Object->Vtable->GetMagic((TEST_OBJECT *)Object);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Faulted = TRUE;
    }
    _SEH2_END;

    ok(!Faulted, "Virtual call through the freed object faulted\n");
    ok(CallResult == TEST_MAGIC, "Virtual call returned 0x%lx, expected 0x%lx\n",
       CallResult, TEST_MAGIC);

Cleanup:
    for (i = 0; i < POOL_COUNT; i++)
    {
        if (Pool[i] != NULL)
            RtlFreeHeap(Heap, 0, (PVOID)Pool[i]);
    }
}

/* Hammer the front-end with a churn of allocations and frees across the LFH
   size classes to shake out subsegment/free-list corruption. */
static
VOID
TestAllocFreeChurn(HANDLE Heap)
{
    static PVOID Buffers[512];
    static const SIZE_T Sizes[] = { 1, 8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 512, 1024, 2048 };
    ULONG s, i, Round;
    BOOLEAN AllOk = TRUE;

    for (s = 0; s < RTL_NUMBER_OF(Sizes); s++)
    {
        for (Round = 0; Round < 4; Round++)
        {
            for (i = 0; i < RTL_NUMBER_OF(Buffers); i++)
            {
                Buffers[i] = RtlAllocateHeap(Heap, 0, Sizes[s]);
                if (Buffers[i] == NULL)
                {
                    AllOk = FALSE;
                    continue;
                }
                /* Tag every block; RtlSizeHeap must report at least what we asked. */
                RtlFillMemory(Buffers[i], Sizes[s], (UCHAR)(i + 1));
                if (RtlSizeHeap(Heap, 0, Buffers[i]) < Sizes[s])
                    AllOk = FALSE;
            }

            /* Free half now, in interleaved order, to fragment the subsegments. */
            for (i = 0; i < RTL_NUMBER_OF(Buffers); i += 2)
            {
                if (Buffers[i] != NULL)
                {
                    RtlFreeHeap(Heap, 0, Buffers[i]);
                    Buffers[i] = NULL;
                }
            }
            for (i = 1; i < RTL_NUMBER_OF(Buffers); i += 2)
            {
                if (Buffers[i] != NULL)
                {
                    RtlFreeHeap(Heap, 0, Buffers[i]);
                    Buffers[i] = NULL;
                }
            }
        }
    }

    ok(AllOk, "Allocation churn hit a failure or a short block\n");
}

START_TEST(RtlLowFragHeap)
{
    HANDLE Heap;

    Heap = CreateLfhHeap();
    if (Heap == NULL)
        return;

    TestFreedPayloadSurvives(Heap);
    TestAllocFreeChurn(Heap);

    RtlDestroyHeap(Heap);
}
