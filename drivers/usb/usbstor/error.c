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

    // clear stall on the corresponding pipe
    Status = USBSTOR_ResetPipeWithHandle(FDODeviceExtension->LowerDeviceObject, Context->Urb.UrbBulkOrInterruptTransfer.PipeHandle);
    DPRINT1("USBSTOR_ResetPipeWithHandle Status %x\n", Status);

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

    USBSTOR_QueueNextRequest(FdoDevice);
}

VOID
NTAPI
USBSTOR_QueueResetPipe(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension)
{
    DPRINT("USBSTOR_QueueResetPipe\n");

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
        FDODeviceExtension->LastTimerActiveSrb = NULL;
        return;
    }
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
           ExQueueWorkItem(&WorkItemData->WorkQueueItem, DelayedWorkQueue);
        }
     }
}
