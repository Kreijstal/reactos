/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite Io Regressions KM-Test (Irp)
 * PROGRAMMER:      Aleksey Bragin <aleksey@reactos.org>
 */
/* Based on code Copyright 2008 Etersoft (Alexander Morozov) */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

START_TEST(IoIrp)
{
    USHORT size;
    IRP *iorp;
    KIRQL Irql;
    LONG Float;

    // 1st test
    size = sizeof(IRP) + 5 * sizeof(IO_STACK_LOCATION);
    iorp = ExAllocatePool(NonPagedPool, size);

    if (NULL != iorp)
    {
        IoInitializeIrp(iorp, size, 5);

        ok(6 == iorp->Type, "Irp type should be 6, but got %d\n", iorp->Type);
        ok(iorp->Size == size, "Irp size should be %d, but got %d\n",
            iorp->Size, size);
        ok(5 == iorp->StackCount, "Irp StackCount should be 5, but got %d\n",
            iorp->StackCount);
        ok(6 == iorp->CurrentLocation, "Irp CurrentLocation should be 6, but got %d\n",
            iorp->CurrentLocation);
        ok(IsListEmpty(&iorp->ThreadListEntry), "IRP thread list is not empty\n");
        ok ((PIO_STACK_LOCATION)(iorp + 1) + 5 ==
            iorp->Tail.Overlay.CurrentStackLocation,
            "CurrentStackLocation mismatch\n");

        ExFreePool(iorp);
    }

    // 2nd test
    size = sizeof(IRP) + 2 * sizeof(IO_STACK_LOCATION);
    iorp = IoAllocateIrp(2, FALSE);

    if (NULL != iorp)
    {
        ok(6 == iorp->Type, "Irp type should be 6, but got %d\n", iorp->Type);
        ok(iorp->Size >= size,
            "Irp size should be more or equal to %d, but got %d\n",
            iorp->Size, size);
        ok(2 == iorp->StackCount, "Irp StackCount should be 2, but got %d\n",
            iorp->StackCount);
        ok(3 == iorp->CurrentLocation, "Irp CurrentLocation should be 3, but got %d\n",
            iorp->CurrentLocation);
        ok(IsListEmpty(&iorp->ThreadListEntry), "IRP thread list is not empty\n");
        ok ((PIO_STACK_LOCATION)(iorp + 1) + 2 ==
            iorp->Tail.Overlay.CurrentStackLocation,
            "CurrentStackLocation mismatch\n");
        ok((IRP_ALLOCATED_FIXED_SIZE & iorp->AllocationFlags),
            "IRP Allocation flags lack fixed size attribute\n");
        ok(!(IRP_LOOKASIDE_ALLOCATION & iorp->AllocationFlags),
            "IRP Allocation flags should not have lookaside allocation\n");

        IoFreeIrp(iorp);
    }

    // 3rd test
    /* A quota-charged IRP comes from the lookaside lists only while this
     * processor has IRP credit left. The credit is per processor: an IRP
     * allocated on one and freed on another moves it there, so once IRPs run
     * on every processor, this one can have none. Stay on it to look. */
    size = sizeof(IRP) + 2 * sizeof(IO_STACK_LOCATION);
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Float = KeGetCurrentPrcb()->LookasideIrpFloat;
    iorp = IoAllocateIrp(2, TRUE);
    KeLowerIrql(Irql);

    if (NULL != iorp)
    {
        ok(6 == iorp->Type, "Irp type should be 6, but got %d\n", iorp->Type);
        ok(iorp->Size >= size,
            "Irp size should be more or equal to %d, but got %d\n",
            iorp->Size, size);
        ok(2 == iorp->StackCount, "Irp StackCount should be 2, but got %d\n",
            iorp->StackCount);
        ok(3 == iorp->CurrentLocation, "Irp CurrentLocation should be 3, but got %d\n",
            iorp->CurrentLocation);
        ok(IsListEmpty(&iorp->ThreadListEntry), "IRP thread list is not empty\n");
        ok ((PIO_STACK_LOCATION)(iorp + 1) + 2 ==
            iorp->Tail.Overlay.CurrentStackLocation,
            "CurrentStackLocation mismatch\n");
        if (Float > 0)
        {
            ok((IRP_ALLOCATED_FIXED_SIZE & iorp->AllocationFlags),
                "IRP Allocation flags lack fixed size attribute\n");
            ok((IRP_LOOKASIDE_ALLOCATION & iorp->AllocationFlags),
                "IRP Allocation flags lack lookaside allocation\n");
        }
        else
        {
            ok(!(IRP_ALLOCATED_FIXED_SIZE & iorp->AllocationFlags),
                "IRP with no credit left has the fixed size attribute\n");
            ok(!(IRP_LOOKASIDE_ALLOCATION & iorp->AllocationFlags),
                "IRP with no credit left has lookaside allocation\n");
        }

        IoFreeIrp(iorp);
    }
}
