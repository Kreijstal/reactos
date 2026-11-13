/*
 * PROJECT:         ReactOS API Tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test for the NtProtectVirtualMemory API
 * PROGRAMMERS:     Jérôme Gardou <jerome.gardou@reactos.org>
 *                  Thomas Faber <thomas.faber@reactos.org>
 */

#include "precomp.h"

static
void
TestReadWrite(void)
{
    ULONG* allocationStart = NULL;
    NTSTATUS status;
    SIZE_T allocationSize;
    ULONG oldProtection;

    /* Reserve a page */
    allocationSize = PAGE_SIZE;
    status = NtAllocateVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        0,
        &allocationSize,
        MEM_RESERVE,
        PAGE_NOACCESS);
    ok(NT_SUCCESS(status), "Reserving memory failed\n");

    /* Commit the page (RW) */
    status = NtAllocateVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        0,
        &allocationSize,
        MEM_COMMIT,
        PAGE_READWRITE);
    ok(NT_SUCCESS(status), "Commiting memory failed\n");

    /* Try writing it */
    StartSeh()
    {
        *allocationStart = 0xFF;
    } EndSeh(STATUS_SUCCESS);

    /* Try reading it */
    StartSeh()
    {
        ok(*allocationStart == 0xFF, "Memory was not written\n");
    } EndSeh(STATUS_SUCCESS);

    /* Set it as read only */
    status = NtProtectVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        &allocationSize,
        PAGE_READONLY,
        &oldProtection);
    ok(NT_SUCCESS(status), "NtProtectVirtualMemory failed.\n");
    ok(oldProtection == PAGE_READWRITE, "Expected PAGE_READWRITE, got %08lx.\n", oldProtection);

    /* Try writing it */
    StartSeh()
    {
        *allocationStart = 0xAA;
    } EndSeh(STATUS_ACCESS_VIOLATION);

    /* Try reading it */
    StartSeh()
    {
        ok(*allocationStart == 0xFF, "read-only memory were changed.\n");
    } EndSeh(STATUS_SUCCESS);

    /* Set it as no access */
    status = NtProtectVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        &allocationSize,
        PAGE_NOACCESS,
        &oldProtection);
    ok(NT_SUCCESS(status), "NtProtectVirtualMemory failed.\n");
    ok(oldProtection == PAGE_READONLY, "Expected PAGE_READONLY, got %08lx.\n", oldProtection);

    /* Try writing it */
    StartSeh()
    {
        *allocationStart = 0xAA;
    } EndSeh(STATUS_ACCESS_VIOLATION);

    /* Try reading it */
    StartSeh()
    {
        ok(*allocationStart == 0, "Test should not go as far as this.\n");
    } EndSeh(STATUS_ACCESS_VIOLATION);

    /* Set it as readable again */
    status = NtProtectVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        &allocationSize,
        PAGE_READONLY,
        &oldProtection);
    ok(NT_SUCCESS(status), "NtProtectVirtualMemory failed.\n");
    ok(oldProtection == PAGE_NOACCESS, "Expected PAGE_READONLY, got %08lx.\n", oldProtection);

    /* Try writing it */
    StartSeh()
    {
        *allocationStart = 0xAA;
    } EndSeh(STATUS_ACCESS_VIOLATION);

    /* Try reading it */
    StartSeh()
    {
        ok(*allocationStart == 0xFF, "Memory content was not preserved.\n");
    } EndSeh(STATUS_SUCCESS);

    /* Free memory */
    status = NtFreeVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        &allocationSize,
        MEM_RELEASE);
    ok(NT_SUCCESS(status), "Failed freeing memory.\n");
}

/* Regression test for CORE-13311 */
static
void
TestFreeNoAccess(void)
{
    PVOID Mem;
    SIZE_T Size;
    NTSTATUS Status;
    ULONG Iteration, PageNumber;
    PUCHAR Page;
    ULONG OldProtection;

    for (Iteration = 0; Iteration < 50000; Iteration++)
    {
        Mem = NULL;
        Size = 16 * PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                         &Mem,
                                         0,
                                         &Size,
                                         MEM_COMMIT,
                                         PAGE_READWRITE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
        {
            break;
        }

        for (PageNumber = 0; PageNumber < 16; PageNumber++)
        {
            Page = Mem;
            Page += PageNumber * PAGE_SIZE;
            ok(*Page == 0,
               "[%lu, %lu] Got non-zero memory. %x at %p\n",
               Iteration, PageNumber, *Page, Page);
            *Page = 123;
        }

        Status = NtProtectVirtualMemory(NtCurrentProcess(),
                                        &Mem,
                                        &Size,
                                        PAGE_NOACCESS,
                                        &OldProtection);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_hex(OldProtection, PAGE_READWRITE);

        Size = 0;
        Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                     &Mem,
                                     &Size,
                                     MEM_RELEASE);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }
}

/*
 * An image view is one allocation: VirtualQuery merges the following sections
 * of the same protection into one region, and the protection of such a region,
 * or of any range inside the view, can be changed at once. This is what the
 * pseudo-relocation code of mingw-w64/Cygwin/MSYS2 binaries does at startup:
 * VirtualQuery a page, then VirtualProtect(BaseAddress, RegionSize).
 */
static
void
TestImageView(void)
{
    WCHAR Path[MAX_PATH];
    HANDLE File, Mapping;
    PUCHAR Base, Address;
    PIMAGE_NT_HEADERS NtHeaders;
    PIMAGE_SECTION_HEADER Sections;
    MEMORY_BASIC_INFORMATION Mbi, Mbi2;
    SIZE_T ImageSize, Size, Ret;
    PVOID ProtectBase;
    ULONG OldProtect, i;
    NTSTATUS Status;
    BOOLEAN Merged = FALSE;

    GetSystemDirectoryW(Path, _countof(Path));
    wcscat(Path, L"\\shell32.dll");
    File = CreateFileW(Path, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    Mapping = CreateFileMappingW(File, NULL, PAGE_EXECUTE_WRITECOPY | SEC_IMAGE, 0, 0, NULL);
    ok(Mapping != NULL, "CreateFileMappingW failed with %lu\n", GetLastError());
    CloseHandle(File);
    if (!Mapping)
        return;
    Base = MapViewOfFile(Mapping, FILE_MAP_COPY | FILE_MAP_EXECUTE, 0, 0, 0);
    ok(Base != NULL, "MapViewOfFile failed with %lu\n", GetLastError());
    CloseHandle(Mapping);
    if (!Base)
        return;

    NtHeaders = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
    Sections = IMAGE_FIRST_SECTION(NtHeaders);
    ImageSize = ROUND_TO_PAGES(NtHeaders->OptionalHeader.SizeOfImage);

    /* Protect each region VirtualQuery reports from a page inside it */
    for (i = 0; i < NtHeaders->FileHeader.NumberOfSections; i++)
    {
        Address = Base + ROUND_TO_PAGES(Sections[i].VirtualAddress + Sections[i].Misc.VirtualSize) - PAGE_SIZE;
        if (Sections[i].Misc.VirtualSize == 0)
            continue;
        Ret = VirtualQuery(Address, &Mbi, sizeof(Mbi));
        ok(Ret == sizeof(Mbi), "[%lu] VirtualQuery failed with %lu\n", i, GetLastError());
        if (Ret != sizeof(Mbi))
            continue;
        ok(Mbi.BaseAddress == Address, "[%lu] BaseAddress %p, expected %p\n", i, Mbi.BaseAddress, Address);
        ok(Mbi.AllocationBase == Base, "[%lu] AllocationBase %p, expected %p\n", i, Mbi.AllocationBase, Base);
        ok((PUCHAR)Mbi.BaseAddress + Mbi.RegionSize <= Base + ImageSize, "[%lu] Region runs past the image\n", i);
        if ((i + 1 < NtHeaders->FileHeader.NumberOfSections) &&
            ((PUCHAR)Mbi.BaseAddress + Mbi.RegionSize > Base + Sections[i + 1].VirtualAddress))
        {
            Merged = TRUE;
        }

        ProtectBase = Mbi.BaseAddress;
        Size = Mbi.RegionSize;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &Size, PAGE_NOACCESS, &OldProtect);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(ProtectBase == Mbi.BaseAddress, "[%lu] ProtectBase %p, expected %p\n", i, ProtectBase, Mbi.BaseAddress);
        ok(Size == Mbi.RegionSize, "[%lu] Size 0x%Ix, expected 0x%Ix\n", i, Size, Mbi.RegionSize);
        ok(OldProtect == Mbi.Protect, "[%lu] OldProtect 0x%lx, expected 0x%lx\n", i, OldProtect, Mbi.Protect);
        if (!NT_SUCCESS(Status))
            continue;

        /* The whole region changed */
        Ret = VirtualQuery(Address, &Mbi2, sizeof(Mbi2));
        ok(Ret == sizeof(Mbi2), "[%lu] VirtualQuery failed with %lu\n", i, GetLastError());
        ok(Mbi2.Protect == PAGE_NOACCESS, "[%lu] Protect 0x%lx\n", i, Mbi2.Protect);
        ok(Mbi2.RegionSize >= Mbi.RegionSize, "[%lu] RegionSize 0x%Ix, expected at least 0x%Ix\n",
           i, Mbi2.RegionSize, Mbi.RegionSize);
        Ret = VirtualQuery((PUCHAR)Mbi.BaseAddress + Mbi.RegionSize - PAGE_SIZE, &Mbi2, sizeof(Mbi2));
        ok(Ret == sizeof(Mbi2) && Mbi2.Protect == PAGE_NOACCESS, "[%lu] Last page protect 0x%lx\n", i, Mbi2.Protect);

        /* And back */
        ProtectBase = Mbi.BaseAddress;
        Size = Mbi.RegionSize;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &Size, Mbi.Protect, &OldProtect);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(OldProtect == PAGE_NOACCESS, "[%lu] OldProtect 0x%lx\n", i, OldProtect);
    }
    ok(Merged, "No region of the image spans more than one section\n");

    /* The whole image at once; the old protection is that of the first page */
    ProtectBase = Base;
    Size = ImageSize;
    Status = NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &Size, PAGE_READONLY, &OldProtect);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(ProtectBase == Base, "ProtectBase %p, expected %p\n", ProtectBase, Base);
    ok(Size == ImageSize, "Size 0x%Ix, expected 0x%Ix\n", Size, ImageSize);
    ok(OldProtect == PAGE_READONLY, "OldProtect 0x%lx\n", OldProtect);
    Ret = VirtualQuery(Base, &Mbi, sizeof(Mbi));
    ok(Ret == sizeof(Mbi), "VirtualQuery failed with %lu\n", GetLastError());
    ok(Mbi.Protect == PAGE_READONLY, "Protect 0x%lx\n", Mbi.Protect);
    ok(Mbi.RegionSize == ImageSize, "RegionSize 0x%Ix, expected 0x%Ix\n", Mbi.RegionSize, ImageSize);

    /* A range that leaves the image view is not one allocation */
    ProtectBase = Base + ImageSize - PAGE_SIZE;
    Size = 2 * PAGE_SIZE;
    Status = NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &Size, PAGE_READONLY, &OldProtect);
    ok_ntstatus(Status, STATUS_CONFLICTING_ADDRESSES);

    ok(UnmapViewOfFile(Base), "UnmapViewOfFile failed with %lu\n", GetLastError());
}

START_TEST(NtProtectVirtualMemory)
{
    TestReadWrite();
    TestFreeNoAccess();
    TestImageView();
}
