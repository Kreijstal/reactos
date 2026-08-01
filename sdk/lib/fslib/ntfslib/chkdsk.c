/*
 * ntfslib - NTFS consistency checker / repair (chkdsk)
 * Orchestrator + on-disk substrate
 * License: GPL-2.0-or-later
 *
 * Portable, allocation-light NTFS checker. Depends only on ntfs_types.h and
 * the MKNTFS_IO read/write-by-offset abstraction, so the same objects compile
 * both inside ReactOS (NTOS_MODE_USER) and standalone in the test harness.
 *
 * This unit: boot-sector parse, $MFT self-map, record/stream I/O, fixup
 * handling, mapping-pair decode, the public NtfsChkVolume() entry point and
 * pass/stage sequencing.  Detection passes live in chkscan.c, repair stages
 * in chkrepair.c; shared internals in chkint.h.
 */

#include "chkint.h"

#if !defined(NTOS_MODE_USER)
int chk_verbose = 0;
#endif

/* ============================================================
 * IO + issue helpers
 * ============================================================ */

int chk_read(const MKNTFS_IO *io, ULONGLONG off, void *buf, ULONG len)
{
    return io->read(io->context, off, buf, len) == 0 ? 0 : -1;
}

int chk_write(const MKNTFS_IO *io, ULONGLONG off, const void *buf, ULONG len)
{
    return io->write(io->context, off, buf, len) == 0 ? 0 : -1;
}

void chk_add_issue(NTFS_CHK_RESULT *res, NTFS_CHK_CODE code,
                   ULONGLONG p0, ULONGLONG p1, int fixed)
{
    if (res->IssueCount < CHK_MAX_ISSUES)
    {
        res->Issues[res->IssueCount].Code   = code;
        res->Issues[res->IssueCount].Param0 = p0;
        res->Issues[res->IssueCount].Param1 = p1;
        res->Issues[res->IssueCount].Fixed  = fixed;
    }
    res->IssueCount++;
    if (fixed)
        res->RepairedCount++;
}

/* ============================================================
 * User-facing message sink (chkdsk-style stage banners + summary)
 *
 * We compose messages without any printf-family dependency so the exact same
 * code links inside ReactOS (no CRT) and in the standalone harness: a message
 * is built by appending literals and decimal numbers into a caller buffer,
 * then handed to opt->Message once.  This is deliberately not on any per-
 * record hot path -- only stage headers, the final summary, and (Verbose)
 * per-issue lines route through here.
 * ============================================================ */

/* Append NUL-terminated src to dst[*pos..cap-1]; always keeps dst NUL-term. */
static void chk_str_append(char *dst, ULONG cap, ULONG *pos, const char *src)
{
    while (src && *src && *pos + 1 < cap)
        dst[(*pos)++] = *src++;
    if (*pos < cap)
        dst[*pos] = '\0';
}

/* Append an unsigned 64-bit value in decimal. */
static void chk_num_append(char *dst, ULONG cap, ULONG *pos, ULONGLONG v)
{
    char tmp[24];
    int i = 0;
    if (v == 0)
        tmp[i++] = '0';
    while (v && i < (int)sizeof(tmp))
    {
        tmp[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (i > 0 && *pos + 1 < cap)
        dst[(*pos)++] = tmp[--i];
    if (*pos < cap)
        dst[*pos] = '\0';
}

/* Emit a single pre-built line through the sink (adds nothing; caller owns
 * newline conventions).  No-op when no sink is installed. */
void chk_emit(const NTFS_CHK_OPTIONS *opt, const char *msg)
{
    if (opt && opt->Message)
        opt->Message(opt->MessageCtx, msg);
}

/* ============================================================
 * Update-sequence-array (fixup) handling
 * ============================================================ */

/* Apply fixups on a just-read record/INDX buffer; returns -1 on mismatch. */
int chk_apply_fixups(UCHAR *buf, ULONG size, ULONG sectorSize)
{
    USHORT usaOff = *(USHORT *)(buf + 4);
    USHORT usaCnt = *(USHORT *)(buf + 6);
    USHORT *usa, usn;
    ULONG sectors, i;

    if (usaCnt == 0)
        return 0;
    if ((ULONG)usaOff + (ULONG)usaCnt * 2 > size)
        return -1;
    sectors = usaCnt - 1;
    if (sectors * sectorSize > size)
        return -1;

    usa = (USHORT *)(buf + usaOff);
    usn = usa[0];
    for (i = 0; i < sectors; i++)
    {
        USHORT *tail = (USHORT *)(buf + (i + 1) * sectorSize - 2);
        if (*tail != usn)
            return -1;         /* torn write / corruption */
        *tail = usa[i + 1];    /* restore original */
    }

    /* Normalize untrusted size fields on FILE records: BytesAllocated must
     * equal the true record size (a corrupt larger value would defeat every
     * later capacity check against the record buffer), and BytesInUse can
     * never exceed it.  In-memory only; the corrected values reach disk
     * whenever a repair rewrites the record. */
    if (*(ULONG *)buf == NRH_FILE_TYPE)
    {
        FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)buf;
        if (h->BytesAllocated != size)
            h->BytesAllocated = size;
        if (h->BytesInUse > size)
            h->BytesInUse = size;
    }
    return 0;
}

/* Prepare a record for write-back: bump USN, save tails, stamp. */
void chk_stamp_fixups(UCHAR *buf, ULONG size, ULONG sectorSize)
{
    USHORT usaOff = *(USHORT *)(buf + 4);
    USHORT usaCnt = *(USHORT *)(buf + 6);
    USHORT *usa, usn;
    ULONG sectors, i;

    if (usaCnt == 0)
        return;
    if ((ULONG)usaOff + (ULONG)usaCnt * 2 > size)
        return;
    sectors = usaCnt - 1;
    if (sectors * sectorSize > size)
        return;

    usa = (USHORT *)(buf + usaOff);
    usn = (USHORT)(usa[0] + 1);
    if (usn == 0 || usn == 0xFFFF)
        usn = 1;
    usa[0] = usn;
    for (i = 0; i < sectors; i++)
    {
        USHORT *tail = (USHORT *)(buf + (i + 1) * sectorSize - 2);
        usa[i + 1] = *tail;
        *tail = usn;
    }
}

/* ============================================================
 * Mapping pairs + stream I/O
 * ============================================================ */

/* Decoder: fills runs[], returns run count (-1 on error). */
int chk_decode_runs(const UCHAR *p, const UCHAR *end,
                    CHK_RUN *runs, ULONG maxRuns)
{
    LONGLONG curLcn = 0;
    ULONG n = 0;

    while (p < end && *p != 0)
    {
        UCHAR hdr = *p++;
        int lenBytes = hdr & 0x0F;
        int offBytes = (hdr >> 4) & 0x0F;
        ULONGLONG len = 0;
        LONGLONG off = 0;
        int i;

        if (lenBytes == 0 || p + lenBytes + offBytes > end)
            return -1;
        for (i = 0; i < lenBytes; i++)
            len |= (ULONGLONG)p[i] << (8 * i);
        p += lenBytes;

        if (n >= maxRuns)
            return -1;

        if (offBytes == 0)
        {
            runs[n].Sparse = 1;
            runs[n].Lcn = 0;
            runs[n].Len = len;
        }
        else
        {
            for (i = 0; i < offBytes; i++)
                off |= (LONGLONG)p[i] << (8 * i);
            if (p[offBytes - 1] & 0x80)               /* sign-extend */
                for (i = offBytes; i < 8; i++)
                    off |= (LONGLONG)0xFF << (8 * i);
            p += offBytes;
            curLcn += off;
            runs[n].Sparse = 0;
            runs[n].Lcn = (ULONGLONG)curLcn;
            runs[n].Len = len;
        }
        n++;
    }
    return (int)n;
}

/* Read/write a byte range of a non-resident stream via its runlist. */
int chk_stream_io(const MKNTFS_IO *io, const CHK_RUN *runs, ULONG nruns,
                  ULONG clusterSize, ULONGLONG streamOff,
                  UCHAR *buf, ULONGLONG len, int write)
{
    ULONGLONG pos = 0;
    ULONG r;

    for (r = 0; r < nruns && len > 0; r++)
    {
        ULONGLONG runBytes = runs[r].Len * clusterSize;
        if (streamOff < pos + runBytes)
        {
            ULONGLONG into = streamOff - pos;
            ULONGLONG avail = runBytes - into;
            ULONG take = (ULONG)CHK_MIN(len, avail);

            if (runs[r].Sparse)
            {
                if (!write)
                    memset(buf, 0, take);
            }
            else
            {
                ULONGLONG diskOff = runs[r].Lcn * clusterSize + into;
                int rc = write ? chk_write(io, diskOff, buf, take)
                               : chk_read(io, diskOff, buf, take);
                if (rc != 0)
                    return -1;
            }
            buf += take;
            len -= take;
            streamOff += take;
        }
        pos += runBytes;
    }
    return len == 0 ? 0 : -1;
}

/* ============================================================
 * Boot sector + MFT self-map
 * ============================================================ */

int chk_read_boot(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    UCHAR sector[512];
    NTFS_BOOT_SECTOR *bs = (NTFS_BOOT_SECTOR *)sector;
    static const char oem[8] = { 'N','T','F','S',' ',' ',' ',' ' };
    int i;
    CHAR cpr;

    if (chk_read(c->Io, 0, sector, 512) != 0)
        return -1;

    if (bs->EndMarker != 0xAA55)
        { chk_add_issue(res, CHK_ERR_BOOT_SIG, 0, 0, 0); return -1; }
    for (i = 0; i < 8; i++)
        if (((char *)bs->OemId)[i] != oem[i])
            { chk_add_issue(res, CHK_ERR_BOOT_OEM, 0, 0, 0); return -1; }

    c->BytesPerSector = bs->BytesPerSector;
    c->SectorsPerCluster = bs->SectorsPerCluster;
    if (c->BytesPerSector < 256 || c->BytesPerSector > 4096 ||
        (c->BytesPerSector & (c->BytesPerSector - 1)) ||
        c->SectorsPerCluster == 0)
        { chk_add_issue(res, CHK_ERR_BOOT_GEOMETRY, 0, 0, 0); return -1; }

    c->BytesPerCluster = c->BytesPerSector * c->SectorsPerCluster;
    c->MftLcn = bs->MftStartLcn;
    c->TotalClusters = bs->TotalSectors / c->SectorsPerCluster;

    cpr = bs->ClustersPerMftRecord;
    if (cpr < 0)
        c->MftRecordSize = 1u << (-cpr);
    else
        c->MftRecordSize = (ULONG)cpr * c->BytesPerCluster;

    cpr = bs->ClustersPerIndexRecord;
    if (cpr < 0)
        c->IndexRecordSize = 1u << (-cpr);
    else
        c->IndexRecordSize = (ULONG)cpr * c->BytesPerCluster;

    if (c->MftRecordSize < 256 || c->MftRecordSize > 65536 ||
        c->MftRecordSize < c->BytesPerSector)
        { chk_add_issue(res, CHK_ERR_BOOT_GEOMETRY, 0, 0, 0); return -1; }

    CHK_TRACE("boot: bps=%lu spc=%lu bpc=%lu mftrec=%lu mftlcn=%llu totclus=%llu\n",
              (unsigned long)c->BytesPerSector, (unsigned long)c->SectorsPerCluster,
              (unsigned long)c->BytesPerCluster, (unsigned long)c->MftRecordSize,
              (unsigned long long)c->MftLcn, (unsigned long long)c->TotalClusters);
    return 0;
}

/* Find the unnamed non-resident $DATA in a record buffer; decode its runs. */
int chk_find_data_runs(CHK_CTX *c, UCHAR *rec,
                       CHK_RUN *runs, ULONG maxRuns, ULONGLONG *dataSize)
{
    FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
    ULONG off = h->AttributeOffset;

    while (off + sizeof(ATTR_RECORD) <= c->MftRecordSize)
    {
        ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
        if (a->Type == AT_END || a->Length == 0)
            break;
        if (off + a->Length > c->MftRecordSize)
            break;
        if (a->Type == AT_DATA && a->NameLength == 0 && a->NonResident)
        {
            const UCHAR *mp = rec + off + a->NR.MappingPairsOffset;
            const UCHAR *end = rec + off + a->Length;
            int n = chk_decode_runs(mp, end, runs, maxRuns);
            if (n < 0)
                return -1;
            if (dataSize)
                *dataSize = (ULONGLONG)a->NR.DataSize;
            return n;
        }
        off += a->Length;
    }
    return -1;
}

int chk_load_mft(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    UCHAR *rec0;
    int n;

    rec0 = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec0)
        return -1;

    /* $MFT record 0 lives at MftLcn; read it directly. */
    if (chk_read(c->Io, c->MftLcn * c->BytesPerCluster, rec0, c->MftRecordSize) != 0)
        { free(rec0); chk_add_issue(res, CHK_ERR_MFT_UNREADABLE, 0, 0, 0); return -1; }

    if (*(ULONG *)rec0 != NRH_FILE_TYPE ||
        chk_apply_fixups(rec0, c->MftRecordSize, c->BytesPerSector) != 0)
        { free(rec0); chk_add_issue(res, CHK_ERR_MFT_UNREADABLE, 0, 0, 0); return -1; }

    n = chk_find_data_runs(c, rec0, c->MftRuns, CHK_MAX_RUNS, &c->MftDataBytes);
    free(rec0);
    if (n <= 0)
        { chk_add_issue(res, CHK_ERR_MFT_UNREADABLE, 0, 0, 0); return -1; }

    c->MftRunCount = (ULONG)n;
    c->RecordCount = (ULONG)(c->MftDataBytes / c->MftRecordSize);
    CHK_TRACE("mft: runs=%lu dataBytes=%llu records=%lu\n",
              (unsigned long)c->MftRunCount, (unsigned long long)c->MftDataBytes,
              (unsigned long)c->RecordCount);
    return 0;
}

int chk_read_record(CHK_CTX *c, ULONG recno, UCHAR *buf)
{
    return chk_stream_io(c->Io, c->MftRuns, c->MftRunCount, c->BytesPerCluster,
                         (ULONGLONG)recno * c->MftRecordSize, buf, c->MftRecordSize, 0);
}

int chk_write_record(CHK_CTX *c, ULONG recno, UCHAR *buf)
{
    return chk_stream_io(c->Io, c->MftRuns, c->MftRunCount, c->BytesPerCluster,
                         (ULONGLONG)recno * c->MftRecordSize, buf, c->MftRecordSize, 1);
}

/* ============================================================
 * Attribute lookup / value read
 * ============================================================ */

/* Locate an attribute inside a fixup-applied record; returns offset or 0. */
ULONG chk_find_attr(CHK_CTX *c, UCHAR *rec, ULONG type,
                    const WCHAR *name, ULONG nameLen)
{
    FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
    ULONG off = h->AttributeOffset;

    while (off + 8 <= c->MftRecordSize)
    {
        ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
        if (a->Type == AT_END || a->Length == 0 || off + a->Length > c->MftRecordSize)
            break;
        if (a->Type == type && a->NameLength == nameLen)
        {
            if (nameLen == 0)
                return off;
            if (a->NameOffset + nameLen * sizeof(WCHAR) <= a->Length)
            {
                const WCHAR *an = (const WCHAR *)(rec + off + a->NameOffset);
                ULONG i;
                for (i = 0; i < nameLen && an[i] == name[i]; i++)
                    ;
                if (i == nameLen)
                    return off;
            }
        }
        off += a->Length;
    }
    return 0;
}

/* Read an attribute's whole value (resident or via runlist); caller frees. */
UCHAR *chk_read_attr_value(CHK_CTX *c, UCHAR *rec, ULONG attrOff,
                           ULONGLONG maxBytes, ULONGLONG *sizeOut)
{
    ATTR_RECORD *a = (ATTR_RECORD *)(rec + attrOff);
    UCHAR *buf;
    ULONGLONG size;

    if (!a->NonResident)
    {
        size = a->Resident.ValueLength;
        if (size > maxBytes ||
            attrOff + a->Resident.ValueOffset + size > c->MftRecordSize)
            return NULL;
        buf = (UCHAR *)malloc((size_t)size + 8);
        if (!buf)
            return NULL;
        memcpy(buf, rec + attrOff + a->Resident.ValueOffset, (size_t)size);
    }
    else
    {
        CHK_RUN *runs = (CHK_RUN *)malloc(sizeof(CHK_RUN) * CHK_MAX_RUNS);
        const UCHAR *mp = rec + attrOff + a->NR.MappingPairsOffset;
        const UCHAR *end = rec + attrOff + a->Length;
        int n;

        if (!runs)
            return NULL;
        size = (ULONGLONG)a->NR.DataSize;
        if (size > maxBytes)
            { free(runs); return NULL; }
        n = chk_decode_runs(mp, end, runs, CHK_MAX_RUNS);
        if (n <= 0)
            { free(runs); return NULL; }
        buf = (UCHAR *)malloc((size_t)size + 8);
        if (!buf)
            { free(runs); return NULL; }
        if (chk_stream_io(c->Io, runs, (ULONG)n, c->BytesPerCluster,
                          0, buf, size, 0) != 0)
            { free(buf); free(runs); return NULL; }
        free(runs);
    }
    if (sizeOut)
        *sizeOut = size;
    return buf;
}

/* ============================================================
 * $UpCase + filename collation
 * ============================================================ */

int chk_load_upcase(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    UCHAR *rec;
    ULONG off;
    ULONGLONG size = 0;
    UCHAR *val = NULL;

    c->UpCase = (WCHAR *)malloc(65536 * sizeof(WCHAR));
    if (!c->UpCase)
        return -1;

    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (rec &&
        chk_read_record(c, FILE_UpCase, rec) == 0 &&
        *(ULONG *)rec == NRH_FILE_TYPE &&
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) == 0 &&
        (off = chk_find_attr(c, rec, AT_DATA, NULL, 0)) != 0)
    {
        val = chk_read_attr_value(c, rec, off, 65536 * sizeof(WCHAR), &size);
    }
    free(rec);

    if (val && size == 65536 * sizeof(WCHAR))
    {
        memcpy(c->UpCase, val, 65536 * sizeof(WCHAR));
        free(val);
        return 0;
    }

    /* Unreadable or wrong size: report once, fall back to the builtin table. */
    if (val)
        free(val);
    chk_add_issue(res, CHK_ERR_UPCASE_BAD, size, 0, 0);
    {
        extern void ntfs_upcase_table_build(WCHAR *uc, ULONG uc_len);
        ntfs_upcase_table_build(c->UpCase, 65536);
    }
    return 0;
}

/* COLLATION_FILENAME: upcase-mapped per position; shorter prefix first;
 * case-sensitive tie-break (matches ntfs-3g ntfs_names_full_collate). */
int chk_collate_filename(const CHK_CTX *c,
                         const WCHAR *a, ULONG alen,
                         const WCHAR *b, ULONG blen)
{
    ULONG i, n = CHK_MIN(alen, blen);

    if (c->UpCase)
    {
        for (i = 0; i < n; i++)
        {
            WCHAR ua = c->UpCase[a[i]], ub = c->UpCase[b[i]];
            if (ua != ub)
                return ua < ub ? -1 : 1;
        }
    }
    else
    {
        for (i = 0; i < n; i++)
            if (a[i] != b[i])
                return a[i] < b[i] ? -1 : 1;
    }
    if (alen != blen)
        return alen < blen ? -1 : 1;
    /* equal case-insensitively: case-sensitive tie-break */
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* ============================================================
 * Public entry point
 * ============================================================ */

int NtfsChkVolume(const MKNTFS_IO *io, const NTFS_CHK_OPTIONS *opt,
                  NTFS_CHK_RESULT *res)
{
    CHK_CTX *c;   /* ~197 KiB: too large for the stack */
    UCHAR *computed = NULL;
    ULONGLONG bmpBytes;
    int rc = 0;
    ULONG unfixed;

    memset(res, 0, sizeof(*res));
    c = (CHK_CTX *)malloc(sizeof(*c));
    if (!c)
        { res->ExitStatus = 1; return -1; }
    memset(c, 0, sizeof(*c));
    c->Io = io;

#if !defined(NTOS_MODE_USER)
    chk_verbose = opt->Verbose;
#endif

    if (chk_read_boot(c, res) != 0)
        { res->ExitStatus = 1; free(c); return -1; }
    if (chk_load_mft(c, res) != 0)
        { res->ExitStatus = 1; free(c); return -1; }

    /* Pass 3 seeds WasDirty; honor CheckOnlyIfDirty by peeking record 3 first. */
    if (opt->CheckOnlyIfDirty)
    {
        UCHAR *rec = (UCHAR *)malloc(c->MftRecordSize);
        int dirty = 0;
        if (rec)
        {
            if (chk_read_record(c, FILE_Volume, rec) == 0 &&
                *(ULONG *)rec == NRH_FILE_TYPE &&
                chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) == 0)
            {
                FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
                ULONG off = h->AttributeOffset;
                while (off + 8 <= c->MftRecordSize)
                {
                    ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
                    if (a->Type == AT_END || a->Length == 0 ||
                        off + a->Length > c->MftRecordSize)
                        break;
                    if (a->Type == AT_VOLUME_INFORMATION && !a->NonResident)
                    {
                        VOLUME_INFORMATION *vi =
                            (VOLUME_INFORMATION *)(rec + off + a->Resident.ValueOffset);
                        dirty = (vi->Flags & VOLUME_IS_DIRTY) != 0;
                        break;
                    }
                    off += a->Length;
                }
            }
            free(rec);
        }
        if (!dirty)
        {
            res->ExitStatus = 0;
            free(c);
            return 0;
        }
    }

    bmpBytes = (c->TotalClusters + 7) / 8;
    computed = (UCHAR *)calloc(1, (size_t)bmpBytes + 8);
    if (!computed)
        { res->ExitStatus = 1; free(c); return -1; }
    c->ClusterMap = computed;

    /* Model allocations.  On failure: degrade to the substrate-only checks
     * (report CHK_ERR_NOMEM) rather than fail or crash. */
    c->Rec = (CHK_REC *)calloc(c->RecordCount, sizeof(CHK_REC));
    c->MftMap = (UCHAR *)calloc(1, (size_t)(c->RecordCount + 7) / 8 + 8);
    if (!c->Rec || !c->MftMap)
        chk_add_issue(res, CHK_ERR_NOMEM, c->RecordCount, 0, 0);

    if (chk_load_upcase(c, res) != 0)
        chk_add_issue(res, CHK_ERR_NOMEM, 0, 1, 0);

    /* Stage 1: basic file system structure -- record scan + attr/bitmap. */
    chk_emit(opt, "\r\nCHKDSK is verifying files (stage 1 of 3)...\r\n");

    /* Pass 1: record scan (fills model + cluster accounting) */
    if (chk_scan_records(c, computed, res, opt) != 0)
        rc = -1;

    if (c->Rec && c->MftMap)
    {
        /* Stage 2: file name linkage -- $I30 walk + connectivity. */
        chk_emit(opt, "CHKDSK is verifying indexes (stage 2 of 3)...\r\n");

        /* Pass 2: $I30 walk (back-refs, ordering, cycles; fills LinksSeen) */
        chk_walk_indexes(c, res, opt);

        /* Pass 3: connectivity (orphans, dir cycles, link counts) */
        chk_connectivity(c, res, opt);

        /* Pass 4: $MFT:$BITMAP cross-check */
        chk_check_mft_bitmap(c, res, opt);
    }

    /* R0: arm the volume dirty flag before the first mutating repair, so an
     * interrupted repair is re-detected on the next mount/check.  Fires when
     * FixErrors and there is anything to repair (detected issues), even if the
     * volume was not originally dirty.  R8 (dirty-clear) is the last write. */
    if (opt->FixErrors && res->IssueCount > 0)
    {
        if (chk_set_volume_flag(c, 1, 0) == 0 && c->Io->flush)
            c->Io->flush(c->Io->context);
    }

    /* S4 record/attribute repairs (need the model; run before it is freed).
     * Order: R1 attr truncation, R1 crosslink, R4 link counts, R5 mft-bitmap. */
    if (opt->FixErrors)
    {
        chk_repair_attributes(c, res);
        chk_repair_crosslinks(c, res);

        /* Snapshot "which record is named in which directory" from a single
         * MFT pass, now that R1's record surgery has settled.  R2 and R3 read
         * it instead of re-scanning the volume per directory / per orphan; on
         * OOM they are skipped rather than allowed to rescan, because that
         * shape does not terminate on a real volume. */
        if (chk_build_repair_index(c) != 0)
            chk_add_issue(res, CHK_ERR_NOMEM, c->RecordCount, 2, 0);

        /* R2: $I30 directory-index rebuild.  Runs after R1 record surgery and
         * before R4/R5/R6 so the corrected index feeds link-count and bitmap
         * reconciliation.  Only directories whose index is bad and whose $I30
         * is wholly within the base record (not attr-list spread) qualify. */
        if (c->Rec && c->ChildStart)
        {
            ULONG d;
            for (d = 0; d < c->RecordCount; d++)
            {
                USHORT f = c->Rec[d].Flags;
                if ((f & (CRF_IN_USE | CRF_DIRECTORY | CRF_INDEX_BAD)) ==
                        (CRF_IN_USE | CRF_DIRECTORY | CRF_INDEX_BAD) &&
                    !(f & (CRF_ATTRLIST_UNSUPPORTED | CRF_CORRUPT | CRF_EXTENSION)))
                    chk_rebuild_index(c, d, res);
            }
        }

        /* R3: orphan recovery.  Runs after R2 (so directories whose index was
         * rebuilt have already re-homed their own children and cleared their
         * CRF_ORPHAN) and before R4 (so the re-homed records' link counts are
         * computed against the found.NNN + refreshed root indexes). */
        if (c->Rec && c->MftMap && c->ChildStart)
            chk_recover_orphans(c, res);

        if (c->Rec)
            chk_repair_linkcounts(c, res);
        if (c->MftMap)
            chk_repair_mft_bitmap(c, res);
    }

    /* Stage 3: free-space / security -- $Bitmap + $MFTMirr cross-checks. */
    chk_emit(opt, "CHKDSK is verifying free space (stage 3 of 3)...\r\n");

    /* Pass 5 / R6: $Bitmap cross-check + repair (both directions) */
    if (chk_crosscheck_bitmap(c, computed, res, opt) != 0)
        CHK_TRACE("bitmap cross-check failed to complete\n");

    /* Pass 6: $MFTMirr compare */
    chk_check_mftmirr(c, res, opt);

    chk_free_repair_index(c);
    free(computed);
    c->ClusterMap = NULL;
    free(c->Rec);
    c->Rec = NULL;
    free(c->MftMap);
    c->MftMap = NULL;
    free(c->UpCase);
    c->UpCase = NULL;

    /* R8 clean-stamp (H8): before clearing the dirty flag, reset $LogFile to
     * the empty/clean state so a stale journal is not replayed over the just-
     * repaired metadata on the next mount.  Only when we repaired something. */
    if (opt->FixErrors && (res->RepairedCount > 0 || res->WasDirty))
    {
        if (chk_stamp_logfile_clean(c, res) != 0)
            CHK_TRACE("R8: $LogFile clean-stamp did not complete\n");
    }

    /* $Volume dirty flag (R8 disarm).  Clear + mark MODIFIED_BY_CHKDSK when the
     * volume was originally dirty OR any repair happened this run (R0 armed it).
     * This is deliberately the last write.
     *
     * Only stamp the volume clean when the run actually converged.  Clearing
     * the flag after a partial repair is worse than not repairing at all: the
     * next boot's CheckOnlyIfDirty probe sees a clean volume, skips the check,
     * and the damage this run could not fix is never revisited -- the volume
     * stays broken while reporting healthy.  Windows' chkdsk leaves the flag
     * set when uncorrected damage remains, so autochk comes back for it. */
    unfixed = res->IssueCount - res->RepairedCount;

    if (res->WasDirty)
    {
        int fixed = 0;
        if (opt->FixErrors && unfixed == 0 && chk_set_volume_flag(c, 0, 1) == 0)
            fixed = 1;
        /* When the flag is deliberately left set, this records itself as an
         * unfixed issue, which is what drives ExitStatus to 1 below. */
        chk_add_issue(res, CHK_ERR_VOLUME_DIRTY, 0, 0, fixed);
    }
    else if (opt->FixErrors && res->RepairedCount > 0 && unfixed == 0)
    {
        /* R0 armed it above; disarm now (no VOLUME_DIRTY issue was recorded). */
        chk_set_volume_flag(c, 0, 1);
        if (c->Io->flush)
            c->Io->flush(c->Io->context);
    }

    free(c);

    unfixed = res->IssueCount - res->RepairedCount;
    if (res->IssueCount == 0)
        res->ExitStatus = 0;
    else if (unfixed == 0)
        res->ExitStatus = 2;
    else
        res->ExitStatus = 1;

    /* Final chkdsk-style summary.  Always emitted (independent of Verbose).
     * Composed without printf so it links identically in-tree and standalone. */
    if (opt->Message)
    {
        char line[160];
        ULONG p;

        /* Verdict line, matching chkdsk's tone. */
        if (res->IssueCount == 0)
            chk_emit(opt, "\r\nWindows has checked the file system and found no problems.\r\n");
        else if (unfixed == 0)
            chk_emit(opt, "\r\nWindows has made corrections to the file system.\r\n");
        else
            chk_emit(opt, "\r\nWindows found problems that were not all corrected.\r\n");

        p = 0; line[0] = '\0';
        chk_num_append(line, sizeof(line), &p, res->RecordsScanned);
        chk_str_append(line, sizeof(line), &p, " file records processed (");
        chk_num_append(line, sizeof(line), &p, res->RecordsInUse);
        chk_str_append(line, sizeof(line), &p, " in use).\r\n");
        chk_emit(opt, line);

        p = 0; line[0] = '\0';
        chk_num_append(line, sizeof(line), &p, res->ClustersUsed);
        chk_str_append(line, sizeof(line), &p, " clusters in use, ");
        chk_num_append(line, sizeof(line), &p, res->ClustersReserved);
        chk_str_append(line, sizeof(line), &p, " reserved/unreferenced.\r\n");
        chk_emit(opt, line);

        p = 0; line[0] = '\0';
        chk_num_append(line, sizeof(line), &p, res->IssueCount);
        chk_str_append(line, sizeof(line), &p, " problems found, ");
        chk_num_append(line, sizeof(line), &p, res->RepairedCount);
        chk_str_append(line, sizeof(line), &p, " repaired.\r\n");
        chk_emit(opt, line);

        /* Only surface the repair-stage counters when repairs actually ran, so
         * a clean read-only pass stays quiet. */
        if (res->OrphansRecovered || res->IndexesRebuilt ||
            res->LinksFixed || res->AttrsTruncated)
        {
            p = 0; line[0] = '\0';
            chk_num_append(line, sizeof(line), &p, res->OrphansRecovered);
            chk_str_append(line, sizeof(line), &p, " orphans recovered, ");
            chk_num_append(line, sizeof(line), &p, res->IndexesRebuilt);
            chk_str_append(line, sizeof(line), &p, " indexes rebuilt, ");
            chk_num_append(line, sizeof(line), &p, res->LinksFixed);
            chk_str_append(line, sizeof(line), &p, " links fixed, ");
            chk_num_append(line, sizeof(line), &p, res->AttrsTruncated);
            chk_str_append(line, sizeof(line), &p, " attributes truncated.\r\n");
            chk_emit(opt, line);
        }
    }

    (void)rc;
    return res->ExitStatus;
}

const char *NtfsChkCodeString(NTFS_CHK_CODE code)
{
    switch (code)
    {
    case CHK_OK:                    return "ok";
    case CHK_ERR_BOOT_SIG:          return "boot sector signature invalid";
    case CHK_ERR_BOOT_OEM:          return "boot OEM id is not NTFS";
    case CHK_ERR_BOOT_GEOMETRY:     return "boot geometry invalid";
    case CHK_ERR_MFT_UNREADABLE:    return "$MFT record 0 unreadable";
    case CHK_ERR_REC_MAGIC:         return "record has no FILE magic";
    case CHK_ERR_USA_MISMATCH:      return "update sequence array mismatch";
    case CHK_ERR_ATTR_OVERRUN:      return "attribute overruns record";
    case CHK_ERR_ATTR_NO_STDINFO:   return "in-use record missing $STANDARD_INFORMATION";
    case CHK_ERR_RUN_OOB:           return "data run references cluster past volume end";
    case CHK_ERR_BITMAP_UNDERALLOC: return "cluster in use but $Bitmap marks it free";
    case CHK_ERR_VOLUME_DIRTY:      return "volume dirty flag is set";
    case CHK_ERR_INDEX_BADREF:      return "$I30 entry references impossible MFT record";
    case CHK_ERR_ATTRLIST_BROKEN:   return "$ATTRIBUTE_LIST entry invalid";
    case CHK_ERR_LOST_EXTENSION:    return "extension record not claimed by any base";
    case CHK_ERR_INDEX_ORDER:       return "$I30 entries out of collation order";
    case CHK_ERR_INDEX_BACKREF:     return "$I30 entry not backed by target $FILE_NAME";
    case CHK_ERR_INDEX_CYCLE:       return "$I30 b-tree cycle";
    case CHK_ERR_INDEX_STRUCT:      return "$I30 index structure invalid";
    case CHK_ERR_INDX_BAD_BLOCK:    return "INDX block unreadable or wrong VCN";
    case CHK_ERR_DIR_CYCLE:         return "directory parent chain loops";
    case CHK_ERR_ORPHAN:            return "record not referenced by any directory";
    case CHK_ERR_LINKCOUNT:         return "link count mismatch";
    case CHK_ERR_XLINK:             return "cluster cross-linked between attributes";
    case CHK_ERR_MFTBMP_MISMATCH:   return "$MFT bitmap disagrees with records";
    case CHK_ERR_BITMAP_OVERALLOC:  return "cluster marked used but unreferenced";
    case CHK_ERR_MFTMIRR_MISMATCH:  return "$MFTMirr differs from $MFT";
    case CHK_ERR_UPCASE_BAD:        return "$UpCase unreadable or wrong size";
    case CHK_ERR_ATTR_TRUNCATED:    return "corrupt attribute truncated";
    case CHK_ERR_NOMEM:             return "out of memory: detection degraded";
    default:                        return "unknown";
    }
}
