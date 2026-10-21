/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS disk quota
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box quota coverage: FileFsControlInformation (quota tracking flags),
 * IRP_MJ_SET_QUOTA and IRP_MJ_QUERY_QUOTA for one SID (by SID list and by a
 * full restart scan).
 *
 * Native entry-point tests use user handles and user virtual memory to cover
 * I/O-manager probing, input capture and access checks. Direct IRPs also
 * exercise file-system completion and quota accounting independently.
 *
 * Replaces the host tests test_quota_roundtrip and test_quota_kernel ($Q/$O
 * view indexes driven through the quota.c workers) and the quota part of
 * test_ntfs_metadata_features.
 *
 * Windows reference: MS-FSCC 2.4.36 FILE_QUOTA_INFORMATION, 2.4.33
 * FILE_GET_QUOTA_INFORMATION, 2.5.2 FILE_FS_CONTROL_INFORMATION; MS-FSA
 * 2.1.5.20/2.1.5.21.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define QUOTA_BUFFER_SIZE   4096
#define TEST_THRESHOLD      (1024LL * 1024)
#define TEST_LIMIT          (2048LL * 1024)

static
NTSTATUS
NTAPI
QuotaIrpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
SendQuotaIrp(
    _In_ HANDLE Volume,
    _In_ UCHAR MajorFunction,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN ReturnSingleEntry,
    _In_opt_ PFILE_GET_QUOTA_INFORMATION SidList,
    _In_ ULONG SidListLength,
    _In_ BOOLEAN RestartScan,
    _Out_opt_ PULONG_PTR Information)
{
    NTSTATUS Status;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    PIRP Irp;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;

    if (Information != NULL)
        *Information = 0;
    Status = ObReferenceObjectByHandle(Volume, 0, *IoFileObjectType, KernelMode,
                                       (PVOID *)&FileObject, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (Irp == NULL)
    {
        ObDereferenceObject(FileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->RequestorMode = KernelMode;
    Irp->UserBuffer = Buffer;
    Irp->AssociatedIrp.SystemBuffer = Buffer;
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = MajorFunction;
    Stack->FileObject = FileObject;
    if (MajorFunction == IRP_MJ_QUERY_QUOTA)
    {
        Stack->Parameters.QueryQuota.Length = Length;
        Stack->Parameters.QueryQuota.SidList = SidList;
        Stack->Parameters.QueryQuota.SidListLength = SidListLength;
        Stack->Parameters.QueryQuota.StartSid = NULL;
        Stack->Flags = (RestartScan ? SL_RESTART_SCAN : 0) |
                       (ReturnSingleEntry ? SL_RETURN_SINGLE_ENTRY : 0);
    }
    else
    {
        Stack->Parameters.SetQuota.Length = Length;
    }
    IoSetCompletionRoutine(Irp, QuotaIrpCompletion, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = Irp->IoStatus.Status;
    if (Information != NULL)
        *Information = Irp->IoStatus.Information;

    IoFreeIrp(Irp);
    ObDereferenceObject(FileObject);
    return Status;
}

static
ULONG
BuildQuotaEntry(
    _Out_writes_bytes_(QUOTA_BUFFER_SIZE) PUCHAR Buffer,
    _In_ PSID Sid,
    _In_ LONGLONG Threshold,
    _In_ LONGLONG Limit)
{
    PFILE_QUOTA_INFORMATION Quota = (PFILE_QUOTA_INFORMATION)Buffer;
    ULONG SidLength = RtlLengthSid(Sid);

    RtlZeroMemory(Buffer, QUOTA_BUFFER_SIZE);
    Quota->NextEntryOffset = 0;
    Quota->SidLength = SidLength;
    Quota->QuotaThreshold.QuadPart = Threshold;
    Quota->QuotaLimit.QuadPart = Limit;
    RtlCopySid(SidLength, &Quota->Sid, Sid);
    return FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) + SidLength;
}

static
VOID
TestQuota(
    _In_ WCHAR Letter)
{
    HANDLE Volume;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_FS_CONTROL_INFORMATION Control, Saved;
    BOOLEAN RestoreControl = FALSE;
    PUCHAR Buffer, List;
    PSID Sid = SeExports->SeAliasUsersSid;
    ULONG Length;
    ULONG_PTR Info;
    PFILE_GET_QUOTA_INFORMATION Get;

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Buffer = NtfsKmtAlloc(QUOTA_BUFFER_SIZE);
    List = NtfsKmtAlloc(256);
    if (Buffer == NULL || List == NULL)
        goto Close;

    /* Quota tracking flags. */
    RtlZeroMemory(&Control, sizeof(Control));
    Status = ZwQueryVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control),
                                          FileFsControlInformation);
    if (!skip(!NtfsKmtIsUnimplemented(Status),
              "FileFsControlInformation not supported (0x%lx)\n", Status))
    {
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(IoStatus.Information, sizeof(FILE_FS_CONTROL_INFORMATION));
        Saved = Control;
        if (!(Control.FileSystemControlFlags & FILE_VC_QUOTA_TRACK))
        {
            Control.FileSystemControlFlags |= FILE_VC_QUOTA_TRACK;
            Status = ZwSetVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control),
                                                FileFsControlInformation);
            ok_eq_hex(Status, STATUS_SUCCESS);
            RestoreControl = NT_SUCCESS(Status);
            RtlZeroMemory(&Control, sizeof(Control));
            Status = ZwQueryVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control),
                                                  FileFsControlInformation);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(Control.FileSystemControlFlags & FILE_VC_QUOTA_TRACK, "FILE_VC_QUOTA_TRACK not set\n");
        }
    }

    /* Set a quota entry for BUILTIN\Users. */
    Length = BuildQuotaEntry(Buffer, Sid, TEST_THRESHOLD, TEST_LIMIT);
    Status = SendQuotaIrp(Volume, IRP_MJ_SET_QUOTA, Buffer, Length, FALSE, NULL, 0, FALSE, NULL);
    if (skip(!NtfsKmtIsUnimplemented(Status), "IRP_MJ_SET_QUOTA not supported (0x%lx)\n", Status))
        goto Restore;
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Query it back through a SID list. */
    Get = (PFILE_GET_QUOTA_INFORMATION)List;
    RtlZeroMemory(List, 256);
    Get->NextEntryOffset = 0;
    Get->SidLength = RtlLengthSid(Sid);
    RtlCopySid(Get->SidLength, &Get->Sid, Sid);
    RtlZeroMemory(Buffer, QUOTA_BUFFER_SIZE);
    Status = SendQuotaIrp(Volume, IRP_MJ_QUERY_QUOTA, Buffer, QUOTA_BUFFER_SIZE, FALSE, Get,
                          FIELD_OFFSET(FILE_GET_QUOTA_INFORMATION, Sid) + Get->SidLength, TRUE, &Info);
    if (!skip(!NtfsKmtIsUnimplemented(Status), "IRP_MJ_QUERY_QUOTA not supported (0x%lx)\n", Status))
    {
        PFILE_QUOTA_INFORMATION Quota = (PFILE_QUOTA_INFORMATION)Buffer;

        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Info >= FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) + RtlLengthSid(Sid), "Information %Iu\n", Info);
        ok_eq_ulong(Quota->NextEntryOffset, 0UL);
        ok_eq_ulong(Quota->SidLength, RtlLengthSid(Sid));
        ok(RtlEqualSid(&Quota->Sid, Sid), "quota SID mismatch\n");
        ok_eq_longlong(Quota->QuotaThreshold.QuadPart, TEST_THRESHOLD);
        ok_eq_longlong(Quota->QuotaLimit.QuadPart, TEST_LIMIT);

        /* A full scan returns it among the volume's entries. */
        {
            BOOLEAN Found = FALSE, Restart = TRUE;
            ULONG Calls = 0;

            for (;;)
            {
                PFILE_QUOTA_INFORMATION Entry = (PFILE_QUOTA_INFORMATION)Buffer;

                RtlZeroMemory(Buffer, QUOTA_BUFFER_SIZE);
                Status = SendQuotaIrp(Volume, IRP_MJ_QUERY_QUOTA, Buffer, QUOTA_BUFFER_SIZE, FALSE,
                                      NULL, 0, Restart, &Info);
                Restart = FALSE;
                if (Status != STATUS_SUCCESS || ++Calls > 100)
                    break;
                for (;;)
                {
                    if (RtlValidSid(&Entry->Sid) && RtlEqualSid(&Entry->Sid, Sid))
                    {
                        Found = TRUE;
                        ok_eq_longlong(Entry->QuotaLimit.QuadPart, TEST_LIMIT);
                    }
                    if (Entry->NextEntryOffset == 0)
                        break;
                    Entry = (PFILE_QUOTA_INFORMATION)((PUCHAR)Entry + Entry->NextEntryOffset);
                }
            }
            ok_eq_hex(Status, STATUS_NO_MORE_ENTRIES);
            ok(Found, "Users quota entry not found by a full scan\n");
        }
    }

    /* Leave the entry unlimited again. */
    Length = BuildQuotaEntry(Buffer, Sid, -1, -1);
    Status = SendQuotaIrp(Volume, IRP_MJ_SET_QUOTA, Buffer, Length, FALSE, NULL, 0, FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

Restore:
    if (RestoreControl)
    {
        Status = ZwSetVolumeInformationFile(Volume, &IoStatus, &Saved, sizeof(Saved),
                                            FileFsControlInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
Close:
    NtfsKmtFree(Buffer);
    NtfsKmtFree(List);
    ZwClose(Volume);
}

static NTSTATUS
QuotaTestLock(HANDLE Handle, UCHAR Minor)
{
    PFILE_OBJECT File;
    PDEVICE_OBJECT Device;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    KEVENT Event;
    LARGE_INTEGER Length;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode, (PVOID *)&File, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Device = IoGetRelatedDeviceObject(File);
    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (Irp == NULL) { ObDereferenceObject(File); return STATUS_INSUFFICIENT_RESOURCES; }
    Length.QuadPart = 10;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->RequestorMode = KernelMode;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_LOCK_CONTROL;
    Stack->MinorFunction = Minor;
    Stack->Flags = SL_EXCLUSIVE_LOCK | SL_FAIL_IMMEDIATELY;
    Stack->FileObject = File;
    Stack->Parameters.LockControl.ByteOffset.QuadPart = 0;
    Stack->Parameters.LockControl.Length = &Length;
    Stack->Parameters.LockControl.Key = 17;
    IoSetCompletionRoutine(Irp, QuotaIrpCompletion, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = Irp->IoStatus.Status;
    IoFreeIrp(Irp);
    ObDereferenceObject(File);
    return Status;
}

/* An immediate native error may leave the caller's IOSB untouched. Observe
 * the completed public IRP for the driver's zero-transfer error contract. */
static VOID
CheckRejectedWriteIrp(HANDLE Handle, LONGLONG Position)
{
    PFILE_OBJECT File;
    PDEVICE_OBJECT Device;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    KEVENT Event;
    NTSTATUS Status;
    CHAR Data = 'f';

    Status = ObReferenceObjectByHandle(Handle, FILE_WRITE_DATA, *IoFileObjectType, KernelMode, (PVOID *)&File, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Device = IoGetRelatedDeviceObject(File);
    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    ok(Irp != NULL, "could not allocate write IRP\n");
    if (Irp == NULL) { ObDereferenceObject(File); return; }
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->RequestorMode = KernelMode;
    Irp->Flags = IRP_WRITE_OPERATION;
    Irp->UserBuffer = &Data;
    IoAllocateMdl(&Data, sizeof(Data), FALSE, FALSE, Irp);
    ok(Irp->MdlAddress != NULL, "could not allocate write MDL\n");
    if (Irp->MdlAddress == NULL) { IoFreeIrp(Irp); ObDereferenceObject(File); return; }
    MmProbeAndLockPages(Irp->MdlAddress, KernelMode, IoReadAccess);
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0xfeedface;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_WRITE;
    Stack->FileObject = File;
    Stack->Parameters.Write.Length = 1;
    Stack->Parameters.Write.ByteOffset.QuadPart = Position;
    IoSetCompletionRoutine(Irp, QuotaIrpCompletion, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    ok_eq_hex(Irp->IoStatus.Status, STATUS_DISK_FULL);
    ok_eq_ulongptr(Irp->IoStatus.Information, 0);
    if (Irp->MdlAddress != NULL)
    {
        MmUnlockPages(Irp->MdlAddress);
        IoFreeMdl(Irp->MdlAddress);
    }
    IoFreeIrp(Irp);
    ObDereferenceObject(File);
}

static LONGLONG
QueryUsed(HANDLE Volume, PSID Sid)
{
    UCHAR List[FIELD_OFFSET(FILE_GET_QUOTA_INFORMATION, Sid) + SECURITY_MAX_SID_SIZE];
    UCHAR Buffer[FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) + SECURITY_MAX_SID_SIZE];
    PFILE_GET_QUOTA_INFORMATION Get = (PVOID)List;
    PFILE_QUOTA_INFORMATION Quota = (PVOID)Buffer;
    NTSTATUS Status;
    ULONG_PTR Information;

    RtlZeroMemory(List, sizeof(List));
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Get->SidLength = RtlLengthSid(Sid);
    RtlCopySid(Get->SidLength, &Get->Sid, Sid);
    Status = SendQuotaIrp(Volume, IRP_MJ_QUERY_QUOTA, Buffer, sizeof(Buffer), TRUE,
                          Get, FIELD_OFFSET(FILE_GET_QUOTA_INFORMATION, Sid) + Get->SidLength, TRUE, &Information);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return -1;
    ok(RtlEqualSid(&Quota->Sid, Sid), "wrong quota owner\n");
    return Quota->QuotaUsed.QuadPart;
}

static VOID
SetOwner(HANDLE File, PSID Sid)
{
    SECURITY_DESCRIPTOR Sd;
    NTSTATUS Status;

    RtlCreateSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(&Sd, Sid, FALSE);
    Status = ZwSetSecurityObject(File, OWNER_SECURITY_INFORMATION, &Sd);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static VOID
TestCharging(PCWSTR Root, WCHAR Letter)
{
    WCHAR Dir[MAX_PATH], Path[MAX_PATH], Stream[MAX_PATH], Alias[MAX_PATH], Padding[161];
    HANDLE Volume = NULL, File = NULL, Ads = NULL;
    IO_STATUS_BLOCK IoStatus;
    FILE_FS_CONTROL_INFORMATION Saved, Control;
    FILE_END_OF_FILE_INFORMATION Eof;
    FILE_SET_SPARSE_BUFFER Sparse;
    PUCHAR Buffer = NULL;
    ULONG Length, i;
    NTSTATUS Status;
    LONGLONG Baseline, OtherBaseline;
    BOOLEAN Restore = FALSE;
    LARGE_INTEGER Offset;

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_charging", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"owner.bin");
    NtfsKmtJoin(Alias, RTL_NUMBER_OF(Alias), Dir, L"alias.bin");
    RtlStringCchPrintfW(Stream, RTL_NUMBER_OF(Stream), L"%ls:extra", Path);
    Status = NtfsKmtCreateFileWithData(Path, "abc", 3);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    SetOwner(File, SeExports->SeWorldSid);
    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = ZwQueryVolumeInformationFile(Volume, &IoStatus, &Saved, sizeof(Saved), FileFsControlInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Buffer = NtfsKmtAlloc(QUOTA_BUFFER_SIZE);
    if (Buffer == NULL) goto Cleanup;
    Control = Saved;
    Control.FileSystemControlFlags &= ~(FILE_VC_QUOTA_TRACK | FILE_VC_QUOTA_ENFORCE);
    Status = ZwSetVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Restore = TRUE;
    /* Enabling tracking must include a file created before it was enabled. */
    Control.FileSystemControlFlags |= FILE_VC_QUOTA_TRACK;
    Status = ZwSetVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Baseline = QueryUsed(Volume, SeExports->SeWorldSid);
    ok(Baseline >= 3, "pre-existing resident data was not charged: %I64d\n", Baseline);
    OtherBaseline = QueryUsed(Volume, SeExports->SeAliasUsersSid);

    Status = NtfsKmtCreateFileWithData(Stream, "extra", 5);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 5);
    for (i = 0; i < RTL_NUMBER_OF(Padding) - 1; i++) Padding[i] = L'q';
    Padding[RTL_NUMBER_OF(Padding) - 1] = UNICODE_NULL;
    for (i = 0; i < 12; i++)
    {
        RtlStringCchPrintfW(Stream, RTL_NUMBER_OF(Stream), L"%ls:spill%02lu-%ls", Path, i, Padding);
        Status = NtfsKmtCreateFileWithData(Stream, "ads", 3);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 5 + (i + 1) * 3);
    }
    for (i = 0; i < 12; i++)
    {
        RtlStringCchPrintfW(Stream, RTL_NUMBER_OF(Stream), L"%ls:spill%02lu-%ls", Path, i, Padding);
        Status = NtfsKmtDeleteTree(Stream);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 5 + (11 - i) * 3);
    }
    RtlStringCchPrintfW(Stream, RTL_NUMBER_OF(Stream), L"%ls:extra", Path);
    Status = NtfsKmtLink(File, Alias, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 5);
    Offset.QuadPart = 3;
    Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, "defg", 4, &Offset, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 9);
    Eof.EndOfFile.QuadPart = 1;
    Status = ZwSetInformationFile(File, &IoStatus, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 3);

    SetOwner(File, SeExports->SeAliasUsersSid);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline - 3);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeAliasUsersSid), OtherBaseline + 6);
    SetOwner(File, SeExports->SeWorldSid);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 3);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeAliasUsersSid), OtherBaseline);

    /* Sparse holes are charged by logical length, not allocated clusters. */
    Status = NtfsKmtOpen(&Ads, Stream, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Sparse.SetSparse = TRUE;
        Status = NtfsKmtFsctl(Ads, FSCTL_SET_SPARSE, &Sparse, sizeof(Sparse), NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Eof.EndOfFile.QuadPart = 1024 * 1024 + 7;
        Status = ZwSetInformationFile(Ads, &IoStatus, &Eof, sizeof(Eof), FileEndOfFileInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline - 2 + Eof.EndOfFile.QuadPart);
        ZwClose(Ads); Ads = NULL;
    }
    ZwClose(File); File = NULL;
    Status = NtfsKmtDeleteTree(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline - 2 + Eof.EndOfFile.QuadPart);
    Status = NtfsKmtDeleteTree(Alias);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline - 3);
    /* Caller-supplied QuotaUsed never overwrites measured usage. */
    Length = BuildQuotaEntry(Buffer, SeExports->SeWorldSid, -1, -1);
    ((PFILE_QUOTA_INFORMATION)Buffer)->QuotaUsed.QuadPart = 123456;
    Status = SendQuotaIrp(Volume, IRP_MJ_SET_QUOTA, Buffer, Length, FALSE, NULL, 0, FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline - 3);

    Status = NtfsKmtCreateFileWithData(Path, "abc", 3);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    SetOwner(File, SeExports->SeWorldSid);
    Length = BuildQuotaEntry(Buffer, SeExports->SeWorldSid, Baseline + 2, Baseline + 2);
    Status = SendQuotaIrp(Volume, IRP_MJ_SET_QUOTA, Buffer, Length, FALSE, NULL, 0, FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Control.FileSystemControlFlags = (Control.FileSystemControlFlags & ~FILE_VC_QUOTA_MASK) | FILE_VC_QUOTA_ENFORCE;
    Status = ZwSetVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwQueryVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Control.FileSystemControlFlags & FILE_VC_QUOTA_MASK, FILE_VC_QUOTA_ENFORCE);
    Offset.QuadPart = 3;
    Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, "de", 2, &Offset, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 2);
    {
        Status = QuotaTestLock(File, IRP_MN_LOCK);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Offset.QuadPart = 5;
        Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, "f", 1, &Offset, NULL);
        ok_eq_hex(Status, STATUS_FILE_LOCK_CONFLICT);
        Status = QuotaTestLock(File, IRP_MN_UNLOCK_SINGLE);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    Offset.QuadPart = 5;
    Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, "f", 1, &Offset, NULL);
    ok_eq_hex(Status, STATUS_DISK_FULL);
    CheckRejectedWriteIrp(File, 5);
    Offset.QuadPart = -1; /* FILE_WRITE_TO_END_OF_FILE */
    Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, "f", 1, &Offset, NULL);
    ok_eq_hex(Status, STATUS_DISK_FULL);
    CheckRejectedWriteIrp(File, -1);
    Eof.EndOfFile.QuadPart = 6;
    Status = ZwSetInformationFile(File, &IoStatus, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_DISK_FULL);
    {
        FILE_STANDARD_INFORMATION Standard;
        Status = ZwQueryInformationFile(File, &IoStatus, &Standard, sizeof(Standard), FileStandardInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_longlong(Standard.EndOfFile.QuadPart, 5);
    }
    Offset.QuadPart = 0;
    Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, "A", 1, &Offset, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS); /* No growth; an exhausted quota permits it. */
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 2);
    /* TRACK takes precedence when both state flags are requested (MS-FSCC). */
    Control.FileSystemControlFlags |= FILE_VC_QUOTA_TRACK;
    Status = ZwSetVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwQueryVolumeInformationFile(Volume, &IoStatus, &Control, sizeof(Control), FileFsControlInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Control.FileSystemControlFlags & FILE_VC_QUOTA_MASK, FILE_VC_QUOTA_TRACK);
    Status = ZwSetInformationFile(File, &IoStatus, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 3);
    ZwClose(File); File = NULL;
    Status = NtfsKmtDeleteTree(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline - 3);
    Length = BuildQuotaEntry(Buffer, SeExports->SeWorldSid, -1, -1);
    Status = SendQuotaIrp(Volume, IRP_MJ_SET_QUOTA, Buffer, Length, FALSE, NULL, 0, FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Persist both $Q usage and the per-file charge, then grow after remount.
     * Losing the per-file baseline would charge the same bytes twice. */
    Status = NtfsKmtCreateFileWithData(Path, "persistent", 10);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    SetOwner(File, SeExports->SeWorldSid);
    ZwClose(File); File = NULL;
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 7);
    Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtFsctl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ZwClose(Volume); Volume = NULL;
    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 7);
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Eof.EndOfFile.QuadPart = 11;
    Status = ZwSetInformationFile(File, &IoStatus, &Eof, sizeof(Eof), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline + 8);
    ZwClose(File); File = NULL;
    Status = NtfsKmtDeleteTree(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_longlong(QueryUsed(Volume, SeExports->SeWorldSid), Baseline - 3);
Cleanup:
    if (Ads != NULL) ZwClose(Ads);
    if (File != NULL) ZwClose(File);
    if (Restore && Volume != NULL)
    {
        Status = ZwSetVolumeInformationFile(Volume, &IoStatus, &Saved, sizeof(Saved), FileFsControlInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (Volume != NULL) ZwClose(Volume);
    if (Buffer != NULL) NtfsKmtFree(Buffer);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static
NTSTATUS
OpenNativeQuotaVolume(WCHAR Letter, ACCESS_MASK Access, BOOLEAN Synchronous, PHANDLE Handle)
{
    WCHAR Path[16];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Original;
    NTSTATUS Status;

    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"\\??\\%c:", Letter);
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    /* Attribute-only opens of a bare device bypass its file system in the
     * I/O manager. Open through the file system first, then reduce the user
     * handle's granted access while retaining that file object. */
    Status = ZwOpenFile(&Original, Access | FILE_READ_EA | SYNCHRONIZE, &Attributes, &IoStatus,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       Synchronous ? FILE_SYNCHRONOUS_IO_NONALERT : 0);
    if (!NT_SUCCESS(Status)) return Status;
    Status = ZwDuplicateObject(NtCurrentProcess(), Original, NtCurrentProcess(), Handle,
                                Access | SYNCHRONIZE, 0, 0);
    ZwClose(Original);
    return Status;
}

static
VOID
TestNativeQuota(WCHAR Letter)
{
    PVOID Memory = NULL;
    SIZE_T RegionSize = 4 * PAGE_SIZE;
    PUCHAR Buffer;
    PFILE_GET_QUOTA_INFORMATION Get;
    PFILE_QUOTA_INFORMATION Entry;
    PIO_STATUS_BLOCK IoStatus;
    PSID StartSid;
    SID_IDENTIFIER_AUTHORITY Authority = {SECURITY_NT_AUTHORITY};
    HANDLE Volume = NULL, ReadOnly = NULL, Async = NULL;
    NTSTATUS Status;
    ULONG Length, Offset, ErrorOffset;

    ok_eq_uint(ExGetPreviousMode(), UserMode);
    if (ExGetPreviousMode() != UserMode) return;
    Status = ZwAllocateVirtualMemory(NtCurrentProcess(), &Memory, 0, &RegionSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Buffer = Memory;
    Get = (PVOID)(Buffer + PAGE_SIZE);
    IoStatus = (PVOID)(Buffer + 2 * PAGE_SIZE);
    StartSid = Buffer + 3 * PAGE_SIZE;
    RtlInitializeSid(StartSid, &Authority, 2);
    *RtlSubAuthoritySid(StartSid, 0) = 21;
    *RtlSubAuthoritySid(StartSid, 1) = 0x4E544653;
    Get->SidLength = RtlLengthSid(StartSid);
    RtlCopySid(Get->SidLength, &Get->Sid, StartSid);
    Length = FIELD_OFFSET(FILE_GET_QUOTA_INFORMATION, Sid) + Get->SidLength;
    Status = OpenNativeQuotaVolume(Letter, FILE_WRITE_DATA, TRUE, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = OpenNativeQuotaVolume(Letter, FILE_READ_ATTRIBUTES, TRUE, &ReadOnly);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;

    Offset = BuildQuotaEntry(Buffer, StartSid, TEST_THRESHOLD, TEST_LIMIT);
    Status = IoCheckQuotaBufferValidity((PVOID)Buffer, Offset, &ErrorOffset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtSetQuotaInformationFile(Volume, IoStatus, Buffer, Offset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_hex(IoStatus->Status, STATUS_SUCCESS);
    Status = NtSetQuotaInformationFile(ReadOnly, IoStatus, Buffer, Offset);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);

    /* Query requires no data access. A SID list also suppresses StartSid,
     * including an inaccessible StartSid pointer. */
    Status = NtQueryQuotaInformationFile(ReadOnly, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, Get, Length, (PSID)1, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Entry = (PVOID)Buffer;
        ok_eq_hex(IoStatus->Status, STATUS_SUCCESS);
        ok_eq_ulongptr(IoStatus->Information,
                        FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) + RtlLengthSid(StartSid));
        ok(RtlEqualSid(&Entry->Sid, StartSid), "native query returned wrong SID\n");
        ok_eq_longlong(Entry->QuotaThreshold.QuadPart, TEST_THRESHOLD);
        ok_eq_longlong(Entry->QuotaLimit.QuadPart, TEST_LIMIT);
    }
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, NULL, PAGE_SIZE,
                                          TRUE, Get, Length, NULL, FALSE);
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);
    Status = NtQueryQuotaInformationFile(Volume, NULL, Buffer, PAGE_SIZE,
                                          TRUE, Get, Length, NULL, FALSE);
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);
    Status = NtSetQuotaInformationFile(Volume, IoStatus, NULL, PAGE_SIZE);
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);
    Get->NextEntryOffset = 1;
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, Get, Length, NULL, FALSE);
    ok_eq_hex(Status, STATUS_QUOTA_LIST_INCONSISTENT);
    Get->NextEntryOffset = Length + 4;
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, Get, Length, NULL, FALSE);
    ok_eq_hex(Status, STATUS_QUOTA_LIST_INCONSISTENT);
    Get->NextEntryOffset = 0;
    Get->SidLength--;
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, Get, Length, NULL, FALSE);
    ok_eq_hex(Status, STATUS_QUOTA_LIST_INCONSISTENT);
    Get->SidLength++;
    ((PISID)StartSid)->Revision = 0;
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, NULL, 0, StartSid, TRUE);
    ok_eq_hex(Status, STATUS_INVALID_SID);
    ((PISID)StartSid)->Revision = SID_REVISION;

    Status = NtSetQuotaInformationFile(Volume, IoStatus, Buffer, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, NULL, 0, StartSid, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok(RtlEqualSid(&((PFILE_QUOTA_INFORMATION)Buffer)->Sid, StartSid),
           "StartSid did not override RestartScan\n");
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, NULL, 0, NULL, FALSE);
    ok_eq_hex(Status, STATUS_NO_MORE_ENTRIES);

    /* Validate the whole set list before dispatch: a malformed second entry
     * must leave the first entry unchanged and identify the offending offset. */
    Offset = BuildQuotaEntry(Buffer, StartSid, 33, 44);
    Offset = ALIGN_UP_BY(Offset, sizeof(ULONGLONG));
    RtlCopyMemory(Buffer + Offset, Buffer, Offset);
    ((PFILE_QUOTA_INFORMATION)Buffer)->NextEntryOffset = Offset;
    ((PFILE_QUOTA_INFORMATION)(Buffer + Offset))->SidLength = MAXULONG;
    Status = NtSetQuotaInformationFile(Volume, IoStatus, Buffer, 2 * Offset);
    ok_eq_hex(Status, STATUS_QUOTA_LIST_INCONSISTENT);
    ok_eq_hex(IoStatus->Status, STATUS_QUOTA_LIST_INCONSISTENT);
    ok_eq_ulongptr(IoStatus->Information, Offset);
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, Get, Length, NULL, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok_eq_longlong(((PFILE_QUOTA_INFORMATION)Buffer)->QuotaLimit.QuadPart, TEST_LIMIT);

    Status = OpenNativeQuotaVolume(Letter, 0, FALSE, &Async);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        IoStatus->Status = STATUS_PENDING;
        Status = NtQueryQuotaInformationFile(Async, IoStatus, Buffer, PAGE_SIZE,
                                              TRUE, Get, Length, NULL, FALSE);
        if (Status == STATUS_PENDING)
        {
            Status = ZwWaitForSingleObject(Async, FALSE, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            Status = IoStatus->Status;
        }
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ok_eq_longlong(((PFILE_QUOTA_INFORMATION)Buffer)->QuotaLimit.QuadPart, TEST_LIMIT);
    }
    Offset = BuildQuotaEntry(Buffer, SeExports->SeAliasAdminsSid, 100, 200);
    Status = NtSetQuotaInformationFile(Volume, IoStatus, Buffer, Offset);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    Offset = BuildQuotaEntry(Buffer, SeExports->SeAliasAdminsSid, -1, -2);
    Status = NtSetQuotaInformationFile(Volume, IoStatus, Buffer, Offset);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    Offset = BuildQuotaEntry(Buffer, StartSid, -1, -2);
    Status = NtSetQuotaInformationFile(Volume, IoStatus, Buffer, Offset);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtSetQuotaInformationFile(Volume, IoStatus, Buffer, Offset);
    ok_eq_hex(Status, STATUS_NO_MATCH);
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, NULL, 0, StartSid, TRUE);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = NtQueryQuotaInformationFile(Volume, IoStatus, Buffer, PAGE_SIZE,
                                          TRUE, Get, Length, NULL, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Entry = (PVOID)Buffer;
        ok_eq_longlong(Entry->QuotaUsed.QuadPart, 0);
        ok_eq_longlong(Entry->QuotaThreshold.QuadPart, 0);
        ok_eq_longlong(Entry->QuotaLimit.QuadPart, 0);
        ok_eq_longlong(Entry->ChangeTime.QuadPart, 0);
    }
Cleanup:
    if (Async) ZwClose(Async);
    if (ReadOnly) ZwClose(ReadOnly);
    if (Volume) ZwClose(Volume);
    RegionSize = 0;
    Status = ZwFreeVirtualMemory(NtCurrentProcess(), &Memory, &RegionSize, MEM_RELEASE);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

START_TEST(NtfsQuota)
{
    WCHAR Root[MAX_PATH];
    WCHAR Letter;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    TestQuota(Letter);
    TestCharging(Root, Letter);
    TestNativeQuota(Letter);
}
