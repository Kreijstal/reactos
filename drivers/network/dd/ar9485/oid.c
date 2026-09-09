/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     OID request dispatcher: identification, scanning, and the
 *              connection surface nwifi's media-specific module drives.
 *
 * The connection sequence nwifi runs is: program DESIRED_SSID_LIST,
 * DESIRED_BSSID_LIST, DESIRED_BSS_TYPE, ENABLED_AUTHENTICATION_ALGORITHM and
 * the two cipher lists, then set OID_DOT11_CONNECT_REQUEST - which carries no
 * payload, because running join/auth/assoc and reporting progress through the
 * NDIS_STATUS_DOT11_* indications is the miniport's job.  For a secure
 * network its supplicant then installs the negotiated keys through
 * OID_DOT11_CIPHER_KEY_MAPPING_KEY and OID_DOT11_CIPHER_DEFAULT_KEY.
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
            /* In 100 bps units; 0 == unknown / disconnected.  11 Mbit is
             * what the data path actually asks the hardware for. */
            U32 = (Adapter->MlmeState == AR9485MlmeAssociated) ? 110000 : 0;
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
            U32 = (Adapter->MlmeState == AR9485MlmeAssociated)
                      ? MediaConnectStateConnected
                      : MediaConnectStateDisconnected;
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
            Stats.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
            Stats.Header.Size     = sizeof(NDIS_STATISTICS_INFO);
            Stats.Header.Type     = NDIS_OBJECT_TYPE_DEFAULT;

            /*
             * Report only the four counters the driver actually maintains and
             * advertise exactly those in SupportedStatistics.  Returning a
             * zeroed block with SupportedStatistics == 0, as this did before,
             * is not "no statistics yet" -- it tells NDIS the miniport keeps
             * none, so ndis.sys stops asking and nothing upstream can ever see
             * a frame count.  That left the transmit path with no numeric
             * observable at all, which is precisely what made the TX status
             * bug so expensive to pin down.
             *
             * No byte counters exist here, so their flags stay clear rather
             * than claiming validity for a zero.
             */
            Stats.ifHCOutUcastPkts     = Adapter->TxFrameCount;
            Stats.ifOutErrors          = Adapter->TxFailureCount;
            Stats.ifHCInUcastPkts      = Adapter->RxUnicastCount;
            Stats.ifHCInMulticastPkts  = Adapter->RxMulticastCount;
            Stats.ifHCInBroadcastPkts  = Adapter->RxBroadcastCount;
            Stats.ifInErrors           = Adapter->RxErrorCount +
                                         Adapter->RxCryptoErrorCount;

            Stats.SupportedStatistics =
                NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT |
                NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR |
                NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV |
                NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_RCV |
                NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_RCV |
                NDIS_STATISTICS_FLAGS_VALID_RCV_ERROR;

            /*
             * Drop the receive-path breakdown into the log ring whenever
             * something asks for statistics, at most once every two seconds.
             * A statistics query is an operator action, so this is
             * self-rate-limiting; it turns `ifstats` on the box into a
             * complete answer to "where did the broadcasts go" without
             * spending a KD stop or a reboot.
             */
            {
                ULONG64 Now = KeQueryInterruptTime();
                if (Now - Adapter->LastStatsLogTime > 20000000ULL)
                {
                    Adapter->LastStatsLogTime = Now;
                    DPRINT1("AR9485: rx uc %I64u mc %I64u bc %I64u | phyerr %I64u "
                            "(grp %lu) crypto %I64u | grp seen %lu ok %lu nokey %lu "
                            "michael %lu | keymiss %lu decrypt %lu qos %lu sub %lu "
                            "short %lu\n",
                            Adapter->RxUnicastCount, Adapter->RxMulticastCount,
                            Adapter->RxBroadcastCount, Adapter->RxErrorCount,
                            Adapter->RxHwErrGroupCount,
                            Adapter->RxCryptoErrorCount,
                            Adapter->RxGroupTkipCount, Adapter->RxGroupMicOkCount,
                            Adapter->RxGroupNoKeyCount, Adapter->RxDropMichael,
                            Adapter->RxDropKeyMiss, Adapter->RxDropDecrypt,
                            Adapter->RxDropQos, Adapter->RxDropSubtype,
                            Adapter->RxDropShort);
                }
            }

            return AR9485WriteToOutBuffer(Req, &Stats, sizeof(Stats));
        }

        case OID_DOT11_MAC_ADDRESS:
        case OID_DOT11_CURRENT_ADDRESS:
            return AR9485WriteToOutBuffer(Req,
                                          Adapter->CurrentMacAddress,
                                          sizeof(DOT11_MAC_ADDRESS));

        case OID_DOT11_PERMANENT_ADDRESS:
            return AR9485WriteToOutBuffer(Req,
                                          Adapter->PermanentMacAddress,
                                          sizeof(DOT11_MAC_ADDRESS));

        case OID_DOT11_OPERATION_MODE_CAPABILITY:
        {
            DOT11_OPERATION_MODE_CAPABILITY Capability;

            NdisZeroMemory(&Capability, sizeof(Capability));
            Capability.uMajorVersion = 2;
            Capability.uMinorVersion = 0;
            Capability.uNumOfTXBuffers = AR9485_RX_BUFFER_COUNT;
            Capability.uNumOfRXBuffers = AR9485_RX_BUFFER_COUNT;
            Capability.uOpModeCapability =
                DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
            return AR9485WriteToOutBuffer(Req,
                                          &Capability,
                                          sizeof(Capability));
        }

        case OID_DOT11_CURRENT_OPERATION_MODE:
        {
            DOT11_CURRENT_OPERATION_MODE Mode;

            NdisZeroMemory(&Mode, sizeof(Mode));
            Mode.uCurrentOpMode = Adapter->CurrentOperationMode;
            return AR9485WriteToOutBuffer(Req, &Mode, sizeof(Mode));
        }

        case OID_DOT11_ENUM_BSS_LIST:
            return AR9485BuildBssList(Adapter, Req);

        case OID_DOT11_DESIRED_BSS_TYPE:
            U32 = dot11_BSS_type_infrastructure;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_DOT11_DESIRED_SSID_LIST:
        {
            struct { DOT11_SSID_LIST List; } Reply;

            NdisZeroMemory(&Reply, sizeof(Reply));
            Reply.List.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            Reply.List.Header.Revision = DOT11_SSID_LIST_REVISION_1;
            Reply.List.Header.Size = sizeof(DOT11_SSID_LIST);
            Reply.List.uNumOfEntries = 1;
            Reply.List.uTotalNumOfEntries = 1;
            Reply.List.SSIDs[0] = Adapter->DesiredSsid;
            return AR9485WriteToOutBuffer(Req, &Reply, sizeof(Reply));
        }

        case OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM:
        {
            DOT11_AUTH_ALGORITHM_LIST List;

            NdisZeroMemory(&List, sizeof(List));
            List.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            List.Header.Revision = DOT11_AUTH_ALGORITHM_LIST_REVISION_1;
            List.Header.Size = sizeof(List);
            List.uNumOfEntries = 1;
            List.uTotalNumOfEntries = 1;
            List.AlgorithmIds[0] = Adapter->AuthAlgorithm;
            return AR9485WriteToOutBuffer(Req, &List, sizeof(List));
        }

        case OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM:
        case OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM:
        {
            DOT11_CIPHER_ALGORITHM_LIST List;

            NdisZeroMemory(&List, sizeof(List));
            List.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            List.Header.Revision = DOT11_CIPHER_ALGORITHM_LIST_REVISION_1;
            List.Header.Size = sizeof(List);
            List.uNumOfEntries = 1;
            List.uTotalNumOfEntries = 1;
            List.AlgorithmIds[0] =
                (Req->DATA.QUERY_INFORMATION.Oid ==
                     OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM)
                    ? Adapter->UnicastCipher : Adapter->MulticastCipher;
            return AR9485WriteToOutBuffer(Req, &List, sizeof(List));
        }

        case OID_DOT11_CURRENT_PACKET_FILTER:
            U32 = Adapter->PacketFilter;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        case OID_DOT11_CIPHER_DEFAULT_KEY_ID:
            U32 = Adapter->GroupKeyId;
            return AR9485WriteToOutBuffer(Req, &U32, sizeof(U32));

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    UNREFERENCED_PARAMETER(U64);
}

/* An OID set whose buffer is too small: report what would have been enough
 * and read nothing. */
static NDIS_STATUS
AR9485BadLength(_In_ PNDIS_OID_REQUEST Req, _In_ ULONG Needed)
{
    Req->DATA.SET_INFORMATION.BytesNeeded = Needed;
    Req->DATA.SET_INFORMATION.BytesRead = 0;
    return NDIS_STATUS_INVALID_LENGTH;
}

/* An OID set whose buffer is big enough but says something impossible. */
static NDIS_STATUS
AR9485BadData(_In_ PNDIS_OID_REQUEST Req)
{
    Req->DATA.SET_INFORMATION.BytesRead = 0;
    return NDIS_STATUS_INVALID_DATA;
}

static NDIS_STATUS
AR9485Set(_In_ PAR9485_ADAPTER Adapter, _In_ PNDIS_OID_REQUEST Req)
{
    PVOID Buffer = Req->DATA.SET_INFORMATION.InformationBuffer;
    ULONG Length = Req->DATA.SET_INFORMATION.InformationBufferLength;

    switch (Req->DATA.SET_INFORMATION.Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_GEN_INTERRUPT_MODERATION:
            /* Accepted but ignored until the data path exists. */
            Req->DATA.SET_INFORMATION.BytesRead =
                Req->DATA.SET_INFORMATION.InformationBufferLength;
            return NDIS_STATUS_SUCCESS;

        case OID_DOT11_SCAN_REQUEST:
            Req->DATA.SET_INFORMATION.BytesRead =
                Req->DATA.SET_INFORMATION.InformationBufferLength;
            return AR9485StartScan(Adapter);

        case OID_DOT11_DESIRED_SSID_LIST:
        {
            PDOT11_SSID_LIST List;

            if (Length < sizeof(DOT11_SSID_LIST))
                return AR9485BadLength(Req, sizeof(DOT11_SSID_LIST));

            List = Buffer;
            NdisZeroMemory(&Adapter->DesiredSsid, sizeof(Adapter->DesiredSsid));
            /* An empty list means "any SSID"; a longer one is a roaming
             * profile this station does not implement, so honour the first
             * entry and say how much of the buffer was consumed. */
            if (List->uNumOfEntries != 0 &&
                Length >= FIELD_OFFSET(DOT11_SSID_LIST, SSIDs) +
                          sizeof(DOT11_SSID) &&
                List->SSIDs[0].uSSIDLength <= DOT11_SSID_MAX_LENGTH)
            {
                Adapter->DesiredSsid = List->SSIDs[0];
            }
            Req->DATA.SET_INFORMATION.BytesRead = Length;
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_DESIRED_BSSID_LIST:
        {
            PDOT11_BSSID_LIST List;

            if (Length < sizeof(DOT11_BSSID_LIST))
                return AR9485BadLength(Req, sizeof(DOT11_BSSID_LIST));

            List = Buffer;
            Adapter->HaveDesiredBssid = FALSE;
            if (List->uNumOfEntries != 0)
            {
                /* The broadcast address is how nwifi spells "no preference". */
                static const UCHAR Broadcast[DOT11_ADDR_LEN] =
                    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

                if (RtlCompareMemory(List->BSSIDs[0], Broadcast,
                                     DOT11_ADDR_LEN) != DOT11_ADDR_LEN)
                {
                    NdisMoveMemory(Adapter->DesiredBssid, List->BSSIDs[0],
                                   DOT11_ADDR_LEN);
                    Adapter->HaveDesiredBssid = TRUE;
                }
            }
            Req->DATA.SET_INFORMATION.BytesRead = Length;
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_DESIRED_BSS_TYPE:
        {
            ULONG BssType;

            if (Length < sizeof(ULONG))
                return AR9485BadLength(Req, sizeof(ULONG));

            BssType = *(PULONG)Buffer;
            Req->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            /* This is a managed-mode station: it joins an infrastructure BSS
             * and nothing else. */
            if (BssType != dot11_BSS_type_infrastructure &&
                BssType != dot11_BSS_type_any)
                return NDIS_STATUS_NOT_SUPPORTED;
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM:
        {
            PDOT11_AUTH_ALGORITHM_LIST List;

            if (Length < sizeof(DOT11_AUTH_ALGORITHM_LIST))
                return AR9485BadLength(Req, sizeof(DOT11_AUTH_ALGORITHM_LIST));

            List = Buffer;
            if (List->uNumOfEntries == 0)
                return AR9485BadData(Req);

            switch (List->AlgorithmIds[0])
            {
                case DOT11_AUTH_ALGO_80211_OPEN:
                case DOT11_AUTH_ALGO_RSNA:
                case DOT11_AUTH_ALGO_RSNA_PSK:
                case DOT11_AUTH_ALGO_WPA:
                case DOT11_AUTH_ALGO_WPA_PSK:
                    /* WPA1 selects the vendor-specific element in the
                     * association request (AR9485BuildWpaIe); RSNA selects
                     * the RSN element.  The key exchange itself is nwifi's. */
                    Adapter->AuthAlgorithm = List->AlgorithmIds[0];
                    break;
                default:
                    /* Shared Key is deprecated; SAE is not implemented. */
                    DPRINT1("AR9485: auth algorithm %u not implemented\n",
                            List->AlgorithmIds[0]);
                    Req->DATA.SET_INFORMATION.BytesRead = 0;
                    return NDIS_STATUS_NOT_SUPPORTED;
            }
            Req->DATA.SET_INFORMATION.BytesRead = Length;
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM:
        case OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM:
        {
            PDOT11_CIPHER_ALGORITHM_LIST List;

            if (Length < sizeof(DOT11_CIPHER_ALGORITHM_LIST))
                return AR9485BadLength(Req,
                                       sizeof(DOT11_CIPHER_ALGORITHM_LIST));

            List = Buffer;
            if (List->uNumOfEntries == 0)
                return AR9485BadData(Req);

            if (Req->DATA.SET_INFORMATION.Oid ==
                OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM)
                Adapter->UnicastCipher = List->AlgorithmIds[0];
            else
                Adapter->MulticastCipher = List->AlgorithmIds[0];

            Req->DATA.SET_INFORMATION.BytesRead = Length;
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_CURRENT_PACKET_FILTER:
            if (Length < sizeof(ULONG))
                return AR9485BadLength(Req, sizeof(ULONG));
            Adapter->PacketFilter = *(PULONG)Buffer;
            Req->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_DOT11_CONNECT_REQUEST:
            /*
             * No payload: everything the join needs was programmed by the
             * OIDs above.  The work runs on the chip worker and reports its
             * outcome through NDIS_STATUS_DOT11_CONNECTION_COMPLETION, so
             * this returns as soon as the request is queued.
             */
            if (Adapter->ChipThread == NULL)
                return NDIS_STATUS_FAILURE;
            if (AR9485LabOwnsChip())
                return NDIS_STATUS_MEDIA_BUSY;
            Req->DATA.SET_INFORMATION.BytesRead = Length;
            AR9485QueueCommand(Adapter, AR9485_CMD_CONNECT);
            return NDIS_STATUS_SUCCESS;

        case OID_DOT11_DISCONNECT_REQUEST:
            if (Adapter->ChipThread == NULL)
                return NDIS_STATUS_FAILURE;
            Req->DATA.SET_INFORMATION.BytesRead = Length;
            AR9485QueueCommand(Adapter, AR9485_CMD_DISCONNECT);
            return NDIS_STATUS_SUCCESS;

        case OID_DOT11_CIPHER_KEY_MAPPING_KEY:
        {
            /* A DOT11_BYTE_ARRAY wrapping a DOT11_CIPHER_KEY_MAPPING_KEY_VALUE:
             * the pairwise key the supplicant derived for one peer. */
            PDOT11_BYTE_ARRAY Array = Buffer;
            PDOT11_CIPHER_KEY_MAPPING_KEY_VALUE Value;
            ULONG ValueSize;

            if (Length < FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer) +
                         FIELD_OFFSET(DOT11_CIPHER_KEY_MAPPING_KEY_VALUE, ucKey))
                return AR9485BadLength(Req, sizeof(DOT11_BYTE_ARRAY));

            ValueSize = Length - FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer);
            if (Array->uNumOfBytes > ValueSize)
                return AR9485BadData(Req);

            Value = (PDOT11_CIPHER_KEY_MAPPING_KEY_VALUE)Array->ucBuffer;
            if (FIELD_OFFSET(DOT11_CIPHER_KEY_MAPPING_KEY_VALUE, ucKey) +
                Value->usKeyLength > ValueSize)
                return AR9485BadData(Req);

            Req->DATA.SET_INFORMATION.BytesRead = Length;
            return AR9485SetCipherKey(Adapter, AR9485_KEY_PAIRWISE,
                                      Value->bDelete ? DOT11_CIPHER_ALGO_NONE
                                                     : Value->AlgorithmId,
                                      Value->ucKey, Value->usKeyLength,
                                      Value->PeerMacAddr);
        }

        case OID_DOT11_CIPHER_DEFAULT_KEY:
        {
            /* The group key, at one of the four default key indices. */
            PDOT11_CIPHER_DEFAULT_KEY_VALUE Value = Buffer;

            if (Length < FIELD_OFFSET(DOT11_CIPHER_DEFAULT_KEY_VALUE, ucKey))
                return AR9485BadLength(Req,
                    FIELD_OFFSET(DOT11_CIPHER_DEFAULT_KEY_VALUE, ucKey));
            if (FIELD_OFFSET(DOT11_CIPHER_DEFAULT_KEY_VALUE, ucKey) +
                Value->usKeyLength > Length)
                return AR9485BadData(Req);
            if (Value->uKeyIndex >= DOT11_MAX_NUM_DEFAULT_KEY)
                return AR9485BadData(Req);

            Req->DATA.SET_INFORMATION.BytesRead = Length;
            return AR9485SetCipherKey(Adapter, Value->uKeyIndex,
                                      Value->bDelete ? DOT11_CIPHER_ALGO_NONE
                                                     : Value->AlgorithmId,
                                      Value->ucKey, Value->usKeyLength,
                                      NULL);
        }

        case OID_DOT11_CIPHER_DEFAULT_KEY_ID:
        {
            ULONG KeyId;

            if (Length < sizeof(ULONG))
                return AR9485BadLength(Req, sizeof(ULONG));
            KeyId = *(PULONG)Buffer;
            if (KeyId >= DOT11_MAX_NUM_DEFAULT_KEY)
                return AR9485BadData(Req);
            Adapter->GroupKeyId = (UCHAR)KeyId;
            Req->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;
        }

        case OID_DOT11_CURRENT_OPERATION_MODE:
        {
            PDOT11_CURRENT_OPERATION_MODE Mode;

            if (Req->DATA.SET_INFORMATION.InformationBufferLength <
                sizeof(DOT11_CURRENT_OPERATION_MODE))
            {
                Req->DATA.SET_INFORMATION.BytesNeeded =
                    sizeof(DOT11_CURRENT_OPERATION_MODE);
                Req->DATA.SET_INFORMATION.BytesRead = 0;
                return NDIS_STATUS_INVALID_LENGTH;
            }

            Mode = Req->DATA.SET_INFORMATION.InformationBuffer;
            if (Mode->uReserved != 0 ||
                Mode->uCurrentOpMode !=
                    DOT11_OPERATION_MODE_EXTENSIBLE_STATION)
            {
                Req->DATA.SET_INFORMATION.BytesRead = 0;
                return NDIS_STATUS_INVALID_DATA;
            }

            Adapter->CurrentOperationMode = Mode->uCurrentOpMode;
            Req->DATA.SET_INFORMATION.BytesRead = sizeof(*Mode);
            return NDIS_STATUS_SUCCESS;
        }

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
