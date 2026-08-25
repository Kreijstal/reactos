/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     On-disk layout of an Intel Wireless firmware (.ucode)
 *              container, and the result of parsing one.
 *
 * Layout is VERBATIM from Linux drivers/net/wireless/intel/iwlwifi/
 * iwl-fw-file.h.  Do not reorder or rename - a resync must stay a
 * mechanical diff.
 *
 * This header and fw/ucode_parse.c are deliberately free of NDIS and DDK
 * dependencies so the parser can be compiled and unit-tested on the host
 * against real linux-firmware blobs.  Define IWL_HOST_HARNESS to build the
 * standalone flavour; see fw/README-harness.md.
 */

#ifndef _IWLWIFI_UCODE_FILE_H_
#define _IWLWIFI_UCODE_FILE_H_

#ifdef IWL_HOST_HARNESS
#include <stdint.h>
#include <stddef.h>
typedef uint8_t   iwl_u8;
typedef uint16_t  iwl_u16;
typedef uint32_t  iwl_u32;
typedef uint64_t  iwl_u64;
typedef size_t    iwl_size_t;
typedef int       iwl_bool;
#define IWL_TRUE  1
#define IWL_FALSE 0
#else
#include <ndis.h>
typedef UCHAR     iwl_u8;
typedef USHORT    iwl_u16;
typedef ULONG     iwl_u32;
typedef ULONGLONG iwl_u64;
typedef SIZE_T    iwl_size_t;
typedef BOOLEAN   iwl_bool;
#define IWL_TRUE  TRUE
#define IWL_FALSE FALSE
#endif

/* "IWL\n" as a little-endian u32. */
#define IWL_TLV_UCODE_MAGIC         0x0a4c5749

#define FW_VER_HUMAN_READABLE_SZ    64
/*
 * Upstream sizes the per-image section array dynamically after a counting
 * pass.  A fixed cap keeps this parser free of any allocator, which is what
 * lets it be unit-tested on the host - so the cap is set from measurement,
 * not from the stale "16" that older kernels used.
 *
 * Observed section counts, largest image per file, measured with
 * fw/harness/ucode_parse_test.c against linux-firmware (2026-08-25):
 *
 *   iwlwifi-5000-5                    2      iwlwifi-9260-th-b0-jf-b0-46  13
 *   iwlwifi-6000g2a-6                 2      iwlwifi-cc-a0-77             50
 *   iwlwifi-3168-29 / 7265D-29        4      iwlwifi-Qu-b0-hr-b0-77       51
 *   iwlwifi-8265-36                  13      iwlwifi-ty-a0-gf-a0-89       58
 *   iwlwifi-9000-pu-b0-jf-b0-46      13      iwlwifi-so-a0-gf-a0-89       60
 *
 * 128 leaves better than 2x headroom over the largest shipping image.  If
 * a future blob trips IwlFwParseTooManySections, raise this - the parser
 * fails loudly rather than silently dropping sections, so the failure will
 * name itself.
 */
#ifndef IWL_UCODE_SECTION_MAX
#define IWL_UCODE_SECTION_MAX       128
#endif

/*
 * Two reserved destination offsets that are markers rather than addresses.
 * A section carrying one of these separates the CPU1 image from the CPU2
 * image, or the resident image from the paged one.
 */
#define CPU1_CPU2_SEPARATOR_SECTION 0xFFFFCCCC
#define PAGING_SEPARATOR_SECTION    0xAAAABBBB

/*
 * Size of the API/capability bitmaps, in u32 words.  Upstream tracks
 * NUM_IWL_UCODE_TLV_API / _CAPA bits; a firmware newer than the driver
 * legitimately advertises words past the end of these, which is a
 * "flags we do not consume", NOT a malformed file.  See the out-of-range
 * handling in IwlParseUcodeFile().
 */
#define IWL_NUM_API_WORDS           4
#define IWL_NUM_CAPA_WORDS          4

#ifdef IWL_HOST_HARNESS
#pragma pack(push, 1)
#else
#include <pshpack1.h>
#endif

/*
 * TLV-flavoured container header.  The legacy (pre-TLV) format put a
 * non-zero version in the first word, so a zero there is what identifies
 * the TLV flavour.
 */
typedef struct _IWL_TLV_UCODE_HEADER
{
    iwl_u32 Zero;           /* 0 - distinguishes this from the v1 format */
    iwl_u32 Magic;          /* IWL_TLV_UCODE_MAGIC */
    iwl_u8  HumanReadable[FW_VER_HUMAN_READABLE_SZ];
    iwl_u32 Ver;            /* major/minor/api/serial, packed */
    iwl_u32 Build;
    iwl_u64 Ignore;         /* was init_size + inst_size in an older rev */
    /* iwl_u8 Data[]; - TLV stream follows */
} IWL_TLV_UCODE_HEADER;

typedef struct _IWL_UCODE_TLV
{
    iwl_u32 Type;
    iwl_u32 Length;
    /* iwl_u8 Data[]; - padded to a 4-byte boundary */
} IWL_UCODE_TLV;

/* Payload of IWL_UCODE_TLV_SEC_*: destination offset, then the image. */
typedef struct _IWL_UCODE_TLV_SEC
{
    iwl_u32 Offset;
    /* iwl_u8 Data[]; */
} IWL_UCODE_TLV_SEC;

/* Payload of IWL_UCODE_TLV_API_CHANGES_SET / ENABLED_CAPABILITIES. */
typedef struct _IWL_UCODE_API
{
    iwl_u32 ApiIndex;
    iwl_u32 ApiFlags;
} IWL_UCODE_API;

typedef struct _IWL_UCODE_CAPA
{
    iwl_u32 ApiIndex;
    iwl_u32 ApiCapa;
} IWL_UCODE_CAPA;

#ifdef IWL_HOST_HARNESS
#pragma pack(pop)
#else
#include <poppack.h>
#endif

/*
 * TLV type numbers.  Only the ones the parser acts on are named; anything
 * else is counted and skipped, which is the correct behaviour - a firmware
 * blob newer than this driver will carry TLVs we have never heard of and
 * must still load.
 */
enum
{
    IWL_UCODE_TLV_INVALID               = 0,
    IWL_UCODE_TLV_INST                  = 1,
    IWL_UCODE_TLV_DATA                  = 2,
    IWL_UCODE_TLV_INIT                  = 3,
    IWL_UCODE_TLV_INIT_DATA             = 4,
    IWL_UCODE_TLV_BOOT                  = 5,
    IWL_UCODE_TLV_PROBE_MAX_LEN         = 6,
    IWL_UCODE_TLV_PAN                   = 7,
    IWL_UCODE_TLV_RUNT_EVTLOG_PTR       = 8,
    IWL_UCODE_TLV_RUNT_EVTLOG_SIZE      = 9,
    IWL_UCODE_TLV_RUNT_ERRLOG_PTR       = 10,
    IWL_UCODE_TLV_INIT_EVTLOG_PTR       = 11,
    IWL_UCODE_TLV_INIT_EVTLOG_SIZE      = 12,
    IWL_UCODE_TLV_INIT_ERRLOG_PTR       = 13,
    IWL_UCODE_TLV_ENHANCE_SENS_TBL      = 14,
    IWL_UCODE_TLV_PHY_CALIBRATION_SIZE  = 15,
    IWL_UCODE_TLV_WOWLAN_INST           = 16,
    IWL_UCODE_TLV_WOWLAN_DATA           = 17,
    IWL_UCODE_TLV_FLAGS                 = 18,
    IWL_UCODE_TLV_SEC_RT                = 19,
    IWL_UCODE_TLV_SEC_INIT              = 20,
    IWL_UCODE_TLV_SEC_WOWLAN            = 21,
    IWL_UCODE_TLV_DEF_CALIB             = 22,
    IWL_UCODE_TLV_PHY_SKU               = 23,
    IWL_UCODE_TLV_SECURE_SEC_RT         = 24,
    IWL_UCODE_TLV_SECURE_SEC_INIT       = 25,
    IWL_UCODE_TLV_SECURE_SEC_WOWLAN     = 26,
    IWL_UCODE_TLV_NUM_OF_CPU            = 27,
    IWL_UCODE_TLV_CSCHEME               = 28,
    IWL_UCODE_TLV_API_CHANGES_SET       = 29,
    IWL_UCODE_TLV_ENABLED_CAPABILITIES  = 30,
    IWL_UCODE_TLV_N_SCAN_CHANNELS       = 31,
    IWL_UCODE_TLV_PAGING                = 32,
    IWL_UCODE_TLV_SEC_RT_USNIFFER       = 34,
    IWL_UCODE_TLV_SDIO_ADMA_ADDR        = 35,
    IWL_UCODE_TLV_FW_VERSION            = 36,
    IWL_UCODE_TLV_FW_DBG_DEST           = 38,
    IWL_UCODE_TLV_FW_DBG_CONF           = 39,
    IWL_UCODE_TLV_FW_DBG_TRIGGER        = 40,
    IWL_UCODE_TLV_CMD_VERSIONS          = 48,
    IWL_UCODE_TLV_FW_GSCAN_CAPA         = 50,
    IWL_UCODE_TLV_FW_MEM_SEG            = 51,
    IWL_UCODE_TLV_UMAC_DEBUG_ADDRS      = 54,
    IWL_UCODE_TLV_LMAC_DEBUG_ADDRS      = 55,
    IWL_UCODE_TLV_FW_RECOVERY_INFO      = 57,
    IWL_UCODE_TLV_HW_TYPE               = 58,
    IWL_UCODE_TLV_FW_FSEQ_VERSION       = 60,
    IWL_UCODE_TLV_PHY_INTEGRATION_VERSION = 61,
    IWL_UCODE_TLV_PNVM_VERSION          = 62,
    IWL_UCODE_TLV_PNVM_SKU              = 64,
    IWL_UCODE_TLV_SEC_TABLE_ADDR        = 66,
    IWL_UCODE_TLV_D3_KEK_KCK_ADDR       = 67,
    IWL_UCODE_TLV_CURRENT_PC            = 68
};

/* Which of the images inside one container a section belongs to. */
typedef enum _IWL_UCODE_IMAGE_TYPE
{
    IWL_UCODE_REGULAR = 0,
    IWL_UCODE_INIT    = 1,
    IWL_UCODE_WOWLAN  = 2,
    IWL_UCODE_REGULAR_USNIFFER = 3,
    IWL_UCODE_TYPE_MAX = 4
} IWL_UCODE_IMAGE_TYPE;

/*
 * One section of one image.  Data points INTO the caller-owned container
 * buffer; nothing here owns memory.
 */
typedef struct _IWL_FW_SECTION
{
    iwl_u32       Offset;   /* destination address in device memory */
    const iwl_u8 *Data;
    iwl_u32       Length;
} IWL_FW_SECTION;

typedef struct _IWL_FW_IMAGE
{
    IWL_FW_SECTION Section[IWL_UCODE_SECTION_MAX];
    iwl_u32        SectionCount;
} IWL_FW_IMAGE;

/* Result of a successful parse. */
typedef struct _IWL_FW_PARSED
{
    iwl_u8       HumanReadable[FW_VER_HUMAN_READABLE_SZ + 1];
    iwl_u32      UcodeVer;
    iwl_u32      Build;

    IWL_FW_IMAGE Image[IWL_UCODE_TYPE_MAX];

    iwl_u32      ApiFlags[IWL_NUM_API_WORDS];
    iwl_u32      CapaFlags[IWL_NUM_CAPA_WORDS];

    iwl_u32      Flags;                 /* IWL_UCODE_TLV_FLAGS payload */
    iwl_u32      NumOfCpus;
    iwl_u32      PhyCalibrationSize;
    iwl_u32      NScanChannels;
    iwl_u32      ProbeMaxLength;
    iwl_u32      PhySku;

    /* TLV types this build does not recognise.  Not an error: it is the
     * expected state when the pinned blob is newer than the driver. */
    iwl_u32      UnknownTlvCount;
    /* API/capability words past the end of our bitmaps, for the same
     * reason.  Also not an error - upstream warns and carries on, and a
     * driver that refused here could never load a newer firmware. */
    iwl_u32      TruncatedApiWordCount;
    iwl_u32      TlvCount;
} IWL_FW_PARSED;

/*
 * Platform NVM (.pnvm) - AX210 and later.
 *
 * The file is a BARE TLV stream: no 88-byte container header, the first
 * byte is the first TLV.  It is a sequence of blocks, each opened by an
 * IWL_UCODE_TLV_PNVM_SKU carrying a 3-word SKU ID; the HW_TYPE,
 * PNVM_VERSION and SEC_RT TLVs that follow belong to that block until the
 * next PNVM_SKU.  The device picks a block by matching the SKU ID the
 * firmware reports in its ALIVE response, which is why the PNVM can only
 * be pushed after the ucode is running.
 */

/*
 * Measured with fw/harness/ucode_parse_test.c against linux-firmware
 * (2026-08-25), blocks per file / sections per block:
 *
 *   iwlwifi-so-a0-gf-a0.pnvm    4 blocks, 2 sections each
 *   iwlwifi-ty-a0-gf-a0.pnvm    4 blocks, 2 sections each
 *   iwlwifi-gl-c0-fm-c0.pnvm   16 blocks, 2 sections each
 *   iwlwifi-bz-b0-fm-c0.pnvm   16 blocks, 2 sections each
 *
 * A dropped block is NOT harmless the way a dropped ucode section would
 * be: the one block that matters is whichever matches this board's SKU,
 * and it could be any of them.  32 is 2x the largest file shipping today.
 */
#ifndef IWL_PNVM_MAX_BLOCKS
#define IWL_PNVM_MAX_BLOCKS         32
#endif
#ifndef IWL_PNVM_MAX_SECTIONS
#define IWL_PNVM_MAX_SECTIONS       16
#endif

/* Deprecated in-band separator upstream still skips over. */
#define IWL_PNVM_SKIP_SECTION       0xddddeeee

typedef struct _IWL_PNVM_BLOCK
{
    iwl_u32        SkuId[3];

    iwl_bool       HasHwType;
    iwl_u16        MacType;
    iwl_u16        RfId;

    iwl_bool       HasVersion;
    iwl_u32        Version;

    IWL_FW_SECTION Section[IWL_PNVM_MAX_SECTIONS];
    iwl_u32        SectionCount;
    /* Sum of the section payloads - what a DMA buffer would have to hold. */
    iwl_u32        TotalDataLength;
} IWL_PNVM_BLOCK;

typedef struct _IWL_PNVM_PARSED
{
    IWL_PNVM_BLOCK Block[IWL_PNVM_MAX_BLOCKS];
    iwl_u32        BlockCount;

    iwl_u32        TlvCount;
    iwl_u32        UnknownTlvCount;
    /* Blocks past IWL_PNVM_MAX_BLOCKS.  Skipped rather than fatal, because
     * a file describes many SKUs and this board is only ever one of them -
     * but if the board's own SKU was in the dropped set, the later
     * IwlPnvmSelectBlock() returns NULL and the caller must say so rather
     * than silently pushing the wrong block. */
    iwl_u32        TruncatedBlockCount;
} IWL_PNVM_PARSED;

/* Parser result codes.  Distinct values so a failure names its own cause
 * in the log rather than collapsing to "bad firmware". */
typedef enum _IWL_FW_PARSE_STATUS
{
    IwlFwParseOk = 0,
    IwlFwParseTooSmall,             /* file shorter than the header */
    IwlFwParseNotTlv,               /* legacy v1 container, unsupported */
    IwlFwParseBadMagic,
    IwlFwParseTruncatedTlv,         /* a TLV header runs past end of file */
    IwlFwParseTlvLengthOverflow,    /* a TLV claims more data than remains */
    IwlFwParseTooManySections,
    IwlFwParseBadSectionLength,     /* SEC_* payload smaller than its offset word */
    IwlFwParseTooManyPnvmSections,
    IwlFwParseSectionOutsideBlock   /* PNVM data before any PNVM_SKU opened a block */
} IWL_FW_PARSE_STATUS;

/*
 * Parse a .ucode container.  Image must remain valid for as long as
 * Parsed is used - Parsed's sections point into it.
 */
IWL_FW_PARSE_STATUS
IwlParseUcodeFile(
    const void *Image,
    iwl_size_t Length,
    IWL_FW_PARSED *Parsed);

const char *
IwlFwParseStatusName(IWL_FW_PARSE_STATUS Status);

/*
 * Parse a .pnvm container.  Image must outlive Parsed - the sections point
 * into it, exactly as for IwlParseUcodeFile().
 */
IWL_FW_PARSE_STATUS
IwlParsePnvmFile(
    const void *Image,
    iwl_size_t Length,
    IWL_PNVM_PARSED *Parsed);

/*
 * Pick the block matching a SKU ID (as reported by the firmware's ALIVE
 * response).  Returns NULL when the file describes no such SKU, which is a
 * real and reportable condition - the blob is for a different board.
 */
const IWL_PNVM_BLOCK *
IwlPnvmSelectBlock(
    const IWL_PNVM_PARSED *Parsed,
    const iwl_u32 SkuId[3]);

#endif /* _IWLWIFI_UCODE_FILE_H_ */
