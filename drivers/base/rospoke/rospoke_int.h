/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver-internal declarations
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 */

#ifndef _ROSPOKE_INT_H_
#define _ROSPOKE_INT_H_

#ifdef ROSPOKE_HOST_HARNESS
/* Host build: stand-ins for the handful of DDK facilities this driver uses, so
 * the parts that are pure logic can be run under ASan on a workstation instead
 * of only on the machine they are meant to debug. */
#include "harness/kstubs.h"
#else
#include <ntddk.h>
#endif

#include <reactos/rospoke.h>

#define ROSPOKE_TAG                 'ekoP'

/*
 * Anything at or below this is busy-waited.  KeDelayExecutionThread rounds up
 * to a clock tick (~15 ms), which would turn a faithful 2 ms bring-up delay
 * into an eightfold one and make a captured trace useless for comparison
 * against a known-good stack.
 */
#define ROSPOKE_STALL_LIMIT_US      20000u

typedef struct _ROSPOKE_SLOT
{
    ULONG Kind;                     /* ROSPOKE_SLOT_* */
    ULONG Length;
    PHYSICAL_ADDRESS PhysicalAddress;
    PVOID VirtualAddress;
    ULONG BarIndex;                 /* ROSPOKE_SLOT_BAR only */
    ULONG CacheType;
} ROSPOKE_SLOT, *PROSPOKE_SLOT;

typedef struct _ROSPOKE_DEVEXT
{
    /*
     * A KMUTEX rather than a FAST_MUTEX because it must be held across
     * MmMapIoSpace and MmAllocateContiguousMemorySpecifyCache, both of which
     * require PASSIVE_LEVEL; a fast mutex would have raised us to APC_LEVEL.
     * Holding it across a whole batch is intended -- two callers poking the
     * same device at once is not a thing to allow.
     */
    KMUTEX Lock;
    BOOLEAN DeviceOpen;
    ULONG BusNumber;
    ULONG DeviceNumber;
    ULONG FunctionNumber;
    ROSPOKE_OPEN_PCI_OUT LastOpen;
    ROSPOKE_SLOT Mmio[ROSPOKE_TARGET_MMIO_COUNT];
    ROSPOKE_SLOT Dma[ROSPOKE_TARGET_DMA_COUNT];
} ROSPOKE_DEVEXT, *PROSPOKE_DEVEXT;

/* pci.c */
NTSTATUS
RospokeOpenPci(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_OPEN_PCI_IN *In,
    _Out_ PROSPOKE_OPEN_PCI_OUT Out);

BOOLEAN
RospokeCfgRead(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

VOID
RospokeDecodeBars(
    _In_reads_(6) const ULONG *Original,
    _In_reads_(6) const ULONG *Probe,
    _Inout_ PROSPOKE_OPEN_PCI_OUT Out);

BOOLEAN
RospokeCfgWrite(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length);

/* slots.c */
NTSTATUS
RospokeMapBar(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_MAP_BAR_IN *In,
    _Out_ PROSPOKE_SLOT_OUT Out);

NTSTATUS
RospokeMapPhys(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_MAP_PHYS_IN *In,
    _Out_ PROSPOKE_SLOT_OUT Out);

NTSTATUS
RospokeAllocDma(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_ALLOC_DMA_IN *In,
    _Out_ PROSPOKE_SLOT_OUT Out);

VOID
RospokeReleaseAll(
    _Inout_ PROSPOKE_DEVEXT Ext);

VOID
RospokeQuerySlots(
    _In_ PROSPOKE_DEVEXT Ext,
    _Out_ PROSPOKE_QUERY_SLOTS_OUT Out);

PROSPOKE_SLOT
RospokeResolveTarget(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ ULONG Target);

NTSTATUS
RospokeReadBlock(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_BLOCK_IN *In,
    _Out_writes_bytes_(In->Length) PVOID Buffer);

NTSTATUS
RospokeWriteBlock(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_BLOCK_IN *In,
    _In_reads_bytes_(In->Length) const VOID *Buffer);

/* exec.c */
VOID
RospokeExec(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_reads_(OpCount) const ROSPOKE_OP *Ops,
    _In_ ULONG OpCount,
    _Out_writes_to_(ResultCapacity, *ResultCount) PULONG Results,
    _In_ ULONG ResultCapacity,
    _Out_ PULONG ResultCount,
    _Out_ PROSPOKE_EXEC_OUT Out);

VOID
RospokeStallUs(
    _In_ ULONG Micros);

#endif /* _ROSPOKE_INT_H_ */
