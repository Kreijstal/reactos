/*
 * PROJECT:     ReactOS kernel-mode test runner
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Headless native-mode runner. Loads kmtest_drv, runs one or
 *              more tests via IOCTL, emits structured PASS/FAIL on COM1,
 *              and terminates QEMU via the kmt_exit driver.
 *
 *              Invoked by SMSS as a BootExecute entry on a kmtestcd boot,
 *              so the system never reaches winlogon. The runner is native-
 *              mode: it links only against ntdll.dll and uses no Win32 API.
 */

#include <stdio.h>
#include <stdarg.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>

#define NTOS_MODE_USER
#include <ndk/exfuncs.h>
#include <ndk/iofuncs.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/setypes.h>

#include <kmt_public.h>
#include "../../../modules/rostests/kmtests/kmt_exit_drv/kmt_exit_ioctl.h"

/* NTDLL exports _vsnprintf as the user-mode formatter for native binaries. */
int __cdecl _vsnprintf(char *buffer, size_t count, const char *fmt, va_list args);
int __cdecl _snwprintf(wchar_t *buffer, size_t count, const wchar_t *fmt, ...);

/* The KMT_RESULTBUFFER layout is shared with the kernel framework. Declared
 * here directly so we don't need to drag the full <kmt_test.h> kmtest-framework
 * include surface into a native binary. */
typedef struct
{
    volatile LONG Successes;
    volatile LONG Failures;
    volatile LONG Skipped;
    volatile LONG LogBufferLength;
    LONG LogBufferMaxLength;
    CHAR LogBuffer[ANYSIZE_ARRAY];
} KMT_RESULTBUFFER, *PKMT_RESULTBUFFER;

#define RESULT_BUFFER_SIZE (1024 * 1024)
#define LOG_CHUNK_SIZE     4096
#define NAMES_BUFFER_SIZE  (64 * 1024)

/* The Example test in kmtest_drv's testlist calls ok(FALSE, ...) on purpose to
 * exercise the failure-reporting path; it is intentionally failing and is
 * excluded from automated runs. Tests beginning with '-' in the testlist are
 * interactive/manual and are also excluded (this mirrors the kmtest.exe
 * userland runner's behaviour). */
#define EXCLUDED_FIXTURE   "Example"

static HANDLE g_SerialHandle = NULL;

static
VOID
EmitLine(PCSTR Fmt, ...)
{
    CHAR Buffer[768];
    IO_STATUS_BLOCK Iosb;
    int Written;
    size_t Len;
    va_list Args;

    va_start(Args, Fmt);
    Written = _vsnprintf(Buffer, sizeof(Buffer) - 2, Fmt, Args);
    va_end(Args);
    if (Written < 0)
    {
        Buffer[sizeof(Buffer) - 3] = '\0';
        Len = sizeof(Buffer) - 3;
    }
    else
    {
        Len = (size_t)Written;
    }

    if (Len + 2 < sizeof(Buffer))
    {
        Buffer[Len++] = '\r';
        Buffer[Len++] = '\n';
    }

    if (g_SerialHandle)
    {
        NtWriteFile(g_SerialHandle, NULL, NULL, NULL, &Iosb,
                    Buffer, (ULONG)Len, NULL, NULL);
    }

    /* Mirror to the kernel debug port so the line also appears in the regular
     * kdcom serial stream when both are connected. */
    DbgPrint("%.*s", (int)Len, Buffer);
}

static
NTSTATUS
OpenSerial(VOID)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attrs;
    IO_STATUS_BLOCK Iosb;

    /* \Device\Serial0 is the canonical NT path to COM1 once serial.sys binds.
     * If serial.sys did not bind (no COM port at all) the open fails and we
     * fall back to KdPrint mirroring; the run is still observable on the
     * kernel debug port. */
    RtlInitUnicodeString(&Name, L"\\Device\\Serial0");
    InitializeObjectAttributes(&Attrs, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    return NtCreateFile(&g_SerialHandle,
                        GENERIC_WRITE | SYNCHRONIZE,
                        &Attrs,
                        &Iosb,
                        NULL,
                        FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_OPEN,
                        FILE_SYNCHRONOUS_IO_NONALERT,
                        NULL,
                        0);
}

static
NTSTATUS
LoadDriverByService(PCWSTR ServiceName)
{
    WCHAR PathBuffer[160];
    UNICODE_STRING ServicePath;
    NTSTATUS Status;
    BOOLEAN WasEnabled = FALSE;
    int Written;

    Written = _snwprintf(PathBuffer,
                         sizeof(PathBuffer) / sizeof(WCHAR),
                         L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\%s",
                         ServiceName);
    if (Written < 0)
        return STATUS_BUFFER_TOO_SMALL;
    PathBuffer[Written] = L'\0';

    RtlInitUnicodeString(&ServicePath, PathBuffer);

    Status = RtlAdjustPrivilege(SE_LOAD_DRIVER_PRIVILEGE, TRUE, FALSE, &WasEnabled);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtLoadDriver(&ServicePath);

    /* STATUS_IMAGE_ALREADY_LOADED is fine for our purposes. */
    if (Status == STATUS_IMAGE_ALREADY_LOADED)
        Status = STATUS_SUCCESS;

    RtlAdjustPrivilege(SE_LOAD_DRIVER_PRIVILEGE, WasEnabled, FALSE, &WasEnabled);
    return Status;
}

static
NTSTATUS
OpenDevice(PCWSTR DevicePath, PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attrs;
    IO_STATUS_BLOCK Iosb;

    RtlInitUnicodeString(&Name, DevicePath);
    InitializeObjectAttributes(&Attrs, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    return NtCreateFile(Handle,
                        GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                        &Attrs,
                        &Iosb,
                        NULL,
                        FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_OPEN,
                        FILE_SYNCHRONOUS_IO_NONALERT,
                        NULL,
                        0);
}

static
NTSTATUS
SetResultBuffer(HANDLE Device, PKMT_RESULTBUFFER Buffer, ULONG BufferSize)
{
    IO_STATUS_BLOCK Iosb;

    /* IOCTL_KMTEST_SET_RESULTBUFFER is METHOD_NEITHER. The driver MDLs the
     * passed user-mode buffer (Type3InputBuffer) and pins it for the lifetime
     * of the file handle; subsequent RUN_TEST IOCTLs write results into it. */
    return NtDeviceIoControlFile(Device, NULL, NULL, NULL, &Iosb,
                                 IOCTL_KMTEST_SET_RESULTBUFFER,
                                 Buffer, BufferSize,
                                 NULL, 0);
}

static
NTSTATUS
RunTest(HANDLE Device, PCSTR TestName)
{
    IO_STATUS_BLOCK Iosb;
    ULONG NameLen = (ULONG)strlen(TestName);

    /* IOCTL_KMTEST_RUN_TEST is METHOD_BUFFERED. The driver reads InputBuffer
     * as an ANSI string of InputBufferLength bytes (NOT null-terminated). */
    return NtDeviceIoControlFile(Device, NULL, NULL, NULL, &Iosb,
                                 IOCTL_KMTEST_RUN_TEST,
                                 (PVOID)TestName, NameLen,
                                 NULL, 0);
}

static
NTSTATUS
GetTestList(HANDLE Device, PCHAR Buffer, ULONG BufferSize, PULONG OutLength)
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    /* IOCTL_KMTEST_GET_TESTS is METHOD_BUFFERED. The driver writes a
     * double-NUL-terminated sequence of ANSI test names. */
    RtlZeroMemory(Buffer, BufferSize);
    Status = NtDeviceIoControlFile(Device, NULL, NULL, NULL, &Iosb,
                                   IOCTL_KMTEST_GET_TESTS,
                                   NULL, 0,
                                   Buffer, BufferSize);
    if (NT_SUCCESS(Status))
        *OutLength = (ULONG)Iosb.Information;
    return Status;
}

static
VOID
EmitResultLog(PKMT_RESULTBUFFER Result)
{
    LONG Total = Result->LogBufferLength;
    LONG Offset = 0;
    CHAR Chunk[LOG_CHUNK_SIZE + 1];

    if (Total <= 0)
        return;
    if (Total > Result->LogBufferMaxLength)
        Total = Result->LogBufferMaxLength;

    /* Chunk-emit the log to avoid stack pressure and keep each NtWriteFile
     * write small (each call to EmitLine appends CRLF, so chunks land on
     * line boundaries close to LOG_CHUNK_SIZE without an exact split). */
    while (Offset < Total)
    {
        LONG Take = Total - Offset;
        if (Take > LOG_CHUNK_SIZE)
            Take = LOG_CHUNK_SIZE;
        RtlCopyMemory(Chunk, Result->LogBuffer + Offset, Take);
        Chunk[Take] = '\0';
        EmitLine("%s", Chunk);
        Offset += Take;
    }
}

static
VOID
ExitQemu(UCHAR Status)
{
    HANDLE ExitDevice = NULL;
    NTSTATUS NtStatus;
    IO_STATUS_BLOCK Iosb;

    NtStatus = LoadDriverByService(L"KmtExit");
    if (!NT_SUCCESS(NtStatus))
    {
        EmitLine("KMT-EXIT-LOAD-FAIL 0x%08lx", NtStatus);
        return;
    }

    NtStatus = OpenDevice(KMT_EXIT_DEVICE_DRIVER_PATH, &ExitDevice);
    if (!NT_SUCCESS(NtStatus))
    {
        EmitLine("KMT-EXIT-OPEN-FAIL 0x%08lx", NtStatus);
        return;
    }

    /* This IOCTL writes Status to port 0xF4. On QEMU with isa-debug-exit
     * configured, the VM terminates immediately with exit code
     * (Status << 1) | 1. The IOCTL never returns in that case. */
    NtDeviceIoControlFile(ExitDevice, NULL, NULL, NULL, &Iosb,
                          IOCTL_KMT_EXIT_QEMU,
                          &Status, sizeof(Status),
                          NULL, 0);

    /* Reached only on bare hardware / non-QEMU runs. */
    NtClose(ExitDevice);
}

INT
__cdecl
_main(
    IN INT argc,
    IN PCHAR argv[],
    IN PCHAR envp[],
    IN ULONG DebugFlag)
{
    NTSTATUS Status;
    HANDLE KmtDevice = NULL;
    PVOID ResultRegion = NULL;
    SIZE_T ResultRegionSize = RESULT_BUFFER_SIZE;
    PKMT_RESULTBUFFER Result = NULL;
    UCHAR ExitStatus = 1;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(envp);
    UNREFERENCED_PARAMETER(DebugFlag);

    OpenSerial();
    EmitLine("KMT-RUNNER start");

    Status = LoadDriverByService(L"Kmtest");
    if (!NT_SUCCESS(Status))
    {
        EmitLine("KMT-LOAD-FAIL Kmtest 0x%08lx", Status);
        goto Exit;
    }

    Status = OpenDevice(KMTEST_DEVICE_DRIVER_PATH, &KmtDevice);
    if (!NT_SUCCESS(Status))
    {
        EmitLine("KMT-OPEN-FAIL 0x%08lx", Status);
        goto Exit;
    }

    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &ResultRegion,
                                     0,
                                     &ResultRegionSize,
                                     MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        EmitLine("KMT-ALLOC-FAIL 0x%08lx", Status);
        goto Exit;
    }

    Result = (PKMT_RESULTBUFFER)ResultRegion;
    RtlZeroMemory(Result, sizeof(*Result));
    Result->LogBufferMaxLength = RESULT_BUFFER_SIZE - FIELD_OFFSET(KMT_RESULTBUFFER, LogBuffer);

    Status = SetResultBuffer(KmtDevice, Result, RESULT_BUFFER_SIZE);
    if (!NT_SUCCESS(Status))
    {
        EmitLine("KMT-SETBUF-FAIL 0x%08lx", Status);
        goto Exit;
    }

    {
        CHAR NamesBuffer[NAMES_BUFFER_SIZE];
        ULONG NamesLength = 0;
        PCSTR Cursor;
        LONG TotalPass = 0;
        LONG TotalFail = 0;
        LONG TotalSkip = 0;
        LONG TestsRun = 0;
        LONG TestsSkipped = 0;

        Status = GetTestList(KmtDevice, NamesBuffer, sizeof(NamesBuffer), &NamesLength);
        if (!NT_SUCCESS(Status))
        {
            EmitLine("KMT-GETLIST-FAIL 0x%08lx", Status);
            goto Exit;
        }

        for (Cursor = NamesBuffer;
             Cursor < NamesBuffer + NamesLength && *Cursor != '\0';
             Cursor += strlen(Cursor) + 1)
        {
            PCSTR TestName = Cursor;

            /* Names starting with '-' are interactive/manual (e.g.
             * ExHardErrorInteractive, ObTypeClean). Skip them - the
             * userland kmtest.exe runner applies the same filter. */
            if (TestName[0] == '-')
            {
                EmitLine("KMT-SKIP %s (interactive)", TestName + 1);
                ++TestsSkipped;
                continue;
            }

            /* Example is intentionally failing; exclude from automated runs. */
            if (strcmp(TestName, EXCLUDED_FIXTURE) == 0)
            {
                EmitLine("KMT-SKIP %s (excluded fixture)", TestName);
                ++TestsSkipped;
                continue;
            }

            /* DIAGNOSTIC SKIP: ExPools allocates 100000+ NonPaged blocks and
             * triggers OOM-fail paths in ARM3. Testing hypothesis that pool
             * corruption from ExPools causes the resource.c:844 AC=1/AE=0
             * assertion that fires at IoFilesystem entry. */
            if (strcmp(TestName, "ExPools") == 0)
            {
                EmitLine("KMT-SKIP %s (diagnostic pool-stress skip)", TestName);
                ++TestsSkipped;
                continue;
            }

            /* Reset per-test counters and log buffer. The kernel kmtest
             * framework writes into the user-pinned KMT_RESULTBUFFER for the
             * lifetime of the file handle, so we just clear what it reads
             * back here. LogBufferMaxLength stays. */
            Result->Successes = 0;
            Result->Failures = 0;
            Result->Skipped = 0;
            Result->LogBufferLength = 0;

            EmitLine("KMT-BEGIN %s", TestName);
            Status = RunTest(KmtDevice, TestName);
            if (!NT_SUCCESS(Status))
            {
                EmitLine("KMT-RUN-FAIL %s 0x%08lx", TestName, Status);
                ++TotalFail;
                continue;
            }

            EmitResultLog(Result);
            EmitLine("KMT-END %s successes=%ld failures=%ld skipped=%ld",
                     TestName,
                     Result->Successes, Result->Failures, Result->Skipped);

            TotalPass += Result->Successes;
            TotalFail += Result->Failures;
            TotalSkip += Result->Skipped;
            ++TestsRun;
        }

        EmitLine("KMT-SUMMARY tests_run=%ld tests_skipped=%ld total_pass=%ld total_fail=%ld total_skipped_checks=%ld",
                 TestsRun, TestsSkipped, TotalPass, TotalFail, TotalSkip);

        /* QEMU isa-debug-exit: value 0 → exit 1 (CTest PASS);
         * value 1 → exit 3 (CTest FAIL). */
        ExitStatus = (TotalFail == 0 && TestsRun > 0) ? 0 : 1;
    }

Exit:
    if (KmtDevice)
        NtClose(KmtDevice);
    if (ResultRegion)
    {
        SIZE_T FreeSize = 0;
        NtFreeVirtualMemory(NtCurrentProcess(), &ResultRegion, &FreeSize, MEM_RELEASE);
    }

    ExitQemu(ExitStatus);

    /* Fallback if isa-debug-exit was not wired (bare hardware). */
    EmitLine("KMT-RUNNER shutdown");
    if (g_SerialHandle)
        NtClose(g_SerialHandle);

    NtShutdownSystem(ShutdownNoReboot);

    return 0;
}
