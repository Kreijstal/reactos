/* NTFS 1.1 clean restart page builder (GPL-2.0-or-later). */
#ifndef NTFS_RESTART_H
#define NTFS_RESTART_H

/* CurrentLsn: the last LSN the log used; the driver's next one follows
 * it, so LSNs stamped on metadata keep increasing. */
int NtfsBuildCleanRestart(UCHAR *Page, ULONG PageSize, ULONG SectorSize,
                          ULONGLONG FileSize, ULONGLONG CurrentLsn);

#endif
