/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite device idle detection test
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * PoRegisterDeviceForIdleDetection: a two-device stack registers its bottom
 * device with short time-outs. Once the device was idle for the time-out,
 * the power manager sends IRP_MN_SET_POWER for the registered device state
 * to the top of the stack, once per idle period; PoSetDeviceBusy holds it
 * off; registering again changes the values and returns the same counter;
 * registering with zero time-outs, and deleting the device, cancel it.
 * Both time-outs are the same, so AC and battery power test the same.
 */

#include <kmt_test.h>
#include "PoIdle.h"

static PDRIVER_OBJECT TestDriverObject;
static KMT_MESSAGE_HANDLER TestMessageHandler;

static PDEVICE_OBJECT LowerDevice;
static PDEVICE_OBJECT UpperDevice;

/* What the power IRP handler saw */
static KEVENT IdleIrpEvent;
static volatile LONG IdleIrpCount;
static volatile LONG IdleIrpUpperFirst;
static DEVICE_POWER_STATE IdleIrpState;
static LARGE_INTEGER IdleIrpTime;
static volatile LONG StrayIrpCount;

static
NTSTATUS
CreateTestDevices(
    _In_ PDRIVER_OBJECT DriverObject)
{
    NTSTATUS Status;
    PDEVICE_OBJECT AttachedDevice;

    Status = IoCreateDevice(DriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &LowerDevice);
    if (!NT_SUCCESS(Status))
        return Status;
    LowerDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = IoCreateDevice(DriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &UpperDevice);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(LowerDevice);
        return Status;
    }

    AttachedDevice = IoAttachDeviceToDeviceStack(UpperDevice, LowerDevice);
    ok_eq_pointer(AttachedDevice, LowerDevice);
    if (!AttachedDevice)
    {
        IoDeleteDevice(UpperDevice);
        IoDeleteDevice(LowerDevice);
        return STATUS_UNSUCCESSFUL;
    }
    UpperDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

static
VOID
DeleteTestDevices(VOID)
{
    IoDetachDevice(LowerDevice);
    IoDeleteDevice(UpperDevice);
    IoDeleteDevice(LowerDevice);
    UpperDevice = NULL;
    LowerDevice = NULL;
}

static
NTSTATUS
PowerIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStackLocation)
{
    BOOLEAN Ours;

    Ours = (DeviceObject == UpperDevice || DeviceObject == LowerDevice) &&
           IoStackLocation->MinorFunction == IRP_MN_SET_POWER &&
           IoStackLocation->Parameters.Power.Type == DevicePowerState;
    if (!Ours)
    {
        InterlockedIncrement(&StrayIrpCount);
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    if (DeviceObject == UpperDevice)
    {
        /* The power manager sends to the top of the stack */
        InterlockedExchange(&IdleIrpUpperFirst, 1);
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(LowerDevice, Irp);
    }

    /* The bottom device completes it, as a driver does that enters the state */
    IdleIrpState = IoStackLocation->Parameters.Power.State.DeviceState;
    PoSetPowerState(DeviceObject, DevicePowerState, IoStackLocation->Parameters.Power.State);
    KeQuerySystemTime(&IdleIrpTime);
    InterlockedIncrement(&IdleIrpCount);
    PoStartNextPowerIrp(Irp);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    KeSetEvent(&IdleIrpEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

static
VOID
ResetIdleIrp(VOID)
{
    KeClearEvent(&IdleIrpEvent);
    IdleIrpCount = 0;
    IdleIrpUpperFirst = 0;
    IdleIrpState = PowerDeviceUnspecified;
}

/* Waits for the idle IRP; returns the milliseconds from Start, or -1 */
static
LONG
WaitForIdleIrp(
    _In_ PLARGE_INTEGER Start,
    _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Timeout.QuadPart = -10000LL * TimeoutMs;
    Status = KeWaitForSingleObject(&IdleIrpEvent, Executive, KernelMode, FALSE, &Timeout);
    if (Status != STATUS_SUCCESS)
        return -1;
    return (LONG)((IdleIrpTime.QuadPart - Start->QuadPart) / 10000);
}

static
VOID
WaitMs(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static
VOID
TestIdleDetection(VOID)
{
    PULONG IdleCounter, Again;
    POWER_STATE PowerState;
    LARGE_INTEGER Start;
    LONG Elapsed;
    ULONG i;

    KmtRegisterIrpHandler(IRP_MJ_POWER, NULL, PowerIrpHandler);
    KeInitializeEvent(&IdleIrpEvent, NotificationEvent, FALSE);

    /* Only disks have standard time-outs */
    IdleCounter = PoRegisterDeviceForIdleDetection(LowerDevice, (ULONG)-1, (ULONG)-1, PowerDeviceD3);
    ok_eq_pointer(IdleCounter, NULL);

    /* Zero time-outs on a device that never registered */
    IdleCounter = PoRegisterDeviceForIdleDetection(LowerDevice, 0, 0, PowerDeviceD3);
    ok_eq_pointer(IdleCounter, NULL);

    /* One second, then D3 */
    ResetIdleIrp();
    KeQuerySystemTime(&Start);
    IdleCounter = PoRegisterDeviceForIdleDetection(LowerDevice, 1, 1, PowerDeviceD3);
    ok(IdleCounter != NULL, "PoRegisterDeviceForIdleDetection returned NULL\n");
    if (skip(IdleCounter != NULL, "No idle counter\n"))
        goto Exit;
    ok_eq_ulong(*IdleCounter, 0UL);

    Elapsed = WaitForIdleIrp(&Start, 10000);
    ok(Elapsed >= 0, "No idle IRP within 10 s\n");
    trace("Idle IRP after %ld ms\n", Elapsed);
    ok_eq_long(IdleIrpCount, 1L);
    ok_eq_long(IdleIrpUpperFirst, 1L);
    ok_eq_int(IdleIrpState, PowerDeviceD3);
    ok_eq_long(StrayIrpCount, 0L);

    /* A device that stays idle gets it once */
    ResetIdleIrp();
    WaitMs(4000);
    ok_eq_long(IdleIrpCount, 0L);

    /* A new registration changes the values */
    Again = PoRegisterDeviceForIdleDetection(LowerDevice, 3, 3, PowerDeviceD2);
    ok_eq_pointer(Again, IdleCounter);

    /* The device is used again: its driver powers it up and keeps it busy */
    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(LowerDevice, DevicePowerState, PowerState);
    PoSetDeviceBusy(IdleCounter);
    ResetIdleIrp();
    /* While the device is busy it is not powered down */
    for (i = 0; i < 60; i++)
    {
        PoSetDeviceBusy(IdleCounter);
        WaitMs(100);
    }
    ok_eq_long(IdleIrpCount, 0L);
    ok(*IdleCounter <= 1, "Idle counter is %lu right after busy\n", *IdleCounter);

    /* Idle again: D2 once the 3 s passed */
    PoSetDeviceBusy(IdleCounter);
    KeQuerySystemTime(&Start);
    Elapsed = WaitForIdleIrp(&Start, 15000);
    ok(Elapsed >= 0, "No idle IRP within 15 s\n");
    ok(Elapsed >= 1500, "Idle IRP after %ld ms, the time-out is 3 s\n", Elapsed);
    trace("Idle IRP after %ld ms\n", Elapsed);
    ok_eq_long(IdleIrpCount, 1L);
    ok_eq_int(IdleIrpState, PowerDeviceD2);

    /* Zero time-outs cancel idle detection */
    Again = PoRegisterDeviceForIdleDetection(LowerDevice, 0, 0, PowerDeviceD3);
    ok_eq_pointer(Again, NULL);
    ResetIdleIrp();
    WaitMs(5000);
    ok_eq_long(IdleIrpCount, 0L);

    /* Register again and delete the device: nothing is sent to it */
    IdleCounter = PoRegisterDeviceForIdleDetection(LowerDevice, 2, 2, PowerDeviceD3);
    ok(IdleCounter != NULL, "PoRegisterDeviceForIdleDetection returned NULL\n");

Exit:
    DeleteTestDevices();
    ResetIdleIrp();
    WaitMs(4000);
    ok_eq_long(IdleIrpCount, 0L);
    ok_eq_long(StrayIrpCount, 0L);

    KmtUnregisterIrpHandler(IRP_MJ_POWER, NULL, PowerIrpHandler);
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(RegistryPath);

    TestDriverObject = DriverObject;

    *DeviceName = L"PoIdle";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE;

    KmtRegisterMessageHandler(0, NULL, TestMessageHandler);

    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();
}

static
NTSTATUS
TestMessageHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    NTSTATUS Status;

    PAGED_CODE();

    switch (ControlCode)
    {
        case IOCTL_RUN_TEST:
            Status = CreateTestDevices(TestDriverObject);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (!NT_SUCCESS(Status))
                return Status;

            TestIdleDetection();
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}
