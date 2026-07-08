/*
 * PROJECT:     ReactOS VirtIO MMIO transport driver
 * LICENSE:     GPL-2.0+
 */

#pragma once

#include <ntddk.h>

#define VIRTIO_MMIO_TAG 'moiV'

#define VIRTIO_MMIO_MAGIC_VALUE        0x000
#define VIRTIO_MMIO_VERSION            0x004
#define VIRTIO_MMIO_DEVICE_ID          0x008
#define VIRTIO_MMIO_VENDOR_ID          0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES    0x010
#define VIRTIO_MMIO_DRIVER_FEATURES    0x020
#define VIRTIO_MMIO_QUEUE_SEL          0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX      0x034
#define VIRTIO_MMIO_QUEUE_NUM          0x038
#define VIRTIO_MMIO_QUEUE_ALIGN        0x03c
#define VIRTIO_MMIO_QUEUE_PFN          0x040
#define VIRTIO_MMIO_QUEUE_READY        0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064
#define VIRTIO_MMIO_STATUS             0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW     0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH    0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW    0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH   0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW     0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH    0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION  0x0fc
#define VIRTIO_MMIO_CONFIG             0x100

#define VIRTIO_MMIO_MAGIC              0x74726976 /* "virt" */

#define VIRTIO_MMIO_BLOCK_DEVICE_ID    2

typedef enum _VIRTIO_MMIO_DEVICE_TYPE
{
    VirtioMmioFdo,
    VirtioMmioPdo
} VIRTIO_MMIO_DEVICE_TYPE;

typedef enum _VIRTIO_MMIO_STATE
{
    VirtioMmioNotStarted,
    VirtioMmioStarted,
    VirtioMmioRemoved
} VIRTIO_MMIO_STATE;

typedef struct _VIRTIO_MMIO_COMMON_EXTENSION
{
    VIRTIO_MMIO_DEVICE_TYPE Type;
    PDEVICE_OBJECT Self;
} VIRTIO_MMIO_COMMON_EXTENSION, *PVIRTIO_MMIO_COMMON_EXTENSION;

typedef struct _VIRTIO_MMIO_PDO_EXTENSION
{
    VIRTIO_MMIO_COMMON_EXTENSION Common;
    PDEVICE_OBJECT ParentFdo;
    BOOLEAN Present;
    BOOLEAN Reported;
    ULONG DeviceId;
    ULONG VendorId;
    ULONG Version;
    PHYSICAL_ADDRESS RegistersPa;
    ULONG RegistersLength;
    ULONG RawInterruptVector;
    ULONG RawInterruptLevel;
    KAFFINITY RawInterruptAffinity;
    ULONG InterruptVector;
    KIRQL InterruptLevel;
    KAFFINITY InterruptAffinity;
} VIRTIO_MMIO_PDO_EXTENSION, *PVIRTIO_MMIO_PDO_EXTENSION;

typedef struct _VIRTIO_MMIO_EXTENSION
{
    VIRTIO_MMIO_COMMON_EXTENSION Common;
    PDEVICE_OBJECT Pdo;
    PDEVICE_OBJECT LowerDevice;
    IO_REMOVE_LOCK RemoveLock;
    VIRTIO_MMIO_STATE State;
    PDEVICE_OBJECT ChildPdo;
    PUCHAR Registers;
    PHYSICAL_ADDRESS RegistersPa;
    ULONG RegistersLength;
    ULONG RawInterruptVector;
    ULONG RawInterruptLevel;
    KAFFINITY RawInterruptAffinity;
    ULONG InterruptVector;
    KIRQL InterruptLevel;
    KAFFINITY InterruptAffinity;
    ULONG Magic;
    ULONG Version;
    ULONG DeviceId;
    ULONG VendorId;
} VIRTIO_MMIO_EXTENSION, *PVIRTIO_MMIO_EXTENSION;
