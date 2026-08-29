/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     802.11 station MLME: join, authenticate, associate, and the
 *              single background thread every radio operation runs on.
 *
 * nwifi's media-specific module drives this through the OID surface.  It
 * programs the desired SSID/BSSID, the authentication algorithm and the
 * cipher pair, then issues OID_DOT11_CONNECT_REQUEST, which carries no
 * payload: running scan/join/auth/assoc and reporting progress through the
 * NDIS_STATUS_DOT11_* indications is the miniport's job.  For a secure
 * network nwifi's supplicant then drives the 4-way handshake over the data
 * path and installs the resulting keys through the cipher-key OIDs.
 *
 * Everything that touches the radio - scanning, joining, harvesting the
 * receive ring, reaping transmit status - runs on the chip worker below.
 * That is deliberate: a scan resets the PHY across thirteen channels, and
 * anything running concurrently with that measures nothing.  It also means
 * the MLME can simply poll the receive ring while waiting for an
 * authentication or association response instead of needing a rendezvous
 * with a separate receive path.
 */

#include "ar9485.h"
#include "ath9k/reg.h"
#include "ath9k/ath_reg.h"
#include "ath9k/mac_desc.h"

#define NDEBUG
#include <debug.h>

/* ath9k hw.h: the reset path's default receive-signal threshold. */
#define AR9485_INIT_RSSI_THR        0x00000700

/* How long to wait for one management response, and how many times to send
 * it before giving up.  802.11 leaves both to the implementation; these are
 * the values a station typically uses. */
#define AR9485_MGMT_TIMEOUT_MS      500
#define AR9485_MGMT_ATTEMPTS        3
#define AR9485_MGMT_POLL_MS         2

/* Chip-worker cadence.  Idle costs almost nothing; while a link is up the
 * loop is what moves receives upward and retires transmits. */
#define AR9485_POLL_IDLE_MS         20
#define AR9485_POLL_ACTIVE_MS       2

/* Beacons are ~100 ms apart; three seconds without one from our own BSS is a
 * link that is gone whether or not the AP said so. */
#define AR9485_BEACON_TIMEOUT_MS    3000

/* RSN suite selectors (IEEE 802.11, table 9-131/9-133). */
#define RSN_SUITE_OUI_0             0x00
#define RSN_SUITE_OUI_1             0x0f
#define RSN_SUITE_OUI_2             0xac
#define RSN_CIPHER_USE_GROUP        0
#define RSN_CIPHER_WEP40            1
#define RSN_CIPHER_TKIP             2
#define RSN_CIPHER_CCMP             4
#define RSN_CIPHER_WEP104           5
#define RSN_AKM_8021X               1
#define RSN_AKM_PSK                 2

/* ================================================================== *
 *  Small helpers
 * ================================================================== */

static ULONG
AR9485ReadLe32(_In_reads_bytes_(4) const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) |
           ((ULONG)p[3] << 24);
}

static USHORT
AR9485ReadLe16(_In_reads_bytes_(2) const UCHAR *p)
{
    return (USHORT)(p[0] | ((USHORT)p[1] << 8));
}

static VOID
AR9485WriteLe16(_Out_writes_bytes_(2) PUCHAR p, _In_ USHORT v)
{
    p[0] = (UCHAR)(v & 0xff);
    p[1] = (UCHAR)(v >> 8);
}

static BOOLEAN
AR9485AddressEqual(_In_ const UCHAR *a, _In_ const UCHAR *b)
{
    return RtlCompareMemory(a, b, DOT11_ADDR_LEN) == DOT11_ADDR_LEN;
}

/* Locate one information element inside an IE blob. */
static const UCHAR *
AR9485FindIe(
    _In_reads_bytes_(Length) const UCHAR *Ies,
    _In_ ULONG Length,
    _In_ UCHAR Id,
    _Out_ PULONG IeLength)
{
    ULONG Offset = 0;

    *IeLength = 0;
    while (Offset + 2 <= Length)
    {
        UCHAR ThisId = Ies[Offset];
        ULONG ThisLength = Ies[Offset + 1];

        if (Offset + 2 + ThisLength > Length)
            break;
        if (ThisId == Id)
        {
            *IeLength = ThisLength;
            return &Ies[Offset + 2];
        }
        Offset += 2 + ThisLength;
    }
    return NULL;
}

static VOID
AR9485Sleep(_In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * (LONGLONG)Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

/* ================================================================== *
 *  Hardware state that the PHY reset does not restore
 * ================================================================== */

/*
 * ath9k splits this across ath9k_hw_reset_opmode() and
 * ath9k_hw_init_queues(); ar9485_hw_start() performs neither, so the
 * miniport has to.  Every one of these registers reads back zero after a
 * bring-up, and two of them - AR_DQCUMASK and AR_STA_ID0/1 - are what stood
 * between this chip and its first transmitted frame.
 */
VOID
AR9485ProgramMacState(_In_ PAR9485_ADAPTER Adapter)
{
    ULONG i;
    const UCHAR *Mac = Adapter->CurrentMacAddress;

    /* DCU i drives QCU i.  Left at zero, the DCU drives nothing, AR_Q_TXE
     * never clears, and no frame is ever arbitrated onto the air. */
    for (i = 0; i < AR_NUM_DCU; ++i)
        AR9485_WRITE_REG(Adapter, AR_DQCUMASK(i), 1 << i);

    /* Station address.  Without it the MAC has no identity to match against
     * for ACK generation, so nothing we send is ever acknowledged. */
    AR9485_WRITE_REG(Adapter, AR_STA_ID0, AR9485ReadLe32(Mac));
    AR9485_WRITE_REG(Adapter, AR_STA_ID1,
                     (ULONG)AR9485ReadLe16(Mac + 4) |
                     AR_STA_ID1_RTS_USE_DEF |
                     AR_STA_ID1_CRPT_MIC_ENABLE |
                     AR_STA_ID1_MCAST_KSRCH |
                     /* Look the key cache up for every frame.  ath9k sets
                      * this in ath9k_hw_set_operating_mode() for all opmodes;
                      * this write is wholesale rather than read-modify-write,
                      * so leaving it out clears what the PHY reset set and
                      * silently disables hardware crypto. */
                     AR_STA_ID1_KSRCH_MODE |
                     /* Sequence numbers come from software, as they do in
                      * ath9k: nwifi numbers the data frames it hands down and
                      * AR9485TransmitFrame() numbers everything. */
                     AR_STA_ID1_PRESERVE_SEQNUM);

    /* An all-ones BSSID mask means "match the address exactly"; a station
     * with one virtual interface wants no don't-care bits. */
    AR9485_WRITE_REG(Adapter, AR_BSSMSKL, 0xffffffff);
    AR9485_WRITE_REG(Adapter, AR_BSSMSKU, 0x0000ffff);

    AR9485_WRITE_REG(Adapter, AR_BSS_ID0, AR9485ReadLe32(Adapter->Bssid));
    AR9485_WRITE_REG(Adapter, AR_BSS_ID1,
                     (ULONG)AR9485ReadLe16(Adapter->Bssid + 4) |
                     ((Adapter->AssociationId & 0x3fff) << AR_BSS_ID1_AID_S));

    AR9485_WRITE_REG(Adapter, AR_ISR, 0xffffffff);
    AR9485_WRITE_REG(Adapter, AR_RSSI_THR, AR9485_INIT_RSSI_THR);

    AR9485ResetTransmitQueue(Adapter);
}

/* Republish the BSSID and association id alone, once the AP has told us
 * which AID we hold. */
static VOID
AR9485WriteAssociationId(_In_ PAR9485_ADAPTER Adapter)
{
    AR9485_WRITE_REG(Adapter, AR_BSS_ID0, AR9485ReadLe32(Adapter->Bssid));
    AR9485_WRITE_REG(Adapter, AR_BSS_ID1,
                     (ULONG)AR9485ReadLe16(Adapter->Bssid + 4) |
                     ((Adapter->AssociationId & 0x3fff) << AR_BSS_ID1_AID_S));
}

/* ================================================================== *
 *  Hardware key cache
 * ================================================================== */

/*
 * ath_hw_keysetmac(): the cache stores the peer address shifted right by one
 * bit, with AR_KEYTABLE_VALID marking a unicast entry.  An entry without
 * that bit is usable for multicast decryption, which is exactly what a group
 * key needs.
 */
static VOID
AR9485KeySetMac(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ ULONG Index,
    _In_opt_ const UCHAR *Mac)
{
    ULONG MacLow = 0, MacHigh = 0, UnicastFlag = AR_KEYTABLE_VALID;

    if (Mac != NULL)
    {
        if (Mac[0] & 0x01)
            UnicastFlag = 0;
        MacLow = AR9485ReadLe32(Mac);
        MacHigh = AR9485ReadLe16(Mac + 4);
        MacLow >>= 1;
        MacLow |= (MacHigh & 1) << 31;
        MacHigh >>= 1;
    }

    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_MAC0(Index), MacLow);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_MAC1(Index), MacHigh | UnicastFlag);
}

static VOID
AR9485KeyReset(_In_ PAR9485_ADAPTER Adapter, _In_ ULONG Index)
{
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY0(Index), 0);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY1(Index), 0);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY2(Index), 0);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY3(Index), 0);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY4(Index), 0);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_TYPE(Index), AR_KEYTABLE_TYPE_CLR);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_MAC0(Index), 0);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_MAC1(Index), 0);
}

NDIS_STATUS
AR9485SetCipherKey(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ ULONG Index,
    _In_ ULONG CipherAlgorithm,
    _In_reads_bytes_opt_(KeyLength) PUCHAR Key,
    _In_ ULONG KeyLength,
    _In_reads_bytes_opt_(DOT11_ADDR_LEN) PUCHAR MacAddress)
{
    ULONG Key0, Key1, Key2, Key3, Key4, KeyType;

    if (Index >= AR9485_KEY_CACHE_SIZE)
        return NDIS_STATUS_INVALID_PARAMETER;

    if (Key == NULL || KeyLength == 0 ||
        CipherAlgorithm == DOT11_CIPHER_ALGO_NONE)
    {
        AR9485KeyReset(Adapter, Index);
        if (Index == AR9485_KEY_PAIRWISE)
            Adapter->PairwiseKeyValid = FALSE;
        else
            Adapter->GroupKeyValid = FALSE;
        return NDIS_STATUS_SUCCESS;
    }

    switch (CipherAlgorithm)
    {
        case DOT11_CIPHER_ALGO_CCMP:
            if (KeyLength != 16)
                return NDIS_STATUS_INVALID_LENGTH;
            KeyType = AR_KEYTABLE_TYPE_CCM;
            break;

        case DOT11_CIPHER_ALGO_WEP40:
            if (KeyLength < 5)
                return NDIS_STATUS_INVALID_LENGTH;
            KeyType = AR_KEYTABLE_TYPE_40;
            break;

        case DOT11_CIPHER_ALGO_WEP104:
            if (KeyLength < 13)
                return NDIS_STATUS_INVALID_LENGTH;
            KeyType = AR_KEYTABLE_TYPE_104;
            break;

        default:
            /* TKIP needs a second cache entry for the Michael MIC keys and a
             * software MIC on the transmit side; nwifi's supplicant
             * negotiates CCMP, so nothing here has ever needed it.  Say so
             * rather than install a key the hardware would misuse. */
            DPRINT1("AR9485: cipher 0x%08lx not implemented\n", CipherAlgorithm);
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    /* The split is not arbitrary: the cache is written as five words of
     * 48/48/32 bits (ath_hw_set_keycache_entry). */
    Key0 = AR9485ReadLe32(Key + 0);
    Key1 = AR9485ReadLe16(Key + 4);
    Key2 = AR9485ReadLe32(Key + 6);
    Key3 = AR9485ReadLe16(Key + 10);
    Key4 = AR9485ReadLe32(Key + 12);
    if (KeyType == AR_KEYTABLE_TYPE_40 || KeyType == AR_KEYTABLE_TYPE_104)
        Key4 &= 0xff;

    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY0(Index), Key0);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY1(Index), Key1);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY2(Index), Key2);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY3(Index), Key3);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_KEY4(Index), Key4);
    AR9485_WRITE_REG(Adapter, AR_KEYTABLE_TYPE(Index), KeyType);
    AR9485KeySetMac(Adapter, Index, MacAddress);

    if (Index == AR9485_KEY_PAIRWISE)
        Adapter->PairwiseKeyValid = TRUE;
    else
        Adapter->GroupKeyValid = TRUE;

    DPRINT1("AR9485: key cache %lu programmed, cipher 0x%08lx, %lu bytes\n",
            Index, CipherAlgorithm, KeyLength);
    return NDIS_STATUS_SUCCESS;
}

/* ================================================================== *
 *  Status indications
 * ================================================================== */

static VOID
AR9485IndicateStatus(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ NDIS_STATUS StatusCode,
    _In_reads_bytes_(BufferSize) PVOID Buffer,
    _In_ ULONG BufferSize)
{
    NDIS_STATUS_INDICATION Indication;

    NdisZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size = sizeof(Indication);
    Indication.SourceHandle = Adapter->MiniportAdapterHandle;
    Indication.StatusCode = StatusCode;
    Indication.StatusBuffer = Buffer;
    Indication.StatusBufferSize = BufferSize;
    NdisMIndicateStatusEx(Adapter->MiniportAdapterHandle, &Indication);
}

VOID
AR9485IndicateLinkState(_In_ PAR9485_ADAPTER Adapter, _In_ BOOLEAN Connected)
{
    NDIS_LINK_STATE LinkState;

    NdisZeroMemory(&LinkState, sizeof(LinkState));
    LinkState.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    LinkState.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    LinkState.Header.Size = sizeof(LinkState);
    LinkState.MediaConnectState = Connected ? MediaConnectStateConnected
                                            : MediaConnectStateDisconnected;
    LinkState.MediaDuplexState = MediaDuplexStateFull;
    /* In 100 bps units.  11 Mbit is what the data path actually asks the
     * hardware for, so report that rather than a headline number. */
    LinkState.XmitLinkSpeed = Connected ? 110000ULL : NDIS_LINK_SPEED_UNKNOWN;
    LinkState.RcvLinkSpeed  = Connected ? 110000ULL : NDIS_LINK_SPEED_UNKNOWN;
    LinkState.PauseFunctions = NdisPauseFunctionsUnsupported;

    AR9485IndicateStatus(Adapter, NDIS_STATUS_LINK_STATE,
                         &LinkState, sizeof(LinkState));
}

static VOID
AR9485IndicateConnectionStart(_In_ PAR9485_ADAPTER Adapter)
{
    DOT11_CONNECTION_START_PARAMETERS Parameters;

    NdisZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = DOT11_CONNECTION_START_PARAMETERS_REVISION_1;
    Parameters.Header.Size = sizeof(Parameters);
    Parameters.BSSType = dot11_BSS_type_infrastructure;

    AR9485IndicateStatus(Adapter, NDIS_STATUS_DOT11_CONNECTION_START,
                         &Parameters, sizeof(Parameters));
}

static VOID
AR9485IndicateConnectionCompletion(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ ULONG Status)
{
    DOT11_CONNECTION_COMPLETION_PARAMETERS Parameters;

    NdisZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = DOT11_CONNECTION_COMPLETION_PARAMETERS_REVISION_1;
    Parameters.Header.Size = sizeof(Parameters);
    Parameters.uStatus = Status;

    AR9485IndicateStatus(Adapter, NDIS_STATUS_DOT11_CONNECTION_COMPLETION,
                         &Parameters, sizeof(Parameters));
}

static VOID
AR9485IndicateAssociationStart(_In_ PAR9485_ADAPTER Adapter)
{
    DOT11_ASSOCIATION_START_PARAMETERS Parameters;

    NdisZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = DOT11_ASSOCIATION_START_PARAMETERS_REVISION_1;
    Parameters.Header.Size = sizeof(Parameters);
    NdisMoveMemory(Parameters.MacAddr, Adapter->Bssid, DOT11_ADDR_LEN);
    Parameters.SSID = Adapter->DesiredSsid;

    AR9485IndicateStatus(Adapter, NDIS_STATUS_DOT11_ASSOCIATION_START,
                         &Parameters, sizeof(Parameters));
}

static VOID
AR9485IndicateAssociationCompletion(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ ULONG Status)
{
    DOT11_ASSOCIATION_COMPLETION_PARAMETERS Parameters;

    NdisZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = DOT11_ASSOCIATION_COMPLETION_PARAMETERS_REVISION_1;
    Parameters.Header.Size = sizeof(Parameters);
    NdisMoveMemory(Parameters.MacAddr, Adapter->Bssid, DOT11_ADDR_LEN);
    Parameters.uStatus = Status;
    Parameters.bPortAuthorized = FALSE;

    if (Status == DOT11_ASSOC_STATUS_SUCCESS)
    {
        Parameters.AuthAlgo = Adapter->AuthAlgorithm;
        Parameters.UnicastCipher = Adapter->UnicastCipher;
        Parameters.MulticastCipher = Adapter->MulticastCipher;
        Parameters.DSInfo = DOT11_DS_UNKNOWN;
    }

    /*
     * uIHVDataSize stays zero on purpose.  It is the only place an
     * association completion can carry the AP's RSN element, and when it is
     * empty nwifi's supplicant falls back to the copy it already holds in
     * its own BSS cache - which came from the beacon this driver reported,
     * so it is the same element by a shorter path.
     */
    AR9485IndicateStatus(Adapter, NDIS_STATUS_DOT11_ASSOCIATION_COMPLETION,
                         &Parameters, sizeof(Parameters));
}

static VOID
AR9485IndicateDisassociation(_In_ PAR9485_ADAPTER Adapter, _In_ ULONG Reason)
{
    DOT11_DISASSOCIATION_PARAMETERS Parameters;

    NdisZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = DOT11_DISASSOCIATION_PARAMETERS_REVISION_1;
    Parameters.Header.Size = sizeof(Parameters);
    NdisMoveMemory(Parameters.MacAddr, Adapter->Bssid, DOT11_ADDR_LEN);
    Parameters.uReason = Reason;

    AR9485IndicateStatus(Adapter, NDIS_STATUS_DOT11_DISASSOCIATION,
                         &Parameters, sizeof(Parameters));
}

/* ================================================================== *
 *  Management-frame construction
 * ================================================================== */

static ULONG
AR9485BuildMacHeader(
    _Out_writes_bytes_(DOT11_MAC_HEADER_LEN) PUCHAR Frame,
    _In_ UCHAR Subtype,
    _In_ const UCHAR *Bssid,
    _In_ const UCHAR *SourceAddress)
{
    PDOT11_MAC_HEADER_3ADDR Header = (PDOT11_MAC_HEADER_3ADDR)Frame;

    NdisZeroMemory(Header, DOT11_MAC_HEADER_LEN);
    Header->FrameControl[0] = (UCHAR)(DOT11_FC0_TYPE_MGMT | Subtype);
    Header->FrameControl[1] = 0;
    Header->Duration = 0;
    /* A management frame to the AP addresses it three times over: receiver,
     * transmitter, and the BSS it belongs to. */
    NdisMoveMemory(Header->Address1, Bssid, DOT11_ADDR_LEN);
    NdisMoveMemory(Header->Address2, SourceAddress, DOT11_ADDR_LEN);
    NdisMoveMemory(Header->Address3, Bssid, DOT11_ADDR_LEN);
    /* AR9485TransmitFrame() fills the sequence number in, for this frame
     * and for the ones nwifi builds, from one counter. */
    Header->SequenceControl = 0;

    return DOT11_MAC_HEADER_LEN;
}

/* Map a dot11 cipher onto its RSN suite selector. */
static UCHAR
AR9485RsnCipherSuite(_In_ ULONG CipherAlgorithm)
{
    switch (CipherAlgorithm)
    {
        case DOT11_CIPHER_ALGO_CCMP:    return RSN_CIPHER_CCMP;
        case DOT11_CIPHER_ALGO_TKIP:    return RSN_CIPHER_TKIP;
        case DOT11_CIPHER_ALGO_WEP40:   return RSN_CIPHER_WEP40;
        case DOT11_CIPHER_ALGO_WEP104:  return RSN_CIPHER_WEP104;
        default:                        return RSN_CIPHER_USE_GROUP;
    }
}

static BOOLEAN
AR9485IsRsnaAuth(_In_ ULONG AuthAlgorithm)
{
    return AuthAlgorithm == DOT11_AUTH_ALGO_RSNA ||
           AuthAlgorithm == DOT11_AUTH_ALGO_RSNA_PSK;
}

/*
 * The RSN element we offer in the association request.  It is built from the
 * cipher pair nwifi programmed rather than copied out of the beacon: the AP
 * advertises what it will accept, and this says which of those we chose.
 */
static ULONG
AR9485BuildRsnIe(_In_ PAR9485_ADAPTER Adapter, _Out_writes_bytes_(22) PUCHAR Out)
{
    ULONG i = 0;

    Out[i++] = DOT11_IE_RSN;
    Out[i++] = 20;
    AR9485WriteLe16(&Out[i], 1);  i += 2;    /* RSN version */

    Out[i++] = RSN_SUITE_OUI_0; Out[i++] = RSN_SUITE_OUI_1; Out[i++] = RSN_SUITE_OUI_2;
    Out[i++] = AR9485RsnCipherSuite(Adapter->MulticastCipher);

    AR9485WriteLe16(&Out[i], 1);  i += 2;    /* one pairwise suite */
    Out[i++] = RSN_SUITE_OUI_0; Out[i++] = RSN_SUITE_OUI_1; Out[i++] = RSN_SUITE_OUI_2;
    Out[i++] = AR9485RsnCipherSuite(Adapter->UnicastCipher);

    AR9485WriteLe16(&Out[i], 1);  i += 2;    /* one AKM suite */
    Out[i++] = RSN_SUITE_OUI_0; Out[i++] = RSN_SUITE_OUI_1; Out[i++] = RSN_SUITE_OUI_2;
    Out[i++] = (Adapter->AuthAlgorithm == DOT11_AUTH_ALGO_RSNA_PSK)
                   ? RSN_AKM_PSK : RSN_AKM_8021X;

    AR9485WriteLe16(&Out[i], 0);  i += 2;    /* no RSN capabilities */

    NT_ASSERT(i == 22);
    return i;
}

static NDIS_STATUS
AR9485SendAuthenticate(_In_ PAR9485_ADAPTER Adapter)
{
    UCHAR Frame[DOT11_MAC_HEADER_LEN + 6];
    ULONG Length;

    Length = AR9485BuildMacHeader(Frame, DOT11_FC0_SUBTYPE_AUTH,
                                  Adapter->Bssid, Adapter->CurrentMacAddress);
    /* Open System even for WPA2: RSNA authenticates in the 4-way handshake,
     * not in the 802.11 authentication exchange. */
    AR9485WriteLe16(&Frame[Length], DOT11_AUTH_ALG_OPEN);       Length += 2;
    AR9485WriteLe16(&Frame[Length], 1);                          Length += 2;
    AR9485WriteLe16(&Frame[Length], 0);                          Length += 2;

    return AR9485TransmitFrame(Adapter, Frame, Length, AR9485_KEY_NONE,
                               AR9485_RATE_1M, FALSE, NULL);
}

static NDIS_STATUS
AR9485SendAssociate(_In_ PAR9485_ADAPTER Adapter)
{
    UCHAR Frame[DOT11_MAC_HEADER_LEN + 4 + 2 + DOT11_SSID_MAX_LENGTH +
                2 + sizeof(Adapter->BssRates) + 22];
    ULONG Length;
    USHORT Capability;
    ULONG RateCount, Extended;

    Length = AR9485BuildMacHeader(Frame, DOT11_FC0_SUBTYPE_ASSOC_REQ,
                                  Adapter->Bssid, Adapter->CurrentMacAddress);

    Capability = DOT11_CAPABILITY_ESS | DOT11_CAPABILITY_SHORT_PREAMBLE;
    if (Adapter->BssCapability & DOT11_CAPABILITY_PRIVACY)
        Capability |= DOT11_CAPABILITY_PRIVACY;
    if (Adapter->BssCapability & DOT11_CAPABILITY_SHORT_SLOT)
        Capability |= DOT11_CAPABILITY_SHORT_SLOT;
    AR9485WriteLe16(&Frame[Length], Capability);    Length += 2;
    AR9485WriteLe16(&Frame[Length], 10);            Length += 2;  /* listen interval */

    /* SSID element. */
    Frame[Length++] = DOT11_IE_SSID;
    Frame[Length++] = (UCHAR)Adapter->DesiredSsid.uSSIDLength;
    NdisMoveMemory(&Frame[Length], Adapter->DesiredSsid.ucSSID,
                   Adapter->DesiredSsid.uSSIDLength);
    Length += Adapter->DesiredSsid.uSSIDLength;

    /* Supported rates, echoing what the BSS advertised.  The element holds
     * at most eight; anything beyond goes in the extended element. */
    RateCount = min(Adapter->BssRateCount, 8UL);
    if (RateCount != 0)
    {
        Frame[Length++] = DOT11_IE_SUPPORTED_RATES;
        Frame[Length++] = (UCHAR)RateCount;
        NdisMoveMemory(&Frame[Length], Adapter->BssRates, RateCount);
        Length += RateCount;

        Extended = Adapter->BssRateCount - RateCount;
        if (Extended != 0)
        {
            Frame[Length++] = DOT11_IE_EXTENDED_RATES;
            Frame[Length++] = (UCHAR)Extended;
            NdisMoveMemory(&Frame[Length], Adapter->BssRates + RateCount, Extended);
            Length += Extended;
        }
    }

    if (AR9485IsRsnaAuth(Adapter->AuthAlgorithm))
        Length += AR9485BuildRsnIe(Adapter, &Frame[Length]);

    NT_ASSERT(Length <= sizeof(Frame));
    return AR9485TransmitFrame(Adapter, Frame, Length, AR9485_KEY_NONE,
                               AR9485_RATE_1M, FALSE, NULL);
}

static VOID
AR9485SendDeauthenticate(_In_ PAR9485_ADAPTER Adapter, _In_ USHORT Reason)
{
    UCHAR Frame[DOT11_MAC_HEADER_LEN + 2];
    ULONG Length;

    Length = AR9485BuildMacHeader(Frame, DOT11_FC0_SUBTYPE_DEAUTH,
                                  Adapter->Bssid, Adapter->CurrentMacAddress);
    AR9485WriteLe16(&Frame[Length], Reason);    Length += 2;

    (VOID)AR9485TransmitFrame(Adapter, Frame, Length, AR9485_KEY_NONE,
                              AR9485_RATE_1M, FALSE, NULL);
}

/* ================================================================== *
 *  Management-frame reception
 * ================================================================== */

VOID
AR9485MlmeReceiveManagement(
    _In_ PAR9485_ADAPTER Adapter,
    _In_reads_bytes_(Length) PUCHAR Frame,
    _In_ ULONG Length)
{
    PDOT11_MAC_HEADER_3ADDR Header = (PDOT11_MAC_HEADER_3ADDR)Frame;
    PUCHAR Body = Frame + DOT11_MAC_HEADER_LEN;
    ULONG BodyLength = Length - DOT11_MAC_HEADER_LEN;
    UCHAR Subtype;

    if (Length < DOT11_MAC_HEADER_LEN)
        return;

    Subtype = Header->FrameControl[0] & DOT11_FC0_SUBTYPE_MASK;

    /* Only the BSS we are joining or hold has anything to say to us. */
    if (Adapter->MlmeState == AR9485MlmeIdle ||
        !AR9485AddressEqual(Header->Address3, Adapter->Bssid))
        return;
    if (!AR9485AddressEqual(Header->Address1, Adapter->CurrentMacAddress) &&
        Subtype != DOT11_FC0_SUBTYPE_BEACON &&
        (Header->Address1[0] & 0x01) == 0)
        return;

    switch (Subtype)
    {
        case DOT11_FC0_SUBTYPE_AUTH:
        {
            USHORT Sequence;

            if (BodyLength < 6)
                return;
            Sequence = AR9485ReadLe16(Body + 2);
            /* Sequence 2 is the AP's half of an Open System exchange. */
            if (Sequence != 2)
                return;
            Adapter->AuthStatus = AR9485ReadLe16(Body + 4);
            InterlockedExchange(&Adapter->AuthResponseSeen, 1);
            DPRINT1("AR9485: authentication response status %u\n",
                    Adapter->AuthStatus);
            break;
        }

        case DOT11_FC0_SUBTYPE_ASSOC_RESP:
        case DOT11_FC0_SUBTYPE_REASSOC_RESP:
        {
            if (BodyLength < 6)
                return;
            Adapter->BssCapability = AR9485ReadLe16(Body);
            Adapter->AssocStatus = AR9485ReadLe16(Body + 2);
            Adapter->AssociationId = AR9485ReadLe16(Body + 4) & 0x3fff;
            InterlockedExchange(&Adapter->AssocResponseSeen, 1);
            DPRINT1("AR9485: association response status %u AID %u\n",
                    Adapter->AssocStatus, Adapter->AssociationId);
            break;
        }

        case DOT11_FC0_SUBTYPE_DEAUTH:
        case DOT11_FC0_SUBTYPE_DISASSOC:
        {
            Adapter->DeauthReason = (BodyLength >= 2) ? AR9485ReadLe16(Body)
                                                      : DOT11_REASON_UNSPECIFIED;
            InterlockedExchange(&Adapter->DeauthSeen, 1);
            DPRINT1("AR9485: %s from AP, reason %u\n",
                    (Subtype == DOT11_FC0_SUBTYPE_DEAUTH) ? "deauthentication"
                                                          : "disassociation",
                    Adapter->DeauthReason);
            break;
        }

        default:
            break;
    }
}

/* ================================================================== *
 *  Join / authenticate / associate
 * ================================================================== */

/*
 * Poll the radio until a flag the receive path sets goes up, or the timeout
 * expires.  Both the MLME and the receive path run on the chip worker, so
 * this is what lets a linear connect sequence see a response at all.
 */
static BOOLEAN
AR9485WaitForResponse(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ volatile LONG *Flag,
    _In_ ULONG TimeoutMs)
{
    ULONGLONG Deadline;

    /* Count real time, not requested time.  KeDelayExecutionThread rounds a
     * sleep up to the next clock tick (~15.6 ms), so charging the loop the
     * REQUESTED AR9485_MGMT_POLL_MS made a nominal 500 ms wait run for about
     * 4.17 s -- which is exactly the spacing the three "authentication attempt
     * N timed out" lines were observed at.  KeQueryInterruptTime is in 100 ns
     * units and is what the tick actually advances. */
    Deadline = KeQueryInterruptTime() + (ULONGLONG)TimeoutMs * 10000ULL;

    for (;;)
    {
        AR9485ReapTransmitStatus(Adapter);
        AR9485PollReceive(Adapter);

        if (InterlockedCompareExchange(Flag, 0, 0) != 0)
            return TRUE;
        if (InterlockedCompareExchange(&Adapter->DeauthSeen, 0, 0) != 0)
            return FALSE;
        if (Adapter->Flags & AR9485_FLAG_HALTING)
            return FALSE;
        if (KeQueryInterruptTime() >= Deadline)
            return FALSE;

        AR9485Sleep(AR9485_MGMT_POLL_MS);
    }
}

/* Pull the SSID, the supported-rate set and the channel out of a cached
 * beacon so the association request can echo them back. */
static VOID
AR9485AdoptBss(_In_ PAR9485_ADAPTER Adapter, _In_ PAR9485_BSS Bss)
{
    const UCHAR *Ie;
    ULONG IeLength;

    NdisMoveMemory(Adapter->Bssid, Bss->Bssid, DOT11_ADDR_LEN);
    Adapter->BssChannelMHz = (USHORT)Bss->ChannelMHz;
    Adapter->BssCapability = Bss->CapabilityInformation;
    Adapter->BssRateCount = 0;

    Ie = AR9485FindIe(Bss->Ies, Bss->IeLength, DOT11_IE_SUPPORTED_RATES,
                      &IeLength);
    if (Ie != NULL && IeLength != 0)
    {
        IeLength = min(IeLength, sizeof(Adapter->BssRates));
        NdisMoveMemory(Adapter->BssRates, Ie, IeLength);
        Adapter->BssRateCount = IeLength;
    }

    Ie = AR9485FindIe(Bss->Ies, Bss->IeLength, DOT11_IE_EXTENDED_RATES,
                      &IeLength);
    if (Ie != NULL && IeLength != 0)
    {
        ULONG Room = sizeof(Adapter->BssRates) - Adapter->BssRateCount;

        IeLength = min(IeLength, Room);
        NdisMoveMemory(Adapter->BssRates + Adapter->BssRateCount, Ie, IeLength);
        Adapter->BssRateCount += IeLength;
    }

    /* An SSID the caller did not name comes from the beacon, so a
     * BSSID-only connect request still produces a correct association
     * request and a correct ASSOCIATION_START indication. */
    if (Adapter->DesiredSsid.uSSIDLength == 0)
    {
        Ie = AR9485FindIe(Bss->Ies, Bss->IeLength, DOT11_IE_SSID, &IeLength);
        if (Ie != NULL && IeLength <= DOT11_SSID_MAX_LENGTH)
        {
            Adapter->DesiredSsid.uSSIDLength = IeLength;
            NdisMoveMemory(Adapter->DesiredSsid.ucSSID, Ie, IeLength);
        }
    }
}

/* Choose the BSS to join: an explicitly named BSSID wins, otherwise the
 * strongest beacon carrying the desired SSID. */
static PAR9485_BSS
AR9485SelectBss(_In_ PAR9485_ADAPTER Adapter)
{
    PAR9485_BSS Best = NULL;
    ULONG i;

    for (i = 0; i < Adapter->BssCount; ++i)
    {
        PAR9485_BSS Bss = &Adapter->Bss[i];
        const UCHAR *Ie;
        ULONG IeLength;

        if (Adapter->HaveDesiredBssid)
        {
            if (AR9485AddressEqual(Bss->Bssid, Adapter->DesiredBssid))
                return Bss;
            continue;
        }

        if (Adapter->DesiredSsid.uSSIDLength == 0)
            continue;

        Ie = AR9485FindIe(Bss->Ies, Bss->IeLength, DOT11_IE_SSID, &IeLength);
        if (Ie == NULL || IeLength != Adapter->DesiredSsid.uSSIDLength)
            continue;
        if (RtlCompareMemory(Ie, Adapter->DesiredSsid.ucSSID, IeLength) != IeLength)
            continue;

        if (Best == NULL || Bss->Rssi > Best->Rssi)
            Best = Bss;
    }

    return Best;
}

static VOID
AR9485TearDownLink(
    _In_ PAR9485_ADAPTER Adapter,
    _In_ BOOLEAN NotifyPeer,
    _In_ ULONG Reason)
{
    BOOLEAN WasAssociated = (Adapter->MlmeState == AR9485MlmeAssociated);

    if (NotifyPeer && Adapter->MlmeState != AR9485MlmeIdle)
        AR9485SendDeauthenticate(Adapter, DOT11_REASON_LEAVING);

    Adapter->MlmeState = AR9485MlmeIdle;
    Adapter->AssociationId = 0;
    Adapter->PairwiseKeyValid = FALSE;
    Adapter->GroupKeyValid = FALSE;
    InterlockedExchange(&Adapter->DeauthSeen, 0);

    /* Drop every key: the next association negotiates fresh ones, and a
     * stale pairwise key would encrypt with something the new AP cannot
     * read. */
    AR9485SetCipherKey(Adapter, AR9485_KEY_PAIRWISE, DOT11_CIPHER_ALGO_NONE,
                       NULL, 0, NULL);
    AR9485SetCipherKey(Adapter, 0, DOT11_CIPHER_ALGO_NONE, NULL, 0, NULL);

    AR9485FlushTransmitQueue(Adapter);
    AR9485WriteAssociationId(Adapter);

    if (WasAssociated)
    {
        AR9485IndicateLinkState(Adapter, FALSE);
        AR9485IndicateDisassociation(Adapter, Reason);
    }
}

static VOID
AR9485DoConnect(_In_ PAR9485_ADAPTER Adapter)
{
    PAR9485_BSS Bss;
    ULONG Attempt;
    ULONG FailureStatus = DOT11_ASSOC_STATUS_CANDIDATE_LIST_EXHAUSTED;

    if (Adapter->MlmeState != AR9485MlmeIdle)
        AR9485TearDownLink(Adapter, TRUE, DOT11_ASSOC_STATUS_DISASSOCIATED_BY_OS);

    /* A deauthentication left over from a previous attempt would otherwise
     * abort every wait in this one before the AP had a chance to answer. */
    InterlockedExchange(&Adapter->DeauthSeen, 0);

    AR9485IndicateConnectionStart(Adapter);

    Bss = AR9485SelectBss(Adapter);
    if (Bss == NULL)
    {
        /* nwifi normally scans before it connects, but a connect request
         * that arrives with a cold cache should still work. */
        DPRINT1("AR9485: no cached BSS matches; scanning before connect\n");
        AR9485RunScan(Adapter);
        Bss = AR9485SelectBss(Adapter);
    }
    if (Bss == NULL)
    {
        DPRINT1("AR9485: connect failed: requested BSS not found\n");
        goto Failed;
    }

    AR9485AdoptBss(Adapter, Bss);
    Adapter->MlmeState = AR9485MlmeJoining;
    Adapter->AssociationId = 0;

    DPRINT1("AR9485: joining %02x:%02x:%02x:%02x:%02x:%02x on %u MHz\n",
            Adapter->Bssid[0], Adapter->Bssid[1], Adapter->Bssid[2],
            Adapter->Bssid[3], Adapter->Bssid[4], Adapter->Bssid[5],
            Adapter->BssChannelMHz);

    /* Park the radio on the BSS's channel and rebuild everything the reset
     * clears, then arm the receiver so the responses have somewhere to land. */
    if (!ar9485_hw_start(Adapter->HwContext, Adapter->IoBase, Adapter->IoLength,
                         Adapter->DeviceId, Adapter->MacVersion,
                         (USHORT)Adapter->MacRevision, Adapter->BssChannelMHz))
    {
        DPRINT1("AR9485: PHY bring-up failed on %u MHz\n", Adapter->BssChannelMHz);
        FailureStatus = DOT11_ASSOC_STATUS_SYSTEM_ERROR;
        goto Failed;
    }
    Adapter->CurrentChannelMHz = Adapter->BssChannelMHz;
    AR9485ProgramMacState(Adapter);
    AR9485ArmReceiver(Adapter);

    AR9485IndicateAssociationStart(Adapter);

    /* --- 802.11 authentication (Open System) --- */
    Adapter->MlmeState = AR9485MlmeAuthenticating;
    InterlockedExchange(&Adapter->AuthResponseSeen, 0);
    for (Attempt = 0; Attempt < AR9485_MGMT_ATTEMPTS; ++Attempt)
    {
        if (AR9485SendAuthenticate(Adapter) != NDIS_STATUS_SUCCESS)
            continue;
        if (AR9485WaitForResponse(Adapter, &Adapter->AuthResponseSeen,
                                  AR9485_MGMT_TIMEOUT_MS))
            break;
        DPRINT1("AR9485: authentication attempt %lu timed out\n", Attempt + 1);
    }
    if (!InterlockedCompareExchange(&Adapter->AuthResponseSeen, 0, 0))
    {
        FailureStatus = DOT11_ASSOC_STATUS_UNREACHABLE;
        goto Failed;
    }
    if (Adapter->AuthStatus != DOT11_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: AP refused authentication, status %u\n",
                Adapter->AuthStatus);
        FailureStatus = DOT11_ASSOC_STATUS_ASSOCIATION_RESPONSE |
                        (Adapter->AuthStatus & DOT11_ASSOC_STATUS_REASON_CODE_MASK);
        goto Failed;
    }

    /* --- Association --- */
    Adapter->MlmeState = AR9485MlmeAssociating;
    InterlockedExchange(&Adapter->AssocResponseSeen, 0);
    for (Attempt = 0; Attempt < AR9485_MGMT_ATTEMPTS; ++Attempt)
    {
        if (AR9485SendAssociate(Adapter) != NDIS_STATUS_SUCCESS)
            continue;
        if (AR9485WaitForResponse(Adapter, &Adapter->AssocResponseSeen,
                                  AR9485_MGMT_TIMEOUT_MS))
            break;
        DPRINT1("AR9485: association attempt %lu timed out\n", Attempt + 1);
    }
    if (!InterlockedCompareExchange(&Adapter->AssocResponseSeen, 0, 0))
    {
        FailureStatus = DOT11_ASSOC_STATUS_UNREACHABLE;
        goto Failed;
    }
    if (Adapter->AssocStatus != DOT11_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: AP refused association, status %u\n",
                Adapter->AssocStatus);
        FailureStatus = DOT11_ASSOC_STATUS_ASSOCIATION_RESPONSE |
                        (Adapter->AssocStatus & DOT11_ASSOC_STATUS_REASON_CODE_MASK);
        goto Failed;
    }

    /* The AID goes into AR_BSS_ID1 so the MAC can recognise its own bit in
     * the traffic-indication map. */
    AR9485WriteAssociationId(Adapter);
    Adapter->LastBeaconTime = KeQueryInterruptTime();
    Adapter->MlmeState = AR9485MlmeAssociated;

    DPRINT1("AR9485: associated with %02x:%02x:%02x:%02x:%02x:%02x, AID %u\n",
            Adapter->Bssid[0], Adapter->Bssid[1], Adapter->Bssid[2],
            Adapter->Bssid[3], Adapter->Bssid[4], Adapter->Bssid[5],
            Adapter->AssociationId);

    /*
     * The link goes up here for an open network.  For a secure one nwifi
     * keeps the port unauthorized until its supplicant finishes the 4-way
     * handshake; the handshake itself travels over this link, so it has to
     * be up either way.
     */
    AR9485IndicateAssociationCompletion(Adapter, DOT11_ASSOC_STATUS_SUCCESS);
    AR9485IndicateLinkState(Adapter, TRUE);
    AR9485IndicateConnectionCompletion(Adapter, DOT11_CONNECTION_STATUS_SUCCESS);
    return;

Failed:
    Adapter->MlmeState = AR9485MlmeIdle;
    Adapter->AssociationId = 0;
    AR9485WriteAssociationId(Adapter);
    AR9485IndicateAssociationCompletion(Adapter, FailureStatus);
    AR9485IndicateConnectionCompletion(Adapter, DOT11_CONNECTION_STATUS_FAILURE);
}

static VOID
AR9485DoDisconnect(_In_ PAR9485_ADAPTER Adapter)
{
    AR9485TearDownLink(Adapter, TRUE, DOT11_ASSOC_STATUS_DISASSOCIATED_BY_OS);

    /* Back to the parking channel so a later scan starts from a known
     * state. */
    if (ar9485_hw_start(Adapter->HwContext, Adapter->IoBase, Adapter->IoLength,
                        Adapter->DeviceId, Adapter->MacVersion,
                        (USHORT)Adapter->MacRevision,
                        AR9485_DEFAULT_CHANNEL_MHZ))
    {
        Adapter->CurrentChannelMHz = AR9485_DEFAULT_CHANNEL_MHZ;
        AR9485ProgramMacState(Adapter);
    }
}

/* ================================================================== *
 *  The chip worker
 * ================================================================== */

static VOID NTAPI
AR9485ChipWorker(_In_ PVOID Context)
{
    PAR9485_ADAPTER Adapter = (PAR9485_ADAPTER)Context;

    DPRINT1("AR9485: chip worker running\n");

    while (!InterlockedCompareExchange(&Adapter->ChipThreadStop, 0, 0))
    {
        LARGE_INTEGER Interval;
        LONG Command;
        ULONG PollMs = (Adapter->MlmeState == AR9485MlmeIdle)
                           ? AR9485_POLL_IDLE_MS : AR9485_POLL_ACTIVE_MS;

        Interval.QuadPart = -10000LL * (LONGLONG)PollMs;
        KeWaitForSingleObject(&Adapter->ChipWake, Executive, KernelMode,
                              FALSE, &Interval);

        if (InterlockedCompareExchange(&Adapter->ChipThreadStop, 0, 0))
            break;

        /*
         * A lab tool holding the chip is mid-experiment.  Standing down here
         * - and re-checking after the busy flag is up - is what keeps this
         * loop from resetting the PHY underneath a running script.
         */
        if (AR9485LabOwnsChip())
            continue;

        InterlockedExchange(&Adapter->ChipBusy, 1);
        KeClearEvent(&Adapter->ChipIdleEvent);

        if (!AR9485LabOwnsChip() && !(Adapter->Flags & AR9485_FLAG_HALTING))
        {
            Command = InterlockedExchange(&Adapter->ChipCommand, AR9485_CMD_NONE);
            switch (Command)
            {
                case AR9485_CMD_SCAN:
                    AR9485RunScan(Adapter);
                    break;
                case AR9485_CMD_CONNECT:
                    AR9485DoConnect(Adapter);
                    break;
                case AR9485_CMD_DISCONNECT:
                    AR9485DoDisconnect(Adapter);
                    break;
                default:
                    break;
            }

            AR9485ReapTransmitStatus(Adapter);
            AR9485PollReceive(Adapter);

            if (InterlockedCompareExchange(&Adapter->DeauthSeen, 0, 0) &&
                Adapter->MlmeState != AR9485MlmeIdle)
            {
                ULONG Reason = DOT11_ASSOC_STATUS_PEER_DEAUTHENTICATED |
                               (Adapter->DeauthReason &
                                DOT11_ASSOC_STATUS_REASON_CODE_MASK);

                AR9485TearDownLink(Adapter, FALSE, Reason);
            }
            else if (Adapter->MlmeState == AR9485MlmeAssociated &&
                     KeQueryInterruptTime() - Adapter->LastBeaconTime >
                         (ULONG64)AR9485_BEACON_TIMEOUT_MS * 10000ULL)
            {
                DPRINT1("AR9485: no beacon from the BSS for %u ms; link lost\n",
                        AR9485_BEACON_TIMEOUT_MS);
                AR9485TearDownLink(Adapter, FALSE,
                                   DOT11_ASSOC_STATUS_UNREACHABLE);
            }
        }

        InterlockedExchange(&Adapter->ChipBusy, 0);
        KeSetEvent(&Adapter->ChipIdleEvent, IO_NO_INCREMENT, FALSE);
    }

    DPRINT1("AR9485: chip worker exiting\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NDIS_STATUS
AR9485StartChipWorker(_In_ PAR9485_ADAPTER Adapter)
{
    HANDLE ThreadHandle;
    NTSTATUS Status;

    KeInitializeEvent(&Adapter->ChipWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Adapter->ChipIdleEvent, NotificationEvent, TRUE);
    Adapter->ChipCommand = AR9485_CMD_NONE;
    Adapter->ChipThreadStop = 0;

    Status = PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS, NULL,
                                  NULL, NULL, AR9485ChipWorker, Adapter);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("AR9485: cannot create chip worker: 0x%08lx\n", Status);
        return NDIS_STATUS_RESOURCES;
    }

    /* Keep a referenced pointer so the halt path can wait for the thread to
     * leave the adapter alone before the adapter is freed. */
    Status = ObReferenceObjectByHandle(ThreadHandle, THREAD_ALL_ACCESS, NULL,
                                       KernelMode, &Adapter->ChipThread, NULL);
    ZwClose(ThreadHandle);
    if (!NT_SUCCESS(Status))
    {
        Adapter->ChipThread = NULL;
        InterlockedExchange(&Adapter->ChipThreadStop, 1);
        KeSetEvent(&Adapter->ChipWake, IO_NO_INCREMENT, FALSE);
        return NDIS_STATUS_RESOURCES;
    }

    return NDIS_STATUS_SUCCESS;
}

VOID
AR9485StopChipWorker(_In_ PAR9485_ADAPTER Adapter)
{
    if (Adapter->ChipThread == NULL)
        return;

    InterlockedExchange(&Adapter->ChipThreadStop, 1);
    KeSetEvent(&Adapter->ChipWake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(Adapter->ChipThread, Executive, KernelMode,
                          FALSE, NULL);
    ObDereferenceObject(Adapter->ChipThread);
    Adapter->ChipThread = NULL;
}

VOID
AR9485QueueCommand(_In_ PAR9485_ADAPTER Adapter, _In_ LONG Command)
{
    InterlockedExchange(&Adapter->ChipCommand, Command);
    KeSetEvent(&Adapter->ChipWake, IO_NO_INCREMENT, FALSE);
}
