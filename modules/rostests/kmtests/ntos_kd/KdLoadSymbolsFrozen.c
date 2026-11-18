/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Symbol load reports while other processors hold the loaded module lock
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * A symbol load report (every driver load) enters the kernel debugger, which
 * freezes the other processors wherever they are. RtlPcToFileHeader, used by
 * every exception dispatch, holds PsLoadedModuleSpinLock, so a processor can be
 * frozen while owning it. The debugger must not wait for that lock (bug #95:
 * boot wedged in KdbpSymFindModule with every other processor frozen).
 * With the bug the system hangs here; the harness reports the stall.
 */

#include <kmt_test.h>

/* Exported on 64 bit only */
typedef PVOID (NTAPI *PFN_RTL_PC_TO_FILE_HEADER)(PVOID, PVOID *);
static PFN_RTL_PC_TO_FILE_HEADER pRtlPcToFileHeader;

NTSYSAPI
VOID
NTAPI
DbgLoadImageSymbols(
    _In_ PSTRING Name,
    _In_ PVOID Base,
    _In_ ULONG_PTR ProcessId);

#define TEST_REPORTS    2000

static volatile LONG StopHammer;
static volatile LONG HammerLoops;

static
VOID
NTAPI
HammerModuleLock(
    _In_ PVOID Context)
{
    KAFFINITY Affinity = (KAFFINITY)1 << (ULONG_PTR)Context;
    PVOID Base;

    KeSetSystemAffinityThread(Affinity);
    while (!StopHammer)
    {
        pRtlPcToFileHeader((PVOID)pRtlPcToFileHeader, &Base);
        InterlockedIncrement(&HammerLoops);
    }
    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(KdLoadSymbolsFrozen)
{
    ANSI_STRING Name = RTL_CONSTANT_STRING("\\SystemRoot\\kmtest_kdsym_fake.sys");
    PVOID Threads[MAXIMUM_PROCESSORS] = { 0 };
    ULONG ThreadCount = 0;
    HANDLE Handle;
    UNICODE_STRING RoutineName = RTL_CONSTANT_STRING(L"RtlPcToFileHeader");
    PVOID FakeImage;
    ULONG Cpu, i;
    NTSTATUS Status;

    if (KeNumberProcessors < 2)
    {
        skip(FALSE, "Needs at least two processors\n");
        return;
    }

    pRtlPcToFileHeader = (PFN_RTL_PC_TO_FILE_HEADER)MmGetSystemRoutineAddress(&RoutineName);
    if (skip(pRtlPcToFileHeader != NULL, "RtlPcToFileHeader is not exported\n"))
        return;

    /* Not an image (no MZ) and in no module: the debugger finds nothing to load */
    FakeImage = ExAllocatePoolZero(NonPagedPool, PAGE_SIZE, 'sdKT');
    if (skip(FakeImage != NULL, "Out of memory\n"))
        return;

    StopHammer = 0;
    HammerLoops = 0;
    for (Cpu = 1; Cpu < (ULONG)KeNumberProcessors && Cpu < RTL_NUMBER_OF(Threads); Cpu++)
    {
        Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                      HammerModuleLock, (PVOID)(ULONG_PTR)Cpu);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            break;
        Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, *PsThreadType, KernelMode,
                                           &Threads[ThreadCount], NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
        if (NT_SUCCESS(Status))
            ThreadCount++;
    }

    /* Let the other processors get going */
    while (HammerLoops < 1000)
        YieldProcessor();

    for (i = 0; i < TEST_REPORTS; i++)
        DbgLoadImageSymbols((PSTRING)&Name, FakeImage, 0);

    InterlockedExchange(&StopHammer, 1);
    for (i = 0; i < ThreadCount; i++)
    {
        KeWaitForSingleObject(Threads[i], Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Threads[i]);
    }

    ok(HammerLoops > 1000, "HammerLoops = %ld\n", HammerLoops);
    trace("%lu symbol load reports, %ld module lookups on %lu other processors\n",
          (ULONG)TEST_REPORTS, HammerLoops, ThreadCount);
    ExFreePoolWithTag(FakeImage, 'sdKT');
}
