/* Intel Native Wi-Fi OID surface. */
#include "iwlwifi.h"
#define NDEBUG
#include <debug.h>

#define IWL_DRIVER_VERSION 0x0002
static const CHAR IwlVendorDescription[] =
    "Intel Wi-Fi 6E AX211 Native Wi-Fi Adapter";

const NDIS_OID IwlSupportedOids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID, OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_DRIVER_VERSION,
    OID_GEN_PHYSICAL_MEDIUM, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_LINK_SPEED,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAC_OPTIONS,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER, OID_GEN_STATISTICS,
    OID_DOT11_MAC_ADDRESS, OID_DOT11_PERMANENT_ADDRESS,
    OID_DOT11_CURRENT_ADDRESS, OID_DOT11_OPERATION_MODE_CAPABILITY,
    OID_DOT11_CURRENT_OPERATION_MODE, OID_DOT11_NIC_POWER_STATE,
    OID_DOT11_HARDWARE_PHY_STATE, OID_DOT11_SCAN_REQUEST,
    OID_DOT11_ENUM_BSS_LIST, OID_DOT11_FLUSH_BSS_LIST,
    OID_DOT11_DESIRED_BSS_TYPE, OID_DOT11_DESIRED_SSID_LIST,
    OID_DOT11_DESIRED_BSSID_LIST,
    OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM,
    OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM,
    OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM,
    OID_DOT11_CONNECT_REQUEST, OID_DOT11_CIPHER_KEY_MAPPING_KEY,
    OID_DOT11_CIPHER_DEFAULT_KEY, OID_DOT11_CIPHER_DEFAULT_KEY_ID
};
const ULONG IwlSupportedOidCount = RTL_NUMBER_OF(IwlSupportedOids);

static NDIS_STATUS
IwlQueryBuffer(PNDIS_OID_REQUEST Request, const VOID *Value, ULONG Length)
{
    if (Request->DATA.QUERY_INFORMATION.InformationBufferLength < Length)
    {
        Request->DATA.QUERY_INFORMATION.BytesWritten = 0;
        Request->DATA.QUERY_INFORMATION.BytesNeeded = Length;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    NdisMoveMemory(Request->DATA.QUERY_INFORMATION.InformationBuffer,
                   Value, Length);
    Request->DATA.QUERY_INFORMATION.BytesWritten = Length;
    Request->DATA.QUERY_INFORMATION.BytesNeeded = Length;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
IwlQueryUlong(PNDIS_OID_REQUEST Request, ULONG Value)
{
    return IwlQueryBuffer(Request, &Value, sizeof(Value));
}

static NDIS_STATUS
IwlQuery(PIWL_ADAPTER Adapter, PNDIS_OID_REQUEST Request)
{
    ULONG Oid = Request->DATA.QUERY_INFORMATION.Oid;
    switch (Oid)
    {
        case OID_GEN_SUPPORTED_LIST:
            return IwlQueryBuffer(Request, IwlSupportedOids,
                IwlSupportedOidCount * sizeof(NDIS_OID));
        case OID_GEN_HARDWARE_STATUS:
            return IwlQueryUlong(Request, NdisHardwareStatusReady);
        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            return IwlQueryUlong(Request, NdisMediumNative802_11);
        case OID_GEN_PHYSICAL_MEDIUM:
            return IwlQueryUlong(Request, NdisPhysicalMediumNative802_11);
        case OID_GEN_MAXIMUM_FRAME_SIZE:
            return IwlQueryUlong(Request, 1500);
        case OID_GEN_MAXIMUM_TOTAL_SIZE:
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            return IwlQueryUlong(Request, 1514);
        case OID_GEN_LINK_SPEED:
            return IwlQueryUlong(Request, 0);
        case OID_GEN_XMIT_OK:
        case OID_GEN_RCV_OK:
        case OID_GEN_XMIT_ERROR:
        case OID_GEN_RCV_ERROR:
        case OID_GEN_RCV_NO_BUFFER:
            return IwlQueryUlong(Request, 0);
        case OID_GEN_VENDOR_ID:
            return IwlQueryUlong(Request, 0x0086a0 |
                ((ULONG)Adapter->RevisionId << 24));
        case OID_GEN_VENDOR_DESCRIPTION:
            return IwlQueryBuffer(Request, IwlVendorDescription,
                                  sizeof(IwlVendorDescription));
        case OID_GEN_VENDOR_DRIVER_VERSION:
        case OID_GEN_DRIVER_VERSION:
            return IwlQueryUlong(Request, IWL_DRIVER_VERSION);
        case OID_GEN_MEDIA_CONNECT_STATUS:
            return IwlQueryUlong(Request, MediaConnectStateDisconnected);
        case OID_GEN_CURRENT_PACKET_FILTER:
        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_GEN_MAC_OPTIONS:
            return IwlQueryUlong(Request, 0);
        case OID_GEN_STATISTICS:
        {
            NDIS_STATISTICS_INFO Statistics;
            NdisZeroMemory(&Statistics, sizeof(Statistics));
            Statistics.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            Statistics.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
            Statistics.Header.Size = sizeof(Statistics);
            return IwlQueryBuffer(Request, &Statistics, sizeof(Statistics));
        }
        case OID_DOT11_MAC_ADDRESS:
        case OID_DOT11_CURRENT_ADDRESS:
            return IwlQueryBuffer(Request, Adapter->CurrentMacAddress,
                                  sizeof(DOT11_MAC_ADDRESS));
        case OID_DOT11_PERMANENT_ADDRESS:
            return IwlQueryBuffer(Request, Adapter->PermanentMacAddress,
                                  sizeof(DOT11_MAC_ADDRESS));
        case OID_DOT11_OPERATION_MODE_CAPABILITY:
        {
            DOT11_OPERATION_MODE_CAPABILITY Capability;
            NdisZeroMemory(&Capability, sizeof(Capability));
            Capability.uMajorVersion = 2;
            Capability.uMinorVersion = 0;
            Capability.uNumOfTXBuffers = IWL_GEN3_CMD_QUEUE_SIZE;
            Capability.uNumOfRXBuffers = IWL_GEN3_RX_QUEUE_SIZE;
            Capability.uOpModeCapability =
                DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
            return IwlQueryBuffer(Request, &Capability, sizeof(Capability));
        }
        case OID_DOT11_CURRENT_OPERATION_MODE:
        {
            DOT11_CURRENT_OPERATION_MODE Mode;
            NdisZeroMemory(&Mode, sizeof(Mode));
            Mode.uCurrentOpMode = Adapter->CurrentOperationMode;
            return IwlQueryBuffer(Request, &Mode, sizeof(Mode));
        }
        case OID_DOT11_NIC_POWER_STATE:
        case OID_DOT11_HARDWARE_PHY_STATE:
            return IwlQueryUlong(Request, TRUE);
        case OID_DOT11_ENUM_BSS_LIST:
            return IwlBuildBssList(Adapter, Request);
        default:
            DPRINT1("iwlwifi: unsupported query OID 0x%08lx\n", Oid);
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static NDIS_STATUS
IwlSetFixed(
    PNDIS_OID_REQUEST Request,
    PVOID Destination,
    ULONG Length)
{
    if (Request->DATA.SET_INFORMATION.InformationBufferLength < Length)
    {
        Request->DATA.SET_INFORMATION.BytesRead = 0;
        Request->DATA.SET_INFORMATION.BytesNeeded = Length;
        return NDIS_STATUS_INVALID_LENGTH;
    }

    NdisMoveMemory(Destination,
                   Request->DATA.SET_INFORMATION.InformationBuffer,
                   Length);
    Request->DATA.SET_INFORMATION.BytesRead = Length;
    Request->DATA.SET_INFORMATION.BytesNeeded = 0;
    return NDIS_STATUS_SUCCESS;
}

static BOOLEAN
IwlIsBroadcastAddress(const UCHAR Address[IWL_MAC_ADDRESS_LENGTH])
{
    ULONG Index;

    for (Index = 0; Index < IWL_MAC_ADDRESS_LENGTH; Index++)
        if (Address[Index] != 0xff)
            return FALSE;
    return TRUE;
}

static NDIS_STATUS
IwlSet(PIWL_ADAPTER Adapter, PNDIS_OID_REQUEST Request)
{
    ULONG Oid = Request->DATA.SET_INFORMATION.Oid;
    switch (Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
        case OID_GEN_CURRENT_LOOKAHEAD:
            Request->DATA.SET_INFORMATION.BytesRead =
                Request->DATA.SET_INFORMATION.InformationBufferLength;
            return NDIS_STATUS_SUCCESS;
        case OID_DOT11_SCAN_REQUEST:
            Request->DATA.SET_INFORMATION.BytesRead =
                Request->DATA.SET_INFORMATION.InformationBufferLength;
            return IwlStartScan(Adapter);
        case OID_DOT11_FLUSH_BSS_LIST:
            Adapter->BssCount = 0;
            Request->DATA.SET_INFORMATION.BytesRead =
                Request->DATA.SET_INFORMATION.InformationBufferLength;
            return NDIS_STATUS_SUCCESS;
        case OID_DOT11_CURRENT_OPERATION_MODE:
        {
            PDOT11_CURRENT_OPERATION_MODE Mode;
            if (Request->DATA.SET_INFORMATION.InformationBufferLength <
                sizeof(DOT11_CURRENT_OPERATION_MODE))
            {
                Request->DATA.SET_INFORMATION.BytesNeeded =
                    sizeof(DOT11_CURRENT_OPERATION_MODE);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            Mode = Request->DATA.SET_INFORMATION.InformationBuffer;
            if (Mode->uReserved != 0 ||
                Mode->uCurrentOpMode !=
                    DOT11_OPERATION_MODE_EXTENSIBLE_STATION)
                return NDIS_STATUS_INVALID_DATA;
            Adapter->CurrentOperationMode = Mode->uCurrentOpMode;
            Request->DATA.SET_INFORMATION.BytesRead = sizeof(*Mode);
            return NDIS_STATUS_SUCCESS;
        }
        case OID_DOT11_DESIRED_BSS_TYPE:
        {
            DOT11_BSS_TYPE Value;
            NDIS_STATUS Status = IwlSetFixed(Request, &Value, sizeof(Value));
            if (Status != NDIS_STATUS_SUCCESS)
                return Status;
            if (Value != dot11_BSS_type_infrastructure &&
                Value != dot11_BSS_type_any)
                return NDIS_STATUS_INVALID_DATA;
            Adapter->DesiredBssType = Value;
            return NDIS_STATUS_SUCCESS;
        }
        case OID_DOT11_DESIRED_SSID_LIST:
        {
            PDOT11_SSID_LIST List =
                Request->DATA.SET_INFORMATION.InformationBuffer;
            ULONG Minimum = FIELD_OFFSET(DOT11_SSID_LIST, SSIDs) +
                            sizeof(DOT11_SSID);
            if (Request->DATA.SET_INFORMATION.InformationBufferLength < Minimum)
            {
                Request->DATA.SET_INFORMATION.BytesNeeded = Minimum;
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (List->Header.Type != NDIS_OBJECT_TYPE_DEFAULT ||
                List->Header.Revision != DOT11_SSID_LIST_REVISION_1 ||
                List->uNumOfEntries != 1 || List->uTotalNumOfEntries < 1 ||
                List->SSIDs[0].uSSIDLength > DOT11_SSID_MAX_LENGTH)
                return NDIS_STATUS_INVALID_DATA;
            Adapter->DesiredSsid = List->SSIDs[0];
            Request->DATA.SET_INFORMATION.BytesRead = Minimum;
            return NDIS_STATUS_SUCCESS;
        }
        case OID_DOT11_DESIRED_BSSID_LIST:
        {
            PDOT11_BSSID_LIST List =
                Request->DATA.SET_INFORMATION.InformationBuffer;
            ULONG Minimum = FIELD_OFFSET(DOT11_BSSID_LIST, BSSIDs) +
                            sizeof(DOT11_MAC_ADDRESS);
            if (Request->DATA.SET_INFORMATION.InformationBufferLength < Minimum)
            {
                Request->DATA.SET_INFORMATION.BytesNeeded = Minimum;
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (List->Header.Type != NDIS_OBJECT_TYPE_DEFAULT ||
                List->Header.Revision != DOT11_BSSID_LIST_REVISION_1 ||
                List->uNumOfEntries != 1 || List->uTotalNumOfEntries < 1)
                return NDIS_STATUS_INVALID_DATA;
            NdisMoveMemory(Adapter->DesiredBssid, List->BSSIDs[0],
                           sizeof(DOT11_MAC_ADDRESS));
            Adapter->DesiredBssidValid =
                !IwlIsBroadcastAddress(Adapter->DesiredBssid);
            Request->DATA.SET_INFORMATION.BytesRead = Minimum;
            return NDIS_STATUS_SUCCESS;
        }
        case OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM:
        {
            PDOT11_AUTH_ALGORITHM_LIST List =
                Request->DATA.SET_INFORMATION.InformationBuffer;
            ULONG Minimum = FIELD_OFFSET(DOT11_AUTH_ALGORITHM_LIST,
                                         AlgorithmIds) +
                            sizeof(DOT11_AUTH_ALGORITHM);
            if (Request->DATA.SET_INFORMATION.InformationBufferLength < Minimum)
            {
                Request->DATA.SET_INFORMATION.BytesNeeded = Minimum;
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (List->Header.Type != NDIS_OBJECT_TYPE_DEFAULT ||
                List->Header.Revision != DOT11_AUTH_ALGORITHM_LIST_REVISION_1 ||
                List->uNumOfEntries != 1 || List->uTotalNumOfEntries < 1)
                return NDIS_STATUS_INVALID_DATA;
            Adapter->AuthenticationAlgorithm = List->AlgorithmIds[0];
            Request->DATA.SET_INFORMATION.BytesRead = Minimum;
            return NDIS_STATUS_SUCCESS;
        }
        case OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM:
        case OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM:
        {
            PDOT11_CIPHER_ALGORITHM_LIST List =
                Request->DATA.SET_INFORMATION.InformationBuffer;
            ULONG Minimum = FIELD_OFFSET(DOT11_CIPHER_ALGORITHM_LIST,
                                         AlgorithmIds) +
                            sizeof(DOT11_CIPHER_ALGORITHM);
            if (Request->DATA.SET_INFORMATION.InformationBufferLength < Minimum)
            {
                Request->DATA.SET_INFORMATION.BytesNeeded = Minimum;
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (List->Header.Type != NDIS_OBJECT_TYPE_DEFAULT ||
                List->Header.Revision != DOT11_CIPHER_ALGORITHM_LIST_REVISION_1 ||
                List->uNumOfEntries != 1 || List->uTotalNumOfEntries < 1)
                return NDIS_STATUS_INVALID_DATA;
            if (Oid == OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM)
                Adapter->UnicastCipher = List->AlgorithmIds[0];
            else
                Adapter->MulticastCipher = List->AlgorithmIds[0];
            Request->DATA.SET_INFORMATION.BytesRead = Minimum;
            return NDIS_STATUS_SUCCESS;
        }
        case OID_DOT11_CONNECT_REQUEST:
            Request->DATA.SET_INFORMATION.BytesRead = 0;
            if (Request->DATA.SET_INFORMATION.InformationBufferLength != 0)
                return NDIS_STATUS_INVALID_LENGTH;
            return IwlStartConnect(Adapter);
        case OID_DOT11_CIPHER_KEY_MAPPING_KEY:
        {
            PDOT11_BYTE_ARRAY Array =
                Request->DATA.SET_INFORMATION.InformationBuffer;
            PDOT11_CIPHER_KEY_MAPPING_KEY_VALUE Value;
            ULONG Header = FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer);
            ULONG ValueHeader =
                FIELD_OFFSET(DOT11_CIPHER_KEY_MAPPING_KEY_VALUE, ucKey);
            if (Request->DATA.SET_INFORMATION.InformationBufferLength < Header ||
                Array->uNumOfBytes < ValueHeader ||
                Array->uNumOfBytes >
                    Request->DATA.SET_INFORMATION.InformationBufferLength - Header)
                return NDIS_STATUS_INVALID_LENGTH;
            Value = (PDOT11_CIPHER_KEY_MAPPING_KEY_VALUE)Array->ucBuffer;
            if (Value->bDelete || Value->usKeyLength >
                    Array->uNumOfBytes - ValueHeader)
                return NDIS_STATUS_NOT_SUPPORTED;
            Request->DATA.SET_INFORMATION.BytesRead = Header + Array->uNumOfBytes;
            return IwlInstallKey(Adapter, 0, Value->AlgorithmId, Value->ucKey,
                                 Value->usKeyLength, FALSE);
        }
        case OID_DOT11_CIPHER_DEFAULT_KEY:
        {
            PDOT11_CIPHER_DEFAULT_KEY_VALUE Value =
                Request->DATA.SET_INFORMATION.InformationBuffer;
            ULONG Header = FIELD_OFFSET(DOT11_CIPHER_DEFAULT_KEY_VALUE, ucKey);
            ULONG Length = Request->DATA.SET_INFORMATION.InformationBufferLength;
            if (Length < Header)
                return NDIS_STATUS_INVALID_LENGTH;
            if (Value->bDelete || Value->usKeyLength > Length - Header)
                return NDIS_STATUS_NOT_SUPPORTED;
            Request->DATA.SET_INFORMATION.BytesRead = Header + Value->usKeyLength;
            return IwlInstallKey(Adapter, Value->uKeyIndex, Value->AlgorithmId,
                                 Value->ucKey, Value->usKeyLength, TRUE);
        }
        case OID_DOT11_CIPHER_DEFAULT_KEY_ID:
            return IwlSetFixed(Request, &Adapter->DefaultKeyId,
                               sizeof(Adapter->DefaultKeyId));
        default:
            DPRINT1("iwlwifi: unsupported set OID 0x%08lx\n", Oid);
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

NDIS_STATUS NTAPI
IwlOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PIWL_ADAPTER Adapter = MiniportAdapterContext;
    switch (OidRequest->RequestType)
    {
        case NdisRequestQueryInformation:
        case NdisRequestQueryStatistics:
            return IwlQuery(Adapter, OidRequest);
        case NdisRequestSetInformation:
            return IwlSet(Adapter, OidRequest);
        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

VOID NTAPI
IwlCancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
}
