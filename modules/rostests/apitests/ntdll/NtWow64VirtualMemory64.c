/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test NtWow64ReadVirtualMemory64 and NtWow64WriteVirtualMemory64
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *PFN_NTWOW64READVIRTUALMEMORY64)(
    _In_ HANDLE ProcessHandle,
    _In_ ULONG64 BaseAddress,
    _Out_writes_bytes_(Size) PVOID Buffer,
    _In_ ULONG64 Size,
    _Out_opt_ PULONG64 NumberOfBytesRead);

typedef NTSTATUS (NTAPI *PFN_NTWOW64WRITEVIRTUALMEMORY64)(
    _In_ HANDLE ProcessHandle,
    _In_ ULONG64 BaseAddress,
    _In_reads_bytes_(Size) PVOID Buffer,
    _In_ ULONG64 Size,
    _Out_opt_ PULONG64 NumberOfBytesWritten);

START_TEST(NtWow64VirtualMemory64)
{
    PFN_NTWOW64READVIRTUALMEMORY64 pNtWow64ReadVirtualMemory64;
    PFN_NTWOW64WRITEVIRTUALMEMORY64 pNtWow64WriteVirtualMemory64;
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    static CHAR Source[] = "WoW64 cross-bitness memory access";
    CHAR Buffer[sizeof(Source)];
    ULONG64 Transferred;
    BOOL Wow64 = FALSE;
    NTSTATUS Status;

    /* These only exist in the 32-bit ntdll of a 64-bit system */
    pNtWow64ReadVirtualMemory64 =
        (PFN_NTWOW64READVIRTUALMEMORY64)GetProcAddress(Ntdll, "NtWow64ReadVirtualMemory64");
    pNtWow64WriteVirtualMemory64 =
        (PFN_NTWOW64WRITEVIRTUALMEMORY64)GetProcAddress(Ntdll, "NtWow64WriteVirtualMemory64");
    if (!IsWow64Process(GetCurrentProcess(), &Wow64) || !Wow64 ||
        !pNtWow64ReadVirtualMemory64 || !pNtWow64WriteVirtualMemory64)
    {
        skip("Not a WoW64 process\n");
        return;
    }

    /* Read our own memory through its 64-bit address */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xCC);
    Transferred = 0xDEADBEEF;
    Status = pNtWow64ReadVirtualMemory64(NtCurrentProcess(),
                                         (ULONG64)(ULONG_PTR)Source,
                                         Buffer,
                                         sizeof(Buffer),
                                         &Transferred);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Transferred == sizeof(Buffer), "Read %I64u bytes\n", Transferred);
    ok(!memcmp(Buffer, Source, sizeof(Source)), "Read '%.*s'\n", (int)sizeof(Buffer) - 1, Buffer);

    /* And write it back elsewhere */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xCC);
    Transferred = 0xDEADBEEF;
    Status = pNtWow64WriteVirtualMemory64(NtCurrentProcess(),
                                          (ULONG64)(ULONG_PTR)Buffer,
                                          Source,
                                          sizeof(Source),
                                          &Transferred);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Transferred == sizeof(Source), "Wrote %I64u bytes\n", Transferred);
    ok(!memcmp(Buffer, Source, sizeof(Source)), "Wrote '%.*s'\n", (int)sizeof(Buffer) - 1, Buffer);

    /* The count is optional */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xCC);
    Status = pNtWow64ReadVirtualMemory64(NtCurrentProcess(),
                                         (ULONG64)(ULONG_PTR)Source,
                                         Buffer,
                                         sizeof(Buffer),
                                         NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok(!memcmp(Buffer, Source, sizeof(Source)), "Read '%.*s'\n", (int)sizeof(Buffer) - 1, Buffer);

    /* Nothing is mapped at the bottom of the address space */
    Transferred = 0xDEADBEEF;
    Status = pNtWow64ReadVirtualMemory64(NtCurrentProcess(), 0x1000, Buffer, sizeof(Buffer), &Transferred);
    ok(Status == STATUS_PARTIAL_COPY || Status == STATUS_ACCESS_VIOLATION,
       "Status = 0x%lx\n", Status);
}
