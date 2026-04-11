/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/pnp.c
 * PURPOSE:          NTFS filesystem driver - IRP_MJ_PNP handler
 *
 * The I/O Manager sends IRP_MJ_PNP to the volume device for things like
 * IRP_MN_QUERY_DEVICE_RELATIONS / TargetDeviceRelation, which the Plug
 * and Play manager uses (via IopGetRelatedTargetDevice in
 * ntoskrnl/io/iomgr/device.c) to walk from a file object back to its
 * underlying physical device. The PnP manager asserts that the response
 * is non-NULL on success — see the ASSERT(DeviceRelations) at line 680
 * — so we cannot just acknowledge the IRP with STATUS_SUCCESS without
 * filling in the out parameters; that crashes text-mode setup as soon
 * as it tries to enumerate disks.
 *
 * fastfat handles a small set of removal-related minor codes itself
 * (QUERY_REMOVE_DEVICE / SURPRISE_REMOVAL / REMOVE_DEVICE /
 * CANCEL_REMOVE_DEVICE) and forwards every other minor code down to
 * the underlying disk driver via IoSkipCurrentIrpStackLocation +
 * IoCallDriver. We mirror that here. The disk driver knows the answers
 * to relations queries, capability queries, etc., and can fill in the
 * IRP correctly.
 *
 * The control device (NtfsGlobalData->DeviceObject) has no underlying
 * storage to forward to, so PNP IRPs that arrive on it (which mostly
 * shouldn't happen) get STATUS_NOT_SUPPORTED.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * Forward a PNP IRP down to the underlying storage device. After this
 * returns the IRP is owned by the disk driver (or already completed by
 * it). The caller must NOT touch the IRP afterwards and must clear
 * IRPCONTEXT_COMPLETE so NtfsDispatch doesn't try to complete it again.
 */
static
NTSTATUS
NtfsForwardPnpIrp(PNTFS_VCB Vcb,
                  PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Vcb->StorageDevice, Irp);
}

/*
 * Mark the volume so subsequent CREATE / READ / WRITE will refuse and
 * the FS knows the underlying device is going away. This is the bare
 * minimum response to a removal request — fastfat does much more
 * (volume lock, dismount, VPB swap) but for now we at least flag the
 * VCB and let the PnP manager / disk driver deal with the rest.
 */
static
VOID
NtfsMarkVolumeForDismount(PNTFS_VCB Vcb)
{
    Vcb->Flags |= VCB_DISMOUNT_PENDING;
}

NTSTATUS
NtfsPnp(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PDEVICE_OBJECT DeviceObject = IrpContext->DeviceObject;
    PNTFS_VCB Vcb;
    NTSTATUS Status;

    DPRINT("NtfsPnp(%p): MinorFunction %u\n", DeviceObject, Stack->MinorFunction);

    /* PNP doesn't apply to the control device — it has no underlying
     * storage to forward to, and the I/O Manager normally doesn't send
     * PNP IRPs there anyway. */
    if (DeviceObject == NtfsGlobalData->DeviceObject)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_NOT_SUPPORTED;
    }

    /* Validate that this is one of our volume device objects. */
    if (DeviceObject->DeviceExtension == NULL)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Vcb = (PNTFS_VCB)DeviceObject->DeviceExtension;
    if (Vcb->Identifier.Type != NTFS_TYPE_VCB)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    switch (Stack->MinorFunction)
    {
        case IRP_MN_QUERY_REMOVE_DEVICE:
            /* The PnP manager is asking permission to remove the
             * underlying device. We don't yet implement the volume
             * lock / open-handle check that fastfat does, but for a
             * fixed-disk install this code path effectively never
             * fires. Forward the question to the disk driver — it
             * will refuse if it has good reason. */
            Status = NtfsForwardPnpIrp(Vcb, Irp);
            IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;
            return Status;

        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            /* Mark our VCB so further file operations refuse, then
             * forward to the disk driver. After this point the device
             * is gone whether we like it or not. */
            NtfsMarkVolumeForDismount(Vcb);
            Status = NtfsForwardPnpIrp(Vcb, Irp);
            IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;
            return Status;

        case IRP_MN_CANCEL_REMOVE_DEVICE:
            /* Removal was cancelled. Clear the dismount-pending flag
             * we set in the prior QUERY_REMOVE and forward so the disk
             * driver also knows. */
            Vcb->Flags &= ~VCB_DISMOUNT_PENDING;
            Status = NtfsForwardPnpIrp(Vcb, Irp);
            IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;
            return Status;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            /* IopGetRelatedTargetDevice sends this with type
             * TargetDeviceRelation to walk from a file back to its
             * physical device. The disk driver fills in the
             * DEVICE_RELATIONS for itself, so we must forward — not
             * complete with STATUS_SUCCESS, which would leave
             * DeviceRelations NULL and trip the assertion that broke
             * the install before. */
            Status = NtfsForwardPnpIrp(Vcb, Irp);
            IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;
            return Status;

        default:
            /* Everything else (capability queries, ID queries, lock
             * queries, etc.) is the disk driver's business. Forward. */
            Status = NtfsForwardPnpIrp(Vcb, Irp);
            IrpContext->Flags &= ~IRPCONTEXT_COMPLETE;
            return Status;
    }
}

/* EOF */
