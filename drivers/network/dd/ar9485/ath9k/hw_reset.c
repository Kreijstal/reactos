/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Phase 3b - PHY/MAC bring-up.  Ports the upstream Linux
 *              ath9k reset sequence far enough that the baseband is
 *              running and parked on a 2.4 GHz channel, which is the
 *              precondition for the receive path.
 *
 * Sources: drivers/net/wireless/ath/ath9k/{hw.c,ar9003_phy.c} from Linux
 * 6.12-rc6.  Function bodies are transcribed verbatim except where a
 * branch is provably dead for an AR9485; every such trim is called out
 * in a comment at the site, and the reason is always one of:
 *
 *   - a different chip's SREV arm (AR_SREV_9100/9330/9340/9550/9531/9561/
 *     9462/9565/9271/9300/9580).  AR_SREV_9485() is true and mutually
 *     exclusive with all of them, so those arms cannot execute.
 *   - a 5 GHz / HT40 / half- / quarter-rate arm.  The AR9485 is a
 *     single-band 2.4 GHz 1x1 part and this driver brings it up HT20
 *     only, so the IS_CHAN_* predicates in hw_min.h are constant-false
 *     and the arms fold away at compile time.
 *   - MCI / bluetooth-coexistence.  The AR9485 has no shared BT front
 *     end wired on this board and ath9k_hw_mci_is_enabled() is false.
 *
 * Ported elsewhere, not here:
 *
 *   - ath9k_hw_apply_txpower() and the whole EEPROM target-power /
 *     open-loop-calibration apply chain.  That is Phase 2b and it lives
 *     in hw_txpower.c; ar9003_hw_process_ini() below calls it at the
 *     point upstream does.  It used to be listed as missing here.
 *
 * Deliberately NOT ported yet, and why:
 *
 *   - The IQ / ADC-DC part of ar9003_hw_init_cal().  Those trim the
 *     transmitter and the ADC; the AGC and noise-floor calibrations,
 *     which the RECEIVER cannot work without at all, are ported below
 *     in ar9485_hw_init_cal().
 *   - ANI (adaptive noise immunity).  Pure sensitivity tuning on top of
 *     a working receiver.
 */

#include "hw_min.h"
#include "ar9485_initvals.h"

/* ar9003_calib.c:686 DELPT - the I/Q correction delta the transmit
 * calibration is told to use. */
#define AR9485_TX_IQCAL_DELPT   32

#define NDEBUG
#include <debug.h>

/* --------------------------------------------------------------------
 *  VERBATIM from hw.c:180
 * -------------------------------------------------------------------- */

void ath9k_hw_write_array(struct ath_hw *ah, const struct ar5416IniArray *array,
              int column, unsigned int *writecnt)
{
    int r;

    ENABLE_REGWRITE_BUFFER(ah);
    for (r = 0; r < (int)array->ia_rows; r++) {
        REG_WRITE(ah, INI_RA(array, r, 0),
              INI_RA(array, r, column));
        DO_DELAY(*writecnt);
    }
    REGWRITE_BUFFER_FLUSH(ah);
}

/* --------------------------------------------------------------------
 *  VERBATIM from hw.c:2029 (ath9k_hw_synth_delay)
 * -------------------------------------------------------------------- */

static void ath9k_hw_synth_delay(struct ath_hw *ah, struct ath9k_channel *chan,
                                 int hw_delay)
{
    UNREFERENCED_PARAMETER(ah);
    UNREFERENCED_PARAMETER(chan);

    hw_delay /= 10;

    /* Upstream scales by 2 / 4 for half- and quarter-rate channels; both
     * predicates are constant-false here (see the file header). */

    udelay(hw_delay + BASE_ACTIVATE_DELAY);
}

/* --------------------------------------------------------------------
 *  Initvals wiring - the AR9485_11_OR_LATER arm of upstream
 *  ar9003_hw_init_mode_regs() (ar9003_hw.c:124), with every other chip's
 *  arm dropped.
 *
 *  Two upstream tables are intentionally left unwired:
 *    - iniModesFastClock: 5 GHz only, unreachable on this part.
 *    - iniAdditional: upstream never assigns it in the AR9485 arm, so it
 *      stays {NULL,0,0} and ar9003_hw_prog_ini()/REG_WRITE_ARRAY skip it.
 *      We zero the whole ath_hw at attach, so that holds here too.
 * -------------------------------------------------------------------- */

static void ar9485_hw_init_mode_regs(struct ath_hw *ah)
{
    /* mac */
    INIT_INI_ARRAY(&ah->iniMac[ATH_INI_CORE], ar9485_1_1_mac_core);
    INIT_INI_ARRAY(&ah->iniMac[ATH_INI_POST], ar9485_1_1_mac_postamble);

    /* bb */
    INIT_INI_ARRAY(&ah->iniBB[ATH_INI_PRE], ar9485_1_1);
    INIT_INI_ARRAY(&ah->iniBB[ATH_INI_CORE], ar9485_1_1_baseband_core);
    INIT_INI_ARRAY(&ah->iniBB[ATH_INI_POST], ar9485_1_1_baseband_postamble);

    /* radio */
    INIT_INI_ARRAY(&ah->iniRadio[ATH_INI_CORE], ar9485_1_1_radio_core);
    INIT_INI_ARRAY(&ah->iniRadio[ATH_INI_POST], ar9485_1_1_radio_postamble);

    /* soc */
    INIT_INI_ARRAY(&ah->iniSOC[ATH_INI_PRE], ar9485_1_1_soc_preamble);

    /* rx/tx gain */
    INIT_INI_ARRAY(&ah->iniModesRxGain, ar9485Common_wo_xlna_rx_gain_1_1);
    INIT_INI_ARRAY(&ah->iniModesTxGain, ar9485_modes_lowest_ob_db_tx_gain_1_1);

    /* Japan 2484 MHz CCK */
    INIT_INI_ARRAY(&ah->iniCckfirJapan2484,
                   ar9485_1_1_baseband_core_txfir_coeff_japan_2484);

    /* PCIe SerDes.  ah->config.pll_pwrsave is left at 0 (upstream's
     * default for a PC-OEM card without the low-power quirk), which
     * selects the clkreq-disable table on both arms. */
    if (ah->config.pll_pwrsave & AR_PCIE_PLL_PWRSAVE_CONTROL) {
        INIT_INI_ARRAY(&ah->iniPcieSerdes,
                       ar9485_1_1_pll_on_cdr_on_clkreq_disable_L1);
        INIT_INI_ARRAY(&ah->iniPcieSerdesLowPower,
                       ar9485_1_1_pll_on_cdr_on_clkreq_disable_L1);
    } else {
        INIT_INI_ARRAY(&ah->iniPcieSerdes,
                       ar9485_1_1_pcie_phy_clkreq_disable_L1);
        INIT_INI_ARRAY(&ah->iniPcieSerdesLowPower,
                       ar9485_1_1_pcie_phy_clkreq_disable_L1);
    }
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_phy.c:820 (ar9003_hw_prog_ini)
 * -------------------------------------------------------------------- */

static void ar9003_hw_prog_ini(struct ath_hw *ah,
                               struct ar5416IniArray *iniArr,
                               int column)
{
    unsigned int i, regWrites = 0;

    /* New INI format: Array may be undefined (pre, core, post arrays) */
    if (!iniArr->ia_array)
        return;

    /*
     * New INI format: Pre, core, and post arrays for a given subsystem
     * may be modal (> 2 columns) or non-modal (2 columns). Determine if
     * the array is non-modal and force the column to 1.
     */
    if (column >= (int)iniArr->ia_columns)
        column = 1;

    for (i = 0; i < iniArr->ia_rows; i++) {
        u32 reg = INI_RA(iniArr, i, 0);
        u32 val = INI_RA(iniArr, i, column);

        REG_WRITE(ah, reg, val);

        DO_DELAY(regWrites);
    }
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_phy.c:938 (ar9003_hw_override_ini).
 *
 *  Trimmed: the AR9462/AR9565 GLB_SWREG block and the 25 MHz-clock SoC
 *  block (AR9340/9531/9550/9561) - other chips' SREV arms.
 * -------------------------------------------------------------------- */

static void ar9003_hw_override_ini(struct ath_hw *ah)
{
    u32 val;

    /*
     * Set the RX_ABORT and RX_DIS and clear it only after
     * RXE is set for MAC. This prevents frames with
     * corrupted descriptor status.
     */
    REG_SET_BIT(ah, AR_DIAG_SW, (AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT));

    /*
     * For AR9280 and above, there is a new feature that allows
     * Multicast search based on both MAC Address and Key ID. By default,
     * this feature is enabled. But since the driver is not using this
     * feature, we switch it off; otherwise multicast search based on
     * MAC addr only will fail.
     */
    val = REG_READ(ah, AR_PCU_MISC_MODE2) & (~AR_ADHOC_MCAST_KEYID_ENABLE);
    val |= AR_AGG_WEP_ENABLE_FIX |
           AR_AGG_WEP_ENABLE |
           AR_PCU_MISC_MODE2_CFP_IGNORE;
    REG_WRITE(ah, AR_PCU_MISC_MODE2, val);

    if (REG_READ(ah, AR_PHY_CL_CAL_CTL) & AR_PHY_CL_CAL_ENABLE)
        ah->enabled_cals |= TX_CL_CAL;
    else
        ah->enabled_cals &= ~TX_CL_CAL;
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_phy.c (ar9003_hw_set_chain_masks)
 * -------------------------------------------------------------------- */

static void ar9003_hw_set_chain_masks(struct ath_hw *ah, u8 rx, u8 tx)
{
    if (ah->caps.tx_chainmask == 5 || ah->caps.rx_chainmask == 5)
        REG_SET_BIT(ah, AR_PHY_ANALOG_SWAP,
                    AR_PHY_SWAP_ALT_CHAIN);

    REG_WRITE(ah, AR_PHY_RX_CHAINMASK, rx);
    REG_WRITE(ah, AR_PHY_CAL_CHAINMASK, rx);

    if ((ah->caps.hw_caps & ATH9K_HW_CAP_APM) && (tx == 0x7))
        tx = 3;

    REG_WRITE(ah, AR_SELFGEN_MASK, tx);
}

/* --------------------------------------------------------------------
 *  VERBATIM from hw.c (ath9k_hw_set11nmac2040)
 * -------------------------------------------------------------------- */

static void ath9k_hw_set11nmac2040(struct ath_hw *ah, struct ath9k_channel *chan)
{
    u32 macmode;

    if (IS_CHAN_HT40(chan) && !ah->config.cwm_ignore_extcca)
        macmode = AR_2040_JOINED_RX_CLEAR;
    else
        macmode = 0;

    REG_WRITE(ah, AR_2040_MODE, macmode);
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_phy.c:610 (ar9003_hw_set_channel_regs)
 * -------------------------------------------------------------------- */

static void ar9003_hw_set_channel_regs(struct ath_hw *ah,
                                       struct ath9k_channel *chan)
{
    u32 phymode;
    u32 enableDacFifo = 0;

    enableDacFifo =
        (REG_READ(ah, AR_PHY_GEN_CTRL) & AR_PHY_GC_ENABLE_DAC_FIFO);

    /* Enable 11n HT, 20 MHz */
    phymode = AR_PHY_GC_HT_EN | AR_PHY_GC_SHORT_GI_40 | enableDacFifo;

    /* Upstream guards this with !AR_SREV_9561(ah), which is true here. */
    phymode |= AR_PHY_GC_SINGLE_HT_LTF1;

    /* Upstream configures dynamic 20/40 here; HT40 is constant-false in
     * this driver, so the baseband stays in HT20. */

    /* make sure we preserve INI settings */
    phymode |= REG_READ(ah, AR_PHY_GEN_CTRL);
    /* turn off Green Field detection for STA for now */
    phymode &= ~AR_PHY_GC_GF_DETECT_EN;

    REG_WRITE(ah, AR_PHY_GEN_CTRL, phymode);

    /* Configure MAC for 20/40 operation */
    ath9k_hw_set11nmac2040(ah, chan);

    /* global transmit timeout (25 TUs default)*/
    REG_WRITE(ah, AR_GTXTO, 25 << AR_GTXTO_TIMEOUT_LIMIT_S);
    /* carrier sense timeout */
    REG_WRITE(ah, AR_CST, 0xF << AR_CST_TIMEOUT_LIMIT_S);
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_phy.c:964 (ar9003_hw_set_rfmode).
 *
 *  The 5 GHz, fast-clock and half/quarter-rate arms are constant-false,
 *  leaving the 2 GHz dynamic-mode write.
 * -------------------------------------------------------------------- */

static void ar9003_hw_set_rfmode(struct ath_hw *ah, struct ath9k_channel *chan)
{
    u32 rfMode = 0;

    if (chan == NULL)
        return;

    rfMode |= AR_PHY_MODE_DYNAMIC;

    REG_WRITE(ah, AR_PHY_MODE, rfMode);
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_phy.c (ar9003_hw_init_bb)
 * -------------------------------------------------------------------- */

static void ar9003_hw_init_bb(struct ath_hw *ah, struct ath9k_channel *chan)
{
    u32 synthDelay;

    /*
     * Wait for the frequency synth to settle (synth goes on
     * via AR_PHY_ACTIVE_EN).  Read the phy active delay register.
     * Value is in 100ns increments.
     */
    synthDelay = REG_READ(ah, AR_PHY_RX_DELAY) & AR_PHY_RX_DELAY_DELAY;

    /* Activate the PHY (includes baseband activate + synthesizer on) */
    REG_WRITE(ah, AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);
    ath9k_hw_synth_delay(ah, chan, synthDelay);
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_phy.c:149 (ar9003_hw_set_channel).
 *
 *  Only the 2 GHz fractional-mode arm survives, and within it only the
 *  AR9485 branch.  ath9k_hw_get_channel_centers() collapses to
 *  `synth_center == chan->channel' for an HT20 channel, so it is folded
 *  in rather than ported - the extension-channel arithmetic it exists
 *  for is HT40-only.
 * -------------------------------------------------------------------- */

static int ar9003_hw_set_channel(struct ath_hw *ah, struct ath9k_channel *chan)
{
    u16 bMode, fracMode = 0, aModeRefSel = 0;
    u32 freq, chan_frac, div, channelSel = 0, reg32 = 0;
    int loadSynthChannel;

    freq = chan->channel;

    /* 2 GHz, fractional mode.  ah->is_clk_25mhz is false for a PCIe
     * AR9485 (the 25 MHz arm is for SoC parts), giving div == 120. */
    if (ah->is_clk_25mhz)
        div = 75;
    else
        div = 120;

    channelSel = (freq * 4) / div;
    chan_frac = (((freq * 4) % div) * 0x20000) / div;
    channelSel = (channelSel << 17) | chan_frac;

    /* Set to 2G mode */
    bMode = 1;

    /* Enable fractional mode for all channels */
    fracMode = 1;
    aModeRefSel = 0;
    loadSynthChannel = 0;

    reg32 = (bMode << 29);
    REG_WRITE(ah, AR_PHY_SYNTH_CONTROL, reg32);

    /* Enable Long shift Select for Synthesizer */
    REG_RMW_FIELD(ah, AR_PHY_65NM_CH0_SYNTH4,
                  AR_PHY_SYNTH4_LONG_SHIFT_SELECT, 1);

    /* Program Synth. setting */
    reg32 = (channelSel << 2) | (fracMode << 30) |
        (aModeRefSel << 28) | (loadSynthChannel << 31);
    REG_WRITE(ah, AR_PHY_65NM_CH0_SYNTH7, reg32);

    /* Toggle Load Synth channel bit */
    loadSynthChannel = 1;
    reg32 = (channelSel << 2) | (fracMode << 30) |
        (aModeRefSel << 28) | (loadSynthChannel << 31);
    REG_WRITE(ah, AR_PHY_65NM_CH0_SYNTH7, reg32);

    ah->curchan = chan;

    return 0;
}

/* --------------------------------------------------------------------
 *  ADAPTED from ar9003_phy.c:855 (ar9003_hw_process_ini).
 *
 *  Trimmed, in upstream order:
 *   - ar9003_doubler_fix(): guarded by AR_SREV_9300/9580/9550, all false.
 *   - the AR9462_20_OR_LATER CUS217 / 5G-XLNA rx-gain blocks.
 *   - the AR9550/9561 rx-gain-bounds and AR9561 xlna blocks.
 *   - the AR9550/9531/9561 tx-gain index selection; the else-arm applies.
 *   - iniModesFastClock: 5 GHz only.
 *   - the AR9531 FCAL power-threshold tweak inside the Japan block.
 *
 *  ath9k_hw_apply_txpower() is present, at the tail, exactly where
 *  upstream puts it (ar9003_phy.c:962).
 *
 *  modesIndex is 4 for every channel this driver programs: 2 GHz, HT20.
 * -------------------------------------------------------------------- */

static int ar9003_hw_process_ini(struct ath_hw *ah, struct ath9k_channel *chan)
{
    unsigned int regWrites = 0, i;
    u32 modesIndex;

    /* 2 GHz, not HT40 */
    modesIndex = 4;

    /*
     * SOC, MAC, BB, RADIO initvals.
     */
    for (i = 0; i < ATH_INI_NUM_SPLIT; i++) {
        ar9003_hw_prog_ini(ah, &ah->iniSOC[i], modesIndex);
        ar9003_hw_prog_ini(ah, &ah->iniMac[i], modesIndex);
        ar9003_hw_prog_ini(ah, &ah->iniBB[i], modesIndex);
        ar9003_hw_prog_ini(ah, &ah->iniRadio[i], modesIndex);
    }

    /*
     * RXGAIN initvals.
     */
    REG_WRITE_ARRAY(&ah->iniModesRxGain, 1, regWrites);

    /*
     * TXGAIN initvals.
     */
    REG_WRITE_ARRAY(&ah->iniModesTxGain, modesIndex, regWrites);

    /*
     * Clock frequency initvals.  Unwired for AR9485; the call is kept so
     * the shape matches upstream and prog_ini's NULL guard handles it.
     */
    REG_WRITE_ARRAY(&ah->iniAdditional, 1, regWrites);

    /*
     * JAPAN regulatory.
     */
    if (chan->channel == 2484)
        ar9003_hw_prog_ini(ah, &ah->iniCckfirJapan2484, 1);

    ah->modes_index = modesIndex;
    ar9003_hw_override_ini(ah);
    ar9003_hw_set_channel_regs(ah, chan);
    ar9003_hw_set_chain_masks(ah, ah->rxchainmask, ah->txchainmask);

    /* Upstream's last statement in this function, and the reason the
     * transmit gain tables the block above just wrote are worth
     * anything: it turns the EEPROM's per-rate target powers and its
     * open-loop calibration piers into register values.  See
     * hw_txpower.c. */
    ath9k_hw_apply_txpower(ah, chan, false);

    return 0;
}

/* --------------------------------------------------------------------
 *  VERBATIM from ar9003_phy.c:593 (ar9003_hw_compute_pll_control)
 * -------------------------------------------------------------------- */

static u32 ar9003_hw_compute_pll_control(struct ath_hw *ah,
                                         struct ath9k_channel *chan)
{
    u32 pll;

    UNREFERENCED_PARAMETER(ah);
    UNREFERENCED_PARAMETER(chan);

    pll = SM(0x5, AR_RTC_9300_PLL_REFDIV);

    /* Upstream selects half/quarter rate here; both constant-false. */

    pll |= SM(0x2c, AR_RTC_9300_PLL_DIV);

    return pll;
}

/* --------------------------------------------------------------------
 *  ADAPTED from hw.c:761 (ath9k_hw_init_pll).
 *
 *  Only the AR_SREV_9485 arm survives; the AR9330, AR9340/9550/9531/9561
 *  and AR9271 arms are other chips.  The AR9565 `pll |= 0x40000' tweak is
 *  likewise dropped.
 * -------------------------------------------------------------------- */

static void ath9k_hw_init_pll(struct ath_hw *ah, struct ath9k_channel *chan)
{
    u32 pll;

    pll = ar9003_hw_compute_pll_control(ah, chan);

    /* program BB PLL ki and kd value, ki=0x4, kd=0x40 */
    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_PLL_PWD, 0x1);
    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_DPLL2_KD, 0x40);
    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_DPLL2_KI, 0x4);

    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_REFDIV, 0x5);
    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_NINI, 0x58);
    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_NFRAC, 0x0);

    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_OUTDIV, 0x1);
    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_LOCAL_PLL, 0x1);
    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_EN_NEGTRIG, 0x1);

    /* program BB PLL phase_shift to 0x6 */
    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL3, AR_CH0_BB_DPLL3_PHASE_SHIFT, 0x6);

    REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_PLL_PWD, 0x0);
    udelay(1000);

    REG_WRITE(ah, AR_RTC_PLL_CONTROL(ah), pll);

    /* AR_SREV_9485 arm of upstream's post-write settle. */
    udelay(1000);

    udelay(RTC_PLL_SETTLE_DELAY);

    REG_WRITE(ah, AR_RTC_SLEEP_CLK(ah), AR_RTC_FORCE_DERIVED_CLK);
}

/* --------------------------------------------------------------------
 *  ADAPTED from hw.c:1391 (ath9k_hw_set_reset).
 *
 *  Trimmed: the AR9100 derived-clock preamble and rst_flags arm, the
 *  AR9340 sync-cause mask, the AR9330 reset workaround, the MCI GPM
 *  offset check, and the AR9300/AR9580 DMA-halt workaround - all other
 *  chips' arms.  AR_SREV_9300_20_OR_LATER is true, so the AR_RC_AHB
 *  writes upstream guards with its negation are skipped, exactly as
 *  upstream would.
 * -------------------------------------------------------------------- */

static bool ath9k_hw_set_reset(struct ath_hw *ah, int type)
{
    u32 rst_flags;
    u32 tmpReg;

    ENABLE_REGWRITE_BUFFER(ah);

    REG_WRITE(ah, AR_WA(ah), ah->WARegVal);
    udelay(10);

    REG_WRITE(ah, AR_RTC_FORCE_WAKE(ah), AR_RTC_FORCE_WAKE_EN |
          AR_RTC_FORCE_WAKE_ON_INT);

    tmpReg = REG_READ(ah, AR_INTR_SYNC_CAUSE(ah));
    tmpReg &= AR_INTR_SYNC_LOCAL_TIMEOUT | AR_INTR_SYNC_RADM_CPL_TIMEOUT;

    if (tmpReg) {
        REG_WRITE(ah, AR_INTR_SYNC_ENABLE(ah), 0);
        REG_WRITE(ah, AR_RC, AR_RC_HOSTIF);
    }

    rst_flags = AR_RTC_RC_MAC_WARM;
    if (type == ATH9K_RESET_COLD)
        rst_flags |= AR_RTC_RC_MAC_COLD;

    REG_WRITE(ah, AR_RTC_RC(ah), rst_flags);

    REGWRITE_BUFFER_FLUSH(ah);

    udelay(50);

    REG_WRITE(ah, AR_RTC_RC(ah), 0);
    if (!ath9k_hw_wait(ah, AR_RTC_RC(ah), AR_RTC_RC_M, 0, AH_WAIT_TIMEOUT)) {
        ath_dbg(ath9k_hw_common(ah), RESET, "RTC stuck in MAC reset\n");
        return false;
    }

    REG_WRITE(ah, AR_RC, 0);

    return true;
}

/* --------------------------------------------------------------------
 *  ADAPTED from hw.c:1441 (ath9k_hw_set_reset_power_on).
 *
 *  Unlike the standalone ar9485_hw_power_on() in hw_chip.c - which
 *  deliberately stops after the RTC comes up because reading the OTP
 *  needs nothing more - this is the full upstream function including the
 *  closing warm reset that drops the MAC and baseband out of reset.
 * -------------------------------------------------------------------- */

static bool ath9k_hw_set_reset_power_on(struct ath_hw *ah)
{
    ENABLE_REGWRITE_BUFFER(ah);

    REG_WRITE(ah, AR_WA(ah), ah->WARegVal);
    udelay(10);

    REG_WRITE(ah, AR_RTC_FORCE_WAKE(ah), AR_RTC_FORCE_WAKE_EN |
          AR_RTC_FORCE_WAKE_ON_INT);

    REG_WRITE(ah, AR_RTC_RESET(ah), 0);

    REGWRITE_BUFFER_FLUSH(ah);

    udelay(2);

    REG_WRITE(ah, AR_RTC_RESET(ah), 1);

    if (!ath9k_hw_wait(ah,
               AR_RTC_STATUS(ah),
               AR_RTC_STATUS_M(ah),
               AR_RTC_STATUS_ON,
               AH_WAIT_TIMEOUT)) {
        ath_dbg(ath9k_hw_common(ah), RESET, "RTC not waking up\n");
        return false;
    }

    return ath9k_hw_set_reset(ah, ATH9K_RESET_WARM);
}

/* --------------------------------------------------------------------
 *  VERBATIM from hw.c:1481 (ath9k_hw_set_reset_reg)
 * -------------------------------------------------------------------- */

static bool ath9k_hw_set_reset_reg(struct ath_hw *ah, u32 type)
{
    bool ret = false;

    REG_WRITE(ah, AR_WA(ah), ah->WARegVal);
    udelay(10);

    REG_WRITE(ah, AR_RTC_FORCE_WAKE(ah),
          AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);

    if (!ah->reset_power_on)
        type = ATH9K_RESET_POWER_ON;

    switch (type) {
    case ATH9K_RESET_POWER_ON:
        ret = ath9k_hw_set_reset_power_on(ah);
        if (ret)
            ah->reset_power_on = true;
        break;
    case ATH9K_RESET_WARM:
    case ATH9K_RESET_COLD:
        ret = ath9k_hw_set_reset(ah, type);
        break;
    default:
        break;
    }

    return ret;
}

/* --------------------------------------------------------------------
 *  ADAPTED from hw.c:2177 (ath9k_hw_set_power_awake).
 *
 *  Trimmed: the AR9100 RTC_RESET set and mdelay, the pre-AR9300 PLL
 *  re-init, and the MCI power-awake hook.
 * -------------------------------------------------------------------- */

static bool ath9k_hw_set_power_awake(struct ath_hw *ah)
{
    u32 val;
    int i;

    /* Set Bits 14 and 17 of AR_WA(ah) before powering on the chip. */
    REG_WRITE(ah, AR_WA(ah), ah->WARegVal);
    udelay(10);

    if ((REG_READ(ah, AR_RTC_STATUS(ah)) &
         AR_RTC_STATUS_M(ah)) == AR_RTC_STATUS_SHUTDOWN) {
        if (!ath9k_hw_set_reset_reg(ah, ATH9K_RESET_POWER_ON)) {
            return false;
        }
    }

    REG_SET_BIT(ah, AR_RTC_FORCE_WAKE(ah), AR_RTC_FORCE_WAKE_EN);
    udelay(50);

    for (i = POWER_UP_TIME / 50; i > 0; i--) {
        val = REG_READ(ah, AR_RTC_STATUS(ah)) & AR_RTC_STATUS_M(ah);
        if (val == AR_RTC_STATUS_ON)
            break;
        udelay(50);
        REG_SET_BIT(ah, AR_RTC_FORCE_WAKE(ah), AR_RTC_FORCE_WAKE_EN);
    }
    if (i == 0) {
        ath_err(ath9k_hw_common(ah), "Failed to wakeup in %uus",
                POWER_UP_TIME / 20);
        return false;
    }

    REG_CLR_BIT(ah, AR_STA_ID1, AR_STA_ID1_PWR_SAV);

    return true;
}

/* --------------------------------------------------------------------
 *  VERBATIM from hw.c (ath9k_hw_init_qos)
 * -------------------------------------------------------------------- */

static void ath9k_hw_init_qos(struct ath_hw *ah)
{
    ENABLE_REGWRITE_BUFFER(ah);

    REG_WRITE(ah, AR_MIC_QOS_CONTROL, 0x100aa);
    REG_WRITE(ah, AR_MIC_QOS_SELECT, 0x3210);

    REG_WRITE(ah, AR_QOS_NO_ACK,
          SM(2, AR_QOS_NO_ACK_TWO_BIT) |
          SM(5, AR_QOS_NO_ACK_BIT_OFF) |
          SM(0, AR_QOS_NO_ACK_BYTE_OFF));

    REG_WRITE(ah, AR_TXOP_X, AR_TXOP_X_VAL);
    REG_WRITE(ah, AR_TXOP_0_3, 0xFFFFFFFF);
    REG_WRITE(ah, AR_TXOP_4_7, 0xFFFFFFFF);
    REG_WRITE(ah, AR_TXOP_8_11, 0xFFFFFFFF);
    REG_WRITE(ah, AR_TXOP_12_15, 0xFFFFFFFF);

    REGWRITE_BUFFER_FLUSH(ah);
}

/* AR9003 EDMA setup from ath9k_hw_set_dma().  The reset path clears these
 * registers, so programming only AR_DATABUF_SIZE in the receive worker is not
 * sufficient: without the DMA burst size, FIFO threshold and EDMA
 * backpressure thresholds, the MAC enters RX but never consumes LP_RXDP.
 *
 * The last two writes are the transmit half, and they are here rather than in
 * the miniport for a reason.  ath9k_hw_set_dma() runs from inside
 * ath9k_hw_reset(), before the baseband is activated; this port used to
 * program the status ring from AR9485ResetTransmitQueue() after the reset had
 * already returned.  On the ASUS X550DP the hardware then never wrote a single
 * status descriptor: the ring registers read back the address we gave them,
 * the frame went on the air (AR_TFCNT, AR_ACK_FAIL both moved), the QCU
 * reported TXEOL -- and AR_ISR_S0 never showed TXOK, and 288 bytes of ring
 * poisoned with 0xa5 came back untouched.  Programming the base at upstream's
 * point in the sequence is the one difference user-mode experiments could not
 * test, because a reset clears the registers on its way past.  See
 * docs/asus.txt. */
static void ar9485_hw_set_dma(struct ath_hw *ah)
{
    REG_RMW(ah, AR_TXCFG, AR_TXCFG_DMASZ_128B, AR_TXCFG_DMASZ_MASK);
    REG_RMW(ah, AR_RXCFG, AR_RXCFG_DMASZ_128B, AR_RXCFG_DMASZ_MASK);
    REG_WRITE(ah, AR_RXFIFO_CFG, 0x200);
    REG_RMW_FIELD(ah, AR_RXBP_THRESH, AR_RXBP_THRESH_HP, 0x1);
    REG_RMW_FIELD(ah, AR_RXBP_THRESH, AR_RXBP_THRESH_LP, 0x1);

    /* Usable PCU transmit buffer.  AR9485 takes the generic size; only
     * AR9285 and AR934x v1.3+ reduce it upstream. */
    REG_WRITE(ah, AR_PCU_TXBUF_CTRL, AR_PCU_TXBUF_CTRL_USABLE_SIZE);

    /* ath9k_hw_reset_txstatus_ring().  Zero until the miniport has allocated
     * the ring, which is the state during the very first bring-up. */
    if (ah->ts_paddr_start != 0)
    {
        REG_WRITE(ah, AR_Q_STATUS_RING_START, ah->ts_paddr_start);
        REG_WRITE(ah, AR_Q_STATUS_RING_END, ah->ts_paddr_end);
    }
}

/* --------------------------------------------------------------------
 *  Miniport-facing entry point.
 *
 *  Condenses the parts of upstream ath9k_hw_reset() (hw.c:1839) that a
 *  receive-capable bring-up needs, in upstream's order:
 *
 *      wake the chip  ->  reset  ->  PLL  ->  initvals  ->  synthesiser
 *      ->  RF mode  ->  QoS  ->  baseband activate
 *
 *  What upstream does in ath9k_hw_reset() that is NOT here: interrupt
 *  mask programming (the RX slice owns that), calibration, ANI, TX queue
 *  reset, and TX power.  See the file header for why each is deferred.
 * -------------------------------------------------------------------- */

/* --------------------------------------------------------------------
 *  AGC and noise-floor calibration -- the receive-side half of
 *  ar9003_hw_init_cal() (ar9003_calib.c) plus ath9k_hw_start_nfcal()
 *  (calib.c:389).
 *
 *  The header note above used to call this "the next slice" and predict
 *  merely reduced sensitivity.  That was wrong, and the bring-up lab
 *  proved it on the hardware (docs/asus.txt 8.3): uncalibrated, this PHY
 *  demodulates NOTHING.  AR_RFCNT stays flat at zero while the clock
 *  counter runs at 4.7 M/100 ms, and no frame ever reaches a DMA buffer.
 *  Driving this same sequence from user mode made real beacons land in
 *  the very next dwell.
 *
 *  It belongs here, at the end of every PHY bring-up, and not once at
 *  init: a channel change clears CLC_SUCCESS and AR_RFCNT goes flat
 *  again, and a scan sweep brings the PHY up thirteen times.
 *
 *  Only the AGC calibration is waited on.  Measured on the AR9485:
 *  780 us once warm, 7-8 ms for the first one after power-on.  The
 *  noise-floor calibration takes 12-140 ms and is deliberately started
 *  and left running -- upstream ath9k_hw_start_nfcal() does the same,
 *  and the lab confirmed reception works while it is still in progress.
 *  Waiting for it would cost ~1.7 s on every 13-channel sweep.
 * -------------------------------------------------------------------- */

static bool ar9485_hw_init_cal(struct ath_hw *ah)
{
    bool txiqcal_done = false;

    REG_SET_BIT(ah, AR_PHY_AGC_CONTROL(ah), AR_PHY_AGC_CONTROL_FLTR_CAL);

    /*
     * Arm the transmit I/Q calibration BEFORE the AGC calibration below.
     *
     * ar9003_hw_init_cal_settings() sets TX_IQ_CAL and, for the AR9485 and
     * later, TX_IQ_ON_AGC_CAL: on this part the transmit I/Q calibration is
     * not a step of its own, the hardware folds it into the AGC calibration.
     * So the whole of ar9003_hw_init_cal_pcoem()'s tx-iqcal block reduces to
     * these two writes plus reading the answer out afterwards.
     *
     * ah->caldata is not modelled, so upstream's "already calibrated on this
     * channel, reload instead" arm never applies and the enable is
     * unconditional.
     */
    REG_RMW_FIELD(ah, AR_PHY_TX_IQCAL_CONTROL_1(ah),
                  AR_PHY_TX_IQCAL_CONTROL_1_IQCORR_I_Q_COFF_DELPT,
                  AR9485_TX_IQCAL_DELPT);
    REG_SET_BIT(ah, AR_PHY_TX_IQCAL_CONTROL_0(ah),
                AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL);
    txiqcal_done = true;

    /* The noise-floor calibration must not be running while the AGC one
     * is: they share AR_PHY_AGC_CONTROL and upstream sequences them. */
    REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL(ah), AR_PHY_AGC_CONTROL_NF);
    REG_SET_BIT(ah, AR_PHY_AGC_CONTROL(ah), AR_PHY_AGC_CONTROL_CAL);

    if (!ath9k_hw_wait(ah, AR_PHY_AGC_CONTROL(ah), AR_PHY_AGC_CONTROL_CAL, 0,
                       AH_WAIT_TIMEOUT)) {
        DPRINT1("AR9485: AGC calibration timed out, AR_PHY_AGC_CONTROL=0x%08x\n",
                REG_READ(ah, AR_PHY_AGC_CONTROL(ah)));
        return false;
    }

    /* ar9003_hw_init_cal_pcoem() runs the peak-detector calibration
     * immediately after the AGC calibration completes, inside the same
     * `run_agc_cal' arm. */
    ar9003_hw_do_pcoem_manual_peak_cal(ah, ah->curchan);

    /* Now read the transmit I/Q result the AGC calibration produced and
     * program the correction coefficients.  Upstream does this after the
     * peak cal too, and only when the cal was actually armed. */
    if (txiqcal_done)
        ar9003_hw_tx_iq_cal_post_proc(ah);

    /* Start the noise-floor calibration and return; see the note above. */
    REG_SET_BIT(ah, AR_PHY_AGC_CONTROL(ah), AR_PHY_AGC_CONTROL_ENABLE_NF);
    REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL(ah), AR_PHY_AGC_CONTROL_NO_UPDATE_NF);
    REG_SET_BIT(ah, AR_PHY_AGC_CONTROL(ah), AR_PHY_AGC_CONTROL_NF);

    return true;
}

bool ar9485_hw_phy_bringup(_Inout_ struct ath_hw *ah, _In_ u16 channel_mhz)
{
    struct ath9k_channel *chan = &ah->channel_store;

    chan->channel = channel_mhz;
    chan->channelFlags = 0;
    ah->curchan = chan;

    /* A 1x1 part: one receive chain, one transmit chain. */
    ah->rxchainmask = 1;
    ah->txchainmask = 1;
    ah->caps.rx_chainmask = 1;
    ah->caps.tx_chainmask = 1;

    /* Latched by __ath9k_hw_init() upstream; AR_WA is unreadable while
     * the chip sleeps, so every reset replays this copy. */
    ah->WARegVal = REG_READ(ah, AR_WA(ah));
    if (ah->WARegVal == AR9485_REG_OUT_OF_RANGE) {
        DPRINT1("AR9485: BAR0 window too small to reach AR_WA at 0x%05x\n",
                AR_WA(ah));
        return false;
    }
    ah->WARegVal |= (AR_WA_D3_L1_DISABLE | AR_WA_ASPM_TIMER_BASED_DISABLE);

    ar9485_hw_init_mode_regs(ah);

    if (!ath9k_hw_set_power_awake(ah)) {
        DPRINT1("AR9485: chip will not wake up\n");
        return false;
    }

    /* reset_power_on is false on the first call, so this performs the
     * full power-on reset and then the warm reset that releases the MAC
     * and baseband. */
    if (!ath9k_hw_set_reset_reg(ah, ATH9K_RESET_WARM)) {
        DPRINT1("AR9485: reset failed\n");
        return false;
    }

    ath9k_hw_init_pll(ah, chan);

    if (ar9003_hw_process_ini(ah, chan) != 0) {
        DPRINT1("AR9485: initvals programming failed\n");
        return false;
    }

    ar9003_hw_set_channel(ah, chan);
    ar9003_hw_set_rfmode(ah, chan);

    /*
     * Apply the board's own analog configuration from the EEPROM.
     * ath9k_hw_reset() (hw.c:1986) does this right after set_rfmode and
     * before it programs the synthesiser; this driver programs the
     * synthesiser earlier, so the call sits directly after set_rfmode,
     * which is the ordering constraint that matters -- every register it
     * touches is one the initvals have already written and the reset has
     * not since cleared.
     *
     * Without this the transmit chain runs on generic initval defaults:
     * no PA bias, no RF switch table, no attenuation.  See hw_board.c.
     */
    if (ah->eeprom_valid)
        ath9k_hw_ar9300_set_board_values(ah, chan);
    else
        DPRINT1("AR9485: no EEPROM image; board values not applied and the "
                "transmitter will run on initval defaults\n");

    /* ath9k_hw_reset: ah->misc_mode.  MIC_NEW_LOC puts a TKIP entry's
     * Michael keys in entry+64 (the layout AR9485SetCipherKey writes);
     * the key search per aggregate subframe is the AR9300+ default. */
    REG_SET_BIT(ah, AR_PCU_MISC,
                AR_PCU_MIC_NEW_LOC_ENA | AR_PCU_ALWAYS_PERFORM_KEYSEARCH);

    ath9k_hw_init_qos(ah);
    ar9485_hw_set_dma(ah);
    ar9003_hw_init_bb(ah, chan);

    if (!ar9485_hw_init_cal(ah))
        return false;

    DPRINT1("AR9485: PHY up on channel %u MHz; AR_PHY_ACTIVE=0x%08x "
            "AR_PHY_GEN_CTRL=0x%08x AR_RTC_STATUS=0x%08x "
            "AR_PHY_AGC_CONTROL=0x%08x\n",
            channel_mhz,
            REG_READ(ah, AR_PHY_ACTIVE),
            REG_READ(ah, AR_PHY_GEN_CTRL),
            REG_READ(ah, AR_RTC_STATUS(ah)),
            REG_READ(ah, AR_PHY_AGC_CONTROL(ah)));

    return true;
}

/* --------------------------------------------------------------------
 *  Opaque wrappers for the NDIS side.
 *
 *  ar9485.h deliberately does not include hw_min.h: the ath9k headers
 *  pull in the whole AR9003 register map and the Linux type shim, and
 *  every miniport translation unit would then carry that surface.  The
 *  miniport instead allocates ar9485_hw_context_size() bytes and passes
 *  the blob back in, so struct ath_hw stays private to ath9k/.
 * -------------------------------------------------------------------- */

SIZE_T
ar9485_hw_context_size(void)
{
    return sizeof(struct ath_hw);
}

bool
ar9485_hw_context_init(_Out_ void *hwctx,
                       _In_ void *bar0_base,
                       _In_ u32 bar0_len,
                       _In_ u16 devid,
                       _In_ u32 macVersion,
                       _In_ u16 macRev)
{
    struct ath_hw *ah = (struct ath_hw *)hwctx;

    /* The miniport allocates this with NdisAllocateMemoryWithTagPriority,
     * which does not zero, so nothing in *ah may be read before the
     * attach below clears it. */
    ar9485_hw_attach(ah, bar0_base, bar0_len, devid, macVersion, macRev);

    return ar9485_hw_eeprom_restore(ah);
}

/*
 * Publish the transmit status ring.  Called once, when the miniport has
 * allocated it; every later reset re-programs the registers from here via
 * ar9485_hw_set_dma().  Byte length rather than an entry count, so this file
 * need not pull in the descriptor layout from mac_desc.h -- the miniport has
 * already worked it out.
 */
void
ar9485_hw_set_txstatus_ring(_Inout_ void *hwctx,
                            _In_ u32 ts_paddr_start,
                            _In_ u32 ring_bytes)
{
    struct ath_hw *ah = (struct ath_hw *)hwctx;

    ah->ts_paddr_start = ts_paddr_start;
    ah->ts_paddr_end = ts_paddr_start + ring_bytes;
}

bool
ar9485_hw_start(_Out_ void *hwctx,
                _In_ void *bar0_base,
                _In_ u32 bar0_len,
                _In_ u16 devid,
                _In_ u32 macVersion,
                _In_ u16 macRev,
                _In_ u16 channel_mhz)
{
    struct ath_hw *ah = (struct ath_hw *)hwctx;
    struct ar9300_eeprom *saved;
    bool had_eeprom;
    u32 ts_paddr_start, ts_paddr_end;

    /*
     * ar9485_hw_attach() zeroes the whole ath_hw, and a scan sweep calls
     * this thirteen times.  The EEPROM image costs a byte-at-a-time walk
     * of the OTP to recover, and the board values now need it on every
     * bring-up, so carry it across the reattach instead of re-reading it
     * per channel.  ar9485_hw_context_init() must have run first; if it
     * did not, eeprom_valid is false and the bring-up says so.
     */
    had_eeprom = ah->eeprom_valid;
    saved = NULL;

    /* Same reason as the EEPROM below: ar9485_hw_attach() zeroes the context,
     * and losing the ring base here would put us straight back to a MAC that
     * transmits and reports nothing. */
    ts_paddr_start = ah->ts_paddr_start;
    ts_paddr_end = ah->ts_paddr_end;

    if (had_eeprom)
    {
        saved = (struct ar9300_eeprom *)
            ExAllocatePoolWithTag(NonPagedPool, sizeof(*saved),
                                  AR9485_COMPAT_TAG);
        if (saved != NULL)
            RtlCopyMemory(saved, &ah->eeprom.ar9300_eep, sizeof(*saved));
    }

    ar9485_hw_attach(ah, bar0_base, bar0_len, devid, macVersion, macRev);

    ah->ts_paddr_start = ts_paddr_start;
    ah->ts_paddr_end = ts_paddr_end;

    if (saved != NULL)
    {
        RtlCopyMemory(&ah->eeprom.ar9300_eep, saved, sizeof(*saved));
        ah->eeprom_valid = true;
        ExFreePoolWithTag(saved, AR9485_COMPAT_TAG);
    }
    else if (had_eeprom)
    {
        /* Could not park the image over the attach, so pay for the OTP
         * walk again rather than bring the transmitter up unconfigured. */
        ar9485_hw_eeprom_restore(ah);
    }

    return ar9485_hw_phy_bringup(ah, channel_mhz);
}
