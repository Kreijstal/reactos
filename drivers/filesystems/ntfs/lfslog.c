/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     $LogFile write-ahead logging of file records
 *
 * Metadata is logged before it is written: a file record update, or any
 * other non-resident metadata write (index buffers, bitmaps, $Secure,
 * attribute lists, $MFTMirr), as UpdateNonResidentValue records, each
 * carrying a range of the bytes the write changes (redo) and the bytes they
 * replace (undo), at the clusters they go to.  The bytes are the ones the
 * disk holds, update sequence arrays applied: a range's redo is right over
 * any version of its bytes, even a torn write, so recovery replays them
 * without reading what is there.
 *
 * As LFS does, records are buffered and packed: they fill the tail page
 * (lfs.c), the pages a batch fills go onto a queue, and the log is forced
 * to disk (the queue written, the tail page written to the next of the two
 * tail-copy pages, then a storage barrier) before a page of the volume
 * stream that a record not on disk yet changed is written (NtfsLfsForceRange,
 * from the paging write) and by an explicit flush.  A full queue is written without a barrier.  Log pages go
 * to the device only, each full page once: the volume stream never caches
 * them, so the lazy writer never writes them again.  The restart area is written when a session logs
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
 * The log is reused as a circular buffer.  A logged write, once issued,
 * leaves its pages of the volume stream in the dirty page table with the
 * oldest record of a change to them that may not be on disk yet (the
 * page's RecLsn, as NTFS keeps it).  When the log is full, a checkpoint
 * writes the pages in the table, and those only, not the rest of the
 * volume stream (file data): the records still needed are then the ones
 * of the pages changed again meanwhile, of writes not issued yet (the
 * pending list), of transactions still open, and the Noop record of an
 * unlogged write, until a flush of the whole volume stream has put it on
 * disk.  A page is kept free behind
 * every batch for the records that commit; a transaction that alone keeps
 * the log full is committed early.  Storage barriers are supplied by the
 * mount layer; the codec has no dependency on cache-manager or
 * storage-device implementation details.
 */
#include "lfswrite.h"
#define NDEBUG
#include <debug.h>

#define NTFS_LOG_DIRTY_BUCKETS 512

/* A page of the volume stream a logged write changed. */
typedef struct _NTFS_LOG_DIRTY_PAGE
{
    LIST_ENTRY Link;
    ULONGLONG Page;             /* its offset in the volume, in pages */
    ULONGLONG RecLsn;           /* the oldest change that may not be on disk */
    ULONGLONG LastLsn;          /* the newest change */
    ULONGLONG NewLsn;           /* while a checkpoint writes it: the oldest
                                   change since it began, 0 none */
    BOOLEAN Flushing;
} NTFS_LOG_DIRTY_PAGE, *PNTFS_LOG_DIRTY_PAGE;

typedef struct _NTFS_LOG_WRITER
{
    PDEVICE_EXTENSION Vcb;
    ERESOURCE Resource;
    BOOLEAN InIo;
    PNTFS_LOG_WRITE_CALLBACK WriteDirect;   /* to the device */
    PNTFS_LOG_FLUSH_CALLBACK Flush;
    /* Batches not forced yet: page images and their log offsets, and a
     * restart page to write after them.  ForceResource guards them; it is
     * never held while waiting for the cache manager. */
    ERESOURCE ForceResource;
    PUCHAR Queue;
    ULONG QueueOffset[NTFS_LOG_QUEUE_PAGES];
    ULONG QueueCount;
    BOOLEAN Unflushed;          /* log writes not behind a barrier yet */
    ULONGLONG QueuedLsn;        /* the last record queued */
    ULONGLONG DurableLsn;       /* the last record a force put on disk */
    /* The tail page as the last batch left it, for the next tail copy. */
    PUCHAR TailImage;
    ULONG TailOffset;
    BOOLEAN TailQueued;
    PUCHAR TailBuild;           /* one page: the tail copy being written */
    USHORT TailUsn[2];          /* of the copy each tail-copy page holds */
    ULONG TailSlot;             /* the tail-copy page written next (0, 1) */
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
    /* The dirty page table, guarded by PendingLock, and the oldest change
     * it does not hold (0: none): its pages are known only to a flush of
     * the whole volume stream. */
    LIST_ENTRY Dirty[NTFS_LOG_DIRTY_BUCKETS];
    ULONG DirtyCount;
    ULONGLONG FullFlushLsn;
    NPAGED_LOOKASIDE_LIST DirtyLookaside;
} NTFS_LOG_WRITER, *PNTFS_LOG_WRITER;

NTSTATUS
NtfsLfsCreateWriter(PDEVICE_EXTENSION Vcb,
                    PNTFS_LOG_WRITE_CALLBACK WriteDirect,
                    PNTFS_LOG_FLUSH_CALLBACK Flush,
                    PVOID *WriterOut)
{
    PNTFS_LOG_WRITER Writer;
    NTSTATUS Status;
    ULONG i;

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
    Writer->WriteDirect = WriteDirect;
    Writer->Flush = Flush;
    KeInitializeSpinLock(&Writer->PendingLock);
    InitializeListHead(&Writer->PendingList);
    InitializeListHead(&Writer->Requests);
    for (i = 0; i < NTFS_LOG_DIRTY_BUCKETS; i++)
        InitializeListHead(&Writer->Dirty[i]);
    ExInitializeNPagedLookasideList(&Writer->DirtyLookaside, NULL, NULL, 0, sizeof(NTFS_LOG_DIRTY_PAGE),
                                    TAG_NTFS, 0);
    *WriterOut = Writer;
    return STATUS_SUCCESS;
}

/* Empties the dirty page table.  The caller holds PendingLock, or the
 * writer is not in use. */
static VOID
NtfsLogFreeDirty(PNTFS_LOG_WRITER Writer)
{
    ULONG i;

    for (i = 0; i < NTFS_LOG_DIRTY_BUCKETS; i++)
    {
        while (!IsListEmpty(&Writer->Dirty[i]))
            ExFreeToNPagedLookasideList(&Writer->DirtyLookaside,
                                        CONTAINING_RECORD(RemoveHeadList(&Writer->Dirty[i]),
                                                          NTFS_LOG_DIRTY_PAGE, Link));
    }
    Writer->DirtyCount = 0;
    Writer->FullFlushLsn = 0;
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
    if (Writer->TailImage != NULL) ExFreePoolWithTag(Writer->TailImage, TAG_NTFS);
    if (Writer->TailBuild != NULL) ExFreePoolWithTag(Writer->TailBuild, TAG_NTFS);
    Writer->Restart = NULL;
    Writer->RestartBuild = NULL;
    Writer->Queue = NULL;
    Writer->TailImage = NULL;
    Writer->TailBuild = NULL;
    Writer->QueueCount = 0;
    Writer->TailQueued = FALSE;
    Writer->RestartQueued = FALSE;
    NtfsLogFreeDirty(Writer);
    RtlZeroMemory(&Writer->Codec, sizeof(Writer->Codec));
}

/* $LogFile has a fixed, fully allocated mapping for this session. Writing
 * mapped runs cannot recurse through a read of its own MFT record. */
static NTSTATUS
NtfsLogRange(PNTFS_LOG_WRITER Writer, ULONG Offset, ULONG Length, PUCHAR Buffer)
{
    PDEVICE_EXTENSION Vcb = Writer->Vcb;
    ULONGLONG Position = Offset, Available, Disk;
    LONGLONG Lcn, RunLength;
    ULONG Within, Part;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->ForceResource));
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
        Status = Writer->WriteDirect(Vcb, Disk, Part, Buffer + (Position - Offset));
        if (!NT_SUCCESS(Status)) return Status;
        Position += Part;
        Length -= Part;
    }
    return STATUS_SUCCESS;
}

/* The volume stream is to hold no copy of a log page the writer wrote:
 * copies it cached before (the mount's reads of the restart area) are
 * dropped when a session starts, and pages read during one when it ends.
 * Where the cache manager cannot drop them, the restart pages are written
 * through it as well when the session ends (NtfsLogCacheRestart): they are
 * what a locked volume's checker reads. */
static VOID
NtfsLogPurgeCache(PNTFS_LOG_WRITER Writer)
{
    PDEVICE_EXTENSION Vcb = Writer->Vcb;
    ULONGLONG Vcn = 0, Clusters = Writer->Codec.LogSize / Vcb->NtfsInfo.BytesPerCluster;
    LONGLONG Lcn, RunLength;
    LARGE_INTEGER Offset;

    while (Vcn < Clusters &&
           FsRtlLookupLargeMcbEntry(&Writer->Data->DataRunsMCB, Vcn, &Lcn, &RunLength, NULL, NULL, NULL) &&
           Lcn >= 0 && RunLength > 0)
    {
        RunLength = min(RunLength, (LONGLONG)(Clusters - Vcn));
        Offset.QuadPart = Lcn * Vcb->NtfsInfo.BytesPerCluster;
        if (!CcPurgeCacheSection(Vcb->StreamFileObject->SectionObjectPointer, &Offset,
                                 (ULONG)(RunLength * Vcb->NtfsInfo.BytesPerCluster), FALSE))
            DPRINT("$LogFile pages stay cached at %I64x\n", Offset.QuadPart);
        Vcn += RunLength;
    }
}

/* The restart page just queued, through the volume stream as well, and
 * written at once: the session is ending, its records and their metadata
 * are on disk, and the page queued has the same bytes. */
static NTSTATUS
NtfsLogCacheRestart(PNTFS_LOG_WRITER Writer)
{
    PDEVICE_EXTENSION Vcb = Writer->Vcb;
    ULONGLONG Position = Writer->RestartOffset, Disk;
    ULONG Length = Writer->Codec.PageSize, Done = 0, Within, Part;
    LONGLONG Lcn, RunLength;
    NTSTATUS Status = STATUS_SUCCESS;

    while (NT_SUCCESS(Status) && Done < Length)
    {
        if (!FsRtlLookupLargeMcbEntry(&Writer->Data->DataRunsMCB, Position / Vcb->NtfsInfo.BytesPerCluster,
                                      &Lcn, &RunLength, NULL, NULL, NULL) || Lcn < 0 || RunLength <= 0)
            return STATUS_FILE_CORRUPT_ERROR;
        Within = (ULONG)(Position % Vcb->NtfsInfo.BytesPerCluster);
        Part = (ULONG)min((ULONGLONG)(Length - Done), (ULONGLONG)RunLength * Vcb->NtfsInfo.BytesPerCluster - Within);
        Disk = (ULONGLONG)Lcn * Vcb->NtfsInfo.BytesPerCluster + Within;
        Status = NtfsWriteDiskCached(Vcb, Disk, Part, Writer->Restart + Done);
        if (NT_SUCCESS(Status))
            Status = Writer->Flush(Vcb, Disk, Part);
        Position += Part;
        Done += Part;
    }
    return Status;
}

/* Writes the queued pages, consecutive ones in one transfer. */
static NTSTATUS
NtfsLogWriteQueue(PNTFS_LOG_WRITER Writer)
{
    ULONG PageSize = Writer->Codec.PageSize, i, Run;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->ForceResource));
    for (i = 0; NT_SUCCESS(Status) && i < Writer->QueueCount; i += Run)
    {
        for (Run = 1; i + Run < Writer->QueueCount &&
                      Writer->QueueOffset[i + Run] == Writer->QueueOffset[i] + Run * PageSize; Run++);
        Status = NtfsLogRange(Writer, Writer->QueueOffset[i], Run * PageSize, Writer->Queue + i * PageSize);
        Writer->Unflushed = TRUE;
    }
    if (NT_SUCCESS(Status))
        Writer->QueueCount = 0;
    return Status;
}

/* Writes the queued pages and the tail page's copy to the device, then the
 * queued restart page, each behind a storage barrier: a restart page never
 * names a record that is not on disk.  The tail copy goes to the copy page
 * the last one did not: that one keeps the tail's records should this
 * write tear, and also those of a page written full by this force. */
static NTSTATUS
NtfsLogForceLocked(PNTFS_LOG_WRITER Writer)
{
    ULONG Offset;
    USHORT Usn;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->ForceResource));
    Status = NtfsLogWriteQueue(Writer);
    if (NT_SUCCESS(Status) && Writer->TailQueued)
    {
        /* Not the number of the copy it overwrites: a torn write mixing the
         * two then fails its fixup. */
        Usn = Writer->TailUsn[Writer->TailSlot ^ 1];
        do
        {
            if (++Usn == 0xFFFF)
                Usn = 1;
        } while (Usn == Writer->TailUsn[Writer->TailSlot]);
        NtfsLfsBuildTailCopy(&Writer->Codec, Writer->TailImage, Writer->TailOffset, Usn,
                             Writer->TailBuild);
        Offset = (NTFS_LFS_TAIL_COPY_PAGE + Writer->TailSlot) * Writer->Codec.PageSize;
        Status = NtfsLogRange(Writer, Offset, Writer->Codec.PageSize, Writer->TailBuild);
        Writer->Unflushed = TRUE;
        if (NT_SUCCESS(Status))
        {
            Writer->TailUsn[Writer->TailSlot] = Usn;
            Writer->TailSlot ^= 1;
            Writer->TailQueued = FALSE;
        }
    }
    if (NT_SUCCESS(Status) && Writer->Unflushed)
    {
        Status = Writer->Flush(Writer->Vcb, 0, MAXULONG);
        if (NT_SUCCESS(Status))
            Writer->Unflushed = FALSE;
    }
    /* Every record queued is on disk. */
    if (NT_SUCCESS(Status))
        Writer->DurableLsn = Writer->QueuedLsn;
    if (NT_SUCCESS(Status) && Writer->RestartQueued)
    {
        Status = NtfsLogRange(Writer, Writer->RestartOffset, Writer->Codec.PageSize,
                              Writer->Restart);
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

/* The next restart page, onto the queue, written after the batches queued
 * before it. */
static NTSTATUS
NtfsLogQueueRestart(PNTFS_LOG_WRITER Writer, BOOLEAN Clean)
{
    ULONG Offset;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource));
    Status = NtfsLfsBuildNextRestart(&Writer->Codec, Clean, Writer->RestartBuild, &Offset);
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

/* The pages of the last batch, onto the queue.  The first record of a
 * session marks the restart area dirty. */
static NTSTATUS
NtfsLogQueueBatch(PNTFS_LOG_WRITER Writer)
{
    PLFS_WRITE_CONTEXT Codec = &Writer->Codec;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource));
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Writer->ForceResource, TRUE);
    if (Writer->QueueCount + Codec->StagedPages > NTFS_LOG_QUEUE_PAGES)
        Status = NtfsLogWriteQueue(Writer);
    for (i = 0; NT_SUCCESS(Status) && i < Codec->StagedPages; i++)
    {
        Writer->QueueOffset[Writer->QueueCount] = NtfsLfsStagedPageOffset(Codec, i);
        RtlCopyMemory(Writer->Queue + Writer->QueueCount * Codec->PageSize,
                      Codec->Stage + i * Codec->PageSize, Codec->PageSize);
        Writer->QueueCount++;
    }
    if (NT_SUCCESS(Status))
    {
        Writer->QueuedLsn = Codec->LastLsn;
        Writer->TailQueued = NtfsLfsTailHasRecords(Codec);
        if (Writer->TailQueued)
        {
            RtlCopyMemory(Writer->TailImage, Codec->Tail, Codec->PageSize);
            Writer->TailOffset = Codec->NextPage;
        }
    }
    ExReleaseResourceLite(&Writer->ForceResource);
    KeLeaveCriticalRegion();
    if (NT_SUCCESS(Status) && !Writer->RestartDirty)
        Status = NtfsLogQueueRestart(Writer, FALSE);
    return Status;
}

static VOID
NtfsLogKeepOldest(PULONGLONG Oldest, ULONGLONG Lsn)
{
    if (Lsn != 0 && (*Oldest == 0 || Lsn < *Oldest))
        *Oldest = Lsn;
}

/* Puts the pages of Length volume bytes at Offset in the dirty page table,
 * changed by the record Lsn.  The caller holds PendingLock. */
static VOID
NtfsLogMarkDirty(PNTFS_LOG_WRITER Writer, ULONGLONG Lsn, LONGLONG Offset, ULONG Length)
{
    ULONGLONG Page, Last;
    PLIST_ENTRY Head, Entry;
    PNTFS_LOG_DIRTY_PAGE Dirty;

    if (Length == 0)
        return;
    for (Page = Offset / PAGE_SIZE, Last = (Offset + Length - 1) / PAGE_SIZE; Page <= Last; Page++)
    {
        Head = &Writer->Dirty[Page % NTFS_LOG_DIRTY_BUCKETS];
        for (Entry = Head->Flink, Dirty = NULL; Entry != Head; Entry = Entry->Flink)
        {
            if (CONTAINING_RECORD(Entry, NTFS_LOG_DIRTY_PAGE, Link)->Page == Page)
            {
                Dirty = CONTAINING_RECORD(Entry, NTFS_LOG_DIRTY_PAGE, Link);
                break;
            }
        }
        if (Dirty == NULL)
        {
            Dirty = ExAllocateFromNPagedLookasideList(&Writer->DirtyLookaside);
            if (Dirty == NULL)
            {
                /* Only a flush of the whole volume stream knows it now. */
                NtfsLogKeepOldest(&Writer->FullFlushLsn, Lsn);
                continue;
            }
            RtlZeroMemory(Dirty, sizeof(*Dirty));
            Dirty->Page = Page;
            Dirty->RecLsn = Dirty->LastLsn = Lsn;
            InsertTailList(Head, &Dirty->Link);
            Writer->DirtyCount++;
            continue;
        }
        NtfsLogKeepOldest(&Dirty->RecLsn, Lsn);
        Dirty->LastLsn = max(Dirty->LastLsn, Lsn);
        if (Dirty->Flushing)
            NtfsLogKeepOldest(&Dirty->NewLsn, Lsn);
    }
}

/* The oldest record still needed: of a page in the dirty page table, of a
 * write not issued yet, of an open transaction, of a change only a flush of
 * the whole volume stream knows, or the Noop record covering an unlogged
 * write in progress.  0 when none is.  The caller holds PendingLock. */
static ULONGLONG
NtfsLogOldestNeeded(PNTFS_LOG_WRITER Writer)
{
    PLIST_ENTRY Entry;
    ULONGLONG Oldest = 0;
    ULONG i;

    for (i = 0; i < NTFS_LOG_DIRTY_BUCKETS; i++)
    {
        for (Entry = Writer->Dirty[i].Flink; Entry != &Writer->Dirty[i]; Entry = Entry->Flink)
            NtfsLogKeepOldest(&Oldest, CONTAINING_RECORD(Entry, NTFS_LOG_DIRTY_PAGE, Link)->RecLsn);
    }
    for (Entry = Writer->PendingList.Flink; Entry != &Writer->PendingList; Entry = Entry->Flink)
        NtfsLogKeepOldest(&Oldest, CONTAINING_RECORD(Entry, NTFS_LOG_PENDING, Link)->Lsn);
    for (Entry = Writer->Requests.Flink; Entry != &Writer->Requests; Entry = Entry->Flink)
    {
        PNTFS_LOG_REQUEST Request = CONTAINING_RECORD(Entry, NTFS_LOG_REQUEST, Link);
        if (Request->Session == Writer->Session && !Request->Forced)
            NtfsLogKeepOldest(&Oldest, Request->FirstLsn);
    }
    NtfsLogKeepOldest(&Oldest, Writer->FullFlushLsn);
    if (Writer->UnloggedWrites != 0)
    {
        ASSERT(Writer->MarkerLsn != 0);
        NtfsLogKeepOldest(&Oldest, Writer->MarkerLsn);
    }
    return Oldest;
}

static VOID
NtfsLogSortPages(PULONGLONG Pages, ULONG Count)
{
    ULONG Gap, i, j;
    ULONGLONG Page;

    for (Gap = Count / 2; Gap != 0; Gap /= 2)
    {
        for (i = Gap; i < Count; i++)
        {
            Page = Pages[i];
            for (j = i; j >= Gap && Pages[j - Gap] > Page; j -= Gap)
                Pages[j] = Pages[j - Gap];
            Pages[j] = Page;
        }
    }
}

/* Checkpoint: writes the pages of the dirty page table, puts them on disk,
 * and drops them from the table unless changed since.  (Writing only the
 * ones whose changes keep the older part of the log in use takes twice the
 * checkpoints, each as dear for its barriers, for pages the next one has
 * to write anyway.)  The restart area then names the oldest record still
 * needed, before any of the space is reused.  A change the table does not
 * hold takes a flush of the whole volume stream. */
static NTSTATUS
NtfsLogCheckpointLocked(PNTFS_LOG_WRITER Writer)
{
    PLFS_WRITE_CONTEXT Codec = &Writer->Codec;
    PULONGLONG Pages;
    PLIST_ENTRY Entry, Next;
    ULONGLONG Oldest, FullFlushLsn;
    ULONG Capacity, Count = 0, i, Run;
    BOOLEAN Full;
    NTSTATUS Status;
    KIRQL OldIrql;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource) && Writer->InIo);
    /* The log first, then everything it describes. */
    Status = NtfsLogForce(Writer);
    if (!NT_SUCCESS(Status))
        return Status;
    /* No record is logged until the checkpoint ends; a write issued from
     * here on stays in the table. */
    Capacity = Writer->DirtyCount + 64;
    Pages = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(ULONGLONG), TAG_NTFS);
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    FullFlushLsn = Writer->FullFlushLsn;
    Full = FullFlushLsn != 0 || Pages == NULL;
    Writer->FullFlushLsn = 0;
    for (i = 0; i < NTFS_LOG_DIRTY_BUCKETS; i++)
    {
        for (Entry = Writer->Dirty[i].Flink; Entry != &Writer->Dirty[i]; Entry = Entry->Flink)
        {
            PNTFS_LOG_DIRTY_PAGE Dirty = CONTAINING_RECORD(Entry, NTFS_LOG_DIRTY_PAGE, Link);
            if (!Full && Count == Capacity)
                continue;
            Dirty->Flushing = TRUE;
            Dirty->NewLsn = 0;
            if (!Full)
                Pages[Count++] = Dirty->Page;
        }
    }
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);

    if (Full)
    {
        Status = Writer->Flush(Writer->Vcb, 0, 0);
    }
    else
    {
        /* Consecutive pages in one request. */
        NtfsLogSortPages(Pages, Count);
        for (i = 0; NT_SUCCESS(Status) && i < Count; i += Run)
        {
            for (Run = 1; i + Run < Count && Pages[i + Run] == Pages[i] + Run && Run < MAXULONG / PAGE_SIZE - 1; Run++);
            Status = Writer->Flush(Writer->Vcb, Pages[i] * PAGE_SIZE, Run * PAGE_SIZE);
        }
        if (NT_SUCCESS(Status))
            Status = Writer->Flush(Writer->Vcb, 0, MAXULONG);
    }

    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    for (i = 0; i < NTFS_LOG_DIRTY_BUCKETS; i++)
    {
        for (Entry = Writer->Dirty[i].Flink; Entry != &Writer->Dirty[i]; Entry = Next)
        {
            PNTFS_LOG_DIRTY_PAGE Dirty = CONTAINING_RECORD(Entry, NTFS_LOG_DIRTY_PAGE, Link);
            Next = Entry->Flink;
            if (!Dirty->Flushing)
                continue;
            Dirty->Flushing = FALSE;
            if (!NT_SUCCESS(Status))
                continue;
            if (Dirty->NewLsn != 0)
            {
                /* Changed again while it was written. */
                Dirty->RecLsn = Dirty->NewLsn;
                continue;
            }
            RemoveEntryList(&Dirty->Link);
            Writer->DirtyCount--;
            ExFreeToNPagedLookasideList(&Writer->DirtyLookaside, Dirty);
        }
    }
    if (!NT_SUCCESS(Status))
        NtfsLogKeepOldest(&Writer->FullFlushLsn, FullFlushLsn);
    /* Unlogged writes that ended are on disk after a flush of the whole
     * volume stream: their Noop record is needed no more. */
    if (NT_SUCCESS(Status) && Full && Writer->UnloggedWrites == 0)
        Writer->MarkerLsn = 0;
    Oldest = NtfsLogOldestNeeded(Writer);
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    if (Pages != NULL)
        ExFreePoolWithTag(Pages, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Oldest == Codec->OldestLsn || !Writer->RestartDirty)
    {
        Codec->OldestLsn = Oldest;
        return STATUS_SUCCESS;
    }
    Codec->OldestLsn = Oldest;
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
NtfsLfsForceRange(PVOID Context, LONGLONG Offset, ULONG Length)
{
    PNTFS_LOG_WRITER Writer = Context;
    ULONGLONG Needed = 0, Page, Last;
    PLIST_ENTRY Head, Entry;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (Writer == NULL || !Writer->Enabled || Length == 0)
        return STATUS_SUCCESS;
    /* The newest record of a change the pages may hold: of the pages in
     * the dirty page table, of any write issued but not in it yet (its
     * bytes may be in these pages), and the Noop record covering unlogged
     * writes until a flush of the whole stream. */
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    for (Page = Offset / PAGE_SIZE, Last = (Offset + Length - 1) / PAGE_SIZE; Page <= Last; Page++)
    {
        Head = &Writer->Dirty[Page % NTFS_LOG_DIRTY_BUCKETS];
        for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
        {
            PNTFS_LOG_DIRTY_PAGE Dirty = CONTAINING_RECORD(Entry, NTFS_LOG_DIRTY_PAGE, Link);
            if (Dirty->Page == Page)
                Needed = max(Needed, Dirty->LastLsn);
        }
    }
    for (Entry = Writer->PendingList.Flink; Entry != &Writer->PendingList; Entry = Entry->Flink)
        Needed = max(Needed, CONTAINING_RECORD(Entry, NTFS_LOG_PENDING, Link)->Lsn);
    if (Writer->FullFlushLsn != 0 || Writer->MarkerLsn != 0)
        Needed = MAXULONGLONG;
    KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    if (Needed == 0)
        return STATUS_SUCCESS;
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Writer->ForceResource, TRUE);
    /* A batch is queued whole: its first record on disk, all of it is. */
    if (Needed > Writer->DurableLsn)
        Status = NtfsLogForceLocked(Writer);
    ExReleaseResourceLite(&Writer->ForceResource);
    KeLeaveCriticalRegion();
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

/* The update sequence number of tail-copy page Slot, 0 if it holds none. */
static USHORT
NtfsLogTailCopyUsn(PNTFS_LOG_WRITER Writer, ULONG Slot)
{
    PNTFS_RECORD_HEADER Hdr = (PNTFS_RECORD_HEADER)Writer->TailBuild;

    if (ReadAttribute(Writer->Vcb, Writer->Data, (NTFS_LFS_TAIL_COPY_PAGE + Slot) * PAGE_SIZE,
                      (PCHAR)Writer->TailBuild, PAGE_SIZE) != PAGE_SIZE ||
        Hdr->UsaOffset < sizeof(*Hdr) || Hdr->UsaOffset > PAGE_SIZE - sizeof(USHORT))
        return 0;
    return *(PUSHORT)(Writer->TailBuild + Hdr->UsaOffset);
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
    /* The largest record carries a chunk of changed bytes; the largest
     * batch, the changes of the largest write logged in its records (a
     * file record is smaller), and the record that commits it. */
    Scratch = NtfsLfsRecordLength(NTFS_LOG_METADATA_CHUNK, NTFS_LOG_METADATA_CHUNK, NTFS_LOG_METADATA_LCNS);
    Batch = 2 * max(NTFS_LOG_MAX_CHANGED, Vcb->NtfsInfo.BytesPerFileRecord) +
            NTFS_LOG_BATCH_RECORDS * NtfsLfsRecordLength(0, 0, NTFS_LOG_METADATA_LCNS) +
            NtfsLfsRecordLength(0, 0, 0);
    Pages = NtfsLfsStagePages(PAGE_SIZE, Batch);
    Writer->Codec.Stage = ExAllocatePoolWithTag(PagedPool, Pages * PAGE_SIZE, TAG_NTFS);
    Writer->Codec.Scratch = ExAllocatePoolWithTag(PagedPool, Scratch, TAG_NTFS);
    Writer->Restart = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_NTFS);
    /* A batch always fits the queue after a force. */
    ASSERT(Pages <= NTFS_LOG_QUEUE_PAGES);
    Writer->RestartBuild = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_NTFS);
    Writer->Queue = ExAllocatePoolWithTag(PagedPool, NTFS_LOG_QUEUE_PAGES * PAGE_SIZE, TAG_NTFS);
    Writer->TailImage = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_NTFS);
    Writer->TailBuild = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_NTFS);
    if (Writer->Codec.Stage == NULL || Writer->Codec.Scratch == NULL || Writer->Restart == NULL ||
        Writer->RestartBuild == NULL || Writer->Queue == NULL || Writer->TailImage == NULL ||
        Writer->TailBuild == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    /* What the tail-copy pages hold, maybe from an earlier session: a copy
     * written over one is numbered differently. */
    Writer->TailUsn[0] = NtfsLogTailCopyUsn(Writer, 0);
    Writer->TailUsn[1] = NtfsLogTailCopyUsn(Writer, 1);
    Writer->TailSlot = 0;
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
        Writer->Unflushed = FALSE;
        Writer->QueuedLsn = Writer->DurableLsn = Writer->Codec.LastLsn;
        Writer->TailQueued = FALSE;
        Writer->RestartQueued = FALSE;
        Writer->MarkerLsn = 0;
        Writer->UnloggedWrites = 0;
        Writer->Enabled = TRUE;
        NtfsLogPurgeCache(Writer);
    }
Cleanup:
    if (!NT_SUCCESS(Status))
        NtfsLogFreeSession(Writer);
    ExReleaseResourceLite(&Writer->Resource);
    return Status;
}

/* The records of one logged write: each carries a range of bytes the write
 * changes, redo the new bytes, undo the old ones. */
typedef struct _NTFS_LOG_BATCH
{
    ULONG Count;
    LFS_EMIT_PARAMS Params[NTFS_LOG_BATCH_RECORDS + 1];
    LONGLONG Lcns[NTFS_LOG_BATCH_RECORDS][NTFS_LOG_METADATA_LCNS];
    ULONGLONG Lsns[NTFS_LOG_BATCH_RECORDS + 1];
} NTFS_LOG_BATCH, *PNTFS_LOG_BATCH;

/* Bytes that differ are logged in ranges; ranges closer than this are
 * logged as one, cheaper than a record header between them. */
#define NTFS_LOG_RANGE_GAP 64

/* Where New differs from Old, outside [SkipStart, SkipStart + SkipLength),
 * in ranges at least Gap apart: into Starts/Ends while there is room.
 * Returns the records they take (a record carries at most a chunk). */
static ULONG
NtfsLogDiff(const UCHAR *Old, const UCHAR *New, ULONG Length, ULONG SkipStart, ULONG SkipLength,
            ULONG Gap, PULONG Starts, PULONG Ends, ULONG Room, PULONG RangesOut)
{
    ULONG i = 0, Start, End, Ranges = 0, Records = 0;

    for (;;)
    {
        while (i < Length && (Old[i] == New[i] || (i >= SkipStart && i - SkipStart < SkipLength)))
            i++;
        if (i == Length)
            break;
        Start = i;
        End = i + 1;
        for (i++; i < Length && i - End < Gap; i++)
        {
            if (Old[i] != New[i] && !(i >= SkipStart && i - SkipStart < SkipLength))
                End = i + 1;
        }
        i = End;
        if (Ranges < Room)
        {
            Starts[Ranges] = Start;
            Ends[Ranges] = End;
        }
        Ranges++;
        Records += (End - Start + NTFS_LOG_METADATA_CHUNK - 1) / NTFS_LOG_METADATA_CHUNK;
    }
    *RangesOut = Ranges;
    return Records;
}

/* Adds the records of the bytes where New differs from Old, the Length
 * bytes Within bytes into cluster Vcn of an attribute, at the clusters Lcns
 * lists (or, with no list, the ones from FirstLcn on), in what is left of
 * the batch's records and of the bytes it may carry (*Bytes).  FALSE when
 * they do not fit. */
static BOOLEAN
NtfsLogAddRanges(PNTFS_LOG_BATCH Batch, ULONG Bpc, const UCHAR *Old, const UCHAR *New, ULONG Length,
                 ULONG SkipStart, ULONG SkipLength, ULONGLONG Vcn, ULONG Within, const LONGLONG *Lcns,
                 LONGLONG FirstLcn, PULONG Bytes)
{
    ULONG Starts[NTFS_LOG_BATCH_RECORDS], Ends[NTFS_LOG_BATCH_RECORDS];
    ULONG Gap, Ranges, Records, Changed, r, k, Done, Part, At, Cluster;

    /* Ranges merged further apart take fewer records, more bytes. */
    for (Gap = NTFS_LOG_RANGE_GAP; ; Gap *= 2)
    {
        Records = NtfsLogDiff(Old, New, Length, SkipStart, SkipLength, Gap, Starts, Ends,
                              RTL_NUMBER_OF(Starts), &Ranges);
        if (Records <= NTFS_LOG_BATCH_RECORDS - Batch->Count && Ranges <= RTL_NUMBER_OF(Starts))
            break;
        if (Gap >= Length)
            return FALSE;
    }
    for (r = 0, Changed = 0; r < Ranges; r++)
        Changed += Ends[r] - Starts[r];
    if (Changed > *Bytes)
        return FALSE;
    *Bytes -= Changed;
    for (r = 0; r < Ranges; r++)
    {
        for (Done = Starts[r]; Done < Ends[r]; Done += Part)
        {
            PLFS_EMIT_PARAMS P = &Batch->Params[Batch->Count];
            PLONGLONG L = Batch->Lcns[Batch->Count];

            ASSERT(Batch->Count < NTFS_LOG_BATCH_RECORDS);
            Part = min(Ends[r] - Done, NTFS_LOG_METADATA_CHUNK);
            At = Within + Done;
            Cluster = At / Bpc;
            RtlZeroMemory(P, sizeof(*P));
            P->RedoOperation = P->UndoOperation = NTFS_LFS_OP_UPDATE_NONRESIDENT_VALUE;
            P->RedoData = New + Done;
            P->UndoData = Old + Done;
            P->RedoLength = P->UndoLength = (USHORT)Part;
            P->TargetVcn = Vcn + Cluster;
            P->AttributeOffset = (USHORT)(At % Bpc);
            P->LcnsToFollow = (USHORT)((At % Bpc + Part + Bpc - 1) / Bpc);
            ASSERT(P->LcnsToFollow <= NTFS_LOG_METADATA_LCNS);
            for (k = 0; k < P->LcnsToFollow; k++)
                L[k] = Lcns != NULL ? Lcns[Cluster + k] : FirstLcn + Cluster + k;
            P->Lcns = L;
            Batch->Count++;
        }
    }
    return TRUE;
}

NTSTATUS
NtfsLfsLogFileRecord(PVOID Context, ULONGLONG MftIndex, PFILE_RECORD_HEADER Image,
                     PNTFS_LOG_PENDING Pending)
{
    PNTFS_LOG_WRITER Writer = Context;
    PDEVICE_EXTENSION Vcb;
    PUCHAR Before = NULL;
    PNTFS_LOG_BATCH Batch = NULL;
    LONGLONG Lcn, RunLength, Lcns[16];
    ULONGLONG ByteOffset, FirstVcn, LastVcn;
    ULONG Count, Bpc, Size, Within, Done, Part, i, Bytes = MAXULONG;
    NTSTATUS Status;
    KIRQL OldIrql;

    Pending->Lsn = 0;
    if (Writer == NULL || !Writer->Enabled || (Writer->InIo && ExIsResourceAcquiredExclusiveLite(&Writer->Resource)))
        return STATUS_SUCCESS;
    Vcb = Writer->Vcb;
    Bpc = Vcb->NtfsInfo.BytesPerCluster;
    Size = Vcb->NtfsInfo.BytesPerFileRecord;
    ByteOffset = MftIndex * Size;
    FirstVcn = ByteOffset / Bpc;
    Within = (ULONG)(ByteOffset % Bpc);
    LastVcn = (ByteOffset + Size - 1) / Bpc;
    Count = (ULONG)(LastVcn - FirstVcn + 1);
    if (Count > RTL_NUMBER_OF(Lcns))
        return STATUS_NOT_SUPPORTED;
    ExAcquireResourceSharedLite(&Vcb->MftContextResource, TRUE);
    for (i = 0, Status = STATUS_SUCCESS; i < Count; i++)
    {
        if (!FsRtlLookupLargeMcbEntry(&Vcb->MFTContext->DataRunsMCB, FirstVcn + i,
                                      &Lcn, &RunLength, NULL, NULL, NULL) || Lcn < 0)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        Lcns[i] = Lcn;
    }
    ExReleaseResourceLite(&Vcb->MftContextResource);
    if (!NT_SUCCESS(Status))
        return Status;
    Before = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    Batch = ExAllocatePoolWithTag(PagedPool, sizeof(*Batch), TAG_NTFS);
    if (Before == NULL || Batch == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    /* The bytes the write replaces, as the volume holds them: read before
     * taking the writer resource, which other metadata writers can wait
     * for while they own MftContextResource.  A slot never initialized, or
     * a new extent of the MFT, holds what its clusters held. */
    for (i = 0, Done = 0; NT_SUCCESS(Status) && Done < Size; i++, Done += Part)
    {
        Part = min(Bpc - (i == 0 ? Within : 0), Size - Done);
        Status = NtfsReadDiskCached(Vcb, Lcns[i] * Bpc + (i == 0 ? Within : 0), Part, Before + Done);
    }
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    /* The changed bytes, but for the record's LSN, stamped once it is
     * known: it has no part in replaying the record. */
    Batch->Count = 0;
    if (!NtfsLogAddRanges(Batch, Bpc, Before, (const UCHAR *)Image, Size,
                          FIELD_OFFSET(NTFS_RECORD_HEADER, Lsn), sizeof(ULONGLONG),
                          FirstVcn, Within, Lcns, 0, &Bytes))
    {
        /* A file record's chunks always fit. */
        ASSERT(FALSE);
        Status = STATUS_INTERNAL_ERROR;
        goto Cleanup;
    }
    if (Batch->Count == 0)
        goto Cleanup;
    /* The bytes the write changes, cluster by cluster. */
    for (i = 0, Done = 0, Pending->ExtentCount = 0; Done < Size; i++, Done += Part)
    {
        Part = min(Bpc - (i == 0 ? Within : 0), Size - Done);
        if (Pending->ExtentCount != 0 &&
            Pending->Extents[Pending->ExtentCount - 1].Offset + Pending->Extents[Pending->ExtentCount - 1].Length ==
            Lcns[i] * Bpc + (i == 0 ? Within : 0))
        {
            Pending->Extents[Pending->ExtentCount - 1].Length += Part;
            continue;
        }
        Pending->Extents[Pending->ExtentCount].Offset = Lcns[i] * Bpc + (i == 0 ? Within : 0);
        Pending->Extents[Pending->ExtentCount].Length = Part;
        Pending->ExtentCount++;
    }
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    Writer->InIo = TRUE;
    Status = NtfsLogEmitOwn(Writer, Batch->Params, Batch->Count, Batch->Lsns);
    if (NT_SUCCESS(Status))
    {
        Pending->Lsn = Batch->Lsns[0];
        KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
        InsertTailList(&Writer->PendingList, &Pending->Link);
        KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    }
    Writer->InIo = FALSE;
    ExReleaseResourceLite(&Writer->Resource);
Cleanup:
    if (Batch != NULL)
        ExFreePoolWithTag(Batch, TAG_NTFS);
    if (Before != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Before);
    return Status;
}

NTSTATUS
NtfsLfsBeginMetadataWrite(PVOID Context, const NTFS_LOG_EXTENT *Extents, ULONG Count,
                          const UCHAR *Buffer, ULONGLONG Offset, PNTFS_LOG_METADATA_WRITE Write)
{
    PNTFS_LOG_WRITER Writer = Context;
    PDEVICE_EXTENSION Vcb;
    PNTFS_LOG_BATCH Batch = NULL;
    PUCHAR Undo = NULL;
    ULONG Total = 0, Bytes = NTFS_LOG_MAX_CHANGED, i, Bpc;
    BOOLEAN Fits = TRUE;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    Write->Pending.Lsn = 0;
    Write->Unlogged = FALSE;
    if (Writer == NULL || !Writer->Enabled || (Writer->InIo && ExIsResourceAcquiredExclusiveLite(&Writer->Resource)))
        return STATUS_SUCCESS;
    Vcb = Writer->Vcb;
    Bpc = Vcb->NtfsInfo.BytesPerCluster;
    for (i = 0; i < Count; i++)
        Total += Extents[i].Length;
    if (Total == 0)
        return STATUS_SUCCESS;
    if (Total <= NTFS_LOG_MAX_METADATA_WRITE && Count <= NTFS_LOG_METADATA_EXTENTS)
    {
        Batch = ExAllocatePoolWithTag(PagedPool, sizeof(*Batch), TAG_NTFS);
        Undo = ExAllocatePoolWithTag(PagedPool, Total, TAG_NTFS);
        if (Batch == NULL || Undo == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        /* The bytes the write replaces, and the ranges of them it changes. */
        Batch->Count = 0;
        for (i = 0, Total = 0; Fits && i < Count; i++)
        {
            Status = NtfsReadDiskCached(Vcb, Extents[i].DiskOffset, Extents[i].Length, Undo + Total);
            if (!NT_SUCCESS(Status))
                goto Cleanup;
            Fits = NtfsLogAddRanges(Batch, Bpc, Undo + Total, Buffer + Extents[i].BufferOffset,
                                    Extents[i].Length, 0, 0, (Offset + Extents[i].BufferOffset) / Bpc,
                                    (ULONG)(Extents[i].DiskOffset % Bpc), NULL, Extents[i].DiskOffset / Bpc,
                                    &Bytes);
            Total += Extents[i].Length;
        }
    }
    else
    {
        Fits = FALSE;
    }
    if (!Fits)
    {
        /* Too large or too scattered a change to log (a whole volume
         * bitmap): the volume is checked after a crash. */
        Status = NtfsLfsBeginUnloggedWrite(Writer);
        Write->Unlogged = NT_SUCCESS(Status);
        goto Cleanup;
    }
    if (Batch->Count == 0)
        goto Cleanup;
    Write->Pending.ExtentCount = Count <= NTFS_LOG_PENDING_EXTENTS ? Count : MAXULONG;
    for (i = 0; i < Count && i < NTFS_LOG_PENDING_EXTENTS; i++)
    {
        Write->Pending.Extents[i].Offset = Extents[i].DiskOffset;
        Write->Pending.Extents[i].Length = Extents[i].Length;
    }

    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    Writer->InIo = TRUE;
    Status = NtfsLogEmitOwn(Writer, Batch->Params, Batch->Count, Batch->Lsns);
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
    if (Undo != NULL)
        ExFreePoolWithTag(Undo, TAG_NTFS);
    if (Batch != NULL)
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
    ULONG i;
    KIRQL OldIrql;

    if (Pending->Lsn == 0)
        return;
    /* Issued: its bytes are dirty in the volume stream until a checkpoint
     * writes them.  Under the lock a checkpoint looks for the oldest record
     * needed with. */
    KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
    RemoveEntryList(&Pending->Link);
    if (Pending->ExtentCount == MAXULONG)
    {
        NtfsLogKeepOldest(&Writer->FullFlushLsn, Pending->Lsn);
    }
    else
    {
        for (i = 0; i < Pending->ExtentCount; i++)
            NtfsLogMarkDirty(Writer, Pending->Lsn, Pending->Extents[i].Offset, Pending->Extents[i].Length);
    }
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
    /* Its bytes are known only to a flush of the whole volume stream: until
     * one, recovery is to see the Noop record. */
    NtfsLogKeepOldest(&Writer->FullFlushLsn, Writer->MarkerLsn);
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
    KIRQL OldIrql;

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
    if (NT_SUCCESS(Status))
    {
        /* Everything logged is on disk. */
        KeAcquireSpinLock(&Writer->PendingLock, &OldIrql);
        NtfsLogFreeDirty(Writer);
        KeReleaseSpinLock(&Writer->PendingLock, OldIrql);
    }
    if (NT_SUCCESS(Status)) Status = NtfsLogQueueRestart(Writer, TRUE);
    if (NT_SUCCESS(Status)) Status = NtfsLogCacheRestart(Writer);
    if (NT_SUCCESS(Status)) Status = NtfsLogQueueRestart(Writer, TRUE);
    if (NT_SUCCESS(Status)) Status = NtfsLogCacheRestart(Writer);
    if (NT_SUCCESS(Status)) Status = NtfsLogForce(Writer);
    if (NT_SUCCESS(Status))
    {
        NtfsLogPurgeCache(Writer);
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
    ExDeleteNPagedLookasideList(&Writer->DirtyLookaside);
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
