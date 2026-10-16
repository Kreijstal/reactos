/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS LZNT1 file compression
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box compression coverage: FSCTL_GET/SET_COMPRESSION, multi-unit
 * compressed writes and reads (compressible and incompressible units),
 * unaligned reads across compression-unit boundaries, in-place rewrite,
 * truncation, FileCompressionInformation, a hole in a compressed file,
 * decompression, and inheritance from a compressed directory.
 *
 * Replaces the host tests test_lznt1_codec, test_compressed_read,
 * test_compressed_write and test_compressed_multiunit, which drove the LZNT1
 * codec and the compressed-unit reader inside the driver.
 *
 * Windows reference: FSCTL_SET_COMPRESSION / FSCTL_GET_COMPRESSION docs,
 * MS-FSCC 2.4.9 FILE_COMPRESSION_INFORMATION. NTFS compression units are
 * 16 clusters; with 4 KiB clusters a unit is 64 KiB.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define KB              1024UL
#define UNIT            (64 * KB)
#define FILE_SIZE       (5 * UNIT + 1234)

/* Unit layout of the test file: text (compresses), pattern (does not),
 * zeros (compresses to nothing), text, pattern, and a partial text tail. */
static
VOID
BuildContent(
    _Out_writes_bytes_(FILE_SIZE) PUCHAR Buffer)
{
    static const CHAR Text[] = "ReactOS NTFS compressed unit test - WineWineWine ROS!ROS! ";
    ULONG i;

    for (i = 0; i < FILE_SIZE; i++)
    {
        ULONG Unit = i / UNIT;
        switch (Unit)
        {
            case 1:
            case 4:
                break; /* pattern below */
            case 2:
                Buffer[i] = 0;
                break;
            default:
                Buffer[i] = (UCHAR)Text[i % (sizeof(Text) - 1)];
                break;
        }
    }
    NtfsKmtFillPattern(Buffer + 1 * UNIT, UNIT, 21, 1 * UNIT);
    NtfsKmtFillPattern(Buffer + 4 * UNIT, UNIT, 21, 4 * UNIT);
}

static
USHORT
GetCompression(
    _In_ HANDLE Handle,
    _Out_ PNTSTATUS Status)
{
    USHORT Format = 0xFFFF;
    ULONG_PTR Info = 0;

    *Status = NtfsKmtFsctl(Handle, FSCTL_GET_COMPRESSION, NULL, 0, &Format, sizeof(Format), &Info);
    if (NT_SUCCESS(*Status))
        ok_eq_ulongptr(Info, sizeof(USHORT));
    return Format;
}

static
NTSTATUS
SetCompression(
    _In_ HANDLE Handle,
    _In_ USHORT Format)
{
    return NtfsKmtFsctl(Handle, FSCTL_SET_COMPRESSION, &Format, sizeof(Format), NULL, 0, NULL);
}

static
ULONG
QueryAttributes(
    _In_ HANDLE Handle)
{
    FILE_BASIC_INFORMATION Basic;

    if (!NT_SUCCESS(NtfsKmtQueryInfo(Handle, &Basic, sizeof(Basic), FileBasicInformation, NULL)))
        return 0;
    return Basic.FileAttributes;
}

static
VOID
CompareRange(
    _In_ HANDLE Handle,
    _In_ const UCHAR *Expected,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ ULONG ExpectedRead,
    _In_ PCSTR What)
{
    PUCHAR Buffer = NtfsKmtAlloc(Length);
    ULONG_PTR Read = 0;
    NTSTATUS Status;

    if (Buffer == NULL)
        return;
    Status = NtfsKmtRead(Handle, Offset, Buffer, Length, &Read);
    ok(Status == STATUS_SUCCESS, "%s: read status 0x%lx\n", What, Status);
    ok(Read == ExpectedRead, "%s: read %Iu bytes, expected %lu\n", What, Read, ExpectedRead);
    ok(RtlCompareMemory(Buffer, Expected + Offset, min(Read, ExpectedRead)) == min(Read, ExpectedRead),
       "%s: data mismatch\n", What);
    NtfsKmtFree(Buffer);
}

static
VOID
TestCompressedFile(
    _In_ PCWSTR Dir,
    _In_ ULONG ClusterShift)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    USHORT Format;
    PUCHAR Content;
    ULONG_PTR Written;
    FILE_COMPRESSION_INFORMATION Compression;
    FILE_STANDARD_INFORMATION Standard;
    FILE_END_OF_FILE_INFORMATION Eof;

    Content = NtfsKmtAlloc(FILE_SIZE);
    if (Content == NULL)
        return;
    BuildContent(Content);

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"lznt1.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ, FILE_CREATE, FILE_NON_DIRECTORY_FILE,
                           NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtfsKmtFree(Content);
        return;
    }

    /* A new file in an uncompressed directory is not compressed. */
    Format = GetCompression(Handle, &Status);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_GET_COMPRESSION not supported (0x%lx)\n", Status))
        goto Close;
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_uint(Format, COMPRESSION_FORMAT_NONE);

    Status = SetCompression(Handle, COMPRESSION_FORMAT_DEFAULT);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_SET_COMPRESSION not supported (0x%lx)\n", Status))
        goto Close;
    ok_eq_hex(Status, STATUS_SUCCESS);
    Format = GetCompression(Handle, &Status);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_uint(Format, COMPRESSION_FORMAT_LZNT1);
    ok(QueryAttributes(Handle) & FILE_ATTRIBUTE_COMPRESSED, "FILE_ATTRIBUTE_COMPRESSED missing\n");

    /* An unknown format is rejected. */
    Status = SetCompression(Handle, 0x55);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* Multi-unit write, then flush so the units are compressed on disk. */
    Status = NtfsKmtWrite(Handle, 0, Content, FILE_SIZE, &Written);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Written, FILE_SIZE);
    Status = NtfsKmtFlush(Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Handle);

    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtfsKmtFree(Content);
        return;
    }

    CompareRange(Handle, Content, 0, FILE_SIZE, FILE_SIZE, "whole file");
    CompareRange(Handle, Content, UNIT - 6, 20, 20, "across unit 0/1");
    CompareRange(Handle, Content, 2 * UNIT - 3, 7, 7, "across unit 1/2");
    CompareRange(Handle, Content, 3 * UNIT - 4097, 8192, 8192, "across unit 2/3");
    CompareRange(Handle, Content, 5 * UNIT + 1000, 4096, 234, "tail past EOF");

    Status = NtfsKmtQueryInfo(Handle, &Compression, sizeof(Compression),
                              FileCompressionInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_uint(Compression.CompressionFormat, COMPRESSION_FORMAT_LZNT1);
        ok_eq_uint(Compression.CompressionUnitShift, 16);
        ok_eq_uint(Compression.ChunkShift, 12);
        ok_eq_uint(Compression.ClusterShift, ClusterShift);
        /* Three of six units compress (two text, one zero), two do not. */
        ok(Compression.CompressedFileSize.QuadPart < (LONGLONG)FILE_SIZE,
           "CompressedFileSize %I64d not below %lu\n",
           Compression.CompressedFileSize.QuadPart, FILE_SIZE);
        ok(Compression.CompressedFileSize.QuadPart >= (LONGLONG)(2 * UNIT),
           "CompressedFileSize %I64d below the incompressible units\n",
           Compression.CompressedFileSize.QuadPart);
    }
    Status = NtfsKmtQueryStandard(Handle, &Standard);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(Standard.EndOfFile.QuadPart, (LONGLONG)FILE_SIZE);

    /* Rewrite a range that spans a compressed and an incompressible unit. */
    NtfsKmtFillPattern(Content + UNIT - 500, 1000, 23, UNIT - 500);
    Status = NtfsKmtWrite(Handle, UNIT - 500, Content + UNIT - 500, 1000, &Written);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtFlush(Handle);
    CompareRange(Handle, Content, 0, FILE_SIZE, FILE_SIZE, "after rewrite");

    /* Truncate into the middle of a unit. */
    Eof.EndOfFile.QuadPart = 3 * UNIT + 100;
    Status = NtfsKmtSetInfo(Handle, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    CompareRange(Handle, Content, 0, 4 * UNIT, 3 * UNIT + 100, "after truncate");

    /* A write far past EOF leaves a zero gap (sparse inside a compressed
     * file, test_compressed_multiunit's sparse+compressed case). */
    Status = NtfsKmtWrite(Handle, 10 * UNIT, Content, 4096, &Written);
    ok_eq_hex(Status, STATUS_SUCCESS);
    {
        PUCHAR Gap = NtfsKmtAlloc(UNIT);
        ULONG_PTR Read;
        if (Gap != NULL)
        {
            Status = NtfsKmtRead(Handle, 6 * UNIT, Gap, UNIT, &Read);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(Read == UNIT && NtfsKmtIsZero(Gap, UNIT), "gap in compressed file not zero\n");
            Status = NtfsKmtRead(Handle, 10 * UNIT, Gap, 4096, &Read);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(Read == 4096 && RtlCompareMemory(Gap, Content, 4096) == 4096,
               "data past the gap mismatch\n");
            NtfsKmtFree(Gap);
        }
    }

    /* Decompress in place: data intact, attribute cleared. */
    Status = SetCompression(Handle, COMPRESSION_FORMAT_NONE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Format = GetCompression(Handle, &Status);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_uint(Format, COMPRESSION_FORMAT_NONE);
    ok(!(QueryAttributes(Handle) & FILE_ATTRIBUTE_COMPRESSED), "FILE_ATTRIBUTE_COMPRESSED still set\n");
    CompareRange(Handle, Content, 0, 3 * UNIT + 100, 3 * UNIT + 100, "after decompress");

Close:
    ZwClose(Handle);
    NtfsKmtFree(Content);
}

static
VOID
TestCompressedDirectory(
    _In_ PCWSTR Dir)
{
    WCHAR SubDir[MAX_PATH];
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    USHORT Format;

    NtfsKmtJoin(SubDir, RTL_NUMBER_OF(SubDir), Dir, L"cdir");
    Status = NtfsKmtCreateDirectory(SubDir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Handle, SubDir, FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                         FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = SetCompression(Handle, COMPRESSION_FORMAT_DEFAULT);
    ZwClose(Handle);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_SET_COMPRESSION not supported (0x%lx)\n", Status))
        return;
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* New files inherit the directory's compression state. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), SubDir, L"inherit.txt");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0, 0,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(QueryAttributes(Handle) & FILE_ATTRIBUTE_COMPRESSED, "new file did not inherit compression\n");
        Format = GetCompression(Handle, &Status);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_uint(Format, COMPRESSION_FORMAT_LZNT1);
        ZwClose(Handle);
    }
}

START_TEST(NtfsCompression)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;
    HANDLE Handle;
    FILE_FS_SIZE_INFORMATION Size;
    IO_STATUS_BLOCK IoStatus;
    ULONG ClusterShift = 12;

    if (!NtfsKmtFindVolume(Root, NULL))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_compress", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    if (NT_SUCCESS(NtfsKmtOpen(&Handle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE)))
    {
        if (NT_SUCCESS(ZwQueryVolumeInformationFile(Handle, &IoStatus, &Size, sizeof(Size),
                                                    FileFsSizeInformation)))
        {
            ULONG Cluster = Size.SectorsPerAllocationUnit * Size.BytesPerSector;
            for (ClusterShift = 0; (1UL << ClusterShift) < Cluster; ClusterShift++);
        }
        ZwClose(Handle);
    }

    TestCompressedFile(Dir, ClusterShift);
    TestCompressedDirectory(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
