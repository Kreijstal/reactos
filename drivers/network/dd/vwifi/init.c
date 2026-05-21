/*
 * PROJECT:     ReactOS vwifi virtual Wi-Fi miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     DriverEntry and adapter initialization for the software-only
 *              NDIS 6.20 Native-802.11 miniport.
 *
 * vwifi pretends to be a real Wi-Fi adapter so the upper stack (wlansvc,
 * wlanapi, ndisuio, DOT11 OID dispatch, eventual WPA2-PSK 4-way handshake)
 * can be exercised in QEMU without any silicon. No PCI, no IRQ, no DMA.
 */

#include "vwifi.h"

#define NDEBUG
#include <debug.h>

NDIS_HANDLE g_VwifiDriverHandle = NULL;

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Chars;
    NDIS_STATUS Status;

    DPRINT1("vwifi: DriverEntry\n");

    NdisZeroMemory(&Chars, sizeof(Chars));
    Chars.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    Chars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    Chars.Header.Size     = sizeof(Chars);

    Chars.MajorNdisVersion   = NDIS_MINIPORT_MAJOR_VERSION;
    Chars.MinorNdisVersion   = NDIS_MINIPORT_MINOR_VERSION;
    Chars.MajorDriverVersion = 1;
    Chars.MinorDriverVersion = 0;

    Chars.InitializeHandlerEx       = (MINIPORT_INITIALIZE *)&VwifiMiniportInitializeEx;
    Chars.HaltHandlerEx             = (MINIPORT_HALT *)&VwifiMiniportHaltEx;
    Chars.UnloadHandler             = (MINIPORT_UNLOAD *)&VwifiMiniportDriverUnload;
    Chars.PauseHandler              = (MINIPORT_PAUSE *)&VwifiMiniportPause;
    Chars.RestartHandler            = (MINIPORT_RESTART *)&VwifiMiniportRestart;
    Chars.OidRequestHandler         = (MINIPORT_OID_REQUEST *)&VwifiOidRequest;
    Chars.SendNetBufferListsHandler = (MINIPORT_SEND_NET_BUFFER_LISTS *)&VwifiSendNetBufferLists;
    Chars.ReturnNetBufferListsHandler = (MINIPORT_RETURN_NET_BUFFER_LISTS *)&VwifiReturnNetBufferLists;
    Chars.CancelSendHandler         = (MINIPORT_CANCEL_SEND *)&VwifiCancelSend;
    Chars.DevicePnPEventNotifyHandler = (MINIPORT_DEVICE_PNP_EVENT_NOTIFY *)&VwifiPnPEventNotify;
    Chars.ShutdownHandlerEx         = (MINIPORT_SHUTDOWN *)&VwifiShutdownEx;
    Chars.CancelOidRequestHandler   = (MINIPORT_CANCEL_OID_REQUEST *)&VwifiCancelOidRequest;

    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Chars, &g_VwifiDriverHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("vwifi: NdisMRegisterMiniportDriver failed 0x%08x\n", Status);
        return Status;
    }

    DPRINT1("vwifi: registered NDIS miniport, handle=%p\n", g_VwifiDriverHandle);
    return STATUS_SUCCESS;
}

VOID
NTAPI
VwifiMiniportDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_VwifiDriverHandle)
    {
        NdisMDeregisterMiniportDriver(g_VwifiDriverHandle);
        g_VwifiDriverHandle = NULL;
    }
    DPRINT1("vwifi: unloaded\n");
}

VOID
VwifiPopulateCannedBssList(_In_ PVWIFI_ADAPTER A)
{
    /* Three canned BSS entries: open AP, WPA2-PSK AP, hidden SSID.
     * Bssids are locally-administered (first byte's 2nd-bit set). */
    static const UCHAR Bssids[3][6] =
    {
        {0x02,0x76,0x77,0x69,0x66,0x01},
        {0x02,0x76,0x77,0x69,0x66,0x02},
        {0x02,0x76,0x77,0x69,0x66,0x03},
    };
    static const char *Names[3] = { "vwifi-open", "vwifi-wpa2", "" };
    static const BOOLEAN Privacy[3] = { FALSE, TRUE, TRUE };
    static const SHORT Rssi[3]      = { -50, -65, -80 };
    static const ULONG Chan[3]      = { 6, 11, 1 };
    ULONG i, n;

    NdisZeroMemory(A->Bss, sizeof(A->Bss));
    A->BssCount = 3;
    for (i = 0; i < 3; i++)
    {
        PVWIFI_BSS_ENTRY e = &A->Bss[i];
        NdisMoveMemory(e->Bssid, Bssids[i], 6);
        n = (ULONG)strlen(Names[i]);
        e->SsidLen = (UCHAR)n;
        if (n) NdisMoveMemory(e->Ssid, Names[i], n);
        e->PhyType = dot11_phy_type_erp;
        /* 2.4 GHz channel center: 2407 + 5 * ch */
        e->ChCenterFreqMHz = 2407 + 5 * Chan[i];
        e->RssiDbm = Rssi[i];
        e->BeaconPeriodTu = VWIFI_BEACON_PERIOD_TU;
        e->PrivacyOn = Privacy[i];
    }
}

static NDIS_STATUS
VwifiSetRegistrationAttributes(_In_ PVWIFI_ADAPTER A)
{
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES Reg;

    NdisZeroMemory(&Reg, sizeof(Reg));
    Reg.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    Reg.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    Reg.Header.Size     = sizeof(Reg);
    Reg.MiniportAdapterContext = A;
    Reg.AttributeFlags  = NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK |
                          NDIS_MINIPORT_ATTRIBUTES_NDIS_WDM;
    Reg.CheckForHangTimeInSeconds = 4;
    Reg.InterfaceType   = NdisInterfaceInternal;
    return NdisMSetMiniportAttributes(A->MiniportAdapterHandle,
                                      (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&Reg);
}

static NDIS_STATUS
VwifiSetGeneralAttributes(_In_ PVWIFI_ADAPTER A)
{
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES Gen;

    NdisZeroMemory(&Gen, sizeof(Gen));
    Gen.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    Gen.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    Gen.Header.Size     = sizeof(Gen);

    Gen.MediaType         = NdisMediumNative802_11;
    Gen.PhysicalMediumType = NdisPhysicalMediumNative802_11;
    Gen.MtuSize           = 1500;
    Gen.MaxXmitLinkSpeed  = 54000000;    /* 54 Mbit/s legacy 802.11g */
    Gen.MaxRcvLinkSpeed   = 54000000;
    Gen.XmitLinkSpeed     = NDIS_LINK_SPEED_UNKNOWN;
    Gen.RcvLinkSpeed      = NDIS_LINK_SPEED_UNKNOWN;
    Gen.MediaConnectState = MediaConnectStateDisconnected;
    Gen.MediaDuplexState  = MediaDuplexStateHalf;
    Gen.LookaheadSize     = 1514;

    Gen.MacOptions        = NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                            NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                            NDIS_MAC_OPTION_NO_LOOPBACK;
    Gen.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED |
                                 NDIS_PACKET_TYPE_MULTICAST |
                                 NDIS_PACKET_TYPE_ALL_MULTICAST |
                                 NDIS_PACKET_TYPE_BROADCAST |
                                 NDIS_PACKET_TYPE_PROMISCUOUS;
    Gen.MaxMulticastListSize = 32;
    Gen.MacAddressLength  = 6;
    NdisMoveMemory(Gen.PermanentMacAddress, A->PermanentMac, 6);
    NdisMoveMemory(Gen.CurrentMacAddress, A->CurrentMac, 6);

    Gen.SupportedStatistics = NDIS_STATISTICS_XMIT_OK_SUPPORTED |
                              NDIS_STATISTICS_RCV_OK_SUPPORTED |
                              NDIS_STATISTICS_BYTES_RCV_SUPPORTED |
                              NDIS_STATISTICS_BYTES_XMIT_SUPPORTED |
                              NDIS_STATISTICS_GEN_STATISTICS_SUPPORTED;
    Gen.AccessType        = NET_IF_ACCESS_BROADCAST;
    Gen.DirectionType     = NET_IF_DIRECTION_SENDRECEIVE;
    Gen.ConnectionType    = NET_IF_CONNECTION_DEDICATED;
    Gen.IfType            = IF_TYPE_IEEE80211;
    Gen.IfConnectorPresent = FALSE;        /* virtual antenna */
    Gen.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
    Gen.SupportedOidList     = NULL;
    Gen.SupportedOidListLength = 0;

    return NdisMSetMiniportAttributes(A->MiniportAdapterHandle,
                                      (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&Gen);
}

NDIS_STATUS
NTAPI
VwifiMiniportInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE DriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS InitParams)
{
    PVWIFI_ADAPTER A;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(DriverContext);
    UNREFERENCED_PARAMETER(InitParams);

    DPRINT1("vwifi: MiniportInitializeEx\n");

    A = NdisAllocateMemoryWithTagPriority(NdisMiniportHandle,
                                          sizeof(VWIFI_ADAPTER),
                                          VWIFI_TAG, NormalPoolPriority);
    if (!A)
        return NDIS_STATUS_RESOURCES;

    NdisZeroMemory(A, sizeof(*A));
    A->MiniportAdapterHandle    = NdisMiniportHandle;
    A->NdisMiniportDriverHandle = g_VwifiDriverHandle;
    NdisAllocateSpinLock(&A->Lock);

    A->PermanentMac[0] = VWIFI_DEFAULT_MAC0;
    A->PermanentMac[1] = VWIFI_DEFAULT_MAC1;
    A->PermanentMac[2] = VWIFI_DEFAULT_MAC2;
    A->PermanentMac[3] = VWIFI_DEFAULT_MAC3;
    A->PermanentMac[4] = VWIFI_DEFAULT_MAC4;
    A->PermanentMac[5] = VWIFI_DEFAULT_MAC5;
    NdisMoveMemory(A->CurrentMac, A->PermanentMac, 6);

    A->CurrentPhyType   = dot11_phy_type_erp;
    A->RadioOn          = TRUE;
    A->CurrentChannel   = VWIFI_DEFAULT_CHANNEL;
    A->PacketFilter     = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST;

    VwifiPopulateCannedBssList(A);

    Status = VwifiSetRegistrationAttributes(A);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("vwifi: registration attrs failed 0x%08x\n", Status);
        goto Fail;
    }
    Status = VwifiSetGeneralAttributes(A);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("vwifi: general attrs failed 0x%08x\n", Status);
        goto Fail;
    }

    NdisMGetDeviceProperty(NdisMiniportHandle, &A->PhysicalDeviceObject,
                           NULL, NULL, NULL, NULL);

    DPRINT1("vwifi: adapter initialized: mac %02x:%02x:%02x:%02x:%02x:%02x, "
            "%u canned BSSes\n",
            A->CurrentMac[0], A->CurrentMac[1], A->CurrentMac[2],
            A->CurrentMac[3], A->CurrentMac[4], A->CurrentMac[5],
            A->BssCount);

    /* Self-indicate one DOT11_SCAN_CONFIRM at adapter bring-up so the
     * producer / 60thunk_rx-observer wiring is exercised once per init,
     * without needing a userspace OID_DOT11_SCAN_REQUEST.  Safe to call
     * after NdisMSetMiniportAttributes has returned success. */
    {
        NDIS_STATUS_INDICATION Indication;
        NDIS_STATUS ScanStatus = NDIS_STATUS_SUCCESS;

        NdisZeroMemory(&Indication, sizeof(Indication));
        Indication.Header.Type      = NDIS_OBJECT_TYPE_STATUS_INDICATION;
        Indication.Header.Revision  = NDIS_STATUS_INDICATION_REVISION_1;
        Indication.Header.Size      = sizeof(NDIS_STATUS_INDICATION);
        Indication.SourceHandle     = A->MiniportAdapterHandle;
        Indication.StatusCode       = NDIS_STATUS_DOT11_SCAN_CONFIRM;
        Indication.StatusBuffer     = &ScanStatus;
        Indication.StatusBufferSize = sizeof(ScanStatus);

        DPRINT1("vwifi: init - self-indicating DOT11_SCAN_CONFIRM\n");
        NdisMIndicateStatusEx(A->MiniportAdapterHandle, &Indication);
    }
    return NDIS_STATUS_SUCCESS;

Fail:
    NdisFreeSpinLock(&A->Lock);
    NdisFreeMemory(A, sizeof(*A), 0);
    return Status;
}

VOID
NTAPI
VwifiMiniportHaltEx(_In_ NDIS_HANDLE Ctx, _In_ NDIS_HALT_ACTION Action)
{
    PVWIFI_ADAPTER A = (PVWIFI_ADAPTER)Ctx;

    UNREFERENCED_PARAMETER(Action);
    DPRINT1("vwifi: HaltEx\n");
    if (A)
    {
        NdisFreeSpinLock(&A->Lock);
        NdisFreeMemory(A, sizeof(*A), 0);
    }
}

NDIS_STATUS
NTAPI
VwifiMiniportPause(_In_ NDIS_HANDLE Ctx, _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS Params)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Params);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
VwifiMiniportRestart(_In_ NDIS_HANDLE Ctx, _In_ PNDIS_MINIPORT_RESTART_PARAMETERS Params)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Params);
    return NDIS_STATUS_SUCCESS;
}

VOID
NTAPI
VwifiPnPEventNotify(_In_ NDIS_HANDLE Ctx, _In_ PNET_DEVICE_PNP_EVENT Event)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Event);
}

VOID
NTAPI
VwifiShutdownEx(_In_ NDIS_HANDLE Ctx, _In_ NDIS_SHUTDOWN_ACTION Action)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Action);
}
