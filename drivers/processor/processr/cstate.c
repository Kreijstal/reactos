/*
 * PROJECT:        ReactOS Generic CPU Driver
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * FILE:           drivers/processor/processr/cstate.c
 * PURPOSE:        Processor idle state (C-state) discovery and entry
 * PROGRAMMERS:    ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include "processr.h"

#include <ntpoapi.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/*
 * We discover the platform's idle states from _CST and hand the kernel one
 * handler per usable state; the kernel decides which of them to enter and when
 * (see PopProcessorIdle in ntoskrnl/po/idle.c).  This file owns only the entry
 * sequence, because that is the part that is firmware and chipset specific.
 *
 * ACPI 6.4 section 8.1 defines the states we care about:
 *   C1  entered with a halt.  Always available, no bus master implications.
 *   C2  entered by reading the register _CST names.  The processor still
 *       maintains cache coherency, so DMA may continue while we are in it.
 *   C3  as C2, but the processor no longer snoops.  Memory written by a bus
 *       master while we sit in C3 is not reflected into our caches, so the OS
 *       must either block bus mastering (the ARB_DIS bit in the FADT's PM2
 *       control register) or know that the hardware handles coherency itself.
 */

/* ACPI 8.1.2/8.1.3: a state whose exit latency exceeds these is, by
 * definition, not usable as that state */
#define PROCESSOR_C2_MAX_LATENCY 100
#define PROCESSOR_C3_MAX_LATENCY 1000

typedef enum _PROCESSOR_IDLE_METHOD
{
    /* Interrupts on, then HLT: the pair is atomic, so no wakeup is lost */
    ProcessorIdleMethodHalt = 0,
    /* Read the register named by _CST; the read itself stops the processor */
    ProcessorIdleMethodIoRead
} PROCESSOR_IDLE_METHOD;

typedef struct _PROCESSOR_IDLE_ENTRY
{
    PROCESSOR_IDLE_METHOD Method;
    /* ACPI C-state number this implements: 1, 2 or 3 */
    ULONG Type;
    /* I/O port to read for ProcessorIdleMethodIoRead */
    USHORT Port;
    /* Worst case time to resume execution, in microseconds */
    ULONG Latency;
    /* Average power draw in this state, milliwatts, as _CST reports it */
    ULONG Power;
} PROCESSOR_IDLE_ENTRY, *PPROCESSOR_IDLE_ENTRY;

typedef struct _PROCESSOR_IDLE_DOMAIN
{
    ULONG StateCount;
    PROCESSOR_IDLE_ENTRY States[MAX_IDLE_HANDLERS];
} PROCESSOR_IDLE_DOMAIN;

/*
 * Like the P-state governor next door, idle states are a property of the
 * machine rather than of one ACPI processor devnode, so the first devnode to
 * find a usable table registers it on behalf of all of them.
 */
static PROCESSOR_IDLE_DOMAIN ProcessorIdleDomain;
static LONG ProcessorIdleClaimed = 0;

/* IDLE ENTRY *****************************************************************/

/*
 * Called by the kernel's idle loop on every idle, at APC_LEVEL or
 * DISPATCH_LEVEL, with interrupts DISABLED.  Must return with them ENABLED.
 *
 * Nothing in here may be pageable and nothing may take a lock: this is the one
 * path in the system that runs with interrupts off on an otherwise idle
 * processor, and a fault or a spin here is a silent hang.
 */
static
NTSTATUS
FASTCALL
ProcessorIdleEnter(
    _In_ ULONG_PTR Context,
    _Inout_ PPROCESSOR_IDLE_TIMES IdleTimes)
{
    PPROCESSOR_IDLE_ENTRY Entry;
    ULONG Index = (ULONG)Context;

    if (Index >= ProcessorIdleDomain.StateCount)
    {
        /* Still owe the caller interrupts even when refusing */
        _enable();
        return STATUS_INVALID_PARAMETER;
    }

    Entry = &ProcessorIdleDomain.States[Index];

    /*
     * The TSC is this HAL's performance counter (hal/halx86/apic/tsc.c reads it
     * directly), and we only ever registered states that leave it running --
     * see the invariant TSC gate in ProcessorIdleDiscover.  So it is both the
     * cheapest clock available here and a valid one across the transition.
     */
    IdleTimes->StartTime = __rdtsc();

    switch (Entry->Method)
    {
        case ProcessorIdleMethodHalt:
            /*
             * STI does not take effect until after the following instruction,
             * so STI;HLT is atomic: an interrupt that arrives in between cannot
             * slip past the halt and leave us asleep with work pending.
             */
            _enable();
            __halt();
            break;

        case ProcessorIdleMethodIoRead:
            /*
             * ACPI 8.1.3: reading the P_LVLx register stops the processor until
             * an interrupt is asserted.  Interrupts stay masked across the read
             * -- the pending interrupt is what ends the state, and we take it
             * once we enable below.  Enabling first would reintroduce exactly
             * the lost wakeup that STI;HLT exists to avoid.
             *
             * Some pre-ICH chipsets need a dummy read of the ACPI PM timer here
             * to guarantee STPCLK# has been asserted before the next
             * instruction retires.  We have no path to the FADT from a driver
             * (there is no IOCTL for it), so we do not implement that; those
             * chipsets are old enough to predate _CST entirely.
             */
            (VOID)READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)Entry->Port);
            _enable();
            break;

        default:
            _enable();
            return STATUS_NOT_IMPLEMENTED;
    }

    IdleTimes->EndTime = __rdtsc();
    return STATUS_SUCCESS;
}

/* DISCOVERY ******************************************************************/

/*
 * TRUE when an element, and the data it claims to carry, lie wholly inside the
 * package that contains it.  Both halves matter: the header has to be there
 * before its DataLength can be read at all, and the length it then declares has
 * to fit as well.  Checking only the first lets a malformed table point Data at
 * whatever follows the buffer.
 */
static
BOOLEAN
ProcessorIdleElementFits(
    _In_ PACPI_METHOD_ARGUMENT Package,
    _In_ PACPI_METHOD_ARGUMENT Element,
    _In_ ULONG Offset)
{
    if (Offset + ACPI_METHOD_ARGUMENT_LENGTH(0) > Package->DataLength)
        return FALSE;

    return (Offset + ACPI_METHOD_ARGUMENT_LENGTH_FROM_ARGUMENT(Element)) <=
           Package->DataLength;
}

/*
 * Parses one { Register, Type, Latency, Power } sub-package of _CST.
 */
static
NTSTATUS
ProcessorIdleParseEntry(
    _In_ PACPI_METHOD_ARGUMENT Package,
    _Out_ PPROCESSOR_IDLE_ENTRY Entry,
    _Out_ PUCHAR AddressSpace)
{
    PACPI_METHOD_ARGUMENT Element;
    PACPI_GENERIC_REGISTER Register;
    ULONG Offset = 0;
    ULONG Values[3];
    ULONG i;

    RtlZeroMemory(Entry, sizeof(*Entry));
    *AddressSpace = 0;

    if (Package->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        return STATUS_ACPI_INVALID_DATA;

    /* Element 0: the Register() resource template describing how to enter */
    Element = (PACPI_METHOD_ARGUMENT)Package->Data;
    if (!ProcessorIdleElementFits(Package, Element, Offset))
        return STATUS_ACPI_INVALID_DATA;

    if (Element->Type != ACPI_METHOD_ARGUMENT_BUFFER ||
        Element->DataLength < sizeof(ACPI_GENERIC_REGISTER))
    {
        return STATUS_ACPI_INVALID_DATA;
    }

    Register = (PACPI_GENERIC_REGISTER)Element->Data;
    if (Register->Tag != ACPI_GENERIC_REGISTER_TAG)
        return STATUS_ACPI_INVALID_DATA;

    *AddressSpace = Register->AddressSpaceId;

    /*
     * Only an I/O address has to be a port; a functional fixed register carries
     * a vendor value here (the MWAIT hint) that is not an address at all.
     */
    if (Register->AddressSpaceId == ACPI_ADDRESS_SPACE_SYSTEM_IO)
    {
        if (Register->Address > 0xFFFF)
            return STATUS_ACPI_INVALID_DATA;

        Entry->Port = (USHORT)Register->Address;
    }

    Offset += ACPI_METHOD_ARGUMENT_LENGTH_FROM_ARGUMENT(Element);
    Element = ACPI_METHOD_NEXT_ARGUMENT(Element);

    /* Elements 1..3: Type, Latency (microseconds), Power (milliwatts) */
    for (i = 0; i < RTL_NUMBER_OF(Values); i++)
    {
        if (!ProcessorIdleElementFits(Package, Element, Offset))
            return STATUS_ACPI_INVALID_DATA;

        if (Element->Type != ACPI_METHOD_ARGUMENT_INTEGER)
            return STATUS_ACPI_INVALID_DATA;

        Values[i] = Element->Argument;

        Offset += ACPI_METHOD_ARGUMENT_LENGTH_FROM_ARGUMENT(Element);
        Element = ACPI_METHOD_NEXT_ARGUMENT(Element);
    }

    Entry->Type = Values[0];
    Entry->Latency = Values[1];
    Entry->Power = Values[2];
    return STATUS_SUCCESS;
}

/*
 * TRUE when the TSC keeps counting at a fixed rate regardless of the processor's
 * power state (CPUID Fn8000_0007_EDX[8], "invariant TSC" on both vendors).
 *
 * This gate is not about the accuracy of our own residency measurement: this
 * HAL returns the raw TSC from KeQueryPerformanceCounter, so a C-state that
 * stops the TSC stops the system's performance counter for everyone.  Without
 * this bit, nothing deeper than a halt may be entered.
 */
static
BOOLEAN
ProcessorIdleHasInvariantTsc(VOID)
{
    int CpuInfo[4];

    __cpuid(CpuInfo, 0x80000000);
    if ((ULONG)CpuInfo[0] < 0x80000007)
        return FALSE;

    __cpuid(CpuInfo, 0x80000007);
    return (((ULONG)CpuInfo[3] & (1 << 8)) != 0);
}

/*
 * Reads _CST and keeps the states we can actually enter safely.
 */
static
NTSTATUS
ProcessorIdleDiscover(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PROCESSOR_IDLE_DOMAIN *Domain)
{
    PACPI_EVAL_OUTPUT_BUFFER Buffer = NULL;
    PACPI_METHOD_ARGUMENT Argument;
    BOOLEAN InvariantTsc;
    ULONG Declared, i;
    ULONG Found = 0;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ProcessorAcpiEvaluateMethod(DeviceObject, "_CST", &Buffer);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Processr: _CST not available (Status 0x%08lx)\n", Status);
        return Status;
    }

    /*
     * _CST returns a package whose first element is the number of C-state
     * sub-packages that follow it, so there must be at least two elements for
     * even one state to be described.
     */
    if (Buffer->Count < 2)
    {
        DPRINT1("Processr: _CST describes no states\n");
        Status = STATUS_ACPI_INVALID_DATA;
        goto Cleanup;
    }

    Argument = Buffer->Argument;
    if (!ProcessorAcpiArgumentFits(Buffer, Argument) ||
        Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER)
    {
        DPRINT1("Processr: _CST does not begin with a state count\n");
        Status = STATUS_ACPI_INVALID_DATA;
        goto Cleanup;
    }

    Declared = Argument->Argument;
    if (Declared > Buffer->Count - 1)
    {
        /* Trust what is actually there over what the count claims */
        Declared = Buffer->Count - 1;
    }

    InvariantTsc = ProcessorIdleHasInvariantTsc();

    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    for (i = 0; i < Declared; i++)
    {
        PROCESSOR_IDLE_ENTRY Entry;
        UCHAR AddressSpace = 0;

        if (!ProcessorAcpiArgumentFits(Buffer, Argument))
        {
            DPRINT1("Processr: _CST entry %lu runs past the evaluation buffer\n", i);
            break;
        }

        Status = ProcessorIdleParseEntry(Argument, &Entry, &AddressSpace);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Processr: malformed _CST entry %lu\n", i);
            break;
        }

        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);

        if (Found >= MAX_IDLE_HANDLERS)
            break;

        /*
         * The kernel walks its handler table as a ladder, one step at a time,
         * so the states must arrive in order and without gaps: entry N has to
         * describe C(N+1).  Firmware that lists them out of order is not
         * something to guess at.
         */
        if (Entry.Type != Found + 1)
        {
            DPRINT1("Processr: _CST entry %lu declares C%lu, expected C%lu - "
                    "ignoring it and everything below it\n",
                    i, Entry.Type, Found + 1);
            break;
        }

        if (Entry.Type == 1)
        {
            /*
             * C1 is a halt on every x86 part; ACPI describes it as a functional
             * fixed register precisely because there is nothing to read.
             */
            Entry.Method = ProcessorIdleMethodHalt;
            Domain->States[Found++] = Entry;
            continue;
        }

        if (!InvariantTsc)
        {
            DPRINT1("Processr: C%lu ignored, this processor's TSC does not run "
                    "at a constant rate across idle states\n", Entry.Type);
            break;
        }

        if (Entry.Type == 2 && Entry.Latency > PROCESSOR_C2_MAX_LATENCY)
        {
            DPRINT1("Processr: C2 ignored, exit latency %luus exceeds the %uus "
                    "ACPI allows for C2\n", Entry.Latency, PROCESSOR_C2_MAX_LATENCY);
            break;
        }

        if (Entry.Type == 3 && Entry.Latency > PROCESSOR_C3_MAX_LATENCY)
        {
            DPRINT1("Processr: C3 ignored, exit latency %luus exceeds the %uus "
                    "ACPI allows for C3\n", Entry.Latency, PROCESSOR_C3_MAX_LATENCY);
            break;
        }

        if (AddressSpace != ACPI_ADDRESS_SPACE_SYSTEM_IO)
        {
            /*
             * The other encoding _CST uses for C2 and deeper is a functional
             * fixed register meaning "native C-state", entered with MONITOR /
             * MWAIT and a vendor hint.  We have no MWAIT intrinsic in the tree
             * (sdk/include/vcruntime provides __halt but not __mwait), and
             * entering it with interrupts masked additionally requires the
             * break-on-interrupt extension reported by CPUID leaf 5.  Guessing
             * at either is a hang, so report it and stop here.
             */
            DPRINT1("Processr: C%lu ignored, it is a native (MWAIT) state and "
                    "only ACPI I/O entry is implemented\n", Entry.Type);
            break;
        }

        if (Entry.Type >= 3)
        {
            /*
             * In C3 the processor stops snooping, so a bus master writing
             * memory behind our back leaves our caches stale.  ACPI's remedy is
             * to assert ARB_DIS in the FADT's PM2 control register for the
             * duration, or to detect bus master activity through BM_STS and
             * stay out of C3 while it is happening.  Both live in the FADT,
             * which a driver has no way to read -- acpi.sys exposes no IOCTL
             * for it.  Entering C3 anyway risks silent memory corruption, not
             * merely a stall, so we stop at C2 until that interface exists.
             */
            DPRINT1("Processr: C3 (I/O entry at port 0x%04x, %luus) discovered "
                    "but not used: no way to reach the FADT's ARB_DIS/BM_STS "
                    "from a driver, and C3 without them can corrupt memory\n",
                    Entry.Port, Entry.Latency);
            break;
        }

        if (Entry.Port == 0)
        {
            DPRINT1("Processr: C%lu ignored, I/O entry with no port\n", Entry.Type);
            break;
        }

        Entry.Method = ProcessorIdleMethodIoRead;
        Domain->States[Found++] = Entry;
    }

    if (Found == 0)
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }

    Domain->StateCount = Found;
    Status = STATUS_SUCCESS;

Cleanup:
    if (Buffer != NULL)
        ExFreePoolWithTag(Buffer, PROCESSOR_TAG);

    return Status;
}

/* PUBLIC FUNCTIONS ***********************************************************/

VOID
ProcessorIdleInitialize(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PPROCESSOR_STATE_HANDLER2 Handler;
    PROCESSOR_IDLE_DOMAIN *Domain = &ProcessorIdleDomain;
    ULONG Length, i;
    NTSTATUS Status;

    PAGED_CODE();

    /* One machine, one idle table: the first devnode here does the work */
    if (InterlockedCompareExchange(&ProcessorIdleClaimed, 1, 0) != 0)
        return;

    Status = ProcessorIdleDiscover(DeviceObject, Domain);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Processr: no usable idle states (Status 0x%08lx), leaving the "
                "kernel on its unconditional halt\n", Status);
        return;
    }

    /*
     * A table that offers nothing but C1 would replace the kernel's halt with
     * our own halt plus bookkeeping.  That buys no power and perturbs the idle
     * path of every machine that gets here, so leave it alone.
     */
    if (Domain->StateCount < 2)
    {
        DPRINT1("Processr: only C1 is usable, leaving the kernel on its "
                "unconditional halt\n");
        Domain->StateCount = 0;
        return;
    }

    Length = sizeof(PROCESSOR_STATE_HANDLER2);
    Handler = ExAllocatePoolZero(NonPagedPool, Length, PROCESSOR_TAG);
    if (Handler == NULL)
    {
        Domain->StateCount = 0;
        return;
    }

    Handler->NumIdleHandlers = Domain->StateCount;
    for (i = 0; i < Domain->StateCount; i++)
    {
        Handler->IdleHandler[i].HardwareLatency = Domain->States[i].Latency;
        Handler->IdleHandler[i].Handler = ProcessorIdleEnter;
    }

    /*
     * NumPerfStates stays zero: the P-state side of this driver drives _PSS
     * and _PCT directly and must remain the only thing changing frequency.
     */
    Status = ZwPowerInformation(ProcessorStateHandler2, Handler, Length, NULL, 0);

    ExFreePoolWithTag(Handler, PROCESSOR_TAG);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Processr: the kernel refused our idle states (Status 0x%08lx)\n",
                Status);
        Domain->StateCount = 0;
        return;
    }

    /*
     * From here the kernel holds a pointer to ProcessorIdleEnter and calls it
     * from the idle loop of every processor, with interrupts disabled and no
     * lock we could take against a teardown.  There is no safe moment to revoke
     * that: a processor can be inside the handler at any instant, and unloading
     * the image under it would jump every idle CPU into freed memory.
     *
     * A driver with no Unload routine is one the I/O manager will not unload,
     * so dropping it here is what makes the registration permanent.  The
     * cleanup it skipped (ProcessorPerfStop) only tore down the P-state
     * governor, which is meant to run for the life of the system in any case.
     */
    DeviceObject->DriverObject->DriverUnload = NULL;

    for (i = 0; i < Domain->StateCount; i++)
    {
        DPRINT1("Processr: C%lu enabled (%s, %luus exit latency, %lumW)\n",
                Domain->States[i].Type,
                Domain->States[i].Method == ProcessorIdleMethodHalt ?
                    "halt" : "ACPI I/O read",
                Domain->States[i].Latency,
                Domain->States[i].Power);
    }
}

/* EOF */
