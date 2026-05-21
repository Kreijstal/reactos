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

/* Verbatim copy of include/ath9k/hw.h:524 — struct ath9k_hw_version. */
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

/* Subset of struct ath_ops from include/ath.h:127 — only the two function
 * pointers ath9k_hw_read_revisions exercises via REG_READ. */
struct ath_ops {
    unsigned int (*read)(void *, u32 reg_offset);
    void         (*write)(void *, u32 val, u32 reg_offset);
};

/* Minimal stand-in for struct ath_hw.  Field names match upstream so the
 * verbatim function bodies compile.  Carries only what slice 2 needs. */
struct ath_hw {
    struct ath_ops reg_ops;
    void          *reg_ctx;          /* opaque cookie passed to read/write */
    struct ath9k_hw_version hw_version;
    bool           is_pciexpress;
    u16          (*get_mac_revision)(void);
};

/* REG_READ / REG_WRITE — copy of hw.h:80,82 macros. */
#define REG_READ(_ah, _reg) \
    (_ah)->reg_ops.read((_ah)->reg_ctx, (_reg))
#define REG_WRITE(_ah, _reg, _val) \
    (_ah)->reg_ops.write((_ah)->reg_ctx, (_val), (_reg))

/* MS / SM — copy of hw.h:122,121 macros (mask-shift / shift-mask). */
#define MS(_v, _f) (((_v) & (_f)) >> _f##_S)
#define SM(_v, _f) (((_v) << _f##_S) & (_f))

/* Device IDs — copy of hw.h:35-72.  Slice 2 cares only about AR9485 but
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
 * with the same field name and type — so the upstream macros expand
 * without modification. */

/* ath_err logging stub — upstream funnels into a printk-like macro; we
 * land in DPRINT1 so failures show up on COM1. */
#define ath9k_hw_common(_ah)  (_ah)
struct _ath_common_dummy { int unused; };
#define ath_err(_common, _fmt, ...) \
    DbgPrint("ath9k_hw: " _fmt "\n", ##__VA_ARGS__)

/* Slice 2 entry point exposed to the miniport side.  Builds a tiny ath_hw
 * on the caller's stack, runs the verbatim revision-read, and returns the
 * decoded macVersion / macRev for the AR9485 chip the miniport bound to.
 */
bool ar9485_read_revisions(_In_ void *bar0_base,
                           _In_ u16 devid,
                           _Out_ u32 *out_macVersion,
                           _Out_ u16 *out_macRev,
                           _Out_ bool *out_is_pciexpress);

#endif /* _AR9485_ATH9K_HW_MIN_H_ */
