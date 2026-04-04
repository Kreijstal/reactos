/*
 * winmkntfs - Portable NTFS formatter
 * Public API header
 * License: GPL-2.0-or-later
 */
#ifndef MKNTFS_H
#define MKNTFS_H

#include "ntfs_types.h"

/* Format options */
typedef struct _MKNTFS_PARAMS {
    ULONGLONG total_sectors;     /* Total sectors on volume */
    ULONG     sector_size;       /* Bytes per sector (default 512) */
    ULONG     cluster_size;      /* Bytes per cluster (0 = auto) */
    ULONG     mft_record_size;   /* Bytes per MFT record (default 1024) */
    ULONG     index_record_size; /* Bytes per index record (default 4096) */
    const WCHAR *label;          /* Volume label (NULL = no label) */
    ULONGLONG serial_number;     /* 0 = auto-generate */
    int       quick_format;      /* Skip zeroing */
} MKNTFS_PARAMS;

/* I/O callbacks - the formatter calls these */
typedef struct _MKNTFS_IO {
    void *context;               /* Opaque handle (e.g., file descriptor or HANDLE) */
    int (*write)(void *ctx, ULONGLONG offset, const void *buf, ULONG length);
    int (*read)(void *ctx, ULONGLONG offset, void *buf, ULONG length);
    int (*flush)(void *ctx);
} MKNTFS_IO;

/*
 * Format an NTFS volume.
 * Returns 0 on success, negative on error.
 */
int mkntfs_format(const MKNTFS_IO *io, const MKNTFS_PARAMS *params);

#endif /* MKNTFS_H */
