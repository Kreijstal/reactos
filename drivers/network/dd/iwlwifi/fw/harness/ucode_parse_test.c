/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Host-side unit test for fw/ucode_parse.c.
 *
 * NOT part of the driver build - nothing in CMakeLists.txt references
 * this.  It compiles the real parser translation unit for the host so the
 * container format can be exercised in milliseconds against real
 * linux-firmware blobs and against hand-built malformed input, instead of
 * needing a guest boot to find out that a bounds check was wrong.
 *
 *   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -DIWL_HOST_HARNESS -I.. -o ucode_parse_test \
 *      ucode_parse_test.c ../ucode_parse.c
 *   ./ucode_parse_test [real-blob.ucode ...]
 *
 * The malformed cases matter more than the happy path: a .ucode file is
 * parsed in kernel mode before anything has authenticated it, so every
 * rejection below is a bug that would otherwise be an overrun.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucode_file.h"

static int g_Failures;
static int g_Checks;

static void
Check(int Condition, const char *What)
{
    g_Checks++;
    if (!Condition)
    {
        printf("  FAIL: %s\n", What);
        g_Failures++;
    }
}

static void
CheckStatus(IWL_FW_PARSE_STATUS Got, IWL_FW_PARSE_STATUS Want, const char *What)
{
    g_Checks++;
    if (Got != Want)
    {
        printf("  FAIL: %s: got \"%s\", want \"%s\"\n",
               What, IwlFwParseStatusName(Got), IwlFwParseStatusName(Want));
        g_Failures++;
    }
}

/* ------------------------------------------------------------------ */
/* Container builder                                                   */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char *Buf;
    size_t         Len;
    size_t         Cap;
} BUILDER;

static void
BuilderPut(BUILDER *b, const void *data, size_t n)
{
    if (b->Len + n > b->Cap)
    {
        b->Cap = (b->Len + n) * 2 + 256;
        b->Buf = realloc(b->Buf, b->Cap);
    }
    memcpy(b->Buf + b->Len, data, n);
    b->Len += n;
}

static void
BuilderPut32(BUILDER *b, uint32_t v)
{
    unsigned char t[4];
    t[0] = (unsigned char)(v & 0xff);
    t[1] = (unsigned char)((v >> 8) & 0xff);
    t[2] = (unsigned char)((v >> 16) & 0xff);
    t[3] = (unsigned char)((v >> 24) & 0xff);
    BuilderPut(b, t, 4);
}

/* Emit a well-formed 88-byte TLV container header. */
static void
BuilderHeader(BUILDER *b, const char *HumanReadable, uint32_t Ver, uint32_t Build)
{
    unsigned char hr[FW_VER_HUMAN_READABLE_SZ];
    unsigned char zero8[8] = {0};

    memset(hr, 0, sizeof(hr));
    if (HumanReadable)
        strncpy((char *)hr, HumanReadable, sizeof(hr) - 1);

    BuilderPut32(b, 0);
    BuilderPut32(b, IWL_TLV_UCODE_MAGIC);
    BuilderPut(b, hr, sizeof(hr));
    BuilderPut32(b, Ver);
    BuilderPut32(b, Build);
    BuilderPut(b, zero8, sizeof(zero8));
}

/* Emit one TLV, padding the payload out to a 4-byte boundary. */
static void
BuilderTlv(BUILDER *b, uint32_t Type, const void *Data, uint32_t Length)
{
    static const unsigned char pad[4] = {0, 0, 0, 0};
    uint32_t aligned = (Length + 3u) & ~3u;

    BuilderPut32(b, Type);
    BuilderPut32(b, Length);
    if (Length)
        BuilderPut(b, Data, Length);
    if (aligned > Length)
        BuilderPut(b, pad, aligned - Length);
}

static void
BuilderTlv32(BUILDER *b, uint32_t Type, uint32_t Value)
{
    unsigned char t[4];
    t[0] = (unsigned char)(Value & 0xff);
    t[1] = (unsigned char)((Value >> 8) & 0xff);
    t[2] = (unsigned char)((Value >> 16) & 0xff);
    t[3] = (unsigned char)((Value >> 24) & 0xff);
    BuilderTlv(b, Type, t, 4);
}

static void
BuilderFree(BUILDER *b)
{
    free(b->Buf);
    b->Buf = NULL;
    b->Len = b->Cap = 0;
}

/* ------------------------------------------------------------------ */
/* Cases                                                               */
/* ------------------------------------------------------------------ */

static void
TestHappyPath(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    IWL_FW_PARSE_STATUS s;
    unsigned char sec[4 + 16];
    unsigned char api[8];

    printf("happy path\n");

    BuilderHeader(&b, "reactos-test", 0x59, 0xd2579d43);

    /* One runtime section: destination 0x00800000 plus 16 bytes. */
    memset(sec, 0xAB, sizeof(sec));
    sec[0] = 0x00; sec[1] = 0x00; sec[2] = 0x80; sec[3] = 0x00;
    BuilderTlv(&b, IWL_UCODE_TLV_SEC_RT, sec, sizeof(sec));

    /* API bitmap word 1. */
    api[0] = 1; api[1] = api[2] = api[3] = 0;
    api[4] = 0x34; api[5] = 0x12; api[6] = 0; api[7] = 0;
    BuilderTlv(&b, IWL_UCODE_TLV_API_CHANGES_SET, api, sizeof(api));

    BuilderTlv32(&b, IWL_UCODE_TLV_NUM_OF_CPU, 2);
    BuilderTlv32(&b, IWL_UCODE_TLV_N_SCAN_CHANNELS, 40);
    /* A type this build has never heard of - must be counted, not fatal. */
    BuilderTlv32(&b, 0x7FFFFFFF, 0xdeadbeef);

    s = IwlParseUcodeFile(b.Buf, b.Len, &p);
    CheckStatus(s, IwlFwParseOk, "well-formed container parses");
    Check(strcmp((char *)p.HumanReadable, "reactos-test") == 0, "human-readable string");
    Check(p.UcodeVer == 0x59, "ucode version");
    Check(p.Build == 0xd2579d43, "build id");
    Check(p.Image[IWL_UCODE_REGULAR].SectionCount == 1, "one regular section");
    Check(p.Image[IWL_UCODE_REGULAR].Section[0].Offset == 0x00800000, "section dest offset");
    Check(p.Image[IWL_UCODE_REGULAR].Section[0].Length == 16, "section length excludes offset word");
    Check(p.Image[IWL_UCODE_REGULAR].Section[0].Data[0] == 0xAB, "section data points past offset word");
    Check(p.ApiFlags[1] == 0x1234, "api bitmap word 1");
    Check(p.NumOfCpus == 2, "num of cpus");
    Check(p.NScanChannels == 40, "n scan channels");
    Check(p.UnknownTlvCount == 1, "unknown TLV counted, not fatal");
    Check(p.TlvCount == 5, "total TLV count");

    BuilderFree(&b);
}

static void
TestTruncatedHeader(void)
{
    unsigned char buf[16] = {0};
    IWL_FW_PARSED p;

    printf("truncated header\n");
    CheckStatus(IwlParseUcodeFile(buf, 0, &p), IwlFwParseTooSmall, "zero-length file");
    CheckStatus(IwlParseUcodeFile(buf, sizeof(buf), &p), IwlFwParseTooSmall, "16-byte file");
}

static void
TestLegacyContainer(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;

    printf("legacy container\n");
    BuilderHeader(&b, "x", 1, 1);
    /* Non-zero first word is what marks the pre-TLV format. */
    b.Buf[0] = 0x01;
    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseNotTlv,
                "v1 container rejected, not mis-parsed");
    BuilderFree(&b);
}

static void
TestBadMagic(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;

    printf("bad magic\n");
    BuilderHeader(&b, "x", 1, 1);
    b.Buf[4] ^= 0xFF;
    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseBadMagic, "corrupt magic");
    BuilderFree(&b);
}

static void
TestTlvLengthOverflow(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char payload[8] = {0};

    printf("TLV length past end of file\n");
    BuilderHeader(&b, "x", 1, 1);
    BuilderTlv(&b, IWL_UCODE_TLV_SEC_RT, payload, sizeof(payload));
    /* Rewrite that TLV's length word to claim far more than the file holds. */
    b.Buf[88 + 4] = 0xFF;
    b.Buf[88 + 5] = 0xFF;
    b.Buf[88 + 6] = 0xFF;
    b.Buf[88 + 7] = 0x7F;
    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseTlvLengthOverflow,
                "oversized TLV length rejected before any read");
    BuilderFree(&b);
}

static void
TestUnpaddedFinalTlv(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char payload[5];

    printf("unpadded final TLV\n");
    memset(payload, 0x11, sizeof(payload));

    BuilderHeader(&b, "x", 1, 1);
    /* Emit the TLV header and a 5-byte payload by hand, WITHOUT the three
     * padding bytes ALIGN(5,4) would demand.  Upstream's
     * `len -= ALIGN(tlv_len, 4)` underflows its size_t here and walks off
     * the end; the clamp in IwlParseUcodeFile must absorb it instead. */
    BuilderPut32(&b, IWL_UCODE_TLV_NUM_OF_CPU);
    BuilderPut32(&b, sizeof(payload));
    BuilderPut(&b, payload, sizeof(payload));

    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseOk,
                "short final TLV absorbed rather than underflowing");
    BuilderFree(&b);
}

static void
TestTrailingGarbage(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char junk[4] = {1, 2, 3, 4};

    printf("trailing garbage\n");
    BuilderHeader(&b, "x", 1, 1);
    BuilderTlv32(&b, IWL_UCODE_TLV_NUM_OF_CPU, 2);
    /* Four trailing bytes look like the start of a TLV header but there is
     * no length word behind them. */
    BuilderPut(&b, junk, sizeof(junk));
    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseTruncatedTlv,
                "bytes after the last TLV rejected");
    BuilderFree(&b);
}

static void
TestShortSectionPayload(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char payload[2] = {0, 0};

    printf("SEC_RT payload shorter than its offset word\n");
    BuilderHeader(&b, "x", 1, 1);
    BuilderTlv(&b, IWL_UCODE_TLV_SEC_RT, payload, sizeof(payload));
    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseBadSectionLength,
                "2-byte SEC_RT rejected");
    BuilderFree(&b);
}

static void
TestEmptySection(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char payload[4];

    printf("separator section (offset word only)\n");
    payload[0] = 0xCC; payload[1] = 0xCC; payload[2] = 0xFF; payload[3] = 0xFF;

    BuilderHeader(&b, "x", 1, 1);
    BuilderTlv(&b, IWL_UCODE_TLV_SEC_RT, payload, sizeof(payload));
    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseOk,
                "CPU1/CPU2 separator accepted");
    Check(p.Image[IWL_UCODE_REGULAR].SectionCount == 1, "separator stored as a section");
    Check(p.Image[IWL_UCODE_REGULAR].Section[0].Offset == CPU1_CPU2_SEPARATOR_SECTION,
          "separator marker preserved");
    Check(p.Image[IWL_UCODE_REGULAR].Section[0].Length == 0, "separator carries no data");
    BuilderFree(&b);
}

static void
TestTooManySections(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char sec[8] = {0};
    int i;

    printf("more sections than IWL_UCODE_SECTION_MAX\n");
    BuilderHeader(&b, "x", 1, 1);
    for (i = 0; i < IWL_UCODE_SECTION_MAX + 1; i++)
        BuilderTlv(&b, IWL_UCODE_TLV_SEC_RT, sec, sizeof(sec));

    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseTooManySections,
                "section overflow rejected rather than truncated");
    BuilderFree(&b);
}

/*
 * An API/capability word past the end of our bitmap is what a firmware
 * NEWER than this driver looks like.  It must load anyway, with the extra
 * word counted and dropped - never rejected, or the driver could only ever
 * run firmware at or below the revision it was written against.  Found by
 * running this harness against a real AX211 blob, which does exactly this.
 */
static void
TestApiIndexPastBitmap(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char api[8] = {0};

    printf("API index past the end of our bitmap\n");
    api[0] = 0xFF;  /* word 255, far past IWL_NUM_API_WORDS */
    api[4] = 0x99;
    BuilderHeader(&b, "x", 1, 1);
    BuilderTlv(&b, IWL_UCODE_TLV_API_CHANGES_SET, api, sizeof(api));
    BuilderTlv(&b, IWL_UCODE_TLV_ENABLED_CAPABILITIES, api, sizeof(api));
    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseOk,
                "newer-than-driver firmware still loads");
    Check(p.TruncatedApiWordCount == 2, "both out-of-range words counted");
    Check(p.ApiFlags[0] == 0 && p.CapaFlags[0] == 0,
          "out-of-range word did not scribble on word 0");
    BuilderFree(&b);
}

/* A payload too short to even hold the index+flags pair IS malformed. */
static void
TestShortApiPayload(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char api[4] = {0};

    printf("API TLV payload shorter than index+flags\n");
    BuilderHeader(&b, "x", 1, 1);
    BuilderTlv(&b, IWL_UCODE_TLV_API_CHANGES_SET, api, sizeof(api));
    CheckStatus(IwlParseUcodeFile(b.Buf, b.Len, &p), IwlFwParseBadSectionLength,
                "4-byte API TLV rejected");
    BuilderFree(&b);
}

/*
 * Feed every truncation of a valid container to the parser.  None may read
 * out of bounds; under ASan that is what actually enforces the claim.
 */
static void
TestProgressiveTruncation(void)
{
    BUILDER b = {0};
    IWL_FW_PARSED p;
    unsigned char sec[4 + 32];
    size_t n;
    int ok = 1;

    printf("progressive truncation (%d-byte steps under ASan)\n", 1);

    memset(sec, 0x5A, sizeof(sec));
    BuilderHeader(&b, "trunc", 1, 1);
    BuilderTlv(&b, IWL_UCODE_TLV_SEC_RT, sec, sizeof(sec));
    BuilderTlv32(&b, IWL_UCODE_TLV_NUM_OF_CPU, 2);
    BuilderTlv32(&b, IWL_UCODE_TLV_PHY_SKU, 0x1234);

    for (n = 0; n <= b.Len; n++)
    {
        /* Copy to an exact-sized allocation so ASan's redzone sits right
         * at the end - parsing the original buffer with a short length
         * would not catch a one-byte overrun. */
        unsigned char *exact = malloc(n ? n : 1);
        memcpy(exact, b.Buf, n);
        (void)IwlParseUcodeFile(exact, n, &p);
        free(exact);
    }

    Check(ok, "no truncation reads out of bounds");
    BuilderFree(&b);
}

/* ------------------------------------------------------------------ */

static void
DumpRealBlob(const char *Path)
{
    FILE *f;
    long size;
    unsigned char *buf;
    IWL_FW_PARSED p;
    IWL_FW_PARSE_STATUS s;
    int i, j;
    static const char *ImageName[IWL_UCODE_TYPE_MAX] =
        { "REGULAR", "INIT", "WOWLAN", "REGULAR_USNIFFER" };

    f = fopen(Path, "rb");
    if (!f)
    {
        printf("  cannot open %s\n", Path);
        g_Failures++;
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size)
    {
        printf("  short read on %s\n", Path);
        g_Failures++;
        fclose(f);
        free(buf);
        return;
    }
    fclose(f);

    s = IwlParseUcodeFile(buf, (size_t)size, &p);
    printf("%s (%ld bytes)\n", Path, size);
    printf("  status         : %s\n", IwlFwParseStatusName(s));
    CheckStatus(s, IwlFwParseOk, "real blob parses");
    if (s != IwlFwParseOk)
    {
        free(buf);
        return;
    }

    printf("  human readable : %s\n", p.HumanReadable);
    printf("  ucode version  : %u\n", p.UcodeVer);
    printf("  build          : 0x%08x\n", p.Build);
    printf("  TLVs           : %u (%u unrecognised, %u api words past bitmap)\n",
           p.TlvCount, p.UnknownTlvCount, p.TruncatedApiWordCount);
    printf("  cpus           : %u\n", p.NumOfCpus);
    printf("  scan channels  : %u\n", p.NScanChannels);
    printf("  probe max len  : %u\n", p.ProbeMaxLength);
    printf("  phy calib size : %u\n", p.PhyCalibrationSize);
    printf("  api flags      : %08x %08x %08x %08x\n",
           p.ApiFlags[0], p.ApiFlags[1], p.ApiFlags[2], p.ApiFlags[3]);
    printf("  capa flags     : %08x %08x %08x %08x\n",
           p.CapaFlags[0], p.CapaFlags[1], p.CapaFlags[2], p.CapaFlags[3]);

    for (i = 0; i < IWL_UCODE_TYPE_MAX; i++)
    {
        if (p.Image[i].SectionCount == 0)
            continue;
        printf("  image %-16s %u section(s)\n", ImageName[i], p.Image[i].SectionCount);
        for (j = 0; j < (int)p.Image[i].SectionCount; j++)
        {
            uint32_t off = p.Image[i].Section[j].Offset;
            const char *note = "";
            if (off == CPU1_CPU2_SEPARATOR_SECTION) note = "  <- CPU1/CPU2 separator";
            else if (off == PAGING_SEPARATOR_SECTION) note = "  <- paging separator";
            printf("    [%2d] dest 0x%08x  %8u bytes%s\n",
                   j, off, p.Image[i].Section[j].Length, note);
        }
    }

    /* Every section must lie inside the buffer we handed in. */
    for (i = 0; i < IWL_UCODE_TYPE_MAX; i++)
    {
        for (j = 0; j < (int)p.Image[i].SectionCount; j++)
        {
            const unsigned char *d = p.Image[i].Section[j].Data;
            uint32_t len = p.Image[i].Section[j].Length;
            Check(d >= buf && d + len <= buf + size, "section lies inside the container");
        }
    }

    free(buf);
}

int
main(int argc, char **argv)
{
    int i;

    TestTruncatedHeader();
    TestLegacyContainer();
    TestBadMagic();
    TestHappyPath();
    TestTlvLengthOverflow();
    TestUnpaddedFinalTlv();
    TestTrailingGarbage();
    TestShortSectionPayload();
    TestEmptySection();
    TestTooManySections();
    TestApiIndexPastBitmap();
    TestShortApiPayload();
    TestProgressiveTruncation();

    for (i = 1; i < argc; i++)
        DumpRealBlob(argv[i]);

    printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
