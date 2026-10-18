/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     NTFS shutdown writeback through the filesystem control device
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

/* These two files deliberately survive the test. A host reader can verify the
 * data and resident metadata on the throwaway disk after system_powerdown,
 * without mounting it. There is no explicit flush before the shutdown IRP. */
START_TEST(NtfsShutdown)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH];
    HANDLE Handle;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Ntfs");
    PFILE_OBJECT ControlFile;
    PDEVICE_OBJECT ControlDevice;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    PUCHAR Buffer;
    const ULONG Size = 256 * 1024 + 123;
    ULONG_PTR Written;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_SHUTDOWN.bin");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Buffer = NtfsKmtAlloc(Size);
    if (Buffer == NULL)
    {
        ZwClose(Handle);
        return;
    }
    NtfsKmtFillPattern(Buffer, Size, 41, 0);
    Status = NtfsKmtWrite(Handle, 0, Buffer, Size, &Written);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Written, Size);
    NtfsKmtFree(Buffer);

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_SHUTDOWN_meta.txt");
    Status = NtfsKmtCreateFileWithData(Path, "NTFS shutdown metadata", 22);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = IoGetDeviceObjectPointer(&Name, FILE_READ_DATA, &ControlFile, &ControlDevice);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        Irp = IoBuildSynchronousFsdRequest(IRP_MJ_SHUTDOWN, ControlDevice,
                                           NULL, 0, NULL, &Event, &IoStatus);
        ok(Irp != NULL, "could not allocate shutdown IRP\n");
        if (Irp != NULL)
        {
            Status = IoCallDriver(ControlDevice, Irp);
            if (Status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
                Status = IoStatus.Status;
            }
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
        ObDereferenceObject(ControlFile);
    }
    ZwClose(Handle);
}
