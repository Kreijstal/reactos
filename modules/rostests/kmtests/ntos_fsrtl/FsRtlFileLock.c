/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     File-lock callback ownership, rejected grants and cancellation rundown
 */

#include <kmt_test.h>

#define LOCK_MARKER ((PVOID)(ULONG_PTR)0x6543210)
#define check_status(Expression, Expected) \
    do { \
        NTSTATUS ActualStatus = (Expression); \
        ok(ActualStatus == (Expected), "%s returned %lx, expected %lx\n", \
           #Expression, ActualStatus, (NTSTATUS)(Expected)); \
    } while (0)

typedef struct _LOCK_CALL
{
    PIRP Irp;
    LARGE_INTEGER Length;
    KEVENT Done, Entered, Release;
    PVOID CallbackContext;
    PVOID DriverContext;
    NTSTATUS Status;
    KIRQL Irql;
    BOOLEAN Deny, Block;
} LOCK_CALL, *PLOCK_CALL;

static NTSTATUS NTAPI
CompleteRequest(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    PLOCK_CALL Call = Context;

    UNREFERENCED_PARAMETER(Device);
    Call->Status = Irp->IoStatus.Status;
    KeSetEvent(&Call->Done, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS NTAPI
CompleteLock(PVOID Context, PIRP Irp)
{
    PLOCK_CALL Call = Irp->Tail.Overlay.DriverContext[2];
    NTSTATUS Status = Irp->IoStatus.Status;

    Call->CallbackContext = Context;
    Call->DriverContext = Irp->Tail.Overlay.DriverContext[0];
    Call->Irql = KeGetCurrentIrql();
    if (Call->Block)
    {
        KeSetEvent(&Call->Entered, IO_NO_INCREMENT, FALSE);
        if (Call->Irql <= APC_LEVEL)
            KeWaitForSingleObject(&Call->Release, Executive, KernelMode, FALSE, NULL);
    }
    if (Call->Deny && Status == STATUS_SUCCESS)
        Status = STATUS_ACCESS_DENIED;
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static BOOLEAN
InitializeCall(PLOCK_CALL Call, PFILE_OBJECT File, UCHAR Minor, ULONG Key,
               BOOLEAN Exclusive, BOOLEAN FailImmediately)
{
    PIO_STACK_LOCATION Stack;

    RtlZeroMemory(Call, sizeof(*Call));
    KeInitializeEvent(&Call->Done, NotificationEvent, FALSE);
    KeInitializeEvent(&Call->Entered, NotificationEvent, FALSE);
    KeInitializeEvent(&Call->Release, NotificationEvent, FALSE);
    Call->Length.QuadPart = 16;
    Call->Irp = IoAllocateIrp(1, FALSE);
    ok(Call->Irp != NULL, "could not allocate lock IRP\n");
    if (Call->Irp == NULL)
        return FALSE;
    Call->Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Call->Irp->RequestorMode = KernelMode;
    Call->Irp->Tail.Overlay.DriverContext[0] = LOCK_MARKER;
    Call->Irp->Tail.Overlay.DriverContext[2] = Call;
    Stack = IoGetNextIrpStackLocation(Call->Irp);
    Stack->MajorFunction = IRP_MJ_LOCK_CONTROL;
    Stack->MinorFunction = Minor;
    Stack->Flags = (Exclusive ? SL_EXCLUSIVE_LOCK : 0) | (FailImmediately ? SL_FAIL_IMMEDIATELY : 0);
    Stack->FileObject = File;
    Stack->Parameters.LockControl.ByteOffset.QuadPart = 0;
    Stack->Parameters.LockControl.Length = &Call->Length;
    Stack->Parameters.LockControl.Key = Key;
    IoSetCompletionRoutine(Call->Irp, CompleteRequest, Call, TRUE, TRUE, TRUE);
    IoSetNextIrpStackLocation(Call->Irp);
    return TRUE;
}

static BOOLEAN
FinishCall(PLOCK_CALL Call, NTSTATUS Expected)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Timeout.QuadPart = -3 * 10000000LL;
    Status = KeWaitForSingleObject(&Call->Done, Executive, KernelMode, FALSE, &Timeout);
    check_status(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
        return FALSE;
    check_status(Call->Status, Expected);
    ok(Call->CallbackContext == Call, "callback context %p, expected %p\n", Call->CallbackContext, Call);
    ok(Call->DriverContext == LOCK_MARKER, "driver context was not restored: %p\n", Call->DriverContext);
    ok(Call->Irql <= APC_LEVEL, "file-lock callback ran at IRQL %u\n", Call->Irql);
    IoFreeIrp(Call->Irp);
    Call->Irp = NULL;
    return TRUE;
}

typedef struct _LOCK_TEST_STATE
{
    FILE_LOCK Lock;
    FILE_OBJECT A, B;
    LOCK_CALL First, Second, Unlock;
} LOCK_TEST_STATE, *PLOCK_TEST_STATE;

START_TEST(FsRtlFileLock)
{
    PLOCK_TEST_STATE State;
    PFILE_LOCK Lock;
    PFILE_OBJECT A, B;
    PLOCK_CALL First, Second, Unlock;
    LARGE_INTEGER Offset, Length, Timeout;
    IO_STATUS_BLOCK IoStatus;
    PFILE_LOCK_INFO Info;
    KIRQL OldIrql;
    BOOLEAN Granted;
    NTSTATUS Status;

    State = ExAllocatePoolWithTag(NonPagedPool, sizeof(*State), 'tLfF');
    ok(State != NULL, "could not allocate test state\n");
    if (State == NULL)
        return;
    RtlZeroMemory(State, sizeof(*State));
    Lock = &State->Lock;
    A = &State->A;
    B = &State->B;
    First = &State->First;
    Second = &State->Second;
    Unlock = &State->Unlock;
    /* An early failure retains this state so a late completion cannot access
     * an expired kernel stack or a freed mock file object. */

    Offset.QuadPart = 0;
    Length.QuadPart = 16;
    FsRtlInitializeFileLock(Lock, CompleteLock, NULL);

    /* A failed callback removes only its own shared acquisition. */
    if (!InitializeCall(First, A, IRP_MN_LOCK, 0, FALSE, TRUE))
        return;
    check_status(FsRtlProcessFileLock(Lock, First->Irp, First), STATUS_SUCCESS);
    if (!FinishCall(First, STATUS_SUCCESS)) return;
    if (!InitializeCall(Second, B, IRP_MN_LOCK, 0, FALSE, TRUE)) return;
    Second->Deny = TRUE;
    check_status(FsRtlProcessFileLock(Lock, Second->Irp, Second), STATUS_ACCESS_DENIED);
    if (!FinishCall(Second, STATUS_ACCESS_DENIED)) return;
    Info = FsRtlGetNextFileLock(Lock, TRUE);
    ok(Info != NULL && Info->FileObject == A, "original shared acquisition was lost\n");
    Info = FsRtlGetNextFileLock(Lock, FALSE);
    ok(Info == NULL, "rejected acquisition remained locked\n");
    check_status(FsRtlFastUnlockSingle(Lock, B, &Offset, &Length, PsGetCurrentProcess(), 0, NULL, FALSE),
                 STATUS_RANGE_NOT_LOCKED);
    check_status(FsRtlFastUnlockSingle(Lock, A, &Offset, &Length, PsGetCurrentProcess(), 0, NULL, FALSE),
                 STATUS_SUCCESS);

    /* Unlocking with a different context must complete the queued request
     * with that request's original context, not the unlocking request's. */
    if (!InitializeCall(First, A, IRP_MN_LOCK, 0, TRUE, TRUE)) return;
    check_status(FsRtlProcessFileLock(Lock, First->Irp, First), STATUS_SUCCESS);
    if (!FinishCall(First, STATUS_SUCCESS)) return;
    if (!InitializeCall(Second, B, IRP_MN_LOCK, 7, TRUE, FALSE)) return;
    check_status(FsRtlProcessFileLock(Lock, Second->Irp, Second), STATUS_PENDING);
    ok_eq_long(KeReadStateEvent(&Second->Done), 0);
    if (!InitializeCall(Unlock, A, IRP_MN_UNLOCK_SINGLE, 0, FALSE, TRUE)) return;
    check_status(FsRtlProcessFileLock(Lock, Unlock->Irp, Unlock), STATUS_SUCCESS);
    if (!FinishCall(Unlock, STATUS_SUCCESS)) return;
    if (!FinishCall(Second, STATUS_SUCCESS)) return;
    check_status(FsRtlFastUnlockSingle(Lock, B, &Offset, &Length, PsGetCurrentProcess(), 7, NULL, FALSE),
                 STATUS_SUCCESS);

    /* An already canceled request exercises the CSQ's inline-cancel path. */
    Granted = FsRtlPrivateLock(Lock, A, &Offset, &Length, PsGetCurrentProcess(), 0,
                               TRUE, TRUE, &IoStatus, NULL, NULL, FALSE);
    ok(Granted, "could not acquire blocker\n");
    if (!InitializeCall(Second, B, IRP_MN_LOCK, 0, TRUE, FALSE)) return;
    Second->Irp->Cancel = TRUE;
    check_status(FsRtlProcessFileLock(Lock, Second->Irp, Second), STATUS_PENDING);
    if (!FinishCall(Second, STATUS_CANCELLED)) return;

    /* Keep a cancel callback alive while teardown detaches the queue. This
     * forces completion to outlive FILE_LOCK's reference to its private state. */
    if (!InitializeCall(Second, B, IRP_MN_LOCK, 0, TRUE, FALSE)) return;
    Second->Block = TRUE;
    check_status(FsRtlProcessFileLock(Lock, Second->Irp, Second), STATUS_PENDING);
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    IoCancelIrp(Second->Irp);
    KeLowerIrql(OldIrql);
    Timeout.QuadPart = -3 * 10000000LL;
    Status = KeWaitForSingleObject(&Second->Entered, Executive, KernelMode, FALSE, &Timeout);
    check_status(Status, STATUS_SUCCESS);
    /* A failed cancel must not leave synchronous teardown waiting for the
     * gate that this thread would otherwise open only after teardown. */
    if (Status != STATUS_SUCCESS)
        KeSetEvent(&Second->Release, IO_NO_INCREMENT, FALSE);
    FsRtlUninitializeFileLock(Lock);
    KeSetEvent(&Second->Release, IO_NO_INCREMENT, FALSE);
    if (!FinishCall(Second, STATUS_CANCELLED)) return;
    ok(!FsRtlAreThereCurrentFileLocks(Lock), "locks survived teardown\n");
    ExFreePoolWithTag(State, 'tLfF');
}
