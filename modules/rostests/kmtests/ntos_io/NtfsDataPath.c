/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS read/write data path
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box data-path coverage:
 *  - unaligned and tail reads of resident and non-resident files, cached and
 *    non-cached, before and after reopen (the bug7 unaligned-read class);
 *  - resident -> non-resident growth in steps (test_resident_grow);
 *  - truncate / extend through FileEndOfFileInformation and
 *    FileAllocationInformation, zero-filled gaps;
 *  - offsets above 4 GiB on a non-sparse file (DISK_FULL, no 32-bit wrap);
 *  - $ATTRIBUTE_LIST growth: many named streams on one file, and enough
 *    hard links that the attribute list itself goes non-resident
 *    (test_attribute_list_migrate, test_attrlist_nonresident).
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define KB              1024UL
#define NONRES_SIZE     (3 * 4096 + 123)
#define STREAM_COUNT    40
#define LINK_COUNT      80

static ULONG ClusterSize = 4096;

static
VOID
ReadCheck(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _In_ NTSTATUS ExpectStatus,
    _In_ ULONG ExpectRead,
    _In_ ULONG Seed,
    _In_ PCSTR What)
{
    PUCHAR Buffer = NtfsKmtAlloc(Length + 16);
    ULONG_PTR Read = 0;
    NTSTATUS Status;

    if (Buffer == NULL)
        return;
    RtlFillMemory(Buffer, Length + 16, 0xCC);
    Status = NtfsKmtRead(Handle, Offset, Buffer, Length, &Read);
    ok(Status == ExpectStatus, "%s: read(%I64u,%lu) status 0x%lx, expected 0x%lx\n",
       What, Offset, Length, Status, ExpectStatus);
    ok(Read == ExpectRead, "%s: read(%I64u,%lu) returned %Iu bytes, expected %lu\n",
       What, Offset, Length, Read, ExpectRead);
    if (NT_SUCCESS(Status) && Read == ExpectRead)
    {
        ok(NtfsKmtCheckPattern(Buffer, ExpectRead, Seed, Offset) == (SIZE_T)-1,
           "%s: data mismatch at +%Iu\n", What, NtfsKmtCheckPattern(Buffer, ExpectRead, Seed, Offset));
        ok(Buffer[Length] == 0xCC, "%s: read wrote past the requested length\n", What);
    }
    NtfsKmtFree(Buffer);
}

static
NTSTATUS
WritePattern(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _In_ ULONG Seed)
{
    PUCHAR Buffer = NtfsKmtAlloc(Length);
    NTSTATUS Status;
    ULONG_PTR Written = 0;

    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    NtfsKmtFillPattern(Buffer, Length, Seed, Offset);
    Status = NtfsKmtWrite(Handle, Offset, Buffer, Length, &Written);
    if (NT_SUCCESS(Status) && Written != Length)
        Status = STATUS_UNSUCCESSFUL;
    NtfsKmtFree(Buffer);
    return Status;
}

static
FILE_STANDARD_INFORMATION
Standard(
    _In_ HANDLE Handle)
{
    FILE_STANDARD_INFORMATION Info;

    ok_eq_hex(NtfsKmtQueryStandard(Handle, &Info), STATUS_SUCCESS);
    return Info;
}

static
VOID
TestUnalignedReads(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    ULONG Pass;

    /* Resident: 100 bytes. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"resident.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0, FILE_SHARE_READ,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = WritePattern(Handle, 0, 100, 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    for (Pass = 0; Pass < 2; Pass++)
    {
        ReadCheck(Handle, 3, 50, STATUS_SUCCESS, 50, 1, "resident middle");
        ReadCheck(Handle, 90, 50, STATUS_SUCCESS, 10, 1, "resident tail");
        ReadCheck(Handle, 99, 1, STATUS_SUCCESS, 1, 1, "resident last byte");
        ReadCheck(Handle, 100, 10, STATUS_END_OF_FILE, 0, 1, "resident at EOF");
        ReadCheck(Handle, 200, 10, STATUS_END_OF_FILE, 0, 1, "resident past EOF");
        /* Second pass: from disk, not from the write. */
        ZwClose(Handle);
        Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            return;
    }
    ZwClose(Handle);

    /* Non-resident: three clusters and a 123-byte tail. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"nonres.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0, FILE_SHARE_READ,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = WritePattern(Handle, 0, NONRES_SIZE, 2);
    ok_eq_hex(Status, STATUS_SUCCESS);
    for (Pass = 0; Pass < 2; Pass++)
    {
        ReadCheck(Handle, 12000, 1000, STATUS_SUCCESS, NONRES_SIZE - 12000, 2, "non-resident tail");
        ReadCheck(Handle, 4090, 20, STATUS_SUCCESS, 20, 2, "across a cluster");
        ReadCheck(Handle, 511, 2, STATUS_SUCCESS, 2, 2, "across a sector");
        ReadCheck(Handle, 1, NONRES_SIZE - 1, STATUS_SUCCESS, NONRES_SIZE - 1, 2, "all but the first byte");
        ReadCheck(Handle, NONRES_SIZE - 1, 4096, STATUS_SUCCESS, 1, 2, "last byte, big buffer");
        ReadCheck(Handle, NONRES_SIZE, 1, STATUS_END_OF_FILE, 0, 2, "non-resident at EOF");
        ZwClose(Handle);
        Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            return;
    }
    ZwClose(Handle);

    /* Non-cached: sector-aligned reads, the last one clipped at EOF. */
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                           FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PUCHAR Buffer = NtfsKmtAlloc(8192);
        ReadCheck(Handle, 4096, 4096, STATUS_SUCCESS, 4096, 2, "non-cached cluster");
        ReadCheck(Handle, 12288, 4096, STATUS_SUCCESS, NONRES_SIZE - 12288, 2, "non-cached tail");

        /* A non-cached aligned write is seen by a later cached read. */
        if (Buffer != NULL)
        {
            ULONG_PTR Written;
            NtfsKmtFillPattern(Buffer, 4096, 3, 4096);
            Status = NtfsKmtWrite(Handle, 4096, Buffer, 4096, &Written);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok_eq_ulongptr(Written, 4096);
            NtfsKmtFree(Buffer);
        }
        ZwClose(Handle);
        Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ReadCheck(Handle, 4096, 4096, STATUS_SUCCESS, 4096, 3, "cached after non-cached write");
            ReadCheck(Handle, 0, 4096, STATUS_SUCCESS, 4096, 2, "neighbour before non-cached write");
            ReadCheck(Handle, 8192, 4096, STATUS_SUCCESS, 4096, 2, "neighbour after non-cached write");
            ZwClose(Handle);
        }
    }
}

static
VOID
TestGrowAndTruncate(
    _In_ PCWSTR Dir)
{
    static const ULONG Steps[] = { 1, 100, 548, 700, 1000, 5000, 70000 };
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    ULONG i;
    ULONGLONG Size = 0;
    FILE_STANDARD_INFORMATION Info;
    FILE_END_OF_FILE_INFORMATION Eof;
    FILE_ALLOCATION_INFORMATION Alloc;
    PUCHAR Zero;
    ULONG_PTR Read;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"grow.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0, FILE_SHARE_READ,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Appends that walk the stream from resident to non-resident. */
    for (i = 0; i < RTL_NUMBER_OF(Steps); i++)
    {
        Status = WritePattern(Handle, Size, Steps[i], 4);
        ok(Status == STATUS_SUCCESS, "append of %lu at %I64u: 0x%lx\n", Steps[i], Size, Status);
        Size += Steps[i];
        Info = Standard(Handle);
        ok(Info.EndOfFile.QuadPart == (LONGLONG)Size, "EOF %I64d after append, expected %I64u\n",
           Info.EndOfFile.QuadPart, Size);
        ok(Info.AllocationSize.QuadPart >= Info.EndOfFile.QuadPart, "AllocationSize %I64d < EOF %I64d\n",
           Info.AllocationSize.QuadPart, Info.EndOfFile.QuadPart);
    }
    ReadCheck(Handle, 0, (ULONG)Size, STATUS_SUCCESS, (ULONG)Size, 4, "after growth");
    Info = Standard(Handle);
    ok(Info.AllocationSize.QuadPart % ClusterSize == 0, "non-resident AllocationSize %I64d not cluster aligned\n",
       Info.AllocationSize.QuadPart);

    /* Truncate to 100 bytes, extend to 9000: the new range reads as zero. */
    Eof.EndOfFile.QuadPart = 100;
    Status = NtfsKmtSetInfo(Handle, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(Standard(Handle).EndOfFile.QuadPart, 100LL);
    Eof.EndOfFile.QuadPart = 9000;
    Status = NtfsKmtSetInfo(Handle, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(Standard(Handle).EndOfFile.QuadPart, 9000LL);
    ReadCheck(Handle, 0, 100, STATUS_SUCCESS, 100, 4, "kept head after truncate");
    Zero = NtfsKmtAlloc(32 * KB);
    if (Zero != NULL)
    {
        Status = NtfsKmtRead(Handle, 100, Zero, 8900, &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == 8900 && NtfsKmtIsZero(Zero, 8900), "extended range not zero\n");

        /* A write past EOF leaves a zero gap. */
        Status = WritePattern(Handle, 20000, 10, 5);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_longlong(Standard(Handle).EndOfFile.QuadPart, 20010LL);
        Status = NtfsKmtRead(Handle, 9000, Zero, 11000, &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == 11000 && NtfsKmtIsZero(Zero, 11000), "gap before a past-EOF write not zero\n");
        ReadCheck(Handle, 20000, 10, STATUS_SUCCESS, 10, 5, "past-EOF write");
    }

    /* AllocationSize: growing keeps EOF, shrinking below EOF truncates. */
    Alloc.AllocationSize.QuadPart = 1024 * 1024;
    Status = NtfsKmtSetInfo(Handle, &Alloc, sizeof(Alloc), FileAllocationInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Info = Standard(Handle);
    ok_eq_longlong(Info.EndOfFile.QuadPart, 20010LL);
    ok(Info.AllocationSize.QuadPart >= 1024 * 1024, "AllocationSize %I64d after growing it\n",
       Info.AllocationSize.QuadPart);
    Alloc.AllocationSize.QuadPart = 8192;
    Status = NtfsKmtSetInfo(Handle, &Alloc, sizeof(Alloc), FileAllocationInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Info = Standard(Handle);
    ok_eq_longlong(Info.EndOfFile.QuadPart, 8192LL);
    ok_eq_longlong(Info.AllocationSize.QuadPart, 8192LL);
    ReadCheck(Handle, 0, 100, STATUS_SUCCESS, 100, 4, "head after allocation shrink");

    /* Above 4 GiB on a non-sparse file the volume is simply too small; a
     * 32-bit wrap of 4 GiB + 8 KiB would wrongly succeed as 8 KiB. */
    Eof.EndOfFile.QuadPart = 0x100000000LL + 8192;
    Status = NtfsKmtSetInfo(Handle, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_DISK_FULL);
    ok_eq_longlong(Standard(Handle).EndOfFile.QuadPart, 8192LL);
    if (Zero != NULL)
    {
        ULONG_PTR Written;
        Status = NtfsKmtWrite(Handle, 0x100000000ULL + 8192, Zero, 16, &Written);
        ok_eq_hex(Status, STATUS_DISK_FULL);
        ok_eq_longlong(Standard(Handle).EndOfFile.QuadPart, 8192LL);
        NtfsKmtFree(Zero);
    }
    ZwClose(Handle);
}

static
VOID
TestManyStreams(
    _In_ PCWSTR Dir)
{
    WCHAR Base[MAX_PATH];
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    ULONG i, Bad = 0, Count = 0;
    PUCHAR Buffer;
    PFILE_STREAM_INFORMATION Info;

    NtfsKmtJoin(Base, RTL_NUMBER_OF(Base), Dir, L"manystreams.bin");
    Status = NtfsKmtCreateFileWithData(Base, "base", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* 40 resident named streams of 200 bytes: far more than one 1 KiB file
     * record holds, so the attributes move behind an $ATTRIBUTE_LIST. */
    for (i = 0; i < STREAM_COUNT; i++)
    {
        RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:stream_number_%02lu", Base, i);
        Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_WRITE, 0, 0, FILE_CREATE,
                               FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
        if (NT_SUCCESS(Status))
        {
            Status = WritePattern(Handle, 0, 200, 100 + i);
            ZwClose(Handle);
        }
        if (!NT_SUCCESS(Status) && ++Bad <= 5)
            trace("stream %lu: 0x%lx\n", i, Status);
    }
    ok(Bad == 0, "%lu named streams could not be created\n", Bad);

    for (i = 0, Bad = 0; i < STREAM_COUNT; i++)
    {
        RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:stream_number_%02lu", Base, i);
        Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        if (!NT_SUCCESS(Status))
        {
            Bad++;
            continue;
        }
        {
            UCHAR Data[256];
            ULONG_PTR Read;
            Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
            if (!NT_SUCCESS(Status) || Read != 200 ||
                NtfsKmtCheckPattern(Data, 200, 100 + i, 0) != (SIZE_T)-1)
                Bad++;
        }
        ZwClose(Handle);
    }
    ok(Bad == 0, "%lu named streams lost or corrupted\n", Bad);

    /* The unnamed stream is still intact and every stream enumerates. */
    Buffer = NtfsKmtAlloc(16 * KB);
    Status = NtfsKmtOpen(&Handle, Base, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status) && Buffer != NULL)
    {
        ULONG_PTR Read;
        Status = NtfsKmtRead(Handle, 0, Buffer, 16, &Read);
        ok(NT_SUCCESS(Status) && Read == 4 && RtlCompareMemory(Buffer, "base", 4) == 4,
           "unnamed stream damaged by attribute-list growth\n");
        Status = NtfsKmtQueryInfo(Handle, Buffer, 16 * KB, FileStreamInformation, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Info = (PFILE_STREAM_INFORMATION)Buffer;
            for (;;)
            {
                Count++;
                if (Info->NextEntryOffset == 0 || Count > 1000)
                    break;
                Info = (PFILE_STREAM_INFORMATION)((PUCHAR)Info + Info->NextEntryOffset);
            }
            ok_eq_ulong(Count, (ULONG)STREAM_COUNT + 1);
        }
        ZwClose(Handle);
    }
    NtfsKmtFree(Buffer);

    Status = NtfsKmtDelete(Base);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static
VOID
TestManyLinks(
    _In_ PCWSTR Dir)
{
    WCHAR LinkDir[MAX_PATH];
    WCHAR Target[MAX_PATH];
    WCHAR Path[MAX_PATH];
    WCHAR Name[64];
    HANDLE Handle;
    NTSTATUS Status;
    ULONG i, Bad = 0;
    FILE_STANDARD_INFORMATION Info;

    NtfsKmtJoin(LinkDir, RTL_NUMBER_OF(LinkDir), Dir, L"manylinks");
    Status = NtfsKmtCreateDirectory(LinkDir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), LinkDir, L"target.bin");
    Status = NtfsKmtCreateFileWithData(Target, "linked", 6);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = NtfsKmtOpen(&Handle, Target, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    /* 80 long names: their $FILE_NAMEs fill several extension records and
     * the $ATTRIBUTE_LIST outgrows the base record (non-resident list). */
    for (i = 0; i < LINK_COUNT; i++)
    {
        RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"VeryLongHardLinkAliasNameNumber%03lu.dat", i);
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), LinkDir, Name);
        Status = NtfsKmtLink(Handle, Path, FALSE);
        if (skip(i != 0 || !NtfsKmtIsUnimplemented(Status), "FileLinkInformation not supported (0x%lx)\n", Status))
        {
            ZwClose(Handle);
            return;
        }
        if (!NT_SUCCESS(Status) && ++Bad <= 5)
            trace("link %lu: 0x%lx\n", i, Status);
    }
    ok(Bad == 0, "%lu links failed\n", Bad);
    Info = Standard(Handle);
    ok_eq_ulong(Info.NumberOfLinks, (ULONG)LINK_COUNT + 1);
    ZwClose(Handle);

    /* Every name opens the same data; then all but one are deleted. */
    for (i = 0, Bad = 0; i < LINK_COUNT; i++)
    {
        RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"VeryLongHardLinkAliasNameNumber%03lu.dat", i);
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), LinkDir, Name);
        Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        if (NT_SUCCESS(Status))
        {
            UCHAR Data[16];
            ULONG_PTR Read;
            Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
            if (!NT_SUCCESS(Status) || Read != 6 || RtlCompareMemory(Data, "linked", 6) != 6)
                Status = STATUS_DATA_ERROR;
            ZwClose(Handle);
        }
        if (!NT_SUCCESS(Status))
            Bad++;
    }
    ok(Bad == 0, "%lu link names did not open or read wrong data\n", Bad);

    for (i = 0, Bad = 0; i < LINK_COUNT; i++)
    {
        RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"VeryLongHardLinkAliasNameNumber%03lu.dat", i);
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), LinkDir, Name);
        if (!NT_SUCCESS(NtfsKmtDelete(Path)))
            Bad++;
    }
    ok(Bad == 0, "%lu link deletes failed\n", Bad);
    Status = NtfsKmtOpen(&Handle, Target, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Info = Standard(Handle);
        ok_eq_ulong(Info.NumberOfLinks, 1UL);
        ZwClose(Handle);
    }
}

START_TEST(NtfsDataPath)
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

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_datapath", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    if (NT_SUCCESS(NtfsKmtOpen(&Handle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE)))
    {
        if (NT_SUCCESS(ZwQueryVolumeInformationFile(Handle, &IoStatus, &Size, sizeof(Size),
                                                    FileFsSizeInformation)))
            ClusterSize = Size.SectorsPerAllocationUnit * Size.BytesPerSector;
        ZwClose(Handle);
    }

    TestUnalignedReads(Dir);
    TestGrowAndTruncate(Dir);
    TestManyStreams(Dir);
    TestManyLinks(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
