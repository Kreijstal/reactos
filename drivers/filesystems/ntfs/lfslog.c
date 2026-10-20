/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     $LogFile write-ahead logging of file records
 *
 * Metadata is logged before it is written: a file record update as an
 * InitializeFileRecordSegment record carrying its after image (redo) and
 * before image (undo), any other non-resident metadata write (index
 * buffers, bitmaps, $Secure, attribute lists) as UpdateNonResidentValue
 * records carrying the bytes written and the bytes they replace.
 *
 * As LFS does, records are buffered: a batch goes into the volume stream's
 * cache and onto a queue, and the queue is forced to disk (written, then a
 * storage barrier) before any metadata page of the volume stream is
 * written (NtfsLfsForce, from the paging write), by an explicit flush, and
 * when the queue fills.  The restart area is written when a session logs
 * its first record (marking the log dirty), at checkpoints and when the
 * log is closed; recovery finds the records after it by scanning forward.
 *
 * The records a request logs (NtfsLfsBeginRequest..NtfsLfsEndRequest, one
 * top-level IRP) form one transaction, committed by a ForgetTransaction
 * record when it ends: recovery undoes a transaction that did not commit,
 * so an operation interrupted half way leaves nothing behind.  Records
 * logged outside a request commit at once.
 *
 * A metadata write too large to log (a whole volume bitmap) is written
 * around NtfsLfsBeginUnloggedWrite, which logs a Noop record once per
 * checkpoint interval: recovery then schedules chkdsk.
 *
 * The log is reused as a circular buffer.  When it is full, a checkpoint
 * flushes the volume: every logged write that has been issued is then on
 * disk, and only records still needed stay in the log: of writes not
 * issued yet (the pending list), of transactions still open, and the Noop
 * record of an unlogged write in progress.  A page is kept free behind
 * every batch for the records that commit; a transaction that alone keeps
 * the log full is committed early.  Storage barriers are supplied by the
 * mount layer; the codec has no dependency on cache-manager or
 * storage-device implementation details.
 */
#include "lfswrite.h"
#define NDEBUG
#include <debug.h>

typedef struct _NTFS_LOG_WRITER
{
    PDEVICE_EXTENSION Vcb;
    ERESOURCE Resource;
    BOOLEAN InIo;
    PNTFS_LOG_WRITE_CALLBACK Write;         /* through the volume stream */
    PNTFS_LOG_WRITE_CALLBACK WriteDirect;   /* to the device */
    PNTFS_LOG_FLUSH_CALLBACK Flush;
    /* Batches not forced yet: page images and their log offsets, and a
     * restart page to write after them.  ForceResource guards them; it is
     * never held while waiting for the cache manager. */
    ERESOURCE ForceResource;
    PUCHAR Queue;
    ULONG QueueOffset[NTFS_LOG_QUEUE_PAGES];
    ULONG QueueCount;
    BOOLEAN RestartQueued;
    ULONG RestartOffset;
    BOOLEAN RestartDirty;       /* the restart area on disk is dirty */
    PNTFS_ATTR_CONTEXT Data;
    LFS_WRITE_CONTEXT Codec;
    PUCHAR Restart;             /* one page: the queued restart */
    PUCHAR RestartBuild;        /* one page: the next one, being built */
    BOOLEAN Enabled;
    BOOLEAN Paused;
    ULONG NextTransaction;
    ULONG Session;              /* changes each time the writer is enabled */
    LIST_ENTRY Requests;        /* NTFS_LOG_REQUEST, guarded by PendingLock */
    KSPIN_LOCK PendingLock;
    LIST_ENTRY PendingList;     /* NTFS_LOG_PENDING, logged but not written */
    /* Guarded by PendingLock: unlogged writes in progress, and the Noop
     * record that covers them in this checkpoint interval (0: none yet). */
    ULONG UnloggedWrites;
    ULONGLONG MarkerLsn;
} NTFS_LOG_WRITER, *PNTFS_LOG_WRITER;

NTSTATUS
NtfsLfsCreateWriter(PDEVICE_EXTENSION Vcb,
                    PNTFS_LOG_WRITE_CALLBACK Write,
                    PNTFS_LOG_WRITE_CALLBACK WriteDirect,
                    PNTFS_LOG_FLUSH_CALLBACK Flush,
                    PVOID *WriterOut)
{
    PNTFS_LOG_WRITER Writer;
    NTSTATUS Status;

    *WriterOut = NULL;
    Writer = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Writer), TAG_NTFS);
    if (Writer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Writer, sizeof(*Writer));
    Status = ExInitializeResourceLite(&Writer->Resource);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Writer, TAG_NTFS);
        return Status;
    }
    Status = ExInitializeResourceLite(&Writer->ForceResource);
    if (!NT_SUCCESS(Status))
    {
        ExDeleteResourceLite(&Writer->Resource);
        ExFreePoolWithTag(Writer, TAG_NTFS);
        return Status;
    }
    Writer->Vcb = Vcb;
    Writer->Write = Write;
    Writer->WriteDirect = WriteDirect;
    Writer->Flush = Flush;
    KeInitializeSpinLock(&Writer->PendingLock);
    InitializeListHead(&Writer->PendingList);
    InitializeListHead(&Writer->Requests);
    *WriterOut = Writer;
    return STATUS_SUCCESS;
}

static VOID
NtfsLogFreeSession(PNTFS_LOG_WRITER Writer)
{
    if (Writer->Data != NULL) { ReleaseAttributeContext(Writer->Data); Writer->Data = NULL; }
    if (Writer->Codec.Stage != NULL) ExFreePoolWithTag(Writer->Codec.Stage, TAG_NTFS);
    if (Writer->Codec.Scratch != NULL) ExFreePoolWithTag(Writer->Codec.Scratch, TAG_NTFS);
    if (Writer->Restart != NULL) ExFreePoolWithTag(Writer->Restart, TAG_NTFS);
    if (Writer->RestartBuild != NULL) ExFreePoolWithTag(Writer->RestartBuild, TAG_NTFS);
    if (Writer->Queue != NULL) ExFreePoolWithTag(Writer->Queue, TAG_NTFS);
    Writer->Restart = NULL;
    Writer->RestartBuild = NULL;
    Writer->Queue = NULL;
    Writer->QueueCount = 0;
    Writer->RestartQueued = FALSE;
    RtlZeroMemory(&Writer->Codec, sizeof(Writer->Codec));
}

typedef enum _LOG_IO
{
    LogIoCached,    /* into the volume stream's cache */
    LogIoDirect     /* to the device */
} LOG_IO;

/* $LogFile has a fixed, fully allocated mapping for this session. Writing
 * mapped runs cannot recurse through a read of its own MFT record. */
static NTSTATUS
NtfsLogRange(PNTFS_LOG_WRITER Writer, ULONG Offset, ULONG Length, PUCHAR Buffer, LOG_IO Io)
{
    PDEVICE_EXTENSION Vcb = Writer->Vcb;
    ULONGLONG Position = Offset, Available, Disk;
    LONGLONG Lcn, RunLength;
    ULONG Within, Part;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource) ||
           ExIsResourceAcquiredExclusiveLite(&Writer->ForceResource));
    ASSERT(Offset <= Writer->Codec.LogSize && Length <= Writer->Codec.LogSize - Offset);
    while (Length != 0)
    {
        if (!FsRtlLookupLargeMcbEntry(&Writer->Data->DataRunsMCB,
                                      Position / Vcb->NtfsInfo.BytesPerCluster,
                                      &Lcn, &RunLength, NULL, NULL, NULL) || Lcn < 0 ||
            (ULONGLONG)Lcn >= Vcb->NtfsInfo.ClusterCount || RunLength <= 0 ||
            (ULONGLONG)RunLength > Vcb->NtfsInfo.ClusterCount - Lcn)
            return STATUS_FILE_CORRUPT_ERROR;
        Within = Position % Vcb->NtfsInfo.BytesPerCluster;
        Available = (ULONGLONG)RunLength * Vcb->NtfsInfo.BytesPerCluster - Within;
        Part = (ULONG)min((ULONGLONG)Length, Available);
        Disk = (ULONGLONG)Lcn * Vcb->NtfsInfo.BytesPerCluster + Within;
        if (Io == LogIoCached)
            Status = Writer->Write(Vcb, Disk, Part, Buffer + (Position - Offset));
        else
            Status = Writer->WriteDirect(Vcb, Disk, Part, Buffer + (Position - Offset));
        if (!NT_SUCCESS(Status)) return Status;
        Position += Part;
        Length -= Part;
    }
    return STATUS_SUCCESS;
}

/* Writes the queued batches to the device, then the queued restart page,
 * each behind a storage barrier: a restart page never names a record that
 * is not on disk. */
static NTSTATUS
NtfsLogForceLocked(PNTFS_LOG_WRITER Writer)
{
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->ForceResource));
    if (Writer->QueueCount != 0)
    {
        for (i = 0; NT_SUCCESS(Status) && i < Writer->QueueCount; i++)
            Status = NtfsLogRange(Writer, Writer->QueueOffset[i], Writer->Codec.PageSize,
                                  Writer->Queue + i * Writer->Codec.PageSize, LogIoDirect);
        if (NT_SUCCESS(Status))
            Status = Writer->Flush(Writer->Vcb, 0, MAXULONG);
        if (NT_SUCCESS(Status))
            Writer->QueueCount = 0;
    }
    if (NT_SUCCESS(Status) && Writer->RestartQueued)
    {
        Status = NtfsLogRange(Writer, Writer->RestartOffset, Writer->Codec.PageSize,
                              Writer->Restart, LogIoDirect);
        if (NT_SUCCESS(Status))
            Status = Writer->Flush(Writer->Vcb, 0, MAXULONG);
        if (NT_SUCCESS(Status))
            Writer->RestartQueued = FALSE;
    }
    return Status;
}

static NTSTATUS
NtfsLogForce(PNTFS_LOG_WRITER Writer)
{
    NTSTATUS Status;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Writer->ForceResource, TRUE);
    Status = NtfsLogForceLocked(Writer);
    ExReleaseResourceLite(&Writer->ForceResource);
    KeLeaveCriticalRegion();
    return Status;
}

/* The next restart page, into the cache now and onto the queue, written
 * after the batches queued before it.  The cached write is made without
 * ForceResource: a paging write of the volume stream, holding the stream,
 * forces the log. */
static NTSTATUS
NtfsLogQueueRestart(PNTFS_LOG_WRITER Writer, BOOLEAN Clean)
{
    ULONG Offset;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource));
    Status = NtfsLfsBuildNextRestart(&Writer->Codec, Clean, Writer->RestartBuild, &Offset);
    if (NT_SUCCESS(Status))
        Status = NtfsLogRange(Writer, Offset, Writer->Codec.PageSize, Writer->RestartBuild, LogIoCached);
    if (!NT_SUCCESS(Status))
        return Status;
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Writer->ForceResource, TRUE);
    /* One restart page is queued at a time, behind every batch before it. */
    Status = NtfsLogForceLocked(Writer);
    if (NT_SUCCESS(Status))
    {
        RtlCopyMemory(Writer->Restart, Writer->RestartBuild, Writer->Codec.PageSize);
        Writer->RestartOffset = Offset;
        Writer->RestartQueued = TRUE;
        Writer->RestartDirty = !Clean;
    }
    ExReleaseResourceLite(&Writer->ForceResource);
    KeLeaveCriticalRegion();
    return Status;
}

/* The pages of the last batch: into the cache, and onto the queue.  The
 * first record of a session marks the restart area dirty. */
static NTSTATUS
NtfsLogQueueBatch(PNTFS_LOG_WRITER Writer)
{
    PLFS_WRITE_CONTEXT Codec = &Writer->Codec;
    ULONG i, Offset;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource));
    for (i = 0; NT_SUCCESS(Status) && i < Codec->StagedPages; i++)
    {
        Offset = NtfsLfsStagedPageOffset(Codec, i);
        Status = NtfsLogRange(Writer, Offset, Codec->PageSize, Codec->Stage + i * Codec->PageSize,
                              LogIoCached);
    }
    if (!NT_SUCCESS(Status))
        return Status;
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Writer->ForceResource, TRUE);
    if (Writer->QueueCount + Codec->StagedPages > NTFS_LOG_QUEUE_PAGES)
        Status = NtfsLogForceLocked(Writer);
    for (i = 0; NT_SUCCESS(Status) && i < Codec->StagedPages; i++)
    {
        Writer->QueueOffset[Writer->QueueCount] = NtfsLfsStagedPageOffset(Codec, i);
        RtlCopyMemory(Writer->Queue + Writer->QueueCount * Codec->PageSize,
                      Codec->Stage + i * Codec->PageSize, Codec->PageSize);
        Writer->QueueCount++;
    }
    ExReleaseResourceLite(&Writer->ForceResource);
    KeLeaveCriticalRegion();
    if (NT_SUCCESS(Status) && !Writer->RestartDirty)
        Status = NtfsLogQueueRestart(Writer, FALSE);
    return Status;
}

/* The oldest logged record whose file record write has not been issued yet,
 * or zero when there is none.  The caller holds PendingLock. */
static ULONGLONG
NtfsLogOldestPending(PNTFS_LOG_WRITER Writer)
{
    PLIST_ENTRY Entry;
    ULONGLONG Oldest = 0;

    for (Entry = Writer->PendingList.Flink; Entry != &Writer->PendingList; Entry = Entry->Flink)
    {
        PNTFS_LOG_PENDING Pending = CONTAINING_RECORD(Entry, NTFS_LOG_PENDING, Link);
        if (Oldest == 0 || Pending->Lsn < Oldest)
            Oldest = Pending->Lsn;
    }
    return Oldest;
}

/* Checkpoint: flushing the volume puts every issued metadata write on disk,
 * so only records still pending are needed afterwards, and the Noop record
 * of an unlogged write still in progress.  The restart area says so before
 * any of the space is reused. */
static NTSTATUS
NtfsLogCheckpointLocked(PNTFS_LOG_WRITER Writer)
{
    PLIST_ENTRY Entry;
    ULONGLONG Oldest;
    NTSTATUS Status;
    KIRQL OldIrql;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource) && Writer->InIo);
    /* The log first, then everything it describes. */
    Status = NtfsLogForce(Writer);
    if (NT_SUCCESS(Status))
        Status = Writer->Flush(Writer->Vcb, 0, 0);
    if (!NT_SUCCESS(Status))
        return Status;
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    Oldest = NtfsLogOldestPending(Writer);
    for (Entry = Writer->Requests.Flink; Entry != &Writer->Requests; Entry = Entry->Flink)
    {
        PNTFS_LOG_REQUEST Request = CONTAINING_RECORD(Entry, NTFS_LOG_REQUEST, Link);
        if (Request->Session == Writer->Session && !Request->Forced && Request->FirstLsn != 0 &&
            (Oldest == 0 || Request->FirstLsn < Oldest))
            Oldest = Request->FirstLsn;
    }
    if (Writer->UnloggedWrites != 0)
    {
        ASSERT(Writer->MarkerLsn != 0);
        if (Oldest == 0 || Writer->MarkerLsn < Oldest)
            Oldest = Writer->MarkerLsn;
    }
    else
    {
        Writer->MarkerLsn = 0;
    }
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    if (Oldest == Writer->Codec.OldestLsn || !Writer->RestartDirty)
    {
        Writer->Codec.OldestLsn = Oldest;
        return STATUS_SUCCESS;
    }
    Writer->Codec.OldestLsn = Oldest;
    /* On disk before any of the space is reused. */
    Status = NtfsLogQueueRestart(Writer, FALSE);
    if (NT_SUCCESS(Status))
        Status = NtfsLogForce(Writer);
    return Status;
}

/* Commits every open transaction now: the log is full of what they keep.
 * Their records from here on commit one by one. */
static NTSTATUS
NtfsLogForceCommit(PNTFS_LOG_WRITER Writer)
{
    LFS_EMIT_PARAMS Params[32];
    ULONGLONG Lsns[32];
    PNTFS_LOG_REQUEST Forced[32];
    PLIST_ENTRY Entry;
    ULONG Count = 0, i;
    NTSTATUS Status;
    KIRQL OldIrql;

    RtlZeroMemory(Params, sizeof(Params));
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    for (Entry = Writer->Requests.Flink; Entry != &Writer->Requests && Count < RTL_NUMBER_OF(Params);
         Entry = Entry->Flink)
    {
        PNTFS_LOG_REQUEST Request = CONTAINING_RECORD(Entry, NTFS_LOG_REQUEST, Link);
        if (Request->Session != Writer->Session || Request->Forced || Request->TransactionId == 0)
            continue;
        Params[Count].RedoOperation = NTFS_LFS_OP_FORGET_TRANSACTION;
        Params[Count].UndoOperation = NTFS_LFS_OP_NOOP;
        Params[Count].TransactionId = Request->TransactionId;
        Forced[Count++] = Request;
    }
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    if (Count == 0)
        return STATUS_LOG_FILE_FULL;
    DPRINT1("$LogFile full: committing %lu transactions early\n", Count);
    Writer->Codec.ReservePages = 0;
    Status = NtfsLfsEmitBatch(&Writer->Codec, Params, Count, Lsns);
    if (NT_SUCCESS(Status))
        Status = NtfsLogQueueBatch(Writer);
    if (NT_SUCCESS(Status))
        Status = NtfsLogForce(Writer);
    if (NT_SUCCESS(Status))
    {
        KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
        for (i = 0; i < Count; i++)
            Forced[i]->Forced = TRUE;
        KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    }
    return Status;
}

/* Emits a batch, checkpointing if the log is full, and queues it.  Params
 * has room for one more
 * record: the commit of OwnTransaction, should it be committed early on
 * the way.  A batch that only commits may use the reserved page. */
static NTSTATUS
NtfsLogEmit(PNTFS_LOG_WRITER Writer, PLFS_EMIT_PARAMS Params, ULONG Count, PULONGLONG Lsns,
            ULONG OwnTransaction, BOOLEAN Commit)
{
    PNTFS_LOG_REQUEST Request;
    NTSTATUS Status;
    KIRQL OldIrql;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource) && Writer->InIo);
    Writer->Codec.ReservePages = Commit ? 0 : 1;
    Status = NtfsLfsEmitBatch(&Writer->Codec, Params, Count, Lsns);
    if (Status == STATUS_LOG_FILE_FULL)
    {
        Status = NtfsLogCheckpointLocked(Writer);
        Writer->Codec.ReservePages = Commit ? 0 : 1;
        if (NT_SUCCESS(Status))
            Status = NtfsLfsEmitBatch(&Writer->Codec, Params, Count, Lsns);
    }
    if (Status == STATUS_LOG_FILE_FULL && !Commit)
    {
        Status = NtfsLogForceCommit(Writer);
        if (NT_SUCCESS(Status))
            Status = NtfsLogCheckpointLocked(Writer);
        if (NT_SUCCESS(Status) && OwnTransaction != 0)
        {
            /* This batch's records belong to a transaction committed early:
             * they commit with it. */
            KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
            Request = NULL;
            {
                PLIST_ENTRY Entry;
                for (Entry = Writer->Requests.Flink; Entry != &Writer->Requests; Entry = Entry->Flink)
                {
                    PNTFS_LOG_REQUEST R = CONTAINING_RECORD(Entry, NTFS_LOG_REQUEST, Link);
                    if (R->TransactionId == OwnTransaction && R->Session == Writer->Session)
                        Request = R;
                }
            }
            KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
            if (Request != NULL && Request->Forced)
            {
                RtlZeroMemory(&Params[Count], sizeof(Params[Count]));
                Params[Count].RedoOperation = NTFS_LFS_OP_FORGET_TRANSACTION;
                Params[Count].UndoOperation = NTFS_LFS_OP_NOOP;
                Params[Count].TransactionId = OwnTransaction;
                Count++;
            }
        }
        Writer->Codec.ReservePages = 1;
        if (NT_SUCCESS(Status))
            Status = NtfsLfsEmitBatch(&Writer->Codec, Params, Count, Lsns);
    }
    if (NT_SUCCESS(Status))
        Status = NtfsLogQueueBatch(Writer);
    return Status;
}

/* The request in progress on this thread, or NULL. */
static PNTFS_LOG_REQUEST
NtfsLogCurrentRequest(PNTFS_LOG_WRITER Writer)
{
    PNTFS_LOG_REQUEST Found = NULL;
    PLIST_ENTRY Entry;
    PKTHREAD Thread = KeGetCurrentThread();
    KIRQL OldIrql;

    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    for (Entry = Writer->Requests.Flink; Entry != &Writer->Requests; Entry = Entry->Flink)
    {
        PNTFS_LOG_REQUEST Request = CONTAINING_RECORD(Entry, NTFS_LOG_REQUEST, Link);
        if (Request->Thread == Thread)
        {
            Found = Request;
            break;
        }
    }
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    return Found;
}

/* Logs records of the current request's transaction, or, outside one,
 * records that commit at once.  Params has room for one more record. */
static NTSTATUS
NtfsLogEmitOwn(PNTFS_LOG_WRITER Writer, PLFS_EMIT_PARAMS Params, ULONG Count, PULONGLONG Lsns)
{
    PNTFS_LOG_REQUEST Request = NtfsLogCurrentRequest(Writer);
    ULONG Transaction, i;
    BOOLEAN Own;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource) && Writer->InIo);
    Own = Request != NULL && Request->Session == Writer->Session && !Request->Forced;
    if (Own && Request->TransactionId != 0)
    {
        Transaction = Request->TransactionId;
    }
    else
    {
        if (++Writer->NextTransaction == 0)
            Writer->NextTransaction = 1;
        Transaction = Writer->NextTransaction;
    }
    for (i = 0; i < Count; i++)
        Params[i].TransactionId = Transaction;
    if (!Own)
    {
        RtlZeroMemory(&Params[Count], sizeof(Params[Count]));
        Params[Count].RedoOperation = NTFS_LFS_OP_FORGET_TRANSACTION;
        Params[Count].UndoOperation = NTFS_LFS_OP_NOOP;
        Params[Count].TransactionId = Transaction;
        Count++;
    }
    else
    {
        /* Checkpoints keep the transaction's records from its first one. */
        Request->TransactionId = Transaction;
    }
    Status = NtfsLogEmit(Writer, Params, Count, Lsns, Own ? Transaction : 0, FALSE);
    if (NT_SUCCESS(Status) && Own && Request->FirstLsn == 0)
        Request->FirstLsn = Lsns[0];
    return Status;
}

VOID
NtfsLfsBeginRequest(PVOID Context, PNTFS_LOG_REQUEST Request)
{
    PNTFS_LOG_WRITER Writer = Context;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    RtlZeroMemory(Request, sizeof(*Request));
    if (Writer == NULL)
    {
        Request->Joined = TRUE;
        return;
    }
    Request->Thread = KeGetCurrentThread();
    Request->Session = Writer->Session;
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    for (Entry = Writer->Requests.Flink; Entry != &Writer->Requests; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, NTFS_LOG_REQUEST, Link)->Thread == Request->Thread)
            Request->Joined = TRUE;
    }
    if (!Request->Joined)
        InsertTailList(&Writer->Requests, &Request->Link);
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
}

NTSTATUS
NtfsLfsEndRequest(PVOID Context, PNTFS_LOG_REQUEST Request, BOOLEAN ForceLog)
{
    PNTFS_LOG_WRITER Writer = Context;
    LFS_EMIT_PARAMS Params;
    ULONGLONG Lsn;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (Request->Joined)
        return STATUS_SUCCESS;
    /* A request that logged nothing has nothing to commit, and must not
     * wait for the writer: the lazy writer's paging writes end here while
     * it holds the volume stream a committing request may be flushing.
     * Only this thread sets TransactionId. */
    if (Request->TransactionId == 0 || Request->Forced)
    {
        KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
        RemoveEntryList(&Request->Link);
        KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
        return ForceLog ? NtfsLfsForce(Writer) : STATUS_SUCCESS;
    }
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    if (Writer->Enabled && Request->Session == Writer->Session &&
        Request->TransactionId != 0 && !Request->Forced)
    {
        /* The commit: until it is on disk, recovery undoes the request. */
        RtlZeroMemory(&Params, sizeof(Params));
        Params.RedoOperation = NTFS_LFS_OP_FORGET_TRANSACTION;
        Params.UndoOperation = NTFS_LFS_OP_NOOP;
        Params.TransactionId = Request->TransactionId;
        Writer->InIo = TRUE;
        Status = NtfsLogEmit(Writer, &Params, 1, &Lsn, 0, TRUE);
        Writer->InIo = FALSE;
        if (!NT_SUCCESS(Status))
            DPRINT1("Couldn't log the commit of transaction %lu (%lx)\n", Request->TransactionId, Status);
    }
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    RemoveEntryList(&Request->Link);
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    ExReleaseResourceLite(&Writer->Resource);
    /* A flush makes the request durable: its commit with it. */
    if (NT_SUCCESS(Status) && ForceLog)
        Status = NtfsLfsForce(Writer);
    return Status;
}

NTSTATUS
NtfsLfsForce(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;

    if (Writer == NULL || !Writer->Enabled)
        return STATUS_SUCCESS;
    return NtfsLogForce(Writer);
}

NTSTATUS
NtfsLfsEnableWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    PDEVICE_EXTENSION Vcb = Writer->Vcb;
    PFILE_RECORD_HEADER Record;
    ULONGLONG Size;
    ULONG Batch, Pages, Scratch;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->IoResource));
    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->DirResource));
    if (Writer->Enabled)
        return STATUS_SUCCESS;
    if (Vcb->Flags & (VCB_DEVICE_REMOVED | VCB_VOLUME_READ_ONLY))
        return STATUS_MEDIA_WRITE_PROTECTED;
    /* A dirty log is recovered at mount; a writer never starts over one. */
    ASSERT(!Vcb->LogFileDirty);
    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    NtfsLogFreeSession(Writer);
    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"", 0, &Writer->Data, NULL);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Size = AttributeDataLength(Writer->Data->pRecord);
    if (!Writer->Data->pRecord->IsNonResident ||
        (Writer->Data->pRecord->Flags & (NTFS_ATTR_SPARSE | NTFS_ATTR_COMPRESSION_MASK)) ||
        Size > MAXULONG || Size < (NTFS_LFS_FIRST_RECORD_PAGE + 2) * PAGE_SIZE ||
        (Size & (PAGE_SIZE - 1)) || Vcb->NtfsInfo.BytesPerFileRecord > MAXUSHORT)
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }
    /* The largest batches: a file record's redo and undo images with the
     * clusters it spans and the record that commits it, or a metadata write
     * in its records. */
    Scratch = max(NtfsLfsRecordLength(Vcb->NtfsInfo.BytesPerFileRecord, Vcb->NtfsInfo.BytesPerFileRecord,
                                      Vcb->NtfsInfo.BytesPerFileRecord / Vcb->NtfsInfo.BytesPerCluster + 2),
                  NtfsLfsRecordLength(NTFS_LOG_METADATA_CHUNK, NTFS_LOG_METADATA_CHUNK,
                                      NTFS_LOG_METADATA_LCNS));
    Batch = max(Scratch, 2 * NTFS_LOG_MAX_METADATA_WRITE +
                         NTFS_LOG_METADATA_RECORDS * NtfsLfsRecordLength(0, 0, NTFS_LOG_METADATA_LCNS)) +
            NtfsLfsRecordLength(0, 0, 0);
    Pages = NtfsLfsStagePages(PAGE_SIZE, Batch);
    Writer->Codec.Stage = ExAllocatePoolWithTag(PagedPool, Pages * PAGE_SIZE, TAG_NTFS);
    Writer->Codec.Scratch = ExAllocatePoolWithTag(PagedPool, Scratch, TAG_NTFS);
    Writer->Restart = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_NTFS);
    /* A batch always fits the queue after a force. */
    ASSERT(Pages <= NTFS_LOG_QUEUE_PAGES);
    Writer->RestartBuild = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_NTFS);
    Writer->Queue = ExAllocatePoolWithTag(PagedPool, NTFS_LOG_QUEUE_PAGES * PAGE_SIZE, TAG_NTFS);
    if (Writer->Codec.Stage == NULL || Writer->Codec.Scratch == NULL || Writer->Restart == NULL ||
        Writer->RestartBuild == NULL || Writer->Queue == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    Status = NtfsLfsInitWriteContext(&Writer->Codec, Vcb->NtfsInfo.BytesPerSector, (ULONG)Size,
                                     PAGE_SIZE, Vcb->LogFileLsn, Writer->Codec.Stage, Pages,
                                     Writer->Codec.Scratch, Scratch);
    if (NT_SUCCESS(Status))
    {
        /* The restart area stays clean until the first record.  Requests
         * of an earlier session hold no transaction of this one. */
        Writer->Session++;
        Writer->RestartDirty = FALSE;
        Writer->QueueCount = 0;
        Writer->RestartQueued = FALSE;
        Writer->MarkerLsn = 0;
        Writer->UnloggedWrites = 0;
        Writer->Enabled = TRUE;
    }
Cleanup:
    if (!NT_SUCCESS(Status))
        NtfsLogFreeSession(Writer);
    ExReleaseResourceLite(&Writer->Resource);
    return Status;
}

NTSTATUS
NtfsLfsLogFileRecord(PVOID Context, ULONGLONG MftIndex, PFILE_RECORD_HEADER Record,
                     PNTFS_LOG_PENDING Pending)
{
    PNTFS_LOG_WRITER Writer = Context;
    PDEVICE_EXTENSION Vcb;
    PFILE_RECORD_HEADER Before;
    LFS_EMIT_PARAMS Params[2];
    LONGLONG Lcn, RunLength, LastVcn;
    LONGLONG Lcns[16];
    ULONGLONG ByteOffset, Lsns[2];
    ULONG Count, i;
    NTSTATUS Status;
    KIRQL OldIrql;

    Pending->Lsn = 0;
    if (Writer == NULL || !Writer->Enabled || (Writer->InIo && ExIsResourceAcquiredExclusiveLite(&Writer->Resource)))
        return STATUS_SUCCESS;
    Vcb = Writer->Vcb;
    RtlZeroMemory(Params, sizeof(Params));
    ByteOffset = MftIndex * Vcb->NtfsInfo.BytesPerFileRecord;
    Params[0].TargetVcn = ByteOffset / Vcb->NtfsInfo.BytesPerCluster;
    LastVcn = (ByteOffset + Vcb->NtfsInfo.BytesPerFileRecord - 1) / Vcb->NtfsInfo.BytesPerCluster;
    Count = (ULONG)(LastVcn - Params[0].TargetVcn + 1);
    if (Count > RTL_NUMBER_OF(Lcns))
        return STATUS_NOT_SUPPORTED;
    Before = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Before == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    /* Capture before taking the writer resource: other metadata writers can
     * own MftContextResource while waiting for this writer. A slot the MFT
     * bitmap just handed out may never have been initialized, and a new
     * extent of the MFT holds whatever its clusters held: no record was
     * there, and the undo image is zeroes, a slot never used. */
    Status = NtfsReadFileRecordEx(Vcb, MftIndex, Before, TRUE);
    if (Status == STATUS_FILE_CORRUPT_ERROR)
    {
        RtlZeroMemory(Before, Vcb->NtfsInfo.BytesPerFileRecord);
        Status = STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(Status)) goto FreeBefore;
    ExAcquireResourceSharedLite(&Vcb->MftContextResource, TRUE);
    for (i = 0; i < Count; i++)
    {
        if (!FsRtlLookupLargeMcbEntry(&Vcb->MFTContext->DataRunsMCB, Params[0].TargetVcn + i,
                                      &Lcn, &RunLength, NULL, NULL, NULL) || Lcn < 0)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        Lcns[i] = Lcn;
    }
    ExReleaseResourceLite(&Vcb->MftContextResource);
    if (!NT_SUCCESS(Status)) goto FreeBefore;
    Params[0].RedoOperation = NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT;
    Params[0].UndoOperation = NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT;
    Params[0].RedoData = Record;
    Params[0].UndoData = Before;
    Params[0].RedoLength = Params[0].UndoLength = (USHORT)Vcb->NtfsInfo.BytesPerFileRecord;
    Params[0].LcnsToFollow = (USHORT)Count;
    Params[0].Lcns = Lcns;
    Params[0].ClusterBlockOffset = (USHORT)((ByteOffset % Vcb->NtfsInfo.BytesPerCluster) / 512);
    Params[0].AttributeFlags = NTFS_LFS_ACTS_ON_MFT;
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    Writer->InIo = TRUE;
    Status = NtfsLogEmitOwn(Writer, Params, 1, Lsns);
    if (NT_SUCCESS(Status))
    {
        Record->Ntfs.Lsn = Lsns[0];
        Pending->Lsn = Lsns[0];
        KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
        InsertTailList(&Writer->PendingList, &Pending->Link);
        KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    }
    Writer->InIo = FALSE;
    ExReleaseResourceLite(&Writer->Resource);
FreeBefore:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Before);
    return Status;
}

NTSTATUS
NtfsLfsBeginMetadataWrite(PVOID Context, const NTFS_LOG_EXTENT *Extents, ULONG Count,
                          const UCHAR *Buffer, ULONGLONG Offset, PNTFS_LOG_METADATA_WRITE Write)
{
    struct
    {
        LFS_EMIT_PARAMS Params[NTFS_LOG_METADATA_RECORDS + 1];
        LONGLONG Lcns[NTFS_LOG_METADATA_RECORDS][NTFS_LOG_METADATA_LCNS];
        ULONGLONG Lsns[NTFS_LOG_METADATA_RECORDS + 1];
        UCHAR Undo[NTFS_LOG_MAX_METADATA_WRITE];
    } *Batch;
    PNTFS_LOG_WRITER Writer = Context;
    PDEVICE_EXTENSION Vcb;
    ULONG Total = 0, Records = 0, i, k, Done, Part, Within, Bpc;
    NTSTATUS Status;
    KIRQL OldIrql;

    Write->Pending.Lsn = 0;
    Write->Unlogged = FALSE;
    if (Writer == NULL || !Writer->Enabled || (Writer->InIo && ExIsResourceAcquiredExclusiveLite(&Writer->Resource)))
        return STATUS_SUCCESS;
    Vcb = Writer->Vcb;
    Bpc = Vcb->NtfsInfo.BytesPerCluster;
    for (i = 0; i < Count; i++)
    {
        Total += Extents[i].Length;
        Records += (Extents[i].Length + NTFS_LOG_METADATA_CHUNK - 1) / NTFS_LOG_METADATA_CHUNK;
    }
    if (Total > NTFS_LOG_MAX_METADATA_WRITE || Records > NTFS_LOG_METADATA_RECORDS)
    {
        Status = NtfsLfsBeginUnloggedWrite(Writer);
        Write->Unlogged = NT_SUCCESS(Status);
        return Status;
    }
    if (Total == 0)
        return STATUS_SUCCESS;

    /* The undo images: what the bytes hold now. */
    Batch = ExAllocatePoolWithTag(PagedPool, sizeof(*Batch), TAG_NTFS);
    if (Batch == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Batch->Params, sizeof(Batch->Params));
    Records = 0;
    Total = 0;
    for (i = 0; i < Count; i++)
    {
        Status = NtfsReadDiskCached(Vcb, Extents[i].DiskOffset, Extents[i].Length, Batch->Undo + Total);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
        for (Done = 0; Done < Extents[i].Length; Done += Part, Records++)
        {
            PLFS_EMIT_PARAMS P = &Batch->Params[Records];
            LONGLONG Disk = Extents[i].DiskOffset + Done;

            Part = min(Extents[i].Length - Done, NTFS_LOG_METADATA_CHUNK);
            Within = (ULONG)(Disk % Bpc);
            P->RedoOperation = P->UndoOperation = NTFS_LFS_OP_UPDATE_NONRESIDENT_VALUE;
            P->RedoData = Buffer + Extents[i].BufferOffset + Done;
            P->UndoData = Batch->Undo + Total + Done;
            P->RedoLength = P->UndoLength = (USHORT)Part;
            P->TargetVcn = (Offset + Extents[i].BufferOffset + Done) / Bpc;
            P->AttributeOffset = (USHORT)Within;
            P->LcnsToFollow = (USHORT)((Within + Part + Bpc - 1) / Bpc);
            ASSERT(P->LcnsToFollow <= NTFS_LOG_METADATA_LCNS);
            for (k = 0; k < P->LcnsToFollow; k++)
                Batch->Lcns[Records][k] = Disk / Bpc + k;
            P->Lcns = Batch->Lcns[Records];
        }
        Total += Extents[i].Length;
    }

    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    Writer->InIo = TRUE;
    Status = NtfsLogEmitOwn(Writer, Batch->Params, Records, Batch->Lsns);
    if (NT_SUCCESS(Status))
    {
        Write->Pending.Lsn = Batch->Lsns[0];
        KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
        InsertTailList(&Writer->PendingList, &Write->Pending.Link);
        KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    }
    Writer->InIo = FALSE;
    ExReleaseResourceLite(&Writer->Resource);
Cleanup:
    ExFreePoolWithTag(Batch, TAG_NTFS);
    return Status;
}

VOID
NtfsLfsEndMetadataWrite(PVOID Context, PNTFS_LOG_METADATA_WRITE Write)
{
    if (Write->Unlogged)
        NtfsLfsEndUnloggedWrite(Context);
    else
        NtfsLfsFileRecordWritten(Context, &Write->Pending);
    Write->Unlogged = FALSE;
}

VOID
NtfsLfsFileRecordWritten(PVOID Context, PNTFS_LOG_PENDING Pending)
{
    PNTFS_LOG_WRITER Writer = Context;
    KIRQL OldIrql;

    if (Pending->Lsn == 0)
        return;
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    RemoveEntryList(&Pending->Link);
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    Pending->Lsn = 0;
}

NTSTATUS
NtfsLfsBeginUnloggedWrite(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    LFS_EMIT_PARAMS Params;
    ULONGLONG Lsn;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN Covered;
    KIRQL OldIrql;

    if (Writer == NULL || !Writer->Enabled)
        return STATUS_SUCCESS;
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    Writer->UnloggedWrites++;
    Covered = Writer->MarkerLsn != 0;
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    if (Covered)
        return STATUS_SUCCESS;

    /* The writer's own flushes never write metadata through here. */
    ASSERT(!(Writer->InIo && ExIsResourceAcquiredExclusiveLite(&Writer->Resource)));
    RtlZeroMemory(&Params, sizeof(Params));
    Params.RedoOperation = NTFS_LFS_OP_NOOP;
    Params.UndoOperation = NTFS_LFS_OP_NOOP;
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    if (Writer->MarkerLsn == 0)
    {
        Writer->InIo = TRUE;
        Status = NtfsLogEmit(Writer, &Params, 1, &Lsn, 0, FALSE);
        Writer->InIo = FALSE;
        if (NT_SUCCESS(Status))
        {
            KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
            Writer->MarkerLsn = Lsn;
            KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
        }
    }
    ExReleaseResourceLite(&Writer->Resource);
    if (!NT_SUCCESS(Status))
        NtfsLfsEndUnloggedWrite(Writer);
    return Status;
}

VOID
NtfsLfsEndUnloggedWrite(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    KIRQL OldIrql;

    if (Writer == NULL || !Writer->Enabled)
        return;
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    ASSERT(Writer->UnloggedWrites != 0);
    Writer->UnloggedWrites--;
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
}

NTSTATUS
NtfsLfsCheckpoint(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    NTSTATUS Status;

    if (Writer == NULL || !Writer->Enabled)
        return STATUS_SUCCESS;
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    Writer->InIo = TRUE;
    Status = NtfsLogCheckpointLocked(Writer);
    Writer->InIo = FALSE;
    ExReleaseResourceLite(&Writer->Resource);
    return Status;
}

NTSTATUS
NtfsLfsFinishWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    NTSTATUS Status;

    if (Writer == NULL || !Writer->Enabled) return STATUS_SUCCESS;
    /* Namespace changes are quiesced; a forced dismount does not wait for
     * other I/O, whose later file record writes are then not logged. */
    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Vcb->DirResource));
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    Writer->InIo = TRUE;
    /* The log, then the metadata it describes, reach storage before either
     * restart copy is marked clean; both copies are, one after the other. */
    Status = NtfsLogForce(Writer);
    if (NT_SUCCESS(Status)) Status = Writer->Flush(Writer->Vcb, 0, 0);
    if (NT_SUCCESS(Status)) Status = NtfsLogQueueRestart(Writer, TRUE);
    if (NT_SUCCESS(Status)) Status = NtfsLogQueueRestart(Writer, TRUE);
    if (NT_SUCCESS(Status)) Status = NtfsLogForce(Writer);
    if (NT_SUCCESS(Status))
    {
        Writer->Enabled = FALSE;
        Writer->Vcb->LogFileLsn = Writer->Codec.LastLsn;
        Writer->MarkerLsn = 0;
    }
    Writer->InIo = FALSE;
    ExReleaseResourceLite(&Writer->Resource);
    return Status;
}

VOID
NtfsLfsDestroyWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;

    if (Writer == NULL) return;
    ASSERT(IsListEmpty(&Writer->PendingList));
    ASSERT(IsListEmpty(&Writer->Requests));
    NtfsLogFreeSession(Writer);
    ExDeleteResourceLite(&Writer->ForceResource);
    ExDeleteResourceLite(&Writer->Resource);
    ExFreePoolWithTag(Writer, TAG_NTFS);
}

BOOLEAN
NtfsLfsWriterEnabled(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    return Writer != NULL && Writer->Enabled;
}

NTSTATUS
NtfsLfsSuspendWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    NTSTATUS Status;

    if (Writer == NULL || !Writer->Enabled) return STATUS_SUCCESS;
    Status = NtfsLfsFinishWriter(Writer);
    if (NT_SUCCESS(Status)) Writer->Paused = TRUE;
    return Status;
}

NTSTATUS
NtfsLfsResumeWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    NTSTATUS Status;

    if (Writer == NULL || !Writer->Paused) return STATUS_SUCCESS;
    Status = NtfsLfsEnableWriter(Writer);
    if (NT_SUCCESS(Status)) Writer->Paused = FALSE;
    return Status;
}
