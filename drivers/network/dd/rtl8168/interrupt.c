/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Realtek 8168/8111 driver
 * FILE:        interrupt.c
 * PURPOSE:     ISR + DPC; descriptor-ring service for TX completions and RX delivery
 */

#include "nic.h"

#define NDEBUG
#include <debug.h>

VOID
NTAPI
MiniportISR (
    OUT PBOOLEAN InterruptRecognized,
    OUT PBOOLEAN QueueMiniportHandleInterrupt,
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    PRTL_ADAPTER adapter = (PRTL_ADAPTER)MiniportAdapterContext;

    adapter->InterruptPending |= NICInterruptRecognized(adapter, InterruptRecognized);

    if (!(*InterruptRecognized))
    {
        *QueueMiniportHandleInterrupt = FALSE;
        return;
    }

    if (adapter->InterruptPending & R_I_LINKCHG)
    {
        adapter->LinkChange = TRUE;
        NICUpdateLinkStatus(adapter);
    }

    /* Ack interrupts so the line drops; actual servicing happens at DISPATCH. */
    NICAcknowledgeInterrupts(adapter);
    *QueueMiniportHandleInterrupt = TRUE;
}

static VOID
RtlServiceTx (
    IN PRTL_ADAPTER Adapter
    )
{
    while (Adapter->TxFull || Adapter->TxConsumer != Adapter->TxProducer)
    {
        PRTL_DESC d = &Adapter->TxRing[Adapter->TxConsumer];

        /* NIC still owns this slot -- stop here, more interrupts will come. */
        if (d->opts1 & DESC_OWN)
            break;

        /* TX completion -- the chip clears OWN but does not surface per-frame
         * status the way RTL8139 did.  We treat OWN-clear as successful and
         * bump errors via the missed-packet / abort statistics if we cared. */
        Adapter->TransmitOk++;

        /* Reinit descriptor for next use -- preserve EOR bit on the tail slot. */
        d->opts1 = (Adapter->TxConsumer == TX_DESC_COUNT - 1) ? DESC_EOR : 0;
        d->opts2 = 0;

        Adapter->TxConsumer = (Adapter->TxConsumer + 1) % TX_DESC_COUNT;
        Adapter->TxFull = FALSE;
    }
}

static VOID
RtlServiceRx (
    IN PRTL_ADAPTER Adapter
    )
{
    ULONG indications = 0;

    for (;;)
    {
        PRTL_DESC d = &Adapter->RxRing[Adapter->RxConsumer];
        ULONG opts1 = d->opts1;
        ULONG length;
        PUCHAR buf;

        /* Still owned by NIC -> nothing more to receive. */
        if (opts1 & DESC_OWN)
            break;

        if (!(opts1 & DESC_FS) || !(opts1 & DESC_LS))
        {
            /* Multi-segment receive shouldn't happen for non-jumbo configs.
             * Drop the descriptor and keep going. */
            NDIS_DbgPrint(MIN_TRACE,
                ("RTL8168: dropping multi-segment RX desc %lu opts1=0x%08lx\n",
                 Adapter->RxConsumer, opts1));
            Adapter->ReceiveError++;
            goto next;
        }

        length = opts1 & DESC_LEN_MASK;
        if (length < 14 || length > RX_BUF_SIZE)
        {
            NDIS_DbgPrint(MIN_TRACE,
                ("RTL8168: bogus RX length %lu on desc %lu\n",
                 length, Adapter->RxConsumer));
            Adapter->ReceiveError++;
            goto next;
        }

        /* The chip appends a 4-byte FCS; trim it from the length we report. */
        if (length >= 4)
            length -= 4;

        if (opts1 & (RXD_CRC | RXD_FAE | RXD_RUNT | RXD_RES | RXD_RWT))
        {
            if (opts1 & RXD_CRC)
                Adapter->ReceiveCrcError++;
            else if (opts1 & RXD_FAE)
                Adapter->ReceiveAlignmentError++;
            Adapter->ReceiveError++;
            goto next;
        }

        buf = Adapter->RxBuffers + (Adapter->RxConsumer * RX_BUF_SIZE);

        NdisMEthIndicateReceive(Adapter->MiniportAdapterHandle,
                                NULL,
                                (PVOID)buf,
                                sizeof(ETH_HEADER),
                                (PVOID)(buf + sizeof(ETH_HEADER)),
                                length - sizeof(ETH_HEADER),
                                length - sizeof(ETH_HEADER));
        Adapter->ReceiveOk++;
        indications++;

next:
        NICRefillRxDescriptor(Adapter, Adapter->RxConsumer);
        Adapter->RxConsumer = (Adapter->RxConsumer + 1) % RX_DESC_COUNT;
    }

    if (indications)
        NdisMEthIndicateReceiveComplete(Adapter->MiniportAdapterHandle);
}

VOID
NTAPI
MiniportHandleInterrupt (
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    PRTL_ADAPTER adapter = (PRTL_ADAPTER)MiniportAdapterContext;

    NdisDprAcquireSpinLock(&adapter->Lock);

    if (adapter->LinkChange)
    {
        NdisDprReleaseSpinLock(&adapter->Lock);
        NdisMIndicateStatus(adapter->MiniportAdapterHandle,
                            adapter->MediaState == NdisMediaStateConnected ?
                                NDIS_STATUS_MEDIA_CONNECT :
                                NDIS_STATUS_MEDIA_DISCONNECT,
                            NULL,
                            0);
        NdisMIndicateStatusComplete(adapter->MiniportAdapterHandle);
        NdisDprAcquireSpinLock(&adapter->Lock);
        adapter->LinkChange = FALSE;
    }

    if (adapter->InterruptPending & (R_I_TXOK | R_I_TXERR | R_I_TXDESCUNAVAIL))
    {
        RtlServiceTx(adapter);
        adapter->InterruptPending &= ~(R_I_TXOK | R_I_TXERR | R_I_TXDESCUNAVAIL);
    }

    if (adapter->InterruptPending & (R_I_RXOK | R_I_RXERR | R_I_RXOVRFLW | R_I_RXFIFOOVR))
    {
        RtlServiceRx(adapter);
        adapter->InterruptPending &= ~(R_I_RXOK | R_I_RXERR | R_I_RXOVRFLW | R_I_RXFIFOOVR);
    }

    if (adapter->InterruptPending & (R_I_SYSERR | R_I_PCSTMOUT))
    {
        NDIS_DbgPrint(MIN_TRACE,
            ("RTL8168: system error interrupt (pending=0x%04x)\n",
             adapter->InterruptPending));
        adapter->InterruptPending &= ~(R_I_SYSERR | R_I_PCSTMOUT);
    }

    NdisDprReleaseSpinLock(&adapter->Lock);
}
