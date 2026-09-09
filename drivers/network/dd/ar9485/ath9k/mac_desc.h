/*
 * Copyright (c) 2008-2011 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * AR9300-family transmit/receive descriptor layout and the control- and
 * status-word bit definitions the miniport's data path needs.  Extracted
 * verbatim from Linux ath9k (ar9003_mac.h and mac.h, 6.12) so the field
 * encodings are the upstream ones rather than re-derived guesses; only the
 * u32 spelling changes, because the ReactOS side compiles these structures
 * directly instead of through the linux-compat shim.
 *
 * The AR9485 is an EDMA part: transmit descriptors are pushed one at a time
 * into a per-QCU hardware FIFO with AR_QTXDP, and completions land in a
 * separate DMA status ring (AR_Q_STATUS_RING_START/END) rather than being
 * written back into the descriptor.
 */

#ifndef _AR9485_MAC_DESC_H_
#define _AR9485_MAC_DESC_H_

/* Field extract/insert, as ath9k spells them (hw_min.h defines the same pair
 * for the translation units that pull the full ath9k stack). */
#ifndef MS
#define MS(_v, _f) (((_v) & (_f)) >> _f##_S)
#endif
#ifndef SM
#define SM(_v, _f) (((_v) << _f##_S) & (_f))
#endif

#define ATHEROS_VENDOR_ID       0x168c

/* ds_info / descriptor identification (ar9003_mac.h) */
#define AR_DescId               0xffff0000
#define AR_DescId_S             16
#define AR_CtrlStat             0x00004000
#define AR_CtrlStat_S           14
#define AR_TxRxDesc             0x00008000
#define AR_TxRxDesc_S           15
#define AR_TxQcuNum             0x00000f00
#define AR_TxQcuNum_S           8

#define AR_BufLen               0x0fff0000
#define AR_BufLen_S             16

#define AR_TxDescId             0xffff0000
#define AR_TxDescId_S           16
#define AR_TxPtrChkSum          0x0000ffff

#define AR_Not_Sounding         0x20000000

/* ctl11 (mac.h) */
#define AR_FrameLen             0x00000fff
#define AR_VirtMoreFrag         0x00001000
#define AR_XmitPower0           0x003f0000
#define AR_XmitPower0_S         16
#define AR_RTSEnable            0x00400000
#define AR_VEOL                 0x00800000
#define AR_ClrDestMask          0x01000000
#define AR_TxIntrReq            0x20000000
#define AR_DestIdxValid         0x40000000
#define AR_CTSEnable            0x80000000

/* ctl12 */
#define AR_TxMore               0x00001000
#define AR_DestIdx              0x000fe000
#define AR_DestIdx_S            13
#define AR_FrameType            0x00f00000
#define AR_FrameType_S          20
#define AR_NoAck                0x01000000
#define AR_InsertTS             0x02000000
#define AR_CorruptFCS           0x04000000
#define AR_ExtOnly              0x08000000
#define AR_ExtAndCtl            0x10000000
#define AR_MoreAggr             0x20000000
#define AR_IsAggr               0x40000000

/* ctl13 */
#define AR_BurstDur             0x00007fff
#define AR_BurstDur_S           0
#define AR_DurUpdateEna         0x00008000
#define AR_XmitDataTries0       0x000f0000
#define AR_XmitDataTries0_S     16
#define AR_XmitDataTries1       0x00f00000
#define AR_XmitDataTries1_S     20
#define AR_XmitDataTries2       0x0f000000
#define AR_XmitDataTries2_S     24
#define AR_XmitDataTries3       0xf0000000
#define AR_XmitDataTries3_S     28

/* ctl14 */
#define AR_XmitRate0            0x000000ff
#define AR_XmitRate0_S          0
#define AR_XmitRate1            0x0000ff00
#define AR_XmitRate1_S          8
#define AR_XmitRate2            0x00ff0000
#define AR_XmitRate2_S          16
#define AR_XmitRate3            0xff000000
#define AR_XmitRate3_S          24

/* ctl18: per-series rate flags and chain select (mac.h upstream).  A series
 * whose ChainSel is 0 selects NO transmit chain; CCK still leaves on this
 * 1x1 part, but OFDM/HT frames never complete (lab, 2026-08-29). */
#define AR_2040_0               0x00000001
#define AR_GI0                  0x00000002
#define AR_ChainSel0            0x0000001c
#define AR_ChainSel0_S          2
#define AR_2040_1               0x00000020
#define AR_GI1                  0x00000040
#define AR_ChainSel1            0x00000380
#define AR_ChainSel1_S          7
#define AR_2040_2               0x00000400
#define AR_GI2                  0x00000800
#define AR_ChainSel2            0x00007000
#define AR_ChainSel2_S          12
#define AR_2040_3               0x00008000
#define AR_GI3                  0x00010000
#define AR_ChainSel3            0x000e0000
#define AR_ChainSel3_S          17
#define AR_RTSCTSRate           0x0ff00000
#define AR_RTSCTSRate_S         20
#define AR_STBC0                0x10000000
#define AR_STBC1                0x20000000
#define AR_STBC2                0x40000000
#define AR_STBC3                0x80000000

/* ctl17 */
#define AR_AggrLen              0x0000ffff
#define AR_AggrLen_S            0
#define AR_PadDelim             0x03fc0000
#define AR_PadDelim_S           18
#define AR_EncrType             0x0c000000
#define AR_EncrType_S           26
#define AR_LDPC                 0x80000000

/*
 * AR_EncrType values.  These are the ath9k_key_type enumerators; the
 * hardware reads the actual key material out of the key cache entry named
 * by AR_DestIdx, so this field only selects the algorithm.
 */
#define AR_ENCR_TYPE_CLEAR      0
#define AR_ENCR_TYPE_WEP        1
#define AR_ENCR_TYPE_AES        2       /* CCMP */
#define AR_ENCR_TYPE_TKIP       3

/* TX status: status3 */
#define AR_FrmXmitOK            0x00000001
#define AR_ExcessiveRetries     0x00000002
#define AR_FIFOUnderrun         0x00000004
#define AR_Filtered             0x00000008
#define AR_RTSFailCnt           0x000000f0
#define AR_RTSFailCnt_S         4
#define AR_DataFailCnt          0x00000f00
#define AR_DataFailCnt_S        8
#define AR_VirtRetryCnt         0x0000f000
#define AR_VirtRetryCnt_S       12
#define AR_TxDelimUnderrun      0x00010000
#define AR_TxDataUnderrun       0x00020000
#define AR_DescCfgErr           0x00040000
#define AR_TxTimerExpired       0x00080000

/* TX status: status8 */
#define AR_TxDone               0x00000001
#define AR_SeqNum               0x00001ffe
#define AR_SeqNum_S             1
#define AR_TxOpExceeded         0x00020000
#define AR_FinalTxIdx           0x00600000
#define AR_FinalTxIdx_S         21

/* RX status */
#define AR_RxRate               0xff000000
#define AR_RxRate_S             24
#define AR_DataLen              0x00000fff
#define AR_RxMore               0x00001000
#define AR_RxRSSICombined       0xff000000
#define AR_RxRSSICombined_S     24
#define AR_RxDone               0x00000001
#define AR_RxFrameOK            0x00000002
#define AR_CRCErr               0x00000004
#define AR_DecryptCRCErr        0x00000008
#define AR_PHYErr               0x00000010
#define AR_MichaelErr           0x00000020
#define AR_RxKeyIdxValid        0x00000100
#define AR_KeyIdx               0x0000fe00
#define AR_KeyIdx_S             9
#define AR_RxMoreAggr           0x00010000
#define AR_RxAggr               0x00020000
#define AR_KeyMiss              0x80000000

/* Transmit control descriptor: 32 dwords, one cache line. */
#include <pshpack4.h>
typedef struct _AR9003_TXC
{
    ULONG info;     /* descriptor information */
    ULONG link;     /* link pointer */
    ULONG data0;    /* data pointer to 1st buffer */
    ULONG ctl3;     /* DMA control 3  */
    ULONG data1;
    ULONG ctl5;
    ULONG data2;
    ULONG ctl7;
    ULONG data3;
    ULONG ctl9;
    ULONG ctl10;
    ULONG ctl11;
    ULONG ctl12;
    ULONG ctl13;
    ULONG ctl14;
    ULONG ctl15;
    ULONG ctl16;
    ULONG ctl17;
    ULONG ctl18;
    ULONG ctl19;
    ULONG ctl20;
    ULONG ctl21;
    ULONG ctl22;
    ULONG ctl23;
    ULONG pad[8];
} AR9003_TXC, *PAR9003_TXC;

typedef struct _AR9003_TXS
{
    ULONG ds_info;
    ULONG status1;
    ULONG status2;
    ULONG status3;
    ULONG status4;
    ULONG status5;
    ULONG status6;
    ULONG status7;
    volatile ULONG status8;
} AR9003_TXS, *PAR9003_TXS;
#include <poppack.h>

/*
 * Legacy PHY rate codes (ath9k_legacy_rates, common-init.c).  The AR9300
 * descriptor's AR_XmitRateN field takes these directly.
 */
#define AR9485_RATE_1M          0x1b
#define AR9485_RATE_2M          0x1a
#define AR9485_RATE_5M5         0x19
#define AR9485_RATE_11M         0x18
#define AR9485_RATE_6M          0x0b
#define AR9485_RATE_12M         0x0a
#define AR9485_RATE_24M         0x09

#endif /* _AR9485_MAC_DESC_H_ */
