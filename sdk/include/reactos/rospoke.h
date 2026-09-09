/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared user/kernel ABI for rospoke.sys
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 *
 * WHAT THIS IS FOR
 *
 * Bringing a PCI device up (arming a DMA ring, walking a PHY reset sequence)
 * normally means editing a miniport, rebuilding, and rebooting, because a PnP
 * driver image can never be unloaded at run time -- IopUnloadDriver() refuses
 * anything without DRVO_LEGACY_DRIVER (ntoskrnl/io/iomgr/driver.c).  rospoke is
 * a *legacy* driver, so `sc stop` really does unload it, and it executes
 * register programs supplied from user mode.  The experiment therefore lives in
 * a text file instead of in a .sys, and the edit/try cycle costs a second
 * instead of a boot.
 *
 * A program is a batch, not a stream of round trips: the initvals table of a
 * wireless PHY is thousands of writes and its reset path polls on microsecond
 * deadlines, neither of which survives a syscall (let alone a TCP hop) per
 * register.  The kernel runs the whole array and returns every read at once.
 *
 * SECURITY
 *
 * This driver hands arbitrary physical-memory and PCI-config read/write to
 * whoever can open its device.  That is total system compromise by design.  It
 * is built only under ENABLE_ROSPOKE (default OFF) and registers no service of
 * its own -- somebody with SeLoadDriverPrivilege has to install it deliberately.
 * Never enable it in a build anyone else will run.
 *
 * This header is included from both kernel and user mode.  It relies on the
 * includer having pulled in the DDK (wdm.h/ntddk.h) or the PSDK (windows.h plus
 * winioctl.h) first, and uses nothing beyond ULONG/USHORT/UCHAR/ULONGLONG,
 * CTL_CODE and METHOD_BUFFERED.
 */

#ifndef _ROSPOKE_H_
#define _ROSPOKE_H_

#define ROSPOKE_ABI_VERSION         1

#define ROSPOKE_DEVICE_NAME         L"\\Device\\RosPoke"
#define ROSPOKE_DOS_NAME            L"\\DosDevices\\RosPoke"
#define ROSPOKE_WIN32_NAME          L"\\\\.\\RosPoke"
#define ROSPOKE_SERVICE_NAME        L"rospoke"
#define ROSPOKE_WIN32_NAME_A        "\\\\.\\RosPoke"
#define ROSPOKE_SERVICE_NAME_A      "rospoke"

#define ROSPOKE_CTL(_i) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (_i), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define IOCTL_ROSPOKE_GET_VERSION   ROSPOKE_CTL(0)
#define IOCTL_ROSPOKE_OPEN_PCI      ROSPOKE_CTL(1)
#define IOCTL_ROSPOKE_MAP_BAR       ROSPOKE_CTL(2)
#define IOCTL_ROSPOKE_MAP_PHYS      ROSPOKE_CTL(3)
#define IOCTL_ROSPOKE_ALLOC_DMA     ROSPOKE_CTL(4)
#define IOCTL_ROSPOKE_READ_BLOCK    ROSPOKE_CTL(5)
#define IOCTL_ROSPOKE_WRITE_BLOCK   ROSPOKE_CTL(6)
#define IOCTL_ROSPOKE_EXEC          ROSPOKE_CTL(7)
#define IOCTL_ROSPOKE_RELEASE_ALL   ROSPOKE_CTL(8)
#define IOCTL_ROSPOKE_QUERY_SLOTS   ROSPOKE_CTL(9)

/*
 * Targets addressed by an op.  Slots are global to the driver, not per open
 * handle, so a second rospoke.exe invocation still sees the BAR the first one
 * mapped -- that is what makes stepping a bring-up sequence across several
 * scripts possible.  IOCTL_ROSPOKE_RELEASE_ALL (or unloading the driver) is the
 * reset button.
 */
#define ROSPOKE_TARGET_MMIO_BASE    0x00u   /* MMIO slots 0x00..0x07 */
#define ROSPOKE_TARGET_MMIO_COUNT   8u
#define ROSPOKE_TARGET_DMA_BASE     0x80u   /* DMA slots 0x80..0x87 */
#define ROSPOKE_TARGET_DMA_COUNT    8u
#define ROSPOKE_TARGET_CFG          0xC0u   /* PCI config space of the open device */

#define ROSPOKE_MAX_OPS             65536u
#define ROSPOKE_MAX_RESULTS         65536u
#define ROSPOKE_MAX_BLOCK           (1024u * 1024u)
#define ROSPOKE_MAX_DMA_BYTES       (16u * 1024u * 1024u)
#define ROSPOKE_BUDGET_US           60000000u   /* whole-batch wall-clock ceiling */
#define ROSPOKE_MAX_DELAY_US        5000000u    /* per DelayUs op */

/* Opcodes.  Ops marked (result) append one ULONG to the result array. */
#define ROSPOKE_OP_END              0u
#define ROSPOKE_OP_WRITE32          1u
#define ROSPOKE_OP_READ32           2u  /* result */
#define ROSPOKE_OP_RMW32            3u  /* result: the value read before the write */
#define ROSPOKE_OP_POLL32           4u  /* result: the value that satisfied the poll */
#define ROSPOKE_OP_DELAY_US         5u
#define ROSPOKE_OP_MARK             6u  /* result: Arg0, echoed back */
#define ROSPOKE_OP_WRITE8           7u
#define ROSPOKE_OP_READ8            8u  /* result */
#define ROSPOKE_OP_WRITE16          9u
#define ROSPOKE_OP_READ16           10u /* result */
#define ROSPOKE_OP_MAX              11u

#include <pshpack4.h>

typedef struct _ROSPOKE_OP
{
    ULONG Opcode;
    ULONG Target;
    ULONG Offset;
    ULONG Arg0;     /* Write: value   Rmw: AndMask  Poll: Mask     Delay: us  Mark: tag */
    ULONG Arg1;     /*                Rmw: OrValue  Poll: Value */
    ULONG Arg2;     /*                              Poll: TimeoutUs */
} ROSPOKE_OP, *PROSPOKE_OP;

/* Execution outcome.  Not an NTSTATUS: these say which op went wrong and why. */
#define ROSPOKE_EXEC_OK             0u
#define ROSPOKE_EXEC_BAD_OPCODE     1u
#define ROSPOKE_EXEC_BAD_TARGET     2u
#define ROSPOKE_EXEC_OUT_OF_BOUNDS  3u
#define ROSPOKE_EXEC_UNALIGNED      4u
#define ROSPOKE_EXEC_POLL_TIMEOUT   5u
#define ROSPOKE_EXEC_RESULT_OVERFLOW 6u
#define ROSPOKE_EXEC_BUDGET         7u
#define ROSPOKE_EXEC_NO_DEVICE      8u  /* a cfg op with no device opened */
#define ROSPOKE_EXEC_CFG_FAILED     9u
#define ROSPOKE_EXEC_MAX            10u

typedef struct _ROSPOKE_EXEC_IN
{
    ULONG OpCount;
    ULONG Flags;            /* reserved, must be 0 */
    /* ROSPOKE_OP Ops[OpCount] follows */
} ROSPOKE_EXEC_IN, *PROSPOKE_EXEC_IN;

typedef struct _ROSPOKE_EXEC_OUT
{
    ULONG ExecStatus;       /* ROSPOKE_EXEC_* */
    ULONG OpsExecuted;      /* == OpCount on success, else the index that failed */
    ULONG ResultCount;
    ULONG ElapsedUs;
    /* ULONG Results[ResultCount] follows */
} ROSPOKE_EXEC_OUT, *PROSPOKE_EXEC_OUT;

/* IOCTL_ROSPOKE_GET_VERSION */
typedef struct _ROSPOKE_VERSION_OUT
{
    ULONG AbiVersion;
    ULONG MmioSlots;
    ULONG DmaSlots;
    ULONG MaxOps;
} ROSPOKE_VERSION_OUT, *PROSPOKE_VERSION_OUT;

/* IOCTL_ROSPOKE_OPEN_PCI */
#define ROSPOKE_OPEN_FORCE          0x00000001u /* allow ExpectedVendorId == 0xFFFF */
#define ROSPOKE_OPEN_NO_SIZE_BARS   0x00000002u /* skip the write-ones BAR sizing probe */
#define ROSPOKE_OPEN_SET_MASTER     0x00000004u /* set COMMAND.BUS_MASTER|MEM|IO */

typedef struct _ROSPOKE_OPEN_PCI_IN
{
    ULONG BusNumber;
    ULONG DeviceNumber;
    ULONG FunctionNumber;
    USHORT ExpectedVendorId;    /* 0xFFFF with ROSPOKE_OPEN_FORCE means "any" */
    USHORT ExpectedDeviceId;
    ULONG Flags;
} ROSPOKE_OPEN_PCI_IN, *PROSPOKE_OPEN_PCI_IN;

#define ROSPOKE_BAR_IO              0x00000001u
#define ROSPOKE_BAR_MEM64           0x00000002u
#define ROSPOKE_BAR_PREFETCHABLE    0x00000004u

typedef struct _ROSPOKE_BAR_INFO
{
    ULONGLONG Base;
    ULONGLONG Length;       /* 0 if unimplemented or sizing was skipped */
    ULONG Flags;
    ULONG Reserved;
} ROSPOKE_BAR_INFO, *PROSPOKE_BAR_INFO;

typedef struct _ROSPOKE_OPEN_PCI_OUT
{
    USHORT VendorId;
    USHORT DeviceId;
    USHORT SubVendorId;
    USHORT SubDeviceId;
    USHORT Command;
    USHORT Status;
    UCHAR RevisionId;
    UCHAR ProgIf;
    UCHAR SubClass;
    UCHAR BaseClass;
    UCHAR HeaderType;
    UCHAR InterruptLine;
    UCHAR InterruptPin;
    UCHAR Reserved;
    ROSPOKE_BAR_INFO Bar[6];
} ROSPOKE_OPEN_PCI_OUT, *PROSPOKE_OPEN_PCI_OUT;

/* IOCTL_ROSPOKE_MAP_BAR */
typedef struct _ROSPOKE_MAP_BAR_IN
{
    ULONG BarIndex;
    ULONG Length;           /* 0 = the whole BAR as sized at open time */
    ULONG Flags;            /* reserved */
} ROSPOKE_MAP_BAR_IN, *PROSPOKE_MAP_BAR_IN;

/* IOCTL_ROSPOKE_MAP_PHYS -- reaches PCIe extended config space, which the HAL's
 * CF8/CFC config path cannot: MMCONFIG_BASE + (bus<<20|dev<<15|fn<<12). */
typedef struct _ROSPOKE_MAP_PHYS_IN
{
    ULONGLONG PhysicalAddress;
    ULONG Length;
    ULONG CacheType;        /* MmNonCached(0) / MmCached(1) / MmWriteCombined(2) */
} ROSPOKE_MAP_PHYS_IN, *PROSPOKE_MAP_PHYS_IN;

/* IOCTL_ROSPOKE_ALLOC_DMA */
#define ROSPOKE_DMA_ABOVE_4G        0x00000001u /* default is a 32-bit-safe buffer */

typedef struct _ROSPOKE_ALLOC_DMA_IN
{
    ULONG Length;
    ULONG Flags;
    ULONG CacheType;
} ROSPOKE_ALLOC_DMA_IN, *PROSPOKE_ALLOC_DMA_IN;

/* Shared reply for MAP_BAR / MAP_PHYS / ALLOC_DMA */
typedef struct _ROSPOKE_SLOT_OUT
{
    ULONG Target;           /* the value to put in ROSPOKE_OP.Target */
    ULONG Length;
    ULONGLONG PhysicalAddress;
} ROSPOKE_SLOT_OUT, *PROSPOKE_SLOT_OUT;

/* IOCTL_ROSPOKE_READ_BLOCK / IOCTL_ROSPOKE_WRITE_BLOCK */
typedef struct _ROSPOKE_BLOCK_IN
{
    ULONG Target;
    ULONG Offset;
    ULONG Length;           /* bytes; for WRITE the payload follows this struct */
    ULONG AccessSize;       /* 1, 2 or 4 -- MMIO must be accessed at its native width */
} ROSPOKE_BLOCK_IN, *PROSPOKE_BLOCK_IN;

/* IOCTL_ROSPOKE_QUERY_SLOTS */
typedef struct _ROSPOKE_SLOT_INFO
{
    ULONG Target;
    ULONG Length;
    ULONGLONG PhysicalAddress;
    ULONG Kind;             /* ROSPOKE_SLOT_* */
    ULONG BarIndex;         /* valid for ROSPOKE_SLOT_BAR */
} ROSPOKE_SLOT_INFO, *PROSPOKE_SLOT_INFO;

#define ROSPOKE_SLOT_FREE           0u
#define ROSPOKE_SLOT_BAR            1u
#define ROSPOKE_SLOT_PHYS           2u
#define ROSPOKE_SLOT_DMA            3u

typedef struct _ROSPOKE_QUERY_SLOTS_OUT
{
    ULONG DeviceOpen;
    ULONG BusNumber;
    ULONG DeviceNumber;
    ULONG FunctionNumber;
    ULONG SlotCount;
    ULONG Reserved;
    ROSPOKE_SLOT_INFO Slot[ROSPOKE_TARGET_MMIO_COUNT + ROSPOKE_TARGET_DMA_COUNT];
} ROSPOKE_QUERY_SLOTS_OUT, *PROSPOKE_QUERY_SLOTS_OUT;

#include <poppack.h>

#endif /* _ROSPOKE_H_ */
