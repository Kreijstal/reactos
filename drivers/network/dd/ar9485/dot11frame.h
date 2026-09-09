/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     IEEE 802.11 frame layout shared by the transmit path and the
 *              MLME.  Only what a managed-mode station actually builds or
 *              parses is here: the three-address MAC header, the management
 *              subtypes used by join/auth/assoc, and the information
 *              elements that go into an association request.
 */

#ifndef _AR9485_DOT11FRAME_H_
#define _AR9485_DOT11FRAME_H_

#define DOT11_ADDR_LEN                  6
#define DOT11_MAC_HEADER_LEN            24
#define DOT11_FCS_LEN                   4
#define DOT11_CCMP_HEADER_LEN           8
#define DOT11_CCMP_MIC_LEN              8
#define DOT11_CCMP_EXT_IV       0x20    /* byte 3 of the CCMP header */
#define DOT11_TKIP_HEADER_LEN           8   /* IV + ExtIV */
/* The TKIP trailer is Michael MIC(8) + ICV(4) and the AR9300 MAC removes
 * NEITHER, whether or not its key search hit: ath9k trims the MIC in software
 * once the hardware has verified it (ath_rx_tasklet, RX_FLAG_MMIC_STRIPPED)
 * and mac80211 trims it after verifying it itself when the hardware could
 * not.  See AR9485_TKIP_TRAILER_LEN in tkiprx.h, which is what receive.c
 * uses; cutting only the ICV hands the layer above eight bytes of MIC. */

/* FrameControl[0]: protocol version, type, subtype. */
#define DOT11_FC0_VERSION_MASK          0x03
#define DOT11_FC0_TYPE_MASK             0x0c
#define DOT11_FC0_TYPE_MGMT             0x00
#define DOT11_FC0_TYPE_CTL              0x04
#define DOT11_FC0_TYPE_DATA             0x08
#define DOT11_FC0_SUBTYPE_MASK          0xf0

#define DOT11_FC0_SUBTYPE_ASSOC_REQ     0x00
#define DOT11_FC0_SUBTYPE_ASSOC_RESP    0x10
#define DOT11_FC0_SUBTYPE_REASSOC_REQ   0x20
#define DOT11_FC0_SUBTYPE_REASSOC_RESP  0x30
#define DOT11_FC0_SUBTYPE_PROBE_REQ     0x40
#define DOT11_FC0_SUBTYPE_PROBE_RESP    0x50
#define DOT11_FC0_SUBTYPE_BEACON        0x80
#define DOT11_FC0_SUBTYPE_DISASSOC      0xa0
#define DOT11_FC0_SUBTYPE_AUTH          0xb0
#define DOT11_FC0_SUBTYPE_DEAUTH        0xc0

#define DOT11_FC0_SUBTYPE_DATA          0x00
#define DOT11_FC0_SUBTYPE_NULL          0x40
#define DOT11_FC0_SUBTYPE_QOS_DATA      0x80

/* FrameControl[1]: DS bits and flags. */
#define DOT11_FC1_DIR_MASK              0x03
#define DOT11_FC1_DIR_NODS              0x00
#define DOT11_FC1_DIR_TODS              0x01
#define DOT11_FC1_DIR_FROMDS            0x02
#define DOT11_FC1_DIR_DSTODS            0x03
#define DOT11_FC1_MORE_FRAG             0x04
#define DOT11_FC1_RETRY                 0x08
#define DOT11_FC1_PWR_MGT               0x10
#define DOT11_FC1_MORE_DATA             0x20
#define DOT11_FC1_FROM_DS               0x02
#define DOT11_FC1_PROTECTED             0x40
#define DOT11_FC1_ORDER                 0x80

/* Capability information bits. */
#define DOT11_CAPABILITY_ESS            0x0001
#define DOT11_CAPABILITY_IBSS           0x0002
#define DOT11_CAPABILITY_PRIVACY        0x0010
#define DOT11_CAPABILITY_SHORT_PREAMBLE 0x0020
#define DOT11_CAPABILITY_SHORT_SLOT     0x0400

/* Authentication algorithms carried in an authentication frame. */
#define DOT11_AUTH_ALG_OPEN             0
#define DOT11_AUTH_ALG_SHARED_KEY       1

/* Status and reason codes we actually act on. */
#define DOT11_STATUS_SUCCESS            0
#define DOT11_REASON_UNSPECIFIED        1
#define DOT11_REASON_LEAVING            3

/* Information element identifiers. */
#define DOT11_IE_SSID                   0
#define DOT11_IE_SUPPORTED_RATES        1
#define DOT11_IE_DS_PARAMETER_SET       3
#define DOT11_IE_RSN                    48
#define DOT11_IE_EXTENDED_RATES         50
#define DOT11_IE_VENDOR_SPECIFIC        221

/* EtherType of an EAPOL frame, in the RFC 1042 SNAP header that follows the
 * MAC header of a data frame.  The supplicant's handshake must go out in the
 * clear even after a pairwise key is installed, so the transmit path has to
 * recognise it. */
#define DOT11_ETHERTYPE_EAPOL           0x888e

#include <pshpack1.h>
typedef struct _DOT11_MAC_HEADER_3ADDR
{
    UCHAR  FrameControl[2];
    USHORT Duration;
    UCHAR  Address1[DOT11_ADDR_LEN];
    UCHAR  Address2[DOT11_ADDR_LEN];
    UCHAR  Address3[DOT11_ADDR_LEN];
    USHORT SequenceControl;
} DOT11_MAC_HEADER_3ADDR, *PDOT11_MAC_HEADER_3ADDR;

typedef struct _DOT11_SNAP_HEADER
{
    UCHAR  Dsap;
    UCHAR  Ssap;
    UCHAR  Control;
    UCHAR  Oui[3];
    USHORT EtherType;   /* big-endian on the wire */
} DOT11_SNAP_HEADER, *PDOT11_SNAP_HEADER;
#include <poppack.h>

C_ASSERT(sizeof(DOT11_MAC_HEADER_3ADDR) == DOT11_MAC_HEADER_LEN);
C_ASSERT(sizeof(DOT11_SNAP_HEADER) == 8);

#endif /* _AR9485_DOT11FRAME_H_ */
