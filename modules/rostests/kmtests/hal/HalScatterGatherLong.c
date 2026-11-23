/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for scatter/gather lists of
 *              transfers whose pages are not physically contiguous
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

/* More pages than the 48 elements the HAL used to allow for */
#define TRANSFER_PAGES  96
#define START_OFFSET    0x200

typedef struct _SG_LONG_CONTEXT
{
    PSCATTER_GATHER_LIST List;
    ULONG Calls;
} SG_LONG_CONTEXT, *PSG_LONG_CONTEXT;

static
VOID
NTAPI
ListControlLong(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PSCATTER_GATHER_LIST ScatterGather,
    _In_ PVOID Context)
{
    PSG_LONG_CONTEXT SgContext = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    SgContext->List = ScatterGather;
    SgContext->Calls++;
}

/* Sort a PFN array in descending order (no two neighbours contiguous) */
static
VOID
SortPfnsDescending(
    _Inout_updates_(Count) PPFN_NUMBER Pfns,
    _In_ ULONG Count)
{
    ULONG i, j;

    for (i = 1; i < Count; i++)
    {
        PFN_NUMBER Pfn = Pfns[i];

        for (j = i; j > 0 && Pfns[j - 1] < Pfn; j--)
            Pfns[j] = Pfns[j - 1];
        Pfns[j] = Pfn;
    }
}

/* Check that List describes Length bytes of Mdl from Offset on, page by page */
static
VOID
CheckList(
    _In_ PCSTR Name,
    _In_ PSCATTER_GATHER_LIST List,
    _In_ PMDL Mdl,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    PPFN_NUMBER Pfns = MmGetMdlPfnArray(Mdl);
    ULONG Element, Done = 0, Bad = 0;

    /* Every page is its own run, so the list has one element per page */
    ok(List->NumberOfElements == ADDRESS_AND_SIZE_TO_SPAN_PAGES(Offset, Length),
       "%s: %lu elements, expected %lu\n", Name, List->NumberOfElements,
       (ULONG)ADDRESS_AND_SIZE_TO_SPAN_PAGES(Offset, Length));

    for (Element = 0; Element < List->NumberOfElements && Done < Length; Element++)
    {
        ULONG Position = Offset + Done;
        ULONGLONG Expected = ((ULONGLONG)Pfns[Position >> PAGE_SHIFT] << PAGE_SHIFT) + BYTE_OFFSET(Position);
        ULONG ExpectedLength = min(PAGE_SIZE - BYTE_OFFSET(Position), Length - Done);

        if ((ULONGLONG)List->Elements[Element].Address.QuadPart != Expected ||
            List->Elements[Element].Length != ExpectedLength)
        {
            if (Bad++ < 3)
            {
                trace("%s: element %lu is 0x%I64x/0x%lx, expected 0x%I64x/0x%lx\n", Name, Element,
                      List->Elements[Element].Address.QuadPart, List->Elements[Element].Length,
                      Expected, ExpectedLength);
            }
        }
        Done += List->Elements[Element].Length;
    }
    ok(Bad == 0, "%s: %lu elements do not describe the buffer's pages\n", Name, Bad);
    ok(Done == Length, "%s: the list covers 0x%lx of 0x%lx bytes\n", Name, Done, Length);
}

/*
 * A bus master that reaches all of memory gets no map registers: the list
 * describes the buffer's own pages, one element per physically contiguous
 * run. A transfer of a fragmented buffer therefore needs one element per page
 * it spans, and the HAL must build such a list for every transfer the adapter
 * accepts (MaximumLength). The HAL had room for 48 elements only; for a
 * longer list it returned from its adapter control routine without calling
 * the driver's routine, after GetScatterGatherList had already returned
 * STATUS_SUCCESS, so the request was lost and its driver waited for it
 * forever (USB stick I/O through usbxhci timing out in usbstor).
 */
START_TEST(HalScatterGatherLong)
{
    DEVICE_DESCRIPTION Description;
    PDMA_ADAPTER Adapter;
    PDEVICE_OBJECT DeviceObject;
    ULONG NumberOfMapRegisters = 0, ListSize = 0, ListMapRegisters = 0, Page;
    PHYSICAL_ADDRESS Low, High, Skip;
    SG_LONG_CONTEXT SgContext;
    PUCHAR Va, SgBuffer = NULL;
    PMDL Mdl;
    NTSTATUS Status;
    KIRQL OldIrql;
    const ULONG Length = TRANSFER_PAGES * PAGE_SIZE - START_OFFSET;

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (skip(NT_SUCCESS(Status), "No device object\n"))
        return;

    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.ScatterGather = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.Dma64BitAddresses = TRUE;
    Description.InterfaceType = PCIBus;
    Description.MaximumLength = TRANSFER_PAGES * PAGE_SIZE;
    Adapter = IoGetDmaAdapter(NULL, &Description, &NumberOfMapRegisters);
    ok(Adapter != NULL, "IoGetDmaAdapter failed\n");
    if (skip(Adapter != NULL, "No adapter\n"))
    {
        IoDeleteDevice(DeviceObject);
        return;
    }
    /* BYTES_TO_PAGES(MaximumLength) + 1 */
    ok_eq_ulong(NumberOfMapRegisters, TRANSFER_PAGES + 1UL);

    /* Pages in descending physical order: no two neighbours are contiguous */
    Low.QuadPart = 0;
    High.QuadPart = -1;
    Skip.QuadPart = 0;
    Mdl = MmAllocatePagesForMdl(Low, High, Skip, TRANSFER_PAGES * PAGE_SIZE);
    ok(Mdl != NULL, "MmAllocatePagesForMdl failed\n");
    if (Mdl && MmGetMdlByteCount(Mdl) != TRANSFER_PAGES * PAGE_SIZE)
    {
        skip(FALSE, "Got only 0x%lx bytes\n", MmGetMdlByteCount(Mdl));
        MmFreePagesFromMdl(Mdl);
        ExFreePool(Mdl);
        Mdl = NULL;
    }
    if (!skip(Mdl != NULL, "No pages\n"))
    {
        PPFN_NUMBER Pfns = MmGetMdlPfnArray(Mdl);

        SortPfnsDescending(Pfns, TRANSFER_PAGES);
        for (Page = 0; Page + 1 < TRANSFER_PAGES; Page++)
            ok(Pfns[Page] != Pfns[Page + 1] + 1 && Pfns[Page] + 1 != Pfns[Page + 1],
               "Pages %lu and %lu are contiguous\n", Page, Page + 1);
        Va = (PUCHAR)MmGetMdlVirtualAddress(Mdl) + START_OFFSET;

        /* The list buffer must hold one element per page */
        Status = Adapter->DmaOperations->CalculateScatterGatherList(Adapter, Mdl, Va, Length,
                                                                    &ListSize, &ListMapRegisters);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(ListMapRegisters, (ULONG)TRANSFER_PAGES);
        ok(ListSize >= FIELD_OFFSET(SCATTER_GATHER_LIST, Elements[TRANSFER_PAGES]),
           "List size %lu is too small for %u elements\n", ListSize, TRANSFER_PAGES);

        /* GetScatterGatherList: the list is built and handed over at once */
        RtlZeroMemory(&SgContext, sizeof(SgContext));
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Status = Adapter->DmaOperations->GetScatterGatherList(Adapter, DeviceObject, Mdl, Va, Length,
                                                              ListControlLong, &SgContext, FALSE);
        KeLowerIrql(OldIrql);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(SgContext.Calls == 1, "GetScatterGatherList: list control called %lu times\n", SgContext.Calls);
        if (SgContext.List)
        {
            CheckList("GetScatterGatherList", SgContext.List, Mdl, START_OFFSET, Length);
            KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
            Adapter->DmaOperations->PutScatterGatherList(Adapter, SgContext.List, FALSE);
            KeLowerIrql(OldIrql);
        }

        /* BuildScatterGatherList builds the list in the caller's buffer */
        if (ListSize)
            SgBuffer = ExAllocatePoolWithTag(NonPagedPool, ListSize, 'LSmK');
        if (!skip(SgBuffer != NULL, "No list buffer\n"))
        {
            RtlZeroMemory(&SgContext, sizeof(SgContext));
            KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
            Status = Adapter->DmaOperations->BuildScatterGatherList(Adapter, DeviceObject, Mdl, Va, Length,
                                                                    ListControlLong, &SgContext, TRUE,
                                                                    SgBuffer, ListSize);
            KeLowerIrql(OldIrql);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(SgContext.Calls == 1, "BuildScatterGatherList: list control called %lu times\n", SgContext.Calls);
            if (SgContext.List)
            {
                ok((PUCHAR)SgContext.List >= SgBuffer &&
                   (PUCHAR)&SgContext.List->Elements[SgContext.List->NumberOfElements] <= SgBuffer + ListSize,
                   "List %p is not in the caller's buffer %p (%lu bytes)\n", SgContext.List, SgBuffer, ListSize);
                CheckList("BuildScatterGatherList", SgContext.List, Mdl, START_OFFSET, Length);
                KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
                Adapter->DmaOperations->PutScatterGatherList(Adapter, SgContext.List, TRUE);
                KeLowerIrql(OldIrql);
            }

            /* A buffer too small for the list is refused, before anything runs */
            RtlZeroMemory(&SgContext, sizeof(SgContext));
            KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
            Status = Adapter->DmaOperations->BuildScatterGatherList(Adapter, DeviceObject, Mdl, Va, Length,
                                                                    ListControlLong, &SgContext, TRUE,
                                                                    SgBuffer,
                                                                    FIELD_OFFSET(SCATTER_GATHER_LIST, Elements[TRANSFER_PAGES / 2]));
            KeLowerIrql(OldIrql);
            ok_eq_hex(Status, STATUS_BUFFER_TOO_SMALL);
            ok(SgContext.Calls == 0, "Too small a buffer: list control called %lu times\n", SgContext.Calls);
            ExFreePoolWithTag(SgBuffer, 'LSmK');
        }

        /*
         * A transfer spanning more pages than the adapter allows
         * (NumberOfMapRegisters) is refused with an error, and its routine
         * is never called.
         */
        {
            PDMA_ADAPTER SmallAdapter;
            ULONG SmallMapRegisters = 0;

            Description.MaximumLength = 16 * PAGE_SIZE;
            SmallAdapter = IoGetDmaAdapter(NULL, &Description, &SmallMapRegisters);
            ok(SmallAdapter != NULL, "IoGetDmaAdapter failed\n");
            if (!skip(SmallAdapter != NULL, "No adapter\n"))
            {
                ok_eq_ulong(SmallMapRegisters, 17UL);
                RtlZeroMemory(&SgContext, sizeof(SgContext));
                KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
                Status = SmallAdapter->DmaOperations->GetScatterGatherList(SmallAdapter, DeviceObject, Mdl, Va, Length,
                                                                           ListControlLong, &SgContext, FALSE);
                KeLowerIrql(OldIrql);
                ok_eq_hex(Status, STATUS_INSUFFICIENT_RESOURCES);
                ok(SgContext.Calls == 0, "Too long a transfer: list control called %lu times\n", SgContext.Calls);
                if (SgContext.List)
                {
                    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
                    SmallAdapter->DmaOperations->PutScatterGatherList(SmallAdapter, SgContext.List, FALSE);
                    KeLowerIrql(OldIrql);
                }
                SmallAdapter->DmaOperations->PutDmaAdapter(SmallAdapter);
            }
        }

        /* The whole MDL, page aligned */
        RtlZeroMemory(&SgContext, sizeof(SgContext));
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Status = Adapter->DmaOperations->GetScatterGatherList(Adapter, DeviceObject, Mdl,
                                                              MmGetMdlVirtualAddress(Mdl),
                                                              TRANSFER_PAGES * PAGE_SIZE,
                                                              ListControlLong, &SgContext, FALSE);
        KeLowerIrql(OldIrql);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(SgContext.Calls == 1, "Whole MDL: list control called %lu times\n", SgContext.Calls);
        if (SgContext.List)
        {
            CheckList("Whole MDL", SgContext.List, Mdl, 0, TRANSFER_PAGES * PAGE_SIZE);
            KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
            Adapter->DmaOperations->PutScatterGatherList(Adapter, SgContext.List, FALSE);
            KeLowerIrql(OldIrql);
        }

        MmFreePagesFromMdl(Mdl);
        ExFreePool(Mdl);
    }

    Adapter->DmaOperations->PutDmaAdapter(Adapter);
    IoDeleteDevice(DeviceObject);
}
