/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Minimal subset of Linux ath9k hw.h sufficient to compile the
 *              verbatim ath9k_hw_read_revisions() body in hw_chip.c.
 *
 *              The upstream `struct ath_hw` carries ~200 fields covering
 *              channels, EEPROM unions, calibration data, antenna combiner
 *              state, mac80211 hooks, and so on.  Pulling all of that in
 *              for the chip-identification slice is wasteful and forces a
 *              dozen sub-headers to be ported in lock-step.  The stub
 *              below carries only the fields ath9k_hw_read_revisions()
 *              touches; future slices grow it as needed.
 *
 *              The field names / types match the upstream hw.h verbatim
 *              so the imported function body compiles unchanged.
 */

#ifndef _AR9485_ATH9K_HW_MIN_H_
#define _AR9485_ATH9K_HW_MIN_H_

#include "../linux-compat.h"
#include "ath_reg.h"
#include "reg.h"
#include "ar9003_phy.h"
#include "ar9003_eeprom_min.h"

/* Verbatim copy of include/ath9k/hw.h:524 - struct ath9k_hw_version. */
typedef enum { ATH_USB_UNUSED = 0 } ath_usb_dev;
struct ath9k_hw_version {
    u32 magic;
    u16 devid;
    u16 subvendorid;
    u32 macVersion;
    u16 macRev;
    u16 phyRev;
    u16 analog5GhzRev;
    u16 analog2GhzRev;
    ath_usb_dev usbdev;
};

/* Subset of struct ath_ops from include/ath.h:127 - only the two function
 * pointers ath9k_hw_read_revisions exercises via REG_READ. */
struct ath_ops {
    unsigned int (*read)(void *, u32 reg_offset);
    void         (*write)(void *, u32 val, u32 reg_offset);
};

/* Verbatim upstream calib.h:30 - a register initialisation table.  The
 * tables themselves live in ar9485_initvals.h as `const u32 [][N]'; the
 * cast to `u32 *' in INIT_INI_ARRAY below is upstream's, and INI_RA does
 * the row/column arithmetic by hand. */
struct ar5416IniArray {
    u32 *ia_array;
    u32 ia_rows;
    u32 ia_columns;
};

/* Verbatim upstream calib.h:42 and hw.h:117. */
#define INIT_INI_ARRAY(iniarray, array) do {            \
        (iniarray)->ia_array = (u32 *)(array);          \
        (iniarray)->ia_rows = ARRAY_SIZE(array);        \
        (iniarray)->ia_columns = ARRAY_SIZE(array[0]);  \
    } while (0)

#define INI_RA(iniarray, row, column) \
    (((iniarray)->ia_array)[(row) * ((iniarray)->ia_columns) + (column)])

/* Upstream ar9003_phy.c splits every subsystem's initvals into a pre /
 * core / post triple (hw.h:1055, enum ath_ini_subsys). */
#define ATH_INI_PRE          0
#define ATH_INI_CORE         1
#define ATH_INI_POST         2
#define ATH_INI_NUM_SPLIT    3

/* Upstream hw.h:1030 - reset granularity. */
#define ATH9K_RESET_POWER_ON 0
#define ATH9K_RESET_WARM     1
#define ATH9K_RESET_COLD     2

/* Upstream hw.h:98 - wake-up budget, microseconds. */
#define POWER_UP_TIME        10000
/* Upstream hw.h:180 - PLL settle, microseconds. */
#define RTC_PLL_SETTLE_DELAY 1000

/* --------------------------------------------------------------------
 *  Channel description.
 *
 *  Upstream carries a fat struct ath9k_channel that points back into
 *  mac80211's ieee80211_channel for the band and bandwidth.  The AR9485
 *  is a single-band 2.4 GHz 1x1 802.11n part, so for this driver the
 *  band predicates are compile-time constants and the HT40 ones are
 *  constant-false: we bring the radio up HT20-only.  Keeping the
 *  upstream macro spellings means the imported function bodies compile
 *  unchanged and the dead arms fold away at build time.
 *
 *  Making HT40 real later means giving channelFlags actual bits and
 *  turning IS_CHAN_HT40/IS_CHAN_HT40PLUS into tests on them; every
 *  upstream call site that needs them is already present below.
 * -------------------------------------------------------------------- */
struct ath9k_channel {
    u16 channel;        /* centre frequency in MHz */
    u32 channelFlags;
};

#define IS_CHAN_2GHZ(_c)            (true)
#define IS_CHAN_5GHZ(_c)            (false)
#define IS_CHAN_HT40(_c)            (false)
#define IS_CHAN_HT40PLUS(_c)        (false)
#define IS_CHAN_HALF_RATE(_c)       (false)
#define IS_CHAN_QUARTER_RATE(_c)    (false)
#define IS_CHAN_A_FAST_CLOCK(_ah, _c) (false)

/* Upstream hw.h - only the capability bits the imported bodies test. */
#define ATH9K_HW_CAP_APM            BIT(4)

/* Upstream hw.h:206 - ah->config.pll_pwrsave bits.  Left at 0 for a
 * PC-OEM AR9485, which selects the clkreq-disable SerDes table. */
#define AR_PCIE_PLL_PWRSAVE_CONTROL BIT(0)
#define AR_PCIE_PLL_PWRSAVE_ON_D3   BIT(1)
#define AR_PCIE_PLL_PWRSAVE_ON_D0   BIT(2)
#define AR_PCIE_CDR_PWRSAVE_ON_D3   BIT(3)
#define AR_PCIE_CDR_PWRSAVE_ON_D0   BIT(4)

/* Upstream hw.h:164 - PHY activation settle floor, microseconds. */
#define BASE_ACTIVATE_DELAY         100

/* Upstream hw.h:175,176 - transmit power ceilings.  Both are in the
 * hardware's "twice dBm" (half-dB) units: 63 == 31.5 dBm per rate, 254
 * == 127 dBm combined.  Used by the Phase 2b transmit-power path. */
#define MAX_RATE_POWER              63
#define MAX_COMBINED_POWER          254 /* 128 dBm, chosen to fit in u8 */

/* Upstream hw.h:763 - the calibration kinds ar9003_hw_override_ini()
 * latches into ah->enabled_cals. */
#define TX_IQ_CAL           BIT(0)
#define TX_IQ_ON_AGC_CAL    BIT(1)
#define TX_CL_CAL           BIT(2)

/* Upstream funnels ath_dbg through a level-filtered printk.  Reset-path
 * messages are cold and worth having on COM1/KDNET during bring-up, so
 * they land in DbgPrint; the level token is dropped unevaluated. */
#define ath_dbg(_common, _level, _fmt, ...) \
    DbgPrint("ath9k_hw: " _fmt, ##__VA_ARGS__)


/* Minimal stand-in for struct ath_hw.  Field names match upstream so the
 * verbatim function bodies compile.  Carries only what slices 2-3 need. */
struct ath_hw {
    struct ath_ops reg_ops;
    void          *reg_ctx;          /* opaque cookie passed to read/write */
    u32            reg_len;          /* mapped BAR0 length, see note below */
    bool           reg_range_warned; /* one-shot latch for the guard below */
    struct ath9k_hw_version hw_version;
    bool           is_pciexpress;
    u16          (*get_mac_revision)(void);

    /* Upstream hw.h:820 - stashed AR_WA value.  ath9k reads AR_WA once in
     * __ath9k_hw_init() and replays it from this copy on every reset,
     * because the register cannot be read while the chip is asleep. */
    u32            WARegVal;

    /* Upstream hw.h:947 - `union ath9k_eeprom' narrowed to the one member
     * an AR9300-family part uses.  Restored by slice 3 (hw_eeprom.c). */
    union {
        struct ar9300_eeprom ar9300_eep;
    } eeprom;

    /* Set once the union above holds a real image.  ar9485_hw_attach()
     * zeroes the whole ath_hw and every channel change reattaches, so
     * ar9485_hw_start() saves and restores both across the call rather
     * than re-walking the OTP on each hop. */
    bool           eeprom_valid;

    /* ---- Phase 3b: PHY/MAC bring-up state -------------------------
     * Register initialisation tables, wired by ar9485_hw_init_mode_regs()
     * from the AR9485 1.1 arrays in ar9485_initvals.h.  Field names match
     * upstream hw.h so the imported ar9003_hw_process_ini() body compiles
     * unchanged. */
    struct ar5416IniArray iniMac[ATH_INI_NUM_SPLIT];
    struct ar5416IniArray iniBB[ATH_INI_NUM_SPLIT];
    struct ar5416IniArray iniRadio[ATH_INI_NUM_SPLIT];
    struct ar5416IniArray iniSOC[ATH_INI_NUM_SPLIT];
    struct ar5416IniArray iniModesRxGain;
    struct ar5416IniArray iniModesTxGain;
    struct ar5416IniArray iniCckfirJapan2484;
    struct ar5416IniArray iniAdditional;
    struct ar5416IniArray iniPcieSerdes;
    struct ar5416IniArray iniPcieSerdesLowPower;

    u32            modes_index;
    struct ath9k_channel *curchan;
    /* Backing store for curchan; upstream's channel array is owned by
     * mac80211's band description, which has no counterpart here. */
    struct ath9k_channel  channel_store;

    /* Upstream hw.h:824 - latched so a warm reset is only attempted once
     * a power-on reset has actually succeeded. */
    bool           reset_power_on;
    bool           is_clk_25mhz;

    u8             txchainmask;
    u8             rxchainmask;
    u32            enabled_cals;

    struct {
        u8  tx_chainmask;
        u8  rx_chainmask;
        u32 hw_caps;
    } caps;

    /* Upstream ath9k.h:51 enum ath9k_ant_div_comb_lna_conf.  Only the two
     * values ar9003_hw_ant_ctrl_apply() programs are needed. */

    struct {
        int cwm_ignore_extcca;
        u32 pll_pwrsave;
    } config;

    /* Upstream hw.h:944 - the AR9003 EDMA transmit status ring, published by
     * the miniport once and re-programmed from ar9485_hw_set_dma() on every
     * reset, exactly where ath9k_hw_set_dma() calls
     * ath9k_hw_reset_txstatus_ring().  Zero means "no ring", which is what
     * the reset leaves behind and what the hardware sees if nobody
     * republishes it. */
    u32            ts_paddr_start;
    u32            ts_paddr_end;
};

/* Bounds guard on register access - a shim-layer addition with no
 * upstream counterpart.
 *
 * Linux maps the whole BAR with pci_iomap() and never range-checks, but
 * on Windows the mapping is exactly MmMapIoSpace(IoAddress, IoLength) and
 * a read past its end faults the kernel.  That matters here because the
 * OTP window this phase touches lives at 0x14000-0x15f1f, far above the
 * 0x0000-0x8xxx range slice 2 used, and the AR9485 BAR0 size is a PnP
 * property we have not yet observed on the target machine.  reg_len lets
 * ar9485_reg_read()/ar9485_reg_write() refuse an out-of-window access and
 * log it instead of bugchecking.  The log is latched to one line per
 * ath_hw: ath9k_hw_wait() polls a register up to 10000 times, and a
 * DPRINT1 per poll would add minutes to a boot on the serial port. */
#define ATH_ANT_DIV_COMB_LNA2       1
#define ATH_ANT_DIV_COMB_LNA1       2

/* Upstream hw.h:417 - the transmit I/Q calibration reports one set of
 * coefficients per calibrated gain, up to this many. */
#define MAX_IQCAL_MEASUREMENT       8

#define AR9485_REG_OUT_OF_RANGE     0xFFFFFFFFu

/* REG_READ / REG_WRITE - copy of hw.h:80,82 macros.  Upstream hands the
 * struct ath_hw itself to the accessors as the opaque cookie; slice 3
 * restored that so ar9485_reg_read() can reach reg_len for its bounds
 * check.  The mapped BAR base lives in ah->reg_ctx. */
#define REG_READ(_ah, _reg) \
    (_ah)->reg_ops.read((_ah), (_reg))
#define REG_WRITE(_ah, _reg, _val) \
    (_ah)->reg_ops.write((_ah), (_val), (_reg))

/* Upstream routes REG_RMW through a bus-op so the AHB parts can batch it;
 * on PCIe it is a plain read-modify-write, which is what we provide.
 * Declared here, defined in hw_chip.c beside the other reg_ops. */
u32 ar9485_reg_rmw(struct ath_hw *ah, u32 reg, u32 set, u32 clr);

#define REG_RMW(_ah, _reg, _set, _clr) \
    ar9485_reg_rmw((_ah), (_reg), (_set), (_clr))
#define REG_RMW_FIELD(_a, _r, _f, _v) \
    REG_RMW(_a, _r, (((_v) << _f##_S) & _f), (_f))
#define REG_READ_FIELD(_a, _r, _f) \
    (((REG_READ(_a, _r) & _f) >> _f##_S))
#define REG_SET_BIT(_a, _r, _f) \
    REG_RMW(_a, _r, (_f), 0)
#define REG_CLR_BIT(_a, _r, _f) \
    REG_RMW(_a, _r, 0, (_f))

/* Upstream buffers register writes on AHB-attached SoC parts.  The AR9485
 * is PCIe and upstream's PCI bus-ops leave these hooks NULL, so the
 * macros are genuine no-ops here, not a simplification. */
#define ENABLE_REGWRITE_BUFFER(_ah)  do { } while (0)
#define REGWRITE_BUFFER_FLUSH(_ah)   do { } while (0)

/* Upstream hw.h:132.  The USB arm is unreachable for a PCIe part. */
#define DO_DELAY(x) do {                    \
        if (((++(x) % 64) == 0))            \
            udelay(1);                      \
    } while (0)

#define REG_WRITE_ARRAY(iniarray, column, regWr) \
    ath9k_hw_write_array(ah, iniarray, column, &(regWr))

/* Verbatim upstream hw.h:177,179 - ath9k_hw_wait()'s poll granularity and
 * the generic register-settle timeout, both in microseconds. */
#define AH_WAIT_TIMEOUT             100000 /* (us) */
#define AH_TIME_QUANTUM             10

/* MS / SM - copy of hw.h:122,121 macros (mask-shift / shift-mask). */
#define MS(_v, _f) (((_v) & (_f)) >> _f##_S)
#define SM(_v, _f) (((_v) << _f##_S) & (_f))

/* Device IDs - copy of hw.h:35-72.  Slice 2 cares only about AR9485 but
 * we copy a few neighbours for documentation and to make the upstream
 * function body compile without trimming the devid switch. */
#define AR5416_AR9100_DEVID          0x000b
#define AR9300_DEVID_AR9300          0x0030
#define AR9300_DEVID_AR9340          0x0031
#define AR9300_DEVID_AR9485_PCIE     0x0032
#define AR9300_DEVID_AR9580          0x0033
#define AR9300_DEVID_AR9462          0x0034
#define AR9300_DEVID_AR9330          0x0035
#define AR9300_DEVID_QCA955X         0x0038
#define AR9300_DEVID_AR9565          0x0036
#define AR9300_DEVID_AR953X          0x003d
#define AR9300_DEVID_QCA956X         0x003f

/* Family-check macros (AR_SREV_9100, AR_SREV_9340, AR_SREV_9462,
 * AR_SREV_9485, AR_SREV_9565, ...) and the AR_SREV(_ah) offset macro
 * come from the verbatim ath9k/reg.h dropped in slice 1.  They depend
 * on ah->hw_version.macVersion which our struct ath_hw above provides
 * with the same field name and type - so the upstream macros expand
 * without modification. */

/* ath_err logging stub - upstream funnels into a printk-like macro; we
 * land in DPRINT1 so failures show up on COM1. */
#define ath9k_hw_common(_ah)  (_ah)
struct _ath_common_dummy { int unused; };
#define ath_err(_common, _fmt, ...) \
    DbgPrint("ath9k_hw: " _fmt "\n", ##__VA_ARGS__)

/* Wire a caller-supplied struct ath_hw to a mapped BAR0.  Shared by every
 * slice so the reg_ops bridge lives in exactly one place. */
void ar9485_hw_attach(_Out_ struct ath_hw *ah,
                      _In_ void *bar0_base,
                      _In_ u32 bar0_len,
                      _In_ u16 devid,
                      _In_ u32 macVersion,
                      _In_ u16 macRev);

/* Verbatim upstream hw.c:60 - poll a register field with a us timeout. */
bool ath9k_hw_wait(struct ath_hw *ah, u32 reg, u32 mask, u32 val, u32 timeout);

/* Slice 2 entry point exposed to the miniport side.  Builds a tiny ath_hw
 * on the caller's stack, runs the verbatim revision-read, and returns the
 * decoded macVersion / macRev for the AR9485 chip the miniport bound to.
 */
bool ar9485_read_revisions(_In_ void *bar0_base,
                           _In_ u32 bar0_len,
                           _In_ u16 devid,
                           _Out_ u32 *out_macVersion,
                           _Out_ u16 *out_macRev,
                           _Out_ bool *out_is_pciexpress);

/* Slice 3a: bring the RTC domain out of reset so the OTP/EEPROM state
 * machine answers.  Derived from upstream ath9k_hw_set_reset_power_on();
 * see hw_chip.c for exactly what was left out and why. */
bool ar9485_hw_power_on(_In_ void *bar0_base,
                        _In_ u32 bar0_len,
                        _In_ u16 devid,
                        _In_ u32 macVersion,
                        _In_ u16 macRev);

/* Slice 3b: restore the EEPROM image out of the card's EEPROM or OTP and
 * hand back the permanent MAC address.  Returns false - having already
 * DPRINT1'd the reason - if the media cannot be read, no block passes its
 * checksum, or the recovered address is not a usable unicast MAC.  There
 * is deliberately no fallback address. */
bool ar9485_hw_eeprom_get_macaddr(_In_ void *bar0_base,
                                  _In_ u32 bar0_len,
                                  _In_ u16 devid,
                                  _In_ u32 macVersion,
                                  _In_ u16 macRev,
                                  _Out_writes_bytes_(6) u8 *out_macaddr,
                                  _Out_ u8 *out_eepromVersion,
                                  _Out_ u8 *out_templateVersion);

/* Verbatim upstream hw.c:180 - blast one initvals table at the chip. */
void ath9k_hw_write_array(struct ath_hw *ah, const struct ar5416IniArray *array,
                          int column, unsigned int *writecnt);

/* --------------------------------------------------------------------
 *  Phase 3b entry point: full PHY/MAC bring-up.
 *
 *  Runs the upstream reset sequence (power-on / warm reset, PLL, the
 *  AR9485 1.1 initvals, synthesiser programming for `channel_mhz', and
 *  baseband activation) against a caller-supplied ath_hw.  On success
 *  the baseband is running and parked on the requested 2.4 GHz channel,
 *  which is the precondition for the receive path.
 *
 *  The ath_hw must outlive the call - the RX path reads ah->curchan -
 *  so the miniport keeps it in its adapter extension rather than on the
 *  stack the way the earlier identification slices did.
 * -------------------------------------------------------------------- */
bool ar9485_hw_phy_bringup(_Inout_ struct ath_hw *ah, _In_ u16 channel_mhz);

/* --------------------------------------------------------------------
 *  Board configuration and the transmit half of the calibration, added
 *  when association was found to transmit without ever being answered.
 *  hw_board.c / hw_calib_tx.c.
 * -------------------------------------------------------------------- */

/* Apply every EEPROM-derived analog setting for `chan' - PA bias, RF
 * switch table, attenuation, internal regulator, tuning caps. */
void ath9k_hw_ar9300_set_board_values(struct ath_hw *ah,
                                      struct ath9k_channel *chan);

/* Read the transmit I/Q calibration the AGC calibration produced and
 * program the correction coefficients.  Must run after the AGC cal. */
void ar9003_hw_tx_iq_cal_post_proc(struct ath_hw *ah);

/* Manual peak-detector calibration; runs after the AGC cal. */
void ar9003_hw_do_pcoem_manual_peak_cal(struct ath_hw *ah,
                                        struct ath9k_channel *chan);

/* Which receive gain table the EEPROM selects (bits 3:0 of txrxgain). */
s32 ar9003_hw_get_rx_gain_idx(struct ath_hw *ah);

/* --------------------------------------------------------------------
 *  Phase 2b: transmit power and the open-loop power-control calibration
 *  apply.  hw_txpower.c.
 *
 *  Programs the per-rate target powers out of the EEPROM into
 *  AR_PHY_POWER_TX_RATE*, the self-generated (ACK/CTS) power into
 *  AR_TPC, and the pier-interpolated open-loop gain delta / thermal
 *  compensation into AR_PHY_TPC_11_B0 / _6_B0 / _18 / _19.  Called from
 *  the tail of ar9003_hw_process_ini(), where upstream calls it.
 *
 *  `test' is upstream's "compute but do not write" flag; there is no
 *  caller for it here and it is ignored.  Requires ah->eeprom_valid.
 * -------------------------------------------------------------------- */
void ath9k_hw_apply_txpower(struct ath_hw *ah, struct ath9k_channel *chan,
                            bool test);

/* Restore the AR9300 EEPROM/OTP image into ah->eeprom.  Expensive - it
 * walks the OTP a byte at a time - so the miniport does it once through
 * ar9485_hw_context_init() and ar9485_hw_start() preserves the result
 * across the reattach that each channel change performs. */
bool ar9485_hw_eeprom_restore(struct ath_hw *ah);

/* Opaque-context wrappers used by the miniport; see hw_reset.c. */
SIZE_T ar9485_hw_context_size(void);

/* One-time initialisation of the miniport's hardware context: attach and
 * read the EEPROM.  Must be called once, on freshly allocated (and
 * therefore uninitialised) storage, before the first ar9485_hw_start(). */
bool ar9485_hw_context_init(_Out_ void *hwctx, _In_ void *bar0_base,
                            _In_ u32 bar0_len, _In_ u16 devid,
                            _In_ u32 macVersion, _In_ u16 macRev);
bool ar9485_hw_start(_Out_ void *hwctx, _In_ void *bar0_base, _In_ u32 bar0_len,
                     _In_ u16 devid, _In_ u32 macVersion, _In_ u16 macRev,
                     _In_ u16 channel_mhz);

#endif /* _AR9485_ATH9K_HW_MIN_H_ */
