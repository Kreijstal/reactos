/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite DMA adapter channel / map register test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * A bus master that cannot reach all memory gets map registers from the one
 * pool every adapter shares. A request that has to wait for them (the pool is
 * used up, or other adapters are already waiting) sits on the master adapter
 * queue until registers are freed or the pool grows. Requests queued on one
 * adapter's channel are started one after the other by IoFreeAdapterChannel,
 * and each of them must be given map registers of its own: a request started
 * while others wait on the master queue must wait too, it must never run on
 * registers nobody reserved for it (the previous request's freed ones), or the
 * next adapter that asks is handed the same registers, i.e. the same bounce
 * pages, and the two transfers corrupt each other.
 *
 * The pool grows by what a waiting request asks for, but a growth is handed
 * out from the head of the master queue, which need not be the request it was
 * sized for: a small request ahead takes a page of a large request's growth.
 * Once the pool has grown, every waiting request must be served all the same;
 * none may be left waiting for registers some other adapter will never free.
 *
 * The map register table the pool lives in has a fixed size, and every growth
 * takes slots of it for good. Growing by one page for every one-register
 * request that waits must not use the table up long before the pool reaches
 * its size: a pool of small long-lived grants (a NIC's transmit ring) has to
 * keep growing, or the next request waits for a free that may never come.
 */

#include <kmt_test.h>

#define FILL_REGISTERS  16      /* per request of the adapters that use up the pool */
#define MAX_FILLERS     320     /* 320 * 16 > the largest pool the HAL builds */
#define MAX_PROBES      96

typedef struct _GRANT
{
    PDMA_ADAPTER Adapter;
    PDEVICE_OBJECT DeviceObject;
    ULONG Count;
    volatile LONG Granted;
    volatile LONG Requested;
    PVOID Base;
} GRANT, *PGRANT;

static GRANT Fillers[MAX_FILLERS];
static GRANT Probes[MAX_PROBES];
static GRANT First, Second, Third;

static ULONG FillersUsed, ProbesUsed;
static BOOLEAN PoolFull;

static
IO_ALLOCATION_ACTION
NTAPI
KeepRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID MapRegisterBase,
    _In_ PVOID Context)
{
    PGRANT Grant = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    Grant->Base = (PVOID)((ULONG_PTR)MapRegisterBase & ~(ULONG_PTR)1); /* MAP_BASE_SW_SG */
    InterlockedIncrement(&Grant->Granted);
    return KeepObject;
}

static
BOOLEAN
Request(
    _In_ PGRANT Grant)
{
    NTSTATUS Status;

    Grant->Requested = TRUE;
    Status = Grant->Adapter->DmaOperations->AllocateAdapterChannel(Grant->Adapter,
                                                                   Grant->DeviceObject,
                                                                   Grant->Count,
                                                                   KeepRoutine,
                                                                   Grant);
    ok_eq_hex(Status, STATUS_SUCCESS);
    /* TRUE when the request was granted at once */
    return Grant->Granted != 0;
}

/*
 * The second request on the shared adapter: it was started by
 * IoFreeAdapterChannel from that adapter's channel queue. Use up the pool so
 * that a request ends up waiting on the master adapter queue, then give the
 * adapter back with this request's register freed.
 */
static
IO_ALLOCATION_ACTION
NTAPI
SecondRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID MapRegisterBase,
    _In_ PVOID Context)
{
    PGRANT Grant = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    Grant->Base = (PVOID)((ULONG_PTR)MapRegisterBase & ~(ULONG_PTR)1);
    InterlockedIncrement(&Grant->Granted);

    for (FillersUsed = 0; FillersUsed < MAX_FILLERS; FillersUsed++)
    {
        if (!Request(&Fillers[FillersUsed]))
        {
            /* This one waits on the master adapter queue now */
            FillersUsed++;
            PoolFull = TRUE;
            break;
        }
    }
    return DeallocateObject;
}

static
PDMA_ADAPTER
GetAdapter(
    _In_ ULONG MaximumLength)
{
    DEVICE_DESCRIPTION Description;
    ULONG NumberOfMapRegisters = 0;

    /* A 32-bit bus master without scatter/gather always needs map registers */
    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.InterfaceType = PCIBus;
    Description.MaximumLength = MaximumLength;
    return IoGetDmaAdapter(NULL, &Description, &NumberOfMapRegisters);
}

static
BOOLEAN
InitGrant(
    _Out_ PGRANT Grant,
    _In_opt_ PDMA_ADAPTER Adapter,
    _In_ ULONG Count)
{
    NTSTATUS Status;

    RtlZeroMemory(Grant, sizeof(*Grant));
    Grant->Count = Count;
    Grant->Adapter = Adapter ? Adapter : GetAdapter(Count * PAGE_SIZE - PAGE_SIZE);
    if (!Grant->Adapter)
        return FALSE;
    /* Each request needs its own wait context block, i.e. device object */
    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &Grant->DeviceObject);
    return NT_SUCCESS(Status);
}

static
ULONG
CountDuplicates(VOID)
{
    PGRANT All[MAX_FILLERS + MAX_PROBES + 3];
    ULONG Count = 0, i, j, Duplicates = 0;

    if (Third.Granted) All[Count++] = &Third;
    for (i = 0; i < FillersUsed; i++)
        if (Fillers[i].Granted) All[Count++] = &Fillers[i];
    for (i = 0; i < ProbesUsed; i++)
        if (Probes[i].Granted) All[Count++] = &Probes[i];

    for (i = 0; i < Count; i++)
    {
        for (j = i + 1; j < Count; j++)
        {
            if (All[i]->Base == All[j]->Base)
            {
                if (Duplicates++ < 3)
                    trace("Map registers at %p held twice\n", All[i]->Base);
            }
        }
    }
    return Duplicates;
}

/*
 * Growths run in delayed worker threads, so under load they finish in no
 * particular order. Reproduce the order that loses a request: a small request
 * P waits ahead of a large one B, and B's growth is done first. The queue walk
 * after it serves the head, P, with one page of B's new run, and B is left one
 * register short. P's own growth then adds a page elsewhere and B still waits;
 * with nobody freeing registers it must be served by growing the pool again.
 *
 * The worker that gets P's growth is held up deterministically: every delayed
 * worker is parked in a test work item first, then one of them is let go to
 * a processor that a DPC keeps busy, and one is let go normally. A queue hands
 * a new item to the thread that started waiting last, so P's growth goes to
 * the held-up thread and B's growth to the free one.
 */
#define MAX_BLOCKERS 128

typedef enum _BLOCKER_ROLE
{
    RoleWait,
    RoleExit,
    RolePin,
    RoleUnpin
} BLOCKER_ROLE;

typedef struct _BLOCKER
{
    WORK_QUEUE_ITEM Item;
    KEVENT Release;
    volatile LONG Role;
    KAFFINITY Affinity;
    volatile LONG Started;
    volatile LONG Done;
} BLOCKER, *PBLOCKER;

static BLOCKER Blockers[MAX_BLOCKERS];
static volatile LONG BlockersNoWait;
static KDPC HogDpc;
static volatile LONG HogRelease, HogRunning;
static GRANT Big;

static
VOID
NTAPI
BlockerRoutine(
    _In_ PVOID Context)
{
    PBLOCKER Blocker = Context;

    InterlockedExchange(&Blocker->Started, 1);
    if (Blocker->Role == RoleWait && !BlockersNoWait)
        KeWaitForSingleObject(&Blocker->Release, Executive, KernelMode, FALSE, NULL);
    if (Blocker->Role == RolePin)
        KeSetSystemAffinityThread(Blocker->Affinity);
    else if (Blocker->Role == RoleUnpin)
    {
        /* Only one worker was pinned; reverting needs a system affinity */
        KeSetSystemAffinityThread(KeQueryActiveProcessors());
        KeRevertToUserAffinityThread();
    }
    InterlockedExchange(&Blocker->Done, 1);
}

static
VOID
NTAPI
HogRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    InterlockedExchange(&HogRunning, 1);
    while (!HogRelease)
        YieldProcessor();
    InterlockedExchange(&HogRunning, 0);
}

static
BOOLEAN
WaitFor(
    _In_ volatile LONG *Flag,
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Delay;
    ULONG i;

    Delay.QuadPart = -10 * 10000;
    for (i = 0; i < Milliseconds / 10; i++)
    {
        if (*Flag)
            return TRUE;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    return *Flag != 0;
}

/* Park every delayed worker thread in a blocker; returns how many there are */
static
ULONG
ParkWorkers(
    _In_ BLOCKER_ROLE Role)
{
    ULONG Count;

    BlockersNoWait = FALSE;
    for (Count = 0; Count < MAX_BLOCKERS; Count++)
    {
        PBLOCKER Blocker = &Blockers[Count];

        RtlZeroMemory(Blocker, sizeof(*Blocker));
        KeInitializeEvent(&Blocker->Release, NotificationEvent, FALSE);
        Blocker->Role = Role;
        ExInitializeWorkItem(&Blocker->Item, BlockerRoutine, Blocker);
        ExQueueWorkItem(&Blocker->Item, DelayedWorkQueue);
        if (!WaitFor(&Blocker->Started, 300))
            break;
    }
    /* Blockers[Count] is still queued: it runs as soon as a worker is free */
    return Count;
}

static
VOID
ReleaseWorkers(
    _In_ ULONG Count)
{
    ULONG i;

    BlockersNoWait = TRUE;
    for (i = 0; i <= Count && i < MAX_BLOCKERS; i++)
        KeSetEvent(&Blockers[i].Release, IO_NO_INCREMENT, FALSE);
    for (i = 0; i <= Count && i < MAX_BLOCKERS; i++)
        WaitFor(&Blockers[i].Done, 5000);
}

static
VOID
FreeGrant(
    _In_ PGRANT Grant)
{
    KIRQL OldIrql;

    if (Grant->Granted != 1)
        return;
    Grant->Granted = 2;
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Grant->Adapter->DmaOperations->FreeAdapterChannel(Grant->Adapter);
    KeLowerIrql(OldIrql);
}

static
BOOLEAN
TestGrowthOutOfOrder(VOID)
{
    ULONG Self, Hog, Workers, Fill, ProbeCount, i;
    KIRQL OldIrql;
    BOOLEAN Queued = FALSE, BigServed, Ok = TRUE;
    PGRANT Small = NULL;

    if (KeNumberProcessors < 2)
    {
        skip(FALSE, "Needs two processors\n");
        return TRUE;
    }
    if (!InitGrant(&Big, NULL, FILL_REGISTERS))
    {
        skip(FALSE, "No grants\n");
        return TRUE;
    }
    for (i = 0; i < MAX_FILLERS; i++)
        Fillers[i].Granted = Fillers[i].Requested = 0;
    for (i = 0; i < MAX_PROBES; i++)
        Probes[i].Granted = Probes[i].Requested = 0;

    /* Stay on one processor; hog another one */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Self = KeGetCurrentProcessorNumber();
    KeLowerIrql(OldIrql);
    KeSetSystemAffinityThread(AFFINITY_MASK(Self));
    Hog = (Self + 1) % KeNumberProcessors;

    Workers = ParkWorkers(RoleWait);
    trace("%lu delayed worker threads parked\n", Workers);
    if (Workers < 2 || Workers >= MAX_BLOCKERS)
    {
        ReleaseWorkers(Workers);
        KeRevertToUserAffinityThread();
        skip(FALSE, "Could not park the worker threads\n");
        return TRUE;
    }
    /* The blocker still queued must not wait when it gets a thread */
    Blockers[Workers].Role = RoleExit;

    /* Use up the pool: the filler that has to wait gets its growth once the
     * first worker is let go below */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    for (Fill = 0; Fill < MAX_FILLERS; )
    {
        if (!Request(&Fillers[Fill++]))
        {
            Queued = TRUE;
            break;
        }
    }
    KeLowerIrql(OldIrql);

    /* The free worker: runs the queued blocker, the filler's growth, then waits */
    Blockers[0].Role = RoleExit;
    KeSetEvent(&Blockers[0].Release, IO_NO_INCREMENT, FALSE);
    ok(WaitFor(&Fillers[Fill - 1].Granted, 5000), "The last filler was not served\n");
    ok(WaitFor(&Blockers[Workers].Done, 5000), "The queued blocker did not run\n");

    /* Take what is left in the holes, one register at a time, until the pool
     * is used up exactly: the probe that has to wait is served by a growth of
     * one page */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    for (ProbeCount = 0; ProbeCount < MAX_PROBES; )
    {
        if (!Request(&Probes[ProbeCount++]))
            break;
    }
    KeLowerIrql(OldIrql);
    ok(WaitFor(&Probes[ProbeCount - 1].Granted, 5000), "The last probe was not served\n");
    trace("%lu fillers, pool full %u, %lu probes\n", Fill, Queued, ProbeCount);

    /* The held-up worker: waits for work last, on the hogged processor */
    Blockers[1].Affinity = AFFINITY_MASK(Hog);
    Blockers[1].Role = RolePin;
    KeSetEvent(&Blockers[1].Release, IO_NO_INCREMENT, FALSE);
    ok(WaitFor(&Blockers[1].Done, 5000), "The pinned worker did not run\n");
    {
        LARGE_INTEGER Delay;
        Delay.QuadPart = -50 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    KeInitializeDpc(&HogDpc, HogRoutine, NULL);
    KeSetTargetProcessorDpc(&HogDpc, (CCHAR)Hog);
    KeSetImportanceDpc(&HogDpc, HighImportance);
    HogRelease = 0;
    KeInsertQueueDpc(&HogDpc, NULL, NULL);
    ok(WaitFor(&HogRunning, 5000), "The hog DPC did not run\n");

    /* P's growth goes to the held-up worker, B's to the free one. Other
     * drivers may have freed registers meanwhile: P is the first one-register
     * request that has to wait. */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    while (ProbeCount < MAX_PROBES)
    {
        if (!Request(&Probes[ProbeCount++]))
        {
            Small = &Probes[ProbeCount - 1];
            break;
        }
    }
    if (Small)
        ok(!Request(&Big), "The large request did not wait\n");
    KeLowerIrql(OldIrql);
    if (!Small)
    {
        /* Nothing is queued; Big was never requested */
        ok(FALSE, "The pool was not used up\n");
        InterlockedExchange(&HogRelease, 1);
        Big.Granted = 2;
        Small = &Big;
    }

    /* Nobody frees anything: B must be served by the pool growing */
    BigServed = WaitFor(&Big.Granted, 2000);
    ok(BigServed, "The large request still waits for map registers after its growth ran\n");
    trace("Small granted %ld, Big granted %ld before P's growth ran\n", Small->Granted, Big.Granted);

    /* Let P's growth run; still nobody frees anything */
    InterlockedExchange(&HogRelease, 1);
    WaitFor(&Small->Granted, 5000);
    if (!BigServed)
    {
        BigServed = WaitFor(&Big.Granted, 2000);
        ok(BigServed, "The large request still waits after every growth ran\n");
    }
    ok(Small->Granted == 1, "Small request granted %ld times\n", Small->Granted);

    /* Give everything back; freeing serves whatever still waits */
    for (i = 0; i < Fill; i++)
        FreeGrant(&Fillers[i]);
    for (i = 0; i < ProbeCount; i++)
        FreeGrant(&Probes[i]);
    if (!WaitFor(&Big.Granted, 5000))
        Ok = FALSE;
    FreeGrant(&Big);
    for (i = 0; i < Fill; i++)
        if (!Fillers[i].Granted) Ok = FALSE;
    for (i = 0; i < ProbeCount; i++)
        if (!Probes[i].Granted) Ok = FALSE;
    ok(Ok, "Requests still wait after everything was freed\n");

    /* Undo the pin: every worker reverts its affinity once */
    ReleaseWorkers(Workers);
    Workers = ParkWorkers(RoleUnpin);
    ReleaseWorkers(Workers);
    KeRevertToUserAffinityThread();

    if (Ok)
    {
        Big.Adapter->DmaOperations->PutDmaAdapter(Big.Adapter);
        IoDeleteDevice(Big.DeviceObject);
    }
    return Ok;
}

/*
 * Hold one-register grants, one request at a time, until 12 MB of map
 * buffers are held: three quarters of the 4096 entry map register table.
 * Nobody frees anything, so every request the pool cannot serve waits for a
 * growth. The grants are kept past the adapter channel
 * (DeallocateObjectKeepRegisters), the way a NIC holds the registers of its
 * transmit ring, so one adapter does for all of them.
 *
 * Every grant is followed by a contiguous page allocation of our own, in the
 * range the map buffers come from, as other drivers' common buffers are on a
 * running system: the next growth then does not continue the previous one
 * physically, and the map register table spends a placeholder slot on it.
 */
#define CAPACITY_GRANTS 3072

typedef struct _CAPACITY
{
    KEVENT Granted;
    PVOID Base;
} CAPACITY, *PCAPACITY;

static PVOID CapacityBases[CAPACITY_GRANTS];
static PVOID CapacitySpacers[CAPACITY_GRANTS];
static CAPACITY Capacity;  /* the routine may still run after a failed test returns */

static
IO_ALLOCATION_ACTION
NTAPI
CapacityRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID MapRegisterBase,
    _In_ PVOID Context)
{
    PCAPACITY Capacity = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    Capacity->Base = MapRegisterBase;
    KeSetEvent(&Capacity->Granted, IO_NO_INCREMENT, FALSE);
    return DeallocateObjectKeepRegisters;
}

static
BOOLEAN
TestGrowthCapacity(VOID)
{
    PDMA_ADAPTER Adapter;
    PDEVICE_OBJECT DeviceObject;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    KIRQL OldIrql;
    ULONG Held, i;
    BOOLEAN Waiting = FALSE;

    Adapter = GetAdapter(PAGE_SIZE / 2);
    if (!Adapter)
    {
        skip(FALSE, "No adapter\n");
        return TRUE;
    }
    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        skip(FALSE, "No device object\n");
        Adapter->DmaOperations->PutDmaAdapter(Adapter);
        return TRUE;
    }
    KeInitializeEvent(&Capacity.Granted, NotificationEvent, FALSE);

    for (Held = 0; Held < CAPACITY_GRANTS; Held++)
    {
        KeClearEvent(&Capacity.Granted);
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Status = Adapter->DmaOperations->AllocateAdapterChannel(Adapter, DeviceObject, 1,
                                                                CapacityRoutine, &Capacity);
        KeLowerIrql(OldIrql);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            break;

        Timeout.QuadPart = -10 * 1000 * 10000LL;    /* 10 s */
        Status = KeWaitForSingleObject(&Capacity.Granted, Executive, KernelMode, FALSE, &Timeout);
        if (Status != STATUS_SUCCESS)
        {
            Waiting = TRUE;
            break;
        }
        CapacityBases[Held] = Capacity.Base;

        {
            PHYSICAL_ADDRESS Lowest, Highest, Boundary;

            Lowest.QuadPart = 0x1000000;
            Highest.QuadPart = 0xFFFFFFFF;
            Boundary.QuadPart = 0;
            CapacitySpacers[Held] = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, Lowest, Highest,
                                                                           Boundary, MmNonCached);
        }
    }
    ok(Held == CAPACITY_GRANTS,
       "Request %lu still waits for map registers with %lu pages held: the pool stopped growing\n",
       Held + 1, Held);
    trace("%lu one-register grants held\n", Held);

    /* Give everything back; freeing serves the request that still waits */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    for (i = 0; i < Held; i++)
        Adapter->DmaOperations->FreeMapRegisters(Adapter, CapacityBases[i], 1);
    KeLowerIrql(OldIrql);
    if (Waiting)
    {
        Timeout.QuadPart = -10 * 1000 * 10000LL;
        Status = KeWaitForSingleObject(&Capacity.Granted, Executive, KernelMode, FALSE, &Timeout);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (Status != STATUS_SUCCESS)
            return FALSE;   /* It would call back into a freed device object */
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Adapter->DmaOperations->FreeMapRegisters(Adapter, Capacity.Base, 1);
        KeLowerIrql(OldIrql);
    }

    for (i = 0; i < Held; i++)
    {
        if (CapacitySpacers[i])
            MmFreeContiguousMemorySpecifyCache(CapacitySpacers[i], PAGE_SIZE, MmNonCached);
        CapacitySpacers[i] = NULL;
    }

    IoDeleteDevice(DeviceObject);
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
    return TRUE;
}

START_TEST(HalAdapterChannel)
{
    PDMA_ADAPTER Shared;
    LARGE_INTEGER Delay;
    KIRQL OldIrql;
    ULONG i, Wait, Pass, Pending = 0;
    BOOLEAN ThirdAtOnce;

    /* The adapter whose channel queue holds three one-register requests */
    Shared = GetAdapter(PAGE_SIZE / 2);
    ok(Shared != NULL, "IoGetDmaAdapter failed\n");
    if (skip(Shared != NULL, "No adapter\n"))
        return;
    if (!InitGrant(&First, Shared, 1) || !InitGrant(&Second, Shared, 1) || !InitGrant(&Third, Shared, 1))
    {
        skip(FALSE, "No device objects\n");
        return;
    }
    for (i = 0; i < MAX_FILLERS; i++)
    {
        if (!InitGrant(&Fillers[i], NULL, FILL_REGISTERS))
        {
            skip(FALSE, "No filler %lu\n", i);
            return;
        }
    }
    for (i = 0; i < MAX_PROBES; i++)
    {
        if (!InitGrant(&Probes[i], NULL, 1))
        {
            skip(FALSE, "No probe %lu\n", i);
            return;
        }
    }

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* First holds the channel, Second and Third queue up behind it */
    ok(Request(&First), "The first request did not get map registers\n");
    Second.Requested = TRUE;
    ok_eq_hex(Shared->DmaOperations->AllocateAdapterChannel(Shared, Second.DeviceObject, 1,
                                                            SecondRoutine, &Second),
              STATUS_SUCCESS);
    ok(!Request(&Third), "The third request was not queued on the busy channel\n");

    /* Start Second; it fills the pool and gives the adapter back, which
     * starts Third while a filler waits on the master adapter queue */
    Shared->DmaOperations->FreeAdapterChannel(Shared);
    ok(Second.Granted == 1, "Second request granted %ld times\n", Second.Granted);
    ThirdAtOnce = (Third.Granted != 0);
    trace("%lu fillers, pool full %u, third started at once %u (Second %p, Third %p)\n",
          FillersUsed, PoolFull, ThirdAtOnce, Second.Base, Third.Base);

    if (PoolFull)
    {
        /* Others wait on the master queue: the third request must wait too */
        ok(!ThirdAtOnce, "The third request started while others waited for map registers\n");
    }
    else
    {
        skip(FALSE, "Could not use up the map register pool\n");
    }
    KeLowerIrql(OldIrql);

    /* The waiting requests start once the pool has grown */
    Delay.QuadPart = -100 * 10000;
    for (Pass = 0; Pass < 2; Pass++)
    {
        for (Wait = 0; Wait < 100; Wait++)
        {
            Pending = !Third.Granted;
            for (i = 0; i < FillersUsed; i++)
                Pending += !Fillers[i].Granted;
            for (i = 0; i < ProbesUsed; i++)
                Pending += !Probes[i].Granted;
            if (!Pending)
                break;
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        }
        ok_eq_ulong(Pending, 0UL);
        ok_eq_ulong(CountDuplicates(), 0UL);
        if (Pass || Pending)
            break;

        /* Now take every map register still free, one by one: none of them
         * may be one that a running request already holds */
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        for (ProbesUsed = 0; ProbesUsed < MAX_PROBES; ProbesUsed++)
        {
            if (!Request(&Probes[ProbesUsed]))
            {
                ProbesUsed++;
                break;
            }
        }
        trace("%lu probes\n", ProbesUsed);
        ok_eq_ulong(CountDuplicates(), 0UL);
        KeLowerIrql(OldIrql);
    }
    ok(Third.Granted == 1, "Third request granted %ld times\n", Third.Granted);

    /* Give everything back */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    if (Third.Granted)
        Shared->DmaOperations->FreeAdapterChannel(Shared);
    for (i = 0; i < FillersUsed; i++)
    {
        if (Fillers[i].Granted)
            Fillers[i].Adapter->DmaOperations->FreeAdapterChannel(Fillers[i].Adapter);
    }
    for (i = 0; i < ProbesUsed; i++)
    {
        if (Probes[i].Granted)
            Probes[i].Adapter->DmaOperations->FreeAdapterChannel(Probes[i].Adapter);
    }
    KeLowerIrql(OldIrql);

    if (Pending)
    {
        /* Requests still queued would call back into freed objects */
        trace("Leaking the adapters, %lu requests still wait\n", Pending);
        return;
    }
    if (!TestGrowthOutOfOrder() || !TestGrowthCapacity())
    {
        /* Requests still queued would call back into freed objects */
        trace("Leaking the adapters, requests still wait\n");
        return;
    }

    for (i = 0; i < MAX_FILLERS; i++)
    {
        Fillers[i].Adapter->DmaOperations->PutDmaAdapter(Fillers[i].Adapter);
        IoDeleteDevice(Fillers[i].DeviceObject);
    }
    for (i = 0; i < MAX_PROBES; i++)
    {
        Probes[i].Adapter->DmaOperations->PutDmaAdapter(Probes[i].Adapter);
        IoDeleteDevice(Probes[i].DeviceObject);
    }
    IoDeleteDevice(First.DeviceObject);
    IoDeleteDevice(Second.DeviceObject);
    IoDeleteDevice(Third.DeviceObject);
    Shared->DmaOperations->PutDmaAdapter(Shared);
}
