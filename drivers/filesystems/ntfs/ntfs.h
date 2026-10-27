#ifndef NTFS_H
#define NTFS_H

#include <ntifs.h>
#include <pseh/pseh2.h>
#include <section_attribs.h>

#if (NTDDI_VERSION < NTDDI_VISTA)
/* Vista+ names this driver implements whatever the build's target version is. */
#define FileHardLinkInformation ((FILE_INFORMATION_CLASS)46)

NTKERNELAPI
VOID
NTAPI
FsRtlNotifyCleanupAll(
    _In_ PNOTIFY_SYNC NotifySync,
    _In_ PLIST_ENTRY NotifyList);
#endif

#define CACHEPAGESIZE(pDeviceExt) \
	((pDeviceExt)->NtfsInfo.UCHARsPerCluster > PAGE_SIZE ? \
	 (pDeviceExt)->NtfsInfo.UCHARsPerCluster : PAGE_SIZE)

#define TAG_NTFS '0ftN'
#define TAG_CCB 'CftN'
#define TAG_FCB 'FftN'
#define TAG_IRP_CTXT 'iftN'
#define TAG_ATT_CTXT 'aftN'
#define TAG_FILE_REC 'rftN'
#define TAG_IO_RUNS 'RftN'

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))
#define ROUND_DOWN(N, S) ((N) - ((N) % (S)))

#define DEVICE_NAME L"\\Ntfs"

#include <pshpack1.h>
typedef struct _BIOS_PARAMETERS_BLOCK
{
    USHORT    BytesPerSector;			// 0x0B
    UCHAR     SectorsPerCluster;		// 0x0D
    UCHAR     Unused0[7];				// 0x0E, checked when volume is mounted
    UCHAR     MediaId;				// 0x15
    UCHAR     Unused1[2];				// 0x16
    USHORT    SectorsPerTrack;		// 0x18
    USHORT    Heads;					// 0x1A
    UCHAR     Unused2[4];				// 0x1C
    UCHAR     Unused3[4];				// 0x20, checked when volume is mounted
} BIOS_PARAMETERS_BLOCK, *PBIOS_PARAMETERS_BLOCK;

typedef struct _EXTENDED_BIOS_PARAMETERS_BLOCK
{
    USHORT    Unknown[2];				// 0x24, always 80 00 80 00
    ULONGLONG SectorCount;			// 0x28
    ULONGLONG MftLocation;			// 0x30
    ULONGLONG MftMirrLocation;		// 0x38
    CHAR      ClustersPerMftRecord;	// 0x40
    UCHAR     Unused4[3];				// 0x41
    CHAR      ClustersPerIndexRecord; // 0x44
    UCHAR     Unused5[3];				// 0x45
    ULONGLONG SerialNumber;			// 0x48
    UCHAR     Checksum[4];			// 0x50
} EXTENDED_BIOS_PARAMETERS_BLOCK, *PEXTENDED_BIOS_PARAMETERS_BLOCK;

typedef struct _BOOT_SECTOR
{
    UCHAR     Jump[3];				// 0x00
    UCHAR     OEMID[8];				// 0x03
    BIOS_PARAMETERS_BLOCK BPB;
    EXTENDED_BIOS_PARAMETERS_BLOCK EBPB;
    UCHAR     BootStrap[426];			// 0x54
    USHORT    EndSector;				// 0x1FE
} BOOT_SECTOR, *PBOOT_SECTOR;
#include <poppack.h>

/* Largest NTFS cluster (Windows 10 1709 and later format up to 2 MiB). */
#define NTFS_MAX_CLUSTER_SIZE (2 * 1024 * 1024)

/* The BPB's SectorsPerCluster: the count itself up to 0x80, above that
 * 256 - log2(count) (clusters over 64 KiB, 0xF4 = 4096 sectors). 0 for a
 * value no NTFS writes. */
FORCEINLINE
ULONG
NtfsDecodeSectorsPerCluster(UCHAR Value)
{
    if (Value <= 0x80)
        return Value;
    if (Value >= 0xF4)
        return 1UL << (256 - Value);
    return 0;
}

//typedef struct _BootSector BootSector;

typedef struct _NTFS_INFO
{
    ULONG BytesPerSector;
    ULONG SectorsPerCluster;
    ULONG BytesPerCluster;
    ULONGLONG SectorCount;
    ULONGLONG ClusterCount;
    ULARGE_INTEGER MftStart;
    ULARGE_INTEGER MftMirrStart;
    ULONG BytesPerFileRecord;
    ULONG BytesPerIndexRecord;

    ULONGLONG SerialNumber;
    USHORT VolumeLabelLength;
    WCHAR VolumeLabel[MAXIMUM_VOLUME_LABEL_LENGTH];
    UCHAR MajorVersion;
    UCHAR MinorVersion;
    USHORT Flags;

    ULONG MftZoneReservation;
} NTFS_INFO, *PNTFS_INFO;

#define NTFS_TYPE_CCB         '20SF'
#define NTFS_TYPE_FCB         '30SF'
#define NTFS_TYPE_VCB         '50SF'
#define NTFS_TYPE_IRP_CONTEXT '60SF'
#define NTFS_TYPE_GLOBAL_DATA '70SF'

typedef struct
{
    ULONG Type;
    ULONG Size;
} NTFSIDENTIFIER, *PNTFSIDENTIFIER;

typedef struct
{
    NTFSIDENTIFIER Identifier;

    /* Removal drains ordinary requests first, then paging requests after
     * making the removed state visible. Paging must remain available while
     * the first phase flushes Cc. Cleanup and close use DirResource instead. */
    ERESOURCE IoResource;
    ERESOURCE PagingResource;
    ERESOURCE DirResource;
    /* Slot allocation/growth never acquires DirResource. The context lock
     * protects the shared $MFT mapping, after physical cluster allocation. */
    ERESOURCE MftAllocationResource;
    ERESOURCE MftContextResource;
//    ERESOURCE FatResource;

    /* Serializes every read-modify-write of a directory's file record and
     * index. Taken after any FCB resource, and before BitmapResource. */
    ERESOURCE IndexResource;

    /* In-memory copy of $Bitmap, loaded at mount. BitmapResource guards
     * the copy and keeps it in step with the disk. */
    ERESOURCE BitmapResource;
    struct _NTFS_ATTR_CONTEXT* BitmapContext;
    PULONG BitmapBuffer;
    ULONG BitmapSize;
    RTL_BITMAP ClusterBitmap;
    ULONGLONG FreeClusterCount;
    BOOLEAN DeferVolumeBitmapWrites; /* only during extend, with BitmapResource exclusive */

    KSPIN_LOCK FcbListLock;
    LIST_ENTRY FcbListHead;
    LIST_ENTRY FileLockList;
    PLIST_ENTRY FcbHashBuckets;
    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY NotifyList;

    PVPB Vpb;
    PDEVICE_OBJECT StorageDevice;
    PFILE_OBJECT StreamFileObject;

    struct _NTFS_ATTR_CONTEXT* MFTContext;
    struct _FILE_RECORD_HEADER* MasterFileTable;
    struct _FCB *VolumeFcb;

    NTFS_INFO NtfsInfo;

    /* $Extend\$ObjId, whose $O indexes the object IDs; 0 on a volume without one */
    ULONGLONG ObjectIdIndexMftIndex;

    /* $Secure, 0 on a volume before NTFS 3.0, and where its next descriptor goes.
     * IndexResource guards the two. */
    ULONGLONG SecureMftIndex;
    ULONG NextSecurityId;
    ULONGLONG SdsEnd;

    /* The change journal, $Extend\$UsnJrnl; 0 while there is none. UsnResource guards
     * these and the journal's records. Taken after FCB resources, before IndexResource. */
    ERESOURCE UsnResource;
    ULONGLONG UsnJournalMftIndex;
    ULONGLONG UsnJournalId;
    ULONGLONG UsnMaximumSize;
    ULONGLONG UsnAllocationDelta;
    USN UsnLowestValidUsn;
    USN UsnFirstUsn;
    USN UsnNextUsn;

    /* $Extend\$Quota, 0 while there is none, and the owner ID the next new owner gets.
     * IndexResource guards the latter. */
    ULONGLONG QuotaMftIndex;
    ULONG NextQuotaOwnerId;
    /* Ordinary mutations take shared ownership while tracking is disabled,
     * exclusive ownership while enabled. Taken before directory/FCB resources;
     * paging I/O cannot change a stream's logical length and bypasses it. */
    ERESOURCE QuotaResource;
    BOOLEAN QuotaTracking;
    BOOLEAN QuotaRebuildRequired;

    NPAGED_LOOKASIDE_LIST FileRecLookasideList;

    /* $UpCase, one upcased code point per UCS-2 value. NTFS defines its
     * own case folding through this file, so comparisons must use it
     * rather than the Rtl table. */
    PWCHAR UpCaseTable;

    ULONG MftDataOffset;
    ULONG Flags;
    ULONG OpenHandleCount;
    /* External file objects, including those retained by MM after cleanup.
     * DirResource serializes attachment and close. Internal streams do not
     * count, and deleting a pathname does not hide a live file object. */
    /* Restart inspection is read-only; emission is disabled by default. */
    BOOLEAN LogFileDirty;
    BOOLEAN LogFileVersionUnsupported;
    USHORT LogFileMajorVersion;
    USHORT LogFileMinorVersion;
    ULONGLONG LogFileLsn;
    PVOID LfsWriter;

    ULONG FileObjectCount;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION, NTFS_VCB, *PNTFS_VCB;

#define VCB_VOLUME_LOCKED        0x0001
/* Volume dismounted: everything but raw volume access is refused */
#define VCB_VOLUME_DISMOUNTED    0x0002
#define VCB_QUERY_REMOVE_PENDING 0x0004
#define VCB_DEVICE_REMOVED       0x0008
#define VCB_VOLUME_READ_ONLY     0x0010
#define VCB_VOLUME_CORRUPT       0x0020

/* A final file-object cleanup can run attached to the system process when
 * closing a kernel handle. Retain every process that submitted locks through
 * this open, so cleanup releases this open's locks in all process contexts. */
typedef struct _NTFS_LOCK_OWNER
{
    struct _NTFS_LOCK_OWNER *Next;
    struct _EPROCESS *Process;
} NTFS_LOCK_OWNER, *PNTFS_LOCK_OWNER;

typedef struct
{
    NTFSIDENTIFIER Identifier;
    LIST_ENTRY     NextCCB;
    PFILE_OBJECT   PtrFileObject;
    PNTFS_LOCK_OWNER LockOwners;
    LARGE_INTEGER  CurrentByteOffset;
    /* for DirectoryControl */
    ULONG Entry;
    /* for DirectoryControl */
    PWCHAR DirectorySearchPattern;
    ULONG LastCluster;
    ULONG LastOffset;
    ULONG Flags;
    /* IRP_MJ_QUERY_EA: one-based index of the EA the next query returns, 0 before the first */
    ULONG EaIndex;
    /* IRP_MJ_QUERY_QUOTA: the owner ID an enumeration goes on from */
    ULONG QuotaOwnerId;
} NTFS_CCB, *PNTFS_CCB;

/* NTFS has no "." or ".." in its indices, so directory enumeration makes them
 * up. These record which of the two a scan has already handed out. */
#define CCB_RETURNED_DOT        0x0001
#define CCB_RETURNED_DOTDOT     0x0002
/* Opened with FILE_DELETE_ON_CLOSE: the file is marked for deletion when this
 * handle is cleaned up */
#define CCB_DELETE_ON_CLOSE     0x0004
/* The open is counted in its FCB's and the volume's open handle counts, which the
 * cleanup of the handle takes back */
#define CCB_COUNTED             0x0008

/* This file object owns one FCB reference, independently of handle counts. */
#define CCB_FCB_REFERENCE       0x0010
#define CCB_EXTERNAL_FILE_OBJECT 0x0020

/* MS-FSA UserSet*Time state belongs to the open, not the FCB. */
#define CCB_USER_SET_WRITE_TIME  0x0040
#define CCB_USER_SET_CHANGE_TIME 0x0080
#define CCB_USER_SET_ACCESS_TIME 0x0100
#define CCB_UPDATE_CHANGE_TIME   0x0200
#define CCB_MANAGE_VOLUME       0x0400

typedef struct
{
    NTFSIDENTIFIER Identifier;
    ERESOURCE Resource;
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT DeviceObject;
    CACHE_MANAGER_CALLBACKS CacheMgrCallbacks;
    ULONG Flags;
    FAST_IO_DISPATCH FastIoDispatch;
    NPAGED_LOOKASIDE_LIST IrpContextLookasideList;
    NPAGED_LOOKASIDE_LIST FcbLookasideList;
    NPAGED_LOOKASIDE_LIST AttrCtxtLookasideList;
    BOOLEAN EnableWriteSupport;
    BOOLEAN ShutdownStarted;
} NTFS_GLOBAL_DATA, *PNTFS_GLOBAL_DATA;


typedef enum
{
    AttributeStandardInformation = 0x10,
    AttributeAttributeList = 0x20,
    AttributeFileName = 0x30,
    AttributeObjectId = 0x40,
    AttributeSecurityDescriptor = 0x50,
    AttributeVolumeName = 0x60,
    AttributeVolumeInformation = 0x70,
    AttributeData = 0x80,
    AttributeIndexRoot = 0x90,
    AttributeIndexAllocation = 0xA0,
    AttributeBitmap = 0xB0,
    AttributeReparsePoint = 0xC0,
    AttributeEAInformation = 0xD0,
    AttributeEA = 0xE0,
    AttributePropertySet = 0xF0,
    AttributeLoggedUtilityStream = 0x100,
    AttributeEnd = 0xFFFFFFFF
} ATTRIBUTE_TYPE, *PATTRIBUTE_TYPE;

// FILE_RECORD_END seems to follow AttributeEnd in every file record starting with $Quota.
// No clue what data is being represented here.
#define FILE_RECORD_END              0x11477982

#define NTFS_FILE_MFT                0
#define NTFS_FILE_MFTMIRR            1
#define NTFS_FILE_LOGFILE            2
#define NTFS_FILE_VOLUME            3
#define NTFS_FILE_ATTRDEF            4
#define NTFS_FILE_ROOT                5
#define NTFS_FILE_BITMAP            6
#define NTFS_FILE_BOOT                7
#define NTFS_FILE_BADCLUS            8
#define NTFS_FILE_QUOTA                9
#define NTFS_FILE_SECURE            9   /* NTFS 3.0 and later */
#define NTFS_FILE_UPCASE            10
#define NTFS_FILE_EXTEND            11
#define NTFS_FILE_FIRST_USER_FILE   16

/* Bounds on allocation headroom for growing data and directory indexes. */
#define NTFS_GROW_AHEAD_MIN (64 * 1024)
#define NTFS_GROW_AHEAD_MAX (4 * 1024 * 1024)

#define NTFS_MFT_MASK 0x0000FFFFFFFFFFFFULL

#define COLLATION_BINARY              0x00
#define COLLATION_FILE_NAME           0x01
#define COLLATION_UNICODE_STRING      0x02
#define COLLATION_NTOFS_ULONG         0x10
#define COLLATION_NTOFS_SID           0x11
#define COLLATION_NTOFS_SECURITY_HASH 0x12
#define COLLATION_NTOFS_ULONGS        0x13

#define INDEX_ROOT_SMALL 0x0
#define INDEX_ROOT_LARGE 0x1

#define INDEX_NODE_SMALL 0x0
#define INDEX_NODE_LARGE 0x1

#define NTFS_INDEX_ENTRY_NODE            1
#define NTFS_INDEX_ENTRY_END            2

#define NTFS_FILE_NAME_POSIX            0
#define NTFS_FILE_NAME_WIN32            1
#define NTFS_FILE_NAME_DOS            2
#define NTFS_FILE_NAME_WIN32_AND_DOS        3

#define NTFS_FILE_TYPE_READ_ONLY  0x1
#define NTFS_FILE_TYPE_HIDDEN     0x2
#define NTFS_FILE_TYPE_SYSTEM     0x4
#define NTFS_FILE_TYPE_ARCHIVE    0x20
#define NTFS_FILE_TYPE_TEMPORARY  0x100
#define NTFS_FILE_TYPE_SPARSE     0x200
#define NTFS_FILE_TYPE_REPARSE    0x400
#define NTFS_FILE_TYPE_COMPRESSED 0x800
#define NTFS_FILE_TYPE_OFFLINE    0x1000
#define NTFS_FILE_TYPE_ENCRYPTED  0x4000
#define NTFS_FILE_TYPE_DIRECTORY  0x10000000

/* NTFS_ATTR_RECORD Flags */
#define NTFS_ATTR_COMPRESSION_MASK 0x00FF
#define NTFS_ATTR_SPARSE           0x8000
#define NTFS_ATTR_COMPRESSED_LZNT1 0x0001
/* A sparse stream is allocated in units of 1 << NTFS_SPARSE_UNIT_SHIFT clusters, like Windows' */
#define NTFS_SPARSE_UNIT_SHIFT     4

/* Indexed Flag in Resident attributes - still somewhat speculative */
#define RA_INDEXED    0x01

typedef struct
{
    ULONG Type;             /* Magic number 'FILE' */
    USHORT UsaOffset;       /* Offset to the update sequence */
    USHORT UsaCount;        /* Size in words of Update Sequence Number & Array (S) */
    ULONGLONG Lsn;          /* $LogFile Sequence Number (LSN) */
} NTFS_RECORD_HEADER, *PNTFS_RECORD_HEADER;

/* NTFS_RECORD_HEADER.Type */
#define NRH_FILE_TYPE  0x454C4946  /* 'FILE' */
#define NRH_INDX_TYPE  0x58444E49  /* 'INDX' */


typedef struct _FILE_RECORD_HEADER
{
    NTFS_RECORD_HEADER Ntfs;
    USHORT SequenceNumber;        /* Sequence number */
    USHORT LinkCount;             /* Hard link count */
    USHORT AttributeOffset;       /* Offset to the first Attribute */
    USHORT Flags;                 /* Flags */
    ULONG BytesInUse;             /* Real size of the FILE record */
    ULONG BytesAllocated;         /* Allocated size of the FILE record */
    ULONGLONG BaseFileRecord;     /* File reference to the base FILE record */
    USHORT NextAttributeNumber;   /* Next Attribute Id */
    USHORT Padding;               /* Align to 4 UCHAR boundary (XP) */
    ULONG MFTRecordNumber;        /* Number of this MFT Record (XP) */
} FILE_RECORD_HEADER, *PFILE_RECORD_HEADER;

/* Flags in FILE_RECORD_HEADER */

#define FRH_IN_USE    0x0001    /* Record is in use */
#define FRH_DIRECTORY 0x0002    /* Record is a directory */
#define FRH_UNKNOWN1  0x0004    /* Don't know */
#define FRH_UNKNOWN2  0x0008    /* Don't know */

typedef struct
{
    ULONG        Type;
    ULONG        Length;
    UCHAR        IsNonResident;
    UCHAR        NameLength;
    USHORT        NameOffset;
    USHORT        Flags;
    USHORT        Instance;
    union
    {
        // Resident attributes
        struct
        {
            ULONG        ValueLength;
            USHORT        ValueOffset;
            UCHAR        Flags;
            UCHAR        Reserved;
        } Resident;
        // Non-resident attributes
        struct
        {
            ULONGLONG        LowestVCN;
            ULONGLONG        HighestVCN;
            USHORT        MappingPairsOffset;
            USHORT        CompressionUnit;
            UCHAR        Reserved[4];
            LONGLONG        AllocatedSize;
            LONGLONG        DataSize;
            LONGLONG        InitializedSize;
            LONGLONG        CompressedSize;
        } NonResident;
    };
} NTFS_ATTR_RECORD, *PNTFS_ATTR_RECORD;

typedef struct
{
    ULONG Type;
    USHORT Length;
    UCHAR NameLength;
    UCHAR NameOffset;
    ULONGLONG StartingVCN;
    ULONGLONG MFTIndex;
    USHORT Instance;
} NTFS_ATTRIBUTE_LIST_ITEM, *PNTFS_ATTRIBUTE_LIST_ITEM;

// The beginning and length of an attribute record are always aligned to an 8-byte boundary,
// relative to the beginning of the file record.
#define ATTR_RECORD_ALIGNMENT 8

// Data runs are aligned to a 4-byte boundary, relative to the start of the attribute record
#define DATA_RUN_ALIGNMENT  4

// Value offset is aligned to a 4-byte boundary, relative to the start of the attribute record
#define VALUE_OFFSET_ALIGNMENT  4

typedef struct
{
    ULONGLONG CreationTime;
    ULONGLONG LastWriteTime;
    ULONGLONG ChangeTime;
    ULONGLONG LastAccessTime;
    ULONG FileAttribute;
    ULONG AlignmentOrReserved[3];
    /* NTFS 3.0 and later: a record written by earlier versions ends here */
    ULONG QuotaId;
    ULONG SecurityId;
    ULONGLONG QuotaCharge;
    LONGLONG Usn;
} STANDARD_INFORMATION, *PSTANDARD_INFORMATION;

/* The $STANDARD_INFORMATION of NTFS 1.x and 2.x */
#define NTFS_STANDARD_INFORMATION_V1_SIZE FIELD_OFFSET(STANDARD_INFORMATION, QuotaId)


typedef struct
{
    ATTRIBUTE_TYPE AttributeType;
    USHORT Length;
    UCHAR NameLength;
    UCHAR NameOffset;
    ULONGLONG StartVcn; // LowVcn
    ULONGLONG FileReferenceNumber;
    USHORT AttributeNumber;
    USHORT AlignmentOrReserved[3];
} ATTRIBUTE_LIST, *PATTRIBUTE_LIST;


typedef struct
{
    ULONGLONG DirectoryFileReferenceNumber;
    ULONGLONG CreationTime;
    ULONGLONG LastWriteTime;
    ULONGLONG ChangeTime;
    ULONGLONG LastAccessTime;
    ULONGLONG AllocatedSize;
    ULONGLONG DataSize;
    ULONG FileAttributes;
    union
    {
        struct
        {
            USHORT PackedEaSize;
            USHORT AlignmentOrReserved;
        } EaInfo;
        ULONG ReparseTag;
    } Extended;
    UCHAR NameLength;
    UCHAR NameType;
    WCHAR Name[1];
} FILENAME_ATTRIBUTE, *PFILENAME_ATTRIBUTE;

typedef struct
{
    ULONG FirstEntryOffset;
    ULONG TotalSizeOfEntries;
    ULONG AllocatedSize;
    UCHAR Flags;
    UCHAR Padding[3];
} INDEX_HEADER_ATTRIBUTE, *PINDEX_HEADER_ATTRIBUTE;

typedef struct
{
    ULONG AttributeType;
    ULONG CollationRule;
    ULONG SizeOfEntry;
    UCHAR ClustersPerIndexRecord;
    UCHAR Padding[3];
    INDEX_HEADER_ATTRIBUTE Header;
} INDEX_ROOT_ATTRIBUTE, *PINDEX_ROOT_ATTRIBUTE;

typedef struct
{
    NTFS_RECORD_HEADER Ntfs;
    ULONGLONG VCN;
    INDEX_HEADER_ATTRIBUTE Header;
} INDEX_BUFFER, *PINDEX_BUFFER;

typedef struct
{
    union
    {
        struct
        {
            ULONGLONG    IndexedFile;
        } Directory;
        struct
        {
            USHORT    DataOffset;
            USHORT    DataLength;
            ULONG    Reserved;
        } ViewIndex;
    } Data;
    USHORT            Length;
    USHORT            KeyLength;
    USHORT            Flags;
    USHORT            Reserved;
    FILENAME_ATTRIBUTE    FileName;
} INDEX_ENTRY_ATTRIBUTE, *PINDEX_ENTRY_ATTRIBUTE;

struct _B_TREE_FILENAME_NODE;
typedef struct _B_TREE_FILENAME_NODE B_TREE_FILENAME_NODE;

// Keys are arranged in nodes as an ordered, linked list
typedef struct _B_TREE_KEY
{
    struct _B_TREE_KEY *NextKey;
    B_TREE_FILENAME_NODE *LesserChild;  // Child-Node. All the keys in this node will be sorted before IndexEntry
    PINDEX_ENTRY_ATTRIBUTE IndexEntry;  // must be last member for FIELD_OFFSET
}B_TREE_KEY, *PB_TREE_KEY;

// Every Node is just an ordered list of keys.
// Sub-nodes can be found attached to a key (if they exist).
// A key's sub-node precedes that key in the ordered list.
typedef struct _B_TREE_FILENAME_NODE
{
    ULONG KeyCount;
    BOOLEAN HasValidVCN;
    BOOLEAN DiskNeedsUpdating;
    ULONGLONG VCN;
    PB_TREE_KEY FirstKey;
} B_TREE_FILENAME_NODE, *PB_TREE_FILENAME_NODE;

/* Only the root node is read when a tree is built from an index. A key whose
 * entry has NTFS_INDEX_ENTRY_NODE set but no LesserChild has its child still
 * on disk, at the VCN its entry records; NtfsLoadChildNode() reads it from
 * IndexAllocationContext on the first descent. */
#define NTFS_MAX_INDEX_NAME_LENGTH 8

/* A tree describes one index of a file record: $I30 of a directory, or a view
 * index such as $O of $Extend\$ObjId. Keys are ordered by CollationRule. */
typedef struct
{
    PB_TREE_FILENAME_NODE RootNode;
    PDEVICE_EXTENSION Vcb;
    struct _NTFS_ATTR_CONTEXT *IndexAllocationContext;
    ULONG IndexedAttributeType;
    ULONG CollationRule;
    ULONG IndexNameLength;
    WCHAR IndexName[NTFS_MAX_INDEX_NAME_LENGTH];
} B_TREE, *PB_TREE;

typedef struct
{
    ULONGLONG Unknown1;
    UCHAR MajorVersion;
    UCHAR MinorVersion;
    USHORT Flags;
    ULONG Unknown2;
} VOLINFO_ATTRIBUTE, *PVOLINFO_ATTRIBUTE;

typedef struct {
    ULONG ReparseTag;
    USHORT DataLength;
    USHORT Reserved;
    UCHAR Data[1];
} REPARSE_POINT_ATTRIBUTE, *PREPARSE_POINT_ATTRIBUTE;

#define IRPCONTEXT_CANWAIT 0x1
#define IRPCONTEXT_COMPLETE 0x2
#define IRPCONTEXT_QUEUE 0x4

/* A run with no backing storage: zeroed on read, rejected on write */
#define NTFS_SPARSE_LBO ((LONGLONG)-1)

/* How many runs a request may span before the run list spills into pool. */
#define NTFS_MAX_IO_RUNS_ON_STACK 8

/* One physically contiguous piece of a transfer. Runs tile the caller's buffer:
 * Runs[i].BufferOffset + Runs[i].ByteCount == Runs[i + 1].BufferOffset. */
typedef struct _NTFS_IO_RUN
{
    LONGLONG Lbo;           /* Byte offset on the volume, or NTFS_SPARSE_LBO */
    ULONG BufferOffset;     /* Byte offset into the caller's buffer */
    ULONG ByteCount;
    PIRP SavedIrp;          /* The IRP issued for this run; owned by blockdev.c */
} NTFS_IO_RUN, *PNTFS_IO_RUN;

/* Small requests stay on the caller's stack; fragmented ones spill into pool */
typedef struct _NTFS_IO_RUN_LIST
{
    PNTFS_IO_RUN Runs;
    ULONG Count;
    ULONG Capacity;
    ULONG TotalLength;
    NTFS_IO_RUN StackRuns[NTFS_MAX_IO_RUNS_ON_STACK];
} NTFS_IO_RUN_LIST, *PNTFS_IO_RUN_LIST;

/* Shared between the issuing thread and every run's completion routine. The
 * issuer waits on SyncEvent, which the last completing run sets. */
typedef struct _NTFS_IO_CONTEXT
{
    volatile LONG IrpCount;         /* Runs still in flight */
    volatile LONG Status;           /* First error seen, else STATUS_SUCCESS */
    volatile LONG BytesTransferred;

    /* Covers the whole transfer; every run cuts a partial MDL from it. Either
     * borrowed from the caller's IRP or built here; we only lock and release
     * it in the latter case. */
    PMDL MasterMdl;
    PVOID MasterVa;                 /* Base address MasterMdl describes */
    BOOLEAN OwnsMasterMdl;

    KEVENT SyncEvent;
} NTFS_IO_CONTEXT, *PNTFS_IO_CONTEXT;

typedef struct
{
    NTFSIDENTIFIER Identifier;
    ULONG Flags;
    PIO_STACK_LOCATION Stack;
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    WORK_QUEUE_ITEM WorkQueueItem;
    PIRP Irp;
    BOOLEAN IsTopLevel;
    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT FileObject;
    NTSTATUS SavedExceptionCode;
    CCHAR PriorityBoost;
} NTFS_IRP_CONTEXT, *PNTFS_IRP_CONTEXT;

typedef struct _NTFS_ATTR_CONTEXT
{
    PUCHAR            CacheRun;
    ULONGLONG            CacheRunOffset;
    LONGLONG            CacheRunStartLCN;
    ULONGLONG            CacheRunLength;
    LONGLONG            CacheRunLastLCN;
    ULONGLONG            CacheRunCurrentOffset;
    LARGE_MCB           DataRunsMCB;
    ULONGLONG           FileMFTIndex;
    ULONGLONG           FileOwnerMFTIndex; /* If attribute list attribute, reference the original file */
    PNTFS_ATTR_RECORD    pRecord;
    /* Transfer to and from the disk directly, not through the volume
     * stream: a paging file's I/O, which must not need cache pages */
    BOOLEAN             DirectIo;
} NTFS_ATTR_CONTEXT, *PNTFS_ATTR_CONTEXT;

#define FCB_CACHE_INITIALIZED   0x0001
#define FCB_IS_VOLUME_STREAM    0x0002
#define FCB_IS_VOLUME           0x0004
#define FCB_DELETE_PENDING      0x0008
#define FCB_DELETED             0x0010
#define FCB_IS_PAGING_FILE      0x0020
#define MAX_PATH                260

/* Longest name a $FILE_NAME can hold, in characters */
#define NTFS_MAX_NAME_LENGTH    255

/* Size of a buffer that holds any $FILE_NAME: fixed part and longest name */
#define NTFS_FOUND_NAME_SIZE \
    (FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + NTFS_MAX_NAME_LENGTH * sizeof(WCHAR))

/* Longest path a UNICODE_STRING can hold, in characters, with its terminator */
#define NTFS_MAX_PATH_LENGTH    (MAXUSHORT / sizeof(WCHAR))

/* Byte locks belong to a data stream, independently of which hard-link path
 * produced its FCB. FcbListLock guards this list and its reference counts. */
typedef struct _NTFS_FILE_LOCK
{
    LIST_ENTRY ListEntry;
    LIST_ENTRY FcbList;
    LONG References;
    BOOLEAN Removed;
    BOOLEAN MultipleFcbs; /* sticky until this shared stream state is released */
    BOOLEAN DeletePending; /* last name removed; retain data until the final close */
    ULONGLONG FileReference;
    WCHAR Stream[MAX_PATH];
    PNTFS_VCB Vcb;
    ERESOURCE Resource;
    FILE_LOCK Lock;
} NTFS_FILE_LOCK, *PNTFS_FILE_LOCK;

typedef struct _FCB
{
    /* Cc and FsRtl access this header through FileObject->FsContext. */
    FSRTL_COMMON_FCB_HEADER RFCB;
    NTFSIDENTIFIER Identifier;
    SECTION_OBJECT_POINTERS SectionObjectPointers;

    PFILE_OBJECT FileObject;
    PNTFS_VCB Vcb;

    WCHAR Stream[MAX_PATH];
    WCHAR *ObjectName;		/* point on filename (250 chars max) in PathName */
    PWCHAR PathName;		/* path+filename, NUL-terminated, from pool (TAG_NTFS) */
    UNICODE_STRING NotifyName; /* stable string descriptor; DirResource guards its buffer */

    ERESOURCE PagingIoResource;
    ERESOURCE MainResource;

    LIST_ENTRY FcbListEntry;
    LIST_ENTRY FcbHashEntry;
    ULONG PathNameHash;
    struct _FCB* ParentFcb;

    ULONG DirIndex;

    LONG RefCount;
    ULONG Flags;
    ULONG OpenHandleCount;
    SHARE_ACCESS ShareAccess;
    PNTFS_FILE_LOCK FileLock;
    LIST_ENTRY FileLockListEntry;

    ULONGLONG MFTIndex;
    ULONGLONG ParentMFTIndex;  /* The directory holding the name the FCB was opened by */
    USHORT SequenceNumber;  /* Of the file record; with MFTIndex, the file reference */
    USHORT LinkCount;

    /* Path FCBs cannot retain a private data view across changes to the
     * hard-link set. The table lock protects generation increments. */
    ULONG CacheGeneration;
    ULONG DataCacheGeneration;

    /* USN_REASON_* the change journal recorded since the last close; Vcb->UsnResource */
    ULONG UsnReasons;

    FILENAME_ATTRIBUTE Entry;

} NTFS_FCB, *PNTFS_FCB;

C_ASSERT(FIELD_OFFSET(NTFS_FCB, RFCB) == 0);

typedef struct _FIND_ATTR_CONTXT
{
    PDEVICE_EXTENSION Vcb;
    BOOLEAN OnlyResident;
    PNTFS_ATTR_RECORD FirstAttr;
    PNTFS_ATTR_RECORD CurrAttr;
    PNTFS_ATTR_RECORD LastAttr;
    PNTFS_ATTRIBUTE_LIST_ITEM NonResidentStart;
    PNTFS_ATTRIBUTE_LIST_ITEM NonResidentEnd;
    PNTFS_ATTRIBUTE_LIST_ITEM NonResidentCur;
    ULONG Offset;
} FIND_ATTR_CONTXT, *PFIND_ATTR_CONTXT;

typedef struct
{
    USHORT USN;
    USHORT Array[];
} FIXUP_ARRAY, *PFIXUP_ARRAY;

extern PNTFS_GLOBAL_DATA NtfsGlobalData;

FORCEINLINE
NTSTATUS
NtfsMarkIrpContextForQueue(PNTFS_IRP_CONTEXT IrpContext)
{
    PULONG Flags = &IrpContext->Flags;

    *Flags &= ~IRPCONTEXT_COMPLETE;
    *Flags |= IRPCONTEXT_QUEUE;

    return STATUS_PENDING;
}

/* NTFS's case folding: the volume's $UpCase once it's loaded */
FORCEINLINE
WCHAR
NtfsUpcaseChar(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ WCHAR Char)
{
    return (Vcb != NULL && Vcb->UpCaseTable != NULL) ? Vcb->UpCaseTable[Char] : RtlUpcaseUnicodeChar(Char);
}

/* attrib.c */

//VOID
//NtfsDumpAttribute(PATTRIBUTE Attribute);

NTSTATUS
AddBitmap(PNTFS_VCB Vcb,
          PFILE_RECORD_HEADER FileRecord,
          PCWSTR Name,
          USHORT NameLength);

NTSTATUS
AddData(PFILE_RECORD_HEADER FileRecord,
        PNTFS_ATTR_RECORD AttributeAddress);

NTSTATUS
AddRun(PNTFS_VCB Vcb,
       PNTFS_ATTR_CONTEXT AttrContext,
       ULONG AttrOffset,
       PFILE_RECORD_HEADER FileRecord,
       ULONGLONG NextAssignedCluster,
       ULONG RunLength);

NTSTATUS
AddIndexAllocation(PNTFS_VCB Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   PCWSTR Name,
                   USHORT NameLength);

NTSTATUS
AddIndexRoot(PNTFS_VCB Vcb,
             PFILE_RECORD_HEADER FileRecord,
             PNTFS_ATTR_RECORD AttributeAddress,
             PINDEX_ROOT_ATTRIBUTE NewIndexRoot,
             ULONG RootLength,
             PCWSTR Name,
             USHORT NameLength);

BOOLEAN
NtfsNameHasStreamPart(PCUNICODE_STRING Name);

NTSTATUS
AddResidentAttribute(PNTFS_VCB Vcb,
                     PFILE_RECORD_HEADER FileRecord,
                     ULONG Type,
                     PCWSTR Name,
                     USHORT NameLength,
                     const VOID *Data,
                     ULONG DataLength);

VOID
NtfsRemoveAttribute(PFILE_RECORD_HEADER FileRecord,
                    ULONG AttrOffset);

PNTFS_ATTR_RECORD
NtfsBuildFileNameAttribute(PFILENAME_ATTRIBUTE Value,
                           ULONG ValueLength);

NTSTATUS
NtfsInsertAttributeRecord(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER FileRecord,
                          PNTFS_ATTR_RECORD Attribute,
                          PUSHORT Instance);

BOOLEAN
NtfsRecordHasAttributeList(PFILE_RECORD_HEADER Record);

NTSTATUS
NtfsReadAttributeList(PNTFS_VCB Vcb,
                      PFILE_RECORD_HEADER BaseRecord,
                      PNTFS_ATTRIBUTE_LIST_ITEM *List,
                      PULONG Length);

NTSTATUS
NtfsGetExtensionRecords(PNTFS_ATTRIBUTE_LIST_ITEM List,
                        ULONG Length,
                        ULONGLONG BaseIndex,
                        PULONGLONG *Indices,
                        PULONG Count);

NTSTATUS
NtfsRebuildAttributeList(PNTFS_VCB Vcb,
                         PFILE_RECORD_HEADER BaseRecord,
                         PULONGLONG NewIndices,
                         ULONG NewCount);

NTSTATUS
NtfsKeepFileNameInBase(PNTFS_VCB Vcb,
                       PFILE_RECORD_HEADER BaseRecord);

NTSTATUS
NtfsAddAttributeToFile(PNTFS_VCB Vcb,
                       PFILE_RECORD_HEADER BaseRecord,
                       PNTFS_ATTR_RECORD Attribute);

NTSTATUS
NtfsAddResidentAttributeToFile(PNTFS_VCB Vcb,
                               PFILE_RECORD_HEADER BaseRecord,
                               ULONG Type,
                               PCWSTR Name,
                               USHORT NameLength,
                               const VOID *Data,
                               ULONG DataLength);

NTSTATUS
NtfsReadAttributeHolder(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT AttrContext,
                        PFILE_RECORD_HEADER BaseRecord,
                        PFILE_RECORD_HEADER *Holder);

NTSTATUS
NtfsCommitAttributeContext(PDEVICE_EXTENSION Vcb,
                           PNTFS_ATTR_CONTEXT Context,
                           PFILE_RECORD_HEADER Record);

NTSTATUS
NtfsResolveAttributeHolder(PNTFS_VCB Vcb,
                           PNTFS_ATTR_CONTEXT Context,
                           PFILE_RECORD_HEADER Record,
                           PFILE_RECORD_HEADER *Holder,
                           PULONG Offset);

VOID
NtfsReleaseAttributeHolder(PNTFS_VCB Vcb,
                           PFILE_RECORD_HEADER BaseRecord,
                           PFILE_RECORD_HEADER Holder);

NTSTATUS
AddFileName(PFILE_RECORD_HEADER FileRecord,
            PNTFS_ATTR_RECORD AttributeAddress,
            ULONGLONG ParentReference,
            PCUNICODE_STRING Name,
            BOOLEAN CaseSensitive,
            ULONG FileAttributes);

NTSTATUS
AddStandardInformation(PFILE_RECORD_HEADER FileRecord,
                       PNTFS_ATTR_RECORD AttributeAddress,
                       ULONG FileAttributes);

NTSTATUS
ConvertDataRunsToLargeMCB(PUCHAR DataRun,
                          PUCHAR DataRunEnd,
                          PLARGE_MCB DataRunsMCB,
                          PULONGLONG pNextVBN);

NTSTATUS
ConvertLargeMCBToDataRuns(PLARGE_MCB DataRunsMCB,
                          ULONGLONG VcnCount,
                          PUCHAR RunBuffer,
                          ULONG MaxBufferSize,
                          PULONG UsedBufferSize);

PUCHAR
DecodeRun(PUCHAR DataRun,
          LONGLONG *DataRunOffset,
          ULONGLONG *DataRunLength,
          PBOOLEAN IsSparse);

ULONG GetFileNameAttributeLength(PFILENAME_ATTRIBUTE FileNameAttribute);

VOID
NtfsDumpDataRuns(PVOID StartOfRun,
                 ULONGLONG CurrentLCN);

VOID
NtfsDumpFileAttributes(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER FileRecord);

PSTANDARD_INFORMATION
GetStandardInformationFromRecord(PDEVICE_EXTENSION Vcb,
                                 PFILE_RECORD_HEADER FileRecord);

PFILENAME_ATTRIBUTE
GetFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                      PFILE_RECORD_HEADER FileRecord,
                      UCHAR NameType);

PFILENAME_ATTRIBUTE
NtfsGetDosNameFromRecord(PDEVICE_EXTENSION Vcb,
                         PFILE_RECORD_HEADER FileRecord,
                         PFILENAME_ATTRIBUTE LongName);

UCHAR
GetPackedByteCount(LONGLONG NumberToPack,
                   BOOLEAN IsSigned);

NTSTATUS
GetLastClusterInDataRun(PDEVICE_EXTENSION Vcb,
                        PNTFS_ATTR_RECORD Attribute,
                        PULONGLONG LastCluster);

BOOLEAN
NtfsHasNtfs3StandardInformation(PDEVICE_EXTENSION Vcb,
                                PFILE_RECORD_HEADER FileRecord);

PFILENAME_ATTRIBUTE
GetBestFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                          PFILE_RECORD_HEADER FileRecord);

NTSTATUS
FindFirstAttributeListItem(PFIND_ATTR_CONTXT Context,
                           PNTFS_ATTRIBUTE_LIST_ITEM *Item);

NTSTATUS
FindNextAttributeListItem(PFIND_ATTR_CONTXT Context,
                          PNTFS_ATTRIBUTE_LIST_ITEM *Item);

NTSTATUS
FindFirstAttribute(PFIND_ATTR_CONTXT Context,
                   PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   BOOLEAN OnlyResident,
                   PNTFS_ATTR_RECORD * Attribute);

NTSTATUS
FindNextAttribute(PFIND_ATTR_CONTXT Context,
                  PNTFS_ATTR_RECORD * Attribute);

VOID
FindCloseAttribute(PFIND_ATTR_CONTXT Context);

NTSTATUS
NtfsStoreMappingPairs(PNTFS_VCB Vcb,
                      PNTFS_ATTR_CONTEXT AttrContext,
                      ULONG AttrOffset,
                      PFILE_RECORD_HEADER FileRecord,
                      ULONGLONG VcnCount);

NTSTATUS
NtfsSetAttributeUnits(PNTFS_VCB Vcb,
                      PNTFS_ATTR_CONTEXT AttrContext,
                      ULONG AttrOffset,
                      PFILE_RECORD_HEADER FileRecord,
                      USHORT Flag);

NTSTATUS
NtfsClearAttributeUnits(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT AttrContext,
                        ULONG AttrOffset,
                        PFILE_RECORD_HEADER FileRecord,
                        USHORT Flag);

NTSTATUS
NtfsMakeAttributeSparse(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT AttrContext,
                        ULONG AttrOffset,
                        PFILE_RECORD_HEADER FileRecord);

NTSTATUS
NtfsAllocateSparseRange(PNTFS_VCB Vcb,
                        PNTFS_ATTR_CONTEXT AttrContext,
                        ULONG AttrOffset,
                        PFILE_RECORD_HEADER FileRecord,
                        ULONGLONG StartVcn,
                        ULONGLONG VcnCount);

NTSTATUS
NtfsDeallocateSparseRange(PNTFS_VCB Vcb,
                          PNTFS_ATTR_CONTEXT AttrContext,
                          ULONG AttrOffset,
                          PFILE_RECORD_HEADER FileRecord,
                          ULONGLONG StartVcn,
                          ULONGLONG VcnCount);

/* Clusters in a unit of allocation of a sparse or compressed attribute */
FORCEINLINE
ULONG
NtfsSparseUnitClusters(PNTFS_ATTR_CONTEXT AttrContext)
{
    return 1UL << AttrContext->pRecord->NonResident.CompressionUnit;
}

/* Whether a non-resident attribute is sparse */
FORCEINLINE
BOOLEAN
NtfsAttributeIsSparse(PNTFS_ATTR_CONTEXT AttrContext)
{
    return AttrContext->pRecord->IsNonResident &&
           (AttrContext->pRecord->Flags & NTFS_ATTR_SPARSE) != 0;
}

/* Whether a non-resident attribute is compressed */
FORCEINLINE
BOOLEAN
NtfsAttributeIsCompressed(PNTFS_ATTR_CONTEXT AttrContext)
{
    return AttrContext->pRecord->IsNonResident &&
           (AttrContext->pRecord->Flags & NTFS_ATTR_COMPRESSION_MASK) != 0;
}

/* Whether a non-resident attribute is allocated in units, with holes: sparse or compressed */
FORCEINLINE
BOOLEAN
NtfsAttributeHasUnits(PNTFS_ATTR_CONTEXT AttrContext)
{
    return NtfsAttributeIsSparse(AttrContext) || NtfsAttributeIsCompressed(AttrContext);
}

NTSTATUS
FreeClusters(PNTFS_VCB Vcb,
             PNTFS_ATTR_CONTEXT AttrContext,
             ULONG AttrOffset,
             PFILE_RECORD_HEADER FileRecord,
             ULONG ClustersToFree);

/* blockdev.c */

NTSTATUS
NtfsReadDiskCached(PDEVICE_EXTENSION Vcb,
                   LONGLONG StartingOffset,
                   ULONG Length,
                   PUCHAR Buffer);

NTSTATUS
NtfsWriteDiskCached(PDEVICE_EXTENSION Vcb,
                    LONGLONG StartingOffset,
                    ULONG Length,
                    const PUCHAR Buffer);

/* notify.c */
NTSTATUS
NtfsNotifyChangeDirectory(PNTFS_IRP_CONTEXT IrpContext);

VOID
NtfsReportPathChange(PNTFS_VCB Vcb, PCWSTR Path, ULONG Filter, ULONG Action);

VOID
NtfsReportChange(PNTFS_FCB Fcb, ULONG Filter, ULONG Action);

VOID
NtfsReportDataChange(PNTFS_FCB Fcb, ULONG Filter);

VOID
NtfsNotifyDirectoryDeleted(PNTFS_FCB Fcb);

/* lock.c / shared stream-lock lifetime in fcb.c */
NTSTATUS
NtfsLockControl(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NTAPI
NtfsCompleteLockIrp(PVOID Context, PIRP Irp);

VOID
NtfsRemoveFileLocks(PNTFS_VCB Vcb);

NTSTATUS
NtfsInitializeFcbLock(PNTFS_FCB Fcb);

VOID
NtfsReferenceFileLock(PNTFS_FILE_LOCK FileLock);

VOID
NtfsReleaseFileLock(PNTFS_FILE_LOCK FileLock);

BOOLEAN
NtfsCheckFileLock(PNTFS_FCB Fcb, PIRP Irp, PLARGE_INTEGER Offset, ULONG Length, BOOLEAN Write);

/* pnp.c */
NTSTATUS
NtfsPnp(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsEjectMedia(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsForwardIrpSynchronous(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* shutdown.c */
NTSTATUS
NtfsShutdown(PNTFS_IRP_CONTEXT IrpContext);

/* flush.c */
NTSTATUS
NtfsFlushVolume(PDEVICE_EXTENSION Vcb);

NTSTATUS
NtfsFlushVolumeStream(PDEVICE_EXTENSION Vcb);

NTSTATUS
NtfsFlushFileCache(PFILE_OBJECT FileObject, BOOLEAN Purge);


VOID
NtfsInitIoRunList(OUT PNTFS_IO_RUN_LIST RunList);

NTSTATUS
NtfsAddIoRun(IN OUT PNTFS_IO_RUN_LIST RunList,
             IN LONGLONG Lbo,
             IN ULONG ByteCount);

VOID
NtfsFreeIoRunList(IN OUT PNTFS_IO_RUN_LIST RunList);

NTSTATUS
NtfsPerformIoRuns(IN PDEVICE_OBJECT DeviceObject,
                  IN UCHAR MajorFunction,
                  IN ULONG SectorSize,
                  IN OUT PUCHAR Buffer,
                  IN PNTFS_IO_RUN_LIST RunList,
                  IN BOOLEAN Override,
                  OUT PULONG BytesTransferred OPTIONAL);

NTSTATUS
NtfsPerformIrpIoRuns(IN PDEVICE_OBJECT DeviceObject,
                     IN UCHAR MajorFunction,
                     IN ULONG SectorSize,
                     IN PIRP Irp,
                     IN OUT PUCHAR Buffer,
                     IN PNTFS_IO_RUN_LIST RunList,
                     IN BOOLEAN Override,
                     OUT PULONG BytesTransferred);

NTSTATUS
NtfsReadDisk(IN PDEVICE_OBJECT DeviceObject,
             IN LONGLONG StartingOffset,
             IN ULONG Length,
             IN ULONG SectorSize,
             IN OUT PUCHAR Buffer,
             IN BOOLEAN Override);

NTSTATUS
NtfsWriteDisk(IN PDEVICE_OBJECT DeviceObject,
              IN LONGLONG StartingOffset,
              IN ULONG Length,
              IN ULONG SectorSize,
              IN const PUCHAR Buffer);

NTSTATUS
NtfsFlushDevice(IN PDEVICE_OBJECT DeviceObject);

NTSTATUS
NtfsReadSectors(IN PDEVICE_OBJECT DeviceObject,
                IN ULONG DiskSector,
                IN ULONG SectorCount,
                IN ULONG SectorSize,
                IN OUT PUCHAR Buffer,
                IN BOOLEAN Override);

NTSTATUS
NtfsDeviceIoControl(IN PDEVICE_OBJECT DeviceObject,
                    IN ULONG ControlCode,
                    IN PVOID InputBuffer,
                    IN ULONG InputBufferSize,
                    IN OUT PVOID OutputBuffer,
                    IN OUT PULONG OutputBufferSize,
                    IN BOOLEAN Override);


/* btree.c */

LONG
CompareTreeKeys(PB_TREE_KEY Key1,
                PB_TREE_KEY Key2,
                ULONG CollationRule,
                BOOLEAN CaseSensitive);

LONG
CollateFileNames(PUNICODE_STRING Name1,
                 PUNICODE_STRING Name2,
                 BOOLEAN CaseSensitive);

NTSTATUS
CreateBTreeFromIndex(PDEVICE_EXTENSION Vcb,
                     PFILE_RECORD_HEADER FileRecordWithIndex,
                     /*PCWSTR IndexName,*/
                     PNTFS_ATTR_CONTEXT IndexRootContext,
                     PINDEX_ROOT_ATTRIBUTE IndexRoot,
                     PB_TREE *NewTree);

NTSTATUS
CreateIndexRootFromBTree(PDEVICE_EXTENSION DeviceExt,
                         PB_TREE Tree,
                         ULONG MaxIndexSize,
                         PINDEX_ROOT_ATTRIBUTE *IndexRoot,
                         ULONG *Length);

NTSTATUS
DemoteBTreeRoot(PB_TREE Tree);

PB_TREE_KEY
CreateBTreeKeyFromFilename(ULONGLONG FileReference,
                           PFILENAME_ATTRIBUTE FileNameAttribute);

PB_TREE_KEY
CreateBTreeKeyFromEntry(PINDEX_ENTRY_ATTRIBUTE Entry);

PINDEX_ENTRY_ATTRIBUTE
NtfsAllocateViewIndexEntry(PVOID Key,
                           USHORT KeyLength,
                           PVOID Data,
                           USHORT DataLength);

VOID
DestroyBTree(PB_TREE Tree);

VOID
DestroyBTreeKey(PB_TREE_KEY Key);

VOID
DestroyBTreeNode(PB_TREE_FILENAME_NODE Node);

VOID
DumpBTree(PB_TREE Tree);

VOID
DumpBTreeKey(PB_TREE Tree,
             PB_TREE_KEY Key,
             ULONG Number,
             ULONG Depth);

VOID
DumpBTreeNode(PB_TREE Tree,
              PB_TREE_FILENAME_NODE Node,
              ULONG Number,
              ULONG Depth);

NTSTATUS
CreateEmptyBTree(PB_TREE *NewTree);

ULONGLONG
GetAllocationOffsetFromVCN(PDEVICE_EXTENSION DeviceExt,
                           ULONG IndexBufferSize,
                           ULONGLONG Vcn);

ULONGLONG
GetIndexEntryVCN(PINDEX_ENTRY_ATTRIBUTE IndexEntry);

ULONG
GetSizeOfIndexEntries(PB_TREE_FILENAME_NODE Node);

NTSTATUS
NtfsInsertKey(PB_TREE Tree,
              ULONGLONG FileReference,
              PFILENAME_ATTRIBUTE FileNameAttribute,
              PB_TREE_FILENAME_NODE Node,
              BOOLEAN CaseSensitive,
              ULONG MaxIndexRootSize,
              ULONG IndexRecordSize,
              PB_TREE_KEY *MedianKey,
              PB_TREE_FILENAME_NODE *NewRightHandSibling);

NTSTATUS
NtfsInsertIndexKey(PB_TREE Tree,
                   PINDEX_ENTRY_ATTRIBUTE Entry,
                   PB_TREE_FILENAME_NODE Node,
                   BOOLEAN CaseSensitive,
                   ULONG MaxIndexRootSize,
                   ULONG IndexRecordSize,
                   PB_TREE_KEY *MedianKey,
                   PB_TREE_FILENAME_NODE *NewRightHandSibling);

NTSTATUS
NtfsRemoveKey(PB_TREE Tree,
              PB_TREE_FILENAME_NODE Node,
              PB_TREE_KEY SearchKey,
              BOOLEAN CaseSensitive);

typedef BOOLEAN (*PNTFS_INDEX_VISITOR)(PINDEX_ENTRY_ATTRIBUTE Entry, PVOID Context);

NTSTATUS
NtfsFindLastKey(PB_TREE Tree,
                PB_TREE_KEY *LastKey);

NTSTATUS
NtfsVisitKeys(PB_TREE Tree,
              PB_TREE_FILENAME_NODE Node,
              PB_TREE_KEY Low,
              PB_TREE_KEY High,
              PNTFS_INDEX_VISITOR Visitor,
              PVOID Context,
              PBOOLEAN Stopped);

NTSTATUS
NtfsFindKey(PB_TREE Tree,
            PB_TREE_FILENAME_NODE Node,
            PB_TREE_KEY SearchKey,
            BOOLEAN CaseSensitive,
            PB_TREE_KEY *FoundKey);

NTSTATUS
SplitBTreeNode(PB_TREE Tree,
               PB_TREE_FILENAME_NODE Node,
               PB_TREE_KEY *MedianKey,
               PB_TREE_FILENAME_NODE *NewRightHandSibling,
               BOOLEAN CaseSensitive);

NTSTATUS
UpdateIndexAllocation(PDEVICE_EXTENSION DeviceExt,
                      PB_TREE Tree,
                      ULONG IndexBufferSize,
                      PFILE_RECORD_HEADER FileRecord);

NTSTATUS
UpdateIndexNode(PDEVICE_EXTENSION DeviceExt,
                PB_TREE Tree,
                PFILE_RECORD_HEADER FileRecord,
                PB_TREE_FILENAME_NODE Node,
                ULONG IndexBufferSize,
                PNTFS_ATTR_CONTEXT IndexAllocationContext,
                ULONG IndexAllocationOffset);

/* close.c */

NTSTATUS
NtfsCleanup(PNTFS_IRP_CONTEXT IrpContext);


/* close.c */

NTSTATUS
NtfsCloseFile(PDEVICE_EXTENSION DeviceExt,
              PFILE_OBJECT FileObject);

NTSTATUS
NtfsClose(PNTFS_IRP_CONTEXT IrpContext);


/* create.c */

NTSTATUS
NtfsCreate(PNTFS_IRP_CONTEXT IrpContext);

PFILE_RECORD_HEADER
NtfsCreateEmptyFileRecord(PDEVICE_EXTENSION DeviceExt);

NTSTATUS
NtfsCreateFileRecord(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG ParentReference,
                     PCUNICODE_STRING Name,
                     ULONG FileAttributes,
                     BOOLEAN IsDirectory,
                     ULONG SecurityId,
                     BOOLEAN CaseSensitive,
                     BOOLEAN CanWait,
                     PULONGLONG NewFileReference);

/* The file reference of the file an FCB is open on: its MFT index and sequence number */
FORCEINLINE
ULONGLONG
NtfsFileReference(PNTFS_FCB Fcb)
{
    return Fcb->MFTIndex | ((ULONGLONG)Fcb->SequenceNumber << 48);
}

/* devctl.c */

NTSTATUS
NtfsDeviceControl(PNTFS_IRP_CONTEXT IrpContext);


/* dirctl.c */

ULONGLONG
NtfsGetFileSize(PDEVICE_EXTENSION DeviceExt,
                PFILE_RECORD_HEADER FileRecord,
                PCWSTR Stream,
                ULONG StreamLength,
                PULONGLONG AllocatedSize);

NTSTATUS
NtfsDirectoryControl(PNTFS_IRP_CONTEXT IrpContext);


/* dispatch.c */

DRIVER_DISPATCH NtfsFsdDispatch;
NTSTATUS NTAPI
NtfsFsdDispatch(PDEVICE_OBJECT DeviceObject,
                PIRP Irp);


/* fastio.c */

BOOLEAN NTAPI
NtfsAcqLazyWrite(PVOID Context,
                 BOOLEAN Wait);

VOID NTAPI
NtfsRelLazyWrite(PVOID Context);

BOOLEAN NTAPI
NtfsAcqReadAhead(PVOID Context,
                 BOOLEAN Wait);

VOID NTAPI
NtfsRelReadAhead(PVOID Context);

FAST_IO_CHECK_IF_POSSIBLE NtfsFastIoCheckIfPossible;
FAST_IO_READ NtfsFastIoRead;
FAST_IO_WRITE NtfsFastIoWrite;


/* fcb.c */

#define NTFS_FCB_HASH_BUCKETS 256

NTSTATUS
NtfsInitializeFcbHash(PNTFS_VCB Vcb);

VOID
NtfsUninitializeFcbHash(PNTFS_VCB Vcb);

NTSTATUS
NtfsSetFCBPath(
    _Inout_ PNTFS_FCB Fcb,
    _In_ PCWSTR Path);

VOID
NtfsReplaceFCBPath(
    _Inout_ PNTFS_FCB Fcb,
    _In_ PWCHAR Path);

PWCHAR
NtfsBuildChildPath(
    _In_ PCWSTR DirectoryPath,
    _In_ PCWSTR Name);

VOID
NtfsSetLinkCount(
    _In_ PNTFS_VCB Vcb,
    _In_ ULONGLONG MftIndex,
    _In_ USHORT LinkCount);

BOOLEAN
NtfsDeferFileDeletion(PNTFS_VCB Vcb, ULONGLONG FileReference);

NTSTATUS
NtfsSetValidDataLength(PDEVICE_EXTENSION DeviceExt,
                       PNTFS_FCB Fcb,
                       PFILE_OBJECT FileObject,
                       LONGLONG Length,
                       PFILE_RECORD_HEADER Record);

NTSTATUS
NtfsMakeStreamFCB(
    _In_ PNTFS_VCB Vcb,
    _In_ PNTFS_FCB BaseFcb,
    _In_ PCUNICODE_STRING StreamName,
    _Out_ PNTFS_FCB *StreamFcb);

PNTFS_FCB
NtfsCreateFCB(PCWSTR FileName,
              PCWSTR Stream,
              PNTFS_VCB Vcb);

VOID
NtfsDestroyFCB(PNTFS_FCB Fcb);

BOOLEAN
NtfsFCBIsDirectory(PNTFS_FCB Fcb);

BOOLEAN
NtfsFCBIsReparsePoint(PNTFS_FCB Fcb);

BOOLEAN
NtfsFCBIsCompressed(PNTFS_FCB Fcb);

BOOLEAN
NtfsFCBIsEncrypted(PNTFS_FCB Fcb);

BOOLEAN
NtfsFCBIsRoot(PNTFS_FCB Fcb);

VOID
NtfsGrabFCB(PNTFS_VCB Vcb,
            PNTFS_FCB Fcb);

VOID
NtfsReleaseFCB(PNTFS_VCB Vcb,
               PNTFS_FCB Fcb);

VOID
NtfsAddFCBToTable(PNTFS_VCB Vcb,
                  PNTFS_FCB Fcb);

VOID
NtfsRemoveFCBFromTable(
    _In_ PNTFS_VCB Vcb,
    _In_ PNTFS_FCB Fcb);

PNTFS_FCB
NtfsGrabFCBFromTable(PNTFS_VCB Vcb,
                     PCWSTR FileName);

NTSTATUS
NtfsFCBInitializeCache(PNTFS_VCB Vcb,
                       PNTFS_FCB Fcb);

PNTFS_FCB
NtfsMakeRootFCB(PNTFS_VCB Vcb);

PNTFS_FCB
NtfsOpenRootFCB(PNTFS_VCB Vcb);

NTSTATUS
NtfsAttachFCBToFileObject(PNTFS_VCB Vcb,
                          PNTFS_FCB Fcb,
                          PFILE_OBJECT FileObject);

NTSTATUS
NtfsGetFCBForFile(PNTFS_VCB Vcb,
                  PNTFS_FCB *pParentFCB,
                  PNTFS_FCB *pFCB,
                  PCWSTR pFileName,
                  BOOLEAN CaseSensitive,
                  PULONG ReparseOffset);

NTSTATUS
NtfsReadFCBAttribute(PNTFS_VCB Vcb,
                     PNTFS_FCB pFCB,
                     ULONG Type,
                     PCWSTR Name,
                     ULONG NameLength,
                     PVOID * Data);

NTSTATUS
NtfsMakeFCBFromDirEntry(PNTFS_VCB Vcb,
                        PNTFS_FCB DirectoryFCB,
                        PUNICODE_STRING Name,
                        PCWSTR Stream,
                        PFILE_RECORD_HEADER Record,
                        ULONGLONG MFTIndex,
                        PNTFS_FCB * fileFCB);


/* finfo.c */

NTSTATUS
NtfsQueryInformation(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsSetEndOfFile(PNTFS_FCB Fcb,
                 PFILE_OBJECT FileObject,
                 PDEVICE_EXTENSION DeviceExt,
                 ULONG IrpFlags,
                 BOOLEAN CaseSensitive,
                 PLARGE_INTEGER NewFileSize);

NTSTATUS
NtfsSetInformation(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsLfsCheckCleanShutdown(PDEVICE_EXTENSION Vcb);

/* fsctl.c */

NTSTATUS
NtfsFileSystemControl(PNTFS_IRP_CONTEXT IrpContext);

/* flush.c */

NTSTATUS
NtfsFlushBuffers(PNTFS_IRP_CONTEXT IrpContext);


/* mft.c */
NTSTATUS
NtfsAddFilenameToDirectory(PDEVICE_EXTENSION DeviceExt,
                           ULONGLONG DirectoryMftIndex,
                           ULONGLONG FileReferenceNumber,
                           PFILENAME_ATTRIBUTE FilenameAttribute,
                           BOOLEAN CaseSensitive);

NTSTATUS
NtfsRemoveFilenameFromDirectory(PDEVICE_EXTENSION DeviceExt,
                                ULONGLONG DirectoryMftIndex,
                                PFILENAME_ATTRIBUTE FilenameAttribute,
                                BOOLEAN CaseSensitive);

NTSTATUS
NtfsInsertIndexEntry(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG MftIndex,
                     PCWSTR IndexName,
                     ULONG IndexNameLength,
                     PINDEX_ENTRY_ATTRIBUTE Entry,
                     BOOLEAN CaseSensitive);

NTSTATUS
NtfsRemoveIndexEntry(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG MftIndex,
                     PCWSTR IndexName,
                     ULONG IndexNameLength,
                     PINDEX_ENTRY_ATTRIBUTE Entry,
                     BOOLEAN CaseSensitive);

NTSTATUS
NtfsFindIndexEntry(PDEVICE_EXTENSION DeviceExt,
                   ULONGLONG MftIndex,
                   PCWSTR IndexName,
                   ULONG IndexNameLength,
                   PINDEX_ENTRY_ATTRIBUTE SearchEntry,
                   PINDEX_ENTRY_ATTRIBUTE *FoundEntry);

NTSTATUS
NtfsFindLastIndexEntry(PDEVICE_EXTENSION DeviceExt,
                       ULONGLONG MftIndex,
                       PCWSTR IndexName,
                       ULONG IndexNameLength,
                       PINDEX_ENTRY_ATTRIBUTE *FoundEntry);

NTSTATUS
NtfsVisitIndexEntries(PDEVICE_EXTENSION DeviceExt,
                      ULONGLONG MftIndex,
                      PCWSTR IndexName,
                      ULONG IndexNameLength,
                      PINDEX_ENTRY_ATTRIBUTE LowEntry,
                      PINDEX_ENTRY_ATTRIBUTE HighEntry,
                      PNTFS_INDEX_VISITOR Visitor,
                      PVOID Context);

NTSTATUS
FreeMftEntry(PDEVICE_EXTENSION DeviceExt,
             ULONGLONG MftIndex);

NTSTATUS
NtfsDeleteFileRecord(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG MftIndex,
                     BOOLEAN CaseSensitive);

NTSTATUS
AddNewMftEntry(PFILE_RECORD_HEADER FileRecord,
               PDEVICE_EXTENSION DeviceExt,
               PULONGLONG DestinationIndex,
               BOOLEAN CanWait);

VOID
NtfsDumpData(ULONG_PTR Buffer, ULONG Length);

PNTFS_ATTR_CONTEXT
PrepareAttributeContext(PNTFS_ATTR_RECORD AttrRecord);

VOID
ReleaseAttributeContext(PNTFS_ATTR_CONTEXT Context);

ULONG
ReadAttribute(PDEVICE_EXTENSION Vcb,
              PNTFS_ATTR_CONTEXT Context,
              ULONGLONG Offset,
              PCHAR Buffer,
              ULONG Length);

ULONG
ReadAttributeToIrp(PDEVICE_EXTENSION Vcb,
                   PNTFS_ATTR_CONTEXT Context,
                   ULONGLONG Offset,
                   PCHAR Buffer,
                   ULONG Length,
                   PIRP Irp);

NTSTATUS
WriteAttribute(PDEVICE_EXTENSION Vcb,
               PNTFS_ATTR_CONTEXT Context,
               ULONGLONG Offset,
               const PUCHAR Buffer,
               ULONG Length,
               PULONG LengthWritten,
               PFILE_RECORD_HEADER FileRecord);

/* Not file data: written through the log. */
BOOLEAN
NtfsAttributeIsMetadata(PNTFS_ATTR_CONTEXT Context);

NTSTATUS
NtfsWriteFileRecordMirror(PDEVICE_EXTENSION Vcb,
                          ULONGLONG MftIndex,
                          PFILE_RECORD_HEADER Image);

NTSTATUS
WriteAttributeFromIrp(PDEVICE_EXTENSION Vcb,
                      PNTFS_ATTR_CONTEXT Context,
                      ULONGLONG Offset,
                      const PUCHAR Buffer,
                      ULONG Length,
                      PULONG LengthWritten,
                      PFILE_RECORD_HEADER FileRecord,
                      PIRP Irp);

ULONGLONG
AttributeDataLength(PNTFS_ATTR_RECORD AttrRecord);

NTSTATUS
InternalSetResidentAttributeLength(PDEVICE_EXTENSION DeviceExt,
                                   PNTFS_ATTR_CONTEXT AttrContext,
                                   PFILE_RECORD_HEADER FileRecord,
                                   ULONG AttrOffset,
                                   ULONG DataSize);

PNTFS_ATTR_RECORD
MoveAttributes(PDEVICE_EXTENSION DeviceExt,
               PNTFS_ATTR_RECORD FirstAttributeToMove,
               ULONG FirstAttributeOffset,
               ULONG_PTR MoveTo);

NTSTATUS
SetAttributeDataLength(PFILE_OBJECT FileObject,
                       PNTFS_FCB Fcb,
                       PNTFS_ATTR_CONTEXT AttrContext,
                       ULONG AttrOffset,
                       PFILE_RECORD_HEADER FileRecord,
                       PLARGE_INTEGER DataSize);

VOID
SetFileRecordEnd(PFILE_RECORD_HEADER FileRecord,
                 PNTFS_ATTR_RECORD AttrEnd,
                 ULONG EndMarker);

NTSTATUS
SetNonResidentAttributeDataLength(PDEVICE_EXTENSION Vcb,
                                  PNTFS_ATTR_CONTEXT AttrContext,
                                  ULONG AttrOffset,
                                  PFILE_RECORD_HEADER FileRecord,
                                  PLARGE_INTEGER DataSize);

NTSTATUS
NtfsExtendAllocation(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_ATTR_CONTEXT AttrContext,
    _In_ ULONG AttrOffset,
    _In_ PFILE_RECORD_HEADER FileRecord,
    _In_ ULONGLONG AllocationSize);

NTSTATUS
NtfsTrimAllocation(
    _In_ PNTFS_FCB Fcb);

NTSTATUS
NtfsSetAllocationSize(PFILE_OBJECT FileObject,
                      PNTFS_FCB Fcb,
                      PNTFS_ATTR_CONTEXT AttrContext,
                      ULONG AttrOffset,
                      PFILE_RECORD_HEADER FileRecord,
                      ULONGLONG AllocationSize);

NTSTATUS
NtfsZeroAttributeRange(PDEVICE_EXTENSION Vcb,
                       PNTFS_ATTR_CONTEXT AttrContext,
                       PFILE_RECORD_HEADER FileRecord,
                       ULONGLONG Start,
                       ULONGLONG End);

NTSTATUS
NtfsGetFileNames(PDEVICE_EXTENSION DeviceExt,
                 PFILE_RECORD_HEADER BaseRecord,
                 PFILENAME_ATTRIBUTE **Names,
                 PULONG Count);

VOID
NtfsFreeFileNames(PFILENAME_ATTRIBUTE *Names,
                  ULONG Count);

NTSTATUS
NtfsDeleteFileLink(PDEVICE_EXTENSION DeviceExt,
                   ULONGLONG MftIndex,
                   ULONGLONG ParentIndex,
                   PCUNICODE_STRING Name,
                   BOOLEAN CaseSensitive,
                   PUSHORT LinksLeft);

NTSTATUS
NtfsFindFileNameRecord(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER BaseRecord,
                       ULONGLONG ParentIndex,
                       PCUNICODE_STRING Name,
                       PFILE_RECORD_HEADER *Holder,
                       PULONG Offset);

NTSTATUS
NtfsRemoveFileNameAttribute(PDEVICE_EXTENSION DeviceExt,
                            PFILE_RECORD_HEADER BaseRecord,
                            ULONGLONG ParentIndex,
                            PCUNICODE_STRING Name);

NTSTATUS
NtfsUnlinkFileName(PDEVICE_EXTENSION DeviceExt,
                   ULONGLONG MftIndex,
                   ULONGLONG ParentIndex,
                   PCUNICODE_STRING Name,
                   BOOLEAN CaseSensitive);

NTSTATUS
NtfsDeleteExtensionRecords(PDEVICE_EXTENSION DeviceExt,
                           PFILE_RECORD_HEADER BaseRecord);

NTSTATUS
NtfsDeleteAttributeFromFile(PDEVICE_EXTENSION DeviceExt,
                            PFILE_RECORD_HEADER BaseRecord,
                            ULONG Type,
                            PCWSTR Name,
                            ULONG NameLength);

NTSTATUS
NtfsSetAttributeValue(PDEVICE_EXTENSION DeviceExt,
                      PFILE_RECORD_HEADER BaseRecord,
                      ULONG Type,
                      PCWSTR Name,
                      USHORT NameLength,
                      const VOID *Value,
                      ULONG ValueLength);

NTSTATUS
NtfsDeleteStream(PDEVICE_EXTENSION DeviceExt,
                 ULONGLONG MftIndex,
                 PCWSTR StreamName);

NTSTATUS
ConvertResidentAttributeToNonResident(PDEVICE_EXTENSION Vcb,
                                      PNTFS_ATTR_CONTEXT AttrContext,
                                      ULONG AttrOffset,
                                      PFILE_RECORD_HEADER FileRecord,
                                      PLARGE_INTEGER DataSize);

NTSTATUS
NtfsWriteStreamAt(PDEVICE_EXTENSION Vcb,
                  ULONGLONG MftIndex,
                  PCWSTR StreamName,
                  ULONG StreamNameLength,
                  ULONGLONG Offset,
                  PVOID Buffer,
                  ULONG Length);

NTSTATUS
NtfsMakeRoomInFileRecord(PDEVICE_EXTENSION DeviceExt,
                         PFILE_RECORD_HEADER FileRecord,
                         ULONG BytesNeeded,
                         ULONG KeepInstance);

NTSTATUS
SetResidentAttributeDataLength(PDEVICE_EXTENSION Vcb,
                               PNTFS_ATTR_CONTEXT AttrContext,
                               ULONG AttrOffset,
                               PFILE_RECORD_HEADER FileRecord,
                               PLARGE_INTEGER DataSize);

ULONGLONG
AttributeAllocatedLength(PNTFS_ATTR_RECORD AttrRecord);

BOOLEAN
CompareFileName(PDEVICE_EXTENSION Vcb,
                PUNICODE_STRING FileName,
                PINDEX_ENTRY_ATTRIBUTE IndexEntry,
                BOOLEAN DirSearch,
                BOOLEAN CaseSensitive);

NTSTATUS
UpdateMftMirror(PNTFS_VCB Vcb);

NTSTATUS
NtfsLoadUpCaseTable(PDEVICE_EXTENSION Vcb);

NTSTATUS
ReadFileRecord(PDEVICE_EXTENSION Vcb,
               ULONGLONG index,
               PFILE_RECORD_HEADER file);

/* As ReadFileRecord; with AllowUnused, a never-initialized (all-zero) slot
 * also reads successfully, as zeroes. */
NTSTATUS
NtfsReadFileRecordEx(PDEVICE_EXTENSION Vcb,
                     ULONGLONG Index,
                     PFILE_RECORD_HEADER Record,
                     BOOLEAN AllowUnused);

/* NTFS duplicates a file's sizes, timestamps and attributes into the $FILE_NAME
 * of its entry in the parent directory's index, so a directory can be
 * enumerated without reading every file record. These flags say which of them
 * the caller changed and so need refreshing there. */
#define NTFS_FILENAME_UPDATE_SIZES  0x1
#define NTFS_FILENAME_UPDATE_TIMES  0x2
#define NTFS_FILENAME_UPDATE_ATTRS  0x4
#define NTFS_FILENAME_UPDATE_REPLACE 0x8

typedef struct _NTFS_FILENAME_UPDATE
{
    ULONG Flags;

    /* NTFS_FILENAME_UPDATE_SIZES */
    ULONGLONG DataSize;
    ULONGLONG AllocatedSize;

    /* NTFS_FILENAME_UPDATE_TIMES */
    ULONGLONG CreationTime;
    ULONGLONG LastWriteTime;
    ULONGLONG ChangeTime;
    ULONGLONG LastAccessTime;

    /* NTFS_FILENAME_UPDATE_ATTRS */
    ULONG FileAttributes;

    /* NTFS_FILENAME_UPDATE_REPLACE: same collated key, new owner and value */
    ULONGLONG FileReference;
    PFILENAME_ATTRIBUTE FileName;
} NTFS_FILENAME_UPDATE, *PNTFS_FILENAME_UPDATE;

NTSTATUS
UpdateIndexEntryFileName(PDEVICE_EXTENSION Vcb,
                         PFILE_RECORD_HEADER MftRecord,
                         PCHAR IndexRecord,
                         ULONG IndexBlockSize,
                         PINDEX_ENTRY_ATTRIBUTE FirstEntry,
                         PINDEX_ENTRY_ATTRIBUTE LastEntry,
                         PUNICODE_STRING FileName,
                         PULONG StartEntry,
                         PULONG CurrentEntry,
                         BOOLEAN DirSearch,
                         PNTFS_FILENAME_UPDATE Update,
                         BOOLEAN CaseSensitive);

NTSTATUS
NtfsUpdateDuplicatedInformation(PDEVICE_EXTENSION Vcb,
                                PFILE_RECORD_HEADER FileRecord,
                                ULONGLONG MFTIndex,
                                ULONG Flags,
                                BOOLEAN CaseSensitive);

VOID
NtfsMarkVolumeCorrupt(PDEVICE_EXTENSION Vcb);

NTSTATUS
UpdateFileNameRecord(PDEVICE_EXTENSION Vcb,
                     ULONGLONG ParentMFTIndex,
                     PUNICODE_STRING FileName,
                     BOOLEAN DirSearch,
                     PNTFS_FILENAME_UPDATE Update,
                     BOOLEAN CaseSensitive);

NTSTATUS
UpdateFileRecord(PDEVICE_EXTENSION Vcb,
                 ULONGLONG MftIndex,
                 PFILE_RECORD_HEADER FileRecord);

BOOLEAN
NtfsAttributeNamesEqual(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PCWSTR Name1,
    _In_ PCWSTR Name2,
    _In_ ULONG Length);

NTSTATUS
FindAttribute(PDEVICE_EXTENSION Vcb,
              PFILE_RECORD_HEADER MftRecord,
              ULONG Type,
              PCWSTR Name,
              ULONG NameLength,
              PNTFS_ATTR_CONTEXT * AttrCtx,
              PULONG Offset);

VOID
ReadVCN(PDEVICE_EXTENSION Vcb,
        PFILE_RECORD_HEADER file,
        ATTRIBUTE_TYPE type,
        ULONGLONG vcn,
        ULONG count,
        PVOID buffer);

NTSTATUS
FixupUpdateSequenceArray(PDEVICE_EXTENSION Vcb,
                         PNTFS_RECORD_HEADER Record);

NTSTATUS
AddFixupArray(PDEVICE_EXTENSION Vcb,
              PNTFS_RECORD_HEADER Record);

NTSTATUS
ReadLCN(PDEVICE_EXTENSION Vcb,
        ULONGLONG lcn,
        ULONG count,
        PVOID buffer);

VOID
EnumerAttribute(PFILE_RECORD_HEADER file,
                PDEVICE_EXTENSION Vcb,
                PDEVICE_OBJECT DeviceObject);

NTSTATUS
NtfsLookupFile(PDEVICE_EXTENSION Vcb,
               PUNICODE_STRING PathName,
               BOOLEAN CaseSensitive,
               PFILE_RECORD_HEADER *FileRecord,
               PULONGLONG MFTIndex);

NTSTATUS
NtfsLookupFileAt(PDEVICE_EXTENSION Vcb,
                 PUNICODE_STRING PathName,
                 BOOLEAN CaseSensitive,
                 PFILE_RECORD_HEADER *FileRecord,
                 PULONGLONG MFTIndex,
                 ULONGLONG CurrentMFTIndex);

VOID
NtfsDumpFileRecord(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord);

NTSTATUS
NtfsFindFileAt(PDEVICE_EXTENSION Vcb,
               PUNICODE_STRING SearchPattern,
               PULONG FirstEntry,
               PFILE_RECORD_HEADER *FileRecord,
               PULONGLONG MFTIndex,
               ULONGLONG CurrentMFTIndex,
               BOOLEAN CaseSensitive,
               PFILENAME_ATTRIBUTE OutFoundName);

NTSTATUS
NtfsIsDirectoryEmpty(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ ULONGLONG MftIndex,
    _Out_ PBOOLEAN Empty);

NTSTATUS
NtfsFindMftRecord(PDEVICE_EXTENSION Vcb,
                  ULONGLONG MFTIndex,
                  PUNICODE_STRING FileName,
                  PULONG FirstEntry,
                  BOOLEAN DirSearch,
                  BOOLEAN CaseSensitive,
                  ULONGLONG *OutMFTIndex,
                  PFILENAME_ATTRIBUTE OutFoundName);

/* misc.c */

BOOLEAN
NtfsIsIrpTopLevel(PIRP Irp);

PNTFS_IRP_CONTEXT
NtfsAllocateIrpContext(PDEVICE_OBJECT DeviceObject,
                       PIRP Irp);

PVOID
NtfsGetUserBuffer(PIRP Irp,
                  BOOLEAN Paging);

NTSTATUS
NtfsLockUserBuffer(IN PIRP Irp,
                   IN ULONG Length,
                   IN LOCK_OPERATION Operation);

BOOLEAN
NtfsIsValidFileName(
    _In_ PCUNICODE_STRING Name);

#if 0
BOOLEAN
wstrcmpjoki(PWSTR s1, PWSTR s2);

VOID
CdfsSwapString(PWCHAR Out,
	       PUCHAR In,
	       ULONG Count);
#endif

VOID
NtfsFileFlagsToAttributes(ULONG NtfsAttributes,
                          PULONG FileAttributes);


/* objid.c */

VOID
NtfsFindObjectIdIndex(PDEVICE_EXTENSION DeviceExt);

NTSTATUS
NtfsReleaseObjectId(PDEVICE_EXTENSION DeviceExt,
                    PFILE_RECORD_HEADER FileRecord);

NTSTATUS
NtfsQueryObjectId(PDEVICE_EXTENSION DeviceExt,
                  PFILE_RECORD_HEADER FileRecord,
                  PFILE_OBJECTID_BUFFER Buffer);

NTSTATUS
NtfsSetObjectId(PDEVICE_EXTENSION DeviceExt,
                PNTFS_FCB Fcb,
                PFILE_RECORD_HEADER FileRecord,
                PFILE_OBJECTID_BUFFER Buffer);

NTSTATUS
NtfsSetObjectIdExtended(PDEVICE_EXTENSION DeviceExt,
                        PNTFS_FCB Fcb,
                        PFILE_RECORD_HEADER FileRecord,
                        PUCHAR ExtendedInfo);

NTSTATUS
NtfsCreateOrGetObjectId(PDEVICE_EXTENSION DeviceExt,
                        PNTFS_FCB Fcb,
                        PFILE_RECORD_HEADER FileRecord,
                        PFILE_OBJECTID_BUFFER Buffer);

NTSTATUS
NtfsDeleteObjectId(PDEVICE_EXTENSION DeviceExt,
                   PFILE_RECORD_HEADER FileRecord);

NTSTATUS
NtfsFindFileByObjectId(PDEVICE_EXTENSION DeviceExt,
                       PUCHAR ObjectId,
                       PULONGLONG MftIndex);


/* ea.c */

ULONG
NtfsGetEaSize(PDEVICE_EXTENSION Vcb,
              PFILE_RECORD_HEADER FileRecord);

NTSTATUS
NtfsCheckEaList(PFILE_FULL_EA_INFORMATION Eas,
                ULONG Length,
                PULONG ErrorOffset);

NTSTATUS
NtfsSetEas(PDEVICE_EXTENSION Vcb,
           PNTFS_FCB Fcb,
           PFILE_FULL_EA_INFORMATION Changes,
           ULONG ChangesLength,
           PULONG ErrorOffset);

NTSTATUS
NtfsQueryEa(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsSetEa(PNTFS_IRP_CONTEXT IrpContext);


/* usnjrnl.c */

VOID
NtfsLoadUsnJournal(PDEVICE_EXTENSION Vcb);

NTSTATUS
NtfsCheckpointUsnJournal(PDEVICE_EXTENSION Vcb);

VOID
NtfsPostUsnChange(PDEVICE_EXTENSION Vcb,
                  PNTFS_FCB Fcb,
                  ULONG Reason);

/* A change to a stream's data: the named-stream reason for a named stream */
FORCEINLINE
ULONG
NtfsUsnDataReason(PNTFS_FCB Fcb,
                  ULONG Reason)
{
    return (Fcb->Stream[0] != UNICODE_NULL) ? Reason << 4 : Reason;
}

VOID
NtfsPostUsnClose(PDEVICE_EXTENSION Vcb,
                 PNTFS_FCB Fcb,
                 ULONG Reason);

NTSTATUS
NtfsQueryUsnJournal(PDEVICE_EXTENSION Vcb,
                    PUSN_JOURNAL_DATA Data);

NTSTATUS
NtfsCreateUsnJournal(PDEVICE_EXTENSION Vcb,
                     PCREATE_USN_JOURNAL_DATA Create);

NTSTATUS
NtfsDeleteUsnJournal(PDEVICE_EXTENSION Vcb,
                     PDELETE_USN_JOURNAL_DATA Delete);

NTSTATUS
NtfsReadUsnJournal(PDEVICE_EXTENSION Vcb,
                   PREAD_USN_JOURNAL_DATA Request,
                   PUCHAR Output,
                   ULONG OutputLength,
                   PULONG Returned);

NTSTATUS
NtfsReadFileUsnData(PDEVICE_EXTENSION Vcb,
                    PNTFS_FCB Fcb,
                    PUCHAR Output,
                    ULONG OutputLength,
                    PULONG Returned);

NTSTATUS
NtfsEnumUsnData(PDEVICE_EXTENSION Vcb,
                PMFT_ENUM_DATA Enum,
                PUCHAR Output,
                ULONG OutputLength,
                PULONG Returned);


/* compressed.c */

NTSTATUS
NtfsReadCompressed(PDEVICE_EXTENSION Vcb,
                   PNTFS_ATTR_CONTEXT Context,
                   ULONGLONG Offset,
                   PUCHAR Buffer,
                   ULONG Length);

NTSTATUS
NtfsWriteCompressed(PDEVICE_EXTENSION Vcb,
                    PNTFS_ATTR_CONTEXT Context,
                    ULONG AttrOffset,
                    PFILE_RECORD_HEADER Holder,
                    ULONGLONG Offset,
                    const UCHAR *Buffer,
                    ULONG Length);

NTSTATUS
NtfsZeroStreamRange(PDEVICE_EXTENSION Vcb,
                    PNTFS_ATTR_CONTEXT Context,
                    ULONG AttrOffset,
                    PFILE_RECORD_HEADER Holder,
                    ULONGLONG Start,
                    ULONGLONG End);

NTSTATUS
NtfsGetCompression(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   PUSHORT Format);

NTSTATUS
NtfsSetCompression(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   USHORT Format);


/* sparse.c */

NTSTATUS
NtfsSetSparse(PDEVICE_EXTENSION Vcb,
              PNTFS_FCB Fcb,
              BOOLEAN SetSparse);

NTSTATUS
NtfsSetZeroData(PDEVICE_EXTENSION Vcb,
                PNTFS_FCB Fcb,
                PFILE_ZERO_DATA_INFORMATION Zero);

NTSTATUS
NtfsQueryAllocatedRanges(PDEVICE_EXTENSION Vcb,
                         PNTFS_FCB Fcb,
                         PFILE_ALLOCATED_RANGE_BUFFER Query,
                         PFILE_ALLOCATED_RANGE_BUFFER Ranges,
                         ULONG RangesLength,
                         PULONG Returned);


/* quotas.c */

VOID
NtfsLoadQuota(PDEVICE_EXTENSION Vcb);

NTSTATUS
NtfsQueryQuota(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsSetQuota(PNTFS_IRP_CONTEXT IrpContext);

BOOLEAN
NtfsQuotaOperation(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsUpdateFileQuota(PDEVICE_EXTENSION Vcb, ULONGLONG MftIndex);

VOID
NtfsReleaseFileQuota(PDEVICE_EXTENSION Vcb, PFILE_RECORD_HEADER Record);

NTSTATUS
NtfsDeleteUserFileLink(PDEVICE_EXTENSION Vcb,
                       ULONGLONG MftIndex,
                       ULONGLONG ParentIndex,
                       PCUNICODE_STRING Name,
                       BOOLEAN CaseSensitive,
                       PUSHORT LinksLeft);

NTSTATUS
NtfsRebuildQuota(PDEVICE_EXTENSION Vcb);

VOID
NtfsInvalidateQuota(PDEVICE_EXTENSION Vcb);

NTSTATUS
NtfsCheckQuotaGrowth(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsQueryFsControl(PDEVICE_EXTENSION Vcb,
                   PFILE_FS_CONTROL_INFORMATION Info,
                   PULONG BufferLength);

NTSTATUS
NtfsSetFsControl(PDEVICE_EXTENSION Vcb,
                 PFILE_FS_CONTROL_INFORMATION Info,
                 ULONG BufferLength);


/* secure.c */

VOID
NtfsInitializeSecurity(PDEVICE_EXTENSION DeviceExt);

NTSTATUS
NtfsReadFileSecurity(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG MftIndex,
                     PSECURITY_DESCRIPTOR *Sd);

NTSTATUS
NtfsAssignSecurity(PDEVICE_EXTENSION DeviceExt,
                   ULONGLONG ParentMftIndex,
                   PACCESS_STATE AccessState,
                   BOOLEAN IsDirectory,
                   PULONG SecurityId);

NTSTATUS
NtfsQuerySecurity(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsSetSecurity(PNTFS_IRP_CONTEXT IrpContext);


/* reparse.c */

ULONG
NtfsGetReparseTag(PDEVICE_EXTENSION DeviceExt,
                  PFILE_RECORD_HEADER FileRecord);

NTSTATUS
NtfsWriteReparsePoint(PDEVICE_EXTENSION DeviceExt,
                      PNTFS_FCB Fcb,
                      PREPARSE_DATA_BUFFER Buffer,
                      ULONG Length);

NTSTATUS
NtfsQueryReparsePoint(PDEVICE_EXTENSION DeviceExt,
                      PNTFS_FCB Fcb,
                      PVOID Output,
                      ULONG OutputLength,
                      PULONG Copied);

NTSTATUS
NtfsRemoveReparsePoint(PDEVICE_EXTENSION DeviceExt,
                       PNTFS_FCB Fcb,
                       PREPARSE_DATA_BUFFER Request,
                       ULONG Length);


/* rw.c */

NTSTATUS
NtfsRead(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsWrite(PNTFS_IRP_CONTEXT IrpContext);


/* volinfo.c */

NTSTATUS
NtfsAllocateClusterRange(PDEVICE_EXTENSION DeviceExt,
                         ULONG FirstCluster,
                         ULONG ClusterCount);

NTSTATUS
NtfsFreeClusterRange(PDEVICE_EXTENSION DeviceExt,
                     ULONG FirstCluster,
                     ULONG ClusterCount);

NTSTATUS
NtfsAllocateClusters(PDEVICE_EXTENSION DeviceExt,
                     ULONG FirstDesiredCluster,
                     ULONG DesiredClusters,
                     PULONG FirstAssignedCluster,
                     PULONG AssignedClusters);

ULONGLONG
NtfsGetFreeClusters(PDEVICE_EXTENSION DeviceExt);

NTSTATUS
NtfsLoadVolumeBitmap(
    _In_ PDEVICE_EXTENSION DeviceExt);

VOID
NtfsFreeVolumeBitmap(
    _In_ PDEVICE_EXTENSION DeviceExt);

NTSTATUS
NtfsExtendVolumeBitmap(PDEVICE_EXTENSION Vcb, ULONG NewClusterCount, PBOOLEAN MetadataTouched);

NTSTATUS
NtfsWriteVolumeBitmap(
    _In_ PDEVICE_EXTENSION DeviceExt,
    _In_ ULONG FirstCluster,
    _In_ ULONG ClusterCount);

NTSTATUS
NtfsQueryVolumeInformation(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsSetVolumeInformation(PNTFS_IRP_CONTEXT IrpContext);


/* ntfs.c */

CODE_SEG("INIT")
DRIVER_INITIALIZE DriverEntry;

CODE_SEG("INIT")
VOID
NTAPI
NtfsInitializeFunctionPointers(PDRIVER_OBJECT DriverObject);

#endif /* NTFS_H */
