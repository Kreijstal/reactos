/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS System Libraries
 * FILE:            dll/ntdll/wow64/memory.c
 * PURPOSE:         WOW64 NTDLL Functions Definitions
 * PROGRAMMER:      Marcin Jabłoński
 */

#include "ntdll32.h"

/*
 * On a real WoW64 guest (BUILD_WOW6432) these are emitted as heaven's-gate
 * syscall stubs by ntoskrnl/ntdll.S so the 64-bit host services them against a
 * full 64-bit address space.  On a plain 32-bit build there is no 64-bit space
 * to reach, so they stay STATUS_NOT_IMPLEMENTED placeholders to keep ntdll
 * linking and exporting the symbols.
 */
#ifndef BUILD_WOW6432
NTSYSAPI
NTSTATUS
NTAPI
NtWow64ReadVirtualMemory64(HANDLE ProcessHandle,
                           UINT64 BaseAddress,
                           PVOID Buffer,
                           UINT64 BufferSize,
                           PUINT64 NumberOfBytesRead)
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSYSAPI
NTSTATUS
NTAPI
NtWow64WriteVirtualMemory64(HANDLE ProcessHandle,
                            UINT64 BaseAddress,
                            PVOID Buffer,
                            UINT64 BufferSize,
                            PUINT64 NumberOfBytesWritten)
{
    return STATUS_NOT_IMPLEMENTED;
}
#endif /* !BUILD_WOW6432 */