/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Native 802.11 transmit path.
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

    for (Current = NetBufferLists; Current != NULL;
         Current = NET_BUFFER_LIST_NEXT_NBL(Current))
    {
        PNET_BUFFER Nb = NET_BUFFER_LIST_FIRST_NB(Current);
        PDOT11_EXTSTA_SEND_CONTEXT Context;
        PUCHAR Frame;
        ULONG Length;
        UCHAR Storage[IWL_GEN3_RX_BUFFER_SIZE];
        BOOLEAN Encrypt = TRUE;

        NET_BUFFER_LIST_STATUS(Current) = NDIS_STATUS_FAILURE;
        if (Nb == NULL || NET_BUFFER_NEXT_NB(Nb) != NULL)
            continue;
        Length = NET_BUFFER_DATA_LENGTH(Nb);
        if (Length < 24 || Length > sizeof(Storage))
            continue;
        Frame = NdisGetDataBuffer(Nb, Length, Storage, 1, 0);
        if (Frame == NULL)
            continue;
        Context = (PDOT11_EXTSTA_SEND_CONTEXT)
            NET_BUFFER_LIST_INFO(Current, MediaSpecificInformation);
        if (Context != NULL &&
            Context->usExemptionActionType == DOT11_EXEMPT_ALWAYS)
            Encrypt = FALSE;
        /* EAPOL is always clear until the pairwise key exists.  Preserve the
         * semantic exemption even on NDIS paths that lose the media-specific
         * context while forwarding a protocol NBL to the lower miniport. */
        if (Length >= 32 && Frame[30] == 0x88 && Frame[31] == 0x8e)
            Encrypt = FALSE;
        if (IwlTransmitFrame(Adapter, Frame, (USHORT)Length, Encrypt, NULL))
        {
            NET_BUFFER_LIST_STATUS(Current) = NDIS_STATUS_SUCCESS;
            if (!Encrypt)
                DPRINT1("iwlwifi: submitted clear data frame len=%lu "
                        "ether-type=%02x%02x\n", Length,
                        Length >= 32 ? Frame[30] : 0,
                        Length >= 32 ? Frame[31] : 0);
        }
    }

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
