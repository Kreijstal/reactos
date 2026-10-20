/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     Log File Service disk layouts
 */
#ifndef NTFS_LFS_H
#define NTFS_LFS_H

#include "ntfs.h"

#define NTFS_LOG_RESTART_MAGIC 0x52545352
#define NTFS_LOG_CLEAN 0x0002
#define NTFS_LOG_MAX_PAGE 0x10000

#include <pshpack1.h>
/* 0x1E bytes: Windows puts the update sequence array right after it, at
 * 0x1E (ntfs-3g layout.h RESTART_PAGE_HEADER, libfsntfs 14.1). */
typedef struct _NTFS_LOG_RESTART_HEADER
{
    NTFS_RECORD_HEADER Header;
    ULONG SystemPageSize;
    ULONG LogPageSize;
    USHORT RestartOffset;
    USHORT MinorVersion;
    USHORT MajorVersion;
} NTFS_LOG_RESTART_HEADER, *PNTFS_LOG_RESTART_HEADER;

typedef struct _NTFS_LOG_RESTART_AREA
{
    ULONGLONG CurrentLsn;
    USHORT LogClients;
    USHORT ClientFreeList;
    USHORT ClientInUseList;
    USHORT Flags;
    ULONG SeqNumberBits;
    USHORT RestartAreaLength;
    USHORT ClientArrayOffset;
    ULONGLONG FileSize;
    ULONG LastLsnDataLength;
    USHORT RecordHeaderLength;
    USHORT LogPageDataOffset;
    ULONG RestartOpenLogCount;
    ULONG Reserved;
} NTFS_LOG_RESTART_AREA, *PNTFS_LOG_RESTART_AREA;
#include <poppack.h>

C_ASSERT(FIELD_OFFSET(NTFS_LOG_RESTART_HEADER, SystemPageSize) == 0x10);
C_ASSERT(FIELD_OFFSET(NTFS_LOG_RESTART_HEADER, RestartOffset) == 0x18);
C_ASSERT(FIELD_OFFSET(NTFS_LOG_RESTART_HEADER, MajorVersion) == 0x1c);
C_ASSERT(sizeof(NTFS_LOG_RESTART_HEADER) == 0x1e);
C_ASSERT(sizeof(NTFS_LOG_RESTART_AREA) == 0x30);

#endif
