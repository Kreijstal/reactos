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
    USHORT status;

    status = NICInterruptRecognized(adapter, InterruptRecognized);

    if (!(*InterruptRecognized))
    {
        *QueueMiniportHandleInterrupt = FALSE;
        return;
    }

    adapter->InterruptPending |= status;

    /* Qualify link-change on the freshly read status bits only -- the
     * accumulated InterruptPending would re-indicate media status on every
     * subsequent interrupt after the first link event. */
    if (status & R_I_LINKCHG)
    {
        adapter->LinkChange = TRUE;
        NICUpdateLinkStatus(adapter);
    }

    /* Ack interrupts so the line drops; actual servicing happens at DISPATCH. */
    NICAcknowledgeInterrupts(adapter, status);
    *QueueMiniportHandleInterrupt = TRUE;
}

/* NdisMSynchronizeWithInterrupt callback context: the ISR does
 * InterruptPending |= status at DIRQL, so the DPC's read-and-clear must run
 * synchronized with the ISR or an ISR update between the read and the clear
 * is lost (a nonatomic RMW race -- rtl8139 carries the same FIXME). */
typedef struct _RTL_ISR_SYNC_CONTEXT {
    PRTL_ADAPTER Adapter;
    USHORT Pending;
    BOOLEAN LinkChange;
} RTL_ISR_SYNC_CONTEXT, *PRTL_ISR_SYNC_CONTEXT;

static BOOLEAN
NTAPI
RtlConsumePendingInterrupts (
    IN PVOID SynchronizeContext
    )
{
    PRTL_ISR_SYNC_CONTEXT ctx = (PRTL_ISR_SYNC_CONTEXT)SynchronizeContext;
    PRTL_ADAPTER adapter = ctx->Adapter;

    ctx->Pending = adapter->InterruptPending;
    ctx->LinkChange = adapter->LinkChange;
    adapter->InterruptPending = 0;
    adapter->LinkChange = FALSE;
    return TRUE;
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
        if (*(volatile ULONG *)&d->opts1 & DESC_OWN)
            break;

        /* Order the OWN check before any further descriptor reads (Linux
         * rtl_tx: rmb after reading DescOwn). */
        KeMemoryBarrier();

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

    /* 8168 erratum (Linux rtl_tx): "TxPoll requests are lost when the Tx
     * packets are too close."  If descriptors are still owned by the chip
     * after reclaiming, kick the doorbell again so a swallowed poll request
     * cannot strand the queue. */
    if (Adapter->TxFull || Adapter->TxConsumer != Adapter->TxProducer)
        RtlWriteReg8(Adapter, R_TXPOLL, B_TXP_NPQ);
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
        ULONG opts1 = *(volatile ULONG *)&d->opts1;
        ULONG length;
        PUCHAR buf;

        /* Still owned by NIC -> nothing more to receive. */
        if (opts1 & DESC_OWN)
            break;

        /* Order the OWN check before reading the rest of the descriptor /
         * buffer (Linux rtl_rx: dma_rmb after reading DescOwn). */
        KeMemoryBarrier();

        /* Error policy modeled on Linux rtl_rx: RxRES summarizes all receive
         * errors (RUNT/CRC frame contents may be corrupt); drop the frame.
         * Bit 27 is MAR (multicast received) on the 8168 -- NOT an error. */
        if (opts1 & RXD_RES)
        {
            if (opts1 & RXD_CRC)
                Adapter->ReceiveCrcError++;
            Adapter->ReceiveError++;
            goto next;
        }

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

        /* RX length field is 14 bits; bits 15/14 are checksum status. */
        length = opts1 & RXD_LEN_MASK;
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
    RTL_ISR_SYNC_CONTEXT sync;

    NdisDprAcquireSpinLock(&adapter->Lock);

    /* Snapshot-and-clear the ISR-accumulated state atomically wrt the ISR;
     * every event group (TX, RX, link, error) is consumed exactly once. */
    sync.Adapter = adapter;
    sync.Pending = 0;
    sync.LinkChange = FALSE;
    NdisMSynchronizeWithInterrupt(&adapter->Interrupt,
                                  RtlConsumePendingInterrupts,
                                  &sync);

    if (sync.LinkChange)
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
    }

    if (sync.Pending & (R_I_TXOK | R_I_TXERR | R_I_TXDESCUNAVAIL))
        RtlServiceTx(adapter);

    if (sync.Pending & (R_I_RXOK | R_I_RXERR | R_I_RXOVRFLW | R_I_RXFIFOOVR))
        RtlServiceRx(adapter);

    /* Linux rtl8169_interrupt: on VER_11 an RxFIFOOver wedges the chip --
     * stop and schedule a reset.  Our equivalent: flag it so the next
     * MiniportCheckForHang poll returns TRUE and NDIS calls MiniportReset. */
    if ((sync.Pending & R_I_RXFIFOOVR) &&
        adapter->MacVersion == RTL_MAC_VER_11)
    {
        adapter->HwHang = TRUE;
    }

    if (sync.Pending & (R_I_SYSERR | R_I_PCSTMOUT))
    {
        NDIS_DbgPrint(MIN_TRACE,
            ("RTL8168: system error interrupt (pending=0x%04x)\n",
             sync.Pending));
        if (sync.Pending & R_I_SYSERR)
            adapter->HwHang = TRUE;
    }

    NdisDprReleaseSpinLock(&adapter->Lock);
}

/*
 * NDIS polls this roughly every 2 seconds; returning TRUE makes NDIS call
 * MiniportReset.  Hang conditions:
 *   - a SYSERR interrupt was seen (PCI error; Linux resets in
 *     rtl8169_pcierr_interrupt),
 *   - the VER_11 RxFIFOOver wedge (see MiniportHandleInterrupt),
 *   - TX watchdog: packets queued but no TxOK progress across two
 *     consecutive polls.
 */
BOOLEAN
NTAPI
MiniportCheckForHang (
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    PRTL_ADAPTER adapter = (PRTL_ADAPTER)MiniportAdapterContext;
    BOOLEAN hang;
    BOOLEAN txPending;

    NdisAcquireSpinLock(&adapter->Lock);

    hang = adapter->HwHang;

    txPending = adapter->TxFull ||
                adapter->TxConsumer != adapter->TxProducer;
    if (txPending &&
        adapter->CheckForHangTxPending &&
        adapter->TransmitOk == adapter->CheckForHangTxOk)
    {
        hang = TRUE;
    }

    adapter->CheckForHangTxPending = txPending;
    adapter->CheckForHangTxOk = adapter->TransmitOk;

    NdisReleaseSpinLock(&adapter->Lock);
    return hang;
}
