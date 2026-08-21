/*
 * PROJECT:        ReactOS Generic CPU Driver
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * FILE:           drivers/processor/processr/pstate.c
 * PURPOSE:        Processor performance state (P-state) discovery and governor
 * PROGRAMMERS:    ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include "processr.h"

#include <acpiioct.h>
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Governor tuning. All thresholds are percentages of a single sample period. */
#define PROCESSOR_SAMPLE_INTERVAL_MS 1000
#define PROCESSOR_BUSY_UP_PERCENT    75
#define PROCESSOR_BUSY_DOWN_PERCENT  35
#define PROCESSOR_DOWN_SAMPLES       2

/* One line per this many 100ns units (10 seconds) at most, see ProcessorPerfLogTransition */
#define PROCESSOR_LOG_INTERVAL (10 * 1000 * 1000 * 10LL)

/* AMD family 10h and later software P-state registers (BKDG, "P-state Control") */
#define MSR_AMD_PSTATE_CURRENT_LIMIT 0xC0010061
#define MSR_AMD_PSTATE_CONTROL       0xC0010062
#define MSR_AMD_PSTATE_STATUS        0xC0010063
#define MSR_AMD_PSTATE_DEF_BASE      0xC0010064
#define AMD_PSTATE_DEF_COUNT         8

/* MSRC001_006[4:B] [P-state [7:0]] */
#define AMD_PSTATE_DEF_ENABLED       0x8000000000000000ULL
#define AMD_PSTATE_DEF_CPUFID_MASK   0x3F
#define AMD_PSTATE_DEF_CPUDID_SHIFT  6
#define AMD_PSTATE_DEF_CPUDID_MASK   0x07

/* MSRC001_0061 [P-state Current Limit] */
#define AMD_PSTATE_LIMIT_CUR_MASK    0x07
#define AMD_PSTATE_LIMIT_MAX_SHIFT   4
#define AMD_PSTATE_LIMIT_MAX_MASK    0x07

/* Intel Enhanced SpeedStep registers */
#define MSR_IA32_PERF_STATUS         0x198
#define MSR_IA32_PERF_CTL            0x199

typedef enum _PROCESSOR_PERF_INTERFACE
{
    ProcessorPerfInterfaceNone = 0,
    /* _PSS control values written through the FFixedHW register named by _PCT */
    ProcessorPerfInterfaceAcpi,
    /* Native AMD software P-state MSRs, used when the firmware exposes no _PSS */
    ProcessorPerfInterfaceAmdMsr
} PROCESSOR_PERF_INTERFACE;

typedef struct _PROCESSOR_CPU_STATE
{
    /* Raw PRCB tick counters, compared as unsigned deltas so wrapping is fine */
    ULONG LastIdleCount;
    ULONG LastTotalCount;
    ULONG CurrentState;
    ULONG IdleSamples;
    BOOLEAN Sampled;
} PROCESSOR_CPU_STATE, *PPROCESSOR_CPU_STATE;

typedef struct _PROCESSOR_PERF_DOMAIN
{
    BOOLEAN Started;
    PDEVICE_OBJECT OwnerDevice;
    PROCESSOR_PERF_INTERFACE Interface;

    ULONG StateCount;
    /* _PPC: index of the fastest state the platform currently permits */
    ULONG PlatformLimit;
    ULONG ControlRegister;
    ULONG StatusRegister;
    PROCESSR_PSTATE States[PROCESSOR_MAX_PERF_STATES];

    ULONG ProcessorCount;
    PPROCESSOR_CPU_STATE Cpu;

    KTIMER Timer;
    KEVENT StopEvent;
    PKTHREAD Thread;
    ULONGLONG LastLogTime;
} PROCESSOR_PERF_DOMAIN;

/*
 * P-state management is a property of the machine, not of a single ACPI
 * processor devnode: every logical processor gets its own devnode but they all
 * share one performance table on the homogeneous packages we support. The first
 * devnode that discovers a usable table therefore owns the governor for all of
 * them.
 */
static PROCESSOR_PERF_DOMAIN ProcessorPerfDomain;
static LONG ProcessorPerfClaimed = 0;

/* CPU IDENTIFICATION *********************************************************/

typedef struct _PROCESSOR_CPU_IDENT
{
    BOOLEAN IsAmd;
    BOOLEAN IsIntel;
    ULONG Family;
    ULONG Model;
    BOOLEAN HasAmdHwPstate;
    BOOLEAN HasIntelEst;
} PROCESSOR_CPU_IDENT, *PPROCESSOR_CPU_IDENT;

static
VOID
ProcessorPerfIdentify(
    _Out_ PPROCESSOR_CPU_IDENT Ident)
{
    int CpuInfo[4];
    ULONG BaseFamily, ExtFamily, BaseModel, ExtModel, MaxExtLeaf;

    RtlZeroMemory(Ident, sizeof(*Ident));

    __cpuid(CpuInfo, 0);
    Ident->IsAmd = (CpuInfo[1] == 0x68747541 /* Auth */ &&
                    CpuInfo[3] == 0x69746E65 /* enti */ &&
                    CpuInfo[2] == 0x444D4163 /* cAMD */);
    Ident->IsIntel = (CpuInfo[1] == 0x756E6547 /* Genu */ &&
                      CpuInfo[3] == 0x49656E69 /* ineI */ &&
                      CpuInfo[2] == 0x6C65746E /* ntel */);

    __cpuid(CpuInfo, 1);
    BaseFamily = ((ULONG)CpuInfo[0] >> 8) & 0x0F;
    ExtFamily = ((ULONG)CpuInfo[0] >> 20) & 0xFF;
    BaseModel = ((ULONG)CpuInfo[0] >> 4) & 0x0F;
    ExtModel = ((ULONG)CpuInfo[0] >> 16) & 0x0F;

    if (BaseFamily == 0x0F)
    {
        Ident->Family = BaseFamily + ExtFamily;
        Ident->Model = BaseModel | (ExtModel << 4);
    }
    else
    {
        Ident->Family = BaseFamily;
        Ident->Model = BaseModel;
    }

    Ident->HasIntelEst = Ident->IsIntel && (((ULONG)CpuInfo[2] & (1 << 7)) != 0);

    /*
     * CPUID Fn8000_0007_EDX[7] (HwPstate) is the architectural statement that
     * MSRC001_006[4:B] and MSRC001_0062/0063 exist. Without it we must not
     * touch those MSRs: a hypervisor that reports an AMD family without
     * implementing them would #GP the rdmsr.
     */
    __cpuid(CpuInfo, 0x80000000);
    MaxExtLeaf = (ULONG)CpuInfo[0];
    if (Ident->IsAmd && MaxExtLeaf >= 0x80000007)
    {
        __cpuid(CpuInfo, 0x80000007);
        Ident->HasAmdHwPstate = (((ULONG)CpuInfo[3] & (1 << 7)) != 0);
    }
}

/* ACPI EVALUATION ************************************************************/

static
NTSTATUS
ProcessorPerfSendIoctl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PIO_STATUS_BLOCK IoStatusBlock)
{
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    PAGED_CODE();

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD,
                                        DeviceObject,
                                        InputBuffer,
                                        InputBufferLength,
                                        OutputBuffer,
                                        OutputBufferLength,
                                        FALSE,
                                        &Event,
                                        IoStatusBlock);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatusBlock->Status;
    }

    return Status;
}

/*
 * Evaluates a control method on the ACPI stack below us and returns the
 * (pool allocated) output buffer on success. The caller frees it.
 */
NTSTATUS
ProcessorAcpiEvaluateMethod(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PCSTR MethodName,
    _Outptr_result_nullonfailure_ PACPI_EVAL_OUTPUT_BUFFER *ReturnBuffer)
{
    ACPI_EVAL_INPUT_BUFFER InputBuffer;
    PACPI_EVAL_OUTPUT_BUFFER OutputBuffer;
    IO_STATUS_BLOCK IoStatusBlock;
    ULONG OutputLength = 512;
    NTSTATUS Status;

    PAGED_CODE();

    *ReturnBuffer = NULL;

    RtlZeroMemory(&InputBuffer, sizeof(InputBuffer));
    InputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(InputBuffer.MethodName, MethodName, sizeof(InputBuffer.MethodName));

    while (TRUE)
    {
        OutputBuffer = ExAllocatePoolZero(NonPagedPool, OutputLength, PROCESSOR_TAG);
        if (OutputBuffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
        Status = ProcessorPerfSendIoctl(DeviceObject,
                                        &InputBuffer,
                                        sizeof(InputBuffer),
                                        OutputBuffer,
                                        OutputLength,
                                        &IoStatusBlock);
        if (Status != STATUS_BUFFER_OVERFLOW && Status != STATUS_BUFFER_TOO_SMALL)
            break;

        /* Retry once with the size ACPI asked for */
        if (IoStatusBlock.Information <= OutputLength ||
            IoStatusBlock.Information > 64 * 1024)
        {
            ExFreePoolWithTag(OutputBuffer, PROCESSOR_TAG);
            return STATUS_BUFFER_TOO_SMALL;
        }

        OutputLength = (ULONG)IoStatusBlock.Information;
        ExFreePoolWithTag(OutputBuffer, PROCESSOR_TAG);
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(OutputBuffer, PROCESSOR_TAG);
        return Status;
    }

    if (OutputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE ||
        OutputBuffer->Length > OutputLength ||
        OutputBuffer->Count == 0)
    {
        ExFreePoolWithTag(OutputBuffer, PROCESSOR_TAG);
        return STATUS_ACPI_INVALID_DATA;
    }

    *ReturnBuffer = OutputBuffer;
    return STATUS_SUCCESS;
}

/*
 * The ACPI method arguments are variable length and self describing, so every
 * step through them has to be bounded by the output buffer ACPI actually filled.
 */
BOOLEAN
ProcessorAcpiArgumentFits(
    _In_ PACPI_EVAL_OUTPUT_BUFFER Buffer,
    _In_ PACPI_METHOD_ARGUMENT Argument)
{
    ULONG_PTR Offset;

    if ((PUCHAR)Argument < (PUCHAR)Buffer->Argument)
        return FALSE;

    Offset = (PUCHAR)Argument - (PUCHAR)Buffer;
    if (Offset + ACPI_METHOD_ARGUMENT_LENGTH(0) > Buffer->Length)
        return FALSE;

    return (Offset + ACPI_METHOD_ARGUMENT_LENGTH_FROM_ARGUMENT(Argument)) <= Buffer->Length;
}

/*
 * Reads the Nth integer out of a nested package argument, bounded by the
 * package's own DataLength so that a malformed table cannot walk off the end.
 */
static
NTSTATUS
ProcessorPerfGetPackageIntegers(
    _In_ PACPI_METHOD_ARGUMENT Package,
    _Out_writes_(Count) PULONG Values,
    _In_ ULONG Count)
{
    PACPI_METHOD_ARGUMENT Element;
    ULONG i, Offset = 0;

    if (Package->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        return STATUS_ACPI_INVALID_DATA;

    Element = (PACPI_METHOD_ARGUMENT)Package->Data;
    for (i = 0; i < Count; i++)
    {
        ULONG ElementLength;

        if (Offset + ACPI_METHOD_ARGUMENT_LENGTH(0) > Package->DataLength)
            return STATUS_ACPI_INVALID_DATA;

        ElementLength = ACPI_METHOD_ARGUMENT_LENGTH_FROM_ARGUMENT(Element);
        if (Offset + ElementLength > Package->DataLength)
            return STATUS_ACPI_INVALID_DATA;

        if (Element->Type != ACPI_METHOD_ARGUMENT_INTEGER)
            return STATUS_ACPI_INVALID_DATA;

        Values[i] = Element->Argument;

        Offset += ElementLength;
        Element = ACPI_METHOD_NEXT_ARGUMENT(Element);
    }

    return STATUS_SUCCESS;
}

/* DISCOVERY ******************************************************************/

/*
 * _PCT returns two Register() resource templates: the control register first,
 * then the status register. We only implement the FFixedHW (MSR) flavour, which
 * is what every P-state capable x86 part ships; SystemIO and SystemMemory
 * P-state registers (very old Intel mobile parts) are rejected.
 */
static
NTSTATUS
ProcessorPerfParseControlRegisters(
    _In_ PACPI_EVAL_OUTPUT_BUFFER Buffer,
    _In_ PPROCESSOR_CPU_IDENT Ident,
    _Out_ PULONG ControlRegister,
    _Out_ PULONG StatusRegister)
{
    PACPI_METHOD_ARGUMENT Argument;
    PACPI_GENERIC_REGISTER Register;
    ULONG i, Address[2] = { 0, 0 };

    if (Buffer->Count < 2)
        return STATUS_ACPI_INVALID_DATA;

    Argument = Buffer->Argument;
    for (i = 0; i < 2; i++)
    {
        if (!ProcessorAcpiArgumentFits(Buffer, Argument))
            return STATUS_ACPI_INVALID_DATA;

        if (Argument->Type != ACPI_METHOD_ARGUMENT_BUFFER ||
            Argument->DataLength < sizeof(ACPI_GENERIC_REGISTER))
        {
            return STATUS_ACPI_INVALID_DATA;
        }

        Register = (PACPI_GENERIC_REGISTER)Argument->Data;
        if (Register->Tag != ACPI_GENERIC_REGISTER_TAG)
            return STATUS_ACPI_INVALID_DATA;

        if (Register->AddressSpaceId != ACPI_ADDRESS_SPACE_FIXED_HW)
        {
            DPRINT1("Processr: _PCT register %lu uses unsupported address space 0x%02x\n",
                    i, Register->AddressSpaceId);
            return STATUS_NOT_SUPPORTED;
        }

        Address[i] = (ULONG)Register->Address;
        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    }

    /*
     * For FFixedHW the address field is vendor defined and firmware commonly
     * leaves it zero, meaning "the architectural register for this vendor".
     */
    if (Address[0] == 0 || Address[1] == 0)
    {
        if (Ident->IsAmd && Ident->HasAmdHwPstate)
        {
            Address[0] = MSR_AMD_PSTATE_CONTROL;
            Address[1] = MSR_AMD_PSTATE_STATUS;
        }
        else if (Ident->HasIntelEst)
        {
            Address[0] = MSR_IA32_PERF_CTL;
            Address[1] = MSR_IA32_PERF_STATUS;
        }
        else
        {
            DPRINT1("Processr: _PCT names a functional fixed register but this "
                    "CPU advertises no P-state MSR interface\n");
            return STATUS_NOT_SUPPORTED;
        }
    }

    *ControlRegister = Address[0];
    *StatusRegister = Address[1];
    return STATUS_SUCCESS;
}

static
NTSTATUS
ProcessorPerfDiscoverAcpi(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PPROCESSOR_CPU_IDENT Ident,
    _Inout_ PROCESSOR_PERF_DOMAIN *Domain)
{
    PACPI_EVAL_OUTPUT_BUFFER Buffer = NULL;
    PACPI_METHOD_ARGUMENT Argument;
    ULONG i, Count;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ProcessorAcpiEvaluateMethod(DeviceObject, "_PSS", &Buffer);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Processr: _PSS not available (Status 0x%08lx)\n", Status);
        return Status;
    }

    Count = min(Buffer->Count, PROCESSOR_MAX_PERF_STATES);

    Argument = Buffer->Argument;
    for (i = 0; i < Count; i++)
    {
        ULONG Values[6];

        if (!ProcessorAcpiArgumentFits(Buffer, Argument))
        {
            DPRINT1("Processr: _PSS entry %lu runs past the evaluation buffer\n", i);
            Status = STATUS_ACPI_INVALID_DATA;
            goto Failure;
        }

        Status = ProcessorPerfGetPackageIntegers(Argument, Values, RTL_NUMBER_OF(Values));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Processr: malformed _PSS entry %lu\n", i);
            goto Failure;
        }

        /* A zero core frequency is meaningless and would break the governor */
        if (Values[0] == 0 || Values[0] > 100000)
        {
            DPRINT1("Processr: _PSS entry %lu has invalid frequency %lu\n", i, Values[0]);
            Status = STATUS_ACPI_INVALID_DATA;
            goto Failure;
        }

        /* ACPI requires _PSS to be sorted from the fastest to the slowest state */
        if (i > 0 && Values[0] >= Domain->States[i - 1].CoreFrequency)
        {
            DPRINT1("Processr: _PSS is not ordered by descending frequency\n");
            Status = STATUS_ACPI_INVALID_DATA;
            goto Failure;
        }

        Domain->States[i].CoreFrequency = Values[0];
        Domain->States[i].Power = Values[1];
        Domain->States[i].TransitionLatency = Values[2];
        Domain->States[i].BusMasterLatency = Values[3];
        Domain->States[i].Control = Values[4];
        Domain->States[i].Status = Values[5];

        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    }

    ExFreePoolWithTag(Buffer, PROCESSOR_TAG);
    Buffer = NULL;

    /* Report what the firmware offered even if the control interface turns out
     * to be one we cannot drive */
    DPRINT1("Processr: _PSS lists %lu states, P0 %lu MHz, P%lu %lu MHz\n",
            Count,
            Domain->States[0].CoreFrequency,
            Count - 1,
            Domain->States[Count - 1].CoreFrequency);

    Status = ProcessorAcpiEvaluateMethod(DeviceObject, "_PCT", &Buffer);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Processr: _PCT not available (Status 0x%08lx)\n", Status);
        return Status;
    }

    Status = ProcessorPerfParseControlRegisters(Buffer,
                                                Ident,
                                                &Domain->ControlRegister,
                                                &Domain->StatusRegister);
    if (!NT_SUCCESS(Status))
        goto Failure;

    ExFreePoolWithTag(Buffer, PROCESSOR_TAG);

    Domain->StateCount = Count;
    Domain->Interface = ProcessorPerfInterfaceAcpi;
    return STATUS_SUCCESS;

Failure:
    if (Buffer != NULL)
        ExFreePoolWithTag(Buffer, PROCESSOR_TAG);

    return Status;
}

/*
 * Native fallback for firmware that exposes no _PSS. Only AMD family 15h is
 * enabled here: the P-state definition encoding below (CpuFid/CpuDid) is
 * family specific and the caller has already checked CPUID for HwPstate.
 */
static
NTSTATUS
ProcessorPerfDiscoverAmdMsr(
    _In_ PPROCESSOR_CPU_IDENT Ident,
    _Inout_ PROCESSOR_PERF_DOMAIN *Domain)
{
    ULONG i, Count = 0;
    ULONGLONG Limit;

    if (!Ident->IsAmd || Ident->Family != 0x15 || !Ident->HasAmdHwPstate)
        return STATUS_NOT_SUPPORTED;

    for (i = 0; i < AMD_PSTATE_DEF_COUNT; i++)
    {
        ULONGLONG Definition;
        ULONG CpuFid, CpuDid, Frequency;

        Definition = __readmsr(MSR_AMD_PSTATE_DEF_BASE + i);
        if ((Definition & AMD_PSTATE_DEF_ENABLED) == 0)
            break;

        CpuFid = (ULONG)(Definition & AMD_PSTATE_DEF_CPUFID_MASK);
        CpuDid = (ULONG)((Definition >> AMD_PSTATE_DEF_CPUDID_SHIFT) &
                         AMD_PSTATE_DEF_CPUDID_MASK);

        /* Family 15h: CoreCOF = 100MHz * (CpuFid + 10h) / 2^CpuDid */
        Frequency = (100 * (CpuFid + 0x10)) >> CpuDid;
        if (Frequency == 0)
            break;

        if (i > 0 && Frequency >= Domain->States[i - 1].CoreFrequency)
            break;

        Domain->States[i].CoreFrequency = Frequency;
        Domain->States[i].Control = i;
        Domain->States[i].Status = i;
        Count = i + 1;
    }

    if (Count < 2)
    {
        DPRINT1("Processr: AMD P-state MSRs describe %lu usable states, ignoring\n", Count);
        return STATUS_NOT_SUPPORTED;
    }

    Domain->StateCount = Count;
    Domain->ControlRegister = MSR_AMD_PSTATE_CONTROL;
    Domain->StatusRegister = MSR_AMD_PSTATE_STATUS;
    Domain->Interface = ProcessorPerfInterfaceAmdMsr;

    /*
     * MSRC001_0061 constrains the range further than the definition registers
     * do; PstateMaxVal is the slowest and CurPstateLimit the fastest allowed.
     */
    Limit = __readmsr(MSR_AMD_PSTATE_CURRENT_LIMIT);
    Domain->PlatformLimit = (ULONG)(Limit & AMD_PSTATE_LIMIT_CUR_MASK);
    if (Domain->PlatformLimit >= Count)
        Domain->PlatformLimit = 0;

    i = (ULONG)((Limit >> AMD_PSTATE_LIMIT_MAX_SHIFT) & AMD_PSTATE_LIMIT_MAX_MASK);
    if (i + 1 < Count && i + 1 > Domain->PlatformLimit)
        Domain->StateCount = i + 1;

    return STATUS_SUCCESS;
}

static
VOID
ProcessorPerfApplyPlatformLimit(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PROCESSOR_PERF_DOMAIN *Domain)
{
    PACPI_EVAL_OUTPUT_BUFFER Buffer;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ProcessorAcpiEvaluateMethod(DeviceObject, "_PPC", &Buffer);
    if (!NT_SUCCESS(Status))
        return;

    if (ProcessorAcpiArgumentFits(Buffer, Buffer->Argument) &&
        Buffer->Argument->Type == ACPI_METHOD_ARGUMENT_INTEGER &&
        Buffer->Argument->Argument < Domain->StateCount)
    {
        Domain->PlatformLimit = Buffer->Argument->Argument;
        DPRINT1("Processr: _PPC limits the fastest usable state to P%lu\n",
                Domain->PlatformLimit);
    }

    ExFreePoolWithTag(Buffer, PROCESSOR_TAG);
}

/* TRANSITIONS ****************************************************************/

static
ULONG
ProcessorPerfClamp(
    _In_ PROCESSOR_PERF_DOMAIN *Domain,
    _In_ ULONG Index)
{
    if (Index >= Domain->StateCount)
        Index = Domain->StateCount - 1;

    if (Index < Domain->PlatformLimit)
        Index = Domain->PlatformLimit;

    return Index;
}

/* Must run on the processor the transition is meant for */
static
VOID
ProcessorPerfWriteControl(
    _In_ PROCESSOR_PERF_DOMAIN *Domain,
    _In_ ULONG Index)
{
    __writemsr(Domain->ControlRegister, Domain->States[Index].Control);
}

/*
 * Probes the control interface once before the governor starts using it. The
 * register comes from firmware (_PCT) or from the AMD P-state definition MSRs,
 * and a virtual machine can describe a register its CPU model does not
 * implement; the resulting #GP would take the whole system down, so find out
 * here instead of in the sampling path.
 */
static
BOOLEAN
ProcessorPerfProbeControl(
    _In_ PROCESSOR_PERF_DOMAIN *Domain,
    _In_ ULONG Index)
{
    volatile BOOLEAN Usable = TRUE;

    _SEH2_TRY
    {
        __writemsr(Domain->ControlRegister, Domain->States[Index].Control);
        __readmsr(Domain->StatusRegister);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Usable = FALSE;
    }
    _SEH2_END;

    return Usable;
}

/*
 * The control register is per-core, so the write has to retire on the core it
 * is meant for. Raising the affinity of the governor thread is enough and lets
 * us keep the whole governor at PASSIVE_LEVEL.
 */
static
VOID
ProcessorPerfApplyState(
    _In_ PROCESSOR_PERF_DOMAIN *Domain,
    _In_ ULONG Processor,
    _In_ ULONG Index)
{
    KeSetSystemAffinityThread((KAFFINITY)1 << Processor);
    ProcessorPerfWriteControl(Domain, Index);
    KeRevertToUserAffinityThread();
}

static
VOID
ProcessorPerfLogTransition(
    _Inout_ PROCESSOR_PERF_DOMAIN *Domain,
    _In_ ULONG Processor,
    _In_ ULONG Index,
    _In_ ULONG Busy)
{
    ULONGLONG Now = KeQueryInterruptTime();

    /* The governor runs on every core every second; never log unthrottled */
    if (Now - Domain->LastLogTime < PROCESSOR_LOG_INTERVAL)
        return;

    Domain->LastLogTime = Now;
    DPRINT1("Processr: CPU%lu -> P%lu (%lu MHz) at %lu%% busy\n",
            Processor, Index, Domain->States[Index].CoreFrequency, Busy);
}

/* GOVERNOR *******************************************************************/

/*
 * Samples the PRCB idle accounting of every processor and steps its P-state.
 * Both the sample and the resulting control register write have to happen on
 * the processor in question, so the whole body runs under its affinity.
 */
static
VOID
ProcessorPerfSample(
    _Inout_ PROCESSOR_PERF_DOMAIN *Domain)
{
    ULONG i;

    for (i = 0; i < Domain->ProcessorCount; i++)
    {
        PPROCESSOR_CPU_STATE Cpu = &Domain->Cpu[i];
        ULONG IdleCount, TotalCount, Index;
        ULONG IdleDelta, TotalDelta;
        ULONG Busy, NewState;

        KeSetSystemAffinityThread((KAFFINITY)1 << i);

        ExGetCurrentProcessorCounts(&IdleCount, &TotalCount, &Index);

        /* Never attribute a sample, or a transition, to the wrong core */
        if (Index != i)
        {
            KeRevertToUserAffinityThread();
            continue;
        }

        if (!Cpu->Sampled)
        {
            Cpu->LastIdleCount = IdleCount;
            Cpu->LastTotalCount = TotalCount;
            Cpu->Sampled = TRUE;
            KeRevertToUserAffinityThread();
            continue;
        }

        IdleDelta = IdleCount - Cpu->LastIdleCount;
        TotalDelta = TotalCount - Cpu->LastTotalCount;
        Cpu->LastIdleCount = IdleCount;
        Cpu->LastTotalCount = TotalCount;

        if (TotalDelta == 0)
        {
            KeRevertToUserAffinityThread();
            continue;
        }

        if (IdleDelta > TotalDelta)
            IdleDelta = TotalDelta;

        Busy = 100 - (ULONG)(((ULONGLONG)IdleDelta * 100) / TotalDelta);

        NewState = Cpu->CurrentState;
        if (Busy >= PROCESSOR_BUSY_UP_PERCENT)
        {
            Cpu->IdleSamples = 0;
            if (NewState > Domain->PlatformLimit)
                NewState--;
        }
        else if (Busy <= PROCESSOR_BUSY_DOWN_PERCENT)
        {
            Cpu->IdleSamples++;
            if (Cpu->IdleSamples >= PROCESSOR_DOWN_SAMPLES)
            {
                Cpu->IdleSamples = 0;
                if (NewState + 1 < Domain->StateCount)
                    NewState++;
            }
        }
        else
        {
            Cpu->IdleSamples = 0;
        }

        NewState = ProcessorPerfClamp(Domain, NewState);
        if (NewState != Cpu->CurrentState)
        {
            ProcessorPerfWriteControl(Domain, NewState);
            Cpu->CurrentState = NewState;

            KeRevertToUserAffinityThread();
            ProcessorPerfLogTransition(Domain, i, NewState, Busy);
            continue;
        }

        KeRevertToUserAffinityThread();
    }
}

static
VOID
NTAPI
ProcessorPerfGovernorThread(
    _In_ PVOID Context)
{
    PROCESSOR_PERF_DOMAIN *Domain = Context;
    PVOID Objects[2];
    LARGE_INTEGER DueTime;
    ULONGLONG Status = 0;
    BOOLEAN Usable;
    ULONG i;

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    KeSetSystemAffinityThread((KAFFINITY)1);
    Usable = ProcessorPerfProbeControl(Domain, Domain->StateCount - 1);
    if (Usable)
        Status = __readmsr(Domain->StatusRegister);
    KeRevertToUserAffinityThread();

    if (!Usable)
    {
        DPRINT1("Processr: control register 0x%08lx is not implemented by this "
                "processor, demand based scaling disabled\n",
                Domain->ControlRegister);
        Domain->Interface = ProcessorPerfInterfaceNone;
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }

    DPRINT1("Processr: governor started at P%lu, CPU0 status register reads 0x%I64x\n",
            Domain->StateCount - 1, Status);

    /*
     * Start every core at the slowest state: the firmware hands the system over
     * in its fastest one and on a thermally constrained package that is exactly
     * the state we must not stay in while nobody is asking for the frequency.
     */
    for (i = 0; i < Domain->ProcessorCount; i++)
    {
        Domain->Cpu[i].CurrentState = Domain->StateCount - 1;
        ProcessorPerfApplyState(Domain, i, Domain->Cpu[i].CurrentState);
    }

    /* Unsigned wrap is intended: it makes the first transition loggable */
    Domain->LastLogTime = KeQueryInterruptTime() - PROCESSOR_LOG_INTERVAL;

    Objects[0] = &Domain->StopEvent;
    Objects[1] = &Domain->Timer;

    DueTime.QuadPart = -10000LL * PROCESSOR_SAMPLE_INTERVAL_MS;
    KeSetTimerEx(&Domain->Timer, DueTime, PROCESSOR_SAMPLE_INTERVAL_MS, NULL);

    while (TRUE)
    {
        NTSTATUS WaitStatus = KeWaitForMultipleObjects(RTL_NUMBER_OF(Objects),
                                                       Objects,
                                                       WaitAny,
                                                       Executive,
                                                       KernelMode,
                                                       FALSE,
                                                       NULL,
                                                       NULL);
        if (WaitStatus != STATUS_WAIT_1)
            break;

        ProcessorPerfSample(Domain);
    }

    KeCancelTimer(&Domain->Timer);

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* INITIALIZATION *************************************************************/

static
VOID
ProcessorPerfReport(
    _In_ PROCESSOR_PERF_DOMAIN *Domain)
{
    ULONG i;

    DPRINT1("Processr: %lu performance states discovered via %s, %lu processors\n",
            Domain->StateCount,
            (Domain->Interface == ProcessorPerfInterfaceAcpi) ? "ACPI _PSS/_PCT"
                                                              : "AMD P-state MSRs",
            Domain->ProcessorCount);
    DPRINT1("Processr: control register 0x%08lx, status register 0x%08lx\n",
            Domain->ControlRegister, Domain->StatusRegister);

    for (i = 0; i < Domain->StateCount; i++)
    {
        DPRINT1("Processr:   P%lu: %lu MHz, %lu mW, %lu us, control 0x%lx\n",
                i,
                Domain->States[i].CoreFrequency,
                Domain->States[i].Power,
                Domain->States[i].TransitionLatency,
                Domain->States[i].Control);
    }
}

VOID
ProcessorPerfInitialize(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PROCESSOR_PERF_DOMAIN *Domain = &ProcessorPerfDomain;
    PROCESSOR_CPU_IDENT Ident;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE ThreadHandle;
    NTSTATUS Status;

    PAGED_CODE();

    /* Only the first processor devnode brings the domain up */
    if (InterlockedCompareExchange(&ProcessorPerfClaimed, 1, 0) != 0)
        return;

    ProcessorPerfIdentify(&Ident);
    DPRINT1("Processr: %s family 0x%lx model 0x%lx, HwPstate %u, EST %u\n",
            Ident.IsAmd ? "AuthenticAMD" : (Ident.IsIntel ? "GenuineIntel" : "unknown"),
            Ident.Family, Ident.Model, Ident.HasAmdHwPstate, Ident.HasIntelEst);

    Status = ProcessorPerfDiscoverAcpi(DeviceExtension->LowerDevice, &Ident, Domain);
    if (!NT_SUCCESS(Status))
    {
        RtlZeroMemory(Domain->States, sizeof(Domain->States));
        Domain->StateCount = 0;
        Domain->PlatformLimit = 0;

        Status = ProcessorPerfDiscoverAmdMsr(&Ident, Domain);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Processr: no usable P-state interface, demand based scaling disabled\n");
            Domain->Interface = ProcessorPerfInterfaceNone;
            return;
        }
    }
    else
    {
        ProcessorPerfApplyPlatformLimit(DeviceExtension->LowerDevice, Domain);
    }

    if (Domain->StateCount < 2)
    {
        DPRINT1("Processr: only %lu performance state, nothing to govern\n",
                Domain->StateCount);
        Domain->Interface = ProcessorPerfInterfaceNone;
        return;
    }

    Domain->PlatformLimit = ProcessorPerfClamp(Domain, Domain->PlatformLimit);
    Domain->ProcessorCount = (ULONG)KeNumberProcessors;

    /*
     * KeSetSystemAffinityThread only addresses the processors of one group, and
     * the affinity mask below is a single ULONG_PTR.
     */
    if (Domain->ProcessorCount > sizeof(KAFFINITY) * 8)
        Domain->ProcessorCount = sizeof(KAFFINITY) * 8;

    ProcessorPerfReport(Domain);

    Domain->Cpu = ExAllocatePoolZero(NonPagedPool,
                                     Domain->ProcessorCount * sizeof(PROCESSOR_CPU_STATE),
                                     PROCESSOR_TAG);
    if (Domain->Cpu == NULL)
        goto Failure;

    KeInitializeTimerEx(&Domain->Timer, SynchronizationTimer);
    KeInitializeEvent(&Domain->StopEvent, NotificationEvent, FALSE);

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&ThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  &ObjectAttributes,
                                  NULL,
                                  NULL,
                                  ProcessorPerfGovernorThread,
                                  Domain);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Processr: PsCreateSystemThread() failed (Status 0x%08lx)\n", Status);
        goto Failure;
    }

    Status = ObReferenceObjectByHandle(ThreadHandle,
                                       THREAD_ALL_ACCESS,
                                       *PsThreadType,
                                       KernelMode,
                                       (PVOID *)&Domain->Thread,
                                       NULL);
    ZwClose(ThreadHandle);
    if (!NT_SUCCESS(Status))
    {
        /* The thread is running and owns the domain now, so let it be */
        DPRINT1("Processr: ObReferenceObjectByHandle() failed (Status 0x%08lx)\n", Status);
        Domain->Thread = NULL;
    }

    Domain->OwnerDevice = DeviceObject;
    Domain->Started = TRUE;
    return;

Failure:
    if (Domain->Cpu != NULL)
    {
        ExFreePoolWithTag(Domain->Cpu, PROCESSOR_TAG);
        Domain->Cpu = NULL;
    }

    Domain->Interface = ProcessorPerfInterfaceNone;
}

VOID
ProcessorPerfStop(
    _In_opt_ PDEVICE_OBJECT DeviceObject)
{
    PROCESSOR_PERF_DOMAIN *Domain = &ProcessorPerfDomain;

    PAGED_CODE();

    if (!Domain->Started)
        return;

    if (DeviceObject != NULL && DeviceObject != Domain->OwnerDevice)
        return;

    Domain->Started = FALSE;
    Domain->OwnerDevice = NULL;
    Domain->Interface = ProcessorPerfInterfaceNone;
    KeSetEvent(&Domain->StopEvent, IO_NO_INCREMENT, FALSE);

    /*
     * Without a referenced thread object there is no way to know when the
     * governor stops touching its buffers, so leave them allocated.
     */
    if (Domain->Thread == NULL)
        return;

    KeWaitForSingleObject(Domain->Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(Domain->Thread);
    Domain->Thread = NULL;

    ExFreePoolWithTag(Domain->Cpu, PROCESSOR_TAG);
    Domain->Cpu = NULL;

    InterlockedExchange(&ProcessorPerfClaimed, 0);
}

/* EOF */
