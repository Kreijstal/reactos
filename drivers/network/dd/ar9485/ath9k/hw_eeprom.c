/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Slice 3b of the AR9485 port (Phase 2a) - recover the
 *              AR9300-family EEPROM image from the card's serial EEPROM or
 *              its on-die OTP and hand the permanent MAC address to the
 *              miniport.
 *
 *              The block between the VERBATIM markers is copied from Linux
 *              drivers/net/wireless/ath/ath9k/ar9003_eeprom.c (v6.6),
 *              lines 3042-3396, plus ath_pci_eeprom_read() from
 *              ath9k/pci.c:800.  Modifications are confined to:
 *
 *                - ath_dbg(common, EEPROM, ...) -> DPRINT1(), because this
 *                  driver is debugged over KDNET on hardware that can only
 *                  be observed one boot at a time and every one of these
 *                  messages names the step that failed.  All of them sit
 *                  on the init path; none is a hot path.
 *                - ath9k_hw_nvram_read()'s platform-data / nvmem / eeprom
 *                  blob arms are dropped.  Those cover embedded AHB parts
 *                  and USB firmware blobs; a PCIe card has neither, so
 *                  upstream would take the bus_ops->eeprom_read arm every
 *                  time, which is what remains.
 *                - the reference-template restore in
 *                  ar9300_compress_decision() is NOT imported.  See the
 *                  long comment at that function.
 *
 *              What this file deliberately does NOT do is invent a MAC
 *              address.  Upstream ath9k_hw_init_macaddr() (hw.c:270) falls
 *              back to eth_random_addr() when the EEPROM yields nothing
 *              usable; a randomly generated address that merely looks
 *              plausible is far worse here than a clean initialization
 *              failure, so every failure path returns false with a
 *              DPRINT1 naming the reason.
 */

#include "hw_min.h"

#define NDEBUG
#include <debug.h>

/* Verbatim ar9003_eeprom.c:24,25 - compressed-block framing. */
#define COMP_HDR_LEN 4
#define COMP_CKSUM_LEN 2

/* Verbatim ar9003_eeprom.c:41 - upper bound on an AR9485 block, equal to
 * sizeof(struct ar9300_eeprom); see the C_ASSERT in ar9003_eeprom_min.h. */
#define EEPROM_DATA_LEN_9485	1088

/* Verbatim eeprom.h:67,68 - the EEPROM aperture inside the register file.
 * Word `off' maps to register AR5416_EEPROM_OFFSET + (off << 2). */
#define AR5416_EEPROM_S             2
#define AR5416_EEPROM_OFFSET        0x2000

/* Scratch buffer for one compressed block, sized as upstream sizes it. */
#define AR9300_EEPROM_WORD_BUF_LEN  2048

/* --------------------------------------------------------------------
 *  VERBATIM from Linux drivers/net/wireless/ath/ath9k/pci.c:800,
 *  ath_pci_eeprom_read().  Upstream reaches the register file through
 *  common->ops->read; ours is the same accessor behind REG_READ.
 * -------------------------------------------------------------------- */

static bool ar9485_pci_eeprom_read(struct ath_hw *ah, u32 off, u16 *data)
{
    REG_READ(ah, AR5416_EEPROM_OFFSET + (off << AR5416_EEPROM_S));

    if (!ath9k_hw_wait(ah,
                AR_EEPROM_STATUS_DATA(ah),
                AR_EEPROM_STATUS_DATA_BUSY |
                AR_EEPROM_STATUS_DATA_PROT_ACCESS, 0,
                AH_WAIT_TIMEOUT)) {
        return false;
    }

    *data = MS(REG_READ(ah, AR_EEPROM_STATUS_DATA(ah)),
            AR_EEPROM_STATUS_DATA_VAL);

    return true;
}

/* Upstream ath9k_hw_nvram_read() (eeprom.c:105) reduced to its PCI arm:
 * the nvmem / firmware-blob / platform-data arms above it only ever fire
 * for AHB and USB parts. */
static bool ath9k_hw_nvram_read(struct ath_hw *ah, u32 off, u16 *data)
{
    return ar9485_pci_eeprom_read(ah, off, data);
}

/* --------------------------------------------------------------------
 *  VERBATIM from Linux ar9003_eeprom.c:3026-3244.
 * -------------------------------------------------------------------- */

static bool ar9300_eeprom_read_byte(struct ath_hw *ah, int address,
                                    u8 *buffer)
{
    u16 val;

    if (unlikely(!ath9k_hw_nvram_read(ah, address / 2, &val)))
        return false;

    *buffer = (val >> (8 * (address % 2))) & 0xff;
    return true;
}

static bool ar9300_eeprom_read_word(struct ath_hw *ah, int address,
                                    u8 *buffer)
{
    u16 val;

    if (unlikely(!ath9k_hw_nvram_read(ah, address / 2, &val)))
        return false;

    buffer[0] = val >> 8;
    buffer[1] = val & 0xff;

    return true;
}

static bool ar9300_read_eeprom(struct ath_hw *ah, int address, u8 *buffer,
                               int count)
{
    int i;

    if ((address < 0) || ((address + count) / 2 > AR9300_EEPROM_SIZE - 1)) {
        DPRINT1("AR9485: eeprom address not in range\n");
        return false;
    }

    /*
     * Since we're reading the bytes in reverse order from a little-endian
     * word stream, an even address means we only use the lower half of
     * the 16-bit word at that address
     */
    if (address % 2 == 0) {
        if (!ar9300_eeprom_read_byte(ah, address--, buffer++))
            goto error;

        count--;
    }

    for (i = 0; i < count / 2; i++) {
        if (!ar9300_eeprom_read_word(ah, address, buffer))
            goto error;

        address -= 2;
        buffer += 2;
    }

    if (count % 2)
        if (!ar9300_eeprom_read_byte(ah, address, buffer))
            goto error;

    return true;

error:
    DPRINT1("AR9485: unable to read eeprom region at offset %d\n", address);
    return false;
}

static bool ar9300_otp_read_word(struct ath_hw *ah, int addr, u32 *data)
{
    REG_READ(ah, AR9300_OTP_BASE(ah) + (4 * addr));

    if (!ath9k_hw_wait(ah, AR9300_OTP_STATUS(ah), AR9300_OTP_STATUS_TYPE,
                       AR9300_OTP_STATUS_VALID, 1000))
        return false;

    *data = REG_READ(ah, AR9300_OTP_READ_DATA(ah));
    return true;
}

static bool ar9300_read_otp(struct ath_hw *ah, int address, u8 *buffer,
                            int count)
{
    u32 data;
    int i;

    for (i = 0; i < count; i++) {
        int offset = 8 * ((address - i) % 4);
        if (!ar9300_otp_read_word(ah, (address - i) / 4, &data))
            return false;

        buffer[i] = (data >> offset) & 0xff;
    }

    return true;
}

static void ar9300_comp_hdr_unpack(u8 *best, int *code, int *reference,
                                   int *length, int *major, int *minor)
{
    unsigned long value[4];

    value[0] = best[0];
    value[1] = best[1];
    value[2] = best[2];
    value[3] = best[3];
    *code = ((value[0] >> 5) & 0x0007);
    *reference = (value[0] & 0x001f) | ((value[1] >> 2) & 0x0020);
    *length = ((value[1] << 4) & 0x07f0) | ((value[2] >> 4) & 0x000f);
    *major = (value[2] & 0x000f);
    *minor = (value[3] & 0x00ff);
}

static u16 ar9300_comp_cksum(u8 *data, int dsize)
{
    int it, checksum = 0;

    for (it = 0; it < dsize; it++) {
        checksum += data[it];
        checksum &= 0xffff;
    }

    return (u16)checksum;
}

static bool ar9300_uncompress_block(struct ath_hw *ah,
                                    u8 *mptr,
                                    int mdataSize,
                                    u8 *block,
                                    int size)
{
    int it;
    int spot;
    int offset;
    int length;

    UNREFERENCED_PARAMETER(ah);

    spot = 0;

    for (it = 0; it < size; it += (length+2)) {
        offset = block[it];
        offset &= 0xff;
        spot += offset;
        length = block[it+1];
        length &= 0xff;

        if (length > 0 && spot >= 0 && spot+length <= mdataSize) {
            RtlCopyMemory(&mptr[spot], &block[it+2], length);
            spot += length;
        } else if (length > 0) {
            DPRINT1("AR9485: bad restore at %d: spot=%d offset=%d length=%d\n",
                    it, spot, offset, length);
            return false;
        }
    }
    return true;
}

/*
 * ADAPTED from ar9003_eeprom.c:3180, ar9300_compress_decision().
 *
 * The one upstream behaviour dropped here is the reference-template
 * restore.  Upstream seeds mptr with ar9300_default and, for a
 * _CompressBlock with a non-zero reference, re-seeds it from one of five
 * ~1100-line const templates (ar9300_default / _x112 / _h116 / _h112 /
 * _x113) before applying the diff.  Those templates exist to supply
 * calibration and target-power tables that no phase before the PHY
 * bring-up consumes, and importing them here would be actively dangerous
 * for this phase's one output: ar9300_default.macAddr is the placeholder
 * {0, 2, 3, 4, 5, 6} (ar9003_eeprom.c:49), i.e. exactly the kind of
 * plausible-looking wrong MAC that must never reach NDIS.
 *
 * Leaving the base image zeroed is safe for MAC recovery because the
 * compressed diff addresses its targets by a running offset, never by
 * content: ar9300_uncompress_block() walks (offset, length, bytes)
 * triples and accumulates `spot', so the bytes it writes and where it
 * writes them do not depend on what was underneath.  A per-card MAC
 * cannot equal the template placeholder, so it is always carried by the
 * diff.  If some card ever does not carry it, the MAC reads back as all
 * zeroes and ar9485_hw_eeprom_get_macaddr() fails loudly - which is the
 * intended outcome.
 *
 * Phase 2b, which needs the calibration tables, is where the templates
 * have to be imported; the reference id is logged here so that phase
 * knows which one this card wants.
 */
static int ar9300_compress_decision(struct ath_hw *ah,
                                    int it,
                                    int code,
                                    int reference,
                                    u8 *mptr,
                                    u8 *word, int length, int mdata_size)
{
    switch (code) {
    case _CompressNone:
        if (length != mdata_size) {
            DPRINT1("AR9485: EEPROM structure size mismatch memory=%d eeprom=%d\n",
                    mdata_size, length);
            return -1;
        }
        RtlCopyMemory(mptr, word + COMP_HDR_LEN, length);
        DPRINT1("AR9485: restored eeprom %d: uncompressed, length %d\n",
                it, length);
        break;
    case _CompressBlock:
        DPRINT1("AR9485: restore eeprom %d: block, reference %d, length %d "
                "(reference template not applied - Phase 2a reads the MAC only)\n",
                it, reference, length);
        ar9300_uncompress_block(ah, mptr, mdata_size,
                                (word + COMP_HDR_LEN), length);
        break;
    default:
        DPRINT1("AR9485: unknown compression code %d\n", code);
        return -1;
    }
    return 0;
}

typedef bool (*eeprom_read_op)(struct ath_hw *ah, int address, u8 *buffer,
                               int count);

static bool ar9300_check_header(void *data)
{
    u32 *word = data;
    return !(*word == 0 || *word == ~0u);
}

static bool ar9300_check_eeprom_header(struct ath_hw *ah, eeprom_read_op read,
                                       int base_addr)
{
    u8 header[4];

    if (!read(ah, base_addr, header, 4))
        return false;

    return ar9300_check_header(header);
}

/*
 * VERBATIM from ar9003_eeprom.c:3266, ar9300_eeprom_restore_internal(),
 * minus the ath9k_hw_use_flash() arm (AHB parts only) and minus the
 * `memcpy(mptr, &ar9300_default, mdata_size)' seed - see
 * ar9300_compress_decision() above for why the templates stay out.
 *
 * Read the configuration data from the eeprom.
 * The data can be put in any specified memory buffer.
 *
 * Returns -1 on error.
 * Returns address of next memory location on success.
 */
static int ar9300_eeprom_restore_internal(struct ath_hw *ah,
                                          u8 *mptr, int mdata_size)
{
#define MSTATE 100
    int cptr;
    u8 *word;
    int code;
    int reference, length, major, minor;
    int osize;
    int it;
    u16 checksum, mchecksum;
    eeprom_read_op read;

    word = kzalloc(AR9300_EEPROM_WORD_BUF_LEN, GFP_KERNEL);
    if (!word)
        return -ENOMEM;

    RtlZeroMemory(mptr, mdata_size);

    read = ar9300_read_eeprom;
    if (AR_SREV_9485(ah))
        cptr = AR9300_BASE_ADDR_4K;
    else if (AR_SREV_9330(ah))
        cptr = AR9300_BASE_ADDR_512;
    else
        cptr = AR9300_BASE_ADDR;
    DPRINT1("AR9485: trying EEPROM access at address 0x%04x\n", cptr);
    if (ar9300_check_eeprom_header(ah, read, cptr))
        goto found;

    cptr = AR9300_BASE_ADDR_4K;
    DPRINT1("AR9485: trying EEPROM access at address 0x%04x\n", cptr);
    if (ar9300_check_eeprom_header(ah, read, cptr))
        goto found;

    cptr = AR9300_BASE_ADDR_512;
    DPRINT1("AR9485: trying EEPROM access at address 0x%04x\n", cptr);
    if (ar9300_check_eeprom_header(ah, read, cptr))
        goto found;

    read = ar9300_read_otp;
    cptr = AR9300_BASE_ADDR;
    DPRINT1("AR9485: trying OTP access at address 0x%04x\n", cptr);
    if (ar9300_check_eeprom_header(ah, read, cptr))
        goto found;

    cptr = AR9300_BASE_ADDR_512;
    DPRINT1("AR9485: trying OTP access at address 0x%04x\n", cptr);
    if (ar9300_check_eeprom_header(ah, read, cptr))
        goto found;

    goto fail;

found:
    DPRINT1("AR9485: found valid EEPROM data at 0x%04x via %s\n", cptr,
            (read == ar9300_read_otp) ? "OTP" : "EEPROM");

    for (it = 0; it < MSTATE; it++) {
        if (!read(ah, cptr, word, COMP_HDR_LEN))
            goto fail;

        if (!ar9300_check_header(word))
            break;

        ar9300_comp_hdr_unpack(word, &code, &reference,
                               &length, &major, &minor);
        DPRINT1("AR9485: found block at %x: code=%d ref=%d length=%d major=%d minor=%d\n",
                cptr, code, reference, length, major, minor);
        if ((!AR_SREV_9485(ah) && length >= 1024) ||
            (AR_SREV_9485(ah) && length > EEPROM_DATA_LEN_9485) ||
            (length > cptr)) {
            DPRINT1("AR9485: skipping bad header\n");
            cptr -= COMP_HDR_LEN;
            continue;
        }

        osize = length;
        read(ah, cptr, word, COMP_HDR_LEN + osize + COMP_CKSUM_LEN);
        checksum = ar9300_comp_cksum(&word[COMP_HDR_LEN], length);
        mchecksum = get_unaligned_le16(&word[COMP_HDR_LEN + osize]);
        DPRINT1("AR9485: checksum %x %x\n", checksum, mchecksum);
        if (checksum == mchecksum) {
            ar9300_compress_decision(ah, it, code, reference, mptr,
                                     word, length, mdata_size);
        } else {
            DPRINT1("AR9485: skipping block with bad checksum\n");
        }
        cptr -= (COMP_HDR_LEN + osize + COMP_CKSUM_LEN);
    }

    kfree(word);
    return cptr;

fail:
    kfree(word);
    return -1;
#undef MSTATE
}

/* --------------------------------------------------------------------
 *  end verbatim
 * -------------------------------------------------------------------- */

/* Upstream is_valid_ether_addr() (linux/etherdevice.h): a usable station
 * address is unicast (bit 0 of octet 0 clear) and not all-zero.  Extended
 * here with the ar9300_default placeholder, so that if a later phase does
 * import the reference templates, a diff that failed to carry the MAC can
 * never be mistaken for a real address. */
static bool
ar9485_is_valid_mac(const u8 *addr)
{
    static const u8 ar9300_default_macaddr[6] = { 0, 2, 3, 4, 5, 6 };
    int i;
    bool all_zero = true;
    bool all_ones = true;

    for (i = 0; i < 6; i++)
    {
        if (addr[i] != 0x00) all_zero = false;
        if (addr[i] != 0xff) all_ones = false;
    }

    if (all_zero || all_ones)
        return false;

    /* Multicast / group bit set is never valid for a station address. */
    if (addr[0] & 0x01)
        return false;

    if (RtlCompareMemory(addr, ar9300_default_macaddr, 6) == 6)
        return false;

    return true;
}

/* --------------------------------------------------------------------
 *  Miniport-side wrapper.  struct ath_hw now embeds the 1088-byte EEPROM
 *  image, so it comes out of the pool rather than off the stack.
 * -------------------------------------------------------------------- */

bool
ar9485_hw_eeprom_get_macaddr(_In_ void *bar0_base,
                             _In_ u32 bar0_len,
                             _In_ u16 devid,
                             _In_ u32 macVersion,
                             _In_ u16 macRev,
                             _Out_writes_bytes_(6) u8 *out_macaddr,
                             _Out_ u8 *out_eepromVersion,
                             _Out_ u8 *out_templateVersion)
{
    struct ath_hw *ah;
    struct ar9300_eeprom *eep;
    int cptr;
    bool ok = false;

    RtlZeroMemory(out_macaddr, 6);
    *out_eepromVersion = 0;
    *out_templateVersion = 0;

    ah = (struct ath_hw *)ExAllocatePoolWithTag(NonPagedPool,
                                                sizeof(*ah),
                                                AR9485_COMPAT_TAG);
    if (ah == NULL)
    {
        DPRINT1("AR9485: out of memory allocating ath_hw for EEPROM restore\n");
        return false;
    }

    ar9485_hw_attach(ah, bar0_base, bar0_len, devid, macVersion, macRev);

    /* Upstream ath9k_hw_ar9300_fill_eeprom() (ar9003_eeprom.c:3390) hands
     * the restore the address of the in-hw EEPROM union and its exact
     * size; mdata_size is load-bearing for both the _CompressNone length
     * equality test and the _CompressBlock bounds check. */
    eep = &ah->eeprom.ar9300_eep;
    cptr = ar9300_eeprom_restore_internal(ah, (u8 *)eep, sizeof(*eep));
    if (cptr < 0)
    {
        DPRINT1("AR9485: EEPROM/OTP restore failed - no readable image via "
                "EEPROM at 0x%04x/0x%04x or OTP at 0x%04x/0x%04x\n",
                AR9300_BASE_ADDR_4K, AR9300_BASE_ADDR_512,
                AR9300_BASE_ADDR, AR9300_BASE_ADDR_512);
        goto Cleanup;
    }

    DPRINT1("AR9485: EEPROM restore finished at 0x%04x: eepromVersion=%u "
            "templateVersion=%u\n",
            cptr, eep->eepromVersion, eep->templateVersion);

    /* Upstream ath9k_hw_init_macaddr() (hw.c:270) assembles the address
     * from EEP_MAC_LSW/MID/MSW, which ar9003_hw_get_eeprom() answers with
     * get_unaligned_be16(eep->macAddr + 0/2/4).  Reassembling big-endian
     * halves and then splitting them again is the identity on the byte
     * array, so copy it straight out. */
    RtlCopyMemory(out_macaddr, eep->macAddr, 6);

    if (!ar9485_is_valid_mac(out_macaddr))
    {
        DPRINT1("AR9485: EEPROM/OTP yielded an unusable MAC address "
                "%02x:%02x:%02x:%02x:%02x:%02x - refusing to invent one\n",
                out_macaddr[0], out_macaddr[1], out_macaddr[2],
                out_macaddr[3], out_macaddr[4], out_macaddr[5]);
        RtlZeroMemory(out_macaddr, 6);
        goto Cleanup;
    }

    *out_eepromVersion   = eep->eepromVersion;
    *out_templateVersion = eep->templateVersion;
    ok = true;

Cleanup:
    ExFreePoolWithTag(ah, AR9485_COMPAT_TAG);
    return ok;
}
