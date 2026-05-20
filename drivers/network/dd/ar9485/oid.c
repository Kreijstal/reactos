/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     OID request dispatcher.  Phase 1a answers a small set of
 *              identification OIDs and returns NDIS_STATUS_NOT_SUPPORTED
 *              for the rest.  OID_DOT11_* handling lands in Phase 2+.
 */

#include "ar9485.h"

#define NDEBUG
#include <debug.h>

#define AR9485_DRIVER_VERSION   0x0001    /* 0.01 - Phase 1a */
#define AR9485_VENDOR_ID_OUI    0x00031e  /* Atheros / Qualcomm OUI */

static const char AR9485_VendorDesc[] = "Atheros AR9485 Wireless Network Adapter";

static NDIS_STATUS
AR9485WriteToOutBuffer(
    _In_ PNDIS_OID_REQUEST Req,
    _In_reads_bytes_(InLen) PVOID In,
    _In_ ULONG InLen)
{
    PVOID Buf = Req->DATA.QUERY_INFORMATION.InformationBuffer;
    ULONG BufLen = Req->DATA.QUERY_INFORMATION.InformationBufferLength;

    if (BufLen < InLen)
    {
        Req->DATA.QUERY_INFORMATION.BytesNeeded = InLen;
        Req->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    NdisMoveMemory(Buf, In, InLen);
    Req->DATA.QUERY_INFORMATION.BytesWritten = InLen;
    Req->DATA.QUERY_INFORMATION.BytesNeeded = InLen;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
AR9485Query(_In_ PAR9485_ADAPTER Adapter, _In_ PNDIS_OID_REQUEST Req)
{
    ULONG U32;
    ULONG64 U64;

    switch (Req->DATA.QUERY_INFORMATION.Oid)
    {
        case OID_GEN_HARDWARE_STATUS:
            U32 = NdisHardwareStatusReady;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            U32 = NdisMediumNative802_11;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_PHYSICAL_MEDIUM:
            U32 = NdisPhysicalMediumNative802_11;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_MAXIMUM_FRAME_SIZE:
            U32 = 1500;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            U32 = 1514;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            U32 = 1514;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_LINK_SPEED:
            U32 = 0;  /* In 100 bps units; 0 == unknown / disconnected */
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_TRANSMIT_BUFFER_SPACE:
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            U32 = 0;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_VENDOR_ID:
            U32 = AR9485_VENDOR_ID_OUI | (Adapter->RevisionId << 24);
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_VENDOR_DESCRIPTION:
            return AR9485WriteToOutBuffer(Req,
                                          (PVOID)AR9485_VendorDesc,
                                          sizeof(AR9485_VendorDesc));

        case OID_GEN_VENDOR_DRIVER_VERSION:
        case OID_GEN_DRIVER_VERSION:
            U32 = AR9485_DRIVER_VERSION;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_MEDIA_CONNECT_STATUS:
            U32 = MediaConnectStateDisconnected;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_CURRENT_PACKET_FILTER:
        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_GEN_MAC_OPTIONS:
        case OID_GEN_INTERRUPT_MODERATION:
            U32 = 0;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_GEN_STATISTICS:
        {
            NDIS_STATISTICS_INFO Stats;
            NdisZeroMemory(&Stats, sizeof(Stats));
            Stats.Header.Revision = 1;
            Stats.Header.Size     = sizeof(NDIS_STATISTICS_INFO);
            Stats.Header.Type     = NDIS_OBJECT_TYPE_DEFAULT;
            Stats.SupportedStatistics = 0;
            return AR9485WriteToOutBuffer(Req, &Stats, sizeof(Stats));
        }

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    UNREFERENCED_PARAMETER(U64);
}

static NDIS_STATUS
AR9485Set(_In_ PAR9485_ADAPTER Adapter, _In_ PNDIS_OID_REQUEST Req)
{
    UNREFERENCED_PARAMETER(Adapter);

    switch (Req->DATA.SET_INFORMATION.Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_GEN_INTERRUPT_MODERATION:
            /* Accepted but ignored until the data path exists. */
            Req->DATA.SET_INFORMATION.BytesRead =
                Req->DATA.SET_INFORMATION.InformationBufferLength;
            return NDIS_STATUS_SUCCESS;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

NDIS_STATUS NTAPI
AR9485OidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PAR9485_ADAPTER Adapter = (PAR9485_ADAPTER)MiniportAdapterContext;

    switch (OidRequest->RequestType)
    {
        case NdisRequestQueryInformation:
        case NdisRequestQueryStatistics:
            return AR9485Query(Adapter, OidRequest);

        case NdisRequestSetInformation:
            return AR9485Set(Adapter, OidRequest);

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

VOID NTAPI
AR9485CancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
}
