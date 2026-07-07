/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     USB mass-storage read-progress reproducer
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 */

#include <kmt_test.h>

#define USBSTOR_READ_CHUNK_SIZE     (64 * 1024)
#define USBSTOR_READ_TARGET_BYTES   (256ULL * 1024 * 1024)
#define USBSTOR_READ_PROGRESS_STEP  (16ULL * 1024 * 1024)
#define USBSTOR_READ_TAG            'RtSU'

static const PCWSTR UsbStorageCandidatePaths[] =
{
    L"\\??\\C:\\msys64\\var\\cache\\pacman\\pkg\\mingw-w64-ucrt-x86_64-gettext-runtime-1.0-1-any.pkg.tar.zst",
    L"\\??\\D:\\msys64\\var\\cache\\pacman\\pkg\\mingw-w64-ucrt-x86_64-gettext-runtime-1.0-1-any.pkg.tar.zst",
    L"\\??\\E:\\msys64\\var\\cache\\pacman\\pkg\\mingw-w64-ucrt-x86_64-gettext-runtime-1.0-1-any.pkg.tar.zst",
    L"\\??\\F:\\msys64\\var\\cache\\pacman\\pkg\\mingw-w64-ucrt-x86_64-gettext-runtime-1.0-1-any.pkg.tar.zst",
    L"\\??\\G:\\msys64\\var\\cache\\pacman\\pkg\\mingw-w64-ucrt-x86_64-gettext-runtime-1.0-1-any.pkg.tar.zst",
    L"\\??\\H:\\msys64\\var\\cache\\pacman\\pkg\\mingw-w64-ucrt-x86_64-gettext-runtime-1.0-1-any.pkg.tar.zst",
    L"\\??\\C:\\msys64\\usr\\bin\\python3.exe",
    L"\\??\\D:\\msys64\\usr\\bin\\python3.exe",
    L"\\??\\E:\\msys64\\usr\\bin\\python3.exe",
    L"\\??\\F:\\msys64\\usr\\bin\\python3.exe",
    L"\\??\\G:\\msys64\\usr\\bin\\python3.exe",
    L"\\??\\H:\\msys64\\usr\\bin\\python3.exe",
    L"\\??\\C:\\msys64\\usr\\bin\\bash.exe",
    L"\\??\\D:\\msys64\\usr\\bin\\bash.exe",
    L"\\??\\E:\\msys64\\usr\\bin\\bash.exe",
    L"\\??\\F:\\msys64\\usr\\bin\\bash.exe",
    L"\\??\\G:\\msys64\\usr\\bin\\bash.exe",
    L"\\??\\H:\\msys64\\usr\\bin\\bash.exe"
};

static
NTSTATUS
OpenCandidateFile(
    _Out_ PHANDLE FileHandle,
    _Out_ PLARGE_INTEGER FileSize,
    _Out_ PCWSTR *MatchedPath)
{
    NTSTATUS Status;
    ULONG i;

    *FileHandle = NULL;
    FileSize->QuadPart = 0;
    *MatchedPath = NULL;

    for (i = 0; i < RTL_NUMBER_OF(UsbStorageCandidatePaths); ++i)
    {
        UNICODE_STRING Name;
        OBJECT_ATTRIBUTES ObjectAttributes;
        IO_STATUS_BLOCK IoStatus;
        FILE_STANDARD_INFORMATION StandardInfo;

        RtlInitUnicodeString(&Name, UsbStorageCandidatePaths[i]);
        InitializeObjectAttributes(&ObjectAttributes,
                                   &Name,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   NULL,
                                   NULL);

        DbgPrint("USBSTORREAD-PROBE-BEGIN: %ls\n", UsbStorageCandidatePaths[i]);
        Status = ZwOpenFile(FileHandle,
                            FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                            &ObjectAttributes,
                            &IoStatus,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
        DbgPrint("USBSTORREAD-PROBE-END: %ls status=0x%lx\n",
                 UsbStorageCandidatePaths[i], Status);
        trace("USBSTORREAD: probe %ls status=0x%lx\n",
              UsbStorageCandidatePaths[i], Status);
        if (!NT_SUCCESS(Status))
            continue;

        Status = ZwQueryInformationFile(*FileHandle,
                                        &IoStatus,
                                        &StandardInfo,
                                        sizeof(StandardInfo),
                                        FileStandardInformation);
        trace("USBSTORREAD: query %ls status=0x%lx eof=%I64d\n",
              UsbStorageCandidatePaths[i],
              Status,
              NT_SUCCESS(Status) ? StandardInfo.EndOfFile.QuadPart : -1);
        DbgPrint("USBSTORREAD-QUERY: %ls status=0x%lx eof=%I64d\n",
                 UsbStorageCandidatePaths[i],
                 Status,
                 NT_SUCCESS(Status) ? StandardInfo.EndOfFile.QuadPart : -1);
        if (!NT_SUCCESS(Status) || StandardInfo.EndOfFile.QuadPart <= 0)
        {
            ZwClose(*FileHandle);
            *FileHandle = NULL;
            continue;
        }

        *FileSize = StandardInfo.EndOfFile;
        *MatchedPath = UsbStorageCandidatePaths[i];
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_FOUND;
}

START_TEST(UsbStorageRead)
{
    NTSTATUS Status;
    HANDLE FileHandle;
    LARGE_INTEGER FileSize;
    PCWSTR Path;
    PUCHAR Buffer;
    ULONGLONG BytesRead = 0;
    ULONGLONG NextProgress = USBSTOR_READ_PROGRESS_STEP;
    LARGE_INTEGER StartCounter, EndCounter, Frequency;
    LONGLONG ElapsedMs;

    DbgPrint("USBSTORREAD-OPEN-CANDIDATES\n");
    Status = OpenCandidateFile(&FileHandle, &FileSize, &Path);
    if (skip(NT_SUCCESS(Status),
             "No MSYS USB storage fixture found (expected C:-H:\\msys64\\usr\\bin\\python3.exe or bash.exe)\n"))
    {
        return;
    }

    Buffer = ExAllocatePoolWithTag(NonPagedPool, USBSTOR_READ_CHUNK_SIZE, USBSTOR_READ_TAG);
    if (skip(Buffer != NULL,
             "Failed to allocate %u-byte read buffer\n",
             (ULONG)USBSTOR_READ_CHUNK_SIZE))
    {
        ZwClose(FileHandle);
        return;
    }

    trace("USBSTORREAD: starting %I64u-byte read workload from %ls size=%I64d\n",
          USBSTOR_READ_TARGET_BYTES, Path, FileSize.QuadPart);
    DbgPrint("USBSTORREAD-START: path=%ls size=%I64d target=%I64u\n",
             Path, FileSize.QuadPart, USBSTOR_READ_TARGET_BYTES);

    StartCounter = KeQueryPerformanceCounter(&Frequency);

    while (BytesRead < USBSTOR_READ_TARGET_BYTES)
    {
        LARGE_INTEGER ByteOffset;
        ULONGLONG RemainingInFile;
        ULONG ThisChunk;
        IO_STATUS_BLOCK IoStatus;

        ByteOffset.QuadPart = BytesRead % FileSize.QuadPart;
        RemainingInFile = FileSize.QuadPart - ByteOffset.QuadPart;
        ThisChunk = (RemainingInFile < USBSTOR_READ_CHUNK_SIZE)
                    ? (ULONG)RemainingInFile
                    : USBSTOR_READ_CHUNK_SIZE;

        Status = ZwReadFile(FileHandle,
                            NULL,
                            NULL,
                            NULL,
                            &IoStatus,
                            Buffer,
                            ThisChunk,
                            &ByteOffset,
                            NULL);
        ok(NT_SUCCESS(Status), "ZwReadFile failed at byte %I64u offset %I64d: 0x%lx\n",
           BytesRead, ByteOffset.QuadPart, Status);
        if (!NT_SUCCESS(Status))
            break;

        ok(IoStatus.Information == ThisChunk,
           "Short read at byte %I64u offset %I64d: got %Iu expected %lu\n",
           BytesRead, ByteOffset.QuadPart, IoStatus.Information, ThisChunk);
        if (IoStatus.Information == 0)
            break;

        BytesRead += IoStatus.Information;
        if (BytesRead >= NextProgress)
        {
            trace("USBSTORREAD: bytes=%I64u\n", BytesRead);
            DbgPrint("USBSTORREAD-PROGRESS: bytes=%I64u\n", BytesRead);
            do
            {
                NextProgress += USBSTOR_READ_PROGRESS_STEP;
            } while (BytesRead >= NextProgress);
        }
    }

    EndCounter = KeQueryPerformanceCounter(NULL);
    ElapsedMs = ((EndCounter.QuadPart - StartCounter.QuadPart) * 1000)
                / Frequency.QuadPart;

    trace("USBSTORREAD: read %I64u bytes in %I64d ms from %ls\n",
          BytesRead, ElapsedMs, Path);
    DbgPrint("USBSTORREAD-DONE: bytes=%I64u elapsed_ms=%I64d\n",
             BytesRead, ElapsedMs);

    ok(BytesRead >= USBSTOR_READ_TARGET_BYTES,
       "Read only %I64u of %I64u target bytes\n",
       BytesRead, USBSTOR_READ_TARGET_BYTES);

    ExFreePoolWithTag(Buffer, USBSTOR_READ_TAG);
    ZwClose(FileHandle);
}
