/*
 *  ReactOS kernel
 *  Copyright (C) 2002 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/blockdev.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Trevor Thompson
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
 * @brief Read disk data through the volume cache manager.
 *
 * Uses CcMapData on the volume stream to avoid building synchronous IRPs.
 * This prevents deadlocks when called from page fault or section flush
 * contexts where kernel APCs are disabled.
 *
 * Only caches reads within the first 128MB of the volume (MFT zone) to
 * avoid paged pool exhaustion from accumulated BCBs during large installs.
 *
 * Falls back to NtfsReadDisk if the cache is not initialized.
 */
#define NTFS_MAX_CACHED_OFFSET (128 * 1024 * 1024LL)
#define NTFS_CACHED_VACB_COUNT ((ULONG)(NTFS_MAX_CACHED_OFFSET / VACB_MAPPING_GRANULARITY))
C_ASSERT(NTFS_CACHED_VACB_COUNT == 512);
C_ASSERT(sizeof(((PDEVICE_EXTENSION)0)->CachedVacbBitmap) * 8 >= NTFS_CACHED_VACB_COUNT);

/* Per-thread sentinel: while a NtfsReadDiskCached invocation is mid-flight
 * with CcMapData on the volume stream, we set TopLevelIrp to the address of
 * this static variable.  A nested NtfsReadDiskCached on the same thread
 * recognizes the sentinel and falls back to NtfsReadDisk, guaranteeing a
 * recursion depth of at most 2 even if Cc faults trigger volume-stream
 * paging IRPs that re-enter the filesystem.
 *
 * The variable's address is distinct from any real PIRP and from every
 * FSRTL_*_TOP_LEVEL_IRP sentinel (which all sit at low integer ULONG_PTRs
 * <= 0x04), so the test is unambiguous. */
static UCHAR NtfsCachedReadSentinel;
#define NTFS_CACHED_READ_SENTINEL ((PIRP)&NtfsCachedReadSentinel)

/* Mark a VACB index (0..NTFS_CACHED_VACB_COUNT-1) as populated.
 * Safe to call concurrently — uses an atomic OR. */
FORCEINLINE
VOID
NtfsMarkCachedVacb(IN PDEVICE_EXTENSION Vcb, IN ULONG VacbIndex)
{
    ULONG Word = VacbIndex / 32;
    ULONG Bit = 1UL << (VacbIndex & 31);
    if ((Vcb->CachedVacbBitmap[Word] & Bit) == 0)
    {
        InterlockedOr((volatile LONG *)&Vcb->CachedVacbBitmap[Word], (LONG)Bit);
    }
}

/* Check whether any VACB in [FirstVacb..LastVacb] (inclusive) is marked. */
FORCEINLINE
BOOLEAN
NtfsAnyCachedVacbInRange(IN PDEVICE_EXTENSION Vcb,
                        IN ULONG FirstVacb,
                        IN ULONG LastVacb)
{
    ULONG FirstWord = FirstVacb / 32;
    ULONG LastWord = LastVacb / 32;
    ULONG Word;
    ULONG Mask;

    if (FirstWord == LastWord)
    {
        Mask = ((LastVacb - FirstVacb == 31) ? (ULONG)-1
                : (((1UL << (LastVacb - FirstVacb + 1)) - 1) << (FirstVacb & 31)));
        return (Vcb->CachedVacbBitmap[FirstWord] & Mask) != 0;
    }

    /* First partial word */
    Mask = (ULONG)-1 << (FirstVacb & 31);
    if (Vcb->CachedVacbBitmap[FirstWord] & Mask)
        return TRUE;

    /* Full middle words */
    for (Word = FirstWord + 1; Word < LastWord; Word++)
    {
        if (Vcb->CachedVacbBitmap[Word] != 0)
            return TRUE;
    }

    /* Last partial word */
    Mask = ((LastVacb & 31) == 31) ? (ULONG)-1
           : ((1UL << ((LastVacb & 31) + 1)) - 1);
    return (Vcb->CachedVacbBitmap[LastWord] & Mask) != 0;
}

NTSTATUS
NtfsReadDiskCached(IN PDEVICE_EXTENSION Vcb,
                   IN LONGLONG StartingOffset,
                   IN ULONG Length,
                   IN OUT PUCHAR Buffer)
{
    PVOID BcbResult;
    PVOID DataBuffer;
    LARGE_INTEGER VolumeOffset;
    BOOLEAN Mapped;
    ULONG ChunkLength;
    ULONG VacbOffset;
    PIRP SavedTopLevelIrp;

    /* Bypass conditions:
     *   - cache not yet initialized on the volume stream
     *   - we are already inside our own CcMapData (sentinel set below)
     *   - read crosses the cached region (avoid unbounded BCB growth)
     *
     * The TopLevelIrp == NULL test that used to live here was an over-broad
     * recursion guard: it fired on every IRP-driven read (NTFS dispatch sets
     * TopLevelIrp at entry), effectively disabling Cc for all MFT / run-list
     * lookups.  The real concern — re-entering ourselves through a Cc page
     * fault on the volume stream — is now handled by the explicit sentinel
     * below.  Page-fault paths into NtfsRead for FCB_IS_VOLUME_STREAM short-
     * circuit to NtfsReadDisk in rw.c, so the recursion chain has depth ≤ 2. */
    if (Vcb->StreamFileObject == NULL ||
        Vcb->StreamFileObject->PrivateCacheMap == NULL ||
        IoGetTopLevelIrp() == NTFS_CACHED_READ_SENTINEL ||
        StartingOffset + Length > NTFS_MAX_CACHED_OFFSET)
    {
        return NtfsReadDisk(Vcb->StorageDevice,
                            StartingOffset,
                            Length,
                            Vcb->NtfsInfo.BytesPerSector,
                            Buffer,
                            FALSE);
    }

    /* Mark this thread as inside a cached read so any reentrant call to
     * NtfsReadDiskCached (e.g. from a page fault Cc triggers while
     * resolving the volume-stream mapping) falls back to direct I/O. */
    SavedTopLevelIrp = IoGetTopLevelIrp();
    IoSetTopLevelIrp(NTFS_CACHED_READ_SENTINEL);

    /* CcMapData cannot map across VACB boundaries (256KB).
       Split the read into chunks that each stay within one VACB. */
    while (Length > 0)
    {
        ULONG VacbIndex;

        VolumeOffset.QuadPart = StartingOffset;
        VacbOffset = (ULONG)(StartingOffset % VACB_MAPPING_GRANULARITY);
        ChunkLength = min(Length, VACB_MAPPING_GRANULARITY - VacbOffset);

        /* Mark this VACB as populated BEFORE CcMapData runs, so a
         * concurrent NtfsWriteDiskCached observes it and performs the
         * purge rather than skipping. Over-marking is harmless — it
         * just costs an extra purge call. */
        VacbIndex = (ULONG)(StartingOffset / VACB_MAPPING_GRANULARITY);
        if (VacbIndex < NTFS_CACHED_VACB_COUNT)
            NtfsMarkCachedVacb(Vcb, VacbIndex);

        Mapped = FALSE;
        _SEH2_TRY
        {
            Mapped = CcMapData(Vcb->StreamFileObject,
                               &VolumeOffset,
                               ChunkLength,
                               MAP_WAIT,
                               &BcbResult,
                               &DataBuffer);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Mapped = FALSE;
        }
        _SEH2_END;

        if (!Mapped)
        {
            IoSetTopLevelIrp(SavedTopLevelIrp);
            return NtfsReadDisk(Vcb->StorageDevice,
                                StartingOffset,
                                Length,
                                Vcb->NtfsInfo.BytesPerSector,
                                Buffer,
                                FALSE);
        }

        RtlCopyMemory(Buffer, DataBuffer, ChunkLength);
        CcUnpinData(BcbResult);

        Buffer += ChunkLength;
        StartingOffset += ChunkLength;
        Length -= ChunkLength;
    }

    IoSetTopLevelIrp(SavedTopLevelIrp);
    return STATUS_SUCCESS;
}

/**
 * @brief Write disk data and keep the volume stream cache coherent.
 *
 * Wraps NtfsWriteDisk: after writing directly to the storage device,
 * purges the corresponding byte range from the volume stream cache so
 * that subsequent NtfsReadDiskCached calls return the fresh data.
 *
 * CcPurgeCacheSection is expensive on ReactOS — it takes the global
 * LockQueueMasterLock spinlock, walks the VACB list, and then calls
 * MmPurgeSegment which walks the section's pages. To avoid paying that
 * cost on every metadata write during install we track a per-VACB
 * bitmap of regions actually mapped via CcMapData and skip the purge
 * for any write whose VACBs are all unmarked. Setup writes heavily to
 * the MFT (offset ~3 MB on small volumes), so the bitmap tends to
 * stay sparse and most writes get the fast path.
 */
NTSTATUS
NtfsWriteDiskCached(IN PDEVICE_EXTENSION Vcb,
                    IN LONGLONG StartingOffset,
                    IN ULONG Length,
                    IN const PUCHAR Buffer)
{
    NTSTATUS Status;
    LARGE_INTEGER PurgeOffset;
    LARGE_INTEGER PurgeLength;
    LONGLONG EndOffset;
    ULONG FirstVacb;
    ULONG LastVacb;

    Status = NtfsWriteDisk(Vcb->StorageDevice,
                           StartingOffset,
                           Length,
                           Vcb->NtfsInfo.BytesPerSector,
                           Buffer);

    if (!NT_SUCCESS(Status))
        return Status;

    if (Vcb->StreamFileObject == NULL ||
        Vcb->StreamFileObject->SectionObjectPointer == NULL ||
        Vcb->StreamFileObject->SectionObjectPointer->SharedCacheMap == NULL ||
        StartingOffset >= NTFS_MAX_CACHED_OFFSET)
    {
        return Status;
    }

    /* Compute the VACB range for the written bytes, clamped to the
     * cached region. */
    EndOffset = StartingOffset + (LONGLONG)Length;
    if (EndOffset > NTFS_MAX_CACHED_OFFSET)
        EndOffset = NTFS_MAX_CACHED_OFFSET;

    FirstVacb = (ULONG)(StartingOffset / VACB_MAPPING_GRANULARITY);
    LastVacb = (ULONG)((EndOffset - 1) / VACB_MAPPING_GRANULARITY);
    if (LastVacb >= NTFS_CACHED_VACB_COUNT)
        LastVacb = NTFS_CACHED_VACB_COUNT - 1;

    /* Fast path: if no VACB in the written range has ever been mapped
     * via CcMapData on this volume, there's nothing to purge. */
    if (!NtfsAnyCachedVacbInRange(Vcb, FirstVacb, LastVacb))
        return Status;

    PurgeOffset.QuadPart = StartingOffset;
    PurgeLength.QuadPart = EndOffset - StartingOffset;
    CcPurgeCacheSection(Vcb->StreamFileObject->SectionObjectPointer,
                        &PurgeOffset,
                        (ULONG)PurgeLength.QuadPart,
                        FALSE);

    return Status;
}

NTSTATUS
NtfsReadDisk(IN PDEVICE_OBJECT DeviceObject,
             IN LONGLONG StartingOffset,
             IN ULONG Length,
             IN ULONG SectorSize,
             IN OUT PUCHAR Buffer,
             IN BOOLEAN Override)
{
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    KEVENT Event;
    PIRP Irp;
    PMDL Mdl;
    NTSTATUS Status;
    ULONGLONG RealReadOffset;
    ULONG RealLength;
    BOOLEAN AllocatedBuffer = FALSE;
    PUCHAR ReadBuffer = Buffer;

    DPRINT("NtfsReadDisk: offset=%I64d len=%lu\n", StartingOffset, Length);

    KeInitializeEvent(&Event,
                      NotificationEvent,
                      FALSE);

    RealReadOffset = (ULONGLONG)StartingOffset;
    RealLength = Length;

    if ((RealReadOffset % SectorSize) != 0 || (RealLength % SectorSize) != 0)
    {
        RealReadOffset = ROUND_DOWN(StartingOffset, SectorSize);
        RealLength = ROUND_UP(Length, SectorSize);

        ReadBuffer = ExAllocatePoolWithTag(NonPagedPool, RealLength + SectorSize, TAG_NTFS);
        if (ReadBuffer == NULL)
        {
            DPRINT1("Not enough memory!\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        AllocatedBuffer = TRUE;
    }

    Offset.QuadPart = RealReadOffset;

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ,
                                       DeviceObject,
                                       ReadBuffer,
                                       RealLength,
                                       &Offset,
                                       &Event,
                                       &IoStatus);
    if (Irp == NULL)
    {
        DPRINT1("IoBuildSynchronousFsdRequest failed\n");

        if (AllocatedBuffer)
        {
            ExFreePoolWithTag(ReadBuffer, TAG_NTFS);
        }

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * NtfsReadDisk can be called from a page fault handler where kernel APCs
     * are disabled. IoBuildSynchronousFsdRequest creates an IRP with Flags=0,
     * so IofCompleteRequest uses APC-based completion. With APCs disabled the
     * completion APC never fires and KeWaitForSingleObject deadlocks.
     *
     * Setting paging IO flags makes IofCompleteRequest signal UserEvent
     * directly instead of queuing an APC. We must also remove the IRP from
     * the thread's IRP list first, because the paging IO completion path
     * calls IoFreeIrp which asserts ThreadListEntry is empty.
     */
    RemoveEntryList(&Irp->ThreadListEntry);
    InitializeListHead(&Irp->ThreadListEntry);
    Irp->Flags |= IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO;

    if (Override)
    {
        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    /*
     * Save the MDL pointer — the paging IO completion path frees the IRP
     * via IoFreeIrp but does NOT unlock or free the MDL. The storage
     * driver may have called MmProbeAndLockPages on it, so we must call
     * MmUnlockPages + IoFreeMdl ourselves after the IRP completes.
     */
    Mdl = Irp->MdlAddress;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    /* Irp has been freed by IofCompleteRequest (paging IO path).
     * Unlock and free the MDL that IoBuildSynchronousFsdRequest allocated. */
    if (Mdl)
    {
        if (Mdl->MdlFlags & MDL_PAGES_LOCKED)
        {
            MmUnlockPages(Mdl);
        }
        IoFreeMdl(Mdl);
    }

    if (AllocatedBuffer)
    {
        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(Buffer, ReadBuffer + (StartingOffset - RealReadOffset), Length);
        }

        ExFreePoolWithTag(ReadBuffer, TAG_NTFS);
    }

    DPRINT("NtfsReadDisk() done (Status %x)\n", Status);

    return Status;
}

/**
* @name NtfsWriteDisk
* @implemented
*
* Writes data from the given buffer to the given DeviceObject.
*
* @param DeviceObject
* Device to write to
*
* @param StartingOffset
* Offset, in bytes, from the start of the device object where the data will be written
*
* @param Length
* How much data will be written, in bytes
*
* @param SectorSize
* Size of the sector on the disk that the write must be aligned to
*
* @param Buffer
* The data that's being written to the device
*
* @return
* STATUS_SUCCESS in case of success, STATUS_INSUFFICIENT_RESOURCES if a memory allocation failed,
* or whatever status IoCallDriver() sets.
*
* @remarks Called by NtfsWriteFile(). May perform a read-modify-write operation if the
* requested write is not sector-aligned.
*
*/
NTSTATUS
NtfsWriteDisk(IN PDEVICE_OBJECT DeviceObject,
              IN LONGLONG StartingOffset,
              IN ULONG Length,
              IN ULONG SectorSize,
              IN const PUCHAR Buffer)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    KEVENT Event;
    PIRP Irp;
    PMDL Mdl;
    NTSTATUS Status;
    ULONGLONG RealWriteOffset;
    ULONG RealLength;
    BOOLEAN AllocatedBuffer = FALSE;
    PUCHAR TempBuffer = NULL;

    DPRINT("NtfsWriteDisk: offset=%I64d len=%lu\n", StartingOffset, Length);

    if (Length == 0)
        return STATUS_SUCCESS;

    RealWriteOffset = (ULONGLONG)StartingOffset;
    RealLength = Length;

    // Does the write need to be adjusted to be sector-aligned?
    if ((RealWriteOffset % SectorSize) != 0 || (RealLength % SectorSize) != 0)
    {
        ULONGLONG relativeOffset;

        // We need to do a read-modify-write. We'll start be copying the entire
        // contents of every sector that will be overwritten.
        // TODO: Optimize (read no more than necessary)

        RealWriteOffset = ROUND_DOWN(StartingOffset, SectorSize);
        RealLength = ROUND_UP(Length, SectorSize);

        // Would the end of our sector-aligned write fall short of the requested write?
        if (RealWriteOffset + RealLength < StartingOffset + Length)
        {
            RealLength += SectorSize;
        }

        // Did we underestimate the memory required somehow?
        if (RealLength + RealWriteOffset < StartingOffset + Length)
        {
            DPRINT1("\a\t\t\t\t\tFIXME: calculated less memory than needed!\n");
            DPRINT1("StartingOffset: %lu\tLength: %lu\tRealWriteOffset: %lu\tRealLength: %lu\n",
                    StartingOffset, Length, RealWriteOffset, RealLength);

            RealLength += SectorSize;
        }

        // Allocate a buffer to copy the existing data to
        TempBuffer = ExAllocatePoolWithTag(NonPagedPool, RealLength, TAG_NTFS);

        // Did we fail to allocate it?
        if (TempBuffer == NULL)
        {
            DPRINT1("Not enough memory!\n");

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // Read the sectors we'll be overwriting into TempBuffer
        Status = NtfsReadDisk(DeviceObject, RealWriteOffset, RealLength, SectorSize, TempBuffer, FALSE);

        // Did we fail the read?
        if (!NT_SUCCESS(Status))
        {
            RtlSecureZeroMemory(TempBuffer, RealLength);
            ExFreePoolWithTag(TempBuffer, TAG_NTFS);
            return Status;
        }

        // Calculate where the new data should be written to, relative to the start of TempBuffer
        relativeOffset = StartingOffset - RealWriteOffset;

        // Modify the tempbuffer with the data being read
        RtlCopyMemory(TempBuffer + relativeOffset, Buffer, Length);

        AllocatedBuffer = TRUE;
    }

    // set the destination offset
    Offset.QuadPart = RealWriteOffset;

    // setup the notification event for the write
    KeInitializeEvent(&Event,
                      NotificationEvent,
                      FALSE);

    // Build an IRP requesting the lower-level [disk] driver to perform the write
    // TODO: Forward the existing IRP instead
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE,
                                       DeviceObject,
                                       // if we allocated a temp buffer, use that instead of the Buffer parameter
                                       ((AllocatedBuffer) ? TempBuffer : Buffer),
                                       RealLength,
                                       &Offset,
                                       &Event,
                                       &IoStatus);
    // Did we fail to build the IRP?
    if (Irp == NULL)
    {
        DPRINT1("IoBuildSynchronousFsdRequest failed\n");

        if (AllocatedBuffer)
        {
            RtlSecureZeroMemory(TempBuffer, RealLength);
            ExFreePoolWithTag(TempBuffer, TAG_NTFS);
        }

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* See comment in NtfsReadDisk — avoid APC-based completion deadlock */
    RemoveEntryList(&Irp->ThreadListEntry);
    InitializeListHead(&Irp->ThreadListEntry);
    Irp->Flags |= IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO;

    Mdl = Irp->MdlAddress;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (Mdl)
    {
        if (Mdl->MdlFlags & MDL_PAGES_LOCKED)
        {
            MmUnlockPages(Mdl);
        }
        IoFreeMdl(Mdl);
    }

    if (AllocatedBuffer)
    {
        // zero the buffer before freeing it, so private user data can't be snooped
        RtlSecureZeroMemory(TempBuffer, RealLength);

        ExFreePoolWithTag(TempBuffer, TAG_NTFS);
    }

    DPRINT("NtfsWriteDisk() done (Status %x)\n", Status);

    return Status;
}

NTSTATUS
NtfsReadSectors(IN PDEVICE_OBJECT DeviceObject,
                IN ULONG DiskSector,
                IN ULONG SectorCount,
                IN ULONG SectorSize,
                IN OUT PUCHAR Buffer,
                IN BOOLEAN Override)
{
    LONGLONG Offset;
    ULONG BlockSize;

    Offset = (LONGLONG)DiskSector * (LONGLONG)SectorSize;
    BlockSize = SectorCount * SectorSize;

    return NtfsReadDisk(DeviceObject, Offset, BlockSize, SectorSize, Buffer, Override);
}


NTSTATUS
NtfsDeviceIoControl(IN PDEVICE_OBJECT DeviceObject,
                    IN ULONG ControlCode,
                    IN PVOID InputBuffer,
                    IN ULONG InputBufferSize,
                    IN OUT PVOID OutputBuffer,
                    IN OUT PULONG OutputBufferSize,
                    IN BOOLEAN Override)
{
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    DPRINT("Building device I/O control request ...\n");
    Irp = IoBuildDeviceIoControlRequest(ControlCode,
                                        DeviceObject,
                                        InputBuffer,
                                        InputBufferSize,
                                        OutputBuffer,
                                        (OutputBufferSize) ? *OutputBufferSize : 0,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
    {
        DPRINT("IoBuildDeviceIoControlRequest() failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Override)
    {
        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    DPRINT("Calling IO Driver... with irp %p\n", Irp);
    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (OutputBufferSize)
    {
        *OutputBufferSize = IoStatus.Information;
    }

    return Status;
}

/* EOF */
