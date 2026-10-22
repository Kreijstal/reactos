/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite processor group routines test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

static volatile LONG DpcProcessor;
static KEVENT DpcEvent;

static
VOID
NTAPI
TargetDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    InterlockedExchange(&DpcProcessor, (LONG)KeGetCurrentProcessorNumberEx(NULL));
    KeSetEvent(&DpcEvent, IO_NO_INCREMENT, FALSE);
}

static
VOID
TestNumbers(VOID)
{
    PROCESSOR_NUMBER Number;
    ULONG Index, Count, MaxCount;
    KIRQL OldIrql;
    NTSTATUS Status;

    Count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    MaxCount = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    ok(Count >= 1 && Count <= MaxCount, "Count %lu, maximum %lu\n", Count, MaxCount);

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    RtlFillMemory(&Number, sizeof(Number), 0xCC);
    Index = KeGetCurrentProcessorNumberEx(&Number);
    ok_eq_ulong(Index, KeGetCurrentProcessorNumber());
    ok_eq_uint(Number.Group, 0);
    ok_eq_uint(Number.Number, Index);
    ok_eq_uint(Number.Reserved, 0);
    ok_eq_ulong(KeGetCurrentProcessorNumberEx(NULL), Index);
    KeLowerIrql(OldIrql);

    for (Index = 0; Index < Count; Index++)
    {
        RtlFillMemory(&Number, sizeof(Number), 0xCC);
        Status = KeGetProcessorNumberFromIndex(Index, &Number);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_uint(Number.Group, 0);
        ok_eq_uint(Number.Number, Index);
        ok_eq_ulong(KeGetProcessorIndexFromNumber(&Number), Index);
    }

    Status = KeGetProcessorNumberFromIndex(MaxCount, &Number);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Number.Group = 1;
    Number.Number = 0;
    Number.Reserved = 0;
    ok_eq_ulong(KeGetProcessorIndexFromNumber(&Number), INVALID_PROCESSOR_INDEX);
    Number.Group = 0;
    Number.Number = (UCHAR)MaxCount;
    ok_eq_ulong(KeGetProcessorIndexFromNumber(&Number), INVALID_PROCESSOR_INDEX);

    ok(KeQueryGroupAffinity(0) == KeQueryActiveProcessors(),
       "Group 0 affinity %Ix, active %Ix\n", KeQueryGroupAffinity(0), KeQueryActiveProcessors());
    ok(KeQueryGroupAffinity(KeQueryActiveGroupCount()) == 0, "Nonexistent group has an affinity\n");
    ok_eq_ulong(RtlNumberOfSetBitsUlongPtr(KeQueryGroupAffinity(0)), Count);
}

static
VOID
TestBitCount(VOID)
{
    ok_eq_ulong(RtlNumberOfSetBitsUlongPtr(0), 0UL);
    ok_eq_ulong(RtlNumberOfSetBitsUlongPtr(1), 1UL);
    ok_eq_ulong(RtlNumberOfSetBitsUlongPtr(0x80000001), 2UL);
    ok_eq_ulong(RtlNumberOfSetBitsUlongPtr(0xF0F0), 8UL);
    ok_eq_ulong(RtlNumberOfSetBitsUlongPtr((ULONG_PTR)-1), (ULONG)(sizeof(ULONG_PTR) * 8));
#ifdef _WIN64
    ok_eq_ulong(RtlNumberOfSetBitsUlongPtr(0x8000000000000000ULL), 1UL);
#endif
}

static
VOID
TestRelationship(VOID)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Info, Entry;
    PROCESSOR_NUMBER Number;
    ULONG Length, Offset, Cores = 0, Packages = 0, Nodes = 0, Groups = 0;
    KAFFINITY CoreUnion = 0, Active = KeQueryActiveProcessors();
    union
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Info;
        UCHAR Bytes[sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) + 4 * sizeof(GROUP_AFFINITY)];
    } Core;
    NTSTATUS Status;

    /* Size query */
    Length = 0;
    Status = KeQueryLogicalProcessorRelationship(NULL, RelationAll, NULL, &Length);
    ok_eq_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Length != 0, "No length returned\n");
    if (skip(Length != 0, "No length\n"))
        return;

    Info = ExAllocatePoolZero(NonPagedPool, Length, 'gPmK');
    if (skip(Info != NULL, "Out of memory\n"))
        return;

    Status = KeQueryLogicalProcessorRelationship(NULL, RelationAll, Info, &Length);
    ok_eq_hex(Status, STATUS_SUCCESS);
    for (Offset = 0; NT_SUCCESS(Status) && Offset < Length; Offset += Entry->Size)
    {
        Entry = (PVOID)((PUCHAR)Info + Offset);
        if (Entry->Size == 0)
        {
            ok(0, "Zero sized entry at %lu\n", Offset);
            break;
        }
        switch (Entry->Relationship)
        {
            case RelationProcessorCore:
                Cores++;
                ok_eq_uint(Entry->Processor.GroupCount, 1);
                ok((Entry->Processor.GroupMask[0].Mask & CoreUnion) == 0, "Cores overlap\n");
                CoreUnion |= Entry->Processor.GroupMask[0].Mask;
                break;
            case RelationProcessorPackage:
                Packages++;
                break;
            case RelationNumaNode:
                Nodes++;
                break;
            case RelationGroup:
                Groups++;
                ok_eq_uint(Entry->Group.ActiveGroupCount, 1);
                ok(Entry->Group.GroupInfo[0].ActiveProcessorMask == Active, "Group mask %Ix, active %Ix\n",
                   Entry->Group.GroupInfo[0].ActiveProcessorMask, Active);
                ok_eq_uint(Entry->Group.GroupInfo[0].ActiveProcessorCount,
                           KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS));
                break;
            default:
                break;
        }
    }
    ok(Cores >= 1, "No cores\n");
    ok(CoreUnion == Active, "Cores cover %Ix, active %Ix\n", CoreUnion, Active);
    ok(Packages >= 1, "No packages\n");
    ok(Nodes >= 1, "No NUMA nodes\n");
    ok_eq_ulong(Groups, 1UL);
    ExFreePoolWithTag(Info, 'gPmK');

    /* What ovpn-dco-win does: the core of one processor, in a fixed buffer */
    KeGetProcessorNumberFromIndex(0, &Number);
    Length = sizeof(Core);
    Status = KeQueryLogicalProcessorRelationship(&Number, RelationProcessorCore, &Core.Info, &Length);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_uint(Core.Info.Relationship, RelationProcessorCore);
        ok(Core.Info.Size <= Length, "Entry size %lu, length %lu\n", Core.Info.Size, Length);
        ok_eq_uint(Core.Info.Processor.GroupCount, 1);
        ok(Core.Info.Processor.GroupMask[0].Mask & AFFINITY_MASK(0),
           "Core mask %Ix lacks processor 0\n", Core.Info.Processor.GroupMask[0].Mask);
    }

    Number.Group = 1;
    Length = sizeof(Core);
    Status = KeQueryLogicalProcessorRelationship(&Number, RelationProcessorCore, &Core.Info, &Length);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
}

static
VOID
TestAffinity(VOID)
{
    GROUP_AFFINITY Affinity, Previous, Nested;
    PROCESSOR_NUMBER Number;
    ULONG Index, Count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    LARGE_INTEGER Timeout;
    KDPC Dpc;
    NTSTATUS Status;

    for (Index = 0; Index < Count; Index++)
    {
        RtlZeroMemory(&Affinity, sizeof(Affinity));
        Affinity.Mask = AFFINITY_MASK(Index);
        RtlFillMemory(&Previous, sizeof(Previous), 0xCC);
        KeSetSystemGroupAffinityThread(&Affinity, &Previous);
        /* The thread had no system affinity, so reverting goes back to user */
        ok(Previous.Mask == 0, "Previous mask %Ix\n", Previous.Mask);
        ok_eq_uint(Previous.Group, 0);
        ok_eq_ulong(KeGetCurrentProcessorNumberEx(&Number), Index);

        /* Nesting returns the outer system affinity */
        KeSetSystemGroupAffinityThread(&Affinity, &Nested);
        ok(Nested.Mask == AFFINITY_MASK(Index), "Nested previous mask %Ix\n", Nested.Mask);
        KeRevertToUserGroupAffinityThread(&Nested);
        ok_eq_ulong(KeGetCurrentProcessorNumberEx(NULL), Index);

        KeRevertToUserGroupAffinityThread(&Previous);
    }

    /* Targeted DPCs run where they were sent */
    KeInitializeEvent(&DpcEvent, SynchronizationEvent, FALSE);
    KeInitializeDpc(&Dpc, TargetDpcRoutine, NULL);
    for (Index = 0; Index < Count; Index++)
    {
        KeGetProcessorNumberFromIndex(Index, &Number);
        Status = KeSetTargetProcessorDpcEx(&Dpc, &Number);
        ok_eq_hex(Status, STATUS_SUCCESS);
        DpcProcessor = -1;
        KeInsertQueueDpc(&Dpc, NULL, NULL);
        Timeout.QuadPart = -10LL * 1000 * 1000 * 5;
        Status = KeWaitForSingleObject(&DpcEvent, Executive, KernelMode, FALSE, &Timeout);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_long(DpcProcessor, (LONG)Index);
    }

    Number.Group = 1;
    Number.Number = 0;
    Status = KeSetTargetProcessorDpcEx(&Dpc, &Number);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
}

START_TEST(KeProcessorGroup)
{
    TestNumbers();
    TestBitCount();
    TestRelationship();
    TestAffinity();
}
