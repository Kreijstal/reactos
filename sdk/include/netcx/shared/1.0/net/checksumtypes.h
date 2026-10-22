/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Packet checksum offload extension types
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_PACKET_RX_CHECKSUM_EVALUATION
{
    NetPacketRxChecksumEvaluationNotChecked = 0,
    NetPacketRxChecksumEvaluationValid = 1,
    NetPacketRxChecksumEvaluationInvalid = 2,
} NET_PACKET_RX_CHECKSUM_EVALUATION;

typedef enum _NET_PACKET_TX_CHECKSUM_ACTION
{
    NetPacketTxChecksumActionPassthrough = 0,
    NetPacketTxChecksumActionRequired = 2,
} NET_PACKET_TX_CHECKSUM_ACTION;

/* Each layer holds a NET_PACKET_RX_CHECKSUM_EVALUATION or a NET_PACKET_TX_CHECKSUM_ACTION */
typedef struct _NET_PACKET_CHECKSUM
{
    UINT8 Layer2 : 2;
    UINT8 Layer3 : 2;
    UINT8 Layer4 : 2;
    UINT8 Reserved : 2;
} NET_PACKET_CHECKSUM;

#ifdef __cplusplus
}
#endif
