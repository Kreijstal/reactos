/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Parser for the TLV-flavoured .ucode firmware container.
 *
 * Mirrors Linux's iwl_parse_tlv_firmware().  Deliberately free of NDIS,
 * the DDK and the CRT so the same translation unit can be built for the
 * host harness (-DIWL_HOST_HARNESS) and run against real linux-firmware
 * blobs without a guest boot - the same trick that made the NTFS work
 * tractable.
 *
 * Every read out of the container is bounds-checked against the caller's
 * length.  A firmware file is attacker-reachable input in the sense that
 * it is parsed before any signature check we perform, so an overrun here
 * would be a kernel-mode bug reachable from a file on disk.
 */

#include "ucode_file.h"

/* Legacy (pre-TLV-section) images carry no destination offset; upstream
 * places them at these fixed RTC addresses. */
#define IWLAGN_RTC_INST_LOWER_BOUND     0x000000
#define IWLAGN_RTC_DATA_LOWER_BOUND     0x800000

#define IWL_ALIGN4(x)   (((x) + 3u) & ~3u)

static iwl_u32
IwlReadLe32(const iwl_u8 *p)
{
    return (iwl_u32)p[0] |
           ((iwl_u32)p[1] << 8) |
           ((iwl_u32)p[2] << 16) |
           ((iwl_u32)p[3] << 24);
}

static void
IwlZeroBytes(void *dst, iwl_size_t n)
{
    iwl_u8 *d = (iwl_u8 *)dst;
    while (n-- > 0)
        *d++ = 0;
}

static void
IwlCopyBytes(void *dst, const void *src, iwl_size_t n)
{
    iwl_u8 *d = (iwl_u8 *)dst;
    const iwl_u8 *s = (const iwl_u8 *)src;
    while (n-- > 0)
        *d++ = *s++;
}

/*
 * Append one section to an image.  Returns FALSE when the image is already
 * carrying IWL_UCODE_SECTION_MAX sections, which upstream treats as a
 * malformed file rather than something to silently truncate.
 */
static iwl_bool
IwlStoreSection(
    IWL_FW_IMAGE *Image,
    iwl_u32 Offset,
    const iwl_u8 *Data,
    iwl_u32 Length)
{
    IWL_FW_SECTION *Section;

    if (Image->SectionCount >= IWL_UCODE_SECTION_MAX)
        return IWL_FALSE;

    Section = &Image->Section[Image->SectionCount];
    Section->Offset = Offset;
    Section->Data   = Data;
    Section->Length = Length;
    Image->SectionCount++;
    return IWL_TRUE;
}

/*
 * A SEC_* payload is a 4-byte destination offset followed by the image
 * bytes.  A payload of exactly 4 bytes is legal and describes an empty
 * section - the CPU1/CPU2 and paging separators are exactly that, a bare
 * marker offset with no data.
 */
static IWL_FW_PARSE_STATUS
IwlStoreTlvSection(
    IWL_FW_IMAGE *Image,
    const iwl_u8 *Data,
    iwl_u32 Length)
{
    iwl_u32 Offset;

    if (Length < sizeof(iwl_u32))
        return IwlFwParseBadSectionLength;

    Offset = IwlReadLe32(Data);

    if (!IwlStoreSection(Image,
                         Offset,
                         Data + sizeof(iwl_u32),
                         Length - (iwl_u32)sizeof(iwl_u32)))
    {
        return IwlFwParseTooManySections;
    }

    return IwlFwParseOk;
}

IWL_FW_PARSE_STATUS
IwlParseUcodeFile(
    const void *Image,
    iwl_size_t Length,
    IWL_FW_PARSED *Parsed)
{
    const iwl_u8 *Base = (const iwl_u8 *)Image;
    const iwl_u8 *Data;
    iwl_size_t Remaining;
    IWL_FW_PARSE_STATUS Status;

    IwlZeroBytes(Parsed, sizeof(*Parsed));

    if (Length < sizeof(IWL_TLV_UCODE_HEADER))
        return IwlFwParseTooSmall;

    /* The legacy container put a non-zero version in the first word.  We
     * do not support those parts' images, and must not mis-parse one as a
     * TLV stream. */
    if (IwlReadLe32(Base + 0) != 0)
        return IwlFwParseNotTlv;

    if (IwlReadLe32(Base + 4) != IWL_TLV_UCODE_MAGIC)
        return IwlFwParseBadMagic;

    IwlCopyBytes(Parsed->HumanReadable, Base + 8, FW_VER_HUMAN_READABLE_SZ);
    Parsed->HumanReadable[FW_VER_HUMAN_READABLE_SZ] = 0;

    Parsed->UcodeVer = IwlReadLe32(Base + 8 + FW_VER_HUMAN_READABLE_SZ);
    Parsed->Build    = IwlReadLe32(Base + 8 + FW_VER_HUMAN_READABLE_SZ + 4);

    Data      = Base + sizeof(IWL_TLV_UCODE_HEADER);
    Remaining = Length - sizeof(IWL_TLV_UCODE_HEADER);

    while (Remaining >= sizeof(IWL_UCODE_TLV))
    {
        iwl_u32 TlvType;
        iwl_u32 TlvLength;
        iwl_u32 Advance;
        const iwl_u8 *TlvData;

        TlvType   = IwlReadLe32(Data + 0);
        TlvLength = IwlReadLe32(Data + 4);
        TlvData   = Data + sizeof(IWL_UCODE_TLV);

        Remaining -= sizeof(IWL_UCODE_TLV);

        if (TlvLength > Remaining)
            return IwlFwParseTlvLengthOverflow;

        /* TLVs are padded to a 4-byte boundary.  The final TLV in a file
         * may legitimately stop short of its own padding, so clamp rather
         * than letting the subtraction wrap - upstream's unclamped
         * `len -= ALIGN(tlv_len, 4)` underflows a size_t on exactly that
         * input. */
        Advance = IWL_ALIGN4(TlvLength);
        if ((iwl_size_t)Advance > Remaining)
            Advance = (iwl_u32)Remaining;

        Parsed->TlvCount++;

        switch (TlvType)
        {
            case IWL_UCODE_TLV_INST:
                if (!IwlStoreSection(&Parsed->Image[IWL_UCODE_REGULAR],
                                     IWLAGN_RTC_INST_LOWER_BOUND,
                                     TlvData, TlvLength))
                    return IwlFwParseTooManySections;
                break;

            case IWL_UCODE_TLV_DATA:
                if (!IwlStoreSection(&Parsed->Image[IWL_UCODE_REGULAR],
                                     IWLAGN_RTC_DATA_LOWER_BOUND,
                                     TlvData, TlvLength))
                    return IwlFwParseTooManySections;
                break;

            case IWL_UCODE_TLV_INIT:
                if (!IwlStoreSection(&Parsed->Image[IWL_UCODE_INIT],
                                     IWLAGN_RTC_INST_LOWER_BOUND,
                                     TlvData, TlvLength))
                    return IwlFwParseTooManySections;
                break;

            case IWL_UCODE_TLV_INIT_DATA:
                if (!IwlStoreSection(&Parsed->Image[IWL_UCODE_INIT],
                                     IWLAGN_RTC_DATA_LOWER_BOUND,
                                     TlvData, TlvLength))
                    return IwlFwParseTooManySections;
                break;

            case IWL_UCODE_TLV_WOWLAN_INST:
                if (!IwlStoreSection(&Parsed->Image[IWL_UCODE_WOWLAN],
                                     IWLAGN_RTC_INST_LOWER_BOUND,
                                     TlvData, TlvLength))
                    return IwlFwParseTooManySections;
                break;

            case IWL_UCODE_TLV_WOWLAN_DATA:
                if (!IwlStoreSection(&Parsed->Image[IWL_UCODE_WOWLAN],
                                     IWLAGN_RTC_DATA_LOWER_BOUND,
                                     TlvData, TlvLength))
                    return IwlFwParseTooManySections;
                break;

            case IWL_UCODE_TLV_SEC_RT:
            case IWL_UCODE_TLV_SECURE_SEC_RT:
                Status = IwlStoreTlvSection(&Parsed->Image[IWL_UCODE_REGULAR],
                                            TlvData, TlvLength);
                if (Status != IwlFwParseOk)
                    return Status;
                break;

            case IWL_UCODE_TLV_SEC_INIT:
            case IWL_UCODE_TLV_SECURE_SEC_INIT:
                Status = IwlStoreTlvSection(&Parsed->Image[IWL_UCODE_INIT],
                                            TlvData, TlvLength);
                if (Status != IwlFwParseOk)
                    return Status;
                break;

            case IWL_UCODE_TLV_SEC_WOWLAN:
            case IWL_UCODE_TLV_SECURE_SEC_WOWLAN:
                Status = IwlStoreTlvSection(&Parsed->Image[IWL_UCODE_WOWLAN],
                                            TlvData, TlvLength);
                if (Status != IwlFwParseOk)
                    return Status;
                break;

            case IWL_UCODE_TLV_SEC_RT_USNIFFER:
                Status = IwlStoreTlvSection(&Parsed->Image[IWL_UCODE_REGULAR_USNIFFER],
                                            TlvData, TlvLength);
                if (Status != IwlFwParseOk)
                    return Status;
                break;

            case IWL_UCODE_TLV_API_CHANGES_SET:
            {
                iwl_u32 ApiIndex;
                if (TlvLength < sizeof(IWL_UCODE_API))
                    return IwlFwParseBadSectionLength;
                ApiIndex = IwlReadLe32(TlvData);
                /* A word past the end of our bitmap carries API flags this
                 * build does not consume.  Upstream warns and continues;
                 * failing the load would make every firmware newer than
                 * the driver unloadable, which is the opposite of the
                 * broad-coverage goal. */
                if (ApiIndex >= IWL_NUM_API_WORDS)
                {
                    Parsed->TruncatedApiWordCount++;
                    break;
                }
                Parsed->ApiFlags[ApiIndex] = IwlReadLe32(TlvData + 4);
                break;
            }

            case IWL_UCODE_TLV_ENABLED_CAPABILITIES:
            {
                iwl_u32 ApiIndex;
                if (TlvLength < sizeof(IWL_UCODE_CAPA))
                    return IwlFwParseBadSectionLength;
                ApiIndex = IwlReadLe32(TlvData);
                if (ApiIndex >= IWL_NUM_CAPA_WORDS)
                {
                    Parsed->TruncatedApiWordCount++;
                    break;
                }
                Parsed->CapaFlags[ApiIndex] = IwlReadLe32(TlvData + 4);
                break;
            }

            case IWL_UCODE_TLV_FLAGS:
                if (TlvLength < sizeof(iwl_u32))
                    return IwlFwParseBadSectionLength;
                Parsed->Flags = IwlReadLe32(TlvData);
                break;

            case IWL_UCODE_TLV_NUM_OF_CPU:
                if (TlvLength < sizeof(iwl_u32))
                    return IwlFwParseBadSectionLength;
                Parsed->NumOfCpus = IwlReadLe32(TlvData);
                break;

            case IWL_UCODE_TLV_PHY_CALIBRATION_SIZE:
                if (TlvLength < sizeof(iwl_u32))
                    return IwlFwParseBadSectionLength;
                Parsed->PhyCalibrationSize = IwlReadLe32(TlvData);
                break;

            case IWL_UCODE_TLV_N_SCAN_CHANNELS:
                if (TlvLength < sizeof(iwl_u32))
                    return IwlFwParseBadSectionLength;
                Parsed->NScanChannels = IwlReadLe32(TlvData);
                break;

            case IWL_UCODE_TLV_PROBE_MAX_LEN:
                if (TlvLength < sizeof(iwl_u32))
                    return IwlFwParseBadSectionLength;
                Parsed->ProbeMaxLength = IwlReadLe32(TlvData);
                break;

            case IWL_UCODE_TLV_PHY_SKU:
                if (TlvLength < sizeof(iwl_u32))
                    return IwlFwParseBadSectionLength;
                Parsed->PhySku = IwlReadLe32(TlvData);
                break;

            default:
                /* Expected whenever the pinned blob is newer than this
                 * driver.  Counted so the log can say so, never fatal. */
                Parsed->UnknownTlvCount++;
                break;
        }

        Data      += sizeof(IWL_UCODE_TLV) + Advance;
        Remaining -= Advance;
    }

    /* Anything left over is not a short final TLV - that case was already
     * absorbed by the Advance clamp - so it is a malformed file. */
    if (Remaining != 0)
        return IwlFwParseTruncatedTlv;

    return IwlFwParseOk;
}

const char *
IwlFwParseStatusName(IWL_FW_PARSE_STATUS Status)
{
    switch (Status)
    {
        case IwlFwParseOk:                  return "ok";
        case IwlFwParseTooSmall:            return "file shorter than TLV header";
        case IwlFwParseNotTlv:              return "legacy non-TLV container";
        case IwlFwParseBadMagic:            return "bad magic";
        case IwlFwParseTruncatedTlv:        return "trailing bytes after last TLV";
        case IwlFwParseTlvLengthOverflow:   return "TLV length past end of file";
        case IwlFwParseTooManySections:     return "more sections than IWL_UCODE_SECTION_MAX";
        case IwlFwParseBadSectionLength:    return "TLV payload too short for its type";
        default:                            return "unknown";
    }
}
