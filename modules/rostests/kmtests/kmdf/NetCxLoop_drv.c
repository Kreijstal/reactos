/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx loopback client driver
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A KMDF NetAdapterCx client in the style of a real one (ovpn-dco-win, the
 * RtEthSample): one adapter, one transmit and one receive queue, system
 * managed receive buffers. Every packet the OS gives the transmit queue is
 * copied to a FIFO and handed back on the receive queue. Built once per
 * NetAdapterCx version (NETADAPTER_VERSION_MINOR 0 and 1).
 */

#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <net/virtualaddress.h>
#include <net/checksum.h>

#include "KmdfNetCx.h"

#define TAG_LOOP        'pLxN'
#define LOOP_FIFO_MAX   256
#define LOOP_FRAME_MAX  2048

typedef struct _LOOP_FRAME
{
    LIST_ENTRY Link;
    ULONG Length;
    UCHAR Data[LOOP_FRAME_MAX];
} LOOP_FRAME, *PLOOP_FRAME;

typedef struct _LOOP_DEVICE
{
    WDFDEVICE Device;
    NETADAPTER Adapter;
    NETPACKETQUEUE RxQueue;
    volatile LONG RxNotificationEnabled;
    volatile LONG TxNotificationEnabled;
    NETPACKETQUEUE TxQueue;

    KSPIN_LOCK FifoLock;
    LIST_ENTRY Fifo;
    LONG FifoDepth;
} LOOP_DEVICE, *PLOOP_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LOOP_DEVICE, LoopGetDevice)

typedef struct _LOOP_ADAPTER
{
    PLOOP_DEVICE LoopDevice;
} LOOP_ADAPTER, *PLOOP_ADAPTER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LOOP_ADAPTER, LoopGetAdapter)

typedef struct _LOOP_QUEUE
{
    PLOOP_DEVICE LoopDevice;
    NET_RING_COLLECTION const *Rings;
    NET_EXTENSION VirtualAddress;
    NET_EXTENSION Checksum;
} LOOP_QUEUE, *PLOOP_QUEUE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LOOP_QUEUE, LoopGetQueue)

/* One device per driver: the state the user mode test reads */
static NETCXLOOP_STATE LoopState;

static
VOID
LoopFifoFlush(
    _In_ PLOOP_DEVICE LoopDevice)
{
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    for (;;)
    {
        KeAcquireSpinLock(&LoopDevice->FifoLock, &OldIrql);
        Entry = IsListEmpty(&LoopDevice->Fifo) ? NULL : RemoveHeadList(&LoopDevice->Fifo);
        if (Entry)
            LoopDevice->FifoDepth--;
        KeReleaseSpinLock(&LoopDevice->FifoLock, OldIrql);

        if (Entry == NULL)
            break;
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, LOOP_FRAME, Link), TAG_LOOP);
    }
}

/* Transmit queue */

static EVT_PACKET_QUEUE_ADVANCE LoopEvtTxAdvance;

static
VOID
NTAPI
LoopEvtTxAdvance(
    _In_ NETPACKETQUEUE PacketQueue)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);
    PLOOP_DEVICE LoopDevice = Queue->LoopDevice;
    NET_RING *Pr = NetRingCollectionGetPacketRing(Queue->Rings);
    NET_RING *Fr = NetRingCollectionGetFragmentRing(Queue->Rings);
    NET_PACKET *Packet;
    NET_FRAGMENT *Fragment;
    NET_FRAGMENT_VIRTUAL_ADDRESS *Va;
    PLOOP_FRAME Frame;
    UINT32 Index, FragmentIndex, i;
    BOOLEAN Looped = FALSE;
    KIRQL OldIrql;

    for (Index = Pr->BeginIndex; Index != Pr->EndIndex; Index = NetRingIncrementIndex(Pr, Index))
    {
        Packet = NetRingGetPacketAtIndex(Pr, Index);
        if (Packet->Ignore || Packet->Scratch)
            continue;

        Frame = ExAllocatePoolZero(NonPagedPool, sizeof(*Frame), TAG_LOOP);
        if (Frame == NULL)
        {
            InterlockedIncrement64(&LoopState.Dropped);
            continue;
        }

        FragmentIndex = Packet->FragmentIndex;
        for (i = 0; i < Packet->FragmentCount; i++)
        {
            Fragment = NetRingGetFragmentAtIndex(Fr, FragmentIndex);
            Va = NetExtensionGetFragmentVirtualAddress(&Queue->VirtualAddress, FragmentIndex);
            if (Frame->Length + Fragment->ValidLength > LOOP_FRAME_MAX || Va->VirtualAddress == NULL)
            {
                Frame->Length = 0;
                break;
            }
            RtlCopyMemory(Frame->Data + Frame->Length,
                          (PUCHAR)Va->VirtualAddress + Fragment->Offset,
                          (SIZE_T)Fragment->ValidLength);
            Frame->Length += (ULONG)Fragment->ValidLength;
            FragmentIndex = NetRingIncrementIndex(Fr, FragmentIndex);
        }

        InterlockedIncrement64(&LoopState.TxPackets);
        InterlockedAdd64(&LoopState.TxBytes, Frame->Length);
        if (Packet->FragmentCount > 1)
            InterlockedIncrement64(&LoopState.TxMultiFragmentPackets);

        KeAcquireSpinLock(&LoopDevice->FifoLock, &OldIrql);
        if (Frame->Length != 0 && LoopDevice->FifoDepth < LOOP_FIFO_MAX)
        {
            InsertTailList(&LoopDevice->Fifo, &Frame->Link);
            LoopDevice->FifoDepth++;
            Frame = NULL;
            Looped = TRUE;
        }
        KeReleaseSpinLock(&LoopDevice->FifoLock, OldIrql);

        if (Frame != NULL)
        {
            InterlockedIncrement64(&LoopState.Dropped);
            ExFreePoolWithTag(Frame, TAG_LOOP);
        }
    }

    /* Everything is copied: give all of it back */
    Pr->BeginIndex = Pr->EndIndex;
    Fr->BeginIndex = Fr->EndIndex;

    /* Wake the receive queue if it is waiting */
    if (Looped && LoopDevice->RxQueue != NULL &&
        InterlockedExchange(&LoopDevice->RxNotificationEnabled, FALSE))
    {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(LoopDevice->RxQueue);
    }
}

static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED LoopEvtTxSetNotificationEnabled;

static
VOID
NTAPI
LoopEvtTxSetNotificationEnabled(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ BOOLEAN NotificationEnabled)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);

    /* Packets never wait here, so this means the queue had some pending */
    InterlockedIncrement(&LoopState.TxArmed);
    InterlockedExchange(&Queue->LoopDevice->TxNotificationEnabled, NotificationEnabled);
    if (NotificationEnabled)
        NetTxQueueNotifyMoreCompletedPacketsAvailable(PacketQueue);
}

static EVT_PACKET_QUEUE_CANCEL LoopEvtTxCancel;

static
VOID
NTAPI
LoopEvtTxCancel(
    _In_ NETPACKETQUEUE PacketQueue)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);
    NET_RING *Pr = NetRingCollectionGetPacketRing(Queue->Rings);
    NET_RING *Fr = NetRingCollectionGetFragmentRing(Queue->Rings);

    InterlockedIncrement(&LoopState.TxCancels);
    Pr->BeginIndex = Pr->EndIndex;
    Fr->BeginIndex = Fr->EndIndex;
}

/* Receive queue */

static EVT_PACKET_QUEUE_ADVANCE LoopEvtRxAdvance;

static
VOID
NTAPI
LoopEvtRxAdvance(
    _In_ NETPACKETQUEUE PacketQueue)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);
    PLOOP_DEVICE LoopDevice = Queue->LoopDevice;
    NET_RING *Pr = NetRingCollectionGetPacketRing(Queue->Rings);
    NET_RING *Fr = NetRingCollectionGetFragmentRing(Queue->Rings);
    NET_PACKET *Packet;
    NET_FRAGMENT *Fragment;
    NET_FRAGMENT_VIRTUAL_ADDRESS *Va;
    NET_PACKET_CHECKSUM *Checksum;
    PLIST_ENTRY Entry;
    PLOOP_FRAME Frame;
    UINT32 PacketIndex = Pr->BeginIndex;
    UINT32 FragmentIndex = Fr->BeginIndex;
    KIRQL OldIrql;

    while (PacketIndex != Pr->EndIndex && FragmentIndex != Fr->EndIndex)
    {
        KeAcquireSpinLock(&LoopDevice->FifoLock, &OldIrql);
        Entry = IsListEmpty(&LoopDevice->Fifo) ? NULL : RemoveHeadList(&LoopDevice->Fifo);
        if (Entry)
            LoopDevice->FifoDepth--;
        KeReleaseSpinLock(&LoopDevice->FifoLock, OldIrql);

        if (Entry == NULL)
            break;
        Frame = CONTAINING_RECORD(Entry, LOOP_FRAME, Link);

        Fragment = NetRingGetFragmentAtIndex(Fr, FragmentIndex);
        Va = NetExtensionGetFragmentVirtualAddress(&Queue->VirtualAddress, FragmentIndex);
        if (Frame->Length > Fragment->Capacity)
        {
            InterlockedIncrement64(&LoopState.Dropped);
            ExFreePoolWithTag(Frame, TAG_LOOP);
            continue;
        }

        RtlCopyMemory(Va->VirtualAddress, Frame->Data, Frame->Length);
        Fragment->ValidLength = Frame->Length;
        Fragment->Offset = 0;

        Packet = NetRingGetPacketAtIndex(Pr, PacketIndex);
        Packet->FragmentIndex = FragmentIndex;
        Packet->FragmentCount = 1;
        RtlZeroMemory(&Packet->Layout, sizeof(Packet->Layout));

        Checksum = NetExtensionGetPacketChecksum(&Queue->Checksum, PacketIndex);
        if (Checksum != NULL)
        {
            Checksum->Layer3 = NetPacketRxChecksumEvaluationValid;
            Checksum->Layer4 = NetPacketRxChecksumEvaluationNotChecked;
        }

        InterlockedIncrement64(&LoopState.RxPackets);
        InterlockedAdd64(&LoopState.RxBytes, Frame->Length);
        ExFreePoolWithTag(Frame, TAG_LOOP);

        PacketIndex = NetRingIncrementIndex(Pr, PacketIndex);
        FragmentIndex = NetRingIncrementIndex(Fr, FragmentIndex);
    }

    Pr->BeginIndex = PacketIndex;
    Fr->BeginIndex = FragmentIndex;
}

static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED LoopEvtRxSetNotificationEnabled;

static
VOID
NTAPI
LoopEvtRxSetNotificationEnabled(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ BOOLEAN NotificationEnabled)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);
    PLOOP_DEVICE LoopDevice = Queue->LoopDevice;
    BOOLEAN Pending;
    KIRQL OldIrql;

    InterlockedIncrement(&LoopState.RxArmed);
    InterlockedExchange(&LoopDevice->RxNotificationEnabled, NotificationEnabled);

    /* Frames may have arrived before the queue was armed */
    KeAcquireSpinLock(&LoopDevice->FifoLock, &OldIrql);
    Pending = !IsListEmpty(&LoopDevice->Fifo);
    KeReleaseSpinLock(&LoopDevice->FifoLock, OldIrql);

    if (NotificationEnabled && Pending &&
        InterlockedExchange(&LoopDevice->RxNotificationEnabled, FALSE))
    {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(PacketQueue);
    }
}

static EVT_PACKET_QUEUE_CANCEL LoopEvtRxCancel;

static
VOID
NTAPI
LoopEvtRxCancel(
    _In_ NETPACKETQUEUE PacketQueue)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);
    NET_RING *Pr = NetRingCollectionGetPacketRing(Queue->Rings);
    NET_RING *Fr = NetRingCollectionGetFragmentRing(Queue->Rings);
    UINT32 Index;

    InterlockedIncrement(&LoopState.RxCancels);

    for (Index = Pr->BeginIndex; Index != Pr->EndIndex; Index = NetRingIncrementIndex(Pr, Index))
        NetRingGetPacketAtIndex(Pr, Index)->Ignore = 1;

    Pr->BeginIndex = Pr->EndIndex;
    Fr->BeginIndex = Fr->EndIndex;
}

static EVT_PACKET_QUEUE_START LoopEvtRxStart;

static
VOID
NTAPI
LoopEvtRxStart(
    _In_ NETPACKETQUEUE PacketQueue)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);

    InterlockedIncrement(&LoopState.RxQueueStarts);
    Queue->LoopDevice->RxQueue = PacketQueue;
}

static EVT_PACKET_QUEUE_STOP LoopEvtRxStop;

static
VOID
NTAPI
LoopEvtRxStop(
    _In_ NETPACKETQUEUE PacketQueue)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);

    InterlockedIncrement(&LoopState.RxQueueStops);
    Queue->LoopDevice->RxQueue = NULL;
}

static EVT_WDF_OBJECT_CONTEXT_DESTROY LoopEvtQueueDestroy;

static
VOID
NTAPI
LoopEvtQueueDestroy(
    _In_ WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object);
    InterlockedIncrement(&LoopState.QueueDestroys);
}

static
VOID
LoopQueueInitialize(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ PLOOP_DEVICE LoopDevice,
    _In_ BOOLEAN Transmit)
{
    PLOOP_QUEUE Queue = LoopGetQueue(PacketQueue);
    NET_EXTENSION_QUERY Query;

    Queue->LoopDevice = LoopDevice;

    if (Transmit)
    {
        Queue->Rings = NetTxQueueGetRingCollection(PacketQueue);
        NET_EXTENSION_QUERY_INIT(&Query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                                 NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
                                 NetExtensionTypeFragment);
        NetTxQueueGetExtension(PacketQueue, &Query, &Queue->VirtualAddress);
    }
    else
    {
        Queue->Rings = NetRxQueueGetRingCollection(PacketQueue);
        NET_EXTENSION_QUERY_INIT(&Query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                                 NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
                                 NetExtensionTypeFragment);
        NetRxQueueGetExtension(PacketQueue, &Query, &Queue->VirtualAddress);

        NET_EXTENSION_QUERY_INIT(&Query, NET_PACKET_EXTENSION_CHECKSUM_NAME,
                                 NET_PACKET_EXTENSION_CHECKSUM_VERSION_1,
                                 NetExtensionTypePacket);
        NetRxQueueGetExtension(PacketQueue, &Query, &Queue->Checksum);
        if (Queue->Checksum.Enabled)
            InterlockedIncrement(&LoopState.ChecksumExtension);
    }

    if (Queue->VirtualAddress.Enabled)
        InterlockedIncrement(&LoopState.VirtualAddressExtension);
}

static EVT_NET_ADAPTER_CREATE_TXQUEUE LoopEvtCreateTxQueue;

static
NTSTATUS
NTAPI
LoopEvtCreateTxQueue(
    _In_ NETADAPTER Adapter,
    _Inout_ NETTXQUEUE_INIT *TxQueueInit)
{
    NET_PACKET_QUEUE_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NETPACKETQUEUE TxQueue;
    PLOOP_DEVICE LoopDevice = LoopGetAdapter(Adapter)->LoopDevice;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, LOOP_QUEUE);
    Attributes.EvtDestroyCallback = LoopEvtQueueDestroy;

    NET_PACKET_QUEUE_CONFIG_INIT(&Config, LoopEvtTxAdvance, LoopEvtTxSetNotificationEnabled, LoopEvtTxCancel);

    Status = NetTxQueueCreate(TxQueueInit, &Attributes, &Config, &TxQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    InterlockedIncrement(&LoopState.TxQueueCreates);
    LoopQueueInitialize(TxQueue, LoopDevice, TRUE);
    LoopDevice->TxQueue = TxQueue;
    return STATUS_SUCCESS;
}

static EVT_NET_ADAPTER_CREATE_RXQUEUE LoopEvtCreateRxQueue;

static
NTSTATUS
NTAPI
LoopEvtCreateRxQueue(
    _In_ NETADAPTER Adapter,
    _Inout_ NETRXQUEUE_INIT *RxQueueInit)
{
    NET_PACKET_QUEUE_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NETPACKETQUEUE RxQueue;
    PLOOP_DEVICE LoopDevice = LoopGetAdapter(Adapter)->LoopDevice;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, LOOP_QUEUE);
    Attributes.EvtDestroyCallback = LoopEvtQueueDestroy;

    NET_PACKET_QUEUE_CONFIG_INIT(&Config, LoopEvtRxAdvance, LoopEvtRxSetNotificationEnabled, LoopEvtRxCancel);
    Config.EvtStart = LoopEvtRxStart;
    Config.EvtStop = LoopEvtRxStop;

    Status = NetRxQueueCreate(RxQueueInit, &Attributes, &Config, &RxQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    InterlockedIncrement(&LoopState.RxQueueCreates);
    LoopQueueInitialize(RxQueue, LoopDevice, FALSE);
    return STATUS_SUCCESS;
}

#if NETADAPTER_VERSION_MINOR >= 1
static EVT_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM LoopEvtSetRxChecksum;

static
VOID
NTAPI
LoopEvtSetRxChecksum(
    _In_ NETADAPTER Adapter,
    _In_ NETOFFLOAD Offload)
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Offload);
}
#endif

static
NTSTATUS
LoopAdapterCreate(
    _In_ PLOOP_DEVICE LoopDevice)
{
    NET_ADAPTER_DATAPATH_CALLBACKS Datapath;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES LinkLayer;
    NET_ADAPTER_LINK_STATE LinkState;
    NET_ADAPTER_TX_CAPABILITIES TxCapabilities;
    NET_ADAPTER_RX_CAPABILITIES RxCapabilities;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NETCONFIGURATION Configuration;
    NETADAPTER_INIT *AdapterInit;
    NETADAPTER Adapter;
    NTSTATUS Status;

    AdapterInit = NetAdapterInitAllocate(LoopDevice->Device);
    if (AdapterInit == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&Datapath, LoopEvtCreateTxQueue, LoopEvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(AdapterInit, &Datapath);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, LOOP_ADAPTER);
    Status = NetAdapterCreate(AdapterInit, &Attributes, &Adapter);
    LoopState.AdapterCreateStatus = Status;
    NetAdapterInitFree(AdapterInit);
    if (!NT_SUCCESS(Status))
        return Status;

    LoopGetAdapter(Adapter)->LoopDevice = LoopDevice;
    LoopDevice->Adapter = Adapter;

    NET_ADAPTER_TX_CAPABILITIES_INIT(&TxCapabilities, 1);
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&RxCapabilities, LOOP_FRAME_MAX, 1);
    NetAdapterSetDataPathCapabilities(Adapter, &TxCapabilities, &RxCapabilities);

    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&LinkLayer, 1000000000ULL, 1000000000ULL);
    NetAdapterSetLinkLayerCapabilities(Adapter, &LinkLayer);
    NetAdapterSetLinkLayerMtuSize(Adapter, 1500);

#if NETADAPTER_VERSION_MINOR >= 1
    {
        NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES RxChecksum;
        NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES_INIT(&RxChecksum, LoopEvtSetRxChecksum);
        NetAdapterOffloadSetRxChecksumCapabilities(Adapter, &RxChecksum);
    }
#endif

    NET_ADAPTER_LINK_STATE_INIT(&LinkState, 1000000000ULL, MediaConnectStateConnected,
                                MediaDuplexStateFull, NetAdapterPauseFunctionTypeUnsupported,
                                NetAdapterAutoNegotiationFlagNone);
    NetAdapterSetLinkState(Adapter, &LinkState);

    /* An entry point the class extension does not implement yet */
    LoopState.StubStatus = NetAdapterOpenConfiguration(Adapter, WDF_NO_OBJECT_ATTRIBUTES, &Configuration);

    Status = NetAdapterStart(Adapter);
    LoopState.AdapterStartStatus = Status;
    return Status;
}

static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL LoopEvtIoDeviceControl;

static
VOID
NTAPI
LoopEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PLOOP_DEVICE LoopDevice = LoopGetDevice(WdfIoQueueGetDevice(Queue));
    PNETCXLOOP_STATE State;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_NETCXLOOP_GET_STATE:
            break;

        case IOCTL_NETCXLOOP_STOP_ADAPTER:
            /* The class extension halts the NDIS adapter and deletes the queues */
            NetAdapterStop(LoopDevice->Adapter);
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;

        case IOCTL_NETCXLOOP_START_ADAPTER:
            /* The adapter comes back with new queues */
            LoopState.AdapterRestartStatus = NetAdapterStart(LoopDevice->Adapter);
            WdfRequestComplete(Request, LoopState.AdapterRestartStatus);
            return;

        default:
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
            return;
    }

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*State), (PVOID *)&State, NULL);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    if (LoopDevice->Adapter != NULL)
        LoopState.NetLuid = NetAdapterGetNetLuid(LoopDevice->Adapter).Value;

    *State = LoopState;
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*State));
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP LoopEvtDeviceCleanup;

static
VOID
NTAPI
LoopEvtDeviceCleanup(
    _In_ WDFOBJECT Object)
{
    LoopFifoFlush(LoopGetDevice(Object));
}

static EVT_WDF_DRIVER_DEVICE_ADD LoopEvtDeviceAdd;

static
NTSTATUS
NTAPI
LoopEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    UNICODE_STRING SymbolicLink;
    WCHAR LinkBuffer[64];
    PLOOP_DEVICE LoopDevice;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    Status = NetDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(Status))
        goto Exit;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, LOOP_DEVICE);
    Attributes.EvtCleanupCallback = LoopEvtDeviceCleanup;

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
        goto Exit;

    LoopDevice = LoopGetDevice(Device);
    LoopDevice->Device = Device;
    KeInitializeSpinLock(&LoopDevice->FifoLock);
    InitializeListHead(&LoopDevice->Fifo);

    RtlInitEmptyUnicodeString(&SymbolicLink, LinkBuffer, sizeof(LinkBuffer));
    RtlUnicodeStringPrintf(&SymbolicLink, L"\\DosDevices\\KmtNetCxLoop2%u", NETADAPTER_VERSION_MINOR);
    Status = WdfDeviceCreateSymbolicLink(Device, &SymbolicLink);
    if (!NT_SUCCESS(Status))
        goto Exit;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = LoopEvtIoDeviceControl;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Status = LoopAdapterCreate(LoopDevice);

Exit:
    LoopState.DeviceAddStatus = Status;
    DbgPrint("NetCxLoop2%u: device add 0x%lx\n", NETADAPTER_VERSION_MINOR, Status);
    return Status;
}

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    ULONG i;

    LoopState.Size = sizeof(LoopState);
    LoopState.VersionMinor = NETADAPTER_VERSION_MINOR;
    LoopState.TableEntries = NetFunctionTableNumEntries;
    LoopState.GlobalsBound = (NetDriverGlobals != NULL);
    for (i = 0; i < NetFunctionTableNumEntries; i++)
    {
        if (NetFunctions[i] == NULL)
            LoopState.NullSlots++;
    }

    DbgPrint("NetCxLoop2%u: bound %lu, %lu/%lu slots empty\n", NETADAPTER_VERSION_MINOR,
             LoopState.GlobalsBound, LoopState.NullSlots, LoopState.TableEntries);

    WDF_DRIVER_CONFIG_INIT(&Config, LoopEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
