/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Apply the per-board analog configuration held in the card's
 *              EEPROM - ath9k_hw_ar9300_set_board_values() and the helpers
 *              it drives.
 *
 *              The blocks between the VERBATIM markers are copied from Linux
 *              drivers/net/wireless/ath/ath9k/ar9003_eeprom.c (v6.12-rc5).
 *
 *              WHY THIS FILE EXISTS.  Until now nothing in this driver ever
 *              applied a single EEPROM-derived analog setting.  The initvals
 *              bring the PHY up with generic defaults, which is enough for a
 *              receiver -- docs/asus.txt records a healthy 41-BSS scan -- but
 *              the transmit chain is board-specific in ways the initvals
 *              cannot know:
 *
 *                xpaBiasLvl      the external PA bias level
 *                antCtrlCommon   the RF switch table (which way the T/R
 *                antCtrlCommon2  switch is thrown, per state)
 *                antCtrlChain
 *                xatten1DB       per-chain attenuation and its margin
 *                xatten1Margin
 *                txEndToXpaOff   when the PA is powered down after a frame
 *
 *              Boot 59 measured the consequence: the MAC transmits (AR_TFCNT
 *              advances by a physically correct number of cycles) and the AP
 *              never acknowledges (AR_ACK_FAIL climbs), while receive is
 *              simultaneously excellent.  An asymmetric link like that is
 *              what an unconfigured PA bias and RF switch produce.
 *
 *              Modifications, confined to:
 *
 *                - ath_dbg() -> DPRINT1(), as elsewhere in this port; all of
 *                  these sit on the reset path, none is a hot path.
 *                - ath9k_hw_gpio_request_out() for the AR9485 external-LNA
 *                  control GPIO is NOT ported; see the comment in
 *                  ar9003_hw_ant_ctrl_apply().
 *                - common->bt_ant_diversity is dropped.  It is set only when
 *                  a shared BT front end is wired, and the AR9485 on this
 *                  board has none (hw_reset.c's header records the same for
 *                  the MCI blocks).  Every arm guarded by it is dead code
 *                  here, so it is not carried.
 *                - the AR9462/9565/9550/9531/9561/9330/9340/9300/9580 arms
 *                  fold away: AR_SREV_9485() is true and mutually exclusive
 *                  with all of them.  Where an entire function is guarded to
 *                  parts we are not, the function is reduced to its guard and
 *                  a comment rather than dropped, so the shape still matches
 *                  upstream and a later part is easy to add.
 */

#include "hw_min.h"

#define NDEBUG
#include <debug.h>

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:3593 - EEPROM accessors.
 * -------------------------------------------------------------------- */

static struct ar9300_modal_eep_header *ar9003_modal_header(struct ath_hw *ah,
                                                           bool is2ghz)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;

    if (is2ghz)
        return &eep->modalHeader2G;
    else
        return &eep->modalHeader5G;
}

static u32 ar9003_hw_ant_ctrl_common_get(struct ath_hw *ah, bool is2ghz)
{
    return le32_to_cpu(ar9003_modal_header(ah, is2ghz)->antCtrlCommon);
}

static u32 ar9003_hw_ant_ctrl_common_2_get(struct ath_hw *ah, bool is2ghz)
{
    return le32_to_cpu(ar9003_modal_header(ah, is2ghz)->antCtrlCommon2);
}

static u16 ar9003_hw_ant_ctrl_chain_get(struct ath_hw *ah, int chain,
                                        bool is2ghz)
{
    __le16 val = ar9003_modal_header(ah, is2ghz)->antCtrlChain[chain];
    return le16_to_cpu(val);
}

s32 ar9003_hw_get_rx_gain_idx(struct ath_hw *ah)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;

    return (eep->baseEepHeader.txrxgain) & 0xf; /* bits 3:0 */
}

/* Upstream reaches this through eep_ops->get_eeprom(EEP_ANT_DIV_CTL1)
 * (ar9003_eeprom.c:3002).  The AR9565 arm returns a constant and folds. */
static u16 ar9003_hw_ant_div_ctl1_get(struct ath_hw *ah)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;

    return eep->base_ext1.ant_div_control;
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:3604, xpaBiasLvl.
 *
 *  This is the external power-amplifier bias level.  On the AR9485 it
 *  lands in AR_CH0_TOP2; the other arms are for parts we are not.
 * -------------------------------------------------------------------- */

static void ar9003_hw_xpa_bias_level_apply(struct ath_hw *ah, bool is2ghz)
{
    int bias = ar9003_modal_header(ah, is2ghz)->xpaBiasLvl;

    REG_RMW_FIELD(ah, AR_CH0_TOP2(ah), AR_CH0_TOP2_XPABIASLVL, bias);
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:3645, the RF switch table.
 * -------------------------------------------------------------------- */

static void ar9003_hw_ant_ctrl_apply(struct ath_hw *ah, bool is2ghz)
{
    int chain;
    u32 regval, value;
    static const u32 switch_chain_reg[AR9300_MAX_CHAINS] = {
            AR_PHY_SWITCH_CHAIN_0,
            AR_PHY_SWITCH_CHAIN_1,
            AR_PHY_SWITCH_CHAIN_2,
    };

    /*
     * Upstream drives the external-LNA control GPIO here when the card
     * uses rx gain table 0 (ar9003_eeprom.c:3657).  That needs
     * ath9k_hw_gpio_request_out() and the AR9003 GPIO output-mux, none of
     * which this port carries, and it configures a RECEIVE-side LNA on a
     * receiver that already works.  Porting it to chase a transmit fault
     * would risk the one half of this radio that is known good, so it is
     * logged instead: if this card does use gain table 0, the line below
     * says so and the GPIO becomes a real work item.
     */
    if (ar9003_hw_get_rx_gain_idx(ah) == 0)
    {
        DPRINT1("AR9485: rx gain table 0 selected; upstream would drive the "
                "external-LNA GPIO %d here, which this port does not "
                "implement\n", AR9300_EXT_LNA_CTL_GPIO_AR9485);
    }

    /* SETTLED, 2026-08-26, by dumping the card's raw OTP through the lab and
     * replaying the decompressor host-side (.claude/ar9485-lab/otp-decode.py).
     * The card's compressed diff CARRIES antCtrlCommon and the low three bytes
     * of antCtrlCommon2 -- the only bytes AR_SWITCH_TABLE_COM2_ALL uses -- as
     * literal zero, so SWITCH_COM and SWITCH_COM_2 reading 0 is this board's
     * own value and not a parse fault.  antCtrlChain is NOT carried, so it is
     * inherited from ar9300_default (0x150), and AR_PHY_SWITCH_CHAIN_0 duly
     * measured 0x00000150 once the template seed went in.  The earlier
     * 0x2a0 -> 0 clobber was the missing seed, and it is fixed. */
    value = ar9003_hw_ant_ctrl_common_get(ah, is2ghz);
    REG_RMW_FIELD(ah, AR_PHY_SWITCH_COM, AR_SWITCH_TABLE_COM_ALL, value);

    value = ar9003_hw_ant_ctrl_common_2_get(ah, is2ghz);
    REG_RMW_FIELD(ah, AR_PHY_SWITCH_COM_2, AR_SWITCH_TABLE_COM2_ALL, value);

    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++) {
        if ((ah->rxchainmask & BIT(chain)) ||
            (ah->txchainmask & BIT(chain))) {
            value = ar9003_hw_ant_ctrl_chain_get(ah, chain, is2ghz);
            REG_RMW_FIELD(ah, switch_chain_reg[chain],
                          AR_SWITCH_TABLE_ALL, value);
        }
    }

    /* AR_SREV_9330 || AR_SREV_9485 || AR_SREV_9565 - we are the 9485. */
    value = ar9003_hw_ant_div_ctl1_get(ah);
    /*
     * main_lnaconf, alt_lnaconf, main_tb, alt_tb
     * are the fields present
     */
    regval = REG_READ(ah, AR_PHY_MC_GAIN_CTRL);
    regval &= (~AR_ANT_DIV_CTRL_ALL);
    regval |= (value & 0x3f) << AR_ANT_DIV_CTRL_ALL_S;
    /* enable_lnadiv */
    regval &= (~AR_PHY_ANT_DIV_LNADIV);
    regval |= ((value >> 6) & 0x1) << AR_PHY_ANT_DIV_LNADIV_S;

    REG_WRITE(ah, AR_PHY_MC_GAIN_CTRL, regval);

    /* enable fast_div */
    regval = REG_READ(ah, AR_PHY_CCK_DETECT);
    regval &= (~AR_FAST_DIV_ENABLE);
    regval |= ((value >> 7) & 0x1) << AR_FAST_DIV_ENABLE_S;
    REG_WRITE(ah, AR_PHY_CCK_DETECT, regval);

    /*
     * Upstream gates this on ATH9K_HW_CAP_ANT_DIV_COMB, which hw.c:2649
     * sets for the 9330/9485/9565 exactly when (ant_div_ctl1 >> 6) == 0x3.
     * The capability is not otherwise modelled in this port, so the test
     * is written out where the capability would have been consulted.
     */
    if ((value >> 6) == 0x3) {
        regval = REG_READ(ah, AR_PHY_MC_GAIN_CTRL);
        /*
         * clear bits 25-30 main_lnaconf, alt_lnaconf,
         * main_tb, alt_tb
         */
        regval &= (~(AR_PHY_ANT_DIV_MAIN_LNACONF |
                     AR_PHY_ANT_DIV_ALT_LNACONF |
                     AR_PHY_ANT_DIV_ALT_GAINTB |
                     AR_PHY_ANT_DIV_MAIN_GAINTB));
        /* by default use LNA1 for the main antenna */
        regval |= (ATH_ANT_DIV_COMB_LNA1 << AR_PHY_ANT_DIV_MAIN_LNACONF_S);
        regval |= (ATH_ANT_DIV_COMB_LNA2 << AR_PHY_ANT_DIV_ALT_LNACONF_S);
        REG_WRITE(ah, AR_PHY_MC_GAIN_CTRL, regval);
    }
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:3796.
 * -------------------------------------------------------------------- */

static void ar9003_hw_drive_strength_apply(struct ath_hw *ah)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    struct ar9300_base_eep_hdr *pBase = &eep->baseEepHeader;
    int drive_strength;
    u32 reg;

    drive_strength = pBase->miscConfiguration & BIT(0);
    if (!drive_strength)
        return;

    reg = REG_READ(ah, AR_PHY_65NM_CH0_BIAS1);
    reg &= ~0x00ffffc0;
    reg |= 0x5 << 21;
    reg |= 0x5 << 18;
    reg |= 0x5 << 15;
    reg |= 0x5 << 12;
    reg |= 0x5 << 9;
    reg |= 0x5 << 6;
    REG_WRITE(ah, AR_PHY_65NM_CH0_BIAS1, reg);

    reg = REG_READ(ah, AR_PHY_65NM_CH0_BIAS2);
    reg &= ~0xffffffe0;
    reg |= 0x5 << 29;
    reg |= 0x5 << 26;
    reg |= 0x5 << 23;
    reg |= 0x5 << 20;
    reg |= 0x5 << 17;
    reg |= 0x5 << 14;
    reg |= 0x5 << 11;
    reg |= 0x5 << 8;
    reg |= 0x5 << 5;
    REG_WRITE(ah, AR_PHY_65NM_CH0_BIAS2, reg);

    reg = REG_READ(ah, AR_PHY_65NM_CH0_BIAS4);
    reg &= ~0xff800000;
    reg |= 0x5 << 29;
    reg |= 0x5 << 26;
    reg |= 0x5 << 23;
    REG_WRITE(ah, AR_PHY_65NM_CH0_BIAS4, reg);
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:3838 - per-chain attenuation.
 *
 *  The 5 GHz interpolation arms are dropped: this is a single-band
 *  2.4 GHz part, so IS_CHAN_2GHZ() is constant-true and the first arm is
 *  the only reachable one.
 * -------------------------------------------------------------------- */

static u16 ar9003_hw_atten_chain_get(struct ath_hw *ah, int chain,
                                     struct ath9k_channel *chan)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;

    (void)chan;

    if (chain >= 0 && chain < 3)
        return eep->modalHeader2G.xatten1DB[chain];

    return 0;
}

static u16 ar9003_hw_atten_chain_get_margin(struct ath_hw *ah, int chain,
                                            struct ath9k_channel *chan)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;

    (void)chan;

    if (chain >= 0 && chain < 3)
        return eep->modalHeader2G.xatten1Margin[chain];

    return 0;
}

static void ar9003_hw_atten_apply(struct ath_hw *ah, struct ath9k_channel *chan)
{
    int i;
    u16 value;
    static const u32 ext_atten_reg[3] = { AR_PHY_EXT_ATTEN_CTL_0,
                                          AR_PHY_EXT_ATTEN_CTL_1,
                                          AR_PHY_EXT_ATTEN_CTL_2 };

    /* Test value. if 0 then attenuation is unused. Don't load anything. */
    for (i = 0; i < AR9300_MAX_CHAINS; i++) {
        if (ah->txchainmask & BIT(i)) {
            value = ar9003_hw_atten_chain_get(ah, i, chan);
            REG_RMW_FIELD(ah, ext_atten_reg[i],
                          AR_PHY_EXT_ATTEN_CTL_XATTEN1_DB, value);

            /* Upstream substitutes a margin of 5 here when gain table 0
             * is in use AND ah->config.xatten_margin_cfg is set.  That
             * config knob is a module parameter defaulting to false and
             * is not modelled, so the EEPROM value is always used. */
            value = ar9003_hw_atten_chain_get_margin(ah, i, chan);

            REG_RMW_FIELD(ah, ext_atten_reg[i],
                          AR_PHY_EXT_ATTEN_CTL_XATTEN1_MARGIN, value);
        }
    }
}

#ifndef AR9485_NO_PMU
/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:3939 - the internal regulator (PMU).
 * -------------------------------------------------------------------- */

static bool is_pmu_set(struct ath_hw *ah, u32 pmu_reg, int pmu_set)
{
    int timeout = 100;

    while (pmu_set != (int)REG_READ(ah, pmu_reg)) {
        if (timeout-- == 0)
            return false;
        REG_WRITE(ah, pmu_reg, pmu_set);
        udelay(10);
    }

    return true;
}

static void ar9003_hw_internal_regulator_apply(struct ath_hw *ah)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    struct ar9300_base_eep_hdr *pBase = &eep->baseEepHeader;

    if (pBase->featureEnable & BIT(4)) {
        /* AR_SREV_9330 || AR_SREV_9485 - we are the 9485. */
        int reg_pmu_set;

        reg_pmu_set = REG_READ(ah, AR_PHY_PMU2(ah)) & ~AR_PHY_PMU2_PGM;
        REG_WRITE(ah, AR_PHY_PMU2(ah), reg_pmu_set);
        if (!is_pmu_set(ah, AR_PHY_PMU2(ah), reg_pmu_set)) {
            DPRINT1("AR9485: PMU2 will not clear PGM\n");
            return;
        }

        reg_pmu_set = (5 << 1) | (7 << 4) |
                      (2 << 8) | (2 << 14) |
                      (6 << 17) | (1 << 20) |
                      (3 << 24) | (1 << 28);

        REG_WRITE(ah, AR_PHY_PMU1(ah), reg_pmu_set);
        if (!is_pmu_set(ah, AR_PHY_PMU1(ah), reg_pmu_set)) {
            DPRINT1("AR9485: PMU1 will not take 0x%08x\n", reg_pmu_set);
            return;
        }

        reg_pmu_set = (REG_READ(ah, AR_PHY_PMU2(ah)) & ~0xFFC00000)
                        | (4 << 26);
        REG_WRITE(ah, AR_PHY_PMU2(ah), reg_pmu_set);
        if (!is_pmu_set(ah, AR_PHY_PMU2(ah), reg_pmu_set)) {
            DPRINT1("AR9485: PMU2 will not take 0x%08x\n", reg_pmu_set);
            return;
        }

        reg_pmu_set = (REG_READ(ah, AR_PHY_PMU2(ah)) & ~0x00200000)
                        | (1 << 21);
        REG_WRITE(ah, AR_PHY_PMU2(ah), reg_pmu_set);
        if (!is_pmu_set(ah, AR_PHY_PMU2(ah), reg_pmu_set)) {
            DPRINT1("AR9485: PMU2 will not take 0x%08x\n", reg_pmu_set);
            return;
        }
    } else {
        /* Internal regulator is OFF; the board runs from an external
         * supply and upstream clears AR_RTC_REG_CONTROL1_SWREG_PROGRAM
         * (ar9003_eeprom.c:4020).  featureEnable bit 4 is set on every
         * AR9485 module seen so far, so this arm is logged if taken. */
        DPRINT1("AR9485: EEPROM says the internal regulator is off "
                "(featureEnable=0x%02x); external-supply path not ported\n",
                pBase->featureEnable);
    }
}

#endif /* !AR9485_NO_PMU */

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:4048.
 *
 *  NOTE, 2026-08-26: the body is gated on featureEnable & 0x40, and this
 *  card's featureEnable is 0x3d -- bit 6 clear -- so on this hardware the
 *  whole function is a no-op.  It is left in because it is verbatim and
 *  other AR9485 boards do set the bit, but nothing here can be concluded
 *  about it from any boot on THIS card.
 * -------------------------------------------------------------------- */

static void ar9003_hw_apply_tuning_caps(struct ath_hw *ah)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    u8 tuning_caps_param = eep->baseEepHeader.params_for_tuning_caps[0];

    if (eep->baseEepHeader.featureEnable & 0x40) {
        tuning_caps_param &= 0x7f;
        REG_RMW_FIELD(ah, AR_CH0_XTAL(ah), AR_CH0_XTAL_CAPINDAC,
                      tuning_caps_param);
        REG_RMW_FIELD(ah, AR_CH0_XTAL(ah), AR_CH0_XTAL_CAPOUTDAC,
                      tuning_caps_param);
    }
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:4088 - when the PA is switched off
 *  after a frame ends.
 * -------------------------------------------------------------------- */

static void ar9003_hw_txend_to_xpa_off_apply(struct ath_hw *ah, bool is2ghz)
{
    u32 value;

    value = ar9003_modal_header(ah, is2ghz)->txEndToXpaOff;

    REG_RMW_FIELD(ah, AR_PHY_XPA_TIMING_CTL,
                  AR_PHY_XPA_TIMING_CTL_TX_END_XPAB_OFF, value);
    REG_RMW_FIELD(ah, AR_PHY_XPA_TIMING_CTL,
                  AR_PHY_XPA_TIMING_CTL_TX_END_XPAA_OFF, value);
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:4147,4155 - thermometer select.
 * -------------------------------------------------------------------- */

static int ar9003_hw_get_thermometer(struct ath_hw *ah)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    struct ar9300_base_eep_hdr *pBase = &eep->baseEepHeader;
    int thermometer = (pBase->miscConfiguration >> 1) & 0x3;

    return thermometer - 1;
}

static void ar9003_hw_thermometer_apply(struct ath_hw *ah)
{
    int thermometer = ar9003_hw_get_thermometer(ah);
    u8 therm_on = (thermometer < 0) ? 0 : 1;

    REG_RMW_FIELD(ah, AR_PHY_65NM_CH0_RXTX4,
                  AR_PHY_65NM_CH0_RXTX4_THERM_ON_OVR, therm_on);

    therm_on = (thermometer == 0);
    REG_RMW_FIELD(ah, AR_PHY_65NM_CH0_RXTX4,
                  AR_PHY_65NM_CH0_RXTX4_THERM_ON, therm_on);

    /* The chain 1 and 2 arms are guarded by caps.chip_chainmask; this is
     * a 1x1 part, so only chain 0 exists. */
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:4204.
 * -------------------------------------------------------------------- */

static void ar9003_hw_apply_minccapwr_thresh(struct ath_hw *ah, bool is2ghz)
{
    struct ar9300_eeprom *eep = &ah->eeprom.ar9300_eep;
    static const u32 cca_ctrl[AR9300_MAX_CHAINS] = {
        AR_PHY_CCA_CTRL_0,
        AR_PHY_CCA_CTRL_1,
        AR_PHY_CCA_CTRL_2,
    };
    int chain;
    u32 val;

    if (!(eep->base_ext1.misc_enable & BIT(2)))
        return;

    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++) {
        if (!(ah->caps.tx_chainmask & BIT(chain)))
            continue;

        val = ar9003_modal_header(ah, is2ghz)->noiseFloorThreshCh[chain];
        REG_RMW_FIELD(ah, cca_ctrl[chain],
                      AR_PHY_EXT_CCA0_THRESH62_1, val);
    }
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_eeprom.c:4235, ath9k_hw_ar9300_set_board_values().
 *
 *  Dropped from the upstream body, each because its own guard excludes
 *  the AR9485:
 *    - ar9003_hw_xpa_timing_control_apply(): 9300/9340/9580/9531/9561.
 *    - ar9003_hw_xlna_bias_strength_apply(): AR_SREV_9300 only.
 *    - ar9003_hw_quick_drop_apply():         9300/9580/9340 only.
 *    - ar9003_hw_thermo_cal_apply():         AR9462 2.0 or later only.
 * -------------------------------------------------------------------- */

void ath9k_hw_ar9300_set_board_values(struct ath_hw *ah,
                                      struct ath9k_channel *chan)
{
    bool is2ghz = IS_CHAN_2GHZ(chan);

    ar9003_hw_xpa_bias_level_apply(ah, is2ghz);
    ar9003_hw_ant_ctrl_apply(ah, is2ghz);
    ar9003_hw_drive_strength_apply(ah);
    ar9003_hw_atten_apply(ah, chan);

    /* Boot 60 wedged ~90 s in.  These two writes were the suspects, being the
     * only members of the set that touch the analog supply and the reference
     * clock rather than the PHY.  The raw OTP dump settled one of them without
     * spending a boot, and misled me about the other:
     *
     *   featureEnable = 0x3d.  ar9003_hw_apply_tuning_caps() is gated on bit
     *   0x40, which is clear, so it is a no-op on this card -- in boot 62 and
     *   in boot 60 alike.  It was never capable of wedging anything.  That
     *   elimination still stands.
     *
     *   BIT(4) IS set, so the PMU reprogram is live code here.  It was then
     *   replayed write-by-write on the running box through the bring-up lab,
     *   straight after the driver's own bring-up: the chip took all four
     *   writes and stayed alive -- AR_SREV constant across ~3.8 s, PMU1/PMU2
     *   holding their values, AR_PHY_SWITCH_CHAIN_0 still 0x150.  On that
     *   basis I called the PMU eliminated too.  It is not.
     *
     * BOOT 63 (2026-08-26 20:28) OVERTURNED THE PMU HALF OF THAT.  With the
     * PMU applied the box booted, ran for roughly 90-120 s -- long enough for
     * luagent to answer and for KD to log the boot drivers and cpu=0/4 -- and
     * then wedged hard: no ARP, no KD, both channels dead, physical power
     * button required.  Boot 62 and boot 63 differ in this one write sequence
     * and nothing else, and boot 60 wedged the same way with the PMU applied.
     * Two wedges with it, clean boots without it.
     *
     * The lab replay was not wrong, it was short.  It watches for about 4 s
     * (AR9485LAB_MAX_TOTAL_WAIT_US) and the failure needs ~90, which is the
     * "slow brownout out of the instrument's reach" the previous note named as
     * still unrefuted.  Treat "the chip survived the writes" as a statement
     * about 4 seconds, never about a boot.
     *
     * So the PMU reprogram is the leading suspect again, on a clean
     * single-variable comparison, and it has NOT been explained -- the ported
     * sequence matches upstream's AR_SREV_9485 arm write for write.  Until it
     * is, AR9485_NO_PMU must be ON for any build that has to survive a boot on
     * this board.  The committed default stays OFF so the source keeps doing
     * what upstream does; the DEPLOYED binary is the NO_PMU one deliberately.
     * A third data point would make this proven rather than strongly
     * indicated.
     *
     * BOOT 64 (2026-08-27 02:22) IS THAT THIRD DATA POINT, AND IT WENT THE
     * OTHER WAY.  The box powered on with the PMU build still on disk, and it
     * did NOT wedge: it was still answering luagent 13 minutes in, long past
     * the 90-120 s mark.  The chip was healthy throughout -- srev=0x002401ff,
     * mac=576.1, PHY up, permanent MAC read back from OTP.  And the PMU had
     * demonstrably run: AR_PHY_PMU1 (0x16c40) read back 0x131c827a, bit for
     * bit the constant this function computes, and AR_PHY_PMU2 (0x16c44) read
     * 0x10200000, carrying both of its writes ((4 << 26) and (1 << 21)).  No
     * initval table touches either offset and this is the only function in the
     * port that writes them, so those values came from this code running.
     * The chip also survived a SECOND application, via the full
     * ar9485_hw_start() that LabReleaseClaim() performs on release.
     *
     * So the single-variable inference from boot 62 vs 63 was too strong.  Two
     * wedges with the PMU and one clean boot with it means the PMU is at most
     * a contributing factor, not a deterministic trigger, and boots 60 and 63
     * are once again unexplained.
     *
     * THAT ALTERNATIVE IS NOW CLOSED.  Boot 65 (2026-08-27 03:00, reached by
     * a 0xCF9 reset over KDNET) came up on the NO_PMU binary and read
     * AR_PHY_PMU1 = 0x13188278, not 0x131c827a.  The two differ in exactly
     * bits 1 and 18 (0x00040002), and AR_PHY_PMU2 read 0x12000000 with its
     * PGM bit (1 << 21) CLEAR, where the programmed value carried it set.
     * So 0x131c827a is not a power-on default -- this function writes it, the
     * chip really was reset between the two boots, and boot 64 really did run
     * the PMU sequence and survive.  THE PMU IS NOT A DETERMINISTIC WEDGE.
     * Boots 60 and 63 need another explanation.
     *
     * (The superseded reasoning is kept below because it records how the
     * question was framed.)  The open alternative had been: 0x131c827a may
     * simply be this chip's power-on default, in which case the boot-64 read
     * proves nothing about which binary was loaded.  Against that reading:
     * upstream programs these registers unconditionally and verifies each
     * write with is_pmu_set(), and the sequence opens by clearing the PMU2
     * PGM handshake bit -- all of which only makes sense if the values differ
     * from reset.  The discriminator costs nothing and arrives on its own: the
     * NO_PMU binary is now the one on disk, so the NEXT boot should read
     * 0x16c40 first.  If it still reads 0x131c827a, that is the hardware
     * default and this entire note reverts to the boot-63 verdict; if it reads
     * anything else, boot 64 stands and the PMU is exonerated as the sole
     * cause.  Do that read before changing the default either way.
     *
     * ONE ASYMMETRY IN THAT TEST, noticed before spending it: a WARM reboot
     * does not necessarily power-cycle the PCIe device, so PMU state can
     * survive into the next boot.  That makes only one outcome decisive.
     * Reading anything other than 0x131c827a proves the chip was reset and
     * this function did not run -- boot 64 stands.  Reading 0x131c827a proves
     * nothing on a warm boot, because it is equally explained by leftover
     * state from the boot before it; separating "hardware default" from
     * "leftover" costs a COLD power cycle.  Do not read a matching value as a
     * confirmation of the boot-63 verdict.
     */
#ifndef AR9485_NO_PMU
    ar9003_hw_internal_regulator_apply(ah);
#else
    DPRINT1("AR9485: internal regulator (PMU) NOT applied - bisect lever for the unexplained boot 60/63 wedges\n");
#endif
    ar9003_hw_apply_tuning_caps(ah);

    ar9003_hw_apply_minccapwr_thresh(ah, is2ghz);
    ar9003_hw_txend_to_xpa_off_apply(ah, is2ghz);
    ar9003_hw_thermometer_apply(ah);
}

/* --------------------------------------------------------------------
 *  end verbatim
 * -------------------------------------------------------------------- */
