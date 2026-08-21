/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Verbatim port of ath9k_hw_read_revisions() from upstream
 *              Linux drivers/net/wireless/ath/ath9k/hw.c.  The function
 *              body is unchanged from upstream; the surrounding glue
 *              builds a small struct ath_hw on the caller's stack so the
 *              verbatim code can run without the full upstream struct.
 *
 *              Slices 2 and 3a of the AR9485 port - see linux-compat.h
 *              and ath9k/hw_min.h for the typed shim that lets this
 *              verbatim source compile against NDIS.
 */

#include "hw_min.h"

#define NDEBUG
#include <debug.h>

/* --------------------------------------------------------------------
 *  reg_ops bridge: NDIS-side BAR0 base + READ_REGISTER_ULONG.
 *
 *  The struct ath_hw carrying the bridge doubles as the context cookie so
 *  the accessors can range-check against the mapped BAR length; see the
 *  AR9485_REG_OUT_OF_RANGE note in hw_min.h for why Windows needs that
 *  and Linux does not.
 * -------------------------------------------------------------------- */

static unsigned int
ar9485_reg_read(void *ctx, u32 reg_offset)
{
    struct ath_hw *ah = (struct ath_hw *)ctx;

    if (reg_offset + sizeof(ULONG) > ah->reg_len)
    {
        if (!ah->reg_range_warned)
        {
            ah->reg_range_warned = true;
            DPRINT1("AR9485: register read 0x%05x outside BAR0 window "
                    "(len 0x%x); further out-of-range accesses are silent\n",
                    reg_offset, ah->reg_len);
        }
        return AR9485_REG_OUT_OF_RANGE;
    }
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)ah->reg_ctx + reg_offset));
}

static void
ar9485_reg_write(void *ctx, u32 val, u32 reg_offset)
{
    struct ath_hw *ah = (struct ath_hw *)ctx;

    if (reg_offset + sizeof(ULONG) > ah->reg_len)
    {
        if (!ah->reg_range_warned)
        {
            ah->reg_range_warned = true;
            DPRINT1("AR9485: register write 0x%05x outside BAR0 window "
                    "(len 0x%x); further out-of-range accesses are silent\n",
                    reg_offset, ah->reg_len);
        }
        return;
    }
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)ah->reg_ctx + reg_offset), val);
}

void
ar9485_hw_attach(_Out_ struct ath_hw *ah,
                 _In_ void *bar0_base,
                 _In_ u32 bar0_len,
                 _In_ u16 devid,
                 _In_ u32 macVersion,
                 _In_ u16 macRev)
{
    RtlZeroMemory(ah, sizeof(*ah));
    ah->reg_ctx                 = bar0_base;
    ah->reg_len                 = bar0_len;
    ah->reg_ops.read            = ar9485_reg_read;
    ah->reg_ops.write           = ar9485_reg_write;
    ah->hw_version.devid        = devid;
    ah->hw_version.macVersion   = macVersion;
    ah->hw_version.macRev       = macRev;
    ah->get_mac_revision        = NULL;
    ah->is_pciexpress           = false;
}

/* --------------------------------------------------------------------
 *  VERBATIM from Linux drivers/net/wireless/ath/ath9k/hw.c:60
 * -------------------------------------------------------------------- */

bool ath9k_hw_wait(struct ath_hw *ah, u32 reg, u32 mask, u32 val, u32 timeout)
{
    int i;

    BUG_ON(timeout < AH_TIME_QUANTUM);

    for (i = 0; i < (timeout / AH_TIME_QUANTUM); i++) {
        if ((REG_READ(ah, reg) & mask) == val)
            return true;

        udelay(AH_TIME_QUANTUM);
    }

    ath_err(ath9k_hw_common(ah),
        "timeout (%d us) on reg 0x%x: 0x%08x & 0x%08x != 0x%08x",
        timeout, reg, REG_READ(ah, reg), mask, val);

    return false;
}

/* --------------------------------------------------------------------
 *  VERBATIM from Linux drivers/net/wireless/ath/ath9k/hw.c:254
 *
 *  Only modification: removed AR_SREV_9100 PCIe override (Linux late-init
 *  path) - the AR9485 always reports the new (val==0xFF sentinel) layout
 *  so the relevant code path is the if-branch below.  The dead branches
 *  are kept verbatim so a future cherry-pick from upstream merges
 *  cleanly with `git apply`.
 * -------------------------------------------------------------------- */

static bool ath9k_hw_read_revisions(struct ath_hw *ah)
{
    u32 srev;
    u32 val;

    if (ah->get_mac_revision)
        ah->hw_version.macRev = ah->get_mac_revision();

    switch (ah->hw_version.devid) {
    case AR5416_AR9100_DEVID:
        ah->hw_version.macVersion = AR_SREV_VERSION_9100;
        break;
    case AR9300_DEVID_AR9330:
        ah->hw_version.macVersion = AR_SREV_VERSION_9330;
        if (!ah->get_mac_revision) {
            val = REG_READ(ah, AR_SREV(ah));
            ah->hw_version.macRev = MS(val, AR_SREV_REVISION2);
        }
        return true;
    case AR9300_DEVID_AR9340:
        ah->hw_version.macVersion = AR_SREV_VERSION_9340;
        return true;
    case AR9300_DEVID_QCA955X:
        ah->hw_version.macVersion = AR_SREV_VERSION_9550;
        return true;
    case AR9300_DEVID_AR953X:
        ah->hw_version.macVersion = AR_SREV_VERSION_9531;
        return true;
    case AR9300_DEVID_QCA956X:
        ah->hw_version.macVersion = AR_SREV_VERSION_9561;
        return true;
    }

    srev = REG_READ(ah, AR_SREV(ah));

    if (srev == (u32)-1) {
        ath_err(ath9k_hw_common(ah),
            "Failed to read SREV register");
        return false;
    }

    val = srev & AR_SREV_ID(ah);

    if (val == 0xFF) {
        val = srev;
        ah->hw_version.macVersion =
            (val & AR_SREV_VERSION2) >> AR_SREV_TYPE2_S;
        ah->hw_version.macRev = MS(val, AR_SREV_REVISION2);

        if (AR_SREV_9462(ah) || AR_SREV_9565(ah))
            ah->is_pciexpress = true;
        else
            ah->is_pciexpress = (val &
                         AR_SREV_TYPE2_HOST_MODE) ? 0 : 1;
    } else {
        if (!AR_SREV_9100(ah))
            ah->hw_version.macVersion = MS(val, AR_SREV_VERSION);

        ah->hw_version.macRev = val & AR_SREV_REVISION;

        if (ah->hw_version.macVersion == AR_SREV_VERSION_5416_PCIE)
            ah->is_pciexpress = true;
    }

    return true;
}

/* --------------------------------------------------------------------
 *  Miniport-side wrapper.  Builds a stack-resident struct ath_hw with
 *  just enough plumbing to run the upstream code path, then copies the
 *  decoded fields out to caller storage.
 * -------------------------------------------------------------------- */

bool
ar9485_read_revisions(_In_ void *bar0_base,
                      _In_ u32 bar0_len,
                      _In_ u16 devid,
                      _Out_ u32 *out_macVersion,
                      _Out_ u16 *out_macRev,
                      _Out_ bool *out_is_pciexpress)
{
    struct ath_hw ah;
    bool ok;

    ar9485_hw_attach(&ah, bar0_base, bar0_len, devid, 0, 0);

    ok = ath9k_hw_read_revisions(&ah);
    if (!ok)
        return false;

    *out_macVersion     = ah.hw_version.macVersion;
    *out_macRev         = ah.hw_version.macRev;
    *out_is_pciexpress  = ah.is_pciexpress;
    return true;
}

/* --------------------------------------------------------------------
 *  Slice 3a: power-on reset of the RTC domain.
 *
 *  ADAPTED from Linux ath9k_hw_set_reset_power_on() (hw.c:1391) plus the
 *  AR_WA read-back that __ath9k_hw_init() (hw.c:1591) performs just
 *  before calling it.  Two upstream tails are deliberately not imported:
 *
 *   - the AR_SREV_9100 / pre-AR9300 AR_RC(AHB) writes, which are dead for
 *     an AR9485 (AR_SREV_9300_20_OR_LATER is true, so upstream skips them
 *     too);
 *   - the closing ath9k_hw_set_reset(ah, ATH9K_RESET_WARM), which drops
 *     the MAC/BB out of reset for the data path.  Reading the OTP only
 *     needs the RTC block awake, and the warm reset pulls in the MCI GPM
 *     offset check, AR_INTR_SYNC_* handling and ah->config - none of
 *     which exists yet.  Phase 2b, which brings up the PHY, is where that
 *     tail belongs.
 *
 *  Everything that is imported is bit-identical to upstream.
 * -------------------------------------------------------------------- */

bool
ar9485_hw_power_on(_In_ void *bar0_base,
                   _In_ u32 bar0_len,
                   _In_ u16 devid,
                   _In_ u32 macVersion,
                   _In_ u16 macRev)
{
    struct ath_hw ahs;
    struct ath_hw *ah = &ahs;

    ar9485_hw_attach(ah, bar0_base, bar0_len, devid, macVersion, macRev);

    /* hw.c:1591 - stash AR_WA with the L1 and ASPM-timer workaround bits
     * set.  Upstream re-writes this copy on every reset because AR_WA is
     * unreadable while the chip sleeps. */
    ah->WARegVal = REG_READ(ah, AR_WA(ah));
    if (ah->WARegVal == AR9485_REG_OUT_OF_RANGE)
    {
        DPRINT1("AR9485: BAR0 window too small to reach AR_WA at 0x%05x\n",
                AR_WA(ah));
        return false;
    }
    ah->WARegVal |= (AR_WA_D3_L1_DISABLE | AR_WA_ASPM_TIMER_BASED_DISABLE);

    REG_WRITE(ah, AR_WA(ah), ah->WARegVal);
    udelay(10);

    REG_WRITE(ah, AR_RTC_FORCE_WAKE(ah), AR_RTC_FORCE_WAKE_EN |
          AR_RTC_FORCE_WAKE_ON_INT);

    REG_WRITE(ah, AR_RTC_RESET(ah), 0);

    udelay(2);

    REG_WRITE(ah, AR_RTC_RESET(ah), 1);

    if (!ath9k_hw_wait(ah,
               AR_RTC_STATUS(ah),
               AR_RTC_STATUS_M(ah),
               AR_RTC_STATUS_ON,
               AH_WAIT_TIMEOUT)) {
        DPRINT1("AR9485: RTC not waking up; AR_RTC_STATUS=0x%08x\n",
                REG_READ(ah, AR_RTC_STATUS(ah)));
        return false;
    }

    DPRINT1("AR9485: RTC powered on, AR_WA=0x%08x AR_RTC_STATUS=0x%08x\n",
            ah->WARegVal, REG_READ(ah, AR_RTC_STATUS(ah)));
    return true;
}
