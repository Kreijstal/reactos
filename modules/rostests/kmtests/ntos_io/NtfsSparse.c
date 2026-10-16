/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS sparse files
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box sparse-file coverage: FSCTL_SET_SPARSE, writes that leave and
 * cross holes, FSCTL_QUERY_ALLOCATED_RANGES, FSCTL_SET_ZERO_DATA, sparse
 * extents in FSCTL_GET_RETRIEVAL_POINTERS, and a write above 4 GiB.
 *
 * Replaces the host tests test_sparse_write (hole fill), test_sparse_cross_hole
 * (a write that starts in allocated data and runs into a hole) and the sparse
 * half of test_fsctl_retrieval_edgecases.
 *
 * Windows reference: FSCTL_SET_SPARSE / FSCTL_SET_ZERO_DATA /
 * FSCTL_QUERY_ALLOCATED_RANGES documentation, MS-FSA 2.1.5.10.x. NTFS
 * allocates sparse files in compression units (16 clusters), so every range
 * below is 64 KiB aligned.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define KB              1024ULL
#define MB              (1024ULL * 1024ULL)
#define GB              (1024ULL * 1024ULL * 1024ULL)
#define UNIT            (64 * KB)

static ULONG ClusterSize;

static
NTSTATUS
QueryRanges(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _In_ ULONGLONG Length,
    _Out_writes_(MaxRanges) PFILE_ALLOCATED_RANGE_BUFFER Ranges,
    _In_ ULONG MaxRanges,
    _Out_ PULONG Count)
{
    FILE_ALLOCATED_RANGE_BUFFER Input;
    ULONG_PTR Info = 0;
    NTSTATUS Status;

    Input.FileOffset.QuadPart = (LONGLONG)Offset;
    Input.Length.QuadPart = (LONGLONG)Length;
    RtlZeroMemory(Ranges, MaxRanges * sizeof(*Ranges));
    Status = NtfsKmtFsctl(Handle, FSCTL_QUERY_ALLOCATED_RANGES, &Input, sizeof(Input),
                          Ranges, MaxRanges * sizeof(*Ranges), &Info);
    *Count = (ULONG)(Info / sizeof(*Ranges));
    return Status;
}

static
NTSTATUS
ZeroData(
    _In_ HANDLE Handle,
    _In_ ULONGLONG From,
    _In_ ULONGLONG To)
{
    FILE_ZERO_DATA_INFORMATION Zero;

    Zero.FileOffset.QuadPart = (LONGLONG)From;
    Zero.BeyondFinalZero.QuadPart = (LONGLONG)To;
    return NtfsKmtFsctl(Handle, FSCTL_SET_ZERO_DATA, &Zero, sizeof(Zero), NULL, 0, NULL);
}

static
VOID
CheckRead(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _In_ ULONG Seed,
    _In_ BOOLEAN ExpectZero,
    _In_ PCSTR What)
{
    PUCHAR Buffer = NtfsKmtAlloc(Length);
    ULONG_PTR Read = 0;
    NTSTATUS Status;

    if (Buffer == NULL)
        return;
    Status = NtfsKmtRead(Handle, Offset, Buffer, Length, &Read);
    ok(Status == STATUS_SUCCESS, "%s: read at %I64u status 0x%lx\n", What, Offset, Status);
    ok(Read == Length, "%s: read %Iu bytes, expected %lu\n", What, Read, Length);
    if (ExpectZero)
        ok(NtfsKmtIsZero(Buffer, Length), "%s: range at %I64u is not zero\n", What, Offset);
    else
        ok(NtfsKmtCheckPattern(Buffer, Length, Seed, Offset) == (SIZE_T)-1,
           "%s: data mismatch at %I64u + %Iu\n", What, Offset,
           NtfsKmtCheckPattern(Buffer, Length, Seed, Offset));
    NtfsKmtFree(Buffer);
}

static
VOID
WritePattern(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _In_ ULONG Seed)
{
    PUCHAR Buffer = NtfsKmtAlloc(Length);
    ULONG_PTR Written = 0;
    NTSTATUS Status;

    if (Buffer == NULL)
        return;
    NtfsKmtFillPattern(Buffer, Length, Seed, Offset);
    Status = NtfsKmtWrite(Handle, Offset, Buffer, Length, &Written);
    ok(Status == STATUS_SUCCESS, "write at %I64u status 0x%lx\n", Offset, Status);
    ok(Written == Length, "write at %I64u wrote %Iu of %lu\n", Offset, Written, Length);
    NtfsKmtFree(Buffer);
}

static
LONGLONG
QueryEof(
    _In_ HANDLE Handle)
{
    FILE_STANDARD_INFORMATION Standard;

    if (!NT_SUCCESS(NtfsKmtQueryStandard(Handle, &Standard)))
        return -1;
    return Standard.EndOfFile.QuadPart;
}

static
VOID
TestNonSparseRanges(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    FILE_ALLOCATED_RANGE_BUFFER Ranges[4];
    ULONG Count;

    /* A non-sparse file reports the whole queried range (clipped to EOF) as
     * one allocated range. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"dense.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ, FILE_CREATE, FILE_NON_DIRECTORY_FILE,
                           NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    WritePattern(Handle, 0, 10000, 1);

    Status = QueryRanges(Handle, 0, 1 * MB, Ranges, RTL_NUMBER_OF(Ranges), &Count);
    if (!skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_QUERY_ALLOCATED_RANGES not supported (0x%lx)\n", Status))
    {
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Count, 1UL);
        ok_eq_longlong(Ranges[0].FileOffset.QuadPart, 0LL);
        ok_eq_longlong(Ranges[0].Length.QuadPart, 10000LL);
    }

    /* FSCTL_SET_ZERO_DATA on a non-sparse file writes zeros in place. */
    Status = ZeroData(Handle, 100, 5000);
    if (!skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_SET_ZERO_DATA not supported (0x%lx)\n", Status))
    {
        ok_eq_hex(Status, STATUS_SUCCESS);
        CheckRead(Handle, 0, 100, 1, FALSE, "dense head");
        CheckRead(Handle, 100, 4900, 1, TRUE, "dense zeroed");
        CheckRead(Handle, 5000, 5000, 1, FALSE, "dense tail");
        ok_eq_longlong(QueryEof(Handle), 10000LL);
    }
    ZwClose(Handle);
}

static
VOID
TestSparse(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    FILE_BASIC_INFORMATION Basic;
    FILE_COMPRESSION_INFORMATION Compression;
    FILE_ALLOCATED_RANGE_BUFFER Ranges[8];
    ULONG Count;
    ULONG_PTR Info;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"sparse.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ, FILE_CREATE, FILE_NON_DIRECTORY_FILE,
                           NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtfsKmtFsctl(Handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, NULL);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_SET_SPARSE not supported (0x%lx)\n", Status))
    {
        ZwClose(Handle);
        return;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = NtfsKmtQueryInfo(Handle, &Basic, sizeof(Basic), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Basic.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE,
       "FILE_ATTRIBUTE_SPARSE_FILE missing: 0x%lx\n", Basic.FileAttributes);

    /* [0,64K) data, [64K,1M) hole, [1M,1M+64K) data. */
    WritePattern(Handle, 0, (ULONG)UNIT, 7);
    WritePattern(Handle, 1 * MB, (ULONG)UNIT, 7);
    ok_eq_longlong(QueryEof(Handle), (LONGLONG)(1 * MB + UNIT));
    CheckRead(Handle, 0, (ULONG)UNIT, 7, FALSE, "first unit");
    CheckRead(Handle, UNIT, (ULONG)(1 * MB - UNIT), 0, TRUE, "hole");
    CheckRead(Handle, 1 * MB, (ULONG)UNIT, 7, FALSE, "second unit");

    Status = QueryRanges(Handle, 0, 1 * MB + UNIT, Ranges, RTL_NUMBER_OF(Ranges), &Count);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Count, 2UL);
    ok_eq_longlong(Ranges[0].FileOffset.QuadPart, 0LL);
    ok_eq_longlong(Ranges[0].Length.QuadPart, (LONGLONG)UNIT);
    ok_eq_longlong(Ranges[1].FileOffset.QuadPart, (LONGLONG)(1 * MB));
    ok_eq_longlong(Ranges[1].Length.QuadPart, (LONGLONG)UNIT);

    /* An output buffer for one range: STATUS_BUFFER_OVERFLOW, one returned. */
    Status = QueryRanges(Handle, 0, 1 * MB + UNIT, Ranges, 1, &Count);
    ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok_eq_ulong(Count, 1UL);

    /* Only the two units are allocated. */
    Status = NtfsKmtQueryInfo(Handle, &Compression, sizeof(Compression),
                              FileCompressionInformation, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok_eq_longlong(Compression.CompressedFileSize.QuadPart, (LONGLONG)(2 * UNIT));

    /* A write that starts inside allocated data and runs into the hole
     * (test_sparse_cross_hole): the untouched prefix survives, the written
     * bytes land at their own offset, the rest of the hole stays zero. */
    WritePattern(Handle, 60 * KB, (ULONG)(8 * KB), 9);
    CheckRead(Handle, 0, (ULONG)(60 * KB), 7, FALSE, "cross-hole prefix");
    CheckRead(Handle, 60 * KB, (ULONG)(8 * KB), 9, FALSE, "cross-hole write");
    CheckRead(Handle, 68 * KB, (ULONG)(1 * MB - 68 * KB), 0, TRUE, "rest of hole");
    CheckRead(Handle, 1 * MB, (ULONG)UNIT, 7, FALSE, "second unit after cross-hole");

    Status = QueryRanges(Handle, 0, 1 * MB + UNIT, Ranges, RTL_NUMBER_OF(Ranges), &Count);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Count, 2UL);
    ok_eq_longlong(Ranges[0].FileOffset.QuadPart, 0LL);
    ok_eq_longlong(Ranges[0].Length.QuadPart, (LONGLONG)(2 * UNIT));
    ok_eq_longlong(Ranges[1].FileOffset.QuadPart, (LONGLONG)(1 * MB));

    /* A hole fill in the middle of the hole (test_sparse_write). */
    WritePattern(Handle, 512 * KB, (ULONG)(4 * KB), 11);
    CheckRead(Handle, 512 * KB, (ULONG)(4 * KB), 11, FALSE, "hole fill");
    CheckRead(Handle, 2 * UNIT, (ULONG)(512 * KB - 2 * UNIT), 0, TRUE, "hole before fill");
    CheckRead(Handle, 516 * KB, (ULONG)(1 * MB - 516 * KB), 0, TRUE, "hole after fill");
    Status = QueryRanges(Handle, 0, 1 * MB + UNIT, Ranges, RTL_NUMBER_OF(Ranges), &Count);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Count, 3UL);
    ok(Ranges[1].FileOffset.QuadPart <= (LONGLONG)(512 * KB) &&
       Ranges[1].FileOffset.QuadPart + Ranges[1].Length.QuadPart >= (LONGLONG)(516 * KB),
       "hole-fill range [%I64d,+%I64d) does not cover the write\n",
       Ranges[1].FileOffset.QuadPart, Ranges[1].Length.QuadPart);

    /* SET_ZERO_DATA over a whole unit deallocates it; EOF stays. */
    Status = ZeroData(Handle, 1 * MB, 1 * MB + UNIT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    CheckRead(Handle, 1 * MB, (ULONG)UNIT, 0, TRUE, "zeroed unit");
    ok_eq_longlong(QueryEof(Handle), (LONGLONG)(1 * MB + UNIT));
    Status = QueryRanges(Handle, 0, 1 * MB + UNIT, Ranges, RTL_NUMBER_OF(Ranges), &Count);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Count, 2UL);
    ok(Count < 2 || Ranges[Count - 1].FileOffset.QuadPart + Ranges[Count - 1].Length.QuadPart <= (LONGLONG)(1 * MB),
       "zeroed unit still allocated\n");

    /* SET_ZERO_DATA on a partial unit zeroes without touching neighbours. */
    Status = ZeroData(Handle, 10 * KB, 20 * KB);
    ok_eq_hex(Status, STATUS_SUCCESS);
    CheckRead(Handle, 0, (ULONG)(10 * KB), 7, FALSE, "before partial zero");
    CheckRead(Handle, 10 * KB, (ULONG)(10 * KB), 0, TRUE, "partial zero");
    CheckRead(Handle, 20 * KB, (ULONG)(40 * KB), 7, FALSE, "after partial zero");

    /* FSCTL_GET_RETRIEVAL_POINTERS reports holes as LCN -1 extents and the
     * extents cover the file to its last allocated cluster. */
    {
        STARTING_VCN_INPUT_BUFFER Vcn;
        PRETRIEVAL_POINTERS_BUFFER Rp = NtfsKmtAlloc(4096);
        ULONG i;
        BOOLEAN SawHole = FALSE;

        if (Rp != NULL)
        {
            Vcn.StartingVcn.QuadPart = 0;
            Status = NtfsKmtFsctl(Handle, FSCTL_GET_RETRIEVAL_POINTERS, &Vcn, sizeof(Vcn),
                                  Rp, 4096, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                ok(Rp->ExtentCount >= 2, "ExtentCount %lu\n", Rp->ExtentCount);
                ok_eq_longlong(Rp->StartingVcn.QuadPart, 0LL);
                for (i = 0; i < Rp->ExtentCount; i++)
                {
                    if (Rp->Extents[i].Lcn.QuadPart == -1)
                        SawHole = TRUE;
                }
                ok(SawHole, "no LCN -1 extent for the hole\n");
                if (Rp->ExtentCount > 0)
                    ok(Rp->Extents[Rp->ExtentCount - 1].NextVcn.QuadPart * ClusterSize >= (LONGLONG)(516 * KB),
                       "extents end at VCN %I64d\n", Rp->Extents[Rp->ExtentCount - 1].NextVcn.QuadPart);
            }
            NtfsKmtFree(Rp);
        }
    }

    /* Above 4 GiB: a sparse file can hold data at 5 GiB on a small volume;
     * a 32-bit truncation of the offset would land it at 1 GiB instead. */
    WritePattern(Handle, 5 * GB, (ULONG)(4 * KB), 13);
    ok_eq_longlong(QueryEof(Handle), (LONGLONG)(5 * GB + 4 * KB));
    CheckRead(Handle, 5 * GB, (ULONG)(4 * KB), 13, FALSE, "5 GiB data");
    CheckRead(Handle, 4 * GB, (ULONG)(64 * KB), 0, TRUE, "4 GiB hole");
    CheckRead(Handle, 1 * GB, (ULONG)(64 * KB), 0, TRUE, "1 GiB hole (32-bit alias)");
    CheckRead(Handle, 0, (ULONG)(10 * KB), 7, FALSE, "head after 5 GiB write");
    Status = QueryRanges(Handle, 4 * GB, 2 * GB, Ranges, RTL_NUMBER_OF(Ranges), &Count);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Count, 1UL);
    ok(Ranges[0].FileOffset.QuadPart <= (LONGLONG)(5 * GB) &&
       Ranges[0].FileOffset.QuadPart + Ranges[0].Length.QuadPart >= (LONGLONG)(5 * GB + 4 * KB),
       "5 GiB range [%I64d,+%I64d)\n", Ranges[0].FileOffset.QuadPart, Ranges[0].Length.QuadPart);

    /* Close and reopen: everything above is on disk, not only in cache. */
    ZwClose(Handle);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    ok_eq_longlong(QueryEof(Handle), (LONGLONG)(5 * GB + 4 * KB));
    CheckRead(Handle, 0, (ULONG)(10 * KB), 7, FALSE, "reopen head");
    CheckRead(Handle, 60 * KB, (ULONG)(8 * KB), 9, FALSE, "reopen cross-hole");
    CheckRead(Handle, 512 * KB, (ULONG)(4 * KB), 11, FALSE, "reopen hole fill");
    CheckRead(Handle, 1 * MB, (ULONG)UNIT, 0, TRUE, "reopen zeroed unit");
    CheckRead(Handle, 5 * GB, (ULONG)(4 * KB), 13, FALSE, "reopen 5 GiB");
    ZwClose(Handle);
}

START_TEST(NtfsSparse)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;
    HANDLE Handle;
    FILE_FS_SIZE_INFORMATION Size;
    IO_STATUS_BLOCK IoStatus;

    if (!NtfsKmtFindVolume(Root, NULL))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_sparse", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    ClusterSize = 4096;
    if (NT_SUCCESS(NtfsKmtOpen(&Handle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE)))
    {
        if (NT_SUCCESS(ZwQueryVolumeInformationFile(Handle, &IoStatus, &Size, sizeof(Size),
                                                    FileFsSizeInformation)))
            ClusterSize = Size.SectorsPerAllocationUnit * Size.BytesPerSector;
        ZwClose(Handle);
    }

    TestNonSparseRanges(Dir);
    TestSparse(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
