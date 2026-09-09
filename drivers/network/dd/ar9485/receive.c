/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     AR9300-EDMA receive path: beacon scanning, management-frame
 *              delivery to the MLME, and data-frame indication to NDIS.
 *
 * The AR9300 family receives into a FIFO of host buffers whose addresses are
 * handed to AR_LP_RXDP one at a time.  Each buffer carries a 48-byte status
 * block ahead of the frame; the hardware retires them in the order they were
 * posted, so a single index is enough to walk the ring, and a buffer is
 * re-posted the moment its frame has been consumed.
 *
 * Everything here runs on the chip worker (mlme.c).  That is what lets the
 * MLME poll for an authentication response inline instead of needing a
 * rendezvous with a separate receive thread.
 */

#include "ar9485.h"
#include "ath9k/reg.h"
#include "ath9k/ath_reg.h"
#include "ath9k/mac_desc.h"
#include "tkiprx.h"

#define NDEBUG
#include <debug.h>

/* ath9k_rx_filter: unicast, multicast, broadcast, beacons.  Probe responses
 * are management frames addressed to us and need no filter bit of their
 * own. */
#define RX_FILTER_STATION   (0x1 | 0x2 | 0x4 | 0x10)
/* Scanning additionally wants other stations' probe requests, which is how a
 * hidden network can still be spotted. */
#define RX_FILTER_SCAN      (RX_FILTER_STATION | 0x80)

#define RX_DESC_ID          0x168c
#define RX_DESC_ID_MASK     0xffff0000
#define RX_DESC_ID_SHIFT    16
#define RX_DESC_CONTROL_STATUS  0x00004000
#define RX_DESC_TX          0x00008000

/*
 * AR_RxRSSICombined is dB above the noise floor, not dBm.  The scan on boot 66
 * produced 4..49 for 37 real BSSes, and everything downstream -- lRSSI,
 * uLinkQuality here, and WlanSvcRssiToQuality() in wlansvc -- reads its input
 * as dBm, so passing the raw value pinned every network in the list at 100%
 * signal.  ath9k converts identically in ath9k_hw_process_rxdesc_edma():
 * rxs->rs_rssi is the same field, and ath9k_process_rssi() then reports
 * rxs->signal = ah->noise + rs_rssi.
 *
 * We do not read the calibrated noise floor back out of AR_PHY_CCA yet, so
 * this uses ath9k's own fallback for that case, ATH_DEFAULT_NOISE_FLOOR.
 * 0x80 is ATH9K_RSSI_BAD: the hardware could not measure this frame, which is
 * reported as the noise floor rather than as a plausible-looking level.
 */
#define AR9485_DEFAULT_NOISE_FLOOR  (-95)
#define AR9485_RSSI_BAD             0x80

static LONG RssiToDbm(ULONG Combined)
{
    if (Combined == AR9485_RSSI_BAD)
        return AR9485_DEFAULT_NOISE_FLOOR;
    return AR9485_DEFAULT_NOISE_FLOOR + (LONG)(CHAR)Combined;
}

typedef struct _AR9485_RX_STATUS {
    ULONG Info, S1, S2, S3, S4, S5, S6, S7, S8, S9, S10;
    volatile ULONG S11;
} AR9485_RX_STATUS, *PAR9485_RX_STATUS;

C_ASSERT(sizeof(AR9485_RX_STATUS) <= AR9485_RX_STATUS_SIZE);

static ULONG ChannelFrequency(ULONG Channel)
{
    return Channel == 14 ? 2484 : 2407 + Channel * 5;
}

/* ================================================================== *
 *  Ring management
 * ================================================================== */

static VOID PostRxBuffer(PAR9485_ADAPTER Adapter, ULONG Index)
{
    RtlZeroMemory(Adapter->RxBuffers[Index].VirtualAddress,
                  AR9485_RX_STATUS_SIZE);
    KeMemoryBarrier();
    AR9485_WRITE_REG(Adapter, AR_LP_RXDP,
                     Adapter->RxBuffers[Index].PhysicalAddress.LowPart);
}

VOID AR9485ArmReceiver(PAR9485_ADAPTER Adapter)
{
    ULONG i;
    ULONG Filter = Adapter->ScanInProgress ? RX_FILTER_SCAN : RX_FILTER_STATION;

    AR9485_WRITE_REG(Adapter, AR_DATABUF_SIZE,
                     AR9485_RX_BUFFER_SIZE - AR9485_RX_STATUS_SIZE);
    AR9485_WRITE_REG(Adapter, AR_RX_FILTER, Filter);
    AR9485_WRITE_REG(Adapter, AR_MCAST_FIL0, MAXULONG);
    AR9485_WRITE_REG(Adapter, AR_MCAST_FIL1, MAXULONG);
    AR9485_WRITE_REG(Adapter, AR_DIAG_SW,
                     AR9485_READ_REG(Adapter, AR_DIAG_SW) &
                     ~(AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT));

    for (i = 0; i < Adapter->RxBufferCount; ++i)
        RtlZeroMemory(Adapter->RxBuffers[i].VirtualAddress,
                      AR9485_RX_STATUS_SIZE);
    KeMemoryBarrier();
    for (i = 0; i < Adapter->RxBufferCount; ++i)
        AR9485_WRITE_REG(Adapter, AR_LP_RXDP,
                         Adapter->RxBuffers[i].PhysicalAddress.LowPart);

    Adapter->RxIndex = 0;
    Adapter->RxArmed = TRUE;

    /* ar9003_hw_rx_enable() writes zero here; on an AR9300 part that is what
     * starts the receive engine, not AR_CR_RXE. */
    AR9485_WRITE_REG(Adapter, AR_CR, 0);
}

/* ================================================================== *
 *  Scan bookkeeping
 * ================================================================== */

static VOID RememberBss(PAR9485_ADAPTER Adapter, PUCHAR Frame, ULONG Length,
                        ULONG Frequency, LONG Rssi)
{
    PAR9485_BSS Bss = NULL;
    ULONG i, IeLength;
    if (Length < 36)
        return;
    for (i = 0; i < Adapter->BssCount; ++i) {
        if (RtlCompareMemory(Adapter->Bss[i].Bssid, Frame + 16, 6) == 6) {
            Bss = &Adapter->Bss[i];
            break;
        }
    }
    if (!Bss) {
        if (Adapter->BssCount == AR9485_MAX_BSS)
            return;
        Bss = &Adapter->Bss[Adapter->BssCount++];
        RtlZeroMemory(Bss, sizeof(*Bss));
        RtlCopyMemory(Bss->Bssid, Frame + 16, 6);
    }
    Bss->Rssi = Rssi;
    Bss->BeaconPeriod = Frame[32] | ((USHORT)Frame[33] << 8);
    Bss->CapabilityInformation = Frame[34] | ((USHORT)Frame[35] << 8);
    IeLength = min(Length - 36, (ULONG)AR9485_MAX_BSS_IE_SIZE);
    Bss->IeLength = IeLength;
    RtlCopyMemory(Bss->Ies, Frame + 36, IeLength);

    /* The channel a BSS lives on is the one its beacon names in the DS
     * Parameter Set IE (id 3), NOT the channel this radio was tuned to when
     * the beacon arrived: 2.4 GHz channels overlap, a strong AP is heard two
     * or three channels either side of its own, the scan sweeps 1..13 and
     * the LAST hearing used to win -- so a channel-6 AP was remembered on
     * channel 8 and the connect then tuned 10 MHz off it, where the AP never
     * decodes a frame from us.  Fall back to the receive channel only for a
     * beacon without a DS Parameter Set. */
    Bss->ChannelMHz = Frequency;
    {
        PUCHAR Ie = Bss->Ies;
        ULONG Left = IeLength;
        while (Left >= 2 && (ULONG)Ie[1] + 2 <= Left) {
            if (Ie[0] == 3 && Ie[1] == 1) {
                if (Ie[2] >= 1 && Ie[2] <= 14)
                    Bss->ChannelMHz = ChannelFrequency(Ie[2]);
                break;
            }
            Left -= Ie[1] + 2;
            Ie += Ie[1] + 2;
        }
    }
}

/* ================================================================== *
 *  Data-frame indication
 * ================================================================== */

/*
 * Hand one 802.11 data frame to NDIS.  The indication uses
 * NDIS_RECEIVE_FLAGS_RESOURCES, which makes it synchronous: NDIS copies what
 * it needs before returning and never calls MiniportReturnNetBufferLists, so
 * the DMA buffer can go straight back onto the ring afterwards and there is
 * no in-flight-buffer lifetime to track.
 */
static VOID IndicateDataFrame(PAR9485_ADAPTER Adapter, PUCHAR Frame,
                              ULONG Length, LONG Rssi, ULONG Rate)
{
    PMDL Mdl;
    PNET_BUFFER_LIST Nbl;
    DOT11_EXTSTA_RECV_CONTEXT RecvContext;

    if (Adapter->RxNblPool == NULL)
        return;

    Mdl = NdisAllocateMdl(Adapter->MiniportAdapterHandle, Frame, Length);
    if (Mdl == NULL)
    {
        Adapter->RxErrorCount++;
        return;
    }

    Nbl = NdisAllocateNetBufferAndNetBufferList(Adapter->RxNblPool, 0, 0,
                                                Mdl, 0, Length);
    if (Nbl == NULL)
    {
        NdisFreeMdl(Mdl);
        Adapter->RxErrorCount++;
        return;
    }

    NdisZeroMemory(&RecvContext, sizeof(RecvContext));
    RecvContext.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    RecvContext.Header.Revision = DOT11_EXTSTA_RECV_CONTEXT_REVISION_1;
    RecvContext.Header.Size = sizeof(RecvContext);
    RecvContext.uPhyId = 0;
    RecvContext.uChCenterFrequency = Adapter->CurrentChannelMHz;
    RecvContext.usNumberOfMPDUsReceived = 1;
    RecvContext.lRSSI = Rssi;
    RecvContext.ucDataRate = (UCHAR)Rate;

    Nbl->SourceHandle = Adapter->MiniportAdapterHandle;
    NET_BUFFER_LIST_INFO(Nbl, MediaSpecificInformation) = &RecvContext;
    NET_BUFFER_LIST_STATUS(Nbl) = NDIS_STATUS_SUCCESS;

    Adapter->RxFrameCount++;
    /* Split by destination.  The 802.11 header is still in front of the
     * frame here, so Address1 is the DA for every direction a station
     * receives. */
    if (Frame[4] & 0x01)
    {
        static const UCHAR Broadcast[DOT11_ADDR_LEN] =
            { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        if (RtlCompareMemory(Frame + 4, Broadcast, DOT11_ADDR_LEN) ==
            DOT11_ADDR_LEN)
            Adapter->RxBroadcastCount++;
        else
            Adapter->RxMulticastCount++;
    }
    else
        Adapter->RxUnicastCount++;

    NdisMIndicateReceiveNetBufferLists(Adapter->MiniportAdapterHandle, Nbl,
                                       NDIS_DEFAULT_PORT_NUMBER, 1,
                                       NDIS_RECEIVE_FLAGS_RESOURCES);

    NdisFreeNetBufferList(Nbl);
    NdisFreeMdl(Mdl);
}

/*
 * A frame the hardware decrypted still carries the CCMP header the sender
 * inserted and the MIC the sender appended; 802.11 leaves stripping them to
 * the layer above the MAC.  Do it here so nwifi's translator sees an
 * ordinary unprotected data frame.  Returns the new length, or 0 if the
 * frame is too short to be one.
 */
static ULONG StripCipher(PUCHAR Frame, ULONG Length, ULONG HeaderLen,
                         ULONG TrailerLen)
{
    if (Length <= DOT11_MAC_HEADER_LEN + HeaderLen + TrailerLen)
        return 0;

    RtlMoveMemory(Frame + HeaderLen, Frame, DOT11_MAC_HEADER_LEN);
    Frame[HeaderLen + 1] &= (UCHAR)~DOT11_FC1_PROTECTED;

    return Length - HeaderLen - TrailerLen;
}

static VOID DispatchFrame(PAR9485_ADAPTER Adapter, PUCHAR Frame, ULONG Length,
                          ULONG Frequency, LONG Rssi, ULONG Rate,
                          ULONG Status11)
{
    UCHAR Type = Frame[0] & DOT11_FC0_TYPE_MASK;
    UCHAR Subtype = Frame[0] & DOT11_FC0_SUBTYPE_MASK;

    if (Length < DOT11_MAC_HEADER_LEN || Length > AR9485_MAX_RX_FRAME)
        return;

    if (Type == DOT11_FC0_TYPE_MGMT)
    {
        if (Subtype == DOT11_FC0_SUBTYPE_BEACON ||
            Subtype == DOT11_FC0_SUBTYPE_PROBE_RESP)
        {
            RememberBss(Adapter, Frame, Length, Frequency, Rssi);

            /* Feed the beacon watchdog while the beacon is our own BSS's. */
            if (Adapter->MlmeState == AR9485MlmeAssociated &&
                RtlCompareMemory(Frame + 16, Adapter->Bssid, DOT11_ADDR_LEN) ==
                    DOT11_ADDR_LEN)
                Adapter->LastBeaconTime = KeQueryInterruptTime();
        }

        AR9485MlmeReceiveManagement(Adapter, Frame, Length);
        return;
    }

    if (Type != DOT11_FC0_TYPE_DATA)
        return;

    /* Only frames from the BSS we hold, and only while we hold it. */
    if (Adapter->MlmeState != AR9485MlmeAssociated)
        return;
    if (RtlCompareMemory(Frame + 10, Adapter->Bssid, DOT11_ADDR_LEN) !=
        DOT11_ADDR_LEN)
        return;

    /* Only plain data frames carry a payload nwifi can translate.  A
     * null-data frame is a keep-alive with nothing above the MAC header, and
     * a QoS data frame has a wider header that neither this driver nor
     * nwifi's translator parses yet. */
    if (Subtype != DOT11_FC0_SUBTYPE_DATA)
    {
        PULONG Counter = (Subtype & DOT11_FC0_SUBTYPE_QOS_DATA) ? &Adapter->RxDropQos
                                                           : &Adapter->RxDropSubtype;
        if ((++*Counter % 1000) == 1)
            DPRINT1("AR9485: dropped data subtype 0x%02x (%s) #%lu, %lu bytes\n",
                    Subtype, (Subtype & DOT11_FC0_SUBTYPE_QOS_DATA) ? "QoS" : "other",
                    *Counter, Length);
        return;
    }

    if (Frame[1] & DOT11_FC1_PROTECTED)
    {
        /*
         * Which key the MAC used, and how much of the crypto work it did,
         * is decided exactly as ath9k decides it.
         *
         * ar9003_hw_process_rxdesc_edma() turns status11 into AT MOST ONE
         * error, in this priority order, and reports the key index only when
         * AR_RxKeyIdxValid says the key search hit.  ath9k_cmn_rx_accept()
         * then throws two of those errors away again:
         *
         *   - AR_KeyMiss counts only for a UNICAST frame whose descriptor
         *     named a valid CCMP pairwise slot ("!is_mc && ccmp_keymap").
         *     A station's group key is written with an all-zero address
         *     (ath_hw_keysetmac(), mac == NULL), so the transmitter-address
         *     key search always misses on group frames -- while the MAC
         *     still decrypts, picking a default key by the IV's key ID.
         *     Boot 85 proved that trusting AR_KeyMiss drops every broadcast.
         *
         *   - AR_MichaelErr counts only when the descriptor named a slot
         *     that really holds a TKIP key ("mic_error = is_valid_tkip &&
         *     ..."), because the hardware Michael check lives in that cache
         *     entry.  On the group key-miss path it never ran, so whatever
         *     the bit says about it is meaningless.
         */
        BOOLEAN IsMulticast = (Frame[4] & 0x01) != 0;
        BOOLEAN KeyIdxValid = (Status11 & AR_RxKeyIdxValid) != 0;
        ULONG KeyIdx = MS(Status11, AR_KeyIdx);
        BOOLEAN ValidPairwise = KeyIdxValid &&
                                KeyIdx == AR9485_KEY_PAIRWISE &&
                                Adapter->PairwiseKeyValid;
        /* ath9k's tkip_keymap / ccmp_keymap, for a cache holding at most the
         * pairwise slot and one group slot. */
        BOOLEAN ValidTkip = (ValidPairwise &&
                             Adapter->UnicastCipher == DOT11_CIPHER_ALGO_TKIP) ||
                            (KeyIdxValid && KeyIdx < DOT11_MAX_NUM_DEFAULT_KEY &&
                             Adapter->GroupKeyValid &&
                             Adapter->MulticastCipher == DOT11_CIPHER_ALGO_TKIP);
        BOOLEAN ValidCcmp = ValidPairwise &&
                            Adapter->UnicastCipher == DOT11_CIPHER_ALGO_CCMP;
        ULONG Error = (Status11 & AR_DecryptCRCErr) ? AR_DecryptCRCErr :
                      (Status11 & AR_MichaelErr)    ? AR_MichaelErr :
                      (Status11 & AR_KeyMiss)       ? AR_KeyMiss : 0;

        if (Error == AR_KeyMiss && (IsMulticast || !ValidCcmp))
            Error = 0;
        if (Error == AR_MichaelErr && !ValidTkip)
            Error = 0;

        if (Error != 0)
        {
            PULONG Counter = (Error == AR_KeyMiss)    ? &Adapter->RxDropKeyMiss :
                             (Error == AR_MichaelErr) ? &Adapter->RxDropMichael :
                                                        &Adapter->RxDropDecrypt;
            Adapter->RxCryptoErrorCount++;
            if ((++*Counter % 1000) == 1)
                DPRINT1("AR9485: protected data %s #%lu: s11 0x%08lx keyidx %lu%s da %02x:..:%02x\n",
                        (Error == AR_KeyMiss) ? "KEY MISS" :
                        (Error == AR_MichaelErr) ? "MICHAEL ERR" : "DECRYPT ERR",
                        *Counter, Status11, KeyIdx,
                        KeyIdxValid ? "" : " (idx invalid)",
                        Frame[4], Frame[9]);
            return;
        }

        /* The IV's key ID says which key decrypted the frame (ath9k
         * common.c ath9k_cmn_rx_skb_postprocess): 0 = the pairwise key,
         * 1-3 = a default/group key.  On a WPA/WPA2-mixed AP that is the
         * authoritative cipher discriminator; the DA multicast bit is not
         * (a group-keyed frame can be unicast-addressed). */
        ULONG KeyId = (Length > DOT11_MAC_HEADER_LEN + 3)
                      ? (ULONG)(Frame[DOT11_MAC_HEADER_LEN + 3] >> 6) : 0;
        ULONG Cipher = (KeyId == 0) ? Adapter->UnicastCipher
                                    : Adapter->MulticastCipher;
        ULONG HeaderLen = DOT11_CCMP_HEADER_LEN;
        ULONG TrailerLen = DOT11_CCMP_MIC_LEN;

        if (Cipher == DOT11_CIPHER_ALGO_TKIP)
        {
            /*
             * The MAC never removes anything: ath9k trims the 8-byte Michael
             * MIC in SOFTWARE (ath_rx_tasklet, "if (rxs->flag &
             * RX_FLAG_MMIC_STRIPPED) skb_trim(skb, skb->len - 8)") after the
             * hardware verified it, and mac80211 trims it after verifying it
             * itself when the hardware could not.  Either way the trailer to
             * cut is MIC(8) + ICV(4) -- cutting only the ICV, as this used
             * to, hands nwifi eight bytes of MIC as if they were payload.
             */
            HeaderLen = DOT11_TKIP_HEADER_LEN;
            TrailerLen = AR9485_TKIP_TRAILER_LEN;

            if (!ValidTkip)
            {
                /* The key search missed, so the MAC decrypted by IV key ID
                 * but could not run Michael.  Do what
                 * ieee80211_rx_h_michael_mic_verify() does. */
                UCHAR Computed[8];
                int MicResult;

                Adapter->RxGroupTkipCount++;

                if (!Adapter->GroupRxMicValid)
                {
                    /* No group Michael key means no way to check, and an
                     * unchecked TKIP frame is exactly what Michael exists to
                     * refuse.  This used to return silently. */
                    Adapter->RxCryptoErrorCount++;
                    if ((++Adapter->RxGroupNoKeyCount % 1000) == 1)
                        DPRINT1("AR9485: group TKIP but no RX MIC key #%lu "
                                "(keyid %lu, mcast cipher 0x%08lx)\n",
                                Adapter->RxGroupNoKeyCount, KeyId,
                                Adapter->MulticastCipher);
                    return;
                }

                MicResult = AR9485TkipRxCheckMic(Frame, Length,
                                                 DOT11_MAC_HEADER_LEN,
                                                 Adapter->GroupRxMicKey,
                                                 Computed);

                /*
                 * Ring diagnostic, first three group frames of the link only
                 * (the AP sends about 1.5/s; the ring is 16 KB).  The SNAP
                 * bytes are the discriminator the next boot needs: if the
                 * MAC really decrypted this frame the payload opens with the
                 * RFC-1042 header aa aa 03 00 00 00, and if it handed us
                 * ciphertext instead they are noise -- which would mean the
                 * Michael check is running over ciphertext and can never
                 * pass, rather than the key being wrong.
                 */
                if (Adapter->RxGroupTkipCount <= 3 &&
                    Length > DOT11_MAC_HEADER_LEN + HeaderLen + 6)
                {
                    const UCHAR *Snap = Frame + DOT11_MAC_HEADER_LEN + HeaderLen;
                    DPRINT1("AR9485: group TKIP #%lu len %lu s11 0x%08lx snap "
                            "%02x%02x%02x%02x%02x%02x mic %s\n",
                            Adapter->RxGroupTkipCount, Length, Status11,
                            Snap[0], Snap[1], Snap[2], Snap[3], Snap[4], Snap[5],
                            (MicResult == AR9485_TKIPRX_OK) ? "OK" :
                            (MicResult == AR9485_TKIPRX_SHORT) ? "SHORT" : "FAIL");
                }

                if (MicResult == AR9485_TKIPRX_SHORT)
                {
                    /* No trailer to compare against; there is nothing to
                     * print but the length. */
                    Adapter->RxCryptoErrorCount++;
                    if ((++Adapter->RxDropShort % 1000) == 1)
                        DPRINT1("AR9485: group TKIP frame too short for a "
                                "trailer #%lu, %lu bytes\n",
                                Adapter->RxDropShort, Length);
                    return;
                }
                if (MicResult != AR9485_TKIPRX_OK)
                {
                    /* Length > header + IV + trailer is guaranteed here:
                     * anything shorter came back SHORT above. */
                    const UCHAR *Got = Frame + Length - TrailerLen;
                    Adapter->RxCryptoErrorCount++;
                    if ((++Adapter->RxDropMichael % 100) == 1)
                        DPRINT1("AR9485: group TKIP Michael MISMATCH #%lu: want "
                                "%02x%02x%02x%02x%02x%02x%02x%02x got "
                                "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                Adapter->RxDropMichael,
                                Computed[0], Computed[1], Computed[2], Computed[3],
                                Computed[4], Computed[5], Computed[6], Computed[7],
                                Got[0], Got[1], Got[2], Got[3],
                                Got[4], Got[5], Got[6], Got[7]);
                    return;
                }
                Adapter->RxGroupMicOkCount++;
            }
        }

        Length = StripCipher(Frame, Length, HeaderLen, TrailerLen);
        if (Length == 0)
        {
            Adapter->RxCryptoErrorCount++;
            if ((++Adapter->RxDropShort % 1000) == 1)
                DPRINT1("AR9485: protected data too short for %lu+%lu trailer #%lu\n",
                        HeaderLen, TrailerLen, Adapter->RxDropShort);
            return;
        }
        Frame += HeaderLen;
    }

    IndicateDataFrame(Adapter, Frame, Length, Rssi, Rate);
}


/* ================================================================== *
 *  Ring drain
 * ================================================================== */

ULONG AR9485PollReceive(PAR9485_ADAPTER Adapter)
{
    ULONG Consumed = 0;
    ULONG Frequency = Adapter->CurrentChannelMHz;

    if (!Adapter->RxArmed || Adapter->RxBufferCount == 0)
        return 0;

    while (Consumed < Adapter->RxBufferCount)
    {
        ULONG Index = Adapter->RxIndex;
        PAR9485_RX_STATUS Rx = Adapter->RxBuffers[Index].VirtualAddress;
        ULONG Length;

        KeMemoryBarrier();
        if (!(Rx->S11 & AR_RxDone))
            break;

        ++Consumed;
        Adapter->RxIndex = (Index + 1) % Adapter->RxBufferCount;

        if (((Rx->Info & RX_DESC_ID_MASK) >> RX_DESC_ID_SHIFT) != RX_DESC_ID ||
            (Rx->Info & (RX_DESC_TX | RX_DESC_CONTROL_STATUS)))
        {
            Adapter->RxErrorCount++;
            PostRxBuffer(Adapter, Index);
            continue;
        }

        /*
         * A frame too long for one buffer is split across several, with
         * AR_RxMore set on every part but the last.  Nothing we ask for can
         * legally arrive that way -- a non-aggregated MPDU tops out at 2346
         * bytes and AR_DATABUF_SIZE is larger -- so a split frame means the
         * air carried something we do not handle, and half of it is worse
         * than none.  The tail buffer is left to DispatchFrame(), whose
         * BSSID check rejects it.
         */
        if (Rx->S2 & AR_RxMore)
        {
            Adapter->RxErrorCount++;
            PostRxBuffer(Adapter, Index);
            continue;
        }

        /*
         * AR_DataLen counts the FCS.  ath9k declares RX_INCLUDES_FCS and
         * mac80211 trims FCS_LEN off every frame before anything looks at
         * it (ieee80211_rx_monitor), so nothing past this point wants the
         * four CRC bytes: left in, the group-TKIP Michael check MICs them as
         * MSDU and reads the sender's MIC four bytes late (every group frame
         * "mismatched" on the FRITZ!Box 7330, boot 90), and the cipher strip
         * hands nwifi four bytes of CRC as payload on every unicast frame.
         */
        Length = Rx->S2 & AR_DataLen;
        if ((Rx->S11 & AR_RxFrameOK) &&
            Length >= DOT11_MAC_HEADER_LEN + DOT11_FCS_LEN &&
            Length <= AR9485_RX_BUFFER_SIZE - AR9485_RX_STATUS_SIZE)
        {
            DispatchFrame(Adapter,
                          (PUCHAR)Rx + AR9485_RX_STATUS_SIZE,
                          Length - DOT11_FCS_LEN,
                          Frequency,
                          RssiToDbm(MS(Rx->S5, AR_RxRSSICombined)),
                          MS(Rx->S1, AR_RxRate),
                          Rx->S11);
        }
        else if (!(Rx->S11 & AR_RxFrameOK))
        {
            Adapter->RxErrorCount++;
            /*
             * Cheap, no printing: was this junk one of OUR AP's group
             * frames, or the 2.4 GHz noise floor?  "InErr climbs ~25/s"
             * carried no answer to that, and the air capture says the AP
             * sends group frames at only ~1.5/s -- so this counter is what
             * decides whether the group traffic dies here, before any of the
             * crypto path, or reaches DispatchFrame at all.
             */
            if (Length >= DOT11_MAC_HEADER_LEN &&
                Adapter->MlmeState == AR9485MlmeAssociated)
            {
                const UCHAR *Frame = (const UCHAR *)Rx + AR9485_RX_STATUS_SIZE;
                if ((Frame[4] & 0x01) &&
                    (Frame[0] & DOT11_FC0_TYPE_MASK) == DOT11_FC0_TYPE_DATA &&
                    RtlCompareMemory(Frame + 10, Adapter->Bssid,
                                     DOT11_ADDR_LEN) == DOT11_ADDR_LEN)
                    Adapter->RxHwErrGroupCount++;
            }
        }

        PostRxBuffer(Adapter, Index);
    }

    return Consumed;
}

/* ================================================================== *
 *  Scan
 * ================================================================== */

VOID AR9485RunScan(PAR9485_ADAPTER Adapter)
{
    ULONG Channel;
    NDIS_STATUS ScanStatus = NDIS_STATUS_SUCCESS;
    NDIS_STATUS_INDICATION Indication;
    /* AR9485StartScan() refuses while a link is up, so a scan never runs
     * over an association; the PHY reset on each of thirteen channels would
     * destroy one. */
    NT_ASSERT(Adapter->MlmeState == AR9485MlmeIdle);

    Adapter->ScanInProgress = TRUE;
    Adapter->BssCount = 0;

    for (Channel = 1; Channel <= 13 &&
         !(Adapter->Flags & AR9485_FLAG_HALTING); ++Channel) {
        ULONG Frequency = ChannelFrequency(Channel);
        if (!ar9485_hw_start(Adapter->HwContext, Adapter->IoBase,
                             Adapter->IoLength, Adapter->DeviceId,
                             Adapter->MacVersion, (USHORT)Adapter->MacRevision,
                             (USHORT)Frequency)) {
            ScanStatus = NDIS_STATUS_FAILURE;
            break;
        }
        Adapter->CurrentChannelMHz = (USHORT)Frequency;
        AR9485ProgramMacState(Adapter);
        AR9485ArmReceiver(Adapter);
        NdisMSleep(150000);
        AR9485PollReceive(Adapter);
    }

    Adapter->ScanInProgress = FALSE;

    if (!(Adapter->Flags & AR9485_FLAG_HALTING)) {
        if (!ar9485_hw_start(Adapter->HwContext, Adapter->IoBase,
                             Adapter->IoLength, Adapter->DeviceId,
                             Adapter->MacVersion, (USHORT)Adapter->MacRevision,
                             AR9485_DEFAULT_CHANNEL_MHZ))
            ScanStatus = NDIS_STATUS_FAILURE;
        Adapter->CurrentChannelMHz = AR9485_DEFAULT_CHANNEL_MHZ;
        AR9485ProgramMacState(Adapter);
        AR9485ArmReceiver(Adapter);

        NdisZeroMemory(&Indication, sizeof(Indication));
        Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
        Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
        Indication.Header.Size = sizeof(Indication);
        Indication.SourceHandle = Adapter->MiniportAdapterHandle;
        Indication.StatusCode = NDIS_STATUS_DOT11_SCAN_CONFIRM;
        Indication.StatusBuffer = &ScanStatus;
        Indication.StatusBufferSize = sizeof(ScanStatus);
        DPRINT1("AR9485: real scan complete: %lu BSSes status 0x%08x\n",
                Adapter->BssCount, ScanStatus);
        NdisMIndicateStatusEx(Adapter->MiniportAdapterHandle, &Indication);
    }
}

/* ================================================================== *
 *  Setup and teardown
 * ================================================================== */

NDIS_STATUS AR9485InitializeReceiver(PAR9485_ADAPTER Adapter)
{
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParameters;
    ULONG i;

    NdisZeroMemory(&PoolParameters, sizeof(PoolParameters));
    PoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParameters.Header.Size = sizeof(PoolParameters);
    PoolParameters.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    PoolParameters.fAllocateNetBuffer = TRUE;
    PoolParameters.ContextSize = 0;
    PoolParameters.PoolTag = AR9485_TAG;
    PoolParameters.DataSize = AR9485_MAX_RX_FRAME;

    Adapter->RxNblPool =
        NdisAllocateNetBufferListPool(Adapter->MiniportAdapterHandle,
                                      &PoolParameters);
    if (Adapter->RxNblPool == NULL)
        return NDIS_STATUS_RESOURCES;

    for (i = 0; i < AR9485_RX_BUFFER_COUNT; ++i) {
        NdisMAllocateSharedMemory(Adapter->MiniportAdapterHandle,
                                  AR9485_RX_BUFFER_SIZE, FALSE,
                                  &Adapter->RxBuffers[i].VirtualAddress,
                                  &Adapter->RxBuffers[i].PhysicalAddress);
        if (!Adapter->RxBuffers[i].VirtualAddress ||
            Adapter->RxBuffers[i].PhysicalAddress.HighPart) {
            DPRINT1("AR9485: RX DMA allocation %lu unavailable below 4GiB\n", i);
            AR9485ShutdownReceiver(Adapter);
            return NDIS_STATUS_RESOURCES;
        }
        ++Adapter->RxBufferCount;
    }
    return NDIS_STATUS_SUCCESS;
}

VOID AR9485ShutdownReceiver(PAR9485_ADAPTER Adapter)
{
    ULONG i;
    if (!Adapter)
        return;

    /* Block the receive engine before the buffers it is filling go back to
     * the allocator.  AR_DIAG_RX_ABORT kills the frame currently crossing
     * from the MAC, AR_DIAG_RX_DIS stops any new one from starting. */
    if (Adapter->IoBase != NULL && Adapter->RxArmed) {
        AR9485_WRITE_REG(Adapter, AR_DIAG_SW,
                         AR9485_READ_REG(Adapter, AR_DIAG_SW) |
                         AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);
        AR9485_WRITE_REG(Adapter, AR_CR, AR_CR_RXD);
        NdisMSleep(2000);
    }

    Adapter->RxArmed = FALSE;
    for (i = 0; i < Adapter->RxBufferCount; ++i) {
        if (Adapter->RxBuffers[i].VirtualAddress)
            NdisMFreeSharedMemory(Adapter->MiniportAdapterHandle,
                                  AR9485_RX_BUFFER_SIZE, FALSE,
                                  Adapter->RxBuffers[i].VirtualAddress,
                                  Adapter->RxBuffers[i].PhysicalAddress);
        Adapter->RxBuffers[i].VirtualAddress = NULL;
    }
    Adapter->RxBufferCount = 0;

    if (Adapter->RxNblPool != NULL) {
        NdisFreeNetBufferListPool(Adapter->RxNblPool);
        Adapter->RxNblPool = NULL;
    }
}

NDIS_STATUS AR9485StartScan(PAR9485_ADAPTER Adapter)
{
    /* A lab tool holding the chip is mid-experiment.  Refusing here is what
     * keeps wlansvc's scan requests from resetting the PHY underneath it. */
    if (AR9485LabOwnsChip())
        return NDIS_STATUS_MEDIA_BUSY;
    if (Adapter->ChipThread == NULL || Adapter->ScanInProgress)
        return NDIS_STATUS_MEDIA_BUSY;
    /*
     * A scan resets the PHY on each of thirteen channels, which no
     * association survives.  Refusing is better than silently dropping a
     * link the caller did not ask us to drop; nwifi retries once the user
     * disconnects.
     */
    if (Adapter->MlmeState != AR9485MlmeIdle)
        return NDIS_STATUS_MEDIA_BUSY;

    AR9485QueueCommand(Adapter, AR9485_CMD_SCAN);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS AR9485BuildBssList(PAR9485_ADAPTER Adapter,
                               PNDIS_OID_REQUEST Request)
{
    ULONG i, Payload = 0, Total, BufferLength;
    PUCHAR Cursor;
    PDOT11_BYTE_ARRAY Array;
    BufferLength = Request->DATA.QUERY_INFORMATION.InformationBufferLength;
    for (i = 0; i < Adapter->BssCount; ++i)
        Payload += FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer) + Adapter->Bss[i].IeLength;
    Total = FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer) + Payload;
    Request->DATA.QUERY_INFORMATION.BytesNeeded = Total;
    if (BufferLength < Total) {
        Request->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    Array = Request->DATA.QUERY_INFORMATION.InformationBuffer;
    RtlZeroMemory(Array, Total);
    Array->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Array->Header.Revision = DOT11_BYTE_ARRAY_REVISION_1;
    Array->Header.Size = sizeof(DOT11_BYTE_ARRAY);
    Array->uNumOfBytes = Array->uTotalNumOfBytes = Payload;
    Cursor = Array->ucBuffer;
    for (i = 0; i < Adapter->BssCount; ++i) {
        PAR9485_BSS Source = &Adapter->Bss[i];
        PDOT11_BSS_ENTRY Entry = (PDOT11_BSS_ENTRY)Cursor;
        Entry->PhySpecificInfo.uChCenterFrequency = Source->ChannelMHz;
        RtlCopyMemory(Entry->dot11BSSID, Source->Bssid, 6);
        Entry->dot11BSSType = dot11_BSS_type_infrastructure;
        Entry->lRSSI = Source->Rssi;
        Entry->uLinkQuality = Source->Rssi >= -50 ? 100 :
                              Source->Rssi <= -100 ? 0 :
                              (ULONG)(2 * (Source->Rssi + 100));
        Entry->bInRegDomain = TRUE;
        Entry->usBeaconPeriod = Source->BeaconPeriod;
        Entry->ullHostTimestamp = KeQueryInterruptTime();
        Entry->usCapabilityInformation = Source->CapabilityInformation;
        Entry->uBufferLength = Source->IeLength;
        RtlCopyMemory(Entry->ucBuffer, Source->Ies, Source->IeLength);
        Cursor += FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer) + Source->IeLength;
    }
    Request->DATA.QUERY_INFORMATION.BytesWritten = Total;
    return NDIS_STATUS_SUCCESS;
}

VOID NTAPI AR9485ReturnNetBufferLists(NDIS_HANDLE Context,
                                      PNET_BUFFER_LIST NetBufferLists,
                                      ULONG ReturnFlags)
{
    /*
     * Receives are indicated with NDIS_RECEIVE_FLAGS_RESOURCES, which lends
     * the NBL to the protocol only for the duration of its callback.  ROS'
     * NDIS hands ownership back through this handler synchronously, from
     * inside NdisMIndicateReceiveNetBufferLists, and the indicating code
     * frees the NBL and its MDL immediately afterwards - so there is
     * genuinely nothing to do here.
     */
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(NetBufferLists);
    UNREFERENCED_PARAMETER(ReturnFlags);
}

#ifdef AR9485_LAB
/* Exposed to lab.c so a user-mode sequence can be compared against the
 * miniport's own RX arming and harvest without rebuilding the driver. */
VOID AR9485LabQueueRx(PAR9485_ADAPTER Adapter)
{
    AR9485ArmReceiver(Adapter);
}

ULONG AR9485LabHarvestRx(PAR9485_ADAPTER Adapter, ULONG Frequency)
{
    UNREFERENCED_PARAMETER(Frequency);
    return AR9485PollReceive(Adapter);
}
#endif /* AR9485_LAB */
