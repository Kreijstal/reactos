/*
 * ntfslib - NTFS consistency checker / repair (chkdsk) internals
 * License: GPL-2.0-or-later
 *
 * Shared between chkdsk.c (orchestrator + on-disk substrate), chkscan.c
 * (detection passes) and chkrepair.c (repair stages).  Everything here
 * depends only on ntfs_types.h + the MKNTFS_IO abstraction, so all three
 * compilation units build both inside ReactOS (NTOS_MODE_USER) and
 * standalone in the test harness.
 */
#ifndef NTFS_CHKINT_H
#define NTFS_CHKINT_H

#if defined(NTOS_MODE_USER)
#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#define malloc(s)       RtlAllocateHeap(RtlGetProcessHeap(), 0, (s))
#define calloc(n, s)    RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)(n) * (s))
#define free(p)         do { if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, (p)); } while (0)
#define memset(d, v, n) RtlFillMemory((d), (n), (v))
#define memcpy(d, s, n) RtlCopyMemory((d), (s), (n))
#define CHK_TRACE(...)  do { } while (0)
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int chk_verbose;
#define CHK_TRACE(...)  do { if (chk_verbose) fprintf(stderr, __VA_ARGS__); } while (0)
#endif

#include "chkdsk.h"
#include "ntfspriv.h"

/* ============================================================
 * Limits / small helpers
 * ============================================================ */

#define CHK_MIN(a, b)   ((a) < (b) ? (a) : (b))
#define CHK_MAX_RUNS    8192
#define CHK_MAX_XLINKS  64

typedef struct _CHK_RUN {
    ULONGLONG Lcn;
    ULONGLONG Len;   /* in clusters */
    int       Sparse;
} CHK_RUN;

/* ============================================================
 * Per-record model row (populated by the pass-1 scan)
 * ============================================================ */

/* CHK_REC.Flags */
#define CRF_IN_USE               0x0001
#define CRF_DIRECTORY            0x0002
#define CRF_EXTENSION            0x0004   /* BaseFileRecord != 0                   */
#define CRF_HAS_ATTRLIST         0x0008
#define CRF_CORRUPT              0x0010   /* magic/fixup/attr chain unrecoverable  */
#define CRF_INDEX_BAD            0x0020   /* directory whose $I30 needs rebuild    */
#define CRF_REACHABLE            0x0040   /* connected to root via parent chain    */
#define CRF_ORPHAN               0x0080   /* in-use base record with no index home */
#define CRF_SYSTEM               0x0100   /* recno < FILE_FIRST_USER               */
#define CRF_ATTRLIST_UNSUPPORTED 0x0200   /* $I30 spread over extents: no rebuild  */
#define CRF_CLAIMED              0x0400   /* extension claimed by a base's list    */
#define CRF_VISITING             0x0800   /* connectivity walk: on current chain   */

typedef struct _CHK_REC {
    ULONGLONG ParentRef;      /* first non-DOS $FILE_NAME's ParentDirectory */
    USHORT    SeqNumber;      /* header SequenceNumber                      */
    USHORT    LinkCountHdr;   /* header LinkCount                           */
    USHORT    FnCount;        /* $FILE_NAME attributes found                */
    USHORT    LinksSeen;      /* index entries observed referencing this    */
    USHORT    Flags;          /* CRF_*                                      */
    USHORT    Reserved;
} CHK_REC;

/* Cross-link ledger entry: cluster claimed by two different attributes. */
typedef struct _CHK_XLINK {
    ULONGLONG Lcn;
    ULONG     FirstRec;
    ULONG     SecondRec;
} CHK_XLINK;

/* ============================================================
 * Checker context
 * ============================================================ */

typedef struct _CHK_CTX {
    const MKNTFS_IO  *Io;
    ULONG      BytesPerSector;
    ULONG      SectorsPerCluster;
    ULONG      BytesPerCluster;
    ULONG      MftRecordSize;
    ULONG      IndexRecordSize;
    ULONGLONG  TotalClusters;
    ULONGLONG  MftLcn;

    CHK_RUN    MftRuns[CHK_MAX_RUNS];
    ULONG      MftRunCount;
    ULONGLONG  MftDataBytes;   /* $MFT:$DATA DataSize */
    ULONG      RecordCount;

    /* ---- model (allocated by the scan passes; may be NULL on OOM) ---- */
    CHK_REC   *Rec;            /* RecordCount rows                          */
    UCHAR     *ClusterMap;     /* computed cluster-in-use bitmap            */
    UCHAR     *MftMap;         /* computed record-in-use bitmap             */
    WCHAR     *UpCase;         /* 65536-entry upcase table ($UpCase or builtin) */
    CHK_XLINK  XLinks[CHK_MAX_XLINKS];
    ULONG      XLinkCount;     /* total cross-linked clusters (may exceed array) */
} CHK_CTX;

/* ---- bitmap bit accessors on a byte buffer ---- */
static __inline int  chk_bmp_get(const UCHAR *b, ULONGLONG bit)
{
    return (b[bit >> 3] >> (bit & 7)) & 1;
}
static __inline void chk_bmp_set(UCHAR *b, ULONGLONG bit)
{
    b[bit >> 3] |= (UCHAR)(1u << (bit & 7));
}
static __inline void chk_bmp_clear(UCHAR *b, ULONGLONG bit)
{
    b[bit >> 3] &= (UCHAR)~(1u << (bit & 7));
}

/* ============================================================
 * Substrate (chkdsk.c)
 * ============================================================ */

int  chk_read(const MKNTFS_IO *io, ULONGLONG off, void *buf, ULONG len);
int  chk_write(const MKNTFS_IO *io, ULONGLONG off, const void *buf, ULONG len);
void chk_add_issue(NTFS_CHK_RESULT *res, NTFS_CHK_CODE code,
                   ULONGLONG p0, ULONGLONG p1, int fixed);

/* Route one pre-built, NUL-terminated line to the user-facing message sink
 * (opt->Message).  No-op when no sink is installed.  Not for hot paths. */
void chk_emit(const NTFS_CHK_OPTIONS *opt, const char *msg);
int  chk_apply_fixups(UCHAR *buf, ULONG size, ULONG sectorSize);
void chk_stamp_fixups(UCHAR *buf, ULONG size, ULONG sectorSize);
int  chk_decode_runs(const UCHAR *p, const UCHAR *end,
                     CHK_RUN *runs, ULONG maxRuns);
int  chk_stream_io(const MKNTFS_IO *io, const CHK_RUN *runs, ULONG nruns,
                   ULONG clusterSize, ULONGLONG streamOff,
                   UCHAR *buf, ULONGLONG len, int write);
int  chk_read_boot(CHK_CTX *c, NTFS_CHK_RESULT *res);
int  chk_find_data_runs(CHK_CTX *c, UCHAR *rec,
                        CHK_RUN *runs, ULONG maxRuns, ULONGLONG *dataSize);
int  chk_load_mft(CHK_CTX *c, NTFS_CHK_RESULT *res);
int  chk_read_record(CHK_CTX *c, ULONG recno, UCHAR *buf);
int  chk_write_record(CHK_CTX *c, ULONG recno, UCHAR *buf);
int  chk_load_upcase(CHK_CTX *c, NTFS_CHK_RESULT *res);

/* Locate an attribute inside a (fixup-applied) record buffer.  name may be
 * NULL for "unnamed only"; returns the offset or 0 if not found. */
ULONG chk_find_attr(CHK_CTX *c, UCHAR *rec, ULONG type,
                    const WCHAR *name, ULONG nameLen);

/* Read a whole attribute stream (resident or non-resident) into a fresh
 * buffer; caller frees.  Caps at maxBytes.  Returns NULL on failure. */
UCHAR *chk_read_attr_value(CHK_CTX *c, UCHAR *rec, ULONG attrOff,
                           ULONGLONG maxBytes, ULONGLONG *sizeOut);

/* COLLATION_FILENAME comparison using c->UpCase (may be NULL: raw compare).
 * <0, 0, >0 like memcmp. */
int chk_collate_filename(const CHK_CTX *c,
                         const WCHAR *a, ULONG alen,
                         const WCHAR *b, ULONG blen);

/* ============================================================
 * Detection passes (chkscan.c)
 * ============================================================ */

int  chk_scan_records(CHK_CTX *c, UCHAR *computed, NTFS_CHK_RESULT *res,
                      const NTFS_CHK_OPTIONS *opt);
int  chk_walk_indexes(CHK_CTX *c, NTFS_CHK_RESULT *res,
                      const NTFS_CHK_OPTIONS *opt);
int  chk_connectivity(CHK_CTX *c, NTFS_CHK_RESULT *res,
                      const NTFS_CHK_OPTIONS *opt);
int  chk_check_mft_bitmap(CHK_CTX *c, NTFS_CHK_RESULT *res,
                          const NTFS_CHK_OPTIONS *opt);
int  chk_crosscheck_bitmap(CHK_CTX *c, UCHAR *computed,
                           NTFS_CHK_RESULT *res, const NTFS_CHK_OPTIONS *opt);
int  chk_check_mftmirr(CHK_CTX *c, NTFS_CHK_RESULT *res,
                       const NTFS_CHK_OPTIONS *opt);

/* ============================================================
 * Repair stages (chkrepair.c)
 * ============================================================ */

int  chk_set_volume_flag(CHK_CTX *c, int setDirty, int markChkdsk);
int  chk_sync_mftmirr_record(CHK_CTX *c, ULONG recno);

/* R8 (H8): reset $LogFile to the empty, dismounted-cleanly state so a stale
 * journal is not replayed over just-repaired metadata.  Writes only the two
 * CLEAN 'RSTR' restart pages (the mkntfs "empty log" landmark); does NOT
 * synthesize redo/undo records.  Called from the R8 disarm phase only when a
 * repair was actually made.  Returns 0 on success, -1 if it could not stamp. */
int  chk_stamp_logfile_clean(CHK_CTX *c, NTFS_CHK_RESULT *res);

/* Is `dir` a live, walkable directory whose index we trust?  Shared with
 * chkscan.c pass 3; used by the link-count repair to gate on a trusted
 * parent so LinksSeen is meaningful. */
int  chk_parent_trusted(CHK_CTX *c, ULONG dir);

/* S4 "easy repairs" (all gated by opt->FixErrors in the orchestrator). */
int  chk_repair_attributes(CHK_CTX *c, NTFS_CHK_RESULT *res);
int  chk_repair_crosslinks(CHK_CTX *c, NTFS_CHK_RESULT *res);
int  chk_repair_linkcounts(CHK_CTX *c, NTFS_CHK_RESULT *res);
int  chk_repair_mft_bitmap(CHK_CTX *c, NTFS_CHK_RESULT *res);

/* Shared VCN-unit rule (H5): VCN counts clusters when a cluster is no larger
 * than an index block, else 512-byte units.  One authority for chkscan.c's
 * walker and chkrepair.c's rebuilder -- never duplicate the rule. */
static __inline ULONG chk_vcn_per_block(ULONG bytesPerCluster, ULONG blockSize)
{
    if (bytesPerCluster <= blockSize)
        return blockSize / bytesPerCluster;
    return blockSize / 512;
}

/* S5: from-scratch bottom-up $I30 rebuild for one directory (opt->FixErrors
 * gated by the orchestrator).  Reconstructs the entry set from every child's
 * intact $FILE_NAME ParentDirectory back-ref, sorts by COLLATION_FILENAME, and
 * writes either a resident INDEX_ROOT (small) or INDEX_ROOT + $INDEX_ALLOCATION
 * + $BITMAP (large).  Returns 0 on success (whether or not it rebuilt), -1 on a
 * hard failure. */
int  chk_rebuild_index(CHK_CTX *c, ULONG dirRec, NTFS_CHK_RESULT *res);

/* S6 (R3): orphan recovery.  Collects every in-use, non-system base record
 * flagged CRF_ORPHAN, allocates a found.NNN directory (a free MFT record >=16),
 * re-homes each orphan's first $FILE_NAME.ParentDirectory to found.NNN (or
 * fabricates a FILEnnnn.CHK name if the orphan has none), then rebuilds
 * found.NNN's $I30 and the root $I30 (via chk_rebuild_index) so found.NNN and
 * its recovered children are reachable.  If the MFT is full ($MFT extension is
 * deferred), leaves the excess orphans in place and reports them.  Returns 0 on
 * success (whether or not it recovered any), -1 on a hard failure. */
int  chk_recover_orphans(CHK_CTX *c, NTFS_CHK_RESULT *res);

#endif /* NTFS_CHKINT_H */
