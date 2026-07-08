/*
 * VirtIO MMIO transport support
 */

#include "osdep.h"
#include "virtio_pci.h"
#include "VirtIO.h"
#include "kdebugprint.h"
#include "virtio_ring.h"
#include "virtio_pci_common.h"
#include "windows/virtio_ring_allocation.h"

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
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064
#define VIRTIO_MMIO_STATUS             0x070
#define VIRTIO_MMIO_CONFIG_GENERATION  0x0fc
#define VIRTIO_MMIO_CONFIG             0x100

#define VIRTIO_MMIO_MAGIC              0x74726976
#define VIRTIO_MMIO_VRING_ALIGN        4096
#define VIRTIO_MMIO_QUEUE_ADDR_SHIFT   12

static void
vio_mmio_get_config(
    VirtIODevice *vdev,
    unsigned offset,
    void *buf,
    unsigned len)
{
    ULONG_PTR ioaddr = vdev->addr + VIRTIO_MMIO_CONFIG + offset;
    u8 *ptr = buf;
    unsigned i;

    for (i = 0; i < len; i++) {
        ptr[i] = ioread8(vdev, ioaddr + i);
    }
}

static void
vio_mmio_set_config(
    VirtIODevice *vdev,
    unsigned offset,
    const void *buf,
    unsigned len)
{
    ULONG_PTR ioaddr = vdev->addr + VIRTIO_MMIO_CONFIG + offset;
    const u8 *ptr = buf;
    unsigned i;

    for (i = 0; i < len; i++) {
        iowrite8(vdev, ptr[i], ioaddr + i);
    }
}

static u32
vio_mmio_get_generation(
    VirtIODevice *vdev)
{
    return ioread32(vdev, vdev->addr + VIRTIO_MMIO_CONFIG_GENERATION);
}

static u8
vio_mmio_get_status(
    VirtIODevice *vdev)
{
    return (u8)ioread32(vdev, vdev->addr + VIRTIO_MMIO_STATUS);
}

static void
vio_mmio_set_status(
    VirtIODevice *vdev,
    u8 status)
{
    iowrite32(vdev, status, vdev->addr + VIRTIO_MMIO_STATUS);
}

static void
vio_mmio_reset(
    VirtIODevice *vdev)
{
    iowrite32(vdev, 0, vdev->addr + VIRTIO_MMIO_STATUS);
    (void)ioread32(vdev, vdev->addr + VIRTIO_MMIO_STATUS);
}

static u64
vio_mmio_get_features(
    VirtIODevice *vdev)
{
    return ioread32(vdev, vdev->addr + VIRTIO_MMIO_DEVICE_FEATURES);
}

static NTSTATUS
vio_mmio_set_features(
    VirtIODevice *vdev,
    u64 features)
{
    vring_transport_features(vdev, &features);
    ASSERT((u32)features == features);
    iowrite32(vdev, (u32)features, vdev->addr + VIRTIO_MMIO_DRIVER_FEATURES);
    return STATUS_SUCCESS;
}

static u16
vio_mmio_set_config_vector(
    VirtIODevice *vdev,
    u16 vector)
{
    UNREFERENCED_PARAMETER(vdev);
    UNREFERENCED_PARAMETER(vector);
    return VIRTIO_MSI_NO_VECTOR;
}

static u16
vio_mmio_set_queue_vector(
    struct virtqueue *vq,
    u16 vector)
{
    UNREFERENCED_PARAMETER(vq);
    UNREFERENCED_PARAMETER(vector);
    return VIRTIO_MSI_NO_VECTOR;
}

static NTSTATUS
vio_mmio_query_vq_alloc(
    VirtIODevice *vdev,
    unsigned index,
    unsigned short *pNumEntries,
    unsigned long *pRingSize,
    unsigned long *pHeapSize)
{
    unsigned long ring_size, data_size;
    u32 num;

    iowrite32(vdev, index, vdev->addr + VIRTIO_MMIO_QUEUE_SEL);
    num = ioread32(vdev, vdev->addr + VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (!num || ioread32(vdev, vdev->addr + VIRTIO_MMIO_QUEUE_PFN)) {
        return STATUS_NOT_FOUND;
    }

    ring_size = ROUND_TO_PAGES(vring_size((u16)num, VIRTIO_MMIO_VRING_ALIGN, false));
    data_size = ROUND_TO_PAGES(vring_control_block_size((u16)num, false));

    *pNumEntries = (u16)num;
    *pRingSize = ring_size + data_size;
    *pHeapSize = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
vio_mmio_setup_vq(
    struct virtqueue **queue,
    VirtIODevice *vdev,
    VirtIOQueueInfo *info,
    unsigned index,
    u16 msix_vec)
{
    struct virtqueue *vq;
    unsigned long ring_size, heap_size;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(msix_vec);

    status = vio_mmio_query_vq_alloc(vdev, index, &info->num, &ring_size, &heap_size);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    info->queue = mem_alloc_contiguous_pages(vdev, ring_size);
    if (info->queue == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    iowrite32(vdev, index, vdev->addr + VIRTIO_MMIO_QUEUE_SEL);
    iowrite32(vdev, info->num, vdev->addr + VIRTIO_MMIO_QUEUE_NUM);
    iowrite32(vdev, VIRTIO_MMIO_VRING_ALIGN, vdev->addr + VIRTIO_MMIO_QUEUE_ALIGN);
    iowrite32(vdev,
              (u32)(mem_get_physical_address(vdev, info->queue) >> VIRTIO_MMIO_QUEUE_ADDR_SHIFT),
              vdev->addr + VIRTIO_MMIO_QUEUE_PFN);

    vq = vring_new_virtqueue_split(index, info->num,
        VIRTIO_MMIO_VRING_ALIGN, vdev,
        info->queue, vp_notify,
        (u8 *)info->queue + ROUND_TO_PAGES(vring_size(info->num, VIRTIO_MMIO_VRING_ALIGN, false)));
    if (!vq) {
        iowrite32(vdev, 0, vdev->addr + VIRTIO_MMIO_QUEUE_PFN);
        mem_free_contiguous_pages(vdev, info->queue);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    vq->notification_addr = (void *)(vdev->addr + VIRTIO_MMIO_QUEUE_NOTIFY);
    *queue = vq;
    return STATUS_SUCCESS;
}

static void
vio_mmio_delete_vq(
    VirtIOQueueInfo *info)
{
    struct virtqueue *vq = info->vq;
    VirtIODevice *vdev = vq->vdev;

    iowrite32(vdev, vq->index, vdev->addr + VIRTIO_MMIO_QUEUE_SEL);
    iowrite32(vdev, 0, vdev->addr + VIRTIO_MMIO_QUEUE_PFN);
    mem_free_contiguous_pages(vdev, info->queue);
}

static const struct virtio_device_ops virtio_mmio_device_ops = {
    /* .get_config = */ vio_mmio_get_config,
    /* .set_config = */ vio_mmio_set_config,
    /* .get_config_generation = */ vio_mmio_get_generation,
    /* .get_status = */ vio_mmio_get_status,
    /* .set_status = */ vio_mmio_set_status,
    /* .reset = */ vio_mmio_reset,
    /* .get_features = */ vio_mmio_get_features,
    /* .set_features = */ vio_mmio_set_features,
    /* .set_config_vector = */ vio_mmio_set_config_vector,
    /* .set_queue_vector = */ vio_mmio_set_queue_vector,
    /* .query_queue_alloc = */ vio_mmio_query_vq_alloc,
    /* .setup_queue = */ vio_mmio_setup_vq,
    /* .delete_queue = */ vio_mmio_delete_vq,
};

NTSTATUS
virtio_mmio_device_initialize(
    VirtIODevice *vdev,
    const VirtIOSystemOps *pSystemOps,
    void *DeviceContext)
{
    size_t length;
    u32 magic, version, device_id;

    RtlZeroMemory(vdev, sizeof(VirtIODevice));
    vdev->DeviceContext = DeviceContext;
    vdev->system = pSystemOps;
    vdev->info = vdev->inline_info;
    vdev->maxQueues = ARRAYSIZE(vdev->inline_info);
    vdev->mmio_used = true;

    length = pci_get_resource_len(vdev, 0);
    vdev->addr = (ULONG_PTR)pci_map_address_range(vdev, 0, 0, length);
    if (!vdev->addr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    magic = ioread32(vdev, vdev->addr + VIRTIO_MMIO_MAGIC_VALUE);
    version = ioread32(vdev, vdev->addr + VIRTIO_MMIO_VERSION);
    device_id = ioread32(vdev, vdev->addr + VIRTIO_MMIO_DEVICE_ID);

    if (magic != VIRTIO_MMIO_MAGIC || version != 1 || device_id == 0) {
        DPrintf(0, "virtio-mmio: unsupported magic/version/device %x/%u/%u\n",
                magic, version, device_id);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    vdev->isr = (u8 *)(vdev->addr + VIRTIO_MMIO_INTERRUPT_STATUS);
    vdev->mmio_interrupt_ack = (u32 *)(vdev->addr + VIRTIO_MMIO_INTERRUPT_ACK);
    vdev->device = &virtio_mmio_device_ops;

    virtio_device_reset(vdev);
    virtio_add_status(vdev, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    virtio_add_status(vdev, VIRTIO_CONFIG_S_DRIVER);

    return STATUS_SUCCESS;
}
