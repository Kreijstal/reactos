/*
 * ntfslib - NTFS consistency checker: detection passes
 * License: GPL-2.0-or-later
 *
 * Read-only scan passes over the volume.  Fills the model in CHK_CTX and
 * reports issues; the only writes ever issued from this unit are the
 * FixErrors-gated bitmap repair in chk_crosscheck_bitmap (to be migrated
 * to chkrepair.c when the full repair state machine lands).
 *
 * Pass 1  chk_scan_records     records, attrs, cluster accounting, model
 *         + attribute-list validation sweep (pass 1.5)
 * Pass 2  chk_walk_indexes     $I30 b-trees: structure, order, back-refs
 * Pass 3  chk_connectivity     orphans, directory cycles, link counts
 * Pass 4  chk_check_mft_bitmap $MFT:$BITMAP vs computed in-use records
 * Pass 5  chk_crosscheck_bitmap $Bitmap vs computed cluster map (both dirs)
 * Pass 6  chk_check_mftmirr    $MFTMirr byte-compare
 */

#include "chkint.h"

static const WCHAR chk_I30[4] = { '$', 'I', '3', '0' };

#define CHK_REF_REC(ref)  ((ULONG)((ref) & 0xFFFFFFFFFFFFULL))
#define CHK_REF_SEQ(ref)  ((USHORT)((ref) >> 48))

/* ============================================================
 * Pass 1: per-record scan + cluster accounting
 * ============================================================ */

static void chk_account_attr(CHK_CTX *c, UCHAR *rec, ULONG attrOff,
                             UCHAR *computed, NTFS_CHK_RESULT *res, ULONG recno)
{
    ATTR_RECORD *a = (ATTR_RECORD *)(rec + attrOff);
    CHK_RUN *runs;   /* 192 KiB: too large for the stack */
    const UCHAR *mp = rec + attrOff + a->NR.MappingPairsOffset;
    const UCHAR *end = rec + attrOff + a->Length;
    int n, i;
    int reportedOob = 0, reportedXlink = 0;

    runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    if (!runs)
        return;
    n = chk_decode_runs(mp, end, runs, CHK_MAX_RUNS);
    if (n < 0)
    {
        free(runs);
        return;
    }
    for (i = 0; i < n; i++)
    {
        ULONGLONG cl;
        if (runs[i].Sparse)
            continue;
        for (cl = 0; cl < runs[i].Len; cl++)
        {
            ULONGLONG lcn = runs[i].Lcn + cl;
            if (lcn >= c->TotalClusters)
            {
                if (!reportedOob)
                    chk_add_issue(res, CHK_ERR_RUN_OOB, recno, lcn, 0);
                reportedOob = 1;
                continue;
            }
            if (chk_bmp_get(computed, lcn))
            {
                /* cross-linked cluster: someone claimed it already */
                if (c->XLinkCount < CHK_MAX_XLINKS)
                {
                    c->XLinks[c->XLinkCount].Lcn = lcn;
                    c->XLinks[c->XLinkCount].FirstRec = (ULONG)-1;
                    c->XLinks[c->XLinkCount].SecondRec = recno;
                }
                c->XLinkCount++;
                if (!reportedXlink)
                    chk_add_issue(res, CHK_ERR_XLINK, recno, lcn, 0);
                reportedXlink = 1;
                continue;
            }
            chk_bmp_set(computed, lcn);
        }
        res->ClustersUsed += runs[i].Len;
    }
    free(runs);
}

/* Validate one base record's $ATTRIBUTE_LIST (pass 1.5 helper). */
static void chk_check_attrlist(CHK_CTX *c, UCHAR *rec, ULONG recno,
                               NTFS_CHK_RESULT *res)
{
    FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
    ULONG alOff = chk_find_attr(c, rec, AT_ATTRIBUTE_LIST, NULL, 0);
    UCHAR *val;
    ULONGLONG size = 0, pos;
    ULONGLONG baseRef;

    if (!alOff)
        return;

    val = chk_read_attr_value(c, rec, alOff, 1024 * 1024, &size);
    if (!val)
    {
        chk_add_issue(res, CHK_ERR_ATTRLIST_BROKEN, recno, 0, 0);
        if (c->Rec)
            c->Rec[recno].Flags |= CRF_ATTRLIST_UNSUPPORTED;
        return;
    }

    baseRef = ntfs_mft_ref(recno, h->SequenceNumber);

    pos = 0;
    while (pos + sizeof(ATTR_LIST_ENTRY) <= size)
    {
        ATTR_LIST_ENTRY *e = (ATTR_LIST_ENTRY *)(val + pos);
        ULONG extRec;

        if (e->Length < sizeof(ATTR_LIST_ENTRY) || pos + e->Length > size)
        {
            chk_add_issue(res, CHK_ERR_ATTRLIST_BROKEN, recno, pos, 0);
            break;
        }

        extRec = CHK_REF_REC(e->MftReference);
        if (extRec != recno)
        {
            /* entry lives in an extension record */
            if (extRec >= c->RecordCount || !c->Rec ||
                !(c->Rec[extRec].Flags & CRF_IN_USE) ||
                !(c->Rec[extRec].Flags & CRF_EXTENSION) ||
                c->Rec[extRec].SeqNumber != CHK_REF_SEQ(e->MftReference))
            {
                chk_add_issue(res, CHK_ERR_ATTRLIST_BROKEN, recno, extRec, 0);
            }
            else
            {
                UCHAR *ext = (UCHAR *)malloc(c->MftRecordSize);
                if (ext)
                {
                    if (chk_read_record(c, extRec, ext) == 0 &&
                        *(ULONG *)ext == NRH_FILE_TYPE &&
                        chk_apply_fixups(ext, c->MftRecordSize, c->BytesPerSector) == 0)
                    {
                        FILE_RECORD_HEADER *eh = (FILE_RECORD_HEADER *)ext;
                        if (eh->BaseFileRecord != baseRef)
                            chk_add_issue(res, CHK_ERR_ATTRLIST_BROKEN,
                                          recno, extRec, 0);
                        else
                            c->Rec[extRec].Flags |= CRF_CLAIMED;
                    }
                    free(ext);
                }
            }

            /* $I30 attrs living outside the base record: rebuild unsupported */
            if (e->Type == AT_INDEX_ROOT || e->Type == AT_INDEX_ALLOCATION ||
                (e->Type == AT_BITMAP && e->NameLength == 4))
            {
                if (c->Rec)
                    c->Rec[recno].Flags |= CRF_ATTRLIST_UNSUPPORTED;
            }
        }
        pos += e->Length;
    }

    free(val);
}

int chk_scan_records(CHK_CTX *c, UCHAR *computed, NTFS_CHK_RESULT *res,
                     const NTFS_CHK_OPTIONS *opt)
{
    UCHAR *rec = (UCHAR *)malloc(c->MftRecordSize);
    ULONG recno;

    if (!rec)
        return -1;

    for (recno = 0; recno < c->RecordCount; recno++)
    {
        FILE_RECORD_HEADER *h = (FILE_RECORD_HEADER *)rec;
        CHK_REC *m = c->Rec ? &c->Rec[recno] : NULL;
        ULONG off;
        int hasStdInfo = 0;
        int isExtension;
        int inUse;

        if (chk_read_record(c, recno, rec) != 0)
            continue;
        res->RecordsScanned++;

        if (*(ULONG *)rec != NRH_FILE_TYPE)
        {
            /* A free slot is normally zeroed; non-zero garbage is corruption. */
            ULONG i, nz = 0;
            for (i = 0; i < 16; i++)
                nz |= rec[i];
            if (nz)
            {
                chk_add_issue(res, CHK_ERR_REC_MAGIC, recno, 0, 0);
                if (m)
                    m->Flags |= CRF_CORRUPT;
            }
            continue;
        }
        if (chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
        {
            chk_add_issue(res, CHK_ERR_USA_MISMATCH, recno, 0, 0);
            if (m)
                m->Flags |= CRF_CORRUPT;
            continue;
        }

        inUse = (h->Flags & FRH_IN_USE) != 0;
        if (!inUse)
            continue;
        res->RecordsInUse++;

        isExtension = (h->BaseFileRecord != 0);

        if (m)
        {
            m->Flags |= CRF_IN_USE;
            m->SeqNumber = h->SequenceNumber;
            m->LinkCountHdr = h->LinkCount;
            if (h->Flags & FRH_DIRECTORY)
                m->Flags |= CRF_DIRECTORY;
            if (isExtension)
                m->Flags |= CRF_EXTENSION;
            if (recno < FILE_FIRST_USER)
                m->Flags |= CRF_SYSTEM;
        }
        if (c->MftMap)
            chk_bmp_set(c->MftMap, recno);

        off = h->AttributeOffset;
        while (off + 8 <= c->MftRecordSize)
        {
            ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
            if (a->Type == AT_END)
                break;
            if (a->Length < sizeof(ATTR_RECORD) - sizeof(((ATTR_RECORD *)0)->NR) ||
                off + a->Length > h->BytesInUse ||
                off + a->Length > c->MftRecordSize)
            {
                chk_add_issue(res, CHK_ERR_ATTR_OVERRUN, recno, off, 0);
                if (m)
                    m->Flags |= CRF_CORRUPT;
                break;
            }

            if (a->Type == AT_STANDARD_INFORMATION)
                hasStdInfo = 1;

            if (a->Type == AT_ATTRIBUTE_LIST && m)
                m->Flags |= CRF_HAS_ATTRLIST;

            if (a->Type == AT_FILE_NAME && !a->NonResident && m)
            {
                FILE_NAME_ATTR *fn =
                    (FILE_NAME_ATTR *)(rec + off + a->Resident.ValueOffset);
                if (a->Resident.ValueOffset + 0x42 <= a->Length)
                {
                    m->FnCount++;
                    /* remember the first non-DOS name's parent */
                    if (m->ParentRef == 0 || fn->FileNameType != FILE_NAME_DOS)
                        if (m->ParentRef == 0)
                            m->ParentRef = fn->ParentDirectory;
                }
            }

            if (a->NonResident)
                chk_account_attr(c, rec, off, computed, res, recno);

            /* $Volume dirty flag (record 3, resident $VOLUME_INFORMATION) */
            if (recno == FILE_Volume && a->Type == AT_VOLUME_INFORMATION &&
                !a->NonResident)
            {
                VOLUME_INFORMATION *vi =
                    (VOLUME_INFORMATION *)(rec + off + a->Resident.ValueOffset);
                if (a->Resident.ValueOffset + VOLUME_INFORMATION_SIZE <= a->Length &&
                    (vi->Flags & VOLUME_IS_DIRTY))
                    res->WasDirty = 1;
            }

            off += a->Length;
        }

        /* Extension records hold attribute extents only: no $SI expected. */
        if (!hasStdInfo && !isExtension)
        {
            chk_add_issue(res, CHK_ERR_ATTR_NO_STDINFO, recno, 0, 0);
            if (m)
                m->Flags |= CRF_CORRUPT;
        }

        if (opt->Verbose)
            CHK_TRACE("rec %lu: inuse dir=%d ext=%d stdinfo=%d\n",
                      (unsigned long)recno, (h->Flags & FRH_DIRECTORY) ? 1 : 0,
                      isExtension, hasStdInfo);
    }

    /* Pass 1.5: validate attribute lists + find lost extension records. */
    if (c->Rec)
    {
        for (recno = 0; recno < c->RecordCount; recno++)
        {
            if ((c->Rec[recno].Flags & (CRF_IN_USE | CRF_HAS_ATTRLIST | CRF_EXTENSION))
                    == (CRF_IN_USE | CRF_HAS_ATTRLIST))
            {
                if (chk_read_record(c, recno, rec) == 0 &&
                    *(ULONG *)rec == NRH_FILE_TYPE &&
                    chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) == 0)
                    chk_check_attrlist(c, rec, recno, res);
            }
        }
        for (recno = 0; recno < c->RecordCount; recno++)
        {
            USHORT f = c->Rec[recno].Flags;
            if ((f & (CRF_IN_USE | CRF_EXTENSION)) == (CRF_IN_USE | CRF_EXTENSION) &&
                !(f & CRF_CLAIMED))
                chk_add_issue(res, CHK_ERR_LOST_EXTENSION, recno, 0, 0);
        }
    }

    free(rec);
    return 0;
}

/* ============================================================
 * Pass 2: $I30 index walk
 * ============================================================ */

#define CHK_WALK_MAX_DEPTH 16
#define CHK_MRU_SLOTS      4

typedef struct _CHK_WALK {
    CHK_CTX         *c;
    NTFS_CHK_RESULT *res;
    const NTFS_CHK_OPTIONS *opt;
    ULONG      dirRec;
    /* large-index state */
    CHK_RUN   *iaRuns;
    int        iaRunCount;
    ULONG      blockSize;      /* bytes per INDX block                     */
    ULONG      vcnPerBlock;    /* VCN units per block (H5)                 */
    ULONGLONG  totalBlocks;
    UCHAR     *iaBmp;          /* $BITMAP($I30) value                      */
    ULONGLONG  iaBmpBits;
    UCHAR     *visited;        /* cycle detection                          */
    /* in-order collation state */
    WCHAR      prevName[256];
    ULONG      prevLen;
    int        havePrev;
    int        bad;            /* structural damage seen                   */
    /* back-ref target MRU cache */
    ULONG      mruRec[CHK_MRU_SLOTS];
    UCHAR     *mruBuf[CHK_MRU_SLOTS];
    int        mruNext;
} CHK_WALK;

static UCHAR *chk_walk_get_record(CHK_WALK *w, ULONG recno)
{
    int i;
    for (i = 0; i < CHK_MRU_SLOTS; i++)
        if (w->mruBuf[i] && w->mruRec[i] == recno)
            return w->mruBuf[i];

    i = w->mruNext;
    w->mruNext = (w->mruNext + 1) % CHK_MRU_SLOTS;
    if (!w->mruBuf[i])
    {
        w->mruBuf[i] = (UCHAR *)malloc(w->c->MftRecordSize);
        if (!w->mruBuf[i])
            return NULL;
    }
    if (chk_read_record(w->c, recno, w->mruBuf[i]) != 0 ||
        *(ULONG *)w->mruBuf[i] != NRH_FILE_TYPE ||
        chk_apply_fixups(w->mruBuf[i], w->c->MftRecordSize,
                         w->c->BytesPerSector) != 0)
    {
        w->mruRec[i] = (ULONG)-1;
        return NULL;
    }
    w->mruRec[i] = recno;
    return w->mruBuf[i];
}

/* Does file-record buffer 'rec' hold a resident $FILE_NAME whose parent is
 * dirRec and whose name equals key? */
static int
chk_rec_has_name(CHK_CTX *c, UCHAR *rec, ULONG dirRec, FILE_NAME_ATTR *key)
{
    ULONG off = ((FILE_RECORD_HEADER *)rec)->AttributeOffset;
    while (off + 8 <= c->MftRecordSize)
    {
        ATTR_RECORD *a = (ATTR_RECORD *)(rec + off);
        if (a->Type == AT_END || a->Length == 0 || off + a->Length > c->MftRecordSize)
            break;
        if (a->Type == AT_FILE_NAME && !a->NonResident &&
            a->Resident.ValueOffset + 0x42 <= a->Length)
        {
            FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)(rec + off + a->Resident.ValueOffset);
            if (CHK_REF_REC(fn->ParentDirectory) == dirRec &&
                fn->FileNameLength == key->FileNameLength)
            {
                ULONG i;
                for (i = 0; i < fn->FileNameLength &&
                            fn->FileName[i] == key->FileName[i]; i++)
                    ;
                if (i == fn->FileNameLength)
                    return 1;
            }
        }
        off += a->Length;
    }
    return 0;
}

/* Search a target record plus any $FILE_NAME attributes it spilled to
 * $ATTRIBUTE_LIST extension records for a name matching 'key' in dirRec.  Hard
 * links beyond what fits in one MFT record live in child records, so a
 * back-reference check that only scanned the base would falsely flag them. */
static int
chk_target_has_name(CHK_CTX *c, ULONG refRec, UCHAR *target,
                    ULONG dirRec, FILE_NAME_ATTR *key)
{
    ULONG alOff;
    ULONGLONG size = 0, pos = 0;
    UCHAR *val;

    if (chk_rec_has_name(c, target, dirRec, key))
        return 1;

    alOff = chk_find_attr(c, target, AT_ATTRIBUTE_LIST, NULL, 0);
    if (!alOff)
        return 0;
    val = chk_read_attr_value(c, target, alOff, 1024 * 1024, &size);
    if (!val)
        return 0;

    while (pos + sizeof(ATTR_LIST_ENTRY) <= size)
    {
        ATTR_LIST_ENTRY *le = (ATTR_LIST_ENTRY *)(val + pos);
        ULONG extRec;

        if (le->Length < sizeof(ATTR_LIST_ENTRY) || pos + le->Length > size)
            break;
        extRec = CHK_REF_REC(le->MftReference);
        if (le->Type == AT_FILE_NAME && extRec != refRec && extRec < c->RecordCount)
        {
            UCHAR *ext = (UCHAR *)malloc(c->MftRecordSize);
            if (ext)
            {
                if (chk_read_record(c, extRec, ext) == 0 &&
                    *(ULONG *)ext == NRH_FILE_TYPE &&
                    chk_apply_fixups(ext, c->MftRecordSize, c->BytesPerSector) == 0 &&
                    chk_rec_has_name(c, ext, dirRec, key))
                {
                    free(ext);
                    free(val);
                    return 1;
                }
                free(ext);
            }
        }
        pos += le->Length;
    }

    free(val);
    return 0;
}

/* Verify one leaf entry: collation order + back-reference. */
static void chk_walk_visit_entry(CHK_WALK *w, INDEX_ENTRY *e)
{
    CHK_CTX *c = w->c;
    FILE_NAME_ATTR *key = (FILE_NAME_ATTR *)((UCHAR *)e + 0x10);
    ULONGLONG ref = e->Data.Directory.IndexedFile;
    ULONG refRec = CHK_REF_REC(ref);
    UCHAR *target;
    int backed = 0;

    /* key must fit in the entry, and the name must fit in the key:
     * fixed FILE_NAME part is 0x42 with one name WCHAR included. */
    if (e->KeyLength < 0x42 ||
        (ULONG)0x10 + e->KeyLength > e->Length ||
        (ULONG)(0x42 - sizeof(WCHAR)) +
            (ULONG)key->FileNameLength * sizeof(WCHAR) > e->KeyLength)
    {
        chk_add_issue(w->res, CHK_ERR_INDEX_STRUCT, w->dirRec, refRec, 0);
        w->bad = 1;
        return;
    }

    /* collation order (strictly ascending) */
    if (w->havePrev)
    {
        if (chk_collate_filename(c, w->prevName, w->prevLen,
                                 key->FileName, key->FileNameLength) >= 0)
        {
            chk_add_issue(w->res, CHK_ERR_INDEX_ORDER, w->dirRec, refRec, 0);
            w->bad = 1;
        }
    }
    if (key->FileNameLength <= 255)
    {
        memcpy(w->prevName, key->FileName,
               key->FileNameLength * sizeof(WCHAR));
        w->prevLen = key->FileNameLength;
        w->havePrev = 1;
    }

    /* back-reference */
    if (refRec >= c->RecordCount)
    {
        chk_add_issue(w->res, CHK_ERR_INDEX_BADREF, w->dirRec, refRec, 0);
        w->bad = 1;
        return;
    }
    if (!c->Rec ||
        !(c->Rec[refRec].Flags & CRF_IN_USE) ||
        (c->Rec[refRec].Flags & CRF_EXTENSION) ||
        c->Rec[refRec].SeqNumber != CHK_REF_SEQ(ref))
    {
        chk_add_issue(w->res, CHK_ERR_INDEX_BACKREF, w->dirRec, refRec, 0);
        w->bad = 1;
        return;
    }

    target = chk_walk_get_record(w, refRec);
    if (target && chk_target_has_name(c, refRec, target, w->dirRec, key))
        backed = 1;

    if (!backed)
    {
        chk_add_issue(w->res, CHK_ERR_INDEX_BACKREF, w->dirRec, refRec, 0);
        w->bad = 1;
        return;
    }

    c->Rec[refRec].LinksSeen++;
}

/* In-order recursive walk over one node's entry list. */
static void chk_walk_node(CHK_WALK *w, UCHAR *entries, ULONG length, int depth)
{
    ULONG eoff = 0;

    if (depth > CHK_WALK_MAX_DEPTH)
    {
        chk_add_issue(w->res, CHK_ERR_INDEX_CYCLE, w->dirRec, depth, 0);
        w->bad = 1;
        return;
    }

    while (eoff + sizeof(INDEX_ENTRY) <= length)
    {
        INDEX_ENTRY *e = (INDEX_ENTRY *)(entries + eoff);
        if (e->Length < sizeof(INDEX_ENTRY) || eoff + e->Length > length)
        {
            chk_add_issue(w->res, CHK_ERR_INDEX_STRUCT, w->dirRec, eoff, 0);
            w->bad = 1;
            return;
        }

        /* descend into the child subtree first (in-order) */
        if (e->Flags & INDEX_ENTRY_NODE)
        {
            ULONGLONG childVcn = *(ULONGLONG *)((UCHAR *)e + e->Length - 8);
            ULONGLONG blockIdx = w->vcnPerBlock ? childVcn / w->vcnPerBlock : 0;

            if (!w->iaRuns || blockIdx >= w->totalBlocks ||
                (w->vcnPerBlock && (childVcn % w->vcnPerBlock)))
            {
                chk_add_issue(w->res, CHK_ERR_INDEX_STRUCT, w->dirRec,
                              childVcn, 0);
                w->bad = 1;
            }
            else if (w->visited && chk_bmp_get(w->visited, blockIdx))
            {
                chk_add_issue(w->res, CHK_ERR_INDEX_CYCLE, w->dirRec,
                              blockIdx, 0);
                w->bad = 1;
            }
            else
            {
                UCHAR *block = (UCHAR *)malloc(w->blockSize);
                if (w->visited)
                    chk_bmp_set(w->visited, blockIdx);
                if (w->iaBmp && blockIdx < w->iaBmpBits &&
                    !chk_bmp_get(w->iaBmp, blockIdx))
                {
                    chk_add_issue(w->res, CHK_ERR_INDX_BAD_BLOCK, w->dirRec,
                                  blockIdx, 0);
                    w->bad = 1;
                }
                if (block)
                {
                    INDEX_BUFFER *ib = (INDEX_BUFFER *)block;
                    if (chk_stream_io(w->c->Io, w->iaRuns, w->iaRunCount,
                                      w->c->BytesPerCluster,
                                      blockIdx * (ULONGLONG)w->blockSize,
                                      block, w->blockSize, 0) != 0 ||
                        ib->Magic != NRH_INDX_TYPE ||
                        chk_apply_fixups(block, w->blockSize,
                                         w->c->BytesPerSector) != 0 ||
                        ib->Vcn != childVcn)
                    {
                        chk_add_issue(w->res, CHK_ERR_INDX_BAD_BLOCK,
                                      w->dirRec, blockIdx, 0);
                        w->bad = 1;
                    }
                    else
                    {
                        UCHAR *base = (UCHAR *)&ib->Header;
                        ULONG limit = ib->Header.IndexLength;
                        if (ib->Header.EntriesOffset < limit &&
                            (ULONG)(base - block) + limit <= w->blockSize)
                        {
                            chk_walk_node(w, base + ib->Header.EntriesOffset,
                                          limit - ib->Header.EntriesOffset,
                                          depth + 1);
                        }
                        else
                        {
                            chk_add_issue(w->res, CHK_ERR_INDX_BAD_BLOCK,
                                          w->dirRec, blockIdx, 0);
                            w->bad = 1;
                        }
                    }
                    free(block);
                }
            }
        }

        if (e->Flags & INDEX_ENTRY_END)
            break;

        chk_walk_visit_entry(w, e);
        eoff += e->Length;
    }
}

static void chk_walk_one_dir(CHK_CTX *c, ULONG recno, UCHAR *rec,
                             NTFS_CHK_RESULT *res, const NTFS_CHK_OPTIONS *opt)
{
    CHK_WALK w;
    ULONG irOff, iaOff, bmOff;
    ATTR_RECORD *ir;
    INDEX_ROOT *root;
    ULONGLONG iaSize = 0;
    UCHAR *base;
    int i;

    memset(&w, 0, sizeof(w));
    w.c = c;
    w.res = res;
    w.opt = opt;
    w.dirRec = recno;

    irOff = chk_find_attr(c, rec, AT_INDEX_ROOT, chk_I30, 4);
    if (!irOff)
    {
        chk_add_issue(res, CHK_ERR_INDEX_STRUCT, recno, 0, 0);
        c->Rec[recno].Flags |= CRF_INDEX_BAD;
        return;
    }
    ir = (ATTR_RECORD *)(rec + irOff);
    if (ir->NonResident ||
        ir->Resident.ValueLength < sizeof(INDEX_ROOT) ||
        irOff + ir->Resident.ValueOffset + ir->Resident.ValueLength >
            c->MftRecordSize)
    {
        chk_add_issue(res, CHK_ERR_INDEX_STRUCT, recno, 1, 0);
        c->Rec[recno].Flags |= CRF_INDEX_BAD;
        return;
    }
    root = (INDEX_ROOT *)(rec + irOff + ir->Resident.ValueOffset);
    if (root->AttributeType != AT_FILE_NAME || root->CollationRule != 1)
    {
        chk_add_issue(res, CHK_ERR_INDEX_STRUCT, recno, 2, 0);
        c->Rec[recno].Flags |= CRF_INDEX_BAD;
        return;
    }

    base = (UCHAR *)&root->Header;
    if (root->Header.EntriesOffset >= root->Header.IndexLength ||
        (ULONG)(base - (rec + irOff + ir->Resident.ValueOffset)) +
            root->Header.IndexLength > ir->Resident.ValueLength)
    {
        chk_add_issue(res, CHK_ERR_INDEX_STRUCT, recno, 3, 0);
        c->Rec[recno].Flags |= CRF_INDEX_BAD;
        return;
    }

    /* large index: bring up $INDEX_ALLOCATION + $BITMAP state */
    iaOff = chk_find_attr(c, rec, AT_INDEX_ALLOCATION, chk_I30, 4);
    if (iaOff)
    {
        ATTR_RECORD *ia = (ATTR_RECORD *)(rec + iaOff);
        if (ia->NonResident)
        {
            const UCHAR *mp = rec + iaOff + ia->NR.MappingPairsOffset;
            const UCHAR *end = rec + iaOff + ia->Length;
            w.iaRuns = (CHK_RUN *)malloc(sizeof(CHK_RUN) * CHK_MAX_RUNS);
            if (w.iaRuns)
            {
                w.iaRunCount = chk_decode_runs(mp, end, w.iaRuns, CHK_MAX_RUNS);
                if (w.iaRunCount <= 0)
                {
                    chk_add_issue(res, CHK_ERR_INDEX_STRUCT, recno, 4, 0);
                    c->Rec[recno].Flags |= CRF_INDEX_BAD;
                    free(w.iaRuns);
                    w.iaRuns = NULL;
                }
                iaSize = (ULONGLONG)ia->NR.DataSize;
            }
        }
    }

    w.blockSize = root->IndexBlockSize;
    if (w.blockSize < 512 || (w.blockSize & (w.blockSize - 1)))
        w.blockSize = c->IndexRecordSize;
    /* H5: VCN units are clusters, or 512-byte chunks when cluster > block */
    w.vcnPerBlock = chk_vcn_per_block(c->BytesPerCluster, w.blockSize);
    w.totalBlocks = w.blockSize ? iaSize / w.blockSize : 0;

    if (w.totalBlocks)
    {
        w.visited = (UCHAR *)calloc(1, (size_t)(w.totalBlocks + 7) / 8 + 8);
        bmOff = chk_find_attr(c, rec, AT_BITMAP, chk_I30, 4);
        if (bmOff)
        {
            ULONGLONG bs = 0;
            w.iaBmp = chk_read_attr_value(c, rec, bmOff,
                                          (w.totalBlocks + 7) / 8 + 8, &bs);
            w.iaBmpBits = bs * 8;
        }
    }

    chk_walk_node(&w, base + root->Header.EntriesOffset,
                  root->Header.IndexLength - root->Header.EntriesOffset, 0);

    if (w.bad)
        c->Rec[recno].Flags |= CRF_INDEX_BAD;

    free(w.iaRuns);
    free(w.iaBmp);
    free(w.visited);
    for (i = 0; i < CHK_MRU_SLOTS; i++)
        free(w.mruBuf[i]);
}

int chk_walk_indexes(CHK_CTX *c, NTFS_CHK_RESULT *res,
                     const NTFS_CHK_OPTIONS *opt)
{
    UCHAR *rec = (UCHAR *)malloc(c->MftRecordSize);
    ULONG recno;

    if (!rec || !c->Rec)
        { free(rec); return -1; }

    for (recno = 0; recno < c->RecordCount; recno++)
    {
        USHORT f = c->Rec[recno].Flags;
        if ((f & (CRF_IN_USE | CRF_DIRECTORY | CRF_EXTENSION | CRF_CORRUPT))
                != (CRF_IN_USE | CRF_DIRECTORY))
            continue;
        if (f & CRF_ATTRLIST_UNSUPPORTED)
            continue;   /* already reported; cannot walk from base alone */

        if (chk_read_record(c, recno, rec) != 0 ||
            *(ULONG *)rec != NRH_FILE_TYPE ||
            chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) != 0)
            continue;

        chk_walk_one_dir(c, recno, rec, res, opt);
    }

    free(rec);
    return 0;
}

/* ============================================================
 * Pass 3: connectivity (orphans, cycles, link counts)
 * ============================================================ */

/* Is `dir` a live, walkable directory whose index we trust?
 * (Non-static: shared with chkrepair.c's link-count repair.) */
int chk_parent_trusted(CHK_CTX *c, ULONG dir)
{
    USHORT f;
    if (dir >= c->RecordCount)
        return 0;
    f = c->Rec[dir].Flags;
    return (f & (CRF_IN_USE | CRF_DIRECTORY)) == (CRF_IN_USE | CRF_DIRECTORY) &&
           !(f & (CRF_INDEX_BAD | CRF_ATTRLIST_UNSUPPORTED | CRF_CORRUPT));
}

static void chk_reach(CHK_CTX *c, ULONG start, NTFS_CHK_RESULT *res)
{
    ULONG cur = start;
    ULONG hops = 0;
    int success = 0;

    while (1)
    {
        CHK_REC *m = &c->Rec[cur];
        ULONG parent;

        if (m->Flags & CRF_REACHABLE)
        {
            success = 1;                 /* joins a known-good chain */
            break;
        }
        if (m->Flags & CRF_VISITING)
        {
            chk_add_issue(res, CHK_ERR_DIR_CYCLE, cur, start, 0);
            break;
        }
        if (cur == FILE_Root)
        {
            m->Flags |= CRF_REACHABLE;
            success = 1;
            break;
        }

        parent = CHK_REF_REC(m->ParentRef);
        if (parent >= c->RecordCount ||
            !(c->Rec[parent].Flags & CRF_IN_USE) ||
            !(c->Rec[parent].Flags & CRF_DIRECTORY) ||
            c->Rec[parent].SeqNumber != CHK_REF_SEQ(m->ParentRef))
            break;                       /* dead parent: unreachable  */

        m->Flags |= CRF_VISITING;
        cur = parent;
        if (++hops > c->RecordCount)
            break;                       /* paranoia bound            */
    }

    /* Unwind: every node we advanced past carries CRF_VISITING and they
     * form a finite parent-linked path from start; clear the stamps (and
     * mark REACHABLE on success).  Nodes where phase 1 stopped without
     * advancing (root / already-reachable join) got their flags there. */
    cur = start;
    while (cur < c->RecordCount && (c->Rec[cur].Flags & CRF_VISITING))
    {
        c->Rec[cur].Flags &= (USHORT)~CRF_VISITING;
        if (success)
            c->Rec[cur].Flags |= CRF_REACHABLE;
        cur = CHK_REF_REC(c->Rec[cur].ParentRef);
    }
}

int chk_connectivity(CHK_CTX *c, NTFS_CHK_RESULT *res,
                     const NTFS_CHK_OPTIONS *opt)
{
    ULONG recno;

    (void)opt;
    if (!c->Rec)
        return -1;

    for (recno = 0; recno < c->RecordCount; recno++)
    {
        USHORT f = c->Rec[recno].Flags;
        if ((f & (CRF_IN_USE | CRF_EXTENSION | CRF_CORRUPT)) != CRF_IN_USE)
            continue;
        chk_reach(c, recno, res);
    }

    for (recno = 0; recno < c->RecordCount; recno++)
    {
        CHK_REC *m = &c->Rec[recno];
        USHORT f = m->Flags;
        ULONG parent;

        if ((f & (CRF_IN_USE | CRF_EXTENSION | CRF_CORRUPT)) != CRF_IN_USE)
            continue;

        parent = CHK_REF_REC(m->ParentRef);

        /* reserved system records without names are not orphans */
        if ((f & CRF_SYSTEM) && m->FnCount == 0)
            continue;

        /* Orphan: no index entry refers to it, or its parent is dead.
         * If the parent's index is itself damaged, the R2 rebuild will
         * re-home this record; don't double-report it as an orphan. */
        if (!(f & CRF_SYSTEM))
        {
            int parentTrusted = chk_parent_trusted(c, parent);
            if (m->FnCount == 0 ||
                (parentTrusted && m->LinksSeen == 0) ||
                (!parentTrusted && parent < c->RecordCount &&
                 !(c->Rec[parent].Flags & CRF_IN_USE)) ||
                parent >= c->RecordCount)
            {
                m->Flags |= CRF_ORPHAN;
                chk_add_issue(res, CHK_ERR_ORPHAN, recno, parent, 0);
                continue;
            }
        }

        /* Link count: only meaningful when every referencing index was
         * walkable; v1 uses the (single) parent's health as the proxy.
         * Root is its own parent: its "." entry lives in its own index. */
        if (chk_parent_trusted(c, recno == FILE_Root ? FILE_Root : parent))
        {
            if (m->LinkCountHdr != m->LinksSeen)
                chk_add_issue(res, CHK_ERR_LINKCOUNT, recno,
                              ((ULONGLONG)m->LinkCountHdr << 32) | m->LinksSeen,
                              0);
        }
    }

    return 0;
}

/* ============================================================
 * Pass 4: $MFT:$BITMAP cross-check
 * ============================================================ */

int chk_check_mft_bitmap(CHK_CTX *c, NTFS_CHK_RESULT *res,
                         const NTFS_CHK_OPTIONS *opt)
{
    UCHAR *rec, *val = NULL;
    ULONG off;
    ULONGLONG size = 0, bit, nbits;
    ULONGLONG mismatchStart = (ULONGLONG)-1;

    (void)opt;
    if (!c->MftMap)
        return -1;

    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return -1;
    if (chk_read_record(c, FILE_MFT, rec) == 0 &&
        *(ULONG *)rec == NRH_FILE_TYPE &&
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) == 0 &&
        (off = chk_find_attr(c, rec, AT_BITMAP, NULL, 0)) != 0)
    {
        val = chk_read_attr_value(c, rec, off, 16 * 1024 * 1024, &size);
    }
    free(rec);
    if (!val)
        return -1;

    nbits = size * 8;
    for (bit = 0; bit < nbits; bit++)
    {
        int disk = chk_bmp_get(val, bit);
        int want = (bit < c->RecordCount) ? chk_bmp_get(c->MftMap, bit) : 0;

        if (disk != want)
        {
            if (mismatchStart == (ULONGLONG)-1)
                mismatchStart = bit;
        }
        else if (mismatchStart != (ULONGLONG)-1)
        {
            chk_add_issue(res, CHK_ERR_MFTBMP_MISMATCH, mismatchStart,
                          bit - mismatchStart, 0);
            mismatchStart = (ULONGLONG)-1;
        }
    }
    if (mismatchStart != (ULONGLONG)-1)
        chk_add_issue(res, CHK_ERR_MFTBMP_MISMATCH, mismatchStart,
                      nbits - mismatchStart, 0);

    free(val);
    return 0;
}

/* ============================================================
 * Pass 5: $Bitmap cross-check (both directions)
 * ============================================================ */

/* Mark the non-sparse $DATA runs of a system file (by record number) into
 * `protectedMap` -- clusters R6 must never free even if unreferenced. */
static void chk_mark_system_runs(CHK_CTX *c, ULONG recno, UCHAR *protectedMap)
{
    UCHAR *rec = (UCHAR *)malloc(c->MftRecordSize);
    CHK_RUN *runs = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    int n, i;

    if (!rec || !runs)
    {
        free(rec);
        free(runs);
        return;
    }
    if (chk_read_record(c, recno, rec) == 0 &&
        *(ULONG *)rec == NRH_FILE_TYPE &&
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) == 0)
    {
        n = chk_find_data_runs(c, rec, runs, CHK_MAX_RUNS, NULL);
        for (i = 0; i < n; i++)
        {
            ULONGLONG cl;
            if (runs[i].Sparse)
                continue;
            for (cl = 0; cl < runs[i].Len; cl++)
            {
                ULONGLONG lcn = runs[i].Lcn + cl;
                if (lcn < c->TotalClusters)
                    chk_bmp_set(protectedMap, lcn);
            }
        }
    }
    free(runs);
    free(rec);
}

int chk_crosscheck_bitmap(CHK_CTX *c, UCHAR *computed,
                          NTFS_CHK_RESULT *res, const NTFS_CHK_OPTIONS *opt)
{
    UCHAR *rec6, *diskBmp, *protectedMap;
    CHK_RUN *bmpRuns;   /* 192 KiB: too large for the stack */
    ULONGLONG bmpBytesNeeded = (c->TotalClusters + 7) / 8;
    ULONGLONG bmpDataSize = 0;
    ULONGLONG cl;
    ULONGLONG underStart = (ULONGLONG)-1, overStart = (ULONGLONG)-1;
    int nruns, dirty = 0, rc = 0;

    bmpRuns = (CHK_RUN *)malloc(CHK_MAX_RUNS * sizeof(CHK_RUN));
    if (!bmpRuns)
        return -1;
    rec6 = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec6)
        { free(bmpRuns); return -1; }
    if (chk_read_record(c, FILE_Bitmap, rec6) != 0 ||
        *(ULONG *)rec6 != NRH_FILE_TYPE ||
        chk_apply_fixups(rec6, c->MftRecordSize, c->BytesPerSector) != 0)
        { free(rec6); free(bmpRuns); return -1; }

    nruns = chk_find_data_runs(c, rec6, bmpRuns, CHK_MAX_RUNS, &bmpDataSize);
    free(rec6);
    if (nruns <= 0)
        { free(bmpRuns); return -1; }

    diskBmp = (UCHAR *)calloc(1, (size_t)bmpBytesNeeded + 8);
    if (!diskBmp)
        { free(bmpRuns); return -1; }

    {
        ULONGLONG toRead = CHK_MIN(bmpBytesNeeded, bmpDataSize);
        if (chk_stream_io(c->Io, bmpRuns, (ULONG)nruns, c->BytesPerCluster,
                          0, diskBmp, toRead, 0) != 0)
            { free(diskBmp); free(bmpRuns); return -1; }
    }

    /* R6 carve-outs (H7): clusters we must never free even when the computed
     * map shows them unreferenced -- the boot region (LCN 0 + $Boot's runs)
     * and $BadClus:$Bad's real (non-sparse) runs.  ClusterMap already accounts
     * every referenced cluster (incl. system files), so "computed-free" is
     * genuinely unreferenced; these two are the only extra exclusions. */
    protectedMap = (UCHAR *)calloc(1, (size_t)bmpBytesNeeded + 8);
    if (protectedMap)
    {
        chk_bmp_set(protectedMap, 0);            /* LCN 0: boot sector */
        chk_mark_system_runs(c, FILE_Boot, protectedMap);
        chk_mark_system_runs(c, FILE_BadClus, protectedMap);
    }

    for (cl = 0; cl <= c->TotalClusters; cl++)
    {
        int cb = (cl < c->TotalClusters) ? chk_bmp_get(computed, cl) : -1;
        int db = (cl < c->TotalClusters) ? chk_bmp_get(diskBmp, cl) : -1;
        int prot = (cl < c->TotalClusters && protectedMap)
                       ? chk_bmp_get(protectedMap, cl) : 1;
        int isOver = (cb == 0 && db == 1 && !prot);

        /* close open ranges (also fired once past the end via cb=db=-1) */
        if (underStart != (ULONGLONG)-1 && !(cb == 1 && db == 0))
        {
            chk_add_issue(res, CHK_ERR_BITMAP_UNDERALLOC, underStart,
                          cl - underStart, opt->FixErrors ? 1 : 0);
            underStart = (ULONGLONG)-1;
        }
        if (overStart != (ULONGLONG)-1 && !isOver)
        {
            chk_add_issue(res, CHK_ERR_BITMAP_OVERALLOC, overStart,
                          cl - overStart, opt->FixErrors ? 1 : 0);
            overStart = (ULONGLONG)-1;
        }
        if (cl == c->TotalClusters)
            break;

        if (cb == 1 && db == 0)
        {
            /* in use but marked free -- dangerous; safe to fix (set bit) */
            if (opt->FixErrors)
            {
                chk_bmp_set(diskBmp, cl);
                dirty = 1;
            }
            if (underStart == (ULONGLONG)-1)
                underStart = cl;
        }
        else if (isOver)
        {
            /* marked used but unreferenced and not carved out: free it. */
            res->ClustersReserved++;
            if (opt->FixErrors)
            {
                chk_bmp_clear(diskBmp, cl);
                dirty = 1;
            }
            if (overStart == (ULONGLONG)-1)
                overStart = cl;
        }
    }

    /* H7: the trailing pad bits past TotalClusters within the last on-disk
     * bitmap byte must always read as used (1). */
    if (opt->FixErrors && (c->TotalClusters & 7))
    {
        UCHAR pad = (UCHAR)(0xFFu << (c->TotalClusters & 7));
        ULONGLONG lastByte = c->TotalClusters / 8;
        if (lastByte < bmpBytesNeeded && (diskBmp[lastByte] & pad) != pad)
        {
            diskBmp[lastByte] |= pad;
            dirty = 1;
        }
    }

    if (dirty)
    {
        ULONGLONG toWrite = CHK_MIN(bmpBytesNeeded, bmpDataSize);
        if (chk_stream_io(c->Io, bmpRuns, (ULONG)nruns, c->BytesPerCluster,
                          0, diskBmp, toWrite, 1) != 0)
            rc = -1;
    }

    free(protectedMap);
    free(diskBmp);
    free(bmpRuns);
    return rc;
}

/* ============================================================
 * Pass 6: $MFTMirr compare
 * ============================================================ */

int chk_check_mftmirr(CHK_CTX *c, NTFS_CHK_RESULT *res,
                      const NTFS_CHK_OPTIONS *opt)
{
    UCHAR *rec, *mirror = NULL, *mft = NULL;
    CHK_RUN runs[64];
    ULONGLONG size = 0;
    ULONG nrec, i;
    int n = 0;

    (void)opt;
    rec = (UCHAR *)malloc(c->MftRecordSize);
    if (!rec)
        return -1;
    if (chk_read_record(c, FILE_MFTMirr, rec) == 0 &&
        *(ULONG *)rec == NRH_FILE_TYPE &&
        chk_apply_fixups(rec, c->MftRecordSize, c->BytesPerSector) == 0)
    {
        n = chk_find_data_runs(c, rec, runs, 64, &size);
    }
    free(rec);
    if (n <= 0 || size == 0)
        return -1;

    nrec = (ULONG)(size / c->MftRecordSize);
    if (nrec > c->RecordCount)
        nrec = c->RecordCount;
    if (nrec == 0)
        return 0;

    mirror = (UCHAR *)malloc((size_t)nrec * c->MftRecordSize);
    mft = (UCHAR *)malloc((size_t)nrec * c->MftRecordSize);
    if (!mirror || !mft)
        { free(mirror); free(mft); return -1; }

    if (chk_stream_io(c->Io, runs, (ULONG)n, c->BytesPerCluster,
                      0, mirror, (ULONGLONG)nrec * c->MftRecordSize, 0) != 0 ||
        chk_stream_io(c->Io, c->MftRuns, c->MftRunCount, c->BytesPerCluster,
                      0, mft, (ULONGLONG)nrec * c->MftRecordSize, 0) != 0)
        { free(mirror); free(mft); return -1; }

    for (i = 0; i < nrec; i++)
    {
        const UCHAR *a = mft + (size_t)i * c->MftRecordSize;
        const UCHAR *b = mirror + (size_t)i * c->MftRecordSize;
        ULONG k;
        for (k = 0; k < c->MftRecordSize; k++)
            if (a[k] != b[k])
                break;
        if (k != c->MftRecordSize)
            chk_add_issue(res, CHK_ERR_MFTMIRR_MISMATCH, i, k, 0);
    }

    free(mirror);
    free(mft);
    return 0;
}
