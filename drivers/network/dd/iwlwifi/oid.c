/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     OID request handling.
 *
 * Phase 1a's MiniportInitializeEx never completes successfully, so NDIS
 * never issues an OID to this driver.  The handlers exist because
 * NdisMRegisterMiniportDriver requires them; they answer NOT_SUPPORTED
 * rather than fabricating values, so the day initialization does succeed
 * the gap is visible instead of silently wrong.
 */

#include "iwlwifi.h"

#define NDEBUG
#include <debug.h>

NDIS_STATUS NTAPI
IwlOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);

    DPRINT1("iwlwifi: OID request type %d oid 0x%08x - not supported yet\n",
            OidRequest->RequestType, OidRequest->DATA.QUERY_INFORMATION.Oid);

    return NDIS_STATUS_NOT_SUPPORTED;
}

VOID NTAPI
IwlCancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
}
