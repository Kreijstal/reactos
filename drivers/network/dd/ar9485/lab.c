/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode bring-up lab: control device and opcode interpreter.
 *
 * ar9485.sys is PnP-bound and cannot be swapped on a live system, so every
 * register-level hypothesis otherwise costs a deploy and a reboot.  This file
 * is the fixed half of the split: an interpreter general enough that no
 * bring-up experiment should ever need new kernel code.  The volatile half --
 * reset sequences, initval tables, descriptor layouts -- lives on the host and
 * arrives as program data.
 *
 * Two rules shape everything here:
 *
 *   1. The chip masters DMA.  Nothing may leave it pointed at memory this
 *      driver has freed; both partitions on the boot medium are NTFS.
 *   2. The machine powers itself off during a prolonged stall, so every wait
 *      is bounded per-op AND per-program.
 *
 * With no adapter bound (the driver started by hand on a machine that has no
 * AR9485, e.g. under QEMU) the interpreter targets a scratch page instead of
 * the BAR.  That exists so the whole chain -- encoding, bounds, marshalling,
 * transport, host parsing -- can be proven before spending the one install
 * boot on real hardware.
 */

#include "ar9485.h"
#include "ath9k/reg.h"
#include "lab_vm.h"

#define NDEBUG
#include <debug.h>

#ifdef AR9485_LAB

#define AR9485LAB_SCRATCH_LENGTH    (64 * 1024)

typedef struct _AR9485LAB_DMA
{
    PVOID VirtualAddress;
    NDIS_PHYSICAL_ADDRESS PhysicalAddress;
    ULONG Length;
    BOOLEAN FromNdis;
} AR9485LAB_DMA, *PAR9485LAB_DMA;

typedef struct _AR9485LAB_STATE
{
    NDIS_HANDLE DeviceHandle;
    PDEVICE_OBJECT DeviceObject;
    KMUTEX Lock;

    /* Published by init.c once MiniportInitializeEx has a usable adapter,
     * and withdrawn before HaltEx tears the BAR mapping down. */
    PAR9485_ADAPTER Adapter;

    /* Whoever holds the chip.  Compared by pointer only, never dereferenced. */
    PFILE_OBJECT Owner;

    PVOID Scratch;
    ULONG ScratchLength;

    AR9485LAB_DMA Runtime[AR9485LAB_MAX_DMA_ALLOC];
    ULONG RuntimeCount;
    ULONG RuntimeBytes;
} AR9485LAB_STATE;

static AR9485LAB_STATE gLab;
static BOOLEAN gLabInitialized = FALSE;

/* ===========================================================================
 *  Locking
 * ===========================================================================
 */

static VOID
LabAcquire(VOID)
{
    KeWaitForSingleObject(&gLab.Lock, Executive, KernelMode, FALSE, NULL);
}

static VOID
LabRelease(VOID)
{
    KeReleaseMutex(&gLab.Lock, FALSE);
}

/* ===========================================================================
 *  Target resolution
 *
 *  Real hardware when an adapter is bound, otherwise a scratch page so the
 *  interpreter itself can be tested without an AR9485 in the machine.
 * ===========================================================================
 */

static BOOLEAN
LabScratchTarget(VOID)
{
    return (gLab.Adapter == NULL || gLab.Adapter->IoBase == NULL);
}

static PVOID
LabTargetBase(_Out_ PULONG Length)
{
    if (!LabScratchTarget())
    {
        *Length = gLab.Adapter->IoLength;
        return gLab.Adapter->IoBase;
    }

    if (gLab.Scratch == NULL)
    {
        gLab.Scratch = ExAllocatePoolWithTag(NonPagedPool,
                                             AR9485LAB_SCRATCH_LENGTH,
                                             AR9485_TAG);
        if (gLab.Scratch != NULL)
        {
            RtlZeroMemory(gLab.Scratch, AR9485LAB_SCRATCH_LENGTH);
            gLab.ScratchLength = AR9485LAB_SCRATCH_LENGTH;
        }
    }

    *Length = gLab.ScratchLength;
    return gLab.Scratch;
}

static ULONG
LabRead32(_In_ PVOID Base, _In_ ULONG Offset)
{
    if (LabScratchTarget())
        return *(volatile ULONG *)((PUCHAR)Base + Offset);

    return READ_REGISTER_ULONG((PULONG)((PUCHAR)Base + Offset));
}

static VOID
LabWrite32(_In_ PVOID Base, _In_ ULONG Offset, _In_ ULONG Value)
{
    if (LabScratchTarget())
        *(volatile ULONG *)((PUCHAR)Base + Offset) = Value;
    else
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Base + Offset), Value);
}

/* ===========================================================================
 *  Waiting, with a per-program budget
 * ===========================================================================
 */

static VOID
LabStall(_In_ ULONG Microseconds)
{
    LARGE_INTEGER Interval;

    /* Short waits busy-spin; anything longer yields, so a program full of
     * millisecond settle times does not pin a processor. */
    if (Microseconds <= 50)
    {
        KeStallExecutionProcessor(Microseconds);
        return;
    }

    Interval.QuadPart = -((LONGLONG)Microseconds * 10);
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

/* ===========================================================================
 *  DMA buffers
 *
 *  Index space: [0, RxBufferCount) are the miniport's fixed RX pool and can
 *  never be freed by a program; the rest are runtime allocations for ring
 *  shapes the fixed pool cannot express.
 * ===========================================================================
 */

static ULONG
LabFixedCount(VOID)
{
    return (gLab.Adapter != NULL) ? gLab.Adapter->RxBufferCount : 0;
}

static VOID
LabFreeRuntimeDma(VOID)
{
    ULONG i;

    for (i = 0; i < gLab.RuntimeCount; ++i)
    {
        if (gLab.Runtime[i].VirtualAddress == NULL)
            continue;

        if (gLab.Runtime[i].FromNdis && gLab.Adapter != NULL)
        {
            NdisMFreeSharedMemory(gLab.Adapter->MiniportAdapterHandle,
                                  gLab.Runtime[i].Length,
                                  FALSE,
                                  gLab.Runtime[i].VirtualAddress,
                                  gLab.Runtime[i].PhysicalAddress);
        }
        else
        {
            MmFreeContiguousMemorySpecifyCache(gLab.Runtime[i].VirtualAddress,
                                               gLab.Runtime[i].Length,
                                               MmNonCached);
        }

        gLab.Runtime[i].VirtualAddress = NULL;
    }

    gLab.RuntimeCount = 0;
    gLab.RuntimeBytes = 0;
}

static ULONG
LabAllocRuntimeDma(_In_ ULONG Count, _In_ ULONG Size)
{
    ULONG i;
    ULONG First = gLab.RuntimeCount;

    for (i = 0; i < Count; ++i)
    {
        PAR9485LAB_DMA Slot = &gLab.Runtime[gLab.RuntimeCount];

        RtlZeroMemory(Slot, sizeof(*Slot));
        Slot->Length = Size;

        if (gLab.Adapter != NULL)
        {
            NdisMAllocateSharedMemory(gLab.Adapter->MiniportAdapterHandle,
                                      Size,
                                      FALSE,
                                      &Slot->VirtualAddress,
                                      &Slot->PhysicalAddress);
            Slot->FromNdis = TRUE;
        }
        else
        {
            PHYSICAL_ADDRESS Low, High, Boundary;

            Low.QuadPart = 0;
            /* The AR9485 descriptor fields this feeds are 32-bit, and the
             * fixed RX pool already refuses anything above 4 GiB. */
            High.QuadPart = 0xFFFFFFFF;
            Boundary.QuadPart = 0;

            Slot->VirtualAddress =
                MmAllocateContiguousMemorySpecifyCache(Size, Low, High,
                                                       Boundary, MmNonCached);
            if (Slot->VirtualAddress != NULL)
                Slot->PhysicalAddress = MmGetPhysicalAddress(Slot->VirtualAddress);
            Slot->FromNdis = FALSE;
        }

        if (Slot->VirtualAddress == NULL || Slot->PhysicalAddress.HighPart != 0)
        {
            if (Slot->VirtualAddress != NULL)
            {
                /* Above 4 GiB is useless to this chip; give it straight back
                 * rather than hand a script a descriptor it cannot program. */
                if (Slot->FromNdis)
                    NdisMFreeSharedMemory(gLab.Adapter->MiniportAdapterHandle,
                                          Size, FALSE, Slot->VirtualAddress,
                                          Slot->PhysicalAddress);
                else
                    MmFreeContiguousMemorySpecifyCache(Slot->VirtualAddress,
                                                       Size, MmNonCached);
                Slot->VirtualAddress = NULL;
            }
            return MAXULONG;
        }

        RtlZeroMemory(Slot->VirtualAddress, Size);
        ++gLab.RuntimeCount;
        gLab.RuntimeBytes += Size;
    }

    return LabFixedCount() + First;
}

/* ===========================================================================
 *  Quiescing the chip
 *
 *  Called before the lab takes over and again on the way out.  Masking the
 *  interrupt mask register and blocking RX is what keeps the chip from
 *  writing into memory the driver is about to free.
 * ===========================================================================
 */

static VOID
LabQuiesceHardware(VOID)
{
    PVOID Base;
    ULONG Length;

    if (LabScratchTarget())
        return;

    Base = LabTargetBase(&Length);
    if (Base == NULL)
        return;

    LabWrite32(Base, AR_IMR, 0);
    LabWrite32(Base, AR_DIAG_SW,
               LabRead32(Base, AR_DIAG_SW) | AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);
    LabWrite32(Base, AR_CR, AR_CR_RXD);
}

/* ===========================================================================
 *  The LAB_TARGET the portable interpreter runs against
 *
 *  lab_vm.c holds the opcode logic and knows nothing about NDIS or this chip.
 *  Everything platform-specific -- the BAR, the RX pool, NDIS shared memory,
 *  PCI config, the ath9k helpers -- is bound here.
 * ===========================================================================
 */

static ULONG
LabTargetRead32(PVOID Context, ULONG Offset)
{
    PVOID Base;
    ULONG Length;

    UNREFERENCED_PARAMETER(Context);

    Base = LabTargetBase(&Length);
    if (Base == NULL)
        return 0;
    return LabRead32(Base, Offset);
}

static VOID
LabTargetWrite32(PVOID Context, ULONG Offset, ULONG Value)
{
    PVOID Base;
    ULONG Length;

    UNREFERENCED_PARAMETER(Context);

    Base = LabTargetBase(&Length);
    if (Base != NULL)
        LabWrite32(Base, Offset, Value);
}

static VOID
LabTargetStall(PVOID Context, ULONG Microseconds)
{
    UNREFERENCED_PARAMETER(Context);
    LabStall(Microseconds);
}

static VOID
LabTargetBarrier(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeMemoryBarrier();
}

static ULONG
LabTargetDmaCount(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return LabFixedCount() + gLab.RuntimeCount;
}

static BOOLEAN
LabTargetDmaBuffer(
    PVOID Context,
    ULONG Index,
    PUCHAR *VirtualAddress,
    PULONG Length,
    PULONG PhysicalLow,
    PULONG Flags)
{
    ULONG Fixed = LabFixedCount();

    UNREFERENCED_PARAMETER(Context);

    if (Index < Fixed)
    {
        if (gLab.Adapter->RxBuffers[Index].VirtualAddress == NULL)
            return FALSE;
        *VirtualAddress = gLab.Adapter->RxBuffers[Index].VirtualAddress;
        *Length = AR9485_RX_BUFFER_SIZE;
        *PhysicalLow = gLab.Adapter->RxBuffers[Index].PhysicalAddress.LowPart;
        *Flags = AR9485LAB_DMA_FIXED;
        return TRUE;
    }

    Index -= Fixed;
    if (Index >= gLab.RuntimeCount ||
        gLab.Runtime[Index].VirtualAddress == NULL)
    {
        return FALSE;
    }

    *VirtualAddress = gLab.Runtime[Index].VirtualAddress;
    *Length = gLab.Runtime[Index].Length;
    *PhysicalLow = gLab.Runtime[Index].PhysicalAddress.LowPart;
    *Flags = AR9485LAB_DMA_RUNTIME;
    return TRUE;
}

static ULONG
LabTargetDmaAlloc(PVOID Context, ULONG Count, ULONG Size)
{
    UNREFERENCED_PARAMETER(Context);

    /* The interpreter enforces the per-op ceilings; the pool-wide ones are
     * ours because only we know what is already allocated. */
    if (gLab.RuntimeCount + Count > AR9485LAB_MAX_DMA_ALLOC)
        return MAXULONG;
    if (gLab.RuntimeBytes + Count * Size > AR9485LAB_MAX_DMA_ALLOC_TOTAL)
        return MAXULONG;

    return LabAllocRuntimeDma(Count, Size);
}

static VOID
LabTargetDmaFreeAll(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    /* Stop the chip before the memory it may be pointed at goes away. */
    LabQuiesceHardware();
    LabFreeRuntimeDma();
}

static ULONG
LabTargetPciAccess(
    PVOID Context,
    BOOLEAN Write,
    ULONG Offset,
    ULONG Width,
    PULONG Value)
{
    ULONG Bytes;

    UNREFERENCED_PARAMETER(Context);

    if (gLab.Adapter == NULL)
        return AR9485LAB_ST_UNSUPPORTED;

    if (Write)
    {
        Bytes = NdisWritePciSlotInformation(gLab.Adapter->MiniportAdapterHandle,
                                            0, Offset, Value, Width);
    }
    else
    {
        Bytes = NdisReadPciSlotInformation(gLab.Adapter->MiniportAdapterHandle,
                                           0, Offset, Value, Width);
    }

    return (Bytes == Width) ? AR9485LAB_ST_OK : AR9485LAB_ST_UNSUPPORTED;
}

static ULONG
LabTargetCallHw(PVOID Context, ULONG Function, ULONG Argument, PULONG Value)
{
    UNREFERENCED_PARAMETER(Context);

    if (gLab.Adapter == NULL)
        return AR9485LAB_ST_UNSUPPORTED;

    switch (Function)
    {
        case AR9485LAB_HW_START:
        {
            USHORT Channel = (USHORT)(Argument ? Argument
                                               : AR9485_DEFAULT_CHANNEL_MHZ);

            *Value = ar9485_hw_start(gLab.Adapter->HwContext,
                                     gLab.Adapter->IoBase,
                                     gLab.Adapter->IoLength,
                                     gLab.Adapter->DeviceId,
                                     gLab.Adapter->MacVersion,
                                     (USHORT)gLab.Adapter->MacRevision,
                                     Channel) ? 1 : 0;
            if (*Value)
                gLab.Adapter->CurrentChannelMHz = Channel;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_HW_QUEUE_RX:
            AR9485LabQueueRx(gLab.Adapter);
            *Value = 1;
            return AR9485LAB_ST_OK;

        case AR9485LAB_HW_HARVEST:
            *Value = AR9485LabHarvestRx(gLab.Adapter,
                                        Argument ? Argument
                                                 : gLab.Adapter->CurrentChannelMHz);
            return AR9485LAB_ST_OK;

        case AR9485LAB_HW_LOG:
        {
            PUCHAR Va;
            ULONG Len, Pa, Flags;

            if (!LabTargetDmaBuffer(NULL, Argument, &Va, &Len, &Pa, &Flags))
                return AR9485LAB_ST_UNSUPPORTED;
            *Value = AR9485LogCopy(Va, Len);
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_HW_STATE:
            *Value = ((ULONG)gLab.Adapter->MlmeState & 0xff) |
                     ((gLab.Adapter->AuthResponseSeen != 0) << 8) |
                     ((gLab.Adapter->AssocResponseSeen != 0) << 9) |
                     ((gLab.Adapter->DeauthSeen != 0) << 10) |
                     ((gLab.Adapter->RxArmed != 0) << 11) |
                     ((gLab.Adapter->TxReady != 0) << 12) |
                     ((gLab.Adapter->TxPending & 0xff) << 16) |
                     ((gLab.Adapter->ScanInProgress != 0) << 24);
            return AR9485LAB_ST_OK;

        default:
            return AR9485LAB_ST_UNSUPPORTED;
    }
}

static VOID
LabBuildTarget(_Out_ PLAB_TARGET Target)
{
    ULONG Length;

    RtlZeroMemory(Target, sizeof(*Target));
    (VOID)LabTargetBase(&Length);

    Target->Context = &gLab;
    Target->WindowLength = Length;
    Target->Read32 = LabTargetRead32;
    Target->Write32 = LabTargetWrite32;
    Target->Stall = LabTargetStall;
    Target->Barrier = LabTargetBarrier;
    Target->DmaCount = LabTargetDmaCount;
    Target->DmaBuffer = LabTargetDmaBuffer;
    Target->DmaAlloc = LabTargetDmaAlloc;
    Target->DmaFreeAll = LabTargetDmaFreeAll;
    Target->PciAccess = LabTargetPciAccess;
    Target->CallHw = LabTargetCallHw;
}

/* ===========================================================================
 *  Claim / release
 * ===========================================================================
 */

static VOID
LabFillInfo(_Out_ PAR9485LAB_INFO Info, _In_opt_ PFILE_OBJECT FileObject)
{
    RtlZeroMemory(Info, sizeof(*Info));
    Info->AbiVersion = AR9485LAB_ABI_VERSION;

    if (gLab.Owner != NULL)
    {
        Info->Flags |= AR9485LAB_INFO_CLAIMED;
        if (gLab.Owner == FileObject)
            Info->Flags |= AR9485LAB_INFO_CLAIMED_BY_ME;
    }

    if (LabScratchTarget())
    {
        ULONG Length;

        Info->Flags |= AR9485LAB_INFO_SCRATCH_TARGET;
        (VOID)LabTargetBase(&Length);
        Info->BarLength = Length;
        Info->DmaRuntimeCount = gLab.RuntimeCount;
        return;
    }

    Info->BarPhysicalLow = gLab.Adapter->IoAddress.LowPart;
    Info->BarPhysicalHigh = gLab.Adapter->IoAddress.HighPart;
    Info->BarLength = gLab.Adapter->IoLength;
    Info->DmaFixedCount = gLab.Adapter->RxBufferCount;
    Info->DmaFixedSize = AR9485_RX_BUFFER_SIZE;
    Info->DmaRuntimeCount = gLab.RuntimeCount;
    Info->SregRaw = gLab.Adapter->SregRaw;
    Info->MacVersion = gLab.Adapter->MacVersion;
    Info->MacRevision = gLab.Adapter->MacRevision;
    Info->CurrentChannelMHz = gLab.Adapter->CurrentChannelMHz;
    Info->DeviceId = gLab.Adapter->DeviceId;
    RtlCopyMemory(Info->PermanentMacAddress,
                  gLab.Adapter->PermanentMacAddress,
                  sizeof(Info->PermanentMacAddress));
    if (gLab.Adapter->PhyUp)
        Info->Flags |= AR9485LAB_INFO_PHY_UP;
}

static NTSTATUS
LabClaim(_In_ PFILE_OBJECT FileObject)
{
    if (gLab.Owner == FileObject)
        return STATUS_SUCCESS;
    if (gLab.Owner != NULL)
        return STATUS_DEVICE_BUSY;

    /* Set the owner BEFORE draining, so AR9485StartScan refuses the next
     * request while this one is still finishing. */
    gLab.Owner = FileObject;

    /* Take the chip away from the scan path before touching a register.  A
     * scan in flight is resetting the PHY across thirteen channels; an
     * experiment racing that measures nothing.  A full scan is thirteen
     * 150 ms dwells, so ten seconds is generous -- and bounded, because an
     * unbounded wait here would make the IOCTL unkillable if the scan
     * worker ever wedged. */
    if (gLab.Adapter != NULL &&
        InterlockedCompareExchange(&gLab.Adapter->ChipBusy, 0, 0))
    {
        LARGE_INTEGER Timeout;
        NTSTATUS WaitStatus;

        Timeout.QuadPart = -100000000LL;    /* 10 s */
        WaitStatus = KeWaitForSingleObject(&gLab.Adapter->ChipIdleEvent,
                                           Executive, KernelMode, FALSE,
                                           &Timeout);
        if (WaitStatus != STATUS_SUCCESS)
        {
            DPRINT1("AR9485: lab claim abandoned, scan did not drain\n");
            gLab.Owner = NULL;
            return STATUS_DEVICE_BUSY;
        }
    }

    DPRINT1("AR9485: lab claimed the chip\n");
    return STATUS_SUCCESS;
}

static VOID
LabReleaseClaim(_In_ PFILE_OBJECT FileObject)
{
    if (gLab.Owner != FileObject)
        return;

    /* Order matters: stop the chip, then free what it was pointed at. */
    LabQuiesceHardware();
    LabFreeRuntimeDma();

    if (gLab.Adapter != NULL && gLab.Adapter->HwContext != NULL &&
        !(gLab.Adapter->Flags & AR9485_FLAG_HALTING))
    {
        /* Hand back a chip in a defined state rather than whatever the last
         * experiment left behind. */
        if (ar9485_hw_start(gLab.Adapter->HwContext,
                            gLab.Adapter->IoBase,
                            gLab.Adapter->IoLength,
                            gLab.Adapter->DeviceId,
                            gLab.Adapter->MacVersion,
                            (USHORT)gLab.Adapter->MacRevision,
                            AR9485_DEFAULT_CHANNEL_MHZ))
        {
            gLab.Adapter->CurrentChannelMHz = AR9485_DEFAULT_CHANNEL_MHZ;
        }
    }

    gLab.Owner = NULL;
    DPRINT1("AR9485: lab released the chip\n");
}

/* ===========================================================================
 *  Dispatch
 * ===========================================================================
 */

static NTSTATUS NTAPI
LabDispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
LabDispatchCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    UNREFERENCED_PARAMETER(DeviceObject);

    /* A lab tool that crashed mid-experiment must not leave scans blocked or
     * the chip mastering into buffers nobody owns any more. */
    LabAcquire();
    LabReleaseClaim(IrpSp->FileObject);
    LabRelease();

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
LabDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG Code = IrpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG InLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG Written = 0;
    PUCHAR Program;
    LAB_TARGET Target;

    UNREFERENCED_PARAMETER(DeviceObject);

    LabAcquire();

    switch (Code)
    {
        case IOCTL_AR9485LAB_INFO:
        {
            if (OutLength < sizeof(AR9485LAB_INFO))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            LabFillInfo((PAR9485LAB_INFO)Buffer, IrpSp->FileObject);
            Written = sizeof(AR9485LAB_INFO);
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_AR9485LAB_CLAIM:
        {
            Status = LabClaim(IrpSp->FileObject);
            if (NT_SUCCESS(Status) && OutLength >= sizeof(AR9485LAB_INFO))
            {
                LabFillInfo((PAR9485LAB_INFO)Buffer, IrpSp->FileObject);
                Written = sizeof(AR9485LAB_INFO);
            }
            break;
        }

        case IOCTL_AR9485LAB_RELEASE:
        {
            LabReleaseClaim(IrpSp->FileObject);
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_AR9485LAB_RUN:
        {
            if (gLab.Owner != IrpSp->FileObject)
            {
                Status = STATUS_INVALID_DEVICE_STATE;
                break;
            }
            if (InLength > AR9485LAB_MAX_PROGRAM_BYTES ||
                OutLength > AR9485LAB_MAX_RESULT_BYTES)
            {
                Status = STATUS_INVALID_BUFFER_SIZE;
                break;
            }
            if (InLength < sizeof(AR9485LAB_PROGRAM))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* METHOD_BUFFERED hands the same system buffer in both
             * directions, so the result header would land on top of the ops
             * still waiting to execute.  Run against a private copy. */
            Program = ExAllocatePoolWithTag(NonPagedPool, InLength, AR9485_TAG);
            if (Program == NULL)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            RtlCopyMemory(Program, Buffer, InLength);
            LabBuildTarget(&Target);
            switch (LabVmRun(&Target, Program, InLength,
                             (PUCHAR)Buffer, OutLength, &Written))
            {
                case AR9485LAB_VM_OK:
                    Status = STATUS_SUCCESS;
                    break;
                case AR9485LAB_VM_ABI_MISMATCH:
                    Status = STATUS_REVISION_MISMATCH;
                    break;
                case AR9485LAB_VM_OUT_TOO_SMALL:
                    Status = STATUS_BUFFER_TOO_SMALL;
                    break;
                default:
                    Status = STATUS_INVALID_PARAMETER;
                    break;
            }
            ExFreePoolWithTag(Program, AR9485_TAG);
            break;
        }

        default:
            break;
    }

    LabRelease();

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Written : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* ===========================================================================
 *  Lifetime
 * ===========================================================================
 */

NDIS_STATUS
AR9485LabCreateControlDevice(_In_ NDIS_HANDLE MiniportDriverHandle)
{
    NDIS_DEVICE_OBJECT_ATTRIBUTES Attributes;
    PDRIVER_DISPATCH Dispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
    NDIS_STRING DeviceName = RTL_CONSTANT_STRING(AR9485LAB_DEVICE_NAME);
    NDIS_STRING SymbolicName = RTL_CONSTANT_STRING(AR9485LAB_SYMBOLIC_NAME);
    UNICODE_STRING Sddl = RTL_CONSTANT_STRING(AR9485LAB_SDDL);
    PDEVICE_OBJECT DeviceObject = NULL;
    NDIS_STATUS Status;

    if (gLabInitialized)
        return NDIS_STATUS_SUCCESS;

    RtlZeroMemory(&gLab, sizeof(gLab));
    KeInitializeMutex(&gLab.Lock, 0);

    RtlZeroMemory(Dispatch, sizeof(Dispatch));
    Dispatch[IRP_MJ_CREATE] = LabDispatchCreateClose;
    Dispatch[IRP_MJ_CLOSE] = LabDispatchCreateClose;
    Dispatch[IRP_MJ_CLEANUP] = LabDispatchCleanup;
    Dispatch[IRP_MJ_DEVICE_CONTROL] = LabDispatchDeviceControl;

    RtlZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Attributes.Header.Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    Attributes.Header.Size = sizeof(NDIS_DEVICE_OBJECT_ATTRIBUTES);
    Attributes.DeviceName = &DeviceName;
    Attributes.SymbolicName = &SymbolicName;
    Attributes.MajorFunctions = Dispatch;
    Attributes.ExtensionSize = 0;
    Attributes.DefaultSDDLString = &Sddl;
    Attributes.DeviceClassGuid = NULL;

    Status = NdisRegisterDeviceEx(MiniportDriverHandle,
                                  &Attributes,
                                  &DeviceObject,
                                  &gLab.DeviceHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: lab NdisRegisterDeviceEx failed 0x%08x\n", Status);
        return Status;
    }

    gLab.DeviceObject = DeviceObject;
    gLabInitialized = TRUE;
    DPRINT1("AR9485: lab control device \\Device\\AR9485Lab created\n");
    return NDIS_STATUS_SUCCESS;
}

VOID
AR9485LabDeleteControlDevice(VOID)
{
    if (!gLabInitialized)
        return;

    LabAcquire();
    LabQuiesceHardware();
    LabFreeRuntimeDma();
    gLab.Owner = NULL;
    if (gLab.Scratch != NULL)
    {
        ExFreePoolWithTag(gLab.Scratch, AR9485_TAG);
        gLab.Scratch = NULL;
        gLab.ScratchLength = 0;
    }
    LabRelease();

    if (gLab.DeviceHandle != NULL)
    {
        NdisDeregisterDeviceEx(gLab.DeviceHandle);
        gLab.DeviceHandle = NULL;
    }
    gLab.DeviceObject = NULL;
    gLabInitialized = FALSE;
}

VOID
AR9485LabAttachAdapter(_In_ PAR9485_ADAPTER Adapter)
{
    if (!gLabInitialized)
        return;

    LabAcquire();
    gLab.Adapter = Adapter;
    LabRelease();
}

VOID
AR9485LabDetachAdapter(_In_ PAR9485_ADAPTER Adapter)
{
    if (!gLabInitialized)
        return;

    LabAcquire();
    if (gLab.Adapter == Adapter)
    {
        /* The BAR mapping and the RX pool are about to go away, so stop the
         * chip and drop every runtime buffer while they are still valid. */
        LabQuiesceHardware();
        LabFreeRuntimeDma();
        gLab.Owner = NULL;
        gLab.Adapter = NULL;
    }
    LabRelease();
}

BOOLEAN
AR9485LabOwnsChip(VOID)
{
    /* Read without the mutex on purpose: this is called from the scan path,
     * and a claim that lands a microsecond later is handled by the drain in
     * LabClaim(), not by this check. */
    return (gLabInitialized && gLab.Owner != NULL);
}

#endif /* AR9485_LAB */
