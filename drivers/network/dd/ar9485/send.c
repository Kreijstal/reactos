/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     AR9300-family transmit engine.
 *
 * The AR9485 arbitrates transmission through ten DCUs feeding ten QCUs.  A
 * frame is described by an ar9003_txc control descriptor in DMA memory; the
 * descriptor's address is pushed into the QCU's hardware FIFO by writing
 * AR_QTXDP, and completion is reported into a separate DMA status ring
 * (AR_Q_STATUS_RING_START/END) rather than written back into the descriptor.
 *
 * Two register writes that the reset path does not perform are what make the
 * difference between silence and a transmitted frame, and both were found the
 * hard way on the ASUS X550DP:
 *
 *   - AR_DQCUMASK(i) must map DCU i onto QCU i (ath9k_hw_init_queues).  With
 *     it left at zero the DCU drives no QCU, AR_Q_TXE never clears, and the
 *     frame is never arbitrated onto the air.
 *   - AR_STA_ID0/AR_STA_ID1 must carry the station address, or the MAC has no
 *     identity to put in the ACK matcher.  Both read back zero after a plain
 *     PHY bring-up.
 *
 * Both are done in AR9485ProgramMacState() (mlme.c), which every reset path
 * calls.  This file owns the ring, the descriptor encoding and the status
 * ring; AR_Q_DESC_CRCCHK is left enabled and the pointer checksum is computed
 * for real, exactly as ar9003_calc_ptr_chksum() does upstream.
 */

#include "ar9485.h"
#include "ath9k/reg.h"
#include "ath9k/ath_reg.h"
#include "ath9k/mac_desc.h"

#define NDEBUG
#include <debug.h>

/* Transmit power in half-dB units, as AR_XmitPower0 wants it. */
#define AR9485_TX_POWER         0x28    /* 20 dBm */

/* Descriptor length in dwords, from ar9003_set_txdesc(); the 0x18 variant is
 * for AR9462/AR9565 only. */
#define AR9485_TXC_DESC_LEN     0x17

/* ------------------------------------------------------------------ */

static ULONG
AR9485CalcPtrChecksum(_In_ PAR9003_TXC Desc)
{
    /* Verbatim ar9003_calc_ptr_chksum(): the hardware verifies this over the
     * ten pointer/length words when AR_Q_DESC_CRCCHK is enabled. */
    ULONG Checksum = Desc->info + Desc->link
                   + Desc->data0 + Desc->ctl3
                   + Desc->data1 + Desc->ctl5
                   + Desc->data2 + Desc->ctl7
                   + Desc->data3 + Desc->ctl9;

    return ((Checksum & 0xffff) + (Checksum >> 16)) & AR_TxPtrChkSum;
}

static VOID
AR9485FreeShared(
    _In_ PAR9485_ADAPTER Adapter,
    _Inout_ PVOID *Virtual,
    _In_ PNDIS_PHYSICAL_ADDRESS Physical,
    _In_ ULONG Length)
{
    if (*Virtual != NULL)
    {
        NdisMFreeSharedMemory(Adapter->MiniportAdapterHandle, Length, FALSE,
                              *Virtual, *Physical);
        *Virtual = NULL;
    }
}

/*
 * Every DMA address the AR9485 takes is a 32-bit register write, so a buffer
 * placed above 4 GiB is unusable rather than merely slow.  Fail the
 * allocation loudly instead of programming a truncated address.
 */
static BOOLEAN
AR9485AllocShared(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ ULONG Length,
    _Out_ PVOID *Virtual,
    _Out_ PNDIS_PHYSICAL_ADDRESS Physical)
{
    *Virtual = NULL;
    Physical->QuadPart = 0;

    NdisMAllocateSharedMemory(Adapter->MiniportAdapterHandle, Length, FALSE,
                              Virtual, Physical);
    if (*Virtual == NULL)
        return FALSE;
    if (Physical->HighPart != 0)
    {
        NdisMFreeSharedMemory(Adapter->MiniportAdapterHandle, Length, FALSE,
                              *Virtual, *Physical);
        *Virtual = NULL;
        return FALSE;
    }
    NdisZeroMemory(*Virtual, Length);
    return TRUE;
}

NDIS_STATUS
AR9485InitializeTransmitter(_In_ PAR9485_ADAPTER Adapter)
{
    ULONG i;
    ULONG RingBytes = AR9485_TXS_RING_COUNT * sizeof(AR9003_TXS);

    NdisAllocateSpinLock(&Adapter->TxLock);
    Adapter->TxInitialized = TRUE;

    for (i = 0; i < AR9485_TX_SLOT_COUNT; ++i)
    {
        PAR9485_TX_SLOT Slot = &Adapter->TxSlots[i];

        if (!AR9485AllocShared(Adapter, sizeof(AR9003_TXC),
                               &Slot->Descriptor, &Slot->DescriptorPa) ||
            !AR9485AllocShared(Adapter, AR9485_TX_FRAME_SIZE,
                               &Slot->Frame, &Slot->FramePa))
        {
            DPRINT1("AR9485: TX slot %lu unavailable below 4GiB\n", i);
            return NDIS_STATUS_RESOURCES;
        }
    }

    if (!AR9485AllocShared(Adapter, RingBytes,
                           &Adapter->TxStatusRing, &Adapter->TxStatusRingPa))
    {
        DPRINT1("AR9485: TX status ring unavailable below 4GiB\n");
        return NDIS_STATUS_RESOURCES;
    }

    /* The ring base has to be in place before the next reset, not after it:
     * ath9k programs it from inside ath9k_hw_reset() and so, now, do we. */
    ar9485_hw_set_txstatus_ring(Adapter->HwContext,
                                Adapter->TxStatusRingPa.LowPart, RingBytes);

    Adapter->TxReady = TRUE;
    return NDIS_STATUS_SUCCESS;
}

VOID
AR9485ShutdownTransmitter(_In_ PAR9485_ADAPTER Adapter)
{
    ULONG i;

    /* MiniportInitializeEx can fail before the transmitter is set up at all,
     * and its failure path unwinds everything unconditionally. */
    if (!Adapter->TxInitialized)
        return;

    Adapter->TxReady = FALSE;

    /*
     * Stop the queues before a single descriptor or frame buffer is freed.
     * A QCU still arbitrating reads DMA memory the allocator has handed to
     * somebody else, which on this machine means corrupting a filesystem
     * rather than merely losing a frame.
     */
    if (Adapter->IoBase != NULL)
    {
        AR9485_WRITE_REG(Adapter, AR_Q_TXD, AR_Q_TXD_M);
        AR9485_WRITE_REG(Adapter, AR_Q_STATUS_RING_START, 0);
        AR9485_WRITE_REG(Adapter, AR_Q_STATUS_RING_END, 0);
        /* The DCUs have up to a frame time of work already fetched. */
        NdisMSleep(2000);
    }

    AR9485FlushTransmitQueue(Adapter);

    for (i = 0; i < AR9485_TX_SLOT_COUNT; ++i)
    {
        PAR9485_TX_SLOT Slot = &Adapter->TxSlots[i];

        AR9485FreeShared(Adapter, &Slot->Descriptor, &Slot->DescriptorPa,
                         sizeof(AR9003_TXC));
        AR9485FreeShared(Adapter, &Slot->Frame, &Slot->FramePa,
                         AR9485_TX_FRAME_SIZE);
    }
    AR9485FreeShared(Adapter, &Adapter->TxStatusRing, &Adapter->TxStatusRingPa,
                     AR9485_TXS_RING_COUNT * sizeof(AR9003_TXS));

    Adapter->TxInitialized = FALSE;
    NdisFreeSpinLock(&Adapter->TxLock);
}

/*
 * Point the hardware at the status ring and program QCU/DCU 0 the way
 * ath9k_hw_resettxqueue() does for a data queue.  Called after every PHY
 * reset, because the reset clears all of it.
 */
VOID
AR9485ResetTransmitQueue(_In_ PAR9485_ADAPTER Adapter)
{
    ULONG RingBytes = AR9485_TXS_RING_COUNT * sizeof(AR9003_TXS);

    if (!Adapter->TxReady)
        return;

    NdisZeroMemory(Adapter->TxStatusRing, RingBytes);
    Adapter->TxStatusIndex = 0;
    KeMemoryBarrier();

    AR9485_WRITE_REG(Adapter, AR_Q_STATUS_RING_START,
                     Adapter->TxStatusRingPa.LowPart);
    AR9485_WRITE_REG(Adapter, AR_Q_STATUS_RING_END,
                     Adapter->TxStatusRingPa.LowPart + RingBytes);

    /* INIT_CWMIN 15, INIT_CWMAX 1023, INIT_AIFS 2 (ath9k mac.h). */
    AR9485_WRITE_REG(Adapter, AR_DLCL_IFS(AR9485_TX_DCU),
                     SM(15, AR_D_LCL_IFS_CWMIN) |
                     SM(1023, AR_D_LCL_IFS_CWMAX) |
                     SM(2, AR_D_LCL_IFS_AIFS));

    /* INIT_SSH_RETRY / INIT_SLG_RETRY are 32 upstream; the per-frame retry
     * count in ctl13 is what actually bounds a transmission. */
    AR9485_WRITE_REG(Adapter, AR_DRETRY_LIMIT(AR9485_TX_DCU),
                     SM(32, AR_D_RETRY_LIMIT_STA_SH) |
                     SM(32, AR_D_RETRY_LIMIT_STA_LG) |
                     SM(10, AR_D_RETRY_LIMIT_FR_SH));

    AR9485_WRITE_REG(Adapter, AR_QMISC(AR9485_TX_QCU),
                     AR_Q_MISC_DCU_EARLY_TERM_REQ);
    AR9485_WRITE_REG(Adapter, AR_DMISC(AR9485_TX_DCU),
                     AR_D_MISC_CW_BKOFF_EN | AR_D_MISC_FRAG_WAIT_EN | 0x2);
    AR9485_WRITE_REG(Adapter, AR_DCHNTIME(AR9485_TX_DCU), 0);

    /* AR9300 and later verify the descriptor pointer checksum. */
    AR9485_WRITE_REG(Adapter, AR_Q_DESC_CRCCHK, AR_Q_DESC_CRCCHK_EN);

    NdisAcquireSpinLock(&Adapter->TxLock);
    Adapter->TxHead = Adapter->TxTail = Adapter->TxPending = 0;
    NdisReleaseSpinLock(&Adapter->TxLock);
}

/* ------------------------------------------------------------------ */

/*
 * Build the control descriptor for one frame and push it into QCU 0's FIFO.
 * Caller holds TxLock.
 *
 * BufferLength is what the DMA engine reads out of the frame buffer;
 * FrameLength is the length the MAC puts on the air, which for an encrypted
 * frame additionally covers the MIC the hardware appends, and always covers
 * the FCS.
 */
static VOID
AR9485SubmitDescriptor(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ PAR9485_TX_SLOT Slot,
    _In_ ULONG BufferLength,
    _In_ ULONG FrameLength,
    _In_ ULONG KeyIndex,
    _In_ ULONG EncryptType,
    _In_ ULONG RateCode,
    _In_ BOOLEAN NoAck)
{
    PAR9003_TXC Desc = (PAR9003_TXC)Slot->Descriptor;
    ULONG Ctl11, Ctl12;

    NdisZeroMemory(Desc, sizeof(*Desc));

    Desc->info = (ATHEROS_VENDOR_ID << AR_DescId_S) |
                 (1 << AR_TxRxDesc_S) |
                 (1 << AR_CtrlStat_S) |
                 (AR9485_TX_QCU << AR_TxQcuNum_S) |
                 AR9485_TXC_DESC_LEN;
    Desc->link  = 0;
    Desc->data0 = Slot->FramePa.LowPart;
    Desc->ctl3  = (BufferLength << AR_BufLen_S) & AR_BufLen;
    Desc->ctl10 = AR9485CalcPtrChecksum(Desc);

    /*
     * AR_TxIntrReq is what makes the hardware write a status descriptor into
     * the ring at all.  Measured on the chip: with the bit clear the QCU
     * transmits (AR_TFCNT and AR_ACK_FAIL both move) and reports only TXEOL,
     * the ring is never touched, AR9485ReapTransmitStatus() never retires a
     * slot, and TX wedges for good once TxPending reaches
     * AR9485_TX_SLOT_COUNT.  With the bit set, the same transmit deposits one
     * 36-byte AR9003_TXS carrying AR_TxDone and the QCU additionally reports
     * TXERR.  Upstream drives it from ATH9K_TXDESC_INTREQ, which ath9k sets
     * on the last buffer it pushes; this port reaps by polling and pushes one
     * descriptor at a time, so every frame asks for it.  AR_IMR leaves the
     * interrupt masked, so this costs a status write, not an interrupt.
     */
    Ctl11 = (FrameLength & AR_FrameLen) |
            SM(AR9485_TX_POWER, AR_XmitPower0) |
            AR_TxIntrReq |
            AR_ClrDestMask;
    Ctl12 = 0;
    if (KeyIndex != AR9485_KEY_NONE)
    {
        Ctl11 |= AR_DestIdxValid;
        Ctl12 |= SM(KeyIndex, AR_DestIdx);
    }
    if (NoAck)
        Ctl12 |= AR_NoAck;

    Desc->ctl11 = Ctl11;
    Desc->ctl12 = Ctl12;

    /* Two rate series: the requested rate, then 1 Mbit CCK as the floor.
     * AR_DurUpdateEna lets the MAC compute the duration field itself. */
    Desc->ctl13 = SM(4, AR_XmitDataTries0) |
                  SM(4, AR_XmitDataTries1) |
                  AR_DurUpdateEna;
    Desc->ctl14 = SM(RateCode, AR_XmitRate0) |
                  SM(AR9485_RATE_1M, AR_XmitRate1);
    Desc->ctl17 = SM(EncryptType, AR_EncrType);
    /* Every rate series names the transmit chain (ar9003_set_txdesc:
     * set11nChainSel).  Left at 0 the MAC hands OFDM frames to the PHY with
     * no chain selected: CCK still radiates, OFDM never retires. */
    Desc->ctl18 = SM(AR9485_TX_CHAINMASK, AR_ChainSel0) |
                  SM(AR9485_TX_CHAINMASK, AR_ChainSel1);
    Desc->ctl19 = AR_Not_Sounding;

    KeMemoryBarrier();

    /*
     * On an EDMA part AR_QTXDP is a FIFO push, not a head pointer: each write
     * queues one more descriptor chain behind the ones already in flight.
     * ath9k skips AR_Q_TXE here for EDMA, but the kick is what the first
     * transmit on this chip was proven with, and it is harmless on a QCU that
     * is already running.
     */
    AR9485_WRITE_REG(Adapter, AR_QTXDP(AR9485_TX_QCU), Slot->DescriptorPa.LowPart);
    AR9485_WRITE_REG(Adapter, AR_Q_TXE, 1 << AR9485_TX_QCU);
}

NDIS_STATUS
AR9485TransmitFrame(
    _In_ PAR9485_ADAPTER Adapter,
    _In_reads_bytes_(Length) PUCHAR Frame,
    _In_ ULONG Length,
    _In_ ULONG KeyIndex,
    _In_ ULONG RateCode,
    _In_ BOOLEAN NoAck,
    _In_opt_ PNET_BUFFER_LIST Nbl)
{
    PAR9485_TX_SLOT Slot;
    ULONG BufferLength, FrameLength, EncryptType = AR_ENCR_TYPE_CLEAR;

    if (!Adapter->TxReady || (Adapter->Flags & AR9485_FLAG_HALTING))
        return NDIS_STATUS_FAILURE;
    if (Length < DOT11_MAC_HEADER_LEN)
        return NDIS_STATUS_INVALID_LENGTH;

    BufferLength = Length;
    FrameLength = Length + DOT11_FCS_LEN;

    if (KeyIndex != AR9485_KEY_NONE)
    {
        /*
         * Only the pairwise CCMP key is ever used on transmit.  The caller
         * has already written the 8-byte CCMP header (PN + ExtIV) after the
         * MAC header -- the MAC encrypts with the PN it finds there and does
         * not generate one -- and the MIC is appended by the MAC, so it
         * counts towards the on-air length but not towards what DMA reads.
         */
        EncryptType = AR_ENCR_TYPE_AES;
        FrameLength += DOT11_CCMP_MIC_LEN;
    }

    if (BufferLength > AR9485_TX_FRAME_SIZE || FrameLength > AR_FrameLen)
        return NDIS_STATUS_INVALID_LENGTH;

    NdisAcquireSpinLock(&Adapter->TxLock);

    if (Adapter->TxPending >= AR9485_TX_SLOT_COUNT)
    {
        NdisReleaseSpinLock(&Adapter->TxLock);
        return NDIS_STATUS_RESOURCES;
    }

    Slot = &Adapter->TxSlots[Adapter->TxHead];
    NT_ASSERT(!Slot->InUse);

    NdisMoveMemory(Slot->Frame, Frame, Length);

    /*
     * Number the frame here, where every transmission funnels through, so one
     * counter covers management frames built by the MLME and data frames
     * built by nwifi alike.  Twelve bits of sequence, four of fragment; we
     * never fragment.  A hardware retry reuses this descriptor and so keeps
     * the number, which is what makes the AP's duplicate filter drop the
     * duplicate rather than the frame.
     */
    {
        USHORT SeqCtl = (USHORT)
            ((InterlockedIncrement(&Adapter->SequenceNumber) & 0x0fff) << 4);

        ((PUCHAR)Slot->Frame)[22] = (UCHAR)(SeqCtl & 0xff);
        ((PUCHAR)Slot->Frame)[23] = (UCHAR)(SeqCtl >> 8);
    }

    Slot->Nbl = Nbl;
    Slot->InUse = TRUE;

    AR9485SubmitDescriptor(Adapter, Slot, BufferLength, FrameLength,
                           KeyIndex, EncryptType, RateCode, NoAck);

    Adapter->TxHead = (Adapter->TxHead + 1) % AR9485_TX_SLOT_COUNT;
    Adapter->TxPending++;

    NdisReleaseSpinLock(&Adapter->TxLock);
    return NDIS_STATUS_SUCCESS;
}

/*
 * Drain the DMA status ring.  A single QCU retires frames in the order they
 * were pushed, so the n-th completion belongs to the n-th outstanding slot.
 */
VOID
AR9485ReapTransmitStatus(_In_ PAR9485_ADAPTER Adapter)
{
    PNET_BUFFER_LIST Completed = NULL;
    PAR9003_TXS Ring;

    if (!Adapter->TxReady)
        return;

    Ring = (PAR9003_TXS)Adapter->TxStatusRing;

    NdisAcquireSpinLock(&Adapter->TxLock);
    for (;;)
    {
        PAR9003_TXS Status = &Ring[Adapter->TxStatusIndex];
        PAR9485_TX_SLOT Slot;
        ULONG Status3;

        KeMemoryBarrier();
        if (!(Status->status8 & AR_TxDone))
            break;
        /* AR_TxDone is the publication point for the whole record; the rest
         * of it must not be read from before that bit was observed. */
        KeMemoryBarrier();

        if (MS(Status->ds_info, AR_DescId) != ATHEROS_VENDOR_ID ||
            MS(Status->ds_info, AR_TxRxDesc) != 1)
        {
            /* Not a transmit completion record: the ring is out of step with
             * the hardware, which no amount of further parsing will fix. */
            DPRINT1("AR9485: bogus TX status ds_info 0x%08lx at %lu\n",
                    Status->ds_info, Adapter->TxStatusIndex);
            NdisZeroMemory(Status, sizeof(*Status));
            Adapter->TxStatusIndex =
                (Adapter->TxStatusIndex + 1) % AR9485_TXS_RING_COUNT;
            continue;
        }

        Status3 = Status->status3;
        NdisZeroMemory(Status, sizeof(*Status));
        Adapter->TxStatusIndex =
            (Adapter->TxStatusIndex + 1) % AR9485_TXS_RING_COUNT;

        if (Adapter->TxPending == 0)
        {
            /* A completion with nothing outstanding means the ring was
             * re-armed under a frame still in the FIFO; drop it. */
            continue;
        }

        Slot = &Adapter->TxSlots[Adapter->TxTail];
        Adapter->TxTail = (Adapter->TxTail + 1) % AR9485_TX_SLOT_COUNT;
        Adapter->TxPending--;
        Slot->InUse = FALSE;

        if (Status3 & AR_FrmXmitOK)
        {
            Adapter->TxFrameCount++;
        }
        else
        {
            Adapter->TxFailureCount++;
            DPRINT("AR9485: TX failed status3 0x%08lx\n", Status3);
        }

        if (Slot->Nbl != NULL)
        {
            NET_BUFFER_LIST_STATUS(Slot->Nbl) =
                (Status3 & AR_FrmXmitOK) ? NDIS_STATUS_SUCCESS
                                         : NDIS_STATUS_FAILURE;
            NET_BUFFER_LIST_NEXT_NBL(Slot->Nbl) = Completed;
            Completed = Slot->Nbl;
            Slot->Nbl = NULL;
        }
    }
    NdisReleaseSpinLock(&Adapter->TxLock);

    if (Completed != NULL)
        NdisMSendNetBufferListsComplete(Adapter->MiniportAdapterHandle,
                                        Completed, 0);
}

/* Give every outstanding NBL back so a halt or a reset does not strand the
 * protocol stack waiting for a send completion the hardware will never
 * report. */
VOID
AR9485FlushTransmitQueue(_In_ PAR9485_ADAPTER Adapter)
{
    PNET_BUFFER_LIST Completed = NULL;
    ULONG i;

    NdisAcquireSpinLock(&Adapter->TxLock);
    for (i = 0; i < AR9485_TX_SLOT_COUNT; ++i)
    {
        PAR9485_TX_SLOT Slot = &Adapter->TxSlots[i];

        Slot->InUse = FALSE;
        if (Slot->Nbl != NULL)
        {
            NET_BUFFER_LIST_STATUS(Slot->Nbl) = NDIS_STATUS_FAILURE;
            NET_BUFFER_LIST_NEXT_NBL(Slot->Nbl) = Completed;
            Completed = Slot->Nbl;
            Slot->Nbl = NULL;
        }
    }
    Adapter->TxHead = Adapter->TxTail = Adapter->TxPending = 0;
    NdisReleaseSpinLock(&Adapter->TxLock);

    if (Completed != NULL)
        NdisMSendNetBufferListsComplete(Adapter->MiniportAdapterHandle,
                                        Completed, 0);
}

/* ------------------------------------------------------------------ */

/* Copy one NET_BUFFER, which may span several MDLs, into a flat buffer. */
static ULONG
AR9485LinearizeNetBuffer(
    _In_ PNET_BUFFER NetBuffer,
    _Out_writes_bytes_(MaxLength) PUCHAR Destination,
    _In_ ULONG MaxLength)
{
    PMDL Mdl = NET_BUFFER_CURRENT_MDL(NetBuffer);
    ULONG Offset = NET_BUFFER_CURRENT_MDL_OFFSET(NetBuffer);
    ULONG Remaining = NET_BUFFER_DATA_LENGTH(NetBuffer);
    ULONG Copied = 0;

    if (Remaining > MaxLength)
        return 0;

    while (Mdl != NULL && Remaining > 0)
    {
        PUCHAR Va = MmGetSystemAddressForMdlSafe(Mdl, LowPagePriority);
        ULONG MdlLength = MmGetMdlByteCount(Mdl);
        ULONG Chunk;

        if (Va == NULL)
            return 0;
        if (Offset >= MdlLength)
        {
            Offset -= MdlLength;
            Mdl = Mdl->Next;
            continue;
        }

        Chunk = MdlLength - Offset;
        if (Chunk > Remaining)
            Chunk = Remaining;
        NdisMoveMemory(Destination + Copied, Va + Offset, Chunk);
        Copied += Chunk;
        Remaining -= Chunk;
        Offset = 0;
        Mdl = Mdl->Next;
    }

    return Copied;
}

/*
 * Does this outbound data frame have to leave in the clear?
 *
 * nwifi marks the supplicant's own EAPOL sends with DOT11_EXEMPT_ALWAYS, but
 * its ordinary 802.3 translation path hardcodes DOT11_EXEMPT_NO_EXEMPTION, so
 * the send context alone is not enough to recognise every EAPOL frame.  The
 * SNAP EtherType is, and getting this wrong encrypts message 2 of the 4-way
 * handshake with a key the AP does not have yet.
 */
static BOOLEAN
AR9485FrameIsExempt(
    _In_ PNET_BUFFER_LIST Nbl,
    _In_reads_bytes_(Length) PUCHAR Frame,
    _In_ ULONG Length)
{
    PDOT11_EXTSTA_SEND_CONTEXT SendContext =
        (PDOT11_EXTSTA_SEND_CONTEXT)NET_BUFFER_LIST_INFO(Nbl,
                                                         MediaSpecificInformation);
    PDOT11_SNAP_HEADER Snap;

    if (SendContext != NULL &&
        SendContext->usExemptionActionType != DOT11_EXEMPT_NO_EXEMPTION)
        return TRUE;

    if (Length < DOT11_MAC_HEADER_LEN + sizeof(DOT11_SNAP_HEADER))
        return FALSE;

    Snap = (PDOT11_SNAP_HEADER)(Frame + DOT11_MAC_HEADER_LEN);
    if (Snap->Dsap != 0xAA || Snap->Ssap != 0xAA || Snap->Control != 0x03)
        return FALSE;

    return RtlUshortByteSwap(Snap->EtherType) == DOT11_ETHERTYPE_EAPOL;
}

VOID NTAPI
AR9485SendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PAR9485_ADAPTER Adapter = (PAR9485_ADAPTER)MiniportAdapterContext;
    PNET_BUFFER_LIST Current = NetBufferLists;
    PNET_BUFFER_LIST Failed = NULL;
    ULONG CompleteFlags = 0;

    UNREFERENCED_PARAMETER(PortNumber);

    if (SendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL)
        CompleteFlags |= NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL;

    while (Current != NULL)
    {
        PNET_BUFFER_LIST Next = NET_BUFFER_LIST_NEXT_NBL(Current);
        PNET_BUFFER NetBuffer = NET_BUFFER_LIST_FIRST_NB(Current);
        UCHAR Frame[AR9485_TX_FRAME_SIZE];
        ULONG Length;
        ULONG KeyIndex = AR9485_KEY_NONE;
        NDIS_STATUS Status = NDIS_STATUS_FAILURE;

        NET_BUFFER_LIST_NEXT_NBL(Current) = NULL;

        /*
         * nwifi hands down one 802.11 frame per NBL.  A chain of several
         * NET_BUFFERs would need one descriptor each and a way to complete
         * the NBL only after the last of them; nothing above us builds one,
         * so reject it rather than transmit a truncated frame.
         */
        if (Adapter->MlmeState != AR9485MlmeAssociated ||
            NetBuffer == NULL || NET_BUFFER_NEXT_NB(NetBuffer) != NULL)
            goto Reject;

        Length = NET_BUFFER_DATA_LENGTH(NetBuffer);
        if (Length < DOT11_MAC_HEADER_LEN)
            goto Reject;

        /* Leave room for the CCMP header the MAC inserts, so the shift
         * below never runs off the end of the staging buffer. */
        if (Length + DOT11_CCMP_HEADER_LEN > sizeof(Frame))
            goto Reject;
        if (AR9485LinearizeNetBuffer(NetBuffer, Frame, sizeof(Frame)) != Length)
            goto Reject;

        if (Adapter->PairwiseKeyValid &&
            !AR9485FrameIsExempt(Current, Frame, Length))
        {
            /* Open the CCMP header gap between the MAC header and the
             * payload and fill it in software.  ath9k hardware never
             * generates the IV: mac80211 sets IEEE80211_KEY_FLAG_GENERATE_IV
             * and ccmp_pn2hdr() writes it, the MAC then encrypts and
             * appends the MIC using the PN it finds in the frame.  Layout
             * (802.11-2012 11.4.3.2): PN0 PN1 rsvd (ExtIV|KeyID<<6) PN2..PN5.
             * A zero header would go out with PN 0 and no ExtIV bit, which
             * every receiver discards as a replay -- what kept the AP from
             * ever forwarding a single frame of ours. */
            PUCHAR Ccmp = Frame + DOT11_MAC_HEADER_LEN;
            ULONG64 Pn = (ULONG64)InterlockedIncrement64((LONG64 *)&Adapter->PairwisePn) - 1;

            NdisMoveMemory(Ccmp + DOT11_CCMP_HEADER_LEN, Ccmp,
                           Length - DOT11_MAC_HEADER_LEN);
            Ccmp[0] = (UCHAR)Pn;
            Ccmp[1] = (UCHAR)(Pn >> 8);
            Ccmp[2] = 0;
            Ccmp[3] = DOT11_CCMP_EXT_IV;      /* pairwise key id 0 */
            Ccmp[4] = (UCHAR)(Pn >> 16);
            Ccmp[5] = (UCHAR)(Pn >> 24);
            Ccmp[6] = (UCHAR)(Pn >> 32);
            Ccmp[7] = (UCHAR)(Pn >> 40);
            Frame[1] |= DOT11_FC1_PROTECTED;
            Length += DOT11_CCMP_HEADER_LEN;
            KeyIndex = AR9485_KEY_PAIRWISE;
        }

        Status = AR9485TransmitFrame(Adapter, Frame, Length, KeyIndex,
                                     AR9485_RATE_11M, FALSE, Current);
        if (Status == NDIS_STATUS_SUCCESS)
        {
            /* The reaper owns the NBL now. */
            Current = Next;
            continue;
        }

Reject:
        NET_BUFFER_LIST_STATUS(Current) = Status;
        NET_BUFFER_LIST_NEXT_NBL(Current) = Failed;
        Failed = Current;
        Current = Next;
    }

    if (Failed != NULL)
        NdisMSendNetBufferListsComplete(Adapter->MiniportAdapterHandle,
                                        Failed, CompleteFlags);
}

VOID NTAPI
AR9485CancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId)
{
    /* Frames are handed straight to the hardware and retired by the status
     * ring; there is no queue to walk for a cancel id. */
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
}
