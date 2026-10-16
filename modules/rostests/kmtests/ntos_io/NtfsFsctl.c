/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS volume/file query FSCTLs
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box coverage of the NTFS query and defrag FSCTLs:
 *  - FSCTL_GET_NTFS_VOLUME_DATA, cross-checked against FileFsSizeInformation
 *    and FileFsVolumeInformation;
 *  - FSCTL_GET_NTFS_FILE_RECORD for $MFT and for a test file;
 *  - FSCTL_GET_VOLUME_BITMAP (whole map, rounded start, overflow, bounds);
 *  - FSCTL_GET_RETRIEVAL_POINTERS (resident/empty file, non-resident file,
 *    StartingVcn in the middle of a run, past the end, buffer sizes,
 *    a fragmented file with an output buffer for one extent);
 *  - FSCTL_MOVE_FILE (moves clusters, data intact, bitmap updated).
 *
 * Replaces the host tests test_fsctl_queries, test_fsctl_retrieval_worker,
 * test_fsctl_retrieval_edgecases (sparse extents are in NtfsSparse) and the
 * MOVE_FILE half of test_fsctl_volume_management / test_fsctl_volmgmt_workers.
 * FSCTL_EXTEND_VOLUME is not covered: it needs a partition that can grow.
 *
 * Windows reference: the FSCTL documentation and MS-FSA 2.1.5.10.x.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define FILE_CLUSTERS   10

static NTFS_VOLUME_DATA_BUFFER VolumeData;
static ULONG ClusterSize;

static
ULONGLONG
RecordNumber(
    _In_ ULONGLONG FileReference)
{
    return FileReference & 0x0000FFFFFFFFFFFFULL;
}

static
VOID
TestVolumeData(
    _In_ HANDLE Volume,
    _In_ HANDLE File)
{
    NTSTATUS Status;
    ULONG_PTR Info;
    IO_STATUS_BLOCK IoStatus;
    FILE_FS_SIZE_INFORMATION Size;
    UCHAR VolumeBuffer[sizeof(FILE_FS_VOLUME_INFORMATION) + 64 * sizeof(WCHAR)];
    PFILE_FS_VOLUME_INFORMATION VolumeInfo = (PFILE_FS_VOLUME_INFORMATION)VolumeBuffer;
    NTFS_VOLUME_DATA_BUFFER Small;

    RtlZeroMemory(&VolumeData, sizeof(VolumeData));
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                          &VolumeData, sizeof(VolumeData), &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, sizeof(NTFS_VOLUME_DATA_BUFFER));

    Status = ZwQueryVolumeInformationFile(File, &IoStatus, &Size, sizeof(Size), FileFsSizeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwQueryVolumeInformationFile(File, &IoStatus, VolumeInfo, sizeof(VolumeBuffer),
                                          FileFsVolumeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);

    ClusterSize = Size.SectorsPerAllocationUnit * Size.BytesPerSector;
    ok_eq_ulong(VolumeData.VolumeSerialNumber.LowPart, VolumeInfo->VolumeSerialNumber);
    ok_eq_ulong(VolumeData.BytesPerSector, Size.BytesPerSector);
    ok_eq_ulong(VolumeData.BytesPerCluster, ClusterSize);
    ok_eq_longlong(VolumeData.TotalClusters.QuadPart, Size.TotalAllocationUnits.QuadPart);
    ok(VolumeData.FreeClusters.QuadPart > 0 &&
       VolumeData.FreeClusters.QuadPart <= VolumeData.TotalClusters.QuadPart,
       "FreeClusters %I64d of %I64d\n", VolumeData.FreeClusters.QuadPart, VolumeData.TotalClusters.QuadPart);
    ok(VolumeData.NumberSectors.QuadPart * VolumeData.BytesPerSector >=
       VolumeData.TotalClusters.QuadPart * VolumeData.BytesPerCluster,
       "NumberSectors %I64d too small\n", VolumeData.NumberSectors.QuadPart);
    ok(VolumeData.BytesPerFileRecordSegment >= 1024 && VolumeData.BytesPerFileRecordSegment <= 4096 &&
       (VolumeData.BytesPerFileRecordSegment & (VolumeData.BytesPerFileRecordSegment - 1)) == 0,
       "BytesPerFileRecordSegment %lu\n", VolumeData.BytesPerFileRecordSegment);
    ok_eq_ulong(VolumeData.ClustersPerFileRecordSegment,
                VolumeData.BytesPerCluster ? VolumeData.BytesPerFileRecordSegment / VolumeData.BytesPerCluster : 0);
    ok(VolumeData.MftValidDataLength.QuadPart >= 16LL * VolumeData.BytesPerFileRecordSegment &&
       VolumeData.BytesPerFileRecordSegment &&
       VolumeData.MftValidDataLength.QuadPart % VolumeData.BytesPerFileRecordSegment == 0,
       "MftValidDataLength %I64d\n", VolumeData.MftValidDataLength.QuadPart);
    ok(VolumeData.MftStartLcn.QuadPart > 0 && VolumeData.MftStartLcn.QuadPart < VolumeData.TotalClusters.QuadPart,
       "MftStartLcn %I64d\n", VolumeData.MftStartLcn.QuadPart);
    ok(VolumeData.Mft2StartLcn.QuadPart > 0 && VolumeData.Mft2StartLcn.QuadPart < VolumeData.TotalClusters.QuadPart &&
       VolumeData.Mft2StartLcn.QuadPart != VolumeData.MftStartLcn.QuadPart,
       "Mft2StartLcn %I64d\n", VolumeData.Mft2StartLcn.QuadPart);
    ok(VolumeData.MftZoneStart.QuadPart <= VolumeData.MftZoneEnd.QuadPart, "MFT zone [%I64d,%I64d)\n",
       VolumeData.MftZoneStart.QuadPart, VolumeData.MftZoneEnd.QuadPart);

    /* A buffer smaller than the structure is too small. */
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                          &Small, sizeof(Small) - 1, NULL);
    ok_eq_hex(Status, STATUS_BUFFER_TOO_SMALL);
}

static
BOOLEAN
FindBytes(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length,
    _In_reads_bytes_(PatternLength) const VOID *Pattern,
    _In_ ULONG PatternLength)
{
    ULONG i;

    for (i = 0; i + PatternLength <= Length; i++)
    {
        if (RtlCompareMemory(Buffer + i, Pattern, PatternLength) == PatternLength)
            return TRUE;
    }
    return FALSE;
}

static
VOID
TestFileRecord(
    _In_ HANDLE Volume,
    _In_ ULONGLONG FileId,
    _In_ PCWSTR FileName)
{
    NTSTATUS Status;
    NTFS_FILE_RECORD_INPUT_BUFFER Input;
    PNTFS_FILE_RECORD_OUTPUT_BUFFER Output;
    ULONG OutputSize = FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + 4096;
    ULONG_PTR Info;

    Output = NtfsKmtAlloc(OutputSize);
    if (Output == NULL)
        return;

    /* Record 0 is $MFT itself. */
    Input.FileReferenceNumber.QuadPart = 0;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_FILE_RECORD, &Input, sizeof(Input),
                          Output, OutputSize, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_ulonglong(RecordNumber(Output->FileReferenceNumber.QuadPart), 0ULL);
        ok_eq_ulong(Output->FileRecordLength, VolumeData.BytesPerFileRecordSegment);
        ok_eq_ulongptr(Info, FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) +
                             Output->FileRecordLength);
        ok(RtlCompareMemory(Output->FileRecordBuffer, "FILE", 4) == 4, "$MFT record lacks FILE magic\n");
        ok(FindBytes(Output->FileRecordBuffer, Output->FileRecordLength, L"$MFT", 8),
           "$MFT record does not carry its name\n");
    }

    /* The test file's record carries its $FILE_NAME. */
    Input.FileReferenceNumber.QuadPart = (LONGLONG)RecordNumber(FileId);
    RtlZeroMemory(Output, OutputSize);
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_FILE_RECORD, &Input, sizeof(Input),
                          Output, OutputSize, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_ulonglong(RecordNumber(Output->FileReferenceNumber.QuadPart), RecordNumber(FileId));
        ok(RtlCompareMemory(Output->FileRecordBuffer, "FILE", 4) == 4, "file record lacks FILE magic\n");
        ok(FindBytes(Output->FileRecordBuffer, Output->FileRecordLength, FileName,
                     (ULONG)(wcslen(FileName) * sizeof(WCHAR))),
           "file record does not carry the file name\n");
    }
    NtfsKmtFree(Output);
}

static
BOOLEAN
BitSet(
    _In_ const UCHAR *Bitmap,
    _In_ ULONGLONG Bit)
{
    return (Bitmap[Bit / 8] >> (Bit % 8)) & 1;
}

/* Returns the whole volume bitmap (pool) or NULL. */
static
PVOLUME_BITMAP_BUFFER
GetBitmap(
    _In_ HANDLE Volume)
{
    STARTING_LCN_INPUT_BUFFER Start;
    PVOLUME_BITMAP_BUFFER Bitmap;
    ULONG Size = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer) +
                 (ULONG)((VolumeData.TotalClusters.QuadPart + 7) / 8) + 8;
    NTSTATUS Status;

    Bitmap = NtfsKmtAlloc(Size);
    if (Bitmap == NULL)
        return NULL;
    Start.StartingLcn.QuadPart = 0;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_VOLUME_BITMAP, &Start, sizeof(Start), Bitmap, Size, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtfsKmtFree(Bitmap);
        return NULL;
    }
    return Bitmap;
}

static
VOID
TestVolumeBitmap(
    _In_ HANDLE Volume,
    _In_ HANDLE File)
{
    NTSTATUS Status;
    PVOLUME_BITMAP_BUFFER Bitmap;
    STARTING_LCN_INPUT_BUFFER Start;
    UCHAR Small[sizeof(VOLUME_BITMAP_BUFFER)];
    PVOLUME_BITMAP_BUFFER SmallBitmap = (PVOLUME_BITMAP_BUFFER)Small;
    NTFS_VOLUME_DATA_BUFFER Now;
    ULONG_PTR Info;
    ULONGLONG Lcn, Free = 0;

    /* Take free-cluster count and bitmap back to back. */
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &Now, sizeof(Now), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Bitmap = GetBitmap(Volume);
    if (Bitmap == NULL)
        return;

    ok_eq_longlong(Bitmap->StartingLcn.QuadPart, 0LL);
    ok_eq_longlong(Bitmap->BitmapSize.QuadPart, VolumeData.TotalClusters.QuadPart);
    ok(BitSet(Bitmap->Buffer, 0), "LCN 0 (boot sector) is free\n");
    ok(BitSet(Bitmap->Buffer, (ULONGLONG)VolumeData.MftStartLcn.QuadPart), "$MFT start LCN is free\n");
    ok(BitSet(Bitmap->Buffer, (ULONGLONG)VolumeData.Mft2StartLcn.QuadPart), "$MFTMirr start LCN is free\n");
    for (Lcn = 0; Lcn < (ULONGLONG)VolumeData.TotalClusters.QuadPart; Lcn++)
    {
        if (!BitSet(Bitmap->Buffer, Lcn))
            Free++;
    }
    ok(Free == (ULONGLONG)Now.FreeClusters.QuadPart, "bitmap has %I64u free clusters, volume data says %I64d\n",
       Free, Now.FreeClusters.QuadPart);
    NtfsKmtFree(Bitmap);

    /* The start is rounded down to a byte boundary; a buffer for 64
     * clusters overflows and returns what fits. */
    Start.StartingLcn.QuadPart = 13;
    RtlZeroMemory(Small, sizeof(Small));
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_VOLUME_BITMAP, &Start, sizeof(Start), Small, sizeof(Small), &Info);
    ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok_eq_longlong(SmallBitmap->StartingLcn.QuadPart, 8LL);
    ok_eq_longlong(SmallBitmap->BitmapSize.QuadPart, VolumeData.TotalClusters.QuadPart - 8);
    ok_eq_ulongptr(Info, sizeof(Small));

    /* A start beyond the volume is invalid. */
    Start.StartingLcn.QuadPart = VolumeData.TotalClusters.QuadPart + 8;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_VOLUME_BITMAP, &Start, sizeof(Start), Small, sizeof(Small), NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* Only a volume handle may ask for the bitmap. */
    Start.StartingLcn.QuadPart = 0;
    Status = NtfsKmtFsctl(File, FSCTL_GET_VOLUME_BITMAP, &Start, sizeof(Start), Small, sizeof(Small), NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
}

/* Query retrieval pointers from StartingVcn into a 4 KiB buffer. */
static
NTSTATUS
GetPointers(
    _In_ HANDLE File,
    _In_ LONGLONG StartingVcn,
    _Out_writes_bytes_(Size) PRETRIEVAL_POINTERS_BUFFER Buffer,
    _In_ ULONG Size,
    _Out_opt_ PULONG_PTR Info)
{
    STARTING_VCN_INPUT_BUFFER Vcn;

    Vcn.StartingVcn.QuadPart = StartingVcn;
    RtlZeroMemory(Buffer, Size);
    return NtfsKmtFsctl(File, FSCTL_GET_RETRIEVAL_POINTERS, &Vcn, sizeof(Vcn), Buffer, Size, Info);
}

/* Map one VCN through a retrieval-pointer buffer; -2 if not covered. */
static
LONGLONG
MapVcn(
    _In_ PRETRIEVAL_POINTERS_BUFFER Rp,
    _In_ LONGLONG Vcn)
{
    LONGLONG Start = Rp->StartingVcn.QuadPart;
    ULONG i;

    for (i = 0; i < Rp->ExtentCount; i++)
    {
        if (Vcn >= Start && Vcn < Rp->Extents[i].NextVcn.QuadPart)
        {
            if (Rp->Extents[i].Lcn.QuadPart == -1)
                return -1;
            return Rp->Extents[i].Lcn.QuadPart + (Vcn - Start);
        }
        Start = Rp->Extents[i].NextVcn.QuadPart;
    }
    return -2;
}

static
VOID
TestRetrievalPointers(
    _In_ PCWSTR Dir,
    _In_ HANDLE File)
{
    NTSTATUS Status;
    PRETRIEVAL_POINTERS_BUFFER Rp, Rp2;
    WCHAR Path[MAX_PATH];
    HANDLE Small;
    FILE_STANDARD_INFORMATION Standard;
    ULONG i;
    LONGLONG TotalVcns;
    STARTING_VCN_INPUT_BUFFER Vcn;
    UCHAR Tiny[sizeof(RETRIEVAL_POINTERS_BUFFER) - 1];
    ULONG_PTR Info;

    Rp = NtfsKmtAlloc(4096);
    Rp2 = NtfsKmtAlloc(4096);
    if (Rp == NULL || Rp2 == NULL)
        goto Free;

    /* Empty and resident files have no clusters to map. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"empty.txt");
    Status = NtfsKmtCreateFileWithData(Path, NULL, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Small, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = GetPointers(Small, 0, Rp, 4096, NULL);
        ok_eq_hex(Status, STATUS_END_OF_FILE);
        ZwClose(Small);
    }
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"resident.txt");
    Status = NtfsKmtCreateFileWithData(Path, "tiny", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Small, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = GetPointers(Small, 0, Rp, 4096, NULL);
        ok_eq_hex(Status, STATUS_END_OF_FILE);
        ZwClose(Small);
    }

    /* The non-resident test file: extents cover exactly its allocation. */
    Status = NtfsKmtQueryStandard(File, &Standard);
    ok_eq_hex(Status, STATUS_SUCCESS);
    TotalVcns = ClusterSize ? Standard.AllocationSize.QuadPart / ClusterSize : 0;
    ok_eq_longlong(TotalVcns, (LONGLONG)FILE_CLUSTERS);

    Status = GetPointers(File, 0, Rp, 4096, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_longlong(Rp->StartingVcn.QuadPart, 0LL);
        ok(Rp->ExtentCount >= 1, "no extents\n");
        ok_eq_ulongptr(Info, FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents) +
                             Rp->ExtentCount * sizeof(Rp->Extents[0]));
        if (Rp->ExtentCount >= 1)
            ok_eq_longlong(Rp->Extents[Rp->ExtentCount - 1].NextVcn.QuadPart, TotalVcns);
        for (i = 0; i < Rp->ExtentCount; i++)
            ok(Rp->Extents[i].Lcn.QuadPart >= 0 && Rp->Extents[i].Lcn.QuadPart < VolumeData.TotalClusters.QuadPart,
               "extent %lu LCN %I64d out of range\n", i, Rp->Extents[i].Lcn.QuadPart);

        /* Starting in the middle of a run: either the run is reported from
         * its start or sliced at the VCN, but VCN 1 maps to the same LCN. */
        Status = GetPointers(File, 1, Rp2, 4096, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok(Rp2->StartingVcn.QuadPart <= 1, "StartingVcn %I64d > requested 1\n", Rp2->StartingVcn.QuadPart);
            ok(MapVcn(Rp2, 1) == MapVcn(Rp, 1), "VCN 1 maps to %I64d from VCN 1 but %I64d from VCN 0\n",
               MapVcn(Rp2, 1), MapVcn(Rp, 1));
        }
    }

    /* Past the last VCN there is nothing to map. */
    Status = GetPointers(File, TotalVcns, Rp, 4096, NULL);
    ok_eq_hex(Status, STATUS_END_OF_FILE);

    /* Too small an input or output buffer. */
    Vcn.StartingVcn.QuadPart = 0;
    Status = NtfsKmtFsctl(File, FSCTL_GET_RETRIEVAL_POINTERS, &Vcn, sizeof(Vcn) - 1, Rp, 4096, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = NtfsKmtFsctl(File, FSCTL_GET_RETRIEVAL_POINTERS, &Vcn, sizeof(Vcn), Tiny, sizeof(Tiny), NULL);
    ok_eq_hex(Status, STATUS_BUFFER_TOO_SMALL);

Free:
    NtfsKmtFree(Rp);
    NtfsKmtFree(Rp2);
}

/* Find Count free clusters, searching down from the end of the volume. */
static
LONGLONG
FindFreeRun(
    _In_ HANDLE Volume,
    _In_ ULONG Count)
{
    PVOLUME_BITMAP_BUFFER Bitmap = GetBitmap(Volume);
    LONGLONG Lcn, Found = -1;
    ULONG Run = 0;

    if (Bitmap == NULL)
        return -1;
    for (Lcn = VolumeData.TotalClusters.QuadPart - 1; Lcn >= 0; Lcn--)
    {
        if (BitSet(Bitmap->Buffer, (ULONGLONG)Lcn))
        {
            Run = 0;
            continue;
        }
        /* Stay clear of the last cluster (backup boot sector). */
        if (Lcn >= VolumeData.TotalClusters.QuadPart - 2)
            continue;
        if (++Run == Count)
        {
            Found = Lcn;
            break;
        }
    }
    NtfsKmtFree(Bitmap);
    return Found;
}

static
VOID
TestMoveFile(
    _In_ HANDLE Volume,
    _In_ PCWSTR FilePath,
    _In_ HANDLE File)
{
    NTSTATUS Status;
    MOVE_FILE_DATA Move;
    PRETRIEVAL_POINTERS_BUFFER Rp;
    LONGLONG Target, OldLcn;
    PVOLUME_BITMAP_BUFFER Bitmap;
    HANDLE Raw;
    PUCHAR Data;
    ULONG_PTR Read;
    BOOLEAN HavePointers;

    Rp = NtfsKmtAlloc(4096);
    Data = NtfsKmtAlloc(FILE_CLUSTERS * ClusterSize);
    if (Rp == NULL || Data == NULL)
        goto Free;

    /* The move itself does not depend on FSCTL_GET_RETRIEVAL_POINTERS: when
     * that fails (covered above) the move is still checked through the
     * bitmap and the data. */
    Status = GetPointers(File, 0, Rp, 4096, NULL);
    HavePointers = NT_SUCCESS(Status);
    OldLcn = HavePointers ? MapVcn(Rp, 4) : -1;

    /* Move VCNs 4-5 to free clusters near the end of the volume. */
    Target = FindFreeRun(Volume, 2);
    ok(Target > 0, "no free 2-cluster run on the volume\n");
    if (Target <= 0)
        goto Free;

    RtlZeroMemory(&Move, sizeof(Move));
    Move.FileHandle = File;
    Move.StartingVcn.QuadPart = 4;
    Move.StartingLcn.QuadPart = Target;
    Move.ClusterCount = 2;
    Status = NtfsKmtFsctl(Volume, FSCTL_MOVE_FILE, &Move, sizeof(Move), NULL, 0, NULL);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_MOVE_FILE not supported (0x%lx)\n", Status))
        goto Free;
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Free;

    Status = GetPointers(File, 0, Rp, 4096, NULL);
    if (HavePointers)
    {
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_longlong(MapVcn(Rp, 4), Target);
        ok_eq_longlong(MapVcn(Rp, 5), Target + 1);
        ok(MapVcn(Rp, 3) >= 0 && MapVcn(Rp, 6) >= 0, "neighbouring VCNs lost their mapping\n");
        ok(Rp->ExtentCount >= 2, "file not fragmented after the move (%lu extents)\n", Rp->ExtentCount);
    }

    /* The bitmap follows: new clusters used, old ones released. */
    Bitmap = GetBitmap(Volume);
    if (Bitmap != NULL)
    {
        ok(BitSet(Bitmap->Buffer, (ULONGLONG)Target) && BitSet(Bitmap->Buffer, (ULONGLONG)Target + 1),
           "moved-to clusters not marked in use\n");
        if (OldLcn >= 0)
            ok(!BitSet(Bitmap->Buffer, (ULONGLONG)OldLcn), "moved-from cluster %I64d still in use\n", OldLcn);
        NtfsKmtFree(Bitmap);
    }

    /* A fragmented file with room for one extent: STATUS_BUFFER_OVERFLOW,
     * and the next call continues where the first stopped. */
    if (HavePointers && Rp->ExtentCount >= 2)
    {
        RETRIEVAL_POINTERS_BUFFER One;
        ULONG_PTR Info;
        STARTING_VCN_INPUT_BUFFER Vcn;

        Vcn.StartingVcn.QuadPart = 0;
        Status = NtfsKmtFsctl(File, FSCTL_GET_RETRIEVAL_POINTERS, &Vcn, sizeof(Vcn), &One, sizeof(One), &Info);
        ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
        ok_eq_ulong(One.ExtentCount, 1UL);
        ok_eq_ulongptr(Info, sizeof(One));
        ok_eq_longlong(One.Extents[0].NextVcn.QuadPart, Rp->Extents[0].NextVcn.QuadPart);
        Vcn.StartingVcn = One.Extents[0].NextVcn;
        Status = NtfsKmtFsctl(File, FSCTL_GET_RETRIEVAL_POINTERS, &Vcn, sizeof(Vcn), &One, sizeof(One), &Info);
        ok(Status == STATUS_SUCCESS || Status == STATUS_BUFFER_OVERFLOW, "continuation 0x%lx\n", Status);
        ok_eq_longlong(One.StartingVcn.QuadPart, Rp->Extents[0].NextVcn.QuadPart);
        ok_eq_longlong(One.Extents[0].Lcn.QuadPart, Rp->Extents[1].Lcn.QuadPart);
    }

    /* The data is intact on disk: read it back without the cache. */
    Status = NtfsKmtCreate(&Raw, NULL, FilePath, GENERIC_READ, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                           FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtRead(Raw, 0, Data, FILE_CLUSTERS * ClusterSize, &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Read, FILE_CLUSTERS * ClusterSize);
        ok(NtfsKmtCheckPattern(Data, FILE_CLUSTERS * ClusterSize, 0x4D4F, 0) == (SIZE_T)-1,
           "data differs after FSCTL_MOVE_FILE at %Iu\n",
           NtfsKmtCheckPattern(Data, FILE_CLUSTERS * ClusterSize, 0x4D4F, 0));
        ZwClose(Raw);
    }

Free:
    NtfsKmtFree(Rp);
    NtfsKmtFree(Data);
}

START_TEST(NtfsFsctl)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    WCHAR Path[MAX_PATH];
    WCHAR Letter;
    NTSTATUS Status;
    HANDLE Volume, File;
    PUCHAR Data;
    IO_STATUS_BLOCK IoStatus;
    FILE_FS_SIZE_INFORMATION Size;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_fsctl", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* A non-resident file of exactly FILE_CLUSTERS clusters. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"clusters.bin");
    Status = NtfsKmtCreate(&File, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(Volume);
        return;
    }
    ClusterSize = 4096;
    if (NT_SUCCESS(ZwQueryVolumeInformationFile(File, &IoStatus, &Size, sizeof(Size), FileFsSizeInformation)))
        ClusterSize = Size.SectorsPerAllocationUnit * Size.BytesPerSector;
    Data = NtfsKmtAlloc(FILE_CLUSTERS * ClusterSize);
    if (Data != NULL)
    {
        NtfsKmtFillPattern(Data, FILE_CLUSTERS * ClusterSize, 0x4D4F, 0);
        Status = NtfsKmtWrite(File, 0, Data, FILE_CLUSTERS * ClusterSize, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        NtfsKmtFlush(File);
        NtfsKmtFree(Data);
    }

    TestVolumeData(Volume, File);
    TestFileRecord(Volume, NtfsKmtQueryFileId(File), L"clusters.bin");
    TestVolumeBitmap(Volume, File);
    TestRetrievalPointers(Dir, File);
    TestMoveFile(Volume, Path, File);

    ZwClose(File);
    ZwClose(Volume);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
