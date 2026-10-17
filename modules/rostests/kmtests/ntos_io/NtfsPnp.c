/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     NTFS removal through the public PnP and file I/O interfaces
 */

#include <kmt_test.h>
#include "NtfsKmt.h"
#include <ntddstor.h>

static
NTSTATUS
NTAPI
PnpCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
SendPnp(PDEVICE_OBJECT Device, UCHAR Minor, PULONG_PTR Information)
{
    KEVENT Event;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    ok(Irp != NULL, "could not allocate PnP IRP\n");
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_PNP;
    Stack->MinorFunction = Minor;
    if (Minor == IRP_MN_QUERY_DEVICE_RELATIONS)
        Stack->Parameters.QueryDeviceRelations.Type = TargetDeviceRelation;
    IoSetCompletionRoutine(Irp, PnpCompletion, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = Irp->IoStatus.Status;
    if (Information != NULL)
        *Information = Irp->IoStatus.Information;
    IoFreeIrp(Irp);
    return Status;
}

static
PDEVICE_OBJECT
ReferenceVolume(WCHAR Drive, PHANDLE VolumeHandle)
{
    PFILE_OBJECT File;
    PDEVICE_OBJECT Device;
    HANDLE Handle;
    NTSTATUS Status;

    Status = NtfsKmtOpenVolume(Drive, GENERIC_READ | GENERIC_WRITE, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;
    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode, (PVOID *)&File, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(Handle);
        return NULL;
    }
    Device = IoGetRelatedDeviceObject(File);
    ObReferenceObject(Device);
    ObDereferenceObject(File);
    if (VolumeHandle != NULL)
        *VolumeHandle = Handle;
    else
        ZwClose(Handle);
    return Device;
}

START_TEST(NtfsPnp)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH], Drive;
    HANDLE File, Section;
    PDEVICE_OBJECT Device;
    PDEVICE_RELATIONS Relations;
    ULONG_PTR Information;
    ULONG i;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, &Drive))
        return;
    Device = ReferenceVolume(Drive, NULL);
    if (Device == NULL)
        return;

    Status = SendPnp(Device, IRP_MN_QUERY_DEVICE_RELATIONS, &Information);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Relations = (PDEVICE_RELATIONS)Information;
    ok(Relations != NULL, "successful TargetDeviceRelation needs an actual relation\n");
    if (NT_SUCCESS(Status) && Relations != NULL)
    {
        ok(Relations->Count != 0, "empty target relation\n");
        for (i = 0; i < Relations->Count; ++i)
            ObDereferenceObject(Relations->Objects[i]);
        ExFreePool(Relations);
    }

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_PNP.bin");
    Status = NtfsKmtCreateFileWithData(Path, "PnP mapped file", 15);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreate(&File, NULL, Path, GENERIC_READ, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = SendPnp(Device, IRP_MN_QUERY_REMOVE_DEVICE, NULL);
        ok(!NT_SUCCESS(Status), "query-remove accepted an open file: %lx\n", Status);
        Status = SendPnp(Device, IRP_MN_EJECT, NULL);
        ok(!NT_SUCCESS(Status), "eject accepted an open file: %lx\n", Status);
        Status = SendPnp(Device, IRP_MN_CANCEL_REMOVE_DEVICE, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);

        Status = ZwCreateSection(&Section, SECTION_MAP_READ, NULL, NULL,
                                  PAGE_READONLY, SEC_COMMIT, File);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(File);
        if (NT_SUCCESS(Status))
        {
            Status = SendPnp(Device, IRP_MN_QUERY_REMOVE_DEVICE, NULL);
            ok(!NT_SUCCESS(Status), "query-remove accepted a live section: %lx\n", Status);
            Status = SendPnp(Device, IRP_MN_CANCEL_REMOVE_DEVICE, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ZwClose(Section);
        }
    }

    Status = SendPnp(Device, IRP_MN_QUERY_REMOVE_DEVICE, NULL);
    trace("idle query-remove returned %lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtCreate(&File, NULL, Path, GENERIC_READ, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
        ok(!NT_SUCCESS(Status), "opened a file after accepted query-remove\n");
        if (NT_SUCCESS(Status))
            ZwClose(File);
    }
    /* Storage can veto removal. In either case CANCEL must leave a usable
     * filesystem; an idle fixed disk is not necessarily ejectable. */
    Status = SendPnp(Device, IRP_MN_CANCEL_REMOVE_DEVICE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreate(&File, NULL, Path, GENERIC_READ, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(File);
    Status = SendPnp(Device, IRP_MN_EJECT, NULL);
    trace("idle PnP eject returned %lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = SendPnp(Device, IRP_MN_QUERY_REMOVE_DEVICE, NULL);
        ok_eq_hex(Status, STATUS_DEVICE_REMOVED);
    }
    else
    {
        Status = NtfsKmtDelete(Path);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ObDereferenceObject(Device);
}

/* This entry owns a fresh throwaway NTFS data disk, separate from the boot
 * volume. It intentionally leaves that data volume unavailable. */
START_TEST(NtfsSurpriseRemove)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH], Drive;
    PDEVICE_OBJECT Device;
    HANDLE File, Volume;
    UCHAR Buffer[512];
    ULONG_PTR Read;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, &Drive))
        return;
    Device = ReferenceVolume(Drive, &Volume);
    if (Device == NULL)
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_REMOVED.bin");
    Status = NtfsKmtCreateFileWithData(Path, "removed disk data", 17);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreate(&File, NULL, Path, GENERIC_READ | GENERIC_WRITE | DELETE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(Volume);
        ObDereferenceObject(Device);
        return;
    }
    Status = NtfsKmtFlush(File);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SendPnp(Device, IRP_MN_SURPRISE_REMOVAL, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRead(File, 0, Buffer, 17, &Read);
    ok_eq_hex(Status, STATUS_DEVICE_REMOVED);
    ok_eq_ulongptr(Read, 0);
    Status = NtfsKmtRead(Volume, 0, Buffer, sizeof(Buffer), &Read);
    ok_eq_hex(Status, STATUS_DEVICE_REMOVED);
    Status = SendPnp(Device, IRP_MN_CANCEL_REMOVE_DEVICE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRead(File, 0, Buffer, 17, &Read);
    ok_eq_hex(Status, STATUS_DEVICE_REMOVED);
    Status = ZwClose(File);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwClose(Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SendPnp(Device, IRP_MN_REMOVE_DEVICE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SendPnp(Device, IRP_MN_REMOVE_DEVICE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ObDereferenceObject(Device);
}

START_TEST(NtfsEject)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH], Drive;
    HANDLE Volume, File;
    PDEVICE_OBJECT Device;
    IO_STATUS_BLOCK IoStatus;
    UCHAR Buffer[512];
    ULONG_PTR Read;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, &Drive))
        return;
    Device = ReferenceVolume(Drive, &Volume);
    if (Device == NULL)
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_EJECT.bin");
    Status = NtfsKmtCreate(&File, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = ZwDeviceIoControlFile(Volume, NULL, NULL, NULL, &IoStatus,
                                       IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0);
        ok(!NT_SUCCESS(Status), "storage eject accepted a busy volume: %lx\n", Status);
        ZwClose(File);
    }
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwDeviceIoControlFile(Volume, NULL, NULL, NULL, &IoStatus,
                                   IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0);
    trace("idle storage eject returned %lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtRead(Volume, 0, Buffer, sizeof(Buffer), &Read);
        ok_eq_hex(Status, STATUS_DEVICE_REMOVED);
    }
    else
    {
        Status = NtfsKmtRead(Volume, 0, Buffer, sizeof(Buffer), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Read, sizeof(Buffer));
    }
    Status = ZwClose(Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ObDereferenceObject(Device);
}

/* Like NtfsShutdown, this leaves fixtures for an offline host reader. A
 * forced REMOVE must flush while the device is still available, even without
 * an earlier QUERY_REMOVE. There is no explicit flush in this entry. */
START_TEST(NtfsRemove)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH], Drive;
    PDEVICE_OBJECT Device;
    HANDLE File;
    PUCHAR Buffer;
    const ULONG Size = 256 * 1024 + 123;
    ULONG_PTR Transferred;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, &Drive))
        return;
    Device = ReferenceVolume(Drive, NULL);
    if (Device == NULL)
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_REMOVE.bin");
    Status = NtfsKmtCreate(&File, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(Device);
        return;
    }
    Buffer = NtfsKmtAlloc(Size);
    if (Buffer == NULL)
    {
        ZwClose(File);
        ObDereferenceObject(Device);
        return;
    }
    NtfsKmtFillPattern(Buffer, Size, 41, 0);
    Status = NtfsKmtWrite(File, 0, Buffer, Size, &Transferred);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Transferred, Size);
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_REMOVE_meta.txt");
    Status = NtfsKmtCreateFileWithData(Path, "NTFS removal metadata!", 22);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SendPnp(Device, IRP_MN_REMOVE_DEVICE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRead(File, 0, Buffer, Size, &Transferred);
    ok_eq_hex(Status, STATUS_DEVICE_REMOVED);
    Status = ZwClose(File);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtFree(Buffer);
    ObDereferenceObject(Device);
}
