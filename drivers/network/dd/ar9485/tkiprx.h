/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     TKIP receive post-processing the AR9300 MAC leaves to
 *              software: the Michael MIC over a group-keyed frame.
 *
 * The hardware key search for a station's group key is a transmitter-address
 * match against the key cache, and a STA group entry is written with an
 * all-zero address (ath_hw_keysetmac(), mac == NULL), so the search ALWAYS
 * misses.  The MAC still decrypts, using the key ID in the IV to pick a
 * default key -- that is exactly what ath9k relies on: with the descriptor's
 * key index invalid, ath9k_cmn_rx_skb_postprocess() re-derives the key index
 * from the IV and still sets RX_FLAG_DECRYPTED, telling mac80211 the payload
 * is plaintext.  What the MAC does NOT do on that path is verify the Michael
 * MIC, because that lives in the key cache entry it could not find.  So
 * mac80211 does it (ieee80211_rx_h_michael_mic_verify) and so do we.
 *
 * This header has no NDIS or kernel dependency on purpose: test_tkiprx.c
 * builds it on the host and runs the spec's Michael vectors plus a synthetic
 * AP->STA group frame through the very code the miniport uses.
 */

#ifndef _AR9485_TKIPRX_H_
#define _AR9485_TKIPRX_H_

/* Widths, spelled without <windows.h> so the host test can use them too.
 * unsigned int is 32 bits on every target this driver builds for. */
typedef unsigned char AR9485_TKIP_U8;
typedef unsigned int  AR9485_TKIP_U32;

/* [IV | Ext IV] in front; [Michael MIC | ICV] behind.  The MIC belongs to
 * the MSDU and the ICV to the MPDU, so on the air the order is
 * ...payload, MIC(8), ICV(4). */
#define AR9485_TKIP_IV_LEN      8
#define AR9485_TKIP_MIC_LEN     8
#define AR9485_TKIP_ICV_LEN     4
#define AR9485_TKIP_TRAILER_LEN (AR9485_TKIP_MIC_LEN + AR9485_TKIP_ICV_LEN)

/* Results of AR9485TkipRxCheckMic(). */
#define AR9485_TKIPRX_OK        0
#define AR9485_TKIPRX_SHORT     1   /* not enough bytes to hold IV+MIC+ICV */
#define AR9485_TKIPRX_MIC_FAIL  2

/* 802.11 header bits this file needs to find DA/SA and the QoS TID. */
#define AR9485_TKIP_FC1_TODS    0x01
#define AR9485_TKIP_FC1_FROMDS  0x02

/* ================================================================== *
 *  Michael, verbatim in behaviour with net/mac80211/michael.c
 * ================================================================== */

static __inline AR9485_TKIP_U32 AR9485MicRol32(AR9485_TKIP_U32 v, int n)
{
    return (AR9485_TKIP_U32)((v << n) | (v >> (32 - n)));
}

static __inline AR9485_TKIP_U32 AR9485MicRor32(AR9485_TKIP_U32 v, int n)
{
    return (AR9485_TKIP_U32)((v >> n) | (v << (32 - n)));
}

static __inline AR9485_TKIP_U32 AR9485MicXswap(AR9485_TKIP_U32 v)
{
    return ((v & 0x00ff00ffu) << 8) | ((v & 0xff00ff00u) >> 8);
}

static __inline AR9485_TKIP_U32 AR9485MicLe32(const AR9485_TKIP_U8 *p)
{
    return (AR9485_TKIP_U32)p[0] | ((AR9485_TKIP_U32)p[1] << 8) |
           ((AR9485_TKIP_U32)p[2] << 16) | ((AR9485_TKIP_U32)p[3] << 24);
}

static __inline void AR9485MichaelBlock(AR9485_TKIP_U32 *l, AR9485_TKIP_U32 *r)
{
    *r ^= AR9485MicRol32(*l, 17); *l += *r;
    *r ^= AR9485MicXswap(*l);     *l += *r;
    *r ^= AR9485MicRol32(*l, 3);  *l += *r;
    *r ^= AR9485MicRor32(*l, 2);  *l += *r;
}

/*
 * Streaming form, so the spec's chained Michael vectors (which have no
 * pseudo header) can be run against this very code from the host test.
 */
typedef struct _AR9485_MICHAEL_CTX
{
    AR9485_TKIP_U32 l, r;
    AR9485_TKIP_U8  Pending[4];
    AR9485_TKIP_U32 PendingLen;
} AR9485_MICHAEL_CTX;

static __inline void
AR9485MichaelInit(AR9485_MICHAEL_CTX *Ctx, const AR9485_TKIP_U8 *Key)
{
    Ctx->l = AR9485MicLe32(Key);
    Ctx->r = AR9485MicLe32(Key + 4);
    Ctx->PendingLen = 0;
}

static __inline void
AR9485MichaelUpdate(AR9485_MICHAEL_CTX *Ctx, const AR9485_TKIP_U8 *Data,
                    AR9485_TKIP_U32 Len)
{
    AR9485_TKIP_U32 i = 0;

    while (Ctx->PendingLen != 0 && i < Len)
    {
        Ctx->Pending[Ctx->PendingLen++] = Data[i++];
        if (Ctx->PendingLen == 4)
        {
            Ctx->l ^= AR9485MicLe32(Ctx->Pending);
            AR9485MichaelBlock(&Ctx->l, &Ctx->r);
            Ctx->PendingLen = 0;
        }
    }
    for (; i + 4 <= Len; i += 4)
    {
        Ctx->l ^= AR9485MicLe32(Data + i);
        AR9485MichaelBlock(&Ctx->l, &Ctx->r);
    }
    for (; i < Len; ++i)
        Ctx->Pending[Ctx->PendingLen++] = Data[i];
}

/* Last block, padded with 0x5a and zeroes, then one all-zero block. */
static __inline void
AR9485MichaelFinal(AR9485_MICHAEL_CTX *Ctx, AR9485_TKIP_U8 Mic[8])
{
    AR9485_TKIP_U8 tail[8];
    AR9485_TKIP_U32 i;

    for (i = 0; i < 8; ++i) tail[i] = 0;
    for (i = 0; i < Ctx->PendingLen; ++i) tail[i] = Ctx->Pending[i];
    tail[Ctx->PendingLen] = 0x5a;

    Ctx->l ^= AR9485MicLe32(tail);     AR9485MichaelBlock(&Ctx->l, &Ctx->r);
    Ctx->l ^= AR9485MicLe32(tail + 4); AR9485MichaelBlock(&Ctx->l, &Ctx->r);

    Mic[0] = (AR9485_TKIP_U8)Ctx->l;
    Mic[1] = (AR9485_TKIP_U8)(Ctx->l >> 8);
    Mic[2] = (AR9485_TKIP_U8)(Ctx->l >> 16);
    Mic[3] = (AR9485_TKIP_U8)(Ctx->l >> 24);
    Mic[4] = (AR9485_TKIP_U8)Ctx->r;
    Mic[5] = (AR9485_TKIP_U8)(Ctx->r >> 8);
    Mic[6] = (AR9485_TKIP_U8)(Ctx->r >> 16);
    Mic[7] = (AR9485_TKIP_U8)(Ctx->r >> 24);
}

/*
 * MIC over the pseudo header DA || SA || Priority || 0 0 0, then the data,
 * padded with 0x5a and zeroes to a whole number of blocks.  Same algorithm,
 * same argument meaning, as net/mac80211/michael.c michael_mic().
 */
static __inline void
AR9485MichaelMic(const AR9485_TKIP_U8 *Key,
                 const AR9485_TKIP_U8 *Da,
                 const AR9485_TKIP_U8 *Sa,
                 AR9485_TKIP_U8 Priority,
                 const AR9485_TKIP_U8 *Data,
                 AR9485_TKIP_U32 Len,
                 AR9485_TKIP_U8 Mic[8])
{
    AR9485_MICHAEL_CTX Ctx;
    AR9485_TKIP_U8 hdr[16];
    AR9485_TKIP_U32 i;

    for (i = 0; i < 6; ++i) { hdr[i] = Da[i]; hdr[6 + i] = Sa[i]; }
    hdr[12] = Priority; hdr[13] = 0; hdr[14] = 0; hdr[15] = 0;

    AR9485MichaelInit(&Ctx, Key);
    AR9485MichaelUpdate(&Ctx, hdr, 16);
    AR9485MichaelUpdate(&Ctx, Data, Len);
    AR9485MichaelFinal(&Ctx, Mic);
}

/* ================================================================== *
 *  Frame-level check
 * ================================================================== */

/*
 * Verify the Michael MIC of one decrypted TKIP MPDU.
 *
 *   Frame     the whole MPDU, MAC header first, exactly as the MAC handed it
 *             over: [header][IV+ExtIV 8][payload][MIC 8][ICV 4].  No FCS:
 *             AR_DataLen includes it (ath9k: RX_INCLUDES_FCS), so the ring
 *             drain takes the four bytes off before the frame gets here.
 *   HeaderLen the 802.11 header length (24, or 26 for QoS data).
 *   RxMicKey  the Michael key for this direction.  For a group key that is
 *             GTK[16..23], the key the AUTHENTICATOR transmits with; the
 *             KDE/OID byte order is TK(16) | authenticator TX MIC(8) |
 *             authenticator RX MIC(8), so no half-swap belongs here.
 *   Computed  optional, receives the MIC this function calculated (for the
 *             diagnostic ring line).
 *
 * Returns one of AR9485_TKIPRX_*.
 */
static __inline int
AR9485TkipRxCheckMic(const AR9485_TKIP_U8 *Frame,
                     AR9485_TKIP_U32 Length,
                     AR9485_TKIP_U32 HeaderLen,
                     const AR9485_TKIP_U8 *RxMicKey,
                     AR9485_TKIP_U8 Computed[8])
{
    AR9485_TKIP_U8 Mic[8];
    const AR9485_TKIP_U8 *Da, *Sa, *Received;
    AR9485_TKIP_U8 Priority = 0;
    AR9485_TKIP_U32 DataLen;
    AR9485_TKIP_U32 i;

    if (Length <= HeaderLen + AR9485_TKIP_IV_LEN + AR9485_TKIP_TRAILER_LEN)
        return AR9485_TKIPRX_SHORT;

    /* ieee80211_get_DA/_SA: the address that is not the BSSID depends on the
     * DS bits.  A station in an infrastructure BSS receives FromDS frames,
     * where Address1 is the DA and Address3 the SA. */
    if (Frame[1] & AR9485_TKIP_FC1_TODS)
        Da = Frame + 16;                        /* Address3 */
    else
        Da = Frame + 4;                         /* Address1 */

    if (Frame[1] & AR9485_TKIP_FC1_FROMDS)
        Sa = (Frame[1] & AR9485_TKIP_FC1_TODS) ? Frame + 24 /* Address4 */
                                               : Frame + 16 /* Address3 */;
    else
        Sa = Frame + 10;                        /* Address2 */

    /* michael_mic(): the priority octet is the QoS TID, or zero. */
    if ((Frame[0] & 0x0c) == 0x08 && (Frame[0] & 0x80) && HeaderLen >= 26)
        Priority = (AR9485_TKIP_U8)(Frame[HeaderLen - 2] & 0x0f);

    DataLen = Length - HeaderLen - AR9485_TKIP_IV_LEN - AR9485_TKIP_TRAILER_LEN;
    AR9485MichaelMic(RxMicKey, Da, Sa, Priority,
                     Frame + HeaderLen + AR9485_TKIP_IV_LEN, DataLen, Mic);

    if (Computed != 0)
        for (i = 0; i < 8; ++i) Computed[i] = Mic[i];

    /* The MIC sits immediately after the payload, ahead of the ICV. */
    Received = Frame + Length - AR9485_TKIP_TRAILER_LEN;
    for (i = 0; i < 8; ++i)
        if (Mic[i] != Received[i])
            return AR9485_TKIPRX_MIC_FAIL;

    return AR9485_TKIPRX_OK;
}

#endif /* _AR9485_TKIPRX_H_ */
