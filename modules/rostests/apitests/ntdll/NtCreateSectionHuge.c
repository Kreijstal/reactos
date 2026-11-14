/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test huge pagefile-backed SEC_RESERVE sections (CoreCLR W^X double mapping)
 */

#include "precomp.h"

#define VIEW_SIZE 0x10000

/* ReactOS still materializes the prototype PTEs of a pagefile-backed section
 * up front, so the largest sections only fit when paged pool is big enough
 * (about 8 GB of RAM for 2 TB). Windows creates them on any machine. */
static
BOOLEAN
IsOutOfPrototypePtes(NTSTATUS Status)
{
    return (Status == STATUS_SECTION_TOO_BIG) ||
           (Status == STATUS_INSUFFICIENT_RESOURCES);
}

static
NTSTATUS
CreateReserveSection(PHANDLE Section, ULONGLONG Size)
{
    LARGE_INTEGER MaximumSize;

    MaximumSize.QuadPart = Size;
    *Section = NULL;
    return NtCreateSection(Section,
                           SECTION_ALL_ACCESS,
                           NULL,
                           &MaximumSize,
                           PAGE_EXECUTE_READWRITE,
                           SEC_RESERVE,
                           NULL);
}

static
PUCHAR
MapAndCommit(HANDLE Section, ULONGLONG Offset, ULONG Protect)
{
    NTSTATUS Status;
    PVOID Base = NULL;
    SIZE_T ViewSize = VIEW_SIZE;
    SIZE_T CommitSize = 2 * PAGE_SIZE;
    LARGE_INTEGER SectionOffset;

    SectionOffset.QuadPart = Offset;
    Status = NtMapViewOfSection(Section, NtCurrentProcess(), &Base, 0, 0, &SectionOffset,
                                &ViewSize, ViewUnmap, 0, Protect);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &CommitSize,
                                     MEM_COMMIT, Protect);
    ok_ntstatus(Status, STATUS_SUCCESS);
    return Base;
}

static
BOOLEAN
ReadByte(volatile UCHAR *Address, PUCHAR Value)
{
    BOOLEAN Ok = TRUE;
    _SEH2_TRY
    {
        *Value = *Address;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Ok = FALSE;
    }
    _SEH2_END;
    return Ok;
}

/* Map a writable and an executable view, commit through both, and check that
 * they alias the same pages -- exactly what CoreCLR's ExecutableAllocator does */
static
VOID
TestDoubleMapping(ULONGLONG Size)
{
    NTSTATUS Status;
    HANDLE Section;
    PUCHAR Rw, Rx;
    UCHAR Value = 0;

    Status = CreateReserveSection(&Section, Size);
    if (IsOutOfPrototypePtes(Status))
    {
        skip("Section of 0x%I64x bytes needs more paged pool (0x%lx)\n", Size, Status);
        return;
    }
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Rx = MapAndCommit(Section, 0, PAGE_EXECUTE_READ);
    Rw = MapAndCommit(Section, 0, PAGE_READWRITE);
    if (Rx && Rw)
    {
        Rw[PAGE_SIZE] = 0xC3;
        ok(ReadByte(&Rx[PAGE_SIZE], &Value), "Reading the RX view faulted\n");
        ok(Value == 0xC3, "Size 0x%I64x: RX view reads 0x%x\n", Size, Value);
    }

    if (Rw) NtUnmapViewOfSection(NtCurrentProcess(), Rw);
    if (Rx) NtUnmapViewOfSection(NtCurrentProcess(), Rx);
    NtClose(Section);
}

/* The prototype PTEs of a 2 TB section take 4 GB of paged pool, which pushes
 * the next section's prototype PTEs more than 4 GB into paged pool. Creating
 * that second section must not disturb the first one's committed pages. The
 * pages are committed 1 GB into the first section, so their prototype PTEs
 * live 2 MB into its prototype PTE array rather than next to its header. */
#define FIRST_OFFSET 0x40000000ULL
static
VOID
TestSecondSectionAboveFourGigabytes(VOID)
{
    NTSTATUS Status;
    HANDLE First, Second;
    PUCHAR View, Again, SecondView;
    UCHAR Value = 0;

    Status = CreateReserveSection(&First, 0x20000000000ULL);
    if (IsOutOfPrototypePtes(Status))
    {
        skip("2 TB section needs more paged pool (0x%lx)\n", Status);
        return;
    }
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    View = MapAndCommit(First, FIRST_OFFSET, PAGE_READWRITE);
    if (!View)
    {
        NtClose(First);
        return;
    }
    View[0] = 0x5A;
    View[PAGE_SIZE] = 0xA5;

    Status = CreateReserveSection(&Second, 0x8000000000ULL);
    if (IsOutOfPrototypePtes(Status))
    {
        skip("512 GB section next to a 2 TB one needs more paged pool (0x%lx)\n", Status);
    }
    else
    {
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            /* A fresh view resolves through the first section's prototype PTEs */
            Again = NULL;
            {
                SIZE_T ViewSize = VIEW_SIZE;
                LARGE_INTEGER SectionOffset;
                SectionOffset.QuadPart = FIRST_OFFSET;
                Status = NtMapViewOfSection(First, NtCurrentProcess(), (PVOID*)&Again, 0, 0, &SectionOffset,
                                            &ViewSize, ViewUnmap, 0, PAGE_READWRITE);
                ok_ntstatus(Status, STATUS_SUCCESS);
            }
            if (Again)
            {
                ok(ReadByte(&Again[0], &Value), "First section page 0 is gone\n");
                ok(Value == 0x5A, "First section page 0 reads 0x%x\n", Value);
                Value = 0;
                ok(ReadByte(&Again[PAGE_SIZE], &Value), "First section page 1 is gone\n");
                ok(Value == 0xA5, "First section page 1 reads 0x%x\n", Value);
                NtUnmapViewOfSection(NtCurrentProcess(), Again);
            }

            SecondView = MapAndCommit(Second, 0, PAGE_READWRITE);
            if (SecondView)
            {
                SecondView[0] = 0x3C;
                ok(SecondView[0] == 0x3C, "Second section does not hold data\n");
                NtUnmapViewOfSection(NtCurrentProcess(), SecondView);
            }
            NtClose(Second);
        }
    }

    NtUnmapViewOfSection(NtCurrentProcess(), View);
    NtClose(First);
}

START_TEST(NtCreateSectionHuge)
{
    /* First, while paged pool has no multi-GB holes yet */
    TestSecondSectionAboveFourGigabytes();
    TestDoubleMapping(0x40000000ULL);      /* 1 GB */
    TestDoubleMapping(0x1000000000ULL);    /* 64 GB */
    TestDoubleMapping(0x10000000000ULL);   /* 1 TB */
    TestDoubleMapping(0x20000000000ULL);   /* 2 TB: CoreCLR's MaxDoubleMappedSize */
}
