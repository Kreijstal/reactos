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
 * The quota IRPs are built here and sent straight to the file system device
 * below the volume handle: that is exactly what NtSetQuotaInformationFile /
 * NtQueryQuotaInformationFile send, but ReactOS' I/O manager still has both
 * as UNIMPLEMENTED stubs, and this test is about the file system.
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
}
