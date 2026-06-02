/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Implementation of GetLogicalProcessorInformationEx (Win7+)
 */

#include "k32_vista.h"
#include <ndk/exfuncs.h>

#define NDEBUG
#include <debug.h>

/*
 * Windows 7+ exposes SystemLogicalProcessorAndGroupInformation via
 * NtQuerySystemInformationEx, but ReactOS's ntdll only stubs that. We degrade
 * by querying the legacy SYSTEM_LOGICAL_PROCESSOR_INFORMATION and translating
 * each entry into its SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX shape with
 * Group = 0 (single-group system).
 */

static
ULONG
TranslatedEntrySize(
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP Relationship)
{
    switch (Relationship)
    {
        case RelationProcessorCore:
        case RelationProcessorPackage:
            /* PROCESSOR_RELATIONSHIP with one GROUP_AFFINITY entry. */
            return FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor.GroupMask)
                   + sizeof(GROUP_AFFINITY);

        case RelationCache:
            return FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache)
                   + sizeof(CACHE_RELATIONSHIP);

        case RelationNumaNode:
            return FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, NumaNode)
                   + sizeof(NUMA_NODE_RELATIONSHIP);

        case RelationGroup:
            return FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group)
                   + FIELD_OFFSET(GROUP_RELATIONSHIP, GroupInfo)
                   + sizeof(PROCESSOR_GROUP_INFO);

        default:
            return 0;
    }
}

static
BOOL
ShouldEmit(
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP Filter,
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP Entry)
{
    if (Filter == RelationAll)
        return TRUE;
    return Filter == Entry;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetLogicalProcessorInformationEx(
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
    _Out_writes_bytes_to_opt_(*ReturnedLength, *ReturnedLength) PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Buffer,
    _Inout_ PDWORD ReturnedLength)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Legacy = NULL;
    ULONG LegacyBytes = 0;
    NTSTATUS Status;
    ULONG i, count, Required = 0;
    PUCHAR Out;
    DWORD AvailableLength;
    BOOL Success = FALSE;
    BOOL HaveAnyCore = FALSE;
    KAFFINITY UnionMask = 0;

    if (ReturnedLength == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Query required size for the legacy data. */
    Status = NtQuerySystemInformation(SystemLogicalProcessorInformation,
                                      NULL,
                                      0,
                                      &LegacyBytes);
    if (Status != STATUS_INFO_LENGTH_MISMATCH &&
        Status != STATUS_BUFFER_TOO_SMALL &&
        !NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }
    if (LegacyBytes == 0)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }

    Legacy = RtlAllocateHeap(RtlGetProcessHeap(), 0, LegacyBytes);
    if (Legacy == NULL)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    Status = NtQuerySystemInformation(SystemLogicalProcessorInformation,
                                      Legacy,
                                      LegacyBytes,
                                      &LegacyBytes);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        goto Cleanup;
    }

    count = LegacyBytes / sizeof(*Legacy);

    /* First pass: compute required output size. */
    for (i = 0; i < count; ++i)
    {
        LOGICAL_PROCESSOR_RELATIONSHIP rel = Legacy[i].Relationship;

        if (rel == RelationProcessorCore)
        {
            HaveAnyCore = TRUE;
            UnionMask |= (KAFFINITY)Legacy[i].ProcessorMask;
        }

        if (!ShouldEmit(RelationshipType, rel))
            continue;

        Required += TranslatedEntrySize(rel);
    }

    /* Synthesise a RelationGroup entry when asked for it (or for "All"),
     * since the legacy info-class never returns one. */
    if (HaveAnyCore &&
        (RelationshipType == RelationGroup || RelationshipType == RelationAll))
    {
        Required += FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group)
                    + FIELD_OFFSET(GROUP_RELATIONSHIP, GroupInfo)
                    + sizeof(PROCESSOR_GROUP_INFO);
    }

    AvailableLength = *ReturnedLength;
    *ReturnedLength = Required;

    if (Buffer == NULL || AvailableLength < Required)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        goto Cleanup;
    }

    /* Second pass: emit translated entries. */
    Out = (PUCHAR)Buffer;
    for (i = 0; i < count; ++i)
    {
        LOGICAL_PROCESSOR_RELATIONSHIP rel = Legacy[i].Relationship;
        ULONG entrySize;
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX dst;

        if (!ShouldEmit(RelationshipType, rel))
            continue;

        entrySize = TranslatedEntrySize(rel);
        dst = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Out;
        RtlZeroMemory(dst, entrySize);

        dst->Relationship = rel;
        dst->Size = entrySize;

        switch (rel)
        {
            case RelationProcessorCore:
                dst->Processor.Flags = Legacy[i].ProcessorCore.Flags;
                dst->Processor.EfficiencyClass = 0;
                dst->Processor.GroupCount = 1;
                dst->Processor.GroupMask[0].Mask = (KAFFINITY)Legacy[i].ProcessorMask;
                dst->Processor.GroupMask[0].Group = 0;
                break;

            case RelationProcessorPackage:
                dst->Processor.Flags = 0;
                dst->Processor.EfficiencyClass = 0;
                dst->Processor.GroupCount = 1;
                dst->Processor.GroupMask[0].Mask = (KAFFINITY)Legacy[i].ProcessorMask;
                dst->Processor.GroupMask[0].Group = 0;
                break;

            case RelationCache:
                dst->Cache.Level = Legacy[i].Cache.Level;
                dst->Cache.Associativity = Legacy[i].Cache.Associativity;
                dst->Cache.LineSize = Legacy[i].Cache.LineSize;
                dst->Cache.CacheSize = Legacy[i].Cache.Size;
                dst->Cache.Type = Legacy[i].Cache.Type;
                dst->Cache.GroupMask.Mask = (KAFFINITY)Legacy[i].ProcessorMask;
                dst->Cache.GroupMask.Group = 0;
                break;

            case RelationNumaNode:
                dst->NumaNode.NodeNumber = Legacy[i].NumaNode.NodeNumber;
                dst->NumaNode.GroupMask.Mask = (KAFFINITY)Legacy[i].ProcessorMask;
                dst->NumaNode.GroupMask.Group = 0;
                break;

            default:
                /* RelationGroup / future relationships aren't produced by the
                 * legacy info-class - skip silently. */
                break;
        }

        Out += entrySize;
    }

    /* Append the synthesised RelationGroup entry. */
    if (HaveAnyCore &&
        (RelationshipType == RelationGroup || RelationshipType == RelationAll))
    {
        ULONG entrySize = FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group)
                          + FIELD_OFFSET(GROUP_RELATIONSHIP, GroupInfo)
                          + sizeof(PROCESSOR_GROUP_INFO);
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX dst =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Out;
        ULONG popCount;
        KAFFINITY mask;

        RtlZeroMemory(dst, entrySize);
        dst->Relationship = RelationGroup;
        dst->Size = entrySize;
        dst->Group.MaximumGroupCount = 1;
        dst->Group.ActiveGroupCount = 1;
        dst->Group.GroupInfo[0].MaximumProcessorCount = 0;

        popCount = 0;
        mask = UnionMask;
        while (mask != 0)
        {
            if (mask & 1)
                ++popCount;
            mask >>= 1;
        }

        dst->Group.GroupInfo[0].ActiveProcessorCount = (UCHAR)popCount;
        dst->Group.GroupInfo[0].MaximumProcessorCount = (UCHAR)popCount;
        dst->Group.GroupInfo[0].ActiveProcessorMask = UnionMask;
    }

    Success = TRUE;

Cleanup:
    if (Legacy != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Legacy);
    return Success;
}
