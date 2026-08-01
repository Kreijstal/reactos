/*
 * NTFS on-disk structure definitions for winmkntfs.
 * Based on ReactOS NTFS driver headers and NTFS documentation.
 * License: GPL-2.0-or-later
 */
#ifndef NTFS_TYPES_H
#define NTFS_TYPES_H

#if defined(NTOS_MODE_USER) || defined(NTOS_KERNEL_MODE)
/* ReactOS NDK environment - include umtypes.h for base types */
#include <ndk/umtypes.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <stdint.h>
#include <string.h>
typedef uint8_t UCHAR, BYTE, *PUCHAR;
typedef int8_t CHAR;
typedef uint16_t USHORT, WCHAR, WORD, *PUSHORT;
typedef uint32_t ULONG, DWORD, *PULONG;
typedef uint64_t ULONGLONG, DWORD64;
typedef int64_t LONGLONG;
typedef int32_t LONG;
typedef int BOOL;
typedef void *HANDLE;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;
#endif

#pragma pack(push, 1)

/* ============================================================
 * Boot Sector
 * ============================================================ */

typedef struct _NTFS_BOOT_SECTOR {
    UCHAR  Jump[3];                 /* 0x00: JMP + NOP */
    UCHAR  OemId[8];               /* 0x03: "NTFS    " */
    USHORT BytesPerSector;          /* 0x0B */
    UCHAR  SectorsPerCluster;       /* 0x0D */
    UCHAR  Unused0[7];             /* 0x0E: reserved */
    UCHAR  MediaDescriptor;        /* 0x15 */
    UCHAR  Unused1[2];             /* 0x16 */
    USHORT SectorsPerTrack;        /* 0x18 */
    USHORT NumberOfHeads;          /* 0x1A */
    ULONG  HiddenSectors;         /* 0x1C */
    UCHAR  Unused2[4];            /* 0x20 */
    UCHAR  BiosDriveNumber;       /* 0x24: BIOS drive (0x80 = first HD) */
    UCHAR  Reserved24;            /* 0x25 */
    UCHAR  ExtBootSignature;      /* 0x26: 0x80 */
    UCHAR  Reserved27;            /* 0x27 */
    ULONGLONG TotalSectors;        /* 0x28 */
    ULONGLONG MftStartLcn;         /* 0x30: MFT cluster */
    ULONGLONG MftMirrStartLcn;     /* 0x38: MFTMirr cluster */
    CHAR   ClustersPerMftRecord;   /* 0x40: signed, negative = 2^|n| bytes */
    UCHAR  Unused4[3];            /* 0x41 */
    CHAR   ClustersPerIndexRecord; /* 0x44: signed, negative = 2^|n| bytes */
    UCHAR  Unused5[3];            /* 0x45 */
    ULONGLONG SerialNumber;        /* 0x48 */
    ULONG  Checksum;              /* 0x50 */
    UCHAR  BootCode[426];         /* 0x54 */
    USHORT EndMarker;             /* 0x1FE: 0xAA55 */
} NTFS_BOOT_SECTOR;

/* ============================================================
 * MFT Record Header
 * ============================================================ */

#define NRH_FILE_TYPE   0x454C4946  /* 'FILE' */
#define NRH_INDX_TYPE   0x58444E49  /* 'INDX' */
#define NRH_RSTR_TYPE   0x52545352  /* 'RSTR' - $LogFile restart page */
#define NRH_RCRD_TYPE   0x44524352  /* 'RCRD' - $LogFile record page  */

/* FILE_RECORD_HEADER flags */
#define FRH_IN_USE      0x0001
#define FRH_DIRECTORY   0x0002

/* ============================================================
 * $LogFile RSTR restart-page layout (for mkntfs)
 *
 * Layout authority: libfsntfs (Apache/LGPL), cross-checked against the
 * ReactOS driver's NtfsLfsParseRestartPage in lfsparse.c.  Field order
 * mirrors drivers/filesystems/ntfs/ntfs.h:NTFS_LFS_RESTART_AREA one-for-
 * one; we duplicate here because ntfslib compiles without driver headers.
 * ============================================================ */

typedef struct _NTFS_LFS_RESTART_AREA {
    ULONGLONG CurrentLsn;               /* 0x00 - last LSN ever written     */
    USHORT    LogClients;               /* 0x08 - count of client records   */
    USHORT    ClientFreeList;           /* 0x0A - head of free client chain */
    USHORT    ClientInUseList;          /* 0x0C - head of in-use chain      */
    USHORT    Flags;                    /* 0x0E - restart flags             */
    ULONG     SeqNumberBits;            /* 0x10 - bits for the seq number   */
    USHORT    RestartAreaLength;        /* 0x14 - byte length of this area  */
    USHORT    ClientArrayOffset;        /* 0x16 - offset to first LogClient */
    ULONGLONG FileSize;                 /* 0x18 - size of $LogFile:$DATA    */
    ULONG     LastLsnDataLength;        /* 0x20 - LastLSN payload length    */
    USHORT    RecordHeaderLength;       /* 0x24 - len of LFS_RECORD header  */
    USHORT    LogPageDataOffset;        /* 0x26 - first byte of client data */
    ULONG     RestartOpenLogCount;      /* 0x28 - open counter              */
    ULONG     Reserved;                 /* 0x2C - must be zero              */
} NTFS_LFS_RESTART_AREA;

#define NTFS_LFS_RESTART_FLAG_CLEAN 0x0002  /* Dismounted cleanly           */

typedef struct _FILE_RECORD_HEADER {
    ULONG     Magic;                 /* 'FILE' */
    USHORT    UpdateSeqOffset;       /* Offset to update sequence array */
    USHORT    UpdateSeqSize;         /* Size in words (inc. USN) */
    ULONGLONG Lsn;                   /* $LogFile sequence number */
    USHORT    SequenceNumber;
    USHORT    LinkCount;
    USHORT    AttributeOffset;       /* Offset to first attribute */
    USHORT    Flags;                 /* FRH_IN_USE, FRH_DIRECTORY */
    ULONG     BytesInUse;            /* Real size of record */
    ULONG     BytesAllocated;        /* Allocated size */
    ULONGLONG BaseFileRecord;
    USHORT    NextAttributeId;
    USHORT    Padding;
    ULONG     MftRecordNumber;
} FILE_RECORD_HEADER;

/* ============================================================
 * Attribute Types
 * ============================================================ */

#define AT_STANDARD_INFORMATION 0x10
#define AT_ATTRIBUTE_LIST       0x20
#define AT_FILE_NAME            0x30
#define AT_OBJECT_ID            0x40
#define AT_SECURITY_DESCRIPTOR  0x50
#define AT_VOLUME_NAME          0x60
#define AT_VOLUME_INFORMATION   0x70
#define AT_DATA                 0x80
#define AT_INDEX_ROOT           0x90
#define AT_INDEX_ALLOCATION     0xA0
#define AT_BITMAP               0xB0
#define AT_EA_INFORMATION       0xD0
#define AT_EA                   0xE0
#define AT_END                  0xFFFFFFFF

/* ============================================================
 * Attribute List entry (value of $ATTRIBUTE_LIST, 0x20)
 * ============================================================ */

typedef struct _ATTR_LIST_ENTRY {
    ULONG     Type;              /* attribute type code                     */
    USHORT    Length;            /* byte length of this list entry          */
    UCHAR     NameLength;        /* in WCHARs                               */
    UCHAR     NameOffset;        /* from start of this entry                */
    ULONGLONG LowestVcn;         /* starting VCN of this attribute extent   */
    ULONGLONG MftReference;      /* record holding the extent (incl. seq)   */
    USHORT    Instance;          /* attribute instance in that record       */
    /* WCHAR Name[] follows at NameOffset */
} ATTR_LIST_ENTRY;

/* ============================================================
 * Attribute Record Header
 * ============================================================ */

typedef struct _ATTR_RECORD {
    ULONG  Type;
    ULONG  Length;                   /* Total length of this attribute */
    UCHAR  NonResident;
    UCHAR  NameLength;              /* In WCHARs */
    USHORT NameOffset;
    USHORT Flags;
    USHORT Instance;
    union {
        struct {  /* Resident */
            ULONG  ValueLength;
            USHORT ValueOffset;
            UCHAR  ResidentFlags;   /* 0x01 = indexed */
            UCHAR  Reserved;
        } Resident;
        struct {  /* Non-resident */
            ULONGLONG LowestVcn;
            ULONGLONG HighestVcn;
            USHORT    MappingPairsOffset;
            USHORT    CompressionUnit;
            UCHAR     Reserved[4];
            LONGLONG  AllocatedSize;
            LONGLONG  DataSize;
            LONGLONG  InitializedSize;
            LONGLONG  CompressedSize;
        } NR;
    };
} ATTR_RECORD;

/* ============================================================
 * Standard Information (0x10)
 * ============================================================ */

typedef struct _STANDARD_INFORMATION {
    ULONGLONG CreationTime;
    ULONGLONG ModificationTime;
    ULONGLONG MftModificationTime;
    ULONGLONG AccessTime;
    ULONG     FileAttributes;
    ULONG     MaxVersions;
    ULONG     VersionNumber;
    ULONG     ClassId;
    /* NTFS 3.0+ fields */
    ULONG     OwnerId;
    ULONG     SecurityId;
    ULONGLONG QuotaCharged;
    ULONGLONG Usn;
} STANDARD_INFORMATION;

/* ============================================================
 * File Name (0x30)
 * ============================================================ */

#define FILE_NAME_POSIX         0
#define FILE_NAME_WIN32         1
#define FILE_NAME_DOS           2
#define FILE_NAME_WIN32_AND_DOS 3

typedef struct _FILE_NAME_ATTR {
    ULONGLONG ParentDirectory;      /* MFT ref of parent */
    ULONGLONG CreationTime;
    ULONGLONG ModificationTime;
    ULONGLONG MftModificationTime;
    ULONGLONG AccessTime;
    ULONGLONG AllocatedSize;
    ULONGLONG DataSize;
    ULONG     FileAttributes;
    union {
        struct {
            USHORT PackedEaSize;
            USHORT Reserved;
        } EaInfo;
        ULONG ReparseTag;
    } Extended;
    UCHAR     FileNameLength;       /* In WCHARs */
    UCHAR     FileNameType;
    WCHAR     FileName[1];          /* Variable length */
} FILE_NAME_ATTR;

/* ============================================================
 * Volume Information (0x70)
 * ============================================================ */

typedef struct _VOLUME_INFORMATION {
    ULONGLONG Reserved;
    UCHAR     MajorVersion;
    UCHAR     MinorVersion;
    USHORT    Flags;
} VOLUME_INFORMATION;

/* The field offsets above are right, but this header is not byte-packed, so
 * the trailing padding after the leading ULONGLONG makes sizeof() 16 while the
 * on-disk value is 12.  Length-check against this, never against sizeof(). */
#define VOLUME_INFORMATION_SIZE 12

/* Volume flags */
#ifndef VOLUME_IS_DIRTY
#define VOLUME_IS_DIRTY         0x0001
#endif
#define VOLUME_RESIZE_LOG_FILE  0x0002
#define VOLUME_UPGRADE_ON_MOUNT 0x0004
#define VOLUME_MOUNTED_ON_NT4   0x0008
#define VOLUME_DELETE_USN_UNDERWAY 0x0010
#define VOLUME_REPAIR_OBJECT_ID 0x0020
#define VOLUME_MODIFIED_BY_CHKDSK 0x8000

/* ============================================================
 * Index structures (0x90, 0xA0, 0xB0)
 * ============================================================ */

typedef struct _INDEX_HEADER {
    ULONG EntriesOffset;            /* Relative to this header */
    ULONG IndexLength;              /* Total size of entries + header */
    ULONG AllocatedSize;
    UCHAR Flags;                    /* 0 = small, 1 = large (has sub-nodes) */
    UCHAR Padding[3];
} INDEX_HEADER;

typedef struct _INDEX_ROOT {
    ULONG AttributeType;            /* Usually AT_FILE_NAME (0x30) */
    ULONG CollationRule;
    ULONG IndexBlockSize;           /* Bytes per index record */
    UCHAR ClustersPerIndexBlock;
    UCHAR Padding[3];
    INDEX_HEADER Header;
} INDEX_ROOT;

/* INDEX_ENTRY flags */
#define INDEX_ENTRY_NODE    0x01    /* Has sub-node */
#define INDEX_ENTRY_END     0x02    /* Last entry in list */

typedef struct _INDEX_ENTRY {
    union {
        struct { ULONGLONG IndexedFile; } Directory;
        struct { USHORT DataOffset; USHORT DataLength; ULONG Reserved; } ViewIndex;
    } Data;
    USHORT Length;                   /* Total length of this entry */
    USHORT KeyLength;               /* Length of key data */
    USHORT Flags;
    USHORT Reserved;
    /* Key data follows (e.g., FILE_NAME_ATTR for $I30) */
} INDEX_ENTRY;

/* ============================================================
 * Index Buffer (INDX record)
 * ============================================================ */

typedef struct _INDEX_BUFFER {
    ULONG     Magic;                /* 'INDX' */
    USHORT    UpdateSeqOffset;
    USHORT    UpdateSeqSize;
    ULONGLONG Lsn;
    ULONGLONG Vcn;
    INDEX_HEADER Header;
} INDEX_BUFFER;

/* ============================================================
 * Well-known MFT record numbers
 * ============================================================ */

#define FILE_MFT        0
#define FILE_MFTMirr    1
#define FILE_LogFile    2
#define FILE_Volume     3
#define FILE_AttrDef    4
#define FILE_Root       5
#define FILE_Bitmap     6
#define FILE_Boot       7
#define FILE_BadClus    8
#define FILE_Secure     9
#define FILE_UpCase     10
#define FILE_Extend     11
#define FILE_FIRST_USER 16

/* ============================================================
 * System file attributes
 * ============================================================ */

#define FILE_ATTR_READONLY           0x00000001
#define FILE_ATTR_HIDDEN             0x00000002
#define FILE_ATTR_SYSTEM             0x00000004
#define FILE_ATTR_ARCHIVE            0x00000020
#define FILE_ATTR_NORMAL             0x00000080
#define FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT 0x10000000
#define FILE_ATTR_DUP_VIEW_INDEX_PRESENT      0x20000000

/* ============================================================
 * Attribute Definition Entry (for $AttrDef)
 * ============================================================ */

typedef struct _ATTR_DEF {
    WCHAR  Name[64];
    ULONG  Type;
    ULONG  DisplayRule;
    ULONG  CollationRule;
    ULONG  Flags;
    ULONGLONG MinSize;
    ULONGLONG MaxSize;
} ATTR_DEF;

/* Attr def flags */
#define ATTR_DEF_INDEXABLE   0x02
#define ATTR_DEF_MUST_BE_NAMED 0x04
#define ATTR_DEF_MUST_BE_RESIDENT 0x10
#define ATTR_DEF_LOG_NONRESIDENT 0x40

/* ============================================================
 * Security Descriptor Header ($Secure)
 * ============================================================ */

typedef struct _SECURITY_DESCRIPTOR_HEADER {
    ULONG     Hash;
    ULONG     SecurityId;
    ULONGLONG Offset;
    ULONG     Length;
} SECURITY_DESCRIPTOR_HEADER;

#pragma pack(pop)

/* Alignment helper -- use NTFS_ALIGN_UP to avoid conflict with the ReactOS SDK
 * ALIGN_UP(size, type) macro which interprets the second argument as a type
 * (taking sizeof), not a numeric alignment value. */
#define NTFS_ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

#endif /* NTFS_TYPES_H */
