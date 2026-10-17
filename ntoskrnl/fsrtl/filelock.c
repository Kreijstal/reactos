/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/fsrtl/filelock.c
 * PURPOSE:         File Locking implementation for File System Drivers
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

PAGED_LOOKASIDE_LIST FsRtlFileLockLookasideList;

/* Note: this aligns the two types of lock entry structs so we can access the
   FILE_LOCK_INFO part in common.  Add elements after Shared if new stuff is needed.
*/
typedef union _COMBINED_LOCK_ELEMENT
{
    struct
    {
        LIST_ENTRY dummy;
        FILE_SHARED_LOCK_ENTRY Shared;
    };
    FILE_EXCLUSIVE_LOCK_ENTRY Exclusive;
}
    COMBINED_LOCK_ELEMENT, *PCOMBINED_LOCK_ELEMENT;

typedef struct _LOCK_INFORMATION
{
    RTL_GENERIC_TABLE RangeTable;
    IO_CSQ Csq;
    KSPIN_LOCK CsqLock;
    LIST_ENTRY CsqList;
    PFILE_LOCK BelongsTo;
    LIST_ENTRY SharedLocks;
    ULONG Generation;
}
    LOCK_INFORMATION, *PLOCK_INFORMATION;

typedef struct _LOCK_SHARED_RANGE
{
    LIST_ENTRY Entry;
    LARGE_INTEGER Start, End;
    ULONG Key;
    PVOID ProcessId;
    PFILE_OBJECT FileObject;
}
    LOCK_SHARED_RANGE, *PLOCK_SHARED_RANGE;

/* A wakeup selects overlapping ranges; cleanup selects the pending locks of
 * one open. Both selections must stay inside the cancel-safe queue lock. */
typedef struct _LOCK_QUEUE_FILTER
{
    PCOMBINED_LOCK_ELEMENT Range;
    PFILE_OBJECT FileObject;
    PVOID Process;
    BOOLEAN MatchKey;
    ULONG Key;
} LOCK_QUEUE_FILTER, *PLOCK_QUEUE_FILTER;

/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
FsRtlCompleteLockIrpReal(IN PCOMPLETE_LOCK_IRP_ROUTINE CompleteRoutine,
                         IN PVOID Context,
                         IN PIRP Irp,
                         IN NTSTATUS Status,
                         OUT PNTSTATUS NewStatus,
                         IN PFILE_OBJECT FileObject OPTIONAL);

/* Generic table methods */

static PVOID NTAPI LockAllocate(PRTL_GENERIC_TABLE Table, CLONG Bytes)
{
    PVOID Result;
    Result = ExAllocatePoolWithTag(NonPagedPool, Bytes, TAG_TABLE);
    DPRINT("LockAllocate(%lu) => %p\n", Bytes, Result);
    return Result;
}

static VOID NTAPI LockFree(PRTL_GENERIC_TABLE Table, PVOID Buffer)
{
    DPRINT("LockFree(%p)\n", Buffer);
    ExFreePoolWithTag(Buffer, TAG_TABLE);
}

static RTL_GENERIC_COMPARE_RESULTS NTAPI LockCompare
(PRTL_GENERIC_TABLE Table, PVOID PtrA, PVOID PtrB)
{
    PCOMBINED_LOCK_ELEMENT A = PtrA, B = PtrB;
    RTL_GENERIC_COMPARE_RESULTS Result;
#if 0
    DPRINT("Starting to compare element %x to element %x\n", PtrA, PtrB);
#endif
    /* Match if we overlap */
    if (((A->Exclusive.FileLock.StartingByte.QuadPart <
          B->Exclusive.FileLock.EndingByte.QuadPart) &&
         (A->Exclusive.FileLock.StartingByte.QuadPart >=
          B->Exclusive.FileLock.StartingByte.QuadPart)) ||
        ((B->Exclusive.FileLock.StartingByte.QuadPart <
          A->Exclusive.FileLock.EndingByte.QuadPart) &&
         (B->Exclusive.FileLock.StartingByte.QuadPart >=
          A->Exclusive.FileLock.StartingByte.QuadPart)))
        return GenericEqual;
    /* Otherwise, key on the starting byte */
    Result =
        (A->Exclusive.FileLock.StartingByte.QuadPart <
         B->Exclusive.FileLock.StartingByte.QuadPart) ? GenericLessThan :
        (A->Exclusive.FileLock.StartingByte.QuadPart >
         B->Exclusive.FileLock.StartingByte.QuadPart) ? GenericGreaterThan :
        GenericEqual;
#if 0
    DPRINT("Compare(%x:%x) %x-%x to %x-%x => %d\n",
           A,B,
           A->Exclusive.FileLock.StartingByte.LowPart,
           A->Exclusive.FileLock.EndingByte.LowPart,
           B->Exclusive.FileLock.StartingByte.LowPart,
           B->Exclusive.FileLock.EndingByte.LowPart,
           Result);
#endif
    return Result;
}

/* CSQ methods */

static NTSTATUS NTAPI LockInsertIrpEx
(PIO_CSQ Csq,
 PIRP Irp,
 PVOID InsertContext)
{
    PLOCK_INFORMATION LockInfo = CONTAINING_RECORD(Csq, LOCK_INFORMATION, Csq);
    InsertTailList(&LockInfo->CsqList, &Irp->Tail.Overlay.ListEntry);
    return STATUS_SUCCESS;
}

static VOID NTAPI LockRemoveIrp(PIO_CSQ Csq, PIRP Irp)
{
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

static PIRP NTAPI LockPeekNextIrp(PIO_CSQ Csq, PIRP Irp, PVOID PeekContext)
{
    PLOCK_QUEUE_FILTER Filter = PeekContext;
    PLOCK_INFORMATION LockInfo = CONTAINING_RECORD(Csq, LOCK_INFORMATION, Csq);
    PLIST_ENTRY Entry = Irp ? Irp->Tail.Overlay.ListEntry.Flink : LockInfo->CsqList.Flink;

    for (; Entry != &LockInfo->CsqList; Entry = Entry->Flink)
    {
        PIO_STACK_LOCATION Stack;
        COMBINED_LOCK_ELEMENT Range;

        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (Filter == NULL)
            return Irp;
        Stack = IoGetCurrentIrpStackLocation(Irp);
        if (Filter->FileObject != NULL)
        {
            if (Stack->FileObject == Filter->FileObject &&
                IoGetRequestorProcess(Irp) == Filter->Process &&
                (!Filter->MatchKey || Stack->Parameters.LockControl.Key == Filter->Key))
                return Irp;
        }
        else
        {
            Range.Exclusive.FileLock.StartingByte = Stack->Parameters.LockControl.ByteOffset;
            Range.Exclusive.FileLock.EndingByte.QuadPart =
                Range.Exclusive.FileLock.StartingByte.QuadPart + Stack->Parameters.LockControl.Length->QuadPart;
            if (LockCompare(&LockInfo->RangeTable, &Range, Filter->Range) == GenericEqual)
                return Irp;
        }
    }
    return NULL;
}

static VOID NTAPI
LockAcquireQueueLock(PIO_CSQ Csq, PKIRQL Irql)
{
    PLOCK_INFORMATION LockInfo = CONTAINING_RECORD(Csq, LOCK_INFORMATION, Csq);
    KeAcquireSpinLock(&LockInfo->CsqLock, Irql);
}

static VOID NTAPI
LockReleaseQueueLock(PIO_CSQ Csq, KIRQL Irql)
{
    PLOCK_INFORMATION LockInfo = CONTAINING_RECORD(Csq, LOCK_INFORMATION, Csq);
    KeReleaseSpinLock(&LockInfo->CsqLock, Irql);
}

static VOID NTAPI
LockCompleteCanceledIrp(PIO_CSQ Csq, PIRP Irp)
{
    NTSTATUS Status;
    PLOCK_INFORMATION LockInfo = CONTAINING_RECORD(Csq, LOCK_INFORMATION, Csq);
    DPRINT("Complete cancelled IRP %p Status %x\n", Irp, STATUS_CANCELLED);
    FsRtlCompleteLockIrpReal
        (LockInfo->BelongsTo->CompleteLockIrpRoutine,
         NULL,
         Irp,
         STATUS_CANCELLED,
         &Status,
         NULL);
}

VOID
NTAPI
FsRtlCompleteLockIrpReal(IN PCOMPLETE_LOCK_IRP_ROUTINE CompleteRoutine,
                         IN PVOID Context,
                         IN PIRP Irp,
                         IN NTSTATUS Status,
                         OUT PNTSTATUS NewStatus,
                         IN PFILE_OBJECT FileObject OPTIONAL)
{
    /* Check if we have a complete routine */
    Irp->IoStatus.Information = 0;
    if (CompleteRoutine)
    {
        /* Check if we have a file object */
        if (FileObject) FileObject->LastLock = NULL;

        /* Set the I/O Status and do completion */
        Irp->IoStatus.Status = Status;
        DPRINT("Calling completion routine %p Status %x\n", Irp, Status);
        *NewStatus = CompleteRoutine(Context, Irp);
    }
    else
    {
        /* Otherwise do a normal I/O complete request */
        DPRINT("Completing IRP %p Status %x\n", Irp, Status);
        FsRtlCompleteRequest(Irp, Status);
        *NewStatus = Status;
    }
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
PFILE_LOCK_INFO
NTAPI
FsRtlGetNextFileLock(IN PFILE_LOCK FileLock,
                     IN BOOLEAN Restart)
{
    PCOMBINED_LOCK_ELEMENT Entry;
    if (!FileLock->LockInformation) return NULL;
    Entry = RtlEnumerateGenericTable(FileLock->LockInformation, Restart);
    if (!Entry) return NULL;
    else return &Entry->Exclusive.FileLock;
}

VOID
NTAPI
FsRtlpExpandLockElement
(PCOMBINED_LOCK_ELEMENT ToExpand,
 PCOMBINED_LOCK_ELEMENT Conflict)
{
    if (ToExpand->Exclusive.FileLock.StartingByte.QuadPart >
        Conflict->Exclusive.FileLock.StartingByte.QuadPart)
    {
        ToExpand->Exclusive.FileLock.StartingByte =
            Conflict->Exclusive.FileLock.StartingByte;
    }
    if (ToExpand->Exclusive.FileLock.EndingByte.QuadPart <
        Conflict->Exclusive.FileLock.EndingByte.QuadPart)
    {
        ToExpand->Exclusive.FileLock.EndingByte =
            Conflict->Exclusive.FileLock.EndingByte;
    }
}

/* This function expands the conflicting range Conflict by removing and reinserting it,
   then adds a shared range of the same size */
PCOMBINED_LOCK_ELEMENT
NTAPI
FsRtlpRebuildSharedLockRange
(PFILE_LOCK FileLock,
 PLOCK_INFORMATION LockInfo,
 PCOMBINED_LOCK_ELEMENT Conflict)
{
    /* Starting at Conflict->StartingByte and going to Conflict->EndingByte
     * capture and expand a shared range from the shared range list.
     * Finish when we've incorporated all overlapping shared regions.
     */
    BOOLEAN InsertedNew = FALSE, RemovedOld;
    COMBINED_LOCK_ELEMENT NewElement = *Conflict;
    PCOMBINED_LOCK_ELEMENT Entry;
    while ((Entry = RtlLookupElementGenericTable
            (FileLock->LockInformation, &NewElement)))
    {
        FsRtlpExpandLockElement(&NewElement, Entry);
        RemovedOld = RtlDeleteElementGenericTable
            (&LockInfo->RangeTable,
             Entry);
        ASSERT(RemovedOld);
    }
    Conflict = RtlInsertElementGenericTable
        (&LockInfo->RangeTable,
         &NewElement,
         sizeof(NewElement),
         &InsertedNew);
    ASSERT(InsertedNew);
    return Conflict;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
FsRtlPrivateLock(IN PFILE_LOCK FileLock,
                 IN PFILE_OBJECT FileObject,
                 IN PLARGE_INTEGER FileOffset,
                 IN PLARGE_INTEGER Length,
                 IN PEPROCESS Process,
                 IN ULONG Key,
                 IN BOOLEAN FailImmediately,
                 IN BOOLEAN ExclusiveLock,
                 OUT PIO_STATUS_BLOCK IoStatus,
                 IN PIRP Irp OPTIONAL,
                 IN PVOID Context OPTIONAL,
                 IN BOOLEAN AlreadySynchronized)
{
    NTSTATUS Status;
    COMBINED_LOCK_ELEMENT ToInsert;
    PCOMBINED_LOCK_ELEMENT Conflict;
    PLOCK_INFORMATION LockInfo;
    PLOCK_SHARED_RANGE NewSharedRange;
    BOOLEAN InsertedNew;
    ULARGE_INTEGER UnsignedStart;
    ULARGE_INTEGER UnsignedEnd;

    DPRINT("FsRtlPrivateLock(%wZ, Offset %08x%08x (%d), Length %08x%08x (%d), Key %x, FailImmediately %u, Exclusive %u)\n",
           &FileObject->FileName,
           FileOffset->HighPart,
           FileOffset->LowPart,
           (int)FileOffset->QuadPart,
           Length->HighPart,
           Length->LowPart,
           (int)Length->QuadPart,
           Key,
           FailImmediately,
           ExclusiveLock);

    UnsignedStart.QuadPart = FileOffset->QuadPart;
    UnsignedEnd.QuadPart = FileOffset->QuadPart + Length->QuadPart;

    if (UnsignedEnd.QuadPart < UnsignedStart.QuadPart)
    {
        DPRINT("File offset out of range\n");
        IoStatus->Status = STATUS_INVALID_PARAMETER;
        if (Irp)
        {
            DPRINT("Complete lock %p Status %x\n", Irp, IoStatus->Status);
            FsRtlCompleteLockIrpReal
                (FileLock->CompleteLockIrpRoutine,
                 Context,
                 Irp,
                 IoStatus->Status,
                 &Status,
                 FileObject);
        }
        return FALSE;
    }

    /* Initialize the lock, if necessary */
    if (!FileLock->LockInformation)
    {
        LockInfo = ExAllocatePoolWithTag(NonPagedPool, sizeof(LOCK_INFORMATION), TAG_FLOCK);
        if (!LockInfo)
        {
            IoStatus->Status = STATUS_NO_MEMORY;
            if (Irp)
                FsRtlCompleteLockIrpReal(FileLock->CompleteLockIrpRoutine, Context,
                                         Irp, IoStatus->Status, &Status, FileObject);
            return FALSE;
        }
        FileLock->LockInformation = LockInfo;

        LockInfo->BelongsTo = FileLock;
        InitializeListHead(&LockInfo->SharedLocks);

        RtlInitializeGenericTable
            (&LockInfo->RangeTable,
             LockCompare,
             LockAllocate,
             LockFree,
             NULL);

        KeInitializeSpinLock(&LockInfo->CsqLock);
        InitializeListHead(&LockInfo->CsqList);

        IoCsqInitializeEx
            (&LockInfo->Csq,
             LockInsertIrpEx,
             LockRemoveIrp,
             LockPeekNextIrp,
             LockAcquireQueueLock,
             LockReleaseQueueLock,
             LockCompleteCanceledIrp);
    }

    LockInfo = FileLock->LockInformation;
    ToInsert.Exclusive.FileLock.FileObject = FileObject;
    ToInsert.Exclusive.FileLock.StartingByte = *FileOffset;
    ToInsert.Exclusive.FileLock.EndingByte.QuadPart = FileOffset->QuadPart + Length->QuadPart;
    ToInsert.Exclusive.FileLock.ProcessId = Process;
    ToInsert.Exclusive.FileLock.Key = Key;
    ToInsert.Exclusive.FileLock.ExclusiveLock = ExclusiveLock;

    Conflict = RtlInsertElementGenericTable
        (FileLock->LockInformation,
         &ToInsert,
         sizeof(ToInsert),
         &InsertedNew);

    if (Conflict && !InsertedNew)
    {
        if (Conflict->Exclusive.FileLock.ExclusiveLock || ExclusiveLock)
        {
            DPRINT("Conflict %08x%08x:%08x%08x Exc %u (Want Exc %u)\n",
                   Conflict->Exclusive.FileLock.StartingByte.HighPart,
                   Conflict->Exclusive.FileLock.StartingByte.LowPart,
                   Conflict->Exclusive.FileLock.EndingByte.HighPart,
                   Conflict->Exclusive.FileLock.EndingByte.LowPart,
                   Conflict->Exclusive.FileLock.ExclusiveLock,
                   ExclusiveLock);
            if (FailImmediately)
            {
                DPRINT("STATUS_LOCK_NOT_GRANTED\n");
                IoStatus->Status = STATUS_LOCK_NOT_GRANTED;
                if (Irp)
                {
                    DPRINT("STATUS_LOCK_NOT_GRANTED: Complete\n");
                    FsRtlCompleteLockIrpReal
                        (FileLock->CompleteLockIrpRoutine,
                         Context,
                         Irp,
                         IoStatus->Status,
                         &Status,
                         FileObject);
                }
                return FALSE;
            }
            else
            {
                IoStatus->Status = STATUS_PENDING;
                if (Irp)
                {
                    Irp->IoStatus.Information = LockInfo->Generation;
                    IoMarkIrpPending(Irp);
                    IoCsqInsertIrpEx
                        (&LockInfo->Csq,
                         Irp,
                         NULL,
                         NULL);
                }
            }
            return FALSE;
        }
        else
        {
            ULONG i;
            /* We know of at least one lock in range that's shared.  We need to
             * find out if any more exist and any are exclusive. */
            for (i = 0; i < RtlNumberGenericTableElements(&LockInfo->RangeTable); i++)
            {
                Conflict = RtlGetElementGenericTable(&LockInfo->RangeTable, i);

                /* The first argument will be inserted as a shared range */
                if (Conflict && (LockCompare(&LockInfo->RangeTable, Conflict, &ToInsert) == GenericEqual))
                {
                    if (Conflict->Exclusive.FileLock.ExclusiveLock)
                    {
                        /* Found an exclusive match */
                        if (FailImmediately)
                        {
                            IoStatus->Status = STATUS_LOCK_NOT_GRANTED;
                            DPRINT("STATUS_LOCK_NOT_GRANTED\n");
                            if (Irp)
                            {
                                DPRINT("STATUS_LOCK_NOT_GRANTED: Complete\n");
                                FsRtlCompleteLockIrpReal
                                    (FileLock->CompleteLockIrpRoutine,
                                     Context,
                                     Irp,
                                     IoStatus->Status,
                                     &Status,
                                     FileObject);
                            }
                        }
                        else
                        {
                            IoStatus->Status = STATUS_PENDING;
                            if (Irp)
                            {
                                IoMarkIrpPending(Irp);
                                IoCsqInsertIrpEx
                                    (&LockInfo->Csq,
                                     Irp,
                                     NULL,
                                     NULL);
                            }
                        }
                        return FALSE;
                    }
                }
            }

            DPRINT("Overlapping shared lock %wZ %08x%08x %08x%08x\n",
                   &FileObject->FileName,
                   Conflict->Exclusive.FileLock.StartingByte.HighPart,
                   Conflict->Exclusive.FileLock.StartingByte.LowPart,
                   Conflict->Exclusive.FileLock.EndingByte.HighPart,
                   Conflict->Exclusive.FileLock.EndingByte.LowPart);
            Conflict = FsRtlpRebuildSharedLockRange(FileLock,
                                                    LockInfo,
                                                    &ToInsert);
            if (!Conflict)
            {
                IoStatus->Status = STATUS_NO_MEMORY;
                if (Irp)
                {
                    FsRtlCompleteLockIrpReal
                        (FileLock->CompleteLockIrpRoutine,
                         Context,
                         Irp,
                         IoStatus->Status,
                         &Status,
                         FileObject);
                }
                return FALSE;
            }

            /* We got here because there were only overlapping shared locks */
            /* A shared lock is both a range *and* a list entry.  Insert the
               entry here. */

            DPRINT("Adding shared lock %wZ\n", &FileObject->FileName);
            NewSharedRange =
                ExAllocatePoolWithTag(NonPagedPool, sizeof(*NewSharedRange), TAG_RANGE);
            if (!NewSharedRange)
            {
                IoStatus->Status = STATUS_NO_MEMORY;
                if (Irp)
                {
                    FsRtlCompleteLockIrpReal
                        (FileLock->CompleteLockIrpRoutine,
                         Context,
                         Irp,
                         IoStatus->Status,
                         &Status,
                         FileObject);
                }
                return FALSE;
            }
            DPRINT("Adding shared lock %wZ\n", &FileObject->FileName);
            NewSharedRange->Start = *FileOffset;
            NewSharedRange->End.QuadPart = FileOffset->QuadPart + Length->QuadPart;
            NewSharedRange->Key = Key;
            NewSharedRange->FileObject = FileObject;
            NewSharedRange->ProcessId = ToInsert.Exclusive.FileLock.ProcessId;
            InsertTailList(&LockInfo->SharedLocks, &NewSharedRange->Entry);

            DPRINT("Acquired shared lock %wZ %08x%08x %08x%08x\n",
                   &FileObject->FileName,
                   Conflict->Exclusive.FileLock.StartingByte.HighPart,
                   Conflict->Exclusive.FileLock.StartingByte.LowPart,
                   Conflict->Exclusive.FileLock.EndingByte.HighPart,
                   Conflict->Exclusive.FileLock.EndingByte.LowPart);
            FileLock->FastIoIsQuestionable = TRUE;
            IoStatus->Status = STATUS_SUCCESS;
            if (Irp)
            {
                FsRtlCompleteLockIrpReal
                    (FileLock->CompleteLockIrpRoutine,
                     Context,
                     Irp,
                     IoStatus->Status,
                     &Status,
                     FileObject);
            }
            return TRUE;
        }
    }
    else if (!Conflict)
    {
        /* Conflict here is (or would be) the newly inserted element, but we ran
         * out of space probably. */
        IoStatus->Status = STATUS_NO_MEMORY;
        if (Irp)
        {
            FsRtlCompleteLockIrpReal
                (FileLock->CompleteLockIrpRoutine,
                 Context,
                 Irp,
                 IoStatus->Status,
                 &Status,
                 FileObject);
        }
        return FALSE;
    }
    else
    {
        DPRINT("Inserted new lock %wZ %08x%08x %08x%08x exclusive %u\n",
               &FileObject->FileName,
               Conflict->Exclusive.FileLock.StartingByte.HighPart,
               Conflict->Exclusive.FileLock.StartingByte.LowPart,
               Conflict->Exclusive.FileLock.EndingByte.HighPart,
               Conflict->Exclusive.FileLock.EndingByte.LowPart,
               Conflict->Exclusive.FileLock.ExclusiveLock);
        if (!ExclusiveLock)
        {
            NewSharedRange =
                ExAllocatePoolWithTag(NonPagedPool, sizeof(*NewSharedRange), TAG_RANGE);
            if (!NewSharedRange)
            {
                IoStatus->Status = STATUS_NO_MEMORY;
                if (Irp)
                {
                    FsRtlCompleteLockIrpReal
                        (FileLock->CompleteLockIrpRoutine,
                         Context,
                         Irp,
                         IoStatus->Status,
                         &Status,
                         FileObject);
                }
                return FALSE;
            }
            DPRINT("Adding shared lock %wZ\n", &FileObject->FileName);
            NewSharedRange->Start = *FileOffset;
            NewSharedRange->End.QuadPart = FileOffset->QuadPart + Length->QuadPart;
            NewSharedRange->Key = Key;
            NewSharedRange->FileObject = FileObject;
            NewSharedRange->ProcessId = Process;
            InsertTailList(&LockInfo->SharedLocks, &NewSharedRange->Entry);
        }

        /* Assume all is cool, and lock is set */
        FileLock->FastIoIsQuestionable = TRUE;
            IoStatus->Status = STATUS_SUCCESS;

        if (Irp)
        {
            /* Complete the request */
            FsRtlCompleteLockIrpReal(FileLock->CompleteLockIrpRoutine,
                                     Context,
                                     Irp,
                                     IoStatus->Status,
                                     &Status,
                                     FileObject);

            /* Update the status */
            IoStatus->Status = Status;
        }
    }

    return TRUE;
}

/* A request can span several disjoint locked ranges. An allowed/shared
 * first match cannot hide an exclusive lock farther into the request. */
static
BOOLEAN
FsRtlpCheckLockAccess(PFILE_LOCK FileLock, PLARGE_INTEGER Offset, PLARGE_INTEGER Length,
                      ULONG Key, PFILE_OBJECT FileObject, PVOID Process, BOOLEAN Write)
{
    PLOCK_INFORMATION Info = FileLock->LockInformation;
    COMBINED_LOCK_ELEMENT Range;
    PCOMBINED_LOCK_ELEMENT Entry;
    ULONG i;

    if (Info == NULL)
        return TRUE;
    Range.Exclusive.FileLock.StartingByte = *Offset;
    Range.Exclusive.FileLock.EndingByte.QuadPart = Offset->QuadPart + Length->QuadPart;
    for (i = 0; i < RtlNumberGenericTableElements(&Info->RangeTable); ++i)
    {
        Entry = RtlGetElementGenericTable(&Info->RangeTable, i);
        if (LockCompare(&Info->RangeTable, &Range, Entry) != GenericEqual)
            continue;
        if (Entry->Exclusive.FileLock.ExclusiveLock)
        {
            if (Entry->Exclusive.FileLock.FileObject != FileObject ||
                Entry->Exclusive.FileLock.ProcessId != Process ||
                Entry->Exclusive.FileLock.Key != Key)
                return FALSE;
        }
        else if (Write)
        {
            /* A shared lock denies writes even by its own holder. */
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
FsRtlCheckLockForReadAccess(IN PFILE_LOCK FileLock, IN PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    LARGE_INTEGER Length;

    Length.QuadPart = Stack->Parameters.Read.Length;
    return FsRtlpCheckLockAccess(FileLock, &Stack->Parameters.Read.ByteOffset, &Length,
                                 Stack->Parameters.Read.Key, Stack->FileObject,
                                 IoGetRequestorProcess(Irp), FALSE);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
FsRtlCheckLockForWriteAccess(IN PFILE_LOCK FileLock, IN PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    LARGE_INTEGER Length;

    Length.QuadPart = Stack->Parameters.Write.Length;
    return FsRtlpCheckLockAccess(FileLock, &Stack->Parameters.Write.ByteOffset, &Length,
                                 Stack->Parameters.Write.Key, Stack->FileObject,
                                 IoGetRequestorProcess(Irp), TRUE);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
FsRtlFastCheckLockForRead(IN PFILE_LOCK FileLock, IN PLARGE_INTEGER FileOffset,
                          IN PLARGE_INTEGER Length, IN ULONG Key,
                          IN PFILE_OBJECT FileObject, IN PVOID Process)
{
    return FsRtlpCheckLockAccess(FileLock, FileOffset, Length, Key, FileObject, Process, FALSE);
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
FsRtlFastCheckLockForWrite(IN PFILE_LOCK FileLock, IN PLARGE_INTEGER FileOffset,
                           IN PLARGE_INTEGER Length, IN ULONG Key,
                           IN PFILE_OBJECT FileObject, IN PVOID Process)
{
    return FsRtlpCheckLockAccess(FileLock, FileOffset, Length, Key, FileObject, Process, TRUE);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlFastUnlockSingle(IN PFILE_LOCK FileLock,
                      IN PFILE_OBJECT FileObject,
                      IN PLARGE_INTEGER FileOffset,
                      IN PLARGE_INTEGER Length,
                      IN PEPROCESS Process,
                      IN ULONG Key,
                      IN PVOID Context OPTIONAL,
                      IN BOOLEAN AlreadySynchronized)
{
    BOOLEAN FoundShared = FALSE;
    PLIST_ENTRY SharedEntry;
    PLOCK_SHARED_RANGE SharedRange = NULL;
    COMBINED_LOCK_ELEMENT Find;
    PCOMBINED_LOCK_ELEMENT Entry;
    PIRP NextMatchingLockIrp;
    LOCK_QUEUE_FILTER Filter = {0};
    PLOCK_INFORMATION InternalInfo = FileLock->LockInformation;
    DPRINT("FsRtlFastUnlockSingle(%wZ, Offset %08x%08x (%d), Length %08x%08x (%d), Key %x)\n",
           &FileObject->FileName,
           FileOffset->HighPart,
           FileOffset->LowPart,
           (int)FileOffset->QuadPart,
           Length->HighPart,
           Length->LowPart,
           (int)Length->QuadPart,
           Key);
    // The region to unlock must correspond exactly to a previously locked region
    // -- msdn
    // But Windows 2003 doesn't assert on it and simply ignores that parameter
    // ASSERT(AlreadySynchronized);
    Find.Exclusive.FileLock.StartingByte = *FileOffset;
    Find.Exclusive.FileLock.EndingByte.QuadPart =
        FileOffset->QuadPart + Length->QuadPart;
    if (!InternalInfo) {
        DPRINT("File not previously locked (ever)\n");
        return STATUS_RANGE_NOT_LOCKED;
    }
    Entry = RtlLookupElementGenericTable(&InternalInfo->RangeTable, &Find);
    if (!Entry) {
        DPRINT("Range not locked %wZ\n", &FileObject->FileName);
        return STATUS_RANGE_NOT_LOCKED;
    }

    DPRINT("Found lock entry: Exclusive %u %08x%08x:%08x%08x %wZ\n",
           Entry->Exclusive.FileLock.ExclusiveLock,
           Entry->Exclusive.FileLock.StartingByte.HighPart,
           Entry->Exclusive.FileLock.StartingByte.LowPart,
           Entry->Exclusive.FileLock.EndingByte.HighPart,
           Entry->Exclusive.FileLock.EndingByte.LowPart,
           &FileObject->FileName);

    if (Entry->Exclusive.FileLock.ExclusiveLock)
    {
        if (Entry->Exclusive.FileLock.Key != Key ||
            Entry->Exclusive.FileLock.ProcessId != Process ||
            Entry->Exclusive.FileLock.FileObject != FileObject ||
            Entry->Exclusive.FileLock.StartingByte.QuadPart != FileOffset->QuadPart ||
            Entry->Exclusive.FileLock.EndingByte.QuadPart !=
            FileOffset->QuadPart + Length->QuadPart)
        {
            DPRINT("Range not locked %wZ\n", &FileObject->FileName);
            return STATUS_RANGE_NOT_LOCKED;
        }
        RtlCopyMemory(&Find, Entry, sizeof(Find));
        // Remove the old exclusive lock region
        RtlDeleteElementGenericTable(&InternalInfo->RangeTable, Entry);
    }
    else
    {
        DPRINT("Shared lock %wZ Start %08x%08x End %08x%08x\n",
               &FileObject->FileName,
               Entry->Exclusive.FileLock.StartingByte.HighPart,
               Entry->Exclusive.FileLock.StartingByte.LowPart,
               Entry->Exclusive.FileLock.EndingByte.HighPart,
               Entry->Exclusive.FileLock.EndingByte.LowPart);
        for (SharedEntry = InternalInfo->SharedLocks.Flink;
             SharedEntry != &InternalInfo->SharedLocks;
             SharedEntry = SharedEntry->Flink)
        {
            SharedRange = CONTAINING_RECORD(SharedEntry, LOCK_SHARED_RANGE, Entry);
            if (SharedRange->Start.QuadPart == FileOffset->QuadPart &&
                SharedRange->End.QuadPart == FileOffset->QuadPart + Length->QuadPart &&
                SharedRange->Key == Key &&
                SharedRange->ProcessId == Process &&
                SharedRange->FileObject == FileObject)
            {
                FoundShared = TRUE;
                DPRINT("Found shared element to delete %wZ Start %08x%08x End %08x%08x Key %x\n",
                       &FileObject->FileName,
                       SharedRange->Start.HighPart,
                       SharedRange->Start.LowPart,
                       SharedRange->End.HighPart,
                       SharedRange->End.LowPart,
                       SharedRange->Key);
                break;
            }
        }
        if (FoundShared)
        {
            /* Remove the found range from the shared range lists */
            RemoveEntryList(&SharedRange->Entry);
            ExFreePoolWithTag(SharedRange, TAG_RANGE);
            /* We need to rebuild the list of shared ranges. */
            DPRINT("Removing the lock entry %wZ (%08x%08x:%08x%08x)\n",
                   &FileObject->FileName,
                   Entry->Exclusive.FileLock.StartingByte.HighPart,
                   Entry->Exclusive.FileLock.StartingByte.LowPart,
                   Entry->Exclusive.FileLock.EndingByte.HighPart,
                   Entry->Exclusive.FileLock.EndingByte.LowPart);

            /* Remember what was in there and remove it from the table */
            Find = *Entry;
            RtlDeleteElementGenericTable(&InternalInfo->RangeTable, &Find);
            /* Put shared locks back in place */
            for (SharedEntry = InternalInfo->SharedLocks.Flink;
                 SharedEntry != &InternalInfo->SharedLocks;
                 SharedEntry = SharedEntry->Flink)
            {
                COMBINED_LOCK_ELEMENT LockElement;
                SharedRange = CONTAINING_RECORD(SharedEntry, LOCK_SHARED_RANGE, Entry);
                LockElement.Exclusive.FileLock.FileObject = SharedRange->FileObject;
                LockElement.Exclusive.FileLock.StartingByte = SharedRange->Start;
                LockElement.Exclusive.FileLock.EndingByte = SharedRange->End;
                LockElement.Exclusive.FileLock.ProcessId = SharedRange->ProcessId;
                LockElement.Exclusive.FileLock.Key = SharedRange->Key;
                LockElement.Exclusive.FileLock.ExclusiveLock = FALSE;

                if (LockCompare(&InternalInfo->RangeTable, &Find, &LockElement) != GenericEqual)
                {
                    DPRINT("Skipping range %08x%08x:%08x%08x\n",
                           LockElement.Exclusive.FileLock.StartingByte.HighPart,
                           LockElement.Exclusive.FileLock.StartingByte.LowPart,
                           LockElement.Exclusive.FileLock.EndingByte.HighPart,
                           LockElement.Exclusive.FileLock.EndingByte.LowPart);
                    continue;
                }
                DPRINT("Re-creating range %08x%08x:%08x%08x\n",
                       LockElement.Exclusive.FileLock.StartingByte.HighPart,
                       LockElement.Exclusive.FileLock.StartingByte.LowPart,
                       LockElement.Exclusive.FileLock.EndingByte.HighPart,
                       LockElement.Exclusive.FileLock.EndingByte.LowPart);
                FsRtlpRebuildSharedLockRange(FileLock, InternalInfo, &LockElement);
            }
        }
        else
        {
            return STATUS_RANGE_NOT_LOCKED;
        }
    }

#ifndef NDEBUG
    DPRINT("Lock still has:\n");
    for (SharedEntry = InternalInfo->SharedLocks.Flink;
         SharedEntry != &InternalInfo->SharedLocks;
         SharedEntry = SharedEntry->Flink)
    {
        SharedRange = CONTAINING_RECORD(SharedEntry, LOCK_SHARED_RANGE, Entry);
        DPRINT("Shared element %wZ Offset %08x%08x Length %08x%08x Key %x\n",
               &FileObject->FileName,
               SharedRange->Start.HighPart,
               SharedRange->Start.LowPart,
               SharedRange->End.HighPart,
               SharedRange->End.LowPart,
               SharedRange->Key);
    }
#endif

    // this is definitely the thing we want
    FileLock->FastIoIsQuestionable = (RtlNumberGenericTableElements(&InternalInfo->RangeTable) != 0);
    InternalInfo->Generation++;
    Filter.Range = &Find;
    while ((NextMatchingLockIrp = IoCsqRemoveNextIrp(&InternalInfo->Csq, &Filter)))
    {
        NTSTATUS Status;
        if (NextMatchingLockIrp->IoStatus.Information == InternalInfo->Generation)
        {
            // We've already looked at this one, meaning that we looped.
            // Put it back and exit.
            IoCsqInsertIrpEx
                (&InternalInfo->Csq,
                 NextMatchingLockIrp,
                 NULL,
                 NULL);
            break;
        }
        // Got a new lock irp... try to do the new lock operation
        // Note that we pick an operation that would succeed at the time
        // we looked, but can't guarantee that it won't just be re-queued
        // because somebody else snatched part of the range in a new thread.
        DPRINT("Locking another IRP %p for %p %wZ\n",
               NextMatchingLockIrp, FileLock, &FileObject->FileName);
        Status = FsRtlProcessFileLock(InternalInfo->BelongsTo, NextMatchingLockIrp, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    DPRINT("Success %wZ\n", &FileObject->FileName);
    return STATUS_SUCCESS;
}

/* Remove only the specified open's locks. Restart a table search after an
 * unlock: shared-range rebuilds and granting waiters can change the table. */
static
NTSTATUS
FsRtlpUnlockAll(PFILE_LOCK FileLock, PFILE_OBJECT FileObject, PEPROCESS Process,
                BOOLEAN MatchKey, ULONG Key, PVOID Context)
{
    PLOCK_INFORMATION Info = FileLock->LockInformation;
    LOCK_QUEUE_FILTER Filter = {0};
    PIRP Irp;
    NTSTATUS Status;

    if (Info == NULL)
        return STATUS_RANGE_NOT_LOCKED;

    Filter.FileObject = FileObject;
    Filter.Process = Process;
    Filter.MatchKey = MatchKey;
    Filter.Key = Key;
    while ((Irp = IoCsqRemoveNextIrp(&Info->Csq, &Filter)) != NULL)
    {
        FsRtlCompleteLockIrpReal(FileLock->CompleteLockIrpRoutine, Context,
                                 Irp, STATUS_CANCELLED, &Status, FileObject);
    }

    for (;;)
    {
        PLIST_ENTRY Entry;
        PLOCK_SHARED_RANGE Range;
        LARGE_INTEGER Start, Length;
        ULONG LockKey = 0;
        BOOLEAN Found = FALSE;

        for (Entry = Info->SharedLocks.Flink; Entry != &Info->SharedLocks; Entry = Entry->Flink)
        {
            Range = CONTAINING_RECORD(Entry, LOCK_SHARED_RANGE, Entry);
            if (Range->FileObject == FileObject && Range->ProcessId == Process &&
                (!MatchKey || Range->Key == Key))
            {
                Start = Range->Start;
                Length.QuadPart = Range->End.QuadPart - Start.QuadPart;
                LockKey = Range->Key;
                Found = TRUE;
                break;
            }
        }
        if (!Found)
            break;
        Status = FsRtlFastUnlockSingle(FileLock, FileObject, &Start, &Length,
                                       Process, LockKey, Context, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    for (;;)
    {
        PCOMBINED_LOCK_ELEMENT Entry;
        FILE_LOCK_INFO LockInfo;
        LARGE_INTEGER Length;
        ULONG i;
        BOOLEAN Found = FALSE;

        for (i = 0; i < RtlNumberGenericTableElements(&Info->RangeTable); ++i)
        {
            Entry = RtlGetElementGenericTable(&Info->RangeTable, i);
            if (Entry->Exclusive.FileLock.ExclusiveLock &&
                Entry->Exclusive.FileLock.FileObject == FileObject &&
                Entry->Exclusive.FileLock.ProcessId == Process &&
                (!MatchKey || Entry->Exclusive.FileLock.Key == Key))
            {
                LockInfo = Entry->Exclusive.FileLock;
                Found = TRUE;
                break;
            }
        }
        if (!Found)
            break;
        Length.QuadPart = LockInfo.EndingByte.QuadPart - LockInfo.StartingByte.QuadPart;
        Status = FsRtlFastUnlockSingle(FileLock, FileObject, &LockInfo.StartingByte,
                                       &Length, Process, LockInfo.Key, Context, TRUE);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlFastUnlockAll(IN PFILE_LOCK FileLock,
                   IN PFILE_OBJECT FileObject,
                   IN PEPROCESS Process,
                   IN PVOID Context OPTIONAL)
{
    return FsRtlpUnlockAll(FileLock, FileObject, Process, FALSE, 0, Context);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlFastUnlockAllByKey(IN PFILE_LOCK FileLock,
                        IN PFILE_OBJECT FileObject,
                        IN PEPROCESS Process,
                        IN ULONG Key,
                        IN PVOID Context OPTIONAL)
{
    return FsRtlpUnlockAll(FileLock, FileObject, Process, TRUE, Key, Context);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
FsRtlProcessFileLock(IN PFILE_LOCK FileLock,
                     IN PIRP Irp,
                     IN PVOID Context OPTIONAL)
{
    PIO_STACK_LOCATION IoStackLocation;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;

    /* Get the I/O Stack location */
    IoStackLocation = IoGetCurrentIrpStackLocation(Irp);
    ASSERT(IoStackLocation->MajorFunction == IRP_MJ_LOCK_CONTROL);

    /* Clear the I/O status block and check what function this is */
    IoStatusBlock.Information = 0;

    DPRINT("FsRtlProcessFileLock(%wZ, MinorFunction %x)\n",
           &IoStackLocation->FileObject->FileName,
           IoStackLocation->MinorFunction);

    switch(IoStackLocation->MinorFunction)
    {
        /* A lock */
    case IRP_MN_LOCK:
    {
        /* Call the private lock routine */
        BOOLEAN Result = FsRtlPrivateLock(FileLock,
                                          IoStackLocation->FileObject,
                                          &IoStackLocation->Parameters.LockControl.ByteOffset,
                                          IoStackLocation->Parameters.LockControl.Length,
                                          IoGetRequestorProcess(Irp),
                                          IoStackLocation->Parameters.LockControl.Key,
                                          IoStackLocation->Flags & SL_FAIL_IMMEDIATELY,
                                          IoStackLocation->Flags & SL_EXCLUSIVE_LOCK,
                                          &IoStatusBlock,
                                          Irp,
                                          Context,
                                          FALSE);
        /* FsRtlPrivateLock has _Must_inspect_result_. Just check this is consistent on debug builds */
        /* FALSE also means queued: STATUS_PENDING is not a granted lock. */
        NT_ASSERT(Result == (IoStatusBlock.Status == STATUS_SUCCESS));
        (void)Result;
        return IoStatusBlock.Status;
    }
        /* A single unlock */
    case IRP_MN_UNLOCK_SINGLE:

        /* Call fast unlock */
        IoStatusBlock.Status =
            FsRtlFastUnlockSingle(FileLock,
                                  IoStackLocation->FileObject,
                                  &IoStackLocation->Parameters.LockControl.
                                  ByteOffset,
                                  IoStackLocation->Parameters.LockControl.
                                  Length,
                                  IoGetRequestorProcess(Irp),
                                  IoStackLocation->Parameters.LockControl.
                                  Key,
                                  Context,
                                  FALSE);
        break;

        /* Total unlock */
    case IRP_MN_UNLOCK_ALL:

        /* Do a fast unlock */
        IoStatusBlock.Status = FsRtlFastUnlockAll(FileLock,
                                                  IoStackLocation->
                                                  FileObject,
                                                  IoGetRequestorProcess(Irp),
                                                  Context);
        break;

        /* Unlock by key */
    case IRP_MN_UNLOCK_ALL_BY_KEY:

        /* Do it */
        IoStatusBlock.Status =
            FsRtlFastUnlockAllByKey(FileLock,
                                    IoStackLocation->FileObject,
                                    IoGetRequestorProcess(Irp),
                                    IoStackLocation->Parameters.
                                    LockControl.Key,
                                    Context);
        break;

        /* Invalid request */
    default:

        /* Complete it */
        FsRtlCompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST);
        IoStatusBlock.Status = STATUS_INVALID_DEVICE_REQUEST;
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* Return the status */
    DPRINT("Lock IRP %p %x\n", Irp, IoStatusBlock.Status);
    FsRtlCompleteLockIrpReal
        (FileLock->CompleteLockIrpRoutine,
         Context,
         Irp,
         IoStatusBlock.Status,
         &Status,
         NULL);
    return IoStatusBlock.Status;
}

/*
 * @implemented
 */
VOID
NTAPI
FsRtlInitializeFileLock (IN PFILE_LOCK FileLock,
                         IN PCOMPLETE_LOCK_IRP_ROUTINE CompleteLockIrpRoutine OPTIONAL,
                         IN PUNLOCK_ROUTINE UnlockRoutine OPTIONAL)
{
    /* Setup the lock */
    RtlZeroMemory(FileLock, sizeof(*FileLock));
    FileLock->FastIoIsQuestionable = FALSE;
    FileLock->CompleteLockIrpRoutine = CompleteLockIrpRoutine;
    FileLock->UnlockRoutine = UnlockRoutine;
    FileLock->LockInformation = NULL;
}

/*
 * @implemented
 */
VOID
NTAPI
FsRtlUninitializeFileLock(IN PFILE_LOCK FileLock)
{
    if (FileLock->LockInformation)
    {
        PIRP Irp;
        PLOCK_INFORMATION InternalInfo = FileLock->LockInformation;
        PCOMBINED_LOCK_ELEMENT Entry;
        PLIST_ENTRY SharedEntry;
        PLOCK_SHARED_RANGE SharedRange;
        // MSDN: this completes any remaining lock IRPs
        for (SharedEntry = InternalInfo->SharedLocks.Flink;
             SharedEntry != &InternalInfo->SharedLocks;)
        {
            SharedRange = CONTAINING_RECORD(SharedEntry, LOCK_SHARED_RANGE, Entry);
            SharedEntry = SharedEntry->Flink;
            RemoveEntryList(&SharedRange->Entry);
            ExFreePoolWithTag(SharedRange, TAG_RANGE);
        }
        while ((Entry = RtlGetElementGenericTable(&InternalInfo->RangeTable, 0)) != NULL)
        {
            RtlDeleteElementGenericTable(&InternalInfo->RangeTable, Entry);
        }
        while ((Irp = IoCsqRemoveNextIrp(&InternalInfo->Csq, NULL)) != NULL)
        {
            NTSTATUS Status;
            FsRtlCompleteLockIrpReal(FileLock->CompleteLockIrpRoutine, NULL,
                                     Irp, STATUS_CANCELLED, &Status,
                                     IoGetCurrentIrpStackLocation(Irp)->FileObject);
        }
        ExFreePoolWithTag(InternalInfo, TAG_FLOCK);
        FileLock->LockInformation = NULL;
    }
    FileLock->FastIoIsQuestionable = FALSE;
}

/*
 * @implemented
 */
PFILE_LOCK
NTAPI
FsRtlAllocateFileLock(IN PCOMPLETE_LOCK_IRP_ROUTINE CompleteLockIrpRoutine OPTIONAL,
                      IN PUNLOCK_ROUTINE UnlockRoutine OPTIONAL)
{
    PFILE_LOCK FileLock;

    /* Try to allocate it */
    FileLock = ExAllocateFromPagedLookasideList(&FsRtlFileLockLookasideList);
    if (FileLock)
    {
        /* Initialize it */
        FsRtlInitializeFileLock(FileLock,
                                CompleteLockIrpRoutine,
                                UnlockRoutine);
    }

    /* Return the lock */
    return FileLock;
}

/*
 * @implemented
 */
VOID
NTAPI
FsRtlFreeFileLock(IN PFILE_LOCK FileLock)
{
    /* Uninitialize and free the lock */
    FsRtlUninitializeFileLock(FileLock);
    ExFreeToPagedLookasideList(&FsRtlFileLockLookasideList, FileLock);
}

/*
 * @implemented
 *
 * Returns TRUE iff there are byte-range lock requests still being processed
 * on the FILE_LOCK (granted but not yet completed, or queued waiters). This
 * mirrors the LockRequestsInProgress counter that FsRtlPrivateLock /
 * FsRtlIncrement|DecrementLockRequestsInProgress maintain.
 */
BOOLEAN
NTAPI
FsRtlAreThereWaitingFileLocks(IN PFILE_LOCK FileLock)
{
    return (FileLock->LockRequestsInProgress > 0);
}

/*
 * @implemented
 *
 * Pre-oplock-grant safety check: returns TRUE if it is safe for the file
 * system to grant a new oplock based on current byte-range lock state.
 * Conservative-correct rule: if any lock is currently held anywhere on the
 * file (FastIoIsQuestionable), refuse. AllocationSize would let us narrow
 * the scan to in-range locks only; that requires walking the FsRtl-private
 * lock tree which the public API doesn't expose, so we keep the conservative
 * behaviour until that walk lands.
 */
BOOLEAN
NTAPI
FsRtlCheckLockForOplockRequest(IN PFILE_LOCK FileLock,
                               IN PLARGE_INTEGER AllocationSize)
{
    UNREFERENCED_PARAMETER(AllocationSize);
    return !FsRtlAreThereCurrentFileLocks(FileLock);
}
