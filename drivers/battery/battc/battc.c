/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            drivers/battery/battc/battc.c
 * PURPOSE:         Battery Class Driver
 * PROGRAMMERS:     Cameron Gutman (cameron.gutman@reactos.org)
 */

#include <battc.h>

#define NDEBUG
#include <debug.h>

/*
 * IOCTL_BATTERY_QUERY_TAG and IOCTL_BATTERY_QUERY_STATUS with a time-out are
 * waits: the IRP stays pending until the battery has a tag, or until its
 * status leaves what the caller passed in BATTERY_WAIT_STATUS (another power
 * state, a capacity below LowCapacity or above HighCapacity), or until the
 * time-out. The miniport is asked to notify the class (SetStatusNotify) and
 * calls BatteryClassStatusNotify when something changed; a miniport that
 * cannot notify is polled. The waits are checked again on a worker thread,
 * because the miniport callbacks run at PASSIVE_LEVEL and a notification may
 * come at DISPATCH_LEVEL.
 */

static WORKER_THREAD_ROUTINE BattcWaitWorker;
static KDEFERRED_ROUTINE BattcWaitTimerDpc;
static DRIVER_CANCEL BattcCancelWait;

NTSTATUS
NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject,
            PUNICODE_STRING RegistryPath)
{
    DPRINT("Battery class driver initialized\n");

    return STATUS_SUCCESS;
}

/* Has the battery left the state the caller waits on? */
static
BOOLEAN
BattcStatusChanged(
    _In_ PBATTERY_WAIT_STATUS Wait,
    _In_ PBATTERY_STATUS Status)
{
    return Wait->PowerState != Status->PowerState ||
           Wait->HighCapacity < Status->Capacity ||
           Wait->LowCapacity > Status->Capacity;
}

static
ULONGLONG
BattcDeadline(
    _In_ ULONG TimeoutMs)
{
    if (TimeoutMs == (ULONG)-1)
        return MAXULONGLONG;
    return KeQueryInterruptTime() + (ULONGLONG)TimeoutMs * 10000;
}

/* Runs the wait worker, or has the running one go round once more; the caller holds the wait lock */
static
VOID
BattcQueueWorkerLocked(
    _In_ PBATTERY_CLASS_DATA BattClass)
{
    if (BattClass->WorkerRunning)
    {
        BattClass->WorkerRerun = TRUE;
    }
    else if (!BattClass->WorkerQueued && !BattClass->Unloading)
    {
        BattClass->WorkerQueued = TRUE;
        KeClearEvent(&BattClass->WorkerIdleEvent);
        ExQueueWorkItem(&BattClass->WaitWorkItem, DelayedWorkQueue);
    }
}

static
VOID
BattcQueueWorker(
    _In_ PBATTERY_CLASS_DATA BattClass)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    BattcQueueWorkerLocked(BattClass);
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
}

static
VOID
NTAPI
BattcWaitTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    BattcQueueWorker(DeferredContext);
}

static
VOID
BattcCompleteWait(
    _In_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    ExFreePoolWithTag(Irp->Tail.Overlay.DriverContext[0], BATTERY_WAIT_TAG);
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
VOID
NTAPI
BattcCancelWait(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PBATTERY_CLASS_DATA BattClass = Irp->Tail.Overlay.DriverContext[1];
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    /* Whoever cleared the cancel routine first owns the IRP: here, us */
    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    /* The miniport no longer needs to notify about this one */
    BattcQueueWorkerLocked(BattClass);
    /* Unloading may free the class data once the list is empty: not touched below */
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    BattcCompleteWait(Irp, STATUS_CANCELLED, 0);
}

/* Puts a wait on a list; FALSE if it was cancelled and the caller must complete it */
static
BOOLEAN
BattcQueueWait(
    _In_ PBATTERY_CLASS_DATA BattClass,
    _In_ PLIST_ENTRY List,
    _In_ PIRP Irp)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    IoSetCancelRoutine(Irp, BattcCancelWait);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL)
    {
        KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
        return FALSE;
    }
    /* If the cancel routine already runs, it takes the IRP off the list */
    InsertTailList(List, &Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
    return TRUE;
}

/* Moves the waits of a list that are not being cancelled to a local list */
static
VOID
BattcTakeWaits(
    _In_ PLIST_ENTRY List,
    _Out_ PLIST_ENTRY Taken)
{
    PLIST_ENTRY Entry, Next;
    PIRP Irp;

    InitializeListHead(Taken);
    for (Entry = List->Flink; Entry != List; Entry = Next)
    {
        Next = Entry->Flink;
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(Irp, NULL) == NULL)
            continue;
        RemoveEntryList(Entry);
        InsertTailList(Taken, Entry);
    }
}

static
VOID
BattcRequeueWait(
    _In_ PBATTERY_CLASS_DATA BattClass,
    _In_ PLIST_ENTRY List,
    _In_ PIRP Irp)
{
    if (!BattcQueueWait(BattClass, List, Irp))
        BattcCompleteWait(Irp, STATUS_CANCELLED, 0);
}

/* Tells the miniport what the status waits need and sets the next wake-up */
static
VOID
BattcArmWaits(
    _In_ PBATTERY_CLASS_DATA BattClass)
{
    PBATTERY_WAIT_CONTEXT WaitContext;
    BATTERY_NOTIFY Notify = { 0 };
    ULONGLONG Next = MAXULONGLONG, Now;
    ULONG Tag = 0;
    BOOLEAN HaveStatusWaits = FALSE, Mixed = FALSE, Poll;
    PLIST_ENTRY Entry;
    LARGE_INTEGER DueTime;
    KIRQL OldIrql;
    NTSTATUS Status;

    PAGED_CODE();

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    for (Entry = BattClass->StatusWaits.Flink; Entry != &BattClass->StatusWaits; Entry = Entry->Flink)
    {
        WaitContext = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry)->Tail.Overlay.DriverContext[0];
        if (!HaveStatusWaits)
        {
            HaveStatusWaits = TRUE;
            Tag = WaitContext->Wait.BatteryTag;
            Notify.PowerState = WaitContext->Wait.PowerState;
            Notify.LowCapacity = WaitContext->Wait.LowCapacity;
            Notify.HighCapacity = WaitContext->Wait.HighCapacity;
        }
        else
        {
            /* One notification serves the waits that agree on tag and power state */
            if (WaitContext->Wait.BatteryTag != Tag || WaitContext->Wait.PowerState != Notify.PowerState)
                Mixed = TRUE;
            Notify.LowCapacity = max(Notify.LowCapacity, WaitContext->Wait.LowCapacity);
            Notify.HighCapacity = min(Notify.HighCapacity, WaitContext->Wait.HighCapacity);
        }
        Next = min(Next, WaitContext->Deadline);
    }
    for (Entry = BattClass->TagWaits.Flink; Entry != &BattClass->TagWaits; Entry = Entry->Flink)
    {
        WaitContext = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry)->Tail.Overlay.DriverContext[0];
        Next = min(Next, WaitContext->Deadline);
    }
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    if (!HaveStatusWaits)
    {
        if (BattClass->NotifySet)
            BattClass->MiniportInfo.DisableStatusNotify(BattClass->MiniportInfo.Context);
        BattClass->NotifySet = FALSE;
        BattClass->NotifyFailed = FALSE;
    }
    else if ((!BattClass->NotifySet && !BattClass->NotifyFailed) ||
             BattClass->NotifyTag != Tag ||
             RtlCompareMemory(&BattClass->Notify, &Notify, sizeof(Notify)) != sizeof(Notify))
    {
        /* Only when it changed: a miniport may notify at once from SetStatusNotify */
        BattClass->NotifyTag = Tag;
        BattClass->Notify = Notify;
        Status = BattClass->MiniportInfo.SetStatusNotify(BattClass->MiniportInfo.Context, Tag, &Notify);
        BattClass->NotifySet = NT_SUCCESS(Status);
        BattClass->NotifyFailed = !NT_SUCCESS(Status);
        if (!NT_SUCCESS(Status))
            DPRINT("SetStatusNotify failed (0x%lx), polling the battery\n", Status);
    }

    Now = KeQueryInterruptTime();
    Poll = HaveStatusWaits && (BattClass->NotifyFailed || Mixed);
    if (Poll)
        Next = min(Next, Now + (ULONGLONG)BATTERY_POLL_INTERVAL_MS * 10000);

    if (Next == MAXULONGLONG)
    {
        KeCancelTimer(&BattClass->WaitTimer);
    }
    else
    {
        DueTime.QuadPart = (Next > Now) ? -(LONGLONG)(Next - Now) : -1;
        KeSetTimer(&BattClass->WaitTimer, DueTime, &BattClass->WaitTimerDpc);
    }
}

/* Completes the waits that are over and keeps the others */
static
VOID
BattcCheckWaits(
    _In_ PBATTERY_CLASS_DATA BattClass)
{
    LIST_ENTRY TagWaits, StatusWaits;
    PBATTERY_WAIT_CONTEXT WaitContext;
    BATTERY_STATUS BattStatus;
    PLIST_ENTRY Entry;
    ULONGLONG Now;
    ULONG Tag;
    KIRQL OldIrql;
    PIRP Irp;
    NTSTATUS Status;

    PAGED_CODE();

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    BattcTakeWaits(&BattClass->TagWaits, &TagWaits);
    BattcTakeWaits(&BattClass->StatusWaits, &StatusWaits);
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    Now = KeQueryInterruptTime();

    while (!IsListEmpty(&TagWaits))
    {
        Entry = RemoveHeadList(&TagWaits);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        WaitContext = Irp->Tail.Overlay.DriverContext[0];

        Status = BattClass->MiniportInfo.QueryTag(BattClass->MiniportInfo.Context, &Tag);
        if (NT_SUCCESS(Status))
        {
            *(PULONG)Irp->AssociatedIrp.SystemBuffer = Tag;
            BattcCompleteWait(Irp, STATUS_SUCCESS, sizeof(ULONG));
        }
        else if (Now >= WaitContext->Deadline)
        {
            BattcCompleteWait(Irp, STATUS_NO_SUCH_DEVICE, 0);
        }
        else
        {
            BattcRequeueWait(BattClass, &BattClass->TagWaits, Irp);
        }
    }

    while (!IsListEmpty(&StatusWaits))
    {
        Entry = RemoveHeadList(&StatusWaits);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        WaitContext = Irp->Tail.Overlay.DriverContext[0];

        RtlZeroMemory(&BattStatus, sizeof(BattStatus));
        Status = BattClass->MiniportInfo.QueryStatus(BattClass->MiniportInfo.Context,
                                                     WaitContext->Wait.BatteryTag,
                                                     &BattStatus);
        if (!NT_SUCCESS(Status))
        {
            /* E.g. STATUS_NO_SUCH_DEVICE: the battery has another tag now */
            BattcCompleteWait(Irp, Status, 0);
        }
        else if (BattcStatusChanged(&WaitContext->Wait, &BattStatus) || Now >= WaitContext->Deadline)
        {
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &BattStatus, sizeof(BattStatus));
            BattcCompleteWait(Irp, STATUS_SUCCESS, sizeof(BattStatus));
        }
        else
        {
            BattcRequeueWait(BattClass, &BattClass->StatusWaits, Irp);
        }
    }
}

static
VOID
NTAPI
BattcWaitWorker(
    _In_ PVOID Parameter)
{
    PBATTERY_CLASS_DATA BattClass = Parameter;
    BOOLEAN Rerun;
    KIRQL OldIrql;

    PAGED_CODE();

    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    ASSERT(BattClass->WorkerQueued && !BattClass->WorkerRunning);
    BattClass->WorkerQueued = FALSE;
    BattClass->WorkerRunning = TRUE;
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

    do
    {
        KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
        BattClass->WorkerRerun = FALSE;
        KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

        BattcCheckWaits(BattClass);
        BattcArmWaits(BattClass);

        KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
        Rerun = BattClass->WorkerRerun && !BattClass->Unloading;
        if (!Rerun)
        {
            BattClass->WorkerRunning = FALSE;
            KeSetEvent(&BattClass->WorkerIdleEvent, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
    } while (Rerun);
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassUnload(PVOID ClassData)
{
    PBATTERY_CLASS_DATA BattClass;
    LIST_ENTRY TagWaits, StatusWaits;
    LARGE_INTEGER Interval;
    BOOLEAN Empty;
    KIRQL OldIrql;
    PIRP Irp;

    DPRINT("Battery %p is being unloaded\n", ClassData);

    BattClass = ClassData;

    /* No more worker runs, and none left running */
    KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
    BattClass->Unloading = TRUE;
    KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
    KeCancelTimer(&BattClass->WaitTimer);
    KeFlushQueuedDpcs();
    KeWaitForSingleObject(&BattClass->WorkerIdleEvent, Executive, KernelMode, FALSE, NULL);

    /* The battery is gone for those still waiting */
    for (;;)
    {
        KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
        BattcTakeWaits(&BattClass->TagWaits, &TagWaits);
        BattcTakeWaits(&BattClass->StatusWaits, &StatusWaits);
        KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);

        while (!IsListEmpty(&TagWaits))
        {
            Irp = CONTAINING_RECORD(RemoveHeadList(&TagWaits), IRP, Tail.Overlay.ListEntry);
            BattcCompleteWait(Irp, STATUS_DEVICE_REMOVED, 0);
        }
        while (!IsListEmpty(&StatusWaits))
        {
            Irp = CONTAINING_RECORD(RemoveHeadList(&StatusWaits), IRP, Tail.Overlay.ListEntry);
            BattcCompleteWait(Irp, STATUS_DEVICE_REMOVED, 0);
        }

        /* A wait being cancelled is taken off by its cancel routine */
        KeAcquireSpinLock(&BattClass->WaitLock, &OldIrql);
        Empty = IsListEmpty(&BattClass->TagWaits) && IsListEmpty(&BattClass->StatusWaits);
        KeReleaseSpinLock(&BattClass->WaitLock, OldIrql);
        if (Empty)
            break;

        Interval.QuadPart = -10 * 1000 * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }

    if (BattClass->NotifySet)
        BattClass->MiniportInfo.DisableStatusNotify(BattClass->MiniportInfo.Context);

    if (BattClass->InterfaceName.Length != 0)
    {
        IoSetDeviceInterfaceState(&BattClass->InterfaceName, FALSE);
        RtlFreeUnicodeString(&BattClass->InterfaceName);
    }

    ExFreePoolWithTag(BattClass,
                      BATTERY_CLASS_DATA_TAG);

    return STATUS_SUCCESS;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassSystemControl(PVOID ClassData,
                          PVOID WmiLibContext,
                          PDEVICE_OBJECT DeviceObject,
                          PIRP Irp,
                          PVOID Disposition)
{
    UNIMPLEMENTED;

    return STATUS_WMI_GUID_NOT_FOUND;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassQueryWmiDataBlock(PVOID ClassData,
                              PDEVICE_OBJECT DeviceObject,
                              PIRP Irp,
                              ULONG GuidIndex,
                              PULONG InstanceLengthArray,
                              ULONG OutBufferSize,
                              PUCHAR Buffer)
{
    UNIMPLEMENTED;

    return STATUS_WMI_GUID_NOT_FOUND;
}

/* The miniport says the tag or status may have changed; any IRQL up to DISPATCH_LEVEL */
BCLASSAPI
NTSTATUS
NTAPI
BatteryClassStatusNotify(
    _In_ PVOID ClassData)
{
    DPRINT("Received battery status notification from %p\n", ClassData);

    BattcQueueWorker(ClassData);
    return STATUS_SUCCESS;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassInitializeDevice(PBATTERY_MINIPORT_INFO MiniportInfo,
                             PVOID *ClassData)
{
    NTSTATUS Status;
    PBATTERY_CLASS_DATA BattClass;

    BattClass = ExAllocatePoolWithTag(NonPagedPool,
                                      sizeof(BATTERY_CLASS_DATA),
                                      BATTERY_CLASS_DATA_TAG);
    if (BattClass == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(BattClass, sizeof(BATTERY_CLASS_DATA));

    RtlCopyMemory(&BattClass->MiniportInfo,
                  MiniportInfo,
                  sizeof(BattClass->MiniportInfo));

    KeInitializeSpinLock(&BattClass->WaitLock);
    InitializeListHead(&BattClass->TagWaits);
    InitializeListHead(&BattClass->StatusWaits);
    ExInitializeWorkItem(&BattClass->WaitWorkItem, BattcWaitWorker, BattClass);
    KeInitializeEvent(&BattClass->WorkerIdleEvent, NotificationEvent, TRUE);
    KeInitializeTimer(&BattClass->WaitTimer);
    KeInitializeDpc(&BattClass->WaitTimerDpc, BattcWaitTimerDpc, BattClass);

    /*
     * Publish the class data to the caller BEFORE advertising the device
     * interface below. IoSetDeviceInterfaceState(..., TRUE) fires a PnP
     * interface-arrival notification, and a consumer (e.g. compbatt or the
     * power manager) can immediately open the interface and send an IOCTL
     * (IOCTL_BATTERY_QUERY_TAG) back to the miniport. The miniport dispatch
     * (CmBattIoctl) forwards it here via BatteryClassIoctl(ClassData, ...),
     * so ClassData must already be visible or that call dereferences NULL
     * (KM AV at offset 8 -> bugcheck 0x7E). Storing it first closes the
     * window; the object is fully constructed at this point.
     */
    *ClassData = BattClass;

    if (MiniportInfo->Pdo != NULL)
    {
        Status = IoRegisterDeviceInterface(MiniportInfo->Pdo,
                                           &GUID_DEVICE_BATTERY,
                                           NULL,
                                           &BattClass->InterfaceName);
        if (NT_SUCCESS(Status))
        {
            DPRINT("Initialized battery interface: %wZ\n", &BattClass->InterfaceName);
            Status = IoSetDeviceInterfaceState(&BattClass->InterfaceName, TRUE);
            if (Status == STATUS_OBJECT_NAME_EXISTS)
            {
                DPRINT1("Got STATUS_OBJECT_NAME_EXISTS for SetDeviceInterfaceState\n");
                Status = STATUS_SUCCESS;
            }
        }
        else
        {
            DPRINT1("IoRegisterDeviceInterface failed (0x%x)\n", Status);
        }
    }

    return STATUS_SUCCESS;
}

/* Leaves a wait pending; returns STATUS_PENDING */
static
NTSTATUS
BattcPendWait(
    _In_ PBATTERY_CLASS_DATA BattClass,
    _In_ PLIST_ENTRY List,
    _In_ PIRP Irp,
    _In_ PBATTERY_WAIT_CONTEXT WaitContext)
{
    Irp->Tail.Overlay.DriverContext[0] = WaitContext;
    Irp->Tail.Overlay.DriverContext[1] = BattClass;
    IoMarkIrpPending(Irp);
    if (!BattcQueueWait(BattClass, List, Irp))
    {
        BattcCompleteWait(Irp, STATUS_CANCELLED, 0);
        return STATUS_PENDING;
    }

    /* Have the miniport notify, and the time-out armed */
    BattcQueueWorker(BattClass);
    return STATUS_PENDING;
}

BCLASSAPI
NTSTATUS
NTAPI
BatteryClassIoctl(PVOID ClassData,
                  PIRP Irp)
{
    PBATTERY_CLASS_DATA BattClass;
    PIO_STACK_LOCATION IrpSp;
    NTSTATUS Status;
    ULONG WaitTime;
    PBATTERY_WAIT_CONTEXT WaitContext;
    PBATTERY_QUERY_INFORMATION BattQueryInfo;
    PBATTERY_SET_INFORMATION BattSetInfo;
    PBATTERY_STATUS BattStatus;
    ULONG ReturnedLength;

    DPRINT("BatteryClassIoctl(%p %p)\n", ClassData, Irp);

    BattClass = ClassData;

    IrpSp = IoGetCurrentIrpStackLocation(Irp);
    Irp->IoStatus.Information = 0;

    DPRINT("Received IOCTL %x for %p\n", IrpSp->Parameters.DeviceIoControl.IoControlCode,
           ClassData);

    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_BATTERY_QUERY_TAG:
            if ((IrpSp->Parameters.DeviceIoControl.InputBufferLength != sizeof(ULONG) && IrpSp->Parameters.DeviceIoControl.InputBufferLength != 0) ||
                IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            WaitTime = IrpSp->Parameters.DeviceIoControl.InputBufferLength == sizeof(ULONG) ? *(PULONG)Irp->AssociatedIrp.SystemBuffer : 0;

            Status = BattClass->MiniportInfo.QueryTag(BattClass->MiniportInfo.Context,
                                                      (PULONG)Irp->AssociatedIrp.SystemBuffer);
            if (NT_SUCCESS(Status))
            {
                Irp->IoStatus.Information = sizeof(ULONG);
                break;
            }

            /* No battery: wait for one, if the caller wants to */
            Status = STATUS_NO_SUCH_DEVICE;
            if (WaitTime == 0)
                break;

            WaitContext = ExAllocatePoolZero(NonPagedPool, sizeof(*WaitContext), BATTERY_WAIT_TAG);
            if (!WaitContext)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            WaitContext->Deadline = BattcDeadline(WaitTime);
            return BattcPendWait(BattClass, &BattClass->TagWaits, Irp, WaitContext);

        case IOCTL_BATTERY_QUERY_STATUS:
            if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(BATTERY_WAIT_STATUS) ||
                IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(BATTERY_STATUS))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            WaitContext = ExAllocatePoolZero(NonPagedPool, sizeof(*WaitContext), BATTERY_WAIT_TAG);
            if (!WaitContext)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            /* The system buffer gets the output */
            WaitContext->Wait = *(PBATTERY_WAIT_STATUS)Irp->AssociatedIrp.SystemBuffer;

            /* Zero the output buffer to prevent leakage of kernel data */
            BattStatus = Irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(BattStatus, sizeof(BATTERY_STATUS));

            Status = BattClass->MiniportInfo.QueryStatus(BattClass->MiniportInfo.Context,
                                                         WaitContext->Wait.BatteryTag,
                                                         BattStatus);

            /* Done at once without a time-out, on failure, or if it already changed */
            if (WaitContext->Wait.Timeout == 0 || !NT_SUCCESS(Status) ||
                BattcStatusChanged(&WaitContext->Wait, BattStatus))
            {
                ExFreePoolWithTag(WaitContext, BATTERY_WAIT_TAG);
                if (NT_SUCCESS(Status))
                    Irp->IoStatus.Information = sizeof(BATTERY_STATUS);
                break;
            }

            WaitContext->Deadline = BattcDeadline(WaitContext->Wait.Timeout);
            return BattcPendWait(BattClass, &BattClass->StatusWaits, Irp, WaitContext);

        case IOCTL_BATTERY_QUERY_INFORMATION:
            if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*BattQueryInfo))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            BattQueryInfo = Irp->AssociatedIrp.SystemBuffer;

            Status = BattClass->MiniportInfo.QueryInformation(BattClass->MiniportInfo.Context,
                                                              BattQueryInfo->BatteryTag,
                                                              BattQueryInfo->InformationLevel,
                                                              BattQueryInfo->AtRate,
                                                              Irp->AssociatedIrp.SystemBuffer,
                                                              IrpSp->Parameters.DeviceIoControl.OutputBufferLength,
                                                              &ReturnedLength);
            Irp->IoStatus.Information = ReturnedLength;
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("QueryInformation failed (0x%x)\n", Status);
            }
            break;

        case IOCTL_BATTERY_SET_INFORMATION:
            if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*BattSetInfo))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            BattSetInfo = Irp->AssociatedIrp.SystemBuffer;

            Status = BattClass->MiniportInfo.SetInformation(BattClass->MiniportInfo.Context,
                                                            BattSetInfo->BatteryTag,
                                                            BattSetInfo->InformationLevel,
                                                            BattSetInfo->Buffer);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("SetInformation failed (0x%x)\n", Status);
            }
            break;

        default:
            DPRINT1("Received unsupported IRP %x\n", IrpSp->Parameters.DeviceIoControl.IoControlCode);
            /* Do NOT complete the irp */
            return STATUS_NOT_SUPPORTED;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}
