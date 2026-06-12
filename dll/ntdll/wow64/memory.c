/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS System Libraries
 * FILE:            dll/ntdll/wow64/memory.c
 * PURPOSE:         WOW64 NTDLL Functions Definitions
 * PROGRAMMER:      Marcin Jabłoński
 */

#include "ntdll32.h"

/*
 * These access a 64-bit address space from a 32-bit WoW64 process and require
 * the cross-bitness syscall path to be in place. They are exported so the
 * 32-bit ntdll links and loads; returning STATUS_NOT_IMPLEMENTED until the
 * heaven's-gate thunk wires them through to the native NtReadVirtualMemory.
 */
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