#ifndef NTFS_H
#define NTFS_H

#include <ntifs.h>
#include <pseh/pseh2.h>
#include <section_attribs.h>

#define CACHEPAGESIZE(pDeviceExt) \
	((pDeviceExt)->NtfsInfo.UCHARsPerCluster > PAGE_SIZE ? \
	 (pDeviceExt)->NtfsInfo.UCHARsPerCluster : PAGE_SIZE)

#define TAG_NTFS '0ftN'
#define TAG_CCB 'CftN'
#define TAG_FCB 'FftN'
#define TAG_IRP_CTXT 'iftN'
#define TAG_ATT_CTXT 'aftN'
#define TAG_FILE_REC 'rftN'
/* Tag for the separately-allocated SECTION_OBJECT_POINTERS struct that may
 * outlive the FCB itself when MM still references it.  See NtfsDestroyFCB
 * in fcb.c for the rationale: previously the entire FCB (~5 KB including
 * two ERESOURCEs and a FILE_LOCK) was leaked when MmForceSectionClosed
 * could not drop the data section; now only the ~24-byte SOP struct is
 * leaked, and the rest of the FCB is freed normally. */
#define TAG_SOP 'SftN'

#define NTFS_ENABLE_INVESTIGATION_TRACE 0
#define NTFS_ENABLE_POOL_CHECK 0

#if NTFS_ENABLE_INVESTIGATION_TRACE
#define NTFS_TRACE(...) DPRINT1(__VA_ARGS__)
#define NTFS_TRACE_IF(Condition, ...) do { if (Condition) DPRINT1(__VA_ARGS__); } while (0)
#define NTFS_TRACE_ENABLED 1
#else
#define NTFS_TRACE(...) do { } while (0)
#define NTFS_TRACE_IF(Condition, ...) do { } while (0)
#define NTFS_TRACE_ENABLED 0
#endif

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))
#define ROUND_DOWN(N, S) ((N) - ((N) % (S)))

/* Pool corruption detector: validate the pool header of the block containing P.
 * P must be a pool allocation (the pointer returned by ExAllocate*, NOT the header).
 * POOL_HEADER lives at (P - sizeof(POOL_HEADER)).  BlockSize/PreviousSize are in
 * POOL_BLOCK_SIZE units (16 on amd64, 8 on x86).  Bitfield widths differ:
 *   amd64: PreviousSize:8, PoolIndex:8, BlockSize:8, PoolType:8
 *   x86:   PreviousSize:9, PoolIndex:7, BlockSize:9, PoolType:7
 * Read the first ULONG of the header and mask the appropriate width so the check
 * works correctly for allocations up to one page on both architectures. */
#if defined(DBG) && NTFS_ENABLE_POOL_CHECK
#ifdef _WIN64
#define NTFS_POOL_BLOCK 16
#define NTFS_POOL_SZ_MASK 0xFF
#else
#define NTFS_POOL_BLOCK 8
#define NTFS_POOL_SZ_MASK 0x1FF
#endif
static inline void NtfsCheckPoolAround(PVOID P, const char *where)
{
    PUCHAR hdr = (PUCHAR)P - NTFS_POOL_BLOCK;
    ULONG ul1 = *(volatile ULONG *)hdr;
    ULONG our_ps = ul1 & NTFS_POOL_SZ_MASK;
    ULONG our_bs = (ul1 >> 16) & NTFS_POOL_SZ_MASK;
    ULONG our_size = our_bs * NTFS_POOL_BLOCK;
    PUCHAR next_hdr = hdr + our_size;
    if (((ULONG_PTR)next_hdr & ~0xFFF) == ((ULONG_PTR)hdr & ~0xFFF))
    {
        ULONG next_ul1 = *(volatile ULONG *)next_hdr;
        ULONG next_ps = next_ul1 & NTFS_POOL_SZ_MASK;
        if (next_ps != our_bs)
        {
            DbgPrint("NTFS POOL CORRUPTION at %s: block %p (bs=%u) next at %p has PrevSize=%u\n",
                    where, P, our_bs, next_hdr + NTFS_POOL_BLOCK, next_ps);
            ASSERT(FALSE);
        }
    }
    if (our_ps != 0)
    {
        PUCHAR prev_hdr = hdr - our_ps * NTFS_POOL_BLOCK;
        if (((ULONG_PTR)prev_hdr & ~0xFFF) == ((ULONG_PTR)hdr & ~0xFFF))
        {
            ULONG prev_ul1 = *(volatile ULONG *)prev_hdr;
            ULONG prev_bs = (prev_ul1 >> 16) & NTFS_POOL_SZ_MASK;
            if (prev_bs != our_ps)
            {
                DbgPrint("NTFS POOL CORRUPTION at %s: block %p (ps=%u) but prev block at %p has BlockSize=%u\n",
                        where, P, our_ps, prev_hdr + NTFS_POOL_BLOCK, prev_bs);
                ASSERT(FALSE);
            }
        }
    }
}
#define NTFS_CHECK_POOL(P, where) NtfsCheckPoolAround(P, where)
#else
#define NTFS_CHECK_POOL(P, where) ((void)0)
#endif

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

    ERESOURCE DirResource;
//    ERESOURCE FatResource;

    /* Serializes read-modify-write of any directory's $INDEX_ALLOCATION
     * (and the matching $INDEX_ROOT writes) across all FCBs on this volume.
     *
     * Held SHARED by readers (BrowseIndexEntries / NtfsFindMftRecord and
     * the recursive BrowseSubNodeIndexEntries) and EXCLUSIVE by writers
     * (UpdateIndexEntryFileNameSize, NtfsAddFilenameToDirectory,
     *  NtfsRemoveFilenameFromDirectory). Without this, a writer's R/M/W
     * cycle on a parent directory's $INDEX_ALLOCATION can race a concurrent
     * path-walk read of the same cluster and the reader observes the
     * cluster mid-write - most visibly the cluster's first 4 bytes are
     * no longer 'INDX' and BrowseSubNodeIndexEntries trips
     * ASSERT(IndexRecord->Ntfs.Type == NRH_INDX_TYPE) (mft.c:3689). See
     * Kreijstal/reactos#14.
     *
     * Locking discipline:
     *   - Sits BELOW DirResource and Fcb->MainResource in the hierarchy.
     *   - Writers (called from NtfsSetInformation / NtfsWrite) hold the
     *     child Fcb->MainResource but no parent lock; readers from
     *     NtfsCreate / NtfsDirControl hold DirResource exclusive. Neither
     *     pair shares another lock with this one, so no AB-BA is added. */
    ERESOURCE IndexResource;

    /* Serializes the read-modify-write of the volume $Bitmap across
     * NtfsAllocateClusters / FreeClusters. The bitmap is read into a local
     * buffer, modified with RtlFindClearBitsAndSet / RtlClearBits, then
     * written back with WriteAttribute. Without serialization, two
     * concurrent allocators each load a private copy of the bitmap, both
     * see the same LCN as free, both mark it set, and both write the
     * bitmap back - and now two attributes own the same cluster. The
     * symptom is BrowseSubNodeIndexEntries reading a directory's
     * $INDEX_ALLOCATION cluster and finding file data (e.g., a .lnk
     * shortcut) instead of an INDX block, tripping the
     * IndexRecord->Ntfs.Type == NRH_INDX_TYPE assertion in mft.c. See
     * Kreijstal/reactos#14.
     *
     * Locking discipline:
     *   - Always held EXCLUSIVE; the bitmap is short and contended only
     *     during allocation/free, so a shared mode would buy nothing.
     *   - Sits BELOW IndexResource (and therefore below DirResource and
     *     Fcb->MainResource) in the hierarchy: WriteAttribute is the
     *     primary path that triggers cluster allocation, and writers in
     *     mft.c already hold IndexResource exclusive when they call into
     *     it. Acquired recursively from WriteAttribute(bitmap) inside
     *     NtfsAllocateClusters - ERESOURCE supports recursive exclusive. */
    ERESOURCE BitmapResource;

    /* Protects the shared Vcb->MFTContext->pRecord (the in-memory $MFT $DATA
     * attribute record, including its encoded runlist) against the
     * free/realloc/rewrite that MFT growth performs in AddRun.  Held SHARED by
     * ReadFileRecord around its ReadAttribute walk and EXCLUSIVE in AddRun
     * around the pRecord mutation.
     *
     * Locking discipline: this is a LEAF - it is the LOWEST lock in the
     * hierarchy (below BitmapResource).  It must be, because ReadFileRecord is
     * called both standalone and while holding IndexResource exclusive AND
     * while holding BitmapResource exclusive (FreeClusters), so it can only ever
     * be acquired last.  Correspondingly, nothing else is acquired while it is
     * held: AddRun takes it only after its caller already allocated clusters and
     * dropped BitmapResource, and the ReadFileRecord walk under it touches no
     * other NTFS resource. */
    ERESOURCE MftContextResource;

    KSPIN_LOCK FcbListLock;
    LIST_ENTRY FcbListHead;

    /* Directory change notification (IRP_MN_NOTIFY_CHANGE_DIRECTORY).
     * Ported from FastFat/CDFS: the FsRtlNotify* package state.  NotifySync is
     * created at mount and destroyed at dismount; NotifyList holds the pending
     * watch entries.  Without these NtfsDirectoryControl returned
     * STATUS_NOT_IMPLEMENTED, so the shell's overlapped ReadDirectoryChangesW
     * failed synchronously (error 1) instead of pending like FAT/Windows. */
    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY NotifyList;

    PVPB Vpb;
    PDEVICE_OBJECT StorageDevice;
    PFILE_OBJECT StreamFileObject;

    struct _NTFS_ATTR_CONTEXT* MFTContext;
    struct _FILE_RECORD_HEADER* MasterFileTable;
    struct _FCB *VolumeFcb;

    /* Set TRUE once MftContextResource is initialized (end of NtfsMountVolume).
     * ReadFileRecord acquires MftContextResource SHARED around its walk of the
     * shared MFTContext->pRecord, because a concurrent IncreaseMftSize frees and
     * rewrites that encoded runlist in place (AddRun); without the shared
     * acquire a reader can walk a buffer that is being reallocated mid-read and
     * get a truncated result (STATUS_PARTIAL_COPY).  During mount bootstrap
     * (NtfsGetVolumeData reads $Volume before the resource exists) this stays
     * FALSE and the lock is skipped - that path is single-threaded, so there is
     * no MFTContext writer to race. */
    BOOLEAN MftReadLockReady;

    NTFS_INFO NtfsInfo;

    /* Cached free cluster count.  NtfsGetFreeClusters reads the entire
     * $Bitmap attribute sector-by-sector to compute this on each call, which
     * stalls IRP_MJ_QUERY_VOLUME_INFORMATION (held under DirResource shared)
     * for tens of seconds on slow media (USB-MSC) and deadlocks any
     * concurrent IRP_MJ_CREATE waiting for DirResource exclusive.  Match the
     * fastfat policy: compute lazily on first query, cache, never recompute.
     * NtfsAllocateClusters / FreeClusters update CachedFreeClustersValid=FALSE
     * so the next query refreshes.  Counter precision is approximate; this
     * mirrors what Windows reports between flushes. */
    volatile LONG CachedFreeClustersValid;   /* 0 = invalid, 1 = valid */
    ULONGLONG CachedFreeClusters;

    NPAGED_LOOKASIDE_LIST FileRecLookasideList;

    ULONG MftDataOffset;
    ULONG Flags;
    ULONG OpenHandleCount;

    /* Cached MFT $Bitmap for fast free-record allocation */
    PUCHAR MftBitmapBuffer;         /* Original allocation (for freeing) */
    PULONG MftBitmapData;           /* ULONG-aligned pointer within buffer */
    ULONG MftBitmapSize;
    ULONG MftNextFreeHint;          /* Next index to start searching from */

    /* $UsnJrnl first-slice state (Kreijstal/reactos#33).
     *
     * When this volume has a journal, UsnJournalFcb points at an FCB-like
     * shim that owns the MFT index of \$Extend\$UsnJrnl so usn.c can
     * Read/WriteAttribute against its $J (:$DATA, non-resident, sparse)
     * and $Max (:$DATA, resident, 32-byte USN_JOURNAL_DATA_V0) streams
     * without re-walking the directory every time.  NULL means "no
     * journal" - NtfsUsnEmitRecord and the FSCTL dispatcher both gate
     * on that so un-journalled volumes pay nothing.
     *
     * UsnJournalId is the per-lifetime ID callers stamp into every
     * READ_USN_JOURNAL request; it must match the one returned by
     * CREATE_USN_JOURNAL or the read is rejected by Windows.
     * UsnMaxSize / UsnAllocationDelta mirror the on-disk $Max fields.
     * UsnNextUsn is the byte offset inside $J where the *next* record
     * will land.  It's 8-byte aligned and persists across remounts by
     * being (re)written into $Max whenever we emit a record. */
    struct _FCB *UsnJournalFcb;
    ULONGLONG UsnJournalId;
    ULONGLONG UsnMaxSize;
    ULONGLONG UsnAllocationDelta;
    ULONGLONG UsnNextUsn;
    ERESOURCE UsnResource;

    /* Disk-quota first-slice state (Kreijstal/reactos#37).
     *
     * QuotaPresent is TRUE when \$Extend\$Quota exists on this volume;
     * IRP_MJ_{QUERY,SET}_QUOTA returns STATUS_NOT_SUPPORTED when FALSE so
     * un-quota'd volumes behave the same as they do on Windows.
     *
     * QuotaFileMft caches the MFT index of the \$Extend\$Quota file so
     * workers don't re-walk the \$Extend directory on every call.
     *
     * Real $Q (owner-ID -> QUOTA_CONTROL) / $O (SID -> owner-ID) maintenance
     * now goes through the generalised btree.c collation API
     * (NtfsBTree{Insert,Find,Remove}Blob on COLLATION_NTOFS_ULONG /
     * COLLATION_NTOFS_SID).  See quota.c for the worker implementations. */
    BOOLEAN QuotaPresent;
    ULONGLONG QuotaFileMft;

    /* $LogFile Phase 0 + Phase 1 state (Kreijstal/reactos#34).
     *
     * LogFileDirty is set TRUE after NtfsMountVolume runs NtfsLfsParseRestart
     * and the restart pages disagree / fail verification / carry a non-clean
     * CurrentLsn landmark.  When set, NtfsDispatch short-circuits IRP_MJ_WRITE,
     * IRP_MJ_SET_INFORMATION, IRP_MJ_SET_SECURITY, IRP_MJ_SET_EA and every
     * mutating FSCTL with STATUS_MEDIA_WRITE_PROTECTED - the volume mounts
     * read-only until the caller reboots into a log-replaying driver or runs
     * chkdsk externally.  This matches the spec intent of the VPB_READ_ONLY
     * plumbing in the issue body; we use a VCB-private flag because the
     * standard VPB flag list in sdk/include/xdk/iotypes.h has no
     * VPB_READ_ONLY bit and Windows-compatibility forbids inventing one.
     *
     * LogFileLsn / LogFileRestartLen carry the Phase-0 dump state so the
     * FSCTL_NTFS_DUMP_LOGFILE worker has something to return without re-
     * reading $LogFile on every call.  These are populated by the mount
     * gate and never re-touched outside a dismount-rebuild. */
    BOOLEAN LogFileDirty;
    ULONGLONG LogFileLsn;

    /* $LogFile restart-page format version, captured by
     * NtfsLfsEvaluateRestartPair from the primary restart page (header
     * offsets NTFS_LFS_RSTR_{MAJOR,MINOR}_VERSION_OFFSET) once it parses.
     * LogFileVersionUnsupported is set when MajorVersion exceeds
     * NTFS_LFS_SUPPORTED_MAJOR_VERSION (LFS 2.0+, Windows 8+): the log
     * format is newer than this implementation, so NtfsMountVolume ORs
     * VCB_VOLUME_READ_ONLY into Flags even though the log is CLEAN -
     * mirroring how down-level Windows NTFS refuses to write LFS 2.0
     * volumes.  All three stay zero when neither restart page parses
     * (blank / corrupt logs keep today's dirty-gate behavior). */
    USHORT LogFileMajorVersion;
    USHORT LogFileMinorVersion;
    BOOLEAN LogFileVersionUnsupported;

    /* $LogFile Phase 2 slice 2: live write-ahead-logging (WAL) state.
     *
     * LoggingEnabled gates the ENTIRE emission path and defaults to FALSE.
     * When FALSE (the default for every normally-mounted volume) the metadata
     * write path is byte-for-byte identical to a volume with no journal at all:
     * NtfsLfsLogMetadataPage() returns STATUS_SUCCESS immediately without
     * touching $LogFile, no LSN is stamped, and no $Volume DIRTY handshake
     * runs.  Emission without recovery (slice 3) gives zero durability and a
     * wrong format/ordering corrupts a real volume, so this stays OFF until
     * replay exists; it is flipped ON only by the userspace harness (and, in
     * a future slice, by an explicit opt-in once recovery lands).
     *
     * LfsContextValid says whether LfsLogContext / LfsImage are initialised.
     * LfsImage is the in-memory mirror of $LogFile:$DATA the emission layer
     * writes into; LfsImageLength is its byte size; LfsPageSize the log page
     * size.  VolumeDirtyOnDisk tracks whether the on-disk $Volume DIRTY bit is
     * currently set, so the set/clear handshake is idempotent. */
    BOOLEAN LoggingEnabled;
    BOOLEAN LfsContextValid;
    BOOLEAN VolumeDirtyOnDisk;
    /* PLFS_WRITE_CONTEXT - opaque here (the struct is declared further down in
     * this header, after DEVICE_EXTENSION).  Allocated only when logging is
     * turned on, freed at teardown. */
    struct _LFS_WRITE_CONTEXT *LfsLogContext;
    PUCHAR LfsImage;
    ULONG LfsImageLength;
    ULONG LfsPageSize;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION, NTFS_VCB, *PNTFS_VCB;

#define VCB_VOLUME_LOCKED       0x0001
#define VCB_VOLUME_CORRUPT      0x0002
#define VCB_DISMOUNT_PENDING    0x0004
/* Volume is mounted read-only.  Set at mount time when $LogFile Phase-1
 * clean-shutdown gate detects a dirty log; checked by NtfsDispatch to
 * return STATUS_MEDIA_WRITE_PROTECTED on every mutating major function.
 * See lfsparse.c banner for the full read-only-mount rationale. */
#define VCB_VOLUME_READ_ONLY    0x0008

typedef struct
{
    NTFSIDENTIFIER Identifier;
    LIST_ENTRY     NextCCB;
    PFILE_OBJECT   PtrFileObject;
    LARGE_INTEGER  CurrentByteOffset;
    ULONG Flags;
    /* for DirectoryControl */
    ULONG Entry;
    /* for DirectoryControl */
    PWCHAR DirectorySearchPattern;
    ULONG LastCluster;
    ULONG LastOffset;
} NTFS_CCB, *PNTFS_CCB;

#define NTFS_CCB_FLAG_COUNTED 0x00000001
/* User explicitly set the corresponding timestamp through this handle
 * (FileBasicInformation): suppress the implicit update at cleanup */
#define NTFS_CCB_FLAG_USER_SET_CREATION    0x00000002
#define NTFS_CCB_FLAG_USER_SET_LAST_ACCESS 0x00000004
#define NTFS_CCB_FLAG_USER_SET_LAST_WRITE  0x00000008
#define NTFS_CCB_FLAG_USER_SET_CHANGE      0x00000010

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

    /* Documented NTFS tuning knobs from
     * \Registry\Machine\SYSTEM\CurrentControlSet\Control\FileSystem, read
     * once at DriverEntry and sanitized through NtfsSanitizeRegistryUlong
     * (Windows-2003 defaults: last-access updates enabled, 8.3 creation
     * enabled, MFT zone multiplier 1).
     *
     * DisableLastAccessUpdate (NtfsDisableLastAccessUpdate) and
     * Disable8dot3NameCreation (NtfsDisable8dot3NameCreation) are stored
     * for the code paths that will consume them: this driver currently
     * performs no implicit last-access updates on read/enumerate and never
     * generates 8.3 alias names (see AddFileName in attrib.c - it already
     * emulates the 8.3-generation-disabled behavior), so neither knob has
     * a consumer yet.  MftZoneReservation (NtfsMftZoneReservation, 1..4)
     * is copied into each volume's NtfsInfo at mount (NtfsGetVolumeData)
     * for the future MFT-zone-aware cluster allocator. */
    BOOLEAN DisableLastAccessUpdate;
    BOOLEAN Disable8dot3NameCreation;
    ULONG MftZoneReservation;

    /* Zombie FCB list - FCBs we tried to destroy but whose
     * SectionObjectPointers MM still references via a live data or image
     * section.  We can't free the FCB while MM might still issue paging
     * I/O against the section's stored FileObject (whose FsContext points
     * here), so we put the FCB on this list and reap it later, when MM
     * has finally cleared the SOP slots from MmDereferenceSegmentWithLock.
     * Reaping is opportunistic: NtfsCreateFCB and NtfsDestroyFCB walk the
     * list and free any whose SOP is now clean.  Protected by ZombieLock. */
    LIST_ENTRY ZombieFcbList;
    KSPIN_LOCK ZombieLock;
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
#define NTFS_FILE_UPCASE            10
#define NTFS_FILE_EXTEND            11
#define NTFS_FILE_FIRST_USER_FILE   16

#define NTFS_MFT_MASK 0x0000FFFFFFFFFFFFULL

/* Upper bound on how far a regular file's unnamed $DATA allocation is
 * reserved ahead of its data size while it grows (see preallocation in
 * SetNonResidentAttributeDataLength).  Bounds the per-file reserved-but-
 * unwritten overshoot; the excess is reclaimed when the file is closed. */
#define NTFS_DATA_PREALLOC_MAX_BYTES (256 * 1024)

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

/* Win32 file-attribute bits a caller is allowed to set on a freshly created
 * file/directory.  These share the same bit values as the FILE_ATTRIBUTE_*
 * flags, so the create disposition's FileAttributes can be masked with this
 * directly.  The DIRECTORY bit is structural (NTFS_FILE_TYPE_DIRECTORY) and
 * handled separately, so it is deliberately excluded here. */
#define NTFS_FILE_ATTRIBUTE_VALID_FLAGS \
    (NTFS_FILE_TYPE_READ_ONLY | NTFS_FILE_TYPE_HIDDEN | NTFS_FILE_TYPE_SYSTEM | \
     NTFS_FILE_TYPE_ARCHIVE | NTFS_FILE_TYPE_TEMPORARY | NTFS_FILE_TYPE_OFFLINE)

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
#define NRH_RSTR_TYPE  0x52545352  /* 'RSTR' - $LogFile restart page */
#define NRH_RCRD_TYPE  0x44524352  /* 'RCRD' - $LogFile record page */
#define NRH_CHKD_TYPE  0x444B4843  /* 'CHKD' - chkdsk-cleaned page */


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
    ULONGLONG ChangeTime;
    ULONGLONG LastWriteTime;
    ULONGLONG LastAccessTime;
    ULONG FileAttribute;
    ULONG AlignmentOrReserved[3];
#if 0
    ULONG QuotaId;
    ULONG SecurityId;
    ULONGLONG QuotaCharge;
    USN Usn;
#endif
} STANDARD_INFORMATION, *PSTANDARD_INFORMATION;


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
    ULONGLONG ChangeTime;
    ULONGLONG LastWriteTime;
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

typedef struct
{
    PB_TREE_FILENAME_NODE RootNode;
    PDEVICE_EXTENSION Vcb;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    struct _NTFS_ATTR_CONTEXT *IndexAllocationContext;
    /* Collation rule for this B-tree's keys (COLLATION_FILE_NAME by default
     * for $I30 indexes).  Generalised per [MS-FSCC] / NTFS-3G:
     *   COLLATION_BINARY - memcmp on key bytes
     *   COLLATION_FILE_NAME - filename (case-insensitive)
     *   COLLATION_UNICODE_STRING - filename (case-sensitive)
     *   COLLATION_NTOFS_ULONG - 4-byte LE unsigned compare ($Q)
     *   COLLATION_NTOFS_SID - $O keys
     *   COLLATION_NTOFS_SECURITY_HASH - $SDH
     *   COLLATION_NTOFS_ULONGS - $ObjId / n-ULONG compare
     */
    ULONG CollationRule;
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
    /* Phase 4A.5: when MigrateAttributeToList moves this attribute into a child
     * file record, set MigratedToMFTIndex to the child's MFT index.  Subsequent
     * AddRun calls then re-read that child record from disk and operate on it
     * instead of the (caller-supplied) base FileRecord, which no longer holds
     * the attribute slot.  Zero means "not migrated, attribute lives in the
     * base FileRecord". */
    ULONGLONG           MigratedToMFTIndex;
    PNTFS_ATTR_RECORD    pRecord;
} NTFS_ATTR_CONTEXT, *PNTFS_ATTR_CONTEXT;

#define FCB_CACHE_INITIALIZED   0x0001
#define FCB_IS_VOLUME_STREAM    0x0002
#define FCB_IS_VOLUME           0x0004
#define FCB_DELETE_PENDING      0x0008
#define MAX_PATH                260

typedef struct _FCB
{
    NTFSIDENTIFIER Identifier;

    FSRTL_COMMON_FCB_HEADER RFCB;

    /* Pointer to a separately-allocated SECTION_OBJECT_POINTERS struct
     * (tag TAG_SOP).  This struct may outlive the FCB: when MM still
     * holds a reference to the data/image section at FCB destruction
     * time, NtfsDestroyFCB orphans this allocation (clearing the back-
     * pointer here so it isn't double-freed) and frees the rest of the
     * FCB normally.  MmDereferenceSegmentWithLock will eventually clear
     * the leaked SOP's DataSectionObject slot when the segment refcount
     * finally drops to zero - by which point nothing reads it.
     *
     * The previous design embedded the SOP struct directly inside the
     * FCB, which forced the entire FCB (with its two ERESOURCEs, FILE_LOCK
     * and 520-byte path strings, ~5 KB total) to be leaked instead of
     * just the 24-byte pointer struct.  Under any sustained file open/
     * close workload that exhausted paged pool and froze the system. */
    PSECTION_OBJECT_POINTERS SectionObjectPointers;

    /* FsRtl-managed byte-range lock state for IRP_MJ_LOCK_CONTROL.
     * Initialized in NtfsCreateFCB via FsRtlInitializeFileLock and torn
     * down in NtfsDestroyFCB via FsRtlUninitializeFileLock. Used by
     * lock.c!NtfsLockControl through FsRtlProcessFileLock and consulted
     * by NtfsRead / NtfsWrite via FsRtlCheckLockForReadAccess /
     * FsRtlCheckLockForWriteAccess so locks are actually enforced. */
    FILE_LOCK FileLock;

    PFILE_OBJECT FileObject;
    PNTFS_VCB Vcb;

    WCHAR Stream[MAX_PATH];
    WCHAR *ObjectName;		/* point on filename (250 chars max) in PathName */
    WCHAR PathName[MAX_PATH];	/* path+filename 260 max */

    /* Case-folded hash of PathName, kept in sync by every PathName
     * writer (NtfsCreateFCB, NtfsUpdateFcbPathName).  Folding must
     * match _wcsicmp exactly (ASCII A-Z only) so that
     * _wcsicmp(a, b) == 0 implies equal hashes: NtfsGrabFCBFromTable
     * compares hashes first and only falls back to _wcsicmp on a hit,
     * turning the per-open linear scan of the volume FCB list from
     * O(entries * pathlen) string compares into O(entries) word
     * compares. */
    ULONG PathNameHash;

    ERESOURCE PagingIoResource;
    ERESOURCE MainResource;

    LIST_ENTRY FcbListEntry;
    /* When this FCB has been "destroyed" but is still kept alive as a
     * zombie because MM has live references via SectionObjectPointers,
     * it lives on NtfsGlobalData->ZombieFcbList linked through this
     * field instead of FcbListEntry.  See NtfsDestroyFCB for the
     * orphan-vs-zombie decision and NtfsReapZombieFcbs for cleanup. */
    LIST_ENTRY ZombieListEntry;
    struct _FCB* ParentFcb;

    ULONG DirIndex;

    LONG RefCount;
    ULONG Flags;
    ULONG OpenHandleCount;

    /* Share access tracking - used by IoCheckShareAccess /
     * IoSetShareAccess / IoUpdateShareAccess / IoRemoveShareAccess.
     * The MM filter callback PreAcquireForSectionSynchronization
     * inspects ShareAccess.Writers to decide whether to report
     * STATUS_FILE_LOCKED_WITH_WRITERS or STATUS_FILE_LOCKED_WITH_ONLY_READERS. */
    SHARE_ACCESS ShareAccess;

    ULONGLONG MFTIndex;
    USHORT LinkCount;

    FILENAME_ATTRIBUTE Entry;

    /* Cached on-disk MFT record for this FCB.  Populated lazily on the
     * first NtfsReadFile cache miss and reused by subsequent reads to
     * avoid re-decoding the $MFT MCB and re-issuing a sector read on
     * every paging IRP.  Allocated from non-paged pool with TAG_NTFS at
     * BytesPerFileRecord bytes; freed in NtfsDestroyFCB.  Installed
     * lock-free via InterlockedCompareExchangePointer so concurrent
     * shared-resource readers can race the install - the loser frees
     * its buffer.  Invalidated by FCB-aware write paths via
     * NtfsInvalidateCachedFileRecord before they commit changes through
     * UpdateFileRecord, so the next reader re-reads the fresh state. */
    PFILE_RECORD_HEADER CachedFileRecord;

    /* Sequential-read prefetch state.  NextPrefetchOffset is the file
     * offset (bytes) where the last paging read ended -- used to detect
     * a contiguous-read pattern in NtfsReadFile so we can speculatively
     * fault the next window of pages into the data section via
     * MmPrefetchPages.  PrefetchInFlight is a 0/1 atomic guard: only one
     * caller at a time runs the prefetch path on a given FCB, and a
     * re-entrant paging read from inside the prefetch (via the section
     * page-in IRP that lands back on NtfsRead) sees the flag set and
     * bypasses it, so we don't recurse or stack pool allocations. */
    LONGLONG NextPrefetchOffset;
    volatile LONG PrefetchInFlight;

} NTFS_FCB, *PNTFS_FCB;

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

/* Validate one DWORD registry tuning knob.  Pure - no kernel dependencies -
 * so the userspace harness can unit-test the clamping rules directly.
 *
 * @param Found    FALSE when the value was absent or not a REG_DWORD.
 * @param Value    Raw DWORD read from the registry (ignored when !Found).
 * @param Default  Windows-2003 default returned for absent/invalid values.
 * @param Minimum / @param Maximum
 *                 Inclusive valid range; out-of-range values fall back to
 *                 Default (matching how Windows treats a bogus
 *                 NtfsMftZoneReservation), they are NOT saturated. */
FORCEINLINE
ULONG
NtfsSanitizeRegistryUlong(BOOLEAN Found,
                          ULONG Value,
                          ULONG Default,
                          ULONG Minimum,
                          ULONG Maximum)
{
    ASSERT(Minimum <= Maximum);
    ASSERT(Default >= Minimum && Default <= Maximum);

    if (!Found || Value < Minimum || Value > Maximum)
        return Default;

    return Value;
}

/* attrib.c */

//VOID
//NtfsDumpAttribute(PATTRIBUTE Attribute);

NTSTATUS
AddBitmap(PNTFS_VCB Vcb,
          PFILE_RECORD_HEADER FileRecord,
          PNTFS_ATTR_RECORD AttributeAddress,
          PCWSTR Name,
          USHORT NameLength);

NTSTATUS
AddData(PFILE_RECORD_HEADER FileRecord,
        PNTFS_ATTR_RECORD AttributeAddress);

NTSTATUS
AddDataStream(PNTFS_VCB Vcb,
              PFILE_RECORD_HEADER FileRecord,
              PCWSTR Name,
              USHORT NameLength);

NTSTATUS
AddRun(PNTFS_VCB Vcb,
       PNTFS_ATTR_CONTEXT AttrContext,
       ULONG AttrOffset,
       PFILE_RECORD_HEADER FileRecord,
       ULONGLONG NextAssignedCluster,
       ULONG RunLength);

NTSTATUS
MigrateAttributeToList(PNTFS_VCB Vcb,
                       PFILE_RECORD_HEADER BaseFileRecord,
                       PNTFS_ATTR_CONTEXT AttrContext,
                       ULONG AttrOffset,
                       PFILE_RECORD_HEADER *OutChildRecord,
                       PULONG OutNewAttrOffset);

NTSTATUS
CoalesceAttributeFromList(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER BaseFileRecord,
                          ULONG AttributeType,
                          PCWSTR Name,
                          USHORT NameLength);

NTSTATUS
NtfsAddHardLinkSpill(PNTFS_VCB Vcb,
                     PFILE_RECORD_HEADER FileRecord,
                     PFILENAME_ATTRIBUTE NewFileName,
                     ULONG NewFileNameLength);

NTSTATUS
NtfsReadAttributeListValue(PNTFS_VCB Vcb,
                           PNTFS_ATTR_RECORD ListAttr,
                           PUCHAR *Value,
                           PULONG ValueLength);

NTSTATUS
NtfsNonResidentListWriteValue(PNTFS_VCB Vcb,
                              PFILE_RECORD_HEADER BaseFileRecord,
                              PNTFS_ATTR_RECORD ListAttr,
                              PUCHAR Value,
                              ULONG ValueLength);

NTSTATUS
NtfsFreeAttributeListClusters(PNTFS_VCB Vcb,
                              PFILE_RECORD_HEADER BaseFileRecord);

NTSTATUS
NtfsFreeClusterRange(PNTFS_VCB Vcb,
                     ULONGLONG FirstCluster,
                     ULONGLONG ClusterCount);

NTSTATUS
AddIndexAllocation(PNTFS_VCB Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   PNTFS_ATTR_RECORD AttributeAddress,
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

NTSTATUS
AddFileName(PFILE_RECORD_HEADER FileRecord,
            PNTFS_ATTR_RECORD AttributeAddress,
            PDEVICE_EXTENSION DeviceExt,
            PFILE_OBJECT FileObject,
            BOOLEAN CaseSensitive,
            ULONG FileAttributes,
            PULONGLONG ParentMftIndex);

NTSTATUS
AddStandardInformation(PFILE_RECORD_HEADER FileRecord,
                       PNTFS_ATTR_RECORD AttributeAddress,
                       ULONG FileAttributes);

NTSTATUS
ConvertDataRunsToLargeMCB(PUCHAR DataRun,
                          PLARGE_MCB DataRunsMCB,
                          PULONGLONG pNextVBN);

NTSTATUS
ConvertLargeMCBToDataRuns(PLARGE_MCB DataRunsMCB,
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
                      UCHAR NameType,
                      PFILENAME_ATTRIBUTE SpillBuffer);

UCHAR
GetPackedByteCount(LONGLONG NumberToPack,
                   BOOLEAN IsSigned);

NTSTATUS
GetLastClusterInDataRun(PDEVICE_EXTENSION Vcb,
                        PNTFS_ATTR_RECORD Attribute,
                        PULONGLONG LastCluster);

PFILENAME_ATTRIBUTE
GetBestFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                          PFILE_RECORD_HEADER FileRecord,
                          PFILENAME_ATTRIBUTE SpillBuffer);

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
FreeClusters(PNTFS_VCB Vcb,
             PNTFS_ATTR_CONTEXT AttrContext,
             ULONG AttrOffset,
             PFILE_RECORD_HEADER FileRecord,
             ULONG ClustersToFree);

/* blockdev.c */

NTSTATUS
NtfsReadDiskCached(IN PDEVICE_EXTENSION Vcb,
                   IN LONGLONG StartingOffset,
                   IN ULONG Length,
                   IN OUT PUCHAR Buffer);

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
NtfsWriteDiskCached(IN PDEVICE_EXTENSION Vcb,
                    IN LONGLONG StartingOffset,
                    IN ULONG Length,
                    IN const PUCHAR Buffer);

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

/* Pure collation comparator on raw key bytes.  Used directly by view-index
 * code (quota, security, objid) and by CompareTreeKeys under the covers.
 * Returns -1/0/+1 per the collation rule.  Does not know about the END
 * sentinel - callers must filter that out first. */
LONG
NtfsCompareKeyBytes(const VOID *Key1,
                    ULONG Key1Len,
                    const VOID *Key2,
                    ULONG Key2Len,
                    ULONG CollationRule,
                    BOOLEAN CaseSensitive);

/* Generalised B-tree insert for view indexes (non-$I30 keys + values).
 * Builds an INDEX_ENTRY_ATTRIBUTE that carries arbitrary key bytes followed
 * by arbitrary value bytes, and inserts it using the tree's CollationRule.
 * If the key already exists, the value is overwritten in place and
 * STATUS_OBJECT_NAME_COLLISION is NOT returned - caller uses this for
 * insert-or-update semantics (matching Windows $Q/$O behaviour). */
NTSTATUS
NtfsBTreeInsertBlob(PB_TREE Tree,
                    const VOID *KeyBytes,
                    ULONG KeyLen,
                    const VOID *ValueBytes,
                    ULONG ValueLen,
                    ULONG MaxIndexRootSize,
                    ULONG IndexRecordSize);

/* Look up a key in the tree and return a pointer to its index entry.  Used
 * by quota/security for read paths.  Returns STATUS_OBJECT_NAME_NOT_FOUND if
 * not found.  The returned pointer is owned by the tree. */
NTSTATUS
NtfsBTreeFindBlob(PB_TREE Tree,
                  const VOID *KeyBytes,
                  ULONG KeyLen,
                  PINDEX_ENTRY_ATTRIBUTE *FoundEntry);

/* Remove a key from the tree by raw key bytes (view-index removal). */
NTSTATUS
NtfsBTreeRemoveBlob(PB_TREE Tree,
                    const VOID *KeyBytes,
                    ULONG KeyLen);

NTSTATUS
CreateBTreeFromIndex(PDEVICE_EXTENSION Vcb,
                     PFILE_RECORD_HEADER FileRecordWithIndex,
                     PNTFS_ATTR_CONTEXT IndexRootContext,
                     PINDEX_ROOT_ATTRIBUTE IndexRoot,
                     PB_TREE *NewTree);

/* Same, but for arbitrary view indexes (e.g. $Q, $O, $SDH).  The $I30
 * wrapper above forwards here. */
NTSTATUS
CreateBTreeFromIndexEx(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER FileRecordWithIndex,
                       PCWSTR IndexName,
                       ULONG IndexNameLen,
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
DemoteBTreeRoot(PB_TREE Tree,
                 ULONG IndexRecordSize,
                 BOOLEAN CaseSensitive);

VOID
DestroyBTree(PB_TREE Tree);

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

/* Same as CreateEmptyBTree but with a caller-specified collation rule.  For
 * $I30 legacy callers, CreateEmptyBTree stays the plain-filename shape. */
NTSTATUS
CreateEmptyBTreeEx(ULONG CollationRule, PB_TREE *NewTree);

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
NtfsRemoveKey(PB_TREE Tree,
              ULONGLONG FileReference,
              PUNICODE_STRING FileName,
              BOOLEAN CaseSensitive);

NTSTATUS
SplitBTreeNode(PB_TREE Tree,
               PB_TREE_FILENAME_NODE Node,
               PB_TREE_KEY *MedianKey,
               PB_TREE_FILENAME_NODE *NewRightHandSibling,
               BOOLEAN CaseSensitive,
               ULONG IndexRecordSize);

NTSTATUS
UpdateIndexAllocation(PDEVICE_EXTENSION DeviceExt,
                      PB_TREE Tree,
                      ULONG IndexBufferSize,
                      PFILE_RECORD_HEADER FileRecord);

NTSTATUS
UpdateIndexNode(PDEVICE_EXTENSION DeviceExt,
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

NTSTATUS
NtfsCreateDirectory(PDEVICE_EXTENSION DeviceExt,
                    PFILE_OBJECT FileObject,
                    BOOLEAN CaseSensitive,
                    ULONG FileAttributes,
                    BOOLEAN CanWait);

PFILE_RECORD_HEADER
NtfsCreateEmptyFileRecord(PDEVICE_EXTENSION DeviceExt);

NTSTATUS
NtfsCreateFileRecord(PDEVICE_EXTENSION DeviceExt,
                     PFILE_OBJECT FileObject,
                     BOOLEAN CaseSensitive,
                     ULONG FileAttributes,
                     BOOLEAN CanWait);

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

/* Typed-stream class of a parsed open path (see NtfsParseStreamPath).
 * Only the $DATA and $INDEX_ALLOCATION stream types are openable
 * ([MS-FSCC] 2.1.5); the class records which one the name spelled out so
 * the create path can enforce the directory/non-directory requirement
 * once the target has resolved. */
typedef enum _NTFS_STREAM_TYPE
{
    /* "file", "file:name" or "file:name:$DATA" - a $DATA open with no
     * extra target requirement (StreamName distinguishes default vs
     * named stream). */
    NtfsStreamTypeData,
    /* Explicit "file::$DATA" - the default (unnamed) data stream; the
     * target must not be a directory (STATUS_FILE_IS_A_DIRECTORY). */
    NtfsStreamTypeDefaultData,
    /* "dir::$INDEX_ALLOCATION" / "dir:$I30:$INDEX_ALLOCATION" - the
     * directory index stream; the target must be a directory
     * (STATUS_NOT_A_DIRECTORY). */
    NtfsStreamTypeIndexAllocation
} NTFS_STREAM_TYPE, *PNTFS_STREAM_TYPE;

NTSTATUS
NtfsParseStreamPath(PUNICODE_STRING FileName,
                    PUNICODE_STRING StreamName,
                    PNTFS_STREAM_TYPE StreamType);

ULONG
NtfsComputePathNameHash(PCWSTR Path);

PNTFS_FCB
NtfsCreateFCB(PCWSTR FileName,
              PCWSTR Stream,
              PNTFS_VCB Vcb);

VOID
NtfsDestroyFCB(PNTFS_FCB Fcb);

VOID
NtfsReapZombieFcbs(VOID);

VOID
NtfsInvalidateCachedFileRecord(PNTFS_FCB Fcb);

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
                  BOOLEAN CaseSensitive);

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

/* fsctl.c */

NTSTATUS
NtfsFileSystemControl(PNTFS_IRP_CONTEXT IrpContext);


/* mft.c */
NTSTATUS
NtfsAddFilenameToDirectory(PDEVICE_EXTENSION DeviceExt,
                           ULONGLONG DirectoryMftIndex,
                           ULONGLONG FileReferenceNumber,
                           PFILENAME_ATTRIBUTE FilenameAttribute,
                           BOOLEAN CaseSensitive);

NTSTATUS
AddNewMftEntry(PFILE_RECORD_HEADER FileRecord,
               PDEVICE_EXTENSION DeviceExt,
               PULONGLONG DestinationIndex,
               BOOLEAN CanWait);

NTSTATUS
NtfsSetMftBitmapInUse(PDEVICE_EXTENSION DeviceExt,
                      ULONGLONG MftIndex,
                      BOOLEAN InUse,
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

NTSTATUS
WriteAttribute(PDEVICE_EXTENSION Vcb,
               PNTFS_ATTR_CONTEXT Context,
               ULONGLONG Offset,
               const PUCHAR Buffer,
               ULONG Length,
               PULONG LengthWritten,
               PFILE_RECORD_HEADER FileRecord);

ULONGLONG
AttributeDataLength(PNTFS_ATTR_RECORD AttrRecord);

NTSTATUS
NtfsFillRetrievalPointersFromMcb(PLARGE_MCB Mcb,
                                 LONGLONG StartingVcn,
                                 LONGLONG AttributeLastVcn,
                                 PRETRIEVAL_POINTERS_BUFFER OutBuffer,
                                 ULONG OutBufferLength,
                                 PULONG BytesReturned);

/*
 * FSCTL_MOVE_FILE / FSCTL_EXTEND_VOLUME worker primitives.  Extracted per
 * the exemplar that shipped with FSCTL_GET_RETRIEVAL_POINTERS so userspace
 * tests can exercise the MCB surgery and bitmap-grow paths without needing
 * a live IRP.  See Kreijstal/reactos#32.
 */
NTSTATUS
NtfsMoveRunInMcb(PLARGE_MCB Mcb,
                 LONGLONG StartingVcn,
                 LONGLONG SourceLcn,
                 LONGLONG TargetLcn,
                 ULONG ClusterCount);

NTSTATUS
NtfsExtendVolumeBitmap(PDEVICE_EXTENSION Vcb,
                       ULONGLONG OldClusterCount,
                       ULONGLONG NewClusterCount);

NTSTATUS
NtfsRelocateAttributeRange(PDEVICE_EXTENSION Vcb,
                           PNTFS_ATTR_CONTEXT AttrContext,
                           ULONG AttrOffset,
                           PFILE_RECORD_HEADER FileRecord,
                           LONGLONG StartingVcn,
                           LONGLONG TargetLcn,
                           ULONG ClusterCount);

NTSTATUS
AddResidentAttribute(PNTFS_VCB Vcb,
                     PFILE_RECORD_HEADER FileRecord,
                     ULONG Type,
                     PCWSTR Name,
                     USHORT NameLength,
                     const VOID *Data,
                     ULONG DataLength);

NTSTATUS
RemoveResidentAttribute(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER FileRecord,
                        PNTFS_ATTR_RECORD AttrAddress);

NTSTATUS
NtfsSetReparsePointOnRecord(PNTFS_VCB Vcb,
                            PFILE_RECORD_HEADER FileRecord,
                            ULONG Tag,
                            const VOID *Data,
                            ULONG DataLength);

NTSTATUS
NtfsGetReparsePointFromRecord(PNTFS_VCB Vcb,
                              PFILE_RECORD_HEADER FileRecord,
                              PULONG TagOut,
                              PVOID Buffer,
                              ULONG BufferLength,
                              PULONG LengthOut);

NTSTATUS
NtfsDeleteReparsePointFromRecord(PNTFS_VCB Vcb,
                                 PFILE_RECORD_HEADER FileRecord);

NTSTATUS
NtfsSetObjectIdOnRecord(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER FileRecord,
                        const UCHAR ObjectId[16]);

NTSTATUS
NtfsGetObjectIdFromRecord(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER FileRecord,
                          UCHAR ObjectIdOut[16]);

NTSTATUS
NtfsDeleteObjectIdFromRecord(PNTFS_VCB Vcb,
                             PFILE_RECORD_HEADER FileRecord);

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
SetResidentAttributeDataLength(PDEVICE_EXTENSION Vcb,
                               PNTFS_ATTR_CONTEXT AttrContext,
                               ULONG AttrOffset,
                               PFILE_RECORD_HEADER FileRecord,
                               PLARGE_INTEGER DataSize);

ULONGLONG
AttributeAllocatedLength(PNTFS_ATTR_RECORD AttrRecord);

BOOLEAN
CompareFileName(PUNICODE_STRING FileName,
                PINDEX_ENTRY_ATTRIBUTE IndexEntry,
                BOOLEAN DirSearch,
                BOOLEAN CaseSensitive);

NTSTATUS
UpdateMftMirror(PNTFS_VCB Vcb);

NTSTATUS
ReadFileRecord(PDEVICE_EXTENSION Vcb,
               ULONGLONG index,
               PFILE_RECORD_HEADER file);

NTSTATUS
UpdateIndexEntryFileNameSize(PDEVICE_EXTENSION Vcb,
                             PFILE_RECORD_HEADER MftRecord,
                             PCHAR IndexRecord,
                             ULONG IndexBlockSize,
                             PINDEX_ENTRY_ATTRIBUTE FirstEntry,
                             PINDEX_ENTRY_ATTRIBUTE LastEntry,
                             PUNICODE_STRING FileName,
                             PULONG StartEntry,
                             PULONG CurrentEntry,
                             BOOLEAN DirSearch,
                             ULONGLONG NewDataSize,
                             ULONGLONG NewAllocatedSize,
                             BOOLEAN CaseSensitive);

NTSTATUS
UpdateFileNameRecord(PDEVICE_EXTENSION Vcb,
                     ULONGLONG ParentMFTIndex,
                     PUNICODE_STRING FileName,
                     BOOLEAN DirSearch,
                     ULONGLONG NewDataSize,
                     ULONGLONG NewAllocationSize,
                     BOOLEAN CaseSensitive);

NTSTATUS
UpdateFileRecord(PDEVICE_EXTENSION Vcb,
                 ULONGLONG MftIndex,
                 PFILE_RECORD_HEADER FileRecord);

NTSTATUS
NtfsStampFileTimes(PDEVICE_EXTENSION DeviceExt,
                   PNTFS_FCB Fcb);

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

NTSTATUS
NtfsIsDirectoryEmpty(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG DirMFTIndex,
                     BOOLEAN CaseSensitive,
                     PBOOLEAN Empty);

NTSTATUS
NtfsDeleteFileRecord(PDEVICE_EXTENSION DeviceExt,
                     PNTFS_FCB Fcb,
                     PCUNICODE_STRING LinkName,
                     ULONGLONG LinkParentMftIndex,
                     BOOLEAN CaseSensitive);

NTSTATUS
NtfsDeleteStream(PDEVICE_EXTENSION DeviceExt,
                 PNTFS_FCB Fcb);

NTSTATUS
NtfsRemoveFilenameFromDirectory(PDEVICE_EXTENSION DeviceExt,
                                ULONGLONG ParentMftIndex,
                                ULONGLONG FileReferenceNumber,
                                PUNICODE_STRING FileName,
                                BOOLEAN CaseSensitive);

NTSTATUS
NtfsRenameFileRecord(PDEVICE_EXTENSION DeviceExt,
                     PNTFS_FCB Fcb,
                     ULONGLONG NewParentMftIndex,
                     PUNICODE_STRING NewFileName,
                     BOOLEAN ReplaceIfExists,
                     BOOLEAN CaseSensitive);

NTSTATUS
NtfsLinkFileRecord(PDEVICE_EXTENSION DeviceExt,
                   PNTFS_FCB Fcb,
                   ULONGLONG NewParentMftIndex,
                   PUNICODE_STRING NewFileName,
                   BOOLEAN ReplaceIfExists,
                   BOOLEAN CaseSensitive);

VOID
NtfsDumpFileRecord(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord);

/* Size of a caller-provided buffer that can hold a copy of any matched
 * index entry's $FILE_NAME key (fixed part + longest possible name). */
#define NTFS_FOUND_NAME_SIZE \
    (FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + 255 * sizeof(WCHAR))

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


/* rw.c */

NTSTATUS
NtfsRead(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsWrite(PNTFS_IRP_CONTEXT IrpContext);


/* ea.c */

NTSTATUS
NtfsQueryEa(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsSetEa(PNTFS_IRP_CONTEXT IrpContext);


/* lock.c */

NTSTATUS
NtfsLockControl(PNTFS_IRP_CONTEXT IrpContext);


/* pnp.c */

NTSTATUS
NtfsPnp(PNTFS_IRP_CONTEXT IrpContext);


/* shutdown.c */

NTSTATUS
NtfsShutdown(PNTFS_IRP_CONTEXT IrpContext);


/* volinfo.c */

NTSTATUS
NtfsAllocateClusters(PDEVICE_EXTENSION DeviceExt,
                     ULONG FirstDesiredCluster,
                     ULONG DesiredClusters,
                     PULONG FirstAssignedCluster,
                     PULONG AssignedClusters);

ULONGLONG
NtfsGetFreeClusters(PDEVICE_EXTENSION DeviceExt);

NTSTATUS
NtfsQueryVolumeInformation(PNTFS_IRP_CONTEXT IrpContext);

NTSTATUS
NtfsSetVolumeInformation(PNTFS_IRP_CONTEXT IrpContext);


/* security.c */

NTSTATUS
NtfsSetSecurityOnRecord(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER FileRecord,
                        const VOID *SD,
                        ULONG SdLength);

NTSTATUS
NtfsGetSecurityFromRecord(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER FileRecord,
                          PVOID Buf,
                          ULONG BufLen,
                          PULONG LenOut);

NTSTATUS
NtfsDeleteSecurityFromRecord(PNTFS_VCB Vcb,
                             PFILE_RECORD_HEADER FileRecord);

/* usn.c */

NTSTATUS
NtfsUsnLoadJournal(PDEVICE_EXTENSION Vcb);

NTSTATUS
NtfsUsnInitJournal(PDEVICE_EXTENSION Vcb,
                   ULONGLONG MaximumSize,
                   ULONGLONG AllocationDelta);

NTSTATUS
NtfsUsnEmitRecord(PDEVICE_EXTENSION Vcb,
                  ULONGLONG FileRef,
                  ULONGLONG ParentRef,
                  ULONG Reason,
                  ULONG SourceInfo,
                  PCWSTR Name,
                  USHORT NameLength);

NTSTATUS
NtfsUsnReadRange(PDEVICE_EXTENSION Vcb,
                 ULONGLONG StartUsn,
                 ULONG ReasonMask,
                 PVOID Buffer,
                 ULONG BufferLength,
                 PULONG BytesReturned);

NTSTATUS
NtfsUsnEnumerate(PDEVICE_EXTENSION Vcb,
                 ULONGLONG StartRef,
                 LONGLONG LowUsn,
                 LONGLONG HighUsn,
                 PVOID Buffer,
                 ULONG BufferLength,
                 PULONG BytesReturned);

VOID
NtfsUsnFreeJournalFcb(struct _FCB *Fcb);

/* lznt1.c */

NTSTATUS
NtfsLznt1Decompress(const UCHAR *In,
                    ULONG InLen,
                    UCHAR *Out,
                    ULONG OutLen,
                    PULONG OutUsed);

/* compress.c */

/* Raw-cluster read callback used by NtfsCompressedReadLogicalEx so the
 * userspace test harness can inject a file-backed reader (no VCB). */
typedef NTSTATUS (*NtfsCompressedRawReadFn)(PNTFS_ATTR_CONTEXT Context,
                                            LONGLONG Lcn,
                                            ULONG LenBytes,
                                            PUCHAR Buffer,
                                            PVOID Ctx);

NTSTATUS
NtfsCompressedReadLogicalEx(PNTFS_ATTR_CONTEXT Context,
                            ULONGLONG Offset,
                            ULONG Len,
                            PUCHAR Out,
                            ULONG BytesPerCluster,
                            NtfsCompressedRawReadFn RawRead,
                            PVOID RawCtx,
                            PULONG BytesReadOut);

NTSTATUS
NtfsCompressedReadLogical(PDEVICE_EXTENSION Vcb,
                          PNTFS_ATTR_CONTEXT Context,
                          ULONGLONG Offset,
                          ULONG Len,
                          PUCHAR Out,
                          PULONG BytesReadOut);

/* quota.c */

NTSTATUS
NtfsQuotaProbe(PDEVICE_EXTENSION Vcb);

NTSTATUS
NtfsQuotaSetEntry(PDEVICE_EXTENSION Vcb,
                  const UCHAR *Sid,
                  ULONG SidLen,
                  const VOID *Limits,
                  ULONG LimitsLen);

NTSTATUS
NtfsQuotaGetEntry(PDEVICE_EXTENSION Vcb,
                  const UCHAR *Sid,
                  ULONG SidLen,
                  PVOID Limits,
                  ULONG LimitsCap,
                  PULONG LimitsOut);

NTSTATUS
NtfsQuotaEnumerate(PDEVICE_EXTENSION Vcb,
                   ULONG StartEntry,
                   PVOID Buf,
                   ULONG BufLen,
                   PULONG BytesRet);

NTSTATUS
NtfsQuotaDeleteEntry(PDEVICE_EXTENSION Vcb,
                     const UCHAR *Sid,
                     ULONG SidLen);

/* ntfs.c */

CODE_SEG("INIT")
DRIVER_INITIALIZE DriverEntry;

CODE_SEG("INIT")
VOID
NTAPI
NtfsInitializeFunctionPointers(PDRIVER_OBJECT DriverObject);

/* lfsparse.c - $LogFile Phase 0 (read-only parser) + Phase 1 (clean-shutdown
 * gate) structures and worker prototypes.  Layout authority is libfsntfs
 * (Apache/LGPL reference) + Microsoft public LFS docs; ntfs3 (GPL-2.0-only)
 * is algorithm-only reference - NO code copy.  See Kreijstal/reactos#34.
 *
 * The names mirror the libfsntfs wording (RESTART_AREA, LFS_RECORD_HEADER)
 * and, where possible, match the public MSDN/LFS terminology so forensic
 * tools can cross-reference.
 */
#include <pshpack1.h>

/* The fixed-size header prefixing an RSTR / RCRD page.  Reuses the standard
 * NTFS_RECORD_HEADER (the 4-byte magic + UsaOffset + UsaCount + Lsn shared
 * with FILE / INDX records).  Both RSTR and RCRD land at the start of a
 * log page and carry per-sector USA fixups applied at the same stride as
 * MFT records (Vcb->NtfsInfo.BytesPerSector). */

/* Body of an $LogFile RESTART AREA (immediately follows NTFS_RECORD_HEADER
 * at RSTR page offset RestartOffset).  Field-for-field layout per libfsntfs
 * documentation (documentation/NTFS%20File%20System.asciidoc §14). */
typedef struct _NTFS_LFS_RESTART_AREA
{
    ULONGLONG CurrentLsn;               /* 0x00 - last LSN ever written      */
    USHORT    LogClients;               /* 0x08 - count of client records    */
    USHORT    ClientFreeList;           /* 0x0A - head of free client chain  */
    USHORT    ClientInUseList;          /* 0x0C - head of in-use chain       */
    USHORT    Flags;                    /* 0x0E - restart flags              */
    ULONG     SeqNumberBits;            /* 0x10 - bits for the seq number    */
    USHORT    RestartAreaLength;        /* 0x14 - byte length of this area   */
    USHORT    ClientArrayOffset;        /* 0x16 - offset to first LogClient  */
    ULONGLONG FileSize;                 /* 0x18 - size of $LogFile:$DATA     */
    ULONG     LastLsnDataLength;        /* 0x20 - serialised data len of the
                                         *        record at CurrentLsn - the
                                         *        Phase-1 clean-shutdown
                                         *        landmark (must equal zero
                                         *        on a clean-mount restart) */
    USHORT    RecordHeaderLength;       /* 0x24 - fixed len of LFS_RECORD    */
    USHORT    LogPageDataOffset;        /* 0x26 - first byte of client data  */
    ULONG     RestartOpenLogCount;      /* 0x28 - incremented on each open   */
    ULONG     Reserved;                 /* 0x2C - reserved, must be zero     */
} NTFS_LFS_RESTART_AREA, *PNTFS_LFS_RESTART_AREA;

/* Restart flags bits (NTFS_LFS_RESTART_AREA.Flags). */
#define NTFS_LFS_RESTART_FLAG_CLEAN 0x0002   /* Volume dismounted cleanly    */

/* Restart-page header version fields.  The layout authority is our own
 * writer pair (NtfsLfsBuildRestartPage in lfs.c and ntfslib's
 * ntfs_build_rstr_page), which stamps the restart-page header as:
 *   0x10 RestartOffset  0x12 SystemPageSize  0x14 LogPageSize
 *   0x16 RestartOffset (libfsntfs mirror)
 *   0x18 MajorVersion   0x1A MinorVersion
 * A MajorVersion above NTFS_LFS_SUPPORTED_MAJOR_VERSION (LFS 2.0 was
 * introduced by Windows 8) means the log format is newer than this 1.x-era
 * implementation understands; the mount gate forces such volumes read-only
 * instead of risking on-disk state a 2.0 replayer would not expect. */
#define NTFS_LFS_RSTR_MAJOR_VERSION_OFFSET  0x18
#define NTFS_LFS_RSTR_MINOR_VERSION_OFFSET  0x1A
#define NTFS_LFS_SUPPORTED_MAJOR_VERSION    1

/* Body of an $LogFile RCRD page record.  Immediately follows the
 * NTFS_RECORD_HEADER.  Every log record starts on a 512-byte-aligned
 * boundary and carries this header, then a variable-size payload
 * ("LogRecordData") described by RedoLength / UndoLength.  See libfsntfs
 * documentation §14.3. */
typedef struct _NTFS_LFS_RECORD_HEADER
{
    ULONGLONG ThisLsn;                  /* 0x00 - this record's LSN          */
    ULONGLONG ClientPreviousLsn;        /* 0x08 - client's previous LSN      */
    ULONGLONG ClientUndoNextLsn;        /* 0x10 - undo-chain predecessor     */
    ULONG     ClientDataLength;         /* 0x18 - total payload bytes        */
    USHORT    ClientId;                 /* 0x1C - originating client index   */
    USHORT    Alignment;                /* 0x1E - should be 0                */
    ULONG     RecordType;               /* 0x20 - 1=UPDATE 2=CHECKPOINT ...  */
    ULONG     TransactionId;            /* 0x24 - opaque per-TX id           */
    USHORT    Flags;                    /* 0x28 - record flags               */
    USHORT    Reserved1[3];             /* 0x2A                              */
    USHORT    RedoOperation;            /* 0x30 - redo opcode                */
    USHORT    UndoOperation;            /* 0x32 - undo opcode                */
    USHORT    RedoOffset;               /* 0x34 - offset of redo payload     */
    USHORT    RedoLength;               /* 0x36 - size of redo payload       */
    USHORT    UndoOffset;               /* 0x38 - offset of undo payload     */
    USHORT    UndoLength;               /* 0x3A - size of undo payload       */
    USHORT    TargetAttribute;          /* 0x3C - attr index this touches    */
    USHORT    LcnsToFollow;             /* 0x3E - LCN list length            */
    USHORT    RecordOffset;             /* 0x40                              */
    USHORT    AttributeOffset;          /* 0x42                              */
    USHORT    MftClusterIndex;          /* 0x44                              */
    USHORT    Alignment2;               /* 0x46                              */
    ULONGLONG TargetVcn;                /* 0x48                              */
    /* Followed by: LCN[] (LcnsToFollow * 8 bytes), then RedoData / UndoData */
} NTFS_LFS_RECORD_HEADER, *PNTFS_LFS_RECORD_HEADER;

#include <poppack.h>

/* LFS record RecordType values (Microsoft-documented).  Carried here so
 * the Phase-0 dump can render human-readable names; Phase 2+ will use
 * them as opcode dispatch keys. */
#define NTFS_LFS_RECTYPE_NORMAL        0x00000001  /* normal log record      */
#define NTFS_LFS_RECTYPE_CHECKPOINT    0x00000002  /* restart checkpoint     */

/* LFS redo/undo operation codes (Microsoft-documented; the same numbering
 * libfsntfs and ntfs3 use).  Phase 2 (slice 1) only needs Noop, the two
 * page-data ops, and the open/commit/forget transaction markers to format
 * a well-formed record; the full matrix is carried for completeness so the
 * emission layer and a future replayer share one enumeration. */
#define NTFS_LFS_OP_NOOP                       0x00
#define NTFS_LFS_OP_COMPENSATION_LOG_RECORD    0x01
#define NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT  0x02
#define NTFS_LFS_OP_DEALLOCATE_FILE_RECORD_SEGMENT  0x03
#define NTFS_LFS_OP_WRITE_END_OF_FILE_RECORD_SEGMENT 0x04
#define NTFS_LFS_OP_CREATE_ATTRIBUTE           0x05
#define NTFS_LFS_OP_DELETE_ATTRIBUTE           0x06
#define NTFS_LFS_OP_UPDATE_RESIDENT_VALUE      0x07
#define NTFS_LFS_OP_UPDATE_NONRESIDENT_VALUE   0x08
#define NTFS_LFS_OP_UPDATE_MAPPING_PAIRS       0x09
#define NTFS_LFS_OP_SET_NEW_ATTRIBUTE_SIZES    0x0B
#define NTFS_LFS_OP_ADD_INDEX_ENTRY_ROOT       0x0C
#define NTFS_LFS_OP_DELETE_INDEX_ENTRY_ROOT    0x0D
#define NTFS_LFS_OP_ADD_INDEX_ENTRY_ALLOCATION 0x0E
#define NTFS_LFS_OP_DELETE_INDEX_ENTRY_ALLOCATION 0x0F
#define NTFS_LFS_OP_SET_INDEX_ENTRY_VCN_ALLOCATION 0x12
#define NTFS_LFS_OP_UPDATE_FILE_NAME_ROOT      0x13
#define NTFS_LFS_OP_UPDATE_FILE_NAME_ALLOCATION 0x14
#define NTFS_LFS_OP_SET_BITS_IN_NONRESIDENT_BITMAP 0x15
#define NTFS_LFS_OP_CLEAR_BITS_IN_NONRESIDENT_BITMAP 0x16
#define NTFS_LFS_OP_OPEN_NONRESIDENT_ATTRIBUTE 0x1B
#define NTFS_LFS_OP_FORGET_TRANSACTION         0x20

/* NTFS_LFS_RECORD_HEADER.Flags bits. */
#define NTFS_LFS_RECORD_FLAG_MULTI_PAGE        0x0001  /* spans >1 page      */

/* Two header lengths matter for an LFS log record (libfsntfs layout):
 *
 *   NTFS_LFS_RECORD_HEADER_LENGTH (0x30) - the GENERIC LFS record header
 *      (ThisLsn .. Flags + reserved).  ClientDataLength counts everything
 *      AFTER this; the restart area's RecordHeaderLength field carries it.
 *
 *   NTFS_LFS_ATTR_HEADER_LENGTH (0x20) - the NTFS attribute-log-record
 *      sub-header that begins the client data (RedoOperation .. TargetVcn).
 *      It is sizeof(NTFS_LFS_RECORD_HEADER) - 0x30.
 *
 * On-disk a record is: generic header (0x30) | NTFS sub-header (0x20) |
 * LCN list (LcnsToFollow*8) | redo payload | undo payload.  RedoOffset and
 * UndoOffset are measured from the start of the client data (offset 0x30),
 * so a record with no LCNs and the sub-header in place has its redo payload
 * at client-relative 0x20 (absolute 0x50). */
#define NTFS_LFS_RECORD_HEADER_LENGTH          0x30
#define NTFS_LFS_ATTR_HEADER_LENGTH            0x20

/* In-memory write-side emission context (slice 1, Kreijstal/reactos#34).
 *
 * This is the standalone, unit-testable cursor over an in-memory image of
 * $LogFile:$DATA.  It is deliberately NOT wired into the live metadata
 * write path yet; the emission functions mutate only the caller-supplied
 * Image buffer.  Field layout mirrors what a future Vcb-resident logging
 * context would carry.
 *
 *   Image / ImageLength : caller-owned $LogFile:$DATA buffer (>= 4 pages:
 *                         two RSTR copies + at least two RCRD pages).
 *   PageSize / SectorSize : page = USA stride container, sector = USA
 *                         fixup stride (matches Vcb->NtfsInfo).
 *   FirstRcrdOffset     : byte offset of the first RCRD page (2 * PageSize:
 *                         past both restart copies).
 *   SeqNumberBits       : sequence-number bit width copied out of the
 *                         restart area; the LSN = (Seq << (64 - bits)) |
 *                         (offset/8) encoding uses it.
 *   CurrentSeqNumber    : sequence number of the page the write cursor is
 *                         in; bumped each time the RCRD region wraps.
 *   WriteOffset         : byte offset (into Image) of the next free record
 *                         slot, 8-byte aligned.  The record's ThisLsn is
 *                         derived from this offset.
 *   LastLsn             : ThisLsn of the most recently emitted record (0
 *                         before the first emit).
 *   ClientId            : log client index (NTFS uses 0).
 */
typedef struct _LFS_WRITE_CONTEXT
{
    PUCHAR    Image;
    ULONG     ImageLength;
    ULONG     PageSize;
    ULONG     SectorSize;
    ULONG     FirstRcrdOffset;
    ULONG     SeqNumberBits;
    ULONGLONG CurrentSeqNumber;
    ULONG     WriteOffset;
    ULONGLONG LastLsn;
    USHORT    ClientId;
} LFS_WRITE_CONTEXT, *PLFS_WRITE_CONTEXT;

/* Parameters describing one log record to format/emit.  Redo and Undo are
 * the before/after byte images of the metadata change (Windows logs the
 * redo = post-change image and undo = pre-change image so analysis can
 * roll forward committed TXs and roll back uncommitted ones). */
typedef struct _LFS_EMIT_PARAMS
{
    USHORT       RedoOperation;
    USHORT       UndoOperation;
    const VOID  *RedoData;
    USHORT       RedoLength;
    const VOID  *UndoData;
    USHORT       UndoLength;
    ULONG        TransactionId;
    ULONGLONG    ClientPreviousLsn;
    ULONGLONG    ClientUndoNextLsn;
    USHORT       TargetAttribute;
    ULONGLONG    TargetVcn;
    USHORT       RecordOffset;
    USHORT       AttributeOffset;
} LFS_EMIT_PARAMS, *PLFS_EMIT_PARAMS;

/* lfsparse.c worker prototypes. */

/* Parse a single RSTR page from @p Page (size @p PageSize).  Applies USA
 * fixups in place, validates the restart-area pointer and length, and
 * returns STATUS_SUCCESS with @p RestartOut populated when the page is
 * coherent.  @p RestartOffsetOut is optional; callers who want to walk
 * client array use it to locate the base.
 *
 * @return  STATUS_SUCCESS - page looks good, fields populated
 *          STATUS_FILE_CORRUPT_ERROR - magic wrong / USA mismatch
 *          STATUS_INVALID_PARAMETER - buffer shape wrong              */
NTSTATUS
NtfsLfsParseRestartPage(PDEVICE_EXTENSION Vcb,
                        PUCHAR Page,
                        ULONG PageSize,
                        PNTFS_LFS_RESTART_AREA RestartOut,
                        PULONG RestartOffsetOut);

/* Parse a single RCRD page from @p Page (size @p PageSize).  Applies USA
 * fixups in place and returns a pointer to the first LFS_RECORD_HEADER
 * in the page.  Phase-0 only inspects the first record; walking the rest
 * is left to Phase 2+.
 *
 * @return  STATUS_SUCCESS - page looks good, pointer populated
 *          STATUS_FILE_CORRUPT_ERROR - magic wrong / USA mismatch     */
NTSTATUS
NtfsLfsParseRecordPage(PDEVICE_EXTENSION Vcb,
                       PUCHAR Page,
                       ULONG PageSize,
                       PNTFS_LFS_RECORD_HEADER *FirstRecordOut);

/* Phase-1 clean-shutdown gate.  Called from NtfsMountVolume after the
 * MFT is up and before the mount succeeds.  Reads the first two restart
 * pages from $LogFile:$DATA (both copies - primary at offset 0, backup
 * at offset PageSize), parses each, and returns:
 *
 *   STATUS_SUCCESS - both copies agree, CurrentLsn landmarks
 *                             match, and restart flags carry the
 *                             "clean" bit.  Caller mounts RW.
 *   STATUS_LOG_FILE_FULL - pages disagree / landmark mismatch / one
 *                             copy torn.  Caller sets VCB_VOLUME_READ_ONLY
 *                             and mounts RO.
 *
 * Any other NTSTATUS is a hard failure (bad $LogFile MFT record, I/O
 * error reading the attribute); the caller maps it to mount failure.
 *
 * On any return path, the Vcb fields LogFileDirty / LogFileLsn are
 * populated so FSCTL_NTFS_DUMP_LOGFILE can report state. */
NTSTATUS
NtfsLfsCheckCleanShutdown(PDEVICE_EXTENSION Vcb);

/* Worker behind NtfsLfsCheckCleanShutdown: evaluate an already-read
 * primary+backup restart-page pair (@p Pages, 2 * @p PageSize bytes,
 * mutated in place by the USA fixups).  Populates Vcb->LogFileDirty,
 * LogFileLsn and the LogFileMajorVersion / LogFileMinorVersion /
 * LogFileVersionUnsupported capture.  Split out so the userspace harness
 * can drive the mount-time gate against synthetic pages without a full
 * $LogFile fixture.  Returns the same statuses as
 * NtfsLfsCheckCleanShutdown. */
NTSTATUS
NtfsLfsEvaluateRestartPair(PDEVICE_EXTENSION Vcb,
                           PUCHAR Pages,
                           ULONG PageSize);

/* Phase-0 dump FSCTL worker.  Reads the first N pages of $LogFile:$DATA
 * (starting at byte 0), parses each, and appends a human-readable line
 * per page into @p Buffer.  @p BufferLength caps the output; @p BytesOut
 * receives the actual byte count written.  Returns STATUS_BUFFER_OVERFLOW
 * (with BytesOut = required size, truncated output) when the buffer is
 * too small, else STATUS_SUCCESS.  No replay, no emission. */
NTSTATUS
NtfsLfsDumpLogfile(PDEVICE_EXTENSION Vcb,
                   PCHAR Buffer,
                   ULONG BufferLength,
                   PULONG BytesOut);

/* Private FSCTL code for the Phase-0 dump.  Sits in the driver-private
 * range (code 0x300) and uses METHOD_BUFFERED so the I/O Manager copies
 * the result through SystemBuffer.  Not shipped to userspace via
 * winioctl.h - lives here so the kernel dispatch and the userspace test
 * can share the macro through this header.  The userspace test harness's
 * ntfs_mock.h doesn't provide CTL_CODE; gate on its presence so the mock
 * just skips this define instead of failing to compile.  Userspace tests
 * don't need the macro - they call NtfsLfsDumpLogfile directly. */
#if defined(CTL_CODE) && !defined(FSCTL_NTFS_DUMP_LOGFILE)
#define FSCTL_NTFS_DUMP_LOGFILE                                 \
    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x300,                    \
             METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* lfs.c - $LogFile Phase 2 slice 1: write-side emission layer.  These
 * functions are standalone (operate on a caller-owned in-memory image)
 * and are NOT yet wired into the live metadata write path. */

/* Derive the restart-area SeqNumberBits from the $LogFile byte size:
 * 67 - bit_length(FileSize) (ntfs-3g RESTART_AREA::seq_number_bits formula).
 * NOT a constant - it depends on the log size. */
ULONG
NtfsLfsSeqNumberBits(ULONGLONG FileSize);

/* Compose / decompose an LSN.  An LSN packs a sequence number in the high
 * @p SeqNumberBits and the log-file byte offset (divided by 8) in the low
 * (64 - SeqNumberBits) bits - the Microsoft LFS encoding. */
ULONGLONG
NtfsLfsMakeLsn(ULONG SeqNumberBits, ULONGLONG SeqNumber, ULONG ByteOffset);

ULONG
NtfsLfsLsnToOffset(ULONG SeqNumberBits, ULONGLONG Lsn);

/* Build a fresh CLEAN RSTR restart page into @p Page (size @p PageSize),
 * USA stamped via AddFixupArray so NtfsLfsParseRestartPage round-trips it.
 * Mirrors mkntfs's build_rstr_page byte-for-byte so a driver-formatted and
 * a mkntfs-formatted restart page are identical.  @p Clean selects the
 * CLEAN landmark flag; @p CurrentLsn / @p LastLsnDataLength stamp the
 * restart-area landmark fields. */
NTSTATUS
NtfsLfsBuildRestartPage(PDEVICE_EXTENSION Vcb,
                        PUCHAR Page,
                        ULONG PageSize,
                        ULONGLONG FileSize,
                        ULONGLONG CurrentLsn,
                        ULONG LastLsnDataLength,
                        BOOLEAN Clean);

/* Initialise an emission context over @p Image (the full $LogFile:$DATA
 * image).  Lays down two CLEAN RSTR copies + empty RCRD pages and primes
 * the write cursor at the first RCRD page.  @p Image must be at least
 * 4 * PageSize bytes. */
NTSTATUS
NtfsLfsInitWriteContext(PDEVICE_EXTENSION Vcb,
                        PLFS_WRITE_CONTEXT Ctx,
                        PUCHAR Image,
                        ULONG ImageLength,
                        ULONG PageSize);

/* Format one log record (header + redo + undo payload) into @p Buffer.
 * @p BufferLength must be >= NTFS_LFS_RECORD_HEADER_LENGTH + RedoLength +
 * UndoLength.  Fills ThisLsn = @p ThisLsn.  Returns the total record byte
 * length in @p RecordLengthOut.  No USA, no page placement - pure codec. */
NTSTATUS
NtfsLfsFormatRecord(PLFS_WRITE_CONTEXT Ctx,
                    const LFS_EMIT_PARAMS *Params,
                    ULONGLONG ThisLsn,
                    PUCHAR Buffer,
                    ULONG BufferLength,
                    PULONG RecordLengthOut);

/* Emit one log record: format it, place it at the write cursor inside the
 * current RCRD page, assign its ThisLsn from the cursor offset, apply the
 * page USA, advance the cursor, and flip the restart area to dirty with
 * CurrentLsn = the new record's LSN.  @p LsnOut receives the assigned LSN.
 * Records that would cross a page boundary advance to the next page first
 * (slice 1 does not split a record across pages). */
NTSTATUS
NtfsLfsEmitRecord(PLFS_WRITE_CONTEXT Ctx,
                  const LFS_EMIT_PARAMS *Params,
                  PULONGLONG LsnOut);

/* Flush the restart area to the CLEAN state (called at clean dismount):
 * stamp both RSTR copies with CLEAN flag + CurrentLsn = Ctx->LastLsn and
 * LastLsnDataLength = 0 so a subsequent Phase-1 mount sees a clean log. */
NTSTATUS
NtfsLfsWriteRestart(PLFS_WRITE_CONTEXT Ctx,
                    PDEVICE_EXTENSION Vcb,
                    BOOLEAN Clean);

/* On-disk $VOLUME_INFORMATION attribute body (AttributeVolumeInformation,
 * 0x70).  The DIRTY bit lives in Flags.  This is the resident value of the
 * $Volume (MFT #3) file's $VOLUME_INFORMATION attribute. */
typedef struct _NTFS_VOLUME_INFORMATION
{
    ULONGLONG Reserved;     /* 0x00 - always zero                            */
    UCHAR     MajorVersion; /* 0x08                                          */
    UCHAR     MinorVersion; /* 0x09                                          */
    USHORT    Flags;        /* 0x0A - volume flags (DIRTY = 0x0001)          */
} NTFS_VOLUME_INFORMATION, *PNTFS_VOLUME_INFORMATION;

/* $VOLUME_INFORMATION.Flags: the on-disk "volume is dirty" bit chkdsk acts
 * on.  Same numeric value as the public VOLUME_IS_DIRTY FSCTL constant. */
#define NTFS_VOLUME_FLAG_DIRTY  0x0001

/* lfslog.c - $LogFile Phase 2 slice 2: live write-ahead-logging glue.
 *
 * These functions bridge the standalone emission layer (lfs.c) into the live
 * driver.  Every one of them is a NO-OP / pass-through when
 * Vcb->LoggingEnabled is FALSE, which is the default for every mounted volume:
 * a normal mount behaves exactly as it did before this slice. */

/* Initialise the Vcb-resident logging context at mount: read $LogFile:$DATA
 * into an in-memory mirror, parse the existing restart cursor (Phase-0
 * parser) to seed the LSN cursor, and build the LFS_WRITE_CONTEXT.  Does NOT
 * format/overwrite a real $LogFile and does NOT turn logging on - it only
 * makes the context available so NtfsLfsLogMetadataPage can run once
 * LoggingEnabled is set.  Returns STATUS_SUCCESS on a usable context. */
NTSTATUS
NtfsLfsMountInitWriteContext(PDEVICE_EXTENSION Vcb);

/* Tear down the logging context (free the in-memory mirror).  Safe to call
 * whether or not the context was initialised. */
VOID
NtfsLfsTeardownWriteContext(PDEVICE_EXTENSION Vcb);

/* Enable / disable live logging at runtime.  Enabling lazily initialises the
 * context if needed.  Default state is DISABLED. */
NTSTATUS
NtfsLfsSetLoggingEnabled(PDEVICE_EXTENSION Vcb, BOOLEAN Enable);

/* The write-ahead emission hook.  Called from the metadata write path
 * (UpdateFileRecord for MFT FILE_RECORDs, the INDX writer for
 * $INDEX_ALLOCATION pages) BEFORE the metadata page is handed to the disk.
 *
 *   - When Vcb->LoggingEnabled is FALSE: returns STATUS_SUCCESS immediately,
 *     touches nothing, stamps no LSN.  The caller's path is unchanged.
 *   - When TRUE: emits a redo+undo record for the change, stamps the assigned
 *     LSN into *PageLsn (the caller writes it into the page's Lsn field),
 *     flushes the log RCRD page + the dirty restart area to $LogFile:$DATA,
 *     and runs the $Volume DIRTY handshake - all BEFORE the caller writes the
 *     metadata page, preserving the WAL invariant.
 *
 * RedoData/RedoLength + UndoData/UndoLength describe the change; pass the new
 * image as redo and the old image as undo (slice 3 replay consumes them). */
NTSTATUS
NtfsLfsLogMetadataPage(PDEVICE_EXTENSION Vcb,
                       USHORT RedoOperation,
                       USHORT UndoOperation,
                       USHORT TargetAttribute,
                       ULONGLONG TargetVcn,
                       const VOID *RedoData,
                       USHORT RedoLength,
                       const VOID *UndoData,
                       USHORT UndoLength,
                       PULONGLONG PageLsn);

/* Set or clear the on-disk $Volume DIRTY bit.  Idempotent (tracks
 * Vcb->VolumeDirtyOnDisk).  No-op when LoggingEnabled is FALSE. */
NTSTATUS
NtfsLfsSetVolumeDirty(PDEVICE_EXTENSION Vcb, BOOLEAN Dirty);

/* Clean-dismount flush: write the CLEAN restart landmark and clear the
 * on-disk DIRTY bit.  No-op when LoggingEnabled is FALSE. */
NTSTATUS
NtfsLfsDismountFlush(PDEVICE_EXTENSION Vcb);

#endif /* NTFS_H */
