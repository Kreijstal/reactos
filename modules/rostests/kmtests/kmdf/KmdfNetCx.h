/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NetAdapterCx test declarations
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

/*
 * The NetAdapterCx test has three parts:
 *
 * - netcxloop20_drv/netcxloop21_drv: a KMDF NetAdapterCx 2.0/2.1 client
 *   driver for a root-enumerated device. Its adapter loops every packet
 *   it is given to transmit back as received. User mode reads its state
 *   through \\.\KmtNetCxLoop2x.
 * - netcxproto_drv: an NDIS 6 protocol driver that opens the loopback
 *   adapter, sends packets through it and checks what comes back.
 * - KmdfNetCx_user.c: installs the class extension and the device and
 *   drives the two drivers.
 */

/* \\.\KmtNetCxLoop20 and \\.\KmtNetCxLoop21 */
#define NETCXLOOP_DOS_NAME_FORMAT   L"\\\\.\\KmtNetCxLoop2%lu"

#define IOCTL_NETCXLOOP_GET_STATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)
/* NetAdapterStop, NetAdapterStart on the running device */
#define IOCTL_NETCXLOOP_STOP_ADAPTER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_NETCXLOOP_START_ADAPTER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef struct _NETCXLOOP_STATE
{
    ULONG Size;

    /* Bind */
    ULONG VersionMinor;
    ULONG TableEntries;
    ULONG NullSlots;
    ULONG GlobalsBound;

    /* Adapter */
    LONG DeviceAddStatus;
    LONG AdapterCreateStatus;
    LONG AdapterStartStatus;
    LONG AdapterRestartStatus;
    LONG StubStatus;            /* NetAdapterOpenConfiguration, a stub */
    ULONG64 NetLuid;
    ULONG MtuSize;

    /* Queues */
    LONG TxQueueCreates;
    LONG RxQueueCreates;
    LONG RxQueueStarts;
    LONG RxQueueStops;
    LONG TxCancels;
    LONG RxCancels;
    LONG QueueDestroys;
    LONG VirtualAddressExtension;
    LONG ChecksumExtension;
    LONG TxArmed;
    LONG RxArmed;

    /* Traffic */
    LONG64 TxPackets;
    LONG64 TxBytes;
    LONG64 TxMultiFragmentPackets;
    LONG64 RxPackets;
    LONG64 RxBytes;
    LONG64 Dropped;
} NETCXLOOP_STATE, *PNETCXLOOP_STATE;

/* netcxproto_drv */
#define IOCTL_NETCXPROTO_SET_ADAPTER    1
#define IOCTL_NETCXPROTO_RUN            2

#define NETCXPROTO_SERVICE_NAME         L"NetCxProto"

/* Raw IP; the adapter's *MediaType keyword selects it */
#define NETCX_TEST_MEDIUM               19

#define NETCX_SERVICE_NAME              L"NetAdapterCx"
#define NETCX_IMAGE_NAME                L"netadaptercx.sys"
