/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Packet queues, rings and the datapath translation
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Each queue has a packet ring and a fragment ring. Elements in
 * [BeginIndex, EndIndex) belong to the client driver; the OS hands it more
 * by moving EndIndex, the client gives them back by moving BeginIndex.
 *
 * Transmit: NET_BUFFERs of the NET_BUFFER_LISTs NDIS sends become one
 * packet each, with one fragment per piece of MDL chain; the fragment's
 * virtual address extension points at the data. When the client returns a
 * packet its NET_BUFFER_LIST loses one outstanding NET_BUFFER; at zero it
 * is completed to NDIS.
 *
 * Receive: the OS owns one buffer per fragment ring element (system managed
 * buffers) and posts empty packets and fragments. Packets the client
 * returns with fragments attached are copied into NET_BUFFER_LISTs and
 * indicated to NDIS; NDIS returns them to NxRxReturnNetBufferLists.
 *
 * Each queue runs on its own system thread, which makes every callback of
 * the queue (EvtStart, EvtAdvance, EvtSetNotificationEnabled, EvtCancel,
 * EvtStop) and all ring updates on the OS side serial without locks:
 *
 *   EvtStart
 *   loop: post, EvtAdvance, drain; when nothing moved, arm the client
 *         notification, poll once more and sleep until notified
 *   EvtCancel, then EvtAdvance until the client gave everything back
 *   EvtStop
 */

#include "nxprivate.h"
#include "nxapi.h"

#define NDEBUG
#include <debug.h>

#define NX_TX_PACKETS           64
#define NX_TX_FRAGMENTS         256
#define NX_RX_ELEMENTS_SMALL    128
#define NX_RX_ELEMENTS_LARGE    32
#define NX_RX_SMALL_FRAME       4096
#define NX_RX_MAX_FRAME         65536
#define NX_RING_MAX_ELEMENTS    4096

/* Receive checksum bits of NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO */
#define NX_CSUM_TCP_FAILED      0x01
#define NX_CSUM_UDP_FAILED      0x02
#define NX_CSUM_IP_FAILED       0x04
#define NX_CSUM_TCP_SUCCEEDED   0x08
#define NX_CSUM_UDP_SUCCEEDED   0x10
#define NX_CSUM_IP_SUCCEEDED    0x20

/* Ethernet types, for raw IP adapters */
#define NX_ETHERTYPE_IPV4       0x0800
#define NX_ETHERTYPE_IPV6       0x86DD

/* NBL->MiniportReserved[0] of a send: NET_BUFFERs still on the ring */
#define NX_NBL_PENDING(_Nbl)    (*(LONG_PTR *)&(_Nbl)->MiniportReserved[0])

static
UINT32
NxRingElements(
    _In_ SIZE_T Hint,
    _In_ UINT32 Default)
{
    UINT32 Count = 8;

    if (Hint == 0)
        return Default;

    while (Count < Hint && Count < NX_RING_MAX_ELEMENTS)
        Count <<= 1;
    return Count;
}

/* NET_RING is cache aligned; pool blocks are not */
static
NET_RING *
NxRingAllocate(
    _In_ UINT32 Count,
    _In_ UINT16 Stride)
{
    SIZE_T Size = FIELD_OFFSET(NET_RING, Buffer) + (SIZE_T)Count * Stride;
    PUCHAR Block;
    NET_RING *Ring;

    Block = ExAllocatePoolZero(NonPagedPool, Size + SYSTEM_CACHE_ALIGNMENT_SIZE + sizeof(PVOID), NX_RING_TAG);
    if (Block == NULL)
        return NULL;

    Ring = (NET_RING *)ALIGN_UP_POINTER_BY(Block + sizeof(PVOID), SYSTEM_CACHE_ALIGNMENT_SIZE);
    ((PVOID *)Ring)[-1] = Block;

    Ring->ElementStride = Stride;
    Ring->NumberOfElements = Count;
    Ring->ElementIndexMask = Count - 1;
    return Ring;
}

static
VOID
NxRingFree(
    _In_opt_ NET_RING *Ring)
{
    if (Ring != NULL)
        ExFreePoolWithTag(((PVOID *)Ring)[-1], NX_RING_TAG);
}

static
VOID
NxQueueFreeResources(
    _In_ PNX_QUEUE Queue)
{
    NxRingFree(Queue->PacketRing);
    NxRingFree(Queue->FragmentRing);
    Queue->PacketRing = Queue->FragmentRing = NULL;

    if (Queue->FragmentVirtualAddress)
        ExFreePoolWithTag(Queue->FragmentVirtualAddress, NX_RING_TAG);
    if (Queue->PacketChecksum)
        ExFreePoolWithTag(Queue->PacketChecksum, NX_RING_TAG);
    if (Queue->PacketNbl)
        ExFreePoolWithTag(Queue->PacketNbl, NX_RING_TAG);
    if (Queue->RxBuffers)
        ExFreePoolWithTag(Queue->RxBuffers, NX_RX_TAG);

    Queue->FragmentVirtualAddress = NULL;
    Queue->PacketChecksum = NULL;
    Queue->PacketNbl = NULL;
    Queue->RxBuffers = NULL;
}

static EVT_WDF_OBJECT_CONTEXT_DESTROY NxEvtQueueDestroy;

static
VOID
NTAPI
NxEvtQueueDestroy(
    _In_ WDFOBJECT Object)
{
    /* After every cleanup callback of the client: nothing uses the rings */
    NxQueueFreeResources(NxQueueGetContext(Object));
}

static
NTSTATUS
NxQueueAllocateResources(
    _In_ PNX_QUEUE Queue)
{
    PNX_ADAPTER Adapter = Queue->Adapter;
    UINT32 Packets, Fragments;
    SIZE_T FrameSize;
    UINT32 i;

    if (Queue->Type == NxQueueTx)
    {
        Packets = NX_TX_PACKETS;
        Fragments = NxRingElements(Adapter->TxCapabilities.FragmentRingNumberOfElementsHint,
                                   NX_TX_FRAGMENTS);
        Queue->ChecksumEnabled = Adapter->TxChecksumCapabilitiesSet ||
                                 Adapter->ChecksumCapabilitiesSet;
    }
    else
    {
        if (Adapter->RxCapabilities.AllocationMode != NetRxFragmentBufferAllocationModeSystem ||
            Adapter->RxCapabilities.AttachmentMode != NetRxFragmentBufferAttachmentModeSystem)
        {
            DbgPrint("NetAdapterCx: driver managed receive buffers are not supported\n");
            return STATUS_NOT_SUPPORTED;
        }

        FrameSize = Adapter->RxCapabilities.MaximumFrameSize;
        if (FrameSize == 0)
            FrameSize = Adapter->MtuSize + 14;
        FrameSize = min(FrameSize, NX_RX_MAX_FRAME);

        Fragments = NxRingElements(Adapter->RxCapabilities.FragmentRingNumberOfElementsHint,
                                   FrameSize <= NX_RX_SMALL_FRAME ? NX_RX_ELEMENTS_SMALL :
                                                                    NX_RX_ELEMENTS_LARGE);
        Packets = Fragments;

        Queue->RxBufferSize = ALIGN_UP_BY(FrameSize, MEMORY_ALLOCATION_ALIGNMENT);
        Queue->RxBuffers = ExAllocatePoolWithTag(NonPagedPool,
                                                 Queue->RxBufferSize * Fragments,
                                                 NX_RX_TAG);
        if (Queue->RxBuffers == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Queue->ChecksumEnabled = Adapter->RxChecksumCapabilitiesSet ||
                                 Adapter->ChecksumCapabilitiesSet;
    }

    Queue->PacketRing = NxRingAllocate(Packets, (UINT16)ALIGN_UP_BY(sizeof(NET_PACKET), 8));
    Queue->FragmentRing = NxRingAllocate(Fragments, sizeof(NET_FRAGMENT));
    Queue->FragmentVirtualAddress = ExAllocatePoolZero(NonPagedPool,
                                                       Fragments * sizeof(NET_FRAGMENT_VIRTUAL_ADDRESS),
                                                       NX_RING_TAG);
    Queue->PacketChecksum = ExAllocatePoolZero(NonPagedPool,
                                               Packets * sizeof(NET_PACKET_CHECKSUM),
                                               NX_RING_TAG);
    if (Queue->Type == NxQueueTx)
    {
        Queue->PacketNbl = ExAllocatePoolZero(NonPagedPool,
                                              Packets * sizeof(PNET_BUFFER_LIST),
                                              NX_RING_TAG);
    }

    if (Queue->PacketRing == NULL || Queue->FragmentRing == NULL ||
        Queue->FragmentVirtualAddress == NULL || Queue->PacketChecksum == NULL ||
        (Queue->Type == NxQueueTx && Queue->PacketNbl == NULL))
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Receive buffers stay attached to their fragment element */
    if (Queue->Type == NxQueueRx)
    {
        for (i = 0; i < Fragments; i++)
        {
            Queue->FragmentVirtualAddress[i].VirtualAddress =
                Queue->RxBuffers + (SIZE_T)i * Queue->RxBufferSize;
        }
    }

    Queue->Rings.Rings[NetRingTypePacket] = Queue->PacketRing;
    Queue->Rings.Rings[NetRingTypeFragment] = Queue->FragmentRing;
    Queue->Rings.Rings[NetRingTypeDataBuffer] = NULL;
    return STATUS_SUCCESS;
}

static
PNX_QUEUE_INIT
NxQueueInitGet(
    _In_ PVOID QueueInit,
    _In_ NX_QUEUE_TYPE Type)
{
    PNX_QUEUE_INIT Init = (PNX_QUEUE_INIT)QueueInit;

    if (Init == NULL || Init->Signature != NX_QUEUE_INIT_SIGNATURE || Init->Type != Type)
        KeBugCheckEx(WDF_VIOLATION, 0x4E4358, (ULONG_PTR)QueueInit, 4, 0);
    return Init;
}

static
PNX_QUEUE
NxQueueGet(
    _In_ NETPACKETQUEUE Handle)
{
    PNX_QUEUE Queue = NxQueueGetContext(Handle);

    if (Queue == NULL)
        KeBugCheckEx(WDF_VIOLATION, 0x4E4358, (ULONG_PTR)Handle, 5, 0);
    return Queue;
}

static
NTSTATUS
NxQueueCreate(
    _In_ PNX_QUEUE_INIT Init,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *QueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG *Configuration,
    _Out_ NETPACKETQUEUE *PacketQueue)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES ClientAttributes;
    PNX_ADAPTER Adapter = Init->Adapter;
    PNX_QUEUE Queue;
    WDFOBJECT Object;
    NTSTATUS Status;

    *PacketQueue = NULL;

    if (Init->CreatedQueue != NULL)
        return STATUS_INVALID_DEVICE_STATE;

    if (Configuration == NULL || Configuration->EvtAdvance == NULL ||
        Configuration->EvtCancel == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, NX_QUEUE);
    Attributes.ParentObject = Adapter->Handle;
    Attributes.EvtDestroyCallback = NxEvtQueueDestroy;

    Status = WdfObjectCreate(&Attributes, &Object);
    if (!NT_SUCCESS(Status))
        return Status;

    Queue = NxQueueGetContext(Object);
    Queue->Type = Init->Type;
    Queue->QueueId = Init->QueueId;
    Queue->Handle = (NETPACKETQUEUE)Object;
    Queue->Adapter = Adapter;
    NX_COPY_SIZED(&Queue->Config, Configuration);
    KeInitializeSpinLock(&Queue->SendLock);
    KeInitializeEvent(&Queue->Notify, SynchronizationEvent, FALSE);

    Status = NxQueueAllocateResources(Queue);
    if (NT_SUCCESS(Status) && QueueAttributes != NULL &&
        QueueAttributes->ContextTypeInfo != NULL)
    {
        ClientAttributes = *QueueAttributes;
        ClientAttributes.ParentObject = NULL;
        ClientAttributes.ExecutionLevel = WdfExecutionLevelInheritFromParent;
        ClientAttributes.SynchronizationScope = WdfSynchronizationScopeInheritFromParent;
        Status = WdfObjectAllocateContext(Object, &ClientAttributes, NULL);
    }

    if (!NT_SUCCESS(Status))
    {
        DbgPrint("NetAdapterCx: %s queue creation failed 0x%lx\n",
                 Init->Type == NxQueueTx ? "transmit" : "receive", Status);
        WdfObjectDelete(Object);
        return Status;
    }

    if (Init->Type == NxQueueTx)
        Adapter->TxQueue = Queue;
    else
        Adapter->RxQueue = Queue;

    Init->CreatedQueue = (NETPACKETQUEUE)Object;
    *PacketQueue = (NETPACKETQUEUE)Object;
    return STATUS_SUCCESS;
}

/* Transmit */

static
ULONG
NxTxCountFragments(
    _In_ PNET_BUFFER Nb)
{
    PMDL Mdl = NET_BUFFER_CURRENT_MDL(Nb);
    ULONG Offset = NET_BUFFER_CURRENT_MDL_OFFSET(Nb);
    ULONG Remaining = NET_BUFFER_DATA_LENGTH(Nb);
    ULONG Count = 0, Length;

    for (; Mdl != NULL && Remaining != 0; Mdl = Mdl->Next, Offset = 0)
    {
        Length = MmGetMdlByteCount(Mdl);
        if (Length <= Offset)
            continue;
        Length = min(Length - Offset, Remaining);
        Remaining -= Length;
        Count++;
    }

    /* A chain shorter than the data length is malformed */
    return Remaining == 0 ? Count : 0;
}

static
VOID
NxTxReleaseNb(
    _In_ PNET_BUFFER_LIST Nbl,
    _Inout_ PNET_BUFFER_LIST *CompleteHead,
    _Inout_ PNET_BUFFER_LIST **CompleteTail)
{
    if (--NX_NBL_PENDING(Nbl) == 0)
    {
        NET_BUFFER_LIST_NEXT_NBL(Nbl) = NULL;
        **CompleteTail = Nbl;
        *CompleteTail = &NET_BUFFER_LIST_NEXT_NBL(Nbl);
    }
}

static
VOID
NxTxComplete(
    _In_ PNX_QUEUE Queue,
    _In_opt_ PNET_BUFFER_LIST Nbls)
{
    if (Nbls != NULL)
        NdisMSendNetBufferListsComplete(Queue->Adapter->NdisHandle, Nbls, 0);
}

static
VOID
NxTxFillLayout(
    _In_ PNX_ADAPTER Adapter,
    _In_ PNET_BUFFER Nb,
    _Out_ NET_PACKET *Packet)
{
    UCHAR Buffer[14];
    PUCHAR Header;
    ULONG L2 = (Adapter->Medium == NdisMedium802_3) ? 14 : 0;
    UCHAR Version;
    USHORT EtherType = 0;

    if (NET_BUFFER_DATA_LENGTH(Nb) < L2 + 1)
        return;

    Header = NdisGetDataBuffer(Nb, L2 + 1, Buffer, 1, 0);
    if (Header == NULL)
        return;

    if (L2 != 0)
    {
        Packet->Layout.Layer2Type = NetPacketLayer2TypeEthernet;
        Packet->Layout.Layer2HeaderLength = 14;
        EtherType = (USHORT)((Header[12] << 8) | Header[13]);
    }
    else
    {
        Packet->Layout.Layer2Type = NetPacketLayer2TypeNull;
    }

    Version = Header[L2] >> 4;
    if ((L2 && EtherType == NX_ETHERTYPE_IPV4) || (!L2 && Version == 4))
    {
        Packet->Layout.Layer3Type = NetPacketLayer3TypeIPv4UnspecifiedOptions;
        Packet->Layout.Layer3HeaderLength = (Header[L2] & 0x0F) * 4;
    }
    else if ((L2 && EtherType == NX_ETHERTYPE_IPV6) || (!L2 && Version == 6))
    {
        Packet->Layout.Layer3Type = NetPacketLayer3TypeIPv6UnspecifiedExtensions;
    }
}

/* Moves queued NET_BUFFERs onto the rings; returns how many */
static
ULONG
NxTxPost(
    _In_ PNX_QUEUE Queue)
{
    PNX_ADAPTER Adapter = Queue->Adapter;
    NET_RING *Pr = Queue->PacketRing;
    NET_RING *Fr = Queue->FragmentRing;
    PNET_BUFFER_LIST Complete = NULL, *CompleteTail = &Complete;
    PNET_BUFFER_LIST Nbl;
    PNET_BUFFER Nb;
    NET_PACKET *Packet;
    NET_FRAGMENT *Fragment;
    PMDL Mdl;
    PUCHAR Va;
    ULONG Offset, Remaining, Length, Fragments, MaxFragments;
    UINT32 PacketFree, FragmentFree, FragmentIndex;
    ULONG Posted = 0;
    KIRQL OldIrql;

    MaxFragments = (ULONG)min(Adapter->TxCapabilities.MaximumNumberOfFragments,
                              (SIZE_T)Fr->NumberOfElements - 1);

    for (;;)
    {
        KeAcquireSpinLock(&Queue->SendLock, &OldIrql);
        Nbl = Queue->SendHead;
        Nb = Queue->SendNextNb;
        KeReleaseSpinLock(&Queue->SendLock, OldIrql);

        if (Nbl == NULL)
            break;
        if (Nb == NULL)
            Nb = NET_BUFFER_LIST_FIRST_NB(Nbl);

        if (Nb == NULL)
        {
            /* A NET_BUFFER_LIST without NET_BUFFERs completes right away */
            KeAcquireSpinLock(&Queue->SendLock, &OldIrql);
            Queue->SendHead = NET_BUFFER_LIST_NEXT_NBL(Nbl);
            if (Queue->SendHead == NULL)
                Queue->SendTail = NULL;
            KeReleaseSpinLock(&Queue->SendLock, OldIrql);
            NxTxReleaseNb(Nbl, &Complete, &CompleteTail);
            continue;
        }

        Fragments = NxTxCountFragments(Nb);

        PacketFree = (Queue->OsPacketBegin - Pr->EndIndex - 1) & Pr->ElementIndexMask;
        FragmentFree = (Queue->OsFragmentBegin - Fr->EndIndex - 1) & Fr->ElementIndexMask;
        if (Fragments != 0 && Fragments <= MaxFragments &&
            (PacketFree == 0 || FragmentFree < Fragments))
        {
            /* Ring full: try again once the client returns elements */
            break;
        }

        if (Fragments == 0 || Fragments > MaxFragments)
        {
            /* Nothing the client could send; drop this NET_BUFFER */
            InterlockedIncrement64(&Adapter->TxErrors);
            NET_BUFFER_LIST_STATUS(Nbl) = NDIS_STATUS_INVALID_PACKET;
            NxTxReleaseNb(Nbl, &Complete, &CompleteTail);
        }
        else
        {
            Packet = NetRingGetPacketAtIndex(Pr, Pr->EndIndex);
            RtlZeroMemory(Packet, sizeof(*Packet));
            Packet->FragmentIndex = Fr->EndIndex;
            Packet->FragmentCount = (UINT16)Fragments;
            NxTxFillLayout(Adapter, Nb, Packet);
            Queue->PacketChecksum[Pr->EndIndex] = (NET_PACKET_CHECKSUM){ 0 };
            Queue->PacketNbl[Pr->EndIndex] = Nbl;

            FragmentIndex = Fr->EndIndex;
            Mdl = NET_BUFFER_CURRENT_MDL(Nb);
            Offset = NET_BUFFER_CURRENT_MDL_OFFSET(Nb);
            Remaining = NET_BUFFER_DATA_LENGTH(Nb);
            for (; Mdl != NULL && Remaining != 0; Mdl = Mdl->Next, Offset = 0)
            {
                Length = MmGetMdlByteCount(Mdl);
                if (Length <= Offset)
                    continue;
                Length = min(Length - Offset, Remaining);
                Remaining -= Length;

                Va = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
                Fragment = NetRingGetFragmentAtIndex(Fr, FragmentIndex);
                RtlZeroMemory(Fragment, sizeof(*Fragment));
                Fragment->ValidLength = Length;
                Fragment->Capacity = Length;
                Queue->FragmentVirtualAddress[FragmentIndex].VirtualAddress =
                    Va ? Va + Offset : NULL;
                FragmentIndex = NetRingIncrementIndex(Fr, FragmentIndex);
            }

            InterlockedIncrement64(&Adapter->TxPackets);
            InterlockedAdd64(&Adapter->TxBytes, NET_BUFFER_DATA_LENGTH(Nb));

            /* Elements first, then the index that hands them over */
            KeMemoryBarrier();
            Fr->EndIndex = FragmentIndex;
            Pr->EndIndex = NetRingIncrementIndex(Pr, Pr->EndIndex);
            Posted++;
        }

        /* Next NET_BUFFER, or the next NET_BUFFER_LIST */
        KeAcquireSpinLock(&Queue->SendLock, &OldIrql);
        Queue->SendNextNb = NET_BUFFER_NEXT_NB(Nb);
        if (Queue->SendNextNb == NULL)
        {
            Queue->SendHead = NET_BUFFER_LIST_NEXT_NBL(Nbl);
            if (Queue->SendHead == NULL)
                Queue->SendTail = NULL;
        }
        KeReleaseSpinLock(&Queue->SendLock, OldIrql);
    }

    NxTxComplete(Queue, Complete);
    return Posted;
}

/* Completes what the client gave back; returns how many packets */
static
ULONG
NxTxDrain(
    _In_ PNX_QUEUE Queue)
{
    NET_RING *Pr = Queue->PacketRing;
    NET_RING *Fr = Queue->FragmentRing;
    PNET_BUFFER_LIST Complete = NULL, *CompleteTail = &Complete;
    PNET_BUFFER_LIST Nbl;
    UINT32 Begin, Index;
    ULONG Drained = 0;

    Begin = *(volatile UINT32 *)&Pr->BeginIndex;
    KeMemoryBarrier();

    for (Index = Queue->OsPacketBegin; Index != Begin; Index = NetRingIncrementIndex(Pr, Index))
    {
        Nbl = Queue->PacketNbl[Index];
        Queue->PacketNbl[Index] = NULL;
        if (Nbl != NULL)
            NxTxReleaseNb(Nbl, &Complete, &CompleteTail);
        Drained++;
    }

    Queue->OsPacketBegin = Begin;
    Queue->OsFragmentBegin = *(volatile UINT32 *)&Fr->BeginIndex;

    NxTxComplete(Queue, Complete);
    return Drained;
}

/* Completes sends that never reached the client */
static
VOID
NxTxFlush(
    _In_ PNX_QUEUE Queue,
    _In_ NDIS_STATUS Status)
{
    PNET_BUFFER_LIST Complete = NULL, *CompleteTail = &Complete;
    PNET_BUFFER_LIST Nbl, Next;
    PNET_BUFFER Nb, StartNb;
    UINT32 Index;
    KIRQL OldIrql;

    /* Packets the client never returned */
    for (Index = 0; Index < Queue->PacketRing->NumberOfElements; Index++)
    {
        Nbl = Queue->PacketNbl[Index];
        Queue->PacketNbl[Index] = NULL;
        if (Nbl != NULL)
        {
            NET_BUFFER_LIST_STATUS(Nbl) = Status;
            NxTxReleaseNb(Nbl, &Complete, &CompleteTail);
        }
    }

    KeAcquireSpinLock(&Queue->SendLock, &OldIrql);
    Nbl = Queue->SendHead;
    StartNb = Queue->SendNextNb;
    Queue->SendHead = Queue->SendTail = NULL;
    Queue->SendNextNb = NULL;
    KeReleaseSpinLock(&Queue->SendLock, OldIrql);

    for (; Nbl != NULL; Nbl = Next, StartNb = NULL)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(Nbl);
        NET_BUFFER_LIST_STATUS(Nbl) = Status;
        Nb = StartNb ? StartNb : NET_BUFFER_LIST_FIRST_NB(Nbl);
        if (Nb == NULL)
            NxTxReleaseNb(Nbl, &Complete, &CompleteTail);
        for (; Nb != NULL; Nb = NET_BUFFER_NEXT_NB(Nb))
            NxTxReleaseNb(Nbl, &Complete, &CompleteTail);
    }

    NxTxComplete(Queue, Complete);
}

VOID
NxQueueSend(
    _In_ PNX_QUEUE Queue,
    _In_ PNET_BUFFER_LIST NetBufferLists)
{
    PNET_BUFFER_LIST Nbl, Last = NULL;
    PNET_BUFFER Nb;
    LONG_PTR Count;
    KIRQL OldIrql;

    for (Nbl = NetBufferLists; Nbl != NULL; Nbl = NET_BUFFER_LIST_NEXT_NBL(Nbl))
    {
        Count = 0;
        for (Nb = NET_BUFFER_LIST_FIRST_NB(Nbl); Nb != NULL; Nb = NET_BUFFER_NEXT_NB(Nb))
            Count++;
        /* An empty list still completes once */
        NX_NBL_PENDING(Nbl) = Count ? Count : 1;
        NET_BUFFER_LIST_STATUS(Nbl) = NDIS_STATUS_SUCCESS;
        Last = Nbl;
    }

    KeAcquireSpinLock(&Queue->SendLock, &OldIrql);
    if (Queue->SendTail != NULL)
        NET_BUFFER_LIST_NEXT_NBL(Queue->SendTail) = NetBufferLists;
    else
        Queue->SendHead = NetBufferLists;
    Queue->SendTail = Last;
    KeReleaseSpinLock(&Queue->SendLock, OldIrql);

    KeSetEvent(&Queue->Notify, IO_NETWORK_INCREMENT, FALSE);
}

/* Receive */

static
ULONG
NxRxPost(
    _In_ PNX_QUEUE Queue)
{
    NET_RING *Pr = Queue->PacketRing;
    NET_RING *Fr = Queue->FragmentRing;
    NET_FRAGMENT *Fragment;
    UINT32 Free, Index;
    ULONG Posted = 0;

    Free = (Queue->OsFragmentBegin - Fr->EndIndex - 1) & Fr->ElementIndexMask;
    for (Index = Fr->EndIndex; Free != 0; Free--, Index = NetRingIncrementIndex(Fr, Index))
    {
        Fragment = NetRingGetFragmentAtIndex(Fr, Index);
        RtlZeroMemory(Fragment, sizeof(*Fragment));
        Fragment->Capacity = Queue->RxBufferSize;
        /* The client may have pointed it elsewhere: re-attach our buffer */
        Queue->FragmentVirtualAddress[Index].VirtualAddress =
            Queue->RxBuffers + (SIZE_T)Index * Queue->RxBufferSize;
        Posted++;
    }
    KeMemoryBarrier();
    Fr->EndIndex = Index;

    Free = (Queue->OsPacketBegin - Pr->EndIndex - 1) & Pr->ElementIndexMask;
    for (Index = Pr->EndIndex; Free != 0; Free--, Index = NetRingIncrementIndex(Pr, Index))
    {
        RtlZeroMemory(NetRingGetPacketAtIndex(Pr, Index), sizeof(NET_PACKET));
        Queue->PacketChecksum[Index] = (NET_PACKET_CHECKSUM){ 0 };
        Posted++;
    }
    KeMemoryBarrier();
    Pr->EndIndex = Index;

    return Posted;
}

static
PNET_BUFFER_LIST
NxRxBuildNbl(
    _In_ PNX_QUEUE Queue,
    _In_ UINT32 PacketIndex,
    _In_ NET_PACKET const *Packet)
{
    PNX_ADAPTER Adapter = Queue->Adapter;
    NET_RING *Fr = Queue->FragmentRing;
    NET_FRAGMENT const *Fragment;
    NET_PACKET_CHECKSUM Checksum;
    PNET_BUFFER_LIST Nbl;
    PUCHAR Data, Source;
    PMDL Mdl;
    SIZE_T Total = 0, Copied = 0;
    UINT32 Index, i;
    ULONG Info = 0;

    if (Packet->FragmentCount == 0 || Packet->FragmentCount > Fr->NumberOfElements ||
        Packet->FragmentIndex >= Fr->NumberOfElements)
    {
        return NULL;
    }

    for (i = 0, Index = Packet->FragmentIndex; i < Packet->FragmentCount;
         i++, Index = NetRingIncrementIndex(Fr, Index))
    {
        Fragment = NetRingGetFragmentAtIndex(Fr, Index);
        if (Fragment->Offset + Fragment->ValidLength > Queue->RxBufferSize)
            return NULL;
        Total += Fragment->ValidLength;
    }

    if (Total == 0 || Total > MAXULONG)
        return NULL;

    Data = ExAllocatePoolWithTag(NonPagedPool, Total, NX_RX_TAG);
    if (Data == NULL)
        return NULL;

    for (i = 0, Index = Packet->FragmentIndex; i < Packet->FragmentCount;
         i++, Index = NetRingIncrementIndex(Fr, Index))
    {
        Fragment = NetRingGetFragmentAtIndex(Fr, Index);
        Source = Queue->FragmentVirtualAddress[Index].VirtualAddress;
        if (Source == NULL)
        {
            ExFreePoolWithTag(Data, NX_RX_TAG);
            return NULL;
        }
        RtlCopyMemory(Data + Copied, Source + Fragment->Offset, Fragment->ValidLength);
        Copied += Fragment->ValidLength;
    }

    Mdl = IoAllocateMdl(Data, (ULONG)Total, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        ExFreePoolWithTag(Data, NX_RX_TAG);
        return NULL;
    }
    MmBuildMdlForNonPagedPool(Mdl);

    Nbl = NdisAllocateNetBufferAndNetBufferList(Adapter->RxNblPool, 0, 0, Mdl, 0, (ULONG)Total);
    if (Nbl == NULL)
    {
        IoFreeMdl(Mdl);
        ExFreePoolWithTag(Data, NX_RX_TAG);
        return NULL;
    }

    Nbl->SourceHandle = Adapter->NdisHandle;

    if (Queue->ChecksumEnabled)
    {
        Checksum = Queue->PacketChecksum[PacketIndex];
        if (Checksum.Layer3 == NetPacketRxChecksumEvaluationValid)
            Info |= NX_CSUM_IP_SUCCEEDED;
        else if (Checksum.Layer3 == NetPacketRxChecksumEvaluationInvalid)
            Info |= NX_CSUM_IP_FAILED;

        if (Packet->Layout.Layer4Type == NetPacketLayer4TypeTcp)
        {
            if (Checksum.Layer4 == NetPacketRxChecksumEvaluationValid)
                Info |= NX_CSUM_TCP_SUCCEEDED;
            else if (Checksum.Layer4 == NetPacketRxChecksumEvaluationInvalid)
                Info |= NX_CSUM_TCP_FAILED;
        }
        else if (Packet->Layout.Layer4Type == NetPacketLayer4TypeUdp)
        {
            if (Checksum.Layer4 == NetPacketRxChecksumEvaluationValid)
                Info |= NX_CSUM_UDP_SUCCEEDED;
            else if (Checksum.Layer4 == NetPacketRxChecksumEvaluationInvalid)
                Info |= NX_CSUM_UDP_FAILED;
        }
        NET_BUFFER_LIST_INFO(Nbl, TcpIpChecksumNetBufferListInfo) = (PVOID)(ULONG_PTR)Info;
    }

    /* A raw IP frame carries no Ethernet type: tell the protocol which IP */
    if (Adapter->Medium == NdisMediumIP)
    {
        USHORT EtherType = ((Data[0] >> 4) == 6) ? NX_ETHERTYPE_IPV6 : NX_ETHERTYPE_IPV4;
        NET_BUFFER_LIST_INFO(Nbl, NetBufferListFrameType) =
            (PVOID)(ULONG_PTR)RtlUshortByteSwap(EtherType);
    }

    InterlockedIncrement64(&Adapter->RxPackets);
    InterlockedAdd64(&Adapter->RxBytes, (LONG64)Total);
    return Nbl;
}

/* Indicates what the client received; returns how many packets moved */
static
ULONG
NxRxDrain(
    _In_ PNX_QUEUE Queue,
    _In_ BOOLEAN Indicate)
{
    PNX_ADAPTER Adapter = Queue->Adapter;
    NET_RING *Pr = Queue->PacketRing;
    NET_RING *Fr = Queue->FragmentRing;
    PNET_BUFFER_LIST Head = NULL, *Tail = &Head, Nbl;
    NET_PACKET const *Packet;
    UINT32 Begin, Index;
    ULONG Drained = 0, Count = 0;

    Begin = *(volatile UINT32 *)&Pr->BeginIndex;
    KeMemoryBarrier();

    for (Index = Queue->OsPacketBegin; Index != Begin; Index = NetRingIncrementIndex(Pr, Index))
    {
        Drained++;
        Packet = NetRingGetPacketAtIndex(Pr, Index);
        if (!Indicate || Packet->Ignore || Packet->FragmentCount == 0)
            continue;

        Nbl = NxRxBuildNbl(Queue, Index, Packet);
        if (Nbl == NULL)
        {
            InterlockedIncrement64(&Adapter->RxDropped);
            continue;
        }

        *Tail = Nbl;
        Tail = &NET_BUFFER_LIST_NEXT_NBL(Nbl);
        Count++;
    }

    /* The data is copied out: the elements can be posted again */
    Queue->OsPacketBegin = Begin;
    Queue->OsFragmentBegin = *(volatile UINT32 *)&Fr->BeginIndex;

    if (Head != NULL)
    {
        KeClearEvent(&Adapter->RxIdle);
        InterlockedAdd(&Adapter->RxOutstanding, (LONG)Count);
        NdisMIndicateReceiveNetBufferLists(Adapter->NdisHandle, Head,
                                           NDIS_DEFAULT_PORT_NUMBER, Count, 0);
    }

    return Drained;
}

VOID
NxRxReturnNetBufferLists(
    _In_ PNX_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists)
{
    PNET_BUFFER_LIST Nbl, Next;
    PMDL Mdl;
    PVOID Data;

    for (Nbl = NetBufferLists; Nbl != NULL; Nbl = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(Nbl);
        NET_BUFFER_LIST_NEXT_NBL(Nbl) = NULL;

        Mdl = NET_BUFFER_FIRST_MDL(NET_BUFFER_LIST_FIRST_NB(Nbl));
        Data = MmGetMdlVirtualAddress(Mdl);
        NdisFreeNetBufferList(Nbl);
        IoFreeMdl(Mdl);
        ExFreePoolWithTag(Data, NX_RX_TAG);

        if (InterlockedDecrement(&Adapter->RxOutstanding) == 0)
            KeSetEvent(&Adapter->RxIdle, IO_NO_INCREMENT, FALSE);
    }
}

/* Execution context */

static
ULONG
NxQueuePoll(
    _In_ PNX_QUEUE Queue)
{
    ULONG Moved;

    if (Queue->Type == NxQueueTx)
    {
        Moved = NxTxPost(Queue);
        Queue->Config.EvtAdvance(Queue->Handle);
        Moved += NxTxDrain(Queue);
    }
    else
    {
        Moved = NxRxPost(Queue);
        Queue->Config.EvtAdvance(Queue->Handle);
        Moved += NxRxDrain(Queue, TRUE);
    }

    return Moved;
}

static
VOID
NxQueueArm(
    _In_ PNX_QUEUE Queue)
{
    NET_RING *Pr = Queue->PacketRing;

    if (Queue->Config.EvtSetNotificationEnabled == NULL)
        return;

    /* As on Windows: a transmit queue is armed only with packets pending */
    if (Queue->Type == NxQueueTx && Pr->BeginIndex == Pr->EndIndex)
        return;

    Queue->Config.EvtSetNotificationEnabled(Queue->Handle, TRUE);
}

static
VOID
NxQueueCancel(
    _In_ PNX_QUEUE Queue)
{
    NET_RING *Pr = Queue->PacketRing;
    NET_RING *Fr = Queue->FragmentRing;
    LARGE_INTEGER Delay;
    ULONG Tries;

    Queue->Config.EvtCancel(Queue->Handle);

    /* The client gives everything back, possibly over several advances */
    Delay.QuadPart = -10 * 1000 * 10;
    for (Tries = 0; Tries < 500; Tries++)
    {
        if (Queue->Type == NxQueueTx)
            NxTxDrain(Queue);
        else
            NxRxDrain(Queue, FALSE);

        if (Pr->BeginIndex == Pr->EndIndex && Fr->BeginIndex == Fr->EndIndex)
            break;

        Queue->Config.EvtAdvance(Queue->Handle);
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    if (Tries == 500)
    {
        DbgPrint("NetAdapterCx: queue %p kept %lu packets after cancel\n",
                 Queue->Handle, (Pr->EndIndex - Pr->BeginIndex) & Pr->ElementIndexMask);
    }

    if (Queue->Type == NxQueueTx)
        NxTxFlush(Queue, NDIS_STATUS_PAUSED);
}

static KSTART_ROUTINE NxQueueThread;

static
VOID
NTAPI
NxQueueThread(
    _In_ PVOID Context)
{
    PNX_QUEUE Queue = Context;
    LARGE_INTEGER Timeout;

    if (Queue->Config.EvtStart != NULL)
        Queue->Config.EvtStart(Queue->Handle);
    KeSetEvent(&Queue->ThreadStarted, IO_NO_INCREMENT, FALSE);

    /* Wakes up now and then even if a notification got lost */
    Timeout.QuadPart = -10LL * 1000 * 1000;

    while (!Queue->StopRequested)
    {
        if (NxQueuePoll(Queue) != 0)
            continue;

        NxQueueArm(Queue);

        /* Work may have arrived just before the client was armed */
        if (NxQueuePoll(Queue) != 0)
            continue;

        KeWaitForSingleObject(&Queue->Notify, Executive, KernelMode, FALSE, &Timeout);
    }

    NxQueueCancel(Queue);

    if (Queue->Config.EvtStop != NULL)
        Queue->Config.EvtStop(Queue->Handle);

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
NTSTATUS
NxQueueStartThread(
    _In_ PNX_QUEUE Queue)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;

    Queue->OsPacketBegin = Queue->OsFragmentBegin = 0;
    Queue->StopRequested = FALSE;
    KeInitializeEvent(&Queue->ThreadStarted, NotificationEvent, FALSE);

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Queue->ThreadHandle, THREAD_ALL_ACCESS, &ObjectAttributes,
                                  NULL, NULL, NxQueueThread, Queue);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle(Queue->ThreadHandle, SYNCHRONIZE, *PsThreadType,
                                       KernelMode, (PVOID *)&Queue->Thread, NULL);
    ASSERT(NT_SUCCESS(Status));
    Queue->Started = TRUE;

    /* The queue counts as started once the client saw EvtStart */
    KeWaitForSingleObject(&Queue->ThreadStarted, Executive, KernelMode, FALSE, NULL);
    return STATUS_SUCCESS;
}

static
VOID
NxQueueStopThread(
    _In_ PNX_QUEUE Queue)
{
    if (!Queue->Started)
        return;

    InterlockedExchange(&Queue->StopRequested, TRUE);
    KeSetEvent(&Queue->Notify, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(Queue->Thread, Executive, KernelMode, FALSE, NULL);

    ObDereferenceObject(Queue->Thread);
    ZwClose(Queue->ThreadHandle);
    Queue->Thread = NULL;
    Queue->ThreadHandle = NULL;
    Queue->Started = FALSE;
}

static
VOID
NxQueueDelete(
    _Inout_ PNX_QUEUE *Queue)
{
    if (*Queue == NULL)
        return;

    NxQueueStopThread(*Queue);
    WdfObjectDelete((*Queue)->Handle);
    *Queue = NULL;
}

/* Called by the miniport Restart handler with the adapter lock held */
NTSTATUS
NxDatapathStart(
    _In_ PNX_ADAPTER Adapter)
{
    NX_QUEUE_INIT Init;
    NTSTATUS Status;

    if (Adapter->DatapathRunning)
        return STATUS_SUCCESS;

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Signature = NX_QUEUE_INIT_SIGNATURE;
    Init.Type = NxQueueTx;
    Init.Adapter = Adapter;
    Init.QueueId = 0;

    Status = Adapter->DatapathCallbacks.EvtAdapterCreateTxQueue(Adapter->Handle,
                                                                (NETTXQUEUE_INIT *)&Init);
    if (NT_SUCCESS(Status) && Init.CreatedQueue == NULL)
        Status = STATUS_UNSUCCESSFUL;
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("NetAdapterCx: EvtAdapterCreateTxQueue failed 0x%lx\n", Status);
        goto Fail;
    }

    RtlZeroMemory(&Init, sizeof(Init));
    Init.Signature = NX_QUEUE_INIT_SIGNATURE;
    Init.Type = NxQueueRx;
    Init.Adapter = Adapter;
    Init.QueueId = 0;

    Status = Adapter->DatapathCallbacks.EvtAdapterCreateRxQueue(Adapter->Handle,
                                                                (NETRXQUEUE_INIT *)&Init);
    if (NT_SUCCESS(Status) && Init.CreatedQueue == NULL)
        Status = STATUS_UNSUCCESSFUL;
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("NetAdapterCx: EvtAdapterCreateRxQueue failed 0x%lx\n", Status);
        goto Fail;
    }

    Status = NxQueueStartThread(Adapter->TxQueue);
    if (NT_SUCCESS(Status))
        Status = NxQueueStartThread(Adapter->RxQueue);
    if (!NT_SUCCESS(Status))
        goto Fail;

    /* Let sends in */
    ExReInitializeRundownProtection(&Adapter->DatapathRundown);
    InterlockedExchange(&Adapter->DatapathRunning, TRUE);
    DbgPrint("NetAdapterCx: adapter %p datapath started\n", Adapter->Handle);
    return STATUS_SUCCESS;

Fail:
    NxQueueDelete(&Adapter->RxQueue);
    NxQueueDelete(&Adapter->TxQueue);
    return Status;
}

/* Called by the miniport Pause handler with the adapter lock held */
VOID
NxDatapathStop(
    _In_ PNX_ADAPTER Adapter)
{
    LARGE_INTEGER Timeout;

    if (!Adapter->DatapathRunning && Adapter->TxQueue == NULL && Adapter->RxQueue == NULL)
        return;

    if (InterlockedExchange(&Adapter->DatapathRunning, FALSE))
    {
        /* No send handler touches the queues past this point */
        ExWaitForRundownProtectionRelease(&Adapter->DatapathRundown);
    }

    NxQueueDelete(&Adapter->TxQueue);
    NxQueueDelete(&Adapter->RxQueue);

    /* NDIS has to give back every receive before the pause completes */
    Timeout.QuadPart = -10LL * 1000 * 1000 * 5;
    if (KeWaitForSingleObject(&Adapter->RxIdle, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
    {
        DbgPrint("NetAdapterCx: %ld receives still with NDIS\n", Adapter->RxOutstanding);
    }

    DbgPrint("NetAdapterCx: adapter %p datapath stopped\n", Adapter->Handle);
}

/* Client entry points */

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
imp_NetTxQueueCreate(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _Inout_ NETTXQUEUE_INIT * NetTxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * TxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG * Configuration,
    _Out_ NETPACKETQUEUE * PacketQueue)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxQueueCreate(NxQueueInitGet(NetTxQueueInit, NxQueueTx),
                         TxQueueAttributes, Configuration, PacketQueue);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
imp_NetRxQueueCreate(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _Inout_ NETRXQUEUE_INIT * NetRxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * RxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG * Configuration,
    _Out_ NETPACKETQUEUE * PacketQueue)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxQueueCreate(NxQueueInitGet(NetRxQueueInit, NxQueueRx),
                         RxQueueAttributes, Configuration, PacketQueue);
}

ULONG
NTAPI
imp_NetTxQueueInitGetQueueId(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETTXQUEUE_INIT * NetTxQueueInit)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxQueueInitGet(NetTxQueueInit, NxQueueTx)->QueueId;
}

ULONG
NTAPI
imp_NetRxQueueInitGetQueueId(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETRXQUEUE_INIT * NetRxQueueInit)
{
    NxGetPrivateGlobals(DriverGlobals);
    return NxQueueInitGet(NetRxQueueInit, NxQueueRx)->QueueId;
}

VOID
NTAPI
imp_NetTxQueueNotifyMoreCompletedPacketsAvailable(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE TxQueue)
{
    NxGetPrivateGlobals(DriverGlobals);
    KeSetEvent(&NxQueueGet(TxQueue)->Notify, IO_NETWORK_INCREMENT, FALSE);
}

VOID
NTAPI
imp_NetRxQueueNotifyMoreReceivedPacketsAvailable(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE RxQueue)
{
    NxGetPrivateGlobals(DriverGlobals);
    KeSetEvent(&NxQueueGet(RxQueue)->Notify, IO_NETWORK_INCREMENT, FALSE);
}

NET_RING_COLLECTION const *
NTAPI
imp_NetTxQueueGetRingCollection(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE NetTxQueue)
{
    NxGetPrivateGlobals(DriverGlobals);
    return &NxQueueGet(NetTxQueue)->Rings;
}

NET_RING_COLLECTION const *
NTAPI
imp_NetRxQueueGetRingCollection(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE NetRxQueue)
{
    NxGetPrivateGlobals(DriverGlobals);
    return &NxQueueGet(NetRxQueue)->Rings;
}

static
VOID
NxQueueGetExtension(
    _In_ PNX_QUEUE Queue,
    _In_ NET_EXTENSION_QUERY const *Query,
    _Out_ NET_EXTENSION *Extension)
{
    RtlZeroMemory(Extension, sizeof(*Extension));

    if (Query == NULL || Query->Name == NULL)
        return;

    if (Query->Type == NetExtensionTypeFragment &&
        Query->Version <= NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1 &&
        _wcsicmp(Query->Name, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME) == 0)
    {
        Extension->Reserved[0] = Queue->FragmentVirtualAddress;
        Extension->Reserved[1] = (PVOID)sizeof(NET_FRAGMENT_VIRTUAL_ADDRESS);
        Extension->Enabled = TRUE;
    }
    else if (Query->Type == NetExtensionTypePacket &&
             Query->Version <= NET_PACKET_EXTENSION_CHECKSUM_VERSION_1 &&
             _wcsicmp(Query->Name, NET_PACKET_EXTENSION_CHECKSUM_NAME) == 0 &&
             Queue->ChecksumEnabled)
    {
        Extension->Reserved[0] = Queue->PacketChecksum;
        Extension->Reserved[1] = (PVOID)sizeof(NET_PACKET_CHECKSUM);
        Extension->Enabled = TRUE;
    }
    else
    {
        DPRINT("Extension %S %lu type %d not available\n",
               Query->Name, Query->Version, Query->Type);
    }
}

VOID
NTAPI
imp_NetTxQueueGetExtension(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE NetTxQueue,
    _In_ NET_EXTENSION_QUERY const * Query,
    _Out_ NET_EXTENSION* Extension)
{
    NxGetPrivateGlobals(DriverGlobals);
    NxQueueGetExtension(NxQueueGet(NetTxQueue), Query, Extension);
}

VOID
NTAPI
imp_NetRxQueueGetExtension(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE NetRxQueue,
    _In_ NET_EXTENSION_QUERY const * Query,
    _Out_ NET_EXTENSION* Extension)
{
    NxGetPrivateGlobals(DriverGlobals);
    NxQueueGetExtension(NxQueueGet(NetRxQueue), Query, Extension);
}

UINT8
NTAPI
imp_NetTxQueueGetDemux8021p(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETPACKETQUEUE Queue)
{
    NxGetPrivateGlobals(DriverGlobals);
    NxQueueGet(Queue);
    /* No 802.1p demultiplexing: every queue takes priority 0 */
    return 0;
}
