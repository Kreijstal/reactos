/*
 * PROJECT:     ReactOS vwifi virtual Wi-Fi miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Scan handling: respond to OID_DOT11_SCAN_REQUEST with our
 *              canned BSS list, and serve OID_DOT11_ENUM_BSS_LIST queries.
 *              Full beacon-frame indication path is deferred to a follow-up
 *              that hooks up the receive timer.
 */

#include "vwifi.h"

#define NDEBUG
#include <debug.h>

static VOID
VwifiIndicateScanConfirm(_In_ PVWIFI_ADAPTER A, _In_ NDIS_STATUS ScanStatus)
{
    NDIS_STATUS_INDICATION Indication;

    NdisZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type     = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size     = sizeof(NDIS_STATUS_INDICATION);
    Indication.SourceHandle    = A->MiniportAdapterHandle;
    Indication.StatusCode      = NDIS_STATUS_DOT11_SCAN_CONFIRM;
    Indication.StatusBuffer    = &ScanStatus;
    Indication.StatusBufferSize = sizeof(ScanStatus);

    DPRINT1("vwifi: indicating NDIS_STATUS_DOT11_SCAN_CONFIRM (0x%08lx)\n",
            ScanStatus);
    NdisMIndicateStatusEx(A->MiniportAdapterHandle, &Indication);
}

NDIS_STATUS
VwifiHandleScanRequest(_In_ PVWIFI_ADAPTER A, _In_ PNDIS_OID_REQUEST R)
{
    NDIS_OID Oid;

    switch (R->RequestType)
    {
    case NdisRequestSetInformation:
        Oid = R->DATA.SET_INFORMATION.Oid;
        if (Oid == OID_DOT11_SCAN_REQUEST)
        {
            /* Our canned BSS list never changes, so the "scan" is
             * instantaneous.  Acknowledge the OID synchronously, then
             * indicate NDIS_STATUS_DOT11_SCAN_CONFIRM so any bound
             * NDIS 6 protocol (eventually ndisdot11 / wlansvc) sees the
             * completion event.  A future commit may DPC-defer this to
             * better match real silicon's async behaviour. */
            DPRINT1("vwifi: OID_DOT11_SCAN_REQUEST acknowledged, %u canned BSSes\n",
                    A->BssCount);
            VwifiIndicateScanConfirm(A, NDIS_STATUS_SUCCESS);
            return NDIS_STATUS_SUCCESS;
        }
        return NDIS_STATUS_NOT_SUPPORTED;

    case NdisRequestQueryInformation:
        Oid = R->DATA.QUERY_INFORMATION.Oid;
        if (Oid == OID_DOT11_ENUM_BSS_LIST)
        {
            /* Until we wire up the full BSS-list output schema, declare
             * that the list is empty (BytesWritten=0) and let the caller
             * proceed.  A follow-up commit will emit the real
             * DOT11_BYTE_ARRAY-formatted output here. */
            R->DATA.QUERY_INFORMATION.BytesWritten = 0;
            return NDIS_STATUS_SUCCESS;
        }
        return NDIS_STATUS_NOT_SUPPORTED;

    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}
