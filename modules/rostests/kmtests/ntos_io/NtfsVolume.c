/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS volume control and dirty bit
 * PROGRAMMER:      ReactOS Team
 *
 * NtfsVolumeControl: FSCTL_LOCK_VOLUME / FSCTL_UNLOCK_VOLUME /
 * FSCTL_DISMOUNT_VOLUME and remount on the next open (host test
 * test_fsctl_volume_control).
 *
 * NtfsVolumeDirty: the volume mounted writable, FSCTL_IS_VOLUME_DIRTY and
 * FSCTL_MARK_VOLUME_DIRTY. This is the black-box part of the $LogFile work
 * (test_logfile_journaling / test_logfile_recovery / test_lfs_* stay host
 * only: log record layout and replay are not observable through the API).
 * WARNING: FSCTL_MARK_VOLUME_DIRTY persists; nothing but chkdsk clears it,
 * so this entry leaves the data volume dirty. It is the last NTFS entry.
 *
 * Windows reference: the FSCTL documentation, MS-FSA 2.1.5.10.x, FastFAT and
 * NTFS both reject FSCTL_IS_VOLUME_DIRTY on anything but a volume open.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"
#include <ntdddisk.h>

/* The same entry runs against MBR and GPT fixtures. Only the NTFS data
 * disk is writable, so finding and using it also exercises mount recognition. */
static VOID
TestPartitionInformation(HANDLE Volume)
{
    PARTITION_INFORMATION_EX Partition;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    RtlZeroMemory(&Partition, sizeof(Partition));
    Status = ZwDeviceIoControlFile(Volume, NULL, NULL, NULL, &IoStatus,
                                   IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0,
                                   &Partition, sizeof(Partition));
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    ok(Partition.PartitionStyle == PARTITION_STYLE_MBR ||
       Partition.PartitionStyle == PARTITION_STYLE_GPT,
       "unexpected partition style %u\n", Partition.PartitionStyle);
    ok(Partition.PartitionLength.QuadPart > 0, "partition has no length\n");
    if (Partition.PartitionStyle == PARTITION_STYLE_MBR)
        ok_eq_uint(Partition.Mbr.PartitionType, PARTITION_IFS);
    trace("NTFS mounted on %s partition, length %I64d\n",
          Partition.PartitionStyle == PARTITION_STYLE_GPT ? "GPT" : "MBR",
          Partition.PartitionLength.QuadPart);
}

/* NTFS metadata is hidden from pathname APIs. Inspect record 8 through the
 * documented FSCTL_GET_NTFS_FILE_RECORD interface and NTFS record layout. */
static VOID
CheckBadClusterRange(HANDLE Volume, ULONGLONG Clusters, ULONG BytesPerCluster, ULONG RecordBytes)
{
    NTFS_FILE_RECORD_INPUT_BUFFER Input;
    PNTFS_FILE_RECORD_OUTPUT_BUFFER Output;
    ULONG OutputSize = FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + RecordBytes;
    ULONG Offset, Type, Length;
    USHORT AttributeOffset, NameOffset;
    ULONGLONG Size, Highest;
    BOOLEAN Found = FALSE;
    NTSTATUS Status;
    PUCHAR Record, Attribute;

    Output = NtfsKmtAlloc(OutputSize);
    if (Output == NULL) return;
    Input.FileReferenceNumber.QuadPart = 8;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_FILE_RECORD, &Input, sizeof(Input), Output, OutputSize, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto CleanupBad;
    ok_eq_longlong(Output->FileReferenceNumber.QuadPart & 0x0000ffffffffffffULL, 8);
    ok(Output->FileRecordLength >= 24 && Output->FileRecordLength <= RecordBytes,
       "invalid returned record length %lu\n", Output->FileRecordLength);
    if (Output->FileRecordLength < 24 || Output->FileRecordLength > RecordBytes) goto CleanupBad;
    Record = Output->FileRecordBuffer;
    ok(RtlEqualMemory(Record, "FILE", 4), "record 8 lacks FILE magic\n");
    RtlCopyMemory(&AttributeOffset, Record + 20, sizeof(AttributeOffset));
    Offset = AttributeOffset;
    while (Offset <= Output->FileRecordLength - 8)
    {
        Attribute = Record + Offset;
        RtlCopyMemory(&Type, Attribute, sizeof(Type));
        if (Type == MAXULONG) break;
        RtlCopyMemory(&Length, Attribute + 4, sizeof(Length));
        ok(Length >= 16 && Length <= Output->FileRecordLength - Offset,
           "invalid attribute length %lu at %lu\n", Length, Offset);
        if (Length < 16 || Length > Output->FileRecordLength - Offset) break;
        RtlCopyMemory(&NameOffset, Attribute + 10, sizeof(NameOffset));
        if (Type == 0x80 && Attribute[9] == 4 && NameOffset <= Length &&
            Length - NameOffset >= 4 * sizeof(WCHAR) &&
            RtlEqualMemory(Attribute + NameOffset, L"$Bad", 4 * sizeof(WCHAR)))
        {
            Found = TRUE;
            ok(Attribute[8] != 0 && Length >= 64, "$Bad is not nonresident\n");
            if (Attribute[8] != 0 && Length >= 64)
            {
                RtlCopyMemory(&Highest, Attribute + 24, sizeof(Highest));
                RtlCopyMemory(&Size, Attribute + 48, sizeof(Size));
                ok_eq_ulonglong(Size, Clusters * BytesPerCluster);
                ok_eq_ulonglong(Highest + 1, Clusters);
            }
            break;
        }
        Offset += Length;
    }
    ok(Found, "record 8 has no $Bad data stream\n");
CleanupBad:
    NtfsKmtFree(Output);
}

static VOID
TestExtendVolume(HANDLE Volume, PCWSTR Dir, PULONGLONG ExpectedSectors, PBOOLEAN Created)
{
    GET_LENGTH_INFORMATION Length;
    NTFS_VOLUME_DATA_BUFFER Before, After;
    FILE_ALLOCATION_INFORMATION Allocation;
    STARTING_LCN_INPUT_BUFFER StartLcn;
    STARTING_VCN_INPUT_BUFFER StartVcn;
    IO_STATUS_BLOCK IoStatus;
    WCHAR Path[MAX_PATH];
    HANDLE File = NULL;
    PUCHAR Buffer;
    ULONGLONG Sectors, PartitionSectors;
    ULONG SectorsPerCluster, i;
    NTSTATUS Status;
    BOOLEAN FoundNew = FALSE;

    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &Before, sizeof(Before), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    *ExpectedSectors = Before.NumberSectors.QuadPart;
    SectorsPerCluster = Before.BytesPerCluster / Before.BytesPerSector;
    Status = ZwDeviceIoControlFile(Volume, NULL, NULL, NULL, &IoStatus,
                                   IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &Length, sizeof(Length));
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    PartitionSectors = Length.Length.QuadPart / Before.BytesPerSector;
    Status = NtfsKmtFsctl(Volume, FSCTL_EXTEND_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Sectors = Before.NumberSectors.QuadPart + 1;
    Status = NtfsKmtFsctl(Volume, FSCTL_EXTEND_VOLUME, &Sectors, sizeof(Sectors) - 1, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = NtfsKmtFsctl(Volume, FSCTL_EXTEND_VOLUME, &Sectors, sizeof(Sectors), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Sectors = Before.NumberSectors.QuadPart - SectorsPerCluster;
    Status = NtfsKmtFsctl(Volume, FSCTL_EXTEND_VOLUME, &Sectors, sizeof(Sectors), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Sectors = Before.NumberSectors.QuadPart + SectorsPerCluster;
    Status = NtfsKmtFsctl(Volume, FSCTL_EXTEND_VOLUME, &Sectors, sizeof(Sectors), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Sectors = PartitionSectors + SectorsPerCluster;
    Status = NtfsKmtFsctl(Volume, FSCTL_EXTEND_VOLUME, &Sectors, sizeof(Sectors), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &After, sizeof(After), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_longlong(After.NumberSectors.QuadPart, Before.NumberSectors.QuadPart);
        ok_eq_longlong(After.TotalClusters.QuadPart, Before.TotalClusters.QuadPart);
        ok_eq_longlong(After.FreeClusters.QuadPart, Before.FreeClusters.QuadPart);
    }

    /* A live-growth fixture formats only part of the partition. Ordinary
     * full-partition fixtures still exercise all rejection cases above. */
    if (PartitionSectors <= Before.NumberSectors.QuadPart + 1 + 64 * SectorsPerCluster)
        return;
    Sectors = PartitionSectors / 2;
    if (Sectors <= Before.NumberSectors.QuadPart + 1 + 64 * SectorsPerCluster)
        Sectors = PartitionSectors;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"extended.bin");
    Status = NtfsKmtCreateFileWithData(Path, "grow", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    *Created = NT_SUCCESS(Status);
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    /* Open files and a shared volume handle do not require a volume lock. */
    Status = NtfsKmtFsctl(File, FSCTL_EXTEND_VOLUME, &Sectors, sizeof(Sectors), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = NtfsKmtFsctl(Volume, FSCTL_EXTEND_VOLUME, &Sectors, sizeof(Sectors), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto CleanupExtend;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &After, sizeof(After), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto CleanupExtend;
    *ExpectedSectors = Sectors - 1;
    ok_eq_longlong(After.NumberSectors.QuadPart, Sectors - 1);
    ok_eq_longlong(After.TotalClusters.QuadPart, (Sectors - 1) / SectorsPerCluster);
    ok(After.FreeClusters.QuadPart > Before.FreeClusters.QuadPart, "extend did not expose free space\n");
    ok(After.FreeClusters.QuadPart <= Before.FreeClusters.QuadPart +
                                     After.TotalClusters.QuadPart - Before.TotalClusters.QuadPart,
       "extend lost existing allocation claims\n");
    Buffer = NtfsKmtAlloc(max(8192, 2 * Before.BytesPerSector));
    if (Buffer == NULL) goto CleanupExtend;
    StartLcn.StartingLcn = Before.TotalClusters;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_VOLUME_BITMAP, &StartLcn, sizeof(StartLcn), Buffer, 8192, NULL);
    ok(Status == STATUS_SUCCESS || Status == STATUS_BUFFER_OVERFLOW, "bitmap status %lx\n", Status);
    if (Status == STATUS_SUCCESS || Status == STATUS_BUFFER_OVERFLOW)
    {
        PVOLUME_BITMAP_BUFFER Bitmap = (PVOLUME_BITMAP_BUFFER)Buffer;
        ULONGLONG Bit = Before.TotalClusters.QuadPart - Bitmap->StartingLcn.QuadPart;
        ok(!(Bitmap->Buffer[Bit / 8] & (1 << (Bit % 8))), "old bitmap padding did not become free\n");
    }
    /* Requiring more clusters than were free before growth forces at least
     * one retrieval extent into the newly exposed allocation range. */
    Allocation.AllocationSize.QuadPart = (Before.FreeClusters.QuadPart + 1) * Before.BytesPerCluster;
    Status = NtfsKmtSetInfo(File, &Allocation, sizeof(Allocation), FileAllocationInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    StartVcn.StartingVcn.QuadPart = 0;
    Status = NtfsKmtFsctl(File, FSCTL_GET_RETRIEVAL_POINTERS, &StartVcn, sizeof(StartVcn), Buffer, 8192, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PRETRIEVAL_POINTERS_BUFFER Runs = (PRETRIEVAL_POINTERS_BUFFER)Buffer;
        LONGLONG Vcn = Runs->StartingVcn.QuadPart;
        ok(Runs->ExtentCount <= (8192 - FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents)) /
                                sizeof(Runs->Extents[0]), "oversized retrieval result\n");
        for (i = 0; i < Runs->ExtentCount &&
                    i < (8192 - FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents)) / sizeof(Runs->Extents[0]); i++)
        {
            if (Runs->Extents[i].Lcn.QuadPart >= 0 &&
                Runs->Extents[i].Lcn.QuadPart + Runs->Extents[i].NextVcn.QuadPart - Vcn > Before.TotalClusters.QuadPart)
                FoundNew = TRUE;
            Vcn = Runs->Extents[i].NextVcn.QuadPart;
        }
        ok(FoundNew, "allocation did not reach the extended region\n");
    }
    if (Sectors < PartitionSectors)
    {
        NTFS_VOLUME_DATA_BUFFER Full;

        Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &Full, sizeof(Full), NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Allocation.AllocationSize.QuadPart += Full.FreeClusters.QuadPart * Full.BytesPerCluster;
            Status = NtfsKmtSetInfo(File, &Allocation, sizeof(Allocation), FileAllocationInformation);
            ok_eq_hex(Status, STATUS_SUCCESS);
            Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &Full, sizeof(Full), NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status)) ok_eq_longlong(Full.FreeClusters.QuadPart, 0);
            Sectors = PartitionSectors;
            Status = NtfsKmtFsctl(Volume, FSCTL_EXTEND_VOLUME, &Sectors, sizeof(Sectors), NULL, 0, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                *ExpectedSectors = Sectors - 1;
                Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &Full, sizeof(Full), NULL);
                ok_eq_hex(Status, STATUS_SUCCESS);
                if (NT_SUCCESS(Status))
                {
                    ok_eq_longlong(Full.NumberSectors.QuadPart, Sectors - 1);
                    ok(Full.FreeClusters.QuadPart > 0, "full-volume extend exposed no free space\n");
                }
            }
        }
    }
    /* Raw reads observe both boot sectors, including the partition's final
     * sector. Their complete sector contents must agree after success. */
    Status = NtfsKmtRead(Volume, 0, Buffer, Before.BytesPerSector, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRead(Volume, (Sectors - 1) * Before.BytesPerSector,
                          Buffer + Before.BytesPerSector, Before.BytesPerSector, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok(RtlEqualMemory(Buffer, Buffer + Before.BytesPerSector, Before.BytesPerSector),
           "primary and backup boot sectors differ\n");
    NtfsKmtFree(Buffer);
    CheckBadClusterRange(Volume, (Sectors - 1) / SectorsPerCluster, Before.BytesPerCluster,
                          Before.BytesPerFileRecordSegment);
CleanupExtend:
    ZwClose(File);
}

START_TEST(NtfsVolumeControl)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    WCHAR Path[MAX_PATH];
    WCHAR Letter;
    NTSTATUS Status;
    HANDLE Volume, File;
    ULONGLONG ExpectedSectors = 0;
    BOOLEAN ExtendedFile = FALSE;
    NTFS_VOLUME_DATA_BUFFER VolumeData;
    UCHAR Data[16];
    ULONG_PTR Read;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }
    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_volctl", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"survivor.txt");
    Status = NtfsKmtCreateFileWithData(Path, "survives dismount", 17);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestPartitionInformation(Volume);
    TestExtendVolume(Volume, Dir, &ExpectedSectors, &ExtendedFile);

    /* Another open handle on the volume blocks the lock. */
    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status))
        NtfsKmtFsctl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    if (File != NULL)
        ZwClose(File);

    /* Unlock without a lock. */
    Status = NtfsKmtFsctl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_NOT_LOCKED);

    /* Lock with no other user handle: opens are then refused. */
    Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_ACCESS_DENIED);
        if (NT_SUCCESS(Status))
            ZwClose(File);
        Status = NtfsKmtFsctl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtFsctl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_NOT_LOCKED);
    }

    /* Opens work again after unlock. */
    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(File);

    /* Lock + dismount; the next open remounts and finds the data. */
    Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtFsctl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ZwClose(Volume);

    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Data, sizeof(Data));
        Status = NtfsKmtRead(File, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == 16 && RtlCompareMemory(Data, "survives dismoun", 16) == 16, "data lost across dismount\n");
        ZwClose(File);
    }

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"extended.bin");
    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, ExtendedFile ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtRead(File, 0, Data, 4, &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Read, 4);
        ok(RtlEqualMemory(Data, "grow", 4), "extend changed existing data across remount\n");
        ZwClose(File);
    }
    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                              &VolumeData, sizeof(VolumeData), NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok_eq_longlong(VolumeData.NumberSectors.QuadPart, ExpectedSectors);
            CheckBadClusterRange(Volume, VolumeData.TotalClusters.QuadPart, VolumeData.BytesPerCluster,
                                  VolumeData.BytesPerFileRecordSegment);
        }
        ZwClose(Volume);
    }
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

START_TEST(NtfsVolumeDirty)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    WCHAR Letter;
    NTSTATUS Status;
    HANDLE Volume, DirHandle;
    ULONG State;
    ULONG_PTR Info;
    IO_STATUS_BLOCK IoStatus;
    UCHAR AttrBuffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_FS_ATTRIBUTE_INFORMATION Attr = (PFILE_FS_ATTRIBUTE_INFORMATION)AttrBuffer;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    /* The volume mounted read/write: not flagged read-only, writes work. */
    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_dirty", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = NtfsKmtOpen(&DirHandle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    RtlZeroMemory(AttrBuffer, sizeof(AttrBuffer));
    Status = ZwQueryVolumeInformationFile(DirHandle, &IoStatus, Attr, sizeof(AttrBuffer),
                                          FileFsAttributeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(!(Attr->FileSystemAttributes & FILE_READ_ONLY_VOLUME), "volume mounted read-only\n");

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(DirHandle);
        return;
    }

    /* Only a volume handle can be asked. */
    Status = NtfsKmtFsctl(DirHandle, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &State, sizeof(State), NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ZwClose(DirHandle);

    /* A cleanly formatted, never-corrupted volume is not dirty. */
    State = 0xFFFFFFFF;
    Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &State, sizeof(State), &Info);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_IS_VOLUME_DIRTY not supported (0x%lx)\n", Status))
    {
        ZwClose(Volume);
        return;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, sizeof(ULONG));
    ok(!(State & VOLUME_IS_DIRTY), "volume reported dirty before MARK_VOLUME_DIRTY (0x%lx)\n", State);

    /* MARK_VOLUME_DIRTY sets the bit. */
    Status = NtfsKmtFsctl(Volume, FSCTL_MARK_VOLUME_DIRTY, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    State = 0;
    Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &State, sizeof(State), &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(State & VOLUME_IS_DIRTY, "VOLUME_IS_DIRTY not set after MARK_VOLUME_DIRTY (0x%lx)\n", State);

    /* A dirty volume stays writable while mounted. */
    {
        WCHAR Path[MAX_PATH];
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"after_dirty.txt");
        Status = NtfsKmtCreateFileWithData(Path, "still writable", 14);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    ZwClose(Volume);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
