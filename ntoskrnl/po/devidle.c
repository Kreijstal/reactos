/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device idle detection (PoRegisterDeviceForIdleDetection)
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A driver registers a device with two idle time-outs in seconds and the
 * device power state to request once the device has been idle that long.
 * The power manager hands out the device's idle counter; PoSetDeviceBusy
 * zeroes it whenever the device is used. Once a second the power manager
 * counts every registered device one second further, and when the counter
 * of a device reaches the time-out that applies (the performance time-out
 * on AC power, the conservation time-out on battery), it sends an
 * IRP_MN_SET_POWER for the registered state to the top of the device's
 * stack. That happens once per idle period: the next IRP needs the device
 * to be busy again first.
 *
 * A disk or mass storage device may register -1 to use the power policy's
 * disk time-out (GUID_DISK_POWERDOWN_TIMEOUT of the active scheme, 0 for
 * never). Registering with both time-outs 0 cancels idle detection, and so
 * does deleting the device object.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* Devices registered for idle detection, protected by PopDopeGlobalLock */
static LIST_ENTRY PopIdleDetectList;
static KTIMER PopIdleScanTimer;
static KDPC PopIdleScanDpc;
static WORK_QUEUE_ITEM PopIdleScanWorkItem;
static BOOLEAN PopIdleScanQueued;
static BOOLEAN PopIdleScanRunning;

/* Written by the power setting callbacks, read by the scan */
static volatile ULONG PopIdlePowerSource = PoAc;
static volatile ULONG PopIdleDiskTimeout = 0;

#define POP_IDLE_SCAN_PERIOD_MS 1000

/* PRIVATE FUNCTIONS *********************************************************/

static
ULONG
PopIdleTimeout(
    _In_ PDEVICE_OBJECT_POWER_EXTENSION Dope)
{
    ULONG Timeout;

    /* On AC power the system optimizes for performance */
    if (PopIdlePowerSource == PoAc)
        Timeout = Dope->PerformanceIdleTime;
    else
        Timeout = Dope->ConservationIdleTime;

    /* The disk time-out of the power policy, 0 when disks never idle */
    if (Timeout == (ULONG)-1)
        Timeout = PopIdleDiskTimeout;

    return Timeout;
}

/* Counts every registered device one second further; called with the lock held */
static
BOOLEAN
PopIdleCountDevices(VOID)
{
    PLIST_ENTRY Entry;
    PDEVICE_OBJECT_POWER_EXTENSION Dope;
    ULONG Timeout, Count;
    BOOLEAN Expired = FALSE;

    for (Entry = PopIdleDetectList.Flink;
         Entry != &PopIdleDetectList;
         Entry = Entry->Flink)
    {
        Dope = CONTAINING_RECORD(Entry, DEVICE_OBJECT_POWER_EXTENSION, IdleList);
        Timeout = PopIdleTimeout(Dope);

        /* PoSetDeviceBusy zeroes the counter without the lock */
        Count = *(volatile ULONG *)&Dope->IdleCount;

        /* Below the time-out, the device was busy since the last request */
        if (Count < Timeout || Timeout == 0)
            Dope->IdleRequested = FALSE;

        if (Count != MAXULONG)
            Count = InterlockedIncrement((PLONG)&Dope->IdleCount);

        if (Timeout != 0 && Count >= Timeout && !Dope->IdleRequested)
        {
            Dope->IdleRequested = TRUE;
            Dope->IdlePending = TRUE;
            Expired = TRUE;
        }
    }

    return Expired;
}

static
VOID
NTAPI
PopIdleScanWorker(
    _In_ PVOID Context)
{
    PLIST_ENTRY Entry;
    PDEVICE_OBJECT_POWER_EXTENSION Dope;
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_NODE DeviceNode;
    POWER_STATE PowerState;
    NTSTATUS Status;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Context);
    PAGED_CODE();

    for (;;)
    {
        /* Take one device whose time-out expired; the IRP is sent without the lock */
        DeviceObject = NULL;
        KeAcquireSpinLock(&PopDopeGlobalLock, &OldIrql);
        for (Entry = PopIdleDetectList.Flink;
             Entry != &PopIdleDetectList;
             Entry = Entry->Flink)
        {
            Dope = CONTAINING_RECORD(Entry, DEVICE_OBJECT_POWER_EXTENSION, IdleList);
            if (Dope->IdlePending)
            {
                Dope->IdlePending = FALSE;
                DeviceObject = Dope->DeviceObject;
                PowerState.DeviceState = Dope->State;

                /* Deleting the device unlinks it under the lock; the object stays until we are done */
                ObReferenceObject(DeviceObject);
                break;
            }
        }

        if (!DeviceObject)
        {
            PopIdleScanQueued = FALSE;
            KeReleaseSpinLock(&PopDopeGlobalLock, OldIrql);
            break;
        }
        KeReleaseSpinLock(&PopDopeGlobalLock, OldIrql);

        DeviceNode = IopGetDeviceNode(DeviceObject);
        DPRINT1("Device %p (%wZ) idle, requesting D%d\n", DeviceObject,
                DeviceNode ? &DeviceNode->InstancePath : &DeviceObject->DriverObject->DriverName,
                PowerState.DeviceState - 1);
        Status = PoRequestPowerIrp(DeviceObject,
                                   IRP_MN_SET_POWER,
                                   PowerState,
                                   NULL,
                                   NULL,
                                   NULL);
        if (!NT_SUCCESS(Status))
            DPRINT1("Idle power IRP for %p failed 0x%lx\n", DeviceObject, Status);

        ObDereferenceObject(DeviceObject);
    }
}

static
VOID
NTAPI
PopIdleScanDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeAcquireSpinLockAtDpcLevel(&PopDopeGlobalLock);

    if (PopIdleCountDevices() && !PopIdleScanQueued)
    {
        /* The power IRPs go out at PASSIVE_LEVEL */
        PopIdleScanQueued = TRUE;
        ExQueueWorkItem(&PopIdleScanWorkItem, DelayedWorkQueue);
    }

    KeReleaseSpinLockFromDpcLevel(&PopDopeGlobalLock);
}

static
NTSTATUS
NTAPI
PopIdleSettingCallback(
    _In_ LPCGUID SettingGuid,
    _In_reads_bytes_(ValueLength) PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    if (ValueLength != sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;

    if (IsEqualGUID(SettingGuid, &GUID_ACDC_POWER_SOURCE))
        PopIdlePowerSource = *(PULONG)Value;
    else if (IsEqualGUID(SettingGuid, &GUID_DISK_POWERDOWN_TIMEOUT))
        PopIdleDiskTimeout = *(PULONG)Value;

    return STATUS_SUCCESS;
}

CODE_SEG("INIT")
VOID
NTAPI
PopInitializeIdleDetection(VOID)
{
    InitializeListHead(&PopIdleDetectList);
    KeInitializeTimerEx(&PopIdleScanTimer, NotificationTimer);
    KeInitializeDpc(&PopIdleScanDpc, PopIdleScanDpcRoutine, NULL);
    ExInitializeWorkItem(&PopIdleScanWorkItem, PopIdleScanWorker, NULL);
}

/* Phase 1, before the power setting worker starts */
CODE_SEG("INIT")
VOID
NTAPI
PopStartIdleDetection(VOID)
{
    PVOID Handle;
    NTSTATUS Status;

    /* The worker reports both values once and again when they change */
    Status = PoRegisterPowerSettingCallback(NULL, &GUID_ACDC_POWER_SOURCE,
                                            PopIdleSettingCallback, NULL, &Handle);
    ASSERT(NT_SUCCESS(Status));
    Status = PoRegisterPowerSettingCallback(NULL, &GUID_DISK_POWERDOWN_TIMEOUT,
                                            PopIdleSettingCallback, NULL, &Handle);
    ASSERT(NT_SUCCESS(Status));
}

/* The device object is being deleted: no more idle IRPs for it */
VOID
NTAPI
PopRemoveIdleDevice(
    _In_ PDEVICE_OBJECT_POWER_EXTENSION Dope)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&PopDopeGlobalLock, &OldIrql);
    if (Dope->IdleList.Flink != &Dope->IdleList)
    {
        RemoveEntryList(&Dope->IdleList);
        InitializeListHead(&Dope->IdleList);
        Dope->IdlePending = FALSE;
    }
    KeReleaseSpinLock(&PopDopeGlobalLock, OldIrql);
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
PULONG
NTAPI
PoRegisterDeviceForIdleDetection(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ConservationIdleTime,
    _In_ ULONG PerformanceIdleTime,
    _In_ DEVICE_POWER_STATE State)
{
    PDEVICE_OBJECT_POWER_EXTENSION Dope;
    KIRQL OldIrql;

    PAGED_CODE();

    /* Both time-outs zero cancel idle detection */
    if (ConservationIdleTime == 0 && PerformanceIdleTime == 0)
    {
        Dope = IoGetDevObjExtension(DeviceObject)->Dope;
        if (Dope)
            PopRemoveIdleDevice(Dope);
        return NULL;
    }

    /* The policy time-outs exist for disks only */
    if ((ConservationIdleTime == (ULONG)-1 || PerformanceIdleTime == (ULONG)-1) &&
        DeviceObject->DeviceType != FILE_DEVICE_DISK &&
        DeviceObject->DeviceType != FILE_DEVICE_MASS_STORAGE)
    {
        DPRINT1("Device %p of type %lu has no standard idle time-outs\n",
                DeviceObject, DeviceObject->DeviceType);
        return NULL;
    }

    /* Idle detection powers a device down */
    if (State <= PowerDeviceD0 || State >= PowerDeviceMaximum)
    {
        DPRINT1("Device %p idle state %d is not a low-power state\n", DeviceObject, State);
        return NULL;
    }

    Dope = PopGetDope(DeviceObject);
    if (!Dope)
        return NULL;

    KeAcquireSpinLock(&PopDopeGlobalLock, &OldIrql);

    /* Only one registration per device: a new one changes the values */
    Dope->ConservationIdleTime = ConservationIdleTime;
    Dope->PerformanceIdleTime = PerformanceIdleTime;
    Dope->State = State;
    Dope->IdleCount = 0;
    Dope->IdleRequested = FALSE;
    if (Dope->IdleList.Flink == &Dope->IdleList)
        InsertTailList(&PopIdleDetectList, &Dope->IdleList);

    /* Count the registered devices once a second from now on */
    if (!PopIdleScanRunning)
    {
        LARGE_INTEGER DueTime;

        PopIdleScanRunning = TRUE;
        DueTime.QuadPart = -10000LL * POP_IDLE_SCAN_PERIOD_MS;
        KeSetTimerEx(&PopIdleScanTimer, DueTime, POP_IDLE_SCAN_PERIOD_MS, &PopIdleScanDpc);
    }

    KeReleaseSpinLock(&PopDopeGlobalLock, OldIrql);

    return &Dope->IdleCount;
}
