/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Stress the section/COW/private-memory combination exercised by
 *              the cygwin gcc/ld toolchain workload.
 */

#include <kmt_test.h>

#define FILE_SIZE       (8 * 1024 * 1024)
#define WRITE_SIZE      (64 * 1024)
#define PRIVATE_SIZE    (16 * 1024 * 1024)
#define ITERATIONS      64
#define WORKER_COUNT    2

static volatile LONG StopWorkers;
static volatile LONG PoolOperations;

static const ULONG PoolTags[] =
{
    'PAMR', /* RMAP */
    ' ftN', /* Ntf  */
    'tSmM', /* MmSt */
    'SSMM'  /* MMSS */
};

static
VOID
NTAPI
PoolWorker(_In_ PVOID Context)
{
    ULONG Seed = (ULONG)(ULONG_PTR)Context + 1;
    PVOID Blocks[16];
    SIZE_T Sizes[16];
    ULONG Index;

    while (!InterlockedCompareExchange((PLONG)&StopWorkers, 0, 0))
    {
        RtlZeroMemory(Blocks, sizeof(Blocks));
        for (Index = 0; Index < RTL_NUMBER_OF(Blocks); Index++)
        {
            Seed = Seed * 1103515245 + 12345;
            Sizes[Index] = 32 + ((Seed >> 8) & 0x1fff);
            Blocks[Index] = ExAllocatePoolWithTag(NonPagedPool,
                                                   Sizes[Index],
                                                   PoolTags[Index % RTL_NUMBER_OF(PoolTags)]);
            if (Blocks[Index])
                RtlFillMemory(Blocks[Index], Sizes[Index], (UCHAR)Seed);
        }

        for (Index = 0; Index < RTL_NUMBER_OF(Blocks); Index++)
        {
            if (Blocks[Index])
                ExFreePoolWithTag(Blocks[Index],
                                  PoolTags[Index % RTL_NUMBER_OF(PoolTags)]);
        }
        InterlockedIncrement((PLONG)&PoolOperations);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
UCHAR
ExpectedByte(_In_ ULONG Iteration, _In_ SIZE_T Offset)
{
    return (UCHAR)(0x5a ^ Iteration ^ (Offset / WRITE_SIZE));
}

START_TEST(MmCygwinSectionStress)
{
    static const UNICODE_STRING FileName =
        RTL_CONSTANT_STRING(L"\\SystemRoot\\kmtest-cygwin-section.bin");
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER FileSize, Offset;
    HANDLE FileHandle = NULL, SectionHandle = NULL;
    PKTHREAD Workers[WORKER_COUNT] = { NULL };
    PUCHAR WriteBuffer = NULL;
    NTSTATUS Status;
    ULONG Iteration, Chunk, Page, Worker;
    ULONG Completed = 0, Mismatches = 0;

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)&FileName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = ZwCreateFile(&FileHandle,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OVERWRITE_IF,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                              FILE_DELETE_ON_CLOSE,
                          NULL,
                          0);
    if (skip(NT_SUCCESS(Status), "ZwCreateFile failed: 0x%08lx\n", Status))
        goto Cleanup;

    WriteBuffer = ExAllocatePoolWithTag(PagedPool, WRITE_SIZE, 'wSyC');
    if (skip(WriteBuffer != NULL, "Could not allocate write buffer\n"))
        goto Cleanup;

    StopWorkers = 0;
    PoolOperations = 0;
    for (Worker = 0; Worker < WORKER_COUNT; Worker++)
        Workers[Worker] = KmtStartThread(PoolWorker, (PVOID)(ULONG_PTR)Worker);

    for (Iteration = 0; Iteration < ITERATIONS; Iteration++)
    {
        PVOID View = NULL, Private = NULL;
        SIZE_T ViewSize = 0, PrivateSize = PRIVATE_SIZE;

        FileSize.QuadPart = FILE_SIZE;
        Status = ZwSetInformationFile(FileHandle,
                                      &IoStatus,
                                      &FileSize,
                                      sizeof(FileSize),
                                      FileEndOfFileInformation);
        if (!NT_SUCCESS(Status))
        {
            ok(FALSE, "iteration %lu: resize failed: 0x%08lx\n", Iteration, Status);
            break;
        }

        for (Chunk = 0; Chunk < FILE_SIZE / WRITE_SIZE; Chunk++)
        {
            Offset.QuadPart = (LONGLONG)Chunk * WRITE_SIZE;
            RtlFillMemory(WriteBuffer,
                          WRITE_SIZE,
                          ExpectedByte(Iteration, (SIZE_T)Offset.QuadPart));
            Status = ZwWriteFile(FileHandle,
                                 NULL,
                                 NULL,
                                 NULL,
                                 &IoStatus,
                                 WriteBuffer,
                                 WRITE_SIZE,
                                 &Offset,
                                 NULL);
            if (!NT_SUCCESS(Status))
                break;
        }
        if (!NT_SUCCESS(Status))
        {
            ok(FALSE, "iteration %lu: write failed: 0x%08lx\n", Iteration, Status);
            break;
        }

        FileSize.QuadPart = FILE_SIZE;
        Status = ZwCreateSection(&SectionHandle,
                                 SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
                                 NULL,
                                 &FileSize,
                                 PAGE_WRITECOPY,
                                 SEC_COMMIT,
                                 FileHandle);
        if (!NT_SUCCESS(Status))
        {
            ok(FALSE, "iteration %lu: section create failed: 0x%08lx\n", Iteration, Status);
            break;
        }

        Status = ZwMapViewOfSection(SectionHandle,
                                    NtCurrentProcess(),
                                    &View,
                                    0,
                                    0,
                                    NULL,
                                    &ViewSize,
                                    ViewUnmap,
                                    0,
                                    PAGE_WRITECOPY);
        if (!NT_SUCCESS(Status))
        {
            ok(FALSE, "iteration %lu: map failed: 0x%08lx\n", Iteration, Status);
            ZwClose(SectionHandle);
            SectionHandle = NULL;
            break;
        }

        /* Read faults exercise MmNotPresentFaultSectionView; writes force the
         * MiCopyFromUserPage COW path.  Keep all COW pages resident until the
         * second verification pass. */
        for (Page = 0; Page < FILE_SIZE / PAGE_SIZE; Page++)
        {
            SIZE_T Position = (SIZE_T)Page * PAGE_SIZE;
            UCHAR Expected = ExpectedByte(Iteration, Position);
            if (((volatile UCHAR *)View)[Position] != Expected)
                Mismatches++;
            ((volatile UCHAR *)View)[Position + 7] = (UCHAR)(Expected ^ 0xa5);
        }

        Status = ZwAllocateVirtualMemory(NtCurrentProcess(),
                                         &Private,
                                         0,
                                         &PrivateSize,
                                         MEM_RESERVE | MEM_COMMIT,
                                         PAGE_READWRITE);
        if (NT_SUCCESS(Status))
        {
            for (Page = 0; Page < PRIVATE_SIZE / PAGE_SIZE; Page++)
                ((volatile UCHAR *)Private)[(SIZE_T)Page * PAGE_SIZE] = (UCHAR)(Page ^ Iteration);
            for (Page = 0; Page < PRIVATE_SIZE / PAGE_SIZE; Page++)
            {
                if (((volatile UCHAR *)Private)[(SIZE_T)Page * PAGE_SIZE] != (UCHAR)(Page ^ Iteration))
                    Mismatches++;
            }
        }
        else
        {
            ok(FALSE, "iteration %lu: private allocation failed: 0x%08lx\n", Iteration, Status);
        }

        for (Page = 0; Page < FILE_SIZE / PAGE_SIZE; Page++)
        {
            SIZE_T Position = (SIZE_T)Page * PAGE_SIZE;
            UCHAR Expected = ExpectedByte(Iteration, Position);
            if (((volatile UCHAR *)View)[Position] != Expected ||
                ((volatile UCHAR *)View)[Position + 7] != (UCHAR)(Expected ^ 0xa5))
                Mismatches++;
        }

        if (Private)
        {
            PrivateSize = 0;
            ZwFreeVirtualMemory(NtCurrentProcess(), &Private, &PrivateSize, MEM_RELEASE);
        }
        ZwUnmapViewOfSection(NtCurrentProcess(), View);
        ZwClose(SectionHandle);
        SectionHandle = NULL;
        Completed++;

        if (Mismatches)
        {
            ok(FALSE, "iteration %lu: detected %lu memory mismatches\n",
               Iteration, Mismatches);
            break;
        }
    }

Cleanup:
    InterlockedExchange((PLONG)&StopWorkers, 1);
    for (Worker = 0; Worker < WORKER_COUNT; Worker++)
    {
        if (Workers[Worker])
            KmtFinishThread(Workers[Worker], NULL);
    }
    if (SectionHandle)
        ZwClose(SectionHandle);
    if (WriteBuffer)
        ExFreePoolWithTag(WriteBuffer, 'wSyC');
    if (FileHandle)
        ZwClose(FileHandle);

    trace("completed=%lu/%u mismatches=%lu poolOperations=%ld\n",
          Completed, ITERATIONS, Mismatches, PoolOperations);
    ok_eq_ulong(Completed, ITERATIONS);
    ok_eq_ulong(Mismatches, 0);
    ok(PoolOperations > 0, "pool worker made no progress\n");
}
