/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Entry point and IOCTL dispatch
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 *
 * WHY THIS DRIVER IS DELIBERATELY NOT A PnP DRIVER
 *
 * IopUnloadDriver refuses to unload anything that lacks DRVO_LEGACY_DRIVER
 * (ntoskrnl/io/iomgr/driver.c), and NtUnloadDriver always passes
 * UnloadPnpDrivers = FALSE.  So a PnP miniport image stays resident for the
 * life of the boot no matter how many times its devnode is disabled and
 * re-enabled -- which is exactly what makes iterating on one cost a reboot.
 *
 * A driver gets DRVO_LEGACY_DRIVER when its PE header lacks
 * IMAGE_DLLCHARACTERISTICS_WDM_DRIVER *and* DriverEntry leaves behind at least
 * one device object.  set_module_type(rospoke kernelmodedriver) passes plain
 * /DRIVER (not /DRIVER:WDM) under MSVC and omits -Wl,--wdmdriver under GCC, so
 * the first condition holds; creating \Device\RosPoke below satisfies the
 * second.  Both are load-bearing.  If either one ever stops being true this
 * driver silently becomes un-unloadable and the reason for its existence goes
 * away, so DriverEntry checks and complains.
 */

#include "rospoke_int.h"

#define NDEBUG
#include <debug.h>

static PDEVICE_OBJECT RospokeDeviceObject = NULL;

static
NTSTATUS
NTAPI
RospokeCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
RospokeHandleExec(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ PVOID SystemBuffer,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    ROSPOKE_EXEC_IN Header;
    ROSPOKE_EXEC_OUT Out;
    PROSPOKE_OP Ops;
    PULONG Results = NULL;
    ULONG OpBytes, Capacity, Produced = 0;

    *Information = 0;

    if (InputLength < sizeof(ROSPOKE_EXEC_IN) || OutputLength < sizeof(ROSPOKE_EXEC_OUT))
        return STATUS_BUFFER_TOO_SMALL;

    Header = *(PROSPOKE_EXEC_IN)SystemBuffer;

    if (Header.Flags != 0)
        return STATUS_INVALID_PARAMETER;
    if (Header.OpCount == 0 || Header.OpCount > ROSPOKE_MAX_OPS)
        return STATUS_INVALID_PARAMETER;

    OpBytes = Header.OpCount * sizeof(ROSPOKE_OP);
    if (InputLength - sizeof(ROSPOKE_EXEC_IN) < OpBytes)
        return STATUS_INVALID_BUFFER_SIZE;

    /*
     * METHOD_BUFFERED aliases input and output in one system buffer, and the
     * result array starts at a lower offset than the ops end at -- writing
     * results in place would overwrite ops we have not executed yet.  Take a
     * private copy.  Non-paged so a fault cannot land in the middle of a timed
     * poll loop.
     */
    Ops = ExAllocatePoolWithTag(NonPagedPool, OpBytes, ROSPOKE_TAG);
    if (Ops == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Ops, (PUCHAR)SystemBuffer + sizeof(ROSPOKE_EXEC_IN), OpBytes);

    Capacity = (OutputLength - sizeof(ROSPOKE_EXEC_OUT)) / sizeof(ULONG);
    if (Capacity > ROSPOKE_MAX_RESULTS)
        Capacity = ROSPOKE_MAX_RESULTS;

    if (Capacity != 0)
    {
        Results = ExAllocatePoolWithTag(NonPagedPool, Capacity * sizeof(ULONG), ROSPOKE_TAG);
        if (Results == NULL)
        {
            ExFreePoolWithTag(Ops, ROSPOKE_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    RospokeExec(Ext, Ops, Header.OpCount, Results, Capacity, &Produced, &Out);

    RtlCopyMemory(SystemBuffer, &Out, sizeof(Out));
    if (Produced != 0)
    {
        RtlCopyMemory((PUCHAR)SystemBuffer + sizeof(ROSPOKE_EXEC_OUT),
                      Results,
                      Produced * sizeof(ULONG));
    }
    *Information = sizeof(ROSPOKE_EXEC_OUT) + Produced * sizeof(ULONG);

    if (Results != NULL)
        ExFreePoolWithTag(Results, ROSPOKE_TAG);
    ExFreePoolWithTag(Ops, ROSPOKE_TAG);

    /* A program that failed on the hardware is still a successful IOCTL: the
     * caller needs the results and the failing op index, not just an error. */
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
RospokeDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PROSPOKE_DEVEXT Ext = DeviceObject->DeviceExtension;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutputLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Information = 0;

    KeWaitForSingleObject(&Ext->Lock, Executive, KernelMode, FALSE, NULL);

    switch (Code)
    {
        case IOCTL_ROSPOKE_GET_VERSION:
        {
            ROSPOKE_VERSION_OUT Version;

            if (OutputLength < sizeof(Version))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Version.AbiVersion = ROSPOKE_ABI_VERSION;
            Version.MmioSlots = ROSPOKE_TARGET_MMIO_COUNT;
            Version.DmaSlots = ROSPOKE_TARGET_DMA_COUNT;
            Version.MaxOps = ROSPOKE_MAX_OPS;
            RtlCopyMemory(Buffer, &Version, sizeof(Version));
            Information = sizeof(Version);
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_ROSPOKE_OPEN_PCI:
        {
            ROSPOKE_OPEN_PCI_IN In;
            ROSPOKE_OPEN_PCI_OUT Out;

            if (InputLength < sizeof(In) || OutputLength < sizeof(Out))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            In = *(PROSPOKE_OPEN_PCI_IN)Buffer;
            Status = RospokeOpenPci(Ext, &In, &Out);
            if (NT_SUCCESS(Status))
            {
                RtlCopyMemory(Buffer, &Out, sizeof(Out));
                Information = sizeof(Out);
            }
            break;
        }

        case IOCTL_ROSPOKE_MAP_BAR:
        {
            ROSPOKE_MAP_BAR_IN In;
            ROSPOKE_SLOT_OUT Out;

            if (InputLength < sizeof(In) || OutputLength < sizeof(Out))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            In = *(PROSPOKE_MAP_BAR_IN)Buffer;
            Status = RospokeMapBar(Ext, &In, &Out);
            if (NT_SUCCESS(Status))
            {
                RtlCopyMemory(Buffer, &Out, sizeof(Out));
                Information = sizeof(Out);
            }
            break;
        }

        case IOCTL_ROSPOKE_MAP_PHYS:
        {
            ROSPOKE_MAP_PHYS_IN In;
            ROSPOKE_SLOT_OUT Out;

            if (InputLength < sizeof(In) || OutputLength < sizeof(Out))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            In = *(PROSPOKE_MAP_PHYS_IN)Buffer;
            Status = RospokeMapPhys(Ext, &In, &Out);
            if (NT_SUCCESS(Status))
            {
                RtlCopyMemory(Buffer, &Out, sizeof(Out));
                Information = sizeof(Out);
            }
            break;
        }

        case IOCTL_ROSPOKE_ALLOC_DMA:
        {
            ROSPOKE_ALLOC_DMA_IN In;
            ROSPOKE_SLOT_OUT Out;

            if (InputLength < sizeof(In) || OutputLength < sizeof(Out))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            In = *(PROSPOKE_ALLOC_DMA_IN)Buffer;
            Status = RospokeAllocDma(Ext, &In, &Out);
            if (NT_SUCCESS(Status))
            {
                RtlCopyMemory(Buffer, &Out, sizeof(Out));
                Information = sizeof(Out);
            }
            break;
        }

        case IOCTL_ROSPOKE_READ_BLOCK:
        {
            ROSPOKE_BLOCK_IN In;

            if (InputLength < sizeof(In))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            In = *(PROSPOKE_BLOCK_IN)Buffer;
            if (OutputLength < In.Length)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Status = RospokeReadBlock(Ext, &In, Buffer);
            if (NT_SUCCESS(Status))
                Information = In.Length;
            break;
        }

        case IOCTL_ROSPOKE_WRITE_BLOCK:
        {
            ROSPOKE_BLOCK_IN In;

            if (InputLength < sizeof(In))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            In = *(PROSPOKE_BLOCK_IN)Buffer;
            if (In.Length > ROSPOKE_MAX_BLOCK ||
                InputLength - sizeof(In) < In.Length)
            {
                Status = STATUS_INVALID_BUFFER_SIZE;
                break;
            }
            Status = RospokeWriteBlock(Ext, &In, (PUCHAR)Buffer + sizeof(In));
            break;
        }

        case IOCTL_ROSPOKE_EXEC:
            Status = RospokeHandleExec(Ext, Buffer, InputLength, OutputLength, &Information);
            break;

        case IOCTL_ROSPOKE_RELEASE_ALL:
            RospokeReleaseAll(Ext);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_ROSPOKE_QUERY_SLOTS:
        {
            ROSPOKE_QUERY_SLOTS_OUT Out;

            if (OutputLength < sizeof(Out))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            RospokeQuerySlots(Ext, &Out);
            RtlCopyMemory(Buffer, &Out, sizeof(Out));
            Information = sizeof(Out);
            break;
        }

        default:
            break;
    }

    KeReleaseMutex(&Ext->Lock, FALSE);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Information : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
VOID
NTAPI
RospokeUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING DosName = RTL_CONSTANT_STRING(ROSPOKE_DOS_NAME);
    PROSPOKE_DEVEXT Ext;

    UNREFERENCED_PARAMETER(DriverObject);

    DPRINT1("rospoke: unloading\n");

    if (RospokeDeviceObject != NULL)
    {
        Ext = RospokeDeviceObject->DeviceExtension;

        /* Nothing else can be in here: the I/O manager has already stopped
         * handing us IRPs by the time DriverUnload runs. */
        RospokeReleaseAll(Ext);

        IoDeleteSymbolicLink(&DosName);
        IoDeleteDevice(RospokeDeviceObject);
        RospokeDeviceObject = NULL;
    }
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(ROSPOKE_DEVICE_NAME);
    UNICODE_STRING DosName = RTL_CONSTANT_STRING(ROSPOKE_DOS_NAME);
    PDEVICE_OBJECT DeviceObject;
    PROSPOKE_DEVEXT Ext;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    Status = IoCreateDevice(DriverObject,
                            sizeof(ROSPOKE_DEVEXT),
                            &DeviceName,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("rospoke: IoCreateDevice failed (0x%08lx)\n", Status);
        return Status;
    }

    Ext = DeviceObject->DeviceExtension;
    RtlZeroMemory(Ext, sizeof(*Ext));
    KeInitializeMutex(&Ext->Lock, 0);

    DeviceObject->Flags |= DO_BUFFERED_IO;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = IoCreateSymbolicLink(&DosName, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("rospoke: IoCreateSymbolicLink failed (0x%08lx)\n", Status);
        IoDeleteDevice(DeviceObject);
        return Status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = RospokeCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = RospokeCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RospokeDeviceControl;
    DriverObject->DriverUnload = RospokeUnload;

    RospokeDeviceObject = DeviceObject;

    /*
     * The point of this driver is that `sc stop` really unloads it.  That needs
     * DRVO_LEGACY_DRIVER, which the I/O manager sets only for a non-WDM image
     * and then clears again if DriverEntry created no device object.  We have
     * just created one, so if the flag is missing the image was linked as a WDM
     * driver and the fast iteration loop is gone -- say so loudly rather than
     * let somebody discover it by wondering why their edits do nothing.
     */
    if (!(DriverObject->Flags & DRVO_LEGACY_DRIVER))
    {
        DPRINT1("rospoke: WARNING - not marked DRVO_LEGACY_DRIVER, so this image "
                "CANNOT be unloaded and hot-reload will not work.  Check that "
                "set_module_type() did not pass /DRIVER:WDM or --wdmdriver.\n");
    }

    DPRINT1("rospoke: loaded, ABI %u, device %S\n", ROSPOKE_ABI_VERSION, ROSPOKE_DEVICE_NAME);
    return STATUS_SUCCESS;
}
