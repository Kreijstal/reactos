/*
 * PROJECT:     ReactOS vwifi virtual Wi-Fi miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Internal definitions for the software-only NDIS 6.20
 *              Native-802.11 miniport.
 */

#ifndef _VWIFI_H_
#define _VWIFI_H_

#include <ndis.h>
#include <ntddndis.h>
#include <ipifcons.h>
#include <windot11.h>

/* Pulled in from the sibling e1000e driver: backfills the NDIS 6.20/6.30
 * symbol gaps in ROS' SDK ndis.h.  TODO: promote to a real
 * sdk/include/ddk/ndis6_compat.h so each NDIS 6 miniport doesn't reach
 * into a sibling tree. */
#include "../e1000e/ndis6_compat.h"

#define VWIFI_TAG               '14iV'      /* "Vi41" */
#define VWIFI_MAX_BSS           4           /* canned BSS list size */
#define VWIFI_DEFAULT_CHANNEL   6
#define VWIFI_BEACON_PERIOD_TU  100         /* 102.4ms standard beacon */

/* Stable test MAC: locally-administered (2nd-bit-of-first-byte set) */
#define VWIFI_DEFAULT_MAC0  0x02
#define VWIFI_DEFAULT_MAC1  0x52
#define VWIFI_DEFAULT_MAC2  0x57
#define VWIFI_DEFAULT_MAC3  0x49
#define VWIFI_DEFAULT_MAC4  0x46
#define VWIFI_DEFAULT_MAC5  0x49

typedef struct _VWIFI_BSS_ENTRY
{
    UCHAR    Bssid[6];
    UCHAR    SsidLen;
    UCHAR    Ssid[32];
    DOT11_PHY_TYPE PhyType;
    ULONG    ChCenterFreqMHz;
    SHORT    RssiDbm;
    ULONG    BeaconPeriodTu;
    BOOLEAN  PrivacyOn;             /* WPA2-PSK */
} VWIFI_BSS_ENTRY, *PVWIFI_BSS_ENTRY;

typedef struct _VWIFI_ADAPTER
{
    NDIS_HANDLE   MiniportAdapterHandle;
    NDIS_HANDLE   NdisMiniportDriverHandle;
    PDEVICE_OBJECT PhysicalDeviceObject;

    NDIS_SPIN_LOCK Lock;

    UCHAR         PermanentMac[6];
    UCHAR         CurrentMac[6];

    /* Last-set radio/operation knobs from OID_DOT11_* */
    DOT11_PHY_TYPE  CurrentPhyType;
    BOOLEAN         RadioOn;
    BOOLEAN         PowerSaveOn;
    ULONG           CurrentChannel;
    ULONG           PacketFilter;

    /* Static canned scan results */
    VWIFI_BSS_ENTRY Bss[VWIFI_MAX_BSS];
    ULONG           BssCount;

    /* Statistics (NDIS 6 OID_GEN_STATISTICS) */
    ULONG64       FramesXmitOk;
    ULONG64       FramesRcvOk;
    ULONG64       BytesXmit;
    ULONG64       BytesRcv;
} VWIFI_ADAPTER, *PVWIFI_ADAPTER;

extern NDIS_HANDLE g_VwifiDriverHandle;

/* init.c */
NDIS_STATUS NTAPI VwifiMiniportInitializeEx(NDIS_HANDLE, NDIS_HANDLE,
                                            PNDIS_MINIPORT_INIT_PARAMETERS);
VOID NTAPI VwifiMiniportHaltEx(NDIS_HANDLE, NDIS_HALT_ACTION);
NDIS_STATUS NTAPI VwifiMiniportPause(NDIS_HANDLE, PNDIS_MINIPORT_PAUSE_PARAMETERS);
NDIS_STATUS NTAPI VwifiMiniportRestart(NDIS_HANDLE, PNDIS_MINIPORT_RESTART_PARAMETERS);
VOID NTAPI VwifiMiniportDriverUnload(PDRIVER_OBJECT);
VOID NTAPI VwifiPnPEventNotify(NDIS_HANDLE, PNET_DEVICE_PNP_EVENT);
VOID NTAPI VwifiShutdownEx(NDIS_HANDLE, NDIS_SHUTDOWN_ACTION);
VOID         VwifiPopulateCannedBssList(PVWIFI_ADAPTER);

/* oid.c */
NDIS_STATUS NTAPI VwifiOidRequest(NDIS_HANDLE, PNDIS_OID_REQUEST);
VOID NTAPI VwifiCancelOidRequest(NDIS_HANDLE, PVOID);

/* send.c */
VOID NTAPI VwifiSendNetBufferLists(NDIS_HANDLE, PNET_BUFFER_LIST, NDIS_PORT_NUMBER, ULONG);
VOID NTAPI VwifiCancelSend(NDIS_HANDLE, PVOID);

/* recv.c */
VOID NTAPI VwifiReturnNetBufferLists(NDIS_HANDLE, PNET_BUFFER_LIST, ULONG);

/* scan.c */
NDIS_STATUS VwifiHandleScanRequest(PVWIFI_ADAPTER, PNDIS_OID_REQUEST);

#endif /* _VWIFI_H_ */
