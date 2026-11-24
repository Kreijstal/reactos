/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite battery class and power source test
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A battery miniport of our own on top of battc.
 *
 * Waits: IOCTL_BATTERY_QUERY_STATUS with a time-out and IOCTL_BATTERY_QUERY_TAG
 * with a wait stay pending in the class driver (the dispatch routine returns
 * STATUS_PENDING) until the battery leaves the state given in
 * BATTERY_WAIT_STATUS, gets a tag, times out or the IRP is cancelled; a status
 * that already differs completes at once; a miniport that cannot notify is
 * polled.
 *
 * Power source: once the battery is a system battery (GUID_DEVICE_BATTERY),
 * the power manager reports GUID_ACDC_POWER_SOURCE as it switches between AC
 * and battery power, without anybody asking for the battery state.
 */

#include <kmt_test.h>
#include <batclass.h>
#include "PoBattery.h"

typedef NTSTATUS (NTAPI *PFN_REGISTER_SETTING)(PDEVICE_OBJECT, LPCGUID, PPOWER_SETTING_CALLBACK, PVOID, PVOID *);
typedef NTSTATUS (NTAPI *PFN_UNREGISTER_SETTING)(PVOID);

static const GUID TestGuidAcDcPowerSource =
    { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };

#define TEST_CAPACITY      100000
#define TEST_TAG           7

static PDRIVER_OBJECT TestDriverObject;
static KMT_MESSAGE_HANDLER TestMessageHandler;

/* The battery our miniport reports */
typedef struct _TEST_BATTERY
{
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT LowerDevice;
    PVOID ClassData;
    KSPIN_LOCK Lock;
    ULONG Tag;
    ULONG PowerState;
    ULONG Capacity;
    BOOLEAN CanNotify;
    LONG SetNotifyCount;
    LONG DisableNotifyCount;
} TEST_BATTERY, *PTEST_BATTERY;

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
ULONG
ElapsedMs(
    _In_ ULONGLONG Start)
{
    return (ULONG)((KeQueryInterruptTime() - Start) / 10000);
}

static
VOID
SetBatteryState(
    _In_ PTEST_BATTERY Battery,
    _In_ ULONG Tag,
    _In_ ULONG PowerState,
    _In_ BOOLEAN Notify)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Battery->Lock, &OldIrql);
    Battery->Tag = Tag;
    Battery->PowerState = PowerState;
    KeReleaseSpinLock(&Battery->Lock, OldIrql);

    if (Notify)
        BatteryClassStatusNotify(Battery->ClassData);
}

/* MINIPORT ******************************************************************/

static
NTSTATUS
NTAPI
TestQueryTag(
    _In_ PVOID Context,
    _Out_ PULONG BatteryTag)
{
    PTEST_BATTERY Battery = Context;

    ok_irql(PASSIVE_LEVEL);
    *BatteryTag = Battery->Tag;
    return Battery->Tag ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

static
NTSTATUS
NTAPI
TestQueryInformation(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ BATTERY_QUERY_INFORMATION_LEVEL Level,
    _In_ LONG AtRate,
    _Out_writes_bytes_to_(BufferLength, *ReturnedLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG ReturnedLength)
{
    PTEST_BATTERY Battery = Context;
    BATTERY_INFORMATION Information;
    BATTERY_REPORTING_SCALE Scale;
    ULONG Time = BATTERY_UNKNOWN_TIME;
    PVOID Source;
    ULONG Length;

    UNREFERENCED_PARAMETER(AtRate);

    *ReturnedLength = 0;
    if (BatteryTag != Battery->Tag || !Battery->Tag)
        return STATUS_NO_SUCH_DEVICE;

    switch (Level)
    {
        case BatteryInformation:
            RtlZeroMemory(&Information, sizeof(Information));
            Information.Capabilities = BATTERY_SYSTEM_BATTERY;
            Information.Technology = 1;
            RtlCopyMemory(Information.Chemistry, "LION", 4);
            Information.DesignedCapacity = TEST_CAPACITY;
            Information.FullChargedCapacity = TEST_CAPACITY;
            Information.DefaultAlert1 = TEST_CAPACITY / 10;
            Information.DefaultAlert2 = TEST_CAPACITY / 20;
            Source = &Information;
            Length = sizeof(Information);
            break;

        case BatteryGranularityInformation:
            Scale.Granularity = 1;
            Scale.Capacity = TEST_CAPACITY;
            Source = &Scale;
            Length = sizeof(Scale);
            break;

        case BatteryEstimatedTime:
            Source = &Time;
            Length = sizeof(Time);
            break;

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (BufferLength < Length)
        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(Buffer, Source, Length);
    *ReturnedLength = Length;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
TestQueryStatus(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _Out_ PBATTERY_STATUS BatteryStatus)
{
    PTEST_BATTERY Battery = Context;
    KIRQL OldIrql;
    NTSTATUS Status = STATUS_SUCCESS;

    ok_irql(PASSIVE_LEVEL);

    KeAcquireSpinLock(&Battery->Lock, &OldIrql);
    if (BatteryTag != Battery->Tag || !Battery->Tag)
    {
        Status = STATUS_NO_SUCH_DEVICE;
    }
    else
    {
        BatteryStatus->PowerState = Battery->PowerState;
        BatteryStatus->Capacity = Battery->Capacity;
        BatteryStatus->Voltage = 12000;
        BatteryStatus->Rate = (Battery->PowerState & BATTERY_DISCHARGING) ? -10000 : 5000;
    }
    KeReleaseSpinLock(&Battery->Lock, OldIrql);
    return Status;
}

static
NTSTATUS
NTAPI
TestSetStatusNotify(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ PBATTERY_NOTIFY BatteryNotify)
{
    PTEST_BATTERY Battery = Context;

    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(BatteryNotify);

    ok_irql(PASSIVE_LEVEL);
    InterlockedIncrement(&Battery->SetNotifyCount);
    return Battery->CanNotify ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
NTAPI
TestDisableStatusNotify(
    _In_ PVOID Context)
{
    PTEST_BATTERY Battery = Context;

    ok_irql(PASSIVE_LEVEL);
    InterlockedIncrement(&Battery->DisableNotifyCount);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
TestSetInformation(
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ BATTERY_SET_INFORMATION_LEVEL Level,
    _In_opt_ PVOID Buffer)
{
    return STATUS_NOT_SUPPORTED;
}

static TEST_BATTERY WaitBattery;
static TEST_BATTERY SystemBattery;

static
PTEST_BATTERY
BatteryFromDevice(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    if (DeviceObject == WaitBattery.DeviceObject)
        return &WaitBattery;
    if (DeviceObject == SystemBattery.DeviceObject)
        return &SystemBattery;
    return NULL;
}

static
NTSTATUS
BatteryIoctlHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStackLocation)
{
    PTEST_BATTERY Battery = BatteryFromDevice(DeviceObject);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(IoStackLocation);

    Status = BatteryClassIoctl(Battery->ClassData, Irp);
    if (Status == STATUS_NOT_SUPPORTED)
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    return Status;
}

/* The battery on the root-enumerated device passes PnP and power on to it */
static
NTSTATUS
BatteryPnpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStackLocation)
{
    PTEST_BATTERY Battery = BatteryFromDevice(DeviceObject);

    UNREFERENCED_PARAMETER(IoStackLocation);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Battery->LowerDevice, Irp);
}

static
NTSTATUS
BatteryPowerHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStackLocation)
{
    PTEST_BATTERY Battery = BatteryFromDevice(DeviceObject);

    UNREFERENCED_PARAMETER(IoStackLocation);

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Battery->LowerDevice, Irp);
}

static
NTSTATUS
CreateBattery(
    _Out_ PTEST_BATTERY Battery,
    _In_opt_ PDEVICE_OBJECT Pdo)
{
    BATTERY_MINIPORT_INFO MiniportInfo;
    NTSTATUS Status;

    RtlZeroMemory(Battery, sizeof(*Battery));
    KeInitializeSpinLock(&Battery->Lock);
    Battery->Tag = TEST_TAG;
    Battery->PowerState = BATTERY_POWER_ON_LINE | BATTERY_CHARGING;
    Battery->Capacity = TEST_CAPACITY / 2;
    Battery->CanNotify = TRUE;

    Status = IoCreateDevice(TestDriverObject, 0, NULL, FILE_DEVICE_BATTERY, 0, FALSE, &Battery->DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;
    Battery->DeviceObject->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    if (Pdo)
    {
        Battery->LowerDevice = IoAttachDeviceToDeviceStack(Battery->DeviceObject, Pdo);
        ok(Battery->LowerDevice != NULL, "Cannot attach to the PDO\n");
        if (!Battery->LowerDevice)
        {
            IoDeleteDevice(Battery->DeviceObject);
            return STATUS_UNSUCCESSFUL;
        }
        KmtRegisterIrpHandler(IRP_MJ_PNP, Battery->DeviceObject, BatteryPnpHandler);
        KmtRegisterIrpHandler(IRP_MJ_POWER, Battery->DeviceObject, BatteryPowerHandler);
    }
    KmtRegisterIrpHandler(IRP_MJ_DEVICE_CONTROL, Battery->DeviceObject, BatteryIoctlHandler);
    Battery->DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    RtlZeroMemory(&MiniportInfo, sizeof(MiniportInfo));
    MiniportInfo.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    MiniportInfo.MinorVersion = BATTERY_CLASS_MINOR_VERSION;
    MiniportInfo.Context = Battery;
    MiniportInfo.QueryTag = TestQueryTag;
    MiniportInfo.QueryInformation = TestQueryInformation;
    MiniportInfo.SetInformation = TestSetInformation;
    MiniportInfo.QueryStatus = TestQueryStatus;
    MiniportInfo.SetStatusNotify = TestSetStatusNotify;
    MiniportInfo.DisableStatusNotify = TestDisableStatusNotify;
    MiniportInfo.Pdo = Pdo;

    /* With a PDO this makes it a system battery (GUID_DEVICE_BATTERY) */
    Status = BatteryClassInitializeDevice(&MiniportInfo, &Battery->ClassData);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return Status;
}

static
VOID
DeleteBattery(
    _Inout_ PTEST_BATTERY Battery)
{
    NTSTATUS Status;

    Status = BatteryClassUnload(Battery->ClassData);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Battery->ClassData = NULL;

    KmtUnregisterIrpHandler(IRP_MJ_DEVICE_CONTROL, Battery->DeviceObject, BatteryIoctlHandler);
    if (Battery->LowerDevice)
    {
        KmtUnregisterIrpHandler(IRP_MJ_PNP, Battery->DeviceObject, BatteryPnpHandler);
        KmtUnregisterIrpHandler(IRP_MJ_POWER, Battery->DeviceObject, BatteryPowerHandler);
        IoDetachDevice(Battery->LowerDevice);
    }
    IoDeleteDevice(Battery->DeviceObject);
    Battery->DeviceObject = NULL;
}

/* WAITS *********************************************************************/

/* One IOCTL, sent from a thread of its own so a dispatch routine that blocks cannot hang the test */
typedef struct _TEST_REQUEST
{
    PTEST_BATTERY Battery;
    PIRP Irp;
    ULONG IoControlCode;
    union
    {
        BATTERY_WAIT_STATUS Wait;
        ULONG Tag;
        BATTERY_STATUS Status;
    } Buffer;
    ULONG InputLength;
    ULONG OutputLength;
    NTSTATUS CallStatus;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Returned;
    KEVENT Completed;
    ULONGLONG Start;
} TEST_REQUEST, *PTEST_REQUEST;

static IO_COMPLETION_ROUTINE RequestCompletion;

static
NTSTATUS
NTAPI
RequestCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PTEST_REQUEST Request = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(&Request->Completed, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static KSTART_ROUTINE RequestThread;

static
VOID
NTAPI
RequestThread(
    _In_ PVOID Context)
{
    PTEST_REQUEST Request = Context;

    Request->CallStatus = IoCallDriver(Request->Battery->DeviceObject, Request->Irp);
    KeSetEvent(&Request->Returned, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
BOOLEAN
StartRequest(
    _Out_ PTEST_REQUEST Request,
    _In_ PTEST_BATTERY Battery,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID Input,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength)
{
    PIO_STACK_LOCATION IoStack;
    HANDLE Thread;
    NTSTATUS Status;

    RtlZeroMemory(Request, sizeof(*Request));
    Request->Battery = Battery;
    Request->IoControlCode = IoControlCode;
    Request->InputLength = InputLength;
    Request->OutputLength = OutputLength;
    if (Input)
        RtlCopyMemory(&Request->Buffer, Input, InputLength);
    KeInitializeEvent(&Request->Returned, NotificationEvent, FALSE);
    KeInitializeEvent(&Request->Completed, NotificationEvent, FALSE);

    Request->Irp = IoAllocateIrp(Battery->DeviceObject->StackSize, FALSE);
    ok(Request->Irp != NULL, "IoAllocateIrp failed\n");
    if (!Request->Irp)
        return FALSE;
    Request->Irp->AssociatedIrp.SystemBuffer = &Request->Buffer;
    Request->Irp->Flags = IRP_BUFFERED_IO;
    Request->Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoStack = IoGetNextIrpStackLocation(Request->Irp);
    IoStack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    IoStack->Parameters.DeviceIoControl.InputBufferLength = InputLength;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = OutputLength;
    IoSetCompletionRoutine(Request->Irp, RequestCompletion, Request, TRUE, TRUE, TRUE);

    Request->Start = KeQueryInterruptTime();
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, RequestThread, Request);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        IoFreeIrp(Request->Irp);
        return FALSE;
    }
    ZwClose(Thread);
    return TRUE;
}

static
BOOLEAN
WaitEvent(
    _In_ PKEVENT Event,
    _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10000LL * TimeoutMs;
    return KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}

/* Whatever happened, the request is over when this returns */
static
VOID
FinishRequest(
    _Inout_ PTEST_REQUEST Request)
{
    /* A dispatch routine that blocks is let go by a state change */
    if (!WaitEvent(&Request->Completed, 10000))
    {
        ok(FALSE, "Request %lx is still not complete; changing the battery state\n", Request->IoControlCode);
        SetBatteryState(Request->Battery, TEST_TAG, BATTERY_DISCHARGING, TRUE);
        KeWaitForSingleObject(&Request->Completed, Executive, KernelMode, FALSE, NULL);
    }
    KeWaitForSingleObject(&Request->Returned, Executive, KernelMode, FALSE, NULL);
    Request->IoStatus = Request->Irp->IoStatus;
    IoFreeIrp(Request->Irp);
    Request->Irp = NULL;
}

static
VOID
StatusWait(
    _Out_ PBATTERY_WAIT_STATUS Wait,
    _In_ ULONG PowerState,
    _In_ ULONG Timeout)
{
    Wait->BatteryTag = TEST_TAG;
    Wait->Timeout = Timeout;
    Wait->PowerState = PowerState;
    Wait->LowCapacity = TEST_CAPACITY / 2;
    Wait->HighCapacity = TEST_CAPACITY / 2;
}

static
VOID
TestBatteryWaits(VOID)
{
    PTEST_BATTERY Battery = &WaitBattery;
    const ULONG OnLine = BATTERY_POWER_ON_LINE | BATTERY_CHARGING;
    static TEST_REQUEST Request;
    BATTERY_WAIT_STATUS Wait;
    ULONG WaitTime;
    NTSTATUS Status;

    Status = CreateBattery(Battery, NULL);
    if (!NT_SUCCESS(Status))
        return;

    /* No time-out: the current status at once */
    StatusWait(&Wait, OnLine, 0);
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Completed, 2000), "Status query without a time-out did not complete\n");
        FinishRequest(&Request);
        ok_eq_hex(Request.CallStatus, STATUS_SUCCESS);
        ok_eq_hex(Request.IoStatus.Status, STATUS_SUCCESS);
        ok_eq_ulong(Request.Buffer.Status.PowerState, OnLine);
        ok_eq_ulong(Request.Buffer.Status.Capacity, (ULONG)TEST_CAPACITY / 2);
    }

    /* A wait is pending until the power state changes */
    StatusWait(&Wait, OnLine, (ULONG)-1);
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Returned, 2000), "The dispatch routine blocks in a status wait\n");
        ok_eq_hex(Request.CallStatus, STATUS_PENDING);
        ok(!WaitEvent(&Request.Completed, 1000), "The wait completed without a change\n");
        ok(Battery->SetNotifyCount > 0, "The miniport was not asked to notify\n");

        SetBatteryState(Battery, TEST_TAG, BATTERY_DISCHARGING, TRUE);
        ok(WaitEvent(&Request.Completed, 3000), "The wait did not complete on the change\n");
        FinishRequest(&Request);
        ok_eq_hex(Request.IoStatus.Status, STATUS_SUCCESS);
        ok_eq_ulong(Request.Buffer.Status.PowerState, (ULONG)BATTERY_DISCHARGING);
    }
    SetBatteryState(Battery, TEST_TAG, OnLine, FALSE);

    /* A status that already differs completes at once */
    StatusWait(&Wait, BATTERY_DISCHARGING, 5000);
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Completed, 1500), "A wait for a state already left did not complete at once\n");
        FinishRequest(&Request);
        ok_eq_hex(Request.CallStatus, STATUS_SUCCESS);
        ok_eq_ulong(Request.Buffer.Status.PowerState, OnLine);
    }

    /* A capacity outside the range completes it too */
    StatusWait(&Wait, OnLine, (ULONG)-1);
    Wait.LowCapacity = TEST_CAPACITY / 4;
    Wait.HighCapacity = TEST_CAPACITY / 2;
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Returned, 2000), "The dispatch routine blocks in a status wait\n");
        ok(!WaitEvent(&Request.Completed, 500), "The wait completed without a change\n");
        Battery->Capacity = TEST_CAPACITY / 2 + 1;
        BatteryClassStatusNotify(Battery->ClassData);
        ok(WaitEvent(&Request.Completed, 3000), "The wait did not complete on a capacity above the range\n");
        FinishRequest(&Request);
        ok_eq_ulong(Request.Buffer.Status.Capacity, (ULONG)TEST_CAPACITY / 2 + 1);
        Battery->Capacity = TEST_CAPACITY / 2;
    }

    /* The time-out ends a wait with the current status */
    StatusWait(&Wait, OnLine, 700);
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Returned, 2000), "The dispatch routine blocks in a status wait\n");
        ok(WaitEvent(&Request.Completed, 5000), "The wait did not time out\n");
        WaitTime = ElapsedMs(Request.Start);
        ok(WaitTime >= 600, "The 700 ms wait ended after %lu ms\n", WaitTime);
        FinishRequest(&Request);
        ok_eq_hex(Request.IoStatus.Status, STATUS_SUCCESS);
        ok_eq_ulong(Request.Buffer.Status.PowerState, OnLine);
    }

    /* A pending wait can be cancelled */
    StatusWait(&Wait, OnLine, (ULONG)-1);
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Returned, 2000), "The dispatch routine blocks in a status wait\n");
        IoCancelIrp(Request.Irp);
        ok(WaitEvent(&Request.Completed, 2000), "The cancelled wait did not complete\n");
        FinishRequest(&Request);
        ok_eq_hex(Request.IoStatus.Status, STATUS_CANCELLED);
    }

    /* A miniport that cannot notify is polled */
    Battery->CanNotify = FALSE;
    StatusWait(&Wait, OnLine, (ULONG)-1);
    Wait.HighCapacity = TEST_CAPACITY;
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Returned, 2000), "The dispatch routine blocks in a status wait\n");
        ok(!WaitEvent(&Request.Completed, 500), "The wait completed without a change\n");
        SetBatteryState(Battery, TEST_TAG, BATTERY_DISCHARGING, FALSE);
        ok(WaitEvent(&Request.Completed, 12000), "A battery that cannot notify was not polled\n");
        FinishRequest(&Request);
        ok_eq_ulong(Request.Buffer.Status.PowerState, (ULONG)BATTERY_DISCHARGING);
    }
    Battery->CanNotify = TRUE;
    SetBatteryState(Battery, TEST_TAG, OnLine, FALSE);

    /* A tag wait: pending while there is no battery, done when one comes */
    SetBatteryState(Battery, 0, OnLine, FALSE);
    WaitTime = (ULONG)-1;
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_TAG, &WaitTime, sizeof(WaitTime), sizeof(ULONG)))
    {
        ok(WaitEvent(&Request.Returned, 2000), "The dispatch routine blocks in a tag wait\n");
        ok_eq_hex(Request.CallStatus, STATUS_PENDING);
        ok(!WaitEvent(&Request.Completed, 500), "The tag wait completed without a battery\n");
        SetBatteryState(Battery, TEST_TAG, OnLine, TRUE);
        ok(WaitEvent(&Request.Completed, 3000), "The tag wait did not complete for the new battery\n");
        FinishRequest(&Request);
        ok_eq_hex(Request.IoStatus.Status, STATUS_SUCCESS);
        ok_eq_ulong(Request.Buffer.Tag, (ULONG)TEST_TAG);
    }

    /* A tag that is gone ends the status wait */
    StatusWait(&Wait, OnLine, (ULONG)-1);
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Returned, 2000), "The dispatch routine blocks in a status wait\n");
        SetBatteryState(Battery, TEST_TAG + 1, OnLine, TRUE);
        ok(WaitEvent(&Request.Completed, 3000), "The wait did not end when the tag changed\n");
        FinishRequest(&Request);
        ok_eq_hex(Request.IoStatus.Status, STATUS_NO_SUCH_DEVICE);
    }
    SetBatteryState(Battery, TEST_TAG, OnLine, FALSE);

    /* Unloading the class data ends the waits */
    StatusWait(&Wait, OnLine, (ULONG)-1);
    if (StartRequest(&Request, Battery, IOCTL_BATTERY_QUERY_STATUS, &Wait, sizeof(Wait), sizeof(BATTERY_STATUS)))
    {
        ok(WaitEvent(&Request.Returned, 2000), "The dispatch routine blocks in a status wait\n");
        if (Request.CallStatus == STATUS_PENDING)
        {
            DeleteBattery(Battery);
            ok(WaitEvent(&Request.Completed, 2000), "Unloading did not end the wait\n");
            FinishRequest(&Request);
            ok_eq_hex(Request.IoStatus.Status, STATUS_DEVICE_REMOVED);
            return;
        }
        FinishRequest(&Request);
    }

    DeleteBattery(Battery);
}

/* POWER SOURCE **************************************************************/

typedef struct _SOURCE_CONTEXT
{
    KEVENT Changed;
    volatile LONG Count;
    volatile LONG Source;
} SOURCE_CONTEXT, *PSOURCE_CONTEXT;

static
NTSTATUS
NTAPI
SourceCallback(
    _In_ LPCGUID SettingGuid,
    _In_ PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID Context)
{
    PSOURCE_CONTEXT Ctx = Context;

    ok(IsEqualGUID(SettingGuid, &TestGuidAcDcPowerSource), "Callback for another setting\n");
    ok_eq_ulong(ValueLength, (ULONG)sizeof(ULONG));
    if (ValueLength == sizeof(ULONG))
        InterlockedExchange(&Ctx->Source, *(PLONG)Value);
    InterlockedIncrement(&Ctx->Count);
    KeSetEvent(&Ctx->Changed, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

/* Waits until the power manager reports Source; nobody queries the battery meanwhile */
static
BOOLEAN
WaitForSource(
    _In_ PSOURCE_CONTEXT Ctx,
    _In_ LONG Source,
    _In_ ULONG TimeoutMs)
{
    ULONGLONG Start = KeQueryInterruptTime();

    while (Ctx->Source != Source)
    {
        ULONG Elapsed = ElapsedMs(Start);

        if (Elapsed >= TimeoutMs)
            return FALSE;
        KeClearEvent(&Ctx->Changed);
        if (Ctx->Source == Source)
            break;
        WaitEvent(&Ctx->Changed, TimeoutMs - Elapsed);
    }
    trace("Power source %ld after %lu ms\n", Source, ElapsedMs(Start));
    return TRUE;
}

static
VOID
TestPowerSource(VOID)
{
    PFN_REGISTER_SETTING pPoRegisterPowerSettingCallback;
    PFN_UNREGISTER_SETTING pPoUnregisterPowerSettingCallback;
    PTEST_BATTERY Battery = &SystemBattery;
    const ULONG OnLine = BATTERY_POWER_ON_LINE | BATTERY_CHARGING;
    static SOURCE_CONTEXT Ctx;
    PDEVICE_OBJECT Pdo = NULL;
    UNICODE_STRING Name;
    PVOID Handle = NULL;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, L"PoRegisterPowerSettingCallback");
    pPoRegisterPowerSettingCallback = (PFN_REGISTER_SETTING)MmGetSystemRoutineAddress(&Name);
    RtlInitUnicodeString(&Name, L"PoUnregisterPowerSettingCallback");
    pPoUnregisterPowerSettingCallback = (PFN_UNREGISTER_SETTING)MmGetSystemRoutineAddress(&Name);
    if (skip(pPoRegisterPowerSettingCallback && pPoUnregisterPowerSettingCallback, "No power setting callbacks\n"))
        return;

    KeInitializeEvent(&Ctx.Changed, NotificationEvent, FALSE);
    Ctx.Source = -1;
    Status = pPoRegisterPowerSettingCallback(NULL, &TestGuidAcDcPowerSource, SourceCallback, &Ctx, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    ok(WaitForSource(&Ctx, PoAc, 5000), "Not on AC power before the battery came (%ld)\n", Ctx.Source);

    /* A root-enumerated device to put the battery on */
    Status = IoReportDetectedDevice(TestDriverObject, InterfaceTypeUndefined, -1, -1,
                                    NULL, NULL, FALSE, &Pdo);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Status = CreateBattery(Battery, Pdo);
    if (!NT_SUCCESS(Status))
        goto Exit;

    /* On AC power with the battery charging */
    WaitMs(2000);
    ok_eq_long(Ctx.Source, (LONG)PoAc);

    /* The charger goes: the power manager hears of it by itself */
    SetBatteryState(Battery, TEST_TAG, BATTERY_DISCHARGING, TRUE);
    ok(WaitForSource(&Ctx, PoDc, 15000), "The switch to battery power was not reported (%ld)\n", Ctx.Source);

    /* And back */
    SetBatteryState(Battery, TEST_TAG, OnLine, TRUE);
    ok(WaitForSource(&Ctx, PoAc, 15000), "The switch back to AC power was not reported (%ld)\n", Ctx.Source);

    DeleteBattery(Battery);

    /* The battery is gone; the system stays on AC */
    WaitMs(2000);
    ok_eq_long(Ctx.Source, (LONG)PoAc);

Exit:
    /* Whatever happened, leave the system on AC power */
    if (Ctx.Source != PoAc)
        trace("Power source left at %ld\n", Ctx.Source);
    Status = pPoUnregisterPowerSettingCallback(Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

/* DRIVER ********************************************************************/

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

    *DeviceName = L"PoBattery";
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
    PAGED_CODE();

    switch (ControlCode)
    {
        case IOCTL_TEST_BATTERY_WAITS:
            TestBatteryWaits();
            break;

        case IOCTL_TEST_POWER_SOURCE:
            TestPowerSource();
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}
