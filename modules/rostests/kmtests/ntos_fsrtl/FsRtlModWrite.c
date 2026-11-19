/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     FsRtl acquisition for the modified page writer and Cc flushes
 */

#include <kmt_test.h>

/*
 * FsRtlAcquireFileForModWriteEx, FsRtlReleaseFileForModWrite,
 * FsRtlAcquireFileForCcFlushEx and FsRtlReleaseFileForCcFlush are ntifs.h
 * exports (NT 5.1 and later). They are looked up by name, so that a kernel
 * that lacks one fails this test instead of refusing to load the driver.
 *
 * The file object here belongs to a device of this driver, which has no fast
 * I/O dispatch, so FsRtl falls back on its default logic for the FCB header.
 * The modified page writer never waits, and that default parallels FastFat's
 * requirements (see its write.c): a write past ValidDataLength gets the main
 * resource exclusive, because extending valid data needs it; any other write
 * gets the paging I/O resource shared, or the main resource shared when the
 * file system asked for that or has no paging I/O resource.
 */

typedef NTSTATUS (NTAPI *PFSRTL_ACQUIRE_FOR_MOD_WRITE)(PFILE_OBJECT, PLARGE_INTEGER, PERESOURCE *);
typedef VOID (NTAPI *PFSRTL_RELEASE_FOR_MOD_WRITE)(PFILE_OBJECT, PERESOURCE);
typedef NTSTATUS (NTAPI *PFSRTL_ACQUIRE_FOR_CC_FLUSH)(PFILE_OBJECT);
typedef VOID (NTAPI *PFSRTL_RELEASE_FOR_CC_FLUSH)(PFILE_OBJECT);

static PFSRTL_ACQUIRE_FOR_MOD_WRITE pFsRtlAcquireFileForModWriteEx;
static PFSRTL_RELEASE_FOR_MOD_WRITE pFsRtlReleaseFileForModWrite;
static PFSRTL_ACQUIRE_FOR_CC_FLUSH pFsRtlAcquireFileForCcFlushEx;
static PFSRTL_RELEASE_FOR_CC_FLUSH pFsRtlReleaseFileForCcFlush;

#define VALID_DATA_LENGTH   (8 * PAGE_SIZE)
#define FILE_SIZE           (16 * PAGE_SIZE)
#define BELOW_VDL           (4 * PAGE_SIZE)
#define PAST_VDL            (12 * PAGE_SIZE)

typedef struct _TEST_FCB
{
    FSRTL_COMMON_FCB_HEADER Header;
    ERESOURCE MainResource;
    ERESOURCE PagingIoResource;
} TEST_FCB, *PTEST_FCB;

typedef struct _HOLDER
{
    PERESOURCE Resource;
    BOOLEAN Exclusive;
    KEVENT Held;
    KEVENT Release;
} HOLDER, *PHOLDER;

static
PVOID
GetRoutine(PCWSTR Name)
{
    UNICODE_STRING String;

    RtlInitUnicodeString(&String, Name);
    return MmGetSystemRoutineAddress(&String);
}

static
VOID
NTAPI
HoldResource(PVOID Context)
{
    PHOLDER Holder = Context;

    KeEnterCriticalRegion();
    if (Holder->Exclusive)
        ExAcquireResourceExclusiveLite(Holder->Resource, TRUE);
    else
        ExAcquireResourceSharedLite(Holder->Resource, TRUE);
    KeSetEvent(&Holder->Held, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(&Holder->Release, Executive, KernelMode, FALSE, NULL);
    ExReleaseResourceLite(Holder->Resource);
    KeLeaveCriticalRegion();
}

static
PKTHREAD
StartHolder(PHOLDER Holder, PERESOURCE Resource, BOOLEAN Exclusive)
{
    PKTHREAD Thread;
    NTSTATUS Status;
    LARGE_INTEGER Timeout;

    Holder->Resource = Resource;
    Holder->Exclusive = Exclusive;
    KeInitializeEvent(&Holder->Held, NotificationEvent, FALSE);
    KeInitializeEvent(&Holder->Release, NotificationEvent, FALSE);
    Thread = KmtStartThread(HoldResource, Holder);
    if (Thread == NULL)
        return NULL;
    Timeout.QuadPart = -10 * 1000 * 1000 * 10LL;
    Status = KeWaitForSingleObject(&Holder->Held, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
    {
        KmtFinishThread(Thread, &Holder->Release);
        return NULL;
    }
    return Thread;
}

/* Do what the modified page writer does for a page ending at EndingOffset:
 * acquire without waiting at APC_LEVEL, check what was acquired and how, then
 * release it. */
static
VOID
CheckModWrite(PCSTR What,
              PFILE_OBJECT FileObject,
              LONGLONG EndingOffset,
              NTSTATUS ExpectedStatus,
              PERESOURCE ExpectedResource,
              BOOLEAN ExpectedExclusive)
{
    LARGE_INTEGER Ending;
    PERESOURCE Resource = (PERESOURCE)(ULONG_PTR)0x55555555;
    NTSTATUS Status;
    KIRQL OldIrql;

    Ending.QuadPart = EndingOffset;
    KeRaiseIrql(APC_LEVEL, &OldIrql);
    Status = pFsRtlAcquireFileForModWriteEx(FileObject, &Ending, &Resource);
    ok(Status == ExpectedStatus, "%s: status 0x%08lx, expected 0x%08lx\n",
       What, Status, ExpectedStatus);
    if (Status == STATUS_SUCCESS)
    {
        ok(Resource == ExpectedResource, "%s: acquired %p, expected %p\n",
           What, Resource, ExpectedResource);
        if (Resource != NULL && Resource == ExpectedResource)
        {
            ok(ExIsResourceAcquiredSharedLite(Resource) != 0,
               "%s: the resource is not held\n", What);
            ok(ExIsResourceAcquiredExclusiveLite(Resource) == ExpectedExclusive,
               "%s: held %s, expected %s\n", What,
               ExIsResourceAcquiredExclusiveLite(Resource) ? "exclusive" : "shared",
               ExpectedExclusive ? "exclusive" : "shared");
        }
        if (Resource != NULL)
        {
            pFsRtlReleaseFileForModWrite(FileObject, Resource);
            ok(ExIsResourceAcquiredSharedLite(Resource) == 0,
               "%s: still held after the release\n", What);
        }
    }
    KeLowerIrql(OldIrql);
}

static
VOID
TestModWrite(PFILE_OBJECT FileObject, PTEST_FCB Fcb)
{
    PERESOURCE Main = &Fcb->MainResource;
    PERESOURCE Paging = &Fcb->PagingIoResource;
    HOLDER Holder;
    PKTHREAD Thread;

    /* Both resources, no flags */
    Fcb->Header.Flags = 0;
    CheckModWrite("below VDL", FileObject, BELOW_VDL, STATUS_SUCCESS, Paging, FALSE);
    CheckModWrite("up to VDL", FileObject, VALID_DATA_LENGTH, STATUS_SUCCESS, Paging, FALSE);
    CheckModWrite("past VDL", FileObject, PAST_VDL, STATUS_SUCCESS, Main, TRUE);

    /* The file system wants its main resource shared */
    Fcb->Header.Flags = FSRTL_FLAG_ACQUIRE_MAIN_RSRC_SH;
    CheckModWrite("main SH, below VDL", FileObject, BELOW_VDL, STATUS_SUCCESS, Main, FALSE);
    CheckModWrite("main SH, past VDL", FileObject, PAST_VDL, STATUS_SUCCESS, Main, TRUE);

    /* The file system wants its main resource exclusive */
    Fcb->Header.Flags = FSRTL_FLAG_ACQUIRE_MAIN_RSRC_EX;
    CheckModWrite("main EX, below VDL", FileObject, BELOW_VDL, STATUS_SUCCESS, Main, TRUE);
    CheckModWrite("main EX, past VDL", FileObject, PAST_VDL, STATUS_SUCCESS, Main, TRUE);
    Fcb->Header.Flags = 0;

    /* No paging I/O resource: the main resource stands in for it */
    Fcb->Header.PagingIoResource = NULL;
    CheckModWrite("no paging, below VDL", FileObject, BELOW_VDL, STATUS_SUCCESS, Main, FALSE);
    CheckModWrite("no paging, past VDL", FileObject, PAST_VDL, STATUS_SUCCESS, Main, TRUE);
    Fcb->Header.PagingIoResource = Paging;

    /* The modified page writer never waits. Another thread owns the main
     * resource exclusively, as a cached writer does: pages below VDL can still
     * go out under the paging I/O resource, pages past it cannot. */
    Thread = StartHolder(&Holder, Main, TRUE);
    if (Thread != NULL)
    {
        CheckModWrite("main owned, below VDL", FileObject, BELOW_VDL, STATUS_SUCCESS, Paging, FALSE);
        CheckModWrite("main owned, past VDL", FileObject, PAST_VDL, STATUS_CANT_WAIT, NULL, FALSE);
        KmtFinishThread(Thread, &Holder.Release);
    }

    /* Another thread owns the main resource shared */
    Thread = StartHolder(&Holder, Main, FALSE);
    if (Thread != NULL)
    {
        CheckModWrite("main shared, below VDL", FileObject, BELOW_VDL, STATUS_SUCCESS, Paging, FALSE);
        CheckModWrite("main shared, past VDL", FileObject, PAST_VDL, STATUS_CANT_WAIT, NULL, FALSE);
        KmtFinishThread(Thread, &Holder.Release);
    }

    /* Another thread owns the paging I/O resource exclusively, as a file
     * system changing the file size does: past VDL only main is needed */
    Thread = StartHolder(&Holder, Paging, TRUE);
    if (Thread != NULL)
    {
        CheckModWrite("paging owned, below VDL", FileObject, BELOW_VDL, STATUS_CANT_WAIT, NULL, FALSE);
        CheckModWrite("paging owned, past VDL", FileObject, PAST_VDL, STATUS_SUCCESS, Main, TRUE);
        KmtFinishThread(Thread, &Holder.Release);
    }

    ok(ExIsResourceAcquiredSharedLite(Main) == 0, "Main resource left held\n");
    ok(ExIsResourceAcquiredSharedLite(Paging) == 0, "Paging I/O resource left held\n");
}

static
VOID
TestCcFlush(PFILE_OBJECT FileObject, PTEST_FCB Fcb)
{
    NTSTATUS Status;

    /* Without a callback both resources are taken, main first */
    KeEnterCriticalRegion();
    Status = pFsRtlAcquireFileForCcFlushEx(FileObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(ExIsResourceAcquiredSharedLite(&Fcb->MainResource) != 0, "Main resource not held\n");
        ok(ExIsResourceAcquiredSharedLite(&Fcb->PagingIoResource) != 0, "Paging I/O resource not held\n");
        pFsRtlReleaseFileForCcFlush(FileObject);
    }
    ok(ExIsResourceAcquiredSharedLite(&Fcb->MainResource) == 0, "Main resource left held\n");
    ok(ExIsResourceAcquiredSharedLite(&Fcb->PagingIoResource) == 0, "Paging I/O resource left held\n");
    KeLeaveCriticalRegion();
}

START_TEST(FsRtlModWrite)
{
    PDEVICE_OBJECT DeviceObject = NULL;
    PFILE_OBJECT FileObject = NULL;
    PTEST_FCB Fcb = NULL;
    NTSTATUS Status;

    pFsRtlAcquireFileForModWriteEx = GetRoutine(L"FsRtlAcquireFileForModWriteEx");
    pFsRtlReleaseFileForModWrite = GetRoutine(L"FsRtlReleaseFileForModWrite");
    pFsRtlAcquireFileForCcFlushEx = GetRoutine(L"FsRtlAcquireFileForCcFlushEx");
    pFsRtlReleaseFileForCcFlush = GetRoutine(L"FsRtlReleaseFileForCcFlush");
    ok(pFsRtlAcquireFileForModWriteEx != NULL, "FsRtlAcquireFileForModWriteEx is not exported\n");
    ok(pFsRtlReleaseFileForModWrite != NULL, "FsRtlReleaseFileForModWrite is not exported\n");
    ok(pFsRtlAcquireFileForCcFlushEx != NULL, "FsRtlAcquireFileForCcFlushEx is not exported\n");
    ok(pFsRtlReleaseFileForCcFlush != NULL, "FsRtlReleaseFileForCcFlush is not exported\n");

    /* A device of ours, without a fast I/O dispatch */
    ok(KmtDriverObject->FastIoDispatch == NULL, "kmtest has a fast I/O dispatch\n");
    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Fcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Fcb), 'WMtK');
    FileObject = ExAllocatePoolWithTag(NonPagedPool, sizeof(*FileObject), 'WMtK');
    if (!Fcb || !FileObject)
    {
        skip(FALSE, "Out of memory\n");
        goto Cleanup;
    }

    RtlZeroMemory(Fcb, sizeof(*Fcb));
    ExInitializeResourceLite(&Fcb->MainResource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);
    Fcb->Header.NodeByteSize = sizeof(*Fcb);
    Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    Fcb->Header.Resource = &Fcb->MainResource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;
    Fcb->Header.AllocationSize.QuadPart = FILE_SIZE;
    Fcb->Header.FileSize.QuadPart = FILE_SIZE;
    Fcb->Header.ValidDataLength.QuadPart = VALID_DATA_LENGTH;

    RtlZeroMemory(FileObject, sizeof(*FileObject));
    FileObject->Type = IO_TYPE_FILE;
    FileObject->Size = sizeof(*FileObject);
    FileObject->DeviceObject = DeviceObject;
    FileObject->FsContext = Fcb;

    if (pFsRtlAcquireFileForModWriteEx && pFsRtlReleaseFileForModWrite)
        TestModWrite(FileObject, Fcb);
    else
        skip(FALSE, "No ModWrite acquisition routines\n");

    if (pFsRtlAcquireFileForCcFlushEx && pFsRtlReleaseFileForCcFlush)
        TestCcFlush(FileObject, Fcb);
    else
        skip(FALSE, "No CcFlush acquisition routines\n");

    ExDeleteResourceLite(&Fcb->PagingIoResource);
    ExDeleteResourceLite(&Fcb->MainResource);

Cleanup:
    if (FileObject)
        ExFreePoolWithTag(FileObject, 'WMtK');
    if (Fcb)
        ExFreePoolWithTag(Fcb, 'WMtK');
    IoDeleteDevice(DeviceObject);
}
