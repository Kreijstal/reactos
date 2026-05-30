/*
 * PROJECT:     ReactOS kernel-mode test runner
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     QEMU isa-debug-exit driver. One IOCTL: write a byte to IO
 *              port 0xF4 so QEMU terminates with a predictable exit code.
 *              Used by kmtestrunner.exe at end-of-pass on a kmtestcd boot.
 */

#include <ntddk.h>
#include "kmt_exit_ioctl.h"

#define NDEBUG
#include <debug.h>

#define QEMU_ISA_DEBUG_EXIT_PORT ((PUCHAR)0xF4)

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD KmtExitUnload;
__drv_dispatchType(IRP_MJ_CREATE)
__drv_dispatchType(IRP_MJ_CLOSE)
static DRIVER_DISPATCH KmtExitCreateClose;
__drv_dispatchType(IRP_MJ_DEVICE_CONTROL)
static DRIVER_DISPATCH KmtExitIoControl;

static PDEVICE_OBJECT KmtExitDeviceObject = NULL;

NTSTATUS
NTAPI
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING DeviceName, SymlinkName;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&DeviceName, KMT_EXIT_DEVICE_DRIVER_PATH);
    Status = IoCreateDevice(DriverObject,
                            0,
                            &DeviceName,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &KmtExitDeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Userland opens via \\.\KmtExit which resolves through \??\KmtExit. */
    RtlInitUnicodeString(&SymlinkName, L"\\??\\" KMT_EXIT_DEVICE_NAME);
    Status = IoCreateSymbolicLink(&SymlinkName, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(KmtExitDeviceObject);
        return Status;
    }

    DriverObject->DriverUnload = KmtExitUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = KmtExitCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = KmtExitCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KmtExitIoControl;

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
KmtExitUnload(
    IN PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING SymlinkName;

    UNREFERENCED_PARAMETER(DriverObject);

    RtlInitUnicodeString(&SymlinkName, L"\\??\\" KMT_EXIT_DEVICE_NAME);
    IoDeleteSymbolicLink(&SymlinkName);

    if (KmtExitDeviceObject)
        IoDeleteDevice(KmtExitDeviceObject);
}

static
NTSTATUS
NTAPI
KmtExitCreateClose(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
KmtExitIoControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    if (IoStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_KMT_EXIT_QEMU)
    {
        UCHAR ExitStatus = 0;

        if (IoStack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(UCHAR) &&
            Irp->AssociatedIrp.SystemBuffer != NULL)
        {
            ExitStatus = *(PUCHAR)Irp->AssociatedIrp.SystemBuffer;
        }

        /*
         * Write to QEMU's isa-debug-exit IO port. QEMU exits with status
         * (ExitStatus << 1) | 1. The instruction returns on bare hardware
         * (port write to an unmapped ISA port is silently dropped), so we
         * still complete the IRP for that case.
         */
        WRITE_PORT_UCHAR(QEMU_ISA_DEBUG_EXIT_PORT, ExitStatus);

        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}
