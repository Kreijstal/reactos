/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     LFS record layout, emission and restart recovery
 */
#ifndef NTFS_LFS_WRITE_H
#define NTFS_LFS_WRITE_H
#include "lfs.h"

#ifndef NTFS_VOLUME_FLAG_DIRTY
#define NTFS_VOLUME_FLAG_DIRTY 0x0001
#endif

#ifndef NTFS_LFS_RESTART_FLAG_CLEAN
#include <pshpack1.h>
typedef struct _NTFS_LFS_RESTART_AREA
{
    ULONGLONG CurrentLsn;
    USHORT    LogClients;
    USHORT    ClientFreeList;
    USHORT    ClientInUseList;
    USHORT    Flags;
    ULONG     SeqNumberBits;
    USHORT    RestartAreaLength;
    USHORT    ClientArrayOffset;
    ULONGLONG FileSize;
    ULONG     LastLsnDataLength;
    USHORT    RecordHeaderLength;
    USHORT    LogPageDataOffset;
    ULONG     RestartOpenLogCount;
    ULONG     Reserved;
} NTFS_LFS_RESTART_AREA, *PNTFS_LFS_RESTART_AREA;

#define NTFS_LFS_RESTART_FLAG_CLEAN 0x0002

typedef struct _NTFS_LFS_CLIENT_RECORD
{
    ULONGLONG OldestLsn;
    ULONGLONG ClientRestartLsn;
    USHORT    PrevClient;
    USHORT    NextClient;
    USHORT    SeqNumber;
    UCHAR     Reserved[6];
    ULONG     ClientNameLength;
    WCHAR     ClientName[64];
} NTFS_LFS_CLIENT_RECORD, *PNTFS_LFS_CLIENT_RECORD;

C_ASSERT(sizeof(NTFS_LFS_CLIENT_RECORD) == 0xA0);
C_ASSERT(FIELD_OFFSET(NTFS_LFS_CLIENT_RECORD, ClientNameLength) == 0x1C);
C_ASSERT(FIELD_OFFSET(NTFS_LFS_CLIENT_RECORD, ClientName) == 0x20);

#define NTFS_LFS_CLIENT_NONE        0xFFFF

#define NTFS_LFS_CLIENT_INDEX_NTFS  0
#define NTFS_LFS_CLIENT_NAME_NTFS   L"NTFS"
/* The client record's sequence number, which every record of the client
 * repeats (ntfs-3g logfile.h LOG_CLIENT_ID.seq_number). */
#define NTFS_LFS_CLIENT_SEQ_NTFS    1

#define NTFS_LFS_RSTR_MAJOR_VERSION_OFFSET  0x1C
#define NTFS_LFS_RSTR_MINOR_VERSION_OFFSET  0x1A
#define NTFS_LFS_SUPPORTED_MAJOR_VERSION    1

/* ntfs-3g logfile.h RECORD_PAGE_HEADER: the header of a log record page. */
typedef struct _NTFS_LFS_PAGE_HEADER
{
    NTFS_RECORD_HEADER Header;  /* "RCRD"; Header.Lsn is the last LSN with
                                   bytes on this page */
    ULONG     Flags;
    USHORT    PageCount;
    USHORT    PagePosition;
    USHORT    NextRecordOffset;
    USHORT    Reserved[3];
    ULONGLONG LastEndLsn;       /* the last record that ends on this page */
} NTFS_LFS_PAGE_HEADER, *PNTFS_LFS_PAGE_HEADER;

C_ASSERT(FIELD_OFFSET(NTFS_LFS_PAGE_HEADER, Flags) == 0x10);
C_ASSERT(FIELD_OFFSET(NTFS_LFS_PAGE_HEADER, NextRecordOffset) == 0x18);
C_ASSERT(FIELD_OFFSET(NTFS_LFS_PAGE_HEADER, LastEndLsn) == 0x20);

#define NTFS_LFS_PAGE_FLAG_RECORD_END 0x00000001

typedef struct _NTFS_LFS_RECORD_HEADER
{
    ULONGLONG ThisLsn;
    ULONGLONG ClientPreviousLsn;
    ULONGLONG ClientUndoNextLsn;
    ULONG     ClientDataLength;
    USHORT    ClientId;         /* LOG_CLIENT_ID.seq_number */
    USHORT    Alignment;        /* LOG_CLIENT_ID.client_index */
    ULONG     RecordType;
    ULONG     TransactionId;
    USHORT    Flags;
    USHORT    Reserved1[3];
    /* The NTFS client data starts here, at 0x30. */
    USHORT    RedoOperation;
    USHORT    UndoOperation;
    USHORT    RedoOffset;       /* from the client data */
    USHORT    RedoLength;
    USHORT    UndoOffset;       /* from the client data */
    USHORT    UndoLength;
    USHORT    TargetAttribute;
    USHORT    LcnsToFollow;
    USHORT    RecordOffset;
    USHORT    AttributeOffset;
    USHORT    MftClusterIndex;  /* 512-byte blocks into TargetVcn */
    USHORT    Alignment2;       /* ntfs-3g: attribute_flags */
    ULONGLONG TargetVcn;
} NTFS_LFS_RECORD_HEADER, *PNTFS_LFS_RECORD_HEADER;

#include <poppack.h>

C_ASSERT(FIELD_OFFSET(NTFS_LFS_RECORD_HEADER, RedoOperation) == 0x30);
C_ASSERT(FIELD_OFFSET(NTFS_LFS_RECORD_HEADER, TargetVcn) == 0x48);
C_ASSERT(sizeof(NTFS_LFS_RECORD_HEADER) == 0x50);

#define NTFS_LFS_RECTYPE_NORMAL        0x00000001
#define NTFS_LFS_RECTYPE_CHECKPOINT    0x00000002

/* NTFS log operations (ntfs-3g ntfsrecover's table, in order) */
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
#define NTFS_LFS_OP_DELETE_DIRTY_CLUSTERS      0x0A
#define NTFS_LFS_OP_SET_NEW_ATTRIBUTE_SIZES    0x0B
#define NTFS_LFS_OP_ADD_INDEX_ENTRY_ROOT       0x0C
#define NTFS_LFS_OP_DELETE_INDEX_ENTRY_ROOT    0x0D
#define NTFS_LFS_OP_ADD_INDEX_ENTRY_ALLOCATION 0x0E
#define NTFS_LFS_OP_DELETE_INDEX_ENTRY_ALLOCATION 0x0F
#define NTFS_LFS_OP_WRITE_END_OF_INDEX_BUFFER  0x10
#define NTFS_LFS_OP_SET_INDEX_ENTRY_VCN_ROOT   0x11
#define NTFS_LFS_OP_SET_INDEX_ENTRY_VCN_ALLOCATION 0x12
#define NTFS_LFS_OP_UPDATE_FILE_NAME_ROOT      0x13
#define NTFS_LFS_OP_UPDATE_FILE_NAME_ALLOCATION 0x14
#define NTFS_LFS_OP_SET_BITS_IN_NONRESIDENT_BITMAP 0x15
#define NTFS_LFS_OP_CLEAR_BITS_IN_NONRESIDENT_BITMAP 0x16
#define NTFS_LFS_OP_HOT_FIX                    0x17
#define NTFS_LFS_OP_END_TOP_LEVEL_ACTION       0x18
#define NTFS_LFS_OP_PREPARE_TRANSACTION        0x19
#define NTFS_LFS_OP_COMMIT_TRANSACTION         0x1A
#define NTFS_LFS_OP_FORGET_TRANSACTION         0x1B
#define NTFS_LFS_OP_OPEN_NONRESIDENT_ATTRIBUTE 0x1C
#define NTFS_LFS_OP_OPEN_ATTRIBUTE_TABLE_DUMP  0x1D
#define NTFS_LFS_OP_ATTRIBUTE_NAMES_DUMP       0x1E
#define NTFS_LFS_OP_DIRTY_PAGE_TABLE_DUMP      0x1F
#define NTFS_LFS_OP_TRANSACTION_TABLE_DUMP     0x20

#define NTFS_LFS_RECORD_FLAG_MULTI_PAGE        0x0001

/* ntfs-3g logfile.h ATTRIBUTE_FLAGS: the record's target is a file record */
#define NTFS_LFS_ACTS_ON_MFT                   0x0002

#define NTFS_LFS_RECORD_HEADER_LENGTH          0x30
#define NTFS_LFS_ATTR_HEADER_LENGTH            0x20
/* First record byte of a record page: past the header and the update
 * sequence array of a 4 KiB page with 512-byte sectors (0x28 + 9 words). */
#define NTFS_LFS_PAGE_DATA_OFFSET              0x40
/* LFS 1.1: two restart pages, then two tail-copy pages, then records. */
#define NTFS_LFS_TAIL_COPY_PAGE                2
#define NTFS_LFS_FIRST_RECORD_PAGE             4

/* The codec packs records into the tail page, the last page of the log in
 * use: each record follows the one before it.  A page is staged, update
 * sequence array applied, for its one write to its place in the log once
 * it is full.  The tail page itself is never written to its place: forces
 * write it to the two tail-copy pages in turn, so the copy a torn write
 * destroys is never the only one of an acknowledged record (LFS 1.1). */
typedef struct _LFS_WRITE_CONTEXT
{
    ULONG     LogSize;
    ULONG     PageSize;
    ULONG     SectorSize;
    ULONG     FirstRcrdOffset;
    ULONG     SeqNumberBits;
    ULONGLONG CurrentSeqNumber; /* the sequence of the tail page */
    ULONG     NextPage;         /* the tail page */
    ULONG     TailUsed;         /* its bytes in use: where the next record goes */
    PUCHAR    Tail;             /* its image, no update sequence array applied */
    ULONGLONG LastLsn;
    /* Oldest record still needed: the log space it and later records use
     * may not be reused.  Zero when no record is needed (none logged yet,
     * or a checkpoint made all of them unnecessary); the next record
     * emitted then becomes the oldest. */
    ULONGLONG OldestLsn;
    ULONG     LastDataLength;   /* client data length of LastLsn's record */
    ULONG     NextRestart;      /* restart copy (0 or 1) written next */
    /* Pages past a batch that must stay reusable too: room kept for the
     * records that commit transactions. */
    ULONG     ReservePages;
    USHORT    RestartUsn;
    USHORT    ClientId;
    PUCHAR    Stage;            /* StagePages pages; the last one is Tail */
    ULONG     StagePages;
    ULONG     StagedPages;      /* pages the last batch filled */
    ULONG     StagedFirst;      /* log offset of the first of them */
    PUCHAR    Scratch;          /* ScratchLength bytes: one formatted record */
    ULONG     ScratchLength;
} LFS_WRITE_CONTEXT, *PLFS_WRITE_CONTEXT;

typedef struct _LFS_EMIT_PARAMS
{
    USHORT       RedoOperation;
    USHORT       UndoOperation;
    const VOID  *RedoData;
    USHORT       RedoLength;
    const VOID  *UndoData;
    USHORT       UndoLength;
    ULONG        TransactionId;
    ULONGLONG    ClientUndoNextLsn;
    USHORT       TargetAttribute;
    ULONGLONG    TargetVcn;
    USHORT       RecordOffset;
    USHORT       AttributeOffset;
    USHORT       LcnsToFollow;
    const LONGLONG *Lcns;
    USHORT       ClusterBlockOffset;
    USHORT       AttributeFlags;
} LFS_EMIT_PARAMS, *PLFS_EMIT_PARAMS;

#endif

ULONG
NtfsLfsSeqNumberBits(ULONGLONG FileSize);

ULONGLONG
NtfsLfsMakeLsn(ULONG SeqNumberBits, ULONGLONG SeqNumber, ULONG ByteOffset);

ULONG
NtfsLfsLsnToOffset(ULONG SeqNumberBits, ULONGLONG Lsn);

NTSTATUS
NtfsLfsBuildRestartPage(PDEVICE_EXTENSION Vcb,
                        PUCHAR Page,
                        ULONG PageSize,
                        ULONGLONG FileSize,
                        ULONGLONG CurrentLsn,
                        ULONG LastLsnDataLength,
                        BOOLEAN Clean);

/* The total length of one record carrying these redo and undo lengths. */
ULONG
NtfsLfsRecordLength(ULONG RedoLength, ULONG UndoLength, ULONG LcnsToFollow);

/* Pages a staging buffer needs for a batch of BatchBytes record bytes. */
ULONG
NtfsLfsStagePages(ULONG PageSize, ULONG BatchBytes);

NTSTATUS
NtfsLfsInitWriteContext(PLFS_WRITE_CONTEXT Ctx,
                        ULONG SectorSize,
                        ULONG LogSize,
                        ULONG PageSize,
                        ULONGLONG LastLsn,
                        PUCHAR Stage,
                        ULONG StagePages,
                        PUCHAR Scratch,
                        ULONG ScratchLength);

NTSTATUS
NtfsLfsFormatRecord(PLFS_WRITE_CONTEXT Ctx,
                    const LFS_EMIT_PARAMS *Params,
                    ULONGLONG ThisLsn,
                    ULONGLONG PreviousLsn,
                    PUCHAR Buffer,
                    ULONG BufferLength,
                    PULONG RecordLengthOut);

/* Packs Count records into the tail page and the pages after it, and
 * returns their LSNs.  The pages the batch filled are staged; the tail page
 * is left in Ctx->Tail.  STATUS_LOG_FILE_FULL, with nothing changed, when a
 * page the batch needs still holds a record at or after OldestLsn. */
NTSTATUS
NtfsLfsEmitBatch(PLFS_WRITE_CONTEXT Ctx,
                 const LFS_EMIT_PARAMS *Params,
                 ULONG Count,
                 PULONGLONG Lsns);

/* The LSN the next batch's first record will get. */
ULONGLONG
NtfsLfsNextLsn(PLFS_WRITE_CONTEXT Ctx);

/* Whether the tail page holds records. */
BOOLEAN
NtfsLfsTailHasRecords(PLFS_WRITE_CONTEXT Ctx);

/* A tail copy of the tail page image Image (of the page at TailOffset)
 * into Page, its update sequence array applied with Usn: Header.Lsn holds
 * TailOffset (LFS's Copy.FileOffset), LastEndLsn its last record. */
VOID
NtfsLfsBuildTailCopy(PLFS_WRITE_CONTEXT Ctx,
                     const UCHAR *Image,
                     ULONG TailOffset,
                     USHORT Usn,
                     PUCHAR Page);

/* The log offset of staged page Index of the last batch. */
ULONG
NtfsLfsStagedPageOffset(PLFS_WRITE_CONTEXT Ctx, ULONG Index);

/* Builds the next restart page (the copies alternate) into Page and returns
 * its log offset. */
NTSTATUS
NtfsLfsBuildNextRestart(PLFS_WRITE_CONTEXT Ctx,
                        BOOLEAN Clean,
                        PUCHAR Page,
                        PULONG OffsetOut);

typedef NTSTATUS NTFS_LOG_WRITE_CALLBACK(PDEVICE_EXTENSION Vcb, LONGLONG Offset,
                                          ULONG Length, const PUCHAR Buffer);
typedef NTFS_LOG_WRITE_CALLBACK *PNTFS_LOG_WRITE_CALLBACK;
/* Length 0: makes every write so far durable, the volume stream's cached
 * ones included.  MAXULONG: a storage barrier for the direct writes.
 * Otherwise writes the volume stream's dirty pages from Offset on. */
typedef NTSTATUS NTFS_LOG_FLUSH_CALLBACK(PDEVICE_EXTENSION Vcb, LONGLONG Offset, ULONG Length);
typedef NTFS_LOG_FLUSH_CALLBACK *PNTFS_LOG_FLUSH_CALLBACK;

/* Log pages buffered before a force writes them. */
#define NTFS_LOG_QUEUE_PAGES 64

NTSTATUS NtfsLfsCreateWriter(PDEVICE_EXTENSION Vcb, PNTFS_LOG_WRITE_CALLBACK WriteDirect,
                             PNTFS_LOG_FLUSH_CALLBACK Flush, PVOID *WriterOut);
/* Puts every record logged so far on disk, wherever durability is asked
 * for. */
NTSTATUS NtfsLfsForce(PVOID Writer);
/* Before Length bytes of the volume stream at Offset are written: puts on
 * disk every record of a change they may hold. */
NTSTATUS NtfsLfsForceRange(PVOID Writer, LONGLONG Offset, ULONG Length);
NTSTATUS NtfsLfsEnableWriter(PVOID Writer);
/* Volume bytes a logged write changes. */
typedef struct _NTFS_LOG_DIRTY
{
    LONGLONG Offset;
    ULONG Length;
} NTFS_LOG_DIRTY, *PNTFS_LOG_DIRTY;

#define NTFS_LOG_PENDING_EXTENTS 16

/* A logged write that has not been issued yet.  The caller keeps it from
 * NtfsLfsLogFileRecord until NtfsLfsFileRecordWritten, which a checkpoint
 * needs to know: until then its records must stay in the log; then its
 * bytes are dirty in the volume stream until a checkpoint writes them. */
typedef struct _NTFS_LOG_PENDING
{
    LIST_ENTRY Link;
    ULONGLONG Lsn;
    ULONG ExtentCount;          /* MAXULONG: more than fit here */
    NTFS_LOG_DIRTY Extents[NTFS_LOG_PENDING_EXTENTS];
} NTFS_LOG_PENDING, *PNTFS_LOG_PENDING;

/* A request in progress on a thread: the records it logs form one
 * transaction, committed when it ends.  The caller provides the storage
 * (NtfsDispatch, on its stack); nested requests of the thread join it. */
typedef struct _NTFS_LOG_REQUEST
{
    LIST_ENTRY Link;
    PKTHREAD Thread;
    ULONG Session;
    ULONG TransactionId;        /* 0 until it logs a record */
    ULONGLONG FirstLsn;         /* its first record */
    BOOLEAN Joined;             /* nested: the outer request is the one */
    BOOLEAN Forced;             /* committed early, for a full log */
} NTFS_LOG_REQUEST, *PNTFS_LOG_REQUEST;

VOID NtfsLfsBeginRequest(PVOID Writer, PNTFS_LOG_REQUEST Request);
NTSTATUS NtfsLfsEndRequest(PVOID Writer, PNTFS_LOG_REQUEST Request, BOOLEAN ForceLog);

/* Logs the bytes a file record write changes, Image being what is written
 * (update sequence array applied); Pending->Lsn receives the LSN to stamp
 * on the record, 0 when nothing changes. */
NTSTATUS NtfsLfsLogFileRecord(PVOID Writer, ULONGLONG MftIndex, PFILE_RECORD_HEADER Image,
                              PNTFS_LOG_PENDING Pending);
VOID NtfsLfsFileRecordWritten(PVOID Writer, PNTFS_LOG_PENDING Pending);
/* Around a metadata write the log does not describe: the log then tells
 * recovery that metadata outside it may be out of date. */
NTSTATUS NtfsLfsBeginUnloggedWrite(PVOID Writer);
VOID NtfsLfsEndUnloggedWrite(PVOID Writer);

/* A piece of a non-resident metadata write: volume bytes and where in the
 * caller's buffer they come from. */
typedef struct _NTFS_LOG_EXTENT
{
    LONGLONG DiskOffset;
    ULONG Length;
    ULONG BufferOffset;
} NTFS_LOG_EXTENT, *PNTFS_LOG_EXTENT;

typedef struct _NTFS_LOG_METADATA_WRITE
{
    NTFS_LOG_PENDING Pending;
    BOOLEAN Unlogged;
} NTFS_LOG_METADATA_WRITE, *PNTFS_LOG_METADATA_WRITE;

/* The largest non-resident metadata write logged, in at most this many
 * extents, changing at most this many bytes; any other one is written
 * around NtfsLfsBeginUnloggedWrite. */
#define NTFS_LOG_MAX_METADATA_WRITE  0x100000
#define NTFS_LOG_METADATA_EXTENTS    32
#define NTFS_LOG_MAX_CHANGED         0x10000
/* A record carries at most this many changed bytes, over at most this
 * many clusters; a write is logged in at most this many records. */
#define NTFS_LOG_METADATA_CHUNK      0x1000
#define NTFS_LOG_METADATA_LCNS       (NTFS_LOG_METADATA_CHUNK / 512 + 2)
#define NTFS_LOG_BATCH_RECORDS       64

/* Logs the bytes a non-resident metadata write changes, before it is
 * issued; NtfsLfsEndMetadataWrite once it has been. */
NTSTATUS NtfsLfsBeginMetadataWrite(PVOID Writer, const NTFS_LOG_EXTENT *Extents, ULONG Count,
                                   const UCHAR *Buffer, ULONGLONG Offset,
                                   PNTFS_LOG_METADATA_WRITE Write);
VOID NtfsLfsEndMetadataWrite(PVOID Writer, PNTFS_LOG_METADATA_WRITE Write);
/* Every metadata write issued so far is to be made durable: the records
 * describing them are no longer needed. */
NTSTATUS NtfsLfsCheckpoint(PVOID Writer);
NTSTATUS NtfsLfsFinishWriter(PVOID Writer);
VOID NtfsLfsDestroyWriter(PVOID Writer);

BOOLEAN NtfsLfsWriterEnabled(PVOID Writer);
NTSTATUS NtfsLfsSuspendWriter(PVOID Writer);
NTSTATUS NtfsLfsResumeWriter(PVOID Writer);

/* Restart recovery (lfsrecov.c) */
typedef struct _NTFS_LOG_RECOVERY_RESULT
{
    ULONG Records;          /* records analysed */
    ULONG Redone;           /* redo images written */
    ULONG Undone;           /* undo images written */
    BOOLEAN SystemRecords;  /* a file record below 16 was rewritten */
    BOOLEAN NeedsCheck;     /* metadata outside the log may be stale */
} NTFS_LOG_RECOVERY_RESULT, *PNTFS_LOG_RECOVERY_RESULT;

NTSTATUS NtfsLfsRecover(PDEVICE_EXTENSION Vcb, PNTFS_LOG_RECOVERY_RESULT Result);

#endif
