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

    BOOLEAN Removed;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT_IRQL_EQUAL(DISPATCH_LEVEL);
    ASSERT(FDODeviceExtension->Common.IsFDO);

    /* IoStartPacket arms this routine, so the IRP is the claimed ActiveSrb and
     * sits in the device queue behind the packet being finished -- or it has
     * just been handed to StartIo, which checks Irp->Cancel itself and
     * completes it there.  It never owns the device queue, so it must not
     * advance it: IoStartNextPacket here released a packet some other request
     * still owned. */
    if (Irp == DeviceObject->CurrentIrp)
    {
        IoReleaseCancelSpinLock(Irp->CancelIrql);
        return;
    }

    Removed = KeRemoveEntryDeviceQueue(&DeviceObject->DeviceQueue,
                                       &Irp->Tail.Overlay.DeviceQueueEntry);
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    NT_ASSERT(Removed);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;

    USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    USBSTOR_QueueStartListHead(DeviceObject);
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
    Irp->IoStatus.Information = 0;

    /* The IRP waited on IrpListHead: it was never started and owns neither
     * ActiveSrb nor the device queue, so there is nothing to advance. */
    USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

BOOLEAN
USBSTOR_QueueAddIrp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    BOOLEAN Wait;
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
     * same order USBSTOR_Cancel uses. */
    IoAcquireCancelSpinLock(&Irp->CancelIrql);
    KeAcquireSpinLockAtDpcLevel(&FDODeviceExtension->IrpListLock);

    /* IoCancelIrp sets Cancel under the cancel lock held here, so an IRP that
     * is not cancelled now can only be cancelled through the routine armed
     * below.  One that already is goes no further: queued, it could be
     * dequeued and started by another processor before a belated cancel
     * routine call here found its routine already taken. */
    if (Irp->Cancel)
    {
        KeReleaseSpinLockFromDpcLevel(&FDODeviceExtension->IrpListLock);
        IoReleaseCancelSpinLock(Irp->CancelIrql);

        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return TRUE;
    }

    /* A request waits on IrpListHead while another one is active, and also
     * while a device reset is in flight: the reset work item restarts the list
     * when it is done (USBSTOR_QueueStartListHead).  Starting it anyway is what
     * the old StartIo reset-park path then had to undo. */
    Wait = FDODeviceExtension->ActiveSrb != NULL ||
           BooleanFlagOn(FDODeviceExtension->Flags,
                         USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE | USBSTOR_FDO_FLAGS_DEVICE_RESETTING);

    if (Wait)
    {
        // add irp to queue, armed before any other CPU can dequeue it
        IoSetCancelRoutine(Irp, USBSTOR_Cancel);
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
         * so test, claim and clear are now all serialised by IrpListLock.
         * No cancel routine yet: the caller's IoStartPacket arms
         * USBSTOR_CancelIo atomically with the device queue insertion, and it
         * and StartIo both handle an IRP that is already cancelled. */
        FDODeviceExtension->ActiveSrb = Request;
    }

    FDODeviceExtension->IrpPendingCount++;
    KeClearEvent(&FDODeviceExtension->NoPendingRequests);

    KeReleaseSpinLockFromDpcLevel(&FDODeviceExtension->IrpListLock);

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    // a waiting request is started later; a claimed one by the caller
    return Wait;
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

    /*
     * Forward-progress tick for the hung-SRB watchdog.  Only a request that
     * actually terminates counts: the park-and-retry path further down puts an
     * IRP back on IrpListHead without finishing it, and counting that would let
     * a reset/retry loop masquerade as progress and silence the watchdog.
     */
    FDODeviceExtension->SrbCompletionCount++;

    // check if this was our current active SRB
    if (FDODeviceExtension->ActiveSrb == Request ||
        (Context->Irp == Irp && FDODeviceExtension->ActiveSrb == Context->Srb))
    {
        // indicate processing is completed
        FDODeviceExtension->ActiveSrb = NULL;
        InterlockedExchange(&FDODeviceExtension->SrbErrorHandlingActive, FALSE);
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
USBSTOR_QueueStartListHead(
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
     * defect USBSTOR_QueueAddIrp had.  IoStartPacket is deliberately left
     * outside: it must not run under a spinlock. */
    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    Busy = FDODeviceExtension->ActiveSrb != NULL ||
           BooleanFlagOn(FDODeviceExtension->Flags,
                         USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE | USBSTOR_FDO_FLAGS_DEVICE_RESETTING);

    while (!Busy && !IsListEmpty(&FDODeviceExtension->IrpListHead))
    {
        Entry = RemoveHeadList(&FDODeviceExtension->IrpListHead);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        /* USBSTOR_Cancel may already own it: it then waits for this lock to
         * unlink the IRP and completes it itself.  Leave it a self-linked
         * entry to unlink and take the next one. */
        if (IoSetCancelRoutine(Irp, NULL) == NULL)
        {
            InitializeListHead(&Irp->Tail.Overlay.ListEntry);
            Irp = NULL;
            continue;
        }

        IoStack = IoGetCurrentIrpStackLocation(Irp);
        Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;
        ASSERT(Request);

        // claim it before anyone else can observe an idle queue
        FDODeviceExtension->ActiveSrb = Request;
        break;
    }

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    if (!Irp)
    {
        USBSTOR_Trace(UsbStorTraceNextReqBusy,
                      (ULONG_PTR)FDODeviceExtension->ActiveSrb,
                      (ULONG_PTR)FDODeviceExtension->Flags);
        return;
    }

    USBSTOR_Trace(UsbStorTraceNextReq, (ULONG_PTR)Irp, (ULONG_PTR)Request);

    /* Starts at once if the device queue is idle; otherwise it is queued
     * behind the packet whose owner is about to call IoStartNextPacket. */
    IoStartPacket(DeviceObject, Irp, &Request->QueueSortKey, USBSTOR_CancelIo);
}

VOID
USBSTOR_QueueNextRequest(
    IN PDEVICE_OBJECT DeviceObject)
{
    /*
     * Called exactly once by the owner of the started packet -- StartIo's own
     * cancel path or a completion routine that has just terminated it -- and
     * so it must release the device queue exactly once, unconditionally.
     *
     * It used to skip IoStartNextPacket whenever ActiveSrb was already set
     * again.  That is the normal outcome of the window between
     * USBSTOR_QueueTerminateRequest and this call: another processor's
     * USBSTOR_QueueAddIrp sees ActiveSrb == NULL, claims it, and its
     * IoStartPacket is inserted into the device queue this packet still holds
     * busy.  Nobody ever released it: the claimed request never reached
     * StartIo, everything else waited behind it on IrpListHead, and the
     * watchdog aborted pipes that had nothing outstanding.  The ASUS X550DP
     * lost its boot disk this way (boot 117: the last CSW completed with
     * USBD status 0, then "next-busy" with the claimed SRB, then silence).
     *
     * Releasing here is right whoever holds ActiveSrb: IoStartNextPacket
     * starts the claimed request if its IoStartPacket was already inserted,
     * or idles the queue so that IoStartPacket starts it directly.
     */
    USBSTOR_QueueStartListHead(DeviceObject);
    IoStartNextPacket(DeviceObject, TRUE);
}

VOID
USBSTOR_QueueRelease(
    IN PDEVICE_OBJECT DeviceObject)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    KIRQL OldLevel;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    // clear freezed status
    FDODeviceExtension->Flags &= ~USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE;

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    // start the oldest waiting request, claiming ActiveSrb for it
    USBSTOR_QueueStartListHead(DeviceObject);
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

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    PDODeviceExtension = (PPDO_DEVICE_EXTENSION)IoStack->DeviceObject->DeviceExtension;
    Request = IoStack->Parameters.Scsi.Srb;
    ASSERT(PDODeviceExtension->Common.IsFDO == FALSE);

    /* Requests wait on IrpListHead while a reset is in flight
     * (USBSTOR_QueueAddIrp, USBSTOR_QueueStartListHead), and the error paths
     * raise DEVICE_RESETTING before they release ActiveSrb, so none can be
     * started into a reset.  The old answer -- parking the IRP back on the list
     * with the device queue still busy -- was the same swallowed-packet hang
     * as USBSTOR_QueueNextRequest's: nothing ever restarted it. */
    NT_ASSERT(!BooleanFlagOn(FDODeviceExtension->Flags, USBSTOR_FDO_FLAGS_DEVICE_RESETTING));

    USBSTOR_Trace(UsbStorTraceStartIo, (ULONG_PTR)Irp, (ULONG_PTR)Request);

    USBSTOR_HandleExecuteSCSI(IoStack->DeviceObject, Irp);

    // FIXME: handle error
}
