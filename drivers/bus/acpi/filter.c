/*
 * PROJECT:     ReactOS ACPI bus driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ACPI bus filters over the PDOs of other bus drivers
 *
 * Like the Windows ACPI driver (ACPIDetectFilterDevices / ACPIBuildFilter /
 * AcpiFilterIrpDispatch), when a device stack ACPI is part of returns its
 * BusRelations, every reported PDO whose IRP_MN_QUERY_CAPABILITIES Address
 * matches the _ADR of a child of our ACPI node gets an ACPI filter attached
 * directly above it. This is how the drivers of PCI devices (and of the
 * children of PCI devices, e.g. IDE channels) reach their ACPI namespace node:
 * IOCTL_ACPI_EVAL_METHOD sent down their own stack is evaluated by the filter.
 * The filter is created before the PnP manager builds the rest of the child's
 * stack, so its FDO and upper filters end up above it.
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

#define NDEBUG
#include <debug.h>

#define TAG_ACPI_FILTER 'FpcA'

/* FUNCTIONS ******************************************************************/

static
BOOLEAN
ACPIIsDeviceNodePresent(
    _In_ ACPI_HANDLE AcpiHandle)
{
    ACPI_HANDLE StaHandle;
    unsigned long long Sta;

    /* No _STA means the device is present */
    if (!ACPI_SUCCESS(AcpiGetHandle(AcpiHandle, "_STA", &StaHandle)))
        return TRUE;

    if (!ACPI_SUCCESS(acpi_evaluate_integer(AcpiHandle, "_STA", NULL, &Sta)))
        return FALSE;

    return (Sta & ACPI_STA_DEVICE_PRESENT) != 0;
}

/**
 * @brief Finds the child device node of ParentHandle whose _ADR equals Address.
 */
static
ACPI_HANDLE
ACPIFindChildByAddress(
    _In_ ACPI_HANDLE ParentHandle,
    _In_ ULONG Address)
{
    ACPI_HANDLE Child = NULL, AdrHandle;
    unsigned long long Adr;

    while (ACPI_SUCCESS(AcpiGetNextObject(ACPI_TYPE_DEVICE, ParentHandle, Child, &Child)))
    {
        if (!ACPI_SUCCESS(AcpiGetHandle(Child, "_ADR", &AdrHandle)))
            continue;

        if (!ACPI_SUCCESS(acpi_evaluate_integer(Child, "_ADR", NULL, &Adr)))
            continue;

        if (Adr != Address)
            continue;

        if (!ACPIIsDeviceNodePresent(Child))
            continue;

        return Child;
    }

    return NULL;
}

/**
 * @brief Creates an ACPI filter for AcpiHandle and attaches it directly above Pdo.
 */
static
NTSTATUS
ACPIBuildFilter(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo,
    _In_ ACPI_HANDLE AcpiHandle,
    _In_ ULONG Address)
{
    PDEVICE_OBJECT Filter, LowerDevice;
    PFILTER_DEVICE_DATA FilterData;
    NTSTATUS Status;
    CHAR PathName[128];
    ACPI_BUFFER PathBuffer = { sizeof(PathName), PathName };

    PAGED_CODE();

    Status = IoCreateDevice(DriverObject,
                            sizeof(FILTER_DEVICE_DATA),
                            NULL,
                            FILE_DEVICE_ACPI,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &Filter);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IoCreateDevice() failed with status 0x%lx\n", Status);
        return Status;
    }

    FilterData = Filter->DeviceExtension;
    FilterData->Common.Self = Filter;
    FilterData->Common.IsFDO = FALSE;
    FilterData->Common.IsFilter = TRUE;
    INITIALIZE_PNP_STATE(FilterData->Common);
    FilterData->Common.DevicePowerState = PowerDeviceUnspecified;
    FilterData->Common.SystemPowerState = PowerSystemWorking;
    FilterData->AcpiHandle = AcpiHandle;
    FilterData->PhysicalDeviceObject = Pdo;
    IoInitializeRemoveLock(&FilterData->RemoveLock, TAG_ACPI_FILTER, 0, 0);

    LowerDevice = IoAttachDeviceToDeviceStack(Filter, Pdo);
    if (!LowerDevice)
    {
        DPRINT1("Failed to attach the ACPI filter to PDO %p\n", Pdo);
        IoDeleteDevice(Filter);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* We only ever attach to a PDO nothing else has been attached to yet */
    ASSERT(LowerDevice == Pdo);
    FilterData->NextLowerDriver = LowerDevice;

    Filter->AlignmentRequirement = LowerDevice->AlignmentRequirement;
    Filter->Flags |= LowerDevice->Flags & (DO_POWER_PAGABLE | DO_DIRECT_IO | DO_BUFFERED_IO);
    Filter->Flags &= ~DO_DEVICE_INITIALIZING;

    if (!ACPI_SUCCESS(AcpiGetName(AcpiHandle, ACPI_FULL_PATHNAME, &PathBuffer)))
        strcpy(PathName, "?");

    DPRINT1("ACPI filter %p over PDO %p (%wZ), address 0x%08lx, node %s\n",
            Filter, Pdo, &Pdo->DriverObject->DriverName, Address, PathName);

    return STATUS_SUCCESS;
}

/**
 * @brief Attaches an ACPI filter above every PDO in DeviceRelations that has
 * a matching child node under ParentHandle, the ACPI node of the device whose
 * BusRelations these are.
 */
VOID
ACPIDetectFilterDevices(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ACPI_HANDLE ParentHandle,
    _In_ PDEVICE_RELATIONS DeviceRelations)
{
    DEVICE_CAPABILITIES Capabilities;
    PDEVICE_OBJECT Pdo;
    ACPI_HANDLE ChildHandle;
    NTSTATUS Status;
    ULONG i;

    PAGED_CODE();
    ASSERT(ParentHandle != NULL);

    for (i = 0; i < DeviceRelations->Count; i++)
    {
        Pdo = DeviceRelations->Objects[i];

        /* One of our own PDOs: it has its ACPI node already */
        if (Pdo->DriverObject == DriverObject)
            continue;

        /*
         * Already filtered by an earlier enumeration. The filter is built the
         * first time the PDO is reported, before the PnP manager attaches
         * anything to it, so any other attached device means we failed then.
         */
        if (Pdo->AttachedDevice != NULL)
        {
            if (Pdo->AttachedDevice->DriverObject != DriverObject)
                DPRINT("PDO %p already has a device stack, not filtered\n", Pdo);
            continue;
        }

        Status = Bus_GetDeviceCapabilities(Pdo, &Capabilities);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("IRP_MN_QUERY_CAPABILITIES on PDO %p failed 0x%lx\n", Pdo, Status);
            continue;
        }

        if (Capabilities.Address == MAXULONG)
            continue;

        ChildHandle = ACPIFindChildByAddress(ParentHandle, Capabilities.Address);
        if (!ChildHandle)
            continue;

        ACPIBuildFilter(DriverObject, Pdo, ChildHandle, Capabilities.Address);
    }
}

NTSTATUS
ACPIFilterDispatchPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PFILTER_DEVICE_DATA FilterData = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    ASSERT(FilterData->Common.IsFilter);

    Status = IoAcquireRemoveLock(&FilterData->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(FilterData->NextLowerDriver, Irp);

    IoReleaseRemoveLock(&FilterData->RemoveLock, Irp);
    return Status;
}

NTSTATUS
ACPIFilterDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PFILTER_DEVICE_DATA FilterData = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_RELATIONS DeviceRelations;
    NTSTATUS Status;

    PAGED_CODE();
    ASSERT(FilterData->Common.IsFilter);

    Status = IoAcquireRemoveLock(&FilterData->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_REMOVE_DEVICE:
        {
            /* Wait for all outstanding requests, then go away with the stack */
            IoReleaseRemoveLockAndWait(&FilterData->RemoveLock, Irp);

            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(FilterData->NextLowerDriver, Irp);

            IoDetachDevice(FilterData->NextLowerDriver);
            IoDeleteDevice(DeviceObject);
            return Status;
        }

        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            if (IoStack->Parameters.QueryDeviceRelations.Type != BusRelations)
                break;

            /*
             * The function driver above us has filled in the relations; let
             * the PDO see them, then filter the children that are in our
             * part of the ACPI namespace (e.g. the devices behind a PCI-to-PCI
             * bridge, or the channels of an IDE controller).
             */
            Status = Bus_SendIrpSynchronously(FilterData->NextLowerDriver, Irp);
            DeviceRelations = (PDEVICE_RELATIONS)Irp->IoStatus.Information;
            if (NT_SUCCESS(Status) && DeviceRelations)
            {
                ACPIDetectFilterDevices(DeviceObject->DriverObject,
                                        FilterData->AcpiHandle,
                                        DeviceRelations);
            }

            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            IoReleaseRemoveLock(&FilterData->RemoveLock, Irp);
            return Status;
        }

        case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        {
            /*
             * On the way down DO_POWER_PAGABLE may become set: mimic the
             * driver above us. On the way up it may become clear: mimic the
             * driver below us.
             */
            if (!DeviceObject->AttachedDevice ||
                (DeviceObject->AttachedDevice->Flags & DO_POWER_PAGABLE))
            {
                DeviceObject->Flags |= DO_POWER_PAGABLE;
            }

            Status = Bus_SendIrpSynchronously(FilterData->NextLowerDriver, Irp);

            if (!(FilterData->NextLowerDriver->Flags & DO_POWER_PAGABLE))
                DeviceObject->Flags &= ~DO_POWER_PAGABLE;

            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            IoReleaseRemoveLock(&FilterData->RemoveLock, Irp);
            return Status;
        }

        default:
            break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(FilterData->NextLowerDriver, Irp);

    IoReleaseRemoveLock(&FilterData->RemoveLock, Irp);
    return Status;
}

NTSTATUS
ACPIFilterDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PFILTER_DEVICE_DATA FilterData = DeviceObject->DeviceExtension;
    NTSTATUS Status;

    ASSERT(FilterData->Common.IsFilter);

    Status = IoAcquireRemoveLock(&FilterData->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    Status = PoCallDriver(FilterData->NextLowerDriver, Irp);

    IoReleaseRemoveLock(&FilterData->RemoveLock, Irp);
    return Status;
}

NTSTATUS
ACPIFilterDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PFILTER_DEVICE_DATA FilterData = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    ASSERT(FilterData->Common.IsFilter);

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_ACPI_ASYNC_EVAL_METHOD:
            ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
            break;

        case IOCTL_ACPI_EVAL_METHOD:
            ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);
            break;

        default:
            /* Not ours, the PDO's driver may know it */
            return ACPIFilterDispatchPassThrough(DeviceObject, Irp);
    }

    Status = IoAcquireRemoveLock(&FilterData->RemoveLock, Irp);
    if (NT_SUCCESS(Status))
    {
        Irp->IoStatus.Information = 0;
        Status = Bus_EvalMethod(FilterData->AcpiHandle, Irp);
        IoReleaseRemoveLock(&FilterData->RemoveLock, Irp);
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* EOF */
