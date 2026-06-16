/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Test driver reproducing the Cc lazy-writer / CcFlushCache
 *                  dirty-VACB race (ASSERT(Vacb->Dirty) in cc/view.c)
 * PROGRAMMER:      ReactOS Developers
 *
 * The race:
 *   CcRosFlushDirtyPages (lazy writer) pulls a dirty VACB off the dirty list,
 *   drops the master lock and calls Callbacks->AcquireForLazyWrite, which can
 *   block.  While it is parked there the VACB is still marked dirty.  A
 *   concurrent CcFlushCache (not gated by SHARED_CACHE_MAP_IN_LAZYWRITE) finds
 *   the same VACB, flushes it and clears its Dirty bit.  When the lazy writer
 *   is released it calls CcRosFlushVacb, whose first action is the
 *   *unconditional* CcRosUnmarkDirtyVacb(Vacb, TRUE) -> ASSERT(Vacb->Dirty)
 *   fires because the VACB is no longer dirty.
 *
 * This driver makes that window deterministic: AcquireForLazyWrite, when armed,
 * signals a worker thread and blocks; the worker thread cleans the VACB via
 * CcFlushCache and then releases the lazy writer straight into the assertion.
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

typedef struct _TEST_FCB
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    FAST_MUTEX HeaderMutex;
    ULONG WriteLength;
} TEST_FCB, *PTEST_FCB;

static KMT_IRP_HANDLER TestIrpHandler;
static FAST_IO_DISPATCH TestFastIoDispatch;

/* Race orchestration state */
static KEVENT LazyParkedEvent;      /* set by AcquireForLazyWrite when parked  */
static KEVENT LazyReleaseEvent;     /* set by the flusher to release the writer */
static volatile LONG RaceArmed = 0; /* 1 = next lazy write should park          */

static
BOOLEAN
NTAPI
FastIoRead(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _Out_ PVOID Buffer,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    IoStatus->Status = STATUS_NOT_SUPPORTED;
    return FALSE;
}

static
BOOLEAN
NTAPI
FastIoWrite(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _Out_ PVOID Buffer,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    IoStatus->Status = STATUS_NOT_SUPPORTED;
    return FALSE;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(RegistryPath);

    *DeviceName = L"CcLazyWriteRace";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE |
             TESTENTRY_BUFFERED_IO_DEVICE |
             TESTENTRY_NO_READONLY_DEVICE;

    KeInitializeEvent(&LazyParkedEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&LazyReleaseEvent, NotificationEvent, FALSE);

    KmtRegisterIrpHandler(IRP_MJ_CLEANUP, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_CREATE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_READ, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_WRITE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_FLUSH_BUFFERS, NULL, TestIrpHandler);

    TestFastIoDispatch.FastIoRead = FastIoRead;
    TestFastIoDispatch.FastIoWrite = FastIoWrite;
    DriverObject->FastIoDispatch = &TestFastIoDispatch;

    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
}

/*
 * Gated lazy-write acquire.  Only the first lazy-write pass after arming parks
 * here; it disarms atomically so the flusher's own CcFlushCache write-back
 * (which does NOT come through this callback) and any later pass run normally.
 */
BOOLEAN
NTAPI
AcquireForLazyWrite(
    _In_ PVOID Context,
    _In_ BOOLEAN Wait)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Wait);

    if (InterlockedCompareExchange(&RaceArmed, 0, 1) == 1)
    {
        DPRINT("[CcLazyWriteRace] lazy writer parked in AcquireForLazyWrite\n");
        KeSetEvent(&LazyParkedEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(&LazyReleaseEvent, Executive, KernelMode, FALSE, NULL);
        DPRINT("[CcLazyWriteRace] lazy writer released; about to flush a now-clean VACB\n");
    }

    return TRUE;
}

VOID
NTAPI
ReleaseFromLazyWrite(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

BOOLEAN
NTAPI
AcquireForReadAhead(
    _In_ PVOID Context,
    _In_ BOOLEAN Wait)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Wait);
    return TRUE;
}

VOID
NTAPI
ReleaseFromReadAhead(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static CACHE_MANAGER_CALLBACKS Callbacks = {
    AcquireForLazyWrite,
    ReleaseFromLazyWrite,
    AcquireForReadAhead,
    ReleaseFromReadAhead,
};

/*
 * Worker thread: waits until the lazy writer is parked, cleans the dirty VACB
 * out from under it with CcFlushCache, then releases the lazy writer so it
 * unconditionally unmarks an already-clean VACB -> ASSERT(Vacb->Dirty).
 */
static
VOID
NTAPI
FlusherRoutine(
    _In_ PVOID Context)
{
    PTEST_FCB Fcb = Context;
    IO_STATUS_BLOCK IoStatus = { 0 };

    KeWaitForSingleObject(&LazyParkedEvent, Executive, KernelMode, FALSE, NULL);

    DPRINT("[CcLazyWriteRace] flusher: cleaning VACB via CcFlushCache\n");
    CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &IoStatus);
    DPRINT("[CcLazyWriteRace] flusher: CcFlushCache done, releasing lazy writer\n");

    KeSetEvent(&LazyReleaseEvent, IO_NO_INCREMENT, FALSE);

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
PVOID
MapAndLockUserBuffer(
    _In_ _Out_ PIRP Irp,
    _In_ ULONG BufferLength)
{
    PMDL Mdl;

    if (Irp->MdlAddress == NULL)
    {
        Mdl = IoAllocateMdl(Irp->UserBuffer, BufferLength, FALSE, FALSE, Irp);
        if (Mdl == NULL)
        {
            return NULL;
        }

        _SEH2_TRY
        {
            MmProbeAndLockPages(Mdl, Irp->RequestorMode, IoWriteAccess);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            IoFreeMdl(Mdl);
            Irp->MdlAddress = NULL;
            _SEH2_YIELD(return NULL);
        }
        _SEH2_END;
    }

    return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
}

static
NTSTATUS
TestIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    LARGE_INTEGER Zero = RTL_CONSTANT_LARGE_INTEGER(0LL);
    NTSTATUS Status;
    PTEST_FCB Fcb;
    CACHE_UNINITIALIZE_EVENT CacheUninitEvent;

    PAGED_CODE();

    DPRINT("IRP %x/%x\n", IoStack->MajorFunction, IoStack->MinorFunction);
    ASSERT(IoStack->MajorFunction == IRP_MJ_CLEANUP ||
           IoStack->MajorFunction == IRP_MJ_CREATE ||
           IoStack->MajorFunction == IRP_MJ_READ ||
           IoStack->MajorFunction == IRP_MJ_WRITE ||
           IoStack->MajorFunction == IRP_MJ_FLUSH_BUFFERS);

    Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;

    if (IoStack->MajorFunction == IRP_MJ_CREATE)
    {
        ok_irql(PASSIVE_LEVEL);

        Fcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Fcb), 'RwlC');
        RtlZeroMemory(Fcb, sizeof(*Fcb));
        ExInitializeFastMutex(&Fcb->HeaderMutex);
        FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->HeaderMutex);

        Fcb->Header.AllocationSize.QuadPart = PAGE_SIZE;
        Fcb->Header.FileSize.QuadPart = PAGE_SIZE;
        Fcb->Header.ValidDataLength.QuadPart = PAGE_SIZE;
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
        IoStack->FileObject->FsContext = Fcb;
        IoStack->FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;

        CcInitializeCacheMap(IoStack->FileObject,
                             (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                             FALSE, &Callbacks, NULL);

        Irp->IoStatus.Information = FILE_OPENED;
        Status = STATUS_SUCCESS;
    }
    else if (IoStack->MajorFunction == IRP_MJ_READ)
    {
        ULONG Length;
        PVOID Buffer;
        LARGE_INTEGER Offset;

        Offset = IoStack->Parameters.Read.ByteOffset;
        Length = IoStack->Parameters.Read.Length;

        ok(BooleanFlagOn(Irp->Flags, IRP_NOCACHE), "IRP not coming from Cc!\n");

        Buffer = MapAndLockUserBuffer(Irp, Length);
        ok(Buffer != NULL, "Null pointer!\n");
        if (Buffer != NULL)
            RtlFillMemory(Buffer, Length, 0xBA);

        Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = Length;
        IoStack->FileObject->CurrentByteOffset.QuadPart = Offset.QuadPart + Length;
    }
    else if (IoStack->MajorFunction == IRP_MJ_WRITE)
    {
        ULONG Length;
        PVOID Buffer;
        LARGE_INTEGER Offset;
        static const UNICODE_STRING RaceTestFileName = RTL_CONSTANT_STRING(L"\\RaceTestFile");

        Offset = IoStack->Parameters.Write.ByteOffset;
        Length = IoStack->Parameters.Write.Length;
        Fcb = IoStack->FileObject->FsContext;

        if (!FlagOn(Irp->Flags, IRP_NOCACHE))
        {
            /* Cached write: dirty the VACB, then arm the race and spin up the
             * flusher thread. */
            BOOLEAN Ret;
            HANDLE ThreadHandle;
            OBJECT_ATTRIBUTES ThreadAttributes;

            ok_irql(PASSIVE_LEVEL);
            ok(RtlCompareUnicodeString(&IoStack->FileObject->FileName, &RaceTestFileName, TRUE) == 0,
               "Unexpected cached write file\n");

            Buffer = Irp->AssociatedIrp.SystemBuffer;
            ok(Buffer != NULL, "Null pointer!\n");

            Fcb->WriteLength = Length;

            InitializeObjectAttributes(&ThreadAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
            Status = PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS,
                                          &ThreadAttributes, NULL, NULL,
                                          FlusherRoutine, Fcb);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
                ZwClose(ThreadHandle);

            _SEH2_TRY
            {
                Ret = CcCopyWrite(IoStack->FileObject, &Offset, Length, TRUE, Buffer);
                ok_bool_true(Ret, "CcCopyWrite");
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;

            /* The VACB is now dirty.  Arm the gate so the next lazy-write pass
             * parks in AcquireForLazyWrite and hands the window to the flusher. */
            InterlockedExchange(&RaceArmed, 1);

            Status = STATUS_SUCCESS;
        }
        else
        {
            /* Non-cached write-back (from CcFlushCache / the lazy writer):
             * just satisfy it. */
            PMDL Mdl;

            ok_irql(PASSIVE_LEVEL);

            Buffer = MapAndLockUserBuffer(Irp, Length);
            ok(Buffer != NULL, "Null pointer!\n");

            Mdl = Irp->MdlAddress;
            ok(Mdl != NULL, "Null pointer for MDL!\n");

            Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = Length;
        }
    }
    else if (IoStack->MajorFunction == IRP_MJ_FLUSH_BUFFERS)
    {
        IO_STATUS_BLOCK IoStatus;

        Fcb = IoStack->FileObject->FsContext;
        CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &IoStatus);

        Status = STATUS_SUCCESS;
    }
    else if (IoStack->MajorFunction == IRP_MJ_CLEANUP)
    {
        ok_irql(PASSIVE_LEVEL);
        KeInitializeEvent(&CacheUninitEvent.Event, NotificationEvent, FALSE);
        CcUninitializeCacheMap(IoStack->FileObject, &Zero, &CacheUninitEvent);
        KeWaitForSingleObject(&CacheUninitEvent.Event, Executive, KernelMode, FALSE, NULL);
        Fcb = IoStack->FileObject->FsContext;
        ExFreePoolWithTag(Fcb, 'RwlC');
        IoStack->FileObject->FsContext = NULL;
        Status = STATUS_SUCCESS;
    }

    if (Status == STATUS_PENDING)
    {
        IoMarkIrpPending(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        Status = STATUS_PENDING;
    }
    else
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return Status;
}
