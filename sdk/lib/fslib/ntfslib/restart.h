/* NTFS 1.1 clean restart page builder (GPL-2.0-or-later). */
#ifndef NTFS_RESTART_H
#define NTFS_RESTART_H

int NtfsBuildCleanRestart(UCHAR *Page, ULONG PageSize, ULONG SectorSize,
                          ULONGLONG FileSize);

#endif
