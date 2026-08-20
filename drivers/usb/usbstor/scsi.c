/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Storage Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     USB block storage device driver.
 * COPYRIGHT:   2005-2006 James Tabor
 *              2011-2012 Michael Martin (michael.martin@reactos.org)
 *              2011-2013 Johannes Anderwald (johannes.anderwald@reactos.org)
 *              2017 Vadim Galyant
 *              2019 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include "usbstor.h"

#define NDEBUG
#include <debug.h>


static
NTSTATUS
USBSTOR_SrbStatusToNtStatus(
    IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR SrbStatus;

    SrbStatus = SRB_STATUS(Srb->SrbStatus);

    switch (SrbStatus)
    {
        case SRB_STATUS_SUCCESS:
            return STATUS_SUCCESS;

        case SRB_STATUS_DATA_OVERRUN:
            return STATUS_BUFFER_OVERFLOW;

        case SRB_STATUS_BAD_FUNCTION:
        case SRB_STATUS_BAD_SRB_BLOCK_LENGTH:
            return STATUS_INVALID_DEVICE_REQUEST;

        case SRB_STATUS_INVALID_LUN:
        case SRB_STATUS_INVALID_TARGET_ID:
        case SRB_STATUS_NO_HBA:
        case SRB_STATUS_NO_DEVICE:
            return STATUS_DEVICE_DOES_NOT_EXIST;

        case SRB_STATUS_TIMEOUT:
            return STATUS_IO_TIMEOUT;

        case SRB_STATUS_BUS_RESET:
        case SRB_STATUS_COMMAND_TIMEOUT:
        case SRB_STATUS_SELECTION_TIMEOUT:
            return STATUS_DEVICE_NOT_CONNECTED;

        default:
            return STATUS_IO_DEVICE_ERROR;
    }
}

static
NTSTATUS
USBSTOR_IssueBulkOrInterruptRequest(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PIRP Irp,
    IN USBD_PIPE_HANDLE PipeHandle,
    IN ULONG TransferFlags,
    IN ULONG TransferBufferLength,
    IN PVOID TransferBuffer,
    IN PMDL TransferBufferMDL,
    IN PIO_COMPLETION_ROUTINE CompletionRoutine)
{
    PIO_STACK_LOCATION NextStack;
    PIRP_CONTEXT Context = &FDODeviceExtension->CurrentIrpContext;
    NTSTATUS Status;

    RtlZeroMemory(&Context->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER));

    Context->Urb.UrbHeader.Length = sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
    Context->Urb.UrbHeader.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;

    Context->Urb.UrbBulkOrInterruptTransfer.PipeHandle = PipeHandle;
    Context->Urb.UrbBulkOrInterruptTransfer.TransferFlags = TransferFlags;
    Context->Urb.UrbBulkOrInterruptTransfer.TransferBufferLength = TransferBufferLength;
    Context->Urb.UrbBulkOrInterruptTransfer.TransferBuffer = TransferBuffer;
    Context->Urb.UrbBulkOrInterruptTransfer.TransferBufferMDL = TransferBufferMDL;

    NextStack = IoGetNextIrpStackLocation(Irp);
    NextStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    NextStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    NextStack->Parameters.Others.Argument1 = &Context->Urb;

    IoSetCompletionRoutine(Irp,
                           CompletionRoutine,
                           FDODeviceExtension,
                           TRUE,
                           TRUE,
                           TRUE);

    Status = IoCallDriver(FDODeviceExtension->LowerDeviceObject, Irp);

    USBSTOR_Trace(Status == STATUS_PENDING || NT_SUCCESS(Status)
                      ? UsbStorTraceIssue : UsbStorTraceIssueFail,
                  (ULONG_PTR)Status,
                  (ULONG_PTR)PipeHandle);

    return Status;
}

static
BOOLEAN
USBSTOR_IsCSWValid(
    PIRP_CONTEXT Context)
{
    if (Context->csw.Signature != CSW_SIGNATURE)
    {
        DPRINT1("[USBSTOR] Expected Signature %x but got %x\n", CSW_SIGNATURE, Context->csw.Signature);
        return FALSE;
    }

    if (Context->csw.Tag != PtrToUlong(Context->Irp))
    {
        DPRINT1("[USBSTOR] Expected Tag %Ix but got %x\n", PtrToUlong(Context->Irp), Context->csw.Tag);
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
USBSTOR_IsSenseDataValid(
    IN PSCSI_REQUEST_BLOCK Srb)
{
    if (Srb->DataBuffer == NULL ||
        Srb->DataTransferLength == 0 ||
        Srb->DataTransferLength > 0xFF)
    {
        return FALSE;
    }

    return ScsiGetSenseKeyAndCodes(Srb->DataBuffer,
                                   (UCHAR)Srb->DataTransferLength,
                                   SCSI_SENSE_OPTIONS_NONE,
                                   NULL,
                                   NULL,
                                   NULL);
}

static
NTSTATUS
USBSTOR_IssueRequestSense(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PIRP Irp);

IO_COMPLETION_ROUTINE USBSTOR_CSWCompletionRoutine;

NTSTATUS
NTAPI
USBSTOR_CSWCompletionRoutine(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Ctx)
{
    PIRP_CONTEXT Context;
    PPDO_DEVICE_EXTENSION PDODeviceExtension;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PSCSI_REQUEST_BLOCK Request;

    DPRINT("USBSTOR_CSWCompletionRoutine Irp %p Ctx %p Status %x\n", Irp, Ctx, Irp->IoStatus.Status);

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)Ctx;
    Context = &FDODeviceExtension->CurrentIrpContext;
    Request = Context->Srb;
    PDODeviceExtension = (PPDO_DEVICE_EXTENSION)Context->PDODeviceObject->DeviceExtension;
    ASSERT(Request);

    USBSTOR_Trace(UsbStorTraceCswDone,
                  (ULONG_PTR)Irp->IoStatus.Status,
                  (ULONG_PTR)Context->Urb.UrbHeader.Status);

    // first check for Irp errors
    if (!NT_SUCCESS(Irp->IoStatus.Status))
    {
        if (USBD_STATUS(Context->Urb.UrbHeader.Status) == USBD_STATUS(USBD_STATUS_STALL_PID))
        {
            if (Context->StallRetryCount < 2)
            {
                ++Context->StallRetryCount;

                // clear stall and resend cbw
                USBSTOR_QueueResetPipe(FDODeviceExtension);

                return STATUS_MORE_PROCESSING_REQUIRED;
            }
        }
        else
        {
            DPRINT1("USBSTOR_CSWCompletionRoutine: Urb.Hdr.Status - %x\n", Context->Urb.UrbHeader.Status);
        }

        goto ResetRecovery;
    }

    // now check the CSW packet validity
    if (!USBSTOR_IsCSWValid(Context) || Context->csw.Status == CSW_STATUS_PHASE_ERROR)
    {
        goto ResetRecovery;
    }

    // finally check for CSW errors
    if (Context->csw.Status == CSW_STATUS_COMMAND_PASSED)
    {
        /*
         * dCSWDataResidue is the device's own statement of how much of the data
         * phase it did not transfer (BOT 1.0 section 5.2), and it is the only
         * end-to-end check this driver has on the host controller's byte
         * accounting.  Ignoring it -- which is what happened here -- lets a
         * host controller that over-reports a short IN transfer complete a read
         * as fully successful while the tail of the buffer still holds whatever
         * it held before.  For a demand-paged image that is a page of stale
         * bytes and an access violation in the process that faulted it in.
         *
         * A residue larger than the data phase is meaningless; leave the length
         * alone in that case rather than underflowing it.
         */
        if (Context->csw.DataResidue != 0 &&
            Context->csw.DataResidue <= Context->cbw.DataTransferLength)
        {
            ULONG Transferred = Context->cbw.DataTransferLength -
                                Context->csw.DataResidue;

            if (Transferred < Request->DataTransferLength)
            {
                DPRINT1("[USBSTOR] CSW residue %lu: device sent %lu of %lu bytes\n",
                        Context->csw.DataResidue,
                        Transferred,
                        Request->DataTransferLength);

                Request->DataTransferLength = Transferred;
                Request->SrbStatus = SRB_STATUS_DATA_OVERRUN;
            }
        }

        /*
         * Read ActiveSrb once.  It is written under IrpListLock by
         * USBSTOR_QueueTerminateRequest and by USBSTOR_StartIo's reset-parking
         * path, either of which can run while this completion is in flight, so
         * re-reading it per statement can observe a different value each time.
         *
         * A NULL here means the original request was terminated or parked
         * underneath us.  That is NOT the "a sense request was sent" case the
         * test below is looking for -- it merely also satisfies
         * "Request != ActiveSrb", and the branch then dereferenced the NULL.
         * Observed on the ASUS X550DP as a 0xc0000005 write to
         * ActiveSrb->SenseInfoBufferLength during repeated pipe-reset recovery.
         */
        PSCSI_REQUEST_BLOCK ActiveSrb = FDODeviceExtension->ActiveSrb;

        // should happen only when a sense request was sent
        if (ActiveSrb != NULL && Request != ActiveSrb)
        {
            if (USBSTOR_IsSenseDataValid(Request))
            {
                ActiveSrb->SenseInfoBufferLength = (UCHAR)Request->DataTransferLength;
                ActiveSrb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
            }
            else
            {
                ActiveSrb->SenseInfoBufferLength = 0;
                ActiveSrb->SrbStatus &= ~SRB_STATUS_AUTOSENSE_VALID;
            }

            Request = ActiveSrb;
            Context->Srb = Request;
        }

        Irp->IoStatus.Status = USBSTOR_SrbStatusToNtStatus(Request);
    }
    else if (Context->csw.Status == CSW_STATUS_COMMAND_FAILED)
    {
        // the command is correct but with failed status - issue request sense
        DPRINT("USBSTOR_CSWCompletionRoutine: CSW_STATUS_COMMAND_FAILED\n");

        ASSERT(FDODeviceExtension->ActiveSrb == Request);

        // setting a generic error status, additional information
        // should be read by higher-level driver from SenseInfoBuffer
        Request->SrbStatus = SRB_STATUS_ERROR;
        Request->ScsiStatus = 2;
        Request->DataTransferLength = 0;

        DPRINT("Flags: %x SBL: %x, buf: %p\n", Request->SrbFlags, Request->SenseInfoBufferLength, Request->SenseInfoBuffer);

        if (!(Request->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE) &&
              Request->SenseInfoBufferLength &&
              Request->SenseInfoBuffer)
        {
            USBSTOR_IssueRequestSense(FDODeviceExtension, Irp);
            return STATUS_MORE_PROCESSING_REQUIRED;
        }

        Irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
    }

    Irp->IoStatus.Information = Request->DataTransferLength;

    // terminate current request
    USBSTOR_QueueTerminateRequest(PDODeviceExtension->LowerDeviceObject, Irp);
    USBSTOR_QueueNextRequest(PDODeviceExtension->LowerDeviceObject);

    return STATUS_CONTINUE_COMPLETION;

ResetRecovery:

    /*
     * Same race as above: ActiveSrb can have been cleared underneath us.  Fall
     * back to the SRB this IRP is actually completing rather than storing a
     * NULL into Context->Srb and dereferencing it two lines later.
     */
    if (FDODeviceExtension->ActiveSrb != NULL)
        Request = FDODeviceExtension->ActiveSrb;

    Context->Srb = Request;
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
    Request->SrbStatus = SRB_STATUS_BUS_RESET;

    USBSTOR_QueueTerminateRequest(PDODeviceExtension->LowerDeviceObject, Irp);
    USBSTOR_QueueResetDevice(FDODeviceExtension);

    /*
     * Release the started packet before the IRP is completed up the stack.
     * Leaving that to USBSTOR_ResetDeviceWorkItemRoutine loses the race with
     * the class driver's retry: the retry arrives while the reset is still
     * running, takes ActiveSrb in USBSTOR_QueueAddIrp and is then swallowed by
     * IoStartPacket, because the packet this routine just finished never
     * released the device queue.  The work item's own USBSTOR_QueueNextRequest
     * then sees ActiveSrb set and returns, and the retry never starts at all.
     */
    USBSTOR_QueueNextRequest(PDODeviceExtension->LowerDeviceObject);

    return STATUS_CONTINUE_COMPLETION;
}

NTSTATUS
USBSTOR_SendCSWRequest(
    PFDO_DEVICE_EXTENSION FDODeviceExtension,
    PIRP Irp)
{
    USBSTOR_Trace(UsbStorTraceCswSend,
                  (ULONG_PTR)Irp,
                  (ULONG_PTR)FDODeviceExtension->CurrentIrpContext.StallRetryCount);

    return USBSTOR_IssueBulkOrInterruptRequest(FDODeviceExtension,
                                               Irp,
                                               FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkInPipeIndex].PipeHandle,
                                               USBD_TRANSFER_DIRECTION_IN,
                                               sizeof(CSW),
                                               &FDODeviceExtension->CurrentIrpContext.csw,
                                               NULL,
                                               USBSTOR_CSWCompletionRoutine);
}

IO_COMPLETION_ROUTINE USBSTOR_DataCompletionRoutine;

NTSTATUS
NTAPI
USBSTOR_DataCompletionRoutine(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Ctx)
{
    PIRP_CONTEXT Context;
    PSCSI_REQUEST_BLOCK Request;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PPDO_DEVICE_EXTENSION PDODeviceExtension;

    DPRINT("USBSTOR_DataCompletionRoutine Irp %p Ctx %p Status %x\n", Irp, Ctx, Irp->IoStatus.Status);

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)Ctx;
    Context = &FDODeviceExtension->CurrentIrpContext;
    Request = Context->Srb;
    PDODeviceExtension = (PPDO_DEVICE_EXTENSION)Context->PDODeviceObject->DeviceExtension;

    USBSTOR_Trace(UsbStorTraceDataDone,
                  (ULONG_PTR)Irp->IoStatus.Status,
                  (ULONG_PTR)Context->Urb.UrbHeader.Status);

    // for Sense Request a partial MDL was already freed (if existed)
    if (Request == FDODeviceExtension->ActiveSrb &&
        Context->Urb.UrbBulkOrInterruptTransfer.TransferBufferMDL != Irp->MdlAddress)
    {
        IoFreeMdl(Context->Urb.UrbBulkOrInterruptTransfer.TransferBufferMDL);
    }

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        if (Context->Urb.UrbBulkOrInterruptTransfer.TransferBufferLength < Request->DataTransferLength)
        {
            Request->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        }
        else
        {
            Request->SrbStatus = SRB_STATUS_SUCCESS;
        }

        Request->DataTransferLength = Context->Urb.UrbBulkOrInterruptTransfer.TransferBufferLength;
        USBSTOR_SendCSWRequest(FDODeviceExtension, Irp);
    }
    else if (USBD_STATUS(Context->Urb.UrbHeader.Status) == USBD_STATUS(USBD_STATUS_STALL_PID))
    {
        ++Context->StallRetryCount;

        Request->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        Request->DataTransferLength = Context->Urb.UrbBulkOrInterruptTransfer.TransferBufferLength;

        // clear stall and resend cbw
        USBSTOR_QueueResetPipe(FDODeviceExtension);
    }
    else
    {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
        Request->SrbStatus = SRB_STATUS_BUS_RESET;

        USBSTOR_QueueTerminateRequest(PDODeviceExtension->LowerDeviceObject, Irp);
        USBSTOR_QueueResetDevice(FDODeviceExtension);

        /* Release the started packet, as in USBSTOR_CSWCompletionRoutine. */
        USBSTOR_QueueNextRequest(PDODeviceExtension->LowerDeviceObject);

        return STATUS_CONTINUE_COMPLETION;
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

IO_COMPLETION_ROUTINE USBSTOR_CBWCompletionRoutine;

NTSTATUS
NTAPI
USBSTOR_CBWCompletionRoutine(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Ctx)
{
    PIRP_CONTEXT Context;
    PSCSI_REQUEST_BLOCK Request;
    PPDO_DEVICE_EXTENSION PDODeviceExtension;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    USBD_PIPE_HANDLE PipeHandle;
    ULONG TransferFlags;
    PMDL Mdl = NULL;
    PVOID TransferBuffer = NULL;

    DPRINT("USBSTOR_CBWCompletionRoutine Irp %p Ctx %p Status %x\n", Irp, Ctx, Irp->IoStatus.Status);

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)Ctx;
    Context = &FDODeviceExtension->CurrentIrpContext;
    Request = Context->Srb;
    PDODeviceExtension = (PPDO_DEVICE_EXTENSION)Context->PDODeviceObject->DeviceExtension;

    USBSTOR_Trace(UsbStorTraceCbwDone,
                  (ULONG_PTR)Irp->IoStatus.Status,
                  (ULONG_PTR)Context->Urb.UrbHeader.Status);

    if (!NT_SUCCESS(Irp->IoStatus.Status))
    {
        goto ResetRecovery;
    }

    // a request without the buffer AND not a sense request
    // for a sense request we provide just a TransferBuffer, an Mdl will be allocated by usbport (see below)
    if (!Irp->MdlAddress && Request == FDODeviceExtension->ActiveSrb)
    {
        Request->SrbStatus = SRB_STATUS_SUCCESS;
        USBSTOR_SendCSWRequest(FDODeviceExtension, Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    // a request with the data buffer

    if ((Request->SrbFlags & SRB_FLAGS_UNSPECIFIED_DIRECTION) == SRB_FLAGS_DATA_IN)
    {
        PipeHandle = FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkInPipeIndex].PipeHandle;
        TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    }
    else if ((Request->SrbFlags & SRB_FLAGS_UNSPECIFIED_DIRECTION) == SRB_FLAGS_DATA_OUT)
    {
        PipeHandle = FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkOutPipeIndex].PipeHandle;
        TransferFlags = USBD_TRANSFER_DIRECTION_OUT;
    }
    else
    {
        // we check the validity of a request in disk.c so we should never be here
        DPRINT1("Warning: shouldn't be here\n");
        goto ResetRecovery;
    }

    // if it is not a Sense Request
    if (Request == FDODeviceExtension->ActiveSrb)
    {
        if (MmGetMdlVirtualAddress(Irp->MdlAddress) == Request->DataBuffer)
        {
            Mdl = Irp->MdlAddress;
        }
        else
        {
            Mdl = IoAllocateMdl(Request->DataBuffer,
                                Request->DataTransferLength,
                                FALSE,
                                FALSE,
                                NULL);

            if (Mdl)
            {
                IoBuildPartialMdl(Irp->MdlAddress,
                                  Mdl,
                                  Request->DataBuffer,
                                  Request->DataTransferLength);
            }
        }

        if (!Mdl)
        {
            DPRINT1("USBSTOR_CBWCompletionRoutine: Mdl - %p\n", Mdl);
            goto ResetRecovery;
        }

    }
    else
    {
        ASSERT(Request->DataBuffer);
        TransferBuffer = Request->DataBuffer;
    }

    USBSTOR_Trace(UsbStorTraceDataSend,
                  (ULONG_PTR)Irp,
                  (ULONG_PTR)Request->DataTransferLength);

    USBSTOR_IssueBulkOrInterruptRequest(FDODeviceExtension,
                                        Irp,
                                        PipeHandle,
                                        TransferFlags,
                                        Request->DataTransferLength,
                                        TransferBuffer,
                                        Mdl,
                                        USBSTOR_DataCompletionRoutine);

    return STATUS_MORE_PROCESSING_REQUIRED;

ResetRecovery:
    /* ActiveSrb can be cleared underneath us; see USBSTOR_CSWCompletionRoutine. */
    if (FDODeviceExtension->ActiveSrb != NULL)
        Request = FDODeviceExtension->ActiveSrb;

    Context->Srb = Request;
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
    Request->SrbStatus = SRB_STATUS_BUS_RESET;

    USBSTOR_QueueTerminateRequest(PDODeviceExtension->LowerDeviceObject, Irp);
    USBSTOR_QueueResetDevice(FDODeviceExtension);

    /* Release the started packet, as in USBSTOR_CSWCompletionRoutine. */
    USBSTOR_QueueNextRequest(PDODeviceExtension->LowerDeviceObject);

    return STATUS_CONTINUE_COMPLETION;
}

VOID
DumpCBW(
    PUCHAR Block)
{
    DPRINT("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
        Block[0] & 0xFF, Block[1] & 0xFF, Block[2] & 0xFF, Block[3] & 0xFF, Block[4] & 0xFF, Block[5] & 0xFF, Block[6] & 0xFF, Block[7] & 0xFF, Block[8] & 0xFF, Block[9] & 0xFF,
        Block[10] & 0xFF, Block[11] & 0xFF, Block[12] & 0xFF, Block[13] & 0xFF, Block[14] & 0xFF, Block[15] & 0xFF, Block[16] & 0xFF, Block[17] & 0xFF, Block[18] & 0xFF, Block[19] & 0xFF,
        Block[20] & 0xFF, Block[21] & 0xFF, Block[22] & 0xFF, Block[23] & 0xFF, Block[24] & 0xFF, Block[25] & 0xFF, Block[26] & 0xFF, Block[27] & 0xFF, Block[28] & 0xFF, Block[29] & 0xFF,
        Block[30] & 0xFF);
}

static
NTSTATUS
USBSTOR_SendCBWRequest(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PIRP Irp)
{
    PPDO_DEVICE_EXTENSION PDODeviceExtension;
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK Request;
    PIRP_CONTEXT Context = &FDODeviceExtension->CurrentIrpContext;

    RtlZeroMemory(&Context->cbw, sizeof(CBW));
    RtlZeroMemory(&Context->Urb, sizeof(URB));

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    PDODeviceExtension = IoStack->DeviceObject->DeviceExtension;
    Request = IoStack->Parameters.Scsi.Srb;

    // Make a CBW structure from SCSI request block
    Context->cbw.Signature = CBW_SIGNATURE;
    Context->cbw.Tag = PtrToUlong(Irp);
    Context->cbw.DataTransferLength = Request->DataTransferLength;
    Context->cbw.Flags = ((UCHAR)Request->SrbFlags & SRB_FLAGS_UNSPECIFIED_DIRECTION) << 1;

    // Per Bulk Only Transfer, LUN is 4 bits; clamp just in case
    Context->cbw.LUN = (UCHAR)(PDODeviceExtension->LUN & 0x0F);
    Context->cbw.CommandBlockLength = Request->CdbLength;

    RtlCopyMemory(&Context->cbw.CommandBlock, Request->Cdb, Request->CdbLength);

    DPRINT("CBW for IRP %p\n", Irp);
    DumpCBW((PUCHAR)&Context->cbw);

    // initialize rest of context
    Context->Irp = Irp;
    Context->PDODeviceObject = IoStack->DeviceObject;
    Context->Srb = Request;
    Context->StallRetryCount = 0;

    USBSTOR_Trace(UsbStorTraceCbwSend,
                  (ULONG_PTR)Irp,
                  (ULONG_PTR)Context->cbw.CommandBlock[0]);

    return USBSTOR_IssueBulkOrInterruptRequest(
        FDODeviceExtension,
        Irp,
        FDODeviceExtension->InterfaceInformation->Pipes[FDODeviceExtension->BulkOutPipeIndex].PipeHandle,
        USBD_TRANSFER_DIRECTION_OUT,
        sizeof(CBW),
        &Context->cbw,
        NULL,
        USBSTOR_CBWCompletionRoutine);
}

static
NTSTATUS
USBSTOR_IssueRequestSense(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK CurrentSrb;
    PSCSI_REQUEST_BLOCK SenseSrb;

    DPRINT("USBSTOR_IssueRequestSense: \n");

    /*
     * Use the SRB the caller just validated (SenseInfoBufferLength and
     * SenseInfoBuffer are both checked non-zero there) rather than re-reading
     * ActiveSrb, which is written under IrpListLock by other paths and can go
     * NULL between the caller's check and this dereference.
     */
    CurrentSrb = FDODeviceExtension->CurrentIrpContext.Srb;
    SenseSrb = &FDODeviceExtension->CurrentIrpContext.SenseSrb;
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    IoStack->Parameters.Scsi.Srb = SenseSrb;

    RtlZeroMemory(SenseSrb, sizeof(*SenseSrb));

    SenseSrb->Function = SRB_FUNCTION_EXECUTE_SCSI;
    SenseSrb->Length = sizeof(*SenseSrb);
    SenseSrb->CdbLength = CDB6GENERIC_LENGTH;
    SenseSrb->SrbFlags = SRB_FLAGS_DATA_IN |
                         SRB_FLAGS_NO_QUEUE_FREEZE |
                         SRB_FLAGS_DISABLE_AUTOSENSE;

    ASSERT(CurrentSrb->SenseInfoBufferLength);
    ASSERT(CurrentSrb->SenseInfoBuffer);
    DPRINT("SenseInfoBuffer %x, SenseInfoBufferLength %x\n", CurrentSrb->SenseInfoBuffer, CurrentSrb->SenseInfoBufferLength);

    SenseSrb->DataTransferLength = CurrentSrb->SenseInfoBufferLength;
    SenseSrb->DataBuffer = CurrentSrb->SenseInfoBuffer;

    SrbGetCdb(SenseSrb)->CDB6GENERIC.OperationCode = SCSIOP_REQUEST_SENSE;
    SrbGetCdb(SenseSrb)->AsByte[4] = CurrentSrb->SenseInfoBufferLength;

    USBSTOR_Trace(UsbStorTraceSense, (ULONG_PTR)Irp, (ULONG_PTR)CurrentSrb);

    return USBSTOR_SendCBWRequest(FDODeviceExtension, Irp);
}

NTSTATUS
USBSTOR_HandleExecuteSCSI(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK Request;
    PPDO_DEVICE_EXTENSION PDODeviceExtension = (PPDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    ASSERT(PDODeviceExtension->Common.IsFDO == FALSE);

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Request = IoStack->Parameters.Scsi.Srb;

    DPRINT("USBSTOR_HandleExecuteSCSI Operation Code %x, Length %lu\n", SrbGetCdb(Request)->CDB10.OperationCode, Request->DataTransferLength);

    /*
     * Some stacks/devices don’t propagate the LUN in the CDB10 header bits.
     * USB BOT uses CBW.bCBWLUN for addressing.
     *
     * TODO: There's actually a bigger bug here though,
     * while the above CAN HAPPPEN:
     * Card Readers multiplex their card slots for example, but we can deal with
     * that as we put more effort into fixing the USB stack.
     */
    if (SrbGetCdb(Request)->CDB10.LogicalUnitNumber != PDODeviceExtension->LUN)
    {
        DPRINT1("USBSTOR_HandleExecuteSCSI: CDB LUN %lu != PDO LUN %lu (using CBW LUN)\n",
                (ULONG)SrbGetCdb(Request)->CDB10.LogicalUnitNumber,
                (ULONG)PDODeviceExtension->LUN);
    }

    return USBSTOR_SendCBWRequest(PDODeviceExtension->LowerDeviceObject->DeviceExtension, Irp);
}
