/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     PCI configuration access and device identification
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 *
 * A legacy driver has no PDO, so there is no BUS_INTERFACE_STANDARD to query
 * and HalGetBusDataByOffset is the only way in.  That is fine here precisely
 * because the caller names the bus/device/function explicitly -- the reason to
 * distrust the HAL path elsewhere is guessing at the address, not the path.
 *
 * It does mean config space stops at offset 0xFF: the HAL speaks CF8/CFC, not
 * MMCONFIG.  Extended capabilities live above that, and IOCTL_ROSPOKE_MAP_PHYS
 * over MMCONFIG_BASE + (bus << 20 | dev << 15 | fn << 12) is how you reach them.
 */

#include "rospoke_int.h"

#ifndef ROSPOKE_HOST_HARNESS
#define NDEBUG
#include <debug.h>
#endif

#define ROSPOKE_CFG_LIMIT           0x100u

#define ROSPOKE_CFG_COMMAND         0x04u
#define ROSPOKE_CFG_BAR0            0x10u
#define ROSPOKE_CFG_BAR_COUNT       6u

#ifndef PCI_ENABLE_IO_SPACE
#define PCI_ENABLE_IO_SPACE         0x0001
#endif
#ifndef PCI_ENABLE_MEMORY_SPACE
#define PCI_ENABLE_MEMORY_SPACE     0x0002
#endif
#ifndef PCI_ENABLE_BUS_MASTER
#define PCI_ENABLE_BUS_MASTER       0x0004
#endif

static
ULONG
RospokeSlotOf(
    _In_ ULONG DeviceNumber,
    _In_ ULONG FunctionNumber)
{
    PCI_SLOT_NUMBER Slot;

    Slot.u.AsULONG = 0;
    Slot.u.bits.DeviceNumber = DeviceNumber;
    Slot.u.bits.FunctionNumber = FunctionNumber;
    return Slot.u.AsULONG;
}

static
BOOLEAN
RospokeCfgReadAt(
    _In_ ULONG Bus,
    _In_ ULONG Device,
    _In_ ULONG Function,
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    ULONG Done;

    if (Offset >= ROSPOKE_CFG_LIMIT || Length > ROSPOKE_CFG_LIMIT - Offset)
        return FALSE;

    Done = HalGetBusDataByOffset(PCIConfiguration,
                                 Bus,
                                 RospokeSlotOf(Device, Function),
                                 Buffer,
                                 Offset,
                                 Length);
    return (Done == Length);
}

static
BOOLEAN
RospokeCfgWriteAt(
    _In_ ULONG Bus,
    _In_ ULONG Device,
    _In_ ULONG Function,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    ULONG Done;

    if (Offset >= ROSPOKE_CFG_LIMIT || Length > ROSPOKE_CFG_LIMIT - Offset)
        return FALSE;

    Done = HalSetBusDataByOffset(PCIConfiguration,
                                 Bus,
                                 RospokeSlotOf(Device, Function),
                                 (PVOID)(ULONG_PTR)Buffer,
                                 Offset,
                                 Length);
    return (Done == Length);
}

BOOLEAN
RospokeCfgRead(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    if (!Ext->DeviceOpen)
        return FALSE;

    return RospokeCfgReadAt(Ext->BusNumber,
                            Ext->DeviceNumber,
                            Ext->FunctionNumber,
                            Offset,
                            Buffer,
                            Length);
}

BOOLEAN
RospokeCfgWrite(
    _In_ PROSPOKE_DEVEXT Ext,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    if (!Ext->DeviceOpen)
        return FALSE;

    return RospokeCfgWriteAt(Ext->BusNumber,
                             Ext->DeviceNumber,
                             Ext->FunctionNumber,
                             Offset,
                             Buffer,
                             Length);
}

/*
 * Turn the (original, all-ones probe) pairs into base/length/flags.  Kept apart
 * from the probing above so it can be exercised on the host: a mis-decoded
 * 64-bit BAR maps the wrong window, and that fails later and elsewhere, looking
 * for all the world like the device ignoring us.
 */
VOID
RospokeDecodeBars(
    _In_reads_(6) const ULONG *Original,
    _In_reads_(6) const ULONG *Probe,
    _Inout_ PROSPOKE_OPEN_PCI_OUT Out)
{
    ULONG Index;

    for (Index = 0; Index < ROSPOKE_CFG_BAR_COUNT; Index++)
    {
        ULONGLONG Mask, Length;
        ULONG Base = Original[Index];

        if (Probe[Index] == 0 || Probe[Index] == 0xFFFFFFFFu)
            continue;

        if (Base & 0x1u)
        {
            /* I/O BAR */
            Mask = (ULONGLONG)(Probe[Index] & ~0x3u);
            Length = (~Mask + 1) & 0xFFFFu;
            Out->Bar[Index].Base = Base & ~0x3u;
            Out->Bar[Index].Length = Length;
            Out->Bar[Index].Flags = ROSPOKE_BAR_IO;
            continue;
        }

        if (Base & 0x8u)
            Out->Bar[Index].Flags |= ROSPOKE_BAR_PREFETCHABLE;

        if ((Base & 0x6u) == 0x4u && Index + 1 < ROSPOKE_CFG_BAR_COUNT)
        {
            /* 64-bit BAR: the upper half lives in the next slot, which is then
             * not a BAR of its own and must not be reported as one. */
            Mask = ((ULONGLONG)Probe[Index + 1] << 32) | (Probe[Index] & ~0xFu);
            Length = ~Mask + 1;
            Out->Bar[Index].Base = ((ULONGLONG)Original[Index + 1] << 32) | (Base & ~0xFu);
            Out->Bar[Index].Length = Length;
            Out->Bar[Index].Flags |= ROSPOKE_BAR_MEM64;
            Index++;
            continue;
        }

        Mask = (ULONGLONG)(Probe[Index] & ~0xFu);
        Length = (~Mask + 1) & 0xFFFFFFFFu;
        Out->Bar[Index].Base = Base & ~0xFu;
        Out->Bar[Index].Length = Length;
    }
}

/*
 * Standard write-all-ones BAR sizing.  Decode has to be off while the BAR holds
 * the probe value, otherwise the device is briefly claiming an enormous window
 * that may overlap somebody else's.  Command is restored before returning even
 * on the failure paths.
 */
static
VOID
RospokeSizeBars(
    _In_ ULONG Bus,
    _In_ ULONG Device,
    _In_ ULONG Function,
    _Inout_ PROSPOKE_OPEN_PCI_OUT Out)
{
    USHORT Command, Quiet;
    ULONG Index;
    ULONG Original[ROSPOKE_CFG_BAR_COUNT];
    ULONG Probe[ROSPOKE_CFG_BAR_COUNT];
    BOOLEAN Restored = TRUE;

    if (!RospokeCfgReadAt(Bus, Device, Function, ROSPOKE_CFG_COMMAND, &Command, sizeof(Command)))
        return;

    Quiet = (USHORT)(Command & ~(PCI_ENABLE_IO_SPACE | PCI_ENABLE_MEMORY_SPACE));
    if (!RospokeCfgWriteAt(Bus, Device, Function, ROSPOKE_CFG_COMMAND, &Quiet, sizeof(Quiet)))
        return;

    for (Index = 0; Index < ROSPOKE_CFG_BAR_COUNT; Index++)
    {
        ULONG Ones = 0xFFFFFFFFu;
        ULONG Offset = ROSPOKE_CFG_BAR0 + Index * sizeof(ULONG);

        Original[Index] = 0;
        Probe[Index] = 0;

        if (!RospokeCfgReadAt(Bus, Device, Function, Offset, &Original[Index], sizeof(ULONG)))
            continue;
        if (!RospokeCfgWriteAt(Bus, Device, Function, Offset, &Ones, sizeof(ULONG)))
            continue;
        if (!RospokeCfgReadAt(Bus, Device, Function, Offset, &Probe[Index], sizeof(ULONG)))
            Restored = FALSE;
        if (!RospokeCfgWriteAt(Bus, Device, Function, Offset, &Original[Index], sizeof(ULONG)))
            Restored = FALSE;
    }

    RospokeCfgWriteAt(Bus, Device, Function, ROSPOKE_CFG_COMMAND, &Command, sizeof(Command));

    if (!Restored)
    {
        DPRINT1("rospoke: BAR sizing on %02lx:%02lx.%lx could not fully restore "
                "the original BARs; the device may now be unusable until reset\n",
                Bus, Device, Function);
    }

    RospokeDecodeBars(Original, Probe, Out);
}

NTSTATUS
RospokeOpenPci(
    _Inout_ PROSPOKE_DEVEXT Ext,
    _In_ const ROSPOKE_OPEN_PCI_IN *In,
    _Out_ PROSPOKE_OPEN_PCI_OUT Out)
{
    UCHAR Header[0x40];
    USHORT VendorId, DeviceId, Command;

    if (In->BusNumber > 0xFFu || In->DeviceNumber > 0x1Fu || In->FunctionNumber > 0x7u)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Out, sizeof(*Out));

    if (!RospokeCfgReadAt(In->BusNumber, In->DeviceNumber, In->FunctionNumber,
                          0, Header, sizeof(Header)))
    {
        DPRINT1("rospoke: config read of %02lx:%02lx.%lx failed\n",
                In->BusNumber, In->DeviceNumber, In->FunctionNumber);
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    VendorId = (USHORT)(Header[0] | (Header[1] << 8));
    DeviceId = (USHORT)(Header[2] | (Header[3] << 8));

    if (VendorId == 0xFFFFu || VendorId == 0x0000u)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    /*
     * The expected-ID check is the guard that stops a mistyped bus:device.fn
     * from turning into register writes on the storage controller.  Waiving it
     * has to be deliberate.
     */
    if (In->ExpectedVendorId != 0xFFFFu && In->ExpectedVendorId != VendorId)
    {
        DPRINT1("rospoke: %02lx:%02lx.%lx is %04x:%04x, caller expected vendor %04x\n",
                In->BusNumber, In->DeviceNumber, In->FunctionNumber,
                VendorId, DeviceId, In->ExpectedVendorId);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (In->ExpectedDeviceId != 0xFFFFu && In->ExpectedDeviceId != DeviceId)
    {
        DPRINT1("rospoke: %02lx:%02lx.%lx is %04x:%04x, caller expected device %04x\n",
                In->BusNumber, In->DeviceNumber, In->FunctionNumber,
                VendorId, DeviceId, In->ExpectedDeviceId);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (In->ExpectedVendorId == 0xFFFFu && !(In->Flags & ROSPOKE_OPEN_FORCE))
        return STATUS_INVALID_PARAMETER;

    Out->VendorId = VendorId;
    Out->DeviceId = DeviceId;
    Out->Command = (USHORT)(Header[0x04] | (Header[0x05] << 8));
    Out->Status = (USHORT)(Header[0x06] | (Header[0x07] << 8));
    Out->RevisionId = Header[0x08];
    Out->ProgIf = Header[0x09];
    Out->SubClass = Header[0x0A];
    Out->BaseClass = Header[0x0B];
    Out->HeaderType = Header[0x0E];
    Out->SubVendorId = (USHORT)(Header[0x2C] | (Header[0x2D] << 8));
    Out->SubDeviceId = (USHORT)(Header[0x2E] | (Header[0x2F] << 8));
    Out->InterruptLine = Header[0x3C];
    Out->InterruptPin = Header[0x3D];

    /* Only a type 0 header has six BARs; probing a bridge's would corrupt its
     * window registers. */
    if ((Out->HeaderType & 0x7Fu) == 0 && !(In->Flags & ROSPOKE_OPEN_NO_SIZE_BARS))
        RospokeSizeBars(In->BusNumber, In->DeviceNumber, In->FunctionNumber, Out);

    if (In->Flags & ROSPOKE_OPEN_SET_MASTER)
    {
        Command = (USHORT)(Out->Command | PCI_ENABLE_IO_SPACE |
                           PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER);
        if (RospokeCfgWriteAt(In->BusNumber, In->DeviceNumber, In->FunctionNumber,
                              ROSPOKE_CFG_COMMAND, &Command, sizeof(Command)))
        {
            Out->Command = Command;
        }
    }

    Ext->DeviceOpen = TRUE;
    Ext->BusNumber = In->BusNumber;
    Ext->DeviceNumber = In->DeviceNumber;
    Ext->FunctionNumber = In->FunctionNumber;
    Ext->LastOpen = *Out;

    DPRINT1("rospoke: opened %02lx:%02lx.%lx %04x:%04x class %02x:%02x rev %02x\n",
            In->BusNumber, In->DeviceNumber, In->FunctionNumber,
            VendorId, DeviceId, Out->BaseClass, Out->SubClass, Out->RevisionId);

    return STATUS_SUCCESS;
}
