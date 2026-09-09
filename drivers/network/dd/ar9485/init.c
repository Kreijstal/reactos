/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     MiniportInitializeEx / HaltEx.  Maps BAR0, reads AR_SREV
 *              for chip-revision identification, brings the RTC domain
 *              out of reset, recovers the permanent MAC address from the
 *              card's EEPROM/OTP, and reports general attributes to NDIS.
 */

#include "ar9485.h"
#include "ath9k/hw_min.h"

#define NDEBUG
#include <debug.h>

static NDIS_STATUS
AR9485SetRegistrationAttributes(_In_ PAR9485_ADAPTER Adapter);
static NDIS_STATUS
AR9485SetGeneralAttributes(_In_ PAR9485_ADAPTER Adapter);
static NDIS_STATUS
AR9485MapHardwareResources(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ PNDIS_RESOURCE_LIST ResourceList);
static VOID
AR9485DetectChip(_In_ PAR9485_ADAPTER Adapter);
static NDIS_STATUS
AR9485ReadMacAddress(_In_ PAR9485_ADAPTER Adapter);

NDIS_STATUS NTAPI
AR9485MiniportInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    PAR9485_ADAPTER Adapter;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(MiniportDriverContext);

    DPRINT1("AR9485: MiniportInitializeEx, handle=%p\n", NdisMiniportHandle);

    Adapter = NdisAllocateMemoryWithTagPriority(NdisMiniportHandle,
                                                sizeof(AR9485_ADAPTER),
                                                AR9485_TAG,
                                                NormalPoolPriority);
    if (Adapter == NULL)
    {
        DPRINT1("AR9485: out of memory allocating adapter context\n");
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

    Status = AR9485SetRegistrationAttributes(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: SetRegistrationAttributes failed 0x%08x\n", Status);
        goto Fail;
    }

    if (MiniportInitParameters->AllocatedResources == NULL)
    {
        DPRINT1("AR9485: no PnP resources allocated\n");
        Status = NDIS_STATUS_RESOURCES;
        goto Fail;
    }

    Status = AR9485MapHardwareResources(Adapter,
                                        MiniportInitParameters->AllocatedResources);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: MapHardwareResources failed 0x%08x\n", Status);
        goto Fail;
    }

    AR9485DetectChip(Adapter);

    if (Adapter->MacVersion != AR_SREV_VERSION_9485)
    {
        /* Phase 1a stayed bound on a mismatch so the readback could be
         * logged from bare metal.  From Phase 2a on we drive the chip -
         * power-on reset, OTP state machine - so a part we cannot
         * identify must not be touched at all. */
        DPRINT1("AR9485: unexpected chip-version 0x%03x (want 0x%03x); "
                "refusing to drive an unidentified part\n",
                Adapter->MacVersion, AR_SREV_VERSION_9485);
        Status = NDIS_STATUS_ADAPTER_NOT_FOUND;
        goto Fail;
    }

    Adapter->Flags |= AR9485_FLAG_HW_RECOGNIZED;

    Status = AR9485ReadMacAddress(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: ReadMacAddress failed 0x%08x\n", Status);
        goto Fail;
    }

    /* Phase 3b: bring the PHY up.  ar9485_hw_start() runs the upstream
     * reset sequence - wake, reset, PLL, AR9485 1.1 initvals, synthesiser,
     * baseband activate - and leaves the radio parked on the default
     * channel.  The context outlives the call because the receive path
     * reads the current channel back out of it. */
    Adapter->HwContext = NdisAllocateMemoryWithTagPriority(NdisMiniportHandle,
                                                           (UINT)ar9485_hw_context_size(),
                                                           AR9485_TAG,
                                                           NormalPoolPriority);
    if (Adapter->HwContext == NULL)
    {
        DPRINT1("AR9485: out of memory allocating hardware context\n");
        Status = NDIS_STATUS_RESOURCES;
        goto Fail;
    }

    /* Recover the EEPROM/OTP image into the context once, before the
     * first bring-up.  ar9485_hw_start() needs it on every reset to apply
     * the board's analog configuration (PA bias, RF switch, attenuation)
     * and carries it across the reattach each channel change performs, so
     * the expensive OTP walk happens exactly here.  A card whose EEPROM
     * will not read is not fatal: the PHY still comes up on initval
     * defaults, which is what every boot before this change ran on, and
     * ar9485_hw_eeprom_restore() has already said why. */
    if (!ar9485_hw_context_init(Adapter->HwContext,
                                Adapter->IoBase,
                                Adapter->IoLength,
                                Adapter->DeviceId,
                                Adapter->MacVersion,
                                (USHORT)Adapter->MacRevision))
    {
        DPRINT1("AR9485: continuing without EEPROM board values\n");
    }

    if (!ar9485_hw_start(Adapter->HwContext,
                         Adapter->IoBase,
                         Adapter->IoLength,
                         Adapter->DeviceId,
                         Adapter->MacVersion,
                         (USHORT)Adapter->MacRevision,
                         AR9485_DEFAULT_CHANNEL_MHZ))
    {
        /* ar9485_hw_start() has already named the failing step. */
        Status = NDIS_STATUS_FAILURE;
        goto Fail;
    }

    Adapter->PhyUp = TRUE;
    Adapter->CurrentChannelMHz = AR9485_DEFAULT_CHANNEL_MHZ;

    Status = AR9485RegisterInterrupt(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: RegisterInterrupt failed 0x%08x\n", Status);
        goto Fail;
    }

    Status = AR9485SetGeneralAttributes(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: SetGeneralAttributes failed 0x%08x\n", Status);
        goto Fail;
    }

    Status = AR9485InitializeReceiver(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: InitializeReceiver failed 0x%08x\n", Status);
        goto Fail;
    }

    Status = AR9485InitializeTransmitter(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: InitializeTransmitter failed 0x%08x\n", Status);
        goto Fail;
    }

    /* Until an upper layer asks for something else, the station is an open
     * one: nwifi replaces all three before it issues a connect request. */
    Adapter->AuthAlgorithm = DOT11_AUTH_ALGO_80211_OPEN;
    Adapter->UnicastCipher = DOT11_CIPHER_ALGO_NONE;
    Adapter->MulticastCipher = DOT11_CIPHER_ALGO_NONE;

    /* ar9485_hw_start() brings the PHY up but leaves the MAC without an
     * address, the DCUs mapped to no queue and the transmit ring unarmed;
     * see AR9485ProgramMacState(). */
    AR9485ProgramMacState(Adapter);
    AR9485ArmReceiver(Adapter);

    Status = AR9485StartChipWorker(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: StartChipWorker failed 0x%08x\n", Status);
        goto Fail;
    }

    DPRINT1("AR9485: initialized: SREV=0x%08x version=0x%03x rev=%u "
            "MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            Adapter->SregRaw, Adapter->MacVersion, Adapter->MacRevision,
            Adapter->PermanentMacAddress[0], Adapter->PermanentMacAddress[1],
            Adapter->PermanentMacAddress[2], Adapter->PermanentMacAddress[3],
            Adapter->PermanentMacAddress[4], Adapter->PermanentMacAddress[5]);

    /* Publish the adapter to the bring-up lab only now: everything it hands a
     * script -- BAR mapping, RX pool, hw context -- has to be valid first. */
    AR9485LabAttachAdapter(Adapter);

    return NDIS_STATUS_SUCCESS;

Fail:
    AR9485StopChipWorker(Adapter);
    AR9485ShutdownTransmitter(Adapter);
    AR9485ShutdownReceiver(Adapter);
    if (Adapter->InterruptHandle != NULL)
        AR9485UnregisterInterrupt(Adapter);
    if (Adapter->HwContext != NULL)
    {
        NdisFreeMemory(Adapter->HwContext,
                       (UINT)ar9485_hw_context_size(),
                       0);
        Adapter->HwContext = NULL;
    }

    if (Adapter->IoBase != NULL)
        MmUnmapIoSpace(Adapter->IoBase, Adapter->IoLength);
    NdisFreeMemory(Adapter, sizeof(*Adapter), 0);
    return Status;
}

VOID NTAPI
AR9485MiniportHaltEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PAR9485_ADAPTER Adapter = (PAR9485_ADAPTER)MiniportAdapterContext;

    UNREFERENCED_PARAMETER(HaltAction);

    if (Adapter == NULL)
        return;

    DPRINT1("AR9485: MiniportHaltEx\n");
    InterlockedOr(&Adapter->Flags, AR9485_FLAG_HALTING);

    /* The chip worker touches the BAR, the DMA rings and the adapter itself,
     * so it has to be gone before any of them are. */
    AR9485StopChipWorker(Adapter);

    /* Before anything is freed: the lab stops the chip and drops its runtime
     * DMA while the BAR and the RX pool are still mapped.  Doing this after
     * ShutdownReceiver would leave the chip mastering into freed pages. */
    AR9485LabDetachAdapter(Adapter);

    AR9485ShutdownTransmitter(Adapter);
    AR9485ShutdownReceiver(Adapter);

    if (Adapter->InterruptHandle != NULL)
        AR9485UnregisterInterrupt(Adapter);
    if (Adapter->HwContext != NULL)
    {
        NdisFreeMemory(Adapter->HwContext,
                       (UINT)ar9485_hw_context_size(),
                       0);
        Adapter->HwContext = NULL;
    }

    if (Adapter->IoBase != NULL)
    {
        MmUnmapIoSpace(Adapter->IoBase, Adapter->IoLength);
        Adapter->IoBase = NULL;
    }
    NdisFreeMemory(Adapter, sizeof(*Adapter), 0);
}

NDIS_STATUS NTAPI
AR9485MiniportPause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(PauseParameters);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS NTAPI
AR9485MiniportRestart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RestartParameters);
    return NDIS_STATUS_SUCCESS;
}

VOID NTAPI
AR9485MiniportDevicePnPEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    PAR9485_ADAPTER Adapter = (PAR9485_ADAPTER)MiniportAdapterContext;

    if (NetDevicePnPEvent->DevicePnPEvent == NdisDevicePnPEventSurpriseRemoved && Adapter != NULL)
        InterlockedOr(&Adapter->Flags, AR9485_FLAG_HALTING);

    DPRINT1("AR9485: PnP event %d\n", NetDevicePnPEvent->DevicePnPEvent);
}

VOID NTAPI
AR9485MiniportShutdownEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ShutdownAction);
    DPRINT1("AR9485: MiniportShutdownEx\n");
}

static NDIS_STATUS
AR9485SetRegistrationAttributes(_In_ PAR9485_ADAPTER Adapter)
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

static NDIS_STATUS
AR9485SetGeneralAttributes(_In_ PAR9485_ADAPTER Adapter)
{
    /* Phase 1a: report enough for NDIS to accept the miniport.  Full
     * 802.11 capability advertisement (PHY types, auth/cipher pairs,
     * BSS list, regulatory domain) lands with Phase 2-3. */
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES GenAttrs;
    NDIS_OID SupportedOids[] = {
        OID_GEN_HARDWARE_STATUS,
        OID_GEN_MEDIA_SUPPORTED,
        OID_GEN_MEDIA_IN_USE,
        OID_GEN_MAXIMUM_FRAME_SIZE,
        OID_GEN_LINK_SPEED,
        OID_GEN_TRANSMIT_BUFFER_SPACE,
        OID_GEN_RECEIVE_BUFFER_SPACE,
        OID_GEN_TRANSMIT_BLOCK_SIZE,
        OID_GEN_RECEIVE_BLOCK_SIZE,
        OID_GEN_VENDOR_ID,
        OID_GEN_VENDOR_DESCRIPTION,
        OID_GEN_VENDOR_DRIVER_VERSION,
        OID_GEN_CURRENT_PACKET_FILTER,
        OID_GEN_CURRENT_LOOKAHEAD,
        OID_GEN_DRIVER_VERSION,
        OID_GEN_MAXIMUM_TOTAL_SIZE,
        OID_GEN_MAC_OPTIONS,
        OID_GEN_MEDIA_CONNECT_STATUS,
        OID_GEN_INTERRUPT_MODERATION,
        OID_GEN_PHYSICAL_MEDIUM,
        OID_GEN_STATISTICS,
        OID_DOT11_MAC_ADDRESS,
        OID_DOT11_PERMANENT_ADDRESS,
        OID_DOT11_CURRENT_ADDRESS,
        OID_DOT11_OPERATION_MODE_CAPABILITY,
        OID_DOT11_CURRENT_OPERATION_MODE,
        OID_DOT11_SCAN_REQUEST,
        OID_DOT11_ENUM_BSS_LIST,
        OID_DOT11_DESIRED_SSID_LIST,
        OID_DOT11_DESIRED_BSSID_LIST,
        OID_DOT11_DESIRED_BSS_TYPE,
        OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM,
        OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM,
        OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM,
        OID_DOT11_CURRENT_PACKET_FILTER,
        OID_DOT11_CONNECT_REQUEST,
        OID_DOT11_DISCONNECT_REQUEST,
        OID_DOT11_CIPHER_DEFAULT_KEY,
        OID_DOT11_CIPHER_DEFAULT_KEY_ID,
        OID_DOT11_CIPHER_KEY_MAPPING_KEY,
    };

    NdisZeroMemory(&GenAttrs, sizeof(GenAttrs));
    GenAttrs.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    GenAttrs.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    GenAttrs.Header.Size     = sizeof(NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES);

    /* Native 802.11 medium.  Until the full DOT11 OID surface is wired up,
     * NDIS will see this as a powered-down wireless interface. */
    GenAttrs.MediaType                = NdisMediumNative802_11;
    GenAttrs.PhysicalMediumType       = NdisPhysicalMediumNative802_11;
    GenAttrs.MtuSize                  = 1500;
    GenAttrs.MaxXmitLinkSpeed         = 150000000ULL;   /* 150 Mbps */
    GenAttrs.MaxRcvLinkSpeed          = 150000000ULL;
    GenAttrs.XmitLinkSpeed            = NDIS_LINK_SPEED_UNKNOWN;
    GenAttrs.RcvLinkSpeed             = NDIS_LINK_SPEED_UNKNOWN;
    GenAttrs.MediaConnectState        = MediaConnectStateDisconnected;
    GenAttrs.MediaDuplexState         = MediaDuplexStateUnknown;
    GenAttrs.LookaheadSize            = 2304;
    GenAttrs.PowerManagementCapabilities = NULL;
    GenAttrs.MacOptions               = NDIS_MAC_OPTION_NO_LOOPBACK |
                                        NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                                        NDIS_MAC_OPTION_RECEIVE_SERIALIZED;
    GenAttrs.SupportedPacketFilters   = NDIS_PACKET_TYPE_DIRECTED |
                                        NDIS_PACKET_TYPE_BROADCAST |
                                        NDIS_PACKET_TYPE_MULTICAST;
    GenAttrs.MaxMulticastListSize     = 32;
    GenAttrs.MacAddressLength         = AR9485_MAC_ADDRESS_LENGTH;

    /* Phase 2a: the permanent address comes out of the card's EEPROM/OTP.
     * MiniportInitializeEx fails before it reaches this point if the
     * restore did not produce a usable one, so the assertion below records
     * the invariant rather than defending against it - there is no path
     * on which an all-zero or invented address is reported to NDIS. */
    NT_ASSERT(Adapter->MacAddressValid);
    NdisMoveMemory(GenAttrs.PermanentMacAddress,
                   Adapter->PermanentMacAddress,
                   AR9485_MAC_ADDRESS_LENGTH);
    NdisMoveMemory(GenAttrs.CurrentMacAddress,
                   Adapter->CurrentMacAddress,
                   AR9485_MAC_ADDRESS_LENGTH);

    GenAttrs.RecvScaleCapabilities    = NULL;
    GenAttrs.AccessType               = NET_IF_ACCESS_BROADCAST;
    GenAttrs.DirectionType            = NET_IF_DIRECTION_SENDRECEIVE;
    GenAttrs.ConnectionType           = NET_IF_CONNECTION_DEDICATED;
    GenAttrs.IfType                   = IF_TYPE_IEEE80211;
    GenAttrs.IfConnectorPresent       = TRUE;
    GenAttrs.SupportedStatistics      = 0;
    GenAttrs.SupportedPauseFunctions  = NdisPauseFunctionsUnsupported;
    GenAttrs.DataBackFillSize         = 0;
    GenAttrs.ContextBackFillSize      = 0;

    GenAttrs.SupportedOidList         = SupportedOids;
    GenAttrs.SupportedOidListLength   = sizeof(SupportedOids);

    return NdisMSetMiniportAttributes(Adapter->MiniportAdapterHandle,
        (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&GenAttrs);
}

static NDIS_STATUS
AR9485MapHardwareResources(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ PNDIS_RESOURCE_LIST ResourceList)
{
    ULONG i;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource;
    BOOLEAN FoundMemory = FALSE;
    BOOLEAN FoundInterrupt = FALSE;

    DPRINT1("AR9485: parsing %u PnP resources\n", ResourceList->Count);

    for (i = 0; i < ResourceList->Count; i++)
    {
        Resource = &ResourceList->PartialDescriptors[i];
        switch (Resource->Type)
        {
            case CmResourceTypeMemory:
                /* AR9485 exposes a single memory BAR.  Measured on real
                 * hardware (ASUS X550DP): PA 0xff900000, len 524288 = 512 KiB
                 * -- NOT the 64 KiB this comment used to claim.  The size
                 * matters: the OTP block the EEPROM path reads lives at
                 * 0x14000-0x15f1f, which a 64 KiB window would not cover. */
                if (FoundMemory)
                    break;
                Adapter->IoAddress = Resource->u.Memory.Start;
                Adapter->IoLength  = Resource->u.Memory.Length;
                Adapter->IoBase    = MmMapIoSpace(Adapter->IoAddress,
                                                  Adapter->IoLength,
                                                  MmNonCached);
                if (Adapter->IoBase == NULL)
                {
                    DPRINT1("AR9485: MmMapIoSpace failed for 0x%I64x len %u\n",
                            Adapter->IoAddress.QuadPart, Adapter->IoLength);
                    return NDIS_STATUS_RESOURCES;
                }
                FoundMemory = TRUE;
                DPRINT1("AR9485: mapped BAR at PA 0x%I64x len %u -> VA %p\n",
                        Adapter->IoAddress.QuadPart, Adapter->IoLength, Adapter->IoBase);
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
                    DPRINT1("AR9485: MSI vector %u level %u\n",
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
                    DPRINT1("AR9485: line-based IRQ vector %u level %u shared=%u\n",
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
        DPRINT1("AR9485: no memory BAR found in resource list\n");
        return NDIS_STATUS_RESOURCES;
    }
    if (!FoundInterrupt)
    {
        DPRINT1("AR9485: no interrupt resource found\n");
        return NDIS_STATUS_RESOURCES;
    }
    return NDIS_STATUS_SUCCESS;
}

static VOID
AR9485DetectChip(_In_ PAR9485_ADAPTER Adapter)
{
    u32  macVersion = 0;
    u16  macRev = 0;
    bool isPciExpress = false;
    bool ok;

    /* Slice 2 of the ath9k port: delegate chip-revision decode to the
     * verbatim ath9k_hw_read_revisions() body in ath9k/hw_chip.c.  The
     * AR9485 PCIe variant carries devid 0x0032; the upstream function
     * looks up the AR_SREV register offset, reads it via REG_READ, and
     * decodes the version / revision fields per the AR9300+ encoding. */
    ok = ar9485_read_revisions(Adapter->IoBase,
                               Adapter->IoLength,
                               AR9300_DEVID_AR9485_PCIE,
                               &macVersion, &macRev, &isPciExpress);
    if (!ok)
    {
        DPRINT1("AR9485: ath9k_hw_read_revisions failed\n");
        Adapter->SregRaw = 0;
        Adapter->MacVersion = 0;
        Adapter->MacRevision = 0;
        return;
    }

    Adapter->SregRaw     = AR9485_READ_REG(Adapter, AR9485_AR_SREV_OFFSET);
    Adapter->MacVersion  = macVersion;
    Adapter->MacRevision = macRev;

    DPRINT1("AR9485: AR_SREV raw=0x%08x version=0x%03x revision=%u pcie=%d\n",
            Adapter->SregRaw, Adapter->MacVersion,
            Adapter->MacRevision, isPciExpress ? 1 : 0);
}

/*
 * Phase 2a: bring the RTC domain out of reset and restore the EEPROM
 * image from the card's serial EEPROM or its on-die OTP, so the miniport
 * can report the card's real permanent MAC address.
 *
 * The AR9485 register file places the OTP controller at 0x14000-0x15f1f
 * (ath9k ar9003_eeprom.h:85-94), well above everything slices 1-2 touch,
 * so the mapped BAR has to be large enough to reach it.  On Windows the
 * mapping is exactly IoLength bytes long and a read past its end faults,
 * hence the explicit check here rather than a fault at OTP-probe time.
 */
static NDIS_STATUS
AR9485ReadMacAddress(_In_ PAR9485_ADAPTER Adapter)
{
    /* Highest register the OTP path touches: AR9300_OTP_READ_DATA at
     * 0x15f1c, plus its own four bytes.  A shorter window is not fatal -
     * the serial-EEPROM aperture at 0x2000 and the RTC block at 0x7000
     * still fit in 64 KiB, so a card with a real EEPROM would still be
     * readable - but it does mean the OTP probe can only fail, and that
     * is worth naming up front on the one boot we get to observe. */
    const ULONG OtpWindowEnd = 0x15f1c + sizeof(ULONG);

    if (Adapter->IoLength < OtpWindowEnd)
    {
        DPRINT1("AR9485: BAR0 window is 0x%x bytes, short of the 0x%x needed "
                "to reach the OTP controller; only the serial-EEPROM path can "
                "succeed\n",
                Adapter->IoLength, OtpWindowEnd);
    }

    if (!ar9485_hw_power_on(Adapter->IoBase,
                            Adapter->IoLength,
                            AR9300_DEVID_AR9485_PCIE,
                            Adapter->MacVersion,
                            (u16)Adapter->MacRevision))
    {
        DPRINT1("AR9485: power-on reset failed; the OTP state machine will "
                "not answer, so no MAC address can be read\n");
        return NDIS_STATUS_FAILURE;
    }

    if (!ar9485_hw_eeprom_get_macaddr(Adapter->IoBase,
                                      Adapter->IoLength,
                                      AR9300_DEVID_AR9485_PCIE,
                                      Adapter->MacVersion,
                                      (u16)Adapter->MacRevision,
                                      Adapter->PermanentMacAddress,
                                      &Adapter->EepromVersion,
                                      &Adapter->TemplateVersion))
    {
        /* ar9485_hw_eeprom_get_macaddr() has already named the failing
         * step on the debug port.  Deliberately no fallback: a plausible
         * but wrong MAC is worse than a miniport that does not start. */
        DPRINT1("AR9485: EEPROM/OTP MAC address recovery failed\n");
        return NDIS_STATUS_FAILURE;
    }

    NdisMoveMemory(Adapter->CurrentMacAddress,
                   Adapter->PermanentMacAddress,
                   AR9485_MAC_ADDRESS_LENGTH);
    Adapter->MacAddressValid = TRUE;

    DPRINT1("AR9485: permanent MAC %02x:%02x:%02x:%02x:%02x:%02x "
            "(eepromVersion=%u templateVersion=%u)\n",
            Adapter->PermanentMacAddress[0], Adapter->PermanentMacAddress[1],
            Adapter->PermanentMacAddress[2], Adapter->PermanentMacAddress[3],
            Adapter->PermanentMacAddress[4], Adapter->PermanentMacAddress[5],
            Adapter->EepromVersion, Adapter->TemplateVersion);
    return NDIS_STATUS_SUCCESS;
}
