/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           Wait-on-address synchronization primitives (Win8+)
 * FILE:              lib/rtl/waitaddr.c
 *
 * NOTES:
 * Implements RtlWaitOnAddress / RtlWakeAddressSingle / RtlWakeAddressAll[NoFence].
 *
 * Each waiter places a small entry on a hash bucket list keyed by the
 * user-supplied address.  Blocking is done via the process-wide keyed event
 * (NULL handle = system-default keyed event), with the per-waiter entry
 * pointer used as the keyed-event "key".  This guarantees that a wake-side
 * NtReleaseKeyedEvent rendezvous matches the precise waiter that the wake
 * side already removed from the bucket list.
 */

/* INCLUDES ******************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* INTERNAL TYPES ************************************************************/

#define ADDR_HASH_BUCKETS 32
#define ADDR_HASH_MASK    (ADDR_HASH_BUCKETS - 1)

typedef struct _ADDR_WAIT_BUCKET
{
    RTL_CRITICAL_SECTION Lock;
    LIST_ENTRY WaiterList;
} ADDR_WAIT_BUCKET, *PADDR_WAIT_BUCKET;

typedef struct _ADDR_WAIT_ENTRY
{
    LIST_ENTRY ListEntry;
    PVOID Address;
    /* TRUE when a waker has removed us from the list and is about to
       (or has already) signaled the keyed event. */
    BOOLEAN Removed;
} ADDR_WAIT_ENTRY, *PADDR_WAIT_ENTRY;

/* GLOBALS *******************************************************************/

static ADDR_WAIT_BUCKET AddrWaitBuckets[ADDR_HASH_BUCKETS];

#define ADDR_INIT_IDLE    0
#define ADDR_INIT_RUNNING 1
#define ADDR_INIT_READY   2

static volatile LONG AddrWaitInitState = ADDR_INIT_IDLE;

/* INTERNAL HELPERS **********************************************************/

static
ULONG
AddrHash(IN PVOID Address)
{
    ULONG_PTR p = (ULONG_PTR)Address;
    /* Drop the bottom couple of bits - typical addresses are aligned -
       then fold the remaining bits with a multiplicative hash. */
    p >>= 2;
    p ^= p >> 16;
#ifdef _WIN64
    p ^= p >> 32;
#endif
    return (ULONG)(p & ADDR_HASH_MASK);
}

static
VOID
AddrEnsureInit(VOID)
{
    ULONG i;

    /* Readiness is published by a single flag, set only once EVERY bucket is
       usable.  A per-bucket flag - or gating on bucket 0, which the winner
       initializes first - lets a thread that hashes to a later bucket run
       ahead of the initializer and enter a still-zeroed RTL_CRITICAL_SECTION.
       That is not merely racy but fatal: a zeroed lock has LockCount 0 rather
       than the -1 RtlInitializeCriticalSection writes, so the very first
       acquirer's InterlockedIncrement yields 1, it takes the contended path,
       and it blocks forever on a lock whose OwningThread is 0 and which
       nobody will ever leave. */
    if (AddrWaitInitState == ADDR_INIT_READY)
        return;

    /* Slow path - winning thread initializes everyone, losers spin briefly. */
    if (InterlockedCompareExchange(&AddrWaitInitState,
                                   ADDR_INIT_RUNNING,
                                   ADDR_INIT_IDLE) == ADDR_INIT_IDLE)
    {
        for (i = 0; i < ADDR_HASH_BUCKETS; i++)
        {
            RtlInitializeCriticalSection(&AddrWaitBuckets[i].Lock);
            InitializeListHead(&AddrWaitBuckets[i].WaiterList);
        }

        /* Interlocked, so every bucket write above is published before the
           flag that advertises them. */
        InterlockedExchange(&AddrWaitInitState, ADDR_INIT_READY);
        return;
    }

    while (AddrWaitInitState != ADDR_INIT_READY)
        YieldProcessor();
}

static
BOOLEAN
AddrCompareEqual(IN volatile VOID *Address,
                 IN PVOID CompareAddress,
                 IN SIZE_T AddressSize)
{
    /* Re-read *Address atomically (size-dependent) and compare against
       *CompareAddress.  Returns TRUE iff bits match. */
    switch (AddressSize)
    {
        case 1:
            return (*(volatile UCHAR *)Address == *(UCHAR *)CompareAddress);
        case 2:
            return (*(volatile USHORT *)Address == *(USHORT *)CompareAddress);
        case 4:
            return (*(volatile ULONG *)Address == *(ULONG *)CompareAddress);
        case 8:
            return (*(volatile ULONGLONG *)Address == *(ULONGLONG *)CompareAddress);
        default:
            return TRUE; /* unreachable; caller validates */
    }
}

/* EXPORTED FUNCTIONS ********************************************************/

NTSTATUS
NTAPI
RtlWaitOnAddress(
    _In_ volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    ADDR_WAIT_ENTRY Entry;
    PADDR_WAIT_BUCKET Bucket;
    NTSTATUS Status;

    if (AddressSize != 1 && AddressSize != 2 && AddressSize != 4 && AddressSize != 8)
        return STATUS_INVALID_PARAMETER;

    AddrEnsureInit();
    Bucket = &AddrWaitBuckets[AddrHash((PVOID)Address)];

    Entry.Address = (PVOID)Address;
    Entry.Removed = FALSE;

    RtlEnterCriticalSection(&Bucket->Lock);

    /* Re-read under the bucket lock so a concurrent wake either sees us
       on the list, or *Address has already changed. */
    if (!AddrCompareEqual(Address, CompareAddress, AddressSize))
    {
        RtlLeaveCriticalSection(&Bucket->Lock);
        return STATUS_SUCCESS;
    }

    InsertTailList(&Bucket->WaiterList, &Entry.ListEntry);
    RtlLeaveCriticalSection(&Bucket->Lock);

    /* Block.  The keyed event "key" is &Entry - the same pointer the
       wake side will pass to NtReleaseKeyedEvent. */
    Status = NtWaitForKeyedEvent(NULL, &Entry, FALSE, Timeout);

    if (Status == STATUS_SUCCESS)
    {
        /* A waker rendezvoused with us - they already removed us
           from the list. */
        return STATUS_SUCCESS;
    }

    /* Timeout / failure path.  We need to either:
       (a) remove ourselves from the list, OR
       (b) drain the keyed event if a waker raced us and is about to /
           already issued NtReleaseKeyedEvent.  The waker holds the
           bucket lock to flip Entry.Removed atomically with the
           list-removal, so checking Removed under the lock is the
           authoritative answer. */
    RtlEnterCriticalSection(&Bucket->Lock);
    if (!Entry.Removed)
    {
        /* No waker reached us - pull ourselves out and return the
           original timeout/error status. */
        RemoveEntryList(&Entry.ListEntry);
        RtlLeaveCriticalSection(&Bucket->Lock);
        return Status;
    }
    RtlLeaveCriticalSection(&Bucket->Lock);

    /* A waker has us flagged Removed - it has either already called
       NtReleaseKeyedEvent (in which case our wait would have succeeded
       and we wouldn't be here) or it is about to.  Drain the matching
       release with an unbounded wait — keyed events guarantee a 1:1
       rendezvous so this completes promptly. */
    (VOID)NtWaitForKeyedEvent(NULL, &Entry, FALSE, NULL);
    return STATUS_SUCCESS;
}

VOID
NTAPI
RtlWakeAddressSingle(_In_ PVOID Address)
{
    PADDR_WAIT_BUCKET Bucket;
    PLIST_ENTRY ListEntry;
    PADDR_WAIT_ENTRY Found = NULL;

    if (Address == NULL)
        return;

    AddrEnsureInit();
    Bucket = &AddrWaitBuckets[AddrHash(Address)];

    RtlEnterCriticalSection(&Bucket->Lock);
    for (ListEntry = Bucket->WaiterList.Flink;
         ListEntry != &Bucket->WaiterList;
         ListEntry = ListEntry->Flink)
    {
        PADDR_WAIT_ENTRY Entry = CONTAINING_RECORD(ListEntry, ADDR_WAIT_ENTRY, ListEntry);
        if (Entry->Address == Address)
        {
            RemoveEntryList(&Entry->ListEntry);
            Entry->Removed = TRUE;
            Found = Entry;
            break;
        }
    }
    RtlLeaveCriticalSection(&Bucket->Lock);

    if (Found != NULL)
    {
        /* Rendezvous with the waiter.  Use NULL timeout (infinite) -
           keyed events guarantee 1:1 matching, so the waiter is
           obligated to either be in the wait already (rendezvous
           succeeds immediately) or to drain it from its timeout-cleanup
           path (also a guaranteed match). */
        (VOID)NtReleaseKeyedEvent(NULL, Found, FALSE, NULL);
    }
}

VOID
NTAPI
RtlWakeAddressAll(_In_ PVOID Address)
{
    PADDR_WAIT_BUCKET Bucket;
    PLIST_ENTRY ListEntry;
    PLIST_ENTRY Next;
    LIST_ENTRY ToWake;

    if (Address == NULL)
        return;

    AddrEnsureInit();
    Bucket = &AddrWaitBuckets[AddrHash(Address)];

    InitializeListHead(&ToWake);

    RtlEnterCriticalSection(&Bucket->Lock);
    ListEntry = Bucket->WaiterList.Flink;
    while (ListEntry != &Bucket->WaiterList)
    {
        PADDR_WAIT_ENTRY Entry = CONTAINING_RECORD(ListEntry, ADDR_WAIT_ENTRY, ListEntry);
        Next = ListEntry->Flink;
        if (Entry->Address == Address)
        {
            RemoveEntryList(&Entry->ListEntry);
            Entry->Removed = TRUE;
            InsertTailList(&ToWake, &Entry->ListEntry);
        }
        ListEntry = Next;
    }
    RtlLeaveCriticalSection(&Bucket->Lock);

    /* Now signal all the matching waiters outside the bucket lock so
       a fresh waker can make progress while we drain rendezvous. */
    while (!IsListEmpty(&ToWake))
    {
        PLIST_ENTRY Le = RemoveHeadList(&ToWake);
        PADDR_WAIT_ENTRY Entry = CONTAINING_RECORD(Le, ADDR_WAIT_ENTRY, ListEntry);
        (VOID)NtReleaseKeyedEvent(NULL, Entry, FALSE, NULL);
    }
}

VOID
NTAPI
RtlWakeAddressAllNoFence(_In_ PVOID Address)
{
    /* The "NoFence" variant skips an explicit memory fence on the wake
       path.  We rely on the bucket critical section's release semantics
       to publish prior writes to waiters; that's already a fence, so
       behaviorally we are equivalent to RtlWakeAddressAll. */
    RtlWakeAddressAll(Address);
}

/* EOF */
