/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for a transfer that completes
 *              inside its scatter/gather list control routine
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#define TRANSFER_SIZE   0x10000
#define ROUNDS          4

typedef struct _SG_COMPLETE_CONTEXT
{
    PDMA_ADAPTER Adapter;
    PUCHAR Buffer;
    ULONG BufferLength;
    ULONG Calls;
} SG_COMPLETE_CONTEXT, *PSG_COMPLETE_CONTEXT;

/*
 * The transfer finishes before the list control routine even returns, as
 * when the device completes it on another processor: the list goes back, the
 * caller's list buffer is the caller's again and gets reused at once.
 */
static
VOID
NTAPI
ListControlComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PSCATTER_GATHER_LIST ScatterGather,
    _In_ PVOID Context)
{
    PSG_COMPLETE_CONTEXT Complete = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    Complete->Calls++;
    Complete->Adapter->DmaOperations->PutScatterGatherList(Complete->Adapter, ScatterGather, TRUE);
    RtlFillMemory(Complete->Buffer, Complete->BufferLength, 0xFF);
}

/*
 * Once the list control routine ran, the list and the buffer it lives in
 * belong to the driver: BuildScatterGatherList must report success without
 * looking at them again. The arm64 HAL read a status back from its context
 * after the routine returned; on MP the disk request had often completed on
 * the other processor by then, the context was freed and reused, and the HAL
 * freed someone else's pool block (0xC2 on a 'DASG' block with a 'MARE'
 * header, corrupted memory areas, rmaps and free lists).
 */
START_TEST(HalScatterGatherReuse)
{
    DEVICE_DESCRIPTION Description;
    PDMA_ADAPTER Adapter;
    PDEVICE_OBJECT DeviceObject;
    ULONG NumberOfMapRegisters = 0, BufferLength = 0, MapRegisters = 0, Round;
    SG_COMPLETE_CONTEXT Complete;
    PUCHAR Buffer, SgBuffer = NULL;
    PMDL Mdl = NULL;
    NTSTATUS Status;
    KIRQL OldIrql;

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No device object\n"))
        return;

    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.ScatterGather = TRUE;
    Description.Dma64BitAddresses = TRUE;
    Description.InterfaceType = PCIBus;
    Description.MaximumLength = TRANSFER_SIZE;
    Adapter = IoGetDmaAdapter(NULL, &Description, &NumberOfMapRegisters);
    ok(Adapter != NULL, "IoGetDmaAdapter failed\n");
    if (skip(Adapter != NULL, "No adapter\n"))
    {
        IoDeleteDevice(DeviceObject);
        return;
    }

    Buffer = ExAllocatePoolWithTag(NonPagedPool, TRANSFER_SIZE, 'RSmK');
    ok(Buffer != NULL, "No buffer\n");
    if (Buffer)
    {
        Mdl = IoAllocateMdl(Buffer, TRANSFER_SIZE, FALSE, FALSE, NULL);
        ok(Mdl != NULL, "No MDL\n");
    }
    if (Mdl)
    {
        MmBuildMdlForNonPagedPool(Mdl);
        Status = Adapter->DmaOperations->CalculateScatterGatherList(Adapter, Mdl, Buffer, TRANSFER_SIZE,
                                                                    &BufferLength, &MapRegisters);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(BufferLength != 0, "No list size\n");
        if (NT_SUCCESS(Status) && BufferLength != 0)
            SgBuffer = ExAllocatePoolWithTag(NonPagedPool, BufferLength, 'RSmK');
    }

    if (!skip(SgBuffer != NULL, "No list buffer\n"))
    {
        for (Round = 0; Round < ROUNDS; Round++)
        {
            RtlZeroMemory(&Complete, sizeof(Complete));
            Complete.Adapter = Adapter;
            Complete.Buffer = SgBuffer;
            Complete.BufferLength = BufferLength;

            KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
            Status = Adapter->DmaOperations->BuildScatterGatherList(Adapter,
                                                                    DeviceObject,
                                                                    Mdl,
                                                                    Buffer,
                                                                    TRANSFER_SIZE,
                                                                    ListControlComplete,
                                                                    &Complete,
                                                                    TRUE,
                                                                    SgBuffer,
                                                                    BufferLength);
            KeLowerIrql(OldIrql);
            ok(Status == STATUS_SUCCESS, "Round %lu: Status 0x%lx\n", Round, Status);
            ok(Complete.Calls == 1, "Round %lu: list control called %lu times\n", Round, Complete.Calls);
        }
        ExFreePoolWithTag(SgBuffer, 'RSmK');
    }

    if (Mdl)
        IoFreeMdl(Mdl);
    if (Buffer)
        ExFreePoolWithTag(Buffer, 'RSmK');
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
    IoDeleteDevice(DeviceObject);
}
