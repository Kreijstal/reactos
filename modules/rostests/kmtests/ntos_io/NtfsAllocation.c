/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     NTFS fragmented allocation, record overflow and truncation
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

static NTSTATUS
Reserve(HANDLE File, ULONGLONG Size)
{
    FILE_ALLOCATION_INFORMATION Allocation;
    IO_STATUS_BLOCK IoStatus;

    Allocation.AllocationSize.QuadPart = Size;
    return ZwSetInformationFile(File, &IoStatus, &Allocation, sizeof(Allocation), FileAllocationInformation);
}

static NTSTATUS
SetSize(HANDLE File, ULONGLONG Size)
{
    FILE_END_OF_FILE_INFORMATION End;
    IO_STATUS_BLOCK IoStatus;

    End.EndOfFile.QuadPart = Size;
    return ZwSetInformationFile(File, &IoStatus, &End, sizeof(End), FileEndOfFileInformation);
}

static VOID
TestCrowdedGrowth(PCWSTR Root, ULONG StreamCount)
{
    WCHAR Dir[MAX_PATH], Path[MAX_PATH], OtherPath[MAX_PATH], StreamPath[MAX_PATH];
    HANDLE File = NULL, Blocker = NULL, Stream = NULL;
    FILE_FS_SIZE_INFORMATION VolumeSize;
    FILE_STANDARD_INFORMATION Before, After;
    IO_STATUS_BLOCK IoStatus;
    UCHAR Data[64], ReadBack[64];
    PUCHAR Buffer = NULL;
    ULONG i, j, ClusterSize, Length;
    ULONG_PTR Returned;
    NTSTATUS Status;
    BOOLEAN Zero;

    Status = NtfsKmtPrepareDirectory(Root, StreamCount == 8 ? L"KMT_NTFS_ALLOC_SMALL" : L"KMT_NTFS_ALLOC_LARGE", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"grow.bin");
    NtfsKmtJoin(OtherPath, RTL_NUMBER_OF(OtherPath), Dir, L"blocker.bin");
    RtlFillMemory(Data, sizeof(Data), 'U');
    Status = NtfsKmtCreateFileWithData(Path, Data, sizeof(Data));
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;

    /* Fill the file record with resident attributes. Growing its default
     * stream must preserve every named stream when mapping pairs need room. */
    for (i = 0; i < StreamCount; i++)
    {
        RtlStringCchPrintfW(StreamPath, RTL_NUMBER_OF(StreamPath), L"%ls:s%02lu", Path, i);
        for (j = 0; j < sizeof(Data); j++) Data[j] = (UCHAR)(i + j);
        Status = NtfsKmtCreateFileWithData(StreamPath, Data, sizeof(Data));
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) goto Cleanup;
    }
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtCreate(&Blocker, NULL, OtherPath, FILE_ALL_ACCESS, FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = ZwQueryVolumeInformationFile(File, &IoStatus, &VolumeSize, sizeof(VolumeSize), FileFsSizeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ClusterSize = VolumeSize.SectorsPerAllocationUnit * VolumeSize.BytesPerSector;

    /* Interleave reservations to exercise disjoint mapping pairs. Allocation
     * is distinct from EOF, and closing the blocker would release its reserve. */
    for (i = 1; i <= 32; i++)
    {
        Status = Reserve(File, (ULONGLONG)i * ClusterSize);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) goto Cleanup;
        Status = Reserve(Blocker, (ULONGLONG)i * ClusterSize);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) goto Cleanup;
    }
    Status = NtfsKmtQueryInfo(File, &Before, sizeof(Before), FileStandardInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(Before.EndOfFile.QuadPart, sizeof(Data));
    ok(Before.AllocationSize.QuadPart >= 32 * (ULONGLONG)ClusterSize, "reservation was lost\n");

    Status = Reserve(File, ((ULONGLONG)VolumeSize.TotalAllocationUnits.QuadPart + 1) * ClusterSize);
    ok_eq_hex(Status, STATUS_DISK_FULL);
    Status = NtfsKmtQueryInfo(File, &After, sizeof(After), FileStandardInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(After.EndOfFile.QuadPart, Before.EndOfFile.QuadPart);
    ok_eq_longlong(After.AllocationSize.QuadPart, Before.AllocationSize.QuadPart);

    Status = SetSize(File, 32 * (ULONGLONG)ClusterSize);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtRead(File, 0, ReadBack, sizeof(ReadBack), &Returned);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Returned, sizeof(ReadBack));
    RtlFillMemory(Data, sizeof(Data), 'U');
    ok(RtlEqualMemory(ReadBack, Data, sizeof(Data)), "growing changed existing data\n");
    Status = NtfsKmtRead(File, 31 * (ULONGLONG)ClusterSize, ReadBack, sizeof(ReadBack), &Returned);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Returned, sizeof(ReadBack));
    Zero = TRUE;
    for (j = 0; j < sizeof(ReadBack); j++) if (ReadBack[j] != 0) Zero = FALSE;
    ok(Zero, "newly initialized allocation is not zero\n");

    Status = SetSize(File, 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtWrite(File, 65536 + 7, "V", 1, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ZwClose(Blocker); Blocker = NULL;
    ZwClose(File); File = NULL;
    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Length = 65536 + 8;
    Buffer = NtfsKmtAlloc(Length);
    if (Buffer == NULL) { ok(FALSE, "read buffer allocation failed\n"); goto Cleanup; }
    Status = NtfsKmtRead(File, 0, Buffer, Length, &Returned);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Returned, Length);
    if (NT_SUCCESS(Status) && Returned == Length)
    {
        ok(Buffer[0] == 'U' && Buffer[Length - 1] == 'V', "truncate/regrow lost boundary bytes\n");
        Zero = TRUE;
        for (j = 1; j < Length - 1; j++) if (Buffer[j] != 0) Zero = FALSE;
        ok(Zero, "truncate/regrow exposed old allocation contents\n");
    }
    for (i = 0; i < StreamCount; i++)
    {
        RtlStringCchPrintfW(StreamPath, RTL_NUMBER_OF(StreamPath), L"%ls:s%02lu", Path, i);
        Status = NtfsKmtOpen(&Stream, StreamPath, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) goto Cleanup;
        Status = NtfsKmtRead(Stream, 0, ReadBack, sizeof(ReadBack), &Returned);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Returned, sizeof(ReadBack));
        for (j = 0; j < sizeof(Data); j++) Data[j] = (UCHAR)(i + j);
        ok(RtlEqualMemory(ReadBack, Data, sizeof(Data)), "named stream %lu changed during growth\n", i);
        ZwClose(Stream); Stream = NULL;
    }
Cleanup:
    if (Buffer != NULL) NtfsKmtFree(Buffer);
    if (Stream != NULL) ZwClose(Stream);
    if (Blocker != NULL) ZwClose(Blocker);
    if (File != NULL) ZwClose(File);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

START_TEST(NtfsAllocation)
{
    WCHAR Root[MAX_PATH];

    if (!NtfsKmtFindVolume(Root, NULL)) return;
    TestCrowdedGrowth(Root, 8);
    TestCrowdedGrowth(Root, 30);
}
