/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that system calls given inaccessible buffers fail with
 *              STATUS_ACCESS_VIOLATION instead of faulting the caller
 *              (in a WoW64 process, the 32-bit to 64-bit thunks touch them)
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"

START_TEST(BadUserBuffers)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    CLIENT_ID ClientId;
    HANDLE Handle;
    PVOID NoAccess;
    NTSTATUS Status;

    NoAccess = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    ok(NoAccess != NULL, "VirtualAlloc failed: %lu\n", GetLastError());
    if (!NoAccess)
        return;

    /* Output buffer */
    Status = NtQueryInformationThread(NtCurrentThread(), ThreadBasicInformation,
                                      NoAccess, sizeof(THREAD_BASIC_INFORMATION), NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    Status = NtQueryInformationProcess(NtCurrentProcess(), ProcessBasicInformation,
                                       NoAccess, sizeof(PROCESS_BASIC_INFORMATION), NULL);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Output handle */
    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    ClientId.UniqueProcess = UlongToHandle(GetCurrentProcessId());
    ClientId.UniqueThread = NULL;
    Status = NtOpenProcess(NoAccess, PROCESS_QUERY_LIMITED_INFORMATION, &ObjectAttributes, &ClientId);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);

    /* Input structures */
    Handle = (HANDLE)(ULONG_PTR)0xDEADBEEF;
    Status = NtOpenProcess(&Handle, PROCESS_QUERY_LIMITED_INFORMATION, NoAccess, &ClientId);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);
    ok(Handle == (HANDLE)(ULONG_PTR)0xDEADBEEF || Handle == NULL, "Handle = %p\n", Handle);

    Handle = (HANDLE)(ULONG_PTR)0xDEADBEEF;
    Status = NtOpenProcess(&Handle, PROCESS_QUERY_LIMITED_INFORMATION, &ObjectAttributes, NoAccess);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);
    ok(Handle == (HANDLE)(ULONG_PTR)0xDEADBEEF || Handle == NULL, "Handle = %p\n", Handle);

    VirtualFree(NoAccess, 0, MEM_RELEASE);
}
