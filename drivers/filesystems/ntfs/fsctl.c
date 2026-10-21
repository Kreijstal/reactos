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

#include "lfswrite.h"

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
    PARTITION_INFORMATION_EX PartitionInfo;
    DISK_GEOMETRY DiskGeometry;
    ULONG ClusterSize, Size, k;
    PBOOT_SECTOR BootSector;
    NTSTATUS Status;

    DPRINT("NtfsHasFileSystem() called\n");

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
        DPRINT1("NtfsDeviceIoControl() failed (Status %lx)\n", Status);
        return Status;
    }

    if (DiskGeometry.MediaType == FixedMedia)
    {
        /* We have found a hard disk. The partition entry is only a hint:
         * the boot sector below decides whether this is NTFS. Use the _EX
         * query, since IOCTL_DISK_GET_PARTITION_INFO fails on anything but
         * MBR, and only reject an MBR partition of a non-IFS type. A GPT
         * partition has a type GUID, not a type byte, to compare against. */
        Size = sizeof(PARTITION_INFORMATION_EX);
        Status = NtfsDeviceIoControl(DeviceToMount,
                                     IOCTL_DISK_GET_PARTITION_INFO_EX,
                                     NULL,
                                     0,
                                     &PartitionInfo,
                                     &Size,
                                     TRUE);
        if (NT_SUCCESS(Status) &&
            PartitionInfo.PartitionStyle == PARTITION_STYLE_MBR &&
            PartitionInfo.Mbr.PartitionType != PARTITION_IFS)
        {
            DPRINT1("Invalid partition type 0x%02x\n", PartitionInfo.Mbr.PartitionType);
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

    /* Check cluster size: a power of two from 512 bytes to 2 MiB (Windows 10
     * 1709 and later format clusters over 64 KiB) */
    ClusterSize = BootSector->BPB.BytesPerSector *
                  NtfsDecodeSectorsPerCluster(BootSector->BPB.SectorsPerCluster);
    if (ClusterSize < 512 || ClusterSize > NTFS_MAX_CLUSTER_SIZE ||
        (ClusterSize & (ClusterSize - 1)) != 0)
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

    return Status;
}


static
ULONG
NtfsQueryMftZoneReservation(VOID)
{
    ULONG ZoneReservation = 1;
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];

    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    QueryTable[0].Name = L"NtfsMftZoneReservation";
    QueryTable[0].EntryContext = &ZoneReservation;

    RtlQueryRegistryValues(RTL_REGISTRY_CONTROL,
                           L"FileSystem",
                           QueryTable,
                           NULL,
                           NULL);

    return ZoneReservation;
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
    NtfsInfo->SectorsPerCluster = NtfsDecodeSectorsPerCluster(BootSector->BPB.SectorsPerCluster);
    NtfsInfo->BytesPerCluster = BootSector->BPB.BytesPerSector * NtfsInfo->SectorsPerCluster;
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

    /* The raw read bypasses ReadFileRecord, so restore the update sequence
     * array here. Every later lookup walks this cached record, and $MFT's
     * own mapping pairs may run across a sector tail. */
    Status = FixupUpdateSequenceArray(DeviceExt, &DeviceExt->MasterFileTable->Ntfs);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Invalid update sequence array in MFT record 0.\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, DeviceExt->MasterFileTable);
        ExDeleteNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        return Status;
    }

    /* Non-resident $MFT attribute lists need the storage device during lookup. */
    DeviceExt->StorageDevice = DeviceObject;

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

    NtfsInfo->MftZoneReservation = NtfsQueryMftZoneReservation();

    /* NTFS defines its own case folding through $UpCase; without it every
     * case-insensitive comparison on this volume would use the wrong table. */
    Status = NtfsLoadUpCaseTable(DeviceExt);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to load $UpCase (Status %lx)\n", Status);
        return Status;
    }

    return Status;
}


/* The log's barrier: Length 0 makes every write so far durable, the
 * volume stream's included; otherwise only the device's own cache. */
static NTSTATUS
NtfsLogFlush(PDEVICE_EXTENSION Vcb, LONGLONG Offset, ULONG Length)
{
    UNREFERENCED_PARAMETER(Offset);
    if (Length == 0)
        return NtfsFlushVolumeStream(Vcb);
    return NtfsFlushDevice(Vcb->StorageDevice);
}

/* A forced log page goes to the device; its cached copy is written too,
 * so a read of the volume sees it. */
static NTSTATUS
NtfsLogWriteDirect(PDEVICE_EXTENSION Vcb, LONGLONG Offset, ULONG Length, const PUCHAR Buffer)
{
    if (Vcb->Flags & VCB_DEVICE_REMOVED)
        return STATUS_DEVICE_REMOVED;
    return NtfsWriteDisk(Vcb->StorageDevice, Offset, Length, Vcb->NtfsInfo.BytesPerSector, Buffer);
}

/* Recovery rewrote system file records the mount had already read: $MFT's
 * own record and $Volume's. */
static NTSTATUS
NtfsReloadVolumeRecords(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PVOLINFO_ATTRIBUTE VolumeInfo;
    NTSTATUS Status;

    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = NtfsReadDiskCached(Vcb, Vcb->NtfsInfo.MftStart.QuadPart * Vcb->NtfsInfo.BytesPerCluster,
                                Vcb->NtfsInfo.BytesPerFileRecord, (PUCHAR)Record);
    if (NT_SUCCESS(Status))
        Status = FixupUpdateSequenceArray(Vcb, &Record->Ntfs);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"", 0, &Context, &Vcb->MftDataOffset);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
        return Status;
    }
    ExAcquireResourceExclusiveLite(&Vcb->MftContextResource, TRUE);
    RtlCopyMemory(Vcb->MasterFileTable, Record, Vcb->NtfsInfo.BytesPerFileRecord);
    ReleaseAttributeContext(Vcb->MFTContext);
    Vcb->MFTContext = Context;
    ExReleaseResourceLite(&Vcb->MftContextResource);

    Status = ReadFileRecord(Vcb, NTFS_FILE_VOLUME, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeVolumeInformation, L"", 0, &Context, NULL);
    if (NT_SUCCESS(Status))
    {
        if (!Context->pRecord->IsNonResident && Context->pRecord->Resident.ValueLength >= sizeof(*VolumeInfo))
        {
            VolumeInfo = (PVOID)((ULONG_PTR)Context->pRecord + Context->pRecord->Resident.ValueOffset);
            Vcb->NtfsInfo.MajorVersion = VolumeInfo->MajorVersion;
            Vcb->NtfsInfo.MinorVersion = VolumeInfo->MinorVersion;
            Vcb->NtfsInfo.Flags = VolumeInfo->Flags;
        }
        ReleaseAttributeContext(Context);
    }
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    return Status;
}

/* Brings $LogFile to a clean state before anything reads metadata it may
 * describe, as NTFS does at mount: a dirty log is recovered.  A log that
 * cannot be recovered leaves the volume read-only; when it is one this
 * driver wrote, chkdsk is due (it repairs the volume and resets the log).
 * A log in another format is Windows': it needs Windows' recovery (the
 * volume may be hibernated), and is left alone. */
static NTSTATUS
NtfsMountLog(PDEVICE_EXTENSION Vcb)
{
    NTFS_LOG_RECOVERY_RESULT Result;
    NTSTATUS Status;

    if (NtfsDeviceIoControl(Vcb->StorageDevice, IOCTL_DISK_IS_WRITABLE, NULL, 0,
                            NULL, NULL, FALSE) == STATUS_MEDIA_WRITE_PROTECTED)
        Vcb->Flags |= VCB_VOLUME_READ_ONLY;

    Status = NtfsLfsCheckCleanShutdown(Vcb);
    if (NT_SUCCESS(Status) && !Vcb->LogFileVersionUnsupported)
        return STATUS_SUCCESS;
    if (Status == STATUS_LOG_FILE_FULL && !Vcb->LogFileVersionUnsupported &&
        !(Vcb->Flags & VCB_VOLUME_READ_ONLY))
    {
        Status = NtfsLfsRecover(Vcb, &Result);
        if (NT_SUCCESS(Status))
        {
            if (Result.SystemRecords)
            {
                Status = NtfsReloadVolumeRecords(Vcb);
                if (!NT_SUCCESS(Status))
                    return Status;
            }
            if (Result.NeedsCheck)
                NtfsMarkVolumeCorrupt(Vcb);
            return STATUS_SUCCESS;
        }
    }

    DPRINT1("Mounting read-only: $LogFile %s (%lx)\n",
            Vcb->LogFileVersionUnsupported ? "has an unsupported version" : "cannot be recovered", Status);
    if (!Vcb->LogFileVersionUnsupported && Status != STATUS_NOT_SUPPORTED)
    {
        /* The mark is the last write: on disk before the volume turns
         * read-only, which no later flush may write through. */
        NtfsMarkVolumeCorrupt(Vcb);
        if (!(Vcb->Flags & VCB_VOLUME_READ_ONLY) && !NT_SUCCESS(NtfsFlushVolumeStream(Vcb)))
            DPRINT1("Couldn't write the dirty mark\n");
    }
    Vcb->Flags |= VCB_VOLUME_READ_ONLY;
    return STATUS_SUCCESS;
}

static NTSTATUS
NtfsMountStartLog(PDEVICE_EXTENSION Vcb)
{
    NTSTATUS Status;

    if (Vcb->Flags & VCB_VOLUME_READ_ONLY)
        return STATUS_SUCCESS;
    Status = NtfsLfsCreateWriter(Vcb, NtfsWriteDiskCached, NtfsLogWriteDirect, NtfsLogFlush,
                                 &Vcb->LfsWriter);
    if (!NT_SUCCESS(Status))
        return Status;
    /* Metadata is logged from here on, as Windows does; a log the writer
     * cannot use leaves the volume as unprotected as before. */
    ExAcquireResourceExclusiveLite(&Vcb->IoResource, TRUE);
    ExAcquireResourceExclusiveLite(&Vcb->DirResource, TRUE);
    Status = NtfsLfsEnableWriter(Vcb->LfsWriter);
    ExReleaseResourceLite(&Vcb->DirResource);
    ExReleaseResourceLite(&Vcb->IoResource);
    if (!NT_SUCCESS(Status))
        DPRINT1("$LogFile is not written on this volume (%lx)\n", Status);
    return STATUS_SUCCESS;
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
    BOOLEAN MftResources = FALSE;
    GET_LENGTH_INFORMATION VolumeLength;
    ULONG LengthSize = sizeof(VolumeLength);

    DPRINT("NtfsMountVolume() called\n");

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

    ExInitializeResourceLite(&Vcb->MftAllocationResource);
    ExInitializeResourceLite(&Vcb->MftContextResource);
    MftResources = TRUE;

    Status = NtfsGetVolumeData(DeviceToMount,
                               Vcb);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    Lookaside = TRUE;
    ExInitializeResourceLite(&Vcb->IoResource);
    ExInitializeResourceLite(&Vcb->PagingResource);
    ExInitializeResourceLite(&Vcb->DirResource);
    ExInitializeResourceLite(&Vcb->IndexResource);
    ExInitializeResourceLite(&Vcb->BitmapResource);
    ExInitializeResourceLite(&Vcb->QuotaResource);
    KeInitializeSpinLock(&Vcb->FcbListLock);

    InitializeListHead(&Vcb->NotifyList);
    _SEH2_TRY
    {
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    NewDeviceObject->Vpb = DeviceToMount->Vpb;
    Vcb->Vpb = NewDeviceObject->Vpb;

    Vcb->StorageDevice = DeviceToMount;
    Vcb->StorageDevice->Vpb->DeviceObject = NewDeviceObject;
    Vcb->StorageDevice->Vpb->RealDevice = Vcb->StorageDevice;
    Vcb->StorageDevice->Vpb->Flags |= VPB_MOUNTED;
    NewDeviceObject->StackSize = Vcb->StorageDevice->StackSize + 1;
    NewDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    Vcb->StreamFileObject = IoCreateStreamFileObject(NULL,
                                                     Vcb->StorageDevice);

    InitializeListHead(&Vcb->FcbListHead);
    InitializeListHead(&Vcb->FileLockList);
    Status = NtfsInitializeFcbHash(Vcb);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

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
    Vcb->StreamFileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    Vcb->StreamFileObject->PrivateCacheMap = NULL;
    Vcb->StreamFileObject->Vpb = Vcb->Vpb;
    Ccb->PtrFileObject = Vcb->StreamFileObject;
    Fcb->FileObject = Vcb->StreamFileObject;
    Fcb->Vcb = Vcb;

    Fcb->Flags = FCB_IS_VOLUME_STREAM;

    Status = NtfsDeviceIoControl(Vcb->StorageDevice, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                                  &VolumeLength, &LengthSize, FALSE);
    if (!NT_SUCCESS(Status))
        goto ByeBye;
    Fcb->RFCB.FileSize = VolumeLength.Length;
    Fcb->RFCB.ValidDataLength = VolumeLength.Length;
    Fcb->RFCB.AllocationSize = VolumeLength.Length;

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

    Status = NtfsMountLog(Vcb);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    Status = NtfsLoadVolumeBitmap(Vcb);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    Status = NtfsMountStartLog(Vcb);
    if (!NT_SUCCESS(Status))
        goto ByeBye;

    NtfsFindObjectIdIndex(Vcb);
    NtfsInitializeSecurity(Vcb);
    NtfsLoadUsnJournal(Vcb);
    NtfsLoadQuota(Vcb);

    /* Get serial number */
    NewDeviceObject->Vpb->SerialNumber = Vcb->NtfsInfo.SerialNumber;

    /* Get volume label */
    NewDeviceObject->Vpb->VolumeLabelLength = Vcb->NtfsInfo.VolumeLabelLength;
    RtlCopyMemory(NewDeviceObject->Vpb->VolumeLabel,
                  Vcb->NtfsInfo.VolumeLabel,
                  Vcb->NtfsInfo.VolumeLabelLength);

    FsRtlNotifyVolumeEvent(Vcb->StreamFileObject, FSRTL_VOLUME_MOUNT);

    Status = STATUS_SUCCESS;

ByeBye:
    if (!NT_SUCCESS(Status))
    {
        /* Cleanup */
        if (Vcb && Vcb->StreamFileObject)
        {
            if (Vcb->StreamFileObject->PrivateCacheMap != NULL)
            {
                CcPurgeCacheSection(Vcb->StreamFileObject->SectionObjectPointer, NULL, 0, FALSE);
                CcUninitializeCacheMap(Vcb->StreamFileObject, NULL, NULL);
            }
            /* Fcb and Ccb are freed below, not by this file object's close. */
            Vcb->StreamFileObject->FsContext = NULL;
            Vcb->StreamFileObject->FsContext2 = NULL;
            ObDereferenceObject(Vcb->StreamFileObject);
        }
        if (Vcb != NULL && Vcb->Vpb != NULL)
        {
            KIRQL OldIrql;

            IoAcquireVpbSpinLock(&OldIrql);
            Vcb->Vpb->Flags &= ~VPB_MOUNTED;
            Vcb->Vpb->DeviceObject = NULL;
            IoReleaseVpbSpinLock(OldIrql);
        }

        if (Fcb)
            NtfsDestroyFCB(Fcb);

        if (Ccb)
            ExFreePool(Ccb);

        if (Vcb != NULL)
        {
            NtfsLfsDestroyWriter(Vcb->LfsWriter);
            Vcb->LfsWriter = NULL;
        }

        if (Vcb != NULL)
        {
            if (Vcb->NotifySync != NULL)
                FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
            NtfsUninitializeFcbHash(Vcb);
            NtfsFreeVolumeBitmap(Vcb);
        }

        if (Vcb != NULL && Vcb->UpCaseTable != NULL)
        {
            ExFreePoolWithTag(Vcb->UpCaseTable, TAG_NTFS);
            Vcb->UpCaseTable = NULL;
        }

        if (MftResources)
        {
            ExDeleteResourceLite(&Vcb->MftContextResource);
            ExDeleteResourceLite(&Vcb->MftAllocationResource);
        }
        if (Lookaside)
        {
            ExDeleteResourceLite(&Vcb->BitmapResource);
            ExDeleteResourceLite(&Vcb->QuotaResource);
            ExDeleteResourceLite(&Vcb->IndexResource);
            ExDeleteResourceLite(&Vcb->DirResource);
            ExDeleteResourceLite(&Vcb->PagingResource);
            ExDeleteResourceLite(&Vcb->IoResource);
            ExDeleteNPagedLookasideList(&Vcb->FileRecLookasideList);
        }

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

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(NTFS_VOLUME_DATA_BUFFER))
        return STATUS_BUFFER_TOO_SMALL;

    ExAcquireResourceSharedLite(&DeviceExt->BitmapResource, TRUE);
    DataBuffer->VolumeSerialNumber.QuadPart = DeviceExt->NtfsInfo.SerialNumber;
    DataBuffer->NumberSectors.QuadPart = DeviceExt->NtfsInfo.SectorCount;
    DataBuffer->TotalClusters.QuadPart = DeviceExt->NtfsInfo.ClusterCount;
    DataBuffer->FreeClusters.QuadPart = NtfsGetFreeClusters(DeviceExt);
    DataBuffer->TotalReserved.QuadPart = 0LL; // FIXME
    ExReleaseResourceLite(&DeviceExt->BitmapResource);
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
        /* METHOD_BUFFERED: the whole reply goes in the system buffer, which the I/O
         * manager copies over the caller's buffer at completion */
        PNTFS_EXTENDED_VOLUME_DATA ExtendedData = (PNTFS_EXTENDED_VOLUME_DATA)(DataBuffer + 1);

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


static
NTSTATUS
GetVolumeBitmap(PDEVICE_EXTENSION DeviceExt,
                PIRP Irp)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION Stack;
    PVOLUME_BITMAP_BUFFER BitmapBuffer;
    LONGLONG StartingLcn;
    ULONGLONG TotalClusters;
    ULONGLONG ToCopy;
    BOOLEAN Overflow = FALSE;

    DPRINT("GetVolumeBitmap(%p, %p)\n", DeviceExt, Irp);

    Stack = IoGetCurrentIrpStackLocation(Irp);

    /* The bitmap is a property of the volume, asked for on a volume handle */
    if (!(((PNTFS_FCB)Stack->FileObject->FsContext)->Flags & FCB_IS_VOLUME))
        return STATUS_INVALID_PARAMETER;

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
    if (StartingLcn < 0 || StartingLcn >= (LONGLONG)DeviceExt->NtfsInfo.ClusterCount)
    {
        DPRINT1("Requested bitmap start beyond partition end: %I64x %I64x\n", DeviceExt->NtfsInfo.ClusterCount, StartingLcn);
        return STATUS_INVALID_PARAMETER;
    }

    /* Round down to a multiple of 8. BitmapSize is every cluster from there on, even when
     * the buffer holds the bits of fewer (MS-FSCC 2.3.32). */
    StartingLcn = StartingLcn & ~7;
    TotalClusters = DeviceExt->NtfsInfo.ClusterCount - StartingLcn;
    ToCopy = (TotalClusters + 7) / 8;
    if ((ToCopy + FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)) > Stack->Parameters.FileSystemControl.OutputBufferLength)
    {
        DPRINT1("Buffer too small: %x, needed: %x\n", Stack->Parameters.FileSystemControl.OutputBufferLength, (ToCopy + FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)));
        Overflow = TRUE;
        ToCopy = Stack->Parameters.FileSystemControl.OutputBufferLength - FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    }

    BitmapBuffer->StartingLcn.QuadPart = StartingLcn;
    BitmapBuffer->BitmapSize.QuadPart = TotalClusters;

    Irp->IoStatus.Information = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&DeviceExt->BitmapResource, TRUE);
    _SEH2_TRY
    {
        RtlCopyMemory(BitmapBuffer->Buffer, (PUCHAR)DeviceExt->BitmapBuffer + StartingLcn / 8, (SIZE_T)ToCopy);
        Irp->IoStatus.Information += (ULONG_PTR)ToCopy;
        Status = (Overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ExReleaseResourceLite(&DeviceExt->BitmapResource);
    KeLeaveCriticalRegion();

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

    /* Only allow locking with the volume open */
    if (!(Fcb->Flags & FCB_IS_VOLUME))
    {
        return STATUS_ACCESS_DENIED;
    }

    /* Locking a locked volume is refused; unlocking one that isn't locked is
     * STATUS_NOT_LOCKED */
    if ((DeviceExt->Flags & VCB_VOLUME_LOCKED) && Lock)
    {
        return STATUS_ACCESS_DENIED;
    }

    if (!(DeviceExt->Flags & VCB_VOLUME_LOCKED) && !Lock)
    {
        return STATUS_NOT_LOCKED;
    }

    /* Deny locking if we're not alone */
    if (Lock && DeviceExt->OpenHandleCount != 1)
    {
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


/**
* @name NtfsDismountVolume
* @implemented
*
* Takes the volume offline so the caller can reformat or eject the media.
*
* @param DeviceExt
* Volume control block of the volume to dismount
*
* @param Irp
* The FSCTL_DISMOUNT_VOLUME request
*
* @return
* STATUS_SUCCESS once the volume is offline, STATUS_INVALID_PARAMETER if the
* request did not come through a handle on the volume itself, or
* STATUS_VOLUME_DISMOUNTED if it already was.
*
* @remarks The VCB is not torn down here: the caller keeps its handle open and
* goes on writing over the volume through it, so the device object has to stay
* alive. What changes is that the volume is no longer mounted,
* VPB_DIRECT_WRITES_ALLOWED lets raw writes through, and anything needing
* on-disk metadata is refused.
*
*/
static
NTSTATUS
NtfsDismountVolume(PDEVICE_EXTENSION DeviceExt,
                   PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    KIRQL SavedIrql;
    NTSTATUS Status;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    FileObject = Stack->FileObject;
    Fcb = FileObject->FsContext;

    /* Only a handle on the volume itself may dismount it */
    if (!(Fcb->Flags & FCB_IS_VOLUME))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (DeviceExt->Flags & VCB_VOLUME_DISMOUNTED)
    {
        return STATUS_VOLUME_DISMOUNTED;
    }

    DPRINT1("Dismounting volume\n");

    FsRtlNotifyVolumeEvent(FileObject, FSRTL_VOLUME_DISMOUNT);

    ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, TRUE);

    Status = NtfsFlushVolume(DeviceExt);
    if (!NT_SUCCESS(Status))
    {
        ExReleaseResourceLite(&DeviceExt->DirResource);
        return Status;
    }

    Status = NtfsLfsFinishWriter(DeviceExt->LfsWriter);
    if (!NT_SUCCESS(Status))
    {
        ExReleaseResourceLite(&DeviceExt->DirResource);
        return Status;
    }

    /* Dirty pages must reach disk before the cache is discarded. */
    CcPurgeCacheSection(DeviceExt->StreamFileObject->SectionObjectPointer, NULL, 0, FALSE);
    CcUninitializeCacheMap(DeviceExt->StreamFileObject, NULL, NULL);

    DeviceExt->Flags |= VCB_VOLUME_DISMOUNTED;

    /* Drop the mounted state and allow raw writes, so the I/O manager stops
     * handing us opens of a partition that's being overwritten */
    IoAcquireVpbSpinLock(&SavedIrql);
    if (DeviceExt->StorageDevice->Vpb != NULL)
    {
        DeviceExt->StorageDevice->Vpb->Flags |= VPB_DIRECT_WRITES_ALLOWED;
        DeviceExt->StorageDevice->Vpb->Flags &= ~VPB_MOUNTED;
    }
    IoReleaseVpbSpinLock(SavedIrql);

    ExReleaseResourceLite(&DeviceExt->DirResource);

    return STATUS_SUCCESS;
}

/**
* @name NtfsReparsePointRequest
*
* FSCTL_SET_REPARSE_POINT, FSCTL_GET_REPARSE_POINT and FSCTL_DELETE_REPARSE_POINT, all
* METHOD_BUFFERED, on a file or directory (not the volume, not a named stream).
*/
static
NTSTATUS
NtfsReparsePointRequest(PDEVICE_EXTENSION DeviceExt,
                        PIRP Irp,
                        ULONG Code)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG Copied;
    ULONG OldAttributes;
    BOOLEAN AttributesChanged;
    NTSTATUS Status;

    /* No buffer to return the data in reaches no further */
    if (Code == FSCTL_GET_REPARSE_POINT &&
        (Stack->Parameters.FileSystemControl.OutputBufferLength == 0 || Buffer == NULL))
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM) || Fcb->Stream[0] != UNICODE_NULL)
        return STATUS_INVALID_PARAMETER;

    if (Code != FSCTL_GET_REPARSE_POINT && !NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
    OldAttributes = Fcb->Entry.FileAttributes;

    switch (Code)
    {
        case FSCTL_SET_REPARSE_POINT:
            Status = NtfsWriteReparsePoint(DeviceExt, Fcb, Buffer,
                                           Stack->Parameters.FileSystemControl.InputBufferLength);
            break;

        case FSCTL_GET_REPARSE_POINT:
            Status = NtfsQueryReparsePoint(DeviceExt, Fcb, Buffer,
                                           Stack->Parameters.FileSystemControl.OutputBufferLength,
                                           &Copied);
            if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
                Irp->IoStatus.Information = Copied;
            break;

        default:
            Status = NtfsRemoveReparsePoint(DeviceExt, Fcb, Buffer,
                                            Stack->Parameters.FileSystemControl.InputBufferLength);
            break;
    }

    if (NT_SUCCESS(Status) && Code != FSCTL_GET_REPARSE_POINT)
        NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_REPARSE_POINT_CHANGE);

    AttributesChanged = NT_SUCCESS(Status) && Code != FSCTL_GET_REPARSE_POINT &&
                        OldAttributes != Fcb->Entry.FileAttributes;
    ExReleaseResourceLite(&Fcb->MainResource);
    if (AttributesChanged)
        NtfsReportChange(Fcb, FILE_NOTIFY_CHANGE_ATTRIBUTES, FILE_ACTION_MODIFIED);

    return Status;
}

/**
* @name NtfsObjectIdRequest
*
* FSCTL_GET_OBJECT_ID, FSCTL_SET_OBJECT_ID, FSCTL_SET_OBJECT_ID_EXTENDED,
* FSCTL_CREATE_OR_GET_OBJECT_ID and FSCTL_DELETE_OBJECT_ID (MS-FSA 2.1.5.10), all
* METHOD_BUFFERED, on a file or directory.
*/
static
NTSTATUS
NtfsObjectIdRequest(PDEVICE_EXTENSION DeviceExt,
                    PIRP Irp,
                    ULONG Code)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG InputLength = Stack->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutputLength = Stack->Parameters.FileSystemControl.OutputBufferLength;
    PFILE_RECORD_HEADER FileRecord;
    FILE_OBJECTID_BUFFER ObjectId;
    NTSTATUS Status;

    if (DeviceExt->ObjectIdIndexMftIndex == 0)
        return STATUS_VOLUME_NOT_UPGRADED;

    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
        return STATUS_INVALID_PARAMETER;

    switch (Code)
    {
        case FSCTL_GET_OBJECT_ID:
        case FSCTL_CREATE_OR_GET_OBJECT_ID:
            if (Buffer == NULL || OutputLength < sizeof(FILE_OBJECTID_BUFFER))
                return STATUS_INVALID_PARAMETER;
            break;

        case FSCTL_SET_OBJECT_ID:
            if (Buffer == NULL || InputLength != sizeof(FILE_OBJECTID_BUFFER))
                return STATUS_INVALID_PARAMETER;
            break;

        case FSCTL_SET_OBJECT_ID_EXTENDED:
            if (Buffer == NULL || InputLength != sizeof(ObjectId.ExtendedInfo))
                return STATUS_INVALID_PARAMETER;
            break;
    }

    if (Code != FSCTL_GET_OBJECT_ID && !NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        switch (Code)
        {
            case FSCTL_GET_OBJECT_ID:
                Status = NtfsQueryObjectId(DeviceExt, FileRecord, &ObjectId);
                break;

            case FSCTL_CREATE_OR_GET_OBJECT_ID:
                Status = NtfsCreateOrGetObjectId(DeviceExt, Fcb, FileRecord, &ObjectId);
                break;

            case FSCTL_SET_OBJECT_ID:
                Status = NtfsSetObjectId(DeviceExt, Fcb, FileRecord, Buffer);
                break;

            case FSCTL_SET_OBJECT_ID_EXTENDED:
                Status = NtfsSetObjectIdExtended(DeviceExt, Fcb, FileRecord, Buffer);
                break;

            default:
                Status = NtfsDeleteObjectId(DeviceExt, FileRecord);
                break;
        }
    }

    /* CREATE_OR_GET too, which may have made one */
    if (NT_SUCCESS(Status) && Code != FSCTL_GET_OBJECT_ID)
        NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_OBJECT_ID_CHANGE);

    ExReleaseResourceLite(&Fcb->MainResource);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (NT_SUCCESS(Status) && (Code == FSCTL_GET_OBJECT_ID || Code == FSCTL_CREATE_OR_GET_OBJECT_ID))
    {
        RtlCopyMemory(Buffer, &ObjectId, sizeof(ObjectId));
        Irp->IoStatus.Information = sizeof(ObjectId);
    }

    return Status;
}

/**
* @name NtfsCaptureFsctlInput
*
* Copies the input of an FSCTL, METHOD_BUFFERED or METHOD_NEITHER, to Input. Exactly
* Length bytes are wanted; a shorter input is STATUS_INVALID_PARAMETER.
*/
static
NTSTATUS
NtfsCaptureFsctlInput(PIRP Irp,
                      PVOID Input,
                      ULONG Length)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG Code = Stack->Parameters.FileSystemControl.FsControlCode;
    PVOID Source;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Stack->Parameters.FileSystemControl.InputBufferLength < Length)
        return STATUS_INVALID_PARAMETER;

    if (METHOD_FROM_CTL_CODE(Code) != METHOD_NEITHER)
    {
        RtlCopyMemory(Input, Irp->AssociatedIrp.SystemBuffer, Length);
        return STATUS_SUCCESS;
    }

    Source = Stack->Parameters.FileSystemControl.Type3InputBuffer;
    _SEH2_TRY
    {
        if (Irp->RequestorMode != KernelMode)
            ProbeForRead(Source, Length, sizeof(UCHAR));
        RtlCopyMemory(Input, Source, Length);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

/**
* @name NtfsReturnFsctlOutput
*
* Copies Length bytes of an FSCTL's output to the caller's buffer.
*/
static
NTSTATUS
NtfsReturnFsctlOutput(PIRP Irp,
                      PVOID Output,
                      ULONG Length)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG Code = Stack->Parameters.FileSystemControl.FsControlCode;
    PVOID Destination;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(Length <= Stack->Parameters.FileSystemControl.OutputBufferLength);

    if (METHOD_FROM_CTL_CODE(Code) == METHOD_BUFFERED)
    {
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Output, Length);
        Irp->IoStatus.Information = Length;
        return STATUS_SUCCESS;
    }

    Destination = Irp->UserBuffer;
    _SEH2_TRY
    {
        if (Irp->RequestorMode != KernelMode)
            ProbeForWrite(Destination, Length, sizeof(UCHAR));
        RtlCopyMemory(Destination, Output, Length);
        Irp->IoStatus.Information = Length;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

/**
* @name NtfsFillRetrievalPointers
*
* Fills Output with the extents of a non-resident attribute from the run holding StartingVcn
* on: as many as fit, holes with an LCN of -1. STATUS_BUFFER_OVERFLOW if some didn't fit.
*/
static
NTSTATUS
NtfsFillRetrievalPointers(PNTFS_ATTR_CONTEXT Context,
                          LONGLONG TotalVcns,
                          LONGLONG StartingVcn,
                          PRETRIEVAL_POINTERS_BUFFER Output,
                          ULONG OutputLength,
                          PULONG Returned)
{
    ULONG Room = (OutputLength - FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents)) / sizeof(Output->Extents[0]);
    LONGLONG Vcn = 0, Lcn, Count;
    BOOLEAN Started = FALSE;
    ULONG Index;

    Output->ExtentCount = 0;

    for (Index = 0; Vcn < TotalVcns; Index++)
    {
        LONGLONG RunVcn, RunLcn;

        if (!FsRtlGetNextLargeMcbEntry(&Context->DataRunsMCB, Index, &RunVcn, &RunLcn, &Count))
        {
            /* Past the last mapping: the rest of the allocation is a hole */
            RunVcn = Vcn;
            RunLcn = -1;
            Count = TotalVcns - Vcn;
        }

        /* A gap before this run is a hole too */
        if (RunVcn > Vcn)
        {
            Lcn = -1;
            Count = RunVcn - Vcn;
            Index--;
        }
        else
        {
            ASSERT(RunVcn == Vcn);
            Lcn = RunLcn;
        }
        Count = min(Count, TotalVcns - Vcn);

        if (!Started && Vcn + Count > StartingVcn)
        {
            Started = TRUE;
            Output->StartingVcn.QuadPart = Vcn;
        }

        if (Started)
        {
            if (Output->ExtentCount == Room)
                break;

            Output->Extents[Output->ExtentCount].NextVcn.QuadPart = Vcn + Count;
            Output->Extents[Output->ExtentCount].Lcn.QuadPart = (Lcn < 0) ? -1 : Lcn;
            Output->ExtentCount++;
        }

        Vcn += Count;
    }

    *Returned = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents) + Output->ExtentCount * sizeof(Output->Extents[0]);

    return (Vcn < TotalVcns) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/**
* @name NtfsGetRetrievalPointers
*
* FSCTL_GET_RETRIEVAL_POINTERS: where the clusters of a stream, or of a directory's index,
* are on the volume. A stream without clusters is STATUS_END_OF_FILE.
*/
static
NTSTATUS
NtfsGetRetrievalPointers(PDEVICE_EXTENSION DeviceExt,
                         PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    ULONG OutputLength = Stack->Parameters.FileSystemControl.OutputBufferLength;
    STARTING_VCN_INPUT_BUFFER Input;
    PRETRIEVAL_POINTERS_BUFFER Output = NULL;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT Context = NULL;
    LONGLONG TotalVcns = 0;
    ULONG Returned = 0;
    NTSTATUS Status;

    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
        return STATUS_INVALID_PARAMETER;

    Status = NtfsCaptureFsctlInput(Irp, &Input, sizeof(Input));
    if (!NT_SUCCESS(Status))
        return Status;
    if (Input.StartingVcn.QuadPart < 0)
        return STATUS_INVALID_PARAMETER;
    if (OutputLength < sizeof(RETRIEVAL_POINTERS_BUFFER))
        return STATUS_BUFFER_TOO_SMALL;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    Output = ExAllocatePoolWithTag(PagedPool, OutputLength, TAG_NTFS);
    if (FileRecord == NULL || Output == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);

        Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
        if (NT_SUCCESS(Status))
        {
            if (NtfsFCBIsDirectory(Fcb))
                Status = FindAttribute(DeviceExt, FileRecord, AttributeIndexAllocation, L"$I30", 4, &Context, NULL);
            else
                Status = FindAttribute(DeviceExt, FileRecord, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);

            /* No index allocation, a resident or an empty stream: no clusters */
            if (NT_SUCCESS(Status) && Context->pRecord->IsNonResident)
                TotalVcns = Context->pRecord->NonResident.AllocatedSize / DeviceExt->NtfsInfo.BytesPerCluster;
            if (Status == STATUS_OBJECT_NAME_NOT_FOUND || Input.StartingVcn.QuadPart >= TotalVcns)
                Status = STATUS_END_OF_FILE;
        }

        if (NT_SUCCESS(Status))
            Status = NtfsFillRetrievalPointers(Context, TotalVcns, Input.StartingVcn.QuadPart, Output, OutputLength, &Returned);

        ExReleaseResourceLite(&Fcb->MainResource);
    }

    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
    {
        NTSTATUS CopyStatus = NtfsReturnFsctlOutput(Irp, Output, Returned);
        if (!NT_SUCCESS(CopyStatus))
            Status = CopyStatus;
    }

    if (Context != NULL)
        ReleaseAttributeContext(Context);
    if (Output != NULL)
        ExFreePoolWithTag(Output, TAG_NTFS);
    if (FileRecord != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsSparseRequest
*
* FSCTL_SET_SPARSE, FSCTL_SET_ZERO_DATA and FSCTL_QUERY_ALLOCATED_RANGES on a file.
*/
static
NTSTATUS
NtfsSparseRequest(PDEVICE_EXTENSION DeviceExt,
                  PIRP Irp,
                  ULONG Code)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    ULONG InputLength = Stack->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutputLength = Stack->Parameters.FileSystemControl.OutputBufferLength;
    FILE_ALLOCATED_RANGE_BUFFER Query;
    FILE_ZERO_DATA_INFORMATION Zero;
    PFILE_ALLOCATED_RANGE_BUFFER Ranges;
    BOOLEAN SetSparse = TRUE;
    ULONG Returned;
    ULONG OldAttributes;
    BOOLEAN DataChanged = FALSE, AttributesChanged;
    NTSTATUS Status;

    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
        return STATUS_INVALID_PARAMETER;

    if (Code == FSCTL_QUERY_ALLOCATED_RANGES)
    {
        Status = NtfsCaptureFsctlInput(Irp, &Query, sizeof(Query));
        if (!NT_SUCCESS(Status))
            return Status;
        if (OutputLength < sizeof(FILE_ALLOCATED_RANGE_BUFFER))
            return STATUS_BUFFER_TOO_SMALL;

        Ranges = ExAllocatePoolWithTag(PagedPool, OutputLength, TAG_NTFS);
        if (Ranges == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);
        Status = NtfsQueryAllocatedRanges(DeviceExt, Fcb, &Query, Ranges, OutputLength, &Returned);
        ExReleaseResourceLite(&Fcb->MainResource);

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
        {
            NTSTATUS CopyStatus = NtfsReturnFsctlOutput(Irp, Ranges, Returned);
            if (!NT_SUCCESS(CopyStatus))
                Status = CopyStatus;
        }

        ExFreePoolWithTag(Ranges, TAG_NTFS);
        return Status;
    }

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    if (Code == FSCTL_SET_ZERO_DATA)
    {
        Status = NtfsCaptureFsctlInput(Irp, &Zero, sizeof(Zero));
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else if (InputLength != 0)
    {
        /* FILE_SET_SPARSE_BUFFER, optional */
        Status = NtfsCaptureFsctlInput(Irp, &SetSparse, sizeof(SetSparse));
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Paging I/O maps the runs too; keep it out while they change */
    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
    OldAttributes = Fcb->Entry.FileAttributes;

    /* These workers change data and mappings through the volume cache. */
    Status = NtfsFlushFileCache(Stack->FileObject, TRUE);
    if (NT_SUCCESS(Status))
    {
        if (Code == FSCTL_SET_ZERO_DATA)
        {
            Status = NtfsSetZeroData(DeviceExt, Fcb, &Zero);
            DataChanged = NT_SUCCESS(Status) && Zero.FileOffset.QuadPart < Zero.BeyondFinalZero.QuadPart &&
                          Zero.FileOffset.QuadPart < Fcb->RFCB.FileSize.QuadPart;
        }
        else
            Status = NtfsSetSparse(DeviceExt, Fcb, SetSparse);
    }

    ExReleaseResourceLite(&Fcb->PagingIoResource);

    if (NT_SUCCESS(Status))
    {
        NtfsPostUsnChange(DeviceExt, Fcb, (Code == FSCTL_SET_ZERO_DATA) ?
                          NtfsUsnDataReason(Fcb, USN_REASON_DATA_OVERWRITE) : USN_REASON_BASIC_INFO_CHANGE);
    }

    AttributesChanged = NT_SUCCESS(Status) && OldAttributes != Fcb->Entry.FileAttributes;
    ExReleaseResourceLite(&Fcb->MainResource);
    if (AttributesChanged)
        NtfsReportChange(Fcb, FILE_NOTIFY_CHANGE_ATTRIBUTES, FILE_ACTION_MODIFIED);
    if (DataChanged)
        NtfsReportDataChange(Fcb, FILE_NOTIFY_CHANGE_LAST_WRITE);

    return Status;
}

/**
* @name NtfsCompressionRequest
*
* FSCTL_GET_COMPRESSION and FSCTL_SET_COMPRESSION, METHOD_BUFFERED, on a file or directory.
*/
static
NTSTATUS
NtfsCompressionRequest(PDEVICE_EXTENSION DeviceExt,
                       PIRP Irp,
                       ULONG Code)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    USHORT Format;
    ULONG OldAttributes;
    BOOLEAN AttributesChanged;
    NTSTATUS Status;

    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
        return STATUS_INVALID_PARAMETER;

    if (Code == FSCTL_GET_COMPRESSION)
    {
        if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(USHORT))
            return STATUS_INVALID_PARAMETER;

        ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);
        Status = NtfsGetCompression(DeviceExt, Fcb, &Format);
        ExReleaseResourceLite(&Fcb->MainResource);

        if (NT_SUCCESS(Status))
            Status = NtfsReturnFsctlOutput(Irp, &Format, sizeof(Format));
        return Status;
    }

    Status = NtfsCaptureFsctlInput(Irp, &Format, sizeof(Format));
    if (!NT_SUCCESS(Status))
        return Status;

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
    OldAttributes = Fcb->Entry.FileAttributes;
    Status = NtfsFlushFileCache(Stack->FileObject, TRUE);
    if (NT_SUCCESS(Status))
        Status = NtfsSetCompression(DeviceExt, Fcb, Format);
    ExReleaseResourceLite(&Fcb->PagingIoResource);

    if (NT_SUCCESS(Status))
        NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_COMPRESSION_CHANGE);

    AttributesChanged = NT_SUCCESS(Status) && OldAttributes != Fcb->Entry.FileAttributes;
    ExReleaseResourceLite(&Fcb->MainResource);
    if (AttributesChanged)
        NtfsReportChange(Fcb, FILE_NOTIFY_CHANGE_ATTRIBUTES, FILE_ACTION_MODIFIED);

    return Status;
}

/**
* @name NtfsMoveClusters
*
* Moves VCNs [Vcn, Vcn + Count) of a stream, all of them in one run of clusters, to the free
* clusters from Lcn on: copies the data, maps the VCNs there, and frees the old clusters.
*/
static
NTSTATUS
NtfsMoveClusters(PDEVICE_EXTENSION DeviceExt,
                 PNTFS_ATTR_CONTEXT Context,
                 ULONG AttrOffset,
                 PFILE_RECORD_HEADER Holder,
                 ULONGLONG Vcn,
                 ULONGLONG Lcn,
                 ULONG Count)
{
    ULONG BytesPerCluster = DeviceExt->NtfsInfo.BytesPerCluster;
    LONGLONG OldLcn, InRun;
    PUCHAR Data;
    NTSTATUS Status;

    if (!FsRtlLookupLargeMcbEntry(&Context->DataRunsMCB, Vcn, &OldLcn, &InRun, NULL, NULL, NULL) ||
        OldLcn == -1 || InRun < Count)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = NtfsAllocateClusterRange(DeviceExt, (ULONG)Lcn, Count);
    if (!NT_SUCCESS(Status))
        return Status;

    Data = ExAllocatePoolWithTag(NonPagedPool, Count * BytesPerCluster, TAG_NTFS);
    if (Data == NULL)
        Status = STATUS_INSUFFICIENT_RESOURCES;
    else
        Status = NtfsReadDiskCached(DeviceExt, OldLcn * BytesPerCluster, Count * BytesPerCluster, Data);
    if (NT_SUCCESS(Status))
        Status = NtfsWriteDiskCached(DeviceExt, Lcn * BytesPerCluster, Count * BytesPerCluster, Data);

    if (NT_SUCCESS(Status))
    {
        FsRtlRemoveLargeMcbEntry(&Context->DataRunsMCB, Vcn, Count);
        if (!FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, Vcn, Lcn, Count))
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(Status))
        Status = NtfsStoreMappingPairs(DeviceExt, Context, AttrOffset, Holder,
                                       Context->pRecord->NonResident.HighestVCN + 1);
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(DeviceExt, Context->FileMFTIndex & NTFS_MFT_MASK, Holder);

    /* Only once the file maps the new clusters do the old ones go */
    if (NT_SUCCESS(Status))
        Status = NtfsFreeClusterRange(DeviceExt, (ULONG)OldLcn, Count);
    else
        NtfsFreeClusterRange(DeviceExt, (ULONG)Lcn, Count);

    if (Data != NULL)
        ExFreePoolWithTag(Data, TAG_NTFS);

    return Status;
}

/**
* @name NtfsMoveFile
*
* Moves ClusterCount clusters of the file Fcb is open on, from StartingVcn, to the free
* clusters from StartingLcn on, run by run.
*/
static
NTSTATUS
NtfsMoveFile(PDEVICE_EXTENSION DeviceExt,
             PNTFS_FCB Fcb,
             PMOVE_FILE_DATA Move)
{
    PFILE_RECORD_HEADER FileRecord, Holder = NULL;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONGLONG Vcn = Move->StartingVcn.QuadPart;
    ULONGLONG Lcn = Move->StartingLcn.QuadPart;
    ULONG Left = Move->ClusterCount;
    ULONG AttrOffset;
    NTSTATUS Status;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        if (NtfsFCBIsDirectory(Fcb))
            Status = FindAttribute(DeviceExt, FileRecord, AttributeIndexAllocation, L"$I30", 4, &Context, &AttrOffset);
        else
            Status = FindAttribute(DeviceExt, FileRecord, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, &AttrOffset);
    }
    if (NT_SUCCESS(Status))
        Status = NtfsReadAttributeHolder(DeviceExt, Context, FileRecord, &Holder);

    /* Only clusters a stream owns move; compressed units have their own layout */
    if (NT_SUCCESS(Status) &&
        (!Context->pRecord->IsNonResident || NtfsAttributeIsCompressed(Context) ||
         Vcn + Left > (ULONGLONG)Context->pRecord->NonResident.HighestVCN + 1))
    {
        Status = STATUS_INVALID_PARAMETER;
    }

    while (NT_SUCCESS(Status) && Left != 0)
    {
        LONGLONG OldLcn, InRun;
        ULONG Piece;

        if (!FsRtlLookupLargeMcbEntry(&Context->DataRunsMCB, Vcn, &OldLcn, &InRun, NULL, NULL, NULL) || OldLcn == -1)
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        Piece = (ULONG)min((LONGLONG)Left, InRun);
        Status = NtfsMoveClusters(DeviceExt, Context, AttrOffset, Holder, Vcn, Lcn, Piece);

        Vcn += Piece;
        Lcn += Piece;
        Left -= Piece;
    }

    if (Holder != NULL)
        NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
    if (Context != NULL)
        ReleaseAttributeContext(Context);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsMoveFileRequest
*
* FSCTL_MOVE_FILE on a volume handle: moves clusters of the file MOVE_FILE_DATA names, which
* must be on this volume, to free clusters of the caller's choosing.
*/
static
NTSTATUS
NtfsMoveFileRequest(PDEVICE_EXTENSION DeviceExt,
                    PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB VolumeFcb = Stack->FileObject->FsContext;
    MOVE_FILE_DATA Move;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    NTSTATUS Status;

    if (!(VolumeFcb->Flags & FCB_IS_VOLUME))
        return STATUS_INVALID_PARAMETER;
    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    Status = NtfsCaptureFsctlInput(Irp, &Move, sizeof(Move));
    if (!NT_SUCCESS(Status))
        return Status;
    if (Move.StartingVcn.QuadPart < 0 || Move.StartingLcn.QuadPart < 0 || Move.ClusterCount == 0)
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByHandle(Move.FileHandle, 0, *IoFileObjectType, Irp->RequestorMode,
                                       (PVOID *)&FileObject, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Fcb = FileObject->FsContext;
    if (IoGetRelatedDeviceObject(FileObject) != IoGetRelatedDeviceObject(Stack->FileObject) ||
        Fcb == NULL || (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM)))
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else
    {
        /* Nothing may map the runs while they change */
        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        Status = NtfsFlushFileCache(FileObject, FALSE);
        if (NT_SUCCESS(Status))
            Status = NtfsMoveFile(DeviceExt, Fcb, &Move);
        ExReleaseResourceLite(&Fcb->PagingIoResource);
        ExReleaseResourceLite(&Fcb->MainResource);
    }

    ObDereferenceObject(FileObject);

    return Status;
}

/**
* @name NtfsUsnRequest
*
* The change journal FSCTLs: CREATE, DELETE, QUERY and READ_USN_JOURNAL and ENUM_USN_DATA on
* the volume, READ_FILE_USN_DATA on a file.
*/
static
NTSTATUS
NtfsUsnRequest(PDEVICE_EXTENSION DeviceExt,
               PIRP Irp,
               ULONG Code)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    ULONG OutputLength = Stack->Parameters.FileSystemControl.OutputBufferLength;
    BOOLEAN OnVolume = BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME);
    union
    {
        CREATE_USN_JOURNAL_DATA Create;
        DELETE_USN_JOURNAL_DATA Delete;
        READ_USN_JOURNAL_DATA Read;
        MFT_ENUM_DATA Enum;
        USN_JOURNAL_DATA Journal;
    } Input;
    PUCHAR Output = NULL;
    ULONG Returned = 0;
    NTSTATUS Status;

    if ((Code == FSCTL_READ_FILE_USN_DATA) == OnVolume)
        return STATUS_INVALID_PARAMETER;

    switch (Code)
    {
        case FSCTL_CREATE_USN_JOURNAL:
        case FSCTL_DELETE_USN_JOURNAL:
            if (!NtfsGlobalData->EnableWriteSupport)
                return STATUS_MEDIA_WRITE_PROTECTED;

            if (Code == FSCTL_CREATE_USN_JOURNAL)
                Status = NtfsCaptureFsctlInput(Irp, &Input.Create, sizeof(Input.Create));
            else
                Status = NtfsCaptureFsctlInput(Irp, &Input.Delete, sizeof(Input.Delete));
            if (!NT_SUCCESS(Status))
                return Status;

            /* Making or deleting $UsnJrnl changes $Extend like any create or delete */
            ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, TRUE);
            if (Code == FSCTL_CREATE_USN_JOURNAL)
                Status = NtfsCreateUsnJournal(DeviceExt, &Input.Create);
            else
                Status = NtfsDeleteUsnJournal(DeviceExt, &Input.Delete);
            ExReleaseResourceLite(&DeviceExt->DirResource);
            return Status;

        case FSCTL_QUERY_USN_JOURNAL:
            if (OutputLength < sizeof(USN_JOURNAL_DATA))
                return STATUS_BUFFER_TOO_SMALL;
            Status = NtfsQueryUsnJournal(DeviceExt, &Input.Journal);
            if (NT_SUCCESS(Status))
                Status = NtfsReturnFsctlOutput(Irp, &Input.Journal, sizeof(Input.Journal));
            return Status;

        case FSCTL_READ_USN_JOURNAL:
            Status = NtfsCaptureFsctlInput(Irp, &Input.Read, sizeof(Input.Read));
            break;

        case FSCTL_ENUM_USN_DATA:
            Status = NtfsCaptureFsctlInput(Irp, &Input.Enum, sizeof(Input.Enum));
            break;

        default:
            Status = STATUS_SUCCESS;
            break;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    /* The records are put together here and copied out at once */
    if (OutputLength != 0)
    {
        Output = ExAllocatePoolWithTag(PagedPool, OutputLength, TAG_NTFS);
        if (Output == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    switch (Code)
    {
        case FSCTL_READ_USN_JOURNAL:
            Status = NtfsReadUsnJournal(DeviceExt, &Input.Read, Output, OutputLength, &Returned);
            break;

        case FSCTL_ENUM_USN_DATA:
            Status = NtfsEnumUsnData(DeviceExt, &Input.Enum, Output, OutputLength, &Returned);
            break;

        default:
            ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);
            Status = NtfsReadFileUsnData(DeviceExt, Fcb, Output, OutputLength, &Returned);
            ExReleaseResourceLite(&Fcb->MainResource);
            break;
    }

    if (NT_SUCCESS(Status))
        Status = NtfsReturnFsctlOutput(Irp, Output, Returned);

    if (Output != NULL)
        ExFreePoolWithTag(Output, TAG_NTFS);

    return Status;
}

/* The flags of $VOLUME_INFORMATION */
#define NTFS_VOLUME_FLAG_DIRTY              0x0001
#define NTFS_VOLUME_FLAG_UPGRADE_ON_MOUNT   0x0004

/**
* @name NtfsSetVolumeFlags
*
* Sets flags in the volume's $VOLUME_INFORMATION, on disk and in the VCB.
*/
static
NTSTATUS
NtfsSetVolumeFlags(PDEVICE_EXTENSION DeviceExt,
                   USHORT Flags,
                   BOOLEAN Set)
{
    PFILE_RECORD_HEADER VolumeRecord;
    PNTFS_ATTR_CONTEXT AttrContext;
    PNTFS_ATTR_RECORD Attribute;
    PVOLINFO_ATTRIBUTE VolumeInfo;
    ULONG AttrOffset;
    NTSTATUS Status;

    VolumeRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (VolumeRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_VOLUME, VolumeRecord);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(DeviceExt, VolumeRecord, AttributeVolumeInformation, L"", 0, &AttrContext, &AttrOffset);

    if (NT_SUCCESS(Status))
    {
        /* $VOLUME_INFORMATION is always resident */
        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)VolumeRecord + AttrOffset);
        ASSERT(!Attribute->IsNonResident);
        VolumeInfo = (PVOLINFO_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
        if (Set) VolumeInfo->Flags |= Flags;
        else VolumeInfo->Flags &= ~Flags;

        Status = UpdateFileRecord(DeviceExt, NTFS_FILE_VOLUME, VolumeRecord);
        if (NT_SUCCESS(Status))
            DeviceExt->NtfsInfo.Flags = VolumeInfo->Flags;

        ReleaseAttributeContext(AttrContext);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, VolumeRecord);

    return Status;
}

/* A failed rollback leaves on-disk metadata inconsistent. Keep that fact
 * visible to chkdsk even if the attempted dirty-bit write also fails. */
VOID
NtfsMarkVolumeCorrupt(PDEVICE_EXTENSION Vcb)
{
    NTSTATUS Status;

    Vcb->Flags |= VCB_VOLUME_CORRUPT;
    if (Vcb->Flags & VCB_VOLUME_READ_ONLY)
        return;
    Status = NtfsSetVolumeFlags(Vcb, NTFS_VOLUME_FLAG_DIRTY, TRUE);
    SetFlag(Vcb->NtfsInfo.Flags, NTFS_VOLUME_FLAG_DIRTY);
    if (!NT_SUCCESS(Status))
        DPRINT1("Couldn't persist the volume dirty flag: %lx\n", Status);
}

/**
* @name NtfsIsVolumeDirty
*
* FSCTL_IS_VOLUME_DIRTY: whether chkdsk is due (VOLUME_IS_DIRTY) or the volume is to be
* upgraded (VOLUME_UPGRADE_SCHEDULED). Only a volume handle can ask.
*/
static
NTSTATUS
NtfsIsVolumeDirty(PDEVICE_EXTENSION DeviceExt,
                  PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    PULONG State = Irp->AssociatedIrp.SystemBuffer;

    if (!(Fcb->Flags & FCB_IS_VOLUME))
        return STATUS_INVALID_PARAMETER;

    if (Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;

    *State = 0;
    if (DeviceExt->NtfsInfo.Flags & NTFS_VOLUME_FLAG_DIRTY)
        *State |= VOLUME_IS_DIRTY;
    if (DeviceExt->NtfsInfo.Flags & NTFS_VOLUME_FLAG_UPGRADE_ON_MOUNT)
        *State |= VOLUME_UPGRADE_SCHEDULED;

    Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}

/**
* @name NtfsMarkVolumeDirty
*
* FSCTL_MARK_VOLUME_DIRTY: makes chkdsk run on the next mount. The volume stays writable
* meanwhile. Only a volume handle can ask.
*/
static
NTSTATUS
NtfsMarkVolumeDirty(PDEVICE_EXTENSION DeviceExt,
                    PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;

    if (!(Fcb->Flags & FCB_IS_VOLUME))
        return STATUS_INVALID_PARAMETER;

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    DeviceExt->Flags |= VCB_VOLUME_CORRUPT;
    return NtfsSetVolumeFlags(DeviceExt, NTFS_VOLUME_FLAG_DIRTY, TRUE);
}

/* FSCTL_EXTEND_VOLUME supplies the new partition-relative sector count.
 * NTFS keeps the last sector (the backup boot sector) outside that count in
 * its allocation geometry: the BPB stores NumberOfSectors - 1. */
static NTSTATUS
NtfsExtendVolume(PDEVICE_EXTENSION Vcb, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNTFS_FCB Fcb = Stack->FileObject->FsContext, StreamFcb;
    GET_LENGTH_INFORMATION Length;
    ULONG LengthSize = sizeof(Length);
    ULONG SectorSize = Vcb->NtfsInfo.BytesPerSector;
    ULONGLONG NumberOfSectors, NewClusters;
    PBOOT_SECTOR BootSector = NULL;
    BOOLEAN PrimaryTouched = FALSE, MetadataTouched = FALSE;
    NTSTATUS Status;

    if (!(Fcb->Flags & FCB_IS_VOLUME))
        return STATUS_INVALID_PARAMETER;
    Status = NtfsCaptureFsctlInput(Irp, &NumberOfSectors, sizeof(NumberOfSectors));
    if (!NT_SUCCESS(Status))
        return Status;
    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (!SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_MANAGE_VOLUME_PRIVILEGE), Irp->RequestorMode))
        return STATUS_PRIVILEGE_NOT_HELD;

    ExAcquireResourceExclusiveLite(&Vcb->DirResource, TRUE);
    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
    Status = NtfsDeviceIoControl(Vcb->StorageDevice, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                                  &Length, &LengthSize, FALSE);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    ASSERT(SectorSize != 0 && Vcb->NtfsInfo.SectorsPerCluster != 0);
    if (NumberOfSectors <= Vcb->NtfsInfo.SectorCount + 1 ||
        NumberOfSectors - (Vcb->NtfsInfo.SectorCount + 1) < Vcb->NtfsInfo.SectorsPerCluster ||
        NumberOfSectors > (ULONGLONG)Length.Length.QuadPart / SectorSize)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }
    NewClusters = (NumberOfSectors - 1) / Vcb->NtfsInfo.SectorsPerCluster;
    if (NewClusters <= Vcb->NtfsInfo.ClusterCount || NewClusters > MAXULONG)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }
    BootSector = ExAllocatePoolWithTag(NonPagedPool, SectorSize, TAG_NTFS);
    if (BootSector == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    Status = NtfsReadDiskCached(Vcb, 0, SectorSize, (PUCHAR)BootSector);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    BootSector->EBPB.SectorCount = NumberOfSectors - 1;

    /* The internal stream represents raw partition I/O. Expand its cache
     * view before writing the new backup sector; allocation remains bounded
     * by the old ClusterBitmap until the final publication below. */
    StreamFcb = Vcb->StreamFileObject->FsContext;
    ExAcquireResourceExclusiveLite(&StreamFcb->MainResource, TRUE);
    StreamFcb->RFCB.FileSize = Length.Length;
    StreamFcb->RFCB.AllocationSize = Length.Length;
    StreamFcb->RFCB.ValidDataLength = Length.Length;
    CcSetFileSizes(Vcb->StreamFileObject, (PCC_FILE_SIZES)&StreamFcb->RFCB.AllocationSize);
    ExReleaseResourceLite(&StreamFcb->MainResource);

    ExAcquireResourceExclusiveLite(&Vcb->MftAllocationResource, TRUE);
    ExAcquireResourceExclusiveLite(&Vcb->BitmapResource, TRUE);
    Status = NtfsWriteDiskCached(Vcb, (NumberOfSectors - 1) * SectorSize,
                                  SectorSize, (PUCHAR)BootSector);
    if (NT_SUCCESS(Status))
        Status = NtfsFlushVolumeStream(Vcb);
    if (NT_SUCCESS(Status))
        Status = NtfsExtendVolumeBitmap(Vcb, (ULONG)NewClusters, &MetadataTouched);
    if (NT_SUCCESS(Status))
        Status = NtfsFlushVolumeStream(Vcb);
    if (NT_SUCCESS(Status))
    {
        PrimaryTouched = TRUE;
        Status = NtfsWriteDiskCached(Vcb, 0, SectorSize, (PUCHAR)BootSector);
    }
    if (NT_SUCCESS(Status))
        Status = NtfsFlushVolumeStream(Vcb);
    if (NT_SUCCESS(Status))
    {
        RtlInitializeBitMap(&Vcb->ClusterBitmap, Vcb->BitmapBuffer, (ULONG)NewClusters);
        Vcb->FreeClusterCount = RtlNumberOfClearBits(&Vcb->ClusterBitmap);
        Vcb->NtfsInfo.ClusterCount = NewClusters;
        Vcb->NtfsInfo.SectorCount = NumberOfSectors - 1;
        Fcb->RFCB.FileSize.QuadPart = (NumberOfSectors - 1) * SectorSize;
        Fcb->RFCB.AllocationSize = Fcb->RFCB.FileSize;
        Fcb->RFCB.ValidDataLength = Fcb->RFCB.FileSize;
    }
    if (!NT_SUCCESS(Status) && (PrimaryTouched || MetadataTouched))
        NtfsMarkVolumeCorrupt(Vcb);
    ExReleaseResourceLite(&Vcb->BitmapResource);
    ExReleaseResourceLite(&Vcb->MftAllocationResource);
Cleanup:
    if (BootSector != NULL)
        ExFreePoolWithTag(BootSector, TAG_NTFS);
    ExReleaseResourceLite(&Fcb->MainResource);
    ExReleaseResourceLite(&Vcb->DirResource);
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
        //case FSCTL_GET_RETRIEVAL_POINTER_BASE:
        //case FSCTL_LOOKUP_STREAM_FROM_CLUSTER:
        case FSCTL_MARK_HANDLE:
        //case FSCTL_SHRINK_VOLUME:
        case FSCTL_WRITE_USN_CLOSE_RECORD:
            UNIMPLEMENTED;
            DPRINT1("Unimplemented user request: %x\n", Stack->Parameters.FileSystemControl.FsControlCode);
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case FSCTL_EXTEND_VOLUME:
            Status = NtfsExtendVolume(DeviceExt, Irp);
            break;

        case FSCTL_LOCK_VOLUME:
            Status = LockOrUnlockVolume(DeviceExt, Irp, TRUE);
            break;

        case FSCTL_UNLOCK_VOLUME:
            Status = LockOrUnlockVolume(DeviceExt, Irp, FALSE);
            break;

        case FSCTL_DISMOUNT_VOLUME:
            Status = NtfsDismountVolume(DeviceExt, Irp);
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

        case FSCTL_SET_REPARSE_POINT:
        case FSCTL_GET_REPARSE_POINT:
        case FSCTL_DELETE_REPARSE_POINT:
            Status = NtfsReparsePointRequest(DeviceExt, Irp, Stack->Parameters.FileSystemControl.FsControlCode);
            break;

        case FSCTL_GET_OBJECT_ID:
        case FSCTL_SET_OBJECT_ID:
        case FSCTL_SET_OBJECT_ID_EXTENDED:
        case FSCTL_CREATE_OR_GET_OBJECT_ID:
        case FSCTL_DELETE_OBJECT_ID:
            Status = NtfsObjectIdRequest(DeviceExt, Irp, Stack->Parameters.FileSystemControl.FsControlCode);
            break;

        case FSCTL_GET_RETRIEVAL_POINTERS:
            Status = NtfsGetRetrievalPointers(DeviceExt, Irp);
            break;

        case FSCTL_MOVE_FILE:
            Status = NtfsMoveFileRequest(DeviceExt, Irp);
            break;

        case FSCTL_GET_COMPRESSION:
        case FSCTL_SET_COMPRESSION:
            Status = NtfsCompressionRequest(DeviceExt, Irp, Stack->Parameters.FileSystemControl.FsControlCode);
            break;

        case FSCTL_SET_SPARSE:
        case FSCTL_SET_ZERO_DATA:
        case FSCTL_QUERY_ALLOCATED_RANGES:
            Status = NtfsSparseRequest(DeviceExt, Irp, Stack->Parameters.FileSystemControl.FsControlCode);
            break;

        case FSCTL_CREATE_USN_JOURNAL:
        case FSCTL_DELETE_USN_JOURNAL:
        case FSCTL_QUERY_USN_JOURNAL:
        case FSCTL_READ_USN_JOURNAL:
        case FSCTL_ENUM_USN_DATA:
        case FSCTL_READ_FILE_USN_DATA:
            Status = NtfsUsnRequest(DeviceExt, Irp, Stack->Parameters.FileSystemControl.FsControlCode);
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
            ExAcquireResourceExclusiveLite(&NtfsGlobalData->Resource, TRUE);
            if (NtfsGlobalData->ShutdownStarted)
                Status = STATUS_SYSTEM_SHUTDOWN;
            else
                Status = NtfsMountVolume(DeviceObject, Irp);
            ExReleaseResourceLite(&NtfsGlobalData->Resource);
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
