/*
 * ntfslib - NTFS consistency checker / repair (chkdsk)
 * Public API header
 * License: GPL-2.0-or-later
 *
 * The core checker NtfsChkVolume() depends only on ntfs_types.h and the
 * MKNTFS_IO read/write-by-offset abstraction, so the same code compiles both
 * inside ReactOS (NTOS_MODE_USER, wired by NtfsChkdsk in ntfslib.c) and
 * standalone on the host (the userspace test harness).
 */
#ifndef NTFS_CHKDSK_H
#define NTFS_CHKDSK_H

#include "mkntfs.h"   /* pulls ntfs_types.h + MKNTFS_IO */

/* Issue codes reported by the checker. */
typedef enum _NTFS_CHK_CODE {
    CHK_OK = 0,
    CHK_ERR_BOOT_SIG,         /* boot sector end marker != 0xAA55            */
    CHK_ERR_BOOT_OEM,         /* OemId != "NTFS    "                         */
    CHK_ERR_BOOT_GEOMETRY,    /* impossible sector/cluster/record sizes      */
    CHK_ERR_MFT_UNREADABLE,   /* could not read/parse $MFT record 0          */
    CHK_ERR_REC_MAGIC,        /* in-use record without 'FILE' magic          */
    CHK_ERR_USA_MISMATCH,     /* update-sequence-array fixup mismatch        */
    CHK_ERR_ATTR_OVERRUN,     /* attribute runs past BytesInUse              */
    CHK_ERR_ATTR_NO_STDINFO,  /* in-use record missing $STANDARD_INFORMATION */
    CHK_ERR_RUN_OOB,          /* data run references cluster past volume end  */
    CHK_ERR_BITMAP_UNDERALLOC,/* cluster used by an attr but $Bitmap says free*/
    CHK_ERR_VOLUME_DIRTY,     /* $Volume has the dirty flag set              */
    CHK_ERR_INDEX_BADREF,     /* $I30 entry references an impossible MFT rec  */
    CHK_ERR_ATTRLIST_BROKEN,  /* $ATTRIBUTE_LIST entry invalid / ext mismatch */
    CHK_ERR_LOST_EXTENSION,   /* extension record not claimed by any list     */
    CHK_ERR_INDEX_ORDER,      /* $I30 entries out of collation order          */
    CHK_ERR_INDEX_BACKREF,    /* $I30 entry not backed by target's $FILE_NAME */
    CHK_ERR_INDEX_CYCLE,      /* $I30 b-tree child VCN visited twice          */
    CHK_ERR_INDEX_STRUCT,     /* $I30 INDEX_ROOT/entry structure invalid      */
    CHK_ERR_INDX_BAD_BLOCK,   /* INDX block magic/fixup/VCN invalid           */
    CHK_ERR_DIR_CYCLE,        /* directory parent chain loops                 */
    CHK_ERR_ORPHAN,           /* in-use record reachable from no index        */
    CHK_ERR_LINKCOUNT,        /* header LinkCount != index entries observed   */
    CHK_ERR_XLINK,            /* cluster claimed by two attributes            */
    CHK_ERR_MFTBMP_MISMATCH,  /* $MFT:$BITMAP disagrees with in-use records   */
    CHK_ERR_BITMAP_OVERALLOC, /* $Bitmap marks used, nothing references       */
    CHK_ERR_MFTMIRR_MISMATCH, /* $MFTMirr differs from the first MFT records  */
    CHK_ERR_UPCASE_BAD,       /* $UpCase unreadable or wrong size             */
    CHK_ERR_ATTR_TRUNCATED,   /* corrupt attribute truncated/deleted (repair) */
    CHK_ERR_NOMEM,            /* model allocation failed: detect-only run     */
    CHK_CODE_MAX
} NTFS_CHK_CODE;

/*
 * The issue list grows on demand.  It used to be a fixed 256-entry array, and
 * that was a correctness bug rather than a reporting limit: the repair stages
 * find their work by scanning this list, so every issue detected past the cap
 * was counted in IssueCount but never recorded, and therefore could never be
 * repaired.  `unfixed = IssueCount - RepairedCount` then never reached 0, so
 * the $Volume dirty flag was never cleared and ExitStatus stayed pinned at 1 --
 * a volume with more than 256 defects reported "problems not all corrected"
 * however many times it was checked.
 *
 * Growth is bounded: past CHK_ISSUES_MAX we deliberately stop recording and
 * keep counting, which leaves the volume dirty so it is checked again rather
 * than stamped clean on incomplete information.
 */
#define CHK_ISSUES_INITIAL 256
#define CHK_ISSUES_MAX     65536

typedef struct _NTFS_CHK_ISSUE {
    NTFS_CHK_CODE Code;
    ULONGLONG     Param0;     /* usually the MFT record number               */
    ULONGLONG     Param1;     /* usually a cluster (LCN) or secondary value  */
    int           Fixed;      /* non-zero if this issue was repaired          */
} NTFS_CHK_ISSUE;

/*
 * Optional user-facing message sink.  The core emits chkdsk-style stage
 * banners and a final summary through this callback (never per-record, except
 * under Verbose).  In ReactOS it forwards to the fmifs PFMIFSCALLBACK so
 * autochk/chkdsk.exe display the text; in the standalone harness it prints to
 * stdout.  NULL disables all messaging.  The string is NUL-terminated and
 * owned by the caller for the duration of the call only.
 */
typedef void (*NTFS_CHK_MSGFN)(void *ctx, const char *msg);

typedef struct _NTFS_CHK_OPTIONS {
    int FixErrors;            /* attempt the safe repairs                    */
    int Verbose;             /* standalone: print per-record progress        */
    int CheckOnlyIfDirty;    /* skip full scan if the volume is not dirty    */
    NTFS_CHK_MSGFN Message;   /* user-facing message sink (may be NULL)       */
    void *MessageCtx;         /* opaque context passed to Message             */
} NTFS_CHK_OPTIONS;

typedef struct _NTFS_CHK_RESULT {
    ULONG RecordsScanned;
    ULONG RecordsInUse;
    ULONGLONG ClustersUsed;      /* clusters attributed to some attribute    */
    ULONGLONG ClustersReserved;  /* $Bitmap marks used but nothing refs them */
    ULONG IssueCount;            /* total issues detected                    */
    ULONG IssueRecorded;         /* entries valid in Issues[] (<= IssueCount) */
    ULONG IssueCapacity;         /* entries allocated in Issues[]            */
    ULONG RepairedCount;         /* issues successfully repaired             */
    int   WasDirty;
    int   ExitStatus;            /* 0 = clean, 1 = errors, 2 = errors fixed  */
    /* repair-stage counters */
    ULONG OrphansRecovered;
    ULONG IndexesRebuilt;
    ULONG AttrsTruncated;
    ULONG LinksFixed;
    NTFS_CHK_ISSUE *Issues;      /* heap list; release with NtfsChkFreeResult */
} NTFS_CHK_RESULT;

/*
 * Run a consistency check (and, if opt->FixErrors, the safe repairs) over the
 * volume exposed by io.  Returns opt-independent status: 0 clean, 1 errors
 * remain, 2 errors were fixed, negative on a fatal read failure.
 */
int NtfsChkVolume(const MKNTFS_IO *io,
                  const NTFS_CHK_OPTIONS *opt,
                  NTFS_CHK_RESULT *res);

/*
 * Release the issue list attached to a result.  Every caller that hands a
 * NTFS_CHK_RESULT to NtfsChkVolume() (or to any chk_* helper that can record
 * an issue) owns the list afterwards and must call this.  Safe on a zeroed
 * result and safe to call more than once.
 *
 * The result must be zeroed before its first use, and released through this
 * function before being reused: NtfsChkVolume() zeroes the struct on entry, so
 * running a second check over a result that still holds a list would leak it.
 */
void NtfsChkFreeResult(NTFS_CHK_RESULT *res);

/* Human-readable text for an issue code (for callers that print results). */
const char *NtfsChkCodeString(NTFS_CHK_CODE code);

/* Utility: force the $Volume dirty flag on (used by tests to seed a repair). */
int NtfsChkSetVolumeDirty(const MKNTFS_IO *io);

#endif /* NTFS_CHKDSK_H */
