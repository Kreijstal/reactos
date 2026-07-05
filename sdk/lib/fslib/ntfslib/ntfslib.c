/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/ntfslib.c
 * PURPOSE:     NTFS format and chkdsk implementation
 * PROGRAMMERS: Pierre Schweitzer (original stub)
 *              winmkntfs contributors (format implementation)
 * LICENSE:     GPL-2.0-or-later
 */

#include <ndk/umtypes.h>
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <fmifs/fmifs.h>

#include "mkntfs.h"
#include "chkdsk.h"

#define NDEBUG
#include <debug.h>

/* ============================================================
 * I/O adapter: bridges MKNTFS_IO callbacks to NtWriteFile/NtReadFile
 * ============================================================ */

typedef struct _NTFS_FORMAT_CONTEXT {
    HANDLE FileHandle;
    PFMIFSCALLBACK Callback;
    ULONG Percent;
} NTFS_FORMAT_CONTEXT;

#define SECTOR_SIZE 512

static int
NtfsFormatWrite(void *ctx, ULONGLONG offset, const void *buf, ULONG length)
{
    NTFS_FORMAT_CONTEXT *fctx = (NTFS_FORMAT_CONTEXT *)ctx;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER FileOffset;
    NTSTATUS Status;
    ULONG AlignedLength;

    /* Direct disk I/O requires sector-aligned offset and length */
    AlignedLength = (length + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);

    if (AlignedLength != length || (offset & (SECTOR_SIZE - 1)))
    {
        /* Unaligned: read-modify-write */
        ULONGLONG AlignedOffset = offset & ~(ULONGLONG)(SECTOR_SIZE - 1);
        ULONG PrePad = (ULONG)(offset - AlignedOffset);
        ULONG TotalLen = (PrePad + length + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
        PUCHAR TempBuf;

        TempBuf = RtlAllocateHeap(RtlGetProcessHeap(), 0, TotalLen);
        if (!TempBuf) return -1;

        /* Read existing sector data */
        FileOffset.QuadPart = (LONGLONG)AlignedOffset;
        Status = NtReadFile(fctx->FileHandle, NULL, NULL, NULL,
                            &IoStatusBlock, TempBuf, TotalLen,
                            &FileOffset, NULL);
        if (!NT_SUCCESS(Status))
            RtlZeroMemory(TempBuf, TotalLen);

        /* Overlay new data */
        RtlCopyMemory(TempBuf + PrePad, buf, length);

        /* Write back */
        Status = NtWriteFile(fctx->FileHandle, NULL, NULL, NULL,
                             &IoStatusBlock, TempBuf, TotalLen,
                             &FileOffset, NULL);
        RtlFreeHeap(RtlGetProcessHeap(), 0, TempBuf);
    }
    else
    {
        FileOffset.QuadPart = (LONGLONG)offset;
        Status = NtWriteFile(fctx->FileHandle, NULL, NULL, NULL,
                             &IoStatusBlock, (PVOID)buf, length,
                             &FileOffset, NULL);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtWriteFile failed at offset 0x%I64x, length %lu: 0x%08x\n",
                offset, length, Status);
        return -1;
    }
    return 0;
}

static int
NtfsFormatRead(void *ctx, ULONGLONG offset, void *buf, ULONG length)
{
    NTFS_FORMAT_CONTEXT *fctx = (NTFS_FORMAT_CONTEXT *)ctx;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER FileOffset;
    NTSTATUS Status;

    if ((length & (SECTOR_SIZE - 1)) || (offset & (SECTOR_SIZE - 1)))
    {
        /* Unaligned read */
        ULONGLONG AlignedOffset = offset & ~(ULONGLONG)(SECTOR_SIZE - 1);
        ULONG PrePad = (ULONG)(offset - AlignedOffset);
        ULONG TotalLen = (PrePad + length + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
        PUCHAR TempBuf;

        TempBuf = RtlAllocateHeap(RtlGetProcessHeap(), 0, TotalLen);
        if (!TempBuf) return -1;

        FileOffset.QuadPart = (LONGLONG)AlignedOffset;
        Status = NtReadFile(fctx->FileHandle, NULL, NULL, NULL,
                            &IoStatusBlock, TempBuf, TotalLen,
                            &FileOffset, NULL);
        if (NT_SUCCESS(Status))
            RtlCopyMemory(buf, TempBuf + PrePad, length);

        RtlFreeHeap(RtlGetProcessHeap(), 0, TempBuf);
    }
    else
    {
        FileOffset.QuadPart = (LONGLONG)offset;
        Status = NtReadFile(fctx->FileHandle, NULL, NULL, NULL,
                            &IoStatusBlock, buf, length,
                            &FileOffset, NULL);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtReadFile failed at offset 0x%I64x, length %lu: 0x%08x\n",
                offset, length, Status);
        return -1;
    }
    return 0;
}

static int
NtfsFormatFlush(void *ctx)
{
    NTFS_FORMAT_CONTEXT *fctx = (NTFS_FORMAT_CONTEXT *)ctx;
    IO_STATUS_BLOCK IoStatusBlock;

    NtFlushBuffersFile(fctx->FileHandle, &IoStatusBlock);
    return 0;
}

/* ============================================================
 * chkdsk message sink: forward core text to the fmifs callback
 *
 * The checker emits chkdsk-style stage banners and a final summary through
 * NTFS_CHK_OPTIONS.Message.  We forward each line as an OUTPUT command with a
 * TEXTOUTPUT payload, exactly the shape autochk/chkdsk.exe's ChkdskCallback
 * consumes (Command == OUTPUT, Argument == PTEXTOUTPUT).
 * ============================================================ */

static void
NtfsChkMessage(void *ctx, const char *msg)
{
    NTFS_FORMAT_CONTEXT *cctx = (NTFS_FORMAT_CONTEXT *)ctx;
    TEXTOUTPUT Output;

    if (cctx == NULL || cctx->Callback == NULL || msg == NULL)
        return;

    Output.Lines = 1;
    Output.Output = (PCHAR)msg;
    cctx->Callback(OUTPUT, 0, (PVOID)&Output);
}

/* ============================================================
 * NtfsFormat - PULIB_FORMAT implementation
 * ============================================================ */

BOOLEAN
NTAPI
NtfsFormat(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN QuickFormat,
    IN BOOLEAN BackwardCompatible,
    IN MEDIA_TYPE MediaType,
    IN PUNICODE_STRING Label,
    IN ULONG ClusterSize)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    DISK_GEOMETRY DiskGeometry;
    IO_STATUS_BLOCK Iosb;
    HANDLE FileHandle;
    PARTITION_INFORMATION_EX PartitionInfo;
    NTSTATUS Status, LockStatus;
    NTFS_FORMAT_CONTEXT FormatContext;
    MKNTFS_IO Io;
    MKNTFS_PARAMS Params;
    ULONGLONG PartitionLength;
    int Result;
    BOOLEAN VolumeLocked = FALSE;

    DPRINT("NtfsFormat(DriveRoot '%wZ')\n", DriveRoot);

    UNREFERENCED_PARAMETER(BackwardCompatible);
    UNREFERENCED_PARAMETER(MediaType);

    /* Report 0% progress */
    if (Callback != NULL)
    {
        ULONG Percent = 0;
        Callback(PROGRESS, 0, (PVOID)&Percent);
    }

    /* Open the partition */
    InitializeObjectAttributes(&ObjectAttributes,
                               DriveRoot,
                               0,
                               NULL,
                               NULL);

    Status = NtOpenFile(&FileHandle,
                        FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
                        &ObjectAttributes,
                        &Iosb,
                        FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_ALERT);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtOpenFile() failed with status 0x%08x\n", Status);
        return FALSE;
    }

    /* Query disk geometry */
    Status = NtDeviceIoControlFile(FileHandle,
                                   NULL, NULL, NULL,
                                   &Iosb,
                                   IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                   NULL, 0,
                                   &DiskGeometry,
                                   sizeof(DISK_GEOMETRY));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IOCTL_DISK_GET_DRIVE_GEOMETRY failed: 0x%08x\n", Status);
        NtClose(FileHandle);
        return FALSE;
    }

    /* Query partition info */
    if (DiskGeometry.MediaType == FixedMedia)
    {
        Status = NtDeviceIoControlFile(FileHandle,
                                       NULL, NULL, NULL,
                                       &Iosb,
                                       IOCTL_DISK_GET_PARTITION_INFO_EX,
                                       NULL, 0,
                                       &PartitionInfo,
                                       sizeof(PARTITION_INFORMATION_EX));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("IOCTL_DISK_GET_PARTITION_INFO_EX failed: 0x%08x\n", Status);
            NtClose(FileHandle);
            return FALSE;
        }
        PartitionLength = (ULONGLONG)PartitionInfo.PartitionLength.QuadPart;
    }
    else
    {
        PartitionLength =
            (ULONGLONG)DiskGeometry.Cylinders.QuadPart *
            (ULONGLONG)DiskGeometry.TracksPerCylinder *
            (ULONGLONG)DiskGeometry.SectorsPerTrack *
            (ULONGLONG)DiskGeometry.BytesPerSector;
    }

    DPRINT("Partition length: %I64u bytes, sector size: %lu\n",
           PartitionLength, DiskGeometry.BytesPerSector);

    /* Lock volume */
    LockStatus = NtFsControlFile(FileHandle,
                                 NULL, NULL, NULL,
                                 &Iosb,
                                 FSCTL_LOCK_VOLUME,
                                 NULL, 0, NULL, 0);
    if (!NT_SUCCESS(LockStatus))
    {
        DPRINT1("WARNING: Failed to lock volume: 0x%x\n", LockStatus);
        NtClose(FileHandle);
        return FALSE;
    }
    VolumeLocked = TRUE;

    /* Set up I/O callbacks */
    FormatContext.FileHandle = FileHandle;
    FormatContext.Callback = Callback;
    FormatContext.Percent = 0;

    Io.context = &FormatContext;
    Io.write = NtfsFormatWrite;
    Io.read = NtfsFormatRead;
    Io.flush = NtfsFormatFlush;

    /* Set up format parameters */
    RtlZeroMemory(&Params, sizeof(Params));
    Params.sector_size = DiskGeometry.BytesPerSector;
    Params.total_sectors = PartitionLength / DiskGeometry.BytesPerSector;
    Params.cluster_size = ClusterSize; /* 0 = auto */
    if (DiskGeometry.MediaType == FixedMedia)
        Params.hidden_sectors = (ULONG)(PartitionInfo.StartingOffset.QuadPart / DiskGeometry.BytesPerSector);

    /* Convert volume label */
    if (Label != NULL && Label->Length > 0)
    {
        Params.label = Label->Buffer;
    }

    /* Format! */
    Result = mkntfs_format(&Io, &Params);

    /* Report 100% progress */
    if (Callback != NULL)
    {
        ULONG Percent = 100;
        Callback(PROGRESS, 0, (PVOID)&Percent);
    }

    /* Dismount and unlock */
    if (VolumeLocked)
    {
        LockStatus = NtFsControlFile(FileHandle,
                                     NULL, NULL, NULL,
                                     &Iosb,
                                     FSCTL_DISMOUNT_VOLUME,
                                     NULL, 0, NULL, 0);
        if (!NT_SUCCESS(LockStatus))
        {
            DPRINT1("Failed to dismount volume: 0x%x\n", LockStatus);
            Result = -1;
        }

        LockStatus = NtFsControlFile(FileHandle,
                                     NULL, NULL, NULL,
                                     &Iosb,
                                     FSCTL_UNLOCK_VOLUME,
                                     NULL, 0, NULL, 0);
        if (!NT_SUCCESS(LockStatus))
        {
            DPRINT1("Failed to unlock volume: 0x%x\n", LockStatus);
        }
    }

    NtClose(FileHandle);

    DPRINT("NtfsFormat() done. Result: %d\n", Result);
    return (Result == 0);
}

BOOLEAN
NTAPI
NtfsChkdsk(
    IN PUNICODE_STRING DriveRoot,
    IN PFMIFSCALLBACK Callback,
    IN BOOLEAN FixErrors,
    IN BOOLEAN Verbose,
    IN BOOLEAN CheckOnlyIfDirty,
    IN BOOLEAN ScanDrive,
    IN PVOID pUnknown1,
    IN PVOID pUnknown2,
    IN PVOID pUnknown3,
    IN PVOID pUnknown4,
    IN PULONG ExitStatus)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK Iosb;
    HANDLE FileHandle;
    NTSTATUS Status, LockStatus;
    NTFS_FORMAT_CONTEXT IoContext;
    MKNTFS_IO Io;
    NTFS_CHK_OPTIONS Options;
    NTFS_CHK_RESULT *Result;
    ACCESS_MASK Access;
    BOOLEAN VolumeLocked = FALSE;
    int Rc;

    UNREFERENCED_PARAMETER(ScanDrive);
    UNREFERENCED_PARAMETER(pUnknown1);
    UNREFERENCED_PARAMETER(pUnknown2);
    UNREFERENCED_PARAMETER(pUnknown3);
    UNREFERENCED_PARAMETER(pUnknown4);

    DPRINT("NtfsChkdsk(DriveRoot '%wZ', Fix %u, OnlyIfDirty %u)\n",
           DriveRoot, FixErrors, CheckOnlyIfDirty);

    if (Callback != NULL)
    {
        ULONG Percent = 0;
        Callback(PROGRESS, 0, (PVOID)&Percent);
    }

    /* The checker result is large (bounded issue array); keep it off the stack. */
    Result = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(NTFS_CHK_RESULT));
    if (Result == NULL)
    {
        *ExitStatus = (ULONG)STATUS_INSUFFICIENT_RESOURCES;
        return FALSE;
    }

    /* Open read-only unless we intend to repair. */
    Access = FILE_GENERIC_READ | SYNCHRONIZE;
    if (FixErrors)
        Access |= FILE_GENERIC_WRITE;

    InitializeObjectAttributes(&ObjectAttributes, DriveRoot, 0, NULL, NULL);
    Status = NtOpenFile(&FileHandle,
                        Access,
                        &ObjectAttributes,
                        &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_ALERT);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtfsChkdsk: NtOpenFile() failed 0x%08x\n", Status);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Result);
        *ExitStatus = (ULONG)Status;
        return FALSE;
    }

    /* Lock the volume for an exclusive offline check when repairing. */
    if (FixErrors)
    {
        LockStatus = NtFsControlFile(FileHandle, NULL, NULL, NULL, &Iosb,
                                     FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0);
        if (NT_SUCCESS(LockStatus))
            VolumeLocked = TRUE;
        else
            DPRINT1("NtfsChkdsk: could not lock volume (0x%x); repair may be unsafe\n",
                    LockStatus);
    }

    IoContext.FileHandle = FileHandle;
    IoContext.Callback = Callback;
    IoContext.Percent = 0;

    Io.context = &IoContext;
    Io.write = NtfsFormatWrite;
    Io.read = NtfsFormatRead;
    Io.flush = NtfsFormatFlush;

    RtlZeroMemory(&Options, sizeof(Options));
    Options.FixErrors = (FixErrors != FALSE);
    Options.Verbose = (Verbose != FALSE);
    Options.CheckOnlyIfDirty = (CheckOnlyIfDirty != FALSE);
    /* Forward the checker's stage banners + summary to autochk/chkdsk.exe via
     * the fmifs callback (OUTPUT/TEXTOUTPUT).  IoContext already carries the
     * PFMIFSCALLBACK, so it doubles as the message-sink context. */
    if (Callback != NULL)
    {
        Options.Message = NtfsChkMessage;
        Options.MessageCtx = &IoContext;
    }

    Rc = NtfsChkVolume(&Io, &Options, Result);

    DPRINT("NtfsChkdsk: exit=%d scanned=%lu inuse=%lu issues=%lu repaired=%lu dirty=%d\n",
           Result->ExitStatus, Result->RecordsScanned, Result->RecordsInUse,
           Result->IssueCount, Result->RepairedCount, Result->WasDirty);

    if (VolumeLocked)
    {
        NtFsControlFile(FileHandle, NULL, NULL, NULL, &Iosb,
                        FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
        NtFsControlFile(FileHandle, NULL, NULL, NULL, &Iosb,
                        FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
    }
    NtClose(FileHandle);

    if (Callback != NULL)
    {
        ULONG Percent = 100;
        Callback(PROGRESS, 0, (PVOID)&Percent);
    }

    /*
     * Map the checker verdict to the autochk/chkdsk exit convention:
     *   0 = no problems, 1 = problems fixed, 2 = uncorrected problems remain,
     *   3 = could not run the check.
     */
    if (Rc < 0)
        *ExitStatus = 3;
    else if (Result->ExitStatus == 2)
        *ExitStatus = 1;
    else if (Result->ExitStatus == 1)
        *ExitStatus = 2;
    else
        *ExitStatus = 0;

    RtlFreeHeap(RtlGetProcessHeap(), 0, Result);

    /* TRUE means the check itself completed (see ExitStatus for the verdict). */
    return (Rc >= 0);
}
