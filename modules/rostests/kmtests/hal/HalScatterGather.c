/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite DMA scatter/gather test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#define CHUNK_SIZE  0x10000
#define CHUNKS      16

typedef struct _SG_CONTEXT
{
    PSCATTER_GATHER_LIST List;
    ULONG Calls;
} SG_CONTEXT, *PSG_CONTEXT;

static
VOID
NTAPI
ListControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PSCATTER_GATHER_LIST ScatterGather,
    _In_ PVOID Context)
{
    PSG_CONTEXT SgContext = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    SgContext->List = ScatterGather;
    SgContext->Calls++;
}

/*
 * A 32-bit PCI bus master that does scatter/gather can reach every page of
 * memory when no memory lies above 4 GB. Windows gives such an adapter no
 * map registers then (HalGetAdapterV2 checks HalpMaximumPhysicalMemoryAddress),
 * so the device gets the buffer's own pages and any number of transfers can
 * be in flight. Map registers are a small shared pool: bouncing through them
 * makes every in-flight transfer of every such device (a NIC's whole transmit
 * ring, say) hold some, and once the pool is used up the next disk request
 * waits for a growth that cannot happen.
 *
 * With memory above 4 GB the device does need map registers, and then the
 * pool has to grow on demand for as many transfers as are in flight
 * (Windows allocates map buffer pages as needed). A fixed 64-register pool
 * let a NIC's transmit ring, which holds its registers until the packets are
 * sent, starve a 16-register disk request indefinitely.
 */
START_TEST(HalScatterGather)
{
    PPHYSICAL_MEMORY_RANGE Ranges, Range;
    ULONGLONG HighestAddress = 0;
    DEVICE_DESCRIPTION Description;
    PDMA_ADAPTER Adapter;
    PDEVICE_OBJECT DeviceObject;
    ULONG NumberOfMapRegisters = 0;
    PUCHAR Buffer;
    PMDL Mdl;
    SG_CONTEXT Contexts[CHUNKS];
    ULONG Chunk, Element, Delivered, Bounced = 0;
    BOOLEAN HighMemory;
    NTSTATUS Status;
    KIRQL OldIrql;

    Ranges = MmGetPhysicalMemoryRanges();
    ok(Ranges != NULL, "MmGetPhysicalMemoryRanges failed\n");
    if (skip(Ranges != NULL, "No memory ranges\n"))
        return;
    for (Range = Ranges; Range->BaseAddress.QuadPart || Range->NumberOfBytes.QuadPart; Range++)
    {
        HighestAddress = max(HighestAddress,
                             (ULONGLONG)(Range->BaseAddress.QuadPart + Range->NumberOfBytes.QuadPart - 1));
    }
    ExFreePool(Ranges);
    trace("Highest physical memory address 0x%I64x\n", HighestAddress);
    HighMemory = HighestAddress > 0xFFFFFFFFULL;

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No device object\n"))
        return;

    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.ScatterGather = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.InterfaceType = PCIBus;
    Description.MaximumLength = CHUNK_SIZE;
    Adapter = IoGetDmaAdapter(NULL, &Description, &NumberOfMapRegisters);
    ok(Adapter != NULL, "IoGetDmaAdapter failed\n");
    if (skip(Adapter != NULL, "No adapter\n"))
    {
        IoDeleteDevice(DeviceObject);
        return;
    }
    trace("NumberOfMapRegisters %lu\n", NumberOfMapRegisters);
    /* Windows reports BYTES_TO_PAGES(MaximumLength) + 1 either way; whether
     * the transfers are actually bounced is what the checks below test */
    ok(NumberOfMapRegisters != 0, "No map registers reported\n");

    Buffer = ExAllocatePoolWithTag(NonPagedPool, CHUNK_SIZE * CHUNKS, 'GSmK');
    ok(Buffer != NULL, "No buffer\n");
    Mdl = Buffer ? IoAllocateMdl(Buffer, CHUNK_SIZE * CHUNKS, FALSE, FALSE, NULL) : NULL;
    ok(Mdl != NULL, "No MDL\n");
    if (!skip(Buffer && Mdl, "No buffer\n"))
    {
        MmBuildMdlForNonPagedPool(Mdl);
        RtlZeroMemory(Contexts, sizeof(Contexts));

        /* Start every transfer before completing any of them, like a NIC
         * filling its transmit ring */
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        for (Chunk = 0; Chunk < CHUNKS; Chunk++)
        {
            Status = Adapter->DmaOperations->GetScatterGatherList(Adapter,
                                                                  DeviceObject,
                                                                  Mdl,
                                                                  Buffer + Chunk * CHUNK_SIZE,
                                                                  CHUNK_SIZE,
                                                                  ListControl,
                                                                  &Contexts[Chunk],
                                                                  TRUE);
            ok(Status == STATUS_SUCCESS, "Chunk %lu: Status 0x%lx\n", Chunk, Status);
        }

        if (HighMemory)
        {
            LARGE_INTEGER Delay;
            ULONG Wait;

            /* Hold every transfer: none of them gives its map registers back,
             * so only growing the pool can start the ones still waiting */
            KeLowerIrql(OldIrql);
            Delay.QuadPart = -100 * 10000;
            for (Wait = 0; Wait < 100; Wait++)
            {
                Delivered = 0;
                for (Chunk = 0; Chunk < CHUNKS; Chunk++)
                    Delivered += Contexts[Chunk].Calls;
                if (Delivered == CHUNKS)
                    break;
                KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            }
            ok(Delivered == CHUNKS, "Only %lu of %u held transfers got map registers\n", Delivered, CHUNKS);
            KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        }

        Delivered = 0;
        for (Chunk = 0; Chunk < CHUNKS; Chunk++)
        {
            PSCATTER_GATHER_LIST List = Contexts[Chunk].List;
            PUCHAR Va = Buffer + Chunk * CHUNK_SIZE;
            ULONG Offset = 0;

            if (!List)
                continue;
            Delivered++;

            /* The device must be handed the buffer's own pages */
            for (Element = 0; Element < List->NumberOfElements; Element++)
            {
                ULONG Done;

                for (Done = 0; Done < List->Elements[Element].Length; Done += PAGE_SIZE)
                {
                    PHYSICAL_ADDRESS Expected = MmGetPhysicalAddress(Va + Offset + Done);

                    if (List->Elements[Element].Address.QuadPart + Done != (ULONGLONG)Expected.QuadPart)
                    {
                        if (Bounced++ < 3)
                            trace("Chunk %lu element %lu: 0x%I64x, buffer at 0x%I64x\n", Chunk, Element,
                                  List->Elements[Element].Address.QuadPart + Done, Expected.QuadPart);
                    }
                }
                Offset += List->Elements[Element].Length;
            }
            ok(Offset == CHUNK_SIZE, "Chunk %lu: mapped %lu bytes\n", Chunk, Offset);
        }
        if (!HighMemory)
        {
            ok(Delivered == CHUNKS, "Only %lu of %u transfers could start at once\n", Delivered, CHUNKS);
            ok(Bounced == 0, "%lu pages were double-buffered\n", Bounced);
        }

        /* Complete them; transfers that had to wait start as others finish */
        for (Chunk = 0; Chunk < CHUNKS; Chunk++)
        {
            if (Contexts[Chunk].List)
            {
                Adapter->DmaOperations->PutScatterGatherList(Adapter, Contexts[Chunk].List, TRUE);
                Contexts[Chunk].List = NULL;
            }
        }
        KeLowerIrql(OldIrql);

        /* Any transfer still waiting needs a map buffer growth at passive level */
        {
            LARGE_INTEGER Delay;
            ULONG Wait;

            Delay.QuadPart = -100 * 10000;
            for (Wait = 0; Wait < 50; Wait++)
            {
                BOOLEAN Pending = FALSE;

                KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
                for (Chunk = 0; Chunk < CHUNKS; Chunk++)
                {
                    if (Contexts[Chunk].List)
                    {
                        Adapter->DmaOperations->PutScatterGatherList(Adapter, Contexts[Chunk].List, TRUE);
                        Contexts[Chunk].List = NULL;
                    }
                    if (Contexts[Chunk].Calls == 0)
                        Pending = TRUE;
                }
                KeLowerIrql(OldIrql);
                if (!Pending)
                    break;
                KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            }
        }

        for (Chunk = 0; Chunk < CHUNKS; Chunk++)
            ok(Contexts[Chunk].Calls == 1, "Chunk %lu: list control called %lu times\n", Chunk, Contexts[Chunk].Calls);
    }

    if (Mdl)
        IoFreeMdl(Mdl);
    if (Buffer)
        ExFreePoolWithTag(Buffer, 'GSmK');
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
    IoDeleteDevice(DeviceObject);
}
