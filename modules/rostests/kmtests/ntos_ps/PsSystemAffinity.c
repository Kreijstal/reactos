/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for the processor affinity of the
 *              System process and the threads and processes created from it
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

/*
 * The System process is created before the application processors are
 * started. Once they are, it must be allowed on every one of them: its
 * threads (worker threads, drivers' system threads) and every process that
 * descends from it take its affinity. With the boot processor's alone, all
 * of them ran on processor 0 only, and a thread that asked for its own
 * affinity on another processor bugchecked with INVALID_AFFINITY_SET.
 */

static
KAFFINITY
QueryProcessAffinity(
    _In_ HANDLE ProcessHandle)
{
    PROCESS_BASIC_INFORMATION Info;
    NTSTATUS Status;

    RtlZeroMemory(&Info, sizeof(Info));
    Status = ZwQueryInformationProcess(ProcessHandle, ProcessBasicInformation, &Info, sizeof(Info), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return (KAFFINITY)Info.AffinityMask;
}

typedef struct _AFFINITY_RUN
{
    ULONG Processors;
    ULONG Ran;
} AFFINITY_RUN, *PAFFINITY_RUN;

static
VOID
NTAPI
AffinityThread(
    _In_ PVOID Context)
{
    PAFFINITY_RUN Run = Context;
    KAFFINITY Active = KeQueryActiveProcessors(), Old = 0;
    ULONG Processor;

    for (Processor = 0; Processor < Run->Processors; Processor++)
    {
        KAFFINITY Previous;

        if (!(Active & AFFINITY_MASK(Processor)))
            continue;

        /* The thread's own affinity, not a system affinity */
        Previous = KeSetAffinityThread(KeGetCurrentThread(), AFFINITY_MASK(Processor));
        if (Processor == 0)
            Old = Previous;
        if (KeGetCurrentProcessorNumber() == Processor)
            Run->Ran |= 1 << Processor;
    }
    if (Old)
        KeSetAffinityThread(KeGetCurrentThread(), Old);

    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(PsSystemAffinity)
{
    KAFFINITY Active = KeQueryActiveProcessors(), System;
    HANDLE SystemHandle, ThreadHandle;
    PVOID ThreadObject;
    AFFINITY_RUN Run;
    NTSTATUS Status;
    ULONG Processor;

    Status = ObOpenObjectByPointer(PsInitialSystemProcess, OBJ_KERNEL_HANDLE, NULL, PROCESS_QUERY_INFORMATION,
                                   *PsProcessType, KernelMode, &SystemHandle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No System process handle\n"))
        return;
    System = QueryProcessAffinity(SystemHandle);
    ZwClose(SystemHandle);

    ok(System == Active, "System process affinity 0x%Ix, active processors 0x%Ix\n",
       (ULONG_PTR)System, (ULONG_PTR)Active);
    /* Our caller's process descends from it */
    ok_eq_ulongptr((ULONG_PTR)QueryProcessAffinity(NtCurrentProcess()), (ULONG_PTR)Active);

    /* A system thread asking for one processor more than its process has bugchecks */
    if (skip(System == Active, "A system thread cannot be given every processor\n"))
        return;

    RtlZeroMemory(&Run, sizeof(Run));
    Run.Processors = min(KeNumberProcessors, 32);
    Status = PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, AffinityThread, &Run);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No thread\n"))
        return;
    Status = ObReferenceObjectByHandle(ThreadHandle, SYNCHRONIZE, *PsThreadType, KernelMode, &ThreadObject, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(ThreadHandle);
    if (NT_SUCCESS(Status))
    {
        KeWaitForSingleObject(ThreadObject, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ThreadObject);
    }

    for (Processor = 0; Processor < Run.Processors; Processor++)
    {
        if (Active & AFFINITY_MASK(Processor))
            ok(Run.Ran & (1 << Processor), "The system thread never ran on processor %lu\n", Processor);
    }
}
