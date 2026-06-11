/*
 * PROJECT:         ReactOS Kernel (ARM64)
 * PURPOSE:         Spin lock support for ARM64
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#undef KeAcquireSpinLock
#undef KeReleaseSpinLock

extern BOOLEAN ExpArm64PoolBootstrapMode;
extern volatile LONG MiArm64PfnLockDepth[MAXIMUM_PROCESSORS];

KIRQL
FASTCALL
KfAcquireSpinLock(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    OldIrql = KfRaiseIrql(DISPATCH_LEVEL);
    KxAcquireSpinLock(SpinLock);
    return OldIrql;
}

VOID
FASTCALL
KfReleaseSpinLock(
    _Inout_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    KxReleaseSpinLock(SpinLock);

    KfLowerIrql(OldIrql);
}

VOID
NTAPI
KeAcquireSpinLock(
    _Inout_ PKSPIN_LOCK SpinLock,
    _Out_ PKIRQL OldIrql)
{
    *OldIrql = KfAcquireSpinLock(SpinLock);
}

VOID
NTAPI
KeReleaseSpinLock(
    _Inout_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    KfReleaseSpinLock(SpinLock, OldIrql);
}

KIRQL
FASTCALL
KeAcquireSpinLockRaiseToDpc(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    KxAcquireSpinLock(SpinLock);
    return OldIrql;
}

KIRQL
FASTCALL
KeAcquireSpinLockRaiseToSynch(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(SYNCH_LEVEL, &OldIrql);
    KxAcquireSpinLock(SpinLock);
    return OldIrql;
}
static
VOID
KiArm64TrackQueuedLockAcquire(
    _In_ KSPIN_LOCK_QUEUE_NUMBER LockNumber)
{
    if (LockNumber == LockQueuePfnLock)
    {
        ULONG CpuIndex = KeGetCurrentProcessorNumber();
        if (CpuIndex < MAXIMUM_PROCESSORS)
        {
            InterlockedIncrement(&MiArm64PfnLockDepth[CpuIndex]);
        }
    }
}

KIRQL
FASTCALL
KeAcquireQueuedSpinLock(
    _In_ KSPIN_LOCK_QUEUE_NUMBER LockNumber)
{
    KIRQL OldIrql;
    PKPRCB Prcb;
    PKSPIN_LOCK Lock;

    if (ExpArm64PoolBootstrapMode && LockNumber == LockQueueMmNonPagedPoolLock)
    {
        return PASSIVE_LEVEL;
    }

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    Prcb = KeGetCurrentPrcb();
    if (Prcb == NULL)
    {
        KeBugCheckEx(SPIN_LOCK_INIT_FAILURE, 1, LockNumber, 0, 0);
    }

    Lock = Prcb->LockQueue[LockNumber].Lock;
    if (Lock == NULL)
    {
        KeBugCheckEx(SPIN_LOCK_INIT_FAILURE, 2, LockNumber, (ULONG_PTR)Prcb, (ULONG_PTR)KeGetPcr());
    }

    KxAcquireSpinLock(Lock);
    KiArm64TrackQueuedLockAcquire(LockNumber);
    return OldIrql;
}

KIRQL
FASTCALL
KeAcquireQueuedSpinLockRaiseToSynch(
    _In_ KSPIN_LOCK_QUEUE_NUMBER LockNumber)
{
    KIRQL OldIrql;

    KeRaiseIrql(SYNCH_LEVEL, &OldIrql);
    KxAcquireSpinLock(KeGetCurrentPrcb()->LockQueue[LockNumber].Lock);
    KiArm64TrackQueuedLockAcquire(LockNumber);
    return OldIrql;
}

VOID
FASTCALL
KeReleaseQueuedSpinLock(
    _In_ KSPIN_LOCK_QUEUE_NUMBER LockNumber,
    _In_ KIRQL OldIrql)
{
    PKPRCB Prcb;
    PKSPIN_LOCK Lock;

    if (ExpArm64PoolBootstrapMode && LockNumber == LockQueueMmNonPagedPoolLock)
    {
        return;
    }

    Prcb = KeGetCurrentPrcb();
    if (Prcb == NULL)
    {
        KeBugCheckEx(SPIN_LOCK_INIT_FAILURE, 3, LockNumber, 0, 0);
    }

    Lock = Prcb->LockQueue[LockNumber].Lock;
    if (Lock == NULL)
    {
        KeBugCheckEx(SPIN_LOCK_INIT_FAILURE, 4, LockNumber, (ULONG_PTR)Prcb, (ULONG_PTR)KeGetPcr());
    }

    KxReleaseSpinLock(Lock);
    if (LockNumber == LockQueuePfnLock)
    {
        ULONG CpuIndex = KeGetCurrentProcessorNumber();
        if (CpuIndex < MAXIMUM_PROCESSORS)
        {
            InterlockedDecrement(&MiArm64PfnLockDepth[CpuIndex]);
        }
    }
    KeLowerIrql(OldIrql);
}

VOID
FASTCALL
KeAcquireInStackQueuedSpinLock(
    _Inout_ PKSPIN_LOCK SpinLock,
    _Inout_ PKLOCK_QUEUE_HANDLE LockHandle)
{
    LockHandle->LockQueue.Next = NULL;
    LockHandle->LockQueue.Lock = SpinLock;

    KeRaiseIrql(DISPATCH_LEVEL, &LockHandle->OldIrql);
    KxAcquireSpinLock(LockHandle->LockQueue.Lock);
}

VOID
FASTCALL
KeAcquireInStackQueuedSpinLockRaiseToSynch(
    _Inout_ PKSPIN_LOCK SpinLock,
    _Inout_ PKLOCK_QUEUE_HANDLE LockHandle)
{
    LockHandle->LockQueue.Next = NULL;
    LockHandle->LockQueue.Lock = SpinLock;

    KeRaiseIrql(SYNCH_LEVEL, &LockHandle->OldIrql);
    KxAcquireSpinLock(LockHandle->LockQueue.Lock);
}

VOID
FASTCALL
KeReleaseInStackQueuedSpinLock(
    _Inout_ PKLOCK_QUEUE_HANDLE LockHandle)
{
    KxReleaseSpinLock(LockHandle->LockQueue.Lock);
    KeLowerIrql(LockHandle->OldIrql);
}

LOGICAL
FASTCALL
KeTryToAcquireQueuedSpinLock(
    _In_ KSPIN_LOCK_QUEUE_NUMBER LockNumber,
    _Out_ PKIRQL OldIrql)
{
    /* This raises IRQL even if locking fails */
    KeRaiseIrql(DISPATCH_LEVEL, OldIrql);

    /* KeTryToAcquireSpinLockAtDpcLevel performs the DBG ownership marking
       that KxReleaseSpinLock verifies, on UP builds too */
    if (!KeTryToAcquireSpinLockAtDpcLevel(
            KeGetCurrentPrcb()->LockQueue[LockNumber].Lock))
    {
        return FALSE;
    }

    KiArm64TrackQueuedLockAcquire(LockNumber);
    return TRUE;
}

BOOLEAN
FASTCALL
KeTryToAcquireQueuedSpinLockRaiseToSynch(
    _In_ KSPIN_LOCK_QUEUE_NUMBER LockNumber,
    _Out_ PKIRQL OldIrql)
{
    /* This raises IRQL even if locking fails */
    KeRaiseIrql(SYNCH_LEVEL, OldIrql);

    /* KeTryToAcquireSpinLockAtDpcLevel performs the DBG ownership marking
       that KxReleaseSpinLock verifies, on UP builds too */
    if (!KeTryToAcquireSpinLockAtDpcLevel(
            KeGetCurrentPrcb()->LockQueue[LockNumber].Lock))
    {
        return FALSE;
    }

    KiArm64TrackQueuedLockAcquire(LockNumber);
    return TRUE;
}
