/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Phase 2b - transmit power and the open-loop power-control
 *              calibration apply.  Ports the upstream Linux ath9k chain
 *
 *                  ath9k_hw_apply_txpower()
 *                    -> ath9k_hw_ar9300_set_txpower()   (eep_ops->set_txpower)
 *                         ar9003_hw_get_target_power_eeprom()
 *                         ar9003_hw_set_power_per_rate_table()
 *                         ar9003_hw_tx_power_regwrite()
 *                         ar9003_hw_calibration_apply()
 *                              ar9003_hw_cal_pier_get()
 *                              ar9003_hw_power_control_override()
 *                         ar9003_hw_selfgen_tpc_txpower()
 *
 *              Sources: drivers/net/wireless/ath/ath9k/{ar9003_eeprom.c,
 *              hw.c,eeprom.c,eeprom.h} from Linux 6.12-rc6.  Function
 *              bodies are transcribed verbatim except where a branch is
 *              provably dead for an AR9485 as this driver drives it; every
 *              such trim is called out at the site, with the same
 *              discipline hw_reset.c uses, and the reason is always one of:
 *
 *                - a different chip's SREV arm (AR_SREV_9550/9531/9561/
 *                  9462_20/9340/9330/9565).  AR_SREV_9485() is true and
 *                  mutually exclusive with all of them, so those arms
 *                  cannot execute.  Nothing in this chain has an
 *                  AR_SREV_9485 arm of its own: on this part
 *                  ar9003_hw_power_control_override() reduces to the
 *                  generic else-arms (one AR_PHY_TPC_19 write, no
 *                  per-chain tempSlope, no AR_PHY_TPC_19_B1 write).
 *                - a 5 GHz / HT40 arm.  The AR9485 is a single-band
 *                  2.4 GHz 1x1 part and this driver brings it up HT20
 *                  only, so the IS_CHAN_* predicates in hw_min.h are
 *                  constant-false and the arms fold at compile time.
 *                  The HT40 entries of the target-power array are
 *                  therefore left at their memset(0) value and written
 *                  as zeroes to the HT40 rate registers - which is
 *                  exactly what upstream does on an HT20 channel.
 *                - chains 1 and 2.  ah->caps.tx_chainmask is 1, so every
 *                  `if (tx_chainmask & BIT(1|2))' arm is dead and the
 *                  per-chain arrays collapse to a scalar for chain 0.
 *                - MCI / bluetooth-coexistence: ath9k_hw_mci_is_enabled()
 *                  is false (see hw_reset.c's header).
 *                - PAPRD.  ar9003_is_paprd_enabled() is false for this
 *                  driver and stays false: the PAPRD training path is not
 *                  ported.  ar9003_paprd_set_txpower() only latches
 *                  ah->paprd_target_power and writes no register, so
 *                  dropping it changes nothing the hardware sees.
 *
 *              REGULATORY.  ReactOS has no regulatory-domain database and
 *              this driver has no ath_regulatory / ath9k_regd_get_ctl(),
 *              so the conformance-test-limit (CTL) machinery upstream
 *              runs in ar9003_hw_set_power_per_rate_table() has no input
 *              here.  Note that with no regdomain upstream itself would
 *              pass cfgCtl == NO_CTL (0xff), which matches no ctlIndex[]
 *              entry in the EEPROM, leaving twiceMaxEdgePower at
 *              MAX_RATE_POWER for every ctl mode; the CTL walk is a
 *              no-op in that configuration, which is why it is not
 *              carried.  What is carried is the scaled-power clamp, fed
 *              with a fixed limit of MAX_RATE_POWER (63 = 31.5 dBm in the
 *              hardware's half-dB units).  That sits above every 2.4 GHz
 *              conducted ceiling (FCC 30 dBm, ETSI 20 dBm EIRP) and above
 *              every target power an AR9485 EEPROM carries, so the clamp
 *              never binds and THE EEPROM TARGET POWERS ARE THE LIMIT.
 *              This is the deliberate assumption: the card is trusted to
 *              have been calibrated for its market.  When a regdomain
 *              exists, the fix is to feed the real limit into
 *              AR9485_TXPOWER_LIMIT_2X and restore the CTL walk.
 *
 *              Deliberately NOT ported:
 *
 *                - ar9003_hw_init_rate_txpower() and the AR_PHY_PWRTX_MAX
 *                  enable.  ah->tpc_enabled is false upstream by default
 *                  (hw.c:474) and there is no debugfs here to flip it, so
 *                  only the else-arm applies and TPC is explicitly
 *                  disabled by writing AR_PHY_PWRTX_MAX = 0.
 *                - storing the interpolated noise floor in ah->nf_2g.
 *                  Its only consumer is the NF/ANI path, which is not
 *                  ported (see hw_reset.c's header).  The values are
 *                  still computed and logged.
 *                - ath9k_hw_update_regulatory_maxpower().  Its whole body
 *                  is a switch on ar5416_get_ntxchains(ah->txchainmask);
 *                  the 1-chain arm is `break', i.e. a no-op.  The
 *                  max_power_level it would adjust is computed below for
 *                  the log line and has no other consumer here.
 */

#include "hw_min.h"

#define NDEBUG
#include <debug.h>

/* --------------------------------------------------------------------
 *  Constants the imported bodies need, from headers this port does not
 *  carry whole.
 * -------------------------------------------------------------------- */

/* Verbatim eeprom.h:161 - the "no such pier" frequency-bin sentinel. */
#define AR5416_BCHAN_UNUSED             0xFF

/*
 * The transmit power limit handed to ar9003_hw_set_power_per_rate_table().
 * See the REGULATORY note in the file header: with no regdomain code this
 * is deliberately the hardware maximum, so the EEPROM target powers are
 * what actually limits the transmitter.
 */
#define AR9485_TXPOWER_LIMIT_2X         MAX_RATE_POWER

/* --------------------------------------------------------------------
 *  VERBATIM from eeprom.h:715 (ath9k_hw_fbin2freq).
 *
 *  The EEPROM stores pier and target-power frequencies as one byte, an
 *  offset from 2300 MHz in the 2.4 GHz band.  is2GHz is always true for
 *  this part; the parameter is kept so the call sites read as upstream.
 * -------------------------------------------------------------------- */

static u16 ath9k_hw_fbin2freq(u8 fbin, bool is2GHz)
{
    if (fbin == AR5416_BCHAN_UNUSED)
        return fbin;

    return (u16) ((is2GHz) ? (2300 + fbin) : (4800 + 5 * fbin));
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:2963 (interpolate).
 * -------------------------------------------------------------------- */

static int interpolate(int x, int xa, int xb, int ya, int yb)
{
    int bf, factor, plus;

    bf = 2 * (yb - ya) * (x - xa) / (xb - xa);
    factor = bf / 2;
    plus = bf % 2;
    return ya + factor + plus;
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:4269 (ar9003_hw_power_interpolate).
 *
 *  Returns the interpolated y value corresponding to the specified x
 *  value from the np ordered pairs of data (px,py).  The pairs do not
 *  have to be in any order.  If the specified x value is less than any
 *  of the px, the returned y value is equal to the py for the lowest px.
 *  If the specified x value is greater than any of the px, the returned
 *  y value is equal to the py for the highest px.
 * -------------------------------------------------------------------- */

static int ar9003_hw_power_interpolate(s32 x, s32 *px, s32 *py, u16 np)
{
    int ip = 0;
    int lx = 0, ly = 0, lhave = 0;
    int hx = 0, hy = 0, hhave = 0;
    int dx = 0;
    int y = 0;

    lhave = 0;
    hhave = 0;

    /* identify best lower and higher x calibration measurement */
    for (ip = 0; ip < np; ip++) {
        dx = x - px[ip];

        /* this measurement is higher than our desired x */
        if (dx <= 0) {
            if (!hhave || dx > (x - hx)) {
                /* new best higher x measurement */
                hx = px[ip];
                hy = py[ip];
                hhave = 1;
            }
        }
        /* this measurement is lower than our desired x */
        if (dx >= 0) {
            if (!lhave || dx < (x - lx)) {
                /* new best lower x measurement */
                lx = px[ip];
                ly = py[ip];
                lhave = 1;
            }
        }
    }

    /* the low x is good */
    if (lhave) {
        /* so is the high x */
        if (hhave) {
            /* they're the same, so just pick one */
            if (hx == lx)
                y = ly;
            else    /* interpolate  */
                y = interpolate(x, lx, hx, ly, hy);
        } else      /* only low is good, use it */
            y = ly;
    } else if (hhave) /* only high is good, use it */
        y = hy;
    else /* nothing is good, this should never happen unless np=0, ???? */
        y = -(1 << 30);
    return y;
}

/* --------------------------------------------------------------------
 *  Target powers out of the EEPROM.
 *
 *  ADAPTED from ar9003_eeprom.c:4323, :4358, :4430.  The 5 GHz arm of
 *  each getter is dropped (single-band part), which also lets the
 *  scratch arrays shrink from AR9300_NUM_5G_20_TARGET_POWERS (8) to the
 *  2 GHz pier count they are actually filled to.  The HT40 getter
 *  (ar9003_eeprom.c:4394) is not ported at all: HT40 is not brought up,
 *  so its callers below are gone.
 * -------------------------------------------------------------------- */

static u8 ar9003_hw_eeprom_get_tgt_pwr(struct ath_hw *ah,
                                       u16 rateIndex, u16 freq, bool is2GHz)
{
    u16 numPiers = AR9300_NUM_2G_20_TARGET_POWERS, i;
    s32 targetPowerArray[AR9300_NUM_2G_20_TARGET_POWERS];
    s32 freqArray[AR9300_NUM_2G_20_TARGET_POWERS];
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    struct cal_tgt_pow_legacy *pEepromTargetPwr = eep->calTargetPower2G;
    u8 *pFreqBin = eep->calTarget_freqbin_2G;

    UNREFERENCED_PARAMETER(is2GHz);

    /*
     * create array of channels and targetpower from
     * targetpower piers stored on eeprom
     */
    for (i = 0; i < numPiers; i++) {
        freqArray[i] = ath9k_hw_fbin2freq(pFreqBin[i], 1);
        targetPowerArray[i] = pEepromTargetPwr[i].tPow2x[rateIndex];
    }

    /* interpolate to get target power for given frequency */
    return (u8) ar9003_hw_power_interpolate((s32) freq,
                                            freqArray,
                                            targetPowerArray, numPiers);
}

static u8 ar9003_hw_eeprom_get_ht20_tgt_pwr(struct ath_hw *ah,
                                            u16 rateIndex,
                                            u16 freq, bool is2GHz)
{
    u16 numPiers = AR9300_NUM_2G_20_TARGET_POWERS, i;
    s32 targetPowerArray[AR9300_NUM_2G_20_TARGET_POWERS];
    s32 freqArray[AR9300_NUM_2G_20_TARGET_POWERS];
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    struct cal_tgt_pow_ht *pEepromTargetPwr = eep->calTargetPower2GHT20;
    u8 *pFreqBin = eep->calTarget_freqbin_2GHT20;

    UNREFERENCED_PARAMETER(is2GHz);

    /*
     * create array of channels and targetpower
     * from targetpower piers stored on eeprom
     */
    for (i = 0; i < numPiers; i++) {
        freqArray[i] = ath9k_hw_fbin2freq(pFreqBin[i], 1);
        targetPowerArray[i] = pEepromTargetPwr[i].tPow2x[rateIndex];
    }

    /* interpolate to get target power for given frequency */
    return (u8) ar9003_hw_power_interpolate((s32) freq,
                                            freqArray,
                                            targetPowerArray, numPiers);
}

static u8 ar9003_hw_eeprom_get_cck_tgt_pwr(struct ath_hw *ah,
                                           u16 rateIndex, u16 freq)
{
    u16 numPiers = AR9300_NUM_2G_CCK_TARGET_POWERS, i;
    s32 targetPowerArray[AR9300_NUM_2G_CCK_TARGET_POWERS];
    s32 freqArray[AR9300_NUM_2G_CCK_TARGET_POWERS];
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    struct cal_tgt_pow_legacy *pEepromTargetPwr = eep->calTargetPowerCck;
    u8 *pFreqBin = eep->calTarget_freqbin_Cck;

    /*
     * create array of channels and targetpower from
     * targetpower piers stored on eeprom
     */
    for (i = 0; i < numPiers; i++) {
        freqArray[i] = ath9k_hw_fbin2freq(pFreqBin[i], 1);
        targetPowerArray[i] = pEepromTargetPwr[i].tPow2x[rateIndex];
    }

    /* interpolate to get target power for given frequency */
    return (u8) ar9003_hw_power_interpolate((s32) freq,
                                            freqArray,
                                            targetPowerArray, numPiers);
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:4592, :4610, :4625 - fan the getters
 *  out over the rate groups.  ar9003_hw_get_ht40_target_powers()
 *  (ar9003_eeprom.c:4671) is not ported; see the file header.
 * -------------------------------------------------------------------- */

static void ar9003_hw_get_legacy_target_powers(struct ath_hw *ah, u16 freq,
                                               u8 *targetPowerValT2,
                                               bool is2GHz)
{
    targetPowerValT2[ALL_TARGET_LEGACY_6_24] =
        ar9003_hw_eeprom_get_tgt_pwr(ah, LEGACY_TARGET_RATE_6_24, freq,
                                     is2GHz);
    targetPowerValT2[ALL_TARGET_LEGACY_36] =
        ar9003_hw_eeprom_get_tgt_pwr(ah, LEGACY_TARGET_RATE_36, freq,
                                     is2GHz);
    targetPowerValT2[ALL_TARGET_LEGACY_48] =
        ar9003_hw_eeprom_get_tgt_pwr(ah, LEGACY_TARGET_RATE_48, freq,
                                     is2GHz);
    targetPowerValT2[ALL_TARGET_LEGACY_54] =
        ar9003_hw_eeprom_get_tgt_pwr(ah, LEGACY_TARGET_RATE_54, freq,
                                     is2GHz);
}

static void ar9003_hw_get_cck_target_powers(struct ath_hw *ah, u16 freq,
                                            u8 *targetPowerValT2)
{
    targetPowerValT2[ALL_TARGET_LEGACY_1L_5L] =
        ar9003_hw_eeprom_get_cck_tgt_pwr(ah, LEGACY_TARGET_RATE_1L_5L,
                                         freq);
    targetPowerValT2[ALL_TARGET_LEGACY_5S] =
        ar9003_hw_eeprom_get_cck_tgt_pwr(ah, LEGACY_TARGET_RATE_5S, freq);
    targetPowerValT2[ALL_TARGET_LEGACY_11L] =
        ar9003_hw_eeprom_get_cck_tgt_pwr(ah, LEGACY_TARGET_RATE_11L, freq);
    targetPowerValT2[ALL_TARGET_LEGACY_11S] =
        ar9003_hw_eeprom_get_cck_tgt_pwr(ah, LEGACY_TARGET_RATE_11S, freq);
}

static void ar9003_hw_get_ht20_target_powers(struct ath_hw *ah, u16 freq,
                                             u8 *targetPowerValT2, bool is2GHz)
{
    targetPowerValT2[ALL_TARGET_HT20_0_8_16] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_0_8_16, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_1_3_9_11_17_19] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_1_3_9_11_17_19,
                                          freq, is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_4] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_4, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_5] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_5, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_6] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_6, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_7] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_7, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_12] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_12, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_13] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_13, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_14] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_14, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_15] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_15, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_20] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_20, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_21] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_21, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_22] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_22, freq,
                                          is2GHz);
    targetPowerValT2[ALL_TARGET_HT20_23] =
        ar9003_hw_eeprom_get_ht20_tgt_pwr(ah, HT_TARGET_RATE_23, freq,
                                          is2GHz);
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_eeprom.c:4724 (ar9003_hw_get_target_power_eeprom).
 *
 *  is2GHz is constant-true and IS_CHAN_HT40() constant-false, so the CCK
 *  and legacy/HT20 fills always run and the HT40 fill never does.
 * -------------------------------------------------------------------- */

static void ar9003_hw_get_target_power_eeprom(struct ath_hw *ah,
                                              struct ath9k_channel *chan,
                                              u8 *targetPowerValT2)
{
    bool is2GHz = IS_CHAN_2GHZ(chan);
    u16 freq = chan->channel;

    ar9003_hw_get_cck_target_powers(ah, freq, targetPowerValT2);
    ar9003_hw_get_legacy_target_powers(ah, freq, targetPowerValT2, is2GHz);
    ar9003_hw_get_ht20_target_powers(ah, freq, targetPowerValT2, is2GHz);
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_eeprom.c:4749 (ar9003_hw_cal_pier_get).
 *
 *  The 5 GHz arm is dropped.  The ichain bound check is kept: it is the
 *  guard that makes the chain loop in ar9003_hw_calibration_apply()
 *  safe, and it costs one comparison on a cold path.
 * -------------------------------------------------------------------- */

static int ar9003_hw_cal_pier_get(struct ath_hw *ah,
                                  bool is2ghz,
                                  int ipier,
                                  int ichain,
                                  int *pfrequency,
                                  int *pcorrection,
                                  int *ptemperature, int *pvoltage,
                                  int *pnf_cal, int *pnf_power)
{
    u8 *pCalPier;
    struct ar9300_cal_data_per_freq_op_loop *pCalPierStruct;
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;

    if (ichain >= AR9300_MAX_CHAINS) {
        DPRINT1("AR9485: invalid chain index, must be less than %d\n",
                AR9300_MAX_CHAINS);
        return -1;
    }

    if (ipier >= AR9300_NUM_2G_CAL_PIERS) {
        DPRINT1("AR9485: invalid 2GHz cal pier index, must be less than %d\n",
                AR9300_NUM_2G_CAL_PIERS);
        return -1;
    }

    pCalPier = &(eep->calFreqPier2G[ipier]);
    pCalPierStruct = &(eep->calPierData2G[ichain][ipier]);

    *pfrequency = ath9k_hw_fbin2freq(*pCalPier, is2ghz);
    *pcorrection = pCalPierStruct->refPower;
    *ptemperature = pCalPierStruct->tempMeas;
    *pvoltage = pCalPierStruct->voltMeas;
    *pnf_cal = pCalPierStruct->rxTempMeas ?
            N2DBM(pCalPierStruct->rxNoisefloorCal) : 0;
    *pnf_power = pCalPierStruct->rxTempMeas ?
            N2DBM(pCalPierStruct->rxNoisefloorPower) : 0;

    return 0;
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_eeprom.c:4803 (ar9003_hw_power_control_override).
 *
 *  This is where the open-loop power control actually gets turned on:
 *  without it the PHY runs the transmit gain table with a zero gain
 *  delta and no thermal compensation, which is the state boot 77 and its
 *  neighbours measured on the metal.
 *
 *  Folded away, in upstream order:
 *    - the chain 1 / chain 2 AR_PHY_TPC_11_B1/B2 and AR_PHY_TPC_6_B1/B2
 *      writes: ah->caps.tx_chainmask is 1, both guards are false.  The
 *      per-chain correction[]/temperature[] arrays therefore collapse to
 *      the chain-0 scalars this signature takes.
 *    - `frequency >= 4000': 5 GHz, unreachable.  temp_slope is always
 *      modalHeader2G.tempSlope, so the AR9550 five-pier interpolation,
 *      the miscConfiguration & 0x20 eight-pier arm, the base_ext2
 *      tempSlopeLow/High arm and the 5 GHz fallback all fold out along
 *      with the local f[]/t[]/t1[]/t2[] scratch arrays.
 *    - the AR9550/9531/9561 per-chain-tempSlope block at the `tempslope'
 *      label: mutually exclusive with AR_SREV_9485, so only the plain
 *      else-arm AR_PHY_TPC_19 write survives.
 *    - the AR_SREV_9462_20_OR_LATER AR_PHY_TPC_19_B1 write: not this part.
 *
 *  `voltage' is a parameter upstream computes, passes and never reads;
 *  it is kept in the signature so the shape matches, and logged by the
 *  caller so a boot log can show what the pier interpolation produced.
 * -------------------------------------------------------------------- */

static void ar9003_hw_power_control_override(struct ath_hw *ah,
                                             int frequency,
                                             int correction,
                                             int voltage, int temperature)
{
    int temp_slope;
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;

    UNREFERENCED_PARAMETER(frequency);
    UNREFERENCED_PARAMETER(voltage);

    REG_RMW(ah, AR_PHY_TPC_11_B0,
            (correction << AR_PHY_TPC_OLPC_GAIN_DELTA_S),
            AR_PHY_TPC_OLPC_GAIN_DELTA);

    /* enable open loop power control on chip */
    REG_RMW(ah, AR_PHY_TPC_6_B0,
            (3 << AR_PHY_TPC_6_ERROR_EST_MODE_S),
            AR_PHY_TPC_6_ERROR_EST_MODE);

    /*
     * enable temperature compensation
     * Need to use register names
     */
    temp_slope = eep->modalHeader2G.tempSlope;

    REG_RMW_FIELD(ah, AR_PHY_TPC_19,
                  AR_PHY_TPC_19_ALPHA_THERM, temp_slope);

    REG_RMW_FIELD(ah, AR_PHY_TPC_18, AR_PHY_TPC_18_THERM_CAL_VALUE,
                  temperature);

    DPRINT1("AR9485: txpower olpc gain delta=%d temp_slope=%d therm_cal=%d "
            "(AR_PHY_TPC_11_B0=0x%08x AR_PHY_TPC_6_B0=0x%08x "
            "AR_PHY_TPC_19=0x%08x AR_PHY_TPC_18=0x%08x)\n",
            correction, temp_slope, temperature,
            REG_READ(ah, AR_PHY_TPC_11_B0),
            REG_READ(ah, AR_PHY_TPC_6_B0),
            REG_READ(ah, AR_PHY_TPC_19),
            REG_READ(ah, AR_PHY_TPC_18));
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_eeprom.c:4957 (ar9003_hw_calibration_apply).
 *
 *  Apply the recorded correction values: pick the best cal pier below
 *  and above the operating frequency and interpolate between them.
 *
 *  Folded away:
 *    - the outer `for (ichain = 0; ichain < AR9300_MAX_CHAINS; ichain++)'
 *      loops.  Only chain 0 has calibration data on a 1x1 part and only
 *      correction[0]/temperature[0] are ever consumed (see
 *      ar9003_hw_power_control_override above), so every per-chain array
 *      becomes a scalar.
 *    - the 5 GHz pier count.
 *    - storing nf_cal / nf_pwr into ah->nf_2g: no consumer here, see the
 *      file header.  Still computed, and logged.
 * -------------------------------------------------------------------- */

static int ar9003_hw_calibration_apply(struct ath_hw *ah, int frequency)
{
    int ipier, npier;
    int lfrequency, lcorrection, ltemperature, lvoltage, lnf_cal, lnf_pwr;
    int hfrequency, hcorrection, htemperature, hvoltage, hnf_cal, hnf_pwr;
    int fdiff;
    int correction, voltage, temperature, nf_cal, nf_pwr;
    int pfrequency, pcorrection, ptemperature, pvoltage, pnf_cal, pnf_pwr;
    bool is2ghz = frequency < 4000;

    npier = AR9300_NUM_2G_CAL_PIERS;

    lfrequency = 0;
    hfrequency = 100000;
    lcorrection = ltemperature = lvoltage = lnf_cal = lnf_pwr = 0;
    hcorrection = htemperature = hvoltage = hnf_cal = hnf_pwr = 0;

    /* identify best lower and higher frequency calibration measurement */
    for (ipier = 0; ipier < npier; ipier++) {
        if (!ar9003_hw_cal_pier_get(ah, is2ghz, ipier, 0,
                                    &pfrequency, &pcorrection,
                                    &ptemperature, &pvoltage,
                                    &pnf_cal, &pnf_pwr)) {
            fdiff = frequency - pfrequency;

            /*
             * this measurement is higher than
             * our desired frequency
             */
            if (fdiff <= 0) {
                if (hfrequency <= 0 || hfrequency >= 100000 ||
                    fdiff > (frequency - hfrequency)) {
                    /* new best higher frequency measurement */
                    hfrequency = pfrequency;
                    hcorrection = pcorrection;
                    htemperature = ptemperature;
                    hvoltage = pvoltage;
                    hnf_cal = pnf_cal;
                    hnf_pwr = pnf_pwr;
                }
            }
            if (fdiff >= 0) {
                if (lfrequency <= 0 ||
                    fdiff < (frequency - lfrequency)) {
                    /* new best lower frequency measurement */
                    lfrequency = pfrequency;
                    lcorrection = pcorrection;
                    ltemperature = ptemperature;
                    lvoltage = pvoltage;
                    lnf_cal = pnf_cal;
                    lnf_pwr = pnf_pwr;
                }
            }
        }
    }

    DPRINT1("AR9485: cal pier ch0 f=%d low=%d %d high=%d %d nf=%d %d "
            "pwr=%d %d\n",
            frequency, lfrequency, lcorrection, hfrequency, hcorrection,
            lnf_cal, hnf_cal, lnf_pwr, hnf_pwr);

    /* interpolate */
    /* they're the same, so just pick one */
    if (hfrequency == lfrequency) {
        correction = lcorrection;
        voltage = lvoltage;
        temperature = ltemperature;
        nf_cal = lnf_cal;
        nf_pwr = lnf_pwr;
    }
    /* the low frequency is good */
    else if (frequency - lfrequency < 1000) {
        /* so is the high frequency, interpolate */
        if (hfrequency - frequency < 1000) {

            correction = interpolate(frequency, lfrequency, hfrequency,
                                     lcorrection, hcorrection);

            temperature = interpolate(frequency, lfrequency, hfrequency,
                                      ltemperature, htemperature);

            voltage = interpolate(frequency, lfrequency, hfrequency,
                                  lvoltage, hvoltage);

            nf_cal = interpolate(frequency, lfrequency, hfrequency,
                                 lnf_cal, hnf_cal);

            nf_pwr = interpolate(frequency, lfrequency, hfrequency,
                                 lnf_pwr, hnf_pwr);
        }
        /* only low is good, use it */
        else {
            correction = lcorrection;
            temperature = ltemperature;
            voltage = lvoltage;
            nf_cal = lnf_cal;
            nf_pwr = lnf_pwr;
        }
    }
    /* only high is good, use it */
    else if (hfrequency - frequency < 1000) {
        correction = hcorrection;
        temperature = htemperature;
        voltage = hvoltage;
        nf_cal = hnf_cal;
        nf_pwr = hnf_pwr;
    } else {    /* nothing is good, presume 0???? */
        correction = 0;
        temperature = 0;
        voltage = 0;
        nf_cal = 0;
        nf_pwr = 0;
    }

    DPRINT1("AR9485: cal apply f=%d correction=%d temperature=%d voltage=%d "
            "nf_cal=%d nf_pwr=%d\n",
            frequency, correction, temperature, voltage, nf_cal, nf_pwr);

    ar9003_hw_power_control_override(ah, frequency, correction, voltage,
                                     temperature);

    return 0;
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:4475 (ar9003_hw_tx_power_regwrite).
 *
 *  Set tx power registers to array of values passed in.  Every write is
 *  carried, the HT40 ones included: on an HT20 channel upstream leaves
 *  those array entries at zero and writes the zeroes, and reproducing
 *  that exactly is what keeps a stale HT40 power out of the registers
 *  after a channel change.
 * -------------------------------------------------------------------- */

static int ar9003_hw_tx_power_regwrite(struct ath_hw *ah, u8 *pPwrArray)
{
#define POW_SM(_r, _s)     (((_r) & 0x3f) << (_s))
    /* make sure forced gain is not set */
    REG_WRITE(ah, AR_PHY_TX_FORCED_GAIN, 0);

    /* Write the OFDM power per rate set */

    /* 6 (LSB), 9, 12, 18 (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(0),
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_6_24], 24) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_6_24], 16) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_6_24], 8) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_6_24], 0));

    /* 24 (LSB), 36, 48, 54 (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(1),
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_54], 24) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_48], 16) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_36], 8) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_6_24], 0));

    /* Write the CCK power per rate set */

    /* 1L (LSB), reserved, 2L, 2S (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(2),
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_1L_5L], 24) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_1L_5L], 16) |
              /* POW_SM(txPowerTimes2,  8) | this is reserved for AR9003 */
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_1L_5L], 0));

    /* 5.5L (LSB), 5.5S, 11L, 11S (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(3),
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_11S], 24) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_11L], 16) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_5S], 8) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_1L_5L], 0)
        );

    /* Write the power for duplicated frames - HT40 */

    /* dup40_cck (LSB), dup40_ofdm, ext20_cck, ext20_ofdm (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(8),
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_6_24], 24) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_1L_5L], 16) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_6_24],  8) |
              POW_SM(pPwrArray[ALL_TARGET_LEGACY_1L_5L],  0)
        );

    /* Write the HT20 power per rate set */

    /* 0/8/16 (LSB), 1-3/9-11/17-19, 4, 5 (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(4),
              POW_SM(pPwrArray[ALL_TARGET_HT20_5], 24) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_4], 16) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_1_3_9_11_17_19], 8) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_0_8_16], 0)
        );

    /* 6 (LSB), 7, 12, 13 (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(5),
              POW_SM(pPwrArray[ALL_TARGET_HT20_13], 24) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_12], 16) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_7], 8) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_6], 0)
        );

    /* 14 (LSB), 15, 20, 21 */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(9),
              POW_SM(pPwrArray[ALL_TARGET_HT20_21], 24) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_20], 16) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_15], 8) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_14], 0)
        );

    /* Mixed HT20 and HT40 rates */

    /* HT20 22 (LSB), HT20 23, HT40 22, HT40 23 (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(10),
              POW_SM(pPwrArray[ALL_TARGET_HT40_23], 24) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_22], 16) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_23], 8) |
              POW_SM(pPwrArray[ALL_TARGET_HT20_22], 0)
        );

    /*
     * Write the HT40 power per rate set
     * correct PAR difference between HT40 and HT20/LEGACY
     * 0/8/16 (LSB), 1-3/9-11/17-19, 4, 5 (MSB)
     */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(6),
              POW_SM(pPwrArray[ALL_TARGET_HT40_5], 24) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_4], 16) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_1_3_9_11_17_19], 8) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_0_8_16], 0)
        );

    /* 6 (LSB), 7, 12, 13 (MSB) */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(7),
              POW_SM(pPwrArray[ALL_TARGET_HT40_13], 24) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_12], 16) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_7], 8) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_6], 0)
        );

    /* 14 (LSB), 15, 20, 21 */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(11),
              POW_SM(pPwrArray[ALL_TARGET_HT40_21], 24) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_20], 16) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_15], 8) |
              POW_SM(pPwrArray[ALL_TARGET_HT40_14], 0)
        );

    return 0;
#undef POW_SM
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_eeprom.c:4455 (ar9003_hw_selfgen_tpc_txpower).
 *
 *  Target power for the frames the MAC generates by itself - ACK and
 *  RTS/CTS.  These never go through a transmit descriptor, so this
 *  single register is the only thing that sets their power; leaving it
 *  at the initvals default is how a card can transmit data frames at a
 *  sane power and still answer nobody.  The 5 GHz arm is dropped.
 * -------------------------------------------------------------------- */

static void ar9003_hw_selfgen_tpc_txpower(struct ath_hw *ah,
                                          struct ath9k_channel *chan,
                                          u8 *pwr_array)
{
    u32 val;

    UNREFERENCED_PARAMETER(chan);

    /* target power values for self generated frames (ACK,RTS/CTS) */
    val = SM(pwr_array[ALL_TARGET_LEGACY_1L_5L], AR_TPC_ACK) |
          SM(pwr_array[ALL_TARGET_LEGACY_1L_5L], AR_TPC_CTS) |
          SM(0x3f, AR_TPC_CHIRP) | SM(0x3f, AR_TPC_RPT);

    REG_WRITE(ah, AR_TPC, val);
}

/* --------------------------------------------------------------------
 *  ADAPTED from eeprom.c:416 (ath9k_hw_get_scaled_power).
 *
 *  The two- and three-chain corrections fold out: ah->txchainmask is 1,
 *  so ar5416_get_ntxchains() is 1 and the switch takes its `break' arm.
 * -------------------------------------------------------------------- */

static u16 ath9k_hw_get_scaled_power(struct ath_hw *ah, u16 power_limit,
                                     u8 antenna_reduction)
{
    u16 reduction = antenna_reduction;

    UNREFERENCED_PARAMETER(ah);

    if (power_limit > reduction)
        power_limit -= reduction;
    else
        power_limit = 0;

    return min_t(u16, power_limit, MAX_RATE_POWER);
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_eeprom.c:5225 (ar9003_hw_set_power_per_rate_table).
 *
 *  Upstream walks the applicable CTL (conformance test limit) modes for
 *  the band - {CTL_11B, CTL_11G, CTL_2GHT20} on an HT20 2.4 GHz channel
 *  - and for each one takes the minimum of the EEPROM's regulatory edge
 *  powers and the scaled power, then clamps that mode's rate group.
 *
 *  Here there is no regdomain and hence no cfgCtl (see the REGULATORY
 *  note in the file header): twiceMaxEdgePower would stay at
 *  MAX_RATE_POWER for all three modes, so minCtlPower is just
 *  scaledPower and all three loops clamp against the same number.  The
 *  three loops are therefore collapsed into one range,
 *  ALL_TARGET_LEGACY_6_24 .. ALL_TARGET_HT20_23, which is exactly the
 *  union the three CTL modes cover on an HT20 2.4 GHz channel.  The
 *  HT40 group is untouched, as it is upstream when HT40 is off.
 *
 *  Also folded: ath9k_hw_get_channel_centers().  With HT40 off it sets
 *  ctl_center == ext_center == synth_center == chan->channel, and the
 *  only consumer was the dropped edge-power lookup.
 *  ath9k_hw_mci_is_enabled() is false, so the MCI power cap inside the
 *  HT20/HT40 arms is dead.
 * -------------------------------------------------------------------- */

static void ar9003_hw_set_power_per_rate_table(struct ath_hw *ah,
                                               struct ath9k_channel *chan,
                                               u8 *pPwrArray,
                                               u8 antenna_reduction,
                                               u16 powerLimit)
{
    u16 scaledPower, minCtlPower;
    int i;

    UNREFERENCED_PARAMETER(chan);

    scaledPower = ath9k_hw_get_scaled_power(ah, powerLimit,
                                            antenna_reduction);

    minCtlPower = min_t(u16, MAX_RATE_POWER, scaledPower);

    DPRINT1("AR9485: txpower limit 2x=%u antenna_reduction=%u "
            "scaledPower=%u minCtlPower=%u (no regdomain: EEPROM targets "
            "are the limit)\n",
            powerLimit, antenna_reduction, scaledPower, minCtlPower);

    for (i = ALL_TARGET_LEGACY_6_24; i <= ALL_TARGET_HT20_23; i++)
        pPwrArray[i] = (u8)min((u16)pPwrArray[i], minCtlPower);
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_eeprom.c:5445 (ath9k_hw_ar9300_set_txpower).
 *
 *  Folded away:
 *    - every ar9003_is_paprd_enabled() arm, and ar9003_paprd_set_txpower():
 *      PAPRD is off and stays off; see the file header.  This also drops
 *      the target_power_val_t2_eep / paprd_scale_factor / min_pwridx
 *      bookkeeping and mcsidx_to_tgtpwridx().
 *    - the `test' parameter: the only caller that passes true is
 *      ath9k_hw_set_txpowerlimit(..., test=true) from the tx99 debugfs
 *      hook, which does not exist here.
 *    - ah->tpc_enabled: false, so only the else-arm runs and TPC is
 *      disabled.  targetPowerValT2_tpc and ar9003_hw_init_rate_txpower()
 *      go with it.
 *    - ath9k_hw_update_regulatory_maxpower(): a no-op at one chain.
 *      regulatory->max_power_level has no consumer in this driver either;
 *      it is still computed, for the log line.
 * -------------------------------------------------------------------- */

static void ath9k_hw_ar9300_set_txpower(struct ath_hw *ah,
                                        struct ath9k_channel *chan,
                                        u8 twiceAntennaReduction,
                                        u16 powerLimit)
{
    u8 targetPowerValT2[ar9300RateSize];
    unsigned int i;
    u16 max_power_level = 0;

    RtlZeroMemory(targetPowerValT2, sizeof(targetPowerValT2));

    /*
     * Get target powers from EEPROM - our baseline for TX Power
     */
    ar9003_hw_get_target_power_eeprom(ah, chan, targetPowerValT2);

    DPRINT1("AR9485: EEPROM target 2x powers ch=%u: CCK 1L/5L=%u 11L=%u "
            "11S=%u  OFDM 6-24=%u 36=%u 48=%u 54=%u  HT20 mcs0=%u mcs7=%u\n",
            chan->channel,
            targetPowerValT2[ALL_TARGET_LEGACY_1L_5L],
            targetPowerValT2[ALL_TARGET_LEGACY_11L],
            targetPowerValT2[ALL_TARGET_LEGACY_11S],
            targetPowerValT2[ALL_TARGET_LEGACY_6_24],
            targetPowerValT2[ALL_TARGET_LEGACY_36],
            targetPowerValT2[ALL_TARGET_LEGACY_48],
            targetPowerValT2[ALL_TARGET_LEGACY_54],
            targetPowerValT2[ALL_TARGET_HT20_0_8_16],
            targetPowerValT2[ALL_TARGET_HT20_7]);

    ar9003_hw_set_power_per_rate_table(ah, chan, targetPowerValT2,
                                       twiceAntennaReduction, powerLimit);

    for (i = 0; i < ar9300RateSize; i++) {
        if (targetPowerValT2[i] > max_power_level)
            max_power_level = targetPowerValT2[i];
    }

    /*
     * The two numbers the next boot log has to show: what the CCK and
     * the OFDM transmitter are finally told to do, in quarter-dBm-free
     * half-dB units (value / 2 == dBm).
     */
    DPRINT1("AR9485: FINAL 2x target powers: CCK 1M=%u (%u dBm) "
            "11M=%u (%u dBm)  OFDM 6M=%u (%u dBm) 24M=%u (%u dBm)  "
            "max=%u (%u dBm)\n",
            targetPowerValT2[ALL_TARGET_LEGACY_1L_5L],
            targetPowerValT2[ALL_TARGET_LEGACY_1L_5L] / 2,
            targetPowerValT2[ALL_TARGET_LEGACY_11L],
            targetPowerValT2[ALL_TARGET_LEGACY_11L] / 2,
            targetPowerValT2[ALL_TARGET_LEGACY_6_24],
            targetPowerValT2[ALL_TARGET_LEGACY_6_24] / 2,
            targetPowerValT2[ALL_TARGET_LEGACY_6_24],
            targetPowerValT2[ALL_TARGET_LEGACY_6_24] / 2,
            max_power_level, max_power_level / 2);

    /* Write target power array to registers */
    ar9003_hw_tx_power_regwrite(ah, targetPowerValT2);
    ar9003_hw_calibration_apply(ah, chan->channel);

    ar9003_hw_selfgen_tpc_txpower(ah, chan, targetPowerValT2);

    /* Disable TPC (ah->tpc_enabled is false; see the header). */
    REG_WRITE(ah, AR_PHY_PWRTX_MAX, 0);

    DPRINT1("AR9485: txpower registers: RATE1=0x%08x RATE2=0x%08x "
            "RATE3=0x%08x RATE4=0x%08x AR_TPC=0x%08x AR_PHY_PWRTX_MAX=0x%08x "
            "AR_PHY_TX_FORCED_GAIN=0x%08x\n",
            REG_READ(ah, AR_PHY_POWER_TX_RATE(0)),
            REG_READ(ah, AR_PHY_POWER_TX_RATE(1)),
            REG_READ(ah, AR_PHY_POWER_TX_RATE(2)),
            REG_READ(ah, AR_PHY_POWER_TX_RATE(3)),
            REG_READ(ah, AR_TPC),
            REG_READ(ah, AR_PHY_PWRTX_MAX),
            REG_READ(ah, AR_PHY_TX_FORCED_GAIN));
}

/* --------------------------------------------------------------------
 *  ADAPTED from hw.c:2952 (ath9k_hw_apply_txpower) plus hw.c:2940
 *  (get_antenna_gain).
 *
 *  Upstream computes
 *      ctl      = ath9k_regd_get_ctl(reg, chan)
 *      chan_pwr = min(chan->chan->max_power * 2, MAX_COMBINED_POWER)
 *      new_pwr  = min(chan_pwr, reg->power_limit)
 *  and hands all three to eep_ops->set_txpower.  None of those inputs
 *  exists here - there is no ath_regulatory and no mac80211
 *  ieee80211_channel - so `ctl' is dropped with the CTL walk and
 *  `new_pwr' becomes the fixed AR9485_TXPOWER_LIMIT_2X.  See the
 *  REGULATORY note in the file header.
 *
 *  get_antenna_gain() reduces to the EEP_ANTENNA_GAIN_2G case of
 *  ath9k_hw_ar9300_get_eeprom() (ar9003_eeprom.c:3009), i.e. one field
 *  of the 2 GHz modal header.
 * -------------------------------------------------------------------- */

void ath9k_hw_apply_txpower(struct ath_hw *ah, struct ath9k_channel *chan,
                            bool test)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    u8 antenna_gain;

    UNREFERENCED_PARAMETER(test);

    if (!chan)
        return;

    if (!ah->eeprom_valid) {
        DPRINT1("AR9485: no EEPROM image; transmit power left at initvals "
                "defaults\n");
        return;
    }

    antenna_gain = (u8)eep->modalHeader2G.antennaGain;

    ath9k_hw_ar9300_set_txpower(ah, chan, antenna_gain,
                                AR9485_TXPOWER_LIMIT_2X);
}
