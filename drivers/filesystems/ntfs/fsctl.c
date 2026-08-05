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
 * FILE:             drivers/filesystems/ntfs/fsctl.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMER:       Eric Kohl
 *                   Valentin Verkhovsky
 *                   Pierre Schweitzer
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#include <ntdddisk.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Tests if the device contains a filesystem that can be mounted
 *           by this fsd.
 */
static
NTSTATUS
NtfsHasFileSystem(PDEVICE_OBJECT DeviceToMount)
{
    PARTITION_INFORMATION PartitionInfo;
    DISK_GEOMETRY DiskGeometry;
    ULONG ClusterSize, Size, k;
    PBOOT_SECTOR BootSector;
    NTSTATUS Status;

    DPRINT("NtfsHasFileSystem() called for device %p\n", DeviceToMount);

    Size = sizeof(DISK_GEOMETRY);
    Status = NtfsDeviceIoControl(DeviceToMount,
                                 IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                 NULL,
                                 0,
                                 &DiskGeometry,
                                 &Size,
                                 TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("NtfsDeviceIoControl() failed (Status %lx)\n", Status);
        return Status;
    }

    if (DiskGeometry.MediaType == FixedMedia)
    {
        /* We have found a hard disk */
        Size = sizeof(PARTITION_INFORMATION);
        Status = NtfsDeviceIoControl(DeviceToMount,
                                     IOCTL_DISK_GET_PARTITION_INFO,
                                     NULL,
                                     0,
                                     &PartitionInfo,
                                     &Size,
                                     TRUE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("NtfsDeviceIoControl() failed (Status %lx)\n", Status);
            return Status;
        }

        if (PartitionInfo.PartitionType != PARTITION_IFS)
        {
            DPRINT("Invalid partition type\n");
            return STATUS_UNRECOGNIZED_VOLUME;
        }
    }

    DPRINT("BytesPerSector: %lu\n", DiskGeometry.BytesPerSector);
    BootSector = ExAllocatePoolWithTag(NonPagedPool,
                                       DiskGeometry.BytesPerSector,
                                       TAG_NTFS);
    if (BootSector == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsReadSectors(DeviceToMount,
                             0,
                             1,
                             DiskGeometry.BytesPerSector,
                             (PVOID)BootSector,
                             TRUE);
    if (!NT_SUCCESS(Status))
    {
        goto ByeBye;
    }

    /*
     * Check values of different fields. If those fields have not expected
     * values, we fail, to avoid mounting partitions that Windows won't mount.
     */

    /* OEMID: this field must be NTFS */
    if (RtlCompareMemory(BootSector->OEMID, "NTFS    ", 8) != 8)
    {
        DPRINT1("Failed with NTFS-identifier: [%.8s]\n", BootSector->OEMID);
        Status = STATUS_UNRECOGNIZED_VOLUME;
        goto ByeBye;
    }

    /* Unused0: this field must be COMPLETELY null */
    for (k = 0; k < 7; k++)
    {
        if (BootSector->BPB.Unused0[k] != 0)
        {
            DPRINT1("Failed in field Unused0: [%.7s]\n", BootSector->BPB.Unused0);
            Status = STATUS_UNRECOGNIZED_VOLUME;
            goto ByeBye;
        }
    }

    /* Unused3: this field must be COMPLETELY null */
    for (k = 0; k < 4; k++)
    {
        if (BootSector->BPB.Unused3[k] != 0)
        {
            DPRINT1("Failed in field Unused3: [%.4s]\n", BootSector->BPB.Unused3);
            Status = STATUS_UNRECOGNIZED_VOLUME;
            goto ByeBye;
        }
    }

    /* Check cluster size */
    ClusterSize = BootSector->BPB.BytesPerSector * BootSector->BPB.SectorsPerCluster;
    if (ClusterSize != 512 && ClusterSize != 1024 &&
        ClusterSize != 2048 && ClusterSize != 4096 &&
        ClusterSize != 8192 && ClusterSize != 16384 &&
        ClusterSize != 32768 && ClusterSize != 65536)
    {
        DPRINT1("Cluster size failed: %hu, %hu, %hu\n",
                BootSector->BPB.BytesPerSector,
                BootSector->BPB.SectorsPerCluster,
                ClusterSize);
        Status = STATUS_UNRECOGNIZED_VOLUME;
        goto ByeBye;
    }

ByeBye:
    ExFreePool(BootSector);

    DPRINT1("NtfsHasFileSystem: returning 0x%lx\n", Status);
    return Status;
}


static
NTSTATUS
NtfsGetVolumeData(PDEVICE_OBJECT DeviceObject,
                  PDEVICE_EXTENSION DeviceExt)
{
    DISK_GEOMETRY DiskGeometry;
    PFILE_RECORD_HEADER VolumeRecord;
    PVOLINFO_ATTRIBUTE VolumeInfo;
    PBOOT_SECTOR BootSector;
    ULONG Size;
    PNTFS_INFO NtfsInfo = &DeviceExt->NtfsInfo;
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtxt;
    PNTFS_ATTR_RECORD Attribute;
    PNTFS_FCB VolumeFcb;
    PWSTR VolumeNameU;

    DPRINT("NtfsGetVolumeData() called\n");

    Size = sizeof(DISK_GEOMETRY);
    Status = NtfsDeviceIoControl(DeviceObject,
                                 IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                 NULL,
                                 0,
                                 &DiskGeometry,
                                 &Size,
                                 TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("NtfsDeviceIoControl() failed (Status %lx)\n", Status);
        return Status;
    }

    DPRINT("BytesPerSector: %lu\n", DiskGeometry.BytesPerSector);
    BootSector = ExAllocatePoolWithTag(NonPagedPool,
                                       DiskGeometry.BytesPerSector,
                                       TAG_NTFS);
    if (BootSector == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsReadSectors(DeviceObject,
                             0, /* Partition boot sector */
                             1,
                             DiskGeometry.BytesPerSector,
                             (PVOID)BootSector,
                             TRUE);
    if (!NT_SUCCESS(Status))
    {
        ExFreePool(BootSector);
        return Status;
    }

    /* Read data from the bootsector */
    NtfsInfo->BytesPerSector = BootSector->BPB.BytesPerSector;
    NtfsInfo->SectorsPerCluster = BootSector->BPB.SectorsPerCluster;
    NtfsInfo->BytesPerCluster = BootSector->BPB.BytesPerSector * BootSector->BPB.SectorsPerCluster;
    NtfsInfo->SectorCount = BootSector->EBPB.SectorCount;
    NtfsInfo->ClusterCount = DeviceExt->NtfsInfo.SectorCount / (ULONGLONG)DeviceExt->NtfsInfo.SectorsPerCluster;

    NtfsInfo->MftStart.QuadPart = BootSector->EBPB.MftLocation;
    NtfsInfo->MftMirrStart.QuadPart = BootSector->EBPB.MftMirrLocation;
    NtfsInfo->SerialNumber = BootSector->EBPB.SerialNumber;
    if (BootSector->EBPB.ClustersPerMftRecord > 0)
        NtfsInfo->BytesPerFileRecord = BootSector->EBPB.ClustersPerMftRecord * NtfsInfo->BytesPerCluster;
    else
        NtfsInfo->BytesPerFileRecord = 1 << (-BootSector->EBPB.ClustersPerMftRecord);
    if (BootSector->EBPB.ClustersPerIndexRecord > 0)
        NtfsInfo->BytesPerIndexRecord = BootSector->EBPB.ClustersPerIndexRecord * NtfsInfo->BytesPerCluster;
    else
        NtfsInfo->BytesPerIndexRecord = 1 << (-BootSector->EBPB.ClustersPerIndexRecord);

    DPRINT("Boot sector information:\n");
    DPRINT("  BytesPerSector:         %hu\n", BootSector->BPB.BytesPerSector);
    DPRINT("  SectorsPerCluster:      %hu\n", BootSector->BPB.SectorsPerCluster);
    DPRINT("  SectorCount:            %I64u\n", BootSector->EBPB.SectorCount);
    DPRINT("  MftStart:               %I64u\n", BootSector->EBPB.MftLocation);
    DPRINT("  MftMirrStart:           %I64u\n", BootSector->EBPB.MftMirrLocation);
    DPRINT("  ClustersPerMftRecord:   %lx\n", BootSector->EBPB.ClustersPerMftRecord);
    DPRINT("  ClustersPerIndexRecord: %lx\n", BootSector->EBPB.ClustersPerIndexRecord);
    DPRINT("  SerialNumber:           %I64x\n", BootSector->EBPB.SerialNumber);

    ExFreePool(BootSector);

    ExInitializeNPagedLookasideList(&DeviceExt->FileRecLookasideList,
                                    NULL, NULL, 0, NtfsInfo->BytesPerFileRecord, TAG_FILE_REC, 0);

    DeviceExt->MasterFileTable = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (DeviceExt->MasterFileTable == NULL)
    {
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsReadSectors(DeviceObject,
                             NtfsInfo->MftStart.u.LowPart * NtfsInfo->SectorsPerCluster,
                             NtfsInfo->BytesPerFileRecord / NtfsInfo->BytesPerSector,
                             NtfsInfo->BytesPerSector,
                             (PVOID)DeviceExt->MasterFileTable,
                             TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed reading MFT.\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return Status;
    }

    /* This raw sector read bypasses ReadFileRecord (MFTContext does not exist
     * yet - it is derived from this very record below), so the update sequence
     * array fixup that ReadFileRecord normally applies has to be done by hand.
     * DeviceExt->MasterFileTable is the *persistent* cached $MFT record 0 that
     * every later FindAttribute() walks, so leaving it in raw on-disk form
     * lodges the USN sentinel (the on-disk value at each sector tail, offsets
     * 510/1022/...) permanently inside the record.  That is invisible while an
     * attribute's data stops short of a sector tail, but a heavily fragmented
     * volume grows $MFT's own $BITMAP/$DATA mapping pairs across offset 510 -
     * the un-restored USN then reads as a bogus run header and the $Bitmap
     * runlist fails to decode ("Couldn't find $Bitmap attribute of master file
     * table"), which is workload-dependent exactly because it depends on the
     * mapping pairs reaching the tail. */
    Status = FixupUpdateSequenceArray(DeviceExt, &DeviceExt->MasterFileTable->Ntfs);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Invalid update sequence array in MFT record 0.\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return Status;
    }

    Status = FindAttribute(DeviceExt,
                           DeviceExt->MasterFileTable,
                           AttributeData,
                           L"",
                           0,
                           &DeviceExt->MFTContext,
                           &DeviceExt->MftDataOffset);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Can't find data attribute for Master File Table.\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return Status;
    }

    VolumeRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (VolumeRecord == NULL)
    {
        DPRINT1("Allocation failed for volume record\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Read Volume File (MFT index 3) */
    DeviceExt->StorageDevice = DeviceObject;
    Status = ReadFileRecord(DeviceExt,
                            NTFS_FILE_VOLUME,
                            VolumeRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed reading volume file\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, VolumeRecord);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return Status;
    }

    /* Enumerate attributes */
    NtfsDumpFileAttributes(DeviceExt, DeviceExt->MasterFileTable);

    /* Enumerate attributes */
    NtfsDumpFileAttributes(DeviceExt, VolumeRecord);

    /* Get volume name */
    Status = FindAttribute(DeviceExt, VolumeRecord, AttributeVolumeName, L"", 0, &AttrCtxt, NULL);

    if (NT_SUCCESS(Status) && AttrCtxt->pRecord->Resident.ValueLength != 0)
    {
        Attribute = AttrCtxt->pRecord;
        DPRINT("Data length %lu\n", AttributeDataLength(Attribute));
        NtfsInfo->VolumeLabelLength =
           min (Attribute->Resident.ValueLength, MAXIMUM_VOLUME_LABEL_LENGTH);
        RtlCopyMemory(NtfsInfo->VolumeLabel,
                      (PVOID)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset),
                      NtfsInfo->VolumeLabelLength);
        VolumeNameU = NtfsInfo->VolumeLabel;
    }
    else
    {
        NtfsInfo->VolumeLabelLength = 0;
        VolumeNameU = L"\0";
    }

    if (NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(AttrCtxt);
    }

    VolumeFcb = NtfsCreateFCB(VolumeNameU, NULL, DeviceExt);
    if (VolumeFcb == NULL)
    {
        DPRINT1("Failed allocating volume FCB\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, VolumeRecord);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    VolumeFcb->Flags = FCB_IS_VOLUME;
    VolumeFcb->RFCB.FileSize.QuadPart = DeviceExt->NtfsInfo.SectorCount * DeviceExt->NtfsInfo.BytesPerSector;
    VolumeFcb->RFCB.ValidDataLength = VolumeFcb->RFCB.FileSize;
    VolumeFcb->RFCB.AllocationSize = VolumeFcb->RFCB.FileSize;
    VolumeFcb->MFTIndex = 0;
    DeviceExt->VolumeFcb = VolumeFcb;

    /* Get volume information */
    Status = FindAttribute(DeviceExt, VolumeRecord, AttributeVolumeInformation, L"", 0, &AttrCtxt, NULL);

    if (NT_SUCCESS(Status) && AttrCtxt->pRecord->Resident.ValueLength != 0)
    {
        Attribute = AttrCtxt->pRecord;
        DPRINT("Data length %lu\n", AttributeDataLength (Attribute));
        VolumeInfo = (PVOID)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);

        NtfsInfo->MajorVersion = VolumeInfo->MajorVersion;
        NtfsInfo->MinorVersion = VolumeInfo->MinorVersion;
        NtfsInfo->Flags = VolumeInfo->Flags;
    }

    if (NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(AttrCtxt);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, VolumeRecord);

    /* MFT zone multiplier: read once (and range-validated) at DriverEntry
     * from ...\Control\FileSystem\NtfsMftZoneReservation. */
    NtfsInfo->MftZoneReservation = NtfsGlobalData->MftZoneReservation;

    return Status;
}


static
NTSTATUS
NtfsMountVolume(PDEVICE_OBJECT DeviceObject,
                PIRP Irp)
{
    PDEVICE_OBJECT NewDeviceObject = NULL;
    PDEVICE_OBJECT DeviceToMount;
    PIO_STACK_LOCATION Stack;
    PNTFS_FCB Fcb = NULL;
    PNTFS_CCB Ccb = NULL;
    PNTFS_VCB Vcb = NULL;
    NTSTATUS Status;
    BOOLEAN Lookaside = FALSE;

    DPRINT1("NtfsMountVolume() called\n");

    if (DeviceObject != NtfsGlobalData->DeviceObject)
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto ByeBye;
    }

    Stack = IoGetCurrentIrpStackLocation(Irp);
    DeviceToMount = Stack->Parameters.MountVolume.DeviceObject;

    Status = NtfsHasFileSystem(DeviceToMount);
    if (!NT_SUCCESS(Status))
    {
        goto ByeBye;
    }

    Status = IoCreateDevice(NtfsGlobalData->DriverObject,
                            sizeof(DEVICE_EXTENSION),
                            NULL,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &NewDeviceObject);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    NewDeviceObject->Flags |= DO_DIRECT_IO;
    Vcb = (PVOID)NewDeviceObject->DeviceExtension;
    RtlZeroMemory(Vcb, sizeof(NTFS_VCB));

    Vcb->Identifier.Type = NTFS_TYPE_VCB;
    Vcb->Identifier.Size = sizeof(NTFS_TYPE_VCB);

    Status = NtfsGetVolumeData(DeviceToMount,
                               Vcb);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    Lookaside = TRUE;

    NewDeviceObject->Vpb = DeviceToMount->Vpb;

    Vcb->StorageDevice = DeviceToMount;
    Vcb->StorageDevice->Vpb->DeviceObject = NewDeviceObject;
    Vcb->StorageDevice->Vpb->RealDevice = Vcb->StorageDevice;
    Vcb->StorageDevice->Vpb->Flags |= VPB_MOUNTED;
    NewDeviceObject->StackSize = Vcb->StorageDevice->StackSize + 1;
    NewDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    Vcb->StreamFileObject = IoCreateStreamFileObject(NULL,
                                                     Vcb->StorageDevice);

    InitializeListHead(&Vcb->FcbListHead);

    /* Hash index over that list; without it every path lookup scans the whole
     * volume's FCBs under a spinlock.  A mount without the index still works
     * (the lookups fall back to the linear scan), so a failure here is fatal
     * only because there is no reason to run a volume that slowly. */
    Status = NtfsInitializeFcbHash(Vcb);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    /* Initialise the directory-change-notification package (matches
     * FastFat/CDFS).  FsRtlNotifyInitializeSync can raise on allocation
     * failure, so guard it; the matching teardown is in the dismount /
     * mount-failure cleanup path. */
    InitializeListHead(&Vcb->NotifyList);
    Vcb->NotifySync = NULL;
    _SEH2_TRY
    {
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        _SEH2_YIELD(goto ByeBye);
    }
    _SEH2_END;

    Fcb = NtfsCreateFCB(NULL, NULL, Vcb);
    if (Fcb == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ByeBye;
    }

    Ccb = ExAllocatePoolWithTag(NonPagedPool,
                                sizeof(NTFS_CCB),
                                TAG_CCB);
    if (Ccb == NULL)
    {
        Status =  STATUS_INSUFFICIENT_RESOURCES;
        goto ByeBye;
    }

    RtlZeroMemory(Ccb, sizeof(NTFS_CCB));

    Ccb->Identifier.Type = NTFS_TYPE_CCB;
    Ccb->Identifier.Size = sizeof(NTFS_TYPE_CCB);

    Vcb->StreamFileObject->FsContext = Fcb;
    Vcb->StreamFileObject->FsContext2 = Ccb;
    Vcb->StreamFileObject->SectionObjectPointer = Fcb->SectionObjectPointers;
    Vcb->StreamFileObject->PrivateCacheMap = NULL;
    Vcb->StreamFileObject->Vpb = Vcb->Vpb;
    Ccb->PtrFileObject = Vcb->StreamFileObject;
    Fcb->FileObject = Vcb->StreamFileObject;
    Fcb->Vcb = (PDEVICE_EXTENSION)Vcb->StorageDevice;

    Fcb->Flags = FCB_IS_VOLUME_STREAM;

    Fcb->RFCB.FileSize.QuadPart = Vcb->NtfsInfo.SectorCount * Vcb->NtfsInfo.BytesPerSector;
    Fcb->RFCB.ValidDataLength.QuadPart = Vcb->NtfsInfo.SectorCount * Vcb->NtfsInfo.BytesPerSector;
    Fcb->RFCB.AllocationSize.QuadPart = Vcb->NtfsInfo.SectorCount * Vcb->NtfsInfo.BytesPerSector; /* Correct? */

//    Fcb->Entry.ExtentLocationL = 0;
//    Fcb->Entry.DataLengthL = DeviceExt->CdInfo.VolumeSpaceSize * BLOCKSIZE;

    _SEH2_TRY
    {
        CcInitializeCacheMap(Vcb->StreamFileObject,
                             (PCC_FILE_SIZES)(&Fcb->RFCB.AllocationSize),
                             TRUE,
                             &(NtfsGlobalData->CacheMgrCallbacks),
                             Fcb);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        goto ByeBye;
    }
    _SEH2_END;

    ExInitializeResourceLite(&Vcb->DirResource);
    ExInitializeResourceLite(&Vcb->IndexResource);
    ExInitializeResourceLite(&Vcb->BitmapResource);
    ExInitializeResourceLite(&Vcb->MftContextResource);
    ExInitializeResourceLite(&Vcb->MftMirrorResource);

    /* $MFTMirr refreshes may run from here on (see UpdateFileRecord). */
    Vcb->MftMirrorReady = TRUE;

    /* MftContextResource now exists: from here on ReadFileRecord serializes its
     * MFTContext walk against MFT growth (see MftReadLockReady in ntfs.h). */
    Vcb->MftReadLockReady = TRUE;

    KeInitializeSpinLock(&Vcb->FcbListLock);

    /* Get serial number */
    NewDeviceObject->Vpb->SerialNumber = Vcb->NtfsInfo.SerialNumber;

    /* Get volume label */
    NewDeviceObject->Vpb->VolumeLabelLength = Vcb->NtfsInfo.VolumeLabelLength;
    RtlCopyMemory(NewDeviceObject->Vpb->VolumeLabel,
                  Vcb->NtfsInfo.VolumeLabel,
                  Vcb->NtfsInfo.VolumeLabelLength);

    FsRtlNotifyVolumeEvent(Vcb->StreamFileObject, FSRTL_VOLUME_MOUNT);

    /* $UsnJrnl bootstrap (Kreijstal/reactos#33): if the volume already has
     * a journal on disk, bring its state into the Vcb so subsequent
     * FSCTLs and emission hooks see a live UsnJournalFcb.  A failure is
     * benign - the volume mounts, the journal simply stays inactive. */
    (void)NtfsUsnLoadJournal(Vcb);

    /* Disk-quota bootstrap (Kreijstal/reactos#37): probe \$Extend\$Quota
     * and cache presence + MFT index.  Absent quota is the common case;
     * IRP_MJ_{QUERY,SET}_QUOTA returns STATUS_NOT_SUPPORTED when absent. */
    (void)NtfsQuotaProbe(Vcb);

    /* Shared security-descriptor store: probe \$Secure and cache presence +
     * MFT index.  NTFS 3.0+ volumes keep every ACL there and reference it by
     * SecurityId, so without this the driver would fall back to a synthetic
     * descriptor for every file Windows ever secured.  Absent \$Secure is the
     * NTFS 1.2 / ReactOS-formatted case and simply leaves the per-record
     * $SECURITY_DESCRIPTOR attribute in charge. */
    (void)NtfsSecureProbe(Vcb);

    /* $LogFile Phase-1 clean-shutdown gate (Kreijstal/reactos#34).
     *
     * Parse both restart pages; on any irregularity (I/O failure, torn
     * backup, dirty landmark, byte-disagreement), force the volume into
     * read-only mode by setting VCB_VOLUME_READ_ONLY.  NtfsDispatch
     * short-circuits every mutating IRP major with
     * STATUS_MEDIA_WRITE_PROTECTED when that flag is set, so the rest
     * of the mount path proceeds exactly as it does for a clean volume
     * except that writes are refused.  Phase 2+ (replay, emission,
     * $Volume::DIRTY handshake) will clear this bit after replay.
     *
     * Mount itself is NOT failed on a dirty log: callers that want to
     * fsck / inspect such a volume still need to open files on it.
     * Rejecting writes is enough to prevent further corruption. */
    {
        NTSTATUS LogStatus = NtfsLfsCheckCleanShutdown(Vcb);
        if (!NT_SUCCESS(LogStatus))
        {
            DPRINT1("NtfsMountVolume: $LogFile dirty (0x%08lx); forcing read-only mount\n",
                    LogStatus);
            Vcb->Flags |= VCB_VOLUME_READ_ONLY;
        }
        else if (Vcb->LogFileVersionUnsupported)
        {
            /* The log is CLEAN but its restart page carries an LFS format
             * newer than this 1.x-era implementation (LFS 2.0, Windows 8+).
             * Mirror the Windows interop policy: a down-level NTFS must
             * not write such a volume, so gate it read-only instead of
             * emitting 1.x state a 2.0 replayer would not expect. */
            DPRINT1("NtfsMountVolume: $LogFile is LFS %u.%u (supported: %u.x); "
                    "volume gated read-only\n",
                    Vcb->LogFileMajorVersion, Vcb->LogFileMinorVersion,
                    NTFS_LFS_SUPPORTED_MAJOR_VERSION);
            Vcb->Flags |= VCB_VOLUME_READ_ONLY;
        }
    }

    /* $LogFile Phase-2 slice-2 write-context bootstrap (Kreijstal/reactos#34).
     *
     * Prepare the Vcb-resident logging context so the write-ahead-logging hook
     * in UpdateFileRecord can run if logging is later enabled.  Logging is
     * left DISABLED here: NtfsLfsMountInitWriteContext sets LoggingEnabled =
     * FALSE, so the metadata write path stays byte-for-byte identical to a
     * volume with no journal.  A failure to build the context is non-fatal -
     * the volume mounts and behaves exactly as today, just without the (still
     * inert) journal context. */
    (void)NtfsLfsMountInitWriteContext(Vcb);

    /* Seed the DIRTY-bit shadow from the platter.  This has to happen AFTER
     * NtfsLfsMountInitWriteContext, which resets it to a clean baseline.  A
     * volume that a previous boot flagged for chkdsk must not look clean to
     * NtfsMarkVolumeCorrupt, or we would rewrite $Volume on every mount. */
    (void)NtfsLfsQueryVolumeDirty(Vcb);

    /* Precompute and cache the free-cluster count so the first
     * IRP_MJ_QUERY_VOLUME_INFORMATION returns immediately.  On slow media
     * (USB-MSC) reading $Bitmap sector-by-sector takes tens of seconds;
     * doing it once here, behind the mount IRP rather than on the hot
     * loader path, hides that cost.  Failures fall back to lazy compute
     * (cache stays invalid). */
    {
        ULONGLONG FreeClusters = NtfsGetFreeClusters(Vcb);
        /* NtfsGetFreeClusters already stores the value and sets
         * CachedFreeClustersValid=1 on success; nothing else to do. */
        (void)FreeClusters;
    }

    Status = STATUS_SUCCESS;

ByeBye:
    if (!NT_SUCCESS(Status))
    {
        /* Cleanup */
        if (Vcb)
            NtfsUninitializeFcbHash(Vcb);

        if (Vcb && Vcb->NotifySync)
            FsRtlNotifyUninitializeSync(&Vcb->NotifySync);

        if (Vcb && Vcb->StreamFileObject)
            ObDereferenceObject(Vcb->StreamFileObject);

        if (Fcb)
            NtfsDestroyFCB(Fcb);

        if (Ccb)
            ExFreePool(Ccb);

        if (Lookaside)
            ExDeleteNPagedLookasideList(&Vcb->FileRecLookasideList);

        if (NewDeviceObject)
            IoDeleteDevice(NewDeviceObject);
    }

    DPRINT("NtfsMountVolume() done (Status: %lx)\n", Status);

    return Status;
}


static
NTSTATUS
NtfsVerifyVolume(PDEVICE_OBJECT DeviceObject,
                 PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    DPRINT1("NtfsVerifyVolume() called\n");
    return STATUS_WRONG_VOLUME;
}


static
NTSTATUS
GetNfsVolumeData(PDEVICE_EXTENSION DeviceExt,
                 PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    PNTFS_VOLUME_DATA_BUFFER DataBuffer;
    PNTFS_ATTR_RECORD Attribute;
    FIND_ATTR_CONTXT Context;
    NTSTATUS Status;

    DataBuffer = (PNTFS_VOLUME_DATA_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    Stack = IoGetCurrentIrpStackLocation(Irp);

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(NTFS_VOLUME_DATA_BUFFER) ||
        Irp->UserBuffer == NULL)
    {
        DPRINT1("Invalid output! %d %p\n", Stack->Parameters.FileSystemControl.OutputBufferLength, Irp->UserBuffer);
        return STATUS_INVALID_PARAMETER;
    }

    DataBuffer->VolumeSerialNumber.QuadPart = DeviceExt->NtfsInfo.SerialNumber;
    DataBuffer->NumberSectors.QuadPart = DeviceExt->NtfsInfo.SectorCount;
    DataBuffer->TotalClusters.QuadPart = DeviceExt->NtfsInfo.ClusterCount;
    DataBuffer->FreeClusters.QuadPart = NtfsGetFreeClusters(DeviceExt);
    DataBuffer->TotalReserved.QuadPart = 0LL; // FIXME
    DataBuffer->BytesPerSector = DeviceExt->NtfsInfo.BytesPerSector;
    DataBuffer->BytesPerCluster = DeviceExt->NtfsInfo.BytesPerCluster;
    DataBuffer->BytesPerFileRecordSegment = DeviceExt->NtfsInfo.BytesPerFileRecord;
    DataBuffer->ClustersPerFileRecordSegment = DeviceExt->NtfsInfo.BytesPerFileRecord / DeviceExt->NtfsInfo.BytesPerCluster;
    DataBuffer->MftStartLcn.QuadPart = DeviceExt->NtfsInfo.MftStart.QuadPart;
    DataBuffer->Mft2StartLcn.QuadPart = DeviceExt->NtfsInfo.MftMirrStart.QuadPart;
    DataBuffer->MftZoneStart.QuadPart = 0; // FIXME
    DataBuffer->MftZoneEnd.QuadPart = 0; // FIXME

    Status = FindFirstAttribute(&Context, DeviceExt, DeviceExt->MasterFileTable, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == AttributeData)
        {
            ASSERT(Attribute->IsNonResident);
            DataBuffer->MftValidDataLength.QuadPart = Attribute->NonResident.DataSize;

            break;
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }
    FindCloseAttribute(&Context);

    Irp->IoStatus.Information = sizeof(NTFS_VOLUME_DATA_BUFFER);

    if (Stack->Parameters.FileSystemControl.OutputBufferLength >= sizeof(NTFS_EXTENDED_VOLUME_DATA) + sizeof(NTFS_VOLUME_DATA_BUFFER))
    {
        PNTFS_EXTENDED_VOLUME_DATA ExtendedData = (PNTFS_EXTENDED_VOLUME_DATA)((ULONG_PTR)Irp->UserBuffer + sizeof(NTFS_VOLUME_DATA_BUFFER));

        ExtendedData->ByteCount = sizeof(NTFS_EXTENDED_VOLUME_DATA);
        ExtendedData->MajorVersion = DeviceExt->NtfsInfo.MajorVersion;
        ExtendedData->MinorVersion = DeviceExt->NtfsInfo.MinorVersion;
        Irp->IoStatus.Information += sizeof(NTFS_EXTENDED_VOLUME_DATA);
    }

    return STATUS_SUCCESS;
}


static
NTSTATUS
GetNtfsFileRecord(PDEVICE_EXTENSION DeviceExt,
                  PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PNTFS_FILE_RECORD_INPUT_BUFFER InputBuffer;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_FILE_RECORD_OUTPUT_BUFFER OutputBuffer;
    ULONGLONG MFTRecord;

    Stack = IoGetCurrentIrpStackLocation(Irp);

    if (Stack->Parameters.FileSystemControl.InputBufferLength < sizeof(NTFS_FILE_RECORD_INPUT_BUFFER) ||
        Irp->AssociatedIrp.SystemBuffer == NULL)
    {
        DPRINT1("Invalid input! %d %p\n", Stack->Parameters.FileSystemControl.InputBufferLength, Irp->AssociatedIrp.SystemBuffer);
        return STATUS_INVALID_PARAMETER;
    }

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < (FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + DeviceExt->NtfsInfo.BytesPerFileRecord) ||
        Irp->AssociatedIrp.SystemBuffer == NULL)
    {
        DPRINT1("Invalid output! %d %p\n", Stack->Parameters.FileSystemControl.OutputBufferLength, Irp->AssociatedIrp.SystemBuffer);
        return STATUS_BUFFER_TOO_SMALL;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    InputBuffer = (PNTFS_FILE_RECORD_INPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;

    MFTRecord = InputBuffer->FileReferenceNumber.QuadPart;
    DPRINT1("Requesting: %I64x\n", MFTRecord);

    do
    {
        Status = ReadFileRecord(DeviceExt, MFTRecord, FileRecord);
        if (NT_SUCCESS(Status))
        {
            if (FileRecord->Flags & FRH_IN_USE)
            {
                break;
            }
        }

        --MFTRecord;
    } while (TRUE);

    DPRINT1("Returning: %I64x\n", MFTRecord);
    OutputBuffer = (PNTFS_FILE_RECORD_OUTPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    OutputBuffer->FileReferenceNumber.QuadPart = MFTRecord;
    OutputBuffer->FileRecordLength = DeviceExt->NtfsInfo.BytesPerFileRecord;
    RtlCopyMemory(OutputBuffer->FileRecordBuffer, FileRecord, DeviceExt->NtfsInfo.BytesPerFileRecord);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    Irp->IoStatus.Information = FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + DeviceExt->NtfsInfo.BytesPerFileRecord;

    return STATUS_SUCCESS;
}


/**
 * @name NtfsIsVolumeDirty
 * @implemented
 *
 * FSCTL_IS_VOLUME_DIRTY: report whether the volume is flagged for chkdsk.
 * Returns a ULONG whose VOLUME_IS_DIRTY bit mirrors the on-disk $Volume
 * DIRTY flag, which is what `fsutil dirty query` prints.
 *
 * The flag is re-read from $Volume rather than answered from the Vcb shadow,
 * so the answer is what a checker running against the platter would see; the
 * shadow is only a cache to keep the set/clear handshake idempotent.
 */
static
NTSTATUS
NtfsIsVolumeDirty(PDEVICE_EXTENSION DeviceExt,
                  PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PULONG VolumeState;
    NTSTATUS Status;

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG))
        return STATUS_BUFFER_TOO_SMALL;

    if (Irp->AssociatedIrp.SystemBuffer == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsLfsQueryVolumeDirty(DeviceExt);
    if (!NT_SUCCESS(Status))
        return Status;

    VolumeState = (PULONG)Irp->AssociatedIrp.SystemBuffer;
    *VolumeState = DeviceExt->VolumeDirtyOnDisk ? VOLUME_IS_DIRTY : 0;
    Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}


/*
 * @name NtfsMarkVolumeDirty
 * @implemented
 *
 * FSCTL_MARK_VOLUME_DIRTY: set the on-disk $Volume DIRTY flag so a check is
 * scheduled for the next boot.  This is the counterpart of
 * FSCTL_IS_VOLUME_DIRTY above and what `fsutil dirty set` drives on Windows;
 * without it there was no supported way to ask for a check, only the driver's
 * internal NtfsMarkVolumeCorrupt on encountering damage.
 *
 * Takes no input and returns no output, matching the Windows FSCTL.
 */
static
NTSTATUS
NtfsMarkVolumeDirty(PDEVICE_EXTENSION DeviceExt,
                    PIRP Irp)
{
    UNREFERENCED_PARAMETER(Irp);

    if (DeviceExt->Flags & VCB_VOLUME_READ_ONLY)
        return STATUS_MEDIA_WRITE_PROTECTED;

    return NtfsLfsSetVolumeDirty(DeviceExt, TRUE);
}


static
NTSTATUS
GetVolumeBitmap(PDEVICE_EXTENSION DeviceExt,
                PIRP Irp)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION Stack;
    PVOLUME_BITMAP_BUFFER BitmapBuffer;
    LONGLONG StartingLcn;
    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONGLONG TotalClusters;
    ULONGLONG ToCopy;
    BOOLEAN Overflow = FALSE;

    DPRINT("GetVolumeBitmap(%p, %p)\n", DeviceExt, Irp);

    Stack = IoGetCurrentIrpStackLocation(Irp);

    if (Stack->Parameters.FileSystemControl.InputBufferLength < sizeof(STARTING_LCN_INPUT_BUFFER))
    {
        DPRINT1("Invalid input! %d\n", Stack->Parameters.FileSystemControl.InputBufferLength);
        return STATUS_INVALID_PARAMETER;
    }

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(VOLUME_BITMAP_BUFFER))
    {
        DPRINT1("Invalid output! %d\n", Stack->Parameters.FileSystemControl.OutputBufferLength);
        return STATUS_BUFFER_TOO_SMALL;
    }

    BitmapBuffer = NtfsGetUserBuffer(Irp, FALSE);
    if (Irp->RequestorMode == UserMode)
    {
        _SEH2_TRY
        {
            ProbeForRead(Stack->Parameters.FileSystemControl.Type3InputBuffer,
                         Stack->Parameters.FileSystemControl.InputBufferLength,
                         sizeof(CHAR));
            ProbeForWrite(BitmapBuffer, Stack->Parameters.FileSystemControl.OutputBufferLength,
                          sizeof(CHAR));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }
    else
    {
        if (Stack->Parameters.FileSystemControl.Type3InputBuffer == NULL ||
            BitmapBuffer == NULL)
        {
            Status = STATUS_INVALID_PARAMETER;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Invalid buffer! %p %p\n", Stack->Parameters.FileSystemControl.Type3InputBuffer, BitmapBuffer);
        return Status;
    }

    StartingLcn = ((PSTARTING_LCN_INPUT_BUFFER)Stack->Parameters.FileSystemControl.Type3InputBuffer)->StartingLcn.QuadPart;
    if (StartingLcn > DeviceExt->NtfsInfo.ClusterCount)
    {
        DPRINT1("Requested bitmap start beyond partition end: %I64x %I64x\n", DeviceExt->NtfsInfo.ClusterCount, StartingLcn);
        return STATUS_INVALID_PARAMETER;
    }

    /* Round down to a multiple of 8 */
    StartingLcn = StartingLcn & ~7;
    TotalClusters = DeviceExt->NtfsInfo.ClusterCount - StartingLcn;
    ToCopy = TotalClusters / 8;
    if ((ToCopy + FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)) > Stack->Parameters.FileSystemControl.OutputBufferLength)
    {
        DPRINT1("Buffer too small: %x, needed: %x\n", Stack->Parameters.FileSystemControl.OutputBufferLength, (ToCopy + FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)));
        Overflow = TRUE;
        ToCopy = Stack->Parameters.FileSystemControl.OutputBufferLength - FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    }

    BitmapRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed reading volume bitmap: %lx\n", Status);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return Status;
    }

    Status = FindAttribute(DeviceExt, BitmapRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed find $DATA for bitmap: %lx\n", Status);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return Status;
    }

    BitmapBuffer->StartingLcn.QuadPart = StartingLcn;
    BitmapBuffer->BitmapSize.QuadPart = ToCopy * 8;

    Irp->IoStatus.Information = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    _SEH2_TRY
    {
        Irp->IoStatus.Information += ReadAttribute(DeviceExt, DataContext, StartingLcn / 8, (PCHAR)BitmapBuffer->Buffer, ToCopy);
        Status = (Overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);

    return Status;
}


/*
 * IRP-bound wrapper for FSCTL_GET_RETRIEVAL_POINTERS.  Opens the unnamed
 * $DATA of the caller's file, then delegates to the MCB walker in
 * attrib.c so both the real driver and userspace tests can share the
 * extent-emission logic.
 */
static
NTSTATUS
GetRetrievalPointers(PDEVICE_EXTENSION DeviceExt,
                     PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    PRETRIEVAL_POINTERS_BUFFER OutBuffer;
    LONGLONG StartingVcn;
    ULONG BytesReturned = 0;
    ULONG OutLen;

    DPRINT("GetRetrievalPointers(%p, %p)\n", DeviceExt, Irp);

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;

    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;

    Fcb = FileObject->FsContext;

    if (Stack->Parameters.FileSystemControl.InputBufferLength < sizeof(STARTING_VCN_INPUT_BUFFER) ||
        Irp->AssociatedIrp.SystemBuffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    OutLen = Stack->Parameters.FileSystemControl.OutputBufferLength;
    if (OutLen < sizeof(RETRIEVAL_POINTERS_BUFFER))
        return STATUS_BUFFER_TOO_SMALL;

    StartingVcn = ((PSTARTING_VCN_INPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer)->StartingVcn.QuadPart;
    OutBuffer = (PRETRIEVAL_POINTERS_BUFFER)Irp->AssociatedIrp.SystemBuffer;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Use the handle's own stream - the unnamed $DATA for a plain open,
     * the named one for an alternate-data-stream handle. */
    Status = FindAttribute(DeviceExt, FileRecord, AttributeData, Fcb->Stream, wcslen(Fcb->Stream), &DataContext, NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (DataContext->pRecord->IsNonResident == 0)
    {
        /* Resident $DATA has no runlist -- report an empty map. */
        OutBuffer->ExtentCount = 0;
        OutBuffer->StartingVcn.QuadPart = StartingVcn;
        Irp->IoStatus.Information = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents);
        Status = STATUS_SUCCESS;
        goto Cleanup;
    }

    Status = NtfsFillRetrievalPointersFromMcb(&DataContext->DataRunsMCB,
                                              StartingVcn,
                                              (LONGLONG)DataContext->pRecord->NonResident.HighestVCN,
                                              OutBuffer,
                                              OutLen,
                                              &BytesReturned);
    Irp->IoStatus.Information = BytesReturned;

Cleanup:
    if (DataContext != NULL)
        ReleaseAttributeContext(DataContext);
    if (FileRecord != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}


/*
 * IRP-bound wrapper for FSCTL_EXTEND_VOLUME.  Parses the MSDN-documented
 * ULONGLONG NumberOfSectors input and delegates to NtfsExtendVolumeBitmap.
 * First-slice scope: grow $Bitmap + bump in-core ClusterCount only; boot
 * sector / $Volume rewrite deferred (see Kreijstal/reactos#32).
 */
static
NTSTATUS
ExtendVolume(PDEVICE_EXTENSION DeviceExt,
             PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    ULONGLONG NumberOfSectors = 0;
    ULONGLONG NewClusterCount;
    ULONGLONG OldClusterCount;

    DPRINT1("ExtendVolume(%p, %p)\n", DeviceExt, Irp);

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;

    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;

    Fcb = FileObject->FsContext;
    if (!(Fcb->Flags & FCB_IS_VOLUME))
        return STATUS_INVALID_PARAMETER;

    if (Stack->Parameters.FileSystemControl.InputBufferLength < sizeof(ULONGLONG) ||
        Irp->AssociatedIrp.SystemBuffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    _SEH2_TRY
    {
        NumberOfSectors = *(PULONGLONG)Irp->AssociatedIrp.SystemBuffer;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (DeviceExt->NtfsInfo.SectorsPerCluster == 0)
        return STATUS_INVALID_PARAMETER;

    NewClusterCount = NumberOfSectors / DeviceExt->NtfsInfo.SectorsPerCluster;
    OldClusterCount = DeviceExt->NtfsInfo.ClusterCount;

    if (NewClusterCount <= OldClusterCount)
    {
        /* MSDN: "If the file system does not support sector-aligned grow,
         * it may round down".  We treat "same or smaller" as a no-op
         * success so that callers probing the capability with a shrink-
         * equivalent call don't see a spurious error.  A true shrink
         * request should go through FSCTL_SHRINK_VOLUME. */
        return STATUS_SUCCESS;
    }

    Status = NtfsExtendVolumeBitmap(DeviceExt, OldClusterCount, NewClusterCount);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Persist the new volume size to the boot sector so the grow survives a
     * remount.  $Bitmap was already grown on disk above; updating
     * EBPB.SectorCount makes NtfsMountVolume() compute the larger ClusterCount
     * on the next mount.  Done after the bitmap grow on purpose: if this write
     * fails we leave the on-disk SectorCount unchanged, so a remount simply
     * sees the old (still-consistent) size instead of a half-applied grow.
     * The backup boot sector at the volume's last sector is refreshed too. */
    {
        ULONG BytesPerSector = DeviceExt->NtfsInfo.BytesPerSector;
        PUCHAR Sector = ExAllocatePoolWithTag(NonPagedPool, BytesPerSector, TAG_NTFS);

        if (Sector == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = NtfsReadSectors(DeviceExt->StorageDevice, 0, 1, BytesPerSector, Sector, FALSE);
        if (NT_SUCCESS(Status))
        {
            ((PBOOT_SECTOR)Sector)->EBPB.SectorCount = NumberOfSectors;

            Status = NtfsWriteDisk(DeviceExt->StorageDevice, 0, BytesPerSector, BytesPerSector, Sector);
            if (NT_SUCCESS(Status))
            {
                /* Backup boot sector lives at the last sector of the volume. */
                NtfsWriteDisk(DeviceExt->StorageDevice,
                              (LONGLONG)NumberOfSectors * BytesPerSector,
                              BytesPerSector, BytesPerSector, Sector);
            }
        }

        ExFreePoolWithTag(Sector, TAG_NTFS);

        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Last step: bump the in-core cluster count so follow-up allocations
     * can land in the freshly-grown region. */
    DeviceExt->NtfsInfo.ClusterCount = NewClusterCount;
    DeviceExt->NtfsInfo.SectorCount = NumberOfSectors;

    return STATUS_SUCCESS;
}


/*
 * IRP-bound wrapper for FSCTL_MOVE_FILE.  Resolves the caller's file
 * handle, opens its unnamed $DATA attribute, and hands the heavy lifting
 * (bitmap R/M/W, raw disk copy, MCB surgery, mapping-pairs rewrite) to
 * NtfsRelocateAttributeRange.  Compressed / encrypted sources are
 * refused up-front.  See Kreijstal/reactos#32 for deferred scope (cache
 * flush, paging-I/O draining).
 */
static
NTSTATUS
MoveFile(PDEVICE_EXTENSION DeviceExt,
         PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    MOVE_FILE_DATA MoveData;
    PFILE_OBJECT TargetFileObject = NULL;
    PNTFS_FCB TargetFcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    ULONG AttrOffset = 0;

    DPRINT1("MoveFile(%p, %p)\n", DeviceExt, Irp);

    Stack = IoGetCurrentIrpStackLocation(Irp);

    if (Stack->Parameters.FileSystemControl.InputBufferLength < sizeof(MOVE_FILE_DATA) ||
        Irp->AssociatedIrp.SystemBuffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    _SEH2_TRY
    {
        MoveData = *(PMOVE_FILE_DATA)Irp->AssociatedIrp.SystemBuffer;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (MoveData.ClusterCount == 0 ||
        MoveData.StartingVcn.QuadPart < 0 ||
        MoveData.StartingLcn.QuadPart < 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = ObReferenceObjectByHandle(MoveData.FileHandle,
                                       0,
                                       *IoFileObjectType,
                                       Irp->RequestorMode,
                                       (PVOID *)&TargetFileObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (TargetFileObject->FsContext == NULL)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }

    TargetFcb = TargetFileObject->FsContext;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Status = ReadFileRecord(DeviceExt, TargetFcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Move the handle's own stream - the unnamed $DATA for a plain open,
     * the named one for an alternate-data-stream handle. */
    Status = FindAttribute(DeviceExt, FileRecord, AttributeData,
                           TargetFcb->Stream, wcslen(TargetFcb->Stream),
                           &DataContext, &AttrOffset);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (!DataContext->pRecord->IsNonResident)
    {
        /* A resident $DATA has no LCNs to relocate.  Callers that hit
         * this with valid Vcn/Count are likely looking at a small file
         * -- return a stable error rather than silently succeeding. */
        Status = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }

    Status = NtfsRelocateAttributeRange(DeviceExt,
                                        DataContext,
                                        AttrOffset,
                                        FileRecord,
                                        MoveData.StartingVcn.QuadPart,
                                        MoveData.StartingLcn.QuadPart,
                                        MoveData.ClusterCount);

Cleanup:
    if (DataContext != NULL)
        ReleaseAttributeContext(DataContext);
    if (FileRecord != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    if (TargetFileObject != NULL)
        ObDereferenceObject(TargetFileObject);
    return Status;
}


static
NTSTATUS
LockOrUnlockVolume(PDEVICE_EXTENSION DeviceExt,
                   PIRP Irp,
                   BOOLEAN Lock)
{
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PIO_STACK_LOCATION Stack;

    DPRINT("LockOrUnlockVolume(%p, %p, %d)\n", DeviceExt, Irp, Lock);

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    Fcb = FileObject->FsContext;

    DPRINT1("LockOrUnlockVolume: Lock=%d FcbFlags=0x%x DevFlags=0x%x DevOHC=%u FcbOHC=%u FcbRefCount=%u\n",
            Lock, Fcb->Flags, DeviceExt->Flags, DeviceExt->OpenHandleCount,
            Fcb->OpenHandleCount, Fcb->RefCount);

    /* Only allow locking with the volume open */
    if (!(Fcb->Flags & FCB_IS_VOLUME))
    {
        DPRINT1("LockOrUnlockVolume: REJECT not FCB_IS_VOLUME\n");
        return STATUS_ACCESS_DENIED;
    }

    /* Bail out if it's already in the demanded state */
    if (((DeviceExt->Flags & VCB_VOLUME_LOCKED) && Lock) ||
        (!(DeviceExt->Flags & VCB_VOLUME_LOCKED) && !Lock))
    {
        DPRINT1("LockOrUnlockVolume: REJECT already in demanded state\n");
        return STATUS_ACCESS_DENIED;
    }

    /* Deny locking if we're not alone */
    if (Lock && DeviceExt->OpenHandleCount != 1)
    {
        DPRINT1("LockOrUnlockVolume: REJECT OpenHandleCount=%u (need 1)\n", DeviceExt->OpenHandleCount);
        return STATUS_ACCESS_DENIED;
    }

    /* Finally, proceed */
    if (Lock)
    {
        DeviceExt->Flags |= VCB_VOLUME_LOCKED;
    }
    else
    {
        DeviceExt->Flags &= ~VCB_VOLUME_LOCKED;
    }

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsDismountVolume(PDEVICE_OBJECT DeviceObject,
                   PIRP Irp)
{
    PDEVICE_EXTENSION DeviceExt;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PIO_STACK_LOCATION Stack;

    DeviceExt = DeviceObject->DeviceExtension;
    DPRINT1("NtfsDismountVolume(%p)\n", DeviceExt);

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    Fcb = FileObject->FsContext;

    /* Dismount may only be issued through a handle that holds the exclusive
     * volume lock.  FSCTL_LOCK_VOLUME requires OpenHandleCount == 1, which
     * guarantees no other code is currently touching the FCB list, the MFT
     * context, or the cached MFT bitmap when we tear them down. */
    if (!(DeviceExt->Flags & VCB_VOLUME_LOCKED))
    {
        DPRINT1("NtfsDismountVolume: REJECT volume not locked\n");
        return STATUS_ACCESS_DENIED;
    }

    if (!(Fcb->Flags & FCB_IS_VOLUME))
    {
        DPRINT1("NtfsDismountVolume: REJECT FileObject is not the volume\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (DeviceExt->Flags & VCB_DISMOUNT_PENDING)
    {
        DPRINT1("NtfsDismountVolume: REJECT already dismounted\n");
        return STATUS_VOLUME_DISMOUNTED;
    }

    FsRtlNotifyVolumeEvent(FileObject, FSRTL_VOLUME_DISMOUNT);

    /* Flush (and purge) the volume-stream cache before tearing the mount down.
     * A formatter that opened the raw volume (mkntfs during a quick format)
     * wrote the new $MFT/$Bitmap through the FCB_IS_VOLUME path, i.e. via
     * CcCopyWrite on Vcb->StreamFileObject (see NtfsWriteDiskCached) - those
     * writes land in this volume-stream cache as DIRTY pages, not on disk.
     * The formatter's own NtFlushBuffersFile flushes its handle's (volume FCB)
     * section, which has no cache map, so it is a no-op for that data.  If we
     * dismount without flushing here, the freshly written metadata reaches disk
     * only if the lazy writer happened to drain it first; the next mount reads
     * $MFT raw from disk (NtfsGetVolumeData) and intermittently sees a torn MFT
     * - some records still zero - so the very first create after format fails
     * with "ReadFileRecord failed: 0 read" / STATUS_PARTIAL_COPY.  Flushing on
     * dismount makes the formatter's writes durable deterministically (this
     * mirrors the shutdown-time NtfsFlushVolume and what Windows ntfs.sys does).
     * The volume is exclusively locked here (VCB_VOLUME_LOCKED), so no other
     * thread is touching the stream while we flush. */
    if (DeviceExt->StreamFileObject != NULL &&
        DeviceExt->StreamFileObject->SectionObjectPointer != NULL)
    {
        IO_STATUS_BLOCK FlushIosb;
        ULONG FlushRetry;

        /* CcFlushCache walks the volume-stream VACBs from offset 0 to the
         * cached volume size and STOPS at the first VACB whose flush fails
         * (ntoskrnl/cc/view.c: it breaks out of the loop and returns the
         * failing status), leaving every later dirty $MFT / $Bitmap page
         * un-written.  CcPurgeCacheSection then discards ALL remaining dirty
         * VACBs (ntoskrnl/cc/fs.c unmarks-and-frees dirty views without
         * writing them).  Purging after a partial flush therefore drops the
         * still-dirty tail of the metadata stream: the on-disk $MFT and
         * $Bitmap are persisted at inconsistent versions, which the next
         * mount sees as cross-linked / leaked clusters (a runlist references
         * a cluster the stale bitmap thinks is free -> a later FreeClusters
         * double-frees it).  This is the producer of the corruption seen after
         * an in-place upgrade, whose reboot dismounts the target volume.
         *
         * A transient CcRosFlushVacb failure (e.g. a momentary paging-write
         * error under the upgrade's memory / IO pressure) must NOT translate
         * into permanent metadata loss.  The volume is held exclusively locked
         * here (VCB_VOLUME_LOCKED), so nothing is dirtying the stream while we
         * flush - retry the flush until it drains cleanly, then purge.  Only
         * purge once the flush actually succeeded; a purge over dirty pages is
         * data loss, not a clean stop. */
        FlushRetry = 0;
        do
        {
            FlushIosb.Status = STATUS_SUCCESS;
            CcFlushCache(DeviceExt->StreamFileObject->SectionObjectPointer,
                         NULL, 0, &FlushIosb);
            if (NT_SUCCESS(FlushIosb.Status))
                break;

            DPRINT1("NtfsDismountVolume: volume-stream flush failed (0x%08lx), retry %lu\n",
                    FlushIosb.Status, FlushRetry + 1);
        }
        while (++FlushRetry < 100);

        if (NT_SUCCESS(FlushIosb.Status))
        {
            /* Metadata is durably on disk: safe to drop the cached views. */
            CcPurgeCacheSection(DeviceExt->StreamFileObject->SectionObjectPointer,
                                NULL, 0, FALSE);
        }
        else
        {
            /* The flush could not be completed after many retries (a genuine
             * device error).  Purging now would silently discard the dirty
             * metadata and corrupt the volume, so leave the dirty pages in the
             * cache and let volume teardown / the underlying storage error
             * surface instead. */
            DPRINT1("NtfsDismountVolume: giving up on volume-stream flush (0x%08lx); NOT purging dirty metadata\n",
                    FlushIosb.Status);
        }
    }

    ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, TRUE);

    /* $LogFile clean-dismount flush (Kreijstal/reactos#34 slice 2).  No-op
     * unless logging was enabled.  Runs while MFTContext / MasterFileTable are
     * still alive (it writes the CLEAN restart landmark and clears the on-disk
     * $Volume DIRTY bit), then tears the logging context down. */
    (void)NtfsLfsDismountFlush(DeviceExt);
    NtfsLfsTeardownWriteContext(DeviceExt);

    /* Drop the cached $MFT $Bitmap.  After dismount the on-disk bitmap may
     * have been rewritten under us (this is exactly what mkntfs does during
     * a quick format), so the next AllocateMftRecord must rebuild it from
     * the freshly read attribute. */
    if (DeviceExt->MftBitmapBuffer)
    {
        ExFreePoolWithTag(DeviceExt->MftBitmapBuffer, TAG_NTFS);
        DeviceExt->MftBitmapBuffer = NULL;
        DeviceExt->MftBitmapData = NULL;
        DeviceExt->MftBitmapSize = 0;
        DeviceExt->MftNextFreeHint = 0;
    }

    /* Release the in-memory MFT attribute context and the cached
     * $MFT file record header.  These describe the OLD layout of $MFT
     * (location, runlist, allocated size).  Anything that asks "where on
     * disk does MFT record N live?" reads the runlist out of MFTContext, so
     * leaving them in place after a quick format means every subsequent
     * ReadFileRecord/UpdateFileRecord goes to stale clusters and corrupts
     * the freshly written volume.  The next mount rebuilds them via
     * NtfsGetVolumeData.
     *
     * Note: we deliberately keep DeviceExt->FileRecLookasideList alive
     * because the volume FCB / volume stream FCB may still be referenced by
     * the locking handle until close, and freeing the lookaside list while
     * pool blocks allocated from it are still in use would tear down the
     * list while a back-pointer to it remains live. */
    if (DeviceExt->MFTContext)
    {
        ReleaseAttributeContext(DeviceExt->MFTContext);
        DeviceExt->MFTContext = NULL;
    }

    if (DeviceExt->MasterFileTable)
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList,
                                    DeviceExt->MasterFileTable);
        DeviceExt->MasterFileTable = NULL;
    }

    /* Mark the VCB dismounted and clear VPB_MOUNTED so the I/O Manager
     * sends IRP_MN_MOUNT_VOLUME on the next access, which routes to
     * NtfsMountVolume → NtfsGetVolumeData and rebuilds the in-memory
     * state from the (now freshly written) on-disk metadata.  The VPB
     * we want to clear is the one shared with the storage device, which
     * NtfsMountVolume hooked into the file system DeviceObject as
     * `NewDeviceObject->Vpb = DeviceToMount->Vpb;` - read it directly
     * from DeviceObject rather than from DeviceExt->Vpb (which the mount
     * path never assigns and is therefore NULL). */
    DeviceExt->Flags |= VCB_DISMOUNT_PENDING;
    if (DeviceObject->Vpb != NULL)
        DeviceObject->Vpb->Flags &= ~VPB_MOUNTED;

    ExReleaseResourceLite(&DeviceExt->DirResource);

    return STATUS_SUCCESS;
}


/*
 * IRP-bound wrappers for the plan-3 metadata round-trip FSCTLs.
 * Each wraps a worker (reparse.c / objid.c) that mutates an in-memory
 * FILE_RECORD_HEADER; this wrapper owns the ReadFileRecord / UpdateFileRecord
 * bookends plus the IRP buffer plumbing.
 */

static
NTSTATUS
FsctlSetReparsePoint(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PREPARSE_DATA_BUFFER Input;
    ULONG InputLength;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;
    Fcb = FileObject->FsContext;

    Input = (PREPARSE_DATA_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    InputLength = Stack->Parameters.FileSystemControl.InputBufferLength;
    if (Input == NULL || InputLength < REPARSE_DATA_BUFFER_HEADER_SIZE)
        return STATUS_INVALID_PARAMETER;
    if ((ULONG)Input->ReparseDataLength + REPARSE_DATA_BUFFER_HEADER_SIZE > InputLength)
        return STATUS_IO_REPARSE_DATA_INVALID;

    /* Invalidate FCB-level MFT record cache: reparse data about to change. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtfsSetReparsePointOnRecord(DeviceExt, FileRecord,
                                         Input->ReparseTag,
                                         (PUCHAR)Input + REPARSE_DATA_BUFFER_HEADER_SIZE,
                                         Input->ReparseDataLength);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);

Cleanup:
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

static
NTSTATUS
FsctlGetReparsePoint(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PREPARSE_DATA_BUFFER Output;
    ULONG OutputLength;
    ULONG Tag = 0;
    ULONG BodyLen = 0;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;
    Fcb = FileObject->FsContext;

    Output = (PREPARSE_DATA_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    OutputLength = Stack->Parameters.FileSystemControl.OutputBufferLength;
    if (Output == NULL || OutputLength < REPARSE_DATA_BUFFER_HEADER_SIZE)
        return STATUS_BUFFER_TOO_SMALL;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtfsGetReparsePointFromRecord(DeviceExt, FileRecord, &Tag,
                                           (PUCHAR)Output + REPARSE_DATA_BUFFER_HEADER_SIZE,
                                           OutputLength - REPARSE_DATA_BUFFER_HEADER_SIZE,
                                           &BodyLen);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Output->ReparseTag = Tag;
    Output->ReparseDataLength = (USHORT)BodyLen;
    Output->Reserved = 0;
    Irp->IoStatus.Information = REPARSE_DATA_BUFFER_HEADER_SIZE + BodyLen;

Cleanup:
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

static
NTSTATUS
FsctlDeleteReparsePoint(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord = NULL;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;
    Fcb = FileObject->FsContext;

    /* Invalidate FCB-level MFT record cache: reparse data about to be removed. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtfsDeleteReparsePointFromRecord(DeviceExt, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);

Cleanup:
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

static
NTSTATUS
FsctlGetObjectId(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PFILE_OBJECTID_BUFFER Output;
    ULONG OutputLength;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;
    Fcb = FileObject->FsContext;

    Output = (PFILE_OBJECTID_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    OutputLength = Stack->Parameters.FileSystemControl.OutputBufferLength;
    if (Output == NULL || OutputLength < sizeof(FILE_OBJECTID_BUFFER))
        return STATUS_BUFFER_TOO_SMALL;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RtlZeroMemory(Output, sizeof(*Output));
    Status = NtfsGetObjectIdFromRecord(DeviceExt, FileRecord, Output->ObjectId);
    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = sizeof(FILE_OBJECTID_BUFFER);

Cleanup:
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

static
NTSTATUS
FsctlSetObjectId(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PFILE_OBJECTID_BUFFER Input;
    ULONG InputLength;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;
    Fcb = FileObject->FsContext;

    Input = (PFILE_OBJECTID_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    InputLength = Stack->Parameters.FileSystemControl.InputBufferLength;
    if (Input == NULL || InputLength < sizeof(FILE_OBJECTID_BUFFER))
        return STATUS_INVALID_PARAMETER;

    /* Invalidate FCB-level MFT record cache: $OBJECT_ID about to change. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtfsSetObjectIdOnRecord(DeviceExt, FileRecord, Input->ObjectId);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);

Cleanup:
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

static
NTSTATUS
FsctlCreateOrGetObjectId(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord = NULL;
    PFILE_OBJECTID_BUFFER Output;
    ULONG OutputLength;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;
    Fcb = FileObject->FsContext;

    Output = (PFILE_OBJECTID_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    OutputLength = Stack->Parameters.FileSystemControl.OutputBufferLength;
    if (Output == NULL || OutputLength < sizeof(FILE_OBJECTID_BUFFER))
        return STATUS_BUFFER_TOO_SMALL;

    /* Invalidate FCB-level MFT record cache: may create a new $OBJECT_ID. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RtlZeroMemory(Output, sizeof(*Output));
    Status = NtfsGetObjectIdFromRecord(DeviceExt, FileRecord, Output->ObjectId);
    if (Status == STATUS_OBJECTID_NOT_FOUND)
    {
        UUID NewId;
        Status = ExUuidCreate(&NewId);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
        RtlCopyMemory(Output->ObjectId, &NewId, sizeof(NewId));
        Status = NtfsSetObjectIdOnRecord(DeviceExt, FileRecord, Output->ObjectId);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
        Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = sizeof(FILE_OBJECTID_BUFFER);

Cleanup:
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

static
NTSTATUS
FsctlDeleteObjectId(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord = NULL;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    if (FileObject == NULL || FileObject->FsContext == NULL)
        return STATUS_INVALID_PARAMETER;
    Fcb = FileObject->FsContext;

    /* Invalidate FCB-level MFT record cache: $OBJECT_ID about to be removed. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtfsDeleteObjectIdFromRecord(DeviceExt, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);

Cleanup:
    if (FileRecord)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}


/* FSCTL_CREATE_USN_JOURNAL IRP wrapper.  Accepts a CREATE_USN_JOURNAL_DATA
 * (MaximumSize + AllocationDelta) and delegates to the state-machine worker.
 * See usn.c banner for the first-slice scope; volumes without a pre-seeded
 * \$Extend\$UsnJrnl observe STATUS_NOT_IMPLEMENTED until the create path
 * lands (follow-up slice). */
static
NTSTATUS
FsctlCreateUsnJournal(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    PCREATE_USN_JOURNAL_DATA Input;
    ULONG InputLen;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    InputLen = Stack->Parameters.FileSystemControl.InputBufferLength;
    if (InputLen < sizeof(CREATE_USN_JOURNAL_DATA))
        return STATUS_INVALID_PARAMETER;
    Input = (PCREATE_USN_JOURNAL_DATA)Irp->AssociatedIrp.SystemBuffer;
    if (Input == NULL)
        return STATUS_INVALID_PARAMETER;

    return NtfsUsnInitJournal(DeviceExt, Input->MaximumSize,
                              Input->AllocationDelta);
}

/* FSCTL_READ_USN_JOURNAL IRP wrapper.  Matches the Windows contract: output
 * buffer is prefixed with an 8-byte next-USN followed by serialised
 * USN_RECORD_V2 entries. */
static
NTSTATUS
FsctlReadUsnJournal(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    PREAD_USN_JOURNAL_DATA Input;
    PVOID Output;
    ULONG OutputLen;
    ULONG BytesRet = 0;
    NTSTATUS Status;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    if (Stack->Parameters.FileSystemControl.InputBufferLength <
        sizeof(READ_USN_JOURNAL_DATA))
        return STATUS_INVALID_PARAMETER;

    Input = (PREAD_USN_JOURNAL_DATA)Irp->AssociatedIrp.SystemBuffer;
    Output = Irp->AssociatedIrp.SystemBuffer;
    OutputLen = Stack->Parameters.FileSystemControl.OutputBufferLength;
    if (Input == NULL || Output == NULL)
        return STATUS_INVALID_PARAMETER;

    if (DeviceExt->UsnJournalFcb == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (Input->UsnJournalID != 0 &&
        Input->UsnJournalID != DeviceExt->UsnJournalId)
        return STATUS_JOURNAL_NOT_ACTIVE;

    Status = NtfsUsnReadRange(DeviceExt,
                              (ULONGLONG)Input->StartUsn,
                              Input->ReasonMask,
                              Output,
                              OutputLen,
                              &BytesRet);
    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = BytesRet;
    return Status;
}

/* FSCTL_ENUM_USN_DATA IRP wrapper.  Input is MFT_ENUM_DATA, output is the
 * same [NextRef | records...] blob the worker produces. */
static
NTSTATUS
FsctlEnumUsnData(PDEVICE_EXTENSION DeviceExt, PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    PMFT_ENUM_DATA Input;
    PVOID Output;
    ULONG OutputLen;
    ULONG BytesRet = 0;
    NTSTATUS Status;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    if (Stack->Parameters.FileSystemControl.InputBufferLength <
        sizeof(MFT_ENUM_DATA))
        return STATUS_INVALID_PARAMETER;

    Input = (PMFT_ENUM_DATA)Irp->AssociatedIrp.SystemBuffer;
    Output = Irp->AssociatedIrp.SystemBuffer;
    OutputLen = Stack->Parameters.FileSystemControl.OutputBufferLength;
    if (Input == NULL || Output == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsUsnEnumerate(DeviceExt,
                              Input->StartFileReferenceNumber,
                              Input->LowUsn,
                              Input->HighUsn,
                              Output,
                              OutputLen,
                              &BytesRet);
    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information = BytesRet;
    return Status;
}


static
NTSTATUS
NtfsUserFsRequest(PDEVICE_OBJECT DeviceObject,
                  PIRP Irp)
{
    NTSTATUS Status;
    PIO_STACK_LOCATION Stack;
    PDEVICE_EXTENSION DeviceExt;

    DPRINT("NtfsUserFsRequest(%p, %p)\n", DeviceObject, Irp);

    Stack = IoGetCurrentIrpStackLocation(Irp);
    DeviceExt = DeviceObject->DeviceExtension;
    switch (Stack->Parameters.FileSystemControl.FsControlCode)
    {
        /* USN journal FSCTLs - first slice (Kreijstal/reactos#33).
         * CREATE / ENUM / READ are wired; the remaining four keep the
         * legacy "no journal" fallback for now, since their semantics
         * depend on bookkeeping (LowestValidUsn, CCB-cached reasons,
         * circular-wrap) that this slice doesn't implement. */
        case FSCTL_CREATE_USN_JOURNAL:
            Status = FsctlCreateUsnJournal(DeviceExt, Irp);
            break;

        case FSCTL_ENUM_USN_DATA:
            Status = FsctlEnumUsnData(DeviceExt, Irp);
            break;

        case FSCTL_READ_USN_JOURNAL:
            Status = FsctlReadUsnJournal(DeviceExt, Irp);
            break;

        case FSCTL_DELETE_USN_JOURNAL:
        case FSCTL_QUERY_USN_JOURNAL:
        case FSCTL_READ_FILE_USN_DATA:
        case FSCTL_WRITE_USN_CLOSE_RECORD:
            DPRINT("USN journal FSCTL deferred to follow-up slice: %x\n",
                   Stack->Parameters.FileSystemControl.FsControlCode);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;

        case FSCTL_GET_RETRIEVAL_POINTERS:
            Status = GetRetrievalPointers(DeviceExt, Irp);
            break;

        case FSCTL_SET_REPARSE_POINT:
            Status = FsctlSetReparsePoint(DeviceExt, Irp);
            break;

        case FSCTL_GET_REPARSE_POINT:
            Status = FsctlGetReparsePoint(DeviceExt, Irp);
            break;

        case FSCTL_DELETE_REPARSE_POINT:
            Status = FsctlDeleteReparsePoint(DeviceExt, Irp);
            break;

        case FSCTL_GET_OBJECT_ID:
            Status = FsctlGetObjectId(DeviceExt, Irp);
            break;

        case FSCTL_SET_OBJECT_ID:
            Status = FsctlSetObjectId(DeviceExt, Irp);
            break;

        case FSCTL_CREATE_OR_GET_OBJECT_ID:
            Status = FsctlCreateOrGetObjectId(DeviceExt, Irp);
            break;

        case FSCTL_DELETE_OBJECT_ID:
            Status = FsctlDeleteObjectId(DeviceExt, Irp);
            break;

        case FSCTL_EXTEND_VOLUME:
            Status = ExtendVolume(DeviceExt, Irp);
            break;

        case FSCTL_MOVE_FILE:
            Status = MoveFile(DeviceExt, Irp);
            break;

        //case FSCTL_GET_RETRIEVAL_POINTER_BASE:
        //case FSCTL_LOOKUP_STREAM_FROM_CLUSTER:
        case FSCTL_MARK_HANDLE:
        //case FSCTL_SHRINK_VOLUME:
            UNIMPLEMENTED;
            DPRINT1("Unimplemented user request: %x\n", Stack->Parameters.FileSystemControl.FsControlCode);
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case FSCTL_LOCK_VOLUME:
            Status = LockOrUnlockVolume(DeviceExt, Irp, TRUE);
            break;

        case FSCTL_UNLOCK_VOLUME:
            Status = LockOrUnlockVolume(DeviceExt, Irp, FALSE);
            break;

        case FSCTL_DISMOUNT_VOLUME:
            Status = NtfsDismountVolume(DeviceObject, Irp);
            break;

        case FSCTL_GET_NTFS_VOLUME_DATA:
            Status = GetNfsVolumeData(DeviceExt, Irp);
            break;

        case FSCTL_GET_NTFS_FILE_RECORD:
            Status = GetNtfsFileRecord(DeviceExt, Irp);
            break;

        case FSCTL_GET_VOLUME_BITMAP:
            Status = GetVolumeBitmap(DeviceExt, Irp);
            break;

        case FSCTL_IS_VOLUME_DIRTY:
            Status = NtfsIsVolumeDirty(DeviceExt, Irp);
            break;

        case FSCTL_MARK_VOLUME_DIRTY:
            Status = NtfsMarkVolumeDirty(DeviceExt, Irp);
            break;

        default:
            DPRINT("Invalid user request: %x\n", Stack->Parameters.FileSystemControl.FsControlCode);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return Status;
}


NTSTATUS
NtfsFileSystemControl(PNTFS_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;

    DPRINT("NtfsFileSystemControl() called\n");
    DPRINT("NtfsFileSystemControl: MinorFunction=%d\n", IrpContext->MinorFunction);

    DeviceObject = IrpContext->DeviceObject;
    Irp = IrpContext->Irp;
    Irp->IoStatus.Information = 0;

    switch (IrpContext->MinorFunction)
    {
        case IRP_MN_KERNEL_CALL:
            DPRINT1("NTFS: IRP_MN_USER_FS_REQUEST\n");
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;

        case IRP_MN_USER_FS_REQUEST:
            Status = NtfsUserFsRequest(DeviceObject, Irp);
            break;

        case IRP_MN_MOUNT_VOLUME:
            DPRINT("NTFS: IRP_MN_MOUNT_VOLUME\n");
            Status = NtfsMountVolume(DeviceObject, Irp);
            break;

        case IRP_MN_VERIFY_VOLUME:
            DPRINT1("NTFS: IRP_MN_VERIFY_VOLUME\n");
            Status = NtfsVerifyVolume(DeviceObject, Irp);
            break;

        default:
            DPRINT1("NTFS FSC: MinorFunction %d\n", IrpContext->MinorFunction);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return Status;
}

/* EOF */
