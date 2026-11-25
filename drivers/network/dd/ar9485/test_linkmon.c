/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Host unit test for the station connection monitor (linkmon.h).
 *
 * Not built by CMake.  Build and run on the host:
 *     gcc -std=c99 -Wall -Wextra -o test_linkmon test_linkmon.c && ./test_linkmon
 *
 * The scenario tests drive a simulated association through the very
 * AR9485LinkMonTick() the chip worker runs, against a model of the AP that
 * disassociates a station it has not received a frame from for
 * AP_INACTIVITY_MS.  That is what the boot-86 air capture showed the
 * FRITZ!Box 7330 doing (Disassociation reason 2 at t+278 s, boot 85 at
 * t+210 s, the STA silent since t+3 s), so the model uses the shortest
 * observed value.  The pre-fix policy -- tear down after three seconds
 * without a beacon, never transmit -- is simulated next to it as the
 * control: it must LOSE the link in the idle scenario, or the oracle proves
 * nothing.
 */

#include <stdio.h>
#include <string.h>

#include "linkmon.h"

static int g_pass = 0;
static int g_fail = 0;

static void Check(const char *name, int ok)
{
    if (ok) { ++g_pass; printf("  PASS  %s\n", name); }
    else    { ++g_fail; printf("  FAIL  %s\n", name); }
}

/* ------------------------------------------------------------------ */
/*  A tiny world: time in ms, beacons every 102 ms while the AP is up.  */
/* ------------------------------------------------------------------ */

#define TICK_MS             2       /* AR9485_POLL_ACTIVE_MS */
#define BEACON_MS           102
#define AP_INACTIVITY_MS    200000  /* below the shortest observed 210 s */

typedef struct
{
    int  ApUp;              /* beacons are sent                        */
    int  ApAcks;            /* AP acknowledges what we send            */
    int  TxStatusWorks;     /* the hardware reports transmit status    */
    int  RxRingDead;        /* our receive DMA stopped                 */
    int  TxHung;            /* frames never complete                   */
    unsigned long long LastHeardFromSta;    /* AP's view, ms           */
    int  ApDisassociated;
    unsigned TxPending;
    int  ProbeOutstanding;
    unsigned long long ProbeDoneAt;         /* ms when status appears  */
    int  ProbeAck;
} WORLD;

typedef struct
{
    int Lost;
    unsigned long long LostAtMs;
    unsigned Reason;
    unsigned Probes;
} OUTCOME;

/* Run the NEW policy from t=0 for DurationMs. */
static OUTCOME RunNew(WORLD *W, AR9485_LINKMON *M, unsigned long long DurationMs,
                      void (*Script)(WORLD *, unsigned long long))
{
    OUTCOME O;
    unsigned long long t;

    memset(&O, 0, sizeof(O));
    AR9485LinkMonReset(M, AR9485_LM_MS(1));
    W->LastHeardFromSta = 0;

    for (t = 1; t <= DurationMs; t += TICK_MS)
    {
        AR9485_LINKMON_ACTION A;

        if (Script)
            Script(W, t);

        if (W->ApUp && !W->RxRingDead && (t % BEACON_MS) < TICK_MS)
            AR9485LinkMonOnBeacon(M, AR9485_LM_MS(t));

        /* Transmit status for the outstanding probe. */
        if (W->ProbeOutstanding && t >= W->ProbeDoneAt && !W->TxHung)
        {
            W->ProbeOutstanding = 0;
            W->TxPending--;
            if (W->TxStatusWorks)
            {
                AR9485LinkMonOnTxDone(M, AR9485_LM_MS(t), W->ProbeAck);
                AR9485LinkMonOnProbeDone(M, AR9485_LM_MS(t), W->ProbeAck);
            }
        }

        if (!W->ApDisassociated && W->ApUp &&
            t - W->LastHeardFromSta > AP_INACTIVITY_MS)
            W->ApDisassociated = 1;

        A = AR9485LinkMonTick(M, AR9485_LM_MS(t), W->TxPending);
        if (A == AR9485LinkMonSendProbe)
        {
            O.Probes++;
            W->TxPending++;
            W->ProbeOutstanding = 1;
            W->ProbeDoneAt = t + 4;
            W->ProbeAck = W->ApUp && W->ApAcks;
            if (W->ProbeAck)
                W->LastHeardFromSta = t;
        }
        else if (A == AR9485LinkMonLinkLost)
        {
            O.Lost = 1;
            O.LostAtMs = t;
            O.Reason = M->LastLostReason;
            return O;
        }

        /* The AP's disassociation reaches us as a frame; the driver tears
         * down on it (DeauthSeen), which this scenario counts as a loss. */
        if (W->ApDisassociated)
        {
            O.Lost = 1;
            O.LostAtMs = t;
            O.Reason = 100;     /* AP-initiated */
            return O;
        }
    }
    return O;
}

/* The pre-fix policy: beacon watchdog only, the station never transmits. */
static OUTCOME RunOld(WORLD *W, unsigned long long DurationMs,
                      void (*Script)(WORLD *, unsigned long long))
{
    OUTCOME O;
    unsigned long long t, LastBeacon = 1;

    memset(&O, 0, sizeof(O));
    W->LastHeardFromSta = 0;
    for (t = 1; t <= DurationMs; t += TICK_MS)
    {
        if (Script)
            Script(W, t);
        if (W->ApUp && !W->RxRingDead && (t % BEACON_MS) < TICK_MS)
            LastBeacon = t;
        if (!W->ApDisassociated && W->ApUp &&
            t - W->LastHeardFromSta > AP_INACTIVITY_MS)
            W->ApDisassociated = 1;
        if (t - LastBeacon > 3000)
        {
            O.Lost = 1; O.LostAtMs = t; O.Reason = AR9485_LINKMON_LOST_BEACON;
            return O;
        }
        if (W->ApDisassociated)
        {
            O.Lost = 1; O.LostAtMs = t; O.Reason = 100;
            return O;
        }
    }
    return O;
}

static void InitWorld(WORLD *W)
{
    memset(W, 0, sizeof(*W));
    W->ApUp = 1;
    W->ApAcks = 1;
    W->TxStatusWorks = 1;
}

/* Scripts. */
static void ScriptApVanishesAt45(WORLD *W, unsigned long long t)
{
    if (t >= 45000) W->ApUp = 0;
}
static void ScriptBeaconGap(WORLD *W, unsigned long long t)
{
    /* 2.5 s of lost beacons (interference), AP still there and ACKing. */
    W->RxRingDead = (t >= 40000 && t < 42500);
}
static void ScriptRxRingDiesAt50(WORLD *W, unsigned long long t)
{
    if (t >= 50000) W->RxRingDead = 1;
}
static void ScriptTxHangsAt45(WORLD *W, unsigned long long t)
{
    if (t >= 45000) W->TxHung = 1;
}
static void ScriptNoStatusAt70(WORLD *W, unsigned long long t)
{
    if (t >= 70000) W->TxStatusWorks = 0;
}

int main(void)
{
    WORLD W;
    AR9485_LINKMON M;
    OUTCOME O;

    printf("connection monitor (linkmon.h)\n");

    /* 1. The bug: an idle, healthy link.  Old policy loses it to the AP's
     *    inactivity timer; the new one keeps it for an hour. */
    InitWorld(&W);
    O = RunOld(&W, 3600000, NULL);
    printf("        old policy, idle link: lost=%d at %llu ms reason %u\n",
           O.Lost, O.LostAtMs, O.Reason);
    Check("control: pre-fix policy loses an idle link to AP inactivity",
          O.Lost && O.Reason == 100 && O.LostAtMs < 300000);

    InitWorld(&W);
    memset(&M, 0, sizeof(M));
    O = RunNew(&W, &M, 3600000, NULL);
    printf("        new policy, idle link: lost=%d probes=%u idle=%u acked=%u\n",
           O.Lost, O.Probes, M.IdleProbes, M.ProbesAcked);
    Check("idle link survives an hour", !O.Lost);
    Check("one keep-alive per 30 s of silence (119..121 in an hour)",
          M.IdleProbes >= 119 && M.IdleProbes <= 121);
    Check("every keep-alive acknowledged", M.ProbesAcked == M.ProbesSent);

    /* The first keep-alive must wait the full 30 s of silence. */
    InitWorld(&W);
    memset(&M, 0, sizeof(M));
    O = RunNew(&W, &M, 29990, NULL);
    Check("no keep-alive before 30 s of silence", M.ProbesSent == 0);
    InitWorld(&W);
    memset(&M, 0, sizeof(M));
    O = RunNew(&W, &M, 30010, NULL);
    Check("first keep-alive at 30 s", M.ProbesSent == 1);

    /* 2. AP disappears: beacons stop and nothing is ACKed.  Must be lost
     *    quickly (beacon-loss probe, then retries) -- no slower than the old
     *    3 s rule by more than a second. */
    InitWorld(&W);
    memset(&M, 0, sizeof(M));
    O = RunNew(&W, &M, 120000, ScriptApVanishesAt45);
    printf("        AP vanishes at 45 s: lost=%d at %llu ms reason %u\n",
           O.Lost, O.LostAtMs, O.Reason);
    Check("vanished AP: link lost by probe failure",
          O.Lost && O.Reason == AR9485_LINKMON_LOST_PROBE);
    Check("vanished AP: beacon-loss probe, lost within 1..4 s",
          O.LostAtMs >= 46000 && O.LostAtMs <= 49000 && M.BeaconLossEvents == 1);

    /* 3. A 2.5 s beacon gap with the AP still ACKing must NOT drop the
     *    link (the old rule survived it only by 0.5 s of margin). */
    InitWorld(&W);
    memset(&M, 0, sizeof(M));
    O = RunNew(&W, &M, 120000, ScriptBeaconGap);
    Check("2.5 s beacon gap, AP acking: link kept", !O.Lost);
    Check("beacon gap was probed", M.BeaconLossEvents == 1 &&
          M.ProbesSent > M.IdleProbes);

    /* 4. Our receive ring dies: ACKs still come back, beacons never do.
     *    Must end the link at BEACON_DEAD so the reconnect resets the chip. */
    InitWorld(&W);
    memset(&M, 0, sizeof(M));
    O = RunNew(&W, &M, 120000, ScriptRxRingDiesAt50);
    printf("        RX ring dies at 50 s: lost=%d at %llu ms reason %u\n",
           O.Lost, O.LostAtMs, O.Reason);
    Check("dead RX ring: lost as beacon loss",
          O.Lost && O.Reason == AR9485_LINKMON_LOST_BEACON);
    Check("dead RX ring: at ~10 s",
          O.LostAtMs >= 59900 && O.LostAtMs <= 60100);

    /* 5. Transmit hang: the probe never completes and stays pending. */
    InitWorld(&W);
    memset(&M, 0, sizeof(M));
    O = RunNew(&W, &M, 200000, ScriptTxHangsAt45);
    printf("        TX hangs at 45 s: lost=%d at %llu ms reason %u\n",
           O.Lost, O.LostAtMs, O.Reason);
    Check("TX hang detected", O.Lost &&
          (O.Reason == AR9485_LINKMON_LOST_TX_STALL ||
           O.Reason == AR9485_LINKMON_LOST_PROBE));
    Check("TX hang: within 4 s of the first stuck frame (60 s probe)",
          O.LostAtMs >= 60000 && O.LostAtMs <= 64000);

    /* 6. Status reporting stops but frames do complete: probes time out
     *    and the link is declared lost rather than probed forever. */
    InitWorld(&W);
    memset(&M, 0, sizeof(M));
    O = RunNew(&W, &M, 200000, ScriptNoStatusAt70);
    Check("no TX status: probes time out, link lost",
          O.Lost && O.Reason == AR9485_LINKMON_LOST_PROBE &&
          M.ProbesTimedOut >= AR9485_LINKMON_PROBE_TRIES);

    /* 7. Real traffic suppresses keep-alives entirely. */
    {
        unsigned long long t;
        memset(&M, 0, sizeof(M));
        AR9485LinkMonReset(&M, AR9485_LM_MS(1));
        for (t = 1; t < 600000; t += TICK_MS)
        {
            if ((t % BEACON_MS) < TICK_MS)
                AR9485LinkMonOnBeacon(&M, AR9485_LM_MS(t));
            if ((t % 10000) < TICK_MS)     /* one ACKed data frame per 10 s */
                AR9485LinkMonOnTxDone(&M, AR9485_LM_MS(t), 1);
            if (AR9485LinkMonTick(&M, AR9485_LM_MS(t), 0) != AR9485LinkMonNone)
                break;
        }
        Check("busy link: no probe, no loss in 10 min", t >= 600000 &&
              M.ProbesSent == 0);
    }

    /* 8. The null-data frame. */
    {
        static const AR9485_LM_U8 Bssid[6] = { 0xc8, 0x0e, 0x14, 0xe4, 0x03, 0xbb };
        static const AR9485_LM_U8 Sta[6]   = { 0x6c, 0x71, 0xd9, 0x68, 0x9c, 0x6d };
        static const AR9485_LM_U8 Want[24] = {
            0x48, 0x01, 0x00, 0x00,
            0xc8, 0x0e, 0x14, 0xe4, 0x03, 0xbb,
            0x6c, 0x71, 0xd9, 0x68, 0x9c, 0x6d,
            0xc8, 0x0e, 0x14, 0xe4, 0x03, 0xbb,
            0x00, 0x00 };
        AR9485_LM_U8 Frame[32];
        unsigned Len;

        memset(Frame, 0xee, sizeof(Frame));
        Len = AR9485BuildNullData(Frame, Bssid, Sta);
        Check("null-data: 24 bytes, data/null, ToDS, A1=A3=BSSID, A2=STA",
              Len == 24 && memcmp(Frame, Want, 24) == 0 && Frame[24] == 0xee);
    }

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
