/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/fsrtl/notify.c
 * PURPOSE:         Change Notifications and Sync for File System Drivers
 * PROGRAMMERS:     Pierre Schweitzer
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* Completion callbacks run after dropping the notify mutex. In particular,
 * a completion can close its file object and reenter notification cleanup. */
typedef struct _NOTIFY_IRP_CONTEXT
{
    WORK_QUEUE_ITEM WorkItem;
    PNOTIFY_CHANGE Change;
    PIRP Irp;
    PVOID SavedDriverContext;
} NOTIFY_IRP_CONTEXT, *PNOTIFY_IRP_CONTEXT;

static VOID
FsRtlNotifyDereferenceSync(PREAL_NOTIFY_SYNC Sync)
{
    ASSERT(Sync->References > 0);
    if (InterlockedDecrement(&Sync->References) == 0)
        ExFreePoolWithTag(Sync, 'SNSF');
}

static __inline VOID
FsRtlNotifyAcquireFastMutex(PREAL_NOTIFY_SYNC Sync)
{
    ULONG_PTR Thread = (ULONG_PTR)KeGetCurrentThread();

    InterlockedIncrement(&Sync->References);
    if (Sync->OwningThread != Thread)
    {
        ExAcquireFastMutexUnsafe(&Sync->FastMutex);
        Sync->OwningThread = Thread;
    }
    Sync->OwnerCount++;
}

static __inline VOID
FsRtlNotifyReleaseFastMutex(PREAL_NOTIFY_SYNC Sync)
{
    LIST_ENTRY Completions;
    PIRP Irp;

    ASSERT(Sync->OwnerCount != 0 && Sync->OwningThread == (ULONG_PTR)KeGetCurrentThread());
    InitializeListHead(&Completions);
    if (--Sync->OwnerCount == 0)
    {
        if (!IsListEmpty(&Sync->Completions))
        {
            Completions = Sync->Completions;
            Completions.Flink->Blink = &Completions;
            Completions.Blink->Flink = &Completions;
            InitializeListHead(&Sync->Completions);
        }
        Sync->OwningThread = 0;
        ExReleaseFastMutexUnsafe(&Sync->FastMutex);
        while (!IsListEmpty(&Completions))
        {
            Irp = CONTAINING_RECORD(RemoveHeadList(&Completions), IRP, Tail.Overlay.ListEntry);
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
        }
    }
    FsRtlNotifyDereferenceSync(Sync);
}

static VOID
FsRtlNotifyQueueCompletion(PNOTIFY_SYNC NotifySync, PIRP Irp)
{
    PREAL_NOTIFY_SYNC Sync = NotifySync;

    ASSERT(Sync->OwnerCount != 0 && Sync->OwningThread == (ULONG_PTR)KeGetCurrentThread());
    InsertTailList(&Sync->Completions, &Irp->Tail.Overlay.ListEntry);
}

/* The list owns one reference, each pending IRP another. Cleanup detaches
 * the list reference; an in-flight cancel retains the entry and its sync. */
static VOID
FsRtlNotifyDereference(PNOTIFY_CHANGE Change,
                       PSECURITY_SUBJECT_CONTEXT *Subject,
                       PEPROCESS *Process)
{
    PREAL_NOTIFY_SYNC Sync;

    ASSERT(Change->ReferenceCount > 0);
    if (InterlockedDecrement((PLONG)&Change->ReferenceCount) != 0)
        return;
    ASSERT(Change->NotifyList.Flink == NULL && IsListEmpty(&Change->NotifyIrps));
    if (Change->AllocatedBuffer != NULL)
    {
        PsReturnProcessPagedPoolQuota(Change->OwningProcess, Change->ThisBufferLength);
        ExFreePoolWithTag(Change->AllocatedBuffer, TAG_FS_NOTIFICATIONS);
    }
    *Subject = Change->SubjectContext;
    *Process = Change->OwningProcess;
    Sync = Change->NotifySync;
    ExFreePoolWithTag(Change, 'NrSF');
    FsRtlNotifyDereferenceSync(Sync);
}

#define FsRtlNotifyGetLastPartOffset(FullLen, TargLen, Type, Chr)                 \
    for (FullPosition = 0; FullPosition < FullLen; ++FullPosition)                \
        if (((Type)NotifyChange->FullDirectoryName->Buffer)[FullPosition] == Chr) \
            ++FullNumberOfParts;                                                  \
    for (LastPartOffset = 0; LastPartOffset < TargLen; ++LastPartOffset) {        \
        if ( ((Type)TargetDirectory.Buffer)[LastPartOffset] == Chr) {             \
            ++TargetNumberOfParts;                                                \
        if (TargetNumberOfParts == FullNumberOfParts)                             \
            break;                                                                \
        }                                                                         \
    }

/* PRIVATE FUNCTIONS *********************************************************/

VOID
FsRtlNotifyCompleteIrpList(IN PNOTIFY_CHANGE NotifyChange,
                           IN NTSTATUS Status);

BOOLEAN
FsRtlNotifySetCancelRoutine(IN PIRP Irp,
                            IN PNOTIFY_CHANGE NotifyChange OPTIONAL);

/*
 * @implemented
 */
static VOID NTAPI
FsRtlCancelNotifyWorker(PVOID Parameter)
{
    PNOTIFY_IRP_CONTEXT Pending = Parameter;
    PIRP Irp = Pending->Irp;
    PNOTIFY_CHANGE NotifyChange = Pending->Change;
    PREAL_NOTIFY_SYNC RealNotifySync = NotifyChange->NotifySync;
    PVOID Buffer;
    PIRP NotifyIrp;
    ULONG BufferLength;
    PIO_STACK_LOCATION Stack;
    BOOLEAN PoolQuotaCharged;
    PSECURITY_SUBJECT_CONTEXT _SEH2_VOLATILE SubjectContext = NULL;
    PEPROCESS _SEH2_VOLATILE Process = NULL;

    Irp->Tail.Overlay.DriverContext[0] = Pending->SavedDriverContext;
    Irp->IoStatus.Information = 0;
    ExFreePoolWithTag(Pending, TAG_FS_NOTIFICATIONS);
    KeEnterCriticalRegion();
    FsRtlNotifyAcquireFastMutex(RealNotifySync);

   _SEH2_TRY
   {
       /* Remove the IRP from the notifications list and mark it pending */
       RemoveEntryList(&(Irp->Tail.Overlay.ListEntry));
       IoMarkIrpPending(Irp);

       /* Now, the tricky part - let's find a buffer big enough to hold the return data */
       if (NotifyChange->Buffer && NotifyChange->AllocatedBuffer == NULL &&
           ((Irp->MdlAddress && MmGetSystemAddressForMdl(Irp->MdlAddress) == NotifyChange->Buffer) ||
           NotifyChange->Buffer == Irp->AssociatedIrp.SystemBuffer))
       {
           /* Assume we didn't find any */
           Buffer = NULL;
           BufferLength = 0;

           /* If we don't have IRPs, check if current buffer is big enough */
           if (IsListEmpty(&NotifyChange->NotifyIrps))
           {
               if (NotifyChange->BufferLength >= NotifyChange->DataLength)
               {
                   BufferLength = NotifyChange->BufferLength;
               }
           }
           else
           {
               /* Otherwise, try to look at next IRP available */
               NotifyIrp = CONTAINING_RECORD(NotifyChange->NotifyIrps.Flink, IRP, Tail.Overlay.ListEntry);
               Stack = IoGetCurrentIrpStackLocation(NotifyIrp);

               /* If its buffer is big enough, get it */
               if (Stack->Parameters.NotifyDirectory.Length >= NotifyChange->BufferLength)
               {
                   /* Is it MDL? */
                   if (NotifyIrp->AssociatedIrp.SystemBuffer == NULL)
                   {
                       if (NotifyIrp->MdlAddress != NULL)
                       {
                           Buffer = MmGetSystemAddressForMdl(NotifyIrp->MdlAddress);
                       }
                   }
                   else
                   {
                       Buffer = NotifyIrp->AssociatedIrp.MasterIrp;
                   }

                   /* Backup our accepted buffer length */
                   BufferLength = Stack->Parameters.NotifyDirectory.Length;
                   if (BufferLength > NotifyChange->BufferLength)
                   {
                       BufferLength = NotifyChange->BufferLength;
                   }
               }
           }

           /* At that point, we *may* have a buffer */

           /* If it has null length, then note that we won't use it */
           if (BufferLength == 0)
           {
               NotifyChange->Flags |= NOTIFY_IMMEDIATELY;
           }
           else
           {
               /* If we have a buffer length, but no buffer then allocate one */
               if (Buffer == NULL)
               {
                   /* Assume we haven't charged quotas */
                   PoolQuotaCharged = FALSE;

                   _SEH2_TRY
                   {
                       /* Charge quotas */
                       PsChargePoolQuota(NotifyChange->OwningProcess, PagedPool, BufferLength);
                       PoolQuotaCharged = TRUE;

                       /* Allocate buffer */
                       Buffer = ExAllocatePoolWithTag(PagedPool | POOL_RAISE_IF_ALLOCATION_FAILURE, BufferLength, TAG_FS_NOTIFICATIONS);
                       NotifyChange->AllocatedBuffer = Buffer;

                       /* Copy data in that buffer */
                       RtlCopyMemory(Buffer, NotifyChange->Buffer, NotifyChange->DataLength);
                       NotifyChange->ThisBufferLength = BufferLength;
                       NotifyChange->Buffer = Buffer;
                   }
                   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                   {
                       /* Something went wrong, have we charged quotas? */
                       if (PoolQuotaCharged)
                       {
                           /* We did, return quotas */
                           PsReturnProcessPagedPoolQuota(NotifyChange->OwningProcess, BufferLength);
                       }

                       /* And notify immediately */
                       NotifyChange->Flags |= NOTIFY_IMMEDIATELY;
                   }
                   _SEH2_END;
               }
               else
               {
                   RtlMoveMemory(Buffer, NotifyChange->Buffer, NotifyChange->DataLength);
                   NotifyChange->Buffer = Buffer;
               }
           }

           /* If we have to notify immediately, ensure that any buffer is 0-ed out */
           if (NotifyChange->Flags & NOTIFY_IMMEDIATELY)
           {
               NotifyChange->Buffer = 0;
               NotifyChange->AllocatedBuffer = 0;
               NotifyChange->LastEntry = 0;
               NotifyChange->DataLength = 0;
               NotifyChange->ThisBufferLength = 0;
           }
       }

       /* It's now time to complete - data are ready */

       /* Set appropriate status and complete */
       Irp->IoStatus.Status = STATUS_CANCELLED;
       FsRtlNotifyQueueCompletion(NotifyChange->NotifySync, Irp);

       FsRtlNotifyDereference(NotifyChange, (PSECURITY_SUBJECT_CONTEXT *)&SubjectContext,
                               (PEPROCESS *)&Process);
    }
    _SEH2_FINALLY
    {
        FsRtlNotifyReleaseFastMutex(RealNotifySync);

        if (Process != NULL)
            ObDereferenceObject(Process);
        /* If the subject security context was captured, release and free it */
        if (SubjectContext)
        {
            SeReleaseSubjectContext(SubjectContext);
            ExFreePool(SubjectContext);
        }
        KeLeaveCriticalRegion();
    }
    _SEH2_END;
}

VOID NTAPI
FsRtlCancelNotify(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PNOTIFY_IRP_CONTEXT Pending = Irp->Tail.Overlay.DriverContext[0];

    UNREFERENCED_PARAMETER(DeviceObject);
    ASSERT(Pending != NULL && Pending->Irp == Irp);
    IoSetCancelRoutine(Irp, NULL);
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        ExInitializeWorkItem(&Pending->WorkItem, FsRtlCancelNotifyWorker, Pending);
        ExQueueWorkItem(&Pending->WorkItem, DelayedWorkQueue);
    }
    else
    {
        FsRtlCancelNotifyWorker(Pending);
    }
}

/*
 * @implemented
 */
VOID
FsRtlCheckNotifyForDelete(IN PLIST_ENTRY NotifyList,
                          IN PVOID FsContext)
{
    PLIST_ENTRY Entry;
    PNOTIFY_CHANGE Change;

    for (Entry = NotifyList->Flink; Entry != NotifyList; Entry = Entry->Flink)
    {
        Change = CONTAINING_RECORD(Entry, NOTIFY_CHANGE, NotifyList);
        if (Change->StreamID == FsContext)
        {
            Change->Flags |= DELETE_IN_PROCESS;
            FsRtlNotifyCompleteIrpList(Change, STATUS_DELETE_PENDING);
        }
    }
}

/*
 *@implemented
 */
PNOTIFY_CHANGE
FsRtlIsNotifyOnList(IN PLIST_ENTRY NotifyList,
                    IN PVOID FsContext)
{
    PLIST_ENTRY NextEntry;
    PNOTIFY_CHANGE NotifyChange;

    if (!IsListEmpty(NotifyList))
    {
        /* Browse the notifications list to find the matching entry */
        for (NextEntry = NotifyList->Flink;
             NextEntry != NotifyList;
             NextEntry = NextEntry->Flink)
        {
            NotifyChange = CONTAINING_RECORD(NextEntry, NOTIFY_CHANGE, NotifyList);
            /* If the current record matches with the given context, it's the good one */
            if (NotifyChange->FsContext == FsContext)
            {
                return NotifyChange;
            }
        }
    }
    return NULL;
}

/*
 * @implemented
 */
VOID
FsRtlNotifyCompleteIrp(IN PIRP Irp,
                       IN PNOTIFY_CHANGE NotifyChange,
                       IN ULONG DataLength,
                       IN NTSTATUS Status,
                       IN BOOLEAN SkipCompletion)
{
    PVOID Buffer;
    PIO_STACK_LOCATION Stack;

    PAGED_CODE();

    /* Check if we need to complete */
    if (SkipCompletion && !FsRtlNotifySetCancelRoutine(Irp, NotifyChange))
    {
        return;
    }

    /* No succes => no data to return just complete */
    if (Status != STATUS_SUCCESS)
    {
        goto Completion;
    }

    /* Ensure there's something to return */
    Stack = IoGetCurrentIrpStackLocation(Irp);
    if (!DataLength || Stack->Parameters.NotifyDirectory.Length < DataLength)
    {
        Status = STATUS_NOTIFY_ENUM_DIR;
        goto Completion;
    }

    /* Ensture there's a buffer where to find data */
    if (!NotifyChange->AllocatedBuffer)
    {
        /* The shared direct buffer can belong to a different pending IRP,
         * notably one whose cancellation already owns its cancel routine. */
        if (Irp->AssociatedIrp.SystemBuffer != NULL)
            Buffer = Irp->AssociatedIrp.SystemBuffer;
        else if (Irp->MdlAddress != NULL)
            Buffer = MmGetSystemAddressForMdl(Irp->MdlAddress);
        else if (!(Stack->Control & SL_PENDING_RETURNED))
            Buffer = Irp->UserBuffer;
        else
        {
            Buffer = ExAllocatePoolWithTag(PagedPool, DataLength, TAG_FS_NOTIFICATIONS);
            if (Buffer != NULL)
            {
                Irp->AssociatedIrp.SystemBuffer = Buffer;
                Irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER | IRP_SYNCHRONOUS_PAGING_IO;
            }
        }
        if (Buffer == NULL)
        {
            Status = STATUS_NOTIFY_ENUM_DIR;
            Irp->IoStatus.Information = 0;
        }
        else
        {
            if (Buffer != NotifyChange->Buffer)
                RtlCopyMemory(Buffer, NotifyChange->Buffer, DataLength);
            Irp->IoStatus.Information = DataLength;
        }
        NotifyChange->Buffer = NULL;
        goto Completion;
    }

    /* Now, browse all the way to return data
     * and find the one that will work. We will
     * return data whatever happens
     */
    if (Irp->AssociatedIrp.SystemBuffer)
    {
        Buffer = Irp->AssociatedIrp.SystemBuffer;
        goto CopyAndComplete;
    }

    if (Irp->MdlAddress)
    {
        Buffer = MmGetSystemAddressForMdl(Irp->MdlAddress);
        goto CopyAndComplete;
    }

    if (!(Stack->Control & SL_PENDING_RETURNED))
    {
        Buffer = Irp->UserBuffer;
        goto CopyAndComplete;
    }

    Irp->Flags |= (IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER | IRP_SYNCHRONOUS_PAGING_IO);
    Irp->AssociatedIrp.SystemBuffer = NotifyChange->AllocatedBuffer;
    /* Nothing to copy */
    goto ReleaseAndComplete;

CopyAndComplete:
    _SEH2_TRY
    {
        RtlCopyMemory(Buffer, NotifyChange->AllocatedBuffer, DataLength);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* Do nothing */
    }
    _SEH2_END;

ReleaseAndComplete:
    PsReturnProcessPagedPoolQuota(NotifyChange->OwningProcess, NotifyChange->ThisBufferLength);

    /* Release buffer UNLESS it's used */
    if (NotifyChange->AllocatedBuffer != Irp->AssociatedIrp.SystemBuffer &&
        NotifyChange->AllocatedBuffer)
    {
        ExFreePoolWithTag(NotifyChange->AllocatedBuffer, 0);
    }

    /* Prepare for return */
    NotifyChange->AllocatedBuffer = 0;
    NotifyChange->ThisBufferLength = 0;
    Irp->IoStatus.Information = DataLength;
    NotifyChange->Buffer = NULL;

    /* Finally complete */
Completion:
    IoMarkIrpPending(Irp);
    Irp->IoStatus.Status = Status;
    FsRtlNotifyQueueCompletion(NotifyChange->NotifySync, Irp);
}

/*
 * @implemented
 */
VOID
FsRtlNotifyCompleteIrpList(IN PNOTIFY_CHANGE NotifyChange,
                           IN NTSTATUS Status)
{
    PIRP Irp;
    ULONG DataLength = NotifyChange->DataLength;
    PLIST_ENTRY Entry, Next;

    NotifyChange->Flags &= ~(NOTIFY_IMMEDIATELY | NOTIFY_LATER);
    NotifyChange->DataLength = 0;
    NotifyChange->LastEntry = 0;
    for (Entry = NotifyChange->NotifyIrps.Flink;
         Entry != &NotifyChange->NotifyIrps;
         Entry = Next)
    {
        Next = Entry->Flink;
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (!FsRtlNotifySetCancelRoutine(Irp, NotifyChange))
            continue; /* Cancellation owns both the IRP and its list entry. */
        RemoveEntryList(Entry);
        FsRtlNotifyCompleteIrp(Irp, NotifyChange, DataLength, Status, FALSE);
        if (Status == STATUS_SUCCESS)
            break;
    }
}

/*
 * @implemented
 */
BOOLEAN
FsRtlNotifySetCancelRoutine(IN PIRP Irp,
                            IN PNOTIFY_CHANGE NotifyChange OPTIONAL)
{
    PDRIVER_CANCEL CancelRoutine;

    /* Acquire cancel lock */
    IoAcquireCancelSpinLock(&Irp->CancelIrql);

    /* If NotifyChange was given */
    if (NotifyChange)
    {
        /* First get cancel routine */
        CancelRoutine = IoSetCancelRoutine(Irp, NULL);
        if (CancelRoutine != NULL)
        {
            PNOTIFY_IRP_CONTEXT Pending = Irp->Tail.Overlay.DriverContext[0];

            Irp->IoStatus.Information = 0;
            IoReleaseCancelSpinLock(Irp->CancelIrql);
            ASSERT(Pending != NULL && Pending->Change == NotifyChange);
            Irp->Tail.Overlay.DriverContext[0] = Pending->SavedDriverContext;
            ExFreePoolWithTag(Pending, TAG_FS_NOTIFICATIONS);
            InterlockedDecrement((PLONG)&NotifyChange->ReferenceCount);
            return TRUE;
        }
        IoReleaseCancelSpinLock(Irp->CancelIrql);
    }
    else
    {
        /* If IRP is cancel, call FsRtl cancel routine */
        if (Irp->Cancel)
        {
             FsRtlCancelNotify(NULL, Irp);
        }
        else
        {
            /* Otherwise, define FsRtl cancel routine as IRP cancel routine */
            IoSetCancelRoutine(Irp, FsRtlCancelNotify);
            /* Release lock */
            IoReleaseCancelSpinLock(Irp->CancelIrql);
        }
    }

    /* Return that we didn't removed cancel routine */
    return FALSE;
}

/*
 * @implemented
 */
BOOLEAN
FsRtlNotifyUpdateBuffer(OUT PFILE_NOTIFY_INFORMATION OutputBuffer,
                        IN ULONG Action,
                        IN PSTRING ParentName,
                        IN PSTRING TargetName,
                        IN PSTRING StreamName,
                        IN BOOLEAN IsUnicode,
                        IN ULONG DataLength)
{
    /* Unless there's an issue with buffers, there's no reason to fail */
    BOOLEAN Succeed = TRUE;
    ULONG AlreadyWritten = 0, ResultSize;

    PAGED_CODE();

    /* Update user buffer with the change that occured
     * First copy parent name if any
     * Then copy target name, there's always one
     * And finally, copy stream name if any
     * If these names aren't unicode, then convert first
     */
    _SEH2_TRY
    {
        OutputBuffer->NextEntryOffset = 0;
        OutputBuffer->Action = Action;
        OutputBuffer->FileNameLength = DataLength - FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName);
        if (IsUnicode)
        {
            if (ParentName->Length)
            {
                RtlCopyMemory(OutputBuffer->FileName, ParentName->Buffer, ParentName->Length);
                OutputBuffer->FileName[ParentName->Length / sizeof(WCHAR)] = L'\\';
                AlreadyWritten = ParentName->Length + sizeof(WCHAR);
            }
            RtlCopyMemory((PVOID)((ULONG_PTR)OutputBuffer->FileName + AlreadyWritten),
                          TargetName->Buffer, TargetName->Length);
            if (StreamName)
            {
                AlreadyWritten += TargetName->Length;
                OutputBuffer->FileName[AlreadyWritten / sizeof(WCHAR)] = L':';
                RtlCopyMemory((PVOID)((ULONG_PTR)OutputBuffer->FileName + AlreadyWritten + sizeof(WCHAR)),
                              StreamName->Buffer, StreamName->Length);
            }
        }
        else
        {
            if (!ParentName->Length)
            {
                ASSERT(StreamName);
                RtlCopyMemory(OutputBuffer->FileName, StreamName->Buffer, StreamName->Length);
            }
            else
            {
                RtlOemToUnicodeN(OutputBuffer->FileName, OutputBuffer->FileNameLength,
                                 &ResultSize, ParentName->Buffer,
                                 ParentName->Length);
                OutputBuffer->FileName[ResultSize / sizeof(WCHAR)] = L'\\';
                AlreadyWritten = ResultSize + sizeof(WCHAR);

                RtlOemToUnicodeN((PVOID)((ULONG_PTR)OutputBuffer->FileName + AlreadyWritten),
                                 OutputBuffer->FileNameLength, &ResultSize,
                                 TargetName->Buffer, TargetName->Length);

                if (StreamName)
                {
                    AlreadyWritten += ResultSize;
                    OutputBuffer->FileName[AlreadyWritten / sizeof(WCHAR)] = L':';
                    RtlOemToUnicodeN((PVOID)((ULONG_PTR)OutputBuffer->FileName + AlreadyWritten + sizeof(WCHAR)),
                                     OutputBuffer->FileNameLength, &ResultSize,
                                     StreamName->Buffer, StreamName->Length);
                }
            }
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Succeed = FALSE;
    }
    _SEH2_END;

    return Succeed;
}

/* PUBLIC FUNCTIONS **********************************************************/

/*++
 * @name FsRtlNotifyChangeDirectory
 * @implemented
 *
 * Lets FSD know if changes occures in the specified directory.
 * Directory will be reenumerated.
 *
 * @param NotifySync
 *        Synchronization object pointer
 *
 * @param FsContext
 *        Used to identify the notify structure
 *
 * @param FullDirectoryName
 *        String (A or W) containing the full directory name
 *
 * @param NotifyList
 *        Notify list pointer (to head)
 *
 * @param WatchTree
 *        True to notify changes in subdirectories too
 *
 * @param CompletionFilter
 *        Used to define types of changes to notify
 *
 * @param NotifyIrp
 *        IRP pointer to complete notify operation. It can be null
 *
 * @return None
 *
 * @remarks This function only redirects to FsRtlNotifyFilterChangeDirectory.
 *
 *--*/
VOID
NTAPI
FsRtlNotifyChangeDirectory(IN PNOTIFY_SYNC NotifySync,
                           IN PVOID FsContext,
                           IN PSTRING FullDirectoryName,
                           IN PLIST_ENTRY NotifyList,
                           IN BOOLEAN WatchTree,
                           IN ULONG CompletionFilter,
                           IN PIRP NotifyIrp)
{
    FsRtlNotifyFilterChangeDirectory(NotifySync,
                                     NotifyList,
                                     FsContext,
                                     FullDirectoryName,
                                     WatchTree,
                                     TRUE,
                                     CompletionFilter,
                                     NotifyIrp,
                                     NULL,
                                     NULL,
                                     NULL);
}

/*++
 * @name FsRtlNotifyCleanup
 * @implemented
 *
 * Called by FSD when all handles to FileObject (identified by FsContext) are closed
 *
 * @param NotifySync
 *        Synchronization object pointer
 *
 * @param NotifyList
 *        Notify list pointer (to head)
 *
 * @param FsContext
 *        Used to identify the notify structure
 *
 * @return None
 *
 * @remarks None
 *
 *--*/
VOID
NTAPI
FsRtlNotifyCleanup(IN PNOTIFY_SYNC NotifySync,
                   IN PLIST_ENTRY NotifyList,
                   IN PVOID FsContext)
{
    PNOTIFY_CHANGE NotifyChange;
    PREAL_NOTIFY_SYNC RealNotifySync;
    PSECURITY_SUBJECT_CONTEXT _SEH2_VOLATILE SubjectContext = NULL;
    PEPROCESS _SEH2_VOLATILE Process = NULL;

    /* Get real structure hidden behind the opaque pointer */
    RealNotifySync = (PREAL_NOTIFY_SYNC)NotifySync;

    /* Acquire the fast mutex */
    FsRtlNotifyAcquireFastMutex(RealNotifySync);

    _SEH2_TRY
    {
        /* Find if there's a matching notification with the FsContext */
        NotifyChange = FsRtlIsNotifyOnList(NotifyList, FsContext);
        if (NotifyChange)
        {
            NotifyChange->Flags |= CLEANUP_IN_PROCESS;
            RemoveEntryList(&NotifyChange->NotifyList);
            NotifyChange->NotifyList.Flink = NULL;
            NotifyChange->NotifyList.Blink = NULL;
            FsRtlNotifyCompleteIrpList(NotifyChange, STATUS_NOTIFY_CLEANUP);
            FsRtlNotifyDereference(NotifyChange, (PSECURITY_SUBJECT_CONTEXT *)&SubjectContext,
                                   (PEPROCESS *)&Process);
        }
    }
    _SEH2_FINALLY
    {
        /* Release fast mutex */
        FsRtlNotifyReleaseFastMutex(RealNotifySync);

        if (Process != NULL)
            ObDereferenceObject(Process);
        /* If the subject security context was captured, release and free it */
        if (SubjectContext)
        {
            SeReleaseSubjectContext(SubjectContext);
            ExFreePool(SubjectContext);
        }
    }
    _SEH2_END;
}

/*
 * @implemented
 */
VOID NTAPI
FsRtlNotifyCleanupAll(PNOTIFY_SYNC NotifySync, PLIST_ENTRY NotifyList)
{
    PREAL_NOTIFY_SYNC Sync = NotifySync;
    PNOTIFY_CHANGE Change;

    FsRtlNotifyAcquireFastMutex(Sync);
    while (!IsListEmpty(NotifyList))
    {
        Change = CONTAINING_RECORD(NotifyList->Flink, NOTIFY_CHANGE, NotifyList);
        FsRtlNotifyCleanup(NotifySync, NotifyList, Change->FsContext);
    }
    FsRtlNotifyReleaseFastMutex(Sync);
}

/*++
 * @name FsRtlNotifyFilterChangeDirectory
 * @implemented
 *
 * FILLME
 *
 * @param NotifySync
 *        FILLME
 *
 * @param NotifyList
 *        FILLME
 *
 * @param FsContext
 *        FILLME
 *
 * @param FullDirectoryName
 *        FILLME
 *
 * @param WatchTree
 *        FILLME
 *
 * @param IgnoreBuffer
 *        FILLME
 *
 * @param CompletionFilter
 *        FILLME
 *
 * @param NotifyIrp
 *        FILLME
 *
 * @param TraverseCallback
 *        FILLME
 *
 * @param SubjectContext
 *        FILLME
 *
 * @param FilterCallback
 *        FILLME
 *
 * @return None
 *
 * @remarks None
 *
 *--*/
VOID
NTAPI
FsRtlNotifyFilterChangeDirectory(IN PNOTIFY_SYNC NotifySync,
                                 IN PLIST_ENTRY NotifyList,
                                 IN PVOID FsContext,
                                 IN PSTRING FullDirectoryName,
                                 IN BOOLEAN WatchTree,
                                 IN BOOLEAN IgnoreBuffer,
                                 IN ULONG CompletionFilter,
                                 IN PIRP NotifyIrp,
                                 IN PCHECK_FOR_TRAVERSE_ACCESS TraverseCallback OPTIONAL,
                                 IN PSECURITY_SUBJECT_CONTEXT SubjectContext OPTIONAL,
                                 IN PFILTER_REPORT_CHANGE FilterCallback OPTIONAL)
{
    ULONG SavedLength;
    BOOLEAN _SEH2_VOLATILE SubjectTransferred = FALSE;
    PNOTIFY_IRP_CONTEXT Pending;
    PIO_STACK_LOCATION Stack;
    PNOTIFY_CHANGE NotifyChange = NULL;
    PREAL_NOTIFY_SYNC RealNotifySync;

    PAGED_CODE();

    DPRINT("FsRtlNotifyFilterChangeDirectory(): %p, %p, %p, %wZ, %u, %u, %u, %p, %p, %p, %p\n",
    NotifySync, NotifyList, FsContext, FullDirectoryName, WatchTree, IgnoreBuffer, CompletionFilter, NotifyIrp,
    TraverseCallback, SubjectContext, FilterCallback);

    /* Get real structure hidden behind the opaque pointer */
    RealNotifySync = (PREAL_NOTIFY_SYNC)NotifySync;

    /* Acquire the fast mutex */
    FsRtlNotifyAcquireFastMutex(RealNotifySync);

    _SEH2_TRY
    {
        /* If we have no IRP, FSD is performing a cleanup */
        if (!NotifyIrp)
        {
            /* So, we delete */
            FsRtlCheckNotifyForDelete(NotifyList, FsContext);
            _SEH2_LEAVE;
        }

        NotifyIrp->IoStatus.Status = STATUS_SUCCESS;
        NotifyIrp->IoStatus.Information = (ULONG_PTR)NULL;

        Stack = IoGetCurrentIrpStackLocation(NotifyIrp);
        /* If FileObject's been cleaned up, just return */
        if (Stack->FileObject->Flags & FO_CLEANUP_COMPLETE)
        {
            IoMarkIrpPending(NotifyIrp);
            NotifyIrp->IoStatus.Status = STATUS_NOTIFY_CLEANUP;
            FsRtlNotifyQueueCompletion(NotifySync, NotifyIrp);
            _SEH2_LEAVE;
        }

        /* Try to find a matching notification has been already registered */
        NotifyChange = FsRtlIsNotifyOnList(NotifyList, FsContext);
        if (NotifyChange)
        {
            /* If it's been found, and is cleaned up, immediatly complete */
            if (NotifyChange->Flags & CLEANUP_IN_PROCESS)
            {
                IoMarkIrpPending(NotifyIrp);
                NotifyIrp->IoStatus.Status = STATUS_NOTIFY_CLEANUP;
                FsRtlNotifyQueueCompletion(NotifySync, NotifyIrp);
                _SEH2_LEAVE;
            }
            /* Or if it's about to be deleted, complete */
            else if (NotifyChange->Flags & DELETE_IN_PROCESS)
            {
                IoMarkIrpPending(NotifyIrp);
                NotifyIrp->IoStatus.Status = STATUS_DELETE_PENDING;
                FsRtlNotifyQueueCompletion(NotifySync, NotifyIrp);
                _SEH2_LEAVE;
            }
            /* Complete now if asked to (and not asked to notify later on) */
            if ((NotifyChange->Flags & NOTIFY_IMMEDIATELY) && !(NotifyChange->Flags & NOTIFY_LATER))
            {
                NotifyChange->Flags &= ~NOTIFY_IMMEDIATELY;
                IoMarkIrpPending(NotifyIrp);
                NotifyIrp->IoStatus.Status = STATUS_NOTIFY_ENUM_DIR;
                FsRtlNotifyQueueCompletion(NotifySync, NotifyIrp);
            }
            /* If no data yet, or asked to notify later on, handle */
            else if (NotifyChange->DataLength == 0 || (NotifyChange->Flags & NOTIFY_LATER))
            {
                goto HandleIRP;
            }
            /* Else, just complete with we have */
            else
            {
                SavedLength = NotifyChange->DataLength;
                NotifyChange->DataLength = 0;
                FsRtlNotifyCompleteIrp(NotifyIrp, NotifyChange, SavedLength, STATUS_SUCCESS, FALSE);
            }

            _SEH2_LEAVE;
        }

        /* Allocate new notification */
        NotifyChange = ExAllocatePoolWithTag(PagedPool | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                             sizeof(NOTIFY_CHANGE), 'NrSF');
        RtlZeroMemory(NotifyChange, sizeof(NOTIFY_CHANGE));

        /* Set basic information */
        NotifyChange->NotifySync = NotifySync;
        NotifyChange->FsContext = FsContext;
        NotifyChange->StreamID = Stack->FileObject->FsContext;
        NotifyChange->TraverseCallback = TraverseCallback;
        NotifyChange->SubjectContext = SubjectContext;
        SubjectTransferred = TRUE;
        InterlockedIncrement(&RealNotifySync->References);
        NotifyChange->FullDirectoryName = FullDirectoryName;
        NotifyChange->FilterCallback = FilterCallback;
        InitializeListHead(&(NotifyChange->NotifyIrps));

        /* Keep trace of WatchTree */
        if (WatchTree)
        {
            NotifyChange->Flags |= WATCH_TREE;
        }

        /* If string is empty, faulty to ANSI */
        if (FullDirectoryName->Length == 0)
        {
            NotifyChange->CharacterSize = sizeof(CHAR);
        }
        else
        {
            /* If it can't contain WCHAR, it's ANSI */
            if (FullDirectoryName->Length < sizeof(WCHAR) || ((CHAR*)FullDirectoryName->Buffer)[1] != 0)
            {
                NotifyChange->CharacterSize = sizeof(CHAR);
            }
            else
            {
                NotifyChange->CharacterSize = sizeof(WCHAR);
            }

            /* Now, check is user is willing to watch root */
            if (FullDirectoryName->Length == NotifyChange->CharacterSize)
            {
                NotifyChange->Flags |= WATCH_ROOT;
            }
        }

        NotifyChange->CompletionFilter = CompletionFilter;

        /* In case we have not to ignore buffer , keep its length */
        if (!IgnoreBuffer)
        {
            NotifyChange->BufferLength = Stack->Parameters.NotifyDirectory.Length;
        }

        NotifyChange->OwningProcess = (PEPROCESS)NotifyIrp->Tail.Overlay.Thread->ThreadsProcess;
        ObReferenceObject(NotifyChange->OwningProcess);

        /* Insert the notification into the notification list */
        InsertTailList(NotifyList, &(NotifyChange->NotifyList));

        NotifyChange->ReferenceCount = 1;

HandleIRP:
        Pending = ExAllocatePoolWithTag(NonPagedPool | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                        sizeof(*Pending), TAG_FS_NOTIFICATIONS);
        Pending->Irp = NotifyIrp;
        Pending->Change = NotifyChange;
        Pending->SavedDriverContext = NotifyIrp->Tail.Overlay.DriverContext[0];
        NotifyIrp->Tail.Overlay.DriverContext[0] = Pending;
        /* Associate the notification to the IRP */
        NotifyIrp->IoStatus.Information = (ULONG_PTR)NotifyChange;
        /* The IRP is pending */
        IoMarkIrpPending(NotifyIrp);
        /* Insert the IRP in the IRP list */
        InsertTailList(&(NotifyChange->NotifyIrps), &(NotifyIrp->Tail.Overlay.ListEntry));
        /* Increment reference count */
        InterlockedIncrement((PLONG)&(NotifyChange->ReferenceCount));
        /* Set cancel routine to FsRtl one */
        FsRtlNotifySetCancelRoutine(NotifyIrp, NULL);
    }
    _SEH2_FINALLY
    {
        /* Release fast mutex */
        FsRtlNotifyReleaseFastMutex(RealNotifySync);

        /* If the subject security context was captured and there's no notify */
        if (SubjectContext && !SubjectTransferred)
        {
            SeReleaseSubjectContext(SubjectContext);
            ExFreePool(SubjectContext);
        }
    }
    _SEH2_END;
}

/*++
 * @name FsRtlNotifyFilterReportChange
 * @implemented
 *
 * FILLME
 *
 * @param NotifySync
 *        FILLME
 *
 * @param NotifyList
 *        FILLME
 *
 * @param FullTargetName
 *        FILLME
 *
 * @param TargetNameOffset
 *        FILLME
 *
 * @param StreamName
 *        FILLME
 *
 * @param NormalizedParentName
 *        FILLME
 *
 * @param FilterMatch
 *        FILLME
 *
 * @param Action
 *        FILLME
 *
 * @param TargetContext
 *        FILLME
 *
 * @param FilterContext
 *        FILLME
 *
 * @return None
 *
 * @remarks None
 *
 *--*/
VOID
NTAPI
FsRtlNotifyFilterReportChange(IN PNOTIFY_SYNC NotifySync,
                              IN PLIST_ENTRY NotifyList,
                              IN PSTRING FullTargetName,
                              IN USHORT TargetNameOffset,
                              IN PSTRING StreamName OPTIONAL,
                              IN PSTRING NormalizedParentName OPTIONAL,
                              IN ULONG FilterMatch,
                              IN ULONG Action,
                              IN PVOID TargetContext,
                              IN PVOID FilterContext)
{
    PIRP Irp;
    PVOID OutputBuffer;
    USHORT FullPosition;
    PLIST_ENTRY NextEntry;
    PIO_STACK_LOCATION Stack;
    PNOTIFY_CHANGE NotifyChange;
    PREAL_NOTIFY_SYNC RealNotifySync;
    PFILE_NOTIFY_INFORMATION FileNotifyInfo;
    BOOLEAN IsStream, IsParent, PoolQuotaCharged;
    STRING TargetDirectory, TargetName, ParentName, IntNormalizedParentName;
    ULONG NumberOfBytes, TargetNumberOfParts, FullNumberOfParts, LastPartOffset, ParentNameOffset, ParentNameLength;
    ULONG DataLength, AlignedDataLength;

    TargetDirectory.Length = 0;
    TargetDirectory.MaximumLength = 0;
    TargetDirectory.Buffer = NULL;
    TargetName.Length = 0;
    TargetName.MaximumLength = 0;
    TargetName.Buffer = NULL;
    ParentName.Length = 0;
    ParentName.MaximumLength = 0;
    ParentName.Buffer = NULL;
    IsStream = FALSE;

    PAGED_CODE();

    DPRINT("FsRtlNotifyFilterReportChange(%p, %p, %p, %u, %p, %p, %p, %x, %x, %p, %p)\n",
           NotifySync, NotifyList, FullTargetName, TargetNameOffset, StreamName, NormalizedParentName,
           FilterMatch, Action, TargetContext, FilterContext);

    /* We need offset in name */
    if (!TargetNameOffset && FullTargetName)
    {
        return;
    }

    /* Get real structure hidden behind the opaque pointer */
    RealNotifySync = (PREAL_NOTIFY_SYNC)NotifySync;
    /* Acquire lock - will be released in finally block */
    FsRtlNotifyAcquireFastMutex(RealNotifySync);
    _SEH2_TRY
    {
        /* Browse all the registered notifications we have */
        for (NextEntry = NotifyList->Flink; NextEntry != NotifyList;
             NextEntry = NextEntry->Flink)
        {
            /* Try to find an entry matching our change */
            NotifyChange = CONTAINING_RECORD(NextEntry, NOTIFY_CHANGE, NotifyList);
            if (FullTargetName != NULL)
            {
                ASSERT(NotifyChange->FullDirectoryName != NULL);
                if (!NotifyChange->FullDirectoryName->Length)
                {
                    continue;
                }

                if (!(FilterMatch & NotifyChange->CompletionFilter))
                {
                    continue;
                }

                /* If no normalized name provided, construct it from full target name */
                if (NormalizedParentName == NULL)
                {
                    IntNormalizedParentName.Buffer = FullTargetName->Buffer;
                    if (TargetNameOffset != NotifyChange->CharacterSize)
                    {
                        IntNormalizedParentName.MaximumLength =
                        IntNormalizedParentName.Length = TargetNameOffset - NotifyChange->CharacterSize;
                    }
                    else
                    {
                        IntNormalizedParentName.MaximumLength =
                        IntNormalizedParentName.Length = TargetNameOffset;
                    }
                    NormalizedParentName = &IntNormalizedParentName;
                }

                /* heh? Watched directory bigger than changed file? */
                if (NormalizedParentName->Length < NotifyChange->FullDirectoryName->Length)
                {
                    continue;
                }

                /* Same len => parent */
                if (NormalizedParentName->Length == NotifyChange->FullDirectoryName->Length)
                {
                    IsParent = TRUE;
                }
                /* If not, then, we have to be watching the tree, otherwise we don't have to report such changes */
                else if (!(NotifyChange->Flags & WATCH_TREE))
                {
                    continue;
                }
                /* And finally, we've to check we're properly \-terminated */
                else
                {
                    if (!(NotifyChange->Flags & WATCH_ROOT))
                    {
                        if (NotifyChange->CharacterSize == sizeof(CHAR))
                        {
                            if (((PSTR)NormalizedParentName->Buffer)[NotifyChange->FullDirectoryName->Length] != '\\')
                            {
                                continue;
                            }
                        }
                        else
                        {
                            if (((PWSTR)NormalizedParentName->Buffer)[NotifyChange->FullDirectoryName->Length / sizeof (WCHAR)] != L'\\')
                            {
                                continue;
                            }
                        }
                    }

                    IsParent = FALSE;
                }

                /* If len matches, then check that both name are equal */
                if (!RtlEqualMemory(NormalizedParentName->Buffer, NotifyChange->FullDirectoryName->Buffer,
                                    NotifyChange->FullDirectoryName->Length))
                {
                    continue;
                }

                /* Call traverse callback (only if we have to traverse ;-)) */
                if (!IsParent
                    && NotifyChange->TraverseCallback != NULL
                    && !NotifyChange->TraverseCallback(NotifyChange->FsContext,
                                                       TargetContext,
                                                       NotifyChange->SubjectContext))
                {
                    continue;
                }

                /* And then, filter callback if provided */
                if (NotifyChange->FilterCallback != NULL
                    && FilterContext != NULL
                    && !NotifyChange->FilterCallback(NotifyChange->FsContext, FilterContext))
                {
                    continue;
                }
            }
            /* We have a stream! */
            else
            {
                ASSERT(NotifyChange->FullDirectoryName == NULL);
                if (TargetContext != NotifyChange->SubjectContext)
                {
                    continue;
                }

                ParentName.Buffer = NULL;
                ParentName.Length = 0;
                IsStream = TRUE;
                IsParent = FALSE;
            }

            /* If we don't have to notify immediately, prepare for output */
            if (!(NotifyChange->Flags & NOTIFY_IMMEDIATELY))
            {
                /* If we have something to output... */
                if (NotifyChange->BufferLength)
                {
                    /* Get size of the output */
                    NumberOfBytes = 0;
                    Irp = NULL;
                    if (!NotifyChange->ThisBufferLength)
                    {
                        if (IsListEmpty(&NotifyChange->NotifyIrps))
                        {
                            NumberOfBytes = NotifyChange->BufferLength;
                        }
                        else
                        {
                            Irp = CONTAINING_RECORD(NotifyChange->NotifyIrps.Flink, IRP, Tail.Overlay.ListEntry);
                            Stack = IoGetCurrentIrpStackLocation(Irp);
                            NumberOfBytes = Stack->Parameters.NotifyDirectory.Length;
                        }
                    }
                    else
                    {
                        NumberOfBytes = NotifyChange->ThisBufferLength;
                    }

                    /* If we're matching parent, we don't care about parent (redundant) */
                    if (IsParent)
                    {
                        ParentName.Length = 0;
                    }
                    else
                    {
                        /* If we don't deal with streams, some more work is required */
                        if (!IsStream)
                        {
                            if (NotifyChange->Flags & WATCH_ROOT ||
                                (NormalizedParentName->Buffer != FullTargetName->Buffer))
                            {
                                /* Construct TargetDirectory if we don't have it yet */
                                if (TargetDirectory.Buffer == NULL)
                                {
                                    TargetDirectory.Buffer = FullTargetName->Buffer;
                                    TargetDirectory.Length = TargetNameOffset;
                                    if (TargetNameOffset != NotifyChange->CharacterSize)
                                    {
                                        TargetDirectory.Length = TargetNameOffset - NotifyChange->CharacterSize;
                                    }
                                    TargetDirectory.MaximumLength = TargetDirectory.Length;
                                }
                                /* Now, we start looking for matching parts (unless we watch root) */
                                TargetNumberOfParts = 0;
                                LastPartOffset = 0;
                                if (!(NotifyChange->Flags & WATCH_ROOT))
                                {
                                    FullNumberOfParts = 1;
                                    if (NotifyChange->CharacterSize == sizeof(CHAR))
                                    {
                                        FsRtlNotifyGetLastPartOffset(NotifyChange->FullDirectoryName->Length,
                                                                     TargetDirectory.Length, PSTR, '\\');
                                    }
                                    else
                                    {
                                        FsRtlNotifyGetLastPartOffset(NotifyChange->FullDirectoryName->Length / sizeof(WCHAR),
                                                                     TargetDirectory.Length / sizeof(WCHAR), PWSTR, L'\\');
                                        LastPartOffset *= NotifyChange->CharacterSize;
                                    }
                                }

                                /* Then, we can construct proper parent name */
                                ParentNameOffset = NotifyChange->CharacterSize + LastPartOffset;
                                ParentName.Buffer = &TargetDirectory.Buffer[ParentNameOffset];
                                ParentNameLength = TargetDirectory.Length;
                            }
                            else
                            {
                                /* Construct parent name even for streams */
                                ParentName.Buffer = &NormalizedParentName->Buffer[NotifyChange->FullDirectoryName->Length] + NotifyChange->CharacterSize;
                                ParentNameLength = NormalizedParentName->Length - NotifyChange->FullDirectoryName->Length;
                                ParentNameOffset = NotifyChange->CharacterSize;
                            }
                            ParentNameLength -= ParentNameOffset;
                            ParentName.Length = ParentNameLength;
                            ParentName.MaximumLength = ParentNameLength;
                        }
                    }

                    /* Start to count amount of data to write, we've first the structure itself */
                    DataLength = FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName);

                    /* If stream, we'll just append stream name */
                    if (IsStream)
                    {
                        ASSERT(StreamName != NULL);
                        DataLength += StreamName->Length;
                    }
                    else
                    {
                        /* If not parent, we've to append parent name */
                        if (!IsParent)
                        {
                            if (NotifyChange->CharacterSize == sizeof(CHAR))
                            {
                                DataLength += RtlOemStringToCountedUnicodeSize(&ParentName);
                            }
                            else
                            {
                                DataLength += ParentName.Length;
                            }
                            DataLength += sizeof(WCHAR);
                        }

                        /* Look for target name & construct it, if required */
                        if (TargetName.Buffer == NULL)
                        {
                            TargetName.Buffer = &FullTargetName->Buffer[TargetNameOffset];
                            TargetName.Length =
                            TargetName.MaximumLength = FullTargetName->Length - TargetNameOffset;
                        }

                        /* Then, we will append it as well */
                        if (NotifyChange->CharacterSize == sizeof(CHAR))
                        {
                            DataLength += RtlOemStringToCountedUnicodeSize(&TargetName);
                        }
                        else
                        {
                            DataLength += TargetName.Length;
                        }

                        /* If we also had a stream name, then we can append it as well */
                        if (StreamName != NULL)
                        {
                            if (NotifyChange->CharacterSize == sizeof(WCHAR))
                            {
                                DataLength += StreamName->Length + sizeof(WCHAR);
                            }
                            else
                            {
                                DataLength = DataLength + RtlOemStringToCountedUnicodeSize(&TargetName) + sizeof(CHAR);
                            }
                        }
                    }

                    /* Get the position where we can put our data (aligned!) */
                    AlignedDataLength = ROUND_UP(NotifyChange->DataLength, sizeof(ULONG));
                    /* If it's higher than buffer length, then, bail out without outputing */
                    if (DataLength > NumberOfBytes || AlignedDataLength + DataLength > NumberOfBytes)
                    {
                        NotifyChange->Flags |= NOTIFY_IMMEDIATELY;
                    }
                    else
                    {
                        OutputBuffer = NULL;
                        FileNotifyInfo = NULL;
                        /* If we already had a buffer, update last entry position */
                        if (NotifyChange->Buffer != NULL)
                        {
                            FileNotifyInfo = (PVOID)((ULONG_PTR)NotifyChange->Buffer + NotifyChange->LastEntry);
                            FileNotifyInfo->NextEntryOffset = AlignedDataLength - NotifyChange->LastEntry;
                            NotifyChange->LastEntry = AlignedDataLength;
                            /* And get our output buffer */
                            OutputBuffer = (PVOID)((ULONG_PTR)NotifyChange->Buffer + AlignedDataLength);
                        }
                        /* If we hadn't buffer, try to find one */
                        else if (Irp != NULL)
                        {
                            if (Irp->AssociatedIrp.SystemBuffer != NULL)
                            {
                                OutputBuffer = Irp->AssociatedIrp.SystemBuffer;
                            }
                            else if (Irp->MdlAddress != NULL)
                            {
                                OutputBuffer = MmGetSystemAddressForMdl(Irp->MdlAddress);
                            }

                            NotifyChange->Buffer = OutputBuffer;
                            NotifyChange->ThisBufferLength = NumberOfBytes;
                        }

                        /* If we couldn't find one, then allocate one */
                        if (NotifyChange->Buffer == NULL)
                        {
                            /* Assign the length buffer */
                            NotifyChange->ThisBufferLength = NumberOfBytes;

                            /* Assume we have not charged quotas */
                            PoolQuotaCharged = FALSE;
                            _SEH2_TRY
                            {
                                /* And charge quotas */
                                PsChargePoolQuota(NotifyChange->OwningProcess, PagedPool, NotifyChange->ThisBufferLength);
                                PoolQuotaCharged = TRUE;
                                OutputBuffer = ExAllocatePoolWithTag(PagedPool | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                                                     NumberOfBytes, TAG_FS_NOTIFICATIONS);
                                NotifyChange->Buffer = OutputBuffer;
                                NotifyChange->AllocatedBuffer = OutputBuffer;
                            }
                            /* If something went wrong during allocation, notify immediately instead of outputing */
                            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                            {
                                if (PoolQuotaCharged)
                                {
                                    PsReturnProcessPagedPoolQuota(NotifyChange->OwningProcess, NotifyChange->ThisBufferLength);
                                }
                                NotifyChange->Flags |= NOTIFY_IMMEDIATELY;
                            }
                            _SEH2_END;
                        }

                        /* Finally, if we have a buffer, fill it in! */
                        if (OutputBuffer != NULL)
                        {
                            if (FsRtlNotifyUpdateBuffer((FILE_NOTIFY_INFORMATION *)OutputBuffer,
                                                         Action, &ParentName, &TargetName,
                                                         StreamName, NotifyChange->CharacterSize == sizeof(WCHAR),
                                                         DataLength))
                            {
                                NotifyChange->DataLength = DataLength + AlignedDataLength;
                            }
                            /* If it failed, notify immediately */
                            else
                            {
                                NotifyChange->Flags |= NOTIFY_IMMEDIATELY;
                            }
                        }
                    }

                    /* If we have to notify right now (something went wrong?) */
                    if (NotifyChange->Flags & NOTIFY_IMMEDIATELY)
                    {
                        /* Ensure that all our buffers are NULL */
                        if (NotifyChange->Buffer != NULL)
                        {
                            if (NotifyChange->AllocatedBuffer != NULL)
                            {
                                PsReturnProcessPagedPoolQuota(NotifyChange->OwningProcess, NotifyChange->ThisBufferLength);
                                ExFreePoolWithTag(NotifyChange->AllocatedBuffer, TAG_FS_NOTIFICATIONS);
                            }

                            NotifyChange->Buffer = NULL;
                            NotifyChange->AllocatedBuffer = NULL;
                            NotifyChange->LastEntry = 0;
                            NotifyChange->DataLength = 0;
                            NotifyChange->ThisBufferLength = 0;
                        }
                    }
                }
            }

            /* If asking for old name in case of a rename, notify later on,
             * so that we can wait for new name.
             * https://learn.microsoft.com/en-us/openspecs/main/ms-openspeclp/3589baea-5b22-48f2-9d43-f5bea4960ddb
             */
            if (Action == FILE_ACTION_RENAMED_OLD_NAME)
            {
                NotifyChange->Flags |= NOTIFY_LATER;
            }
            else
            {
                NotifyChange->Flags &= ~NOTIFY_LATER;
                if (!IsListEmpty(&NotifyChange->NotifyIrps))
                {
                    FsRtlNotifyCompleteIrpList(NotifyChange, STATUS_SUCCESS);
                }
            }
        }
    }
    _SEH2_FINALLY
    {
        FsRtlNotifyReleaseFastMutex(RealNotifySync);
    }
    _SEH2_END;
}

/*++
 * @name FsRtlNotifyFullChangeDirectory
 * @implemented
 *
 * Lets FSD know if changes occures in the specified directory.
 *
 * @param NotifySync
 *        Synchronization object pointer
 *
 * @param NotifyList
 *        Notify list pointer (to head)
 *
 * @param FsContext
 *        Used to identify the notify structure
 *
 * @param FullDirectoryName
 *        String (A or W) containing the full directory name
 *
 * @param WatchTree
 *        True to notify changes in subdirectories too
 *
 * @param IgnoreBuffer
 *        True to reenumerate directory. It's ignored it NotifyIrp is null
 *
 * @param CompletionFilter
 *        Used to define types of changes to notify
 *
 * @param NotifyIrp
 *        IRP pointer to complete notify operation. It can be null
 *
 * @param TraverseCallback
 *        Pointer to a callback function. It's called each time a change is
 *        done in a subdirectory of the main directory. It's ignored it NotifyIrp
 *        is null
 *
 * @param SubjectContext
 *        Pointer to pass to SubjectContext member of TraverseCallback.
 *        It's freed after use. It's ignored it NotifyIrp is null
 *
 * @return None
 *
 * @remarks This function only redirects to FsRtlNotifyFilterChangeDirectory.
 *
 *--*/
VOID
NTAPI
FsRtlNotifyFullChangeDirectory(IN PNOTIFY_SYNC NotifySync,
                               IN PLIST_ENTRY NotifyList,
                               IN PVOID FsContext,
                               IN PSTRING FullDirectoryName,
                               IN BOOLEAN WatchTree,
                               IN BOOLEAN IgnoreBuffer,
                               IN ULONG CompletionFilter,
                               IN PIRP NotifyIrp,
                               IN PCHECK_FOR_TRAVERSE_ACCESS TraverseCallback OPTIONAL,
                               IN PSECURITY_SUBJECT_CONTEXT SubjectContext OPTIONAL)
{
    FsRtlNotifyFilterChangeDirectory(NotifySync,
                                     NotifyList,
                                     FsContext,
                                     FullDirectoryName,
                                     WatchTree,
                                     IgnoreBuffer,
                                     CompletionFilter,
                                     NotifyIrp,
                                     TraverseCallback,
                                     SubjectContext,
                                     NULL);
}

/*++
 * @name FsRtlNotifyFullReportChange
 * @implemented
 *
 * Complets the pending notify IRPs.
 *
 * @param NotifySync
 *        Synchronization object pointer
 *
 * @param NotifyList
 *        Notify list pointer (to head)
 *
 * @param FullTargetName
 *        String (A or W) containing the full directory name that changed
 *
 * @param TargetNameOffset
 *        Offset, in FullTargetName, of the final component that is in the changed directory
 *
 * @param StreamName
 *        String (A or W) containing a stream name
 *
 * @param NormalizedParentName
 *        String (A or W) containing the full directory name that changed with long names
 *
 * @param FilterMatch
 *        Flags that will be compared to the completion filter
 *
 * @param Action
 *        Action code to store in user's buffer
 *
 * @param TargetContext
 *        Pointer to a callback function. It's called each time a change is
 *        done in a subdirectory of the main directory.
 *
 * @return None
 *
 * @remarks This function only redirects to FsRtlNotifyFilterReportChange.
 *
 *--*/
VOID
NTAPI
FsRtlNotifyFullReportChange(IN PNOTIFY_SYNC NotifySync,
                            IN PLIST_ENTRY NotifyList,
                            IN PSTRING FullTargetName,
                            IN USHORT TargetNameOffset,
                            IN PSTRING StreamName OPTIONAL,
                            IN PSTRING NormalizedParentName OPTIONAL,
                            IN ULONG FilterMatch,
                            IN ULONG Action,
                            IN PVOID TargetContext)
{
    FsRtlNotifyFilterReportChange(NotifySync,
                                  NotifyList,
                                  FullTargetName,
                                  TargetNameOffset,
                                  StreamName,
                                  NormalizedParentName,
                                  FilterMatch,
                                  Action,
                                  TargetContext,
                                  NULL);
}

/*++
 * @name FsRtlNotifyInitializeSync
 * @implemented
 *
 * Allocates the internal structure associated with notifications.
 *
 * @param NotifySync
 *        Opaque pointer. It will receive the address of the allocated internal structure.
 *
 * @return None
 *
 * @remarks This function raise an exception in case of a failure.
 *
 *--*/
VOID
NTAPI
FsRtlNotifyInitializeSync(IN PNOTIFY_SYNC *NotifySync)
{
    PREAL_NOTIFY_SYNC RealNotifySync;

    *NotifySync = NULL;

    RealNotifySync = ExAllocatePoolWithTag(NonPagedPool | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                           sizeof(REAL_NOTIFY_SYNC), 'SNSF');
    ExInitializeFastMutex(&(RealNotifySync->FastMutex));
    RealNotifySync->OwningThread = 0;
    RealNotifySync->OwnerCount = 0;
    RealNotifySync->References = 1;
    InitializeListHead(&RealNotifySync->Completions);

    *NotifySync = RealNotifySync;
}

/*++
 * @name FsRtlNotifyReportChange
 * @implemented
 *
 * Complets the pending notify IRPs.
 *
 * @param NotifySync
 *        Synchronization object pointer
 *
 * @param NotifyList
 *        Notify list pointer (to head)
 *
 * @param FullTargetName
 *        String (A or W) containing the full directory name that changed
 *
 * @param FileNamePartLength
 *        Length of the final component that is in the changed directory
 *
 * @param FilterMatch
 *        Flags that will be compared to the completion filter
 *
 * @return None
 *
 * @remarks This function only redirects to FsRtlNotifyFilterReportChange.
 *
 *--*/
VOID
NTAPI
FsRtlNotifyReportChange(IN PNOTIFY_SYNC NotifySync,
                        IN PLIST_ENTRY NotifyList,
                        IN PSTRING FullTargetName,
                        IN PUSHORT FileNamePartLength,
                        IN ULONG FilterMatch)
{
      FsRtlNotifyFilterReportChange(NotifySync,
                                    NotifyList,
                                    FullTargetName,
                                    FullTargetName->Length - *FileNamePartLength,
                                    NULL,
                                    NULL,
                                    FilterMatch,
                                    0,
                                    NULL,
                                    NULL);
}

/*++
 * @name FsRtlNotifyUninitializeSync
 * @implemented
 *
 * Uninitialize a NOTIFY_SYNC object
 *
 * @param NotifySync
 *        Address of a pointer to a PNOTIFY_SYNC object previously
 *        initialized by FsRtlNotifyInitializeSync()
 *
 * @return None
 *
 * @remarks None
 *
 *--*/
VOID
NTAPI
FsRtlNotifyUninitializeSync(IN PNOTIFY_SYNC *NotifySync)
{
    if (*NotifySync)
    {
        PREAL_NOTIFY_SYNC Sync = *NotifySync;

        *NotifySync = NULL;
        FsRtlNotifyDereferenceSync(Sync);
    }
}

