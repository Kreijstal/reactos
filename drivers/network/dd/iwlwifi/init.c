/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     MiniportInitializeEx / HaltEx.
 *
 * Phase 1a runs the full identification chain and stops there:
 *
 *   read PCI config -> match hw/devices.c -> map BAR0 -> mask interrupts
 *   -> prepare-card-hw -> APM init -> read CSR_HW_REV / CSR_HW_RF_ID
 *   -> locate and parse the matching .ucode container
 *
 * and then DELIBERATELY FAILS.  A native-802.11 miniport with no permanent
 * address and no data path is not a network adapter, and presenting one to
 * NDIS would be worse than presenting none.  The permanent address on
 * family 9000 and later comes out of NVM through a firmware command, which
 * needs the ucode actually running - Phase 2's job.
 *
 * What Phase 1a is FOR is the log it produces: drop this driver on any
 * Intel wireless machine and it will say which part it is, whether the
 * transport powers up, and whether a usable firmware blob is installed.
 * That is the information every later phase depends on, and it is
 * collectable on hardware we do not otherwise have.
 */

/* This is the one translation unit that references
 * GUID_BUS_INTERFACE_STANDARD, so it is the one that emits the definition.
 * initguid.h must precede wdmguid.h, and both must precede iwlwifi.h -
 * which pulls wdmguid.h in again behind its include guard. */
#include <initguid.h>
#include <wdmguid.h>

#include "iwlwifi.h"

#define NDEBUG
#include <debug.h>

#define PCI_VENDOR_ID_INTEL     0x8086

static NDIS_STATUS
IwlSetRegistrationAttributes(_In_ PIWL_ADAPTER Adapter);
static NDIS_STATUS
IwlQueryBusInterface(_In_ PIWL_ADAPTER Adapter);
static NDIS_STATUS
IwlIdentifyDevice(_In_ PIWL_ADAPTER Adapter);
static NDIS_STATUS
IwlMapHardwareResources(
    _In_ PIWL_ADAPTER Adapter,
    _In_ PNDIS_RESOURCE_LIST ResourceList);
static VOID
IwlReadHardwareRevision(_In_ PIWL_ADAPTER Adapter);
static VOID
IwlCleanupAdapter(_In_ PIWL_ADAPTER Adapter);

NDIS_STATUS NTAPI
IwlMiniportInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    PIWL_ADAPTER Adapter;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(MiniportDriverContext);

    DPRINT1("iwlwifi: MiniportInitializeEx, handle=%p\n", NdisMiniportHandle);

    Adapter = NdisAllocateMemoryWithTagPriority(NdisMiniportHandle,
                                                sizeof(IWL_ADAPTER),
                                                IWL_TAG,
                                                NormalPoolPriority);
    if (Adapter == NULL)
    {
        DPRINT1("iwlwifi: out of memory allocating adapter context\n");
        return NDIS_STATUS_RESOURCES;
    }

    NdisZeroMemory(Adapter, sizeof(*Adapter));
    Adapter->MiniportAdapterHandle    = NdisMiniportHandle;
    Adapter->NdisMiniportDriverHandle = g_NdisMiniportDriverHandle;

    NdisMGetDeviceProperty(NdisMiniportHandle,
                           &Adapter->PhysicalDeviceObject,
                           NULL,
                           NULL,
                           NULL,
                           NULL);

    Status = IwlSetRegistrationAttributes(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("iwlwifi: SetRegistrationAttributes failed 0x%08x\n", Status);
        goto Fail;
    }

    Status = IwlQueryBusInterface(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    Status = IwlIdentifyDevice(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    if (MiniportInitParameters->AllocatedResources == NULL)
    {
        DPRINT1("iwlwifi: no PnP resources allocated\n");
        Status = NDIS_STATUS_RESOURCES;
        goto Fail;
    }

    Status = IwlMapHardwareResources(Adapter,
                                     MiniportInitParameters->AllocatedResources);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("iwlwifi: MapHardwareResources failed 0x%08x\n", Status);
        goto Fail;
    }

    /* Mask everything before the device is allowed to do anything.  The
     * line may be shared, and a part left in an odd state by platform
     * firmware can assert immediately. */
    IwlDisableInterrupts(Adapter);

    Status = IwlPrepareCardHw(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    /* A software reset from a known-owned state, then power up. */
    IwlSwReset(Adapter);

    Status = IwlApmInit(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    IwlReadHardwareRevision(Adapter);

    Adapter->Flags |= IWL_FLAG_HW_RECOGNIZED;

    Status = IwlLoadFirmware(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        /* IwlLoadFirmware() has already named the reason. */
        goto Fail;
    }

    if (Adapter->Cfg->Flags & IWL_CFG_NEEDS_PNVM)
    {
        Status = IwlLoadPnvm(Adapter);
        if (Status != NDIS_STATUS_SUCCESS)
            goto Fail;
    }

    DPRINT1("iwlwifi: ================ identification complete ============\n");
    DPRINT1("iwlwifi:   part      : %s (8086:%04x subsys %04x rev %02x)\n",
            Adapter->Cfg->Name, Adapter->DeviceId,
            Adapter->SubsystemId, Adapter->RevisionId);
    DPRINT1("iwlwifi:   family    : %s%s\n",
            IwlFamilyName(Adapter->Cfg->Family),
            (Adapter->Cfg->Flags & IWL_CFG_INTEGRATED) ? " (integrated CNVi)" : "");
    DPRINT1("iwlwifi:   HW_REV    : 0x%08x  type 0x%03x step %u dash %u\n",
            Adapter->HwRev, CSR_HW_REV_TYPE(Adapter->HwRev),
            Adapter->HwRevStep, Adapter->HwRevDash);
    if (Adapter->Cfg->Family >= IWL_DEVICE_FAMILY_9000)
    {
        DPRINT1("iwlwifi:   RF_ID     : 0x%08x  chip 0x%03x step %u dash %u\n",
                Adapter->HwRfId,
                CSR_HW_RF_ID_TYPE_CHIP_ID(Adapter->HwRfId),
                CSR_HW_RF_ID_TYPE_STEP(Adapter->HwRfId),
                CSR_HW_RF_ID_TYPE_DASH(Adapter->HwRfId));
    }
    DPRINT1("iwlwifi:   firmware  : %s\n", Adapter->FwName);
    if (Adapter->Cfg->Flags & IWL_CFG_NEEDS_PNVM)
    {
        DPRINT1("iwlwifi:   pnvm      : %s, %u SKU block(s) - the one that "
                "applies is chosen from the firmware's ALIVE response\n",
                Adapter->PnvmName, Adapter->PnvmParsed->BlockCount);
    }
    DPRINT1("iwlwifi: =====================================================\n");

    /*
     * Everything Phase 1a set out to prove is proven.  Refuse to expose an
     * adapter we cannot give a permanent address or a data path - see the
     * file header.  This is not an error path; it is the end of the phase.
     */
    DPRINT1("iwlwifi: Phase 1a stops here: no NVM address and no data path "
            "yet, so this part is NOT presented to NDIS as a usable "
            "adapter.  The identification above is the deliverable.\n");
    Status = NDIS_STATUS_NOT_SUPPORTED;

Fail:
    IwlCleanupAdapter(Adapter);
    NdisFreeMemory(Adapter, sizeof(*Adapter), 0);
    return Status;
}

VOID NTAPI
IwlMiniportHaltEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PIWL_ADAPTER Adapter = (PIWL_ADAPTER)MiniportAdapterContext;

    UNREFERENCED_PARAMETER(HaltAction);

    if (Adapter == NULL)
        return;

    DPRINT1("iwlwifi: MiniportHaltEx\n");
    InterlockedOr(&Adapter->Flags, IWL_FLAG_HALTING);

    IwlCleanupAdapter(Adapter);
    NdisFreeMemory(Adapter, sizeof(*Adapter), 0);
}

/*
 * Tear down in the exact reverse of the order things were brought up.  Safe
 * to call at any point in MiniportInitializeEx: every step is guarded by
 * the state it depends on, so a failure three steps in unwinds exactly the
 * three steps that ran.
 */
static VOID
IwlCleanupAdapter(_In_ PIWL_ADAPTER Adapter)
{
    if (Adapter->InterruptHandle != NULL)
        IwlUnregisterInterrupt(Adapter);

    IwlFreePnvm(Adapter);
    IwlFreeFirmware(Adapter);

    if (Adapter->IoBase != NULL)
    {
        IwlDisableInterrupts(Adapter);
        IwlApmStop(Adapter);
        MmUnmapIoSpace(Adapter->IoBase, Adapter->IoLength);
        Adapter->IoBase = NULL;
    }

    if (Adapter->BusInterfaceValid)
    {
        if (Adapter->BusInterface.InterfaceDereference != NULL)
            Adapter->BusInterface.InterfaceDereference(Adapter->BusInterface.Context);
        Adapter->BusInterfaceValid = FALSE;
    }
}

NDIS_STATUS NTAPI
IwlMiniportPause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(PauseParameters);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS NTAPI
IwlMiniportRestart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RestartParameters);
    return NDIS_STATUS_SUCCESS;
}

VOID NTAPI
IwlMiniportDevicePnPEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    PIWL_ADAPTER Adapter = (PIWL_ADAPTER)MiniportAdapterContext;

    if (NetDevicePnPEvent->DevicePnPEvent == NdisDevicePnPEventSurpriseRemoved &&
        Adapter != NULL)
    {
        InterlockedOr(&Adapter->Flags, IWL_FLAG_HALTING);
    }

    DPRINT1("iwlwifi: PnP event %d\n", NetDevicePnPEvent->DevicePnPEvent);
}

VOID NTAPI
IwlMiniportShutdownEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    PIWL_ADAPTER Adapter = (PIWL_ADAPTER)MiniportAdapterContext;

    UNREFERENCED_PARAMETER(ShutdownAction);
    DPRINT1("iwlwifi: MiniportShutdownEx\n");

    /* Leave the device quiet across the reboot rather than mid-DMA. */
    if (Adapter != NULL && Adapter->IoBase != NULL)
    {
        IwlDisableInterrupts(Adapter);
        IwlApmStop(Adapter);
    }
}

static NDIS_STATUS
IwlSetRegistrationAttributes(_In_ PIWL_ADAPTER Adapter)
{
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES RegAttrs;

    NdisZeroMemory(&RegAttrs, sizeof(RegAttrs));
    RegAttrs.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    RegAttrs.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    RegAttrs.Header.Size     = sizeof(RegAttrs);
    RegAttrs.MiniportAdapterContext = Adapter;
    RegAttrs.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE |
                              NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER |
                              NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK;
    RegAttrs.CheckForHangTimeInSeconds = 4;
    RegAttrs.InterfaceType = NdisInterfacePci;

    return NdisMSetMiniportAttributes(Adapter->MiniportAdapterHandle,
        (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&RegAttrs);
}

/*
 * Ask the PDO's bus driver for BUS_INTERFACE_STANDARD so PCI config space
 * can be read for THIS device.  Mandatory - see the note on the
 * BusInterface field in iwlwifi.h.
 */
static NDIS_STATUS
IwlQueryBusInterface(_In_ PIWL_ADAPTER Adapter)
{
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;
    IO_STATUS_BLOCK IoStatusBlock;
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT TargetDevice;

    if (Adapter->PhysicalDeviceObject == NULL)
    {
        DPRINT1("iwlwifi: no PDO; cannot read PCI config space\n");
        return NDIS_STATUS_FAILURE;
    }

    TargetDevice = IoGetAttachedDeviceReference(Adapter->PhysicalDeviceObject);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       TargetDevice,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &IoStatusBlock);
    if (Irp == NULL)
    {
        ObDereferenceObject(TargetDevice);
        return NDIS_STATUS_RESOURCES;
    }

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = IRP_MJ_PNP;
    IrpSp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    IrpSp->Parameters.QueryInterface.InterfaceType = &GUID_BUS_INTERFACE_STANDARD;
    IrpSp->Parameters.QueryInterface.Size = sizeof(BUS_INTERFACE_STANDARD);
    IrpSp->Parameters.QueryInterface.Version = 1;
    IrpSp->Parameters.QueryInterface.Interface = (PINTERFACE)&Adapter->BusInterface;
    IrpSp->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    Status = IoCallDriver(TargetDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatusBlock.Status;
    }

    ObDereferenceObject(TargetDevice);

    if (!NT_SUCCESS(Status) || Adapter->BusInterface.GetBusData == NULL)
    {
        DPRINT1("iwlwifi: QUERY_INTERFACE(BUS_INTERFACE_STANDARD) failed 0x%08x; "
                "refusing to guess the device identity\n", Status);
        return NDIS_STATUS_FAILURE;
    }

    Adapter->BusInterfaceValid = TRUE;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
IwlIdentifyDevice(_In_ PIWL_ADAPTER Adapter)
{
    PCI_COMMON_CONFIG PciConfig;
    ULONG BytesRead;

    NdisZeroMemory(&PciConfig, sizeof(PciConfig));

    BytesRead = Adapter->BusInterface.GetBusData(Adapter->BusInterface.Context,
                                                 PCI_WHICHSPACE_CONFIG,
                                                 &PciConfig,
                                                 0,
                                                 sizeof(PCI_COMMON_CONFIG));
    if (BytesRead < PCI_COMMON_HDR_LENGTH)
    {
        DPRINT1("iwlwifi: short PCI config read (%u bytes)\n", BytesRead);
        return NDIS_STATUS_FAILURE;
    }

    Adapter->VendorId    = PciConfig.VendorID;
    Adapter->DeviceId    = PciConfig.DeviceID;
    Adapter->SubsystemId = PciConfig.u.type0.SubSystemID;
    Adapter->RevisionId  = PciConfig.RevisionID;

    if (Adapter->VendorId != PCI_VENDOR_ID_INTEL)
    {
        DPRINT1("iwlwifi: bound to a non-Intel device %04x:%04x\n",
                Adapter->VendorId, Adapter->DeviceId);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    Adapter->Cfg = IwlLookupDevice(Adapter->DeviceId, Adapter->SubsystemId);
    if (Adapter->Cfg == NULL)
    {
        /* Deliberately fatal.  The bring-up sequence differs by family and
         * an unrecognised part must not be driven on the assumption that
         * it resembles a known one.  Adding it is a row in
         * hw/devices.c - which is exactly the intended way to grow
         * coverage. */
        DPRINT1("iwlwifi: unknown Intel wireless device 8086:%04x (subsys %04x). "
                "Add a row to hw/devices.c's IwlDeviceTable to support it.\n",
                Adapter->DeviceId, Adapter->SubsystemId);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    DPRINT1("iwlwifi: %s - 8086:%04x subsys %04x rev %02x, family %s\n",
            Adapter->Cfg->Name, Adapter->DeviceId, Adapter->SubsystemId,
            Adapter->RevisionId, IwlFamilyName(Adapter->Cfg->Family));
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
IwlMapHardwareResources(
    _In_ PIWL_ADAPTER Adapter,
    _In_ PNDIS_RESOURCE_LIST ResourceList)
{
    ULONG i;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource;
    BOOLEAN FoundMemory = FALSE;
    BOOLEAN FoundInterrupt = FALSE;

    DPRINT1("iwlwifi: parsing %u PnP resources\n", ResourceList->Count);

    for (i = 0; i < ResourceList->Count; i++)
    {
        Resource = &ResourceList->PartialDescriptors[i];
        switch (Resource->Type)
        {
            case CmResourceTypeMemory:
                /* Intel wireless parts expose a single memory BAR.  The CSR
                 * block is only the first 1 KiB of it; the rest is not
                 * reachable until the APM is up. */
                if (FoundMemory)
                    break;
                Adapter->IoAddress = Resource->u.Memory.Start;
                Adapter->IoLength  = Resource->u.Memory.Length;
                Adapter->IoBase    = MmMapIoSpace(Adapter->IoAddress,
                                                  Adapter->IoLength,
                                                  MmNonCached);
                if (Adapter->IoBase == NULL)
                {
                    DPRINT1("iwlwifi: MmMapIoSpace failed for 0x%I64x len %u\n",
                            Adapter->IoAddress.QuadPart, Adapter->IoLength);
                    return NDIS_STATUS_RESOURCES;
                }
                FoundMemory = TRUE;
                DPRINT1("iwlwifi: mapped BAR at PA 0x%I64x len %u -> VA %p\n",
                        Adapter->IoAddress.QuadPart, Adapter->IoLength,
                        Adapter->IoBase);
                break;

            case CmResourceTypeInterrupt:
                if (Resource->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
                {
                    Adapter->InterruptVector   = Resource->u.MessageInterrupt.Translated.Vector;
                    Adapter->InterruptLevel    = (KIRQL)Resource->u.MessageInterrupt.Translated.Level;
                    Adapter->InterruptAffinity = Resource->u.MessageInterrupt.Translated.Affinity;
                    Adapter->InterruptModeType = Latched;
                    Adapter->InterruptShared   = FALSE;
                    Adapter->HasMessageInterrupt = TRUE;
                    DPRINT1("iwlwifi: MSI vector %u level %u\n",
                            Adapter->InterruptVector, Adapter->InterruptLevel);
                }
                else
                {
                    Adapter->InterruptVector   = Resource->u.Interrupt.Vector;
                    Adapter->InterruptLevel    = (KIRQL)Resource->u.Interrupt.Level;
                    Adapter->InterruptAffinity = Resource->u.Interrupt.Affinity;
                    Adapter->InterruptModeType = (Resource->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                                 Latched : LevelSensitive;
                    Adapter->InterruptShared   = (Resource->ShareDisposition == CmResourceShareShared);
                    Adapter->HasMessageInterrupt = FALSE;
                    DPRINT1("iwlwifi: line-based IRQ vector %u level %u shared=%u\n",
                            Adapter->InterruptVector, Adapter->InterruptLevel,
                            Adapter->InterruptShared);
                }
                FoundInterrupt = TRUE;
                break;

            default:
                break;
        }
    }

    if (!FoundMemory)
    {
        DPRINT1("iwlwifi: no memory BAR found in resource list\n");
        return NDIS_STATUS_RESOURCES;
    }
    if (!FoundInterrupt)
    {
        DPRINT1("iwlwifi: no interrupt resource found\n");
        return NDIS_STATUS_RESOURCES;
    }

    /* The CSR block must at minimum be mapped, or every register access
     * below reads off the end of the window. */
    if (Adapter->IoLength < 0x1000)
    {
        DPRINT1("iwlwifi: BAR window of %u bytes is too small for the CSR block\n",
                Adapter->IoLength);
        return NDIS_STATUS_RESOURCES;
    }

    return NDIS_STATUS_SUCCESS;
}

static VOID
IwlReadHardwareRevision(_In_ PIWL_ADAPTER Adapter)
{
    Adapter->HwRev = IwlRead32(Adapter, CSR_HW_REV);

    /*
     * From family 8000 on, the step and dash live in CSR_HW_REV_WA_REG
     * rather than in the low bits of CSR_HW_REV, which read as zero.
     * Reading them from the wrong place yields a plausible-looking A-step
     * for every part, which is the sort of wrong answer that survives a
     * long time before anyone questions it.
     */
    if (Adapter->Cfg->Family >= IWL_DEVICE_FAMILY_8000)
    {
        ULONG Wa = IwlRead32(Adapter, CSR_HW_REV_WA_REG);

        Adapter->HwRevStep = (Wa >> 2) & 0x3;
        Adapter->HwRevDash = Wa & 0x3;
    }
    else
    {
        Adapter->HwRevStep = CSR_HW_REV_STEP(Adapter->HwRev);
        Adapter->HwRevDash = CSR_HW_REV_DASH(Adapter->HwRev);
    }

    if (Adapter->Cfg->Family >= IWL_DEVICE_FAMILY_9000)
        Adapter->HwRfId = IwlRead32(Adapter, CSR_HW_RF_ID);
}
