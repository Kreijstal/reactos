/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Station connection monitor: idle keep-alive, beacon-loss
 *              probing and transmit-stall detection while associated.
 *
 * The miniport used to have exactly one liveness rule: three seconds without
 * a beacon from our BSS tears the link down.  Nothing ever transmitted on an
 * idle link, and the FRITZ!Box 7330 disassociates a station it has not heard
 * from: the boot-86 air capture shows the STA silent from t+3 s and the AP's
 * Disassociation reason 2 at t+278 s (boot 85: t+210 s), with no group rekey
 * in between.  On boot 175 tcpip routes everything out of the wired NIC
 * (#300), so the radio is just as silent after DHCP, and the link drops the
 * same way a few minutes in.
 *
 * mac80211 never lets a station go quiet like that.  Its connection monitor
 * (net/mac80211/mlme.c, ieee80211_sta_conn_mon_timer) probes the AP with a
 * null-data frame once IEEE80211_CONNECTION_IDLE_TIME (30 s) passes without an
 * acknowledged frame, and beacon loss (IEEE80211_BEACON_LOSS_COUNT beacons)
 * starts the same probe instead of disconnecting at once; only a probe the AP
 * fails to acknowledge IEEE80211_MAX_NULLFUNC_TRIES times ends the link
 * (ieee80211_mgd_probe_ap / ieee80211_sta_connection_lost).  This file is that
 * policy, plus ath9k's TX-hang check (ath_tx_complete_poll_work: frames
 * pending and no completion across polls means the chip needs a reset, which
 * the reconnect after a teardown performs).
 *
 * Two deliberate differences from mac80211:
 *   - "idle" means no ACKED TRANSMIT for 30 s, not "no unicast receive".  The
 *     AP's inactivity timer counts what the station sends, and unicast
 *     traffic arriving from the AP says nothing about that.
 *   - beacons absent for AR9485_LINKMON_BEACON_DEAD_MS end the link even if
 *     probes are acknowledged.  ACKs are matched by the MAC, so they keep
 *     arriving when the receive DMA ring is what died; only a reset brings
 *     that back.
 *
 * No NDIS or kernel dependency: test_linkmon.c builds this on the host.
 * Times are KeQueryInterruptTime() units (100 ns).
 */

#ifndef _AR9485_LINKMON_H_
#define _AR9485_LINKMON_H_

typedef unsigned long long AR9485_LM_TIME;
typedef unsigned int       AR9485_LM_U32;
typedef unsigned char      AR9485_LM_U8;

#define AR9485_LM_MS(ms)    ((AR9485_LM_TIME)(ms) * 10000ULL)

/* mac80211 IEEE80211_CONNECTION_IDLE_TIME. */
#define AR9485_LINKMON_IDLE_PROBE_MS    30000
/* mac80211 IEEE80211_BEACON_LOSS_COUNT is 7 beacons; at the usual 102.4 ms
 * interval that is ~0.7 s.  Rounded up: the receive ring is polled. */
#define AR9485_LINKMON_BEACON_LOSS_MS   1000
/* How long a probe may stay without a transmit status before it counts as
 * unacknowledged (mac80211 waits IEEE80211_PROBE_WAIT_MS-ish, HZ/2). */
#define AR9485_LINKMON_PROBE_WAIT_MS    500
/* mac80211 IEEE80211_MAX_NULLFUNC_TRIES. */
#define AR9485_LINKMON_PROBE_TRIES      2
/* Beacons gone this long ends the link whatever the probes say (see above). */
#define AR9485_LINKMON_BEACON_DEAD_MS   10000
/* Frames pending with no completion at all for this long: transmit hang. */
#define AR9485_LINKMON_TX_STALL_MS      3000

typedef enum _AR9485_LINKMON_ACTION
{
    AR9485LinkMonNone = 0,
    AR9485LinkMonSendProbe,
    AR9485LinkMonLinkLost
} AR9485_LINKMON_ACTION;

/* Why a link was declared lost (AR9485_LINKMON.LastLostReason). */
#define AR9485_LINKMON_LOST_NONE        0
#define AR9485_LINKMON_LOST_PROBE       1   /* AP did not ACK the probes    */
#define AR9485_LINKMON_LOST_BEACON      2   /* beacons gone for DEAD_MS      */
#define AR9485_LINKMON_LOST_TX_STALL    3   /* transmit engine stopped       */

/* Why a probe was sent (AR9485_LINKMON.LastProbeReason). */
#define AR9485_LINKMON_PROBE_IDLE       1
#define AR9485_LINKMON_PROBE_BEACON     2
#define AR9485_LINKMON_PROBE_RETRY      3

typedef struct _AR9485_LINKMON
{
    /* Per-link timing, rearmed by AR9485LinkMonReset(). */
    AR9485_LM_TIME LastBeacon;      /* last beacon from our BSS            */
    AR9485_LM_TIME LastAck;         /* last transmit the AP acknowledged   */
    AR9485_LM_TIME LastProbe;       /* when the last probe went out        */
    AR9485_LM_TIME ProbeInFlight;   /* send time of the outstanding probe  */
    AR9485_LM_TIME StallSince;      /* frames pending, no completion since */
    AR9485_LM_U32  ProbeFailures;   /* consecutive unacknowledged probes   */
    AR9485_LM_U32  BeaconLost;      /* currently inside a beacon gap       */

    /* Cumulative, never reset: these are what a post-mortem reads. */
    AR9485_LM_U32  Links;           /* associations monitored              */
    AR9485_LM_U32  ProbesSent;
    AR9485_LM_U32  ProbesAcked;
    AR9485_LM_U32  ProbesFailed;    /* status said not acknowledged        */
    AR9485_LM_U32  ProbesTimedOut;  /* no status came back at all          */
    AR9485_LM_U32  IdleProbes;
    AR9485_LM_U32  BeaconLossEvents;
    AR9485_LM_U32  LinksLost;
    AR9485_LM_U32  LastLostReason;
    AR9485_LM_U32  LastProbeReason;
    AR9485_LM_TIME LastLostTime;
    AR9485_LM_TIME LinkStart;
} AR9485_LINKMON, *PAR9485_LINKMON;

/* Start monitoring a fresh association. */
static __inline void
AR9485LinkMonReset(PAR9485_LINKMON M, AR9485_LM_TIME Now)
{
    M->LastBeacon = Now;
    M->LastAck = Now;
    M->LastProbe = 0;
    M->ProbeInFlight = 0;
    M->StallSince = 0;
    M->ProbeFailures = 0;
    M->BeaconLost = 0;
    M->LinkStart = Now;
    M->Links++;
}

static __inline void
AR9485LinkMonOnBeacon(PAR9485_LINKMON M, AR9485_LM_TIME Now)
{
    M->LastBeacon = Now;
    M->BeaconLost = 0;
}

/* Every transmit completion.  Acked means AR_FrmXmitOK: the AP ACKed it. */
static __inline void
AR9485LinkMonOnTxDone(PAR9485_LINKMON M, AR9485_LM_TIME Now, int Acked)
{
    M->StallSince = 0;
    if (Acked)
        M->LastAck = Now;
}

/* The completion of a probe (after AR9485LinkMonOnTxDone for the same frame),
 * or a probe that could not even be queued (Acked = 0). */
static __inline void
AR9485LinkMonOnProbeDone(PAR9485_LINKMON M, AR9485_LM_TIME Now, int Acked)
{
    (void)Now;
    if (M->ProbeInFlight == 0)
        return;     /* already written off as timed out */
    M->ProbeInFlight = 0;
    if (Acked)
    {
        M->ProbesAcked++;
        M->ProbeFailures = 0;
    }
    else
    {
        M->ProbesFailed++;
        M->ProbeFailures++;
    }
}

static __inline AR9485_LINKMON_ACTION
AR9485LinkMonLost(PAR9485_LINKMON M, AR9485_LM_TIME Now, AR9485_LM_U32 Reason)
{
    M->LinksLost++;
    M->LastLostReason = Reason;
    M->LastLostTime = Now;
    M->ProbeInFlight = 0;
    return AR9485LinkMonLinkLost;
}

/*
 * One evaluation, run on every chip-worker pass while associated.  TxPending
 * is the number of frames handed to the hardware and not yet completed.
 */
static __inline AR9485_LINKMON_ACTION
AR9485LinkMonTick(PAR9485_LINKMON M, AR9485_LM_TIME Now, AR9485_LM_U32 TxPending)
{
    AR9485_LM_U32 Reason;

    /* Transmit hang: frames queued and nothing completing. */
    if (TxPending == 0)
        M->StallSince = 0;
    else if (M->StallSince == 0)
        M->StallSince = Now;
    else if (Now - M->StallSince >= AR9485_LM_MS(AR9485_LINKMON_TX_STALL_MS))
        return AR9485LinkMonLost(M, Now, AR9485_LINKMON_LOST_TX_STALL);

    if (Now - M->LastBeacon >= AR9485_LM_MS(AR9485_LINKMON_BEACON_DEAD_MS))
        return AR9485LinkMonLost(M, Now, AR9485_LINKMON_LOST_BEACON);

    if (M->ProbeInFlight != 0)
    {
        if (Now - M->ProbeInFlight < AR9485_LM_MS(AR9485_LINKMON_PROBE_WAIT_MS))
            return AR9485LinkMonNone;
        /* No status at all: count it as unacknowledged. */
        M->ProbeInFlight = 0;
        M->ProbesTimedOut++;
        M->ProbeFailures++;
    }

    if (M->ProbeFailures >= AR9485_LINKMON_PROBE_TRIES)
        return AR9485LinkMonLost(M, Now, AR9485_LINKMON_LOST_PROBE);

    if (Now - M->LastBeacon >= AR9485_LM_MS(AR9485_LINKMON_BEACON_LOSS_MS))
    {
        if (!M->BeaconLost)
        {
            M->BeaconLost = 1;
            M->BeaconLossEvents++;
        }
    }

    if (M->ProbeFailures != 0)
    {
        /* Retry an unacknowledged probe, spaced like the first wait. */
        if (Now - M->LastProbe < AR9485_LM_MS(AR9485_LINKMON_PROBE_WAIT_MS))
            return AR9485LinkMonNone;
        Reason = AR9485_LINKMON_PROBE_RETRY;
    }
    else if (M->BeaconLost &&
             (M->LastProbe == 0 ||
              Now - M->LastProbe >= AR9485_LM_MS(AR9485_LINKMON_BEACON_LOSS_MS)))
    {
        Reason = AR9485_LINKMON_PROBE_BEACON;
    }
    else if (Now - M->LastAck >= AR9485_LM_MS(AR9485_LINKMON_IDLE_PROBE_MS))
    {
        Reason = AR9485_LINKMON_PROBE_IDLE;
        M->IdleProbes++;
    }
    else
    {
        return AR9485LinkMonNone;
    }

    M->LastProbe = Now;
    M->ProbeInFlight = Now ? Now : 1;
    M->ProbesSent++;
    M->LastProbeReason = Reason;
    return AR9485LinkMonSendProbe;
}

/*
 * The probe itself: a null-data frame to the AP (802.11 9.3.2.1, subtype 4),
 * ToDS, power management clear, no body.  It is never encrypted -- there is
 * no payload to protect -- so it needs no key and cannot be a replay.
 * Writes 24 bytes; the sequence number is left for AR9485TransmitFrame().
 */
#define AR9485_NULLDATA_LEN     24

static __inline unsigned int
AR9485BuildNullData(AR9485_LM_U8 *Frame, const AR9485_LM_U8 *Bssid,
                    const AR9485_LM_U8 *Source)
{
    unsigned int i;

    Frame[0] = 0x48;        /* type data (0x08) | subtype null (0x40) */
    Frame[1] = 0x01;        /* ToDS */
    Frame[2] = 0;           /* duration: the MAC fills it */
    Frame[3] = 0;
    for (i = 0; i < 6; ++i)
    {
        Frame[4 + i] = Bssid[i];        /* A1 = receiver = AP     */
        Frame[10 + i] = Source[i];      /* A2 = transmitter = us  */
        Frame[16 + i] = Bssid[i];       /* A3 = DA = BSSID        */
    }
    Frame[22] = 0;
    Frame[23] = 0;
    return AR9485_NULLDATA_LEN;
}

#endif /* _AR9485_LINKMON_H_ */
