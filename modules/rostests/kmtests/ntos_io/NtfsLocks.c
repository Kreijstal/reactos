/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     NTFS byte-range ownership, shared locks, aliases and waiting IRPs
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

/* kmt's ok_eq macros evaluate their arguments twice. Each I/O operation here
 * must be issued only once, including unlock and completion/free operations. */
#define check_status(Expression, Expected) \
    do { \
        NTSTATUS ActualStatus = (Expression); \
        ok(ActualStatus == (Expected), "%s returned %lx, expected %lx\n", \
           #Expression, ActualStatus, (NTSTATUS)(Expected)); \
    } while (0)

typedef struct _LOCK_REQUEST
{
    PIRP Irp;
    PFILE_OBJECT File;
    LARGE_INTEGER Length;
    KEVENT Event;
    NTSTATUS Status;
} LOCK_REQUEST, *PLOCK_REQUEST;

static
NTSTATUS
NTAPI
LockCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    PLOCK_REQUEST Request = Context;

    UNREFERENCED_PARAMETER(Device);
    Request->Status = Irp->IoStatus.Status;
    KeSetEvent(&Request->Event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
StartLock(HANDLE File, UCHAR Minor, ULONGLONG Offset, ULONGLONG Length,
          ULONG Key, UCHAR Flags, PLOCK_REQUEST *Result)
{
    PLOCK_REQUEST Request;
    PDEVICE_OBJECT Device;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    *Result = NULL;
    Request = NtfsKmtAlloc(sizeof(*Request));
    if (Request == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Request, sizeof(*Request));
    Status = ObReferenceObjectByHandle(File, 0, *IoFileObjectType, KernelMode,
                                       (PVOID *)&Request->File, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsKmtFree(Request);
        return Status;
    }
    Device = IoGetRelatedDeviceObject(Request->File);
    Request->Irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (Request->Irp == NULL)
    {
        ObDereferenceObject(Request->File);
        NtfsKmtFree(Request);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Request->Length.QuadPart = Length;
    Request->Status = STATUS_PENDING;
    KeInitializeEvent(&Request->Event, NotificationEvent, FALSE);
    Request->Irp->RequestorMode = KernelMode;
    Request->Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Stack = IoGetNextIrpStackLocation(Request->Irp);
    Stack->MajorFunction = IRP_MJ_LOCK_CONTROL;
    Stack->MinorFunction = Minor;
    Stack->Flags = Flags;
    Stack->FileObject = Request->File;
    Stack->Parameters.LockControl.ByteOffset.QuadPart = Offset;
    Stack->Parameters.LockControl.Length = &Request->Length;
    Stack->Parameters.LockControl.Key = Key;
    IoSetCompletionRoutine(Request->Irp, LockCompletion, Request, TRUE, TRUE, TRUE);
    *Result = Request;
    return IoCallDriver(Device, Request->Irp);
}

static
NTSTATUS
FinishLock(PLOCK_REQUEST Request)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    if (Request == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Timeout.QuadPart = -3 * 10000000LL;
    Status = KeWaitForSingleObject(&Request->Event, Executive, KernelMode, FALSE, &Timeout);
    check_status(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
    {
        IoCancelIrp(Request->Irp);
        Status = KeWaitForSingleObject(&Request->Event, Executive, KernelMode, FALSE, &Timeout);
        check_status(Status, STATUS_SUCCESS);
        if (Status != STATUS_SUCCESS)
            return STATUS_IO_TIMEOUT; /* Keep the heap request and its file alive. */
    }
    Status = Request->Status;
    IoFreeIrp(Request->Irp);
    ObDereferenceObject(Request->File);
    NtfsKmtFree(Request);
    return Status;
}

static
NTSTATUS
Lock(HANDLE File, UCHAR Minor, ULONGLONG Offset, ULONGLONG Length, ULONG Key, BOOLEAN Exclusive)
{
    PLOCK_REQUEST Request;

    StartLock(File, Minor, Offset, Length, Key,
               SL_FAIL_IMMEDIATELY | (Exclusive ? SL_EXCLUSIVE_LOCK : 0), &Request);
    return FinishLock(Request);
}

static
NTSTATUS
ReadKey(HANDLE File, LONGLONG Offset, ULONG Key)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Position;
    UCHAR Byte;

    Position.QuadPart = Offset;
    return ZwReadFile(File, NULL, NULL, NULL, &IoStatus, &Byte, 1, &Position, &Key);
}

START_TEST(NtfsLocks)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH], Alias[MAX_PATH], Stream[MAX_PATH];
    HANDLE A = NULL, B = NULL, C = NULL, S = NULL, LinkFile = NULL;
    UCHAR Buffer[128];
    ULONG_PTR Transferred;
    PLOCK_REQUEST Pending;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_LOCKS.bin");
    NtfsKmtJoin(Alias, RTL_NUMBER_OF(Alias), Root, L"KMT_NTFS_LOCKS_alias.bin");
    NtfsKmtJoin(Stream, RTL_NUMBER_OF(Stream), Root, L"KMT_NTFS_LOCKS.bin:other");
    NtfsKmtFillPattern(Buffer, sizeof(Buffer), 53, 0);
    Status = NtfsKmtCreateFileWithData(Path, Buffer, sizeof(Buffer));
    check_status(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&A, Path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_NON_DIRECTORY_FILE);
    check_status(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&B, Path, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    check_status(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&C, Path, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    check_status(Status, STATUS_SUCCESS);
    if (A == NULL || B == NULL || C == NULL)
        goto Cleanup;

    Status = Lock(A, IRP_MN_LOCK, 0, 16, 0, TRUE);
    check_status(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    check_status(Lock(B, IRP_MN_LOCK, 0, 16, 0, TRUE), STATUS_LOCK_NOT_GRANTED);
    check_status(NtfsKmtRead(B, 0, Buffer, 1, &Transferred), STATUS_FILE_LOCK_CONFLICT);
    ok_eq_ulongptr(Transferred, 0);
    check_status(NtfsKmtWrite(B, 8, Buffer, 1, &Transferred), STATUS_FILE_LOCK_CONFLICT);
    check_status(NtfsKmtWrite(A, 8, Buffer, 1, &Transferred), STATUS_SUCCESS);
    check_status(NtfsKmtRead(A, 0, Buffer, 16, &Transferred), STATUS_SUCCESS);
    check_status(Lock(A, IRP_MN_LOCK, 0, 16, 0, TRUE), STATUS_LOCK_NOT_GRANTED);
    check_status(Lock(B, IRP_MN_UNLOCK_SINGLE, 0, 16, 0, FALSE), STATUS_RANGE_NOT_LOCKED);
    check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 0, 16, 1, FALSE), STATUS_RANGE_NOT_LOCKED);
    check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 0, 8, 0, FALSE), STATUS_RANGE_NOT_LOCKED);
    check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 0, 16, 0, FALSE), STATUS_SUCCESS);
    check_status(NtfsKmtRead(B, 0, Buffer, 1, &Transferred), STATUS_SUCCESS);

    check_status(Lock(A, IRP_MN_LOCK, 0, 16, 0, FALSE), STATUS_SUCCESS);
    check_status(Lock(B, IRP_MN_LOCK, 8, 16, 0, FALSE), STATUS_SUCCESS);
    check_status(NtfsKmtRead(C, 0, Buffer, 24, &Transferred), STATUS_SUCCESS);
    check_status(NtfsKmtWrite(A, 0, Buffer, 1, &Transferred), STATUS_FILE_LOCK_CONFLICT);
    check_status(NtfsKmtWrite(B, 8, Buffer, 1, &Transferred), STATUS_FILE_LOCK_CONFLICT);
    check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 0, 16, 0, FALSE), STATUS_SUCCESS);
    check_status(NtfsKmtWrite(C, 0, Buffer, 1, &Transferred), STATUS_SUCCESS);
    check_status(NtfsKmtWrite(C, 8, Buffer, 1, &Transferred), STATUS_FILE_LOCK_CONFLICT);
    check_status(Lock(B, IRP_MN_UNLOCK_ALL_BY_KEY, 0, 0, 0, FALSE), STATUS_SUCCESS);
    check_status(NtfsKmtWrite(C, 8, Buffer, 1, &Transferred), STATUS_SUCCESS);

    check_status(Lock(A, IRP_MN_LOCK, 0, 8, 0, TRUE), STATUS_SUCCESS);
    check_status(Lock(B, IRP_MN_LOCK, 16, 8, 0, TRUE), STATUS_SUCCESS);
    ZwClose(A);
    A = NULL;
    check_status(NtfsKmtRead(C, 0, Buffer, 1, &Transferred), STATUS_SUCCESS);
    check_status(NtfsKmtRead(C, 16, Buffer, 1, &Transferred), STATUS_FILE_LOCK_CONFLICT);
    check_status(Lock(B, IRP_MN_UNLOCK_SINGLE, 16, 8, 0, FALSE), STATUS_SUCCESS);
    Status = NtfsKmtOpen(&A, Path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_NON_DIRECTORY_FILE);
    check_status(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    check_status(Lock(A, IRP_MN_LOCK, 32, 8, 0x1234, TRUE), STATUS_SUCCESS);
    check_status(ReadKey(A, 32, 0), STATUS_FILE_LOCK_CONFLICT);
    check_status(ReadKey(A, 32, 0x1234), STATUS_SUCCESS);
    check_status(ReadKey(B, 32, 0x1234), STATUS_FILE_LOCK_CONFLICT);
    check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 32, 8, 0x1234, FALSE), STATUS_SUCCESS);
    check_status(Lock(A, IRP_MN_LOCK, 0x100000000ULL, 8, 0, TRUE), STATUS_SUCCESS);
    check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 0x100000000ULL, 8, 0, FALSE), STATUS_SUCCESS);

    Status = NtfsKmtLink(A, Alias, FALSE);
    check_status(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtOpen(&LinkFile, Alias, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
        check_status(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            check_status(Lock(A, IRP_MN_LOCK, 0, 8, 0, TRUE), STATUS_SUCCESS);
            check_status(NtfsKmtRead(LinkFile, 0, Buffer, 1, &Transferred), STATUS_FILE_LOCK_CONFLICT);
            check_status(Lock(LinkFile, IRP_MN_LOCK, 0, 8, 0, TRUE), STATUS_LOCK_NOT_GRANTED);
            check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 0, 8, 0, FALSE), STATUS_SUCCESS);
            ZwClose(LinkFile);
            LinkFile = NULL;
        }
    }
    Status = NtfsKmtCreateFileWithData(Stream, Buffer, sizeof(Buffer));
    check_status(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&S, Stream, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    check_status(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        check_status(Lock(A, IRP_MN_LOCK, 0, 8, 0, TRUE), STATUS_SUCCESS);
        check_status(NtfsKmtRead(S, 0, Buffer, 1, &Transferred), STATUS_SUCCESS);
        check_status(Lock(S, IRP_MN_LOCK, 0, 8, 0, TRUE), STATUS_SUCCESS);
        check_status(Lock(S, IRP_MN_UNLOCK_SINGLE, 0, 8, 0, FALSE), STATUS_SUCCESS);
        check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 0, 8, 0, FALSE), STATUS_SUCCESS);
    }

    check_status(Lock(A, IRP_MN_LOCK, 64, 8, 0, TRUE), STATUS_SUCCESS);
    Status = StartLock(B, IRP_MN_LOCK, 64, 8, 0, SL_EXCLUSIVE_LOCK, &Pending);
    check_status(Status, STATUS_PENDING);
    if (Pending != NULL)
    {
        ok_eq_long(KeReadStateEvent(&Pending->Event), 0);
        check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 64, 8, 0, FALSE), STATUS_SUCCESS);
        check_status(FinishLock(Pending), STATUS_SUCCESS);
        check_status(Lock(B, IRP_MN_UNLOCK_SINGLE, 64, 8, 0, FALSE), STATUS_SUCCESS);
    }
    check_status(Lock(A, IRP_MN_LOCK, 64, 8, 0, TRUE), STATUS_SUCCESS);
    Status = StartLock(B, IRP_MN_LOCK, 64, 8, 0, SL_EXCLUSIVE_LOCK, &Pending);
    check_status(Status, STATUS_PENDING);
    if (Pending != NULL)
    {
        IoCancelIrp(Pending->Irp);
        check_status(FinishLock(Pending), STATUS_CANCELLED);
    }
    check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 64, 8, 0, FALSE), STATUS_SUCCESS);

    check_status(Lock(A, IRP_MN_LOCK, 64, 8, 0, TRUE), STATUS_SUCCESS);
    Status = StartLock(B, IRP_MN_LOCK, 64, 8, 0, SL_EXCLUSIVE_LOCK, &Pending);
    check_status(Status, STATUS_PENDING);
    if (Pending != NULL)
    {
        check_status(ZwClose(B), STATUS_SUCCESS);
        B = NULL;
        check_status(FinishLock(Pending), STATUS_CANCELLED);
    }
    check_status(Lock(A, IRP_MN_UNLOCK_SINGLE, 64, 8, 0, FALSE), STATUS_SUCCESS);

Cleanup:
    if (LinkFile != NULL) ZwClose(LinkFile);
    if (S != NULL) ZwClose(S);
    if (C != NULL) ZwClose(C);
    if (B != NULL) ZwClose(B);
    if (A != NULL) ZwClose(A);
    NtfsKmtDelete(Stream);
    NtfsKmtDelete(Alias);
    NtfsKmtDelete(Path);
}

static
NTSTATUS
NTAPI
RemovalCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

START_TEST(NtfsLockRemove)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH];
    HANDLE A = NULL, B = NULL;
    PLOCK_REQUEST Pending = NULL;
    PDEVICE_OBJECT Device;
    PFILE_OBJECT File;
    PIRP Irp;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_LOCK_REMOVE.bin");
    Status = NtfsKmtCreateFileWithData(Path, "lock removal", 12);
    check_status(Status, STATUS_SUCCESS);
    check_status(NtfsKmtOpen(&A, Path, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE), STATUS_SUCCESS);
    check_status(NtfsKmtOpen(&B, Path, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE), STATUS_SUCCESS);
    if (A == NULL || B == NULL)
        goto CleanupRemove;
    check_status(Lock(A, IRP_MN_LOCK, 0, 8, 0, TRUE), STATUS_SUCCESS);
    Status = StartLock(B, IRP_MN_LOCK, 0, 8, 0, SL_EXCLUSIVE_LOCK, &Pending);
    check_status(Status, STATUS_PENDING);
    Status = ObReferenceObjectByHandle(A, 0, *IoFileObjectType, KernelMode, (PVOID *)&File, NULL);
    check_status(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto CleanupRemove;
    Device = IoGetRelatedDeviceObject(File);
    ObReferenceObject(Device);
    ObDereferenceObject(File);
    Irp = IoAllocateIrp(Device->StackSize, FALSE);
    ok(Irp != NULL, "could not allocate surprise-removal IRP\n");
    if (Irp != NULL)
    {
        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        Irp->RequestorMode = KernelMode;
        Irp->Tail.Overlay.Thread = PsGetCurrentThread();
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->MajorFunction = IRP_MJ_PNP;
        Stack->MinorFunction = IRP_MN_SURPRISE_REMOVAL;
        IoSetCompletionRoutine(Irp, RemovalCompletion, &Event, TRUE, TRUE, TRUE);
        Status = IoCallDriver(Device, Irp);
        if (Status == STATUS_PENDING)
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        check_status(Irp->IoStatus.Status, STATUS_SUCCESS);
        IoFreeIrp(Irp);
        if (Pending != NULL)
        {
            check_status(FinishLock(Pending), STATUS_DEVICE_REMOVED);
            Pending = NULL;
        }
    }
    ObDereferenceObject(Device);
CleanupRemove:
    if (Pending != NULL)
    {
        IoCancelIrp(Pending->Irp);
        FinishLock(Pending);
    }
    if (B != NULL) check_status(ZwClose(B), STATUS_SUCCESS);
    if (A != NULL) check_status(ZwClose(A), STATUS_SUCCESS);
}
