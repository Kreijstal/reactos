/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Phase 1a TX stub.  Drops every NBL with NDIS_STATUS_FAILURE
 *              so the protocol stack treats the link as down.  Real
 *              MiniportSendNetBufferLists lands with Phase 4b together
 *              with the ath9k xmit.c port.
 */

#include "ar9485.h"

#define NDEBUG
#include <debug.h>

VOID NTAPI
AR9485SendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PAR9485_ADAPTER Adapter = (PAR9485_ADAPTER)MiniportAdapterContext;
    PNET_BUFFER_LIST Current = NetBufferLists;
    PNET_BUFFER_LIST Next;
    ULONG CompleteFlags = 0;

    UNREFERENCED_PARAMETER(PortNumber);

    if (SendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL)
        CompleteFlags |= NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL;

    while (Current != NULL)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(Current);
        NET_BUFFER_LIST_STATUS(Current) = NDIS_STATUS_FAILURE;
        Current = Next;
    }

    NdisMSendNetBufferListsComplete(Adapter->MiniportAdapterHandle,
                                    NetBufferLists,
                                    CompleteFlags);
}

VOID NTAPI
AR9485CancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
}
