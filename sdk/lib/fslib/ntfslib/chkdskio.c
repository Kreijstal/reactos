/*
 * PROJECT:     ReactOS NTFS filesystem library
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Native I/O adapter for the offline consistency checker
 */
#include <ndk/umtypes.h>
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <fmifs/fmifs.h>
#include "chkdsk.h"
#define NDEBUG
#include <debug.h>

typedef struct _NTFS_CHECK_CONTEXT
{
    HANDLE Handle;
    ULONG SectorSize;
    NTSTATUS FailureStatus;
    PFMIFSCALLBACK Callback;
} NTFS_CHECK_CONTEXT;

static int
NtfsCheckTransfer(NTFS_CHECK_CONTEXT *Context, ULONGLONG Offset,
                  PVOID Buffer, ULONG Length, BOOLEAN Write)
{
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Position;
    NTSTATUS Status;
    ULONG Prefix, AlignedLength;
    PUCHAR AlignedBuffer = Buffer;
    BOOLEAN Unaligned;

    if (Length == 0)
        return 0;
    Prefix = (ULONG)(Offset % Context->SectorSize);
    if (Length > MAXULONG - Prefix - (Context->SectorSize - 1))
    {
        Context->FailureStatus = STATUS_INVALID_PARAMETER;
        return -1;
    }
    AlignedLength = (Prefix + Length + Context->SectorSize - 1) & ~(Context->SectorSize - 1);
    Position.QuadPart = (LONGLONG)(Offset - Prefix);
    Unaligned = Prefix != 0 || AlignedLength != Length;
    if (Unaligned)
    {
        AlignedBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, AlignedLength);
        if (AlignedBuffer == NULL)
        {
            Context->FailureStatus = STATUS_INSUFFICIENT_RESOURCES;
            return -1;
        }
    }
    if (!Write || Unaligned)
    {
        Status = NtReadFile(Context->Handle, NULL, NULL, NULL, &Iosb,
                            AlignedBuffer, AlignedLength, &Position, NULL);
        if (!NT_SUCCESS(Status) || Iosb.Information != AlignedLength)
            goto Failure;
        if (!Write && Unaligned)
            RtlCopyMemory(Buffer, AlignedBuffer + Prefix, Length);
    }
    if (Write)
    {
        if (Unaligned)
            RtlCopyMemory(AlignedBuffer + Prefix, Buffer, Length);
        Status = NtWriteFile(Context->Handle, NULL, NULL, NULL, &Iosb,
                             AlignedBuffer, AlignedLength, &Position, NULL);
        if (!NT_SUCCESS(Status) || Iosb.Information != AlignedLength)
            goto Failure;
    }
    if (Unaligned)
        RtlFreeHeap(RtlGetProcessHeap(), 0, AlignedBuffer);
    return 0;
Failure:
    if (NT_SUCCESS(Context->FailureStatus))
        Context->FailureStatus = NT_SUCCESS(Status) ? STATUS_DEVICE_DATA_ERROR : Status;
    if (Unaligned)
        RtlFreeHeap(RtlGetProcessHeap(), 0, AlignedBuffer);
    return -1;
}

static int
NtfsCheckRead(void *Context, ULONGLONG Offset, void *Buffer, ULONG Length)
{
    return NtfsCheckTransfer(Context, Offset, Buffer, Length, FALSE);
}

static int
NtfsCheckWrite(void *Context, ULONGLONG Offset, const void *Buffer, ULONG Length)
{
    return NtfsCheckTransfer(Context, Offset, (PVOID)Buffer, Length, TRUE);
}

static int
NtfsCheckFlush(void *Context)
{
    NTFS_CHECK_CONTEXT *Io = Context;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = NtFlushBuffersFile(Io->Handle, &Iosb);

    if (!NT_SUCCESS(Status) && NT_SUCCESS(Io->FailureStatus))
        Io->FailureStatus = Status;
    return NT_SUCCESS(Status) ? 0 : -1;
}

static void
NtfsCheckMessage(void *Context, const char *Message)
{
    NTFS_CHECK_CONTEXT *Io = Context;
    TEXTOUTPUT Output;

    Output.Lines = 1;
    Output.Output = (PCHAR)Message;
    Io->Callback(OUTPUT, 0, &Output);
}

BOOLEAN
NTAPI
NtfsChkdsk(PUNICODE_STRING DriveRoot, PFMIFSCALLBACK Callback,
            BOOLEAN FixErrors, BOOLEAN Verbose, BOOLEAN CheckOnlyIfDirty,
            BOOLEAN ScanDrive, PVOID Unknown1, PVOID Unknown2,
            PVOID Unknown3, PVOID Unknown4, PULONG ExitStatus)
{
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    DISK_GEOMETRY Geometry;
    NTFS_CHECK_CONTEXT Context;
    MKNTFS_IO Io;
    NTFS_CHK_OPTIONS Options;
    NTFS_CHK_RESULT *Result;
    NTSTATUS Status, UnlockStatus;
    ACCESS_MASK Access;
    BOOLEAN Locked = FALSE;
    int Rc = -1;

    UNREFERENCED_PARAMETER(ScanDrive);
    UNREFERENCED_PARAMETER(Unknown1);
    UNREFERENCED_PARAMETER(Unknown2);
    UNREFERENCED_PARAMETER(Unknown3);
    UNREFERENCED_PARAMETER(Unknown4);
    Result = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Result));
    if (Result == NULL)
    {
        *ExitStatus = STATUS_INSUFFICIENT_RESOURCES;
        return FALSE;
    }
    Context.Handle = NULL;
    Context.FailureStatus = STATUS_SUCCESS;
    Context.Callback = Callback;
    Access = FILE_GENERIC_READ | SYNCHRONIZE;
    if (FixErrors)
        Access |= FILE_GENERIC_WRITE;
    InitializeObjectAttributes(&Attributes, DriveRoot, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Context.Handle, Access, &Attributes, &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_ALERT);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = NtDeviceIoControlFile(Context.Handle, NULL, NULL, NULL, &Iosb,
                                   IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                                   &Geometry, sizeof(Geometry));
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Context.SectorSize = Geometry.BytesPerSector;
    if (Context.SectorSize < 512 || Context.SectorSize > 4096 ||
        (Context.SectorSize & (Context.SectorSize - 1)) != 0)
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }
    if (FixErrors)
    {
        Status = NtFsControlFile(Context.Handle, NULL, NULL, NULL, &Iosb,
                                 FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0);
        if (!NT_SUCCESS(Status))
        {
            if (Callback != NULL)
                Callback(VOLUMEINUSE, 0, NULL);
            goto Cleanup;
        }
        Locked = TRUE;
        Status = NtFsControlFile(Context.Handle, NULL, NULL, NULL, &Iosb,
                                 FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }
    Io.context = &Context;
    Io.read = NtfsCheckRead;
    Io.write = NtfsCheckWrite;
    Io.flush = NtfsCheckFlush;
    RtlZeroMemory(&Options, sizeof(Options));
    Options.FixErrors = FixErrors;
    Options.Verbose = Verbose;
    Options.CheckOnlyIfDirty = CheckOnlyIfDirty;
    if (Callback != NULL)
    {
        ULONG Percent = 0;
        Callback(PROGRESS, 0, &Percent);
        Options.Message = NtfsCheckMessage;
        Options.MessageCtx = &Context;
    }
    Rc = NtfsChkVolume(&Io, &Options, Result);
    Status = (Rc < 0 || Result->ExitStatus == 1) ? STATUS_DISK_CORRUPT_ERROR : STATUS_SUCCESS;
    if (!NT_SUCCESS(Context.FailureStatus))
    {
        Status = Context.FailureStatus;
        Rc = -1;
    }
    DPRINT1("NtfsChkdsk: %wZ Fix %u OnlyIfDirty %u -> rc %d, issues %lu, recorded %lu, "
            "repaired %lu, dirty %u, Status 0x%08lx\n",
            DriveRoot, FixErrors, CheckOnlyIfDirty, Rc, Result->IssueCount,
            Result->IssueRecorded, Result->RepairedCount, Result->WasDirty, Status);
    if (Callback != NULL && Rc >= 0)
    {
        ULONG Percent = 100;
        Callback(PROGRESS, 0, &Percent);
    }
Cleanup:
    if (Locked)
    {
        UnlockStatus = NtFsControlFile(Context.Handle, NULL, NULL, NULL, &Iosb,
                                       FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
        if (NT_SUCCESS(Status) && !NT_SUCCESS(UnlockStatus))
        {
            Status = UnlockStatus;
            Rc = -1;
        }
    }
    if (Context.Handle != NULL)
        NtClose(Context.Handle);
    NtfsChkFreeResult(Result);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Result);
    *ExitStatus = Status;
    return Rc >= 0;
}
