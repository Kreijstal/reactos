/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     MiniportInitializeEx / HaltEx.
 *
 * Initializes the AX210-family transport and Native Wi-Fi scan interface:
 *
 *   read PCI config -> match hw/devices.c -> map BAR0 -> mask interrupts
 *   -> prepare-card-hw -> APM init -> read CSR_HW_REV / CSR_HW_RF_ID
 *   -> locate and load firmware/PNVM -> query NVM and permanent MAC
 *   -> configure regulatory/MAC/link scan contexts -> register with NDIS.
 *
 * Association and the data path build on the same persistent firmware and
 * RX queues; initialization succeeds only after those queues and the real
 * permanent address are available.
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
IwlSetGeneralAttributes(_In_ PIWL_ADAPTER Adapter);
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
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParameters;
    NDIS_TIMER_CHARACTERISTICS TimerCharacteristics;

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
    NdisAllocateSpinLock(&Adapter->TxLock);
    NdisZeroMemory(&PoolParameters, sizeof(PoolParameters));
    PoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParameters.Header.Revision =
        NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParameters.Header.Size = sizeof(PoolParameters);
    PoolParameters.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    PoolParameters.fAllocateNetBuffer = TRUE;
    PoolParameters.PoolTag = IWL_TAG;
    Adapter->RxNblPool = NdisAllocateNetBufferListPool(
        NdisMiniportHandle, &PoolParameters);
    if (Adapter->RxNblPool == NULL)
    {
        Status = NDIS_STATUS_RESOURCES;
        goto Fail;
    }
    NdisZeroMemory(&TimerCharacteristics, sizeof(TimerCharacteristics));
    TimerCharacteristics.Header.Type = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
    TimerCharacteristics.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
    TimerCharacteristics.Header.Size =
        NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1;
    TimerCharacteristics.TimerFunction = IwlRxPollTimerDpc;
    TimerCharacteristics.FunctionContext = Adapter;
    Status = NdisAllocateTimerObject(NdisMiniportHandle,
                                     &TimerCharacteristics,
                                     &Adapter->RxPollTimer);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

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

    /* Before ownership only CSR_INT_MASK is safe to touch.  In particular,
     * acknowledging FH_INT_STATUS while an AX211 is still in D0U can wedge
     * its MMIO path.  This matches iwl_trans_pcie_start_fw(): prepare the
     * card first, then clear latched interrupt state. */
    DPRINT1("iwlwifi: masking interrupts before ownership\n");
    IwlWrite32(Adapter, CSR_INT_MASK, 0);
    DPRINT1("iwlwifi: interrupt mask written, preparing hardware\n");

    Status = IwlPrepareCardHw(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    IwlDisableInterrupts(Adapter);

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

    Status = IwlGen3AllocateFirmwareDma(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    Status = IwlRegisterInterrupt(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("iwlwifi: interrupt registration failed 0x%08x\n", Status);
        goto Fail;
    }

    Status = IwlGen3StartFirmware(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    if (!Adapter->MacAddressValid)
    {
        DPRINT1("iwlwifi: firmware started without a valid permanent MAC\n");
        Status = NDIS_STATUS_INVALID_DATA;
        goto Fail;
    }

    Adapter->CurrentOperationMode =
        DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
    Adapter->DesiredBssType = dot11_BSS_type_infrastructure;
    Adapter->AuthenticationAlgorithm = DOT11_AUTH_ALGO_80211_OPEN;
    Adapter->UnicastCipher = DOT11_CIPHER_ALGO_NONE;
    Adapter->MulticastCipher = DOT11_CIPHER_ALGO_NONE;
    Status = IwlInitializeScan(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;
    Status = IwlSetGeneralAttributes(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

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

    DPRINT1("iwlwifi: Native Wi-Fi miniport ready, initial scan has %lu BSSes\n",
            Adapter->BssCount);
    return NDIS_STATUS_SUCCESS;

Fail:
    IwlCleanupAdapter(Adapter);
    if (Adapter->RxPollTimer != NULL)
        NdisFreeTimerObject(Adapter->RxPollTimer);
    if (Adapter->RxNblPool != NULL)
        NdisFreeNetBufferListPool(Adapter->RxNblPool);
    NdisFreeSpinLock(&Adapter->TxLock);
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
    if (Adapter->RxPollTimer != NULL)
        NdisFreeTimerObject(Adapter->RxPollTimer);
    if (Adapter->RxNblPool != NULL)
        NdisFreeNetBufferListPool(Adapter->RxNblPool);
    NdisFreeSpinLock(&Adapter->TxLock);
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
    if (Adapter->RxPollTimer != NULL)
    {
        InterlockedExchange(&Adapter->DataPathReady, 0);
        NdisCancelTimerObject(Adapter->RxPollTimer);
    }
    IwlShutdownScan(Adapter);

    if (Adapter->InterruptHandle != NULL)
        IwlUnregisterInterrupt(Adapter);

    IwlFreePnvm(Adapter);
    IwlGen3FreeFirmwareDma(Adapter);
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

static NDIS_STATUS
IwlSetGeneralAttributes(_In_ PIWL_ADAPTER Adapter)
{
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES Attributes;
    NdisZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.Header.Type =
        NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    Attributes.Header.Revision =
        NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    Attributes.Header.Size = sizeof(Attributes);
    Attributes.MediaType = NdisMediumNative802_11;
    Attributes.PhysicalMediumType = NdisPhysicalMediumNative802_11;
    Attributes.MtuSize = 1500;
    Attributes.MaxXmitLinkSpeed = 2400000000ULL;
    Attributes.MaxRcvLinkSpeed = 2400000000ULL;
    Attributes.XmitLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    Attributes.RcvLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    Attributes.MediaConnectState = MediaConnectStateDisconnected;
    Attributes.MediaDuplexState = MediaDuplexStateUnknown;
    Attributes.LookaheadSize = 2304;
    Attributes.MacOptions = NDIS_MAC_OPTION_NO_LOOPBACK |
                            NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                            NDIS_MAC_OPTION_RECEIVE_SERIALIZED;
    Attributes.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED |
                                        NDIS_PACKET_TYPE_BROADCAST |
                                        NDIS_PACKET_TYPE_MULTICAST;
    Attributes.MaxMulticastListSize = 32;
    Attributes.MacAddressLength = IWL_MAC_ADDRESS_LENGTH;
    NdisMoveMemory(Attributes.PermanentMacAddress,
                   Adapter->PermanentMacAddress, IWL_MAC_ADDRESS_LENGTH);
    NdisMoveMemory(Attributes.CurrentMacAddress,
                   Adapter->CurrentMacAddress, IWL_MAC_ADDRESS_LENGTH);
    Attributes.AccessType = NET_IF_ACCESS_BROADCAST;
    Attributes.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    Attributes.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    Attributes.IfType = IF_TYPE_IEEE80211;
    Attributes.IfConnectorPresent = TRUE;
    Attributes.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
    Attributes.SupportedOidList = (PNDIS_OID)IwlSupportedOids;
    Attributes.SupportedOidListLength =
        IwlSupportedOidCount * sizeof(NDIS_OID);

    return NdisMSetMiniportAttributes(Adapter->MiniportAdapterHandle,
        (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&Attributes);
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
