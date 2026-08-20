/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Storage Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     USB block storage device driver.
 * COPYRIGHT:   2005-2006 James Tabor
 *              2011-2012 Michael Martin (michael.martin@reactos.org)
 *              2011-2013 Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "usbstor.h"

#define NDEBUG
#include <debug.h>


VOID
USBSTOR_QueueInitialize(
    PFDO_DEVICE_EXTENSION FDODeviceExtension)
{
    ASSERT(FDODeviceExtension->Common.IsFDO);
    KeInitializeSpinLock(&FDODeviceExtension->IrpListLock);
    InitializeListHead(&FDODeviceExtension->IrpListHead);
    KeInitializeEvent(&FDODeviceExtension->NoPendingRequests, NotificationEvent, TRUE);
}

VOID
NTAPI
USBSTOR_CancelIo(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PIRP Irp)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT_IRQL_EQUAL(DISPATCH_LEVEL);
    ASSERT(FDODeviceExtension->Common.IsFDO);

    // this IRP isn't in our list here
    // now release the cancel lock
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;

    USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    USBSTOR_QueueNextRequest(DeviceObject);
}

VOID
NTAPI
USBSTOR_Cancel(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PIRP Irp)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT_IRQL_EQUAL(DISPATCH_LEVEL);
    ASSERT(FDODeviceExtension->Common.IsFDO);

    KeAcquireSpinLockAtDpcLevel(&FDODeviceExtension->IrpListLock);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLockFromDpcLevel(&FDODeviceExtension->IrpListLock);

    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;

    USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    USBSTOR_QueueNextRequest(DeviceObject);
}

BOOLEAN
USBSTOR_QueueAddIrp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PDRIVER_CANCEL OldDriverCancel;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    BOOLEAN IrpListFreeze;
    BOOLEAN SrbProcessing;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_REQUEST_BLOCK Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    IoMarkIrpPending(Irp);

    /* Arm the cancel routine BEFORE the IRP becomes visible on the list.
     * Queueing first, dropping IrpListLock and only then arming (as this used
     * to do) opened the boot-34/35 window on the ASUS X550DP: the moment the
     * list lock dropped, the previous request's CSW completion on another CPU
     * ran USBSTOR_QueueNextRequest, dequeued this IRP, IoStartPacket'd it and
     * USBSTOR_StartIo cleared the (not yet set) routine -- then the belated
     * IoSetCancelRoutine here armed an IRP already in flight or already
     * completed, and classpnp's IoReuseIrp tripped
     * ASSERT(!Irp->CancelRoutine) (ntoskrnl irp.c:2039) on the next packet
     * submission.  Lock order is cancel lock first, then IrpListLock -- the
     * same order USBSTOR_Cancel and the StartIo reset-park path use. */
    IoAcquireCancelSpinLock(&Irp->CancelIrql);
    KeAcquireSpinLockAtDpcLevel(&FDODeviceExtension->IrpListLock);

    SrbProcessing = FDODeviceExtension->ActiveSrb != NULL;

    if (SrbProcessing)
    {
        // add irp to queue, armed before any other CPU can dequeue it
        OldDriverCancel = IoSetCancelRoutine(Irp, USBSTOR_Cancel);
        InsertTailList(&FDODeviceExtension->IrpListHead, &Irp->Tail.Overlay.ListEntry);
    }
    else
    {
        /* Claim the slot with the very lock that just tested it.  Assigning
         * after the release (as this used to do) let two processors both
         * observe ActiveSrb == NULL, both drop the lock and both become the
         * active request: the loser tripped "Assertion failed:
         * FDODeviceExtension->ActiveSrb == NULL" below, and in a release build
         * silently overwrote ActiveSrb and lost a request.  Seen on the ASUS
         * X550DP under the burst of SRB_FUNCTION_FLUSH at service start.
         * USBSTOR_QueueTerminateRequest clears ActiveSrb under this same lock,
         * so test, claim and clear are now all serialised by IrpListLock. */
        OldDriverCancel = IoSetCancelRoutine(Irp, USBSTOR_CancelIo);
        FDODeviceExtension->ActiveSrb = Request;
    }

    FDODeviceExtension->IrpPendingCount++;
    KeClearEvent(&FDODeviceExtension->NoPendingRequests);

    // check if queue is freezed
    IrpListFreeze = BooleanFlagOn(FDODeviceExtension->Flags, USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE);

    KeReleaseSpinLockFromDpcLevel(&FDODeviceExtension->IrpListLock);

    // check if the irp has already been cancelled
    if (Irp->Cancel && OldDriverCancel == NULL)
    {
        /* Detach the routine before invoking it, exactly as IoCancelIrp does:
         * calling through Irp->CancelRoutine directly left the field set, so
         * the IRP completed back to the class driver still armed and its
         * IoReuseIrp asserted.  Return TRUE ("do not start") -- the old FALSE
         * sent the caller into IoStartPacket with an IRP that the cancel
         * routine has already completed. */
        PDRIVER_CANCEL CancelRoutine = IoSetCancelRoutine(Irp, NULL);
        ASSERT(CancelRoutine != NULL);
        CancelRoutine(DeviceObject, Irp);
        return TRUE;
    }

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    // if list is freezed, dont start this packet
    DPRINT("IrpListFreeze: %lu IrpPendingCount %lu\n", IrpListFreeze, FDODeviceExtension->IrpPendingCount);

    return (IrpListFreeze || SrbProcessing);
}

PIRP
USBSTOR_RemoveIrp(
    IN PDEVICE_OBJECT DeviceObject)
{
    KIRQL OldLevel;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PLIST_ENTRY Entry;
    PIRP Irp = NULL;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    if (!IsListEmpty(&FDODeviceExtension->IrpListHead))
    {
        Entry = RemoveHeadList(&FDODeviceExtension->IrpListHead);

        // get offset to start of irp
        Irp = (PIRP)CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
    }

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    return Irp;
}

VOID
USBSTOR_QueueWaitForPendingRequests(
    IN PDEVICE_OBJECT DeviceObject)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    KeWaitForSingleObject(&FDODeviceExtension->NoPendingRequests,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);
}

VOID
USBSTOR_QueueTerminateRequest(
    IN PDEVICE_OBJECT FDODeviceObject,
    IN PIRP Irp)
{
    KIRQL OldLevel;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_REQUEST_BLOCK Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;
    PIRP_CONTEXT Context;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)FDODeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);
    Context = &FDODeviceExtension->CurrentIrpContext;

    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    FDODeviceExtension->IrpPendingCount--;

    // check if this was our current active SRB
    if (FDODeviceExtension->ActiveSrb == Request ||
        (Context->Irp == Irp && FDODeviceExtension->ActiveSrb == Context->Srb))
    {
        // indicate processing is completed
        FDODeviceExtension->ActiveSrb = NULL;
    }

    // Set the event if nothing else is pending
    if (FDODeviceExtension->IrpPendingCount == 0 &&
        FDODeviceExtension->ActiveSrb == NULL)
    {
        KeSetEvent(&FDODeviceExtension->NoPendingRequests, IO_NO_INCREMENT, FALSE);
    }

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    USBSTOR_Trace(UsbStorTraceTerminate,
                  (ULONG_PTR)Irp,
                  (ULONG_PTR)FDODeviceExtension->ActiveSrb);
}

VOID
USBSTOR_QueueNextRequest(
    IN PDEVICE_OBJECT DeviceObject)
{
    KIRQL OldLevel;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PIRP Irp = NULL;
    PLIST_ENTRY Entry;
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK Request = NULL;
    BOOLEAN Busy;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    /* Test, dequeue and claim are one atomic step.  Doing them unlocked let two
     * processors both see ActiveSrb == NULL and both start a request, the same
     * defect USBSTOR_QueueAddIrp had.  USBSTOR_RemoveIrp is inlined here rather
     * than called because it takes this same lock.  IoStartPacket is deliberately
     * left outside: it must not run at DISPATCH_LEVEL under a spinlock. */
    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    // check first if there's already a request pending or the queue is frozen
    Busy = FDODeviceExtension->ActiveSrb != NULL ||
           BooleanFlagOn(FDODeviceExtension->Flags, USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE);

    if (!Busy && !IsListEmpty(&FDODeviceExtension->IrpListHead))
    {
        Entry = RemoveHeadList(&FDODeviceExtension->IrpListHead);

        // get offset to start of irp
        Irp = (PIRP)CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        IoStack = IoGetCurrentIrpStackLocation(Irp);
        Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;
        ASSERT(Request);

        // claim it before anyone else can observe an idle queue
        FDODeviceExtension->ActiveSrb = Request;
    }

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    if (Busy)
    {
        // no work to do yet
        USBSTOR_Trace(UsbStorTraceNextReqBusy,
                      (ULONG_PTR)FDODeviceExtension->ActiveSrb,
                      (ULONG_PTR)FDODeviceExtension->Flags);
        return;
    }

    USBSTOR_Trace(UsbStorTraceNextReq,
                  (ULONG_PTR)FDODeviceExtension->IrpPendingCount,
                  (ULONG_PTR)FDODeviceExtension->Flags);

    // is there an irp pending
    if (!Irp)
    {
        // no work to do
        IoStartNextPacket(DeviceObject, TRUE);
        return;
    }

    // start next packet
    IoStartPacket(DeviceObject, Irp, &Request->QueueSortKey, USBSTOR_CancelIo);
    IoStartNextPacket(DeviceObject, TRUE);
}

VOID
USBSTOR_QueueRelease(
    IN PDEVICE_OBJECT DeviceObject)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PIRP Irp;
    KIRQL OldLevel;
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK Request;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    // clear freezed status
    FDODeviceExtension->Flags &= ~USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE;

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    // grab newest irp
    Irp = USBSTOR_RemoveIrp(DeviceObject);

    if (!Irp)
    {
        return;
    }

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;

    IoStartPacket(DeviceObject,
                  Irp,
                  &Request->QueueSortKey,
                  USBSTOR_CancelIo);
}

VOID
NTAPI
USBSTOR_StartIo(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK Request;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PPDO_DEVICE_EXTENSION PDODeviceExtension;
    KIRQL OldLevel;
    BOOLEAN ResetInProgress;

    DPRINT("USBSTOR_StartIo\n");

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    IoAcquireCancelSpinLock(&OldLevel);

    IoSetCancelRoutine(Irp, NULL);

    // check if the irp has been cancelled
    if (Irp->Cancel)
    {
        IoReleaseCancelSpinLock(OldLevel);

        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;

        USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        USBSTOR_QueueNextRequest(DeviceObject);
        return;
    }

    IoReleaseCancelSpinLock(OldLevel);

    KeAcquireSpinLock(&FDODeviceExtension->CommonLock, &OldLevel);
    ResetInProgress = BooleanFlagOn(FDODeviceExtension->Flags, USBSTOR_FDO_FLAGS_DEVICE_RESETTING);
    KeReleaseSpinLock(&FDODeviceExtension->CommonLock, OldLevel);

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    PDODeviceExtension = (PPDO_DEVICE_EXTENSION)IoStack->DeviceObject->DeviceExtension;
    Request = IoStack->Parameters.Scsi.Srb;
    ASSERT(PDODeviceExtension->Common.IsFDO == FALSE);

    if (ResetInProgress)
    {
        /*
         * A reset is in flight.  Reporting SRB_STATUS_NO_DEVICE here fails the
         * request for a device that is merely being recovered -- and the
         * retry that follows a reset-recovery completion regularly lands in
         * exactly this window.  Park the request on the driver's own queue
         * instead; the reset work item restarts it through
         * USBSTOR_QueueNextRequest once the device is usable again.
         */
        /* Park and arm in one atomic step, cancel lock first as
         * USBSTOR_Cancel establishes that order.  Arming after the list lock
         * was dropped (as this used to do) raced the reset work item's
         * USBSTOR_QueueNextRequest: it could dequeue and restart the IRP the
         * instant ActiveSrb went NULL, and the belated IoSetCancelRoutine then
         * armed USBSTOR_Cancel on an IRP already back in flight.  The transfer
         * completed to classpnp with the routine still set and IoReuseIrp
         * tripped ASSERT(!Irp->CancelRoutine) on the next packet submission
         * (seen on the ASUS X550DP during xHCI reset storms, boot 34). */
        IoAcquireCancelSpinLock(&OldLevel);
        KeAcquireSpinLockAtDpcLevel(&FDODeviceExtension->IrpListLock);

        if (FDODeviceExtension->ActiveSrb == Request)
            FDODeviceExtension->ActiveSrb = NULL;

        InsertHeadList(&FDODeviceExtension->IrpListHead, &Irp->Tail.Overlay.ListEntry);
        IoSetCancelRoutine(Irp, USBSTOR_Cancel);

        KeReleaseSpinLockFromDpcLevel(&FDODeviceExtension->IrpListLock);
        IoReleaseCancelSpinLock(OldLevel);

        /* Keep the device queue busy: USBSTOR_QueueNextRequest owns it until
         * the driver has no work left, and releases it there. */
        return;
    }

    USBSTOR_Trace(UsbStorTraceStartIo, (ULONG_PTR)Irp, (ULONG_PTR)Request);

    USBSTOR_HandleExecuteSCSI(IoStack->DeviceObject, Irp);

    // FIXME: handle error
}
