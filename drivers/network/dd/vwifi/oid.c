/*
 * PROJECT:     ReactOS vwifi virtual Wi-Fi miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     OID dispatcher: OID_GEN_* baseline plus the OID_DOT11_*
 *              surface needed to satisfy wlansvc enumeration / scan probes.
 */

#include "vwifi.h"

#define NDEBUG
#include <debug.h>

static const ULONG g_VwifiSupportedOids[] = {
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_MAXIMUM_LOOKAHEAD,
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
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_STATISTICS,
    OID_GEN_PHYSICAL_MEDIUM,
    OID_GEN_INTERRUPT_MODERATION,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_DOT11_OPERATION_MODE_CAPABILITY,
    OID_DOT11_CURRENT_OPERATION_MODE,
    OID_DOT11_MPDU_MAX_LENGTH,
    OID_DOT11_STATION_ID,
    OID_DOT11_CURRENT_PHY_TYPE,
    OID_DOT11_SUPPORTED_PHY_TYPES,
    OID_DOT11_CURRENT_CHANNEL,
    OID_DOT11_NIC_POWER_STATE,
    OID_DOT11_SCAN_REQUEST,
    OID_DOT11_ENUM_BSS_LIST,
    OID_DOT11_FLUSH_BSS_LIST,
    OID_DOT11_DESIRED_BSS_TYPE,
    OID_DOT11_CURRENT_PACKET_FILTER,
    OID_DOT11_RESET_REQUEST,
};

#define VWIFI_RETURN_ULONG(Buf, BufLen, BytesWritten, Val)               \
    do {                                                                  \
        if ((BufLen) < sizeof(ULONG)) return NDIS_STATUS_BUFFER_TOO_SHORT; \
        *(PULONG)(Buf) = (ULONG)(Val);                                    \
        *(BytesWritten) = sizeof(ULONG);                                  \
    } while (0)

#define VWIFI_RETURN_BYTES(Buf, BufLen, BytesWritten, Src, SrcLen)        \
    do {                                                                  \
        if ((BufLen) < (SrcLen)) return NDIS_STATUS_BUFFER_TOO_SHORT;      \
        NdisMoveMemory((Buf), (Src), (SrcLen));                            \
        *(BytesWritten) = (ULONG)(SrcLen);                                 \
    } while (0)

static NDIS_STATUS
VwifiQueryOid(_In_ PVWIFI_ADAPTER A, _In_ PNDIS_OID_REQUEST R)
{
    NDIS_OID Oid = R->DATA.QUERY_INFORMATION.Oid;
    PVOID Buf    = R->DATA.QUERY_INFORMATION.InformationBuffer;
    ULONG BufLen = R->DATA.QUERY_INFORMATION.InformationBufferLength;
    PULONG Written = &R->DATA.QUERY_INFORMATION.BytesWritten;
    PULONG Needed  = &R->DATA.QUERY_INFORMATION.BytesNeeded;

    *Written = 0;
    *Needed  = 0;

    switch (Oid)
    {
    case OID_GEN_SUPPORTED_LIST:
        if (BufLen < sizeof(g_VwifiSupportedOids)) {
            *Needed = sizeof(g_VwifiSupportedOids);
            return NDIS_STATUS_BUFFER_TOO_SHORT;
        }
        NdisMoveMemory(Buf, g_VwifiSupportedOids, sizeof(g_VwifiSupportedOids));
        *Written = sizeof(g_VwifiSupportedOids);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_HARDWARE_STATUS:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, NdisHardwareStatusReady);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, NdisMediumNative802_11);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_PHYSICAL_MEDIUM:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, NdisPhysicalMediumNative802_11);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_CURRENT_LOOKAHEAD:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, 1514);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_LINK_SPEED:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, 540000);   /* 100 bps */
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_TRANSMIT_BUFFER_SPACE:
    case OID_GEN_RECEIVE_BUFFER_SPACE:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, 64 * 1024);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_VENDOR_ID:
        /* Use IEEE OUI 0x52:0x57:0x49 ("RWI") repurposed for vwifi */
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, 0x00524957);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_VENDOR_DESCRIPTION:
    {
        static const CHAR Desc[] = "ReactOS Virtual Wi-Fi Adapter";
        VWIFI_RETURN_BYTES(Buf, BufLen, Written, Desc, sizeof(Desc));
        return NDIS_STATUS_SUCCESS;
    }

    case OID_GEN_VENDOR_DRIVER_VERSION:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, 0x00010000);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_DRIVER_VERSION:
    {
        USHORT v = (NDIS_MINIPORT_MAJOR_VERSION << 8) | NDIS_MINIPORT_MINOR_VERSION;
        if (BufLen < sizeof(USHORT)) return NDIS_STATUS_BUFFER_TOO_SHORT;
        *(PUSHORT)Buf = v;
        *Written = sizeof(USHORT);
        return NDIS_STATUS_SUCCESS;
    }

    case OID_GEN_CURRENT_PACKET_FILTER:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, A->PacketFilter);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_XMIT_OK:
    case OID_GEN_RCV_OK:
        if (BufLen < sizeof(ULONG64)) return NDIS_STATUS_BUFFER_TOO_SHORT;
        *(PULONG64)Buf = (Oid == OID_GEN_XMIT_OK) ? A->FramesXmitOk : A->FramesRcvOk;
        *Written = sizeof(ULONG64);
        return NDIS_STATUS_SUCCESS;

    case OID_802_3_PERMANENT_ADDRESS:
        VWIFI_RETURN_BYTES(Buf, BufLen, Written, A->PermanentMac, 6);
        return NDIS_STATUS_SUCCESS;

    case OID_802_3_CURRENT_ADDRESS:
        VWIFI_RETURN_BYTES(Buf, BufLen, Written, A->CurrentMac, 6);
        return NDIS_STATUS_SUCCESS;

    case OID_802_3_MAXIMUM_LIST_SIZE:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, 32);
        return NDIS_STATUS_SUCCESS;

    /* -------- DOT11 read-side -------- */
    case OID_DOT11_STATION_ID:
        VWIFI_RETURN_BYTES(Buf, BufLen, Written, A->CurrentMac, 6);
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_CURRENT_PHY_TYPE:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, A->CurrentPhyType);
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_CURRENT_CHANNEL:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, A->CurrentChannel);
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_NIC_POWER_STATE:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, A->RadioOn ? TRUE : FALSE);
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_MPDU_MAX_LENGTH:
        VWIFI_RETURN_ULONG(Buf, BufLen, Written, 2304);
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_ENUM_BSS_LIST:
        return VwifiHandleScanRequest(A, R);

    default:
        DPRINT("vwifi: unhandled query OID 0x%08x\n", Oid);
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static NDIS_STATUS
VwifiSetOid(_In_ PVWIFI_ADAPTER A, _In_ PNDIS_OID_REQUEST R)
{
    NDIS_OID Oid = R->DATA.SET_INFORMATION.Oid;
    PVOID Buf    = R->DATA.SET_INFORMATION.InformationBuffer;
    ULONG BufLen = R->DATA.SET_INFORMATION.InformationBufferLength;
    PULONG Read  = &R->DATA.SET_INFORMATION.BytesRead;
    PULONG Needed = &R->DATA.SET_INFORMATION.BytesNeeded;

    *Read = 0;
    *Needed = 0;

    switch (Oid)
    {
    case OID_GEN_CURRENT_PACKET_FILTER:
    case OID_DOT11_CURRENT_PACKET_FILTER:
        if (BufLen < sizeof(ULONG)) return NDIS_STATUS_INVALID_LENGTH;
        A->PacketFilter = *(PULONG)Buf;
        *Read = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_CURRENT_LOOKAHEAD:
        if (BufLen < sizeof(ULONG)) return NDIS_STATUS_INVALID_LENGTH;
        *Read = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;

    case OID_802_3_MULTICAST_LIST:
        *Read = BufLen;
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_CURRENT_PHY_TYPE:
        if (BufLen < sizeof(ULONG)) return NDIS_STATUS_INVALID_LENGTH;
        A->CurrentPhyType = *(PULONG)Buf;
        *Read = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_NIC_POWER_STATE:
        if (BufLen < sizeof(ULONG)) return NDIS_STATUS_INVALID_LENGTH;
        A->RadioOn = !!*(PULONG)Buf;
        *Read = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_SCAN_REQUEST:
        *Read = BufLen;
        return VwifiHandleScanRequest(A, R);

    case OID_DOT11_RESET_REQUEST:
        *Read = BufLen;
        return NDIS_STATUS_SUCCESS;

    case OID_DOT11_FLUSH_BSS_LIST:
        /* No-op; the canned list is what we have */
        *Read = BufLen;
        return NDIS_STATUS_SUCCESS;

    default:
        DPRINT("vwifi: unhandled set OID 0x%08x\n", Oid);
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

NDIS_STATUS
NTAPI
VwifiOidRequest(_In_ NDIS_HANDLE Ctx, _In_ PNDIS_OID_REQUEST Req)
{
    PVWIFI_ADAPTER A = (PVWIFI_ADAPTER)Ctx;
    NDIS_STATUS Status;

    switch (Req->RequestType)
    {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        Status = VwifiQueryOid(A, Req);
        break;
    case NdisRequestSetInformation:
        Status = VwifiSetOid(A, Req);
        break;
    case NdisRequestMethod:
        /* No method OIDs implemented yet */
        Status = NDIS_STATUS_NOT_SUPPORTED;
        break;
    default:
        Status = NDIS_STATUS_NOT_SUPPORTED;
        break;
    }
    return Status;
}

VOID
NTAPI
VwifiCancelOidRequest(_In_ NDIS_HANDLE Ctx, _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(RequestId);
}
