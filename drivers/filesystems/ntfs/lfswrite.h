/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     Experimental LFS emission, disabled by default
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

#define NTFS_LFS_RSTR_MAJOR_VERSION_OFFSET  0x1C
#define NTFS_LFS_RSTR_MINOR_VERSION_OFFSET  0x1A
#define NTFS_LFS_SUPPORTED_MAJOR_VERSION    1

typedef struct _NTFS_LFS_RECORD_HEADER
{
    ULONGLONG ThisLsn;
    ULONGLONG ClientPreviousLsn;
    ULONGLONG ClientUndoNextLsn;
    ULONG     ClientDataLength;
    USHORT    ClientId;
    USHORT    Alignment;
    ULONG     RecordType;
    ULONG     TransactionId;
    USHORT    Flags;
    USHORT    Reserved1[3];
    USHORT    RedoOperation;
    USHORT    UndoOperation;
    USHORT    RedoOffset;
    USHORT    RedoLength;
    USHORT    UndoOffset;
    USHORT    UndoLength;
    USHORT    TargetAttribute;
    USHORT    LcnsToFollow;
    USHORT    RecordOffset;
    USHORT    AttributeOffset;
    USHORT    MftClusterIndex;
    USHORT    Alignment2;
    ULONGLONG TargetVcn;

} NTFS_LFS_RECORD_HEADER, *PNTFS_LFS_RECORD_HEADER;

#include <poppack.h>

#define NTFS_LFS_RECTYPE_NORMAL        0x00000001
#define NTFS_LFS_RECTYPE_CHECKPOINT    0x00000002

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

#define NTFS_LFS_RECORD_FLAG_MULTI_PAGE        0x0001

#define NTFS_LFS_RECORD_HEADER_LENGTH          0x30
#define NTFS_LFS_ATTR_HEADER_LENGTH            0x20

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
    USHORT       LcnsToFollow;
    const LONGLONG *Lcns;
    USHORT       ClusterBlockOffset;
    USHORT       TargetBlockSize;
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

NTSTATUS
NtfsLfsInitWriteContext(PDEVICE_EXTENSION Vcb,
                        PLFS_WRITE_CONTEXT Ctx,
                        PUCHAR Image,
                        ULONG ImageLength,
                        ULONG PageSize);

NTSTATUS
NtfsLfsFormatRecord(PLFS_WRITE_CONTEXT Ctx,
                    const LFS_EMIT_PARAMS *Params,
                    ULONGLONG ThisLsn,
                    PUCHAR Buffer,
                    ULONG BufferLength,
                    PULONG RecordLengthOut);

NTSTATUS
NtfsLfsEmitRecord(PLFS_WRITE_CONTEXT Ctx,
                  const LFS_EMIT_PARAMS *Params,
                  PULONGLONG LsnOut);

NTSTATUS
NtfsLfsWriteRestart(PLFS_WRITE_CONTEXT Ctx,
                    PDEVICE_EXTENSION Vcb,
                    BOOLEAN Clean);

typedef NTSTATUS NTFS_LOG_WRITE_CALLBACK(PDEVICE_EXTENSION Vcb, LONGLONG Offset,
                                          ULONG Length, const PUCHAR Buffer);
typedef NTFS_LOG_WRITE_CALLBACK *PNTFS_LOG_WRITE_CALLBACK;
typedef NTSTATUS NTFS_LOG_FLUSH_CALLBACK(PDEVICE_EXTENSION Vcb);
typedef NTFS_LOG_FLUSH_CALLBACK *PNTFS_LOG_FLUSH_CALLBACK;
typedef NTSTATUS NTFS_LOG_DIRTY_CALLBACK(PDEVICE_EXTENSION Vcb, BOOLEAN Dirty);
typedef NTFS_LOG_DIRTY_CALLBACK *PNTFS_LOG_DIRTY_CALLBACK;

NTSTATUS NtfsLfsCreateWriter(PDEVICE_EXTENSION Vcb, PNTFS_LOG_WRITE_CALLBACK Write,
                             PNTFS_LOG_FLUSH_CALLBACK Flush,
                             PNTFS_LOG_DIRTY_CALLBACK SetDirty, PVOID *WriterOut);
NTSTATUS NtfsLfsEnableWriter(PVOID Writer);
NTSTATUS NtfsLfsLogFileRecord(PVOID Writer, ULONGLONG MftIndex, PFILE_RECORD_HEADER Record);
NTSTATUS NtfsLfsFinishWriter(PVOID Writer);
VOID NtfsLfsDestroyWriter(PVOID Writer);

BOOLEAN NtfsLfsWriterEnabled(PVOID Writer);
NTSTATUS NtfsLfsSuspendWriter(PVOID Writer);
NTSTATUS NtfsLfsResumeWriter(PVOID Writer);

#endif
