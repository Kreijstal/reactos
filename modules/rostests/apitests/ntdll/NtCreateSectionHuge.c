/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test huge pagefile-backed SEC_RESERVE sections (CoreCLR W^X double mapping)
 */

#include "precomp.h"

#define VIEW_SIZE 0x10000

/* The prototype PTEs of a pagefile-backed section take paged pool address
 * space, so on ReactOS the largest sections only fit when paged pool is big
 * enough (about 8 GB of RAM for 2 TB). Windows creates them on any machine. */
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

static
BOOLEAN
QueryPerformance(PSYSTEM_PERFORMANCE_INFORMATION Info)
{
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemPerformanceInformation, Info, sizeof(*Info), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status);
}

static
VOID
CheckState(PVOID Address, ULONG ExpectedState, SIZE_T ExpectedSize)
{
    NTSTATUS Status;
    MEMORY_BASIC_INFORMATION Info;

    Status = NtQueryVirtualMemory(NtCurrentProcess(), Address, MemoryBasicInformation,
                                  &Info, sizeof(Info), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    ok(Info.State == ExpectedState, "%p: State 0x%lx, expected 0x%lx\n", Address, Info.State, ExpectedState);
    ok(Info.RegionSize == ExpectedSize, "%p: RegionSize 0x%Ix, expected 0x%Ix\n",
       Address, Info.RegionSize, ExpectedSize);
}

/* Reserving pages commits nothing: Windows creates a 2 TB SEC_RESERVE section
 * (CoreCLR's W^X double mapping, in every .NET process) without consuming
 * memory or commit, and charges commit only for pages committed in a view. */
#define RESERVE_SIZE  0x20000000000ULL     /* 2 TB */
#define COMMIT_OFFSET 0x10000000000ULL     /* 1 TB in */
#define COMMIT_PAGES  256
#define COMMIT_VIEW_SIZE (4 * COMMIT_PAGES * PAGE_SIZE)
#define MB_IN_PAGES(Mb) ((Mb) * (0x100000 / PAGE_SIZE))
static
VOID
TestReserveConsumesNothing(VOID)
{
    NTSTATUS Status;
    HANDLE Section;
    SYSTEM_PERFORMANCE_INFORMATION Base, Before, After;
    LARGE_INTEGER Start, End, Frequency, SectionOffset;
    ULONGLONG Milliseconds;
    PUCHAR Rw = NULL, Ro = NULL;
    PVOID Address;
    SIZE_T Size;
    UCHAR Value = 0;

    if (!QueryPerformance(&Base))
        return;
    NtQueryPerformanceCounter(&Start, &Frequency);
    Status = CreateReserveSection(&Section, RESERVE_SIZE);
    NtQueryPerformanceCounter(&End, NULL);
    if (IsOutOfPrototypePtes(Status))
    {
        skip("2 TB section needs more paged pool (0x%lx)\n", Status);
        return;
    }
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status) || !QueryPerformance(&After))
        return;

    Milliseconds = (End.QuadPart - Start.QuadPart) * 1000 / Frequency.QuadPart;
    trace("2 TB SEC_RESERVE section: %I64u ms, available pages %lu -> %lu, committed pages %lu -> %lu\n",
          Milliseconds, Base.AvailablePages, After.AvailablePages, Base.CommittedPages, After.CommittedPages);
    ok((LONG)(Base.AvailablePages - After.AvailablePages) < (LONG)MB_IN_PAGES(64),
       "Reserving 2 TB consumed %ld pages of RAM\n", (LONG)(Base.AvailablePages - After.AvailablePages));
    ok((LONG)(After.CommittedPages - Base.CommittedPages) < (LONG)MB_IN_PAGES(64),
       "Reserving 2 TB charged %ld pages of commit\n", (LONG)(After.CommittedPages - Base.CommittedPages));
    ok(Milliseconds < 500, "Reserving 2 TB took %I64u ms\n", Milliseconds);

    /* Map a view 1 TB in: nothing in it is committed yet */
    SectionOffset.QuadPart = COMMIT_OFFSET;
    Size = COMMIT_VIEW_SIZE;
    Status = NtMapViewOfSection(Section, NtCurrentProcess(), (PVOID*)&Rw, 0, 0, &SectionOffset,
                                &Size, ViewUnmap, 0, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Close;
    CheckState(Rw, MEM_RESERVE, COMMIT_VIEW_SIZE);
    ok(!ReadByte(Rw, &Value), "Reading a reserved page did not fault\n");

    /* Committing pages through the view charges them */
    QueryPerformance(&Before);
    Address = Rw;
    Size = COMMIT_PAGES * PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    QueryPerformance(&After);
    ok((LONG)(After.CommittedPages - Before.CommittedPages) >= COMMIT_PAGES / 2,
       "Committing %u section pages charged %ld pages\n", COMMIT_PAGES,
       (LONG)(After.CommittedPages - Before.CommittedPages));
    CheckState(Rw, MEM_COMMIT, COMMIT_PAGES * PAGE_SIZE);
    CheckState(Rw + COMMIT_PAGES * PAGE_SIZE, MEM_RESERVE, COMMIT_VIEW_SIZE - COMMIT_PAGES * PAGE_SIZE);
    if (!NT_SUCCESS(Status))
        goto Unmap;
    Rw[0] = 0x11;
    Rw[(COMMIT_PAGES - 1) * PAGE_SIZE + 5] = 0x22;
    ok(!ReadByte(&Rw[COMMIT_PAGES * PAGE_SIZE], &Value), "Reading past the committed pages did not fault\n");

    /* A second view sees the same pages, and committing them again is free */
    Size = COMMIT_VIEW_SIZE;
    Status = NtMapViewOfSection(Section, NtCurrentProcess(), (PVOID*)&Ro, 0, 0, &SectionOffset,
                                &Size, ViewUnmap, 0, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        CheckState(Ro, MEM_COMMIT, COMMIT_PAGES * PAGE_SIZE);
        ok(ReadByte(&Ro[0], &Value) && (Value == 0x11), "Second view page 0 reads 0x%x\n", Value);
        Value = 0;
        ok(ReadByte(&Ro[(COMMIT_PAGES - 1) * PAGE_SIZE + 5], &Value) && (Value == 0x22),
           "Second view last page reads 0x%x\n", Value);

        QueryPerformance(&Before);
        Address = Ro;
        Size = COMMIT_PAGES * PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        QueryPerformance(&After);
        ok((LONG)(After.CommittedPages - Before.CommittedPages) < COMMIT_PAGES / 2,
           "Committing committed pages again charged %ld pages\n",
           (LONG)(After.CommittedPages - Before.CommittedPages));

        /* Section pages cannot be decommitted, only unmapped */
        Address = Ro;
        Size = PAGE_SIZE;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_DECOMMIT);
        ok_ntstatus(Status, STATUS_UNABLE_TO_DELETE_SECTION);
        ok(ReadByte(&Rw[0], &Value) && (Value == 0x11), "Page 0 reads 0x%x after decommit\n", Value);

        NtUnmapViewOfSection(NtCurrentProcess(), Ro);
    }

Unmap:
    NtUnmapViewOfSection(NtCurrentProcess(), Rw);
Close:
    QueryPerformance(&Before);
    NtClose(Section);

    /* Deleting the section gives back everything it held */
    if (QueryPerformance(&After))
    {
        trace("Closed: available pages %lu -> %lu, committed pages %lu -> %lu\n",
              Before.AvailablePages, After.AvailablePages, Before.CommittedPages, After.CommittedPages);
        ok((LONG)(Base.CommittedPages - After.CommittedPages) > -(LONG)MB_IN_PAGES(16),
           "Section teardown left %ld pages of commit charged\n",
           (LONG)(After.CommittedPages - Base.CommittedPages));
        ok((LONG)(Base.AvailablePages - After.AvailablePages) < (LONG)MB_IN_PAGES(64),
           "Section teardown left %ld pages of RAM in use\n",
           (LONG)(Base.AvailablePages - After.AvailablePages));
    }
}

/* The prototype PTEs of a reserved section are left unwritten in paged pool
 * pages nobody has touched yet; the first commit (or access) in a 2 MB chunk
 * of the section brings in the page holding its 512 prototype PTEs. That page
 * must read as zero (reserved): a page recycled from the free list still
 * holds whatever its last owner wrote. Feed the free list with pages that
 * read like committed prototype PTEs right before each first touch, from
 * several threads at once, and check that nothing but the committed page of
 * each chunk shows up as committed. */
#define FRESH_SECTION_SIZE  0x1000000000ULL       /* 64 GB */
#define FRESH_VIEW_SIZE     0x80000000ULL         /* 2 GB = 1024 chunks */
#define FRESH_CHUNK         0x200000ULL           /* covered by one page of prototype PTEs */
#define FRESH_CHUNKS        (FRESH_VIEW_SIZE / FRESH_CHUNK)
#define FRESH_COMMIT_CHUNKS (FRESH_CHUNKS / 2)    /* commit in the first half, read in the second */
#define FRESH_THREADS       4
#define FRESH_DIRTY_SIZE    (8 * 0x100000)
/* A demand-zero PTE with PAGE_READWRITE protection: a committed prototype PTE */
#define FRESH_GARBAGE       0x80ULL

typedef struct _FRESH_CONTEXT
{
    PUCHAR View;
    ULONG Index;
    ULONG CommitFailures;
    ULONG MissingFaults;
} FRESH_CONTEXT, *PFRESH_CONTEXT;

static
VOID
DirtyFreePages(VOID)
{
    NTSTATUS Status;
    PVOID Address = NULL;
    SIZE_T Size = FRESH_DIRTY_SIZE;
    PULONGLONG Pointer;
    SIZE_T i;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
        return;
    for (Pointer = Address, i = 0; i < FRESH_DIRTY_SIZE / sizeof(ULONGLONG); i++)
        Pointer[i] = FRESH_GARBAGE;
    Size = 0;
    NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_RELEASE);
}

static
ULONG
NTAPI
FreshPrototypePteThread(PVOID Parameter)
{
    PFRESH_CONTEXT Context = Parameter;
    ULONG Chunk;
    NTSTATUS Status;
    PVOID Address;
    SIZE_T Size;
    UCHAR Value;

    /* Chunk 0 shares its page of prototype PTEs with the segment header */
    for (Chunk = 1 + Context->Index; Chunk < FRESH_CHUNKS; Chunk += FRESH_THREADS)
    {
        DirtyFreePages();
        Address = Context->View + Chunk * FRESH_CHUNK;
        if (Chunk < FRESH_COMMIT_CHUNKS)
        {
            Size = PAGE_SIZE;
            Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size,
                                             MEM_COMMIT, PAGE_READWRITE);
            if (!NT_SUCCESS(Status))
                Context->CommitFailures++;
        }
        else
        {
            /* The first access to a chunk brings in its prototype PTEs, and
               must still find the page reserved */
            if (ReadByte(Address, &Value))
                Context->MissingFaults++;
        }
    }
    RtlExitUserThread(STATUS_SUCCESS);
    return 0;
}

static
VOID
TestFreshPrototypePtePages(VOID)
{
    NTSTATUS Status;
    HANDLE Section, Threads[FRESH_THREADS];
    FRESH_CONTEXT Contexts[FRESH_THREADS];
    LARGE_INTEGER SectionOffset;
    MEMORY_BASIC_INFORMATION Info;
    PUCHAR View = NULL, Address;
    SIZE_T Size = FRESH_VIEW_SIZE;
    ULONG i, Chunk, CommitFailures = 0, MissingFaults = 0, BadCommitted = 0, BadReserved = 0, BadData = 0;
    UCHAR Value;

    Status = CreateReserveSection(&Section, FRESH_SECTION_SIZE);
    if (IsOutOfPrototypePtes(Status))
    {
        skip("64 GB section needs more paged pool (0x%lx)\n", Status);
        return;
    }
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    SectionOffset.QuadPart = 0;
    Status = NtMapViewOfSection(Section, NtCurrentProcess(), (PVOID*)&View, 0, 0, &SectionOffset,
                                &Size, ViewUnmap, 0, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Section);
        return;
    }

    for (i = 0; i < FRESH_THREADS; i++)
    {
        Contexts[i].View = View;
        Contexts[i].Index = i;
        Contexts[i].CommitFailures = 0;
        Contexts[i].MissingFaults = 0;
        Status = RtlCreateUserThread(NtCurrentProcess(), NULL, FALSE, 0, 0, 0,
                                     FreshPrototypePteThread, &Contexts[i], &Threads[i], NULL);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            Threads[i] = NULL;
    }
    for (i = 0; i < FRESH_THREADS; i++)
    {
        if (!Threads[i])
            continue;
        NtWaitForSingleObject(Threads[i], FALSE, NULL);
        NtClose(Threads[i]);
        CommitFailures += Contexts[i].CommitFailures;
        MissingFaults += Contexts[i].MissingFaults;
    }

    for (Chunk = 1; Chunk < FRESH_CHUNKS; Chunk++)
    {
        Address = View + Chunk * FRESH_CHUNK;
        if (Chunk < FRESH_COMMIT_CHUNKS)
        {
            /* The committed page is a zero page and holds data */
            Status = NtQueryVirtualMemory(NtCurrentProcess(), Address, MemoryBasicInformation,
                                          &Info, sizeof(Info), NULL);
            if (!NT_SUCCESS(Status) || (Info.State != MEM_COMMIT) || (Info.RegionSize != PAGE_SIZE))
                BadCommitted++;
            Value = 0xFF;
            if (!ReadByte(Address, &Value) || (Value != 0))
                BadData++;
            _SEH2_TRY
            {
                Address[1] = 0x42;
                if (Address[1] != 0x42)
                    BadData++;
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                BadData++;
            }
            _SEH2_END;
            Address += PAGE_SIZE;
        }

        /* Everything else in the chunk is still reserved */
        Status = NtQueryVirtualMemory(NtCurrentProcess(), Address, MemoryBasicInformation,
                                      &Info, sizeof(Info), NULL);
        if (!NT_SUCCESS(Status) || (Info.State != MEM_RESERVE))
            BadReserved++;
        Status = NtQueryVirtualMemory(NtCurrentProcess(), View + (Chunk + 1) * FRESH_CHUNK - PAGE_SIZE,
                                      MemoryBasicInformation, &Info, sizeof(Info), NULL);
        if (!NT_SUCCESS(Status) || (Info.State != MEM_RESERVE))
            BadReserved++;
    }

    ok(CommitFailures == 0, "%lu of %lu commits failed\n", CommitFailures, (ULONG)FRESH_COMMIT_CHUNKS - 1);
    ok(MissingFaults == 0, "%lu of %lu reads of a reserved page did not fault\n",
       MissingFaults, (ULONG)(FRESH_CHUNKS - FRESH_COMMIT_CHUNKS));
    ok(BadCommitted == 0, "%lu committed pages are not committed alone\n", BadCommitted);
    ok(BadData == 0, "%lu committed pages do not read back zero or written data\n", BadData);
    ok(BadReserved == 0, "%lu reserved pages read as committed\n", BadReserved);

    NtUnmapViewOfSection(NtCurrentProcess(), View);
    NtClose(Section);
}

/* Every page committed in a section is charged once and given back when the
 * section goes away: creating, committing and closing sections over and over
 * must not eat into the commit limit */
#define CHARGE_ROUNDS       16
#define CHARGE_SECTION_SIZE (64 * 0x100000)
#define CHARGE_COMMIT_PAGES 1024
static
VOID
TestCommitChargeReturned(VOID)
{
    NTSTATUS Status;
    SYSTEM_PERFORMANCE_INFORMATION Before, After;
    LARGE_INTEGER SectionOffset, MaximumSize;
    HANDLE Section;
    PUCHAR View;
    PVOID Address;
    SIZE_T Size, i;
    ULONG Round;

    if (!QueryPerformance(&Before))
        return;
    for (Round = 0; Round < CHARGE_ROUNDS; Round++)
    {
        /* Reserved, committed through a view (twice), touched */
        Status = CreateReserveSection(&Section, CHARGE_SECTION_SIZE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            return;
        View = NULL;
        Size = 0;
        SectionOffset.QuadPart = 0;
        Status = NtMapViewOfSection(Section, NtCurrentProcess(), (PVOID*)&View, 0, 0, &SectionOffset,
                                    &Size, ViewUnmap, 0, PAGE_READWRITE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Address = View + 4 * 0x100000;
            Size = CHARGE_COMMIT_PAGES * PAGE_SIZE;
            Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
            ok_ntstatus(Status, STATUS_SUCCESS);
            Address = View + 4 * 0x100000;
            Size = CHARGE_COMMIT_PAGES * PAGE_SIZE;
            Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
            ok_ntstatus(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                for (i = 0; i < CHARGE_COMMIT_PAGES; i++)
                    View[4 * 0x100000 + i * PAGE_SIZE] = (UCHAR)i;
            }
            NtUnmapViewOfSection(NtCurrentProcess(), View);
        }
        NtClose(Section);

        /* Committed at creation, mapped, touched */
        MaximumSize.QuadPart = CHARGE_COMMIT_PAGES * PAGE_SIZE;
        Status = NtCreateSection(&Section, SECTION_ALL_ACCESS, NULL, &MaximumSize,
                                 PAGE_READWRITE, SEC_COMMIT, NULL);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            return;
        View = NULL;
        Size = 0;
        Status = NtMapViewOfSection(Section, NtCurrentProcess(), (PVOID*)&View, 0, 0, NULL,
                                    &Size, ViewUnmap, 0, PAGE_READWRITE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            for (i = 0; i < CHARGE_COMMIT_PAGES; i++)
                View[i * PAGE_SIZE] = (UCHAR)i;
            NtUnmapViewOfSection(NtCurrentProcess(), View);
        }
        NtClose(Section);
    }
    if (!QueryPerformance(&After))
        return;

    trace("%u rounds: committed pages %lu -> %lu\n", CHARGE_ROUNDS, Before.CommittedPages, After.CommittedPages);
    ok((LONG)(After.CommittedPages - Before.CommittedPages) < CHARGE_COMMIT_PAGES / 2,
       "%u rounds of %u committed section pages left %ld pages charged\n",
       CHARGE_ROUNDS, 2 * CHARGE_COMMIT_PAGES, (LONG)(After.CommittedPages - Before.CommittedPages));
}

START_TEST(NtCreateSectionHuge)
{
    /* First, while paged pool has no multi-GB holes yet */
    TestSecondSectionAboveFourGigabytes();
    TestReserveConsumesNothing();
    TestFreshPrototypePtePages();
    TestCommitChargeReturned();
    TestDoubleMapping(0x40000000ULL);      /* 1 GB */
    TestDoubleMapping(0x1000000000ULL);    /* 64 GB */
    TestDoubleMapping(0x10000000000ULL);   /* 1 TB */
    TestDoubleMapping(0x20000000000ULL);   /* 2 TB: CoreCLR's MaxDoubleMappedSize */
}
