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

/* Keep every acquisition separately. Overlap is not an equivalence relation:
 * merging shared intervals loses both their owners and exact unlock ranges. */
typedef struct _COMBINED_LOCK_ELEMENT
{
    FILE_EXCLUSIVE_LOCK_ENTRY Exclusive;
    ULONGLONG Sequence;
} COMBINED_LOCK_ELEMENT, *PCOMBINED_LOCK_ELEMENT;

typedef struct _LOCK_INFORMATION
{
    RTL_GENERIC_TABLE RangeTable;
    IO_CSQ Csq;
    KSPIN_LOCK CsqLock;
    LIST_ENTRY CsqList;
    PCOMPLETE_LOCK_IRP_ROUTINE CompleteRoutine;
    LONG References;
    ULONGLONG NextSequence;
    ULONG Generation;
} LOCK_INFORMATION, *PLOCK_INFORMATION;

/* One reference belongs to FILE_LOCK, another to every queued or dequeued
 * request until its completion/reattempt returns. Cancellation can complete
 * outside the queue lock while the filesystem uninitializes the FILE_LOCK. */
typedef struct _PENDING_FILE_LOCK
{
    PLOCK_INFORMATION Info;
    FILE_LOCK_INFO Request;
    PVOID Context;
    PVOID SavedDriverContext;
    ULONG Generation;
    WORK_QUEUE_ITEM WorkItem;
    PIRP Irp;
    NTSTATUS Status;
} PENDING_FILE_LOCK, *PPENDING_FILE_LOCK;

static VOID
LockDereferenceInfo(PLOCK_INFORMATION Info)
{
    ASSERT(Info->References > 0);
    if (InterlockedDecrement(&Info->References) == 0)
        ExFreePoolWithTag(Info, TAG_FLOCK);
}

static PPENDING_FILE_LOCK
LockTakePending(PIRP Irp)
{
    PPENDING_FILE_LOCK Pending = Irp->Tail.Overlay.DriverContext[0];

    ASSERT(Pending != NULL);
    Irp->Tail.Overlay.DriverContext[0] = Pending->SavedDriverContext;
    return Pending;
}

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

static VOID
LockWakeWaiters(PFILE_LOCK FileLock, PLOCK_INFORMATION Info, PCOMBINED_LOCK_ELEMENT Range);

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

static RTL_GENERIC_COMPARE_RESULTS NTAPI
LockCompare(PRTL_GENERIC_TABLE Table, PVOID PtrA, PVOID PtrB)
{
    PCOMBINED_LOCK_ELEMENT A = PtrA, B = PtrB;

    UNREFERENCED_PARAMETER(Table);
    return A->Sequence < B->Sequence ? GenericLessThan :
           A->Sequence > B->Sequence ? GenericGreaterThan : GenericEqual;
}

/* MS-FSA 2.1.4.10 uses unsigned, inclusive last-byte offsets. A zero-length
 * range at zero never overlaps; at another offset it can be contained by a
 * range spanning the bytes immediately before and after that offset. */
static BOOLEAN
LockRangesOverlap(PFILE_LOCK_INFO A, PFILE_LOCK_INFO B)
{
    ULONGLONG StartA = A->StartingByte.QuadPart, StartB = B->StartingByte.QuadPart;
    ULONGLONG LengthA = A->Length.QuadPart, LengthB = B->Length.QuadPart;

    if ((StartA == 0 && LengthA == 0) || (StartB == 0 && LengthB == 0))
        return FALSE;
    return StartA <= StartB + LengthB - 1 && StartA + LengthA - 1 >= StartB;
}

static BOOLEAN
LockConflicts(PLOCK_INFORMATION Info, PFILE_LOCK_INFO Request, BOOLEAN LockIntent)
{
    PCOMBINED_LOCK_ELEMENT Entry;
    PFILE_LOCK_INFO Existing;
    ULONG i;

    for (i = 0; i < RtlNumberGenericTableElements(&Info->RangeTable); ++i)
    {
        Entry = RtlGetElementGenericTable(&Info->RangeTable, i);
        Existing = &Entry->Exclusive.FileLock;
        if (!LockRangesOverlap(Request, Existing))
            continue;
        if (Existing->ExclusiveLock)
        {
            if (Existing->FileObject != Request->FileObject ||
                Existing->ProcessId != Request->ProcessId || Existing->Key != Request->Key ||
                (Request->ExclusiveLock && LockIntent))
                return TRUE;
        }
        else if (Request->ExclusiveLock)
            return TRUE;
    }
    return FALSE;
}

/* CSQ methods */

static NTSTATUS NTAPI LockInsertIrpEx
(PIO_CSQ Csq,
 PIRP Irp,
 PVOID InsertContext)
{
    PLOCK_INFORMATION LockInfo = CONTAINING_RECORD(Csq, LOCK_INFORMATION, Csq);
    PPENDING_FILE_LOCK Pending = InsertContext;

    ASSERT(Pending != NULL && Pending->Info == LockInfo);
    InterlockedIncrement(&LockInfo->References);
    Irp->Tail.Overlay.DriverContext[0] = Pending;
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
        PPENDING_FILE_LOCK Pending;

        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (Filter == NULL)
            return Irp;
        Pending = Irp->Tail.Overlay.DriverContext[0];
        if (Filter->FileObject != NULL)
        {
            if (Pending->Request.FileObject == Filter->FileObject &&
                Pending->Request.ProcessId == Filter->Process &&
                (!Filter->MatchKey || Pending->Request.Key == Filter->Key))
                return Irp;
        }
        else
        {
            if (LockRangesOverlap(&Pending->Request, &Filter->Range->Exclusive.FileLock))
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
LockCompletePendingWorker(PVOID Parameter)
{
    PPENDING_FILE_LOCK Pending = Parameter;
    PLOCK_INFORMATION Info = Pending->Info;
    NTSTATUS Status;

    FsRtlCompleteLockIrpReal(Info->CompleteRoutine, Pending->Context, Pending->Irp,
                             Pending->Status, &Status, NULL);
    ExFreePoolWithTag(Pending, TAG_FLOCK);
    LockDereferenceInfo(Info);
}

static VOID
LockCompletePending(PIRP Irp, NTSTATUS Status)
{
    PPENDING_FILE_LOCK Pending = LockTakePending(Irp);

    Pending->Irp = Irp;
    Pending->Status = Status;
    /* The CSQ can invoke cancellation with its spin lock held. The file-lock
     * completion callback runs at <= APC_LEVEL and may close a file object. */
    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        ExInitializeWorkItem(&Pending->WorkItem, LockCompletePendingWorker, Pending);
        ExQueueWorkItem(&Pending->WorkItem, DelayedWorkQueue);
    }
    else
    {
        LockCompletePendingWorker(Pending);
    }
}

static VOID NTAPI
LockCompleteCanceledIrp(PIO_CSQ Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    LockCompletePending(Irp, STATUS_CANCELLED);
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
    PLOCK_INFORMATION LockInfo;
    COMBINED_LOCK_ELEMENT ToInsert = {0};
    PCOMBINED_LOCK_ELEMENT Entry;
    PFILE_LOCK_INFO Request = &ToInsert.Exclusive.FileLock;
    BOOLEAN InsertedNew, Granted = FALSE;
    NTSTATUS Status;
    ULONGLONG Start = FileOffset->QuadPart, Size = Length->QuadPart;

    UNREFERENCED_PARAMETER(AlreadySynchronized);
    IoStatus->Information = 0;
    if (Size != 0 && Start + Size - 1 < Start)
    {
        IoStatus->Status = STATUS_INVALID_LOCK_RANGE;
        goto Complete;
    }
    if (FileLock->LockInformation == NULL)
    {
        LockInfo = ExAllocatePoolWithTag(NonPagedPool, sizeof(*LockInfo), TAG_FLOCK);
        if (LockInfo == NULL)
        {
            IoStatus->Status = STATUS_NO_MEMORY;
            goto Complete;
        }
        RtlZeroMemory(LockInfo, sizeof(*LockInfo));
        LockInfo->CompleteRoutine = FileLock->CompleteLockIrpRoutine;
        LockInfo->References = 1;
        RtlInitializeGenericTable(&LockInfo->RangeTable, LockCompare, LockAllocate, LockFree, NULL);
        KeInitializeSpinLock(&LockInfo->CsqLock);
        InitializeListHead(&LockInfo->CsqList);
        IoCsqInitializeEx(&LockInfo->Csq, LockInsertIrpEx, LockRemoveIrp, LockPeekNextIrp,
                         LockAcquireQueueLock, LockReleaseQueueLock, LockCompleteCanceledIrp);
        FileLock->LockInformation = LockInfo;
    }
    LockInfo = FileLock->LockInformation;
    Request->FileObject = FileObject;
    Request->StartingByte = *FileOffset;
    Request->Length = *Length;
    Request->EndingByte.QuadPart = Start + Size - 1;
    Request->ProcessId = Process;
    Request->Key = Key;
    Request->ExclusiveLock = ExclusiveLock;
    if (LockConflicts(LockInfo, Request, TRUE))
    {
        if (FailImmediately)
        {
            IoStatus->Status = STATUS_LOCK_NOT_GRANTED;
            goto Complete;
        }
        IoStatus->Status = STATUS_PENDING;
        if (Irp != NULL)
        {
            PPENDING_FILE_LOCK Pending;

            Pending = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Pending), TAG_FLOCK);
            if (Pending == NULL)
            {
                IoStatus->Status = STATUS_NO_MEMORY;
                goto Complete;
            }
            Pending->Info = LockInfo;
            Pending->Request = *Request;
            Pending->Context = Context;
            Pending->SavedDriverContext = Irp->Tail.Overlay.DriverContext[0];
            Pending->Generation = LockInfo->Generation;
            IoCsqInsertIrpEx(&LockInfo->Csq, Irp, NULL, Pending);
        }
        return FALSE;
    }
    ToInsert.Sequence = ++LockInfo->NextSequence;
    ASSERT(ToInsert.Sequence != 0);
    Entry = RtlInsertElementGenericTable(&LockInfo->RangeTable, &ToInsert,
                                        sizeof(ToInsert), &InsertedNew);
    if (Entry == NULL)
    {
        IoStatus->Status = STATUS_NO_MEMORY;
        goto Complete;
    }
    ASSERT(InsertedNew);
    Granted = TRUE;
    FileLock->FastIoIsQuestionable = TRUE;
    IoStatus->Status = STATUS_SUCCESS;
Complete:
    if (Irp != NULL)
    {
        FsRtlCompleteLockIrpReal(FileLock->CompleteLockIrpRoutine, Context, Irp,
                                 IoStatus->Status, &Status, FileObject);
        IoStatus->Status = Status;
        if (Granted && !NT_SUCCESS(Status) && FileLock->LockInformation == LockInfo &&
            RtlDeleteElementGenericTable(&LockInfo->RangeTable, &ToInsert))
        {
            /* Reject only this acquisition, preserving older identical shared
             * ranges and their ownership. The callback has completed the IRP. */
            LockWakeWaiters(FileLock, LockInfo, &ToInsert);
        }
    }
    return NT_SUCCESS(IoStatus->Status) && IoStatus->Status != STATUS_PENDING;
}

static BOOLEAN
FsRtlpCheckLockAccess(PFILE_LOCK FileLock, PLARGE_INTEGER Offset, PLARGE_INTEGER Length,
                      ULONG Key, PFILE_OBJECT FileObject, PVOID Process, BOOLEAN Write)
{
    PLOCK_INFORMATION Info = FileLock->LockInformation;
    FILE_LOCK_INFO Request = {0};

    if (Info == NULL)
        return TRUE;
    Request.StartingByte = *Offset;
    Request.Length = *Length;
    Request.FileObject = FileObject;
    Request.ProcessId = Process;
    Request.Key = Key;
    Request.ExclusiveLock = Write;
    return !LockConflicts(Info, &Request, FALSE);
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

static VOID
LockWakeWaiters(PFILE_LOCK FileLock, PLOCK_INFORMATION Info, PCOMBINED_LOCK_ELEMENT Range)
{
    LOCK_QUEUE_FILTER Filter = {0};
    PIRP NextMatchingLockIrp;

    FileLock->FastIoIsQuestionable = (RtlNumberGenericTableElements(&Info->RangeTable) != 0);
    Info->Generation++;
    Filter.Range = Range;
    while ((NextMatchingLockIrp = IoCsqRemoveNextIrp(&Info->Csq, &Filter)))
    {
        PPENDING_FILE_LOCK Pending = LockTakePending(NextMatchingLockIrp);
        IO_STATUS_BLOCK IoStatus;
        BOOLEAN Granted;

        if (Pending->Generation == Info->Generation)
        {
            /* All matching requests have been attempted in this pass. */
            IoCsqInsertIrpEx(&Info->Csq, NextMatchingLockIrp, NULL, Pending);
            LockDereferenceInfo(Info);
            break;
        }
        Granted = FsRtlPrivateLock(FileLock, Pending->Request.FileObject,
                                   &Pending->Request.StartingByte, &Pending->Request.Length,
                                   Pending->Request.ProcessId, Pending->Request.Key, FALSE,
                                   Pending->Request.ExclusiveLock, &IoStatus,
                                   NextMatchingLockIrp, Pending->Context, TRUE);
        ASSERT(Granted == (NT_SUCCESS(IoStatus.Status) && IoStatus.Status != STATUS_PENDING));
        (void)Granted;
        ExFreePoolWithTag(Pending, TAG_FLOCK);
        LockDereferenceInfo(Info);
    }

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
    PLOCK_INFORMATION InternalInfo = FileLock->LockInformation;
    PCOMBINED_LOCK_ELEMENT Entry;
    COMBINED_LOCK_ELEMENT Find;
    PFILE_LOCK_INFO Lock;
    ULONG i;
    BOOLEAN Found = FALSE;

    UNREFERENCED_PARAMETER(AlreadySynchronized);
    if ((ULONGLONG)Length->QuadPart != 0 &&
        (ULONGLONG)FileOffset->QuadPart + (ULONGLONG)Length->QuadPart - 1 < (ULONGLONG)FileOffset->QuadPart)
        return STATUS_INVALID_LOCK_RANGE;
    if (InternalInfo == NULL)
        return STATUS_RANGE_NOT_LOCKED;
    for (i = 0; i < RtlNumberGenericTableElements(&InternalInfo->RangeTable); ++i)
    {
        Entry = RtlGetElementGenericTable(&InternalInfo->RangeTable, i);
        Lock = &Entry->Exclusive.FileLock;
        if (Lock->FileObject == FileObject && Lock->ProcessId == Process && Lock->Key == Key &&
            Lock->StartingByte.QuadPart == FileOffset->QuadPart && Lock->Length.QuadPart == Length->QuadPart)
        {
            Find = *Entry;
            Found = TRUE;
            if (Lock->ExclusiveLock)
                break;
        }
    }
    if (!Found)
        return STATUS_RANGE_NOT_LOCKED;
    Found = RtlDeleteElementGenericTable(&InternalInfo->RangeTable, &Find);
    ASSERT(Found);
    FileObject->LastLock = NULL;
    if (FileLock->UnlockRoutine != NULL)
        FileLock->UnlockRoutine(Context, &Find.Exclusive.FileLock);
    LockWakeWaiters(FileLock, InternalInfo, &Find);

    DPRINT("Success %wZ\n", &FileObject->FileName);
    return STATUS_SUCCESS;
}



/* Cancel this open's waiters before releasing its granted locks. */
static NTSTATUS
FsRtlpUnlockAll(PFILE_LOCK FileLock, PFILE_OBJECT FileObject, PEPROCESS Process,
                BOOLEAN MatchKey, ULONG Key, PVOID Context)
{
    PLOCK_INFORMATION Info = FileLock->LockInformation;
    LOCK_QUEUE_FILTER Filter = {0};
    PCOMBINED_LOCK_ELEMENT Entry;
    FILE_LOCK_INFO LockInfo;
    PIRP Irp;
    NTSTATUS Status;
    ULONG i;
    BOOLEAN Found;

    if (Info == NULL)
        return STATUS_RANGE_NOT_LOCKED;
    Filter.FileObject = FileObject;
    Filter.Process = Process;
    Filter.MatchKey = MatchKey;
    Filter.Key = Key;
    while ((Irp = IoCsqRemoveNextIrp(&Info->Csq, &Filter)) != NULL)
        LockCompletePending(Irp, STATUS_CANCELLED);
    for (;;)
    {
        Found = FALSE;
        for (i = 0; i < RtlNumberGenericTableElements(&Info->RangeTable); ++i)
        {
            Entry = RtlGetElementGenericTable(&Info->RangeTable, i);
            if (Entry->Exclusive.FileLock.FileObject == FileObject &&
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
        Status = FsRtlFastUnlockSingle(FileLock, FileObject, &LockInfo.StartingByte,
                                       &LockInfo.Length, Process, LockInfo.Key, Context, TRUE);
        ASSERT(NT_SUCCESS(Status));
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
        NT_ASSERT(Result == (NT_SUCCESS(IoStatusBlock.Status) && IoStatusBlock.Status != STATUS_PENDING));
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

        FileLock->LockInformation = NULL;
        while ((Entry = RtlGetElementGenericTable(&InternalInfo->RangeTable, 0)) != NULL)
        {
            RtlDeleteElementGenericTable(&InternalInfo->RangeTable, Entry);
        }
        while ((Irp = IoCsqRemoveNextIrp(&InternalInfo->Csq, NULL)) != NULL)
            LockCompletePending(Irp, STATUS_CANCELLED);
        LockDereferenceInfo(InternalInfo);
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
