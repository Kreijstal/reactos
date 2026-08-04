/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           Slim Reader/Writer (SRW) Routines
 * PROGRAMMER:        Thomas Weidenmueller <w3seek@reactos.com>
 *
 * NOTES:             The algorithms used in this implementation
 *                    may be different from Vista's implementation.
 *                    Since applications should treat the RTL_SRWLOCK
 *                    structure as opaque data, it should not matter.
 *                    The algorithms are probably not as optimized.
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

#ifdef _WIN64
#define InterlockedBitTestAndSetPointer(ptr,val) InterlockedBitTestAndSet64((PLONGLONG)ptr,(LONGLONG)val)
#define InterlockedAddPointer(ptr,val) InterlockedAdd64((PLONGLONG)ptr,(LONGLONG)val)
#define InterlockedAndPointer(ptr,val) InterlockedAnd64((PLONGLONG)ptr,(LONGLONG)val)
#define InterlockedOrPointer(ptr,val) InterlockedOr64((PLONGLONG)ptr,(LONGLONG)val)
#define _ONE 1LL
#else
#define InterlockedBitTestAndSetPointer(ptr,val) InterlockedBitTestAndSet((PLONG)ptr,(LONG)val)
#define InterlockedAddPointer(ptr,val) InterlockedAdd((PLONG)ptr,(LONG)val)
#define InterlockedAndPointer(ptr,val) InterlockedAnd((PLONG)ptr,(LONG)val)
#define InterlockedOrPointer(ptr,val) InterlockedOr((PLONG)ptr,(LONG)val)
#define _ONE 1L
#endif

#define RTL_SRWLOCK_OWNED_BIT   0
#define RTL_SRWLOCK_CONTENDED_BIT   1
#define RTL_SRWLOCK_SHARED_BIT  2
#define RTL_SRWLOCK_CONTENTION_LOCK_BIT 3
#define RTL_SRWLOCK_OWNED   (_ONE << RTL_SRWLOCK_OWNED_BIT)
#define RTL_SRWLOCK_CONTENDED   (_ONE << RTL_SRWLOCK_CONTENDED_BIT)
#define RTL_SRWLOCK_SHARED  (_ONE << RTL_SRWLOCK_SHARED_BIT)
#define RTL_SRWLOCK_CONTENTION_LOCK (_ONE << RTL_SRWLOCK_CONTENTION_LOCK_BIT)
#define RTL_SRWLOCK_MASK    (RTL_SRWLOCK_OWNED | RTL_SRWLOCK_CONTENDED | \
                             RTL_SRWLOCK_SHARED | RTL_SRWLOCK_CONTENTION_LOCK)
#define RTL_SRWLOCK_BITS    4

/* States of the handshake that decides whether a waiter parks in the kernel
   and, correspondingly, whether a releaser has to signal a keyed event. A
   waiter only ever blocks after publishing RTLP_SRWLOCK_WAIT_BLOCKING, and a
   releaser only ever signals a waiter it observed in that state, so exactly
   one of "the waiter parks" and "the releaser signals" happens. Getting this
   wrong in either direction is fatal: an unmatched NtReleaseKeyedEvent blocks
   the releaser forever, and a missed signal leaves the waiter asleep. */
#define RTLP_SRWLOCK_WAIT_SPINNING  0
#define RTLP_SRWLOCK_WAIT_BLOCKING  1
#define RTLP_SRWLOCK_WAIT_WOKEN     2

/* How long to spin before parking. Spinning wins when the owner is about to
   release, which is the common case for the short critical sections SRW locks
   are meant for. Parking wins when it is not, and it is what keeps a heavily
   contended lock from burning every processor the owner needs in order to make
   progress -- without it, N waiting threads starve the one thread that could
   actually release the lock, and the lock stops making progress at all. */
#define RTLP_SRWLOCK_SPIN_COUNT     1024

/* Relative (negative) 100ns units: 100ms. */
#define RTLP_SRWLOCK_WAIT_TIMEOUT   (-100 * 10000LL)

typedef struct _RTLP_SRWLOCK_SHARED_WAKE
{
    LONG Wake;
    volatile struct _RTLP_SRWLOCK_SHARED_WAKE *Next;

    /* One of the RTLP_SRWLOCK_WAIT_* states above. */
    LONG WaitState;

    /* Scratch link, used by a releaser to collect the nodes it still owes a
       keyed event to. Only ever touched for nodes whose owner is parked, and
       therefore cannot return and reclaim this storage. */
    volatile struct _RTLP_SRWLOCK_SHARED_WAKE *WakeNext;
} volatile RTLP_SRWLOCK_SHARED_WAKE, *PRTLP_SRWLOCK_SHARED_WAKE;

typedef struct _RTLP_SRWLOCK_WAITBLOCK
{
    /* SharedCount is the number of shared acquirers. */
    LONG SharedCount;

    /* Last points to the last wait block in the chain. The value
       is only valid when read from the first wait block. */
    volatile struct _RTLP_SRWLOCK_WAITBLOCK *Last;

    /* Next points to the next wait block in the chain. */
    volatile struct _RTLP_SRWLOCK_WAITBLOCK *Next;

    union
    {
        /* Wake is only valid for exclusive wait blocks */
        LONG Wake;
        /* The wake chain is only valid for shared wait blocks */
        struct
        {
            PRTLP_SRWLOCK_SHARED_WAKE SharedWakeChain;
            PRTLP_SRWLOCK_SHARED_WAKE LastSharedWake;
        };
    };

    BOOLEAN Exclusive;

    /* One of the RTLP_SRWLOCK_WAIT_* states above. Only meaningful for
       exclusive wait blocks -- shared acquirers wait on their own wake node,
       and so use the WaitState in RTLP_SRWLOCK_SHARED_WAKE instead. */
    LONG WaitState;
} volatile RTLP_SRWLOCK_WAITBLOCK, *PRTLP_SRWLOCK_WAITBLOCK;

/* Tests whether the condition a waiter is waiting for has come true. */
typedef BOOLEAN (NTAPI *PRTLP_SRWLOCK_SATISFIED)(IN PVOID Context1,
                                                 IN PVOID Context2);


/* Park the calling thread until it is woken by RtlpSRWLockBeginWake() below,
   re-testing Satisfied() across the publication of the parked state so that a
   wake landing in that window cannot be lost. Returns when the caller should
   test its wait condition again -- it does not guarantee the condition holds. */
static VOID
NTAPI
RtlpSRWLockPark(IN OUT volatile LONG *WaitState,
                IN PVOID Key,
                IN PRTLP_SRWLOCK_SATISFIED Satisfied,
                IN PVOID Context1,
                IN PVOID Context2)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    /* Announce that we are about to park before re-testing the condition. A
       releaser only signals waiters it sees as BLOCKING, so a wake that lands
       between the caller's test and this exchange would be missed entirely if
       we did not test once more below. */
    if (InterlockedCompareExchange((PLONG)WaitState,
                                   RTLP_SRWLOCK_WAIT_BLOCKING,
                                   RTLP_SRWLOCK_WAIT_SPINNING) !=
        RTLP_SRWLOCK_WAIT_SPINNING)
    {
        /* Somebody already woke us; the caller's next test will see why. */
        YieldProcessor();
        return;
    }

    if (Satisfied(Context1, Context2))
    {
        /* It came true while we were publishing. Back out of parking -- but
           only if the releaser has not already committed to signalling us. If
           it has, we have to consume that signal, or it would be left dangling
           on this key for whichever thread parks on it next. */
        if (InterlockedCompareExchange((PLONG)WaitState,
                                       RTLP_SRWLOCK_WAIT_SPINNING,
                                       RTLP_SRWLOCK_WAIT_BLOCKING) ==
            RTLP_SRWLOCK_WAIT_BLOCKING)
        {
            return;
        }
    }

    /* Wait with a timeout rather than indefinitely. Every wake in this file
       goes through RtlpSRWLockBeginWake(), so a signal should always arrive;
       the timeout exists to bound the cost of ever being wrong about that. A
       missed wake then degrades to polling instead of losing the thread. */
    Timeout.QuadPart = RTLP_SRWLOCK_WAIT_TIMEOUT;
    for (;;)
    {
        Status = NtWaitForKeyedEvent(NULL, Key, FALSE, &Timeout);
        ASSERT(STATUS_INVALID_HANDLE != Status);
        if (Status != STATUS_TIMEOUT)
            break;

        /* Nothing signalled us. If the releaser has not committed we may stop
           waiting; if it has, we must stay, or its release would dangle. */
        if (InterlockedCompareExchange((PLONG)WaitState,
                                       RTLP_SRWLOCK_WAIT_SPINNING,
                                       RTLP_SRWLOCK_WAIT_BLOCKING) ==
            RTLP_SRWLOCK_WAIT_BLOCKING)
        {
            break;
        }
    }
}


/* Claim the parking handshake for a waiter that is about to be released.
   This must be called *before* that waiter's Wake flag is set: the moment the
   flag becomes visible a spinning waiter may return, and its wait block --
   which lives on its stack -- ceases to exist. Returns TRUE if the waiter
   parked in the kernel, in which case it cannot return, and so stays alive,
   until the caller signals its key with NtReleaseKeyedEvent(). */
static BOOLEAN
NTAPI
RtlpSRWLockBeginWake(IN OUT volatile LONG *WaitState)
{
    return (InterlockedExchange((PLONG)WaitState, RTLP_SRWLOCK_WAIT_WOKEN) ==
            RTLP_SRWLOCK_WAIT_BLOCKING);
}


/* Hand over every parked waiter collected on a WakeNext list. */
static VOID
NTAPI
RtlpSRWLockWakeParked(IN PRTLP_SRWLOCK_SHARED_WAKE ParkedList)
{
    PRTLP_SRWLOCK_SHARED_WAKE Parked;

    while (ParkedList != NULL)
    {
        /* Read the link before signalling: the moment this node's owner is
           released it returns, and the node goes with its stack frame. */
        Parked = ParkedList;
        ParkedList = Parked->WakeNext;

        (void)NtReleaseKeyedEvent(NULL,
                                  (PVOID)(ULONG_PTR)Parked,
                                  FALSE,
                                  NULL);
    }
}


static VOID
NTAPI
RtlpReleaseWaitBlockLockExclusive(IN OUT PRTL_SRWLOCK SRWLock,
                                  IN PRTLP_SRWLOCK_WAITBLOCK FirstWaitBlock)
{
    PRTLP_SRWLOCK_WAITBLOCK Next;
    PRTLP_SRWLOCK_SHARED_WAKE WakeChain, NextWake;
    PRTLP_SRWLOCK_SHARED_WAKE ParkedList = NULL;
    PVOID ExclusiveKey = NULL;
    LONG_PTR NewValue;
    BOOLEAN Exclusive;

    /* NOTE: We're currently in an exclusive lock in contended mode. */

    /* Wait blocks and their wake nodes live on the waiting threads' stacks and
       die as soon as those threads are released. Snapshot everything we still
       need out of the first wait block up front, so that nothing is read from
       it after it has been woken below. */
    Exclusive = FirstWaitBlock->Exclusive;
    Next = FirstWaitBlock->Next;
    WakeChain = Exclusive ? NULL : FirstWaitBlock->SharedWakeChain;

    if (Next != NULL)
    {
        /* There's more blocks chained, we need to update the pointers
           in the next wait block and update the wait block pointer. */
        NewValue = (LONG_PTR)Next | RTL_SRWLOCK_OWNED | RTL_SRWLOCK_CONTENDED;
        if (!Exclusive)
        {
            /* The next wait block has to be an exclusive lock! */
            ASSERT(Next->Exclusive);

            /* Save the shared count */
            Next->SharedCount = FirstWaitBlock->SharedCount;

            NewValue |= RTL_SRWLOCK_SHARED;
        }

        Next->Last = FirstWaitBlock->Last;
    }
    else
    {
        /* Convert the lock to a simple lock. */
        if (Exclusive)
            NewValue = RTL_SRWLOCK_OWNED;
        else
        {
            ASSERT(FirstWaitBlock->SharedCount > 0);

            NewValue = ((LONG_PTR)FirstWaitBlock->SharedCount << RTL_SRWLOCK_BITS) |
                       RTL_SRWLOCK_SHARED | RTL_SRWLOCK_OWNED;
        }
    }

    /* Wake the released acquirers now that everything we need has been read
       out of their wait blocks. Setting a waiter's Wake flag is what hands it
       the lock, so from that point on it may return at any moment and its wait
       block -- stack storage of the waiting thread -- ceases to exist. Nothing
       below may read from a block after waking it. */
    if (Exclusive)
    {
        /* Claim the parking handshake first: once Wake is set, a spinning
           waiter may return and take its wait block -- and its WaitState --
           away with its stack frame. A parked waiter cannot, so it stays
           alive until we signal its key further down. */
        if (RtlpSRWLockBeginWake(&FirstWaitBlock->WaitState))
            ExclusiveKey = (PVOID)(ULONG_PTR)FirstWaitBlock;

        (void)InterlockedOr(&FirstWaitBlock->Wake,
                            TRUE);
    }
    else
    {
        /* If we were the first one to acquire the shared
           lock, we now need to wake all others... */
        do
        {
            /* Read the link before waking this node: waking it hands the
               owning thread its lock, after which the node is gone. */
            NextWake = WakeChain->Next;

            if (RtlpSRWLockBeginWake(&WakeChain->WaitState))
            {
                /* A parked waiter cannot return until it is signalled, so its
                   node stays alive and can be threaded onto a private list to
                   be signalled once we are out of the wait block lock. */
                WakeChain->WakeNext = ParkedList;
                ParkedList = WakeChain;
            }

            (void)InterlockedOr((PLONG)&WakeChain->Wake,
                                TRUE);

            WakeChain = NextWake;
        } while (WakeChain != NULL);
    }

    (void)InterlockedExchangePointer(&SRWLock->Ptr, (PVOID)NewValue);

    /* Only now, with the wait block lock dropped, hand the parked threads
       over. NtReleaseKeyedEvent() blocks until its target actually reaches
       NtWaitForKeyedEvent(), and doing that while still holding the wait
       block lock would leave every other thread spinning for it -- trading
       one convoy for another. */
    if (ExclusiveKey != NULL)
        (void)NtReleaseKeyedEvent(NULL, ExclusiveKey, FALSE, NULL);

    RtlpSRWLockWakeParked(ParkedList);
}


static VOID
NTAPI
RtlpReleaseWaitBlockLockLastShared(IN OUT PRTL_SRWLOCK SRWLock,
                                   IN PRTLP_SRWLOCK_WAITBLOCK FirstWaitBlock)
{
    PRTLP_SRWLOCK_WAITBLOCK Next;
    LONG_PTR NewValue;
    BOOLEAN Parked;

    /* NOTE: We're currently in a shared lock in contended mode. */

    /* The next acquirer to be unwaited *must* be an exclusive lock! */
    ASSERT(FirstWaitBlock->Exclusive);

    Next = FirstWaitBlock->Next;
    if (Next != NULL)
    {
        /* There's more blocks chained, we need to update the pointers
           in the next wait block and update the wait block pointer. */
        NewValue = (LONG_PTR)Next | RTL_SRWLOCK_OWNED | RTL_SRWLOCK_CONTENDED;

        Next->Last = FirstWaitBlock->Last;
    }
    else
    {
        /* Convert the lock to a simple exclusive lock. */
        NewValue = RTL_SRWLOCK_OWNED;
    }

    /* Wake before publishing the new lock value, so that the wait block cannot
       be released -- and its stack storage reused -- while we still use it.
       The handshake has to be claimed before Wake is set, for the same
       reason: afterwards the block may already be gone. */
    Parked = RtlpSRWLockBeginWake(&FirstWaitBlock->WaitState);

    (void)InterlockedOr(&FirstWaitBlock->Wake,
                        TRUE);

    (void)InterlockedExchangePointer(&SRWLock->Ptr, (PVOID)NewValue);

    /* Signal only after the wait block lock is dropped -- see the comment in
       RtlpReleaseWaitBlockLockExclusive(). */
    if (Parked)
    {
        (void)NtReleaseKeyedEvent(NULL,
                                  (PVOID)(ULONG_PTR)FirstWaitBlock,
                                  FALSE,
                                  NULL);
    }
}


static VOID
NTAPI
RtlpReleaseWaitBlockLock(IN OUT PRTL_SRWLOCK SRWLock)
{
    InterlockedAndPointer(&SRWLock->Ptr,
                          ~RTL_SRWLOCK_CONTENTION_LOCK);
}


static PRTLP_SRWLOCK_WAITBLOCK
NTAPI
RtlpAcquireWaitBlockLock(IN OUT PRTL_SRWLOCK SRWLock)
{
    LONG_PTR PrevValue;
    PRTLP_SRWLOCK_WAITBLOCK WaitBlock;

    while (1)
    {
        PrevValue = InterlockedOrPointer(&SRWLock->Ptr,
                                         RTL_SRWLOCK_CONTENTION_LOCK);

        if (!(PrevValue & RTL_SRWLOCK_CONTENTION_LOCK))
            break;

        YieldProcessor();
    }

    if (!(PrevValue & RTL_SRWLOCK_CONTENDED) ||
        (PrevValue & ~RTL_SRWLOCK_MASK) == 0)
    {
        /* Too bad, looks like the wait block was removed in the
           meanwhile, unlock again */
        RtlpReleaseWaitBlockLock(SRWLock);
        return NULL;
    }

    WaitBlock = (PRTLP_SRWLOCK_WAITBLOCK)(PrevValue & ~RTL_SRWLOCK_MASK);

    return WaitBlock;
}


static BOOLEAN
NTAPI
RtlpSRWLockExclusiveSatisfied(IN PVOID Context1,
                              IN PVOID Context2)
{
    PRTL_SRWLOCK SRWLock = (PRTL_SRWLOCK)Context1;
    PRTLP_SRWLOCK_WAITBLOCK WaitBlock = (PRTLP_SRWLOCK_WAITBLOCK)Context2;
    LONG_PTR CurrentValue;

    CurrentValue = *(volatile LONG_PTR *)&SRWLock->Ptr;
    if (CurrentValue & RTL_SRWLOCK_SHARED)
        return FALSE;

    if (!(CurrentValue & RTL_SRWLOCK_CONTENDED))
    {
        /* The last wait block was removed and/or we're finally a simple
           exclusive lock. This means we don't need to wait anymore, we
           acquired the lock! */
        return TRUE;
    }

    /* Our wait block became the first one in the chain, we own the lock now! */
    return (WaitBlock->Wake != 0);
}


static VOID
NTAPI
RtlpAcquireSRWLockExclusiveWait(IN OUT PRTL_SRWLOCK SRWLock,
                                IN PRTLP_SRWLOCK_WAITBLOCK WaitBlock)
{
    ULONG SpinCount = RTLP_SRWLOCK_SPIN_COUNT;

    while (!RtlpSRWLockExclusiveSatisfied(SRWLock, (PVOID)(ULONG_PTR)WaitBlock))
    {
        if (SpinCount != 0)
        {
            SpinCount--;
            YieldProcessor();
            continue;
        }

        /* Spinning did not get us the lock, so stop competing for the
           processor that the current owner needs in order to release it. */
        RtlpSRWLockPark(&WaitBlock->WaitState,
                        (PVOID)(ULONG_PTR)WaitBlock,
                        RtlpSRWLockExclusiveSatisfied,
                        SRWLock,
                        (PVOID)(ULONG_PTR)WaitBlock);
    }
}


static BOOLEAN
NTAPI
RtlpSRWLockSharedWakeSatisfied(IN PVOID Context1,
                               IN PVOID Context2)
{
    PRTLP_SRWLOCK_SHARED_WAKE WakeChain = (PRTLP_SRWLOCK_SHARED_WAKE)Context2;

    UNREFERENCED_PARAMETER(Context1);

    return (WakeChain->Wake != 0);
}


static BOOLEAN
NTAPI
RtlpSRWLockSharedSatisfied(IN PVOID Context1,
                           IN PVOID Context2)
{
    PRTL_SRWLOCK SRWLock = (PRTL_SRWLOCK)Context1;
    PRTLP_SRWLOCK_SHARED_WAKE WakeChain = (PRTLP_SRWLOCK_SHARED_WAKE)Context2;
    LONG_PTR CurrentValue;

    CurrentValue = *(volatile LONG_PTR *)&SRWLock->Ptr;
    if (!(CurrentValue & RTL_SRWLOCK_SHARED))
        return FALSE;

    /* The RTL_SRWLOCK_OWNED bit always needs to be set when
       RTL_SRWLOCK_SHARED is set! */
    ASSERT(CurrentValue & RTL_SRWLOCK_OWNED);

    if (!(CurrentValue & RTL_SRWLOCK_CONTENDED))
    {
        /* The last wait block was removed and/or we're finally a simple
           shared lock. This means we don't need to wait anymore, we
           acquired the lock! */
        return TRUE;
    }

    /* Our wait block became the first one in the chain, we own the lock now! */
    return (WakeChain->Wake != 0);
}


static VOID
NTAPI
RtlpAcquireSRWLockSharedWait(IN OUT PRTL_SRWLOCK SRWLock,
                             IN OUT PRTLP_SRWLOCK_WAITBLOCK FirstWait  OPTIONAL,
                             IN OUT PRTLP_SRWLOCK_SHARED_WAKE WakeChain)
{
    PRTLP_SRWLOCK_SATISFIED Satisfied;
    ULONG SpinCount = RTLP_SRWLOCK_SPIN_COUNT;

    /* When we queued a wait block of our own the lock value tells us nothing
       about our own turn, so the wake node is the only thing to watch. */
    Satisfied = (FirstWait != NULL) ? RtlpSRWLockSharedWakeSatisfied
                                    : RtlpSRWLockSharedSatisfied;

    while (!Satisfied(SRWLock, (PVOID)(ULONG_PTR)WakeChain))
    {
        if (SpinCount != 0)
        {
            SpinCount--;
            YieldProcessor();
            continue;
        }

        /* Spinning did not get us the lock, so stop competing for the
           processor that the current owner needs in order to release it. */
        RtlpSRWLockPark(&WakeChain->WaitState,
                        (PVOID)(ULONG_PTR)WakeChain,
                        Satisfied,
                        SRWLock,
                        (PVOID)(ULONG_PTR)WakeChain);
    }
}


VOID
NTAPI
RtlInitializeSRWLock(OUT PRTL_SRWLOCK SRWLock)
{
    SRWLock->Ptr = NULL;
}


VOID
NTAPI
RtlAcquireSRWLockShared(IN OUT PRTL_SRWLOCK SRWLock)
{
    __ALIGNED(16) RTLP_SRWLOCK_WAITBLOCK StackWaitBlock;
    RTLP_SRWLOCK_SHARED_WAKE SharedWake;
    LONG_PTR CurrentValue, NewValue;
    PRTLP_SRWLOCK_WAITBLOCK First, Shared, FirstWait;

    while (1)
    {
        CurrentValue = *(volatile LONG_PTR *)&SRWLock->Ptr;

        if (CurrentValue & RTL_SRWLOCK_SHARED)
        {
            /* NOTE: It is possible that the RTL_SRWLOCK_OWNED bit is set! */

            if (CurrentValue & RTL_SRWLOCK_CONTENDED)
            {
                /* There's other waiters already, lock the wait blocks and
                   increment the shared count */
                First = RtlpAcquireWaitBlockLock(SRWLock);
                if (First != NULL)
                {
                    FirstWait = NULL;

                    if (First->Exclusive)
                    {
                        /* We need to setup a new wait block! Although
                           we're currently in a shared lock and we're acquiring
                           a shared lock, there are exclusive locks queued. We need
                           to wait until those are released. */
                        Shared = First->Last;

                        if (Shared->Exclusive)
                        {
                            StackWaitBlock.Exclusive = FALSE;
                            StackWaitBlock.SharedCount = 1;
                            StackWaitBlock.Next = NULL;
                            StackWaitBlock.Last = &StackWaitBlock;
                            StackWaitBlock.SharedWakeChain = &SharedWake;

                            Shared->Next = &StackWaitBlock;
                            First->Last = &StackWaitBlock;

                            Shared = &StackWaitBlock;
                            FirstWait = &StackWaitBlock;
                        }
                        else
                        {
                            Shared->LastSharedWake->Next = &SharedWake;
                            Shared->SharedCount++;
                        }
                    }
                    else
                    {
                        Shared = First;
                        Shared->LastSharedWake->Next = &SharedWake;
                        Shared->SharedCount++;
                    }

                    SharedWake.Next = NULL;
                    SharedWake.Wake = 0;
                    SharedWake.WaitState = RTLP_SRWLOCK_WAIT_SPINNING;

                    Shared->LastSharedWake = &SharedWake;

                    RtlpReleaseWaitBlockLock(SRWLock);

                    RtlpAcquireSRWLockSharedWait(SRWLock,
                                                 FirstWait,
                                                 &SharedWake);

                    /* Successfully incremented the shared count, we acquired the lock */
                    break;
                }
            }
            else
            {
                /* This is a fastest path, just increment the number of
                   current shared locks */

                /* Since the RTL_SRWLOCK_SHARED bit is set, the RTL_SRWLOCK_OWNED bit also has
                   to be set! */

                ASSERT(CurrentValue & RTL_SRWLOCK_OWNED);

                NewValue = (CurrentValue >> RTL_SRWLOCK_BITS) + 1;
                NewValue = (NewValue << RTL_SRWLOCK_BITS) | (CurrentValue & RTL_SRWLOCK_MASK);

                if ((LONG_PTR)InterlockedCompareExchangePointer(&SRWLock->Ptr,
                                                                (PVOID)NewValue,
                                                                (PVOID)CurrentValue) == CurrentValue)
                {
                    /* Successfully incremented the shared count, we acquired the lock */
                    break;
                }
            }
        }
        else
        {
            if (CurrentValue & RTL_SRWLOCK_OWNED)
            {
                /* The resource is currently acquired exclusively */
                if (CurrentValue & RTL_SRWLOCK_CONTENDED)
                {
                    SharedWake.Next = NULL;
                    SharedWake.Wake = 0;
                    SharedWake.WaitState = RTLP_SRWLOCK_WAIT_SPINNING;

                    /* There's other waiters already, lock the wait blocks and
                       increment the shared count. If the last block in the chain
                       is an exclusive lock, add another block. */

                    StackWaitBlock.Exclusive = FALSE;
                    StackWaitBlock.SharedCount = 0;
                    StackWaitBlock.Next = NULL;
                    StackWaitBlock.Last = &StackWaitBlock;
                    StackWaitBlock.SharedWakeChain = &SharedWake;

                    First = RtlpAcquireWaitBlockLock(SRWLock);
                    if (First != NULL)
                    {
                        Shared = First->Last;
                        if (Shared->Exclusive)
                        {
                            Shared->Next = &StackWaitBlock;
                            First->Last = &StackWaitBlock;

                            Shared = &StackWaitBlock;
                            FirstWait = &StackWaitBlock;
                        }
                        else
                        {
                            FirstWait = NULL;
                            Shared->LastSharedWake->Next = &SharedWake;
                        }

                        Shared->SharedCount++;
                        Shared->LastSharedWake = &SharedWake;

                        RtlpReleaseWaitBlockLock(SRWLock);

                        RtlpAcquireSRWLockSharedWait(SRWLock,
                                                     FirstWait,
                                                     &SharedWake);

                        /* Successfully incremented the shared count, we acquired the lock */
                        break;
                    }
                }
                else
                {
                    SharedWake.Next = NULL;
                    SharedWake.Wake = 0;
                    SharedWake.WaitState = RTLP_SRWLOCK_WAIT_SPINNING;

                    /* We need to setup the first wait block. Currently an exclusive lock is
                       held, change the lock to contended mode. */
                    StackWaitBlock.Exclusive = FALSE;
                    StackWaitBlock.SharedCount = 1;
                    StackWaitBlock.Next = NULL;
                    StackWaitBlock.Last = &StackWaitBlock;
                    StackWaitBlock.SharedWakeChain = &SharedWake;
                    StackWaitBlock.LastSharedWake = &SharedWake;

                    NewValue = (ULONG_PTR)&StackWaitBlock | RTL_SRWLOCK_OWNED | RTL_SRWLOCK_CONTENDED;
                    if ((LONG_PTR)InterlockedCompareExchangePointer(&SRWLock->Ptr,
                                                                    (PVOID)NewValue,
                                                                    (PVOID)CurrentValue) == CurrentValue)
                    {
                        RtlpAcquireSRWLockSharedWait(SRWLock,
                                                     &StackWaitBlock,
                                                     &SharedWake);

                        /* Successfully set the shared count, we acquired the lock */
                        break;
                    }
                }
            }
            else
            {
                /* This is a fast path, we can simply try to set the shared count to 1 */
                NewValue = (1 << RTL_SRWLOCK_BITS) | RTL_SRWLOCK_SHARED | RTL_SRWLOCK_OWNED;

                /* The RTL_SRWLOCK_CONTENDED bit should never be set if neither the
                   RTL_SRWLOCK_SHARED nor the RTL_SRWLOCK_OWNED bit is set */
                ASSERT(!(CurrentValue & RTL_SRWLOCK_CONTENDED));

                if ((LONG_PTR)InterlockedCompareExchangePointer(&SRWLock->Ptr,
                                                                (PVOID)NewValue,
                                                                (PVOID)CurrentValue) == CurrentValue)
                {
                    /* Successfully set the shared count, we acquired the lock */
                    break;
                }
            }
        }

        YieldProcessor();
    }
}


VOID
NTAPI
RtlReleaseSRWLockShared(IN OUT PRTL_SRWLOCK SRWLock)
{
    LONG_PTR CurrentValue, NewValue;
    PRTLP_SRWLOCK_WAITBLOCK WaitBlock;
    BOOLEAN LastShared;

    while (1)
    {
        CurrentValue = *(volatile LONG_PTR *)&SRWLock->Ptr;

        if (CurrentValue & RTL_SRWLOCK_SHARED)
        {
            if (CurrentValue & RTL_SRWLOCK_CONTENDED)
            {
                /* There's a wait block, we need to wake a pending
                   exclusive acquirer if this is the last shared release */
                WaitBlock = RtlpAcquireWaitBlockLock(SRWLock);
                if (WaitBlock != NULL)
                {
                    LastShared = (--WaitBlock->SharedCount == 0);

                    if (LastShared)
                        RtlpReleaseWaitBlockLockLastShared(SRWLock,
                                                           WaitBlock);
                    else
                        RtlpReleaseWaitBlockLock(SRWLock);

                    /* We released the lock */
                    break;
                }
            }
            else
            {
                /* This is a fast path, we can simply decrement the shared
                   count and store the pointer */
                NewValue = CurrentValue >> RTL_SRWLOCK_BITS;

                if (--NewValue != 0)
                {
                    NewValue = (NewValue << RTL_SRWLOCK_BITS) | RTL_SRWLOCK_SHARED | RTL_SRWLOCK_OWNED;
                }

                if ((LONG_PTR)InterlockedCompareExchangePointer(&SRWLock->Ptr,
                                                                (PVOID)NewValue,
                                                                (PVOID)CurrentValue) == CurrentValue)
                {
                    /* Successfully released the lock */
                    break;
                }
            }
        }
        else
        {
            /* The RTL_SRWLOCK_SHARED bit has to be present now,
               even in the contended case! */
            RtlRaiseStatus(STATUS_RESOURCE_NOT_OWNED);
        }

        YieldProcessor();
    }
}


VOID
NTAPI
RtlAcquireSRWLockExclusive(IN OUT PRTL_SRWLOCK SRWLock)
{
    __ALIGNED(16) RTLP_SRWLOCK_WAITBLOCK StackWaitBlock;
    PRTLP_SRWLOCK_WAITBLOCK First, Last;

    if (InterlockedBitTestAndSetPointer(&SRWLock->Ptr,
                                        RTL_SRWLOCK_OWNED_BIT))
    {
        LONG_PTR CurrentValue, NewValue;

        while (1)
        {
            CurrentValue = *(volatile LONG_PTR *)&SRWLock->Ptr;

            if (CurrentValue & RTL_SRWLOCK_SHARED)
            {
                /* A shared lock is being held right now. We need to add a wait block! */

                if (CurrentValue & RTL_SRWLOCK_CONTENDED)
                {
                    goto AddWaitBlock;
                }
                else
                {
                    /* There are no wait blocks so far, we need to add ourselves as the first
                       wait block. We need to keep the shared count! */
                    StackWaitBlock.Exclusive = TRUE;
                    StackWaitBlock.SharedCount = (LONG)(CurrentValue >> RTL_SRWLOCK_BITS);
                    StackWaitBlock.Next = NULL;
                    StackWaitBlock.Last = &StackWaitBlock;
                    StackWaitBlock.Wake = 0;
                    StackWaitBlock.WaitState = RTLP_SRWLOCK_WAIT_SPINNING;

                    NewValue = (ULONG_PTR)&StackWaitBlock | RTL_SRWLOCK_SHARED | RTL_SRWLOCK_CONTENDED | RTL_SRWLOCK_OWNED;

                    if ((LONG_PTR)InterlockedCompareExchangePointer(&SRWLock->Ptr,
                                                                    (PVOID)NewValue,
                                                                    (PVOID)CurrentValue) == CurrentValue)
                    {
                        RtlpAcquireSRWLockExclusiveWait(SRWLock,
                                                        &StackWaitBlock);

                        /* Successfully acquired the exclusive lock */
                        break;
                    }
                }
            }
            else
            {
                if (CurrentValue & RTL_SRWLOCK_OWNED)
                {
                    /* An exclusive lock is being held right now. We need to add a wait block! */

                    if (CurrentValue & RTL_SRWLOCK_CONTENDED)
                    {
AddWaitBlock:
                        StackWaitBlock.Exclusive = TRUE;
                        StackWaitBlock.SharedCount = 0;
                        StackWaitBlock.Next = NULL;
                        StackWaitBlock.Last = &StackWaitBlock;
                        StackWaitBlock.Wake = 0;
                        StackWaitBlock.WaitState = RTLP_SRWLOCK_WAIT_SPINNING;

                        First = RtlpAcquireWaitBlockLock(SRWLock);
                        if (First != NULL)
                        {
                            Last = First->Last;
                            Last->Next = &StackWaitBlock;
                            First->Last = &StackWaitBlock;

                            RtlpReleaseWaitBlockLock(SRWLock);

                            RtlpAcquireSRWLockExclusiveWait(SRWLock,
                                                            &StackWaitBlock);

                            /* Successfully acquired the exclusive lock */
                            break;
                        }
                    }
                    else
                    {
                        /* There are no wait blocks so far, we need to add ourselves as the first
                           wait block. We need to keep the shared count! */
                        StackWaitBlock.Exclusive = TRUE;
                        StackWaitBlock.SharedCount = 0;
                        StackWaitBlock.Next = NULL;
                        StackWaitBlock.Last = &StackWaitBlock;
                        StackWaitBlock.Wake = 0;
                        StackWaitBlock.WaitState = RTLP_SRWLOCK_WAIT_SPINNING;

                        NewValue = (ULONG_PTR)&StackWaitBlock | RTL_SRWLOCK_OWNED | RTL_SRWLOCK_CONTENDED;
                        if ((LONG_PTR)InterlockedCompareExchangePointer(&SRWLock->Ptr,
                                                                        (PVOID)NewValue,
                                                                        (PVOID)CurrentValue) == CurrentValue)
                        {
                            RtlpAcquireSRWLockExclusiveWait(SRWLock,
                                                            &StackWaitBlock);

                            /* Successfully acquired the exclusive lock */
                            break;
                        }
                    }
                }
                else
                {
                    if (!InterlockedBitTestAndSetPointer(&SRWLock->Ptr,
                                                         RTL_SRWLOCK_OWNED_BIT))
                    {
                        /* We managed to get hold of a simple exclusive lock! */
                        break;
                    }
                }
            }

            YieldProcessor();
        }
    }
}


VOID
NTAPI
RtlReleaseSRWLockExclusive(IN OUT PRTL_SRWLOCK SRWLock)
{
    LONG_PTR CurrentValue, NewValue;
    PRTLP_SRWLOCK_WAITBLOCK WaitBlock;

    while (1)
    {
        CurrentValue = *(volatile LONG_PTR *)&SRWLock->Ptr;

        if (!(CurrentValue & RTL_SRWLOCK_OWNED))
        {
            RtlRaiseStatus(STATUS_RESOURCE_NOT_OWNED);
        }

        if (!(CurrentValue & RTL_SRWLOCK_SHARED))
        {
            if (CurrentValue & RTL_SRWLOCK_CONTENDED)
            {
                /* There's a wait block, we need to wake the next pending
                   acquirer (exclusive or shared) */
                WaitBlock = RtlpAcquireWaitBlockLock(SRWLock);
                if (WaitBlock != NULL)
                {
                    RtlpReleaseWaitBlockLockExclusive(SRWLock,
                                                      WaitBlock);

                    /* We released the lock */
                    break;
                }
            }
            else
            {
                /* This is a fast path, we can simply clear the RTL_SRWLOCK_OWNED
                   bit. All other bits should be 0 now because this is a simple
                   exclusive lock and no one is waiting. */

                ASSERT(!(CurrentValue & ~RTL_SRWLOCK_OWNED));

                NewValue = 0;
                if ((LONG_PTR)InterlockedCompareExchangePointer(&SRWLock->Ptr,
                                                                (PVOID)NewValue,
                                                                (PVOID)CurrentValue) == CurrentValue)
                {
                    /* We released the lock */
                    break;
                }
            }
        }
        else
        {
            /* The RTL_SRWLOCK_SHARED bit must not be present now,
               not even in the contended case! */
            RtlRaiseStatus(STATUS_RESOURCE_NOT_OWNED);
        }

        YieldProcessor();
    }
}

BOOLEAN
NTAPI
RtlTryAcquireSRWLockShared(PRTL_SRWLOCK SRWLock)
{

    LONG_PTR CompareValue, NewValue, GotValue;

    do
    {
        CompareValue = *(volatile LONG_PTR *)&SRWLock->Ptr;
        NewValue = ((CompareValue >> RTL_SRWLOCK_BITS) + 1) | RTL_SRWLOCK_SHARED | RTL_SRWLOCK_OWNED;

        /* Only increment shared count if there is no waiter */
        CompareValue &= ~RTL_SRWLOCK_MASK | RTL_SRWLOCK_SHARED | RTL_SRWLOCK_OWNED;
    } while (
        ((GotValue = (LONG_PTR)InterlockedCompareExchangePointer(&SRWLock->Ptr, (LONG_PTR*)NewValue, (LONG_PTR*)CompareValue)) != CompareValue)
        && (((GotValue & RTL_SRWLOCK_MASK) == (RTL_SRWLOCK_SHARED | RTL_SRWLOCK_OWNED)) || (GotValue == 0)));

    return ((GotValue & RTL_SRWLOCK_MASK) == (RTL_SRWLOCK_SHARED | RTL_SRWLOCK_OWNED)) || (GotValue == 0);
}

BOOLEAN
NTAPI
RtlTryAcquireSRWLockExclusive(PRTL_SRWLOCK SRWLock)
{
    return InterlockedCompareExchangePointer(&SRWLock->Ptr, (ULONG_PTR*)RTL_SRWLOCK_OWNED, 0) == 0;
}
