/*
 * ntfslib - shared on-disk structure builders
 * License: GPL-2.0-or-later
 *
 * Construction helpers whose bodies live in chkutil.c, shared with the
 * chkdsk repair engine (chkrepair.c).  All operate on caller-provided
 * buffers and carry no formatter state:
 *   - the attribute builders take the record's instance counter and the
 *     volume cluster size explicitly instead of MKNTFS_STATE
 *   - NtfsChkBuildFileName takes the complete parent MFT reference
 *     (record | seq << 48) and the name type explicitly
 */
#ifndef NTFS_PRIV_H
#define NTFS_PRIV_H

#include "ntfs_types.h"

/* Encode one data run (length, lcn delta); returns bytes written. */
int NtfsChkEncodeRun(UCHAR *dst, ULONGLONG length, LONGLONG lcn_offset);

/* MFT reference: record number | (seq << 48). */
ULONGLONG NtfsChkMftReference(ULONG record, USHORT seq);

/* Initialize a blank FILE record header (incl. update sequence array). */
void NtfsChkInitializeRecord(FILE_RECORD_HEADER *rec, ULONG rec_size,
                           ULONG sector_size, USHORT seq_num, USHORT flags,
                           ULONG mft_num);

/* Fill a FILE_NAME_ATTR; parent_ref must be a complete MFT reference. */
void NtfsChkBuildFileName(FILE_NAME_ATTR *fn, ULONGLONG parent_ref,
                         const WCHAR *name, UCHAR name_len, UCHAR name_type,
                         ULONG file_attrs, ULONGLONG alloc_size,
                         ULONGLONG data_size, ULONGLONG time_val);

/* Append a resident attribute to a FILE record; returns pointer past it. */
UCHAR *NtfsChkAddResidentAttribute(USHORT *next_instance, FILE_RECORD_HEADER *rec,
                              ULONG type, const WCHAR *name, ULONG name_len,
                              const void *value, ULONG value_len,
                              UCHAR resident_flags);

/* Append a non-resident attribute with a single-run mapping. */
UCHAR *NtfsChkAddNonresidentAttribute(USHORT *next_instance, ULONG cluster_size,
                                 FILE_RECORD_HEADER *rec,
                                 ULONG type, const WCHAR *name, ULONG name_len,
                                 ULONGLONG start_lcn, ULONGLONG num_clusters,
                                 ULONGLONG data_size, ULONGLONG initialized_size);

void NtfsChkBuildUpcase(WCHAR *Table, ULONG Entries);

#include "restart.h"

#endif /* NTFS_PRIV_H */
