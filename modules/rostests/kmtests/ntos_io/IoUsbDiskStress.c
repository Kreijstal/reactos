/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite: concurrent raw I/O to a USB disk
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>
#include <ntddstor.h>
#include <ntdddisk.h>

/*
 * Several system threads write a pattern to their own region of a scratch
 * USB disk and read it back, concurrently, with transfers of up to the
 * 1 MB usbstor accepts. A request the storage stack loses shows up as an
 * I/O that does not complete within IO_TIMEOUT_S; a request answered with
 * the wrong data shows up in the compare.
 *
 * Phase 1 uses transfers of at most 48 pages (no scatter/gather list longer
 * than 48 elements whatever the page layout). Phase 2 uses transfers of 49
 * to 256 pages from buffers whose pages are in descending physical order, so
 * the host controller gets one scatter/gather element per page.
 *
 * Only runs with a disk whose bus type is USB and whose first sectors are
 * zero (an unformatted scratch disk); it writes from 1 MB on.
 */

#define MAX_THREADS     8
#define MAX_PAGES       256
#define REGION_SIZE     (32 * 1024 * 1024)
#define REGION_BASE     (1024 * 1024)
#define IO_TIMEOUT_S    30

typedef struct _STRESS_THREAD
{
    HANDLE Disk;
    ULONG Index;
    ULONG MinPages, MaxPages;
    BOOLEAN Descending;
    LARGE_INTEGER EndTime;
    /* results */
    ULONG Ios, TimedOut, Failed, Mismatches;
    ULONGLONG Bytes;
    NTSTATUS LastStatus;
    ULONG LastLength;
    ULONGLONG LastOffset;
    /* Not on the stack: an I/O that timed out may still complete into it */
    IO_STATUS_BLOCK IoStatusBlock;
} STRESS_THREAD, *PSTRESS_THREAD;

/* Sort a PFN array in descending order (no two neighbours contiguous) */
static
VOID
SortPfnsDescending(
    _Inout_updates_(Count) PPFN_NUMBER Pfns,
    _In_ ULONG Count)
{
    ULONG i, j;

    for (i = 1; i < Count; i++)
    {
        PFN_NUMBER Pfn = Pfns[i];

        for (j = i; j > 0 && Pfns[j - 1] < Pfn; j--)
            Pfns[j] = Pfns[j - 1];
        Pfns[j] = Pfn;
    }
}

/* MAX_PAGES pages mapped in system space, optionally in descending physical order */
static
PVOID
AllocateBuffer(
    _In_ BOOLEAN Descending,
    _Out_ PMDL *PagesMdl)
{
    PHYSICAL_ADDRESS Low, High, Skip;
    PMDL Mdl;
    PVOID Va;

    *PagesMdl = NULL;
    Low.QuadPart = 0;
    High.QuadPart = -1;
    Skip.QuadPart = 0;
    Mdl = MmAllocatePagesForMdl(Low, High, Skip, MAX_PAGES * PAGE_SIZE);
    if (!Mdl)
        return NULL;
    if (MmGetMdlByteCount(Mdl) != MAX_PAGES * PAGE_SIZE)
    {
        MmFreePagesFromMdl(Mdl);
        ExFreePool(Mdl);
        return NULL;
    }
    if (Descending)
        SortPfnsDescending(MmGetMdlPfnArray(Mdl), MAX_PAGES);

    Va = MmMapLockedPagesSpecifyCache(Mdl, KernelMode, MmCached, NULL, FALSE, NormalPagePriority);
    if (!Va)
    {
        MmFreePagesFromMdl(Mdl);
        ExFreePool(Mdl);
        return NULL;
    }
    *PagesMdl = Mdl;
    return Va;
}

static
VOID
FreeBuffer(
    _In_ PVOID Va,
    _In_ PMDL Mdl)
{
    MmUnmapLockedPages(Va, Mdl);
    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);
}

/* Returns FALSE if the I/O did not complete in time (its buffer stays in use) */
static
BOOLEAN
DoIo(
    _In_ PSTRESS_THREAD Thread,
    _In_ BOOLEAN Write,
    _In_ PVOID Buffer,
    _In_ ULONG Length,
    _In_ ULONGLONG Offset,
    _In_ HANDLE Event,
    _Out_ PNTSTATUS IoStatus)
{
    PIO_STATUS_BLOCK IoStatusBlock = &Thread->IoStatusBlock;
    LARGE_INTEGER ByteOffset, Timeout;
    NTSTATUS Status;

    ByteOffset.QuadPart = Offset;
    ZwClearEvent(Event);
    RtlZeroMemory(IoStatusBlock, sizeof(*IoStatusBlock));
    if (Write)
        Status = ZwWriteFile(Thread->Disk, Event, NULL, NULL, IoStatusBlock, Buffer, Length, &ByteOffset, NULL);
    else
        Status = ZwReadFile(Thread->Disk, Event, NULL, NULL, IoStatusBlock, Buffer, Length, &ByteOffset, NULL);
    if (Status == STATUS_PENDING)
    {
        Timeout.QuadPart = -(LONGLONG)IO_TIMEOUT_S * 10 * 1000 * 1000;
        Status = ZwWaitForSingleObject(Event, FALSE, &Timeout);
        if (Status == STATUS_TIMEOUT)
        {
            Thread->LastLength = Length;
            Thread->LastOffset = Offset;
            *IoStatus = STATUS_TIMEOUT;
            return FALSE;
        }
        Status = IoStatusBlock->Status;
    }
    *IoStatus = Status;
    return TRUE;
}

static
VOID
NTAPI
StressThread(
    _In_ PVOID Context)
{
    PSTRESS_THREAD Thread = Context;
    PMDL WriteMdl = NULL, ReadMdl = NULL;
    PULONG WriteVa, ReadVa;
    ULONG Seed = 0x2400 + Thread->Index * 7919, Sequence = 0;
    LARGE_INTEGER Now;
    OBJECT_ATTRIBUTES EventAttributes;
    HANDLE Event = NULL;
    NTSTATUS Status;

    InitializeObjectAttributes(&EventAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, &EventAttributes, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
    {
        Thread->LastStatus = Status;
        Thread->Failed++;
        PsTerminateSystemThread(STATUS_SUCCESS);
    }
    WriteVa = AllocateBuffer(Thread->Descending, &WriteMdl);
    ReadVa = AllocateBuffer(Thread->Descending, &ReadMdl);
    if (!WriteVa || !ReadVa)
    {
        Thread->LastStatus = STATUS_INSUFFICIENT_RESOURCES;
        Thread->Failed++;
        goto Done;
    }

    for (;;)
    {
        ULONG Pages, Length, Words, Word, Bad;
        ULONGLONG Offset;

        KeQuerySystemTime(&Now);
        if (Now.QuadPart >= Thread->EndTime.QuadPart)
            break;

        Pages = Thread->MinPages + RtlRandomEx(&Seed) % (Thread->MaxPages - Thread->MinPages + 1);
        Length = Pages * PAGE_SIZE;
        Offset = REGION_BASE + (ULONGLONG)Thread->Index * REGION_SIZE +
                 (ULONGLONG)(RtlRandomEx(&Seed) % ((REGION_SIZE - Length) / 512 + 1)) * 512;
        Words = Length / sizeof(ULONG);
        Sequence++;
        for (Word = 0; Word < Words; Word++)
            WriteVa[Word] = (Thread->Index << 28) ^ (Sequence << 12) ^ Word;
        RtlFillMemory(ReadVa, Length, 0xA5);

        if (!DoIo(Thread, TRUE, WriteVa, Length, Offset, Event, &Status))
        {
            /* The write never completed: its buffer may still be used */
            Thread->TimedOut++;
            WriteVa = NULL;
            break;
        }
        if (!NT_SUCCESS(Status))
        {
            Thread->LastStatus = Status;
            Thread->LastLength = Length;
            Thread->LastOffset = Offset;
            Thread->Failed++;
            continue;
        }
        if (!DoIo(Thread, FALSE, ReadVa, Length, Offset, Event, &Status))
        {
            Thread->TimedOut++;
            ReadVa = NULL;
            break;
        }
        if (!NT_SUCCESS(Status))
        {
            Thread->LastStatus = Status;
            Thread->LastLength = Length;
            Thread->LastOffset = Offset;
            Thread->Failed++;
            continue;
        }
        Bad = 0;
        for (Word = 0; Word < Words; Word++)
        {
            if (ReadVa[Word] != WriteVa[Word])
                Bad++;
        }
        if (Bad)
        {
            Thread->Mismatches++;
            Thread->LastLength = Length;
            Thread->LastOffset = Offset;
        }
        Thread->Ios += 2;
        Thread->Bytes += 2ULL * Length;
    }

Done:
    /* A buffer of an I/O that never completed is leaked on purpose */
    if (WriteVa)
        FreeBuffer(WriteVa, WriteMdl);
    if (ReadVa)
        FreeBuffer(ReadVa, ReadMdl);
    ZwClose(Event);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
HANDLE
OpenUsbScratchDisk(VOID)
{
    ULONG Disk;

    for (Disk = 0; Disk < 16; Disk++)
    {
        WCHAR NameBuffer[64];
        UNICODE_STRING Name;
        OBJECT_ATTRIBUTES ObjectAttributes;
        IO_STATUS_BLOCK IoStatus;
        STORAGE_PROPERTY_QUERY Query;
        UCHAR Descriptor[512];
        GET_LENGTH_INFORMATION LengthInfo;
        LARGE_INTEGER Offset;
        PULONG Sector;
        HANDLE Handle;
        NTSTATUS Status;
        ULONG i;
        BOOLEAN Blank = TRUE;

        RtlStringCbPrintfW(NameBuffer, sizeof(NameBuffer), L"\\Device\\Harddisk%lu\\Partition0", Disk);
        RtlInitUnicodeString(&Name, NameBuffer);
        InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        Status = ZwOpenFile(&Handle, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &ObjectAttributes, &IoStatus,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
        if (!NT_SUCCESS(Status))
            continue;

        RtlZeroMemory(&Query, sizeof(Query));
        Query.PropertyId = StorageDeviceProperty;
        Query.QueryType = PropertyStandardQuery;
        RtlZeroMemory(Descriptor, sizeof(Descriptor));
        Status = ZwDeviceIoControlFile(Handle, NULL, NULL, NULL, &IoStatus, IOCTL_STORAGE_QUERY_PROPERTY,
                                       &Query, sizeof(Query), Descriptor, sizeof(Descriptor));
        if (!NT_SUCCESS(Status) || ((PSTORAGE_DEVICE_DESCRIPTOR)Descriptor)->BusType != BusTypeUsb)
        {
            ZwClose(Handle);
            continue;
        }

        Status = ZwDeviceIoControlFile(Handle, NULL, NULL, NULL, &IoStatus, IOCTL_DISK_GET_LENGTH_INFO,
                                       NULL, 0, &LengthInfo, sizeof(LengthInfo));
        if (!NT_SUCCESS(Status) ||
            LengthInfo.Length.QuadPart < REGION_BASE + (LONGLONG)MAX_THREADS * REGION_SIZE)
        {
            trace("USB disk %lu is too small (0x%lx)\n", Disk, Status);
            ZwClose(Handle);
            continue;
        }

        /* Only a blank scratch disk: its first 64 KB must be zero */
        Sector = ExAllocatePoolWithTag(NonPagedPool, 0x10000, 'SUmK');
        if (!Sector)
        {
            ZwClose(Handle);
            continue;
        }
        Offset.QuadPart = 0;
        Status = ZwReadFile(Handle, NULL, NULL, NULL, &IoStatus, Sector, 0x10000, &Offset, NULL);
        for (i = 0; NT_SUCCESS(Status) && i < 0x10000 / sizeof(ULONG); i++)
        {
            if (Sector[i] != 0)
                Blank = FALSE;
        }
        ExFreePoolWithTag(Sector, 'SUmK');
        if (!NT_SUCCESS(Status) || !Blank)
        {
            trace("USB disk %lu is not a blank scratch disk (0x%lx)\n", Disk, Status);
            ZwClose(Handle);
            continue;
        }

        ZwClose(Handle);

        /* Reopen for asynchronous I/O, so a lost request can be timed out */
        Status = ZwOpenFile(&Handle, GENERIC_READ | GENERIC_WRITE, &ObjectAttributes, &IoStatus,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
        if (!NT_SUCCESS(Status))
            continue;
        trace("Using USB disk %lu, 0x%I64x bytes\n", Disk, LengthInfo.Length.QuadPart);
        return Handle;
    }
    return NULL;
}

static
BOOLEAN
RunPhase(
    _In_ PCSTR Name,
    _In_ HANDLE Disk,
    _In_ ULONG ThreadCount,
    _In_ ULONG MinPages,
    _In_ ULONG MaxPages,
    _In_ BOOLEAN Descending,
    _In_ ULONG Seconds)
{
    static STRESS_THREAD Threads[MAX_THREADS];
    PKTHREAD ThreadObjects[MAX_THREADS];
    KWAIT_BLOCK WaitBlocks[MAX_THREADS];
    LARGE_INTEGER EndTime, Timeout;
    ULONG i, Started = 0, Ios = 0, TimedOut = 0, Failed = 0, Mismatches = 0;
    NTSTATUS FailStatus = STATUS_SUCCESS;
    ULONG FailLength = 0;
    ULONGLONG FailOffset = 0;
    ULONGLONG Bytes = 0;
    NTSTATUS Status;

    KeQuerySystemTime(&EndTime);
    EndTime.QuadPart += (LONGLONG)Seconds * 10 * 1000 * 1000;

    for (i = 0; i < ThreadCount; i++)
    {
        HANDLE ThreadHandle;

        RtlZeroMemory(&Threads[i], sizeof(Threads[i]));
        Threads[i].Disk = Disk;
        Threads[i].Index = i;
        Threads[i].MinPages = MinPages;
        Threads[i].MaxPages = MaxPages;
        Threads[i].Descending = Descending;
        Threads[i].EndTime = EndTime;
        Status = PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, StressThread, &Threads[i]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            break;
        Status = ObReferenceObjectByHandle(ThreadHandle, SYNCHRONIZE, *PsThreadType, KernelMode,
                                           (PVOID *)&ThreadObjects[Started], NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(ThreadHandle);
        if (NT_SUCCESS(Status))
            Started++;
    }

    /* Every I/O either completes or times out after IO_TIMEOUT_S */
    Timeout.QuadPart = -((LONGLONG)Seconds + 2 * IO_TIMEOUT_S + 10) * 10 * 1000 * 1000;
    Status = KeWaitForMultipleObjects(Started, (PVOID *)ThreadObjects, WaitAll, Executive, KernelMode,
                                      FALSE, &Timeout, WaitBlocks);
    ok(Status == STATUS_SUCCESS, "%s: the stress threads did not finish (0x%lx)\n", Name, Status);
    if (Status != STATUS_SUCCESS)
    {
        /* Their contexts stay in use: leave them (static) alone */
        return FALSE;
    }

    for (i = 0; i < Started; i++)
    {
        PSTRESS_THREAD Thread = &Threads[i];

        ObDereferenceObject(ThreadObjects[i]);
        Ios += Thread->Ios;
        Bytes += Thread->Bytes;
        TimedOut += Thread->TimedOut;
        Failed += Thread->Failed;
        Mismatches += Thread->Mismatches;
        if ((Thread->TimedOut || Thread->Failed || Thread->Mismatches) && FailLength == 0)
        {
            FailStatus = Thread->LastStatus;
            FailLength = Thread->LastLength;
            FailOffset = Thread->LastOffset;
        }
        if (Thread->TimedOut || Thread->Failed || Thread->Mismatches)
        {
            trace("%s: thread %lu: %lu timed out, %lu failed (0x%lx), %lu mismatches, last 0x%lx bytes at 0x%I64x\n",
                  Name, i, Thread->TimedOut, Thread->Failed, Thread->LastStatus, Thread->Mismatches,
                  Thread->LastLength, Thread->LastOffset);
        }
    }
    /* On the debug log too, so a run shows how much I/O it really did */
    DbgPrint("IoUsbDiskStress %s: %lu threads, %lu I/Os, %I64u MB, %lu timed out, %lu failed, %lu mismatches\n",
             Name, Started, Ios, Bytes >> 20, TimedOut, Failed, Mismatches);
    ok(TimedOut == 0, "%s: %lu I/Os did not complete within %u s (0x%lx bytes at 0x%I64x)\n",
       Name, TimedOut, IO_TIMEOUT_S, FailLength, FailOffset);
    ok(Failed == 0, "%s: %lu I/Os failed (0x%lx, 0x%lx bytes at 0x%I64x)\n",
       Name, Failed, FailStatus, FailLength, FailOffset);
    ok(Mismatches == 0, "%s: %lu reads returned other data than was written (0x%lx bytes at 0x%I64x)\n",
       Name, Mismatches, FailLength, FailOffset);
    ok(Ios != 0, "%s: no I/O completed\n", Name);
    return TimedOut == 0;
}

START_TEST(IoUsbDiskStress)
{
    HANDLE Disk;
    ULONG ThreadCount;

    Disk = OpenUsbScratchDisk();
    if (skip(Disk != NULL, "No blank USB scratch disk\n"))
        return;

    ThreadCount = min(MAX_THREADS, max(4, 2 * (ULONG)KeNumberProcessors));

    /* At most 48 pages: concurrency only */
    if (RunPhase("Short", Disk, ThreadCount, 1, 48, FALSE, 35))
    {
        /* 49 to 256 pages, one scatter/gather element per page */
        RunPhase("Fragmented", Disk, ThreadCount, 49, MAX_PAGES, TRUE, 20);
    }
    else
    {
        skip(FALSE, "The disk lost a request, not trying longer transfers\n");
    }

    ZwClose(Disk);
}
