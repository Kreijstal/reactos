/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/arm64/ipi.c
 * PURPOSE:         Inter-processor interrupts for ARM64
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*
 * All IPIs travel on one SGI (HAL_ARM64_SGI_IPI, connected at IPI_LEVEL).
 * What the target is asked to do is carried in its PRCB, as on NT:
 *
 *  - RequestSummary: IPI_APC / IPI_DPC request bits, set by KiIpiSend.
 *  - SignalDone:     the PRCB of a processor that sent this one a packet.
 *                    A sender claims the slot with a compare-exchange and the
 *                    target releases it once it has picked the packet up.
 *  - IpiFrozen:      the freeze state machine (freeze.c), a state value and
 *                    never a set of request bits, so freezing and thawing
 *                    cannot erase a pending APC/DPC request.
 *
 * A packet lives in the sender's KiIpiPacket slot. The sender's TargetSet
 * holds the targets that have not finished it yet; each target clears its own
 * bit in KiIpiSignalPacketDone and the sender may not reuse the slot until
 * TargetSet is empty.
 */

extern KSPIN_LOCK KiReverseStallIpiLock;

/* The level NT synchronizes at on MP, CLOCK_LEVEL - 1 */
#define KI_IPI_SYNCH_LEVEL (IPI_LEVEL - 2)

#ifdef CONFIG_SMP
typedef struct _KI_ARM64_IPI_PACKET
{
    PKIPI_WORKER Worker;
    PVOID Parameter1;
    PVOID Parameter2;
    PVOID Parameter3;
} KI_ARM64_IPI_PACKET, *PKI_ARM64_IPI_PACKET;

static KI_ARM64_IPI_PACKET KiIpiPacket[MAXIMUM_PROCESSORS];

static
VOID
KiIpiProcessRequests(
    _In_ PKPRCB Prcb,
    _In_opt_ PKTRAP_FRAME TrapFrame,
    _In_opt_ PKEXCEPTION_FRAME ExceptionFrame)
{
    ULONG64 RequestSummary;
    PKPRCB Sender;

    ASSERT(KeGetCurrentIrql() >= IPI_LEVEL);

    /* A freeze request first: the debugger owns the machine */
    if (Prcb->IpiFrozen == IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        KiProcessorFreezeHandler(TrapFrame, ExceptionFrame);
    }

    /* Software interrupt requests */
    RequestSummary = (ULONG64)InterlockedExchange64((volatile LONG64 *)&Prcb->RequestSummary, 0);
    ASSERT((RequestSummary & ~(ULONG64)(IPI_APC | IPI_DPC)) == 0);

    if (RequestSummary & IPI_APC)
    {
        HalRequestSoftwareInterrupt(APC_LEVEL);
    }

    if (RequestSummary & IPI_DPC)
    {
        Prcb->DpcInterruptRequested = TRUE;
        HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }

    /* A packet: copy it out, free our slot for the next sender, run it */
    Sender = (PKPRCB)InterlockedExchangePointer((PVOID *)&Prcb->SignalDone, NULL);
    if (Sender != NULL)
    {
        PKI_ARM64_IPI_PACKET Packet = &KiIpiPacket[Sender->Number];
        PKIPI_WORKER Worker = Packet->Worker;
        PVOID Parameter1 = Packet->Parameter1;
        PVOID Parameter2 = Packet->Parameter2;
        PVOID Parameter3 = Packet->Parameter3;

        ASSERT(Sender != Prcb);
        ASSERT(Sender->TargetSet & Prcb->SetMember);

        /* The worker signals completion through KiIpiSignalPacketDone */
        Worker((PKIPI_CONTEXT)Sender, Parameter1, Parameter2, Parameter3);
    }
}

/*
 * Called by a processor that spins waiting for other processors. Service
 * what would otherwise wait for our own IPI: a packet sent to us (its sender
 * may be the processor we are waiting for) and a freeze request, whose SGI
 * we cannot take while we spin at or above IPI_LEVEL.
 */
static
VOID
KiIpiPollRequests(
    _In_ PKPRCB Prcb)
{
    KIRQL OldIrql;

    if ((Prcb->SignalDone == NULL) &&
        (Prcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE))
    {
        return;
    }

    if (KeGetCurrentIrql() < IPI_LEVEL)
    {
        KeRaiseIrql(IPI_LEVEL, &OldIrql);
        KiIpiProcessRequests(Prcb, NULL, NULL);
        KeLowerIrql(OldIrql);
    }
    else if (Prcb->IpiFrozen == IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        /*
         * Already at IPI_LEVEL or above, inside a packet or a generic call:
         * only the freeze can be answered here. There is never a second
         * packet for us, KeIpiGenericCall serializes all senders.
         */
        KiPollFreezeExecution(Prcb);
    }
}
#endif

VOID
NTAPI
KiIpiGenericCallTarget(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID BroadcastFunction,
    _In_ PVOID Argument,
    _In_ PVOID Count)
{
#ifdef CONFIG_SMP
    PKPRCB Prcb = KeGetCurrentPrcb();

    ASSERT(KeGetCurrentIrql() == IPI_LEVEL);

    /* Check in, then wait until the initiator releases every processor */
    InterlockedDecrement((volatile LONG *)Count);
    while (InterlockedCompareExchange((volatile LONG *)Count, 0, 0) != 0)
    {
        KiIpiPollRequests(Prcb);
        YieldProcessor();
    }

    ((PKIPI_BROADCAST_WORKER)BroadcastFunction)((ULONG_PTR)Argument);
    KiIpiSignalPacketDone(PacketContext);
#else
    UNREFERENCED_PARAMETER(PacketContext);
    UNREFERENCED_PARAMETER(BroadcastFunction);
    UNREFERENCED_PARAMETER(Argument);
    UNREFERENCED_PARAMETER(Count);
    ASSERT(FALSE);
#endif
}

VOID
FASTCALL
KiIpiSend(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
#ifdef CONFIG_SMP
    KAFFINITY Remaining = TargetSet;
    ULONG Processor;

    /* Freezes go through freeze.c, packets through KiIpiSendPacket */
    ASSERT((IpiRequest == IPI_APC) || (IpiRequest == IPI_DPC));

    while (BitScanForwardAffinity(&Processor, Remaining))
    {
        Remaining &= ~AFFINITY_MASK(Processor);
        ASSERT(KiProcessorBlock[Processor] != NULL);
        InterlockedOr64((volatile LONG64 *)&KiProcessorBlock[Processor]->RequestSummary,
                        IpiRequest);
    }

    /* The HAL orders the request stores before the SGI */
    HalRequestIpi(TargetSet);
#else
    UNREFERENCED_PARAMETER(TargetSet);
    UNREFERENCED_PARAMETER(IpiRequest);
#endif
}

VOID
NTAPI
KiIpiSendPacket(
    _In_ KAFFINITY TargetSet,
    _In_ PKIPI_WORKER WorkerFunction,
    _In_ PKIPI_BROADCAST_WORKER BroadcastFunction,
    _In_ ULONG_PTR Context,
    _Inout_ PULONG Count)
{
#ifdef CONFIG_SMP
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKI_ARM64_IPI_PACKET Packet = &KiIpiPacket[Prcb->Number];
    KAFFINITY Remaining = TargetSet;
    ULONG Processor;

    ASSERT(KeGetCurrentIrql() >= KI_IPI_SYNCH_LEVEL);
    ASSERT(TargetSet != 0);
    ASSERT((TargetSet & Prcb->SetMember) == 0);
    ASSERT((TargetSet & ~KeActiveProcessors) == 0);

    /* Our previous packet must be finished before the slot is reused */
    ASSERT(Prcb->TargetSet == 0);

    Packet->Worker = WorkerFunction;
    Packet->Parameter1 = (PVOID)BroadcastFunction;
    Packet->Parameter2 = (PVOID)Context;
    Packet->Parameter3 = (PVOID)Count;
    InterlockedExchange64((volatile LONG64 *)&Prcb->TargetSet, (LONG64)TargetSet);

    /* Claim every target's packet slot; the interlocked store publishes the packet */
    while (BitScanForwardAffinity(&Processor, Remaining))
    {
        PKPRCB TargetPrcb = KiProcessorBlock[Processor];

        Remaining &= ~AFFINITY_MASK(Processor);
        while (InterlockedCompareExchangePointer((PVOID *)&TargetPrcb->SignalDone,
                                                 Prcb,
                                                 NULL) != NULL)
        {
            /* Its current sender may be waiting for us */
            KiIpiPollRequests(Prcb);
            YieldProcessor();
        }
    }

    HalRequestIpi(TargetSet);
#else
    UNREFERENCED_PARAMETER(TargetSet);
    UNREFERENCED_PARAMETER(WorkerFunction);
    UNREFERENCED_PARAMETER(BroadcastFunction);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Count);
    ASSERT(FALSE);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDone(
    _In_ PKIPI_CONTEXT PacketContext)
{
#ifdef CONFIG_SMP
    PKPRCB Sender = (PKPRCB)PacketContext;
    KAFFINITY SetMember = KeGetCurrentPrcb()->SetMember;

    ASSERT(Sender->TargetSet & SetMember);
    InterlockedAnd64((volatile LONG64 *)&Sender->TargetSet, ~(LONG64)SetMember);
#else
    UNREFERENCED_PARAMETER(PacketContext);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDoneAndStall(
    _In_ PKIPI_CONTEXT PacketContext,
    _Inout_ volatile PULONG ReverseStall)
{
#ifdef CONFIG_SMP
    PKPRCB Prcb = KeGetCurrentPrcb();
    ULONG StallValue;

    /* Sample before signalling: the sender may change it right after */
    StallValue = *ReverseStall;
    KiIpiSignalPacketDone(PacketContext);

    while (*ReverseStall == StallValue)
    {
        KiIpiPollRequests(Prcb);
        YieldProcessor();
    }
#else
    UNREFERENCED_PARAMETER(PacketContext);
    UNREFERENCED_PARAMETER(ReverseStall);
#endif
}

BOOLEAN
NTAPI
KiIpiServiceRoutine(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
#ifdef CONFIG_SMP
    KiIpiProcessRequests(KeGetCurrentPrcb(), TrapFrame, ExceptionFrame);
#else
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);
#endif
    return TRUE;
}

/*
 * @implemented
 *
 * Run Function on every active processor at IPI_LEVEL, all of them at once:
 * the targets check in and wait until the initiator releases them together
 * with itself, and the initiator returns only after every target is done.
 */
ULONG_PTR
NTAPI
KeIpiGenericCall(
    _In_ PKIPI_BROADCAST_WORKER Function,
    _In_ ULONG_PTR Argument)
{
    ULONG_PTR Status;
    KIRQL OldIrql, OldIrql2;
#ifdef CONFIG_SMP
    PKPRCB Prcb;
    KAFFINITY TargetSet;
    volatile LONG Count;
#endif

    /*
     * Raise to the NT synchronization level, IPI_LEVEL - 2, above every device
     * interrupt. SYNCH_LEVEL is still DISPATCH_LEVEL on this port (#173), and
     * an initiator waiting there takes device interrupts: one whose interrupt
     * lock is held by a target -- which waits at IPI_LEVEL for this initiator
     * to release it -- spins forever.
     */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < KI_IPI_SYNCH_LEVEL) KeRaiseIrql(KI_IPI_SYNCH_LEVEL, &OldIrql);

    /* Serialize all generic calls: one packet in flight */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

#ifdef CONFIG_SMP
    Prcb = KeGetCurrentPrcb();
    TargetSet = KeActiveProcessors & ~Prcb->SetMember;
    if (TargetSet != 0)
    {
        /* Every target checks in by decrementing; we are the last one */
        KAFFINITY Remaining = TargetSet;

        for (Count = 1; Remaining != 0; Remaining &= Remaining - 1) Count++;
        KiIpiSendPacket(TargetSet,
                        KiIpiGenericCallTarget,
                        Function,
                        Argument,
                        (PULONG)&Count);

        while (InterlockedCompareExchange(&Count, 0, 0) != 1)
        {
            KiIpiPollRequests(Prcb);
            YieldProcessor();
        }
    }
#endif

    KeRaiseIrql(IPI_LEVEL, &OldIrql2);

#ifdef CONFIG_SMP
    /* Release the targets */
    if (TargetSet != 0) InterlockedExchange(&Count, 0);
#endif

    Status = Function(Argument);

#ifdef CONFIG_SMP
    /* Count lives on our stack: wait until every target is done with it */
    if (TargetSet != 0)
    {
        ASSERT(Prcb == KeGetCurrentPrcb());
        while (InterlockedCompareExchange64((volatile LONG64 *)&Prcb->TargetSet, 0, 0) != 0)
        {
            KiIpiPollRequests(Prcb);
            YieldProcessor();
        }
    }
#endif

    KeLowerIrql(OldIrql2);
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);
    KeLowerIrql(OldIrql);
    return Status;
}
