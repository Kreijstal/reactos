/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     The transmit half of ar9003_hw_init_cal() - transmit I/Q
 *              calibration and the manual peak-detector calibration.
 *
 *              The blocks between the VERBATIM markers are copied from Linux
 *              drivers/net/wireless/ath/ath9k/ar9003_calib.c (v6.12-rc5).
 *
 *              hw_reset.c already carries the RECEIVE half - the AGC filter
 *              calibration and the noise-floor calibration - and its header
 *              records the transmit half as "deliberately NOT ported yet".
 *              Boot 59 turned that into a measured fault: the MAC transmits
 *              and the AP never acknowledges, with an otherwise perfect
 *              receiver.  An uncalibrated transmitter produces exactly that.
 *
 *              On the AR9485 the transmit I/Q calibration does not run as a
 *              standalone step: ar9003_hw_init_cal_settings() sets both
 *              TX_IQ_CAL and TX_IQ_ON_AGC_CAL, so the hardware folds it into
 *              the AGC calibration that hw_reset.c already runs.  What this
 *              file adds is therefore (a) arming it before that AGC cal, and
 *              (b) reading the result out afterwards and programming the
 *              correction coefficients, which is where the real work is.
 *
 *              Modifications, confined to:
 *
 *                - ath_dbg() -> DPRINT1(), as elsewhere in this port.
 *                - ah->caldata is NULL throughout.  Upstream uses it to
 *                  carry calibration results across a channel change and to
 *                  reload them instead of recalibrating; this driver has no
 *                  such cache yet, so every reset calibrates afresh.  Each
 *                  `if (caldata)' arm is therefore dropped, not stubbed.
 *                - the AR9550 median filter and its ar955x_tx_iq_cal_median()
 *                  are dropped (AR_SREV_9550 is false), so outlier detection
 *                  always runs.
 *                - `static struct coeff coeff' becomes a caller-provided
 *                  stack object.  Upstream's file-scope static is shared by
 *                  every adapter in the system; one AR9485 makes that
 *                  harmless there, but a static mutable in a kernel driver
 *                  that can be loaded twice is a defect waiting to happen.
 *                - MAXIQCAL collapses to 1.  It indexes repeated calibration
 *                  passes for the AR9550 median filter only; with that gone,
 *                  iqcal_idx is always 0.
 */

#include "hw_min.h"

#define NDEBUG
#include <debug.h>

#define MAX_MEASUREMENT     MAX_IQCAL_MEASUREMENT
#define MAX_MAG_DELTA       11
#define MAX_PHS_DELTA       10

/* Upstream's third dimension sizes the AR9550 multi-pass median filter,
 * which is not ported; one pass is all this part ever runs. */
#define MAXIQCAL            1

struct coeff {
    int mag_coeff[AR9300_MAX_CHAINS][MAX_MEASUREMENT][MAXIQCAL];
    int phs_coeff[AR9300_MAX_CHAINS][MAX_MEASUREMENT][MAXIQCAL];
    int iqc_coeff[2];
};

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_calib.c:551
 * -------------------------------------------------------------------- */

static bool ar9003_hw_solve_iq_cal(struct ath_hw *ah,
                                   s32 sin_2phi_1,
                                   s32 cos_2phi_1,
                                   s32 sin_2phi_2,
                                   s32 cos_2phi_2,
                                   s32 mag_a0_d0,
                                   s32 phs_a0_d0,
                                   s32 mag_a1_d0,
                                   s32 phs_a1_d0,
                                   s32 solved_eq[])
{
    s32 f1 = cos_2phi_1 - cos_2phi_2,
        f3 = sin_2phi_1 - sin_2phi_2,
        f2;
    s32 mag_tx, phs_tx, mag_rx, phs_rx;
    const s32 result_shift = 1 << 15;

    (void)ah;

    f2 = ((f1 >> 3) * (f1 >> 3) + (f3 >> 3) * (f3 >> 3)) >> 9;

    if (!f2) {
        DPRINT1("AR9485: IQ cal: divide by 0\n");
        return false;
    }

    /* mag mismatch, tx */
    mag_tx = f1 * (mag_a0_d0 - mag_a1_d0) + f3 * (phs_a0_d0 - phs_a1_d0);
    /* phs mismatch, tx */
    phs_tx = f3 * (-mag_a0_d0 + mag_a1_d0) + f1 * (phs_a0_d0 - phs_a1_d0);

    mag_tx = (mag_tx / f2);
    phs_tx = (phs_tx / f2);

    /* mag mismatch, rx */
    mag_rx = mag_a0_d0 - (cos_2phi_1 * mag_tx + sin_2phi_1 * phs_tx) /
             result_shift;
    /* phs mismatch, rx */
    phs_rx = phs_a0_d0 + (sin_2phi_1 * mag_tx - cos_2phi_1 * phs_tx) /
             result_shift;

    solved_eq[0] = mag_tx;
    solved_eq[1] = phs_tx;
    solved_eq[2] = mag_rx;
    solved_eq[3] = phs_rx;

    return true;
}

static s32 ar9003_hw_find_mag_approx(struct ath_hw *ah, s32 in_re, s32 in_im)
{
    s32 abs_i = ATH_ABS(in_re),
        abs_q = ATH_ABS(in_im),
        max_abs, min_abs;

    (void)ah;

    if (abs_i > abs_q) {
        max_abs = abs_i;
        min_abs = abs_q;
    } else {
        max_abs = abs_q;
        min_abs = abs_i;
    }

    return max_abs - (max_abs / 32) + (min_abs / 8) + (min_abs / 4);
}

#define DELPT 32

static bool ar9003_hw_calc_iq_corr(struct ath_hw *ah,
                                   s32 chain_idx,
                                   const s32 iq_res[],
                                   s32 iqc_coeff[])
{
    s32 i2_m_q2_a0_d0, i2_p_q2_a0_d0, iq_corr_a0_d0,
        i2_m_q2_a0_d1, i2_p_q2_a0_d1, iq_corr_a0_d1,
        i2_m_q2_a1_d0, i2_p_q2_a1_d0, iq_corr_a1_d0,
        i2_m_q2_a1_d1, i2_p_q2_a1_d1, iq_corr_a1_d1;
    s32 mag_a0_d0, mag_a1_d0, mag_a0_d1, mag_a1_d1,
        phs_a0_d0, phs_a1_d0, phs_a0_d1, phs_a1_d1,
        sin_2phi_1, cos_2phi_1,
        sin_2phi_2, cos_2phi_2;
    s32 mag_tx, phs_tx, mag_rx, phs_rx;
    s32 solved_eq[4], mag_corr_tx, phs_corr_tx, mag_corr_rx, phs_corr_rx,
        q_q_coff, q_i_coff;
    const s32 res_scale = 1 << 15;
    const s32 delpt_shift = 1 << 8;
    s32 mag1, mag2;

    i2_m_q2_a0_d0 = iq_res[0] & 0xfff;
    i2_p_q2_a0_d0 = (iq_res[0] >> 12) & 0xfff;
    iq_corr_a0_d0 = ((iq_res[0] >> 24) & 0xff) + ((iq_res[1] & 0xf) << 8);

    if (i2_m_q2_a0_d0 > 0x800)
        i2_m_q2_a0_d0 = -((0xfff - i2_m_q2_a0_d0) + 1);

    if (i2_p_q2_a0_d0 > 0x800)
        i2_p_q2_a0_d0 = -((0xfff - i2_p_q2_a0_d0) + 1);

    if (iq_corr_a0_d0 > 0x800)
        iq_corr_a0_d0 = -((0xfff - iq_corr_a0_d0) + 1);

    i2_m_q2_a0_d1 = (iq_res[1] >> 4) & 0xfff;
    i2_p_q2_a0_d1 = (iq_res[2] & 0xfff);
    iq_corr_a0_d1 = (iq_res[2] >> 12) & 0xfff;

    if (i2_m_q2_a0_d1 > 0x800)
        i2_m_q2_a0_d1 = -((0xfff - i2_m_q2_a0_d1) + 1);

    if (iq_corr_a0_d1 > 0x800)
        iq_corr_a0_d1 = -((0xfff - iq_corr_a0_d1) + 1);

    i2_m_q2_a1_d0 = ((iq_res[2] >> 24) & 0xff) + ((iq_res[3] & 0xf) << 8);
    i2_p_q2_a1_d0 = (iq_res[3] >> 4) & 0xfff;
    iq_corr_a1_d0 = iq_res[4] & 0xfff;

    if (i2_m_q2_a1_d0 > 0x800)
        i2_m_q2_a1_d0 = -((0xfff - i2_m_q2_a1_d0) + 1);

    if (i2_p_q2_a1_d0 > 0x800)
        i2_p_q2_a1_d0 = -((0xfff - i2_p_q2_a1_d0) + 1);

    if (iq_corr_a1_d0 > 0x800)
        iq_corr_a1_d0 = -((0xfff - iq_corr_a1_d0) + 1);

    i2_m_q2_a1_d1 = (iq_res[4] >> 12) & 0xfff;
    i2_p_q2_a1_d1 = ((iq_res[4] >> 24) & 0xff) + ((iq_res[5] & 0xf) << 8);
    iq_corr_a1_d1 = (iq_res[5] >> 4) & 0xfff;

    if (i2_m_q2_a1_d1 > 0x800)
        i2_m_q2_a1_d1 = -((0xfff - i2_m_q2_a1_d1) + 1);

    if (i2_p_q2_a1_d1 > 0x800)
        i2_p_q2_a1_d1 = -((0xfff - i2_p_q2_a1_d1) + 1);

    if (iq_corr_a1_d1 > 0x800)
        iq_corr_a1_d1 = -((0xfff - iq_corr_a1_d1) + 1);

    if ((i2_p_q2_a0_d0 == 0) || (i2_p_q2_a0_d1 == 0) ||
        (i2_p_q2_a1_d0 == 0) || (i2_p_q2_a1_d1 == 0)) {
        DPRINT1("AR9485: IQ cal divide by 0: a0_d0=%d a0_d1=%d a1_d0=%d "
                "a1_d1=%d\n",
                i2_p_q2_a0_d0, i2_p_q2_a0_d1,
                i2_p_q2_a1_d0, i2_p_q2_a1_d1);
        return false;
    }

    if ((i2_p_q2_a0_d0 < 1024) || (i2_p_q2_a0_d0 > 2047) ||
        (i2_p_q2_a1_d0 < 0) || (i2_p_q2_a1_d1 < 0) ||
        (i2_p_q2_a0_d0 <= i2_m_q2_a0_d0) ||
        (i2_p_q2_a0_d0 <= iq_corr_a0_d0) ||
        (i2_p_q2_a0_d1 <= i2_m_q2_a0_d1) ||
        (i2_p_q2_a0_d1 <= iq_corr_a0_d1) ||
        (i2_p_q2_a1_d0 <= i2_m_q2_a1_d0) ||
        (i2_p_q2_a1_d0 <= iq_corr_a1_d0) ||
        (i2_p_q2_a1_d1 <= i2_m_q2_a1_d1) ||
        (i2_p_q2_a1_d1 <= iq_corr_a1_d1)) {
        return false;
    }

    mag_a0_d0 = (i2_m_q2_a0_d0 * res_scale) / i2_p_q2_a0_d0;
    phs_a0_d0 = (iq_corr_a0_d0 * res_scale) / i2_p_q2_a0_d0;

    mag_a0_d1 = (i2_m_q2_a0_d1 * res_scale) / i2_p_q2_a0_d1;
    phs_a0_d1 = (iq_corr_a0_d1 * res_scale) / i2_p_q2_a0_d1;

    mag_a1_d0 = (i2_m_q2_a1_d0 * res_scale) / i2_p_q2_a1_d0;
    phs_a1_d0 = (iq_corr_a1_d0 * res_scale) / i2_p_q2_a1_d0;

    mag_a1_d1 = (i2_m_q2_a1_d1 * res_scale) / i2_p_q2_a1_d1;
    phs_a1_d1 = (iq_corr_a1_d1 * res_scale) / i2_p_q2_a1_d1;

    /* w/o analog phase shift */
    sin_2phi_1 = (((mag_a0_d0 - mag_a0_d1) * delpt_shift) / DELPT);
    /* w/o analog phase shift */
    cos_2phi_1 = (((phs_a0_d1 - phs_a0_d0) * delpt_shift) / DELPT);
    /* w/  analog phase shift */
    sin_2phi_2 = (((mag_a1_d0 - mag_a1_d1) * delpt_shift) / DELPT);
    /* w/  analog phase shift */
    cos_2phi_2 = (((phs_a1_d1 - phs_a1_d0) * delpt_shift) / DELPT);

    /*
     * force sin^2 + cos^2 = 1;
     * find magnitude by approximation
     */
    mag1 = ar9003_hw_find_mag_approx(ah, cos_2phi_1, sin_2phi_1);
    mag2 = ar9003_hw_find_mag_approx(ah, cos_2phi_2, sin_2phi_2);

    if ((mag1 == 0) || (mag2 == 0)) {
        DPRINT1("AR9485: IQ cal divide by 0: mag1=%d mag2=%d\n", mag1, mag2);
        return false;
    }

    /* normalization sin and cos by mag */
    sin_2phi_1 = (sin_2phi_1 * res_scale / mag1);
    cos_2phi_1 = (cos_2phi_1 * res_scale / mag1);
    sin_2phi_2 = (sin_2phi_2 * res_scale / mag2);
    cos_2phi_2 = (cos_2phi_2 * res_scale / mag2);

    /* calculate IQ mismatch */
    if (!ar9003_hw_solve_iq_cal(ah,
                                sin_2phi_1, cos_2phi_1,
                                sin_2phi_2, cos_2phi_2,
                                mag_a0_d0, phs_a0_d0,
                                mag_a1_d0,
                                phs_a1_d0, solved_eq)) {
        DPRINT1("AR9485: ar9003_hw_solve_iq_cal() failed\n");
        return false;
    }

    mag_tx = solved_eq[0];
    phs_tx = solved_eq[1];
    mag_rx = solved_eq[2];
    phs_rx = solved_eq[3];

    if (res_scale == mag_tx) {
        DPRINT1("AR9485: IQ cal divide by 0: mag_tx=%d res_scale=%d\n",
                mag_tx, res_scale);
        return false;
    }

    /* calculate and quantize Tx IQ correction factor */
    mag_corr_tx = (mag_tx * res_scale) / (res_scale - mag_tx);
    phs_corr_tx = -phs_tx;

    q_q_coff = (mag_corr_tx * 128 / res_scale);
    q_i_coff = (phs_corr_tx * 256 / res_scale);

    if (q_i_coff < -63)
        q_i_coff = -63;
    if (q_i_coff > 63)
        q_i_coff = 63;
    if (q_q_coff < -63)
        q_q_coff = -63;
    if (q_q_coff > 63)
        q_q_coff = 63;

    iqc_coeff[0] = (q_q_coff * 128) + (0x7f & q_i_coff);

    if (-mag_rx == res_scale) {
        DPRINT1("AR9485: IQ cal divide by 0: mag_rx=%d res_scale=%d\n",
                mag_rx, res_scale);
        return false;
    }

    /* calculate and quantize Rx IQ correction factors */
    mag_corr_rx = (-mag_rx * res_scale) / (res_scale + mag_rx);
    phs_corr_rx = -phs_rx;

    q_q_coff = (mag_corr_rx * 128 / res_scale);
    q_i_coff = (phs_corr_rx * 256 / res_scale);

    if (q_i_coff < -63)
        q_i_coff = -63;
    if (q_i_coff > 63)
        q_i_coff = 63;
    if (q_q_coff < -63)
        q_q_coff = -63;
    if (q_q_coff > 63)
        q_q_coff = 63;

    iqc_coeff[1] = (q_q_coff * 128) + (0x7f & q_i_coff);

    (void)chain_idx;
    return true;
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_calib.c:840
 * -------------------------------------------------------------------- */

static void ar9003_hw_detect_outlier(int mp_coeff[][MAXIQCAL],
                                     int nmeasurement,
                                     int max_delta)
{
    int mp_max = -64, max_idx = 0;
    int mp_min = 63, min_idx = 0;
    int mp_avg = 0, i, outlier_idx = 0, mp_count = 0;

    /* find min/max mismatch across all calibrated gains */
    for (i = 0; i < nmeasurement; i++) {
        if (mp_coeff[i][0] > mp_max) {
            mp_max = mp_coeff[i][0];
            max_idx = i;
        } else if (mp_coeff[i][0] < mp_min) {
            mp_min = mp_coeff[i][0];
            min_idx = i;
        }
    }

    /* find average (exclude max abs value) */
    for (i = 0; i < nmeasurement; i++) {
        if ((ATH_ABS(mp_coeff[i][0]) < ATH_ABS(mp_max)) ||
            (ATH_ABS(mp_coeff[i][0]) < ATH_ABS(mp_min))) {
            mp_avg += mp_coeff[i][0];
            mp_count++;
        }
    }

    /*
     * finding mean magnitude/phase if possible, otherwise
     * just use the last value as the mean
     */
    if (mp_count)
        mp_avg /= mp_count;
    else
        mp_avg = mp_coeff[nmeasurement - 1][0];

    /* detect outlier */
    if (ATH_ABS(mp_max - mp_min) > max_delta) {
        if (ATH_ABS(mp_max - mp_avg) > ATH_ABS(mp_min - mp_avg))
            outlier_idx = max_idx;
        else
            outlier_idx = min_idx;

        mp_coeff[outlier_idx][0] = mp_avg;
    }
}

static void ar9003_hw_tx_iq_cal_outlier_detection(struct ath_hw *ah,
                                                  struct coeff *coeff)
{
    int i, im, nmeasurement;
    int magnitude, phase;
    u32 tx_corr_coeff[MAX_MEASUREMENT][AR9300_MAX_CHAINS];

    RtlZeroMemory(tx_corr_coeff, sizeof(tx_corr_coeff));
    for (i = 0; i < MAX_MEASUREMENT / 2; i++) {
        tx_corr_coeff[i * 2][0] = tx_corr_coeff[(i * 2) + 1][0] =
                    AR_PHY_TX_IQCAL_CORR_COEFF_B0(ah, i);
        /* chains 1 and 2 do not exist on the AR9485 */
    }

    /* Load the average of 2 passes */
    for (i = 0; i < AR9300_MAX_CHAINS; i++) {
        if (!(ah->txchainmask & (1 << i)))
            continue;
        nmeasurement = REG_READ_FIELD(ah,
                AR_PHY_TX_IQCAL_STATUS_B0(ah),
                AR_PHY_CALIBRATED_GAINS_0);

        if (nmeasurement > MAX_MEASUREMENT)
            nmeasurement = MAX_MEASUREMENT;

        /* detect outlier only if nmeasurement > 1 */
        if (nmeasurement > 1) {
            /* Detect magnitude outlier */
            ar9003_hw_detect_outlier(coeff->mag_coeff[i],
                                     nmeasurement, MAX_MAG_DELTA);

            /* Detect phase outlier */
            ar9003_hw_detect_outlier(coeff->phs_coeff[i],
                                     nmeasurement, MAX_PHS_DELTA);
        }

        for (im = 0; im < nmeasurement; im++) {
            magnitude = coeff->mag_coeff[i][im][0];
            phase = coeff->phs_coeff[i][im][0];

            coeff->iqc_coeff[0] =
                (phase & 0x7f) | ((magnitude & 0x7f) << 7);

            if ((im % 2) == 0)
                REG_RMW_FIELD(ah, tx_corr_coeff[im][i],
                    AR_PHY_TX_IQCAL_CORR_COEFF_00_COEFF_TABLE,
                    coeff->iqc_coeff[0]);
            else
                REG_RMW_FIELD(ah, tx_corr_coeff[im][i],
                    AR_PHY_TX_IQCAL_CORR_COEFF_01_COEFF_TABLE,
                    coeff->iqc_coeff[0]);
        }
    }

    REG_RMW_FIELD(ah, AR_PHY_TX_IQCAL_CONTROL_3,
                  AR_PHY_TX_IQCAL_CONTROL_3_IQCORR_EN, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_RX_IQCAL_CORR_B0,
                  AR_PHY_RX_IQCAL_CORR_B0_LOOPBACK_IQCORR_EN, 0x1);
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_calib.c:1053
 * -------------------------------------------------------------------- */

void ar9003_hw_tx_iq_cal_post_proc(struct ath_hw *ah)
{
    const u32 txiqcal_status[AR9300_MAX_CHAINS] = {
        AR_PHY_TX_IQCAL_STATUS_B0(ah),
        AR_PHY_TX_IQCAL_STATUS_B1,
        AR_PHY_TX_IQCAL_STATUS_B2,
    };
    const u32 chan_info_tab[] = {
        AR_PHY_CHAN_INFO_TAB_0,
        AR_PHY_CHAN_INFO_TAB_1,
        AR_PHY_CHAN_INFO_TAB_2,
    };
    struct coeff coeff;
    s32 iq_res[6];
    int i, im, j;
    int nmeasurement = 0;

    RtlZeroMemory(&coeff, sizeof(coeff));

    for (i = 0; i < AR9300_MAX_CHAINS; i++) {
        if (!(ah->txchainmask & (1 << i)))
            continue;

        nmeasurement = REG_READ_FIELD(ah,
                AR_PHY_TX_IQCAL_STATUS_B0(ah),
                AR_PHY_CALIBRATED_GAINS_0);
        if (nmeasurement > MAX_MEASUREMENT)
            nmeasurement = MAX_MEASUREMENT;

        for (im = 0; im < nmeasurement; im++) {
            if (REG_READ(ah, txiqcal_status[i]) &
                    AR_PHY_TX_IQCAL_STATUS_FAILED) {
                DPRINT1("AR9485: Tx IQ cal failed for chain %d\n", i);
                return;
            }

            for (j = 0; j < 3; j++) {
                u32 idx = 2 * j, offset = 4 * (3 * im + j);

                REG_RMW_FIELD(ah,
                        AR_PHY_CHAN_INFO_MEMORY(ah),
                        AR_PHY_CHAN_INFO_TAB_S2_READ, 0);

                /* 32 bits */
                iq_res[idx] = REG_READ(ah, chan_info_tab[i] + offset);

                REG_RMW_FIELD(ah,
                        AR_PHY_CHAN_INFO_MEMORY(ah),
                        AR_PHY_CHAN_INFO_TAB_S2_READ, 1);

                /* 16 bits */
                iq_res[idx + 1] = 0xffff &
                        REG_READ(ah, chan_info_tab[i] + offset);
            }

            if (!ar9003_hw_calc_iq_corr(ah, i, iq_res, coeff.iqc_coeff)) {
                DPRINT1("AR9485: failed to calculate IQ correction "
                        "(chain %d, measurement %d)\n", i, im);
                return;
            }

            coeff.phs_coeff[i][im][0] = coeff.iqc_coeff[0] & 0x7f;
            coeff.mag_coeff[i][im][0] = (coeff.iqc_coeff[0] >> 7) & 0x7f;

            if (coeff.mag_coeff[i][im][0] > 63)
                coeff.mag_coeff[i][im][0] -= 128;
            if (coeff.phs_coeff[i][im][0] > 63)
                coeff.phs_coeff[i][im][0] -= 128;
        }
    }

    ar9003_hw_tx_iq_cal_outlier_detection(ah, &coeff);

    DPRINT1("AR9485: Tx IQ calibration applied over %d gain measurement(s)\n",
            nmeasurement);
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_calib.c:1198 - manual peak-detector calibration.
 *
 *  peak_detect_threshold is 0 on this part; the 8/11 arms are for the
 *  AR9550/9531/9561.
 * -------------------------------------------------------------------- */

static void ar9003_hw_manual_peak_cal(struct ath_hw *ah, u8 chain, bool is_2g)
{
    int offset[8] = {0}, total = 0, test;
    int agc_out, i;
    const int peak_detect_threshold = 0;

    (void)is_2g;

    /*
     * Turn off LNA/SW.
     */
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_GAINSTAGES(chain),
                  AR_PHY_65NM_RXRF_GAINSTAGES_RX_OVERRIDE, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_GAINSTAGES(chain),
                  AR_PHY_65NM_RXRF_GAINSTAGES_LNAON_CALDC, 0x0);

    /* AR_SREV_9003_PCOEM(): true for the AR9485, 2 GHz arm. */
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_GAINSTAGES(chain),
                  AR_PHY_65NM_RXRF_GAINSTAGES_LNA2G_GAIN_OVR, 0x0);

    /*
     * Turn off RXON.
     */
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXTX2(chain),
                  AR_PHY_65NM_RXTX2_RXON_OVR, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXTX2(chain),
                  AR_PHY_65NM_RXTX2_RXON, 0x0);

    /*
     * Turn on AGC for cal.
     */
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain),
                  AR_PHY_65NM_RXRF_AGC_AGC_OVERRIDE, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain),
                  AR_PHY_65NM_RXRF_AGC_AGC_ON_OVR, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain),
                  AR_PHY_65NM_RXRF_AGC_AGC_CAL_OVR, 0x1);

    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain),
                  AR_PHY_65NM_RXRF_AGC_AGC2G_DBDAC_OVR,
                  peak_detect_threshold);

    for (i = 6; i > 0; i--) {
        offset[i] = BIT(i - 1);
        test = total + offset[i];

        REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain),
                      AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR, test);
        udelay(100);
        agc_out = REG_READ_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain),
                                 AR_PHY_65NM_RXRF_AGC_AGC_OUT);
        offset[i] = (agc_out) ? 0 : 1;
        total += (offset[i] << (i - 1));
    }

    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain),
                  AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR, total);

    /*
     * Turn on LNA.
     */
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_GAINSTAGES(chain),
                  AR_PHY_65NM_RXRF_GAINSTAGES_RX_OVERRIDE, 0);
    /*
     * Turn off RXON.
     */
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXTX2(chain),
                  AR_PHY_65NM_RXTX2_RXON_OVR, 0);
    /*
     * Turn off peak detect calibration.
     */
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain),
                  AR_PHY_65NM_RXRF_AGC_AGC_CAL_OVR, 0);
}

/* ar9003_calib.c:1299.  The RTT early-return and the caldac save-off are
 * both guarded by ATH9K_HW_CAP_RTT, which hw.c:2649 grants only to the
 * AR9462 2.0 and the AR9565 - not to this part. */
void ar9003_hw_do_pcoem_manual_peak_cal(struct ath_hw *ah,
                                        struct ath9k_channel *chan)
{
    int i;

    for (i = 0; i < AR9300_MAX_CHAINS; i++) {
        if (!(ah->rxchainmask & (1 << i)))
            continue;
        ar9003_hw_manual_peak_cal(ah, (u8)i, IS_CHAN_2GHZ(chan));
    }
}

/* --------------------------------------------------------------------
 *  end verbatim
 * -------------------------------------------------------------------- */
