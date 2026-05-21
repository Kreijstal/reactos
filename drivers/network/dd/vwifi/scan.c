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
            /* Caller wants us to start a scan.  Our canned list never
             * changes; we acknowledge immediately and a future commit
             * will queue NDIS_STATUS_DOT11_SCAN_CONFIRM indication.  */
            DPRINT1("vwifi: OID_DOT11_SCAN_REQUEST acknowledged, %u canned BSSes\n",
                    A->BssCount);
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
