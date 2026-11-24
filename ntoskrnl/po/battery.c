/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Following the composite battery for AC/DC switches
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * Once the system has a battery, the power manager keeps one request pending
 * on the composite battery: IOCTL_BATTERY_QUERY_TAG until it has a tag, then
 * IOCTL_BATTERY_QUERY_STATUS waiting for the power state to leave the last one
 * seen. The battery class completes it when the battery (or the AC adapter)
 * reports a change, so a switch between AC and battery power reaches
 * GUID_ACDC_POWER_SOURCE without anybody asking for the battery state.
 *
 * Everything but the completion routine runs on the power setting worker
 * thread, which waits on PopBatteryWaitEvent.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

typedef enum _POP_BATTERY_WAIT
{
    PopBatteryWaitNone,
    PopBatteryWaitTag,
    PopBatteryWaitFirstStatus,
    PopBatteryWaitStatus
} POP_BATTERY_WAIT;

static const UNICODE_STRING PopCompositeBatteryName =
    RTL_CONSTANT_STRING(L"\\Device\\CompositeBattery");

static POP_BATTERY_WAIT PopBatteryWait;
static HANDLE PopBatteryHandle;
static PFILE_OBJECT PopBatteryFile;
static PDEVICE_OBJECT PopBatteryDevice;
static PIRP PopBatteryIrp;
static ULONG PopBatteryTag;
KEVENT PopBatteryWaitEvent;

static union
{
    ULONG Tag;
    BATTERY_WAIT_STATUS Wait;
    BATTERY_STATUS Status;
} PopBatteryBuffer;

/* PRIVATE FUNCTIONS *********************************************************/

static IO_COMPLETION_ROUTINE PopBatteryIrpCompletion;

static
NTSTATUS
NTAPI
PopBatteryIrpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Context);

    /* The worker looks at it; the IRP is ours to reuse */
    KeSetEvent(&PopBatteryWaitEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
VOID
PopSendBatteryRequest(
    _In_ POP_BATTERY_WAIT Wait,
    _In_ ULONG IoControlCode,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength)
{
    PIO_STACK_LOCATION IoStack;

    PAGED_CODE();

    IoReuseIrp(PopBatteryIrp, STATUS_NOT_SUPPORTED);
    PopBatteryIrp->AssociatedIrp.SystemBuffer = &PopBatteryBuffer;
    PopBatteryIrp->Flags = IRP_BUFFERED_IO;
    PopBatteryIrp->Tail.Overlay.Thread = PsGetCurrentThread();
    PopBatteryIrp->Tail.Overlay.OriginalFileObject = PopBatteryFile;

    IoStack = IoGetNextIrpStackLocation(PopBatteryIrp);
    IoStack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    IoStack->FileObject = PopBatteryFile;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    IoStack->Parameters.DeviceIoControl.InputBufferLength = InputLength;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = OutputLength;
    IoSetCompletionRoutine(PopBatteryIrp, PopBatteryIrpCompletion, NULL, TRUE, TRUE, TRUE);

    PopBatteryWait = Wait;
    IoCallDriver(PopBatteryDevice, PopBatteryIrp);
}

static
VOID
PopWaitForBatteryTag(VOID)
{
    /* Until a battery is there */
    PopBatteryBuffer.Tag = (ULONG)-1;
    PopSendBatteryRequest(PopBatteryWaitTag, IOCTL_BATTERY_QUERY_TAG, sizeof(ULONG), sizeof(ULONG));
}

static
VOID
PopWaitForBatteryStatus(
    _In_ POP_BATTERY_WAIT Wait,
    _In_ ULONG PowerState)
{
    RtlZeroMemory(&PopBatteryBuffer, sizeof(PopBatteryBuffer));
    PopBatteryBuffer.Wait.BatteryTag = PopBatteryTag;
    PopBatteryBuffer.Wait.PowerState = PowerState;
    if (Wait == PopBatteryWaitFirstStatus)
    {
        /* The current status, to know what to wait on */
        PopBatteryBuffer.Wait.Timeout = 0;
    }
    else
    {
        /* Only a power state change; capacities do not matter here */
        PopBatteryBuffer.Wait.Timeout = (ULONG)-1;
        PopBatteryBuffer.Wait.LowCapacity = 0;
        PopBatteryBuffer.Wait.HighCapacity = MAXULONG;
    }
    PopSendBatteryRequest(Wait,
                          IOCTL_BATTERY_QUERY_STATUS,
                          sizeof(BATTERY_WAIT_STATUS),
                          sizeof(BATTERY_STATUS));
}

static
VOID
PopStopBatteryWait(
    _In_ NTSTATUS Status)
{
    PAGED_CODE();

    DPRINT1("No longer following the composite battery (0x%lx)\n", Status);
    IoFreeIrp(PopBatteryIrp);
    PopBatteryIrp = NULL;
    ObDereferenceObject(PopBatteryFile);
    PopBatteryFile = NULL;
    PopBatteryDevice = NULL;
    ZwClose(PopBatteryHandle);
    PopBatteryHandle = NULL;
    PopBatteryWait = PopBatteryWaitNone;
}

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
PopInitializeBatteryWait(VOID)
{
    KeInitializeEvent(&PopBatteryWaitEvent, SynchronizationEvent, FALSE);
    PopBatteryWait = PopBatteryWaitNone;
}

/* Starts following the composite battery if the system has a battery */
VOID
NTAPI
PopStartBatteryWait(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    PAGED_CODE();

    if (PopBatteryWait != PopBatteryWaitNone || !PopCapabilities.SystemBatteriesPresent)
        return;

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)&PopCompositeBatteryName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwOpenFile(&PopBatteryHandle,
                        FILE_READ_DATA,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Cannot open the composite battery (0x%lx)\n", Status);
        PopBatteryHandle = NULL;
        return;
    }

    Status = ObReferenceObjectByHandle(PopBatteryHandle,
                                       FILE_READ_DATA,
                                       IoFileObjectType,
                                       KernelMode,
                                       (PVOID*)&PopBatteryFile,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(PopBatteryHandle);
        PopBatteryHandle = NULL;
        return;
    }

    PopBatteryDevice = IoGetRelatedDeviceObject(PopBatteryFile);
    PopBatteryIrp = IoAllocateIrp(PopBatteryDevice->StackSize, FALSE);
    if (!PopBatteryIrp)
    {
        ObDereferenceObject(PopBatteryFile);
        PopBatteryFile = NULL;
        ZwClose(PopBatteryHandle);
        PopBatteryHandle = NULL;
        return;
    }

    PopWaitForBatteryTag();
}

/* The pending request completed: take the news and wait again */
VOID
NTAPI
PopBatteryWaitCompleted(VOID)
{
    NTSTATUS Status;

    PAGED_CODE();
    ASSERT(PopBatteryWait != PopBatteryWaitNone);

    Status = PopBatteryIrp->IoStatus.Status;
    switch (PopBatteryWait)
    {
        case PopBatteryWaitTag:
            if (!NT_SUCCESS(Status))
            {
                PopStopBatteryWait(Status);
                return;
            }
            PopBatteryTag = PopBatteryBuffer.Tag;
            PopWaitForBatteryStatus(PopBatteryWaitFirstStatus, 0);
            return;

        case PopBatteryWaitFirstStatus:
        case PopBatteryWaitStatus:
            if (Status == STATUS_NO_SUCH_DEVICE)
            {
                /* The batteries changed: wait for the new tag */
                PopWaitForBatteryTag();
                return;
            }
            if (!NT_SUCCESS(Status))
            {
                PopStopBatteryWait(Status);
                return;
            }

            DPRINT("Composite battery power state 0x%lx\n", PopBatteryBuffer.Status.PowerState);
            PopBatteryStateChanged(BooleanFlagOn(PopBatteryBuffer.Status.PowerState, BATTERY_POWER_ON_LINE));
            PopWaitForBatteryStatus(PopBatteryWaitStatus, PopBatteryBuffer.Status.PowerState);
            return;

        default:
            ASSERT(FALSE);
            return;
    }
}
