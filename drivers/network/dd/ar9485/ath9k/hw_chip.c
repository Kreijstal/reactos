/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Verbatim port of ath9k_hw_read_revisions() from upstream
 *              Linux drivers/net/wireless/ath/ath9k/hw.c.  The function
 *              body is unchanged from upstream; the surrounding glue
 *              builds a small struct ath_hw on the caller's stack so the
 *              verbatim code can run without the full upstream struct.
 *
 *              Slice 2 of the AR9485 Phase 2a port — see linux-compat.h
 *              and ath9k/hw_min.h for the typed shim that lets this
 *              verbatim source compile against NDIS.
 */

#include "hw_min.h"

/* --------------------------------------------------------------------
 *  reg_ops bridge: NDIS-side BAR0 base + READ_REGISTER_ULONG.
 * -------------------------------------------------------------------- */

static unsigned int
ar9485_reg_read(void *ctx, u32 reg_offset)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)ctx + reg_offset));
}

static void
ar9485_reg_write(void *ctx, u32 val, u32 reg_offset)
{
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)ctx + reg_offset), val);
}

/* --------------------------------------------------------------------
 *  VERBATIM from Linux drivers/net/wireless/ath/ath9k/hw.c:254
 *
 *  Only modification: removed AR_SREV_9100 PCIe override (Linux late-init
 *  path) — the AR9485 always reports the new (val==0xFF sentinel) layout
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
                      _In_ u16 devid,
                      _Out_ u32 *out_macVersion,
                      _Out_ u16 *out_macRev,
                      _Out_ bool *out_is_pciexpress)
{
    struct ath_hw ah;
    bool ok;

    RtlZeroMemory(&ah, sizeof(ah));
    ah.reg_ctx           = bar0_base;
    ah.reg_ops.read      = ar9485_reg_read;
    ah.reg_ops.write     = ar9485_reg_write;
    ah.hw_version.devid  = devid;
    ah.get_mac_revision  = NULL;
    ah.is_pciexpress     = false;

    ok = ath9k_hw_read_revisions(&ah);
    if (!ok)
        return false;

    *out_macVersion     = ah.hw_version.macVersion;
    *out_macRev         = ah.hw_version.macRev;
    *out_is_pciexpress  = ah.is_pciexpress;
    return true;
}
