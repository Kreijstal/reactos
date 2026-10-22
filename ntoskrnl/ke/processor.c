/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Portable processor related routines
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

KAFFINITY KeActiveProcessors = 0;

/* Number of processors */
#if (NTDDI_VERSION >= NTDDI_VISTA)
volatile CCHAR KeNumberProcessors = 0;
#else
CCHAR KeNumberProcessors = 0;
#endif

#ifdef CONFIG_SMP

/* Theoretical maximum number of processors that can be handled.
 * Set once at run-time. Returned by KeQueryMaximumProcessorCount(). */
ULONG KeMaximumProcessors = MAXIMUM_PROCESSORS;

/* Maximum number of logical processors that can be started
 * (including dynamically) at run-time. If 0: do not perform checks. */
ULONG KeNumprocSpecified = 0;

/* Maximum number of logical processors that can be started
 * at boot-time. If 0: do not perform checks. */
ULONG KeBootprocSpecified = 0;

#endif // CONFIG_SMP

/* FUNCTIONS *****************************************************************/

static ULONG
KiCountAffinityBits(
    _In_ KAFFINITY Affinity)
{
    ULONG Count = 0;

    while (Affinity != 0)
    {
        Count += Affinity & 1;
        Affinity >>= 1;
    }

    return Count;
}

KAFFINITY
NTAPI
KeQueryActiveProcessors(VOID)
{
    return KeActiveProcessors;
}

ULONG
NTAPI
KeQueryMaximumProcessorCount(VOID)
{
#ifdef CONFIG_SMP
    return KeMaximumProcessors;
#else
    return 1;
#endif
}

ULONG
NTAPI
KeQueryActiveProcessorCountEx(
    _In_ USHORT GroupNumber)
{
    if ((GroupNumber != 0) && (GroupNumber != ALL_PROCESSOR_GROUPS))
    {
        return 0;
    }

    return KiCountAffinityBits(KeActiveProcessors);
}

ULONG
NTAPI
KeQueryMaximumProcessorCountEx(
    _In_ USHORT GroupNumber)
{
    if ((GroupNumber != 0) && (GroupNumber != ALL_PROCESSOR_GROUPS))
    {
        return 0;
    }

    return KeQueryMaximumProcessorCount();
}

USHORT
NTAPI
KeQueryActiveGroupCount(VOID)
{
    return 1;
}

USHORT
NTAPI
KeQueryMaximumGroupCount(VOID)
{
    return 1;
}

KAFFINITY
NTAPI
KeQueryGroupAffinity(
    _In_ USHORT GroupNumber)
{
    if (GroupNumber != 0)
    {
        return 0;
    }

    return KeActiveProcessors;
}

/**
 * Retrieves the number of the current processor.
 *
 * \param ProcessorNumber Pointer to a PROCESSOR_NUMBER structure that receives the processor number.
 *
 * \return NTSTATUS The status of the operation.
 */
NTSTATUS
NTAPI
NtGetCurrentProcessorNumberEx(
    _Out_ PPROCESSOR_NUMBER ProcessorNumber)
{
    _SEH2_TRY
    {
        ProbeForWrite(ProcessorNumber, sizeof(PROCESSOR_NUMBER), __alignof(PROCESSOR_NUMBER));
        ProcessorNumber->Group = 0; // TODO: Support processor groups
        ProcessorNumber->Number = (UCHAR)KeGetCurrentProcessorNumber();
        ProcessorNumber->Reserved = 0;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        return _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

#ifndef _M_ARM64
/* ARM64 implements this in ke/arm64/irql.c */
ULONG
NTAPI
KeGetCurrentProcessorNumberEx(
    _Out_opt_ PPROCESSOR_NUMBER ProcNumber)
{
    ULONG Number = KeGetCurrentProcessorNumber();

    if (ProcNumber)
    {
        /* ReactOS only supports a single processor group */
        ProcNumber->Group = 0;
        ProcNumber->Number = (UCHAR)Number;
        ProcNumber->Reserved = 0;
    }

    /* With a single group, the system-wide index is the number */
    return Number;
}
#endif

NTSTATUS
NTAPI
KeGetProcessorNumberFromIndex(
    _In_ ULONG ProcIndex,
    _Out_ PPROCESSOR_NUMBER ProcNumber)
{
    if (ProcIndex >= KeQueryMaximumProcessorCount())
        return STATUS_INVALID_PARAMETER;

    ProcNumber->Group = 0;
    ProcNumber->Number = (UCHAR)ProcIndex;
    ProcNumber->Reserved = 0;
    return STATUS_SUCCESS;
}

ULONG
NTAPI
KeGetProcessorIndexFromNumber(
    _In_ PPROCESSOR_NUMBER ProcNumber)
{
    if ((ProcNumber->Group != 0) ||
        (ProcNumber->Number >= KeQueryMaximumProcessorCount()))
    {
        return INVALID_PROCESSOR_INDEX;
    }

    return ProcNumber->Number;
}

/* Returns the set of logical processors sharing a core with Processor */
static KAFFINITY
KiGetCoreProcessorSet(
    _In_ ULONG Processor)
{
#ifndef _M_ARM
    PKPRCB Prcb = KiProcessorBlock[Processor];

    if (Prcb && Prcb->MultiThreadProcessorSet)
        return (KAFFINITY)Prcb->MultiThreadProcessorSet & KeActiveProcessors;
#endif
    return AFFINITY_MASK(Processor);
}

static BOOLEAN
KiIsCoreMaster(
    _In_ ULONG Processor)
{
#ifndef _M_ARM
    PKPRCB Prcb = KiProcessorBlock[Processor];

    if (Prcb && Prcb->MultiThreadSetMaster)
        return (Prcb->MultiThreadSetMaster == Prcb);
#endif
    return TRUE;
}

/*
 * Describes the topology the way Windows does, for the single processor group
 * ReactOS supports: one record per core (SMT siblings share a record), one
 * package, one NUMA node and one group. No cache records are returned.
 */
NTSTATUS
NTAPI
KeQueryLogicalProcessorRelationship(
    _In_opt_ PPROCESSOR_NUMBER ProcessorNumber,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
    _Out_writes_bytes_opt_(*Length) PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Information,
    _Inout_ PULONG Length)
{
    const ULONG ProcessorSize = FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                             Processor.GroupMask) + sizeof(GROUP_AFFINITY);
    const ULONG NumaSize = FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                        NumaNode) + sizeof(NUMA_NODE_RELATIONSHIP);
    const ULONG GroupSize = FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group) +
                            FIELD_OFFSET(GROUP_RELATIONSHIP, GroupInfo) +
                            sizeof(PROCESSOR_GROUP_INFO);
    KAFFINITY Active = KeActiveProcessors, Filter = Active, Mask;
    ULONG Pass, i, Required = 0, Offset;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Entry;

    if (!Length)
        return STATUS_INVALID_PARAMETER;

    if ((RelationshipType != RelationProcessorCore) &&
        (RelationshipType != RelationNumaNode) &&
        (RelationshipType != RelationCache) &&
        (RelationshipType != RelationProcessorPackage) &&
        (RelationshipType != RelationGroup) &&
        (RelationshipType != RelationAll))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (ProcessorNumber)
    {
        if ((ProcessorNumber->Group != 0) ||
            (ProcessorNumber->Number >= MAXIMUM_PROCESSORS) ||
            !(Active & AFFINITY_MASK(ProcessorNumber->Number)))
        {
            return STATUS_INVALID_PARAMETER;
        }
        Filter = AFFINITY_MASK(ProcessorNumber->Number);
    }

    /* Pass 0 computes the size, pass 1 fills the buffer */
    for (Pass = 0; Pass < 2; Pass++)
    {
        Offset = 0;

        if ((RelationshipType == RelationProcessorCore) || (RelationshipType == RelationAll))
        {
            for (i = 0; i < MAXIMUM_PROCESSORS; i++)
            {
                if (!(Active & AFFINITY_MASK(i)) || !KiIsCoreMaster(i))
                    continue;

                Mask = KiGetCoreProcessorSet(i);
                if (!(Mask & Filter))
                    continue;

                if (Pass)
                {
                    Entry = (PVOID)((PUCHAR)Information + Offset);
                    RtlZeroMemory(Entry, ProcessorSize);
                    Entry->Relationship = RelationProcessorCore;
                    Entry->Size = ProcessorSize;
                    Entry->Processor.Flags = (Mask & (Mask - 1)) ? LTP_PC_SMT : 0;
                    Entry->Processor.GroupCount = 1;
                    Entry->Processor.GroupMask[0].Mask = Mask;
                    Entry->Processor.GroupMask[0].Group = 0;
                }
                Offset += ProcessorSize;
            }
        }

        if ((RelationshipType == RelationNumaNode) || (RelationshipType == RelationAll))
        {
            if (Pass)
            {
                Entry = (PVOID)((PUCHAR)Information + Offset);
                RtlZeroMemory(Entry, NumaSize);
                Entry->Relationship = RelationNumaNode;
                Entry->Size = NumaSize;
                Entry->NumaNode.NodeNumber = 0;
                Entry->NumaNode.GroupMask.Mask = Active;
                Entry->NumaNode.GroupMask.Group = 0;
            }
            Offset += NumaSize;
        }

        if ((RelationshipType == RelationProcessorPackage) || (RelationshipType == RelationAll))
        {
            if (Pass)
            {
                Entry = (PVOID)((PUCHAR)Information + Offset);
                RtlZeroMemory(Entry, ProcessorSize);
                Entry->Relationship = RelationProcessorPackage;
                Entry->Size = ProcessorSize;
                Entry->Processor.GroupCount = 1;
                Entry->Processor.GroupMask[0].Mask = Active;
                Entry->Processor.GroupMask[0].Group = 0;
            }
            Offset += ProcessorSize;
        }

        if ((RelationshipType == RelationGroup) || (RelationshipType == RelationAll))
        {
            if (Pass)
            {
                Entry = (PVOID)((PUCHAR)Information + Offset);
                RtlZeroMemory(Entry, GroupSize);
                Entry->Relationship = RelationGroup;
                Entry->Size = GroupSize;
                Entry->Group.MaximumGroupCount = 1;
                Entry->Group.ActiveGroupCount = 1;
                Entry->Group.GroupInfo[0].MaximumProcessorCount = (UCHAR)KeQueryMaximumProcessorCount();
                Entry->Group.GroupInfo[0].ActiveProcessorCount = (UCHAR)KiCountAffinityBits(Active);
                Entry->Group.GroupInfo[0].ActiveProcessorMask = Active;
            }
            Offset += GroupSize;
        }

        if (Pass == 0)
        {
            Required = Offset;
            if (!Information || (*Length < Required))
            {
                *Length = Required;
                return STATUS_INFO_LENGTH_MISMATCH;
            }
        }
    }

    *Length = Required;
    return STATUS_SUCCESS;
}
