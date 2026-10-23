/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private declarations of netadaptercx.sys
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#include <ntddk.h>
#include <ndis.h>
#include <wdf.h>
#include <wdfcx.h>
#include <wdfldr.h>
#include <netadaptercx.h>
#include <net/virtualaddress.h>
#include <net/checksum.h>
#include <drivers/ndis/ndiswdf.h>

#ifndef WDF_VIOLATION
#define WDF_VIOLATION           ((ULONG)0x0000010DL)
#endif

/* NDIS 6.0, not declared by our ndis.h yet */
NDIS_STATUS
NTAPI
NdisOpenConfigurationEx(
    _In_ PNDIS_CONFIGURATION_OBJECT ConfigObject,
    _Out_ PNDIS_HANDLE ConfigurationHandle);

#define NX_TAG                  'xCtN'
#define NX_RING_TAG             'gRtN'
#define NX_RX_TAG               'xRtN'

/* NetAdapterCx versions this class extension serves */
#define NX_VERSION_MAJOR        2
#define NX_VERSION_MINOR        1
#define NX_TABLE_ENTRIES_2_0    75
#define NX_TABLE_ENTRIES_2_1    122

C_ASSERT(NetFunctionTableNumEntries == NX_TABLE_ENTRIES_2_1);

#define NX_GLOBALS_SIGNATURE    'IxNG'

typedef struct _NX_DEVICE NX_DEVICE, *PNX_DEVICE;
typedef struct _NX_ADAPTER NX_ADAPTER, *PNX_ADAPTER;
typedef struct _NX_QUEUE NX_QUEUE, *PNX_QUEUE;

/*
 * One per bound client driver. The client only sees Public, through its
 * NetDriverGlobals pointer; every API gets it back as its first argument.
 */
typedef struct _NX_PRIVATE_GLOBALS
{
    ULONG Signature;
    NET_DRIVER_GLOBALS Public;

    /* The client's WDF_DRIVER_GLOBALS; Driver is set once its DriverEntry
     * has called WdfDriverCreate */
    PWDF_DRIVER_GLOBALS ClientDriverGlobals;
    ULONG ClientMinor;

    /* NDIS miniport driver registration of the client, made on first use */
    FAST_MUTEX Lock;
    PDRIVER_OBJECT ClientDriverObject;
    NDIS_HANDLE MiniportDriverHandle;
} NX_PRIVATE_GLOBALS, *PNX_PRIVATE_GLOBALS;

FORCEINLINE
PNX_PRIVATE_GLOBALS
NxGetPrivateGlobals(
    _In_ PNET_DRIVER_GLOBALS Globals)
{
    PNX_PRIVATE_GLOBALS Private = CONTAINING_RECORD(Globals, NX_PRIVATE_GLOBALS, Public);

    if (Globals == NULL || Private->Signature != NX_GLOBALS_SIGNATURE)
    {
        KeBugCheckEx(WDF_VIOLATION, 0x4E4358 /* "NCX" */, (ULONG_PTR)Globals, 0, 0);
    }
    return Private;
}

/* Copies a versioned client structure: Size bytes at most, rest zeroed */
#define NX_COPY_SIZED(_Dst, _Src)                                              \
    do {                                                                       \
        ULONG _sz = (_Src)->Size;                                              \
        RtlZeroMemory((_Dst), sizeof(*(_Dst)));                                \
        if (_sz > sizeof(*(_Dst))) _sz = sizeof(*(_Dst));                      \
        RtlCopyMemory((_Dst), (_Src), _sz);                                    \
    } while (0)

/*
 * Per WDFDEVICE state. It is the context of a WDF object the class
 * extension creates as a child of the client's device, so it goes away
 * with the device.
 */
struct _NX_DEVICE
{
    LIST_ENTRY Link;
    WDFDEVICE Device;
    /* The object this is the context of; parent of the NETADAPTERs */
    WDFOBJECT Object;
    /* Lock order: device, then adapter. Mutexes keep IRQL at PASSIVE_LEVEL,
     * which the client callbacks made under them require. */
    KMUTEX Lock;
    LIST_ENTRY Adapters;
    /* Between EvtCxDevicePostSelfManagedIoInit and release of the hardware */
    BOOLEAN Started;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NX_DEVICE, NxDeviceGetObjectContext)

/* Opaque NETADAPTER_INIT of the client API */
typedef struct _NX_ADAPTER_INIT
{
    ULONG Signature;
    WDFDEVICE Device;
    NET_ADAPTER_DATAPATH_CALLBACKS DatapathCallbacks;
    NETADAPTER CreatedAdapter;
} NX_ADAPTER_INIT, *PNX_ADAPTER_INIT;

#define NX_ADAPTER_INIT_SIGNATURE 'IAxN'

/* NETOFFLOAD handed to the client's offload callbacks */
typedef struct _NX_OFFLOAD
{
    ULONG Signature;
    BOOLEAN IPv4;
    BOOLEAN Tcp;
    BOOLEAN Udp;
} NX_OFFLOAD, *PNX_OFFLOAD;

#define NX_OFFLOAD_SIGNATURE 'OfxN'

struct _NX_ADAPTER
{
    NETADAPTER Handle;
    WDFDEVICE Device;
    PNX_DEVICE NxDevice;
    PNX_PRIVATE_GLOBALS Globals;
    LIST_ENTRY DeviceLink;

    /* Serializes NDIS lifecycle and datapath start/stop */
    KMUTEX Lock;

    NET_ADAPTER_DATAPATH_CALLBACKS DatapathCallbacks;

    /* Capabilities the client reported */
    NET_ADAPTER_LINK_LAYER_CAPABILITIES LinkLayerCapabilities;
    ULONG MtuSize;
    NET_ADAPTER_TX_CAPABILITIES TxCapabilities;
    NET_ADAPTER_RX_CAPABILITIES RxCapabilities;
    BOOLEAN DatapathCapabilitiesSet;
    NET_ADAPTER_LINK_STATE LinkState;
    NET_ADAPTER_LINK_LAYER_ADDRESS PermanentAddress;
    NET_ADAPTER_LINK_LAYER_ADDRESS CurrentAddress;

    NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES ChecksumCapabilities;
    NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES TxChecksumCapabilities;
    NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES RxChecksumCapabilities;
    BOOLEAN ChecksumCapabilitiesSet;
    BOOLEAN TxChecksumCapabilitiesSet;
    BOOLEAN RxChecksumCapabilitiesSet;
    NX_OFFLOAD TxChecksumOffload;
    NX_OFFLOAD RxChecksumOffload;

    /* Lifecycle */
    BOOLEAN StartRequested;     /* NetAdapterStart called */
    BOOLEAN NdisAdded;          /* NdisWdfPnPAddDevice done */
    BOOLEAN NdisStarted;        /* NdisWdfActionPnpStart succeeded */
    BOOLEAN Initialized;        /* inside MiniportInitializeEx .. HaltEx */
    volatile LONG DatapathRunning;
    /* Held by send handlers while they use the queues */
    EX_RUNDOWN_REF DatapathRundown;
    NDIS_HANDLE NdisHandle;

    /* From the device's registry keywords */
    NDIS_MEDIUM Medium;
    NDIS_PHYSICAL_MEDIUM PhysicalMedium;
    NET_IFTYPE IfType;
    NET_IF_ACCESS_TYPE AccessType;
    NET_IF_DIRECTION_TYPE DirectionType;
    NET_IF_CONNECTION_TYPE ConnectionType;
    ULONG NetLuidIndex;

    ULONG PacketFilter;
    ULONG Lookahead;

    /* Datapath */
    PNX_QUEUE TxQueue;
    PNX_QUEUE RxQueue;
    NDIS_HANDLE RxNblPool;
    volatile LONG RxOutstanding;
    KEVENT RxIdle;

    /* Statistics */
    volatile LONG64 TxPackets;
    volatile LONG64 TxBytes;
    volatile LONG64 TxErrors;
    volatile LONG64 RxPackets;
    volatile LONG64 RxBytes;
    volatile LONG64 RxDropped;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NX_ADAPTER, NxAdapterGetContext)

typedef enum _NX_QUEUE_TYPE
{
    NxQueueTx,
    NxQueueRx,
} NX_QUEUE_TYPE;

/* Opaque NETTXQUEUE_INIT/NETRXQUEUE_INIT of the client API */
typedef struct _NX_QUEUE_INIT
{
    ULONG Signature;
    NX_QUEUE_TYPE Type;
    PNX_ADAPTER Adapter;
    ULONG QueueId;
    NETPACKETQUEUE CreatedQueue;
} NX_QUEUE_INIT, *PNX_QUEUE_INIT;

#define NX_QUEUE_INIT_SIGNATURE 'IQxN'

struct _NX_QUEUE
{
    NX_QUEUE_TYPE Type;
    ULONG QueueId;
    NETPACKETQUEUE Handle;
    PNX_ADAPTER Adapter;
    NET_PACKET_QUEUE_CONFIG Config;

    NET_RING_COLLECTION Rings;
    NET_RING *PacketRing;
    NET_RING *FragmentRing;

    /* Extensions, kept in arrays parallel to the rings */
    NET_FRAGMENT_VIRTUAL_ADDRESS *FragmentVirtualAddress;
    NET_PACKET_CHECKSUM *PacketChecksum;
    BOOLEAN ChecksumEnabled;

    /* Ring indices up to which the OS has taken back elements */
    UINT32 OsPacketBegin;
    UINT32 OsFragmentBegin;

    /* Tx: NET_BUFFER_LIST each posted packet belongs to */
    PNET_BUFFER_LIST *PacketNbl;
    /* Tx: send queue not yet on the ring */
    KSPIN_LOCK SendLock;
    PNET_BUFFER_LIST SendHead;
    PNET_BUFFER_LIST SendTail;
    /* Tx: next NET_BUFFER of SendHead to post */
    PNET_BUFFER SendNextNb;

    /* Rx: system managed fragment buffers, one per fragment ring element */
    PUCHAR RxBuffers;
    SIZE_T RxBufferSize;

    /* Execution context: one thread runs every callback of the queue */
    HANDLE ThreadHandle;
    PKTHREAD Thread;
    KEVENT Notify;
    /* Set by the thread once EvtStart returned */
    KEVENT ThreadStarted;
    volatile LONG StopRequested;
    BOOLEAN Started;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NX_QUEUE, NxQueueGetContext)

/* driver.c */
extern NDIS_HANDLE NxNdisCxHandle;

NTSTATUS
NxGetMiniportDriverHandle(
    _In_ PNX_PRIVATE_GLOBALS Globals,
    _In_ PDRIVER_OBJECT ClientDriverObject,
    _Out_ PNDIS_HANDLE MiniportDriverHandle);

VOID
NxFillFunctionTable(
    _Out_writes_(NetFunctionTableNumEntries) NETFUNC *Table);

#define NxLock(_Mutex) \
    KeWaitForSingleObject((_Mutex), Executive, KernelMode, FALSE, NULL)
#define NxUnlock(_Mutex) \
    KeReleaseMutex((_Mutex), FALSE)

/* device.c */
VOID
NxDeviceInitializeList(VOID);

/* Creates the state on first use */
PNX_DEVICE
NxDeviceGet(
    _In_ WDFDEVICE Device);

/* NULL if the class extension never saw the device start or an adapter */
PNX_DEVICE
NxDeviceFind(
    _In_ WDFDEVICE Device);

/* adapter.c */
VOID
NxAdapterStartNdis(
    _In_ PNX_ADAPTER Adapter);

VOID
NxAdapterStopNdis(
    _In_ PNX_ADAPTER Adapter);

VOID
NxAdapterRemoveNdis(
    _In_ PNX_ADAPTER Adapter);

/* queue.c */
NTSTATUS
NxDatapathStart(
    _In_ PNX_ADAPTER Adapter);

VOID
NxDatapathStop(
    _In_ PNX_ADAPTER Adapter);

VOID
NxQueueSend(
    _In_ PNX_QUEUE Queue,
    _In_ PNET_BUFFER_LIST NetBufferLists);

VOID
NxRxReturnNetBufferLists(
    _In_ PNX_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists);

/* miniport.c */
VOID
NxMiniportFillCharacteristics(
    _Out_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics);

VOID
NxMiniportIndicateLinkState(
    _In_ PNX_ADAPTER Adapter);

/* api.c: client entry points live in the export table; see driver.c */
