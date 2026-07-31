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
 * user-supplied address and then blocks in NtWaitForAlertByThreadId; a waker
 * removes entries under the bucket lock and alerts each one's thread by id.
 *
 * The alert is per-thread and latching, which is what makes this correct: an
 * alert delivered to a thread that has not blocked yet is not lost, it is
 * consumed by that thread's next wait. That removes the two structural
 * hazards of the keyed-event rendezvous this used to be built on - a waker
 * had to pair 1:1 with its waiter, so it blocked until the waiter arrived
 * (stranding it forever if the waiter died in between), and a waiter that
 * timed out concurrently with a wake had to drain the pending release with a
 * second, unbounded wait.
 *
 * The alert is a thread-wide token rather than an address-specific one, so a
 * stale alert from an already-satisfied wake can make a later wait return
 * early. That is a spurious wakeup, which WaitOnAddress explicitly permits
 * (callers must re-check their predicate), and it is why the wait is wrapped
 * in a loop that re-checks the deadline rather than trusting a single return.
 */

/* INCLUDES ******************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

#if (NTDDI_VERSION >= NTDDI_WIN8)

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
    HANDLE ThreadId;
    /* TRUE once a waker has taken us off the list and is going to alert us. */
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

/* Take ourselves back off the bucket list, reporting whether a waker had
   already claimed us.  A claimed entry means an alert is inbound (or has
   already landed), so the caller must treat the wait as satisfied rather
   than as a timeout. */
static
BOOLEAN
AddrRemoveSelf(IN PADDR_WAIT_BUCKET Bucket,
               IN PADDR_WAIT_ENTRY Entry)
{
    BOOLEAN Claimed;

    RtlEnterCriticalSection(&Bucket->Lock);
    Claimed = Entry->Removed;
    if (!Claimed)
        RemoveEntryList(&Entry->ListEntry);
    RtlLeaveCriticalSection(&Bucket->Lock);

    return Claimed;
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
    LARGE_INTEGER Deadline;
    LARGE_INTEGER Remaining;
    NTSTATUS Status;

    if (AddressSize != 1 && AddressSize != 2 && AddressSize != 4 && AddressSize != 8)
        return STATUS_INVALID_PARAMETER;

    AddrEnsureInit();
    Bucket = &AddrWaitBuckets[AddrHash((PVOID)Address)];

    Entry.Address = (PVOID)Address;
    Entry.ThreadId = NtCurrentTeb()->ClientId.UniqueThread;
    Entry.Removed = FALSE;

    /* A relative timeout has to be turned into an absolute deadline up front:
       the wait below can be retried after a spurious alert, and re-passing the
       original relative value would restart the clock each time. */
    if ((Timeout != NULL) && (Timeout->QuadPart < 0))
    {
        NtQuerySystemTime(&Deadline);
        Deadline.QuadPart -= Timeout->QuadPart;
    }
    else if (Timeout != NULL)
    {
        Deadline = *Timeout;
    }

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

    for (;;)
    {
        PLARGE_INTEGER WaitTimeout = NULL;

        if (Timeout != NULL)
        {
            NtQuerySystemTime(&Remaining);
            Remaining.QuadPart -= Deadline.QuadPart;
            if (Remaining.QuadPart >= 0)
            {
                /* Deadline already passed. */
                if (AddrRemoveSelf(Bucket, &Entry))
                    return STATUS_SUCCESS;
                return STATUS_TIMEOUT;
            }
            WaitTimeout = &Remaining;
        }

        Status = NtWaitForAlertByThreadId(&Entry, WaitTimeout);

        if (Status == STATUS_TIMEOUT)
        {
            /* A waker may have claimed us in the window between the wait
               expiring and us reclaiming the bucket lock; if so its alert is
               already in flight and this is a wake, not a timeout. */
            if (AddrRemoveSelf(Bucket, &Entry))
                return STATUS_SUCCESS;
            return STATUS_TIMEOUT;
        }

        if (!NT_SUCCESS(Status))
        {
            if (AddrRemoveSelf(Bucket, &Entry))
                return STATUS_SUCCESS;
            return Status;
        }

        /* Alerted. Ours only if a waker actually took us off the list -
           otherwise it was a stale alert left over by an earlier wake and we
           have to keep waiting. */
        RtlEnterCriticalSection(&Bucket->Lock);
        if (Entry.Removed)
        {
            RtlLeaveCriticalSection(&Bucket->Lock);
            return STATUS_SUCCESS;
        }
        RtlLeaveCriticalSection(&Bucket->Lock);
    }
}

VOID
NTAPI
RtlWakeAddressSingle(_In_ PVOID Address)
{
    PADDR_WAIT_BUCKET Bucket;
    PLIST_ENTRY ListEntry;
    HANDLE ThreadId = NULL;

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
            ThreadId = Entry->ThreadId;
            break;
        }
    }
    RtlLeaveCriticalSection(&Bucket->Lock);

    /* Outside the lock: the alert cannot block, but there is no reason to
       hold up other wakers while it runs.  Once Removed is set the waiter
       will not exit without seeing it, so the entry stays alive until then. */
    if (ThreadId != NULL)
        NtAlertThreadByThreadId(ThreadId);
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

    /* Alert while still holding the bucket lock.  Each woken waiter needs the
       lock to confirm its Removed flag before it can return and reuse its
       stack entry, so nobody can vanish out from under this walk. */
    while (!IsListEmpty(&ToWake))
    {
        PLIST_ENTRY Le = RemoveHeadList(&ToWake);
        PADDR_WAIT_ENTRY Entry = CONTAINING_RECORD(Le, ADDR_WAIT_ENTRY, ListEntry);
        NtAlertThreadByThreadId(Entry->ThreadId);
    }
    RtlLeaveCriticalSection(&Bucket->Lock);
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

#endif /* NTDDI_VERSION >= NTDDI_WIN8 */

/* EOF */
