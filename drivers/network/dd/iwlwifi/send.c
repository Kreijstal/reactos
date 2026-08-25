/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Transmit path placeholder.
 *
 * There are no TX rings until the firmware is running, so anything handed
 * down here is completed as a failure immediately.  Silently dropping it
 * would strand the owner's NET_BUFFER_LISTs forever.
 */

#include "iwlwifi.h"

#define NDEBUG
#include <debug.h>

VOID NTAPI
IwlSendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PIWL_ADAPTER Adapter = (PIWL_ADAPTER)MiniportAdapterContext;
    PNET_BUFFER_LIST Current;
    ULONG CompleteFlags = 0;

    UNREFERENCED_PARAMETER(PortNumber);

    /* Propagate the caller's IRQL promise to the completion, or NDIS may
     * lower IRQL underneath a DISPATCH_LEVEL caller. */
    if (NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendFlags))
        CompleteFlags |= NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL;

    for (Current = NetBufferLists; Current != NULL; Current = NET_BUFFER_LIST_NEXT_NBL(Current))
        NET_BUFFER_LIST_STATUS(Current) = NDIS_STATUS_FAILURE;

    NdisMSendNetBufferListsComplete(Adapter->MiniportAdapterHandle,
                                    NetBufferLists,
                                    CompleteFlags);
}

VOID NTAPI
IwlCancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId)
{
    /* Nothing is ever queued, so there is nothing to cancel. */
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
}
