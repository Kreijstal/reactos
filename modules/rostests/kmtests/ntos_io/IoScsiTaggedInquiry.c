/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that a burst of tagged INQUIRY SRBs sent to a storage unit all complete
 * COPYRIGHT:   Copyright 2026 ReactOS Project
 *
 * Several SIMPLE-tagged INQUIRY SRBs go to the port driver's unit at once, from
 * DISPATCH_LEVEL, so no DPC can run on this processor until all of them were sent.
 * A miniport that completes some SRBs from a DPC must then complete every SRB it
 * queued for that DPC: requesting a DPC that is still queued does not queue it again.
 * storahci took one SRB per DPC run, so all but one of the burst stayed behind for good.
 * Untagged requests (every pass-through, the class drivers' INQUIRY) do not show it:
 * the port driver issues those one at a time.
 */

#include <kmt_test.h>
#include <ntstrsafe.h>
#include <srb.h>
#include <scsi.h>

#define BURST       8
#define ROUNDS      50
#define DATA_LENGTH 96
#define WAIT_MS     10000
#define TAG         'qTsK'

typedef struct
{
    PIRP Irp;
    SCSI_REQUEST_BLOCK Srb;
    UCHAR Data[DATA_LENGTH];
} BURST_REQUEST, *PBURST_REQUEST;

typedef struct
{
    BURST_REQUEST Request[BURST];
    volatile LONG Pending;
    KEVENT Done;
} BURST_CONTEXT, *PBURST_CONTEXT;

/* Set when requests are stuck in the port driver: their memory and the unit stay referenced */
static BOOLEAN Stuck;

static IO_COMPLETION_ROUTINE BurstCompletion;

static
NTSTATUS
NTAPI
BurstCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PBURST_CONTEXT Burst = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (InterlockedDecrement(&Burst->Pending) == 0)
        KeSetEvent(&Burst->Done, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
VOID
FreeBurst(
    _In_ PBURST_CONTEXT Burst)
{
    ULONG i;

    for (i = 0; i < BURST; i++)
    {
        if (Burst->Request[i].Irp)
        {
            if (Burst->Request[i].Irp->MdlAddress)
                IoFreeMdl(Burst->Request[i].Irp->MdlAddress);
            IoFreeIrp(Burst->Request[i].Irp);
        }
    }
    ExFreePoolWithTag(Burst, TAG);
}

/* Returns FALSE when the burst did not come back */
static
BOOLEAN
SendBurst(
    _In_ PCWSTR Name,
    _In_ PDEVICE_OBJECT Unit,
    _In_ ULONG Round)
{
    PBURST_CONTEXT Burst;
    PBURST_REQUEST Request;
    PIO_STACK_LOCATION Stack;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    KIRQL OldIrql;
    ULONG i;

    Burst = ExAllocatePoolZero(NonPagedPool, sizeof(*Burst), TAG);
    if (skip(Burst != NULL, "%ls: no memory\n", Name))
        return TRUE;

    KeInitializeEvent(&Burst->Done, NotificationEvent, FALSE);
    Burst->Pending = BURST;

    for (i = 0; i < BURST; i++)
    {
        Request = &Burst->Request[i];
        Request->Irp = IoAllocateIrp(Unit->StackSize, FALSE);
        if (skip(Request->Irp != NULL, "%ls: no IRP\n", Name))
        {
            FreeBurst(Burst);
            return TRUE;
        }
        if (skip(IoAllocateMdl(Request->Data, DATA_LENGTH, FALSE, FALSE, Request->Irp) != NULL,
                 "%ls: no MDL\n", Name))
        {
            FreeBurst(Burst);
            return TRUE;
        }
        MmBuildMdlForNonPagedPool(Request->Irp->MdlAddress);

        Request->Srb.Length = sizeof(SCSI_REQUEST_BLOCK);
        Request->Srb.Function = SRB_FUNCTION_EXECUTE_SCSI;
        Request->Srb.SrbFlags = SRB_FLAGS_DATA_IN | SRB_FLAGS_DISABLE_AUTOSENSE |
                                SRB_FLAGS_NO_QUEUE_FREEZE | SRB_FLAGS_QUEUE_ACTION_ENABLE;
        Request->Srb.QueueAction = SRB_SIMPLE_TAG_REQUEST;
        Request->Srb.DataBuffer = Request->Data;
        Request->Srb.DataTransferLength = DATA_LENGTH;
        Request->Srb.TimeOutValue = 10;
        Request->Srb.OriginalRequest = Request->Irp;
        Request->Srb.CdbLength = 6;
        Request->Srb.Cdb[0] = SCSIOP_INQUIRY;
        Request->Srb.Cdb[4] = DATA_LENGTH;

        Stack = IoGetNextIrpStackLocation(Request->Irp);
        Stack->MajorFunction = IRP_MJ_SCSI;
        Stack->MinorFunction = IRP_MN_SCSI_CLASS;
        Stack->Parameters.Scsi.Srb = &Request->Srb;
        IoSetCompletionRoutine(Request->Irp, BurstCompletion, Burst, TRUE, TRUE, TRUE);
    }

    /* No DPC runs on this processor until the whole burst is out */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    for (i = 0; i < BURST; i++)
        (VOID)IoCallDriver(Unit, Burst->Request[i].Irp);
    KeLowerIrql(OldIrql);

    Timeout.QuadPart = -10000LL * WAIT_MS;
    Status = KeWaitForSingleObject(&Burst->Done, Executive, KernelMode, FALSE, &Timeout);
    if (Status != STATUS_SUCCESS)
    {
        ok(FALSE, "%ls: round %lu: %ld of %d tagged INQUIRYs did not come back within %u s\n",
           Name, Round, Burst->Pending, BURST, WAIT_MS / 1000);
        /* The IRPs may still complete into Burst: leave it */
        return FALSE;
    }

    for (i = 0; i < BURST; i++)
    {
        Request = &Burst->Request[i];
        /* A short INQUIRY answer is an underrun: SRB_STATUS_DATA_OVERRUN, STATUS_BUFFER_OVERFLOW */
        if (SRB_STATUS(Request->Srb.SrbStatus) == SRB_STATUS_DATA_OVERRUN)
        {
            ok(Request->Irp->IoStatus.Status == STATUS_BUFFER_OVERFLOW || NT_SUCCESS(Request->Irp->IoStatus.Status),
               "%ls: round %lu: INQUIRY %lu underrun failed with 0x%lx\n",
               Name, Round, i, Request->Irp->IoStatus.Status);
            ok(Request->Srb.DataTransferLength >= 36 && Request->Srb.DataTransferLength <= DATA_LENGTH,
               "%ls: round %lu: INQUIRY %lu returned %lu bytes\n",
               Name, Round, i, Request->Srb.DataTransferLength);
        }
        else
        {
            ok(SRB_STATUS(Request->Srb.SrbStatus) == SRB_STATUS_SUCCESS,
               "%ls: round %lu: INQUIRY %lu SRB status 0x%x\n", Name, Round, i, Request->Srb.SrbStatus);
            ok(NT_SUCCESS(Request->Irp->IoStatus.Status),
               "%ls: round %lu: INQUIRY %lu failed with 0x%lx, SRB status 0x%x\n",
               Name, Round, i, Request->Irp->IoStatus.Status, Request->Srb.SrbStatus);
        }
    }

    FreeBurst(Burst);
    return TRUE;
}

static
BOOLEAN
TestUnit(
    _In_ PCWSTR Name)
{
    UNICODE_STRING DeviceName;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject, Unit;
    NTSTATUS Status;
    ULONG Round;

    RtlInitUnicodeString(&DeviceName, Name);
    Status = IoGetDeviceObjectPointer(&DeviceName, FILE_READ_ATTRIBUTES, &FileObject, &DeviceObject);
    if (!NT_SUCCESS(Status))
        return FALSE;

    /* The bottom of the stack is the port driver's unit (PDO) */
    Unit = IoGetDeviceAttachmentBaseRef(DeviceObject);
    trace("%ls: unit %p of %wZ\n", Name, Unit, &Unit->DriverObject->DriverName);

    for (Round = 0; Round < ROUNDS; Round++)
    {
        if (!SendBurst(Name, Unit, Round))
        {
            /* The stuck requests keep the unit and the file referenced */
            Stuck = TRUE;
            return TRUE;
        }
    }

    ObDereferenceObject(Unit);
    ObDereferenceObject(FileObject);
    return TRUE;
}

START_TEST(IoScsiTaggedInquiry)
{
    static const PCWSTR Formats[] = { L"\\Device\\Harddisk%lu\\Partition0", L"\\Device\\CdRom%lu" };
    WCHAR Name[64];
    ULONG Format, Number, Found = 0;

    for (Format = 0; Format < RTL_NUMBER_OF(Formats); Format++)
    {
        for (Number = 0; Number < 16 && !Stuck; Number++)
        {
            RtlStringCbPrintfW(Name, sizeof(Name), Formats[Format], Number);
            if (TestUnit(Name))
                Found++;
        }
    }

    if (!Found)
        skip(FALSE, "No storage unit found\n");
}
