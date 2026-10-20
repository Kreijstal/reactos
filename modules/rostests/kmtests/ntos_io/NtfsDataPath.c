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
 *  - setup's copy pattern: a non-cached page-rounded write, shrunk back
 *    to the real size, read back cached and after a remount;
 *  - setup's cabinet extraction: a file grown by a data section and filled
 *    through a mapped view, whose partial last page is a clamped paging write;
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

/* Exercise a transfer larger than a VACB and an overwrite crossing its edge.
 * Reopen through cached and noncached handles after an explicit flush. */
static
VOID
TestCacheBoundaries(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    ULONG Pass;
    const ULONG Size = 3 * 256 * KB + 123;
    const ULONG Edge = 256 * KB - 513;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"cache-boundary.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = WritePattern(Handle, 0, Size, 20);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ReadCheck(Handle, 0, Size, STATUS_SUCCESS, Size, 20, "large cached transfer");
    Status = WritePattern(Handle, Edge, 4096, 21);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ReadCheck(Handle, Edge, 4096, STATUS_SUCCESS, 4096, 21, "overwrite across cache boundary");
    Status = NtfsKmtFlush(Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Handle);

    for (Pass = 0; Pass < 2; Pass++)
    {
        Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                               FILE_NON_DIRECTORY_FILE | (Pass ? FILE_NO_INTERMEDIATE_BUFFERING : 0),
                               NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            continue;
        ReadCheck(Handle, 0, 4096, STATUS_SUCCESS, 4096, 20, "head after cache flush");
        ReadCheck(Handle, 256 * KB, 3072, STATUS_SUCCESS, 3072, 21, "overwritten range after cache flush");
        ReadCheck(Handle, 512 * KB, 4096, STATUS_SUCCESS, 4096, 20, "next cache view after flush");
        ZwClose(Handle);
    }
}

/* A mapped section keeps its stream alive after the originating handle closes.
 * Fault the pages only after close, churn other FCBs, then unmap and reuse the
 * same name. No driver internals are inspected. */
static
VOID
TestMappedLifetime(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH], Other[MAX_PATH];
    HANDLE Handle, Section;
    OBJECT_ATTRIBUTES Attributes;
    PVOID View = NULL;
    SIZE_T ViewSize = 0;
    NTSTATUS Status;
    ULONG Index;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"mapped.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = WritePattern(Handle, 0, 8192, 30);
    ok_eq_hex(Status, STATUS_SUCCESS);
    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateSection(&Section, SECTION_MAP_READ | SECTION_MAP_WRITE, &Attributes, NULL,
                              PAGE_READWRITE, SEC_COMMIT, Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(Handle);
        return;
    }
    Status = ZwMapViewOfSection(Section, NtCurrentProcess(), &View, 0, 0, NULL,
                                 &ViewSize, ViewUnmap, 0, PAGE_READWRITE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Handle);
    if (NT_SUCCESS(Status))
    {
        ok(NtfsKmtCheckPattern(View, 8192, 30, 0) == (SIZE_T)-1,
           "mapped data changed after closing its file handle\n");
        /* A normal read must see a dirty mapped write, including when its
         * explicit flush is entered with the file's resource held shared. */
        {
            UCHAR Original = ((PUCHAR)View)[4097];
            UCHAR Data = 0;
            ULONG_PTR Read;

            ((PUCHAR)View)[4097] = Original ^ 0x7F;
            Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                Status = NtfsKmtRead(Handle, 4097, &Data, 1, &Read);
                ok_eq_hex(Status, STATUS_SUCCESS);
                ok(Read == 1 && Data == (UCHAR)(Original ^ 0x7F), "read missed a mapped write\n");
                ((PUCHAR)View)[4097] = Original;
                Status = NtfsKmtRead(Handle, 4097, &Data, 1, &Read);
                ok_eq_hex(Status, STATUS_SUCCESS);
                ok(Read == 1 && Data == Original, "read missed a second mapped write\n");
                ZwClose(Handle);
            }
        }
        NtfsKmtJoin(Other, RTL_NUMBER_OF(Other), Dir, L"map-churn.bin");
        for (Index = 0; Index < 32; Index++)
        {
            Status = NtfsKmtCreateFileWithData(Other, "churn", 5);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (!NT_SUCCESS(Status))
                break;
            Status = NtfsKmtDelete(Other);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
        ok(NtfsKmtCheckPattern(View, 8192, 30, 0) == (SIZE_T)-1,
           "mapped data changed during FCB churn\n");
        Status = ZwUnmapViewOfSection(NtCurrentProcess(), View);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ZwClose(Section);
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Path, "new file", 8);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        UCHAR Data[8];
        ULONG_PTR Read;

        Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == sizeof(Data) && RtlCompareMemory(Data, "new file", sizeof(Data)) == sizeof(Data),
           "reused name returned data from the former section\n");
        ZwClose(Handle);
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

/* Setup's file copy: a non-cached write of the page-rounded source size,
 * then FileEndOfFileInformation back to the real size.  The data must be on
 * disk, so it is checked again after a dismount forces a fresh mount. */
static
VOID
TestNonCachedWriteThenShrink(
    _In_ PCWSTR Dir,
    _In_ WCHAR Drive)
{
    static const ULONG Sizes[] = { 67, 700, 3620, 4095, 4096, 4258, 6088 };
    WCHAR Path[MAX_PATH], Name[32];
    FILE_END_OF_FILE_INFORMATION Eof;
    HANDLE Handle, Volume;
    PUCHAR Buffer;
    ULONG_PTR Written;
    NTSTATUS Status;
    ULONG i, Pass, Rounded;

    Buffer = NtfsKmtAlloc(2 * PAGE_SIZE);
    if (Buffer == NULL)
        return;
    ok(((ULONG_PTR)Buffer & (PAGE_SIZE - 1)) == 0, "Buffer %p not page aligned\n", Buffer);
    for (i = 0; i < RTL_NUMBER_OF(Sizes); i++)
    {
        RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"nocache%lu.bin", Sizes[i]);
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, Name);
        Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0, 0, FILE_CREATE,
                               FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING | FILE_SEQUENTIAL_ONLY,
                               NULL, 0, NULL);
        ok(Status == STATUS_SUCCESS, "create %lu: 0x%lx\n", Sizes[i], Status);
        if (!NT_SUCCESS(Status))
            continue;
        Rounded = ROUND_TO_PAGES(Sizes[i]);
        NtfsKmtFillPattern(Buffer, Rounded, 9, 0);
        Status = NtfsKmtWrite(Handle, 0, Buffer, Rounded, &Written);
        ok(Status == STATUS_SUCCESS && Written == Rounded, "non-cached write of %lu: 0x%lx, %Iu\n",
           Rounded, Status, Written);
        Eof.EndOfFile.QuadPart = Sizes[i];
        Status = NtfsKmtSetInfo(Handle, &Eof, sizeof(Eof), FileEndOfFileInformation);
        ok(Status == STATUS_SUCCESS, "shrink to %lu: 0x%lx\n", Sizes[i], Status);
        ZwClose(Handle);
    }
    NtfsKmtFree(Buffer);

    for (Pass = 0; Pass < 2; Pass++)
    {
        if (Pass == 1)
        {
            Status = NtfsKmtOpenVolume(Drive, GENERIC_READ | GENERIC_WRITE, &Volume);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (!NT_SUCCESS(Status))
                return;
            Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                Status = NtfsKmtFsctl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL);
                ok_eq_hex(Status, STATUS_SUCCESS);
            }
            ZwClose(Volume);
            if (!NT_SUCCESS(Status))
                return;
        }
        for (i = 0; i < RTL_NUMBER_OF(Sizes); i++)
        {
            CHAR What[64];

            RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"nocache%lu.bin", Sizes[i]);
            NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, Name);
            Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
            ok(Status == STATUS_SUCCESS, "reopen %lu: 0x%lx\n", Sizes[i], Status);
            if (!NT_SUCCESS(Status))
                continue;
            ok_eq_longlong(Standard(Handle).EndOfFile.QuadPart, (LONGLONG)Sizes[i]);
            RtlStringCchPrintfA(What, sizeof(What), "non-cached %lu %s", Sizes[i], Pass ? "after remount" : "reopened");
            ReadCheck(Handle, 0, Sizes[i], STATUS_SUCCESS, Sizes[i], 9, What);
            ZwClose(Handle);
        }
    }
}

/* Setup's cabinet extraction: an empty file grown by a data section of the
 * real size, filled through a mapped view, then unmapped and closed.  The
 * last, partial page reaches disk only as a paging write clamped to EOF. */
static
VOID
TestSectionExtendedFiles(
    _In_ PCWSTR Dir,
    _In_ WCHAR Drive)
{
    static const ULONG Sizes[] = { 67, 700, 3620, 4096, 6088, 9000 };
    WCHAR Path[MAX_PATH], Name[32];
    OBJECT_ATTRIBUTES Attributes;
    LARGE_INTEGER MaxSize;
    HANDLE Handle, Section, Volume;
    PVOID View;
    SIZE_T ViewSize;
    NTSTATUS Status;
    ULONG i, Pass;

    for (i = 0; i < RTL_NUMBER_OF(Sizes); i++)
    {
        RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"section%lu.bin", Sizes[i]);
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, Name);
        Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0, 0, FILE_CREATE,
                               FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
        ok(Status == STATUS_SUCCESS, "create %lu: 0x%lx\n", Sizes[i], Status);
        if (!NT_SUCCESS(Status))
            continue;
        MaxSize.QuadPart = Sizes[i];
        InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = ZwCreateSection(&Section, SECTION_ALL_ACCESS, &Attributes, &MaxSize,
                                 PAGE_READWRITE, SEC_COMMIT, Handle);
        ok(Status == STATUS_SUCCESS, "section %lu: 0x%lx\n", Sizes[i], Status);
        if (NT_SUCCESS(Status))
        {
            View = NULL;
            ViewSize = 0;
            Status = ZwMapViewOfSection(Section, NtCurrentProcess(), &View, 0, 0, NULL,
                                        &ViewSize, ViewUnmap, 0, PAGE_READWRITE);
            ok(Status == STATUS_SUCCESS, "map %lu: 0x%lx\n", Sizes[i], Status);
            if (NT_SUCCESS(Status))
            {
                NtfsKmtFillPattern(View, Sizes[i], 11, 0);
                ZwUnmapViewOfSection(NtCurrentProcess(), View);
            }
            ZwClose(Section);
        }
        ZwClose(Handle);
    }

    for (Pass = 0; Pass < 2; Pass++)
    {
        if (Pass == 1)
        {
            Status = NtfsKmtOpenVolume(Drive, GENERIC_READ | GENERIC_WRITE, &Volume);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (!NT_SUCCESS(Status))
                return;
            Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                Status = NtfsKmtFsctl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL);
                ok_eq_hex(Status, STATUS_SUCCESS);
            }
            ZwClose(Volume);
            if (!NT_SUCCESS(Status))
                return;
        }
        for (i = 0; i < RTL_NUMBER_OF(Sizes); i++)
        {
            CHAR What[64];

            RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"section%lu.bin", Sizes[i]);
            NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, Name);
            Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
            ok(Status == STATUS_SUCCESS, "reopen %lu: 0x%lx\n", Sizes[i], Status);
            if (!NT_SUCCESS(Status))
                continue;
            ok_eq_longlong(Standard(Handle).EndOfFile.QuadPart, (LONGLONG)Sizes[i]);
            RtlStringCchPrintfA(What, sizeof(What), "section-extended %lu %s", Sizes[i],
                                Pass ? "after remount" : "reopened");
            ReadCheck(Handle, 0, Sizes[i], STATUS_SUCCESS, Sizes[i], 11, What);
            ZwClose(Handle);
        }
    }
}

/* NtCreatePagingFile's own create: supersede as a paging file, deleted on
 * close, with no DELETE access and the I/O manager's parameter check
 * skipped, then size it through FileEndOfFileInformation. */
static
VOID
TestPagingFileCreate(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER AllocationSize;
    FILE_END_OF_FILE_INFORMATION Eof;
    HANDLE Handle;
    NTSTATUS Status;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"pagefile.sys");
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    AllocationSize.QuadPart = 16 * 1024 * 1024 + PAGE_SIZE;
    Status = IoCreateFile(&Handle,
                          SYNCHRONIZE | WRITE_DAC | FILE_READ_DATA | FILE_WRITE_DATA,
                          &Attributes,
                          &IoStatus,
                          &AllocationSize,
                          FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN,
                          FILE_SHARE_WRITE,
                          FILE_SUPERSEDE,
                          FILE_DELETE_ON_CLOSE | FILE_NO_COMPRESSION | FILE_NO_INTERMEDIATE_BUFFERING,
                          NULL,
                          0,
                          CreateFileTypeNone,
                          NULL,
                          IO_OPEN_PAGING_FILE | IO_NO_PARAMETER_CHECKING);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Eof.EndOfFile.QuadPart = 16 * 1024 * 1024;
    Status = ZwSetInformationFile(Handle, &IoStatus, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(Standard(Handle).EndOfFile.QuadPart, 16LL * 1024 * 1024);
    ZwClose(Handle);
}

/* A directory has no data stream: reading or writing its handle is an
 * invalid request, not a byte-range lock check. */
static
VOID
TestDirectoryReadWrite(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    UCHAR Data[16] = { 0 };
    ULONG_PTR Bytes;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"subdir");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
                           FILE_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Bytes);
    ok_eq_hex(Status, STATUS_INVALID_DEVICE_REQUEST);
    Status = NtfsKmtWrite(Handle, 0, Data, sizeof(Data), &Bytes);
    ok_eq_hex(Status, STATUS_INVALID_DEVICE_REQUEST);
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
    IO_STATUS_BLOCK IoStatus;
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;
    HANDLE Handle;
    FILE_FS_SIZE_INFORMATION Size;
    WCHAR Drive;

    if (!NtfsKmtFindVolume(Root, &Drive))
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
    TestCacheBoundaries(Dir);
    TestMappedLifetime(Dir);
    TestGrowAndTruncate(Dir);
    TestNonCachedWriteThenShrink(Dir, Drive);
    TestSectionExtendedFiles(Dir, Drive);
    TestPagingFileCreate(Dir);
    TestDirectoryReadWrite(Dir);
    TestManyStreams(Dir);
    TestManyLinks(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
