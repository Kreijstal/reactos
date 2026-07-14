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
 * contexts where kernel APCs are disabled, and lets metadata reads hit the
 * same volume stream cache that NtfsWriteDiskCached writes through.
 *
 * Falls back to NtfsReadDisk if the cache is not initialized.
 */
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

    /* The cache manager faults pages in by issuing IRP_PAGING_IO back to
     * NtfsFsdRead on the volume StreamFileObject.  NtfsReadFile shortcuts
     * any FCB_IS_VOLUME or FCB_IS_VOLUME_STREAM read to NtfsReadDisk
     * (rw.c), which talks directly to the storage stack and does not
     * re-enter NtfsReadDiskCached.  The recursion chain therefore has a
     * fixed depth of two and terminates without a guard.
     *
     * This matches Windows ntfs.sys: cached reads on the volume stream
     * are not gated by TopLevelIrp; the paging-IO path on the volume
     * stream FCB is the recursion terminator. */
    if (Vcb->StreamFileObject == NULL ||
        Vcb->StreamFileObject->PrivateCacheMap == NULL)
    {
        return NtfsReadDisk(Vcb->StorageDevice,
                            StartingOffset,
                            Length,
                            Vcb->NtfsInfo.BytesPerSector,
                            Buffer,
                            FALSE);
    }

    /* CcMapData cannot map across VACB boundaries (256KB).
       Split the read into chunks that each stay within one VACB. */
    while (Length > 0)
    {
        VolumeOffset.QuadPart = StartingOffset;
        VacbOffset = (ULONG)(StartingOffset % VACB_MAPPING_GRANULARITY);
        ChunkLength = min(Length, VACB_MAPPING_GRANULARITY - VacbOffset);

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

    return STATUS_SUCCESS;
}

/**
 * @brief Write metadata through the volume stream cache (write-back).
 *
 * Metadata updates (MFT records, $INDEX_ALLOCATION nodes, $BITMAP, ...) are
 * copied into the volume stream cache with CcCopyWrite, which marks the pages
 * dirty and leaves the actual device write to the Cc lazy writer.  The lazy
 * writer flushes the dirty volume stream pages asynchronously by issuing a
 * paging write that NtfsWriteFile() routes - for the FCB_IS_VOLUME_STREAM
 * stream - to NtfsWriteDisk().  IRP_MJ_SHUTDOWN (NtfsFlushVolume) and volume
 * dismount both CcFlushCache the same stream, so dirty metadata reaches disk
 * on a clean stop.
 *
 * This replaces the previous synchronous write-through: an install issues tens
 * of thousands of tiny metadata writes, and paying a blocking device IRP for
 * each one dominated wall-clock time.  Writing through the cache lets those
 * updates coalesce and removes the per-write round-trip from the hot path.
 * Reads of the same metadata use the same cache (NtfsReadDiskCached), so the
 * cached copy is the single coherent source until it is flushed.
 *
 * When the cache is not available - mount and format run before
 * CcInitializeCacheMap, and CcCopyWrite cannot describe a range beyond the
 * cached stream size - the write falls back to a synchronous, immediately
 * durable NtfsWriteDisk().
 */
NTSTATUS
NtfsWriteDiskCached(IN PDEVICE_EXTENSION Vcb,
                    IN LONGLONG StartingOffset,
                    IN ULONG Length,
                    IN const PUCHAR Buffer)
{
    LARGE_INTEGER CacheOffset;
    LONGLONG EndOffset;
    BOOLEAN CachedWrite = FALSE;

    EndOffset = StartingOffset + (LONGLONG)Length;

    if (Vcb->StreamFileObject != NULL &&
        Vcb->StreamFileObject->SectionObjectPointer != NULL &&
        Vcb->StreamFileObject->SectionObjectPointer->SharedCacheMap != NULL &&
        EndOffset <= CcGetFileSizePointer(Vcb->StreamFileObject)->QuadPart)
    {
        CacheOffset.QuadPart = StartingOffset;
        _SEH2_TRY
        {
            CcCopyWrite(Vcb->StreamFileObject,
                        &CacheOffset,
                        Length,
                        TRUE,
                        (PVOID)Buffer);
            CachedWrite = TRUE;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            CachedWrite = FALSE;
        }
        _SEH2_END;

        if (CachedWrite)
            return STATUS_SUCCESS;
    }

    /* Cache not yet initialised (mount/format bootstrap) or CcCopyWrite could
     * not take the data: write straight to the device so it is durable now. */
    return NtfsWriteDisk(Vcb->StorageDevice,
                         StartingOffset,
                         Length,
                         Vcb->NtfsInfo.BytesPerSector,
                         Buffer);
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
    BOOLEAN PagingCompletion = FALSE;
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
     * are disabled. IoBuildSynchronousFsdRequest normally completes through a
     * kernel APC, which cannot run in that state. Only use the paging-IO
     * completion shortcut for those APC-disabled callers; normal metadata
     * reads must keep the standard synchronous IRP completion path.
     */
    if (KeAreAllApcsDisabled())
    {
        RemoveEntryList(&Irp->ThreadListEntry);
        InitializeListHead(&Irp->ThreadListEntry);
        Irp->Flags |= IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO;
        PagingCompletion = TRUE;
    }

    if (Override)
    {
        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    /*
     * Save the MDL pointer - the paging IO completion path frees the IRP
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

    /* In the paging-IO completion path, IofCompleteRequest frees the IRP but
     * does not unlock/free the MDL that IoBuildSynchronousFsdRequest allocated.
     * The normal APC completion path owns both the IRP and MDL. */
    if (PagingCompletion && Mdl)
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
    BOOLEAN PagingCompletion = FALSE;
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

    /* See comment in NtfsReadDisk -- avoid APC-based completion deadlock only
     * for callers that cannot receive the normal completion APC. */
    if (KeAreAllApcsDisabled())
    {
        RemoveEntryList(&Irp->ThreadListEntry);
        InitializeListHead(&Irp->ThreadListEntry);
        Irp->Flags |= IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO;
        PagingCompletion = TRUE;
    }

    Mdl = Irp->MdlAddress;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (PagingCompletion && Mdl)
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
