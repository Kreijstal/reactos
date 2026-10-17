/*
 *  ReactOS kernel
 *  Copyright (C) 2002, 2014 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/volume.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsLoadVolumeBitmap
*
* Reads $Bitmap into memory once at mount. Cluster allocation and freeing work
* on this copy and write back only the sectors they change.
*
* @param DeviceExt
* Volume being mounted. Its file record lookaside list and MFT context must be ready.
*
* @return
* STATUS_SUCCESS, or an error if $Bitmap could not be found or read.
*/
NTSTATUS
NtfsLoadVolumeBitmap(
    _In_ PDEVICE_EXTENSION DeviceExt)
{
    PFILE_RECORD_HEADER BitmapRecord;
    ULONGLONG BitmapSize;
    ULONG BufferSize;
    NTSTATUS Status;

    BitmapRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (BitmapRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_BITMAP, BitmapRecord);
    if (NT_SUCCESS(Status))
    {
        /* The context keeps its own copy of the attribute record */
        Status = FindAttribute(DeviceExt, BitmapRecord, AttributeData, L"", 0, &DeviceExt->BitmapContext, NULL);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Unable to find $Bitmap (Status %lx)\n", Status);
        DeviceExt->BitmapContext = NULL;
        return Status;
    }

    BitmapSize = AttributeDataLength(DeviceExt->BitmapContext->pRecord);
    if (BitmapSize > MAXULONG ||
        BitmapSize * 8 < DeviceExt->NtfsInfo.ClusterCount ||
        DeviceExt->NtfsInfo.ClusterCount > MAXULONG)
    {
        DPRINT1("$Bitmap size %I64u does not cover %I64u clusters\n",
                BitmapSize, DeviceExt->NtfsInfo.ClusterCount);
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Fail;
    }

    /* Whole sectors, so write back never reads first */
    BufferSize = ROUND_UP((ULONG)BitmapSize, DeviceExt->NtfsInfo.BytesPerSector);
    DeviceExt->BitmapBuffer = ExAllocatePoolWithTag(PagedPool, BufferSize, TAG_NTFS);
    if (DeviceExt->BitmapBuffer == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }
    RtlZeroMemory(DeviceExt->BitmapBuffer, BufferSize);

    if (ReadAttribute(DeviceExt, DeviceExt->BitmapContext, 0, (PCHAR)DeviceExt->BitmapBuffer, (ULONG)BitmapSize) != (ULONG)BitmapSize)
    {
        DPRINT1("Unable to read $Bitmap\n");
        Status = STATUS_UNSUCCESSFUL;
        goto Fail;
    }

    DeviceExt->BitmapSize = (ULONG)BitmapSize;

    /* $Bitmap is rounded up to a cluster and covers a few clusters more than
     * the volume has. Bounding by ClusterCount ignores the surplus bits, but
     * write back still preserves them on disk. */
    RtlInitializeBitMap(&DeviceExt->ClusterBitmap, DeviceExt->BitmapBuffer, (ULONG)DeviceExt->NtfsInfo.ClusterCount);
    DeviceExt->FreeClusterCount = RtlNumberOfClearBits(&DeviceExt->ClusterBitmap);

    DPRINT("Volume bitmap loaded: %I64u of %I64u clusters free\n",
           DeviceExt->FreeClusterCount, DeviceExt->NtfsInfo.ClusterCount);

    return STATUS_SUCCESS;

Fail:
    NtfsFreeVolumeBitmap(DeviceExt);
    return Status;
}

/* $BadClus:$Bad covers every VCN in the volume. Its holes are valid without
 * NTFS_ATTR_SPARSE, so the ordinary nonresident resize would allocate them.
 * Retain actual bad-cluster mappings and only extend the trailing hole. */
static NTSTATUS
NtfsExtendBadClusters(PDEVICE_EXTENSION Vcb, ULONG NewClusterCount)
{
    PFILE_RECORD_HEADER Record, Holder = NULL;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PNTFS_ATTR_RECORD Attribute;
    ULONG Offset;
    NTSTATUS Status;

    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadFileRecord(Vcb, NTFS_FILE_BADCLUS, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"$Bad", 4, &Context, NULL);
    if (NT_SUCCESS(Status))
        Status = NtfsResolveAttributeHolder(Vcb, Context, Record, &Holder, &Offset);
    if (NT_SUCCESS(Status))
    {
        ASSERT(Context->pRecord->IsNonResident);
        Status = NtfsStoreMappingPairs(Vcb, Context, Offset, Holder, NewClusterCount);
        if (NT_SUCCESS(Status))
        {
            Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Holder + Offset);
            Attribute->NonResident.DataSize = (ULONGLONG)NewClusterCount * Vcb->NtfsInfo.BytesPerCluster;
            Context->pRecord->NonResident.DataSize = Attribute->NonResident.DataSize;
            Status = NtfsCommitAttributeContext(Vcb, Context, Holder);
        }
    }
    NtfsReleaseAttributeHolder(Vcb, Record, Holder);
    if (Context != NULL)
        ReleaseAttributeContext(Context);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    return Status;
}

/* The caller owns MftAllocationResource and BitmapResource exclusively.
 * Recursive metadata allocations may use the new range while writeback of
 * allocation bits is deferred until $Bitmap can hold those bits itself.
 * Restore the old allocation bound on return; the request publishes the
 * larger bound only after committing the primary boot sector. */
NTSTATUS
NtfsExtendVolumeBitmap(PDEVICE_EXTENSION Vcb, ULONG NewClusterCount, PBOOLEAN MetadataTouched)
{
    PFILE_RECORD_HEADER Record = NULL, Holder = NULL;
    PULONG Buffer = NULL, OldBuffer;
    LARGE_INTEGER Size;
    ULONG Offset, Written, OldSize = Vcb->BitmapSize;
    ULONG OldClusterCount = (ULONG)Vcb->NtfsInfo.ClusterCount;
    ULONGLONG OldFreeClusters = Vcb->FreeClusterCount;
    NTSTATUS Status;

    *MetadataTouched = FALSE;
    ASSERT(NewClusterCount > OldClusterCount);
    ASSERT(!Vcb->DeferVolumeBitmapWrites);
    Size.QuadPart = ALIGN_UP_BY(max(OldSize, ((ULONGLONG)NewClusterCount + 7) / 8),
                                Vcb->NtfsInfo.BytesPerSector);
    if (Size.QuadPart > MAXULONG)
        return STATUS_FILE_CORRUPT_ERROR;
    Buffer = ExAllocatePoolWithTag(PagedPool, (ULONG)Size.QuadPart, TAG_NTFS);
    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Buffer == NULL || Record == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    Status = ReadFileRecord(Vcb, NTFS_FILE_BITMAP, Record);
    if (NT_SUCCESS(Status))
        Status = NtfsResolveAttributeHolder(Vcb, Vcb->BitmapContext, Record, &Holder, &Offset);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    ASSERT(Vcb->BitmapContext->pRecord->IsNonResident);

    RtlFillMemory(Buffer, (ULONG)Size.QuadPart, 0xff);
    RtlCopyMemory(Buffer, Vcb->BitmapBuffer, OldSize);
    OldBuffer = Vcb->BitmapBuffer;
    Vcb->BitmapBuffer = Buffer;
    Buffer = NULL;
    RtlInitializeBitMap(&Vcb->ClusterBitmap, Vcb->BitmapBuffer, NewClusterCount);
    RtlClearBits(&Vcb->ClusterBitmap, OldClusterCount, NewClusterCount - OldClusterCount);
    Vcb->FreeClusterCount = OldFreeClusters + NewClusterCount - OldClusterCount;
    Vcb->DeferVolumeBitmapWrites = TRUE;
    *MetadataTouched = TRUE;
    Status = SetNonResidentAttributeDataLength(Vcb, Vcb->BitmapContext, Offset, Holder, &Size);
    if (NT_SUCCESS(Status))
        Status = NtfsCommitAttributeContext(Vcb, Vcb->BitmapContext, Holder);
    if (NT_SUCCESS(Status))
        Status = NtfsExtendBadClusters(Vcb, NewClusterCount);
    if (NT_SUCCESS(Status))
    {
        Status = WriteAttribute(Vcb, Vcb->BitmapContext, 0, (PUCHAR)Vcb->BitmapBuffer,
                                 (ULONG)Size.QuadPart, &Written, Holder);
        if (NT_SUCCESS(Status) && Written != Size.QuadPart)
            Status = STATUS_UNEXPECTED_IO_ERROR;
    }
    Vcb->DeferVolumeBitmapWrites = FALSE;
    Vcb->BitmapSize = (ULONG)AttributeDataLength(Vcb->BitmapContext->pRecord);
    RtlInitializeBitMap(&Vcb->ClusterBitmap, Vcb->BitmapBuffer, OldClusterCount);
    Vcb->FreeClusterCount = RtlNumberOfClearBits(&Vcb->ClusterBitmap);
    if (!NT_SUCCESS(Status))
    {
        /* A failed resize may have reserved old-range clusters for metadata.
         * Keep those claims in memory and attempt to persist them as well;
         * returning the old snapshot would make their owners cross-linked. */
        (VOID)WriteAttribute(Vcb, Vcb->BitmapContext, 0, (PUCHAR)Vcb->BitmapBuffer,
                             min(OldSize, Vcb->BitmapSize), &Written, Holder);
    }
    ExFreePoolWithTag(OldBuffer, TAG_NTFS);
Cleanup:
    NtfsReleaseAttributeHolder(Vcb, Record, Holder);
    if (Record != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    if (Buffer != NULL)
        ExFreePoolWithTag(Buffer, TAG_NTFS);
    return Status;
}

/**
* @name NtfsFreeVolumeBitmap
*
* Releases what NtfsLoadVolumeBitmap() set up. Safe on a partly loaded volume.
*
* @param DeviceExt
* Volume whose bitmap copy is released.
*/
VOID
NtfsFreeVolumeBitmap(
    _In_ PDEVICE_EXTENSION DeviceExt)
{
    if (DeviceExt->BitmapBuffer != NULL)
    {
        ExFreePoolWithTag(DeviceExt->BitmapBuffer, TAG_NTFS);
        DeviceExt->BitmapBuffer = NULL;
    }

    if (DeviceExt->BitmapContext != NULL)
    {
        ReleaseAttributeContext(DeviceExt->BitmapContext);
        DeviceExt->BitmapContext = NULL;
    }
}

/**
* @name NtfsWriteVolumeBitmap
*
* Writes the sectors of $Bitmap that hold the given clusters' bits.
* The caller holds BitmapResource exclusively.
*
* @param DeviceExt
* Volume whose bitmap changed.
*
* @param FirstCluster
* First cluster whose bit changed.
*
* @param ClusterCount
* Number of clusters, starting at FirstCluster, to cover. Can't be 0.
*
* @return
* Status from WriteAttribute().
*/
NTSTATUS
NtfsWriteVolumeBitmap(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ ULONG FirstCluster,
    _In_ ULONG ClusterCount)
{
    ULONG SectorSize = DeviceExt->NtfsInfo.BytesPerSector;
    ULONG Start;
    ULONG End;
    ULONG LengthWritten;

    ASSERT(ClusterCount != 0);
    if (DeviceExt->DeferVolumeBitmapWrites)
        return STATUS_SUCCESS;

    Start = ROUND_DOWN(FirstCluster / 8, SectorSize);
    End = ROUND_UP((FirstCluster + ClusterCount - 1) / 8 + 1, SectorSize);
    End = min(End, ROUND_UP(DeviceExt->BitmapSize, SectorSize));

    return WriteAttribute(DeviceExt,
                          DeviceExt->BitmapContext,
                          Start,
                          (PUCHAR)DeviceExt->BitmapBuffer + Start,
                          End - Start,
                          &LengthWritten,
                          NULL);
}

ULONGLONG
NtfsGetFreeClusters(PDEVICE_EXTENSION DeviceExt)
{
    ULONGLONG FreeClusters;

    DPRINT("NtfsGetFreeClusters(%p)\n", DeviceExt);

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&DeviceExt->BitmapResource, TRUE);
    FreeClusters = DeviceExt->FreeClusterCount;
    ExReleaseResourceLite(&DeviceExt->BitmapResource);
    KeLeaveCriticalRegion();

    return FreeClusters;
}

/**
* NtfsAllocateClusters
* Allocates a run of clusters. The run allocated might be smaller than DesiredClusters.
*/
NTSTATUS
NtfsAllocateClusters(PDEVICE_EXTENSION DeviceExt,
                     ULONG FirstDesiredCluster,
                     ULONG DesiredClusters,
                     PULONG FirstAssignedCluster,
                     PULONG AssignedClusters)
{
    NTSTATUS Status;
    PRTL_BITMAP Bitmap = &DeviceExt->ClusterBitmap;
    ULONG AssignedRun;

    ASSERT(DesiredClusters != 0);
    DPRINT("NtfsAllocateClusters(%p, %lu, %lu, %p, %p)\n", DeviceExt, FirstDesiredCluster, DesiredClusters, FirstAssignedCluster, AssignedClusters);

    *AssignedClusters = 0;

    /* Files growing in parallel must not both claim the same free run */
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&DeviceExt->BitmapResource, TRUE);

    if (DeviceExt->FreeClusterCount < DesiredClusters)
    {
        Status = STATUS_DISK_FULL;
        goto Quit;
    }

    /* The hint is just past the file's last cluster, which can be past the volume end */
    if (FirstDesiredCluster >= Bitmap->SizeOfBitMap)
        FirstDesiredCluster = 0;

    // TODO: Observe MFT reservation zone

    // Can we get one contiguous run?
    AssignedRun = RtlFindClearBitsAndSet(Bitmap, DesiredClusters, FirstDesiredCluster);

    if (AssignedRun != 0xFFFFFFFF)
    {
        *FirstAssignedCluster = AssignedRun;
        *AssignedClusters = DesiredClusters;
    }
    else
    {
        ULONG Cursor = FirstDesiredCluster;
        ULONG Start, Count;

        /* Minimize mapping pairs when the request needs several extents.
         * Prefer the largest extent after the hint, so successive extensions
         * can coalesce, rather than consuming every small hole in between. */
        while (Cursor < Bitmap->SizeOfBitMap)
        {
            Count = RtlFindNextForwardRunClear(Bitmap, Cursor, &Start);
            if (Count == 0)
                break;
            if (Count > *AssignedClusters)
            {
                *FirstAssignedCluster = Start;
                *AssignedClusters = Count;
            }
            ASSERT(Start + Count > Cursor);
            Cursor = Start + Count;
        }

        if (*AssignedClusters == 0)
            *AssignedClusters = RtlFindLongestRunClear(Bitmap, FirstAssignedCluster);
        ASSERT(*AssignedClusters != 0);

        /* Hand out no more than was asked for, and mark it in use */
        *AssignedClusters = min(*AssignedClusters, DesiredClusters);
        RtlSetBits(Bitmap, *FirstAssignedCluster, *AssignedClusters);
    }

    Status = NtfsWriteVolumeBitmap(DeviceExt, *FirstAssignedCluster, *AssignedClusters);
    if (NT_SUCCESS(Status))
    {
        DeviceExt->FreeClusterCount -= *AssignedClusters;
    }
    else
    {
        /* Keep the copy in step with what the disk says */
        RtlClearBits(Bitmap, *FirstAssignedCluster, *AssignedClusters);
        *AssignedClusters = 0;
    }

Quit:
    ExReleaseResourceLite(&DeviceExt->BitmapResource);
    KeLeaveCriticalRegion();

    return Status;
}

/**
* @name NtfsAllocateClusterRange
* @implemented
*
* Marks exactly [FirstCluster, FirstCluster + ClusterCount) in use: STATUS_ALREADY_COMMITTED
* if any of them is, STATUS_INVALID_PARAMETER if they aren't on the volume.
*/
NTSTATUS
NtfsAllocateClusterRange(PDEVICE_EXTENSION DeviceExt,
                         ULONG FirstCluster,
                         ULONG ClusterCount)
{
    PRTL_BITMAP Bitmap = &DeviceExt->ClusterBitmap;
    NTSTATUS Status;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&DeviceExt->BitmapResource, TRUE);

    if (ClusterCount == 0 || FirstCluster >= Bitmap->SizeOfBitMap ||
        ClusterCount > Bitmap->SizeOfBitMap - FirstCluster)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (!RtlAreBitsClear(Bitmap, FirstCluster, ClusterCount))
    {
        Status = STATUS_ALREADY_COMMITTED;
    }
    else
    {
        RtlSetBits(Bitmap, FirstCluster, ClusterCount);
        Status = NtfsWriteVolumeBitmap(DeviceExt, FirstCluster, ClusterCount);
        if (NT_SUCCESS(Status))
            DeviceExt->FreeClusterCount -= ClusterCount;
        else
            RtlClearBits(Bitmap, FirstCluster, ClusterCount);
    }

    ExReleaseResourceLite(&DeviceExt->BitmapResource);
    KeLeaveCriticalRegion();

    return Status;
}

/**
* @name NtfsCopyVariablePart
*
* Copies as much of a query's variable-length tail as the buffer holds after its fixed
* part, the way every NTFS query does: the fixed part is always complete, a tail that
* doesn't fit is cut short and the query answers STATUS_BUFFER_OVERFLOW.
*
* @return
* The number of bytes copied.
*/
static
ULONG
NtfsCopyVariablePart(
    _Out_writes_bytes_(Room) PVOID Destination,
    _In_ ULONG Room,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ ULONG Length)
{
    ULONG BytesToCopy = min(Room, Length);

    RtlCopyMemory(Destination, Source, BytesToCopy);
    return BytesToCopy;
}

static
NTSTATUS
NtfsGetFsVolumeInformation(PDEVICE_OBJECT DeviceObject,
                           PFILE_FS_VOLUME_INFORMATION FsVolumeInfo,
                           PULONG BufferLength)
{
    ULONG Fixed = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
    ULONG Copied;

    DPRINT("NtfsGetFsVolumeInformation() called\n");

    if (*BufferLength < Fixed)
        return STATUS_INFO_LENGTH_MISMATCH;

    FsVolumeInfo->VolumeSerialNumber = DeviceObject->Vpb->SerialNumber;
    FsVolumeInfo->VolumeCreationTime.QuadPart = 0;
    FsVolumeInfo->SupportsObjects = FALSE;

    Copied = NtfsCopyVariablePart(FsVolumeInfo->VolumeLabel,
                                  *BufferLength - Fixed,
                                  DeviceObject->Vpb->VolumeLabel,
                                  DeviceObject->Vpb->VolumeLabelLength);
    FsVolumeInfo->VolumeLabelLength = Copied;

    *BufferLength -= Fixed + Copied;

    return (Copied < DeviceObject->Vpb->VolumeLabelLength) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetFsAttributeInformation(PDEVICE_EXTENSION DeviceExt,
                              PFILE_FS_ATTRIBUTE_INFORMATION FsAttributeInfo,
                              PULONG BufferLength)
{
    ULONG Fixed = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
    ULONG Copied;

    DPRINT("NtfsGetFsAttributeInformation()\n");

    if (*BufferLength < Fixed)
        return STATUS_INFO_LENGTH_MISMATCH;

    /* What this driver implements of what Windows' NTFS reports */
    FsAttributeInfo->FileSystemAttributes =
        FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK |
        FILE_SUPPORTS_OPEN_BY_FILE_ID | FILE_NAMED_STREAMS | FILE_SUPPORTS_HARD_LINKS |
        FILE_SUPPORTS_REPARSE_POINTS;
    if (DeviceExt->ObjectIdIndexMftIndex != 0)
        FsAttributeInfo->FileSystemAttributes |= FILE_SUPPORTS_OBJECT_IDS;
    if (DeviceExt->SecureMftIndex != 0)
        FsAttributeInfo->FileSystemAttributes |= FILE_PERSISTENT_ACLS;
    if (DeviceExt->NtfsInfo.MajorVersion >= 3)
        FsAttributeInfo->FileSystemAttributes |= FILE_SUPPORTS_USN_JOURNAL;
    if (DeviceExt->QuotaMftIndex != 0)
        FsAttributeInfo->FileSystemAttributes |= FILE_VOLUME_QUOTAS;
    FsAttributeInfo->FileSystemAttributes |= FILE_SUPPORTS_EXTENDED_ATTRIBUTES | FILE_SUPPORTS_SPARSE_FILES |
                                                 FILE_FILE_COMPRESSION;
    /* Writes are refused everywhere unless write support was enabled */
    if (!NtfsGlobalData->EnableWriteSupport)
        FsAttributeInfo->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
    FsAttributeInfo->MaximumComponentNameLength = NTFS_MAX_NAME_LENGTH;

    Copied = NtfsCopyVariablePart(FsAttributeInfo->FileSystemName, *BufferLength - Fixed, L"NTFS", 8);
    FsAttributeInfo->FileSystemNameLength = Copied;

    *BufferLength -= Fixed + Copied;

    return (Copied < 8) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetFsSizeInformation(PDEVICE_OBJECT DeviceObject,
                         PFILE_FS_SIZE_INFORMATION FsSizeInfo,
                         PULONG BufferLength)
{
    PDEVICE_EXTENSION DeviceExt = DeviceObject->DeviceExtension;

    DPRINT("NtfsGetFsSizeInformation()\n");

    if (*BufferLength < sizeof(FILE_FS_SIZE_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    ExAcquireResourceSharedLite(&DeviceExt->BitmapResource, TRUE);
    FsSizeInfo->AvailableAllocationUnits.QuadPart = NtfsGetFreeClusters(DeviceExt);
    FsSizeInfo->TotalAllocationUnits.QuadPart = DeviceExt->NtfsInfo.ClusterCount;
    FsSizeInfo->SectorsPerAllocationUnit = DeviceExt->NtfsInfo.SectorsPerCluster;
    FsSizeInfo->BytesPerSector = DeviceExt->NtfsInfo.BytesPerSector;
    ExReleaseResourceLite(&DeviceExt->BitmapResource);

    *BufferLength -= sizeof(FILE_FS_SIZE_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetFsFullSizeInformation(PDEVICE_OBJECT DeviceObject,
                             PFILE_FS_FULL_SIZE_INFORMATION FsFullSizeInfo,
                             PULONG BufferLength)
{
    PDEVICE_EXTENSION DeviceExt = DeviceObject->DeviceExtension;
    ULONGLONG FreeClusters;

    DPRINT("NtfsGetFsFullSizeInformation()\n");

    if (*BufferLength < sizeof(FILE_FS_FULL_SIZE_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    /* No quotas are enforced, so the caller can have every free cluster */
    ExAcquireResourceSharedLite(&DeviceExt->BitmapResource, TRUE);
    FreeClusters = NtfsGetFreeClusters(DeviceExt);
    FsFullSizeInfo->TotalAllocationUnits.QuadPart = DeviceExt->NtfsInfo.ClusterCount;
    FsFullSizeInfo->CallerAvailableAllocationUnits.QuadPart = FreeClusters;
    FsFullSizeInfo->ActualAvailableAllocationUnits.QuadPart = FreeClusters;
    FsFullSizeInfo->SectorsPerAllocationUnit = DeviceExt->NtfsInfo.SectorsPerCluster;
    FsFullSizeInfo->BytesPerSector = DeviceExt->NtfsInfo.BytesPerSector;
    ExReleaseResourceLite(&DeviceExt->BitmapResource);

    *BufferLength -= sizeof(FILE_FS_FULL_SIZE_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetFsDeviceInformation(PDEVICE_OBJECT DeviceObject,
                           PFILE_FS_DEVICE_INFORMATION FsDeviceInfo,
                           PULONG BufferLength)
{
    DPRINT("NtfsGetFsDeviceInformation()\n");
    DPRINT("FsDeviceInfo = %p\n", FsDeviceInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);
    DPRINT("Required length %lu\n", sizeof(FILE_FS_DEVICE_INFORMATION));

    if (*BufferLength < sizeof(FILE_FS_DEVICE_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    FsDeviceInfo->DeviceType = FILE_DEVICE_DISK;
    FsDeviceInfo->Characteristics = DeviceObject->Characteristics;

    DPRINT("NtfsGetFsDeviceInformation() finished.\n");

    *BufferLength -= sizeof(FILE_FS_DEVICE_INFORMATION);
    DPRINT("BufferLength %lu\n", *BufferLength);

    return STATUS_SUCCESS;
}


NTSTATUS
NtfsQueryVolumeInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    FS_INFORMATION_CLASS FsInformationClass;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status = STATUS_SUCCESS;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PDEVICE_EXTENSION DeviceExt;

    DPRINT("NtfsQueryVolumeInformation() called\n");

    ASSERT(IrpContext);

    Irp = IrpContext->Irp;
    DeviceObject = IrpContext->DeviceObject;
    DeviceExt = DeviceObject->DeviceExtension;
    Stack = IrpContext->Stack;

    if (!ExAcquireResourceSharedLite(&DeviceExt->DirResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    FsInformationClass = Stack->Parameters.QueryVolume.FsInformationClass;
    BufferLength = Stack->Parameters.QueryVolume.Length;
    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(SystemBuffer, BufferLength);

    DPRINT("FsInformationClass %d\n", FsInformationClass);
    DPRINT("SystemBuffer %p\n", SystemBuffer);

    switch (FsInformationClass)
    {
        case FileFsVolumeInformation:
            Status = NtfsGetFsVolumeInformation(DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileFsAttributeInformation:
            Status = NtfsGetFsAttributeInformation(DeviceObject->DeviceExtension,
                                                   SystemBuffer,
                                                   &BufferLength);
            break;

        case FileFsSizeInformation:
            Status = NtfsGetFsSizeInformation(DeviceObject,
                                              SystemBuffer,
                                              &BufferLength);
            break;

        case FileFsDeviceInformation:
            Status = NtfsGetFsDeviceInformation(DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileFsFullSizeInformation:
            Status = NtfsGetFsFullSizeInformation(DeviceObject,
                                                  SystemBuffer,
                                                  &BufferLength);
            break;

        case FileFsControlInformation:
            Status = NtfsQueryFsControl(DeviceExt, SystemBuffer, &BufferLength);
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
    }

    ExReleaseResourceLite(&DeviceExt->DirResource);

    /* An overflow still filled the fixed part, which the caller gets back */
    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
        Irp->IoStatus.Information =
            Stack->Parameters.QueryVolume.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}


NTSTATUS
NtfsSetVolumeInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp;
    PIO_STACK_LOCATION Stack;
    PDEVICE_EXTENSION DeviceExt;
    NTSTATUS Status;

    DPRINT("NtfsSetVolumeInformation() called\n");

    ASSERT(IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceExt = IrpContext->DeviceObject->DeviceExtension;
    Irp->IoStatus.Information = 0;

    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
        return STATUS_INVALID_DEVICE_REQUEST;

    switch (Stack->Parameters.SetVolume.FsInformationClass)
    {
        case FileFsControlInformation:
            Status = NtfsSetFsControl(DeviceExt, Irp->AssociatedIrp.SystemBuffer, Stack->Parameters.SetVolume.Length);
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
            break;
    }

    return Status;
}

/* EOF */
