/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     MMIO window and DMA buffer slots
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 *
 * Slots live in the device extension, not in the file object, so they outlive
 * the process that created them.  That is deliberate: stepping a bring-up
 * sequence means running script after script against the same mapped BAR and
 * the same descriptor ring, and each script is its own rospoke.exe invocation.
 * IOCTL_ROSPOKE_RELEASE_ALL, or unloading the driver, is the way back to a
 * clean slate.
 */

#include "rospoke_int.h"

#ifndef ROSPOKE_HOST_HARNESS
#define NDEBUG
#include <debug.h>
#endif

static
MEMORY_CACHING_TYPE
RospokeCacheType(
    _In_ ULONG Requested)
{
    switch (Requested)
    {
        case 1: return MmCached;
        case 2: return MmWriteCombined;
        default: return MmNonCached;
    }
}

PROSPOKE_SLOT
RospokeResolveTarget(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ ULONG Target)
{
    PROSPOKE_SLOT Slot;

    if (Target < ROSPOKE_TARGET_MMIO_COUNT)
        Slot = &Ext->Mmio[Target];
    else if (Target >= ROSPOKE_TARGET_DMA_BASE &&
             Target < ROSPOKE_TARGET_DMA_BASE + ROSPOKE_TARGET_DMA_COUNT)
        Slot = &Ext->Dma[Target - ROSPOKE_TARGET_DMA_BASE];
    else
        return NULL;

    return (Slot->Kind == ROSPOKE_SLOT_FREE) ? NULL : Slot;
}

static
PROSPOKE_SLOT
RospokeTakeMmioSlot(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _Out_ PULONG Target)
{
    ULONG Index;

    for (Index = 0; Index < ROSPOKE_TARGET_MMIO_COUNT; Index++)
    {
        if (Ext->Mmio[Index].Kind == ROSPOKE_SLOT_FREE)
        {
            *Target = ROSPOKE_TARGET_MMIO_BASE + Index;
            return &Ext->Mmio[Index];
        }
    }
    return NULL;
}

static
PROSPOKE_SLOT
RospokeTakeDmaSlot(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _Out_ PULONG Target)
{
    ULONG Index;

    for (Index = 0; Index < ROSPOKE_TARGET_DMA_COUNT; Index++)
    {
        if (Ext->Dma[Index].Kind == ROSPOKE_SLOT_FREE)
        {
            *Target = ROSPOKE_TARGET_DMA_BASE + Index;
            return &Ext->Dma[Index];
        }
    }
    return NULL;
}

static
NTSTATUS
RospokeMapWindow(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ ULONG Length,
    _In_ ULONG CacheTypeIn,
    _In_ ULONG Kind,
    _In_ ULONG BarIndex,
    _Out_ PROSPOKE_SLOT_OUT Out)
{
    PROSPOKE_SLOT Slot;
    ULONG Target;
    PVOID Va;

    if (Length == 0 || Length > ROSPOKE_MAX_BLOCK * 64)
        return STATUS_INVALID_PARAMETER;

    Slot = RospokeTakeMmioSlot(Ext, &Target);
    if (Slot == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Va = MmMapIoSpace(PhysicalAddress, Length, RospokeCacheType(CacheTypeIn));
    if (Va == NULL)
    {
        DPRINT1("rospoke: MmMapIoSpace(%I64x, %lx) failed\n", PhysicalAddress.QuadPart, Length);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Slot->Kind = Kind;
    Slot->Length = Length;
    Slot->PhysicalAddress = PhysicalAddress;
    Slot->VirtualAddress = Va;
    Slot->BarIndex = BarIndex;
    Slot->CacheType = CacheTypeIn;

    Out->Target = Target;
    Out->Length = Length;
    Out->PhysicalAddress = (ULONGLONG)PhysicalAddress.QuadPart;

    DPRINT1("rospoke: slot %lu -> pa %I64x len %lx va %p\n",
            Target, PhysicalAddress.QuadPart, Length, Va);
    return STATUS_SUCCESS;
}

NTSTATUS
RospokeMapBar(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_MAP_BAR_IN *In,
    _Out_ PROSPOKE_SLOT_OUT Out)
{
    const ROSPOKE_BAR_INFO *Bar;
    PHYSICAL_ADDRESS Physical;
    ULONG Length;

    if (!Ext->DeviceOpen)
        return STATUS_DEVICE_NOT_READY;
    if (In->BarIndex >= 6)
        return STATUS_INVALID_PARAMETER;

    Bar = &Ext->LastOpen.Bar[In->BarIndex];
    if (Bar->Length == 0)
    {
        DPRINT1("rospoke: BAR%lu is unimplemented or was never sized\n", In->BarIndex);
        return STATUS_INVALID_PARAMETER;
    }
    if (Bar->Flags & ROSPOKE_BAR_IO)
    {
        /* An I/O BAR is port space; it has no physical window to map.  Nothing
         * this driver targets needs one, so say so rather than fake it. */
        DPRINT1("rospoke: BAR%lu is I/O space, which this driver does not map\n", In->BarIndex);
        return STATUS_NOT_SUPPORTED;
    }

    Length = In->Length ? In->Length : (ULONG)Bar->Length;
    if ((ULONGLONG)Length > Bar->Length)
        return STATUS_INVALID_PARAMETER;

    Physical.QuadPart = (LONGLONG)Bar->Base;
    return RospokeMapWindow(Ext, Physical, Length, 0, ROSPOKE_SLOT_BAR, In->BarIndex, Out);
}

NTSTATUS
RospokeMapPhys(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_MAP_PHYS_IN *In,
    _Out_ PROSPOKE_SLOT_OUT Out)
{
    PHYSICAL_ADDRESS Physical;

    if (In->CacheType > 2)
        return STATUS_INVALID_PARAMETER;

    Physical.QuadPart = (LONGLONG)In->PhysicalAddress;
    return RospokeMapWindow(Ext, Physical, In->Length, In->CacheType,
                            ROSPOKE_SLOT_PHYS, (ULONG)-1, Out);
}

NTSTATUS
RospokeAllocDma(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_ALLOC_DMA_IN *In,
    _Out_ PROSPOKE_SLOT_OUT Out)
{
    PROSPOKE_SLOT Slot;
    PHYSICAL_ADDRESS Low, High, Boundary;
    MEMORY_CACHING_TYPE Cache;
    ULONG Target, Length;
    PVOID Va;

    if (In->Length == 0 || In->Length > ROSPOKE_MAX_DMA_BYTES)
        return STATUS_INVALID_PARAMETER;
    if (In->CacheType > 2)
        return STATUS_INVALID_PARAMETER;

    Slot = RospokeTakeDmaSlot(Ext, &Target);
    if (Slot == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Page-granular so the buffer is naturally aligned enough for any
     * descriptor ring, and so the length we free matches the length we asked
     * for -- MmFreeContiguousMemorySpecifyCache insists on that. */
    Length = (In->Length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    Low.QuadPart = 0;
    Boundary.QuadPart = 0;
    /*
     * Default to a 32-bit-addressable buffer.  Plenty of the hardware worth
     * poking at (ath9k among it) has a 32-bit DMA engine, and a ring the device
     * cannot reach fails in a way that looks exactly like "the device ignored
     * us", which is the hardest possible thing to debug here.
     */
    High.QuadPart = (In->Flags & ROSPOKE_DMA_ABOVE_4G) ? (LONGLONG)-1 : 0xFFFFFFFFLL;
    Cache = RospokeCacheType(In->CacheType);

    Va = MmAllocateContiguousMemorySpecifyCache(Length, Low, High, Boundary, Cache);
    if (Va == NULL)
    {
        DPRINT1("rospoke: contiguous alloc of %lx bytes below %I64x failed\n",
                Length, High.QuadPart);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Va, Length);

    Slot->Kind = ROSPOKE_SLOT_DMA;
    Slot->Length = Length;
    Slot->PhysicalAddress = MmGetPhysicalAddress(Va);
    Slot->VirtualAddress = Va;
    Slot->BarIndex = (ULONG)-1;
    Slot->CacheType = In->CacheType;

    Out->Target = Target;
    Out->Length = Length;
    Out->PhysicalAddress = (ULONGLONG)Slot->PhysicalAddress.QuadPart;

    DPRINT1("rospoke: dma slot %lx -> pa %I64x len %lx va %p\n",
            Target, Slot->PhysicalAddress.QuadPart, Length, Va);
    return STATUS_SUCCESS;
}

VOID
RospokeReleaseAll(
    _Inout_ PROSPOKE_DEVEXT Ext)
{
    ULONG Index;

    for (Index = 0; Index < ROSPOKE_TARGET_MMIO_COUNT; Index++)
    {
        PROSPOKE_SLOT Slot = &Ext->Mmio[Index];

        if (Slot->Kind == ROSPOKE_SLOT_FREE)
            continue;

        MmUnmapIoSpace(Slot->VirtualAddress, Slot->Length);
        RtlZeroMemory(Slot, sizeof(*Slot));
    }

    for (Index = 0; Index < ROSPOKE_TARGET_DMA_COUNT; Index++)
    {
        PROSPOKE_SLOT Slot = &Ext->Dma[Index];

        if (Slot->Kind == ROSPOKE_SLOT_FREE)
            continue;

        MmFreeContiguousMemorySpecifyCache(Slot->VirtualAddress,
                                           Slot->Length,
                                           RospokeCacheType(Slot->CacheType));
        RtlZeroMemory(Slot, sizeof(*Slot));
    }

    Ext->DeviceOpen = FALSE;
    RtlZeroMemory(&Ext->LastOpen, sizeof(Ext->LastOpen));
}

VOID
RospokeQuerySlots(
    _In_ PROSPOKE_DEVEXT Ext,
    _Out_ PROSPOKE_QUERY_SLOTS_OUT Out)
{
    ULONG Index, Count = 0;

    RtlZeroMemory(Out, sizeof(*Out));

    Out->DeviceOpen = Ext->DeviceOpen;
    Out->BusNumber = Ext->BusNumber;
    Out->DeviceNumber = Ext->DeviceNumber;
    Out->FunctionNumber = Ext->FunctionNumber;

    for (Index = 0; Index < ROSPOKE_TARGET_MMIO_COUNT; Index++)
    {
        if (Ext->Mmio[Index].Kind == ROSPOKE_SLOT_FREE)
            continue;

        Out->Slot[Count].Target = ROSPOKE_TARGET_MMIO_BASE + Index;
        Out->Slot[Count].Length = Ext->Mmio[Index].Length;
        Out->Slot[Count].PhysicalAddress = (ULONGLONG)Ext->Mmio[Index].PhysicalAddress.QuadPart;
        Out->Slot[Count].Kind = Ext->Mmio[Index].Kind;
        Out->Slot[Count].BarIndex = Ext->Mmio[Index].BarIndex;
        Count++;
    }

    for (Index = 0; Index < ROSPOKE_TARGET_DMA_COUNT; Index++)
    {
        if (Ext->Dma[Index].Kind == ROSPOKE_SLOT_FREE)
            continue;

        Out->Slot[Count].Target = ROSPOKE_TARGET_DMA_BASE + Index;
        Out->Slot[Count].Length = Ext->Dma[Index].Length;
        Out->Slot[Count].PhysicalAddress = (ULONGLONG)Ext->Dma[Index].PhysicalAddress.QuadPart;
        Out->Slot[Count].Kind = Ext->Dma[Index].Kind;
        Out->Slot[Count].BarIndex = (ULONG)-1;
        Count++;
    }

    Out->SlotCount = Count;
}

static
NTSTATUS
RospokeCheckBlock(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_BLOCK_IN *In,
    _Out_ PROSPOKE_SLOT *SlotOut)
{
    PROSPOKE_SLOT Slot;

    if (In->AccessSize != 1 && In->AccessSize != 2 && In->AccessSize != 4)
        return STATUS_INVALID_PARAMETER;
    if (In->Length == 0 || In->Length > ROSPOKE_MAX_BLOCK)
        return STATUS_INVALID_PARAMETER;
    if ((In->Length % In->AccessSize) != 0 || (In->Offset % In->AccessSize) != 0)
        return STATUS_DATATYPE_MISALIGNMENT;

    Slot = RospokeResolveTarget(Ext, In->Target);
    if (Slot == NULL)
        return STATUS_INVALID_PARAMETER;
    if (In->Offset > Slot->Length || In->Length > Slot->Length - In->Offset)
        return STATUS_INVALID_BUFFER_SIZE;

    *SlotOut = Slot;
    return STATUS_SUCCESS;
}

NTSTATUS
RospokeReadBlock(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_BLOCK_IN *In,
    _Out_writes_bytes_(In->Length) PVOID Buffer)
{
    PROSPOKE_SLOT Slot;
    NTSTATUS Status;
    PUCHAR Base;

    Status = RospokeCheckBlock(Ext, In, &Slot);
    if (!NT_SUCCESS(Status))
        return Status;

    Base = (PUCHAR)Slot->VirtualAddress + In->Offset;

    if (Slot->Kind == ROSPOKE_SLOT_DMA)
    {
        RtlCopyMemory(Buffer, Base, In->Length);
        return STATUS_SUCCESS;
    }

    /* MMIO has to be read at the width the device expects; a memcpy would be
     * free to split or merge accesses and some registers latch on width. */
    switch (In->AccessSize)
    {
        case 4:
            READ_REGISTER_BUFFER_ULONG((PULONG)Base, (PULONG)Buffer, In->Length / 4);
            break;
        case 2:
            READ_REGISTER_BUFFER_USHORT((PUSHORT)Base, (PUSHORT)Buffer, In->Length / 2);
            break;
        default:
            READ_REGISTER_BUFFER_UCHAR(Base, (PUCHAR)Buffer, In->Length);
            break;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
RospokeWriteBlock(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_BLOCK_IN *In,
    _In_reads_bytes_(In->Length) const VOID *Buffer)
{
    PROSPOKE_SLOT Slot;
    NTSTATUS Status;
    PUCHAR Base;

    Status = RospokeCheckBlock(Ext, In, &Slot);
    if (!NT_SUCCESS(Status))
        return Status;

    Base = (PUCHAR)Slot->VirtualAddress + In->Offset;

    if (Slot->Kind == ROSPOKE_SLOT_DMA)
    {
        RtlCopyMemory(Base, Buffer, In->Length);
        return STATUS_SUCCESS;
    }

    switch (In->AccessSize)
    {
        case 4:
            WRITE_REGISTER_BUFFER_ULONG((PULONG)Base, (PULONG)(ULONG_PTR)Buffer, In->Length / 4);
            break;
        case 2:
            WRITE_REGISTER_BUFFER_USHORT((PUSHORT)Base, (PUSHORT)(ULONG_PTR)Buffer, In->Length / 2);
            break;
        default:
            WRITE_REGISTER_BUFFER_UCHAR(Base, (PUCHAR)(ULONG_PTR)Buffer, In->Length);
            break;
    }
    return STATUS_SUCCESS;
}
