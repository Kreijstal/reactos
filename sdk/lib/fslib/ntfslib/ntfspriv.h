/*
 * ntfslib - shared on-disk structure builders
 * License: GPL-2.0-or-later
 *
 * Construction helpers whose bodies live in mkntfs.c, shared with the
 * chkdsk repair engine (chkrepair.c).  All operate on caller-provided
 * buffers and carry no formatter state:
 *   - the attribute builders take the record's instance counter and the
 *     volume cluster size explicitly instead of MKNTFS_STATE
 *   - ntfs_make_file_name takes the complete parent MFT reference
 *     (record | seq << 48) and the name type explicitly
 */
#ifndef NTFS_PRIV_H
#define NTFS_PRIV_H

#include "ntfs_types.h"

/* Encode one data run (length, lcn delta); returns bytes written. */
int ntfs_encode_run(UCHAR *dst, ULONGLONG length, LONGLONG lcn_offset);

/* MFT reference: record number | (seq << 48). */
ULONGLONG ntfs_mft_ref(ULONG record, USHORT seq);

/* Initialize a blank FILE record header (incl. update sequence array). */
void ntfs_init_file_record(FILE_RECORD_HEADER *rec, ULONG rec_size,
                           ULONG sector_size, USHORT seq_num, USHORT flags,
                           ULONG mft_num);

/* Fill a FILE_NAME_ATTR; parent_ref must be a complete MFT reference. */
void ntfs_make_file_name(FILE_NAME_ATTR *fn, ULONGLONG parent_ref,
                         const WCHAR *name, UCHAR name_len, UCHAR name_type,
                         ULONG file_attrs, ULONGLONG alloc_size,
                         ULONGLONG data_size, ULONGLONG time_val);

/* Build an INDEX_ENTRY keyed by a fabricated FILE_NAME (root system files);
 * returns the aligned entry size. */
ULONG ntfs_make_index_entry(UCHAR *buf, ULONG mft_num, USHORT seq,
                            const WCHAR *name, UCHAR name_len,
                            ULONG file_attrs, ULONGLONG time_val);

/* Append a resident attribute to a FILE record; returns pointer past it. */
UCHAR *ntfs_add_resident_attr(USHORT *next_instance, FILE_RECORD_HEADER *rec,
                              ULONG type, const WCHAR *name, ULONG name_len,
                              const void *value, ULONG value_len,
                              UCHAR resident_flags);

/* Append a non-resident attribute with a single-run mapping. */
UCHAR *ntfs_add_nonresident_attr(USHORT *next_instance, ULONG cluster_size,
                                 FILE_RECORD_HEADER *rec,
                                 ULONG type, const WCHAR *name, ULONG name_len,
                                 ULONGLONG start_lcn, ULONGLONG num_clusters,
                                 ULONGLONG data_size, ULONGLONG initialized_size);

/* Build one spec-compliant CLEAN 'RSTR' $LogFile restart page into the
 * caller's page buffer (page_size bytes, USA already applied).  This is the
 * exact "empty, dismounted-cleanly" landmark mkntfs writes at format time and
 * that the driver's NtfsLfsCheckCleanShutdown accepts; the chkdsk repair
 * engine reuses it to stamp $LogFile clean after a repair (H8). */
void ntfs_build_rstr_page(UCHAR *page, ULONG page_size, ULONG sector_size,
                          ULONGLONG file_size);

#endif /* NTFS_PRIV_H */
