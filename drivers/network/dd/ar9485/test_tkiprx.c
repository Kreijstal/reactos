/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Host unit test for the group-TKIP receive path in tkiprx.h.
 *
 * Not built by CMake.  Build and run on the host:
 *     gcc -std=c99 -Wall -Wextra -o test_tkiprx test_tkiprx.c && ./test_tkiprx
 *
 * What this proves, without hardware:
 *   - the Michael implementation matches the specification's chained vectors;
 *   - a synthetic AP->STA group frame, shaped exactly like the ones the
 *     FRITZ!Box 7330 puts on the air (FromDS, DA = broadcast, TKIP key ID 2,
 *     80-byte MPDU carrying SNAP + ARP), verifies with GTK[16..23] -- the
 *     authenticator's TX Michael key, which is the half mlme.c captures;
 *   - and it does NOT verify with GTK[24..31], so a half-swap regression
 *     cannot pass silently;
 *   - the trailer arithmetic puts the MIC where the MIC is and leaves the
 *     driver's post-strip frame exactly the 802.11 header + SNAP + payload.
 */

#include <stdio.h>
#include <string.h>

#include "tkiprx.h"

static int g_pass = 0;
static int g_fail = 0;

static void Check(const char *name, int ok)
{
    if (ok) { ++g_pass; printf("  PASS  %s\n", name); }
    else    { ++g_fail; printf("  FAIL  %s\n", name); }
}

static void PrintHex(const char *label, const unsigned char *p, unsigned n)
{
    unsigned i;
    printf("        %s", label);
    for (i = 0; i < n; ++i) printf("%02x", p[i]);
    printf("\n");
}

/* ------------------------------------------------------------------ *
 *  1.  Michael, the specification's chained test vectors
 * ------------------------------------------------------------------ */

static void TestMichaelVectors(void)
{
    /* Each line: the key, the message, and the expected MIC.  The key of
     * every line but the first is the MIC of the line before it (the
     * "chained" form the TKIP specification publishes). */
    static const struct {
        const char *Key;        /* 8 bytes, hex */
        const char *Message;
        const char *Mic;        /* 8 bytes, hex */
    } Vectors[] = {
        { "0000000000000000", "",        "82925c1ca1d130b8" },
        { "82925c1ca1d130b8", "M",       "434721ca40639b3f" },
        { "434721ca40639b3f", "Mi",      "e8f9becae97e5d29" },
        { "e8f9becae97e5d29", "Mic",     "90038fc6cf13c1db" },
        { "90038fc6cf13c1db", "Mich",    "d55e100510128986" },
        { "d55e100510128986", "Michael", "0a942b124ecaa546" },
    };
    unsigned v;

    printf("Michael specification vectors (chained)\n");
    for (v = 0; v < sizeof(Vectors) / sizeof(Vectors[0]); ++v)
    {
        AR9485_MICHAEL_CTX Ctx;
        unsigned char Key[8], Want[8], Got[8];
        char Name[64];
        unsigned i;

        for (i = 0; i < 8; ++i)
        {
            unsigned b;
            sscanf(Vectors[v].Key + 2 * i, "%2x", &b); Key[i] = (unsigned char)b;
            sscanf(Vectors[v].Mic + 2 * i, "%2x", &b); Want[i] = (unsigned char)b;
        }

        AR9485MichaelInit(&Ctx, Key);
        AR9485MichaelUpdate(&Ctx, (const unsigned char *)Vectors[v].Message,
                            (AR9485_TKIP_U32)strlen(Vectors[v].Message));
        AR9485MichaelFinal(&Ctx, Got);

        sprintf(Name, "vector %u (\"%s\")", v, Vectors[v].Message);
        Check(Name, memcmp(Got, Want, 8) == 0);
        if (memcmp(Got, Want, 8) != 0)
        {
            PrintHex("want ", Want, 8);
            PrintHex("got  ", Got, 8);
        }
    }

    /* The streaming form must agree with a single-shot update, whatever the
     * split: the driver feeds the 16-byte pseudo header and the payload as
     * two separate updates, and a payload that is not a multiple of four
     * makes the split matter. */
    {
        static const unsigned char Key[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        unsigned char Data[37], A[8], B[8];
        AR9485_MICHAEL_CTX Ctx;
        unsigned i, split, agree = 1;

        for (i = 0; i < sizeof(Data); ++i) Data[i] = (unsigned char)(i * 7 + 3);

        AR9485MichaelInit(&Ctx, Key);
        AR9485MichaelUpdate(&Ctx, Data, sizeof(Data));
        AR9485MichaelFinal(&Ctx, A);

        for (split = 0; split <= sizeof(Data); ++split)
        {
            AR9485MichaelInit(&Ctx, Key);
            AR9485MichaelUpdate(&Ctx, Data, split);
            AR9485MichaelUpdate(&Ctx, Data + split,
                                (AR9485_TKIP_U32)(sizeof(Data) - split));
            AR9485MichaelFinal(&Ctx, B);
            if (memcmp(A, B, 8) != 0) agree = 0;
        }
        Check("streaming update is split-independent", agree);
    }
}

/* ------------------------------------------------------------------ *
 *  2.  A real AP->STA group frame
 * ------------------------------------------------------------------ */

/*
 * The FRITZ!Box 7330's broadcasts, byte for byte off the monitor capture
 * (scratchpad/ros-assoc.pcap, 2026-08-31):
 *
 *   08 42 00 00                          Data, FromDS, Protected
 *   ff ff ff ff ff ff                    Address1 = DA = broadcast
 *   c8 0e 14 e4 03 bb                    Address2 = BSSID
 *   c8 0e 14 e4 03 b9                    Address3 = SA (the router's LAN MAC)
 *   70 55                                sequence control
 *   01 21 dd a0 00 00 00 00              TKIP IV: TSC1, WEPSeed, TSC0,
 *                                        KeyID 2 + ExtIV, TSC2..TSC5
 *   <36 bytes: SNAP aa aa 03 00 00 00 08 06 + a 28-byte ARP>
 *   <8 bytes Michael MIC>
 *   <4 bytes ICV>
 *                                        = 80 bytes, no FCS (AR_DataLen
 *                                        carries it; the ring drain strips
 *                                        it before the check, as mac80211
 *                                        does for ath9k's RX_INCLUDES_FCS)
 */
static const unsigned char g_Header[24] = {
    0x08, 0x42, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xc8, 0x0e, 0x14, 0xe4, 0x03, 0xbb,
    0xc8, 0x0e, 0x14, 0xe4, 0x03, 0xb9,
    0x70, 0x55
};
static const unsigned char g_Iv[8] = { 0x01, 0x21, 0xdd, 0xa0, 0x00, 0x00, 0x00, 0x00 };

/* SNAP + a broadcast ARP request for 192.168.188.43 from 192.168.188.1. */
static const unsigned char g_Payload[36] = {
    0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x06,
    0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
    0xc8, 0x0e, 0x14, 0xe4, 0x03, 0xb9,
    0xc0, 0xa8, 0xbc, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc0, 0xa8, 0xbc, 0x2b
};

/*
 * The 32-byte GTK exactly as the AP puts it in the msg3 GTK KDE and as
 * rsna_supplicant stores it: TK, then the Michael key the AUTHENTICATOR
 * transmits with, then the one it receives with.  A station receiving from
 * the AP must use the first of the two -- bytes 16..23.
 */
static const unsigned char g_Gtk[32] = {
    /* TK */
    0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
    0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
    /* authenticator TX Michael key = the station's RX key */
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    /* authenticator RX Michael key = the station's TX key */
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00
};

/* Assemble the frame the hardware hands the driver: header, IV, PLAINTEXT
 * payload (the MAC decrypted it by IV key ID even though the key search
 * missed), the sender's Michael MIC, and the ICV. */
static unsigned BuildGroupFrame(unsigned char *Out, const unsigned char *MicKey)
{
    unsigned char Mic[8];
    unsigned Length;

    memcpy(Out, g_Header, 24);
    memcpy(Out + 24, g_Iv, 8);
    memcpy(Out + 32, g_Payload, sizeof(g_Payload));

    /* The AP computes Michael over DA || SA || priority 0 || 0 0 0 || MSDU,
     * with DA = Address1 and SA = Address3 for a FromDS frame. */
    AR9485MichaelMic(MicKey, g_Header + 4, g_Header + 16, 0,
                     g_Payload, (AR9485_TKIP_U32)sizeof(g_Payload), Mic);
    memcpy(Out + 32 + sizeof(g_Payload), Mic, 8);

    Length = 32 + (unsigned)sizeof(g_Payload) + 8;
    Out[Length + 0] = 0xde; Out[Length + 1] = 0xad;   /* ICV: the MAC already */
    Out[Length + 2] = 0xbe; Out[Length + 3] = 0xef;   /* checked it for us    */
    return Length + 4;
}

static void TestGroupFrame(void)
{
    unsigned char Frame[128];
    unsigned char Computed[8];
    unsigned Length;

    printf("AP->STA group TKIP frame\n");

    Length = BuildGroupFrame(Frame, g_Gtk + 16);
    Check("frame is the 80 bytes the air shows", Length == 80);

    Check("verifies with GTK[16..23] (authenticator TX Michael key)",
          AR9485TkipRxCheckMic(Frame, Length, 24, g_Gtk + 16, Computed) ==
          AR9485_TKIPRX_OK);

    Check("rejects GTK[24..31] (the classic swapped-half bug)",
          AR9485TkipRxCheckMic(Frame, Length, 24, g_Gtk + 24, Computed) ==
          AR9485_TKIPRX_MIC_FAIL);

    Check("rejects the TK itself as a Michael key",
          AR9485TkipRxCheckMic(Frame, Length, 24, g_Gtk, Computed) ==
          AR9485_TKIPRX_MIC_FAIL);

    /* A single flipped payload byte must be caught: that is the whole point
     * of running Michael in software on this path. */
    {
        unsigned char Bad[128];
        memcpy(Bad, Frame, Length);
        Bad[40] ^= 0x01;
        Check("rejects a flipped payload byte",
              AR9485TkipRxCheckMic(Bad, Length, 24, g_Gtk + 16, Computed) ==
              AR9485_TKIPRX_MIC_FAIL);
    }

    /* Michael binds the addresses, so a frame relayed with a different SA
     * must fail even though the payload is untouched. */
    {
        unsigned char Bad[128];
        memcpy(Bad, Frame, Length);
        Bad[16] ^= 0x40;                     /* Address3 = SA */
        Check("rejects a rewritten source address",
              AR9485TkipRxCheckMic(Bad, Length, 24, g_Gtk + 16, Computed) ==
              AR9485_TKIPRX_MIC_FAIL);
    }

    /* Taking Address2 for the SA -- the NoDS rule -- must NOT verify a
     * FromDS frame, which is what makes the direction test meaningful. */
    {
        unsigned char Wrong[8];
        AR9485MichaelMic(g_Gtk + 16, g_Header + 4, g_Header + 10, 0,
                         g_Payload, (AR9485_TKIP_U32)sizeof(g_Payload), Wrong);
        Check("FromDS SA is Address3, not Address2 (BSSID)",
              memcmp(Wrong, Frame + 68, 8) != 0);
    }

    /* Trailer arithmetic: the MIC starts at Length - 12, and stripping
     * IV(8) + MIC(8) + ICV(4) leaves the header plus the payload -- the 60
     * bytes nwifi's translator expects (24 + 8 SNAP + 28 ARP). */
    Check("MIC sits at Length - (MIC + ICV)",
          memcmp(Frame + Length - AR9485_TKIP_TRAILER_LEN,
                 Frame + 32 + sizeof(g_Payload), 8) == 0);
    Check("post-strip length is header + payload",
          Length - AR9485_TKIP_IV_LEN - AR9485_TKIP_TRAILER_LEN == 60);

    /* Too short to hold a trailer at all. */
    Check("a frame with no room for IV+MIC+ICV is SHORT",
          AR9485TkipRxCheckMic(Frame, 24 + 8 + 12, 24, g_Gtk + 16, Computed) ==
          AR9485_TKIPRX_SHORT);
}

/* ------------------------------------------------------------------ *
 *  3.  Direction and priority rules
 * ------------------------------------------------------------------ */

static void TestDirectionsAndPriority(void)
{
    unsigned char Frame[128];
    unsigned char Mic[8], Computed[8];
    unsigned Length;

    printf("address and priority selection\n");

    /* ToDS (the station's own broadcast, encrypted with the same GTK):
     * DA = Address3, SA = Address2. */
    memcpy(Frame, g_Header, 24);
    Frame[1] = 0x41;                          /* ToDS + Protected */
    memcpy(Frame + 4,  "\xc8\x0e\x14\xe4\x03\xbb", 6);  /* Address1 = BSSID */
    memcpy(Frame + 10, "\x6c\x71\xd9\x68\x9c\x6d", 6);  /* Address2 = SA    */
    memcpy(Frame + 16, "\xff\xff\xff\xff\xff\xff", 6);  /* Address3 = DA    */
    memcpy(Frame + 24, g_Iv, 8);
    memcpy(Frame + 32, g_Payload, sizeof(g_Payload));
    AR9485MichaelMic(g_Gtk + 24, Frame + 16, Frame + 10, 0,
                     g_Payload, (AR9485_TKIP_U32)sizeof(g_Payload), Mic);
    memcpy(Frame + 32 + sizeof(g_Payload), Mic, 8);
    Length = 32 + (unsigned)sizeof(g_Payload) + 8 + 4;
    Check("ToDS takes DA from Address3 and SA from Address2",
          AR9485TkipRxCheckMic(Frame, Length, 24, g_Gtk + 24, Computed) ==
          AR9485_TKIPRX_OK);

    /* QoS data: a 26-byte header, and the priority octet is the TID. */
    {
        unsigned char Qos[128];
        unsigned QLen;

        memcpy(Qos, g_Header, 24);
        Qos[0] = 0x88;                        /* QoS Data */
        Qos[24] = 0x05;                       /* QoS control: TID 5 */
        Qos[25] = 0x00;
        memcpy(Qos + 26, g_Iv, 8);
        memcpy(Qos + 34, g_Payload, sizeof(g_Payload));
        AR9485MichaelMic(g_Gtk + 16, Qos + 4, Qos + 16, 5,
                         g_Payload, (AR9485_TKIP_U32)sizeof(g_Payload), Mic);
        memcpy(Qos + 34 + sizeof(g_Payload), Mic, 8);
        QLen = 34 + (unsigned)sizeof(g_Payload) + 8 + 4;

        Check("QoS data uses the TID as the Michael priority",
              AR9485TkipRxCheckMic(Qos, QLen, 26, g_Gtk + 16, Computed) ==
              AR9485_TKIPRX_OK);

        /* Priority 0 must not verify a TID-5 frame: that is the injection
         * attack the priority octet exists to stop. */
        AR9485MichaelMic(g_Gtk + 16, Qos + 4, Qos + 16, 0,
                         g_Payload, (AR9485_TKIP_U32)sizeof(g_Payload), Mic);
        Check("priority 0 does not verify a TID-5 frame",
              memcmp(Mic, Qos + 34 + sizeof(g_Payload), 8) != 0);
    }
}

int main(void)
{
    TestMichaelVectors();
    TestGroupFrame();
    TestDirectionsAndPriority();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
