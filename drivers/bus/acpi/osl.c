/*******************************************************************************
*                                                                              *
* ACPI Component Architecture Operating System Layer (OSL) for ReactOS         *
*                                                                              *
*******************************************************************************/

#include "precomp.h"

#include <arc/arc.h>
#include <reactos/drivers/acpi/acpi.h>
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

#define TAG_ACPI_LOADER_RSDP_TABLE   'RpcA'

static PKINTERRUPT AcpiInterrupt;
static BOOLEAN AcpiInterruptHandlerRegistered = FALSE;
static ACPI_OSD_HANDLER AcpiIrqHandler = NULL;
static PVOID AcpiIrqContext = NULL;
static ULONG AcpiIrqNumber = 0;
static ULONG AcpiSciVector = 0;
static KIRQL AcpiSciIrql = PASSIVE_LEVEL;
static volatile LONG AcpiSciMasked = 0;
/* SCI delivery: ISR (DIRQL) just queues the DPC; the DPC (DISPATCH_LEVEL)
 * runs the ACPICA service routine, where AcpiOsExecute() may safely
 * allocate non-paged pool and queue a worker. The ISR masks the
 * level-triggered SCI until the DPC clears its status bits; otherwise a
 * firmware-pending SCI can immediately retrigger forever and starve the DPC. */
static KDPC AcpiSciDpc;
/* The SCI is a shareable level line: on this hardware other devices (the
 * ASUS X550DP routes OHCI to the same link) can assert it while ACPI owns
 * nothing. Claiming unconditionally starves the sharing device's ISR and
 * turns its pending interrupt into an endless ghost-SCI storm. Cache the
 * PM1/GPE register ports from the FADT so the ISR can test the
 * architectural SCI condition (STS & EN) at DIRQL and decline interrupts
 * that are not ours. */
static struct
{
    USHORT Pm1aSts;
    USHORT Pm1aEn;
    USHORT Pm1bSts;
    USHORT Pm1bEn;
    USHORT Gpe0Sts;
    USHORT Gpe0En;
    USHORT Gpe1Sts;
    USHORT Gpe1En;
    UCHAR Gpe0Half;
    UCHAR Gpe1Half;
    BOOLEAN Ready;
} AcpiSciHw;
static volatile ULONG AcpiSciTotalCount = 0;
static volatile ULONG AcpiSciOwnedCount = 0;
static volatile ULONG AcpiSciGhostCount = 0;
static ULONG AcpiSciConsecutiveGhosts = 0;
/* TODO: Replace these local declarations with the NDK headers once the
 * acpi.sys build context can consume the required NDK dependencies cleanly.
 */
NTHALAPI
VOID
NTAPI
HalDisableSystemInterrupt(
    _In_ ULONG Vector,
    _In_ KIRQL Irql);

NTHALAPI
BOOLEAN
NTAPI
HalEnableSystemInterrupt(
    _In_ ULONG Vector,
    _In_ KIRQL Irql,
    _In_ KINTERRUPT_MODE InterruptMode);

extern NTSYSAPI PLOADER_PARAMETER_BLOCK KeLoaderBlock;
extern NTSYSAPI
PCONFIGURATION_COMPONENT_DATA
NTAPI
KeFindConfigurationNextEntry(
    _In_ PCONFIGURATION_COMPONENT_DATA Child,
    _In_ CONFIGURATION_CLASS Class,
    _In_ CONFIGURATION_TYPE Type,
    _In_opt_ PULONG ComponentKey,
    _Inout_ PCONFIGURATION_COMPONENT_DATA *NextLink);

static ACPI_TABLE_RSDP *AcpiLoaderRsdp = NULL;

static
UCHAR
AcpiChecksumBuffer(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length)
{
    ULONG Index;
    UCHAR Sum;

    Sum = 0;
    for (Index = 0; Index < Length; ++Index)
        Sum = (UCHAR)(Sum + Buffer[Index]);

    return (UCHAR)(0 - Sum);
}

static
PACPI_BIOS_MULTI_NODE
AcpiGetLoaderAcpiBiosNode(VOID)
{
    PCONFIGURATION_COMPONENT_DATA ComponentEntry = NULL;
    PCONFIGURATION_COMPONENT_DATA Next = NULL;
    PCM_PARTIAL_RESOURCE_LIST ResourceList;

    if (!KeLoaderBlock || !KeLoaderBlock->ConfigurationRoot)
        return NULL;

    ComponentEntry = KeFindConfigurationNextEntry(KeLoaderBlock->ConfigurationRoot,
                                                  AdapterClass,
                                                  MultiFunctionAdapter,
                                                  NULL,
                                                  &Next);
    while (ComponentEntry)
    {
        if ((ComponentEntry->ComponentEntry.Identifier != NULL) &&
            !_stricmp(ComponentEntry->ComponentEntry.Identifier, "ACPI BIOS"))
        {
            break;
        }

        Next = ComponentEntry;
        ComponentEntry = KeFindConfigurationNextEntry(KeLoaderBlock->ConfigurationRoot,
                                                      AdapterClass,
                                                      MultiFunctionAdapter,
                                                      NULL,
                                                      &Next);
    }

    if (!ComponentEntry)
        return NULL;

    ResourceList = ComponentEntry->ConfigurationData;
    if (!ResourceList ||
        (ComponentEntry->ComponentEntry.ConfigurationDataLength <
         FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[1]) +
         FIELD_OFFSET(ACPI_BIOS_MULTI_NODE, E820Entry)) ||
        (ResourceList->Count < 1) ||
        (ResourceList->PartialDescriptors[0].Type != CmResourceTypeDeviceSpecific))
    {
        DPRINT1("Loader ACPI BIOS node is missing valid device-specific data\n");
        return NULL;
    }

    return (PACPI_BIOS_MULTI_NODE)(ResourceList + 1);
}

static
ACPI_PHYSICAL_ADDRESS
AcpiBuildLoaderRootPointer(VOID)
{
    PACPI_BIOS_MULTI_NODE NodeData;
    ACPI_TABLE_HEADER *RootTable = NULL;
    PHYSICAL_ADDRESS PhysicalAddress;

    if (AcpiLoaderRsdp != NULL)
    {
        PhysicalAddress = MmGetPhysicalAddress(AcpiLoaderRsdp);
        return (ACPI_PHYSICAL_ADDRESS)PhysicalAddress.QuadPart;
    }

    NodeData = AcpiGetLoaderAcpiBiosNode();
    if (!NodeData || (NodeData->RsdtAddress.QuadPart == 0))
        goto Failure;

    RootTable = MmMapIoSpace(NodeData->RsdtAddress, sizeof(*RootTable), MmNonCached);
    if (!RootTable)
    {
        DPRINT1("Unable to map loader ACPI root table at 0x%I64x\n",
                NodeData->RsdtAddress.QuadPart);
        goto Failure;
    }

    AcpiLoaderRsdp = ExAllocatePoolZero(NonPagedPool,
                                        sizeof(*AcpiLoaderRsdp),
                                        TAG_ACPI_LOADER_RSDP_TABLE);
    if (!AcpiLoaderRsdp)
    {
        DPRINT1("Unable to allocate synthetic RSDP for loader ACPI tables\n");
        goto Failure;
    }

    RtlCopyMemory(AcpiLoaderRsdp->Signature,
                  ACPI_SIG_RSDP,
                  sizeof(AcpiLoaderRsdp->Signature));
    RtlCopyMemory(AcpiLoaderRsdp->OemId, "ROS   ", sizeof(AcpiLoaderRsdp->OemId));

    if (RtlEqualMemory(RootTable->Signature, ACPI_SIG_XSDT, ACPI_NAMESEG_SIZE))
    {
        AcpiLoaderRsdp->Revision = 2;
        AcpiLoaderRsdp->Length = sizeof(*AcpiLoaderRsdp);
        AcpiLoaderRsdp->XsdtPhysicalAddress = NodeData->RsdtAddress.QuadPart;
    }
    else if (RtlEqualMemory(RootTable->Signature, ACPI_SIG_RSDT, ACPI_NAMESEG_SIZE))
    {
        if (NodeData->RsdtAddress.QuadPart > MAXULONG)
        {
            DPRINT1("Loader RSDT address does not fit in 32 bits: 0x%I64x\n",
                    NodeData->RsdtAddress.QuadPart);
            goto Failure;
        }

        AcpiLoaderRsdp->Revision = 0;
        AcpiLoaderRsdp->RsdtPhysicalAddress = NodeData->RsdtAddress.LowPart;
    }
    else
    {
        DPRINT1("Loader ACPI root table has unexpected signature '%.4s'\n",
                RootTable->Signature);
        goto Failure;
    }

    AcpiLoaderRsdp->Checksum = AcpiChecksumBuffer((const UCHAR *)AcpiLoaderRsdp,
                                                  ACPI_RSDP_CHECKSUM_LENGTH);
    if (AcpiLoaderRsdp->Revision >= 2)
    {
        AcpiLoaderRsdp->ExtendedChecksum =
            AcpiChecksumBuffer((const UCHAR *)AcpiLoaderRsdp,
                               sizeof(*AcpiLoaderRsdp));
    }

    MmUnmapIoSpace(RootTable, sizeof(*RootTable));
    PhysicalAddress = MmGetPhysicalAddress(AcpiLoaderRsdp);
    return (ACPI_PHYSICAL_ADDRESS)PhysicalAddress.QuadPart;

Failure:
    if (AcpiLoaderRsdp)
    {
        ExFreePoolWithTag(AcpiLoaderRsdp, TAG_ACPI_LOADER_RSDP_TABLE);
        AcpiLoaderRsdp = NULL;
    }
    if (RootTable)
        MmUnmapIoSpace(RootTable, sizeof(*RootTable));
    return 0;
}

ACPI_STATUS
AcpiOsInitialize (void)
{
    DPRINT("AcpiOsInitialize called\n");

    AcpiBuildLoaderRootPointer();

#ifndef NDEBUG
    /* Verboseness level of the acpica core */
    AcpiDbgLevel = 0x00FFFFFF;
    AcpiDbgLayer = 0xFFFFFFFF;
#endif

    (VOID)AcpiBuildLoaderRootPointer();

    return AE_OK;
}

ACPI_STATUS
AcpiOsTerminate(void)
{
    DPRINT("AcpiOsTerminate() called\n");

    /* Release the synthetic RSDP built from the loader-provided ACPI tables.
     * ACPICA has already torn down its table mappings (AcpiUtSubsystemShutdown)
     * by the time it calls us, so nothing references this buffer anymore. */
    if (AcpiLoaderRsdp != NULL)
    {
        ExFreePoolWithTag(AcpiLoaderRsdp, TAG_ACPI_LOADER_RSDP_TABLE);
        AcpiLoaderRsdp = NULL;
    }

    return AE_OK;
}

ACPI_PHYSICAL_ADDRESS
AcpiOsGetRootPointer (
    void)
{
    ACPI_PHYSICAL_ADDRESS pa = 0;
    PHYSICAL_ADDRESS PhysicalAddress;

    DPRINT("AcpiOsGetRootPointer\n");

    if (AcpiLoaderRsdp != NULL)
    {
        PhysicalAddress = MmGetPhysicalAddress(AcpiLoaderRsdp);
        return (ACPI_PHYSICAL_ADDRESS)PhysicalAddress.QuadPart;
    }

    pa = AcpiBuildLoaderRootPointer();
    if (pa != 0)
        return pa;

    AcpiFindRootPointer(&pa);
    return pa;
}

ACPI_STATUS
AcpiOsPredefinedOverride(
    const ACPI_PREDEFINED_NAMES *PredefinedObject,
    ACPI_STRING                 *NewValue)
{
    if (!PredefinedObject || !NewValue)
    {
        DPRINT1("Invalid parameter\n");
        return AE_BAD_PARAMETER;
    }

    /* No override */
    *NewValue = NULL;

    return AE_OK;
}

ACPI_STATUS
AcpiOsTableOverride(
    ACPI_TABLE_HEADER *ExistingTable,
    ACPI_TABLE_HEADER **NewTable)
{
    if (!ExistingTable || !NewTable)
    {
        DPRINT1("Invalid parameter\n");
        return AE_BAD_PARAMETER;
    }

    /* No override */
    *NewTable = NULL;

    return AE_OK;
}

ACPI_STATUS
AcpiOsPhysicalTableOverride(
    ACPI_TABLE_HEADER       *ExistingTable,
    ACPI_PHYSICAL_ADDRESS   *NewAddress,
    UINT32                  *NewTableLength)
{
    if (!ExistingTable || !NewAddress || !NewTableLength)
    {
        DPRINT1("Invalid parameter\n");
        return AE_BAD_PARAMETER;
    }

    /* No override */
    *NewAddress     = 0;
    *NewTableLength = 0;

    return AE_OK;
}

static
BOOLEAN
OslIsRamRange(
    ULONG64 Start,
    ULONG64 Length)
{
    static ULONG64 RamBase = 0, RamEnd = 0;
    ULONG64 End = Start + Length;

    if (RamEnd == 0)
    {
        PPHYSICAL_MEMORY_RANGE Ranges, Range;

        Ranges = MmGetPhysicalMemoryRanges();
        if (!Ranges)
            return FALSE;

        RamBase = ~0ULL;
        for (Range = Ranges; Range->NumberOfBytes.QuadPart != 0; Range++)
        {
            ULONG64 RangeStart = (ULONG64)Range->BaseAddress.QuadPart;
            ULONG64 RangeEnd = RangeStart + (ULONG64)Range->NumberOfBytes.QuadPart;

            if (RangeStart < RamBase) RamBase = RangeStart;
            if (RangeEnd > RamEnd) RamEnd = RangeEnd;
        }
        ExFreePool(Ranges);

        if (RamEnd == 0)
            return FALSE;
    }

    /*
     * Treat the whole RAM envelope as RAM-backed: the ACPI tables live in
     * firmware reclaim/NVS regions that the memory manager excludes from
     * its physical memory ranges, yet they are ordinary RAM. MMIO sits
     * outside the RAM envelope on the supported platforms.
     */
    return (Start >= RamBase) && (End <= RamEnd);
}

void *
AcpiOsMapMemory (
    ACPI_PHYSICAL_ADDRESS   phys,
    ACPI_SIZE               length)
{
    PHYSICAL_ADDRESS Address;
    MEMORY_CACHING_TYPE CacheType;
    PVOID Ptr;

    DPRINT("AcpiOsMapMemory(phys 0x%p  size 0x%X)\n", phys, length);

    /*
     * ACPI tables live in RAM and ACPICA accesses them with arbitrary
     * (unaligned, byte-granular) loads and stores. Map RAM-backed ranges
     * cached: on ARM64 a non-cached mapping has Device memory semantics,
     * where unaligned accesses take alignment faults. Only real MMIO
     * (operation regions on device memory) gets a non-cached mapping.
     */
    Address.QuadPart = (ULONGLONG)phys;
    CacheType = OslIsRamRange((ULONG64)phys, length) ? MmCached : MmNonCached;
    Ptr = MmMapIoSpace(Address, length, CacheType);
    if (!Ptr)
    {
        DPRINT1("Mapping failed\n");
    }

    return Ptr;
}

void
AcpiOsUnmapMemory (
    void                    *virt,
    ACPI_SIZE               length)
{
    DPRINT("AcpiOsMapMemory(phys 0x%p  size 0x%X)\n", virt, length);

    ASSERT(virt);

    MmUnmapIoSpace(virt, length);
}

ACPI_STATUS
AcpiOsGetPhysicalAddress(
    void *LogicalAddress,
    ACPI_PHYSICAL_ADDRESS *PhysicalAddress)
{
    PHYSICAL_ADDRESS PhysAddr;

    if (!LogicalAddress || !PhysicalAddress)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    PhysAddr = MmGetPhysicalAddress(LogicalAddress);

    *PhysicalAddress = (ACPI_PHYSICAL_ADDRESS)PhysAddr.QuadPart;

    return AE_OK;
}

void *
AcpiOsAllocate (ACPI_SIZE size)
{
    DPRINT("AcpiOsAllocate size %d\n",size);
    return ExAllocatePoolWithTag(NonPagedPool, size, 'ipcA');
}

void
AcpiOsFree(void *ptr)
{
    if (!ptr)
        DPRINT1("Attempt to free null pointer!!!\n");
    ExFreePoolWithTag(ptr, 'ipcA');
}

BOOLEAN
AcpiOsReadable(
    void *Memory,
    ACPI_SIZE Length)
{
    BOOLEAN Ret = FALSE;

    _SEH2_TRY
    {
        ProbeForRead(Memory, Length, sizeof(UCHAR));
        Ret = TRUE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Ret = FALSE;
    }
    _SEH2_END;

    return Ret;
}

BOOLEAN
AcpiOsWritable(
    void *Memory,
    ACPI_SIZE Length)
{
    BOOLEAN Ret = FALSE;

    _SEH2_TRY
    {
        ProbeForWrite(Memory, Length, sizeof(UCHAR));
        Ret = TRUE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Ret = FALSE;
    }
    _SEH2_END;

    return Ret;
}

ACPI_THREAD_ID
AcpiOsGetThreadId (void)
{
    /* Thread ID must be non-zero */
    return (ULONG_PTR)PsGetCurrentThreadId() + 1;
}

typedef struct _ACPI_OSL_WORK_ITEM
{
    WORK_QUEUE_ITEM         Item;
    ACPI_OSD_EXEC_CALLBACK  Function;
    void                   *Context;
} ACPI_OSL_WORK_ITEM, *PACPI_OSL_WORK_ITEM;

static
VOID
NTAPI
AcpiOsExecuteWorker(IN PVOID Parameter)
{
    PACPI_OSL_WORK_ITEM WorkItem = (PACPI_OSL_WORK_ITEM)Parameter;
    ACPI_OSD_EXEC_CALLBACK Function = WorkItem->Function;
    void *Context = WorkItem->Context;

    ExFreePoolWithTag(WorkItem, 'IpcA');
    Function(Context);
}

ACPI_STATUS
AcpiOsExecute (
    ACPI_EXECUTE_TYPE       Type,
    ACPI_OSD_EXEC_CALLBACK  Function,
    void                    *Context)
{
    PACPI_OSL_WORK_ITEM WorkItem;

    DPRINT("AcpiOsExecute\n");

    /*
     * ACPICA may call this from the SCI ISR (DIRQL) - e.g. when dispatching
     * GPE Notify() handlers for hot-plug events. Creating a thread directly
     * would assert at PASSIVE_LEVEL; instead queue a system worker that
     * runs the callback at PASSIVE_LEVEL.
     */
    WorkItem = ExAllocatePoolWithTag(NonPagedPool, sizeof(*WorkItem), 'IpcA');
    if (!WorkItem)
        return AE_NO_MEMORY;

    WorkItem->Function = Function;
    WorkItem->Context = Context;
    ExInitializeWorkItem(&WorkItem->Item, AcpiOsExecuteWorker, WorkItem);
    ExQueueWorkItem(&WorkItem->Item, DelayedWorkQueue);

    return AE_OK;
}

void
AcpiOsSleep (UINT64 milliseconds)
{
    DPRINT("AcpiOsSleep %d\n", milliseconds);
    KeStallExecutionProcessor(milliseconds*1000);
}

void
AcpiOsStall (UINT32 microseconds)
{
    DPRINT("AcpiOsStall %d\n",microseconds);
    KeStallExecutionProcessor(microseconds);
}

ACPI_STATUS
AcpiOsCreateMutex(
    ACPI_MUTEX *OutHandle)
{
    PFAST_MUTEX Mutex;

    if (!OutHandle)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    Mutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(FAST_MUTEX), 'LpcA');
    if (!Mutex) return AE_NO_MEMORY;

    ExInitializeFastMutex(Mutex);

    *OutHandle = (ACPI_MUTEX)Mutex;

    return AE_OK;
}

void
AcpiOsDeleteMutex(
    ACPI_MUTEX Handle)
{
    if (!Handle)
    {
        DPRINT1("Bad parameter\n");
        return;
    }

    ExFreePoolWithTag(Handle, 'LpcA');
}

ACPI_STATUS
AcpiOsAcquireMutex(
    ACPI_MUTEX Handle,
    UINT16 Timeout)
{
    if (!Handle)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    /* Check what the caller wants us to do */
    if (Timeout == ACPI_DO_NOT_WAIT)
    {
        /* Try to acquire without waiting */
        if (!ExTryToAcquireFastMutex((PFAST_MUTEX)Handle))
            return AE_TIME;
    }
    else
    {
        /* Block until we get it */
        ExAcquireFastMutex((PFAST_MUTEX)Handle);
    }

    return AE_OK;
}

void
AcpiOsReleaseMutex(
    ACPI_MUTEX Handle)
{
    if (!Handle)
    {
        DPRINT1("Bad parameter\n");
        return;
    }

    ExReleaseFastMutex((PFAST_MUTEX)Handle);
}

typedef struct _ACPI_SEM {
    UINT32 CurrentUnits;
    KEVENT Event;
    KSPIN_LOCK Lock;
} ACPI_SEM, *PACPI_SEM;

ACPI_STATUS
AcpiOsCreateSemaphore(
    UINT32 MaxUnits,
    UINT32 InitialUnits,
    ACPI_SEMAPHORE *OutHandle)
{
    PACPI_SEM Sem;

    if (!OutHandle)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    Sem = ExAllocatePoolWithTag(NonPagedPool, sizeof(ACPI_SEM), 'LpcA');
    if (!Sem) return AE_NO_MEMORY;

    Sem->CurrentUnits = InitialUnits;
    KeInitializeEvent(&Sem->Event, SynchronizationEvent, Sem->CurrentUnits != 0);
    KeInitializeSpinLock(&Sem->Lock);

    *OutHandle = (ACPI_SEMAPHORE)Sem;

    return AE_OK;
}

ACPI_STATUS
AcpiOsDeleteSemaphore(
    ACPI_SEMAPHORE Handle)
{
    if (!Handle)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    ExFreePoolWithTag(Handle, 'LpcA');

    return AE_OK;
}

ACPI_STATUS
AcpiOsWaitSemaphore(
    ACPI_SEMAPHORE Handle,
    UINT32 Units,
    UINT16 Timeout)
{
    PACPI_SEM Sem = Handle;
    KIRQL OldIrql;

    if (!Handle)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    KeAcquireSpinLock(&Sem->Lock, &OldIrql);

    /* Make sure we can wait if we have fewer units than we need */
    if ((Timeout == ACPI_DO_NOT_WAIT) && (Sem->CurrentUnits < Units))
    {
        /* We can't so we must bail now */
        KeReleaseSpinLock(&Sem->Lock, OldIrql);
        return AE_TIME;
    }

    /* Time to block until we get enough units */
    while (Sem->CurrentUnits < Units)
    {
        KeReleaseSpinLock(&Sem->Lock, OldIrql);
        KeWaitForSingleObject(&Sem->Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        KeAcquireSpinLock(&Sem->Lock, &OldIrql);
    }

    Sem->CurrentUnits -= Units;

    if (Sem->CurrentUnits != 0) KeSetEvent(&Sem->Event, IO_NO_INCREMENT, FALSE);

    KeReleaseSpinLock(&Sem->Lock, OldIrql);

    return AE_OK;
}

ACPI_STATUS
AcpiOsSignalSemaphore(
    ACPI_SEMAPHORE Handle,
    UINT32 Units)
{
    PACPI_SEM Sem = Handle;
    KIRQL OldIrql;

    if (!Handle)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    KeAcquireSpinLock(&Sem->Lock, &OldIrql);

    Sem->CurrentUnits += Units;
    KeSetEvent(&Sem->Event, IO_NO_INCREMENT, FALSE);

    KeReleaseSpinLock(&Sem->Lock, OldIrql);

    return AE_OK;
}

ACPI_STATUS
AcpiOsCreateLock(
    ACPI_SPINLOCK *OutHandle)
{
    PKSPIN_LOCK SpinLock;

    if (!OutHandle)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    SpinLock = ExAllocatePoolWithTag(NonPagedPool, sizeof(KSPIN_LOCK), 'LpcA');
    if (!SpinLock) return AE_NO_MEMORY;

    KeInitializeSpinLock(SpinLock);

    *OutHandle = (ACPI_SPINLOCK)SpinLock;

    return AE_OK;
}

void
AcpiOsDeleteLock(
    ACPI_SPINLOCK Handle)
{
    if (!Handle)
    {
        DPRINT1("Bad parameter\n");
        return;
    }

    ExFreePoolWithTag(Handle, 'LpcA');
}

ACPI_CPU_FLAGS
AcpiOsAcquireLock(
    ACPI_SPINLOCK Handle)
{
    KIRQL OldIrql;

    if ((OldIrql = KeGetCurrentIrql()) >= DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel((PKSPIN_LOCK)Handle);
    }
    else
    {
        KeAcquireSpinLock((PKSPIN_LOCK)Handle, &OldIrql);
    }

    return (ACPI_CPU_FLAGS)OldIrql;
}

void
AcpiOsReleaseLock(
    ACPI_SPINLOCK Handle,
    ACPI_CPU_FLAGS Flags)
{
    KIRQL OldIrql = (KIRQL)Flags;

    if (OldIrql >= DISPATCH_LEVEL)
    {
        KeReleaseSpinLockFromDpcLevel((PKSPIN_LOCK)Handle);
    }
    else
    {
        KeReleaseSpinLock((PKSPIN_LOCK)Handle, OldIrql);
    }
}

static
VOID
NTAPI
OslSciDpcRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (AcpiIrqHandler)
        (*AcpiIrqHandler)(AcpiIrqContext);

    /* Make the ISR eligible to queue us before unmasking the level line. */
    if (InterlockedExchange(&AcpiSciMasked, 0))
    {
        if (!HalEnableSystemInterrupt(AcpiSciVector,
                                      AcpiSciIrql,
                                      LevelSensitive))
        {
            /* IoConnectInterrupt may still be finishing the first connection.
             * Leave the state armed so the install path can retry once the
             * interrupt object is fully connected. */
            InterlockedExchange(&AcpiSciMasked, 1);
            DPRINT1("ACPITRACE: failed to re-enable SCI vector %lu\n",
                    AcpiSciVector);
        }
    }
}

/* Extract an I/O port from the preferred extended GAS, falling back to the
 * legacy 32-bit block address (which is I/O space by definition). Returns 0
 * when the block is absent or not in system I/O space. */
static
USHORT
OslSciBlockPort(
    const ACPI_GENERIC_ADDRESS *ExtendedBlock,
    UINT32 LegacyBlock)
{
    if ((ExtendedBlock != NULL) && (ExtendedBlock->Address != 0))
    {
        if ((ExtendedBlock->SpaceId != ACPI_ADR_SPACE_SYSTEM_IO) ||
            (ExtendedBlock->Address > 0xFFFF))
        {
            return 0;
        }
        return (USHORT)ExtendedBlock->Address;
    }
    if (LegacyBlock > 0xFFFF)
    {
        return 0;
    }
    return (USHORT)LegacyBlock;
}

static
VOID
OslSciHwInit(VOID)
{
    /* AcpiGbl_FADT is ACPICA's canonicalized copy: the extended GAS fields
     * are synthesized from the legacy ones for pre-revision-2 tables, so it
     * is safe to consume regardless of the firmware's FADT revision (the
     * raw table from AcpiGetTable is not: QEMU ships revision 1 without any
     * X fields at all). */
    const ACPI_TABLE_FADT *Fadt = &AcpiGbl_FADT;
    USHORT Port;
    UCHAR Half;

    RtlZeroMemory(&AcpiSciHw, sizeof(AcpiSciHw));

    /* PM1a is mandatory: without it we cannot evaluate the SCI condition
     * and keep the historical claim-everything behavior. */
    Port = OslSciBlockPort(&Fadt->XPm1aEventBlock, Fadt->Pm1aEventBlock);
    if ((Port == 0) || (Fadt->Pm1EventLength < 4))
    {
        DPRINT1("ACPITRACE: PM1a event block unusable; "
                "SCI ownership check disabled\n");
        return;
    }
    AcpiSciHw.Pm1aSts = Port;
    AcpiSciHw.Pm1aEn = Port + (Fadt->Pm1EventLength / 2);

    Port = OslSciBlockPort(&Fadt->XPm1bEventBlock, Fadt->Pm1bEventBlock);
    if (Port != 0)
    {
        AcpiSciHw.Pm1bSts = Port;
        AcpiSciHw.Pm1bEn = Port + (Fadt->Pm1EventLength / 2);
    }

    Port = OslSciBlockPort(&Fadt->XGpe0Block, Fadt->Gpe0Block);
    Half = Fadt->Gpe0BlockLength / 2;
    if ((Port != 0) && (Half != 0) && (Half <= 32))
    {
        AcpiSciHw.Gpe0Sts = Port;
        AcpiSciHw.Gpe0En = Port + Half;
        AcpiSciHw.Gpe0Half = Half;
    }

    Port = OslSciBlockPort(&Fadt->XGpe1Block, Fadt->Gpe1Block);
    Half = Fadt->Gpe1BlockLength / 2;
    if ((Port != 0) && (Half != 0) && (Half <= 32))
    {
        AcpiSciHw.Gpe1Sts = Port;
        AcpiSciHw.Gpe1En = Port + Half;
        AcpiSciHw.Gpe1Half = Half;
    }

    AcpiSciHw.Ready = TRUE;
    DPRINT1("ACPITRACE: SCI ownership check armed: PM1a %x/%x PM1b %x/%x "
            "GPE0 %x/%x+%u GPE1 %x/%x+%u\n",
            AcpiSciHw.Pm1aSts, AcpiSciHw.Pm1aEn,
            AcpiSciHw.Pm1bSts, AcpiSciHw.Pm1bEn,
            AcpiSciHw.Gpe0Sts, AcpiSciHw.Gpe0En, AcpiSciHw.Gpe0Half,
            AcpiSciHw.Gpe1Sts, AcpiSciHw.Gpe1En, AcpiSciHw.Gpe1Half);
}

/* The architectural SCI assert condition: any PM1 fixed-event or GPE with
 * both its status and enable bits set. Raw port reads only; this runs at
 * DIRQL where no ACPICA lock may be taken. */
static
BOOLEAN
OslSciIsAsserted(VOID)
{
    USHORT Pending;
    UCHAR i;

    Pending = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)AcpiSciHw.Pm1aSts) &
              READ_PORT_USHORT((PUSHORT)(ULONG_PTR)AcpiSciHw.Pm1aEn);
    if (AcpiSciHw.Pm1bSts != 0)
    {
        Pending |= READ_PORT_USHORT((PUSHORT)(ULONG_PTR)AcpiSciHw.Pm1bSts) &
                   READ_PORT_USHORT((PUSHORT)(ULONG_PTR)AcpiSciHw.Pm1bEn);
    }
    if (Pending != 0)
    {
        return TRUE;
    }

    for (i = 0; i < AcpiSciHw.Gpe0Half; i++)
    {
        if (READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(AcpiSciHw.Gpe0Sts + i)) &
            READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(AcpiSciHw.Gpe0En + i)))
        {
            return TRUE;
        }
    }
    for (i = 0; i < AcpiSciHw.Gpe1Half; i++)
    {
        if (READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(AcpiSciHw.Gpe1Sts + i)) &
            READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(AcpiSciHw.Gpe1En + i)))
        {
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN NTAPI
OslIsrStub(
  PKINTERRUPT Interrupt,
  PVOID ServiceContext)
{
  UNREFERENCED_PARAMETER(Interrupt);
  UNREFERENCED_PARAMETER(ServiceContext);

  AcpiSciTotalCount++;

  /* Decline interrupts that are not ours so the ISR of a device sharing
   * the level line gets its turn; claiming them would leave that device
   * asserted forever. Every 256th consecutive ghost is claimed anyway as a
   * pressure valve: if nobody on the line owns the assert (firmware bug),
   * this degrades to the historical mask-and-scan behavior at 1/256 duty
   * instead of an unthrottled DIRQL storm. */
  if ((AcpiSciHw.Ready) && (!OslSciIsAsserted()))
  {
      AcpiSciGhostCount++;
      if ((AcpiSciGhostCount & 0x3FFF) == 1)
      {
          DPRINT1("ACPITRACE: ghost SCI %lu (total %lu owned %lu)\n",
                  AcpiSciGhostCount, AcpiSciTotalCount, AcpiSciOwnedCount);
      }
      if ((++AcpiSciConsecutiveGhosts & 0xFF) != 0)
      {
          return FALSE;
      }
  }
  else
  {
      AcpiSciConsecutiveGhosts = 0;
      AcpiSciOwnedCount++;
  }

  /* SCI is level-triggered shared with the ACPI dispatcher. The ACPICA
   * handler may dispatch GPE Notify() callbacks via AcpiOsExecute, which
   * needs to allocate from non-paged pool and is therefore not legal at
   * DIRQL. Mask the asserted level line before deferring the handler; the
   * DPC clears the SCI source and then unmasks the line. */
  if (InterlockedCompareExchange(&AcpiSciMasked, 1, 0) == 0)
  {
      HalDisableSystemInterrupt(AcpiSciVector, AcpiSciIrql);
      KeInsertQueueDpc(&AcpiSciDpc, NULL, NULL);
  }

  return TRUE;
}

UINT32
AcpiOsInstallInterruptHandler (
    UINT32                  InterruptNumber,
    ACPI_OSD_HANDLER        ServiceRoutine,
    void                    *Context)
{
    ULONG Vector;
    ULONG SystemInterrupt;
    KIRQL DIrql;
    KAFFINITY Affinity;
    KAFFINITY InterruptAffinity;
    NTSTATUS Status;

    if (AcpiInterruptHandlerRegistered)
    {
        DPRINT1("Reregister interrupt attempt failed\n");
        return AE_ALREADY_EXISTS;
    }

    if (!ServiceRoutine)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    DPRINT("AcpiOsInstallInterruptHandler()\n");
    SystemInterrupt = InterruptNumber;
#ifdef _M_AMD64
    if ((SystemInterrupt < 16) &&
        !HalpTranslateIsaInterrupt(SystemInterrupt, &SystemInterrupt))
    {
        DPRINT1("ACPITRACE: SCI IRQ %lu has no supported IOAPIC route\n",
                InterruptNumber);
        return AE_ERROR;
    }
#endif

    Vector = HalGetInterruptVector(
        Internal,
        0,
        SystemInterrupt,
        SystemInterrupt,
        &DIrql,
        &Affinity);

    DPRINT("ACPITRACE: SCI IRQ %lu/GSI %lu mapped to vector %lu IRQL %u affinity %p\n",
            InterruptNumber,
            SystemInterrupt,
            Vector,
            DIrql,
            (PVOID)(ULONG_PTR)Affinity);

    /* The SCI is one physical, level-triggered line. IoConnectInterrupt
     * creates one interrupt object per processor in its affinity mask, all
     * sharing a spin lock, while the APIC HAL retargets the same I/O APIC
     * entry as each object is connected. An already-asserted SCI can then be
     * delivered on the old and new destination CPUs concurrently, leaving a
     * CPU spinning in KiInterruptDispatch before our ISR gets a chance to
     * mask the line. Route the SCI to one processor; the DPC can still run on
     * any processor selected by the scheduler. */
    InterruptAffinity = Affinity & (0 - Affinity);
    if (InterruptAffinity == 0)
    {
        DPRINT1("ACPITRACE: SCI has no usable processor affinity\n");
        return AE_ERROR;
    }
    DPRINT("ACPITRACE: SCI connection affinity restricted to %p\n",
            (PVOID)(ULONG_PTR)InterruptAffinity);

    OslSciHwInit();

    AcpiIrqNumber = SystemInterrupt;
    AcpiSciVector = Vector;
    AcpiSciIrql = DIrql;
    InterlockedExchange(&AcpiSciMasked, 0);
    AcpiIrqHandler = ServiceRoutine;
    AcpiIrqContext = Context;
    AcpiInterruptHandlerRegistered = TRUE;

    KeInitializeDpc(&AcpiSciDpc, OslSciDpcRoutine, NULL);

    Status = IoConnectInterrupt(
        &AcpiInterrupt,
        OslIsrStub,
        NULL,
        NULL,
        Vector,
        DIrql,
        DIrql,
        LevelSensitive,
        TRUE,
        InterruptAffinity,
        FALSE);

    DPRINT("ACPITRACE: SCI IoConnectInterrupt returned 0x%08lx object %p\n",
            Status,
            AcpiInterrupt);

    if (!NT_SUCCESS(Status))
    {
        AcpiInterruptHandlerRegistered = FALSE;
        AcpiIrqHandler = NULL;
        AcpiIrqContext = NULL;
        DPRINT("Could not connect to interrupt %d\n", Vector);
        return AE_ERROR;
    }

    /* An asserted SCI can run the DPC before IoConnectInterrupt returns. If
     * HAL rejected that early unmask, retry now that the connection and its
     * vector mapping are stable. */
    if (InterlockedExchange(&AcpiSciMasked, 0))
    {
        if (!HalEnableSystemInterrupt(AcpiSciVector,
                                      AcpiSciIrql,
                                      LevelSensitive))
        {
            DPRINT1("ACPITRACE: failed final SCI enable for vector %lu\n",
                    AcpiSciVector);
            IoDisconnectInterrupt(AcpiInterrupt);
            AcpiInterrupt = NULL;
            AcpiInterruptHandlerRegistered = FALSE;
            AcpiIrqHandler = NULL;
            AcpiIrqContext = NULL;
            return AE_ERROR;
        }
        DPRINT("ACPITRACE: final SCI enable retry succeeded\n");
    }
    return AE_OK;
}

ACPI_STATUS
AcpiOsRemoveInterruptHandler (
    UINT32                  InterruptNumber,
    ACPI_OSD_HANDLER        ServiceRoutine)
{
    DPRINT("AcpiOsRemoveInterruptHandler()\n");

    if (!ServiceRoutine)
    {
        DPRINT1("Bad parameter\n");
        return AE_BAD_PARAMETER;
    }

    if (AcpiInterruptHandlerRegistered)
    {
        IoDisconnectInterrupt(AcpiInterrupt);
        AcpiInterrupt = NULL;
        AcpiInterruptHandlerRegistered = FALSE;
    }
    else
    {
        DPRINT1("Trying to remove non-existing interrupt handler\n");
        return AE_NOT_EXIST;
    }

    return AE_OK;
}

ACPI_STATUS
AcpiOsReadMemory (
    ACPI_PHYSICAL_ADDRESS   Address,
    UINT64                  *Value,
    UINT32                  Width)
{
    DPRINT("AcpiOsReadMemory %p\n", Address);
    switch (Width)
    {
    case 8:
        *Value = (*(PUCHAR)(ULONG_PTR)Address);
        break;

    case 16:
        *Value = (*(PUSHORT)(ULONG_PTR)Address);
        break;

    case 32:
        *Value = (*(PULONG)(ULONG_PTR)Address);
        break;

    case 64:
        *Value = (*(PULONGLONG)(ULONG_PTR)Address);
        break;

    default:
        DPRINT1("AcpiOsReadMemory got bad width: %d\n",Width);
        return (AE_BAD_PARAMETER);
        break;
    }
    return (AE_OK);
}

ACPI_STATUS
AcpiOsWriteMemory (
    ACPI_PHYSICAL_ADDRESS   Address,
    UINT64                  Value,
    UINT32                  Width)
{
    DPRINT("AcpiOsWriteMemory %p\n", Address);
    switch (Width)
    {
    case 8:
        *(PUCHAR)(ULONG_PTR)Address = Value;
        break;

    case 16:
        *(PUSHORT)(ULONG_PTR)Address = Value;
        break;

    case 32:
        *(PULONG)(ULONG_PTR)Address = Value;
        break;

    case 64:
        *(PULONGLONG)(ULONG_PTR)Address = Value;
        break;

    default:
        DPRINT1("AcpiOsWriteMemory got bad width: %d\n",Width);
        return (AE_BAD_PARAMETER);
        break;
    }

    return (AE_OK);
}

ACPI_STATUS
AcpiOsReadPort (
    ACPI_IO_ADDRESS         Address,
    UINT32                  *Value,
    UINT32                  Width)
{
    DPRINT("AcpiOsReadPort %p, width %d\n",Address,Width);

    switch (Width)
    {
    case 8:
        *Value = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)Address);
        break;

    case 16:
        *Value = READ_PORT_USHORT((PUSHORT)(ULONG_PTR)Address);
        break;

    case 32:
        *Value = READ_PORT_ULONG((PULONG)(ULONG_PTR)Address);
        break;

    default:
        DPRINT1("AcpiOsReadPort got bad width: %d\n",Width);
        return (AE_BAD_PARAMETER);
        break;
    }
    return (AE_OK);
}

ACPI_STATUS
AcpiOsWritePort (
    ACPI_IO_ADDRESS         Address,
    UINT32                  Value,
    UINT32                  Width)
{
    DPRINT("AcpiOsWritePort %p, width %d\n",Address,Width);
    switch (Width)
    {
    case 8:
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Address, Value);
        break;

    case 16:
        WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)Address, Value);
        break;

    case 32:
        WRITE_PORT_ULONG((PULONG)(ULONG_PTR)Address, Value);
        break;

    default:
        DPRINT1("AcpiOsWritePort got bad width: %d\n",Width);
        return (AE_BAD_PARAMETER);
        break;
    }
    return (AE_OK);
}

BOOLEAN
OslIsPciDevicePresent(ULONG BusNumber, ULONG SlotNumber)
{
    UINT32 ReadLength;
    PCI_COMMON_CONFIG PciConfig;

    /* Detect device presence by reading the PCI configuration space */

    ReadLength = HalGetBusDataByOffset(PCIConfiguration,
                                       BusNumber,
                                       SlotNumber,
                                       &PciConfig,
                                       0,
                                       sizeof(PciConfig));
    if (ReadLength == 0)
    {
        DPRINT("PCI device is not present\n");
        return FALSE;
    }

    ASSERT(ReadLength >= 2);

    if (PciConfig.VendorID == PCI_INVALID_VENDORID)
    {
        DPRINT("Invalid vendor ID in PCI configuration space\n");
        return FALSE;
    }

    DPRINT("PCI device is present\n");

    return TRUE;
}

ACPI_STATUS
AcpiOsReadPciConfiguration (
    ACPI_PCI_ID             *PciId,
    UINT32                  Reg,
    UINT64                  *Value,
    UINT32                  Width)
{
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = PciId->Device;
    slot.u.bits.FunctionNumber = PciId->Function;

    DPRINT("AcpiOsReadPciConfiguration, slot=0x%X, func=0x%X\n", slot.u.AsULONG, Reg);

    if (!OslIsPciDevicePresent(PciId->Bus, slot.u.AsULONG))
        return AE_NOT_FOUND;

    /* Width is in BITS */
    HalGetBusDataByOffset(PCIConfiguration,
        PciId->Bus,
        slot.u.AsULONG,
        Value,
        Reg,
        (Width >> 3));

    return AE_OK;
}

ACPI_STATUS
AcpiOsWritePciConfiguration (
    ACPI_PCI_ID              *PciId,
    UINT32                   Reg,
    UINT64                   Value,
    UINT32                   Width)
{
    ULONG buf = Value;
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = PciId->Device;
    slot.u.bits.FunctionNumber = PciId->Function;

    DPRINT("AcpiOsWritePciConfiguration, slot=0x%x\n", slot.u.AsULONG);
    if (!OslIsPciDevicePresent(PciId->Bus, slot.u.AsULONG))
        return AE_NOT_FOUND;

    /* Width is in BITS */
    HalSetBusDataByOffset(PCIConfiguration,
        PciId->Bus,
        slot.u.AsULONG,
        &buf,
        Reg,
        (Width >> 3));

    return AE_OK;
}

void ACPI_INTERNAL_VAR_XFACE
AcpiOsPrintf (
    const char              *Fmt,
    ...)
{
    va_list                 Args;
    va_start (Args, Fmt);

    AcpiOsVprintf (Fmt, Args);

    va_end (Args);
    return;
}

void
AcpiOsVprintf (
    const char              *Fmt,
    va_list                 Args)
{
    /* Keep ACPICA bring-up diagnostics observable on the boot debugger. */
    vDbgPrintEx (-1, DPFLTR_ERROR_LEVEL, Fmt, Args);
    return;
}

void
AcpiOsRedirectOutput(
    void *Destination)
{
    /* No-op */
    DPRINT1("Output redirection not supported\n");
}

UINT64
AcpiOsGetTimer(
    void)
{
    LARGE_INTEGER CurrentTime;

    KeQuerySystemTime(&CurrentTime);
    return CurrentTime.QuadPart;
}

void
AcpiOsWaitEventsComplete(void)
{
    /*
     * Wait for all asynchronous events to complete.
     * This implementation does nothing.
     */
    return;
}

ACPI_STATUS
AcpiOsSignal (
    UINT32                  Function,
    void                    *Info)
{
    ACPI_SIGNAL_FATAL_INFO *FatalInfo = Info;

    switch (Function)
    {
    case ACPI_SIGNAL_FATAL:
        if (Info)
            DPRINT1 ("AcpiOsBreakpoint: %d %d %d ****\n", FatalInfo->Type, FatalInfo->Code, FatalInfo->Argument);
        else
            DPRINT1 ("AcpiOsBreakpoint ****\n");
        break;
    case ACPI_SIGNAL_BREAKPOINT:
        if (Info)
            DPRINT1 ("AcpiOsBreakpoint: %s ****\n", Info);
        else
            DPRINT1 ("AcpiOsBreakpoint ****\n");
        break;
    }

    ASSERT(FALSE);

    return (AE_OK);
}

ACPI_STATUS
AcpiOsEnterSleep(
    UINT8 SleepState,
    UINT32 RegaValue,
    UINT32 RegbValue)
{
    DPRINT1("Entering sleep state S%u.\n", SleepState);
    return AE_OK;
}

ACPI_STATUS
AcpiOsGetLine(
    char *Buffer,
    UINT32 BufferLength,
    UINT32 *BytesRead)
{
    DPRINT1("File reading not supported\n");
    return AE_ERROR;
}
