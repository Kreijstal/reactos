/*
 * PROJECT:     ReactOS vwifi virtual Wi-Fi miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     TX path: with no radio we just complete the NBL chain.
 */

#include "vwifi.h"

#define NDEBUG
#include <debug.h>

VOID
NTAPI
VwifiSendNetBufferLists(
    _In_ NDIS_HANDLE Ctx,
    _In_ PNET_BUFFER_LIST NblChain,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PVWIFI_ADAPTER A = (PVWIFI_ADAPTER)Ctx;
    PNET_BUFFER_LIST Nbl;
    ULONG Flags = 0;

    UNREFERENCED_PARAMETER(PortNumber);

    if (NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendFlags))
        Flags |= NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL;

    for (Nbl = NblChain; Nbl != NULL; Nbl = NET_BUFFER_LIST_NEXT_NBL(Nbl))
    {
        PNET_BUFFER Nb = NET_BUFFER_LIST_FIRST_NB(Nbl);
        NET_BUFFER_LIST_STATUS(Nbl) = NDIS_STATUS_SUCCESS;
        while (Nb)
        {
            A->FramesXmitOk++;
            A->BytesXmit += NET_BUFFER_DATA_LENGTH(Nb);
            Nb = NET_BUFFER_NEXT_NB(Nb);
        }
    }

    NdisMSendNetBufferListsComplete(A->MiniportAdapterHandle, NblChain, Flags);
}

VOID
NTAPI
VwifiCancelSend(_In_ NDIS_HANDLE Ctx, _In_ PVOID CancelId)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(CancelId);
}
