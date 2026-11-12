/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Test driver: data modified in a pinned buffer after
 *                  CcSetDirtyPinnedData must reach the disk
 * PROGRAMMER:      ReactOS Developers
 *
 * File systems may mark a BCB dirty BEFORE they modify the pinned buffer
 * (fastfat's FatPrepareWriteVolumeFile pins the FAT "pre-dirty", then the
 * caller stores the FAT entries).  A write-behind that runs between the mark
 * and the modification must not consume the dirty state: Windows does not
 * write back a range while a BCB covering it is pinned, so the modification
 * is written once the BCB is unpinned.  ReactOS used to set the Mm dirty bit
 * at CcSetDirtyPinnedData time; a flush in the window wrote the old bytes,
 * cleaned the page, and the modification was never written (lost FAT links
 * -> truncated cluster chains after a crash).
 *
 * The test pins page 0, marks it dirty, lets a worker thread run CcFlushCache
 * (standing in for the lazy writer) while the BCB is still pinned and
 * unmodified, then modifies the buffer, unpins, flushes again and checks
 * what the last non-cached write put on "disk".
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define OLD_BYTE 0xBA
#define NEW_BYTE 0x5A

typedef struct _TEST_FCB
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    FAST_MUTEX HeaderMutex;
} TEST_FCB, *PTEST_FCB;

static KMT_IRP_HANDLER TestIrpHandler;
static FAST_IO_DISPATCH TestFastIoDispatch;

/* What the non-cached writes put on the backing store */
static UCHAR DiskByte = OLD_BYTE;
static volatile LONG DiskWrites = 0;

static KEVENT FlushDoneEvent;

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

    *DeviceName = L"CcPinnedDirty";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE |
             TESTENTRY_BUFFERED_IO_DEVICE |
             TESTENTRY_NO_READONLY_DEVICE;

    KmtRegisterIrpHandler(IRP_MJ_CLEANUP, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_CREATE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_READ, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_WRITE, NULL, TestIrpHandler);

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

static
BOOLEAN
NTAPI
AcquireForLazyWrite(
    _In_ PVOID Context,
    _In_ BOOLEAN Wait)
{
    return TRUE;
}

static
VOID
NTAPI
ReleaseFromLazyWrite(
    _In_ PVOID Context)
{
}

static
BOOLEAN
NTAPI
AcquireForReadAhead(
    _In_ PVOID Context,
    _In_ BOOLEAN Wait)
{
    return TRUE;
}

static
VOID
NTAPI
ReleaseFromReadAhead(
    _In_ PVOID Context)
{
}

static CACHE_MANAGER_CALLBACKS Callbacks = {
    AcquireForLazyWrite,
    ReleaseFromLazyWrite,
    AcquireForReadAhead,
    ReleaseFromReadAhead,
};

/* Stand-in for the lazy writer: write back whatever Cc considers dirty. */
static
VOID
NTAPI
FlusherRoutine(
    _In_ PVOID Context)
{
    PTEST_FCB Fcb = Context;
    IO_STATUS_BLOCK IoStatus;

    CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &IoStatus);
    KeSetEvent(&FlushDoneEvent, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
TestPinnedDirtyThenModify(
    _In_ PFILE_OBJECT FileObject)
{
    PTEST_FCB Fcb = FileObject->FsContext;
    LARGE_INTEGER Offset = RTL_CONSTANT_LARGE_INTEGER(0LL);
    LARGE_INTEGER Timeout;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES ThreadAttributes;
    HANDLE ThreadHandle;
    PVOID Bcb;
    PUCHAR Buffer;
    BOOLEAN Ret;
    BOOLEAN FlushedWhilePinned;
    NTSTATUS Status;

    Ret = FALSE;
    _SEH2_TRY
    {
        Ret = CcPinRead(FileObject, &Offset, PAGE_SIZE, PIN_WAIT, &Bcb, (PVOID *)&Buffer);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Ret = FALSE;
    }
    _SEH2_END;
    ok_bool_true(Ret, "CcPinRead");
    if (!Ret)
        return;
    ok_eq_hex(Buffer[0], OLD_BYTE);

    /* Mark the BCB dirty BEFORE modifying it, as fastfat does. */
    CcSetDirtyPinnedData(Bcb, NULL);

    /* A write-behind happens now, while the BCB is pinned but unmodified. */
    KeInitializeEvent(&FlushDoneEvent, NotificationEvent, FALSE);
    InitializeObjectAttributes(&ThreadAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS, &ThreadAttributes,
                                  NULL, NULL, FlusherRoutine, Fcb);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        CcUnpinData(Bcb);
        return;
    }
    ZwClose(ThreadHandle);

    /* Windows may hold the flush off until the BCB is unpinned: don't wait
     * for it forever. */
    Timeout.QuadPart = -2 * 1000 * 1000 * 10LL;
    FlushedWhilePinned = (KeWaitForSingleObject(&FlushDoneEvent, Executive, KernelMode,
                                                FALSE, &Timeout) == STATUS_SUCCESS);
    trace("flush while pinned %s, disk writes so far %ld\n",
          FlushedWhilePinned ? "completed" : "held off", DiskWrites);

    /* Now the caller stores its data and unpins. */
    RtlFillMemory(Buffer, PAGE_SIZE, NEW_BYTE);
    CcUnpinData(Bcb);

    if (!FlushedWhilePinned)
        KeWaitForSingleObject(&FlushDoneEvent, Executive, KernelMode, FALSE, NULL);

    /* Write back everything that is dirty now. */
    CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &IoStatus);
    ok_eq_hex(IoStatus.Status, STATUS_SUCCESS);

    /* The modification must have reached the disk. */
    ok(DiskWrites > 0, "No write-back at all\n");
    ok(DiskByte == NEW_BYTE,
       "Modification made after CcSetDirtyPinnedData was lost: disk has 0x%x, expected 0x%x\n",
       DiskByte, NEW_BYTE);
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

    Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;

    if (IoStack->MajorFunction == IRP_MJ_CREATE)
    {
        ok_irql(PASSIVE_LEVEL);

        Fcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Fcb), 'DpcC');
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
                             TRUE, &Callbacks, NULL);

        Irp->IoStatus.Information = FILE_OPENED;
        Status = STATUS_SUCCESS;
    }
    else if (IoStack->MajorFunction == IRP_MJ_READ)
    {
        ULONG Length;
        PVOID Buffer;

        Length = IoStack->Parameters.Read.Length;
        ok(BooleanFlagOn(Irp->Flags, IRP_NOCACHE), "IRP not coming from Cc!\n");

        Buffer = MapAndLockUserBuffer(Irp, Length);
        ok(Buffer != NULL, "Null pointer!\n");
        if (Buffer != NULL)
            RtlFillMemory(Buffer, Length, DiskByte);

        Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = Length;
    }
    else if (IoStack->MajorFunction == IRP_MJ_WRITE)
    {
        ULONG Length;
        PUCHAR Buffer;

        Length = IoStack->Parameters.Write.Length;

        if (!FlagOn(Irp->Flags, IRP_NOCACHE))
        {
            /* The user-mode part's write is the trigger for the test.  Pin
             * and unpin the way a file system does: inside a critical
             * region (BCBs are resources). */
            ok_irql(PASSIVE_LEVEL);
            FsRtlEnterFileSystem();
            TestPinnedDirtyThenModify(IoStack->FileObject);
            FsRtlExitFileSystem();
            Irp->IoStatus.Information = Length;
            Status = STATUS_SUCCESS;
        }
        else
        {
            /* Write-back from Cc/Mm: record what reaches the "disk". */
            Buffer = MapAndLockUserBuffer(Irp, Length);
            ok(Buffer != NULL, "Null pointer!\n");
            if (Buffer != NULL && IoStack->Parameters.Write.ByteOffset.QuadPart == 0)
            {
                DiskByte = Buffer[0];
                InterlockedIncrement(&DiskWrites);
            }
            Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = Length;
        }
    }
    else if (IoStack->MajorFunction == IRP_MJ_CLEANUP)
    {
        ok_irql(PASSIVE_LEVEL);
        KeInitializeEvent(&CacheUninitEvent.Event, NotificationEvent, FALSE);
        CcUninitializeCacheMap(IoStack->FileObject, &Zero, &CacheUninitEvent);
        KeWaitForSingleObject(&CacheUninitEvent.Event, Executive, KernelMode, FALSE, NULL);
        Fcb = IoStack->FileObject->FsContext;
        ExFreePoolWithTag(Fcb, 'DpcC');
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
