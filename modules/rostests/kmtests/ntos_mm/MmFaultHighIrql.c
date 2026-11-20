/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Page faults above APC_LEVEL must bugcheck
 */

/*
 * A page fault on a page that is not present cannot be resolved above
 * APC_LEVEL: resolving it needs the working set lock, may need the PFN
 * lock and may have to wait for I/O. Windows stops the system with
 * IRQL_NOT_LESS_OR_EQUAL (0xA) instead, before it takes any lock; from a
 * driver, as here, that is DRIVER_IRQL_NOT_LESS_OR_EQUAL (0xD1).
 *
 * Both tests are hidden: on a correct kernel they never return. They pass
 * when the system bugchecks with 0xD1 (P2 = 2, the IRQL), and fail when the
 * fault is resolved (a failure is reported), on any other bugcheck, or when
 * the system hangs.
 *
 *  - MmFaultAtDispatch touches a not yet faulted-in page of a system view
 *    at DISPATCH_LEVEL.
 *  - MmFaultHoldingPfnLock does the same while holding the PFN lock, the
 *    situation of a fault inside the memory manager's own PFN-locked code.
 */

#include <kmt_test.h>

#define VIEW_SIZE (16 * PAGE_SIZE)

static
PVOID
MapFreshView(
    _Out_ PVOID *SectionObject)
{
    NTSTATUS Status;
    HANDLE SectionHandle;
    LARGE_INTEGER MaximumSize;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PVOID Base = NULL;
    SIZE_T ViewSize = VIEW_SIZE;

    *SectionObject = NULL;

    /* A pagefile-backed section: its pages are demand-zero until touched */
    MaximumSize.QuadPart = VIEW_SIZE;
    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateSection(&SectionHandle,
                             SECTION_ALL_ACCESS,
                             &ObjectAttributes,
                             &MaximumSize,
                             PAGE_READWRITE,
                             SEC_COMMIT,
                             NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;

    Status = ObReferenceObjectByHandle(SectionHandle,
                                       SECTION_ALL_ACCESS,
                                       NULL,
                                       KernelMode,
                                       SectionObject,
                                       NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(SectionHandle);
    if (!NT_SUCCESS(Status))
        return NULL;

    Status = MmMapViewInSystemSpace(*SectionObject, &Base, &ViewSize);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(*SectionObject);
        *SectionObject = NULL;
        return NULL;
    }

    /* Nothing touched the view yet, so its pages are not present */
    ok(!MmIsAddressValid((PUCHAR)Base + VIEW_SIZE / 2),
       "View page %p is already valid\n", (PUCHAR)Base + VIEW_SIZE / 2);

    return Base;
}

static
VOID
UnmapView(
    _In_ PVOID Base,
    _In_ PVOID SectionObject)
{
    MmUnmapViewInSystemSpace(Base);
    ObDereferenceObject(SectionObject);
}

START_TEST(MmFaultAtDispatch)
{
    PVOID Base, SectionObject;
    volatile ULONG *Page;
    ULONG Value;
    KIRQL OldIrql;

    Base = MapFreshView(&SectionObject);
    if (!Base)
        return;
    Page = (volatile ULONG *)((PUCHAR)Base + VIEW_SIZE / 2);

    trace("Touching not-present %p at DISPATCH_LEVEL; expecting bugcheck 0xD1\n", Page);
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Value = *Page;
    KeLowerIrql(OldIrql);

    ok(FALSE, "A not-present fault at DISPATCH_LEVEL was resolved (read 0x%lx) instead of bugchecking\n", Value);

    UnmapView(Base, SectionObject);
}

START_TEST(MmFaultHoldingPfnLock)
{
    PVOID Base, SectionObject;
    volatile ULONG *Page;
    ULONG Value;
    KIRQL OldIrql;

    Base = MapFreshView(&SectionObject);
    if (!Base)
        return;
    Page = (volatile ULONG *)((PUCHAR)Base + VIEW_SIZE / 2);

    trace("Touching not-present %p holding the PFN lock; expecting bugcheck 0xD1\n", Page);
    OldIrql = KeAcquireQueuedSpinLock(LockQueuePfnLock);
    Value = *Page;
    KeReleaseQueuedSpinLock(LockQueuePfnLock, OldIrql);

    ok(FALSE, "A not-present fault under the PFN lock was resolved (read 0x%lx) instead of bugchecking\n", Value);

    UnmapView(Base, SectionObject);
}
