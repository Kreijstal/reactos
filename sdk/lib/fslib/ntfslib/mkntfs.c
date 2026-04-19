/*
 * winmkntfs - Portable NTFS formatter
 * Core formatting logic, ported from ntfs-3g mkntfs.
 *
 * Copyright (C) 2000-2007 Anton Altaparmakov
 * Copyright (C) 2025 winmkntfs contributors
 * License: GPL-2.0-or-later
 *
 * Creates NTFS 3.1 volumes with these system files:
 *   $MFT, $MFTMirr, $LogFile, $Volume, $AttrDef,
 *   . (root), $Bitmap, $Boot, $BadClus, $Secure, $UpCase, $Extend
 */

#if defined(NTOS_MODE_USER)
/* ReactOS build: use NDK/RTL functions instead of CRT */
#include <ndk/umtypes.h>
#include <ndk/kefuncs.h>
#include <ndk/rtlfuncs.h>
#define printf(...)     /* suppress output in library mode */
#define fprintf(...)    /* suppress output in library mode */
#define malloc(s)       RtlAllocateHeap(RtlGetProcessHeap(), 0, (s))
#define calloc(n, s)    RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, (n) * (s))
#define free(p)         do { if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, (p)); } while(0)
#define memset(d, v, n) RtlFillMemory(d, n, v)
#define memcpy(d, s, n) RtlCopyMemory(d, s, n)
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#endif

#include "mkntfs.h"

/* Forward declarations for embedded data */
extern const ATTR_DEF *get_attrdef_table(ULONG *count);
extern void ntfs_upcase_table_build(WCHAR *uc, ULONG uc_len);

/* ============================================================
 * Internal state
 * ============================================================ */

typedef struct _MKNTFS_STATE {
    const MKNTFS_IO *io;

    /* Volume geometry */
    ULONG sector_size;
    ULONG cluster_size;
    ULONG sectors_per_cluster;
    ULONGLONG total_sectors;
    ULONGLONG total_clusters;
    ULONG mft_record_size;
    ULONG index_record_size;

    /* MFT layout */
    ULONGLONG mft_lcn;          /* First cluster of $MFT */
    ULONGLONG mft_mirr_lcn;     /* First cluster of $MFTMirr */
    ULONG mft_records;          /* Number of MFT records to create (at least 16+11 for reserve) */
    ULONGLONG mft_size;         /* $MFT byte size */
    ULONGLONG mft_clusters;     /* $MFT cluster count */

    /* $LogFile */
    ULONGLONG logfile_lcn;
    ULONGLONG logfile_size;
    ULONGLONG logfile_clusters;

    /* Bitmaps */
    UCHAR *lcn_bitmap;          /* Cluster allocation bitmap */
    ULONG lcn_bitmap_size;      /* In bytes */
    UCHAR *mft_bitmap;          /* MFT record allocation bitmap */
    ULONG mft_bitmap_size;
    ULONGLONG mft_bitmap_lcn;   /* Cluster for MFT bitmap (non-resident) */

    /* MFT buffer */
    UCHAR *mft_buf;             /* All MFT records */

    /* $AttrDef */
    ULONGLONG attrdef_lcn;
    ULONGLONG attrdef_clusters;

    /* $Bitmap */
    ULONGLONG bitmap_lcn;
    ULONGLONG bitmap_clusters;

    /* $UpCase */
    WCHAR *upcase;
    ULONG upcase_len;           /* In WCHARs (65536) */
    ULONGLONG upcase_lcn;
    ULONGLONG upcase_clusters;

    /* Root directory $I30 index allocation */
    ULONGLONG root_idx_lcn;     /* Cluster for INDX block */

    /* Volume info */
    const WCHAR *label;
    ULONGLONG serial_number;

    /* Attribute instance counter per record */
    USHORT next_instance;

    /* Security ID for current record (set before each build_*) */
    ULONG current_security_id;

    /* Timestamp for all system files (set once) */
    ULONGLONG now;

} MKNTFS_STATE;

/* ============================================================
 * Helpers
 * ============================================================ */

static ULONGLONG mkntfs_time(void)
{
#if defined(NTOS_MODE_USER)
    /* Use NtQuerySystemTime — returns NTFS-epoch 100ns ticks directly */
    LARGE_INTEGER SystemTime;
    NtQuerySystemTime(&SystemTime);
    return (ULONGLONG)SystemTime.QuadPart;
#else
    /* NTFS epoch: 1601-01-01, time_t epoch: 1970-01-01
       Difference: 11644473600 seconds
       NTFS unit: 100ns intervals */
    time_t t = time(NULL);
    return ((ULONGLONG)t + 11644473600ULL) * 10000000ULL;
#endif
}

static int io_write(const MKNTFS_STATE *s, ULONGLONG offset, const void *buf, ULONG len)
{
    return s->io->write(s->io->context, offset, buf, len);
}

/* Set bits [start, start+count) in bitmap */
static void bitmap_set(UCHAR *bmp, ULONGLONG start, ULONGLONG count)
{
    ULONGLONG i;
    for (i = 0; i < count; i++) {
        ULONGLONG bit = start + i;
        bmp[bit >> 3] |= (1 << (bit & 7));
    }
}

/* Auto-select cluster size based on volume size */
static ULONG auto_cluster_size(ULONGLONG total_bytes)
{
    /* Windows defaults - NTFS typically uses 4096 for most volumes */
    (void)total_bytes;
    return 4096;
}

/* Encode signed ClustersPerMftRecord / ClustersPerIndexRecord
   If size is a power-of-2 smaller than cluster_size, encode as -log2(size) */
static CHAR encode_clusters_per_record(ULONG record_size, ULONG cluster_size)
{
    if (record_size >= cluster_size) {
        return (CHAR)(record_size / cluster_size);
    }
    /* Negative exponent: record_size = 2^|n| */
    int n = 0;
    ULONG v = record_size;
    while (v > 1) { v >>= 1; n++; }
    return (CHAR)(-n);
}

/* ============================================================
 * Data run encoding
 * ============================================================ */

/* Count bytes needed to encode a signed value */
static int runlen_bytes(LONGLONG val)
{
    int n;
    if (val == 0) return 0;
    if (val > 0) {
        for (n = 1; n <= 8; n++) {
            if (val < (1LL << (n * 8 - 1))) return n;
        }
    } else {
        for (n = 1; n <= 8; n++) {
            if (val >= -(1LL << (n * 8 - 1))) return n;
        }
    }
    return 8;
}

static int runlen_bytes_unsigned(ULONGLONG val)
{
    int n;
    if (val == 0) return 1; /* length field must be at least 1 byte */
    for (n = 1; n <= 8; n++) {
        if (val < (1ULL << (n * 8))) return n;
    }
    return 8;
}

/* Encode a single data run: (length, lcn_offset) relative to previous LCN.
   Returns bytes written. */
static int encode_run(UCHAR *dst, ULONGLONG length, LONGLONG lcn_offset)
{
    int len_bytes = runlen_bytes_unsigned(length);
    int off_bytes = runlen_bytes(lcn_offset);
    int i;

    dst[0] = (UCHAR)((off_bytes << 4) | len_bytes);
    for (i = 0; i < len_bytes; i++)
        dst[1 + i] = (UCHAR)(length >> (i * 8));
    for (i = 0; i < off_bytes; i++)
        dst[1 + len_bytes + i] = (UCHAR)(lcn_offset >> (i * 8));
    return 1 + len_bytes + off_bytes;
}

/* ============================================================
 * Update Sequence Array (fixup) support
 * ============================================================ */

/* Apply fixup: set the USN and store original last-2-bytes of each sector */
static void apply_fixup(void *record, ULONG record_size, ULONG sector_size)
{
    FILE_RECORD_HEADER *hdr = (FILE_RECORD_HEADER *)record;
    USHORT *usa;
    ULONG i, num_sectors;
    USHORT usn;

    num_sectors = record_size / sector_size;
    usa = (USHORT *)((UCHAR *)record + hdr->UpdateSeqOffset);
    usn = *usa;  /* USN value */

    /* For each sector, save the last 2 bytes and replace with USN */
    for (i = 0; i < num_sectors; i++) {
        USHORT *sector_end = (USHORT *)((UCHAR *)record + (i + 1) * sector_size - 2);
        usa[1 + i] = *sector_end;   /* Save original */
        *sector_end = usn;           /* Replace with USN */
    }
}

/* ============================================================
 * Attribute builders
 * ============================================================ */

/* Initialize a file record header */
static void init_file_record(FILE_RECORD_HEADER *rec, ULONG rec_size, ULONG sector_size,
                              USHORT seq_num, USHORT flags, ULONG mft_num)
{
    ULONG usa_offset, usa_size;

    memset(rec, 0, rec_size);
    rec->Magic = NRH_FILE_TYPE;

    /* Update sequence: right after the header */
    usa_offset = sizeof(FILE_RECORD_HEADER);
    usa_size = 1 + (rec_size / sector_size);  /* 1 USN + N array entries */
    rec->UpdateSeqOffset = (USHORT)usa_offset;
    rec->UpdateSeqSize = (USHORT)usa_size;

    /* Set USN to 1 */
    USHORT *usa = (USHORT *)((UCHAR *)rec + usa_offset);
    usa[0] = 1;  /* USN value */

    rec->SequenceNumber = seq_num;
    rec->LinkCount = 0;
    rec->AttributeOffset = (USHORT)NTFS_ALIGN_UP(usa_offset + usa_size * 2, 8);
    rec->Flags = flags;
    rec->BytesAllocated = rec_size;
    rec->MftRecordNumber = mft_num;
    rec->NextAttributeId = 0;
}

/* Finalize a record: set NextAttributeId from tracked instance counter */
static void finalize_record(MKNTFS_STATE *s, FILE_RECORD_HEADER *rec)
{
    rec->NextAttributeId = s->next_instance;
}

/* Get pointer to where the next attribute should be written */
static UCHAR *get_attr_pos(FILE_RECORD_HEADER *rec)
{
    UCHAR *base = (UCHAR *)rec;
    UCHAR *pos = base + rec->AttributeOffset;

    /* Walk existing attributes to find the end */
    while (pos < base + rec->BytesAllocated - 8) {
        ATTR_RECORD *a = (ATTR_RECORD *)pos;
        if (a->Type == AT_END || a->Type == 0 || a->Length == 0)
            break;
        pos += a->Length;
    }
    return pos;
}

/* Add a resident attribute. Returns pointer past the new attribute. */
static UCHAR *add_resident_attr(MKNTFS_STATE *s, FILE_RECORD_HEADER *rec,
                                 ULONG type, const WCHAR *name, ULONG name_len,
                                 const void *value, ULONG value_len,
                                 UCHAR resident_flags)
{
    UCHAR *pos = get_attr_pos(rec);
    ATTR_RECORD *a = (ATTR_RECORD *)pos;
    ULONG name_off, value_off, total_len;

    /* Zero only the header, not the AT_END marker that follows */
    memset(a, 0, sizeof(ATTR_RECORD));
    a->Type = type;
    a->NonResident = 0;
    a->NameLength = (UCHAR)name_len;
    a->Flags = 0;
    a->Instance = s->next_instance++;

    /* Name goes right after the fixed header (offset 0x18 for resident) */
    name_off = 0x18;
    if (name_len > 0) {
        a->NameOffset = (USHORT)name_off;
        memcpy(pos + name_off, name, name_len * sizeof(WCHAR));
        value_off = NTFS_ALIGN_UP(name_off + name_len * sizeof(WCHAR), 4);
    } else {
        a->NameOffset = (USHORT)name_off;
        value_off = name_off;
    }

    a->Resident.ValueOffset = (USHORT)value_off;
    a->Resident.ValueLength = value_len;
    a->Resident.ResidentFlags = resident_flags;

    if (value && value_len > 0)
        memcpy(pos + value_off, value, value_len);

    total_len = NTFS_ALIGN_UP(value_off + value_len, 8);
    a->Length = total_len;

    /* Write AT_END marker after this attribute */
    ULONG *end = (ULONG *)(pos + total_len);
    *end = AT_END;

    rec->BytesInUse = (ULONG)(pos + total_len + 8 - (UCHAR *)rec); /* +8 for AT_END + padding */
    return pos + total_len;
}

/* Add a non-resident attribute with a simple single-run mapping. */
static UCHAR *add_nonresident_attr(MKNTFS_STATE *s, FILE_RECORD_HEADER *rec,
                                    ULONG type, const WCHAR *name, ULONG name_len,
                                    ULONGLONG start_lcn, ULONGLONG num_clusters,
                                    ULONGLONG data_size, ULONGLONG initialized_size)
{
    UCHAR *pos = get_attr_pos(rec);
    ATTR_RECORD *a = (ATTR_RECORD *)pos;
    ULONG name_off, runs_off, total_len;
    UCHAR run_buf[16];
    int run_len;

    memset(a, 0, 0x48); /* Non-resident header is 0x40, pad to 0x48 */
    a->Type = type;
    a->NonResident = 1;
    a->NameLength = (UCHAR)name_len;
    a->Flags = 0;
    a->Instance = s->next_instance++;

    /* Name offset: right after the fixed non-resident header (0x40) */
    name_off = 0x40;
    if (name_len > 0) {
        a->NameOffset = (USHORT)name_off;
        memcpy(pos + name_off, name, name_len * sizeof(WCHAR));
        runs_off = NTFS_ALIGN_UP(name_off + name_len * sizeof(WCHAR), 4);
    } else {
        a->NameOffset = (USHORT)name_off;
        runs_off = name_off;
    }

    a->NR.LowestVcn = 0;
    a->NR.HighestVcn = num_clusters - 1;
    a->NR.MappingPairsOffset = (USHORT)runs_off;
    a->NR.AllocatedSize = (LONGLONG)(num_clusters * s->cluster_size);
    a->NR.DataSize = (LONGLONG)data_size;
    a->NR.InitializedSize = (LONGLONG)initialized_size;

    /* Encode single data run */
    run_len = encode_run(run_buf, num_clusters, (LONGLONG)start_lcn);
    memcpy(pos + runs_off, run_buf, run_len);
    pos[runs_off + run_len] = 0; /* Terminator */

    total_len = NTFS_ALIGN_UP(runs_off + run_len + 1, 8);
    a->Length = total_len;

    /* AT_END marker */
    ULONG *end = (ULONG *)(pos + total_len);
    *end = AT_END;

    rec->BytesInUse = (ULONG)(pos + total_len + 8 - (UCHAR *)rec);
    return pos + total_len;
}

/* ============================================================
 * System file record builders
 * ============================================================ */

static void make_file_name(FILE_NAME_ATTR *fn, ULONGLONG parent_mft_ref,
                            const WCHAR *name, UCHAR name_len,
                            ULONG file_attrs, ULONGLONG alloc_size,
                            ULONGLONG data_size, ULONGLONG time_val)
{
    /* Parent ref: MFT number in low 48 bits, seq in high 16 */
    fn->ParentDirectory = parent_mft_ref | ((ULONGLONG)1 << 48); /* seq 1 */
    fn->CreationTime = time_val;
    fn->ModificationTime = time_val;
    fn->MftModificationTime = time_val;
    fn->AccessTime = time_val;
    fn->AllocatedSize = alloc_size;
    fn->DataSize = data_size;
    fn->FileAttributes = file_attrs;
    fn->Extended.EaInfo.PackedEaSize = 0;
    fn->Extended.EaInfo.Reserved = 0;
    fn->FileNameLength = name_len;
    fn->FileNameType = FILE_NAME_WIN32_AND_DOS;
    memcpy(fn->FileName, name, name_len * sizeof(WCHAR));
}

/* MFT reference: record number | (seq << 48) */
static ULONGLONG mft_ref(ULONG record, USHORT seq)
{
    return (ULONGLONG)record | ((ULONGLONG)seq << 48);
}

/* Helper: add standard $SI + $FN to a record */
static void add_si_and_fn(MKNTFS_STATE *s, FILE_RECORD_HEADER *rec,
                           const WCHAR *name, UCHAR name_len,
                           ULONG file_attrs, ULONGLONG alloc_size,
                           ULONGLONG data_size)
{
    STANDARD_INFORMATION si;
    UCHAR fn_buf[sizeof(FILE_NAME_ATTR) + 16 * sizeof(WCHAR)];
    FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)fn_buf;

    memset(&si, 0, sizeof(si));
    si.CreationTime = si.ModificationTime = si.MftModificationTime = si.AccessTime = s->now;
    si.FileAttributes = file_attrs;
    si.SecurityId = s->current_security_id;
    add_resident_attr(s, rec, AT_STANDARD_INFORMATION, NULL, 0, &si, 72, 0);

    memset(fn_buf, 0, sizeof(fn_buf));
    make_file_name(fn, FILE_Root, name, name_len,
                   file_attrs, alloc_size, data_size, s->now);
    add_resident_attr(s, rec, AT_FILE_NAME, NULL, 0, fn_buf,
                      sizeof(FILE_NAME_ATTR) - sizeof(WCHAR) + name_len * sizeof(WCHAR),
                      0x01); /* ResidentFlags=0x01: indexed */
}

/* Build $MFT (record 0) */
static void build_mft(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 0 * s->mft_record_size);

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, FRH_IN_USE, FILE_MFT);
    rec->LinkCount = 1;

    WCHAR mft_name[] = { '$', 'M', 'F', 'T' };
    add_si_and_fn(s, rec, mft_name, 4,
                  FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM,
                  s->mft_size, s->mft_size);

    /* $DATA (non-resident) - the MFT data itself */
    add_nonresident_attr(s, rec, AT_DATA, NULL, 0,
                         s->mft_lcn, s->mft_clusters,
                         s->mft_size, s->mft_size);

    /* $BITMAP (non-resident - MFT bitmap) */
    add_nonresident_attr(s, rec, AT_BITMAP, NULL, 0,
                         s->mft_bitmap_lcn, 1,
                         s->mft_bitmap_size, s->mft_bitmap_size);

    finalize_record(s, rec);
}

/* Build $MFTMirr (record 1) */
static void build_mftmirr(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 1 * s->mft_record_size);
    ULONGLONG mirr_size = 4 * s->mft_record_size;
    ULONGLONG mirr_clusters = (mirr_size + s->cluster_size - 1) / s->cluster_size;

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, FRH_IN_USE, FILE_MFTMirr);
    rec->LinkCount = 1;

    WCHAR name[] = { '$', 'M', 'F', 'T', 'M', 'i', 'r', 'r' };
    add_si_and_fn(s, rec, name, 8, FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM,
                  mirr_size, mirr_size);

    add_nonresident_attr(s, rec, AT_DATA, NULL, 0,
                         s->mft_mirr_lcn, mirr_clusters,
                         mirr_size, mirr_size);

    finalize_record(s, rec);
}

/* Build $LogFile (record 2) */
static void build_logfile(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 2 * s->mft_record_size);

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, FRH_IN_USE, FILE_LogFile);
    rec->LinkCount = 1;

    WCHAR name[] = { '$', 'L', 'o', 'g', 'F', 'i', 'l', 'e' };
    add_si_and_fn(s, rec, name, 8, FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM,
                  s->logfile_size, s->logfile_size);

    add_nonresident_attr(s, rec, AT_DATA, NULL, 0,
                         s->logfile_lcn, s->logfile_clusters,
                         s->logfile_size, s->logfile_size);

    finalize_record(s, rec);
}

/* Build $Volume (record 3) */
static void build_volume(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 3 * s->mft_record_size);
    VOLUME_INFORMATION vi;

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, FRH_IN_USE, FILE_Volume);
    rec->LinkCount = 1;

    WCHAR name[] = { '$', 'V', 'o', 'l', 'u', 'm', 'e' };
    add_si_and_fn(s, rec, name, 7, FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM, 0, 0);

    /* $VOLUME_NAME */
    if (s->label) {
        ULONG label_len = 0;
        while (s->label[label_len]) label_len++;
        add_resident_attr(s, rec, AT_VOLUME_NAME, NULL, 0,
                          s->label, label_len * sizeof(WCHAR), 0);
    }

    /* $VOLUME_INFORMATION: NTFS 3.1 */
    memset(&vi, 0, sizeof(vi));
    vi.MajorVersion = 3;
    vi.MinorVersion = 1;
    add_resident_attr(s, rec, AT_VOLUME_INFORMATION, NULL, 0, &vi, sizeof(vi), 0);

    finalize_record(s, rec);
}

/* Build $AttrDef (record 4) */
static void build_attrdef(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 4 * s->mft_record_size);

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, FRH_IN_USE, FILE_AttrDef);
    rec->LinkCount = 1;

    ULONG attrdef_count;
    const ATTR_DEF *attrdef_table = get_attrdef_table(&attrdef_count);
    ULONG attrdef_bytes = attrdef_count * sizeof(ATTR_DEF);
    (void)attrdef_table;

    WCHAR name[] = { '$', 'A', 't', 't', 'r', 'D', 'e', 'f' };
    add_si_and_fn(s, rec, name, 8, FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM,
                  attrdef_bytes, attrdef_bytes);

    /* $DATA - non-resident (too large for 1024-byte MFT record) */
    s->attrdef_clusters = (attrdef_bytes + s->cluster_size - 1) / s->cluster_size;
    add_nonresident_attr(s, rec, AT_DATA, NULL, 0,
                         s->attrdef_lcn, s->attrdef_clusters,
                         attrdef_bytes, attrdef_bytes);
    bitmap_set(s->lcn_bitmap, s->attrdef_lcn, s->attrdef_clusters);

    finalize_record(s, rec);
}

/* Helper: create an index entry for a system file in root directory */
static ULONG make_index_entry(UCHAR *buf, ULONG mft_num, USHORT seq,
                               const WCHAR *name, UCHAR name_len,
                               ULONG file_attrs, ULONGLONG time_val)
{
    INDEX_ENTRY *ie = (INDEX_ENTRY *)buf;
    FILE_NAME_ATTR *fn;
    ULONG fn_size = sizeof(FILE_NAME_ATTR) - sizeof(WCHAR) + name_len * sizeof(WCHAR);
    ULONG entry_size = NTFS_ALIGN_UP(sizeof(INDEX_ENTRY) + fn_size, 8);

    memset(buf, 0, entry_size);
    ie->Data.Directory.IndexedFile = mft_ref(mft_num, seq);
    ie->Length = (USHORT)entry_size;
    ie->KeyLength = (USHORT)fn_size;
    ie->Flags = 0;

    fn = (FILE_NAME_ATTR *)(buf + sizeof(INDEX_ENTRY));
    fn->ParentDirectory = mft_ref(FILE_Root, 1);
    fn->CreationTime = time_val;
    fn->ModificationTime = time_val;
    fn->MftModificationTime = time_val;
    fn->AccessTime = time_val;
    fn->FileAttributes = file_attrs;
    fn->FileNameLength = name_len;
    fn->FileNameType = FILE_NAME_WIN32_AND_DOS;
    memcpy(fn->FileName, name, name_len * sizeof(WCHAR));

    return entry_size;
}

/* Build root directory (record 5) — uses INDEX_ALLOCATION for all system files */
static void build_root(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 5 * s->mft_record_size);

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1,
                     FRH_IN_USE | FRH_DIRECTORY, FILE_Root);
    rec->LinkCount = 1;

    WCHAR dot = '.';
    add_si_and_fn(s, rec, &dot, 1,
                  FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM | FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT,
                  0, 0);

    /* $INDEX_ROOT for $I30 — large index (entries in INDX block) */
    WCHAR i30_name[] = { '$', 'I', '3', '0' };
    {
        /* INDEX_ROOT with just an end entry pointing to sub-node VCN 0 */
        UCHAR idx_buf[sizeof(INDEX_ROOT) + sizeof(INDEX_ENTRY) + 8]; /* +8 for VCN */
        INDEX_ROOT *ir = (INDEX_ROOT *)idx_buf;
        INDEX_ENTRY *ie;

        memset(idx_buf, 0, sizeof(idx_buf));
        ir->AttributeType = AT_FILE_NAME;
        ir->CollationRule = 1; /* COLLATION_FILE_NAME */
        ir->IndexBlockSize = s->index_record_size;
        ir->ClustersPerIndexBlock = (UCHAR)(s->index_record_size / s->cluster_size);
        if (ir->ClustersPerIndexBlock == 0) ir->ClustersPerIndexBlock = 1;

        ir->Header.EntriesOffset = sizeof(INDEX_HEADER);
        ir->Header.IndexLength = sizeof(INDEX_HEADER) + sizeof(INDEX_ENTRY) + 8;
        ir->Header.AllocatedSize = sizeof(INDEX_HEADER) + sizeof(INDEX_ENTRY) + 8;
        ir->Header.Flags = 1; /* Large index — has sub-nodes */

        ie = (INDEX_ENTRY *)(idx_buf + sizeof(INDEX_ROOT));
        ie->Length = sizeof(INDEX_ENTRY) + 8; /* +8 for sub-node VCN */
        ie->Flags = INDEX_ENTRY_END | INDEX_ENTRY_NODE;
        /* VCN of sub-node at end of entry */
        *(ULONGLONG *)((UCHAR *)ie + sizeof(INDEX_ENTRY)) = 0;

        add_resident_attr(s, rec, AT_INDEX_ROOT, i30_name, 4,
                          idx_buf, sizeof(INDEX_ROOT) + sizeof(INDEX_ENTRY) + 8, 0);
    }

    /* $INDEX_ALLOCATION — non-resident $I30 pointing to INDX cluster */
    add_nonresident_attr(s, rec, AT_INDEX_ALLOCATION, i30_name, 4,
                         s->root_idx_lcn, 1, /* 1 cluster */
                         s->index_record_size, s->index_record_size);

    /* $BITMAP for $I30 — 1 bit set (block 0 in use) */
    {
        UCHAR bmp[8];
        memset(bmp, 0, sizeof(bmp));
        bmp[0] = 1; /* Block 0 is in use */
        add_resident_attr(s, rec, AT_BITMAP, i30_name, 4, bmp, 8, 0);
    }

    finalize_record(s, rec);
}

/* Build the INDX block for root directory (written to disk separately) */
static int write_root_index(MKNTFS_STATE *s)
{
    UCHAR *buf;
    INDEX_BUFFER *ib;
    ULONG usa_offset, usa_size;
    ULONG entries_offset;
    UCHAR *entries_pos;
    ULONG entries_size = 0;
    int i;

    buf = (UCHAR *)calloc(1, s->index_record_size);
    if (!buf) return -1;

    ib = (INDEX_BUFFER *)buf;
    ib->Magic = NRH_INDX_TYPE;
    usa_offset = sizeof(INDEX_BUFFER);
    usa_size = 1 + (s->index_record_size / s->sector_size);
    ib->UpdateSeqOffset = (USHORT)usa_offset;
    ib->UpdateSeqSize = (USHORT)usa_size;
    ib->Vcn = 0;

    /* Set USN value */
    USHORT *usa = (USHORT *)(buf + usa_offset);
    usa[0] = 1;

    entries_offset = (ULONG)NTFS_ALIGN_UP(usa_offset + usa_size * 2, 8);
    ib->Header.EntriesOffset = entries_offset - (ULONG)((UCHAR *)&ib->Header - buf);
    entries_pos = buf + entries_offset;

    /* All system files, sorted by NTFS uppercase collation */
    /* $ (0x24) sorts before . (0x2E), within $ names sort alphabetically by uppercase */
    struct { ULONG mft_num; const WCHAR *name; UCHAR len; ULONG attrs; ULONGLONG alloc; ULONGLONG data; } sysfiles[] = {
        { FILE_AttrDef,  (WCHAR[]){ '$','A','t','t','r','D','e','f' }, 8,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_BadClus,  (WCHAR[]){ '$','B','a','d','C','l','u','s' }, 8,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_Bitmap,   (WCHAR[]){ '$','B','i','t','m','a','p' }, 7,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_Boot,     (WCHAR[]){ '$','B','o','o','t' }, 5,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_Extend,   (WCHAR[]){ '$','E','x','t','e','n','d' }, 7,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM|FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT, 0, 0 },
        { FILE_LogFile,  (WCHAR[]){ '$','L','o','g','F','i','l','e' }, 8,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_MFT,      (WCHAR[]){ '$','M','F','T' }, 4,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_MFTMirr,  (WCHAR[]){ '$','M','F','T','M','i','r','r' }, 8,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_Secure,   (WCHAR[]){ '$','S','e','c','u','r','e' }, 7,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM|FILE_ATTR_DUP_VIEW_INDEX_PRESENT, 0, 0 },
        { FILE_UpCase,   (WCHAR[]){ '$','U','p','C','a','s','e' }, 7,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_Volume,   (WCHAR[]){ '$','V','o','l','u','m','e' }, 7,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM, 0, 0 },
        { FILE_Root,     (WCHAR[]){ '.' }, 1,
          FILE_ATTR_HIDDEN|FILE_ATTR_SYSTEM|FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT, 0, 0 },
    };
    int num_sysfiles = sizeof(sysfiles) / sizeof(sysfiles[0]);

    for (i = 0; i < num_sysfiles; i++) {
        entries_size += make_index_entry(entries_pos + entries_size,
                                         sysfiles[i].mft_num, 1,
                                         sysfiles[i].name, sysfiles[i].len,
                                         sysfiles[i].attrs, s->now);
    }

    /* End entry */
    INDEX_ENTRY *end = (INDEX_ENTRY *)(entries_pos + entries_size);
    memset(end, 0, sizeof(INDEX_ENTRY));
    end->Length = sizeof(INDEX_ENTRY);
    end->Flags = INDEX_ENTRY_END;
    entries_size += sizeof(INDEX_ENTRY);

    ib->Header.IndexLength = ib->Header.EntriesOffset + entries_size;
    ib->Header.AllocatedSize = s->index_record_size - entries_offset + ib->Header.EntriesOffset;
    ib->Header.Flags = 0; /* Leaf node */

    /* Apply INDX fixup */
    {
        USHORT *indx_usa = (USHORT *)(buf + ib->UpdateSeqOffset);
        USHORT usn = indx_usa[0];
        ULONG num_sectors = s->index_record_size / s->sector_size;
        ULONG j;
        for (j = 0; j < num_sectors; j++) {
            USHORT *sector_end = (USHORT *)(buf + (j + 1) * s->sector_size - 2);
            indx_usa[1 + j] = *sector_end;
            *sector_end = usn;
        }
    }

    /* Write INDX block to disk */
    int ret = io_write(s, s->root_idx_lcn * s->cluster_size, buf, s->index_record_size);
    free(buf);
    return ret;
}

/* Build $Bitmap (record 6) */
static void build_bitmap(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 6 * s->mft_record_size);

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, FRH_IN_USE, FILE_Bitmap);
    rec->LinkCount = 1;

    WCHAR name[] = { '$', 'B', 'i', 't', 'm', 'a', 'p' };
    add_si_and_fn(s, rec, name, 7, FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM,
                  s->bitmap_clusters * s->cluster_size, s->lcn_bitmap_size);

    add_nonresident_attr(s, rec, AT_DATA, NULL, 0,
                         s->bitmap_lcn, s->bitmap_clusters,
                         s->lcn_bitmap_size, s->lcn_bitmap_size);

    bitmap_set(s->lcn_bitmap, s->bitmap_lcn, s->bitmap_clusters);

    finalize_record(s, rec);
}

/* Build $Boot (record 7) */
static void build_boot(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 7 * s->mft_record_size);
    ULONGLONG boot_clusters = 1;
    if (s->cluster_size < s->sector_size * 16)
        boot_clusters = (s->sector_size * 16 + s->cluster_size - 1) / s->cluster_size;

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, FRH_IN_USE, FILE_Boot);
    rec->LinkCount = 1;

    WCHAR name[] = { '$', 'B', 'o', 'o', 't' };
    add_si_and_fn(s, rec, name, 5, FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM,
                  boot_clusters * s->cluster_size, boot_clusters * s->cluster_size);

    add_nonresident_attr(s, rec, AT_DATA, NULL, 0,
                         0, boot_clusters,
                         boot_clusters * s->cluster_size,
                         boot_clusters * s->cluster_size);

    finalize_record(s, rec);
}

/* Build a simple system file record with just $SI + $FN and empty $DATA or $I30 */
static void build_simple_system_file(MKNTFS_STATE *s, ULONG record_num,
                                      const WCHAR *name, UCHAR name_len,
                                      USHORT flags)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + record_num * s->mft_record_size);
    ULONG file_attrs = FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM;

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, flags, record_num);
    rec->LinkCount = 1;

    if (flags & FRH_DIRECTORY)
        file_attrs |= FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT;

    add_si_and_fn(s, rec, name, name_len, file_attrs, 0, 0);

    if (flags & FRH_DIRECTORY) {
        /* Empty $INDEX_ROOT */
        UCHAR idx_buf[sizeof(INDEX_ROOT) + sizeof(INDEX_ENTRY)];
        INDEX_ROOT *ir = (INDEX_ROOT *)idx_buf;
        INDEX_ENTRY *ie;

        memset(idx_buf, 0, sizeof(idx_buf));
        ir->AttributeType = AT_FILE_NAME;
        ir->CollationRule = 1;
        ir->IndexBlockSize = s->index_record_size;
        ir->ClustersPerIndexBlock = (UCHAR)(s->index_record_size / s->cluster_size);
        if (ir->ClustersPerIndexBlock == 0) ir->ClustersPerIndexBlock = 1;
        ir->Header.EntriesOffset = sizeof(INDEX_HEADER);
        ir->Header.IndexLength = sizeof(INDEX_HEADER) + sizeof(INDEX_ENTRY);
        ir->Header.AllocatedSize = sizeof(INDEX_HEADER) + sizeof(INDEX_ENTRY);

        ie = (INDEX_ENTRY *)(idx_buf + sizeof(INDEX_ROOT));
        ie->Length = sizeof(INDEX_ENTRY);
        ie->Flags = INDEX_ENTRY_END;

        WCHAR i30_name[] = { '$', 'I', '3', '0' };
        add_resident_attr(s, rec, AT_INDEX_ROOT, i30_name, 4,
                          idx_buf, sizeof(idx_buf), 0);
    } else {
        /* Empty resident $DATA */
        add_resident_attr(s, rec, AT_DATA, NULL, 0, NULL, 0, 0);
    }

    finalize_record(s, rec);
}

/*
 * Minimal self-relative security descriptor for NTFS system files.
 * Owner/Group = S-1-5-18 (Local System), DACL grants SYSTEM full control.
 * This matches what Windows creates for fresh NTFS system files.
 */
static ULONG build_default_sd(UCHAR *buf, ULONG buf_size)
{
    /* Self-relative security descriptor with:
       Owner: S-1-5-18 (offset 0x14)
       Group: S-1-5-18 (offset 0x20)
       DACL:  Allow SYSTEM full access (offset 0x2C)
       No SACL */
    static const UCHAR sd_data[] = {
        /* Revision=1, Sbz1=0, Control=0x8004 (SE_DACL_PRESENT | SE_SELF_RELATIVE) */
        0x01, 0x00, 0x04, 0x80,
        /* OffsetOwner=0x30, OffsetGroup=0x3C, OffsetSacl=0, OffsetDacl=0x14 */
        0x30, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
        /* DACL: Revision=2, Size=0x1C, AceCount=1 */
        0x02, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x00, 0x00,
        /* ACE: Type=0(Allow), Flags=0, Size=0x14, Mask=0x1F01FF (FULL) */
        0x00, 0x00, 0x14, 0x00, 0xFF, 0x01, 0x1F, 0x00,
        /* SID: S-1-5-18 (Revision=1, SubAuthCount=1, IA=5, SubAuth=18) */
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
        0x12, 0x00, 0x00, 0x00,
        /* Owner SID: S-1-5-18 */
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
        0x12, 0x00, 0x00, 0x00,
        /* Group SID: S-1-5-18 */
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
        0x12, 0x00, 0x00, 0x00,
    };
    ULONG sd_len = sizeof(sd_data);
    if (sd_len > buf_size) return 0;
    memcpy(buf, sd_data, sd_len);
    return sd_len;
}

/* Simple hash for security descriptor (djb2-style, matches ntfs-3g's approach) */
static ULONG sd_hash(const UCHAR *data, ULONG len)
{
    ULONG hash = 0;
    ULONG i;
    for (i = 0; i < len; i++) {
        hash = ((hash >> 29) | (hash << 3)) + data[i];
    }
    return hash;
}

/* Build $Secure (record 9) with $SDS, $SDH, $SII containing a default SD */
static void build_secure(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + FILE_Secure * s->mft_record_size);
    #define FRH_VIEW_INDEX 0x0004
    UCHAR sd_buf[256];
    ULONG sd_len;
    ULONG hash_val;

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1,
                     FRH_IN_USE | FRH_VIEW_INDEX, FILE_Secure);
    rec->LinkCount = 1;

    WCHAR name[] = { '$', 'S', 'e', 'c', 'u', 'r', 'e' };
    add_si_and_fn(s, rec, name, 7,
                  FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM | FILE_ATTR_DUP_VIEW_INDEX_PRESENT,
                  0, 0);

    /* Build the default security descriptor */
    sd_len = build_default_sd(sd_buf, sizeof(sd_buf));
    hash_val = sd_hash(sd_buf, sd_len);

    /* $SDS named data stream — contains SECURITY_DESCRIPTOR_HEADER + SD data */
    {
        ULONG sds_entry_len = sizeof(SECURITY_DESCRIPTOR_HEADER) + sd_len;
        ULONG sds_padded = NTFS_ALIGN_UP(sds_entry_len, 16);
        UCHAR sds_buf[512];
        SECURITY_DESCRIPTOR_HEADER *sdh = (SECURITY_DESCRIPTOR_HEADER *)sds_buf;

        memset(sds_buf, 0, sizeof(sds_buf));
        sdh->Hash = hash_val;
        sdh->SecurityId = 256; /* First security ID */
        sdh->Offset = 0;
        sdh->Length = sds_entry_len;
        memcpy(sds_buf + sizeof(SECURITY_DESCRIPTOR_HEADER), sd_buf, sd_len);

        WCHAR sds_name[] = { '$', 'S', 'D', 'S' };
        add_resident_attr(s, rec, AT_DATA, sds_name, 4, sds_buf, sds_padded, 0);
    }

    /* $SDH index root — one entry + end */
    {
        /* SDH index entry: key is SECURITY_DESCRIPTOR_HEADER, data is SECURITY_DESCRIPTOR_HEADER */
        UCHAR idx_buf[sizeof(INDEX_ROOT) + 128];
        INDEX_ROOT *ir = (INDEX_ROOT *)idx_buf;
        UCHAR *ie_pos;
        INDEX_ENTRY *ie;
        SECURITY_DESCRIPTOR_HEADER sdh_key;
        ULONG ie_data_off, ie_len;

        memset(idx_buf, 0, sizeof(idx_buf));
        ir->AttributeType = 0;
        ir->CollationRule = 0x12; /* COLLATION_NTOFS_SECURITY_HASH */
        ir->IndexBlockSize = s->index_record_size;
        ir->ClustersPerIndexBlock = (UCHAR)(s->index_record_size / s->cluster_size);
        if (ir->ClustersPerIndexBlock == 0) ir->ClustersPerIndexBlock = 1;

        ie_pos = idx_buf + sizeof(INDEX_ROOT);

        /* SDH entry: key=SECURITY_DESCRIPTOR_HEADER (20 bytes), data=SDH_DATA (not used, just pad) */
        memset(&sdh_key, 0, sizeof(sdh_key));
        sdh_key.Hash = hash_val;
        sdh_key.SecurityId = 256;
        sdh_key.Offset = 0;
        sdh_key.Length = sizeof(SECURITY_DESCRIPTOR_HEADER) + sd_len;

        ie = (INDEX_ENTRY *)ie_pos;
        ie->Data.ViewIndex.DataOffset = sizeof(INDEX_ENTRY) + sizeof(SECURITY_DESCRIPTOR_HEADER);
        ie->Data.ViewIndex.DataLength = sizeof(SECURITY_DESCRIPTOR_HEADER);
        ie->KeyLength = sizeof(SECURITY_DESCRIPTOR_HEADER);
        ie_data_off = sizeof(INDEX_ENTRY) + sizeof(SECURITY_DESCRIPTOR_HEADER) + sizeof(SECURITY_DESCRIPTOR_HEADER);
        ie_len = NTFS_ALIGN_UP(ie_data_off, 8);
        ie->Length = (USHORT)ie_len;
        ie->Flags = 0;
        memcpy(ie_pos + sizeof(INDEX_ENTRY), &sdh_key, sizeof(sdh_key));
        memcpy(ie_pos + sizeof(INDEX_ENTRY) + sizeof(SECURITY_DESCRIPTOR_HEADER), &sdh_key, sizeof(sdh_key));
        ie_pos += ie_len;

        /* End entry */
        ie = (INDEX_ENTRY *)ie_pos;
        memset(ie, 0, sizeof(INDEX_ENTRY));
        ie->Length = sizeof(INDEX_ENTRY);
        ie->Flags = INDEX_ENTRY_END;
        ie_pos += sizeof(INDEX_ENTRY);

        ULONG entries_size = (ULONG)(ie_pos - (idx_buf + sizeof(INDEX_ROOT)));
        ir->Header.EntriesOffset = sizeof(INDEX_HEADER);
        ir->Header.IndexLength = sizeof(INDEX_HEADER) + entries_size;
        ir->Header.AllocatedSize = sizeof(INDEX_HEADER) + entries_size;

        WCHAR sdh_name[] = { '$', 'S', 'D', 'H' };
        add_resident_attr(s, rec, AT_INDEX_ROOT, sdh_name, 4,
                          idx_buf, sizeof(INDEX_ROOT) + entries_size, 0);
    }

    /* $SII index root — one entry + end */
    {
        UCHAR idx_buf[sizeof(INDEX_ROOT) + 128];
        INDEX_ROOT *ir = (INDEX_ROOT *)idx_buf;
        UCHAR *ie_pos;
        INDEX_ENTRY *ie;
        SECURITY_DESCRIPTOR_HEADER sii_data;
        ULONG ie_len;

        memset(idx_buf, 0, sizeof(idx_buf));
        ir->AttributeType = 0;
        ir->CollationRule = 0x10; /* COLLATION_NTOFS_ULONG */
        ir->IndexBlockSize = s->index_record_size;
        ir->ClustersPerIndexBlock = (UCHAR)(s->index_record_size / s->cluster_size);
        if (ir->ClustersPerIndexBlock == 0) ir->ClustersPerIndexBlock = 1;

        ie_pos = idx_buf + sizeof(INDEX_ROOT);

        /* SII entry: key=SecurityId (ULONG), data=SECURITY_DESCRIPTOR_HEADER */
        ie = (INDEX_ENTRY *)ie_pos;
        ULONG sec_id = 256;
        ie->Data.ViewIndex.DataOffset = sizeof(INDEX_ENTRY) + sizeof(ULONG);
        ie->Data.ViewIndex.DataLength = sizeof(SECURITY_DESCRIPTOR_HEADER);
        ie->KeyLength = sizeof(ULONG);
        ie_len = NTFS_ALIGN_UP(sizeof(INDEX_ENTRY) + sizeof(ULONG) + sizeof(SECURITY_DESCRIPTOR_HEADER), 8);
        ie->Length = (USHORT)ie_len;
        ie->Flags = 0;
        memcpy(ie_pos + sizeof(INDEX_ENTRY), &sec_id, sizeof(ULONG));

        memset(&sii_data, 0, sizeof(sii_data));
        sii_data.Hash = hash_val;
        sii_data.SecurityId = 256;
        sii_data.Offset = 0;
        sii_data.Length = sizeof(SECURITY_DESCRIPTOR_HEADER) + sd_len;
        memcpy(ie_pos + sizeof(INDEX_ENTRY) + sizeof(ULONG), &sii_data, sizeof(sii_data));
        ie_pos += ie_len;

        /* End entry */
        ie = (INDEX_ENTRY *)ie_pos;
        memset(ie, 0, sizeof(INDEX_ENTRY));
        ie->Length = sizeof(INDEX_ENTRY);
        ie->Flags = INDEX_ENTRY_END;
        ie_pos += sizeof(INDEX_ENTRY);

        ULONG entries_size = (ULONG)(ie_pos - (idx_buf + sizeof(INDEX_ROOT)));
        ir->Header.EntriesOffset = sizeof(INDEX_HEADER);
        ir->Header.IndexLength = sizeof(INDEX_HEADER) + entries_size;
        ir->Header.AllocatedSize = sizeof(INDEX_HEADER) + entries_size;

        WCHAR sii_name[] = { '$', 'S', 'I', 'I' };
        add_resident_attr(s, rec, AT_INDEX_ROOT, sii_name, 4,
                          idx_buf, sizeof(INDEX_ROOT) + entries_size, 0);
    }

    finalize_record(s, rec);
}

/* Build $UpCase (record 10) */
static void build_upcase(MKNTFS_STATE *s)
{
    FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + 10 * s->mft_record_size);
    ULONGLONG upcase_bytes = (ULONGLONG)s->upcase_len * sizeof(WCHAR);

    s->next_instance = 0;
    init_file_record(rec, s->mft_record_size, s->sector_size, 1, FRH_IN_USE, FILE_UpCase);
    rec->LinkCount = 1;

    WCHAR name[] = { '$', 'U', 'p', 'C', 'a', 's', 'e' };
    add_si_and_fn(s, rec, name, 7, FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM,
                  s->upcase_clusters * s->cluster_size, upcase_bytes);

    add_nonresident_attr(s, rec, AT_DATA, NULL, 0,
                         s->upcase_lcn, s->upcase_clusters,
                         upcase_bytes, upcase_bytes);

    bitmap_set(s->lcn_bitmap, s->upcase_lcn, s->upcase_clusters);

    finalize_record(s, rec);
}

/* Build records 8-15 (excluding 10 which is UpCase) as simple system files */
static void build_remaining_system_files(MKNTFS_STATE *s)
{
    /* $BadClus (8) */
    WCHAR n_badclus[] = { '$', 'B', 'a', 'd', 'C', 'l', 'u', 's' };
    build_simple_system_file(s, FILE_BadClus, n_badclus, 8, FRH_IN_USE);

    /* $Secure (9) - built separately by build_secure() */
    /* $UpCase (10) - built separately by build_upcase() */

    /* $Extend (11) - directory */
    WCHAR n_extend[] = { '$', 'E', 'x', 't', 'e', 'n', 'd' };
    build_simple_system_file(s, FILE_Extend, n_extend, 7, FRH_IN_USE | FRH_DIRECTORY);

    /* Records 12-15: unused but marked with FILE magic */
    ULONG i;
    for (i = 12; i < 16; i++) {
        FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s->mft_buf + i * s->mft_record_size);
        init_file_record(rec, s->mft_record_size, s->sector_size,
                         (USHORT)(i + 1), 0, i); /* Not in use */
        /* Write AT_END only */
        ULONG *end = (ULONG *)((UCHAR *)rec + rec->AttributeOffset);
        *end = AT_END;
        rec->BytesInUse = rec->AttributeOffset + 8;
    }
}

/* ============================================================
 * $LogFile initialization
 * ============================================================ */

/* Pick the log-page size for this volume.  Matches the driver's
 * LfsPageSize in drivers/filesystems/ntfs/lfsparse.c: NTFS uses a 4 KiB
 * log page as long as the cluster size is at most 4 KiB; above that the
 * page size equals the cluster size.  Both RSTR copies plus subsequent
 * RCRD pages are laid out at this stride. */
static ULONG lfs_page_size(const MKNTFS_STATE *s)
{
    return (s->cluster_size > 4096) ? s->cluster_size : 4096;
}

/* Apply the LFS restart-page USA.  Shape identical to apply_fixup above
 * (which is used for FILE / INDX records): for every sector in the page,
 * save the original last-2-bytes into USA[1..N] and stamp the USN into
 * the last 2 bytes.  FixupUpdateSequenceArray in the driver reverses
 * this on read, and the parser's magic check happens after the fixup. */
static void apply_lfs_fixup(UCHAR *page, ULONG page_size, ULONG sector_size,
                             USHORT usa_offset, USHORT usa_count)
{
    USHORT *usa;
    ULONG i, num_sectors;
    USHORT usn;

    usa = (USHORT *)(page + usa_offset);
    usn = usa[0];
    num_sectors = usa_count - 1;
    if (num_sectors > page_size / sector_size)
        num_sectors = page_size / sector_size;

    for (i = 0; i < num_sectors; i++) {
        USHORT *sector_end = (USHORT *)(page + (i + 1) * sector_size - 2);
        usa[1 + i] = *sector_end;       /* Save original (usually 0). */
        *sector_end = usn;              /* Stamp USN sentinel.        */
    }
}

/* Build one spec-compliant CLEAN RSTR restart page.
 *
 * Layout (sized by page_size, typically 4096):
 *
 *   0x00..0x0F   NTFS_RECORD_HEADER
 *                  Type      = 'RSTR'
 *                  UsaOffset = 0x28    (past the restart-page header fields)
 *                  UsaCount  = sectors-per-page + 1
 *                  Lsn       = 0       (fresh volume)
 *   0x10         RestartOffset USHORT (where the NTFS_LFS_RESTART_AREA
 *                sits).  The ReactOS driver parser reads this field.
 *                We put the restart area at 0x40 so it sits past the USA.
 *   0x12         SystemPageSize USHORT  (log page size in bytes)
 *   0x14         LogPageSize USHORT     (log page size in bytes)
 *   0x16         RestartOffset USHORT   (libfsntfs view, matches 0x10 copy)
 *   0x18         MajorVersion USHORT    (1)
 *   0x1A         MinorVersion USHORT    (0)
 *   0x1C..0x27   reserved / alignment
 *   0x28..       USA array (USHORT USN + num_sectors saved words)
 *   0x40..0x6F   NTFS_LFS_RESTART_AREA (CLEAN landmark)
 *   0x70..0xBF   a single LOG_CLIENT_RECORD (zero-filled payload; the
 *                ReactOS driver's Phase-1 gate does not inspect it, but
 *                we leave the ClientFreeList/ClientInUseList chain sane
 *                for a future replayer).
 *   ...          zero-fill out to the last sector
 *   sector ends  USN sentinel stamped by apply_lfs_fixup
 *
 * After the USA is stamped, every sector's last USHORT equals the USN so
 * FixupUpdateSequenceArray succeeds when the driver reads the page back.
 */
static void build_rstr_page(UCHAR *page, ULONG page_size, ULONG sector_size,
                             ULONGLONG file_size)
{
    /* NTFS_RECORD_HEADER field offsets — match driver layout exactly. */
    NTFS_LFS_RESTART_AREA area;
    USHORT usa_offset = 0x28;
    USHORT usa_count  = (USHORT)(page_size / sector_size + 1);
    USHORT restart_offset = 0x40;
    USHORT usn = 1;   /* Any non-zero 16-bit value; driver doesn't care
                       * about the specific USN value on a fresh page. */

    memset(page, 0, page_size);

    /* NTFS_RECORD_HEADER: Type | UsaOffset | UsaCount | Lsn (8 bytes) */
    *(ULONG  *)(page + 0x00) = NRH_RSTR_TYPE;
    *(USHORT *)(page + 0x04) = usa_offset;
    *(USHORT *)(page + 0x06) = usa_count;
    *(ULONGLONG *)(page + 0x08) = 0;              /* Lsn = 0 */

    /* RestartOffset at 0x10 — the field the ReactOS driver parser reads.
     * Keep a mirror at 0x16 so libfsntfs dumpers see the same value. */
    *(USHORT *)(page + 0x10) = restart_offset;
    *(USHORT *)(page + 0x12) = (USHORT)page_size; /* SystemPageSize */
    *(USHORT *)(page + 0x14) = (USHORT)page_size; /* LogPageSize    */
    *(USHORT *)(page + 0x16) = restart_offset;    /* RestartOffset  */
    *(USHORT *)(page + 0x18) = 1;                 /* MajorVersion   */
    *(USHORT *)(page + 0x1A) = 0;                 /* MinorVersion   */

    /* USA: USN at usa[0]; saved-originals at usa[1..num_sectors] are
     * filled in by apply_lfs_fixup below (they're the original last-2-
     * bytes of each sector, which on a zero-page are 0). */
    *(USHORT *)(page + usa_offset) = usn;

    /* Restart area: CLEAN landmark.  CurrentLsn=0, LastLsnDataLength=0,
     * CLEAN flag set.  RestartAreaLength / ClientArrayOffset let a
     * future replayer locate the LOG_CLIENT_RECORD array that starts
     * immediately after.  LogClients=1, ClientFreeList=0 (client 0 is
     * on the free list — no active clients on a fresh volume),
     * ClientInUseList = 0xFFFF (no in-use chain). */
    memset(&area, 0, sizeof(area));
    area.CurrentLsn          = 0;
    area.LogClients          = 1;
    area.ClientFreeList      = 0;
    area.ClientInUseList     = 0xFFFF;
    area.Flags               = NTFS_LFS_RESTART_FLAG_CLEAN;
    area.SeqNumberBits       = 0x2D;
    area.RestartAreaLength   = (USHORT)sizeof(NTFS_LFS_RESTART_AREA);
    area.ClientArrayOffset   = (USHORT)sizeof(NTFS_LFS_RESTART_AREA);
    area.FileSize            = file_size;
    area.LastLsnDataLength   = 0;
    area.RecordHeaderLength  = 0x30;
    area.LogPageDataOffset   = 0x40;
    area.RestartOpenLogCount = 1;
    area.Reserved            = 0;
    memcpy(page + restart_offset, &area, sizeof(area));

    /* Apply the USA sentinel pass: stamps every sector's trailing
     * USHORT with the USN so the driver's FixupUpdateSequenceArray
     * matches on read. */
    apply_lfs_fixup(page, page_size, sector_size, usa_offset, usa_count);
}

/* Write the $LogFile with two byte-identical CLEAN RSTR pages followed
 * by 0xFF-filled tail (empty RCRD region).  The ReactOS driver's Phase-1
 * clean-shutdown gate reads the first two pages and requires:
 *   - both parse (USA ok, RSTR magic, restart-area offset in range)
 *   - both byte-exact equal
 *   - Flags & NTFS_LFS_RESTART_FLAG_CLEAN set
 *   - LastLsnDataLength == 0
 * See drivers/filesystems/ntfs/lfsparse.c:NtfsLfsCheckCleanShutdown. */
static int write_logfile(MKNTFS_STATE *s)
{
    UCHAR *page = NULL;
    UCHAR *tail = NULL;
    ULONG page_size = lfs_page_size(s);
    ULONGLONG offset = s->logfile_lcn * s->cluster_size;
    ULONGLONG remaining;
    ULONG chunk_size = 65536;
    int ret = -1;

    /* Build one CLEAN RSTR page; write it verbatim twice. */
    page = (UCHAR *)malloc(page_size);
    if (!page) goto done;
    build_rstr_page(page, page_size, s->sector_size, s->logfile_size);

    if (io_write(s, offset, page, page_size) < 0) goto done;
    if (io_write(s, offset + page_size, page, page_size) < 0) goto done;

    /* Fill the rest with 0xFF (empty journal marker). */
    tail = (UCHAR *)malloc(chunk_size);
    if (!tail) goto done;
    memset(tail, 0xFF, chunk_size);

    offset += (ULONGLONG)page_size * 2;
    if (s->logfile_size < (ULONGLONG)page_size * 2)
        remaining = 0;
    else
        remaining = s->logfile_size - (ULONGLONG)page_size * 2;

    while (remaining > 0) {
        ULONG to_write = remaining > chunk_size ? chunk_size : (ULONG)remaining;
        if (io_write(s, offset, tail, to_write) < 0) goto done;
        offset += to_write;
        remaining -= to_write;
    }

    ret = 0;

done:
    free(page);
    free(tail);
    return ret;
}

/* ============================================================
 * Main format entry point
 * ============================================================ */

int mkntfs_format(const MKNTFS_IO *io, const MKNTFS_PARAMS *params)
{
    MKNTFS_STATE s;
    ULONGLONG total_bytes;
    int ret = -1;
    ULONG i;
    ULONGLONG next_lcn;

    memset(&s, 0, sizeof(s));
    s.io = io;
    s.now = mkntfs_time();

    /* --- Geometry --- */
    s.sector_size = params->sector_size ? params->sector_size : 512;
    s.total_sectors = params->total_sectors;
    total_bytes = s.total_sectors * s.sector_size;

    s.cluster_size = params->cluster_size ? params->cluster_size : auto_cluster_size(total_bytes);
    if (s.cluster_size < s.sector_size)
        s.cluster_size = s.sector_size;
    s.sectors_per_cluster = s.cluster_size / s.sector_size;
    s.total_clusters = total_bytes / s.cluster_size;

    s.mft_record_size = params->mft_record_size ? params->mft_record_size : 1024;
    s.index_record_size = params->index_record_size ? params->index_record_size : 4096;

    s.label = params->label;
    s.serial_number = params->serial_number;
    if (s.serial_number == 0) {
#if defined(NTOS_MODE_USER)
        {
            LARGE_INTEGER Counter;
            NtQueryPerformanceCounter(&Counter, NULL);
            s.serial_number = (ULONGLONG)Counter.QuadPart;
        }
#else
        srand((unsigned)time(NULL));
        s.serial_number = ((ULONGLONG)rand() << 32) | (ULONGLONG)rand();
#endif
    }

    printf("Formatting NTFS volume:\n");
    printf("  Sector size:    %lu\n", (unsigned long)s.sector_size);
    printf("  Cluster size:   %lu\n", (unsigned long)s.cluster_size);
    printf("  Total sectors:  %llu\n", (unsigned long long)s.total_sectors);
    printf("  Total clusters: %llu\n", (unsigned long long)s.total_clusters);

    /* --- Allocate bitmaps --- */
    s.lcn_bitmap_size = (ULONG)((s.total_clusters + 7) / 8);
    s.lcn_bitmap = (UCHAR *)calloc(1, s.lcn_bitmap_size);
    if (!s.lcn_bitmap) goto cleanup;

    /* MFT: at least 16 records */
    s.mft_records = 16;
    s.mft_size = (ULONGLONG)s.mft_records * s.mft_record_size;
    s.mft_clusters = (s.mft_size + s.cluster_size - 1) / s.cluster_size;
    s.mft_size = s.mft_clusters * s.cluster_size;

    s.mft_bitmap_size = (s.mft_records + 7) / 8;
    s.mft_bitmap = (UCHAR *)calloc(1, s.mft_bitmap_size);
    if (!s.mft_bitmap) goto cleanup;

    /* Mark MFT records 0-15 as reserved in the MFT bitmap.
     * NTFS reserves the first 16 MFT entries for system files.
     * Records 0-11 are populated with system file data;
     * records 12-15 are empty placeholders but must still be
     * marked as used so the driver allocates new files starting
     * at MFT index 16 (NTFS_FILE_FIRST_USER_FILE). */
    for (i = 0; i < 16; i++)
        bitmap_set(s.mft_bitmap, i, 1);

    /* --- Layout: Cluster allocation ---
       Boot | MFT | LogFile | AttrDef | Bitmap | UpCase | RootIdx | ... | MFTMirr(mid) */

    /* $Boot: cluster 0 */
    ULONGLONG boot_clusters = 1;
    if (s.cluster_size < s.sector_size * 16)
        boot_clusters = (s.sector_size * 16 + s.cluster_size - 1) / s.cluster_size;
    bitmap_set(s.lcn_bitmap, 0, boot_clusters);

    /* $MFT */
    s.mft_lcn = boot_clusters;
    bitmap_set(s.lcn_bitmap, s.mft_lcn, s.mft_clusters);
    next_lcn = s.mft_lcn + s.mft_clusters;

    /* $LogFile */
    s.logfile_size = total_bytes / 400;
    if (s.logfile_size < 2 * 1024 * 1024) s.logfile_size = 2 * 1024 * 1024;
    if (s.logfile_size > 64 * 1024 * 1024) s.logfile_size = 64 * 1024 * 1024;
    s.logfile_size = NTFS_ALIGN_UP(s.logfile_size, (ULONGLONG)s.cluster_size);
    s.logfile_clusters = s.logfile_size / s.cluster_size;
    s.logfile_lcn = next_lcn;
    bitmap_set(s.lcn_bitmap, s.logfile_lcn, s.logfile_clusters);
    next_lcn += s.logfile_clusters;

    /* $AttrDef */
    {
        ULONG acount;
        get_attrdef_table(&acount);
        ULONG attrdef_bytes = acount * sizeof(ATTR_DEF);
        s.attrdef_clusters = (attrdef_bytes + s.cluster_size - 1) / s.cluster_size;
        s.attrdef_lcn = next_lcn;
        next_lcn += s.attrdef_clusters;
    }

    /* $Bitmap */
    s.bitmap_clusters = (s.lcn_bitmap_size + s.cluster_size - 1) / s.cluster_size;
    s.bitmap_lcn = next_lcn;
    next_lcn += s.bitmap_clusters;

    /* $UpCase */
    {
        ULONGLONG upcase_bytes = 65536ULL * sizeof(WCHAR);
        s.upcase_clusters = (upcase_bytes + s.cluster_size - 1) / s.cluster_size;
        s.upcase_lcn = next_lcn;
        next_lcn += s.upcase_clusters;
    }

    /* MFT bitmap (1 cluster, non-resident) */
    s.mft_bitmap_lcn = next_lcn;
    bitmap_set(s.lcn_bitmap, s.mft_bitmap_lcn, 1);
    next_lcn += 1;

    /* Root directory INDX block (1 cluster) */
    s.root_idx_lcn = next_lcn;
    bitmap_set(s.lcn_bitmap, s.root_idx_lcn, 1);
    next_lcn += 1;

    /* $MFTMirr at mid-volume */
    ULONGLONG mirr_clusters = (4 * s.mft_record_size + s.cluster_size - 1) / s.cluster_size;
    s.mft_mirr_lcn = s.total_clusters / 2;
    bitmap_set(s.lcn_bitmap, s.mft_mirr_lcn, mirr_clusters);

    printf("  MFT at cluster:     %llu (%llu clusters)\n",
           (unsigned long long)s.mft_lcn, (unsigned long long)s.mft_clusters);
    printf("  LogFile at cluster: %llu (%llu clusters)\n",
           (unsigned long long)s.logfile_lcn, (unsigned long long)s.logfile_clusters);
    printf("  MFTMirr at cluster: %llu\n", (unsigned long long)s.mft_mirr_lcn);

    /* --- Allocate MFT buffer --- */
    s.mft_buf = (UCHAR *)calloc(1, (size_t)s.mft_size);
    if (!s.mft_buf) goto cleanup;

    /* --- Build upcase table --- */
    s.upcase_len = 65536;
    s.upcase = (WCHAR *)calloc(s.upcase_len, sizeof(WCHAR));
    if (!s.upcase) goto cleanup;
    ntfs_upcase_table_build(s.upcase, s.upcase_len);

    /* --- Build all MFT records --- */
    printf("  Building MFT records...\n");
    s.current_security_id = 256; /* Default SD for system files */
    build_mft(&s);
    build_mftmirr(&s);
    build_logfile(&s);
    build_volume(&s);
    build_attrdef(&s);
    build_root(&s);
    build_bitmap(&s);
    build_boot(&s);
    build_remaining_system_files(&s);
    s.current_security_id = 257; /* $Secure gets its own SD */
    build_secure(&s);
    s.current_security_id = 256;
    build_upcase(&s);

    /* --- Apply fixups to all MFT records --- */
    for (i = 0; i < s.mft_records; i++) {
        FILE_RECORD_HEADER *rec = (FILE_RECORD_HEADER *)(s.mft_buf + i * s.mft_record_size);
        if (rec->Magic == NRH_FILE_TYPE)
            apply_fixup(rec, s.mft_record_size, s.sector_size);
    }

    /* --- Write boot sector --- */
    printf("  Writing boot sector...\n");
    {
        NTFS_BOOT_SECTOR bs;
        memset(&bs, 0, sizeof(bs));
        bs.Jump[0] = 0xEB; bs.Jump[1] = 0x52; bs.Jump[2] = 0x90;
        memcpy(bs.OemId, "NTFS    ", 8);
        bs.BytesPerSector = (USHORT)s.sector_size;
        bs.SectorsPerCluster = (UCHAR)s.sectors_per_cluster;
        bs.MediaDescriptor = 0xF8;
        bs.SectorsPerTrack = 63;
        bs.NumberOfHeads = 255;
        bs.HiddenSectors = params->hidden_sectors;
        bs.BiosDriveNumber = 0x80;
        bs.ExtBootSignature = 0x80;
        bs.TotalSectors = s.total_sectors - 1;
        bs.MftStartLcn = s.mft_lcn;
        bs.MftMirrStartLcn = s.mft_mirr_lcn;
        bs.ClustersPerMftRecord = encode_clusters_per_record(s.mft_record_size, s.cluster_size);
        bs.ClustersPerIndexRecord = encode_clusters_per_record(s.index_record_size, s.cluster_size);
        bs.SerialNumber = s.serial_number;
        bs.EndMarker = 0xAA55;

        ret = io_write(&s, 0, &bs, sizeof(bs));
        if (ret < 0) { fprintf(stderr, "Failed to write boot sector\n"); goto cleanup; }

        ULONGLONG backup_offset = (s.total_sectors - 1) * s.sector_size;
        io_write(&s, backup_offset, &bs, sizeof(bs));
    }

    /* --- Write $LogFile --- */
    printf("  Writing $LogFile...\n");
    if (write_logfile(&s) < 0) {
        fprintf(stderr, "Failed to write $LogFile\n");
        goto cleanup;
    }

    /* --- Write MFT --- */
    printf("  Writing MFT...\n");
    ret = io_write(&s, s.mft_lcn * s.cluster_size, s.mft_buf, (ULONG)s.mft_size);
    if (ret < 0) { fprintf(stderr, "Failed to write MFT\n"); goto cleanup; }

    /* --- Write MFT Mirror --- */
    printf("  Writing MFT mirror...\n");
    ret = io_write(&s, s.mft_mirr_lcn * s.cluster_size, s.mft_buf, 4 * s.mft_record_size);
    if (ret < 0) { fprintf(stderr, "Failed to write MFT mirror\n"); goto cleanup; }

    /* --- Write MFT $Bitmap --- */
    printf("  Writing MFT bitmap...\n");
    {
        UCHAR *mft_bmp_buf = (UCHAR *)calloc(1, s.cluster_size);
        if (!mft_bmp_buf) goto cleanup;
        memcpy(mft_bmp_buf, s.mft_bitmap, s.mft_bitmap_size);
        ret = io_write(&s, s.mft_bitmap_lcn * s.cluster_size, mft_bmp_buf, s.cluster_size);
        free(mft_bmp_buf);
        if (ret < 0) { fprintf(stderr, "Failed to write MFT bitmap\n"); goto cleanup; }
    }

    /* --- Write $Bitmap --- */
    printf("  Writing $Bitmap...\n");
    {
        ULONG write_size = (ULONG)(s.bitmap_clusters * s.cluster_size);
        UCHAR *bmp_buf = (UCHAR *)calloc(1, write_size);
        if (!bmp_buf) goto cleanup;
        memcpy(bmp_buf, s.lcn_bitmap, s.lcn_bitmap_size);
        ret = io_write(&s, s.bitmap_lcn * s.cluster_size, bmp_buf, write_size);
        free(bmp_buf);
        if (ret < 0) { fprintf(stderr, "Failed to write bitmap\n"); goto cleanup; }
    }

    /* --- Write $AttrDef --- */
    printf("  Writing $AttrDef...\n");
    {
        ULONG acount;
        const ATTR_DEF *atable = get_attrdef_table(&acount);
        ret = io_write(&s, s.attrdef_lcn * s.cluster_size, atable, acount * sizeof(ATTR_DEF));
        if (ret < 0) { fprintf(stderr, "Failed to write $AttrDef\n"); goto cleanup; }
    }

    /* --- Write $UpCase --- */
    printf("  Writing $UpCase...\n");
    ret = io_write(&s, s.upcase_lcn * s.cluster_size, s.upcase, s.upcase_len * sizeof(WCHAR));
    if (ret < 0) { fprintf(stderr, "Failed to write $UpCase\n"); goto cleanup; }

    /* --- Write root directory INDX block --- */
    printf("  Writing root directory index...\n");
    if (write_root_index(&s) < 0) {
        fprintf(stderr, "Failed to write root index\n");
        goto cleanup;
    }

    /* --- Flush --- */
    if (s.io->flush)
        s.io->flush(s.io->context);

    printf("Format complete.\n");
    ret = 0;

cleanup:
    free(s.lcn_bitmap);
    free(s.mft_bitmap);
    free(s.mft_buf);
    free(s.upcase);
    return ret;
}
