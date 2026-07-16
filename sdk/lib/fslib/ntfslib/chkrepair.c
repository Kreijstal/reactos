/*
 * ntfslib - NTFS consistency checker: repair stages
 * License: GPL-2.0-or-later
 *
 * Mutating repair stages, run only under opt->FixErrors.  Every write goes
 * through chk_stamp_fixups + chk_write_record / chk_stream_io.
 *
 * Currently implemented: $Volume dirty-flag manipulation (arm/disarm +
 * repair) and the test-seeding utilities.  The full repair state machine
 * (record surgery, $I30 rebuild, orphan recovery, bitmap writes, $MFTMirr
 * resync) lands here stage by stage.
 */

#include "chkint.h"

/* ============================================================
 * $MFTMirr resync (H11: every write to a mirrored record -- the first
 * DataSize/RecordSize records, normally 0-3 -- must hit the mirror too)
 * ============================================================ */

int chk_sync_mftmirr_record(CHK_CTX *c, ULONG recno)
{
    UCHAR *rec = NULL, *raw = NULL;
    CHK_RUN runs[64];
    ULONGLONG size = 0;
    int n = 0, rc = -1;

    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return -1;
    if (chk_read_record(c, FILE_MFTMirr, rec) == 0 &&
        *(ULONG *)rec == NRH_FILE_TYPE &&
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) == 0)
    {
        n = chk_find_data_runs(c, rec, runs, 64, &size);
    }

    if (n > 0 && (ULONGLONG)(recno + 1) * c->MftRecordSize <= size)
    {
        raw = (UCHAR *)malloc(c->MftRecordSize);
        if (raw &&
            chk_read_record(c, recno, raw) == 0 &&
            chk_stream_io(c->Io, runs, (ULONG)n, c->BytesPerCluster,
                          (ULONGLONG)recno * c->MftRecordSize,
                          raw, c->MftRecordSize, 1) == 0)
            rc = 0;
        free(raw);
    }
    else if (n > 0)
    {
        rc = 0;   /* record not mirrored: nothing to sync */
    }

    free(rec);
    return rc;
}

/* ============================================================
 * $Volume dirty-flag repair
 * ============================================================ */

int chk_set_volume_flag(CHK_CTX *c, int setDirty, int markChkdsk)
{
    UCHAR *rec = (UCHAR *)malloc(c->MftRecordSize);
    FILE_RECORD_HEADER *h;
    ULONG off;
    int done = -1;

    if (!rec)
        return -1;
    if (chk_read_record(c, FILE_Volume, rec) != 0 ||
        *(ULONG *)rec != NRH_FILE_TYPE ||
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
        { free(rec); return -1; }

    h = (FILE_RECORD_HEADER *)rec;
    off = h->AttributeOffset;
    while (off + 8 <= c->MftRecordSize)
    {
        ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
        if (a->Type == AT_END || a->Length == 0 || off + a->Length > c->MftRecordSize)
            break;
        if (a->Type == AT_VOLUME_INFORMATION && !a->NonResident)
        {
            VOLUME_INFORMATION *vi =
                (VOLUME_INFORMATION *)(rec + off + a->Resident.ValueOffset);
            if (setDirty)
                vi->Flags |= VOLUME_IS_DIRTY;
            else
                vi->Flags &= (USHORT)~VOLUME_IS_DIRTY;
            if (markChkdsk)
                vi->Flags |= VOLUME_MODIFIED_BY_CHKDSK;
            done = 0;
            break;
        }
        off += a->Length;
    }

    if (done == 0)
    {
        chk_stamp_fixups(rec, c->MftRecordSize, c->BytesPerSector);
        if (chk_write_record(c, FILE_Volume, rec) != 0)
            done = -1;
        else if (chk_sync_mftmirr_record(c, FILE_Volume) != 0)
            done = -1;
    }
    free(rec);
    return done;
}

/* ============================================================
 * $LogFile clean-stamp (H8: mark the journal clean so a stale log is
 * not replayed over the just-repaired metadata on the next mount)
 * ============================================================ */

/* Log page size: matches mkntfs's lfs_page_size and the driver's LfsPageSize
 * -- 4 KiB unless the cluster is larger, in which case the page tracks the
 * cluster.  One authority; keep in step with mkntfs.c:lfs_page_size. */
static __inline ULONG chk_lfs_page_size(const CHK_CTX *c)
{
    return (c->BytesPerCluster > 4096) ? c->BytesPerCluster : 4096;
}

/* Reset $LogFile (MFT record 2) to the empty, dismounted-cleanly state.
 *
 * H8 rationale: we do NOT synthesize a speculative redo/undo record stream
 * (whose on-disk layout is not yet oracle-confirmed).  We only write the two
 * byte-identical CLEAN 'RSTR' restart pages -- the exact "empty log" landmark
 * mkntfs writes at format time (ntfs_build_rstr_page) and that the ReactOS
 * driver's NtfsLfsCheckCleanShutdown accepts.  This makes LFS replay a no-op,
 * which is precisely what `chkdsk /f` does after repairing metadata: it drops
 * the pending journal rather than replaying it.  The layout is unambiguous
 * (ntfs_types.h:NTFS_LFS_RESTART_AREA + NTFS_LFS_RESTART_FLAG_CLEAN, sourced
 * from libfsntfs and cross-checked against the driver parser), so this is the
 * Windows-correct SAFE subset -- not a guess.
 *
 * Idempotent: writing the CLEAN landmark twice yields the same bytes, and it
 * is only ever called when a repair was actually made. */
int chk_stamp_logfile_clean(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    UCHAR *rec = NULL, *page = NULL;
    CHK_RUN *runs;   /* 192 KiB: too large for the stack */
    ULONGLONG dataSize = 0;
    ULONG pageSize;
    int n, rc = -1;

    (void)res;

    rec = (UCHAR *)malloc(c->MftRecordSize);
    runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    if (!rec || !runs)
    {
        free(rec);
        free(runs);
        return -1;
    }

    /* Read + verify the $LogFile base record. */
    if (chk_read_record(c, FILE_LogFile, rec) != 0 ||
        *(ULONG *)rec != NRH_FILE_TYPE ||
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
    {
        CHK_TRACE("R8: $LogFile record unreadable; skipping clean-stamp\n");
        free(rec);
        free(runs);
        return -1;
    }

    /* Locate the unnamed non-resident $DATA runs (the log stream itself). */
    n = chk_find_data_runs(c, rec, runs, CHK_MAX_RUNS, &dataSize);
    if (n <= 0 || dataSize == 0)
    {
        CHK_TRACE("R8: $LogFile:$DATA not found/empty; skipping clean-stamp\n");
        free(rec);
        free(runs);
        return -1;
    }

    pageSize = chk_lfs_page_size(c);
    if ((ULONGLONG)pageSize * 2 > dataSize)
    {
        CHK_TRACE("R8: $LogFile too small for two restart pages; skipping\n");
        free(rec);
        free(runs);
        return -1;
    }

    page = (UCHAR *)malloc(pageSize);
    if (!page)
    {
        free(rec);
        free(runs);
        return -1;
    }

    /* Build one CLEAN RSTR page (USA already applied) and write it twice, at
     * stream offsets 0 and pageSize -- byte-identical, exactly like mkntfs. */
    ntfs_build_rstr_page(page, pageSize, c->BytesPerSector, dataSize);

    if (chk_stream_io(c->Io, runs, (ULONG)n, c->BytesPerCluster,
                      0, page, pageSize, 1) == 0 &&
        chk_stream_io(c->Io, runs, (ULONG)n, c->BytesPerCluster,
                      pageSize, page, pageSize, 1) == 0)
    {
        rc = 0;
        CHK_TRACE("R8: $LogFile stamped clean (2 x %lu-byte RSTR pages)\n",
                  pageSize);
    }

    free(page);
    free(rec);
    free(runs);
    return rc;
}

/* ============================================================
 * S4 shared attribute surgery
 * ============================================================ */

#define CHK_REF_REC(ref)  ((ULONG)((ref) & 0xFFFFFFFFFFFFULL))
#define CHK_REF_SEQ(ref)  ((USHORT)((ref) >> 48))

static void *
chk_grow_alloc(void *old, size_t oldSize, size_t newSize)
{
    void *newPtr;

    newPtr = malloc(newSize);
    if (!newPtr)
        return NULL;

    if (old && oldSize)
        memcpy(newPtr, old, oldSize < newSize ? oldSize : newSize);

    free(old);
    return newPtr;
}

/* Re-encode a (possibly truncated) runlist into mapping pairs starting at
 * dst.  Emits inter-run LCN deltas exactly like the on-disk format and a
 * terminating zero byte.  Returns bytes written including the terminator, or
 * -1 if it would exceed cap. */
static int chk_encode_runlist(UCHAR *dst, ULONG cap,
                              const CHK_RUN *runs, ULONG nruns)
{
    ULONG i;
    ULONG pos = 0;
    LONGLONG prevLcn = 0;

    for (i = 0; i < nruns; i++)
    {
        UCHAR tmp[24];
        int rl;
        if (runs[i].Sparse)
        {
            /* Sparse: length only, zero offset bytes (offBytes==0). */
            int lenBytes, k;
            ULONGLONG len = runs[i].Len;
            for (lenBytes = 1; lenBytes < 8; lenBytes++)
                if ((len >> (lenBytes * 8)) == 0)
                    break;
            if ((ULONG)pos + 1 + lenBytes >= cap)
                return -1;
            tmp[0] = (UCHAR)lenBytes;   /* offBytes nibble == 0 */
            for (k = 0; k < lenBytes; k++)
                tmp[1 + k] = (UCHAR)(len >> (k * 8));
            rl = 1 + lenBytes;
        }
        else
        {
            rl = ntfs_encode_run(tmp, runs[i].Len,
                                 (LONGLONG)runs[i].Lcn - prevLcn);
            prevLcn = (LONGLONG)runs[i].Lcn;
            if ((ULONG)pos + rl >= cap)
                return -1;
        }
        memcpy(dst + pos, tmp, rl);
        pos += rl;
    }
    if (pos + 1 > cap)
        return -1;
    dst[pos++] = 0;   /* terminator */
    return (int)pos;
}

/* Truncate a non-resident attribute's runlist so no run references a cluster
 * at/after `firstBadVcn` (a VCN boundary).  Rewrites the mapping pairs in
 * place, updates HighestVcn/AllocatedSize/DataSize/InitializedSize, and
 * re-terminates.  The attribute's Length is preserved (only the mapping-pair
 * region is rewritten, zero-padded to the old extent).  Returns 0 on success,
 * 1 if the whole attribute becomes empty (caller may delete), -1 on failure.
 *
 * `truncVcn` is expressed in the attribute's own VCN space (LowestVcn..). */
static int chk_truncate_attr_runs(CHK_CTX *c, UCHAR *rec, ULONG attrOff,
                                  ULONGLONG truncVcn)
{
    ATTR_RECORD *a = (ATTR_RECORD *)(rec + attrOff);
    CHK_RUN *runs;   /* 192 KiB: too large for the stack */
    const UCHAR *mp = rec + attrOff + a->NR.MappingPairsOffset;
    const UCHAR *end = rec + attrOff + a->Length;
    ULONG mpCap = a->Length - a->NR.MappingPairsOffset;
    ULONGLONG vcn = a->NR.LowestVcn;
    ULONGLONG keptClusters = 0;
    int n, i, keep, rl;

    if (!a->NonResident)
        return -1;

    runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    if (!runs)
        return -1;

    n = chk_decode_runs(mp, end, runs, CHK_MAX_RUNS);
    if (n < 0)
        { free(runs); return -1; }

    /* Walk runs, keeping those wholly below truncVcn; clip a straddling run. */
    keep = 0;
    for (i = 0; i < n; i++)
    {
        ULONGLONG runStart = vcn;
        ULONGLONG runEnd = vcn + runs[i].Len;   /* exclusive */
        if (runStart >= truncVcn)
            break;                 /* this and all later runs dropped */
        if (runEnd > truncVcn)
            runs[i].Len = truncVcn - runStart;   /* clip the straddler */
        keptClusters += runs[i].Len;
        keep = i + 1;
        vcn = runEnd;
    }

    if (keep == 0)
        { free(runs); return 1; }   /* nothing salvageable */

    /* Rewrite mapping pairs into the existing region (zero-pad the tail). */
    {
        UCHAR *dst = rec + attrOff + a->NR.MappingPairsOffset;
        rl = chk_encode_runlist(dst, mpCap, runs, (ULONG)keep);
        if (rl < 0)
            { free(runs); return -1; }
        if ((ULONG)rl < mpCap)
            memset(dst + rl, 0, mpCap - rl);
    }
    free(runs);

    /* Fix sizes.  Highest VCN in the attribute is (LowestVcn + kept - 1).
     * The streams S4 truncates are single-extent (LowestVcn==0); only then can
     * AllocatedSize/DataSize be recomputed from the kept cluster count. */
    a->NR.HighestVcn = a->NR.LowestVcn + keptClusters - 1;
    if (a->NR.LowestVcn == 0)
    {
        a->NR.AllocatedSize = (LONGLONG)keptClusters * c->BytesPerCluster;
        if (a->NR.DataSize > a->NR.AllocatedSize)
            a->NR.DataSize = a->NR.AllocatedSize;
        if (a->NR.InitializedSize > a->NR.DataSize)
            a->NR.InitializedSize = a->NR.DataSize;
    }
    return 0;
}

/* Delete an attribute record: memmove the tail down over it, fix BytesInUse.
 * Never call for $STANDARD_INFORMATION/$FILE_NAME or the AT_END sentinel. */
static void chk_delete_attr(CHK_CTX *c, UCHAR *rec, ULONG attrOff)
{
    FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
    ATTR_RECORD *a = (ATTR_RECORD *)(rec + attrOff);
    ULONG len = a->Length;
    ULONG tail;

    if (attrOff + len > c->MftRecordSize || len == 0)
        return;
    tail = h->BytesInUse - (attrOff + len);
    memmove(rec + attrOff, rec + attrOff + len, tail);
    memset(rec + attrOff + tail, 0, len);
    h->BytesInUse -= len;
}

/* ============================================================
 * R1: attribute truncation / deletion (OOB data runs)
 * ============================================================ */

/* For each record flagged corrupt by an OOB run (CHK_ERR_RUN_OOB) or an
 * attribute overrun, truncate the offending non-resident attribute at the
 * first cluster that references past the volume end.  System records 0-11 are
 * never touched (fixing them requires the mirror/format writer, out of scope).
 *
 * Only well-defined OOB-run truncation is attempted here; anything undecodable
 * on a non-critical attribute is deleted.  Cases that cannot be repaired
 * safely are left reported-but-unfixed. */
int chk_repair_attributes(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    UCHAR *rec;
    CHK_RUN *runs;   /* 192 KiB: too large for the stack */
    ULONG i;

    if (!c->Rec)
        return -1;
    rec = (UCHAR *)malloc(c->MftRecordSize);
    runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    if (!rec || !runs)
    {
        free(rec);
        free(runs);
        return -1;
    }

    for (i = 0; i < res->IssueCount && i < CHK_MAX_ISSUES; i++)
    {
        NTFS_CHK_ISSUE *iss = &res->Issues[i];
        ULONG recno = (ULONG)iss->Param0;
        ULONG off;
        int didFix = 0;

        if (iss->Fixed)
            continue;
        if (iss->Code != CHK_ERR_RUN_OOB)
            continue;
        if (recno < FILE_FIRST_USER || recno >= c->RecordCount)
            continue;   /* never touch system records */

        if (chk_read_record(c, recno, rec) != 0 ||
            *(ULONG *)rec != NRH_FILE_TYPE ||
            chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
            continue;

        /* Walk attributes; for each non-resident one, find the first VCN whose
         * cluster is out of bounds and truncate there. */
        {
            FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
            off = h->AttributeOffset;
            while (off + 8 <= c->MftRecordSize)
            {
                ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
                if (a->Type == AT_END || a->Length == 0 ||
                    off + a->Length > c->MftRecordSize)
                    break;
                if (a->NonResident &&
                    a->Type != AT_STANDARD_INFORMATION &&
                    a->Type != AT_FILE_NAME)
                {
                    const UCHAR *mp = rec + off + a->NR.MappingPairsOffset;
                    const UCHAR *end = rec + off + a->Length;
                    int n = chk_decode_runs(mp, end, runs, CHK_MAX_RUNS);
                    if (n > 0)
                    {
                        ULONGLONG vcn = a->NR.LowestVcn;
                        ULONGLONG badVcn = (ULONGLONG)-1;
                        int r;
                        for (r = 0; r < n; r++)
                        {
                            if (!runs[r].Sparse)
                            {
                                ULONGLONG cl;
                                for (cl = 0; cl < runs[r].Len; cl++)
                                    if (runs[r].Lcn + cl >= c->TotalClusters)
                                    {
                                        badVcn = vcn + cl;
                                        break;
                                    }
                            }
                            if (badVcn != (ULONGLONG)-1)
                                break;
                            vcn += runs[r].Len;
                        }
                        if (badVcn != (ULONGLONG)-1)
                        {
                            int tr = chk_truncate_attr_runs(c, rec, off, badVcn);
                            if (tr == 0)
                                didFix = 1;
                            else if (tr == 1 &&
                                     a->Type != AT_STANDARD_INFORMATION &&
                                     a->Type != AT_FILE_NAME)
                            {
                                chk_delete_attr(c, rec, off);
                                didFix = 1;
                                continue;   /* attr layout shifted: rescan */
                            }
                        }
                    }
                }
                off += a->Length;
            }
        }

        if (didFix)
        {
            chk_stamp_fixups(rec, c->MftRecordSize, c->BytesPerSector);
            if (chk_write_record(c, recno, rec) == 0)
            {
                if (recno < FILE_FIRST_USER)
                    chk_sync_mftmirr_record(c, recno);
                iss->Fixed = 1;
                res->RepairedCount++;
                res->AttrsTruncated++;
                chk_add_issue(res, CHK_ERR_ATTR_TRUNCATED, recno, 0, 1);
                CHK_TRACE("R1: truncated OOB attr in rec %lu\n",
                          (unsigned long)recno);
            }
        }
    }

    free(runs);
    free(rec);
    return 0;
}

/* ============================================================
 * R1: cross-link truncation
 * ============================================================ */

/* Does any record with a number strictly lower than `notThis` claim `lcn` in
 * one of its non-resident, non-$SI/$FN attributes?  Used to confirm there is a
 * true "winner" (the lowest-numbered claimant keeps the cluster) before we
 * truncate the higher-numbered loser.  Guarantees system files 0-11 (and any
 * lower record) always win. */
static int chk_lcn_claimed_below(CHK_CTX *c, ULONGLONG lcn, ULONG notThis)
{
    UCHAR *rec = (UCHAR *)malloc(c->MftRecordSize);
    CHK_RUN *runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    ULONG recno;
    int found = 0;

    if (!rec || !runs)
    {
        free(rec);
        free(runs);
        return 0;
    }
    for (recno = 0; recno < notThis && recno < c->RecordCount && !found; recno++)
    {
        FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
        ULONG off;
        if (chk_read_record(c, recno, rec) != 0 ||
            *(ULONG *)rec != NRH_FILE_TYPE ||
            chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
            continue;
        if (!(h->Flags & FRH_IN_USE))
            continue;
        off = h->AttributeOffset;
        while (off + 8 <= c->MftRecordSize)
        {
            ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
            if (a->Type == AT_END || a->Length == 0 ||
                off + a->Length > c->MftRecordSize)
                break;
            if (a->NonResident)
            {
                int n = chk_decode_runs(rec + off + a->NR.MappingPairsOffset,
                                        rec + off + a->Length, runs,
                                        CHK_MAX_RUNS);
                int r;
                for (r = 0; r < n; r++)
                    if (!runs[r].Sparse &&
                        lcn >= runs[r].Lcn && lcn < runs[r].Lcn + runs[r].Len)
                        { found = 1; break; }
            }
            if (found)
                break;
            off += a->Length;
        }
    }
    free(runs);
    free(rec);
    return found;
}

/* Truncate the loser of each cross-linked cluster at the first VCN that maps
 * to the shared LCN.  Policy: the lowest-numbered claimant keeps the cluster;
 * detection recorded the later claimant as SecondRec, which is the loser -- but
 * only truncate it after confirming a strictly-lower-numbered record also
 * claims the LCN (so the true owner survives and system files 0-11 always win).
 * If the ledger overflowed (XLinkCount > CHK_MAX_XLINKS) the excess entries are
 * simply not in the array and stay reported. */
int chk_repair_crosslinks(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    UCHAR *rec;
    CHK_RUN *runs;   /* 192 KiB: too large for the stack */
    ULONG li;
    ULONG ledgerCount = CHK_MIN(c->XLinkCount, (ULONG)CHK_MAX_XLINKS);

    rec = (UCHAR *)malloc(c->MftRecordSize);
    runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    if (!rec || !runs)
    {
        free(rec);
        free(runs);
        return -1;
    }

    for (li = 0; li < ledgerCount; li++)
    {
        ULONGLONG lcn = c->XLinks[li].Lcn;
        ULONG recno = c->XLinks[li].SecondRec;
        ULONG off;
        int didFix = 0;

        if (recno >= c->RecordCount)
            continue;
        /* Confirm a lower-numbered owner exists; if not, we cannot safely
         * pick a loser -- leave the xlink reported. */
        if (!chk_lcn_claimed_below(c, lcn, recno))
            continue;

        if (chk_read_record(c, recno, rec) != 0 ||
            *(ULONG *)rec != NRH_FILE_TYPE ||
            chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
            continue;

        {
            FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
            off = h->AttributeOffset;
            while (off + 8 <= c->MftRecordSize && !didFix)
            {
                ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
                if (a->Type == AT_END || a->Length == 0 ||
                    off + a->Length > c->MftRecordSize)
                    break;
                if (a->NonResident &&
                    a->Type != AT_STANDARD_INFORMATION &&
                    a->Type != AT_FILE_NAME)
                {
                    const UCHAR *mp = rec + off + a->NR.MappingPairsOffset;
                    const UCHAR *end = rec + off + a->Length;
                    int n = chk_decode_runs(mp, end, runs, CHK_MAX_RUNS);
                    if (n > 0)
                    {
                        ULONGLONG vcn = a->NR.LowestVcn;
                        ULONGLONG hitVcn = (ULONGLONG)-1;
                        int r;
                        for (r = 0; r < n; r++)
                        {
                            if (!runs[r].Sparse &&
                                lcn >= runs[r].Lcn &&
                                lcn < runs[r].Lcn + runs[r].Len)
                            {
                                hitVcn = vcn + (lcn - runs[r].Lcn);
                                break;
                            }
                            vcn += runs[r].Len;
                        }
                        if (hitVcn != (ULONGLONG)-1)
                        {
                            int tr = chk_truncate_attr_runs(c, rec, off, hitVcn);
                            if (tr == 0)
                                didFix = 1;
                            else if (tr == 1)
                            {
                                chk_delete_attr(c, rec, off);
                                didFix = 1;
                            }
                        }
                    }
                }
                off += a->Length;
            }
        }

        if (didFix)
        {
            chk_stamp_fixups(rec, c->MftRecordSize, c->BytesPerSector);
            if (chk_write_record(c, recno, rec) == 0)
            {
                ULONG k;
                res->AttrsTruncated++;
                /* mark every recorded XLINK issue for this SecondRec fixed */
                for (k = 0; k < res->IssueCount && k < CHK_MAX_ISSUES; k++)
                    if (res->Issues[k].Code == CHK_ERR_XLINK &&
                        !res->Issues[k].Fixed &&
                        (ULONG)res->Issues[k].Param0 == recno)
                    {
                        res->Issues[k].Fixed = 1;
                        res->RepairedCount++;
                    }
                CHK_TRACE("R1: truncated crosslink lcn %llu in rec %lu\n",
                          (unsigned long long)lcn, (unsigned long)recno);
            }
        }
    }

    free(runs);
    free(rec);
    return 0;
}

/* ============================================================
 * R4: link-count fix
 * ============================================================ */

int chk_repair_linkcounts(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    UCHAR *rec;
    ULONG recno;

    if (!c->Rec)
        return -1;
    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return -1;

    for (recno = 0; recno < c->RecordCount; recno++)
    {
        CHK_REC *m = &c->Rec[recno];
        USHORT f = m->Flags;
        ULONG parent, gate;

        if ((f & (CRF_IN_USE | CRF_EXTENSION | CRF_CORRUPT)) != CRF_IN_USE)
            continue;
        if (f & CRF_ORPHAN)
            continue;   /* leave for S6 orphan recovery */
        if (m->LinkCountHdr == m->LinksSeen)
            continue;

        /* LinksSeen is only trustworthy when the referencing directory was
         * fully walked.  Root references itself; everyone else via parent. */
        parent = CHK_REF_REC(m->ParentRef);
        gate = (recno == FILE_Root) ? FILE_Root : parent;
        if (!chk_parent_trusted(c, gate))
            continue;

        if (chk_read_record(c, recno, rec) != 0 ||
            *(ULONG *)rec != NRH_FILE_TYPE ||
            chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
            continue;

        ((FILE_RECORD_HEADER *)rec)->LinkCount = m->LinksSeen;
        chk_stamp_fixups(rec, c->MftRecordSize, c->BytesPerSector);
        if (chk_write_record(c, recno, rec) != 0)
            continue;
        if (recno < FILE_FIRST_USER)
            chk_sync_mftmirr_record(c, recno);

        m->LinkCountHdr = m->LinksSeen;
        res->LinksFixed++;

        {
            ULONG k;
            for (k = 0; k < res->IssueCount && k < CHK_MAX_ISSUES; k++)
                if (res->Issues[k].Code == CHK_ERR_LINKCOUNT &&
                    !res->Issues[k].Fixed &&
                    (ULONG)res->Issues[k].Param0 == recno)
                {
                    res->Issues[k].Fixed = 1;
                    res->RepairedCount++;
                }
        }
        CHK_TRACE("R4: fixed link count of rec %lu -> %u\n",
                  (unsigned long)recno, (unsigned)m->LinksSeen);
    }

    free(rec);
    return 0;
}

/* ============================================================
 * R5: $MFT:$BITMAP rewrite from the computed MftMap
 * ============================================================ */

int chk_repair_mft_bitmap(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    UCHAR *rec;
    ULONG off;
    ULONGLONG onDiskSize = 0;
    ULONGLONG writeBytes;
    int anyMismatch = 0;
    ULONG k;

    if (!c->MftMap)
        return -1;

    /* Only run when a mismatch was actually reported. */
    for (k = 0; k < res->IssueCount && k < CHK_MAX_ISSUES; k++)
        if (res->Issues[k].Code == CHK_ERR_MFTBMP_MISMATCH &&
            !res->Issues[k].Fixed)
            { anyMismatch = 1; break; }
    if (!anyMismatch)
        return 0;

    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return -1;
    if (chk_read_record(c, FILE_MFT, rec) != 0 ||
        *(ULONG *)rec != NRH_FILE_TYPE ||
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0 ||
        (off = chk_find_attr(c, rec, AT_BITMAP, NULL, 0)) == 0)
        { free(rec); return -1; }

    /* Build the corrected bitmap from MftMap (bits >= RecordCount forced 0). */
    {
        ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
        ULONGLONG modelBytes = (ULONGLONG)(c->RecordCount + 7) / 8;

        if (!a->NonResident)
        {
            /* Resident: overwrite the in-record value up to its length. */
            ULONGLONG vlen = a->Resident.ValueLength;
            UCHAR *dst = rec + off + a->Resident.ValueOffset;
            ULONGLONG j;
            onDiskSize = vlen;
            writeBytes = CHK_MIN(vlen, modelBytes);
            memcpy(dst, c->MftMap, (size_t)writeBytes);
            /* force trailing bits (>= RecordCount) to 0 within the model span */
            for (j = writeBytes; j < vlen; j++)
                dst[j] = 0;
            chk_stamp_fixups(rec, c->MftRecordSize, c->BytesPerSector);
            if (chk_write_record(c, FILE_MFT, rec) != 0)
                { free(rec); return -1; }
        }
        else
        {
            /* Non-resident: write MftMap over $MFT:$BITMAP's runlist, but
             * never beyond the on-disk DataSize (do not grow/shrink attr). */
            CHK_RUN *runs;   /* 192 KiB: too large for the stack */
            const UCHAR *mp = rec + off + a->NR.MappingPairsOffset;
            const UCHAR *end = rec + off + a->Length;
            int n;
            UCHAR *buf;
            onDiskSize = (ULONGLONG)a->NR.DataSize;
            runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
            if (!runs)
                { free(rec); return -1; }
            n = chk_decode_runs(mp, end, runs, CHK_MAX_RUNS);
            if (n <= 0)
                { free(runs); free(rec); return -1; }
            writeBytes = CHK_MIN(onDiskSize, modelBytes);
            buf = (UCHAR *)calloc(1, (size_t)onDiskSize + 8);
            if (!buf)
                { free(runs); free(rec); return -1; }
            memcpy(buf, c->MftMap, (size_t)writeBytes);
            /* bytes past the model span stay 0 (records >= RecordCount) */
            if (chk_stream_io(c->Io, runs, (ULONG)n, c->BytesPerCluster,
                              0, buf, onDiskSize, 1) != 0)
                { free(buf); free(runs); free(rec); return -1; }
            free(buf);
            free(runs);
        }
    }
    free(rec);

    /* Mark all MFT-bitmap mismatch issues fixed. */
    for (k = 0; k < res->IssueCount && k < CHK_MAX_ISSUES; k++)
        if (res->Issues[k].Code == CHK_ERR_MFTBMP_MISMATCH &&
            !res->Issues[k].Fixed)
        {
            res->Issues[k].Fixed = 1;
            res->RepairedCount++;
        }
    CHK_TRACE("R5: rewrote $MFT:$BITMAP (%llu bytes)\n",
              (unsigned long long)onDiskSize);
    return 0;
}

/* ============================================================
 * S5 (R2): from-scratch bottom-up $I30 directory-index rebuild
 *
 * For a directory whose $I30 was flagged CRF_INDEX_BAD, discard the old
 * (damaged) INDEX_ROOT/INDEX_ALLOCATION/$BITMAP($I30) and reconstruct the whole
 * index from the children: every in-use base record owning a $FILE_NAME whose
 * ParentDirectory == dirRec contributes one entry per $FILE_NAME (Win32 and DOS
 * both).  The duplicated size/attribute info in each key is refreshed (H6) from
 * the target's live $DATA / $STANDARD_INFORMATION so the result matches what a
 * real chkdsk would leave behind.  Entries are sorted by COLLATION_FILENAME and
 * packed either resident (small) or into INDX blocks with a promoted parent
 * level (large).  Clusters are allocated first-fit from c->ClusterMap after the
 * old $I30 clusters are freed there; R6 persists c->ClusterMap to $Bitmap.
 * ============================================================ */

static const WCHAR chk_i30_name[4] = { '$', 'I', '3', '0' };

/* Offset of FILE_NAME_ATTR::FileName[0] -- the fixed part before the name.
 * A $FILE_NAME key with N name WCHARs is CHK_FN_FIXED + N*sizeof(WCHAR) bytes
 * (matches ntfs_make_index_entry's sizeof(FILE_NAME_ATTR)-sizeof(WCHAR)+N*2). */
#define CHK_FN_FIXED  0x42

/* One collected leaf entry: a self-contained INDEX_ENTRY (0x10 header + key). */
typedef struct _CHK_RB_ENT {
    UCHAR    *blob;     /* malloc'd: INDEX_ENTRY header + FILE_NAME key, 8-aligned */
    ULONG     len;      /* blob length (aligned)                                   */
    ULONG     keyLen;   /* FILE_NAME key length                                    */
    ULONG     targetRec;/* IndexedFile record number (for stable dup tie-break)    */
} CHK_RB_ENT;

/* Return the target's unnamed non-resident-or-resident $DATA size + alloc, and
 * $STANDARD_INFORMATION FileAttributes.  Any field left untouched if absent. */
static void chk_rb_refresh_info(CHK_CTX *c, UCHAR *tgt, int isDir,
                                ULONGLONG *dataSize, ULONGLONG *allocSize,
                                ULONG *fileAttrs)
{
    FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)tgt;
    ULONG off = h->AttributeOffset;

    *dataSize = 0;
    *allocSize = 0;
    *fileAttrs = 0;

    while (off + 8 <= c->MftRecordSize)
    {
        ATTR_RECORD *a = (ATTR_RECORD *)(tgt + off);
        if (a->Type == AT_END || a->Length == 0 ||
            off + a->Length > c->MftRecordSize)
            break;

        if (a->Type == AT_STANDARD_INFORMATION && !a->NonResident &&
            a->Resident.ValueOffset + sizeof(STANDARD_INFORMATION) <= a->Length)
        {
            STANDARD_INFORMATION *si =
                (STANDARD_INFORMATION *)(tgt + off + a->Resident.ValueOffset);
            *fileAttrs = si->FileAttributes;
        }
        /* unnamed $DATA carries the file's real sizes (dirs have none) */
        if (a->Type == AT_DATA && a->NameLength == 0 && !isDir)
        {
            if (a->NonResident)
            {
                *dataSize = (ULONGLONG)a->NR.DataSize;
                *allocSize = (ULONGLONG)a->NR.AllocatedSize;
            }
            else
            {
                ULONGLONG vl = a->Resident.ValueLength;
                *dataSize = vl;
                *allocSize = NTFS_ALIGN_UP(vl, 8);
            }
        }
        off += a->Length;
    }

    /* On-disk $FILE_NAME FileAttributes mirror $SI but always carry the
     * directory index-present bit for directories. */
    if (isDir)
        *fileAttrs |= FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT;
    else
        *fileAttrs &= ~(ULONG)FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT;
}

/* Build one leaf entry blob from a target's $FILE_NAME (already validated to
 * fit).  Refreshes the duplicated info (H6).  Returns malloc'd blob or NULL. */
static UCHAR *chk_rb_make_entry(CHK_CTX *c, ULONG targetRec, USHORT targetSeq,
                                const FILE_NAME_ATTR *srcFn, int isDir,
                                ULONG *outLen, ULONG *outKeyLen)
{
    ULONG keyLen = CHK_FN_FIXED +
                   (ULONG)srcFn->FileNameLength * sizeof(WCHAR);
    ULONG entryLen = (ULONG)NTFS_ALIGN_UP(sizeof(INDEX_ENTRY) + keyLen, 8);
    UCHAR *blob = (UCHAR *)calloc(1, entryLen);
    INDEX_ENTRY *e;
    FILE_NAME_ATTR *dst;
    ULONGLONG dataSize, allocSize;
    ULONG fileAttrs;
    UCHAR *tgt;

    if (!blob)
        return NULL;

    e = (INDEX_ENTRY *)blob;
    e->Data.Directory.IndexedFile = ntfs_mft_ref(targetRec, targetSeq);
    e->Length = (USHORT)entryLen;
    e->KeyLength = (USHORT)keyLen;
    e->Flags = 0;

    dst = (FILE_NAME_ATTR *)(blob + sizeof(INDEX_ENTRY));
    memcpy(dst, srcFn, keyLen);

    /* H6: refresh the duplicated size/attribute info from the live target. */
    tgt = (UCHAR *)malloc(c->MftRecordSize);
    if (tgt && chk_read_record(c, targetRec, tgt) == 0 &&
        *(ULONG *)tgt == NRH_FILE_TYPE &&
        chk_apply_fixups(tgt, c->MftRecordSize, c->BytesPerSector) == 0)
    {
        chk_rb_refresh_info(c, tgt, isDir, &dataSize, &allocSize, &fileAttrs);
        dst->DataSize = dataSize;
        dst->AllocatedSize = allocSize;
        dst->FileAttributes = fileAttrs;
    }
    free(tgt);

    *outLen = entryLen;
    *outKeyLen = keyLen;
    return blob;
}

/* Comparator: COLLATION_FILENAME on the two entries' keys, dup tie-break on
 * targetRec (lowest wins) so the survivor of an exact-name duplicate is stable. */
static int chk_rb_cmp(const CHK_CTX *c, const CHK_RB_ENT *a, const CHK_RB_ENT *b)
{
    const FILE_NAME_ATTR *ka = (const FILE_NAME_ATTR *)(a->blob + sizeof(INDEX_ENTRY));
    const FILE_NAME_ATTR *kb = (const FILE_NAME_ATTR *)(b->blob + sizeof(INDEX_ENTRY));
    int r = chk_collate_filename(c, ka->FileName, ka->FileNameLength,
                                 kb->FileName, kb->FileNameLength);
    if (r != 0)
        return r;
    if (a->targetRec != b->targetRec)
        return a->targetRec < b->targetRec ? -1 : 1;
    return 0;
}

/* Insertion sort (bounded by one directory's child count; stable-ish and simple). */
static void chk_rb_sort(CHK_CTX *c, CHK_RB_ENT *ents, ULONG n)
{
    ULONG i, j;
    for (i = 1; i < n; i++)
    {
        CHK_RB_ENT key = ents[i];
        j = i;
        while (j > 0 && chk_rb_cmp(c, &ents[j - 1], &key) > 0)
        {
            ents[j] = ents[j - 1];
            j--;
        }
        ents[j] = key;
    }
}

/* Allocate `count` contiguous free clusters first-fit from c->ClusterMap; marks
 * them used in the map and returns the start LCN, or 0 on failure (cluster 0 is
 * never handed out, so 0 is an unambiguous error). */
static ULONGLONG chk_rb_alloc_clusters(CHK_CTX *c, ULONGLONG count)
{
    ULONGLONG lcn, run = 0, start = 0;

    if (!c->ClusterMap || count == 0)
        return 0;
    for (lcn = 1; lcn < c->TotalClusters; lcn++)
    {
        if (!chk_bmp_get(c->ClusterMap, lcn))
        {
            if (run == 0)
                start = lcn;
            if (++run == count)
            {
                ULONGLONG k;
                for (k = 0; k < count; k++)
                    chk_bmp_set(c->ClusterMap, start + k);
                return start;
            }
        }
        else
        {
            run = 0;
        }
    }
    return 0;
}

/* Free every non-sparse cluster of the directory's current $INDEX_ALLOCATION in
 * c->ClusterMap so they can be reused (R6 then clears the on-disk $Bitmap). */
static void chk_rb_free_old_ia(CHK_CTX *c, UCHAR *rec)
{
    ULONG iaOff = chk_find_attr(c, rec, AT_INDEX_ALLOCATION, chk_i30_name, 4);
    ATTR_RECORD *a;
    CHK_RUN *runs;   /* 192 KiB: too large for the stack */
    int n, i;

    if (!iaOff || !c->ClusterMap)
        return;
    a = (ATTR_RECORD *)(rec + iaOff);
    if (!a->NonResident)
        return;
    runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    if (!runs)
        return;
    n = chk_decode_runs(rec + iaOff + a->NR.MappingPairsOffset,
                        rec + iaOff + a->Length, runs, CHK_MAX_RUNS);
    for (i = 0; i < n; i++)
    {
        ULONGLONG cl;
        if (runs[i].Sparse)
            continue;
        for (cl = 0; cl < runs[i].Len; cl++)
        {
            ULONGLONG lcn = runs[i].Lcn + cl;
            if (lcn < c->TotalClusters)
                chk_bmp_clear(c->ClusterMap, lcn);
        }
    }
    free(runs);
}

/* Remove the three old $I30 attributes from a record (INDEX_ROOT,
 * INDEX_ALLOCATION, $BITMAP:$I30).  Repeatedly rescans since chk_delete_attr
 * shifts the layout. */
static void chk_rb_strip_old_attrs(CHK_CTX *c, UCHAR *rec)
{
    ULONG off;
    int again = 1;
    while (again)
    {
        again = 0;
        off = chk_find_attr(c, rec, AT_INDEX_ROOT, chk_i30_name, 4);
        if (!off) off = chk_find_attr(c, rec, AT_INDEX_ALLOCATION, chk_i30_name, 4);
        if (!off) off = chk_find_attr(c, rec, AT_BITMAP, chk_i30_name, 4);
        if (off)
        {
            chk_delete_attr(c, rec, off);
            again = 1;
        }
    }
}

/* Space left in the base record for new resident attributes (after the current
 * BytesInUse, keeping room for the 4-byte AT_END terminator that stays). */
static ULONG chk_rb_record_free(CHK_CTX *c, UCHAR *rec)
{
    FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
    ULONG inUse = h->BytesInUse;      /* includes the AT_END sentinel already */
    if (inUse + 8 >= c->MftRecordSize)
        return 0;
    return c->MftRecordSize - inUse;
}

/* Emit a resident (small/leaf) INDEX_ROOT holding all `n` entries + END. */
static int chk_rb_emit_small(CHK_CTX *c, UCHAR *rec, CHK_RB_ENT *ents, ULONG n,
                             ULONG blockSize, ULONG clustersPerBlock,
                             USHORT *nextInstance)
{
    ULONG entriesBytes = 0, i;
    ULONG hdrEntriesOff = sizeof(INDEX_HEADER);
    ULONG valLen;
    UCHAR *val, *ep;
    INDEX_ROOT *ir;
    INDEX_ENTRY *end;

    for (i = 0; i < n; i++)
        entriesBytes += ents[i].len;
    entriesBytes += sizeof(INDEX_ENTRY);   /* END entry */

    valLen = sizeof(INDEX_ROOT) + entriesBytes;
    val = (UCHAR *)calloc(1, valLen);
    if (!val)
        return -1;

    ir = (INDEX_ROOT *)val;
    ir->AttributeType = AT_FILE_NAME;
    ir->CollationRule = 1;   /* COLLATION_FILE_NAME */
    ir->IndexBlockSize = blockSize;
    ir->ClustersPerIndexBlock = (UCHAR)clustersPerBlock;
    ir->Header.EntriesOffset = hdrEntriesOff;
    ir->Header.IndexLength = hdrEntriesOff + entriesBytes;
    ir->Header.AllocatedSize = hdrEntriesOff + entriesBytes;
    ir->Header.Flags = 0;    /* leaf root: no sub-nodes */

    ep = val + sizeof(INDEX_ROOT);
    for (i = 0; i < n; i++)
    {
        memcpy(ep, ents[i].blob, ents[i].len);
        ep += ents[i].len;
    }
    end = (INDEX_ENTRY *)ep;
    end->Length = sizeof(INDEX_ENTRY);
    end->Flags = INDEX_ENTRY_END;

    ntfs_add_resident_attr(nextInstance, (FILE_RECORD_HEADER *)rec,
                           AT_INDEX_ROOT, chk_i30_name, 4, val, valLen, 0);
    free(val);
    return 0;
}

/* ============================================================
 * Large-index emitter: true multi-level bottom-up bulk load.
 *
 * A "level" is an ordered array of RBENT.  Each RBENT carries the leaf-form
 * INDEX_ENTRY blob (0x10 header + FILE_NAME key, 8-aligned, Flags cleared) and,
 * for interior levels, a child VCN pointing at the subtree of keys strictly
 * less than this entry's key.  Level 0 is the leaves (isInterior==0, no child).
 *
 * Packing one level into INDX blocks fills each block greedily; the entry that
 * would overflow is promoted to the parent (its key becomes a separator whose
 * child VCN points at the block just closed).  The block's own END entry is a
 * NODE carrying that separator's original down-pointer (its rightmost child);
 * the last block's rightmost child is the level's incoming `rightmostVcn`.  The
 * last block's VCN becomes the parent level's incoming rightmostVcn.  Recurse
 * until a level has few enough entries to fit INDEX_ROOT.
 * ============================================================ */

typedef struct _CHK_RB_LEVEL_ENT {
    UCHAR    *key;      /* leaf-form INDEX_ENTRY blob (aligned8, Flags=0)      */
    ULONG     keyLen;   /* its length                                          */
    ULONGLONG childVcn; /* interior: subtree of keys < this key's key          */
} RBENT;

/* Interior-form length of one entry (adds the 8-byte trailing child VCN). */
static __inline ULONG chk_rb_interior_len(ULONG keyLen)
{
    return (ULONG)NTFS_ALIGN_UP(keyLen + 8, 8);
}

/* Write one entry into `dst` in the requested form; returns bytes written. */
static ULONG chk_rb_write_entry(UCHAR *dst, const RBENT *e, int asInterior)
{
    if (!asInterior)
    {
        memcpy(dst, e->key, e->keyLen);
        ((INDEX_ENTRY *)dst)->Flags &= (USHORT)~INDEX_ENTRY_NODE;
        ((INDEX_ENTRY *)dst)->Length = (USHORT)e->keyLen;
        return e->keyLen;
    }
    else
    {
        ULONG il = chk_rb_interior_len(e->keyLen);
        memcpy(dst, e->key, e->keyLen);
        memset(dst + e->keyLen, 0, il - e->keyLen);
        ((INDEX_ENTRY *)dst)->Flags |= INDEX_ENTRY_NODE;
        ((INDEX_ENTRY *)dst)->Length = (USHORT)il;
        *(ULONGLONG *)(dst + il - 8) = e->childVcn;
        return il;
    }
}

/* One serialized INDX block awaiting write. */
typedef struct _CHK_RB_BLK {
    UCHAR    *body;     /* entries + END (no INDEX_HEADER)                     */
    ULONG     bodyLen;
    UCHAR     interior; /* Header.Flags for this block (0 leaf, 1 interior)    */
    ULONGLONG vcn;
} CHK_RB_BLK;

/* Emit the large layout: build INDX levels bottom-up, then the record attrs. */
static int chk_rb_emit_large(CHK_CTX *c, UCHAR *rec, CHK_RB_ENT *ents, ULONG n,
                             ULONG blockSize, ULONG clustersPerBlock,
                             USHORT *nextInstance, NTFS_CHK_RESULT *res)
{
    /* H5: VCN units per INDX block, from the ONE shared authority also used by
     * the chkscan.c walker.  clusterSize<=blockSize -> clusters (>=1); a bigger
     * cluster -> 512-byte units.  The 4K-cluster/4K-block test image gives 1;
     * the >block case is covered by the helper's own math, asserted below (no
     * image variant produces it, so it is unit-checked rather than run here). */
    ULONG vcnPerBlock = chk_vcn_per_block(c->BytesPerCluster, blockSize);
    ULONG hdrOff = (ULONG)NTFS_ALIGN_UP(sizeof(INDEX_BUFFER) +
                        (1 + blockSize / c->BytesPerSector) * sizeof(USHORT), 8);
    ULONG usableBody = blockSize - hdrOff;   /* bytes for entries+END per INDX */

    RBENT *level = NULL;                     /* current level being packed     */
    ULONG levelN = 0;
    CHK_RB_BLK *blocks = NULL;               /* all serialized INDX blocks     */
    ULONG blockCount = 0, blockCap = 0;
    ULONG nextBlockVcnUnit = 0;              /* running VCN (in vcn units)      */
    ULONGLONG rightmost = 0;                 /* rightmost child of current lvl  */
    int haveRightmost = 0;                   /* leaf level has no incoming rm   */
    UCHAR *rootBody = NULL;                  /* INDEX_ROOT entry list (no hdr)  */
    ULONG rootLen = 0;
    int rootInterior = 0;
    ULONGLONG iaLcn = 0, iaClusters = 0;
    UCHAR *iaBuf = NULL, *bmp = NULL;
    ULONG bmpBytes;
    int rc = -1;
    ULONG i;

    /* Seed level 0 from the collected leaf entries. */
    level = (RBENT *)calloc(n ? n : 1, sizeof(RBENT));
    if (!level)
        goto done;
    for (i = 0; i < n; i++)
    {
        level[i].key = ents[i].blob;   /* borrowed; freed by the caller       */
        level[i].keyLen = ents[i].len;
        level[i].childVcn = 0;
    }
    levelN = n;

    /* Bulk-load levels bottom-up until one fits INDEX_ROOT. */
    for (;;)
    {
        RBENT *parent = NULL;
        ULONG parentN = 0, parentCap = 0;
        int isInterior = haveRightmost;   /* level 0 is leaves; deeper interior */
        ULONG endLen = isInterior
            ? (ULONG)NTFS_ALIGN_UP(sizeof(INDEX_ENTRY) + 8, 8)
            : sizeof(INDEX_ENTRY);

        /* Does this whole level fit a single INDEX_ROOT node?  If so it is the
         * root and we stop (the root's END is a NODE at the level's rightmost). */
        {
            ULONG total = 0;
            for (i = 0; i < levelN; i++)
                total += chk_rb_interior_len(level[i].keyLen);
            total += (ULONG)NTFS_ALIGN_UP(sizeof(INDEX_ENTRY) + 8, 8); /* END NODE */
            if (sizeof(INDEX_ROOT) + total + 0x18 + 8 <= chk_rb_record_free(c, rec) &&
                total <= usableBody /* sanity: also representable as a node */)
            {
                /* Serialize the root body: all entries (interior form) + END NODE. */
                UCHAR *p;
                rootBody = (UCHAR *)calloc(1, total);
                if (!rootBody)
                    goto done;
                p = rootBody;
                for (i = 0; i < levelN; i++)
                    p += chk_rb_write_entry(p, &level[i], 1);
                {
                    INDEX_ENTRY *end = (INDEX_ENTRY *)p;
                    ULONG el = (ULONG)NTFS_ALIGN_UP(sizeof(INDEX_ENTRY) + 8, 8);
                    end->Length = (USHORT)el;
                    end->Flags = INDEX_ENTRY_END | INDEX_ENTRY_NODE;
                    *(ULONGLONG *)((UCHAR *)end + el - 8) =
                        haveRightmost ? rightmost : 0;
                    p += el;
                }
                rootLen = (ULONG)(p - rootBody);
                rootInterior = 1;
                free(level);
                level = NULL;
                break;
            }
        }

        /* Pack this level into INDX blocks; collect promoted separators. */
        i = 0;
        while (i < levelN)
        {
            ULONG start = i, bodyLen = 0;
            ULONGLONG blkVcn;
            UCHAR *body, *p;
            ULONG sepIdx;
            int haveSep;

            while (i < levelN)
            {
                ULONG el = isInterior ? chk_rb_interior_len(level[i].keyLen)
                                      : level[i].keyLen;
                if (bodyLen + el + endLen > usableBody)
                    break;
                bodyLen += el;
                i++;
            }
            if (i == start)          /* one entry can't fit even alone */
                goto done_level;

            blkVcn = (ULONGLONG)nextBlockVcnUnit;
            nextBlockVcnUnit += vcnPerBlock;

            haveSep = (i < levelN);
            sepIdx = i;              /* separator is level[i] if haveSep */

            body = (UCHAR *)calloc(1, bodyLen + endLen);
            if (!body)
                goto done_level;
            p = body;
            {
                ULONG k;
                for (k = start; k < i; k++)
                    p += chk_rb_write_entry(p, &level[k], isInterior);
            }
            {
                INDEX_ENTRY *end = (INDEX_ENTRY *)p;
                end->Length = (USHORT)endLen;
                if (isInterior)
                {
                    end->Flags = INDEX_ENTRY_END | INDEX_ENTRY_NODE;
                    /* rightmost child of this block = the separator's own
                     * down-pointer, or (last block) the incoming rightmost. */
                    *(ULONGLONG *)((UCHAR *)end + endLen - 8) =
                        haveSep ? level[sepIdx].childVcn : rightmost;
                }
                else
                {
                    end->Flags = INDEX_ENTRY_END;
                }
                p += endLen;
            }

            if (blockCount == blockCap)
            {
                ULONG nc = blockCap ? blockCap * 2 : 16;
                CHK_RB_BLK *nb = (CHK_RB_BLK *)chk_grow_alloc(blocks,
                    (size_t)blockCap * sizeof(*nb), (size_t)nc * sizeof(*nb));
                if (!nb) { free(body); blocks = NULL; goto done_level; }
                blocks = nb; blockCap = nc;
            }
            blocks[blockCount].body = body;
            blocks[blockCount].bodyLen = (ULONG)(p - body);
            blocks[blockCount].interior = (UCHAR)isInterior;
            blocks[blockCount].vcn = blkVcn;
            blockCount++;

            if (haveSep)
            {
                /* Promote level[sepIdx] as a separator whose child = this block. */
                if (parentN == parentCap)
                {
                    ULONG nc = parentCap ? parentCap * 2 : 32;
                    RBENT *np = (RBENT *)chk_grow_alloc(parent,
                        (size_t)parentCap * sizeof(*np), (size_t)nc * sizeof(*np));
                    if (!np) { parent = NULL; goto done_level; }
                    parent = np; parentCap = nc;
                }
                parent[parentN].key = level[sepIdx].key;
                parent[parentN].keyLen = level[sepIdx].keyLen;
                parent[parentN].childVcn = blkVcn;
                parentN++;
                i++;   /* consume the separator */
            }
            else
            {
                /* Last block of this level: its VCN is the parent's rightmost. */
                rightmost = blkVcn;
                haveRightmost = 1;
            }
            continue;

        done_level:
            free(parent);
            goto done;
        }

        /* Advance to the parent level (the leaf keys are borrowed from `ents`;
         * interior levels' keys are also borrowed pointers into `ents`, so we
         * only free the RBENT array, never the key blobs). */
        free(level);
        level = parent;
        levelN = parentN;
        /* haveRightmost/rightmost now describe the parent level's rightmost. */
    }

    if (blockCount == 0 || !rootBody)
        goto done;

    /* Allocate one contiguous run for all INDX blocks (first-fit). */
    iaClusters = ((ULONGLONG)blockCount * blockSize + c->BytesPerCluster - 1) /
                 c->BytesPerCluster;
    iaLcn = chk_rb_alloc_clusters(c, iaClusters);
    if (iaLcn == 0)
        goto done;

    /* Serialize every INDX block into the run buffer, stamp fixups, write. */
    iaBuf = (UCHAR *)calloc(1, (size_t)iaClusters * c->BytesPerCluster);
    if (!iaBuf)
        goto done;
    {
        ULONG b;
        for (b = 0; b < blockCount; b++)
        {
            ULONGLONG blockIndex = blocks[b].vcn / (vcnPerBlock ? vcnPerBlock : 1);
            UCHAR *blk = iaBuf + (size_t)blockIndex * blockSize;
            INDEX_BUFFER *ib = (INDEX_BUFFER *)blk;
            ULONG usaSize = 1 + blockSize / c->BytesPerSector;
            USHORT *usa;

            ib->Magic = NRH_INDX_TYPE;
            ib->UpdateSeqOffset = (USHORT)sizeof(INDEX_BUFFER);
            ib->UpdateSeqSize = (USHORT)usaSize;
            ib->Vcn = blocks[b].vcn;
            usa = (USHORT *)(blk + ib->UpdateSeqOffset);
            usa[0] = 1;

            ib->Header.EntriesOffset = hdrOff - (ULONG)((UCHAR *)&ib->Header - blk);
            ib->Header.IndexLength = ib->Header.EntriesOffset + blocks[b].bodyLen;
            ib->Header.AllocatedSize = blockSize - hdrOff + ib->Header.EntriesOffset;
            ib->Header.Flags = blocks[b].interior ? 1 : 0;

            memcpy(blk + hdrOff, blocks[b].body, blocks[b].bodyLen);
            chk_stamp_fixups(blk, blockSize, c->BytesPerSector);
        }
        {
            CHK_RUN run;
            run.Lcn = iaLcn; run.Len = iaClusters; run.Sparse = 0;
            if (chk_stream_io(c->Io, &run, 1, c->BytesPerCluster, 0, iaBuf,
                              (ULONGLONG)iaClusters * c->BytesPerCluster, 1) != 0)
                goto done;
        }
    }

    /* Build the resident INDEX_ROOT (Flags=1) from the top level's body. */
    {
        ULONG valLen = sizeof(INDEX_ROOT) + rootLen;
        UCHAR *val = (UCHAR *)calloc(1, valLen);
        INDEX_ROOT *ir;
        if (!val)
            goto done;
        ir = (INDEX_ROOT *)val;
        ir->AttributeType = AT_FILE_NAME;
        ir->CollationRule = 1;
        ir->IndexBlockSize = blockSize;
        ir->ClustersPerIndexBlock = (UCHAR)clustersPerBlock;
        ir->Header.EntriesOffset = sizeof(INDEX_HEADER);
        ir->Header.IndexLength = sizeof(INDEX_HEADER) + rootLen;
        ir->Header.AllocatedSize = sizeof(INDEX_HEADER) + rootLen;
        ir->Header.Flags = 1;   /* large: has sub-nodes */
        memcpy(val + sizeof(INDEX_ROOT), rootBody, rootLen);
        ntfs_add_resident_attr(nextInstance, (FILE_RECORD_HEADER *)rec,
                               AT_INDEX_ROOT, chk_i30_name, 4, val, valLen, 0);
        free(val);
    }

    /* $INDEX_ALLOCATION (single run) covering every block. */
    ntfs_add_nonresident_attr(nextInstance, c->BytesPerCluster,
                              (FILE_RECORD_HEADER *)rec,
                              AT_INDEX_ALLOCATION, chk_i30_name, 4,
                              iaLcn, iaClusters,
                              (ULONGLONG)blockCount * blockSize,
                              (ULONGLONG)blockCount * blockSize);

    /* $BITMAP:$I30 with the low `blockCount` bits set. */
    bmpBytes = (ULONG)NTFS_ALIGN_UP((blockCount + 7) / 8, 8);
    bmp = (UCHAR *)calloc(1, bmpBytes);
    if (!bmp)
        goto done;
    {
        ULONG b;
        for (b = 0; b < blockCount; b++)
            chk_bmp_set(bmp, b);
    }
    ntfs_add_resident_attr(nextInstance, (FILE_RECORD_HEADER *)rec,
                           AT_BITMAP, chk_i30_name, 4, bmp, bmpBytes, 0);

    (void)res;
    (void)rootInterior;
    rc = 0;

done:
    free(level);
    {
        ULONG b;
        for (b = 0; b < blockCount; b++)
            free(blocks[b].body);
    }
    free(blocks);
    free(rootBody);
    free(iaBuf);
    free(bmp);
    return rc;
}

/* Undo pass-2's LinksSeen credit for one directory's current on-disk index,
 * expressed as one credit per child $FILE_NAME whose ParentDirectory == dirRec.
 * chk_rebuild_index re-credits each rebuilt link exactly once and assumes the
 * directory contributed nothing yet -- but pass-2's $I30 walk credits each
 * entry it visits BEFORE it may bail on a later bad entry, leaving a partial
 * credit.  Uncrediting first makes the assumption hold, so the rebuild leaves
 * LinksSeen reflecting exactly the rebuilt index (no double count). */
static void chk_uncredit_dir_links(CHK_CTX *c, ULONG dirRec)
{
    UCHAR *scan;
    ULONG recno;

    scan = (UCHAR *)malloc(c->MftRecordSize);
    if (!scan)
        return;
    for (recno = 0; recno < c->RecordCount; recno++)
    {
        FILE_RECORD_HEADER *sh = (FILE_RECORD_HEADER *)scan;
        ULONG off;
        USHORT f = c->Rec[recno].Flags;

        if ((f & (CRF_IN_USE | CRF_EXTENSION)) != CRF_IN_USE)
            continue;
        if (chk_read_record(c, recno, scan) != 0 ||
            *(ULONG *)scan != NRH_FILE_TYPE ||
            chk_apply_fixups(scan, c->MftRecordSize, c->BytesPerSector) != 0)
            continue;
        if (!(sh->Flags & FRH_IN_USE) || sh->BaseFileRecord != 0)
            continue;

        off = sh->AttributeOffset;
        while (off + 8 <= c->MftRecordSize)
        {
            ATTR_RECORD *a = (ATTR_RECORD *)(scan + off);
            if (a->Type == AT_END || a->Length == 0 ||
                off + a->Length > c->MftRecordSize)
                break;
            if (a->Type == AT_FILE_NAME && !a->NonResident &&
                a->Resident.ValueOffset + CHK_FN_FIXED <= a->Length)
            {
                FILE_NAME_ATTR *fn =
                    (FILE_NAME_ATTR *)(scan + off + a->Resident.ValueOffset);
                if ((ULONG)(fn->ParentDirectory & 0xFFFFFFFFFFFFULL) == dirRec &&
                    c->Rec[recno].LinksSeen > 0)
                    c->Rec[recno].LinksSeen--;
            }
            off += a->Length;
        }
    }
    free(scan);
}

int chk_rebuild_index(CHK_CTX *c, ULONG dirRec, NTFS_CHK_RESULT *res)
{
    UCHAR *dir = NULL, *scan = NULL;
    CHK_RB_ENT *ents = NULL;
    ULONG entCount = 0, entCap = 0;
    ULONG recno, k;
    ULONG blockSize, clustersPerBlock;
    USHORT nextInstance;
    ULONG smallBytes, i;
    int rc = -1;
    int isLarge;

    if (!c->Rec || dirRec >= c->RecordCount)
        return -1;
    if (!(c->Rec[dirRec].Flags & CRF_DIRECTORY))
        return -1;

    dir = (UCHAR *)malloc(c->MftRecordSize);
    scan = (UCHAR *)malloc(c->MftRecordSize);
    if (!dir || !scan)
        goto out;
    if (chk_read_record(c, dirRec, dir) != 0 ||
        *(ULONG *)dir != NRH_FILE_TYPE ||
        chk_apply_fixups(dir, c->MftRecordSize, c->BytesPerSector) != 0)
        goto out;

    /* Preserve the old INDEX_ROOT's block size before we strip it. */
    blockSize = c->IndexRecordSize;
    {
        ULONG irOff = chk_find_attr(c, dir, AT_INDEX_ROOT, chk_i30_name, 4);
        if (irOff)
        {
            ATTR_RECORD *a = (ATTR_RECORD *)(dir + irOff);
            if (!a->NonResident &&
                a->Resident.ValueLength >= sizeof(INDEX_ROOT))
            {
                INDEX_ROOT *r = (INDEX_ROOT *)(dir + irOff + a->Resident.ValueOffset);
                if (r->IndexBlockSize >= 512 &&
                    !(r->IndexBlockSize & (r->IndexBlockSize - 1)))
                    blockSize = r->IndexBlockSize;
            }
        }
    }
    clustersPerBlock = blockSize / c->BytesPerCluster;
    if (clustersPerBlock == 0)
        clustersPerBlock = 1;

    /* -------- Pass A: count-then-emit collection of child entries. -------- */
    for (recno = 0; recno < c->RecordCount; recno++)
    {
        FILE_RECORD_HEADER *sh;
        ULONG off;
        USHORT f;

        if (c->Rec)
        {
            f = c->Rec[recno].Flags;
            if ((f & (CRF_IN_USE | CRF_EXTENSION)) != CRF_IN_USE)
                continue;
        }
        if (chk_read_record(c, recno, scan) != 0 ||
            *(ULONG *)scan != NRH_FILE_TYPE ||
            chk_apply_fixups(scan, c->MftRecordSize, c->BytesPerSector) != 0)
            continue;
        sh = (FILE_RECORD_HEADER *)scan;
        if (!(sh->Flags & FRH_IN_USE) || sh->BaseFileRecord != 0)
            continue;

        off = sh->AttributeOffset;
        while (off + 8 <= c->MftRecordSize)
        {
            ATTR_RECORD *a = (ATTR_RECORD *)(scan + off);
            if (a->Type == AT_END || a->Length == 0 ||
                off + a->Length > c->MftRecordSize)
                break;
            if (a->Type == AT_FILE_NAME && !a->NonResident &&
                a->Resident.ValueOffset + CHK_FN_FIXED <= a->Length)
            {
                FILE_NAME_ATTR *fn =
                    (FILE_NAME_ATTR *)(scan + off + a->Resident.ValueOffset);
                ULONG keyBytes = CHK_FN_FIXED +
                                 (ULONG)fn->FileNameLength * sizeof(WCHAR);
                if ((ULONG)((ULONGLONG)fn->ParentDirectory & 0xFFFFFFFFFFFFULL) == dirRec &&
                    a->Resident.ValueOffset + keyBytes <= a->Length)
                {
                    int isDir = (sh->Flags & FRH_DIRECTORY) != 0;
                    UCHAR *blob;
                    ULONG blen, klen;

                    blob = chk_rb_make_entry(c, recno, sh->SequenceNumber,
                                             fn, isDir, &blen, &klen);
                    if (blob)
                    {
                        if (entCount == entCap)
                        {
                            ULONG nc = entCap ? entCap * 2 : 64;
                            CHK_RB_ENT *ne =
                                (CHK_RB_ENT *)chk_grow_alloc(ents,
                                                             entCap * sizeof(*ents),
                                                             nc * sizeof(*ents));
                            if (!ne) { free(blob); goto out; }
                            ents = ne; entCap = nc;
                        }
                        ents[entCount].blob = blob;
                        ents[entCount].len = blen;
                        ents[entCount].keyLen = klen;
                        ents[entCount].targetRec = recno;
                        entCount++;
                    }
                }
            }
            off += a->Length;
        }
    }

    /* -------- Sort + drop exact-duplicate keys (keep lowest targetRec). -- */
    chk_rb_sort(c, ents, entCount);
    {
        ULONG w = 0;
        for (i = 0; i < entCount; i++)
        {
            if (w > 0)
            {
                const FILE_NAME_ATTR *ka =
                    (const FILE_NAME_ATTR *)(ents[w - 1].blob + sizeof(INDEX_ENTRY));
                const FILE_NAME_ATTR *kb =
                    (const FILE_NAME_ATTR *)(ents[i].blob + sizeof(INDEX_ENTRY));
                if (ka->FileNameLength == kb->FileNameLength &&
                    chk_collate_filename(c, ka->FileName, ka->FileNameLength,
                                         kb->FileName, kb->FileNameLength) == 0)
                {
                    /* exact case-insensitive dup of same name in same dir */
                    ULONG cmp = 0;
                    while (cmp < ka->FileNameLength &&
                           ka->FileName[cmp] == kb->FileName[cmp])
                        cmp++;
                    if (cmp == ka->FileNameLength)
                    {
                        chk_add_issue(res, CHK_ERR_INDEX_STRUCT, dirRec,
                                      ents[i].targetRec, 1);
                        free(ents[i].blob);
                        continue;
                    }
                }
            }
            ents[w++] = ents[i];
        }
        entCount = w;
    }

    /* -------- Rewrite the directory record: strip old $I30, free its
     *          clusters, then emit the new small or large layout. -------- */
    chk_rb_free_old_ia(c, dir);
    chk_rb_strip_old_attrs(c, dir);

    nextInstance = ((FILE_RECORD_HEADER *)dir)->NextAttributeId;

    /* Decide small vs large: does INDEX_ROOT(hdr)+entries+END fit the record? */
    smallBytes = sizeof(INDEX_ROOT);
    for (i = 0; i < entCount; i++)
        smallBytes += ents[i].len;
    smallBytes += sizeof(INDEX_ENTRY);   /* END */

    isLarge = (smallBytes + 0x18 + 8 > chk_rb_record_free(c, dir));

    if (!isLarge)
    {
        if (chk_rb_emit_small(c, dir, ents, entCount, blockSize,
                              clustersPerBlock, &nextInstance) != 0)
            goto out;
    }
    else
    {
        if (chk_rb_emit_large(c, dir, ents, entCount, blockSize,
                              clustersPerBlock, &nextInstance, res) != 0)
            goto out;
    }

    ((FILE_RECORD_HEADER *)dir)->NextAttributeId = nextInstance;

    chk_stamp_fixups(dir, c->MftRecordSize, c->BytesPerSector);
    if (chk_write_record(c, dirRec, dir) != 0)
        goto out;
    if (dirRec < FILE_FIRST_USER)
        chk_sync_mftmirr_record(c, dirRec);

    /* Mark the directory's index issues fixed + clear CRF_INDEX_BAD. */
    for (k = 0; k < res->IssueCount && k < CHK_MAX_ISSUES; k++)
    {
        NTFS_CHK_ISSUE *iss = &res->Issues[k];
        if (iss->Fixed || (ULONG)iss->Param0 != dirRec)
            continue;
        if (iss->Code == CHK_ERR_INDEX_STRUCT || iss->Code == CHK_ERR_INDEX_ORDER ||
            iss->Code == CHK_ERR_INDEX_BACKREF || iss->Code == CHK_ERR_INDEX_BADREF ||
            iss->Code == CHK_ERR_INDEX_CYCLE || iss->Code == CHK_ERR_INDX_BAD_BLOCK)
        {
            iss->Fixed = 1;
            res->RepairedCount++;
        }
    }
    c->Rec[dirRec].Flags &= (USHORT)~CRF_INDEX_BAD;

    /* Credit each re-homed child's model LinksSeen so the later R4 link-count
     * pass sees the correct count.  Pass-2 may have partially credited this
     * directory before its walk bailed on a bad entry, so first undo any credit
     * its current on-disk children carry for it, then re-add exactly one link
     * per surviving rebuilt entry.  Also clears any stale ORPHAN / LINKCOUNT
     * issue already reported for a re-homed child. */
    chk_uncredit_dir_links(c, dirRec);
    for (i = 0; i < entCount; i++)
    {
        ULONG tr = ents[i].targetRec;
        if (tr < c->RecordCount)
        {
            c->Rec[tr].LinksSeen++;
            c->Rec[tr].Flags &= (USHORT)~CRF_ORPHAN;
            for (k = 0; k < res->IssueCount && k < CHK_MAX_ISSUES; k++)
            {
                NTFS_CHK_ISSUE *iss = &res->Issues[k];
                if (!iss->Fixed && (ULONG)iss->Param0 == tr &&
                    (iss->Code == CHK_ERR_ORPHAN ||
                     iss->Code == CHK_ERR_LINKCOUNT))
                {
                    iss->Fixed = 1;
                    res->RepairedCount++;
                }
            }
        }
    }

    res->IndexesRebuilt++;
    CHK_TRACE("R2: rebuilt $I30 of rec %lu (%lu entries, %s)\n",
              (unsigned long)dirRec, (unsigned long)entCount,
              isLarge ? "large" : "small");
    rc = 0;

out:
    for (i = 0; i < entCount; i++)
        free(ents[i].blob);
    free(ents);
    free(dir);
    free(scan);
    return rc;
}

/* ============================================================
 * S6 (R3): orphan recovery into found.NNN
 *
 * chkdsk's classic "recover orphans": in-use base records that no directory
 * index reaches are re-homed under a synthetic found.NNN directory created in
 * the root, so no live file is silently lost.  Steps:
 *   1. count CRF_ORPHAN base records (system 0-15 exempt);
 *   2. allocate a found.NNN name + a free MFT record >=16 and build it as a
 *      directory whose $FILE_NAME parents to root (matching root's $SI times);
 *   3. re-home each orphan's $FILE_NAME.ParentDirectory to found.NNN (or, if it
 *      has no $FILE_NAME, fabricate FILEnnnn.CHK), writing the record to disk;
 *   4. rebuild found.NNN's $I30 (now populated by the re-homed children) and the
 *      root's $I30 (now containing found.NNN) via chk_rebuild_index.
 *
 * H4: the new record's SequenceNumber is (stale header seq + 1); its ref uses
 * the same seq.  Ordering: every re-homed record and found.NNN itself are
 * written to disk BEFORE the two rebuilds, because chk_rebuild_index re-scans
 * the MFT reading each record's on-disk $FILE_NAME.ParentDirectory.
 * ============================================================ */

/* Read root's $STANDARD_INFORMATION SecurityId + a representative timestamp so
 * found.NNN inherits sensible values.  Falls back to zero on any failure. */
static void chk_orphan_root_si(CHK_CTX *c, ULONG *secId, ULONGLONG *timeVal)
{
    UCHAR *root;
    ULONG off;

    *secId = 0;
    *timeVal = 0;
    root = (UCHAR *)malloc(c->MftRecordSize);
    if (!root)
        return;
    if (chk_read_record(c, FILE_Root, root) == 0 &&
        *(ULONG *)root == NRH_FILE_TYPE &&
        chk_apply_fixups(root, c->MftRecordSize, c->BytesPerSector) == 0 &&
        (off = chk_find_attr(c, root, AT_STANDARD_INFORMATION, NULL, 0)) != 0)
    {
        ATTR_RECORD *a = (ATTR_RECORD *)(root + off);
        if (!a->NonResident &&
            a->Resident.ValueOffset + sizeof(STANDARD_INFORMATION) <= a->Length)
        {
            STANDARD_INFORMATION *si =
                (STANDARD_INFORMATION *)(root + off + a->Resident.ValueOffset);
            *secId = si->SecurityId;
            *timeVal = si->CreationTime;
        }
    }
    free(root);
}

/* Does any in-use record own a Win32/POSIX $FILE_NAME whose ParentDirectory is
 * FILE_Root and whose name equals `name` (case-sensitively, since found.NNN is
 * ASCII)?  Used to skip a found.NNN slot that already exists. */
static int chk_orphan_name_taken(CHK_CTX *c, const WCHAR *name, ULONG nameLen)
{
    UCHAR *rec;
    ULONG recno;
    int taken = 0;

    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return 0;
    for (recno = FILE_FIRST_USER; recno < c->RecordCount && !taken; recno++)
    {
        FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
        ULONG off;
        if (!c->Rec || !(c->Rec[recno].Flags & CRF_IN_USE))
            continue;
        if (chk_read_record(c, recno, rec) != 0 ||
            *(ULONG *)rec != NRH_FILE_TYPE ||
            chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
            continue;
        if (!(h->Flags & FRH_IN_USE) || h->BaseFileRecord != 0)
            continue;
        off = h->AttributeOffset;
        while (off + 8 <= c->MftRecordSize)
        {
            ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
            if (a->Type == AT_END || a->Length == 0 ||
                off + a->Length > c->MftRecordSize)
                break;
            if (a->Type == AT_FILE_NAME && !a->NonResident)
            {
                FILE_NAME_ATTR *fn =
                    (FILE_NAME_ATTR *)(rec + off + a->Resident.ValueOffset);
                if ((ULONG)(fn->ParentDirectory & 0xFFFFFFFFFFFFULL) == FILE_Root &&
                    fn->FileNameLength == nameLen)
                {
                    ULONG k = 0;
                    while (k < nameLen && fn->FileName[k] == name[k])
                        k++;
                    if (k == nameLen)
                        taken = 1;
                }
            }
            off += a->Length;
        }
    }
    free(rec);
    return taken;
}

/* Build the found.NNN name (POSIX ASCII WCHARs) into name[], returns length. */
static ULONG chk_orphan_found_name(WCHAR *name, ULONG nnn)
{
    name[0] = 'f'; name[1] = 'o'; name[2] = 'u'; name[3] = 'n'; name[4] = 'd';
    name[5] = '.';
    name[6] = (WCHAR)('0' + (nnn / 100) % 10);
    name[7] = (WCHAR)('0' + (nnn / 10) % 10);
    name[8] = (WCHAR)('0' + nnn % 10);
    return 9;
}

/* Find a free MFT record index >= FILE_FIRST_USER (MftMap bit clear), returns
 * (ULONG)-1 if none.  Harvests the stale header sequence number of that slot so
 * the caller can allocate seq+1 (H4). */
static ULONG chk_orphan_free_record(CHK_CTX *c, USHORT *staleSeq)
{
    UCHAR *rec;
    ULONG recno, found = (ULONG)-1;

    *staleSeq = 0;
    if (!c->MftMap)
        return (ULONG)-1;
    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return (ULONG)-1;
    for (recno = FILE_FIRST_USER; recno < c->RecordCount; recno++)
    {
        if (chk_bmp_get(c->MftMap, recno))
            continue;
        /* Free slot: harvest its stale seq if the slot still parses. */
        if (chk_read_record(c, recno, rec) == 0 &&
            *(ULONG *)rec == NRH_FILE_TYPE &&
            chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) == 0)
        {
            USHORT s = ((FILE_RECORD_HEADER *)rec)->SequenceNumber;
            *staleSeq = s ? s : 1;
        }
        else
        {
            *staleSeq = 1;
        }
        found = recno;
        break;
    }
    free(rec);
    return found;
}

/* Re-home one orphan record: rewrite its first usable Win32/POSIX
 * $FILE_NAME.ParentDirectory to `foundRef`; if it has NO $FILE_NAME at all,
 * fabricate FILEnnnn.CHK parented to found.NNN.  Writes the record back.
 * Returns 0 on success, -1 on failure. */
static int chk_orphan_rehome(CHK_CTX *c, ULONG recno, ULONGLONG foundRef,
                             ULONG secId, ULONGLONG timeVal)
{
    UCHAR *rec;
    FILE_RECORD_HEADER *h;
    ULONG off;
    int rehomed = 0, rc = -1;

    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return -1;
    if (chk_read_record(c, recno, rec) != 0 ||
        *(ULONG *)rec != NRH_FILE_TYPE ||
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
        { free(rec); return -1; }

    h = (FILE_RECORD_HEADER *)rec;

    /* Retarget the first non-DOS (or, failing that, the first) $FILE_NAME. */
    off = h->AttributeOffset;
    while (off + 8 <= c->MftRecordSize)
    {
        ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
        if (a->Type == AT_END || a->Length == 0 ||
            off + a->Length > c->MftRecordSize)
            break;
        if (a->Type == AT_FILE_NAME && !a->NonResident)
        {
            FILE_NAME_ATTR *fn =
                (FILE_NAME_ATTR *)(rec + off + a->Resident.ValueOffset);
            /* Point every $FILE_NAME at found.NNN so no key back-refs the old
             * (dead) parent -- multiple names of one file all move together. */
            fn->ParentDirectory = foundRef;
            rehomed = 1;
        }
        off += a->Length;
    }

    if (!rehomed)
    {
        /* No $FILE_NAME: fabricate FILEnnnn.CHK (POSIX) under found.NNN. */
        WCHAR nm[12];
        FILE_NAME_ATTR *fnbuf;
        ULONG nlen, fnBytes;
        int isDir = (h->Flags & FRH_DIRECTORY) != 0;
        ULONG fileAttrs = isDir ? FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT
                                : FILE_ATTR_ARCHIVE;
        USHORT nextInstance = h->NextAttributeId;

        nm[0]='F'; nm[1]='I'; nm[2]='L'; nm[3]='E';
        nm[4] = (WCHAR)('0' + (recno / 1000) % 10);
        nm[5] = (WCHAR)('0' + (recno / 100) % 10);
        nm[6] = (WCHAR)('0' + (recno / 10) % 10);
        nm[7] = (WCHAR)('0' + recno % 10);
        nm[8]='.'; nm[9]='C'; nm[10]='H'; nm[11]='K';
        nlen = 12;

        fnBytes = (ULONG)(sizeof(FILE_NAME_ATTR) - sizeof(WCHAR) +
                          nlen * sizeof(WCHAR));
        fnbuf = (FILE_NAME_ATTR *)calloc(1, fnBytes);
        if (!fnbuf)
            { free(rec); return -1; }
        ntfs_make_file_name(fnbuf, foundRef, nm, (UCHAR)nlen,
                            FILE_NAME_POSIX, fileAttrs, 0, 0, timeVal);
        ntfs_add_resident_attr(&nextInstance, h, AT_FILE_NAME, NULL, 0,
                               fnbuf, fnBytes, 0);
        h->NextAttributeId = nextInstance;
        free(fnbuf);
        (void)secId;
    }

    chk_stamp_fixups(rec, c->MftRecordSize, c->BytesPerSector);
    if (chk_write_record(c, recno, rec) == 0)
        rc = 0;
    free(rec);
    return rc;
}

/* Set one bit in the on-disk $MFT:$BITMAP for a newly-allocated record.  R5
 * (chk_repair_mft_bitmap) only fires when a mismatch was detected up front;
 * found.NNN is allocated during repair, so its bit must be persisted here or a
 * re-check would flag CHK_ERR_MFTBMP_MISMATCH.  Resident and non-resident
 * $MFT:$BITMAP are both handled.  Returns 0 on success, -1 on failure. */
static int chk_orphan_set_mft_bit(CHK_CTX *c, ULONG recno)
{
    UCHAR *rec;
    ULONG off;
    int rc = -1;

    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return -1;
    if (chk_read_record(c, FILE_MFT, rec) != 0 ||
        *(ULONG *)rec != NRH_FILE_TYPE ||
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0 ||
        (off = chk_find_attr(c, rec, AT_BITMAP, NULL, 0)) == 0)
        { free(rec); return -1; }

    {
        ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
        ULONGLONG byteOff = recno / 8;
        UCHAR mask = (UCHAR)(1u << (recno & 7));

        if (!a->NonResident)
        {
            if (byteOff < a->Resident.ValueLength)
            {
                UCHAR *dst = rec + off + a->Resident.ValueOffset;
                dst[byteOff] |= mask;
                chk_stamp_fixups(rec, c->MftRecordSize, c->BytesPerSector);
                if (chk_write_record(c, FILE_MFT, rec) == 0)
                    rc = 0;
            }
        }
        else
        {
            CHK_RUN *runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
            if (runs)
            {
                int n = chk_decode_runs(rec + off + a->NR.MappingPairsOffset,
                                        rec + off + a->Length, runs, CHK_MAX_RUNS);
                if (n > 0 && byteOff < (ULONGLONG)a->NR.DataSize)
                {
                    UCHAR b = 0;
                    if (chk_stream_io(c->Io, runs, (ULONG)n, c->BytesPerCluster,
                                      byteOff, &b, 1, 0) == 0)
                    {
                        b |= mask;
                        if (chk_stream_io(c->Io, runs, (ULONG)n, c->BytesPerCluster,
                                          byteOff, &b, 1, 1) == 0)
                            rc = 0;
                    }
                }
                free(runs);
            }
        }
    }
    free(rec);
    return rc;
}

int chk_recover_orphans(CHK_CTX *c, NTFS_CHK_RESULT *res)
{
    ULONG recno, orphanCount = 0;
    ULONG nnn, foundRec = (ULONG)-1;
    USHORT staleSeq = 0, foundSeq;
    ULONGLONG foundRef, rootRef;
    ULONG rootSecId = 0;
    ULONGLONG rootTime = 0;
    WCHAR foundName[9];
    ULONG foundNameLen = 0;
    UCHAR *rec = NULL;
    int rehomed = 0;

    if (!c->Rec || !c->MftMap)
        return -1;

    /* 1. Count real orphans (in-use base records, never system 0-15). */
    for (recno = FILE_FIRST_USER; recno < c->RecordCount; recno++)
    {
        USHORT f = c->Rec[recno].Flags;
        if ((f & (CRF_IN_USE | CRF_EXTENSION | CRF_CORRUPT | CRF_SYSTEM))
                == CRF_IN_USE &&
            (f & CRF_ORPHAN))
            orphanCount++;
    }
    if (orphanCount == 0)
        return 0;

    /* 2a. Pick the first free found.NNN name that is not already a root child. */
    for (nnn = 0; nnn < 1000; nnn++)
    {
        foundNameLen = chk_orphan_found_name(foundName, nnn);
        if (!chk_orphan_name_taken(c, foundName, foundNameLen))
            break;
    }
    if (nnn >= 1000)
        return 0;   /* absurd: 1000 found.NNN already exist -- give up quietly */

    /* 2b. Find a free MFT record; MFT extension is deferred (do not fabricate). */
    foundRec = chk_orphan_free_record(c, &staleSeq);
    if (foundRec == (ULONG)-1)
    {
        chk_add_issue(res, CHK_ERR_ORPHAN, orphanCount, 0, 0);
        CHK_TRACE("R3: MFT full: %lu orphans unrecovered\n",
                  (unsigned long)orphanCount);
        return 0;
    }
    foundSeq = (USHORT)(staleSeq + 1);
    if (foundSeq == 0)
        foundSeq = 1;
    foundRef = ntfs_mft_ref(foundRec, foundSeq);

    /* Root reference: use root's live header sequence number for the back-ref. */
    {
        USHORT rootSeq = c->Rec[FILE_Root].SeqNumber;
        rootRef = ntfs_mft_ref(FILE_Root, rootSeq ? rootSeq : 1);
    }
    chk_orphan_root_si(c, &rootSecId, &rootTime);

    /* 3. Build the found.NNN directory record and write it FIRST (so the later
     *    root rebuild sees its $FILE_NAME on disk). */
    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return -1;
    {
        FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
        USHORT nextInstance;
        STANDARD_INFORMATION si;
        FILE_NAME_ATTR *fn;
        ULONG fnBytes;

        ntfs_init_file_record(h, c->MftRecordSize, c->BytesPerSector,
                              foundSeq, FRH_IN_USE | FRH_DIRECTORY, foundRec);
        h->LinkCount = 1;
        nextInstance = h->NextAttributeId;

        /* $STANDARD_INFORMATION (times/SecurityId inherited from root). */
        memset(&si, 0, sizeof(si));
        si.CreationTime = rootTime;
        si.ModificationTime = rootTime;
        si.MftModificationTime = rootTime;
        si.AccessTime = rootTime;
        si.FileAttributes = FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT;
        si.SecurityId = rootSecId;
        ntfs_add_resident_attr(&nextInstance, h, AT_STANDARD_INFORMATION,
                               NULL, 0, &si, sizeof(si), 0);

        /* $FILE_NAME (found.NNN, POSIX) parented to root. */
        fnBytes = (ULONG)(sizeof(FILE_NAME_ATTR) - sizeof(WCHAR) +
                          foundNameLen * sizeof(WCHAR));
        fn = (FILE_NAME_ATTR *)calloc(1, fnBytes);
        if (!fn)
            { free(rec); return -1; }
        ntfs_make_file_name(fn, rootRef, foundName, (UCHAR)foundNameLen,
                            FILE_NAME_POSIX,
                            FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT, 0, 0,
                            rootTime);
        ntfs_add_resident_attr(&nextInstance, h, AT_FILE_NAME, NULL, 0,
                               fn, fnBytes, 0);
        free(fn);

        /* Empty $INDEX_ROOT ($I30): chk_rebuild_index reads its IndexBlockSize
         * and then re-emits it populated. */
        {
            UCHAR idx[sizeof(INDEX_ROOT) + sizeof(INDEX_ENTRY)];
            INDEX_ROOT *ir = (INDEX_ROOT *)idx;
            INDEX_ENTRY *ie;
            ULONG cpb = c->IndexRecordSize / c->BytesPerCluster;
            memset(idx, 0, sizeof(idx));
            ir->AttributeType = AT_FILE_NAME;
            ir->CollationRule = 1;   /* COLLATION_FILE_NAME */
            ir->IndexBlockSize = c->IndexRecordSize;
            ir->ClustersPerIndexBlock = (UCHAR)(cpb ? cpb : 1);
            ir->Header.EntriesOffset = sizeof(INDEX_HEADER);
            ir->Header.IndexLength = sizeof(INDEX_HEADER) + sizeof(INDEX_ENTRY);
            ir->Header.AllocatedSize = sizeof(INDEX_HEADER) + sizeof(INDEX_ENTRY);
            ir->Header.Flags = 0;
            ie = (INDEX_ENTRY *)(idx + sizeof(INDEX_ROOT));
            ie->Length = sizeof(INDEX_ENTRY);
            ie->Flags = INDEX_ENTRY_END;
            ntfs_add_resident_attr(&nextInstance, h, AT_INDEX_ROOT,
                                   chk_i30_name, 4, idx, sizeof(idx), 0);
        }

        h->NextAttributeId = nextInstance;
        chk_stamp_fixups(rec, c->MftRecordSize, c->BytesPerSector);
        if (chk_write_record(c, foundRec, rec) != 0)
            { free(rec); return -1; }
    }
    free(rec);
    rec = NULL;

    /* Mark found.NNN allocated + model it as an in-use root-child directory so
     * both rebuilds accept it (found's own $I30 rebuild + root's, which must
     * include found.NNN). */
    chk_bmp_set(c->MftMap, foundRec);
    /* Persist the bit to $MFT:$BITMAP now: R5 only runs on a pre-detected
     * mismatch, and found.NNN was allocated during this repair pass. */
    chk_orphan_set_mft_bit(c, foundRec);
    {
        CHK_REC *m = &c->Rec[foundRec];
        m->Flags = (USHORT)(CRF_IN_USE | CRF_DIRECTORY);
        m->SeqNumber = foundSeq;
        m->LinkCountHdr = 1;
        m->LinksSeen = 0;   /* root rebuild will credit the +1 */
        m->FnCount = 1;
        m->ParentRef = rootRef;
    }

    /* 4. Re-home every orphan onto found.NNN and write each record BEFORE the
     *    rebuilds (chk_rebuild_index re-scans on-disk ParentDirectory). */
    for (recno = FILE_FIRST_USER; recno < c->RecordCount; recno++)
    {
        CHK_REC *m = &c->Rec[recno];
        USHORT f = m->Flags;
        ULONG k;

        if ((f & (CRF_IN_USE | CRF_EXTENSION | CRF_CORRUPT | CRF_SYSTEM))
                != CRF_IN_USE ||
            !(f & CRF_ORPHAN))
            continue;

        if (chk_orphan_rehome(c, recno, foundRef, rootSecId, rootTime) != 0)
            continue;

        /* Model: now a reachable child of found.NNN. */
        m->ParentRef = foundRef;
        m->Flags &= (USHORT)~CRF_ORPHAN;
        m->Flags |= CRF_REACHABLE;
        if (m->FnCount == 0)
            m->FnCount = 1;   /* fabricated FILEnnnn.CHK */

        res->OrphansRecovered++;
        rehomed = 1;

        /* Clear the reported ORPHAN issue for this record. */
        for (k = 0; k < res->IssueCount && k < CHK_MAX_ISSUES; k++)
        {
            NTFS_CHK_ISSUE *iss = &res->Issues[k];
            if (!iss->Fixed && iss->Code == CHK_ERR_ORPHAN &&
                (ULONG)iss->Param0 == recno)
            {
                iss->Fixed = 1;
                res->RepairedCount++;
            }
        }
        CHK_TRACE("R3: re-homed orphan rec %lu under found.%03lu\n",
                  (unsigned long)recno, (unsigned long)nnn);
    }

    if (!rehomed)
    {
        /* No orphan actually moved (all re-home writes failed): back out the
         * model change so we do not leave a phantom directory. */
        c->Rec[foundRec].Flags = 0;
        chk_bmp_clear(c->MftMap, foundRec);
        return 0;
    }

    /* 5. Build found.NNN's $I30 from its now-parented children (its children's
     *    LinksSeen were 0 -- orphans -- so no uncredit needed there). */
    chk_rebuild_index(c, foundRec, res);

    /* Insert found.NNN into the root index.  chk_rebuild_index uncredits root's
     * current LinksSeen contribution before re-crediting, so root children are
     * not double-counted (found.NNN itself is credited exactly once). */
    chk_rebuild_index(c, FILE_Root, res);

    CHK_TRACE("R3: recovered %lu orphans into found.%03lu (rec %lu)\n",
              (unsigned long)res->OrphansRecovered, (unsigned long)nnn,
              (unsigned long)foundRec);
    return 0;
}

/* ============================================================
 * Test / utility entry points
 * ============================================================ */

/* Force the volume dirty (used by the harness to seed the dirty repair). */
int NtfsChkSetVolumeDirty(const MKNTFS_IO *io)
{
    CHK_CTX *c;   /* ~197 KiB: too large for the stack */
    NTFS_CHK_RESULT tmp;
    int rc;

    c = (CHK_CTX *)malloc(sizeof(*c));
    if (!c)
        return -1;
    memset(c, 0, sizeof(*c));
    memset(&tmp, 0, sizeof(tmp));
    c->Io = io;
    if (chk_read_boot(c, &tmp) != 0)
        { free(c); return -1; }
    if (chk_load_mft(c, &tmp) != 0)
        { free(c); return -1; }
    rc = chk_set_volume_flag(c, 1, 0);
    free(c);
    return rc;
}

/* Clear the $Bitmap bit for the MFT's own first cluster, fabricating a
 * "used but marked free" (underalloc) condition.  Returns the cleared LCN
 * in *clearedLcn.  Used by the harness to exercise repair. */
int NtfsChkTestClearMftBit(const MKNTFS_IO *io, ULONGLONG *clearedLcn)
{
    CHK_CTX *c;   /* ~197 KiB: too large for the stack */
    NTFS_CHK_RESULT tmp;
    UCHAR *rec6;
    CHK_RUN *bmpRuns;
    ULONGLONG lcn, byteOff;
    UCHAR b;
    int nruns;

    c = (CHK_CTX *)malloc(sizeof(*c));
    bmpRuns = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    if (!c || !bmpRuns)
    {
        free(c);
        free(bmpRuns);
        return -1;
    }
    memset(c, 0, sizeof(*c));
    memset(&tmp, 0, sizeof(tmp));
    c->Io = io;
    if (chk_read_boot(c, &tmp) != 0 || chk_load_mft(c, &tmp) != 0)
        { free(bmpRuns); free(c); return -1; }

    rec6 = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec6)
        { free(bmpRuns); free(c); return -1; }
    if (chk_read_record(c, FILE_Bitmap, rec6) != 0 ||
        *(ULONG *)rec6 != NRH_FILE_TYPE ||
        chk_apply_fixups(rec6, c->MftRecordSize, c->BytesPerSector) != 0)
        { free(rec6); free(bmpRuns); free(c); return -1; }
    nruns = chk_find_data_runs(c, rec6, bmpRuns, CHK_MAX_RUNS, NULL);
    free(rec6);
    if (nruns <= 0)
        { free(bmpRuns); free(c); return -1; }

    lcn = c->MftLcn;
    byteOff = lcn / 8;
    if (chk_stream_io(io, bmpRuns, (ULONG)nruns, c->BytesPerCluster,
                      byteOff, &b, 1, 0) != 0)
        { free(bmpRuns); free(c); return -1; }
    b &= (UCHAR)~(1u << (lcn & 7));
    if (chk_stream_io(io, bmpRuns, (ULONG)nruns, c->BytesPerCluster,
                      byteOff, &b, 1, 1) != 0)
        { free(bmpRuns); free(c); return -1; }
    if (clearedLcn)
        *clearedLcn = lcn;
    free(bmpRuns);
    free(c);
    return 0;
}
