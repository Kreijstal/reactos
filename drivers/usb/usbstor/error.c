/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Storage Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     USB block storage device driver.
 * COPYRIGHT:   2005-2006 James Tabor
 *              2011-2012 Michael Martin (michael.martin@reactos.org)
 *              2011-2013 Johannes Anderwald (johannes.anderwald@reactos.org)
 *              2019 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include "usbstor.h"

#define NDEBUG
#include <debug.h>


//
// Bulk-Only-Transport breadcrumb trace.  See usbstor.h for why this exists.
//
static USBSTOR_TRACE_ENTRY UsbStorTraceRing[USBSTOR_TRACE_ENTRIES];
static LONG UsbStorTraceSeq = 0;
static LONG UsbStorTraceDumpsLeft = 8;

//
// Queued-versus-ran counters for every deferred recovery path.  A queued count
// that runs ahead of its ran count is a recovery that never executed.
//
static LONG UsbStorResetPipeQueued = 0;
static LONG UsbStorResetPipeRan = 0;
static LONG UsbStorResetDevQueued = 0;
static LONG UsbStorResetDevRan = 0;
static LONG UsbStorAbortQueued = 0;
static LONG UsbStorAbortRan = 0;
static ULONG UsbStorResettingTicks = 0;

static PCSTR UsbStorTraceNames[UsbStorTraceMax] =
{
    "none",
    "startio",
    "cbw>",
    "cbw<",
    "data>",
    "data<",
    "csw>",
    "csw<",
    "issue",
    "issue!",
    "sense",
    "q:rstpipe",
    "rstpipe{",
    "rstpipe}",
    "q:rstdev",
    "rstdev{",
    "rstdev}",
    "q:abort",
    "abort{",
    "abort}",
    "term",
    "next",
    "next-busy"
};

VOID
USBSTOR_Trace(
    IN ULONG Code,
    IN ULONG_PTR A,
    IN ULONG_PTR B)
{
    PUSBSTOR_TRACE_ENTRY Entry;
    LARGE_INTEGER Now;
    LONG Index;

    Index = InterlockedIncrement(&UsbStorTraceSeq) - 1;
    Entry = &UsbStorTraceRing[(ULONG)Index % USBSTOR_TRACE_ENTRIES];

    KeQuerySystemTime(&Now);

    Entry->Time = (ULONGLONG)Now.QuadPart;
    Entry->A = A;
    Entry->B = B;
    Entry->Code = Code;
}

static
VOID
USBSTOR_TraceFetch(
    IN LONG Index,
    IN LONG Seq,
    IN ULONGLONG Now,
    OUT PCSTR *Name,
    OUT PVOID *A,
    OUT PVOID *B,
    OUT PULONG AgeMs)
{
    PUSBSTOR_TRACE_ENTRY Entry;
    ULONG Code;

    if (Index >= Seq)
    {
        *Name = "-";
        *A = NULL;
        *B = NULL;
        *AgeMs = 0;
        return;
    }

    Entry = &UsbStorTraceRing[(ULONG)Index % USBSTOR_TRACE_ENTRIES];
    Code = Entry->Code < UsbStorTraceMax ? Entry->Code : 0;

    *Name = UsbStorTraceNames[Code];
    *A = (PVOID)Entry->A;
    *B = (PVOID)Entry->B;
    *AgeMs = (ULONG)((Now - Entry->Time) / 10000);
}

VOID
USBSTOR_DumpTrace(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PCSTR Reason)
{
    PIRP_CONTEXT Context;
    LARGE_INTEGER Now;
    LONG Seq, First, ix;

    if (InterlockedDecrement(&UsbStorTraceDumpsLeft) < 0)
    {
        InterlockedIncrement(&UsbStorTraceDumpsLeft);
        return;
    }

    Context = &FDODeviceExtension->CurrentIrpContext;
    KeQuerySystemTime(&Now);

    DPRINT1("[USBSTOR-TRACE] %s: fdo %p active %p ctxsrb %p irp %p flags %lx "
            "pending %lu stallretry %lu errhandling %lu ticks %lu\n",
            Reason,
            FDODeviceExtension,
            FDODeviceExtension->ActiveSrb,
            Context->Srb,
            Context->Irp,
            FDODeviceExtension->Flags,
            FDODeviceExtension->IrpPendingCount,
            Context->StallRetryCount,
            FDODeviceExtension->SrbErrorHandlingActive,
            FDODeviceExtension->TimerTicksOnActiveSrb);

    DPRINT1("[USBSTOR-TRACE] recovery: rstpipe q=%ld r=%ld | rstdev q=%ld r=%ld | "
            "abort q=%ld r=%ld | urb.func %x urb.status %x urb.len %lx\n",
            UsbStorResetPipeQueued, UsbStorResetPipeRan,
            UsbStorResetDevQueued, UsbStorResetDevRan,
            UsbStorAbortQueued, UsbStorAbortRan,
            Context->Urb.UrbHeader.Function,
            Context->Urb.UrbHeader.Status,
            Context->Urb.UrbBulkOrInterruptTransfer.TransferBufferLength);

    Seq = UsbStorTraceSeq;
    First = Seq > USBSTOR_TRACE_ENTRIES ? Seq - USBSTOR_TRACE_ENTRIES : 0;

    /*
     * Three events per line.  Every DPRINT1 costs milliseconds on a serial
     * port and this runs in the timer DPC at DISPATCH_LEVEL, so the line count
     * -- not the byte count -- is what has to stay small.
     */
    for (ix = First; ix < Seq; ix += 3)
    {
        PCSTR N0, N1, N2;
        PVOID A0, B0, A1, B1, A2, B2;
        ULONG G0, G1, G2;

        USBSTOR_TraceFetch(ix,     Seq, (ULONGLONG)Now.QuadPart, &N0, &A0, &B0, &G0);
        USBSTOR_TraceFetch(ix + 1, Seq, (ULONGLONG)Now.QuadPart, &N1, &A1, &B1, &G1);
        USBSTOR_TraceFetch(ix + 2, Seq, (ULONGLONG)Now.QuadPart, &N2, &A2, &B2, &G2);

        DPRINT1("[USBSTOR-TRACE] %3ld %s %p %p -%lu | %s %p %p -%lu | %s %p %p -%lu\n",
                ix,
                N0, A0, B0, G0,
                N1, A1, B1, G1,
                N2, A2, B2, G2);
    }
}

NTSTATUS
USBSTOR_GetEndpointStatus(
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR bEndpointAddress,
    OUT PUSHORT Value)
{
    PURB Urb;
    NTSTATUS Status;

    DPRINT("Allocating URB\n");
    Urb = (PURB)AllocateItem(NonPagedPool, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    if (!Urb)
    {
        DPRINT1("OutofMemory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // build status
    UsbBuildGetStatusRequest(Urb, URB_FUNCTION_GET_STATUS_FROM_ENDPOINT, bEndpointAddress & 0x0F, Value, NULL, NULL);

    // send the request
    DPRINT1("Sending Request DeviceObject %p, Urb %p\n", DeviceObject, Urb);
    Status = USBSTOR_SyncUrbRequest(DeviceObject, Urb);

    FreeItem(Urb);
    return Status;
}

NTSTATUS
USBSTOR_ResetPipeWithHandle(
    IN PDEVICE_OBJECT DeviceObject,
    IN USBD_PIPE_HANDLE PipeHandle)
{
    PURB Urb;
    NTSTATUS Status;

    DPRINT("Allocating URB\n");
    Urb = (PURB)AllocateItem(NonPagedPool, sizeof(struct _URB_PIPE_REQUEST));
    if (!Urb)
    {
        DPRINT1("OutofMemory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Urb->UrbPipeRequest.Hdr.Length = sizeof(struct _URB_PIPE_REQUEST);
    Urb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL;
    Urb->UrbPipeRequest.PipeHandle = PipeHandle;

    // send the request
    DPRINT1("Sending Request DeviceObject %p, Urb %p\n", DeviceObject, Urb);
    Status = USBSTOR_SyncUrbRequest(DeviceObject, Urb);

    FreeItem(Urb);
    return Status;
}

NTSTATUS
USBSTOR_AbortPipeWithHandle(
    IN PDEVICE_OBJECT DeviceObject,
    IN USBD_PIPE_HANDLE PipeHandle)
{
    PURB Urb;
    NTSTATUS Status;

    Urb = (PURB)AllocateItem(NonPagedPool, sizeof(struct _URB_PIPE_REQUEST));
    if (!Urb)
    {
        DPRINT1("OutofMemory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Urb->UrbPipeRequest.Hdr.Length = sizeof(struct _URB_PIPE_REQUEST);
    Urb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
    Urb->UrbPipeRequest.PipeHandle = PipeHandle;

    Status = USBSTOR_SyncUrbRequest(DeviceObject, Urb);
    DPRINT1("USBSTOR_AbortPipeWithHandle Status %x\n", Status);

    FreeItem(Urb);
    return Status;
}

VOID
NTAPI
USBSTOR_ResetPipeWorkItemRoutine(
    IN PDEVICE_OBJECT FdoDevice,
    IN PVOID Ctx)
{
    NTSTATUS Status;
    PFDO_DEVICE_EXTENSION FDODeviceExtension = (PFDO_DEVICE_EXTENSION)Ctx;
    PIRP_CONTEXT Context = &FDODeviceExtension->CurrentIrpContext;

    InterlockedIncrement(&UsbStorResetPipeRan);
    USBSTOR_Trace(UsbStorTraceResetPipeRun,
                  (ULONG_PTR)Context->Urb.UrbBulkOrInterruptTransfer.PipeHandle,
                  (ULONG_PTR)Context->Irp);

    // clear stall on the corresponding pipe
    Status = USBSTOR_ResetPipeWithHandle(FDODeviceExtension->LowerDeviceObject, Context->Urb.UrbBulkOrInterruptTransfer.PipeHandle);
    DPRINT1("USBSTOR_ResetPipeWithHandle Status %x\n", Status);

    USBSTOR_Trace(UsbStorTraceResetPipeEnd, (ULONG_PTR)Status, (ULONG_PTR)Context->Irp);

    // now resend the csw as the stall got cleared
    USBSTOR_SendCSWRequest(FDODeviceExtension, Context->Irp);
}

VOID
NTAPI
USBSTOR_ResetDeviceWorkItemRoutine(
    IN PDEVICE_OBJECT FdoDevice,
    IN PVOID Context)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    UINT32 ix;
    NTSTATUS Status;
    KIRQL OldIrql;

    DPRINT("USBSTOR_ResetDeviceWorkItemRoutine\n");

    FDODeviceExtension = FdoDevice->DeviceExtension;

    InterlockedIncrement(&UsbStorResetDevRan);
    USBSTOR_Trace(UsbStorTraceResetDevRun,
                  (ULONG_PTR)FDODeviceExtension->ActiveSrb,
                  (ULONG_PTR)FDODeviceExtension->Flags);

    for (ix = 0; ix < 3; ++ix)
    {
        // first perform a mass storage reset step 1 in 5.3.4 USB Mass Storage Bulk Only Specification
        Status = USBSTOR_ResetDevice(FDODeviceExtension->LowerDeviceObject, FDODeviceExtension);
        if (NT_SUCCESS(Status))
        {
            // step 2 reset bulk in pipe section 5.3.4
            Status = USBSTOR_ResetPipeWithHandle(FDODeviceExtension->LowerDeviceObject, FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkInPipeIndex].PipeHandle);
            if (NT_SUCCESS(Status))
            {
                // finally reset bulk out pipe
                Status = USBSTOR_ResetPipeWithHandle(FDODeviceExtension->LowerDeviceObject, FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkOutPipeIndex].PipeHandle);
                if (NT_SUCCESS(Status))
                {
                    break;
                }
            }
        }
    }

    KeAcquireSpinLock(&FDODeviceExtension->CommonLock, &OldIrql);
    FDODeviceExtension->Flags &= ~USBSTOR_FDO_FLAGS_DEVICE_RESETTING;
    KeReleaseSpinLock(&FDODeviceExtension->CommonLock, OldIrql);

    USBSTOR_Trace(UsbStorTraceResetDevEnd,
                  (ULONG_PTR)Status,
                  (ULONG_PTR)FDODeviceExtension->ActiveSrb);

    USBSTOR_QueueNextRequest(FdoDevice);
}

VOID
NTAPI
USBSTOR_QueueResetPipe(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension)
{
    DPRINT("USBSTOR_QueueResetPipe\n");

    InterlockedIncrement(&UsbStorResetPipeQueued);
    USBSTOR_Trace(UsbStorTraceQueueResetPipe,
                  (ULONG_PTR)FDODeviceExtension->ActiveSrb,
                  (ULONG_PTR)FDODeviceExtension->Flags);

    IoQueueWorkItem(FDODeviceExtension->ResetDeviceWorkItem,
                    USBSTOR_ResetPipeWorkItemRoutine,
                    CriticalWorkQueue,
                    FDODeviceExtension);
}

VOID
NTAPI
USBSTOR_QueueResetDevice(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension)
{
    KIRQL OldIrql;

    DPRINT("USBSTOR_QueueResetDevice\n");

    KeAcquireSpinLock(&FDODeviceExtension->CommonLock, &OldIrql);
    FDODeviceExtension->Flags |= USBSTOR_FDO_FLAGS_DEVICE_RESETTING;
    KeReleaseSpinLock(&FDODeviceExtension->CommonLock, OldIrql);

    InterlockedIncrement(&UsbStorResetDevQueued);
    USBSTOR_Trace(UsbStorTraceQueueResetDev,
                  (ULONG_PTR)FDODeviceExtension->ActiveSrb,
                  (ULONG_PTR)FDODeviceExtension->Flags);

    IoQueueWorkItem(FDODeviceExtension->ResetDeviceWorkItem,
                    USBSTOR_ResetDeviceWorkItemRoutine,
                    CriticalWorkQueue,
                    NULL);
}

VOID
NTAPI
USBSTOR_TimerWorkerRoutine(
    IN PVOID Context)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    NTSTATUS Status;
    PERRORHANDLER_WORKITEM_DATA WorkItemData = (PERRORHANDLER_WORKITEM_DATA)Context;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)WorkItemData->DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    InterlockedIncrement(&UsbStorAbortRan);
    USBSTOR_Trace(UsbStorTraceTimerAbortRun,
                  (ULONG_PTR)FDODeviceExtension->ActiveSrb,
                  (ULONG_PTR)FDODeviceExtension->Flags);

    /*
     * Abort the bulk pipes and stop there.
     *
     * The only thing this watchdog can do that the driver's own error handling
     * cannot is unstick a transfer that never completes: no completion routine
     * runs for it, so nothing else ever notices.  While it is outstanding the
     * pipe belongs to it and every other request is refused with
     * USBD_STATUS_ERROR_BUSY -- which is why resetting the device from here used
     * to fail at its first step and accomplish nothing.
     *
     * Aborting completes the dead transfer back through its own completion
     * routine with USBD_STATUS_CANCELED.  That reaches the existing reset
     * recovery path, which fails the SRB, resets the device and restarts the
     * queue.  Doing the reset here as well would race that path -- two resets
     * against one device, both driving the shared ResetDeviceWorkItem.
     */
    Status = USBSTOR_AbortPipeWithHandle(FDODeviceExtension->LowerDeviceObject,
                                         FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkInPipeIndex].PipeHandle);
    if (NT_SUCCESS(Status))
    {
        USBSTOR_AbortPipeWithHandle(FDODeviceExtension->LowerDeviceObject,
                                    FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkOutPipeIndex].PipeHandle);
    }

    USBSTOR_Trace(UsbStorTraceTimerAbortEnd,
                  (ULONG_PTR)Status,
                  (ULONG_PTR)FDODeviceExtension->ActiveSrb);

    // clear timer srb
    FDODeviceExtension->LastTimerActiveSrb = NULL;
    FDODeviceExtension->TimerTicksOnActiveSrb = 0;

    ExFreePoolWithTag(WorkItemData, USB_STOR_TAG);
}

VOID
NTAPI
USBSTOR_TimerRoutine(
    PDEVICE_OBJECT DeviceObject,
     PVOID Context)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    BOOLEAN ResetDevice = FALSE;
    BOOLEAN DumpTrace = FALSE;
    PERRORHANDLER_WORKITEM_DATA WorkItemData;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)Context;
    DPRINT("[USBSTOR] TimerRoutine entered\n");

    /*
     * Stand down while the driver's own error recovery owns the device.
     * USBSTOR_QueueResetPipe and USBSTOR_QueueResetDevice both hand work to the
     * single ResetDeviceWorkItem, so a watchdog reset raised on top of an
     * in-flight stall recovery would re-queue a live IO_WORKITEM and issue a
     * second, concurrent reset to the same device.  A stalled transfer does
     * complete its URB, so the stall path recovers on its own; this watchdog is
     * only here for transfers the controller never completes at all.
     */
    if (FDODeviceExtension->Flags & USBSTOR_FDO_FLAGS_DEVICE_RESETTING)
    {
        /*
         * A device reset owns the device, and this routine stands down for as
         * long as that lasts -- so a reset work item that is queued and never
         * runs silences the only watchdog that could notice.  Report it.
         * The tick counter is file-static: this is a diagnostic and there is
         * one bulk storage FDO on the machine being investigated.
         */
        if (++UsbStorResettingTicks == 5)
        {
            USBSTOR_DumpTrace(FDODeviceExtension, "stuck in DEVICE_RESETTING");
        }

        FDODeviceExtension->LastTimerActiveSrb = NULL;
        return;
    }

    UsbStorResettingTicks = 0;
    // DPRINT1("[USBSTOR] ActiveSrb %p ResetInProgress %x LastTimerActiveSrb %p\n", FDODeviceExtension->ActiveSrb, FDODeviceExtension->ResetInProgress, FDODeviceExtension->LastTimerActiveSrb);

    KeAcquireSpinLockAtDpcLevel(&FDODeviceExtension->IrpListLock);

    // is there an active srb and no global reset is in progress
    if (FDODeviceExtension->ActiveSrb && /* FDODeviceExtension->ResetInProgress == FALSE && */ FDODeviceExtension->TimerWorkQueueEnabled)
    {
        if (FDODeviceExtension->LastTimerActiveSrb != NULL && FDODeviceExtension->LastTimerActiveSrb == FDODeviceExtension->ActiveSrb)
        {
            /*
             * Time the request out against the SRB's own TimeOutValue rather
             * than declaring a hang after two ticks.  A legitimately slow
             * command -- a boot-time READ_10 against a cold device -- easily
             * exceeds two seconds, and resetting the device underneath it
             * aborts the command at the device while this driver goes on
             * waiting for its data and status phases.  The bus is then wedged
             * for good: the device wants a new CBW, the driver keeps reading
             * the old transfer, and every retry stalls.
             */
            ULONG Timeout = FDODeviceExtension->ActiveSrb->TimeOutValue;

            if (Timeout == 0)
                Timeout = 10;

            if (++FDODeviceExtension->TimerTicksOnActiveSrb >= Timeout)
            {
                DPRINT1("[USBSTOR] ActiveSrb %p timed out after %lu seconds\n",
                        FDODeviceExtension->ActiveSrb, Timeout);
                ResetDevice = TRUE;
                DumpTrace = TRUE;
            }
        }
        else
        {
            // update pointer
            FDODeviceExtension->LastTimerActiveSrb = FDODeviceExtension->ActiveSrb;
            FDODeviceExtension->TimerTicksOnActiveSrb = 0;
        }
    }
    else
    {
        // reset srb
        FDODeviceExtension->LastTimerActiveSrb = NULL;
        FDODeviceExtension->TimerTicksOnActiveSrb = 0;
    }

    KeReleaseSpinLockFromDpcLevel(&FDODeviceExtension->IrpListLock);

    if (DumpTrace)
    {
        USBSTOR_DumpTrace(FDODeviceExtension, "ActiveSrb timed out");
    }

    if (ResetDevice && FDODeviceExtension->TimerWorkQueueEnabled && FDODeviceExtension->SrbErrorHandlingActive == FALSE)
    {
        WorkItemData = ExAllocatePoolWithTag(NonPagedPool,
                                             sizeof(ERRORHANDLER_WORKITEM_DATA),
                                             USB_STOR_TAG);
        if (WorkItemData)
        {
           // Initialize and queue the work item to handle the error
           ExInitializeWorkItem(&WorkItemData->WorkQueueItem,
                                 USBSTOR_TimerWorkerRoutine,
                                 WorkItemData);

           WorkItemData->DeviceObject = FDODeviceExtension->FunctionalDeviceObject;

           DPRINT1("[USBSTOR] Queing Timer WorkItem\n");
           InterlockedIncrement(&UsbStorAbortQueued);
           USBSTOR_Trace(UsbStorTraceTimerAbortQueue,
                         (ULONG_PTR)FDODeviceExtension->ActiveSrb,
                         (ULONG_PTR)FDODeviceExtension->Flags);
           ExQueueWorkItem(&WorkItemData->WorkQueueItem, DelayedWorkQueue);
        }
     }
}
