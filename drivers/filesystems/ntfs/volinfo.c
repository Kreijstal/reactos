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

ULONGLONG
NtfsGetFreeClusters(PDEVICE_EXTENSION DeviceExt)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONGLONG BitmapDataSize;
    PCHAR BitmapData;
    ULONGLONG FreeClusters = 0;
    RTL_BITMAP Bitmap;

    DPRINT("NtfsGetFreeClusters(%p)\n", DeviceExt);

    /* Fast path: return the cached count if the cache is still valid.
     * NtfsAllocateClusters / FreeClusters invalidate it on every cluster
     * state change, so the value is always at least as fresh as the last
     * commit.  Reading the entire bitmap is O(volume_size) and fatally
     * slow on USB-MSC, so this cache is load-bearing. */
    if (InterlockedCompareExchange(&DeviceExt->CachedFreeClustersValid, 1, 1) == 1)
    {
        return DeviceExt->CachedFreeClusters;
    }

    BitmapRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        return 0;
    }

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return 0;
    }

    Status = FindAttribute(DeviceExt, BitmapRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return 0;
    }

    BitmapDataSize = AttributeDataLength(DataContext->pRecord);
    ASSERT((BitmapDataSize * 8) >= DeviceExt->NtfsInfo.ClusterCount);
    BitmapData = ExAllocatePoolWithTag(NonPagedPool, ROUND_UP(BitmapDataSize, DeviceExt->NtfsInfo.BytesPerSector), TAG_NTFS);
    if (BitmapData == NULL)
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return 0;
    }

    /* Read the entire bitmap in one ReadAttribute call.  Doing this
     * sector-by-sector (the previous code) issued ~600 separate I/Os on
     * a 10 GiB volume - fine on a fast disk, ~tens-of-seconds on USB-MSC.
     * ReadAttribute handles the run list internally and coalesces
     * contiguous extents, so the underlying I/O count drops to ~run-count. */
    ReadAttribute(DeviceExt, DataContext, 0, (PCHAR)BitmapData, (ULONG)BitmapDataSize);
    ReleaseAttributeContext(DataContext);

    DPRINT("Total clusters: %I64x\n", DeviceExt->NtfsInfo.ClusterCount);
    DPRINT("Total clusters in bitmap: %I64x\n", BitmapDataSize * 8);
    DPRINT("Diff in size: %I64d B\n", ((BitmapDataSize * 8) - DeviceExt->NtfsInfo.ClusterCount) * DeviceExt->NtfsInfo.SectorsPerCluster * DeviceExt->NtfsInfo.BytesPerSector);

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, DeviceExt->NtfsInfo.ClusterCount);
    FreeClusters = RtlNumberOfClearBits(&Bitmap);

    ExFreePoolWithTag(BitmapData, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);

    /* Cache result.  A concurrent racer may overwrite it with a slightly
     * different value, but the cache only needs to be "approximately right
     * between commits", which both readers compute. */
    DeviceExt->CachedFreeClusters = FreeClusters;
    InterlockedExchange(&DeviceExt->CachedFreeClustersValid, 1);

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
    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONGLONG BitmapDataSize;
    PUCHAR BitmapData;
    ULONGLONG FreeClusters = 0;
    RTL_BITMAP Bitmap;
    ULONG AssignedRun;
    ULONG LengthWritten;
    BOOLEAN BitmapLockHeld;

    DPRINT("NtfsAllocateClusters(%p, %lu, %lu, %p, %p)\n", DeviceExt, FirstDesiredCluster, DesiredClusters, FirstAssignedCluster, AssignedClusters);

    /* Serialize the entire read-modify-write of the volume bitmap. Without
     * this lock two concurrent allocators each load a private copy of the
     * bitmap, both find the same LCN free, both mark it set, and both
     * write the bitmap back - handing out the same cluster twice. The
     * writeback below reads $Bitmap's MFT record and may itself recurse
     * back into NtfsAllocateClusters if WriteAttribute needs to grow the
     * bitmap, but ERESOURCE supports recursive exclusive acquisition so
     * the inner call is harmless. See ntfs.h:BitmapResource and
     * Kreijstal/reactos#14. */
    BitmapLockHeld = ExAcquireResourceExclusiveLite(&DeviceExt->BitmapResource, TRUE);

    BitmapRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        if (BitmapLockHeld)
            ExReleaseResourceLite(&DeviceExt->BitmapResource);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&DeviceExt->BitmapResource);
        return Status;
    }

    Status = FindAttribute(DeviceExt, BitmapRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&DeviceExt->BitmapResource);
        return Status;
    }

    BitmapDataSize = AttributeDataLength(DataContext->pRecord);
    BitmapDataSize = min(BitmapDataSize, 0xffffffff);
    ASSERT((BitmapDataSize * 8) >= DeviceExt->NtfsInfo.ClusterCount);
    BitmapData = ExAllocatePoolWithTag(NonPagedPool, ROUND_UP(BitmapDataSize, DeviceExt->NtfsInfo.BytesPerSector), TAG_NTFS);
    if (BitmapData == NULL)
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&DeviceExt->BitmapResource);
        return  STATUS_INSUFFICIENT_RESOURCES;
    }

    DPRINT("Total clusters: %I64x\n", DeviceExt->NtfsInfo.ClusterCount);
    DPRINT("Total clusters in bitmap: %I64x\n", BitmapDataSize * 8);
    DPRINT("Diff in size: %I64d B\n", ((BitmapDataSize * 8) - DeviceExt->NtfsInfo.ClusterCount) * DeviceExt->NtfsInfo.SectorsPerCluster * DeviceExt->NtfsInfo.BytesPerSector);

    ReadAttribute(DeviceExt, DataContext, 0, (PCHAR)BitmapData, (ULONG)BitmapDataSize);

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, DeviceExt->NtfsInfo.ClusterCount);
    FreeClusters = RtlNumberOfClearBits(&Bitmap);

    if (FreeClusters < DesiredClusters)
    {
        ReleaseAttributeContext(DataContext);

        ExFreePoolWithTag(BitmapData, TAG_NTFS);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        if (BitmapLockHeld)
            ExReleaseResourceLite(&DeviceExt->BitmapResource);
        return STATUS_DISK_FULL;
    }

    // TODO: Observe MFT reservation zone

    // Can we get one contiguous run?
    AssignedRun = RtlFindClearBitsAndSet(&Bitmap, DesiredClusters, FirstDesiredCluster);

    if (AssignedRun != 0xFFFFFFFF)
    {
        *FirstAssignedCluster = AssignedRun;
        *AssignedClusters = DesiredClusters;
    }
    else
    {
        ULONG ForwardStart = 0;
        ULONG ForwardLength;
        ULONG LongestStart = 0;
        ULONG LongestLength;

        /* No single free extent is large enough, so the allocation has to be
         * split across several runs.  Pick the extent that covers as much of
         * the request as possible rather than the first one we stumble upon:
         * taking the nearest hole regardless of its size strip-mines the
         * volume's smallest gaps, so a caller that loops until ClustersNeeded
         * reaches zero can come back with hundreds of one-cluster runs.  Every
         * run costs a mapping pair in the attribute, and once those no longer
         * fit in an MFT record AddRun fails outright - which is how a directory
         * index that only needs a few clusters ends up unable to grow at all. */
        ForwardLength = RtlFindNextForwardRunClear(&Bitmap, FirstDesiredCluster, &ForwardStart);
        LongestLength = RtlFindLongestRunClear(&Bitmap, &LongestStart);

        /* Prefer the run at the caller's hint when it is no worse than the
         * best one available, since keeping the allocation close to the
         * previous run is what lets the MCB coalesce the two into one. */
        if (ForwardLength != 0 && ForwardLength >= LongestLength)
        {
            *FirstAssignedCluster = ForwardStart;
            *AssignedClusters = ForwardLength;
        }
        else
        {
            *FirstAssignedCluster = LongestStart;
            *AssignedClusters = LongestLength;
        }

        if (*AssignedClusters > DesiredClusters)
            *AssignedClusters = DesiredClusters;

        /* FreeClusters >= DesiredClusters was checked above, so at least one
         * free extent has to exist; handing back zero clusters would spin the
         * caller's "allocate until ClustersNeeded is satisfied" loop forever. */
        if (*AssignedClusters == 0)
        {
            DPRINT1("Bitmap reports %I64u free clusters but no free run found!\n", FreeClusters);
            ReleaseAttributeContext(DataContext);
            ExFreePoolWithTag(BitmapData, TAG_NTFS);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
            if (BitmapLockHeld)
                ExReleaseResourceLite(&DeviceExt->BitmapResource);
            return STATUS_DISK_FULL;
        }

        // Mark the allocated clusters as in-use in the bitmap
        RtlSetBits(&Bitmap, *FirstAssignedCluster, *AssignedClusters);
    }

    Status = WriteAttribute(DeviceExt, DataContext, 0, BitmapData, (ULONG)BitmapDataSize, &LengthWritten, BitmapRecord);

    ReleaseAttributeContext(DataContext);

    ExFreePoolWithTag(BitmapData, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);

    /* Cluster state changed; invalidate the cached free-clusters count.
     * Next IRP_MJ_QUERY_VOLUME_INFORMATION will recompute on demand. */
    InterlockedExchange(&DeviceExt->CachedFreeClustersValid, 0);

    if (BitmapLockHeld)
        ExReleaseResourceLite(&DeviceExt->BitmapResource);

    return Status;
}

static
NTSTATUS
NtfsGetFsVolumeInformation(PDEVICE_OBJECT DeviceObject,
                           PFILE_FS_VOLUME_INFORMATION FsVolumeInfo,
                           PULONG BufferLength)
{
    DPRINT("NtfsGetFsVolumeInformation() called\n");
    DPRINT("FsVolumeInfo = %p\n", FsVolumeInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);

    DPRINT("Vpb %p\n", DeviceObject->Vpb);

    DPRINT("Required length %lu\n",
           sizeof(FILE_FS_VOLUME_INFORMATION) + DeviceObject->Vpb->VolumeLabelLength);
    DPRINT("LabelLength %hu\n",
           DeviceObject->Vpb->VolumeLabelLength);
    DPRINT("Label %.*S\n",
           DeviceObject->Vpb->VolumeLabelLength / sizeof(WCHAR),
           DeviceObject->Vpb->VolumeLabel);

    if (*BufferLength < sizeof(FILE_FS_VOLUME_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    if (*BufferLength < (sizeof(FILE_FS_VOLUME_INFORMATION) + DeviceObject->Vpb->VolumeLabelLength))
        return STATUS_BUFFER_OVERFLOW;

    /* valid entries */
    FsVolumeInfo->VolumeSerialNumber = DeviceObject->Vpb->SerialNumber;
    FsVolumeInfo->VolumeLabelLength = DeviceObject->Vpb->VolumeLabelLength;
    memcpy(FsVolumeInfo->VolumeLabel,
           DeviceObject->Vpb->VolumeLabel,
           DeviceObject->Vpb->VolumeLabelLength);

    /* dummy entries */
    FsVolumeInfo->VolumeCreationTime.QuadPart = 0;
    FsVolumeInfo->SupportsObjects = FALSE;

    *BufferLength -= (sizeof(FILE_FS_VOLUME_INFORMATION) + DeviceObject->Vpb->VolumeLabelLength);

    DPRINT("BufferLength %lu\n", *BufferLength);
    DPRINT("NtfsGetFsVolumeInformation() done\n");

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetFsAttributeInformation(PDEVICE_EXTENSION DeviceExt,
                              PFILE_FS_ATTRIBUTE_INFORMATION FsAttributeInfo,
                              PULONG BufferLength)
{
    DPRINT("NtfsGetFsAttributeInformation()\n");
    DPRINT("FsAttributeInfo = %p\n", FsAttributeInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);
    DPRINT("Required length %lu\n", (sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 8));

    if (*BufferLength < sizeof (FILE_FS_ATTRIBUTE_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    if (*BufferLength < (sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 8))
        return STATUS_BUFFER_OVERFLOW;

    /* Report FILE_PERSISTENT_ACLS like Windows NTFS: the driver stores and
     * returns per-file security descriptors (see security.c / $SECURITY), and
     * FileInternalInformation returns a stable, unique IndexNumber (the MFT
     * record number).  POSIX layers such as the Cygwin/MSYS2 runtime gate their
     * "good inode" path on this flag; without it they fall back to synthesising
     * inodes by hashing the path name, which is both slower and (on the runtime
     * side) buggy, so an unpatched MSYS2 install misbehaves on ReactOS NTFS even
     * though it works on Windows NTFS.  FILE_READ_ONLY_VOLUME is reported only
     * when the mount gate forced the volume read-only (dirty or too-new
     * $LogFile, VCB_VOLUME_READ_ONLY); a normal read/write mount must not
     * set it. */
    FsAttributeInfo->FileSystemAttributes =
        FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_PERSISTENT_ACLS;
    if (BooleanFlagOn(DeviceExt->Flags, VCB_VOLUME_READ_ONLY))
        FsAttributeInfo->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
    FsAttributeInfo->MaximumComponentNameLength = 255;
    FsAttributeInfo->FileSystemNameLength = 8;

    memcpy(FsAttributeInfo->FileSystemName, L"NTFS", 8);

    DPRINT("Finished NtfsGetFsAttributeInformation()\n");

    *BufferLength -= (sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 8);
    DPRINT("BufferLength %lu\n", *BufferLength);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetFsSizeInformation(PDEVICE_OBJECT DeviceObject,
                         PFILE_FS_SIZE_INFORMATION FsSizeInfo,
                         PULONG BufferLength)
{
    PDEVICE_EXTENSION DeviceExt;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("NtfsGetFsSizeInformation()\n");
    DPRINT("FsSizeInfo = %p\n", FsSizeInfo);

    if (*BufferLength < sizeof(FILE_FS_SIZE_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    DeviceExt = DeviceObject->DeviceExtension;

    FsSizeInfo->AvailableAllocationUnits.QuadPart = NtfsGetFreeClusters(DeviceExt);
    FsSizeInfo->TotalAllocationUnits.QuadPart = DeviceExt->NtfsInfo.ClusterCount;
    FsSizeInfo->SectorsPerAllocationUnit = DeviceExt->NtfsInfo.SectorsPerCluster;
    FsSizeInfo->BytesPerSector = DeviceExt->NtfsInfo.BytesPerSector;

    DPRINT("Finished NtfsGetFsSizeInformation()\n");
    if (NT_SUCCESS(Status))
        *BufferLength -= sizeof(FILE_FS_SIZE_INFORMATION);

    return Status;
}


static
NTSTATUS
NtfsGetFsFullSizeInformation(PDEVICE_OBJECT DeviceObject,
                             PFILE_FS_FULL_SIZE_INFORMATION FsFullSizeInfo,
                             PULONG BufferLength)
{
    PDEVICE_EXTENSION DeviceExt;
    ULONGLONG FreeClusters;

    DPRINT("NtfsGetFsFullSizeInformation()\n");
    DPRINT("FsFullSizeInfo = %p\n", FsFullSizeInfo);

    if (*BufferLength < sizeof(FILE_FS_FULL_SIZE_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    DeviceExt = DeviceObject->DeviceExtension;
    FreeClusters = NtfsGetFreeClusters(DeviceExt);

    FsFullSizeInfo->TotalAllocationUnits.QuadPart = DeviceExt->NtfsInfo.ClusterCount;
    /* No per-user quota support: caller-available equals actual-available. */
    FsFullSizeInfo->CallerAvailableAllocationUnits.QuadPart = FreeClusters;
    FsFullSizeInfo->ActualAvailableAllocationUnits.QuadPart = FreeClusters;
    FsFullSizeInfo->SectorsPerAllocationUnit = DeviceExt->NtfsInfo.SectorsPerCluster;
    FsFullSizeInfo->BytesPerSector = DeviceExt->NtfsInfo.BytesPerSector;

    DPRINT("Finished NtfsGetFsFullSizeInformation()\n");

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
        return STATUS_BUFFER_OVERFLOW;

    FsDeviceInfo->DeviceType = FILE_DEVICE_DISK;
    FsDeviceInfo->Characteristics = DeviceObject->Characteristics;

    /* Surface the mount-time read-only gate (dirty or too-new $LogFile)
     * the same way a write-protected medium would be reported. */
    if (BooleanFlagOn(((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->Flags,
                      VCB_VOLUME_READ_ONLY))
    {
        FsDeviceInfo->Characteristics |= FILE_READ_ONLY_DEVICE;
    }

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

        case FileFsFullSizeInformation:
            Status = NtfsGetFsFullSizeInformation(DeviceObject,
                                                  SystemBuffer,
                                                  &BufferLength);
            break;

        case FileFsDeviceInformation:
            Status = NtfsGetFsDeviceInformation(DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
    }

    ExReleaseResourceLite(&DeviceExt->DirResource);

    if (NT_SUCCESS(Status))
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

    DPRINT("NtfsSetVolumeInformation() called\n");

    ASSERT(IrpContext);

    Irp = IrpContext->Irp;
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;

    return STATUS_NOT_SUPPORTED;
}

/* EOF */
