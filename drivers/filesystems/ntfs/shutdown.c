/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/shutdown.c
 * PURPOSE:          NTFS filesystem driver - IRP_MJ_SHUTDOWN handler
 *
 * Without this handler the I/O Manager's IopShutdownBaseFileSystems pass
 * sees STATUS_INVALID_DEVICE_REQUEST for NTFS volumes, which means dirty
 * cached writes that the lazy writer hasn't picked up yet are silently
 * lost. The most visible symptom is that registry hive writes performed
 * late in setup or in winlogon's first-boot path (e.g. the per-SID
 * ProfileList\<SID> entries written by CreateUserProfileExW) never reach
 * disk, leaving the user permanently unable to log in. fastfat solves
 * the same problem in fastfat/shutdown.c by flushing every mounted VCB
 * before completing the shutdown IRP.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * Push the whole volume-stream cache (cached $MFT / $Bitmap / index metadata)
 * to disk, retrying until it drains cleanly.
 *
 * CcFlushCache walks the volume-stream VACBs from offset 0 to the cached
 * volume size and STOPS at the first VACB whose flush fails, returning that
 * status and leaving every later dirty page un-written.  A single transient
 * paging-write failure would therefore leave the tail of $MFT / $Bitmap dirty
 * in the cache; if the mount is then torn down (or the box powers off), those
 * updates are lost and the on-disk $MFT and $Bitmap are persisted at
 * inconsistent versions - the classic cross-linked / leaked-cluster corruption
 * that a later mount trips over.  Retry the flush until it reports success so
 * the whole stream reaches disk.
 */
static
VOID
NtfsFlushVolumeStream(PNTFS_VCB Vcb)
{
    IO_STATUS_BLOCK IoStatus;
    ULONG Retry = 0;

    if (Vcb->StreamFileObject == NULL ||
        Vcb->StreamFileObject->SectionObjectPointer == NULL)
        return;

    do
    {
        IoStatus.Status = STATUS_SUCCESS;
        CcFlushCache(Vcb->StreamFileObject->SectionObjectPointer,
                     NULL, 0, &IoStatus);
        if (NT_SUCCESS(IoStatus.Status))
            return;

        DPRINT1("NtfsFlushVolumeStream: volume-stream flush failed (0x%08lx), retry %lu\n",
                IoStatus.Status, Retry + 1);
    }
    while (++Retry < 100);
}

/*
 * Flush every cached page on a single mounted volume to disk.
 *
 * Snapshots the VCB's FcbListHead under FcbListLock (a spinlock) into
 * an array, releases the lock, then calls CcFlushCache on each FCB's
 * section. Also flushes the volume stream file object so that any data
 * that lives in the volume-level cache map (the cached MFT/index VACBs
 * used by NtfsRead/WriteDiskCached) hits disk.
 *
 * The snapshot-then-flush pattern is required because CcFlushCache can
 * block - it acquires the section's resource and may wait on paging
 * I/O - so it cannot be called at DISPATCH_LEVEL while the spinlock is
 * held. Each snapshotted FCB has its RefCount bumped under the lock so a
 * concurrent NtfsReleaseFCB cannot free it while we are flushing it, and
 * the bump is undone afterwards with a matching raw decrement (NOT via
 * NtfsReleaseFCB - see the third pass for why evicting here is harmful).
 */
static
VOID
NtfsFlushVolume(PNTFS_VCB Vcb)
{
    KIRQL OldIrql;
    PLIST_ENTRY Entry;
    PNTFS_FCB Fcb;
    PNTFS_FCB *Snapshot = NULL;
    ULONG SnapshotCount = 0;
    ULONG SnapshotCapacity = 0;
    ULONG i;
    IO_STATUS_BLOCK IoStatus;

    /* First pass: count the open FCBs so we can allocate a buffer of
     * the right size in one shot. */
    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);
    for (Entry = Vcb->FcbListHead.Flink;
         Entry != &Vcb->FcbListHead;
         Entry = Entry->Flink)
    {
        SnapshotCapacity++;
    }
    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);

    if (SnapshotCapacity == 0)
    {
        /* No open FCBs, but the volume-stream cache may still hold dirty
         * metadata (e.g. the formatter wrote $MFT/$Bitmap through it). */
        NtfsFlushVolumeStream(Vcb);
        return;
    }

    Snapshot = ExAllocatePoolWithTag(NonPagedPool,
                                     SnapshotCapacity * sizeof(PNTFS_FCB),
                                     TAG_FCB);
    if (Snapshot == NULL)
    {
        DPRINT1("NtfsFlushVolume: out of memory snapshotting %lu FCBs\n",
                SnapshotCapacity);
        NtfsFlushVolumeStream(Vcb);
        return;
    }

    /* Second pass: re-walk under the lock and bump RefCount on each FCB
     * we capture. The capacity may have grown since the first pass; if
     * so we just stop at SnapshotCapacity and miss the new entries -
     * they belong to in-flight IRPs that the I/O Manager will quiesce
     * before phase 2 of shutdown anyway. RefCount is normally bumped
     * via NtfsGrabFCB, which itself takes FcbListLock; we can't call it
     * here because we already hold the lock, so we increment directly. */
    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);
    for (Entry = Vcb->FcbListHead.Flink;
         Entry != &Vcb->FcbListHead && SnapshotCount < SnapshotCapacity;
         Entry = Entry->Flink)
    {
        Fcb = CONTAINING_RECORD(Entry, NTFS_FCB, FcbListEntry);
        Fcb->RefCount++;
        Snapshot[SnapshotCount++] = Fcb;
    }
    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);

    /* Third pass: flush each FCB outside the lock.  We deliberately do NOT
     * drop these references via NtfsReleaseFCB: that routine's behaviour is
     * keyed on the resulting RefCount value, and a cached, closed file FCB
     * sits at RefCount == 1 (NtfsMakeFCBFromDirEntry leaves it there with its
     * cache map live).  Our pass-2 snapshot bumped it to 2, so NtfsReleaseFCB
     * would bring it back to 1 and fire the cache-map teardown branch
     * (CcFlushCache + CcPurgeCacheSection + CcUninitializeCacheMap, NULLing
     * FileObject).  Tearing a cache map down during the final shutdown flush
     * re-enters NTFS write-back - it reacquires the FCB PagingIoResource and
     * issues synchronous disk writes against an already-quiescing storage
     * stack, which can wedge shutdown - and it drops the last data-section
     * reference at a point where MmDereferenceSegmentWithLock can re-enter
     * the page-out path.  Shutdown must *flush*, not *evict*: push the dirty
     * pages and leave eviction to volume dismount. */
    for (i = 0; i < SnapshotCount; i++)
    {
        Fcb = Snapshot[i];

        if (Fcb->SectionObjectPointers != NULL &&
            (Fcb->SectionObjectPointers->DataSectionObject != NULL ||
             Fcb->SectionObjectPointers->SharedCacheMap != NULL))
        {
            CcFlushCache(Fcb->SectionObjectPointers,
                         NULL,
                         0,
                         &IoStatus);
        }
    }

    /* Flush the volume-stream metadata AFTER all FCB data has been pushed:
     * completing the file-data writes can advance each file's ValidDataLength
     * and rewrite its $MFT record / $Bitmap runs, which dirties the volume
     * stream again.  Flushing metadata last (and retrying until it drains)
     * guarantees the on-disk $MFT and $Bitmap match the final in-memory state,
     * so no cross-links or leaked clusters survive a clean stop. */
    NtfsFlushVolumeStream(Vcb);

    /* Drop the snapshot references with a raw decrement that exactly mirrors
     * the raw increment taken in pass 2, under the same lock.  This is a true
     * no-op on the FCB lifecycle and cannot trip the value-keyed cache
     * teardown or NtfsDestroyFCB paths inside NtfsReleaseFCB. */
    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);
    for (i = 0; i < SnapshotCount; i++)
        Snapshot[i]->RefCount--;
    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);

    ExFreePoolWithTag(Snapshot, TAG_FCB);
}

/*
 * Forward IRP_MJ_SHUTDOWN to the underlying storage device.
 *
 * This is best-effort: it gives the disk driver a chance to flush its
 * own write cache. fastfat does this between flushing each VCB.
 */
static
VOID
NtfsShutdownStorageDevice(PDEVICE_OBJECT StorageDevice)
{
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIRP Irp;

    if (StorageDevice == NULL)
        return;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_SHUTDOWN,
                                       StorageDevice,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &IoStatus);
    if (Irp == NULL)
        return;

    if (IoCallDriver(StorageDevice, Irp) == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }
}

/*
 * IRP_MJ_SHUTDOWN dispatch entry. Called once on the NTFS control device
 * during phase 1 of IoShutdownSystem (after CmShutdownSystem has had a
 * chance to push hive writes through CcCopyWrite, but before the disk
 * drivers themselves are torn down).
 *
 * The I/O Manager only delivers a single shutdown IRP to the file system
 * control device, not one per mounted volume - so we have to walk every
 * device the NTFS driver owns and flush each VCB explicitly. The driver
 * doesn't keep a separate VCB list, but DriverObject->DeviceObject is the
 * head of a singly linked list of every IoCreateDevice that this driver
 * has performed (the kernel maintains it via DEVICE_OBJECT::NextDevice).
 * Each volume that NtfsMountVolume creates lands on that list with its
 * DeviceExtension typed as NTFS_TYPE_VCB; the control device that was
 * created in DriverEntry is also on the list with NTFS_TYPE_GLOBAL_DATA.
 */
NTSTATUS
NtfsShutdown(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT DeviceObject;
    PNTFS_VCB Vcb;

    DPRINT1("NtfsShutdown()\n");

    /* Iterate every device this driver owns. */
    DeviceObject = NtfsGlobalData->DriverObject->DeviceObject;
    while (DeviceObject != NULL)
    {
        /* Skip the control device - its extension is NTFS_GLOBAL_DATA, not a VCB. */
        if (DeviceObject != NtfsGlobalData->DeviceObject &&
            DeviceObject->DeviceExtension != NULL)
        {
            Vcb = (PNTFS_VCB)DeviceObject->DeviceExtension;
            if (Vcb->Identifier.Type == NTFS_TYPE_VCB)
            {
                NtfsFlushVolume(Vcb);
                NtfsShutdownStorageDevice(Vcb->StorageDevice);
            }
        }

        DeviceObject = DeviceObject->NextDevice;
    }

    /* IoUnregisterFileSystem on the control device tells the I/O Manager
     * we're not going to mount anything else. fastfat does this in its
     * common shutdown routine. */
    IoUnregisterFileSystem(NtfsGlobalData->DeviceObject);

    IrpContext->Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

/* EOF */
