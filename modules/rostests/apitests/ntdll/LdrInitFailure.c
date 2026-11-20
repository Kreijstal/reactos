/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test the exit status of a process whose loader initialization
 *              fails before it got anywhere (before the NLS tables are set up)
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"

/* An address no user-mode query can succeed on */
#define BOGUS_STACK_LIMIT ((PVOID)~(ULONG_PTR)0xFFFF)

static
NTSTATUS
WriteProcessParameters(
    _In_ HANDLE Process,
    _In_ PUNICODE_STRING ImagePath)
{
    PRTL_USER_PROCESS_PARAMETERS Parameters = NULL;
    PROCESS_BASIC_INFORMATION BasicInfo;
    PVOID RemoteParameters = NULL;
    SIZE_T Size;
    NTSTATUS Status;

    Status = RtlCreateProcessParameters(&Parameters, ImagePath, NULL, NULL, ImagePath,
                                        NULL, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The child has no environment of ours to point to */
    Parameters->Environment = NULL;
    RtlDeNormalizeProcessParams(Parameters);

    Size = Parameters->MaximumLength;
    Status = NtAllocateVirtualMemory(Process, &RemoteParameters, 0, &Size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (NT_SUCCESS(Status))
    {
        Status = NtWriteVirtualMemory(Process, RemoteParameters, Parameters,
                                      Parameters->Length, NULL);
    }
    if (NT_SUCCESS(Status))
    {
        Status = NtQueryInformationProcess(Process, ProcessBasicInformation,
                                           &BasicInfo, sizeof(BasicInfo), NULL);
    }
    if (NT_SUCCESS(Status))
    {
        Status = NtWriteVirtualMemory(Process,
                                      &BasicInfo.PebBaseAddress->ProcessParameters,
                                      &RemoteParameters,
                                      sizeof(RemoteParameters),
                                      NULL);
    }

    RtlDestroyProcessParameters(Parameters);
    return Status;
}

START_TEST(LdrInitFailure)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    SECTION_IMAGE_INFORMATION ImageInfo;
    MEMORY_BASIC_INFORMATION MemoryInfo;
    PROCESS_BASIC_INFORMATION BasicInfo;
    UNICODE_STRING ImagePath = { 0 };
    WCHAR DosPath[MAX_PATH];
    IO_STATUS_BLOCK IoStatus;
    INITIAL_TEB InitialTeb;
    CLIENT_ID ClientId;
    CONTEXT Context;
    HANDLE File = NULL, Section = NULL, Process = NULL, Thread = NULL;
    PVOID Stack = NULL;
    SIZE_T StackSize = 0x10000;
    ULONG HardErrorMode = 0;
    BOOL Wow64 = FALSE;
    NTSTATUS Status, Expected;

    if (IsWow64Process(GetCurrentProcess(), &Wow64) && Wow64)
    {
        /* A WoW64 process can't start a thread in a new process this way */
        skip("WoW64 process\n");
        return;
    }

    /* Any image does: the process never gets to run it */
    GetModuleFileNameW(NULL, DosPath, _countof(DosPath));
    if (!RtlDosPathNameToNtPathName_U(DosPath, &ImagePath, NULL, NULL))
    {
        skip("RtlDosPathNameToNtPathName_U(%ls) failed\n", DosPath);
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &ImagePath, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&File, FILE_READ_DATA | FILE_EXECUTE | SYNCHRONIZE, &ObjectAttributes,
                        &IoStatus, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtCreateSection(&Section, SECTION_ALL_ACCESS, NULL, NULL, PAGE_EXECUTE,
                             SEC_IMAGE, File);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtQuerySection(Section, SectionImageInformation, &ImageInfo, sizeof(ImageInfo), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtCreateProcess(&Process, PROCESS_ALL_ACCESS, NULL, NtCurrentProcess(),
                             FALSE, Section, NULL, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = WriteProcessParameters(Process, &ImagePath);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Don't let the failure wait for someone to click a message box away */
    Status = NtSetInformationProcess(Process, ProcessDefaultHardErrorMode,
                                     &HardErrorMode, sizeof(HardErrorMode));
    ok_hex(Status, STATUS_SUCCESS);

    /* The status the child's loader will get querying its stack */
    Expected = NtQueryVirtualMemory(Process, BOGUS_STACK_LIMIT, MemoryBasicInformation,
                                    &MemoryInfo, sizeof(MemoryInfo), NULL);
    ok(!NT_SUCCESS(Expected), "NtQueryVirtualMemory(%p) returned 0x%lx\n",
       BOGUS_STACK_LIMIT, Expected);

    Status = NtAllocateVirtualMemory(Process, &Stack, 0, &StackSize,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /*
     * Without an allocated stack base the TEB has no deallocation stack,
     * and the loader queries its stack limit to find one first thing,
     * long before it initializes the process.
     */
    RtlZeroMemory(&InitialTeb, sizeof(InitialTeb));
    InitialTeb.StackBase = (PUCHAR)Stack + StackSize;
    InitialTeb.StackLimit = BOGUS_STACK_LIMIT;
    RtlInitializeContext(Process, &Context, NULL,
                         (PTHREAD_START_ROUTINE)ImageInfo.TransferAddress,
                         (PUCHAR)Stack + StackSize);

    Status = NtCreateThread(&Thread, THREAD_ALL_ACCESS, NULL, Process, &ClientId,
                            &Context, &InitialTeb, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (WaitForSingleObject(Process, 30000) != WAIT_OBJECT_0)
    {
        ok(0, "The process did not exit\n");
        NtTerminateProcess(Process, STATUS_TIMEOUT);
        WaitForSingleObject(Process, 5000);
    }

    /* The process dies with the failure, not with a fault reporting it */
    Status = NtQueryInformationProcess(Process, ProcessBasicInformation,
                                       &BasicInfo, sizeof(BasicInfo), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok(BasicInfo.ExitStatus != STATUS_ACCESS_VIOLATION, "Exit status 0x%lx\n", BasicInfo.ExitStatus);
    if (is_reactos())
    {
        ok(BasicInfo.ExitStatus == Expected, "Exit status 0x%lx, expected 0x%lx\n",
           BasicInfo.ExitStatus, Expected);
    }

Cleanup:
    if (Thread)
        NtClose(Thread);
    if (Process)
    {
        NtTerminateProcess(Process, STATUS_SUCCESS);
        NtClose(Process);
    }
    if (Section)
        NtClose(Section);
    if (File)
        NtClose(File);
    RtlFreeUnicodeString(&ImagePath);
}
