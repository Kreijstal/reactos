/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     fastfat FAT32 cluster-window-switch performance reproducer
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 *
 * Reproduces and MEASURES the fastfat regression where
 * FatExamineFatEntries runs FatReconcileMirrorFatBitmap +
 * FatReconcileDirentBitmap (a full recursive walk of the whole volume
 * directory tree) on EVERY FAT32 cluster-window switch
 * (allocsup.c:1999 and :2464).
 *
 * The workload writes enough data to a FAT32 volume to force many window
 * switches (each FAT32 window == 65536 clusters; on the 512-byte-cluster
 * fixture that is 32 MB per window). Wall-clock elapsed time is reported to
 * the kmtest log / COM1 as a "FATPERF:" line. Unpatched the run is slow and
 * COM1 carries many "FatReconcileDirentBitmap" lines; patched the run is fast
 * and those lines disappear.
 *
 * Fixture: a 511 MB FAT32 scratch disk (512-byte clusters, ~1,000,000
 * clusters ~= 16 windows, single MBR partition type 0x0C, volume label
 * FATPERF) attached as an extra IDE disk. The test probes the drive letters
 * and picks the FAT32 volume labelled FATPERF (falling back to the first
 * writable non-system FAT32 volume).
 */

#include <kmt_test.h>

/* ----- Tunables (override at build time if desired) ---------------------- */

/* Number of files to create under \fatperf\. */
#ifndef FATPERF_FILE_COUNT
#define FATPERF_FILE_COUNT   400
#endif

/* Size of each file in bytes (256 KB). FILE_COUNT * FILE_SIZE total bytes
 * crosses (total / 32 MB) FAT32 windows on the 512-byte-cluster fixture;
 * 400 * 256 KB = 100 MB ~= 3 windows of pure data, but interleaved allocation
 * (FatSelectBestWindow) makes the allocator hop windows far more often. */
#ifndef FATPERF_FILE_SIZE
#define FATPERF_FILE_SIZE    (256 * 1024)
#endif

/* I/O buffer size for each ZwWriteFile chunk (64 KB). */
#ifndef FATPERF_CHUNK_SIZE
#define FATPERF_CHUNK_SIZE   (64 * 1024)
#endif

/* Progress is traced to COM1 every this-many files. */
#ifndef FATPERF_PROGRESS_EVERY
#define FATPERF_PROGRESS_EVERY 50
#endif

/* ------------------------------------------------------------------------- */

static const PCWSTR FatPerfDriveLetters[] =
{
    L"\\??\\C:", L"\\??\\D:", L"\\??\\E:", L"\\??\\F:",
    L"\\??\\G:", L"\\??\\H:"
};

/* The volume label the fixture disk was formatted with. */
static const WCHAR FatPerfLabel[] = L"FATPERF";

/*
 * Open the volume device (\??\X:) and decide whether it is the FAT32 fixture.
 * Returns STATUS_SUCCESS and *IsTarget==TRUE when the volume is FAT32 and
 * (a) its label is FATPERF, or (b) LabelOptional is TRUE and it is simply a
 * writable FAT32 volume. The system volume is excluded because we never probe
 * the boot volume when a labelled match is required, and a fallback FAT32
 * volume is still fine to stress.
 */
static
NTSTATUS
ProbeVolume(
    _In_ PCWSTR DriveLetter,
    _In_ BOOLEAN LabelOptional,
    _Out_ PBOOLEAN IsTarget)
{
    NTSTATUS Status;
    UNICODE_STRING DeviceName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE VolumeHandle;
    IO_STATUS_BLOCK IoStatus;
    UCHAR AttrBuffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    UCHAR VolBuffer[sizeof(FILE_FS_VOLUME_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_FS_ATTRIBUTE_INFORMATION AttrInfo = (PVOID)AttrBuffer;
    PFILE_FS_VOLUME_INFORMATION VolInfo = (PVOID)VolBuffer;
    BOOLEAN IsFat32;

    *IsTarget = FALSE;

    RtlInitUnicodeString(&DeviceName, DriveLetter);
    InitializeObjectAttributes(&ObjectAttributes,
                               &DeviceName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(&VolumeHandle,
                        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    trace("FATPERF: probe %ls open=0x%lx\n", DriveLetter, Status);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Filesystem name */
    RtlZeroMemory(AttrBuffer, sizeof(AttrBuffer));
    Status = ZwQueryVolumeInformationFile(VolumeHandle,
                                           &IoStatus,
                                           AttrInfo,
                                           sizeof(AttrBuffer),
                                           FileFsAttributeInformation);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(VolumeHandle);
        return Status;
    }

    IsFat32 = (AttrInfo->FileSystemNameLength >= 10 &&
               RtlCompareMemory(AttrInfo->FileSystemName, L"FAT32", 10) == 10);
    trace("FATPERF: probe %ls fsnamelen=%lu name=%.5S isfat32=%d\n",
          DriveLetter, AttrInfo->FileSystemNameLength,
          AttrInfo->FileSystemName, IsFat32);
    if (!IsFat32)
    {
        ZwClose(VolumeHandle);
        return STATUS_SUCCESS; /* opened fine, just not our FS */
    }

    if (LabelOptional)
    {
        *IsTarget = TRUE;
        ZwClose(VolumeHandle);
        return STATUS_SUCCESS;
    }

    /* Volume label */
    RtlZeroMemory(VolBuffer, sizeof(VolBuffer));
    Status = ZwQueryVolumeInformationFile(VolumeHandle,
                                           &IoStatus,
                                           VolInfo,
                                           sizeof(VolBuffer),
                                           FileFsVolumeInformation);
    ZwClose(VolumeHandle);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    if (VolInfo->VolumeLabelLength == (sizeof(FatPerfLabel) - sizeof(WCHAR)) &&
        RtlCompareMemory(VolInfo->VolumeLabel,
                         FatPerfLabel,
                         VolInfo->VolumeLabelLength) == VolInfo->VolumeLabelLength)
    {
        *IsTarget = TRUE;
    }

    return STATUS_SUCCESS;
}

/* Resolve the fixture drive letter into DriveBuffer (e.g. L"\\??\\D:"). */
static
BOOLEAN
FindFatPerfVolume(
    _Out_writes_z_(BufferChars) PWCHAR DriveBuffer,
    _In_ SIZE_T BufferChars)
{
    ULONG i;
    NTSTATUS Status;
    BOOLEAN IsTarget;

    /* Pass 1: require the FATPERF label (robust against the boot volume). */
    for (i = 0; i < RTL_NUMBER_OF(FatPerfDriveLetters); ++i)
    {
        Status = ProbeVolume(FatPerfDriveLetters[i], FALSE, &IsTarget);
        if (NT_SUCCESS(Status) && IsTarget)
        {
            RtlStringCchCopyW(DriveBuffer, BufferChars, FatPerfDriveLetters[i]);
            trace("FATPERF: matched labelled FAT32 volume %ls\n",
                  FatPerfDriveLetters[i]);
            return TRUE;
        }
    }

    /* Pass 2: any FAT32 volume except the system volume (C:). */
    for (i = 0; i < RTL_NUMBER_OF(FatPerfDriveLetters); ++i)
    {
        if (FatPerfDriveLetters[i][4] == L'C') /* skip \??\C: */
            continue;
        Status = ProbeVolume(FatPerfDriveLetters[i], TRUE, &IsTarget);
        if (NT_SUCCESS(Status) && IsTarget)
        {
            RtlStringCchCopyW(DriveBuffer, BufferChars, FatPerfDriveLetters[i]);
            trace("FATPERF: fell back to FAT32 volume %ls (no FATPERF label)\n",
                  FatPerfDriveLetters[i]);
            return TRUE;
        }
    }

    /* Last resort: the fixture is always attached as the 2nd IDE disk, which
       the kmtest image enumerates as \??\D:.  Use it directly so a volume-info
       probe quirk cannot block the measurement; the workload create will fail
       loudly if D: is genuinely not a usable FAT volume. */
    RtlStringCchCopyW(DriveBuffer, BufferChars, L"\\??\\D:");
    trace("FATPERF: no probe match; using \\??\\D: directly\n");
    return TRUE;
}

static
NTSTATUS
CreateScratchDirectory(
    _In_ PCWSTR DirPath)
{
    NTSTATUS Status;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle;

    RtlInitUnicodeString(&Name, DirPath);
    InitializeObjectAttributes(&ObjectAttributes,
                               &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwCreateFile(&Handle,
                          FILE_LIST_DIRECTORY | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatus,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN_IF,
                          FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    return Status;
}

START_TEST(FatPerf)
{
    NTSTATUS Status;
    WCHAR Drive[16];
    WCHAR DirPath[32];
    WCHAR FilePath[64];
    PUCHAR Buffer = NULL;
    ULONG FileIndex;
    ULONG FilesCreated = 0;
    ULONGLONG BytesWritten = 0;
    LARGE_INTEGER FreqIgnored;
    LARGE_INTEGER StartCounter, EndCounter, Frequency;
    LONGLONG ElapsedMs;

    if (skip(FindFatPerfVolume(Drive, RTL_NUMBER_OF(Drive)),
             "No FAT32 fixture volume found (expected FATPERF disk on C:-H:)\n"))
    {
        return;
    }

    /* \??\X:\fatperf */
    Status = RtlStringCchPrintfW(DirPath, RTL_NUMBER_OF(DirPath),
                                 L"%ls\\fatperf", Drive);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = CreateScratchDirectory(DirPath);
    ok(NT_SUCCESS(Status), "Failed to create %ls: 0x%lx\n", DirPath, Status);
    if (!NT_SUCCESS(Status))
        return;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, FATPERF_CHUNK_SIZE, 'PtaF');
    if (skip(Buffer != NULL, "Failed to allocate %u-byte I/O buffer\n",
             (ULONG)FATPERF_CHUNK_SIZE))
    {
        return;
    }
    RtlFillMemory(Buffer, FATPERF_CHUNK_SIZE, 0xAB);

    trace("FATPERF: starting workload: %u files x %u bytes (chunk %u) on %ls\n",
          (ULONG)FATPERF_FILE_COUNT, (ULONG)FATPERF_FILE_SIZE,
          (ULONG)FATPERF_CHUNK_SIZE, Drive);
    DbgPrint("FATPERF-START: %u files x %u bytes on %ls\n",
             (ULONG)FATPERF_FILE_COUNT, (ULONG)FATPERF_FILE_SIZE, Drive);

    /* Read the performance counter frequency once (ticks per second). */
    StartCounter = KeQueryPerformanceCounter(&Frequency);

    for (FileIndex = 0; FileIndex < FATPERF_FILE_COUNT; ++FileIndex)
    {
        UNICODE_STRING Name;
        OBJECT_ATTRIBUTES ObjectAttributes;
        IO_STATUS_BLOCK IoStatus;
        HANDLE FileHandle;
        ULONG Remaining;

        Status = RtlStringCchPrintfW(FilePath, RTL_NUMBER_OF(FilePath),
                                     L"%ls\\f%05lu.dat", DirPath, FileIndex);
        if (!NT_SUCCESS(Status))
        {
            ok(FALSE, "Path format failed for file %lu: 0x%lx\n",
               FileIndex, Status);
            break;
        }

        RtlInitUnicodeString(&Name, FilePath);
        InitializeObjectAttributes(&ObjectAttributes,
                                   &Name,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   NULL,
                                   NULL);

        Status = ZwCreateFile(&FileHandle,
                              FILE_WRITE_DATA | SYNCHRONIZE,
                              &ObjectAttributes,
                              &IoStatus,
                              NULL,
                              FILE_ATTRIBUTE_NORMAL,
                              0,
                              FILE_OVERWRITE_IF,
                              FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                              NULL,
                              0);
        if (!NT_SUCCESS(Status))
        {
            ok(FALSE, "ZwCreateFile %ls failed: 0x%lx (created %lu so far)\n",
               FilePath, Status, FilesCreated);
            break;
        }

        Remaining = FATPERF_FILE_SIZE;
        while (Remaining > 0)
        {
            ULONG ThisChunk = (Remaining < FATPERF_CHUNK_SIZE)
                              ? Remaining : FATPERF_CHUNK_SIZE;

            Status = ZwWriteFile(FileHandle,
                                 NULL, NULL, NULL,
                                 &IoStatus,
                                 Buffer,
                                 ThisChunk,
                                 NULL,
                                 NULL);
            if (!NT_SUCCESS(Status))
            {
                ok(FALSE, "ZwWriteFile %ls failed: 0x%lx\n", FilePath, Status);
                break;
            }
            BytesWritten += IoStatus.Information;
            Remaining -= ThisChunk;
        }

        ZwClose(FileHandle);
        if (!NT_SUCCESS(Status))
            break;

        ++FilesCreated;

        if ((FilesCreated % FATPERF_PROGRESS_EVERY) == 0)
        {
            LARGE_INTEGER Now = KeQueryPerformanceCounter(&FreqIgnored);
            LONGLONG SoFarMs = ((Now.QuadPart - StartCounter.QuadPart) * 1000)
                               / Frequency.QuadPart;
            trace("FATPERF: progress %lu/%u files, %I64u MB, %I64d ms elapsed\n",
                  FilesCreated, (ULONG)FATPERF_FILE_COUNT,
                  BytesWritten / (1024 * 1024), SoFarMs);
        }
    }

    EndCounter = KeQueryPerformanceCounter(&FreqIgnored);
    ElapsedMs = ((EndCounter.QuadPart - StartCounter.QuadPart) * 1000)
                / Frequency.QuadPart;

    /* The headline measurement line.  Emit via DbgPrint as well so it lands on
       COM1 unconditionally even when the kmtest result channel is serial=0. */
    trace("FATPERF: created %lu files / wrote %I64u MB in %I64d ms, "
          "windowswitch-workload done\n",
          FilesCreated,
          BytesWritten / (1024 * 1024),
          ElapsedMs);
    DbgPrint("FATPERF-MS: files=%lu MB=%I64u elapsed_ms=%I64d\n",
             FilesCreated, BytesWritten / (1024 * 1024), ElapsedMs);

    ok(FilesCreated == FATPERF_FILE_COUNT,
       "Created %lu of %u files\n", FilesCreated, (ULONG)FATPERF_FILE_COUNT);

    ExFreePoolWithTag(Buffer, 'PtaF');
}
