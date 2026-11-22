/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for kernel page faults taken by
 *              several threads sharing each processor
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#define THREADS_PER_PROCESSOR   3 /* one waker, two fault threads */
#define MAX_THREADS             (THREADS_PER_PROCESSOR * 16)
#define REGION_SIZE             (1024 * 1024)
#define RUN_TIME_MS             8000
#define CALLS_PER_PAGE          8

typedef struct _FAULT_THREAD
{
    KAFFINITY Affinity;
    BOOLEAN Waker;
    ULONGLONG Deadline;
    NTSTATUS Status;
    ULONG Faults;
    ULONG WrongProcessor;
} FAULT_THREAD, *PFAULT_THREAD;

/*
 * Touch freshly committed pages from kernel mode and make system calls from
 * kernel mode: every first touch is a demand-zero page fault and every Zw
 * call an SVC, both synchronous exceptions taken in kernel mode. Their
 * handler runs with interrupts enabled, so the clock preempts it at any
 * point and the other threads on the processor take their own exceptions
 * meanwhile. The arm64 trap handler kept a per-processor "exception in
 * progress" flag across that preemptible window and bugchecked the next
 * thread's ordinary fault as a recursive exception.
 */
static
VOID
NTAPI
FaultThread(
    _In_ PVOID Context)
{
    PFAULT_THREAD Fault = Context;
    PVOID Base = NULL;
    SIZE_T Size = REGION_SIZE;
    NTSTATUS Status;
    ULONG Offset;

    KeSetSystemAffinityThread(Fault->Affinity);

    if (Fault->Waker)
    {
        LARGE_INTEGER Delay;

        /*
         * Wake on every clock tick at a higher priority, preempting whatever
         * the fault threads are doing - inside their fault handler too - and
         * take a fault of our own right away.
         */
        KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY + 1);
        Delay.QuadPart = -10000;
        while (KeQueryInterruptTime() < Fault->Deadline)
        {
            PVOID Page = NULL;

            Size = PAGE_SIZE;
            Status = ZwAllocateVirtualMemory(ZwCurrentProcess(), &Page, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!NT_SUCCESS(Status))
                break;

            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            *(volatile ULONG *)Page = 1;
            Fault->Faults++;

            Size = 0;
            Status = ZwFreeVirtualMemory(ZwCurrentProcess(), &Page, &Size, MEM_RELEASE);
            if (!NT_SUCCESS(Status))
                break;
        }
    }
    else if (NT_SUCCESS(Status = ZwAllocateVirtualMemory(ZwCurrentProcess(), &Base, 0, &Size, MEM_RESERVE, PAGE_READWRITE)))
    {
        while (KeQueryInterruptTime() < Fault->Deadline)
        {
            PVOID Commit = Base;

            Size = REGION_SIZE;
            Status = ZwAllocateVirtualMemory(ZwCurrentProcess(), &Commit, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
            if (!NT_SUCCESS(Status))
                break;

            for (Offset = 0; Offset < REGION_SIZE; Offset += PAGE_SIZE)
            {
                PROCESS_BASIC_INFORMATION Info;
                ULONG Call;

                *(volatile ULONG *)((PUCHAR)Base + Offset) = Offset;
                Fault->Faults++;

                /* A system call from kernel mode is a synchronous exception too */
                for (Call = 0; Call < CALLS_PER_PAGE; Call++)
                    ZwQueryInformationProcess(ZwCurrentProcess(), ProcessBasicInformation, &Info, sizeof(Info), NULL);
            }
            if ((KeQueryActiveProcessors() & Fault->Affinity) &&
                !(Fault->Affinity & ((KAFFINITY)1 << (KeGetCurrentProcessorNumber()))))
            {
                Fault->WrongProcessor++;
            }

            Size = REGION_SIZE;
            Status = ZwFreeVirtualMemory(ZwCurrentProcess(), &Base, &Size, MEM_DECOMMIT);
            if (!NT_SUCCESS(Status))
                break;
        }

        Size = 0;
        ZwFreeVirtualMemory(ZwCurrentProcess(), &Base, &Size, MEM_RELEASE);
    }

    Fault->Status = Status;
    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(MmFaultPreempt)
{
    static FAULT_THREAD Faults[MAX_THREADS];
    PKTHREAD Threads[MAX_THREADS];
    KAFFINITY Active = KeQueryActiveProcessors();
    ULONGLONG Deadline = KeQueryInterruptTime() + RUN_TIME_MS * 10000ULL;
    ULONG Count = 0, i, Processor;

    RtlZeroMemory(Faults, sizeof(Faults));
    for (Processor = 0; Processor < sizeof(KAFFINITY) * 8 && Count + THREADS_PER_PROCESSOR <= MAX_THREADS; Processor++)
    {
        if (!(Active & ((KAFFINITY)1 << (Processor))))
            continue;

        for (i = 0; i < THREADS_PER_PROCESSOR; i++)
        {
            HANDLE Handle;
            NTSTATUS Status;

            Faults[Count].Affinity = ((KAFFINITY)1 << (Processor));
            Faults[Count].Waker = (i == 0);
            Faults[Count].Deadline = Deadline;
            Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, NULL, NULL, NULL, FaultThread, &Faults[Count]);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (!NT_SUCCESS(Status))
                continue;

            Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&Threads[Count], NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ZwClose(Handle);
            if (NT_SUCCESS(Status))
                Count++;
        }
    }

    for (i = 0; i < Count; i++)
    {
        KeWaitForSingleObject(Threads[i], Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Threads[i]);

        ok(NT_SUCCESS(Faults[i].Status), "Thread %lu: status 0x%lx\n", i, Faults[i].Status);
        ok(Faults[i].Faults != 0, "Thread %lu took no faults\n", i);
        ok(Faults[i].WrongProcessor == 0, "Thread %lu ran off its processor %lu times\n", i, Faults[i].WrongProcessor);
        trace("Thread %lu (affinity 0x%Ix): %lu faults\n", i, Faults[i].Affinity, Faults[i].Faults);
    }
}
